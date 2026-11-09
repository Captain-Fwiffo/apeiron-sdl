#include "pict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsrc.h"

typedef struct {
    const uint8_t *d;
    size_t len, pos;
    int bad;
} Rd;

static uint32_t rd_u8(Rd *r) { if (r->pos + 1 > r->len) { r->bad = 1; return 0; } return r->d[r->pos++]; }
static uint32_t rd_u16(Rd *r) { if (r->pos + 2 > r->len) { r->bad = 1; r->pos = r->len; return 0; } uint32_t v = be16(r->d + r->pos); r->pos += 2; return v; }
static uint32_t rd_u32(Rd *r) { if (r->pos + 4 > r->len) { r->bad = 1; r->pos = r->len; return 0; } uint32_t v = be32(r->d + r->pos); r->pos += 4; return v; }
static void rd_skip(Rd *r, size_t n) { if (r->pos + n > r->len) { r->bad = 1; r->pos = r->len; } else r->pos += n; }

typedef struct { int top, left, bottom, right; } Rect;

static Rect rd_rect(Rd *r)
{
    Rect q;
    q.top = (int16_t)rd_u16(r); q.left = (int16_t)rd_u16(r); q.bottom = (int16_t)rd_u16(r); q.right = (int16_t)rd_u16(r);
    return q;
}

/* PackBits: unpack one row into out[outlen]; unit is 1 or 2 bytes. Returns 0 on success. */
static int unpack_row(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen, int unit)
{
    size_t i = 0, o = 0;
    while (i < inlen && o < outlen) {
        int c = (int8_t)in[i++];
        if (c >= 0) {
            size_t n = (size_t)(c + 1) * unit;
            if (i + n > inlen || o + n > outlen) return -1;
            memcpy(out + o, in + i, n);
            i += n; o += n;
        } else if (c != -128) {
            size_t n = (size_t)(1 - c);
            if (i + (size_t)unit > inlen || o + n * unit > outlen) return -1;
            for (size_t k = 0; k < n; k++) { memcpy(out + o, in + i, (size_t)unit); o += (size_t)unit; }
            i += (size_t)unit;
        }
    }
    return (o == outlen) ? 0 : -1;
}

