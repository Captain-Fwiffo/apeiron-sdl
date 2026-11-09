#include "cicn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rd16s(const uint8_t *p) { return (int16_t)(p[0] << 8 | p[1]); }
static unsigned rd16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }

/* layout: PixMap (50 bytes), mask BitMap (14), icon BitMap (14), icon data handle (4), mask data, bitmap data, ColorTable, pixel data */
int cicn_decode(const uint8_t *d, size_t len, CicnImage *img, char *msg, size_t msglen)
{
    memset(img, 0, sizeof *img);
    if (len < 50 + 14 + 14 + 4 + 8) { snprintf(msg, msglen, "cicn too short"); return 1; }
    unsigned rowbytes = rd16(d + 4) & 0x3FFF;
    int top = rd16s(d + 6), left = rd16s(d + 8), bottom = rd16s(d + 10), right = rd16s(d + 12);
    int depth = (int)rd16(d + 32);
    int w = right - left, h = bottom - top;
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || (depth != 1 && depth != 2 && depth != 4 && depth != 8)) { snprintf(msg, msglen, "cicn: unsupported size %dx%d depth %d", w, h, depth); return 1; }
    unsigned mask_rb = rd16(d + 50 + 4);
    unsigned icon_rb = rd16(d + 50 + 14 + 4);
    size_t p = 50 + 14 + 14 + 4;
    size_t mask_off = p, mask_len = (size_t)mask_rb * (size_t)h;
    p += mask_len + (size_t)icon_rb * (size_t)h;
    if (p + 8 > len) { snprintf(msg, msglen, "cicn: truncated"); return 1; }
    unsigned ct_size = rd16(d + p + 6);
    size_t ct_off = p + 8, pix_off = ct_off + ((size_t)ct_size + 1) * 8;
    if (pix_off + (size_t)rowbytes * (size_t)h > len || ct_size > 255) { snprintf(msg, msglen, "cicn: bad colour table or data"); return 1; }
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0xFF000000u;
    for (unsigned i = 0; i <= ct_size; i++) {
        const uint8_t *e = d + ct_off + (size_t)i * 8;
        unsigned idx = rd16(e) & 0xFF;
        pal[idx] = 0xFF000000u | (uint32_t)(e[2]) << 16 | (uint32_t)(e[4]) << 8 | e[6];
    }
    img->argb = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!img->argb) { snprintf(msg, msglen, "out of memory"); return 1; }
    img->width = w; img->height = h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *row = d + pix_off + (size_t)y * rowbytes;
            unsigned bit = (unsigned)x * (unsigned)depth, v = (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1u);
            int opaque = (d[mask_off + (size_t)y * mask_rb + (unsigned)(x >> 3)] >> (7 - (x & 7))) & 1;
            img->argb[y * w + x] = opaque ? pal[v] : 0u;
        }
    return 0;
}

void cicn_free(CicnImage *img) { free(img->argb); memset(img, 0, sizeof *img); }

/* layout: type (0x8001), pixmap offset, pixel data offset, 3 more fields (20 bytes), the one-bit picture (32 bytes), the mask (32), the hot spot (v, h), table and id; the
 * PixMap is at its offset, the pixels at theirs, and the colour table follows the pixels */
int crsr_decode(const uint8_t *d, size_t len, CicnImage *img, int *hot_x, int *hot_y, char *msg, size_t msglen)
{
    memset(img, 0, sizeof *img);
    if (len < 96 + 50 || rd16(d) != 0x8001) { snprintf(msg, msglen, "not a colour cursor"); return 1; }
    size_t pm = ((size_t)d[2] << 24 | (size_t)d[3] << 16 | (size_t)d[4] << 8 | d[5]), px = ((size_t)d[6] << 24 | (size_t)d[7] << 16 | (size_t)d[8] << 8 | d[9]);
    if (pm + 50 > len) { snprintf(msg, msglen, "bad pixmap offset"); return 1; }
    unsigned rb = rd16(d + pm + 4) & 0x3FFF;
    int w, h;
    int top = rd16s(d + pm + 6), left = rd16s(d + pm + 8), bottom = rd16s(d + pm + 10), right = rd16s(d + pm + 12);
    w = right - left; h = bottom - top;
    int depth = (int)rd16(d + pm + 32);
    if (w <= 0 || h <= 0 || w > 32 || h > 32 || (depth != 1 && depth != 2 && depth != 4 && depth != 8)) { snprintf(msg, msglen, "cursor: unsupported size/depth"); return 1; }
    size_t ct = px + (size_t)rb * (size_t)h;
    if (ct + 8 > len) { snprintf(msg, msglen, "cursor: truncated"); return 1; }
    unsigned ct_size = rd16(d + ct + 6);
    if (ct_size > 255 || ct + 8 + ((size_t)ct_size + 1) * 8 > len || px + (size_t)rb * (size_t)h > len) { snprintf(msg, msglen, "cursor: bad colour table"); return 1; }
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0xFF000000u;
    for (unsigned i = 0; i <= ct_size; i++) {
        const uint8_t *e = d + ct + 8 + (size_t)i * 8;
        pal[rd16(e) & 0xFF] = 0xFF000000u | (uint32_t)e[2] << 16 | (uint32_t)e[4] << 8 | e[6];
    }
    img->argb = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!img->argb) { snprintf(msg, msglen, "out of memory"); return 1; }
    img->width = w; img->height = h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *row = d + px + (size_t)y * rb;
            unsigned bit = (unsigned)x * (unsigned)depth, v = (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1u);
            int opaque = y < 16 && x < 16 && ((d[52 + y * 2 + (x >> 3)] >> (7 - (x & 7))) & 1);
            img->argb[y * w + x] = opaque ? pal[v] : 0u;
        }
    *hot_y = rd16s(d + 84); *hot_x = rd16s(d + 86);
    return 0;
}

/* the Mac's standard 8-bit colour table: a 6x6x6 cube from white down (0..214), ramps of red, green, blue and grey (215..254), black (255) */
static uint32_t std8_color(int i)
{
    static const int ramp[10] = { 0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11 };
    static const int cube[6] = { 0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00 };
    int r, g, b;
    if (i < 215) { r = cube[i / 36]; g = cube[(i / 6) % 6]; b = cube[i % 6]; }
    else if (i < 255) { int k = ramp[(i - 215) % 10], q = (i - 215) / 10; r = q == 0 ? k : 0; g = q == 1 ? k : 0; b = q == 2 ? k : 0; if (q == 3) r = g = b = k; }
    else r = g = b = 0;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

int icon_decode(const uint8_t *icl8, size_t icl8_len, const uint8_t *icn, size_t icn_len, CicnImage *img)
{
    if (icl8_len < 1024 || icn_len < 256) return 1;
    img->width = img->height = 32;
    img->argb = malloc(32 * 32 * sizeof(uint32_t));
    if (!img->argb) return 1;
    for (int i = 0; i < 1024; i++) {
        int on = icn[128 + i / 8] >> (7 - i % 8) & 1;
        img->argb[i] = on ? 0xFF000000u | std8_color(icl8[i]) : 0;
    }
    return 0;
}
