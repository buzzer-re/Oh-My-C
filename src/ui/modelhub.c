/* modelhub.c - the Models hub opened by /model and Alt+M (port of omp overlays/model-hub.ts on
 * overlays/hub-frame.ts): a fullscreen split frame titled "Models" with a sidebar (✦ Roles n/16,
 * ⬢ All models, ● providers with credentials and their model counts, ○ catalog providers without
 * credentials) and a body that is either the model browser (status row, "Kind:" segment track, search,
 * rows, two detail rows), the Roles view (role rows with assignment, thinking level and ↻ cycle badges,
 * the Ctrl+P cycle track) or a provider's "not configured" page. Enter on a model opens the footer role
 * strip (assign / clear a role, then a thinking-level strip for reasoning models); Enter on a role row
 * assigns it from the browser. Roles persist to modelRoles.<role> in config.yml; assigning default also
 * switches the session model.
 *
 * Not ported: Recent scope, retry-fallback chains (f), compaction limits (k), custom roles (n), presets
 * (s, p/P, Ctrl+←/→), project-scoped role storage, provider discovery refresh (F5), mouse. */
#include "ui/modelbrowser.h"
#include "ui/overlays.h"
#include "ui/ui.h"

#include "config/auth.h"
#include "config/settings.h"
#include "tui/fuzzy.h"

#include <ctype.h>

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

/* ---------- roles (coding-agent config/model-roles.ts MODEL_ROLES) ---------- */

typedef struct {
    const char *id, *tag, *name;
    tui_tc_t color;
    bool kind_section;
    const char *accepts; /* model kind accepted ("chat" also takes tiny for tiny/memory) */
    bool auto_select;    /* `pi/<role>` resolves over the chat pool */
} role_t;

static const role_t ROLES[] = {
    {"default", "DEFAULT", "Default", TUI_TC_SUCCESS, false, "chat", true},
    {"smol", "SMOL", "Fast", TUI_TC_WARNING, false, "chat", true},
    {"slow", "SLOW", "Thinking", TUI_TC_ACCENT, false, "chat", true},
    {"vision", "VISION", "Vision", TUI_TC_ERROR, false, "chat", false},
    {"plan", "PLAN", "Architect", TUI_TC_MUTED, false, "chat", false},
    {"commit", "COMMIT", "Commit", TUI_TC_DIM, false, "chat", false},
    {"tiny", "TINY", "Tiny", TUI_TC_DIM, false, "tiny|chat", true},
    {"memory", "MEMORY", "Memory", TUI_TC_DIM, false, "tiny|chat", true},
    {"task", "TASK", "Subtask", TUI_TC_MUTED, false, "chat", false},
    {"advisor", "ADVISOR", "Advisor", TUI_TC_ACCENT, false, "chat", false},
    {"widget", "WIDGET", "Job widgets", TUI_TC_DIM, false, "chat", true},
    {"image", "IMAGE", "Image generation", TUI_TC_ACCENT, true, "image", true},
    {"web", "WEB", "Web search", TUI_TC_SUCCESS, true, "search", true},
    {"speech", "SPEECH", "Speech", TUI_TC_WARNING, true, "tts", true},
    {"dictation", "DICTATION", "Dictation", TUI_TC_WARNING, true, "stt", true},
    {"judge", "JUDGE", "Judge", TUI_TC_MUTED, true, "judge|chat", true},
};
#define NROLES ARRAY_LEN(ROLES)

static const char *const KINDS[] = {"all", "chat", "tiny", "image", "tts", "stt", "search", "judge", "embedding", "rerank", "video"};

static const char *model_kind(const ai_model_t *m) {
    const char *k = m && m->raw ? json_get_str(m->raw, "kind") : NULL;
    return k && *k ? k : "chat";
}

static bool role_accepts(const role_t *r, const ai_model_t *m) {
    const char *k = model_kind(m);
    char buf[32];
    snprintf(buf, sizeof buf, "%s", r->accepts);
    for (char *t = strtok(buf, "|"); t; t = strtok(NULL, "|"))
        if (!strcmp(t, k)) return true;
    return false;
}

/* ---------- state ---------- */

enum { E_ROLES, E_ALL, E_SEP, E_PROVIDER };

typedef struct {
    int kind;
    char *provider;
    char *label;
    char *annotation;
    bool locked;
    bool oauth;
    int catalog_count;
} entry_t;

enum { R_ROLE, R_SEP, R_NEWROLE, R_NEWFALLBACK };
typedef struct {
    int kind;
    int role; /* index into ROLES */
} rrow_t;

typedef struct {
    char *label, *styled;
    int action; /* 0 assign, 1 unassign, 2 thinking, 3 noop (fallback chips) */
    int role;
    char *level;
} chip_t;

typedef struct {
    tui_component_t base;
    ui_t *u;
    VEC(entry_t) entries;
    size_t active;
    int sidebar_scroll;
    bool follow;
    bool focus_list;
    ui_model_browser_t *b;
    VEC(const ai_model_t *) available;
    int kind_tab;
    int role_tab; /* 0 all, 1 chat, 2 kinds */
    VEC(rrow_t) rrows;
    int role_index, role_scroll;
    int assigning; /* role index being assigned, -1 none */
    /* strip */
    bool strip;
    int strip_kind; /* 0 role, 1 thinking */
    VEC(chip_t) chips;
    size_t chip_index;
    char *strip_selector;
    const ai_model_t *strip_model;
    int strip_role;
    bool strip_return;
    char *strip_initial;
} hub_t;

static int sidebar_rows_cache = 10;

/* ---------- role assignments ---------- */

typedef struct {
    const ai_model_t *model;
    bool auto_selected;
    char *level; /* configured thinking level (owned), NULL = inherit */
} assign_t;

static const ai_model_t *find_available(hub_t *h, const char *sel) {
    if (!sel) return NULL;
    for (size_t i = 0; i < h->available.n; i++) {
        char *s = xasprintf("%s/%s", h->available.v[i]->provider, h->available.v[i]->id);
        bool eq = !strcmp(s, sel);
        free(s);
        if (eq) return h->available.v[i];
    }
    return catalog_resolve(sel);
}

/* configured roles first; the rest auto-select over the available pool (omp resolves `pi/<role>`
 * patterns; omc approximates with the default model, else the first accepted model) */
static assign_t resolve_role(hub_t *h, int r) {
    assign_t a = {0};
    char *path = xasprintf("modelRoles.%s", ROLES[r].id);
    const char *val = settings_str(path, NULL);
    free(path);
    if (val && *val) {
        const char *colon = strrchr(val, ':');
        char *sel = colon && !strchr(colon, '/') ? xstrndup(val, (size_t)(colon - val)) : xstrdup(val);
        a.model = find_available(h, sel);
        if (colon && !strchr(colon, '/')) a.level = xstrdup(colon + 1);
        else if (!strcmp(ROLES[r].id, "default")) {
            const char *dl = settings_str("defaultThinkingLevel", NULL);
            if (dl) a.level = xstrdup(dl);
        }
        free(sel);
        if (a.model) return a;
        free(a.level);
        a.level = NULL;
    }
    if (!ROLES[r].auto_select || !h->available.n) return a;
    const char *def = settings_str("modelRoles.default", NULL);
    char *defsel = def ? xstrndup(def, strcspn(def, ":")) : NULL;
    const ai_model_t *dm = find_available(h, defsel);
    free(defsel);
    if (!dm) dm = h->u->app->model;
    if (dm && role_accepts(&ROLES[r], dm)) a.model = dm;
    else
        for (size_t i = 0; i < h->available.n && !a.model; i++)
            if (role_accepts(&ROLES[r], h->available.v[i])) a.model = h->available.v[i];
    a.auto_selected = a.model != NULL;
    return a;
}

