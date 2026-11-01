/* statusline.c - status band and composer chrome (port of omp packages/tui/src/status-line/
 * component.ts / segments.ts / presets.ts / separators.ts, prompt/composer-hints.ts and the
 * composer wiring of interactive-mode.ts).
 *
 * The status line renders in one of five layouts: `box` (powerline groups joined by the
 * context gauge, embedded in the box composer's top border), `band` (box opened by a soft cap,
 * the band composer's top row; omp's default), `plain-full` / `plain-left` (standalone bottom
 * bar) and `plain-right` (the chip on the claude/rule composers' top rule). */
#include "ui/ui.h"

#include "config/settings.h"
#include "core/proc.h"
#include "tui/color.h"
#include "tui/composer.h"

#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

/* ---------- small helpers ---------- */

static const char *sym(const char *k) { return tui_sym(k); }

static char *with_icon(const char *icon, const char *text) {
    return icon && *icon ? xasprintf("%s %s", icon, text) : xstrdup(text);
}

static char *fgw(tui_tc_t slot, const char *s) { return xasprintf("%s%s\x1b[39m", tui_fg_seq(TH(), slot), s); }

static char *fg_rgb(uint32_t rgb, const char *s) {
    char b[32];
    return xasprintf("%s%s\x1b[39m", tui_rgb_fg(TH(), rgb, b, sizeof b), s);
}

/* upstream formatNumber */
static char *fmt_number(double n) {
    char b[32];
    double a = n;
    if (a < 1000) {
        if (a == floor(a)) snprintf(b, sizeof b, "%.0f", a);
        else snprintf(b, sizeof b, "%g", a);
    } else if (a < 10000 || (a >= 1e6 && a < 1e7) || (a >= 1e9 && a < 1e10)) {
        double d = a < 10000 ? a / 1e3 : a < 1e7 ? a / 1e6 : a / 1e9;
        const char *u = a < 10000 ? "K" : a < 1e7 ? "M" : "B";
        snprintf(b, sizeof b, "%.1f", d);
        size_t l = strlen(b);
        if (l > 2 && !strcmp(b + l - 2, ".0")) b[l - 2] = 0;
        strcat(b, u);
    } else {
        double d = a < 1e6 ? a / 1e3 : a < 1e9 ? a / 1e6 : a / 1e9;
        const char *u = a < 1e6 ? "K" : a < 1e9 ? "M" : "B";
        snprintf(b, sizeof b, "%.0f%s", floor(d + 0.5), u);
    }
    return xstrdup(b);
}

static char *sanitize_status(const char *s) {
    sb_t b = SB_INIT;
    bool sp = false;
    char *plain = tui_strip_ansi(s ? s : "");
    for (const char *p = plain; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) c = ' ';
        if (c == ' ') {
            if (b.len && !sp) sb_putc(&b, ' ');
            sp = true;
            continue;
        }
        sp = false;
        sb_putc(&b, (char)c);
    }
    free(plain);
    while (b.len && b.s[b.len - 1] == ' ') b.s[--b.len] = 0;
    return sb_detach(&b);
}

/* ---------- state ---------- */

typedef struct {
    int64_t active_ms, started; /* turn meter (time_spent, brand spinner) */
    bool brand_working;
    uint32_t fade_from, fade_to;
    int64_t fade_start; /* 0 = no fade */
    int fade_timer;
    int tick_timer;
    /* git */
    pthread_mutex_t gmu;
    char *git_root;
    int64_t git_fetched;
    bool git_inflight, git_have;
    int staged, unstaged, untracked;
    /* composer hint usage */
    json_t *hint_uses;
} sl_state_t;

static sl_state_t g_sl = {.gmu = PTHREAD_MUTEX_INITIALIZER};

static const char *composer_shape(void) {
    const char *s = settings_str("composer.shape", "band");
    return s && *s ? s : "band";
}

const char *ui_composer_shape(void) { return composer_shape(); }

/* ---------- key hints (upstream key-hint-format.ts) ---------- */

char *ui_key_hint(const char *key) {
#ifdef __APPLE__
    const bool mac = true;
#else
    const bool mac = false;
#endif
    static const char *const MODS[4][3] = {
        {"ctrl", "key.ctrl", "key.ctrlMac"}, {"alt", "key.alt", "key.altMac"}, {"shift", "key.shift", "key.shift"}, {"super", "key.super", "key.superMac"}};
    static const char *const KEYS[][2] = {{"esc", "key.esc"},     {"escape", "key.esc"}, {"enter", "key.enter"},
                                          {"return", "key.enter"}, {"space", "key.space"}, {"tab", "key.tab"},
                                          {"backspace", "key.backspace"}, {"delete", "key.delete"}, {"up", "key.up"},
                                          {"down", "key.down"},   {"left", "key.left"},  {"right", "key.right"}};
    static const char *const WORDS[][2] = {{"home", "Home"}, {"end", "End"}, {"pageup", "PgUp"}, {"pagedown", "PgDn"}, {"insert", "Ins"}};
    strvec_t parts = {0};
    size_t kl = strlen(key);
    bool plus_key = kl > 0 && key[kl - 1] == '+';
    char *k = xstrndup(key, plus_key ? kl - 1 : kl);
    for (char *p = k, *q; p; p = q) {
        q = strchr(p, '+');
        if (q) *q++ = 0;
        if (*p || !q) strvec_push(&parts, p);
    }
    free(k);
    if (plus_key) {
        if (parts.n) parts.n--, free(parts.v[parts.n]);
        strvec_push(&parts, "+");
    }
    sb_t b = SB_INIT;
    if (!parts.n) return sb_detach(&b);
    char *base = parts.v[parts.n - 1];
    for (int m = 0; m < 4; m++) {
        bool has = false;
        for (size_t i = 0; i + 1 < parts.n; i++)
            if (str_eq(parts.v[i], MODS[m][0])) has = true;
        if (!has) continue;
        const char *label = sym(MODS[m][mac ? 2 : 1]);
        bool letter = false;
        for (const char *c = label; *c; c++)
            if (isalnum((unsigned char)*c)) letter = true;
        sb_append(&b, label);
        sb_append(&b, letter ? "+" : sym("key.joiner"));
    }
    char *lower = str_lower(base);
    bool done = false;
    for (int m = 0; m < 4 && !done; m++)
        if (str_eq(lower, MODS[m][0])) sb_append(&b, sym(MODS[m][mac ? 2 : 1])), done = true;
    for (size_t i = 0; i < ARRAY_LEN(KEYS) && !done; i++)
        if (str_eq(lower, KEYS[i][0])) sb_append(&b, sym(KEYS[i][1])), done = true;
    for (size_t i = 0; i < ARRAY_LEN(WORDS) && !done; i++)
        if (str_eq(lower, WORDS[i][0])) sb_append(&b, WORDS[i][1]), done = true;
    if (!done) {
        if (strlen(base) == 1) {
            sb_putc(&b, parts.n == 1 ? base[0] : (char)toupper((unsigned char)base[0]));
        } else {
            sb_putc(&b, (char)toupper((unsigned char)base[0]));
            sb_append(&b, base + 1);
        }
    }
    free(lower);
    strvec_free(&parts);
    return sb_detach(&b);
}

char *ui_double_tap_hint(const char *key) {
    char *l = ui_key_hint(key);
    bool letter = false;
    for (const char *c = l; *c; c++)
        if (isalnum((unsigned char)*c)) letter = true;
    char *r = xasprintf("%s%s%s", l, letter ? " " : sym("key.joiner"), l);
    free(l);
    return r;
}

/* ---------- composer hint usage (persisted like omp's agent.db usage counters) ---------- */

static char *hint_path(void) { return omc_agent_path("usage.json"); }

static void hints_load(void) {
    if (g_sl.hint_uses) return;
    char *p = hint_path();
    size_t n = 0;
    char *d = fs_read_limit(p, 1 << 16, &n, NULL);
    json_t *j = d ? json_parse(d, n, NULL) : NULL;
    free(d);
    free(p);
    if (!json_is_obj(j)) {
        json_free(j);
        j = json_obj();
    }
    g_sl.hint_uses = j;
}

static int hint_uses(const char *id) {
    hints_load();
    return (int)json_get_int(json_get(g_sl.hint_uses, "hint"), id, 0);
}

void ui_hint_record(const char *id) {
    hints_load();
    json_t *h = json_get(g_sl.hint_uses, "hint");
    if (!json_is_obj(h)) {
        json_set(g_sl.hint_uses, "hint", json_obj());
        h = json_get(g_sl.hint_uses, "hint");
    }
    json_set_int(h, id, json_get_int(h, id, 0) + 1);
    char *s = json_dump(g_sl.hint_uses, 0);
    char *p = hint_path();
    char *dir = path_dirname(p);
    fs_mkdir_p(dir, 0700);
    fs_write_atomic(p, s, strlen(s), 0600);
    free(dir);
    free(p);
    free(s);
}

