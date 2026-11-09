#include "pictdraw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"

typedef struct { const uint8_t *d; size_t len, pos; int bad; } R;

static unsigned u8(R *r) { if (r->pos + 1 > r->len) { r->bad = 1; return 0; } return r->d[r->pos++]; }
static unsigned u16(R *r) { if (r->pos + 2 > r->len) { r->bad = 1; r->pos = r->len; return 0; } unsigned v = (unsigned)(r->d[r->pos] << 8 | r->d[r->pos + 1]); r->pos += 2; return v; }
static int s16(R *r) { return (int16_t)u16(r); }
static void skip(R *r, size_t n) { if (r->pos + n > r->len) { r->bad = 1; r->pos = r->len; } else r->pos += n; }
static void align(R *r) { if (r->pos & 1) skip(r, 1); }

typedef struct { int top, left, bottom, right; } Rc;
static Rc rect(R *r) { Rc c; c.top = s16(r); c.left = s16(r); c.bottom = s16(r); c.right = s16(r); return c; }

static uint32_t snap(const uint8_t *pal, unsigned red, unsigned green, unsigned blue)
{
    if (!pal) return 0xFF000000u | red << 16 | green << 8 | blue;
    int best = 0; long bd = 1L << 30;
    for (int i = 0; i < 256; i++) {
        long dr = (long)pal[i * 3] - (long)red, dg = (long)pal[i * 3 + 1] - (long)green, db = (long)pal[i * 3 + 2] - (long)blue, d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return 0xFF000000u | (uint32_t)pal[best * 3] << 16 | (uint32_t)pal[best * 3 + 1] << 8 | pal[best * 3 + 2];
}

static void fill(uint32_t *fb, int w, int h, Rc c, uint32_t col)
{
    for (int y = c.top; y < c.bottom; y++) for (int x = c.left; x < c.right; x++) if (x >= 0 && y >= 0 && x < w && y < h) fb[y * w + x] = col;
}

static int unpack(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    size_t i = 0, o = 0;
    while (i < inlen && o < outlen) {
        int n = (int8_t)in[i++];
        if (n >= 0) { size_t c = (size_t)n + 1; if (i + c > inlen || o + c > outlen) return 1; memcpy(out + o, in + i, c); i += c; o += c; }
        else if (n != -128) { size_t c = (size_t)(1 - n); if (i >= inlen || o + c > outlen) return 1; memset(out + o, in[i++], c); o += c; }
    }
    return o == outlen ? 0 : 1;
}

/* BitsRect / PackBitsRect (indexed, 1 to 8 bits) drawn at its destination rectangle */
static int bits(R *r, int op, uint32_t *fb, int w, int h, const uint8_t *pal)
{
    int packed = op >= 0x98, has_rgn = (op == 0x91 || op == 0x99);
    unsigned rbraw = u16(r);
    int ispix = (rbraw & 0x8000) != 0, rb = (int)(rbraw & 0x3FFF);
    Rc bounds = rect(r);
    int depth = 1, pack_type = 0;
    uint8_t ct[256 * 3];
    memset(ct, 0, sizeof ct);
    if (ispix) {
        skip(r, 2); pack_type = (int)u16(r); skip(r, 14); depth = (int)u16(r); skip(r, 2 + 2 + 4 + 4 + 4);
        skip(r, 4); unsigned flags = u16(r); int n = s16(r) + 1;
        if (n < 1 || n > 256) return 1;
        for (int j = 0; j < n; j++) {
            unsigned value = u16(r), rr = u16(r), gg = u16(r), bb = u16(r);
            int idx = (flags & 0x8000) ? j : (int)(value & 0xFF);
            ct[idx * 3] = (uint8_t)(rr >> 8); ct[idx * 3 + 1] = (uint8_t)(gg >> 8); ct[idx * 3 + 2] = (uint8_t)(bb >> 8);
        }
    } else { ct[0] = ct[1] = ct[2] = 255; }
    Rc src = rect(r), dst = rect(r);
    skip(r, 2);
    if (has_rgn) { unsigned n = u16(r); if (n >= 2) skip(r, n - 2); }
    if (r->bad || (depth != 1 && depth != 2 && depth != 4 && depth != 8)) return 1;
    int bw = bounds.right - bounds.left, bh = bounds.bottom - bounds.top;
    if (bw <= 0 || bh <= 0 || rb <= 0 || bw > 4096 || bh > 4096) return 1;
    uint8_t *row = (uint8_t *)malloc((size_t)rb + 8);
    if (!row) return 1;
    for (int y = 0; y < bh; y++) {
        if (!packed || rb < 8 || pack_type == 1) { if (r->pos + (size_t)rb > r->len) { free(row); return 1; } memcpy(row, r->d + r->pos, (size_t)rb); r->pos += (size_t)rb; }
        else {
            size_t n = rb > 250 ? u16(r) : u8(r);
            if (r->bad || r->pos + n > r->len || unpack(r->d + r->pos, n, row, (size_t)rb)) { free(row); return 1; }
            r->pos += n;
        }
        int sy = y - (src.top - bounds.top);
        if (sy < 0 || sy >= src.bottom - src.top) continue;
        int dy = dst.top + sy;
        for (int x = 0; x < src.right - src.left; x++) {
            int bx = x + (src.left - bounds.left), dx = dst.left + x;
            if (bx < 0 || bx >= bw || dx < 0 || dy < 0 || dx >= w || dy >= h) continue;
            int v = depth == 8 ? row[bx] : depth == 4 ? (row[bx >> 1] >> ((bx & 1) ? 0 : 4)) & 15 : depth == 2 ? (row[bx >> 2] >> (6 - 2 * (bx & 3))) & 3 : (row[bx >> 3] >> (7 - (bx & 7))) & 1;
            fb[dy * w + dx] = snap(pal, ct[v * 3], ct[v * 3 + 1], ct[v * 3 + 2]);
        }
    }
    free(row);
    return 0;
}

int pict_draw(const uint8_t *data, size_t len, uint32_t *fb, int w, int h, const uint8_t *pal, char *msg, size_t msglen)
{
    if (len < 16 || data[10] != 0x00 || data[11] != 0x11 || data[12] != 0x02 || data[13] != 0xFF) { snprintf(msg, msglen, "not a version 2 picture"); return 1; }
    R r = { data, len, 14, 0 };
    uint32_t color = snap(pal, 0, 0, 0);
    int px = 0, py = 0, size = 12;
    int bold = 0, underline = 0, prev_start = 0, spextra = 0, ink_end = -1000;
    for (;;) {
        if (r.bad || r.pos >= r.len) { snprintf(msg, msglen, "ran off the end"); return 1; }
        unsigned op = u16(&r);
        switch (op) {
        case 0x0000: case 0x001C: case 0x001E: case 0x0038: case 0x0039: case 0x003A: case 0x003B: case 0x003C: break;
        case 0x0C00: skip(&r, 24); break;
        case 0x0001: { unsigned n = u16(&r); if (n >= 2) skip(&r, n - 2); break; }
        case 0x00FF: return 0;
        case 0x00A0: skip(&r, 2); break;
        case 0x00A1: { skip(&r, 2); unsigned n = u16(&r); skip(&r, n); align(&r); break; }
        case 0x0031: { Rc c = rect(&r); fill(fb, w, h, c, color); break; }                       /* PaintRect */
        case 0x0020: skip(&r, 8); break;
        case 0x0021: skip(&r, 4); break;
        case 0x0022: skip(&r, 6); break;
        case 0x0023: skip(&r, 2); break;
        case 0x0030: case 0x0032: case 0x0033: case 0x0034: skip(&r, 8); break;
        case 0x0002: case 0x0009: case 0x000A: case 0x0010: skip(&r, 8); break;
        case 0x0007: case 0x000B: case 0x000C: skip(&r, 4); break;
        case 0x0003: case 0x0008: case 0x0005: case 0x0015: case 0x0016: skip(&r, 2); break;
        case 0x0004: { unsigned f = u16(&r) >> 8; bold = f & 1; underline = (f & 4) != 0; break; }
        case 0x000D: size = (int)u16(&r); break;
        case 0x002E: spextra = (int)u16(&r); skip(&r, 2); break;                          /* SpExtra: pixels added to every space (the pages are justified) */
        case 0x0006: case 0x000E: case 0x000F: skip(&r, 4); break;
        case 0x001A: { unsigned rr = u16(&r) >> 8, gg = u16(&r) >> 8, bb = u16(&r) >> 8; color = snap(pal, rr, gg, bb); break; }
        case 0x001B: skip(&r, 6); break;
        case 0x002C: { unsigned n = u16(&r); skip(&r, n); align(&r); break; }
        case 0x0028: case 0x0029: case 0x002A: case 0x002B: {
            if (op == 0x0028) { py = s16(&r); px = s16(&r); }
            else if (op == 0x0029) {                                           /* from where the previous text ended */
                int dh = (int8_t)u8(&r);
                px += dh;
                if (dh < 0 && px < ink_end + 7) px = ink_end + 7;                   /* the offsets were made for Geneva's widths: do not run into the word before */
            }
            else if (op == 0x002A) { px = prev_start; py += (int8_t)u8(&r); }   /* a line down from where the previous text began (as the pages need it) */
            else { px += (int8_t)u8(&r); py += (int8_t)u8(&r); }
            prev_start = px;
            unsigned n = u8(&r);
            if (r.pos + n > r.len) { snprintf(msg, msglen, "truncated text"); return 1; }
            char s[256];
            size_t m = 0;
            for (unsigned i = 0; i < n && m < 250; i++) {
                unsigned char c = r.d[r.pos + i];
                if (c == 0xD0 || c == 0xD1) s[m++] = '-';                                          /* en and em dash */
                else if (c == 0xD2 || c == 0xD3) s[m++] = '"';
                else if (c == 0xD5 || c == 0xD4) s[m++] = '\'';
                else if (c == 0xA5) s[m++] = '*';                                                 /* the bullet */
                else if (c < 32) continue;                                                        /* line ends in the text */
                else if (c > 126) s[m++] = '?';
                else s[m++] = (char)c;
            }
            s[m] = 0;
            skip(&r, n); align(&r);
            const FFont *ff = size >= 16 ? &font_serif18 : bold ? &font_sans9b : &font_sans9;          /* New York / Geneva stand-ins from open fonts */
            int wd = 0;
            for (const char *q = s; ; ) {                                                         /* word by word, so that each space can take SpExtra */
                const char *sp = strchr(q, ' ');
                char word[256];
                size_t wl = sp ? (size_t)(sp - q) : strlen(q);
                memcpy(word, q, wl); word[wl] = 0;
                wd += ffont_draw(fb, w, h, px + wd, py, ff, color, word);
                if (!sp) break;
                wd += ffont_width(ff, " ") + spextra;
                q = sp + 1;
            }
            { int e = (int)strlen(s); while (e > 0 && s[e - 1] == ' ') e--; char tmp[256]; memcpy(tmp, s, (size_t)e); tmp[e] = 0; int nsp = 0; for (int k = 0; k < e; k++) if (tmp[k] == ' ') nsp++; ink_end = px + ffont_width(ff, tmp) + spextra * nsp; }
            if (underline) fill(fb, w, h, (Rc){ py + 1, px, py + 2, px + wd }, color);
            px += wd;
            break;
        }
        case 0x0090: case 0x0091: case 0x0098: case 0x0099:
            if (bits(&r, (int)op, fb, w, h, pal)) { snprintf(msg, msglen, "a bitmap could not be drawn"); return 1; }
            align(&r);
            break;
        default:
            snprintf(msg, msglen, "unsupported operation 0x%04X at %zu", op, r.pos - 2);
            return 1;
        }
    }
}
