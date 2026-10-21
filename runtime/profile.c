/* MKU_PROFILE=1: a sampling profiler. A background thread reads rt_cur_fn
 * (the last recompiled function entered) every 200 us; profile_report()
 * prints where the game spent its time. */
#include "recomp.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOTS 8192
static struct { uint32_t addr; uint32_t hits; } g_slots[SLOTS];
static uint32_t g_total;

static void *sampler(void *arg) {
    (void)arg;
    for (;;) {
        usleep(200);
        uint32_t a = rt_cur_fn;
        uint32_t h = (a >> 2) * 2654435761u % SLOTS;
        for (int i = 0; i < SLOTS; i++, h = (h + 1) % SLOTS) {
            if (g_slots[h].addr == a || g_slots[h].addr == 0) {
                g_slots[h].addr = a;
                g_slots[h].hits++;
                break;
            }
        }
        g_total++;
    }
    return NULL;
}

void profile_start(void) {
    if (!getenv("MKU_PROFILE")) return;
    pthread_t t;
    pthread_create(&t, NULL, sampler, NULL);
}

static int by_hits(const void *a, const void *b) {
    return (int)((const typeof(g_slots[0]) *)b)->hits - (int)((const typeof(g_slots[0]) *)a)->hits;
}

void profile_report(void) {
    if (!g_total) return;
    static typeof(g_slots[0]) sorted[SLOTS];
    memcpy(sorted, g_slots, sizeof sorted);
    qsort(sorted, SLOTS, sizeof sorted[0], by_hits);
    fprintf(stderr, "[profile] %u samples; most frequent last-entered functions (module-relative):\n", g_total);
    for (int i = 0; i < 25 && sorted[i].hits; i++)
        fprintf(stderr, "  %6.2f%%  func_%08X\n", 100.0 * sorted[i].hits / g_total, sorted[i].addr - rt_image_base);
}
