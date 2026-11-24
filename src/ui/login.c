/* login.c - provider sign-in UI shared by the setup wizard and /login (ports of omp
 * overlays/oauth-selector.ts and overlays/login-dialog.ts, v18.14.2): a searchable provider list in a
 * "Select provider to login" panel, then a "Login to <provider>" dialog that shows the authorize URL,
 * instructions (device codes: "Enter code: XXXX-XXXX"), progress, and the flow's prompts (API keys, enterprise
 * domains, regions, OTP codes) while a worker thread runs the provider's login from src/config/auth.c. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "config/auth.h"
#include "config/settings.h"
#include "core/fs.h"
#include "tui/fuzzy.h"

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#define OAUTH_MAX_VISIBLE 10

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

/* ======================================================================================= */
/* provider selector                                                                        */
/* ======================================================================================= */

struct ui_oauth_sel {
    json_t *providers; /* borrowed array */
    VEC(size_t) items; /* provider indices after filters */
    VEC(size_t) vis;   /* indices into items */
    size_t sel;
    char *query;
    int max_visible;
    char *status_message;
    bool logout;
};

static const json_t *sel_prov(const ui_oauth_sel_t *s, size_t item) { return json_at(s->providers, s->items.v[item]); }

static char *prov_source(const char *id) {
    json_t *list = auth_list(), *e;
    char *src = NULL;
    json_arr_foreach(list, i, e) if (!src && str_eq(json_get_str(e, "provider"), id)) src =
        xstrdup(str_eq(json_get_str(e, "type"), "oauth") ? "login" : "api key");
    json_free(list);
    if (!src && auth_has_credentials(id)) src = xstrdup("env");
    return src;
}

static void sel_refilter(ui_oauth_sel_t *s) {
    s->vis.n = 0;
    size_t n = s->items.n;
    char **texts = xcalloc(n ? n : 1, sizeof *texts);
    for (size_t i = 0; i < n; i++) {
        const json_t *p = sel_prov(s, i);
        sb_t b = SB_INIT;
        char *nm = auth_login_name(p);
        sb_appendf(&b, "%s %s", nm, json_get_str(p, "id"));
        free(nm);
        char *src = prov_source(json_get_str(p, "id"));
        if (src) sb_appendf(&b, " logged in authenticated %s", src);
        free(src);
        texts[i] = sb_detach(&b);
    }
    size_t *idx = xmalloc((n ? n : 1) * sizeof *idx);
    size_t m = tui_fuzzy_rank(s->query ? s->query : "", (const char *const *)texts, n, idx);
    for (size_t i = 0; i < m; i++) vec_push(&s->vis, idx[i]);
    free(idx);
    for (size_t i = 0; i < n; i++) free(texts[i]);
    free(texts);
    s->sel = 0; /* upstream setQuery(value, false): selection resets */
}

ui_oauth_sel_t *ui_oauth_sel_new(bool logout) {
    ui_oauth_sel_t *s = xcalloc(1, sizeof *s);
    s->logout = logout;
    s->max_visible = OAUTH_MAX_VISIBLE;
    s->providers = auth_oauth_providers();
    json_t *disabled = settings_get("disabledProviders"), *p;
    json_arr_foreach(s->providers, i, p) {
        const char *id = json_get_str(p, "id");
        bool off = !auth_login_listed(p); /* not a login provider (openai, azure), or paused by its auth rules */
        if (logout) {
            off = off || !auth_has_credentials(id);
        } else {
            json_t *d;
            json_arr_foreach(disabled, k, d) if (str_eq(json_sv(d), id) || str_eq(json_sv(d), json_get_str(p, "storeAs"))) off =
                true;
        }
        if (!off) vec_push(&s->items, i);
    }
    sel_refilter(s);
    return s;
}

void ui_oauth_sel_free(ui_oauth_sel_t *s) {
    if (!s) return;
    vec_free(&s->items);
    vec_free(&s->vis);
    free(s->query);
    free(s->status_message);
    free(s);
}

void ui_oauth_sel_refresh(ui_oauth_sel_t *s) {
    size_t keep = s->sel;
    sel_refilter(s);
    if (keep < s->vis.n) s->sel = keep;
}

