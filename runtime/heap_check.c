// TOTK_HEAP_CHECK: walks MUA3's general-purpose heaps at presents and reports the first block whose header is broken,
// so a buffer overrun is caught a frame after it happens instead of at a later free.
//
// The allocator (main+0x138cf60 allocates, main+0x138d760 frees) keeps a boundary tag in front of every block:
//   block+0x00  previous block in address order (0 for the first)
//   block+0x08  size of the block's user area: negative while in use, positive while free
//   block+0x10  used: the caller's allocation info (two words); free: the free list links
// The user pointer is block + hdr and the next block starts at block + hdr + |size|. The allocator object holds
// hdr at +0x18, the free-list heads at +0x20 (an array at the start of the heap, also at +0x38), the first block at
// +0x28 and the end of the heap at +0x30; the last block ends exactly there. MUA3 crashed (play 9) in free when a
// block's "previous" word held bytes that overran the block before it.
//
// TOTK_HEAP_CHECK=<n> walks every n-th present (1 = every present). TOTK_HEAP_CHECK_AT=<hex>[,...] names allocator
// objects; the default is the heap that crashed. Each walk holds the game's heap mutex (the lock the heap's virtual
// methods take), so it sees no half-done allocation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define MAX_HEAPS 8
#define LOCK_FN 0x700159b1c0ull     // the mutex lock the heap wrappers call (main+0x159b1c0)
#define UNLOCK_FN 0x700159b1d0ull
#define LOCK_ROOT 0x7002188460ull   // [[LOCK_ROOT]] + 0xa0 is the mutex
#define MAX_REPORTS 4

typedef struct {
    uint64_t obj;                   // allocator object
    int state;                      // 0 unchecked, 1 walking, -1 off (layout not understood or reported enough)
    int reports;
    uint64_t walks, last_blocks;
} Heap;

static Heap heaps[MAX_HEAPS];
static int nheaps, every = -1;
static uint64_t presents;

static void config(void) {
    every = 0;
    if (!RT_MUA3) return;
    const char *e = cfg_get("TOTK_HEAP_CHECK");
    if (!e || !*e || !strcmp(e, "0")) return;
    every = atoi(e) > 0 ? atoi(e) : 1;
    const char *at = cfg_get("TOTK_HEAP_CHECK_AT");
    if (!at || !*at) at = "7003d4e7d0";
    for (const char *p = at; *p && nheaps < MAX_HEAPS;) {
        char *end;
        uint64_t v = strtoull(p, &end, 16);
        if (end == p) break;
        heaps[nheaps++].obj = v;
        p = *end == ',' ? end + 1 : end;
    }
    rt_log("heap-check: every %d present(s), %d heap(s)", every, nheaps);
}

static uint64_t rd(uint64_t a) { return plat_mem_readable(a, 8) ? LD64(a) : 0xdeaddeaddeaddeadull; }

// A word that may be a pointer: what it points at, or a string when it points at printable text.
static void describe(uint64_t v, char *out, size_t n) {
    out[0] = 0;
    if (v < 0x10000 || v >= RT_ADDRESS_LIMIT || !plat_mem_readable(v, 1)) return;
    char where[128];
    ldr_describe(v, where, sizeof where);
    char s[49];
    int k = 0;
    while (k < 48 && plat_mem_readable(v + k, 1)) {
        uint8_t ch = LD8(v + k);
        if (!ch || ch < 0x20 || ch > 0x7e) break;
        s[k++] = (char)ch;
    }
    s[k] = 0;
    if (k >= 4 && (!plat_mem_readable(v + k, 1) || !LD8(v + k)))
        snprintf(out, n, " -> %s \"%s\"", where, s);
    else
        snprintf(out, n, " -> %s", where);
}

static void hexdump(uint64_t a, uint64_t n) {
    for (uint64_t o = 0; o < n; o += 16) {
        char line[160];
        int k = snprintf(line, sizeof line, "    %#llx:", (unsigned long long)(a + o));
        char asc[17];
        for (int i = 0; i < 16; i++) {
            uint8_t b = plat_mem_readable(a + o + i, 1) ? LD8(a + o + i) : 0;
            k += snprintf(line + k, sizeof line - k, "%s%02x", i == 8 ? "  " : " ", b);
            asc[i] = b >= 0x20 && b < 0x7f ? (char)b : '.';
        }
        asc[16] = 0;
        rt_log("%s  |%s|", line, asc);
    }
}

static void block_line(const char *what, uint64_t b, uint64_t hdr) {
    int64_t size = (int64_t)rd(b + 8);
    uint64_t i0 = rd(b + 0x10), i1 = rd(b + 0x18);
    char d0[256], d1[256];
    describe(i0, d0, sizeof d0);
    describe(i1, d1, sizeof d1);
    rt_log("heap-check:   %s block %#llx (user %#llx): prev %#llx, size %lld (%s), +0x10 %#llx%s, +0x18 %#llx%s", what,
           (unsigned long long)b, (unsigned long long)(b + hdr), (unsigned long long)rd(b),
           (long long)(size < 0 ? -size : size), size < 0 ? "in use" : "free", (unsigned long long)i0, d0,
           (unsigned long long)i1, d1);
}

