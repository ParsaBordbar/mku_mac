"""Load a PSP PRX (relocatable ELF) into a flat memory image.

A PSP game executable is an ELF file with e_type 0xFFA0 ("PRX"). Unlike a
normal Linux ELF, it is linked at address 0 and carries its relocation
tables so the PSP kernel can load it anywhere. We use those tables for two
things:

  1. Produce a memory image at a fixed base (0 by default), exactly like the
     PSP loader would.
  2. Know *precisely* which words are pointers. That separates code from data
     without guessing -- the hardest problem in static recompilation.

Only the standard library is used, so every step stays readable.

Reference: PPSSPP Core/ELF/ElfReader.cpp (LoadRelocations) and
pspsdk's psp-prxgen.
"""

from __future__ import annotations
import struct
from dataclasses import dataclass, field

# PSP-specific section type for relocations (standard ELF uses SHT_REL = 9).
SHT_PSP_REL = 0x700000A0

# MIPS relocation types used by PSP games.
R_MIPS_NONE = 0
R_MIPS_32 = 2     # full 32-bit pointer stored in a word (data tables, vtables)
R_MIPS_26 = 4     # target of a `j`/`jal` instruction (26-bit field)
R_MIPS_HI16 = 5   # upper half of an address built with `lui`
R_MIPS_LO16 = 6   # lower half (addiu / ori / lw / sw offset)

RELOC_NAMES = {0: "NONE", 2: "32", 4: "26", 5: "HI16", 6: "LO16"}


@dataclass
class Section:
    index: int
    name: str
    type: int
    flags: int
    addr: int         # vaddr relative to the load base (add PspModule.base for absolute)
    offset: int
    size: int
    link: int
    info: int


@dataclass
class Segment:
    index: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    flags: int


@dataclass
class Reloc:
    offset: int       # virtual address of the word being patched
    type: int         # R_MIPS_*
    target_seg: int   # segment whose base gets added
    section: str      # which .rel.* table it came from


@dataclass
class Import:
    library: str
    nid: int
    stub_addr: int    # address of the 8-byte stub the game `jal`s into
    name: str | None = None


@dataclass
class PspModule:
    path: str
    entry: int
    name: str
    sections: list[Section]
    segments: list[Segment]
    relocs: list[Reloc]
    imports: list[Import]
    memory: bytearray = field(repr=False)
    base: int = 0

    # -- helpers ------------------------------------------------------------

    def section(self, name: str) -> Section:
        for s in self.sections:
            if s.name == name:
                return s
        raise KeyError(name)

    def read_u32(self, addr: int) -> int:
        return struct.unpack_from("<I", self.memory, addr - self.base)[0]

    def read_cstr(self, addr: int) -> str:
        off = addr - self.base
        end = self.memory.index(b"\0", off)
        return self.memory[off:end].decode("latin-1")

    def text_range(self) -> tuple[int, int]:
        """Address range of all executable sections (.text + .sceStub.*)."""
        execs = [s for s in self.sections if s.flags & 0x4 and s.size]  # SHF_EXECINSTR
        return min(s.addr for s in execs), max(s.addr + s.size for s in execs)


