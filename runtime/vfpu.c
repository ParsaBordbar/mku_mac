/*
 * The PSP's vector unit (VFPU), executed one instruction at a time.
 *
 * The recompiler turns every VFPU instruction into `vfpu_exec(c, pc, op)`;
 * only the VFPU branches (bvf/bvt) are recompiled as real control flow.
 * Decoding at runtime is fine here: VFPU instructions are 0.4% of this game.
 *
 * Registers: 128 floats = 8 matrices x 4 columns x 4 rows. A 7-bit register
 * number packs matrix (bits 2-4), column (bits 0-1) and row/transpose (5-6);
 * how those bits are read depends on the vector size. Physical layout (as in
 * PPSSPP, which this file follows for encodings and semantics):
 *     v[matrix * 16 + column * 4 + row]
 *
 * Prefixes: vpfxs/vpfxt rewrite how the NEXT instruction reads its S and T
 * operands (swizzle, abs, negate, constants); vpfxd how it writes (saturate,
 * write mask). Every arithmetic instruction consumes ("eats") them.
 *
 * Known simplifications vs. real hardware (see docs/lessons/06b-vfpu.md):
 *   - dot products and matrix math use plain float sums (hardware rounds its
 *     own way); trig uses libm instead of the PSP's exact tables
 *   - prefixes on matrix instructions are ignored (logged once)
 *   - NaN/denormal result fix-ups are not applied
 */
#include "recomp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { PFX_S, PFX_T, PFX_D, CTRL_CC };

static const float k_prefix_consts[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};

#define PI_F 3.14159265358979323846f
static const float k_vcst[32] = {
    0.f, 3.40282347e38f, 1.41421356f, 0.70710678f, 1.12837917f, 0.63661977f, 0.31830989f,
    PI_F / 4, PI_F / 2, PI_F, 2.71828183f, 1.44269504f, 0.43429448f, 0.69314718f, 2.30258509f,
    2 * PI_F, PI_F / 6, 0.30103000f, 3.32192809f, 0.86602540f,
};

typedef union { float f[16]; uint32_t u[16]; int32_t i[16]; } Vec;

/* VFPU angles are in quarter turns: vsin(1) = sin(pi/2) = 1. Kept as separate
 * non-inlined functions: if the compiler sees sinf(a) and cosf(a) together it
 * fuses them into one sincos call, which rounds differently by 1 ulp. */
__attribute__((noinline)) static float sin_q(float x) { return sinf(x * (PI_F / 2)); }
__attribute__((noinline)) static float cos_q(float x) { return cosf(x * (PI_F / 2)); }

static int vsize(uint32_t op) { return (int)(((op >> 7) & 1) | ((op >> 14) & 2)) + 1; }   /* 1..4 lanes */

/* ---- register access ---- */

static int single_index(int reg) { return ((reg >> 2) & 7) * 16 + (reg & 3) * 4 + ((reg >> 5) & 3); }

static void vector_indices(int reg, int n, int idx[4]) {
    if (n == 1) { idx[0] = single_index(reg); return; }
    int row = n == 3 ? (reg >> 6) & 1 : (reg >> 5) & 2;
    int mtx = ((reg >> 2) & 7) * 16, col = reg & 3, transpose = (reg >> 5) & 1;
    for (int i = 0; i < n; i++)
        idx[i] = transpose ? mtx + col + ((row + i) & 3) * 4 : mtx + col * 4 + ((row + i) & 3);
}

static void read_vec(Ctx *c, Vec *out, int n, int reg) {
    int idx[4];
    vector_indices(reg, n, idx);
    for (int i = 0; i < 4; i++) out->u[i] = 0;
    for (int i = 0; i < n; i++) out->f[i] = c->v[idx[i]];
}

