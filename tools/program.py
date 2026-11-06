"""What the recompiler needs to know about a program, independent of its origin.

Two sources:
  from_psp()  -- the game: PSP PRX + build/functions.csv from find_functions.py
  from_elf()  -- test programs: an ordinary MIPS ELF linked by ld.lld, with a
                 symbol table that tells us the functions directly
"""

from __future__ import annotations
import csv
import os
import struct
import sys
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nids
import psp_elf

# Where the PSP kernel loads a game module (start of user memory + 0x4000).
PSP_LOAD_BASE = 0x08804000


@dataclass
class Program:
    memory: bytearray = field(repr=False)   # bytes at [base, base + len)
    base: int
    entry: int
    functions: dict[int, tuple[int, str]]  # addr -> (size, name)
    imports: dict[int, str]                 # stub addr -> HLE function name
    case_labels: set[int]                   # addresses used by switch jump tables
    text_lo: int
    text_hi: int

    def read_u32(self, addr: int) -> int:
        return struct.unpack_from("<I", self.memory, addr - self.base)[0]


def from_psp(elf_path: str, functions_csv: str, base: int = PSP_LOAD_BASE) -> Program:
    m = psp_elf.load(elf_path, base=base)
    nids.name_imports(m.imports, nids.load_nid_db())

    functions = {}
    for row in csv.DictReader(open(functions_csv)):
        if row["evidence"] == "import":
            continue
        functions[base + int(row["address"], 16)] = (int(row["size"]), row["name"])

    imports = {imp.stub_addr: imp.name or f"{imp.library}_{imp.nid:08X}" for imp in m.imports}

    text = m.section(".text")
    text_lo, text_hi = base + text.addr, base + text.addr + text.size

    # Pointers in .rodata that land inside .text: switch-table case labels
    # (and a few const function pointers, which are harmless to include).
    case_labels = set()
    for r in m.relocs:
        if r.section == ".rel.rodata" and r.type == psp_elf.R_MIPS_32:
            target = m.read_u32(r.offset + base)
            if text_lo <= target < text_hi:
                case_labels.add(target)

    return Program(m.memory, base, m.entry, functions, imports, case_labels, text_lo, text_hi)


def from_elf(path: str) -> Program:
    """Load a statically linked little-endian MIPS ELF (tests)."""
    data = open(path, "rb").read()
    e_entry, e_phoff, e_shoff = struct.unpack_from("<III", data, 0x18)
    e_phnum, = struct.unpack_from("<H", data, 0x2C)
    e_shnum, e_shstrndx = struct.unpack_from("<HH", data, 0x30)

    loads = []
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from("<8I", data, e_phoff + i * 32)
        if p_type == 1 and p_memsz:
            loads.append((p_off, p_vaddr, p_filesz, p_memsz))
    base = min(v for _, v, _, _ in loads)
    memory = bytearray(max(v + ms for _, v, _, ms in loads) - base)
    for off, vaddr, filesz, _ in loads:
        memory[vaddr - base:vaddr - base + filesz] = data[off:off + filesz]

    secs = [struct.unpack_from("<10I", data, e_shoff + i * 40) for i in range(e_shnum)]
    shstr = secs[e_shstrndx][4]
    name_of = lambda off, tab: data[tab + off:data.index(b"\0", tab + off)].decode()

    functions = {}
    text_lo, text_hi = 1 << 32, 0
    for s in secs:
        if name_of(s[0], shstr) == ".text":
            text_lo, text_hi = s[3], s[3] + s[5]
        if s[1] == 2:  # SHT_SYMTAB
            strtab = secs[s[6]][4]
            for k in range(s[5] // 16):
                st_name, st_value, st_size, st_info, _, _ = struct.unpack_from("<IIIBBH", data, s[4] + k * 16)
                if st_info & 0xF == 2 and st_size:  # STT_FUNC
                    functions[st_value] = (st_size, name_of(st_name, strtab))

    return Program(memory, base, e_entry, functions, {}, set(), text_lo, text_hi)