def _sext16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def load(path: str, base: int = 0) -> PspModule:
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF file (encrypted EBOOT? use the decrypted BOOT.BIN)")
    e_type, = struct.unpack_from("<H", data, 0x10)
    if e_type != 0xFFA0:
        raise ValueError(f"expected PSP PRX (e_type 0xFFA0), got {e_type:#x}")

    e_entry, e_phoff, e_shoff = struct.unpack_from("<III", data, 0x18)
    e_phnum, = struct.unpack_from("<H", data, 0x2C)
    e_shnum, e_shstrndx = struct.unpack_from("<HH", data, 0x30)

    # Program headers: what actually gets loaded into memory.
    segments = []
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, _p_paddr, p_filesz, p_memsz, p_flags, _ = \
            struct.unpack_from("<8I", data, e_phoff + i * 32)
        if p_type == 1:  # PT_LOAD
            segments.append(Segment(i, p_off, p_vaddr, p_filesz, p_memsz, p_flags))

    # Section headers: names and boundaries (.text, .rodata, .rel.text, ...).
    raw = [struct.unpack_from("<10I", data, e_shoff + i * 40) for i in range(e_shnum)]
    strtab_off = raw[e_shstrndx][4]

    def sname(off):
        start = strtab_off + off
        return data[start:data.index(b"\0", start)].decode()

    sections = [Section(i, sname(r[0]), r[1], r[2], r[3], r[4], r[5], r[6], r[7])
                for i, r in enumerate(raw)]

    # Build the memory image: every segment copied to base + vaddr, bss zeroed.
    mem_end = max(s.vaddr + s.memsz for s in segments)
    memory = bytearray(mem_end)
    for seg in segments:
        memory[seg.vaddr:seg.vaddr + seg.filesz] = data[seg.offset:seg.offset + seg.filesz]

    seg_base = [base + seg.vaddr for seg in segments]

    # Parse and apply relocations.
    #   r_info bits 0-7   : relocation type
    #   r_info bits 8-15  : segment that r_offset is relative to
    #   r_info bits 16-23 : segment whose base address gets added
    relocs: list[Reloc] = []
    for s in sections:
        if s.type != SHT_PSP_REL:
            continue
        for k in range(s.size // 8):
            r_offset, r_info = struct.unpack_from("<II", data, s.offset + k * 8)
            rtype = r_info & 0xFF
            ofs_seg = (r_info >> 8) & 0xFF
            tgt_seg = (r_info >> 16) & 0xFF
            relocs.append(Reloc(segments[ofs_seg].vaddr + r_offset, rtype, tgt_seg, s.name))

    _apply_relocations(memory, relocs, seg_base, base)

    module = PspModule(path, base + e_entry, "", sections, segments, relocs, [], memory, base)
    module.name = _module_name(module)
    module.imports = _parse_imports(module)
    return module


def _apply_relocations(memory: bytearray, relocs: list[Reloc], seg_base: list[int], base: int):
    """Patch the image like the PSP kernel does.

    HI16/LO16 come in pairs: `lui r, %hi(addr)` then `addiu r, r, %lo(addr)`.
    The full address is only known once we see the LO16, so HI16s wait in a
    pending list. Because %lo is *signed*, %hi must be rounded: hi = (addr + 0x8000) >> 16.
    """
    def rd(a):
        return struct.unpack_from("<I", memory, a - base)[0]

    def wr(a, v):
        struct.pack_into("<I", memory, a - base, v & 0xFFFFFFFF)

    pending_hi: list[Reloc] = []
    for r in relocs:
        addr = r.offset + base
        delta = seg_base[r.target_seg]
        op = rd(addr)
        if r.type == R_MIPS_32:
            wr(addr, op + delta)
        elif r.type == R_MIPS_26:
            target = ((op & 0x03FFFFFF) << 2) + delta
            wr(addr, (op & 0xFC000000) | ((target >> 2) & 0x03FFFFFF))
        elif r.type == R_MIPS_HI16:
            pending_hi.append(r)
        elif r.type == R_MIPS_LO16:
            lo = _sext16(op & 0xFFFF)
            for hi in pending_hi:
                hi_addr = hi.offset + base
                hi_op = rd(hi_addr)
                full = ((hi_op & 0xFFFF) << 16) + lo + delta
                wr(hi_addr, (hi_op & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF))
            pending_hi.clear()
            wr(addr, (op & 0xFFFF0000) | ((lo + delta) & 0xFFFF))


def _module_name(m: PspModule) -> str:
    try:
        info = m.section(".rodata.sceModuleInfo")
    except KeyError:
        return "?"
    off = info.addr + 4   # section addresses are vaddrs = offsets into m.memory
    return m.memory[off:off + 28].split(b"\0")[0].decode()


def _parse_imports(m: PspModule) -> list[Import]:
    """Read .lib.stub: one entry per system library the game imports.

    struct SceLibStubEntry {          // 5 words = 20 bytes
        u32 libname;                  // pointer to "sceDisplay", ...
        u16 version, attr;
        u8  entry_size_words, num_vars; u16 num_funcs;
        u32 nid_table;                // array of u32 NIDs
        u32 stub_table;               // array of 8-byte stubs (jr ra; syscall)
    };
    A NID is the first 4 bytes of SHA-1(function name), so names have to be
    recovered from a database (see nids.py).
    """
    try:
        sec = m.section(".lib.stub")
    except KeyError:
        return []
    out = []
    addr, end = m.base + sec.addr, m.base + sec.addr + sec.size
    while addr < end:
        libname_p = m.read_u32(addr)
        size_words = m.memory[addr - m.base + 8]
        nfuncs, = struct.unpack_from("<H", m.memory, addr - m.base + 10)
        nid_tab = m.read_u32(addr + 12)
        stub_tab = m.read_u32(addr + 16)
        lib = m.read_cstr(libname_p)
        for i in range(nfuncs):
            out.append(Import(lib, m.read_u32(nid_tab + 4 * i), stub_tab + 8 * i))
        addr += size_words * 4
    return out