/* Lanes whose D-prefix write-mask bit is set are left untouched. */
static void write_vec(Ctx *c, const Vec *in, int n, int reg, int honour_mask) {
    int idx[4];
    vector_indices(reg, n, idx);
    uint32_t mask = honour_mask ? (c->vctrl[PFX_D] >> 8) & 0xF : 0;
    for (int i = 0; i < n; i++)
        if (!(mask & (1u << i))) c->v[idx[i]] = in->f[i];
}

static void matrix_indices(int reg, int n, int idx[16]) {
    int transpose = (reg >> 5) & 1, row;
    if (n == 1) { transpose = 0; row = (reg >> 5) & 3; }
    else if (n == 3) row = (reg >> 6) & 1;
    else row = (reg >> 5) & 2;
    int mtx = ((reg >> 2) & 7) * 16, col = reg & 3;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
            idx[j * 4 + i] = mtx + (transpose ? ((row + i) & 3) * 4 + ((col + j) & 3)
                                              : ((col + j) & 3) * 4 + ((row + i) & 3));
}

static void read_mtx(Ctx *c, Vec *out, int n, int reg) {
    int idx[16];
    matrix_indices(reg, n, idx);
    for (int k = 0; k < 16; k++) out->u[k] = 0;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) out->f[j * 4 + i] = c->v[idx[j * 4 + i]];
}

static void write_mtx(Ctx *c, const Vec *in, int n, int reg) {
    int idx[16];
    matrix_indices(reg, n, idx);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) c->v[idx[j * 4 + i]] = in->f[j * 4 + i];
}

/* ---- prefixes ---- */

static void apply_prefix_st(Vec *v, uint32_t pfx) {
    if (pfx == 0xE4) return;   /* identity: x y z w, no abs/neg/const */
    Vec orig = *v;
    for (int i = 0; i < 4; i++) {
        int sw = (pfx >> (2 * i)) & 3, abs_ = (pfx >> (8 + i)) & 1;
        int cst = (pfx >> (12 + i)) & 1, neg = (pfx >> (16 + i)) & 1;
        if (cst) v->f[i] = k_prefix_consts[sw + (abs_ << 2)];
        else {
            v->u[i] = orig.u[sw];
            if (abs_) v->u[i] &= 0x7FFFFFFFu;
        }
        if (neg) v->u[i] ^= 0x80000000u;
    }
}

static void apply_prefix_d(Ctx *c, Vec *v, int n) {
    uint32_t pfx = c->vctrl[PFX_D];
    for (int i = 0; i < n && pfx; i++) {
        int sat = (pfx >> (2 * i)) & 3;
        if (sat == 1) v->f[i] = v->f[i] <= 0.f ? 0.f : v->f[i] > 1.f ? 1.f : v->f[i];
        else if (sat == 3) v->f[i] = v->f[i] < -1.f ? -1.f : v->f[i] > 1.f ? 1.f : v->f[i];
    }
}

static void eat_prefixes(Ctx *c) {
    c->vctrl[PFX_S] = 0xE4;
    c->vctrl[PFX_T] = 0xE4;
    c->vctrl[PFX_D] = 0;
}

static void read_s(Ctx *c, Vec *v, int n, uint32_t op) { read_vec(c, v, n, (op >> 8) & 0x7F); apply_prefix_st(v, c->vctrl[PFX_S]); }
static void read_t(Ctx *c, Vec *v, int n, uint32_t op) { read_vec(c, v, n, (op >> 16) & 0x7F); apply_prefix_st(v, c->vctrl[PFX_T]); }

/* Finish an arithmetic op: saturate, write vd honouring the mask, eat prefixes. */
static void finish(Ctx *c, Vec *d, int n, uint32_t op) {
    apply_prefix_d(c, d, n);
    write_vec(c, d, n, op & 0x7F, 1);
    eat_prefixes(c);
}

static float dot(const float *a, const float *b, int n) {
    float s = 0.f;
    for (int i = 0; i < n; i++) s += a[i] * b[i];
    return s;
}

