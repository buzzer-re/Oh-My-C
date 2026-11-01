/* render.c - component model, differential renderer, overlays, event loop. */
#include "tui/render.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#define LINE_TERMINATOR "\x1b[0m\x1b]8;;\x07"

/* ---------- component basics ---------- */

void tui_component_init(tui_component_t *c, const tui_component_vt *vt) {
    memset(c, 0, sizeof *c);
    c->vt = vt;
}

void tui_component_render(tui_component_t *c, int width, strvec_t *out) {
    if (!c || c->hidden || !c->vt || !c->vt->render) return;
    c->vt->render(c, width < 1 ? 1 : width, out);
}

bool tui_component_handle_key(tui_component_t *c, const tui_key_t *k) {
    if (!c || !c->vt || !c->vt->handle_key) return false;
    return c->vt->handle_key(c, k);
}

void tui_component_invalidate(tui_component_t *c) {
    if (c && c->vt && c->vt->invalidate) c->vt->invalidate(c);
}

void tui_component_destroy(tui_component_t *c) {
    if (!c) return;
    if (c->tui && tui_focus(c->tui) == c) tui_set_focus(c->tui, NULL);
    if (c->vt && c->vt->destroy) c->vt->destroy(c);
    else free(c);
}

void tui_component_request_render(tui_component_t *c) {
    if (c && c->tui) tui_request_render(c->tui);
}

/* ---------- container ---------- */

static void container_render(tui_component_t *c, int width, strvec_t *out) {
    tui_container_render_children((tui_container_t *)c, width, out);
}

static void container_invalidate(tui_component_t *c) {
    tui_container_t *ct = (tui_container_t *)c;
    for (size_t i = 0; i < ct->children.n; i++) tui_component_invalidate(ct->children.v[i]);
}

static void container_destroy(tui_component_t *c) {
    tui_container_destroy_children((tui_container_t *)c);
    free(c);
}

static const tui_component_vt CONTAINER_VT = {"container", container_render, NULL, container_invalidate,
                                              container_destroy};

void tui_container_init(tui_container_t *c) {
    memset(c, 0, sizeof *c);
    tui_component_init(&c->base, &CONTAINER_VT);
}

tui_container_t *tui_container_new(void) {
    tui_container_t *c = xmalloc(sizeof *c);
    tui_container_init(c);
    return c;
}

void tui_container_render_children(tui_container_t *c, int width, strvec_t *out) {
    for (size_t i = 0; i < c->children.n; i++) tui_component_render(c->children.v[i], width, out);
}

void tui_container_destroy_children(tui_container_t *c) {
    for (size_t i = 0; i < c->children.n; i++) tui_component_destroy(c->children.v[i]);
    vec_free(&c->children);
}

void tui_container_add(tui_container_t *c, tui_component_t *child) {
    if (!child) return;
    if (!child->tui) child->tui = c->base.tui;
    vec_push(&c->children, child);
    tui_component_request_render(&c->base);
}

void tui_container_insert(tui_container_t *c, size_t index, tui_component_t *child) {
    if (!child) return;
    if (index > c->children.n) index = c->children.n;
    vec_push(&c->children, child);
    memmove(c->children.v + index + 1, c->children.v + index, (c->children.n - 1 - index) * sizeof(child));
    c->children.v[index] = child;
    if (!child->tui) child->tui = c->base.tui;
    tui_component_request_render(&c->base);
}

bool tui_container_remove(tui_container_t *c, tui_component_t *child) {
    for (size_t i = 0; i < c->children.n; i++) {
        if (c->children.v[i] == child) {
            memmove(c->children.v + i, c->children.v + i + 1, (c->children.n - i - 1) * sizeof(child));
            c->children.n--;
            tui_component_request_render(&c->base);
            return true;
        }
    }
    return false;
}

void tui_container_clear(tui_container_t *c) {
    tui_container_destroy_children(c);
    tui_component_request_render(&c->base);
}

size_t tui_container_count(const tui_container_t *c) { return c->children.n; }

/* ---------- TUI state ---------- */

typedef struct {
    tui_component_t *comp;
    tui_overlay_opts_t opts;
    tui_component_t *prev_focus;
} overlay_t;

