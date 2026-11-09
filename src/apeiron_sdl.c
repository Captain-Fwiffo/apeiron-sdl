/* Apeiron for Windows and Linux: SDL front end (first milestone).
 *
 * Opens a 640x480 window (or a 4:3 multiple of it), reads apeiron.ini, loads the user's original data files, draws the title art with the
 * game's own palettes, runs a fixed-rate loop whose speed comes from the ini, and turns the mouse into the "pending movement" values the
 * game logic expects. The game logic itself (src/creatures.c) is not connected yet.
 *
 *   apeiron [data folder]      default: ./data next to the program
 *   APEIRON_SHOT=file.bmp      writes the first frame to a BMP and exits (for tests; works with SDL_VIDEODRIVER=dummy)
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "font.h"
#include "game.h"
#include "pict.h"
#include "pictdraw.h"
#include "rsrc.h"
#include "snd.h"
#include "title.h"
#include "writers.h"

#define FIELD_W 432          /* the playfield is 27 columns of 16 px */
#define SCREEN_W 640
#define SCREEN_H 480

typedef struct {
    int loop_hz;             /* passes of the game loop per second: 30, 60 or 120 recommended */
    double scale;            /* window size as a multiple of 640x480 (hundredths allowed, e.g. 1.5) */
    int fade_ms;             /* length of palette fades */
    int shots_per_step_only; /* 1: avoid the original's machine-speed dependent shot handling */
    int steady_ticks;        /* 1: Ticks advance by a fixed amount per pass instead of following the clock (see apeiron.ini) */
    double mouse_speed;      /* multiplier on mouse movement (1 = the picture follows the pointer one to one at any scale) */
    int fix_glide;           /* 1: heads that meet head-on at a side wall no longer end up in the same place and slide down the wall together (an original bug) */
    int fix_coin;            /* 1: a coin thrown straight up falls back down instead of staying at the top (an original bug) */
    int pede_log;            /* 1: write apeiron_pede.txt when a head comes down five rows within 40 passes without poison (the "glide") */
    int hitch_log;           /* 1: write apeiron_hitches.txt (frames that took too long, and game time against real time every 5 s) */
    int intro;               /* 1: the start-up intro (the logos and the theme) is shown */
    int record;              /* 1: every game is written to apeiron_replay_<date>_<time>.bin, so it can be played again exactly (APEIRON_REPLAY=file) */
    char data_dir[512];
} Config;

static void config_defaults(Config *c)
{
    c->loop_hz = 60; c->scale = 1.0; c->fade_ms = 400; c->shots_per_step_only = 0; c->steady_ticks = 0; c->intro = 1; c->mouse_speed = 1.0; c->hitch_log = 0; c->fix_coin = 0; c->fix_glide = 0; c->pede_log = 0; c->record = 0;
    snprintf(c->data_dir, sizeof c->data_dir, "data");
}

/* apeiron.ini: key = value lines, ';' or '#' start comments */
static void config_load(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#' || *p == '\n' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*val == ' ' || *val == '\t') val++;
        for (char *e = val + strlen(val); e > val && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'); ) *--e = 0;
        if (!strcmp(key, "loop_hz")) c->loop_hz = atoi(val);
        else if (!strcmp(key, "scale")) c->scale = atof(val);
        else if (!strcmp(key, "fade_ms")) c->fade_ms = atoi(val);
        else if (!strcmp(key, "shots_per_step_only")) c->shots_per_step_only = atoi(val);
        else if (!strcmp(key, "steady_ticks")) c->steady_ticks = atoi(val);
        else if (!strcmp(key, "mouse_speed")) c->mouse_speed = atof(val);
        else if (!strcmp(key, "fix_coin")) c->fix_coin = atoi(val);
        else if (!strcmp(key, "fix_glide")) c->fix_glide = atoi(val);
        else if (!strcmp(key, "pede_log")) c->pede_log = atoi(val);
        else if (!strcmp(key, "hitch_log")) c->hitch_log = atoi(val);
        else if (!strcmp(key, "record")) c->record = atoi(val);
        else if (!strcmp(key, "intro")) c->intro = atoi(val);
        else if (!strcmp(key, "data_dir")) snprintf(c->data_dir, sizeof c->data_dir, "%s", val);
    }
    fclose(f);
    if (c->loop_hz < 10) c->loop_hz = 10;
    if (c->loop_hz > 240) c->loop_hz = 240;
    if (c->mouse_speed < 0.1) c->mouse_speed = 0.1;
    if (c->mouse_speed > 10.0) c->mouse_speed = 10.0;
    if (c->scale < 0.5) c->scale = 0.5;
    if (c->scale > 4.0) c->scale = 4.0;
}

/* a picture from one of the user's files, or NULL */
static int load_pict(const char *dir, const char *file, int id, PictImage *img)
{
    ResFile rf;
    char err[256], msg[256];
    if (rsrc_open(&rf, dir, file, err, sizeof err)) { fprintf(stderr, "%s\n", err); return -1; }
    const Resource *r = rsrc_find(&rf, "PICT", id);
    int rc = -1;
    if (r && pict_decode(r->data, r->size, img, msg, sizeof msg) == PICT_OK && img->indexed) rc = 0;
    else fprintf(stderr, "%s: PICT %d could not be loaded\n", file, id);
    rsrc_close(&rf);
    return rc;
}

/* copies an indexed picture to the 32-bit frame buffer at (x, y) using the picture's own palette */
static void blit_pict(uint32_t *fb, const PictImage *img, int x, int y)
{
    for (int j = 0; j < img->height; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= SCREEN_H) continue;
        for (int i = 0; i < img->width; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= SCREEN_W) continue;
            const uint8_t *c = &img->palette[img->pixels[j * img->width + i] * 3];
            fb[yy * SCREEN_W + xx] = 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        }
    }
}

static SDL_AudioDeviceID g_audio;
static void audio_lock(void *ud, int lock) { (void)ud; if (g_audio) { if (lock) SDL_LockAudioDevice(g_audio); else SDL_UnlockAudioDevice(g_audio); } }
static void audio_callback(void *ud, Uint8 *stream, int len) { Game *gm = (Game *)ud; mixer_mix(&gm->mixer, (int16_t *)stream, len / 2, 22050); }

static int g_grabbed;                              /* the mouse is captured for the game (Ctrl-G lets it go, as in QEMU; a click takes it back) */
static int g_regrab;                               /* the mouse was let go with Ctrl-G and Ctrl-G or a click may take it back */
static int g_test_shot;                            /* test runs (APEIRON_SHOT) keep the mouse free of all this */
static void grab(int on) { g_grabbed = on; SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE); if (!on && !g_regrab) { } }

/* every place that waits for the user polls through this: Ctrl-G lets the mouse go where it is captured (the game and the game-over screens) and takes it back */
static int poll_event(SDL_Event *e)
{
    while (SDL_PollEvent(e)) {
        if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_g && (e->key.keysym.mod & KMOD_CTRL) && !g_test_shot) {
            if (g_grabbed) { grab(0); g_regrab = 1; }
            else if (g_regrab) { grab(1); g_regrab = 0; }
            continue;                                                              /* the key is used up */
        }
        return 1;
    }
    return 0;
}

/* mouse state shared by the game loop and the end-of-round loops (which swallow events of their own): movement not yet handed to the game, and the button as it really is */
static double g_mx, g_my;
static int g_button;
static double g_scale = 1.0;
static double g_speed = 1.0;
static void add_motion(int xrel, int yrel) { g_mx += xrel * g_speed / g_scale; g_my += yrel * g_speed / g_scale; }
static void sync_mouse(void) { g_mx = g_my = 0; g_button = (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0; }   /* a new game starts with the button as it is now */
static void take_motion(int *dx, int *dy) { *dx = (int)g_mx; *dy = (int)g_my; g_mx -= *dx; g_my -= *dy; }

/* ---- replay: the game is deterministic, so what it takes in is all that has to be written down: the random seed at each new game, and for every step the
 * mouse movement, the button and the Ticks (the end-of-round loops are written the same way), plus the keys. apeiron_replay_*.bin holds these (about 10 bytes
 * per step); APEIRON_REPLAY=file plays one again without a window, with the head watcher on.  Records: N seed(4) level(2) | S dx(2) dy(2) flags(1) ticks(4) |
 * W the same inside an end-of-round loop | K key | E the stop key | M a mark (F8) | H hash of the game before a step, every 600 steps | Z hash at the end ---- */
static FILE *g_rec, *g_rep;
static Game *g_rec_game;
static uint32_t g_rec_steps, g_rep_steps, g_rec_marks;
static int g_rep_bad;
static uint8_t g_rb[12];
static void put16(uint8_t **p, int v) { if (v > 32767) v = 32767; if (v < -32768) v = -32768; *(*p)++ = (uint8_t)(v & 255); *(*p)++ = (uint8_t)((v >> 8) & 255); }
static void put32(uint8_t **p, uint32_t v) { for (int i = 0; i < 4; i++) *(*p)++ = (uint8_t)(v >> (8 * i)); }
static int get16(const uint8_t *b) { return (int16_t)(b[0] | b[1] << 8); }
static uint32_t get32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}
/* the game's variables around A5 and its random seed: what a replay is checked against */
static uint32_t sim_hash(Game *g)
{
    uint32_t h = 2166136261u;
    const uint8_t *m = g->mem + g->ctx.a5 - 0x800u;
    for (uint32_t i = 0; i < 0x900u; i++) h = (h ^ m[i]) * 16777619u;
    for (uint32_t i = 0; i < 4; i++) h = (h ^ g->mem[0xA01C2u + i]) * 16777619u;
    return h;
}
static void rec_close(void)
{
    if (!g_rec) return;
    uint8_t b[5], *p = b; *p++ = 'Z'; put32(&p, g_rec_game ? sim_hash(g_rec_game) : 0);
    fwrite(b, 1, (size_t)(p - b), g_rec); fclose(g_rec); g_rec = NULL; g_rec_game = NULL;
}
static void rec_open(Game *g, int fix_coin, int fix_glide)
{
    char name[64]; time_t t = time(NULL);
    strftime(name, sizeof name, "apeiron_replay_%Y%m%d_%H%M%S.bin", localtime(&t));
    g_rec = fopen(name, "wb");
    if (!g_rec) { fprintf(stderr, "the replay file %s could not be created\n", name); return; }
    const uint8_t hd[8] = { 'A', 'P', 'R', 'P', 1, (uint8_t)(fix_coin != 0), (uint8_t)(fix_glide != 0), 0 };
    fwrite(hd, 1, 8, g_rec);
    g_rec_game = g;
    atexit(rec_close);
    fprintf(stderr, "recording to %s\n", name);
}
static void rec_new(Game *g, int level) { if (!g_rec) return; uint8_t b[7], *p = b; *p++ = 'N'; put32(&p, gm_rd32(&g->ctx, 0xA01C2u)); put16(&p, level); fwrite(b, 1, 7, g_rec); }
static void rec_tag(char tag) { if (!g_rec) return; fputc(tag, g_rec); if (tag == 'M') fflush(g_rec); }
static void rec_key(unsigned char ch) { if (!g_rec) return; fputc('K', g_rec); fputc(ch, g_rec); }
static void rec_input(Game *g, char tag, int dx, int dy, int flags, uint32_t ticks)
{
    if (!g_rec) return;
    uint8_t b[10], *p = b;
    if (tag == 'S') {
        if (g_rec_steps % 600 == 0) { *p++ = 'H'; put32(&p, sim_hash(g)); fwrite(b, 1, (size_t)(p - b), g_rec); p = b; }
        if (++g_rec_steps % 60 == 0) fflush(g_rec);
    }
    *p++ = (uint8_t)tag; put16(&p, dx); put16(&p, dy); *p++ = (uint8_t)flags; put32(&p, ticks);
    fwrite(b, 1, (size_t)(p - b), g_rec);
}
static int rep_read(void);
static int rep_side(Game *g, int t);
/* whether a sound is still playing is up to the audio thread, so the recording keeps each answer and a replay (which has no audio) is given them back */
static int busy_hook(Game *g, int playing)
{
    if (g_rec) { fputc('B', g_rec); fputc(playing != 0, g_rec); }
    if (g_rep) {
        int t;
        while ((t = rep_read()) != 0 && rep_side(g, t)) { }
        if (t != 'B') { g->ctx.quit = 1; g_rep_bad = 1; fprintf(stderr, "a sound check was expected at step %u, got record %d\n", g_rep_steps, t); return 0; }
        return g_rb[0];
    }
    return playing;
}
/* the keys, as the game and the recording see them */
static void key_in(Game *g, unsigned char ch) { rec_key(ch); game_key(g, ch); }