/* resolveComposerHint: `<key>` accent + dim italic label */
static char *composer_hint(void *ud) {
    ui_t *u = ud;
    if (hint_uses("effort") >= 3) return NULL;
    if (json_len(u->app->agent->messages) > 0) return NULL;
    char *key = ui_key_hint("shift+tab");
    char *k = fgw(TUI_TC_ACCENT, key);
    char *r = xasprintf("%s %s\x1b[3mto change thinking effort\x1b[23m\x1b[39m", k, tui_fg_seq(TH(), TUI_TC_DIM));
    free(k);
    free(key);
    return r;
}

/* ---------- settings ---------- */

typedef struct {
    strvec_t left, right;
    const char *separator;
    json_t *opts; /* merged segment options (owned) */
} sl_settings_t;

static void preset_def(const char *name, sl_settings_t *s) {
    static const struct {
        const char *name, *left, *right, *sep, *opts;
    } P[] = {
        {"default", "pi vim model mode collab stream path git pr context_pct cost", "session_name", "powerline-thin",
         "{\"model\":{\"showThinkingLevel\":true},\"path\":{\"abbreviate\":true,\"maxLength\":40,\"stripWorkPrefix\":true},"
         "\"git\":{\"showBranch\":true,\"showStaged\":true,\"showUnstaged\":true,\"showUntracked\":true}}"},
        {"minimal", "vim path git", "session_name mode context_pct", "slash",
         "{\"path\":{\"abbreviate\":true,\"maxLength\":30},\"git\":{\"showBranch\":true,\"showStaged\":false,\"showUnstaged\":false,"
         "\"showUntracked\":false}}"},
        {"compact", "vim model mode git pr", "session_name cost context_pct", "powerline-thin",
         "{\"model\":{\"showThinkingLevel\":false},\"git\":{\"showBranch\":true,\"showStaged\":true,\"showUnstaged\":true,"
         "\"showUntracked\":false}}"},
        {"full", "pi vim hostname model mode path git pr subagents",
         "session_name cache_hit token_in token_out token_rate cache_read cost context_pct time_spent time", "powerline",
         "{\"model\":{\"showThinkingLevel\":true},\"path\":{\"abbreviate\":true,\"maxLength\":50},\"git\":{\"showBranch\":true,"
         "\"showStaged\":true,\"showUnstaged\":true,\"showUntracked\":true},\"time\":{\"format\":\"24h\",\"showSeconds\":false}}"},
        {"nerd", "pi vim hostname model mode path git pr session subagents",
         "session_name token_in token_out cache_read cache_write token_rate cost context_pct context_total time_spent time",
         "powerline",
         "{\"model\":{\"showThinkingLevel\":true},\"path\":{\"abbreviate\":true,\"maxLength\":60},\"git\":{\"showBranch\":true,"
         "\"showStaged\":true,\"showUnstaged\":true,\"showUntracked\":true},\"time\":{\"format\":\"24h\",\"showSeconds\":true}}"},
        {"ascii", "vim model mode path git pr", "session_name token_total cost context_pct", "ascii",
         "{\"model\":{\"showThinkingLevel\":true},\"path\":{\"abbreviate\":true,\"maxLength\":40},\"git\":{\"showBranch\":true,"
         "\"showStaged\":true,\"showUnstaged\":true,\"showUntracked\":true}}"},
        {"custom", "vim model mode path git pr", "session_name token_total cost context_pct", "powerline-thin", "{}"},
    };
    size_t idx = 0;
    for (size_t i = 0; i < ARRAY_LEN(P); i++)
        if (str_eq(name, P[i].name)) idx = i;
    char *l = xstrdup(P[idx].left), *r = xstrdup(P[idx].right);
    for (char *t = strtok(l, " "); t; t = strtok(NULL, " ")) strvec_push(&s->left, t);
    for (char *t = strtok(r, " "); t; t = strtok(NULL, " ")) strvec_push(&s->right, t);
    free(l);
    free(r);
    s->separator = P[idx].sep;
    s->opts = json_parse_str(P[idx].opts);
}

static void load_settings(sl_settings_t *s) {
    memset(s, 0, sizeof *s);
    const char *preset = settings_str("statusLine.preset", "default");
    preset_def(preset, s);
    if (str_eq(preset, "custom")) {
        json_t *l = settings_get("statusLine.leftSegments"), *r = settings_get("statusLine.rightSegments"), *x;
        if (json_is_arr(l)) {
            strvec_free(&s->left);
            json_arr_foreach(l, i, x) if (json_is_str(x)) strvec_push(&s->left, json_sv(x));
        }
        if (json_is_arr(r)) {
            strvec_free(&s->right);
            json_arr_foreach(r, i, x) if (json_is_str(x)) strvec_push(&s->right, json_sv(x));
        }
    }
    const char *sep = settings_str("statusLine.separator", NULL);
    if (sep && *sep) s->separator = sep;
    json_t *so = settings_get("statusLine.segmentOptions");
    if (json_is_obj(so)) json_merge(s->opts, so);
}

static void free_settings(sl_settings_t *s) {
    strvec_free(&s->left);
    strvec_free(&s->right);
    json_free(s->opts);
}

static bool seg_bool(const sl_settings_t *s, const char *seg, const char *key, bool def) {
    return json_get_bool(json_get(s->opts, seg), key, def);
}

/* ---------- git (cheap HEAD read on the render path; status refreshed off-thread, 10s TTL) ---------- */

static char *find_git_dir(const char *cwd, char **root) {
    char *d = xstrdup(cwd);
    for (int depth = 0; depth < 64; depth++) {
        char *g = path_join(d, ".git");
        if (fs_is_dir(g)) {
            *root = d;
            return g;
        }
        if (fs_is_file(g)) { /* worktree / submodule: "gitdir: <path>" */
            char *txt = fs_read_limit(g, 4096, NULL, NULL);
            free(g);
            if (txt && str_starts(txt, "gitdir:")) {
                char *t = str_trim(txt + 7);
                char *abs = path_abs(t, d);
                free(t);
                free(txt);
                *root = d;
                return abs;
            }
            free(txt);
            *root = d;
            return NULL;
        }
        free(g);
        if (!strcmp(d, "/") || !*d) break;
        char *p = path_dirname(d);
        free(d);
        d = p;
    }
    free(d);
    *root = NULL;
    return NULL;
}

static char *git_branch(const char *cwd) {
    char *root = NULL;
    char *gd = find_git_dir(cwd, &root);
    free(root);
    if (!gd) return NULL;
    char *head = path_join(gd, "HEAD");
    free(gd);
    char *txt = fs_read_limit(head, 4096, NULL, NULL);
    free(head);
    if (!txt) return NULL;
    char *t = str_trim(txt);
    free(txt);
    char *r;
    if (str_starts(t, "ref:")) {
        char *ref = str_trim(t + 4);
        r = xstrdup(str_starts(ref, "refs/heads/") ? ref + 11 : ref);
        free(ref);
    } else {
        r = xstrdup("detached");
    }
    free(t);
    return r;
}

static void *git_status_thread(void *arg) {
    char *cwd = arg;
    const char *argv[] = {"git", "--no-optional-locks", "status", "--porcelain=v1", "-z", "--untracked-files=normal", NULL};
    char *out = proc_capture(argv, cwd, 5000);
    int st = 0, un = 0, ut = 0;
    bool ok = out != NULL;
    if (out) {
        /* -z records: "XY path\0" (renames carry an extra "\0orig") */
        const char *p = out;
        while (*p) {
            size_t n = strlen(p);
            if (n >= 3) {
                char x = p[0], y = p[1];
                if (x == '?' && y == '?') ut++;
                else {
                    if (x != ' ' && x != '!') st++;
                    if (y != ' ' && y != '!') un++;
                }
                if (x == 'R' || x == 'C') {
                    p += n + 1;
                    if (*p) p += strlen(p);
                }
            }
            p += n + 1;
        }
        free(out);
    }
    pthread_mutex_lock(&g_sl.gmu);
    bool changed = !g_sl.git_have || g_sl.staged != st || g_sl.unstaged != un || g_sl.untracked != ut;
    if (ok) g_sl.staged = st, g_sl.unstaged = un, g_sl.untracked = ut, g_sl.git_have = true;
    g_sl.git_fetched = mono_ms();
    g_sl.git_inflight = false;
    pthread_mutex_unlock(&g_sl.gmu);
    if (changed && g_ui && g_ui->tui) tui_request_render(g_ui->tui);
    free(cwd);
    return NULL;
}

