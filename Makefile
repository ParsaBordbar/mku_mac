# mku-port build
#   make            find functions, recompile, build build/mku
#   make test       recompiler test suite
#   make run        build and run (players: use ./mku-play instead)
# Pass ELF=path/to/BOOT.elf if your dump lives elsewhere.

ELF    ?= ../work/mkunchained-psp/BOOT.elf
CC     := clang
CXX    := clang++
CFLAGS := -O2 -g -w -ffp-contract=off -fno-math-errno -Iruntime -Ibuild/recomp
RT_OPT := -O3
# DEPS=deps: the small static libraries from tools/build_deps.sh (the Mac app
# ships them, so players need no Homebrew). Otherwise, what's on this system.
ifneq ($(DEPS),)
SDL_CFLAGS := -I$(DEPS)/include/SDL2 -I$(DEPS)/include -D_THREAD_SAFE
SDL_LIBS   := -L$(DEPS)/lib $(shell cat $(DEPS)/sdl2-libs.txt)
AV_CFLAGS  := -I$(DEPS)/include
AV_LIBS    := $(DEPS)/lib/libavcodec.a $(DEPS)/lib/libavutil.a
else
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
FFMPEG     := $(shell brew --prefix ffmpeg 2>/dev/null)
ifneq ($(FFMPEG),)
AV_CFLAGS  := -I$(FFMPEG)/include
AV_LIBS    := -L$(FFMPEG)/lib -lavcodec -lavutil
else
AV_CFLAGS  := $(shell pkg-config --cflags libavcodec libavutil 2>/dev/null)
AV_LIBS    := $(shell pkg-config --libs libavcodec libavutil 2>/dev/null)
endif
endif

.PHONY: all recomp game run test clean pads

ISO    ?= ../dumps/mkunchained-psp/Mortal Kombat - Unchained (USA).iso

all: game

build/functions.csv: $(ELF) tools/find_functions.py tools/psp_elf.py
	python3 -I tools/find_functions.py $(ELF) -o $@

build/recomp/funcs.h: build/functions.csv tools/recomp.py tools/mips.py tools/program.py config/patches.txt
	python3 -I tools/recomp.py --psp $(ELF) --functions build/functions.csv --patches config/patches.txt --out build/recomp

recomp: build/recomp/funcs.h

# Second make pass so the wildcard sees the freshly generated files.
game: recomp
	@$(MAKE) --no-print-directory build/mku DEPS=$(DEPS)

GEN_C   = $(wildcard build/recomp/*.c)
RT_C    = $(wildcard runtime/*.c runtime/hle/*.c runtime/patches/*.c runtime/gpu/*.c runtime/host/*.c)
# Third-party code (fetched by tools/fetch_third_party.sh): xBRZ texture upscaler.
TP_OBJS = build/obj/tp/xbrz.o build/obj/tp/xbrz_glue.o
OBJS    = $(patsubst build/recomp/%.c,build/obj/gen/%.o,$(GEN_C)) \
          $(patsubst runtime/%.c,build/obj/rt/%.o,$(RT_C)) $(TP_OBJS)

build/mku: $(OBJS)
	$(CXX) $(OBJS) $(SDL_LIBS) $(AV_LIBS) -lm -lpthread -o $@

third_party/xbrz/xbrz.cpp third_party/stb/stb_image.h:
	@echo "missing $@ -- run tools/fetch_third_party.sh" && false

build/obj/tp/xbrz.o: third_party/xbrz/xbrz.cpp
	@mkdir -p $(@D)
	$(CXX) -O2 -std=c++17 -w -Iruntime/thirdparty -c $< -o $@

build/obj/tp/xbrz_glue.o: runtime/thirdparty/xbrz_glue.cpp third_party/xbrz/xbrz.cpp
	@mkdir -p $(@D)
	$(CXX) -O2 -std=c++17 -w -Iruntime/thirdparty -Ithird_party/xbrz -c $< -o $@

build/obj/gen/%.o: build/recomp/%.c runtime/recomp.h build/recomp/funcs.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

build/obj/rt/%.o: runtime/%.c runtime/recomp.h runtime/hle/kernel.h runtime/fiber.h runtime/gpu/gpu.h runtime/host/host.h third_party/stb/stb_image.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(RT_OPT) $(SDL_CFLAGS) $(AV_CFLAGS) -Ithird_party -Wall -Wno-unused-function -c $< -o $@

# The runtime reads game files straight from your ISO (read-only).
run: game
	MKU_ISO="$(ISO)" ./build/mku build/recomp/image.bin

test:
	python3 -I tests/recomp/run.py
	python3 -I tests/recomp/run.py -i
	@mkdir -p build && $(CC) -O2 -Wall -Iruntime tests/runtime/fiber_test.c runtime/fiber.c -o build/fiber_test && ./build/fiber_test
	@$(CC) -O2 -Wall -Iruntime tests/runtime/lighting_test.c runtime/gpu/lighting.c -lm -o build/lighting_test && ./build/lighting_test
	@python3 -I tests/tool/test_mku_play.py

# List connected game controllers (PS4/PS5/Xbox...) as SDL sees them.
pads:
	@mkdir -p build && $(CC) -O1 $(SDL_CFLAGS) tools/host/pads.c $(SDL_LIBS) -o build/pads && ./build/pads

clean:
	rm -rf build/recomp build/obj build/mku build/tests
