/*
 * HD textures: dumping, replacement packs and xBRZ upscaling.
 *
 * Every texture the game uses is decoded once by the texture cache (raster.c)
 * into RGBA8888. Here that decoded image can be swapped for a better one:
 *
 *   MKU_TEXDUMP=1        save each distinct texture as build/texdump/<hash>.png
 *   MKU_TEXPACK=dir      if dir/<hash>.png exists, use it instead (it may be
 *                        2x, 4x, 8x... larger: e.g. upscaled with an AI tool)
 *   MKU_TEXSCALE=2|4     otherwise upscale with xBRZ (a pixel-art filter)
 *
 * <hash> is a 64-bit FNV-1a hash of the decoded pixels and their size, so it
 * identifies a texture *as it looks* (palette applied), independent of where
 * the game happens to load it.
 */
#include "gpu.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void xbrz_scale_rgba(int factor, const uint32_t *src, uint32_t *dst, int w, int h);   /* xbrz_glue.cpp */

static int g_dump, g_upscale = 1;
static const char *g_pack_dir;
static uint64_t *g_pack, *g_dumped;   /* sorted hashes present in the pack / already dumped */
static size_t g_npack, g_ndumped, g_cap_dumped;
static unsigned g_replaced, g_scaled;

/* Results by content hash, so a texture decoded again (cache eviction, a
 * palette switching back and forth) doesn't pay for xBRZ / PNG loading again. */
typedef struct { uint64_t hash; uint32_t *px; int w, h; } Done;
#define DONE_SLOTS 4096
static Done g_done[DONE_SLOTS];
static size_t g_done_bytes;
#define DONE_BUDGET (384u << 20)

static uint32_t *done_copy(const Done *d, int *ow, int *oh) {
    size_t n = sizeof(uint32_t) * (size_t)d->w * (size_t)d->h;
    uint32_t *out = malloc(n);
    memcpy(out, d->px, n);
    *ow = d->w;
    *oh = d->h;
    return out;
}

static void done_put(uint64_t hash, const uint32_t *px, int w, int h) {
    Done *d = &g_done[hash % DONE_SLOTS];
    size_t n = sizeof(uint32_t) * (size_t)w * (size_t)h;
    if (d->px) { g_done_bytes -= sizeof(uint32_t) * (size_t)d->w * (size_t)d->h; free(d->px); d->px = NULL; }
    if (g_done_bytes + n > DONE_BUDGET) return;
    d->px = malloc(n);
    memcpy(d->px, px, n);
    d->hash = hash; d->w = w; d->h = h;
    g_done_bytes += n;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int has(const uint64_t *set, size_t n, uint64_t h) { return n && bsearch(&h, set, n, sizeof h, cmp_u64) != NULL; }

void texpack_init(void) {
    g_dump = getenv("MKU_TEXDUMP") != NULL;
    if (getenv("MKU_TEXSCALE")) g_upscale = atoi(getenv("MKU_TEXSCALE"));
    if (g_upscale != 2 && g_upscale != 4) g_upscale = 1;
    g_pack_dir = getenv("MKU_TEXPACK");
    if (g_pack_dir) {
        DIR *d = opendir(g_pack_dir);
        if (!d) {
            fprintf(stderr, "[tex] MKU_TEXPACK=%s: can't open the folder\n", g_pack_dir);
            g_pack_dir = NULL;
        } else {
            size_t cap = 0;
            struct dirent *e;
            while ((e = readdir(d))) {
                unsigned long long h;
                char tail[8];
                if (sscanf(e->d_name, "%16llx.%4s", &h, tail) == 2 && !strcmp(tail, "png")) {
                    if (g_npack == cap) g_pack = realloc(g_pack, sizeof *g_pack * (cap = cap ? cap * 2 : 256));
                    g_pack[g_npack++] = h;
                }
            }
            closedir(d);
            qsort(g_pack, g_npack, sizeof *g_pack, cmp_u64);
            fprintf(stderr, "[tex] texture pack %s: %zu replacement textures\n", g_pack_dir, g_npack);
        }
    }
    if (g_dump) { mkdir("build", 0755); mkdir("build/texdump", 0755); fprintf(stderr, "[tex] dumping textures to build/texdump/\n"); }
    if (g_upscale > 1) fprintf(stderr, "[tex] upscaling textures %dx with xBRZ\n", g_upscale);
}

/* Runtime switch (hotkey / tests): 0 = original textures, else xBRZ factor. */
void texpack_set_upscale(int factor) {
    g_upscale = factor == 2 || factor == 4 ? factor : 1;
    fprintf(stderr, "[tex] texture upscaling %s\n", g_upscale > 1 ? (g_upscale == 2 ? "2x" : "4x") : "off");
}
int texpack_upscale(void) { return g_upscale; }

uint64_t texpack_hash(const uint32_t *px, int w, int h) {
    uint64_t x = 1469598103934665603ull;
    x = (x ^ (uint64_t)w) * 1099511628211ull;
    x = (x ^ (uint64_t)h) * 1099511628211ull;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) x = (x ^ px[i]) * 1099511628211ull;
    return x;
}

