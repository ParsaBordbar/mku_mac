/*
 * MIPS interpreter: the fallback for code the static recompiler never saw.
 *
 * MK loads compiled script overlays (.mko files) into the heap at fight
 * start and calls into them. That code doesn't exist in BOOT.BIN, so it has
 * no recompiled function; rt_call_indirect() hands such addresses to
 * rt_interpret(), which runs them one instruction at a time.
 *
 * - The semantics are the same helpers the recompiled code uses (recomp.h),
 *   and the same differential test suite checks both (tests/recomp, -i mode).
 * - Calls from interpreted code to recompiled functions run natively.
 * - An interpreted function ends when it jumps to the return address it was
 *   called with.
 */
#include "recomp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static inline int32_t s16(uint32_t w) { return (int16_t)(w & 0xFFFF); }

uint64_t rt_interp_count;   /* instructions interpreted (statistics) */

int rt_have_function(uint32_t addr);   /* runtime.c */

static void call(Ctx *c, uint32_t target) {
    if (rt_have_function(target)) rt_call_indirect(c, target);
    else rt_interpret(c, target);
}

static int fpu_op(Ctx *c, uint32_t w) {
    uint32_t fmt = (w >> 21) & 31, ft = (w >> 16) & 31, fs = (w >> 11) & 31, fd = (w >> 6) & 31, fn = w & 63;
    switch (fmt) {
    case 0x00: if (ft) c->r[ft] = c->fi[fs]; return 1;            /* mfc1 */
    case 0x02: if (ft) c->r[ft] = CFC1(c, fs); return 1;          /* cfc1 */
    case 0x04: c->fi[fs] = c->r[ft]; return 1;                    /* mtc1 */
    case 0x06: CTC1(c, fs, c->r[ft]); return 1;                   /* ctc1 */
    case 0x10:
        switch (fn) {
        case 0x00: c->f[fd] = c->f[fs] + c->f[ft]; return 1;
        case 0x01: c->f[fd] = c->f[fs] - c->f[ft]; return 1;
        case 0x02: c->f[fd] = c->f[fs] * c->f[ft]; return 1;
        case 0x03: c->f[fd] = c->f[fs] / c->f[ft]; return 1;
        case 0x04: c->f[fd] = sqrtf(c->f[fs]); return 1;
        case 0x05: c->fi[fd] = c->fi[fs] & 0x7FFFFFFFu; return 1;
        case 0x06: c->fi[fd] = c->fi[fs]; return 1;
        case 0x07: c->fi[fd] = c->fi[fs] ^ 0x80000000u; return 1;
        case 0x0C: c->fi[fd] = CVT_W_S(c->f[fs], 0); return 1;
        case 0x0D: c->fi[fd] = CVT_W_S(c->f[fs], 1); return 1;
        case 0x0E: c->fi[fd] = CVT_W_S(c->f[fs], 2); return 1;
        case 0x0F: c->fi[fd] = CVT_W_S(c->f[fs], 3); return 1;
        case 0x24: c->fi[fd] = CVT_W_S(c->f[fs], c->fcr31); return 1;
        default:
            if (fn >= 0x30) { c->fcc = FCMP(c->f[fs], c->f[ft], fn & 15); return 1; }
        }
        return 0;
    case 0x14:
        if (fn == 0x20) { c->f[fd] = (float)(int32_t)c->fi[fs]; return 1; }
        return 0;
    }
    return 0;
}

