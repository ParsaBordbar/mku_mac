/*
 * Software renderer for the PSP GE: draws straight into emulated VRAM, like
 * the real chip. Field layouts follow PPSSPP's GPU/GPUState.h and
 * GPU/ge_constants.h; the pipeline is our own and deliberately simple.
 *
 *   PRIM ──► decode vertices ──► transform (skip in "through" 2D mode)
 *        ──► assemble triangles / sprites ──► rasterise
 *        ──► per pixel: texture, texture function, alpha/stencil/depth tests,
 *            blend, colour mask ──► write the framebuffer in its pixel format
 *
 * Not yet: lighting, fog, texture projection/env-map modes, mipmaps, splines,
 * logic ops, dithering, proper near-plane clipping (triangles behind the
 * camera are dropped).
 */
#include "gpu.h"
#include "ge_cmds.h"
#include "../host/host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VERTS 65536

uint64_t g_raster_us;   /* time spent rasterising (MKU_FPS stats) */
uint64_t g_tris_queued, g_tris_culled_w, g_draws_3d;   /* MKU_FPS stats */
uint64_t g_area_kind[3];   /* bounding-box pixels: clears, sprites, triangles */
int g_dump_draws;   /* set by present.c for the MKU_GE_DUMP frame */
extern uint64_t now_us(void);
void gpu_flush(void);

typedef struct {
    float x, y, z, w;      /* screen x/y (pixels), depth (0..65535), 1/w for perspective */
    float u, v;            /* texture coords in texels */
    float r, g, b, a;      /* colour 0..255 */
} Vert;

static Vert g_verts[MAX_VERTS];

/* ------------------------------------------------------------------------ */
/* stats (MKU_GE_STATS=1) */

static unsigned g_prims[8], g_through[2], g_transfers, g_texfmt[16];
static int g_warned[16];
#define WARN_ONCE(id, ...) do { if (!g_warned[id]++) fprintf(stderr, "[gpu] " __VA_ARGS__); } while (0)

extern uint64_t g_fallback[3];
extern uint32_t g_fallback_mask, g_fallback_blend;

void gpu_draw_report(void) {
    fprintf(stderr, "[gpu] reference-path pixels: stencil %llu, blend %llu (last mode 0x%X), colour mask %llu (last 0x%08X)\n",
            (unsigned long long)g_fallback[0], (unsigned long long)g_fallback[1], g_fallback_blend,
            (unsigned long long)g_fallback[2], g_fallback_mask);
    static const char *pn[] = {"points", "lines", "linestrip", "triangles", "tristrip", "trifan", "sprites", "?"};
    fprintf(stderr, "[gpu] prims:");
    for (int i = 0; i < 8; i++) if (g_prims[i]) fprintf(stderr, " %s=%u", pn[i], g_prims[i]);
    fprintf(stderr, "\n[gpu] 2D (through) draws: %u, 3D draws: %u, block transfers: %u\n", g_through[1], g_through[0], g_transfers);
    fprintf(stderr, "[gpu] textured draws by format:");
    for (int i = 0; i < 16; i++) if (g_texfmt[i]) fprintf(stderr, " fmt%d=%u", i, g_texfmt[i]);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------------ */
/* colours */

typedef struct { int r, g, b, a; } Rgba;

/* 5/6-bit to 8-bit expansion without a division per pixel */
static const uint8_t k5[32] = {0, 8, 16, 25, 33, 41, 49, 58, 66, 74, 82, 90, 99, 107, 115, 123,
                               132, 140, 148, 156, 165, 173, 181, 189, 197, 206, 214, 222, 230, 239, 247, 255};
static const uint8_t k6[64] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 45, 49, 53, 57, 61, 65, 69, 73, 77, 81, 85,
                               89, 93, 97, 101, 105, 109, 113, 117, 121, 125, 130, 134, 138, 142, 146, 150, 154, 158,
                               162, 166, 170, 174, 178, 182, 186, 190, 194, 198, 202, 206, 210, 215, 219, 223, 227,
                               231, 235, 239, 243, 247, 251, 255};

static inline __attribute__((always_inline)) Rgba unpack(uint32_t p, int fmt) {   /* fmt: 0=565 1=5551 2=4444 3=8888 */
    Rgba c;
    switch (fmt) {
    case 0:
        c.r = k5[p & 31]; c.g = k6[(p >> 5) & 63]; c.b = k5[(p >> 11) & 31]; c.a = 255;
        break;
    case 1:
        c.r = k5[p & 31]; c.g = k5[(p >> 5) & 31]; c.b = k5[(p >> 10) & 31]; c.a = (p >> 15) ? 255 : 0;
        break;
    case 2:
        c.r = (p & 15) * 17; c.g = ((p >> 4) & 15) * 17; c.b = ((p >> 8) & 15) * 17; c.a = ((p >> 12) & 15) * 17;
        break;
    default:
        c.r = p & 255; c.g = (p >> 8) & 255; c.b = (p >> 16) & 255; c.a = p >> 24;
        break;
    }
    return c;
}

