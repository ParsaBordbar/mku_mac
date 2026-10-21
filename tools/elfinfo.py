#!/usr/bin/env python3
"""Lesson 1 tool: print everything we know about a PSP executable.

usage: python3 -I tools/elfinfo.py BOOT.elf [--imports]
"""

import argparse
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nids
import psp_elf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--imports", action="store_true", help="list every imported function")
    args = ap.parse_args()

    m = psp_elf.load(args.elf)
    print(f"module   : {m.name}")
    print(f"entry    : {m.entry:#010x}")
    print(f"memory   : {len(m.memory):#x} bytes ({len(m.memory) / 2**20:.1f} MiB incl. bss)")

    print("\nsegments (what the PSP loads):")
    for s in m.segments:
        print(f"  #{s.index} vaddr {s.vaddr:#010x} file {s.filesz:#9x} mem {s.memsz:#9x}")

    print("\nsections (size >= 64 KiB):")
    for s in m.sections:
        if s.size >= 0x10000 and s.type != psp_elf.SHT_PSP_REL:
            print(f"  {s.name:<12} {s.addr:#010x}  {s.size / 1024:8.1f} KiB")

    kinds = Counter((r.section, psp_elf.RELOC_NAMES.get(r.type, r.type)) for r in m.relocs)
    print(f"\nrelocations: {len(m.relocs)} total")
    for (sec, kind), n in sorted(kinds.items(), key=lambda kv: -kv[1])[:8]:
        print(f"  {sec:<14} R_MIPS_{kind:<5} {n:7d}")

    db = nids.load_nid_db()
    hits = nids.name_imports(m.imports, db)
    libs = Counter(i.library for i in m.imports)
    print(f"\nimports: {len(m.imports)} functions from {len(libs)} libraries"
          f" ({hits} named{'' if db else '; run tools/fetch_third_party.sh for names'})")
    for lib, n in libs.most_common():
        print(f"  {lib:<22} {n:3d}")

    if args.imports:
        print()
        for i in m.imports:
            print(f"  {i.stub_addr:#010x}  {i.library:<20} {i.nid:#010x}  {i.name or '?'}")


if __name__ == "__main__":
    main()
