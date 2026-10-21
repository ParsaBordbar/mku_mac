/* Division and remainder. Divisors are forced non-zero (C would be undefined). */
typedef unsigned u32;
typedef int s32;

u32 t_divu(u32 a, u32 b, u32 c, u32 d) { u32 y = b | 1; return a / y + (c % y) * 3 + d / 10u; }
u32 t_divs(u32 a, u32 b, u32 c, u32 d) {
    s32 x = (s32)(a & 0x7FFFFFFF) - 0x40000000, y = (s32)(b | 1) >> 3;
    if (y == 0) y = -7;
    return (u32)(x / y) ^ (u32)(x % y) ^ (u32)((s32)c / 7) ^ (u32)((s32)d % -13);
}
