/* Integer arithmetic, logic, shifts, comparisons. */
typedef unsigned u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;

u32 t_add_sub(u32 a, u32 b, u32 c, u32 d) { return a + b - c + (d << 3); }
u32 t_logic(u32 a, u32 b, u32 c, u32 d) { return (a & b) | (c ^ d) | ~(a | d); }
u32 t_shifts(u32 a, u32 b, u32 c, u32 d) {
    return (a << (b & 31)) ^ (c >> (d & 31)) ^ (u32)((s32)a >> (c & 31)) ^ (a >> 7) ^ (u32)((s32)b >> 13);
}
u32 t_compare(u32 a, u32 b, u32 c, u32 d) {
    return ((s32)a < (s32)b) | ((a < b) << 1) | (((s32)c < 100) << 2) | ((d < 0xFFFFFFF0u) << 3) |
           ((a == c) << 4) | (((s32)d >= 0) << 5);
}
u32 t_mul(u32 a, u32 b, u32 c, u32 d) { return a * b + c * 12345u - d * d; }
u32 t_mul_high(u32 a, u32 b, u32 c, u32 d) {
    u64 p = (u64)a * b;
    s64 q = (s64)(s32)c * (s32)d;
    return (u32)(p >> 32) ^ (u32)((u64)q >> 32) ^ (u32)q;
}
u32 t_constants(u32 a, u32 b, u32 c, u32 d) {
    return (a + 0x12345678u) ^ (b | 0xFFFF0000u) ^ (c & 0x00FF00FFu) ^ (d - 70000u);
}
u32 t_signext(u32 a, u32 b, u32 c, u32 d) {
    return (u32)(s32)(signed char)a + (u32)(s32)(short)b + (unsigned char)c + (unsigned short)d;
}