static int decode_bitmap(Rd *r, int op, const Rect *frame, PictImage *img, char *msg, size_t msglen)
{
    int direct = (op == 0x9A || op == 0x9B);
    int has_rgn = (op == 0x91 || op == 0x99 || op == 0x9B);
    int packed = (op >= 0x98);
    if (direct) rd_skip(r, 4);
    uint32_t rowbytes_raw = rd_u16(r);
    int ispix = (rowbytes_raw & 0x8000) || direct;
    int rb = (int)(rowbytes_raw & 0x3FFF);
    Rect bounds = rd_rect(r);
    int pack_type = 0, depth = 1, cmp_count = 1;
    int npal = 0;
    uint8_t pal[256 * 3];
    memset(pal, 0, sizeof pal);
    if (ispix) {
        rd_skip(r, 2);                       /* pmVersion */
        pack_type = (int)rd_u16(r);
        rd_skip(r, 4 + 4 + 4 + 2);           /* packSize, hRes, vRes, pixelType */
        depth = (int)rd_u16(r);
        cmp_count = (int)rd_u16(r);
        rd_skip(r, 2 + 4 + 4 + 4);           /* cmpSize, planeBytes, pmTable, pmReserved */
        if (!direct) {
            rd_skip(r, 4);                   /* ctSeed */
            uint32_t flags = rd_u16(r);
            int n = (int)(int16_t)rd_u16(r) + 1;
            if (n < 1 || n > 256) { snprintf(msg, msglen, "bad colour table size %d", n); return PICT_BAD; }
            for (int j = 0; j < n; j++) {
                uint32_t value = rd_u16(r);
                uint32_t red = rd_u16(r), green = rd_u16(r), blue = rd_u16(r);
                int idx = (flags & 0x8000) ? j : (int)(value & 0xFF);          /* a device table (flag set) has no usable value field: entries are in order */
                if (idx >= 256) idx = 255;
                pal[idx * 3] = (uint8_t)(red >> 8); pal[idx * 3 + 1] = (uint8_t)(green >> 8); pal[idx * 3 + 2] = (uint8_t)(blue >> 8);
                if (idx + 1 > npal) npal = idx + 1;
            }
        }
    } else {
        /* a 1-bit BitMap: a set bit is black */
        pal[0] = pal[1] = pal[2] = 255;
        pal[3] = pal[4] = pal[5] = 0;
        npal = 2;
    }
    Rect src = rd_rect(r), dst = rd_rect(r);
    rd_skip(r, 2);                           /* transfer mode */
    if (has_rgn) { uint32_t n = rd_u16(r); if (n >= 2) rd_skip(r, n - 2); }
    if (r->bad) { snprintf(msg, msglen, "truncated bitmap header"); return PICT_BAD; }

    int bw = bounds.right - bounds.left, bh = bounds.bottom - bounds.top;
    if (bw <= 0 || bh <= 0 || bw > 8192 || bh > 8192) { snprintf(msg, msglen, "bad bitmap size %dx%d", bw, bh); return PICT_BAD; }
    int sw = src.right - src.left, sh = src.bottom - src.top;
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (sw != dw || sh != dh) { snprintf(msg, msglen, "scaled bitmap (src %dx%d, dst %dx%d) is not supported", sw, sh, dw, dh); return PICT_UNSUPPORTED; }

    int cw = frame->right - frame->left, ch = frame->bottom - frame->top;
    int rgb = direct && (depth == 16 || depth == 32);
    if (direct && !rgb) { snprintf(msg, msglen, "direct bitmap with %d bits is not supported", depth); return PICT_UNSUPPORTED; }
    if (!direct && depth != 1 && depth != 2 && depth != 4 && depth != 8) { snprintf(msg, msglen, "%d-bit indexed bitmap is not supported", depth); return PICT_UNSUPPORTED; }

    img->width = cw; img->height = ch; img->indexed = !rgb; img->depth = depth; img->bitmap_opcode = op;
    img->pixels = (uint8_t *)calloc((size_t)cw * ch, rgb ? 3 : 1);
    if (!img->pixels) { snprintf(msg, msglen, "out of memory"); return PICT_BAD; }
    if (!rgb) { memcpy(img->palette, pal, sizeof pal); img->palette_size = npal; }

    uint8_t *row = (uint8_t *)malloc((size_t)rb + 8);
    if (!row) { snprintf(msg, msglen, "out of memory"); return PICT_BAD; }
    int ox = dst.left - frame->left, oy = dst.top - frame->top;       /* where the destination rect sits on the canvas */
    for (int y = 0; y < bh; y++) {
        if (!packed || rb < 8 || pack_type == 1) {
            if (r->pos + (size_t)rb > r->len) { free(row); snprintf(msg, msglen, "truncated bitmap data"); return PICT_BAD; }
            memcpy(row, r->d + r->pos, (size_t)rb);
            r->pos += (size_t)rb;
        } else {
            size_t n = (rb > 250) ? rd_u16(r) : rd_u8(r);
            if (r->bad || r->pos + n > r->len) { free(row); snprintf(msg, msglen, "truncated packed row"); return PICT_BAD; }
            int unit = (depth == 16 && pack_type == 3) ? 2 : 1;
            if (unpack_row(r->d + r->pos, n, row, (size_t)rb, unit) != 0) { free(row); snprintf(msg, msglen, "bad PackBits row %d", y); return PICT_BAD; }
            r->pos += n;
        }
        int sy = y - (src.top - bounds.top);                /* row inside the source rect */
        if (sy < 0 || sy >= sh) continue;
        int cy = oy + sy;
        if (cy < 0 || cy >= ch) continue;
        for (int x = 0; x < sw; x++) {
            int bx = x + (src.left - bounds.left);
            int cx = ox + x;
            if (bx < 0 || bx >= bw || cx < 0 || cx >= cw) continue;
            if (!rgb) {
                int v;
                if (depth == 8) v = row[bx];
                else if (depth == 4) v = (row[bx >> 1] >> ((bx & 1) ? 0 : 4)) & 15;
                else if (depth == 2) v = (row[bx >> 2] >> (6 - 2 * (bx & 3))) & 3;
                else v = (row[bx >> 3] >> (7 - (bx & 7))) & 1;
                img->pixels[(size_t)cy * cw + cx] = (uint8_t)v;
            } else {
                uint8_t *p = &img->pixels[((size_t)cy * cw + cx) * 3];
                if (depth == 32) {
                    if (pack_type == 4) {                    /* planar: [A] R G B, each bw bytes */
                        int planes = cmp_count;
                        int base = (planes == 4) ? bw : 0;
                        p[0] = row[base + bx]; p[1] = row[base + bw + bx]; p[2] = row[base + 2 * bw + bx];
                    } else {                                 /* chunky xRGB */
                        p[0] = row[bx * 4 + 1]; p[1] = row[bx * 4 + 2]; p[2] = row[bx * 4 + 3];
                    }
                } else {                                     /* 16 bit, 5-5-5 */
                    unsigned v = be16(row + bx * 2);
                    p[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31); p[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31); p[2] = (uint8_t)((v & 31) * 255 / 31);
                }
            }
        }
    }
    free(row);
    return PICT_OK;
}