static strvec_t cycle_order(void) {
    strvec_t v = {0};
    json_t *c = settings_get("cycleOrder");
    if (json_is_arr(c)) {
        json_t *x;
        json_arr_foreach(c, i, x) if (json_is_str(x)) strvec_push(&v, json_sv(x));
    } else {
        strvec_push(&v, "smol");
        strvec_push(&v, "default");
        strvec_push(&v, "slow");
    }
    return v;
}

/* ---------- sidebar ---------- */

static void entries_free(hub_t *h) {
    for (size_t i = 0; i < h->entries.n; i++) {
        free(h->entries.v[i].provider);
        free(h->entries.v[i].label);
        free(h->entries.v[i].annotation);
    }
    h->entries.n = 0;
}

static void push_entry(hub_t *h, int kind, const char *label, const char *provider, char *ann_owned, bool locked) {
    entry_t e = {kind, provider ? xstrdup(provider) : NULL, xstrdup(label), ann_owned, locked, false, 0};
    vec_push(&h->entries, e);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static const char *hub_query(const hub_t *h) { return h->b && h->b->query && *h->b->query ? h->b->query : NULL; }

/* fuzzy match count of the live query among a provider's available models (NULL provider: all) */
static int match_count(const hub_t *h, const char *provider) {
    tui_fuzzy_query_t *q = tui_fuzzy_query_new(hub_query(h));
    int n = 0;
    for (size_t i = 0; i < h->available.n; i++) {
        const ai_model_t *m = h->available.v[i];
        if (provider && strcmp(m->provider, provider)) continue;
        bool free_ = m->cost_input == 0 && m->cost_output == 0;
        char *t = xasprintf("%s/%s%s", m->provider, m->id, free_ ? " free" : "");
        if (tui_fuzzy_query_match(q, t, NULL)) n++;
        free(t);
    }
    tui_fuzzy_query_free(q);
    return n;
}

/* entries skipped by sidebar hops (separators; while searching: roles, locked and match-less scopes) */
static bool hop_skipped(const hub_t *h, const entry_t *e) {
    if (e->kind == E_SEP) return true;
    if (!hub_query(h)) return false;
    if (e->kind == E_ROLES) return true;
    if (e->kind == E_PROVIDER) return e->locked || match_count(h, e->provider) == 0;
    return false;
}

static void build_sidebar(hub_t *h) {
    char *keep = h->entries.n ? xstrdup(h->entries.v[h->active].label) : NULL;
    int keep_kind = h->entries.n ? h->entries.v[h->active].kind : E_ALL;
    entries_free(h);
    int assigned = 0;
    for (size_t r = 0; r < NROLES; r++) {
        char *path = xasprintf("modelRoles.%s", ROLES[r].id);
        const char *val = settings_str(path, NULL);
        free(path);
        if (val && *val) assigned++;
    }
    push_entry(h, E_ROLES, "Roles", NULL, xasprintf("%d/%zu", assigned, NROLES), false);
    push_entry(h, E_ALL, "All models", NULL, xasprintf("%zu", h->available.n), false);
    strvec_t unlocked = {0}, locked = {0};
    for (size_t i = 0; i < catalog_count(); i++) {
        const char *p = catalog_at(i)->provider;
        bool has = auth_has_credentials(p) || catalog_provider_keyless(p);
        strvec_t *dst = has ? &unlocked : &locked;
        if (!strvec_contains(dst, p)) strvec_push(dst, p);
    }
    qsort(unlocked.v, unlocked.n, sizeof *unlocked.v, cmp_str);
    if (hub_query(h)) { /* providers with matches float to the top (each group alphabetical) */
        strvec_t matched = {0}, rest = {0};
        for (size_t i = 0; i < unlocked.n; i++) strvec_push(match_count(h, unlocked.v[i]) > 0 ? &matched : &rest, unlocked.v[i]);
        for (size_t i = 0; i < rest.n; i++) strvec_push(&matched, rest.v[i]);
        strvec_free(&unlocked);
        unlocked = matched;
        strvec_free(&rest);
    }
    qsort(locked.v, locked.n, sizeof *locked.v, cmp_str);
    json_t *oauth = catalog_auth_providers();
    if (unlocked.n) {
        push_entry(h, E_SEP, "", NULL, NULL, false);
        for (size_t i = 0; i < unlocked.n; i++) {
            int n = 0;
            for (size_t k = 0; k < h->available.n; k++)
                if (!strcmp(h->available.v[k]->provider, unlocked.v[i])) n++;
            push_entry(h, E_PROVIDER, unlocked.v[i], unlocked.v[i], xasprintf("%d", n), false);
        }
    }
    if (locked.n) {
        push_entry(h, E_SEP, "", NULL, NULL, false);
        for (size_t i = 0; i < locked.n; i++) {
            push_entry(h, E_PROVIDER, locked.v[i], locked.v[i], NULL, true);
            entry_t *e = &h->entries.v[h->entries.n - 1];
            json_t *o;
            json_arr_foreach(oauth, k, o) if (str_eq(json_get_str(o, "id"), locked.v[i])) e->oauth = true;
            for (size_t c = 0; c < catalog_count(); c++)
                if (!strcmp(catalog_at(c)->provider, locked.v[i])) e->catalog_count++;
        }
    }
    strvec_free(&unlocked);
    strvec_free(&locked);
    h->active = 1;
    for (size_t i = 0; i < h->entries.n; i++)
        if (h->entries.v[i].kind == keep_kind && str_eq(h->entries.v[i].label, keep)) h->active = i;
    free(keep);
}

static int sidebar_width(const hub_t *h) {
    int longest = 0;
    for (size_t i = 0; i < h->entries.n; i++)
        longest = MAX(longest, tui_visible_width(h->entries.v[i].label) +
                                   (h->entries.v[i].annotation ? tui_visible_width(h->entries.v[i].annotation) : 0) + 5);
    return MAX(18, MIN(26, longest));
}

static void render_sidebar(hub_t *h, int width, int rows, strvec_t *out) {
    if (h->follow) {
        int a = (int)h->active;
        if (a < h->sidebar_scroll) h->sidebar_scroll = a;
        else if (a >= h->sidebar_scroll + rows) h->sidebar_scroll = a - rows + 1;
        h->follow = false;
    }
    h->sidebar_scroll = MAX(0, MIN(h->sidebar_scroll, MAX(0, (int)h->entries.n - rows)));
    for (int i = h->sidebar_scroll; i < MIN((int)h->entries.n, h->sidebar_scroll + rows); i++) {
        const entry_t *e = &h->entries.v[i];
        if (e->kind == E_SEP) {
            sb_t r = SB_INIT;
            sb_repeat(&r, "\xe2\x94\x80", width);
            strvec_push_own(out, S(TUI_TC_BORDER, sb_cstr(&r)));
            sb_free(&r);
            continue;
        }
        bool active = (size_t)i == h->active;
        bool searching = hub_query(h) != NULL;
        int mc = -1;
        if (searching) {
            if (e->kind == E_PROVIDER && !e->locked) mc = match_count(h, e->provider);
            else if (e->kind == E_ALL) mc = match_count(h, NULL);
        }
        bool muted = e->locked || mc == 0 || (searching && e->kind == E_ROLES);
        const char *iconsym = e->kind == E_ROLES ? tui_sym("icon.extensionSkill")
                              : e->kind == E_ALL ? tui_sym("icon.model")
                              : muted            ? tui_sym("status.shadowed")
                                                 : tui_sym("status.enabled");
        char *icon = S(muted ? TUI_TC_DIM : e->kind == E_PROVIDER ? TUI_TC_SUCCESS : TUI_TC_ACCENT, iconsym);
        char *cursor = active && !h->focus_list ? S(TUI_TC_ACCENT, tui_sym("nav.cursor")) : xstrdup(" ");
        char *label;
        if (muted) label = S(TUI_TC_DIM, e->label);
        else if (active) {
            char *a = S(TUI_TC_ACCENT, e->label);
            label = tui_bold(a);
            free(a);
        } else label = xstrdup(e->label);
        char *left = xasprintf("%s %s %s", cursor, icon, label);
        char mcs[24];
        snprintf(mcs, sizeof mcs, "%d", mc);
        char *ann = S(TUI_TC_DIM, mc >= 0 ? mcs : e->annotation ? e->annotation : "");
        int lw = tui_visible_width(left), aw = tui_visible_width(ann);
        char *line;
        if (lw + aw + 1 <= width) line = xasprintf("%s%*s%s", left, width - lw - aw, "", ann);
        else {
            char *t = tui_truncate_to_width(left, width, NULL);
            line = tui_pad_to_width(t, width);
            free(t);
        }
        strvec_push_own(out, line);
        free(icon);
        free(cursor);
        free(label);
        free(left);
        free(ann);
    }
    while ((int)out->n < rows) strvec_push(out, "");
}

/* ---------- body ---------- */

static bool is_browser_view(const hub_t *h) {
    const entry_t *e = &h->entries.v[h->active];
    return h->assigning >= 0 || e->kind == E_ALL || (e->kind == E_PROVIDER && !e->locked);
}

static void rebuild_browser(hub_t *h) {
    char *query = h->b && h->b->query ? xstrdup(h->b->query) : NULL;
    const ui_model_item_t *cur = h->b ? ui_model_browser_selected(h->b) : NULL;
    char *keep = cur ? xstrdup(cur->selector) : NULL;
    bool focused = h->b ? h->b->focused : false;
    ui_model_browser_free(h->b);
    h->b = ui_model_browser_new();
    h->b->focused = focused;
    const entry_t *e = &h->entries.v[h->active];
    const char *prov = h->assigning < 0 && e->kind == E_PROVIDER ? e->provider : NULL;
    for (size_t i = 0; i < h->available.n; i++) {
        const ai_model_t *m = h->available.v[i];
        if (prov && strcmp(m->provider, prov)) continue;
        if (h->kind_tab > 0 && strcmp(model_kind(m), KINDS[h->kind_tab])) continue;
        ui_model_browser_add(h->b, m);
    }
    h->b->query = query;
    h->b->single_pool = h->available.n == 1;
    ui_model_browser_done_adding(h->b);
    if (keep) ui_model_browser_set_current(h->b, keep);
    free(h->b->current); /* the hub marks no current model */
    h->b->current = NULL;
    free(keep);
}

static void build_role_rows(hub_t *h) {
    h->rrows.n = 0;
    bool any_chat = false;
    for (size_t r = 0; r < NROLES; r++) {
        if (ROLES[r].kind_section) continue;
        if (h->role_tab == 2) continue;
        vec_push(&h->rrows, ((rrow_t){R_ROLE, (int)r}));
        any_chat = true;
    }
    if (h->role_tab == 0 && any_chat) vec_push(&h->rrows, ((rrow_t){R_SEP, -1}));
    for (size_t r = 0; r < NROLES; r++) {
        if (!ROLES[r].kind_section || h->role_tab == 1) continue;
        vec_push(&h->rrows, ((rrow_t){R_ROLE, (int)r}));
    }
    vec_push(&h->rrows, ((rrow_t){R_NEWROLE, -1}));
    vec_push(&h->rrows, ((rrow_t){R_SEP, -1}));
    vec_push(&h->rrows, ((rrow_t){R_NEWFALLBACK, -1}));
    h->role_index = MIN(h->role_index, (int)h->rrows.n - 1);
}

/* chrome/segment-track.ts renderSegmentTrack */
static char *segment_track(const char *const *labels, size_t n, int active) {
    static const tui_tc_t CAND[] = {TUI_TC_ACCENT,        TUI_TC_SUCCESS,      TUI_TC_WARNING,       TUI_TC_ERROR,
                                    TUI_TC_MD_CODE,       TUI_TC_MD_LINK,      TUI_TC_SYNTAX_STRING, TUI_TC_SYNTAX_KEYWORD,
                                    TUI_TC_SYNTAX_FUNCTION, TUI_TC_SYNTAX_NUMBER, TUI_TC_SYNTAX_OPERATOR, TUI_TC_SYNTAX_VARIABLE};
    tui_tc_t pal[ARRAY_LEN(CAND)];
    size_t np = 0;
    for (size_t i = 0; i < ARRAY_LEN(CAND) && np < n; i++) {
        bool dup = false;
        for (size_t k = 0; k < np; k++)
            if (!strcmp(tui_fg_seq(NULL, pal[k]), tui_fg_seq(NULL, CAND[i]))) dup = true;
        if (!dup) pal[np++] = CAND[i];
    }
    if (!np) pal[np++] = TUI_TC_ACCENT;
    char *thin = S(TUI_TC_STATUS_LINE_SEP, tui_sym("sep.powerlineThin"));
    sb_t t = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            if ((int)i == active || (int)i - 1 == active) sb_append(&t, "  ");
            else sb_appendf(&t, " %s ", thin);
        }
        const char *fg = tui_fg_seq(NULL, pal[i % np]);
        if ((int)i != active) {
            sb_appendf(&t, "%s%s\x1b[39m", fg, labels[i]);
            continue;
        }
        char bg[40];
        snprintf(bg, sizeof bg, "%s", fg);
        char *p = strstr(bg, "\x1b[38;");
        if (p) p[2] = '4';
        int64_t rgb = tui_theme()->rgb[pal[i % np]];
        const char *contrast;
        if (rgb >= 0) {
            double luma = 0.299 * ((rgb >> 16) & 255) + 0.587 * ((rgb >> 8) & 255) + 0.114 * (rgb & 255);
            contrast = luma > 140 ? "\x1b[38;2;0;0;0m" : "\x1b[38;2;255;255;255m";
        } else contrast = tui_fg_seq(NULL, TUI_TC_TEXT);
        sb_appendf(&t, "%s%s%s%s\x1b[1m %s \x1b[22m\x1b[49m%s%s\x1b[39m", fg, tui_sym("sep.powerlineRight"), bg, contrast,
                   labels[i], fg, tui_sym("sep.powerlineLeft"));
    }
    free(thin);
    return sb_detach(&t);
}

