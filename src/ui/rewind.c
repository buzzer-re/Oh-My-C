/* rewind.c - the Esc-Esc / /branch rewind selector (port of omp overlays/rewind-selector.ts over
 * chat/transcript-outline.ts and chat/transcript-browser.ts, as shipped in omp 18.14.2).
 *
 * A fullscreen page on the alternate screen: the current branch replayed as the transcript renders it,
 * framed by rules with a "↶ Rewind · pick the point to continue from" header and a key-hint footer, and a
 * dotted outline (╭┄╮ ┆ ╰┄╯) around the transcript item the rewind would land on. Tool results fold into
 * the turn that rendered their call; entries that render nothing are never outlined.
 *
 * Up/Down step through items, Left/Right jump between user turns, or - when the outlined turn has sibling
 * branches in the session tree - slide a camera over a strip of half-width columns (current path first,
 * each alternate branch's latest path beside it). `f` filters the current path by words (Esc leaves the
 * filter, keeping the selection), `a` loads turns older than the 600-entry replay window, the expand key
 * toggles tool output, Enter rewinds, Esc cancels. PgUp/PgDn/Home/End/Shift+arrows and the wheel scroll
 * without moving the selection. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

#define RECENT_LIMIT 600
#define STRIP_GAP 2
#define SLIDE_MS 160
#define FAST_SCROLL 5

typedef struct {
    char *entry_id, *turn_id;
    bool user;
    size_t start, end;
} target_t;

typedef VEC(target_t) targets_t;
typedef VEC(strvec_t) rows_t; /* rendered rows per transcript child */
typedef VEC(size_t) idxv_t;

typedef struct {
    char *root_id, *label;
    tui_container_t *c;
    targets_t targets;
} column_t;

typedef struct {
    char *turn_id;
    VEC(column_t *) cols;
} strip_t;

typedef struct {
    tui_component_t base;
    ui_t *u;
    ui_rewind_cb_t cb;
    json_t *entries; /* the whole branch's transcript entries */
    bool truncated;
    tui_container_t *main;
    targets_t targets;
    size_t selected;
    bool *visible; /* per main target, from the last frame (NULL = all) */
    bool *sib_visible;
    size_t nsib_visible;
    bool expanded;
    VEC(strip_t) strips;
    size_t active, sib_sel;
    double slide_from, slide_to;
    int64_t slide_start;
    bool sliding;
    int timer;
    bool filtering;
    ui_input_t filter;
    rows_t main_rows; /* main rows of the last frame (filter haystack) */
    /* scroll view */
    int offset;
    char *anchor_id;
    int anchor_start, anchor_end, anchor_w, anchor_h, anchor_total;
    bool done;
} rewind_t;

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static void rows_free(rows_t *r) {
    for (size_t i = 0; i < r->n; i++) strvec_free(&r->v[i]);
    vec_free(r);
}

static void targets_free(targets_t *t) {
    for (size_t i = 0; i < t->n; i++) free(t->v[i].entry_id), free(t->v[i].turn_id);
    vec_free(t);
}

/* ---------- entries ---------- */

static bool is_transcript_entry(const json_t *e) {
    const char *t = json_get_str(e, "type");
    return str_eq(t, "message") || str_eq(t, "custom_message");
}

static bool user_initiator(const json_t *m) {
    const char *ct = json_get_str(m, "customType");
    return (str_eq(ct, "skill-prompt") || str_eq(ct, "collab-prompt")) && str_eq(json_get_str(m, "attribution"), "user");
}

static bool is_user_request(const json_t *e) {
    const char *t = json_get_str(e, "type");
    if (str_eq(t, "message")) {
        const json_t *m = json_get(e, "message");
        const char *role = json_get_str(m, "role");
        return str_eq(role, "user") || (str_eq(role, "custom") && user_initiator(m));
    }
    return str_eq(t, "custom_message") && user_initiator(e);
}

static char *text_of(const json_t *content, const char *sep) {
    if (json_is_str(content)) return xstrdup(json_sv(content));
    sb_t b = SB_INIT;
    const json_t *blk;
    bool first = true;
    json_arr_foreach(content, i, blk) {
        if (!str_eq(json_get_str(blk, "type"), "text")) continue;
        if (!first) sb_append(&b, sep);
        sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
        first = false;
    }
    return sb_detach(&b);
}

/* isUserTurnEntry: a user message with prompt text, or a user-initiated custom message */
static bool is_user_turn(const json_t *e) {
    if (str_eq(json_get_str(e, "type"), "message") && str_eq(json_get_str(json_get(e, "message"), "role"), "user")) {
        char *t = text_of(json_get(json_get(e, "message"), "content"), "");
        bool any = false;
        for (char *p = t; *p; p++) any |= !isspace((unsigned char)*p);
        free(t);
        return any;
    }
    return is_user_request(e);
}

/* userTurnLabel: the prompt text on one line */
static char *user_turn_label(const json_t *e) {
    const json_t *m = str_eq(json_get_str(e, "type"), "message") ? json_get(e, "message") : e;
    char *t = text_of(json_get(m, "content"), " ");
    sb_t b = SB_INIT;
    bool space = false;
    for (char *p = t; *p; p++) {
        if (isspace((unsigned char)*p)) space = b.len > 0;
        else {
            if (space) sb_putc(&b, ' ');
            space = false;
            sb_putc(&b, *p);
        }
    }
    free(t);
    return sb_detach(&b);
}

