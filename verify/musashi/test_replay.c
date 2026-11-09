/* Plays a recording (apeiron_replay_*.bin, see the README of apeiron_port) through the port AND the original 68k code, step by step, and compares the
 * game's state after every step.
 *
 *   test_replay <apeiron_work folder> <data folder> <recording.bin> [max mismatches]
 *
 * The port runs the recorded game exactly as `APEIRON_REPLAY` does. A second copy of the state lives in the 68k image; for each recorded step the same
 * input (mouse, button, Ticks, the sound-playing answers) is given to the original loop body (seg3_123C, as in test_loop) and the two memories are compared:
 * the application globals, the engine, lists, grids, sprites and heap (not the screen buffers: the 68k drawing is stubbed). Both sides allocate with the
 * port's allocator, so pointers can be compared.
 * Not covered by the 68k side, so the 68k copy is set equal to the port's again (and the step is not compared) after: a new game (the port's new-game
 * routine builds the state), and a step in which an end-of-round loop, the pause or a cheat key ran. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/game.h"
#include "harness.h"
#include "m68k.h"

static Game game;
static GameCtx c68;                          /* allocator state of the 68k side (its memory is g_mem) */
static FILE *rep;
static uint8_t rb[12];
static uint32_t steps, compared, skipped, marks, mismatches, new_games;
static int wait_used, resync, stream_bad, hash_bad;
static uint32_t busy_q[256];
static int busy_n, busy_i, busy_under;

static int get16(const uint8_t *b) { return (int16_t)(b[0] | b[1] << 8); }
static uint32_t get32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }

static uint32_t sim_hash(Game *g)            /* the same as the front end's, to check the recording is played as it was recorded */
{
    uint32_t h = 2166136261u;
    const uint8_t *m = g->mem + g->ctx.a5 - 0x800u;
    for (uint32_t i = 0; i < 0x900u; i++) h = (h ^ m[i]) * 16777619u;
    for (uint32_t i = 0; i < 4; i++) h = (h ^ g->mem[0xA01C2u + i]) * 16777619u;
    return h;
}

static int rep_read(void)
{
    int t = fgetc(rep), n;
    if (t == EOF) return 0;
    n = t == 'N' ? 6 : (t == 'S' || t == 'W') ? 9 : (t == 'K' || t == 'B') ? 1 : (t == 'H' || t == 'Z') ? 4 : (t == 'E' || t == 'M') ? 0 : -1;
    if (n < 0 || (n && fread(rb, 1, (size_t)n, rep) != (size_t)n)) return 0;
    return t;
}

static int side(Game *g, int t)
{
    GameCtx *c = &g->ctx;
    if (t == 'K') { game_key(g, rb[0]); resync = 1; }
    else if (t == 'E') { gm_wr8(c, c->a5 - 0x730u, 0); g_mem[g_a5 - 0x730u] = 0; }
    else if (t == 'M') printf("  (mark %u in the recording, after step %u)\n", ++marks, steps);
    else if (t == 'H') { if (get32(rb) != sim_hash(g) && !hash_bad) { hash_bad = 1; printf("the port itself has drifted from the recording before step %u\n", steps); } }
    else return 0;
    return 1;
}

static int busy_hook(Game *g, int playing)
{
    (void)playing;
    int t;
    while ((t = rep_read()) != 0 && side(g, t)) { }
    if (t != 'B') { g->ctx.quit = 1; stream_bad = 1; return 0; }
    if (busy_n < 256) busy_q[busy_n++] = rb[0] ? 0xFFu : 0u;
    return rb[0];
}

static void wait_cb(void *ud)
{
    Game *gm = (Game *)ud;
    int t;
    wait_used = 1;
    while ((t = rep_read()) != 0 && side(gm, t)) { }
    if (t != 'W') { gm->ctx.quit = 1; if (t) stream_bad = 1; return; }
    gm->caps = rb[4] & 1;
    gm_wr16(&gm->ctx, gm->ctx.a5 - 0x41Cu, (uint16_t)(gm_rd16(&gm->ctx, gm->ctx.a5 - 0x41Cu) + get16(rb)));
    gm_wr16(&gm->ctx, gm->ctx.a5 - 0x41Au, (uint16_t)(gm_rd16(&gm->ctx, gm->ctx.a5 - 0x41Au) + get16(rb + 2)));
    gm_wr32(&gm->ctx, 0x16A, get32(rb + 5));
}

