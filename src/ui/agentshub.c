/* agentshub.c - the /agents hub (port of omp overlays/agents-hub.ts on overlays/hub-frame.ts, with the host
 * callbacks of modes/agents-hub-deps.ts), text mode: a fullscreen split frame titled "Agents" with a sidebar
 * (⬢ All agents n, ● Project / User / Bundled counts, + New agent) and a body listing agent definitions with
 * type-to-filter search and a pinned detail block (description, effective model → resolved, prewalk, advisor,
 * file). Enter opens the agent strip (enable/disable, model, prewalk, advisor); a property opens its value strip
 * (agent default / on / off / pick model… / pattern… / clear override). "pick model…" dives into the model
 * browser; "pattern…" opens an inline input. Space toggles an agent, Ctrl+R reloads. "+ New agent" drafts a
 * definition with the agent-creation architect prompt on the session model and saves it to the project or user
 * agents directory. Settings: task.disabledAgents, task.agentModelOverrides, task.agentPrewalk, task.agentAdvisor
 * (global config.yml). */
#include "ui/modelbrowser.h"
#include "ui/overlays.h"
#include "ui/tools/tc.h"
#include "ui/ui.h"

#include "config/model_resolver.h"
#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"
#include "tools/tools.h"
#include "tui/color.h"
#include "tui/fuzzy.h"

#include <ctype.h>
#include <pthread.h>

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

enum { P_MODEL, P_PREWALK, P_ADVISOR };
static const char *const PROP[] = {"model", "prewalk", "advisor"};
enum { SRC_PROJECT, SRC_USER, SRC_BUNDLED };
static const char *const SRC_LABEL[] = {"Project", "User", "Bundled"};
static const char *const SRC_TAG[] = {"project", "user", "bundled"};

typedef struct {
    char *name, *description, *file_path, *model; /* model: the definition's comma pattern list or NULL */
    json_t *prewalk, *advisor;                    /* frontmatter: bool / string / NULL (owned clones) */
    int source;
    bool disabled;
    char *ovr[3]; /* task.agentModelOverrides / agentPrewalk / agentAdvisor entries */
} hagent_t;

enum { E_ALL, E_SOURCE, E_NEW, E_SEP };
typedef struct {
    int kind, source;
    char *label, *annotation;
} hentry_t;

enum { ST_NONE, ST_CHIPS, ST_PATTERN };
enum { A_TOGGLE, A_PROPERTY, A_SET, A_PICK, A_PATTERN };
typedef struct {
    char *styled;
    int action, property;
    char *value; /* A_SET (NULL clears) */
} chip_t;

typedef struct {
    int start, end, index;
} crange_t;

typedef struct agents_hub {
    tui_component_t base;
    ui_t *u;
    VEC(hagent_t) agents;
    VEC(hentry_t) entries;
    size_t active;   /* entry index */
    bool focus_list; /* "list" vs "scope" */
    VEC(int) rows;   /* agent indices; -1 = "+ New agent…" */
    int row_index, row_hover, list_scroll, list_row_start, sidebar_scroll;
    sb_t search;
    char *notice, *load_error;
    /* strips */
    int strip, strip_agent, strip_property; /* strip_property -1: the agent strip */
    VEC(chip_t) chips;
    size_t chip_index;
    VEC(crange_t) ranges;
    sb_t pattern;
    /* model browser assignment */
    ui_model_browser_t *b;
    int assign_agent, assign_property; /* assign_agent -1: not assigning */
    /* create flow */
    bool create_input, generating;
    sb_t create_text;
    int create_scope; /* SRC_PROJECT / SRC_USER */
    char *spec_id, *spec_when, *spec_prompt, *create_error;
    sb_t streaming;
    pthread_t gen_th;
    bool gen_live;
    volatile sig_atomic_t gen_cancel;
    int content_rows, sidebar_w, body_w;
} hub_t;

/* ---------- data (agents-hub-deps.ts) ---------- */

static void hagent_free(hagent_t *a) {
    free(a->name), free(a->description), free(a->file_path), free(a->model);
    json_free(a->prewalk), json_free(a->advisor);
    for (int i = 0; i < 3; i++) free(a->ovr[i]);
}

static const char *const OVR_KEYS[] = {"agentModelOverrides", "agentPrewalk", "agentAdvisor"};

static char *override_of(int prop, const char *name) {
    json_t *v = json_get(json_get_obj(settings_get("task"), OVR_KEYS[prop]), name);
    if (json_is_arr(v)) {
        strvec_t s = {0};
        json_t *x;
        json_arr_foreach(v, i, x) if (json_is_str(x)) strvec_push(&s, json_sv(x));
        char *j = strvec_join(&s, ",");
        strvec_free(&s);
        v = NULL;
        char *t = str_trim(j);
        free(j);
        if (*t) return t;
        free(t);
        return NULL;
    }
    if (!json_is_str(v)) return NULL;
    char *t = str_trim(json_sv(v));
    if (*t) return t;
    free(t);
    return NULL;
}

static int cmp_agent(const void *x, const void *y) {
    const hagent_t *a = x, *b = y;
    if (a->source != b->source) return a->source - b->source;
    return strcasecmp(a->name, b->name);
}

static void load_agents(hub_t *h) {
    for (size_t i = 0; i < h->agents.n; i++) hagent_free(&h->agents.v[i]);
    h->agents.n = 0;
    json_t *defs = agent_defs_load(h->u->app->cwd), *dis = settings_get("task.disabledAgents"), *v;
    const char *k;
    json_obj_foreach(defs, i, k, v) {
        hagent_t a = {0};
        a.name = xstrdup(k);
        a.description = xstrdup(json_get_str(v, "description") ? json_get_str(v, "description") : "");
        const char *src = json_get_str(v, "source");
        if (!src || str_eq(src, "bundled")) {
            a.source = SRC_BUNDLED;
            a.file_path = xasprintf("embedded:%s.md", k);
        } else {
            a.source = str_starts(src, h->u->app->cwd) ? SRC_PROJECT : SRC_USER;
            a.file_path = xstrdup(src);
        }
        if (json_get_str(v, "model")) a.model = xstrdup(json_get_str(v, "model"));
        if (json_get(v, "prewalk")) a.prewalk = json_clone(json_get(v, "prewalk"));
        if (json_get(v, "advisor")) a.advisor = json_clone(json_get(v, "advisor"));
        json_t *d;
        json_arr_foreach(dis, di, d) if (str_eq(json_sv(d), k)) a.disabled = true;
        for (int p = 0; p < 3; p++) a.ovr[p] = override_of(p, k);
        vec_push(&h->agents, a);
    }
    json_free(defs);
    if (h->agents.n > 1) xqsort(h->agents.v, h->agents.n, sizeof *h->agents.v, cmp_agent);
}

/* resolveAgentModelPatterns (no request model): the settings override, the agent's own model (bar
 * session-inheriting aliases, `@task` excepted), else the session's active / default model */
static strvec_t effective_model_patterns(hub_t *h, const hagent_t *a) {
    strvec_t pats = mr_configured_patterns(a->ovr[P_MODEL]);
    if (pats.n) return pats;
    strvec_free(&pats);
    if (a->model) {
        strvec_t single = mr_pattern_list(a->model);
        const char *one = single.n == 1 ? single.v[0] : NULL;
        bool inherits = one && (!strcmp(one, "default") || !strcmp(one, "@default") || !strcmp(one, "*") ||
                                !strcmp(one, "pi/default") || !strcmp(one, "@task") || !strcmp(one, "pi/task"));
        bool task_alias = one && (!strcmp(one, "@task") || !strcmp(one, "pi/task"));
        strvec_t cp = mr_configured_patterns(a->model);
        strvec_free(&single);
        if (cp.n && (task_alias || !inherits)) return cp;
        strvec_free(&cp);
    }
    char *active = h->u->app->model ? app_model_label(h->u->app) : NULL;
    const char *def = settings_str("modelRoles.default", NULL);
    const char *fb = active && *active ? active : def ? def : "";
    pats = mr_configured_patterns(fb);
    free(active);
    return pats;
}

/* resolvePatterns: "provider/id[:level]" of the first pattern that resolves, or NULL */
static char *resolve_patterns(const strvec_t *pats) {
    if (!pats->n) return NULL;
    mr_result_t r;
    if (!mr_resolve_override(pats, &r) || !r.model) return NULL;
    char *k = mr_model_key(r.model);
    char *s = r.explicit_level && r.level ? xasprintf("%s:%s", k, r.level) : xstrdup(k);
    free(k);
    mr_result_clear(&r);
    return s;
}

static const char *flag_pattern(const json_t *v) { return json_is_str(v) && *json_sv(v) ? json_sv(v) : NULL; }