typedef struct {
    int id;
    int interval;
    int64_t due;
    bool repeat;
    bool removed;
    void (*fn)(void *);
    void *ud;
} timer_t_;

typedef struct {
    void (*fn)(void *);
    void *ud;
} posted_t;

struct tui {
    tui_term_t *term;
    tui_container_t *root;
    tui_component_t *focus;
    VEC(overlay_t) overlays;

    /* renderer state */
    strvec_t prev;
    int prev_width, prev_height;
    int hw_row;       /* content row the terminal cursor is on */
    int viewport_top; /* content row shown on screen row 0 (may be negative) */
    bool have_frame;
    bool show_hw_cursor;
    int min_interval;
    int64_t last_render;
    int render_count, full_count;

    /* cross-thread */
    pthread_mutex_t mu;
    pthread_t ui_thread;
    bool ui_thread_set;
    bool render_pending, full_pending;
    bool stop_requested;
    int stop_code;
    VEC(posted_t) posted;
    int wake[2];

    VEC(timer_t_) timers;
    int next_timer_id;

    tui_keydec_t dec;
    tui_key_hook_fn key_hook;
    void *key_hook_ud;
    tui_signal_hook_fn sig_hook;
    void *sig_hook_ud;
    bool started;
    bool input_eof;
    int start_screen_row; /* 0-based screen row where content row 0 starts */
};

static bool on_ui_thread(tui_t *t) { return t->ui_thread_set && pthread_equal(pthread_self(), t->ui_thread); }

static void wake(tui_t *t) {
    if (t->wake[1] < 0) return;
    char b = 1;
    ssize_t r = write(t->wake[1], &b, 1);
    (void)r;
}

tui_t *tui_new(tui_term_t *term) {
    tui_t *t = xcalloc(1, sizeof *t);
    t->term = term ? term : tui_term_new(STDIN_FILENO, STDOUT_FILENO);
    t->root = tui_container_new();
    t->root->base.tui = t;
    t->min_interval = 16;
    pthread_mutex_init(&t->mu, NULL);
    t->wake[0] = t->wake[1] = -1;
    if (pipe(t->wake) == 0) {
        for (int i = 0; i < 2; i++) {
            fcntl(t->wake[i], F_SETFD, FD_CLOEXEC);
            fcntl(t->wake[i], F_SETFL, fcntl(t->wake[i], F_GETFL) | O_NONBLOCK);
        }
    }
    tui_keydec_init(&t->dec);
    t->next_timer_id = 1;
    t->ui_thread = pthread_self();
    t->ui_thread_set = true;
    return t;
}

void tui_free(tui_t *t) {
    if (!t) return;
    tui_shutdown(t);
    t->focus = NULL;
    vec_free(&t->overlays);
    tui_component_destroy(&t->root->base);
    strvec_free(&t->prev);
    tui_keydec_free(&t->dec);
    vec_free(&t->timers);
    vec_free(&t->posted);
    if (t->wake[0] >= 0) close(t->wake[0]);
    if (t->wake[1] >= 0) close(t->wake[1]);
    pthread_mutex_destroy(&t->mu);
    tui_term_free(t->term);
    free(t);
}

tui_term_t *tui_term(tui_t *t) { return t->term; }
tui_container_t *tui_root(tui_t *t) { return t->root; }
tui_component_t *tui_focus(tui_t *t) { return t->focus; }
void tui_attach(tui_t *t, tui_component_t *c) {
    if (c) c->tui = t;
}

void tui_set_focus(tui_t *t, tui_component_t *c) {
    if (t->focus == c) return;
    if (t->focus) t->focus->focused = false;
    t->focus = c;
    if (c) {
        c->focused = true;
        c->tui = t;
    }
    tui_request_render(t);
}

void tui_set_key_hook(tui_t *t, tui_key_hook_fn fn, void *ud) {
    t->key_hook = fn;
    t->key_hook_ud = ud;
}

void tui_set_signal_hook(tui_t *t, tui_signal_hook_fn fn, void *ud) {
    t->sig_hook = fn;
    t->sig_hook_ud = ud;
}

void tui_set_show_hardware_cursor(tui_t *t, bool show) {
    t->show_hw_cursor = show;
    tui_request_render(t);
}