static char *hints2(const char *a, const char *b) {
    char *x = ui_key_hint(a), *y = ui_key_hint(b), *r = xasprintf("%s/%s", x, y);
    free(x);
    free(y);
    return r;
}

static void status_row(hub_t *h, int width, strvec_t *out) {
    const entry_t *e = &h->entries.v[h->active];
    char *text;
    if (h->assigning >= 0) {
        char *enter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel");
        char *lab = tui_bold(ROLES[h->assigning].tag);
        char *t = xasprintf(" Assigning %s \xe2\x80\x94 %s assigns, %s cancels", lab, enter, cancel);
        strvec_push_own(out, S(TUI_TC_ACCENT, t));
        free(t);
        free(lab);
        free(enter);
        free(cancel);
        char *last = out->v[out->n - 1];
        out->v[out->n - 1] = tui_truncate_to_width(last, width, NULL);
        free(last);
        return;
    }
    if (e->kind == E_ROLES) {
        char *f = ui_key_hint("f");
        text = xasprintf(" Model roles \xe2\x80\x94 %s adds a retry fallback, cleared roles fall back to auto-selection", f);
        free(f);
    } else if (e->kind == E_PROVIDER)
        text = e->locked ? xasprintf(" %s \xc2\xb7 not configured", e->label)
                         : xasprintf(" %s \xc2\xb7 %s models", e->label, e->annotation ? e->annotation : "0");
    else text = xstrdup(" All available models");
    char *s = S(TUI_TC_MUTED, text);
    strvec_push_own(out, tui_truncate_to_width(s, width, NULL));
    free(s);
    free(text);
}

