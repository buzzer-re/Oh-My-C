/* move.c - the `/move` overlay (port of omp packages/tui/src/overlays/move-overlay.ts with the filesystem source of
 * coding-agent/src/modes/move-directory-source.ts): a `Path: ` input over a live list of matching directories.
 * Tab fills the input with the highlighted directory, Enter confirms the highlighted one (else the typed path),
 * Esc cancels. */
#include "ui/ui.h"

#include "core/fs.h"

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define MOVE_MAX_RESULTS 15

typedef struct {
    tui_component_t base;
    ui_input_t in;
    char *cwd;
    strvec_t values, labels;
    int sel;
    bool finished;
    void (*done)(void *ud, const char *dir);
    void *ud;
} move_ov_t;

/* resolveMovePath: ~, absolute, or relative to cwd */
static char *resolve_move_path(const char *input, const char *cwd) {
    char *t = str_trim(input);
    char *r;
    if (!strcmp(t, "~")) r = xstrdup(home_dir());
    else if (str_starts(t, "~/")) r = path_join(home_dir(), t + 2);
    else if (t[0] == '/') r = path_normalize(t);
    else r = path_abs(t, cwd);
    free(t);
    return r;
}

/* Dirent.isDirectory, falling back to stat for symlinks and unknown entry types */
static bool entry_is_dir(const char *dir, const struct dirent *e) {
    if (e->d_type == DT_DIR) return true;
    if (e->d_type != DT_LNK && e->d_type != DT_UNKNOWN) return false;
    char *p = path_join(dir, e->d_name);
    bool d = fs_is_dir(p);
    free(p);
    return d;
}

/* String.prototype.localeCompare (ICU root collation, approximated): punctuation < digits < letters, case folded
 * first, lowercase before uppercase on ties */
static int coll_class(unsigned char c) { return isdigit(c) ? 1 : isalpha(c) ? 2 : c >= 0x80 ? 3 : 0; }
static int locale_cmp(const char *a, const char *b) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (; *x && *y; x++, y++) {
        int cx = coll_class(*x), cy = coll_class(*y);
        if (cx != cy) return cx - cy;
        int lx = tolower(*x), ly = tolower(*y);
        if (lx != ly) return lx - ly;
    }
    if (*x || *y) return *x ? 1 : -1;
    for (x = (const unsigned char *)a, y = (const unsigned char *)b; *x && *y; x++, y++)
        if (*x != *y) return islower(*x) ? -1 : 1;
    return 0;
}

static void push_sorted(move_ov_t *m, const char *value, const char *label, bool sort) {
    size_t at = m->labels.n;
    if (sort)
        while (at > 0 && locale_cmp(m->labels.v[at - 1], label) > 0) at--;
    strvec_push(&m->values, value);
    strvec_push(&m->labels, label);
    for (size_t i = m->labels.n - 1; i > at; i--) {
        char *t = m->values.v[i];
        m->values.v[i] = m->values.v[i - 1];
        m->values.v[i - 1] = t;
        t = m->labels.v[i];
        m->labels.v[i] = m->labels.v[i - 1];
        m->labels.v[i - 1] = t;
    }
}

/* listChildDirectories: the first `max` visible child directories (readdir order), sorted by label */
static void list_children(move_ov_t *m, const char *dir, size_t max, const char *query, bool hidden, bool sort) {
    DIR *d = opendir(dir);
    if (!d) return;
    char *lower = query ? str_lower(query) : NULL;
    struct dirent *e;
    while ((e = readdir(d)) && m->values.n < max) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!hidden && e->d_name[0] == '.') continue;
        if (lower && *lower) {
            char *ln = str_lower(e->d_name);
            bool hit = strstr(ln, lower) != NULL;
            free(ln);
            if (!hit) continue;
        }
        if (!entry_is_dir(dir, e)) continue;
        char *v = path_join(dir, e->d_name), *l = xasprintf("%s/", e->d_name);
        push_sorted(m, v, l, sort);
        free(v);
        free(l);
    }
    free(lower);
    closedir(d);
}

/* searchDirectories */
static void update_results(move_ov_t *m) {
    strvec_free(&m->values);
    strvec_free(&m->labels);
    memset(&m->values, 0, sizeof m->values);
    memset(&m->labels, 0, sizeof m->labels);
    const char *prefix = ui_input_value(&m->in);
    size_t max = MOVE_MAX_RESULTS + 5;
    if (!*prefix) list_children(m, m->cwd, max, NULL, false, true);
    else {
        const char *slash = strrchr(prefix, '/');
        char *base_dir, *query;
        if (!slash) {
            base_dir = xstrdup(m->cwd);
            query = xstrdup(prefix);
        } else {
            char *base = xstrndup(prefix, (size_t)(slash - prefix) + 1);
            base_dir = resolve_move_path(base, m->cwd);
            free(base);
            query = xstrdup(slash + 1);
        }
        bool hidden = query[0] == '.';
        char *resolved = hidden ? NULL : resolve_move_path(prefix, m->cwd);
        if (resolved && fs_is_dir(resolved)) list_children(m, resolved, max, NULL, false, true);
        else list_children(m, base_dir, max, query, hidden, false);
        free(resolved);
        free(base_dir);
        free(query);
    }
    if (m->sel >= (int)m->values.n) m->sel = MAX(0, (int)m->values.n - 1);
}