void tui_set_min_render_interval(tui_t *t, int ms) { t->min_interval = ms < 0 ? 0 : ms; }

const strvec_t *tui_prev_lines(const tui_t *t) { return &t->prev; }
int tui_render_count(const tui_t *t) { return t->render_count; }
int tui_full_redraw_count(const tui_t *t) { return t->full_count; }

/* ---------- overlays ---------- */

void tui_overlay_show(tui_t *t, tui_component_t *c, const tui_overlay_opts_t *opts) {
    overlay_t o;
    memset(&o, 0, sizeof o);
    o.comp = c;
    if (opts) o.opts = *opts;
    o.prev_focus = t->focus;
    c->tui = t;
    vec_push(&t->overlays, o);
    tui_set_focus(t, c);
    tui_request_render(t);
}

void tui_overlay_hide(tui_t *t, tui_component_t *c) {
    for (size_t i = 0; i < t->overlays.n; i++) {
        if (t->overlays.v[i].comp != c) continue;
        tui_component_t *pf = t->overlays.v[i].prev_focus;
        memmove(t->overlays.v + i, t->overlays.v + i + 1, (t->overlays.n - i - 1) * sizeof(overlay_t));
        t->overlays.n--;
        /* fix up later overlays that remembered c as their previous focus */
        for (size_t j = 0; j < t->overlays.n; j++)
            if (t->overlays.v[j].prev_focus == c) t->overlays.v[j].prev_focus = pf;
        if (t->focus == c) tui_set_focus(t, t->overlays.n ? t->overlays.v[t->overlays.n - 1].comp : pf);
        tui_request_render(t);
        return;
    }
}

bool tui_overlay_visible(const tui_t *t) { return t->overlays.n > 0; }

static char *composite_line(const char *base, const char *ol, int col, int ow, int width) {
    sb_t b = SB_INIT;
    char *before = tui_slice_cols(base, 0, col);
    sb_append(&b, before);
    tui_sb_spaces(&b, col - tui_visible_width(before));
    free(before);
    sb_append(&b, TUI_RESET);
    char *mid = tui_truncate_to_width(ol, ow, "");
    sb_append(&b, mid);
    sb_append(&b, TUI_RESET);
    sb_append(&b, TUI_LINK_CLOSE);
    tui_sb_spaces(&b, ow - tui_visible_width(mid));
    free(mid);
    int rest = width - col - ow;
    if (rest > 0) {
        char *after = tui_slice_cols(base, col + ow, rest);
        sb_append(&b, after);
        free(after);
    }
    return sb_detach(&b);
}

static void composite_overlays(tui_t *t, strvec_t *lines, int width, int height) {
    for (size_t oi = 0; oi < t->overlays.n; oi++) {
        overlay_t *o = &t->overlays.v[oi];
        int ow;
        if (o->opts.width > 0) ow = MIN(width, o->opts.width);
        else if (o->opts.width < 0) ow = MAX(1, width * MIN(100, -o->opts.width) / 100);
        else ow = MAX(1, width - 2 * MAX(0, o->opts.margin_x));
        int col = o->opts.width != 0 ? (width - ow) / 2 : MIN(MAX(0, o->opts.margin_x), width - 1);
        strvec_t ol = {0};
        tui_component_render(o->comp, ow, &ol);
        int maxh = o->opts.max_height > 0 ? MIN(o->opts.max_height, height) : height;
        int oh = MIN((int)ol.n, maxh);
        if (oh <= 0) {
            strvec_free(&ol);
            continue;
        }
        int off = MAX(0, o->opts.offset_y);
        /* region: last `vh` rows of content (padding content when shorter than the overlay) */
        int need = oh + off;
        while ((int)lines->n < need && (int)lines->n < height) strvec_push(lines, "");
        int vh = MIN(height, (int)lines->n);
        int vstart = (int)lines->n - vh;
        int row;
        if (o->opts.anchor == TUI_ANCHOR_TOP) row = off;
        else if (o->opts.anchor == TUI_ANCHOR_CENTER) row = (vh - oh) / 2;
        else row = vh - off - oh;
        if (row < 0) row = 0;
        for (int i = 0; i < oh && row + i < vh; i++) {
            size_t idx = (size_t)(vstart + row + i);
            char *nl = composite_line(lines->v[idx], ol.v[i], col, ow, width);
            free(lines->v[idx]);
            lines->v[idx] = nl;
        }
        strvec_free(&ol);
    }
}