static inline __attribute__((always_inline)) uint32_t pack(Rgba c, int fmt) {
    switch (fmt) {
    case 0: return (uint32_t)((c.r >> 3) | ((c.g >> 2) << 5) | ((c.b >> 3) << 11));
    case 1: return (uint32_t)((c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) | ((c.a >> 7) << 15));
    case 2: return (uint32_t)((c.r >> 4) | ((c.g >> 4) << 4) | ((c.b >> 4) << 8) | ((c.a >> 4) << 12));
    default: return (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | ((uint32_t)c.a << 24);
    }
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ------------------------------------------------------------------------ */
/* vertex decoding */

typedef struct {
    int tc, col, nrm, pos, wt, idx, nweights, nmorph, through;
    int o_wt, o_tc, o_col, o_nrm, o_pos, size;
} VFormat;

static int align_up(int x, int a) { return (x + a - 1) & ~(a - 1); }

static VFormat vformat(uint32_t vt) {
    static const int tc_size[4] = {0, 2, 4, 8}, tc_align[4] = {1, 1, 2, 4};
    static const int col_size[8] = {0, 0, 0, 0, 2, 2, 2, 4}, col_align[8] = {1, 1, 1, 1, 2, 2, 2, 4};
    static const int xyz_size[4] = {0, 3, 6, 12}, xyz_align[4] = {1, 1, 2, 4};
    static const int wt_size[4] = {0, 1, 2, 4};
    VFormat f = {
        .tc = vt & 3, .col = (vt >> 2) & 7, .nrm = (vt >> 5) & 3, .pos = (vt >> 7) & 3,
        .wt = (vt >> 9) & 3, .idx = (vt >> 11) & 3, .nweights = ((vt >> 14) & 7) + 1,
        .nmorph = ((vt >> 18) & 7) + 1, .through = (vt >> 23) & 1,
    };
    int off = 0, maxa = 1;
#define FIELD(o, size, al) do { if (size) { off = align_up(off, al); o = off; off += size; if (al > maxa) maxa = al; } } while (0)
    FIELD(f.o_wt, f.wt ? wt_size[f.wt] * f.nweights : 0, wt_size[f.wt] ? wt_size[f.wt] : 1);
    FIELD(f.o_tc, tc_size[f.tc], tc_align[f.tc]);
    FIELD(f.o_col, col_size[f.col], col_align[f.col]);
    FIELD(f.o_nrm, xyz_size[f.nrm], xyz_align[f.nrm]);
    FIELD(f.o_pos, xyz_size[f.pos], xyz_align[f.pos]);
#undef FIELD
    f.size = align_up(off, maxa);
    return f;
}

/* Component i of a signed 8/16-bit or float triple. Outside through mode the
 * integer formats are fixed point: s8 / 128, s16 / 32768. */
static float read_comp(uint32_t a, int fmt, int i, int through) {
    switch (fmt) {
    case 1: { int8_t v = (int8_t)MEM_LBU(a + (uint32_t)i); return through ? (float)v : v / 128.f; }
    case 2: { int16_t v = (int16_t)MEM_LHU(a + 2u * (uint32_t)i); return through ? (float)v : v / 32768.f; }
    case 3: { uint32_t w = MEM_LW(a + 4 * i); float f; memcpy(&f, &w, 4); return f; }
    }
    return 0.f;
}

static void mul43(const float *m, const float in[3], float out[3]) {   /* 4x3 column-major + translation */
    for (int i = 0; i < 3; i++) out[i] = in[0] * m[i] + in[1] * m[3 + i] + in[2] * m[6 + i] + m[9 + i];
}

/* Decode one vertex (with morphing and skinning) into object/screen space. */
static void decode_vertex(const VFormat *f, uint32_t addr, float pos[3], float uv[2], Rgba *col) {
    float p[3] = {0}, t[2] = {0}, c[4] = {0};
    int has_col = f->col >= 4;
    for (int m = 0; m < f->nmorph; m++) {
        uint32_t a = addr + (uint32_t)(m * f->size);
        float w = f->nmorph > 1 ? ge.morph[m] : 1.f;
        if (f->tc) {
            for (int i = 0; i < 2; i++) {
                float v;
                if (f->tc == 1) v = MEM_LBU(a + f->o_tc + i) / (f->through ? 1.f : 128.f);
                else if (f->tc == 2) v = MEM_LHU(a + f->o_tc + 2 * i) / (f->through ? 1.f : 32768.f);
                else v = read_comp(a + f->o_tc, 3, i, 0);
                t[i] += v * w;
            }
        }
        if (has_col) {
            uint32_t raw = f->col == 7 ? MEM_LW(a + f->o_col) : MEM_LHU(a + f->o_col);
            Rgba cc = unpack(raw, f->col == 4 ? 0 : f->col == 5 ? 1 : f->col == 6 ? 2 : 3);
            c[0] += cc.r * w; c[1] += cc.g * w; c[2] += cc.b * w; c[3] += cc.a * w;
        }
        for (int i = 0; i < 3; i++) {
            float v;
            if (f->through) {   /* screen coordinates: x, y signed, z unsigned */
                if (f->pos == 2) v = i == 2 ? (float)MEM_LHU(a + f->o_pos + 4) : (float)(int16_t)MEM_LHU(a + f->o_pos + 2 * i);
                else v = read_comp(a + f->o_pos, f->pos, i, 1);
            } else {
                v = read_comp(a + f->o_pos, f->pos, i, 0);
            }
            p[i] += v * w;
        }
    }
    if (f->wt && !f->through) {   /* skinning: blend the position through up to 8 bone matrices */
        float sk[3] = {0};
        for (int b = 0; b < f->nweights; b++) {
            float w = f->wt == 1 ? MEM_LBU(addr + f->o_wt + b) / 128.f
                    : f->wt == 2 ? MEM_LHU(addr + f->o_wt + 2 * b) / 32768.f
                    : read_comp(addr + f->o_wt, 3, b, 0);
            float o[3];
            mul43(&ge.bone[12 * b], p, o);
            for (int i = 0; i < 3; i++) sk[i] += o[i] * w;
        }
        memcpy(p, sk, sizeof p);
    }
    memcpy(pos, p, sizeof p);
    uv[0] = t[0];
    uv[1] = t[1];
    if (has_col) *col = (Rgba){(int)c[0], (int)c[1], (int)c[2], (int)c[3]};
    else {   /* no vertex colour: material diffuse/alpha */
        Rgba m = unpack(ge.cmd[GE_MATERIALDIFFUSE] | ((ge.cmd[GE_MATERIALALPHA] & 0xFF) << 24), 3);
        *col = m;
    }
}

/* ------------------------------------------------------------------------ */
/* textures */

typedef struct {
    uint32_t addr;
    int w, h, stride, fmt, swizzled, clamp_u, clamp_v, linear;
    const uint32_t *px;   /* decoded RGBA8888 (R in the low byte), pw*ph */
    int pw, ph, sc;       /* size of px: w*sc x h*sc (sc > 1: HD replacement or upscaled, texpack.c) */
} Texture;

static uint8_t g_clut[1024 * 4];

static uint32_t g_clut_version;   /* bumps whenever the palette changes */

/* LOADCLUT: the palette is copied now (games reuse its memory right after).
 * g_clut_version is a hash of its contents, so textures drawn with palettes
 * that alternate (one per fighter) each keep their own cache entry. */
static void load_clut(void) {
    uint32_t addr = (ge.cmd[GE_CLUTADDR] & 0x00FFFFF0) | ((ge.cmd[GE_CLUTADDRUPPER] << 8) & 0x0F000000);
    uint32_t blocks = (ge.cmd[GE_LOADCLUT] & 0x7F) == 0x40 ? 0x40 : ge.cmd[GE_LOADCLUT] & 0x3F;
    memcpy(g_clut, rt_ptr(addr), blocks * 32);
    uint64_t h = 1469598103934665603ull, w;
    for (uint32_t i = 0; i < blocks * 32; i += 8) { memcpy(&w, g_clut + i, 8); h = (h ^ w) * 1099511628211ull; }
    g_clut_version = (uint32_t)(h ^ (h >> 32)) ^ blocks;
}

static int bits_per_texel(int fmt) {
    static const int bpp[11] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};
    return fmt < 11 ? bpp[fmt] : 32;
}

/* Byte offset of the texel row/byte, honouring the PSP "swizzle": 16-byte x 8-row tiles. */
static uint32_t texel_offset(const Texture *t, int x_bytes, int y) {
    int stride_bytes = t->stride * bits_per_texel(t->fmt) / 8;
    if (!t->swizzled) return (uint32_t)(y * stride_bytes + x_bytes);
    int tiles_per_row = stride_bytes / 16;
    return (uint32_t)((((y >> 3) * tiles_per_row + (x_bytes >> 4)) << 7) + ((y & 7) << 4) + (x_bytes & 15));
}

static Rgba clut_lookup(uint32_t index) {
    uint32_t cf = ge.cmd[GE_CLUTFORMAT];
    int pal_fmt = cf & 3, shift = (cf >> 2) & 0x1F, mask = (cf >> 8) & 0xFF, start = ((cf >> 16) & 0x1F) << 4;
    uint32_t i = ((index >> shift) & (uint32_t)mask) | ((uint32_t)start & (pal_fmt == 3 ? 0xFF : 0x1FF));
    if (pal_fmt == 3) {
        uint32_t p;
        memcpy(&p, &g_clut[(i & 0xFF) * 4], 4);
        return unpack(p, 3);
    }
    uint16_t p;
    memcpy(&p, &g_clut[(i & 0x1FF) * 2], 2);
    return unpack(p, pal_fmt);
}

static Rgba fetch_texel(const Texture *t, int x, int y) {
    if (t->clamp_u) x = x < 0 ? 0 : x >= t->w ? t->w - 1 : x; else x &= t->w - 1;
    if (t->clamp_v) y = y < 0 ? 0 : y >= t->h ? t->h - 1 : y; else y &= t->h - 1;
    int bpp = bits_per_texel(t->fmt);
    if (t->fmt >= 8) {   /* DXT: 4x4 blocks */
        WARN_ONCE(0, "DXT textures not implemented yet\n");
        return (Rgba){255, 0, 255, 255};
    }
    if (bpp == 4) {
        uint8_t b = MEM_LBU(t->addr + texel_offset(t, x >> 1, y));
        return clut_lookup((x & 1) ? b >> 4 : b & 15);
    }
    uint32_t a = t->addr + texel_offset(t, x * bpp / 8, y);
    switch (t->fmt) {
    case 0: case 1: case 2: return unpack(MEM_LHU(a), t->fmt);
    case 3: return unpack(MEM_LW(a), 3);
    case 5: return clut_lookup(MEM_LBU(a));
    case 6: return clut_lookup(MEM_LHU(a));
    default: return clut_lookup(MEM_LW(a));
    }
}

/* ---- decoded texture cache ----
 * Each (address, format, size, palette) is decoded to RGBA8888 once. The PSP
 * signals texture changes with TEXFLUSH; after one (and once per frame) an
 * entry is re-validated by hashing its source bytes, and re-decoded only if
 * they changed. */
typedef struct {
    uint32_t addr, key, clut_version, verified_gen;
    int w, h, fmt, pw, ph;
    uint32_t last_used;
    uint64_t hash;
    uint32_t *px;
} TexEntry;

#define TEX_CACHE 4096
static TexEntry g_tex[TEX_CACHE];
static uint32_t g_tex_gen = 1, g_tex_clock;
uint64_t g_tex_decodes;   /* MKU_FPS stats */
extern int g_dump_draws;

void gpu_texture_flush(void) { g_tex_gen++; }

/* Forget every decoded texture (after switching HD textures on or off). */
void gpu_texture_cache_clear(void) {
    gpu_flush();
    for (int i = 0; i < TEX_CACHE; i++) { free(g_tex[i].px); g_tex[i].px = NULL; }
}

static uint64_t hash_bytes(uint32_t addr, uint32_t n) {
    const uint8_t *p = rt_ptr(addr);
    uint64_t h = 1469598103934665603ull, w;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h = (h ^ w) * 1099511628211ull; }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void decode_texture(TexEntry *e, const Texture *t) {
    if (e->px) gpu_flush();   /* queued draws may still use the old pixels */
    Texture raw = *t;
    raw.clamp_u = raw.clamp_v = 0;
    free(e->px);
    e->px = malloc((size_t)t->w * (size_t)t->h * 4);
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            Rgba c = fetch_texel(&raw, x, y);
            e->px[y * t->w + x] = (uint32_t)c.r | (uint32_t)c.g << 8 | (uint32_t)c.b << 16 | (uint32_t)c.a << 24;
        }
    e->pw = t->w;
    e->ph = t->h;
    int ow, oh;
    uint32_t *hd = texpack_process(e->px, t->w, t->h, &ow, &oh);   /* dump / HD replacement / xBRZ */
    if (hd) {
        free(e->px);
        e->px = hd;
        e->pw = ow;
        e->ph = oh;
    }
}

static const uint32_t *cached_texture(Texture *t) {
    /* queued draws may still render into this texture (render-to-texture) */
    if ((t->addr & 0x0FE00000u) == (VRAM_BASE & 0x0FE00000u)) {
        gpu_flush();
        surf_writeback(t->addr, (uint32_t)t->stride * (uint32_t)t->h * (uint32_t)bits_per_texel(t->fmt) / 8);
    }
    uint32_t pal = t->fmt >= 4 && t->fmt <= 7;
    uint32_t key = (uint32_t)t->fmt | (uint32_t)t->w << 4 | (uint32_t)t->h << 16 | (uint32_t)t->swizzled << 30 |
                   (uint32_t)t->stride * 2654435761u;
    uint32_t clutv = pal ? g_clut_version ^ ge.cmd[GE_CLUTFORMAT] * 40503u : 0;
    uint32_t bytes = (uint32_t)t->stride * (uint32_t)t->h * (uint32_t)bits_per_texel(t->fmt) / 8;
    /* 4-way set associative: a texture can live in any of 4 entries of its set */
    uint32_t mix = (t->addr ^ key ^ clutv) * 0x9E3779B1u;   /* Fibonacci hashing: spread the bits */
    uint32_t set = ((mix >> 22) % (TEX_CACHE / 4)) * 4;
    TexEntry *e = NULL;
    for (int k = 0; k < 4 && !e; k++) {
        TexEntry *c = &g_tex[set + k];
        if (c->px && c->addr == t->addr && c->key == key && c->clut_version == clutv) e = c;
    }
    if (!e) {   /* miss: take an empty entry, else the least recently used */
        e = &g_tex[set];
        for (int k = 0; k < 4; k++) {
            TexEntry *c = &g_tex[set + k];
            if (!c->px) { e = c; break; }
            if (c->last_used < e->last_used) e = c;
        }
    }
    e->last_used = ++g_tex_clock;
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("MKU_NO_TEXCACHE") != NULL;
    if (nocache) e->verified_gen = 0;   /* debug: re-hash the source on every use */
    if (e->px && e->addr == t->addr && e->key == key && e->clut_version == clutv) {
        t->pw = e->pw; t->ph = e->ph; t->sc = e->pw / t->w;
        if (e->verified_gen == g_tex_gen) return e->px;
        uint64_t h = hash_bytes(t->addr, bytes);
        if (h == e->hash) { e->verified_gen = g_tex_gen; return e->px; }
        e->hash = h;
        if (g_dump_draws) fprintf(stderr, "[texlog] CHANGED addr=%08X %dx%d fmt=%d clut=%08X\n", t->addr, t->w, t->h, t->fmt, clutv);
    } else {
        e->hash = hash_bytes(t->addr, bytes);
        if (g_dump_draws) fprintf(stderr, "[texlog] MISS    addr=%08X %dx%d fmt=%d clut=%08X (evicting %08X)\n", t->addr, t->w, t->h, t->fmt, clutv, e->addr);
    }
    e->addr = t->addr; e->key = key; e->clut_version = clutv; e->w = t->w; e->h = t->h; e->fmt = t->fmt;
    e->verified_gen = g_tex_gen;
    decode_texture(e, t);
    g_tex_decodes++;
    t->pw = e->pw; t->ph = e->ph; t->sc = e->pw / t->w;
    return e->px;
}

