/* Colour icon ('cicn') decoder: a Mac colour icon is a pixmap of 1, 2, 4 or 8 bits with its own colour table, a one-bit mask and a black-and-white bitmap.
 * The result is a 32-bit ARGB image; mask bits that are clear give alpha 0 (the icon is drawn with PlotCIcon). */
#ifndef APEIRON_CICN_H
#define APEIRON_CICN_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int width, height;
    uint32_t *argb;        /* width * height pixels, alpha 0 or 255 */
} CicnImage;

/* Returns 0 on success. */
int cicn_decode(const uint8_t *data, size_t len, CicnImage *img, char *msg, size_t msglen);
void cicn_free(CicnImage *img);

/* Colour cursor ('crsr'): a 16 x 16 colour picture with a one-bit mask; also gives the hot spot. Returns 0 on success. */
int crsr_decode(const uint8_t *data, size_t len, CicnImage *img, int *hot_x, int *hot_y, char *msg, size_t msglen);

/* Application icon: the 32 x 32 8-bit icon ('icl8', Mac standard colour table) with the mask from the 'ICN#' resource of the same id. Returns 0 on success. */
int icon_decode(const uint8_t *icl8, size_t icl8_len, const uint8_t *icn, size_t icn_len, CicnImage *img);

#endif
