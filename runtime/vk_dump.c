// TOTK_DUMP=<frame>: a frame dump for the Vulkan back end, which RenderDoc cannot capture (it hides
// VK_EXT_external_memory_host). Between presents frame-1 and frame, the replay's command buffers also copy, after each
// render pass, its colour and depth targets and, after each dispatch, the storage images it writes into host buffers;
// the completion thread turns them into 8-bit PNGs once the submission is done. 3D textures (LUTs, volumes) sampled or
// written in the frame are copied once each, slices side by side. Files in TOTK_DUMP_DIR (default "dump"):
//   pass_<seq>_t<i>_<w>x<h>_<vkformat>_<name>.png   colour target i (td: depth; img<slot>: storage image; lut<id>: 3D;
//                                                    a BC target drawn through a block view: its blocks as that view)
//   ..._tm.png                                       float formats: x/(1+x) of the same image (the plain one clamps)
//   passes.txt   per pass or dispatch: targets, draws, each distinct pipeline + texture set with its shaders' SPIR-V
//                hashes (sh_<hash>.spv written once) and the bound textures with their samplers
//   images.txt   per image: channel min/max, distinct texel values over the image and over its top quarter
// Sampled 8-bit 2D textures up to TEX2D_CAP texels are copied once each as pass_<seq>_tex<id>_... at the end of the pass
// that samples them; texture lines carry the storage address, mirror state, upload count and the image's and slot's
// swizzle. Copies and texture clears are logged as "copy/clear after pass N". TOTK_DUMP_LOG=<from> also logs the
// frames from..frame-1 to passes.txt, with PNGs only of targets up to 64K texels. TOTK_DUMP_BUF=<addr>:<size>[:...]
// snapshots one GPU buffer range after every logged pass (buf_<pass>.bin and bufs.txt, see buf_snap).
// PNGs are opaque (alpha dropped). Integer formats are scaled by the image maximum, depth by its min..max range, SNORM
// as x/2+1/2, UNORM and sRGB bytes written as stored. Caps: MAX_PASSES passes copied, PNG_CAP bytes of PNGs.
// TOTK_DUMP_TEX=<id>|<w>x<h>[,...]: those texture slots, or 2D textures of that size, are copied where first sampled,
// whatever their format.
// TOTK_DUMP_UBO=<n>: each listed pipeline also prints the first n vec4 of its user uniform blocks (c3 and up; 1 means 16,
// the cap is 4096), read from guest memory when the entry is listed. A block the submission rewrites with UpdateUniformBuffer shows its value from
// before the submission (vk_exec_draw.c upd_note).
// TOTK_DUMP_SKIP_RUNS=1: a render pass with the same targets as the render pass before it is logged but not copied
// (the terrain decals draw one pass each into the G-buffer and would use up both caps).
// Depth-stencil targets also give their stencil (..._tds_..._s8.png). Multisampled colour targets are resolved into a
// temporary image first. BC textures and BC targets drawn through block views are decoded (bcdec) instead of being
// shown as raw blocks.
// TOTK_DUMP_RAW (default 1): every image whose format holds more than 8 bits per channel (float, 16/32-bit, depth,
// 10-bit, integer) is also written as <name>.raw, its texels exactly as the GPU stores them, rows packed, slices one after
// the other; images.txt names the file, its size and format (tools/dump_raw.py reads texels from it). BC6H textures
// write their decoded texels as rgba32f. RAW_CAP bytes at most.
// TOTK_DUMP_PASS=<item>[,...]: detail for chosen passes. An item is a pass number as passes.txt prints it, or sh:<hash>
// for every draw or dispatch whose shader (vs, fs or cs) has that SPIR-V hash. A chosen render pass copies its targets
// before its first draw (pass_<n>_before_...); every chosen draw copies the targets after it (pass_<n>_d<draw>_...), its
// vertex streams, index buffer, indirect arguments and the storage buffers its stages bind (.bin, BIN_ONE_CAP bytes
// each), and writes its full state, its uniform blocks (64 vec4 each) and its textures to draws.txt; the textures it
// samples are copied whatever their format. A chosen dispatch copies the storage buffers it binds, after it runs.
// DRAW_SNAP_MAX draws are snapshotted at most; to do it the replay ends the pass and begins it again around the copies.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

// bcdec (MIT or Unlicense, see third_party/bcdec/LICENSE): the BC1-BC7 block decoders for the dump's PNGs
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "third_party/bcdec/bcdec.h"
#pragma clang diagnostic pop

#define MAX_PASSES 400
#define PNG_CAP (2ull << 30)
#define INFLIGHT_CAP (3ull << 30)   // host buffers waiting for their submission
#define MAX_ENTS 48                 // distinct pipeline + texture sets listed per pass
#define MAX_LUTS 256                // 3D textures and small sampled 8-bit 2D textures dumped per frame
#define TEX2D_CAP (1u << 20)        // texels of a sampled 2D texture that is dumped
#define RAW_CAP (2ull << 30)        // bytes of .raw files (TOTK_DUMP_RAW)
#define BIN_CAP (1ull << 30)        // bytes of buffer copies (.bin) of chosen draws and dispatches
#define BIN_ONE_CAP (16ull << 20)   // bytes of one such buffer; a larger one is cut
#define DRAW_SNAP_MAX 512           // draws snapshotted (TOTK_DUMP_PASS)

int vk_dump_armed;
static uint64_t dump_frame, log_from;   // log_from: TOTK_DUMP_LOG, first frame logged to passes.txt without images
static char dir[400];
static FILE *passes_f, *images_f;
static int seq, finished, copied;      // copied: passes and dispatches of the dump frame whose images were copied
static int skip_runs, dump_ubo;        // TOTK_DUMP_SKIP_RUNS, TOTK_DUMP_UBO (vec4 per block, 0 for none)
static uint32_t want_tex[16][2];       // TOTK_DUMP_TEX: slot id and 0, or width and height
static int nwant_tex;
static uint64_t last_targets;          // targets_sig of the last render pass; 0 after a dispatch
static const char *pass_skip;          // why the current pass copies nothing, or NULL
static volatile int64_t png_bytes, inflight, bin_bytes;
static uint64_t raw_bytes;             // under img_lock
static int dump_raw;                   // TOTK_DUMP_RAW
// TOTK_DUMP_PASS: chosen pass numbers and shader hashes; draws.txt receives the chosen draws
static int want_pass[32], nwant_pass, nwant_sh;
static uint64_t want_sh[16];
static FILE *draws_f;
static PlatMutex rec_lock = PLAT_MUTEX_INIT, img_lock = PLAT_MUTEX_INIT;

// ---- formats

enum { K_UNORM, K_SNORM, K_UINT, K_SINT, K_FLOAT, K_RGB10A2, K_BGR10A2, K_RG11B10, K_E5, K_R5G6B5, K_D16, K_D24, K_D32F };
enum { M_UNORM, M_SNORM, M_INT, M_FLOAT, M_DEPTH };
typedef struct { VkFormat vk; const char *name; uint8_t bytes, nc, bits, kind, bgr; } Fmt;

#define F(v, n, by, nc, bi, k) {VK_FORMAT_##v, n, by, nc, bi, k, 0}
static const Fmt fmts[] = {
    F(R8_UNORM, "r8", 1, 1, 8, K_UNORM), F(R8_SNORM, "r8sn", 1, 1, 8, K_SNORM), F(R8_UINT, "r8ui", 1, 1, 8, K_UINT),
    F(R8_SINT, "r8i", 1, 1, 8, K_SINT), F(R8_SRGB, "r8srgb", 1, 1, 8, K_UNORM),
    F(R8G8_UNORM, "rg8", 2, 2, 8, K_UNORM), F(R8G8_SNORM, "rg8sn", 2, 2, 8, K_SNORM),
    F(R8G8_UINT, "rg8ui", 2, 2, 8, K_UINT), F(R8G8_SINT, "rg8i", 2, 2, 8, K_SINT),
    F(R8G8B8A8_UNORM, "rgba8", 4, 4, 8, K_UNORM), F(R8G8B8A8_SNORM, "rgba8sn", 4, 4, 8, K_SNORM),
    F(R8G8B8A8_UINT, "rgba8ui", 4, 4, 8, K_UINT), F(R8G8B8A8_SINT, "rgba8i", 4, 4, 8, K_SINT),
    F(R8G8B8A8_SRGB, "rgba8srgb", 4, 4, 8, K_UNORM),
    {VK_FORMAT_B8G8R8A8_UNORM, "bgra8", 4, 4, 8, K_UNORM, 1}, {VK_FORMAT_B8G8R8A8_SRGB, "bgra8srgb", 4, 4, 8, K_UNORM, 1},
    F(A2B10G10R10_UNORM_PACK32, "rgb10a2", 4, 4, 10, K_RGB10A2), F(A2R10G10B10_UNORM_PACK32, "bgr10a2", 4, 4, 10, K_BGR10A2),
    F(R5G6B5_UNORM_PACK16, "r5g6b5", 2, 3, 5, K_R5G6B5),
    F(R16_UNORM, "r16", 2, 1, 16, K_UNORM), F(R16_SNORM, "r16sn", 2, 1, 16, K_SNORM), F(R16_UINT, "r16ui", 2, 1, 16, K_UINT),
    F(R16_SINT, "r16i", 2, 1, 16, K_SINT), F(R16_SFLOAT, "r16f", 2, 1, 16, K_FLOAT),
    F(R16G16_UNORM, "rg16", 4, 2, 16, K_UNORM), F(R16G16_SNORM, "rg16sn", 4, 2, 16, K_SNORM),
    F(R16G16_UINT, "rg16ui", 4, 2, 16, K_UINT), F(R16G16_SINT, "rg16i", 4, 2, 16, K_SINT),
    F(R16G16_SFLOAT, "rg16f", 4, 2, 16, K_FLOAT),
    F(R16G16B16A16_UNORM, "rgba16", 8, 4, 16, K_UNORM), F(R16G16B16A16_SNORM, "rgba16sn", 8, 4, 16, K_SNORM),
    F(R16G16B16A16_UINT, "rgba16ui", 8, 4, 16, K_UINT), F(R16G16B16A16_SINT, "rgba16i", 8, 4, 16, K_SINT),
    F(R16G16B16A16_SFLOAT, "rgba16f", 8, 4, 16, K_FLOAT),
    F(R32_UINT, "r32ui", 4, 1, 32, K_UINT), F(R32_SINT, "r32i", 4, 1, 32, K_SINT), F(R32_SFLOAT, "r32f", 4, 1, 32, K_FLOAT),
    F(R32G32_UINT, "rg32ui", 8, 2, 32, K_UINT), F(R32G32_SINT, "rg32i", 8, 2, 32, K_SINT),
    F(R32G32_SFLOAT, "rg32f", 8, 2, 32, K_FLOAT),
    F(R32G32B32A32_UINT, "rgba32ui", 16, 4, 32, K_UINT), F(R32G32B32A32_SINT, "rgba32i", 16, 4, 32, K_SINT),
    F(R32G32B32A32_SFLOAT, "rgba32f", 16, 4, 32, K_FLOAT),
    F(B10G11R11_UFLOAT_PACK32, "r11g11b10f", 4, 3, 11, K_RG11B10), F(E5B9G9R9_UFLOAT_PACK32, "rgb9e5", 4, 3, 9, K_E5),
    // depth aspect of depth formats, as vkCmdCopyImageToBuffer writes it
    F(D16_UNORM, "d16", 2, 1, 16, K_D16), F(D16_UNORM_S8_UINT, "d16s8", 2, 1, 16, K_D16),
    F(X8_D24_UNORM_PACK32, "d24", 4, 1, 24, K_D24), F(D24_UNORM_S8_UINT, "d24s8", 4, 1, 24, K_D24),
    F(D32_SFLOAT, "d32f", 4, 1, 32, K_D32F), F(D32_SFLOAT_S8_UINT, "d32fs8", 4, 1, 32, K_D32F),
    F(S8_UINT, "s8", 1, 1, 8, K_UINT),   // the stencil aspect, as vkCmdCopyImageToBuffer writes it
};
#undef F

