/*
 * abk_code.c - run the lifted copies of the routines in EA's sound banks.
 * Shared by the Windows host and the Android runtime; see nfs_abk.h.
 *
 * A routine is identified by content, not by address: the bank lives wherever
 * the game's allocator put it, and the same block appears in many banks. The
 * whole block around the called address must equal the local bank's bytes,
 * except the call operands the game patched. A small cache keeps the hot
 * callbacks (one per sound tick) off the slow path; a hit is re-checked
 * against the whole block. Checking only the routine's first 32 bytes was not
 * enough: engine banks of different cars (car_00_eng, car_77_eng, ...) share
 * routine prefixes, and after a car change loaded another bank at the same
 * address the old car's routine walked the new voice data with the wrong
 * layout (Carbon crash in 0x89819F reading a garbage curve pointer).
 */
#include <stdlib.h>
#include <string.h>
#include "nfs_abk.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int claim(volatile long *s) { return InterlockedCompareExchange(s, 1, 0) == 0; }
static void publish(volatile long *s) { InterlockedExchange(s, 2); }
static int ready(volatile long *s) { return InterlockedCompareExchange(s, 2, 2) == 2; }
static void pause_(void) { SwitchToThread(); }
#else
#include <sched.h>
static int claim(volatile long *s) { long z = 0; return __atomic_compare_exchange_n(s, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
static void publish(volatile long *s) { __atomic_store_n(s, 2, __ATOMIC_RELEASE); }
static int ready(volatile long *s) { return __atomic_load_n(s, __ATOMIC_ACQUIRE) == 2; }
static void pause_(void) { sched_yield(); }
#endif

typedef struct { uint32_t block, offset; recomp_func_t fn; } routine_t;
static routine_t *g_routines;
static unsigned g_nroutines;
static uint8_t **g_masks;          /* per block: 1 where the game patches the byte */
typedef struct { uint32_t from, to; } span_t;
static span_t **g_spans;           /* per block: unpatched byte ranges, for memcmp */
static unsigned *g_nspans;

static void build(void) {
    g_masks = (uint8_t **)calloc(nfs_abk_block_count, sizeof *g_masks);
    g_routines = (routine_t *)calloc(abk_recomp_dispatch_count, sizeof *g_routines);
    g_spans = (span_t **)calloc(nfs_abk_block_count, sizeof *g_spans);
    g_nspans = (unsigned *)calloc(nfs_abk_block_count, sizeof *g_nspans);
    for (unsigned b = 0; b < nfs_abk_block_count; b++) {
        const nfs_abk_block_t *k = &nfs_abk_blocks[b];
        g_masks[b] = (uint8_t *)calloc(k->len, 1);
        for (uint32_t i = 0; i < k->nrel; i++) memset(g_masks[b] + k->rel[i], 1, 4);
        g_spans[b] = (span_t *)calloc(k->nrel + 1, sizeof **g_spans);
        for (uint32_t i = 0; i < k->len;) {
            while (i < k->len && g_masks[b][i]) i++;
            uint32_t from = i;
            while (i < k->len && !g_masks[b][i]) i++;
            if (i > from) g_spans[b][g_nspans[b]++] = (span_t){ from, i };
        }
        for (uint32_t i = 0; i < abk_recomp_dispatch_count; i++) {
            uint32_t a = abk_recomp_dispatch_table[i].address;
            if (a >= k->va && a < k->va + k->len)
                g_routines[g_nroutines++] = (routine_t){ b, a - k->va, abk_recomp_dispatch_table[i].func };
        }
    }
}

/* guest [base, base+from..to) equals the block there, patched operands aside */
static int same(uint32_t base, unsigned b, uint32_t from, uint32_t to) {
    const nfs_abk_block_t *k = &nfs_abk_blocks[b];
    for (uint32_t i = from; i < to; i++)
        if (!g_masks[b][i] && MEM8(base + i) != k->code[i]) return 0;
    return 1;
}

/* the whole block at guest base equals block b, patched operands aside */
static int same_block(uint32_t base, unsigned b) {
    const nfs_abk_block_t *k = &nfs_abk_blocks[b];
    for (unsigned s = 0; s < g_nspans[b]; s++) {
        const span_t *p = &g_spans[b][s];
        if (memcmp((const void *)ADDR(base + p->from), k->code + p->from, p->to - p->from)) return 0;
    }
    return 1;
}

#define CACHE 64
static struct { uint32_t va; uint32_t r; } g_cache[CACHE];

recomp_func_t nfs_abk_lookup(uint32_t va) {
    static volatile long state;
    if (!ready(&state)) {
        if (claim(&state)) { build(); publish(&state); }
        else while (!ready(&state)) pause_();
    }
    unsigned h = (va >> 2) % CACHE;
    uint32_t cva = g_cache[h].va, cr = g_cache[h].r;
    if (cva == va && cr < g_nroutines) {
        const routine_t *r = &g_routines[cr];
        uint32_t base = va - r->offset;
        if (nfs_guest_readable(base, nfs_abk_blocks[r->block].len) && same_block(base, r->block)) return r->fn;
    }
    if (!nfs_guest_readable(va, 1)) return NULL;
    for (unsigned i = 0; i < g_nroutines; i++) {
        const routine_t *r = &g_routines[i];
        const nfs_abk_block_t *k = &nfs_abk_blocks[r->block];
        if (!g_masks[r->block][r->offset] && MEM8(va) != k->code[r->offset]) continue;
        uint32_t base = va - r->offset;
        if (!nfs_guest_readable(base, k->len) || !same(base, r->block, 0, k->len)) continue;
        g_cache[h].va = 0;            /* never pair a new va with a stale routine */
        g_cache[h].r = i;
        g_cache[h].va = va;
        return r->fn;
    }
    return NULL;
}