/* resolveAgentPrewalkPattern over resolveAgentPrewalkDefault (task.prewalk arms the bundled `task`) */
static char *effective_prewalk(const hagent_t *a) {
    const json_t *pw = a->prewalk;
    bool def_true = json_is_bool(pw) ? pw->u.b
                                     : !pw && settings_bool("task.prewalk", false) && a->source == SRC_BUNDLED && str_eq(a->name, "task");
    const char *agent_pattern = flag_pattern(pw);
    const char *o = a->ovr[P_PREWALK];
    if (o) {
        if (!strcasecmp(o, "off") || !strcasecmp(o, "false")) return NULL;
        if (!strcasecmp(o, "on") || !strcasecmp(o, "true")) return xstrdup(agent_pattern ? agent_pattern : "@smol");
        return xstrdup(o);
    }
    if (def_true) return xstrdup("@smol");
    return agent_pattern ? xstrdup(agent_pattern) : NULL;
}

/* resolveAgentAdvisorSelection: selection.model ?? "@advisor" */
static char *effective_advisor(const hagent_t *a) {
    const char *agent_pattern = flag_pattern(a->advisor);
    const char *o = a->ovr[P_ADVISOR];
    if (o) {
        if (!strcasecmp(o, "off") || !strcasecmp(o, "false")) return NULL;
        if (!strcasecmp(o, "on") || !strcasecmp(o, "true")) return xstrdup(agent_pattern ? agent_pattern : "@advisor");
        return xstrdup(o);
    }
    if (json_is_bool(a->advisor) && a->advisor->u.b) return xstrdup("@advisor");
    return agent_pattern ? xstrdup(agent_pattern) : NULL;
}

static void set_disabled(const char *name, bool disabled) {
    json_t *g = json_get(json_get_obj(settings_global(), "task"), "disabledAgents"), *x;
    json_t *arr = json_arr();
    bool present = false;
    json_arr_foreach(g, i, x) {
        if (!json_is_str(x)) continue;
        if (str_eq(json_sv(x), name)) {
            present = true;
            if (!disabled) continue;
        }
        json_push_str(arr, json_sv(x));
    }
    if (disabled && !present) json_push_str(arr, name);
    settings_set_global("task.disabledAgents", arr);
}

/* ---------- sidebar and rows ---------- */

static void entries_free(hub_t *h) {
    for (size_t i = 0; i < h->entries.n; i++) free(h->entries.v[i].label), free(h->entries.v[i].annotation);
    h->entries.n = 0;
}

static void push_entry(hub_t *h, int kind, int source, const char *label, char *ann) {
    hentry_t e = {kind, source, xstrdup(label), ann};
    vec_push(&h->entries, e);
}

static void build_sidebar(hub_t *h) {
    int old_kind = h->entries.n ? h->entries.v[h->active].kind : E_ALL;
    int old_src = h->entries.n ? h->entries.v[h->active].source : -1;
    entries_free(h);
    int counts[3] = {0};
    for (size_t i = 0; i < h->agents.n; i++) counts[h->agents.v[i].source]++;
    push_entry(h, E_ALL, -1, "All agents", xasprintf("%zu", h->agents.n));
    bool any = counts[0] || counts[1] || counts[2];
    if (any) {
        push_entry(h, E_SEP, -1, "", NULL);
        for (int s = 0; s < 3; s++)
            if (counts[s]) push_entry(h, E_SOURCE, s, SRC_LABEL[s], xasprintf("%d", counts[s]));
    }
    push_entry(h, E_SEP, -1, "", NULL);
    push_entry(h, E_NEW, -1, "New agent", NULL);
    h->active = 0;
    for (size_t i = 0; i < h->entries.n; i++)
        if (h->entries.v[i].kind == old_kind && h->entries.v[i].source == old_src) h->active = i;
}

static void build_rows(hub_t *h) {
    h->rows.n = 0;
    const hentry_t *e = &h->entries.v[h->active];
    const char *q = sb_cstr(&h->search);
    tui_fuzzy_query_t *fq = *q ? tui_fuzzy_query_new(q) : NULL;
    for (size_t i = 0; i < h->agents.n; i++) {
        const hagent_t *a = &h->agents.v[i];
        if (e->kind == E_SOURCE && a->source != e->source) continue;
        if (fq) {
            char *text = xasprintf("%s %s %s %s", a->name, a->description, SRC_LABEL[a->source], a->ovr[P_MODEL] ? a->ovr[P_MODEL] : "");
            bool ok = tui_fuzzy_query_match(fq, text, NULL);
            free(text);
            if (!ok) continue;
        }
        vec_push(&h->rows, (int)i);
    }
    if (fq) tui_fuzzy_query_free(fq);
    vec_push(&h->rows, -1);
}

static void clamp_row(hub_t *h) { h->row_index = MAX(0, MIN(h->row_index, (int)h->rows.n - 1)); }

static hagent_t *selected_agent(hub_t *h) {
    if (h->row_index < 0 || h->row_index >= (int)h->rows.n) return NULL;
    int ai = h->rows.v[h->row_index];
    return ai >= 0 ? &h->agents.v[ai] : NULL;
}

static void reload(hub_t *h) {
    hagent_t *sel = selected_agent(h);
    char *name = sel ? xstrdup(sel->name) : NULL;
    free(h->load_error);
    h->load_error = NULL;
    load_agents(h);
    build_sidebar(h);
    build_rows(h);
    if (name)
        for (size_t i = 0; i < h->rows.n; i++)
            if (h->rows.v[i] >= 0 && str_eq(h->agents.v[h->rows.v[i]].name, name)) h->row_index = (int)i;
    free(name);
    clamp_row(h);
}

/* ---------- mutations ---------- */

static void set_notice(hub_t *h, char *owned) {
    free(h->notice);
    h->notice = owned;
}

static void toggle_agent(hub_t *h, hagent_t *a) {
    a->disabled = !a->disabled;
    set_disabled(a->name, a->disabled);
    set_notice(h, xasprintf("%s %s", a->name, a->disabled ? "disabled" : "enabled"));
}

static char *describe_property(hub_t *h, const hagent_t *a, int prop) {
    if (prop == P_MODEL) {
        strvec_t pats = effective_model_patterns(h, a);
        char *resolved = resolve_patterns(&pats);
        char *joined = pats.n ? strvec_join(&pats, ",") : NULL;
        const char *base = a->ovr[P_MODEL] ? a->ovr[P_MODEL] : joined ? joined : "session model";
        char *r = xasprintf("%s model: %s%s%s", a->name, base, resolved ? " \xe2\x86\x92 " : "", resolved ? resolved : "");
        free(resolved);
        free(joined);
        strvec_free(&pats);
        return r;
    }
    char *p = prop == P_PREWALK ? effective_prewalk(a) : effective_advisor(a);
    char *r = p ? xasprintf("%s %s: on (%s)", a->name, PROP[prop], p) : xasprintf("%s %s: off", a->name, PROP[prop]);
    free(p);
    return r;
}

static void set_override(hub_t *h, hagent_t *a, int prop, const char *value) {
    char *t = value ? str_trim(value) : NULL;
    if (t && !*t) {
        free(t);
        t = NULL;
    }
    free(a->ovr[prop]);
    a->ovr[prop] = t ? xstrdup(t) : NULL;
    settings_set_global_entry(prop == P_MODEL ? "task.agentModelOverrides" : prop == P_PREWALK ? "task.agentPrewalk" : "task.agentAdvisor", a->name,
                              t ? json_str(t) : NULL);
    free(t);
    set_notice(h, describe_property(h, a, prop));
}

/* ---------- strips ---------- */

static void chips_free(hub_t *h) {
    for (size_t i = 0; i < h->chips.n; i++) free(h->chips.v[i].styled), free(h->chips.v[i].value);
    h->chips.n = 0;
}

static void push_chip(hub_t *h, char *styled, int action, int prop, const char *value) {
    chip_t c = {styled, action, prop, value ? xstrdup(value) : NULL};
    vec_push(&h->chips, c);
}

static char *property_summary(hub_t *h, const hagent_t *a, int prop) {
    UNUSED(h);
    if (prop == P_MODEL) return xstrdup(a->ovr[P_MODEL] ? a->ovr[P_MODEL] : "auto");
    char *p = prop == P_PREWALK ? effective_prewalk(a) : effective_advisor(a);
    return p ? p : xstrdup("off");
}

