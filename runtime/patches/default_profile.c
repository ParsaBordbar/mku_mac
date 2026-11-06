/*
 * A default profile, so that every game mode just works.
 *
 * Konquest and others require a signed-in profile, and the original makes you
 * create one (name, icon, a six-button kode, twice, then a save) and then type
 * the kode at the main menu in every session. Here, instead:
 *
 *   - if there is no save yet, one is written holding a profile "PLAYER 1"
 *     whose kode is the cross button six times;
 *   - when the main menu has been up for a moment and nobody is signed in, the
 *     game's own sign-in process starts with that kode, just as the kode screen
 *     starts it (func_00161920, func_001ABD6C).
 *
 * A player's own profiles still work: an existing save is never touched, and if
 * no profile has this kode, the sign-in simply finds nothing and L still works.
 * MKU_NO_AUTOPROFILE=1 turns all of this off.
 */
#include "../hle/kernel.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "default_save.inc"

#define G(off) (rt_image_base + (off))
#define SCREEN_TOP     G(0x37576C)   /* index of the current screen... */
#define SCREEN_STACK   G(0x417D10)   /* ...in this stack */
#define SCREEN_MAIN_MENU 9
#define SIGNED_IN_P1   G(0x592848)   /* 1 once player 1 has a profile */
#define SIGNIN_STATUS  G(0x375770)   /* 0 running, 2 succeeded */
#define KODE_STATE_P1  G(0x311EE0)   /* the kode screen: 1-6 digits typed, 7 accepted; the menu reads it */
#define KODE_ACCEPTED  7
#define UI_EVENT       G(0x1343B0)   /* post a UI message */
#define UI_KODE_STATE  0x1FAA        /* "the kode state changed", to the player's screen */
#define PROC_CREATE    G(0x11E940)
#define PROC_SIGNIN    G(0x1ADCA0)
#define INPUT_READ_RA  G(0x1EE538)   /* the main loop's sceCtrlRead: not inside a process */
#define KODE_CROSS     11            /* from the table at 0x312038 */
#define MENU_SETTLE_READS 30

void default_profile_install(void) {
    if (getenv("MKU_NO_AUTOPROFILE")) return;
    const char *root = getenv("MKU_MS");
    char path[600];
    snprintf(path, sizeof path, "%s/PSP/SAVEDATA/ULUS10102MKU0000/DATA.BIN", root ? root : "build/ms0");
    struct stat st;
    if (stat(path, &st) == 0) return;
    for (char *q = path + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(path, 0755); *q = '/'; }
    uint8_t *data = calloc(1, DEFAULT_SAVE_SIZE);
    for (size_t i = 0; i < sizeof k_default_save / sizeof k_default_save[0]; i++)
        memcpy(data + k_default_save[i].offset, k_default_save[i].bytes, k_default_save[i].size);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, DEFAULT_SAVE_SIZE, f);
        fclose(f);
        fprintf(stderr, "[profile] created a default profile (PLAYER 1)\n");
    } else {
        perror(path);
    }
    free(data);
}

/* A private copy of the registers, on the game's stack but below what it's using. */
static Ctx *call_ctx(Ctx *c) {
    static Ctx t;
    t = *c;
    t.r[29] = c->r[29] - 0x200;
    return &t;
}

static int start_signin(Ctx *c) {
    uint32_t kode = mem_alloc(16, 1, 16);
    memset(rt_ptr(kode), 0, 16);
    memset(rt_ptr(kode), KODE_CROSS, 6);
    Ctx *t = call_ctx(c);
    uint32_t out = t->r[29] + 0x100;
    MEM_SW(out, 0);
    t->r[4] = 0x3009;
    t->r[5] = 0x1F;
    t->r[6] = PROC_SIGNIN;
    t->r[7] = 0x14;
    t->r[8] = out;
    rt_call_indirect(t, PROC_CREATE);
    uint32_t args = MEM_LW(out);
    if (!t->r[2] || !args) return 0;
    MEM_SW(args + 8, 0);        /* player 1 */
    MEM_SW(args + 12, 0);       /* controller 1 */
    MEM_SW(args + 16, kode);
    MEM_SW(SIGNED_IN_P1, 0);
    MEM_SW(SIGNIN_STATUS, 0);
    return 1;
}

/* Called on every controller read: drives the sign-in from the main loop. */
void default_profile_tick(Ctx *c) {
    static enum { WAIT_MENU, SIGNING_IN, DONE } state;
    static int settled, checked;
    if (!checked) {
        checked = 1;
        if (getenv("MKU_NO_AUTOPROFILE")) state = DONE;
    }
    if (state == DONE || c->r[31] != INPUT_READ_RA) return;

    if (state == WAIT_MENU) {
        int32_t top = (int32_t)MEM_LW(SCREEN_TOP);
        if (top < 0 || MEM_LW(SCREEN_STACK + 4u * (uint32_t)top) != SCREEN_MAIN_MENU) {
            settled = 0;
            return;
        }
        if (++settled < MENU_SETTLE_READS) return;
        if (MEM_LW(SIGNED_IN_P1) == 1 || !start_signin(c)) {
            state = DONE;
            return;
        }
        state = SIGNING_IN;
        return;
    }

    /* Status 1 and 3 are "no such kode" and the like: a player's own save. L still works. */
    uint32_t status = MEM_LW(SIGNIN_STATUS);
    if (status == 0) return;
    state = DONE;
    if (status != 2) return;
    /* What the kode screen does after a successful sign-in (func_00163B04). */
    MEM_SW(KODE_STATE_P1, KODE_ACCEPTED);
    Ctx *t = call_ctx(c);
    t->r[4] = UI_KODE_STATE;
    t->r[5] = 1;
    rt_call_indirect(t, UI_EVENT);
    fprintf(stderr, "[profile] signed in as PLAYER 1\n");
}
