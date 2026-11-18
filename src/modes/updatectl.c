/* updatectl.c - the interactive launch's update check (omp main.ts checkForNewVersion + the notifyNewVersion /
 * runAutoUpdate hand-off), the "Update Available" notice (ui-helpers.ts showNewVersionNotification) and the launch
 * side of `update.auto` (cli/auto-update.ts runAutoUpdate / follow): the welcome banner's version label follows the
 * detached worker (`omc __omc_worker_auto_update`, cli/sub_update.c) through its status file.
 *
 * Everything that may wait on the network or the worker runs on one detached thread; the UI thread only receives
 * posted results. Nothing is shown before the gate opens (the startup transcript is rendered and no setup wizard or
 * session picker is still up), so the notice never lands inside a partial transcript. Failures are silent. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* POSIX_SPAWN_SETSID */
#endif
#include "modes/interactive.h"

#include "coding/selfupdate.h"
#include "config/settings.h"
#include "core/fs.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define CHECK_TIMEOUT_MS 5000
#define POLL_MS 250

/* ---------- the notice (TranscriptNotice: DynamicBorder / Text(title + line, 1, 0) / DynamicBorder) ---------- */

typedef struct {
    tui_component_t base;
    char *version;
} notice_t;

static void warning_border(strvec_t *out, int width) {
    sb_t r = SB_INIT;
    sb_repeat(&r, tui_sym("boxRound.horizontal"), MAX(1, width));
    strvec_push_own(out, fg(TUI_TC_WARNING, sb_cstr(&r)));
    sb_free(&r);
}

static void notice_render(tui_component_t *c, int width, strvec_t *out) {
    notice_t *n = (notice_t *)c;
    warning_border(out, width);
    /* styled at render time (Text.setStyleFn), so a theme switch repaints it in the new colors */
    char *title = fg(TUI_TC_WARNING, "Update Available"), *bold = tui_bold(title);
    char *prefix_raw = xasprintf("New version %s is available. Run: ", n->version);
    char *prefix = fg(TUI_TC_MUTED, prefix_raw), *cmd = fg(TUI_TC_ACCENT, "omc update");
    char *text = xasprintf("%s\n%s%s", bold, prefix, cmd);
    int pad = tui_padding_x(1);
    strvec_t w = {0};
    tui_wrap_text_with_ansi(text, MAX(1, width - 2 * pad), &w);
    for (size_t i = 0; i < w.n; i++) {
        sb_t b = SB_INIT;
        tui_sb_spaces(&b, pad);
        sb_append(&b, w.v[i]);
        strvec_push_own(out, sb_detach(&b));
    }
    strvec_free(&w);
    free(text);
    free(cmd);
    free(prefix);
    free(prefix_raw);
    free(bold);
    free(title);
    warning_border(out, width);
}

static void notice_destroy(tui_component_t *c) {
    free(((notice_t *)c)->version);
    free(c);
}

static const tui_component_vt NOTICE_VT = {"update-notice", notice_render, NULL, NULL, notice_destroy};

static void show_notice(ui_t *u, const char *version) {
    if (!settings_bool("startup.checkUpdate", true)) return; /* notifyNewVersion */
    notice_t *n = xcalloc(1, sizeof *n);
    tui_component_init(&n->base, &NOTICE_VT);
    n->version = xstrdup(version);
    tui_container_add(u->chat, &n->base);
    tui_request_render(u->tui);
}

/* ---------- posting from the worker thread ---------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static tui_t *g_tui;      /* the running UI, NULL once it shuts down */
static bool g_gate;       /* the startup transcript is up: results may show */
static char *g_pending;   /* a notice that arrived before the gate opened */
static bool g_running;    /* a check thread is live (one per process) */

typedef struct {
    int kind; /* 0 notice, 1 welcome indicator */
    int phase;
    char *version;
    double fraction;
} post_t;

static void post_apply(void *ud) {
    post_t *p = ud;
    ui_t *u = g_ui;
    if (u && u->tui) {
        if (p->kind == 0) {
            show_notice(u, p->version);
        } else if (u->welcome) {
            ui_welcome_set_update(u->welcome, p->phase, p->version, p->fraction);
            tui_request_render(u->tui);
        }
    }
    free(p->version);
    free(p);
}

static void post(int kind, int phase, const char *version, double fraction) {
    pthread_mutex_lock(&g_mu);
    if (g_tui && kind == 0 && !g_gate) {
        free(g_pending);
        g_pending = xstrdup(version);
    } else if (g_tui) {
        post_t *p = xcalloc(1, sizeof *p);
        *p = (post_t){kind, phase, version ? xstrdup(version) : NULL, fraction};
        tui_post(g_tui, post_apply, p);
    }
    pthread_mutex_unlock(&g_mu);
}