/* Execute one non-control-flow instruction. Returns 0 if unknown. */
static int exec_simple(Ctx *c, uint32_t pc, uint32_t w) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    uint32_t a = c->r[rs] + (uint32_t)s16(w);
#define SET(reg_, v) do { uint32_t v_ = (v); if (reg_) c->r[reg_] = v_; } while (0)
    switch (op) {
    case 0x00:
        switch (w & 63) {
        case 0x00: SET(rd, c->r[rt] << sa); return 1;
        case 0x02: SET(rd, rs == 1 ? ROTR(c->r[rt], sa) : c->r[rt] >> sa); return 1;
        case 0x03: SET(rd, (uint32_t)((int32_t)c->r[rt] >> sa)); return 1;
        case 0x04: SET(rd, c->r[rt] << (c->r[rs] & 31)); return 1;
        case 0x06: SET(rd, sa == 1 ? ROTR(c->r[rt], c->r[rs]) : c->r[rt] >> (c->r[rs] & 31)); return 1;
        case 0x07: SET(rd, (uint32_t)((int32_t)c->r[rt] >> (c->r[rs] & 31))); return 1;
        case 0x0A: if (c->r[rt] == 0) SET(rd, c->r[rs]); return 1;
        case 0x0B: if (c->r[rt] != 0) SET(rd, c->r[rs]); return 1;
        case 0x0C: rt_syscall(c, pc, (w >> 6) & 0xFFFFF); return 1;
        case 0x0D: rt_break(c, pc, (w >> 6) & 0xFFFFF); return 1;
        case 0x0F: return 1;
        case 0x10: SET(rd, c->hi); return 1;
        case 0x11: c->hi = c->r[rs]; return 1;
        case 0x12: SET(rd, c->lo); return 1;
        case 0x13: c->lo = c->r[rs]; return 1;
        case 0x16: SET(rd, CLZ(c->r[rs])); return 1;
        case 0x17: SET(rd, CLZ(~c->r[rs])); return 1;
        case 0x18: MULT(c, c->r[rs], c->r[rt]); return 1;
        case 0x19: MULTU(c, c->r[rs], c->r[rt]); return 1;
        case 0x1A: DIV(c, c->r[rs], c->r[rt]); return 1;
        case 0x1B: DIVU(c, c->r[rs], c->r[rt]); return 1;
        case 0x1C: MADD(c, (int64_t)(int32_t)c->r[rs] * (int32_t)c->r[rt]); return 1;
        case 0x1D: MADD(c, (int64_t)((uint64_t)c->r[rs] * c->r[rt])); return 1;
        case 0x2E: MADD(c, -((int64_t)(int32_t)c->r[rs] * (int32_t)c->r[rt])); return 1;
        case 0x2F: MADD(c, (int64_t)-((uint64_t)c->r[rs] * c->r[rt])); return 1;
        case 0x20: case 0x21: SET(rd, c->r[rs] + c->r[rt]); return 1;
        case 0x22: case 0x23: SET(rd, c->r[rs] - c->r[rt]); return 1;
        case 0x24: SET(rd, c->r[rs] & c->r[rt]); return 1;
        case 0x25: SET(rd, c->r[rs] | c->r[rt]); return 1;
        case 0x26: SET(rd, c->r[rs] ^ c->r[rt]); return 1;
        case 0x27: SET(rd, ~(c->r[rs] | c->r[rt])); return 1;
        case 0x2A: SET(rd, (int32_t)c->r[rs] < (int32_t)c->r[rt]); return 1;
        case 0x2B: SET(rd, c->r[rs] < c->r[rt]); return 1;
        case 0x2C: SET(rd, (int32_t)c->r[rs] > (int32_t)c->r[rt] ? c->r[rs] : c->r[rt]); return 1;
        case 0x2D: SET(rd, (int32_t)c->r[rs] < (int32_t)c->r[rt] ? c->r[rs] : c->r[rt]); return 1;
        }
        return 0;
    case 0x08: case 0x09: SET(rt, a); return 1;                                   /* addi(u) */
    case 0x0A: SET(rt, (int32_t)c->r[rs] < s16(w)); return 1;                      /* slti */
    case 0x0B: SET(rt, c->r[rs] < (uint32_t)s16(w)); return 1;                     /* sltiu */
    case 0x0C: SET(rt, c->r[rs] & (w & 0xFFFF)); return 1;
    case 0x0D: SET(rt, c->r[rs] | (w & 0xFFFF)); return 1;
    case 0x0E: SET(rt, c->r[rs] ^ (w & 0xFFFF)); return 1;
    case 0x0F: SET(rt, w << 16); return 1;                                         /* lui */
    case 0x11: return fpu_op(c, w);
    case 0x1F:
        switch (w & 63) {
        case 0x00: SET(rt, EXT(c->r[rs], sa, rd + 1)); return 1;
        case 0x04: SET(rt, INS(c->r[rt], c->r[rs], sa, rd - sa + 1)); return 1;
        case 0x20:
            switch (sa) {
            case 0x02: SET(rd, WSBH(c->r[rt])); return 1;
            case 0x03: SET(rd, __builtin_bswap32(c->r[rt])); return 1;
            case 0x10: SET(rd, (uint32_t)(int32_t)(int8_t)c->r[rt]); return 1;
            case 0x14: SET(rd, BITREV(c->r[rt])); return 1;
            case 0x18: SET(rd, (uint32_t)(int32_t)(int16_t)c->r[rt]); return 1;
            }
        }
        return 0;
    case 0x20: SET(rt, MEM_LB(a)); return 1;
    case 0x21: SET(rt, MEM_LH(a)); return 1;
    case 0x22: SET(rt, MEM_LWL(c->r[rt], a)); return 1;
    case 0x23: case 0x30: SET(rt, MEM_LW(a)); return 1;
    case 0x24: SET(rt, MEM_LBU(a)); return 1;
    case 0x25: SET(rt, MEM_LHU(a)); return 1;
    case 0x26: SET(rt, MEM_LWR(c->r[rt], a)); return 1;
    case 0x28: MEM_SB(a, c->r[rt]); return 1;
    case 0x29: MEM_SH(a, c->r[rt]); return 1;
    case 0x2A: MEM_SWL(a, c->r[rt]); return 1;
    case 0x2B: MEM_SW(a, c->r[rt]); return 1;
    case 0x2E: MEM_SWR(a, c->r[rt]); return 1;
    case 0x38: MEM_SW(a, c->r[rt]); SET(rt, 1); return 1;                          /* sc */
    case 0x2F: return 1;                                                           /* cache */
    case 0x31: c->fi[rt] = MEM_LW(a); return 1;                                    /* lwc1 */
    case 0x39: MEM_SW(a, c->fi[rt]); return 1;                                     /* swc1 */
    case 0x12: case 0x18: case 0x19: case 0x1B: case 0x32: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        vfpu_exec(c, pc, w);                                                       /* VFPU */
        return 1;
    }
    return 0;