static struct { int on, paused, ff; double speed; double hz; uint64_t next; SDL_Window *win; SDL_Renderer *ren; SDL_Texture *tex; uint32_t frames; } V;     /* the viewer, below */
static int rep_read(void)
{
    int t = fgetc(g_rep), n;
    if (t == EOF) return 0;
    n = t == 'N' ? 6 : (t == 'S' || t == 'W') ? 9 : (t == 'K' || t == 'B') ? 1 : (t == 'H' || t == 'Z') ? 4 : (t == 'E' || t == 'M') ? 0 : -1;
    if (n < 0 || (n && fread(g_rb, 1, (size_t)n, g_rep) != (size_t)n)) return 0;
    return t;
}
/* the records that are neither a step nor a wait; returns 1 if it was one of them */
static int rep_side(Game *g, int t)
{
    GameCtx *c = &g->ctx;
    if (t == 'K') game_key(g, g_rb[0]);
    else if (t == 'E') gm_wr8(c, c->a5 - 0x730u, 0);
    else if (t == 'M') { fprintf(stderr, "mark %u: after step %u\n", ++g_rec_marks, g_rep_steps); V.ff = 0; }
    else if (t == 'H') { if (get32(g_rb) != sim_hash(g) && !g_rep_bad) { g_rep_bad = 1; fprintf(stderr, "the replay has drifted from the recorded game before step %u\n", g_rep_steps); } }
    else return 0;
    return 1;
}
/* watching a replay (a recording dropped on the exe, or --replay file): the game is drawn after every step at the loop rate.
 * Space pauses, + and - change the speed, 0 is normal speed, N runs fast to the next mark, Esc or closing the window stops. */
static uint32_t g_vfb[SCREEN_W * SCREEN_H];
static void view_title(const Game *g)
{
    char t[96]; snprintf(t, sizeof t, "Apeiron replay - step %u - %s%s", g_rep_steps, V.ff ? "fast to the next mark" : V.paused ? "paused" : "playing", "");
    if (!V.ff && !V.paused) snprintf(t + strlen(t), sizeof t - strlen(t), " (%.2gx)", V.speed);
    (void)g; SDL_SetWindowTitle(V.win, t);
}
static void view_frame(Game *g)
{
    if (!V.on) return;
    uint64_t freq = SDL_GetPerformanceFrequency();
    SDL_Event e;
    for (;;) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) { g->ctx.quit = 1; return; }
            if (e.type != SDL_KEYDOWN) continue;
            SDL_Keycode k = e.key.keysym.sym;
            if (k == SDLK_SPACE) { V.paused ^= 1; V.ff = 0; }
            else if (k == SDLK_PLUS || k == SDLK_EQUALS || k == SDLK_KP_PLUS) { if (V.speed < 16) V.speed *= 2; V.ff = 0; }
            else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) { if (V.speed > 0.125) V.speed /= 2; V.ff = 0; }
            else if (k == SDLK_0 || k == SDLK_KP_0) { V.speed = 1; V.ff = 0; V.paused = 0; }
            else if (k == SDLK_n) { V.ff = 1; V.paused = 0; }
            V.next = 0; view_title(g);
        }
        if (!V.paused) break;
        SDL_RenderPresent(V.ren);
        SDL_Delay(15);
    }
    if (!V.ff || ++V.frames % 30 == 0) {
        game_render(g, g_vfb);
        SDL_UpdateTexture(V.tex, NULL, g_vfb, SCREEN_W * 4);
        SDL_RenderClear(V.ren); SDL_RenderCopy(V.ren, V.tex, NULL, NULL); SDL_RenderPresent(V.ren);
        if (V.frames % 15 == 0 || !V.ff) { static uint32_t last; if (g_rep_steps != last && g_rep_steps % 20 == 0) { last = g_rep_steps; view_title(g); } }
    }
    if (V.ff) return;
    uint64_t now = SDL_GetPerformanceCounter(), stepc = (uint64_t)((double)freq / (V.hz * V.speed));
    if (!V.next || V.next + freq / 4 < now) V.next = now;
    V.next += stepc;
    if (V.next > now) SDL_Delay((Uint32)((V.next - now) * 1000 / freq));
}
/* what sdl_wait does while replaying: the next wait record stands in for the events, the clock and the mouse */
static void replay_wait(Game *gm)
{
    int t;
    while ((t = rep_read()) != 0 && rep_side(gm, t)) { }
    if (t != 'W') { gm->ctx.quit = 1; if (t) g_rep_bad = 1; fprintf(stderr, "wait: got record %d (%c) at step %u\n", t, t ? t : 63, g_rep_steps); return; }
    gm->caps = g_rb[4] & 1;
    gm_wr16(&gm->ctx, gm->ctx.a5 - 0x41Cu, (uint16_t)(gm_rd16(&gm->ctx, gm->ctx.a5 - 0x41Cu) + get16(g_rb)));
    gm_wr16(&gm->ctx, gm->ctx.a5 - 0x41Au, (uint16_t)(gm_rd16(&gm->ctx, gm->ctx.a5 - 0x41Au) + get16(g_rb + 2)));
    gm_wr32(&gm->ctx, 0x16A, get32(g_rb + 5));
    view_frame(gm);
}

/* The end-of-round routines run loops of their own; this is what they call once per pass: events, one frame on screen, pacing at the loop rate, new Ticks. */
static struct { SDL_Renderer *ren; SDL_Texture *tex; uint32_t *fb; Game *game; uint64_t start, freq, next; double step; int shot; uint32_t passes; int loop_hz; int steady; } W;

static void present_frame(void)
{
    game_render(W.game, W.fb);
    if (getenv("APEIRON_PAUSESHOT") && W.game->paused) {                       /* test: a picture of the paused game, once */
        static int done;
        if (!done) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(W.fb, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
            if (s) { SDL_SaveBMP(s, getenv("APEIRON_PAUSESHOT")); SDL_FreeSurface(s); }
            done = 1;
        }
    }
    if (getenv("APEIRON_SLOWDRAW")) SDL_Delay((Uint32)atoi(getenv("APEIRON_SLOWDRAW")));     /* test: a drawing that takes this many ms */
    SDL_UpdateTexture(W.tex, NULL, W.fb, SCREEN_W * 4);
    SDL_RenderClear(W.ren);
    SDL_RenderCopy(W.ren, W.tex, NULL, NULL);
    SDL_RenderPresent(W.ren);
}

static void sdl_wait(void *ud)
{
    Game *gm = (Game *)ud;
    if (g_rep) { replay_wait(gm); return; }
    uint64_t t_in = SDL_GetPerformanceCounter();
    SDL_Event e;
    int wdx, wdy;
    while (poll_event(&e)) {
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && !g_grabbed && g_regrab && !W.shot) { grab(1); g_regrab = 0; }
        else if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) gm->ctx.quit = 1;
        else if (e.type == SDL_TEXTINPUT) for (const char *t = e.text.text; *t; t++) key_in(gm, (unsigned char)*t);
        else if (e.type == SDL_MOUSEMOTION) { if (g_grabbed || W.shot || !g_regrab) add_motion(e.motion.xrel, e.motion.yrel); }
        else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) g_button = 1;
        else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) g_button = 0;
    }
    {   /* the shooter can be moved between rounds (it cannot shoot there: the loops never fire) */
        take_motion(&wdx, &wdy);
        gm_wr16(&gm->ctx, gm->ctx.a5 - 0x41Cu, (uint16_t)(gm_rd16(&gm->ctx, gm->ctx.a5 - 0x41Cu) + wdx));
        gm_wr16(&gm->ctx, gm->ctx.a5 - 0x41Au, (uint16_t)(gm_rd16(&gm->ctx, gm->ctx.a5 - 0x41Au) + wdy));
    }
    gm->caps = (SDL_GetModState() & KMOD_CAPS) != 0;
    if (getenv("APEIRON_PAUSETEST")) gm->caps = W.passes >= 50 && W.passes < 150;     /* test: Caps Lock held for 100 passes */
    if (getenv("APEIRON_TYPE") && W.passes == 100) {            /* test: type this text while paused (with APEIRON_PAUSETEST) and report the lives */
        for (const char *t = getenv("APEIRON_TYPE"); *t; t++) game_key(gm, (unsigned char)*t);
        for (int i = 0; i < 9; i++) fprintf(stderr, "%c", gm_rd8(&gm->ctx, gm->ctx.a5 - 0x46Cu + (unsigned)i));
        fprintf(stderr, " <- history\n");
        fprintf(stderr, "mode %d code0 len %d first %c; typed '%s': lives %d, cheat flag %d\n", gm_rd8(&gm->ctx, gm->ctx.a5 - 0x45Fu), gm->cheat_codes[0][0], gm->cheat_codes[0][1], getenv("APEIRON_TYPE"), (int)gm_rd16(&gm->ctx, gm->ctx.a5 - 0x6FAu), gm_rd8(&gm->ctx, gm->ctx.a5 - 0x72Au));
    }
    present_frame();
    uint64_t now = SDL_GetPerformanceCounter();
    uint32_t n = 1;                                               /* passes this call stands for */
    if (!W.shot && W.steady) {
        /* steady Ticks follow the pass count, so when drawing a frame takes longer than a step (a big window, software scaling, a slow monitor) the passes that fell due
         * meanwhile are counted here, as the main loop does by running them; otherwise the end-of-round loops would run slow in step with the frame rate */
        uint64_t stepc = (uint64_t)(W.step * (double)W.freq);
        if (W.next == 0) W.next = t_in;                           /* the first call of a sequence (the main loop clears it before each game step) */
        W.next += stepc;
        if (W.next <= now) {
            uint64_t k = (now - W.next) / stepc;
            if (k > 8) { k = 8; W.next = now; } else W.next += k * stepc;
            n += (uint32_t)k;
        } else SDL_Delay((Uint32)((W.next - now) * 1000 / W.freq));
        now = SDL_GetPerformanceCounter();
    } else if (!W.shot) {
        if (W.next < now) W.next = now;                           /* fell behind: do not try to catch up */
        W.next += (uint64_t)(W.step * (double)W.freq);
        now = SDL_GetPerformanceCounter();
        if (W.next > now) SDL_Delay((Uint32)((W.next - now) * 1000 / W.freq));
        now = SDL_GetPerformanceCounter();
    }
    uint32_t ticks = 36000u + (W.shot || W.steady ? (uint32_t)((W.passes + n - 1) * 60.15 / W.loop_hz) : (uint32_t)((double)(now - W.start) / (double)W.freq * 60.15));
    W.passes += n;
    gm_wr32(&gm->ctx, 0x16A, ticks);
    rec_input(gm, 'W', wdx, wdy, gm->caps, ticks);
    if (getenv("APEIRON_WAITDBG") && (W.passes / 30) != ((W.passes - n) / 30)) fprintf(stderr, "wait: t=%.2f s passes %u ticks %u\n", (double)(now - W.start) / (double)W.freq, W.passes, ticks);
}

/* ---- the game-over sequence (seg3_16B4) ----
 * The original frees the sprites, blanks the palette, plays "Hooka Music" and shows picture 888 (the caterpillar on its mushroom) centred in the window,
 * fading the palette up to colour table 888; after a second the eight pictures 900-907 follow, one every 8 Ticks, drawn over it with their top left at
 * (225, 99) in the window; it waits for the music to end, then looks where the score goes in the table (the first stored score it beats) and plays a
 * sound (a cheated game never makes the table). Here the fade is done on the RGB picture and lasts fade_ms. */
