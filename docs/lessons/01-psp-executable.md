# Lesson 01 — What a PSP game executable is

## The disc

```
PSP_GAME/
  PARAM.SFO          title, disc ID (ULUS10102), firmware version (2.71)
  SYSDIR/EBOOT.BIN   the executable, ENCRYPTED (starts with "~PSP")
  SYSDIR/BOOT.BIN    same executable, UNENCRYPTED (starts with "\x7fELF")  ← lucky!
  USRDIR/MODULES/    Sony system libraries (.PRX) shipped with the game
  USRDIR/ARCHIVE.IMP 577 MB of game assets, ARCHIVE.BIN is its index
  USRDIR/SNDSPSP/    sound banks (.BNK) and music (.MUS)
  USRDIR/MOVIEPSP/   videos (.PMF = PSP MPEG-4/H.264)
```

Many PSP games ship `BOOT.BIN` as all zeros, forcing you to decrypt `EBOOT.BIN`.
This one shipped a plain copy — we skip a whole step.

## ELF in one paragraph

ELF is the container format for executables on Linux, PSP, PS3, Switch, Android.
It has **program headers** (what to load into memory, where) and **section
headers** (named pieces: `.text` = code, `.rodata` = constants, `.data` =
initialized globals, `.bss` = zeroed globals). The loader only needs the program
headers; section headers are a gift for us.

```
$ python3 -I tools/elfinfo.py BOOT.elf
module   : MK6               ← internal name: Deception was MK's 6th main game
entry    : 0x001dd210
segments : #0 0x000000 (code+data, 3.7 MiB)   #1 0x3ad660 (bss, 2.2 MiB)
```

## Why the game is a "PRX" and why relocations matter

`e_type = 0xFFA0` means **PRX**: a relocatable module. It is linked as if it
lived at address 0, and the PSP kernel moves it to wherever there's free memory.
To make that possible the file lists every place that contains an address:

| Type | Instruction / data | Meaning |
|---|---|---|
| `R_MIPS_26` | `jal 0x12340` | call target (41,913 of them) |
| `R_MIPS_HI16` + `R_MIPS_LO16` | `lui a0, 0x2b` / `addiu a0, a0, 0x4750` | an address split into two halves |
| `R_MIPS_32` | a word in `.data` | a pointer (vtables, callback tables, jump tables) |

The kernel uses this to *move* the code. **We use it to understand the code:**
the hardest question in static recompilation is "is this number an address or
just a number?" — and here the linker answered it for all 132,526 cases.

### Detail worth knowing: HI16/LO16 rounding
MIPS builds a 32-bit address with `lui` (upper 16 bits) and then `addiu`
(adds a *signed* 16-bit value). If the low half is ≥ 0x8000 it's negative,
so the upper half must be rounded up by one: `hi = (addr + 0x8000) >> 16`.
See `_apply_relocations` in `tools/psp_elf.py`.

### Detail worth knowing: PSP's relocation format
Standard ELF puts the relocation type in `r_info`. PSP packs three things:
`type | (offset_segment << 8) | (target_segment << 16)`, section type `0x700000A0`.

## Imports and NIDs

The game doesn't contain the PSP's system code; it imports **249 functions** from
26 libraries. Each import is an 8-byte stub the kernel patches at load time.
Functions are identified by a **NID** = first 4 bytes of SHA-1 of the name, so
`sceIoOpen` → `0x109F50BC`. We look names up in pspdev's PSPLibDoc database.

The import list is the **contract** our port must fulfil — the game can only
talk to the outside world through these 249 functions:

| Library | # | Our replacement |
|---|---|---|
| ThreadManForUser | 43 | host threads, semaphores, event flags |
| sceMpeg | 24 | FFmpeg (videos) |
| sceSasCore | 21 | our software mixer → SDL audio |
| IoFileMgrForUser | 17 | read files from the ISO |
| sceGe_user | 15 | **GE → OpenGL renderer** (the hardest) |
| sceUtility | 15 | save files, dialogs |
| sceNetAdhoc* | 33 | stub: "no network" |
| sceCtrl | 2 | SDL gamepad |

## Exercises 🧑‍🎓
1. Run `elfinfo.py --imports`. Which functions suggest the game uses multiple
   threads? Which suggest it streams files in the background?
2. Open `tools/psp_elf.py` and find where `.bss` gets zeroed. Why don't we need
   to read anything from the file for it?
3. Compute the NID of `sceCtrlReadBufferPositive` yourself in Python
   (`hashlib.sha1(b"...").digest()[:4]`, little-endian) and check it against
   the list. (Some later-firmware NIDs are randomized and won't match — not
   these old ones.)
