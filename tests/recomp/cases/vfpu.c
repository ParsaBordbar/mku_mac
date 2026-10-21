/* VFPU tests. Each t_* function builds input data in memory, then
 *   on MIPS:  calls an assembly helper (vf_*) that runs real VFPU instructions
 *   natively: calls a C reference (ref_*) of what those instructions should do.
 *
 * No assembler knows VFPU instructions, so they are written as `.word`
 * expressions via the macros below. Register names:
 *   C<m><c>0 = column c of matrix m = m*4 + c        (e.g. C100 = 4)
 *   R<m>0<r> = row r of matrix m    = 0x20 | m*4 + r (e.g. R001 = 0x21)
 *   S<m><c><r> = single = m*4 + c + r*32             (e.g. S010 = 1)
 *   M<m>00 = matrix m = m*4, E<m>00 = its transpose = 0x20 | m*4
 *
 * The references follow the documented behaviour (PPSSPP's interpreter);
 * they catch indexing/wiring bugs in runtime/vfpu.c, not hardware quirks. */
typedef unsigned u32;
typedef int s32;

#define ALIGN16 __attribute__((aligned(16)))
#define PI_F 3.14159265358979323846f

static u32 fbits(float f) { union { float f; u32 u; } v = {f}; return v.u; }
static float bitsf(u32 u) { union { u32 u; float f; } v = {u}; return v.f; }
static u32 hash(const u32 *w, int n) {
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}
static float as_f(u32 x) { return (float)(s32)(x >> 8) / 37.0f; }
static void fill(float *f, u32 a, u32 b, u32 c, u32 d) {
    f[0] = as_f(a); f[1] = as_f(b); f[2] = as_f(c); f[3] = as_f(d);
}

#ifdef __mips__
#define CALL(name, ...) vf_##name(__VA_ARGS__)
void vf_regmap(u32 *out, u32 a, u32 b, u32 c);
void vf_arith(float *out, const float *x);
void vf_prefix(float *out, const float *x);
void vf_cmp(u32 *out, const float *x);
void vf_conv(u32 *out, const float *x, const u32 *ints);
void vf_math(float *out, const float *x, const float *y);
void vf_matrix(float *out, const float *s, const float *t);

/* ---- encoders: "op|fields" as assembler expressions ----
 * XSTR expands macro arguments (like Q) before turning them into text. */
#define STR(x) #x
#define XSTR(x) STR(x)
#define W(x) ".word " x "\n"
#define V3(op, d, s, t, sz) W(XSTR(op|((t)<<16)|((s)<<8)|(d)|(sz)))
#define V2(op, d, s, sz) V3(op, d, s, 0, sz)
#define LVQ(vt, off, rs) W(XSTR(0xD8000000|((rs)<<21)|(((vt)&0x1F)<<16)|(off)|(((vt)>>5)&1)))
#define SVQ(vt, off, rs) W(XSTR(0xF8000000|((rs)<<21)|(((vt)&0x1F)<<16)|(off)|(((vt)>>5)&1)))
#define SVS(vt, off, rs) W(XSTR(0xE8000000|((rs)<<21)|(((vt)&0x1F)<<16)|(off)|(((vt)>>5)&3)))
#define MTV(rt, vd) W(XSTR(0x48E00000|((rt)<<16)|(vd)))
#define MFV(rt, vs) W(XSTR(0x48600000|((rt)<<16)|(vs)))
#define PFX(op, data) W(XSTR(op|(data)))
#define Q 0x8080
#define T 0x8000
#define P 0x80

#define FUNC(name) ".globl " #name "\n.type " #name ", @function\n" #name ":\n"
#define END(name) "    jr $ra\n    nop\n.size " #name ", .-" #name "\n"