static const Fmt *fmt_of(VkFormat f) {
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
        if (fmts[i].vk == f) return &fmts[i];
    return NULL;
}

static const char *fmt_name(VkFormat f, char tmp[16]) {
    const Fmt *d = fmt_of(f);
    if (d) return d->name;
    snprintf(tmp, 16, "vk%d", (int)f);
    return tmp;
}

static int mode_of(const Fmt *f) {
    switch (f->kind) {
    case K_SNORM: return M_SNORM;
    case K_UINT: case K_SINT: return M_INT;
    case K_FLOAT: case K_RG11B10: case K_E5: return M_FLOAT;
    case K_D16: case K_D24: case K_D32F: return M_DEPTH;
    default: return M_UNORM;
    }
}

// Unsigned small float with a 5-bit exponent and mbits of mantissa (half: 10, R11G11B10: 6 and 5).
static float ufloat(uint32_t v, int mbits) {
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf((float)(m | 1u << mbits), (int)e - 15 - mbits);
}

static int32_t sext(uint32_t v, int bits) { return bits >= 32 ? (int32_t)v : (int32_t)(v << (32 - bits)) >> (32 - bits); }

static void decode(const Fmt *f, const uint8_t *p, float o[4]) {
    uint32_t u = 0;
    o[0] = o[1] = o[2] = 0, o[3] = 1;
    switch (f->kind) {
    case K_RGB10A2: case K_BGR10A2:
        memcpy(&u, p, 4);
        o[0] = (u & 1023) / 1023.f, o[1] = (u >> 10 & 1023) / 1023.f, o[2] = (u >> 20 & 1023) / 1023.f, o[3] = (u >> 30) / 3.f;
        if (f->kind == K_BGR10A2) { float t = o[0]; o[0] = o[2], o[2] = t; }
        return;
    case K_RG11B10:
        memcpy(&u, p, 4);
        o[0] = ufloat(u & 2047, 6), o[1] = ufloat(u >> 11 & 2047, 6), o[2] = ufloat(u >> 22 & 1023, 5);
        return;
    case K_E5: {
        memcpy(&u, p, 4);
        float s = ldexpf(1.f, (int)(u >> 27) - 24);
        o[0] = (u & 511) * s, o[1] = (u >> 9 & 511) * s, o[2] = (u >> 18 & 511) * s;
        return;
    }
    case K_R5G6B5:
        memcpy(&u, p, 2);
        o[0] = (u >> 11 & 31) / 31.f, o[1] = (u >> 5 & 63) / 63.f, o[2] = (u & 31) / 31.f;
        return;
    case K_D16: memcpy(&u, p, 2); o[0] = u / 65535.f; return;
    case K_D24: memcpy(&u, p, 4); o[0] = (u & 0xffffff) / 16777215.f; return;
    case K_D32F: memcpy(&o[0], p, 4); return;
    }
    int cb = f->bits / 8;
    for (int c = 0; c < f->nc; c++) {
        u = 0;
        memcpy(&u, p + c * cb, (size_t)cb);
        float x;
        switch (f->kind) {
        case K_UNORM: x = (float)u / (float)((1ull << f->bits) - 1); break;
        case K_SNORM: x = fmaxf((float)sext(u, f->bits) / (float)((1u << (f->bits - 1)) - 1), -1.f); break;
        case K_UINT: x = (float)u; break;
        case K_SINT: x = (float)sext(u, f->bits); break;
        default:
            if (f->bits == 16) x = (u & 0x8000 ? -1.f : 1.f) * ufloat(u & 0x7fff, 10);
            else memcpy(&x, &u, 4);
        }
        o[c] = x;
    }
    if (f->bgr) { float t = o[0]; o[0] = o[2], o[2] = t; }
}

// ---- conversion on the completion thread

typedef struct {
    VkBuffer buf;
    VmaAllocation alloc;
    const uint8_t *map;
    uint64_t bytes;
    const Fmt *f;       // texel format of the buffer; of a BC image, the uncompressed format of its block size
    uint32_t w, h, d;   // buffer texels (BC: blocks); d: 3D slices, laid side by side in the PNG
    VkFormat bc;        // the BC format the blocks are decoded as, else VK_FORMAT_UNDEFINED
    uint32_t tw, th;    // BC: texels of one slice
    char name[160];
} Job;

static int is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }

// Decodes the blocks of j into px (RGBA floats, pw texels per row, slices side by side); returns the PNG mode and sets
// *nc to the channel count.
static int bc_decode(const Job *j, float *px, uint32_t pw, int *nc) {
    VkFormat f = j->bc;
    int sgn = f == VK_FORMAT_BC4_SNORM_BLOCK || f == VK_FORMAT_BC5_SNORM_BLOCK || f == VK_FORMAT_BC6H_SFLOAT_BLOCK;
    for (uint32_t z = 0; z < j->d; z++)
        for (uint32_t by = 0; by < j->h; by++)
            for (uint32_t bx = 0; bx < j->w; bx++) {
                const uint8_t *b = j->map + (((uint64_t)z * j->h + by) * j->w + bx) * j->f->bytes;
                float t[16][4], cf[48];
                uint8_t c8[64];
                for (int i = 0; i < 16; i++) t[i][0] = t[i][1] = t[i][2] = 0, t[i][3] = 1;
                if (f <= VK_FORMAT_BC3_SRGB_BLOCK || f >= VK_FORMAT_BC7_UNORM_BLOCK) {
                    if (f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK) bcdec_bc1(b, c8, 16);
                    else if (f <= VK_FORMAT_BC2_SRGB_BLOCK) bcdec_bc2(b, c8, 16);
                    else if (f <= VK_FORMAT_BC3_SRGB_BLOCK) bcdec_bc3(b, c8, 16);
                    else bcdec_bc7(b, c8, 16);
                    for (int i = 0; i < 16; i++)
                        for (int c = 0; c < 4; c++) t[i][c] = c8[4 * i + c] / 255.f;
                } else if (f <= VK_FORMAT_BC4_SNORM_BLOCK) {
                    bcdec_bc4_float(b, cf, 4, sgn);
                    for (int i = 0; i < 16; i++) t[i][0] = cf[i];
                } else if (f <= VK_FORMAT_BC5_SNORM_BLOCK) {
                    bcdec_bc5_float(b, cf, 8, sgn);
                    for (int i = 0; i < 16; i++) t[i][0] = cf[2 * i], t[i][1] = cf[2 * i + 1];
                } else {
                    bcdec_bc6h_float(b, cf, 12, sgn);
                    for (int i = 0; i < 16; i++) t[i][0] = cf[3 * i], t[i][1] = cf[3 * i + 1], t[i][2] = cf[3 * i + 2];
                }
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t X = bx * 4 + x, Y = by * 4 + y;
                        if (X < j->tw && Y < j->th) memcpy(px + 4 * ((uint64_t)Y * pw + (uint64_t)z * j->tw + X), t[y * 4 + x], 16);
                    }
            }
    *nc = f <= VK_FORMAT_BC4_SNORM_BLOCK && f >= VK_FORMAT_BC4_UNORM_BLOCK ? 1
          : f <= VK_FORMAT_BC5_SNORM_BLOCK && f >= VK_FORMAT_BC5_UNORM_BLOCK ? 2 : 4;
    if (f == VK_FORMAT_BC6H_UFLOAT_BLOCK || f == VK_FORMAT_BC6H_SFLOAT_BLOCK) return M_FLOAT;
    return sgn ? M_SNORM : M_UNORM;
}

// 1 when the PNG loses values of format f: more than 8 bits per channel, or integer
static int raw_wanted(const Fmt *f) {
    if (f->kind == K_R5G6B5) return 0;
    return f->bits > 8 || (f->kind != K_UNORM && f->kind != K_SNORM);
}