static void render_roles(hub_t *h, int width, int rows, strvec_t *out) {
    size_t before = out->n;
    static const char *const TABS[] = {"all", "chat", "kinds"};
    char *track = segment_track(TABS, 3, h->role_tab), *ak = hints2("alt+left", "alt+right");
    char *rl = S(TUI_TC_DIM, "Roles:"), *akd = S(TUI_TC_DIM, ak);
    char *line = xasprintf(" %s %s  %s", rl, track, akd);
    strvec_push_own(out, tui_truncate_to_width(line, width, NULL));
    free(line);
    free(track);
    free(ak);
    free(rl);
    free(akd);
    int start_row = (int)(out->n - before);
    int tagw = 0;
    for (size_t i = 0; i < h->rrows.n; i++)
        if (h->rrows.v[i].kind == R_ROLE) tagw = MAX(tagw, tui_visible_width(ROLES[h->rrows.v[i].role].tag));
    strvec_t order = cycle_order();
    int total = (int)h->rrows.n;
    int capacity = MAX(0, rows - 2 - start_row);
    bool overflow = total > capacity;
    int vh = overflow ? MAX(0, capacity - 1) : capacity;
    if (vh > 0) {
        if (h->role_index < h->role_scroll) h->role_scroll = h->role_index;
        else if (h->role_index >= h->role_scroll + vh) h->role_scroll = h->role_index - vh + 1;
        h->role_scroll = MAX(0, MIN(h->role_scroll, MAX(0, total - vh)));
    } else h->role_scroll = 0;
    int end = MIN(h->role_scroll + vh, total);
    for (int i = h->role_scroll; i < end; i++) {
        const rrow_t *r = &h->rrows.v[i];
        bool sel = i == h->role_index;
        char *cursor = sel && h->focus_list ? S(TUI_TC_ACCENT, tui_sym("nav.cursor")) : xstrdup(" ");
        if (r->kind == R_SEP) {
            sb_t s = SB_INIT;
            sb_repeat(&s, "\xe2\x94\x80", MAX(1, width - 6));
            char *b = S(TUI_TC_BORDER, sb_cstr(&s));
            strvec_push_own(out, xasprintf("   %s", b));
            free(b);
            sb_free(&s);
        } else if (r->kind == R_NEWROLE || r->kind == R_NEWFALLBACK) {
            char *l = S(sel ? TUI_TC_ACCENT : TUI_TC_DIM, r->kind == R_NEWROLE ? "+ New role\xe2\x80\xa6" : "+ New fallback\xe2\x80\xa6");
            char *t = xasprintf(" %s %s", cursor, l);
            strvec_push_own(out, tui_truncate_to_width(t, width, NULL));
            free(t);
            free(l);
        } else {
            const role_t *ro = &ROLES[r->role];
            assign_t a = resolve_role(h, r->role);
            char *tag = xasprintf("%-*s", tagw, ro->tag);
            char *dot, *tags, *value, *level = xstrdup("");
            if (a.model && !a.auto_selected) {
                dot = S(ro->color, tui_sym("status.enabled"));
                tags = S(ro->color, tag);
                char *pp = xasprintf("%s/", a.model->provider), *pd = S(TUI_TC_DIM, pp);
                char *id = sel ? S(TUI_TC_ACCENT, a.model->id) : xstrdup(a.model->id);
                value = xasprintf("%s%s", pd, id);
                free(pp);
                free(pd);
                free(id);
                if (a.level && strcmp(a.level, "inherit")) {
                    const char *glyph = NULL;
                    char key[48];
                    if (!strcmp(a.level, "off")) glyph = tui_sym("status.disabled");
                    else {
                        snprintf(key, sizeof key, "thinking.%s", !strcmp(a.level, "auto") ? "autoPending" : a.level);
                        glyph = tui_sym(key);
                    }
                    char *g = glyph ? xstrndup(glyph, strcspn(glyph, " ")) : xstrdup("");
                    static const struct {
                        const char *v, *l;
                    } LBL[] = {{"minimal", "min"}};
                    const char *label = a.level;
                    for (size_t k = 0; k < ARRAY_LEN(LBL); k++)
                        if (!strcmp(LBL[k].v, a.level)) label = LBL[k].l;
                    char *lv = *g ? xasprintf("%s %s", g, label) : xstrdup(label);
                    free(level);
                    level = S(TUI_TC_DIM, lv);
                    free(lv);
                    free(g);
                }
            } else if (a.model) {
                dot = S(TUI_TC_DIM, tui_sym("status.shadowed"));
                tags = S(TUI_TC_DIM, tag);
                char *t = xasprintf("auto \xe2\x86\x92 %s/%s", a.model->provider, a.model->id);
                value = S(TUI_TC_DIM, t);
                free(t);
            } else {
                dot = S(TUI_TC_DIM, tui_sym("status.shadowed"));
                tags = S(TUI_TC_DIM, tag);
                value = S(TUI_TC_DIM, "\xe2\x80\x94");
            }
            char *cyc = xstrdup("");
            for (size_t k = 0; k < order.n; k++)
                if (!strcmp(order.v[k], ro->id)) {
                    free(cyc);
                    char *c = xasprintf("%s %zu", tui_sym("icon.loop"), k + 1);
                    cyc = S(TUI_TC_ACCENT, c);
                    free(c);
                }
            char *l = xasprintf(" %s %s %s  %s", cursor, dot, tags, value);
            char *right = *level && *cyc ? xasprintf("%s  %s", level, cyc) : xasprintf("%s%s", level, cyc);
            int rw = tui_visible_width(right), lw = tui_visible_width(l);
            if (rw > 0 && lw + rw + 2 <= width) {
                char *l2 = xasprintf("%s%*s%s", l, width - lw - rw - 1, "", right);
                free(l);
                l = l2;
            }
            strvec_push_own(out, tui_truncate_to_width(l, width, NULL));
            free(l);
            free(right);
            free(cyc);
            free(level);
            free(dot);
            free(tags);
            free(value);
            free(tag);
            free(a.level);
        }
        free(cursor);
    }
    if (overflow) {
        int above = h->role_scroll, below = total - end;
        sb_t p = SB_INIT;
        sb_append(&p, "   ");
        if (above > 0) sb_appendf(&p, "\xe2\x86\x91 %d more", above);
        if (below > 0) sb_appendf(&p, "%s\xe2\x86\x93 %d more", above > 0 ? "   " : "", below);
        char *d = S(TUI_TC_DIM, sb_cstr(&p));
        strvec_push_own(out, tui_truncate_to_width(d, width, NULL));
        free(d);
        sb_free(&p);
    }
    while ((int)(out->n - before) < rows - 1) strvec_push(out, "");
    if (rows >= 2) {
        char *ck = ui_key_hint("ctrl+p");
        char *last;
        if (order.n) {
            int ai = -1;
            const rrow_t *sr = h->role_index < (int)h->rrows.n ? &h->rrows.v[h->role_index] : NULL;
            for (size_t k = 0; sr && sr->kind == R_ROLE && k < order.n; k++)
                if (!strcmp(order.v[k], ROLES[sr->role].id)) ai = (int)k;
            char *tr = segment_track((const char *const *)order.v, order.n, ai);
            char *c = xasprintf("%s cycle:", ck), *cd = S(TUI_TC_DIM, c);
            last = xasprintf("  %s %s", cd, tr);
            free(c);
            free(cd);
            free(tr);
        } else {
            char *ckk = ui_key_hint("c");
            char *t = xasprintf("  %s cycle is empty \xe2\x80\x94 press %s on a role to add it", ck, ckk);
            last = S(TUI_TC_DIM, t);
            free(t);
            free(ckk);
        }
        strvec_push_own(out, tui_truncate_to_width(last, width, NULL));
        free(last);
        free(ck);
    }
    strvec_free(&order);
}

