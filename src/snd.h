/* 'snd ' resource decoder (format 1 and 2, standard sampled-sound header, 8-bit unsigned PCM) and 'clut' colour tables. */
#ifndef APEIRON_SND_H
#define APEIRON_SND_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    double rate_hz;            /* sample rate, 22254.54... for Apeiron's sounds */
    int channels;              /* 1 */
    int bits;                  /* 8 (unsigned samples, silence is 0x80) */
    size_t frames;
    const uint8_t *samples;    /* points into the resource */
    uint32_t loop_start, loop_end;
} SndData;

/* Returns 0 on success; on failure a message is written to msg. */
int snd_decode(const uint8_t *data, size_t len, SndData *out, char *msg, size_t msglen);

/* clut: returns the number of entries (0 on error); rgb receives r,g,b per index (up to 256). */
int clut_decode(const uint8_t *data, size_t len, uint8_t rgb[256 * 3]);

#endif