// Writes bytes to <dir>/<file> within RAW_CAP; NULL when written, else why not.
static const char *write_raw(const char *file, const void *data, uint64_t bytes) {
    plat_mutex_lock(&img_lock);
    int ok = raw_bytes + bytes <= RAW_CAP;
    if (ok) raw_bytes += bytes;
    plat_mutex_unlock(&img_lock);
    if (!ok) return "raw cap reached";
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "wb");
    if (!f) return "cannot write";
    fwrite(data, 1, (size_t)bytes, f);
    fclose(f);
    return NULL;
}

// Distinct raw texel values among rows [y0, y1) of every slice.
static uint64_t distinct(const Job *j, uint32_t y0, uint32_t y1) {
    uint64_t n = (uint64_t)j->w * (y1 - y0) * j->d, cap = 1024;
    while (cap < 2 * n) cap *= 2;
    uint64_t *set = calloc(cap, sizeof *set), count = 0;
    if (!set) return 0;
    for (uint32_t z = 0; z < j->d; z++)
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = 0; x < j->w; x++) {
                const uint8_t *p = j->map + (((uint64_t)z * j->h + y) * j->w + x) * j->f->bytes;
                uint64_t a = 0, b = 0;
                memcpy(&a, p, j->f->bytes < 8 ? j->f->bytes : 8);
                if (j->f->bytes > 8) memcpy(&b, p + 8, j->f->bytes - 8u);
                uint64_t k = (a ^ (b * 0x9E3779B97F4A7C15ull)) + 1;
                if (!k) k = 1;
                uint64_t i = (k * 0xff51afd7ed558ccdull) >> 17;
                for (;; i++) {
                    uint64_t *s = &set[i & (cap - 1)];
                    if (*s == k) break;
                    if (!*s) { *s = k, count++; break; }
                }
            }
    free(set);
    return count;
}

static uint8_t u8(float x) { return x > 0 ? x < 1 ? (uint8_t)(x * 255.f + .5f) : 255 : 0; }

static void write_images(const Job *j) {
    uint32_t sw = j->bc ? j->tw : j->w, sh = j->bc ? j->th : j->h;   // texels of one slice
    uint32_t pw = sw * j->d, ph = sh;
    uint64_t n = (uint64_t)pw * ph;
    float *px = malloc(n * 16);
    uint8_t *out = malloc(n * 4);
    if (!px || !out) rt_fatal("vk_dump: out of memory for %s", j->name);
    int mode, nc;
    if (j->bc) {
        mode = bc_decode(j, px, pw, &nc);
    } else {
        mode = mode_of(j->f), nc = j->f->nc;
        for (uint32_t z = 0; z < j->d; z++)
            for (uint32_t y = 0; y < j->h; y++)
                for (uint32_t x = 0; x < j->w; x++)
                    decode(j->f, j->map + (((uint64_t)z * j->h + y) * j->w + x) * j->f->bytes,
                           px + 4 * ((uint64_t)y * pw + (uint64_t)z * j->w + x));
    }
    char raw[200] = "";
    if (dump_raw && (j->bc ? mode == M_FLOAT : raw_wanted(j->f))) {   // before NaNs are zeroed
        char file[200];
        snprintf(file, sizeof file, "%s.raw", j->name);
        const char *why = j->bc ? write_raw(file, px, n * 16) : write_raw(file, j->map, j->bytes);
        if (why) snprintf(raw, sizeof raw, " raw skipped: %s", why);
        else if (j->bc) snprintf(raw, sizeof raw, " raw %s %ux%ux1 rgba32f", file, pw, ph);
        else snprintf(raw, sizeof raw, " raw %s %ux%ux%u %s", file, j->w, j->h, j->d, j->f->name);
    }
    float mn[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, mx[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    uint64_t nans = 0;
    for (uint64_t i = 0; i < n; i++) {
        float *o = px + 4 * i;
        for (int c = 0; c < 4; c++) {
            if (isnan(o[c])) { nans++, o[c] = 0; continue; }
            if (o[c] < mn[c]) mn[c] = o[c];
            if (o[c] > mx[c]) mx[c] = o[c];
        }
    }
    int grey = nc == 1;
    float scale = 1, lo = mn[0], range = mx[0] - mn[0];
    if (mode == M_INT) {
        float m = 1;
        for (int c = 0; c < nc && c < 3; c++) m = fmaxf(m, fmaxf(fabsf(mx[c]), fabsf(mn[c])));
        scale = 1.f / m;
    }
    for (int pass = 0; pass < (mode == M_FLOAT ? 2 : 1); pass++) {
        for (uint64_t i = 0; i < n; i++) {
            const float *o = px + 4 * i;
            for (int c = 0; c < 3; c++) {
                float x = o[grey ? 0 : c];
                if (mode == M_SNORM) x = x * .5f + .5f;
                else if (mode == M_INT) x *= scale;
                else if (mode == M_DEPTH) x = range > 0 ? (x - lo) / range : 0;
                else if (mode == M_FLOAT && pass) x = x > 0 ? x / (1 + x) : 0;
                out[4 * i + c] = u8(x);
            }
            out[4 * i + 3] = 255;
        }
        char path[600];
        snprintf(path, sizeof path, "%s/%s%s.png", dir, j->name, pass ? "_tm" : "");
        rt_write_png(path, out, pw, ph);
    }
    uint64_t all = distinct(j, 0, j->h), top = distinct(j, 0, j->h / 4 ? j->h / 4 : 1);
    plat_mutex_lock(&img_lock);
    if (images_f) {
        fprintf(images_f, "%s %ux%ux%u min %g %g %g %g max %g %g %g %g distinct %llu top-quarter %llu%s nan %llu%s%s\n",
                j->name, sw, sh, j->d, mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3], (unsigned long long)all,
                (unsigned long long)top, j->bc ? " (of the blocks)" : "", (unsigned long long)nans,
                j->bc ? " bc-decoded" : "", raw);
        fflush(images_f);
    }
    plat_mutex_unlock(&img_lock);
    free(out);
    free(px);
}

static void job_done(void *arg) {
    Job *j = arg;
    VmaAllocator vma = vk_ctx()->vma;
    vmaInvalidateAllocation(vma, j->alloc, 0, VK_WHOLE_SIZE);
    write_images(j);
    vmaDestroyBuffer(vma, j->buf, j->alloc);
    plat_add64(&inflight, -(int64_t)j->bytes);
    free(j);
}

// ---- recording

// Records the copy of one subresource into a host buffer and queues its PNGs. Returns NULL, or why it was skipped.
// The buffer is w x h x d texels of vf; the image region is `ext` image texels at depth plane z (differs for the
// blocks of a BC image read as an uncompressed format of the block size, and for a 3D slice). bc: the BC format those
// blocks are decoded as for the PNG (ext then gives the texels), else VK_FORMAT_UNDEFINED. A stencil-only aspect copies
// the stencil (vf VK_FORMAT_S8_UINT); an aspect with depth copies the depth.
static const char *copy_region(VkExec *ex, VkImage image, VkFormat vf, VkImageAspectFlags aspect, uint32_t level,
                               uint32_t layer, uint32_t z, VkExtent3D ext, uint32_t w, uint32_t h, uint32_t d,
                               const char *name, VkFormat bc) {
    const Fmt *f = fmt_of(vf);
    if (!f) return "format not decoded";
    if (__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1 != dump_frame && (uint64_t)w * h * d > 65536)
        return "logged frame, large image";
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    else if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    else if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) return "aspect not copied";
    uint64_t bytes = (uint64_t)w * h * d * f->bytes;
    uint64_t est = ((uint64_t)h * (1 + 4ull * w * d) + 128) * (mode_of(f) == M_FLOAT ? 2 : 1);
    if ((uint64_t)png_bytes + est > PNG_CAP) return "PNG cap reached";
    if ((uint64_t)inflight + bytes > INFLIGHT_CAP) return "too many copies in flight";
    Job *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk_dump: out of memory");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        free(j);
        return "host buffer allocation failed";
    }
    j->map = info.pMappedData, j->bytes = bytes, j->f = f, j->w = w, j->h = h, j->d = d;
    if (is_bc(bc)) j->bc = bc, j->tw = ext.width, j->th = ext.height;
    snprintf(j->name, sizeof j->name, "%s", name);
    VkMemoryBarrier2 pre = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkMemoryBarrier2 post = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                             .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                             .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                             .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &pre};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkBufferImageCopy r = {.imageSubresource = {aspect, level, layer, 1}, .imageOffset = {0, 0, (int32_t)z},
                           .imageExtent = ext};   // packed rows: a BC row of ext.width texels is w blocks
    vkCmdCopyImageToBuffer(ex->cb, image, VK_IMAGE_LAYOUT_GENERAL, j->buf, 1, &r);
    di.pMemoryBarriers = &post;
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 3;
    plat_add64(&png_bytes, (int64_t)est);
    plat_add64(&inflight, (int64_t)bytes);
    vk_completion_add(ex->fence, job_done, j);
    return NULL;
}

static const char *copy_out(VkExec *ex, VkImage image, VkFormat vf, VkImageAspectFlags aspect, uint32_t level,
                            uint32_t layer, uint32_t w, uint32_t h, uint32_t d, const char *name) {
    return copy_region(ex, image, vf, aspect, level, layer, 0, (VkExtent3D){w, h, d}, w, h, d, name, VK_FORMAT_UNDEFINED);
}

// A multisampled colour target: resolved into a temporary image (destroyed when the submission completes), which is
// copied out. Depth, stencil and integer formats are not resolved.
typedef struct { VkImage img; VmaAllocation alloc; } TmpImg;
static void tmp_done(void *arg) {
    TmpImg *t = arg;
    vmaDestroyImage(vk_ctx()->vma, t->img, t->alloc);
    free(t);
}

