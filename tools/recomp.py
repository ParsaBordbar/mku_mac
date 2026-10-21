#!/usr/bin/env python3
"""Lesson 05: the static recompiler. MIPS machine code in, C out.

Each MIPS function becomes one C function `void rc_NAME(Ctx *c)`. Each
instruction becomes one or two C statements operating on `c->r[...]`.
Control flow maps like this:

    branch inside the function   -> goto L_xxxxxxxx
    jal X                        -> c->r[31] = return address; rc_X(c);
    jal <import stub>            -> hle_sceSomething(c);
    jr $ra                       -> return;
    jr $t0 (switch jump table)   -> switch (t0) { case ...: goto ...; }
    jalr $t9 (function pointer)  -> rt_call_indirect(c, t9);
    j X / branch to another func -> tail call: rc_X(c); return;

The one MIPS rule that shapes everything: the instruction after a branch or
jump (the *delay slot*) executes before the jump happens. So we evaluate the
branch condition first, then emit the delay slot, then jump.

usage:
  python3 -I tools/recomp.py --psp BOOT.elf --functions build/functions.csv --out build/recomp
  python3 -I tools/recomp.py --elf test.elf --out build/tests/x        (test programs)
"""

import argparse
import os
import re
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mips
import program

FUNCS_PER_FILE = 250


def R(n: int) -> str:
    return "0u" if n == 0 else f"c->r[{n}]"


def setr(n: int, expr: str) -> str:
    """Assignment to a GPR; writes to $zero are dropped (it is hard-wired to 0)."""
    return "" if n == 0 else f"c->r[{n}] = {expr};"


def imm(v: int) -> str:
    return f"0x{v & 0xFFFFFFFF:X}u"


def add_imm(reg: int, v: int) -> str:
    if v == 0:
        return R(reg)
    return f"{R(reg)} - {-v}u" if v < 0 else f"{R(reg)} + {v}u"


