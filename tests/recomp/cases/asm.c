/* Hand-written MIPS for patterns clang never generates but the game does:
 * delay slots that modify the branch's own register, branch-likely,
 * jal whose delay slot reads $ra, and PSP-only instructions.
 *
 * On MIPS the functions come from the assembly below; on the host the C
 * reference versions underneath are compiled instead. */
typedef unsigned u32;
typedef int s32;

#ifdef __mips__
__asm__(
    ".text\n"
    ".set noreorder\n"          /* we place delay slots ourselves */

    /* loop: the delay slot decrements the register the branch just tested */
    ".globl t_delay_cond\n.type t_delay_cond, @function\n"
    "t_delay_cond:\n"
    "    andi  $a0, $a0, 0xff\n"
    "    move  $v0, $zero\n"
    "1:  addiu $v0, $v0, 3\n"
    "    bnez  $a0, 1b\n"
    "    addiu $a0, $a0, -1\n"
    "    jr    $ra\n"
    "    addu  $v0, $v0, $a1\n"
    ".size t_delay_cond, .-t_delay_cond\n"

    /* branch-likely: the delay slot only runs when the branch is taken */
    ".globl t_likely\n.type t_likely, @function\n"
    "t_likely:\n"
    "    move  $v0, $zero\n"
    "    andi  $a0, $a0, 1\n"
    "    beql  $a0, $zero, 1f\n"
    "    addiu $v0, $v0, 100\n"
    "    addiu $v0, $v0, 1\n"
    "1:  bnel  $a2, $zero, 2f\n"
    "    addu  $v0, $v0, $a2\n"
    "    addiu $v0, $v0, 7\n"
    "2:  jr    $ra\n"
    "    addu  $v0, $v0, $a1\n"
    ".size t_likely, .-t_likely\n"

    /* jal writes $ra before its delay slot runs */
    ".globl t_jal_ra\n.type t_jal_ra, @function\n"
    "t_jal_ra:\n"
    "    addiu $sp, $sp, -8\n"
    "    sw    $ra, 4($sp)\n"
    "    jal   ra_helper\n"
    "    move  $a0, $ra\n"           /* must see the NEW $ra */
    "    lw    $ra, 4($sp)\n"
    "    jr    $ra\n"
    "    addiu $sp, $sp, 8\n"
    ".size t_jal_ra, .-t_jal_ra\n"
    ".type ra_helper, @function\n"
    "ra_helper:\n"
    "    jr    $ra\n"
    "    subu  $v0, $a0, $ra\n"       /* 0 if the delay slot saw the new $ra */
    ".size ra_helper, .-ra_helper\n"

    /* PSP-only: max, min, clz (encoded by hand -- no assembler knows them) */
    ".globl t_allegrex\n.type t_allegrex, @function\n"
    "t_allegrex:\n"
    "    .word 0x0085102C\n"          /* max  $v0, $a0, $a1 */
    "    .word 0x00C7182D\n"          /* min  $v1, $a2, $a3 */
    "    .word 0x00804016\n"          /* clz  $t0, $a0 */
    "    addu  $v0, $v0, $v1\n"
    "    jr    $ra\n"
    "    addu  $v0, $v0, $t0\n"
    ".size t_allegrex, .-t_allegrex\n"

    /* MIPS32r2 instructions the PSP has */
    ".globl t_r2\n.type t_r2, @function\n"
    "t_r2:\n"
    ".set push\n.set mips32r2\n"
    "    ext   $v0, $a0, 3, 9\n"
    "    ins   $a1, $a2, 4, 12\n"
    "    seb   $t0, $a3\n"
    "    seh   $t1, $a3\n"
    "    wsbh  $t2, $a0\n"
    "    rotr  $t3, $a1, 7\n"
    ".set pop\n"
    "    xor   $v0, $v0, $a1\n"
    "    xor   $v0, $v0, $t0\n"
    "    xor   $v0, $v0, $t1\n"
    "    xor   $v0, $v0, $t2\n"
    "    jr    $ra\n"
    "    xor   $v0, $v0, $t3\n"
    ".size t_r2, .-t_r2\n"

    ".set reorder\n");
#else
u32 t_delay_cond(u32 a, u32 b, u32 c, u32 d) { return 3 * ((a & 0xff) + 1) + b; }
u32 t_likely(u32 a, u32 b, u32 c, u32 d) {
    u32 v = (a & 1) == 0 ? 100 : 1;
    v = c != 0 ? v + c : v + 7;
    return v + b;
}
u32 t_jal_ra(u32 a, u32 b, u32 c, u32 d) { return 0; }
u32 t_allegrex(u32 a, u32 b, u32 c, u32 d) {
    s32 mx = (s32)a > (s32)b ? (s32)a : (s32)b;
    s32 mn = (s32)c < (s32)d ? (s32)c : (s32)d;
    return (u32)mx + (u32)mn + (a ? (u32)__builtin_clz(a) : 32);
}
u32 t_r2(u32 a, u32 b, u32 c, u32 d) {
    u32 ext = (a >> 3) & 0x1FF;
    u32 ins = (b & ~(0xFFFu << 4)) | ((c & 0xFFF) << 4);
    u32 seb = (u32)(s32)(signed char)d, seh = (u32)(s32)(short)d;
    u32 wsbh = ((a & 0x00FF00FF) << 8) | ((a >> 8) & 0x00FF00FF);
    u32 rotr = (ins >> 7) | (ins << 25);
    return ext ^ ins ^ seb ^ seh ^ wsbh ^ rotr;
}
#endif
