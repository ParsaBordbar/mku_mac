# Mortal Kombat: Unchained, native

Play *Mortal Kombat: Unchained* (PSP, 2006) as a native program on a modern
computer, from your own copy of the game. This is **not an emulator**: the game's
code is translated to C and compiled for your machine.

Remastered: 60 fps fights, up to 4x resolution, smoother textures, a free camera.

## No piracy: bring your own game

**We do not provide, host or link to any game files, and we do not support piracy.**

You must own the game and use an ISO made from your own copy. This repository
contains only our own tools and runtime: no game code, no game data, and no
pre-built version of the game. Everything game-related is generated on your
computer, from your ISO. Please don't share game files, ISOs or built copies of
the game, and don't ask for them here.

## Play on a Mac (no coding needed)

You need a Mac with Apple Silicon (M1 or newer) on macOS 13 or later, and the ISO of
your own copy: the US version, ULUS-10102 v1.03, as a plain `.iso` (not CSO).
Other versions are refused.

1. Download **MK-Unchained-mac.zip** from
   [Releases](https://github.com/ParsaBordbar/mku_mac/releases), unzip it, and move
   **MK Unchained** to your Applications folder.
2. Open it. macOS blocks it the first time, because the app isn't notarized by Apple.
   Click **Done**, then go to **System Settings > Privacy & Security**, scroll down, and
   click **Open Anyway**.
3. Drop your ISO onto the window.
4. If Apple's Command Line Tools aren't installed, a window asks: click **Install**. It's
   free and from Apple. The app carries on by itself when it's done.
5. Wait about a minute while the game is prepared for your Mac, then press **Play**.

Next time, just open the app and press Play.

## Play from a terminal

```sh
./mku-play "Mortal Kombat - Unchained (USA).iso"
```

You don't need to create a profile: a default one (PLAYER 1) is made and signed in for
you, so every mode including Konquest just works. Your own profiles still work: press
L at the main menu and enter your kode.

The first run checks your ISO, tells you the install command if a free tool is
missing, then translates the game for your computer (about a minute). After that
it starts right away and remembers your ISO. You need:

- macOS: `xcode-select --install`, then `brew install sdl2 ffmpeg`.
- Linux (should work, not tested yet): `sudo apt install make clang libsdl2-dev pkg-config libavcodec-dev libavutil-dev`.

### Controls

| | Keyboard | Game controller |
|---|---|---|
| Move | Arrows | D-pad / left stick |
| Cross / Circle / Square / Triangle | Z / X / A / S | Same buttons |
| L / R | Q / W | Shoulders, triggers |
| Start / Select | Enter / Right Shift | Options / Share |
| Camera: rotate | T F G H | Right stick |
| Camera: zoom | R / Y | Hold R3, move right stick |
| Camera: reset | C | L3 |
| Quit | Esc | |

### Graphics

| Key | Option | Values |
|---|---|---|
| F1 | Texture upscaling (xBRZ) | Original, 2x, 4x |
| F2 | Resolution | 480x272, 960x544 (default), 1440x816, 1920x1088 |
| F3 | Window scaling | Smooth, sharp |

The window title shows the current settings and frame rate. Settings stay in
`mku.ini`. If the frame rate drops, lower the resolution with F2.
`./mku-play --scale 3 --textures 2` sets them from the command line.

### Texture packs

`MKU_TEXDUMP=1 ./mku-play` saves every texture the game shows to `build/texdump/`.
Upscale them however you like, keep the file names, and make each one 2, 4 or 8
times the original size. Load the folder with `./mku-play --pack my_textures/`.

### Known gaps

- Intro movies are skipped, and there's no sound reverb.
- On some stages (Slaughterhouse, Konquest's training hall) the floor and parts of
  the walls show as one flat colour. Still being investigated.
- Konquest's open world runs at 20-30 fps for now; fights run at about 60.
- Only tested on an Apple Silicon Mac so far.

Saves are in `saves/` (the app keeps them in
`~/Library/Application Support/MK Unchained/game/saves/`).

---

# For developers

To build the Mac app: `tools/build_deps.sh && tools/make_app.sh` (makes `dist/`).

A learning project: a port of the game (internal module name `MK6`) by **static
recompilation**: translate the MIPS machine code to C ahead of time, like
N64Recomp or XenonRecomp, plus a runtime that reimplements the PSP system
functions the game calls. Start with [docs/ROADMAP.md](docs/ROADMAP.md).

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
