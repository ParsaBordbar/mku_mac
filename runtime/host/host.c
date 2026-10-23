/*
 * The host side: an SDL2 window showing the PSP screen, and keyboard /
 * game controller input for sceCtrl.
 *
 * SDL must live on the main thread on macOS. The main thread is free once
 * _start has handed over to the game's own threads, so it becomes the window
 * loop. The game thread hands finished frames over in host_submit_frame().
 *
 * MKU_HEADLESS=1 runs without a window (for automated runs and screenshots).
 */
#include "host.h"

#include <SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 480
#define H 272

static int g_headless = 1;
static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;

static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
#define MAX_FRAME (W * H * 16)   /* up to 4x rendering resolution */
static uint32_t g_frame[MAX_FRAME];
static int g_frame_w = W, g_frame_h = H, g_frame_new;

/* Display settings and their hotkeys (F1 textures, F2 resolution, F3 sharp).
 * The game thread applies requests at the next frame (host_take_requests). */
static int g_set_scale = 1, g_set_tex = 1, g_set_sharp;
static atomic_int g_req_scale = -1, g_req_tex = -1, g_fps;

void settings_save(int scale, int texscale, int sharp);

void host_take_requests(int *scale, int *texscale) {
    *scale = atomic_exchange(&g_req_scale, -1);
    *texscale = atomic_exchange(&g_req_tex, -1);
}

void host_report_fps(int fps) { atomic_store(&g_fps, fps); }

static void update_title(void) {
    char t[160];
    snprintf(t, sizeof t, "Mortal Kombat: Unchained (native) - %dx%d, textures %s - %d fps   [F1 textures  F2 resolution  F3 %s]",
             480 * g_set_scale, 272 * g_set_scale, g_set_tex > 1 ? (g_set_tex == 2 ? "xBRZ 2x" : "xBRZ 4x") : "original",
             atomic_load(&g_fps), g_set_sharp ? "smooth" : "sharp");
    SDL_SetWindowTitle(g_window, t);
}

static void hotkey(SDL_Scancode k) {
    if (k == SDL_SCANCODE_F1) {
        g_set_tex = g_set_tex == 1 ? 2 : g_set_tex == 2 ? 4 : 1;
        atomic_store(&g_req_tex, g_set_tex);
    } else if (k == SDL_SCANCODE_F2) {
        g_set_scale = g_set_scale % 4 + 1;
        atomic_store(&g_req_scale, g_set_scale);
    } else if (k == SDL_SCANCODE_F3) {
        g_set_sharp = !g_set_sharp;
        if (g_texture) SDL_SetTextureScaleMode(g_texture, g_set_sharp ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    } else {
        return;
    }
    settings_save(g_set_scale, g_set_tex, g_set_sharp);
    update_title();
}

static _Atomic uint32_t g_buttons;
static _Atomic uint32_t g_analog = 0x8080;   /* lx | ly << 8 */
static SDL_GameController *g_pad;

void host_init(void) {
    if (getenv("MKU_HEADLESS")) return;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[host] SDL_Init failed (%s), running headless\n", SDL_GetError());
        return;
    }
    g_window = SDL_CreateWindow("Mortal Kombat: Unchained (native)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                W * 2, H * 2, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(g_renderer, W, H);   /* keeps 480:272 with black bars */
    /* Smooth scaling to the window (MKU_SHARP=1: nearest neighbour, the raw pixels).
     * The texture is (re)created at the rendering resolution on the first frame. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, getenv("MKU_SHARP") ? "0" : "1");
    g_headless = 0;
    if (getenv("MKU_SCALE")) g_set_scale = atoi(getenv("MKU_SCALE"));
    if (getenv("MKU_TEXSCALE")) g_set_tex = atoi(getenv("MKU_TEXSCALE"));
    if (g_set_scale < 1 || g_set_scale > 4) g_set_scale = 1;
    if (g_set_tex != 2 && g_set_tex != 4) g_set_tex = 1;
    g_set_sharp = getenv("MKU_SHARP") != NULL;
    update_title();
    host_audio_init();
    fprintf(stderr, "[host] window open. Keys: arrows=d-pad, Enter=START, RShift=SELECT, "
                    "Z=X(cross) X=O(circle) A=[] S=/\\, Q/W=L/R, IJKL=analog, TFGH/RY/C=camera, "
                    "F1=HD textures F2=resolution F3=sharp, Esc=quit\n");
}

/* Game thread: a complete frame is ready (called from sceDisplaySetFrameBuf). */
void host_submit_frame(const uint32_t *rgba, int w, int h) {
    if (g_headless || w * h > MAX_FRAME) return;
    pthread_mutex_lock(&g_frame_lock);
    memcpy(g_frame, rgba, sizeof(uint32_t) * (size_t)(w * h));
    g_frame_w = w;
    g_frame_h = h;
    g_frame_new = 1;
    pthread_mutex_unlock(&g_frame_lock);
}

/* MKU_PRESS="200-210:8,400-405:4000" holds buttons (hex PSP bits) during those
 * frame ranges: scripted input for automated test runs. */
extern uint64_t display_frame_count(void);

static uint32_t scripted_buttons(void) {
    static const char *script = (const char *)-1;
    if (script == (const char *)-1) script = getenv("MKU_PRESS");
    if (!script) return 0;
    uint64_t f = display_frame_count();
    uint32_t b = 0;
    for (const char *s = script; *s;) {
        unsigned long long from, to;
        unsigned bits;
        if (sscanf(s, "%llu-%llu:%x", &from, &to, &bits) == 3 && f >= from && f <= to) b |= bits;
        s += strcspn(s, ",");
        if (*s) s++;
    }
    return b;
}

uint32_t host_buttons(void) { return atomic_load(&g_buttons) | scripted_buttons(); }
uint32_t host_analog(void) { return atomic_load(&g_analog); }

/* PSP button bits */
enum {
    B_SELECT = 0x1, B_START = 0x8, B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80,
    B_L = 0x100, B_R = 0x200, B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000,
};

static uint32_t keyboard_buttons(const Uint8 *k) {
    uint32_t b = 0;
    if (k[SDL_SCANCODE_UP]) b |= B_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= B_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= B_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= B_RIGHT;
    if (k[SDL_SCANCODE_RETURN]) b |= B_START;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE]) b |= B_SELECT;
    if (k[SDL_SCANCODE_Z]) b |= B_CROSS;
    if (k[SDL_SCANCODE_X]) b |= B_CIRCLE;
    if (k[SDL_SCANCODE_A]) b |= B_SQUARE;
    if (k[SDL_SCANCODE_S]) b |= B_TRIANGLE;
    if (k[SDL_SCANCODE_Q]) b |= B_L;
    if (k[SDL_SCANCODE_W]) b |= B_R;
    return b;
}

