#!/usr/bin/env python3
"""Lesson 03 oracle test: compare our decoder with LLVM on every instruction.

LLVM doesn't know the PSP-only instructions (VFPU, min/max, clz in SPECIAL,
madd in SPECIAL, ...). Those are reported separately as "psp-only" instead of
as mismatches, so the real mismatch count should reach 0.

usage: python3 -I tools/check_disasm.py BOOT.elf [--show 20]
needs: brew install llvm
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mips
import psp_elf

PSP_ONLY = {"max", "min", "clz", "clo", "madd", "maddu", "msub", "msubu", "bitrev", "wsbw",
            "invalid"} | set(mips._VFPU_OPS.values())


def find_llvm_objdump():
    for p in ("/opt/homebrew/opt/llvm/bin/llvm-objdump", "/usr/local/opt/llvm/bin/llvm-objdump"):
        if os.path.exists(p):
            return p
    return shutil.which("llvm-objdump-mips") or sys.exit("Homebrew LLVM not found: brew install llvm")


def relocated_elf(m: psp_elf.PspModule) -> bytes:
    """A copy of the ELF with our relocated segment bytes written back, so LLVM
    and our decoder see identical instructions. e_type becomes ET_EXEC so LLVM
    treats it as an ordinary executable."""
    data = bytearray(open(m.path, "rb").read())
    struct.pack_into("<H", data, 0x10, 2)
    for seg in m.segments:
        data[seg.offset:seg.offset + seg.filesz] = m.memory[seg.vaddr:seg.vaddr + seg.filesz]
    return bytes(data)


def llvm_disasm(objdump, m: psp_elf.PspModule) -> dict[int, str]:
    with tempfile.NamedTemporaryFile(suffix=".elf") as f:
        f.write(relocated_elf(m))
        f.flush()
        out = subprocess.run(
            [objdump, "-d", "--section=.text", "--triple=mipsel", "--mcpu=mips32r2",
             "--no-show-raw-insn", f.name],
            capture_output=True, text=True).stdout
    result = {}
    line_re = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")
    for line in out.splitlines():
        m = line_re.match(line)
        if m:
            text = re.sub(r"\s*<[^>]*>", "", m.group(2)).strip()
            result[int(m.group(1), 16)] = re.sub(r"\s+", " ", text)
    return result


def normalize(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--show", type=int, default=15, help="mismatches to print per mnemonic")
    args = ap.parse_args()

    m = psp_elf.load(args.elf)
    text = m.section(".text")
    code = bytes(m.memory[text.addr - m.base:text.addr + text.size - m.base])
    theirs = llvm_disasm(find_llvm_objdump(), m)

    ok = 0
    psp_only = Counter()
    bad = Counter()
    examples = {}
    for k, w in enumerate(struct.unpack(f"<{len(code) // 4}I", code)):
        addr = text.addr + 4 * k
        ins = mips.decode(w, addr)
        if ins.op in PSP_ONLY or w >> 26 in mips.VFPU_OPCODES:
            psp_only[ins.op] += 1
            continue
        ours = normalize(ins.text(llvm=True))
        # llvm-objdump prints a run of zero words as "..." instead of nops
        ref = theirs.get(addr, "nop" if w == 0 else "<missing>")
        if ours == ref:
            ok += 1
        else:
            bad[ins.op] += 1
            examples.setdefault(ins.op, []).append((addr, w, ours, ref))

    total = ok + sum(bad.values())
    print(f"match    : {ok}/{total} ({100 * ok / total:.3f}%)")
    print(f"psp-only : {sum(psp_only.values())} {dict(psp_only.most_common())}")
    print(f"mismatch : {sum(bad.values())}")
    for op, n in bad.most_common():
        print(f"\n  {op}: {n}")
        for addr, w, ours, ref in examples[op][:args.show]:
            print(f"    {addr:8x} {w:08x}  ours: {ours:<32} llvm: {ref}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