/* ---------- renderer ---------- */

typedef struct {
    int row, col;
    bool found;
} cursor_pos_t;

static void strip_markers(char *s) {
    static const char M[] = TUI_CURSOR_MARKER;
    size_t ml = sizeof M - 1;
    char *p;
    while ((p = strstr(s, M))) memmove(p, p + ml, strlen(p + ml) + 1);
}

static cursor_pos_t extract_cursor(strvec_t *lines, int height) {
    cursor_pos_t cp = {0, 0, false};
    int top = MAX(0, (int)lines->n - height);
    for (int i = (int)lines->n - 1; i >= 0; i--) {
        char *s = lines->v[i];
        char *m = strstr(s, TUI_CURSOR_MARKER);
        if (!m) continue;
        if (!cp.found && i >= top) {
            cp.found = true;
            cp.row = i;
            cp.col = tui_visible_width_n(s, (size_t)(m - s));
        }
        strip_markers(s);
    }
    return cp;
}

static void normalize_lines(strvec_t *lines, int width) {
    for (size_t i = 0; i < lines->n; i++) {
        char *s = lines->v[i];
        for (char *p = s; *p; p++)
            if (*p == '\n' || *p == '\r') *p = ' ';
        if (tui_visible_width(s) > width) {
            char *tr = tui_truncate_to_width(s, width, "");
            free(s);
            s = tr;
        }
        lines->v[i] = xasprintf("%s%s", s, LINE_TERMINATOR);
        free(s);
    }
}

static void move_rows(tui_term_t *tm, int delta) {
    if (delta > 0) tui_term_printf(tm, "\x1b[%dB", delta);
    else if (delta < 0) tui_term_printf(tm, "\x1b[%dA", -delta);
}

static void position_cursor(tui_t *t, const cursor_pos_t *cp, int total) {
    tui_term_t *tm = t->term;
    if (!cp->found || total <= 0) {
        if (!tm->cursor_hidden) tui_term_cursor_show(tm, false);
        return;
    }
    int row = MIN(MAX(cp->row, 0), total - 1);
    move_rows(tm, row - t->hw_row);
    tui_term_printf(tm, "\x1b[%dG", cp->col + 1);
    t->hw_row = row;
    if (t->show_hw_cursor) {
        if (tm->cursor_hidden) tui_term_cursor_show(tm, true);
    } else if (!tm->cursor_hidden) {
        tui_term_cursor_show(tm, false);
    }
}

static void finish_frame(tui_t *t, strvec_t *lines, const cursor_pos_t *cp, int width, int height) {
    position_cursor(t, cp, (int)lines->n);
    tui_term_sync_end(t->term);
    tui_term_flush(t->term);
    strvec_free(&t->prev);
    t->prev = *lines;
    memset(lines, 0, sizeof *lines);
    t->prev_width = width;
    t->prev_height = height;
    t->have_frame = true;
    t->render_count++;
}

static void full_render(tui_t *t, strvec_t *lines, const cursor_pos_t *cp, int width, int height, bool clear) {
    tui_term_t *tm = t->term;
    tui_term_sync_begin(tm);
    if (clear) {
        tui_term_clear_screen(tm, true);
        t->viewport_top = 0;
        t->full_count++;
    } else {
        tui_term_puts(tm, "\r");
        t->viewport_top = -t->start_screen_row;
    }
    for (size_t i = 0; i < lines->n; i++) {
        if (i) tui_term_puts(tm, "\r\n");
        tui_term_puts(tm, lines->v[i]);
    }
    int last = MAX(0, (int)lines->n - 1);
    t->hw_row = last;
    if (last > t->viewport_top + height - 1) t->viewport_top = last - height + 1;
    finish_frame(t, lines, cp, width, height);
}

