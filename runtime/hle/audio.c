/*
 * sceAudio: the PSP's 8 PCM output channels (16-bit, 44.1 kHz).
 *
 * Samples go to the host sound card (host/audio.c), which mixes the channels.
 * A "blocking" output waits while the channel still has more than about one
 * buffer queued, so the game's audio thread is paced by the real sound card,
 * exactly like on the PSP. Without a sound device (MKU_HEADLESS) the same
 * pacing is simulated with timers.
 */
#include "kernel.h"
#include "../host/host.h"

#include <string.h>

#define AUDIO_RATE 44100u
#define VOLUME_MAX 0x8000
#define FORMAT_MONO 0x10

static struct { int reserved; uint32_t samples, format; uint64_t busy_until; } g_ch[8];

void hle_sceAudioChReserve(Ctx *c) {
    int ch = (int)A0;
    if (ch < 0) {
        for (ch = 0; ch < 8 && g_ch[ch].reserved; ch++) {}
        if (ch == 8) RET(0x80260002u);   /* no free channel */
    }
    if (ch >= 8) RET(ERR_ILLEGAL_ARGUMENT);
    g_ch[ch].reserved = 1;
    g_ch[ch].samples = A1;
    g_ch[ch].format = A2;
    TRACE("(%d, %u samples, %s) -> %d", (int)A0, A1, A2 == FORMAT_MONO ? "mono" : "stereo", ch);
    RET(ch);
}

void hle_sceAudioChRelease(Ctx *c) { if (A0 < 8) g_ch[A0].reserved = 0; RET(0); }
void hle_sceAudioSetChannelDataLen(Ctx *c) { if (A0 < 8) g_ch[A0].samples = A1; RET(0); }
void hle_sceAudioChangeChannelConfig(Ctx *c) { if (A0 < 8) g_ch[A0].format = A1; RET(0); }
void hle_sceAudioChangeChannelVolume(Ctx *c) { RET(0); }

/* MKU_WAV=1: also write every channel's samples to build/audio_chN.raw
 * (16-bit stereo 44.1 kHz) to check sound without listening. */
static void capture(int ch, uint32_t buf, int n, int mono) {
    static int enabled = -1;
    static FILE *f[8];
    if (enabled < 0) enabled = getenv("MKU_WAV") != NULL;
    if (!enabled) return;
    if (!f[ch]) {
        char path[64];
        snprintf(path, sizeof path, "build/audio_ch%d.raw", ch);
        f[ch] = fopen(path, "wb");
    }
    if (!f[ch]) return;
    for (int i = 0; i < n; i++) {
        uint16_t l = MEM_LHU(buf + (mono ? 2u : 4u) * (uint32_t)i), r = mono ? l : MEM_LHU(buf + 4u * (uint32_t)i + 2);
        fwrite(&l, 2, 1, f[ch]);
        fwrite(&r, 2, 1, f[ch]);
    }
    fflush(f[ch]);
}

static void audio_output(Ctx *c, int ch, int vol_l, int vol_r, uint32_t buf, int blocking) {
    if (ch < 0 || ch >= 8 || !g_ch[ch].reserved) RET(ERR_ILLEGAL_ARGUMENT);
    int n = (int)g_ch[ch].samples;
    capture(ch, buf, n, g_ch[ch].format == FORMAT_MONO);

    if (host_audio_enabled()) {
        /* wait until at most one buffer is still queued on this channel */
        int queued;
        while (blocking && (queued = host_audio_queued(ch)) > n)
            sched_wait(g_current, WAIT_DELAY, 0, (uint64_t)(queued - n) * 1000000u / AUDIO_RATE + 1, 0);
        static int16_t tmp[4096 * 2];
        if (n > 4096) n = 4096;
        int mono = g_ch[ch].format == FORMAT_MONO;
        for (int i = 0; i < n; i++) {
            int l, r;
            if (mono) l = r = (int16_t)MEM_LHU(buf + 2u * (uint32_t)i);
            else { l = (int16_t)MEM_LHU(buf + 4u * (uint32_t)i); r = (int16_t)MEM_LHU(buf + 4u * (uint32_t)i + 2); }
            tmp[2 * i] = (int16_t)(l * vol_l / VOLUME_MAX);
            tmp[2 * i + 1] = (int16_t)(r * vol_r / VOLUME_MAX);
        }
        host_audio_write(ch, tmp, n);
        RET(n);
    }

    /* no sound device: simulate the timing */
    uint64_t now = now_us();
    if (blocking && g_ch[ch].busy_until > now)
        sched_wait(g_current, WAIT_DELAY, 0, g_ch[ch].busy_until - now, 0);
    now = now_us();
    uint64_t start = g_ch[ch].busy_until > now ? g_ch[ch].busy_until : now;
    g_ch[ch].busy_until = start + (uint64_t)n * 1000000u / AUDIO_RATE;
    RET(n);
}

/* args: channel, volume, buffer / channel, left volume, right volume, buffer */
void hle_sceAudioOutputBlocking(Ctx *c) { audio_output(c, (int)A0, (int)A1, (int)A1, A2, 1); }
void hle_sceAudioOutputPannedBlocking(Ctx *c) { audio_output(c, (int)A0, (int)A1, (int)A2, A3, 1); }
void hle_sceAudioOutputPanned(Ctx *c) { audio_output(c, (int)A0, (int)A1, (int)A2, A3, 0); }

void hle_sceAudioGetChannelRestLength(Ctx *c) {
    if (A0 >= 8) RET(ERR_ILLEGAL_ARGUMENT);
    if (host_audio_enabled()) RET(host_audio_queued((int)A0));
    uint64_t now = now_us();
    RET(g_ch[A0].busy_until > now ? (g_ch[A0].busy_until - now) * AUDIO_RATE / 1000000u : 0);
}
