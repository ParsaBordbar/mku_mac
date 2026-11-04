/*
 * sceGe_user: display-list execution.
 *
 * A display list is an array of 32-bit commands: command = top 8 bits,
 * data = low 24 bits. Most commands just set a register (cmd[]); a few
 * control flow (JUMP/CALL/RET), draw (PRIM) or end the list (FINISH + END).
 *
 * Lists run synchronously inside sceGeListEnQueue, up to their "stall"
 * address: games keep appending commands after enqueueing and move the stall
 * forward with sceGeListUpdateStallAddr. Semantics follow PPSSPP's
 * GPU/GPUCommon.cpp.
 */
#include "gpu.h"
#include "ge_cmds.h"
#include "../hle/kernel.h"

#include <string.h>

GeState ge;

typedef struct {
    int used, done;
    uint32_t pc, stall;
    int cbid;
    uint32_t arg;
    uint32_t stack[8], stack_offset[8];
    int sp;
    uint32_t signal;   /* behaviour of the last SIGNAL */
} GeList;

#define MAX_LISTS 64
static GeList g_lists[MAX_LISTS];

typedef struct { uint32_t signal_func, signal_arg, finish_func, finish_arg; int used; } GeCallback;
static GeCallback g_callbacks[16];

static uint64_t g_cmd_count[256];
static int g_stats = -1;

/* Callbacks run like interrupt handlers: own registers, own stack, no rescheduling. */
extern int g_in_interrupt;
static uint32_t g_cb_stack;

static void call_handler(uint32_t func, uint32_t a0, uint32_t a1) {
    if (!func) return;
    gpu_flush();   /* game code runs: finish queued drawing first */
    if (!g_cb_stack) g_cb_stack = mem_alloc(0x4000, 1, 256) + 0x4000;
    Ctx ic;
    rt_ctx_init(&ic);
    ic.r[4] = a0;
    ic.r[5] = a1;
    ic.r[29] = g_cb_stack - 16;
    ic.r[28] = g_current ? g_current->ctx.r[28] : 0;
    int was = g_in_interrupt;
    static RtShadow cb_shadow;
    RtShadow *saved = rt_shadow;
    rt_shadow = &cb_shadow;
    cb_shadow.depth = 0;
    g_in_interrupt = 1;
    rt_call_indirect(&ic, func);
    g_in_interrupt = was;
    rt_shadow = saved;
}

static void load_matrix_value(float *m, uint32_t *idx, uint32_t size, uint32_t data) {
    m[*idx % size] = ge_float(data);
    *idx = (*idx + 1) % size;
}

