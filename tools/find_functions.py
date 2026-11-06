#!/usr/bin/env python3
"""Lesson 2 tool: find every function in the executable.

A recompiler translates one function at a time, so first we need the list of
function start addresses. Machine code has no markers, so we collect evidence:

  jal      : a `jal X` instruction somewhere calls X  -> X is a function (strong)
  data_ptr : a pointer in .data points into .text      -> vtable / callback (strong)
  code_ptr : `lui+addiu` builds a .text address in code -> callback, e.g. a thread
             entry passed to sceKernelCreateThread       (strong)
  rodata_ptr: a pointer in .rodata points into .text     -> either a const function
             table OR a switch jump table (case label inside a function). Weak.
  after_ret: code right after `jr $ra` + delay slot that nothing references -> maybe
             dead or only reached indirectly. Weak.

Relocations make the "strong" sources exact: we never mistake a number for a
pointer, because the linker told us which words are pointers.

usage: python3 -I tools/find_functions.py BOOT.elf -o build/functions.csv
"""

from __future__ import annotations
import argparse
import csv
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nids
import psp_elf

JR_RA = 0x03E00008
NOP = 0x00000000
OP_JAL = 3
OP_ADDIU = 0x09
OP_ORI = 0x0D


def collect(m: psp_elf.PspModule):
    text = m.section(".text")
    exec_lo, exec_hi = m.text_range()
    in_text = lambda a: text.addr <= a < text.addr + text.size

    evidence: dict[int, set[str]] = defaultdict(set)
    evidence[m.entry].add("entry")

    # Imported stubs are tiny 8-byte "functions" the game jal's into.
    stub_names = {}
    for imp in m.imports:
        stub_names[imp.stub_addr] = imp.name or f"{imp.library}_{imp.nid:08X}"

    # Upper halves loaded by `lui`, per destination register. A LO16 belongs to
    # the last `lui` into the register it reads (its rs field) -- NOT simply the
    # latest HI16: GCC often does `lui $a1, X` once and then uses `$a1` for many
    # loads, with other lui/addiu pairs in between.
    hi_by_reg: dict[int, int] = {}
    for r in m.relocs:
        word = m.read_u32(r.offset)
        if r.section == ".rel.text":
            if r.type == psp_elf.R_MIPS_26 and word >> 26 == OP_JAL:
                target = (r.offset & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
                if target not in stub_names:
                    evidence[target].add("jal")
            elif r.type == psp_elf.R_MIPS_HI16:
                hi_by_reg[(word >> 16) & 31] = word & 0xFFFF
            elif r.type == psp_elf.R_MIPS_LO16 and word >> 26 in (OP_ADDIU, OP_ORI):
                # only addiu/ori *build* an address; lw/sw with a LO16 just access data
                hi = hi_by_reg.get((word >> 21) & 31)
                if hi is None:
                    continue
                lo = word & 0xFFFF
                if word >> 26 == OP_ADDIU and lo & 0x8000:
                    lo -= 0x10000
                target = ((hi << 16) + lo) & 0xFFFFFFFF
                if in_text(target) and r.target_seg == 0:
                    evidence[target].add("code_ptr")
        elif r.type == psp_elf.R_MIPS_32 and in_text(word):
            if r.section in (".rel.data", ".rel.linkonce.d", ".rel.cplinit"):
                evidence[word].add("data_ptr")
            elif r.section == ".rel.rodata":
                evidence[word].add("rodata_ptr")

    # Code that follows a return and is not padding.
    code = m.memory[text.addr - m.base:text.addr - m.base + text.size]
    words = struct.unpack(f"<{len(code) // 4}I", code)
    for i, w in enumerate(words[:-2]):
        if w == JR_RA:
            j = i + 2  # skip delay slot
            while j < len(words) and words[j] == NOP:
                j += 1  # alignment padding
            addr = text.addr + 4 * j
            if j < len(words) and addr not in evidence:
                evidence[addr].add("after_ret")

    return evidence, stub_names, words, text


def classify(evidence):
    """Strong evidence makes a function; weak evidence only counts if it lines up."""
    strong = {"entry", "jal", "data_ptr", "code_ptr"}
    starts = {a for a, ev in evidence.items() if ev & strong}
    # rodata pointers that are also reached by another source are functions;
    # the rest are probably switch-case labels (kept as "maybe").
    maybe = {a for a, ev in evidence.items() if not ev & strong}
    return starts, maybe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("-o", "--out", default="build/functions.csv")
    args = ap.parse_args()

    m = psp_elf.load(args.elf)
    nids.name_imports(m.imports, nids.load_nid_db())
    evidence, stub_names, words, text = collect(m)
    starts, maybe = classify(evidence)

    # Promote after_ret candidates that fall in a gap no strong function covers:
    # a function "ends" at the next start, so an after_ret inside a long tail
    # of unreferenced code is most likely its own function.
    ordered = sorted(starts)
    text_end = text.addr + text.size
    rows = []
    for idx, addr in enumerate(ordered):
        end = ordered[idx + 1] if idx + 1 < len(ordered) else text_end
        name = "_start" if addr == m.entry else f"func_{addr:08X}"
        rows.append((addr, end - addr, name, "+".join(sorted(evidence[addr]))))
    for addr, name in sorted(stub_names.items()):
        rows.append((addr, 8, name, "import"))
    rows.sort()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["address", "size", "name", "evidence"])
        for addr, size, name, ev in rows:
            w.writerow([f"0x{addr:08X}", size, name, ev])
    with open(os.path.splitext(args.out)[0] + "_maybe.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["address", "evidence"])
        for a in sorted(maybe):
            w.writerow([f"0x{a:08X}", "+".join(sorted(evidence[a]))])

    # Summary
    count = defaultdict(int)
    for a in starts:
        for e in evidence[a]:
            count[e] += 1
    sizes = sorted(r[1] for r in rows if r[3] != "import")
    print(f"functions found : {len(starts)}  (+{len(stub_names)} import stubs)")
    for k in ("entry", "jal", "data_ptr", "code_ptr", "rodata_ptr", "after_ret"):
        print(f"  with {k:<10}: {count[k]}")
    print(f"maybe-functions : {len(maybe)}  (rodata_ptr-only = switch labels? after_ret-only = unreferenced?)")
    print(f"size  median {sizes[len(sizes) // 2]} B, largest {sizes[-1]} B")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