static const char *copy_msaa(VkExec *ex, const VkExecTarget *t, const char *name) {
    if (t->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return "multisampled depth/stencil (not resolved)";
    VkFormat vf = t->img ? t->img->format : t->format;   // resolve needs the formats the images were created with
    const Fmt *f = fmt_of(vf);
    if (!f) return "format not decoded";
    if (mode_of(f) == M_INT) return "multisampled integer (not resolved)";
    TmpImg *ti = calloc(1, sizeof *ti);
    if (!ti) rt_fatal("vk_dump: out of memory");
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = vf,
                            .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, &ti->img, &ti->alloc, NULL) != VK_SUCCESS) {
        free(ti);
        return "resolve image allocation failed";
    }
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_RESOLVE_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .dstStageMask = VK_PIPELINE_STAGE_2_RESOLVE_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = ti->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkImageResolve r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {t->width, t->height, 1}};
    vkCmdResolveImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, ti->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    ex->recorded += 2;
    vk_completion_add(ex->fence, tmp_done, ti);
    return copy_out(ex, ti->img, vf, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, t->width, t->height, 1, name);
}

// ---- buffer copies to files (chosen draws and dispatches): <dir>/<file>, written when the submission completes
typedef struct { VkBuffer buf; VmaAllocation alloc; const uint8_t *map; uint64_t size; char path[600]; } FileJob;

static void file_done(void *arg) {
    FileJob *j = arg;
    vmaInvalidateAllocation(vk_ctx()->vma, j->alloc, 0, VK_WHOLE_SIZE);
    FILE *f = fopen(j->path, "wb");
    if (f) fwrite(j->map, 1, (size_t)j->size, f), fclose(f);
    vmaDestroyBuffer(vk_ctx()->vma, j->buf, j->alloc);
    free(j);
}

// Records the copy of guest GPU memory [addr, addr+size) into <file>; size is cut to what is backed and BIN_ONE_CAP.
// Returns NULL, or why nothing was copied; *copied gets the bytes copied. Only outside a render pass.
static const char *copy_buffer(VkExec *ex, uint64_t addr, uint64_t size, const char *file, uint64_t *copied) {
    *copied = 0;
    if (!addr || !size) return "no buffer";
    VkMemSpan s;
    if (!vk_mem_resolve(addr, 1, &s)) return "not backed";
    VK_MEM_PICK(s);   // the GPU's view of the memory
    if (size > s.avail) size = s.avail;
    if (size > BIN_ONE_CAP) size = BIN_ONE_CAP;
    if ((uint64_t)bin_bytes + size > BIN_CAP) return "buffer cap reached";
    FileJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk_dump: out of memory");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        free(j);
        return "host buffer allocation failed";
    }
    j->map = info.pMappedData, j->size = size;
    snprintf(j->path, sizeof j->path, "%s/%s", dir, file);
    VkMemoryBarrier2 mb[2] = {
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT},
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
         .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb[0]};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkBufferCopy c = {s.offset, 0, size};
    vkCmdCopyBuffer(ex->cb, s.buffer, j->buf, 1, &c);
    di.pMemoryBarriers = &mb[1];
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 3;
    plat_add64(&bin_bytes, (int64_t)size);
    vk_completion_add(ex->fence, file_done, j);
    *copied = size;
    return NULL;
}

// "-> file (n bytes)" or " skipped: why" for a copy_buffer result
static void bin_note(FILE *out, const char *file, const char *why, uint64_t copied, uint64_t asked) {
    if (why) fprintf(out, " skipped: %s\n", why);
    else fprintf(out, " -> %s (%llu bytes%s)\n", file, (unsigned long long)copied, copied < asked ? ", cut" : "");
}

// ---- TOTK_DUMP_BUF=<addr>:<size>[:<off>:<stride>:<w>:<h>]: after every pass and dispatch that is logged, the GPU
// buffer range is copied to a host buffer; when it differs from the previous copy it goes to buf_<pass>.bin, and
// bufs.txt gets the changed bytes' extent plus, with the grid arguments, one map per dword of the w x h records of
// `stride` bytes at `off` (each record a letter: its value's index in the table under the map).

static uint64_t bd_addr, bd_size, bd_off, bd_stride;
static uint32_t bd_w, bd_h;
static uint8_t *bd_prev;
static FILE *bufs_f;

typedef struct { VkBuffer buf; VmaAllocation alloc; const uint8_t *map; int pass; } BufJob;

static void buf_grid(const uint8_t *m) {
    for (uint32_t dw = 0; dw < bd_stride / 4; dw++) {
        uint32_t vals[52];
        int nv = 0, over = 0;
        fprintf(bufs_f, "  dword %u:\n", dw);
        for (uint32_t y = 0; y < bd_h; y++) {
            char line[260];
            uint32_t x;
            for (x = 0; x < bd_w && x < 256; x++) {
                uint64_t o = bd_off + ((uint64_t)y * bd_w + x) * bd_stride + dw * 4;
                if (o + 4 > bd_size) { line[x] = ' '; continue; }
                uint32_t v;
                memcpy(&v, m + o, 4);
                int k = 0;
                while (k < nv && vals[k] != v) k++;
                if (k == nv) {
                    if (nv < 52) vals[nv++] = v;
                    else { over = 1; line[x] = '?'; continue; }
                }
                line[x] = (char)(k < 26 ? 'a' + k : 'A' + k - 26);
            }
            line[x] = 0;
            fprintf(bufs_f, "    %2u %s\n", y, line);
        }
        for (int k = 0; k < nv; k++) fprintf(bufs_f, "%s%c=%08x", k % 8 ? " " : "    ", k < 26 ? 'a' + k : 'A' + k - 26, vals[k]);
        fprintf(bufs_f, "%s\n", over ? " (more: ?)" : "");
    }
}

static void buf_done(void *arg) {
    BufJob *j = arg;
    vmaInvalidateAllocation(vk_ctx()->vma, j->alloc, 0, VK_WHOLE_SIZE);
    plat_mutex_lock(&img_lock);
    if (bufs_f) {
        uint64_t lo = bd_size, hi = 0;
        for (uint64_t i = 0; i < bd_size; i++)
            if (!bd_prev || j->map[i] != bd_prev[i]) { if (i < lo) lo = i; hi = i + 1; }
        if (hi) {
            char path[600];
            snprintf(path, sizeof path, "%s/buf_%04d.bin", dir, j->pass);
            FILE *f = fopen(path, "wb");
            if (f) fwrite(j->map, 1, bd_size, f), fclose(f);
            fprintf(bufs_f, "after pass %04d: bytes %#llx..%#llx changed\n", j->pass, (unsigned long long)lo,
                    (unsigned long long)hi);
            if (bd_stride) buf_grid(j->map);
            fflush(bufs_f);
            if (!bd_prev) bd_prev = malloc(bd_size);
            if (bd_prev) memcpy(bd_prev, j->map, bd_size);
        }
    }
    plat_mutex_unlock(&img_lock);
    vmaDestroyBuffer(vk_ctx()->vma, j->buf, j->alloc);
    free(j);
}

static void buf_snap(VkExec *ex, int pass) {
    if (!bd_size) return;
    VkMemSpan s;
    if (!vk_mem_resolve(bd_addr, bd_size, &s) || s.avail < bd_size) return;
    VK_MEM_PICK(s);   // the GPU's view of the buffer
    BufJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk_dump: out of memory");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bd_size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        free(j);
        return;
    }
    j->map = info.pMappedData, j->pass = pass;
    VkMemoryBarrier2 mb[2] = {
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT},
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb[0]};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkBufferCopy c = {s.offset, 0, bd_size};
    vkCmdCopyBuffer(ex->cb, s.buffer, j->buf, 1, &c);
    di.pMemoryBarriers = &mb[1];
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 3;
    vk_completion_add(ex->fence, buf_done, j);
}

static int active(void);
// Draws prepare_draw dropped in the current pass, by reason (vk_dump_dropped); printed with the pass (idea from Astral
// Chain Recomp 813d115, MIT), so a pass whose draw count falls short of the game's shows why.
static const char *drop_names[VK_DROP_N] = {"disabled", "scissor", "no-program", "no-vs", "shader-pending",
                                            "shader-failed", "dlaa", "pipeline-pending", "pipeline-failed"};
static int drop_cnt[VK_DROP_N];
void vk_dump_dropped(int why) {
    if ((unsigned)why >= VK_DROP_N) return;
    plat_mutex_lock(&rec_lock);
    if (active()) drop_cnt[why]++;
    plat_mutex_unlock(&rec_lock);
}

static int active(void) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    return !finished && (f == dump_frame || (log_from && f >= log_from && f < dump_frame));
}
static uint64_t frame_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1; }

typedef struct { char *s; size_t n, cap; } Str;