/* min/max order: denormals count as zero, sign-magnitude ordering. */
static int32_t order_key(uint32_t u) {
    uint32_t mag = u & 0x7FFFFFFFu;
    if (mag < 0x00800000u) mag = 0;
    return (u & 0x80000000u) ? -(int32_t)mag : (int32_t)mag;
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
    if (exp == 0x1F) bits = sign | 0x7F800000u | (man << 13);
    else if (exp == 0) {
        if (!man) bits = sign;
        else {   /* denormal half: normalise */
            int e = -1;
            do { e++; man <<= 1; } while (!(man & 0x400));
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((man & 0x3FF) << 13);
        }
    } else bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    union { uint32_t u; float f; } r = {bits};
    return r.f;
}

static void unimplemented(Ctx *c, uint32_t pc, uint32_t op, const char *what) {
    rt_unimplemented(c, pc, op, what);
}

/* ------------------------------------------------------------------------ */

static void vfpu_load_store(Ctx *c, uint32_t pc, uint32_t op) {
    uint32_t addr = c->r[(op >> 21) & 31] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
    Vec v;
    switch (op >> 26) {
    case 0x32: {   /* lv.s */
        int vt = (int)(((op >> 16) & 0x1F) | ((op & 3) << 5));
        uint32_t w = MEM_LW(addr);
        memcpy(&c->v[single_index(vt)], &w, 4);
        return;
    }
    case 0x3A: {   /* sv.s */
        int vt = (int)(((op >> 16) & 0x1F) | ((op & 3) << 5));
        uint32_t w;
        memcpy(&w, &c->v[single_index(vt)], 4);
        MEM_SW(addr, w);
        return;
    }
    }
    int vt = (int)(((op >> 16) & 0x1F) | ((op & 1) << 5));
    int offset = (addr >> 2) & 3;
    switch (op >> 26) {
    case 0x36:   /* lv.q: loads ignore the write mask and keep prefixes pending */
        for (int i = 0; i < 4; i++) v.u[i] = MEM_LW(addr + 4 * i);
        write_vec(c, &v, 4, vt, 0);
        return;
    case 0x3E:   /* sv.q */
        read_vec(c, &v, 4, vt);
        for (int i = 0; i < 4; i++) MEM_SW(addr + 4 * i, v.u[i]);
        return;
    case 0x35:   /* lvl.q / lvr.q: unaligned quad, in two halves */
        read_vec(c, &v, 4, vt);
        if (!(op & 2)) for (int i = 0; i <= offset; i++) v.u[3 - i] = MEM_LW(addr - 4 * i);
        else for (int i = 0; i <= 3 - offset; i++) v.u[i] = MEM_LW(addr + 4 * i);
        write_vec(c, &v, 4, vt, 0);
        return;
    case 0x3D:   /* svl.q / svr.q */
        read_vec(c, &v, 4, vt);
        if (!(op & 2)) for (int i = 0; i <= offset; i++) MEM_SW(addr - 4 * i, v.u[3 - i]);
        else for (int i = 0; i <= 3 - offset; i++) MEM_SW(addr + 4 * i, v.u[i]);
        return;
    }
    unimplemented(c, pc, op, "vfpu load/store");
}

static void vfpu_mftv(Ctx *c, uint32_t pc, uint32_t op) {
    uint32_t imm = op & 0xFF, rt = (op >> 16) & 31;
    if (((op >> 21) & 31) == 3) {   /* mfv / mfvc: VFPU -> GPR */
        uint32_t val = 0;
        if (imm < 128) memcpy(&val, &c->v[single_index((int)imm)], 4);
        else if (imm < 128 + 16) val = c->vctrl[imm - 128];
        if (rt) c->r[rt] = val;
    } else if (((op >> 21) & 31) == 7) {   /* mtv / mtvc */
        if (imm < 128) memcpy(&c->v[single_index((int)imm)], &c->r[rt], 4);
        else if (imm < 128 + 16) c->vctrl[imm - 128] = c->r[rt];
    } else {
        unimplemented(c, pc, op, "cop2");
    }
}