static int go_skip;                                  /* Esc or the mouse button was pressed while the game-over sequence plays */
static uint32_t go_skip_test_at;                     /* test: the time (SDL ticks) at which Esc is pretended to be pressed; 0 = never */
static uint32_t go_img[SCREEN_W * SCREEN_H];
static int go_level = 256;                       /* 0..256: how far the fade has come */

static void go_present(void)
{
    static uint32_t out[SCREEN_W * SCREEN_H];
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint32_t p = go_img[i];
        out[i] = 0xFF000000u | ((((p >> 16) & 255u) * (uint32_t)go_level / 256u) << 16) | ((((p >> 8) & 255u) * (uint32_t)go_level / 256u) << 8) | (((p & 255u) * (uint32_t)go_level) / 256u);
    }
    SDL_UpdateTexture(W.tex, NULL, out, SCREEN_W * 4);
    SDL_RenderClear(W.ren);
    SDL_RenderCopy(W.ren, W.tex, NULL, NULL);
    SDL_RenderPresent(W.ren);
}

/* shows the picture for ms milliseconds; 0 if the user closed the window or pressed Esc, *click set when the mouse button went down */
static int go_wait(int ms, int *click)
{
    uint64_t end = SDL_GetPerformanceCounter() + (uint64_t)ms * W.freq / 1000u;
    do {
        SDL_Event e;
        if (go_skip_test_at && SDL_GetTicks() >= go_skip_test_at) go_skip = 1;
        while (poll_event(&e)) {
            if (e.type == SDL_QUIT) return 0;
            if ((e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) || (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)) go_skip = 1;
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && click) *click = 1;
        }
        go_present();
        if (SDL_GetPerformanceCounter() < end) SDL_Delay(4);
    } while (SDL_GetPerformanceCounter() < end);
    return 1;
}

static int go_ticks(int t) { return (int)(t * 1000.0 / 60.15); }

static void go_sound(Game *g, int id, int prio) { audio_lock(NULL, 1); mixer_play(&g->mixer, id, prio); audio_lock(NULL, 0); }

/* ---- palette effects ----
 * The transition (seg2_0838, called by seg2_0838's 10 callers: starting a game, game over, the instructions): sound 128, the picture turns grey (seg6_0B00(7, 1)
 * with the grey 0x7FFF: every colour becomes its lightness), 5 Ticks later it fades to white in 30 steps and then to black in 30 steps (seg6_07E2, one palette
 * refresh per step). The fade-ins (seg6_0974 to the new screen's table: 20 steps for the title, the game and the instructions, 60 for the caterpillar).
 * Here the effects are done on the RGB picture; a 30-step fade lasts fade_ms (apeiron.ini), the others in proportion. */

static void fx_show(const uint32_t *img)
{
    SDL_UpdateTexture(W.tex, NULL, img, SCREEN_W * 4);
    SDL_RenderClear(W.ren);
    SDL_RenderCopy(W.ren, W.tex, NULL, NULL);
    SDL_RenderPresent(W.ren);
}

static int fx_on(const Config *cfg) { return cfg->fade_ms > 0 && !getenv("APEIRON_NOFADE"); }
static double fx_secs(const Config *cfg, int steps) { return (double)steps * (double)cfg->fade_ms / 30.0 / 1000.0; }

/* the window events during an effect: 0 if the window was closed; Esc or the mouse button sets go_skip */
static int fx_events(void)
{
    SDL_Event e;
    if (go_skip_test_at && SDL_GetTicks() >= go_skip_test_at) go_skip = 1;
    while (poll_event(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if ((e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) || (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)) go_skip = 1;
    }
    return 1;
}

static uint32_t fx_scale(uint32_t p, uint32_t level)                               /* level 0..256 */
{
    return 0xFF000000u | ((((p >> 16) & 255u) * level / 256u) << 16) | ((((p >> 8) & 255u) * level / 256u) << 8) | ((p & 255u) * level / 256u);
}

static uint32_t fx_mix(uint32_t a, uint32_t b, uint32_t t)                          /* a towards b, t 0..256 */
{
    uint32_t r = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        int va = (int)((a >> sh) & 255u), vb = (int)((b >> sh) & 255u);
        r |= (uint32_t)(va + (vb - va) * (int)t / 256) << sh;
    }
    return r;
}

/* a fade up from black to img; returns 0 if the window was closed. skippable: Esc or the mouse button ends it at once */
static int fade_in(const Config *cfg, const uint32_t *img, int steps, int skippable)
{
    static uint32_t out[SCREEN_W * SCREEN_H];
    if (!fx_on(cfg)) { fx_show(img); return fx_events(); }
    double dur = fx_secs(cfg, steps);
    uint64_t t0 = SDL_GetPerformanceCounter();
    for (;;) {
        if (!fx_events()) return 0;
        double el = (double)(SDL_GetPerformanceCounter() - t0) / (double)W.freq;
        if (el >= dur || (skippable && go_skip)) break;
        uint32_t level = (uint32_t)(el / dur * 256.0);
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) out[i] = fx_scale(img[i], level);
        fx_show(out);
        SDL_Delay(2);
    }
    fx_show(img);
    return 1;
}

/* the transition away from src; returns 0 if the window was closed */
static int transition_out(Game *g, const Config *cfg, const uint32_t *src)
{
    static uint32_t grey[SCREEN_W * SCREEN_H], out[SCREEN_W * SCREEN_H];
    go_sound(g, 128, 3);
    if (!fx_on(cfg)) return fx_events();
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint32_t p = src[i];
        unsigned r = (p >> 16) & 255u, gg = (p >> 8) & 255u, b = p & 255u;
        unsigned mx = r > gg ? (r > b ? r : b) : (gg > b ? gg : b), mn = r < gg ? (r < b ? r : b) : (gg < b ? gg : b), l = (mx + mn) / 2;
        grey[i] = 0xFF000000u | l << 16 | l << 8 | l;
    }
    fx_show(grey);
    if (getenv("APEIRON_FXSHOT")) {                                                      /* test: a picture of the grey screen of the first transition */
        static int once;
        if (!once) { once = 1; SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormatFrom(grey, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888); if (sf) { SDL_SaveBMP(sf, getenv("APEIRON_FXSHOT")); SDL_FreeSurface(sf); } }
    }
    uint64_t t0 = SDL_GetPerformanceCounter();
    while ((double)(SDL_GetPerformanceCounter() - t0) / (double)W.freq * 60.15 < 5.0) { if (!fx_events()) return 0; SDL_Delay(2); }      /* 5 Ticks of grey */
    for (int phase = 0; phase < 2; phase++) {                                            /* to white, then to black */
        uint32_t from = phase == 0 ? 0u : 0xFFFFFFFFu, to = phase == 0 ? 0xFFFFFFFFu : 0xFF000000u;
        double dur = fx_secs(cfg, 30);
        t0 = SDL_GetPerformanceCounter();
        for (;;) {
            if (!fx_events()) return 0;
            double el = (double)(SDL_GetPerformanceCounter() - t0) / (double)W.freq;
            if (el >= dur) break;
            uint32_t t = (uint32_t)(el / dur * 256.0);
            for (int i = 0; i < SCREEN_W * SCREEN_H; i++) out[i] = fx_mix(phase == 0 ? grey[i] : 0xFFFFFFFFu, to, t);
            (void)from;
            fx_show(out);
            SDL_Delay(2);
        }
    }
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) out[i] = 0xFF000000u;
    fx_show(out);
    t0 = SDL_GetPerformanceCounter();
    while (mixer_is_playing(&g->mixer, 128) && (double)(SDL_GetPerformanceCounter() - t0) / (double)W.freq < 3.0) { if (!fx_events()) return 0; SDL_Delay(5); }   /* the sound is let finish */
    return 1;
}

/* ---- the start-up intro (seg2_049C) ----
 * First the Ambrosia logo (Apeiron Titles PICT 999, centred) fades up from black (30 steps) while snd 144 plays; when the sound ends a transition (grey, white,
 * black) leads to PICT 889, the Apeiron logo with a fractal fill, which fades up (30 steps) after snd 143, the intro theme, has started and half a second has
 * passed; then the palette is turned round one place every 3 Ticks (entries 1 to 246 move down by one, entry 1 goes to 246: the colours swirl) for up to 180
 * turns, until the theme ends or the mouse button is pressed; then another transition. Esc or the mouse button ends either part here. */
static void intro_paint(const PictImage *pic, const uint8_t *pal, int x0, int y0)
{
    for (int y = 0; y < pic->height; y++)
        for (int x = 0; x < pic->width; x++) {
            int xx = x0 + x, yy = y0 + y;
            if (xx < 0 || yy < 0 || xx >= SCREEN_W || yy >= SCREEN_H) continue;
            const uint8_t *c = &pal[pic->pixels[y * pic->width + x] * 3];
            go_img[yy * SCREEN_W + xx] = 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        }
}