static inline uint32_t texel(const Texture *t, int x, int y) {
    if (t->clamp_u) x = x < 0 ? 0 : x >= t->pw ? t->pw - 1 : x; else x &= t->pw - 1;
    if (t->clamp_v) y = y < 0 ? 0 : y >= t->ph ? t->ph - 1 : y; else y &= t->ph - 1;
    return t->px[y * t->pw + x];
}

static inline Rgba rgba_of(uint32_t p) { return (Rgba){(int)(p & 255), (int)((p >> 8) & 255), (int)((p >> 16) & 255), (int)(p >> 24)}; }

/* Bilinear (or nearest) texture lookup. The 2x2 footprint's wrapped/clamped
 * coordinates are worked out once, not per texel. */
static inline __attribute__((always_inline)) Rgba sample(const Texture *t, float u, float v) {
    if (t->sc > 1) { u *= (float)t->sc; v *= (float)t->sc; }   /* HD texture: texel space is sc times finer */
    if (!t->linear) return rgba_of(texel(t, (int)floorf(u), (int)floorf(v)));
    float fu = u - 0.5f, fv = v - 0.5f;
    float flu = floorf(fu), flv = floorf(fv);
    int x0 = (int)flu, y0 = (int)flv;
    int ax = (int)((fu - flu) * 256.f), ay = (int)((fv - flv) * 256.f);   /* 8-bit fixed-point weights */
    int xa, xb, ya, yb;
    if (t->clamp_u) {
        xa = x0 < 0 ? 0 : x0 >= t->pw ? t->pw - 1 : x0;
        xb = x0 + 1 < 0 ? 0 : x0 + 1 >= t->pw ? t->pw - 1 : x0 + 1;
    } else {
        xa = x0 & (t->pw - 1);
        xb = (x0 + 1) & (t->pw - 1);
    }
    if (t->clamp_v) {
        ya = y0 < 0 ? 0 : y0 >= t->ph ? t->ph - 1 : y0;
        yb = y0 + 1 < 0 ? 0 : y0 + 1 >= t->ph ? t->ph - 1 : y0 + 1;
    } else {
        ya = y0 & (t->ph - 1);
        yb = (y0 + 1) & (t->ph - 1);
    }
    const uint32_t *r0 = t->px + ya * t->pw, *r1 = t->px + yb * t->pw;
    uint32_t c00 = r0[xa], c10 = r0[xb], c01 = r1[xa], c11 = r1[xb];
    /* Per channel: ((c00*(256-ax) + c10*ax)*(256-ay) + (c01*(256-ax) + c11*ax)*ay + 32768) >> 16,
     * computed two channels at a time ("SIMD within a register"): R and B
     * share one word, G and A another. The horizontal step fits 16-bit lanes
     * (at most 255*256); the vertical step uses 32-bit lanes of a uint64. */
    const uint32_t m = 0x00FF00FFu;
    uint32_t iax = (uint32_t)(256 - ax), uax = (uint32_t)ax;
    uint32_t top_rb = (c00 & m) * iax + (c10 & m) * uax, top_ga = ((c00 >> 8) & m) * iax + ((c10 >> 8) & m) * uax;
    uint32_t bot_rb = (c01 & m) * iax + (c11 & m) * uax, bot_ga = ((c01 >> 8) & m) * iax + ((c11 >> 8) & m) * uax;
#define SPREAD(w) ((uint64_t)((w) & 0xFFFFu) | ((uint64_t)((w) >> 16) << 32))
    uint64_t iay = (uint64_t)(256 - ay), uay = (uint64_t)ay, round = 32768u | (32768ull << 32);
    uint64_t rb = (SPREAD(top_rb) * iay + SPREAD(bot_rb) * uay + round) >> 16;
    uint64_t ga = (SPREAD(top_ga) * iay + SPREAD(bot_ga) * uay + round) >> 16;
#undef SPREAD
    Rgba r;
    r.r = (int)(rb & 0xFFFF); r.b = (int)((rb >> 32) & 0xFFFF);
    r.g = (int)(ga & 0xFFFF); r.a = (int)((ga >> 32) & 0xFFFF);
    return r;
}

static Texture current_texture(void) {
    Texture t;
    t.addr = (ge.cmd[GE_TEXADDR0] & 0xFFFFF0) | ((ge.cmd[GE_TEXBUFWIDTH0] << 8) & 0x0F000000);
    t.stride = ge.cmd[GE_TEXBUFWIDTH0] & 0x7FF;
    t.w = 1 << (ge.cmd[GE_TEXSIZE0] & 0xF);
    t.h = 1 << ((ge.cmd[GE_TEXSIZE0] >> 8) & 0xF);
    t.fmt = ge.cmd[GE_TEXFORMAT] & 0xF;
    t.swizzled = ge.cmd[GE_TEXMODE] & 1;
    t.clamp_u = ge.cmd[GE_TEXWRAP] & 1;
    t.clamp_v = (ge.cmd[GE_TEXWRAP] >> 8) & 1;
    t.linear = (ge.cmd[GE_TEXFILTER] >> 8) & 1;   /* magnification filter */
    if (t.stride == 0) t.stride = t.w;
    t.pw = t.w; t.ph = t.h; t.sc = 1;
    t.px = cached_texture(&t);
    return t;
}

/* ------------------------------------------------------------------------ */
/* per-pixel pipeline */

typedef struct {
    uint32_t fb, zb;
    int fb_stride, zb_stride, fb_fmt;
    int sx1, sy1, sx2, sy2;
    int clear, clear_color, clear_alpha, clear_depth;
    int textured, tex_func, tex_alpha, tex_double;
    Rgba env;
    int alpha_test, at_func, at_ref, at_mask;
    int stencil_test, st_func, st_ref, st_mask, op_sfail, op_zfail, op_zpass;
    int depth_test, z_func, z_write;
    int blend, bl_a, bl_b, bl_eq;
    Rgba fix_a, fix_b;
    uint32_t write_mask;   /* bits that must NOT be written (packed ABGR8888) */
    int gouraud;
    Texture tex;
    int fast_blend;   /* blend is src*a + dst*(1-a), added */
} Pipe;

