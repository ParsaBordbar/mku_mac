"""Lesson 03: decoder for the PSP CPU ("Allegrex").

Allegrex = MIPS II (32-bit, little-endian) + a few MIPS32r2 instructions
(ext/ins/seb/seh/wsbh/rotr) + PSP-only ones (min/max, clz/clo and madd/msub in
the SPECIAL table instead of SPECIAL2) + FPU (single precision only) + VFPU.

Every MIPS instruction is 32 bits, in one of three layouts:

    R-type  | op:6 | rs:5 | rt:5 | rd:5 | sa:5 | funct:6 |   register ops
    I-type  | op:6 | rs:5 | rt:5 |        imm:16         |   immediates, loads, branches
    J-type  | op:6 |            target:26               |   j / jal

`decode()` returns a structured Insn that the recompiler (phase 5) will use;
`Insn.text()` prints it in llvm-objdump's style so we can diff against LLVM.
VFPU instructions are recognized but not decoded yet (phase 6).
"""

from dataclasses import dataclass, field

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

# Instruction kinds the recompiler cares about.
BRANCH = "branch"      # conditional, PC-relative, has a delay slot
JUMP = "jump"          # j / jr: unconditional, has a delay slot
CALL = "call"          # jal / jalr / bal: sets $ra, has a delay slot
OTHER = "other"


@dataclass
class Insn:
    addr: int
    word: int
    op: str                       # canonical mnemonic, e.g. "addiu"
    args: list = field(default_factory=list)   # ("r", 4) gpr, ("f", 2) fpr, ("i", -8) imm,
                                               # ("m", base, off) memory, ("t", addr) target
    kind: str = OTHER
    likely: bool = False          # "branch likely": delay slot only runs if taken
    valid: bool = True

    @property
    def has_delay_slot(self) -> bool:
        return self.kind in (BRANCH, JUMP, CALL)

    def target(self):
        for a in self.args:
            if a[0] == "t":
                return a[1]
        return None

    def text(self, llvm: bool = False) -> str:
        """Human-friendly text with LLVM's aliases (move, nop, b, beqz, ...).

        llvm=True prints exactly like llvm-objdump (numeric registers, hex
        immediates) so check_disasm.py can compare meaning, not style.
        """
        op, args = _alias(self)
        if not args:
            return op
        return f"{op}\t" + ", ".join(_fmt(a, llvm) for a in args)


# Registers llvm-objdump prints by name; all others as $N.
_LLVM_NAMED = {0: "zero", 28: "gp", 29: "sp", 30: "fp", 31: "ra"}


def _reg(n: int, llvm: bool) -> str:
    if llvm:
        return "$" + _LLVM_NAMED.get(n, str(n))
    return "$" + GPR[n]


def _hex(v: int) -> str:
    return f"-{-v:#x}" if v < 0 else f"{v:#x}"


def _fmt(a, llvm: bool = False) -> str:
    kind = a[0]
    if kind == "r":
        return _reg(a[1], llvm)
    if kind == "f":
        return f"$f{a[1]}"
    if kind == "c":            # coprocessor control register / raw register number
        return f"${a[1]}"
    if kind == "i":
        return _hex(a[1]) if llvm else str(a[1])
    if kind == "x":            # unsigned immediate printed in hex (lui/andi/ori/xori)
        return hex(a[1])
    if kind == "m":
        off = _hex(a[2]) if llvm else str(a[2])
        return f"{off}({_reg(a[1], llvm)})"
    if kind == "t":
        return hex(a[1])
    return str(a[1])


def _alias(i: Insn):
    """Map canonical instructions to the pseudo-instructions LLVM prints."""
    op, a = i.op, i.args
    if op == "sll" and i.word == 0:
        return "nop", []
    if op in ("addu", "or") and a[2] == ("r", 0):
        return "move", a[:2]
    if op == "beq" and a[0] == a[1] == ("r", 0):
        return "b", a[2:]
    if op in ("beq", "bne", "beql", "bnel") and a[1] == ("r", 0):
        return op[:3] + "z" + op[3:], [a[0], a[2]]      # beqz, bnez, beqzl, bnezl
    if op in ("div", "divu"):
        return op, [("r", 0)] + a                       # LLVM: div $zero, rs, rt
    if op == "break" and a == [("x", 0), ("x", 0)]:
        return op, []
    if op == "subu" and a[1] == ("r", 0):
        return "negu", [a[0], a[2]]
    if op == "nor" and a[2] == ("r", 0):
        return "not", a[:2]
    if op == "bgezal" and a[0] == ("r", 0):
        return "bal", a[1:]
    if op == "jalr" and a[0] == ("r", 31):
        return "jalr", a[1:]
    return op, a


def _sext16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


