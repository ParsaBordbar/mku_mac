# Lesson 09: remastering, and making it a tool for other people

The game ran. This phase made it look and run better than the original, then
made it usable by someone who has never heard of a recompiler.

## 60 fps was hiding behind a wrong counter

The fps counter said 60 during fights while the game felt bad. The game calls
`sceDisplaySetFrameBuf` **twice** per frame (once latched, once immediate), and
each call counted as a frame: the real number was 30.

Then `func_00061400` turned out to be Midway's own limiter: if a frame took less
than 20,000 us, it sleeps the rest. That's a 50 fps cap, which the vblank wait
turned into about 30 on real hardware. It also stores `0.02 / frame time` as a
speed factor, so the logic follows real time and raising the cap is safe. Two
instruction patches (`@word:OLD:NEW` in `config/patches.txt`, OLD is verified) gave
60 fps. **Check:** the round timer dropped 59, 54, 49, 44 in shots exactly 5.0 s apart.

## Making the renderer fast, without trusting yourself

The trick that made this safe: keep the slow, readable pixel pipeline as a
**reference** (`shade_pixel_ref`), and write the fast one beside it.
`MKU_RASTER_CHECK=1` draws each batch both ways and compares VRAM. Then a
deliberate bug (`256 - a` in the blend) must be caught: 20,134 bad batches.

An earlier version of that check compared banded against banded. A mutation in
the band clipping passed, because both sides had it. **Mutation tests test the
test.** That same fixed check then found a real bug: stepping float weights from
a different row in each band rounded differently on triangle edges.

Measure before optimising. Each guess below was wrong until profiled:

| Guess | What measuring showed |
|---|---|
| Blend math is slow | The fast path wasn't even running: the framebuffer is 5551, not 8888 |
| Interpolation dominates | Half true; bilinear filtering was 40% of shading |
| 10 threads scale | Only 2x: 40-50 small batches each woke the pool. Binning, finer bands, spinning: 3.2x |
| Rendering is what's slow | 40-70 **texture decodes** per frame hid in there: palettes and a bad hash |

The last one is the best. Fibonacci-hashing the cache sets and keying palettes by
content took decodes to 0 and 2x rendering from 6.4 to 5 ms.

SWAR (SIMD within a register) did the bilinear filter two channels per multiply.
It's checked against the plain formula on 50 million random cases.

## HD: three independent things

- **Resolution** (`runtime/gpu/surface.c`, F2): each render target gets a twin N
  times larger, outside PSP memory but inside the guest address space. The
  rasteriser needs no special case: it gets a different base address, stride and
  scaled coordinates. VRAM is synced only when something reads it, and a hash
  notices when the game writes VRAM itself.
- **Texture upscaling** (`runtime/gpu/texpack.c`, F1): xBRZ, a filter designed for
  pixel art.
- **Texture packs**: `MKU_TEXDUMP=1` saves every texture as `<hash>.png`; a folder of
  larger PNGs with the same names replaces them. No pack exists for this game yet.

## The graphics it was missing

Lighting (`runtime/gpu/lighting.c`), fog, backface culling, near-plane clipping and
texture generation. Lighting has a unit test against hand-computed values, also
mutation-checked. Free camera rotation is an orbit applied in view space, around
the depth of last frame's skinned models. It needed real near-plane clipping:
before, any triangle with a vertex behind the camera vanished, leaving holes.

## Sharing it: what you may hand to other people

The recompiled C is derived from the game's code, so a pre-built binary would
distribute the game. So `mku-play` ships nothing of the game and does the
translation on the player's machine, from their ISO:

1. Parse ISO 9660 and read `PSP_GAME/SYSDIR/BOOT.BIN`.
2. Compare its SHA-256 with the supported version. The patches address exact
   locations, so another version would crash in confusing ways. Refuse politely.
3. Check for the free tools needed, and print the install command if any is missing.
4. Run the recompiler and compiler (about a minute, once).
5. Start the game.

## For people who don't code: the Mac app

`launcher/macos/Launcher.swift` is a window over `mku-play`, in native Swift: about
300 KB, where Tauri would add a Rust toolchain and megabytes for the same few screens.
The hard part was never the window. It was removing what a player would have to
install: `tools/build_deps.sh` builds SDL2 and an FFmpeg with only the ATRAC3 decoder
as static libraries, so the app needs just Apple's free Command Line Tools.

Testing on the machine you built on hides things. Three bugs only showed up by
imitating someone else's Mac:

- Apple's Python is 3.9 and the tools used 3.10 syntax. **Check:** with the fix, it
  generates byte-identical C to the build the game had been running on.
- `sdl2-libs.txt` held an absolute path into this machine's home folder. It linked here
  and would have failed everywhere else. It was only caught by reading the output, so
  `make_app.sh` now refuses such paths.
- A quarantined app doesn't start at all until the player approves it, which is why
  the README walks through **Open Anyway**.

## 🧑‍🎓 Exercises

1. Dump textures (`MKU_TEXDUMP=1`), upscale a few with an AI tool, and load them
   with `./mku-play --pack DIR`. Name them exactly as dumped; size must be 2, 4 or
   8 times the original.
2. NEON: the per-pixel loop still does one pixel at a time. Vectorise attribute
   stepping in `draw_triangle` and measure with `MKU_RASTER_BENCH=1 MKU_SCALE=1`.
3. Add the European version to `SUPPORTED` in `mku-play`: what else must change?
   (Hint: `config/patches.txt`, `G()` in `mk_process.c`.)
