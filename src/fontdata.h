/* Glyph tables rendered from open-licence fonts (see tools/gen_fonts.py): coverage bitmaps for ASCII 32..126 and the copyright sign (index 95), used for the title screen and the dialogs. */
#ifndef APEIRON_FONTDATA_H
#define APEIRON_FONTDATA_H

typedef struct { short advance, w, h, x, y; int off; } FGlyph;      /* x, y: offset of the bitmap's top left from the pen on the baseline */
typedef struct { int ascent; const FGlyph *gl; const unsigned char *px; } FFont;

extern const FFont font_serif18, font_sans9, font_sans9b;

#endif