static uint32_t pad_buttons(void) {
    if (!g_pad) return 0;
    static const struct { SDL_GameControllerButton sdl; uint32_t psp; } map[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, B_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, B_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, B_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, B_RIGHT},
        {SDL_CONTROLLER_BUTTON_START, B_START}, {SDL_CONTROLLER_BUTTON_BACK, B_SELECT},
        {SDL_CONTROLLER_BUTTON_A, B_CROSS}, {SDL_CONTROLLER_BUTTON_B, B_CIRCLE},
        {SDL_CONTROLLER_BUTTON_X, B_SQUARE}, {SDL_CONTROLLER_BUTTON_Y, B_TRIANGLE},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_L}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_R},
        {SDL_CONTROLLER_BUTTON_TOUCHPAD, B_SELECT},   /* PS4/PS5 touchpad click */
    };
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GameControllerGetButton(g_pad, map[i].sdl)) b |= map[i].psp;
    /* the PSP has no L2/R2: treat the triggers as L/R too */
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= B_L;
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= B_R;
    return b;
}

static uint32_t analog(const Uint8 *k) {
    int lx = 128, ly = 128;
    if (g_pad) {
        lx = 128 + SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) / 256;
        ly = 128 + SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) / 256;
    }
    if (k[SDL_SCANCODE_J]) lx = 0;
    if (k[SDL_SCANCODE_L]) lx = 255;
    if (k[SDL_SCANCODE_I]) ly = 0;
    if (k[SDL_SCANCODE_K]) ly = 255;
    lx = lx < 0 ? 0 : lx > 255 ? 255 : lx;
    ly = ly < 0 ? 0 : ly > 255 ? 255 : ly;
    return (uint32_t)lx | ((uint32_t)ly << 8);
}

/* Free camera (host side only; the game never knows). Angles in radians,
 * zoom in view-space units toward the orbit point. The renderer reads it via
 * host_camera(). MKU_CAM="yaw,pitch,zoom" fixes it (for test screenshots). */
static _Atomic float g_cam_yaw, g_cam_pitch, g_cam_zoom;

