/* sessions.c - /resume session selector (port of omp overlays/session-selector.ts): fullscreen
 * "Resume Session (current folder)" panel with a search field, two/three-row session blocks
 * (title or first message, dim preview, "date · size · current" metadata), Tab to toggle all
 * projects, Enter to resume, Esc to cancel. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "core/fs.h"
#include "tui/fuzzy.h"

#include <math.h>
#include <sys/stat.h>

typedef struct {
    tui_component_t base;
    ui_t *u;
    session_list_t all;
    VEC(size_t) vis;
    size_t sel;
    char *query;
    bool all_projects;
} sess_view_t;

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static char *format_date(int64_t ms) {
    int64_t diff = now_ms() - ms;
    int64_t mins = diff / 60000, hours = diff / 3600000, days = diff / 86400000;
    if (mins < 1) return xstrdup("just now");
    if (mins < 60) return xasprintf("%lld minute%s ago", (long long)mins, mins != 1 ? "s" : "");
    if (hours < 24) return xasprintf("%lld hour%s ago", (long long)hours, hours != 1 ? "s" : "");
    if (days == 1) return xstrdup("1 day ago");
    if (days < 7) return xasprintf("%lld days ago", (long long)days);
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof buf, "%m/%d/%Y", &tm); /* en-US toLocaleDateString */
    char *p = buf[0] == '0' ? buf + 1 : buf;
    return xstrdup(p);
}

static char *format_bytes(int64_t n) {
    if (n < 1024) return xasprintf("%lldB", (long long)n);
    if (n < 1024 * 1024) return xasprintf("%.1fKB", n / 1024.0);
    if (n < 1024LL * 1024 * 1024) return xasprintf("%.1fMB", n / (1024.0 * 1024));
    return xasprintf("%.1fGB", n / (1024.0 * 1024 * 1024));
}

static void sess_refilter(sess_view_t *v) {
    v->vis.n = 0;
    size_t n = v->all.n;
    char **texts = xcalloc(n ? n : 1, sizeof *texts);
    for (size_t i = 0; i < n; i++)
        texts[i] = xasprintf("%s %s %s", v->all.v[i].title ? v->all.v[i].title : "",
                             v->all.v[i].first_message ? v->all.v[i].first_message : "", v->all.v[i].id ? v->all.v[i].id : "");
    size_t *idx = xmalloc((n ? n : 1) * sizeof *idx);
    size_t m = tui_fuzzy_rank(v->query ? v->query : "", (const char *const *)texts, n, idx);
    for (size_t i = 0; i < m; i++) vec_push(&v->vis, idx[i]);
    free(idx);
    for (size_t i = 0; i < n; i++) free(texts[i]);
    free(texts);
    v->sel = 0;
}

static void sess_load(sess_view_t *v) {
    session_list_free(&v->all);
    memset(&v->all, 0, sizeof v->all);
    if (v->all_projects) {
        session_list_all(&v->all);
    } else {
        char *dir = session_dir_for_cwd(v->u->app->cwd);
        session_list(dir, &v->all);
        free(dir);
    }
    sess_refilter(v);
}

static int block_rows(const session_info_t *s) { return (s->title ? 3 : 2) + 1; }