static bool git_status(const char *cwd, int *st, int *un, int *ut) {
    pthread_mutex_lock(&g_sl.gmu);
    bool stale = !g_sl.git_inflight && (!g_sl.git_fetched || mono_ms() - g_sl.git_fetched >= 10000);
    if (stale) {
        g_sl.git_inflight = true;
        pthread_t th;
        if (pthread_create(&th, NULL, git_status_thread, xstrdup(cwd)) == 0) pthread_detach(th);
        else g_sl.git_inflight = false;
    }
    bool have = g_sl.git_have;
    *st = g_sl.staged, *un = g_sl.unstaged, *ut = g_sl.untracked;
    pthread_mutex_unlock(&g_sl.gmu);
    return have;
}

/* ---------- segment context ---------- */

typedef struct {
    ui_t *u;
    app_t *a;
    const sl_settings_t *set;
    const char *title;    /* session title or NULL */
    bool accent;          /* session accent applies */
    uint32_t accent_rgb;  /* when accent */
    double pct;           /* context percent, <0 unknown */
    int64_t window, ctx_tokens;
    bool auto_compact;
    int64_t now;
    int path_max; /* override of path.maxLength (-1 = option) */
} segctx_t;

/* theme.fg for accent-role text: the session accent when enabled, else the theme color */
static char *accent_fg(const segctx_t *c, tui_tc_t slot, const char *s) {
    return c->accent ? fg_rgb(c->accent_rgb, s) : fgw(slot, s);
}

static ai_effort_t effective_effort(const app_t *a) {
    return a->model && a->model->reasoning ? a->thinking : EFFORT_OFF;
}

static char *thinking_display(const segctx_t *c) {
    const app_t *a = c->a;
    if (!seg_bool(c->set, "model", "showThinkingLevel", true) || !a->model || !a->model->reasoning) return xstrdup("");
    if (a->thinking == EFFORT_OFF) return xasprintf("%s off", sym("status.disabled"));
    char key[48];
    snprintf(key, sizeof key, "thinking.%s", effort_name(a->thinking));
    const char *d = sym(key);
    return xstrdup(*d ? d : effort_name(a->thinking));
}

static int64_t turn_elapsed(void) { return g_sl.started ? MAX(0, mono_ms() - g_sl.started) : -1; }

