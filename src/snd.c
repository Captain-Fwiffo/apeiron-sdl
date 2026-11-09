#include "snd.h"

#include <stdio.h>
#include <string.h>

#include "rsrc.h"

int snd_decode(const uint8_t *d, size_t len, SndData *out, char *msg, size_t msglen)
{
    memset(out, 0, sizeof *out);
    if (len < 8) { snprintf(msg, msglen, "too short"); return 1; }
    size_t pos = 2;
    unsigned fmt = be16(d);
    if (fmt == 1) {
        unsigned n = be16(d + pos);
        pos += 2 + (size_t)n * 6;                 /* synthesizer list: id (2) + init option (4) each */
    } else if (fmt == 2) {
        pos += 2;                                  /* reference count */
    } else {
        snprintf(msg, msglen, "unknown snd format %u", fmt);
        return 1;
    }
    if (pos + 2 > len) { snprintf(msg, msglen, "truncated command list"); return 1; }
    unsigned ncmd = be16(d + pos);
    pos += 2;
    long header = -1;
    for (unsigned i = 0; i < ncmd; i++) {
        if (pos + 8 > len) { snprintf(msg, msglen, "truncated command"); return 1; }
        unsigned cmd = be16(d + pos) & 0x7FFF;
        uint32_t p2 = be32(d + pos + 4);
        if (cmd == 0x50 || cmd == 0x51) header = (long)p2;       /* soundCmd, bufferCmd: offset of the sound header */
        pos += 8;
    }
    if (header < 0 || (size_t)header + 22 > len) { snprintf(msg, msglen, "no sound header"); return 1; }
    const uint8_t *h = d + header;
    uint32_t length = be32(h + 4);
    uint32_t rate = be32(h + 8);
    uint8_t encode = h[20];
    if (encode != 0x00) { snprintf(msg, msglen, "extended or compressed sound header (encode 0x%02X) is not supported", encode); return 1; }
    if ((size_t)header + 22 + length > len) { snprintf(msg, msglen, "sound data runs past the resource (%u bytes)", length); return 1; }
    out->rate_hz = rate / 65536.0;
    out->channels = 1;
    out->bits = 8;
    out->frames = length;
    out->samples = h + 22;
    out->loop_start = be32(h + 12);
    out->loop_end = be32(h + 16);
    return 0;
}

int clut_decode(const uint8_t *d, size_t len, uint8_t rgb[256 * 3])
{
    if (len < 8) return 0;
    unsigned flags = be16(d + 4);
    int n = (int)(int16_t)be16(d + 6) + 1;
    if (n < 1 || n > 256 || len < 8 + (size_t)n * 8) return 0;
    memset(rgb, 0, 256 * 3);
    int used = 0;
    for (int j = 0; j < n; j++) {
        const uint8_t *e = d + 8 + j * 8;
        int idx = (flags & 0x8000) ? (be16(e) & 0xFF) : j;
        rgb[idx * 3] = e[2]; rgb[idx * 3 + 1] = e[4]; rgb[idx * 3 + 2] = e[6];
        if (idx + 1 > used) used = idx + 1;
    }
    return used;
}