/* returns 0 if the window was closed */
static void intro_shot(const char *suffix)                                         /* test: a picture of the intro, APEIRON_INTROSHOT=prefix */
{
    if (!getenv("APEIRON_INTROSHOT")) return;
    char path[512];
    snprintf(path, sizeof path, "%s%s.bmp", getenv("APEIRON_INTROSHOT"), suffix);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(go_img, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
    if (s) { SDL_SaveBMP(s, path); SDL_FreeSurface(s); }
}

/* the colour table of the given id from Apeiron Titles (what the original sets the screen palette to for that picture); 0 if there is none */
static int titles_clut(const Config *cfg, int id, uint8_t pal[256 * 3])
{
    ResFile rf; char err[256];
    if (rsrc_open(&rf, cfg->data_dir, "Apeiron Titles", err, sizeof err)) return 0;
    const Resource *r = rsrc_find(&rf, "clut", id);
    int ok = r && clut_decode(r->data, r->size, pal) > 0;
    rsrc_close(&rf);
    return ok;
}

static int intro_screens(Game *g, const Config *cfg)
{
    PictImage logo, fract;
    int alive = 1;
    go_skip = 0; go_skip_test_at = 0;
    go_level = 256;
    if (load_pict(cfg->data_dir, "Apeiron Titles", 999, &logo) == 0) {
        memset(go_img, 0, sizeof go_img);
        intro_paint(&logo, logo.palette, (SCREEN_W - logo.width) / 2, (SCREEN_H - logo.height) / 2);
        go_sound(g, 144, 3);
        alive = fade_in(cfg, go_img, 30, 1);
        intro_shot("_logo");
        int click = 0;
        while (alive && !go_skip && mixer_is_playing(&g->mixer, 144)) alive = go_wait(20, &click);
        pict_free(&logo);
        if (alive) { go_present(); alive = transition_out(g, cfg, go_img); }
    }
    if (!alive) return 0;
    go_skip = 0;
    if (load_pict(cfg->data_dir, "Apeiron Titles", 889, &fract) == 0) {
        uint8_t pal[256 * 3];
        memcpy(pal, fract.palette, sizeof pal);
        if (titles_clut(cfg, 889, pal)) {   /* the original draws the picture on a screen whose palette is colour table 889: each colour of the picture lands on the entry of that table that holds it (all 150 do, exactly) */
            uint8_t map[256];
            for (int i = 0; i < 256; i++) {
                int best = 0, bd = 1 << 30;
                for (int j = 0; j < 256; j++) {
                    int dr = fract.palette[i * 3] - pal[j * 3], dg = fract.palette[i * 3 + 1] - pal[j * 3 + 1], db = fract.palette[i * 3 + 2] - pal[j * 3 + 2], d = dr * dr + dg * dg + db * db;
                    if (d < bd) { bd = d; best = j; }
                }
                map[i] = (uint8_t)best;
            }
            for (int i = 0; i < fract.width * fract.height; i++) fract.pixels[i] = map[fract.pixels[i]];
        }
        int x0 = (SCREEN_W - fract.width) / 2, y0 = (SCREEN_H - fract.height) / 2, click = 0;
        memset(go_img, 0, sizeof go_img);
        intro_paint(&fract, pal, x0, y0);
        go_sound(g, 143, 3);
        alive = go_wait(go_ticks(30), &click);
        if (alive) alive = fade_in(cfg, go_img, 30, 1);
        for (int i = 0; alive && !go_skip && i < 180; i++) {
            uint64_t t0 = SDL_GetPerformanceCounter();
            uint8_t first[3] = { pal[3], pal[4], pal[5] };
            for (int k = 1; k < 246; k++) memcpy(&pal[k * 3], &pal[(k + 1) * 3], 3);          /* entry k takes the colour of entry k + 1 */
            memcpy(&pal[246 * 3], first, 3);
            intro_paint(&fract, pal, x0, y0);
            go_present();
            if (i == 0) intro_shot("_fract0");
            if (i == 40) intro_shot("_fract40");
            while (alive && !go_skip && (double)(SDL_GetPerformanceCounter() - t0) / (double)W.freq * 60.15 < 3.0) alive = go_wait(2, &click);   /* 3 Ticks a turn */
            if (!mixer_is_playing(&g->mixer, 143)) break;                                      /* the theme is over */
        }
        pict_free(&fract);
        if (go_skip) { audio_lock(NULL, 1); mixer_stop_all(&g->mixer); audio_lock(NULL, 0); }
        if (alive) { go_present(); alive = transition_out(g, cfg, go_img); }
    }
    return alive;
}

/* The name dialog (seg8_016A) was a Mac dialog box with an edit field holding the last name; here the field is drawn under the picture with the built-in font.
 * Typing replaces the name (it starts selected), Backspace deletes, Return or Esc accepts what is shown. Returns 0 if the window was closed meanwhile. */
static int go_enter_name(char *name, int place, int auto_mode)
{
    char buf[40];
    snprintf(buf, sizeof buf, "%s", name);
    int selected = 1, done = 0, alive = 1;
    if (auto_mode && getenv("APEIRON_NAME")) { snprintf(buf, sizeof buf, "%s", getenv("APEIRON_NAME")); selected = 0; }
    SDL_StartTextInput();
    uint64_t t0 = SDL_GetPerformanceCounter();
    do {
        SDL_Event e;
        while (!auto_mode && poll_event(&e)) {
            if (e.type == SDL_QUIT) { alive = 0; done = 1; }
            else if (e.type == SDL_TEXTINPUT) {
                for (const char *t = e.text.text; *t; t++) {
                    if ((unsigned char)*t < 32 || (unsigned char)*t > 126) continue;
                    if (selected) { buf[0] = 0; selected = 0; }
                    size_t n = strlen(buf);
                    if (n < 31) { buf[n] = *t; buf[n + 1] = 0; }
                }
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_BACKSPACE) { if (selected) { buf[0] = 0; selected = 0; } else if (buf[0]) buf[strlen(buf) - 1] = 0; }
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE) done = 1;      /* Return or Esc accepts the name (the original had only OK) */
            }
        }
        for (int y = 396; y < SCREEN_H; y++) for (int x = 0; x < SCREEN_W; x++) go_img[y * SCREEN_W + x] = 0xFF000000u;
        (void)place;
        const char *l1 = "Congratulations, you got a high score!!!", *l2 = "Enter your name for the hall of fame:";     /* the original's wording (DITL 161) */
        ffont_draw(go_img, SCREEN_W, SCREEN_H, (SCREEN_W - ffont_width(&font_sans9b, l1)) / 2, 408, &font_sans9b, 0xFFFFFFFFu, l1);
        ffont_draw(go_img, SCREEN_W, SCREEN_H, (SCREEN_W - ffont_width(&font_sans9b, l2)) / 2, 422, &font_sans9b, 0xFFFFFFFFu, l2);
        const int fl = (SCREEN_W - 257) / 2, ft = 430, fr = fl + 257, fbm = ft + 16;                                         /* the edit field: 257 x 16 as in the DITL */
        for (int yy = ft; yy < fbm; yy++) for (int xx = fl; xx < fr; xx++) go_img[yy * SCREEN_W + xx] = (yy == ft || yy == fbm - 1 || xx == fl || xx == fr - 1) ? 0xFF808080u : 0xFFFFFFFFu;
        int tw = ffont_width(&font_sans9b, buf);
        if (selected && tw) for (int yy = ft + 3; yy < fbm - 3; yy++) for (int xx = fl + 3; xx < fl + 5 + tw; xx++) go_img[yy * SCREEN_W + xx] = 0xFF000000u;
        ffont_draw(go_img, SCREEN_W, SCREEN_H, fl + 4, ft + 12, &font_sans9b, selected ? 0xFFFFFFFFu : 0xFF000000u, buf);
        if (!selected && ((SDL_GetPerformanceCounter() - t0) * 2 / W.freq) % 2 == 0) ffont_draw(go_img, SCREEN_W, SCREEN_H, fl + 4 + tw, ft + 12, &font_sans9b, 0xFF000000u, "_");
        ffont_draw(go_img, SCREEN_W, SCREEN_H, (SCREEN_W - ffont_width(&font_sans9, "Return = OK")) / 2, 466, &font_sans9, 0xFF909090u, "Return = OK");
        go_present();
        if (auto_mode) done = 1; else SDL_Delay(10);
    } while (!done);
    SDL_StopTextInput();
    if (getenv("APEIRON_GOSHOT2")) {
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(go_img, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
        if (s) { SDL_SaveBMP(s, getenv("APEIRON_GOSHOT2")); SDL_FreeSurface(s); }
    }
    snprintf(name, 256, "%s", buf);
    return alive;
}

/* returns 1 to play again, 0 to quit */
static int game_over_flow(Game *g, const Config *cfg, int auto_mode, int *place)
{
    *place = -1;
    GameCtx *c = &g->ctx;
    int alive = 1;
    go_skip = 0;
    go_skip_test_at = getenv("APEIRON_GOSKIP") ? SDL_GetTicks() + (uint32_t)atoi(getenv("APEIRON_GOSKIP")) : 0;
    game_render(g, W.fb);                                                 /* the game as it stands, for the transition */
    if (!transition_out(g, cfg, W.fb)) return 0;
    audio_lock(NULL, 1); mixer_stop_all(&g->mixer); audio_lock(NULL, 0);
    memset(go_img, 0, sizeof go_img);
    go_level = 256;
    PictImage pic;
    if (load_pict(cfg->data_dir, "Apeiron Titles", 888, &pic) == 0) {
        blit_pict(go_img, &pic, (SCREEN_W - pic.width) / 2, (SCREEN_H - pic.height) / 2);
        pict_free(&pic);
    }
    go_sound(g, 150, 3);
    int click = 0;
    alive = fade_in(cfg, go_img, 60, 1);                                  /* the palette comes up from black (60 steps) */
    if (alive && !go_skip) alive = go_wait(go_ticks(60), &click);
    for (int i = 0; alive && !go_skip && i < 8; i++) {
        alive = go_wait(go_ticks(8), &click);
        if (load_pict(cfg->data_dir, "Apeiron Titles", 900 + i, &pic) == 0) { blit_pict(go_img, &pic, 225, 99); pict_free(&pic); }
    }
    while (alive && !go_skip && mixer_is_playing(&g->mixer, 150)) alive = go_wait(20, &click);
    if (go_skip) { audio_lock(NULL, 1); mixer_stop_all(&g->mixer); audio_lock(NULL, 0); }       /* the mouse button or Esc: the sounds stop and it goes on to the score */
    if (getenv("APEIRON_GOSHOT")) {                                      /* test: the finished picture */
        go_present();
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(go_img, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
        if (s) { SDL_SaveBMP(s, getenv("APEIRON_GOSHOT")); SDL_FreeSurface(s); }
    }
    if (!alive) return 0;
    int32_t score = (int32_t)gm_rd32(c, c->a5 - 0x702u);
    if (!alive) return 0;
    int pos = g->scores_ok ? scores_position(&g->scores, score) : -1;
    if (pos >= 0 && !gm_rd8(c, c->a5 - 0x72Au)) {
        go_sound(g, (int)gm_rd32(c, c->a5 - 0x560u), 3);
        char name[256];
        snprintf(name, sizeof name, "%s", g->scores.last_name);
        alive = go_enter_name(name, pos + 1, auto_mode);
        if (!alive) return 0;                                           /* the window was closed: the score is not saved (the original saved only after OK) */
        scores_insert(&g->scores, pos, name, score, (int16_t)gm_rd16(c, c->a5 - 0x734u));
        *place = pos;
        if (game_scores_save(g)) fprintf(stderr, "the scores file could not be written\n");
        if (getenv("APEIRON_DEBUG")) fprintf(stderr, "score %d goes to place %d\n", (int)score, pos + 1);
    } else {
        go_sound(g, (int)gm_rd32(c, c->a5 - 0x548u), 3);
    }
    if (alive) { go_present(); alive = transition_out(g, cfg, go_img); }       /* away from the caterpillar (the title fades in) */
    return alive;
}

/* ---- the title screen: the buttons and keys of seg4_1114 / seg4_1522 / seg4_0378 ---- */
static uint32_t title_fb[TITLE_W * TITLE_H];

static void title_present(const TitleRes *tr, const ScoreTable *s, int highlight, int pressed)
{
    title_draw(title_fb, tr, s, highlight, pressed);
    SDL_UpdateTexture(W.tex, NULL, title_fb, SCREEN_W * 4);
    SDL_RenderClear(W.ren);
    SDL_RenderCopy(W.ren, W.tex, NULL, NULL);
    SDL_RenderPresent(W.ren);
}

/* seg4_14A6: the music flag and the volume from the table */
static void apply_settings(Game *g)
{
    gm_wr8(&g->ctx, g->ctx.a5 - 0x708u, g->scores.music != 0);
    g->mixer.volume = g->scores.volume & 3;
}

/* ---- a Mac-style dialog box drawn over the title (the original's were DLOG / DITL resources: white box, black frame, push buttons) ---- */
static const TitleRes *g_tr;                       /* what the title is drawn from, for the dialogs that sit on top of it */
static int g_hl = -1;
static SDL_Cursor *g_hand;                         /* the pointing hand of the title (Apeiron 'crsr' 128) */
static const Config *g_cfg;                        /* for the screens that read more of the user's files */

static void box_fill(uint32_t *fb, int l, int t, int r, int b, uint32_t c)
{
    for (int y = t; y < b; y++) for (int x = l; x < r; x++) if (x >= 0 && y >= 0 && x < TITLE_W && y < TITLE_H) fb[y * TITLE_W + x] = c;
}

static void box_frame(uint32_t *fb, int l, int t, int r, int b, uint32_t c)
{
    box_fill(fb, l, t, r, t + 1, c); box_fill(fb, l, b - 1, r, b, c); box_fill(fb, l, t, l + 1, b, c); box_fill(fb, r - 1, t, r, b, c);
}

/* the dialog window: white, framed in black with a shadow */
static void box_window(uint32_t *fb, int l, int t, int r, int b)
{
    box_fill(fb, l + 3, t + 3, r + 3, b + 3, 0xFF000000u);
    box_fill(fb, l, t, r, b, 0xFFFFFFFFu);
    box_frame(fb, l, t, r, b, 0xFF000000u);
    box_frame(fb, l + 2, t + 2, r - 2, b - 2, 0xFF000000u);
}

static void box_button(uint32_t *fb, int l, int t, int r, int b, const char *label, int is_default, int pressed)
{
    uint32_t ink = pressed ? 0xFFFFFFFFu : 0xFF000000u;
    box_fill(fb, l, t, r, b, pressed ? 0xFF000000u : 0xFFFFFFFFu);
    box_frame(fb, l, t, r, b, 0xFF000000u);
    if (is_default) box_frame(fb, l - 3, t - 3, r + 3, b + 3, 0xFF000000u);
    ffont_draw(fb, TITLE_W, TITLE_H, l + ((r - l) - ffont_width(&font_sans9b, label)) / 2, t + ((b - t) + 7) / 2, &font_sans9b, ink, label);
}

/* text wrapped at the words, 10 px per line */
static void box_text(uint32_t *fb, int x, int y, int width, const char *s, uint32_t color)
{
    char line[128];
    int per = width / 6, n = 0;
    while (*s) {
        int len = (int)strlen(s), cut = len > per ? per : len;
        if (cut < len) { while (cut > 0 && s[cut] != ' ') cut--; if (cut == 0) cut = per; }
        n = cut < (int)sizeof line - 1 ? cut : (int)sizeof line - 1;
        memcpy(line, s, (size_t)n); line[n] = 0;
        ffont_draw(fb, TITLE_W, TITLE_H, x, y + 8, &font_sans9b, color, line);
        y += 12; s += cut; while (*s == ' ') s++;
    }
}

/* "Are you sure you want to clear the high scores?" (DLOG 160 at 114, 79; the buttons Cancel, the default, and Clear). Returns 1 for Clear, 0 for Cancel, -1 if the window was closed. */
static int modal_clear(Game *g)
{
    GameCtx *c = &g->ctx;
    const int dl = 114, dt = 79, dr = 400, db = 165;
    go_sound(g, 145, 3);
    int down = -1, result = -2;
    while (result == -2) {
        SDL_Event e;
        while (poll_event(&e) && result == -2) {
            if (e.type == SDL_QUIT) result = -1;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE) result = 0;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int x = e.button.x - dl, y = e.button.y - dt;
                down = (y >= 62 && y < 82 && x >= 218 && x < 282) ? 0 : (y >= 62 && y < 82 && x >= 141 && x < 205) ? 1 : -1;
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && down >= 0) {
                int x = e.button.x - dl, y = e.button.y - dt;
                int up = (y >= 62 && y < 82 && x >= 218 && x < 282) ? 0 : (y >= 62 && y < 82 && x >= 141 && x < 205) ? 1 : -1;
                if (up == down) result = down;
                down = -1;
            }
        }
        title_draw(title_fb, g_tr, &g->scores, g_hl, -1);
        box_window(title_fb, dl, dt, dr, db);
        if (g_tr->icon[0].argb) { /* the caterpillar's face (icon 128 in the original) is not loaded here: the dialog goes without it */ }
        box_text(title_fb, dl + 57, dt + 14, 229, "Are you sure you want to clear the high scores?", 0xFF000000u);
        box_button(title_fb, dl + 218, dt + 62, dl + 282, dt + 82, "Cancel", 1, down == 0);
        box_button(title_fb, dl + 141, dt + 62, dl + 205, dt + 82, "Clear", 0, down == 1);
        SDL_UpdateTexture(W.tex, NULL, title_fb, SCREEN_W * 4);
        SDL_RenderClear(W.ren); SDL_RenderCopy(W.ren, W.tex, NULL, NULL); SDL_RenderPresent(W.ren);
        if (getenv("APEIRON_MODALTEST")) {                              /* test: a picture of the dialog, then the answer given in the variable (1 clear, 0 cancel) */
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(title_fb, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
            if (s && getenv("APEIRON_DLGSHOT")) { SDL_SaveBMP(s, getenv("APEIRON_DLGSHOT")); }
            if (s) SDL_FreeSurface(s);
            result = atoi(getenv("APEIRON_MODALTEST"));
        }
        SDL_Delay(8);
    }
    (void)c;
    while (result >= 0 && mixer_is_playing(&g->mixer, 145)) SDL_Delay(10);        /* the sound is let finish, as the original does */
    return result;
}