__attribute__((format(printf, 2, 3))) static void sp(Str *b, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    if (k < 0) return;
    if (b->n + (size_t)k + 1 > b->cap) {
        b->cap = (b->n + (size_t)k + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if (!b->s) rt_fatal("vk_dump: out of memory");
    }
    va_start(a, fmt);
    vsnprintf(b->s + b->n, (size_t)k + 1, fmt, a);
    va_end(a);
    b->n += (size_t)k;
}

// SPIR-V hash of a shader; the first time, its SPIR-V goes to sh_<hash>.spv.
static uint64_t shader_hash(const GfxShader *s) {
    static struct { const GfxShader *s; uint64_t h; } tab[1024];
    static int n;
    if (!s) return 0;
    for (int i = 0; i < n; i++)
        if (tab[i].s == s) return tab[i].h;
    uint64_t words = 0, h = 0xcbf29ce484222325ull;
    const uint32_t *w = gfx_shader_spirv(s, &words);
    const uint8_t *b = (const uint8_t *)w;
    for (uint64_t i = 0; i < words * 4; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    char path[600];
    snprintf(path, sizeof path, "%s/sh_%016llx.spv", dir, (unsigned long long)h);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(w, 4, (size_t)words, f), fclose(f);
    if (n < 1024) tab[n].s = s, tab[n].h = h, n++;
    return h;
}

// Per pass: distinct pipeline + texture sets, draws, 3D textures to copy at its end.
typedef struct { VkPipeline p; uint64_t th; int draws; char *text; } Ent;
static Ent ents[MAX_ENTS];
static int nents, ents_over, pass_draws;
typedef struct { VkImage image; VkFormat format; uint32_t w, h, d; int id, is2d; } Lut;
static Lut luts[MAX_LUTS];          // queued for the next pass end or dispatch
static int nluts;
// TOTK_DUMP_PASS state. snap_pass: the open render pass was chosen by number. snap_draw: the draw being recorded is
// chosen; vk_dump_draw_done snapshots it. snapping: a snapshot ends and begins the pass again, and the pass hooks stand
// aside. force_tex: describe_bindings queues every sampled texture. draw_text: the chosen draw's description.
static int snap_pass, snap_draw, snapping, snaps, force_tex;
static Str draw_text;
static uint32_t snap_vattr[16];
static vs_st snap_vstream[16];
static VkImage luts_done[MAX_LUTS];
static int nluts_done;

static void queue_lut(const VkTexImage *t, int id) {
    if (frame_now() != dump_frame) return;
    for (int i = 0; i < nluts_done; i++)
        if (luts_done[i] == t->image) return;
    if (nluts_done == MAX_LUTS) return;
    luts_done[nluts_done++] = t->image;
    luts[nluts++] = (Lut){t->image, t->format, t->width, t->height, t->type == VK_IMAGE_TYPE_3D ? t->depth : 1, id,
                          t->type != VK_IMAGE_TYPE_3D};
}

// The uncompressed format of a BC format's block size, UNDEFINED for other formats: a BC texture is copied as its raw
// blocks (one texel per block) and decoded on the completion thread (bc_decode).
static VkFormat block_format(VkFormat f) {
    if (f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK ||
        (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK))
        return VK_FORMAT_R32G32_UINT;
    if (f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) return VK_FORMAT_R32G32B32A32_UINT;
    return VK_FORMAT_UNDEFINED;
}

static void flush_luts(VkExec *ex, int n) {
    char fn[16];
    for (int i = 0; i < nluts; i++) {
        const Lut *l = &luts[i];
        char name[160];
        VkFormat bf = block_format(l->format);
        snprintf(name, sizeof name, "pass_%04d_%s%d_%ux%ux%u_%d_%s%s", n, l->is2d ? "tex" : "lut", l->id, l->w, l->h, l->d,
                 (int)l->format, fmt_name(l->format, fn), bf ? "_bc" : "");
        const char *why = bf ? copy_region(ex, l->image, bf, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0,
                                           (VkExtent3D){l->w, l->h, l->d}, (l->w + 3) / 4, (l->h + 3) / 4, l->d, name,
                                           l->format)
                             : copy_out(ex, l->image, l->format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, l->w, l->h, l->d, name);
        fprintf(passes_f, "  %s tex %d -> %s.png%s%s\n", l->is2d ? "2D" : "3D", l->id, name, why ? " skipped: " : "",
                why ? why : "");
    }
    nluts = 0;
}

static int nvn_stage(int gfx_stage) { return gfx_stage == GFX_STAGE_VS ? 0 : gfx_stage == GFX_STAGE_FS ? 1 : 5; }

static const char *decl_type(uint16_t t, char tmp[24]) {
    static const char *base[] = {"?", "1D", "buffer", "2D", "3D", "cube"};
    snprintf(tmp, 24, "%s%s%s%s", base[(t & 0xff) < 6 ? t & 0xff : 0], t & 0x100 ? "Array" : "", t & 0x200 ? "MS" : "",
             t & 0x400 ? "Shadow" : "");
    return tmp;
}

static const char *wrap_name(int w) {
    static const char *n[] = {"clamp", "repeat", "mirror_clamp", "mirror_clamp_edge", "mirror_clamp_border",
                              "clamp_border", "mirrored_repeat", "clamp_edge"};
    return w >= 0 && w < 8 ? n[w] : "?";
}

static uint64_t handle_of(const VkExec *ex, int st, int slot, int img) {
    if (img) return slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
    return slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
}

// The textures, images and samplers of info's bindings; 3D textures are queued for a copy.
static void describe_bindings(Str *b, const VkExec *ex, const VkPipeInfo *info) {
    static const char *stn[6] = {"vs", "fs", "?", "?", "?", "cs"};
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        const GfxShaderRes *r = bd->res;
        if (r && bd->kind == GFX_RES_SSBO) {   // where the address lives (c0: the stage's NVN slot table) and the stage's slots
            int st = nvn_stage(bd->stage);
            sp(b, "    %s ssbo at c%d+%#x%s, bound:", stn[st], r->cbuf_slot, r->handle * 4, r->flags & 1 ? " written" : "");
            for (int k = 0; k < 16; k++)
                if (ex->ssbo[st][k].addr)
                    sp(b, " %d=%#llx+%#llx", k, (unsigned long long)ex->ssbo[st][k].addr, (unsigned long long)ex->ssbo[st][k].size);
            sp(b, "\n");
            continue;
        }
        if (r && bd->kind == GFX_RES_UBO && dump_ubo && r->slot >= 3 && r->slot - 3 < 18) {
            int st = nvn_stage(bd->stage);
            const p_bind_buf *u = &ex->ubo[st][r->slot - 3];
            VkMemSpan ms;
            sp(b, "    %s ubo c%d addr %#llx size %#llx", stn[st], r->slot, (unsigned long long)u->addr, (unsigned long long)u->size);
            if (u->addr && vk_mem_resolve(u->addr, 1, &ms)) {
                uint64_t n = u->size && u->size < ms.avail ? u->size : ms.avail;
                const float *f = (const float *)(uintptr_t)u->addr;
                for (uint64_t k = 0; k < (uint64_t)dump_ubo && (k + 1) * 16 <= n; k++)
                    sp(b, "%s[%llu] %g %g %g %g", k % 4 ? " " : "\n      ", (unsigned long long)k, f[4 * k], f[4 * k + 1],
                       f[4 * k + 2], f[4 * k + 3]);
            } else {
                sp(b, " (not mapped)");
            }
            sp(b, "\n");
            continue;
        }
        if (!r || (bd->kind != GFX_RES_TEX && bd->kind != GFX_RES_IMG)) continue;
        int st = nvn_stage(bd->stage), img = bd->kind == GFX_RES_IMG;
        char dt[24], fn[16];
        if (bd->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || bd->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            sp(b, "    %s %s slot %d: buffer texture (zero view bound)\n", stn[st], img ? "img" : "tex", r->slot);
            continue;
        }
        for (uint32_t k = 0; k < (bd->count ? bd->count : 1u); k++) {
            int slot = r->slot + (int)k;
            uint64_t h = handle_of(ex, st, slot, img);
            sp(b, "    %s %s slot %d decl %s%s: ", stn[st], img ? "img" : "tex", slot, decl_type(r->type, dt),
               img && (r->flags & 4) ? " store" : "");
            if (!h) { sp(b, "no handle (dummy)\n"); continue; }
            view_t v;
            int hv = 0;
            const VkTexImage *t = vk_texture_by_id(NVN_HANDLE_TEX(h), &v, &hv);
            if (!t) {
                char why[200];
                vk_texture_slot_info(NVN_HANDLE_TEX(h), why, sizeof why);
                sp(b, "tex %d unresolved (dummy): %s\n", NVN_HANDLE_TEX(h), why);
                continue;
            }
            static const char *vt[] = {"1D", "2D", "3D", "cube", "1DArray", "2DArray", "cubeArray"};
            sp(b, "tex %d %s(%d) %ux%ux%u levels %u layers %u image %s", NVN_HANDLE_TEX(h), fmt_name(t->format, fn),
               (int)t->format, t->width, t->height, t->depth, t->levels, t->layers,
               t->view_type <= 6 ? vt[t->view_type] : "?");
            if (hv && v.set_format) sp(b, " view-nvn-fmt %#x", v.format);
            if (hv && v.set_levels) sp(b, " view-levels %u+%u", v.base_level, v.nlevels);
            if (hv && v.set_layers) sp(b, " view-layers %u+%u", v.min_layer, v.nlayers);
            samp_par s;
            if (img) {
                // storage image: no sampler
            } else if (r->separate) {
                sp(b, " | separate sampler (not replayed: default sampler)");
            } else if (vk_sampler_params(NVN_HANDLE_SAMP(h), &s)) {
                static const char *minn[] = {"N", "L", "N_mipN", "L_mipN", "N_mipL", "L_mipL"};
                int mf = s.minf;
                sp(b, " | samp %d: mag %s min %s (vk min %s mip %s) wrap %s/%s/%s aniso %g lod %g..%g bias %g%s",
                   NVN_HANDLE_SAMP(h), s.magf == 1 ? "L" : "N", mf < 6 ? minn[mf] : "?",
                   mf == 1 || mf == 3 || mf == 5 ? "L" : "N", mf <= 1 ? "none" : mf >= 4 ? "L" : "N",
                   wrap_name(s.ws), wrap_name(s.wt), wrap_name(s.wr), s.aniso, s.lod_min, s.lod_max, s.lod_bias,
                   s.cmp_mode ? " compare" : "");
            } else {
                sp(b, " | samp %d empty (default sampler)", NVN_HANDLE_SAMP(h));
            }
            uint32_t ups;
            int last, wr;
            vk_texture_debug(t, &ups, &last, &wr);
            sp(b, " | addr %#llx mirrored %d gpu_owned %d bound %d uploads %u last-check %d written-since %d",
               (unsigned long long)t->addr, t->mirrored, t->gpu_owned, t->bound, ups, last, wr);
            uint8_t ssw[4] = {0};
            vk_texture_slot_swz(NVN_HANDLE_TEX(h), ssw);
            sp(b, " | swz image %u%u%u%u slot %u%u%u%u", t->swz[0], t->swz[1], t->swz[2], t->swz[3], ssw[0], ssw[1], ssw[2],
               ssw[3]);
            if (hv && v.set_swizzle) sp(b, " view %u%u%u%u", v.swz[0], v.swz[1], v.swz[2], v.swz[3]);
            sp(b, "\n");
            int wanted = force_tex;   // a chosen draw (TOTK_DUMP_PASS) copies every texture it samples
            for (int k = 0; k < nwant_tex; k++)
                wanted |= want_tex[k][1] ? want_tex[k][0] == t->width && want_tex[k][1] == t->height
                                         : want_tex[k][0] == (uint32_t)NVN_HANDLE_TEX(h);
            if (t->type == VK_IMAGE_TYPE_3D) queue_lut(t, NVN_HANDLE_TEX(h));
            else if (wanted && t->type == VK_IMAGE_TYPE_2D && t->samples == 1) queue_lut(t, NVN_HANDLE_TEX(h));
            else if (!img && t->type == VK_IMAGE_TYPE_2D && t->samples == 1 && (uint64_t)t->width * t->height <= TEX2D_CAP &&
                     (t->format == VK_FORMAT_R8_UNORM || t->format == VK_FORMAT_R8G8_UNORM ||
                      t->format == VK_FORMAT_R8G8B8A8_UNORM || t->format == VK_FORMAT_R8G8B8A8_SRGB ||
                      t->format == VK_FORMAT_B8G8R8A8_UNORM || t->format == VK_FORMAT_B8G8R8A8_SRGB))
                queue_lut(t, NVN_HANDLE_TEX(h));
        }
    }
}

static uint64_t bindings_hash(const VkExec *ex, const VkPipeInfo *info) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (bd->res && bd->kind == GFX_RES_SSBO) {
            int st = nvn_stage(bd->stage), slot = bd->res->slot;
            if (slot >= 0 && slot < 16) h = (h ^ ex->ssbo[st][slot].addr) * 0x100000001b3ull;
            continue;
        }
        if (!bd->res || (bd->kind != GFX_RES_TEX && bd->kind != GFX_RES_IMG)) continue;
        for (uint32_t k = 0; k < (bd->count ? bd->count : 1u); k++)
            h = (h ^ handle_of(ex, nvn_stage(bd->stage), bd->res->slot + (int)k, bd->kind == GFX_RES_IMG)) * 0x100000001b3ull;
    }
    return h;
}