# ---------------------------------------------------------------------------

_LOADSTORE = {
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
    0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
    0x30: "ll", 0x38: "sc",
}
_ALU_IMM = {0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu",
            0x0C: "andi", 0x0D: "ori", 0x0E: "xori"}
_BRANCH2 = {0x04: ("beq", False), 0x05: ("bne", False), 0x14: ("beql", True), 0x15: ("bnel", True)}
_BRANCH1 = {0x06: ("blez", False), 0x07: ("bgtz", False), 0x16: ("blezl", True), 0x17: ("bgtzl", True)}
_REGIMM = {0x00: ("bltz", False), 0x01: ("bgez", False), 0x02: ("bltzl", True), 0x03: ("bgezl", True),
           0x10: ("bltzal", False), 0x11: ("bgezal", False), 0x12: ("bltzall", True), 0x13: ("bgezall", True)}
_SPECIAL_3REG = {0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or",
                 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
                 0x0A: "movz", 0x0B: "movn", 0x2C: "max", 0x2D: "min"}
_SPECIAL_MULDIV = {0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
                   0x1C: "madd", 0x1D: "maddu", 0x2E: "msub", 0x2F: "msubu"}
_FPU_S = {0x00: "add.s", 0x01: "sub.s", 0x02: "mul.s", 0x03: "div.s"}
_FPU_S_UNARY = {0x04: "sqrt.s", 0x05: "abs.s", 0x06: "mov.s", 0x07: "neg.s",
                0x0C: "round.w.s", 0x0D: "trunc.w.s", 0x0E: "ceil.w.s", 0x0F: "floor.w.s",
                0x24: "cvt.w.s"}
_FPU_COND = ["f", "un", "eq", "ueq", "olt", "ult", "ole", "ule",
             "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"]
VFPU_OPCODES = {0x12, 0x18, 0x19, 0x1B, 0x32, 0x34, 0x35, 0x36, 0x37, 0x3A, 0x3C, 0x3D, 0x3E, 0x3F}
_VFPU_OPS = {0x12: "cop2", 0x18: "vfpu0", 0x19: "vfpu1", 0x1B: "vfpu3", 0x32: "lv.s",
             0x34: "vfpu4", 0x35: "lvl/lvr.q", 0x36: "lv.q", 0x37: "vfpu6", 0x3A: "sv.s",
             0x3C: "vfpu5", 0x3D: "svl/svr.q", 0x3E: "sv.q", 0x3F: "vfpu7"}


def decode(word: int, addr: int) -> Insn:
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    sa = (word >> 6) & 31
    funct = word & 63
    imm = word & 0xFFFF
    simm = _sext16(imm)
    branch_target = (addr + 4 + (simm << 2)) & 0xFFFFFFFF
    R = lambda n: ("r", n)

    if op == 0x00:
        return _special(word, addr, rs, rt, rd, sa, funct)
    if op == 0x01 and rt in _REGIMM:
        name, likely = _REGIMM[rt]
        kind = CALL if rt & 0x10 else BRANCH
        return Insn(addr, word, name, [R(rs), ("t", branch_target)], kind, likely)
    if op in (0x02, 0x03):
        target = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        return Insn(addr, word, "j" if op == 2 else "jal", [("t", target)], JUMP if op == 2 else CALL)
    if op in _BRANCH2:
        name, likely = _BRANCH2[op]
        return Insn(addr, word, name, [R(rs), R(rt), ("t", branch_target)], BRANCH, likely)
    if op in _BRANCH1 and rt == 0:
        name, likely = _BRANCH1[op]
        return Insn(addr, word, name, [R(rs), ("t", branch_target)], BRANCH, likely)
    if op in _ALU_IMM:
        name = _ALU_IMM[op]
        val = ("x", imm) if op >= 0x0C else ("i", simm)   # logical ops zero-extend
        return Insn(addr, word, name, [R(rt), R(rs), val])
    if op == 0x0F:
        return Insn(addr, word, "lui", [R(rt), ("x", imm)])
    if op in _LOADSTORE:
        return Insn(addr, word, _LOADSTORE[op], [R(rt), ("m", rs, simm)])
    if op == 0x31:
        return Insn(addr, word, "lwc1", [("f", rt), ("m", rs, simm)])
    if op == 0x39:
        return Insn(addr, word, "swc1", [("f", rt), ("m", rs, simm)])
    if op == 0x2F:
        return Insn(addr, word, "cache", [("i", rt), ("m", rs, simm)])
    if op == 0x11:
        return _cop1(word, addr, rs, rt, rd, sa, funct, branch_target)
    if op == 0x1F:
        return _special3(word, addr, rs, rt, rd, sa, funct)
    if op == 0x12 and rs == 8:   # bvf/bvt/bvfl/bvtl: branch on a VFPU condition bit
        name, likely = [("bvf", False), ("bvt", False), ("bvfl", True), ("bvtl", True)][rt & 3]
        return Insn(addr, word, name, [("i", (rt >> 2) & 7), ("t", branch_target)], BRANCH, likely)
    if op in _VFPU_OPS:
        return Insn(addr, word, vfpu_name(word), [("x", word)], OTHER)
    return Insn(addr, word, "invalid", [("x", word)], valid=False)