void host_camera(float *yaw, float *pitch, float *zoom) {
    *yaw = atomic_load(&g_cam_yaw);
    *pitch = atomic_load(&g_cam_pitch);
    *zoom = atomic_load(&g_cam_zoom);
}

static void camera_init_from_env(void) {
    const char *e = getenv("MKU_CAM");
    float y = 0, p = 0, z = 0;
    if (e && sscanf(e, "%f,%f,%f", &y, &p, &z) >= 2) {
        atomic_store(&g_cam_yaw, y);
        atomic_store(&g_cam_pitch, p);
        atomic_store(&g_cam_zoom, z);
    }
}

static void camera_update(const Uint8 *k, float dt) {
    float dyaw = 0, dpitch = 0, dzoom = 0;
    int reset = k[SDL_SCANCODE_C];
    if (k[SDL_SCANCODE_F]) dyaw -= 1;
    if (k[SDL_SCANCODE_H]) dyaw += 1;
    if (k[SDL_SCANCODE_T]) dpitch -= 1;
    if (k[SDL_SCANCODE_G]) dpitch += 1;
    if (k[SDL_SCANCODE_R]) dzoom += 1;
    if (k[SDL_SCANCODE_Y]) dzoom -= 1;
    if (g_pad) {
        float rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.f;
        float ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.f;
        if (fabsf(rx) < 0.15f) rx = 0;   /* dead zone */
        if (fabsf(ry) < 0.15f) ry = 0;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) dzoom -= ry;
        else { dyaw += rx; dpitch += ry; }
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_LEFTSTICK)) reset = 1;
    }
    if (reset) {
        atomic_store(&g_cam_yaw, 0.f);
        atomic_store(&g_cam_pitch, 0.f);
        atomic_store(&g_cam_zoom, 0.f);
        return;
    }
    float pitch = atomic_load(&g_cam_pitch) + dpitch * 1.5f * dt;
    if (pitch > 1.3f) pitch = 1.3f;
    if (pitch < -1.3f) pitch = -1.3f;
    atomic_store(&g_cam_yaw, atomic_load(&g_cam_yaw) + dyaw * 1.8f * dt);
    atomic_store(&g_cam_pitch, pitch);
    atomic_store(&g_cam_zoom, atomic_load(&g_cam_zoom) + dzoom * 4.f * dt);
}

/* Main thread, forever. */
void host_run(void) {
    camera_init_from_env();
    if (g_headless) {
        for (;;) pause();
    }
    static uint32_t frame[MAX_FRAME];
    int fw = W, fh = H, tex_w = 0, tex_h = 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
                fprintf(stderr, "[host] quit\n");
                exit(0);
            }
            if (e.type == SDL_KEYDOWN && !e.key.repeat) hotkey(e.key.keysym.scancode);
            if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) {
                g_pad = SDL_GameControllerOpen(e.cdevice.which);
                if (g_pad) fprintf(stderr, "[host] controller: %s\n", SDL_GameControllerName(g_pad));
            }
            if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad) {
                SDL_GameControllerClose(g_pad);
                g_pad = NULL;
            }
        }
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        atomic_store(&g_buttons, keyboard_buttons(k) | pad_buttons());
        atomic_store(&g_analog, analog(k));
        {
            static uint64_t last;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
            float dt = last ? (now - last) / 1e6f : 0.f;
            last = now;
            camera_update(k, dt > 0.1f ? 0.1f : dt);
        }

        int have = 0;
        pthread_mutex_lock(&g_frame_lock);
        if (g_frame_new) {
            fw = g_frame_w;
            fh = g_frame_h;
            memcpy(frame, g_frame, sizeof(uint32_t) * (size_t)(fw * fh));
            g_frame_new = 0;
            have = 1;
        }
        pthread_mutex_unlock(&g_frame_lock);
        if (have && (fw != tex_w || fh != tex_h)) {
            if (g_texture) SDL_DestroyTexture(g_texture);
            /* ABGR8888 = bytes R,G,B,A in memory on little-endian: the PSP's own layout. */
            g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, fw, fh);
            SDL_SetTextureScaleMode(g_texture, g_set_sharp ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
            tex_w = fw;
            tex_h = fh;
        }
        if (have) SDL_UpdateTexture(g_texture, NULL, frame, fw * 4);
        {   /* refresh the fps in the title once a second */
            static uint32_t last_title;
            uint32_t now = SDL_GetTicks();
            if (now - last_title > 1000) { last_title = now; update_title(); }
        }
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_renderer);
        if (g_texture) SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
        SDL_RenderPresent(g_renderer);   /* waits for the display's vsync */
    }
}