static int pass_wanted(int n) {
    for (int i = 0; i < nwant_pass; i++)
        if (want_pass[i] == n) return 1;
    return 0;
}

static int sh_wanted(uint64_t h) {
    for (int i = 0; i < nwant_sh; i++)
        if (h && want_sh[i] == h) return 1;
    return 0;
}

// The fixed-function state of a draw, its blend equations and scissor, then its bindings and fragment c3.
static void state_text(Str *b, const VkExec *ex, const VkPipeState *s, const VkPipeInfo *info) {
    sp(b, "vs %016llx fs %016llx prog %llu prim %d | depth test %u write %u func %u, stencil %u func %u ref %#x "
           "vmask %#x wmask %#x ops %u/%u/%u, chmask %#x, blend %#x, alpha func %u ref %g, cull %u front %u | offset en "
           "%u mode %u factor %g units %g clamp %g, depth -w %u clamp %u, vp0 z %g..%g range %g..%g\n",
       (unsigned long long)shader_hash(s->vs), (unsigned long long)shader_hash(s->fs),
       (unsigned long long)(ex->prog ? ex->prog->serial : 0), s->prim, (unsigned)s->ds.dt, (unsigned)s->ds.dw,
       (unsigned)s->ds.df, (unsigned)s->ds.st, (unsigned)s->ds.sf_f, ex->st_ref[0], ex->st_vmask[0], ex->st_wmask[0],
       (unsigned)s->ds.fail_f, (unsigned)s->ds.zfail_f, (unsigned)s->ds.zpass_f, s->chmask.mask,
       (unsigned)s->color.blend_en, (unsigned)s->color.alpha_func, (double)s->alpha_ref, (unsigned)s->poly.cull,
       (unsigned)s->poly.front, (unsigned)s->poly.offset_en, (unsigned)s->poly.mode, (double)ex->poly_offset.factor,
       (double)ex->poly_offset.units, (double)ex->poly_offset.clamp, (unsigned)s->depth_minus_w,
       (unsigned)s->depth_clamp, (double)ex->vp[0][4], (double)ex->vp[0][5], (double)ex->depth_range[0],
       (double)ex->depth_range[1]);
    sp(b, "   ");
    if (s->color.blend_en)
        for (int k = 0; k < 8; k++) {
            if (!(s->color.blend_en >> k & 1)) continue;
            const blend_st *bl = &s->blend[k];
            sp(b, " blend t%d rgb %u*src %u*dst eq %u, a %u*src %u*dst eq %u;", k, (unsigned)bl->src_rgb,
               (unsigned)bl->dst_rgb, (unsigned)bl->eq_rgb, (unsigned)bl->src_a, (unsigned)bl->dst_a, (unsigned)bl->eq_a);
        }
    sp(b, " blend color %g %g %g %g, logic op %u", (double)ex->blend_color[0], (double)ex->blend_color[1],
       (double)ex->blend_color[2], (double)ex->blend_color[3], (unsigned)s->color.logic_op);
    if (ex->has_scissor) sp(b, ", scissor %d,%d %dx%d", ex->scissor.x, ex->scissor.y, ex->scissor.w, ex->scissor.h);
    else sp(b, ", no scissor");
    sp(b, ", vp0 %g,%g %gx%g\n", (double)ex->vp[0][0], (double)ex->vp[0][1], (double)ex->vp[0][2], (double)ex->vp[0][3]);
    describe_bindings(b, ex, info);
    // The fragment stage's uniform block 3 as of the submit (the GPU's view), first 32 vec4s: MUA3's forward pass reads
    // its fog strength from [22].w.
    const p_bind_buf *u3 = &ex->ubo[1][0];   // NVN stage 1: fragment
    uint64_t n3 = u3->size < 512 ? u3->size & ~15ull : 512;
    const uint8_t *c3 = u3->addr && n3 ? vk_usnap_bytes(u3->addr, n3) : NULL;
    if (c3) {
        sp(b, "    fs c3 at %#llx (%llu bytes):", (unsigned long long)u3->addr, (unsigned long long)u3->size);
        for (uint64_t k = 0; k < n3 / 16; k++) {
            float f[4];
            memcpy(f, c3 + 16 * k, 16);
            sp(b, " [%llu] %g %g %g %g;", (unsigned long long)k, f[0], f[1], f[2], f[3]);
        }
        sp(b, "\n");
    } else if (u3->addr) {
        sp(b, "    fs c3 at %#llx (%llu bytes): no submit copy\n", (unsigned long long)u3->addr, (unsigned long long)u3->size);
    }
}

void vk_dump_draw(VkExec *ex, const VkPipeState *s, VkPipeline p, const VkPipeInfo *info) {
    plat_mutex_lock(&rec_lock);
    snap_draw = 0;
    if (!active()) goto out;
    pass_draws++;
    if (draws_f && frame_now() == dump_frame && snaps < DRAW_SNAP_MAX &&
        (snap_pass || sh_wanted(shader_hash(s->vs)) || sh_wanted(shader_hash(s->fs)))) {
        // a chosen draw: its whole description, with every uniform block and texture, goes to draws.txt after it
        snap_draw = 1;
        draw_text.n = 0;
        sp(&draw_text, "pass %04d draw %04d: ", seq, pass_draws - 1);
        int du = dump_ubo;
        dump_ubo = du > 64 ? du : 64, force_tex = 1;
        state_text(&draw_text, ex, s, info);
        dump_ubo = du, force_tex = 0;
        memcpy(snap_vattr, s->vattr, sizeof snap_vattr);
        memcpy(snap_vstream, s->vstream, sizeof snap_vstream);
    }
    uint64_t th = bindings_hash(ex, info);
    for (int i = 0; i < nents; i++)
        if (ents[i].p == p && ents[i].th == th) {
            ents[i].draws++;
            goto out;
        }
    if (nents == MAX_ENTS) {
        ents_over++;
        goto out;
    }
    Str b = {0};
    state_text(&b, ex, s, info);
    ents[nents++] = (Ent){p, th, 1, b.s};
out:
    plat_mutex_unlock(&rec_lock);
}