/* appendOutlineEntries: replay `entries` into `c`, returning the selectable targets */
static void replay(rewind_t *r, tui_container_t *c, const json_t *entries, targets_t *out) {
    size_t n = json_len(entries);
    size_t *ends = xmalloc((n ? n : 1) * sizeof *ends);
    ui_chat_replay_into(r->u, c, entries, ends);
    size_t before = 0;
    for (size_t i = 0; i < n; i++) {
        const json_t *e = json_at(entries, i);
        size_t after = ends[i];
        target_t *prev = out->n ? &out->v[out->n - 1] : NULL;
        if (str_eq(json_get_str(e, "type"), "message") && str_eq(json_get_str(json_get(e, "message"), "role"), "toolResult") &&
            prev) {
            free(prev->entry_id);
            prev->entry_id = xstrdup(json_get_str(e, "id"));
            if (after > prev->end) prev->end = after;
        } else if (before < after) {
            target_t t = {xstrdup(json_get_str(e, "id")), xstrdup(json_get_str(e, "id")), is_user_turn(e), before, after};
            vec_push(out, t);
        }
        before = after;
    }
    free(ends);
}

/* recentTranscriptEntries: the tail from a user-request boundary within the last 600 entries */
static json_t *recent_entries(const json_t *entries) {
    size_t n = json_len(entries);
    size_t from = 0;
    if (n > RECENT_LIMIT)
        for (size_t i = n - RECENT_LIMIT; i > 0; i--)
            if (is_user_request(json_at(entries, i))) {
                from = i;
                break;
            }
    json_t *out = json_arr();
    for (size_t i = from; i < n; i++) json_push(out, json_clone(json_at(entries, i)));
    return out;
}

static void load_main(rewind_t *r, const json_t *entries) {
    if (r->main) tui_component_destroy(&r->main->base);
    targets_free(&r->targets);
    memset(&r->targets, 0, sizeof r->targets);
    r->main = tui_container_new();
    replay(r, r->main, entries, &r->targets);
    free(r->visible);
    r->visible = NULL;
}

/* loadFullHistory: replay the whole branch, keeping the outline on the same turn */
static void load_full(rewind_t *r) {
    if (!r->truncated) return;
    char *sel = r->targets.n ? xstrdup(r->targets.v[r->selected].turn_id) : NULL;
    load_main(r, r->entries);
    r->truncated = false;
    rows_free(&r->main_rows);
    memset(&r->main_rows, 0, sizeof r->main_rows);
    ssize_t found = -1;
    for (size_t i = 0; sel && i < r->targets.n; i++)
        if (str_eq(r->targets.v[i].turn_id, sel)) found = (ssize_t)i;
    r->selected = found >= 0 ? (size_t)found : r->targets.n ? r->targets.n - 1 : 0;
    free(sel);
}

/* ---------- branch strip ---------- */

static void strip_free(strip_t *s) {
    for (size_t i = 0; i < s->cols.n; i++) {
        column_t *c = s->cols.v[i];
        tui_component_destroy(&c->c->base);
        targets_free(&c->targets);
        free(c->root_id), free(c->label), free(c);
    }
    vec_free(&s->cols);
    free(s->turn_id);
}

/* #siblingBranchPaths: every other child of the turn's parent, each unrolled along its latest children */
static json_t *sibling_paths(rewind_t *r, const char *entry_id, strvec_t *roots) {
    session_t *s = r->u->app->session;
    json_t *paths = json_arr();
    const json_t *entry = s ? session_get(s, entry_id) : NULL;
    if (!entry) return paths;
    const char *parent = json_get_str(entry, "parentId");
    const json_t *e;
    json_arr_foreach(s->entries, i, e) {
        if (str_eq(json_get_str(e, "id"), entry_id) || !str_eq(json_get_str(e, "parentId"), parent)) continue;
        if (!parent && json_get_str(e, "parentId")) continue;
        json_t *path = json_arr();
        const json_t *node = e;
        for (size_t guard = 0; node && guard <= json_len(s->entries); guard++) {
            if (is_transcript_entry(node)) json_push(path, json_clone(node));
            const json_t *last = NULL, *c;
            json_arr_foreach(s->entries, k, c) if (str_eq(json_get_str(c, "parentId"), json_get_str(node, "id"))) last = c;
            node = last;
        }
        if (json_len(path)) {
            json_push(paths, path);
            strvec_push(roots, json_get_str(e, "id"));
        } else json_free(path);
    }
    return paths;
}

static strip_t *strip_columns(rewind_t *r) {
    if (!r->targets.n) return NULL;
    const char *turn = r->targets.v[r->selected].turn_id;
    for (size_t i = 0; i < r->strips.n; i++)
        if (str_eq(r->strips.v[i].turn_id, turn)) return &r->strips.v[i];
    strip_t st = {xstrdup(turn), {0}};
    strvec_t roots = {0};
    json_t *paths = sibling_paths(r, turn, &roots);
    const json_t *p;
    json_arr_foreach(paths, i, p) {
        column_t *c = xcalloc(1, sizeof *c);
        c->root_id = xstrdup(roots.v[i]);
        c->c = tui_container_new();
        replay(r, c->c, p, &c->targets);
        const json_t *e;
        json_arr_foreach(p, k, e) if (!c->label && is_user_turn(e)) {
            char *l = user_turn_label(e);
            if (*l) c->label = l;
            else free(l);
        }
        if (!c->label) c->label = xstrdup(c->root_id);
        vec_push(&st.cols, c);
    }
    json_free(paths);
    strvec_free(&roots);
    vec_push(&r->strips, st);
    return &r->strips.v[r->strips.n - 1];
}

