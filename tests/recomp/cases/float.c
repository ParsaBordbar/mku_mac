/* Single-precision FPU: arithmetic, comparisons (incl. NaN), conversions,
 * register moves, float arguments and float memory traffic. */
typedef unsigned u32;
typedef int s32;

static float bits_to_f(u32 x) { union { u32 u; float f; } v = {x}; return v.f; }
static u32 f_to_bits(float f) { union { float f; u32 u; } v = {f}; return v.u; }
/* For arithmetic results: any NaN counts as the same NaN. Which input's NaN
 * "payload" survives depends on operand order and on the CPU (MIPS and ARM
 * even disagree on which bit marks a quiet NaN), so only NaN-ness is tested.
 * Moves (t_fbits) and sign flips still compare exact bits. */
static u32 arith_bits(float f) { return f != f ? 0x7FC00000u : f_to_bits(f); }

/* Map a test input to a float. Most inputs become "normal" game-like numbers;
 * some pass through as raw bits so NaN, infinity, -0.0 and denormals show up. */
static float as_float(u32 x) {
    switch (x & 7) {
    case 0: return bits_to_f(x);                       /* raw bits: anything goes */
    case 1: return bits_to_f(x & 0x807FFFFFu);         /* denormal or zero */
    case 2: return bits_to_f(0x7FC00000u | (x >> 9)); /* NaN */
    case 3: return (x & 8) ? -1.0f / 0.0f : 1.0f / 0.0f;
    default: return (float)(s32)(x >> 8) / 37.0f;      /* ordinary numbers */
    }
}

u32 t_farith(u32 a, u32 b, u32 c, u32 d) {
    float x = as_float(a), y = as_float(b), z = as_float(c), w = as_float(d);
    return arith_bits(x * y + z) ^ arith_bits((x - w) / (y + 1.5f)) ^ f_to_bits(-z);
}
u32 t_fcompare(u32 a, u32 b, u32 c, u32 d) {
    float x = as_float(a), y = as_float(b);
    return (x < y) | (x <= y) << 1 | (x == y) << 2 | (x > y) << 3 | (x >= y) << 4 |
           (x != y) << 5 | !(x < y) << 6 | (x != x) << 7;
}
u32 t_fconvert(u32 a, u32 b, u32 c, u32 d) {
    float x = (float)(s32)a;                  /* cvt.s.w */
    float y = as_float(b);
    s32 i = (y > -2.0e9f && y < 2.0e9f) ? (s32)y : 12345;   /* trunc.w.s, in range only */
    return f_to_bits(x) ^ (u32)i ^ (u32)(s32)(as_float(c) > 0.5f ? 1.0f : 2.0f);
}
u32 t_fmath(u32 a, u32 b, u32 c, u32 d) {
    float x = as_float(a);
    return arith_bits(__builtin_sqrtf(__builtin_fabsf(x))) ^ f_to_bits(__builtin_fabsf(as_float(b)));
}
__attribute__((noinline)) static float mix(float p, float q, float r) { return p * 0.25f + q * r; }
u32 t_fcalls(u32 a, u32 b, u32 c, u32 d) {
    volatile float arr[8];
    for (u32 i = 0; i < 8; i++) arr[i] = as_float(a + i * b);
    float acc = 0.0f;
    for (u32 i = 0; i < 8; i++) acc = mix(acc, arr[i], as_float(c));
    return arith_bits(acc);
}
u32 t_fbits(u32 a, u32 b, u32 c, u32 d) {
    float x = bits_to_f(a);                   /* mtc1 */
    return f_to_bits(x) + arith_bits(bits_to_f(b) * 1.0f);   /* mfc1 */
}
