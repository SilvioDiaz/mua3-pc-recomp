// Function-entry profiler, adapted from LAS Recomp runtime/fnprof.c (MIT, Copyright (c) 2026 Peiikai).
//
// TOTK_FNPROF=<from>:<to> counts guest function entries (every generated function starts with FN_ENTER, cpu.h)
// between those presented frames, then writes TOTK_FNPROF_OUT (default fnprof.txt in the working folder): one line
// per function, most called first: "<addr> <count> <module+offset (symbol)>". Diffing two runs shows which game
// functions a change brings in or leaves out; the counts also show hot code. TOTK_FNPROF=0:<to> starts at process
// start, so threads the game starts before the first frame are counted too.
//
// TOTK_FNPROF_BT=<hex addr>[,...]: the first time each listed function is entered inside the window, log who called
// it (guest lr) and its x0 with the first words it points to (vtable, [x0+8]).
//
// g_fnprof is shared with resolution.c: bit 0 is its render-size watch (res_fn_hit), bit 1 is this profiler.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

volatile int g_fnprof = 0;

#define NSLOT (1 << 18)
static struct { _Atomic uint64_t addr; _Atomic uint64_t count; } slots[NSLOT];
static _Atomic int dropped;   // the table was full

static uint64_t bt_addrs[256];
static int bt_n = -1;
static _Atomic uint8_t bt_done[256];

static int guest_ptr(uint64_t p) { return p >= 0x1000000ull && p < (1ull << 39) && !(p & 7); }

static void bt_check(uint64_t a, uint64_t lr, uint64_t x0) {
    for (int i = 0; i < bt_n; i++)
        if (bt_addrs[i] == a && !atomic_exchange(&bt_done[i], 1)) {
            char at[128], from[128];
            uint64_t vt = guest_ptr(x0) ? *(uint64_t *)(uintptr_t)x0 : 0;
            uint64_t p8 = guest_ptr(x0) ? *(uint64_t *)(uintptr_t)(x0 + 8) : 0;
            rt_log("fnprof-bt: %s entered from %s, x0 %#llx (vtable %#llx, [x0+8] %#llx)",
                   ldr_describe(a, at, sizeof at), ldr_describe(lr - 4, from, sizeof from), (unsigned long long)x0,
                   (unsigned long long)vt, (unsigned long long)p8);
        }
}

static void prof_hit(uint64_t a, uint64_t lr, uint64_t x0) {
    if (bt_n) bt_check(a, lr, x0);
    uint32_t h = (uint32_t)((a >> 2) * 0x9E3779B1u) & (NSLOT - 1);
    for (int n = 0; n < NSLOT; n++, h = (h + 1) & (NSLOT - 1)) {
        uint64_t cur = atomic_load_explicit(&slots[h].addr, memory_order_relaxed);
        if (cur == 0 && atomic_compare_exchange_strong(&slots[h].addr, &cur, a)) cur = a;
        if (cur == a) {
            atomic_fetch_add_explicit(&slots[h].count, 1, memory_order_relaxed);
            return;
        }
    }
    atomic_store(&dropped, 1);
}

void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    int on = g_fnprof;
    if (on & 1) res_fn_hit(addr, x0);
    if (on & 2) prof_hit(addr, lr, x0);
}

static long prof_from = -2, prof_to;

static void prof_config(void) {
    if (prof_from != -2) return;
    prof_from = -1;
    const char *e = getenv("TOTK_FNPROF");
    if (e && (sscanf(e, "%ld:%ld", &prof_from, &prof_to) != 2 || prof_to <= prof_from)) {
        rt_log("fnprof: TOTK_FNPROF=%s is not <from>:<to> with to > from; profiler off", e);
        prof_from = -1;
    }
    bt_n = 0;
    for (const char *p = getenv("TOTK_FNPROF_BT"); p && *p && bt_n < 256;) {
        char *end;
        uint64_t v = strtoull(p, &end, 16);
        if (end == p) break;
        bt_addrs[bt_n++] = v;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
}

static int by_count(const void *x, const void *y) {
    uint64_t a = ((const uint64_t *)x)[1], b = ((const uint64_t *)y)[1];
    return a < b ? 1 : a > b ? -1 : 0;
}

// Called by the presenting thread with the number of the frame just presented (vk_backend.c).
void fnprof_frame(uint64_t frame) {
    prof_config();
    if (prof_from < 0) return;
    if ((long)frame == prof_from && prof_from > 0) {
        __atomic_or_fetch(&g_fnprof, 2, __ATOMIC_SEQ_CST);
        rt_log("fnprof: on at frame %llu", (unsigned long long)frame);
    }
    if ((long)frame != prof_to) return;
    __atomic_and_fetch(&g_fnprof, ~2, __ATOMIC_SEQ_CST);
    uint64_t (*rows)[2] = malloc(sizeof(uint64_t[2]) * NSLOT);
    int n = 0;
    for (int i = 0; rows && i < NSLOT; i++)
        if (slots[i].addr) { rows[n][0] = slots[i].addr; rows[n][1] = slots[i].count; n++; }
    if (rows) qsort(rows, n, sizeof rows[0], by_count);
    const char *out = getenv("TOTK_FNPROF_OUT") ? getenv("TOTK_FNPROF_OUT") : "fnprof.txt";
    FILE *f = fopen(out, "w");
    char where[160];
    for (int i = 0; f && i < n; i++)
        fprintf(f, "%llx %llu %s\n", (unsigned long long)rows[i][0], (unsigned long long)rows[i][1],
                ldr_describe(rows[i][0], where, sizeof where));
    if (f) fclose(f);
    free(rows);
    rt_log("fnprof: %d functions written to %s%s (frames %ld-%ld)", n, f ? out : "(open failed)",
           atomic_load(&dropped) ? ", table full: some functions not counted" : "", prof_from, prof_to);
    prof_from = -1;
}

__attribute__((constructor)) static void fnprof_early(void) {
    prof_config();
    if (prof_from == 0) __atomic_or_fetch(&g_fnprof, 2, __ATOMIC_SEQ_CST);
}
