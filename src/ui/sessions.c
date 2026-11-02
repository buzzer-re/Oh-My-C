/* sessions.c - /resume session selector (port of omp overlays/session-selector.ts): fullscreen
 * "Resume Session (current folder)" panel with a search field, two/three-row session blocks
 * (title or first message, dim preview, "date · size · current · ✔ done · fork" metadata), pinned
 * sessions first (📌, <agent>/session-pins.json, Alt+K / Alt+Shift+K), Delete/Backspace-on-empty
 * deletes after a "Delete session?" confirmation, F2 renames in a "Rename Session" panel, Tab
 * toggles all projects, Enter resumes, Esc cancels. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "core/fs.h"
#include "tui/fuzzy.h"

#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

enum { ST_UNKNOWN, ST_COMPLETE, ST_INTERRUPTED, ST_ABORTED, ST_ERROR, ST_PENDING };

typedef struct {
    tui_component_t base;
    ui_t *u;
    session_list_t all;
    int *status;  /* per `all` entry */
    bool *forked; /* header parentSession */
    VEC(size_t) vis;
    size_t sel;
    char *query;
    bool all_projects;
    strvec_t pins; /* pinned session ids */
    int mode;      /* 0 list, 1 delete confirmation, 2 rename */
    int choice;    /* delete dialog: 0 Yes, 1 No */
    sb_t rename;
    char *error;
    bool fake_cursor;
} sess_view_t;

/* ---------- pins (session-pins.json: a tab-indented JSON array of ids) ---------- */

static void pins_load(sess_view_t *v) {
    for (size_t i = 0; i < v->pins.n; i++) free(v->pins.v[i]);
    v->pins.n = 0;
    char *p = omc_agent_path("session-pins.json");
    size_t n = 0;
    char *d = fs_read_limit(p, 1 << 20, &n, NULL);
    json_t *j = d ? json_parse(d, n, NULL) : NULL;
    json_t *x;
    json_arr_foreach(json_is_arr(j) ? j : NULL, i, x) if (json_is_str(x)) strvec_push(&v->pins, json_sv(x));
    json_free(j);
    free(d);
    free(p);
}

static bool is_pinned(const sess_view_t *v, const char *id) { return id && strvec_contains(&v->pins, id); }

static char *pins_save(sess_view_t *v) {
    char *p = omc_agent_path("session-pins.json");
    sb_t b = SB_INIT;
    if (!v->pins.n) sb_append(&b, "[]");
    else {
        sb_append(&b, "[\n");
        for (size_t i = 0; i < v->pins.n; i++) {
            json_t *s = json_str(v->pins.v[i]);
            char *q = json_dump(s, 0);
            sb_appendf(&b, "\t%s%s\n", q, i + 1 < v->pins.n ? "," : "");
            free(q);
            json_free(s);
        }
        sb_append(&b, "]");
    }
    char *err = fs_write_atomic(p, b.s, b.len, 0600) != 0 ? xasprintf("Failed to save session pins: %s", strerror(errno)) : NULL;
    sb_free(&b);
    free(p);
    return err;
}

/* ---------- lifecycle status from the file tail (session-listing.ts deriveSessionStatus) ---------- */

static int status_of_message(const json_t *m) {
    const char *role = json_get_str(m, "role");
    if (str_eq(role, "assistant")) {
        const char *sr = json_get_str(m, "stopReason");
        if (str_eq(sr, "error")) return ST_ERROR;
        if (str_eq(sr, "aborted")) return ST_ABORTED;
        if (str_eq(sr, "length")) return ST_INTERRUPTED;
        json_t *b;
        json_arr_foreach(json_get_arr(m, "content"), i, b) if (str_eq(json_get_str(b, "type"), "toolCall")) return ST_INTERRUPTED;
        return ST_COMPLETE;
    }
    if (str_eq(role, "toolResult")) return ST_INTERRUPTED;
    if (str_eq(role, "user")) return ST_PENDING;
    return ST_UNKNOWN;
}