static uint32_t mix_rgb(uint32_t a, uint32_t b, double t) {
    int r = (int)floor(((a >> 16) & 255) + ((((b >> 16) & 255) - (double)((a >> 16) & 255)) * t) + 0.5);
    int g = (int)floor(((a >> 8) & 255) + ((((b >> 8) & 255) - (double)((a >> 8) & 255)) * t) + 0.5);
    int bl = (int)floor((a & 255) + (((b & 255) - (double)(a & 255)) * t) + 0.5);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static uint32_t brand_sample(uint32_t settled, int64_t now) {
    if (!g_sl.fade_start) return settled;
    double t = (double)(now - g_sl.fade_start) / 450.0;
    if (t >= 1) {
        g_sl.fade_start = 0;
        return settled;
    }
    double e = t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
    return mix_rgb(g_sl.fade_from, g_sl.fade_to, e);
}

static void fade_tick(void *ud) {
    ui_t *u = ud;
    if (!g_sl.fade_start || mono_ms() - g_sl.fade_start >= 450) {
        if (g_sl.fade_timer) tui_remove_timer(u->tui, g_sl.fade_timer);
        g_sl.fade_timer = 0;
    }
    tui_request_render(u->tui);
}

static uint32_t brand_rgb(const segctx_t *c, bool working, bool visible) {
    uint32_t idle = tui_theme_rgb(TH(), TUI_TC_DIM);
    uint32_t work = c->accent ? c->accent_rgb : tui_theme_rgb(TH(), TUI_TC_ACCENT);
    int64_t now = mono_ms();
    if (working != g_sl.brand_working) {
        uint32_t prev = g_sl.brand_working ? work : idle;
        g_sl.fade_from = brand_sample(prev, now);
        g_sl.fade_to = working ? work : idle;
        g_sl.fade_start = now;
        g_sl.brand_working = working;
        if (visible && !g_sl.fade_timer && c->u && c->u->tui) g_sl.fade_timer = tui_add_timer(c->u->tui, 40, true, fade_tick, c->u);
    }
    return brand_sample(working ? work : idle, now);
}

/* returns owned content or NULL when not visible */
static char *render_segment(const char *id, const segctx_t *c) {
    const app_t *a = c->a;
    if (str_eq(id, "pi")) {
        int64_t el = turn_elapsed();
        bool vis = strvec_contains(&c->set->left, "pi") || strvec_contains(&c->set->right, "pi");
        uint32_t rgb = brand_rgb(c, el >= 0, vis);
        char *content;
        if (el >= 0) {
            const char *const *frames;
            size_t nf = tui_sym_spinner("activity", &frames);
            const char *fr = nf ? frames[(mono_ms() / 80) % nf] : "";
            int64_t s = el / 1000;
            char t[16];
            if (s < 60) snprintf(t, sizeof t, "%llds", (long long)s);
            else if (s < 3600) snprintf(t, sizeof t, "%lldm", (long long)(s / 60));
            else snprintf(t, sizeof t, "%lldh", (long long)MIN(99, s / 3600));
            content = xasprintf("%s %s", fr, t);
        } else {
            content = xstrdup(sym("icon.omp"));
        }
        char *r = fg_rgb(rgb, content);
        free(content);
        return r;
    }
    if (str_eq(id, "model")) {
        const char *name = a->model ? (a->model->name && *a->model->name ? a->model->name : a->model->id) : "no-model";
        if (str_starts(name, "Claude ")) name += 7;
        char *td = thinking_display(c);
        bool compact = settings_bool("statusLine.compactThinkingLevel", true) && *td;
        char *icon;
        if (compact) {
            char *sp = strchr(td, ' ');
            icon = sp ? xstrndup(td, (size_t)(sp - td)) : xstrdup(td);
        } else {
            icon = xstrdup(sym("icon.model"));
        }
        char *wi = with_icon(icon, name);
        char *content = accent_fg(c, TUI_TC_STATUS_LINE_MODEL, wi);
        if (!compact && *td) {
            char *tail = xasprintf("%s%s", sym("sep.dot"), td);
            char *tc = accent_fg(c, TUI_TC_STATUS_LINE_MODEL, tail);
            char *n = xasprintf("%s%s", content, tc);
            free(content), free(tail), free(tc);
            content = n;
        }
        free(wi), free(icon), free(td);
        return content;
    }
    if (str_eq(id, "mode")) {
        const char *pause = sym("icon.pause");
        if (a->plan_mode) {
            char *w = with_icon(sym("icon.plan"), "Plan");
            char *r = accent_fg(c, TUI_TC_ACCENT, w);
            free(w);
            return r;
        }
        const goal_t *g = app_goal((app_t *)a);
        if (g) {
            const char *st = goal_status(g);
            const char *icon = sym("icon.goal");
            tui_tc_t color = TUI_TC_ACCENT;
            if (str_eq(st, "paused") || !goal_enabled(g)) icon = *pause ? pause : sym("status.pending"), color = TUI_TC_WARNING;
            else if (str_eq(st, "complete")) icon = sym("status.success"), color = TUI_TC_SUCCESS;
            else if (str_eq(st, "budget-limited")) icon = sym("status.warning"), color = TUI_TC_WARNING;
            else if (str_eq(st, "dropped")) icon = sym("status.aborted"), color = TUI_TC_DIM;
            char *w = with_icon(icon, "Goal");
            char *r = color == TUI_TC_ACCENT ? accent_fg(c, color, w) : fgw(color, w);
            free(w);
            return r;
        }
        return NULL;
    }
    if (str_eq(id, "path")) {
        bool strip = seg_bool(c->set, "path", "stripWorkPrefix", true);
        const char *cwd = a->cwd;
        bool scratch = false;
        char *disp = xstrdup(cwd);
        const char *tmp = env_get("TMPDIR");
        char *home_tmp = path_join(home_dir(), "tmp");
        const char *roots[] = {tmp && *tmp ? tmp : "/tmp", home_tmp, "/tmp", "/var/tmp"};
        for (size_t i = 0; i < ARRAY_LEN(roots) && !scratch; i++) {
            char *r = path_normalize(roots[i]);
            size_t rl = strlen(r);
            while (rl > 1 && r[rl - 1] == '/') r[--rl] = 0;
            if (!strcmp(cwd, r) || (str_starts(cwd, r) && cwd[rl] == '/')) {
                scratch = true;
                free(disp);
                disp = xstrdup(cwd[rl] == '/' && cwd[rl + 1] ? cwd + rl + 1 : cwd);
            }
            free(r);
        }
        if (!scratch) {
            char *pr = path_join(home_dir(), "Projects"), *rp = path_join(home_dir(), "repos");
            const char *dr[] = {pr, rp, "/work"};
            for (size_t i = 0; i < 3; i++) {
                size_t rl = strlen(dr[i]);
                if (str_starts(cwd, dr[i]) && cwd[rl] == '/' && cwd[rl + 1]) {
                    free(disp);
                    disp = xstrdup(cwd + rl + 1);
                    break;
                }
            }
            free(pr), free(rp);
        }
        free(home_tmp);
        char *pwd = xstrdup(strip ? disp : cwd);
        free(disp);
        if (seg_bool(c->set, "path", "abbreviate", true)) {
            const char *h = home_dir();
            size_t hl = h ? strlen(h) : 0;
            if (hl > 1 && str_starts(pwd, h) && (pwd[hl] == 0 || pwd[hl] == '/')) {
                char *n = xasprintf("~%s", pwd + hl);
                free(pwd);
                pwd = n;
            }
        }
        int maxl = c->path_max >= 0 ? c->path_max : (int)json_get_int(json_get(c->set->opts, "path"), "maxLength", 40);
        /* clampPathLength counts UTF-16 units; paths are near-always BMP */
        size_t ulen = utf8_strlen(pwd);
        if ((int)ulen > maxl) {
            size_t keep = (size_t)MAX(0, maxl - 1), skip = ulen - keep;
            const char *p = pwd;
            while (skip && *p) {
                uint32_t cp;
                p += utf8_decode(p, strlen(p), &cp);
                skip--;
            }
            char *n = xasprintf("\xe2\x80\xa6%s", p);
            free(pwd);
            pwd = n;
        }
        char *w = with_icon(scratch && strip ? sym("icon.scratchFolder") : sym("icon.folder"), pwd);
        char *r = fgw(TUI_TC_STATUS_LINE_PATH, w);
        free(w), free(pwd);
        return r;
    }
    if (str_eq(id, "git")) {
        if (!settings_bool("git.enabled", true)) return NULL;
        char *branch = git_branch(a->cwd);
        if (!branch) return NULL;
        int st = 0, un = 0, ut = 0;
        bool have = git_status(a->cwd, &st, &un, &ut);
        bool dirty = have && (st || un || ut);
        sb_t b = SB_INIT;
        bool show_branch = seg_bool(c->set, "git", "showBranch", true);
        if (show_branch) {
            char *w = with_icon(sym("icon.branch"), branch);
            sb_append(&b, w);
            free(w);
        }
        if (have) {
            strvec_t ind = {0};
            char t[32];
            if (seg_bool(c->set, "git", "showUnstaged", true) && un) snprintf(t, sizeof t, "*%d", un), strvec_push_own(&ind, fgw(TUI_TC_STATUS_LINE_DIRTY, t));
            if (seg_bool(c->set, "git", "showStaged", true) && st) snprintf(t, sizeof t, "+%d", st), strvec_push_own(&ind, fgw(TUI_TC_STATUS_LINE_STAGED, t));
            if (seg_bool(c->set, "git", "showUntracked", true) && ut) snprintf(t, sizeof t, "?%d", ut), strvec_push_own(&ind, fgw(TUI_TC_STATUS_LINE_UNTRACKED, t));
            if (ind.n) {
                char *it = strvec_join(&ind, " ");
                if (!b.len && !show_branch) {
                    char *w = with_icon(sym("icon.git"), it);
                    sb_append(&b, w);
                    free(w);
                } else {
                    if (b.len) sb_putc(&b, ' ');
                    sb_append(&b, it);
                }
                free(it);
            }
            strvec_free(&ind);
        }
        free(branch);
        if (!b.len) {
            sb_free(&b);
            return NULL;
        }
        char *r = fgw(dirty ? TUI_TC_STATUS_LINE_GIT_DIRTY : TUI_TC_STATUS_LINE_GIT_CLEAN, b.s);
        sb_free(&b);
        return r;
    }
    if (str_eq(id, "context_pct")) {
        double pct = c->pct < 0 ? 0 : c->pct;
        tui_tc_t color = TUI_TC_STATUS_LINE_CONTEXT;
        /* getContextUsageLevel */
        double w = (double)c->window;
        double thr[3][2] = {{90, 500000}, {70, 270000}, {50, 150000}};
        tui_tc_t cols[3] = {TUI_TC_ERROR, TUI_TC_THINKING_HIGH, TUI_TC_WARNING};
        for (int i = 0; i < 3; i++) {
            if (!(pct > 0)) break;
            double t = w > 0 ? MIN(thr[i][0], thr[i][1] / w * 100) : thr[i][0];
            if (pct >= t) {
                color = cols[i];
                break;
            }
        }
        char *txt;
        if (c->pct < 0 && c->window > 0) txt = fmt_number((double)c->window);
        else if (c->window <= 0) {
            char *n = fmt_number((double)c->ctx_tokens);
            txt = xasprintf("%s/?", n);
            free(n);
        } else {
            char *n = fmt_number((double)c->window);
            txt = xasprintf("%.1f%%/%s", c->pct, n);
            free(n);
        }
        char *t = fgw(color, txt);
        free(txt);
        char *autoi = xstrdup("");
        if (c->auto_compact && *sym("icon.auto")) {
            char *ai = fgw(color, sym("icon.auto"));
            free(autoi);
            autoi = xasprintf(" %s", ai);
            free(ai);
        }
        char *body = xasprintf("%s%s", t, autoi);
        char *r = with_icon(sym("icon.context"), body);
        free(t), free(autoi), free(body);
        return r;
    }
    if (str_eq(id, "context_total")) {
        if (!c->window) return NULL;
        char *n = fmt_number((double)c->window);
        char *w = with_icon(sym("icon.context"), n);
        char *r = fgw(TUI_TC_STATUS_LINE_CONTEXT, w);
        free(n), free(w);
        return r;
    }
    if (str_eq(id, "cost")) {
        if (!(a->total_cost > 0)) return NULL;
        char t[48];
        snprintf(t, sizeof t, "$%.2f", a->total_cost);
        return fgw(TUI_TC_STATUS_LINE_COST, t);
    }
    if (str_eq(id, "session_name")) {
        if (!c->title) return NULL;
        char *s = sanitize_status(c->title);
        char *r = accent_fg(c, TUI_TC_ACCENT, s);
        free(s);
        return r;
    }
    if (str_eq(id, "session")) {
        const char *sid = a->session ? session_id(a->session) : NULL;
        char d[16];
        snprintf(d, sizeof d, "%.8s", sid && *sid ? sid : "new");
        return with_icon(sym("icon.session"), d);
    }
    if (str_eq(id, "hostname")) {
        char h[256] = "";
        gethostname(h, sizeof h - 1);
        char *dot = strchr(h, '.');
        if (dot) *dot = 0;
        char *w = with_icon(sym("icon.host"), h);
        if (!c->accent) return w;
        char *r = fg_rgb(c->accent_rgb, w);
        free(w);
        return r;
    }
    if (str_eq(id, "time") || str_eq(id, "time_spent")) {
        if (str_eq(id, "time_spent")) {
            int64_t ms = g_sl.active_ms + (g_sl.started ? mono_ms() - g_sl.started : 0);
            if (ms < 1000) return NULL;
            char t[32];
            if (ms < 60000) {
                int64_t tenths = (ms + 50) / 100;
                if (tenths < 600) snprintf(t, sizeof t, "%.1fs", tenths / 10.0);
                else snprintf(t, sizeof t, "1m");
            } else if (ms < 3600000) {
                int64_t m = ms / 60000, s = (ms % 60000) / 1000;
                if (s) snprintf(t, sizeof t, "%lldm%llds", (long long)m, (long long)s);
                else snprintf(t, sizeof t, "%lldm", (long long)m);
            } else {
                int64_t h = ms / 3600000, m = (ms % 3600000) / 60000;
                if (m) snprintf(t, sizeof t, "%lldh%lldm", (long long)h, (long long)m);
                else snprintf(t, sizeof t, "%lldh", (long long)h);
            }
            return with_icon(sym("icon.time"), t);
        }
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        json_t *to = json_get(c->set->opts, "time");
        bool h12 = str_eq(json_get_str(to, "format"), "12h");
        int hours = tm.tm_hour;
        const char *suffix = "";
        if (h12) {
            suffix = hours >= 12 ? "pm" : "am";
            hours = hours % 12 ? hours % 12 : 12;
        }
        char t[32];
        if (json_get_bool(to, "showSeconds", false)) snprintf(t, sizeof t, "%d:%02d:%02d%s", hours, tm.tm_min, tm.tm_sec, suffix);
        else snprintf(t, sizeof t, "%d:%02d%s", hours, tm.tm_min, suffix);
        return with_icon(sym("icon.time"), t);
    }
    if (str_starts(id, "token_") || str_starts(id, "cache_")) {
        json_t *st = app_stats((app_t *)a);
        double in = json_get_num(st, "input", 0), out = json_get_num(st, "output", 0), cr = json_get_num(st, "cacheRead", 0),
               cw = json_get_num(st, "cacheWrite", 0);
        json_free(st);
        double v = 0;
        const char *icon = "";
        tui_tc_t color = TUI_TC_STATUS_LINE_SPEND;
        if (str_eq(id, "token_in")) v = in, icon = sym("icon.input");
        else if (str_eq(id, "token_out")) v = out, icon = sym("icon.output"), color = TUI_TC_STATUS_LINE_OUTPUT;
        else if (str_eq(id, "token_total")) v = in + out + cw, icon = sym("icon.tokens");
        else if (str_eq(id, "cache_read")) v = cr, icon = sym("icon.cache");
        else if (str_eq(id, "cache_write")) v = cw, icon = sym("icon.cache");
        else if (str_eq(id, "cache_hit")) {
            if (!cr) return NULL;
            char t[32];
            snprintf(t, sizeof t, "%.2f%%", cr / (cr + cw + in) * 100);
            char *p = fgw(TUI_TC_STATUS_LINE_SPEND, t);
            char *r = xasprintf("%s %s", sym("icon.cache"), p);
            free(p);
            return r;
        } else return NULL; /* token_rate: no live meter yet */
        if (!(v > 0)) return NULL;
        char *n = fmt_number(v);
        char *w = with_icon(icon, n);
        char *r = fgw(color, w);
        free(n), free(w);
        return r;
    }
    return NULL; /* vim, collab, stream, pr, subagents, status, usage: nothing to show */
}

/* ---------- context estimate ---------- */

/* Tokens of the request before any provider usage: system prompt + tool schemas, estimated
 * at ~4.1 characters per token (omp counts with its native tokenizer; 4.1 matches it on
 * the default system prompt + tool schemas). */
static int64_t estimate_context(const app_t *a) {
    agent_t *ag = a->agent;
    size_t chars = 0;
    pthread_mutex_lock(&ag->mu);
    json_t *x;
    json_arr_foreach(ag->system_prompt, i, x) if (json_is_str(x)) chars += strlen(json_sv(x));
    for (size_t i = 0; i < ag->tools.n; i++) {
        json_t *w = agent_tool_wire(ag->tools.v[i]);
        char *s = json_dump(json_get(w, "parameters"), 0);
        chars += strlen(s) + strlen(json_get_str(w, "name") ? json_get_str(w, "name") : "") +
                 strlen(json_get_str(w, "description") ? json_get_str(w, "description") : "");
        free(s);
        json_free(w);
    }
    pthread_mutex_unlock(&ag->mu);
    return (int64_t)((double)chars / 4.1);
}

static void fill_context(segctx_t *c) {
    app_t *a = c->a;
    c->window = a->model ? a->model->context_window : 0;
    int64_t used = a->last_context_tokens > 0 ? a->last_context_tokens : estimate_context(a);
    c->ctx_tokens = used;
    c->pct = c->window > 0 ? (double)used / (double)c->window * 100 : -1;
    c->auto_compact = settings_bool("compaction.enabled", true);
}

/* auto-compaction trigger and speculation start, percent of the window (-1 = none) */
static void compaction_bounds(int64_t window, double *thr, double *spec) {
    *thr = *spec = -1;
    if (window <= 0 || !settings_bool("compaction.enabled", true)) return;
    int64_t abs_t = settings_int("compaction.thresholdTokens", 0), t;
    int64_t reserve = MAX((int64_t)floor(window * 0.15), settings_int("compaction.reserveTokens", 16384));
    if (reserve >= window) reserve = MAX(1, (int64_t)floor(window * 0.15));
    int64_t budget = MAX(0, MIN(window - 1, window - reserve));
    if (abs_t > 0) t = abs_t >= window ? budget : MIN(window - 1, abs_t);
    else {
        double p = settings_num("compaction.thresholdPercent", -1);
        t = p > 0 ? MIN(budget, (int64_t)floor(window * MIN(99.0, MAX(1.0, p)) / 100)) : budget;
    }
    if (t <= 0 || t > window) return;
    *thr = (double)t / window * 100;
    int64_t lead = MIN(32000, MAX(8192, (int64_t)floor(t * 0.125)));
    *spec = (double)MAX(0, t - lead) / window * 100;
}

static char *fmt_embedded_pct(double p) {
    return p > 0 && p < 1 ? xasprintf("%.1f%%", p) : xasprintf("%.0f%%", floor(p + 0.5));
}

static int embedded_min_width(double pct, int64_t window) {
    int w = 0;
    if (pct >= 0) {
        char *s = fmt_embedded_pct(pct);
        w = (int)strlen(s) + 2;
        free(s);
    }
    char *n = fmt_number((double)window);
    w += (int)strlen(n) + 2;
    free(n);
    return w;
}

static char *repeat(const char *g, int n) {
    sb_t b = SB_INIT;
    for (int i = 0; i < n; i++) sb_append(&b, g);
    return sb_detach(&b);
}

static char *gauge_fill(int gap, const segctx_t *c, bool embed) {
    char ub[32], tb[32];
    const char *used = c->accent ? tui_rgb_fg(TH(), c->accent_rgb, ub, sizeof ub) : tui_fg_seq(TH(), TUI_TC_BORDER_ACCENT);
    const char *H = sym("boxRound.horizontal");
    const char *mode = settings_str("statusLine.contextLine", "embedded");
    double pct = c->pct;
    if (str_eq(mode, "off") || (pct < 0 && c->window <= 0)) {
        char *r = repeat(H, gap);
        char *o = xasprintf("\x1b[49m%s%s\x1b[39m", used, r);
        free(r);
        return o;
    }
    double clamped = pct < 0 ? 0 : MIN(100.0, MAX(0.0, pct));
    char *plabel = xstrdup(""), *wlabel = xstrdup("");
    int pstart = -1, wstart = -1, scale = gap;
    bool overflow = pct > 100;
    if (embed && gap >= embedded_min_width(pct, c->window)) {
        free(plabel), free(wlabel);
        plabel = pct < 0 ? xstrdup("") : fmt_embedded_pct(overflow ? pct : clamped);
        wlabel = fmt_number((double)c->window);
        if (overflow) {
            pstart = gap - (int)strlen(plabel);
            wstart = pstart - 1 - (int)strlen(wlabel);
        } else {
            wstart = gap - (int)strlen(wlabel) - 1;
        }
        scale = wstart;
    }
    int ucount = MIN(scale, MAX(1, (int)floor(clamped / 100 * scale + 0.5)));
    int spec_i = -1, thr_i = -1;
    if ((str_eq(mode, "annotated") || str_eq(mode, "embedded")) && c->auto_compact && gap >= 8) {
        double thr, spec;
        compaction_bounds(c->window, &thr, &spec);
        if (thr >= 0) {
#define CELL(p) MIN(scale - 1, MAX(0, (int)floor((p) / 100 * scale + 0.5)))
            thr_i = CELL(thr);
            if (spec >= 0) spec_i = CELL(spec);
#undef CELL
            if (spec_i == thr_i) spec_i = -1;
        }
    }
    int plen = (int)strlen(plabel), wlen = (int)strlen(wlabel);
    if (plen && pstart < 0) {
        int maxs = scale - plen - 1, pref = MIN(maxs, MAX(1, ucount));
        for (int d = 0; d <= maxs; d++) {
            int l = pref - d;
#define OVL(s) ((spec_i >= (s) && spec_i < (s) + plen) || (thr_i >= (s) && thr_i < (s) + plen))
            if (l >= 1 && !OVL(l)) {
                pstart = l;
                break;
            }
            if (!d) continue;
            int r = pref + d;
            if (r <= maxs && !OVL(r)) {
                pstart = r;
                break;
            }
#undef OVL
        }
    }
    uint32_t raw = c->accent ? c->accent_rgb : tui_theme_rgb(TH(), TUI_TC_BORDER_ACCENT);
    const char *thr_col = tui_rgb_fg(TH(), tui_adjust_hsv(raw, 0, 0.7, 0.75), tb, sizeof tb);
    const char *unused = tui_fg_seq(TH(), TUI_TC_BORDER), *specc = tui_fg_seq(TH(), TUI_TC_MUTED),
               *ovc = tui_fg_seq(TH(), TUI_TC_ERROR);
    int pend = pstart >= 0 ? pstart + plen : -1, wend = wstart >= 0 ? wstart + wlen : -1;
    sb_t out = SB_INIT;
    sb_append(&out, "\x1b[49m");
    const char *active = NULL;
    int i = 0;
    while (i < gap) {
        const char *color;
        char *text;
        if (i >= pstart && i < pend) {
            int e = MIN(pend, gap);
            color = overflow ? ovc : used;
            text = xstrndup(plabel + (i - pstart), (size_t)(e - i));
            i = e;
        } else if (i == thr_i) {
            color = thr_col;
            text = xstrdup(sym("context.compaction"));
            i++;
        } else if (i == spec_i) {
            color = specc;
            text = xstrdup(sym("context.speculation"));
            i++;
        } else {
            bool inw = i >= wstart && i < wend;
            int e = MIN(inw ? wend : i < ucount ? ucount : gap, gap);
            if (pstart > i && pstart < e) e = pstart;
            if (thr_i > i && thr_i < e) e = thr_i;
            if (spec_i > i && spec_i < e) e = spec_i;
            if (!inw && wstart > i && wstart < e) e = wstart;
            if (inw) {
                color = thr_col;
                text = xstrndup(wlabel + (i - wstart), (size_t)(e - i));
            } else {
                color = i < ucount ? used : unused;
                text = repeat(H, e - i);
            }
            i = e;
        }
        if (!active || strcmp(color, active)) {
            sb_append(&out, color);
            active = color;
        }
        sb_append(&out, text);
        free(text);
    }
    sb_append(&out, "\x1b[39m");
    free(plabel), free(wlabel);
    return sb_detach(&out);
}

/* ---------- layouts ---------- */

typedef enum { L_BOX, L_BAND, L_PLAIN_FULL, L_PLAIN_LEFT, L_PLAIN_RIGHT } layout_t;

typedef struct {
    const char *left, *right;      /* separators */
    const char *cap_l, *cap_r;     /* endCaps.left / endCaps.right; NULL = none */
} sepdef_t;

static sepdef_t get_sep(const char *style, bool plain) {
    static char slash[8], pipe_[8];
    if (plain) return (sepdef_t){"\xc2\xb7", "\xc2\xb7", NULL, NULL};
    if (str_eq(style, "powerline")) return (sepdef_t){sym("sep.powerlineLeft"), sym("sep.powerlineRight"), sym("sep.powerlineRight"), sym("sep.powerlineLeft")};
    if (str_eq(style, "slash") || str_eq(style, "pipe")) {
        char *t = str_trim(sym(str_eq(style, "slash") ? "sep.slash" : "sep.pipe"));
        char *dst = str_eq(style, "slash") ? slash : pipe_;
        snprintf(dst, 8, "%s", t);
        free(t);
        return (sepdef_t){dst, dst, NULL, NULL};
    }
    if (str_eq(style, "block")) return (sepdef_t){sym("sep.block"), sym("sep.block"), NULL, NULL};
    if (str_eq(style, "none")) return (sepdef_t){sym("sep.space"), sym("sep.space"), NULL, NULL};
    if (str_eq(style, "ascii")) return (sepdef_t){sym("sep.asciiLeft"), sym("sep.asciiRight"), NULL, NULL};
    return (sepdef_t){sym("sep.powerlineThinLeft"), sym("sep.powerlineThinRight"), sym("sep.powerlineRight"), sym("sep.powerlineLeft")};
}

static bool is_ctx_seg(const char *id) { return str_eq(id, "context_pct") || str_eq(id, "context_total"); }

static void build_ctx(segctx_t *c, ui_t *u, const sl_settings_t *set) {
    memset(c, 0, sizeof *c);
    c->u = u;
    c->a = u->app;
    c->set = set;
    c->title = u->app->session ? u->app->session->title : NULL;
    if (c->title && !*c->title) c->title = NULL;
    c->accent = c->title && settings_bool("statusLine.sessionAccent", true);
    if (c->accent) c->accent_rgb = tui_session_accent(c->title, TH());
    c->path_max = -1;
    c->now = now_ms();
    fill_context(c);
}

static int group_width(const int *w, int n, int cap, int sepw) {
    if (!n) return 0;
    int s = 0;
    for (int i = 0; i < n; i++) s += w[i];
    return s + (n - 1) * (sepw + 2) + 2 + cap;
}

static char *render_group(strvec_t *parts, bool left, const sepdef_t *sd, bool transparent, const char *bg, const char *fgt,
                          const char *sepc, const char *band_cap) {
    if (!parts->n) return xstrdup("");
    const char *sep = left ? sd->left : sd->right;
    const char *cap = sd->cap_l && !transparent ? (left ? sd->cap_r : sd->cap_l) : "";
    char prefix[64];
    if (str_starts(bg, "\x1b[48;")) snprintf(prefix, sizeof prefix, "\x1b[38;%s", bg + 5);
    else snprintf(prefix, sizeof prefix, "%s", bg); /* "\x1b[49m": no fill to borrow */
    char *cap_text = *cap ? xasprintf("%s%s\x1b[0m", prefix, cap) : xstrdup("");
    char *open = left && band_cap && *band_cap ? xasprintf("%s%s\x1b[0m", prefix, band_cap) : xstrdup("");
    char *joiner = xasprintf(" %s%s%s ", sepc, sep, fgt);
    char *joined = strvec_join(parts, joiner);
    char *content = xasprintf("%s%s %s \x1b[0m", bg, fgt, joined);
    char *r = left ? xasprintf("%s%s%s", open, content, cap_text) : xasprintf("%s%s", cap_text, content);
    free(cap_text), free(open), free(joiner), free(joined), free(content);
    return r;
}

static char *render_status(ui_t *u, int width, layout_t layout) {
    sl_settings_t set;
    load_settings(&set);
    bool plain = layout != L_BOX && layout != L_BAND;
    segctx_t c;
    build_ctx(&c, u, &set);
    sepdef_t sd = get_sep(set.separator, plain);
    bool transparent_layout = layout == L_PLAIN_FULL || layout == L_PLAIN_LEFT;
    const char *bg = transparent_layout || settings_bool("statusLine.transparent", false) ? "\x1b[49m" : tui_bg_seq(TH(), TUI_TC_STATUS_LINE_BG);
    bool transparent = !strcmp(bg, "\x1b[49m");
    const char *fgt = tui_fg_seq(TH(), TUI_TC_TEXT), *sepc = tui_fg_seq(TH(), TUI_TC_STATUS_LINE_SEP);

    strvec_t left = {0}, right = {0}, lids = {0}, rids = {0};
    if (layout != L_PLAIN_RIGHT)
        for (size_t i = 0; i < set.left.n; i++) {
            if (layout == L_BAND && str_eq(set.left.v[i], "session_name")) continue;
            char *r = render_segment(set.left.v[i], &c);
            if (r && *r) strvec_push_own(&left, r), strvec_push(&lids, set.left.v[i]);
            else free(r);
        }
    if (layout != L_PLAIN_LEFT)
        for (size_t i = 0; i < set.right.n; i++) {
            if (layout == L_BAND && str_eq(set.right.v[i], "session_name")) continue;
            char *r = render_segment(set.right.v[i], &c);
            if (r && *r) strvec_push_own(&right, r), strvec_push(&rids, set.right.v[i]);
            else free(r);
        }
    bool has_ctx = false, has_non = false;
    for (size_t i = 0; i < lids.n; i++) is_ctx_seg(lids.v[i]) ? (has_ctx = true) : (has_non = true);
    for (size_t i = 0; i < rids.n; i++) is_ctx_seg(rids.v[i]) ? (has_ctx = true) : (has_non = true);
    bool embed = !plain && str_eq(settings_str("statusLine.contextLine", "embedded"), "embedded") && c.window > 0 && has_ctx && has_non;
    if (embed) {
        for (int side = 0; side < 2; side++) {
            strvec_t *p = side ? &right : &left, *ids = side ? &rids : &lids;
            size_t w = 0;
            for (size_t i = 0; i < ids->n; i++) {
                if (is_ctx_seg(ids->v[i])) {
                    free(p->v[i]);
                    free(ids->v[i]);
                    continue;
                }
                p->v[w] = p->v[i];
                ids->v[w] = ids->v[i];
                w++;
            }
            p->n = ids->n = w;
        }
    }
    int W = MAX(0, width);
    int lw[64], rw[64];
    int nl = (int)MIN(left.n, 64), nr = (int)MIN(right.n, 64);
    left.n = (size_t)nl, right.n = (size_t)nr;
    for (int i = 0; i < nl; i++) lw[i] = tui_visible_width(left.v[i]);
    for (int i = 0; i < nr; i++) rw[i] = tui_visible_width(right.v[i]);
    int lsw = tui_visible_width(sd.left), rsw = tui_visible_width(sd.right);
    bool caps = sd.cap_l && !transparent;
    int lcap = caps ? tui_visible_width(sd.cap_r) : 0, rcap = caps ? tui_visible_width(sd.cap_l) : 0;
    const char *band_cap = layout == L_BAND && caps ? sym("sep.powerlineCapLeft") : "";
    int bcap = *band_cap ? tui_visible_width(band_cap) : 0;
    int lwidth = group_width(lw, nl, lcap + bcap, lsw), rwidth = group_width(rw, nr, rcap, rsw);
    int emb_w = embed ? embedded_min_width(c.pct, c.window) : 0;
#define MINGAP() (!emb_w ? ((nl > 0 && nr > 0) ? 1 : 0) : (nl + nr == 1 && lwidth + rwidth + emb_w > W) ? 1 : emb_w)
#define TOTAL() (lwidth + rwidth + MINGAP())
    if (W > 0) {
        int ni = -1;
        for (size_t i = 0; i < rids.n; i++)
            if (str_eq(rids.v[i], "session_name")) ni = (int)i;
        if (ni >= 0 && TOTAL() > W) {
            int cur = rw[ni], shrink = MIN(MAX(0, cur - 8), TOTAL() - W);
            if (shrink > 0) {
                char *t = tui_truncate_to_width(right.v[ni], cur - shrink, NULL);
                free(right.v[ni]);
                right.v[ni] = t;
                int nw = tui_visible_width(t);
                rwidth += nw - cur;
                rw[ni] = nw;
            }
        }
        while (TOTAL() > W && nr > 0) {
            int rem = rw[nr - 1];
            free(right.v[--nr]);
            right.n = (size_t)nr;
            rwidth = nr > 0 ? rwidth - rem - rsw - 2 : 0;
        }
        int pi = -1;
        for (int i = 0; i < nl; i++)
            if (str_eq(lids.v[i], "path")) pi = i;
        if (pi >= 0 && TOTAL() > W) {
            int over = TOTAL() - W, cur = lw[pi], shrinkable = cur - 8;
            if (shrinkable > 0) {
                int by = MIN(shrinkable, over);
                int curmax = (int)json_get_int(json_get(set.opts, "path"), "maxLength", 40);
                int nm = MAX(4, MIN(curmax, cur) - by);
                c.path_max = nm;
                char *rr = render_segment("path", &c);
                if (rr) {
                    int pw = tui_visible_width(rr);
                    for (int k = 0; k < 8; k++) {
                        int saved = cur - pw;
                        if (saved >= by) break;
                        int next = MAX(4, nm - (by - saved));
                        if (next >= nm) break;
                        nm = next;
                        c.path_max = nm;
                        char *adj = render_segment("path", &c);
                        if (!adj) break;
                        free(rr);
                        rr = adj;
                        pw = tui_visible_width(rr);
                    }
                    free(left.v[pi]);
                    left.v[pi] = rr;
                    lwidth += pw - cur;
                    lw[pi] = pw;
                }
                c.path_max = -1;
            }
        }
        while (TOTAL() > W && nl > 0) {
            int di = nl - 1;
            for (int i = nl - 1; i >= 0; i--)
                if (!str_eq(lids.v[i], "path")) {
                    di = i;
                    break;
                }
            int rem = lw[di];
            free(left.v[di]);
            free(lids.v[di]);
            memmove(&left.v[di], &left.v[di + 1], sizeof(char *) * (size_t)(nl - di - 1));
            memmove(&lids.v[di], &lids.v[di + 1], sizeof(char *) * (size_t)(nl - di - 1));
            memmove(&lw[di], &lw[di + 1], sizeof(int) * (size_t)(nl - di - 1));
            nl--;
            left.n = lids.n = (size_t)nl;
            lwidth = nl > 0 ? lwidth - rem - lsw - 2 : 0;
        }
    }
#undef TOTAL
#undef MINGAP
    char *lg = render_group(&left, true, &sd, transparent, bg, fgt, sepc, band_cap);
    char *rg = render_group(&right, false, &sd, transparent, bg, fgt, sepc, NULL);
    char *res;
    if (!*lg && !*rg) res = xstrdup("");
    else if (W == 0 || (plain && (!nl || !nr))) res = xasprintf("%s%s%s", lg, *lg && *rg ? " " : "", rg);
    else {
        int gap = MAX(1, W - lwidth - rwidth);
        if (plain) {
            char *sp = repeat(" ", gap);
            res = xasprintf("%s%s%s", lg, sp, rg);
            free(sp);
        } else {
            char *g = gauge_fill(gap, &c, embed);
            res = xasprintf("%s%s%s", lg, g, rg);
            free(g);
        }
    }
    free(lg), free(rg);
    strvec_free(&left), strvec_free(&right), strvec_free(&lids), strvec_free(&rids);
    free_settings(&set);
    return res;
}

/* editor top chrome provider */
static char *status_top(void *ud, int avail, int *width) {
    ui_t *u = ud;
    const tui_composer_style_t *st = tui_composer_style(composer_shape());
    layout_t l = st->status_attachment == TUI_ATTACH_TOP_BAND        ? L_BAND
                 : st->status_attachment == TUI_ATTACH_TOP_RULE_CHIP ? L_PLAIN_RIGHT
                                                                     : L_BOX;
    char *s = render_status(u, avail, l);
    *width = tui_visible_width(s);
    return s;
}

char *ui_status_render(ui_t *u, int width, const char *layout) {
    layout_t l = str_eq(layout, "box") ? L_BOX : str_eq(layout, "band") ? L_BAND : str_eq(layout, "plain-left") ? L_PLAIN_LEFT
               : str_eq(layout, "plain-right") ? L_PLAIN_RIGHT : L_PLAIN_FULL;
    return render_status(u, width, l);
}

/* ---------- standalone bottom bar (pi / borderless / claude / rule / field / rail) ---------- */

static void status_render(tui_component_t *c, int width, strvec_t *out) {
    ui_t *u = c->user;
    const tui_composer_style_t *st = tui_composer_style(composer_shape());
    if (st->bottom_bar == TUI_BOTTOM_NONE || tui_editor_autocomplete_open(u->editor)) return;
    char *s = render_status(u, width, st->bottom_bar == TUI_BOTTOM_LEFT ? L_PLAIN_LEFT : L_PLAIN_FULL);
    if (*s) {
        if (st->bottom_bar_gap) strvec_push(out, "");
        strvec_push_own(out, s);
    } else {
        free(s);
    }
}

static const tui_component_vt STATUS_VT = {"status", status_render, NULL, NULL, NULL};

tui_component_t *ui_status_new(ui_t *u) {
    tui_component_t *c = xcalloc(1, sizeof(tui_component_t));
    tui_component_init(c, &STATUS_VT);
    c->user = u;
    return c;
}

/* ---------- working / idle status row and the editor top gap ---------- */

/* band composer: the title docks right-aligned, dim italic, on the working row */
char *ui_title_trailer(ui_t *u) {
    if (!str_eq(composer_shape(), "band")) return NULL;
    const char *t = u->app->session ? u->app->session->title : NULL;
    if (!t || !*t) return NULL;
    char *s = sanitize_status(t);
    char *r = xasprintf("\x1b[2;3m%s\x1b[23;22m", s);
    free(s);
    return r;
}

typedef struct {
    tui_component_t base;
    ui_t *u;
    tui_component_t *inner; /* the loader */
} statusrow_t;

/* shimmerText (classic mode): a cosine band sweeping at 30 cells/s over low/mid/high tiers */
static char *shimmer(const char *text, const char *low, const char *mid, const char *high) {
    int total = (int)utf8_strlen(text);
    if (!total) return xstrdup("");
    double travel = (double)now_ms() / 1000.0 * 30.0;
    double pos = fmod(travel, total + 20);
    double lo = pos - 10 - 6, hi = pos - 10 + 6;
    sb_t b = SB_INIT;
    int run = -1, idx = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp;
        int k = utf8_decode(p, strlen(p), &cp);
        int tier = 0;
        if (idx >= lo && idx <= hi) {
            double dist = fabs(idx + 10 - pos);
            double in = dist >= 6 ? 0 : 0.5 * (1 + cos(M_PI * dist / 6));
            tier = in >= 0.65 ? 2 : in >= 0.22 ? 1 : 0;
        }
        if (tier != run) {
            if (run == 2) sb_append(&b, "\x1b[22m\x1b[39m");
            else if (run >= 0) sb_append(&b, "\x1b[39m");
            if (tier == 2) sb_append(&b, "\x1b[1m");
            sb_append(&b, tier == 0 ? low : tier == 1 ? mid : high);
            run = tier;
        }
        sb_appendn(&b, p, (size_t)k);
        p += k;
        idx++;
    }
    sb_append(&b, run == 2 ? "\x1b[22m\x1b[39m" : "\x1b[39m");
    return sb_detach(&b);
}

