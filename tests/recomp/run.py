#!/usr/bin/env python3
"""Recompiler test suite: native C vs. recompiled MIPS, same inputs, same answers.

For every tests/recomp/cases/*.c and every optimization level:
  1. clang compiles it for MIPS II and ld.lld links it at 0x08804000
  2. tools/recomp.py turns the MIPS ELF into C
  3. a generated harness calls each `t_*` function twice per input vector --
     the native build (compiled for the Mac directly) and the recompiled one --
     and compares the return values

usage: python3 -I tests/recomp/run.py [-k name] [-v]
needs: brew install llvm lld
"""

import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import program
import recomp

LLVM = "/opt/homebrew/opt/llvm/bin"
LLD = "/opt/homebrew/opt/lld/bin/ld.lld"
MIPS_CFLAGS = ["--target=mipsel-none-elf", "-march=mips2", "-ffreestanding", "-fno-pic", "-mno-abicalls",
               "-G0", "-mhard-float", "-msingle-float", "-mfp32", "-fno-math-errno", "-ffp-contract=off",
               "-fno-jump-tables", "-mno-check-zero-division", "-fno-builtin"]
OPT_LEVELS = ["-O0", "-O2"]
VECTORS = 200
INTERP = False

HARNESS = r"""
#include "funcs.h"
#include <stdio.h>
#include <stdlib.h>

%(decls)s

static const struct {
    const char *name;
    uint32_t (*native)(uint32_t, uint32_t, uint32_t, uint32_t);
    RecompFn rc;
    uint32_t addr;
} tests[] = {
%(rows)s
};

static const uint32_t interesting[] = {
    0, 1, 2, 3, 7, 15, 16, 31, 32, 100, 255, 1000, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
    0x12345678, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF, 0xDEADBEEF, 1234567,
};
#define N_INTERESTING (sizeof interesting / sizeof interesting[0])

int main(int argc, char **argv) {
    rt_init_memory();
    if (argc < 2 || rt_load_image(argv[1])) return 2;
    uint32_t seed = 12345;
    int failed_tests = 0;
    for (unsigned t = 0; t < sizeof tests / sizeof tests[0]; t++) {
        int fails = 0;
        for (int k = 0; k < %(vectors)d; k++) {
            uint32_t a[4];
            for (int j = 0; j < 4; j++) {
                seed = seed * 1103515245u + 12345u;
                a[j] = (seed >> 8) & 1 ? interesting[(seed >> 16) %% N_INTERESTING] : seed;
            }
            Ctx c;
            rt_ctx_init(&c);
            c.r[29] = 0x09F00000u;   /* stack pointer, top of user memory */
            c.r[4] = a[0]; c.r[5] = a[1]; c.r[6] = a[2]; c.r[7] = a[3];
            if (getenv("RT_INTERP")) {   /* interpreter mode: same test, interpreted */
                rt_interp_only = 1;
                rt_interpret(&c, tests[t].addr);
            } else {
                tests[t].rc(&c);
            }
            uint32_t want = tests[t].native(a[0], a[1], a[2], a[3]);
            if (c.r[2] != want && fails++ < 3)
                printf("  FAIL %%s(0x%%X, 0x%%X, 0x%%X, 0x%%X): recompiled 0x%%X, native 0x%%X\n",
                       tests[t].name, a[0], a[1], a[2], a[3], c.r[2], want);
        }
        if (fails) failed_tests++;
        printf("  %%-16s %%s\n", tests[t].name, fails ? "FAIL" : "ok");
    }
    return failed_tests ? 1 : 0;
}
"""


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0 and kw.get("check_ok", True):
        print("$ " + " ".join(cmd))
        print(r.stdout + r.stderr)
    return r


def build_case(src: str, opt: str, out: str, verbose: bool) -> bool:
    os.makedirs(out, exist_ok=True)
    objs = []
    for c_file in (src, os.path.join(ROOT, "tests/recomp/support.c")):
        obj = os.path.join(out, os.path.basename(c_file) + ".o")
        if run([f"{LLVM}/clang", *MIPS_CFLAGS, opt, "-c", c_file, "-o", obj]).returncode:
            return False
        objs.append(obj)
    elf = os.path.join(out, "test.elf")
    if run([LLD, "-static", "-Ttext=0x08804000", "-o", elf, *objs]).returncode:
        return False

    prog = program.from_elf(elf)
    stats = recomp.generate(prog, os.path.join(out, "rc"))
    if verbose:
        print(f"    recomp: {dict(stats)}")

    tests = sorted(n for _, n in prog.functions.values() if n.startswith("t_"))
    decls = "\n".join(f"uint32_t {t}(uint32_t, uint32_t, uint32_t, uint32_t);" for t in tests)
    addr_of = {n: a for a, (_, n) in prog.functions.items()}
    rows = "\n".join(f'    {{"{t}", {t}, rc_{t}, 0x{addr_of[t]:08X}u}},' for t in tests)
    harness = os.path.join(out, "harness.c")
    with open(harness, "w") as f:
        f.write(HARNESS % {"decls": decls, "rows": rows, "vectors": VECTORS})

    exe = os.path.join(out, "harness")
    rc_dir = os.path.join(out, "rc")
    cmd = ["clang", "-O1", "-w", "-ffp-contract=off", "-fno-math-errno", f"-I{ROOT}/runtime", f"-I{rc_dir}", harness, src,
           os.path.join(ROOT, "runtime/runtime.c"), os.path.join(ROOT, "runtime/vfpu.c"), os.path.join(ROOT, "runtime/interp.c"), *glob.glob(os.path.join(rc_dir, "*.c")), "-o", exe]
    if run(cmd).returncode:
        return False
    env = dict(os.environ)
    if INTERP:
        env["RT_INTERP"] = "1"
    r = run([exe, os.path.join(rc_dir, "image.bin")], env=env)
    print(r.stdout, end="")
    if r.stderr:
        print(r.stderr, end="")
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", help="only cases whose name contains this")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("-i", action="store_true", help="test the interpreter instead of the recompiled code")
    args = ap.parse_args()
    global INTERP
    INTERP = args.i

    results = []
    for src in sorted(glob.glob(os.path.join(ROOT, "tests/recomp/cases/*.c"))):
        name = os.path.splitext(os.path.basename(src))[0]
        if args.k and args.k not in name:
            continue
        for opt in OPT_LEVELS:
            print(f"{name} {opt}")
            ok = build_case(src, opt, os.path.join(ROOT, "build/tests", f"{name}{opt}"), args.v)
            results.append((f"{name} {opt}", ok))

    passed = sum(ok for _, ok in results)
    print(f"\n{passed}/{len(results)} test programs passed")
    for name, ok in results:
        if not ok:
            print(f"  FAILED: {name}")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