void ui_oauth_sel_set_max_height(ui_oauth_sel_t *s, int lines) {
    int strict = lines - 1 - 2, relaxed = lines - 1 - 1;
    s->max_visible = MIN(OAUTH_MAX_VISIBLE, MAX(1, MAX(strict, MIN(relaxed, 3))));
}

static bool sel_search_enabled(const ui_oauth_sel_t *s) { return (int)s->items.n > s->max_visible; }

void ui_oauth_sel_render(ui_oauth_sel_t *s, int width, strvec_t *out) {
    int inner = MAX(1, width - 4);
    strvec_t body = {0};
    int total = (int)s->vis.n, maxv = s->max_visible;
    int start = 0;
    if (total > 0 && maxv > 0) start = MAX(0, MIN((int)s->sel - maxv / 2, MAX(0, total - maxv)));
    int end = MIN(total, start + maxv);
    strvec_t rows = {0};
    for (int i = start; i < end; i++) {
        const json_t *p = sel_prov(s, s->vis.v[i]);
        bool selected = (size_t)i == s->sel;
        sb_t l = SB_INIT;
        char *name = auth_login_name(p);
        if (selected) {
            char *cur = xasprintf("%s ", tui_sym("nav.cursor"));
            tui_fg(&l, NULL, TUI_TC_ACCENT, cur);
            free(cur);
            tui_fg(&l, NULL, TUI_TC_ACCENT, name);
        } else {
            sb_appendf(&l, "  %s", name);
        }
        free(name);
        char *src = prov_source(json_get_str(p, "id"));
        if (src) {
            char *st = xasprintf(" %s logged in", tui_sym("status.enabled"));
            tui_fg(&l, NULL, TUI_TC_SUCCESS, st);
            free(st);
            char *s2 = xasprintf(" (%s)", src);
            tui_fg(&l, NULL, TUI_TC_MUTED, s2);
            free(s2);
            free(src);
        }
        strvec_push_own(&rows, sb_detach(&l));
    }
    int h = (int)rows.n;
    bool bar = h > 0 && total > h;
    int cw = MAX(0, inner - (bar ? 1 : 0));
    int ts = 0, te = h;
    if (bar) {
        int size = MAX(1, MIN((h * h) / total, h));
        int maxoff = MAX(0, total - h);
        ts = maxoff == 0 ? 0 : (int)lround((double)MIN(start, maxoff) / maxoff * (h - size));
        te = ts + size;
    }
    for (int r = 0; r < h; r++) {
        char *tr = tui_truncate_to_width(rows.v[r], cw, NULL);
        if (!bar) {
            strvec_push_own(&body, tr);
            continue;
        }
        sb_t b = SB_INIT;
        sb_append(&b, tr);
        tui_sb_spaces(&b, cw - tui_visible_width(tr));
        free(tr);
        bool thumb = r >= ts && r < te;
        tui_fg(&b, NULL, thumb ? TUI_TC_ACCENT : TUI_TC_MUTED, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
        strvec_push_own(&body, sb_detach(&b));
    }
    strvec_free(&rows);
    if (sel_search_enabled(s) || (s->query && *s->query)) {
        char *t = str_trim(s->query ? s->query : "");
        char *line;
        if (!*t) {
            line = S(TUI_TC_MUTED, "Type to search");
        } else {
            char *lab = S(TUI_TC_MUTED, "Search: ");
            line = xasprintf("%s%s\x1b[7m \x1b[27m", lab, s->query);
            free(lab);
        }
        strvec_push_own(&body, tui_truncate_to_width(line, inner, NULL));
        free(line);
        free(t);
    }
    if (total == 0) {
        const char *m = s->items.n ? "No matching providers"
                        : s->logout ? "No stored provider credentials to log out"
                                    : "No OAuth providers available";
        strvec_push_own(&body, S(TUI_TC_MUTED, m));
    }
    if (s->status_message) {
        strvec_push(&body, "");
        strvec_push_own(&body, S(TUI_TC_WARNING, s->status_message));
    }
    strvec_push_own(out, ui_box_top(width, s->logout ? "Select provider to logout" : "Select provider to login"));
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    strvec_push_own(out, ui_box_bottom(width));
    strvec_free(&body);
}

int ui_oauth_sel_key(ui_oauth_sel_t *s, const tui_key_t *k, const char **picked) {
    *picked = NULL;
    if (tui_kb(k, "tui.select.cancel")) return UI_SEL_CANCEL;
    long n = (long)s->vis.n;
    bool moved = true;
    if (tui_kb(k, "tui.select.up") && n) s->sel = (size_t)(((long)s->sel - 1 + n) % n);
    else if (tui_kb(k, "tui.select.down") && n) s->sel = (size_t)(((long)s->sel + 1) % n);
    else if (tui_key_matches(k, "pageup") && n) s->sel = (size_t)MAX(0, (long)s->sel - s->max_visible);
    else if (tui_key_matches(k, "pagedown") && n) s->sel = (size_t)MIN(n - 1, (long)s->sel + s->max_visible);
    else moved = false;
    if (moved) {
        free(s->status_message);
        s->status_message = NULL;
        return UI_SEL_HANDLED;
    }
    if (tui_key_matches(k, "enter") || tui_key_matches(k, "ctrl+j")) {
        if (!n) return UI_SEL_HANDLED;
        *picked = json_get_str(sel_prov(s, s->vis.v[s->sel]), "id");
        return UI_SEL_PICK;
    }
    if (!sel_search_enabled(s)) return UI_SEL_IGNORED;
    const char *before = s->query ? s->query : "";
    const char *txt = tui_key_text(k);
    bool printable = k->kind == TUI_KEY_CHAR && txt && *txt;
    if (!*before && (tui_kb(k, "tui.editor.deleteCharBackward") || (printable && isspace((unsigned char)txt[0]))))
        return UI_SEL_IGNORED;
    char *next = NULL;
    if (printable) next = xasprintf("%s%s", before, txt);
    else if (tui_kb(k, "tui.editor.deleteCharBackward") && *before) {
        size_t i = strlen(before) - 1;
        while (i > 0 && ((unsigned char)before[i] & 0xC0) == 0x80) i--;
        next = xstrndup(before, i);
    } else if (tui_kb(k, "tui.editor.deleteWordBackward")) {
        size_t i = strlen(before);
        while (i > 0 && before[i - 1] == ' ') i--;
        while (i > 0 && before[i - 1] != ' ') i--;
        next = xstrndup(before, i);
    } else if (tui_kb(k, "tui.editor.deleteToLineStart")) {
        next = xstrdup("");
    }
    if (!next) return UI_SEL_IGNORED;
    if (strcmp(next, before)) {
        free(s->query);
        s->query = next;
        free(s->status_message);
        s->status_message = NULL;
        sel_refilter(s);
    } else {
        free(next);
    }
    return UI_SEL_HANDLED;
}

/* ======================================================================================= */
/* login dialog + worker                                                                     */
/* ======================================================================================= */

typedef enum { DI_TEXT, DI_INPUT } ditem_kind_t;

typedef struct {
    ditem_kind_t kind;
    char *text; /* styled text (wrapped at render); "" = Spacer(1) */
} ditem_t;

struct ui_login {
    ui_t *u;
    char *provider, *name;
    pthread_mutex_t mu;
    VEC(ditem_t) items;
    /* active input */
    bool input_open, input_secret, input_is_code, manual_pending;
    long input_idx;         /* item index of the mounted input field, -1 when none */
    sb_t input;
    char *pasted;           /* handed to the worker (code paste) */
    /* worker prompt (OAuthPrompt) waiting for an answer */
    pthread_cond_t cv;
    bool prompt_waiting;
    char *prompt_answer;
    volatile int abort_flag;
    bool closed;            /* dialog dismissed (cancel or finished) */
    int refcount;
    ui_login_done_fn done;
    void *done_ud;
    /* worker result */
    int rc;
    char *err;
};

static void dpush(ui_login_t *l, ditem_kind_t kind, char *text_owned) {
    ditem_t d = {kind, text_owned};
    vec_push(&l->items, d);
}

static void dspacer(ui_login_t *l) { dpush(l, DI_TEXT, xstrdup("")); }

static void login_release(ui_login_t *l) {
    pthread_mutex_lock(&l->mu);
    int rc = --l->refcount;
    pthread_mutex_unlock(&l->mu);
    if (rc > 0) return;
    for (size_t i = 0; i < l->items.n; i++) free(l->items.v[i].text);
    vec_free(&l->items);
    sb_free(&l->input);
    free(l->pasted);
    free(l->prompt_answer);
    pthread_cond_destroy(&l->cv);
    free(l->provider);
    free(l->name);
    free(l->err);
    pthread_mutex_destroy(&l->mu);
    free(l);
}

static void request(ui_login_t *l) {
    if (l->u && l->u->tui) tui_request_render(l->u->tui);
}

#ifdef _WIN32
/* open.ts windowsOpenerCommand: PowerShell's Start-Process, anchored to %SystemRoot%\System32, with the target in a
 * single-quoted literal inside an -EncodedCommand (UTF-16LE, base64) so no cmd/PowerShell parsing sees its `&`s;
 * spawned hidden with stdio ignored */
static void win_open(const char *target) {
    const char *sr = env_get("SystemRoot");
    if (!sr) sr = env_get("SYSTEMROOT");
    char *abs = xasprintf("%s\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", sr ? sr : "C:\\Windows");
    char *ps = fs_is_file(abs) ? abs : xstrdup("powershell.exe");
    if (ps != abs) free(abs);
    sb_t q = SB_INIT;
    for (const char *p = target; *p; p++) {
        if (*p == '\'') sb_putc(&q, '\'');
        sb_putc(&q, *p);
    }
    char *script = xasprintf("$ErrorActionPreference='Stop';Start-Process '%s'", sb_cstr(&q));
    sb_free(&q);
    /* UTF-8 -> UTF-16LE bytes */
    sb_t u16 = SB_INIT;
    for (size_t i = 0; script[i];) {
        uint32_t cp;
        int k = utf8_decode(script + i, strlen(script + i), &cp);
        i += (size_t)(k > 0 ? k : 1);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            uint16_t hi = (uint16_t)(0xD800 + (cp >> 10)), lo = (uint16_t)(0xDC00 + (cp & 0x3FF));
            sb_putc(&u16, (char)(hi & 0xff)), sb_putc(&u16, (char)(hi >> 8));
            sb_putc(&u16, (char)(lo & 0xff)), sb_putc(&u16, (char)(lo >> 8));
        } else {
            sb_putc(&u16, (char)(cp & 0xff)), sb_putc(&u16, (char)(cp >> 8));
        }
    }
    char *enc = base64_encode(u16.s ? u16.s : "", u16.len);
    char *argv[] = {ps, "-NoProfile", "-NonInteractive", "-EncodedCommand", enc, NULL};
    omc_win_spawn_opts_t so = {.fd_in = -1, .fd_out = -1, .fd_err = -1, .hidden = true};
    omc_win_spawn(argv, &so);
    free(enc);
    sb_free(&u16);
    free(script);
    free(ps);
}
#endif