static void render_locked(hub_t *h, const entry_t *e, int width, int rows, strvec_t *out) {
    size_t before = out->n;
    strvec_push(out, "");
    char *t = xasprintf("  %s has no credentials configured", e->label), *ts = S(TUI_TC_WARNING, t);
    strvec_push_own(out, tui_truncate_to_width(ts, width, NULL));
    free(t);
    free(ts);
    strvec_push(out, "");
    json_t *info = catalog_provider_info(e->provider);
    json_t *envs = json_get_arr(info, "envVars");
    if (envs && json_len(envs)) {
        sb_t b = SB_INIT;
        json_t *x;
        json_arr_foreach(envs, i, x) sb_appendf(&b, "%s%s", i ? " or " : "", json_sv(x) ? json_sv(x) : "");
        char *m = xasprintf("  Set %s in your environment, or add a key in config.", sb_cstr(&b)), *ms = S(TUI_TC_MUTED, m);
        strvec_push_own(out, tui_truncate_to_width(ms, width, NULL));
        free(m);
        free(ms);
        sb_free(&b);
    } else {
        char *ms = S(TUI_TC_MUTED, "  Add an API key for this provider in config.");
        strvec_push_own(out, tui_truncate_to_width(ms, width, NULL));
        free(ms);
    }
    if (e->oauth) {
        char *en = ui_key_hint("enter"), *l = xasprintf("  %s Log in with OAuth (%s)", tui_sym("nav.cursor"), en), *ls = S(TUI_TC_ACCENT, l);
        strvec_push_own(out, tui_truncate_to_width(ls, width, NULL));
        free(en);
        free(l);
        free(ls);
    }
    strvec_push(out, "");
    if (e->catalog_count > 0) {
        char *c = xasprintf("  %d models in catalog:", e->catalog_count), *cs = S(TUI_TC_DIM, c);
        strvec_push_own(out, tui_truncate_to_width(cs, width, NULL));
        free(c);
        free(cs);
        for (size_t i = 0; i < catalog_count() && (int)(out->n - before) < rows; i++) {
            const ai_model_t *m = catalog_at(i);
            if (strcmp(m->provider, e->provider)) continue;
            char *l = xasprintf("    %s", m->id), *ls = S(TUI_TC_DIM, l);
            strvec_push_own(out, tui_truncate_to_width(ls, width, NULL));
            free(l);
            free(ls);
        }
    }
    while ((int)(out->n - before) < rows) strvec_push(out, "");
    while ((int)(out->n - before) > rows) free(out->v[--out->n]);
}

static void render_body(hub_t *h, int width, int rows, strvec_t *out) {
    size_t before = out->n;
    status_row(h, width, out);
    const entry_t *e = &h->entries.v[h->active];
    if (e->kind == E_ROLES && h->assigning < 0) {
        render_roles(h, width, rows - 1, out);
    } else if (e->kind == E_PROVIDER && e->locked && h->assigning < 0) {
        render_locked(h, e, width, rows - 1, out);
    } else {
        char *track = segment_track(KINDS, ARRAY_LEN(KINDS), h->kind_tab), *ak = hints2("alt+left", "alt+right");
        char *kl = S(TUI_TC_DIM, "Kind:"), *akd = S(TUI_TC_DIM, ak);
        char *line = xasprintf(" %s %s  %s", kl, track, akd);
        strvec_push_own(out, tui_truncate_to_width(line, width, NULL));
        free(line);
        free(track);
        free(ak);
        free(kl);
        free(akd);
        ui_model_browser_set_max_visible(h->b, rows - 2 - 5);
        h->b->focused = h->focus_list;
        ui_model_browser_render(h->b, width, out);
    }
    while ((int)(out->n - before) < rows) strvec_push(out, "");
    while ((int)(out->n - before) > rows) free(out->v[--out->n]);
}

/* ---------- footer ---------- */

static char *footer_hint(hub_t *h) {
    char *enter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel"), *ud = hints2("up", "down");
    char *left = ui_key_hint("left"), *lr = hints2("left", "right"), *er = hints2("enter", "right");
    char *alr = hints2("alt+left", "alt+right");
    char *r;
    const entry_t *e = &h->entries.v[h->active];
    if (h->strip) {
        if (h->strip_kind == 0) r = xasprintf("%s choose \xc2\xb7 %s assign/clear \xc2\xb7 %s cancel", lr, enter, cancel);
        else r = xasprintf("%s thinking level \xc2\xb7 %s apply \xc2\xb7 %s keep", lr, enter, cancel);
    } else if (h->assigning >= 0) {
        if (!h->focus_list)
            r = xasprintf("%s models \xc2\xb7 %s providers \xc2\xb7 type to search \xc2\xb7 %s kind \xc2\xb7 %s cancel", er, ud, alr, cancel);
        else
            r = xasprintf("%s assign \xc2\xb7 %s models \xc2\xb7 %s providers \xc2\xb7 type to search \xc2\xb7 %s kind \xc2\xb7 %s cancel", enter, ud,
                          left, alr, cancel);
    } else if (e->kind == E_ROLES) {
        if (!h->focus_list) r = xasprintf("%s providers \xc2\xb7 %s roles \xc2\xb7 %s tabs \xc2\xb7 %s close", ud, er, alr, cancel);
        else {
            char *f = ui_key_hint("f"), *x = ui_key_hint("x"), *k = ui_key_hint("k"), *c = ui_key_hint("c"), *n = ui_key_hint("n"),
                 *s = ui_key_hint("s"), *t = ui_key_hint("t");
            const rrow_t *row = h->role_index < (int)h->rrows.n ? &h->rrows.v[h->role_index] : NULL;
            if (row && row->kind == R_NEWFALLBACK) r = xasprintf("%s rows \xc2\xb7 %s new model/provider fallback chain \xc2\xb7 %s providers", ud, enter, left);
            else {
                char *thinking = xstrdup("");
                char *compaction = xstrdup("");
                if (row && row->kind == R_ROLE) {
                    assign_t a = resolve_role(h, row->role);
                    if (a.model && !a.auto_selected) {
                        free(compaction);
                        compaction = xasprintf(" \xc2\xb7 %s compaction limit", k);
                        if (a.model->reasoning) {
                            free(thinking);
                            thinking = xasprintf(" \xc2\xb7 %s thinking", t);
                        }
                    }
                    free(a.level);
                }
                r = xasprintf("%s rows \xc2\xb7 %s pick \xc2\xb7 %s fallback \xc2\xb7 %s clear%s%s \xc2\xb7 %s cycle \xc2\xb7 [/] reorder \xc2\xb7 %s new \xc2\xb7 %s save preset", ud,
                              enter, f, x, thinking, compaction, c, n, s);
                free(thinking);
                free(compaction);
            }
            free(f);
            free(x);
            free(k);
            free(c);
            free(n);
            free(s);
            free(t);
        }
    } else if (e->kind == E_PROVIDER && e->locked) {
        r = e->oauth ? xasprintf("%s log in \xc2\xb7 %s providers \xc2\xb7 %s close", enter, ud, cancel) : xasprintf("%s providers \xc2\xb7 %s close", ud, cancel);
    } else {
        char *refresh = e->kind == E_PROVIDER ? ({
            char *f5 = ui_key_hint("f5"), *x = xasprintf(" \xc2\xb7 %s refresh", f5);
            free(f5);
            x;
        })
                                              : xstrdup("");
        if (!h->focus_list)
            r = xasprintf("%s models \xc2\xb7 %s providers \xc2\xb7 type to search \xc2\xb7 %s kind%s \xc2\xb7 %s close", er, ud, alr, refresh, cancel);
        else
            r = xasprintf("%s assign roles \xc2\xb7 %s models \xc2\xb7 %s providers \xc2\xb7 type to search \xc2\xb7 %s kind%s \xc2\xb7 %s close", enter, ud, left,
                          alr, refresh, cancel);
        free(refresh);
    }
    free(enter);
    free(cancel);
    free(ud);
    free(left);
    free(lr);
    free(er);
    free(alr);
    return r;
}

