/* Host sound output: an SDL audio device at 44.1 kHz stereo; the callback
 * mixes the 8 PSP channels, each fed through its own ring buffer. */
#include "host.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHANNELS 8
#define RING 16384   /* frames per channel */

static SDL_AudioDeviceID g_dev;
static struct { int16_t buf[RING * 2]; int read, count; } g_ring[CHANNELS];

static void callback(void *user, Uint8 *stream, int len) {
    (void)user;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    for (int i = 0; i < frames; i++) {
        int l = 0, r = 0;
        for (int c = 0; c < CHANNELS; c++) {
            if (!g_ring[c].count) continue;
            int p = g_ring[c].read;
            l += g_ring[c].buf[2 * p];
            r += g_ring[c].buf[2 * p + 1];
            g_ring[c].read = (p + 1) % RING;
            g_ring[c].count--;
        }
        out[2 * i] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
        out[2 * i + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
    }
}

void host_audio_init(void) {
    if (getenv("MKU_HEADLESS") || getenv("MKU_NOSOUND")) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[host] no sound: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want = {.freq = 44100, .format = AUDIO_S16SYS, .channels = 2, .samples = 512, .callback = callback};
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!g_dev) {
        fprintf(stderr, "[host] no sound: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    char *name = NULL;
    SDL_AudioSpec spec;
    if (SDL_GetDefaultAudioInfo(&name, &spec, 0) == 0 && name) {   /* the device macOS is set to use */
        fprintf(stderr, "[host] sound: %s (the macOS default output)\n", name);
        SDL_free(name);
    } else {
        fprintf(stderr, "[host] sound: default output device\n");
    }
}

int host_audio_enabled(void) { return g_dev != 0; }

int host_audio_queued(int ch) {
    SDL_LockAudioDevice(g_dev);
    int n = g_ring[ch].count;
    SDL_UnlockAudioDevice(g_dev);
    return n;
}

void host_audio_write(int ch, const int16_t *stereo, int frames) {
    SDL_LockAudioDevice(g_dev);
    for (int i = 0; i < frames && g_ring[ch].count < RING; i++) {
        int p = (g_ring[ch].read + g_ring[ch].count) % RING;
        g_ring[ch].buf[2 * p] = stereo[2 * i];
        g_ring[ch].buf[2 * p + 1] = stereo[2 * i + 1];
        g_ring[ch].count++;
    }
    SDL_UnlockAudioDevice(g_dev);
}
