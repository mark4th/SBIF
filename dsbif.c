// dsbif.c     The SOMETIMES better image format decompression routines
// -----------------------------------------------------------------------

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <zstd.h>

#include "sbif.h"

// -----------------------------------------------------------------------

uint16_t width;
uint16_t height;

uint16_t run;
uint8_t rle;

FILE *out_fp;

uint8_t *in_buff;
uint8_t *out_buff;

uint8_t *in_p;
uint8_t *out_p;

uint8_t bits;
uint8_t num_bits;

uint8_t *z_buff;
uint32_t z_size;

// -----------------------------------------------------------------------

static void reset(void)
{
    num_bits = 0;
    run      = 0;
}

// -----------------------------------------------------------------------

void get_run(void)
{
    rle = *in_p++;
    run = 1;

    switch (rle)
    {
        case MARK8:
          run = (*in_p++);
          rle = (*in_p++);
          break;

        // this will probably be somewhat kinda rare ish

        case MARK16:
          run  = (*in_p++) << 8;
          run += (*in_p++);
          rle  = (*in_p++);
          break;
    }
}

// -----------------------------------------------------------------------

static uint8_t read_bit(void)
{
    uint8_t c;

    if (num_bits == 0)
    {
        if (run == 0)
        {
            get_run();
        }

        bits     = rle;
        num_bits = 8;
        run--;
    }

    c = (bits & 0x80) ? 1 : 0;
    bits <<= 1;
    num_bits--;

    return c;
}

// -----------------------------------------------------------------------

static uint8_t read_bits(uint8_t n)
{
    uint8_t c = 0;

    while (n)
    {
        c <<= 1;
        c |= read_bit();
        n--;
    }

    return c;
}

// -----------------------------------------------------------------------

void flush_bits(void)
{
    while (num_bits != 0)
    {
        read_bit();
    }
}

// -----------------------------------------------------------------------
// horizontal decompression

static void horizontal(void)
{
    uint16_t i;
    uint8_t bit;
    uint8_t c;

    i = width;

    c = read_bits(8);       // read fist pixel of scan line
    *out_p++ = c;

    while (--i)             // must be pre-decrement
    {
        bit = read_bit();

        if (bit != 0)
        {
            c = read_bits(8);
        }

        *out_p++ = c;
    }
}

// -----------------------------------------------------------------------
// vertical decompression

static void vertical(void)
{
    uint16_t i;
    uint8_t bit;
    uint8_t *q;

    i = width;
    q = (out_p - width);

    while (i--)             // must be post decrement
    {
        bit = read_bit();

        *out_p = (bit == 0)
            ? *q
            : read_bits(8);

        out_p++;
        q++;
    }
}

// -----------------------------------------------------------------------
// horizontal differential decompression

static void horizontal_diff(void)
{
    uint16_t i;
    uint8_t c;
    uint8_t d;
    uint8_t bit;

    i = width;

    c = read_bits(8);
    *out_p++ = c;

    while (--i)             // must be pre-decrement
    {
        bit = read_bit();

        if (bit != 0)
        {
            d = read_bits(8);
        }

        c += d;
        *out_p++ = c;
    }
}

// -----------------------------------------------------------------------

static void v(uint8_t *q)
{
    uint16_t i;
    uint8_t d;
    uint8_t bit;

    i = width;

    d      = read_bits(8);
    *out_p = (*q + d);

    out_p++;
    q++;

    while (--i)
    {
        bit = read_bit();

        if (bit != 0)
        {
            d = read_bits(8);
        }

        *out_p = (*q + d);

        out_p++;
        q++;
    }
}

// -----------------------------------------------------------------------
// vertical differential decompression

static void vertical_diff(void)
{
    v(out_p - width);
}

// -----------------------------------------------------------------------
// offset vertical differential decompression

static void offset_diff(void)
{
    v(out_p - (2 * width));
}

// -----------------------------------------------------------------------
// decompress all scan lines of image

static void sb_decompress(void)
{
    tag_t tag;
    uint16_t i;

    out_p = out_buff;
    reset();

    i = height;

    while (i--)
    {
        tag = read_bits(3);

        // decompress based on method specified in tag
        // and graph decompression visually

        // if the compression graph is different from what we produce here
        // then either compression or decompression is broken.   this was
        // actually a very helpful tool during development.  it also looks
        // cool and scientifical !

        switch (tag)
        {
            case HORIZONTAL:       printf("▬");  horizontal();       break;
            case VERTICAL:         printf("▮");  vertical();         break;
            case HORIZONTAL_DIFF:  printf("▭");  horizontal_diff();  break;
            case VERTICAL_DIFF:    printf("▯");  vertical_diff();    break;
            case OFFSET_DIFF:      printf("◈");  offset_diff();      break;
        }

        flush_bits();
    }
}

// -----------------------------------------------------------------------

static uint32_t check_header(void)
{
    int n;

    sbif_header_t *header = (sbif_header_t *)in_buff;

    if (header->magic != (uint32_t)'FIBS')
    {
        printf("Bad Magic\n");
        exit(0);
    }

    width  = header->width;
    height = header->height;

    return(width * height);
}

// -----------------------------------------------------------------------

static void zstd_decompress(void)
{
    unsigned long long const rSize = ZSTD_getFrameContentSize(in_p, z_size);

    void* const rBuff = calloc(rSize, 1);

    size_t const dSize = ZSTD_decompress(z_buff, rSize, in_p, z_size);
}

// -----------------------------------------------------------------------

void main(int argc, char **argv)
{
    int n;
    int size;
    struct stat st;
    clock_t start;              // time at start and end of compression
    clock_t finish;

    FILE *fp;

    fp = fopen(argv[1], "rb");
    fstat(fileno(fp), &st);

    in_buff  = calloc(st.st_size, 1);

    n = fread(in_buff, 1, st.st_size, fp);
    fclose(fp);

    size = check_header();

    out_buff = calloc(size * 4, 1);
    z_buff   = calloc(size * 4, 1);

    in_p   = in_buff    + sizeof(sbif_header_t);
    z_size = st.st_size - sizeof(sbif_header_t);

    zstd_decompress();

    out_fp = fopen(argv[2], "wb");

    start = clock();

    in_p  = z_buff;
    out_p = out_buff;

    for (n = 0; n != 4; n++)
    {
        sb_decompress();
        printf("\n\n");
        fwrite(out_buff, size, 1, out_fp);
    }

    finish = clock();
    printf("%dms\n", (int)(finish - start) / 1000);
}

// =======================================================================
