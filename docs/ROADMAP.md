# Roadmap

Each phase ends with something you can run and check. Don't start a phase until
the previous one's checkpoint passes — bugs in early layers (a wrong relocation,
a missed function) show up much later as crashes that are very hard to trace back.

Legend: ✅ done · 🟡 in progress · ⬜ not started · 🧑‍🎓 = exercise for you

---

## Phase 0 — Setup ✅
- Dump own disc, keep dumps outside the repo, `.gitignore` all game data.
- Found: `BOOT.BIN` is an **unencrypted** PRX ELF (no decryption needed).

## Phase 1 — Understand the executable ✅ → [lesson 01](lessons/01-psp-executable.md)
- `psp_elf.py`: load segments, apply PSP relocations, parse imports.
- Result: 2.7 MiB code, 132,526 relocations, 249 imports from 26 PSP libraries, all named.
- **Checkpoint:** `elfinfo.py` prints 249/249 named imports. ✅

## Phase 2 — Find all functions ✅ → [lesson 02](lessons/02-finding-functions.md)
- `find_functions.py`: 13,167 functions from jal / data / code pointers.
- **Checkpoint:** ≥ 99% of `jal` targets start right after a `jr $ra`. ✅ (99.8%)
- 🧑‍🎓 Investigate the 53 `code_ptr` starts that don't follow a return (lesson 02).
- 🧑‍🎓 Decide what to do with the 440 `after_ret`-only blocks.

## Phase 3 — Disassembler ✅ → [lesson 03](lessons/03-disassembler.md)
Write your own decoder for the PSP CPU (Allegrex = MIPS II + some MIPS32 + VFPU).
The recompiler needs the same decoder, so this is not throwaway work.
- `tools/mips.py`: decode R/I/J formats → structured `Insn` (+ text).
- `tools/disasm.py`: disassemble a function / range / `--stats`.
- `tools/check_disasm.py`: oracle test against LLVM. **705,309/705,309 match.** ✅
- Found: `sceKernelSetCompilerVersion(0x30306)` → probably GCC 3.3.6.
- **How to do it properly — use an oracle.** LLVM already disassembles MIPS:
  `brew install llvm`, then compare your output with
  `$(brew --prefix llvm)/bin/llvm-objdump -d --triple=mipsel -mcpu=mips2`
  on every instruction in `.text`. Your decoder is done when the only
  differences are PSP-only instructions (VFPU, `ext`/`ins`, `min`/`max`…),
  which LLVM doesn't know.
- **Checkpoint:** 100% match on non-VFPU instructions; unknown opcodes listed.

## Phase 4 — Names ⬜
Unnamed `func_001DD210` everywhere is miserable. Recover names first.
- 🧑‍🎓 Strings: error messages contain function names (`mem_card_open()`,
  `Error in obj_start_morph mk_anim.c`). Find which function loads each string
  (HI16/LO16 pair pointing at it) → rename. Output `names.csv`.
