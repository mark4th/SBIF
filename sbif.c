// sbif.c    - The SOMETIMES better image format compression routines
// -----------------------------------------------------------------------

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <zstd.h>

#include "sbif.h"
#include "lodepng.h"        // makes the build 487658265295 times slower

// -----------------------------------------------------------------------
// global variables because im lazy and ... why not?

// the RGBA data is separated out into their respective buffers

uint8_t *r_buff;            // all red   pixels of image
uint8_t *g_buff;            // all green pixels of image
uint8_t *b_buff;            // all blue  pixels of image
uint8_t *a_buff;            // all alpha bytes  of image

// each method is tried one at a time and the method producing the
// smallest results is chosen.

uint8_t *h_comp_buff;       // compression try buffers for each method.
uint8_t *v_comp_buff;
uint8_t *h_diff_buff;
uint8_t *v_diff_buff;
uint8_t *z_diff_buff;       // offset vertical compression buffer

uint32_t h_comp_len;        // cursors for each of the above
uint32_t v_comp_len;
uint32_t h_diff_len;
uint32_t v_diff_len;
uint32_t z_diff_len;

uint32_t out_len;           // length of current try
uint32_t best;              // best length of all tries

// as data is compressed into individual bits those bits are staged
// here until a complete byte is compiled.  this byte is then
// compared with the current run length byte and if they are the
// same the run length is incremented.  If they are different the
// old run is written out to the output buffer and a new run is
// started with length 1

uint8_t bit_cache;          // bit data output staging area
uint8_t num_bits;           // how many bits are in the cache so far

uint16_t run;               // current output data run length
uint8_t rle;                // run data

char *infile;               // input file name
char *outfile;              // output file name

int width;                  // dimensions of image being compressed
int height;
int size;                   // with * height

uint8_t *in_p;              // current position within input buffer
uint8_t *out_p;             // current position within final output buffer

FILE *out_fp;               // end result written out to this file

uint8_t *png_image;         // decoding PNG should be simpler

clock_t start;              // time at start and end of compression
clock_t finish;

// zstd encoding of the data my algorithms produce was always intended
// but it was only added when I had proved my algorithms were working
// zstd encoding is considered stage 3 of the process

uint8_t *s3_buff;           // stage 3 zstd compression input buffer
uint32_t s3_size;           // how much data we have stuffed in there

// -----------------------------------------------------------------------
// reset encoding engine for new data

static void reset(void)
{
    out_len   = 0;
    bit_cache = 0;
    num_bits  = 0;
    run       = 0;
}

// -----------------------------------------------------------------------
// write rle run to output buffer with an 8 or 16 bit run length

void write_run(void)
{
    // run length can be anywhere from 0x0001 to 0xffff

    if (run < 3)
    {
        // if the data is not one of the run markers then just write run
        // instances of rle to the output buffer.  otherwise fall through
        // to write a run with an 8 bit count of 1 or 2 (i.e. data that is
        // the same as the RLE markers need to be escaped)

        if ((rle != MARK8) && (rle != MARK16))
        {
            while (run)
            {
                *out_p++ = rle;
                out_len++;
                run--;
            }
            return;         // run is zero here
        }
    }

    // is the run length within 8 bits?

    if (run < 0x100)
    {
        *out_p++ = MARK8;
        *out_p++ = (uint8_t)(run & 0xff);
        *out_p++ = rle;

        out_len += 3;
    }
    else
    {
        *out_p++ = MARK16;
        *out_p++ = (uint8_t)(run >> 8);
        *out_p++ = (uint8_t)(run & 0xff);
        *out_p++ = rle;

        out_len += 4;
    }

    run = 0;                // run is zero here too!
}

// -----------------------------------------------------------------------
// write a single bit out to the bit cache

void write_bit(uint8_t bit)
{
    bit_cache <<= 1;
    bit_cache |= (bit) ? 1 : 0;
    num_bits++;

    // if the cache is full...

    if (num_bits == 8)
    {
        // if the cached bits are not the same as the current run
        // write that run out and start a new one

        if (bit_cache != rle)
        {
            // do we have a previous run that we need to write out?

            if (run != 0)
            {
                write_run();
            }

            // run will be zero here, set new rle value and set the
            // new run length to one

            rle = bit_cache;
        }

        // either increments the current run length or sets it to 1

        run++;
        num_bits = 0;
    }
}

