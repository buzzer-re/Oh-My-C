/* mcpwizard.c - MCPAddWizard (port of omp packages/tui/src/overlays/mcp-add-wizard.ts, ANSI rendering, v18.14.2):
 * the "Add MCP Server" panel in the composer's place. Steps: name, transport, command / args or URL, the
 * connection test with auth detection, OAuth (discovered endpoints or edited settings, the browser sign-in,
 * the health check), API key + where it goes, scope, review. Esc steps back (cancels on the first step),
 * Ctrl+C cancels, Esc / Ctrl+C during the browser wait stop the OAuth flow.
 *
 * Connection tests and the OAuth flow run on worker threads (mcpcmd.c helpers); their results come back
 * through tui_post and are dropped when the user stepped away meanwhile (`attempt`) or the wizard closed. */
#include "tui/motion.h"
#include "ui/mcpint.h"

#include "core/fs.h"

#include <ctype.h>
#include <pthread.h>

typedef enum {
    ST_NAME, ST_TRANSPORT, ST_COMMAND, ST_ARGS, ST_URL, ST_AUTH_METHOD, ST_OAUTH_ERROR, ST_OAUTH_AUTH_URL,
    ST_OAUTH_TOKEN_URL, ST_OAUTH_CLIENT_ID, ST_OAUTH_CLIENT_SECRET, ST_OAUTH_SCOPES, ST_APIKEY, ST_AUTH_LOCATION,
    ST_ENV_VAR_NAME, ST_HEADER_NAME, ST_SCOPE, ST_CONFIRM
} step_t;

enum { PH_NAME, PH_TRANSPORT, PH_CONNECTION, PH_AUTH, PH_SCOPE, PH_REVIEW, PH_COUNT };

static int step_phase(step_t s) {
    switch (s) {
    case ST_NAME: return PH_NAME;
    case ST_TRANSPORT: return PH_TRANSPORT;
    case ST_COMMAND: case ST_ARGS: case ST_URL: return PH_CONNECTION;
    case ST_SCOPE: return PH_SCOPE;
    case ST_CONFIRM: return PH_REVIEW;
    default: return PH_AUTH;
    }
}

typedef enum { K_INPUT, K_CHOICE, K_ASYNC, K_CONFIRM } kind_t;

typedef struct {
    char *label, *desc;
} choice_t;

typedef struct wiz {
    tui_component_t base;
    ui_t *u;
    int refs;
    bool closed;
    step_t step;
    kind_t kind;
    int phase; /* the heading's phase (async steps name theirs) */
    int sel;
    int attempt;
    char *validation;
    /* state */
    char *name, *command, *args, *url;
    const char *transport; /* NULL until chosen */
    const char *auth_method; /* none / oauth / manual */
    char *o_auth, *o_token, *o_reg, *o_client, *o_secret, *o_scopes, *o_resource;
    bool o_done;
    char *apikey;
    const char *auth_loc; /* env / header / NULL */
    char *env_name, *header_name;
    const char *scope;
    char *www; /* challenge from the connection test */
    /* oauth failure */
    bool fail_cancelled;
    char *fail_msg, *fail_tip;
    volatile int oauth_abort;
    bool oauth_running;
    /* mounted step */
    char *title;
    tui_tc_t tone;
    char *lead;          /* input label / choice intro text (styled) */
    strvec_t intro;      /* custom intro rows (styled, wrapped at render) */
    char *detail;        /* "e.g. …" */
    char *summary;       /* "✗ error" */
    char *hint;          /* input hint / choice footer (styled) */
    bool has_input, secret, optional;
    ui_input_t in;
    choice_t ch[3];
    int nch;
    strvec_t body;       /* async / confirm content rows (styled; "" = blank) */
    char *footer;        /* async / confirm footer (styled) */
    int health_row;      /* body row of the animated health line, -1 none */
    int spin_frame, spin_timer, step_timer;
} wiz_t;

static wiz_t *g_wiz;

bool ui_mcp_wizard_active(void) { return g_wiz != NULL; }

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static char *interrupt_key(void) {
    const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), "app.interrupt");
    return ui_key_hint(keys && keys->n ? keys->v[0] : "escape");
}

/* sanitize: tabs replaced, truncated to 120 columns */
static char *sanitize(const char *t) {
    char *r = tui_replace_tabs(t ? t : "");
    char *o = tui_truncate_to_width(r, 120, NULL);
    free(r);
    return o;
}

static void wiz_unref(wiz_t *w) {
    if (__atomic_sub_fetch(&w->refs, 1, __ATOMIC_SEQ_CST)) return;
    free(w->validation);
    free(w->name), free(w->command), free(w->args), free(w->url);
    free(w->o_auth), free(w->o_token), free(w->o_reg), free(w->o_client), free(w->o_secret), free(w->o_scopes), free(w->o_resource);
    free(w->apikey), free(w->env_name), free(w->header_name), free(w->www);
    free(w->fail_msg), free(w->fail_tip);
    free(w->title), free(w->lead), free(w->detail), free(w->summary), free(w->hint), free(w->footer);
    strvec_free(&w->intro);
    strvec_free(&w->body);
    for (int i = 0; i < 3; i++) free(w->ch[i].label), free(w->ch[i].desc);
    ui_input_free(&w->in);
    free(w);
}

static void set_str(char **dst, const char *v) {
    free(*dst);
    *dst = v ? xstrdup(v) : NULL;
}

static void request(wiz_t *w) {
    if (w->u && w->u->tui) tui_request_render(w->u->tui);
}

/* ---------- mounting a step ---------- */