static void do_render(tui_t *t) {
    tui_term_t *tm = t->term;
    int width = MAX(1, tm->cols), height = MAX(1, tm->rows);
    strvec_t lines = {0};
    tui_component_render(&t->root->base, width, &lines);
    if (t->overlays.n) composite_overlays(t, &lines, width, height);
    cursor_pos_t cp = extract_cursor(&lines, height);
    normalize_lines(&lines, width);
    t->last_render = mono_ms();

    bool full = t->full_pending;
    t->full_pending = false;
    if (!t->have_frame) {
        full_render(t, &lines, &cp, width, height, false);
        return;
    }
    if (full || width != t->prev_width || height != t->prev_height) {
        full_render(t, &lines, &cp, width, height, true);
        return;
    }

    int nn = (int)lines.n, pn = (int)t->prev.n;
    int first = -1, last = -1;
    for (int i = 0; i < MAX(nn, pn); i++) {
        const char *o = i < pn ? t->prev.v[i] : "";
        const char *n = i < nn ? lines.v[i] : "";
        if (strcmp(o, n)) {
            if (first < 0) first = i;
            last = i;
        }
    }
    bool appended = nn > pn;
    if (appended) {
        if (first < 0) first = pn;
        last = nn - 1;
    }
    bool append_start = appended && first == pn && first > 0;

    if (first < 0) {
        tui_term_sync_begin(tm);
        finish_frame(t, &lines, &cp, width, height);
        return;
    }

    if (first >= nn) {
        /* only deletions at the end */
        int target = MAX(0, nn - 1);
        int extra = pn - nn;
        if (extra >= height || target < t->viewport_top) {
            full_render(t, &lines, &cp, width, height, true);
            return;
        }
        tui_term_sync_begin(tm);
        move_rows(tm, target - t->hw_row);
        tui_term_puts(tm, "\r");
        if (nn == 0) {
            /* clear from the first row down */
            for (int i = 0; i < extra; i++) {
                tui_term_puts(tm, "\x1b[2K");
                if (i < extra - 1) tui_term_puts(tm, "\x1b[1B");
            }
            if (extra > 1) tui_term_printf(tm, "\x1b[%dA", extra - 1);
        } else {
            tui_term_puts(tm, "\x1b[1B");
            for (int i = 0; i < extra; i++) {
                tui_term_puts(tm, "\r\x1b[2K");
                if (i < extra - 1) tui_term_puts(tm, "\x1b[1B");
            }
            tui_term_printf(tm, "\x1b[%dA", extra);
        }
        t->hw_row = target;
        finish_frame(t, &lines, &cp, width, height);
        return;
    }

    int move_target = append_start ? first - 1 : first;
    if (first < t->viewport_top || move_target < t->viewport_top) {
        full_render(t, &lines, &cp, width, height, true);
        return;
    }

    tui_term_sync_begin(tm);
    int vbottom = t->viewport_top + height - 1;
    if (move_target > vbottom) {
        int cur_screen = MIN(MAX(t->hw_row - t->viewport_top, 0), height - 1);
        int down = height - 1 - cur_screen;
        if (down > 0) tui_term_printf(tm, "\x1b[%dB", down);
        int scroll = move_target - vbottom;
        for (int i = 0; i < scroll; i++) tui_term_puts(tm, "\r\n");
        t->viewport_top += scroll;
        t->hw_row = move_target;
    }
    move_rows(tm, move_target - t->hw_row);
    tui_term_puts(tm, append_start ? "\r\n" : "\r");

    int render_end = MIN(last, nn - 1);
    for (int i = first; i <= render_end; i++) {
        if (i > first) tui_term_puts(tm, "\r\n");
        tui_term_puts(tm, "\x1b[2K");
        tui_term_puts(tm, lines.v[i]);
    }
    int final_row = render_end;
    if (pn > nn) {
        if (render_end < nn - 1) {
            tui_term_printf(tm, "\x1b[%dB", nn - 1 - render_end);
            final_row = nn - 1;
        }
        int extra = pn - nn;
        for (int i = 0; i < extra; i++) tui_term_puts(tm, "\r\n\x1b[2K");
        tui_term_printf(tm, "\x1b[%dA", extra);
    }
    t->hw_row = final_row;
    if (final_row > t->viewport_top + height - 1) t->viewport_top = final_row - height + 1;
    finish_frame(t, &lines, &cp, width, height);
}

