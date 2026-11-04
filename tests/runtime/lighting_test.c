/* GE lighting against values worked out by hand from the formulas. */
#include "gpu/lighting.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void near_eq(const char *what, float got, float want) {
    if (fabsf(got - want) > 1e-4f) {
        printf("FAIL %s: got %f, want %f\n", what, got, want);
        fails++;
    }
}

static GeLighting base(void) {
    GeLighting g;
    memset(&g, 0, sizeof g);
    for (int k = 0; k < 3; k++) { g.mat_amb[k] = 0.5f; g.mat_dif[k] = 0.8f; g.mat_spe[k] = 1.f; }
    g.mat_amb[3] = 1.f;
    g.view_dir[2] = 1.f;
    g.shininess = 2.f;
    return g;
}

int main(void) {
    const float pos[3] = {0, 0, 0}, nz[3] = {0, 0, 2};   /* normals get normalised */
    const float white[4] = {1, 1, 1, 1}, red[4] = {1, 0, 0, 0.5f};
    float o[4];

    /* global ambient * material ambient + emissive; alpha = ambient a * material a */
    GeLighting g = base();
    g.ambient[0] = 0.5f; g.ambient[1] = 0.25f; g.ambient[2] = 0.f; g.ambient[3] = 0.5f;
    g.mat_emi[2] = 0.1f;
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("ambient r", o[0], 0.25f); near_eq("ambient g", o[1], 0.125f);
    near_eq("emissive b", o[2], 0.1f); near_eq("alpha", o[3], 0.5f);

    /* directional light at 60 degrees: N.L = 0.5; diffuse 0.8 * 1 * 0.5, plus light ambient 0.5 * 0.5 */
    g = base();
    g.l[0].on = 1;
    g.l[0].pos[0] = sqrtf(3.f) * 3; g.l[0].pos[2] = 3.f;   /* unnormalised direction */
    for (int k = 0; k < 3; k++) { g.l[0].dif[k] = 1.f; g.l[0].amb[k] = 0.5f; }
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("diffuse", o[0], 0.4f + 0.25f);

    /* light behind the surface: diffuse clamps to 0, light ambient stays */
    g.l[0].pos[2] = -3.f;
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("back-facing", o[0], 0.25f);

    /* material update: diffuse from the vertex colour (red), only with a vertex colour */
    g = base();
    g.update = 2;
    g.l[0].on = 1; g.l[0].pos[2] = 1.f;
    for (int k = 0; k < 3; k++) g.l[0].dif[k] = 1.f;
    ge_light_vertex(&g, pos, nz, red, 1, o);
    near_eq("update r", o[0], 1.f); near_eq("update g", o[1], 0.f);
    ge_light_vertex(&g, pos, nz, red, 0, o);
    near_eq("no vertex colour g", o[1], 0.8f);

    /* point light 2 units away, attenuation 1 / (1 + 0.5 * 2 + 0.25 * 4) = 1/3 */
    g = base();
    g.l[0].on = 1; g.l[0].type = 1; g.l[0].pos[2] = 2.f;
    g.l[0].att[0] = 1.f; g.l[0].att[1] = 0.5f; g.l[0].att[2] = 0.25f;
    for (int k = 0; k < 3; k++) g.l[0].dif[k] = 1.f;
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("attenuation", o[0], 0.8f / 3.f);

    /* spot: inside the cone (cos = 1) with conv 2, then outside it */
    g.l[0].type = 2; g.l[0].dir[2] = 1.f; g.l[0].cutoff = 0.5f; g.l[0].conv = 2.f;
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("spot inside", o[0], 0.8f / 3.f);
    g.l[0].dir[2] = -1.f;
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("spot outside", o[0], 0.f);

    /* specular: L = view = N, so N.H = 1 */
    g = base();
    g.l[0].on = 1; g.l[0].comp = 1; g.l[0].pos[2] = 1.f;
    for (int k = 0; k < 3; k++) g.l[0].spe[k] = 0.5f;
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("specular", o[0], 0.5f);
    /* L at 90 degrees to the view: H at 45 degrees, N.H^2 = 0.5, but N.L = 0 still counts */
    g.l[0].pos[0] = 1.f; g.l[0].pos[2] = 0.f;
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("specular 45", o[0], 0.25f);
    /* behind: no specular */
    g.l[0].pos[2] = -1.f;
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("specular behind", o[0], 0.f);

    /* powered diffuse: (N.L)^shininess = 0.5^2 */
    g = base();
    g.l[0].on = 1; g.l[0].comp = 2;
    g.l[0].pos[0] = sqrtf(3.f); g.l[0].pos[2] = 1.f;
    for (int k = 0; k < 3; k++) g.l[0].dif[k] = 1.f;
    ge_light_vertex(&g, pos, nz, white, 0, o);
    near_eq("powered diffuse", o[0], 0.8f * 0.25f);

    /* clamping, and a zero normal acts as +z */
    g = base();
    g.mat_emi[0] = 3.f;
    const float zero[3] = {0, 0, 0};
    ge_light_vertex(&g, pos, zero, white, 0, o);
    near_eq("clamp", o[0], 1.f);

    /* environment map: (N.L + 1) / 2 */
    g = base();
    g.l[1].pos[0] = 1.f;
    near_eq("shade map side", ge_shade_map_coord(&g, 1, pos, nz), 0.5f);
    g.l[1].pos[0] = 0.f; g.l[1].pos[2] = -1.f;
    near_eq("shade map back", ge_shade_map_coord(&g, 1, pos, nz), 0.f);

    near_eq("pow 0 exponent", ge_pow(0.f, 0.f), 1.f);
    near_eq("pow negative base", ge_pow(-1.f, 2.f), 0.f);

    if (fails) { printf("lighting_test: %d failed\n", fails); return 1; }
    printf("lighting_test: ok\n");
    return 0;
}
