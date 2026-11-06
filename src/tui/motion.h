/*
 * motion.h - the shared animation knobs (omp spinner-clock.ts, frame-rate.ts, motion-effects.ts, theme/shimmer.ts mode and
 * config/motion-presets.ts): one period and one phase for every spinner, the repaint ceiling, and the switch over decorative
 * motion. Dependency-free: the CLI flag table parses with these helpers during bootstrap.
 */
#ifndef OMC_TUI_MOTION_H
#define OMC_TUI_MOTION_H

#include <stdbool.h>
#include <stdint.h>

#define TUI_DEFAULT_SPINNER_INTERVAL_MS 80
#define TUI_SPINNER_INTERVAL_STATIC 0
#define TUI_DEFAULT_MAX_FPS 30

/* The live spinner period in ms; TUI_SPINNER_INTERVAL_STATIC when spinners are still. */
int tui_spinner_interval(void);
bool tui_spinner_animated(void);
void tui_set_spinner_interval(int ms);
/* Glyph index on the shared wall clock, frame 0 when spinners are static. */
int tui_shared_spinner_frame(int frame_count);
/* The clock's tick count reduced to an int: any frame count up to 16 stays in phase (tc_spinner_frame takes it modulo). */
int tui_spinner_tick(void);
/* ms until the next multiple of period_ms on the wall clock, so timers armed from it fire together. */
int tui_spinner_tick_delay(int period_ms);

/* Shortest interval between two frames at the tui.maxFps ceiling, in ms. */
double tui_min_frame_interval_ms(void);
void tui_set_max_fps(int fps);

bool tui_motion_effects_enabled(void);
void tui_set_motion_effects(bool on);

/* display.shimmer, after tui.motion's preset */
typedef enum { TUI_SHIMMER_CLASSIC, TUI_SHIMMER_KITT, TUI_SHIMMER_DISABLED } tui_shimmer_mode_t;
tui_shimmer_mode_t tui_shimmer_mode(void);
void tui_set_shimmer_mode(tui_shimmer_mode_t mode);

/* tui.motion presets */
typedef enum { TUI_MOTION_FULL, TUI_MOTION_REDUCED, TUI_MOTION_NONE } tui_motion_mode_t;
int tui_motion_preset_interval(tui_motion_mode_t mode);
bool tui_motion_preset_effects(tui_motion_mode_t mode);

#endif