/* Execute until END+FINISH, or the stall address. */
static void run_list(GeList *l) {
    if (g_stats < 0) g_stats = getenv("MKU_GE_STATS") != NULL;
    while (!l->done && (l->stall == 0 || l->pc != l->stall)) {
        uint32_t op = MEM_LW(l->pc), cmd = op >> 24, data = op & 0xFFFFFF;
        uint32_t here = l->pc;
        l->pc += 4;
        g_cmd_count[cmd]++;
        switch (cmd) {
        case GE_NOP: break;
        case GE_VADDR: ge.vaddr = ge_address(data); break;
        case GE_IADDR: ge.iaddr = ge_address(data); break;
        case GE_OFFSETADDR: ge.offset_addr = data << 8; break;
        case GE_ORIGIN: ge.offset_addr = here; break;
        case GE_JUMP: l->pc = ge_address(data & 0xFFFFFC); break;
        case GE_BJUMP: break;   /* bounding box tests always pass: never jump */
        case GE_CALL:
            if (l->sp < 8) {
                l->stack[l->sp] = l->pc;
                l->stack_offset[l->sp++] = ge.offset_addr;
                l->pc = ge_address(data & 0xFFFFFC);
            }
            break;
        case GE_RET:
            if (l->sp > 0) {
                l->sp--;
                l->pc = l->stack[l->sp];
                ge.offset_addr = l->stack_offset[l->sp];
            }
            break;
        case GE_SIGNAL:
        case GE_FINISH:
            ge.cmd[cmd] = data;
            break;
        case GE_END: {
            uint32_t prev = MEM_LW(here - 4), pcmd = prev >> 24;
            if (pcmd == GE_FINISH) {
                l->done = 1;
                if (l->cbid >= 0 && g_callbacks[l->cbid].used)
                    call_handler(g_callbacks[l->cbid].finish_func, prev & 0xFFFF, g_callbacks[l->cbid].finish_arg);
            } else if (pcmd == GE_SIGNAL) {
                uint32_t behaviour = (prev >> 16) & 0xFF, sig = prev & 0xFFFF, enddata = data & 0xFFFF;
                uint32_t target = ((sig << 16) | enddata) & 0xFFFFFFFCu;
                l->signal = behaviour;
                switch (behaviour) {
                case 0x01: case 0x02:   /* handler suspend / continue: call the signal handler */
                    if (l->cbid >= 0 && g_callbacks[l->cbid].used)
                        call_handler(g_callbacks[l->cbid].signal_func, sig, g_callbacks[l->cbid].signal_arg);
                    break;
                case 0x10: l->pc = target; break;                          /* jump */
                case 0x13: l->pc = target + here; break;                   /* relative jump */
                case 0x15: l->pc = ge_address(target); break;              /* origin jump */
                case 0x11: case 0x14: case 0x16:                           /* call variants */
                    if (l->sp < 8) {
                        l->stack[l->sp] = l->pc;
                        l->stack_offset[l->sp++] = ge.offset_addr;
                        l->pc = behaviour == 0x11 ? target : behaviour == 0x14 ? target + here : ge_address(target);
                    }
                    break;
                case 0x12:                                                 /* return */
                    if (l->sp > 0) { l->sp--; l->pc = l->stack[l->sp]; ge.offset_addr = l->stack_offset[l->sp]; }
                    break;
                default: break;   /* pause / sync: nothing to do when running synchronously */
                }
            }
            break;
        }
        case GE_PRIM:
            ge.cmd[cmd] = data;
            gpu_draw_prim(data);
            break;
        case GE_TEXFLUSH:   /* the game changed texture memory: re-check cached textures */
            gpu_texture_flush();
            break;
        case GE_LOADCLUT:   /* the palette is copied now: games reuse its memory right after */
            ge.cmd[cmd] = data;
            gpu_load_clut();
            break;
        case GE_TRANSFERSTART:
            ge.cmd[cmd] = data;
            gpu_block_transfer(data);
            break;
        /* matrix uploads: a "number" command picks the slot, "data" commands fill it */
        case GE_WORLDMATRIXNUMBER: ge.world_i = data & 0xF; break;
        case GE_WORLDMATRIXDATA: load_matrix_value(ge.world, &ge.world_i, 12, data); break;
        case GE_VIEWMATRIXNUMBER: ge.view_i = data & 0xF; break;
        case GE_VIEWMATRIXDATA: load_matrix_value(ge.view, &ge.view_i, 12, data); break;
        case GE_PROJMATRIXNUMBER: ge.proj_i = data & 0xF; break;
        case GE_PROJMATRIXDATA: load_matrix_value(ge.proj, &ge.proj_i, 16, data); break;
        case GE_TGENMATRIXNUMBER: ge.tgen_i = data & 0xF; break;
        case GE_TGENMATRIXDATA: load_matrix_value(ge.tgen, &ge.tgen_i, 12, data); break;
        case GE_BONEMATRIXNUMBER: ge.bone_i = data & 0x7F; break;
        case GE_BONEMATRIXDATA: load_matrix_value(ge.bone, &ge.bone_i, 96, data); break;
        default:
            if (cmd >= GE_MORPHWEIGHT0 && cmd <= GE_MORPHWEIGHT0 + 7) ge.morph[cmd - GE_MORPHWEIGHT0] = ge_float(data);
            ge.cmd[cmd] = data;
            break;
        }
    }
    /* Queued drawing stays queued: like the real GE, drawing is asynchronous, and
     * the CPU must sync (sceGeDrawSync, sceGeListSync) before it looks at the result.
     * The game feeds lists piecemeal through stall updates, so flushing here
     * would mean dozens of tiny batches per frame. */
}

