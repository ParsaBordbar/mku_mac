/* GE per-vertex lighting and texture coordinate generation (lighting.c). */
#pragma once

#include <stdint.h>

typedef struct {
    int on, type, comp;      /* type: 0 directional, 1 point, 2 spot; comp: 0 diffuse, 1 + specular, 2 powered diffuse */
    float pos[3], dir[3], att[3], cutoff, conv;
    float amb[3], dif[3], spe[3];
} GeLight;

typedef struct {
    GeLight l[4];
    float ambient[4];        /* global ambient, RGBA 0..1 */
    float mat_amb[4];        /* material ambient + material alpha */
    float mat_dif[3], mat_spe[3], mat_emi[3];
    float shininess;
    int update;              /* material update bits: 1 ambient, 2 diffuse, 4 specular come from the vertex */
    float view_dir[3];
} GeLighting;

float ge_pow(float x, float e);

/* color0 (RGBA 0..1) is the vertex colour; has_color says whether there is one.
 * world_pos and world_normal are in world space; the normal needs no length. */
void ge_light_vertex(const GeLighting *g, const float world_pos[3], const float world_normal[3],
                     const float color0[4], int has_color, float out[4]);

/* Environment map: one texture coordinate (0..1) from light `ls` and the normal. */
float ge_shade_map_coord(const GeLighting *g, int ls, const float world_pos[3], const float world_normal[3]);