/* best-effort browser launch (omp ctx.openInBrowser / utils/open.ts openPath) */
void ui_open_in_browser(const char *url) {
    if (env_truthy("OMC_NO_BROWSER")) return;
#ifdef _WIN32
    win_open(url);
#else
    const char *opener = which("xdg-open") ? "xdg-open" : which("open") ? "open" : NULL;
    if (!opener) return;
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() != 0) _exit(0); /* double fork: the opener is reparented, never a zombie */
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 1);
            dup2(dn, 2);
        }
        execlp(opener, opener, url, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
#endif
}

/* LoginDialogComponent.showAuth */
static void show_auth(ui_login_t *l, const char *url, const char *instructions, const char *launch) {
    for (size_t i = 0; i < l->items.n; i++) free(l->items.v[i].text);
    l->items.n = 0;
    l->input_idx = -1; /* the cleared container no longer mounts the field */
    dspacer(l);
    char *acc = xasprintf("\x1b]8;;%s\x07%s\x1b]8;;\x07", url, url);
    dpush(l, DI_TEXT, S(TUI_TC_ACCENT, acc));
    free(acc);
    char *k = ui_key_hint(omc_platform_darwin() ? "super" : "ctrl");
    char *hint = xasprintf("\x1b]8;;%s\x07%s+click to open\x1b]8;;\x07", url, k);
    dpush(l, DI_TEXT, S(TUI_TC_DIM, hint));
    free(hint);
    free(k);
    if (launch && strcmp(launch, url)) {
        char *t = xasprintf("Local shortcut (this machine only): %s", launch);
        dpush(l, DI_TEXT, S(TUI_TC_DIM, t));
        free(t);
    }
    if (instructions && *instructions) {
        dspacer(l);
        dpush(l, DI_TEXT, S(TUI_TC_WARNING, instructions));
    }
    if (l->u && l->u->tui) tui_term_copy_osc52(tui_term(l->u->tui), url, strlen(url));
    ui_open_in_browser(url);
}

