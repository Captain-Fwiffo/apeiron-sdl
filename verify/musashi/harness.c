#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"

uint8_t g_mem[MEM_SIZE];
uint32_t g_a5;
uint32_t g_world_len;
uint64_t g_instr_count;
int g_trap_hits;
uint16_t g_last_trap;

/* ---- memory callbacks (big-endian, out-of-range reads give 0, writes are dropped) ---- */
unsigned int m68k_read_memory_8(unsigned int a) { return a < MEM_SIZE ? g_mem[a] : 0; }
unsigned int m68k_read_memory_16(unsigned int a) { return a + 1 < MEM_SIZE ? (unsigned)(g_mem[a] << 8 | g_mem[a + 1]) : 0; }
unsigned int m68k_read_memory_32(unsigned int a) { return a + 3 < MEM_SIZE ? ((unsigned)g_mem[a] << 24 | g_mem[a + 1] << 16 | g_mem[a + 2] << 8 | g_mem[a + 3]) : 0; }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { if (a < MEM_SIZE) g_mem[a] = (uint8_t)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { if (a + 1 < MEM_SIZE) { g_mem[a] = (uint8_t)(v >> 8); g_mem[a + 1] = (uint8_t)v; } }
void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    if (a + 3 < MEM_SIZE) { g_mem[a] = (uint8_t)(v >> 24); g_mem[a + 1] = (uint8_t)(v >> 16); g_mem[a + 2] = (uint8_t)(v >> 8); g_mem[a + 3] = (uint8_t)v; }
}

uint16_t rd16(uint32_t a) { return (uint16_t)m68k_read_memory_16(a); }
uint32_t rd32(uint32_t a) { return m68k_read_memory_32(a); }
void wr16(uint32_t a, uint16_t v) { m68k_write_memory_16(a, v); }
void wr32(uint32_t a, uint32_t v) { m68k_write_memory_32(a, v); }

/* ---- instruction hook: stops at the sentinel, counts instructions and skips A-line traps (recorded) ---- */
static uint64_t s_limit;
static int s_done;

CallRec g_calls[160];
int g_ncalls;
static struct { uint32_t addr; int n; uint32_t ret; } s_stubs[2048];
uint32_t (*g_stub_cb)(uint32_t addr, const uint8_t *raw);
static int s_nstubs;
static int16_t s_hash[8192];
static unsigned hash_of(uint32_t a) { return (a * 2654435761u) >> 19; }
void harness_clear_stubs(void) { s_nstubs = 0; memset(s_hash, 0, sizeof s_hash); }
void harness_add_stub(uint32_t addr, int nbytes) { s_stubs[s_nstubs].addr = addr; s_stubs[s_nstubs].n = nbytes; s_stubs[s_nstubs].ret = 0; { unsigned h = hash_of(addr); while (s_hash[h & 8191]) h++; s_hash[h & 8191] = (int16_t)(s_nstubs + 1); } s_nstubs++; }
void harness_set_stub_ret(uint32_t addr, uint32_t v) { for (int i = 0; i < s_nstubs; i++) if (s_stubs[i].addr == addr) s_stubs[i].ret = v; }