static void notify(const char *version) { post(0, 0, version, 0); }

static bool ui_gone(void) {
    pthread_mutex_lock(&g_mu);
    bool gone = !g_tui;
    pthread_mutex_unlock(&g_mu);
    return gone;
}

/* ---------- auto-update: describe / follow (cli/auto-update.ts) ---------- */

typedef struct {
    int phase; /* UI_UPD_* */
    char *version;
    double fraction;
} shown_t;

/* describeAutoUpdate: what the banner shows for a worker's state; phase UI_UPD_NONE when nothing */
static shown_t describe(const su_state_t *s) {
    shown_t r = {UI_UPD_NONE, NULL, -1};
    const char *v = s->version ? s->version : s->target;
    if (!v) return r;
    switch (s->phase) {
    case SU_PH_CHECKING:
    case SU_PH_INSTALLING: r.phase = UI_UPD_UPDATING; break;
    case SU_PH_DOWNLOADING:
        r.phase = UI_UPD_UPDATING;
        if (s->total > 0) r.fraction = (double)s->received / (double)s->total;
        break;
    case SU_PH_DONE: r.phase = su_compare_versions(v, OMC_VERSION) > 0 ? UI_UPD_READY : UI_UPD_NONE; break;
    case SU_PH_FAILED: r.phase = UI_UPD_FAILED; break;
    case SU_PH_CURRENT: break;
    }
    if (r.phase) r.version = xstrdup(v);
    return r;
}

typedef struct {
    char *key; /* last shown, to skip repeats */
} follow_t;

static void show(follow_t *f, const shown_t *s) {
    char *key = s->phase ? xasprintf("%d %s %d", s->phase, s->version, (int)(s->fraction * 1000)) : xstrdup("-");
    if (f->key && !strcmp(f->key, key)) {
        free(key);
        return;
    }
    free(f->key);
    f->key = key;
    post(1, s->phase, s->phase ? s->version : NULL, s->fraction);
}

/* mirror the running worker's status until its lock is free (and the worker this launch spawned, if any, has exited),
 * then show how the run ended; a run that ended without a final status shows as failed, and a failure also gets the
 * startup notice, since the banner may have scrolled away or be hidden (startup.quiet) */
static void follow(const char *file, pid_t spawned, const char *target) {
    int64_t since = now_ms(), start = mono_ms();
    bool exited = spawned <= 0, live = false;
    follow_t f = {0};
    if (spawned > 0) {
        shown_t s = {UI_UPD_UPDATING, (char *)target, -1};
        show(&f, &s);
    }
    while (mono_ms() - start < SU_WORKER_DEADLINE_MS + 60000 && !ui_gone()) {
        if (!exited && waitpid(spawned, NULL, WNOHANG) != 0) exited = true;
        bool held = su_lock_held(file);
        if (!held && exited) break;
        su_state_t st;
        if (held && su_state_read(file, &st)) {
            /* under a held lock a final status is the previous run's (the worker has not written yet) or this run's
             * just before release */
            if (!su_phase_final(st.phase)) {
                live = true;
                shown_t s = describe(&st);
                show(&f, &s);
                free(s.version);
            }
            su_state_free(&st);
        }
        usleep(POLL_MS * 1000);
    }
    if (ui_gone()) {
        free(f.key);
        return;
    }
    su_state_t fin;
    bool have = su_state_read(file, &fin);
    shown_t out = {UI_UPD_NONE, NULL, -1};
    bool any = true;
    if (have && su_phase_final(fin.phase) && (live || fin.at >= since)) {
        out = describe(&fin);
    } else if (live || spawned > 0) {
        const char *v = have && fin.version ? fin.version : have && fin.target ? fin.target : target;
        if (v) out = (shown_t){UI_UPD_FAILED, xstrdup(v), -1};
    } else {
        any = false;
    }
    if (any) {
        show(&f, &out);
        if (out.phase == UI_UPD_FAILED) notify(out.version);
    }
    free(out.version);
    if (have) su_state_free(&fin);
    free(f.key);
}