/* the working row (omp Loader with the interrupt key as its one-frame spinner): a blank line,
 * then ` ⎋ Working…` with the message shimmering and the title docked right */
static void working_row(ui_t *u, const char *message, int width, strvec_t *out) {
    const char *title = u->app->session ? u->app->session->title : NULL;
    bool accent = title && *title && settings_bool("statusLine.sessionAccent", true);
    char mb[32], db[32];
    const char *low = tui_fg_seq(TH(), TUI_TC_DIM), *mid = tui_fg_seq(TH(), TUI_TC_MUTED), *high = tui_fg_seq(TH(), TUI_TC_ACCENT);
    char *key = ui_key_hint("escape");
    char *frame = xasprintf(" %s", key);
    free(key);
    char *spin;
    if (accent) {
        uint32_t a = tui_session_accent(title, TH());
        tui_rgb_fg(TH(), a, mb, sizeof mb);
        tui_rgb_fg(TH(), tui_adjust_hsv(a, 0, 0.55, 0.65), db, sizeof db);
        spin = xasprintf("%s%s\x1b[39m", db, frame);
        mid = high = mb;
    } else {
        spin = fgw(TUI_TC_MUTED, frame);
    }
    char *msg = shimmer(message, low, mid, high);
    sb_t b = SB_INIT;
    sb_putc(&b, ' ');
    sb_append(&b, spin);
    sb_putc(&b, ' ');
    sb_append(&b, msg);
    int body_w = 1 + tui_visible_width(frame) + 1 + tui_visible_width(message);
    char *trailer = ui_title_trailer(u);
    if (trailer) {
        int gap = width - body_w - tui_visible_width(trailer);
        if (gap >= 2) {
            tui_sb_spaces(&b, gap);
            sb_append(&b, trailer);
        }
        free(trailer);
    }
    strvec_push(out, "");
    char *line = sb_detach(&b);
    if (tui_visible_width(line) > width) {
        char *t = tui_truncate_to_width(line, width, "");
        free(line);
        line = t;
    }
    strvec_push_own(out, line);
    free(frame), free(spin), free(msg);
}