# ---------------------------------------------------------------------------
# VFPU names. Field positions follow PPSSPP's Core/MIPS/MIPSTables.cpp.
# Operands are executed by the runtime from the raw word, so only the name
# (with its .s/.p/.t/.q size suffix) is decoded here.

_V0 = ["vadd", "vsub", "vsbn", None, None, None, None, "vdiv"]
_V1 = ["vmul", "vdot", "vscl", None, "vhdp", "vcrs", "vdet", None]
_V3 = ["vcmp", None, "vmin", "vmax", None, "vscmp", "vsge", "vslt"]
_V4 = ["vmov", "vabs", "vneg", "vidt", "vsat0", "vsat1", "vzero", "vone"] + [None] * 8 + \
      ["vrcp", "vrsq", "vsin", "vcos", "vexp2", "vlog2", "vsqrt", "vasin",
       "vnrcp", None, "vnsin", None, "vrexp2", None, None, None]
_V7 = ["vrnds", "vrndi", "vrndf1", "vrndf2"] + [None] * 14 + ["vf2h", "vh2f", None, None, "vsbz", "vlgb",
       "vuc2ifs", "vc2i", "vus2i", "vs2i", "vi2uc", "vi2c", "vi2us", "vi2s"]
_V9 = ["vsrt1", "vsrt2", "vbfy1", "vbfy2", "vocp", "vsocp", "vfad", "vavg", "vsrt3", "vsrt4", "vsgn"] + \
      [None] * 5 + ["vmfvc", "vmtvc"] + [None] * 7 + ["vt4444", "vt5551", "vt5650"] + [None] * 4
_V4J = {3: "vcst", 16: "vf2in", 17: "vf2iz", 18: "vf2iu", 19: "vf2id", 20: "vi2f", 21: "vcmov"}
_V5 = ["vpfxs", "vpfxs", "vpfxt", "vpfxt", "vpfxd", "vpfxd", "viim", "vfim"]
_V6 = ["vmmul"] * 4 + ["vtfm2"] * 4 + ["vtfm3"] * 4 + ["vtfm4"] * 4 + ["vmscl"] * 4 + ["vcrsp/vqmul"] * 4
_VMAT = {0: "vmmov", 3: "vmidt", 6: "vmzero", 7: "vmone"}
_SIZE = [".s", ".p", ".t", ".q"]


def vfpu_size(word: int) -> int:
    """0 = single, 1 = pair, 2 = triple, 3 = quad (bits 7 and 15)."""
    return ((word >> 7) & 1) | ((word >> 14) & 2)


def vfpu_name(word: int) -> str:
    op, sub3, sub5, sub16 = word >> 26, (word >> 23) & 7, (word >> 21) & 31, (word >> 16) & 31
    sz = _SIZE[vfpu_size(word)]
    name = None
    if op == 0x18: name = _V0[sub3]
    elif op == 0x19: name = _V1[sub3]
    elif op == 0x1B: name = _V3[sub3]
    elif op == 0x34:
        if sub5 == 0: name = _V4[sub16]
        elif sub5 == 1: name = _V7[sub16]
        elif sub5 == 2: name = _V9[sub16]
        elif sub5 >= 24: name = "vwbn"
        else: name = _V4J.get(sub5)
    elif op == 0x37:
        name = _V5[sub3]
        return name or "vfpu?"       # prefixes / immediates have no size
    elif op == 0x3C:
        if sub5 < 24: name = _V6[sub5]
        elif sub5 == 28: name = _VMAT.get((word >> 16) & 15)
        elif sub5 == 29: name = "vrot"
    elif op == 0x12:
        return {3: "mfv", 7: "mtv"}.get((word >> 21) & 31, "cop2?") + ("c" if word & 0x80 else "")
    elif op in (0x32, 0x3A): return "lv.s" if op == 0x32 else "sv.s"
    elif op in (0x36, 0x3E): return ("lv" if op == 0x36 else "sv") + ("l.q" if word & 2 and False else ".q")
    elif op in (0x35, 0x3D): return ("lvl/lvr.q" if op == 0x35 else "svl/svr.q")
    elif op == 0x3F: return "vflush/vnop/vsync"
    return (name + sz) if name else "vfpu?"


