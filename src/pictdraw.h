/* A small PICT interpreter for the pages of the instructions (Apeiron Titles PICT 4000-4002): they are not bitmaps but QuickDraw drawing - a black page, lines of
 * text in Geneva with changes of colour, face and position, and a few small bitmaps (the power-up pictures). The interpreter understands exactly what those use:
 * PaintRect, RGBForeColor, TxFont / TxFace / TxSize, the four text operations, PackBitsRect / BitsRect, comments and the like. The text is drawn with the
 * port's own font (src/font.c) because the Mac's Geneva is not available. */
#ifndef APEIRON_PICTDRAW_H
#define APEIRON_PICTDRAW_H

#include <stddef.h>
#include <stdint.h>

/* Draws the picture into fb (w x h, 0xAARRGGBB), the picture's top left at the fb's top left. palette (256 RGB entries) or NULL: if given, every colour is
 * moved to the nearest entry, as the Mac did when it drew into a window with that colour table. Returns 0 on success; unknown operations stop the drawing (returns 1). */
int pict_draw(const uint8_t *data, size_t len, uint32_t *fb, int w, int h, const uint8_t *palette, char *msg, size_t msglen);

#endif
