/* Loops, branches, switches (compare chains), early returns. */
typedef unsigned u32;
typedef int s32;

u32 t_loop_sum(u32 a, u32 b, u32 c, u32 d) {
    u32 n = a & 255, s = 0;
    for (u32 i = 0; i < n; i++) s += i * (b | 1) + (c ^ i);
    return s + d;
}
u32 t_while_bits(u32 a, u32 b, u32 c, u32 d) {
    u32 n = 0;
    while (a) { n += a & 1; a >>= 1; }       /* popcount */
    do { n += 3; b >>= 4; } while (b);
    return n ^ c ^ d;
}
u32 t_branches(u32 a, u32 b, u32 c, u32 d) {
    if ((s32)a < 0) return b;
    if (a > 1000) {
        if (c & 1) return c + d;
        return c - d;
    }
    if (a == 7 || b == 7) return 77;
    return (s32)d > (s32)c ? d : c;
}
u32 t_switch(u32 a, u32 b, u32 c, u32 d) {
    u32 r = 0;
    switch (a & 15) {
    case 0: r = b; break;
    case 1: r = c; break;
    case 2: r = b + c; /* fall through */
    case 3: r += d; break;
    case 7: r = b * c; break;
    case 9: return 999;
    case 12: r = d ^ b; break;
    default: r = a;
    }
    return r + 1;
}
u32 t_nested_loops(u32 a, u32 b, u32 c, u32 d) {
    u32 s = 0;
    for (u32 i = 0; i < (a & 15); i++)
        for (u32 j = 0; j < (b & 15); j++) {
            if (((i + j) & 3) == 3) continue;
            if (i * j > (c & 63)) break;
            s += i ^ j ^ d;
        }
    return s;
}