static char *render_strip(hub_t *h, int width) {
    char *prefix;
    if (h->strip_kind == 0) {
        char *a = S(TUI_TC_ACCENT, h->strip_model->id), *d = S(TUI_TC_DIM, " \xe2\x86\x92");
        prefix = xasprintf("%s%s ", a, d);
        free(a);
        free(d);
    } else {
        char *low = str_lower(ROLES[h->strip_role].tag);
        char *a = S(ROLES[h->strip_role].color, low), *t = xasprintf(" \xc2\xb7 %s \xe2\x86\x92", h->strip_model->id), *d = S(TUI_TC_DIM, t);
        prefix = xasprintf("%s%s ", a, d);
        free(low);
        free(a);
        free(t);
        free(d);
    }
    int avail = MAX(1, width - tui_visible_width(prefix));
    size_t n = h->chips.n;
    int *cw = xcalloc(n + 1, sizeof *cw);
    for (size_t i = 0; i < n; i++) {
        char *b = xasprintf(" %s ", h->chips.v[i].styled);
        cw[i] = tui_visible_width(b) + (i == h->chip_index ? 2 : 0) + 1;
        free(b);
    }
#define START_FOR(target, out)                                                                                         \
    do {                                                                                                               \
        size_t s_ = 0;                                                                                                 \
        while (s_ < (target)) {                                                                                        \
            int sum_ = s_ > 0 ? 2 : 0;                                                                                 \
            for (size_t i_ = s_; i_ <= (target); i_++) sum_ += cw[i_];                                                 \
            if (sum_ <= avail) break;                                                                                  \
            s_++;                                                                                                      \
        }                                                                                                              \
        (out) = s_;                                                                                                    \
    } while (0)
    size_t start;
    START_FOR(MIN(h->chip_index + 1, n ? n - 1 : 0), start);
    if (start > h->chip_index) START_FOR(h->chip_index, start);
#undef START_FOR
    free(cw);
    sb_t line = SB_INIT;
    sb_append(&line, prefix);
    if (start > 0) {
        char *d = S(TUI_TC_DIM, "\xe2\x80\xa6 ");
        sb_append(&line, d);
        free(d);
    }
    for (size_t i = start; i < n; i++) {
        char *body = xasprintf(" %s ", h->chips.v[i].styled);
        if (i == h->chip_index) {
            char *lb = S(TUI_TC_ACCENT, "["), *rb = S(TUI_TC_ACCENT, "]");
            char *in = xasprintf("%s%s%s", lb, body, rb), *bg = tui_bgs(NULL, TUI_TC_SELECTED_BG, in);
            sb_append(&line, bg);
            free(lb);
            free(rb);
            free(in);
            free(bg);
        } else sb_append(&line, body);
        sb_putc(&line, ' ');
        free(body);
    }
    char *r = tui_truncate_to_width(sb_cstr(&line), width, NULL);
    sb_free(&line);
    free(prefix);
    return r;
}

/* ---------- frame (hub-frame.ts) ---------- */

static void paint(sb_t *b, const char *s) { tui_fg(b, NULL, TUI_TC_BORDER, s); }

static void hub_render(tui_component_t *c, int width, strvec_t *out) {
    hub_t *h = (hub_t *)c;
    int height = MAX(16, tui_term(h->u->tui)->rows > 0 ? tui_term(h->u->tui)->rows : 40);
    int content = MAX(10, height - 4);
    int L = sidebar_width(h);
    /* SplitPane: "│ " left " │ " right " │" (left preserved, body floor 28) */
    int R = width - 7 - L;
    if (R < 28) {
        L = MAX(18, width - 7 - 28);
        R = width - 7 - L;
    }
    if (R < 0) R = 0;
    sidebar_rows_cache = content;
    const char *H = tui_sym("boxRound.horizontal");
    int dcol = L + 3, left_len = MAX(0, dcol - 1), right_len = MAX(0, width - 2 - dcol);
    /* top border */
    sb_t t = SB_INIT, r = SB_INIT;
    char *shown = tui_truncate_to_width(" Models ", MAX(0, left_len - 1), NULL);
    int fill = MAX(0, left_len - 1 - tui_visible_width(shown));
    sb_appendf(&r, "%s%s", tui_sym("boxRound.topLeft"), H);
    paint(&t, sb_cstr(&r));
    sb_clear(&r);
    char *acc = S(TUI_TC_ACCENT, shown), *bold = tui_bold(acc);
    sb_append(&t, bold);
    free(acc);
    free(bold);
    free(shown);
    sb_repeat(&r, H, fill);
    sb_append(&r, tui_sym("boxSharp.teeDown"));
    sb_repeat(&r, H, right_len);
    sb_append(&r, tui_sym("boxRound.topRight"));
    paint(&t, sb_cstr(&r));
    strvec_push_own(out, sb_detach(&t));
    sb_free(&r);
    /* split rows */
    strvec_t side = {0}, body = {0};
    render_sidebar(h, L, content, &side);
    render_body(h, R, content, &body);
    char *bar = S(TUI_TC_BORDER, tui_sym("boxRound.vertical"));
    for (int i = 0; i < content; i++) {
        char *sl = ui_fit_line(i < (int)side.n ? side.v[i] : "", L);
        char *bl = ui_fit_line(i < (int)body.n ? body.v[i] : "", R);
        strvec_push_own(out, xasprintf("%s %s %s %s %s", bar, sl, bar, bl, bar));
        free(sl);
        free(bl);
    }
    strvec_free(&side);
    strvec_free(&body);
    /* divider */
    sb_t d = SB_INIT, dd = SB_INIT;
    sb_append(&d, tui_sym("boxSharp.teeRight"));
    sb_repeat(&d, H, left_len);
    sb_append(&d, tui_sym("boxSharp.teeUp"));
    sb_repeat(&d, H, right_len);
    sb_append(&d, tui_sym("boxSharp.teeLeft"));
    paint(&dd, sb_cstr(&d));
    strvec_push_own(out, sb_detach(&dd));
    sb_free(&d);
    /* footer */
    char *foot;
    if (h->strip) foot = render_strip(h, width - 4);
    else {
        char *f = footer_hint(h), *fd = S(TUI_TC_DIM, f);
        foot = tui_truncate_to_width(fd, width - 4, NULL);
        free(f);
        free(fd);
    }
    strvec_push_own(out, ui_box_row(foot, width));
    free(foot);
    strvec_push_own(out, ui_box_bottom(width));
    free(bar);
}

/* ---------- actions ---------- */

static void close_hub(hub_t *h) {
    ui_t *u = h->u;
    tui_overlay_hide(u->tui, &h->base);
    tui_term_puts(tui_term(u->tui), "\x1b[?1049l");
    tui_set_focus(u->tui, &u->editor->base);
    tui_request_full_render(u->tui);
    tui_component_destroy(&h->base);
}

static void chips_free(hub_t *h) {
    for (size_t i = 0; i < h->chips.n; i++) {
        free(h->chips.v[i].label);
        free(h->chips.v[i].styled);
        free(h->chips.v[i].level);
    }
    h->chips.n = 0;
}

static void close_strip(hub_t *h) {
    bool back = h->strip && h->strip_kind == 1 && h->strip_return;
    h->strip = false;
    chips_free(h);
    free(h->strip_selector);
    h->strip_selector = NULL;
    free(h->strip_initial);
    h->strip_initial = NULL;
    if (back) {
        for (size_t i = 0; i < h->entries.n; i++)
            if (h->entries.v[i].kind == E_ROLES) h->active = i;
        h->focus_list = true;
    }
}

static void push_chip(hub_t *h, const char *label, char *styled, int action, int role, const char *level) {
    chip_t c = {xstrdup(label), styled, action, role, level ? xstrdup(level) : NULL};
    vec_push(&h->chips, c);
}

static void refresh_after_mutation(hub_t *h) {
    build_sidebar(h);
    build_role_rows(h);
    rebuild_browser(h);
}