def _special(word, addr, rs, rt, rd, sa, funct) -> Insn:
    R = lambda n: ("r", n)
    if funct in (0x00, 0x03):
        return Insn(addr, word, "sll" if funct == 0 else "sra", [R(rd), R(rt), ("i", sa)])
    if funct == 0x02:   # rs field = 1 turns srl into rotr (MIPS32r2)
        return Insn(addr, word, "rotr" if rs == 1 else "srl", [R(rd), R(rt), ("i", sa)])
    if funct in (0x04, 0x07):
        return Insn(addr, word, "sllv" if funct == 4 else "srav", [R(rd), R(rt), R(rs)])
    if funct == 0x06:   # sa field = 1 turns srlv into rotrv
        return Insn(addr, word, "rotrv" if sa == 1 else "srlv", [R(rd), R(rt), R(rs)])
    if funct == 0x08:
        return Insn(addr, word, "jr", [R(rs)], JUMP)
    if funct == 0x09:
        return Insn(addr, word, "jalr", [R(rd), R(rs)], CALL)
    if funct == 0x0C:
        return Insn(addr, word, "syscall", [("x", (word >> 6) & 0xFFFFF)])
    if funct == 0x0D:
        # two code fields; GCC uses break 0,7 for divide-by-zero, 0,6 for overflow
        return Insn(addr, word, "break", [("x", (word >> 16) & 0x3FF), ("x", (word >> 6) & 0x3FF)])
    if funct == 0x0F:
        return Insn(addr, word, "sync")
    if funct in (0x10, 0x12):
        return Insn(addr, word, "mfhi" if funct == 0x10 else "mflo", [R(rd)])
    if funct in (0x11, 0x13):
        return Insn(addr, word, "mthi" if funct == 0x11 else "mtlo", [R(rs)])
    if funct in (0x16, 0x17):   # Allegrex puts clz/clo here, not in SPECIAL2
        return Insn(addr, word, "clz" if funct == 0x16 else "clo", [R(rd), R(rs)])
    if funct in _SPECIAL_MULDIV:
        return Insn(addr, word, _SPECIAL_MULDIV[funct], [R(rs), R(rt)])
    if funct in _SPECIAL_3REG:
        return Insn(addr, word, _SPECIAL_3REG[funct], [R(rd), R(rs), R(rt)])
    return Insn(addr, word, "invalid", [("x", word)], valid=False)


def _special3(word, addr, rs, rt, rd, sa, funct) -> Insn:
    R = lambda n: ("r", n)
    if funct == 0x00:   # ext rt, rs, pos, size      (pos = sa, size = rd + 1)
        return Insn(addr, word, "ext", [R(rt), R(rs), ("i", sa), ("i", rd + 1)])
    if funct == 0x04:   # ins rt, rs, pos, size      (pos = sa, size = rd - sa + 1)
        return Insn(addr, word, "ins", [R(rt), R(rs), ("i", sa), ("i", rd - sa + 1)])
    if funct == 0x20:
        names = {0x02: "wsbh", 0x03: "wsbw", 0x10: "seb", 0x14: "bitrev", 0x18: "seh"}
        if sa in names:
            return Insn(addr, word, names[sa], [R(rd), R(rt)])
    return Insn(addr, word, "invalid", [("x", word)], valid=False)


def _cop1(word, addr, fmt, ft, fs, fd, funct, branch_target) -> Insn:
    R = lambda n: ("r", n)
    F = lambda n: ("f", n)
    if fmt in (0x00, 0x04):
        return Insn(addr, word, "mfc1" if fmt == 0 else "mtc1", [R(ft), F(fs)])
    if fmt in (0x02, 0x06):
        return Insn(addr, word, "cfc1" if fmt == 2 else "ctc1", [R(ft), ("c", fs)])
    if fmt == 0x08:
        names = ["bc1f", "bc1t", "bc1fl", "bc1tl"]
        if ft < 4:
            return Insn(addr, word, names[ft], [("t", branch_target)], BRANCH, ft >= 2)
    if fmt == 0x10:   # .S (single precision)
        if funct in _FPU_S:
            return Insn(addr, word, _FPU_S[funct], [F(fd), F(fs), F(ft)])
        if funct in _FPU_S_UNARY:
            return Insn(addr, word, _FPU_S_UNARY[funct], [F(fd), F(fs)])
        if funct >= 0x30:
            return Insn(addr, word, f"c.{_FPU_COND[funct & 15]}.s", [F(fs), F(ft)])
    if fmt == 0x14 and funct == 0x20:
        return Insn(addr, word, "cvt.s.w", [F(fd), F(fs)])
    return Insn(addr, word, "invalid", [("x", word)], valid=False)
