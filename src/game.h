/* The running game: the 68k-style memory image the ported logic works on, the sprite classes cut from the user's sprite strips, the three
 * off-screen buffers the engine draws through, and the host side of the calls the original made to the Mac (drawing, sound, palette).
 */
#ifndef APEIRON_GAME_H
#define APEIRON_GAME_H

#include <stdint.h>

#include "creatures.h"
#include "scores.h"
#include "mixer.h"
#include "pict.h"

#define GAME_SCREEN_W 640
#define GAME_SCREEN_H 480
#define GAME_FIELD_W 432
#define GAME_CLASSES 21
#define MEM_SIZE_BYTES 0x1000000u

typedef struct {
    int w, h, frames;
    uint8_t *pixels;                 /* frames * w * h palette indices, 0 = transparent */
} GameClass;

typedef struct Game {
    GameCtx ctx;
    uint8_t *mem;                    /* the 16 MB image */
    GameClass cls[GAME_CLASSES];
    uint8_t palette[256 * 3];
    uint8_t base_palette[256 * 3];   /* the game's colour table (clut 128) as loaded; the colour-rock flash changes `palette` and this restores it */
    uint8_t title_palette[256 * 3];  /* clut 129: the table the coin and flea strips (which carry none) were drawn for */
    int black_index;
    uint32_t screen_addr;            /* 640 x 480 indexed screen buffer inside the image */
    int mouse_button;
    int running;
    uint32_t sound_log[16];          /* sounds requested since the last call to game_take_sounds */
    int n_sounds;
    uint8_t cheat_codes[11][32];  /* STR# 132 of the Apeiron file: the 11 codes as Pascal strings, still ROT13-encoded as stored */
    ScoreTable scores;            /* the high-score file ('ApPr' 128 of Apeiron Scores) as read at start */
    int scores_ok;
    char data_dir[512];
    PictImage pause_pic;          /* Apeiron Titles PICT 3130: the PAUSED picture (seg4_085C draws it while the game is paused) */
    int paused;
    int caps;                     /* Caps Lock is on (the original's pause key: bit 1 of the key map byte at 0x17B) */
    uint8_t digit[11][24 * 32];      /* the digit glyphs of the side panel (0-9 and a blank) */
    uint8_t *icon[7];                /* life, empty, guided, pass-through, machine gun, shield, 6th power-up */
    int icon_w[7], icon_h[7];
    uint8_t *backdrop[11];           /* the level backdrops (pixel patterns 128..138 of Apeiron Titles) */
    int backdrop_w[11], backdrop_h[11];
    Mixer mixer;
    void (*audio_lock)(void *ud, int lock);   /* set by the front end: guards the mixer against the audio thread */
    int (*busy_hook)(struct Game *g, int playing);   /* set by the front end: sees whether a sound is still playing (the audio thread decides, so a recording keeps the answer) and may replace it */
    void *audio_ud;
    char error[256];
} Game;

/* Loads everything from the user's files in `dir`. Returns 0 on success, otherwise a message in g->error. */
int game_init(Game *g, const char *dir);
void game_free(Game *g);
void game_dump_class(const Game *g, int k, const char *path);
void game_dump_class_raw(const Game *g, int k, const char *path);
/* Starts a new game: lives, score, wave 1, 30 mushrooms, the first round. */
void game_new(Game *g);
/* One pass of the game loop with the mouse movement since the last pass. Returns 0 when the loop wants to stop. */
int game_step(Game *g, int dx, int dy, int button, uint32_t ticks);
int game_scores_save(Game *g);                /* writes the table back into the scores file; 0 on success */
void game_key(Game *g, unsigned char ch);     /* a typed character (seg4_0378): only while paused does it do anything, it feeds the code words */
/* Converts the screen buffer to 32-bit pixels (ARGB) for display. */
void game_render(const Game *g, uint32_t *fb);

#endif
