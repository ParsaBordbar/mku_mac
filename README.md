# mku-port

A learning project: a **native** port of *Mortal Kombat: Unchained* (PSP, 2006 —
the handheld version of *MK: Deception*, internal module name `MK6`) to macOS,
then Windows, then Nintendo Switch homebrew.

Approach: **static recompilation** (translate the game's MIPS machine code to C
ahead of time, like N64Recomp / XenonRecomp), plus a runtime that reimplements
the PSP system functions the game calls. Later, recompiled functions get
replaced by hand-decompiled C, one at a time.

No game data is in this repo. Everything is generated from your own dump.

## Setup

```sh
# 1. Your own disc dump, mounted read-only; copy the unencrypted executable out
hdiutil attach -readonly -nobrowse -mountpoint /tmp/mku "path/to/Mortal Kombat - Unchained (USA).iso"
cp /tmp/mku/PSP_GAME/SYSDIR/BOOT.BIN ../work/mkunchained-psp/BOOT.elf
hdiutil detach /tmp/mku

# 2. Reference data (PSP NID database)
tools/fetch_third_party.sh
```

## Tools

| Tool | Lesson | What it does |
|---|---|---|
| `tools/elfinfo.py BOOT.elf [--imports]` | [01](docs/lessons/01-psp-executable.md) | Segments, sections, relocations, imported PSP functions |
| `tools/find_functions.py BOOT.elf` | [02](docs/lessons/02-finding-functions.md) | Every function start → `build/functions.csv` |
| `tools/disasm.py BOOT.elf 0x1dd210` / `--stats` | [03](docs/lessons/03-disassembler.md) | Disassemble with our own decoder |
| `tools/check_disasm.py BOOT.elf` | [03](docs/lessons/03-disassembler.md) | Oracle test vs LLVM (needs `brew install llvm`) |

Run Python tools with `python3 -I` (isolated mode). Standard library only.

## Build and run (macOS, Apple Silicon)

```sh
brew install llvm lld   # MIPS clang + linker for the tests, MIPS objdump for check_disasm
make test               # recompiler test suite
make                    # recompile the game → build/mku (native arm64)
make run                # reads game files from your ISO (set ISO=... if it lives elsewhere)
MKU_TRACE=1 MKU_TIMEOUT=20 make run   # log every PSP system call, stop after 20 s
```

| Path | What |
|---|---|
| `tools/recomp.py` | MIPS → C ([lesson 05](docs/lessons/05-recompiler.md)) |
| `runtime/` | C runtime the generated code links against |
| `runtime/hle/` | the PSP kernel and system libraries, reimplemented ([lesson 07](docs/lessons/07-kernel.md)) |
| `runtime/vfpu.c` | the PSP vector unit ([lesson 06b](docs/lessons/06b-vfpu-and-processes.md)) |
| `runtime/fiber.c`, `runtime/patches/` | fibers + hand-written replacements for Midway's coroutine code (`config/patches.txt`) |
| `tests/recomp/` | differential tests: native C vs recompiled MIPS |

## Plan

See [docs/ROADMAP.md](docs/ROADMAP.md).