static void clear_step(wiz_t *w) {
    free(w->title), w->title = NULL;
    free(w->lead), w->lead = NULL;
    free(w->detail), w->detail = NULL;
    free(w->summary), w->summary = NULL;
    free(w->hint), w->hint = NULL;
    free(w->footer), w->footer = NULL;
    strvec_free(&w->intro);
    strvec_free(&w->body);
    for (int i = 0; i < 3; i++) free(w->ch[i].label), free(w->ch[i].desc), w->ch[i].label = w->ch[i].desc = NULL;
    w->nch = 0;
    w->has_input = false;
    w->secret = false;
    w->health_row = -1;
    if (w->spin_timer && w->u->tui) tui_remove_timer(w->u->tui, w->spin_timer);
    w->spin_timer = 0;
}

/* #inputStep */
static void input_step(wiz_t *w, const char *title, const char *lead, const char *initial, const char *placeholder,
                       const char *note, const char *escape, bool optional, bool secret, const char *error) {
    clear_step(w);
    w->kind = K_INPUT;
    w->title = xstrdup(title);
    w->tone = TUI_TC_ACCENT;
    w->lead = xstrdup(lead);
    w->has_input = true;
    w->secret = secret;
    w->optional = optional;
    ui_input_set(&w->in, initial ? initial : "");
    if (placeholder) {
        char *t = xasprintf("e.g. %s", placeholder);
        w->detail = S(TUI_TC_MUTED, t);
        free(t);
    }
    if (error) {
        char *s = sanitize(error), *t = xasprintf("✗ %s", s);
        w->summary = S(TUI_TC_ERROR, t);
        free(s), free(t);
    }
    char *submit = ui_editor_key("tui.input.submit"), *cancel = ui_editor_key("tui.select.cancel");
    char *kh = escape ? xasprintf("[%s to continue, %s to %s]", submit, cancel, escape) : xasprintf("[Press %s to skip or continue]", submit);
    char *h = note ? xasprintf("[%s]\n%s", note, kh) : xstrdup(kh);
    w->hint = S(TUI_TC_MUTED, h);
    free(submit), free(cancel), free(kh), free(h);
}

static void choice_footer(wiz_t *w, const char *escape) {
    char *up = ui_editor_key("tui.select.up"), *down = ui_editor_key("tui.select.down"), *en = ui_key_hint("enter"), *ik = interrupt_key();
    char *t = xasprintf("[%s/%s to navigate, %s to select, %s to %s]", up, down, en, ik, escape);
    w->hint = S(TUI_TC_MUTED, t);
    free(up), free(down), free(en), free(ik), free(t);
}

static void choice_step(wiz_t *w, const char *title, tui_tc_t tone, const char *lead, const char *const *labels,
                        const char *const *descs, int n) {
    clear_step(w);
    w->kind = K_CHOICE;
    w->title = xstrdup(title);
    w->tone = tone;
    w->lead = lead ? xstrdup(lead) : NULL;
    for (int i = 0; i < n && i < 3; i++) {
        w->ch[i].label = xstrdup(labels[i]);
        w->ch[i].desc = descs && descs[i] ? xstrdup(descs[i]) : NULL;
    }
    w->nch = n;
    choice_footer(w, "go back");
}

static void async_step(wiz_t *w, int phase, const char *title, tui_tc_t tone) {
    clear_step(w);
    w->kind = K_ASYNC;
    w->phase = phase;
    w->title = xstrdup(title);
    w->tone = tone;
}

static void body_push(wiz_t *w, tui_tc_t slot, const char *t) {
    strvec_push_own(&w->body, (int)slot < 0 ? xstrdup(t) : S(slot, t));
}

/* ---------- steps ---------- */

static const char *TR_LABEL[3] = {"Local command", "Remote server", "Server-sent events"};
static const char *TR_DESC[3] = {"omp starts it on this machine", "Streamable HTTP endpoint", "Older HTTP servers"};
static const char *TR_TAG[3] = {"stdio", "http", "sse"};

static int transport_index(const char *t) { return str_eq(t, "http") ? 1 : str_eq(t, "sse") ? 2 : 0; }

/* where `scope`'s config file is: relative inside the project, else ~-shortened */
static char *config_label(wiz_t *w, const char *scope) {
    const char *cwd = w->u->app->cwd;
    char *file = mcp_config_path(scope, cwd);
    size_t cl = strlen(cwd);
    char *r;
    if (!strncmp(file, cwd, cl) && file[cl] == '/') r = xstrdup(file + cl + 1);
    else {
        const char *home = home_dir();
        size_t hl = home ? strlen(home) : 0;
        r = hl > 1 && !strncmp(file, home, hl) && (file[hl] == '/' || !file[hl]) ? xasprintf("~%s", file + hl) : xstrdup(file);
    }
    free(file);
    return r;
}

