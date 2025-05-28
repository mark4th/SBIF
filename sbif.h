// sbif.h    The somewhat better image format header
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------

#define MARK8  (0xfc)
#define MARK16 (0xfd)

// -----------------------------------------------------------------------

typedef enum
{
    HORIZONTAL      = 0,
    VERTICAL        = 1,
    HORIZONTAL_DIFF = 2,
    VERTICAL_DIFF   = 3,
    OFFSET_DIFF     = 4
} tag_t;

// -----------------------------------------------------------------------

typedef struct
{
    uint32_t magic;
    uint16_t width;
    uint16_t height;
} sbif_header_t;

// -----------------------------------------------------------------------
// because c is fkkn annoying

#pragma GCC diagnostic ignored "-Wmultichar"

// =======================================================================