#undef SET
}

/* Branch condition for conditional branches; *likely set for branch-likely. */
static int branch_taken(Ctx *c, uint32_t w, int *likely, int *link) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    int32_t s = (int32_t)c->r[rs];
    *likely = 0;
    *link = 0;
    switch (op) {
    case 0x04: return c->r[rs] == c->r[rt];
    case 0x05: return c->r[rs] != c->r[rt];
    case 0x06: return s <= 0;
    case 0x07: return s > 0;
    case 0x14: *likely = 1; return c->r[rs] == c->r[rt];
    case 0x15: *likely = 1; return c->r[rs] != c->r[rt];
    case 0x16: *likely = 1; return s <= 0;
    case 0x17: *likely = 1; return s > 0;
    case 0x01:
        *likely = (rt & 2) != 0;
        *link = (rt & 0x10) != 0;
        return (rt & 1) ? s >= 0 : s < 0;
    case 0x11:   /* bc1f/bc1t(l) */
        *likely = (rt & 2) != 0;
        return (rt & 1) ? c->fcc : !c->fcc;
    case 0x12: {   /* bvf/bvt(l) */
        int bit = (c->vctrl[3] >> ((rt >> 2) & 7)) & 1;
        *likely = (rt & 2) != 0;
        return (rt & 1) ? bit : !bit;
    }
    }
    return -1;
}

static int is_branch(uint32_t w) {
    uint32_t op = w >> 26, rs = (w >> 21) & 31;
    return op == 0x04 || op == 0x05 || op == 0x06 || op == 0x07 || (op >= 0x14 && op <= 0x17) || op == 0x01 ||
           (op == 0x11 && rs == 8) || (op == 0x12 && rs == 8);
}

void rt_interpret(Ctx *c, uint32_t pc) {
    static int announced;
    if (!announced++) fprintf(stderr, "[interp] running code outside the executable at 0x%08X (loaded or generated at runtime)\n", pc);
    uint32_t ret = c->r[31];   /* we're done when control comes back here */
    for (;;) {
        uint32_t w = MEM_LW(pc), op = w >> 26;
        rt_interp_count++;
        uint32_t next = pc + 4, target = 0;
        int kind = 0;   /* 0 plain, 1 jump to target, 2 call target, 3 jump reg, 4 call reg */
        uint32_t link_reg = 31, reg_target = 0;

        if (op == 0x02 || op == 0x03) {
            target = (pc & 0xF0000000u) | ((w & 0x03FFFFFF) << 2);
            kind = op == 0x02 ? 1 : 2;
        } else if (op == 0x00 && (w & 63) == 0x08) {
            reg_target = c->r[(w >> 21) & 31];
            kind = 3;
        } else if (op == 0x00 && (w & 63) == 0x09) {
            reg_target = c->r[(w >> 21) & 31];
            link_reg = (w >> 11) & 31;
            kind = 4;
        } else if (is_branch(w)) {
            int likely, link, taken = branch_taken(c, w, &likely, &link);
            uint32_t btarget = pc + 4 + ((uint32_t)s16(w) << 2);
            if (link) c->r[31] = pc + 8;
            if (likely && !taken) { pc += 8; continue; }   /* delay slot skipped */
            uint32_t dw = MEM_LW(pc + 4);
            if (!exec_simple(c, pc + 4, dw)) rt_unimplemented(c, pc + 4, dw, "interp delay slot");
            if (taken) {
                if (link) { call(c, btarget); pc += 8; }
                else pc = btarget;
            } else {
                pc += 8;
            }
            continue;
        } else {
            if (!exec_simple(c, pc, w)) rt_unimplemented(c, pc, w, "interp");
            pc = next;
            continue;
        }

        /* jumps and calls: link first, then the delay slot, then go */
        if (kind == 2) c->r[31] = pc + 8;
        if (kind == 4 && link_reg) c->r[link_reg] = pc + 8;
        uint32_t dw = MEM_LW(pc + 4);
        if (!exec_simple(c, pc + 4, dw)) rt_unimplemented(c, pc + 4, dw, "interp delay slot");
        switch (kind) {
        case 1: pc = target; break;
        case 2: call(c, target); pc += 8; break;
        case 3:
            if (reg_target == ret) return;   /* jr $ra: our function returns */
            if (rt_have_function(reg_target)) { rt_call_indirect(c, reg_target); return; }   /* tail call */
            pc = reg_target;
            break;
        case 4: call(c, reg_target); pc += 8; break;
        }
    }
}
