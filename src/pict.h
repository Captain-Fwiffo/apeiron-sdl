/* PICT decoder for the pictures Apeiron and its plugin packs use.
 *
 * Decodes the first bitmap of a PICT into an 8-bit indexed image with its own palette (or, for the direct-colour
 * opcodes, an RGB image). It deliberately keeps palette indices, because the game depends on them.
 *
 * Supported opcodes: version 1 and 2 headers, clip, the pen/text/colour state opcodes (skipped), BitsRect (0x90),
 * BitsRgn (0x91), PackBitsRect (0x98), PackBitsRgn (0x99), DirectBitsRect (0x9A, 32 and 16 bit), DirectBitsRgn (0x9B),
 * comments, end of picture. Anything else makes the decoder stop and report it; a picture with no bitmap (only
 * vector drawing) is reported as such.
 */
#ifndef APEIRON_PICT_H
#define APEIRON_PICT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height;
    int indexed;               /* 1: pixels are palette indices; 0: pixels are RGB triples */
    int palette_size;          /* entries used, indexed only */
    uint8_t palette[256 * 3];  /* RGB, indexed only; entry i is the colour of index i */
    uint8_t *pixels;           /* width*height bytes (indexed) or width*height*3 bytes (RGB) */
    /* what was found, for reports */
    int version;               /* 1 or 2 */
    int bitmap_opcode;         /* 0x98, 0x99 ... or 0 if none */
    int depth;                 /* bits per pixel of the source bitmap */
} PictImage;

typedef enum { PICT_OK = 0, PICT_NO_BITMAP = 1, PICT_UNSUPPORTED = 2, PICT_BAD = 3 } PictStatus;

PictStatus pict_decode(const uint8_t *data, size_t len, PictImage *img, char *msg, size_t msglen);
void pict_free(PictImage *img);

#endif
