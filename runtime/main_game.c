/* Host entry point: set up PSP memory and the kernel, then call the game's
 * _start natively on the "root" thread. */
#include "hle/kernel.h"
#include "host/host.h"

void profile_start(void);
void surf_init(void);

#include <stdio.h>
#include <string.h>

#define STACK_TOP 0x09FFF000u   /* top of PSP user memory, minus room for argv */
#define ARGV_ADDR 0x09FFF100u

int main(int argc, char **argv) {
    const char *image = argc > 1 ? argv[1] : "build/recomp/image.bin";

    setvbuf(stdout, NULL, _IONBF, 0);   /* keep [main] and [hle] lines in order */
    rt_init_memory();
    if (rt_load_image(image)) return 1;
    surf_init();
    host_init();
    kernel_init();
    profile_start();
    sysmem_init();

    /* The PSP kernel starts a module with a0 = size of the argument block and
     * a1 = pointer to it; for a game the argument is the executable's path. */
    const char *path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    strcpy((char *)rt_ptr(ARGV_ADDR), path);

    Thread *root = kernel_root_thread();
    Ctx *c = &root->ctx;
    c->r[29] = STACK_TOP;
    c->r[4] = (uint32_t)strlen(path) + 1;
    c->r[5] = ARGV_ADDR;

    fprintf(stderr, "[main] image 0x%08X-0x%08X, calling _start at 0x%08X\n",
            rt_image_base, rt_image_base + rt_image_size, rt_entry);
    rt_call_indirect(c, rt_entry);
    fprintf(stderr, "[main] _start returned v0=0x%X, handing over to the game's threads\n", c->r[2]);
    kernel_root_finished();
    host_run();   /* window + input on the main thread; never returns */
}
