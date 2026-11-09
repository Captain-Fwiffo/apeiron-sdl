/* The Sound Tool's four-channel mixer: sampled sounds (8-bit unsigned mono, ~22254.5 Hz) played on four channels with priorities.
 * A new sound takes the first free channel; if all are busy it replaces the channel with the lowest priority, but only if its own priority is not lower.
 * mixer_mix() produces signed 16-bit mono at any device rate (the caller locks the device around mixer_play, which the game thread calls). */
#ifndef APEIRON_MIXER_H
#define APEIRON_MIXER_H

#include <stdint.h>

#define MIXER_MAX_SOUNDS 2048
#define MIXER_CHANNELS 4

typedef struct {
    uint8_t *samples;
    uint32_t frames;
    double rate;
} MixSound;

typedef struct {
    int id;                    /* sound id of the channel, -1 when free */
    int priority;
    double pos;                /* position in source frames */
} MixChannel;

typedef struct {
    MixSound sound[MIXER_MAX_SOUNDS];
    MixChannel ch[MIXER_CHANNELS];
    int volume;                /* 0..3: Off, Soft, Moderate, Loud */
    int music_id, music_next;  /* the music track (the original's fifth channel): the start sound, then the loop sound forever; -1 when stopped */
    double music_pos;
} Mixer;

void mixer_init(Mixer *m);
void mixer_free(Mixer *m);
int mixer_load(Mixer *m, int id, const uint8_t *samples, uint32_t frames, double rate);
void mixer_play(Mixer *m, int id, int priority);
int mixer_is_playing(const Mixer *m, int id);
void mixer_music_start(Mixer *m, int start_id, int loop_id);   /* seg7_0886 on channel 4 + seg3_336C: start sound, then the loop sound again and again */
void mixer_stop_all(Mixer *m);                                  /* seg7_04B4: every channel and the music */
void mixer_music_stop(Mixer *m);                                /* seg7_07AE(4) */
void mixer_mix(Mixer *m, int16_t *out, int n, int device_rate);

#endif