static void render_step(wiz_t *w) {
    char *t = NULL;
    switch (w->step) {
    case ST_NAME:
        input_step(w, "Name the server", "A unique name: it labels the server in /mcp and prefixes its tools.", w->name, "github",
                   "Letters, numbers, dash, underscore, dot and colon", "cancel", false, false, w->validation);
        break;
    case ST_TRANSPORT: {
        t = xasprintf("Pick how %s talks to omp.", w->name ? w->name : "");
        const char *labels[3];
        char *own[3];
        for (int i = 0; i < 3; i++) labels[i] = own[i] = xasprintf("%s (%s)", TR_LABEL[i], TR_TAG[i]);
        choice_step(w, "How does omp reach it?", TUI_TC_ACCENT, t, labels, TR_DESC, 3);
        for (int i = 0; i < 3; i++) free(own[i]);
        break;
    }
    case ST_COMMAND:
        input_step(w, "Command", "The program omp starts for this server.", w->command, "npx", NULL, "go back", false, false, NULL);
        break;
    case ST_ARGS:
        t = xasprintf("Passed to %s, separated by spaces. omp tries the connection next.", w->command ? w->command : "");
        input_step(w, "Arguments", t, w->args, "-y @modelcontextprotocol/server-github", NULL, NULL, true, false, NULL);
        break;
    case ST_URL:
        t = xasprintf("Where %s listens. omp tries the connection next.", w->name ? w->name : "");
        input_step(w, "Server URL", t, w->url, "https://mcp.example.com/mcp", "Starts with http:// or https://", "go back", false, false,
                   w->validation);
        break;
    case ST_AUTH_METHOD: {
        t = xasprintf("%s needs credentials before omp can use it.", w->name ? w->name : "");
        const char *l[2] = {"Sign in with OAuth", "API key or token"};
        const char *d[2] = {"Opens your browser", "Paste it, or run a command that prints it"};
        choice_step(w, "How does it authenticate?", TUI_TC_ACCENT, t, l, d, 2);
        break;
    }
    case ST_OAUTH_ERROR: {
        const char *l[2] = {"Try again", "Edit OAuth settings"};
        const char *d[2] = {"Opens the browser again", "Endpoints, client and scopes"};
        choice_step(w, w->fail_cancelled ? "Sign-in cancelled" : "Sign-in failed", w->fail_cancelled ? TUI_TC_MUTED : TUI_TC_ERROR, NULL, l, d, 2);
        if (w->fail_msg) {
            strvec_push(&w->intro, w->fail_msg);
            strvec_push(&w->intro, "");
            if (w->fail_tip) {
                char *tp = xasprintf("Tip: %s", w->fail_tip);
                strvec_push_own(&w->intro, S(TUI_TC_MUTED, tp));
                free(tp);
                strvec_push(&w->intro, "");
            }
        }
        strvec_push(&w->intro, "Choose next action:");
        break;
    }
    case ST_OAUTH_AUTH_URL:
        input_step(w, "Authorization URL", "The provider's OAuth authorize endpoint.", w->o_auth, "https://auth.example.com/oauth/authorize",
                   NULL, "go back", false, false, NULL);
        break;
    case ST_OAUTH_TOKEN_URL:
        input_step(w, "Token URL", "The endpoint that exchanges the authorization code for a token.", w->o_token,
                   "https://auth.example.com/oauth/token", NULL, "go back", false, false, NULL);
        break;
    case ST_OAUTH_CLIENT_ID:
        input_step(w, "Client ID", "The ID of the OAuth app registered with the provider.", w->o_client, NULL, NULL, "go back", false, false,
                   NULL);
        break;
    case ST_OAUTH_CLIENT_SECRET:
        input_step(w, "Client secret", "Leave it empty for PKCE-only flows.", w->o_secret, NULL, NULL, "go back", true, true, NULL);
        break;
    case ST_OAUTH_SCOPES:
        input_step(w, "Scopes", "What omp may access, separated by spaces. Sign-in starts next.", w->o_scopes, "read write", NULL, "go back",
                   true, false, NULL);
        break;
    case ST_APIKEY:
        input_step(w, "API key", "Paste the key or token. Start with ! to run a command that prints it, e.g. a password manager.",
                   w->apikey, "!op read op://dev/github/token", NULL, "go back", false, false, NULL);
        break;
    case ST_AUTH_LOCATION: {
        const char *l[2] = {"Environment variable", "HTTP header"};
        const char *d[2] = {"Set when the server starts", "Sent with every request"};
        choice_step(w, "Where does the key go?", TUI_TC_ACCENT, "How the server expects to receive it.", l, d, 2);
        break;
    }
    case ST_ENV_VAR_NAME:
        input_step(w, "Environment variable", "The variable the server reads its key from.", w->env_name, "API_KEY", NULL, "go back", false,
                   false, NULL);
        break;
    case ST_HEADER_NAME:
        input_step(w, "Header name", "The HTTP header that carries the key.", w->header_name, "Authorization", NULL, "go back", false, false,
                   NULL);
        break;
    case ST_SCOPE: {
        char *up = config_label(w, "user"), *pp = config_label(w, "project");
        const char *l[2] = {"Every project", "This project"};
        const char *d[2] = {up, pp};
        choice_step(w, "Where should it live?", TUI_TC_ACCENT, "Saved for you everywhere, or only in this project's config.", l, d, 2);
        free(up), free(pp);
        break;
    }
    case ST_CONFIRM: {
        clear_step(w);
        w->kind = K_CONFIRM;
        w->title = xstrdup("Review");
        w->tone = TUI_TC_ACCENT;
        const char *tr = w->transport ? w->transport : "stdio";
        const char *scope = w->scope ? w->scope : "user";
        char *an = S(TUI_TC_ACCENT, w->name ? w->name : "");
        strvec_push_own(&w->body, xasprintf("Name: %s", an));
        free(an);
        strvec_push_own(&w->body, xasprintf("Transport: %s", tr));
        if (str_eq(tr, "stdio")) {
            strvec_push_own(&w->body, xasprintf("Command: %s", w->command ? w->command : ""));
            if (w->args && *w->args) strvec_push_own(&w->body, xasprintf("Arguments: %s", w->args));
        } else {
            char *s = sanitize(w->url);
            strvec_push_own(&w->body, xasprintf("URL: %s", s));
            free(s);
        }
        if (str_eq(w->auth_method, "oauth")) strvec_push(&w->body, "Auth: OAuth (authenticated)");
        else if (str_eq(w->auth_method, "manual"))
            strvec_push_own(&w->body, xasprintf("Auth: API key via %s (%s)", str_eq(w->auth_loc, "env") ? "env" : "header",
                                                str_eq(w->auth_loc, "env") ? w->env_name : w->header_name));
        else strvec_push(&w->body, "Auth: None");
        char *pl = config_label(w, scope);
        strvec_push_own(&w->body, xasprintf("Saved in: %s (%s)", str_eq(scope, "user") ? "Every project" : "This project", pl));
        free(pl);
        strvec_push(&w->body, "");
        strvec_push(&w->body, "Save this configuration?");
        char *en = ui_key_hint("enter"), *ik = interrupt_key(), *f = xasprintf("[%s to save, %s to go back]", en, ik);
        w->footer = S(TUI_TC_MUTED, f);
        free(en), free(ik), free(f);
        break;
    }
    }
    free(t);
}

