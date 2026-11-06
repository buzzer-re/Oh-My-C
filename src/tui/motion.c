/* motion.c - shared spinner clock, frame-rate ceiling, decorative motion switch and the motion presets (see motion.h). */
#include "tui/motion.h"

#include <math.h>

#include "core/core.h"

#define MOTION_REDUCED_INTERVAL_MS 250

static volatile int g_interval = TUI_DEFAULT_SPINNER_INTERVAL_MS;
static volatile int g_fps = TUI_DEFAULT_MAX_FPS;
static volatile bool g_effects = true;
static volatile int g_shimmer = TUI_SHIMMER_CLASSIC;

int tui_spinner_interval(void) { return g_interval; }
bool tui_spinner_animated(void) { return g_interval != TUI_SPINNER_INTERVAL_STATIC; }
void tui_set_spinner_interval(int ms) { g_interval = ms; }

int tui_shared_spinner_frame(int frame_count) {
    int iv = g_interval;
    if (frame_count <= 0 || iv == TUI_SPINNER_INTERVAL_STATIC) return 0;
    return (int)((now_ms() / iv) % frame_count);
}

int tui_spinner_tick(void) {
    int iv = g_interval;
    if (iv == TUI_SPINNER_INTERVAL_STATIC) return 0;
    return (int)((now_ms() / iv) % 720720); /* lcm(1..16) */
}

int tui_spinner_tick_delay(int period_ms) {
    if (period_ms < 1) period_ms = 1;
    int64_t now = now_ms();
    int64_t periods = now / period_ms;
    int64_t d = (periods + 1) * period_ms - now;
    return d < 1 ? 1 : (int)d;
}

double tui_min_frame_interval_ms(void) { return 1000.0 / g_fps; }
void tui_set_max_fps(int fps) { g_fps = fps; }

bool tui_motion_effects_enabled(void) { return g_effects; }
void tui_set_motion_effects(bool on) { g_effects = on; }

tui_shimmer_mode_t tui_shimmer_mode(void) { return (tui_shimmer_mode_t)g_shimmer; }
void tui_set_shimmer_mode(tui_shimmer_mode_t mode) { g_shimmer = mode; }

int tui_motion_preset_interval(tui_motion_mode_t mode) {
    return mode == TUI_MOTION_NONE      ? TUI_SPINNER_INTERVAL_STATIC
           : mode == TUI_MOTION_REDUCED ? MOTION_REDUCED_INTERVAL_MS
                                        : TUI_DEFAULT_SPINNER_INTERVAL_MS;
}

bool tui_motion_preset_effects(tui_motion_mode_t mode) { return mode == TUI_MOTION_FULL; }