__asm__(
    ".text\n.set noreorder\n"

    /* where do single registers land in rows and columns? */
    FUNC(vf_regmap)
    MTV(5, 0) MTV(6, 1) MTV(7, 2) MTV(5, 3)        /* S000 S010 S020 S030 = a b c a */
    MTV(7, 32) MTV(6, 64) MTV(5, 96)               /* S001 S002 S003     = c b a   */
    SVQ(0x20, 0, 4)                                /* R000 -> out[0..3] */
    SVQ(0, 16, 4)                                  /* C000 -> out[4..7] */
    END(vf_regmap)

    FUNC(vf_arith)
    LVQ(0x20, 0, 5)                                /* R000 = x */
    V3(0x64000000, 0x21, 0x20, 0x20, Q)            /* vmul.q R001, R000, R000 */
    V3(0x60000000, 0x22, 0x21, 0x20, Q)            /* vadd.q R002, R001, R000 */
    V3(0x64800000, 12, 0x20, 0x22, Q)              /* vdot.q S300, R000, R002 */
    V3(0x65000000, 0x23, 0x20, 12, Q)              /* vscl.q R003, R000, S300 */
    V3(0x60800000, 0x22, 0x23, 0x21, Q)            /* vsub.q R002, R003, R001 */
    V3(0x63800000, 0x24, 0x22, 0x21, Q)            /* vdiv.q R100, R002, R001 */
    V3(0x66000000, 13, 0x20, 0x21, T)              /* vhdp.t S310, R000, R001 */
    SVQ(0x22, 0, 4) SVQ(0x24, 16, 4) SVS(12, 32, 4) SVS(13, 36, 4)
    END(vf_arith)

    FUNC(vf_prefix)
    LVQ(0x20, 0, 5)                                /* R000 = x */
    PFX(0xDC000000, 0x180DB)                       /* S: (-w, z, y, 1/2) */
    PFX(0xDD000000, 0xFE4)                         /* T: |x y z w| */
    PFX(0xDE000000, 0x404)                         /* D: lane 1 saturates to [0,1], lane 2 masked */
    V3(0x60000000, 0x21, 0x20, 0x20, Q)            /* vadd.q R001, R000, R000 */
    PFX(0xDC000000, 0x4F7B9)                       /* S: constants 1/3, 1/4, -1/6, 2 */
    V2(0xD0000000, 0x22, 0x20, Q)                  /* vmov.q R002, R000 */
    SVQ(0x21, 0, 4) SVQ(0x22, 16, 4)
    END(vf_prefix)

    FUNC(vf_cmp)
    LVQ(0x20, 0, 5)
    PFX(0xDD000000, 0xF000)                        /* T = (0,0,0,0) */
    V3(0x6C000000, 0, 0x20, 0x20, Q|2)             /* vcmp.q LT, R000, 0 */
    MFV(8, 131) "    sw $t0, 0($a0)\n"             /* mfvc t0, CC */
    V3(0x6C000000, 0, 0x20, 0x20, Q|8)             /* vcmp.q EZ, R000 */
    MFV(8, 131) "    sw $t0, 4($a0)\n"
    PFX(0xDD000000, 0xF055)                        /* T = (1,1,1,1) */
    V3(0x6C000000, 0, 0x20, 0x20, Q|7)             /* vcmp.q GT, R000, 1 */
    MFV(8, 131) "    sw $t0, 8($a0)\n"
    "    move $t1, $zero\n"
    W("0x49030000|(4<<18)|2")                      /* bvtl any, +2 */
    "    addiu $t1, $t1, 100\n"                    /* delay slot: only when taken */
    "    addiu $t1, $t1, 1\n"
    "    sw $t1, 12($a0)\n"
    W("0xD2A00000|(0<<19)|(6<<16)|(0x20<<8)|0x21|0x8080")   /* vcmov.q R001, R000, per-lane CC */
    SVQ(0x21, 16, 4)
    END(vf_cmp)

    FUNC(vf_conv)
    LVQ(0x20, 0, 5)
    V2(0xD2230000, 0x21, 0x20, Q)                  /* vf2iz.q R001, R000, 3  (x8, toward zero) */
    V2(0xD2000000, 0x22, 0x20, Q)                  /* vf2in.q R002, R000, 0  (nearest even)    */
    V2(0xD2600000, 0x23, 0x20, Q)                  /* vf2id.q R003, R000, 0  (floor)          */
    SVQ(0x21, 0, 4) SVQ(0x22, 16, 4) SVQ(0x23, 32, 4)
    LVQ(0x24, 0, 6)                                /* R100 = ints */
    V2(0xD2820000, 0x25, 0x24, Q)                  /* vi2f.q R101, R100, 2  (/4) */
    V2(0xD03C0000, 8, 0x24, Q)                     /* vi2uc.q S200, R100 */
    V2(0xD03D0000, 40, 0x24, Q)                    /* vi2c.q  S201, R100 */
    V2(0xD03F0000, 0x26, 0x24, Q)                  /* vi2s.q  R102 (pair), R100 */
    V2(0xD03B0000, 0x27, 0x24, P)                  /* vs2i.p  R103 (quad), R100 */
    SVQ(0x25, 48, 4) SVQ(8, 64, 4) SVQ(0x26, 80, 4) SVQ(0x27, 96, 4)
    END(vf_conv)

    FUNC(vf_math)
    LVQ(0x20, 0, 5) LVQ(0x21, 0, 6)                /* R000 = x, R001 = y */
    V2(0xD0120000, 0x22, 0x20, Q)                  /* vsin.q R002, R000 */
    V2(0xD0130000, 0x23, 0x20, Q)                  /* vcos.q R003, R000 */
    SVQ(0x22, 0, 4) SVQ(0x23, 16, 4)
    V2(0xD0010000, 0x22, 0x20, Q)                  /* vabs.q */
    V2(0xD0110000, 0x22, 0x22, Q)                  /* vrsq.q (of |x|) */
    V2(0xD0020000, 0x23, 0x21, Q)                  /* vneg.q */
    SVQ(0x22, 32, 4) SVQ(0x23, 48, 4)
    V2(0xD0050000, 0x22, 0x20, Q)                  /* vsat1.q */
    V2(0xD0170000, 0x23, 0x22, Q)                  /* vasin.q (of clamped) */
    SVQ(0x22, 64, 4) SVQ(0x23, 80, 4)
    V3(0x6D000000, 0x22, 0x20, 0x21, Q)            /* vmin.q */
    V3(0x6D800000, 0x23, 0x20, 0x21, Q)            /* vmax.q */
    SVQ(0x22, 96, 4) SVQ(0x23, 112, 4)
    V3(0x6F000000, 0x22, 0x20, 0x21, Q)            /* vsge.q */
    V3(0x6F800000, 0x23, 0x20, 0x21, Q)            /* vslt.q */
    SVQ(0x22, 128, 4) SVQ(0x23, 144, 4)
    V2(0xD0440000, 0x22, 0x20, Q)                  /* vocp.q */
    V2(0xD04A0000, 0x23, 0x21, Q)                  /* vsgn.q */
    SVQ(0x22, 160, 4) SVQ(0x23, 176, 4)
    V2(0xD0460000, 8, 0x20, Q)                     /* vfad.q S200 */
    V2(0xD0690000, 0x22, 0, Q)                     /* vcst.q R002, PI */
    W("0xDF000000|(40<<16)|0xFF85")                /* viim.s S201, -123 */
    V2(0xD0030000, 0x23, 0, Q)                     /* vidt.q R003 (row 3 -> lane 3 is 1) */
    SVQ(8, 192, 4) SVQ(0x22, 208, 4) SVQ(0x23, 224, 4)
    V3(0x66800000, 0x22, 0x20, 0x21, T)            /* vcrs.t R002, R000, R001 */
    V3(0xF2800000, 0x23, 0x20, 0x21, T)            /* vcrsp.t R003, R000, R001 */
    V3(0xF2800000, 0x24, 0x20, 0x21, Q)            /* vqmul.q R100, R000, R001 */
    SVQ(0x22, 240, 4) SVQ(0x23, 256, 4) SVQ(0x24, 272, 4)
    END(vf_math)

    FUNC(vf_matrix)
    LVQ(0, 0, 5) LVQ(1, 16, 5) LVQ(2, 32, 5) LVQ(3, 48, 5)       /* M000 = s, by columns */
    LVQ(4, 0, 6) LVQ(5, 16, 6) LVQ(6, 32, 6) LVQ(7, 48, 6)       /* M100 = t */
    V3(0xF0000000, 8, 0, 4, Q)                                   /* vmmul.q M200, M000, M100 */
    SVQ(8, 0, 4) SVQ(9, 16, 4) SVQ(10, 32, 4) SVQ(11, 48, 4)
    V3(0xF0000000, 12, 0x20, 4, Q)                               /* vmmul.q M300, E000, M100 */
    SVQ(12, 64, 4) SVQ(13, 80, 4) SVQ(14, 96, 4) SVQ(15, 112, 4)
    V3(0xF1800000, 8, 0, 4, Q)                                   /* vtfm4.q C200, M000, C100 */
    SVQ(8, 128, 4)
    V3(0xF1800000, 9, 0, 5, T)                                   /* vhtfm4.t C210, M000, C110 */
    SVQ(9, 144, 4)
    V3(0xF2000000, 8, 0, 4, Q)                                   /* vmscl.q M200, M000, S100 */
    SVQ(8, 160, 4) SVQ(9, 176, 4) SVQ(10, 192, 4) SVQ(11, 208, 4)
    W("0xF3830000|0x8080|12")                                    /* vmidt.q M300 */
    SVQ(12, 224, 4) SVQ(13, 240, 4) SVQ(14, 256, 4) SVQ(15, 272, 4)
    W("0xF3A00000|(4<<16)|(4<<8)|12|0x8080")                     /* vrot.q C300, S100, [c, s, 0, 0] */
    SVQ(12, 288, 4)
    END(vf_matrix)

    ".set reorder\n");