/* ---------- rendering ---------- */

static void push_wrapped_rows(strvec_t *out, const char *styled, int width) {
    if (!styled) return;
    char *clean = tui_replace_tabs(styled);
    tui_wrap_text_with_ansi(clean, MAX(1, width), out);
    free(clean);
}

static void slot_gap(strvec_t *out) {
    if (out->n) strvec_push(out, "");
}

static void wiz_content(wiz_t *w, int width, strvec_t *out) {
    /* heading */
    char *ph = xasprintf("  %d/%d", (w->kind == K_ASYNC ? w->phase : step_phase(w->step)) + 1, PH_COUNT);
    char *t1 = S(w->tone, w->title ? w->title : ""), *t2 = S(TUI_TC_DIM, ph), *head = xasprintf("%s%s", t1, t2);
    push_wrapped_rows(out, head, width);
    free(ph), free(t1), free(t2), free(head);
    if (w->kind == K_INPUT) {
        /* TextFormField: label, details, spacer, control, spacer, error, summary, hint */
        strvec_t f = {0};
        push_wrapped_rows(&f, w->lead, width);
        if (w->detail) push_wrapped_rows(&f, w->detail, width);
        strvec_push(&f, "");
        ui_input_t shown = {0};
        if (w->secret) {
            size_t cps = 0, curcp = 0;
            const char *s = ui_input_value(&w->in);
            for (size_t i = 0; s[i]; i++)
                if (((unsigned char)s[i] & 0xC0) != 0x80) {
                    if (i < w->in.cur) curcp++;
                    cps++;
                }
            sb_t m = SB_INIT;
            for (size_t i = 0; i < cps; i++) sb_putc(&m, '*');
            ui_input_set(&shown, sb_cstr(&m));
            shown.cur = curcp;
            sb_free(&m);
        }
        strvec_push_own(&f, ui_input_render(w->secret ? &shown : &w->in, "> ", width, true));
        ui_input_free(&shown);
        strvec_push(&f, "");
        if (w->summary) {
            push_wrapped_rows(&f, w->summary, width);
            strvec_push(&f, "");
        }
        push_wrapped_rows(&f, w->hint, width);
        slot_gap(out);
        for (size_t i = 0; i < f.n; i++) strvec_push(out, f.v[i]);
        strvec_free(&f);
        return;
    }
    if (w->kind == K_CHOICE) {
        if (w->intro.n) {
            slot_gap(out);
            for (size_t i = 0; i < w->intro.n; i++) {
                if (!*w->intro.v[i]) strvec_push(out, "");
                else push_wrapped_rows(out, w->intro.v[i], width);
            }
        } else if (w->lead) {
            slot_gap(out);
            push_wrapped_rows(out, w->lead, width);
        }
        slot_gap(out);
        for (int i = 0; i < w->nch; i++) {
            bool sel = i == w->sel;
            char *cur = xasprintf("%s ", tui_sym("nav.cursor"));
            char *pre = sel ? S(TUI_TC_ACCENT, cur) : NULL;
            free(cur);
            char *lab = sel ? S(TUI_TC_ACCENT, w->ch[i].label) : xstrdup(w->ch[i].label);
            char *row = sel ? xasprintf("%s%s", pre, lab) : xasprintf("  %s", lab);
            push_wrapped_rows(out, row, width);
            if (w->ch[i].desc && !sel) {
                char *d = S(TUI_TC_DIM, w->ch[i].desc), *dr = xasprintf("    %s", d);
                push_wrapped_rows(out, dr, width);
                free(d), free(dr);
            }
            free(pre), free(lab), free(row);
        }
        slot_gap(out);
        push_wrapped_rows(out, w->hint, width);
        return;
    }
    /* async / confirm: content then footer */
    slot_gap(out);
    for (size_t i = 0; i < w->body.n; i++) {
        if ((int)i == w->health_row) {
            const tui_theme_t *th = TH();
            const char *f = th->nspinner ? th->spinner[w->spin_frame % th->nspinner] : "|";
            char *r = xasprintf("%s Checking server connection...", f), *m = S(TUI_TC_MUTED, r);
            push_wrapped_rows(out, m, width);
            free(r), free(m);
            continue;
        }
        if (!*w->body.v[i]) strvec_push(out, "");
        else push_wrapped_rows(out, w->body.v[i], width);
    }
    if (w->footer) {
        slot_gap(out);
        push_wrapped_rows(out, w->footer, width);
    }
}

static void wiz_render(tui_component_t *c, int width, strvec_t *out) {
    wiz_t *w = c->user;
    strvec_push_own(out, ui_box_top(width, "Add MCP Server"));
    strvec_t body = {0};
    strvec_push(&body, "");
    wiz_content(w, MAX(1, width - 4), &body);
    strvec_push(&body, "");
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    strvec_free(&body);
    strvec_push_own(out, ui_box_bottom(width));
}

/* ---------- flow ---------- */

static void close_wizard(wiz_t *w, bool cancelled) {
    if (w->closed) return;
    ui_t *u = w->u;
    w->oauth_abort = 1;
    ui_close_selector(u); /* destroys the component (sets closed) */
    if (cancelled) mcpui_wizard_cancelled(u);
}

static void go_scope(wiz_t *w) {
    w->step = ST_SCOPE;
    w->sel = 0;
    render_step(w);
    request(w);
}

typedef struct {
    wiz_t *w;
    int attempt;
    bool check_attempt;
    bool none;
} later_t;