static size_t strip_count(rewind_t *r) {
    strip_t *s = strip_columns(r);
    return s ? s->cols.n : 0;
}

static void stop_slide(rewind_t *r) {
    r->sliding = false;
    if (r->timer) tui_remove_timer(r->u->tui, r->timer);
    r->timer = 0;
}

static double slide_position(rewind_t *r, int64_t now) {
    if (!r->sliding) return (double)r->active;
    double t = MIN(1.0, (double)(now - r->slide_start) / SLIDE_MS);
    double eased = 1 - pow(1 - t, 3);
    return r->slide_from + (r->slide_to - r->slide_from) * eased;
}

static void slide_tick(void *ud) {
    rewind_t *r = ud;
    if (!r->sliding || mono_ms() - r->slide_start >= SLIDE_MS) stop_slide(r);
    tui_request_render(r->u->tui);
}

static void slide_to(rewind_t *r, size_t variant) {
    int64_t now = mono_ms();
    double from = slide_position(r, now);
    r->slide_from = from;
    r->slide_to = (double)variant;
    r->slide_start = now;
    r->sliding = true;
    r->active = variant;
    if (!r->timer) r->timer = tui_add_timer(r->u->tui, 16, true, slide_tick, r);
}

/* ---------- rows and outline composition (transcript-outline.ts) ---------- */

static bool blank_row(const char *s) {
    char *p = tui_strip_ansi(s);
    bool b = true;
    for (char *q = p; *q; q++)
        if (!isspace((unsigned char)*q)) b = false;
    free(p);
    return b;
}

/* rendered rows of each child at `w` columns: the transcript's blank separator row, then its rows (prompt
 * zone marks dropped) */
static void render_children(rewind_t *r, tui_container_t *c, size_t from, size_t to, int w, rows_t *out) {
    bool saved = r->u->expand_tools;
    r->u->expand_tools = r->expanded;
    for (size_t i = from; i < to && i < c->children.n; i++) {
        strvec_t raw = {0}, rows = {0};
        tui_component_render(c->children.v[i], w, &raw);
        size_t a = 0, b = raw.n;
        while (a < b && blank_row(raw.v[a])) a++;
        while (b > a && blank_row(raw.v[b - 1])) b--;
        if (a < b) strvec_push(&rows, "");
        for (size_t k = a; k < b; k++) {
            sb_t o = SB_INIT;
            for (const char *p = raw.v[k]; *p;) {
                if (!strncmp(p, "\x1b]133;", 6)) {
                    const char *e = p + 6;
                    while (*e && *e != '\x07' && !(e[0] == '\x1b' && e[1] == '\\')) e++;
                    p = *e == '\x07' ? e + 1 : *e ? e + 2 : e;
                    continue;
                }
                sb_putc(&o, *p++);
            }
            strvec_push_own(&rows, sb_detach(&o));
        }
        strvec_free(&raw);
        vec_push(out, rows);
    }
    r->u->expand_tools = saved;
}

static bool target_visible(const rows_t *rows, size_t base, const target_t *t) {
    for (size_t i = t->start; i < t->end; i++) {
        if (i < base || i - base >= rows->n) continue;
        const strvec_t *cr = &rows->v[i - base];
        for (size_t k = 0; k < cr->n; k++)
            if (!blank_row(cr->v[k])) return true;
    }
    return false;
}

static char *outline_rule(const char *left, const char *right, int inner) {
    sb_t b = SB_INIT;
    sb_append(&b, left);
    sb_repeat(&b, tui_sym("boxDotted.horizontal"), MAX(0, inner + 2));
    sb_append(&b, right);
    char *r = S(TUI_TC_ACCENT, sb_cstr(&b));
    sb_free(&b);
    return r;
}

typedef struct {
    strvec_t lines;
    int sel_start, sel_end;
} composed_t;

/* composeOutlineColumn over rows[from, to) (rows indexed from `base`) */
static void compose(const rows_t *rows, size_t base, size_t from, size_t to, const target_t *targets, ssize_t sel,
                    int col_w, const strvec_t *header, composed_t *out) {
    int inner = MAX(10, col_w - 4);
    memset(out, 0, sizeof *out);
    out->sel_start = out->sel_end = -1;
    if (header)
        for (size_t i = 0; i < header->n; i++) strvec_push(&out->lines, header->v[i]);
    const target_t *t = sel >= 0 ? &targets[sel] : NULL;
    char *vert = S(TUI_TC_ACCENT, tui_sym("boxDotted.vertical"));
    for (size_t idx = from; idx < to; idx++) {
        if (t && idx == t->start && t->end <= to) {
            strvec_t seg = {0};
            for (size_t c = t->start; c < t->end; c++)
                if (c >= base && c - base < rows->n)
                    for (size_t k = 0; k < rows->v[c - base].n; k++) strvec_push(&seg, rows->v[c - base].v[k]);
            size_t head = 0, tail = seg.n;
            while (head < tail && blank_row(seg.v[head])) head++;
            while (tail > head && blank_row(seg.v[tail - 1])) tail--;
            for (size_t k = 0; k < head; k++) strvec_push(&out->lines, "");
            out->sel_start = (int)out->lines.n;
            strvec_push_own(&out->lines, outline_rule(tui_sym("boxRound.topLeft"), tui_sym("boxRound.topRight"), inner));
            for (size_t k = head; k < tail; k++) {
                char *tr = tui_truncate_to_width(seg.v[k], inner, NULL), *pad = tui_pad_to_width(tr, inner);
                strvec_push_own(&out->lines, xasprintf("%s %s %s", vert, pad, vert));
                free(tr), free(pad);
            }
            strvec_push_own(&out->lines, outline_rule(tui_sym("boxRound.bottomLeft"), tui_sym("boxRound.bottomRight"), inner));
            out->sel_end = (int)out->lines.n;
            for (size_t k = tail; k < seg.n; k++) strvec_push(&out->lines, "");
            strvec_free(&seg);
            idx = t->end - 1;
            continue;
        }
        if (idx < base || idx - base >= rows->n) continue;
        const strvec_t *cr = &rows->v[idx - base];
        for (size_t k = 0; k < cr->n; k++)
            strvec_push_own(&out->lines, *cr->v[k] ? xasprintf("  %s", cr->v[k]) : xstrdup(""));
    }
    free(vert);
}

