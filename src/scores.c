#include "scores.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)(v >> 16)); wr16(p + 2, (uint16_t)v); }

static void get_pstr(const uint8_t *p, char *out)
{
    unsigned n = p[0];
    memcpy(out, p + 1, n);
    out[n] = 0;
}

static void put_pstr(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    if (n > 255) n = 255;
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
}

int scores_parse(ScoreTable *t, const uint8_t *data, size_t len)
{
    if (len != SCORES_SIZE) return -1;
    memmove(t->raw, data, SCORES_SIZE);
    get_pstr(data, t->last_name);
    t->volume = (int16_t)rd16(data + 0x100);
    t->setting2 = (int16_t)rd16(data + 0x102);
    t->music = (int16_t)rd16(data + 0x104);
    for (int i = 0; i < SCORES_ENTRIES; i++) {
        get_pstr(data + 0x106 + i * 256, t->name[i]);
        t->score[i] = (int32_t)rd32(data + 0xB06 + i * 4);
        t->wave[i] = (int16_t)rd16(data + 0xB2E + i * 2);
    }
    return 0;
}

void scores_pack(ScoreTable *t)
{
    uint8_t *d = t->raw;
    put_pstr(d, t->last_name);
    wr16(d + 0x100, (uint16_t)t->volume);
    wr16(d + 0x102, (uint16_t)t->setting2);
    wr16(d + 0x104, (uint16_t)t->music);
    for (int i = 0; i < SCORES_ENTRIES; i++) {
        put_pstr(d + 0x106 + i * 256, t->name[i]);
        wr32(d + 0xB06 + i * 4, (uint32_t)t->score[i]);
        wr16(d + 0xB2E + i * 2, (uint16_t)t->wave[i]);
    }
}

int scores_position(const ScoreTable *t, int32_t score)
{
    for (int i = 0; i < SCORES_ENTRIES; i++)
        if (score > t->score[i]) return i;
    return -1;
}

void scores_insert(ScoreTable *t, int pos, const char *name, int32_t score, int16_t wave)
{
    uint8_t *d = t->raw;
    if (pos < 0 || pos >= SCORES_ENTRIES) return;
    for (int i = SCORES_ENTRIES - 2; i >= pos; i--) {
        memcpy(d + 0xB06 + (i + 1) * 4, d + 0xB06 + i * 4, 4);
        memcpy(d + 0xB2E + (i + 1) * 2, d + 0xB2E + i * 2, 2);
        memmove(d + 0x106 + (i + 1) * 256, d + 0x106 + i * 256, 256);
    }
    wr32(d + 0xB06 + pos * 4, (uint32_t)score);
    wr16(d + 0xB2E + pos * 2, (uint16_t)wave);
    put_pstr(d + 0x106 + pos * 256, name);
    put_pstr(d, name);
    scores_parse(t, d, SCORES_SIZE);
}

void scores_clear(ScoreTable *t, const char *name)
{
    uint8_t *d = t->raw;
    for (int i = 0; i < SCORES_ENTRIES; i++) {
        put_pstr(d + 0x106 + i * 256, name);
        wr32(d + 0xB06 + i * 4, 0);
        wr16(d + 0xB2E + i * 2, 1);
    }
    scores_parse(t, d, SCORES_SIZE);
}