static void later_cb(void *p) {
    later_t *l = p;
    wiz_t *w = l->w;
    if (!w->closed && (!l->check_attempt || l->attempt == w->attempt)) {
        w->step_timer = 0;
        if (l->none) w->auth_method = "none";
        go_scope(w);
    }
    wiz_unref(w);
    free(l);
}

static void continue_to_scope(wiz_t *w, int delay, bool check_attempt, bool none) {
    later_t *l = xcalloc(1, sizeof *l);
    l->w = w;
    l->attempt = w->attempt;
    l->check_attempt = check_attempt;
    l->none = none;
    __atomic_add_fetch(&w->refs, 1, __ATOMIC_SEQ_CST);
    w->step_timer = tui_add_timer(w->u->tui, delay, false, later_cb, l);
}

/* the server config from the wizard state; with auth: the API key in env / headers */
static json_t *build_config(wiz_t *w, bool include_auth, bool with_timeout) {
    json_t *c = json_obj();
    const char *tr = w->transport ? w->transport : "stdio";
    bool manual = include_auth && str_eq(w->auth_method, "manual") && w->apikey && *w->apikey;
    if (str_eq(tr, "stdio")) {
        json_set_str(c, "type", "stdio");
        json_set_str(c, "command", w->command ? w->command : "");
        if (with_timeout) json_set_int(c, "timeout", 5000);
        if (w->args && *w->args) {
            strvec_t parts = {0};
            char *dup = xstrdup(w->args), *save = NULL;
            for (char *tk = strtok_r(dup, " \t\n\r", &save); tk; tk = strtok_r(NULL, " \t\n\r", &save)) strvec_push(&parts, tk);
            free(dup);
            json_t *a = json_arr();
            for (size_t i = 0; i < parts.n; i++) json_push_str(a, parts.v[i]);
            json_set(c, "args", a);
            strvec_free(&parts);
        }
        if (manual) {
            json_t *e = json_obj();
            json_set_str(e, w->env_name && *w->env_name ? w->env_name : "API_KEY", w->apikey);
            json_set(c, "env", e);
        }
    } else {
        json_set_str(c, "type", tr);
        json_set_str(c, "url", w->url ? w->url : "");
        if (with_timeout) json_set_int(c, "timeout", 5000);
        if (manual) {
            json_t *h = json_obj();
            json_set_str(h, w->header_name && *w->header_name ? w->header_name : "Authorization", w->apikey);
            json_set(c, "headers", h);
        }
    }
    return c;
}

static void launch_oauth(wiz_t *w);

/* --- connection test --- */

typedef struct {
    wiz_t *w;
    int attempt;
    json_t *cfg;
    int rc;
    char *err, *www;
    bool auth;
    json_t *endpoints;
} test_job_t;

static void test_done_ui(void *p) {
    test_job_t *j = p;
    wiz_t *w = j->w;
    if (w->closed || j->attempt != w->attempt) goto out;
    if (j->rc == 0) {
        async_step(w, PH_CONNECTION, "Connected", TUI_TC_SUCCESS);
        body_push(w, TUI_TC_SUCCESS, "✓ No authentication required");
        request(w);
        continue_to_scope(w, 1000, true, true);
    } else if (j->auth) {
        set_str(&w->www, j->www);
        if (j->endpoints) {
            json_t *e = j->endpoints;
            set_str(&w->o_auth, json_get_str(e, "authorizationUrl"));
            set_str(&w->o_token, json_get_str(e, "tokenUrl"));
            set_str(&w->o_reg, json_get_str(e, "registrationUrl"));
            set_str(&w->o_client, json_get_str(e, "clientId") ? json_get_str(e, "clientId") : "");
            set_str(&w->o_scopes, json_get_str(e, "scopes") ? json_get_str(e, "scopes") : "");
            set_str(&w->o_resource, json_get_str(e, "resource") ? json_get_str(e, "resource") : w->url);
            w->auth_method = "oauth";
            launch_oauth(w);
        } else {
            w->step = ST_APIKEY;
            render_step(w);
            request(w);
        }
    } else {
        async_step(w, PH_CONNECTION, "Couldn't connect", TUI_TC_WARNING);
        char *s = sanitize(j->err);
        body_push(w, (tui_tc_t)-1, s);
        free(s);
        strvec_push(&w->body, "");
        body_push(w, TUI_TC_MUTED, "Adding server anyway...");
        request(w);
        continue_to_scope(w, 2000, true, true);
    }
out:
    json_free(j->cfg);
    json_free(j->endpoints);
    free(j->err);
    free(j->www);
    wiz_unref(w);
    free(j);
}

static void *test_worker(void *p) {
    test_job_t *j = p;
    j->rc = mcp_probe("mcp_probe", j->cfg, 5000, NULL, NULL, NULL, &j->err, &j->auth, &j->www);
    if (j->rc != 0 && j->auth && mcp_config_url(j->cfg)) j->endpoints = mcp_oauth_discover(mcp_config_url(j->cfg), j->www);
    tui_post(j->w->u->tui, test_done_ui, j);
    return NULL;
}

/* #testConnectionAndDetectAuth */
static void test_connection(wiz_t *w) {
    test_job_t *j = xcalloc(1, sizeof *j);
    j->w = w;
    j->attempt = ++w->attempt;
    j->cfg = build_config(w, false, true);
    __atomic_add_fetch(&w->refs, 1, __ATOMIC_SEQ_CST);
    const char *target;
    sb_t tb = SB_INIT;
    if (str_eq(json_get_str(j->cfg, "type"), "stdio")) {
        sb_append(&tb, w->command ? w->command : "");
        json_t *a;
        json_arr_foreach(json_get_arr(j->cfg, "args"), i, a) sb_appendf(&tb, " %s", json_sv(a));
        target = sb_cstr(&tb);
    } else target = w->url ? w->url : "";
    async_step(w, PH_CONNECTION, "Trying the connection", TUI_TC_ACCENT);
    char *s = sanitize(target), *t = xasprintf("Connecting to %s…", s);
    body_push(w, TUI_TC_MUTED, t);
    free(s), free(t);
    sb_free(&tb);
    request(w);
    mcpui_spawn(test_worker, j);
}

