/* 'ppat' resources: Color QuickDraw pixel patterns (the level backdrops are fills with these). Decodes the pattern's pixel map to an indexed image with its
 * colour table. Old-style (black and white) patterns come out as a 8 x 8 two-colour image. */
#ifndef APEIRON_PPAT_H
#define APEIRON_PPAT_H

#include <stddef.h>
#include <stdint.h>

#include "pict.h"

/* Returns 0 on success; img->pixels must be released with pict_free. */
int ppat_decode(const uint8_t *data, size_t len, PictImage *img, char *msg, size_t msglen);

#endif