/* the 68k side's allocator is the port's (header word, free lists), on the 68k memory */
static uint32_t cb_alloc(uint32_t size) { return gm_alloc(&c68, size); }
static void cb_free(uint32_t p)
{
    if (!p) return;
    uint32_t size = gm_rd32(&c68, p - 4);
    for (int i = 0; i < 8; i++)
        if (c68.free_size[i] == size || c68.free_size[i] == 0) { c68.free_size[i] = size; gm_wr32(&c68, p, c68.free_head[i]); c68.free_head[i] = p; return; }
}
static uint32_t stub_cb(uint32_t addr, const uint8_t *raw)
{
    (void)raw;
    if (addr == g_a5 + 0x3D2u) { if (busy_i < busy_n) return busy_q[busy_i++]; busy_under = 1; return 0; }
    return 0;
}

static void sync68(void)                     /* the 68k copy of the state is made equal to the port's */
{
    const uint8_t *p = game.mem;
    memcpy(&g_mem[0x160], &p[0x160], 0x20);
    memcpy(&g_mem[A5_WORLD], &p[A5_WORLD], g_a5 - A5_WORLD);
    memcpy(&g_mem[0x300000], &p[0x300000], 0x400000);
    memcpy(&g_mem[0xA00000], &p[0xA00000], 0x2000);
    memcpy(&g_mem[0xA01C2], &p[0xA01C2], 4);               /* the random seed: a static of code segment 9 (0x0A01C2), not part of the colour tables at 0xA00000 */
    c68.heap_next = game.ctx.heap_next;
    memcpy(c68.free_size, game.ctx.free_size, sizeof c68.free_size);
    memcpy(c68.free_head, game.ctx.free_head, sizeof c68.free_head);
}

static const struct { uint32_t base, len; const char *name; } kRegions[] = {
    { 0x16A, 4, "Ticks" }, { A5_WORLD, 0, "the application globals" }, { 0x300000, 0x400000, "engine, lists, grids, sprites, heap" }, { 0xA01C2, 4, "random seed" } };

static int compare(void)
{
    int bad = 0;
    for (int r = 0; r < 4; r++) {
        uint32_t base = kRegions[r].base, len = kRegions[r].len ? kRegions[r].len : g_a5 - A5_WORLD;
        if (!memcmp(&g_mem[base], &game.mem[base], len)) continue;
        int shown = 0, count = 0;
        for (uint32_t k = 0; k < len; k++) {
            if (g_mem[base + k] == game.mem[base + k]) continue;
            count++;
            if (shown < 6) {
                shown++;
                if (base == A5_WORLD) printf("    %s: A5%+d  68k %02X  port %02X\n", kRegions[r].name, (int)(base + k) - (int)g_a5, g_mem[base + k], game.mem[base + k]);
                else printf("    %s: %06X  68k %02X  port %02X\n", kRegions[r].name, base + k, g_mem[base + k], game.mem[base + k]);
            }
        }
        printf("    (%d bytes differ in %s)\n", count, kRegions[r].name);
        bad = 1;
    }
    if (c68.heap_next != game.ctx.heap_next) { printf("    the heap pointers differ: 68k %06X, port %06X\n", c68.heap_next, game.ctx.heap_next); bad = 1; }
    return bad;
}

