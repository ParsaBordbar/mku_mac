/*
 * sceSasCore: the PSP's software sound mixer ("SAS"), used by Sony's SCREAM
 * sound engine for sound effects and music voices.
 *
 * 32 voices. Each plays a VAG sample (PSP/PS1 ADPCM) or raw PCM at some
 * pitch, shaped by an ADSR volume envelope, and __sceSasCore mixes one
 * "grain" (e.g. 256 samples) of all voices into a stereo buffer that the game
 * then sends to sceAudio. Behaviour follows PPSSPP's Core/HW/SasAudio.cpp;
 * reverb ("effects") and noise voices are not implemented (silent).
 */
#include "kernel.h"

#include <string.h>

#define VOICES 32
#define PITCH_BASE 0x1000
#define ENV_MAX 0x40000000LL

enum Curve { LIN_INC, LIN_DEC, LIN_BENT, EXP_DEC, EXP_INC, DIRECT };
enum EnvState { E_OFF, E_ATTACK, E_DECAY, E_SUSTAIN, E_RELEASE };

typedef struct {
    int64_t height;
    enum EnvState state;
    int a_rate, d_rate, s_rate, r_rate, s_level;
    enum Curve a_type, d_type, s_type, r_type;
} Envelope;

typedef struct {
    int type;                  /* 0 none, 1 VAG, 2 PCM */
    int on, playing, paused, loop;
    uint32_t addr, size;
    int pitch, vol_l, vol_r;
    Envelope env;
    /* VAG decoder */
    uint32_t read;
    int block, nblocks, cur, end, loop_block, loop_next, s1, s2;
    int16_t dec[28];
    /* PCM */
    uint32_t pcm_index, pcm_loop;
    /* resampling */
    uint32_t frac;
    int16_t hist[2];
} Voice;

static Voice g_v[VOICES];
static int g_grain = 256, g_output_mode;

/* ---- envelope (PPSSPP's rate/curve formulas) ---- */

static int simple_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = ((7 - (n & 3)) << 26) >> (n >> 2);
    return r ? r : 1;
}
static int exponent_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = ((7 - (n & 3)) << 24) >> (n >> 2);
    return r ? r : 1;
}

static void env_simple(Envelope *e, uint32_t env1, uint32_t env2) {
    e->a_rate = simple_rate((int)(env1 >> 8));
    e->a_type = (env1 & 0x8000) ? LIN_BENT : LIN_INC;
    int dn = (env1 >> 4) & 0xF;
    e->d_rate = dn ? (int)(0x80000000u >> dn) : 0x7FFFFFFF;
    e->d_type = EXP_DEC;
    e->s_type = (enum Curve)((env2 >> 14) & 3);
    e->s_rate = e->s_type == EXP_DEC ? exponent_rate((int)(env2 >> 6)) : simple_rate((int)(env2 >> 6));
    e->r_type = (env2 & 0x20) ? EXP_DEC : LIN_DEC;
    int rn = env2 & 0x1F;
    if (rn == 31) e->r_rate = 0;
    else if (e->r_type == LIN_DEC) e->r_rate = rn == 30 ? 0x40000000 : rn == 29 ? 1 : 0x10000000 >> rn;
    else e->r_rate = rn ? (int)(0x80000000u >> rn) : 0x7FFFFFFF;
    e->s_level = ((env1 & 0xF) + 1) << 26;
}

static void env_walk(Envelope *e, enum Curve type, int rate) {
    int64_t d;
    switch (type) {
    case LIN_INC: e->height += rate; break;
    case LIN_DEC: e->height -= rate; break;
    case LIN_BENT: e->height += e->height <= ENV_MAX * 3 / 4 ? rate : rate / 4; break;
    case EXP_DEC:
        d = e->height - ENV_MAX;
        d += (-d * rate) >> 32;
        e->height = d + ENV_MAX - ((int64_t)rate + 3) / 4;
        break;
    case EXP_INC:
        d = e->height - ENV_MAX;
        d += (-d * rate) >> 32;
        e->height = d + 0x4000 + ENV_MAX;
        break;
    default: e->height = rate; break;
    }
}

static void env_set(Envelope *e, enum EnvState s) {
    if (e->height > ENV_MAX) e->height = ENV_MAX;
    e->state = s;
}

static void env_step(Envelope *e) {
    switch (e->state) {
    case E_ATTACK:
        env_walk(e, e->a_type, e->a_rate);
        if (e->height >= ENV_MAX || e->height < 0) env_set(e, E_DECAY);
        break;
    case E_DECAY:
        env_walk(e, e->d_type, e->d_rate);
        if (e->height < e->s_level) env_set(e, E_SUSTAIN);
        break;
    case E_SUSTAIN:
        env_walk(e, e->s_type, e->s_rate);
        if (e->height <= 0) { e->height = 0; env_set(e, E_RELEASE); }
        break;
    case E_RELEASE:
        env_walk(e, e->r_type, e->r_rate);
        if (e->height <= 0) { e->height = 0; env_set(e, E_OFF); }
        break;
    case E_OFF: break;
    }
}

/* ---- VAG: 16-byte blocks of 28 4-bit ADPCM samples ---- */

