/* sceUmdUser, scePower, sceUtility (partial), ModuleMgrForUser, LoadExecForUser.
 * (sceAudio: audio.c, sceSasCore: sas.c) */
#include "kernel.h"

#include <string.h>

/* ---- UMD: the disc is always inserted and ready ---- */
#define UMD_READY 0x32   /* PRESENT | INITED | READY */
void hle_sceUmdActivate(Ctx *c) { TRACE("(%u, \"%s\")", A0, GSTR(A1)); RET(0); }
void hle_sceUmdGetDriveStat(Ctx *c) { RET(UMD_READY); }
void hle_sceUmdWaitDriveStat(Ctx *c) { RET(0); }
void hle_sceUmdWaitDriveStatCB(Ctx *c) { kernel_run_callbacks(c); RET(0); }
void hle_sceUmdCancelWaitDriveStat(Ctx *c) { RET(0); }

/* ---- power ---- */
void hle_scePowerSetCpuClockFrequency(Ctx *c) { TRACE("(%u MHz)", A0); RET(0); }
void hle_scePowerSetBusClockFrequency(Ctx *c) { RET(0); }
void hle_scePowerRegisterCallback(Ctx *c) { RET(0); }
void hle_scePowerUnregisterCallback(Ctx *c) { RET(0); }
void hle_scePowerIsBatteryCharging(Ctx *c) { RET(0); }

/* ---- system settings ---- */
void hle_sceUtilityGetSystemParamInt(Ctx *c) {
    uint32_t v = 0;
    switch (A0) {
    case 8: v = 1; break;   /* language: English */
    case 9: v = 1; break;   /* button preference: X = confirm */
    }
    TRACE("(%u) -> %u", A0, v);
    MEM_SW(A1, v);
    RET(0);
}

/* ---- modules: the game loads Sony's libraries from its disc. We already
 * implement those functions here, so loading just hands out an id. ---- */
void hle_sceKernelLoadModule(Ctx *c) {
    int uid = kobj_new(OBJ_MODULE, NULL);
    LOG("(\"%s\") -> 0x%X (functions provided by HLE)", GSTR(A0), uid);
    RET(uid);
}
void hle_sceKernelStartModule(Ctx *c) { RET(0); }
void hle_sceKernelStopModule(Ctx *c) { RET(0); }
void hle_sceKernelUnloadModule(Ctx *c) { kobj_free((int)A0); RET(0); }
void hle_sceKernelGetModuleId(Ctx *c) { RET(1); }
void hle_sceKernelGetModuleIdByAddress(Ctx *c) { RET(1); }

void hle_sceKernelExitGame(Ctx *c) {
    fprintf(stderr, "[kernel] sceKernelExitGame -- bye\n");
    rt_hle_report();
    exit(0);
}
void hle_sceKernelStopUnloadSelfModule(Ctx *c) { hle_sceKernelExitGame(c); }
void hle_sceKernelRegisterExitCallback(Ctx *c) { RET(0); }

