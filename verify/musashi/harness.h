/* Musashi differential harness for Apeiron 1.0.2.
 *
 * Maps the original 68k code exactly as apeiron_work/ghidra_scripts/BuildApeiron.java does:
 *   CODE n (1..9, header included) at 0x20000 + (n-1)*0x10000, a routine of the jump table at base + 4 + off;
 *   the A5 world (below-A5 globals, application parameters, jump table with every slot rewritten as JMP abs.l) at 0x200000,
 *   A5 = 0x200000 + "below".
 * Memory is a flat 16 MB array, so a C re-implementation and the original code can be run on identical memory and compared.
 */
#ifndef APEIRON_HARNESS_H
#define APEIRON_HARNESS_H

#include <stdint.h>
#include <stddef.h>

#define MEM_SIZE   0x1000000u
#define CODE_BASE  0x20000u
#define SEG_STRIDE 0x10000u
#define A5_WORLD   0x200000u
#define STACK_TOP  0x00F00000u
#define SENTINEL   0x00F80000u   /* return address that stops a call */

extern uint8_t g_mem[MEM_SIZE];
extern uint32_t g_a5;            /* A5 value */
extern uint32_t g_world_len;     /* size of the A5 world at A5_WORLD */

static inline uint32_t seg_base(int n) { return CODE_BASE + (uint32_t)(n - 1) * SEG_STRIDE; }

uint16_t rd16(uint32_t a);
uint32_t rd32(uint32_t a);
void wr16(uint32_t a, uint16_t v);
void wr32(uint32_t a, uint32_t v);

/* Loads code0..code9 from dir. Returns 0 on success. */
int image_load(const char *dir);
/* Runs the routine at `addr` with the given 16/32-bit stack arguments (pushed in call order, Pascal/C as the caller wants),
 * returns D0. `words` are pushed as given (each entry 16 bits when wide==0, 32 bits when wide==1). Max 8 args. */
uint32_t m68k_call(uint32_t addr, const uint32_t *args, const int *wide, int nargs, uint64_t max_instr);
extern uint64_t g_instr_count;

/* Stubs: a call to `addr` is not executed; it is logged with its raw stack arguments (nargs bytes after the return address) and
 * returns at once with D0 = 0. */
typedef struct { uint32_t addr; int n; uint8_t raw[16]; } CallRec;
extern CallRec g_calls[160];
extern int g_ncalls;
extern uint32_t (*g_stub_cb)(uint32_t addr, const uint8_t *raw);   /* if set: D0 returned by every stub */
void harness_clear_stubs(void);
void harness_add_stub(uint32_t addr, int nbytes);
void harness_set_stub_ret(uint32_t addr, uint32_t v);   /* D0 returned by a stub */
extern uint32_t g_extra_stack;     /* bytes of dummy stack between the return address and the stack top at entry (to start inside a function body) */
extern uint32_t g_heap_next;      /* next block _NewPtr hands out */
extern uint32_t g_stub_lo, g_stub_hi;   /* every address in [lo, hi) is a stub that returns at once */
extern uint32_t (*g_alloc_cb)(uint32_t size);
extern void (*g_free_cb)(uint32_t p);
extern int g_trap_hits;          /* A-line traps hit during the last call */
extern uint16_t g_last_trap;

#endif