static void scan_session(const char *path, int *status, bool *forked) {
    *status = ST_UNKNOWN;
    *forked = false;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return;
    char head[8192];
    size_t hn = fread(head, 1, sizeof head - 1, f);
    head[hn] = 0;
    strvec_t hl = str_split_lines(head, hn);
    for (size_t i = 0; i < hl.n && i < 3; i++) {
        json_t *e = json_parse_str(hl.v[i]);
        if (str_eq(json_get_str(e, "type"), "session") && json_get_str(e, "parentSession")) *forked = true;
        json_free(e);
    }
    strvec_free(&hl);
    const long TAIL = 256 * 1024;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), off = size > TAIL ? size - TAIL : 0;
    fseek(f, off, SEEK_SET);
    char *buf = xmalloc((size_t)(size - off) + 1);
    size_t n = fread(buf, 1, (size_t)(size - off), f);
    buf[n] = 0;
    fclose(f);
    strvec_t lines = str_split_lines(buf, n);
    for (size_t i = lines.n; i-- > 0;) {
        if (lines.v[i][0] != '{') continue;
        json_t *e = json_parse_str(lines.v[i]);
        if (!e) continue;
        if (str_eq(json_get_str(e, "type"), "message") && json_get(e, "message")) {
            *status = status_of_message(json_get(e, "message"));
            json_free(e);
            break;
        }
        json_free(e);
    }
    strvec_free(&lines);
    free(buf);
}

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
    for (int pass = 0; pass < 2; pass++) /* pinned sessions first, each group in rank order */
        for (size_t i = 0; i < m; i++)
            if (is_pinned(v, v->all.v[idx[i]].id) == (pass == 0)) vec_push(&v->vis, idx[i]);
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
    free(v->status);
    free(v->forked);
    v->status = xcalloc(v->all.n + 1, sizeof *v->status);
    v->forked = xcalloc(v->all.n + 1, sizeof *v->forked);
    for (size_t i = 0; i < v->all.n; i++) scan_session(v->all.v[i].path, &v->status[i], &v->forked[i]);
    sess_refilter(v);
}

static int block_rows(const session_info_t *s) { return (s->title ? 3 : 2) + 1; }

