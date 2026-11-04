/* The PSP graphics engine ("GE"): display lists in, pixels in VRAM out. */
#pragma once

#include "../recomp.h"

#define VRAM_BASE 0x04000000u
#define VRAM_SIZE 0x00200000u

/* Everything the display lists have set. cmd[] holds the last 24-bit value
 * written by each command, like the hardware's registers. */
typedef struct {
    uint32_t cmd[256];
    float world[12], view[12], proj[16], tgen[12], bone[8 * 12];
    uint32_t world_i, view_i, proj_i, tgen_i, bone_i;
    float morph[8];
    uint32_t vaddr, iaddr, offset_addr;
} GeState;

extern GeState ge;

/* GE floats are 24 bits: the top 24 bits of an IEEE single. */
static inline float ge_float(uint32_t data) {
    uint32_t bits = data << 8;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* "Relative" addresses combine BASE (top 4 bits) with the 24-bit data. */
static inline uint32_t ge_address(uint32_t data) {
    return (ge.offset_addr + (((ge.cmd[0x10] & 0x000F0000u) << 8) | data)) & 0x0FFFFFFFu;
}

/* raster.c */
void gpu_draw_prim(uint32_t data);        /* PRIM command */
void gpu_block_transfer(uint32_t data);   /* TRANSFERSTART */
void gpu_load_clut(void);                 /* LOADCLUT */
void gpu_texture_flush(void);             /* TEXFLUSH: re-validate cached textures */
void gpu_flush(void);                     /* draw all queued primitives */
void gpu_frame_end(void);                 /* once per shown frame (free camera bookkeeping) */
void gpu_stats_report(void);   /* ge.c: command counts (MKU_GE_STATS=1) */
void gpu_draw_report(void);    /* raster.c */

void gpu_set_no_lighting(int off);        /* raster.c: MKU_NO_LIGHTING */

/* surface.c: high-resolution twins of the render targets (MKU_SCALE) */
extern int g_scale;
void surf_init(void);
uint32_t surf_target(uint32_t vaddr, int stride, int fmt, int rows);   /* fmt 4 = depth */
void surf_writeback(uint32_t addr, uint32_t len);
uint32_t surf_display(uint32_t vaddr, int stride, int fmt);
void surf_frame_end(void);
void surf_set_scale(int n);

/* texpack.c: HD texture dumps, replacement packs, xBRZ upscaling */
void texpack_init(void);
uint32_t *texpack_process(const uint32_t *px, int w, int h, int *ow, int *oh);
void texpack_set_upscale(int factor);
int texpack_upscale(void);
void gpu_texture_cache_clear(void);   /* raster.c */

/* display.c / host: the framebuffer the game wants shown */
void display_framebuffer(uint32_t *addr, uint32_t *width, uint32_t *format);