static void audit_frames(void)               /* DBG_FRAMES=1: sprites whose frame has no drawing routine in their class */
{
    GameCtx *c = &game.ctx;
    for (uint32_t l = gm_rd32(c, 0x3A0000u + 4); l; l = gm_rd32(c, l + 0x12))
        for (uint32_t sp = gm_rd32(c, l + 4); sp; sp = gm_rd32(c, sp + 0x5A)) {
            uint32_t h = gm_rd32(c, sp + 4), d = gm_rd32(c, h);
            int f = (int16_t)gm_rd16(c, sp + 0x48);
            if (f < 1 || f > 200 || !gm_rd32(c, d + gm_rd32(c, d + 0xE) + (uint32_t)(f - 1) * 4))
                printf("  step %u: sprite %08X tag %c%c%c%c class %u frame %d has no draw routine; visible %d, rect %d,%d\n", steps, sp, gm_rd8(c, sp) , gm_rd8(c, sp + 1), gm_rd8(c, sp + 2), gm_rd8(c, sp + 3), (h - 0x3C0000u) / 4, f, gm_rd8(c, sp + 0x1C), (int16_t)gm_rd16(c, sp + 8), (int16_t)gm_rd16(c, sp + 10));
        }
}

/* A sprite at frame 0 (a pede head before its first frame step, seen at wave 12) makes the original draw through table entry -1. Its real class data has
 * something there that the port's stand-in class data does not, so for the 68k step that entry is a routine that returns at once (and is put back). */
static uint32_t frame0_saved[GAME_CLASSES];
static void frame0_stub(int on)
{
    for (int k = 0; k < GAME_CLASSES; k++) {
        uint32_t d = gm_rd32(&c68, 0x3C0000u + (uint32_t)k * 4);          /* the class handle's master pointer */
        if (!d) continue;
        uint32_t at = d + gm_rd32(&c68, d + 0xE) - 4;
        if (on) { frame0_saved[k] = gm_rd32(&c68, at); gm_wr32(&c68, at, 0x7000F00u - d); }
        else gm_wr32(&c68, at, frame0_saved[k]);
    }
}