class FunctionGen:
    def __init__(self, prog: program.Program, names: dict[int, str], hle: dict[int, str],
                 setjmps: set[int] = frozenset()):
        self.setjmps = setjmps  # calls to these become a host setjmp in the caller (RT_SETJMP)
        self.prog = prog
        self.names = names      # function addr -> C name
        self.hle = hle          # import stub addr -> C name
        self.stats = Counter()

    # -- straight-line instructions -----------------------------------------

    def simple(self, i: mips.Insn) -> str:
        w, op, a = i.word, i.op, i.args
        rs, rt, rd, sa = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        simm = a[-1][1] if a and a[-1][0] == "i" else 0

        def mem_addr():
            base, off = a[1][1], a[1][2]
            return add_imm(base, off)

        if op == "sll" and w == 0:
            return ""
        if op in ("addu", "add"):
            return setr(rd, f"{R(rs)} + {R(rt)}")
        if op in ("subu", "sub"):
            return setr(rd, f"{R(rs)} - {R(rt)}")
        if op in ("addiu", "addi"):
            return setr(rt, add_imm(rs, simm))
        if op == "and":
            return setr(rd, f"{R(rs)} & {R(rt)}")
        if op == "or":
            return setr(rd, f"{R(rs)} | {R(rt)}")
        if op == "xor":
            return setr(rd, f"{R(rs)} ^ {R(rt)}")
        if op == "nor":
            return setr(rd, f"~({R(rs)} | {R(rt)})")
        if op == "slt":
            return setr(rd, f"(int32_t){R(rs)} < (int32_t){R(rt)}")
        if op == "sltu":
            return setr(rd, f"{R(rs)} < {R(rt)}")
        if op == "slti":
            return setr(rt, f"(int32_t){R(rs)} < {simm}")
        if op == "sltiu":   # immediate is sign-extended, then compared unsigned
            return setr(rt, f"{R(rs)} < {imm(simm)}")
        if op in ("andi", "ori", "xori"):
            sym = {"andi": "&", "ori": "|", "xori": "^"}[op]
            return setr(rt, f"{R(rs)} {sym} {imm(w & 0xFFFF)}")
        if op == "lui":
            return setr(rt, imm((w & 0xFFFF) << 16))
        if op == "sll":
            return setr(rd, f"{R(rt)} << {sa}")
        if op == "srl":
            return setr(rd, f"{R(rt)} >> {sa}")
        if op == "sra":
            return setr(rd, f"(uint32_t)((int32_t){R(rt)} >> {sa})")
        if op == "rotr":
            return setr(rd, f"ROTR({R(rt)}, {sa})")
        if op == "sllv":
            return setr(rd, f"{R(rt)} << ({R(rs)} & 31)")
        if op == "srlv":
            return setr(rd, f"{R(rt)} >> ({R(rs)} & 31)")
        if op == "srav":
            return setr(rd, f"(uint32_t)((int32_t){R(rt)} >> ({R(rs)} & 31))")
        if op == "rotrv":
            return setr(rd, f"ROTR({R(rt)}, {R(rs)})")
        if op == "mult":
            return f"MULT(c, {R(rs)}, {R(rt)});"
        if op == "multu":
            return f"MULTU(c, {R(rs)}, {R(rt)});"
        if op in ("madd", "msub"):
            sign = "" if op == "madd" else "-"
            return f"MADD(c, {sign}((int64_t)(int32_t){R(rs)} * (int32_t){R(rt)}));"
        if op in ("maddu", "msubu"):
            sign = "" if op == "maddu" else "-"
            return f"MADD(c, (int64_t){sign}((uint64_t){R(rs)} * {R(rt)}));"
        if op == "div":
            return f"DIV(c, {R(rs)}, {R(rt)});"
        if op == "divu":
            return f"DIVU(c, {R(rs)}, {R(rt)});"
        if op == "mfhi":
            return setr(rd, "c->hi")
        if op == "mflo":
            return setr(rd, "c->lo")
        if op == "mthi":
            return f"c->hi = {R(rs)};"
        if op == "mtlo":
            return f"c->lo = {R(rs)};"
        if op == "movz":
            return "" if rd == 0 else f"if ({R(rt)} == 0) {setr(rd, R(rs))}"
        if op == "movn":
            return "" if rd == 0 else f"if ({R(rt)} != 0) {setr(rd, R(rs))}"
        if op in ("max", "min"):
            cmp = ">" if op == "max" else "<"
            return setr(rd, f"(int32_t){R(rs)} {cmp} (int32_t){R(rt)} ? {R(rs)} : {R(rt)}")
        if op == "clz":
            return setr(rd, f"CLZ({R(rs)})")
        if op == "clo":
            return setr(rd, f"CLZ(~{R(rs)})")
        if op == "ext":
            return setr(rt, f"EXT({R(rs)}, {a[2][1]}, {a[3][1]})")
        if op == "ins":
            return setr(rt, f"INS({R(rt)}, {R(rs)}, {a[2][1]}, {a[3][1]})")
        if op == "seb":
            return setr(rd, f"(uint32_t)(int32_t)(int8_t){R(rt)}")
        if op == "seh":
            return setr(rd, f"(uint32_t)(int32_t)(int16_t){R(rt)}")
        if op == "wsbh":
            return setr(rd, f"WSBH({R(rt)})")
        if op == "wsbw":
            return setr(rd, f"__builtin_bswap32({R(rt)})")
        if op == "bitrev":
            return setr(rd, f"BITREV({R(rt)})")
        loads = {"lw": "MEM_LW", "lh": "MEM_LH", "lhu": "MEM_LHU", "lb": "MEM_LB", "lbu": "MEM_LBU", "ll": "MEM_LW"}
        if op in loads:
            return setr(rt, f"{loads[op]}({mem_addr()})")
        stores = {"sw": "MEM_SW", "sh": "MEM_SH", "sb": "MEM_SB"}
        if op in stores:
            return f"{stores[op]}({mem_addr()}, {R(rt)});"
        if op == "sc":   # store-conditional: single core, always succeeds
            return f"MEM_SW({mem_addr()}, {R(rt)}); {setr(rt, '1')}"
        if op in ("lwl", "lwr"):
            return setr(rt, f"MEM_{op.upper()}({R(rt)}, {mem_addr()})")
        if op in ("swl", "swr"):
            return f"MEM_{op.upper()}({mem_addr()}, {R(rt)});"
        if op in ("cache", "sync"):
            return f"/* {op} */"
        if op == "break":
            return f"rt_break(c, {imm(i.addr)}, {(w >> 6) & 0xFFFFF});"
        if op == "syscall":
            return f"rt_syscall(c, {imm(i.addr)}, 0x{(w >> 6) & 0xFFFFF:X});"

        fpu = self.fpu(i)
        if fpu is not None:
            return fpu

        if w >> 26 in mips.VFPU_OPCODES:
            self.stats["vfpu"] += 1
            return f"vfpu_exec(c, {imm(i.addr)}, {imm(w)});"

        self.stats[f"unimpl:{op}"] += 1
        return f'rt_unimplemented(c, {imm(i.addr)}, {imm(w)}, "{op}");'

    def fpu(self, i: mips.Insn):
        """Floating point. c->f[] views the FPU registers as float, c->fi[] as raw bits."""
        w, op, a = i.word, i.op, i.args
        fd, fs, ft = (w >> 6) & 31, (w >> 11) & 31, (w >> 16) & 31
        F = lambda n: f"c->f[{n}]"
        FI = lambda n: f"c->fi[{n}]"

        if op == "lwc1":
            return f"{FI(a[0][1])} = MEM_LW({add_imm(a[1][1], a[1][2])});"
        if op == "swc1":
            return f"MEM_SW({add_imm(a[1][1], a[1][2])}, {FI(a[0][1])});"
        if op == "mtc1":   # bits move unchanged: mtc1 is a copy, not a conversion
            return f"{FI(fs)} = {R(ft)};"
        if op == "mfc1":
            return setr(ft, FI(fs))
        if op == "cfc1":
            return setr(ft, f"CFC1(c, {fs})")
        if op == "ctc1":
            return f"CTC1(c, {fs}, {R(ft)});"
        binary = {"add.s": "+", "sub.s": "-", "mul.s": "*", "div.s": "/"}
        if op in binary:
            return f"{F(fd)} = {F(fs)} {binary[op]} {F(ft)};"
        if op == "mov.s":
            return f"{FI(fd)} = {FI(fs)};"
        if op == "neg.s":
            return f"{FI(fd)} = {FI(fs)} ^ 0x80000000u;"   # flips the sign bit, NaN payload kept
        if op == "abs.s":
            return f"{FI(fd)} = {FI(fs)} & 0x7FFFFFFFu;"
        if op == "sqrt.s":
            return f"{F(fd)} = __builtin_sqrtf({F(fs)});"
        if op == "cvt.s.w":
            return f"{F(fd)} = (float)(int32_t){FI(fs)};"
        modes = {"round.w.s": "0", "trunc.w.s": "1", "ceil.w.s": "2", "floor.w.s": "3",
                 "cvt.w.s": "c->fcr31"}
        if op in modes:
            return f"{FI(fd)} = CVT_W_S({F(fs)}, {modes[op]});"
        if op.startswith("c.") and op.endswith(".s"):
            return f"c->fcc = FCMP({F(fs)}, {F(ft)}, {w & 15});"
        return None

    # -- control flow ---------------------------------------------------------

    def call(self, target: int) -> str:
        if target in self.setjmps:
            return "RT_SETJMP(c);"
        if target in self.hle:
            return f"{self.hle[target]}(c);"
        if target in self.names:
            return f"{self.names[target]}(c);"
        self.stats["call_unknown"] += 1
        return f"rt_call_indirect(c, {imm(target)});"

    def goto(self, i: mips.Insn, target: int, lo: int, hi: int) -> str:
        """Jump to `target`: a label in this function, or a tail call."""
        if lo <= target < hi:
            return f"goto L_{target:08X};"
        if target in self.names or target in self.hle:
            self.stats["tail_call"] += 1
            return f"{{ {self.call(target)} return; }}"
        self.stats["bad_jump"] += 1
        return f"{{ rt_bad_jump(c, {imm(i.addr)}, {imm(target)}); return; }}"

    def condition(self, i: mips.Insn) -> str:
        w = i.word
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        base = i.op.rstrip("l") if i.op not in ("bltzal", "bgezal", "bltzall", "bgezall") else i.op[:4]
        if i.op in ("bc1f", "bc1fl"):
            return "!c->fcc"
        if i.op in ("bc1t", "bc1tl"):
            return "c->fcc"
        if i.op in ("bvf", "bvt", "bvfl", "bvtl"):   # VFPU condition bit imm3
            bit = f"((c->vctrl[3] >> {(w >> 18) & 7}) & 1)"
            return bit if i.op.startswith("bvt") else f"!{bit}"
        if base in ("beq", "bne"):
            if rs == rt:
                return "1" if base == "beq" else "0"
            return f"{R(rs)} {'==' if base == 'beq' else '!='} {R(rt)}"
        cmp = {"blez": "<= 0", "bgtz": "> 0", "bltz": "< 0", "bgez": ">= 0"}[base]
        return f"(int32_t){R(rs)} {cmp}"

    def control(self, i: mips.Insn, delay: str, lo: int, hi: int, case_labels: list[int]) -> str:
        w = i.word
        rs, rd = (w >> 21) & 31, (w >> 11) & 31
        ret = imm(i.addr + 8)
        op = i.op

        # Linking instructions write $ra (or rd) immediately, so the delay slot
        # already sees the new return address.
        if op == "jal":
            return f"c->r[31] = {ret}; {delay} {self.call(i.target())}"
        if op == "j":
            return f"{delay} {self.goto(i, i.target(), lo, hi)}"
        if op == "jalr":
            return (f"{{ uint32_t t = {R(rs)}; {setr(rd, ret)} {delay} "
                    f"rt_call_indirect(c, t); }}")
        if op == "jr":
            if rs == 31:
                return f"{delay} return;"
            # Computed jump: a switch statement's jump table, or a tail call
            # through a function pointer.
            self.stats["jr_switch"] += 1
            cases = " ".join(f"case {imm(t)}: goto L_{t:08X};" for t in case_labels)
            return (f"{{ uint32_t t = {R(rs)}; {delay} switch (t) {{ {cases} "
                    f"default: rt_call_indirect(c, t); return; }} }}")

        # Conditional branches (bal/bltzal/bgezal also link).
        cond = self.condition(i)
        target = i.target()
        if op in ("bltzal", "bgezal", "bltzall", "bgezall"):
            if lo <= target < hi:
                self.stats["bad_jump"] += 1
                return f"{{ rt_bad_jump(c, {imm(i.addr)}, {imm(target)}); return; }}"
            take = f"{self.call(target)}"
            if i.likely:
                return f"{{ int cond = {cond}; c->r[31] = {ret}; if (cond) {{ {delay} {take} }} }}"
            return f"{{ int cond = {cond}; c->r[31] = {ret}; {delay} if (cond) {take} }}"

        jump = self.goto(i, target, lo, hi)
        if i.likely:   # delay slot only executes when the branch is taken
            return f"if ({cond}) {{ {delay} {jump} }}"
        if cond == "1":
            return f"{delay} {jump}"
        # Condition first: the delay slot may overwrite a register it reads.
        return f"{{ int cond = {cond}; {delay} if (cond) {jump} }}"

    # -- one function ------------------------------------------------------------

    def function(self, addr: int, size: int) -> str:
        p = self.prog
        lo, hi = addr, addr + size
        insns = [mips.decode(p.read_u32(a), a) for a in range(lo, hi, 4)]

        # Every address something can jump to needs a label.
        case_labels = sorted(t for t in p.case_labels if lo <= t < hi)
        labels = set(case_labels)
        for k, ins in enumerate(insns):
            t = ins.target()
            if ins.kind in (mips.BRANCH, mips.JUMP) and t is not None and lo <= t < hi:
                labels.add(t)
        # A delay slot that is itself a jump target gets emitted twice (once
        # inside the branch, once standalone); the fall-through path must then
        # skip the standalone copy, so the instruction after needs a label too.
        for k, ins in enumerate(insns[:-1]):
            if ins.has_delay_slot and insns[k + 1].addr in labels and ins.addr + 8 < hi:
                labels.add(ins.addr + 8)

        out = [f"void {self.names[addr]}(Ctx *c) {{", f"    RT_ENTER({imm(addr)});"]
        k = 0
        while k < len(insns):
            ins = insns[k]
            if ins.addr in labels:
                out.append(f"L_{ins.addr:08X}:;")
            if ins.has_delay_slot:
                nxt = insns[k + 1] if k + 1 < len(insns) else None
                if nxt is not None and nxt.has_delay_slot:
                    self.stats["branch_in_delay_slot"] += 1
                    delay = f'rt_unimplemented(c, {imm(nxt.addr)}, {imm(nxt.word)}, "branch in delay slot");'
                else:
                    delay = self.simple(nxt) if nxt else ""
                out.append(f"    {self.control(ins, delay, lo, hi, case_labels)}  /* {ins.addr:08X}: {ins.text()} */")
                if nxt is not None and nxt.addr in labels:
                    after = ins.addr + 8
                    out.append(f"    goto L_{after:08X};" if after < hi else f"    {self.goto(ins, after, lo, hi)}")
                    out.append(f"L_{nxt.addr:08X}:;")
                    out.append(f"    {self.simple(nxt)}  /* {nxt.addr:08X}: {nxt.text()} (delay slot target) */")
                k += 2
            else:
                code = self.simple(ins)
                if code:
                    out.append(f"    {code}  /* {ins.addr:08X}: {ins.text()} */")
                k += 1

        # Code that runs off the end continues into the next function.
        if hi in self.names:
            out.append(f"    {self.names[hi]}(c); return;  /* fall through */")
        else:
            out.append(f"    rt_bad_jump(c, {imm(hi - 4)}, {imm(hi)});")
        out.append("}")
        return "\n".join(out) + "\n"