/* --- OAuth --- */

typedef struct {
    wiz_t *w;
    json_t *cfg, *health_cfg;
    char *url, *www;
    int rc;
    bool cancelled;
    char *err;
    int health_rc;
    char *health_err;
} oauth_job_t;

static void health_spin(void *ud) {
    wiz_t *w = ud;
    w->spin_frame = tui_spinner_tick();
    request(w);
}

static void oauth_signed_in_ui(void *p) {
    oauth_job_t *j = p;
    wiz_t *w = j->w;
    if (!w->closed) {
        w->o_done = true;
        async_step(w, PH_AUTH, "Checking the server", TUI_TC_SUCCESS);
        body_push(w, TUI_TC_MUTED, "Running connection health check...");
        w->health_row = (int)w->body.n;
        strvec_push(&w->body, "");
        w->spin_timer = tui_add_clock_timer(w->u->tui, NULL, health_spin, w);
        request(w);
    }
    wiz_unref(w);
}

static void oauth_done_ui(void *p) {
    oauth_job_t *j = p;
    wiz_t *w = j->w;
    w->oauth_running = false;
    if (!w->closed) {
        if (j->rc == 0) {
            if (w->spin_timer) tui_remove_timer(w->u->tui, w->spin_timer);
            w->spin_timer = 0;
            int row = w->health_row;
            w->health_row = -1;
            if (row >= 0 && (size_t)row < w->body.n) {
                free(w->body.v[row]);
                w->body.v[row] = j->health_rc == 0 ? S(TUI_TC_SUCCESS, "✓ Health check passed")
                                                   : S(TUI_TC_WARNING, "⚠ Health check failed (will still save config)");
            }
            if (j->health_rc != 0) {
                strvec_push(&w->body, "");
                char *s = sanitize(j->health_err);
                body_push(w, TUI_TC_MUTED, s);
                free(s);
            }
            request(w);
            continue_to_scope(w, j->health_rc == 0 ? 1000 : 2000, false, false);
        } else {
            const char *m = j->err ? j->err : "";
            free(w->fail_tip);
            w->fail_tip = NULL;
            if (j->cancelled) w->fail_tip = xstrdup("Try again reopens the browser.");
            else if (strstr(m, "timeout") || strstr(m, "timed out")) w->fail_tip = xstrdup("Complete authorization faster next time.");
            else if (strstr(m, "Invalid OAuth URLs")) w->fail_tip = xstrdup("Check that the OAuth URLs are correct.");
            else if (strstr(m, "ECONNREFUSED")) w->fail_tip = xstrdup("Verify the OAuth server is accessible.");
            w->fail_cancelled = j->cancelled;
            free(w->fail_msg);
            w->fail_msg = sanitize(m);
            w->sel = 0;
            w->step = ST_OAUTH_ERROR;
            render_step(w);
            request(w);
        }
    }
    json_free(j->cfg);
    json_free(j->health_cfg);
    free(j->url), free(j->www), free(j->err), free(j->health_err);
    wiz_unref(w);
    free(j);
}

static void *oauth_worker(void *p) {
    oauth_job_t *j = p;
    wiz_t *w = j->w;
    j->rc = mcpui_oauth_flow(w->u, j->url, j->cfg, j->www, &w->oauth_abort, false, &j->err, &j->cancelled);
    if (j->rc == 0) {
        __atomic_add_fetch(&w->refs, 1, __ATOMIC_SEQ_CST);
        tui_post(w->u->tui, oauth_signed_in_ui, j);
        /* health check with the new credentials, 10 s */
        j->health_rc = mcp_probe("mcp_probe", j->health_cfg, 10000, NULL, NULL, NULL, &j->health_err, NULL, NULL);
        if (j->health_rc != 0 && j->health_err && strstr(j->health_err, "timed out")) {
            free(j->health_err);
            j->health_err = xstrdup("Health check timed out after 10 seconds");
        }
    }
    tui_post(w->u->tui, oauth_done_ui, j);
    return NULL;
}

/* #launchOAuthFlow */
static void launch_oauth(wiz_t *w) {
    if (!w->o_auth || !*w->o_auth || !w->o_token || !*w->o_token) {
        async_step(w, PH_AUTH, "OAuth settings incomplete", TUI_TC_ERROR);
        strvec_push(&w->body, "Authorization and Token URLs are required.");
        char *ik = interrupt_key(), *f = xasprintf("[Press %s to go back]", ik);
        w->footer = S(TUI_TC_MUTED, f);
        free(ik), free(f);
        request(w);
        return;
    }
    async_step(w, PH_AUTH, "Sign in to continue", TUI_TC_ACCENT);
    strvec_push(&w->body, "Launching OAuth flow...");
    body_push(w, TUI_TC_MUTED, "Browser will open automatically.");
    strvec_push(&w->body, "");
    body_push(w, TUI_TC_WARNING, "If browser doesn't open, copy the URL from chat.");
    char *ik = interrupt_key(), *f = xasprintf("(Press %s to cancel)", ik);
    w->footer = S(TUI_TC_MUTED, f);
    free(ik), free(f);
    request(w);
    oauth_job_t *j = xcalloc(1, sizeof *j);
    j->w = w;
    j->url = xstrdup(w->url ? w->url : "");
    j->www = w->www ? xstrdup(w->www) : NULL;
    j->cfg = build_config(w, false, false);
    json_t *oc = json_obj();
    json_set_str(oc, "authorizationUrl", w->o_auth);
    json_set_str(oc, "tokenUrl", w->o_token);
    if (w->o_client && *w->o_client) json_set_str(oc, "clientId", w->o_client);
    if (w->o_secret && *w->o_secret) json_set_str(oc, "clientSecret", w->o_secret);
    if (w->o_scopes && *w->o_scopes) {
        json_t *sc = json_arr();
        char *dup = xstrdup(w->o_scopes), *save = NULL;
        for (char *tk = strtok_r(dup, " \t", &save); tk; tk = strtok_r(NULL, " \t", &save)) json_push_str(sc, tk);
        free(dup);
        json_set(oc, "scopes", sc);
    }
    json_set(j->cfg, "oauth", oc);
    j->health_cfg = build_config(w, true, false);
    w->oauth_abort = 0;
    w->oauth_running = true;
    __atomic_add_fetch(&w->refs, 1, __ATOMIC_SEQ_CST);
    mcpui_spawn(oauth_worker, j);
}