static void statusrow_render(tui_component_t *c, int width, strvec_t *out) {
    statusrow_t *r = (statusrow_t *)c;
    size_t before = out->n;
    if (r->inner && !r->inner->hidden) {
        tui_loader_t *l = (tui_loader_t *)r->inner;
        working_row(r->u, l->message ? l->message : "Working\xe2\x80\xa6", width, out);
    }
    if (out->n == before) {
        char *t = ui_title_trailer(r->u);
        if (t) {
            strvec_push(out, "");
            sb_t b = SB_INIT;
            tui_sb_spaces(&b, MAX(0, width - tui_visible_width(t)));
            sb_append(&b, t);
            strvec_push_own(out, sb_detach(&b));
            free(t);
        }
    }
    r->u->status_row_occupied = out->n > before;
}

static void statusrow_invalidate(tui_component_t *c) {
    statusrow_t *r = (statusrow_t *)c;
    if (r->inner) tui_component_invalidate(r->inner);
}

static void statusrow_destroy(tui_component_t *c) {
    statusrow_t *r = (statusrow_t *)c;
    if (r->inner) tui_component_destroy(r->inner);
    free(r);
}

static const tui_component_vt STATUSROW_VT = {"status-row", statusrow_render, NULL, statusrow_invalidate, statusrow_destroy};