static void open_agent_strip(hub_t *h, int ai) {
    chips_free(h);
    hagent_t *a = &h->agents.v[ai];
    char *t = a->disabled ? xasprintf("%s enable", tui_sym("status.enabled")) : xasprintf("%s disable", tui_sym("status.disabled"));
    push_chip(h, S(a->disabled ? TUI_TC_SUCCESS : TUI_TC_DIM, t), A_TOGGLE, 0, NULL);
    free(t);
    for (int p = 0; p < 3; p++) {
        char *sum = property_summary(h, a, p), *ds = xasprintf(": %s", sum);
        char *x = S(TUI_TC_ACCENT, PROP[p]), *y = S(TUI_TC_DIM, ds);
        push_chip(h, xasprintf("%s%s", x, y), A_PROPERTY, p, NULL);
        free(sum), free(ds), free(x), free(y);
    }
    h->strip = ST_CHIPS;
    h->strip_agent = ai;
    h->strip_property = -1;
    h->chip_index = 1;
}

static char *mark(const char *label, bool active) {
    if (!active) return S(TUI_TC_MUTED, label);
    char *t = xasprintf("%s %s", tui_sym("status.enabled"), label), *r = S(TUI_TC_ACCENT, t);
    free(t);
    return r;
}

static void open_property_strip(hub_t *h, int ai, int prop) {
    chips_free(h);
    hagent_t *a = &h->agents.v[ai];
    char *cur = a->ovr[prop] ? str_lower(a->ovr[prop]) : NULL;
    if (prop == P_MODEL) {
        push_chip(h, S(TUI_TC_ACCENT, "pick model\xe2\x80\xa6"), A_PICK, prop, NULL);
        push_chip(h, S(TUI_TC_MUTED, "pattern\xe2\x80\xa6"), A_PATTERN, prop, NULL);
        if (a->ovr[P_MODEL]) push_chip(h, S(TUI_TC_WARNING, "clear override"), A_SET, prop, NULL);
    } else {
        push_chip(h, mark("agent default", !cur), A_SET, prop, NULL);
        push_chip(h, mark("on", str_eq(cur, "on")), A_SET, prop, "on");
        push_chip(h, mark("off", str_eq(cur, "off")), A_SET, prop, "off");
        push_chip(h, S(TUI_TC_ACCENT, "pick model\xe2\x80\xa6"), A_PICK, prop, NULL);
        push_chip(h, S(TUI_TC_MUTED, "pattern\xe2\x80\xa6"), A_PATTERN, prop, NULL);
    }
    free(cur);
    h->strip = ST_CHIPS;
    h->strip_agent = ai;
    h->strip_property = prop;
    h->chip_index = 0;
}

static void open_pattern_strip(hub_t *h, int ai, int prop) {
    sb_clear(&h->pattern);
    if (h->agents.v[ai].ovr[prop]) sb_append(&h->pattern, h->agents.v[ai].ovr[prop]);
    h->strip = ST_PATTERN;
    h->strip_agent = ai;
    h->strip_property = prop;
}

static void close_strip(hub_t *h) {
    h->strip = ST_NONE;
    chips_free(h);
    h->ranges.n = 0;
}

static void start_assign(hub_t *h, int ai, int prop) {
    ui_model_browser_free(h->b);
    /* buildBrowserItems(getAvailableModels()) in sortModelItems order (MRU); the agents hub shows no role chips */
    h->b = ui_session_model_browser();
    for (size_t i = 0; i < h->b->nroles; i++) free(h->b->roles[i].role), free(h->b->roles[i].label);
    h->b->nroles = 0;
    h->b->show_provider = true;
    h->b->empty_text = xstrdup("  No models available \xe2\x80\x94 configure a provider in /models first.");
    ui_model_browser_done_adding(h->b);
    h->assign_agent = ai;
    h->assign_property = prop;
    if (h->agents.v[ai].ovr[prop]) ui_model_browser_select(h->b, h->agents.v[ai].ovr[prop]);
}

static void cancel_assign(hub_t *h) {
    h->assign_agent = -1;
    ui_model_browser_free(h->b);
    h->b = NULL;
}

static void activate_chip(hub_t *h) {
    if (h->strip != ST_CHIPS || h->chip_index >= h->chips.n) return;
    chip_t c = h->chips.v[h->chip_index];
    int ai = h->strip_agent;
    switch (c.action) {
    case A_TOGGLE:
        toggle_agent(h, &h->agents.v[ai]);
        close_strip(h);
        return;
    case A_PROPERTY: open_property_strip(h, ai, c.property); return;
    case A_SET: {
        char *v = c.value ? xstrdup(c.value) : NULL;
        int p = c.property;
        close_strip(h);
        set_override(h, &h->agents.v[ai], p, v);
        free(v);
        return;
    }
    case A_PICK: {
        int p = c.property;
        close_strip(h);
        start_assign(h, ai, p);
        return;
    }
    case A_PATTERN: open_pattern_strip(h, ai, c.property); return;
    }
}

/* Esc: a property strip steps back to the agent strip, the pattern input to its property strip */
static void strip_back(hub_t *h) {
    if (h->strip == ST_CHIPS && h->strip_property >= 0) open_agent_strip(h, h->strip_agent);
    else if (h->strip == ST_PATTERN) open_property_strip(h, h->strip_agent, h->strip_property);
    else close_strip(h);
}

/* ---------- create flow ---------- */

static bool create_active(const hub_t *h) { return h->create_input || h->generating || h->spec_id; }

static void clear_spec(hub_t *h) {
    free(h->spec_id), free(h->spec_when), free(h->spec_prompt);
    h->spec_id = h->spec_when = h->spec_prompt = NULL;
}

static void begin_create(hub_t *h) {
    if (h->generating) return;
    free(h->create_error);
    h->create_error = NULL;
    clear_spec(h);
    sb_clear(&h->create_text);
    h->create_input = true;
}

static void clear_create(hub_t *h) {
    h->create_input = false;
    h->generating = false;
    sb_clear(&h->create_text);
    clear_spec(h);
    free(h->create_error);
    h->create_error = NULL;
    sb_clear(&h->streaming);
}

/* parseGeneratedAgentSpec: a bare JSON object (tried first) or one inside a code fence */
static char *parse_spec(hub_t *h, const char *raw) {
    json_t *j = json_parse_str(raw);
    if (!j) {
        const char *f = strstr(raw, "```");
        char *inner = NULL;
        if (f) {
            const char *s = f + 3;
            if (!strncasecmp(s, "json", 4)) s += 4;
            const char *e = strstr(s, "```");
            if (e) inner = xstrndup(s, (size_t)(e - s));
        }
        if (!inner) {
            const char *a = strchr(raw, '{'), *b = strrchr(raw, '}');
            inner = a && b && b >= a ? xstrndup(a, (size_t)(b - a + 1)) : xstrdup(raw);
        }
        char *t = str_trim(inner);
        free(inner);
        j = json_parse_str(t);
        if (!j) { /* JSON.parse's (JavaScriptCore) message for the extracted text */
            const char *p = t;
            while (*p && isspace((unsigned char)*p)) p++;
            size_t n = 0;
            while (p[n] && (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '$')) n++;
            char *e = !*p                              ? xstrdup("JSON Parse error: Unexpected EOF")
                      : n && !isdigit((unsigned char)*p) ? xasprintf("JSON Parse error: Unexpected identifier \"%.*s\"", (int)n, p)
                                                         : xasprintf("JSON Parse error: Unrecognized token '%c'", *p);
            free(t);
            return e;
        }
        free(t);
    }
    if (!json_is_obj(j)) {
        json_free(j);
        return xstrdup("Model output is not a JSON object");
    }
    const char *id = json_get_str(j, "identifier"), *when = json_get_str(j, "whenToUse"), *sp = json_get_str(j, "systemPrompt");
    if (!id || !when || !sp) {
        json_free(j);
        return xstrdup("Model output is missing required fields (identifier, whenToUse, systemPrompt)");
    }
    char *ti = str_trim(id), *tw = str_trim(when), *ts = str_trim(sp);
    json_free(j);
    /* ^[a-z0-9]+(?:-[a-z0-9]+){1,5}$ */
    int words = 1;
    bool ok = *ti && ti[0] != '-' && ti[strlen(ti) - 1] != '-';
    for (const char *p = ti; ok && *p; p++) {
        if (*p == '-') {
            if (p[1] == '-') ok = false;
            words++;
        } else if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p)) ok = false;
    }
    char *err = NULL;
    if (!ok || words < 2 || words > 6) err = xstrdup("Generated identifier is invalid (must be lowercase kebab-case, 2+ words)");
    else if (strncasecmp(tw, "use this agent when", 19)) err = xstrdup("Generated whenToUse must start with 'Use this agent when...'");
    else if (!*ts) err = xstrdup("Generated systemPrompt is empty");
    if (err) {
        free(ti), free(tw), free(ts);
        return err;
    }
    h->spec_id = ti;
    h->spec_when = tw;
    h->spec_prompt = ts;
    return NULL;
}

typedef struct {
    hub_t *h;
    char *delta;
    char *raw, *error;
    bool done;
} gen_msg_t;