static void open_input(ui_login_t *l, bool secret, bool code) {
    l->input_open = true;
    l->input_secret = secret;
    l->input_is_code = code;
    sb_clear(&l->input);
    l->input_idx = (long)l->items.n;
    dpush(l, DI_INPUT, xstrdup(""));
}

/* showPrompt: multi-step flows keep prior answers visible ("> value", secrets masked), then the new question */
static void show_prompt(ui_login_t *l, const char *message, const char *placeholder, bool secret) {
    if (l->input_idx >= 0 && (size_t)l->input_idx < l->items.n && l->items.v[l->input_idx].kind == DI_INPUT) {
        char *ans = xasprintf("> %s", l->input_secret ? "********" : sb_cstr(&l->input));
        free(l->items.v[l->input_idx].text);
        l->items.v[l->input_idx].kind = DI_TEXT;
        l->items.v[l->input_idx].text = S(TUI_TC_DIM, ans);
        free(ans);
        l->input_idx = -1;
    }
    dspacer(l);
    dpush(l, DI_TEXT, S(TUI_TC_TEXT, message));
    if (placeholder && *placeholder) {
        char *t = xasprintf("e.g., %s", placeholder);
        dpush(l, DI_TEXT, S(TUI_TC_DIM, t));
        free(t);
    }
    open_input(l, secret, false);
    char *c = ui_editor_key("tui.select.cancel"), *s = ui_editor_key("tui.input.submit");
    char *t = xasprintf("(%s to cancel, %s to submit)", c, s);
    dpush(l, DI_TEXT, S(TUI_TC_DIM, t));
    free(t);
    free(c);
    free(s);
}