void tui_render_now(tui_t *t) {
    pthread_mutex_lock(&t->mu);
    t->render_pending = false;
    pthread_mutex_unlock(&t->mu);
    do_render(t);
}

void tui_request_render(tui_t *t) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    bool was = t->render_pending;
    t->render_pending = true;
    pthread_mutex_unlock(&t->mu);
    if (!was && !on_ui_thread(t)) wake(t);
}

void tui_request_full_render(tui_t *t) {
    pthread_mutex_lock(&t->mu);
    t->full_pending = true;
    t->render_pending = true;
    pthread_mutex_unlock(&t->mu);
    if (!on_ui_thread(t)) wake(t);
}

void tui_post(tui_t *t, void (*fn)(void *ud), void *ud) {
    posted_t p = {fn, ud};
    pthread_mutex_lock(&t->mu);
    vec_push(&t->posted, p);
    pthread_mutex_unlock(&t->mu);
    wake(t);
}

void tui_stop(tui_t *t, int code) {
    pthread_mutex_lock(&t->mu);
    if (!t->stop_requested) t->stop_code = code;
    t->stop_requested = true;
    pthread_mutex_unlock(&t->mu);
    wake(t);
}

/* ---------- timers ---------- */

int tui_add_timer(tui_t *t, int interval_ms, bool repeat, void (*fn)(void *ud), void *ud) {
    timer_t_ tm;
    memset(&tm, 0, sizeof tm);
    tm.id = t->next_timer_id++;
    tm.interval = interval_ms < 1 ? 1 : interval_ms;
    tm.due = mono_ms() + tm.interval;
    tm.repeat = repeat;
    tm.fn = fn;
    tm.ud = ud;
    vec_push(&t->timers, tm);
    return tm.id;
}

void tui_remove_timer(tui_t *t, int id) {
    for (size_t i = 0; i < t->timers.n; i++)
        if (t->timers.v[i].id == id) t->timers.v[i].removed = true;
}

static void run_timers(tui_t *t, int64_t now) {
    size_t n = t->timers.n;
    for (size_t i = 0; i < n && i < t->timers.n; i++) {
        timer_t_ *tm = &t->timers.v[i];
        if (tm->removed || tm->due > now) continue;
        void (*fn)(void *) = tm->fn;
        void *ud = tm->ud;
        if (tm->repeat) {
            tm->due += tm->interval;
            if (tm->due <= now) tm->due = now + tm->interval;
        } else {
            tm->removed = true;
        }
        fn(ud); /* may add/remove timers (vector may move) */
    }
    size_t w = 0;
    for (size_t i = 0; i < t->timers.n; i++)
        if (!t->timers.v[i].removed) t->timers.v[w++] = t->timers.v[i];
    t->timers.n = w;
}

/* ---------- input & lifecycle ---------- */

void tui_dispatch_key(tui_t *t, const tui_key_t *k) {
    if (k->kind == TUI_KEY_RESPONSE) {
        if (k->text) tui_term_handle_response(t->term, k->text, k->text_len);
        return;
    }
    if (t->key_hook && t->key_hook(t, k, t->key_hook_ud)) {
        tui_request_render(t);
        return;
    }
    tui_component_t *target = t->overlays.n ? t->overlays.v[t->overlays.n - 1].comp : t->focus;
    if (k->kind == TUI_KEY_MOUSE && target && target->wants_mouse && t->overlays.n) {
        tui_component_handle_key(target, k);
        tui_request_render(t);
        return;
    }
    if (k->kind == TUI_KEY_FOCUS_IN || k->kind == TUI_KEY_FOCUS_OUT || k->kind == TUI_KEY_MOUSE) return;
    if (target) tui_component_handle_key(target, k);
    tui_request_render(t);
}

static void process_keys(tui_t *t, int64_t now) {
    tui_key_t k;
    while (!t->stop_requested && tui_keydec_next(&t->dec, &k, now)) {
        tui_dispatch_key(t, &k);
        tui_key_free(&k);
    }
}

void tui_feed_input(tui_t *t, const char *data, size_t n) {
    tui_keydec_feed(&t->dec, data, n);
    process_keys(t, mono_ms());
}