static void gen_apply(void *p) {
    gen_msg_t *m = p;
    hub_t *h = m->h;
    if (m->delta) sb_append(&h->streaming, m->delta);
    if (m->done) {
        if (h->gen_live) pthread_join(h->gen_th, NULL);
        h->gen_live = false;
        h->generating = false;
        if (m->error) h->create_error = xstrdup(m->error);
        else {
            char *e = parse_spec(h, m->raw ? m->raw : "");
            if (e) h->create_error = e;
            else {
                free(h->notice);
                h->notice = NULL;
            }
        }
    }
    tui_component_request_render(&h->base);
    free(m->delta), free(m->raw), free(m->error);
    free(m);
}

static void gen_post(hub_t *h, char *delta, char *raw, char *error, bool done) {
    gen_msg_t *m = xcalloc(1, sizeof *m);
    m->h = h;
    m->delta = delta;
    m->raw = raw;
    m->error = error;
    m->done = done;
    tui_post(h->u->tui, gen_apply, m);
}

static void gen_event(void *ud, const ai_event_t *ev) {
    hub_t *h = ud;
    if (ev->type == AI_EV_TEXT_DELTA && ev->delta_len) gen_post(h, xstrndup(ev->delta, ev->delta_len), NULL, NULL, false);
}

typedef struct {
    hub_t *h;
    char *request;
} gen_job_t;

/* generateAgent: a tool-less turn with the agent-creation architect system prompt on the session model */
static void *gen_main(void *p) {
    gen_job_t *g = p;
    hub_t *h = g->h;
    app_t *a = h->u->app;
    const ai_model_t *m = a->model;
    if (!m) {
        gen_post(h, NULL, NULL, xstrdup("No available model to generate agent specification."), true);
        free(g->request);
        free(g);
        return NULL;
    }
    char *sys = prompt_render("coding-agent/prompts/system/agent-creation-architect.md", NULL);
    json_t *uc = json_obj();
    json_set_str(uc, "request", g->request);
    char *user = prompt_render("coding-agent/prompts/system/agent-creation-user.md", uc);
    json_free(uc);
    json_t *sysv = json_arr(), *msgs = json_arr();
    json_push_str(sysv, sys ? sys : "");
    json_push(msgs, msg_user_text(user ? user : ""));
    json_t *wire = ai_transform_messages(msgs, m);
    ai_context_t ctx = {.system_prompt = sysv, .messages = wire};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    char *key = app_api_key_for(m);
    o.api_key = key;
    o.cancel = &h->gen_cancel;
    json_t *r = ai_stream(m, &ctx, &o, gen_event, h);
    const char *stop = json_get_str(r, "stopReason");
    char *text = ai_message_text(r), *t = text ? str_trim(text) : xstrdup("");
    if (str_eq(stop, "error") || str_eq(stop, "aborted"))
        gen_post(h, NULL, NULL, xstrdup(json_get_str(r, "errorMessage") ? json_get_str(r, "errorMessage") : "Request was aborted"), true);
    else if (!*t) gen_post(h, NULL, NULL, xstrdup("No response returned by agent creation architect."), true);
    else gen_post(h, NULL, xstrdup(t), NULL, true);
    free(text), free(t), free(key), free(sys), free(user);
    json_free(r), json_free(wire), json_free(msgs), json_free(sysv);
    free(g->request);
    free(g);
    return NULL;
}

static void generate(hub_t *h) {
    char *d = str_trim(sb_cstr(&h->create_text));
    sb_clear(&h->create_text);
    sb_append(&h->create_text, d);
    if (!*d) {
        free(h->create_error);
        h->create_error = xstrdup("Description is required.");
        free(d);
        return;
    }
    free(h->create_error);
    h->create_error = NULL;
    clear_spec(h);
    sb_clear(&h->streaming);
    gen_job_t *g = xcalloc(1, sizeof *g);
    g->h = h;
    g->request = d;
    h->generating = true;
    h->gen_cancel = 0;
    if (pthread_create(&h->gen_th, NULL, gen_main, g) != 0) {
        h->generating = false;
        h->create_error = xstrdup("failed to start generation");
        free(g->request);
        free(g);
    } else h->gen_live = true;
}

/* YAML.stringify of a plain scalar: quoted when a plain scalar would not round-trip */
static char *yaml_scalar(const char *s) {
    bool plain = *s && !isspace((unsigned char)s[0]) && !isspace((unsigned char)s[strlen(s) - 1]) && !strchr("-?:,[]{}#&*!|>'\"%@`", s[0]) &&
                 !strstr(s, ": ") && !strstr(s, " #") && !strchr(s, '\n') && !str_eq(s, "true") && !str_eq(s, "false") && !str_eq(s, "null");
    if (plain) return xstrdup(s);
    json_t *j = json_str(s);
    char *r = json_dump(j, 0);
    json_free(j);
    return r;
}

static void save_spec(hub_t *h) {
    if (!h->spec_id) return;
    char *dir;
    if (h->create_scope == SRC_USER) dir = omc_agent_path("agents");
    else {
        const char *pd = settings_project_dir();
        dir = pd ? path_join(pd, "agents") : path_join(h->u->app->cwd, ".omc/agents");
    }
    char *name = xasprintf("%s.md", h->spec_id), *file = path_join(dir, name);
    free(name);
    if (fs_exists(file)) {
        char *sp = tc_shorten_path(file);
        free(h->create_error);
        h->create_error = xasprintf("Agent file already exists: %s", sp);
        free(sp), free(file), free(dir);
        return;
    }
    fs_mkdir_p(dir, 0755);
    char *n = yaml_scalar(h->spec_id), *w = yaml_scalar(h->spec_when);
    char *body = xasprintf("---\nname: %s\ndescription: %s\n---\n\n%s\n", n, w, h->spec_prompt);
    int rc = fs_write(file, body, strlen(body));
    free(n), free(w), free(body);
    if (rc != 0) {
        free(h->create_error);
        h->create_error = xasprintf("Failed to write %s", file);
        free(file), free(dir);
        return;
    }
    char *sp = tc_shorten_path(file), *id = xstrdup(h->spec_id);
    clear_create(h);
    set_notice(h, xasprintf("Created agent %s at %s", id, sp));
    free(sp), free(id), free(file), free(dir);
    reload(h);
}

/* ---------- rendering ---------- */

/* Input.render: prompt + text with an inverse cursor cell, padded to width */
static char *render_input(const char *prompt, const char *text, int width) {
    int avail = MAX(1, width - tui_visible_width(prompt));
    int tw = tui_visible_width(text);
    char *vis = tw + 1 > avail ? tui_slice_cols(text, tw + 1 - avail, avail - 1) : xstrdup(text);
    int vw = tui_visible_width(vis) + 1;
    char *r = xasprintf("%s%s\x1b[7m \x1b[27m%*s", prompt, vis, MAX(0, avail - vw), "");
    free(vis);
    return r;
}

static int sidebar_width(const hub_t *h) {
    int longest = 0;
    for (size_t i = 0; i < h->entries.n; i++)
        longest = MAX(longest, tui_visible_width(h->entries.v[i].label) +
                                   (h->entries.v[i].annotation ? tui_visible_width(h->entries.v[i].annotation) : 0) + 5);
    return MAX(16, MIN(24, longest));
}

static void render_sidebar(hub_t *h, int width, int rows, strvec_t *out) {
    int a = (int)h->active;
    if (a < h->sidebar_scroll) h->sidebar_scroll = a;
    else if (a >= h->sidebar_scroll + rows) h->sidebar_scroll = a - rows + 1;
    for (int i = h->sidebar_scroll; i < MIN((int)h->entries.n, h->sidebar_scroll + rows); i++) {
        const hentry_t *e = &h->entries.v[i];
        if (e->kind == E_SEP) {
            sb_t r = SB_INIT;
            sb_repeat(&r, "\xe2\x94\x80", width);
            strvec_push_own(out, S(TUI_TC_BORDER, sb_cstr(&r)));
            sb_free(&r);
            continue;
        }
        bool active = i == a;
        char *cursor = active && !h->focus_list ? S(TUI_TC_ACCENT, tui_sym("nav.cursor")) : xstrdup(" ");
        const char *isym = e->kind == E_ALL ? tui_sym("icon.model") : e->kind == E_NEW ? "+" : tui_sym("status.enabled");
        char *icon = S(e->kind == E_NEW ? TUI_TC_DIM : TUI_TC_ACCENT, isym);
        char *label;
        if (active) {
            char *x = S(TUI_TC_ACCENT, e->label);
            label = tui_bold(x);
            free(x);
        } else label = xstrdup(e->label);
        char *left = xasprintf("%s %s %s", cursor, icon, label), *ann = S(TUI_TC_DIM, e->annotation ? e->annotation : "");
        int lw = tui_visible_width(left), aw = tui_visible_width(ann);
        char *line = lw + aw + 1 <= width ? xasprintf("%s%*s%s", left, width - lw - aw, "", ann) : tui_truncate_to_width(left, width, NULL);
        strvec_push_own(out, line);
        free(cursor), free(icon), free(label), free(left), free(ann);
    }
}

