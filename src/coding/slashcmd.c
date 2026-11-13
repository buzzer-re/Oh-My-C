/* slashcmd.c - builtin slash-command registry + text-mode (RPC/ACP) handlers. See slashcmd.h. */
#include "coding/slashcmd.h"

#include "coding/advisor.h"
#include "coding/memory.h"
#include "coding/modelctl.h"
#include "coding/modelpreset.h"
#include "coding/prewalk.h"
#include "config/settings.h"
#include "config/usage.h"
#include "mcp/mcp.h"
#include "core/embed.h"
#include "session/dump.h"
#include "session/session.h"
#include "session/worktree.h"
#include "tools/approval.h"

#include <ctype.h>
#include <strings.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>

/* ---------- what omc implements ---------- */

static void h_model(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_effort(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_switch(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_modelpreset(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_prewalk(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_advisor(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_compact(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_shake(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_handoff(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_retry(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_memory(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_rename(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_dirs(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_dump(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_forget_approvals(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_fast(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_slow(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_extended_context(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_fresh(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_pin(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_unpin(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_move(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_wt(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_add_dir(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_remove_dir(const slash_parsed_t *, slash_rt_t *, slash_result_t *);
static void h_usage(const slash_parsed_t *, slash_rt_t *, slash_result_t *);

/* upstream name -> supported subcommands ("*" all, NULL none), TUI handler present, text handler.
 * Subcommands left out are not implemented (their upstream behaviour would be advertised falsely). */
static const struct {
    const char *name;
    const char *subs;
    bool tui;
    slash_handle_fn handle;
} OMC[] = {
    {"settings", NULL, true, NULL},
    {"setup", "*", true, NULL},
    {"plan", NULL, true, NULL},
    {"plan-review", NULL, true, NULL},
    {"goal", "*", true, NULL},
    {"guided-goal", NULL, true, NULL},
    {"loop", NULL, true, NULL},
    {"queue", NULL, true, NULL},
    {"model", NULL, true, h_model},
    {"switch", NULL, true, h_switch},
    {"prewalk", "*", false, h_prewalk},
    {"modelpreset", "*", true, h_modelpreset},
    {"effort", "*", true, h_effort},
    {"fast", "*", true, h_fast},
    {"slow", "*", true, h_slow},
    {"skillful", "*", true, slash_skillful_text},
    {"extended-context", "*", true, h_extended_context},
    {"advisor", "on off status dump", true, h_advisor},
    {"export", NULL, true, NULL},
    {"dump", "*", true, h_dump},
    {"copy", NULL, true, NULL},
    {"open", NULL, true, NULL},
    {"todo", "*", true, NULL},
    {"session", "info delete pin", true, NULL},
    {"jobs", "full kill", true, NULL},
    {"agents", NULL, true, NULL},
    {"hub", NULL, true, NULL},
    {"usage", "*", true, h_usage},
    {"changelog", "*", true, slash_changelog_text},
    {"hotkeys", NULL, true, NULL},
    {"tools", NULL, true, NULL},
    {"context", NULL, true, NULL},
    {"branch", NULL, true, NULL},
    {"fork", NULL, true, NULL},
    {"tree", NULL, true, NULL},
    {"login", NULL, true, NULL},
    {"logout", NULL, true, NULL},
    {"mcp", "add list remove test reauth unauth enable disable reconnect reload resources prompts notifications help", true, slash_mcp_text},
    {"new", NULL, true, NULL},
    {"fresh", NULL, true, h_fresh},
    {"clear", NULL, true, NULL},
    {"delete", NULL, true, NULL},
    {"compact", "soft", true, h_compact},
    {"shake", "*", true, h_shake},
    {"handoff", NULL, true, h_handoff},
    {"resume", NULL, true, NULL},
    {"pin", NULL, false, h_pin},
    {"unpin", NULL, false, h_unpin},
    {"btw", NULL, true, NULL},
    {"retry", NULL, true, h_retry},
    {"memory", "view stats diagnose queue sync clear reset enqueue rebuild", true, h_memory},
    {"rename", NULL, true, h_rename},
    {"move", NULL, true, h_move},
    {"wt", NULL, true, h_wt},
    {"add-dir", NULL, false, h_add_dir},
    {"remove-dir", NULL, false, h_remove_dir},
    {"dirs", NULL, false, h_dirs},
    {"exit", NULL, true, NULL},
    {"restart", NULL, true, NULL},
    {"local", NULL, true, NULL},
    {"skills", NULL, true, NULL},
    {"force", NULL, true, slash_force_text},
    {"pause", NULL, true, NULL},
    {"debug", NULL, true, NULL},
    {"record", NULL, true, NULL},
    {"forget-approvals", NULL, false, h_forget_approvals},
    {"quit", NULL, true, NULL},
    {"ssh", "*", true, slash_ssh_text},
};

/* the alias of an upstream command whose target omc lacks never reaches the registry */
static VEC(slash_cmd_t *) g_reg;
static json_t *g_meta;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static bool word_in(const char *list, const char *w) {
    if (!list) return false;
    if (!strcmp(list, "*")) return true;
    size_t n = strlen(w);
    for (const char *p = list; (p = strstr(p, w)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
    return false;
}

static void registry_init(void) {
    size_t len = 0;
    const char *data = embed_get("assets/tui/slash-commands.json", &len);
    g_meta = data ? json_parse(data, len, NULL) : NULL;
    if (!g_meta) g_meta = json_arr();
    json_t *e;
    json_arr_foreach(g_meta, i, e) {
        const char *name = json_get_str(e, "name");
        size_t k = 0;
        while (k < ARRAY_LEN(OMC) && !str_eq(OMC[k].name, name)) k++;
        if (k == ARRAY_LEN(OMC)) continue;
        slash_cmd_t *c = xcalloc(1, sizeof *c);
        c->name = name;
        json_t *al;
        json_arr_foreach(json_get_arr(e, "aliases"), j, al) strvec_push(&c->aliases, json_sv(al));
        c->icon = json_get_str(e, "icon");
        c->description = json_get_str(e, "description") ? json_get_str(e, "description") : "";
        c->acp_description = json_get_str(e, "acpDescription");
        c->inline_hint = json_get_str(e, "inlineHint");
        c->acp_input_hint = json_get_str(e, "acpInputHint");
        c->allow_args = json_get_bool(e, "allowArgs", false);
        c->sub_optional = json_get_bool(e, "subcommandOptional", false);
        c->tui = OMC[k].tui;
        c->handle = OMC[k].handle;
        json_t *subs = json_get_arr(e, "subcommands"), *s;
        c->subs = xcalloc(json_len(subs) + 1, sizeof *c->subs);
        json_arr_foreach(subs, j, s) {
            if (!word_in(OMC[k].subs, json_get_str(s, "name"))) continue;
            c->subs[c->nsubs++] = (slash_sub_t){json_get_str(s, "name"), json_get_str(s, "description"), json_get_str(s, "usage")};
        }
        /* a hint the extractor left as a template literal (/compact's `[${MODES}] [focus]`): the alternatives are the subcommands */
        const char *tmpl_end = c->acp_input_hint && str_starts(c->acp_input_hint, "[${") ? strstr(c->acp_input_hint, "}]") : NULL;
        if (tmpl_end) {
            sb_t h = SB_INIT;
            sb_putc(&h, '[');
            for (size_t j = 0; j < c->nsubs; j++) sb_appendf(&h, "%s%s", j ? "|" : "", c->subs[j].name);
            sb_append(&h, tmpl_end + 1);
            c->acp_input_hint = sb_detach(&h);
        } else if (c->acp_input_hint && *c->acp_input_hint == '[' && json_len(subs) != c->nsubs && c->nsubs) {
            /* an ACP hint naming every upstream subcommand: keep the alternatives omc implements */
            sb_t h = SB_INIT;
            sb_putc(&h, '[');
            const char *p = c->acp_input_hint + 1;
            int kept = 0;
            for (;;) {
                const char *q = p;
                int depth = 0;
                for (; *q && !(depth == 0 && (*q == '|' || *q == ']')); q++) {
                    if (*q == '[') depth++;
                    else if (*q == ']') depth--;
                }
                size_t word = strcspn(p, " |]");
                char *w = xstrndup(p, word);
                bool on = false;
                for (size_t j = 0; j < c->nsubs && !on; j++) on = str_eq(c->subs[j].name, w);
                free(w);
                if (on) sb_appendf(&h, "%s%.*s", kept++ ? "|" : "", (int)(q - p), p);
                if (*q != '|') {
                    sb_append(&h, *q ? q : "]");
                    break;
                }
                p = q + 1;
            }
            c->acp_input_hint = sb_detach(&h);
        }
        /* the level's description is generated upstream and the extracted table leaves it blank */
        if (!strcmp(c->name, "effort"))
            for (size_t j = 0; j < c->nsubs; j++)
                if (str_eq(c->subs[j].name, "minimal") && (!c->subs[j].description || !*c->subs[j].description))
                    c->subs[j].description = "Very brief reasoning (~1k tokens)";
        /* /resume: omc has no foreign-session (@claude/@codex) import */
        if (!strcmp(c->name, "resume")) c->inline_hint = "[session id]";
        if (!strcmp(c->name, "restart")) c->description = "Restart omc with the same launch flags, resuming this session";
        if (!strcmp(c->name, "record")) c->description = "Start or stop recording this screen to a replayable file (omc play)";
        vec_push(&g_reg, c);
    }
    /* omc-only commands */
    slash_cmd_t *q = xcalloc(1, sizeof *q);
    q->name = "quiet";
    q->icon = "quiet"; /* a muted bell: command_icon() in src/ui/slash.c */
    q->description = "Hide warnings and status messages when omc opens";
    q->inline_hint = "[on|off|status]";
    q->allow_args = true;
    q->tui = true;
    q->subs = xcalloc(1, sizeof *q->subs);
    vec_push(&g_reg, q);
    slash_cmd_t *m = xcalloc(1, sizeof *m);
    m->name = "migrate";
    m->description = "Copy your omp configuration into omc";
    m->inline_hint = "[--dry-run] [--force] [--no-sessions]";
    m->allow_args = true;
    m->tui = true;
    m->subs = xcalloc(1, sizeof *m->subs);
    vec_push(&g_reg, m);
}

size_t slash_count(void) {
    pthread_once(&g_once, registry_init);
    return g_reg.n;
}

const slash_cmd_t *slash_at(size_t i) { return i < slash_count() ? g_reg.v[i] : NULL; }

const slash_cmd_t *slash_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < slash_count(); i++) {
        const slash_cmd_t *c = g_reg.v[i];
        if (!strcmp(c->name, name)) return c;
        for (size_t k = 0; k < c->aliases.n; k++)
            if (!strcmp(c->aliases.v[k], name)) return c;
    }
    return NULL;
}

static char *trim_dup(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

bool slash_parse(const char *text, slash_parsed_t *out) {
    memset(out, 0, sizeof *out);
    if (!text || text[0] != '/' || !text[1]) return false;
    const char *body = text + 1;
    size_t sep = 0;
    while (body[sep] && !isspace((unsigned char)body[sep]) && body[sep] != ':') sep++;
    out->name = xstrndup(body, sep);
    out->args = body[sep] ? trim_dup(body + sep + 1) : xstrdup("");
    out->text = text;
    return true;
}

void slash_parsed_free(slash_parsed_t *p) {
    free(p->name);
    free(p->args);
    memset(p, 0, sizeof *p);
}

const slash_cmd_t *slash_match(const char *text, slash_parsed_t *out) {
    if (!slash_parse(text, out)) return NULL;
    const slash_cmd_t *c = slash_lookup(out->name);
    if (!c || (*out->args && !c->allow_args)) {
        slash_parsed_free(out);
        return NULL;
    }
    return c;
}

char *slash_format_description(const char *d, char *(*key_fmt)(const char *key)) {
    sb_t b = SB_INIT;
    for (const char *p = d ? d : ""; *p;) {
        const char *k = strstr(p, "{key:");
        const char *end = k ? strchr(k, '}') : NULL;
        if (!k || !end) {
            sb_append(&b, p);
            break;
        }
        sb_appendn(&b, p, (size_t)(k - p));
        char *key = xstrndup(k + 5, (size_t)(end - k - 5));
        char *f = key_fmt ? key_fmt(key) : NULL;
        sb_append(&b, f ? f : key);
        free(f);
        free(key);
        p = end + 1;
    }
    return sb_detach(&b);
}

/* ---------- shared parsing ---------- */

void slash_subcommand(const char *args, char **verb, char **rest) {
    char *t = trim_dup(args ? args : "");
    size_t n = 0;
    while (t[n] && !isspace((unsigned char)t[n])) n++;
    *verb = xstrndup(t, n);
    for (char *p = *verb; *p; p++) *p = (char)tolower((unsigned char)*p);
    *rest = trim_dup(t + n);
    free(t);
}

bool slash_parse_compact_args(const char *args, const char **mode, char **instructions, char **err) {
    static const char *const MODES[] = {"soft", "remote", "snapcompact"};
    *mode = NULL;
    *instructions = NULL;
    *err = NULL;
    char *t = trim_dup(args ? args : "");
    if (!*t) {
        free(t);
        return true;
    }
    size_t n = 0;
    while (t[n] && !isspace((unsigned char)t[n])) n++;
    char *first = xstrndup(t, n);
    for (char *p = first; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (size_t i = 0; i < ARRAY_LEN(MODES); i++)
        if (!strcmp(first, MODES[i])) *mode = MODES[i];
    free(first);
    if (!*mode) { /* no recognized mode prefix: the whole thing is focus */
        *instructions = t;
        return true;
    }
    char *focus = trim_dup(t + n);
    free(t);
    if (!strcmp(*mode, "snapcompact") && *focus) {
        *err = xstrdup("/compact snapcompact does not take focus instructions (it archives history without an LLM summary).");
        free(focus);
        *mode = NULL;
        return false;
    }
    if (*focus) *instructions = focus;
    else free(focus);
    return true;
}

const char *slash_parse_shake_mode(const char *args, char **err) {
    char *v = trim_dup(args ? args : "");
    for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p);
    const char *m = !*v || !strcmp(v, "elide") ? "elide" : !strcmp(v, "images") ? "images" : !strcmp(v, "thinking") ? "thinking" : NULL;
    if (!m) *err = xasprintf("Unknown /shake mode \"%s\". Use elide, images, or thinking.", v);
    free(v);
    return m;
}

void slash_effort_selectors(app_t *a, strvec_t *out) {
    if (!a->model || !a->model->reasoning) return;
    strvec_push(out, "off");
    /* auto never opts an unverified ladder into thinking */
    if (!json_get_bool(a->model->thinking, "unverified", false)) strvec_push(out, "auto");
    for (int i = EFFORT_MINIMAL; i <= EFFORT_MAX; i++)
        if (model_supports_effort(a->model, (ai_effort_t)i)) strvec_push(out, effort_name((ai_effort_t)i));
}

char *slash_no_thinking_message(app_t *a) {
    return a->model ? xasprintf("%s/%s has no adjustable thinking level.", a->model->provider, a->model->id)
                    : xstrdup("The current model has no adjustable thinking level.");
}

const char *slash_configured_thinking(app_t *a) {
    if (!a->model || !a->model->reasoning) return "model default";
    return a->auto_thinking ? "auto" : effort_name(a->thinking);
}

int slash_resolve_effort(app_t *a, const char *args, char **level, char **err) {
    if (!a->model || !a->model->reasoning) {
        *err = slash_no_thinking_message(a);
        return -1;
    }
    strvec_t ch = {0};
    slash_effort_selectors(a, &ch);
    char *sel = trim_dup(args);
    for (char *p = sel; *p; p++) *p = (char)tolower((unsigned char)*p);
    bool ok = false;
    for (size_t i = 0; i < ch.n && !ok; i++) ok = !strcmp(ch.v[i], sel);
    if (ok) *level = xstrdup(sel);
    else {
        sb_t b = SB_INIT;
        sb_appendf(&b, "Unknown thinking level: %s. Available: ", sel);
        for (size_t i = 0; i < ch.n; i++) sb_appendf(&b, "%s%s", i ? ", " : "", ch.v[i]);
        *err = sb_detach(&b);
    }
    free(sel);
    strvec_free(&ch);
    return ok ? 0 : -1;
}

const char *slash_advisor_set(app_t *a, int mode) {
    bool on = mode < 0 ? !a->advisor_enabled : mode > 0;
    bool active = app_set_advisor(a, on);
    if (active) return "Advisor enabled.";
    if (a->advisor_enabled) return "Advisor setting enabled, but no model is assigned to the 'advisor' role.";
    return "Advisor disabled.";
}

/* Number.prototype.toLocaleString() (en-US grouping) */
char *slash_locale_int(int64_t v) {
    char raw[32];
    snprintf(raw, sizeof raw, "%lld", (long long)(v < 0 ? -v : v));
    sb_t b = SB_INIT;
    if (v < 0) sb_putc(&b, '-');
    size_t n = strlen(raw);
    for (size_t i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) sb_putc(&b, ',');
        sb_putc(&b, raw[i]);
    }
    return sb_detach(&b);
}

/* formatAdvisorStatus (ACP/text) */
char *slash_advisor_status_text(app_t *a) {
    if (!a->advisor)
        return xstrdup(a->advisor_enabled ? "Advisor setting is enabled, but no model is assigned to the 'advisor' role." : "Advisor is disabled.");
    json_t *st = advisor_stats(a->advisor);
    json_t *m = json_get(st, "model"), *tk = json_get(st, "tokens");
    int64_t ct = json_get_int(st, "contextTokens", 0), cw = json_get_int(st, "contextWindow", 0);
    char *cts = slash_locale_int(ct), *cws = slash_locale_int(cw);
    char *ctx = cw > 0 ? xasprintf("Context: %s / %s tokens (%d%%)", cts, cws, (int)lround(100.0 * (double)ct / (double)cw))
                       : xasprintf("Context: %s tokens", cts);
    sb_t sp = SB_INIT;
    char *in = slash_locale_int(json_get_int(tk, "input", 0)), *out = slash_locale_int(json_get_int(tk, "output", 0));
    sb_appendf(&sp, "%s input, %s output", in, out);
    if (json_get_int(tk, "cacheRead", 0) > 0) {
        char *x = slash_locale_int(json_get_int(tk, "cacheRead", 0));
        sb_appendf(&sp, ", %s cache read", x);
        free(x);
    }
    if (json_get_int(tk, "cacheWrite", 0) > 0) {
        char *x = slash_locale_int(json_get_int(tk, "cacheWrite", 0));
        sb_appendf(&sp, ", %s cache write", x);
        free(x);
    }
    char *s = xasprintf("Advisor is enabled (%s/%s). %s. Spend: %s, $%.4f.", json_get_str(m, "provider"), json_get_str(m, "id"), ctx,
                        sb_cstr(&sp), json_get_num(st, "cost", 0));
    sb_free(&sp);
    free(in);
    free(out);
    free(ctx);
    free(cts);
    free(cws);
    json_free(st);
    return s;
}

/* ---------- text-mode handlers ---------- */

/* resolveSessionModelSelector: a model + its explicit `:level` selector (owned or NULL) */
const ai_model_t *slash_resolve_session_model(const char *selector, char **level) {
    *level = NULL;
    char *t = str_trim(selector ? selector : "");
    ai_effort_t e;
    bool has_e = false;
    const ai_model_t *m = *t ? app_resolve_model_pattern(t, &e, &has_e) : NULL;
    if (m) {
        const char *colon = strrchr(t, ':');
        if (colon && str_eq(colon + 1, "auto")) *level = xstrdup("auto");
        else if (has_e) *level = xstrdup(effort_name(e));
    }
    free(t);
    return m;
}

static void out(slash_rt_t *rt, const char *text) {
    if (rt->output && text && *text) rt->output(rt->ud, text);
}

static void usage(slash_rt_t *rt, slash_result_t *res, const char *text) {
    out(rt, text);
    res->failed = true;
}

static char *lower_trim(const char *args) {
    char *t = str_trim(args ? args : "");
    for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
    return t;
}

/* builtin-modes.ts fast / slow / extended-context handle */
static void h_fast(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *arg = lower_trim(c->args);
    char *m = modelctl_fast_command(rt->app, arg);
    if (m) out(rt, m);
    else usage(rt, res, "Usage: /fast [on|ultra|off|status]");
    free(m);
    free(arg);
}

static void h_slow(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *arg = lower_trim(c->args);
    char *m = modelctl_slow_command(rt->app, arg);
    if (m) out(rt, m);
    else usage(rt, res, "Usage: /slow [on|off|status]");
    free(m);
    free(arg);
}

static void h_extended_context(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *m = modelctl_extended_context_command(c->args);
    if (m) out(rt, m);
    else usage(rt, res, "Usage: /extended-context [on|off|status]");
    free(m);
}

static void h_model(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    app_t *a = rt->app;
    if (*c->args) {
        char *err = NULL;
        if (app_set_model(a, c->args, &err) != 0) {
            char *m = xasprintf("Unknown model: %s. Use ACP `session/setModel` for picker-driven selection or list available models with /model.", c->args);
            usage(rt, res, m);
            free(m);
        } else {
            char *m = xasprintf("Model set to %s/%s.", a->model->provider, a->model->id);
            out(rt, m);
            free(m);
        }
        free(err);
        return;
    }
    char *m = a->model ? xasprintf("Current model: %s/%s", a->model->provider, a->model->id) : xstrdup("No model is currently selected.");
    out(rt, m);
    free(m);
}

/* /switch (builtin-modes.ts handle): session-only model, never persisted */
static void h_switch(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    app_t *a = rt->app;
    if (!*c->args) {
        char *m = a->model ? xasprintf("Current model: %s/%s", a->model->provider, a->model->id)
                           : xstrdup("No model is currently selected.");
        out(rt, m);
        free(m);
        return;
    }
    char *lv = NULL;
    const ai_model_t *m = slash_resolve_session_model(c->args, &lv);
    if (!m) {
        char *u = xasprintf("Unknown model: %s", c->args);
        usage(rt, res, u);
        free(u);
        free(lv);
        return;
    }
    char *err = NULL;
    if (app_set_model_temporary(a, m, lv, NULL, &err) != 0) {
        char *u = xasprintf("Failed to switch model: %s", err);
        usage(rt, res, u);
        free(u);
    } else {
        char *o = xasprintf("Session-only model: %s/%s.", m->provider, m->id);
        out(rt, o);
        free(o);
    }
    free(err);
    free(lv);
}

/* /prewalk [into] | restart [from] [into] | off */
static void h_prewalk(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    bool failed = false;
    char *m = prewalk_command(rt->app, c->args, &failed);
    if (m && failed) usage(rt, res, m);
    else out(rt, m);
    free(m);
}

/* /modelpreset (runPresetsCommand) */
static void h_modelpreset(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    mp_outcome_t o;
    mp_command(rt->app, c->args, &o);
    if (o.usage) usage(rt, res, o.message);
    else out(rt, o.message);
    mp_outcome_free(&o);
}

static void h_effort(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    app_t *a = rt->app;
    if (!*c->args) {
        if (!a->model || !a->model->reasoning) {
            char *m = slash_no_thinking_message(a);
            out(rt, m);
            free(m);
            return;
        }
        strvec_t ch = {0};
        slash_effort_selectors(a, &ch);
        sb_t b = SB_INIT;
        sb_appendf(&b, "Thinking: %s\nAvailable: ", slash_configured_thinking(a));
        for (size_t i = 0; i < ch.n; i++) sb_appendf(&b, "%s%s", i ? ", " : "", ch.v[i]);
        out(rt, sb_cstr(&b));
        sb_free(&b);
        strvec_free(&ch);
        return;
    }
    char *lv = NULL, *err = NULL;
    if (slash_resolve_effort(a, c->args, &lv, &err) != 0) {
        usage(rt, res, err);
        free(err);
        return;
    }
    app_set_thinking_level(a, lv);
    char *m = xasprintf("Thinking set to %s.", lv);
    out(rt, m);
    free(m);
    free(lv);
}

static void h_advisor(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *verb, *rest;
    slash_subcommand(c->args, &verb, &rest);
    if (!*verb || !strcmp(verb, "toggle")) out(rt, slash_advisor_set(rt->app, -1));
    else if (!strcmp(verb, "on")) out(rt, slash_advisor_set(rt->app, 1));
    else if (!strcmp(verb, "off")) out(rt, slash_advisor_set(rt->app, 0));
    else if (!strcmp(verb, "status")) {
        char *s = slash_advisor_status_text(rt->app);
        out(rt, s);
        free(s);
    } else if (!strcmp(verb, "dump")) {
        char *h = app_advisor_history_text(rt->app, strcasecmp(rest, "raw") != 0);
        out(rt, h ? h : "Advisor is not active for this session.");
        free(h);
    } else if (!strcmp(verb, "configure"))
        out(rt, "/advisor configure opens an interactive editor and is only available in the interactive TUI.");
    else usage(rt, res, "Usage: /advisor [on|off|status|dump [raw]|configure]");
    free(verb);
    free(rest);
}

static void h_compact(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    const char *mode;
    char *focus, *err = NULL;
    if (!slash_parse_compact_args(c->args, &mode, &focus, &err)) {
        usage(rt, res, err);
        free(err);
        return;
    }
    int64_t before = rt->app->last_context_tokens;
    if (app_compact_mode(rt->app, mode, focus, &err) != 0) {
        char *m = xasprintf("Compaction failed: %s", err ? err : "compaction failed");
        usage(rt, res, m);
        free(m);
    } else {
        json_t *st = app_stats(rt->app);
        int64_t after = json_get_int(st, "contextTokens", 0);
        json_free(st);
        char *m = before > 0 && after > 0
                      ? xasprintf("Compaction complete. Tokens: %lld -> %lld (saved %lld).", (long long)before, (long long)after, (long long)(before - after))
                      : xstrdup("Compaction complete.");
        out(rt, m);
        free(m);
    }
    free(err);
    free(focus);
}

static void h_shake(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *err = NULL;
    const char *mode = slash_parse_shake_mode(c->args, &err);
    if (!mode) {
        usage(rt, res, err);
        free(err);
        return;
    }
    app_shake_result_t r;
    app_shake(rt->app, mode, &r);
    char *s = app_shake_summary(&r);
    out(rt, s);
    free(s);
}

static void h_handoff(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    if (agent_is_running(rt->app->agent)) {
        usage(rt, res, "Wait for the current response to finish or abort it before handing off.");
        return;
    }
    if (rt->app->compacting) {
        usage(rt, res, "Handoff generation is already in progress.");
        return;
    }
    char *err = NULL;
    if (app_handoff(rt->app, *c->args ? c->args : NULL, &err) != 0) {
        char *m = xasprintf("Handoff failed: %s", err ? err : "handoff failed");
        usage(rt, res, m);
        free(m);
    } else {
        out(rt, "Context handed off and compacted in place.");
    }
    free(err);
}

static void h_retry(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    (void)c;
    if (agent_is_running(rt->app->agent)) {
        usage(rt, res, "Wait for the current response to finish or abort it before retrying.");
        return;
    }
    if (!app_retry_prepare(rt->app)) {
        usage(rt, res, "Nothing to retry.");
        return;
    }
    out(rt, "Retrying the last failed turn.");
    if (rt->agent_invoked) rt->agent_invoked(rt->ud);
    agent_continue(rt->app->agent);
    res->agent_invoked = true;
}

static void h_memory(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    memory_cmd_result_t r;
    memory_command(rt->app, c->args, &r);
    if (str_eq(r.level, "warning") || str_eq(r.level, "error")) res->failed = true;
    out(rt, r.text);
    memory_cmd_result_free(&r);
}

static void h_rename(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    (void)res;
    app_t *a = rt->app;
    char *t = *c->args ? xstrdup(c->args) : app_generate_title(a);
    if (!t || !*t) {
        out(rt, "Could not generate a session title. Use /rename <title> to set one.");
    } else {
        session_set_title(a->session, t, "user");
        char *m = xasprintf("Session renamed to %s.", t);
        out(rt, m);
        free(m);
    }
    free(t);
}

/* builtin-session.ts usage.handle: helpers/usage-report.ts buildUsageReportText, or the reset listing / spend */
char *slash_usage_reset_text(app_t *a, const char *arg) {
    const char *prov = a && a->model ? a->model->provider : NULL;
    json_t *st = usage_list_reset_credits(prov), *accounts = usage_reset_accounts(st);
    json_free(st);
    char *r;
    char *t = str_trim(arg ? arg : "");
    if (json_len(accounts) == 0) r = xstrdup("No provider accounts found. Use /login to add one.");
    else if (!*t) {
        sb_t b = SB_INIT;
        sb_append(&b, "Saved rate-limit resets:");
        json_t *ac;
        json_arr_foreach(accounts, i, ac) {
            char *l = usage_reset_account_line(ac);
            sb_appendf(&b, "\n%s", l);
            free(l);
        }
        sb_append(&b, "\n\nSpend one with `/usage reset <provider>/<credential id>` or `/usage reset <provider>/active`.");
        r = sb_detach(&b);
    } else {
        char *err = NULL;
        const json_t *acc = usage_resolve_reset_target(accounts, t, "/usage reset", &err);
        if (!acc) r = err, err = NULL;
        else {
            json_t *o = usage_redeem_reset_credit(json_get_obj(acc, "target"));
            char *m = usage_describe_redeem(o, json_get_str(acc, "label"));
            r = usage_one_line(m);
            free(m);
            json_free(o);
        }
        free(err);
    }
    free(t);
    json_free(accounts);
    return r;
}

char *slash_usage_report_text(app_t *a) {
    json_t *reports = usage_fetch_reports(NULL);
    char *r;
    if (json_len(reports) > 0) {
        const char *prov = a && a->model ? a->model->provider : NULL;
        json_t *id = usage_active_identity(prov);
        json_t *sel = usage_model_selectors(reports);
        r = usage_report_text(reports, now_ms(), prov, id, sel);
        json_free(sel);
        json_free(id);
    } else {
        json_t *st = app_stats(a);
        json_t *x = json_obj();
        int64_t in = json_get_int(st, "input", 0), o = json_get_int(st, "output", 0), cr = json_get_int(st, "cacheRead", 0),
                cw = json_get_int(st, "cacheWrite", 0);
        json_set_int(x, "input", in);
        json_set_int(x, "output", o);
        json_set_int(x, "cacheRead", cr);
        json_set_int(x, "cacheWrite", cw);
        json_set_int(x, "totalTokens", in + o + cr + cw);
        json_set_int(x, "premiumRequests", json_get_int(st, "premiumRequests", 0));
        json_set_num(x, "cost", json_get_num(st, "cost", 0));
        r = usage_session_text(x);
        json_free(x);
        json_free(st);
    }
    json_free(reports);
    return r;
}

static void h_usage(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *verb, *rest;
    slash_subcommand(c->args, &verb, &rest);
    if (!*verb || (!strcmp(verb, "show") && !*rest)) {
        char *m = slash_usage_report_text(rt->app);
        out(rt, m);
        free(m);
    } else if (!strcmp(verb, "reset")) {
        char *m = slash_usage_reset_text(rt->app, rest);
        out(rt, m);
        free(m);
    } else usage(rt, res, "Usage: /usage [show|reset [provider/credential-id|provider/active]]");
    free(verb);
    free(rest);
}

static void h_dirs(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    (void)c;
    (void)res;
    char *m = app_workspace_dirs_text(rt->app, NULL);
    out(rt, m);
    free(m);
}

/* ---------- builtin-lifecycle.ts: fresh, pin, move, wt, add-dir, remove-dir ---------- */

static void outf(slash_rt_t *rt, slash_result_t *res, bool failed, const char *fmt, ...) PRINTF_LIKE(4, 5);
static void outf(slash_rt_t *rt, slash_result_t *res, bool failed, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *m = xvasprintf(fmt, ap);
    va_end(ap);
    out(rt, m);
    if (failed) res->failed = true;
    free(m);
}

static void h_fresh(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    (void)c;
    int n = app_fresh_session(rt->app);
    if (n < 0) out(rt, "Wait for the current response to finish or abort it before refreshing provider state.");
    else outf(rt, res, false, "Fresh provider session started (%d %s pruned).", n, n == 1 ? "provider state" : "provider states");
}

/* resolvePinTarget: the argument's match, else the active session, or the usage result (NULL) */
static char *pin_target(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res, const char *verb) {
    char *id = NULL;
    if (*c->args) {
        char *path = session_resolve(c->args, rt->app->cwd);
        session_t *s = path ? session_open(path, NULL) : NULL;
        if (s) id = xstrdup(session_id(s));
        session_free(s);
        free(path);
        if (!id) outf(rt, res, true, "Session \"%s\" not found.", c->args);
    } else if (rt->app->session && session_id(rt->app->session)) {
        id = xstrdup(session_id(rt->app->session));
    } else {
        char *m = xasprintf("No active session to %s.", verb);
        usage(rt, res, m);
        free(m);
    }
    return id;
}

static void h_pin(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *id = pin_target(c, rt, res, "pin");
    if (!id) return;
    char *err = NULL;
    int r = session_pin_toggle(id, &err);
    if (r < 0) usage(rt, res, err);
    else out(rt, r ? "Session pinned to the top of the resume list." : "Session unpinned.");
    free(err);
    free(id);
}

static void h_unpin(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *id = pin_target(c, rt, res, "unpin");
    if (!id) return;
    char *err = NULL;
    int r = session_pin_clear(id, &err);
    if (r < 0) usage(rt, res, err);
    else out(rt, r ? "Session unpinned." : "Session is not pinned.");
    free(err);
    free(id);
}

/* an existing directory or a usage message (owned) */
static char *check_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return xasprintf("Directory does not exist: %s", path);
    if (!S_ISDIR(st.st_mode)) return xasprintf("Not a directory: %s", path);
    return NULL;
}

static void h_move(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    app_t *a = rt->app;
    if (agent_is_running(a->agent)) {
        usage(rt, res, "Cannot move while streaming.");
        return;
    }
    if (!*c->args) {
        usage(rt, res, "Usage: /move <path>");
        return;
    }
    char *path = app_resolve_user_path(a, c->args);
    char *bad = check_dir(path), *err = NULL;
    if (bad) usage(rt, res, bad);
    else if (app_move(a, path, &err) != 0) usage(rt, res, err);
    else outf(rt, res, false, "Moved to %s.", a->cwd);
    free(bad);
    free(err);
    free(path);
}

static void h_wt(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    app_t *a = rt->app;
    if (agent_is_running(a->agent)) {
        usage(rt, res, "Cannot create a worktree while streaming.");
        return;
    }
    char *branch = *c->args ? xstrdup(c->args) : session_wt_default_branch();
    char *source = xstrdup(a->cwd), *err = NULL;
    session_wt_t wt;
    if (session_wt_create(source, branch, true, &wt, &err) != 0) {
        outf(rt, res, true, "Worktree creation failed: %s", err);
    } else if (app_move(a, wt.path, &err) != 0) {
        usage(rt, res, err);
    } else {
        char *cerr = NULL;
        int cleaned = session_wt_clean_source(source, &cerr);
        if (cleaned < 0) outf(rt, res, false, "Warning: Worktree created, but cleaning source checkout failed: %s", cerr);
        char *sum = session_wt_summary(&wt, cleaned > 0);
        out(rt, sum);
        free(sum);
        free(cerr);
    }
    session_wt_free(&wt);
    free(err);
    free(source);
    free(branch);
}

static void h_add_dir(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    app_t *a = rt->app;
    if (agent_is_running(a->agent)) {
        usage(rt, res, "Cannot add a directory while streaming.");
        return;
    }
    if (!*c->args) {
        char *m = app_workspace_dirs_text(a, "Usage: /add-dir <path>");
        usage(rt, res, m);
        free(m);
        return;
    }
    char *path = app_resolve_user_path(a, c->args);
    char *bad = check_dir(path), *added = NULL, *err = NULL;
    if (bad) usage(rt, res, bad);
    else {
        int r = app_add_workspace_dir(a, path, &added, &err);
        if (r < 0) usage(rt, res, err);
        else if (r == 0) outf(rt, res, false, "Already in the workspace: %s", path);
        else {
            char *note = xasprintf("Added %s.", added);
            char *m = app_workspace_dirs_text(a, note);
            out(rt, m);
            free(m);
            free(note);
        }
    }
    free(added);
    free(err);
    free(bad);
    free(path);
}

static void h_remove_dir(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    app_t *a = rt->app;
    if (agent_is_running(a->agent)) {
        usage(rt, res, "Cannot remove a directory while streaming.");
        return;
    }
    if (!*c->args) {
        usage(rt, res, "Usage: /remove-dir <path>");
        return;
    }
    char *path = app_resolve_user_path(a, c->args);
    if (!strcmp(path, a->cwd)) {
        usage(rt, res, "Cannot remove the working directory; use /move to change it.");
        free(path);
        return;
    }
    char *removed = NULL;
    if (app_remove_workspace_dir(a, path, &removed) == 0) outf(rt, res, false, "Not a workspace directory: %s", path);
    else {
        char *note = xasprintf("Removed %s.", removed);
        char *m = app_workspace_dirs_text(a, note);
        out(rt, m);
        free(m);
        free(note);
    }
    free(removed);
    free(path);
}

/* builtin-control.ts forget-approvals: clearApprovalGrants (the shared file and every session tier) */
static void h_forget_approvals(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    (void)c;
    (void)res;
    approval_grants_clear();
    out(rt, "Remembered approvals forgotten; the next read or write asks again.");
}

/* builtin-collaboration.ts dump handle: transcript (+ the LLM request sidecar path) or an archive report */
static void h_dump(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *verb, *rest;
    slash_subcommand(c->args, &verb, &rest);
    if (!strcmp(verb, "all") || !strcmp(verb, "anon")) {
        dump_archive_t ar;
        char *err = NULL;
        int rc = app_dump_archive(rt->app, !strcmp(verb, "anon"), &ar, &err);
        if (rc == 0) out(rt, "No messages to dump yet.");
        else if (rc < 0) {
            char *m = xasprintf("Failed to write session dump: %s", err ? err : "Unknown error");
            usage(rt, res, m);
            free(m);
        } else {
            char *r = dump_archive_report(&ar);
            out(rt, r);
            free(r);
            dump_archive_free(&ar);
        }
        free(err);
    } else if (*verb) {
        usage(rt, res, "Usage: /dump [all|anon]");
    } else {
        char *text = app_format_session_text(rt->app);
        if (!text || !*text) out(rt, "No messages to dump yet.");
        else {
            char *err = NULL, *side = app_dump_llm_request(rt->app, &err);
            char *m = side ? xasprintf("%s\n\nLLM request JSON: %s\nThis file persists on disk and may contain raw context/secrets \xe2\x80\x94 treat accordingly.", text, side) : xstrdup(text);
            out(rt, m);
            free(m);
            free(side);
            free(err);
        }
        free(text);
    }
    free(verb);
    free(rest);
}

void slash_execute_text(app_t *a, const char *text, slash_output_fn output, void *ud, slash_result_t *res) {
    slash_execute_text_ex(a, text, output, NULL, ud, res);
}

void slash_execute_text_ex(app_t *a, const char *text, slash_output_fn output, void (*agent_invoked)(void *ud), void *ud,
                           slash_result_t *res) {
    memset(res, 0, sizeof *res);
    slash_parsed_t p;
    if (!slash_parse(text, &p)) return;
    const slash_cmd_t *c = slash_lookup(p.name);
    if (c && c->handle) { /* ACP dispatch does not gate on allowArgs */
        slash_rt_t rt = {a, output, ud, agent_invoked};
        res->kind = SLASH_CONSUMED;
        c->handle(&p, &rt, res);
    }
    slash_parsed_free(&p);
}

bool slash_invokes_agent(app_t *a, const char *name) {
    return str_eq(name, "retry") && app_retry_available(a);
}

void slash_result_free(slash_result_t *res) {
    free(res->prompt);
    memset(res, 0, sizeof *res);
}

static void append_command(json_t *arr, strvec_t *seen, const char *name, const char *desc, const char *hint,
                           const char *source) {
    if (!name || strvec_contains(seen, name)) return;
    strvec_push(seen, name);
    json_t *o = json_obj();
    json_set_str(o, "name", name);
    json_set_str(o, "description", desc ? desc : "");
    if (hint) {
        json_t *in = json_obj();
        json_set_str(in, "hint", hint);
        json_set(o, "input", in);
    }
    json_set_str(o, "source", source);
    json_push(arr, o);
}

json_t *slash_available_commands(app_t *a) {
    (void)a;
    json_t *arr = json_arr();
    for (size_t i = 0; i < slash_count(); i++) {
        const slash_cmd_t *c = g_reg.v[i];
        if (!c->handle) continue;
        json_t *o = json_obj();
        json_set_str(o, "name", c->name);
        if (c->aliases.n) {
            json_t *al = json_arr();
            for (size_t k = 0; k < c->aliases.n; k++) json_push_str(al, c->aliases.v[k]);
            json_set(o, "aliases", al);
        }
        char *d = slash_format_description(c->acp_description ? c->acp_description : c->description, NULL);
        json_set_str(o, "description", d);
        free(d);
        const char *hint = c->acp_input_hint ? c->acp_input_hint : c->inline_hint;
        if (hint) {
            json_t *in = json_obj();
            json_set_str(in, "hint", hint);
            json_set(o, "input", in);
        }
        if (c->nsubs) {
            json_t *subs = json_arr();
            for (size_t k = 0; k < c->nsubs; k++) {
                json_t *s = json_obj();
                json_set_str(s, "name", c->subs[k].name);
                if (c->subs[k].description) json_set_str(s, "description", c->subs[k].description);
                if (c->subs[k].usage) json_set_str(s, "usage", c->subs[k].usage);
                json_push(subs, s);
            }
            json_set(o, "subcommands", subs);
        }
        json_set_str(o, "source", "builtin");
        json_push(arr, o);
    }
    if (!a) return arr;
    /* appendCommand: first name wins; builtin aliases are reserved too */
    strvec_t seen = {0};
    for (size_t i = 0; i < slash_count(); i++) {
        const slash_cmd_t *c = g_reg.v[i];
        if (!c->handle) continue;
        strvec_push(&seen, c->name);
        for (size_t k = 0; k < c->aliases.n; k++) strvec_push(&seen, c->aliases.v[k]);
    }
#define APPEND(NAME, DESC, HINT, SOURCE) append_command(arr, &seen, NAME, DESC, HINT, SOURCE)
    if (settings_bool("skills.enableSkillCommands", true)) {
        for (size_t i = 0; i < a->disc.skills.n; i++) {
            const skill_t *s = &a->disc.skills.v[i];
            char *name = xasprintf("skill:%s", s->name);
            char *desc = s->description && *s->description ? xstrdup(s->description) : xasprintf("Run %s skill", s->name);
            APPEND(name, desc, "arguments", "skill");
            free(desc);
            free(name);
        }
    }
    if (a->mcp) {
        json_t *mc = mcp_prompt_commands(a->mcp), *c;
        json_arr_foreach(mc, i, c) APPEND(json_get_str(c, "name"), json_get_str(c, "description"), "arguments", "mcp_prompt");
        json_free(mc);
    }
    for (size_t i = 0; i < a->file_cmds.n; i++) {
        const file_cmd_t *f = &a->file_cmds.v[i];
        APPEND(f->name, f->description, f->argument_hint, "file");
    }
#undef APPEND
    strvec_free(&seen);
    return arr;
}

json_t *slash_acp_available_commands(app_t *a) {
    json_t *all = slash_available_commands(a), *arr = json_arr(), *c;
    json_arr_foreach(all, i, c) {
        json_t *o = json_obj();
        json_set_str(o, "name", json_get_str(c, "name"));
        json_set_str(o, "description", json_get_str(c, "description") ? json_get_str(c, "description") : "");
        if (json_get(c, "input")) json_set(o, "input", json_clone(json_get(c, "input")));
        json_push(arr, o);
    }
    json_free(all);
    return arr;
}