static void move_render(tui_component_t *c, int width, strvec_t *out) {
    move_ov_t *m = (move_ov_t *)c;
    int inner = MAX(1, width - 4);
    char *prompt = tui_fgs(NULL, TUI_TC_DIM, "Path: ");
    char *field = ui_input_render(&m->in, prompt, inner, c->focused);
    strvec_push_own(out, ui_box_top(width, "Move to directory"));
    strvec_push_own(out, ui_box_row(field, width));
    strvec_push_own(out, ui_box_row("", width));
    if (!m->values.n && *ui_input_value(&m->in)) {
        char *d = tui_fgs(NULL, TUI_TC_DIM, "No matching directories");
        strvec_push_own(out, ui_box_row(d, width));
        free(d);
    } else {
        for (size_t i = 0; i < m->values.n && i < MOVE_MAX_RESULTS; i++) {
            bool selected = (int)i == m->sel;
            char *marker = selected ? tui_fgs(NULL, TUI_TC_ACCENT, "\xe2\x96\xb6 ") : xstrdup("  ");
            char *label = tui_fgs(NULL, selected ? TUI_TC_ACCENT : TUI_TC_TEXT, m->labels.v[i]);
            char *row = xasprintf("%s%s", marker, label);
            strvec_push_own(out, ui_box_row(row, width));
            free(marker);
            free(label);
            free(row);
        }
    }
    strvec_push_own(out, ui_box_row("", width));
    char *up = ui_editor_key("tui.select.up"), *down = ui_editor_key("tui.select.down");
    char *tab = ui_key_hint("tab"), *enter = ui_key_hint("enter"), *esc = ui_editor_key("tui.select.cancel");
    char *hint = xasprintf("Type to filter \xc2\xb7 %s/%s navigate \xc2\xb7 %s accept \xc2\xb7 %s confirm \xc2\xb7 %s cancel", up, down, tab,
                           enter, esc);
    char *dh = tui_fgs(NULL, TUI_TC_DIM, hint);
    strvec_push_own(out, ui_box_row(dh, width));
    strvec_push_own(out, ui_box_bottom(width));
    free(dh), free(hint), free(up), free(down), free(tab), free(enter), free(esc), free(field), free(prompt);
}

static void move_finish(move_ov_t *m, const char *dir) {
    if (m->finished) return;
    m->finished = true;
    m->done(m->ud, dir);
}

static bool move_key(tui_component_t *c, const tui_key_t *k) {
    move_ov_t *m = (move_ov_t *)c;
    if (m->finished) return true;
    if (tui_kb(k, "tui.select.cancel") || tui_key_matches(k, "escape") || tui_key_matches(k, "ctrl+c")) {
        move_finish(m, NULL);
        return true;
    }
    if (tui_key_matches(k, "enter") || tui_key_matches(k, "return")) { /* #confirm */
        if (m->values.n) move_finish(m, m->values.v[m->sel]);
        else {
            char *t = str_trim(ui_input_value(&m->in));
            move_finish(m, *t ? t : NULL);
            free(t);
        }
        return true;
    }
    if (tui_kb(k, "tui.select.up") || tui_key_matches(k, "up")) {
        if (m->values.n && m->sel > 0) m->sel--;
    } else if (tui_kb(k, "tui.select.down") || tui_key_matches(k, "down")) {
        if (m->values.n && m->sel + 1 < (int)m->values.n) m->sel++;
    } else if (tui_key_matches(k, "tab")) { /* #accept */
        if (m->values.n) {
            char *v = xstrdup(m->values.v[m->sel]);
            ui_input_set(&m->in, v);
            free(v);
            m->sel = 0;
            update_results(m);
        }
    } else {
        char *before = xstrdup(ui_input_value(&m->in));
        ui_input_key(&m->in, k);
        if (strcmp(before, ui_input_value(&m->in)) != 0) {
            m->sel = 0;
            update_results(m);
        }
        free(before);
    }
    tui_component_request_render(c);
    return true;
}

static void move_destroy(tui_component_t *c) {
    move_ov_t *m = (move_ov_t *)c;
    ui_input_free(&m->in);
    strvec_free(&m->values);
    strvec_free(&m->labels);
    free(m->cwd);
    free(m);
}

static const tui_component_vt MOVE_VT = {"move-overlay", move_render, move_key, NULL, move_destroy};

tui_component_t *ui_move_overlay_new(const char *cwd, void (*done)(void *ud, const char *dir), void *ud) {
    move_ov_t *m = xcalloc(1, sizeof *m);
    tui_component_init(&m->base, &MOVE_VT);
    m->cwd = xstrdup(cwd);
    m->done = done;
    m->ud = ud;
    update_results(m);
    return &m->base;
}