static void push_trunc(strvec_t *out, char *owned, int width) {
    strvec_push_own(out, tui_truncate_to_width(owned, width, NULL));
    free(owned);
}

static void status_row(hub_t *h, int width, strvec_t *out) {
    char *t;
    if (h->load_error) {
        char *x = xasprintf(" %s", h->load_error);
        t = S(TUI_TC_ERROR, x);
        free(x);
    } else if (h->assign_agent >= 0) {
        const hagent_t *a = &h->agents.v[h->assign_agent];
        char *what = h->assign_property == P_MODEL ? xstrdup("model override") : xasprintf("%s model", PROP[h->assign_property]);
        char *bn = tui_bold(a->name), *enter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel");
        char *x = xasprintf(" Picking %s for %s \xe2\x80\x94 %s assigns, %s cancels", what, bn, enter, cancel);
        t = S(TUI_TC_ACCENT, x);
        free(what), free(bn), free(enter), free(cancel), free(x);
    } else if (create_active(h)) t = S(TUI_TC_ACCENT, " New agent \xe2\x80\x94 describe it and let the architect draft it");
    else if (h->notice) {
        char *x = xasprintf(" %s", h->notice);
        t = S(TUI_TC_SUCCESS, x);
        free(x);
    } else {
        const hentry_t *e = &h->entries.v[h->active];
        size_t n = 0;
        for (size_t i = 0; i < h->rows.n; i++) n += h->rows.v[i] >= 0;
        char *x = e->kind == E_SOURCE ? xasprintf(" %s agents \xc2\xb7 %zu", e->label, n) : xasprintf(" All agents \xc2\xb7 %zu", n);
        t = S(TUI_TC_MUTED, x);
        free(x);
    }
    push_trunc(out, t, width);
}

static void render_list(hub_t *h, int width, int rows, strvec_t *out) {
    size_t base = out->n;
    const char *q = sb_cstr(&h->search);
    char *st;
    if (*q) {
        char *in = render_input("", q, tui_visible_width(q) + 1);
        st = S(TUI_TC_ACCENT, in);
        free(in);
    } else st = S(TUI_TC_DIM, "type to filter");
    char *sl = S(TUI_TC_MUTED, "search:");
    push_trunc(out, xasprintf(" %s %s", sl, st), width);
    free(sl), free(st);
    strvec_push(out, "");
    h->list_row_start = (int)(out->n - base) + 1; /* + the status row */
    int detail = 4, visible = MAX(3, rows - (int)(out->n - base) - detail);
    if (h->row_index < h->list_scroll) h->list_scroll = h->row_index;
    else if (h->row_index >= h->list_scroll + visible) h->list_scroll = h->row_index - visible + 1;
    h->list_scroll = MAX(0, MIN(h->list_scroll, MAX(0, (int)h->rows.n - visible)));
    int name_w = 0;
    for (size_t i = 0; i < h->rows.n; i++)
        if (h->rows.v[i] >= 0) name_w = MAX(name_w, tui_visible_width(h->agents.v[h->rows.v[i]].name));
    for (int i = h->list_scroll; i < MIN((int)h->rows.n, h->list_scroll + visible); i++) {
        bool sel = i == h->row_index, hov = i == h->row_hover;
        char *cursor = sel && h->focus_list ? S(TUI_TC_ACCENT, tui_sym("nav.cursor")) : xstrdup(" ");
        char *line;
        if (h->rows.v[i] < 0) {
            char *t = S(sel ? TUI_TC_ACCENT : TUI_TC_DIM, "+ New agent\xe2\x80\xa6");
            line = xasprintf(" %s %s", cursor, t);
            free(t);
            if (hov) {
                char *bg = tui_bgs(NULL, TUI_TC_SELECTED_BG, line);
                free(line);
                line = bg;
            }
            free(cursor);
            push_trunc(out, line, width);
            continue;
        }
        const hagent_t *a = &h->agents.v[h->rows.v[i]];
        char *dot = a->disabled ? S(TUI_TC_DIM, tui_sym("status.disabled")) : S(TUI_TC_SUCCESS, tui_sym("status.enabled"));
        char *nm = tui_replace_tabs(a->name), *padded = tui_pad_to_width(nm, name_w), *ns;
        if (a->disabled) ns = S(TUI_TC_DIM, padded);
        else if (sel) {
            char *x = S(TUI_TC_ACCENT, padded);
            ns = tui_bold(x);
            free(x);
        } else ns = xstrdup(padded);
        sb_t right = SB_INIT;
        if (a->ovr[P_MODEL]) {
            char *x = S(TUI_TC_WARNING, a->ovr[P_MODEL]);
            sb_append(&right, x);
            free(x);
        }
        char *pw = effective_prewalk(a), *ad = effective_advisor(a);
        if (pw) {
            char *t = xasprintf("pre:%s", pw), *x = S(TUI_TC_DIM, t);
            if (right.len) sb_append(&right, "  ");
            sb_append(&right, x);
            free(t), free(x);
        }
        if (ad) {
            char *t = xasprintf("adv:%s", ad), *x = S(TUI_TC_DIM, t);
            if (right.len) sb_append(&right, "  ");
            sb_append(&right, x);
            free(t), free(x);
        }
        free(pw), free(ad);
        char *tag = S(TUI_TC_DIM, SRC_TAG[a->source]);
        line = xasprintf(" %s %s %s  %s", cursor, dot, ns, tag);
        int rw = tui_visible_width(sb_cstr(&right)), lw = tui_visible_width(line);
        if (rw > 0 && lw + rw + 2 <= width) {
            char *x = xasprintf("%s%*s%s", line, width - lw - rw - 1, "", sb_cstr(&right));
            free(line);
            line = x;
        }
        char *tr = tui_truncate_to_width(line, width, NULL);
        free(line);
        line = tr;
        if (hov) {
            char *p = tui_pad_to_width(line, width), *bg = tui_bgs(NULL, TUI_TC_SELECTED_BG, p);
            free(line), free(p);
            line = bg;
        }
        strvec_push_own(out, line);
        sb_free(&right);
        free(cursor), free(dot), free(nm), free(padded), free(ns), free(tag);
    }
    while ((int)(out->n - base) < rows - detail) strvec_push(out, "");
    sb_t rule = SB_INIT;
    sb_repeat(&rule, "\xe2\x94\x80", MAX(1, width));
    strvec_push_own(out, S(TUI_TC_BORDER, sb_cstr(&rule)));
    sb_free(&rule);
    hagent_t *a = selected_agent(h);
    if (a) {
        char *d = tui_replace_tabs(a->description), *dd = S(TUI_TC_DIM, d);
        push_trunc(out, xasprintf(" %s", dd), width);
        free(d), free(dd);
        strvec_t pats = effective_model_patterns(h, a);
        char *res = resolve_patterns(&pats), *mk = S(TUI_TC_MUTED, "model:");
        char *pl = pats.n ? strvec_join(&pats, ",") : NULL, *pv = pl ? tui_replace_tabs(pl) : S(TUI_TC_DIM, "(session model)");
        char *arrow = NULL;
        if (res) {
            char *ar = S(TUI_TC_DIM, "\xe2\x86\x92"), *rs = S(TUI_TC_SUCCESS, res);
            arrow = xasprintf(" %s %s", ar, rs);
            free(ar), free(rs);
        }
        push_trunc(out, xasprintf(" %s %s%s", mk, pv, arrow ? arrow : ""), width);
        free(res), free(mk), free(pl), free(pv), free(arrow);
        strvec_free(&pats);
        char *pw = effective_prewalk(a), *ad = effective_advisor(a);
        char *pk = S(TUI_TC_MUTED, "prewalk:"), *pvs = pw ? S(TUI_TC_SUCCESS, pw) : S(TUI_TC_DIM, "off");
        char *ak = S(TUI_TC_MUTED, "advisor:"), *avs = ad ? S(TUI_TC_SUCCESS, ad) : S(TUI_TC_DIM, "off");
        char *fp = a->file_path ? tc_shorten_path(a->file_path) : NULL, *fps = fp ? S(TUI_TC_DIM, fp) : NULL;
        push_trunc(out, fps ? xasprintf(" %s %s   %s %s   %s", pk, pvs, ak, avs, fps) : xasprintf(" %s %s   %s %s", pk, pvs, ak, avs), width);
        free(pw), free(ad), free(pk), free(pvs), free(ak), free(avs), free(fp), free(fps);
    } else {
        strvec_push_own(out, S(TUI_TC_DIM, " Select an agent to inspect"));
        strvec_push(out, "");
        strvec_push(out, "");
    }
}

