/* Apeiron creature move/frame callbacks, re-implemented from the original 68k code (CODE 3) and checked against it with Musashi
 * (verify/musashi/test_creatures.c).
 *
 * The functions work on a flat memory image laid out like the Mac original (sprite records and the playfield grid are addressed by
 * 32-bit "pointers", the application globals sit around A5), so the original data layouts, 16.16 fixed point and 16-bit wrap-around
 * are kept exactly. Position H (+0x2A) runs 0..0x1A0 and V (+0x2E) 0..0x1E0 in pixels; the grid is indexed [H/16][V/16].
 */
#ifndef APEIRON_CREATURES_H
#define APEIRON_CREATURES_H

#include <stddef.h>
#include <stdint.h>

#define MEM_MASK 0xFFFFFFu

/* Calls the original code makes to routines that are not ported yet (sound player, explosion and mushroom allocators). They are
 * reported with their raw stack arguments, first argument first, so a test can compare them with what the 68k code pushed. */
#define EXT_SOUND     1   /* (long soundId, short priority, long 0) */
#define EXT_EXPLOSION 2   /* (long sprite, long value)  = seg3_3E26 */
#define EXT_MUSHROOM  3   /* (short h cell, short v cell) = seg3_31A0 */
#define EXT_FIND_OVERLAP 4 /* (long sprite, long list) -> first sprite of the list overlapping `sprite`, or 0 = seg3_055E */

typedef struct GameCtx {
    uint8_t *mem;                 /* 16 MB image */
    uint32_t a5;                  /* A5 (globals are at negative offsets) */
    uint32_t (*ext)(void *ud, int kind, const uint8_t *raw, int n);   /* the result matters for EXT_FIND_OVERLAP only */
    void *ud;
    int native_overlap;           /* 1: overlap searches use engine_overlap instead of the EXT_FIND_OVERLAP callback */
    int reuse_memory;             /* 1: freed blocks are reused (the game); 0: plain bump allocation, as the test harness does */
    uint32_t free_size[8], free_head[8];
    uint32_t heap_next;           /* next free address of the memory standing in for the Mac heap (_NewPtr) */
    int native_endwave;           /* 1: the end-of-round routines (seg3_26C2, seg3_2836) run here; 0: they are logged calls (the test harness) */
    void (*wait)(void *ud);       /* called once per pass of those routines' own loops: show a frame, pace the loop, store the new Ticks at 0x16A (NULL: Ticks += 1) */
    int fix_coin;                 /* 1: a coin thrown straight up (no sideways speed) falls back, instead of rising to the top and staying there as in the original (apeiron.ini fix_coin) */
    int fix_glide;                /* 1: two heads that meet head-on at a side wall are not put on top of each other (they stayed locked together and slid down the wall); apeiron.ini fix_glide */
    int quit;                     /* set by the host when the user closes the window: the routines' loops stop */
} GameCtx;

/* memory access, big-endian, 24-bit addresses (like the 68000) */
uint8_t  gm_rd8(const GameCtx *g, uint32_t a);
uint16_t gm_rd16(const GameCtx *g, uint32_t a);
uint32_t gm_rd32(const GameCtx *g, uint32_t a);
void gm_wr8(GameCtx *g, uint32_t a, uint8_t v);
void gm_wr16(GameCtx *g, uint32_t a, uint16_t v);
void gm_wr32(GameCtx *g, uint32_t a, uint32_t v);

/* engine */
void sprite_deactivate(GameCtx *g, uint32_t s);              /* seg3_08DA */
void grid_remove_mushroom(GameCtx *g, int16_t h, int16_t v); /* seg3_3296 */
void sprite_move_default(GameCtx *g, uint32_t s);            /* seg3_0B0E */
void frame_next_to_count(GameCtx *g, uint32_t s);            /* seg3_0BD8, 4260, 492E, 5554: +1, wrap to 1 after +0x46 */
void frame_gecko(GameCtx *g, uint32_t s);                    /* seg3_4310 (8 frames) */
void frame_scorpion(GameCtx *g, uint32_t s);                 /* seg3_4BA6 (20 frames) */
void frame_psychedelic(GameCtx *g, uint32_t s);              /* seg3_47C2 (+4, wraps to 5) */
void frame_pede_body(GameCtx *g, uint32_t s);                /* seg3_51A4 */

/* creature move callbacks */
void move_gecko(GameCtx *g, uint32_t s);        /* seg3_4286 */
void move_scorpion(GameCtx *g, uint32_t s);     /* seg3_4330 */
void move_flea(GameCtx *g, uint32_t s);         /* seg3_40B4 */
void move_spaceship(GameCtx *g, uint32_t s);    /* seg3_4954 */
void move_coin(GameCtx *g, uint32_t s);         /* seg3_4AE6 */
void move_timed_bonus(GameCtx *g, uint32_t s);  /* seg3_4832 (multiplier) and seg3_47E8 (psychedelic mushroom) */
void move_bullet(GameCtx *g, uint32_t s);       /* seg3_4A9A */
void move_pede(GameCtx *g, uint32_t s);         /* seg3_4BC6: Pentipede head (phed) and body (pbod) segments */

/* game state updates; calls to routines not ported yet go through GameCtx.ext with kind = the address of the routine in the 68k image
 * (0x1000 and up; A-line traps use their opcode, jump-table routines A5 + offset) and their raw stack arguments */
