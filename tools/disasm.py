#!/usr/bin/env python3
"""Lesson 03 tool: disassemble functions using our own decoder.

usage:
  python3 -I tools/disasm.py BOOT.elf 0x1dd210            # one function (needs build/functions.csv)
  python3 -I tools/disasm.py BOOT.elf 0x1dd210 0x1dd300   # address range
  python3 -I tools/disasm.py BOOT.elf --stats             # how much of .text decodes
"""

import argparse
import csv
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mips
import psp_elf


def load_functions(path):
    funcs = {}
    if os.path.exists(path):
        for row in csv.DictReader(open(path)):
            funcs[int(row["address"], 16)] = (int(row["size"]), row["name"])
    return funcs


def words(m, start, end):
    return struct.unpack(f"<{(end - start) // 4}I", m.memory[start - m.base:end - m.base])


def print_range(m, funcs, start, end):
    branch_targets = set()
    insns = [mips.decode(w, start + 4 * i) for i, w in enumerate(words(m, start, end))]
    for i in insns:
        if i.kind == mips.BRANCH and i.target() is not None:
            branch_targets.add(i.target())
    for i in insns:
        if i.addr in funcs:
            print(f"\n{funcs[i.addr][1]}:")
        elif i.addr in branch_targets:
            print(f"  .L{i.addr:08X}:")
        text = i.text()
        t = i.target()
        if i.kind == mips.CALL and t in funcs:
            text += f" <{funcs[t][1]}>"
        print(f"  {i.addr:8x}: {i.word:08x}  {text}")


def stats(m):
    text = m.section(".text")
    c = Counter()
    for k, w in enumerate(words(m, text.addr, text.addr + text.size)):
        c[mips.decode(w, text.addr + 4 * k).op] += 1
    total = sum(c.values())
    print(f"{total} instructions, {len(c)} distinct mnemonics")
    print(f"invalid: {c['invalid']} ({100 * c['invalid'] / total:.3f}%)  (data inside .text, or bugs)")
    vfpu = sum(v for k, v in c.items() if k in mips._VFPU_OPS.values())
    print(f"VFPU (not decoded yet): {vfpu}")
    for op, n in c.most_common(25):
        print(f"  {op:<10} {n:7d}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("start", nargs="?")
    ap.add_argument("end", nargs="?")
    ap.add_argument("--functions", default="build/functions.csv")
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    m = psp_elf.load(args.elf)
    if args.stats:
        return stats(m)
    funcs = load_functions(args.functions)
    start = int(args.start, 16)
    if args.end:
        end = int(args.end, 16)
    elif start in funcs:
        end = start + funcs[start][0]
    else:
        sys.exit("address is not a known function start; give an end address")
    print_range(m, funcs, start, end)


if __name__ == "__main__":
    main()