static void wrap_into(const char *text, int width, strvec_t *w) {
    strvec_t lines = str_split(text, "\n");
    for (size_t i = 0; i < lines.n; i++) {
        char *t = tui_replace_tabs(lines.v[i]);
        tui_wrap_text_with_ansi(t, width, w);
        free(t);
    }
    strvec_free(&lines);
}

static void render_create(hub_t *h, int width, int rows, strvec_t *out) {
    size_t base = out->n;
    strvec_push(out, "");
    if (h->spec_id) {
        char *x = S(TUI_TC_ACCENT, " Review generated agent");
        push_trunc(out, tui_bold(x), width);
        free(x);
        strvec_push(out, "");
        char *a = xasprintf(" Identifier: %s", h->spec_id), *b = xasprintf(" Scope: %s", h->create_scope == SRC_USER ? "user" : "project");
        push_trunc(out, S(TUI_TC_MUTED, a), width);
        push_trunc(out, S(TUI_TC_MUTED, b), width);
        free(a), free(b);
        strvec_push(out, "");
        strvec_push_own(out, S(TUI_TC_MUTED, " whenToUse:"));
        strvec_t w = {0};
        char *wt = tui_replace_tabs(h->spec_when);
        tui_wrap_text_with_ansi(wt, MAX(20, width - 2), &w);
        free(wt);
        for (size_t i = 0; i < w.n && i < 6; i++) push_trunc(out, xasprintf(" %s", w.v[i]), width);
        strvec_free(&w);
        strvec_push(out, "");
        strvec_push_own(out, S(TUI_TC_MUTED, " systemPrompt preview:"));
        strvec_t p = {0};
        wrap_into(h->spec_prompt, MAX(20, width - 4), &p);
        int budget = MAX(3, rows - (int)(out->n - base) - 3);
        for (int i = 0; i < (int)p.n && i < budget; i++) {
            char *d = S(TUI_TC_DIM, p.v[i]);
            push_trunc(out, xasprintf("   %s", d), width);
            free(d);
        }
        if ((int)p.n > budget) {
            char *m = xasprintf("   \xe2\x80\xa6 %d more lines", (int)p.n - budget);
            strvec_push_own(out, S(TUI_TC_DIM, m));
            free(m);
        }
        strvec_free(&p);
    } else {
        char *x = S(TUI_TC_ACCENT, " Create new agent");
        push_trunc(out, tui_bold(x), width);
        free(x);
        strvec_push(out, "");
        char *m = S(TUI_TC_MUTED, " Describe what the agent should do; scope: "), *sc = S(TUI_TC_ACCENT, h->create_scope == SRC_USER ? "user" : "project");
        push_trunc(out, xasprintf("%s%s", m, sc), width);
        free(m), free(sc);
        strvec_push(out, "");
        if (h->create_input && !h->generating) {
            /* Editor without a border, "> " prompt gutter, cursor at the end of the text */
            strvec_t lines = str_split(sb_cstr(&h->create_text), "\n");
            if (!lines.n) strvec_push(&lines, "");
            int iw = MAX(20, width - 2);
            for (size_t i = 0; i < lines.n; i++) {
                strvec_t w = {0};
                char *t = tui_replace_tabs(lines.v[i]);
                tui_wrap_text_with_ansi(*t ? t : " ", iw - 2, &w);
                free(t);
                for (size_t k = 0; k < w.n; k++) {
                    bool last = i + 1 == lines.n && k + 1 == w.n;
                    const char *g = i == 0 && k == 0 ? "> " : "  ";
                    const char *txt = *lines.v[i] ? w.v[k] : "";
                    /* the editor paints its text in the resolved text color */
                    char cb[32];
                    tui_rgb_fg(NULL, tui_theme_rgb(NULL, TUI_TC_TEXT), cb, sizeof cb);
                    push_trunc(out, xasprintf("%s%s%s%s\x1b[39m", g, cb, txt, last ? "\xe2\x96\x8f" : ""), width);
                }
                strvec_free(&w);
            }
            strvec_free(&lines);
        }
        if (h->generating) {
            strvec_push_own(out, S(TUI_TC_MUTED, " Generating\xe2\x80\xa6"));
            strvec_push(out, "");
            strvec_t w = {0};
            wrap_into(sb_cstr(&h->streaming), MAX(20, width - 4), &w);
            int budget = MAX(3, rows - (int)(out->n - base) - 2);
            for (size_t i = w.n > (size_t)budget ? w.n - (size_t)budget : 0; i < w.n; i++) {
                char *d = S(TUI_TC_DIM, w.v[i]);
                push_trunc(out, xasprintf("  %s", d), width);
                free(d);
            }
            strvec_free(&w);
        }
    }
    if (h->create_error) {
        strvec_push(out, "");
        char *e = tui_replace_tabs(h->create_error), *x = xasprintf(" %s", e);
        push_trunc(out, S(TUI_TC_ERROR, x), width);
        free(e), free(x);
    }
}

static void render_body(hub_t *h, int width, int rows, strvec_t *out) {
    status_row(h, width, out);
    if (create_active(h)) render_create(h, width, rows - 1, out);
    else if (h->assign_agent >= 0 && h->b) {
        ui_model_browser_set_max_visible(h->b, rows - 1 - 5);
        h->b->focused = true;
        ui_model_browser_render(h->b, width, out);
    } else render_list(h, width, rows - 1, out);
    while ((int)out->n < rows) strvec_push(out, "");
    while ((int)out->n > rows) {
        free(out->v[out->n - 1]);
        out->n--;
    }
}

static char *hints2(const char *a, const char *b) {
    char *x = ui_key_hint(a), *y = ui_key_hint(b), *r = xasprintf("%s/%s", x, y);
    free(x), free(y);
    return r;
}

static char *footer_hint(hub_t *h) {
    char *enter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel"), *ud = hints2("up", "down"), *r;
    if (h->strip == ST_PATTERN) {
        const char *values = h->strip_property == P_MODEL ? "a model pattern" : "\"on\", \"off\", or a model pattern";
        r = xasprintf("Enter %s (role aliases like @smol and :level suffixes work; empty clears) \xc2\xb7 %s back", values, cancel);
    } else if (h->strip == ST_CHIPS) {
        char *lr = hints2("left", "right");
        r = h->strip_property >= 0 ? xasprintf("%s choose \xc2\xb7 %s apply \xc2\xb7 %s back", lr, enter, cancel)
                                   : xasprintf("%s choose \xc2\xb7 %s open \xc2\xb7 %s cancel", lr, enter, cancel);
        free(lr);
    } else if (h->assign_agent >= 0) {
        r = xasprintf("%s pick \xc2\xb7 %s models \xc2\xb7 type to search \xc2\xb7 %s cancel", enter, ud, cancel);
    } else if (create_active(h)) {
        char *tab = ui_key_hint("tab");
        if (h->spec_id) {
            char *rk = ui_key_hint("r");
            r = xasprintf("%s save \xc2\xb7 %s scope \xc2\xb7 %s regenerate \xc2\xb7 %s cancel", enter, tab, rk, cancel);
            free(rk);
        } else if (h->generating) r = xstrdup("Generating\xe2\x80\xa6");
        else {
            const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), "app.message.followUp");
            sb_t g = SB_INIT;
            for (size_t i = 0; keys && i < keys->n; i++) {
                char *k = ui_key_hint(keys->v[i]);
                sb_appendf(&g, "%s%s", i ? "/" : "", k);
                free(k);
            }
            if (!g.len) {
                char *k = hints2("ctrl+q", "ctrl+enter");
                sb_append(&g, k);
                free(k);
            }
            r = xasprintf("%s generate \xc2\xb7 %s newline \xc2\xb7 %s scope \xc2\xb7 %s cancel", sb_cstr(&g), enter, tab, cancel);
            sb_free(&g);
        }
        free(tab);
    } else if (!h->focus_list) {
        char *re = hints2("right", "enter");
        r = xasprintf("%s scopes \xc2\xb7 %s agents \xc2\xb7 %s close", ud, re, cancel);
        free(re);
    } else {
        char *sp = ui_key_hint("space"), *cr = ui_key_hint("ctrl+r");
        r = xasprintf("%s configure \xc2\xb7 %s enable/disable \xc2\xb7 %s rows \xc2\xb7 type to search \xc2\xb7 %s reload \xc2\xb7 %s close", enter, sp, ud, cr, cancel);
        free(sp), free(cr);
    }
    free(enter), free(cancel), free(ud);
    return r;
}