/* seg8_05CC after the dialog: every name back to the first string of STR# 131 ("Hector"), score 0, wave 1, and the file written */
static void clear_scores(Game *g)
{
    ResFile rf;
    char err[256], name[64] = "Hector";
    if (!rsrc_open(&rf, g->data_dir, "Apeiron", err, sizeof err)) {
        const Resource *r = rsrc_find(&rf, "STR#", 131);
        if (r && r->size > 3 && r->data[2] < 60 && 3u + r->data[2] <= r->size) { memcpy(name, r->data + 3, r->data[2]); name[r->data[2]] = 0; }
        rsrc_close(&rf);
    }
    scores_clear(&g->scores, name);
    if (g->scores_ok && game_scores_save(g)) fprintf(stderr, "the scores file could not be written\n");
}

/* ---- Instructions (seg8_06FE starts it, seg8_0792 turns the page, seg8_0AF0 and seg8_0B5A make the little demonstration sprites) ----
 * Three pages (Apeiron Titles PICT 4000-4002): the text drawn by the picture itself, with demonstration sprites beside it that cycle their frames every
 * 2 Ticks; on the last page coins bounce around at the left edge. A click or a key turns the page; after the third the title returns. */
typedef struct { int cls, x, y, frame, cb; double vx, vy; int bounce; } Demo;

/* frame callbacks of the demonstration sprites (jump table 0x452, 0x45a, 0x462, 0x46a, 0x472, 0x47a) */
static void demo_frame(Demo *d, const GameClass *c)
{
    switch (d->cb) {
    case 0x452: if (++d->frame > c->frames) d->frame = 1; break;                 /* seg8_0C4A */
    case 0x45A: if (++d->frame > 8) d->frame = 1; break;                         /* seg8_0C22 */
    case 0x462: break;                                                           /* seg8_0C42: stays */
    case 0x46A: d->frame += 4; if (d->frame > c->frames) d->frame = 5; break;    /* seg8_0C70 */
    case 0x472: if (++d->frame > 14) d->frame = 1; break;                        /* seg8_0C96 */
    case 0x47A: if (++d->frame > 45) d->frame = 32; break;                       /* seg8_0CB6 */
    }
}

static void demo_coin(Demo *d, const GameClass *c)                              /* seg8_0B5A */
{
    d->cls = 14; d->cb = 0x452; d->bounce = 1;
    d->vx = (double)((rand() % 70) << 12) / 65536.0 * ((rand() % 2) ? 1 : -1);
    d->vy = -(double)((10 + rand() % 20) << 15) / 65536.0;
    d->frame = rand() % c->frames + 1;
    d->x = rand() % 48; d->y = rand() % 300;
}

/* seg8_0CDE: gravity of one pixel per step, bounces off the floor losing a pixel of speed each time, off the walls at x 0 and 48 */
static int demo_bounce(Demo *d, const GameClass *c)
{
    d->vy += 1.0;
    d->y += (int)d->vy; d->x += (int)d->vx;
    if (d->y + c->h >= 480) {
        d->y = 480 - c->h;
        int s = (int)d->vy;
        if (s >= 0 && s < 4) return 1;                                           /* too slow: it goes and a new coin is made */
        d->vy = -(double)(s - 1);
    }
    if (d->y < 0) d->y = 0;
    if (d->x < 0) { d->vx = -d->vx; d->x = 0; }
    if (d->x > 48) { d->vx = -d->vx; d->x = 48; }
    return 0;
}

static void demo_draw(uint32_t *fb, const Game *g, const Demo *d)
{
    const GameClass *c = &g->cls[d->cls];
    if (d->frame < 1 || d->frame > c->frames) return;
    const uint8_t *px = &c->pixels[(size_t)(d->frame - 1) * c->w * c->h];                 /* frames are numbered from 1 */
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            int xx = d->x + x, yy = d->y + y;
            if (!px[y * c->w + x] || xx < 0 || yy < 0 || xx >= TITLE_W || yy >= TITLE_H) continue;
            const uint8_t *p = &g->base_palette[px[y * c->w + x] * 3];
            fb[yy * TITLE_W + xx] = 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
}

/* the sprites the original makes for a page; class numbers are the indices of the class table in game.c */
static int demo_page(int page, Demo *d)
{
    static const Demo p0[] = {
        { 2, 24, 54, 1, 0x452 }, { 3, 30, 106, 1, 0x452 }, { 0, 24, 163, 5, 0x462 }, { 1, 6, 225, 0x26, 0x47A }, { 1, 22, 225, 0x21, 0x47A },
        { 1, 38, 225, 1, 0x472 }, { 4, 24, 285, 1, 0x452 }, { 6, 20, 330, 1, 0x452 }, { 5, 12, 380, 1, 0x45A }, { 18, 22, 440, 1, 0x452 } };
    static const Demo p1[] = { { 16, 24, 54, 1, 0x462 }, { 0, 24, 112, 1, 0x46A }, { 14, 24, 163, 1, 0x452 } };
    if (page == 0) { memcpy(d, p0, sizeof p0); return (int)(sizeof p0 / sizeof p0[0]); }
    if (page == 1) { memcpy(d, p1, sizeof p1); return (int)(sizeof p1 / sizeof p1[0]); }
    return 0;
}

/* returns 1 to go back to the title, 0 if the window was closed */
static int instructions(Game *g, const Config *cfg)
{
    GameCtx *c = &g->ctx;
    go_sound(g, (int)gm_rd32(c, c->a5 - 0x510u), 2);
    ResFile rf;
    char err[256], msg[256];
    static uint32_t page_fb[TITLE_W * TITLE_H], out[TITLE_W * TITLE_H];
    if (rsrc_open(&rf, cfg->data_dir, "Apeiron Titles", err, sizeof err)) return 1;
    int result = 1;
    for (int page = 0; page < 3 && result == 1; page++) {
        const Resource *r = rsrc_find(&rf, "PICT", 4000 + page);
        for (int i = 0; i < TITLE_W * TITLE_H; i++) page_fb[i] = 0xFF000000u;
        if (r && pict_draw(r->data, r->size, page_fb, TITLE_W, TITLE_H, g->base_palette, msg, sizeof msg)) fprintf(stderr, "instructions page %d: %s\n", page + 1, msg);
        Demo demo[24];
        int n = demo_page(page, demo);
        if (page == 2) { n = 5 + rand() % 10; for (int i = 0; i < n; i++) demo_coin(&demo[i], &g->cls[14]); }
        memcpy(out, page_fb, sizeof out);
        for (int i = 0; i < n; i++) demo_draw(out, g, &demo[i]);
        if (!fade_in(cfg, out, 20, 0)) { result = 0; break; }                    /* the page comes up from black */
        uint64_t t0 = SDL_GetPerformanceCounter();
        long steps_done = 0;
        int turn = 0;
        while (!turn && result == 1) {
            SDL_Event e;
            while (poll_event(&e)) {
                if (e.type == SDL_QUIT) result = 0;
                else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_KEYDOWN) turn = 1;
            }
            double ticks = (double)(SDL_GetPerformanceCounter() - t0) / (double)W.freq * 60.15;
            long steps = (long)(ticks / 2.0);                                   /* the sprites move every 2 Ticks */
            for (; steps_done < steps; steps_done++)
                for (int i = 0; i < n; i++) {
                    demo_frame(&demo[i], &g->cls[demo[i].cls]);
                    if (demo[i].bounce && demo_bounce(&demo[i], &g->cls[demo[i].cls])) demo_coin(&demo[i], &g->cls[14]);
                }
            memcpy(out, page_fb, sizeof out);
            for (int i = 0; i < n; i++) demo_draw(out, g, &demo[i]);
            SDL_UpdateTexture(W.tex, NULL, out, SCREEN_W * 4);
            SDL_RenderClear(W.ren); SDL_RenderCopy(W.ren, W.tex, NULL, NULL); SDL_RenderPresent(W.ren);
            if (getenv("APEIRON_PAGESHOT") && ticks > 60.0) {                   /* test: a picture of each page after about a second, then on to the next */
                char path[512];
                snprintf(path, sizeof path, "%s%d.bmp", getenv("APEIRON_PAGESHOT"), page + 1);
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(out, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
                if (s) { SDL_SaveBMP(s, path); SDL_FreeSurface(s); }
                turn = 1;
            }
            SDL_Delay(8);
        }
        if (result == 1) {
            go_sound(g, (int)gm_rd32(c, c->a5 - 0x508u), 3);                                   /* the page turn */
            if (!transition_out(g, cfg, out)) result = 0;
        }
    }
    rsrc_close(&rf);
    return result;
}