PictStatus pict_decode(const uint8_t *data, size_t len, PictImage *img, char *msg, size_t msglen)
{
    memset(img, 0, sizeof *img);
    msg[0] = 0;
    if (len < 14) { snprintf(msg, msglen, "too short"); return PICT_BAD; }
    Rd r = { data, len, 10, 0 };
    Rect frame;
    {
        Rd h = { data, len, 2, 0 };
        frame = rd_rect(&h);
    }
    int v2;
    if (data[10] == 0x00 && data[11] == 0x11 && data[12] == 0x02 && data[13] == 0xFF) { v2 = 1; r.pos = 14; }
    else if (data[10] == 0x11 && data[11] == 0x01) { v2 = 0; r.pos = 12; }
    else { snprintf(msg, msglen, "no PICT version opcode"); return PICT_BAD; }
    img->version = v2 ? 2 : 1;
    if (frame.right - frame.left <= 0 || frame.bottom - frame.top <= 0) { snprintf(msg, msglen, "empty picture frame"); return PICT_BAD; }

    for (;;) {
        if (r.pos >= r.len || r.bad) { snprintf(msg, msglen, "ran off the end"); return PICT_BAD; }
        unsigned op = v2 ? rd_u16(&r) : rd_u8(&r);
        if (r.bad) { snprintf(msg, msglen, "truncated"); return PICT_BAD; }
        if (v2 && op == 0x0C00) { rd_skip(&r, 24); continue; }
        if (op == 0x00FF) { snprintf(msg, msglen, "picture has no bitmap (vector drawing only)"); return PICT_NO_BITMAP; }
        switch (op) {
        case 0x0000: case 0x001C: case 0x001E: case 0x0038: case 0x0039: case 0x003A: case 0x003B: case 0x003C:
        case 0x0048: case 0x0049: case 0x004A: case 0x004B: case 0x004C: case 0x0058: case 0x0059: case 0x005A: case 0x005B: case 0x005C:
            break;
        case 0x0001: { uint32_t n = rd_u16(&r); if (n >= 2) rd_skip(&r, n - 2); break; }
        case 0x0002: case 0x0009: case 0x000A: case 0x0010: case 0x0020: case 0x0030: case 0x0031: case 0x0032: case 0x0033: case 0x0034:
        case 0x0040: case 0x0041: case 0x0042: case 0x0043: case 0x0044: case 0x0050: case 0x0051: case 0x0052: case 0x0053: case 0x0054:
            rd_skip(&r, 8); break;
        case 0x0003: case 0x0005: case 0x0008: case 0x000D: case 0x0015: case 0x0016: case 0x0023: case 0x00A0:
            rd_skip(&r, 2); break;
        case 0x0004: rd_skip(&r, v2 ? 2 : 1); break;
        case 0x0006: case 0x0007: case 0x000B: case 0x000C: case 0x000E: case 0x000F: case 0x0021: case 0x0068: case 0x0069: case 0x006A: case 0x006B: case 0x006C:
            rd_skip(&r, 4); break;
        case 0x001A: case 0x001B: case 0x001D: case 0x001F: case 0x0022:
            rd_skip(&r, 6); break;
        case 0x0060: case 0x0061: case 0x0062: case 0x0063: case 0x0064: rd_skip(&r, 12); break;
        case 0x002C: case 0x002D: case 0x002E: case 0x002F: { uint32_t n = rd_u16(&r); rd_skip(&r, n); if (v2 && (r.pos & 1)) rd_skip(&r, 1); break; }
        case 0x0028: { rd_skip(&r, 4); uint32_t n = rd_u8(&r); rd_skip(&r, n); if (v2 && (r.pos & 1)) rd_skip(&r, 1); break; }
        case 0x0029: case 0x002A: { rd_skip(&r, 1); uint32_t n = rd_u8(&r); rd_skip(&r, n); if (v2 && (r.pos & 1)) rd_skip(&r, 1); break; }
        case 0x002B: { rd_skip(&r, 2); uint32_t n = rd_u8(&r); rd_skip(&r, n); if (v2 && (r.pos & 1)) rd_skip(&r, 1); break; }
        case 0x00A1: { rd_skip(&r, 2); uint32_t n = rd_u16(&r); rd_skip(&r, n); if (v2 && (r.pos & 1)) rd_skip(&r, 1); break; }
        case 0x0090: case 0x0091: case 0x0098: case 0x0099: case 0x009A: case 0x009B: {
            PictStatus s = (PictStatus)decode_bitmap(&r, (int)op, &frame, img, msg, msglen);
            if (s != PICT_OK) { pict_free(img); return s; }
            return PICT_OK;
        }
        default:
            if ((op >= 0x0070 && op <= 0x0077) || (op >= 0x0080 && op <= 0x0087)) { uint32_t n = rd_u16(&r); if (n >= 2) rd_skip(&r, n - 2); break; }
            if (v2 && op >= 0x0100 && op <= 0x7FFF) { rd_skip(&r, (op >> 8) * 2); break; }
            if (v2 && ((op >= 0x00B0 && op <= 0x00CF) || (op >= 0x8000 && op <= 0x80FF))) break;
            if (v2 && ((op >= 0x00D0 && op <= 0x00FE) || (op >= 0x8100))) { uint32_t n = rd_u32(&r); rd_skip(&r, n); break; }
            snprintf(msg, msglen, "unsupported PICT opcode 0x%04X at offset %zu", op, r.pos - (v2 ? 2 : 1));
            return PICT_UNSUPPORTED;
        }
    }
}

void pict_free(PictImage *img)
{
    free(img->pixels);
    img->pixels = NULL;
}