// Copies target t as <pre>_<tag>_... and writes its line to out; skip: why nothing is copied, or NULL. A depth-stencil
// target also gives its stencil (<tag>s, s8).
static void target_copy(VkExec *ex, const VkExecTarget *t, const char *tag, const char *pre, const char *skip, FILE *out) {
    char fn[16], name[160];
    if (!t->image) {
        fprintf(out, "  %s: nvn %#x target %d %ux%u: not renderable, left out\n", tag, t->tex->d.format,
                t->tex->d.target, t->tex->d.width, t->tex->d.height);
        return;
    }
    snprintf(name, sizeof name, "%s_%s_%ux%u_%d_%s", pre, tag, t->width, t->height, (int)t->format,
             fmt_name(t->format, fn));
    // a block view: the plane's blocks, read as the view format (one texel per block), decoded as the image's BC format
    uint32_t tw = t->tex->d.width >> t->level ? t->tex->d.width >> t->level : 1;
    uint32_t th = t->tex->d.height >> t->level ? t->tex->d.height >> t->level : 1;
    VkFormat bc = t->blocks && t->img && is_bc(t->img->format) ? t->img->format : VK_FORMAT_UNDEFINED;
    const char *why = skip ? skip : t->samples > 1 ? copy_msaa(ex, t, name) :
                      t->blocks ? copy_region(ex, t->image, t->format, t->aspect, t->level, t->is3d ? 0 : t->base_layer,
                                              t->is3d ? t->base_layer : 0, (VkExtent3D){tw, th, 1}, t->width,
                                              t->height, 1, name, bc) :
                      t->is3d ? "3D slice (the volume is dumped as lut<id> where it is sampled)" :
                      copy_out(ex, t->image, t->format, t->aspect, t->level, t->base_layer, t->width, t->height, 1, name);
    fprintf(out, "  %s: nvn %#x %s(%d) %ux%u level %u layers %u+%u samples %u addr %#llx -> %s.png%s%s%s\n", tag,
            t->nvn_format, fmt_name(t->format, fn), (int)t->format, t->width, t->height, t->level, t->base_layer,
            t->layers, t->samples, (unsigned long long)(uintptr_t)t->tex->mem, name,
            why ? "" : t->samples > 1 ? " (resolved)" : bc ? " (BC decoded)" : "", why ? " skipped: " : "", why ? why : "");
    if (skip || t->samples > 1 || t->is3d || t->blocks || !(t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ||
        !(t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return;
    snprintf(name, sizeof name, "%s_%ss_%ux%u_s8", pre, tag, t->width, t->height);
    why = copy_region(ex, t->image, VK_FORMAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT, t->level, t->base_layer, 0,
                      (VkExtent3D){t->width, t->height, 1}, t->width, t->height, 1, name, VK_FORMAT_UNDEFINED);
    fprintf(out, "  %ss: stencil of %s -> %s.png%s%s\n", tag, tag, name, why ? " skipped: " : "", why ? why : "");
}

// Every target of the current pass, as target_copy does it
static void targets_copy(VkExec *ex, const char *pre, const char *skip, FILE *out) {
    for (int i = 0; i < 8; i++) {
        if (!ex->color[i].tex) continue;
        char tag[8];
        snprintf(tag, sizeof tag, "t%d", i);
        target_copy(ex, &ex->color[i], tag, pre, skip, out);
    }
    if (ex->depth.tex) target_copy(ex, &ex->depth, "td", pre, skip, out);
}

// vk_exec_pass_begin, before the pass begins: a pass chosen by number copies its targets as they are before it.
void vk_dump_pass_begin(VkExec *ex) {
    if (snapping) return;
    plat_mutex_lock(&rec_lock);
    snap_pass = !finished && draws_f && frame_now() == dump_frame && pass_wanted(seq);
    if (snap_pass) {
        static int before_done = -1;   // a pass that begins again after an in-pass barrier gets a new number
        if (before_done != seq) {
            before_done = seq;
            vk_exec_barrier(ex);   // feedback-layout targets back to GENERAL, the layout the copies use
            char pre[48];
            snprintf(pre, sizeof pre, "pass_%04d_before", seq);
            fprintf(draws_f, "pass %04d before it begins:\n", seq);
            targets_copy(ex, pre, NULL, draws_f);
            fflush(draws_f);
        }
    }
    plat_mutex_unlock(&rec_lock);
}

// After a chosen draw (vk_dump_draw set snap_draw): the pass is ended, the targets, vertex streams, index buffer,
// indirect arguments and storage buffers are copied, and the pass begins again. what: the draw call and its arguments.
void vk_dump_draw_done(VkExec *ex, const char *what, uint64_t ibuf, uint64_t ibytes, uint64_t ind, uint64_t ind_bytes) {
    if (!snap_draw) return;
    snap_draw = 0;
    int reopen = 0;
    snapping = 1;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);   // ends the pass: copies cannot run inside one
    snapping = 0;
    plat_mutex_lock(&rec_lock);
    if (!draws_f) {   // the dump ended in between
        plat_mutex_unlock(&rec_lock);
        snapping = 1;
        vk_exec_inline_end(reopen);
        snapping = 0;
        return;
    }
    int n = seq, d = pass_draws - 1;
    snaps++;
    fputs(draw_text.s ? draw_text.s : "", draws_f);
    fprintf(draws_f, "  call: %s\n", what);
    if (cb) {
        char pre[48], file[200];
        uint64_t got;
        const char *why;
        snprintf(pre, sizeof pre, "pass_%04d_d%04d", n, d);
        targets_copy(ex, pre, NULL, draws_f);
        uint32_t streams = 0;
        for (int a = 0; a < 16; a++) {
            va_st v;
            memcpy(&v, &snap_vattr[a], sizeof v);
            if (!v.fmt) continue;
            streams |= 1u << v.stream;
            fprintf(draws_f, "  attr %d: nvn format %#x stream %u offset %#x\n", a, (unsigned)v.fmt, (unsigned)v.stream,
                    (unsigned)v.offset);
        }
        for (int s = 0; s < 16; s++) {
            if (!(streams >> s & 1)) continue;
            const NVNbufferRange *r = &ex->vbuf[s];
            snprintf(file, sizeof file, "%s_vb%d.bin", pre, s);
            fprintf(draws_f, "  stream %d: stride %u divisor %u addr %#llx size %#llx", s, snap_vstream[s].stride,
                    snap_vstream[s].divisor, (unsigned long long)r->address, (unsigned long long)r->size);
            why = copy_buffer(ex, r->address, r->size, file, &got);
            bin_note(draws_f, file, why, got, r->size);
        }
        if (ibuf) {
            snprintf(file, sizeof file, "%s_ib.bin", pre);
            fprintf(draws_f, "  index buffer addr %#llx", (unsigned long long)ibuf);
            why = copy_buffer(ex, ibuf, ibytes, file, &got);
            bin_note(draws_f, file, why, got, ibytes);
        }
        if (ind) {
            snprintf(file, sizeof file, "%s_indirect.bin", pre);
            fprintf(draws_f, "  indirect arguments addr %#llx", (unsigned long long)ind);
            why = copy_buffer(ex, ind, ind_bytes, file, &got);
            bin_note(draws_f, file, why, got, ind_bytes);
        }
        static const char *stn[2] = {"vs", "fs"};
        for (int st = 0; st < 2; st++)
            for (int k = 0; k < 16; k++) {
                const p_bind_buf *b = &ex->ssbo[st][k];
                if (!b->addr) continue;
                snprintf(file, sizeof file, "%s_%s_ssbo%d.bin", pre, stn[st], k);
                fprintf(draws_f, "  %s ssbo %d addr %#llx size %#llx", stn[st], k, (unsigned long long)b->addr,
                        (unsigned long long)b->size);
                why = copy_buffer(ex, b->addr, b->size, file, &got);
                bin_note(draws_f, file, why, got, b->size);
            }
    } else {
        fprintf(draws_f, "  not recorded by the replay thread: no snapshot\n");
    }
    fflush(draws_f);
    plat_mutex_unlock(&rec_lock);
    snapping = 1;
    vk_exec_inline_end(reopen);
    snapping = 0;
}

static uint64_t targets_sig(const VkExec *ex) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        uint64_t v[3] = {(uint64_t)(uintptr_t)t->image, t->level, t->base_layer};
        for (int k = 0; k < 3; k++) h = (h ^ v[k]) * 1099511628211ull;
    }
    return h;
}

// Sets pass_skip for the pass or dispatch being logged and counts it when it copies.
static void pass_decide(int same_targets) {
    if (frame_now() != dump_frame) copied = 0;
    pass_skip = copied >= MAX_PASSES ? "pass cap" : same_targets ? "same targets as the previous pass" : NULL;
    if (!pass_skip) copied++;
}

void vk_dump_pass_end(VkExec *ex) {
    if (snapping) return;   // vk_dump_draw_done ends the pass for its copies: the same pass goes on
    plat_mutex_lock(&rec_lock);
    if (!active()) goto out;
    snap_pass = 0;
    int n = seq++;
    uint64_t sig = targets_sig(ex);
    pass_decide(skip_runs && sig == last_targets);
    last_targets = sig;
    fprintf(passes_f, "pass %04d: render pass, draws %d, area %ux%u layers %u, fence %llu, frame %llu\n", n, pass_draws,
            ex->pass_w, ex->pass_h, ex->pass_layers, (unsigned long long)ex->fence, (unsigned long long)frame_now());
    if (!pass_skip && ex->nfbl) vk_exec_barrier(ex);   // feedback-layout targets back to GENERAL, the copies' layout
    char pre[24];
    snprintf(pre, sizeof pre, "pass_%04d", n);
    targets_copy(ex, pre, pass_skip, passes_f);
    {
        int any = 0;
        for (int i = 0; i < VK_DROP_N; i++)
            if (drop_cnt[i]) {
                fprintf(passes_f, "%s %s %d", any ? "," : "  draws dropped:", drop_names[i], drop_cnt[i]);
                any = 1, drop_cnt[i] = 0;
            }
        if (any) fputs("\n", passes_f);
    }
    for (int i = 0; i < nents; i++) {
        fprintf(passes_f, "  pipe %d (draws %d): %s", i, ents[i].draws, ents[i].text);
        free(ents[i].text);
    }
    if (ents_over) fprintf(passes_f, "  ... %d more draws with other pipeline/texture sets\n", ents_over);
    if (!pass_skip) flush_luts(ex, n);
    else nluts = 0;
    buf_snap(ex, n);
    fflush(passes_f);
    nents = ents_over = pass_draws = 0;
out:
    plat_mutex_unlock(&rec_lock);
}