/* positionRail: `… ○ ◉ ○ …` centred */
static char *position_rail(size_t count, size_t active, bool more_left, bool more_right, int width) {
    sb_t b = SB_INIT;
    if (more_left) {
        char *d = S(TUI_TC_DIM, "\xe2\x80\xa6 ");
        sb_append(&b, d);
        free(d);
    } else sb_append(&b, "  ");
    for (size_t i = 0; i < count; i++) {
        char *d = i == active ? S(TUI_TC_ACCENT, tui_sym("radio.selected")) : S(TUI_TC_DIM, tui_sym("radio.unselected"));
        if (i) sb_putc(&b, ' ');
        sb_append(&b, d);
        free(d);
    }
    if (more_right) {
        char *d = S(TUI_TC_DIM, " \xe2\x80\xa6");
        sb_append(&b, d);
        free(d);
    }
    int pad = MAX(0, (width - tui_visible_width(sb_cstr(&b))) / 2);
    sb_t o = SB_INIT;
    tui_sb_spaces(&o, pad);
    sb_append(&o, sb_cstr(&b));
    sb_free(&b);
    return sb_detach(&o);
}

static void column_header(rewind_t *r, size_t index, size_t count, const char *label, int col_w, strvec_t *out) {
    char *cap = xasprintf("%s %zu/%zu %s %s", tui_sym("icon.branch"), index + 1, count, tui_sym("sep.dot"), label);
    char *tr = tui_truncate_to_width(cap, MAX(0, col_w - 2), NULL);
    char *c = S(index == r->active ? TUI_TC_ACCENT : TUI_TC_DIM, tr);
    strvec_push_own(out, xasprintf(" %s", c));
    strvec_push(out, "");
    free(cap), free(tr), free(c);
}

/* ---------- filter ---------- */

