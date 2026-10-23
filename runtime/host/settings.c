/*
 * mku.ini: the player's settings, so they survive restarts.
 *
 *   scale=2        rendering resolution (1 = PSP's 480x272, 2 = 960x544, ...)
 *   texscale=4     texture upscaling with xBRZ (1 = off, 2, 4)
 *   sharp=0        1 = show raw pixels when the window is bigger than the image
 *
 * An environment variable (MKU_SCALE, MKU_TEXSCALE, MKU_SHARP) overrides the
 * file. The in-game hotkeys (F1-F3) change the values and save the file.
 */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_FILE "mku.ini"

static const struct { const char *key, *env, *def; } k_settings[] = {
    {"scale", "MKU_SCALE", "2"},
    {"texscale", "MKU_TEXSCALE", "1"},
    {"sharp", "MKU_SHARP", NULL},
};

/* Before anything reads the MKU_* variables: fill in the ones not set. */
void settings_load(void) {
    char vals[3][32] = {{0}};
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char key[32], val[32];
            if (line[0] == '#' || sscanf(line, " %31[^= ] = %31s", key, val) != 2) continue;
            for (int i = 0; i < 3; i++)
                if (!strcmp(key, k_settings[i].key)) strcpy(vals[i], val);
        }
        fclose(f);
    }
    for (int i = 0; i < 3; i++) {
        if (getenv(k_settings[i].env)) continue;
        const char *v = vals[i][0] ? vals[i] : k_settings[i].def;
        if (!v) continue;
        if (!strcmp(k_settings[i].key, "sharp") && strcmp(v, "1")) continue;   /* MKU_SHARP is a flag */
        setenv(k_settings[i].env, v, 0);
    }
}

void settings_save(int scale, int texscale, int sharp) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "# Mortal Kombat: Unchained (native port) settings. F1/F2/F3 in game change them.\n");
    fprintf(f, "scale=%d\ntexscale=%d\nsharp=%d\n", scale, texscale, sharp);
    fclose(f);
}