static char *render_strip(hub_t *h, int width) {
    h->ranges.n = 0;
    const hagent_t *a = &h->agents.v[h->strip_agent];
    if (h->strip == ST_PATTERN) {
        char *lt = xasprintf("%s %s pattern:", a->name, PROP[h->strip_property]), *label = S(TUI_TC_ACCENT, lt);
        int iw = MAX(8, MIN(40, width - tui_visible_width(lt) - 4));
        char *in = render_input("> ", sb_cstr(&h->pattern), iw), *line = xasprintf("%s %s", label, in);
        char *r = tui_truncate_to_width(line, width, NULL);
        free(lt), free(label), free(in), free(line);
        return r;
    }
    char *an = S(TUI_TC_ACCENT, a->name), *tail = h->strip_property >= 0 ? xasprintf(" \xc2\xb7 %s \xe2\x86\x92", PROP[h->strip_property]) : xstrdup(" \xe2\x86\x92");
    char *dt = S(TUI_TC_DIM, tail), *prefix = xasprintf("%s%s ", an, dt);
    free(an), free(tail), free(dt);
    /* HubFrame.renderChips from the first chip */
    sb_t line = SB_INIT;
    sb_append(&line, prefix);
    int col = 2 + tui_visible_width(prefix);
    for (size_t i = 0; i < h->chips.n; i++) {
        char *body = xasprintf(" %s ", h->chips.v[i].styled);
        bool sel = i == h->chip_index;
        int w = tui_visible_width(body) + (sel ? 2 : 0);
        crange_t cr = {col, col + w, (int)i};
        vec_push(&h->ranges, cr);
        if (sel) {
            char *lb = S(TUI_TC_ACCENT, "["), *rb = S(TUI_TC_ACCENT, "]"), *in = xasprintf("%s%s%s", lb, body, rb);
            char *bg = tui_bgs(NULL, TUI_TC_SELECTED_BG, in);
            sb_append(&line, bg);
            free(lb), free(rb), free(in), free(bg);
        } else sb_append(&line, body);
        sb_putc(&line, ' ');
        col += w + 1;
        free(body);
    }
    char *r = tui_truncate_to_width(sb_cstr(&line), width, NULL);
    sb_free(&line);
    free(prefix);
    return r;
}

static void paint(sb_t *b, const char *s) { tui_fg(b, NULL, TUI_TC_BORDER, s); }

static void hub_render(tui_component_t *c, int width, strvec_t *out) {
    hub_t *h = (hub_t *)c;
    int height = MAX(16, tui_term(h->u->tui)->rows > 0 ? tui_term(h->u->tui)->rows : 40);
    int content = MAX(10, height - 4);
    int L = sidebar_width(h), R = MAX(0, width - 7 - L);
    h->sidebar_w = L;
    h->body_w = R;
    h->content_rows = content;
    const char *H = tui_sym("boxRound.horizontal");
    int dcol = L + 3, left_len = MAX(0, dcol - 1), right_len = MAX(0, width - 2 - dcol);
    sb_t t = SB_INIT, r = SB_INIT;
    char *shown = tui_truncate_to_width(" Agents ", MAX(0, left_len - 1), NULL);
    int fill = MAX(0, left_len - 1 - tui_visible_width(shown));
    sb_appendf(&r, "%s%s", tui_sym("boxRound.topLeft"), H);
    paint(&t, sb_cstr(&r));
    sb_clear(&r);
    char *acc = S(TUI_TC_ACCENT, shown), *bold = tui_bold(acc);
    sb_append(&t, bold);
    free(acc), free(bold), free(shown);
    sb_repeat(&r, H, fill);
    sb_append(&r, tui_sym("boxSharp.teeDown"));
    sb_repeat(&r, H, right_len);
    sb_append(&r, tui_sym("boxRound.topRight"));
    paint(&t, sb_cstr(&r));
    strvec_push_own(out, sb_detach(&t));
    sb_free(&r);
    strvec_t side = {0}, body = {0};
    render_sidebar(h, L, content, &side);
    render_body(h, R, content, &body);
    char *bar = S(TUI_TC_BORDER, tui_sym("boxRound.vertical"));
    for (int i = 0; i < content; i++) {
        char *sl = ui_fit_line(i < (int)side.n ? side.v[i] : "", L), *bl = ui_fit_line(i < (int)body.n ? body.v[i] : "", R);
        strvec_push_own(out, xasprintf("%s %s %s %s %s", bar, sl, bar, bl, bar));
        free(sl), free(bl);
    }
    strvec_free(&side);
    strvec_free(&body);
    sb_t d = SB_INIT, dd = SB_INIT;
    sb_append(&d, tui_sym("boxSharp.teeRight"));
    sb_repeat(&d, H, left_len);
    sb_append(&d, tui_sym("boxSharp.teeUp"));
    sb_repeat(&d, H, right_len);
    sb_append(&d, tui_sym("boxSharp.teeLeft"));
    paint(&dd, sb_cstr(&d));
    strvec_push_own(out, sb_detach(&dd));
    sb_free(&d);
    char *foot;
    if (h->strip) foot = render_strip(h, width - 4);
    else {
        h->ranges.n = 0;
        char *f = footer_hint(h), *fd = S(TUI_TC_DIM, f);
        foot = tui_truncate_to_width(fd, width - 4, NULL);
        free(f), free(fd);
    }
    strvec_push_own(out, ui_box_row(foot, width));
    free(foot);
    strvec_push_own(out, ui_box_bottom(width));
    free(bar);
}

/* ---------- input ---------- */

static const char *const MOUSE_ON = "\x1b[?1000h\x1b[?1003h\x1b[?1006h";
static const char *const MOUSE_OFF = "\x1b[?1006l\x1b[?1003l\x1b[?1000l";

static void close_hub(hub_t *h) {
    ui_t *u = h->u;
    tui_overlay_hide(u->tui, &h->base);
    tui_term_puts(tui_term(u->tui), MOUSE_OFF);
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    tui_component_destroy(&h->base);
}

static bool is_enter(const tui_key_t *k) { return tui_key_matches(k, "enter") || tui_key_matches(k, "return"); }

static void move_sidebar(hub_t *h, int delta) {
    int n = (int)h->entries.n, i = (int)h->active;
    for (int step = 0; step < n; step++) {
        i = (i + delta + n) % n;
        if (h->entries.v[i].kind != E_SEP) {
            h->active = (size_t)i;
            if (h->entries.v[i].kind != E_NEW) {
                build_rows(h);
                h->row_index = 0;
                h->list_scroll = 0;
            }
            return;
        }
    }
}

static void activate_row(hub_t *h, int idx) {
    if (idx < 0 || idx >= (int)h->rows.n) return;
    if (h->rows.v[idx] < 0) begin_create(h);
    else open_agent_strip(h, h->rows.v[idx]);
}

/* the single-line inputs (search, pattern): printable text, backspace */
static bool line_input(sb_t *s, const tui_key_t *k) {
    if (tui_key_matches(k, "backspace")) {
        if (s->len) {
            size_t n = s->len;
            do n--;
            while (n > 0 && ((unsigned char)s->s[n] & 0xC0) == 0x80);
            sb_truncate(s, n);
        }
        return true;
    }
    const char *t = tui_key_text(k);
    if ((k->kind == TUI_KEY_CHAR || k->kind == TUI_KEY_PASTE) && t && *t && !(k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT | TUI_MOD_SUPER))) {
        for (const char *p = t; *p; p++)
            if (*p != '\n' && *p != '\r') sb_putc(s, *p);
        return true;
    }
    return false;
}

static bool parse_sgr(const char *s, int *button, int *col, int *row, bool *release) {
    if (!s || strncmp(s, "\x1b[<", 3)) return false;
    char fin = 0;
    if (sscanf(s + 3, "%d;%d;%d%c", button, col, row, &fin) != 4 || (fin != 'M' && fin != 'm')) return false;
    *col -= 1;
    *row -= 1;
    *release = fin == 'm';
    return true;
}