/* vadd vsub vdiv / vmul vdot vscl vhdp vcrs / vcmp vmin vmax vsge vslt */
static void vfpu_binary(Ctx *c, uint32_t pc, uint32_t op) {
    int n = vsize(op), sub = (op >> 23) & 7;
    Vec s, t, d = {{0}};
    switch (op >> 26) {
    case 0x18:   /* vadd vsub vdiv */
        if (sub != 0 && sub != 1 && sub != 7) return unimplemented(c, pc, op, "vsbn");
        read_s(c, &s, n, op);
        read_t(c, &t, n, op);
        for (int i = 0; i < n; i++)
            d.f[i] = sub == 0 ? s.f[i] + t.f[i] : sub == 1 ? s.f[i] - t.f[i] : s.f[i] / t.f[i];
        return finish(c, &d, n, op);
    case 0x19:
        if (sub == 0) {   /* vmul */
            read_s(c, &s, n, op);
            read_t(c, &t, n, op);
            for (int i = 0; i < n; i++) d.f[i] = s.f[i] * t.f[i];
            return finish(c, &d, n, op);
        }
        if (sub == 1) {   /* vdot: prefixes see all four lanes (unused ones are 0) */
            read_s(c, &s, n, op);
            read_t(c, &t, n, op);
            d.f[0] = dot(s.f, t.f, 4);
            return finish(c, &d, 1, op);
        }
        if (sub == 2) {   /* vscl: vector * scalar */
            read_s(c, &s, n, op);
            float k = c->v[single_index((int)((op >> 16) & 0x7F))];
            for (int i = 0; i < n; i++) d.f[i] = s.f[i] * k;
            return finish(c, &d, n, op);
        }
        if (sub == 4) {   /* vhdp: homogeneous dot, last S lane counts as 1 */
            read_vec(c, &s, n, (op >> 8) & 0x7F);
            s.f[n - 1] = 1.f;
            read_t(c, &t, n, op);
            d.f[0] = dot(s.f, t.f, n);
            return finish(c, &d, 1, op);
        }
        if (sub == 5 && n == 3) {   /* vcrs.t: s.yzx * t.zxy (half a cross product) */
            read_vec(c, &s, n, (op >> 8) & 0x7F);
            read_vec(c, &t, n, (op >> 16) & 0x7F);
            d.f[0] = s.f[1] * t.f[2];
            d.f[1] = s.f[2] * t.f[0];
            d.f[2] = s.f[0] * t.f[1];
            return finish(c, &d, n, op);
        }
        return unimplemented(c, pc, op, "vfpu1 op");
    case 0x1B:
        read_s(c, &s, n, op);
        read_t(c, &t, n, op);
        if (sub == 0) {   /* vcmp: sets CC bits 0-3 per lane, 4 = any, 5 = all */
            uint32_t cond = op & 15, cc = 0, any = 0, all = 1, affected = 0x30;
            for (int i = 0; i < n; i++) {
                float a = s.f[i], b = t.f[i];
                int r;
                switch (cond) {
                case 0: r = 0; break;            case 1: r = a == b; break;
                case 2: r = a < b; break;        case 3: r = a <= b; break;
                case 4: r = 1; break;            case 5: r = a != b; break;
                case 6: r = a >= b; break;       case 7: r = a > b; break;
                case 8: r = a == 0.f; break;     case 9: r = isnan(a); break;
                case 10: r = isinf(a); break;    case 11: r = !isfinite(a); break;
                case 12: r = a != 0.f; break;    case 13: r = !isnan(a); break;
                case 14: r = !isinf(a); break;   default: r = isfinite(a); break;
                }
                cc |= (uint32_t)r << i;
                any |= (uint32_t)r;
                all &= (uint32_t)r;
                affected |= 1u << i;
            }
            c->vctrl[CTRL_CC] = (c->vctrl[CTRL_CC] & ~affected) | ((cc | any << 4 | all << 5) & affected);
            eat_prefixes(c);
            return;
        }
        for (int i = 0; i < n; i++) {
            switch (sub) {
            case 2: d.u[i] = order_key(s.u[i]) < order_key(t.u[i]) ? s.u[i] : t.u[i]; break;   /* vmin */
            case 3: d.u[i] = order_key(s.u[i]) > order_key(t.u[i]) ? s.u[i] : t.u[i]; break;   /* vmax */
            case 6: d.f[i] = isnan(s.f[i]) || isnan(t.f[i]) ? 0.f : s.f[i] >= t.f[i]; break;  /* vsge */
            case 7: d.f[i] = isnan(s.f[i]) || isnan(t.f[i]) ? 0.f : s.f[i] < t.f[i]; break;   /* vslt */
            default: return unimplemented(c, pc, op, "vscmp");
            }
        }
        return finish(c, &d, n, op);
    }
}

