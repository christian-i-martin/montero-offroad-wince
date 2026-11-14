/* PNG decoding for the map tiles, through stb_image (third_party/stb_image.h, public domain or
 * MIT, see THIRD_PARTY.md). Only the PNG decoder is compiled in: no file access, no threads, no
 * processor-specific code. stb_image is not hardened against hostile files; the tiles come from
 * our own map builder (tools/map/), and map.c limits their size. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_SIMD
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 2048
#define STBI_ASSERT(x) ((void)0)

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#endif
#include "../third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "map.h"

unsigned char *map_png_decode(const unsigned char *data, int length, int *w, int *h)
{
    int channels = 0;
    return stbi_load_from_memory(data, length, w, h, &channels, 3);
}

void map_png_free(unsigned char *pixels)
{
    stbi_image_free(pixels);
}
