/* Host platform layer: window, frames, input (host.c, SDL2). */
#pragma once

#include <stdint.h>

void host_init(void);                       /* main thread, before the game starts */
void host_run(void);                        /* main thread, never returns */
void host_submit_frame(const uint32_t *rgba);   /* game thread: a finished 480x272 frame */
uint32_t host_buttons(void);                /* PSP button bits currently held */
uint32_t host_analog(void);                 /* lx | ly << 8, 128 = centre */

/* audio.c: sound output (SDL); not available when headless */
void host_audio_init(void);
int host_audio_enabled(void);
int host_audio_queued(int ch);                                 /* frames still queued */
void host_audio_write(int ch, const int16_t *stereo, int frames);