static void report(Heap *h, uint64_t hdr, const uint64_t prev[3], uint64_t bad, const char *why) {
    rt_log("heap-check: heap %#llx corrupt at present %llu (walk %llu): %s", (unsigned long long)h->obj,
           (unsigned long long)presents, (unsigned long long)h->walks, why);
    for (int i = 2; i >= 0; i--)
        if (prev[i]) block_line(i == 0 ? "last good" : "earlier", prev[i], hdr);
    rt_log("heap-check:   broken block at %#llx; the last good block's user area ends there. Bytes around it:",
           (unsigned long long)bad);
    uint64_t from = bad >= 0x80 ? bad - 0x80 : bad;
    if (prev[0] && prev[0] + hdr > from) from = prev[0] + hdr;
    hexdump(from & ~15ull, (bad - (from & ~15ull)) + 0x40);
    if (++h->reports >= MAX_REPORTS) h->state = -1;
}

// Walks one heap; returns blocks seen, or 0 when the layout is not understood (logged once).
static uint64_t walk(Heap *h) {
    uint64_t a = h->obj, hdr = rd(a + 0x18), first = rd(a + 0x28), end = rd(a + 0x30);
    if (hdr == 0 || hdr > 0x100 || (hdr & 7) || !first || end <= first || end - first > (32ull << 30) ||
        !plat_mem_readable(first, 16)) {
        if (h->state == 0) {
            rt_log("heap-check: %#llx does not look like the heap allocator (hdr %#llx, first %#llx, end %#llx); off",
                   (unsigned long long)a, (unsigned long long)hdr, (unsigned long long)first, (unsigned long long)end);
            hexdump(a, 0x60);
            h->state = -1;
        }
        return 0;
    }
    // The first block's "previous" word holds the last block (the chain is circular); it is checked after the walk.
    uint64_t prev[3] = {0, 0, 0}, b = first, n = 0, first_prev = rd(first);
    while (b < end) {
        char why[192];
        uint64_t p = rd(b);
        int64_t size = (int64_t)rd(b + 8);
        uint64_t mag = (uint64_t)(size < 0 ? -size : size);
        if (n && p != prev[0]) {
            snprintf(why, sizeof why, "block %#llx says its previous block is %#llx, the walk came from %#llx",
                     (unsigned long long)b, (unsigned long long)p, (unsigned long long)prev[0]);
            report(h, hdr, prev, b, why);
            return n;
        }
        if (mag > end - b - hdr) {
            snprintf(why, sizeof why, "block %#llx has size %lld, past the end of the heap %#llx", (unsigned long long)b,
                     (long long)size, (unsigned long long)end);
            report(h, hdr, prev, b, why);
            return n;
        }
        if (size >= 0) {
            uint64_t l0 = rd(b + 0x10), l1 = rd(b + 0x18);
            if ((l0 && (l0 < first || l0 >= end)) || (l1 && (l1 < first || l1 >= end))) {
                snprintf(why, sizeof why, "free block %#llx has free-list links %#llx / %#llx outside the heap",
                         (unsigned long long)b, (unsigned long long)l0, (unsigned long long)l1);
                report(h, hdr, prev, b, why);
                return n;
            }
        }
        prev[2] = prev[1];
        prev[1] = prev[0];
        prev[0] = b;
        b += hdr + mag;
        n++;
    }
    if (b != end && h->state == 0) {
        rt_log("heap-check: heap %#llx: the walk from %#llx ends at %#llx, not at %#llx; off", (unsigned long long)a,
               (unsigned long long)first, (unsigned long long)b, (unsigned long long)end);
        hexdump(a, 0x60);
        h->state = -1;
        return 0;
    }
    if (first_prev && first_prev != prev[0]) {
        if (h->state == 0) {
            rt_log("heap-check: heap %#llx: the first block's previous word %#llx is not the last block %#llx; off",
                   (unsigned long long)a, (unsigned long long)first_prev, (unsigned long long)prev[0]);
            h->state = -1;
            return 0;
        }
        char why[160];
        snprintf(why, sizeof why, "the first block's link to the last block is %#llx, the last block is %#llx",
                 (unsigned long long)first_prev, (unsigned long long)prev[0]);
        report(h, hdr, prev, first, why);
    }
    return n;
}

void heap_check_present(Cpu *c) {
    if (every < 0) config();
    if (!every || ++presents % (uint64_t)every) return;
    uint64_t root = rd(LOCK_ROOT);
    uint64_t lk = root ? rd(root) : 0;
    if (!lk || lk == 0xdeaddeaddeaddeadull) return;   // the game has not made its heap lock yet
    uint64_t mtx = lk + 0xa0;
    for (int i = 0; i < nheaps; i++) {
        Heap *h = &heaps[i];
        if (h->state < 0) continue;
        rt_call(c, LOCK_FN, 1, mtx);
        uint64_t n = walk(h);
        rt_call(c, UNLOCK_FN, 1, mtx);
        if (h->state < 0) continue;
        h->walks++;
        if (h->state == 0 && n) {
            h->state = 1;
            rt_log("heap-check: heap %#llx: hdr %#llx, %#llx..%#llx, %llu blocks", (unsigned long long)h->obj,
                   (unsigned long long)rd(h->obj + 0x18), (unsigned long long)rd(h->obj + 0x20),
                   (unsigned long long)rd(h->obj + 0x30), (unsigned long long)n);
        }
        if (n) h->last_blocks = n;
        if (h->walks % 1800 == 0)
            rt_log("heap-check: heap %#llx: %llu walks clean, %llu blocks", (unsigned long long)h->obj,
                   (unsigned long long)h->walks, (unsigned long long)h->last_blocks);
    }
}