// -----------------------------------------------------------------------
// write the lower n bits of data c out

void write_bits(uint8_t c, uint8_t n)
{
    uint8_t mask;

    mask = (1 << (n - 1));

    while (mask != 0)
    {
        write_bit(c & mask);
        mask >>= 1;
    }
}

// -----------------------------------------------------------------------
// write bits out zero till the bit cache is flushed then write the run

void flush_bits(void)
{
    while (num_bits != 0)
    {
        write_bit(0);
    }
    write_run();
}

// -----------------------------------------------------------------------
// each compressed scan line has a three byte compression method tag
// so we know how to decompress it!

static void write_tag(tag_t tag)
{
    write_bits(tag, 3);
}

// -----------------------------------------------------------------------
// somewhat cheezy because it assumes pixel channel data is always 8 bits
// not sure if this is a safe assumption but this is not production code

static void new_byte(uint8_t c)
{
    write_bit(1);
    write_bits(c, 8);
}

// -----------------------------------------------------------------------
// horizontal compression

// After writing out the TAG bits, the first pixel of the scan line is
// written out as is.  Each pixel after the first is then compared with
// the previous one on the scan line.  If the current pixel is the same
// color as the one to its left then we output a single ZERO bit.  If
// it is a different color we output a ONE bit followed by the bits of
// the new pixel color.

void horizontal(uint8_t *p)
{
    uint16_t i;
    uint8_t c;              // previous byte
    uint8_t d;              // current byte

    i       = width;        // loopy thing
    in_p    = p;            // pointer to data to be compressed
    out_p   = h_comp_buff;  // where to stage the results of this try
    out_len = 0;

    write_tag(HORIZONTAL);

    c = *in_p++;            // write first pixel of scan line as is
    write_bits(c, 8);

    // tribal knowledge incoming...

    while (--i)             // this MUST be pre-decrement
    {
        d = *in_p++;        // get second++ pixel

        (c == d)            // same as previous?
            ? write_bit(0)
            : new_byte(d);
        c = d;              // new pixel is now the previous pixel
    }

    flush_bits();
    h_comp_len = out_len;
}

// -----------------------------------------------------------------------
// vertical compression

// After writing out the TAG bits, each pixel is compared to the one
// directly above it.  If they are the same color a single ZERO bit is
// written out.  Otherwise we write out a ONE bit followed by the bits of
// the new pixel color.

void vertical(uint8_t *p)
{
    uint16_t i;
    uint8_t *q;

    i       = width;        // loopy thing
    in_p    = p;            // point to input data current pixel
    q       = (p - width);  // point q at pixel above current one
    out_p   = v_comp_buff;  // where to stage the results of this try
    out_len = 0;

    write_tag(VERTICAL);

    while (i--)             // this MUST be post decrement
    {
        (*in_p == *q)
            ? write_bit(0)
            : new_byte(*in_p);
        in_p++;
        q++;
    }

    flush_bits();
    v_comp_len = out_len;
}

// -----------------------------------------------------------------------
// horizontal differential compression

// After writing out the TAG bits, the first pixel of the scan line is
// written out as is.  The next pixel is written out as a single ONE bit
// followed by the difference between it and the first pixel.  For each
// pixel after the second the delta between it and the one to it's left is
// computed and if this delta is the same as the previous pixel's delta
// then a single ZERO bit is written out.  Otherwise a ONE bit is written
// followed by the new delta.

void horizontal_diff(uint8_t *p)
{
    uint16_t i;
    uint8_t c1;
    uint8_t c2;

    uint16_t d1;
    uint16_t d2;

    i       = width;        // loopy thing
    in_p    = p;            // data to be compressed
    out_p   = h_diff_buff;  // where to stage this try
    out_len = 0;

    write_tag(HORIZONTAL_DIFF);

    c1 = *in_p++;           // first byte of scan line is first pixel
    write_bits(c1, 8);      // as is
    d1 = -1;                // there is no "previous" difference yet

    while (--i)             // MUST be pre-decrement
    {
        c2 = *in_p++;       // get second++ pixel

        // calculate the difference between the current pixel c2 and
        // the previous pixel c1

        d2 = (uint8_t)(c2 - c1);

        (d2 == d1)
            ? write_bit(0)
            : new_byte(d2);

        d1 = d2;
        c1 = c2;
    }

    flush_bits();
    h_diff_len = out_len;
}

