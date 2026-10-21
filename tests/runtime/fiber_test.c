/* Fiber switch test: two fibers ping-pong with the main context; callee-saved
 * state (locals kept in registers across switches, floats) must survive. */
#include "fiber.h"

#include <stdio.h>
#include <stdlib.h>

static Fiber g_main, g_a, g_b;
static int g_log[16], g_n;

static void run_a(void *arg) {
    double keep = 1.5;                       /* lives in a callee-saved FP register */
    for (int i = 0; i < 3; i++) {
        g_log[g_n++] = 100 + i + (int)(intptr_t)arg;
        keep *= 2.0;
        fiber_switch(&g_a, &g_b);
    }
    g_log[g_n++] = (int)keep;                /* 1.5 * 8 = 12 */
    fiber_switch(&g_a, &g_main);
    abort();
}

static void run_b(void *arg) {
    (void)arg;
    for (int i = 0;; i++) {
        g_log[g_n++] = 200 + i;
        fiber_switch(&g_b, i < 2 ? &g_a : &g_main);
    }
}

int main(void) {
    static char sa[64 * 1024], sb[64 * 1024];
    fiber_init(&g_a, sa, sizeof sa, run_a, (void *)(intptr_t)10);
    fiber_init(&g_b, sb, sizeof sb, run_b, NULL);
    fiber_switch(&g_main, &g_a);             /* a b a b a b -> main */
    fiber_switch(&g_main, &g_a);             /* a finishes -> main */
    int want[] = {110, 200, 111, 201, 112, 202, 12};
    int ok = g_n == 7;
    for (int i = 0; ok && i < 7; i++) ok = g_log[i] == want[i];
    printf("fiber_test: %s (", ok ? "ok" : "FAIL");
    for (int i = 0; i < g_n; i++) printf("%s%d", i ? " " : "", g_log[i]);
    printf(")\n");
    return ok ? 0 : 1;
}
