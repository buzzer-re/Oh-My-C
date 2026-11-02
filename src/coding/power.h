/*
 * power.h - sleep prevention while the agent runs a turn (port of omp session/power-assertion-backend.ts and
 * pi-natives power.rs, `power.sleepPrevention`: off | idle (default) | display | system).
 *
 * omp takes the platform assertion natively; omc, limited to libc, uses the platform's own tools as a child:
 *   Linux  `systemd-inhibit --what=idle[:sleep] --who=omc --why='omc agent session' --mode=block
 *          --no-ask-password cat` — the logind inhibitor lives as long as `cat` reads its stdin from a pipe omc
 *          holds; closing the pipe (release, or omc dying) ends it. omp's best-effort desktop ScreenSaver inhibit
 *          for `display`/`system` (a cookie bound to a session-bus connection) has no CLI equivalent and is not
 *          taken. No system bus socket or no `systemd-inhibit` latches the backend off silently, like omp's
 *          PowerBackendUnavailable.
 *   macOS  `caffeinate -i [-d] [-s -u] -w <omc pid>`, killed on release.
 * Acquire/release are reference counted (one assertion for nested runs) and thread-safe.
 */
#ifndef OMC_POWER_H
#define OMC_POWER_H

#include "core/core.h"

void power_acquire(void);
void power_release(void);
/* the child command for a mode ("idle" | "display" | "system"), NULL for "off" (owned; tests) */
strvec_t power_command(const char *mode);

#endif