/* ---- About Apeiron (seg8_04FE, DLOG 150 at 126, 42): the programmer and artist credits (PICT 150), the script logo (PICT 151), the copyright (PICT 153), and a
 * line under "thanks:" that shows the next of the names of STR# 150 every 120 Ticks (the dialog filter, seg8_03D0, and the item's draw proc, seg8_0364) ---- */
static void box_pict(uint32_t *fb, const PictImage *im, int x0, int y0)
{
    if (!im->pixels || !im->indexed) return;
    for (int y = 0; y < im->height; y++)
        for (int x = 0; x < im->width; x++) {
            int xx = x0 + x, yy = y0 + y;
            if (xx < 0 || yy < 0 || xx >= TITLE_W || yy >= TITLE_H) continue;
            const uint8_t *c = &im->palette[im->pixels[y * im->width + x] * 3];
            fb[yy * TITLE_W + xx] = 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        }
}

/* returns 1 when closed with OK, 0 if the window was closed */
static int about_box(Game *g)
{
    const int dl = 126, dt = 42, dr = 394, db = 326;
    ResFile rf;
    char err[256], msg[256];
    PictImage pic[3];
    static const int ids[3] = { 150, 151, 153 };
    memset(pic, 0, sizeof pic);
    char names[70][96];
    int nnames = 0;
    memset(names, 0, sizeof names);
    if (!rsrc_open(&rf, g->data_dir, "Apeiron", err, sizeof err)) {
        for (int k = 0; k < 3; k++) { const Resource *r = rsrc_find(&rf, "PICT", ids[k]); if (r) pict_decode(r->data, r->size, &pic[k], msg, sizeof msg); }
        const Resource *r = rsrc_find(&rf, "STR#", 150);
        if (r && r->size >= 2) {
            unsigned n = (unsigned)(r->data[0] << 8 | r->data[1]), p = 2;
            for (unsigned k = 0; k < n && k < 69 && p < r->size; k++) {
                unsigned len = r->data[p];
                if (p + 1 + len > r->size) break;
                size_t m = 0;
                for (unsigned i = 0; i < len && m < 90; i++) { unsigned char ch = r->data[p + 1 + i]; names[k][m++] = (ch < 32 || ch > 126) ? '?' : (char)ch; }
                p += 1 + len; nnames++;
            }
        }
        rsrc_close(&rf);
    }
    int result = -2, shown = 0;                                    /* shown: the name now under "thanks:" (the original counts from 1 and wraps after 62) */
    uint64_t last = SDL_GetPerformanceCounter(), t_open = last;
    while (result == -2) {
        SDL_Event e;
        while (poll_event(&e) && result == -2) {
            if (e.type == SDL_QUIT) result = 0;
            else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER || e.key.keysym.sym == SDLK_ESCAPE)) result = 1;
            else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                int x = e.button.x - dl, y = e.button.y - dt;
                if (x >= 197 && x < 264 && y >= 260 && y < 280) result = 1;
            }
        }
        uint64_t now = SDL_GetPerformanceCounter();
        if ((double)(now - last) / (double)W.freq * 60.15 > 120.0) { shown = shown + 1 > 62 ? 1 : shown + 1; last = now; }
        title_draw(title_fb, g_tr, &g->scores, g_hl, -1);
        box_window(title_fb, dl, dt, dr, db);
        box_pict(title_fb, &pic[0], dl + 46, dt + 84);
        box_pict(title_fb, &pic[1], dl + 77, dt + 15);
        box_pict(title_fb, &pic[2], dl + 2, dt + 274);
        if (shown >= 1 && shown <= nnames) ffont_draw(title_fb, TITLE_W, TITLE_H, dl + 118, dt + 225 + 12, &font_sans9b, 0xFF000000u, names[shown - 1]);
        box_button(title_fb, dl + 197, dt + 260, dl + 264, dt + 280, "OK", 1, 0);
        SDL_UpdateTexture(W.tex, NULL, title_fb, SCREEN_W * 4);
        SDL_RenderClear(W.ren); SDL_RenderCopy(W.ren, W.tex, NULL, NULL); SDL_RenderPresent(W.ren);
        if (getenv("APEIRON_ABOUTSHOT") && (double)(now - t_open) / (double)W.freq > 0.3) {            /* test: a picture, then OK */
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(title_fb, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
            if (s) { SDL_SaveBMP(s, getenv("APEIRON_ABOUTSHOT")); SDL_FreeSurface(s); }
            result = 1;
        }
        SDL_Delay(8);
    }
    for (int k = 0; k < 3; k++) pict_free(&pic[k]);
    return result;
}

/* What a button does: 1 = play, 2 = quit, 0 = stay on the title. The arrows change the table, apply it, save the file and click. */
static int title_do(Game *g, int b)
{
    GameCtx *c = &g->ctx;
    if (b == TB_PLAY) return 1;
    if (b == TB_QUIT) return 2;
    if (b >= TB_VOL_DOWN) {
        ScoreTable *s = &g->scores;
        if (b == TB_VOL_DOWN) { if (--s->volume < 0) s->volume = 3; }
        else if (b == TB_VOL_UP) { if (++s->volume > 3) s->volume = 0; }
        else if (b == TB_MUSIC_LEFT) { if (--s->music < 0) s->music = 1; }
        else { if (++s->music > 1) s->music = 0; }
        scores_pack(s);
        apply_settings(g);
        if (g->scores_ok && game_scores_save(g)) fprintf(stderr, "the scores file could not be written\n");
        go_sound(g, (int)gm_rd32(c, c->a5 - 0x564u), 3);
    }
    if (b == TB_CLEAR) {
        int r = modal_clear(g);
        if (r < 0) return 2;
        if (r == 1) { clear_scores(g); g_hl = -1; go_sound(g, (int)gm_rd32(c, c->a5 - 0x56Cu), 3); }
        return 0;
    }
    if (b == TB_INSTR) {
        if (!transition_out(g, g_cfg, title_fb)) return 2;
        if (!instructions(g, g_cfg)) return 2;
        title_draw(title_fb, g_tr, &g->scores, g_hl, -1);
        if (!fade_in(g_cfg, title_fb, 20, 0)) return 2;
        return 0;
    }
    if (b == TB_ABOUT) { if (!about_box(g)) return 2; return 0; }
    return 0;
}

/* returns 1 to play, 0 to quit */
static int title_screen(Game *g, const TitleRes *tr, int highlight)
{
    GameCtx *c = &g->ctx;
    g_tr = tr; g_hl = highlight;
    grab(0); g_regrab = 0;
    if (g_hand) SDL_SetCursor(g_hand);
    SDL_ShowCursor(SDL_ENABLE);
    int pressed = -1, down = -1, result = 0;
    const char *clicks = getenv("APEIRON_CLICKS");
    if (!clicks) {                                                        /* the title comes up from black (seg6_0974, 20 steps) */
        title_draw(title_fb, tr, &g->scores, highlight, -1);
        if (!fade_in(g_cfg, title_fb, 20, 0)) return 0;
    }                       /* test: buttons clicked without the mouse ("0;5;7") and a picture of the result */
    if (clicks) {
        for (const char *p = clicks; *p; ) { int b = atoi(p); result = title_do(g, b); while (*p && *p != ';') p++; if (*p) p++; if (result) break; }
        title_present(tr, &g->scores, highlight, -1);
        if (getenv("APEIRON_TITLESHOT")) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(title_fb, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
            if (s) { SDL_SaveBMP(s, getenv("APEIRON_TITLESHOT")); SDL_FreeSurface(s); }
        }
        return result == 1;
    }
    while (!result) {
        SDL_Event e;
        while (poll_event(&e) && !result) {
            if (e.type == SDL_QUIT) result = 2;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_p) result = title_do(g, TB_PLAY);
                else if (k == SDLK_q || k == SDLK_ESCAPE) result = 2;
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                down = title_hit(e.button.x, e.button.y);
                if (down >= 0) { pressed = down; go_sound(g, (int)gm_rd32(c, c->a5 - 0x57Cu), 2); }
                else go_sound(g, (int)gm_rd32(c, c->a5 - 0x574u), 2);                  /* a click that misses every button (seg4_1114) */
            } else if (e.type == SDL_MOUSEMOTION && down >= 0) {
                int inside = title_hit(e.motion.x, e.motion.y) == down;
                if (inside && pressed < 0) { pressed = down; go_sound(g, (int)gm_rd32(c, c->a5 - 0x57Cu), 2); }
                else if (!inside && pressed >= 0) { pressed = -1; go_sound(g, (int)gm_rd32(c, c->a5 - 0x53Cu), 2); }
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && down >= 0) {
                if (title_hit(e.button.x, e.button.y) == down) result = title_do(g, down);
                pressed = -1; down = -1;
            }
        }
        title_present(tr, &g->scores, highlight, pressed);
        SDL_Delay(8);
    }
    if (result == 1 && !transition_out(g, g_cfg, title_fb)) return 0;      /* Play: the screen goes grey, white, black (seg2_0838) */
    return result == 1;
}

/* the game screen coming up from black after a new game starts (seg6_0974, 20 steps) */
static int start_fx(Game *g, const Config *cfg)
{
    game_render(g, W.fb);
    return fade_in(cfg, W.fb, 20, 0);
}

/* a new game, noted in the recording with the seed it starts from */
static void new_game(Game *g)
{
    rec_new(g, getenv("APEIRON_LEVEL") ? atoi(getenv("APEIRON_LEVEL")) : 0);
    game_new(g);
}

/* APEIRON_REPLAY=file: plays a recording again without a window and reports what the head watcher sees (marks are the F8 presses).
 * APEIRON_REPLAY_SHOT=step writes the screen after that step to replay_<step>.png */