/* opcode 0x34: unary ops, conversions, constants */
static void vfpu_34(Ctx *c, uint32_t pc, uint32_t op) {
    int n = vsize(op), sub5 = (op >> 21) & 31, sub16 = (op >> 16) & 31;
    Vec s, d = {{0}};

    if (sub5 == 0) {   /* VFPU4 table */
        int vd = op & 0x7F;
        if (sub16 == 3 || sub16 == 6 || sub16 == 7) {   /* vidt vzero vone: S prefix is replaced */
            int offmask = n >= 3 ? 3 : 1;
            for (int i = 0; i < n; i++)
                d.f[i] = sub16 == 7 ? 1.f : sub16 == 6 ? 0.f : (i == (vd & offmask) ? 1.f : 0.f);
            return finish(c, &d, n, op);
        }
        read_s(c, &s, n, op);
        for (int i = 0; i < n; i++) {
            float x = s.f[i];
            switch (sub16) {
            case 0: d.u[i] = s.u[i]; break;                              /* vmov */
            case 1: d.u[i] = s.u[i] & 0x7FFFFFFFu; break;                /* vabs */
            case 2: d.u[i] = s.u[i] ^ 0x80000000u; break;                /* vneg */
            case 4: d.f[i] = x <= 0.f ? 0.f : x > 1.f ? 1.f : x; break;  /* vsat0 */
            case 5: d.f[i] = x < -1.f ? -1.f : x > 1.f ? 1.f : x; break; /* vsat1 */
            case 16: d.f[i] = 1.f / x; break;                            /* vrcp */
            case 17: d.f[i] = 1.f / sqrtf(x); break;                     /* vrsq */
            case 18: d.f[i] = sin_q(x); break;                           /* vsin: argument in quarter turns */
            case 19: d.f[i] = cos_q(x); break;                           /* vcos */
            case 20: d.f[i] = exp2f(x); break;                           /* vexp2 */
            case 21: d.f[i] = log2f(x); break;                           /* vlog2 */
            case 22: d.f[i] = sqrtf(x); break;                           /* vsqrt */
            case 23: d.f[i] = asinf(x) / (PI_F / 2); break;              /* vasin: result in quarter turns */
            case 24: d.f[i] = -1.f / x; break;                           /* vnrcp */
            case 26: d.f[i] = -sin_q(x); break;                          /* vnsin */
            case 28: d.f[i] = 1.f / exp2f(x); break;                     /* vrexp2 */
            default: return unimplemented(c, pc, op, "vfpu4 op");
            }
        }
        return finish(c, &d, n, op);
    }

    if (sub5 == 1) {   /* VFPU7: integer pack/unpack */
        if (sub16 >= 24 && sub16 <= 27) {   /* vuc2ifs vc2i vus2i vs2i */
            read_s(c, &s, n, op);
            int on = n;
            switch (sub16 & 3) {
            case 0: for (int i = 0; i < 4; i++) d.u[i] = ((s.u[0] >> (8 * i)) & 0xFF) * 0x01010101u >> 1; on = 4; break;
            case 1: for (int i = 0; i < 4; i++) d.u[i] = ((s.u[0] >> (8 * i)) & 0xFF) << 24; on = 4; break;
            case 2:
            case 3: {
                int m = n > 2 ? 2 : n;
                for (int i = 0; i < m; i++) {
                    uint32_t lo = s.u[i] & 0xFFFF, hi = s.u[i] & 0xFFFF0000u;
                    d.u[2 * i] = (sub16 & 3) == 3 ? lo << 16 : lo << 15;
                    d.u[2 * i + 1] = (sub16 & 3) == 3 ? hi : hi >> 1;
                }
                on = m * 2;
                break;
            }
            }
            return finish(c, &d, on, op);
        }
        if (sub16 >= 28) {   /* vi2uc vi2c vi2us vi2s */
            read_s(c, &s, n, op);
            int on = 1;
            switch (sub16 & 3) {
            case 0: for (int i = 0; i < 4; i++) { int32_t x = s.i[i] < 0 ? 0 : s.i[i]; d.u[0] |= ((uint32_t)(x >> 23) & 0xFF) << (8 * i); } break;
            case 1: for (int i = 0; i < 4; i++) d.u[0] |= (s.u[i] >> 24) << (8 * i); break;
            case 2:
            case 3: {
                int elems = (n + 1) / 2;
                for (int i = 0; i < elems; i++) {
                    if ((sub16 & 3) == 2) {
                        int32_t lo = s.i[2 * i] < 0 ? 0 : s.i[2 * i], hi = s.i[2 * i + 1] < 0 ? 0 : s.i[2 * i + 1];
                        d.u[i] = (uint32_t)(lo >> 15) | ((uint32_t)(hi >> 15) << 16);
                    } else {
                        d.u[i] = (s.u[2 * i] >> 16) | ((s.u[2 * i + 1] >> 16) << 16);
                    }
                }
                on = n >= 3 ? 2 : 1;
                break;
            }
            }
            return finish(c, &d, on, op);
        }
        return unimplemented(c, pc, op, "vfpu7 op");
    }

    if (sub5 == 2) {   /* VFPU9 */
        switch (sub16) {
        case 4:   /* vocp: 1 - s */
            read_s(c, &s, n, op);
            for (int i = 0; i < n; i++) d.f[i] = isnan(s.f[i]) ? fabsf(s.f[i]) : 1.f - s.f[i];
            return finish(c, &d, n, op);
        case 6:   /* vfad: sum of lanes */
            read_s(c, &s, n, op);
            d.f[0] = 0.f;
            for (int i = 0; i < 4; i++) d.f[0] += s.f[i];
            return finish(c, &d, 1, op);
        case 10:   /* vsgn */
            read_s(c, &s, n, op);
            for (int i = 0; i < n; i++)
                d.f[i] = (s.u[i] & 0x7F800000u) == 0 ? 0.f : (s.u[i] >> 31) ? -1.f : 1.f;
            return finish(c, &d, n, op);
        case 16:   /* vmfvc: control register -> VFPU register */
            d.u[0] = ((op >> 8) & 0x7F) < 16 ? c->vctrl[(op >> 8) & 0x7F] : 0;
            write_vec(c, &d, 1, op & 0x7F, 0);
            return;
        case 17:   /* vmtvc */
            read_vec(c, &s, 1, (op >> 8) & 0x7F);
            if ((op & 0x7F) < 16) c->vctrl[op & 0x7F] = s.u[0];
            return;
        }
        return unimplemented(c, pc, op, "vfpu9 op");
    }

    if (sub5 == 3) {   /* vcst: load a constant */
        for (int i = 0; i < n; i++) d.f[i] = k_vcst[sub16];
        return finish(c, &d, n, op);
    }

    if (sub5 >= 16 && sub5 <= 19) {   /* vf2in vf2iz vf2iu vf2id: float -> int with scale 2^imm */
        read_s(c, &s, n, op);
        double mult = (double)(1u << sub16);
        for (int i = 0; i < n; i++) {
            if (isnan(s.f[i])) { d.u[i] = 0x7FFFFFFFu; continue; }
            double x = (double)s.f[i] * mult;
            if (x > 2147483647.0) d.u[i] = 0x7FFFFFFFu;
            else if (x <= -2147483648.0) d.u[i] = 0x80000000u;
            else switch (sub5) {
                case 16: d.i[i] = (int32_t)nearbyint(x); break;   /* round to nearest even */
                case 17: d.i[i] = (int32_t)trunc(x); break;
                case 18: d.i[i] = (int32_t)ceil(x); break;
                default: d.i[i] = (int32_t)floor(x); break;
                }
        }
        write_vec(c, &d, n, op & 0x7F, 1);   /* no saturation on integers */
        eat_prefixes(c);
        return;
    }

    if (sub5 == 20) {   /* vi2f: int -> float, scaled by 2^-imm */
        read_s(c, &s, n, op);
        float mult = 1.f / (float)(1u << sub16);
        for (int i = 0; i < n; i++) d.f[i] = (float)s.i[i] * mult;
        return finish(c, &d, n, op);
    }

    if (sub5 == 21) {   /* vcmov: conditional move on a CC bit (imm3 == 6: per lane) */
        int tf = (op >> 19) & 1, imm3 = (op >> 16) & 7;
        read_s(c, &s, n, op);
        read_vec(c, &d, n, op & 0x7F);
        apply_prefix_st(&d, c->vctrl[PFX_T]);
        uint32_t cc = c->vctrl[CTRL_CC];
        for (int i = 0; i < n; i++) {
            int bit = imm3 == 6 ? (cc >> i) & 1 : (cc >> imm3) & 1;
            if (bit == !tf) d.f[i] = s.f[i];
        }
        return finish(c, &d, n, op);
    }

    unimplemented(c, pc, op, "vfpu 0x34 op");
}