static void sess_list_lines(sess_view_t *v, int width, int budget, strvec_t *out) {
    char *in = xasprintf("> %s" TUI_CURSOR_MARKER, v->query ? v->query : "");
    strvec_push_own(out, in);
    strvec_push(out, "");
    if (!v->vis.n) {
        char *m;
        if (v->all_projects) {
            m = S(TUI_TC_MUTED, "No sessions found");
        } else {
            char *k = ui_key_hint("tab");
            char *t = xasprintf("No sessions in current folder. Press %s to view all.", k);
            m = S(TUI_TC_MUTED, t);
            free(t);
            free(k);
        }
        strvec_push_own(out, tui_truncate_to_width(m, width, NULL));
        free(m);
        return;
    }
    size_t n = v->vis.n;
    /* window by actual block height around the selection (getMenuWindow) */
    int total = 0;
    for (size_t i = 0; i < n; i++) total += block_rows(&v->all.v[v->vis.v[i]]);
    budget = MAX(1, budget);
    size_t start = v->sel, end = v->sel + 1;
    int used = block_rows(&v->all.v[v->vis.v[v->sel]]);
    int before = 0, target = MAX(0, budget - MIN(used, budget)) / 2;
    while (start > 0 && before + block_rows(&v->all.v[v->vis.v[start - 1]]) <= target) before += block_rows(&v->all.v[v->vis.v[--start]]);
    used += before;
    while (end < n && used + block_rows(&v->all.v[v->vis.v[end]]) <= budget) used += block_rows(&v->all.v[v->vis.v[end++]]);
    while (start > 0 && used + block_rows(&v->all.v[v->vis.v[start - 1]]) <= budget) used += block_rows(&v->all.v[v->vis.v[--start]]);
    int offset = 0;
    for (size_t i = 0; i < start; i++) offset += block_rows(&v->all.v[v->vis.v[i]]);
    int total_rows = total - 1;
    bool bar = total_rows > budget;
    int rw = MAX(1, width - (bar ? 1 : 0));
    const char *cur_path = v->u->app->session ? v->u->app->session->file : NULL;
    strvec_t rows = {0};
    const char *dotsym = tui_sym("sep.dot");
    char *dot = S(TUI_TC_DIM, dotsym && *dotsym ? dotsym : "\xc2\xb7");
    for (size_t i = start; i < end; i++) {
        const session_info_t *s = &v->all.v[v->vis.v[i]];
        bool selected = i == v->sel;
        char *cursym = xasprintf("%s ", tui_sym("nav.cursor"));
        int cw = tui_visible_width(cursym);
        char *cursor = selected ? S(TUI_TC_ACCENT, cursym) : xasprintf("%*s", cw, "");
        char *msg = xstrdup(s->first_message ? s->first_message : "");
        for (char *q = msg; *q; q++)
            if (*q == '\n') *q = ' ';
        char *m2 = str_trim(msg);
        free(msg);
        int maxw = rw - cw;
        const char *head = s->title ? s->title : m2;
        char *t = tui_truncate_to_width(head, MAX(0, maxw), NULL);
        char *tb = selected ? tui_bold(t) : xstrdup(t);
        strvec_push_own(&rows, xasprintf("%s%s", cursor, tb));
        if (s->title) {
            char *pv = tui_truncate_to_width(m2, MAX(0, maxw), NULL);
            char *pd = S(TUI_TC_DIM, pv);
            strvec_push_own(&rows, xasprintf("  %s", pd));
            free(pv);
            free(pd);
        }
        struct stat st;
        int64_t size = s->path && stat(s->path, &st) == 0 ? (int64_t)st.st_size : 0;
        char *date = format_date(s->mtime_ms), *bytes = format_bytes(size);
        char *dd = S(TUI_TC_DIM, date), *bd = S(TUI_TC_DIM, bytes);
        sb_t meta = SB_INIT;
        sb_appendf(&meta, "  %s %s %s", dd, dot, bd);
        if (cur_path && s->path && !strcmp(cur_path, s->path)) {
            char *c = S(TUI_TC_ACCENT, "current");
            sb_appendf(&meta, " %s %s", dot, c);
            free(c);
        }
        if (v->all_projects && s->cwd) {
            char *pd = path_display(s->cwd, NULL);
            char *ps = S(TUI_TC_DIM, pd);
            sb_appendf(&meta, " %s %s", dot, ps);
            free(ps);
            free(pd);
        }
        strvec_push_own(&rows, tui_truncate_to_width(sb_cstr(&meta), rw, NULL));
        sb_free(&meta);
        if (i + 1 < end) strvec_push(&rows, "");
        free(dd);
        free(bd);
        free(date);
        free(bytes);
        free(t);
        free(tb);
        free(m2);
        free(cursor);
        free(cursym);
    }
    free(dot);
    int h = (int)rows.n;
    int ts = 0, te = h;
    if (bar) {
        int size = MAX(1, MIN((h * h) / total_rows, h)), maxoff = MAX(0, total_rows - h);
        ts = maxoff ? (int)lround((double)MIN(offset, maxoff) / maxoff * (h - size)) : 0;
        te = ts + size;
    }
    for (int r = 0; r < h; r++) {
        if (!bar) {
            strvec_push(out, rows.v[r]);
            continue;
        }
        char *fit = ui_fit_line(rows.v[r], rw);
        bool thumb = r >= ts && r < te;
        char *b = S(thumb ? TUI_TC_ACCENT : TUI_TC_MUTED, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
        strvec_push_own(out, xasprintf("%s%s", fit, b));
        free(fit);
        free(b);
    }
    strvec_free(&rows);
}

static void sess_render(tui_component_t *c, int width, strvec_t *out) {
    sess_view_t *v = (sess_view_t *)c;
    int rows = MAX(8, tui_term(v->u->tui)->rows);
    int inner = MAX(1, width - 4);
    char *title = xasprintf("Resume Session (%s)", v->all_projects ? "all projects" : "current folder");
    strvec_t lines = {0};
    strvec_push_own(&lines, ui_box_top(width, title));
    free(title);
    strvec_push_own(&lines, ui_box_row("", width));
    strvec_t body = {0};
    /* list budget: terminal rows - top - spacer - search/blank - footer(4) */
    sess_list_lines(v, inner, rows - 1 - 1 - 2 - 4, &body);
    for (size_t i = 0; i < body.n; i++) strvec_push_own(&lines, ui_box_row(body.v[i], width));
    strvec_free(&body);
    int target = rows - 4;
    while ((int)lines.n > target) free(lines.v[--lines.n]);
    while ((int)lines.n < target) strvec_push_own(&lines, ui_box_row("", width));
    char *e = ui_key_hint("enter"), *tab = ui_key_hint("tab"), *esc = ui_key_hint("escape");
    char *hint = xasprintf("[%s select \xc2\xb7 %s %s \xc2\xb7 %s cancel]", e, tab, v->all_projects ? "current folder" : "all projects", esc);
    char *hm = S(TUI_TC_MUTED, hint);
    strvec_push_own(&lines, ui_box_row("", width));
    strvec_push_own(&lines, ui_box_row(hm, width));
    strvec_push_own(&lines, ui_box_row("", width));
    strvec_push_own(&lines, ui_box_bottom(width));
    free(hm);
    free(hint);
    free(e);
    free(tab);
    free(esc);
    for (size_t i = 0; i < lines.n; i++) strvec_push(out, lines.v[i]);
    strvec_free(&lines);
}

static void sess_close(sess_view_t *v) {
    ui_t *u = v->u;
    tui_overlay_hide(u->tui, &v->base);
    tui_term_puts(tui_term(u->tui), "\x1b[?1049l");
    tui_set_focus(u->tui, &u->editor->base);
    tui_request_full_render(u->tui);
    tui_component_destroy(&v->base);
}

static bool sess_key(tui_component_t *c, const tui_key_t *k) {
    sess_view_t *v = (sess_view_t *)c;
    if (tui_kb(k, "tui.select.cancel")) {
        sess_close(v);
        return true;
    }
    long n = (long)v->vis.n;
    if (tui_kb(k, "tui.select.up") && n) v->sel = (size_t)(((long)v->sel - 1 + n) % n);
    else if (tui_kb(k, "tui.select.down") && n) v->sel = (size_t)(((long)v->sel + 1) % n);
    else if (tui_key_matches(k, "tab")) {
        v->all_projects = !v->all_projects;
        sess_load(v);
    } else if (tui_key_matches(k, "enter")) {
        if (!n) return true;
        char *path = xstrdup(v->all.v[v->vis.v[v->sel]].path);
        ui_t *u = v->u;
        sess_close(v);
        ui_resume_session(u, path);
        free(path);
        return true;
    } else {
        const char *before = v->query ? v->query : "";
        const char *txt = tui_key_text(k);
        char *next = NULL;
        if (k->kind == TUI_KEY_CHAR && txt && *txt) next = xasprintf("%s%s", before, txt);
        else if (tui_kb(k, "tui.editor.deleteCharBackward") && *before) {
            size_t i = strlen(before) - 1;
            while (i > 0 && ((unsigned char)before[i] & 0xC0) == 0x80) i--;
            next = xstrndup(before, i);
        } else if (tui_kb(k, "tui.editor.deleteToLineStart")) next = xstrdup("");
        if (next) {
            free(v->query);
            v->query = next;
            sess_refilter(v);
        }
    }
    tui_component_request_render(c);
    return true;
}

static void sess_destroy(tui_component_t *c) {
    sess_view_t *v = (sess_view_t *)c;
    session_list_free(&v->all);
    vec_free(&v->vis);
    free(v->query);
    free(v);
}

static const tui_component_vt SESS_VT = {"session-selector", sess_render, sess_key, NULL, sess_destroy};

void ui_overlay_session(ui_t *u) {
    sess_view_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &SESS_VT);
    v->u = u;
    sess_load(v);
    tui_term_puts(tui_term(u->tui), "\x1b[?1049h");
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &v->base, &o);
    tui_request_full_render(u->tui);
}
