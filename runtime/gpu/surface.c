/*
 * High-resolution render surfaces (MKU_SCALE=2..4).
 *
 * The game draws into framebuffers in VRAM at 480x272. To render at N times
 * that resolution, every framebuffer / depth buffer the GE draws into gets a
 * twin N times larger, stored outside PSP memory (but inside our guest address
 * space, so the rasteriser's normal memory access works on it). The
 * rasteriser draws into the twin with screen coordinates scaled by N; the
 * window shows the twin directly.
 *
 * VRAM itself is only brought up to date when something else needs it:
 *   - surf_writeback(): before a texture is read from VRAM (render-to-texture)
 *     or a block transfer touches it, the twin is downsampled into VRAM;
 *   - if VRAM changed behind our back (the CPU or a block transfer wrote
 *     pixels), a hash of the VRAM region differs from the one taken at the
 *     last sync, and the twin is refreshed from VRAM before it is drawn into.
 */
#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_scale = 1;

#define HIRES_BASE 0x20000000u   /* beyond PSP RAM, inside our 1 GiB mapping */
#define SLOT_SIZE  0x01000000u   /* 16 MiB per surface: 512 x 512 x 4 bytes at 4x */
#define MAX_SURF   16

typedef struct {
    uint32_t vaddr;      /* VRAM address of the PSP buffer */
    int stride, fmt;     /* in PSP pixels; fmt 0-3 colour, 4 = 16-bit depth */
    int rows;            /* rows in use (grows with the scissor) */
    uint32_t hi;         /* address of the twin */
    uint64_t hash;       /* hash of the VRAM region at the last sync */
    uint64_t checked;    /* frame number of the last VRAM change check */
    int dirty;           /* the twin has pixels VRAM doesn't have */
} Surf;

static Surf g_surf[MAX_SURF];
static int g_nsurf;
static uint64_t g_frame_no = 1;

void surf_init(void) {
    const char *e = getenv("MKU_SCALE");
    if (e) g_scale = atoi(e);
    if (g_scale < 1) g_scale = 1;
    if (g_scale > 4) g_scale = 4;
    if (g_scale > 1) fprintf(stderr, "[gpu] rendering at %dx (%dx%d)\n", g_scale, 480 * g_scale, 272 * g_scale);
}

static int bytes_pp(int fmt) { return fmt == 3 ? 4 : 2; }

static uint64_t hash_region(uint32_t addr, uint32_t n) {
    const uint8_t *p = rt_ptr(addr);
    uint64_t h = 1469598103934665603ull, w;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h = (h ^ w) * 1099511628211ull; }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static uint32_t vram_bytes(const Surf *s) { return (uint32_t)(s->stride * s->rows * bytes_pp(s->fmt)); }

/* VRAM -> twin: each PSP pixel becomes an N x N block. */
static void upload(Surf *s) {
    int n = g_scale, bpp = bytes_pp(s->fmt), hs = s->stride * n;
    for (int y = 0; y < s->rows; y++) {
        const uint8_t *src = rt_ptr(s->vaddr + (uint32_t)(y * s->stride * bpp));
        for (int dy = 0; dy < n; dy++) {
            uint8_t *dst = rt_ptr(s->hi + (uint32_t)((y * n + dy) * hs * bpp));
            for (int x = 0; x < s->stride; x++)
                for (int dx = 0; dx < n; dx++) memcpy(dst + (x * n + dx) * bpp, src + x * bpp, (size_t)bpp);
        }
    }
    s->hash = hash_region(s->vaddr, vram_bytes(s));
    s->dirty = 0;
}

static void unpack16(uint32_t p, int fmt, int c[4]) {
    switch (fmt) {
    case 0: c[0] = (p & 31) * 255 / 31; c[1] = ((p >> 5) & 63) * 255 / 63; c[2] = ((p >> 11) & 31) * 255 / 31; c[3] = 255; break;
    case 1: c[0] = (p & 31) * 255 / 31; c[1] = ((p >> 5) & 31) * 255 / 31; c[2] = ((p >> 10) & 31) * 255 / 31; c[3] = (p >> 15) ? 255 : 0; break;
    default: c[0] = (p & 15) * 17; c[1] = ((p >> 4) & 15) * 17; c[2] = ((p >> 8) & 15) * 17; c[3] = ((p >> 12) & 15) * 17; break;
    }
}

static uint32_t pack16(const int c[4], int fmt) {
    switch (fmt) {
    case 0: return (uint32_t)((c[0] >> 3) | ((c[1] >> 2) << 5) | ((c[2] >> 3) << 11));
    case 1: return (uint32_t)((c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10) | ((c[3] >> 7) << 15));
    default: return (uint32_t)((c[0] >> 4) | ((c[1] >> 4) << 4) | ((c[2] >> 4) << 8) | ((c[3] >> 4) << 12));
    }
}