/* onAssign: persist modelRoles.<role> ("provider/id[:level]"); default also switches the session model */
static void assign_role(hub_t *h, const ai_model_t *m, int role, const char *level) {
    char *sel = xasprintf("%s/%s", m->provider, m->id);
    bool concrete = level && strcmp(level, "inherit") && strcmp(level, "auto");
    bool is_default = !strcmp(ROLES[role].id, "default");
    char *val = (level && strcmp(level, "inherit") && !(is_default && !strcmp(level, "auto"))) ? xasprintf("%s:%s", sel, level) : xstrdup(sel);
    char *path = xasprintf("modelRoles.%s", ROLES[role].id);
    settings_set_global(path, json_str(val));
    if (is_default) {
        if (level && !strcmp(level, "auto")) settings_set_global("defaultThinkingLevel", json_str("auto"));
        char *err = NULL;
        if (app_set_model(h->u->app, sel, &err) != 0) add_notice(h->u, "error", err);
        else if (concrete) {
            ai_effort_t e;
            if (effort_parse(level, &e) == 0) app_set_thinking(h->u->app, e);
        }
        free(err);
    }
    free(path);
    free(val);
    free(sel);
}

static void thinking_options(const ai_model_t *m, strvec_t *out) {
    if (!m->reasoning) return;
    strvec_push(out, "inherit");
    strvec_push(out, "off");
    strvec_push(out, "auto");
    for (int i = EFFORT_MINIMAL; i <= EFFORT_MAX; i++)
        if (model_supports_effort(m, (ai_effort_t)i)) strvec_push(out, effort_name((ai_effort_t)i));
}

static void open_thinking_strip(hub_t *h, const ai_model_t *m, int role, bool back, const char *committed) {
    strvec_t opts = {0};
    thinking_options(m, &opts);
    if (!opts.n) {
        strvec_free(&opts);
        if (back) {
            for (size_t i = 0; i < h->entries.n; i++)
                if (h->entries.v[i].kind == E_ROLES) h->active = i;
            h->focus_list = true;
        }
        return;
    }
    chips_free(h);
    size_t pre = 0;
    for (size_t i = 0; i < opts.n; i++) {
        const char *lv = opts.v[i];
        const char *label = !strcmp(lv, "minimal") ? "min" : lv;
        char key[48];
        const char *glyph = NULL;
        if (!strcmp(lv, "off")) glyph = tui_sym("status.disabled");
        else if (strcmp(lv, "inherit")) {
            snprintf(key, sizeof key, "thinking.%s", !strcmp(lv, "auto") ? "autoPending" : lv);
            glyph = tui_sym(key);
        }
        char *g = glyph ? xstrndup(glyph, strcspn(glyph, " ")) : xstrdup("");
        char *styled;
        if (*g) {
            char *ga = S(TUI_TC_ACCENT, g);
            styled = xasprintf("%s %s", ga, label);
            free(ga);
        } else styled = xstrdup(label);
        free(g);
        push_chip(h, label, styled, 2, role, lv);
        if (committed && !strcmp(committed, lv)) pre = i;
    }
    strvec_free(&opts);
    h->strip = true;
    h->strip_kind = 1;
    h->chip_index = pre;
    h->strip_model = m;
    h->strip_role = role;
    h->strip_return = back;
    free(h->strip_initial);
    h->strip_initial = xstrdup(committed ? committed : "inherit");
}

static void do_assign(hub_t *h, const ai_model_t *m, int role, bool back) {
    assign_t cur = resolve_role(h, role);
    const char *level = cur.model && !cur.auto_selected && cur.level ? cur.level : "inherit";
    strvec_t opts = {0};
    thinking_options(m, &opts);
    if (!strvec_contains(&opts, level)) level = "inherit";
    char *lv = xstrdup(level);
    assign_role(h, m, role, lv);
    refresh_after_mutation(h);
    open_thinking_strip(h, m, role, back, lv);
    free(lv);
    strvec_free(&opts);
    free(cur.level);
}

static void open_role_strip(hub_t *h, const ai_model_t *m) {
    chips_free(h);
    for (size_t r = 0; r < NROLES; r++) {
        if (!role_accepts(&ROLES[r], m)) continue;
        assign_t a = resolve_role(h, (int)r);
        bool here = a.model && !a.auto_selected && a.model == m;
        free(a.level);
        char *label = str_lower(ROLES[r].tag);
        char *styled;
        if (here) {
            char *t = xasprintf("%s %s", tui_sym("status.enabled"), label), *c = S(ROLES[r].color, t);
            char *s2 = xasprintf(" %s", tui_sym("status.success")), *d = S(TUI_TC_DIM, s2);
            styled = xasprintf("%s%s", c, d);
            free(t);
            free(c);
            free(s2);
            free(d);
        } else styled = S(ROLES[r].color, label);
        push_chip(h, label, styled, here ? 1 : 0, (int)r, NULL);
        free(label);
    }
    char *fm = xasprintf("fallbacks:%s", m->id), *fp = xasprintf("fallbacks:%s/*", m->provider);
    push_chip(h, fm, S(TUI_TC_MUTED, fm), 3, -1, NULL);
    push_chip(h, fp, S(TUI_TC_MUTED, fp), 3, -1, NULL);
    free(fm);
    free(fp);
    if (!strcmp(model_kind(m), "chat")) push_chip(h, "fallback", S(TUI_TC_MUTED, "retry-fallback"), 3, -1, NULL);
    h->strip = true;
    h->strip_kind = 0;
    h->chip_index = 0;
    h->strip_model = m;
    h->strip_return = false;
}

static void activate_chip(hub_t *h) {
    if (h->chip_index >= h->chips.n) return;
    chip_t *c = &h->chips.v[h->chip_index];
    const ai_model_t *m = h->strip_model;
    if (c->action == 0) {
        int role = c->role;
        h->strip = false;
        chips_free(h);
        do_assign(h, m, role, false);
    } else if (c->action == 1) {
        char *path = xasprintf("modelRoles.%s", ROLES[c->role].id);
        settings_unset_global(path);
        free(path);
        close_strip(h);
        refresh_after_mutation(h);
    } else if (c->action == 2) {
        bool changed = strcmp(c->level, h->strip_initial ? h->strip_initial : "inherit") != 0;
        int role = c->role;
        char *lv = xstrdup(c->level);
        close_strip(h);
        if (changed) {
            assign_role(h, m, role, lv);
            refresh_after_mutation(h);
        }
        free(lv);
    } else {
        close_strip(h); /* retry-fallback chains are not ported */
    }
}

static void move_sidebar(hub_t *h, int delta) {
    size_t n = h->entries.n;
    size_t i = h->active;
    for (size_t step = 0; step < n; step++) {
        i = (i + n + (size_t)(delta < 0 ? n - 1 : 1)) % n;
        if (hop_skipped(h, &h->entries.v[i])) continue;
        if (h->entries.v[i].kind == E_ROLES) h->assigning = -1;
        h->active = i;
        h->follow = true;
        rebuild_browser(h);
        return;
    }
}

static void select_all_entry(hub_t *h) {
    for (size_t i = 0; i < h->entries.n; i++)
        if (h->entries.v[i].kind == E_ALL) h->active = i;
    h->follow = true;
    rebuild_browser(h);
}

static bool printable(const tui_key_t *k) {
    const char *t = tui_key_text(k);
    return k->kind == TUI_KEY_CHAR && t && *t && !(k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT | TUI_MOD_SUPER)) && !isspace((unsigned char)*t);
}

