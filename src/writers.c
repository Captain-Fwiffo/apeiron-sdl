#include "writers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_ready) crc_init();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    put32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = crc_update(0, (const uint8_t *)type, 4);
    crc = crc_update(crc, data, len);
    uint8_t c[4];
    put32(c, crc);
    fwrite(c, 1, 4, f);
}

static int write_png(const char *path, int w, int h, int color_type, int bytes_per_pixel, const uint8_t *pixels, const uint8_t *palette, int npal)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = (uint8_t)color_type; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    if (color_type == 3) chunk(f, "PLTE", palette, (size_t)npal * 3);

    size_t row = (size_t)w * bytes_per_pixel + 1;          /* filter byte + pixels */
    size_t raw_len = row * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    for (int y = 0; y < h; y++) {
        raw[row * y] = 0;
        memcpy(raw + row * y + 1, pixels + (size_t)y * w * bytes_per_pixel, (size_t)w * bytes_per_pixel);
    }
    /* zlib stream of stored blocks */
    size_t nblocks = (raw_len + 65534) / 65535;
    if (nblocks == 0) nblocks = 1;
    size_t z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!z) { free(raw); fclose(f); return -1; }
    size_t zp = 0;
    z[zp++] = 0x78; z[zp++] = 0x01;
    size_t off = 0;
    do {
        size_t n = raw_len - off;
        if (n > 65535) n = 65535;
        int last = (off + n >= raw_len);
        z[zp++] = (uint8_t)last;
        z[zp++] = (uint8_t)(n & 0xFF); z[zp++] = (uint8_t)(n >> 8);
        z[zp++] = (uint8_t)(~n & 0xFF); z[zp++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + zp, raw + off, n);
        zp += n; off += n;
    } while (off < raw_len);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    put32(z + zp, (b << 16) | a);
    zp += 4;
    chunk(f, "IDAT", z, zp);
    chunk(f, "IEND", NULL, 0);
    free(raw); free(z);
    return fclose(f) == 0 ? 0 : -1;
}

int write_png_indexed(const char *path, int w, int h, const uint8_t *pixels, const uint8_t *palette, int npal)
{
    if (npal < 1) npal = 1;
    if (npal > 256) npal = 256;
    return write_png(path, w, h, 3, 1, pixels, palette, npal);
}

int write_png_rgb(const char *path, int w, int h, const uint8_t *rgb)
{
    return write_png(path, w, h, 2, 3, rgb, NULL, 0);
}

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int write_wav8(const char *path, const uint8_t *samples, size_t frames, unsigned rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); le32(h + 4, (uint32_t)(36 + frames));
    memcpy(h + 8, "WAVEfmt ", 8); le32(h + 16, 16);
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;                 /* PCM, mono */
    le32(h + 24, rate); le32(h + 28, rate);                     /* byte rate = rate for 8-bit mono */
    h[32] = 1; h[33] = 0; h[34] = 8; h[35] = 0;
    memcpy(h + 36, "data", 4); le32(h + 40, (uint32_t)frames);
    fwrite(h, 1, 44, f);
    fwrite(samples, 1, frames, f);
    return fclose(f) == 0 ? 0 : -1;
}