/* twin -> VRAM: average each N x N block (colour); depth takes the top-left sample. */
static void downsample(Surf *s) {
    int n = g_scale, bpp = bytes_pp(s->fmt), hs = s->stride * n;
    for (int y = 0; y < s->rows; y++) {
        uint8_t *dst = rt_ptr(s->vaddr + (uint32_t)(y * s->stride * bpp));
        for (int x = 0; x < s->stride; x++) {
            const uint8_t *src0 = rt_ptr(s->hi + (uint32_t)(((y * n) * hs + x * n) * bpp));
            if (s->fmt == 4) { memcpy(dst + x * 2, src0, 2); continue; }
            int sum[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < n; dy++)
                for (int dx = 0; dx < n; dx++) {
                    const uint8_t *q = src0 + (dy * hs + dx) * bpp;
                    int c[4];
                    if (bpp == 4) { c[0] = q[0]; c[1] = q[1]; c[2] = q[2]; c[3] = q[3]; }
                    else { uint16_t h; memcpy(&h, q, 2); unpack16(h, s->fmt, c); }
                    for (int k = 0; k < 4; k++) sum[k] += c[k];
                }
            int avg[4];
            for (int k = 0; k < 4; k++) avg[k] = (sum[k] + n * n / 2) / (n * n);
            if (bpp == 4) { uint8_t o[4] = {(uint8_t)avg[0], (uint8_t)avg[1], (uint8_t)avg[2], (uint8_t)avg[3]}; memcpy(dst + x * 4, o, 4); }
            else { uint16_t h = (uint16_t)pack16(avg, s->fmt); memcpy(dst + x * 2, &h, 2); }
        }
    }
    s->hash = hash_region(s->vaddr, vram_bytes(s));
    s->dirty = 0;
}

static Surf *find(uint32_t vaddr, int stride, int fmt) {
    for (int i = 0; i < g_nsurf; i++)
        if (g_surf[i].vaddr == vaddr && g_surf[i].stride == stride && g_surf[i].fmt == fmt) return &g_surf[i];
    return NULL;
}

/* The twin for a buffer the GE is about to draw into; rows = rows it may touch. */
uint32_t surf_target(uint32_t vaddr, int stride, int fmt, int rows) {
    if (stride <= 0) stride = 512;
    if (rows > 512) rows = 512;
    Surf *s = find(vaddr, stride, fmt);
    if (!s) {
        if (g_nsurf == MAX_SURF) {   /* recycle the oldest (rare: games use a handful) */
            gpu_flush();
            if (g_surf[0].dirty) downsample(&g_surf[0]);
            memmove(g_surf, g_surf + 1, sizeof(Surf) * (MAX_SURF - 1));
            g_nsurf--;
        }
        s = &g_surf[g_nsurf];
        memset(s, 0, sizeof *s);
        s->vaddr = vaddr; s->stride = stride; s->fmt = fmt; s->rows = rows;
        s->hi = HIRES_BASE + (uint32_t)g_nsurf * SLOT_SIZE;
        g_nsurf++;
        upload(s);
        s->checked = g_frame_no;
    } else {
        if (rows > s->rows) {   /* newly covered rows start from VRAM */
            gpu_flush();
            if (s->dirty) downsample(s);
            s->rows = rows;
            upload(s);
        }
        if (s->checked != g_frame_no) {   /* once a frame: did anything else write VRAM? */
            s->checked = g_frame_no;
            if (hash_region(s->vaddr, vram_bytes(s)) != s->hash) {
                gpu_flush();
                upload(s);
            }
        }
    }
    s->dirty = 1;
    return s->hi;
}

/* Before VRAM [addr, addr+len) is read by anyone but the rasteriser. */
void surf_writeback(uint32_t addr, uint32_t len) {
    if (g_scale == 1) return;
    addr &= 0x0FFFFFFFu;
    for (int i = 0; i < g_nsurf; i++) {
        Surf *s = &g_surf[i];
        uint32_t a = s->vaddr & 0x0FFFFFFFu, n = vram_bytes(s);
        if (s->dirty && addr < a + n && a < addr + len) {
            gpu_flush();
            downsample(s);
        }
    }
}

/* The twin of a displayed framebuffer, if there is one (present.c). */
uint32_t surf_display(uint32_t vaddr, int stride, int fmt) {
    if (g_scale == 1) return 0;
    Surf *s = find(vaddr, stride, fmt);
    return s ? s->hi : 0;
}

void surf_frame_end(void) { g_frame_no++; }