#else
#define CALL(name, ...) ref_##name(__VA_ARGS__)
#include <math.h>

/* Separate, non-inlined, like runtime/vfpu.c: otherwise clang fuses sinf+cosf
 * of the same angle into one sincos call that rounds differently. */
__attribute__((noinline)) static float sin_q(float x) { return sinf(x * (PI_F / 2)); }
__attribute__((noinline)) static float cos_q(float x) { return cosf(x * (PI_F / 2)); }

static void ref_regmap(u32 *out, u32 a, u32 b, u32 c) {
    u32 r[8] = {a, b, c, a, a, c, b, a};
    for (int i = 0; i < 8; i++) out[i] = r[i];
}
static float dot4(const float *a, const float *b) {
    float s = 0.f;
    for (int i = 0; i < 4; i++) s += a[i] * b[i];
    return s;
}
static void ref_arith(float *out, const float *x) {
    float m[4], a[4], sc[4], q[4], dv[4];
    for (int i = 0; i < 4; i++) { m[i] = x[i] * x[i]; a[i] = m[i] + x[i]; }
    float d = dot4(x, a);
    for (int i = 0; i < 4; i++) { sc[i] = x[i] * d; q[i] = sc[i] - m[i]; dv[i] = q[i] / m[i]; }
    float h[4] = {x[0], x[1], 1.f, 0.f}, mm[4] = {m[0], m[1], m[2], 0.f};
    float hd = 0.f;
    for (int i = 0; i < 3; i++) hd += h[i] * mm[i];
    for (int i = 0; i < 4; i++) { out[i] = q[i]; out[4 + i] = dv[i]; }
    out[8] = d; out[9] = hd;
}
static void ref_prefix(float *out, const float *x) {
    float s[4] = {-x[3], x[2], x[1], 0.5f};
    for (int i = 0; i < 4; i++) out[i] = s[i] + fabsf(x[i]);
    out[1] = out[1] <= 0.f ? 0.f : out[1] > 1.f ? 1.f : out[1];
    out[2] = 0.f;   /* masked: keeps the register's old value (0) */
    out[4] = 1.f / 3.f; out[5] = 0.25f; out[6] = -1.f / 6.f; out[7] = 2.f;
}
static void ref_cmp(u32 *out, const float *x) {
    u32 lt = 0, ez = 0, gt = 0;
    for (int i = 0; i < 4; i++) { lt |= (u32)(x[i] < 0.f) << i; ez |= (u32)(x[i] == 0.f) << i; gt |= (u32)(x[i] > 1.f) << i; }
    lt |= (lt ? 16u : 0) | (lt == 15 ? 32u : 0);
    ez |= (ez ? 16u : 0) | (ez == 15 ? 32u : 0);
    gt |= (gt ? 16u : 0) | (gt == 15 ? 32u : 0);
    out[0] = lt; out[1] = ez; out[2] = gt;
    out[3] = (gt & 16) ? 100 : 1;
    for (int i = 0; i < 4; i++) out[4 + i] = (gt >> i) & 1 ? fbits(x[i]) : 0;
}
static s32 f2i(float f, int scale, int mode) {
    if (f != f) return 0x7FFFFFFF;
    double v = (double)f * (double)(1u << scale);
    if (v > 2147483647.0) return 0x7FFFFFFF;
    if (v <= -2147483648.0) return (s32)0x80000000u;
    return mode == 0 ? (s32)nearbyint(v) : mode == 1 ? (s32)trunc(v) : (s32)floor(v);
}
static void ref_conv(u32 *out, const float *x, const u32 *in) {
    for (int i = 0; i < 28; i++) out[i] = 0;
    for (int i = 0; i < 4; i++) {
        out[i] = (u32)f2i(x[i], 3, 1);
        out[4 + i] = (u32)f2i(x[i], 0, 0);
        out[8 + i] = (u32)f2i(x[i], 0, 2);
        out[12 + i] = fbits((float)(s32)in[i] * 0.25f);
    }
    u32 uc = 0, ch = 0;
    for (int i = 0; i < 4; i++) {
        s32 v = (s32)in[i] < 0 ? 0 : (s32)in[i];
        uc |= ((u32)(v >> 23) & 0xFF) << (8 * i);
        ch |= (in[i] >> 24) << (8 * i);
    }
    out[16] = uc; out[17] = ch;   /* S200, S201 = lanes 0,1 of C200 */
    out[20] = (in[0] >> 16) | ((in[1] >> 16) << 16);
    out[21] = (in[2] >> 16) | ((in[3] >> 16) << 16);
    out[24] = (in[0] & 0xFFFF) << 16; out[25] = in[0] & 0xFFFF0000u;
    out[26] = (in[1] & 0xFFFF) << 16; out[27] = in[1] & 0xFFFF0000u;
}
static void ref_math(float *out, const float *x, const float *y) {
    for (int i = 0; i < 72; i++) out[i] = 0.f;
    for (int i = 0; i < 4; i++) {
        float c = x[i] < -1.f ? -1.f : x[i] > 1.f ? 1.f : x[i];
        out[i] = sin_q(x[i]);
        out[4 + i] = cos_q(x[i]);
        out[8 + i] = 1.f / sqrtf(fabsf(x[i]));
        out[12 + i] = bitsf(fbits(y[i]) ^ 0x80000000u);
        out[16 + i] = c;
        out[20 + i] = asinf(c) / (PI_F / 2);
        u32 xs = fbits(x[i]), ys = fbits(y[i]);
        s32 kx = (s32)((xs & 0x7FFFFFFF) < 0x800000 ? 0 : xs & 0x7FFFFFFF), ky = (s32)((ys & 0x7FFFFFFF) < 0x800000 ? 0 : ys & 0x7FFFFFFF);
        if (xs >> 31) kx = -kx;
        if (ys >> 31) ky = -ky;
        out[24 + i] = kx < ky ? x[i] : y[i];
        out[28 + i] = kx > ky ? x[i] : y[i];
        out[32 + i] = x[i] >= y[i] ? 1.f : 0.f;
        out[36 + i] = x[i] < y[i] ? 1.f : 0.f;
        out[40 + i] = 1.f - x[i];
        out[44 + i] = (ys & 0x7F800000u) == 0 ? 0.f : (ys >> 31) ? -1.f : 1.f;
        out[52 + i] = PI_F;
        out[56 + i] = i == 3 ? 1.f : 0.f;
    }
    out[48] = ((x[0] + x[1]) + x[2]) + x[3];
    out[49] = -123.f;
    out[60] = x[1] * y[2]; out[61] = x[2] * y[0]; out[62] = x[0] * y[1];
    out[63] = PI_F;   /* 3-lane writes leave lane 3 alone: R002 still holds vcst PI */
    out[67] = 1.f;    /* ... and R003 still holds vidt's 1 */
    out[64] = x[1] * y[2] - x[2] * y[1]; out[65] = x[2] * y[0] - x[0] * y[2]; out[66] = x[0] * y[1] - x[1] * y[0];
    float q0[4] = {y[3], y[2], -y[1], y[0]}, q1[4] = {-y[2], y[3], y[0], y[1]};
    float q2[4] = {y[1], -y[0], y[3], y[2]}, q3[4] = {-y[0], -y[1], -y[2], y[3]};
    out[68] = dot4(x, q0); out[69] = dot4(x, q1); out[70] = dot4(x, q2); out[71] = dot4(x, q3);
}
/* Memory holds matrices by columns: s[4*j + i] = row i of column j. */
static void ref_matrix(float *out, const float *s, const float *t) {
    for (int i = 0; i < 76; i++) out[i] = 0.f;
    for (int a = 0; a < 4; a++)
        for (int b = 0; b < 4; b++) {
            float sum = 0.f, sumt = 0.f;
            for (int k = 0; k < 4; k++) {
                sum += s[4 * b + k] * t[4 * a + k];      /* vmmul: column b of S . column a of T */
                sumt += s[4 * k + b] * t[4 * a + k];     /* with S transposed (E000) */
            }
            out[4 * a + b] = sum;
            out[16 + 4 * a + b] = sumt;
        }
    for (int i = 0; i < 4; i++) {
        float h[4] = {t[4], t[5], t[6], 1.f};
        out[32 + i] = dot4(&s[4 * i], &t[0]);           /* vtfm4: column i of S . t */
        out[36 + i] = dot4(&s[4 * i], h);               /* vhtfm4: w = 1 */
    }
    for (int k = 0; k < 16; k++) out[40 + k] = s[k] * t[0];
    for (int k = 0; k < 16; k++) out[56 + k] = (k % 5 == 0) ? 1.f : 0.f;
    out[72] = cos_q(t[0]); out[73] = sin_q(t[0]);
}
#endif