static inline __attribute__((always_inline)) int compare(int func, int a, int b) {
    switch (func) {
    case 0: return 0;
    case 1: return 1;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

static Pipe setup_pipe(void) {
    Pipe p;
    memset(&p, 0, sizeof p);
    p.fb = VRAM_BASE | (ge.cmd[GE_FRAMEBUFPTR] & 0x1FFFF0);
    p.fb_stride = ge.cmd[GE_FRAMEBUFWIDTH] & 0x7FC;
    p.fb_fmt = ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3;
    p.zb = VRAM_BASE | (ge.cmd[GE_ZBUFPTR] & 0x1FFFF0);
    p.zb_stride = ge.cmd[GE_ZBUFWIDTH] & 0x7FC;
    p.sx1 = ge.cmd[GE_SCISSOR1] & 0x3FF; p.sy1 = (ge.cmd[GE_SCISSOR1] >> 10) & 0x3FF;
    p.sx2 = ge.cmd[GE_SCISSOR2] & 0x3FF; p.sy2 = (ge.cmd[GE_SCISSOR2] >> 10) & 0x3FF;
    p.clear = ge.cmd[GE_CLEARMODE] & 1;
    p.clear_color = (ge.cmd[GE_CLEARMODE] >> 8) & 1;
    p.clear_alpha = (ge.cmd[GE_CLEARMODE] >> 9) & 1;
    p.clear_depth = (ge.cmd[GE_CLEARMODE] >> 10) & 1;
    p.textured = !p.clear && (ge.cmd[GE_TEXTUREMAPENABLE] & 1);
    p.tex_func = ge.cmd[GE_TEXFUNC] & 7;
    p.tex_alpha = (ge.cmd[GE_TEXFUNC] >> 8) & 1;
    p.tex_double = (ge.cmd[GE_TEXFUNC] >> 16) & 1;
    p.env = unpack(ge.cmd[GE_TEXENVCOLOR] | 0xFF000000u, 3);
    p.alpha_test = !p.clear && (ge.cmd[GE_ALPHATESTENABLE] & 1);
    p.at_func = ge.cmd[GE_ALPHATEST] & 7;
    p.at_ref = (ge.cmd[GE_ALPHATEST] >> 8) & 0xFF;
    p.at_mask = (ge.cmd[GE_ALPHATEST] >> 16) & 0xFF;
    p.stencil_test = !p.clear && (ge.cmd[GE_STENCILTESTENABLE] & 1) && p.fb_fmt != 0;
    p.st_func = ge.cmd[GE_STENCILTEST] & 7;
    p.st_ref = (ge.cmd[GE_STENCILTEST] >> 8) & 0xFF;
    p.st_mask = (ge.cmd[GE_STENCILTEST] >> 16) & 0xFF;
    p.op_sfail = ge.cmd[GE_STENCILOP] & 7;
    p.op_zfail = (ge.cmd[GE_STENCILOP] >> 8) & 7;
    p.op_zpass = (ge.cmd[GE_STENCILOP] >> 16) & 7;
    p.depth_test = !p.clear && (ge.cmd[GE_ZTESTENABLE] & 1);
    p.z_func = ge.cmd[GE_ZTEST] & 7;
    p.z_write = p.clear ? p.clear_depth : !(ge.cmd[GE_ZWRITEDISABLE] & 1) && p.depth_test;
    p.blend = !p.clear && (ge.cmd[GE_ALPHABLENDENABLE] & 1);
    p.bl_a = ge.cmd[GE_BLENDMODE] & 0xF;
    p.bl_b = (ge.cmd[GE_BLENDMODE] >> 4) & 0xF;
    p.bl_eq = (ge.cmd[GE_BLENDMODE] >> 8) & 7;
    p.fix_a = unpack(ge.cmd[GE_BLENDFIXEDA], 3);
    p.fix_b = unpack(ge.cmd[GE_BLENDFIXEDB], 3);
    p.write_mask = p.clear ? (p.clear_color ? 0 : 0x00FFFFFFu) | (p.clear_alpha ? 0 : 0xFF000000u)
                           : (ge.cmd[GE_MASKRGB] & 0xFFFFFF) | ((ge.cmd[GE_MASKALPHA] & 0xFF) << 24);
    p.gouraud = ge.cmd[GE_SHADEMODE] & 1;
    if (p.textured) p.tex = current_texture();
    if (g_scale > 1) {   /* draw into the high-resolution twins (surface.c) */
        int n = g_scale, rows = p.sy2 + 1;
        p.fb = surf_target(p.fb, p.fb_stride, p.fb_fmt, rows);
        if (p.depth_test || p.z_write) p.zb = surf_target(p.zb, p.zb_stride, 4, rows);
        p.fb_stride *= n;
        p.zb_stride *= n;
        p.sx1 *= n; p.sy1 *= n;
        p.sx2 = (p.sx2 + 1) * n - 1; p.sy2 = (p.sy2 + 1) * n - 1;
    }
    p.fast_blend = p.blend && p.bl_a == 2 && p.bl_b == 3 && p.bl_eq == 0;
    return p;
}

static inline int stencil_op(int op, int cur, int ref) {
    switch (op) {
    case 0: return cur;
    case 1: return 0;
    case 2: return ref;
    case 3: return ~cur & 0xFF;
    case 4: return cur < 255 ? cur + 1 : 255;
    default: return cur > 0 ? cur - 1 : 0;
    }
}

static int blend_factor(int which, int f, Rgba s, Rgba d, Rgba fix, int ch) {
    int sc[3] = {s.r, s.g, s.b}, dc[3] = {d.r, d.g, d.b}, fc[3] = {fix.r, fix.g, fix.b};
    switch (f) {
    case 0: return which == 0 ? dc[ch] : sc[ch];               /* other colour */
    case 1: return 255 - (which == 0 ? dc[ch] : sc[ch]);       /* 1 - other colour */
    case 2: return s.a;
    case 3: return 255 - s.a;
    case 4: return d.a;
    case 5: return 255 - d.a;
    case 6: return clamp255(2 * s.a);
    case 7: return clamp255(255 - 2 * s.a);
    case 8: return clamp255(2 * d.a);
    case 9: return clamp255(255 - 2 * d.a);
    default: return fc[ch];                                     /* fixed A / B */
    }
}

static uint64_t g_bench_pixels;   /* MKU_RASTER_BENCH: pixels shaded in the serial replay */
static int g_bench_skip;          /* MKU_BENCH_SKIP: 1 = no pixel shading, 2 = no texturing (timing experiments) */
static int g_bench_counting;

static void shade_pixel_ref(const Pipe *p, int x, int y, float z, float u, float v, Rgba col) {
    if (x < p->sx1 || x > p->sx2 || y < p->sy1 || y > p->sy2) return;
    if (g_bench_counting) g_bench_pixels++;
    int bpp = p->fb_fmt == 3 ? 4 : 2;
    uint32_t fa = p->fb + (uint32_t)((y * p->fb_stride + x) * bpp);
    uint32_t za = p->zb + (uint32_t)((y * p->zb_stride + x) * 2);
    int zi = (int)(z < 0 ? 0 : z > 65535 ? 65535 : z);
    Rgba c = col;

    if (p->textured) {
        Rgba t = sample(&p->tex, u, v);
        int mul = p->tex_double ? 2 : 1;
        switch (p->tex_func) {
        case 0:   /* modulate */
            c.r = c.r * t.r / 255; c.g = c.g * t.g / 255; c.b = c.b * t.b / 255;
            if (p->tex_alpha) c.a = c.a * t.a / 255;
            break;
        case 1:   /* decal */
            if (p->tex_alpha) {
                c.r = (t.r * t.a + c.r * (255 - t.a)) / 255;
                c.g = (t.g * t.a + c.g * (255 - t.a)) / 255;
                c.b = (t.b * t.a + c.b * (255 - t.a)) / 255;
            } else { c.r = t.r; c.g = t.g; c.b = t.b; }
            break;
        case 2:   /* blend with the environment colour */
            c.r = (c.r * (255 - t.r) + p->env.r * t.r) / 255;
            c.g = (c.g * (255 - t.g) + p->env.g * t.g) / 255;
            c.b = (c.b * (255 - t.b) + p->env.b * t.b) / 255;
            if (p->tex_alpha) c.a = c.a * t.a / 255;
            break;
        case 3:   /* replace */
            c.r = t.r; c.g = t.g; c.b = t.b;
            if (p->tex_alpha) c.a = t.a;
            break;
        default:  /* add */
            c.r += t.r; c.g += t.g; c.b += t.b;
            if (p->tex_alpha) c.a = c.a * t.a / 255;
            break;
        }
        c.r = clamp255(c.r * mul); c.g = clamp255(c.g * mul); c.b = clamp255(c.b * mul); c.a = clamp255(c.a);
    }

    if (p->alpha_test && !compare(p->at_func, c.a & p->at_mask, p->at_ref & p->at_mask)) return;

    uint32_t dst_raw = bpp == 4 ? MEM_LW(fa) : MEM_LHU(fa);
    Rgba d = unpack(dst_raw, p->fb_fmt);
    int stencil = d.a, new_stencil = stencil;

    if (p->stencil_test) {
        if (!compare(p->st_func, p->st_ref & p->st_mask, stencil & p->st_mask)) {
            new_stencil = stencil_op(p->op_sfail, stencil, p->st_ref);
            goto write_stencil_only;
        }
    }
    if (p->depth_test) {
        if (!compare(p->z_func, zi, MEM_LHU(za))) {
            if (p->stencil_test) new_stencil = stencil_op(p->op_zfail, stencil, p->st_ref);
            goto write_stencil_only;
        }
    }
    if (p->stencil_test) new_stencil = stencil_op(p->op_zpass, stencil, p->st_ref);
    if (p->z_write) MEM_SH(za, (uint32_t)zi);

    if (p->blend) {
        Rgba s = c, out;
        int sc[3] = {s.r, s.g, s.b}, dc[3] = {d.r, d.g, d.b}, oc[3];
        for (int ch = 0; ch < 3; ch++) {
            int a = blend_factor(0, p->bl_a, s, d, p->fix_a, ch), b = blend_factor(1, p->bl_b, s, d, p->fix_b, ch);
            int sv = sc[ch] * a / 255, dv = dc[ch] * b / 255;
            switch (p->bl_eq) {
            case 0: oc[ch] = sv + dv; break;
            case 1: oc[ch] = sv - dv; break;
            case 2: oc[ch] = dv - sv; break;
            case 3: oc[ch] = sc[ch] < dc[ch] ? sc[ch] : dc[ch]; break;
            case 4: oc[ch] = sc[ch] > dc[ch] ? sc[ch] : dc[ch]; break;
            default: oc[ch] = abs(sc[ch] - dc[ch]); break;
            }
        }
        out.r = clamp255(oc[0]); out.g = clamp255(oc[1]); out.b = clamp255(oc[2]); out.a = s.a;
        c = out;
    }
    /* The framebuffer's alpha channel doubles as the stencil buffer. */
    c.a = p->stencil_test ? new_stencil : c.a;
    {
        uint32_t src = pack(c, 3), dst8888 = pack(d, 3);
        uint32_t merged = (src & ~p->write_mask) | (dst8888 & p->write_mask);
        uint32_t outp = pack(unpack(merged, 3), p->fb_fmt);
        if (bpp == 4) MEM_SW(fa, outp); else MEM_SH(fa, outp);
    }
    return;

write_stencil_only:
    if (p->stencil_test && new_stencil != stencil && !(p->write_mask & 0xFF000000u)) {
        d.a = new_stencil;
        uint32_t outp = pack(d, p->fb_fmt);
        if (bpp == 4) MEM_SW(fa, outp); else MEM_SH(fa, outp);
    }
}

/* The fast pixel pipeline: the same arithmetic as shade_pixel_ref() (which
 * stays as the readable reference; MKU_RASTER_CHECK=1 compares the two on
 * every batch), with the common cases taken out of the generic code: 32-bit
 * framebuffers, src-alpha blending, no colour mask. */
uint64_t g_fallback[3];   /* debug counters (unused now that the fast path covers everything) */
uint32_t g_fallback_mask, g_fallback_blend;

/* One blend factor for all three channels at once (same values as blend_factor()). */
static inline void blend_factors(int which, int f, Rgba s, Rgba d, Rgba fix, int out[3]) {
    const Rgba o = which == 0 ? d : s;   /* "the other colour" */
    switch (f) {
    case 0: out[0] = o.r; out[1] = o.g; out[2] = o.b; return;
    case 1: out[0] = 255 - o.r; out[1] = 255 - o.g; out[2] = 255 - o.b; return;
    case 2: out[0] = out[1] = out[2] = s.a; return;
    case 3: out[0] = out[1] = out[2] = 255 - s.a; return;
    case 4: out[0] = out[1] = out[2] = d.a; return;
    case 5: out[0] = out[1] = out[2] = 255 - d.a; return;
    case 6: out[0] = out[1] = out[2] = clamp255(2 * s.a); return;
    case 7: out[0] = out[1] = out[2] = clamp255(255 - 2 * s.a); return;
    case 8: out[0] = out[1] = out[2] = clamp255(2 * d.a); return;
    case 9: out[0] = out[1] = out[2] = clamp255(255 - 2 * d.a); return;
    default: out[0] = fix.r; out[1] = fix.g; out[2] = fix.b; return;
    }
}

static inline int blend_eq(int eq, int sc, int dc, int sv, int dv) {
    switch (eq) {
    case 0: return sv + dv;
    case 1: return sv - dv;
    case 2: return dv - sv;
    case 3: return sc < dc ? sc : dc;
    case 4: return sc > dc ? sc : dc;
    default: return abs(sc - dc);
    }
}

/* The fast pixel pipeline: exactly the arithmetic of shade_pixel_ref() (which
 * stays as the readable reference; MKU_RASTER_CHECK=1 compares the two on
 * every batch), without its generic per-channel helpers, and with the common
 * src-alpha blend and "no colour mask" cases taken out. */
static inline __attribute__((always_inline)) void shade_pixel(const Pipe *p, int x, int y, float z, float u, float v, Rgba c) {
    if (x < p->sx1 || x > p->sx2 || y < p->sy1 || y > p->sy2) return;
    if (g_bench_counting) g_bench_pixels++;
    int bpp = p->fb_fmt == 3 ? 4 : 2;
    uint8_t *fp = rt_ptr(p->fb + (uint32_t)((y * p->fb_stride + x) * bpp));
    uint8_t *zp = rt_ptr(p->zb + (uint32_t)((y * p->zb_stride + x) * 2));
    int zi = (int)(z < 0 ? 0 : z > 65535 ? 65535 : z);

    if (p->textured && g_bench_skip != 2) {
        Rgba t = sample(&p->tex, u, v);
        int mul = p->tex_double ? 2 : 1;
        switch (p->tex_func) {
        case 0:
            c.r = c.r * t.r / 255; c.g = c.g * t.g / 255; c.b = c.b * t.b / 255;
            if (p->tex_alpha) c.a = c.a * t.a / 255;
            break;
        case 1:
            if (p->tex_alpha) {
                c.r = (t.r * t.a + c.r * (255 - t.a)) / 255;
                c.g = (t.g * t.a + c.g * (255 - t.a)) / 255;
                c.b = (t.b * t.a + c.b * (255 - t.a)) / 255;
            } else { c.r = t.r; c.g = t.g; c.b = t.b; }
            break;
        case 2:
            c.r = (c.r * (255 - t.r) + p->env.r * t.r) / 255;
            c.g = (c.g * (255 - t.g) + p->env.g * t.g) / 255;
            c.b = (c.b * (255 - t.b) + p->env.b * t.b) / 255;
            if (p->tex_alpha) c.a = c.a * t.a / 255;
            break;
        case 3:
            c.r = t.r; c.g = t.g; c.b = t.b;
            if (p->tex_alpha) c.a = t.a;
            break;
        default:
            c.r += t.r; c.g += t.g; c.b += t.b;
            if (p->tex_alpha) c.a = c.a * t.a / 255;
            break;
        }
        c.r = clamp255(c.r * mul); c.g = clamp255(c.g * mul); c.b = clamp255(c.b * mul); c.a = clamp255(c.a);
    }
    if (p->alpha_test && !compare(p->at_func, c.a & p->at_mask, p->at_ref & p->at_mask)) return;

    uint32_t raw;
    if (bpp == 4) memcpy(&raw, fp, 4);
    else { uint16_t h; memcpy(&h, fp, 2); raw = h; }
    Rgba d = unpack(raw, p->fb_fmt);
    int stencil = d.a, new_stencil = stencil;

    if (p->stencil_test && !compare(p->st_func, p->st_ref & p->st_mask, stencil & p->st_mask)) {
        new_stencil = stencil_op(p->op_sfail, stencil, p->st_ref);
        goto stencil_only;
    }
    if (p->depth_test) {
        uint16_t zd;
        memcpy(&zd, zp, 2);
        if (!compare(p->z_func, zi, zd)) {
            if (p->stencil_test) new_stencil = stencil_op(p->op_zfail, stencil, p->st_ref);
            goto stencil_only;
        }
    }
    if (p->stencil_test) new_stencil = stencil_op(p->op_zpass, stencil, p->st_ref);
    if (p->z_write) { uint16_t zw = (uint16_t)zi; memcpy(zp, &zw, 2); }

    if (p->fast_blend) {   /* src * a + dst * (1 - a) */
        int a = c.a, ia = 255 - a;
        c.r = clamp255(c.r * a / 255 + d.r * ia / 255);
        c.g = clamp255(c.g * a / 255 + d.g * ia / 255);
        c.b = clamp255(c.b * a / 255 + d.b * ia / 255);
    } else if (p->blend) {
        int fa[3], fb[3];
        blend_factors(0, p->bl_a, c, d, p->fix_a, fa);
        blend_factors(1, p->bl_b, c, d, p->fix_b, fb);
        int r = blend_eq(p->bl_eq, c.r, d.r, c.r * fa[0] / 255, d.r * fb[0] / 255);
        int g = blend_eq(p->bl_eq, c.g, d.g, c.g * fa[1] / 255, d.g * fb[1] / 255);
        int b = blend_eq(p->bl_eq, c.b, d.b, c.b * fa[2] / 255, d.b * fb[2] / 255);
        c.r = clamp255(r); c.g = clamp255(g); c.b = clamp255(b);
    }
    if (p->stencil_test) c.a = new_stencil;   /* the alpha channel is the stencil buffer */
    uint32_t out;
    if (!p->write_mask) {
        out = pack(c, p->fb_fmt);
    } else {
        uint32_t merged = (pack(c, 3) & ~p->write_mask) | (pack(d, 3) & p->write_mask);
        out = pack(unpack(merged, 3), p->fb_fmt);
    }
    if (bpp == 4) memcpy(fp, &out, 4);
    else { uint16_t h = (uint16_t)out; memcpy(fp, &h, 2); }
    return;

stencil_only:
    if (p->stencil_test && new_stencil != stencil && !(p->write_mask & 0xFF000000u)) {
        d.a = new_stencil;
        uint32_t o = pack(d, p->fb_fmt);
        if (bpp == 4) memcpy(fp, &o, 4);
        else { uint16_t h = (uint16_t)o; memcpy(fp, &h, 2); }
    }
}

/* ------------------------------------------------------------------------ */
/* rasterisation */

static float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Triangles, with every attribute as a plane equation over the screen:
 * S(x, y) = S(row start) + dS/dx * steps, one add per attribute per pixel.
 * With perspective, u/w, v/w, colour/w and 1/w are the linear quantities and
 * each pixel divides once. Each row starts from an exact evaluation, so a
 * row's pixels don't depend on which band drew it (MKU_RASTER_CHECK), and
 * each row only walks the span where all three edge functions can be >= 0. */
static inline void draw_triangle(const Pipe *p, const Vert *a, const Vert *b, const Vert *c, const Vert *flat, int ry0, int ry1, int ref) {
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
    if (area == 0.f) return;
    float inv = 1.f / area;
    float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < p->sx1) x0 = p->sx1;
    if (y0 < p->sy1) y0 = p->sy1;
    if (x1 > p->sx2 + 1) x1 = p->sx2 + 1;
    if (y1 > p->sy2 + 1) y1 = p->sy2 + 1;
    if (y0 < ry0) y0 = ry0;   /* only this worker's rows */
    if (y1 > ry1) y1 = ry1;
    if (x0 >= x1 || y0 >= y1) return;

    float px0 = x0 + 0.5f;
    float w0_dx = (b->y - c->y) * inv, w1_dx = (c->y - a->y) * inv, w2_dx = -w0_dx - w1_dx;
    int perspective = !(a->w == b->w && b->w == c->w);
    int gouraud = p->gouraud;
    Rgba fc = {(int)flat->r, (int)flat->g, (int)flat->b, (int)flat->a};

    /* per-vertex values of the interpolated quantities: [0]=q (1/w), then
     * u, v, r, g, b, a (each times q when perspective-correct), [7]=z */
    enum { Q, U, V, R, G, B, A, Z, NATTR };
    float va[3][NATTR];
    const Vert *vs[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        const Vert *t = vs[i];
        float q = perspective ? t->w : 1.f;
        va[i][Q] = q;
        va[i][U] = t->u * q; va[i][V] = t->v * q;
        va[i][R] = t->r * q; va[i][G] = t->g * q; va[i][B] = t->b * q; va[i][A] = t->a * q;
        va[i][Z] = t->z;
    }
    float d0[NATTR], d1[NATTR], ddx[NATTR];   /* S = s2 + w0*d0 + w1*d1 */
    for (int k = 0; k < NATTR; k++) {
        d0[k] = va[0][k] - va[2][k];
        d1[k] = va[1][k] - va[2][k];
        ddx[k] = d0[k] * w0_dx + d1[k] * w1_dx;
    }
    int nattr = gouraud ? NATTR : A + 1;   /* colours only matter when gouraud */

    for (int y = y0; y < y1; y++) {
        float py = y + 0.5f;
        float w0r = edge(b->x, b->y, c->x, c->y, px0, py) * inv, w1r = edge(c->x, c->y, a->x, a->y, px0, py) * inv;
        float w2r = 1.f - w0r - w1r;
        /* the span where every edge function is >= 0 (with a pixel of margin) */
        float lo = 0.f, hi = (float)(x1 - x0 - 1);
        const float wr[3] = {w0r, w1r, w2r}, wd[3] = {w0_dx, w1_dx, w2_dx};
        int empty = 0;
        for (int e = 0; e < 3; e++) {
            if (wd[e] > 0.f) { float t = -wr[e] / wd[e]; if (t - 1.f > lo) lo = t - 1.f; }
            else if (wd[e] < 0.f) { float t = wr[e] / -wd[e]; if (t + 1.f < hi) hi = t + 1.f; }
            else if (wr[e] < 0.f) empty = 1;
        }
        if (empty || lo > hi) continue;
        int xs = (int)lo, xe = (int)hi;
        if (xs < 0) xs = 0;
        if (xe > x1 - x0 - 1) xe = x1 - x0 - 1;
        float fs = (float)xs;
        float w0 = w0r + w0_dx * fs, w1 = w1r + w1_dx * fs;
        float at[NATTR];
        for (int k = 0; k < NATTR; k++) at[k] = va[2][k] + w0 * d0[k] + w1 * d1[k];
        for (int x = x0 + xs; x <= x0 + xe; x++) {
            float w2 = 1.f - w0 - w1;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                float iw = perspective ? 1.f / at[Q] : 1.f;
                Rgba col = fc;
                if (gouraud) {
                    col.r = (int)(at[R] * iw + 0.5f);
                    col.g = (int)(at[G] * iw + 0.5f);
                    col.b = (int)(at[B] * iw + 0.5f);
                    col.a = (int)(at[A] * iw + 0.5f);
                }
                float u = at[U] * iw, v = at[V] * iw;
                if (ref) shade_pixel_ref(p, x, y, at[Z], u, v, col);
                else if (g_bench_skip == 1) g_bench_pixels += (int)u ^ col.r;
                else shade_pixel(p, x, y, at[Z], u, v, col);
            }
            w0 += w0_dx;
            w1 += w1_dx;
            for (int k = 0; k < nattr; k++) at[k] += ddx[k];
            if (!gouraud) at[Z] += ddx[Z];
        }
    }
}