static void hub_mouse(hub_t *h, const tui_key_t *k) {
    int button, col, row;
    bool release;
    if (!parse_sgr(k->text, &button, &col, &row, &release)) return;
    int wheel = (button & 64) && !(button & 2) ? ((button & 1) ? 1 : -1) : 0;
    bool motion = (button & 32) && !wheel;
    bool click = !release && !wheel && !motion && (button & 3) == 0;
    int footer_row = 1 + h->content_rows + 1;
    if (row == footer_row && h->strip == ST_CHIPS) {
        if (click)
            for (size_t i = 0; i < h->ranges.n; i++)
                if (col >= h->ranges.v[i].start && col < h->ranges.v[i].end) {
                    h->chip_index = (size_t)h->ranges.v[i].index;
                    activate_chip(h);
                    return;
                }
        return;
    }
    int line = row - 1;
    bool over_content = line >= 0 && line < h->content_rows;
    bool over_side = over_content && col >= 2 && col < 2 + h->sidebar_w;
    int body_x = 2 + h->sidebar_w + 3;
    bool over_body = over_content && col >= body_x && col < body_x + h->body_w;
    int body_line = line - 1;
    if (h->assign_agent >= 0) {
        if (over_body && h->b && body_line >= 0) {
            int r = ui_model_browser_mouse(h->b, body_line, wheel, motion, click);
            if (r == UI_MB_ACTIVATE) {
                const ui_model_item_t *it = ui_model_browser_selected(h->b);
                if (it) {
                    char *sel = xstrdup(it->selector);
                    int ai = h->assign_agent, p = h->assign_property;
                    cancel_assign(h);
                    set_override(h, &h->agents.v[ai], p, sel);
                    free(sel);
                }
            }
        }
        return;
    }
    if (create_active(h) || h->strip) return;
    if (wheel) {
        if (over_side) h->sidebar_scroll = MAX(0, MIN(MAX(0, (int)h->entries.n - h->content_rows), h->sidebar_scroll + wheel));
        else if (over_body) h->row_index = MAX(0, MIN((int)h->rows.n - 1, h->row_index + wheel));
        return;
    }
    if (motion) {
        int hr = line - h->list_row_start + h->list_scroll;
        h->row_hover = over_body && hr >= 0 && hr < (int)h->rows.n ? hr : -1;
        return;
    }
    if (!click) return;
    if (over_side) {
        int idx = h->sidebar_scroll + line;
        if (idx < (int)h->entries.n && h->entries.v[idx].kind != E_SEP) {
            if (h->entries.v[idx].kind == E_NEW) begin_create(h);
            else {
                h->active = (size_t)idx;
                build_rows(h);
                h->row_index = 0;
                h->focus_list = false;
            }
        }
        return;
    }
    if (over_body) {
        h->focus_list = true;
        int ll = line - h->list_row_start + h->list_scroll;
        if (ll >= 0 && ll < (int)h->rows.n) {
            if (ll == h->row_index) activate_row(h, ll);
            else h->row_index = ll;
        }
    }
}

static void create_key(hub_t *h, const tui_key_t *k) {
    if (h->spec_id) {
        if (tui_kb(k, "tui.select.cancel")) clear_create(h);
        else if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab")) h->create_scope = h->create_scope == SRC_PROJECT ? SRC_USER : SRC_PROJECT;
        else if (k->kind == TUI_KEY_CHAR && !(k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT)) && (k->codepoint == 'r' || k->codepoint == 'R')) generate(h);
        else if (is_enter(k)) save_spec(h);
        return;
    }
    if (tui_kb(k, "tui.select.cancel")) {
        if (!h->generating) clear_create(h);
        return;
    }
    if (h->generating) return;
    if (tui_kb(k, "app.message.followUp")) {
        generate(h);
        return;
    }
    if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab")) {
        h->create_scope = h->create_scope == SRC_PROJECT ? SRC_USER : SRC_PROJECT;
        return;
    }
    if (is_enter(k)) {
        sb_putc(&h->create_text, '\n');
        return;
    }
    if (k->kind == TUI_KEY_PASTE && tui_key_text(k)) {
        sb_append(&h->create_text, tui_key_text(k));
        return;
    }
    line_input(&h->create_text, k);
}

static bool hub_key(tui_component_t *c, const tui_key_t *k) {
    hub_t *h = (hub_t *)c;
    tui_component_request_render(c);
    if (k->kind == TUI_KEY_MOUSE) {
        hub_mouse(h, k);
        return true;
    }
    if (h->strip) {
        if (tui_kb(k, "tui.select.cancel")) strip_back(h);
        else if (h->strip == ST_PATTERN) {
            if (is_enter(k)) {
                char *v = xstrdup(sb_cstr(&h->pattern));
                int ai = h->strip_agent, p = h->strip_property;
                close_strip(h);
                set_override(h, &h->agents.v[ai], p, v);
                free(v);
            } else line_input(&h->pattern, k);
        } else if (tui_key_matches(k, "left") || tui_key_matches(k, "up") || tui_key_matches(k, "shift+tab"))
            h->chip_index = (h->chip_index + h->chips.n - 1) % MAX(1, h->chips.n);
        else if (tui_key_matches(k, "right") || tui_key_matches(k, "down") || tui_key_matches(k, "tab"))
            h->chip_index = (h->chip_index + 1) % MAX(1, h->chips.n);
        else if (is_enter(k)) activate_chip(h);
        return true;
    }
    if (create_active(h)) {
        create_key(h, k);
        return true;
    }
    if (tui_kb(k, "tui.select.cancel")) {
        if (h->assign_agent >= 0) cancel_assign(h);
        else if (h->search.len) {
            sb_clear(&h->search);
            build_rows(h);
            clamp_row(h);
        } else close_hub(h);
        return true;
    }
    if (h->assign_agent >= 0) {
        int r = ui_model_browser_key(h->b, k);
        if (r == UI_MB_CANCEL) cancel_assign(h);
        else if (r == UI_MB_ACTIVATE) {
            const ui_model_item_t *it = ui_model_browser_selected(h->b);
            if (it) {
                char *sel = xstrdup(it->selector);
                int ai = h->assign_agent, p = h->assign_property;
                cancel_assign(h);
                set_override(h, &h->agents.v[ai], p, sel);
                free(sel);
            }
        }
        return true;
    }
    if (tui_key_matches(k, "ctrl+r")) {
        reload(h);
        return true;
    }
    if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab")) {
        h->focus_list = !h->focus_list;
        return true;
    }
    if (tui_key_matches(k, "left")) {
        h->focus_list = false;
        return true;
    }
    if (tui_key_matches(k, "right")) {
        h->focus_list = true;
        return true;
    }
    if (!h->focus_list) {
        if (tui_kb(k, "tui.select.up")) {
            move_sidebar(h, -1);
            return true;
        }
        if (tui_kb(k, "tui.select.down")) {
            move_sidebar(h, 1);
            return true;
        }
        if (is_enter(k)) {
            if (h->entries.v[h->active].kind == E_NEW) begin_create(h);
            else h->focus_list = true;
            return true;
        }
    }
    if (tui_kb(k, "tui.select.up")) {
        h->row_index = MAX(0, h->row_index - 1);
        return true;
    }
    if (tui_kb(k, "tui.select.down")) {
        h->row_index = MIN((int)h->rows.n - 1, h->row_index + 1);
        return true;
    }
    if (is_enter(k)) {
        activate_row(h, h->row_index);
        return true;
    }
    const char *t = tui_key_text(k);
    if (t && !strcmp(t, " ") && !h->search.len) {
        hagent_t *a = selected_agent(h);
        if (a) toggle_agent(h, a);
        return true;
    }
    /* type-to-filter */
    size_t before = h->search.len;
    if (!line_input(&h->search, k)) return true;
    if (h->search.len < before) {
        build_rows(h);
        clamp_row(h);
    } else if (h->search.len > before) {
        h->focus_list = true;
        build_rows(h);
        h->row_index = 0;
        h->list_scroll = 0;
    }
    return true;
}

static void hub_destroy(tui_component_t *c) {
    hub_t *h = (hub_t *)c;
    if (h->gen_live) {
        h->gen_cancel = 1;
        pthread_join(h->gen_th, NULL);
    }
    for (size_t i = 0; i < h->agents.n; i++) hagent_free(&h->agents.v[i]);
    vec_free(&h->agents);
    entries_free(h);
    vec_free(&h->entries);
    vec_free(&h->rows);
    chips_free(h);
    vec_free(&h->chips);
    vec_free(&h->ranges);
    sb_free(&h->search);
    sb_free(&h->pattern);
    sb_free(&h->create_text);
    sb_free(&h->streaming);
    clear_spec(h);
    free(h->create_error);
    free(h->notice);
    free(h->load_error);
    ui_model_browser_free(h->b);
    free(h);
}

static const tui_component_vt HUB_VT = {"agents-hub", hub_render, hub_key, NULL, hub_destroy};

void ui_agents_hub_open(ui_t *u) {
    hub_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &HUB_VT);
    h->base.wants_mouse = true;
    h->u = u;
    h->focus_list = true;
    h->row_hover = -1;
    h->assign_agent = -1;
    h->strip_agent = -1;
    h->content_rows = 10;
    reload(h);
    tui_alt_screen(u->tui, true);
    tui_term_puts(tui_term(u->tui), MOUSE_ON);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &h->base, &o);
    tui_set_focus(u->tui, &h->base);
}