static bool word_char(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

/* every query word in `hay` (lowercase): Latin/digit words as whole words, others as substrings */
static bool word_in(const char *hay, const char *word) {
    bool latin = true;
    for (const unsigned char *p = (const unsigned char *)word; *p; p++)
        if (!(isalnum(*p) || *p == '_')) latin = false;
    size_t wl = strlen(word);
    for (const char *p = strstr(hay, word); p; p = strstr(p + 1, word)) {
        if (!latin) return true;
        bool before = p > hay && word_char((unsigned char)p[-1]);
        bool after = word_char((unsigned char)p[wl]);
        if (!before && !after) return true;
    }
    return false;
}

static bool main_selectable(const rewind_t *r, size_t i) { return !r->visible || r->visible[i]; }

static void filter_matches(rewind_t *r, idxv_t *out) {
    out->n = 0;
    char *q = xstrdup(ui_input_value(&r->filter));
    for (char *p = q; *p; p++) *p = (char)tolower((unsigned char)*p);
    strvec_t words = {0};
    for (char *w = strtok(q, " \t\n"); w; w = strtok(NULL, " \t\n")) strvec_push(&words, w);
    for (size_t i = 0; i < r->targets.n; i++) {
        if (!main_selectable(r, i)) continue;
        bool all = true;
        for (size_t k = 0; k < words.n && all; k++) {
            bool any = false;
            for (size_t c = r->targets.v[i].start; c < r->targets.v[i].end && !any && c < r->main_rows.n; c++) {
                sb_t t = SB_INIT;
                for (size_t x = 0; x < r->main_rows.v[c].n; x++) {
                    if (x) sb_putc(&t, '\n');
                    char *p = tui_strip_ansi(r->main_rows.v[c].v[x]);
                    sb_append(&t, p);
                    free(p);
                }
                for (size_t x = 0; x < t.len; x++) t.s[x] = (char)tolower((unsigned char)t.s[x]);
                any = word_in(sb_cstr(&t), words.v[k]);
                sb_free(&t);
            }
            all = any;
        }
        if (all) vec_push(out, i);
    }
    strvec_free(&words);
    free(q);
}

/* ---------- movement ---------- */

static void move(rewind_t *r, int delta, bool users_only) {
    for (ssize_t i = (ssize_t)r->selected + delta; i >= 0 && i < (ssize_t)r->targets.n; i += delta) {
        if (main_selectable(r, (size_t)i) && (!users_only || r->targets.v[i].user)) {
            r->selected = (size_t)i;
            r->active = 0;
            r->sib_sel = 0;
            stop_slide(r);
            return;
        }
    }
    if (delta < 0 && r->truncated) {
        load_full(r);
        move(r, delta, users_only);
    }
}

static void move_vertical(rewind_t *r, int delta) {
    if (r->active > 0) {
        strip_t *s = strip_columns(r);
        column_t *c = s && r->active - 1 < s->cols.n ? s->cols.v[r->active - 1] : NULL;
        ssize_t n = c ? (ssize_t)c->targets.n : 0;
        ssize_t i = (ssize_t)r->sib_sel + delta;
        while (i >= 0 && i < n && r->sib_visible && (size_t)i < r->nsib_visible && !r->sib_visible[i]) i += delta;
        if (i >= 0 && i < n) r->sib_sel = (size_t)i;
        else if (delta < 0) {
            r->active = 0;
            r->sib_sel = 0;
            stop_slide(r);
            move(r, -1, false);
        }
        return;
    }
    move(r, delta, false);
}

static void step_filtered(rewind_t *r, int delta, bool users_only) {
    idxv_t m = {0};
    filter_matches(r, &m);
    ssize_t next = -1;
    if (delta < 0) {
        for (size_t i = 0; i < m.n; i++)
            if (m.v[i] < r->selected && (!users_only || r->targets.v[m.v[i]].user)) next = (ssize_t)m.v[i];
    } else {
        for (size_t i = 0; i < m.n && next < 0; i++)
            if (m.v[i] > r->selected && (!users_only || r->targets.v[m.v[i]].user)) next = (ssize_t)m.v[i];
    }
    if (next >= 0) r->selected = (size_t)next;
    vec_free(&m);
}

static void filter_changed(rewind_t *r) {
    idxv_t m = {0};
    filter_matches(r, &m);
    bool has = false;
    for (size_t i = 0; i < m.n; i++) has |= m.v[i] == r->selected;
    if (!has && m.n) {
        ssize_t best = -1;
        for (size_t i = 0; i < m.n; i++)
            if (m.v[i] < r->selected) best = (ssize_t)m.v[i];
        r->selected = best >= 0 ? (size_t)best : m.v[m.n - 1];
    }
    vec_free(&m);
}

/* ---------- frame ---------- */

static char *hint_keys(const char *a, const char *b) {
    char *x = ui_editor_key(a), *y = ui_editor_key(b);
    char *r = xasprintf("%s/%s", x, y);
    free(x), free(y);
    return r;
}

static char *header_line(void) {
    char *b = tui_bold("Rewind"), *d = S(TUI_TC_DIM, "pick the point to continue from");
    char *r = xasprintf("%s %s%s%s", tui_sym("icon.rewind"), b, tui_sym("sep.dot"), d);
    free(b), free(d);
    return r;
}

/* the scroll view's reveal of the outlined range (margin 1, nearest) when its identity or geometry changed */
static void reveal(rewind_t *r, const char *id, int start, int end, int width, int height, int total) {
    if (!id || start < 0 || end <= start) return;
    if (r->anchor_id && str_eq(r->anchor_id, id) && r->anchor_start == start && r->anchor_end == end && r->anchor_w == width &&
        r->anchor_h == height && r->anchor_total == total)
        return;
    int b = MAX(0, start - 1), y = MIN(total, end + 1);
    int maxoff = MAX(0, total - height);
    r->offset = MAX(0, MIN(r->offset, maxoff));
    if (b < r->offset) r->offset = b;
    else if (y > r->offset + height) r->offset = MAX(b, y - 1) - height + 1;
    r->offset = MAX(0, MIN(r->offset, maxoff));
    free(r->anchor_id);
    r->anchor_id = xstrdup(id);
    r->anchor_start = start, r->anchor_end = end, r->anchor_w = width, r->anchor_h = height, r->anchor_total = total;
}

static int term_rows(rewind_t *r) {
    int rows = r->u->tui ? tui_term(r->u->tui)->rows : 40;
    return rows > 0 ? rows : 40;
}

static void rewind_render(tui_component_t *c, int width, strvec_t *out) {
    rewind_t *r = (rewind_t *)c;
    int content_w = MAX(1, width - 1);
    int child_w = MAX(10, content_w - 4);
    rows_free(&r->main_rows);
    memset(&r->main_rows, 0, sizeof r->main_rows);
    render_children(r, r->main, 0, r->main->children.n, child_w, &r->main_rows);
    /* adopt visibility: a selection that shows nothing moves to the nearest item that does */
    free(r->visible);
    r->visible = xcalloc(MAX(1, r->targets.n), sizeof *r->visible);
    for (size_t i = 0; i < r->targets.n; i++) r->visible[i] = target_visible(&r->main_rows, 0, &r->targets.v[i]);
    if (r->targets.n && !r->visible[r->selected]) {
        ssize_t pick = -1;
        for (ssize_t i = (ssize_t)r->selected - 1; i >= 0 && pick < 0; i--)
            if (r->visible[i]) pick = i;
        for (size_t i = r->selected + 1; i < r->targets.n && pick < 0; i++)
            if (r->visible[i]) pick = (ssize_t)i;
        if (pick >= 0) {
            r->selected = (size_t)pick;
            r->active = 0;
            r->sib_sel = 0;
        }
    }
    strvec_t body = {0};
    char *anchor = NULL;
    int sel_start = -1, sel_end = -1;
    char *footer;
    char *updown = hint_keys("tui.select.up", "tui.select.down");
    char *lr = xasprintf("%s/%s", tui_sym("key.left"), tui_sym("key.right"));
    {
        char *kl = ui_key_hint("left"), *kr = ui_key_hint("right");
        free(lr);
        lr = xasprintf("%s/%s", kl, kr);
        free(kl), free(kr);
    }
    char *kenter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel");
    if (r->filtering) {
        idxv_t m = {0};
        filter_matches(r, &m);
        targets_t ft = {0};
        rows_t frows = {0};
        ssize_t fsel = -1;
        for (size_t i = 0; i < m.n; i++) {
            const target_t *t = &r->targets.v[m.v[i]];
            size_t start = frows.n;
            for (size_t k = t->start; k < t->end && k < r->main_rows.n; k++) {
                strvec_t copy = {0};
                for (size_t x = 0; x < r->main_rows.v[k].n; x++) strvec_push(&copy, r->main_rows.v[k].v[x]);
                vec_push(&frows, copy);
            }
            target_t nt = {NULL, NULL, t->user, start, frows.n};
            vec_push(&ft, nt);
            if (m.v[i] == r->selected) fsel = (ssize_t)i;
        }
        composed_t comp;
        compose(&frows, 0, 0, frows.n, ft.v, fsel, content_w, NULL, &comp);
        const char *q = ui_input_value(&r->filter);
        if (m.n) {
            body = comp.lines;
            memset(&comp.lines, 0, sizeof comp.lines);
        } else {
            char *t = xasprintf("  No items match \"%s\"", q);
            strvec_push_own(&body, S(TUI_TC_MUTED, t));
            free(t);
        }
        strvec_free(&comp.lines);
        sel_start = comp.sel_start, sel_end = comp.sel_end;
        if (sel_start >= 0) anchor = xasprintf("rewind:filter:%s:%s", q, r->targets.n ? r->targets.v[r->selected].turn_id : "");
        char *count;
        if (!m.n) count = S(TUI_TC_ERROR, "no matches");
        else {
            char *t = fsel >= 0 ? xasprintf("%zd/%zu", fsel + 1, m.n) : xasprintf("-/%zu", m.n);
            count = S(TUI_TC_DIM, t);
            free(t);
        }
        char *keys = xasprintf("%s step  %s user turns  %s rewind  %s show all", updown, lr, kenter, cancel);
        char *dk = S(TUI_TC_DIM, keys);
        char *prompt = S(TUI_TC_ACCENT, "filter:"), *pr = xasprintf("%s ", prompt);
        char *plain = xasprintf("filter: %s", q);
        char *in = ui_input_render(&r->filter, pr, tui_visible_width(plain) + 1, r->base.focused);
        footer = xasprintf("%s  %s  %s", in, count, dk);
        free(count), free(keys), free(dk), free(prompt), free(pr), free(plain), free(in);
        for (size_t i = 0; i < ft.n; i++) free(ft.v[i].entry_id), free(ft.v[i].turn_id);
        vec_free(&ft);
        rows_free(&frows);
        vec_free(&m);
    } else {
        strip_t *st = strip_columns(r);
        size_t ncols = st ? st->cols.n : 0;
        if (ncols && r->targets.n) {
            const target_t *anc = &r->targets.v[r->selected];
            int col_w = MAX(24, (content_w - STRIP_GAP) / 2);
            size_t count = ncols + 1;
            composed_t prefix;
            compose(&r->main_rows, 0, 0, anc->start, NULL, -1, content_w, NULL, &prefix);
            int cw = MAX(10, col_w - 4);
            VEC(composed_t) cols = {0};
            {
                rows_t suffix = {0};
                render_children(r, r->main, anc->start, r->main->children.n, cw, &suffix);
                targets_t sts = {0};
                for (size_t i = r->selected; i < r->targets.n; i++) {
                    target_t t = r->targets.v[i];
                    t.entry_id = t.turn_id = NULL;
                    t.start -= anc->start;
                    t.end -= anc->start;
                    vec_push(&sts, t);
                }
                strvec_t hdr = {0};
                column_header(r, 0, count, "current", col_w, &hdr);
                composed_t c0;
                compose(&suffix, 0, 0, suffix.n, sts.v, r->active == 0 ? 0 : -1, col_w, &hdr, &c0);
                vec_push(&cols, c0);
                strvec_free(&hdr);
                vec_free(&sts);
                rows_free(&suffix);
            }
            for (size_t i = 0; i < ncols; i++) {
                column_t *col = st->cols.v[i];
                rows_t crow = {0};
                render_children(r, col->c, 0, col->c->children.n, cw, &crow);
                if (r->active == i + 1) {
                    free(r->sib_visible);
                    r->nsib_visible = col->targets.n;
                    r->sib_visible = xcalloc(MAX(1, col->targets.n), sizeof *r->sib_visible);
                    for (size_t k = 0; k < col->targets.n; k++) r->sib_visible[k] = target_visible(&crow, 0, &col->targets.v[k]);
                }
                strvec_t hdr = {0};
                column_header(r, i + 1, count, col->label, col_w, &hdr);
                composed_t cc;
                compose(&crow, 0, 0, crow.n, col->targets.v, r->active == i + 1 ? (ssize_t)r->sib_sel : -1, col_w, &hdr, &cc);
                vec_push(&cols, cc);
                strvec_free(&hdr);
                rows_free(&crow);
            }
            int stride = col_w + STRIP_GAP;
            int total_w = (int)count * col_w + (int)(count - 1) * STRIP_GAP;
            double pos = slide_position(r, mono_ms());
#define CAMERA_AT(p) MAX(0.0, MIN((p) * stride - (content_w - col_w) / 2.0, (double)MAX(0, total_w - content_w)))
            int camera = (int)floor(CAMERA_AT(pos));
            body = prefix.lines;
            if (count > 2) {
                double settled = CAMERA_AT((double)r->active);
                strvec_push_own(&body, position_rail(count, r->active, settled > 0.5, settled + content_w < total_w - 0.5, content_w));
                strvec_push(&body, "");
            }
#undef CAMERA_AT
            composed_t *act = &cols.v[r->active < cols.n ? r->active : 0];
            sel_start = act->sel_start >= 0 ? (int)body.n + act->sel_start : -1;
            sel_end = act->sel_end >= 0 ? (int)body.n + act->sel_end : -1;
            size_t height = 0;
            for (size_t i = 0; i < cols.n; i++) height = MAX(height, cols.v[i].lines.n);
            for (size_t row = 0; row < height; row++) {
                sb_t line = SB_INIT;
                int filled = 0;
                for (size_t i = 0; i < count; i++) {
                    int x0 = (int)i * stride - camera, x1 = x0 + col_w;
                    int v0 = MAX(0, x0), v1 = MIN(content_w, x1);
                    if (v1 <= v0) continue;
                    const char *src = row < cols.v[i].lines.n ? cols.v[i].lines.v[row] : "";
                    char *padded = tui_pad_to_width(src, col_w);
                    char *slice = tui_slice_cols(padded, v0 - x0, v1 - v0);
                    char *sp = tui_pad_to_width(slice, v1 - v0);
                    tui_sb_spaces(&line, MAX(0, v0 - filled));
                    sb_append(&line, sp);
                    filled = v1;
                    free(padded), free(slice), free(sp);
                }
                strvec_push_own(&body, sb_detach(&line));
            }
            for (size_t i = 0; i < cols.n; i++) strvec_free(&cols.v[i].lines);
            vec_free(&cols);
            const target_t *outlined = r->active > 0 && r->active - 1 < ncols && r->sib_sel < st->cols.v[r->active - 1]->targets.n
                                           ? &st->cols.v[r->active - 1]->targets.v[r->sib_sel]
                                           : NULL;
            if (sel_start >= 0) {
                if (r->active > 0)
                    anchor = xasprintf("rewind:sibling:%s:%s", st->cols.v[r->active - 1]->root_id,
                                       outlined ? outlined->entry_id : "");
                else anchor = xasprintf("rewind:main:%s", anc->turn_id);
            }
        } else {
            composed_t comp;
            compose(&r->main_rows, 0, 0, r->main_rows.n, r->targets.v, r->targets.n ? (ssize_t)r->selected : -1, content_w, NULL, &comp);
            body = comp.lines;
            sel_start = comp.sel_start, sel_end = comp.sel_end;
            if (sel_start >= 0) anchor = xasprintf("rewind:main:%s", r->targets.v[r->selected].turn_id);
        }
        char *pos = r->targets.n ? xasprintf("%zu/%zu  ", r->selected + 1, r->targets.n) : xstrdup("");
        char *lateral = xasprintf("%s %s", lr, ncols ? "branches" : "user turns");
        char *kf = ui_key_hint("f"), *ka = ui_key_hint("a"), *kx = ui_editor_key("app.tools.expand");
        if (!*kx) free(kx), kx = ui_key_hint("ctrl+o");
        char *earlier = r->truncated ? xasprintf("%s earlier turns  ", ka) : xstrdup("");
        char *keys = xasprintf("%s%s step  %s  %s filter  %s rewind  %s%s expand  %s cancel", pos, updown, lateral, kf, kenter,
                               earlier, kx, cancel);
        footer = S(TUI_TC_DIM, keys);
        free(pos), free(lateral), free(kf), free(ka), free(kx), free(earlier), free(keys);
    }
    free(updown), free(lr), free(kenter), free(cancel);

    /* TranscriptBrowser frame: rule, header, rule, body viewport, footer, rule */
    int height = term_rows(r);
    int body_h = MAX(3, height - 2 - 3);
    reveal(r, anchor, sel_start, sel_end, content_w, body_h, (int)body.n);
    free(anchor);
    sb_t rule = SB_INIT;
    sb_repeat(&rule, tui_sym("boxRound.horizontal"), MAX(1, width));
    char *border = S(TUI_TC_BORDER, sb_cstr(&rule));
    sb_free(&rule);
    char *hdr = header_line();
    char *ht = tui_truncate_to_width(hdr, MAX(0, width - 1), NULL), *ft = tui_truncate_to_width(footer, MAX(0, width - 1), NULL);
    strvec_push(out, border);
    strvec_push_own(out, xasprintf(" %s", ht));
    strvec_push(out, border);
    ui_scroll_view(&body, body_h, &r->offset, width, out);
    strvec_push_own(out, xasprintf(" %s", ft));
    strvec_push(out, border);
    free(border), free(hdr), free(ht), free(ft), free(footer);
    strvec_free(&body);
}

/* ---------- input ---------- */

static void finish_select(rewind_t *r, const char *entry_id) {
    if (r->done || !entry_id) return;
    r->done = true;
    if (r->cb.on_select) r->cb.on_select(r->cb.ud, entry_id);
}

static void finish_cancel(rewind_t *r) {
    if (r->done) return;
    r->done = true;
    if (r->cb.on_cancel) r->cb.on_cancel(r->cb.ud);
}

static bool scroll_key(rewind_t *r, const tui_key_t *k) {
    int page = MAX(1, MAX(3, term_rows(r) - 5) - 1);
    if (tui_key_matches(k, "shift+up")) r->offset -= FAST_SCROLL;
    else if (tui_key_matches(k, "shift+down")) r->offset += FAST_SCROLL;
    else if (tui_key_matches(k, "pageup")) r->offset -= page;
    else if (tui_key_matches(k, "pagedown")) r->offset += page;
    else if (tui_key_matches(k, "home")) r->offset = 0;
    else if (tui_key_matches(k, "end")) r->offset = INT_MAX / 2;
    else return false;
    r->offset = MAX(0, r->offset);
    return true;
}

static void mouse(rewind_t *r, const tui_key_t *k) {
    const char *t = k->text;
    if (!t || strncmp(t, "\x1b[<", 3)) return;
    int b = atoi(t + 3);
    if ((b & 64) && !(b & 2)) r->offset = MAX(0, r->offset + ((b & 1) ? 3 : -3));
}

static void filter_key(rewind_t *r, const tui_key_t *k) {
    if (tui_kb(k, "tui.select.cancel") || tui_key_matches(k, "escape")) r->filtering = false;
    else if (tui_key_matches(k, "enter") || tui_key_matches(k, "ctrl+j")) {
        idxv_t m = {0};
        filter_matches(r, &m);
        bool has = false;
        for (size_t i = 0; i < m.n; i++) has |= m.v[i] == r->selected;
        vec_free(&m);
        if (has) finish_select(r, r->targets.v[r->selected].entry_id);
    } else if (tui_kb(k, "app.tools.expand")) r->expanded = !r->expanded;
    else if (tui_kb(k, "tui.select.up")) step_filtered(r, -1, false);
    else if (tui_key_matches(k, "left")) step_filtered(r, -1, true);
    else if (tui_kb(k, "tui.select.down")) step_filtered(r, 1, false);
    else if (tui_key_matches(k, "right")) step_filtered(r, 1, true);
    else if (tui_key_matches(k, "backspace") && !*ui_input_value(&r->filter)) r->filtering = false;
    else {
        char *before = xstrdup(ui_input_value(&r->filter));
        if (ui_input_key(&r->filter, k)) {
            if (strcmp(before, ui_input_value(&r->filter))) filter_changed(r);
        } else scroll_key(r, k);
        free(before);
    }
}

static bool rewind_key(tui_component_t *c, const tui_key_t *k) {
    rewind_t *r = (rewind_t *)c;
    if (r->done) return true;
    if (k->kind == TUI_KEY_MOUSE) mouse(r, k);
    else if (r->filtering) filter_key(r, k);
    else if (tui_kb(k, "tui.select.cancel") || tui_key_matches(k, "escape")) {
        finish_cancel(r);
        return true;
    } else if (str_eq(tui_key_text(k), "f")) {
        load_full(r);
        ui_input_free(&r->filter);
        r->filtering = true;
        r->active = 0;
        r->sib_sel = 0;
        stop_slide(r);
    } else if (tui_kb(k, "app.tools.expand")) r->expanded = !r->expanded;
    else if (tui_kb(k, "tui.select.up")) move_vertical(r, -1);
    else if (tui_kb(k, "tui.select.down")) move_vertical(r, 1);
    else if (tui_key_matches(k, "left")) {
        if (r->active > 0) slide_to(r, r->active - 1);
        else move(r, -1, true);
    } else if (tui_key_matches(k, "right")) {
        size_t n = strip_count(r);
        if (r->active < n) {
            r->sib_sel = 0;
            slide_to(r, r->active + 1);
        } else if (r->active == 0) move(r, 1, true);
    } else if (str_eq(tui_key_text(k), "a") || str_eq(tui_key_text(k), "A")) load_full(r);
    else if (tui_key_matches(k, "enter") || tui_key_matches(k, "ctrl+j")) {
        strip_t *s = r->active > 0 ? strip_columns(r) : NULL;
        const target_t *t = NULL;
        if (r->active > 0) {
            column_t *col = s && r->active - 1 < s->cols.n ? s->cols.v[r->active - 1] : NULL;
            t = col && r->sib_sel < col->targets.n ? &col->targets.v[r->sib_sel] : NULL;
        } else if (r->targets.n) t = &r->targets.v[r->selected];
        if (t) finish_select(r, t->entry_id);
        return true;
    } else scroll_key(r, k);
    tui_component_request_render(c);
    return true;
}

static void rewind_destroy(tui_component_t *c) {
    rewind_t *r = (rewind_t *)c;
    stop_slide(r);
    if (r->main) tui_component_destroy(&r->main->base);
    targets_free(&r->targets);
    for (size_t i = 0; i < r->strips.n; i++) strip_free(&r->strips.v[i]);
    vec_free(&r->strips);
    rows_free(&r->main_rows);
    free(r->visible);
    free(r->sib_visible);
    free(r->anchor_id);
    ui_input_free(&r->filter);
    json_free(r->entries);
    free(r);
}

static const tui_component_vt REWIND_VT = {"rewind-selector", rewind_render, rewind_key, NULL, rewind_destroy};

tui_component_t *ui_rewind_selector_new(ui_t *u, const ui_rewind_cb_t *cb) {
    session_t *s = u->app->session;
    if (!s || !s->leaf_id) return NULL;
    json_t *path = session_path_to(s, s->leaf_id);
    json_t *entries = json_arr();
    const json_t *e;
    json_arr_foreach(path, i, e) if (is_transcript_entry(e)) json_push(entries, json_clone(e));
    json_free(path);
    if (!json_len(entries)) {
        json_free(entries);
        return NULL;
    }
    rewind_t *r = xcalloc(1, sizeof *r);
    tui_component_init(&r->base, &REWIND_VT);
    r->base.wants_mouse = true;
    r->u = u;
    if (cb) r->cb = *cb;
    r->entries = entries;
    json_t *tail = recent_entries(entries);
    r->truncated = json_len(tail) < json_len(entries);
    load_main(r, tail);
    json_free(tail);
    if (!r->targets.n) {
        rewind_destroy(&r->base);
        return NULL;
    }
    r->selected = r->targets.n - 1;
    return &r->base;
}