static void run68k(int dx, int dy, int button, uint32_t ticks, int caps)
{
    wr32(0x16A, ticks);
    g_mem[0x17B] = caps ? 2 : 0;
    wr16(g_a5 - 0x41C, (uint16_t)((int16_t)rd16(g_a5 - 0x41C) + dx));
    wr16(g_a5 - 0x41A, (uint16_t)((int16_t)rd16(g_a5 - 0x41A) + dy));
    g_mem[g_a5 - 0x418] = button ? 1 : 0;
    g_extra_stack = 12; busy_i = 0; busy_under = 0;
    uint8_t keep = g_mem[g_a5 - 0x4D2];                   /* the loop's "keep going" flag: cleared so that the body runs one pass, as test_loop does */
    g_mem[g_a5 - 0x4D2] = 0;
    frame0_stub(1);
    extern int g_trace; g_trace = getenv("TRACE_STEP") && steps == (uint32_t)atoi(getenv("TRACE_STEP"));
    m68k_call(0x41244, NULL, NULL, 0, 3000000);
    g_trace = 0;
    frame0_stub(0);
    g_mem[g_a5 - 0x4D2] = keep;
    if (g_instr_count >= 3000000) printf("  the 68k step did not finish (pc %06X, last trap %04X, %d trap hits)\n", m68k_get_reg(NULL, M68K_REG_PC), g_last_trap, g_trap_hits);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: test_replay <apeiron_work> <data folder> <recording.bin> [max mismatches]\n"); return 2; }
    int max_bad = argc > 4 ? atoi(argv[4]) : 10;
    if (image_load(argv[1])) return 2;
    rep = fopen(argv[3], "rb");
    uint8_t hd[8];
    if (!rep || fread(hd, 1, 8, rep) != 8 || memcmp(hd, "APRP", 4) || hd[4] != 1) { fprintf(stderr, "%s is not an Apeiron replay\n", argv[3]); return 2; }
    if (game_init(&game, argv[2])) { fprintf(stderr, "%s\n", game.error); return 2; }
    if (game.ctx.a5 != g_a5) { fprintf(stderr, "the port's A5 (%06X) is not the harness's (%06X)\n", game.ctx.a5, g_a5); return 2; }
    game.ctx.fix_coin = hd[5]; game.ctx.fix_glide = hd[6]; game.ctx.native_endwave = 1; game.ctx.wait = wait_cb; game.busy_hook = busy_hook;
    c68.mem = g_mem; c68.a5 = g_a5; c68.reuse_memory = 1;
    g_stub_lo = 0x7000000u; g_stub_hi = 0x8000000u;           /* the port's blit routines (game.c BLIT_BASE) */
    g_stub_cb = stub_cb; g_alloc_cb = cb_alloc; g_free_cb = cb_free;
    harness_clear_stubs();
    harness_add_stub(g_a5 + 0x3C2u, 10); harness_add_stub(g_a5 + 0x3D2u, 4); harness_add_stub(g_a5 + 0x3DAu, 2); harness_add_stub(g_a5 + 0x4E2u, 4);
    harness_add_stub(g_a5 + 0x36Au, 5); harness_add_stub(g_a5 + 0x39Au, 6); harness_add_stub(g_a5 + 0x3E2u, 12);
    static const struct { uint32_t addr; int n; } st[] = { { 0x415E4, 9 }, { 0x41428, 0 }, { 0x4157A, 0 }, { 0x42596, 0 }, { 0x426C2, 2 }, { 0x42836, 0 },
                                                          { 0x40098, 0 }, { 0x405CE, 0 }, { 0x400C2, 0 }, { 0x432F8, 0 } };
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++) harness_add_stub(st[i].addr, st[i].n);

    int t, ended = 0;
    while (mismatches < (uint32_t)max_bad && (t = rep_read()) != 0) {
        if (t == 'N') {
            char lv[16]; int l = get16(rb + 4);
            snprintf(lv, sizeof lv, "%d", l ? l : 1); set_env("APEIRON_LEVEL", lv);
            gm_wr32(&game.ctx, 0xA01C2u, get32(rb));
            game_new(&game);
            sync68(); new_games++;
            printf("new game after step %u (the 68k copy starts from the port's new-game state)\n", steps);
        } else if (t == 'S') {
            int dx = get16(rb), dy = get16(rb + 2), fl = rb[4]; uint32_t ticks = get32(rb + 5);
            game.caps = (fl & 2) != 0;
            busy_n = 0; wait_used = 0; resync = 0;
            game_step(&game, dx, dy, fl & 1, ticks);
            steps++;
            if (wait_used || resync || game.ctx.quit) { sync68(); skipped++; if (getenv("DBG_SKIP")) printf("  not compared: step %u\n", steps); continue; }
            if (getenv("DBG") && steps < 4) printf("  seed before: 68k %08X port %08X\n", rd32(0xA01C2), gm_rd32(&game.ctx, 0xA01C2u));
            if (getenv("DBG_FRAMES")) audit_frames();
            run68k(dx, dy, (fl & 1) ^ (getenv("MUT") && steps % 50 == 0), ticks, game.caps);          /* MUT=1: a deliberate difference every 50 steps, to see that the comparison notices */
            if (getenv("DBG") && steps < 4) printf("  seed after: 68k %08X port %08X\n", rd32(0xA01C2), gm_rd32(&game.ctx, 0xA01C2u));
            compared++;
            if (busy_i != busy_n || busy_under) { printf("step %u: the 68k asked for %d sound checks, the port had %d\n", steps, busy_i + busy_under, busy_n); mismatches++; sync68(); continue; }
            if (compare()) { printf("step %u: the 68k code and the port differ (above)\n", steps); mismatches++; sync68(); }
        } else if (t == 'Z') {
            ended = 1;
            if (get32(rb) != sim_hash(&game) && !hash_bad) { hash_bad = 1; printf("the port ended in a different state from the recording\n"); }
        } else side(&game, t);
        if (game.ctx.quit) break;
    }
    printf("replay through the original 68k code: %u steps, %u compared, %u not compared (end of a round, pause or cheat keys: the 68k copy is reset to the port's state), %u new games, %u marks\n", steps, compared, skipped, new_games, marks);
    printf("%u steps where the state differed; the port %s%s\n", mismatches, hash_bad || stream_bad ? "did NOT stay in step with the recording" : "stayed in step with the recording", ended ? " to its end" : " (the recording has no end mark)");
    return mismatches || hash_bad || stream_bad ? 1 : 0;
}
