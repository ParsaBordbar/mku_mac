/* Loads/stores of every width, stack arrays, globals, unaligned access. */
typedef unsigned u32;
typedef unsigned short u16;
typedef unsigned char u8;

static u32 g_table[16] = {1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987};
static u8 g_bytes[64];
static u32 g_counter;

u32 t_stack_array(u32 a, u32 b, u32 c, u32 d) {
    volatile u32 arr[32];
    for (u32 i = 0; i < 32; i++) arr[i] = a * i + b;
    u32 s = 0;
    for (u32 i = 0; i < 32; i += (c & 3) + 1) s += arr[i];
    return s ^ d;
}
u32 t_globals(u32 a, u32 b, u32 c, u32 d) {
    g_counter += a;              /* persists across calls -- same on both sides */
    g_table[b & 15] ^= c;
    return g_table[d & 15] + g_counter;
}
u32 t_bytes(u32 a, u32 b, u32 c, u32 d) {
    for (u32 i = 0; i < 64; i++) g_bytes[i] = (u8)(a + i * b);
    volatile signed char *sb = (volatile signed char *)g_bytes;
    volatile short *sh = (volatile short *)g_bytes;
    volatile u16 *uh = (volatile u16 *)g_bytes;
    return (u32)sb[c & 63] + (u32)sh[d & 31] + uh[(c >> 3) & 31] + g_bytes[(d >> 5) & 63];
}
struct __attribute__((packed)) Packed { u8 tag; u32 value; u16 half; };
u32 t_unaligned(u32 a, u32 b, u32 c, u32 d) {
    volatile struct Packed p[4];
    for (u32 i = 0; i < 4; i++) { p[i].tag = (u8)i; p[i].value = a + i * b; p[i].half = (u16)(c + i); }
    return p[d & 3].value ^ p[(d >> 2) & 3].half ^ p[1].tag;
}
u32 t_struct_copy(u32 a, u32 b, u32 c, u32 d) {
    struct S { u32 x[12]; } s1, s2;
    for (u32 i = 0; i < 12; i++) s1.x[i] = a ^ (i * c);
    s2 = s1;                     /* likely a memcpy call or unrolled copy */
    s2.x[b % 12] = d;
    u32 r = 0;
    for (u32 i = 0; i < 12; i++) r = r * 31 + s2.x[i];
    return r;
}