/* --- navigation --- */

static void go_back(wiz_t *w) {
    w->attempt++;
    free(w->validation);
    w->validation = NULL;
    switch (w->step) {
    case ST_TRANSPORT: w->step = ST_NAME; break;
    case ST_COMMAND:
    case ST_URL:
        w->step = ST_TRANSPORT;
        w->sel = transport_index(w->transport);
        break;
    case ST_ARGS: w->step = ST_COMMAND; break;
    case ST_AUTH_METHOD:
    case ST_OAUTH_AUTH_URL:
    case ST_APIKEY: w->step = str_eq(w->transport, "stdio") ? ST_ARGS : ST_URL; break;
    case ST_AUTH_LOCATION: w->step = ST_APIKEY; break;
    case ST_ENV_VAR_NAME:
    case ST_HEADER_NAME:
        if (str_eq(w->transport, "stdio")) w->step = ST_APIKEY;
        else {
            w->step = ST_AUTH_LOCATION;
            w->sel = str_eq(w->auth_loc, "env") ? 0 : 1;
        }
        break;
    case ST_OAUTH_TOKEN_URL: w->step = ST_OAUTH_AUTH_URL; break;
    case ST_OAUTH_CLIENT_ID: w->step = ST_OAUTH_TOKEN_URL; break;
    case ST_OAUTH_CLIENT_SECRET: w->step = ST_OAUTH_CLIENT_ID; break;
    case ST_OAUTH_SCOPES: w->step = ST_OAUTH_CLIENT_SECRET; break;
    case ST_SCOPE:
        if (str_eq(w->auth_method, "oauth")) w->step = ST_OAUTH_SCOPES;
        else w->step = str_eq(w->auth_loc, "env") ? ST_ENV_VAR_NAME : ST_HEADER_NAME;
        break;
    case ST_OAUTH_ERROR: w->step = ST_OAUTH_AUTH_URL; break;
    case ST_CONFIRM:
        w->step = ST_SCOPE;
        w->sel = str_eq(w->scope, "user") ? 0 : 1;
        break;
    default: break;
    }
    render_step(w);
}

static void complete(wiz_t *w) {
    if (!w->scope) return;
    json_t *cfg = build_config(w, true, false);
    char *name = xstrdup(w->name ? w->name : "");
    const char *scope = w->scope;
    ui_t *u = w->u;
    close_wizard(w, false);
    mcpui_add_server(u, name, cfg, scope);
    json_free(cfg);
    free(name);
}

/* #selectCurrentOption */
static void select_option(wiz_t *w) {
    switch (w->step) {
    case ST_TRANSPORT:
        w->transport = TR_TAG[MAX(0, MIN(2, w->sel))];
        w->step = str_eq(w->transport, "stdio") ? ST_COMMAND : ST_URL;
        break;
    case ST_AUTH_METHOD:
        w->auth_method = w->sel == 0 ? "oauth" : "manual";
        w->step = w->sel == 0 ? ST_OAUTH_AUTH_URL : ST_APIKEY;
        break;
    case ST_OAUTH_ERROR:
        if (w->sel == 0) {
            launch_oauth(w);
            return;
        }
        w->step = ST_OAUTH_AUTH_URL;
        break;
    case ST_AUTH_LOCATION:
        w->auth_loc = w->sel == 0 ? "env" : "header";
        w->step = w->sel == 0 ? ST_ENV_VAR_NAME : ST_HEADER_NAME;
        break;
    case ST_SCOPE:
        w->scope = w->sel == 0 ? "user" : "project";
        w->step = ST_CONFIRM;
        w->sel = 0;
        break;
    case ST_CONFIRM: complete(w); return;
    default: return;
    }
    render_step(w);
}

static bool valid_http_url(const char *v, const char **why) {
    const char *p = strstr(v, "://");
    if (!p || p == v) return *why = "Invalid URL format (must start with http:// or https://)", false;
    for (const char *c = v; c < p; c++)
        if (!isalnum((unsigned char)*c) && *c != '+' && *c != '-' && *c != '.')
            return *why = "Invalid URL format (must start with http:// or https://)", false;
    if (strncasecmp(v, "http://", 7) && strncasecmp(v, "https://", 8)) return *why = "URL must use http:// or https:// scheme", false;
    if (!p[3] || p[3] == '/') return *why = "Invalid URL format (must start with http:// or https://)", false;
    return true;
}