- Load into **Ghidra** with our function list (we'll write an import script)
  for comfortable reading.
- Later: cross-reference the GameCube *MK Deception* build — same source,
  different compiler. Strings and constants line up across both.
- **Checkpoint:** ≥ 300 functions named, `_start` → `main` path understood.

## Phase 5 — Recompiler v0: integer code ✅ → [lesson 05](lessons/05-recompiler.md)
- `tools/recomp.py` + `runtime/`: MIPS → C, delay slots, branch-likely,
  calls, jump tables, function pointers, weak HLE stubs.
- `tests/recomp/`: differential tests (native C vs recompiled MIPS) at -O0/-O2,
  hand-written asm for GCC-only patterns, mutation-tested. **12/12 pass.** ✅
- **Checkpoint:** whole game → 52 MB of C → native arm64 `build/mku`. ✅
  It runs `_start`, which creates and starts the main thread (HLE stubs log it).
- 130 `bad_jump`s were false function starts from `code_ptr` — fixed in phase 7 (lesson 07).

## Phase 6 — FPU ✅ → [lesson 06](lessons/06-fpu.md), VFPU ✅ → [lesson 06b](lessons/06b-vfpu-and-processes.md)
- FPU: all single-precision instructions; `-ffp-contract=off`; NaN-aware tests. ✅
- VFPU: `runtime/vfpu.c` runs all 45 forms the game uses (`vfpu_exec`), VFPU
  branches recompiled. Tests via `.word`-encoded asm + C references, mutation-tested. ✅
- Known simplifications: plain dot products, libm trig, no prefixes on matrix ops.

## Phase 7 — Runtime and boot ✅ → [lesson 07](lessons/07-kernel.md)
- `runtime/hle/`: threads on host threads with a single "baton", semaphores,
  event flags, pools, memory allocator, files straight from the ISO (ISO9660 +
  raw `umd0:` sector reads), vblank interrupts, display, audio pacing.
- Fixed `find_functions.py` HI16/LO16 pairing: 130 bad jumps → 3.
- **Checkpoint:** boots to the first `sceDisplaySetFrameBuf`. ✅
- Midway's coroutine "process" system (hand-written asm) replaced with
  fibers: `runtime/fiber.c` + `runtime/patches/mk_process.c` +
  `config/patches.txt` (lesson 06b). **Runs at a steady 60 fps** past the
  legal screen; movie playback (`sceMpeg*`) still stubbed.
- Debug tools: `MKU_TRACE=1`, `MKU_TIMEOUT=N`, automatic hang report.

## Phase 8 — Graphics ✅ → [lesson 08](lessons/08-graphics-and-audio.md)
- GE command interpreter and a software renderer: textures (CLUT, swizzled),
  blending, stencil, depth, lighting, fog, culling, near-plane clipping, texgen.
- Not yet: mipmaps, splines, logic ops, dithering.
- **Checkpoint:** title screen, menus and fights visible. ✅

## Phase 9 — Audio and video 🟡 → [lesson 08](lessons/08-graphics-and-audio.md)
- `sceSasCore` (effects) and `sceAtrac3plus` (music via FFmpeg). ✅ No reverb yet.
- `sceMpeg`: `.PMF` movies are still skipped through the real API. ⬜
- **Checkpoint:** menu music plays. ✅

## Phase 10 — Playable on Mac ✅ → [lesson 08](lessons/08-graphics-and-audio.md)
- Input (keyboard, game controller), saves, ad-hoc stubbed.
- **Checkpoint:** a 10-minute scripted run plays through 6+ ladder fights. ✅

## Phase 10b — Remaster and player tool ✅ → [lesson 09](lessons/09-remaster.md)
- 60 fps fights, 2x-4x resolution, xBRZ textures, texture packs, free camera.
- `./mku-play game.iso`: builds from the player's own ISO, ships no game data.

## Phase 11 — Windows and Switch ⬜
- Fibers already support x86-64 (System V): Linux and Intel Macs should build; untested.
- Windows: same C code, build with clang-cl or MinGW, SDL2; needs a Win64 fiber switch.
- Switch: devkitPro + libnx, SDL2 for Switch, OpenGL via mesa (or deko3d).
  Memory: Switch has 4 GiB, PSP game needs 32 MiB — easy.
- **Checkpoint:** runs on your Switch from the homebrew menu.

## Phase 12 — Decompilation (forever, for fun) ⬜
- Replace recompiled functions with readable C one at a time. The game keeps
  running, so every replacement is testable.
- Unlocks: widescreen, 60 fps, mods, and a cleaner engine for the GameCube
  *Deception* build later.

---

## Rules for doing it properly
1. **Never commit game data** or generated code from it. Regenerate from your dump.
2. **Oracle testing everywhere:** LLVM for disassembly, small compiled programs
   for the recompiler, PPSSPP traces for HLE/graphics. Using an emulator as a
   *debugging reference* is how every recomp project works.
3. **Log unknowns loudly.** Unimplemented instruction / HLE call / GE command →
   print it once with its address. Never silently return.
4. **Small commits** with the checkpoint they pass.
5. **Write the lesson** after each phase (`docs/lessons/NN-*.md`): what you
   learned, what surprised you. Future-you will need it.