/* spawnDetached: the worker outlives this session (its own session, stdio on the appended log) */
static pid_t spawn_worker(const char *target) {
    char *self = su_self_exe(), *log = su_auto_log_path();
    if (!self) {
        free(log);
        return -1;
    }
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    strvec_t env = {0};
    for (char **e = environ; *e; e++)
        if (strncmp(*e, "OMC_AUTO_UPDATE_TARGET=", 23)) strvec_push(&env, *e);
    strvec_push_own(&env, xasprintf("OMC_AUTO_UPDATE_TARGET=%s", target));
    vec_push(&env, NULL);
    char *argv[] = {self, "__omc_worker_auto_update", NULL};
    pid_t pid = -1;
    if (posix_spawn(&pid, self, &fa, &at, argv, env.v) != 0) pid = -1;
    env.n--;
    strvec_free(&env);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    free(self);
    free(log);
    return pid;
}

/* ---------- the check thread ---------- */

typedef struct {
    bool check, autoupd, can_self_update;
} chk_job_t;

/* checkForNewVersion: the latest release when it is newer than this build, else NULL (owned) */
static char *newer_version(void) {
    char *err = NULL, *v = su_latest_version(CHECK_TIMEOUT_MS, &err);
    free(err);
    if (v && su_compare_versions(v, OMC_VERSION) <= 0) {
        free(v);
        v = NULL;
    }
    return v;
}

static void *check_main(void *arg) {
    chk_job_t *j = arg;
    if (!j->autoupd) {
        char *v = newer_version();
        if (v) notify(v);
        free(v);
    } else if (!j->can_self_update) {
        char *v = newer_version(); /* where omc cannot update itself the check only feeds the notice */
        if (v) notify(v);
        free(v);
    } else {
        /* runAutoUpdate */
        char *file = su_auto_state_path();
        if (su_lock_held(file)) {
            follow(file, 0, NULL);
        } else {
            char *target = newer_version();
            if (su_lock_held(file)) { /* another launch started a worker while this one was checking */
                follow(file, 0, NULL);
            } else if (target) {
                su_state_t last;
                bool have = su_state_read(file, &last);
                if (have && !su_should_start_worker(&last, target, now_ms())) {
                    shown_t s = describe(&last);
                    if (s.phase == UI_UPD_READY) post(1, s.phase, s.version, s.fraction);
                    else if (last.phase != SU_PH_CURRENT) notify(target);
                    free(s.version);
                } else {
                    pid_t pid = spawn_worker(target);
                    if (pid > 0) follow(file, pid, target);
                    else notify(target);
                }
                if (have) su_state_free(&last);
            }
            free(target);
        }
        free(file);
    }
    free(j);
    pthread_mutex_lock(&g_mu);
    g_running = false;
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

void updatectl_start(ui_t *u, bool gate_open) {
    if (env_truthy("OMC_NO_UPDATE_CHECK")) return;
    bool check = settings_bool("startup.checkUpdate", true), autoupd = settings_bool("update.auto", false);
    if (!check && !autoupd) return;
    /* checkouts update through git: "run omc update" would be wrong advice */
    bool is_self = false;
    char *err = NULL, *target = su_update_target(&is_self, &err);
    bool checkout = !target || su_is_source_checkout(target);
    free(target);
    free(err);
    if (checkout) return;
    free(su_state_dir()); /* resolves the config dir (a lazily set static) here, not on the thread */
    pthread_mutex_lock(&g_mu);
    bool busy = g_running;
    if (!busy) {
        g_running = true;
        g_tui = u->tui;
        g_gate = gate_open;
    }
    pthread_mutex_unlock(&g_mu);
    if (busy) return;
    chk_job_t *j = xcalloc(1, sizeof *j);
    j->check = check;
    j->autoupd = autoupd;
    bool prebuilt;
    su_platform(&prebuilt);
    j->can_self_update = prebuilt;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &a, check_main, j) != 0) {
        free(j);
        pthread_mutex_lock(&g_mu);
        g_running = false;
        g_tui = NULL;
        pthread_mutex_unlock(&g_mu);
    }
    pthread_attr_destroy(&a);
}

void updatectl_open_gate(ui_t *u) {
    pthread_mutex_lock(&g_mu);
    g_gate = true;
    char *v = g_pending;
    g_pending = NULL;
    if (v && g_tui) {
        post_t *p = xcalloc(1, sizeof *p);
        *p = (post_t){0, 0, v, 0};
        tui_post(g_tui, post_apply, p); /* after the callback that opened the gate (e.g. the resumed transcript) */
        v = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    free(v);
}

void updatectl_shutdown(void) {
    pthread_mutex_lock(&g_mu);
    g_tui = NULL;
    g_gate = false;
    free(g_pending);
    g_pending = NULL;
    pthread_mutex_unlock(&g_mu);
}