// -----------------------------------------------------------------------
// vertical differential (see below)

// there is no need to give any function an overly verbose name.  using
// entire sentences for function names just makes the code an unreadable
// mess.   There is also absolutely no such thing as self documenting code
// and people claiming there is are simply use this as a rationalization
// for not properly commenting their code.  Un-commented code is BAD,
// especially when you are the poor shmuck that got volunteered to take
// over maintaining it.

static void v(uint8_t *p, uint8_t *q)
{
    uint16_t i;
    uint8_t d1, d2;

    in_p    = p;            // data to be compressed
    i       = width;        // loopy thing
    out_len = 0;

    // calculate the delta between initial two vertically adjacent pixels

    d2 = (uint8_t)(*in_p - *q);

    write_bits(d2, 8);      // write the delta

    while (--i)
    {
        d1 = d2;
        in_p++;
        q++;

        // calculate the delta between the next two vertically
        // adjacent pixels

        d2 = (uint8_t)(*in_p - *q);

        // is the new delta the same as the previous difference...

        (d2 == d1)
            ? write_bit(0)
            : new_byte(d2);
    }

    flush_bits();
}

// -----------------------------------------------------------------------
// vertical differential compression

// After writing out the TAG bits, the delta between the first pixel and
// the one directly above it is computed and is written out as is. For
// each subsequent pixel the delta between it and the one immediately
// above it is computed and if this delta is the same as the previous
// delta then a single ZERO bit is written out.  Otherwise a ONE bit is
// written followed by the new delta.

void vertical_diff(uint8_t *p)
{
    uint8_t *q;

    q       = (p - width);  // point q at pixel above current one
    out_p   = v_diff_buff;  // where to stage results of this try

    write_tag(VERTICAL_DIFF);

    v(p, q);

    v_diff_len = out_len;
}

// -----------------------------------------------------------------------
// Offset vertical differential compression

// This method is identical to Vertical Differential Compression except
// the deltas are computed between the current pixel and the one two scan
// lines above it.

void offset_diff(uint8_t *p)
{
    uint8_t *q;

    // point q at pixel two scan lines above the current one

    q       = (p - (2 * width));
    out_p   = z_diff_buff;  // where to stage results of this try

    write_tag(OFFSET_DIFF);

    v(p, q);

    z_diff_len = out_len;
}

// -----------------------------------------------------------------------
// write the SBIF file format header out to the disk file

static void write_header(void)
{
    sbif_header_t header;

    header.magic = (uint32_t)'FIBS';
    header.width = width;
    header.height = height;

    fwrite(&header, 1, sizeof(header), out_fp);
}

// -----------------------------------------------------------------------
// factored out because it is just a bunch of if / and / but loops

static tag_t get_best(uint16_t i)
{
    tag_t tag;

    if (h_comp_len < best)
    {
        best = h_comp_len;
        tag = HORIZONTAL;
    }

    if (v_comp_len < best)
    {
        best = v_comp_len;
        tag = VERTICAL;
    }

    if (h_diff_len <= best)
    {
        best = h_diff_len;
        tag = HORIZONTAL_DIFF;
    }

    if (v_diff_len < best)
    {
        best = v_diff_len;
        tag = VERTICAL_DIFF;
    }

    if (i < height - 1)
    {
        if (z_diff_len < best)
        {
            best = z_diff_len;
            tag = OFFSET_DIFF;
        }
    }

    return tag;
}

// -----------------------------------------------------------------------
// staging area for sbif compressed scan line data which will be passed to
// the zstd compression routines as the source buffer once all scan lines
// of the image have been sbif compressed

// zstd compression is stage 3

