/* Minimal PNG and WAV writers (no compression, no dependencies) for the exporter tool. */
#ifndef APEIRON_WRITERS_H
#define APEIRON_WRITERS_H

#include <stddef.h>
#include <stdint.h>

/* Indexed PNG: pixels are w*h palette indices, palette is npal RGB triples. Returns 0 on success. */
int write_png_indexed(const char *path, int w, int h, const uint8_t *pixels, const uint8_t *palette, int npal);
/* RGB PNG: pixels are w*h*3 bytes. */
int write_png_rgb(const char *path, int w, int h, const uint8_t *rgb);
/* 8-bit unsigned mono WAV. */
int write_wav8(const char *path, const uint8_t *samples, size_t frames, unsigned rate);

#endif