int g_trace;
uint32_t g_stub_lo, g_stub_hi;
uint32_t (*g_alloc_cb)(uint32_t size);      /* if set: _NewPtr and _DisposPtr go through these (a whole-game replay uses the allocator of the port) */
void (*g_free_cb)(uint32_t p);
uint32_t g_heap_next = 0x500000;
uint32_t g_extra_stack;
void harness_instr_hook(unsigned int pc)
{
    if (g_trace && g_instr_count < 100000) printf("  pc=%06X\n", pc);
    if (pc == SENTINEL) { s_done = 1; m68k_end_timeslice(); return; }
    if (g_stub_hi && pc >= g_stub_lo && pc < g_stub_hi) {        /* a range of addresses that are all stubs (the port's drawing routines), each just returns */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
        m68k_set_reg(M68K_REG_D0, 0); m68k_set_reg(M68K_REG_SP, sp + 4); m68k_set_reg(M68K_REG_PC, rd32(sp));
        return;
    }
    g_instr_count++;
    if (g_instr_count > s_limit) { s_done = 2; m68k_end_timeslice(); return; }
    for (unsigned hh = hash_of(pc); s_hash[hh & 8191]; hh++) {
        int i = s_hash[hh & 8191] - 1;
        if (s_stubs[i].addr == pc) {
            uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
            if (g_ncalls < 160) {
                CallRec *c = &g_calls[g_ncalls++];
                c->addr = pc; c->n = s_stubs[i].n;
                if (c->n == -2 || c->n == -3) {           /* register stubs: A0, A1, D0, D1 (or A0, D0) instead of stack arguments */
                    uint32_t r[4] = { m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1) };
                    int full = c->n == -2;
                    uint32_t v[4] = { r[0], full ? r[1] : r[2], full ? r[2] : 0, full ? r[3] : 0 };
                    c->n = full ? 16 : 8;
                    for (int k = 0; k < c->n / 4; k++) { c->raw[k * 4] = (uint8_t)(v[k] >> 24); c->raw[k * 4 + 1] = (uint8_t)(v[k] >> 16); c->raw[k * 4 + 2] = (uint8_t)(v[k] >> 8); c->raw[k * 4 + 3] = (uint8_t)v[k]; }
                } else {
                    for (int k = 0; k < c->n; k++) c->raw[k] = g_mem[(sp + 4 + k) & 0xFFFFFF];
                }
            }
            m68k_set_reg(M68K_REG_D0, g_stub_cb ? g_stub_cb(pc, g_ncalls ? g_calls[g_ncalls - 1].raw : 0) : s_stubs[i].ret);
            m68k_set_reg(M68K_REG_SP, sp + 4);
            m68k_set_reg(M68K_REG_PC, rd32(sp));
            return;
        }
    }
    uint16_t op = rd16(pc);
    if (op == 0xA11E) {                                   /* _NewPtr: D0 = size, returns A0 = block, D0 = 0; a bump allocator */
        uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
        if (g_alloc_cb) m68k_set_reg(M68K_REG_A0, g_alloc_cb(size));
        else {
            m68k_set_reg(M68K_REG_A0, g_heap_next);
            g_heap_next += (size + 3) & ~3u;
        }
        m68k_set_reg(M68K_REG_D0, 0);
        m68k_set_reg(M68K_REG_PC, pc + 2);
        return;
    }
    if (op == 0xA8A8) {                                   /* _OffsetRect(r, dh, dv): Pascal, pops 8 bytes */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
        int16_t dv = (int16_t)rd16(sp), dh = (int16_t)rd16(sp + 2);
        uint32_t r = rd32(sp + 4);
        wr16(r, (uint16_t)(rd16(r) + dv)); wr16(r + 2, (uint16_t)(rd16(r + 2) + dh));
        wr16(r + 4, (uint16_t)(rd16(r + 4) + dv)); wr16(r + 6, (uint16_t)(rd16(r + 6) + dh));
        m68k_set_reg(M68K_REG_SP, sp + 8);
        m68k_set_reg(M68K_REG_PC, pc + 2);
        return;
    }
    if (op == 0xA8A7) {                                   /* _SetRect(r, left, top, right, bottom): Pascal, pops 12 bytes */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
        uint16_t bottom = rd16(sp), right = rd16(sp + 2), top = rd16(sp + 4), left = rd16(sp + 6);
        uint32_t r = rd32(sp + 8);
        wr16(r, top); wr16(r + 2, left); wr16(r + 4, bottom); wr16(r + 6, right);
        m68k_set_reg(M68K_REG_SP, sp + 12);
        m68k_set_reg(M68K_REG_PC, pc + 2);
        return;
    }
    if (op == 0xA05D) { m68k_set_reg(M68K_REG_PC, pc + 2); return; }   /* _SwapMMUMode: nothing to do */
    if (op == 0xA01F) {                                   /* _DisposPtr: A0 = block; logged, not freed */
        if (g_free_cb) g_free_cb(m68k_get_reg(NULL, M68K_REG_A0));
        if (g_ncalls < 160) {
            CallRec *c = &g_calls[g_ncalls++];
            uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
            c->addr = op; c->n = 4;
            c->raw[0] = (uint8_t)(a0 >> 24); c->raw[1] = (uint8_t)(a0 >> 16); c->raw[2] = (uint8_t)(a0 >> 8); c->raw[3] = (uint8_t)a0;
        }
        m68k_set_reg(M68K_REG_PC, pc + 2);
        return;
    }
    if ((op & 0xF000) == 0xA000) {
        if (g_ncalls < 160) {
            CallRec *c = &g_calls[g_ncalls++];
            uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
            c->addr = op; c->n = 8;
            for (int k = 0; k < 8; k++) c->raw[k] = g_mem[(sp + k) & 0xFFFFFF];
        }
        g_trap_hits++;
        g_last_trap = op;
        if (op == 0xAA3F || op == 0xA855) m68k_set_reg(M68K_REG_SP, m68k_get_reg(NULL, M68K_REG_SP) + 8);   /* _SetEntries is a Pascal procedure: it pops its 8 bytes of arguments */
        m68k_set_reg(M68K_REG_PC, pc + 2);
    }
}