/* Ask the terminal where the cursor is (CPR) so inline content starting mid-screen is
 * tracked correctly. Other bytes received meanwhile are kept as input. */
static int query_cursor_row(tui_t *t) {
    tui_term_t *tm = t->term;
    if (!tm->is_tty) return 0;
    tui_term_puts(tm, "\x1b[6n");
    tui_term_flush(tm);
    char buf[256];
    size_t len = 0;
    int64_t deadline = mono_ms() + 200;
    int row = 0;
    while (len < sizeof buf - 1) {
        int left = (int)(deadline - mono_ms());
        if (left <= 0) break;
        struct pollfd p = {tm->in_fd, POLLIN, 0};
        if (poll(&p, 1, left) <= 0) break;
        ssize_t r = read(tm->in_fd, buf + len, sizeof buf - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
        buf[len] = 0;
        /* find ESC [ digits ; digits R */
        for (size_t i = 0; i + 1 < len; i++) {
            if (buf[i] != '\x1b' || buf[i + 1] != '[') continue;
            size_t j = i + 2;
            int a = 0, b = 0;
            bool semi = false, ok = true, da = false, db = false;
            while (j < len && ((buf[j] >= '0' && buf[j] <= '9') || buf[j] == ';')) {
                if (buf[j] == ';') {
                    if (semi) { ok = false; break; }
                    semi = true;
                } else if (!semi) {
                    a = a * 10 + (buf[j] - '0');
                    da = true;
                } else {
                    b = b * 10 + (buf[j] - '0');
                    db = true;
                }
                if (a > 100000 || b > 100000) { ok = false; break; }
                j++;
            }
            if (ok && semi && da && db && j < len && buf[j] == 'R') {
                row = a;
                tui_keydec_feed(&t->dec, buf, i);
                tui_keydec_feed(&t->dec, buf + j + 1, len - j - 1);
                return row;
            }
        }
    }
    tui_keydec_feed(&t->dec, buf, len);
    return 0;
}

void tui_start(tui_t *t) {
    if (t->started) return;
    t->ui_thread = pthread_self();
    t->ui_thread_set = true;
    tui_term_install_signals(t->term);
    tui_term_start(t->term);
    tui_term_query_size(t->term);
    if (!t->have_frame) {
        int row = query_cursor_row(t);
        t->start_screen_row = row > 0 ? MIN(row - 1, t->term->rows - 1) : 0;
    }
    t->started = true;
    tui_request_render(t);
}

static void move_below_content(tui_t *t) {
    if (!t->have_frame) return;
    tui_term_t *tm = t->term;
    int last = MAX(0, (int)t->prev.n - 1);
    move_rows(tm, last - t->hw_row);
    t->hw_row = last;
    if (t->prev.n) tui_term_puts(tm, "\r\n");
    else tui_term_puts(tm, "\r");
    tui_term_flush(tm);
}

void tui_shutdown(tui_t *t) {
    if (!t->started) return;
    move_below_content(t);
    tui_term_stop(t->term);
    t->started = false;
}

void tui_suspend(tui_t *t) {
    if (!t->started) return;
    move_below_content(t);
    tui_term_stop(t->term);
    tui_term_set_loop_active(false);
    struct sigaction dfl, old;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGTSTP, &dfl, &old);
    raise(SIGTSTP);
    /* resumed */
    sigaction(SIGTSTP, &old, NULL);
    tui_term_set_loop_active(true);
    tui_term_start(t->term);
    tui_term_query_size(t->term);
    /* start a fresh inline frame below whatever the shell printed */
    strvec_free(&t->prev);
    t->have_frame = false;
    int row = query_cursor_row(t);
    t->start_screen_row = row > 0 ? MIN(row - 1, t->term->rows - 1) : 0;
    tui_request_render(t);
}

static void handle_signal(tui_t *t, int sig) {
    if (sig == SIGWINCH) {
        int oc = t->term->cols, orows = t->term->rows;
        tui_term_query_size(t->term);
        if (oc != t->term->cols || orows != t->term->rows) {
            tui_component_invalidate(&t->root->base);
            tui_request_full_render(t);
        }
        return;
    }
    if (sig == SIGTSTP) {
        tui_suspend(t);
        return;
    }
    if (sig == SIGCONT) {
        if (t->started && !t->term->raw) tui_term_enter_raw(t->term);
        tui_request_full_render(t);
        return;
    }
    if (t->sig_hook && t->sig_hook(t, sig, t->sig_hook_ud)) return;
    tui_stop(t, 128 + sig);
}