/* opcode 0x3C: matrix operations, vrot */
static int g_warned_matrix_prefix;

static void vfpu_matrix(Ctx *c, uint32_t pc, uint32_t op) {
    int n = vsize(op), sub5 = (op >> 21) & 31;
    int vd = op & 0x7F, vs = (op >> 8) & 0x7F, vt = (op >> 16) & 0x7F;
    Vec s, t, d = {{0}};

    if (sub5 != 29 && (c->vctrl[PFX_S] != 0xE4 || c->vctrl[PFX_T] != 0xE4 || c->vctrl[PFX_D]) && !g_warned_matrix_prefix++)
        fprintf(stderr, "[vfpu] prefix on a matrix instruction at 0x%08X ignored\n", pc);

    if (sub5 < 4) {   /* vmmul */
        read_mtx(c, &s, n, vs);
        read_mtx(c, &t, n, vt);
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++) d.f[a * 4 + b] = dot(&s.f[b * 4], &t.f[a * 4], n);
        write_mtx(c, &d, n, vd);
    } else if (sub5 < 16) {   /* vtfmN / vhtfmN: matrix * vector */
        int ins = (op >> 23) & 3, side = ins + 1, tn = n < side ? n : side;
        float t2[4] = {0, 0, 0, 0};
        read_mtx(c, &s, side, vs);
        read_vec(c, &t, side, vt);
        for (int i = 0; i < 4; i++) t2[i] = i < tn ? t.f[i] : i == ins ? 1.f : 0.f;   /* vhtfm: w = 1 */
        for (int i = 0; i <= ins; i++) d.f[i] = dot(&s.f[i * 4], t2, 4);
        write_vec(c, &d, side, vd, 1);
    } else if (sub5 < 20) {   /* vmscl: matrix * scalar */
        read_mtx(c, &s, n, vs);
        float k = c->v[single_index(vt)];
        for (int k2 = 0; k2 < 16; k2++) d.f[k2] = s.f[k2] * k;
        write_mtx(c, &d, n, vd);
    } else if (sub5 < 24) {   /* vcrsp.t (cross product) / vqmul.q (quaternion product) */
        read_vec(c, &s, n, vs);
        read_vec(c, &t, n, vt);
        float *a = s.f, *b = t.f;
        if (n == 3) {
            d.f[0] = a[1] * b[2] - a[2] * b[1];
            d.f[1] = a[2] * b[0] - a[0] * b[2];
            d.f[2] = a[0] * b[1] - a[1] * b[0];
        } else if (n == 4) {
            float q0[4] = {b[3], b[2], -b[1], b[0]}, q1[4] = {-b[2], b[3], b[0], b[1]};
            float q2[4] = {b[1], -b[0], b[3], b[2]}, q3[4] = {-b[0], -b[1], -b[2], b[3]};
            d.f[0] = dot(a, q0, 4); d.f[1] = dot(a, q1, 4); d.f[2] = dot(a, q2, 4); d.f[3] = dot(a, q3, 4);
        } else return unimplemented(c, pc, op, "vcrsp/vqmul size");
        write_vec(c, &d, n, vd, 1);
    } else if (sub5 == 28) {   /* vmmov vmidt vmzero vmone */
        int kind = (op >> 16) & 15;
        if (kind == 0) read_mtx(c, &d, n, vs);
        else if (kind == 3 || kind == 6 || kind == 7)
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) d.f[j * 4 + i] = kind == 7 ? 1.f : kind == 3 && i == j ? 1.f : 0.f;
        else return unimplemented(c, pc, op, "vfpu matrix set");
        write_mtx(c, &d, n, vd);
    } else if (sub5 == 29) {   /* vrot: put sin/cos of an angle into chosen lanes */
        int imm = (op >> 16) & 31, sin_lane = (imm >> 2) & 3, cos_lane = imm & 3;
        float angle = c->v[single_index(vs)];
        float sine = sin_q(angle), cosine = cos_q(angle);
        if (imm & 0x10) sine = -sine;
        if (sin_lane == cos_lane) for (int i = 0; i < 4; i++) d.f[i] = sine;
        else d.f[sin_lane] = sine;
        d.f[cos_lane] = cosine;
        apply_prefix_d(c, &d, n);
        write_vec(c, &d, n, vd, 1);
    } else {
        return unimplemented(c, pc, op, "vfpu6 op");
    }
    eat_prefixes(c);
}