static int replay_run(Game *game, const Config *cfg, const char *path)
{
    g_rep = fopen(path, "rb");
    uint8_t hd[8];
    if (!g_rep || fread(hd, 1, 8, g_rep) != 8 || memcmp(hd, "APRP", 4) || hd[4] != 1) { fprintf(stderr, "%s is not an Apeiron replay\n", path); return 1; }
    game->ctx.fix_coin = hd[5]; game->ctx.fix_glide = hd[6] || getenv("APEIRON_FIXGLIDE") != NULL;   /* the env var forces the fix on, to see what a recording does with it (it then runs differently from the recorded game) */ game->ctx.native_endwave = 1; game->ctx.wait = sdl_wait; game->busy_hook = busy_hook;
    set_env("APEIRON_PEDEWATCH", "1");
    if (getenv("APEIRON_VIEW")) {                        /* watching: a window and the sound, at the loop rate */
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
        V.win = SDL_CreateWindow("Apeiron replay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(SCREEN_W * cfg->scale + 0.5), (int)(SCREEN_H * cfg->scale + 0.5), 0);
        V.ren = V.win ? SDL_CreateRenderer(V.win, -1, SDL_RENDERER_ACCELERATED) : NULL;
        if (V.win && !V.ren) V.ren = SDL_CreateRenderer(V.win, -1, SDL_RENDERER_SOFTWARE);
        if (!V.win || !V.ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
        SDL_RenderSetLogicalSize(V.ren, SCREEN_W, SCREEN_H);
        V.tex = SDL_CreateTexture(V.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
        SDL_AudioSpec want, have; SDL_zero(want);
        want.freq = 22050; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 512; want.callback = audio_callback; want.userdata = game;
        g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audio) { game->audio_lock = audio_lock; SDL_PauseAudioDevice(g_audio, 0); }
        V.on = 1; V.speed = 1; V.hz = cfg->loop_hz;
    }
    uint32_t shot_at = getenv("APEIRON_REPLAY_SHOT") ? (uint32_t)atoi(getenv("APEIRON_REPLAY_SHOT")) : 0;
    int t, ended = 0;
    while ((t = rep_read()) != 0) {
        if (t == 'N') {
            char lv[16]; int l = get16(g_rb + 4);
            snprintf(lv, sizeof lv, "%d", l ? l : 1); set_env("APEIRON_LEVEL", lv);
            gm_wr32(&game->ctx, 0xA01C2u, get32(g_rb));
            game_new(game);
            fprintf(stderr, "new game after step %u\n", g_rep_steps);
            view_frame(game);
        } else if (t == 'S') {
            game->caps = (g_rb[4] & 2) != 0;
            if (getenv("APEIRON_FIXGLIDE_FROM") && g_rep_steps == (uint32_t)atoi(getenv("APEIRON_FIXGLIDE_FROM"))) game->ctx.fix_glide = 1;      /* test: the fix from this step on */
            game_step(game, get16(g_rb), get16(g_rb + 2), g_rb[4] & 1, get32(g_rb + 5));
            g_rep_steps++;
            view_frame(game);
            if (shot_at && g_rep_steps == shot_at) {
                static uint32_t fb[GAME_SCREEN_W * GAME_SCREEN_H]; static uint8_t rgb[GAME_SCREEN_W * GAME_SCREEN_H * 3];
                char name[64]; snprintf(name, sizeof name, "replay_%u.png", shot_at);
                game_render(game, fb);
                for (int i = 0; i < GAME_SCREEN_W * GAME_SCREEN_H; i++) { rgb[i * 3] = (uint8_t)(fb[i] >> 16); rgb[i * 3 + 1] = (uint8_t)(fb[i] >> 8); rgb[i * 3 + 2] = (uint8_t)fb[i]; }
                write_png_rgb(name, GAME_SCREEN_W, GAME_SCREEN_H, rgb);
                fprintf(stderr, "wrote %s\n", name);
            }
        } else if (t == 'Z') {
            ended = 1;
            if (get32(g_rb) != sim_hash(game) && !g_rep_bad) { g_rep_bad = 1; fprintf(stderr, "the replay ended in a different state from the recorded game\n"); }
        } else rep_side(game, t);        /* keys, the stop key, marks, checks; a stray wait record (from the game-over screens) is skipped */
        if (game->ctx.quit) break;
    }
    GameCtx *c = &game->ctx;
    if (V.on && !c->quit) { fprintf(stderr, "the replay is over; Esc or closing the window ends it\n"); while (!c->quit) { V.paused = 1; view_frame(game); } }
    fprintf(stderr, "replay: %u steps, %u marks, wave %d, score %u, lives %d, %s\n", g_rep_steps, g_rec_marks, (int)(int16_t)gm_rd16(c, c->a5 - 0x734u), gm_rd32(c, c->a5 - 0x702u), (int)(int16_t)gm_rd16(c, c->a5 - 0x6FAu),
            g_rep_bad ? "it did NOT stay in step with the recording" : ended ? "it stayed in step with the recording to the end" : "the recording stops here (no end mark: the game was cut off)");
    return g_rep_bad ? 3 : 0;
}

int main(int argc, char **argv)
{
    Config cfg;
    config_defaults(&cfg);
    config_load(&cfg, "apeiron.ini");
    g_cfg = &cfg;
    g_cfg = &cfg;
    int ai = 1;                                      /* apeiron [data folder]  |  apeiron file.bin  |  apeiron --replay file.bin [data folder]: the last two watch a recording */
    if (argc > 2 && !strcmp(argv[1], "--replay")) { set_env("APEIRON_REPLAY", argv[2]); set_env("APEIRON_VIEW", "1"); ai = 3; }
    else if (argc > 1 && strlen(argv[1]) > 4 && !strcmp(argv[1] + strlen(argv[1]) - 4, ".bin")) { set_env("APEIRON_REPLAY", argv[1]); set_env("APEIRON_VIEW", "1"); ai = 2; }
    if (argc > ai) snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s", argv[ai]);

    static uint32_t fb[SCREEN_W * SCREEN_H];
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = 0xFF000000u;

    static Game game;
    if (game_init(&game, cfg.data_dir)) {
        fprintf(stderr, "%s\nPut the original game files in the folder '%s' (see README).\n", game.error, cfg.data_dir);
        return 1;
    }
    if (getenv("APEIRON_DUMPCLASS")) { game_dump_class(&game, atoi(getenv("APEIRON_DUMPCLASS")), "class_dump.ppm"); game_dump_class_raw(&game, atoi(getenv("APEIRON_DUMPCLASS")), "class_dump.raw"); return 0; }
    if (getenv("APEIRON_REPLAY")) return replay_run(&game, &cfg, getenv("APEIRON_REPLAY"));
    if (cfg.record || getenv("APEIRON_RECORD")) rec_open(&game, cfg.fix_coin || getenv("APEIRON_FIXCOIN") != NULL, cfg.fix_glide || getenv("APEIRON_FIXGLIDE") != NULL);
    new_game(&game);
    if (getenv("APEIRON_DEBUG")) fprintf(stderr, "music: playing id %d (start %u frames, loop %u frames), volume %d\n", game.mixer.music_id, game.mixer.sound[1000].frames, game.mixer.sound[1001].frames, game.mixer.volume);
    if (getenv("APEIRON_LISTS")) {
        static const int offs[5] = { -0x644, -0x648, -0x640, -0x63C, -0x64C };
        for (int li = 0; li < 5; li++) {
            uint32_t l = gm_rd32(&game.ctx, game.ctx.a5 + (uint32_t)offs[li]);
            for (uint32_t s = gm_rd32(&game.ctx, l + 4); s; s = gm_rd32(&game.ctx, s + 0x5A)) {
                uint32_t t = gm_rd32(&game.ctx, s);
                if (li == 4) continue;
                fprintf(stderr, "list %d %c%c%c%c vis=%d pos=(%d,%d) class=%06X frame=%d", li, t >> 24, (t >> 16) & 255, (t >> 8) & 255, t & 255, gm_rd8(&game.ctx, s + 0x1C), (int)(gm_rd32(&game.ctx, s + 0x2A) >> 16), (int)(gm_rd32(&game.ctx, s + 0x2E) >> 16), gm_rd32(&game.ctx, s + 4), gm_rd16(&game.ctx, s + 0x48));
                fputc(10, stderr);
            }
        }
    }
    if (getenv("APEIRON_COINTEST")) spawn_coins(&game.ctx, gm_rd32(&game.ctx, game.ctx.a5 - 0x59Cu), gm_rd32(&game.ctx, game.ctx.a5 - 0x600u));

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("Apeiron", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(SCREEN_W * cfg.scale + 0.5), (int)(SCREEN_H * cfg.scale + 0.5), 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!win || !ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    {                                                       /* the application icon of the original, if the Apeiron file has it */
        ResFile irf; char ierr[256];
        if (!rsrc_open(&irf, cfg.data_dir, "Apeiron", ierr, sizeof ierr)) {
            const Resource *i8 = rsrc_find(&irf, "icl8", 128), *im = rsrc_find(&irf, "ICN#", 128);
            CicnImage ii;
            if (i8 && im && icon_decode(i8->data, i8->size, im->data, im->size, &ii) == 0) {
                SDL_Surface *is = SDL_CreateRGBSurfaceWithFormatFrom(ii.argb, 32, 32, 32, 128, SDL_PIXELFORMAT_ARGB8888);
                if (is) { SDL_SetWindowIcon(win, is); SDL_FreeSurface(is); }
                cicn_free(&ii);
            }
            rsrc_close(&irf);
        }
    }
    SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);            /* keeps 4:3 (letterboxed) at any window size */
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    grab(0);
    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = 22050; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 512; want.callback = audio_callback; want.userdata = &game;
        g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audio) { game.audio_lock = audio_lock; SDL_PauseAudioDevice(g_audio, 0); }
        else fprintf(stderr, "no audio: %s\n", SDL_GetError());
    }

    g_coll_log = getenv("APEIRON_COLLLOG") != NULL;
    if (cfg.pede_log) pede_log_open("apeiron_pede.txt");
    if (getenv("APEIRON_COINFIXTEST")) {                /* test: coins thrown straight up (APEIRON_FIXCOIN=1 turns the fix on) */
        game.ctx.fix_coin = getenv("APEIRON_FIXCOIN") != NULL;
        game_new(&game);
        coin_fix_test(&game.ctx);
        return 0;
    }
    if (getenv("APEIRON_PEDEFUZZ")) {                /* test: many small set-ups of a few heads at a side wall; the watcher (APEIRON_PEDEWATCH) reports fast descents */
        int trials = atoi(getenv("APEIRON_PEDEFUZZ")), passes_each = getenv("APEIRON_FUZZPASSES") ? atoi(getenv("APEIRON_FUZZPASSES")) : 150;
        for (int t = 0; t < trials; t++) {
            if (getenv("APEIRON_FUZZGAME")) gm_wr32(&game.ctx, 0xA01C2u, (uint32_t)t * 7919u + 1u);        /* the game's own wave set-up (APEIRON_LEVEL), a different seed each trial */
            game_new(&game);
            if (!getenv("APEIRON_FUZZGAME")) pede_fuzz_setup(&game.ctx, (uint32_t)t + 1);
            g_watch_reset = 1;
            fprintf(stderr, "[trial %d]\n", t + 1);
            for (int p = 0; p < passes_each; p++) { gm_wr8(&game.ctx, game.ctx.a5 - 0x729u, 0); game_step(&game, getenv("APEIRON_FUZZFIRE") ? (int)(5.0 * sin((t * 31 + p) * 0.05)) : 0, getenv("APEIRON_FUZZFIRE") ? (int)(2.0 * sin((t * 17 + p) * 0.021)) : 0, getenv("APEIRON_FUZZFIRE") != NULL, 36000u + (uint32_t)(p * 60.15 / 60.0) + (uint32_t)t * 1000u); }
            if (getenv("APEIRON_FUZZLOG")) fprintf(stderr, "  end: pedes alive %d, score %u, lives %d, level %d\n", (int)(int16_t)gm_rd16(&game.ctx, game.ctx.a5 - 0x6E6u), gm_rd32(&game.ctx, game.ctx.a5 - 0x702u), (int)(int16_t)gm_rd16(&game.ctx, game.ctx.a5 - 0x6FAu), (int)(int16_t)gm_rd16(&game.ctx, game.ctx.a5 - 0x734u));
        }
        return 0;
    }
    const char *shot = getenv("APEIRON_SHOT");
    const char *passes_env = getenv("APEIRON_PASSES");
    int shot_passes = passes_env ? atoi(passes_env) : 300;
    int autofire = getenv("APEIRON_AUTOFIRE") != NULL;
    int running = 1;
    g_scale = cfg.scale; g_speed = cfg.mouse_speed;
    uint64_t freq = SDL_GetPerformanceFrequency(), start = SDL_GetPerformanceCounter(), last = start;
    double acc = 0.0, step = 1.0 / cfg.loop_hz;
    uint32_t passes = 0;
    const char *prof_env = getenv("APEIRON_PROFILE");                /* timing report: APEIRON_PROFILE=1 (and APEIRON_RUNSECS=n to stop after n seconds) */
    double run_secs = getenv("APEIRON_RUNSECS") ? atof(getenv("APEIRON_RUNSECS")) : 0.0;
    uint64_t pr_step = 0, pr_step_max = 0, pr_conv = 0, pr_present = 0, pr_frame_max = 0, pr_last_frame = SDL_GetPerformanceCounter();
    uint64_t pr_last_frame_hl = SDL_GetPerformanceCounter();
    uint32_t pr_frames = 0, pr_passes = 0, pr_late = 0, pr_delta[5] = { 0, 0, 0, 0, 0 }, pr_prev_ticks = 0;
    FILE *hl = (cfg.hitch_log || getenv("APEIRON_HITCHLOG")) ? fopen("apeiron_hitches.txt", "w") : NULL;
    uint64_t hl_next = start + 5 * freq, hl_frame_passes = 0; uint32_t hl_game0 = 0, hl_real0 = 36000u;
    if (hl) fprintf(hl, "Apeiron hitch log: loop %d Hz, steady_ticks %d. 'frame' lines: a frame that took more than 1.5 steps (total time, drawing, waiting for the monitor, and how many game passes ran in it).\n'time' lines: every 5 s, game ticks counted against real Ticks (about 148%% is normal: the game adds a tick of its own per step).\n", cfg.loop_hz, cfg.steady_ticks);
    g_test_shot = shot != NULL;
    W.ren = ren; W.tex = tex; W.fb = fb; W.game = &game; W.start = start; W.freq = freq; W.step = step; W.shot = shot != NULL; W.steady = cfg.steady_ticks; W.loop_hz = cfg.loop_hz; W.next = start;
    game.busy_hook = busy_hook;
    game.ctx.native_endwave = 1; game.ctx.wait = sdl_wait; game.ctx.fix_coin = cfg.fix_coin || getenv("APEIRON_FIXCOIN") != NULL; game.ctx.fix_glide = cfg.fix_glide || getenv("APEIRON_FIXGLIDE") != NULL;
    static TitleRes titleres;
    int have_title = !shot && !getenv("APEIRON_NOTITLE") && title_load(&titleres, cfg.data_dir) == 0;
    int highlight = -1;
    if (have_title) {
        audio_lock(NULL, 1); mixer_stop_all(&game.mixer); audio_lock(NULL, 0);                  /* the game starts when Play is pressed */
        apply_settings(&game);
        {                                                                                         /* the pointing hand of the title */
            ResFile crf; char cerr[256], cmsg[256];
            if (!rsrc_open(&crf, cfg.data_dir, "Apeiron", cerr, sizeof cerr)) {
                const Resource *cr = rsrc_find(&crf, "crsr", 128);
                CicnImage ci; int hx = 0, hy = 0;
                if (cr && crsr_decode(cr->data, cr->size, &ci, &hx, &hy, cmsg, sizeof cmsg) == 0) {
                    SDL_Surface *cs = SDL_CreateRGBSurfaceWithFormatFrom(ci.argb, ci.width, ci.height, 32, ci.width * 4, SDL_PIXELFORMAT_ARGB8888);
                    if (cs) { g_hand = SDL_CreateColorCursor(cs, hx, hy); SDL_FreeSurface(cs); }
                    cicn_free(&ci);
                }
                rsrc_close(&crf);
            }
        }
        if (cfg.intro && !getenv("APEIRON_NOINTRO") && !intro_screens(&game, &cfg)) running = 0;      /* the start-up screens */
        if (!running) { }
        else if (!title_screen(&game, &titleres, highlight)) running = 0;
        else { new_game(&game); if (!start_fx(&game, &cfg)) running = 0; }
    }
    if (!shot && running) grab(1);
    sync_mouse();
    while (running) {
        SDL_Event e;
        while (poll_event(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) { rec_tag('E'); gm_wr8(&game.ctx, game.ctx.a5 - 0x730u, 0); }      /* the stop key ends the game (it goes to the game-over screen) */
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F8 && g_rec) { rec_tag('M'); char tt[48]; snprintf(tt, sizeof tt, "Apeiron - marked (%u)", ++g_rec_marks); SDL_SetWindowTitle(win, tt); }      /* a mark in the recording: "it just happened" */
            else if (e.type == SDL_TEXTINPUT) for (const char *t = e.text.text; *t; t++) key_in(&game, (unsigned char)*t);
            else if (e.type == SDL_MOUSEMOTION) { if (g_grabbed || shot || !g_regrab) add_motion(e.motion.xrel, e.motion.yrel); }
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) { if (!g_grabbed && g_regrab && !shot) { grab(1); g_regrab = 0; } else g_button = 1; }          /* a click in the window takes the mouse back, without firing */
            else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) g_button = 0;
        }
        game.caps = (SDL_GetModState() & KMOD_CAPS) != 0;
        if (getenv("APEIRON_PAUSETEST")) game.caps = passes >= 50 && passes < 150;
        uint64_t now = SDL_GetPerformanceCounter();
        acc += (double)(now - last) / (double)freq;
        last = now;
        if (acc > 0.25) acc = 0.25;
        if (shot) acc = step * 4;                       /* test mode: run passes as fast as possible */
        while (acc >= step && running) {
            if (shot || cfg.steady_ticks) W.passes = passes;
            uint32_t ticks = 36000u + (shot || cfg.steady_ticks ? (uint32_t)(passes * 60.15 / cfg.loop_hz) : (uint32_t)((double)(now - start) / (double)freq * 60.15));   /* the Mac's Ticks count from boot, never from zero */
            if (getenv("APEIRON_PSYCTEST")) {      /* test: as if the colour rock was shot at pass 50, and report the music now and then */
                GameCtx *tc = &game.ctx;
                if (passes == 50) {
                    gm_wr32(tc, tc->a5 - 0x714u, gm_rd32(tc, tc->a5 - 0x6DEu)); gm_wr32(tc, tc->a5 - 0x710u, gm_rd32(tc, tc->a5 - 0x6DEu));
                    gm_wr8(tc, tc->a5 - 0x716u, 1); gm_wr32(tc, tc->a5 - 0x706u, 10); mixer_music_stop(&game.mixer);
                }
                if (passes % 100 == 0) fprintf(stderr, "pass %u: music %d psyc flag %d points %d\n", passes, game.mixer.music_id, gm_rd8(tc, tc->a5 - 0x716u), (int)gm_rd32(tc, tc->a5 - 0x706u));
            }
            if (getenv("APEIRON_FLASHTEST") && passes == 50) {        /* test: the death-flash palette from pass 50 on (the table the game sets when the shooter dies) */
                uint8_t raw[8] = { 0x00, 0xA0, 0x40, 0x18, 0x00, 0xFF, 0, 0 };
                game.ctx.ext(game.ctx.ud, 0xAA3F, raw, 8);
            }
            if (getenv("APEIRON_GOTEST") && passes == 100) gm_wr32(&game.ctx, game.ctx.a5 - 0x702u, 1234);                          /* test: a score that makes the table */
            if (getenv("APEIRON_GOTEST") && (passes == 100 || passes == 400)) gm_wr8(&game.ctx, game.ctx.a5 - 0x730u, 0);          /* test: the stop key twice, to see game over and the next game */
            if (getenv("APEIRON_ENDWAVE") && passes == 100) gm_wr16(&game.ctx, game.ctx.a5 - 0x6E6u, 0);      /* test: pretend the last pede segment was shot */
            if (pr_prev_ticks) { uint32_t d = ticks - pr_prev_ticks; pr_delta[d > 4 ? 4 : d]++; }
            pr_prev_ticks = ticks;
            uint64_t t0 = SDL_GetPerformanceCounter();
            int pending_x, pending_y; take_motion(&pending_x, &pending_y);
            hl_frame_passes++;
            W.next = 0;
            if (getenv("APEIRON_WIGGLE")) { pending_x += (int)(5.0 * sin(passes * 0.05)); pending_y += (int)(2.0 * sin(passes * 0.021)); }       /* test: the shooter sways about */
            rec_input(&game, 'S', pending_x, pending_y, (g_button || autofire) | (game.caps << 1), ticks);
            int alive = game_step(&game, pending_x, pending_y, g_button || autofire, ticks);
            if (game.ctx.quit) running = 0;
            else if (!alive) {
                int place = -1;
                if (game_over_flow(&game, &cfg, shot != NULL || getenv("APEIRON_GOTEST") != NULL, &place)) {
                    if (have_title) {                                   /* back to the title, with the new score's row in white */
                        highlight = place;
                        if (!title_screen(&game, &titleres, highlight)) running = 0;
                        else grab(1);
                    }
                    if (running) { new_game(&game); if (have_title && !start_fx(&game, &cfg)) running = 0; sync_mouse(); passes = shot || cfg.steady_ticks ? W.passes : passes; }
                } else running = 0;
                acc = 0.0; last = SDL_GetPerformanceCounter();
            }
            { uint64_t d = SDL_GetPerformanceCounter() - t0; pr_step += d; if (d > pr_step_max) pr_step_max = d; pr_passes++; }
            passes = (shot || cfg.steady_ticks) ? W.passes + 1 : passes + 1;
            acc -= step;
            if (shot && (int)passes >= shot_passes) break;
        }
        uint64_t r0 = SDL_GetPerformanceCounter();
        game_render(&game, fb);
        uint64_t r1 = SDL_GetPerformanceCounter();
        SDL_UpdateTexture(tex, NULL, fb, SCREEN_W * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        uint64_t r2 = SDL_GetPerformanceCounter();
        pr_conv += r1 - r0; pr_present += r2 - r1; pr_frames++;
        if (r2 - pr_last_frame > pr_frame_max) pr_frame_max = r2 - pr_last_frame;
        if ((double)(r2 - pr_last_frame) / (double)freq > 1.5 * step) pr_late++;
        pr_last_frame = r2;
        if (hl) {
            double ms = 1000.0 / (double)freq;
            if ((double)(r2 - pr_last_frame_hl) / (double)freq > 1.5 * step && pr_frames > 1)
                fprintf(hl, "frame %.1f s: %.1f ms total (draw %.2f, present %.1f), %u passes\n", (double)(r2 - start) / (double)freq, (double)(r2 - pr_last_frame_hl) * ms, (double)(r1 - r0) * ms, (double)(r2 - r1) * ms, (unsigned)hl_frame_passes);
            pr_last_frame_hl = r2; hl_frame_passes = 0;
            if (r2 >= hl_next) {
                uint32_t g1 = gm_rd32(&game.ctx, game.ctx.a5 - 0x6DEu), r1t = gm_rd32(&game.ctx, 0x16A);
                if (r1t != hl_real0) fprintf(hl, "time %.0f s: game ticks %u in %u real Ticks (%.1f%%)\n", (double)(r2 - start) / (double)freq, g1 - hl_game0, r1t - hl_real0, 100.0 * (double)(g1 - hl_game0) / (double)(r1t - hl_real0));
                hl_game0 = g1; hl_real0 = r1t; hl_next += 5 * freq; fflush(hl);
            }
        }
        if (prof_env && pr_frames % 300 == 0) {
            double ms = 1000.0 / (double)freq;
            fprintf(stderr, "frames %u passes %u: step avg %.3f max %.3f ms | rgb convert %.3f ms | present %.3f ms | frame max %.1f ms, late frames %u\n", pr_frames, pr_passes,
                    (double)pr_step * ms / (pr_passes ? pr_passes : 1), (double)pr_step_max * ms, (double)pr_conv * ms / pr_frames, (double)pr_present * ms / pr_frames, (double)pr_frame_max * ms, pr_late);
            fprintf(stderr, "game ticks counted %u, real ticks elapsed %u (%.1f%%)\n", gm_rd32(&game.ctx, game.ctx.a5 - 0x6DEu), gm_rd32(&game.ctx, 0x16A) - 36000u,
                    100.0 * (double)gm_rd32(&game.ctx, game.ctx.a5 - 0x6DEu) / (double)(gm_rd32(&game.ctx, 0x16A) - 36000u));
            fprintf(stderr, "Ticks between passes: 0:%u 1:%u 2:%u 3:%u 4+:%u\n", pr_delta[0], pr_delta[1], pr_delta[2], pr_delta[3], pr_delta[4]);
            pr_step_max = 0; pr_frame_max = 0;
        }
        if (run_secs > 0.0 && (double)(r2 - start) / (double)freq > run_secs) break;
        if (shot && (int)passes >= shot_passes) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(fb, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_ARGB8888);
            if (s) { SDL_SaveBMP(s, shot); SDL_FreeSurface(s); }
            printf("wrote %s after %u passes (loop %d Hz, scale %.2f)\n", shot, passes, cfg.loop_hz, cfg.scale);
            break;
        }
        SDL_Delay(1);
    }
    if (g_audio) { SDL_CloseAudioDevice(g_audio); g_audio = 0; }
    rec_close();                                       /* before the game's memory goes */
    game_free(&game);
    SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
