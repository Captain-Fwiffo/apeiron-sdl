#include "ppat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u16(const uint8_t *p) { return (uint32_t)(p[0] << 8 | p[1]); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int ppat_decode(const uint8_t *d, size_t len, PictImage *img, char *msg, size_t msglen)
{
    memset(img, 0, sizeof *img);
    if (len < 28) { snprintf(msg, msglen, "ppat too short"); return -1; }
    uint32_t type = u16(d), map_off = u32(d + 2), data_off = u32(d + 6);
    if (type == 0) {                                   /* old style: 8 x 8 bits at pat1Data */
        img->width = img->height = 8; img->indexed = 1; img->palette_size = 2;
        img->palette[0] = img->palette[1] = img->palette[2] = 255;
        img->pixels = (uint8_t *)malloc(64);
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) img->pixels[y * 8 + x] = (d[20 + y] >> (7 - x)) & 1;
        return 0;
    }
    if (map_off + 50 > len) { snprintf(msg, msglen, "ppat pixel map outside the resource"); return -1; }
    const uint8_t *pm = d + map_off;
    uint32_t rb = u16(pm + 4) & 0x3FFF;
    int top = (int16_t)u16(pm + 6), left = (int16_t)u16(pm + 8), bottom = (int16_t)u16(pm + 10), right = (int16_t)u16(pm + 12);
    int depth = (int)u16(pm + 32);
    uint32_t ct_off = u32(pm + 42);
    int w = right - left, h = bottom - top;
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || depth < 1 || depth > 8) { snprintf(msg, msglen, "ppat bad size %dx%d depth %d", w, h, depth); return -1; }
    if (data_off + (size_t)rb * h > len) { snprintf(msg, msglen, "ppat pixel data outside the resource"); return -1; }
    if (ct_off + 8 > len) { snprintf(msg, msglen, "ppat colour table outside the resource"); return -1; }
    const uint8_t *ct = d + ct_off;
    uint32_t flags = u16(ct + 4);
    int n = (int)u16(ct + 6) + 1;
    if (n > 256 || ct_off + 8 + (size_t)n * 8 > len) { snprintf(msg, msglen, "ppat bad colour table"); return -1; }
    int npal = 0;
    for (int j = 0; j < n; j++) {
        uint32_t value = u16(ct + 8 + j * 8);
        int idx = (flags & 0x8000) ? j : (int)(value & 0xFF);
        if (idx > 255) idx = 255;
        img->palette[idx * 3] = ct[10 + j * 8]; img->palette[idx * 3 + 1] = ct[12 + j * 8]; img->palette[idx * 3 + 2] = ct[14 + j * 8];
        if (idx + 1 > npal) npal = idx + 1;
    }
    img->width = w; img->height = h; img->indexed = 1; img->palette_size = npal; img->depth = depth;
    img->pixels = (uint8_t *)calloc((size_t)w * h, 1);
    if (!img->pixels) { snprintf(msg, msglen, "out of memory"); return -1; }
    const uint8_t *src = d + data_off;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int bit = x * depth;
            uint8_t byte = src[(size_t)y * rb + bit / 8];
            img->pixels[y * w + x] = (uint8_t)((byte >> (8 - depth - (bit % 8))) & ((1 << depth) - 1));
        }
    return 0;
}