/* showManualInput (paste-code providers) */
static void show_manual_input(ui_login_t *l) {
    char *sk = ui_editor_key("tui.input.submit");
    char *p = xasprintf("Paste the authorization code (or full redirect URL), then press %s:", sk);
    dspacer(l);
    dpush(l, DI_TEXT, S(TUI_TC_DIM, p));
    open_input(l, false, true);
    char *c = ui_editor_key("tui.select.cancel");
    char *t = xasprintf("(%s to cancel)", c);
    dpush(l, DI_TEXT, S(TUI_TC_DIM, t));
    free(t);
    free(c);
    free(p);
    free(sk);
}

static const json_t *find_provider(const char *id) {
    json_t *p;
    json_arr_foreach(auth_oauth_providers(), i, p) if (str_eq(json_get_str(p, "id"), id)) return p;
    return NULL;
}

static void on_progress_cb(void *ud, const char *msg) {
    ui_login_t *l = ud;
    pthread_mutex_lock(&l->mu);
    if (!l->closed) {
        dpush(l, DI_TEXT, S(TUI_TC_DIM, msg));
        if (l->manual_pending) { /* onManualCodeInput starts after the first progress report */
            l->manual_pending = false;
            show_manual_input(l);
        }
    }
    pthread_mutex_unlock(&l->mu);
    request(l);
}