def c_name(name: str) -> str:
    return "rc_" + re.sub(r"[^A-Za-z0-9_]", "_", name)


def load_patches(path: str | None, base: int) -> dict[int, str]:
    """config/patches.txt: `address  c_function  # comment` -> {absolute addr: c_function}.
    A function name starting with "@" marks a call-site rewrite (e.g. @setjmp)."""
    patches = {}
    if path:
        for line in open(path):
            line = line.split("#")[0].split()
            if len(line) >= 2:
                patches[base + int(line[0], 16)] = line[1]
    return patches


def apply_word_patches(prog: program.Program, patches: dict[int, str]) -> None:
    """`addr @word:OLD:NEW` replaces one instruction word before recompiling.
    OLD must match what is there, so a patch can't silently land on the wrong
    build of the game."""
    for addr, name in patches.items():
        if not name.startswith("@word:"):
            continue
        old, new = (int(x, 16) for x in name.split(":")[1:3])
        have = prog.read_u32(addr)
        if have != old:
            raise SystemExit(f"patch at 0x{addr - prog.base:X}: expected 0x{old:08X}, found 0x{have:08X}")
        struct.pack_into("<I", prog.memory, addr - prog.base, new)


def generate(prog: program.Program, out_dir: str, patches: dict[int, str] | None = None) -> Counter:
    patches = patches or {}
    setjmps = {a for a, n in patches.items() if n == "@setjmp"}
    patches = {a: n for a, n in patches.items() if not n.startswith("@")}
    os.makedirs(out_dir, exist_ok=True)
    names = {a: c_name(n) for a, (_, n) in prog.functions.items()}
    hle = {a: "hle_" + re.sub(r"[^A-Za-z0-9_]", "_", n) for a, n in prog.imports.items()}
    gen = FunctionGen(prog, names, hle, setjmps)
    addrs = sorted(prog.functions)

    with open(os.path.join(out_dir, "funcs.h"), "w") as f:
        f.write('#pragma once\n#include "recomp.h"\n\n')
        for a in addrs:
            f.write(f"void {names[a]}(Ctx *c);\n")
        for name in sorted(set(hle.values()) | set(patches.values())):
            f.write(f"void {name}(Ctx *c);\n")

    for chunk in range(0, len(addrs), FUNCS_PER_FILE):
        path = os.path.join(out_dir, f"funcs_{chunk // FUNCS_PER_FILE:03d}.c")
        with open(path, "w") as f:
            f.write('#include "funcs.h"\n\n')
            for a in addrs[chunk:chunk + FUNCS_PER_FILE]:
                if a in patches:   # hand-written replacement (config/patches.txt)
                    f.write(f"void {names[a]}(Ctx *c) {{ {patches[a]}(c); }}\n\n")
                    gen.stats["patched"] += 1
                    continue
                f.write(gen.function(a, prog.functions[a][0]))
                f.write("\n")
        gen.stats["files"] += 1

    # Address -> function table for indirect calls (also the import stubs).
    table = sorted([(a, names[a], prog.functions[a][1]) for a in addrs] +
                   [(a, hle[a], prog.imports[a]) for a in prog.imports])
    with open(os.path.join(out_dir, "table.c"), "w") as f:
        f.write('#include "funcs.h"\n\n')
        f.write("const RecompFnEntry rt_func_table[] = {\n")
        for a, cname, name in table:
            f.write(f'    {{0x{a:08X}u, {cname}, "{name}"}},\n')
        f.write("};\n")
        f.write(f"const uint32_t rt_func_table_len = {len(table)};\n")
        f.write(f"const uint32_t rt_image_base = 0x{prog.base:08X}u;\n")
        f.write(f"const uint32_t rt_image_size = {len(prog.memory)}u;\n")
        f.write(f"const uint32_t rt_entry = 0x{prog.entry:08X}u;\n")

    # Default HLE implementations: log and return 0. `weak` lets a real
    # implementation anywhere in runtime/hle/ replace one without touching this file.
    with open(os.path.join(out_dir, "hle_stubs.c"), "w") as f:
        f.write('#include "funcs.h"\n\n')
        for a in sorted(prog.imports):
            f.write(f'__attribute__((weak)) void {hle[a]}(Ctx *c) {{ '
                    f'rt_hle_unimplemented(c, "{prog.imports[a]}"); c->r[2] = 0; }}\n')

    with open(os.path.join(out_dir, "image.bin"), "wb") as f:
        f.write(prog.memory)

    gen.stats["functions"] = len(addrs)
    return gen.stats


def main():
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--psp", help="PSP PRX (BOOT.elf)")
    src.add_argument("--elf", help="statically linked MIPS ELF (tests)")
    ap.add_argument("--functions", default="build/functions.csv")
    ap.add_argument("--out", required=True)
    ap.add_argument("--patches", help="config/patches.txt: functions replaced by hand-written C")
    args = ap.parse_args()

    prog = program.from_psp(args.psp, args.functions) if args.psp else program.from_elf(args.elf)
    patches = load_patches(args.patches, prog.base)
    apply_word_patches(prog, patches)
    stats = generate(prog, args.out, patches)
    for k, v in sorted(stats.items()):
        print(f"  {k:<28} {v}")


if __name__ == "__main__":
    main()