static void s3_write(uint8_t *p, uint16_t n)
{
    memcpy(&s3_buff[s3_size], p, n);
    s3_size += n;
}

// -----------------------------------------------------------------------
// compress the entire image using stages one and two

void sb_compress(uint8_t *p)
{
    uint16_t i;
    uint8_t *q;

    tag_t tag;

    i = height;

    reset();

    horizontal(p);          // first scan always compressed horizontally
    printf("▬");            // graph each scan lines compression method

    // write the compressed data of the first scan line out to zstd
    // staging buffer

    s3_write(h_comp_buff, h_comp_len);

    while (--i)
    {
        p += width;         // point p at next scan line

        best = -1;

        horizontal(p);      // try each method
        vertical(p);
        horizontal_diff(p);
        vertical_diff(p);

        if (i < height - 1)
        {
            offset_diff(p);
        }

        tag = get_best(i);

        // point q to the try buffer with the best results and
        // visually graph the selected decompression method

        switch (tag)
        {
            case HORIZONTAL:      q = h_comp_buff;   printf("▬");  break;
            case VERTICAL:        q = v_comp_buff;   printf("▮");  break;
            case HORIZONTAL_DIFF: q = h_diff_buff;   printf("▭");  break;
            case VERTICAL_DIFF:   q = v_diff_buff;   printf("▯");  break;
            case OFFSET_DIFF:     q = z_diff_buff;   printf("◈");  break;
        }

        s3_write(q, best);
    }

    printf("\n\n");
}

// -----------------------------------------------------------------------

static void load_png(void)
{
    unsigned error;

    error = lodepng_decode32_file(&png_image, &width, &height, infile);

    if(error)
    {
        printf("error %u: %s\n", error, lodepng_error_text(error));
        exit(0);
    }

    size   = width * height;
}

// -----------------------------------------------------------------------

static void zstd_compress(void)
{
    void *z_out_buff;

    size_t z_size = ZSTD_compressBound(s3_size);

    z_out_buff = calloc(z_size, 1);

    z_size = ZSTD_compress(z_out_buff, z_size, s3_buff, s3_size, 9);

    write_header();         // write SBIF image header to file

    fwrite(z_out_buff, 1, z_size, out_fp);
    fclose(out_fp);
}

// -----------------------------------------------------------------------

void main(int argc, char **argv)
{
    uint32_t i;
    FILE *raw_fp;

    infile  = argv[1];
    outfile = argv[2];

    load_png();

    out_fp = fopen(outfile, "wb");
    printf("%s %d %d\n\n", infile, width, height);

    // try buffers for each compression method

    h_comp_buff  = calloc(width * 4, 1);
    v_comp_buff  = calloc(width * 4, 1);
    h_diff_buff  = calloc(width * 4, 1);
    v_diff_buff  = calloc(width * 4, 1);
    z_diff_buff  = calloc(width * 4, 1);

    // each color channel gets loaded into its own buffer

    r_buff = calloc(size, 1);
    g_buff = calloc(size, 1);
    b_buff = calloc(size, 1);
    a_buff = calloc(size, 1);

    in_p = png_image;

    s3_buff = calloc(size * 6, 1);

    // having to do this part is annoying

    for (i = 0; i < size; i++)
    {
        r_buff[i] = *in_p++;
        g_buff[i] = *in_p++;
        b_buff[i] = *in_p++;
        a_buff[i] = *in_p++;
    }

    free(png_image);

    // save out the uncompressed data so we can verify our
    // decompression results

    raw_fp = fopen("image.raw", "wb");
    fwrite(r_buff, 1, size, raw_fp);
    fwrite(g_buff, 1, size, raw_fp);
    fwrite(b_buff, 1, size, raw_fp);
    fwrite(a_buff, 1, size, raw_fp);
    fclose(raw_fp);

    // compress each channel independently

    start = clock();

    sb_compress(r_buff);
    sb_compress(g_buff);
    sb_compress(b_buff);
    sb_compress(a_buff);

    zstd_compress();

    finish = clock();

    printf("%dms\n\n", (int)(finish - start) / 1000);

    // misra violation!  Guru Meditation, too lazy to free buffers
}

// =======================================================================