/* ------------------------------------------------------------------------ */

void vfpu_exec(Ctx *c, uint32_t pc, uint32_t op) {
    switch (op >> 26) {
    case 0x12: vfpu_mftv(c, pc, op); return;
    case 0x18: case 0x19: case 0x1B: vfpu_binary(c, pc, op); return;
    case 0x34: vfpu_34(c, pc, op); return;
    case 0x37: {
        int sub = (op >> 23) & 7;
        if (sub < 6) {   /* vpfxs / vpfxt / vpfxd */
            int which = (op >> 24) & 3;
            c->vctrl[PFX_S + which] = which == PFX_D ? op & 0xFFF : op & 0xFFFFF;
            return;
        }
        Vec d = {{0}};   /* viim: integer immediate / vfim: half-float immediate */
        d.f[0] = sub == 6 ? (float)(int16_t)(op & 0xFFFF) : half_to_float((uint16_t)op);
        apply_prefix_d(c, &d, 1);
        write_vec(c, &d, 1, (op >> 16) & 0x7F, 1);
        eat_prefixes(c);
        return;
    }
    case 0x3C: vfpu_matrix(c, pc, op); return;
    case 0x32: case 0x3A: case 0x35: case 0x36: case 0x3D: case 0x3E: vfpu_load_store(c, pc, op); return;
    case 0x3F:   /* vnop / vsync / vflush */
        if ((op & 0xFFFF0000u) != 0xFFFF0000u) eat_prefixes(c);
        return;
    }
    unimplemented(c, pc, op, "vfpu");
}