/* #saveInputAndProceed */
static void save_input(wiz_t *w) {
    char *value = str_trim(ui_input_value(&w->in));
    switch (w->step) {
    case ST_NAME: {
        set_str(&w->name, value);
        const char *e = mcp_validate_server_name(value);
        set_str(&w->validation, e);
        if (!e) {
            w->step = ST_TRANSPORT;
            w->sel = 0;
        }
        break;
    }
    case ST_COMMAND:
        if (!*value) goto out;
        set_str(&w->command, value);
        w->step = ST_ARGS;
        break;
    case ST_ARGS:
        set_str(&w->args, value);
        test_connection(w);
        goto out;
    case ST_URL: {
        set_str(&w->url, value);
        const char *why = NULL;
        if (!*value) set_str(&w->validation, "URL is required");
        else if (!valid_http_url(value, &why)) set_str(&w->validation, why);
        else {
            set_str(&w->validation, NULL);
            test_connection(w);
            goto out;
        }
        break;
    }
    case ST_OAUTH_AUTH_URL:
        if (!*value) goto out;
        set_str(&w->o_auth, value);
        w->step = ST_OAUTH_TOKEN_URL;
        break;
    case ST_OAUTH_TOKEN_URL:
        if (!*value) goto out;
        set_str(&w->o_token, value);
        w->step = ST_OAUTH_CLIENT_ID;
        break;
    case ST_OAUTH_CLIENT_ID:
        if (!*value) goto out;
        set_str(&w->o_client, value);
        w->step = ST_OAUTH_CLIENT_SECRET;
        break;
    case ST_OAUTH_CLIENT_SECRET:
        set_str(&w->o_secret, value);
        w->step = ST_OAUTH_SCOPES;
        break;
    case ST_OAUTH_SCOPES:
        set_str(&w->o_scopes, value);
        launch_oauth(w);
        goto out;
    case ST_APIKEY:
        if (!*value) goto out;
        set_str(&w->apikey, value);
        if (str_eq(w->transport, "stdio")) w->step = ST_ENV_VAR_NAME;
        else {
            w->step = ST_AUTH_LOCATION;
            w->sel = 0;
        }
        break;
    case ST_ENV_VAR_NAME:
        if (!*value) goto out;
        set_str(&w->env_name, value);
        w->auth_loc = "env";
        w->step = ST_SCOPE;
        w->sel = 0;
        break;
    case ST_HEADER_NAME:
        if (!*value) goto out;
        set_str(&w->header_name, value);
        w->auth_loc = "header";
        w->step = ST_SCOPE;
        w->sel = 0;
        break;
    default: break;
    }
    render_step(w);
out:
    free(value);
}

static bool is_ctrl_c(const tui_key_t *k) { return tui_key_matches(k, "ctrl+c"); }

static bool wiz_key_inner(wiz_t *w, const tui_key_t *k);

static bool wiz_key(tui_component_t *c, const tui_key_t *k) {
    wiz_t *w = c->user;
    if (w->closed) return true;
    __atomic_add_fetch(&w->refs, 1, __ATOMIC_SEQ_CST); /* closing the wizard destroys the component */
    bool r = wiz_key_inner(w, k);
    wiz_unref(w);
    return r;
}

static bool wiz_key_inner(wiz_t *w, const tui_key_t *k) {
    bool interrupt = tui_kb(k, "app.interrupt");
    if (w->oauth_running && (is_ctrl_c(k) || interrupt)) {
        w->oauth_abort = 1;
        return true;
    }
    if (is_ctrl_c(k)) {
        close_wizard(w, true);
        return true;
    }
    if (w->kind == K_INPUT && w->has_input) {
        if (tui_kb(k, "tui.input.submit")) {
            char *v = str_trim(ui_input_value(&w->in));
            bool empty = !*v;
            free(v);
            if (empty && !w->optional) return true; /* empty: "reject" keeps the field */
            save_input(w);
        } else if (tui_kb(k, "tui.select.cancel")) {
            if (w->step == ST_NAME) {
                close_wizard(w, true);
                return true;
            }
            go_back(w);
        } else {
            ui_input_key(&w->in, k);
        }
        request(w);
        return true;
    }
    if (w->kind == K_ASYNC && !interrupt) return true;
    if (interrupt) {
        if (w->step == ST_NAME) {
            close_wizard(w, true);
            return true;
        }
        go_back(w);
    } else if (tui_key_matches(k, "enter") || tui_key_matches(k, "return") ||
               (k->kind == TUI_KEY_CHAR && k->codepoint == '\n' && !k->mods)) {
        select_option(w);
        if (w->closed) return true;
    } else if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) {
        int n = w->kind == K_CHOICE ? MAX(1, w->nch) : 1;
        w->sel = (w->sel + (tui_kb(k, "tui.select.up") ? -1 : 1) + n) % n;
    }
    request(w);
    return true;
}

static void wiz_destroy(tui_component_t *c) {
    wiz_t *w = c->user;
    w->closed = true;
    w->oauth_abort = 1;
    if (w->spin_timer && w->u->tui) tui_remove_timer(w->u->tui, w->spin_timer);
    w->spin_timer = 0;
    if (g_wiz == w) g_wiz = NULL;
    free(c);
    wiz_unref(w);
}

static const tui_component_vt WIZ_VT = {"mcp-add-wizard", wiz_render, wiz_key, NULL, wiz_destroy};

void ui_mcp_wizard_open(ui_t *u, const char *initial_name) {
    wiz_t *w = xcalloc(1, sizeof *w);
    w->u = u;
    w->refs = 1; /* the component */
    w->auth_method = "none";
    w->env_name = xstrdup("API_KEY");
    w->header_name = xstrdup("Authorization");
    w->health_row = -1;
    char *init = initial_name ? str_trim(initial_name) : NULL;
    if (init && *init) {
        w->name = init;
        w->step = ST_TRANSPORT;
    } else {
        free(init);
        w->step = ST_NAME;
    }
    render_step(w);
    tui_component_t *c = xcalloc(1, sizeof *c);
    tui_component_init(c, &WIZ_VT);
    c->user = w;
    g_wiz = w;
    ui_show_selector(u, c, c);
}