/* Sprites: axis-aligned rectangles from two corners; colour and depth come
 * from the second vertex, texture coordinates are interpolated linearly. */
static inline void draw_sprite(const Pipe *p, const Vert *a, const Vert *b, int ry0, int ry1, int ref) {
    int x0 = (int)floorf(fminf(a->x, b->x)), x1 = (int)floorf(fmaxf(a->x, b->x));
    int y0 = (int)floorf(fminf(a->y, b->y)), y1 = (int)floorf(fmaxf(a->y, b->y));
    float dx = b->x - a->x, dy = b->y - a->y;
    Rgba col = {(int)b->r, (int)b->g, (int)b->b, (int)b->a};
    if (y0 < ry0) y0 = ry0;
    if (y1 > ry1) y1 = ry1;
    for (int y = y0 < p->sy1 ? p->sy1 : y0; y < y1 && y <= p->sy2; y++) {
        float ty = dy != 0.f ? ((y + 0.5f) - a->y) / dy : 0.f;
        float v = a->v + (b->v - a->v) * ty;
        for (int x = x0 < p->sx1 ? p->sx1 : x0; x < x1 && x <= p->sx2; x++) {
            float tx = dx != 0.f ? ((x + 0.5f) - a->x) / dx : 0.f;
            float uu = a->u + (b->u - a->u) * tx;
            if (ref) shade_pixel_ref(p, x, y, b->z, uu, v, col); else shade_pixel(p, x, y, b->z, uu, v, col);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* parallel rasterisation
 *
 * Triangles and sprites are queued with a snapshot of the pipeline state and
 * drawn when the queue is flushed: at the end of each display list (before
 * game code can look at VRAM), and before anything that reads or rewrites
 * memory the queued draws might touch (block transfers, textures in VRAM,
 * a change of render target). The screen is cut into 16-row bands; every
 * worker takes the next free band and draws *all* queued primitives clipped
 * to it, in order. Each pixel therefore sees exactly the same sequence of
 * writes as in a serial renderer, so the output is identical.
 *
 * MKU_RASTER_THREADS=n sets the thread count (1 = serial).
 * MKU_RASTER_CHECK=1 draws every batch both ways and compares VRAM.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define BAND 8           /* rows per band */
#define MAX_BANDS 1024

typedef struct {
    Vert a, b, c;
    int pipe, sprite;
    int y0, y1;   /* rows covered, inclusive */
} Job;

static Job *g_jobs;
static int g_njobs, g_cap_jobs;
static Pipe *g_pipes;
static int g_npipes, g_cap_pipes;
static uint32_t g_batch_fb = 0xFFFFFFFFu, g_batch_zb;
static int g_batch_ymax;
static uint64_t g_batch_area;   /* sum of bounding-box areas: is the batch worth the threads? */

/* Binning: for each band, the jobs that touch it, in submission order
 * (a counting sort, so each band walks only its own jobs). */
static int *g_bin_jobs, g_cap_bin;
static int g_bin_start[MAX_BANDS + 1];

static void bin_jobs(void) {
    int nb = (g_batch_ymax + BAND) / BAND;
    if (nb > MAX_BANDS) nb = MAX_BANDS;
    static int count[MAX_BANDS];
    memset(count, 0, sizeof(int) * (size_t)nb);
    int total = 0;
    for (int i = 0; i < g_njobs; i++) {
        int b0 = g_jobs[i].y0 / BAND, b1 = g_jobs[i].y1 / BAND;
        if (b1 >= nb) b1 = nb - 1;
        for (int b = b0; b <= b1; b++) count[b]++;
        total += b1 - b0 + 1;
    }
    if (total > g_cap_bin) {
        g_cap_bin = total * 2;
        g_bin_jobs = realloc(g_bin_jobs, sizeof(int) * (size_t)g_cap_bin);
    }
    g_bin_start[0] = 0;
    for (int b = 0; b < nb; b++) g_bin_start[b + 1] = g_bin_start[b] + count[b];
    memcpy(count, g_bin_start, sizeof(int) * (size_t)nb);   /* reuse as fill cursors */
    for (int i = 0; i < g_njobs; i++) {
        int b0 = g_jobs[i].y0 / BAND, b1 = g_jobs[i].y1 / BAND;
        if (b1 >= nb) b1 = nb - 1;
        for (int b = b0; b <= b1; b++) g_bin_jobs[count[b]++] = i;
    }
}

static int g_nthreads;
static pthread_t g_workers[16];
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pool_go = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_pool_gen;
static atomic_int g_pool_busy;
static atomic_int g_next_band;
static int g_nbands;

static void draw_band(int band) {
    int ry0 = band * BAND, ry1 = ry0 + BAND;
    for (int k = g_bin_start[band]; k < g_bin_start[band + 1]; k++) {
        const Job *j = &g_jobs[g_bin_jobs[k]];
        const Pipe *p = &g_pipes[j->pipe];
        if (j->sprite) draw_sprite(p, &j->a, &j->b, ry0, ry1, 0);
        else draw_triangle(p, &j->a, &j->b, &j->c, &j->c, ry0, ry1, 0);
    }
}

/* Serial, no bands. ref=1: with the reference pixel pipeline (the oracle for
 * MKU_RASTER_CHECK); ref=0: the fast one (MKU_RASTER_BENCH). */
static void draw_unbanded(int ref) {
    for (int i = 0; i < g_njobs; i++) {
        const Job *j = &g_jobs[i];
        const Pipe *p = &g_pipes[j->pipe];
        if (j->sprite) draw_sprite(p, &j->a, &j->b, 0, 1 << 20, ref);
        else draw_triangle(p, &j->a, &j->b, &j->c, &j->c, 0, 1 << 20, ref);
    }
}

static void take_bands(void) {
    int b;
    while ((b = atomic_fetch_add(&g_next_band, 1)) < g_nbands) draw_band(b);
}

static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__)
    __asm__ volatile("pause");
#endif
}

/* Workers spin for a moment after each batch (batches come in bursts within
 * a frame, and waking a sleeping thread costs tens of microseconds), then
 * sleep until the next frame. */
static int g_spin_limit = 4000;

static void *worker(void *arg) {
    (void)arg;
    uint64_t seen = 0;
    for (;;) {
        uint64_t g;
        int spins = 0;
        while ((g = atomic_load(&g_pool_gen)) == seen) {
            if (++spins < g_spin_limit) { cpu_relax(); continue; }
            pthread_mutex_lock(&g_pool_lock);
            while (atomic_load(&g_pool_gen) == seen) pthread_cond_wait(&g_pool_go, &g_pool_lock);
            pthread_mutex_unlock(&g_pool_lock);
        }
        seen = g;
        take_bands();
        atomic_fetch_sub(&g_pool_busy, 1);
    }
    return NULL;
}

static void pool_init(void) {
    const char *e = getenv("MKU_RASTER_THREADS");
    long n = e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    g_nthreads = (int)n;
    if (getenv("MKU_RASTER_SPIN")) g_spin_limit = atoi(getenv("MKU_RASTER_SPIN"));
    for (int i = 0; i < g_nthreads - 1; i++) pthread_create(&g_workers[i], NULL, worker, NULL);
}

#define PARALLEL_MIN_AREA 8192   /* pixels of bounding boxes below which one thread is faster */

static void run_batch(int threads) {
    bin_jobs();
    g_nbands = (g_batch_ymax + BAND) / BAND;
    if (g_nbands > MAX_BANDS) g_nbands = MAX_BANDS;
    atomic_store(&g_next_band, 0);
    if (threads <= 1 || g_batch_area < PARALLEL_MIN_AREA) {
        take_bands();
        return;
    }
    atomic_store(&g_pool_busy, threads - 1);
    atomic_fetch_add(&g_pool_gen, 1);
    pthread_mutex_lock(&g_pool_lock);
    pthread_cond_broadcast(&g_pool_go);
    pthread_mutex_unlock(&g_pool_lock);
    take_bands();   /* the calling thread works too */
    while (atomic_load(&g_pool_busy)) cpu_relax();
}

/* MKU_RASTER_BENCH=1: after drawing each batch for real, replay it on one
 * thread (on a saved copy of VRAM) and time it: the renderer's own cost,
 * independent of thread scheduling. Reported every 300 frames. */
static int g_bench = -1;
static uint64_t g_bench_us, g_bench_jobs, g_bench_par_us, g_bench_batches;
static uint8_t g_bench_vram[VRAM_SIZE];

void raster_bench_report(void) {
    static unsigned frames;
    if (g_bench != 1 || ++frames % 300) return;
    fprintf(stderr, "[bench] serial raster: %.2f ms/frame, %.1f ns/pixel, %.0f pixels/frame, %.0f prims/frame; "
            "parallel (%d threads): %.2f ms/frame, %.1f batches/frame\n",
            g_bench_us / 1000.0 / 300, g_bench_pixels ? g_bench_us * 1000.0 / g_bench_pixels : 0.0,
            g_bench_pixels / 300.0, g_bench_jobs / 300.0, g_nthreads, g_bench_par_us / 1000.0 / 300, g_bench_batches / 300.0);
    g_bench_us = g_bench_pixels = g_bench_jobs = g_bench_par_us = g_bench_batches = 0;
}

static void bench_batch(void) {
    memcpy(g_bench_vram, rt_ptr(VRAM_BASE), VRAM_SIZE);   /* state before the batch */
}

static void bench_replay(void) {
    static uint8_t after[VRAM_SIZE];
    memcpy(after, rt_ptr(VRAM_BASE), VRAM_SIZE);
    memcpy(rt_ptr(VRAM_BASE), g_bench_vram, VRAM_SIZE);
    g_bench_counting = 1;
    uint64_t t0 = now_us();
    draw_unbanded(0);
    if (g_bench_skip == 1) g_bench_pixels = 0;
    g_bench_us += now_us() - t0;
    g_bench_counting = 0;
    g_bench_jobs += (uint64_t)g_njobs;
    memcpy(rt_ptr(VRAM_BASE), after, VRAM_SIZE);
}

void gpu_flush(void) {
    if (!g_njobs) return;
    if (!g_nthreads) pool_init();
    if (g_bench < 0) {
        g_bench = getenv("MKU_RASTER_BENCH") != NULL;
        g_bench_skip = getenv("MKU_BENCH_SKIP") ? atoi(getenv("MKU_BENCH_SKIP")) : 0;
    }
    if (g_bench && g_scale > 1) g_bench = 0;   /* bench copies VRAM only: 1x */
    if (g_bench) bench_batch();
    uint64_t t0 = now_us();
    static int check = -1;
    if (check < 0) check = getenv("MKU_RASTER_CHECK") != NULL;
    if (check && g_scale == 1) {   /* differential test: unbanded serial vs banded parallel on the same input */
        static uint8_t before[VRAM_SIZE], serial[VRAM_SIZE];
        static unsigned batches, bad;
        memcpy(before, rt_ptr(VRAM_BASE), VRAM_SIZE);
        draw_unbanded(1);
        memcpy(serial, rt_ptr(VRAM_BASE), VRAM_SIZE);
        memcpy(rt_ptr(VRAM_BASE), before, VRAM_SIZE);
        run_batch(g_nthreads);
        batches++;
        if (memcmp(serial, rt_ptr(VRAM_BASE), VRAM_SIZE)) {
            size_t k = 0;
            while (serial[k] == ((uint8_t *)rt_ptr(VRAM_BASE))[k]) k++;
            fprintf(stderr, "[raster-check] batch %u (%d prims): fast parallel renderer differs from the reference at VRAM+0x%zX\n", batches, g_njobs, k);
            bad++;
        } else if (batches % 2000 == 0) {
            fprintf(stderr, "[raster-check] %u batches identical, %u different\n", batches, bad);
        }
    } else {
        run_batch(g_nthreads);
    }
    if (g_bench) { g_bench_par_us += now_us() - t0; g_bench_batches++; bench_replay(); }
    g_njobs = 0;
    g_npipes = 0;
    g_batch_ymax = 0;
    g_batch_area = 0;
    g_batch_fb = 0xFFFFFFFFu;
    if (!g_bench) g_raster_us += now_us() - t0;
}

static int queue_pipe(const Pipe *p) {
    /* a different render target: earlier draws may alias it, so finish them first */
    if (g_batch_fb != 0xFFFFFFFFu && (p->fb != g_batch_fb || p->zb != g_batch_zb)) gpu_flush();
    g_batch_fb = p->fb;
    g_batch_zb = p->zb;
    if (g_npipes == g_cap_pipes) {
        g_cap_pipes = g_cap_pipes ? g_cap_pipes * 2 : 256;
        g_pipes = realloc(g_pipes, sizeof *g_pipes * (size_t)g_cap_pipes);
    }
    g_pipes[g_npipes] = *p;
    return g_npipes++;
}

static void queue_job(int pipe, const Vert *a0, const Vert *b0, const Vert *c0, int sprite) {
    const Pipe *p = &g_pipes[pipe];
    Vert sa = *a0, sb = *b0, sc;
    if (!sprite) sc = *c0;
    if (g_scale > 1) {   /* screen coordinates of the high-resolution twin */
        float n = (float)g_scale;
        sa.x *= n; sa.y *= n; sb.x *= n; sb.y *= n;
        if (!sprite) { sc.x *= n; sc.y *= n; }
    }
    const Vert *a = &sa, *b = &sb, *c = sprite ? NULL : &sc;
    float ylo = sprite ? fminf(a->y, b->y) : fminf(a->y, fminf(b->y, c->y));
    float yhi = sprite ? fmaxf(a->y, b->y) : fmaxf(a->y, fmaxf(b->y, c->y));
    int y0 = (int)floorf(ylo), y1 = (int)ceilf(yhi);
    if (y0 < p->sy1) y0 = p->sy1;
    if (y1 > p->sy2) y1 = p->sy2;
    if (y0 > y1) return;
    if (g_njobs == g_cap_jobs) {
        g_cap_jobs = g_cap_jobs ? g_cap_jobs * 2 : 4096;
        g_jobs = realloc(g_jobs, sizeof *g_jobs * (size_t)g_cap_jobs);
    }
    g_tris_queued++;
    Job *j = &g_jobs[g_njobs++];
    j->a = *a;
    j->b = *b;
    if (!sprite) j->c = *c;
    j->pipe = pipe;
    j->sprite = sprite;
    j->y0 = y0;
    j->y1 = y1;
    if (y1 > g_batch_ymax) g_batch_ymax = y1;
    {
        float xlo = sprite ? fminf(a->x, b->x) : fminf(a->x, fminf(b->x, c->x));
        float xhi = sprite ? fmaxf(a->x, b->x) : fmaxf(a->x, fmaxf(b->x, c->x));
        if (xlo < (float)p->sx1) xlo = (float)p->sx1;
        if (xhi > (float)p->sx2) xhi = (float)p->sx2;
        uint64_t area = xhi >= xlo ? (uint64_t)((xhi - xlo + 1.f) * (float)(y1 - y0 + 1)) : 0;
        g_batch_area += area;
        g_area_kind[p->clear ? 0 : sprite ? 1 : 2] += area;
    }
}

/* ------------------------------------------------------------------------ */
/* transform */

/* Free camera: an orbit around a point in front of the game's camera, applied
 * in view space (so the game's own camera still moves as it likes). The orbit
 * point's depth follows the skinned models (the fighters) of the last frame. */
static struct { float cy, sy, cp, sp, zoom; int on; } g_cam;
static float g_pivot_z = -8.f;
static double g_pivot_acc, g_pivot_acc_all;
static unsigned g_pivot_n, g_pivot_n_all;

void gpu_frame_end(void) {   /* present.c, once per shown frame */
    if (g_pivot_n) g_pivot_z = (float)(g_pivot_acc / g_pivot_n);
    else if (g_pivot_n_all) g_pivot_z = (float)(g_pivot_acc_all / g_pivot_n_all);
    g_pivot_acc = g_pivot_acc_all = 0;
    g_pivot_n = g_pivot_n_all = 0;
}

static void camera_setup(void) {
    float yaw, pitch, zoom;
    host_camera(&yaw, &pitch, &zoom);
    g_cam.on = yaw != 0.f || pitch != 0.f || zoom != 0.f;
    g_cam.cy = cosf(yaw); g_cam.sy = sinf(yaw);
    g_cam.cp = cosf(pitch); g_cam.sp = sinf(pitch);
    g_cam.zoom = zoom;
}

static void camera_apply(float v[3]) {
    float pz = g_pivot_z;
    float x = v[0], y = v[1], z = v[2] - pz;
    float x1 = g_cam.cy * x + g_cam.sy * z, z1 = -g_cam.sy * x + g_cam.cy * z;   /* yaw: around y */
    float y2 = g_cam.cp * y - g_cam.sp * z1, z2 = g_cam.sp * y + g_cam.cp * z1;  /* pitch: around x */
    v[0] = x1;
    v[1] = y2;
    v[2] = z2 + pz + (pz < 0 ? g_cam.zoom : -g_cam.zoom);   /* zoom: toward the orbit point */
}

/* object space -> clip space (before the divide by w) */
static void transform(const float pos[3], int skinned, float c[4]) {
    float wv[3], vv[3];
    mul43(ge.world, pos, wv);
    mul43(ge.view, wv, vv);
    if (skinned) { g_pivot_acc += vv[2]; g_pivot_n++; }
    g_pivot_acc_all += vv[2]; g_pivot_n_all++;
    if (g_cam.on) camera_apply(vv);
    const float *m = ge.proj;
    for (int i = 0; i < 4; i++) c[i] = vv[0] * m[i] + vv[1] * m[4 + i] + vv[2] * m[8 + i] + m[12 + i];
}

/* clip space -> screen (viewport, offset); w becomes 1/w for perspective */
static void project(const float c[4], Vert *out) {
    float iw = 1.f / c[3];
    float sx = ge_float(ge.cmd[GE_VIEWPORTXSCALE]), cx = ge_float(ge.cmd[GE_VIEWPORTXCENTER]);
    float sy = ge_float(ge.cmd[GE_VIEWPORTYSCALE]), cy = ge_float(ge.cmd[GE_VIEWPORTYCENTER]);
    float sz = ge_float(ge.cmd[GE_VIEWPORTZSCALE]), cz = ge_float(ge.cmd[GE_VIEWPORTZCENTER]);
    out->x = c[0] * iw * sx + cx - (float)(ge.cmd[GE_OFFSETX] & 0xFFFF) / 16.f;
    out->y = c[1] * iw * sy + cy - (float)(ge.cmd[GE_OFFSETY] & 0xFFFF) / 16.f;
    out->z = c[2] * iw * sz + cz;
    out->w = iw;
}

/* Near-plane clipping. A triangle with a vertex behind the camera (w <= NEAR_W)
 * is cut against the plane w = NEAR_W (Sutherland-Hodgman, one plane) in clip
 * space, where all attributes are still linear, giving up to two triangles. */
#define NEAR_W 1e-3f

typedef struct { float c[4], u, v, r, g, b, a; } ClipV;

static float g_clip[MAX_VERTS][4];

static ClipV clipv(int i) {
    const Vert *v = &g_verts[i];
    ClipV o = {{g_clip[i][0], g_clip[i][1], g_clip[i][2], g_clip[i][3]}, v->u, v->v, v->r, v->g, v->b, v->a};
    return o;
}

static ClipV lerp_clip(const ClipV *p, const ClipV *q, float t) {
    ClipV o;
    for (int k = 0; k < 4; k++) o.c[k] = p->c[k] + (q->c[k] - p->c[k]) * t;
    o.u = p->u + (q->u - p->u) * t; o.v = p->v + (q->v - p->v) * t;
    o.r = p->r + (q->r - p->r) * t; o.g = p->g + (q->g - p->g) * t;
    o.b = p->b + (q->b - p->b) * t; o.a = p->a + (q->a - p->a) * t;
    return o;
}

static Vert screen_of(const ClipV *p) {
    Vert v;
    project(p->c, &v);
    v.u = p->u; v.v = p->v; v.r = p->r; v.g = p->g; v.b = p->b; v.a = p->a;
    return v;
}

uint64_t g_tris_clipped;

static void queue_job(int pipe, const Vert *a, const Vert *b, const Vert *c, int sprite);

/* One 3D triangle (vertex indices). `flat` (provoking vertex) colours flat-shaded triangles. */
static void tri3d(int pipe, int i0, int i1, int i2, const uint8_t *ok) {
    if (ok[i0] && ok[i1] && ok[i2]) {
        queue_job(pipe, &g_verts[i0], &g_verts[i1], &g_verts[i2], 0);
        return;
    }
    g_tris_clipped++;
    ClipV in[3] = {clipv(i0), clipv(i1), clipv(i2)}, out[4];
    int n = 0;
    for (int k = 0; k < 3; k++) {
        const ClipV *p = &in[k], *q = &in[(k + 1) % 3];
        int pin = p->c[3] > NEAR_W, qin = q->c[3] > NEAR_W;
        if (pin) out[n++] = *p;
        if (pin != qin) out[n++] = lerp_clip(p, q, (NEAR_W - p->c[3]) / (q->c[3] - p->c[3]));
    }
    if (n < 3) return;
    Vert sv[4];
    for (int k = 0; k < n; k++) sv[k] = screen_of(&out[k]);
    /* flat shading uses the last original vertex's colour */
    for (int k = 0; k < n; k++)
        if (!g_pipes[pipe].gouraud) { sv[k].r = in[2].r; sv[k].g = in[2].g; sv[k].b = in[2].b; sv[k].a = in[2].a; }
    queue_job(pipe, &sv[0], &sv[1], &sv[2], 0);
    if (n == 4) queue_job(pipe, &sv[0], &sv[2], &sv[3], 0);
}

static void draw_prim_impl(uint32_t data);
void gpu_draw_prim(uint32_t data) { draw_prim_impl(data); }

static void draw_prim_impl(uint32_t data) {
    int type = (data >> 16) & 7, count = data & 0xFFFF;
    uint32_t vt = ge.cmd[GE_VERTEXTYPE];
    VFormat f = vformat(vt);
    g_prims[type]++;
    g_through[f.through]++;
    if (!count || !f.pos) return;
    if (count > MAX_VERTS) count = MAX_VERTS;

    Pipe p = setup_pipe();
    if (p.textured) g_texfmt[p.tex.fmt]++;
    if (!f.through && (ge.cmd[GE_LIGHTINGENABLE] & 1)) WARN_ONCE(1, "lighting not implemented yet (drawn unlit)\n");
    if (ge.cmd[GE_TEXMAPMODE] & 3) WARN_ONCE(2, "texture map mode %u not implemented\n", ge.cmd[GE_TEXMAPMODE] & 3);

    float su = ge.cmd[GE_TEXSCALEU] ? ge_float(ge.cmd[GE_TEXSCALEU]) : 1.f;
    float sv = ge.cmd[GE_TEXSCALEV] ? ge_float(ge.cmd[GE_TEXSCALEV]) : 1.f;
    float ou = ge_float(ge.cmd[GE_TEXOFFSETU]), ov = ge_float(ge.cmd[GE_TEXOFFSETV]);

    if (!f.through) camera_setup();
    uint8_t *ok = malloc((size_t)count);
    for (int i = 0; i < count; i++) {
        uint32_t index = (uint32_t)i;
        if (f.idx == 1) index = MEM_LBU(ge.iaddr + (uint32_t)i);
        else if (f.idx == 2) index = MEM_LHU(ge.iaddr + 2u * (uint32_t)i);
        else if (f.idx == 3) index = MEM_LW(ge.iaddr + 4u * (uint32_t)i);
        float pos[3], uv[2];
        Rgba col;
        decode_vertex(&f, ge.vaddr + index * (uint32_t)(f.size * f.nmorph), pos, uv, &col);
        Vert *v = &g_verts[i];
        if (f.through) {
            v->x = pos[0]; v->y = pos[1]; v->z = pos[2]; v->w = 1.f;
            v->u = uv[0]; v->v = uv[1];
            ok[i] = 1;
        } else {
            transform(pos, f.wt != 0, g_clip[i]);
            ok[i] = g_clip[i][3] > NEAR_W;
            if (ok[i]) project(g_clip[i], v);
            v->u = (uv[0] * su + ou) * (p.textured ? p.tex.w : 1);
            v->v = (uv[1] * sv + ov) * (p.textured ? p.tex.h : 1);
        }
        v->r = (float)col.r; v->g = (float)col.g; v->b = (float)col.b; v->a = (float)col.a;
    }
    /* Advance the vertex/index pointers like the hardware does, so a following
     * PRIM without a new VADDR continues where this one stopped. */
    if (f.idx) ge.iaddr += (uint32_t)count * (f.idx == 1 ? 1u : f.idx == 2 ? 2u : 4u);
    else ge.vaddr += (uint32_t)(count * f.size * f.nmorph);

    int pi = type >= 3 && type <= 6 ? queue_pipe(&p) : -1;
    if (g_dump_draws) {   /* MKU_GE_DUMP=frame: one line per draw of that frame */
        float zmin = 1e9f, zmax = -1e9f;
        int vis = 0;
        for (int i = 0; i < count; i++) if (ok[i]) { vis++; zmin = fminf(zmin, g_verts[i].z); zmax = fmaxf(zmax, g_verts[i].z); }
        fprintf(stderr, "[draw] fbfmt=%d type=%d n=%d vis=%d vt=%06X %s fb=%08X tex=%s%08X fmt=%d %dx%d func=%d at=%d/%d,%d zt=%d/%d zw=%d bl=%d(%X,%X,%d) st=%d mask=%08X z=%.0f..%.0f x0=%.0f y0=%.0f\n",
                p.fb_fmt, type, count, vis, vt, f.through ? "2D" : "3D", p.fb, p.textured ? "" : "-", p.textured ? p.tex.addr : 0,
                p.tex.fmt, p.tex.w, p.tex.h, p.tex_func, p.alpha_test, p.at_func, p.at_ref, p.depth_test, p.z_func, p.z_write,
                p.blend, p.bl_a, p.bl_b, p.bl_eq, p.stencil_test, p.write_mask, zmin, zmax, g_verts[0].x, g_verts[0].y);
    }
    if (!f.through) {
        g_draws_3d++;
        for (int i = 0; i < count; i++) g_tris_culled_w += !ok[i];
    }
    switch (type) {
    case 3:   /* triangles */
        for (int i = 0; i + 2 < count; i += 3)
            if (f.through) { if (ok[i] && ok[i + 1] && ok[i + 2]) queue_job(pi, &g_verts[i], &g_verts[i + 1], &g_verts[i + 2], 0); }
            else tri3d(pi, i, i + 1, i + 2, ok);
        break;
    case 4:   /* strip */
        for (int i = 0; i + 2 < count; i++)
            if (f.through) { if (ok[i] && ok[i + 1] && ok[i + 2]) queue_job(pi, &g_verts[i], &g_verts[i + 1], &g_verts[i + 2], 0); }
            else tri3d(pi, i, i + 1, i + 2, ok);
        break;
    case 5:   /* fan */
        for (int i = 1; i + 1 < count; i++)
            if (f.through) { if (ok[0] && ok[i] && ok[i + 1]) queue_job(pi, &g_verts[0], &g_verts[i], &g_verts[i + 1], 0); }
            else tri3d(pi, 0, i, i + 1, ok);
        break;
    case 6:   /* sprites */
        for (int i = 0; i + 1 < count; i += 2)
            if (ok[i] && ok[i + 1]) queue_job(pi, &g_verts[i], &g_verts[i + 1], NULL, 1);
        break;
    default:
        WARN_ONCE(3, "points/lines not implemented yet\n");
        break;
    }
    free(ok);
}

/* LOADCLUT happens when the command executes, so the palette is a snapshot. */
void gpu_load_clut(void) { load_clut(); }

/* TRANSFERSTART: copy a rectangle of pixels (e.g. RAM -> VRAM). */
void gpu_block_transfer(uint32_t data) {
    gpu_flush();
    surf_writeback(0x04000000u, VRAM_SIZE);   /* rare: bring all of VRAM up to date */
    g_transfers++;
    uint32_t src = (ge.cmd[GE_TRANSFERSRC] & 0xFFFFF0) | ((ge.cmd[GE_TRANSFERSRCW] & 0xFF0000) << 8);
    uint32_t dst = (ge.cmd[GE_TRANSFERDST] & 0xFFFFF0) | ((ge.cmd[GE_TRANSFERDSTW] & 0xFF0000) << 8);
    int sstride = ge.cmd[GE_TRANSFERSRCW] & 0x7F8, dstride = ge.cmd[GE_TRANSFERDSTW] & 0x7F8;
    int sx = ge.cmd[GE_TRANSFERSRCPOS] & 0x3FF, sy = (ge.cmd[GE_TRANSFERSRCPOS] >> 10) & 0x3FF;
    int dx = ge.cmd[GE_TRANSFERDSTPOS] & 0x3FF, dy = (ge.cmd[GE_TRANSFERDSTPOS] >> 10) & 0x3FF;
    int w = (ge.cmd[GE_TRANSFERSIZE] & 0x3FF) + 1, h = ((ge.cmd[GE_TRANSFERSIZE] >> 10) & 0x3FF) + 1;
    int bpp = (data & 1) ? 4 : 2;
    for (int y = 0; y < h; y++)
        memmove(rt_ptr(dst + (uint32_t)(((dy + y) * dstride + dx) * bpp)),
                rt_ptr(src + (uint32_t)(((sy + y) * sstride + sx) * bpp)), (size_t)(w * bpp));
}