void end_round_repair(GameCtx *g, uint16_t which);      /* seg3_26C2: round-end / respawn jingle, then every damaged mushroom restored in turn */
void cheat_key(GameCtx *g, uint8_t ch, const uint8_t codes[11][32]);   /* seg4_0378 in the paused mode: the typed character against the 11 code words */
void game_pause(GameCtx *g);                            /* seg3_2596: the pause (Caps Lock down) */
void end_round_bonus(GameCtx *g);                       /* seg3_2836: the multiplier and the bonus counted into the score */
void update_score_display(GameCtx *g);         /* seg3_1A72: score counting up, extra life every 20,000, lives and bonus display */
uint16_t step_update(GameCtx *g);              /* seg3_1BB6: the per-step update: timers, level limits, player hits, pick-ups, death and respawn */

/* effects and the callbacks of the sprites they create */
void frame_explosion(GameCtx *g, uint32_t s);      /* seg3_4B6A */
void move_score_digit(GameCtx *g, uint32_t s);     /* seg3_472E */
void move_fragment(GameCtx *g, uint32_t s);        /* seg3_44EE */
void frame_fragment(GameCtx *g, uint32_t s);       /* seg3_44BA */
void move_spark(GameCtx *g, uint32_t s);           /* seg3_46C8 */
void frame_spark(GameCtx *g, uint32_t s);          /* seg3_469A */
void spawn_explosion(GameCtx *g, uint32_t src, uint32_t cls);          /* seg3_3E26 */
void spawn_mushroom(GameCtx *g, int16_t h, int16_t v);                 /* seg3_31A0 */
void spawn_fragments(GameCtx *g, uint32_t a, uint32_t b);              /* seg3_3558 */
void spawn_sparks(GameCtx *g, uint32_t src);                           /* seg3_363A */
void spawn_score_digits(GameCtx *g, uint32_t src, int16_t value, int class_b);   /* seg3_3740 (class_b = 0), seg3_3854 (class_b = 1) */

/* shooter, shots, coins and what a shot hits */
void move_shooter(GameCtx *g, uint32_t s);         /* seg3_5216 */
void frame_shooter(GameCtx *g, uint32_t s);        /* seg3_54C8 */
void move_shot(GameCtx *g, uint32_t s);            /* seg3_54DA */
void move_yummy(GameCtx *g, uint32_t s);           /* seg3_454C */
void fire_shot(GameCtx *g);                        /* seg3_2256 */
void spawn_root(GameCtx *g, uint32_t src);         /* seg3_3968 */
void spawn_coins(GameCtx *g, uint32_t src, uint32_t cls);   /* seg3_3EF4 */
void spawn_special_shot(GameCtx *g, uint32_t src);          /* seg3_4010 */
void shot_hit(GameCtx *g, uint32_t shot, int consume);      /* seg3_557A */

void spawn_psychedelic(GameCtx *g);               /* seg3_3A36 */
void spawn_multiplier(GameCtx *g);                /* seg3_3B02 */
void spawn_fodder(GameCtx *g);                    /* seg3_3BD8 */

void spawn_fleas(GameCtx *g);                     /* seg3_3CEA */
void add_pede_head(GameCtx *g);                   /* seg3_42128: a reinforcement head after a touchdown */

void shield_start(GameCtx *g);                    /* seg3_2048 */
void move_shield(GameCtx *g, uint32_t s);         /* seg3_490E */
void frame_shield(GameCtx *g, uint32_t s);        /* seg3_4874 */
void house_clean(GameCtx *g);                     /* seg3_20CC */
void timed_launches(GameCtx *g);                  /* seg3_2334 */

void clear_playfield(GameCtx *g);                 /* seg3_30F6 */

void build_round(GameCtx *g);                     /* seg3_299A: set up everything for a new round or life */

void mark_mushrooms_under(GameCtx *g, uint32_t list);   /* seg3_0F2C */
extern int g_watch_reset, g_coll_log;
int coin_fix_test(GameCtx *g);                          /* test: APEIRON_COINFIXTEST */
void pede_fuzz_setup(GameCtx *g, uint32_t seed);        /* test: APEIRON_PEDEFUZZ */
int pede_log_open(const char *path);                  /* apeiron.ini pede_log: writes the report of fast descents to this file */
int pede_log_on(void);
void watch_pedes(GameCtx *g, uint32_t pass);          /* test: APEIRON_PEDEWATCH */
int game_pass(GameCtx *g);                        /* one pass of the in-game loop, seg3_123C */

/* sprite engine */
uint32_t gm_alloc(GameCtx *g, uint32_t size);
uint32_t engine_list_create(GameCtx *g, uint32_t tag);                               /* seg3_0712 */
uint32_t engine_sprite_alloc(GameCtx *g, uint32_t cls, uint32_t tag, uint32_t list);  /* seg3_0782 */
void engine_unlink(GameCtx *g, uint32_t list, uint32_t sprite);                      /* seg3_0884 */
void engine_activate(GameCtx *g, uint32_t sprite);                                   /* seg3_0902 */
uint32_t engine_overlap(GameCtx *g, uint32_t sprite, uint32_t list);                 /* seg3_055E */
void engine_move_pass(GameCtx *g);                                                   /* seg3_00EA */
void engine_draw_pass(GameCtx *g);
void engine_full_redraw(GameCtx *g);                                                /* seg3_05CE (host calls 0xC0B1, 0xC0B2) */                                                   /* seg3_020C.. */

#endif