/* NaN bits differ between "which NaN" paths; tests compare NaN-ness only. */
static void canon(u32 *w, int n) { for (int i = 0; i < n; i++) if ((w[i] & 0x7F800000u) == 0x7F800000u && (w[i] & 0x7FFFFF)) w[i] = 0x7FC00000u; }

u32 t_vregmap(u32 a, u32 b, u32 c, u32 d) {
    ALIGN16 u32 out[8];
    CALL(regmap, out, a, b, c);
    return hash(out, 8) ^ d;
}
u32 t_varith(u32 a, u32 b, u32 c, u32 d) {
    ALIGN16 float x[4], out[10];
    fill(x, a, b, c, d);
    CALL(arith, out, x);
    canon((u32 *)out, 10);
    return hash((u32 *)out, 10);
}
u32 t_vprefix(u32 a, u32 b, u32 c, u32 d) {
    ALIGN16 float x[4], out[8];
    fill(x, a, b, c, d);
    CALL(prefix, out, x);
    canon((u32 *)out, 8);
    return hash((u32 *)out, 8);
}
u32 t_vcmp(u32 a, u32 b, u32 c, u32 d) {
    ALIGN16 float x[4];
    ALIGN16 u32 out[8];
    fill(x, a, b, c, d);
    if (a & 1) x[1] = 0.f;   /* make EZ and "all" cases happen */
    CALL(cmp, out, x);
    return hash(out, 8);
}
u32 t_vconv(u32 a, u32 b, u32 c, u32 d) {
    ALIGN16 float x[4];
    ALIGN16 u32 in[4] = {a, b, c, d}, out[28];
    fill(x, a, b, c, d);
    if (b & 1) x[2] = bitsf(a);   /* raw bits: huge values, NaN, infinity */
    CALL(conv, out, x, in);
    return hash(out, 28);
}
u32 t_vmath(u32 a, u32 b, u32 c, u32 d) {
    ALIGN16 float x[4], y[4], out[72];
    fill(x, a, b, c, d);
    fill(y, d, c, b, a);
    for (int i = 0; i < 4; i++) x[i] *= 0.01f;   /* keep trig arguments moderate */
    CALL(math, out, x, y);
    canon((u32 *)out, 72);
    return hash((u32 *)out, 72);
}
u32 t_vmatrix(u32 a, u32 b, u32 c, u32 d) {
    ALIGN16 float s[16], t[16], out[76];
    for (int i = 0; i < 16; i++) {
        s[i] = as_f(a * (u32)(i + 1) + b);
        t[i] = as_f(c * (u32)(i + 3) ^ d);
    }
    t[0] *= 0.001f;   /* vrot angle */
    CALL(matrix, out, s, t);
    canon((u32 *)out, 76);
    return hash((u32 *)out, 76);
}