void gpu_stats_report(void) {
    if (g_stats <= 0) return;
    fprintf(stderr, "[ge] command counts:\n");
    for (int i = 0; i < 256; i++)
        if (g_cmd_count[i]) fprintf(stderr, "  %02X %-22s %llu\n", i, ge_cmd_names[i] ? ge_cmd_names[i] : "?", (unsigned long long)g_cmd_count[i]);
}

/* ------------------------------------------------------------------------ */
/* sceGe_user */

static int enqueue(Ctx *c) {
    uint32_t list = A0 & 0x0FFFFFFF, stall = A1 & 0x0FFFFFFF;
    int cbid = (int)A2;
    for (int i = 0; i < MAX_LISTS; i++) {
        if (!g_lists[i].used || g_lists[i].done) {
            g_lists[i] = (GeList){.used = 1, .pc = list, .stall = stall, .cbid = cbid < 16 ? cbid : -1};
            if (A3) g_lists[i].arg = MEM_LW(A3);   /* PspGeListArgs, unused for now */
            run_list(&g_lists[i]);
            return i;
        }
    }
    LOG(": too many display lists");
    return -1;
}

void hle_sceGeListEnQueue(Ctx *c) {
    TRACE("(list=0x%08X, stall=0x%08X, cbid=%d)", A0, A1, (int)A2);
    RET(enqueue(c));
}
void hle_sceGeListEnQueueHead(Ctx *c) { hle_sceGeListEnQueue(c); }

void hle_sceGeListUpdateStallAddr(Ctx *c) {
    if (A0 >= MAX_LISTS || !g_lists[A0].used) RET(0x80000100u);
    g_lists[A0].stall = A1 & 0x0FFFFFFF;
    run_list(&g_lists[A0]);
    RET(0);
}

/* Lists finish synchronously, so syncing never has to wait. */
void hle_sceGeListSync(Ctx *c) { gpu_flush(); RET(0); }   /* 0 = completed */
void hle_sceGeDrawSync(Ctx *c) { gpu_flush(); RET(0); }
void hle_sceGeBreak(Ctx *c) { RET(0); }
void hle_sceGeContinue(Ctx *c) { RET(0); }

void hle_sceGeEdramGetAddr(Ctx *c) { RET(VRAM_BASE); }
void hle_sceGeEdramGetSize(Ctx *c) { RET(VRAM_SIZE); }
void hle_sceGeEdramSetAddrTranslation(Ctx *c) { RET(0x400); }

void hle_sceGeGetCmd(Ctx *c) { RET(A0 < 256 ? ge.cmd[A0] | (A0 << 24) : 0); }

/* The context is the register state; the game saves and restores it around
 * its own lists (e.g. for overlays). */
void hle_sceGeSaveContext(Ctx *c) { memcpy(rt_ptr(A0), ge.cmd, 512 * 4 < sizeof ge.cmd ? 512 * 4 : sizeof ge.cmd); RET(0); }
void hle_sceGeRestoreContext(Ctx *c) { memcpy(ge.cmd, rt_ptr(A0), sizeof ge.cmd); RET(0); }

void hle_sceGeSetCallback(Ctx *c) {
    for (int i = 0; i < 16; i++) {
        if (!g_callbacks[i].used) {
            /* PspGeCallbackData: signal_func, signal_arg, finish_func, finish_arg */
            g_callbacks[i] = (GeCallback){MEM_LW(A0), MEM_LW(A0 + 4), MEM_LW(A0 + 8), MEM_LW(A0 + 12), 1};
            TRACE("(signal=0x%08X, finish=0x%08X) -> %d", g_callbacks[i].signal_func, g_callbacks[i].finish_func, i);
            RET(i);
        }
    }
    RET(0x80000022u);
}

void hle_sceGeUnsetCallback(Ctx *c) {
    if (A0 < 16) g_callbacks[A0].used = 0;
    RET(0);
}
