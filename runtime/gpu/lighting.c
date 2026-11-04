/*
 * The GE's fixed-function lighting, per vertex. Formulas follow PPSSPP's
 * GPU/Common/VertexShaderGenerator.cpp (plain floats, not the chip's exact
 * 24-bit arithmetic):
 *
 *   sum  = global ambient * material ambient + emissive
 *   each enabled light adds, scaled by attenuation and spot factor:
 *     light ambient * material ambient + light diffuse * material diffuse * max(N.L, 0)
 *   specular adds light specular * material specular * (N.H)^shininess when N.L >= 0
 *
 * tests/runtime/lighting_test.c checks it against hand-computed values.
 */
#include "lighting.h"

#include <math.h>

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void normalize_or_z(const float in[3], float out[3]) {
    float len2 = dot3(in, in);
    if (len2 > 0.f && isfinite(len2)) {
        float k = 1.f / sqrtf(len2);
        out[0] = in[0] * k; out[1] = in[1] * k; out[2] = in[2] * k;
    } else {
        out[0] = 0.f; out[1] = 0.f; out[2] = 1.f;
    }
}

float ge_pow(float x, float e) {
    if (e <= 0.f) return 1.f;
    return x > 0.f ? powf(x, e) : 0.f;
}

static float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

void ge_light_vertex(const GeLighting *g, const float world_pos[3], const float world_normal[3],
                     const float color0[4], int has_color, float out[4]) {
    float n[3];
    normalize_or_z(world_normal, n);
    const float *mamb = (g->update & 1) && has_color ? color0 : g->mat_amb;
    const float *mdif = (g->update & 2) && has_color ? color0 : g->mat_dif;
    const float *mspe = (g->update & 4) && has_color ? color0 : g->mat_spe;

    float sum[4], spec[3] = {0.f, 0.f, 0.f};
    for (int k = 0; k < 4; k++) sum[k] = g->ambient[k] * mamb[k];
    for (int k = 0; k < 3; k++) sum[k] += g->mat_emi[k];

    for (int i = 0; i < 4; i++) {
        const GeLight *l = &g->l[i];
        if (!l->on) continue;
        float L[3], scale = 1.f;
        if (l->type == 0) {
            normalize_or_z(l->pos, L);
        } else {
            float d[3] = {l->pos[0] - world_pos[0], l->pos[1] - world_pos[1], l->pos[2] - world_pos[2]};
            float dist2 = dot3(d, d), dist = sqrtf(dist2);
            normalize_or_z(d, L);
            scale = clamp01(1.f / (l->att[0] + l->att[1] * dist + l->att[2] * dist2));
            if (l->type != 1) {   /* spot */
                float sd[3];
                normalize_or_z(l->dir, sd);
                float angle = dot3(sd, L);
                scale = angle >= l->cutoff ? scale * ge_pow(angle, l->conv) : 0.f;
            }
        }
        float ldot = dot3(L, n);
        float diffuse = ldot;
        if (l->comp == 2) diffuse = ge_pow(ldot, g->shininess);
        if (diffuse < 0.f) diffuse = 0.f;
        for (int k = 0; k < 3; k++) sum[k] += (l->amb[k] * mamb[k] + l->dif[k] * mdif[k] * diffuse) * scale;
        if (l->comp == 1 && ldot >= 0.f) {
            float h[3] = {L[0] + g->view_dir[0], L[1] + g->view_dir[1], L[2] + g->view_dir[2]}, hn[3];
            normalize_or_z(h, hn);
            float s = ge_pow(dot3(hn, n), g->shininess);
            for (int k = 0; k < 3; k++) spec[k] += l->spe[k] * mspe[k] * s * scale;
        }
    }
    /* Separate specular (LIGHTMODE 1) is added here too: the renderer has one colour per vertex. */
    for (int k = 0; k < 3; k++) out[k] = clamp01(sum[k] + spec[k]);
    out[3] = clamp01(sum[3]);
}

float ge_shade_map_coord(const GeLighting *g, int ls, const float world_pos[3], const float world_normal[3]) {
    const GeLight *l = &g->l[ls & 3];
    float n[3], L[3];
    normalize_or_z(world_normal, n);
    if (l->type == 0) {
        normalize_or_z(l->pos, L);
    } else {
        float d[3] = {l->pos[0] - world_pos[0], l->pos[1] - world_pos[1], l->pos[2] - world_pos[2]};
        normalize_or_z(d, L);
    }
    if (l->comp == 1) {
        float h[3] = {L[0] + g->view_dir[0], L[1] + g->view_dir[1], L[2] + g->view_dir[2]};
        normalize_or_z(h, L);
    }
    return (dot3(L, n) + 1.f) * 0.5f;
}
