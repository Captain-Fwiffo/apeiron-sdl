/* A small built-in font (5 x 7 pixels, ASCII 32..126; descenders use an eighth row) for the few places where the port has to write text itself
 * (the high-score name entry): the original used the Mac's dialog boxes. Glyph data: five columns, bit 0 is the top row. */
#ifndef APEIRON_FONT_H
#define APEIRON_FONT_H

#include <stdint.h>

/* Draws s into a 32-bit image of width img_w and height img_h with its top left at (x, y), each pixel of the font drawn as scale x scale. Returns the width drawn. */
int font_draw(uint32_t *img, int img_w, int img_h, int x, int y, int scale, uint32_t color, const char *s);
int font_width(int scale, const char *s);
/* the same, each letter spaced by its own width instead of a fixed cell */
int font_draw_p(uint32_t *img, int img_w, int img_h, int x, int y, int scale, uint32_t color, const char *s);
int font_width_p(int scale, const char *s);


#include "fontdata.h"
/* Text from the rendered glyph tables: the pen starts at (x, baseline), coverage is blended into the image; returns the advance. 0xA9 is the copyright sign. */
int ffont_draw(uint32_t *img, int img_w, int img_h, int x, int baseline, const FFont *f, uint32_t color, const char *s);
int ffont_width(const FFont *f, const char *s);

#endif