static void sess_list_lines(sess_view_t *v, int width, int budget, strvec_t *out) {
    /* after a delete omp's search Input loses the terminal cursor and paints its own cell */
    char *in = v->fake_cursor ? xasprintf("> %s\x1b[7m \x1b[27m", v->query ? v->query : "")
                              : xasprintf("> %s" TUI_CURSOR_MARKER, v->query ? v->query : "");
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
        bool pinned = is_pinned(v, s->id);
        char *pin = pinned ? S(TUI_TC_ACCENT, tui_sym("icon.pin")) : NULL;
        char *pinp = pin ? xasprintf("%s ", pin) : xstrdup("");
        int pinw = pinned ? tui_visible_width(tui_sym("icon.pin")) + 1 : 0;
        char *t = tui_truncate_to_width(head, MAX(0, maxw - pinw), NULL);
        char *tb = selected ? tui_bold(t) : xstrdup(t);
        strvec_push_own(&rows, xasprintf("%s%s%s", cursor, pinp, tb));
        free(pin);
        free(pinp);
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
        static const struct {
            int slot;
            const char *sym, *label;
        } STATUS[] = {{0, NULL, NULL},
                      {TUI_TC_SUCCESS, "status.success", "done"},
                      {TUI_TC_WARNING, "status.warning", "interrupted"},
                      {TUI_TC_MUTED, "status.aborted", "aborted"},
                      {TUI_TC_ERROR, "status.error", "error"},
                      {TUI_TC_ACCENT, "status.pending", "pending"}};
        int stv = v->status[v->vis.v[i]];
        if (stv > ST_UNKNOWN) {
            char *t2 = xasprintf("%s %s", tui_sym(STATUS[stv].sym), STATUS[stv].label), *c = S(STATUS[stv].slot, t2);
            sb_appendf(&meta, " %s %s", dot, c);
            free(c);
            free(t2);
        }
        if (v->forked[v->vis.v[i]]) {
            char *t2 = xasprintf("%s fork", tui_sym("icon.branch")), *c = S(TUI_TC_DIM, t2);
            sb_appendf(&meta, " %s %s", dot, c);
            free(c);
            free(t2);
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

/* HookSelectorComponent("Delete session?\n<name>", ["Yes", "No"]) in the list's place */
static void delete_dialog_lines(sess_view_t *v, int width, strvec_t *out) {
    const session_info_t *s = &v->all.v[v->vis.v[v->sel]];
    char *name;
    if (s->title && *s->title) name = xstrdup(s->title);
    else if (s->first_message && *s->first_message) {
        name = xstrdup(s->first_message);
        size_t cut = 0, cps = 0;
        while (name[cut] && cps < 40) {
            cut++;
            while (name[cut] && ((unsigned char)name[cut] & 0xC0) == 0x80) cut++;
            cps++;
        }
        name[cut] = 0;
    } else name = xstrdup(s->id ? s->id : "");
    int inner = MAX(1, width - 4);
    strvec_t body = {0};
    strvec_push(&body, "");
    char *nm = S(TUI_TC_ACCENT, name);
    ui_push_text(&body, nm, inner);
    free(nm);
    strvec_push(&body, "");
    static const char *const OPTS[2] = {"Yes", "No"};
    for (int i = 0; i < 2; i++) {
        if (i == v->choice) {
            char *c = xasprintf("%s %s", tui_sym("nav.cursor"), OPTS[i]), *ca = S(TUI_TC_ACCENT, c);
            char *row = xasprintf(" %s", ca), *pad = tui_pad_to_width(row, inner);
            strvec_push_own(&body, tui_bgs(NULL, TUI_TC_SELECTED_BG, pad));
            free(c);
            free(ca);
            free(row);
            free(pad);
        } else strvec_push_own(&body, xasprintf("   %s", OPTS[i]));
    }
    strvec_push(&body, "");
    char *ud = ui_key_hint("up"), *dd = ui_key_hint("down"), *e = ui_key_hint("enter"), *esc = ui_key_hint("escape");
    char *h = xasprintf("%s/%s navigate  %s select  %s cancel", ud, dd, e, esc);
    strvec_push_own(&body, S(TUI_TC_DIM, h));
    strvec_push(&body, "");
    strvec_push_own(out, ui_box_top(width, "Delete session?"));
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    strvec_push_own(out, ui_box_bottom(width));
    strvec_free(&body);
    free(h);
    free(ud);
    free(dd);
    free(e);
    free(esc);
    free(name);
}

/* OverlayPanel("Rename Session") hosting the "Rename session" TextFormField */
static void rename_dialog_lines(sess_view_t *v, int width, strvec_t *out) {
    int inner = MAX(1, width - 4);
    strvec_push_own(out, ui_box_top(width, "Rename Session"));
    char *l = S(TUI_TC_ACCENT, "Rename session"), *lb = tui_bold(l);
    strvec_push_own(out, ui_box_row(lb, width));
    strvec_push_own(out, ui_box_row("", width));
    /* Input: "> " prompt, text, inverse cursor cell, scrolled to keep the cursor visible */
    const char *t = sb_cstr(&v->rename);
    int avail = MAX(1, inner - 2);
    int tw = tui_visible_width(t);
    char *vis = tw + 1 > avail ? tui_slice_cols(t, tw + 1 - avail, avail) : xstrdup(t);
    char *line = xasprintf("> %s\x1b[7m \x1b[27m", vis);
    strvec_push_own(out, ui_box_row(line, width));
    strvec_push_own(out, ui_box_row("", width));
    strvec_push_own(out, ui_box_bottom(width));
    free(line);
    free(vis);
    free(l);
    free(lb);
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
    if (v->error) { /* #showError: an error line and a spacer above the content */
        char *t = tui_replace_tabs(v->error), *e = xasprintf("Error: %s", t), *es = S(TUI_TC_ERROR, e);
        ui_push_text(&body, es, inner);
        strvec_push(&body, "");
        free(t);
        free(e);
        free(es);
    }
    if (v->mode == 1) delete_dialog_lines(v, inner, &body);
    else if (v->mode == 2) rename_dialog_lines(v, inner, &body);
    /* list budget: terminal rows - top - spacer - search/blank - footer(4) */
    else sess_list_lines(v, inner, rows - 1 - 1 - 2 - 4 - (int)body.n, &body);
    for (size_t i = 0; i < body.n; i++) strvec_push_own(&lines, ui_box_row(body.v[i], width));
    strvec_free(&body);
    int target = rows - 4;
    while ((int)lines.n > target) free(lines.v[--lines.n]);
    while ((int)lines.n < target) strvec_push_own(&lines, ui_box_row("", width));
    char *e = ui_key_hint("enter"), *tab = ui_key_hint("tab"), *esc = ui_key_hint("escape");
    char *del = ui_key_hint("delete"), *bs = ui_key_hint("backspace"), *f2 = ui_key_hint("f2");
    const session_info_t *cur = v->vis.n ? &v->all.v[v->vis.v[v->sel]] : NULL;
    bool unpin = cur && is_pinned(v, cur->id);
    char *pk = ui_key_hint(unpin ? "alt+shift+k" : "alt+k");
    char *hint = xasprintf("[%s/%s delete \xc2\xb7 %s rename \xc2\xb7 %s %s \xc2\xb7 %s select \xc2\xb7 %s %s \xc2\xb7 %s cancel]", del, bs,
                           f2, pk, unpin ? "unpin" : "pin", e, tab, v->all_projects ? "current folder" : "all projects", esc);
    free(del);
    free(bs);
    free(f2);
    free(pk);
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
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    tui_component_destroy(&v->base);
}

static void delete_selected(sess_view_t *v) {
    const session_info_t *s = &v->all.v[v->vis.v[v->sel]];
    char *path = xstrdup(s->path);
    ui_t *u = v->u;
    const char *cur = u->app->session ? u->app->session->file : NULL;
    if (cur && !strcmp(cur, path)) { /* #detachActiveSessionBeforeDeletion: move to a fresh session */
        app_new_session(u->app);
        tui_container_clear(u->chat);
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        free(v->error);
        v->error = xasprintf("Failed to delete session: %s", strerror(errno));
    } else {
        if (str_ends(path, ".jsonl")) {
            char *art = xstrndup(path, strlen(path) - 6);
            fs_rm_rf(art);
            free(art);
        }
        size_t keep = v->sel;
        sess_load(v);
        if (v->vis.n) v->sel = MIN(keep, v->vis.n - 1);
    }
    free(path);
}

static void rename_selected(sess_view_t *v) {
    char *title = str_trim(sb_cstr(&v->rename));
    v->mode = 0;
    if (!*title) { /* empty submit: cancel */
        free(title);
        return;
    }
    const session_info_t *s = &v->all.v[v->vis.v[v->sel]];
    ui_t *u = v->u;
    const char *cur = u->app->session ? u->app->session->file : NULL;
    bool ok = false;
    if (cur && !strcmp(cur, s->path)) {
        session_set_title(u->app->session, title, "user");
        ok = true;
    } else {
        char *err = NULL;
        session_t *ss = session_open(s->path, &err);
        if (ss) {
            session_set_title(ss, title, "user");
            session_free(ss);
            ok = true;
        }
        free(err);
    }
    if (!ok) {
        free(v->error);
        v->error = xstrdup("Failed to rename session: it is currently in use or no longer available");
    } else {
        session_info_t *e = &v->all.v[v->vis.v[v->sel]];
        free(e->title);
        e->title = xstrdup(title);
    }
    free(title);
}

static bool sess_key(tui_component_t *c, const tui_key_t *k) {
    sess_view_t *v = (sess_view_t *)c;
    if (v->mode == 1) {
        if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) v->choice = 1 - v->choice;
        else if (tui_kb(k, "tui.select.cancel")) v->mode = 0;
        else if (tui_key_matches(k, "enter")) {
            v->mode = 0;
            free(v->error);
            v->error = NULL;
            if (v->choice == 0) {
                delete_selected(v);
                v->fake_cursor = true;
            }
        }
        tui_component_request_render(c);
        return true;
    }
    if (v->mode == 2) {
        const char *txt = tui_key_text(k);
        if (tui_kb(k, "tui.select.cancel")) v->mode = 0;
        else if (tui_key_matches(k, "enter")) {
            free(v->error);
            v->error = NULL;
            rename_selected(v);
        } else if (tui_kb(k, "tui.editor.deleteCharBackward")) {
            size_t i = v->rename.len;
            while (i > 0 && ((unsigned char)v->rename.s[--i] & 0xC0) == 0x80) {
            }
            sb_truncate(&v->rename, i);
        } else if (tui_kb(k, "tui.editor.deleteToLineStart")) sb_clear(&v->rename);
        else if (k->kind == TUI_KEY_PASTE && k->text) sb_append(&v->rename, k->text);
        else if (k->kind == TUI_KEY_CHAR && txt && *txt && !(k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT | TUI_MOD_SUPER)))
            sb_append(&v->rename, txt);
        tui_component_request_render(c);
        return true;
    }
    if (tui_kb(k, "tui.select.cancel")) {
        sess_close(v);
        return true;
    }
    long n = (long)v->vis.n;
    if (n && (tui_key_matches(k, "delete") || (tui_key_matches(k, "backspace") && !(v->query && *v->query)))) {
        v->mode = 1;
        v->choice = 0;
        tui_component_request_render(c);
        return true;
    }
    if (n && tui_key_matches(k, "f2")) {
        const session_info_t *s = &v->all.v[v->vis.v[v->sel]];
        sb_clear(&v->rename);
        sb_append(&v->rename, s->title ? s->title : "");
        v->mode = 2;
        tui_component_request_render(c);
        return true;
    }
    if (n && (tui_key_matches(k, "alt+k") || tui_key_matches(k, "alt+shift+k"))) {
        const session_info_t *s = &v->all.v[v->vis.v[v->sel]];
        bool want = tui_key_matches(k, "alt+k");
        if (s->id && want != is_pinned(v, s->id)) {
            char *id = xstrdup(s->id);
            pins_load(v);
            if (want) {
                if (!strvec_contains(&v->pins, id)) strvec_push(&v->pins, id);
            } else {
                for (size_t i = 0; i < v->pins.n; i++)
                    if (!strcmp(v->pins.v[i], id)) {
                        free(v->pins.v[i]);
                        memmove(&v->pins.v[i], &v->pins.v[i + 1], (v->pins.n - i - 1) * sizeof *v->pins.v);
                        v->pins.n--;
                        break;
                    }
            }
            free(v->error);
            v->error = pins_save(v);
            sess_refilter(v);
            for (size_t i = 0; i < v->vis.n; i++) /* the cursor follows the session */
                if (str_eq(v->all.v[v->vis.v[i]].id, id)) v->sel = i;
            free(id);
        }
        tui_component_request_render(c);
        return true;
    }
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
    free(v->status);
    free(v->forked);
    strvec_free(&v->pins);
    sb_free(&v->rename);
    free(v->error);
    vec_free(&v->vis);
    free(v->query);
    free(v);
}

static const tui_component_vt SESS_VT = {"session-selector", sess_render, sess_key, NULL, sess_destroy};

void ui_overlay_session(ui_t *u) {
    sess_view_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &SESS_VT);
    v->u = u;
    pins_load(v);
    sess_load(v);
    tui_alt_screen(u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &v->base, &o);
}