static void on_auth_cb(void *ud, const char *url, const char *launch, const char *instructions) {
    ui_login_t *l = ud;
    pthread_mutex_lock(&l->mu);
    if (!l->closed) {
        show_auth(l, url, instructions, launch);
        l->manual_pending = json_get_bool(find_provider(l->provider), "pasteCode", false);
    }
    pthread_mutex_unlock(&l->mu);
    request(l);
}

static char *poll_code_cb(void *ud) {
    ui_login_t *l = ud;
    pthread_mutex_lock(&l->mu);
    char *p = l->pasted;
    l->pasted = NULL;
    pthread_mutex_unlock(&l->mu);
    return p;
}

/* OAuthController.onPrompt: blocks the worker until the dialog submits or closes */
static char *prompt_cb(void *ud, const auth_prompt_t *p) {
    ui_login_t *l = ud;
    pthread_mutex_lock(&l->mu);
    if (l->closed || l->abort_flag) {
        pthread_mutex_unlock(&l->mu);
        return NULL;
    }
    show_prompt(l, p->message, p->placeholder, p->secret);
    free(l->prompt_answer);
    l->prompt_answer = NULL;
    l->prompt_waiting = true;
    pthread_mutex_unlock(&l->mu);
    request(l);
    pthread_mutex_lock(&l->mu);
    while (l->prompt_waiting && !l->closed && !l->abort_flag) pthread_cond_wait(&l->cv, &l->mu);
    char *a = l->prompt_waiting ? NULL : l->prompt_answer;
    l->prompt_answer = NULL;
    l->prompt_waiting = false;
    pthread_mutex_unlock(&l->mu);
    return a;
}

static void wake_worker(ui_login_t *l) {
    pthread_mutex_lock(&l->mu);
    pthread_cond_broadcast(&l->cv);
    pthread_mutex_unlock(&l->mu);
}

static void login_finish_ui(void *ud) {
    ui_login_t *l = ud;
    bool was_closed = l->closed;
    l->closed = true;
    if (!was_closed && l->done) {
        int outcome = l->rc == 0 ? UI_LOGIN_SUCCESS : UI_LOGIN_ERROR;
        l->done(l->done_ud, outcome, l->name, l->err);
    }
    request(l);
    login_release(l);
}

static void *login_worker(void *ud) {
    ui_login_t *l = ud;
    auth_login_hooks_t h = {0};
    h.on_auth = on_auth_cb;
    h.on_progress = on_progress_cb;
    h.poll_code = poll_code_cb;
    h.prompt = prompt_cb;
    h.abort = &l->abort_flag;
    h.ud = l;
    l->rc = auth_login_hooked(l->provider, &h, &l->err);
    /* the new credential may unlock models: refresh before the dialog reports success, as omp does (#5780) */
    if (l->rc == 0) catalog_discover_provider(l->provider);
    tui_post(l->u->tui, login_finish_ui, l);
    return NULL;
}

ui_login_t *ui_login_start(ui_t *u, const char *provider, ui_login_done_fn done, void *ud) {
    const json_t *p = find_provider(provider);
    ui_login_t *l = xcalloc(1, sizeof *l);
    l->u = u;
    l->provider = xstrdup(provider);
    l->name = p ? auth_login_name(p) : xstrdup(provider);
    l->done = done;
    l->done_ud = ud;
    l->refcount = 1; /* owner */
    l->input_idx = -1;
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->cv, NULL);
    if (!json_get_obj(p, "login")) {
        l->rc = -1;
        l->err = xasprintf("%s has no interactive login", l->name);
        l->refcount++;
        tui_post(u->tui, login_finish_ui, l);
        return l;
    }
    l->refcount++; /* worker */
    pthread_t th;
    if (pthread_create(&th, NULL, login_worker, l) != 0) {
        l->rc = -1;
        l->err = xstrdup("cannot start login thread");
        tui_post(u->tui, login_finish_ui, l);
        return l;
    }
    pthread_detach(th);
    return l;
}