tui_component_t *ui_status_row_new(ui_t *u, tui_component_t *loader) {
    statusrow_t *r = xcalloc(1, sizeof *r);
    tui_component_init(&r->base, &STATUSROW_VT);
    r->u = u;
    r->inner = loader;
    return &r->base;
}

static void gap_render(tui_component_t *c, int width, strvec_t *out) {
    ui_t *u = c->user;
    if (str_eq(composer_shape(), "band") && u->status_row_occupied) return;
    strvec_push(out, "");
}

static const tui_component_vt GAP_VT = {"editor-top-gap", gap_render, NULL, NULL, NULL};

tui_component_t *ui_editor_gap_new(ui_t *u) {
    tui_component_t *c = xcalloc(1, sizeof *c);
    tui_component_init(c, &GAP_VT);
    c->user = u;
    return c;
}

/* ---------- composer wiring ---------- */

static char *chrome_border(void *ud, const char *s) {
    ui_t *u = ud;
    char *txt = tui_editor_get_text(u->editor);
    const char *p = txt;
    while (*p == ' ' || *p == '\t') p++;
    char *r;
    if (*p == '!') r = fgw(TUI_TC_BASH_MODE, s);
    else if (*p == '$') r = fgw(TUI_TC_PYTHON_MODE, s);
    else {
        const char *title = u->app->session ? u->app->session->title : NULL;
        if (title && *title && settings_bool("statusLine.sessionAccent", true)) r = fg_rgb(tui_session_accent(title, TH()), s);
        else r = fgw((tui_tc_t)(TUI_TC_THINKING_OFF + (int)effective_effort(u->app)), s);
    }
    free(txt);
    return r;
}