static void roles_input(hub_t *h, const tui_key_t *k) {
    if (!h->focus_list) {
        if (tui_key_matches(k, "enter") || tui_key_matches(k, "space")) h->focus_list = true;
        return;
    }
    int n = (int)h->rrows.n;
    if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) {
        int d = tui_kb(k, "tui.select.up") ? -1 : 1, idx = h->role_index;
        for (int i = 0; i < n; i++) {
            idx = (idx + d + n) % n;
            if (h->rrows.v[idx].kind != R_SEP) break;
        }
        h->role_index = idx;
        return;
    }
    const rrow_t *row = h->role_index < n ? &h->rrows.v[h->role_index] : NULL;
    int role = row && row->kind == R_ROLE ? row->role : -1;
    const char *txt = tui_key_text(k);
    char ch = k->kind == TUI_KEY_CHAR && txt && strlen(txt) == 1 ? txt[0] : 0;
    if (tui_key_matches(k, "enter")) {
        if (role >= 0) { /* startAssign: the full catalog, cleared query, current model preselected */
            h->assigning = role;
            h->focus_list = true;
            free(h->b->query);
            h->b->query = NULL;
            rebuild_browser(h);
            assign_t a = resolve_role(h, role);
            if (a.model) {
                char *s = xasprintf("%s/%s", a.model->provider, a.model->id);
                ui_model_browser_set_current(h->b, s);
                free(h->b->current);
                h->b->current = NULL;
                free(s);
            }
            free(a.level);
        }
    } else if (tui_key_matches(k, "backspace") || tui_key_matches(k, "delete") || ch == 'x') {
        if (role >= 0) {
            char *path = xasprintf("modelRoles.%s", ROLES[role].id);
            if (settings_str(path, NULL)) settings_unset_global(path);
            free(path);
            refresh_after_mutation(h);
        }
    } else if (ch == 'c' || ch == '[' || ch == ']' || tui_key_matches(k, "shift+up") || tui_key_matches(k, "shift+down")) {
        if (role < 0) return;
        strvec_t order = cycle_order();
        long idx = -1;
        for (size_t i = 0; i < order.n; i++)
            if (!strcmp(order.v[i], ROLES[role].id)) idx = (long)i;
        if (ch == 'c') {
            if (idx >= 0) {
                free(order.v[idx]);
                memmove(&order.v[idx], &order.v[idx + 1], (order.n - (size_t)idx - 1) * sizeof *order.v);
                order.n--;
            } else strvec_push(&order, ROLES[role].id);
        } else {
            long to = idx + ((ch == '[' || tui_key_matches(k, "shift+up")) ? -1 : 1);
            if (idx < 0 || to < 0 || to >= (long)order.n) {
                strvec_free(&order);
                return;
            }
            char *tmp = order.v[idx];
            order.v[idx] = order.v[to];
            order.v[to] = tmp;
        }
        json_t *arr = json_arr();
        for (size_t i = 0; i < order.n; i++) json_push_str(arr, order.v[i]);
        settings_set_global("cycleOrder", arr);
        strvec_free(&order);
        refresh_after_mutation(h);
    } else if (ch == 't' && role >= 0) {
        assign_t a = resolve_role(h, role);
        if (a.model && !a.auto_selected && a.model->reasoning) open_thinking_strip(h, a.model, role, true, a.level ? a.level : "inherit");
        free(a.level);
    }
}

static bool hub_key(tui_component_t *c, const tui_key_t *k) {
    hub_t *h = (hub_t *)c;
    tui_component_request_render(c);
    if (h->strip) {
        if (tui_kb(k, "tui.select.cancel")) close_strip(h);
        else if (tui_key_matches(k, "left") || tui_key_matches(k, "shift+tab")) h->chip_index = (h->chip_index + h->chips.n - 1) % MAX(1, h->chips.n);
        else if (tui_key_matches(k, "right") || tui_key_matches(k, "tab")) h->chip_index = (h->chip_index + 1) % MAX(1, h->chips.n);
        else if (tui_key_matches(k, "enter")) activate_chip(h);
        return true;
    }
    entry_t *e = &h->entries.v[h->active];
    bool roles_view = e->kind == E_ROLES && h->assigning < 0;
    bool locked_view = e->kind == E_PROVIDER && e->locked && h->assigning < 0;
    if (tui_kb(k, "tui.select.cancel")) {
        if (h->assigning >= 0) {
            h->assigning = -1;
            for (size_t i = 0; i < h->entries.n; i++)
                if (h->entries.v[i].kind == E_ROLES) h->active = i;
            h->focus_list = true;
            rebuild_browser(h);
        } else if (is_browser_view(h) && h->b->query && *h->b->query) {
            ui_model_browser_key(h->b, k);
            build_sidebar(h);
        } else close_hub(h);
        return true;
    }
    if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab")) {
        h->focus_list = !h->focus_list;
        return true;
    }
    if (tui_key_matches(k, "alt+left") || tui_key_matches(k, "alt+b") || tui_key_matches(k, "alt+right") || tui_key_matches(k, "alt+f")) {
        int d = tui_key_matches(k, "alt+left") || tui_key_matches(k, "alt+b") ? -1 : 1;
        if (roles_view) {
            h->role_tab = (h->role_tab + 3 + d) % 3;
            h->role_index = 0;
            build_role_rows(h);
        } else {
            int n = (int)ARRAY_LEN(KINDS);
            h->kind_tab = (h->kind_tab + n + d) % n;
            rebuild_browser(h);
        }
        return true;
    }
    if (tui_key_matches(k, "left")) {
        h->focus_list = false;
        return true;
    }
    if (tui_key_matches(k, "right")) {
        if (roles_view || is_browser_view(h)) h->focus_list = true;
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
    }
    if (roles_view) {
        bool edits = h->b->query && *h->b->query && (printable(k) || tui_key_matches(k, "backspace"));
        if ((!h->focus_list && printable(k)) || edits) {
            select_all_entry(h);
            h->focus_list = true;
            ui_model_browser_key(h->b, k);
            build_sidebar(h);
            return true;
        }
        roles_input(h, k);
        return true;
    }
    if (locked_view) {
        if (printable(k)) {
            select_all_entry(h);
            h->focus_list = true;
            ui_model_browser_key(h->b, k);
            build_sidebar(h);
        } else if (tui_key_matches(k, "enter") && e->oauth) {
            char *prov = xstrdup(e->provider);
            ui_t *u = h->u;
            close_hub(h);
            ui_overlay_login(u, false);
            free(prov);
        }
        return true;
    }
    if (!h->focus_list && tui_key_matches(k, "enter")) {
        h->focus_list = true;
        return true;
    }
    char *before = xstrdup(h->b->query ? h->b->query : "");
    int r = ui_model_browser_key(h->b, k);
    bool changed = strcmp(before, h->b->query ? h->b->query : "") != 0;
    if (printable(k) || changed) h->focus_list = true;
    if (changed) build_sidebar(h);
    free(before);
    if (r == UI_MB_ACTIVATE) {
        const ui_model_item_t *it = ui_model_browser_selected(h->b);
        if (!it) return true;
        if (h->assigning >= 0) {
            int role = h->assigning;
            h->assigning = -1;
            do_assign(h, it->model, role, true);
            if (!h->strip) { /* no thinking ladder: back to the roles view */
                for (size_t i = 0; i < h->entries.n; i++)
                    if (h->entries.v[i].kind == E_ROLES) h->active = i;
                h->focus_list = true;
            }
        } else open_role_strip(h, it->model);
    }
    return true;
}

static void hub_destroy(tui_component_t *c) {
    hub_t *h = (hub_t *)c;
    entries_free(h);
    vec_free(&h->entries);
    ui_model_browser_free(h->b);
    vec_free(&h->available);
    vec_free(&h->rrows);
    chips_free(h);
    vec_free(&h->chips);
    free(h->strip_selector);
    free(h->strip_initial);
    free(h);
}

static const tui_component_vt HUB_VT = {"model-hub", hub_render, hub_key, NULL, hub_destroy};

void ui_overlay_model(ui_t *u) {
    hub_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &HUB_VT);
    h->u = u;
    h->assigning = -1;
    h->follow = true;
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (auth_has_credentials(m->provider) || catalog_provider_keyless(m->provider)) vec_push(&h->available, m);
    }
    build_sidebar(h);
    build_role_rows(h);
    rebuild_browser(h);
    h->b->focused = false;
    (void)sidebar_rows_cache;
    tui_term_puts(tui_term(u->tui), "\x1b[?1049h");
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &h->base, &o);
    tui_set_focus(u->tui, &h->base);
    tui_request_full_render(u->tui);
}