static int read_file(const char *dir, const char *name, uint8_t **buf, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    *buf = (uint8_t *)malloc(*len ? *len : 1);
    if (fread(*buf, 1, *len, f) != *len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int image_load(const char *dir)
{
    memset(g_mem, 0, sizeof g_mem);
    uint8_t *c0; size_t l0;
    if (read_file(dir, "code0.bin", &c0, &l0)) return -1;
    uint32_t above = (uint32_t)(c0[0] << 24 | c0[1] << 16 | c0[2] << 8 | c0[3]);
    uint32_t below = (uint32_t)(c0[4] << 24 | c0[5] << 16 | c0[6] << 8 | c0[7]);
    uint32_t jt_off = (uint32_t)(c0[12] << 24 | c0[13] << 16 | c0[14] << 8 | c0[15]);
    g_a5 = A5_WORLD + below;
    g_world_len = above + below;
    for (int n = 1; n <= 9; n++) {
        char nm[32]; uint8_t *b; size_t l;
        snprintf(nm, sizeof nm, "code%d.bin", n);
        if (read_file(dir, nm, &b, &l)) return -1;
        memcpy(&g_mem[seg_base(n)], b, l);
        free(b);
    }
    size_t nent = (l0 - 16) / 8;
    for (size_t i = 0; i < nent; i++) {
        uint32_t off = (uint32_t)(c0[16 + i * 8] << 8 | c0[17 + i * 8]);
        uint32_t seg = (uint32_t)(c0[20 + i * 8] << 8 | c0[21 + i * 8]);
        uint32_t target = seg_base((int)seg) + 4 + off;
        uint32_t p = A5_WORLD + below + jt_off + (uint32_t)i * 8;
        wr16(p + 2, 0x4EF9); wr32(p + 4, target);   /* a jsr d(A5) lands at entry + 2; entry + 0 holds the segment offset */
    }
    (void)above;
    free(c0);
    return 0;
}

uint32_t m68k_call(uint32_t addr, const uint32_t *args, const int *wide, int nargs, uint64_t max_instr)
{
    static int inited;
    if (!inited) { m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68020); inited = 1; }
    m68k_pulse_reset();
    uint32_t sp = STACK_TOP;
    for (int i = nargs - 1; i >= 0; i--) {
        if (wide[i]) { sp -= 4; wr32(sp, args[i]); }
        else { sp -= 2; wr16(sp, (uint16_t)args[i]); }
    }
    sp -= 4; wr32(sp, SENTINEL);
    sp -= g_extra_stack;
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_A5, g_a5);
    m68k_set_reg(M68K_REG_PC, addr);
    g_instr_count = 0; g_trap_hits = 0; g_ncalls = 0; s_done = 0; s_limit = max_instr;
    while (!s_done) m68k_execute(100000);
    if (s_done == 2) fprintf(stderr, "call at %06X ran past %llu instructions\n", addr, (unsigned long long)max_instr);
    return m68k_get_reg(NULL, M68K_REG_D0);
}