static int pow2_multiple(int big, int small) {
    if (big < small || big % small) return 0;
    int k = big / small;
    return (k & (k - 1)) == 0;
}

/* Called by the texture cache for each newly decoded texture. Returns a
 * malloc'd replacement (and its size), or NULL to keep the original. */
uint32_t *texpack_process(const uint32_t *px, int w, int h, int *ow, int *oh) {
    if (!g_dump && !g_pack_dir && g_upscale == 1) return NULL;
    uint64_t hash = texpack_hash(px, w, h);

    if (g_dump && !has(g_dumped, g_ndumped, hash)) {
        char path[64];
        snprintf(path, sizeof path, "build/texdump/%016llx.png", (unsigned long long)hash);
        struct stat st;
        if (stat(path, &st) != 0) stbi_write_png(path, w, h, 4, px, w * 4);
        if (g_ndumped == g_cap_dumped) g_dumped = realloc(g_dumped, sizeof *g_dumped * (g_cap_dumped = g_cap_dumped ? g_cap_dumped * 2 : 256));
        g_dumped[g_ndumped++] = hash;
        qsort(g_dumped, g_ndumped, sizeof *g_dumped, cmp_u64);
    }

    {
        const Done *d = &g_done[hash % DONE_SLOTS];
        if (d->px && d->hash == hash) return done_copy(d, ow, oh);
    }

    if (g_pack_dir && has(g_pack, g_npack, hash)) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%016llx.png", g_pack_dir, (unsigned long long)hash);
        int iw, ih, n;
        uint8_t *img = stbi_load(path, &iw, &ih, &n, 4);
        if (img && pow2_multiple(iw, w) && pow2_multiple(ih, h) && iw / w == ih / h) {
            if (!g_replaced++) fprintf(stderr, "[tex] using replacement textures from %s\n", g_pack_dir);
            *ow = iw;
            *oh = ih;
            done_put(hash, (uint32_t *)img, iw, ih);
            return (uint32_t *)img;   /* stbi allocates with malloc */
        }
        if (img) {
            fprintf(stderr, "[tex] %s is %dx%d: must be the original %dx%d times 2, 4, 8...\n", path, iw, ih, w, h);
            stbi_image_free(img);
        }
    }

    if (g_upscale > 1 && w * h <= 512 * 512) {
        uint32_t *out = malloc(sizeof(uint32_t) * (size_t)(w * h * g_upscale * g_upscale));
        xbrz_scale_rgba(g_upscale, px, out, w, h);
        g_scaled++;
        *ow = w * g_upscale;
        *oh = h * g_upscale;
        done_put(hash, out, *ow, *oh);
        return out;
    }
    return NULL;
}