static void drain(int fd) {
    char buf[256];
    while (read(fd, buf, sizeof buf) > 0) {
    }
}

bool tui_run_once(tui_t *t, int timeout_ms) {
    int64_t now = mono_ms();
    int to = timeout_ms;
#define CLAMP_TO(v)                            \
    do {                                       \
        int v_ = (int)(v);                     \
        if (v_ < 0) v_ = 0;                    \
        if (to < 0 || v_ < to) to = v_;        \
    } while (0)
    for (size_t i = 0; i < t->timers.n; i++)
        if (!t->timers.v[i].removed) CLAMP_TO(t->timers.v[i].due - now);
    int kt = tui_keydec_timeout_ms(&t->dec, now);
    if (kt >= 0) CLAMP_TO(kt);
    pthread_mutex_lock(&t->mu);
    bool rp = t->render_pending, stop = t->stop_requested;
    size_t npost = t->posted.n;
    pthread_mutex_unlock(&t->mu);
    if (stop) return false;
    if (npost) to = 0;
    if (rp) CLAMP_TO(t->last_render + t->min_interval - now);
#undef CLAMP_TO

    struct pollfd fds[3];
    int nf = 0, in_i = -1, sig_i = -1, wake_i = -1;
    if (t->started && !t->input_eof && t->term->in_fd >= 0) {
        in_i = nf;
        fds[nf++] = (struct pollfd){t->term->in_fd, POLLIN, 0};
    }
    if (tui_term_signal_fd() >= 0) {
        sig_i = nf;
        fds[nf++] = (struct pollfd){tui_term_signal_fd(), POLLIN, 0};
    }
    if (t->wake[0] >= 0) {
        wake_i = nf;
        fds[nf++] = (struct pollfd){t->wake[0], POLLIN, 0};
    }
    int pr = poll(fds, (nfds_t)nf, to);
    if (pr < 0 && errno != EINTR) return false;
    if (pr > 0) {
        if (wake_i >= 0 && (fds[wake_i].revents & POLLIN)) drain(t->wake[0]);
        if (sig_i >= 0 && (fds[sig_i].revents & POLLIN)) {
            int sig;
            while ((sig = tui_term_read_signal()) > 0) handle_signal(t, sig);
        }
        if (in_i >= 0 && (fds[in_i].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[65536];
            ssize_t r = read(t->term->in_fd, buf, sizeof buf);
            if (r > 0) tui_keydec_feed(&t->dec, buf, (size_t)r);
            else if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
                t->input_eof = true;
                tui_stop(t, 0);
            }
        }
    }
    now = mono_ms();
    process_keys(t, now);

    /* posted callbacks */
    pthread_mutex_lock(&t->mu);
    posted_t *pv = t->posted.v;
    size_t pn = t->posted.n;
    t->posted.v = NULL;
    t->posted.n = t->posted.cap = 0;
    pthread_mutex_unlock(&t->mu);
    for (size_t i = 0; i < pn; i++) pv[i].fn(pv[i].ud);
    free(pv);

    run_timers(t, mono_ms());

    now = mono_ms();
    pthread_mutex_lock(&t->mu);
    bool do_it = t->render_pending && (now - t->last_render >= t->min_interval || t->stop_requested);
    if (do_it) t->render_pending = false;
    stop = t->stop_requested;
    pthread_mutex_unlock(&t->mu);
    if (do_it && t->started) do_render(t);
    return !stop;
}

int tui_run(tui_t *t) {
    tui_start(t);
    tui_term_set_loop_active(true);
    pthread_mutex_lock(&t->mu);
    t->stop_requested = false;
    t->stop_code = 0;
    pthread_mutex_unlock(&t->mu);
    while (tui_run_once(t, -1)) {
    }
    tui_term_set_loop_active(false);
    /* final frame so the last state is visible in scrollback */
    if (t->render_pending) tui_render_now(t);
    tui_shutdown(t);
    return t->stop_code;
}