static const int k_vag_coef[16][2] = {
    {0, 0}, {60, 0}, {115, 52}, {98, 55}, {122, 60},
};

static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static void vag_start(Voice *v) {
    v->read = v->addr;
    v->nblocks = (int)(v->size / 16);
    v->block = -1;
    v->cur = 28;
    v->end = 0;
    v->loop_block = -1;
    v->loop_next = 0;
    v->s1 = v->s2 = 0;
}

static void vag_block(Voice *v) {
    if (v->block == v->nblocks - 1) { v->end = 1; return; }
    uint32_t p = v->read;
    int predict = MEM_LBU(p), flags = MEM_LBU(p + 1);
    int shift = predict & 0xF;
    predict >>= 4;
    if (flags == 7) { v->end = 1; return; }
    if (flags == 6) v->loop_block = v->block;
    else if (flags == 3 && v->loop) v->loop_next = 1;
    int c1 = k_vag_coef[predict & 15][0], c2 = -k_vag_coef[predict & 15][1];
    int s1 = v->s1, s2 = v->s2;
    for (int i = 0; i < 28; i += 2) {
        uint8_t d = MEM_LBU(p + 2 + (uint32_t)i / 2);
        int a = (int16_t)((d & 0xF) << 12) >> shift;
        int b = (int16_t)((d & 0xF0) << 8) >> shift;
        s2 = clamp16(a + ((s1 * c1 + s2 * c2) >> 6));
        s1 = clamp16(b + ((s2 * c1 + s1 * c2) >> 6));
        v->dec[i] = (int16_t)s2;
        v->dec[i + 1] = (int16_t)s1;
    }
    v->s1 = s1;
    v->s2 = s2;
    v->cur = 0;
    v->block++;
    v->read += 16;
}

static int16_t next_sample(Voice *v) {
    if (v->type == 2) {   /* PCM */
        if (v->pcm_index >= v->size) {
            if (!v->loop) return 0;
            v->pcm_index = v->pcm_loop;
        }
        return (int16_t)MEM_LHU(v->addr + 2 * v->pcm_index++);
    }
    if (v->end) return 0;
    if (v->cur == 28) {
        if (v->loop_next) {
            v->read = v->addr + 16u * (uint32_t)v->loop_block + 16;
            v->block = v->loop_block;
            v->loop_next = 0;
        }
        vag_block(v);
        if (v->end) return 0;
    }
    return v->dec[v->cur++];
}

static int samples_ended(const Voice *v) {
    return v->type == 1 ? v->end : v->type == 2 ? (!v->loop && v->pcm_index >= v->size) : 0;
}

/* ---- mixing ---- */

static void mix_voice(Voice *v, int32_t *mix) {
    for (int i = 0; i < g_grain; i++) {
        /* resample with linear interpolation; pitch 0x1000 = 1:1 at 44.1 kHz */
        while (v->frac >= PITCH_BASE) {
            v->hist[0] = v->hist[1];
            v->hist[1] = next_sample(v);
            v->frac -= PITCH_BASE;
        }
        int f = (int)(v->frac & 0xFFF);
        int s = v->hist[0] + (((v->hist[1] - v->hist[0]) * f) >> 12);
        v->frac += (uint32_t)v->pitch;
        int env = (int)((v->env.height + (1 << 14)) >> 15);
        env_step(&v->env);
        s = ((s * env) + (1 << 14)) >> 15;
        mix[2 * i] += (s * v->vol_l) >> 12;
        mix[2 * i + 1] += (s * v->vol_r) >> 12;
    }
    if (samples_ended(v)) { v->env.state = E_OFF; v->env.height = 0; }
    if (v->env.state == E_OFF) { v->playing = 0; v->on = 0; }
}

static void sas_mix(uint32_t out, uint32_t in, int lvol, int rvol) {
    static int32_t mix[2048 * 2];
    memset(mix, 0, (size_t)g_grain * 2 * sizeof mix[0]);
    for (int i = 0; i < VOICES; i++)
        if (g_v[i].playing && !g_v[i].paused) mix_voice(&g_v[i], mix);
    if (g_output_mode == 0) {   /* mixed: interleaved stereo */
        for (int i = 0; i < g_grain * 2; i++) {
            int s = mix[i];
            if (in) s += ((int16_t)MEM_LHU(in + 2u * (uint32_t)i) * (i & 1 ? rvol : lvol)) >> 12;
            MEM_SH(out + 2u * (uint32_t)i, (uint32_t)(uint16_t)clamp16(s));
        }
    } else {   /* raw: four planes (L, R, effect L, effect R) */
        for (int i = 0; i < g_grain; i++) {
            MEM_SH(out + 2u * (uint32_t)i, (uint16_t)clamp16(mix[2 * i]));
            MEM_SH(out + 2u * (uint32_t)(g_grain + i), (uint16_t)clamp16(mix[2 * i + 1]));
            MEM_SH(out + 2u * (uint32_t)(2 * g_grain + i), 0);
            MEM_SH(out + 2u * (uint32_t)(3 * g_grain + i), 0);
        }
    }
}

/* ---- API ---- */