void ui_login_free(ui_login_t *l) {
    if (!l) return;
    l->closed = true;
    l->abort_flag = 1;
    wake_worker(l);
    login_release(l);
}

bool ui_login_active(const ui_login_t *l) { return l && !l->closed; }

/* OverlayPanel("Login to <name>") */
void ui_login_render(ui_login_t *l, int width, strvec_t *out) {
    int inner = MAX(1, width - 4);
    strvec_t body = {0};
    pthread_mutex_lock(&l->mu);
    for (size_t i = 0; i < l->items.n; i++) {
        ditem_t *d = &l->items.v[i];
        if (d->kind == DI_INPUT) {
            sb_t in = SB_INIT;
            sb_append(&in, "> ");
            const char *v = sb_cstr(&l->input);
            if (l->input_secret) {
                for (const char *p = v; *p; p++)
                    if (((unsigned char)*p & 0xC0) != 0x80) sb_append(&in, "\xe2\x80\xa2");
            } else {
                sb_append(&in, v);
            }
            if (l->input_open) sb_append(&in, TUI_CURSOR_MARKER); /* focused field: hardware cursor */
            strvec_push_own(&body, tui_truncate_to_width(sb_cstr(&in), inner, NULL));
            sb_free(&in);
        } else if (!*d->text) {
            strvec_push(&body, "");
        } else {
            ui_push_text(&body, d->text, inner);
        }
    }
    pthread_mutex_unlock(&l->mu);
    char *title = xasprintf("Login to %s", l->name);
    strvec_push_own(out, ui_box_top(width, title));
    free(title);
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    strvec_push_own(out, ui_box_bottom(width));
    strvec_free(&body);
}

