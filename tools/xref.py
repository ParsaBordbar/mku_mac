#!/usr/bin/env python3
"""Cross-references: which functions use a given string or address?

Every address the code builds with lui + addiu/ori (paired by register, see
lesson 07) and every pointer word in .data/.rodata is collected, then the
query is matched against them.

usage:
  python3 -I tools/xref.py BOOT.elf --string LOADING      # strings containing text
  python3 -I tools/xref.py BOOT.elf --addr 0x58E900        # a data address (module-relative)
"""

import argparse
import bisect
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psp_elf

OP_ADDIU, OP_ORI = 0x09, 0x0D


def build_refs(m):
    refs = []   # (target address, site address)
    hi_by_reg = {}
    for r in m.relocs:
        word = m.read_u32(r.offset)
        if r.section == ".rel.text":
            if r.type == psp_elf.R_MIPS_HI16:
                hi_by_reg[(word >> 16) & 31] = word & 0xFFFF
            elif r.type == psp_elf.R_MIPS_LO16:
                hi = hi_by_reg.get((word >> 21) & 31)
                if hi is None:
                    continue
                lo = word & 0xFFFF
                if word >> 26 != OP_ORI and lo & 0x8000:
                    lo -= 0x10000
                refs.append((((hi << 16) + lo) & 0xFFFFFFFF, r.offset))
        elif r.type == psp_elf.R_MIPS_32:
            refs.append((word, r.offset))
    return refs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--string")
    ap.add_argument("--addr")
    ap.add_argument("--functions", default="build/functions.csv")
    args = ap.parse_args()

    m = psp_elf.load(args.elf)
    funcs = sorted((int(r["address"], 16), r["name"]) for r in csv.DictReader(open(args.functions))
                   if r["evidence"] != "import")
    starts = [a for a, _ in funcs]
    by_target = {}
    for target, site in build_refs(m):
        by_target.setdefault(target, []).append(site)

    def owner(site):
        i = bisect.bisect_right(starts, site) - 1
        return funcs[i][1] if i >= 0 else "?"

    targets = []
    if args.addr:
        targets.append((int(args.addr, 16), args.addr))
    if args.string:
        data = bytes(m.memory)
        for match in re.finditer(re.escape(args.string.encode()), data):
            start = data.rfind(b"\0", 0, match.start()) + 1   # beginning of the C string
            end = data.index(b"\0", match.end())
            targets.append((start, data[start:end].decode("latin-1")))
    for addr, label in sorted(set(targets)):
        sites = by_target.get(addr, [])
        print(f"0x{addr:08X} {label!r}: {len(sites)} reference(s)")
        for s in sites[:12]:
            print(f"    0x{s:08X} in {owner(s)}")


if __name__ == "__main__":
    main()