static char *chrome_accent(void *ud, const char *s) { return fgw(TUI_TC_ACCENT, s); }

static char *chrome_surface(void *ud, const char *s) {
    /* bgFill(userMessageBg, fgOnBg(userMessageText, userMessageBg, s)) */
    const char *bg = tui_bg_seq(TH(), TUI_TC_USER_MESSAGE_BG);
    char fb[32];
    const char *fgs = tui_rgb_fg(TH(), tui_theme_rgb(TH(), TUI_TC_USER_MESSAGE_TEXT), fb, sizeof fb);
    sb_t b = SB_INIT;
    sb_append(&b, bg);
    sb_append(&b, fgs);
    for (const char *p = s; *p;) {
        if (!strncmp(p, "\x1b[0m", 4)) {
            sb_append(&b, "\x1b[0m");
            sb_append(&b, bg);
            sb_append(&b, fgs);
            p += 4;
        } else if (!strncmp(p, "\x1b[39m", 5)) {
            sb_append(&b, "\x1b[39m");
            sb_append(&b, fgs);
            p += 5;
        } else if (!strncmp(p, "\x1b[49m", 5)) {
            sb_append(&b, "\x1b[49m");
            sb_append(&b, bg);
            p += 5;
        } else {
            sb_putc(&b, *p++);
        }
    }
    sb_append(&b, "\x1b[39m\x1b[49m");
    return sb_detach(&b);
}

void ui_chrome_setup(ui_t *u) {
    tui_editor_t *ed = u->editor;
    tui_editor_set_composer(ed, composer_shape());
    tui_editor_set_chrome_colors(ed, chrome_border, chrome_accent, chrome_surface, u);
    tui_editor_set_top_provider(ed, status_top, u);
    tui_editor_set_placeholder_fn(ed, composer_hint, u);
    char b[32];
    snprintf(ed->text_fg, sizeof ed->text_fg, "%s", tui_rgb_fg(TH(), tui_theme_rgb(TH(), TUI_TC_TEXT), b, sizeof b));
    ed->use_terminal_cursor = settings_bool("showHardwareCursor", true);
    tui_set_show_hardware_cursor(u->tui, ed->use_terminal_cursor);
}

/* repaint while a turn runs so the brand spinner and timer advance */
static void turn_tick(void *ud) {
    ui_t *u = ud;
    tui_request_render(u->tui);
}

void ui_chrome_turn(ui_t *u, bool active) {
    if (active) {
        if (g_sl.started) return;
        g_sl.started = mono_ms();
        if (!g_sl.tick_timer) g_sl.tick_timer = tui_add_timer(u->tui, 33, true, turn_tick, u); /* shimmer cadence */
    } else {
        if (!g_sl.started) return;
        g_sl.active_ms += MAX(0, mono_ms() - g_sl.started);
        g_sl.started = 0;
        if (g_sl.tick_timer) tui_remove_timer(u->tui, g_sl.tick_timer);
        g_sl.tick_timer = 0;
    }
    tui_request_render(u->tui);
}