#define VOICE(n) do { if ((int)(n) < 0 || (n) >= VOICES) RET(0x80420010u); } while (0)

void hle___sceSasInit(Ctx *c) {
    g_grain = A1 > 0 && A1 <= 2048 ? (int)A1 : 256;
    g_output_mode = (int)A3;
    memset(g_v, 0, sizeof g_v);
    TRACE("(grain=%u, voices=%u, mode=%u, rate=%u)", A1, A2, A3, T0);
    RET(0);
}

void hle___sceSasCore(Ctx *c) { sas_mix(A1, 0, 0, 0); RET(0); }
void hle___sceSasCoreWithMix(Ctx *c) { sas_mix(A1, A1, (int)A2, (int)A3); RET(0); }

void hle___sceSasSetVoice(Ctx *c) {   /* core, voice, vag address, size, loop */
    VOICE(A1);
    Voice *v = &g_v[A1];
    int changed = v->type != 1 || v->addr != A2 || v->size != A3 || v->loop != (int)(T0 != 0);
    v->type = 1;
    v->addr = A2;
    v->size = A3;
    v->loop = T0 != 0;
    if (v->on) v->playing = 1;
    if (changed) vag_start(v);
    RET(0);
}

void hle___sceSasSetVoicePCM(Ctx *c) {   /* core, voice, pcm address, samples, loop position */
    VOICE(A1);
    Voice *v = &g_v[A1];
    v->type = 2;
    v->addr = A2;
    v->size = A3;
    v->pcm_index = 0;
    v->loop = (int32_t)T0 >= 0;
    v->pcm_loop = v->loop ? T0 : 0;
    v->playing = 1;
    RET(0);
}

void hle___sceSasSetPitch(Ctx *c) { VOICE(A1); g_v[A1].pitch = (int)A2; RET(0); }

void hle___sceSasSetVolume(Ctx *c) {   /* core, voice, left, right, effect left, effect right */
    VOICE(A1);
    g_v[A1].vol_l = (int)A2;
    g_v[A1].vol_r = (int)A3;
    RET(0);
}

void hle___sceSasSetKeyOn(Ctx *c) {
    VOICE(A1);
    Voice *v = &g_v[A1];
    if (v->type == 1) vag_start(v);
    if (v->type == 2) v->pcm_index = 0;
    v->env.height = 0;
    v->env.state = E_ATTACK;
    v->frac = 0;
    v->hist[0] = v->hist[1] = 0;
    v->playing = v->on = 1;
    v->paused = 0;
    RET(0);
}

void hle___sceSasSetKeyOff(Ctx *c) {
    VOICE(A1);
    g_v[A1].on = 0;
    env_set(&g_v[A1].env, E_RELEASE);
    RET(0);
}

void hle___sceSasSetSL(Ctx *c) { VOICE(A1); g_v[A1].env.s_level = (int)A2; RET(0); }

void hle___sceSasSetADSR(Ctx *c) {   /* core, voice, flag, attack, decay, sustain, release (rates) */
    VOICE(A1);
    Envelope *e = &g_v[A1].env;
    if (A2 & 1) e->a_rate = (int)A3;
    if (A2 & 2) e->d_rate = (int)T0;
    if (A2 & 4) e->s_rate = (int)T1;
    if (A2 & 8) e->r_rate = (int)c->r[10];
    RET(0);
}

void hle___sceSasSetADSRmode(Ctx *c) {   /* same layout, curve types */
    VOICE(A1);
    Envelope *e = &g_v[A1].env;
    if (A2 & 1) e->a_type = (enum Curve)A3;
    if (A2 & 2) e->d_type = (enum Curve)T0;
    if (A2 & 4) e->s_type = (enum Curve)T1;
    if (A2 & 8) e->r_type = (enum Curve)c->r[10];
    RET(0);
}

void hle___sceSasSetSimpleADSR(Ctx *c) { VOICE(A1); env_simple(&g_v[A1].env, A2, A3); RET(0); }

void hle___sceSasGetEnvelopeHeight(Ctx *c) { VOICE(A1); RET((uint32_t)g_v[A1].env.height); }

void hle___sceSasGetEndFlag(Ctx *c) {
    uint32_t f = 0;
    for (int i = 0; i < VOICES; i++) if (!g_v[i].playing) f |= 1u << i;
    RET(f);
}

void hle___sceSasGetPauseFlag(Ctx *c) {
    uint32_t f = 0;
    for (int i = 0; i < VOICES; i++) if (g_v[i].paused) f |= 1u << i;
    RET(f);
}

void hle___sceSasSetPause(Ctx *c) {
    for (int i = 0; i < VOICES; i++) if (A1 & (1u << i)) g_v[i].paused = A2 != 0;
    RET(0);
}

/* Not implemented yet: noise voices and the reverb unit. */
void hle___sceSasSetNoise(Ctx *c) { RET(0); }
void hle___sceSasRevType(Ctx *c) { RET(0); }
void hle___sceSasRevParam(Ctx *c) { RET(0); }
void hle___sceSasRevEVOL(Ctx *c) { RET(0); }
void hle___sceSasRevVON(Ctx *c) { RET(0); }