void vk_dump_dispatch(VkExec *ex, const GfxShader *cs, VkPipeline p, const VkPipeInfo *info, const p_dispatch *d) {
    (void)p;
    plat_mutex_lock(&rec_lock);
    if (!active()) goto out;
    int n = seq++;
    pass_decide(0);
    last_targets = 0;
    uint64_t csh = shader_hash(cs);
    int chosen = draws_f && frame_now() == dump_frame && (pass_wanted(n) || sh_wanted(csh));
    Str b = {0};
    int du = dump_ubo;
    if (chosen) dump_ubo = du > 64 ? du : 64;
    describe_bindings(&b, ex, info);
    dump_ubo = du;
    fprintf(passes_f, "pass %04d: dispatch %dx%dx%d cs %016llx prog %llu caller %#llx, fence %llu\n%s", n, d->x, d->y,
            d->z, (unsigned long long)csh, (unsigned long long)(ex->prog ? ex->prog->serial : 0),
            (unsigned long long)d->caller, (unsigned long long)ex->fence, b.s ? b.s : "");
    free(b.s);
    for (int k = 0; chosen && k < 16; k++) {   // TOTK_DUMP_PASS: the storage buffers as the dispatch left them
        const p_bind_buf *sb = &ex->ssbo[5][k];
        if (!sb->addr) continue;
        char file[64];
        uint64_t got;
        snprintf(file, sizeof file, "pass_%04d_ssbo%d.bin", n, k);
        fprintf(passes_f, "  ssbo %d addr %#llx size %#llx after the dispatch", k, (unsigned long long)sb->addr,
                (unsigned long long)sb->size);
        const char *why = copy_buffer(ex, sb->addr, sb->size, file, &got);
        bin_note(passes_f, file, why, got, sb->size);
    }
    for (uint32_t i = 0; i < info->n_bindings && !pass_skip; i++) {   // storage images the dispatch writes
        const VkPipeBinding *bd = &info->bindings[i];
        if (!bd->res || bd->kind != GFX_RES_IMG || !(bd->res->flags & 4) || bd->type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            continue;
        uint64_t h = handle_of(ex, 5, bd->res->slot, 1);
        view_t v;
        int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (!t) continue;
        uint32_t lv = hv && v.set_levels ? v.base_level : 0, layer = hv && v.set_layers ? v.min_layer : 0;
        if (lv >= t->levels || layer >= t->layers) continue;
        uint32_t w = t->width >> lv ? t->width >> lv : 1, hh = t->height >> lv ? t->height >> lv : 1;
        uint32_t dd = t->type == VK_IMAGE_TYPE_3D ? (t->depth >> lv ? t->depth >> lv : 1) : 1;
        char fn[16], name[160];
        snprintf(name, sizeof name, "pass_%04d_img%d_%ux%u%s_%d_%s", n, bd->res->slot, w, hh, dd > 1 ? "x3d" : "",
                 (int)t->format, fmt_name(t->format, fn));
        if (t->type == VK_IMAGE_TYPE_3D) {   // a written volume: this copy replaces the sampled-3D one
            for (int k = 0; k < nluts_done; k++)
                if (luts_done[k] == t->image) luts_done[k] = VK_NULL_HANDLE;
        }
        const char *why = t->samples > 1 ? "multisampled" :
                          copy_out(ex, t->image, t->format, t->aspect, lv, layer, w, hh, dd, name);
        fprintf(passes_f, "  writes img slot %d level %u layer %u depth %u -> %s.png%s%s\n", bd->res->slot, lv, layer,
                dd, name, why ? " skipped: " : "", why ? why : "");
    }
    if (!pass_skip) flush_luts(ex, n);
    else nluts = 0;
    buf_snap(ex, n);
    fflush(passes_f);
out:
    plat_mutex_unlock(&rec_lock);
}

static const char *tex_desc(const tex_t *t, char *buf, size_t n) {
    if (!t) return "null";
    snprintf(buf, n, "addr %#llx nvn-fmt %#x %ux%ux%u", (unsigned long long)(uintptr_t)t->mem, t->d.format, t->d.width,
             t->d.height, t->d.depth);
    return buf;
}

static void region_desc(const NVNcopyRegion *r, char *buf, size_t n) {
    snprintf(buf, n, "(%d,%d,%d %dx%dx%d)", r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth);
}

void vk_dump_copy(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex;
    plat_mutex_lock(&rec_lock);
    if (!active()) goto out;
    char a[96], b[96], ra[64], rb[64];
    switch (op) {
    case OP_COPY_B2B: {
        const p_copy_b2b *p = payload;
        fprintf(passes_f, "copy after pass %d: buffer %#llx -> buffer %#llx, %llu bytes\n", seq - 1,
                (unsigned long long)p->src, (unsigned long long)p->dst, (unsigned long long)p->size);
        break;
    }
    case OP_COPY_B2T: case OP_COPY_T2B: {
        const p_copy_b2t *p = payload;
        region_desc(&p->r, ra, sizeof ra);
        fprintf(passes_f, "copy after pass %d: %s buffer %#llx %s texture %s region %s flags %#x\n", seq - 1,
                op == OP_COPY_B2T ? "buffer->texture" : "texture->buffer", (unsigned long long)p->src,
                op == OP_COPY_B2T ? "->" : "<-", tex_desc(p->tex, a, sizeof a), ra, p->flags);
        break;
    }
    case OP_COPY_T2T: {
        const p_copy_t2t *p = payload;
        region_desc(&p->sr, ra, sizeof ra);
        region_desc(&p->dr, rb, sizeof rb);
        fprintf(passes_f, "copy after pass %d: texture %s %s -> texture %s %s flags %#x\n", seq - 1,
                tex_desc(p->src, a, sizeof a), ra, tex_desc(p->dst, b, sizeof b), rb, p->flags);
        break;
    }
    case OP_CLEAR_TEX: {
        const p_clear_tex *p = payload;
        region_desc(&p->r, ra, sizeof ra);
        fprintf(passes_f, "clear after pass %d: texture %s region %s mask %#x\n", seq - 1, tex_desc(p->tex, a, sizeof a),
                ra, p->mask);
        break;
    }
    default: break;
    }
out:
    plat_mutex_unlock(&rec_lock);
}

static void make_dirs(char *path) { plat_file_mkdir_p(path); }

static void dump_start(uint64_t frame);

void vk_dump_init(void) {
    const char *e = cfg_get("TOTK_DUMP");
    uint64_t f = e && *e ? strtoull(e, NULL, 10) : 0;
    if (f) dump_start(f);
}

// F7 in the game window (gfx_window_win.c): the frame three presents from now is dumped, as TOTK_DUMP=<that frame>
// would; once per run.
void vk_dump_key(void) {
    static int used;
    if (used || vk_dump_armed) return;
    used = 1;
    dump_start(frame_now() + 3);
}

static void dump_start(uint64_t frame) {
    dump_frame = frame;
    const char *lf = cfg_get("TOTK_DUMP_LOG");
    if (lf && *lf) log_from = strtoull(lf, NULL, 10);
    const char *sr = cfg_get("TOTK_DUMP_SKIP_RUNS");
    skip_runs = sr && *sr && *sr != '0';
    const char *du = cfg_get("TOTK_DUMP_UBO");
    dump_ubo = du && *du ? atoi(du) : 0;
    if (dump_ubo == 1) dump_ubo = 16;
    if (dump_ubo > 4096) dump_ubo = 4096;
    if (dump_ubo < 0) dump_ubo = 0;
    const char *wt = cfg_get("TOTK_DUMP_TEX");
    for (char *q = (char *)wt; q && *q && nwant_tex < 16; q += *q == ',') {
        uint32_t *w = want_tex[nwant_tex++];
        w[0] = (uint32_t)strtoul(q, &q, 10);
        w[1] = *q == 'x' ? (uint32_t)strtoul(q + 1, &q, 10) : 0;
    }
    const char *rw = cfg_get("TOTK_DUMP_RAW");
    dump_raw = !(rw && *rw == '0');
    const char *wp = cfg_get("TOTK_DUMP_PASS");   // numbers and sh:<hex>, separated by commas
    for (const char *q = wp; q && *q;) {
        while (*q == ',' || *q == ' ') q++;
        if (!*q) break;
        char *e;
        if (!strncmp(q, "sh:", 3)) {
            uint64_t h = strtoull(q + 3, &e, 16);
            if (nwant_sh < 16) want_sh[nwant_sh++] = h;
        } else {
            long v = strtol(q, &e, 10);
            if (e != q && nwant_pass < 32) want_pass[nwant_pass++] = (int)v;
        }
        if (e == q) e = (char *)q + 1;
        q = e;
    }
    const char *d = cfg_get("TOTK_DUMP_DIR");
    snprintf(dir, sizeof dir, "%s", d && *d ? d : "dump");
    make_dirs(dir);
    char path[600];
    snprintf(path, sizeof path, "%s/passes.txt", dir);
    passes_f = fopen(path, "w");
    snprintf(path, sizeof path, "%s/images.txt", dir);
    images_f = fopen(path, "w");
    if (!passes_f || !images_f) rt_fatal("vk_dump: cannot write to %s", dir);
    if (nwant_pass || nwant_sh) {
        snprintf(path, sizeof path, "%s/draws.txt", dir);
        if (!(draws_f = fopen(path, "w"))) rt_fatal("vk_dump: cannot write to %s", dir);
    }
    const char *bf = cfg_get("TOTK_DUMP_BUF");
    if (bf && *bf) {
        char *q;
        bd_addr = strtoull(bf, &q, 0);
        if (*q == ':') bd_size = strtoull(q + 1, &q, 0);
        if (*q == ':') bd_off = strtoull(q + 1, &q, 0);
        if (*q == ':') bd_stride = strtoull(q + 1, &q, 0) & ~3ull;
        if (*q == ':') bd_w = (uint32_t)strtoul(q + 1, &q, 0);
        if (*q == ':') bd_h = (uint32_t)strtoul(q + 1, &q, 0);
        if (!bd_w || !bd_h) bd_stride = 0;
        snprintf(path, sizeof path, "%s/bufs.txt", dir);
        if (bd_size && !(bufs_f = fopen(path, "w"))) bd_size = 0;
    }
    vk_dump_armed = 1;
    rt_log("vk_dump: frame %llu goes to %s", (unsigned long long)dump_frame, dir);
}

void vk_dump_present(uint64_t frame) {
    if (frame != dump_frame) return;
    plat_mutex_lock(&rec_lock);
    finished = 1;
    plat_mutex_unlock(&rec_lock);
    vk_sync_finish();   // the frame's copies are converted and written
    plat_mutex_lock(&img_lock);
    fclose(images_f);
    images_f = NULL;
    if (bufs_f) fclose(bufs_f), bufs_f = NULL;
    plat_mutex_unlock(&img_lock);
    fclose(passes_f);
    passes_f = NULL;
    plat_mutex_lock(&rec_lock);
    if (draws_f) fclose(draws_f), draws_f = NULL;
    plat_mutex_unlock(&rec_lock);
    rt_log("vk_dump: frame %llu: %d passes and dispatches, %d draws snapshotted, about %llu MB of PNGs, %llu MB raw, "
           "%llu MB of buffers in %s", (unsigned long long)frame, seq, snaps, (unsigned long long)(png_bytes >> 20),
           (unsigned long long)(raw_bytes >> 20), (unsigned long long)(bin_bytes >> 20), dir);
}
