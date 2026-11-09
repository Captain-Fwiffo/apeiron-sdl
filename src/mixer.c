#include "mixer.h"

#include <stdlib.h>
#include <string.h>

void mixer_init(Mixer *m)
{
    memset(m, 0, sizeof *m);
    for (int i = 0; i < MIXER_CHANNELS; i++) m->ch[i].id = -1;
    m->volume = 2;
    m->music_id = m->music_next = -1;
}

void mixer_free(Mixer *m)
{
    for (int i = 0; i < MIXER_MAX_SOUNDS; i++) free(m->sound[i].samples);
    memset(m, 0, sizeof *m);
}

int mixer_load(Mixer *m, int id, const uint8_t *samples, uint32_t frames, double rate)
{
    if (id < 0 || id >= MIXER_MAX_SOUNDS) return -1;
    free(m->sound[id].samples);
    m->sound[id].samples = (uint8_t *)malloc(frames ? frames : 1);
    if (!m->sound[id].samples) return -1;
    memcpy(m->sound[id].samples, samples, frames);
    m->sound[id].frames = frames;
    m->sound[id].rate = rate;
    return 0;
}

void mixer_play(Mixer *m, int id, int priority)
{
    if (id < 0 || id >= MIXER_MAX_SOUNDS || !m->sound[id].samples) return;
    int pick = -1;
    for (int i = 0; i < MIXER_CHANNELS && pick < 0; i++) if (m->ch[i].id < 0) pick = i;
    if (pick < 0) {
        int lowest = 0;
        for (int i = 1; i < MIXER_CHANNELS; i++) if (m->ch[i].priority < m->ch[lowest].priority) lowest = i;
        if (priority < m->ch[lowest].priority) return;
        pick = lowest;
    }
    m->ch[pick].id = id; m->ch[pick].priority = priority; m->ch[pick].pos = 0.0;
}

void mixer_music_start(Mixer *m, int start_id, int loop_id)
{
    if (m->music_id >= 0) return;                         /* the channel is taken: the original only replaces a track of lower priority */
    if (start_id < 0 || start_id >= MIXER_MAX_SOUNDS || !m->sound[start_id].samples) start_id = loop_id;
    if (start_id < 0 || start_id >= MIXER_MAX_SOUNDS || !m->sound[start_id].samples) return;
    m->music_id = start_id; m->music_next = loop_id; m->music_pos = 0.0;
}

void mixer_music_stop(Mixer *m) { m->music_id = -1; }

void mixer_stop_all(Mixer *m)
{
    for (int i = 0; i < MIXER_CHANNELS; i++) m->ch[i].id = -1;
    m->music_id = -1;
}

int mixer_is_playing(const Mixer *m, int id)
{
    for (int i = 0; i < MIXER_CHANNELS; i++) if (m->ch[i].id == id) return 1;
    return 0;
}

void mixer_mix(Mixer *m, int16_t *out, int n, int device_rate)
{
    static const int gain[4] = { 0, 40, 70, 100 };       /* percent, by the volume setting */
    int g = gain[m->volume & 3];
    for (int i = 0; i < n; i++) {
        int32_t sum = 0;
        for (int c = 0; c < MIXER_CHANNELS; c++) {
            MixChannel *ch = &m->ch[c];
            if (ch->id < 0) continue;
            const MixSound *s = &m->sound[ch->id];
            uint32_t p = (uint32_t)ch->pos;
            if (p + 1 >= s->frames) { ch->id = -1; continue; }
            double frac = ch->pos - p;
            double v = ((double)s->samples[p] * (1.0 - frac) + (double)s->samples[p + 1] * frac) - 128.0;
            sum += (int32_t)(v * 256.0);
            ch->pos += s->rate / device_rate;
        }
        if (m->music_id >= 0) {
            const MixSound *s = &m->sound[m->music_id];
            uint32_t p = (uint32_t)m->music_pos;
            if (p + 1 >= s->frames) {                     /* done: the loop sound follows (and follows itself) */
                int nx = m->music_next;
                m->music_id = (nx >= 0 && nx < MIXER_MAX_SOUNDS && m->sound[nx].samples) ? nx : -1;
                m->music_pos = 0.0;
            } else {
                double frac = m->music_pos - p;
                double v = ((double)s->samples[p] * (1.0 - frac) + (double)s->samples[p + 1] * frac) - 128.0;
                sum += (int32_t)(v * 256.0);
                m->music_pos += s->rate / device_rate;
            }
        }
        sum = sum * g / 100;
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        out[i] = (int16_t)sum;
    }
}
