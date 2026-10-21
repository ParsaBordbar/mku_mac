/* Calls: recursion, function pointers (jalr), more than 4 arguments (stack). */
typedef unsigned u32;

__attribute__((noinline)) static u32 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
__attribute__((noinline)) static u32 sum8(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}
__attribute__((noinline)) static u32 op_add(u32 x, u32 y) { return x + y; }
__attribute__((noinline)) static u32 op_xor(u32 x, u32 y) { return x ^ y; }
__attribute__((noinline)) static u32 op_mul(u32 x, u32 y) { return x * y; }
static u32 (*const g_ops[3])(u32, u32) = {op_add, op_xor, op_mul};

u32 t_fib(u32 a, u32 b, u32 c, u32 d) { return fib(a & 15) + b + c + d; }
u32 t_many_args(u32 a, u32 b, u32 c, u32 d) { return sum8(a, b, c, d, a ^ b, c ^ d, a + d, b - c); }
u32 t_fnptr(u32 a, u32 b, u32 c, u32 d) {
    u32 (*volatile f)(u32, u32) = g_ops[a % 3];
    return f(b, c) + g_ops[d % 3](c, d);
}
