/* Turning the PSP framebuffer (in VRAM) into host pixels: for the window and
 * for screenshots. MKU_SHOTS=60,300,900 saves those frame numbers as
 * build/shots/frame_NNNNN.bmp. */
#include "gpu.h"
#include "../host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SCREEN_W 480
#define SCREEN_H 272

/* RGBA8888 (R in the lowest byte) of what the game asked to display, at the
 * rendering resolution (480x272 times MKU_SCALE). */
int present_capture(uint32_t *out, int *ow, int *oh) {
    uint32_t addr, stride, fmt;
    display_framebuffer(&addr, &stride, &fmt);
    if (!addr || !stride) return 0;
    int n = 1;
    uint32_t hi = surf_display((addr & 0x0FFFFFFFu) | 0x04000000u, (int)stride, (int)fmt);
    if (hi) { addr = hi; stride *= (uint32_t)g_scale; n = g_scale; }
    int W = SCREEN_W * n, H = SCREEN_H * n;
    *ow = W; *oh = H;
    /* 5/6/4-bit -> 8-bit expansion tables (same values as v * 255 / 31 etc.) */
    static uint8_t e5[32], e6[64];
    if (!e6[63]) {
        for (int i = 0; i < 32; i++) e5[i] = (uint8_t)(i * 255 / 31);
        for (int i = 0; i < 64; i++) e6[i] = (uint8_t)(i * 255 / 63);
    }
    for (int y = 0; y < H; y++) {
        uint32_t *o = out + y * W;
        if (fmt == 3) {
            const uint32_t *row = (const uint32_t *)rt_ptr(addr + (uint32_t)(y * (int)stride * 4));
            for (int x = 0; x < W; x++) o[x] = row[x] | 0xFF000000u;   /* displayed alpha is ignored */
            continue;
        }
        const uint16_t *row = (const uint16_t *)rt_ptr(addr + (uint32_t)(y * (int)stride * 2));
        for (int x = 0; x < W; x++) {
            uint32_t p = row[x], r, g, b;
            if (fmt == 0) { r = e5[p & 31]; g = e6[(p >> 5) & 63]; b = e5[(p >> 11) & 31]; }
            else if (fmt == 1) { r = e5[p & 31]; g = e5[(p >> 5) & 31]; b = e5[(p >> 10) & 31]; }
            else { r = (p & 15) * 17; g = ((p >> 4) & 15) * 17; b = ((p >> 8) & 15) * 17; }
            o[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    }
    return 1;
}

static void write_bmp(const char *path, const uint32_t *px, int W, int H) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t row = (uint32_t)W * 3, size = 54 + row * (uint32_t)H;
    uint8_t h[54] = {'B', 'M'};
    memcpy(h + 2, &size, 4);
    h[10] = 54; h[14] = 40;
    int32_t w = W, hh = H;
    memcpy(h + 18, &w, 4); memcpy(h + 22, &hh, 4);
    h[26] = 1; h[28] = 24;
    fwrite(h, 1, 54, f);
    for (int y = H - 1; y >= 0; y--)   /* BMP rows go bottom-up, BGR */
        for (int x = 0; x < W; x++) {
            uint32_t p = px[y * W + x];
            uint8_t bgr[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p};
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
}

/* Called for every displayed frame (sceDisplaySetFrameBuf). */
/* MKU_FPS=1: once a second, frame time statistics. */
extern uint64_t now_us(void);
extern uint64_t g_raster_us, g_idle_us, rt_interp_count, g_tris_queued, g_tris_culled_w, g_draws_3d;
extern uint64_t vblank_count(void);

static void fps_stats(void) {
    static int on = -1;
    static uint64_t last, window_start, worst, frames, raster_at_start;
    if (on < 0) on = getenv("MKU_FPS") != NULL;
    if (!on) return;
    uint64_t t = now_us();
    static uint64_t raster_last, idle_last, interp_last, vb_last, nframe;
    nframe++;
    if (last && t - last > 20000u && getenv("MKU_FPS_SLOW"))
        fprintf(stderr, "[fps] slow frame #%llu: %.1f ms = %llu vblanks (rendering %.1f ms, idle %.1f ms, %llu interpreted instrs)\n",
                (unsigned long long)nframe, (t - last) / 1000.0, (unsigned long long)(vblank_count() - vb_last),
                (g_raster_us - raster_last) / 1000.0, (g_idle_us - idle_last) / 1000.0,
                (unsigned long long)(rt_interp_count - interp_last));
    raster_last = g_raster_us; idle_last = g_idle_us; interp_last = rt_interp_count; vb_last = vblank_count();
    if (last && t - last > worst) worst = t - last;
    last = t;
    frames++;
    if (t - window_start >= 1000000u) {
        if (window_start)
            fprintf(stderr, "[fps] %llu fps, slowest frame %.1f ms, rendering %.1f ms/frame (%.0f%% of a 16.7 ms frame)\n",
                    (unsigned long long)frames, worst / 1000.0, (g_raster_us - raster_at_start) / 1000.0 / frames,
                    100.0 * (g_raster_us - raster_at_start) / frames / 16683.0);
        static uint64_t q0, c0, d0, k0[3];
        extern uint64_t g_area_kind[3];
        if (window_start && frames)
            fprintf(stderr, "[fps]   bbox pixels per frame: clear %llu, sprites %llu, triangles %llu\n",
                    (unsigned long long)((g_area_kind[0] - k0[0]) / frames), (unsigned long long)((g_area_kind[1] - k0[1]) / frames),
                    (unsigned long long)((g_area_kind[2] - k0[2]) / frames));
        memcpy(k0, g_area_kind, sizeof k0);
        if (window_start && frames)
            fprintf(stderr, "[fps]   per frame: %llu 3D draws, %llu prims queued, %llu vertices behind the camera\n",
                    (unsigned long long)((g_draws_3d - d0) / frames), (unsigned long long)((g_tris_queued - q0) / frames),
                    (unsigned long long)((g_tris_culled_w - c0) / frames));
        q0 = g_tris_queued; c0 = g_tris_culled_w; d0 = g_draws_3d;
        window_start = t;
        frames = 0;
        worst = 0;
        raster_at_start = g_raster_us;
    }
}

extern int g_dump_draws;
void raster_bench_report(void);

void present_frame_shown(uint64_t frame) {
    fps_stats();
    static long dump_at = -2;
    if (dump_at == -2) dump_at = getenv("MKU_GE_DUMP") ? atol(getenv("MKU_GE_DUMP")) : -1;
    g_dump_draws = dump_at >= 0 && frame >= (uint64_t)dump_at && frame < (uint64_t)dump_at + 2;
    gpu_texture_flush();   /* re-validate textures at least once per frame */
    gpu_frame_end();
    raster_bench_report();
    surf_frame_end();
    static uint32_t rgba[SCREEN_W * SCREEN_H * 16];
    int fw = SCREEN_W, fh = SCREEN_H;
    if (present_capture(rgba, &fw, &fh)) host_submit_frame(rgba, fw, fh);
    static const char *shots = (const char *)-1;
    if (shots == (const char *)-1) shots = getenv("MKU_SHOTS");
    if (!shots) return;
    char want[24];
    snprintf(want, sizeof want, "%llu", (unsigned long long)frame);
    for (const char *s = shots; *s;) {
        size_t n = strcspn(s, ",");
        if (n == strlen(want) && !strncmp(s, want, n)) {
            uint32_t *px = rgba;
            {
                char path[64];
                mkdir("build/shots", 0755);
                snprintf(path, sizeof path, "build/shots/frame_%05llu.bmp", (unsigned long long)frame);
                write_bmp(path, px, fw, fh);
                fprintf(stderr, "[shot] %s at %.2f s\n", path, now_us() / 1e6);
            }
        }
        s += n + (s[n] == ',');
    }
}