bool ui_login_key(ui_login_t *l, const tui_key_t *k) {
    if (l->closed) return false;
    if (tui_kb(k, "tui.select.cancel")) {
        /* LoginDialogComponent#cancel: abort, close; no outcome */
        l->abort_flag = 1;
        l->closed = true;
        wake_worker(l);
        if (l->done) l->done(l->done_ud, UI_LOGIN_CANCELLED, l->name, NULL);
        return true;
    }
    if (!l->input_open) return true;
    const char *txt = tui_key_text(k);
    pthread_mutex_lock(&l->mu);
    if (tui_kb(k, "tui.input.submit")) {
        if (l->input_is_code) {
            free(l->pasted);
            l->pasted = str_trim(sb_cstr(&l->input));
        } else if (l->prompt_waiting) {
            /* prompt answered: the field stays mounted until the next question replaces it */
            free(l->prompt_answer);
            l->prompt_answer = xstrdup(sb_cstr(&l->input));
            l->prompt_waiting = false;
            l->input_open = false;
            pthread_cond_broadcast(&l->cv);
        }
        pthread_mutex_unlock(&l->mu);
        return true;
    }
    if (k->kind == TUI_KEY_PASTE && k->text) sb_append(&l->input, k->text);
    else if (tui_kb(k, "tui.editor.deleteCharBackward")) {
        size_t n = l->input.len;
        while (n > 0 && ((unsigned char)l->input.s[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        sb_truncate(&l->input, n);
    } else if (tui_kb(k, "tui.editor.deleteToLineStart")) sb_clear(&l->input);
    else if (k->kind == TUI_KEY_CHAR && txt && *txt) sb_append(&l->input, txt);
    pthread_mutex_unlock(&l->mu);
    return true;
}

/* ======================================================================================= */
/* /login and /logout (selector in the editor slot, then the dialog)                         */
/* ======================================================================================= */

typedef struct {
    tui_component_t base;
    ui_t *u;
    ui_oauth_sel_t *sel;
    ui_login_t *login;
    bool logout;
    char *reopen_hub; /* provider whose Models hub entry reopens after a successful login */
} login_view_t;

static void login_view_render(tui_component_t *c, int width, strvec_t *out) {
    login_view_t *v = (login_view_t *)c;
    if (v->login) ui_login_render(v->login, width, out);
    else ui_oauth_sel_render(v->sel, width, out);
}

typedef struct {
    ui_t *u;
    char *msg;
    const char *level;
    char *reopen; /* reopen the Models hub on this provider */
} login_close_t;

static void login_view_close_cb(void *ud) {
    login_close_t *c = ud;
    ui_close_selector(c->u); /* destroys the view (and frees the login) */
    if (c->msg) add_notice(c->u, c->level, c->msg);
    if (c->reopen) ui_overlay_model_provider(c->u, c->reopen);
    free(c->reopen);
    free(c->msg);
    free(c);
}

static void login_view_done(void *ud, int outcome, const char *name, const char *err) {
    login_view_t *v = ud;
    ui_t *u = v->u;
    char *msg = NULL;
    const char *level = "info";
    if (outcome == UI_LOGIN_SUCCESS) {
        char *path = omc_agent_path("auth.json");
        msg = xasprintf("Logged in to %s. Credentials saved to %s", name, path);
        free(path);
    } else if (outcome == UI_LOGIN_ERROR) {
        msg = xasprintf("Login failed: %s", err ? err : "unknown error");
        level = "error";
    } else {
        msg = xstrdup("Login cancelled"); /* dialog onComplete -> showStatus, replacing "Logging in to ..." */
    }
    /* close on the next loop turn: we may be inside the view's own key handler */
    login_close_t *c = xcalloc(1, sizeof *c);
    c->u = u;
    c->msg = msg;
    c->level = level;
    if (outcome == UI_LOGIN_SUCCESS && v->reopen_hub) c->reopen = xstrdup(v->reopen_hub);
    tui_post(u->tui, login_view_close_cb, c);
}

static bool login_view_key(tui_component_t *c, const tui_key_t *k) {
    login_view_t *v = (login_view_t *)c;
    if (v->login) {
        ui_login_key(v->login, k);
        tui_component_request_render(c);
        return true;
    }
    const char *picked = NULL;
    int r = ui_oauth_sel_key(v->sel, k, &picked);
    if (r == UI_SEL_CANCEL) {
        ui_close_selector(v->u);
        return true;
    }
    if (r == UI_SEL_PICK && picked) {
        if (v->logout) {
            char *id = xstrdup(picked);
            ui_t *u = v->u;
            ui_close_selector(u);
            if (auth_logout(id) == 0) {
                char *m = xasprintf("Logged out of %s", id);
                add_notice(u, "info", m);
                free(m);
            } else {
                add_notice(u, "error", "Logout failed");
            }
            free(id);
            return true;
        }
        char *st = xasprintf("Logging in to %s\xe2\x80\xa6", picked); /* omp showStatus before the dialog */
        add_notice(v->u, "info", st);
        free(st);
        v->login = ui_login_start(v->u, picked, login_view_done, v);
    }
    tui_component_request_render(c);
    return true;
}

static void login_view_destroy(tui_component_t *c) {
    login_view_t *v = (login_view_t *)c;
    if (v->login) {
        v->login->done = NULL;
        ui_login_free(v->login);
    }
    ui_oauth_sel_free(v->sel);
    free(v->reopen_hub);
    free(v);
}

static const tui_component_vt LOGIN_VIEW_VT = {"login", login_view_render, login_view_key, NULL, login_view_destroy};

void ui_overlay_login(ui_t *u, bool logout) {
    login_view_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &LOGIN_VIEW_VT);
    v->u = u;
    v->logout = logout;
    v->sel = ui_oauth_sel_new(logout);
    ui_show_selector(u, &v->base, &v->base);
}

/* omp #loginThenReopenModelHub: that provider's login dialog in the editor slot; a successful login
 * reopens the Models hub on the provider */
void ui_overlay_login_provider(ui_t *u, const char *provider, bool reopen_hub) {
    login_view_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &LOGIN_VIEW_VT);
    v->u = u;
    v->sel = ui_oauth_sel_new(false);
    if (reopen_hub) v->reopen_hub = xstrdup(provider);
    char *st = xasprintf("Logging in to %s\xe2\x80\xa6", provider);
    add_notice(u, "info", st);
    free(st);
    v->login = ui_login_start(u, provider, login_view_done, v);
    ui_show_selector(u, &v->base, &v->base);
}
