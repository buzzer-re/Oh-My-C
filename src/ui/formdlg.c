/* formdlg.c - the ask tool's HTML form above the composer (port of omp packages/tui/src/html-form/dialog.ts
 * HtmlFormDialog, row rendering, chrome/ask-fold.ts and html-form/media.ts; hosted like extension-ui-controller
 * #showLocalFormDialog / #mountAsk).
 *
 * The form stacks in one column inside an "Ask" panel over the working row, the composer staying under it: the context
 * before the first question on top, then every question (a top-level `<fieldset>`, or a field of loose content between
 * fieldsets, with the context before it). The question holding focus is open, its context and the focused field's
 * controls shown; every other field folds to its question over its answer or option chips. Focus on Submit folds them
 * all. The panel ({@link DIALOG_HEIGHT_RATIO} of the terminal at most, leaving ASK_RESERVED_ROWS) scrolls to keep the
 * focused row in view, with the open question's head when it fits, and past a picture it would cut when it can; one it
 * must cut is a dim frame naming it.
 *
 * Keys: ↑/↓ move between choices and fields, crossing from a question's last row into the next, Tab/Shift+Tab jump
 * fields, Space toggles (or opens a dropdown), ←/→ change a dropdown, Enter picks a radio and moves on, Enter on Submit
 * validates and submits or opens the first question to fix, PgUp/PgDn (←/→ on choices) open the previous or next
 * question. Esc (outside an open dropdown or note) and Tab on Submit hand the keys to the composer, where Esc
 * interrupts the turn and so cancels the ask. ↑ on a question's first row first scrolls up to its head when that is
 * cut off, app.ask.hide collapses the form to one row, and the faint composer shows the form's key hints. With a
 * timeout, inactivity submits the current values.
 *
 * Tern's native card (native/ask-card.ts, pills a click picks) is not ported: omc has no native rendering. */
#include "ui/overlays.h"
#include "ui/ui.h"
#include "ui/signals.h"

#include "config/settings.h"
#include "core/fs.h"
#include "img/img.h"
#include "net/http.h"
#include "text/htmlform.h"
#include "tui/ansi.h"
#include "tui/termcaps.h"
#include "ui/tools/tc.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>

#define DIALOG_HEIGHT_RATIO 0.7
#define MIN_BODY_ROWS 6
#define CHROME_ROWS 2 /* top and bottom border */
/* chrome/ask-fold.ts ASK_RESERVED_ROWS: rows the panel leaves to the composer, its borders, the spacer and working row
 * over it, the status line and some slack */
#define ASK_RESERVED_ROWS 8
#define FOLD_INDENT "  " /* cells the folded summary and error sit in from the mark */
#define FIELD_INDENT 4
#define MAX_TEXTAREA_ROWS 10
#define DEFAULT_TITLE "Ask"
#define SUBMIT_LABEL "Submit"
#define CAPTION_MAX_ROWS 4
#define MAX_PICTURE_ROWS 16
#define PICTURE_SHARE 0.35
#define DROPDOWN_PAGE 5
#define OTHER_PLACEHOLDER "type your own"
#define WELL_PLACEHOLDER "type your answer" /* a folded text field's well when the form gives no placeholder */
#define PILL_LIMIT 4        /* native/ask-card.ts: option pills a folded question shows */
#define PILL_LABEL_WIDTH 20 /* widest a pill's label grows, in cells, before it is cut */
#define MAX_FORM_IMAGE_BYTES (10 * 1024 * 1024)
#define FETCH_TIMEOUT_MS 10000

enum { STOP_OPTION, STOP_OTHER, STOP_SELECT, STOP_TEXT, STOP_SUBMIT };

typedef struct {
    int kind;
    hf_field_t *field;
    int option;
} stop_t;

enum { MEDIA_LOADING, MEDIA_FAILED, MEDIA_READY };

struct media_job;

typedef struct {
    const hf_block_t *blk;
    int state;
    char *reason;      /* failed */
    char *b64, *mime;  /* ready raster (NULL b64: an SVG drawing, which omc cannot rasterize) */
    char *key;         /* image key (Kitty ids) */
    struct media_job *job;
} media_t;

/* blocks [start, end) of one form section */
typedef struct {
    size_t section, start, end;
} part_t;
typedef VEC(part_t) parts_t;

/* a question group: the section parts it shows while open, and the fields in them */
typedef struct {
    parts_t parts;
    VEC(hf_field_t *) fields;
} group_t;

typedef struct fd {
    tui_component_t base;
    ui_form_opts_t o;
    hf_form_t *form;
    uint64_t id;
    VEC(stop_t) stops;
    bool **checked;          /* per field index (choice) */
    ui_input_t *inputs;      /* per field index (single-line text) */
    tui_editor_t **editors;  /* per field index (textarea) */
    ui_input_t *others;      /* per field index (choice with `other`) */
    char **other_seen;       /* per field index: a radio group's Other text as last seen */
    char **errors;           /* per field index */
    size_t nerrors;
    int focus;
    bool sole; /* the keys reach one field only: Enter on it submits (omp 05c0e53513) */
    parts_t intro;           /* context above every question group */
    VEC(group_t) groups;     /* question groups, the one holding focus open */
    int *field_group;        /* per field index: its group */
    int scroll;              /* first column row the body shows */
    /* set while ↑ scrolls up to the open question's cut-off head: the body keeps its offset instead of following
     * focus until another key brings focus back */
    bool peek;
    struct {
        bool valid;
        int focus, offset, rows, head;
    } view;                  /* the last body render: the focus it showed, its offset, height and head row */
    char *height_key;
    int height_rows;
    hf_field_t *open;        /* dropdown showing its options */
    int highlight;
    int64_t deadline;
    int remaining;           /* -1: no countdown */
    int timer_id;
    VEC(media_t) media;
    int media_version;
    bool hidden, closed;
    char ***notes;            /* per field index (choice): per option, the saved note (NULL: none) */
    int hold;                 /* the run shows `blocked` on this form (ui_signals_hold_blocked); 0: none */
    tui_editor_t *note_ed;    /* the note being edited (NULL: none) */
    hf_field_t *note_field;
    int note_option;
} fd_t;

static const tui_component_vt FD_VT;
static pthread_mutex_t g_live_mu = PTHREAD_MUTEX_INITIALIZER;
static fd_t *g_live;
static uint64_t g_next_id = 1;

/* ---------- text helpers ---------- */

static char *fg_bold(tui_tc_t slot, const char *s) {
    char *f = fg(slot, s), *b = tui_bold(f);
    free(f);
    return b;
}

/* sanitize: tabs expanded, control bytes (escape sequences, \r) dropped, \n kept */
static char *sanitize(const char *s) {
    char *t = tui_replace_tabs(s ? s : "");
    sb_t b = SB_INIT;
    for (const char *p = t; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c < 0x20 && c != '\n') || c == 0x7f) continue;
        sb_putc(&b, *p);
    }
    free(t);
    return sb_detach(&b);
}

/* sanitize + whitespace runs to one space */
static char *sanitize_line(const char *s) {
    char *a = sanitize(s);
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = a; *p; p++) {
        if (isspace((unsigned char)*p)) {
            if (!sp) sb_putc(&b, ' ');
            sp = true;
        } else {
            sb_putc(&b, *p);
            sp = false;
        }
    }
    free(a);
    return sb_detach(&b);
}

static char *option_text(const hf_option_t *o) {
    char *t = hf_spans_text(&o->label), *s = sanitize_line(t);
    free(t);
    char *r = hf_one_line(s);
    free(s);
    return r;
}

static const char *marker_sym(bool multi, bool selected) {
    return tui_sym(multi ? (selected ? "checkbox.checked" : "checkbox.unchecked") : (selected ? "radio.selected" : "radio.unselected"));
}

static char *pad(int n) {
    sb_t b = SB_INIT;
    tui_sb_spaces(&b, MAX(0, n));
    return sb_detach(&b);
}

/* truncateToWidth (Ellipsis.Unicode): the native truncation resets before the ellipsis only when it copied SGR codes,
 * so a plain string's ellipsis keeps the color it is wrapped in */
static char *trunc_with(const char *s, int w, const char *ellipsis) {
    char *r = tui_truncate_to_width(s, MAX(0, w), ellipsis);
    if (s && !strchr(s, '\x1b')) {
        char *at = strstr(r, TUI_RESET);
        if (at) memmove(at, at + strlen(TUI_RESET), strlen(at + strlen(TUI_RESET)) + 1);
    }
    return r;
}

static char *trunc_to(const char *s, int w) { return trunc_with(s, w, NULL); }

/* the base style a span run takes: `slot` color, optionally bold */
typedef struct {
    tui_tc_t slot;
    bool bold;
} base_t;

static char *base_apply(base_t b, const char *text) { return b.bold ? fg_bold(b.slot, text) : fg(b.slot, text); }

/* styleSpans: one line of spans over `base`; code keeps the markdown code color */
static char *style_line(const hf_spans_t *line, base_t base) {
    sb_t out = SB_INIT;
    for (size_t i = 0; i < line->n; i++) {
        char *t = sanitize(line->v[i].text), *s;
        if (line->v[i].style == HF_CODE) s = fg(TUI_TC_MD_CODE, t);
        else if (line->v[i].style == HF_STRONG) {
            char *x = base_apply(base, t);
            s = tui_bold(x);
            free(x);
        } else if (line->v[i].style == HF_EM) {
            char *x = base_apply(base, t);
            s = tui_italic(x);
            free(x);
        } else s = base_apply(base, t);
        sb_append(&out, s);
        free(s);
        free(t);
    }
    return sb_detach(&out);
}

/* hang: spans wrapped under `prefix`, continuation and hard-break rows hanging at the prefix column */
static void hang(const char *prefix, const hf_spans_t *spans, base_t base, int width, strvec_t *out) {
    char *indent = pad(tui_visible_width(prefix));
    hf_spans_t line = {0};
    bool first = true;
    for (size_t i = 0;; i++) {
        bool end = i >= spans->n;
        const char *text = end ? "" : spans->v[i].text;
        const char *p = text;
        for (;;) {
            const char *nl = end ? NULL : strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            if (len) {
                hf_span_t sp = {xstrndup(p, len), end ? HF_PLAIN : spans->v[i].style};
                vec_push(&line, sp);
            }
            if (!nl) break;
            char *styled = style_line(&line, base);
            tc_wrap_prefixed(first ? prefix : indent, indent, styled, width, out);
            free(styled);
            first = false;
            for (size_t k = 0; k < line.n; k++) free(line.v[k].text);
            line.n = 0;
            p = nl + 1;
        }
        if (end) break;
    }
    char *styled = style_line(&line, base);
    tc_wrap_prefixed(first ? prefix : indent, indent, styled, width, out);
    free(styled);
    for (size_t k = 0; k < line.n; k++) free(line.v[k].text);
    free(line.v);
    free(indent);
}

static char *editor_keys2(const char *a, const char *b) {
    char *x = ui_editor_key(a), *y = ui_editor_key(b), *r = xasprintf("%s/%s", x, y);
    free(x);
    free(y);
    return r;
}

static int term_rows(const fd_t *v) {
    if (v->o.tui) return tui_term(v->o.tui)->rows;
    return v->o.term_rows > 0 ? v->o.term_rows : 40;
}

/* ---------- state ---------- */

static size_t fidx(const hf_field_t *f) { return f->index; }

static bool is_dropdown(const hf_field_t *f) { return f->select && !f->multi; }

static char *text_value(const fd_t *v, const hf_field_t *f) {
    if (f->multiline) return tui_editor_get_expanded_text(v->editors[fidx(f)]);
    return xstrdup(ui_input_value(&v->inputs[fidx(f)]));
}

/* #otherChosen: the Other answer counts when it has text and, for a radio group, no option is picked; typing into
 * a radio group's Other drops the picked option */
static bool other_chosen(fd_t *v, const hf_field_t *f) {
    if (!f->other) return false;
    size_t i = fidx(f);
    const char *text = ui_input_value(&v->others[i]);
    if (!f->multi && strcmp(text, v->other_seen[i] ? v->other_seen[i] : "")) {
        free(v->other_seen[i]);
        v->other_seen[i] = xstrdup(text);
        char *t = str_trim(text);
        if (*t) memset(v->checked[i], 0, f->opts.n * sizeof(bool));
        free(t);
    }
    char *t = str_trim(text);
    bool blank = !*t;
    free(t);
    if (blank) return false;
    if (f->multi) return true;
    for (size_t k = 0; k < f->opts.n; k++)
        if (v->checked[i][k]) return false;
    return true;
}

static json_t *field_value(fd_t *v, const hf_field_t *f) {
    if (f->kind == HF_TEXT) {
        char *t = text_value(v, f);
        json_t *r = hf_text_value(f, t);
        free(t);
        return r;
    }
    const char *other = other_chosen(v, f) ? ui_input_value(&v->others[fidx(f)]) : NULL;
    return hf_choice_value(f, v->checked[fidx(f)], other);
}

static json_t *form_data(fd_t *v) {
    json_t *d = json_obj();
    for (size_t i = 0; i < v->form->hidden_names.n; i++) json_set_str(d, v->form->hidden_names.v[i], v->form->hidden_values.v[i]);
    for (size_t i = 0; i < v->form->fields.n; i++) {
        hf_field_t *f = v->form->fields.v[i];
        if (!*f->name) continue;
        json_t *x = field_value(v, f);
        if (x) json_set(d, f->name, x);
    }
    return d;
}

static int current(const fd_t *v, const hf_field_t *f) {
    for (size_t k = 0; k < f->opts.n; k++)
        if (v->checked[fidx(f)][k]) return (int)k;
    return -1;
}

static void set_error(fd_t *v, const hf_field_t *f, char *err) {
    size_t i = fidx(f);
    if (v->errors[i]) v->nerrors--;
    free(v->errors[i]);
    v->errors[i] = err;
    if (err) v->nerrors++;
}

static void choose(fd_t *v, const hf_field_t *f, int option) {
    memset(v->checked[fidx(f)], 0, f->opts.n * sizeof(bool));
    v->checked[fidx(f)][option] = true;
    set_error(v, f, NULL);
}

static void toggle(fd_t *v, const hf_field_t *f, int option) {
    if (!f->multi) {
        choose(v, f, option);
        return;
    }
    v->checked[fidx(f)][option] = !v->checked[fidx(f)][option];
    set_error(v, f, NULL);
}

/* #step: the nearest enabled option `delta` steps from `from`, clamped to the ends; `from` when none */
static int step(const hf_field_t *f, int from, int delta) {
    int dir = delta > 0 ? 1 : -1, found = from, remaining = abs(delta);
    for (int i = from + dir; i >= 0 && i < (int)f->opts.n && remaining > 0; i += dir) {
        if (f->opts.v[i].disabled) continue;
        found = i;
        remaining--;
    }
    return found;
}

/* #typeAhead: the next enabled option after `from` (wrapping) whose label starts with `typed` */
static int type_ahead(const hf_field_t *f, int from, const char *typed) {
    size_t n = f->opts.n, tl = strlen(typed);
    for (size_t off = 1; off <= n; off++) {
        size_t i = (size_t)((from + (int)off) % (int)n + (int)n) % n;
        if (f->opts.v[i].disabled) continue;
        char *t = option_text(&f->opts.v[i]);
        bool m = strlen(t) >= tl && !strncasecmp(t, typed, tl);
        free(t);
        if (m) return (int)i;
    }
    return -2;
}

static void request(fd_t *v) {
    if (v->o.tui) tui_request_render(v->o.tui);
}

/* ---------- media (html-form/media.ts) ---------- */

typedef struct media_job {
    uint64_t owner;
    size_t slot;
    char *src, *cwd;
    tui_t *tui;
    volatile sig_atomic_t cancel;
    char *b64, *mime, *reason;
} media_job_t;

static bool svg_like(const uint8_t *d, size_t n) {
    size_t m = MIN(n, 4096);
    for (size_t i = 0; i + 4 < m; i++)
        if (d[i] == '<' && tolower(d[i + 1]) == 's' && tolower(d[i + 2]) == 'v' && tolower(d[i + 3]) == 'g' &&
            (isspace(d[i + 4]) || d[i + 4] == '>'))
            return true;
    return false;
}

static int hexv(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? tolower(c) - 'a' + 10 : -1; }

/* readSource: an http(s) URL, a data: URI, a file: URL or a path (`~/`, absolute or relative to cwd) */
static uint8_t *read_source(media_job_t *j, size_t *n, char **err) {
    const char *src = j->src;
    *n = 0;
    if (!strncasecmp(src, "data:", 5)) {
        const char *comma = strchr(src, ',');
        if (!comma) {
            *err = xstrdup("malformed data: URI");
            return NULL;
        }
        size_t hl = (size_t)(comma - src - 5);
        bool b64 = hl >= 7 && !strncasecmp(comma - 7, ";base64", 7);
        const char *payload = comma + 1;
        if (b64) {
            sb_t s = SB_INIT;
            for (const char *p = payload; *p; p++)
                if (!isspace((unsigned char)*p)) sb_putc(&s, *p);
            uint8_t *d = base64_decode(sb_cstr(&s), s.len, n);
            sb_free(&s);
            if (!d) *err = xstrdup("Invalid base64");
            return d;
        }
        sb_t s = SB_INIT;
        for (const char *p = payload; *p; p++) {
            if (*p == '%' && hexv((unsigned char)p[1]) >= 0 && hexv((unsigned char)p[2]) >= 0) {
                sb_putc(&s, (char)(hexv((unsigned char)p[1]) * 16 + hexv((unsigned char)p[2])));
                p += 2;
            } else sb_putc(&s, *p);
        }
        *n = s.len;
        return (uint8_t *)sb_detach(&s);
    }
    if (!strncasecmp(src, "http://", 7) || !strncasecmp(src, "https://", 8)) {
        http_req_t req = {.url = src, .timeout_ms = FETCH_TIMEOUT_MS, .max_redirects = 10,
                          .max_body = MAX_FORM_IMAGE_BYTES + 1, .cancel = &j->cancel};
        http_resp_t resp = {0};
        uint8_t *d = NULL;
        if (http_request(&req, &resp) != 0) *err = xstrdup(resp.error ? resp.error : "fetch failed");
        else if (resp.status < 200 || resp.status > 299) *err = xasprintf("HTTP %d", resp.status);
        else {
            const char *cl = http_headers_get(&resp.headers, "content-length");
            if ((cl && strtod(cl, NULL) > MAX_FORM_IMAGE_BYTES) || resp.truncated)
                *err = xasprintf("larger than %d MiB", MAX_FORM_IMAGE_BYTES >> 20);
            else {
                d = (uint8_t *)resp.body;
                *n = resp.body_len;
                resp.body = NULL;
            }
        }
        http_resp_free(&resp);
        return d;
    }
    char *path;
    if (!strncasecmp(src, "file:", 5)) {
        const char *p = src + 5;
        if (!strncmp(p, "//", 2)) {
            p += 2;
            const char *slash = strchr(p, '/');
            p = slash ? slash : p + strlen(p);
        }
        char *dec = url_decode(p);
        path = dec;
    } else if (!strcmp(src, "~") || !strncmp(src, "~/", 2)) {
        const char *home = getenv("HOME");
        path = xasprintf("%s%s", home ? home : "", src + 1);
    } else if (src[0] == '/') path = xstrdup(src);
    else path = xasprintf("%s/%s", j->cwd && *j->cwd ? j->cwd : ".", src);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > MAX_FORM_IMAGE_BYTES) {
        *err = xasprintf("larger than %d MiB", MAX_FORM_IMAGE_BYTES >> 20);
        free(path);
        return NULL;
    }
    size_t len = 0;
    char *d = fs_read(path, &len);
    if (!d) *err = xasprintf("ENOENT: no such file or directory, open '%s'", path);
    free(path);
    *n = len;
    return (uint8_t *)d;
}

/* loadFormImage */
static void media_load(media_job_t *j) {
    size_t n = 0;
    char *err = NULL;
    uint8_t *d = read_source(j, &n, &err);
    if (!d) {
        j->reason = err ? err : xstrdup("unreadable");
        return;
    }
    img_meta_t m;
    if (n > MAX_FORM_IMAGE_BYTES) j->reason = xasprintf("larger than %d MiB", MAX_FORM_IMAGE_BYTES >> 20);
    else if (img_sniff(d, MIN(n, 64), &m) && img_fmt_mime(m.fmt)) {
        j->b64 = base64_encode(d, n);
        j->mime = xstrdup(img_fmt_mime(m.fmt));
    } else if (svg_like(d, n)) j->mime = xstrdup("image/svg+xml");
    else j->reason = xstrdup("not a PNG, JPEG, GIF, WebP or SVG image");
    free(d);
}

static void media_job_free(media_job_t *j) {
    free(j->src);
    free(j->cwd);
    free(j->b64);
    free(j->mime);
    free(j->reason);
    free(j);
}

static void media_settle(fd_t *v, media_t *m, media_job_t *j) {
    m->job = NULL;
    if (j->reason) {
        m->state = MEDIA_FAILED;
        m->reason = j->reason;
        j->reason = NULL;
    } else {
        m->state = MEDIA_READY;
        m->b64 = j->b64;
        m->mime = j->mime;
        j->b64 = j->mime = NULL;
    }
    v->media_version++;
}

/* UI thread: a background load landed */
static void media_done(void *ud) {
    media_job_t *j = ud;
    pthread_mutex_lock(&g_live_mu);
    fd_t *v = g_live && g_live->id == j->owner && !j->cancel ? g_live : NULL;
    pthread_mutex_unlock(&g_live_mu);
    if (v && j->slot < v->media.n && v->media.v[j->slot].job == j) {
        media_settle(v, &v->media.v[j->slot], j);
        request(v);
    }
    media_job_free(j);
}

static void *media_thread(void *p) {
    media_job_t *j = p;
    media_load(j);
    tui_post(j->tui, media_done, j);
    return NULL;
}

/* an inline <svg> is ready at once; an <img> loads in the background and re-renders when it lands */
static void init_media(fd_t *v, const hf_block_t *blk) {
    media_t m = {.blk = blk, .state = MEDIA_LOADING};
    m.key = xasprintf("askform-%llu-%zu", (unsigned long long)v->id, v->media.n);
    if (blk->kind == HF_B_SVG) {
        m.state = MEDIA_READY;
        m.mime = xstrdup("image/svg+xml");
        vec_push(&v->media, m);
        return;
    }
    media_job_t *j = xcalloc(1, sizeof *j);
    j->owner = v->id;
    j->slot = v->media.n;
    j->src = xstrdup(blk->src);
    j->cwd = xstrdup(v->o.cwd ? v->o.cwd : ".");
    j->tui = v->o.tui;
    m.job = j;
    vec_push(&v->media, m);
    pthread_t th;
    if (!v->o.tui || pthread_create(&th, NULL, media_thread, j) != 0) {
        /* no UI loop (tests): load inline */
        media_load(j);
        media_settle(v, &v->media.v[v->media.n - 1], j);
        media_job_free(j);
        return;
    }
    pthread_detach(th);
}

static media_t *media_of(fd_t *v, const hf_block_t *blk) {
    for (size_t i = 0; i < v->media.n; i++)
        if (v->media.v[i].blk == blk) return &v->media.v[i];
    return NULL;
}

/* rows one picture may fill: a third of the terminal, within sensible bounds */
static int picture_rows(const fd_t *v) { return MAX(4, MIN(MAX_PICTURE_ROWS, (int)floor(term_rows(v) * PICTURE_SHARE))); }

/* one dim row standing in for a picture that is loading or could not be shown */
static char *media_note(const hf_block_t *blk, const media_t *m) {
    char *name;
    if (blk->alt && *blk->alt) name = sanitize(blk->alt);
    else if (blk->kind == HF_B_IMAGE) {
        char *s = xstrndup(blk->src, 80);
        name = sanitize(s);
        free(s);
    } else name = xstrdup("drawing");
    char *r;
    if (m->state == MEDIA_FAILED) {
        char *why = sanitize(m->reason);
        r = xasprintf("\xe2\x96\xa3 %s \xe2\x80\x94 couldn't load: %s", name, why);
        free(why);
    } else r = xasprintf("\xe2\x96\xa3 %s \xe2\x80\x94 loading\xe2\x80\xa6", name);
    free(name);
    return r;
}

static void media_rows(fd_t *v, const hf_block_t *blk, const media_t *m, int width, strvec_t *out) {
    if (m->state != MEDIA_READY || !m->b64) return; /* an SVG drawing stays blank: omc has no SVG rasterizer */
    tui_image_opts_t io = tui_image_default_opts(term_rows(v), (int)settings_num("tui.maxInlineImageColumns", 100),
                                                 (int)settings_num("tui.maxInlineImageRows", 20));
    io.max_rows = picture_rows(v);
    char sgr[48];
    snprintf(sgr, sizeof sgr, "%s", tui_fg_seq(NULL, TUI_TC_DIM));
    const char *alt = blk->alt && *blk->alt ? blk->alt : "image";
    tui_image_render(m->b64, m->mime, m->key, alt, width, &io, sgr, out);
}

/* ---------- countdown ---------- */

static void stop_countdown(fd_t *v) {
    if (v->timer_id > 0 && v->o.tui) tui_remove_timer(v->o.tui, v->timer_id);
    v->timer_id = 0;
    v->remaining = -1;
}

static void reset_countdown(fd_t *v) {
    if (v->remaining < 0 || v->o.timeout_ms <= 0) return;
    v->deadline = mono_ms() + v->o.timeout_ms;
    v->remaining = (int)((v->o.timeout_ms + 999) / 1000);
}

static const fd_t *g_closed; /* the dialog a key just closed (compared, never dereferenced) */

static void ask_unmount(ui_t *u, tui_component_t *ask);

static void finish(fd_t *v, json_t *data, json_t *notes, bool timed_out) {
    g_closed = v;
    if (v->closed) {
        json_free(data);
        json_free(notes);
        return;
    }
    v->closed = true;
    stop_countdown(v);
    ui_form_done_fn done = v->o.done;
    void *ud = v->o.ud;
    if (v->o.ui) ask_unmount(v->o.ui, &v->base); /* destroys v */
    if (done) done(ud, data, notes, timed_out);
    else {
        json_free(data);
        json_free(notes);
    }
}

/* ---------- option notes ---------- */

/* optionNotes: saved notes on selected options ({name, question, option, value, text}); unselected drafts stay
 * private to the dialog */
static json_t *option_notes(fd_t *v) {
    json_t *arr = json_arr();
    for (size_t i = 0; i < v->form->fields.n; i++) {
        hf_field_t *f = v->form->fields.v[i];
        size_t fi = fidx(f);
        if (f->kind == HF_TEXT || !v->notes[fi] || !*f->name) continue;
        other_chosen(v, f);
        for (size_t k = 0; k < f->opts.n; k++) {
            const char *saved = v->notes[fi][k];
            char *text = saved ? str_trim(saved) : NULL;
            if (!text || !*text || f->opts.v[k].disabled || !v->checked[fi][k]) {
                free(text);
                continue;
            }
            json_t *n = json_obj();
            json_set_str(n, "name", f->name);
            json_set_str(n, "question", f->question && *f->question ? f->question : f->name);
            char *ot = option_text(&f->opts.v[k]);
            json_set_str(n, "option", *ot ? ot : f->opts.v[k].value);
            free(ot);
            json_set_str(n, "value", f->opts.v[k].value);
            json_set_str(n, "text", text);
            free(text);
            json_push(arr, n);
        }
    }
    if (!json_len(arr)) {
        json_free(arr);
        return NULL;
    }
    return arr;
}

/* app.ask.note keys; on a dropdown a text-typing key (`n`, `shift+n`) stays type-ahead */
static strvec_t note_keys(bool dropdown) {
    strvec_t out = {0};
    const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), "app.ask.note");
    for (size_t i = 0; keys && i < keys->n; i++) {
        const char *k = keys->v[i];
        bool typing = strlen(k) == 1 || (str_starts(k, "shift+") && strlen(k + 6) == 1);
        if (dropdown && typing) continue;
        strvec_push(&out, k);
    }
    return out;
}

static bool matches_note_key(const tui_key_t *k, bool dropdown) {
    strvec_t keys = note_keys(dropdown);
    bool hit = false;
    for (size_t i = 0; i < keys.n && !hit; i++) hit = tui_key_matches(k, keys.v[i]);
    strvec_free(&keys);
    return hit;
}

/* the marker after a noted option: a submitted note, or a draft on an unchosen option (NULL: none; owned) */
static char *note_tag(fd_t *v, const hf_field_t *f, int option, int *width) {
    char **n = v->notes[fidx(f)];
    if (width) *width = 0;
    if (!n || option < 0 || !n[option]) return NULL;
    bool chosen = v->checked[fidx(f)][option];
    const char *text = chosen ? "  \xe2\x9c\x8e note" : "  \xe2\x9c\x8e draft";
    if (width) *width = tui_visible_width(text);
    return fg(chosen ? TUI_TC_SUCCESS : TUI_TC_DIM, text);
}

/* the open note's key hints; a note on an unchosen option is kept but not submitted */
static char *note_hint(fd_t *v) {
    bool chosen = v->checked[fidx(v->note_field)][v->note_option];
    return xasprintf("enter save \xc2\xb7 blank removes \xc2\xb7 esc keep%s", chosen ? "" : " \xc2\xb7 sent only if chosen");
}

static void close_note(fd_t *v) {
    if (v->note_ed) tui_component_destroy(&v->note_ed->base);
    v->note_ed = NULL;
    v->note_field = NULL;
}

static void save_note(fd_t *v) {
    char *raw = tui_editor_get_expanded_text(v->note_ed), *text = str_trim(raw);
    free(raw);
    char **n = v->notes[fidx(v->note_field)];
    free(n[v->note_option]);
    n[v->note_option] = *text ? text : NULL;
    if (!*text) free(text);
    close_note(v);
}

static void start_note(fd_t *v, int target) {
    const stop_t *s = &v->stops.v[v->focus];
    if (v->hidden || (s->kind != STOP_OPTION && s->kind != STOP_SELECT)) return;
    v->peek = false; /* the editor draws under the focused stop, so the body must show it */
    hf_field_t *f = s->field;
    int option = target >= 0 ? target : s->kind == STOP_OPTION ? s->option : v->open ? v->highlight : current(v, f);
    if (option < 0 || option >= (int)f->opts.n || f->opts.v[option].disabled) return;
    close_note(v);
    tui_editor_t *ed = tui_editor_new();
    tui_editor_set_composer(ed, "borderless");
    free(ed->gutter);
    ed->gutter = xstrdup("");
    ed->padding_x = 0;
    ed->max_height = 4;
    ed->disable_submit = true;
    if (v->o.draft) {
        snprintf(ed->text_fg, sizeof ed->text_fg, "%s", v->o.draft->text_fg);
        ed->use_terminal_cursor = v->o.draft->use_terminal_cursor;
    }
    const char *saved = v->notes[fidx(f)] ? v->notes[fidx(f)][option] : NULL;
    tui_editor_set_text(ed, saved ? saved : "");
    ed->cl = 0; /* a saved note reopens at its start */
    ed->cc = 0;
    v->note_ed = ed;
    v->note_field = f;
    v->note_option = option;
}

static void note_key(fd_t *v, const tui_key_t *k) {
    if (tui_kb(k, "tui.select.cancel")) close_note(v);
    else if (tui_kb(k, "tui.input.submit")) save_note(v);
    else tui_editor_handle_key(v->note_ed, k);
}

static void on_tick(void *ud) {
    fd_t *v = ud;
    int64_t left = v->deadline - mono_ms();
    if (left <= 0) {
        stop_countdown(v);
        if (v->note_ed) save_note(v);
        finish(v, form_data(v), option_notes(v), true);
        return;
    }
    v->remaining = (int)((left + 999) / 1000);
    request(v);
}

/* ---------- focus ---------- */

/* the field of stop `i`; NULL for Submit */
static hf_field_t *field_of_stop(const fd_t *v, int i) {
    if (i < 0 || i >= (int)v->stops.n) return NULL;
    const stop_t *s = &v->stops.v[i];
    return s->kind == STOP_SUBMIT ? NULL : s->field;
}

/* #groupOfStop: the question group holding stop `i`; groups.n for Submit */
static int group_of_stop(const fd_t *v, int i) {
    hf_field_t *f = field_of_stop(v, i);
    if (!f) return (int)v->groups.n;
    return v->field_group[fidx(f)];
}

static void set_focus(fd_t *v, int index) {
    v->focus = MAX(0, MIN((int)v->stops.n - 1, index));
    v->peek = false;
}

/* #moveStop: move focus `delta` stops, across questions. ↑ on an open question's first stop whose head the body cuts
 * off scrolls up toward that head first, a body less a row at a time; the next ↑ at the head crosses into the previous
 * question */
static void move_stop(fd_t *v, int delta) {
    int group = group_of_stop(v, v->focus);
    if (delta < 0 && v->view.valid && v->view.focus == v->focus && v->view.offset > v->view.head &&
        (v->focus == 0 || group_of_stop(v, v->focus - 1) != group)) {
        v->peek = true;
        v->scroll = MAX(v->view.head, v->view.offset - MAX(1, v->view.rows - 1));
        return;
    }
    set_focus(v, v->focus + delta);
}

static const void *field_of(const stop_t *s) { return s->kind == STOP_SUBMIT ? (const void *)s : (const void *)s->field; }


/* #answerStop: the stop where `f`'s answer is: its checked option, its chosen Other, else its first stop; Submit's
 * stop when `f` has none (omp 6b213b6d3b) */
static int answer_stop(fd_t *v, const hf_field_t *f) {
    int first = -1;
    for (size_t at = 0; at < v->stops.n; at++) {
        const stop_t *st = &v->stops.v[at];
        if (st->kind == STOP_SUBMIT || st->field != f) continue;
        if (first < 0) first = (int)at;
        if ((st->kind == STOP_OPTION && f->kind == HF_CHOICE && v->checked[fidx(f)][st->option]) ||
            (st->kind == STOP_OTHER && other_chosen(v, f)))
            return (int)at;
    }
    return first >= 0 ? first : (int)v->stops.n - 1;
}

/* jump to the next (or previous) field, at its answer; Submit counts as a field */
static void move_field(fd_t *v, int delta) {
    const void *cur = field_of(&v->stops.v[v->focus]);
    int i = v->focus;
    while (i + delta >= 0 && i + delta < (int)v->stops.n && field_of(&v->stops.v[i]) == cur) i += delta;
    const stop_t *t = &v->stops.v[i];
    set_focus(v, t->kind != STOP_SUBMIT ? answer_stop(v, t->field) : i);
}

static void submit(fd_t *v);

/* #advance: Enter on a field moves to the next one, or submits when the form asks only this one */
static void advance(fd_t *v) {
    if (v->sole) submit(v);
    else move_field(v, 1);
}

/* #jumpQuestion: open the previous or next question group at its first field's answer; past the last, Submit */
static void jump_question(fd_t *v, int delta) {
    int target = group_of_stop(v, v->focus) + delta;
    for (int i = 0; i < (int)v->stops.n; i++)
        if (group_of_stop(v, i) == target) {
            hf_field_t *f = field_of_stop(v, i);
            set_focus(v, f ? answer_stop(v, f) : i);
            return;
        }
}

static void submit(fd_t *v) {
    for (size_t i = 0; i < v->form->fields.n; i++) set_error(v, v->form->fields.v[i], NULL);
    for (size_t i = 0; i < v->form->fields.n; i++) {
        hf_field_t *f = v->form->fields.v[i];
        json_t *x = field_value(v, f);
        char *e = hf_field_error(f, x);
        json_free(x);
        if (e) set_error(v, f, e);
    }
    if (!v->nerrors) {
        stop_countdown(v);
        finish(v, form_data(v), option_notes(v), false);
        return;
    }
    for (size_t s = 0; s < v->stops.n; s++)
        if (v->stops.v[s].kind != STOP_SUBMIT && v->errors[fidx(v->stops.v[s].field)]) {
            set_focus(v, (int)s);
            break;
        }
}

/* ---------- keys ---------- */

static bool is_confirm(const tui_key_t *k) { return tui_kb(k, "tui.select.confirm"); }

static const char *printable(const tui_key_t *k) {
    const char *t = tui_key_text(k);
    return t && *t && (unsigned char)*t >= 0x20 ? t : NULL;
}

static void text_key(fd_t *v, hf_field_t *f, const tui_key_t *k) {
    bool up = tui_kb(k, "tui.select.up"), down = tui_kb(k, "tui.select.down");
    size_t i = fidx(f);
    if (!f->multiline) {
        if (up || down) {
            move_stop(v, down ? 1 : -1);
            return;
        }
        if (tui_kb(k, "tui.input.submit") || (k->kind == TUI_KEY_CHAR && k->codepoint == '\n' && !k->mods)) {
            advance(v);
            return;
        }
        char *before = xstrdup(ui_input_value(&v->inputs[i]));
        ui_input_key(&v->inputs[i], k);
        if (strcmp(before, ui_input_value(&v->inputs[i]))) set_error(v, f, NULL);
        free(before);
        return;
    }
    tui_editor_t *ed = v->editors[i];
    if ((up && ed->cl == 0) || (down && ed->cl + 1 >= ed->lines.n)) {
        move_stop(v, down ? 1 : -1);
        return;
    }
    if (tui_kb(k, "tui.input.submit")) {
        advance(v);
        return;
    }
    char *before = tui_editor_get_text(ed);
    tui_editor_handle_key(ed, k);
    char *after = tui_editor_get_text(ed);
    if (strcmp(before, after)) set_error(v, f, NULL);
    free(before);
    free(after);
}

/* the Other row is a text field: keys type into it, ↑/↓ leave it, Enter moves to the next field */
static void other_key(fd_t *v, hf_field_t *f, const tui_key_t *k) {
    if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) {
        move_stop(v, tui_kb(k, "tui.select.down") ? 1 : -1);
        return;
    }
    ui_input_t *in = &v->others[fidx(f)];
    if (is_confirm(k)) {
        /* back on Other after picking an option: Enter makes the kept text the answer again */
        char *t = str_trim(ui_input_value(in));
        if (!f->multi && *t) memset(v->checked[fidx(f)], 0, f->opts.n * sizeof(bool));
        free(t);
        advance(v);
        return;
    }
    char *before = xstrdup(ui_input_value(in));
    ui_input_key(in, k);
    if (strcmp(before, ui_input_value(in))) set_error(v, f, NULL);
    free(before);
}

/* a closed dropdown: ←/→ change it, Space opens it, a letter jumps to a matching option */
static void select_key(fd_t *v, hf_field_t *f, const tui_key_t *k) {
    if (matches_note_key(k, true)) {
        start_note(v, -1);
        return;
    }
    int cur = current(v, f);
    if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) {
        move_stop(v, tui_kb(k, "tui.select.down") ? 1 : -1);
        return;
    }
    if (tui_key_matches(k, "left") || tui_key_matches(k, "right")) {
        int next = step(f, cur, tui_key_matches(k, "right") ? 1 : -1);
        if (next >= 0) choose(v, f, next);
        return;
    }
    if (tui_key_matches(k, "space")) {
        v->open = f;
        v->highlight = cur >= 0 ? cur : step(f, -1, 1);
        return;
    }
    if (is_confirm(k)) {
        advance(v);
        return;
    }
    const char *typed = printable(k);
    int m = typed ? type_ahead(f, cur, typed) : -2;
    if (m >= 0) choose(v, f, m);
}

/* an open dropdown owns the keys until a pick (Enter/Space), Esc or Tab closes it */
static void dropdown_key(fd_t *v, const tui_key_t *k) {
    hf_field_t *f = v->open;
    if (matches_note_key(k, true)) {
        start_note(v, v->highlight);
        return;
    }
    if (tui_kb(k, "tui.select.cancel")) {
        v->open = NULL;
        return;
    }
    if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) {
        v->highlight = step(f, v->highlight, tui_kb(k, "tui.select.down") ? 1 : -1);
        return;
    }
    if (tui_kb(k, "tui.select.pageUp") || tui_kb(k, "tui.select.pageDown")) {
        v->highlight = step(f, v->highlight, tui_kb(k, "tui.select.pageDown") ? DROPDOWN_PAGE : -DROPDOWN_PAGE);
        return;
    }
    if (tui_key_matches(k, "space") || is_confirm(k)) {
        if (v->highlight >= 0) choose(v, f, v->highlight);
        v->open = NULL;
        return;
    }
    if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab")) {
        v->open = NULL;
        move_field(v, tui_key_matches(k, "shift+tab") ? -1 : 1);
        return;
    }
    const char *typed = printable(k);
    int m = typed ? type_ahead(f, v->highlight, typed) : -2;
    if (m >= 0) v->highlight = m;
}

static void set_hidden(fd_t *v, bool hidden) {
    if (v->hidden == hidden) return;
    v->hidden = hidden;
    v->open = NULL;
}

static void release_keys(fd_t *v) {
    if (v->o.release_keys) v->o.release_keys(v->o.hook_ud);
}

static bool fd_key(tui_component_t *c, const tui_key_t *k) {
    fd_t *v = (fd_t *)c;
    if (v->closed) return true;
    g_closed = NULL;
    reset_countdown(v);
    if (tui_kb(k, "app.ask.hide")) {
        set_hidden(v, !v->hidden);
        request(v);
        return true;
    }
    if (v->note_ed && !v->hidden) {
        note_key(v, k);
        request(v);
        return true;
    }
    if (v->open) {
        dropdown_key(v, k);
        request(v);
        return true;
    }
    /* Esc hands the keys to the composer, where Esc interrupts the turn and so cancels the ask */
    if (tui_kb(k, "tui.select.cancel")) {
        release_keys(v);
        return true;
    }
    if (v->hidden) {
        if (is_confirm(k)) set_hidden(v, false);
        request(v);
        return true;
    }
    bool confirm = is_confirm(k), up = tui_kb(k, "tui.select.up"), down = tui_kb(k, "tui.select.down");
    if (v->peek && !up) {
        /* focus sits below the rows shown: the keys that would act on it bring it back instead of acting unseen */
        v->peek = false;
        if (down || tui_key_matches(k, "space") || confirm) {
            request(v);
            return true;
        }
    }
    if (tui_kb(k, "tui.select.pageDown") || tui_kb(k, "tui.select.pageUp")) {
        jump_question(v, tui_kb(k, "tui.select.pageDown") ? 1 : -1);
        request(v);
        return true;
    }
    stop_t *s = &v->stops.v[v->focus];
    if (tui_key_matches(k, "tab") && s->kind == STOP_SUBMIT) {
        release_keys(v);
        return true;
    }
    if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab")) {
        move_field(v, tui_key_matches(k, "shift+tab") ? -1 : 1);
        request(v);
        return true;
    }
    if (s->kind == STOP_TEXT) text_key(v, s->field, k);
    else if (s->kind == STOP_SELECT) select_key(v, s->field, k);
    else if (s->kind == STOP_OTHER) other_key(v, s->field, k);
    else if (up || down) move_stop(v, down ? 1 : -1);
    else if (tui_key_matches(k, "left") || tui_key_matches(k, "right")) jump_question(v, tui_key_matches(k, "right") ? 1 : -1);
    else if (s->kind == STOP_SUBMIT) {
        if (confirm) {
            submit(v);
            if (g_closed == v) { /* submitted: v is gone */
                g_closed = NULL;
                return true;
            }
        }
    } else if (matches_note_key(k, false)) start_note(v, -1);
    else if (tui_key_matches(k, "space")) toggle(v, s->field, s->option);
    else if (confirm) {
        /* Enter picks a radio; on a checkbox group with nothing checked it checks the focused box */
        bool none = true;
        for (size_t q = 0; q < s->field->opts.n; q++)
            if (v->checked[fidx(s->field)][q]) none = false;
        if (!s->field->multi || none) toggle(v, s->field, s->option);
        advance(v);
    }
    if (g_closed == v) { /* a sole field's Enter submitted: v is gone */
        g_closed = NULL;
        return true;
    }
    request(v);
    return true;
}

/* ---------- rows ---------- */

/* whether the keyboard can reach `f`: a text field, or a choice with an enabled option or an Other answer */
static bool reachable(const hf_field_t *f) {
    if (f->kind == HF_TEXT || f->other) return true;
    for (size_t k = 0; k < f->opts.n; k++)
        if (!f->opts.v[k].disabled) return true;
    return false;
}

/* #answered: `f` holds an answer that passes its rules */
static bool answered(fd_t *v, const hf_field_t *f) {
    json_t *x = field_value(v, f);
    bool r = false;
    if (x && !(json_is_str(x) && !*json_sv(x)) && !(json_is_arr(x) && !json_len(x))) {
        char *e = hf_field_error(f, x);
        r = !e;
        free(e);
    }
    json_free(x);
    return r;
}

/* #progress: `2 of 5 answered`, for forms of two or more questions the keyboard reaches (NULL otherwise) */
static char *progress_text(fd_t *v) {
    size_t named = 0, done = 0;
    for (size_t i = 0; i < v->form->fields.n; i++) {
        hf_field_t *f = v->form->fields.v[i];
        if (!*f->name || !reachable(f)) continue;
        named++;
        done += answered(v, f);
    }
    return named < 2 ? NULL : xasprintf("%zu of %zu answered", done, named);
}

static char *title_text(fd_t *v) {
    const char *t = v->o.title ? v->o.title : DEFAULT_TITLE;
    char *p = progress_text(v);
    char *base = p ? xasprintf("%s \xc2\xb7 %s", t, p) : xstrdup(t);
    free(p);
    if (v->remaining < 0) return base;
    char *r = xasprintf("%s (%ds)", base, v->remaining);
    free(base);
    return r;
}

static char *cursor_prefix(bool focused) {
    if (!focused) return xstrdup("  ");
    char *c = xasprintf("%s ", tui_sym("nav.cursor")), *r = fg(TUI_TC_ACCENT, c);
    free(c);
    return r;
}

/* Input.render with prompt "": windowed value, cursor cell (or `override` at the end), padded to width */
static char *input_row(const ui_input_t *in, int width, bool focused, bool mask, const char *override) {
    ui_input_t tmp = {0};
    const ui_input_t *src = in;
    if (mask) {
        const char *s = ui_input_value(in);
        size_t cur = 0;
        for (size_t i = 0; s[i]; i++) {
            if (((unsigned char)s[i] & 0xC0) == 0x80) continue;
            if (i < in->cur) cur++;
            sb_append(&tmp.v, "\xe2\x80\xa2");
        }
        tmp.cur = cur * 3;
        src = &tmp;
    }
    char *r;
    if (override && src->cur >= src->v.len) {
        int ow = tui_visible_width(override);
        const char *s = ui_input_value(src);
        int avail = width;
        int bw = tui_visible_width(s);
        int skip = MAX(0, bw + ow - avail);
        char *before = tui_slice_cols(s, skip, bw - skip);
        sb_t b = SB_INIT;
        sb_append(&b, before);
        if (focused) sb_append(&b, TUI_CURSOR_MARKER);
        sb_append(&b, override);
        tui_sb_spaces(&b, MAX(0, avail - tui_visible_width(before) - ow));
        free(before);
        r = sb_detach(&b);
    } else r = ui_input_render(src, "", width, focused);
    sb_free(&tmp.v);
    return r;
}

/* `○ Other: <text>`: an inline input while focused, else the text or a dim prompt */
static char *other_line(fd_t *v, hf_field_t *f, bool focused, int width) {
    bool chosen = other_chosen(v, f);
    ui_input_t *in = &v->others[fidx(f)];
    char *cur = cursor_prefix(focused);
    char *mk = fg(chosen ? TUI_TC_SUCCESS : TUI_TC_DIM, marker_sym(f->multi, chosen));
    char *lab = fg(focused ? TUI_TC_ACCENT : chosen ? TUI_TC_TOOL_OUTPUT : TUI_TC_TEXT, HF_OTHER_LABEL ": ");
    char *prefix = xasprintf("%s%s %s", cur, mk, lab);
    int inner = MAX(1, width - tui_visible_width(prefix));
    const char *value = ui_input_value(in);
    char *shown;
    if (focused) {
        char *ov = NULL;
        if (!*value) {
            char *ph = trunc_to(OTHER_PLACEHOLDER, MAX(1, inner - 1)), *d = fg(TUI_TC_DIM, ph);
            ov = xasprintf("\x1b[7m \x1b[27m%s", d);
            free(ph);
            free(d);
        }
        shown = input_row(in, inner, v->base.focused && !v->hidden && !v->note_ed, false, ov);
        free(ov);
    } else if (*value) {
        char *s = sanitize(value), *t = trunc_to(s, inner);
        shown = fg(chosen ? TUI_TC_TOOL_OUTPUT : TUI_TC_DIM, t);
        free(s);
        free(t);
    } else shown = fg(TUI_TC_DIM, OTHER_PLACEHOLDER);
    char *r = xasprintf("%s%s", prefix, shown);
    free(shown);
    free(prefix);
    free(lab);
    free(mk);
    free(cur);
    return r;
}

/* label rows with the focus cursor, a required star and an optional dim suffix */
static void label_lines(const hf_field_t *f, bool focused, const char *suffix, int width, strvec_t *out) {
    char *cur = cursor_prefix(focused);
    size_t first = out->n;
    base_t base = {focused ? TUI_TC_ACCENT : TUI_TC_TEXT, false};
    hang(cur, &f->label, base, width, out);
    if (out->n == first) strvec_push(out, cur);
    char *req = f->required ? fg(TUI_TC_ERROR, " *") : xstrdup("");
    char *last = out->v[out->n - 1];
    out->v[out->n - 1] = xasprintf("%s%s%s", last, req, suffix);
    free(last);
    free(req);
    free(cur);
}

/* a dropdown's label and its one value row: `‹ us-east-1 (N. Virginia) ›  1/8` */
static void dropdown_field_lines(fd_t *v, const hf_field_t *f, bool focused, int width, strvec_t *out) {
    int cur = current(v, f);
    int enabled = 0;
    for (size_t k = 0; k < f->opts.n; k++) enabled += !f->opts.v[k].disabled;
    char *position = NULL;
    if (cur < 0) position = xstrdup("");
    else {
        char *p = xasprintf("  %d/%zu", cur + 1, f->opts.n), *d = fg(TUI_TC_DIM, p), *tag = note_tag(v, f, cur, NULL);
        position = xasprintf("%s%s", d, tag ? tag : "");
        free(p), free(d), free(tag);
    }
    int inner = MAX(1, width - FIELD_INDENT - 4 - tui_visible_width(position));
    char *value;
    if (cur < 0) value = fg(TUI_TC_DIM, "\xe2\x80\x94");
    else {
        char *t = option_text(&f->opts.v[cur]), *tr = trunc_to(t, inner);
        value = fg(focused ? TUI_TC_ACCENT : TUI_TC_TOOL_OUTPUT, tr);
        free(t);
        free(tr);
    }
    tui_tc_t arrows = focused && enabled > 1 ? TUI_TC_ACCENT : TUI_TC_DIM;
    char *l = fg(arrows, "\xe2\x80\xb9 "), *r = fg(arrows, " \xe2\x80\xba");
    char *row = xasprintf("%s%s%s%s", l, value, r, position);
    if (!f->label.n) {
        char *c = cursor_prefix(focused), *req = f->required ? fg(TUI_TC_ERROR, " *") : xstrdup("");
        strvec_push_own(out, xasprintf("%s%s%s", c, row, req));
        free(c);
        free(req);
    } else {
        label_lines(f, focused, "", width, out);
        char *bar = fg(focused ? TUI_TC_ACCENT : TUI_TC_BORDER, tui_sym("boxRound.vertical"));
        char *ind = pad(FIELD_INDENT - 2);
        strvec_push_own(out, xasprintf("%s%s %s", ind, bar, row));
        free(ind);
        free(bar);
    }
    free(row);
    free(l);
    free(r);
    free(value);
    free(position);
}

/* textarea height: the `rows` asked for, grown to fit a prefilled draft, capped */
static int textarea_rows(const hf_field_t *f) {
    int draft = 1;
    for (const char *p = f->value; *p; p++) draft += *p == '\n';
    double rows = MAX(2.0, MAX(f->rows, (double)draft));
    return (int)MIN((double)MAX_TEXTAREA_ROWS, rows);
}

static void text_field_lines(fd_t *v, const hf_field_t *f, bool focused, int width, strvec_t *out) {
    char *bounds;
    char *mn = NULL, *mx = NULL;
    if (f->has_min) {
        sb_t b = SB_INIT;
        json_js_num_sb(&b, f->min);
        mn = sb_detach(&b);
    }
    if (f->has_max) {
        sb_t b = SB_INIT;
        json_js_num_sb(&b, f->max);
        mx = sb_detach(&b);
    }
    if (mn && mx) bounds = xasprintf("%s\xe2\x80\x93%s", mn, mx);
    else if (mn) bounds = xasprintf("\xe2\x89\xa5 %s", mn);
    else if (mx) bounds = xasprintf("\xe2\x89\xa4 %s", mx);
    else bounds = xstrdup("");
    free(mn);
    free(mx);
    char *suffix;
    if (*bounds) {
        char *b2 = xasprintf("  %s", bounds);
        suffix = fg(TUI_TC_DIM, b2);
        free(b2);
    } else suffix = xstrdup("");
    label_lines(f, focused, suffix, width, out);
    free(suffix);
    free(bounds);
    char *bar = fg(focused ? TUI_TC_ACCENT : TUI_TC_BORDER, tui_sym("boxRound.vertical"));
    char *ind = pad(FIELD_INDENT - 2);
    char *prefix = xasprintf("%s%s ", ind, bar);
    free(ind);
    free(bar);
    int inner = MAX(1, width - FIELD_INDENT);
    char *value = text_value(v, f);
    strvec_t rows = {0};
    bool live = v->base.focused && !v->hidden && !v->note_ed;
    if (f->multiline) {
        int height = textarea_rows(f);
        if (focused) {
            tui_editor_t *ed = v->editors[fidx(f)];
            ed->base.focused = live;
            ed->base.tui = v->base.tui;
            tui_component_render(&ed->base, inner, &rows);
        } else if (*value) {
            char *s = sanitize(value);
            strvec_t ls = str_split(s, "\n");
            for (size_t i = 0; i < ls.n; i++) {
                char *t = trunc_to(ls.v[i], inner);
                strvec_push_own(&rows, fg(TUI_TC_TOOL_OUTPUT, t));
                free(t);
            }
            strvec_free(&ls);
            free(s);
        } else {
            char *s = sanitize(f->placeholder), *t = trunc_to(s, inner);
            strvec_push_own(&rows, fg(TUI_TC_DIM, t));
            free(s);
            free(t);
        }
        if ((int)rows.n > height) {
            while ((int)rows.n > height) free(rows.v[--rows.n]);
            if (!focused) {
                free(rows.v[height - 1]);
                char *t = trunc_to("\xe2\x80\xa6", inner);
                rows.v[height - 1] = fg(TUI_TC_DIM, t);
                free(t);
            }
        }
        while ((int)rows.n < height) strvec_push(&rows, "");
    } else if (focused) {
        char *ov = NULL;
        if (!*value && *f->placeholder) {
            char *s = sanitize(f->placeholder), *ph = trunc_to(s, MAX(1, inner - 1)), *d = fg(TUI_TC_DIM, ph);
            ov = xasprintf("\x1b[7m \x1b[27m%s", d);
            free(s);
            free(ph);
            free(d);
        }
        strvec_push_own(&rows, input_row(&v->inputs[fidx(f)], inner, live, str_eq(f->type, "password"), ov));
        free(ov);
    } else if (*value) {
        char *shown;
        if (str_eq(f->type, "password")) {
            sb_t b = SB_INIT;
            for (size_t i = 0; i < hf_utf16_len(value); i++) sb_append(&b, "\xe2\x80\xa2");
            shown = sb_detach(&b);
        } else shown = sanitize(value);
        char *t = trunc_to(shown, inner);
        strvec_push_own(&rows, fg(TUI_TC_TOOL_OUTPUT, t));
        free(t);
        free(shown);
    } else {
        char *s = sanitize(f->placeholder), *t = trunc_to(s, inner);
        strvec_push_own(&rows, fg(TUI_TC_DIM, t));
        free(s);
        free(t);
    }
    for (size_t i = 0; i < rows.n; i++) strvec_push_own(out, xasprintf("%s%s", prefix, rows.v[i]));
    strvec_free(&rows);
    free(value);
    free(prefix);
}

/* one run of a field's rows: an option, a control, its label or error; `stop` when focus rests on it */
typedef struct {
    strvec_t lines;
    bool stop;
} run_t;
typedef VEC(run_t) runs_t;

static void runs_free(runs_t *r) {
    for (size_t i = 0; i < r->n; i++) strvec_free(&r->v[i].lines);
    free(r->v);
    r->v = NULL;
    r->n = 0;
}

static void push_run(runs_t *r, strvec_t lines, bool stop) {
    run_t x = {lines, stop};
    vec_push(r, x);
}

/* #fieldRuns: rows of one open field in runs, each flagged when it is a focus stop; stops are numbered from
 * first_stop */
static void field_runs(fd_t *v, hf_field_t *f, int width, int focused_stop, int first_stop, runs_t *out) {
    const char *err = v->errors[fidx(f)];
    strvec_t err_lines = {0};
    if (err) {
        char *t = trunc_to(err, MAX(1, width - FIELD_INDENT)), *e = fg(TUI_TC_ERROR, t), *ind = pad(FIELD_INDENT);
        strvec_push_own(&err_lines, xasprintf("%s%s", ind, e));
        free(t);
        free(e);
        free(ind);
    }
    if (f->kind == HF_TEXT || is_dropdown(f)) {
        bool focused = focused_stop == first_stop;
        strvec_t lines = {0};
        if (f->kind == HF_TEXT) text_field_lines(v, f, focused, width, &lines);
        else dropdown_field_lines(v, f, focused, width, &lines);
        for (size_t i = 0; i < err_lines.n; i++) strvec_push(&lines, err_lines.v[i]);
        strvec_free(&err_lines);
        bool has_stop = f->kind == HF_TEXT;
        for (size_t k = 0; k < f->opts.n; k++) has_stop |= !f->opts.v[k].disabled;
        push_run(out, lines, has_stop);
        return;
    }
    other_chosen(v, f); /* settle typing into a radio group's Other before drawing its options */
    if (f->label.n) {
        strvec_t lines = {0};
        hang("  ", &f->label, (base_t){TUI_TC_TEXT, false}, width, &lines);
        push_run(out, lines, false);
    }
    int stop = first_stop;
    for (size_t i = 0; i < f->opts.n; i++) {
        const hf_option_t *o = &f->opts.v[i];
        bool focused = !o->disabled && focused_stop == stop;
        if (!o->disabled) stop++;
        bool ch = v->checked[fidx(f)][i];
        char *cur = cursor_prefix(focused);
        char *mk = fg(ch ? TUI_TC_SUCCESS : TUI_TC_DIM, marker_sym(f->multi, ch));
        tui_tc_t color = o->disabled ? TUI_TC_DIM : focused ? TUI_TC_ACCENT : ch ? TUI_TC_TOOL_OUTPUT : TUI_TC_TEXT;
        char *prefix = xasprintf("%s%s ", cur, mk);
        strvec_t lines = {0};
        int tw;
        char *tag = note_tag(v, f, (int)i, &tw);
        hang(prefix, &o->label, (base_t){color, false}, MAX(1, width - tw), &lines);
        if (!lines.n) strvec_push(&lines, "");
        if (tag) {
            char *l0 = xasprintf("%s%s", lines.v[0], tag);
            free(lines.v[0]);
            lines.v[0] = l0;
            free(tag);
        }
        push_run(out, lines, !o->disabled);
        free(prefix);
        free(mk);
        free(cur);
    }
    if (f->other) {
        strvec_t lines = {0};
        strvec_push_own(&lines, other_line(v, f, focused_stop == stop, width));
        push_run(out, lines, true);
    }
    if (err_lines.n) push_run(out, err_lines, false);
    else strvec_free(&err_lines);
}

/* table.ts shrinkEvenly: the widest columns shrink first, level by level, down to their minimums */
static int shrink_evenly(int *w, const int *mins, size_t n, int overflow) {
    while (overflow > 0) {
        int top = -1;
        for (size_t c = 0; c < n; c++)
            if (w[c] > mins[c] && w[c] > top) top = w[c];
        if (top < 0) break;
        int level = INT_MIN;
        size_t nwide = 0;
        for (size_t c = 0; c < n; c++) {
            if (w[c] <= mins[c]) continue;
            if (w[c] < top) level = MAX(level, w[c]);
            else {
                nwide++;
                level = MAX(level, mins[c]);
            }
        }
        int step = MIN(top - level, (int)((overflow + (int)nwide - 1) / (int)nwide));
        for (size_t c = n; c-- > 0 && overflow > 0;) { /* widest.reverse() */
            if (w[c] <= mins[c] || w[c] != top) continue;
            int cut = MIN(step, overflow);
            w[c] -= cut;
            overflow -= cut;
        }
    }
    return overflow;
}

/* table rows over as many lines as their tallest cell: cells word-wrap within their fitted width and keep <br>
 * breaks (renderTableRowLines, omp e701ef8cbe) */
static void table_lines(const hf_block_t *blk, int width, strvec_t *out) {
    size_t count = 0;
    for (size_t r = 0; r < blk->rows.n; r++) count = MAX(count, blk->rows.v[r].n);
    VEC(char **) cells = {0};
    int *w = xcalloc(count + 1, sizeof *w), *mins = xcalloc(count + 1, sizeof *mins);
    for (size_t c = 0; c < count; c++) w[c] = 1;
    for (size_t r = 0; r < blk->rows.n; r++) {
        char **row = xcalloc(count + 1, sizeof *row);
        for (size_t c = 0; c < count; c++) {
            if (c < blk->rows.v[r].n) {
                char *t = hf_spans_text(&blk->rows.v[r].v[c]);
                row[c] = sanitize(t);
                free(t);
            } else row[c] = xstrdup("");
            strvec_t ls = str_split(row[c], "\n");
            for (size_t k = 0; k < ls.n; k++) w[c] = MAX(w[c], tui_visible_width(ls.v[k]));
            strvec_free(&ls);
        }
        vec_push(&cells, row);
    }
    /* fitWidths: every column wraps, minWidth 4 */
    int gap = 2, total = 0;
    for (size_t c = 0; c < count; c++) {
        total += w[c];
        mins[c] = MIN(w[c], 4);
    }
    shrink_evenly(w, mins, count, total + (count ? (int)(count - 1) * gap : 0) - width);
    sb_t rule = SB_INIT;
    sb_repeat(&rule, "\xe2\x94\x80", width);
    for (size_t r = 0; r < cells.n; r++) {
        if (r > 0 && r == blk->header_rows) strvec_push_own(out, fg(TUI_TC_DIM, sb_cstr(&rule)));
        bool head = r < blk->header_rows;
        strvec_t *stacks = xcalloc(count + 1, sizeof *stacks);
        size_t height = 1;
        for (size_t c = 0; c < count; c++) {
            strvec_t hard = str_split(cells.v[r][c], "\n");
            for (size_t k = 0; k < hard.n; k++) {
                strvec_t wrapped = {0};
                tui_wrap_text_with_ansi(hard.v[k], MAX(1, w[c]), &wrapped);
                if (!wrapped.n) strvec_push(&stacks[c], "");
                for (size_t q = 0; q < wrapped.n; q++) strvec_push(&stacks[c], wrapped.v[q]);
                strvec_free(&wrapped);
            }
            if (!hard.n) strvec_push(&stacks[c], "");
            strvec_free(&hard);
            height = MAX(height, stacks[c].n);
        }
        for (size_t l = 0; l < height; l++) {
            sb_t line = SB_INIT;
            for (size_t c = 0; c < count; c++) {
                const char *t = l < stacks[c].n ? stacks[c].v[l] : "";
                char *fit = tui_visible_width(t) > w[c] ? trunc_to(t, w[c]) : xstrdup(t);
                char *padded = xasprintf("%s%*s", fit, MAX(0, w[c] - tui_visible_width(fit)), "");
                char *styled = head ? fg_bold(TUI_TC_TEXT, padded) : fg(TUI_TC_MUTED, padded);
                if (c) sb_append(&line, "  ");
                sb_append(&line, styled);
                free(styled);
                free(padded);
                free(fit);
            }
            strvec_push_own(out, sb_detach(&line));
        }
        for (size_t c = 0; c < count; c++) strvec_free(&stacks[c]);
        free(stacks);
        for (size_t c = 0; c < count; c++) free(cells.v[r][c]);
        free(cells.v[r]);
    }
    sb_free(&rule);
    free(cells.v);
    free(w);
    free(mins);
}

/* #contextRows: rows of a non-field block: its wrapped lines, list items or table rows, or a picture (*picture set
 * when the rows are a drawn picture) */
static void context_rows(fd_t *v, const hf_block_t *blk, int width, strvec_t *out, bool *picture) {
    *picture = false;
    switch (blk->kind) {
    case HF_B_TEXT: {
        base_t base = blk->heading ? (base_t){TUI_TC_TEXT, true} : (base_t){TUI_TC_MUTED, false};
        hang("", &blk->spans, base, width, out);
        break;
    }
    case HF_B_LIST:
        for (size_t i = 0; i < blk->items.n; i++) {
            char *b = blk->ordered ? xasprintf("%zu. ", i + 1) : xstrdup("\xe2\x80\xa2 ");
            char *bullet = fg(TUI_TC_MD_LIST_BULLET, b);
            hang(bullet, &blk->items.v[i], (base_t){TUI_TC_MUTED, false}, width, out);
            free(bullet);
            free(b);
        }
        break;
    case HF_B_PRE: {
        char *s = sanitize(blk->text);
        strvec_t ls = str_split(s, "\n");
        for (size_t i = 0; i < ls.n; i++) {
            char *t = trunc_to(ls.v[i], MAX(1, width - 2)), *c = fg(TUI_TC_MD_CODE_BLOCK, t);
            strvec_push_own(out, xasprintf("  %s", c));
            free(t);
            free(c);
        }
        strvec_free(&ls);
        free(s);
        break;
    }
    case HF_B_IMAGE:
    case HF_B_SVG: {
        media_t *m = media_of(v, blk);
        size_t before = out->n;
        if (m) media_rows(v, blk, m, width, out);
        if (out->n > before) *picture = true;
        else if (m) {
            char *n = media_note(blk, m), *t = trunc_to(n, width);
            strvec_push_own(out, fg(TUI_TC_DIM, t));
            free(n);
            free(t);
        }
        break;
    }
    case HF_B_TABLE:
        table_lines(blk, width, out);
        break;
    case HF_B_FIELD:
        break;
    }
}

/* ---------- folded questions (chrome/ask-fold.ts, native/ask-card.ts foldPills) ---------- */

enum { SUM_ANSWER, SUM_WELL, SUM_PILLS };

typedef struct {
    char *label;
    bool chosen;
} pill_t;

/* FoldSummary: the typed answer, an empty field's placeholder, or the options as pills */
typedef struct {
    int kind;
    char *text; /* answer / well placeholder */
    char *note; /* the first chosen option's note, one line (NULL: none) */
    VEC(pill_t) pills;
} summary_t;

static void summary_free(summary_t *s) {
    free(s->text);
    free(s->note);
    for (size_t i = 0; i < s->pills.n; i++) free(s->pills.v[i].label);
    free(s->pills.v);
}

/* text.replace(/\s+/g, " ") of a sanitized string, trimmed when asked */
static char *one_space(const char *s, bool trim) {
    char *a = sanitize_line(s);
    if (!trim) return a;
    char *t = str_trim(a);
    free(a);
    return t;
}

/* #foldSummary */
static void fold_summary(fd_t *v, const hf_field_t *f, summary_t *out) {
    memset(out, 0, sizeof *out);
    if (f->kind == HF_TEXT) {
        char *value = text_value(v, f), *t = str_trim(value);
        if (!*t) {
            char *ph = sanitize(f->placeholder);
            out->kind = SUM_WELL;
            out->text = *ph ? ph : xstrdup(WELL_PLACEHOLDER);
            if (!*ph) free(ph);
        } else if (str_eq(f->type, "password")) {
            sb_t b = SB_INIT;
            for (size_t i = 0; i < hf_utf16_len(value); i++) sb_append(&b, "\xe2\x80\xa2");
            out->kind = SUM_ANSWER;
            out->text = sb_detach(&b);
        } else {
            out->kind = SUM_ANSWER;
            out->text = one_space(value, true);
        }
        free(t);
        free(value);
        return;
    }
    /* before reading the checks: typing a radio group's Other drops its picked option */
    bool other = other_chosen(v, f);
    const bool *checked = v->checked[fidx(f)];
    out->kind = SUM_PILLS;
    for (size_t k = 0; k < f->opts.n; k++) {
        if (f->opts.v[k].disabled && !checked[k]) continue;
        char *t = option_text(&f->opts.v[k]);
        pill_t p = {*t ? t : xstrdup(f->opts.v[k].value), checked[k]};
        if (!*t) free(t);
        vec_push(&out->pills, p);
    }
    if (other) {
        char *typed = one_space(ui_input_value(&v->others[fidx(f)]), true);
        pill_t p = {xasprintf("%s: %s", HF_OTHER_LABEL, typed), true};
        vec_push(&out->pills, p);
        free(typed);
    }
    char **notes = v->notes[fidx(f)];
    for (size_t k = 0; notes && k < f->opts.n && !out->note; k++) {
        if (!checked[k] || !notes[k]) continue;
        char *t = str_trim(notes[k]);
        if (*t) out->note = one_space(t, false);
        free(t);
    }
}

/* noteText: the note after a summary, dim (owned; "" without one) */
static char *note_text(const char *note) {
    if (!note) return xstrdup("");
    char *t = trunc_to(note, 60), *s = xasprintf("  \xe2\x9c\x8e %s", t), *r = fg(TUI_TC_DIM, s);
    free(t);
    free(s);
    return r;
}

/* summaryLine (owned; "" when it shows nothing) */
static char *summary_line(const summary_t *s) {
    if (s->kind == SUM_ANSWER) {
        char *t = one_space(s->text, false), *m = fg(TUI_TC_MUTED, t), *n = note_text(s->note);
        char *r = xasprintf("%s%s", m, n);
        free(t), free(m), free(n);
        return r;
    }
    if (s->kind == SUM_WELL) return fg(TUI_TC_DIM, s->text);
    /* foldPills: options in order, up to PILL_LIMIT, the chosen ones always among them, each cut to a label width */
    size_t chosen = 0;
    for (size_t i = 0; i < s->pills.n; i++) chosen += s->pills.v[i].chosen;
    size_t room = chosen >= PILL_LIMIT ? 0 : PILL_LIMIT - chosen, shown = 0;
    bool any_chosen = false;
    sb_t b = SB_INIT;
    for (size_t i = 0; i < s->pills.n && shown < PILL_LIMIT; i++) {
        const pill_t *p = &s->pills.v[i];
        if (!p->chosen) {
            if (!room) continue;
            room--;
        }
        char *cut = trunc_to(p->label, PILL_LABEL_WIDTH);
        size_t cl = strlen(cut);
        if (cl >= 3 && !strcmp(cut + cl - 3, "\xe2\x80\xa6")) { /* a cut after a word keeps no space before its ellipsis */
            size_t e = cl - 3;
            while (e > 0 && isspace((unsigned char)cut[e - 1])) e--;
            if (e < cl - 3) memmove(cut + e, cut + cl - 3, 4);
        }
        char *chip;
        if (p->chosen) {
            char *t = xasprintf(" \xe2\x9c\x93 %s ", cut), *f = fg(TUI_TC_SUCCESS, t);
            chip = tui_bgs(TH(), TUI_TC_TOOL_SUCCESS_BG, f);
            free(t), free(f);
            any_chosen = true;
        } else {
            char *t = xasprintf(" %s ", cut), *f = fg(TUI_TC_MUTED, t);
            chip = tui_bgs(TH(), TUI_TC_SELECTED_BG, f);
            free(t), free(f);
        }
        if (shown) sb_putc(&b, ' ');
        sb_append(&b, chip);
        free(chip);
        free(cut);
        shown++;
    }
    size_t hidden = s->pills.n - shown;
    if (hidden > 0) {
        char *t = xasprintf(" +%zu ", hidden), *f = fg(TUI_TC_DIM, t), *chip = tui_bgs(TH(), TUI_TC_SELECTED_BG, f);
        if (shown) sb_putc(&b, ' ');
        sb_append(&b, chip);
        free(t), free(f), free(chip);
    }
    if (any_chosen) {
        char *n = note_text(s->note);
        sb_append(&b, n);
        free(n);
    }
    return sb_detach(&b);
}

/* renderFoldedQuestion: the mark and wrapped question, then its summary cut to one row, then its error */
static void folded_rows(fd_t *v, const hf_field_t *f, int width, strvec_t *out) {
    const char *err = v->errors[fidx(f)];
    char *mark = err ? fg(TUI_TC_ERROR, "!") : answered(v, f) ? fg(TUI_TC_SUCCESS, "\xe2\x9c\x93") : fg(TUI_TC_DIM, "\xe2\x97\x8b");
    int inner = MAX(1, width - (int)strlen(FOLD_INDENT));
    char *q = sanitize(f->question && *f->question ? f->question : f->name), *qs = fg(TUI_TC_TEXT, q);
    strvec_t wrapped = {0};
    tui_wrap_text_with_ansi(qs, inner, &wrapped);
    for (size_t i = 0; i < wrapped.n; i++) strvec_push_own(out, i ? xasprintf(FOLD_INDENT "%s", wrapped.v[i]) : xasprintf("%s %s", mark, wrapped.v[i]));
    strvec_free(&wrapped);
    summary_t sum;
    fold_summary(v, f, &sum);
    char *line = summary_line(&sum);
    if (*line) {
        char *t = tui_truncate_to_width(line, inner, NULL);
        strvec_push_own(out, xasprintf(FOLD_INDENT "%s", t));
        free(t);
    }
    if (err) {
        char *e = fg(TUI_TC_ERROR, err);
        strvec_t ew = {0};
        tui_wrap_text_with_ansi(e, inner, &ew);
        for (size_t i = 0; i < ew.n; i++) strvec_push_own(out, xasprintf(FOLD_INDENT "%s", ew.v[i]));
        strvec_free(&ew);
        free(e);
    }
    free(line);
    summary_free(&sum);
    free(qs);
    free(q);
    free(mark);
}

/* ---------- question groups (buildGroups) ---------- */

/* Question groups: one per `<fieldset>` holding fields, and one per field of loose content between fieldsets, each
 * with the context before it. Sections of context alone before the first group are the intro, shown above every
 * group; later ones join the next group, or the last one when nothing follows. */
static void build_groups(fd_t *v) {
    parts_t lead = {0};
    const hf_form_t *form = v->form;
    for (size_t si = 0; si < form->sections.n; si++) {
        const hf_section_t *sec = &form->sections.v[si];
        size_t end = sec->blocks.n;
        bool any = false;
        for (size_t bi = 0; bi < end && !any; bi++) any = sec->blocks.v[bi].kind == HF_B_FIELD;
        if (!any) {
            part_t p = {si, 0, end};
            vec_push(v->groups.n ? &lead : &v->intro, p);
            continue;
        }
        /* where each question of this section ends: after each loose field, the last one at the section's end. A
         * group's question also keeps the text between its options, unless its options straddle the next field: then
         * that field keeps the context before it. A field the keyboard cannot reach is no question of its own */
        VEC(size_t) cuts = {0};
        if (!sec->fieldset) {
            for (size_t bi = 0; bi < end; bi++) {
                const hf_block_t *blk = &sec->blocks.v[bi];
                if (blk->kind != HF_B_FIELD || !reachable(blk->field)) continue;
                size_t through = MAX(bi + 1, blk->options_end);
                size_t next = end;
                for (size_t k = bi + 1; k < end; k++)
                    if (sec->blocks.v[k].kind == HF_B_FIELD) {
                        next = k;
                        break;
                    }
                size_t cut = through <= next ? through : bi + 1;
                vec_push(&cuts, cut);
            }
        }
        if (!cuts.n) vec_push(&cuts, end);
        else cuts.v[cuts.n - 1] = end;
        size_t start = 0;
        for (size_t c = 0; c < cuts.n; c++) {
            group_t g = {0};
            for (size_t i = 0; i < lead.n; i++) vec_push(&g.parts, lead.v[i]);
            lead.n = 0;
            part_t p = {si, start, cuts.v[c]};
            vec_push(&g.parts, p);
            for (size_t bi = start; bi < cuts.v[c]; bi++)
                if (sec->blocks.v[bi].kind == HF_B_FIELD) vec_push(&g.fields, sec->blocks.v[bi].field);
            vec_push(&v->groups, g);
            start = cuts.v[c];
        }
        free(cuts.v);
    }
    if (v->groups.n)
        for (size_t i = 0; i < lead.n; i++) vec_push(&v->groups.v[v->groups.n - 1].parts, lead.v[i]);
    free(lead.v);
    v->field_group = xcalloc(form->fields.n + 1, sizeof *v->field_group);
    for (size_t g = 0; g < v->groups.n; g++)
        for (size_t i = 0; i < v->groups.v[g].fields.n; i++) v->field_group[fidx(v->groups.v[g].fields.v[i])] = (int)g;
}

/* ---------- layout ---------- */

/* a picture's rows in the column, drawn again only while the body shows it whole */
typedef struct {
    int start, end;
    const hf_block_t *blk;
} placed_t;

/* the stacked column at one width, and where focus, the open question and the overlays sit in it */
typedef struct {
    int width;
    strvec_t lines;
    int focus_start, focus_end; /* rows [start, end) of the focused stop */
    int head;                   /* first row of the open question: 0 for the first, Submit's row on Submit */
    bool has_dropdown;
    int dd_top, dd_row;         /* the open dropdown's field's first row and its value row */
    int note_row;               /* last row of the option whose note is open (-1: none) */
    VEC(placed_t) pictures;
} layout_t;

static void layout_free(layout_t *l) {
    strvec_free(&l->lines);
    free(l->pictures.v);
}

/* pushes `rows` a blank row under the rows before them (none when `tight`); returns their first row. Takes the rows */
static int emit(layout_t *L, strvec_t *rows, bool tight) {
    if (L->lines.n && !tight) strvec_push(&L->lines, "");
    int start = (int)L->lines.n;
    for (size_t i = 0; i < rows->n; i++) {
        strvec_push_own(&L->lines, rows->v[i]);
        rows->v[i] = NULL;
    }
    free(rows->v);
    memset(rows, 0, sizeof *rows);
    return start;
}

/* #isFoldCaption: `blocks[at]` is the caption of a folded field right after it, which shows it as its question */
static bool is_fold_caption(const hf_section_t *sec, size_t at, const hf_field_t *focused) {
    if (at + 1 >= sec->blocks.n) return false;
    const hf_block_t *blk = &sec->blocks.v[at], *next = &sec->blocks.v[at + 1];
    if (blk->kind != HF_B_TEXT || next->kind != HF_B_FIELD || next->field == focused) return false;
    char *t = hf_spans_text(&blk->spans), *one = hf_one_line(t);
    bool r = str_eq(next->field->question ? next->field->question : "", one);
    free(t);
    free(one);
    return r;
}

static int first_stop_of(const fd_t *v, const hf_field_t *f) {
    for (size_t i = 0; i < v->stops.n; i++)
        if (v->stops.v[i].kind != STOP_SUBMIT && v->stops.v[i].field == f) return (int)i;
    return -1;
}

static void emit_parts(fd_t *v, layout_t *L, const parts_t *parts, int focus, int highlight, int width) {
    const hf_field_t *focus_field = field_of_stop(v, focus);
    for (size_t pi = 0; pi < parts->n; pi++) {
        const part_t *p = &parts->v[pi];
        const hf_section_t *sec = &v->form->sections.v[p->section];
        /* a title keeps the block after it under it; a short text block right before a field, that field */
        enum { KEEP_NONE, KEEP_TITLE, KEEP_CAPTION } keep = KEEP_NONE;
        if (p->start == 0 && sec->title.n) {
            strvec_t rows = {0};
            hang("", &sec->title, (base_t){TUI_TC_TEXT, true}, width, &rows);
            emit(L, &rows, false);
            keep = KEEP_TITLE;
        }
        for (size_t at = p->start; at < p->end; at++) {
            const hf_block_t *blk = &sec->blocks.v[at];
            if (blk->kind != HF_B_FIELD) {
                if (is_fold_caption(sec, at, focus_field)) continue;
                strvec_t rows = {0};
                bool picture;
                context_rows(v, blk, width, &rows, &picture);
                int n = (int)rows.n;
                int top = emit(L, &rows, keep == KEEP_TITLE);
                if (picture) {
                    placed_t pl = {top, top + n, blk};
                    vec_push(&L->pictures, pl);
                }
                keep = blk->kind == HF_B_TEXT && n <= CAPTION_MAX_ROWS ? KEEP_CAPTION : KEEP_NONE;
                continue;
            }
            hf_field_t *f = blk->field;
            bool tight = keep != KEEP_NONE;
            keep = KEEP_NONE;
            if (f != focus_field) {
                strvec_t rows = {0};
                folded_rows(v, f, width, &rows);
                emit(L, &rows, tight);
                continue;
            }
            int first = first_stop_of(v, f);
            runs_t runs = {0};
            field_runs(v, f, width, highlight, first, &runs);
            strvec_t all = {0};
            for (size_t r = 0; r < runs.n; r++)
                for (size_t k = 0; k < runs.v[r].lines.n; k++) strvec_push(&all, runs.v[r].lines.v[k]);
            int top = emit(L, &all, tight);
            int row = top, stop = first;
            for (size_t r = 0; r < runs.n; r++) {
                if (runs.v[r].stop && stop++ == focus) {
                    L->focus_start = row;
                    L->focus_end = row + (int)runs.v[r].lines.n;
                }
                row += (int)runs.v[r].lines.n;
            }
            if (f->kind == HF_CHOICE && v->open == f) {
                int err_rows = v->errors[fidx(f)] ? 1 : 0;
                L->has_dropdown = true;
                L->dd_top = top;
                L->dd_row = top + (int)runs.v[0].lines.n - 1 - err_rows; /* the value row, above the error */
            }
            if (v->note_ed && v->note_field == f) L->note_row = L->focus_end - 1;
            runs_free(&runs);
        }
    }
}

/* #layout: the stacked column at `width`, stop `focus` holding focus and stop `highlight` drawn focused (-1: none):
 * the intro, then each question group (the one holding focus open: its context, its focused field's controls and its
 * other fields folded; the rest folded field by field), then the Submit row */
static void build_layout(fd_t *v, int width, int focus, int highlight, layout_t *L) {
    memset(L, 0, sizeof *L);
    L->width = width;
    L->note_row = -1;
    const hf_field_t *focus_field = field_of_stop(v, focus);
    int open = group_of_stop(v, focus);
    emit_parts(v, L, &v->intro, focus, highlight, width);
    for (size_t g = 0; g < v->groups.n; g++) {
        const group_t *grp = &v->groups.v[g];
        if ((int)g != open) {
            for (size_t i = 0; i < grp->fields.n; i++) {
                strvec_t rows = {0};
                folded_rows(v, grp->fields.v[i], width, &rows);
                emit(L, &rows, false);
            }
            continue;
        }
        int before = (int)L->lines.n;
        emit_parts(v, L, &grp->parts, focus, highlight, width);
        L->head = g == 0 || before == 0 ? 0 : before + 1; /* the first group's head is the intro's too */
    }
    char *sub;
    if (highlight == (int)v->stops.n - 1) {
        char *t = xasprintf("%s %s", tui_sym("nav.cursor"), SUBMIT_LABEL), *b = tui_bold(t);
        sub = fg(TUI_TC_ACCENT, b);
        free(t);
        free(b);
    } else sub = fg(TUI_TC_MUTED, "  " SUBMIT_LABEL);
    char *pending;
    if (v->nerrors) {
        char *t = xasprintf("  %zu to fix", v->nerrors);
        pending = fg(TUI_TC_ERROR, t);
        free(t);
    } else pending = xstrdup("");
    strvec_t rows = {0};
    strvec_push_own(&rows, xasprintf("%s%s", sub, pending));
    int submit_row = emit(L, &rows, false);
    if (!focus_field) {
        L->focus_start = submit_row;
        L->focus_end = submit_row + 1;
        L->head = submit_row;
    }
    free(sub);
    free(pending);
}

/* #bodyRows: the column's natural height (the tallest it gets with any one question open, or with Submit focused),
 * capped by the terminal. Fixed per width, terminal height, pictures and errors, so moving focus or typing never
 * resizes the panel; only a failed submit (new error rows) may */
static int body_rows(fd_t *v, int width) {
    int trows = term_rows(v);
    sb_t key = SB_INIT;
    sb_appendf(&key, "%d:%d:%d:", width, trows, v->media_version);
    for (size_t i = 0; i < v->form->fields.n; i++)
        if (v->errors[i]) sb_appendf(&key, "%zu=%s%c", i, v->errors[i], 1);
    if (v->height_key && !strcmp(v->height_key, sb_cstr(&key))) {
        sb_free(&key);
        return v->height_rows;
    }
    /* the panel leaves the composer and status their reserved rows, but keeps a usable body on a tiny terminal */
    int panel = MIN((int)floor(trows * DIALOG_HEIGHT_RATIO), trows - ASK_RESERVED_ROWS);
    int cap = MAX(MIN_BODY_ROWS, panel - CHROME_ROWS);
    int rows = 1;
    /* a field opens alike whichever of its stops holds focus, so its first stop measures it */
    for (int s = 0; s < (int)v->stops.n && rows < cap; s++) {
        if (s > 0 && field_of_stop(v, s - 1) == field_of_stop(v, s)) continue;
        layout_t L;
        build_layout(v, width, s, -1, &L);
        rows = MAX(rows, (int)L.lines.n);
        layout_free(&L);
    }
    rows = MIN(cap, rows);
    free(v->height_key);
    v->height_key = sb_detach(&key);
    v->height_rows = rows;
    return rows;
}

/* the dropdown box at most `max_rows` tall, scrolled to keep the highlight visible; empty when it cannot fit */
static void dropdown_box(fd_t *v, int width, int max_rows, strvec_t *out) {
    const hf_field_t *f = v->open;
    int n = (int)f->opts.n, visible = MIN(n, max_rows - 2);
    if (visible < 1) return;
    strvec_t labels = {0};
    int widest = 0;
    for (int i = 0; i < n; i++) {
        strvec_push_own(&labels, option_text(&f->opts.v[i]));
        int tw;
        free(note_tag(v, f, i, &tw));
        widest = MAX(widest, tui_visible_width(labels.v[i]) + tw);
    }
    int inner = MAX(4, MIN(width - FIELD_INDENT - 4, widest + 4));
    int first = MAX(0, MIN(n - visible, v->highlight - visible / 2));
    int cur = current(v, f);
    const char *h = tui_sym("boxRound.horizontal");
    for (int edge = 0; edge < 2; edge++) {
        if (edge == 1) {
            for (int i = first; i < first + visible; i++) {
                bool hl = i == v->highlight;
                char *c = hl ? xasprintf("%s ", tui_sym("nav.cursor")) : xstrdup("  ");
                char *mark = i == cur ? fg(TUI_TC_SUCCESS, "\xe2\x9c\x93") : xstrdup(" ");
                tui_tc_t color = f->opts.v[i].disabled ? TUI_TC_DIM : hl ? TUI_TC_ACCENT : TUI_TC_TEXT;
                int tw;
                char *tag = note_tag(v, f, i, &tw);
                char *lab = trunc_to(labels.v[i], MAX(1, inner - 4 - tw));
                char *cl0 = xasprintf("%s%s", c, lab), *cs0 = fg(color, cl0), *cs = xasprintf("%s%s", cs0, tag ? tag : "");
                free(cs0), free(tag);
                char *padding = pad(inner - 3 - tui_visible_width(lab) - tw);
                char *lv = xasprintf("%s ", tui_sym("boxRound.vertical")), *rv = xasprintf(" %s", tui_sym("boxRound.vertical"));
                char *ls = fg(TUI_TC_ACCENT, lv), *rs = fg(TUI_TC_ACCENT, rv);
                strvec_push_own(out, xasprintf("%s%s%s%s%s", ls, cs, padding, mark, rs));
                free(c), free(mark), free(lab), free(cl0), free(cs), free(padding), free(lv), free(rv), free(ls), free(rs);
            }
        }
        bool top = edge == 0;
        bool more = top ? first > 0 : first + visible < n;
        sb_t r = SB_INIT;
        sb_append(&r, tui_sym(top ? "boxRound.topLeft" : "boxRound.bottomLeft"));
        if (more) {
            sb_append(&r, h);
            sb_append(&r, top ? "\xe2\x86\x91" : "\xe2\x86\x93");
            sb_repeat(&r, h, inner);
        } else sb_repeat(&r, h, inner + 2);
        sb_append(&r, tui_sym(top ? "boxRound.topRight" : "boxRound.bottomRight"));
        strvec_push_own(out, fg(TUI_TC_ACCENT, sb_cstr(&r)));
        sb_free(&r);
    }
    strvec_free(&labels);
}

/* cuts(at): pictures straddling an edge of a body `rows` tall starting at `at` */
static int picture_cuts(const layout_t *L, int at, int rows) {
    int n = 0;
    for (size_t i = 0; i < L->pictures.n; i++) {
        const placed_t *p = &L->pictures.v[i];
        if ((p->start < at && at < p->end) || (p->start < at + rows && at + rows < p->end)) n++;
    }
    return n;
}

/* clearPictureEdges: `offset` moved so fewer pictures straddle an edge (a picture draws only whole; a cut one draws as
 * a frame): among the offsets that put a picture's top or bottom on an edge, the one cutting the fewest, nearest
 * `offset`, that keeps rows [keep_start, keep_end) in view (keep_start < 0: none) and stays at or above `limit` */
static int clear_picture_edges(const layout_t *L, int offset, int rows, int max, int keep_start, int keep_end, int limit) {
    int own = picture_cuts(L, offset, rows), best = offset, fewest = own;
    for (size_t i = 0; i < L->pictures.n; i++) {
        const placed_t *p = &L->pictures.v[i];
        int cands[4] = {p->start, p->end, p->end - rows, p->start - rows};
        for (int c = 0; c < 4; c++) {
            int at = cands[c];
            if (own == 0 || at < 0 || at > MIN(max, limit)) continue;
            if (keep_start >= 0 && (keep_start < at || (keep_end > at + rows && keep_start != at))) continue;
            int count = picture_cuts(L, at, rows);
            bool nearer = abs(at - offset) < abs(best - offset);
            if (count < fewest || (count == fewest && fewest < own && nearer)) {
                best = at;
                fewest = count;
            }
        }
    }
    return best;
}

/* cutPictureRows: a dim frame naming a picture the body cuts off over its `visible` rows, open on each cut side */
static void cut_picture_rows(const char *alt, int visible, int width, bool cut_top, bool cut_bottom, strvec_t *out) {
    char *name = xasprintf("\xe2\x96\xa3 %s", alt);
    if (visible <= 1) {
        char *t = trunc_to(name, width);
        strvec_push_own(out, fg(TUI_TC_DIM, t));
        free(t);
        free(name);
        return;
    }
    int inner = MAX(0, width - 3); /* a column short of the body so the frame doesn't run into the scrollbar */
    char *lt = cut_top ? xasprintf(" %s", name) : xasprintf("\xe2\x94\x80 %s ", name);
    char *label = trunc_to(lt, inner);
    int lw = tui_visible_width(label);
    sb_t row = SB_INIT;
    if (cut_top) {
        sb_appendf(&row, "\xe2\x94\x82%s", label);
        tui_sb_spaces(&row, MAX(0, inner - lw));
        sb_append(&row, "\xe2\x94\x82");
    } else {
        sb_appendf(&row, "\xe2\x95\xad%s", label);
        sb_repeat(&row, "\xe2\x94\x80", MAX(0, inner - lw));
        sb_append(&row, "\xe2\x95\xae");
    }
    strvec_push_own(out, fg(TUI_TC_DIM, sb_cstr(&row)));
    sb_free(&row);
    for (int r = 1; r < visible - 1; r++) {
        sb_t m = SB_INIT;
        sb_append(&m, "\xe2\x94\x82");
        tui_sb_spaces(&m, inner);
        sb_append(&m, "\xe2\x94\x82");
        strvec_push_own(out, fg(TUI_TC_DIM, sb_cstr(&m)));
        sb_free(&m);
    }
    sb_t last = SB_INIT;
    if (cut_bottom) {
        sb_append(&last, "\xe2\x94\x82");
        tui_sb_spaces(&last, inner);
        sb_append(&last, "\xe2\x94\x82");
    } else {
        sb_append(&last, "\xe2\x95\xb0");
        sb_repeat(&last, "\xe2\x94\x80", inner);
        sb_append(&last, "\xe2\x95\xaf");
    }
    strvec_push_own(out, fg(TUI_TC_DIM, sb_cstr(&last)));
    sb_free(&last);
    free(label);
    free(lt);
    free(name);
}

/* mediaName: what a picture stands for in text: its alt, else its source or `drawing` */
static char *media_name(const hf_block_t *blk) {
    if (blk->alt && *blk->alt) return sanitize(blk->alt);
    if (blk->kind == HF_B_IMAGE) {
        char *s = xstrndup(blk->src, 80), *r = sanitize(s);
        free(s);
        return r;
    }
    return xstrdup("drawing");
}

/* ScrollView (scrollbar "auto") over an already-windowed slice: `shown` is the body's `height` rows from `offset` of
 * `total` logical rows */
static void scroll_window(const strvec_t *shown, int height, int offset, int total, int width, strvec_t *out) {
    bool bar = total > height && width > 1;
    int cw = MAX(0, width - (bar ? 1 : 0));
    int ts = 0, te = height;
    if (bar) {
        int size = MAX(1, MIN((height * height) / total, height));
        int maxoff = MAX(0, total - height);
        int off = MAX(0, MIN(offset, maxoff));
        ts = maxoff ? (int)lround((double)off / maxoff * (height - size)) : 0;
        te = ts + size;
    }
    for (int row = 0; row < height; row++) {
        const char *src = row < (int)shown->n ? shown->v[row] : "";
        char *rt = tui_replace_tabs(src), *tr = trunc_to(rt, cw);
        free(rt);
        if (!bar) {
            strvec_push_own(out, tr);
            continue;
        }
        char *padded = tui_pad_to_width(tr, cw);
        bool thumb = row >= ts && row < te;
        char *g = fg(thumb ? TUI_TC_ACCENT : TUI_TC_MUTED, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
        strvec_push_own(out, xasprintf("%s%s", padded, g));
        free(g);
        free(padded);
        free(tr);
    }
}

/* #renderBody: `rows` rows of the column from the scroll offset, the shown pictures drawn, the open note editor or
 * dropdown over them, and a scrollbar while the column overflows. The offset moves as little as keeps the focused
 * stop's rows in view (its top when it is taller than the body), then up to the open question's head when that fits
 * along, then to the nearest offset that cuts fewer pictures. While ↑ peeks at a cut-off head it keeps the peeked
 * offset, or one above where the body was that cuts fewer. An open note keeps rows under its option for the editor */
static void render_body(fd_t *v, const layout_t *L, int rows, int width, strvec_t *out) {
    int nlines = (int)L->lines.n;
    int max = MAX(0, nlines - rows);
    int offset = MIN(v->scroll, max);
    if (!v->peek) {
        if (L->focus_start < offset || L->focus_end - L->focus_start >= rows) offset = L->focus_start;
        else if (L->focus_end > offset + rows) offset = L->focus_end - rows;
        if (L->head < offset && L->focus_end - L->head <= rows) offset = L->head;
        offset = MIN(offset, max);
    }
    /* like the dropdown, the note editor covers the rows under its option rather than pushing them */
    int note_row = v->note_ed ? L->note_row : -1;
    if (note_row >= 0) offset = MAX(offset, note_row - MAX(0, rows - 3));
    else {
        /* ↑ peeking toward the head moves the body up at least a row, never back down */
        int limit = v->peek ? MAX(offset, (v->view.valid ? v->view.offset : offset) - 1) : max;
        offset = clear_picture_edges(L, offset, rows, max, v->peek ? -1 : L->focus_start, L->focus_end, limit);
    }
    v->scroll = offset;
    v->view.valid = true;
    v->view.focus = v->focus;
    v->view.offset = offset;
    v->view.rows = rows;
    v->view.head = L->head;
    strvec_t shown = {0};
    for (int i = offset; i < offset + rows; i++) strvec_push(&shown, i < nlines ? L->lines.v[i] : "");
    for (size_t pi = 0; pi < L->pictures.n; pi++) {
        const placed_t *p = &L->pictures.v[pi];
        int top = p->start - offset, bottom = p->end - offset;
        if (bottom <= 0 || top >= rows) continue;
        /* drawn only whole, so only shown pictures take image slots and none draws past the body's edge; a cut one is
         * a frame naming it */
        int from = MAX(0, top), to = MIN(rows, bottom);
        strvec_t drawn = {0};
        if (top >= 0 && bottom <= rows) {
            media_t *m = media_of(v, p->blk);
            if (m) media_rows(v, p->blk, m, L->width, &drawn);
        } else {
            char *alt = media_name(p->blk);
            cut_picture_rows(alt, to - from, L->width, top < 0, bottom > rows, &drawn);
            free(alt);
        }
        for (int r = from; r < to; r++) {
            free(shown.v[r]);
            shown.v[r] = xstrdup(r - from < (int)drawn.n ? drawn.v[r - from] : "");
        }
        strvec_free(&drawn);
    }
    if (note_row >= 0) {
        int inner = MAX(1, L->width - FIELD_INDENT);
        int top = note_row - offset + 1;
        v->note_ed->max_height = MAX(1, MIN(4, rows - top - 1));
        char *ot = option_text(&v->note_field->opts.v[v->note_option]), *hint = note_hint(v);
        char *ht = xasprintf("Note for %s \xc2\xb7 %s", ot, hint), *htr = trunc_to(ht, inner);
        strvec_t editing = {0};
        strvec_push_own(&editing, fg(TUI_TC_MUTED, htr));
        free(ot), free(hint), free(ht), free(htr);
        v->note_ed->base.focused = v->base.focused && !v->hidden;
        v->note_ed->base.tui = v->base.tui;
        tui_component_render(&v->note_ed->base, inner, &editing);
        char *ind = pad(FIELD_INDENT);
        /* shown.splice(top, editing.length, ...editing) */
        strvec_t spliced = {0};
        for (int i = 0; i < top && i < (int)shown.n; i++) strvec_push(&spliced, shown.v[i]);
        for (size_t i = 0; i < editing.n; i++) strvec_push_own(&spliced, xasprintf("%s%s", ind, editing.v[i]));
        for (int i = top + (int)editing.n; i < (int)shown.n; i++) strvec_push(&spliced, shown.v[i]);
        free(ind);
        strvec_free(&editing);
        strvec_free(&shown);
        shown = spliced;
    } else if (L->has_dropdown && v->open) {
        int row = L->dd_row - offset, top = MAX(0, L->dd_top - offset);
        if (row >= 0 && row < rows) {
            /* #overlayDropdown: below its value row, or above its field when there is more room there */
            int below = rows - row - 1, above = top;
            int wanted = (int)v->open->opts.n + 2;
            bool place_below = below >= MIN(wanted, 5) || below >= above;
            strvec_t box = {0};
            dropdown_box(v, L->width, place_below ? below : above, &box);
            if (box.n) {
                int start = place_below ? row + 1 : top - (int)box.n;
                while ((int)shown.n < start + (int)box.n) strvec_push(&shown, "");
                char *ind = pad(FIELD_INDENT - 2);
                for (size_t i = 0; i < box.n; i++) {
                    free(shown.v[start + i]);
                    shown.v[start + i] = xasprintf("%s%s", ind, box.v[i]);
                }
                free(ind);
            }
            strvec_free(&box);
        }
    }
    scroll_window(&shown, rows, offset, MAX(nlines, offset + rows), width, out);
    strvec_free(&shown);
}

/* #keyHintText: the focused stop's keys, moving, Esc to the composer, hiding and jumping questions; the open note's or
 * dropdown's keys while one is open */
static char *key_hint_text(fd_t *v) {
    if (v->note_ed) return note_hint(v);
    char *hide = ui_editor_key("app.ask.hide"), *esc = ui_editor_key("tui.select.cancel");
    char *composer = *hide ? xasprintf("%s composer \xc2\xb7 %s hide", esc, hide) : xasprintf("%s composer", esc);
    char *enter = ui_key_hint("enter"), *updown = editor_keys2("tui.select.up", "tui.select.down");
    const stop_t *s = &v->stops.v[v->focus];
    strvec_t nk = note_keys(s->kind == STOP_SELECT);
    char *nkh = nk.n ? ui_key_hint(nk.v[0]) : NULL;
    char *note = nkh ? xasprintf(" \xc2\xb7 %s note", nkh) : xstrdup("");
    free(nkh);
    strvec_free(&nk);
    char *r;
    if (v->open) {
        r = xasprintf("%s highlight \xc2\xb7 %s choose%s \xc2\xb7 %s close", updown, enter, note, esc);
        free(hide), free(esc), free(composer), free(enter), free(updown), free(note);
        return r;
    }
    char *space = ui_key_hint("space"), *left = ui_key_hint("left"), *right = ui_key_hint("right"), *tab = ui_key_hint("tab");
    const char *next = v->sole ? "submit" : "next";
    char *action;
    if (s->kind == STOP_SUBMIT) action = xasprintf("%s submit", enter);
    else if (s->kind == STOP_TEXT) action = xasprintf("%s %s", enter, next);
    else if (s->kind == STOP_OTHER) action = xasprintf("type your own \xc2\xb7 %s %s", enter, next);
    else if (s->kind == STOP_SELECT)
        action = xasprintf("%s open \xc2\xb7 %s/%s change \xc2\xb7 %s %s", space, left, right, enter, next);
    else if (s->field->multi) action = xasprintf("%s toggle \xc2\xb7 %s %s", space, enter, next);
    else action = xasprintf("%s %s", enter, v->sole ? "submit" : "choose");
    sb_t b = SB_INIT;
    sb_append(&b, action);
    if (s->kind == STOP_OPTION || s->kind == STOP_SELECT) sb_append(&b, note);
    sb_appendf(&b, " \xc2\xb7 %s move", updown);
    if (s->kind != STOP_SUBMIT) sb_appendf(&b, " \xc2\xb7 %s next field", tab);
    sb_appendf(&b, " \xc2\xb7 %s", composer);
    if (v->groups.n > 1) {
        char *keys = s->kind == STOP_TEXT || s->kind == STOP_SELECT ? editor_keys2("tui.select.pageUp", "tui.select.pageDown")
                                                                     : xasprintf("%s/%s", left, right);
        sb_appendf(&b, " \xc2\xb7 %s question", keys);
        free(keys);
    }
    free(action), free(space), free(left), free(right), free(tab), free(note);
    free(hide), free(esc), free(composer), free(enter), free(updown);
    return sb_detach(&b);
}

/* #hiddenSummary: the question in focus (else the first) for the hidden row, with how many more there are */
static char *hidden_summary(fd_t *v) {
    const stop_t *s = &v->stops.v[v->focus];
    const hf_field_t *f = s->kind != STOP_SUBMIT ? s->field : v->form->fields.n ? v->form->fields.v[0] : NULL;
    char *q = f ? sanitize_line(f->question ? f->question : "") : xstrdup("");
    size_t more = v->form->fields.n ? v->form->fields.n - 1 : 0;
    if (!more) return q;
    char *r = xasprintf("%s (+%zu more)", q, more);
    free(q);
    return r;
}

/* #renderHidden: one row, the title, the question clipped to fit, and how to bring it back */
static void render_hidden(fd_t *v, int width, strvec_t *out) {
    char *hide = ui_editor_key("app.ask.hide"), *enter = ui_key_hint("enter"), *esc = ui_editor_key("tui.select.cancel");
    char *show = *hide ? xasprintf("%s/%s", hide, enter) : xstrdup(enter);
    char *ht = xasprintf(" \xc2\xb7 %s show \xc2\xb7 %s composer", show, esc), *hint = fg(TUI_TC_DIM, ht);
    char *tt = title_text(v), *lt = xasprintf("%s hidden:", tt), *label = fg(TUI_TC_ACCENT, lt);
    int room = MAX(1, width - tui_visible_width(label) - tui_visible_width(hint) - 1);
    char *sum = hidden_summary(v), *clip = trunc_to(sum, room), *cm = fg(TUI_TC_MUTED, clip);
    char *line = xasprintf("%s %s%s", label, cm, hint);
    strvec_push_own(out, tui_truncate_to_width(line, width, NULL));
    free(line), free(cm), free(clip), free(sum), free(label), free(lt), free(tt), free(hint), free(ht);
    free(show), free(hide), free(enter), free(esc);
}

static void fd_render(tui_component_t *c, int width, strvec_t *out) {
    fd_t *v = (fd_t *)c;
    if (v->hidden) {
        render_hidden(v, width, out);
        return;
    }
    int inner = MAX(1, width - 4);
    char *skip = v->o.skip_notice ? v->o.skip_notice(v->o.hook_ud) : NULL;
    /* the notice takes a body row, so the panel keeps its height while it comes and goes */
    int rows = MAX(1, body_rows(v, inner) - (skip ? 1 : 0));
    int highlight = v->base.focused ? v->focus : -1;
    layout_t L;
    build_layout(v, inner, v->focus, highlight, &L);
    if ((int)L.lines.n > rows && inner > 1) { /* an overflowing column gives the scrollbar a column */
        layout_free(&L);
        build_layout(v, inner - 1, v->focus, highlight, &L);
    }
    char *title = title_text(v);
    strvec_t body = {0};
    render_body(v, &L, rows, inner, &body);
    strvec_push_own(out, ui_box_top(width, title));
    for (size_t i = 0; i < body.n; i++) {
        char *t = tui_visible_width(body.v[i]) > inner ? tui_truncate_to_width(body.v[i], inner, NULL) : xstrdup(body.v[i]);
        strvec_push_own(out, ui_box_row(t, width));
        free(t);
    }
    if (skip) {
        char *one = sanitize_line(skip), *t = xasprintf("\xe2\x9a\xa0 %s", one), *w = fg(TUI_TC_WARNING, t);
        char *tr = tui_visible_width(w) > inner ? tui_truncate_to_width(w, inner, NULL) : xstrdup(w);
        strvec_push_own(out, ui_box_row(tr, width));
        free(one), free(t), free(w), free(tr);
    }
    strvec_push_own(out, ui_box_bottom(width));
    free(skip);
    free(title);
    strvec_free(&body);
    layout_free(&L);
}

static void fd_invalidate(tui_component_t *c) {
    fd_t *v = (fd_t *)c;
    free(v->height_key);
    v->height_key = NULL;
}

static void fd_destroy(tui_component_t *c) {
    fd_t *v = (fd_t *)c;
    stop_countdown(v);
    if (v->hold) ui_signals_release_blocked(v->hold);
    pthread_mutex_lock(&g_live_mu);
    if (g_live == v) g_live = NULL;
    pthread_mutex_unlock(&g_live_mu);
    for (size_t i = 0; i < v->media.n; i++) {
        media_t *m = &v->media.v[i];
        if (m->job) m->job->cancel = 1; /* the job frees itself when its load lands */
        /* the picture's id'd Kitty placement outlives a repaint: it leaves with the form */
        if (m->state == MEDIA_READY) tui_image_release(m->key);
        free(m->reason), free(m->b64), free(m->mime), free(m->key);
    }
    free(v->media.v);
    close_note(v);
    size_t n = v->form->fields.n;
    for (size_t i = 0; i < n; i++) {
        if (v->notes[i])
            for (size_t k = 0; k < v->form->fields.v[i]->opts.n; k++) free(v->notes[i][k]);
        free(v->notes[i]);
        free(v->checked[i]);
        ui_input_free(&v->inputs[i]);
        ui_input_free(&v->others[i]);
        if (v->editors[i]) tui_component_destroy(&v->editors[i]->base);
        free(v->other_seen[i]);
        free(v->errors[i]);
    }
    free(v->checked), free(v->inputs), free(v->others), free(v->editors), free(v->other_seen), free(v->errors);
    free(v->notes);
    free(v->stops.v);
    free(v->intro.v);
    for (size_t g = 0; g < v->groups.n; g++) {
        free(v->groups.v[g].parts.v);
        free(v->groups.v[g].fields.v);
    }
    free(v->groups.v);
    free(v->field_group);
    free(v->height_key);
    bool notify = !v->closed && v->o.done;
    ui_form_done_fn done = v->o.done;
    void *ud = v->o.ud;
    free(v->o.title);
    free(v->o.cwd);
    hf_form_free(v->form);
    free(v);
    if (notify) done(ud, NULL, NULL, false); /* torn down from outside (e.g. /new): cancelled */
}

static const tui_component_vt FD_VT = {"form-dialog", fd_render, fd_key, fd_invalidate, fd_destroy};

bool ui_form_is_dialog(const tui_component_t *c) { return c && c->vt == &FD_VT; }

tui_component_t *ui_form_dialog_new(const char *html, const ui_form_opts_t *o) {
    fd_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &FD_VT);
    v->o = *o;
    v->o.title = o->title ? xstrdup(o->title) : NULL;
    v->o.cwd = o->cwd ? xstrdup(o->cwd) : NULL;
    v->remaining = -1;
    v->form = hf_parse(html);
    pthread_mutex_lock(&g_live_mu);
    v->id = g_next_id++;
    g_live = v;
    pthread_mutex_unlock(&g_live_mu);
    size_t n = v->form->fields.n;
    v->checked = xcalloc(n + 1, sizeof *v->checked);
    v->inputs = xcalloc(n + 1, sizeof *v->inputs);
    v->others = xcalloc(n + 1, sizeof *v->others);
    v->editors = xcalloc(n + 1, sizeof *v->editors);
    v->other_seen = xcalloc(n + 1, sizeof *v->other_seen);
    v->errors = xcalloc(n + 1, sizeof *v->errors);
    v->notes = xcalloc(n + 1, sizeof *v->notes);
    build_groups(v);
    for (size_t si = 0; si < v->form->sections.n; si++) {
        const hf_section_t *sec = &v->form->sections.v[si];
        for (size_t bi = 0; bi < sec->blocks.n; bi++) {
            const hf_block_t *blk = &sec->blocks.v[bi];
            if (blk->kind == HF_B_IMAGE || blk->kind == HF_B_SVG) init_media(v, blk);
            if (blk->kind != HF_B_FIELD) continue;
            hf_field_t *f = blk->field;
            size_t i = fidx(f);
            if (f->kind == HF_TEXT) {
                if (f->multiline) {
                    tui_editor_t *ed = tui_editor_new();
                    tui_editor_set_composer(ed, "borderless");
                    free(ed->gutter);
                    ed->gutter = xstrdup("");
                    ed->padding_x = 0;
                    ed->max_height = textarea_rows(f);
                    ed->disable_submit = true;
                    if (o->draft) {
                        snprintf(ed->text_fg, sizeof ed->text_fg, "%s", o->draft->text_fg);
                        ed->use_terminal_cursor = o->draft->use_terminal_cursor;
                    }
                    tui_editor_set_text(ed, f->value);
                    ed->cl = 0; /* a prefilled draft opens at its first line */
                    ed->cc = 0;
                    v->editors[i] = ed;
                } else {
                    ui_input_set(&v->inputs[i], f->value);
                    v->inputs[i].cur = 0; /* a prefilled input opens at the start of its value */
                }
                stop_t s = {STOP_TEXT, f, 0};
                vec_push(&v->stops, s);
                continue;
            }
            v->checked[i] = xcalloc(f->opts.n + 1, sizeof(bool));
            v->notes[i] = xcalloc(f->opts.n + 1, sizeof(char *));
            for (size_t k = 0; k < f->opts.n; k++) {
                if (!f->opts.v[k].checked) continue;
                if (!f->multi) memset(v->checked[i], 0, f->opts.n * sizeof(bool)); /* several checked radios: the last wins */
                v->checked[i][k] = true;
            }
            if (is_dropdown(f)) {
                bool any = false;
                for (size_t k = 0; k < f->opts.n; k++) any |= !f->opts.v[k].disabled;
                if (any) {
                    stop_t s = {STOP_SELECT, f, 0};
                    vec_push(&v->stops, s);
                }
                continue;
            }
            for (size_t k = 0; k < f->opts.n; k++)
                if (!f->opts.v[k].disabled) {
                    stop_t s = {STOP_OPTION, f, (int)k};
                    vec_push(&v->stops, s);
                }
            if (f->other) {
                stop_t s = {STOP_OTHER, f, 0};
                vec_push(&v->stops, s);
            }
        }
    }
    stop_t sub = {STOP_SUBMIT, NULL, 0};
    vec_push(&v->stops, sub);
    {
        const hf_field_t *only = NULL;
        size_t distinct = 0;
        for (size_t i = 0; i + 1 < v->stops.n; i++)
            if (v->stops.v[i].field != only) {
                bool seen = false;
                for (size_t k = 0; k < i && !seen; k++) seen = v->stops.v[k].field == v->stops.v[i].field;
                if (!seen) distinct++;
                only = v->stops.v[i].field;
            }
        v->sole = distinct == 1;
    }
    /* open where the answer is, so Enter accepts a preselected choice instead of overwriting it */
    if (v->stops.n > 1) v->focus = answer_stop(v, v->stops.v[0].field);
    if (o->timeout_ms > 0) {
        v->deadline = mono_ms() + o->timeout_ms;
        v->remaining = (int)((o->timeout_ms + 999) / 1000);
        if (o->tui) v->timer_id = tui_add_timer(o->tui, 1000, true, on_tick, v);
    }
    return &v->base;
}

/* HtmlFormDialog.givenAnswers: what the user answered so far, for a form closed before its submit: the named fields
 * whose value differs from the form's defaults, and the notes on selected options; NULL when there are neither.
 * Owned {data, notes?} */
json_t *ui_form_dialog_given(tui_component_t *c) {
    if (!ui_form_is_dialog(c)) return NULL;
    fd_t *v = (fd_t *)c;
    json_t *defaults = hf_default_data(v->form), *current = form_data(v), *data = json_obj();
    bool changed = false;
    const char *k;
    json_t *x;
    json_obj_foreach(current, i, k, x) {
        char *a = json_dump_js(x), *b = json_get(defaults, k) ? json_dump_js(json_get(defaults, k)) : NULL;
        bool same = b && !strcmp(a, b);
        free(a);
        free(b);
        if (same) continue;
        json_set(data, k, json_clone(x));
        changed = true;
    }
    json_free(current);
    json_free(defaults);
    json_t *notes = option_notes(v);
    if (!changed && !notes) {
        json_free(data);
        return NULL;
    }
    json_t *r = json_obj();
    json_set(r, "data", data);
    if (notes) json_set(r, "notes", notes);
    return r;
}

/* HtmlFormDialog.keyHints: the form's key hints on one dim line, for the faint composer under it (owned) */
char *ui_form_dialog_key_hints(tui_component_t *c) {
    if (!ui_form_is_dialog(c)) return xstrdup("");
    char *t = key_hint_text((fd_t *)c), *r = fg(TUI_TC_DIM, t);
    free(t);
    return r;
}

json_t *ui_form_dialog_data(tui_component_t *c) { return ui_form_is_dialog(c) ? form_data((fd_t *)c) : NULL; }

void ui_form_dialog_tick(tui_component_t *c, int64_t now_ms) {
    if (!ui_form_is_dialog(c)) return;
    fd_t *v = (fd_t *)c;
    if (v->remaining < 0) return;
    v->deadline = now_ms; /* tests: the deadline passes */
    on_tick(v);
}

/* ---------- the ask slot above the composer (extension-ui-controller #mountAsk, interactive-mode AskContainer) ---------- */

static struct {
    ui_t *u;
    tui_component_t *ask; /* the ask waiting for answers (not owned by the slot) */
} g_slot;

/* AskContainer: the ask, a blank row parting it from the working row under it */
static void slot_render(tui_component_t *c, int width, strvec_t *out) {
    UNUSED(c);
    if (!g_slot.ask) return;
    size_t before = out->n;
    tui_component_render(g_slot.ask, width, out);
    if (out->n > before) strvec_push(out, "");
}

static void slot_invalidate(tui_component_t *c) {
    UNUSED(c);
    if (g_slot.ask) tui_component_invalidate(g_slot.ask);
}

static void slot_destroy(tui_component_t *c) {
    if (g_slot.ask) {
        tui_component_t *a = g_slot.ask;
        g_slot.ask = NULL;
        tui_component_destroy(a);
    }
    g_slot.u = NULL;
    free(c);
}

static const tui_component_vt SLOT_VT = {"ask-slot", slot_render, NULL, slot_invalidate, slot_destroy};

tui_component_t *ui_ask_slot_new(ui_t *u) {
    g_slot.u = u;
    tui_component_t *c = xcalloc(1, sizeof *c);
    tui_component_init(c, &SLOT_VT);
    return c;
}

tui_component_t *ui_ask_current(const ui_t *u) {
    UNUSED(u);
    return g_slot.ask;
}

bool ui_ask_focus(ui_t *u) {
    if (!g_slot.ask) return false;
    tui_set_focus(u->tui, g_slot.ask);
    tui_request_render(u->tui);
    return true;
}

/* #focusComposer: the keys to the composer (Esc or Tab past the ask's last stop, the ask closing) */
static void focus_composer(void *ud) {
    ui_t *u = ud;
    ui_focus_editor_area(u);
    tui_request_render(u->tui);
}

bool ui_ask_holds_keys(const ui_t *u) { return g_slot.ask && tui_focus(u->tui) != &u->editor->base; }

/* #draftSkipsAsk: sending the composer's draft now skips the ask shown: a plain message, which reaches the agent as the
 * user's and closes the ask, not a command, a shell or Python run or a `/btw` question */
bool ui_ask_draft_skips(ui_t *u) {
    if (!g_slot.ask) return false;
    char *text = tui_editor_get_text(u->editor), *t = str_trim(text);
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    size_t dollars = p[0] == '$' ? (p[1] == '$' ? 2 : 1) : 0;
    bool python = dollars && (p[dollars] == ' ' || p[dollars] == '\t' || p[dollars] == '\n' || p[dollars] == '\r');
    bool r = *t && t[0] != '/' && p[0] != '!' && p[0] != '?' && !python;
    free(t);
    free(text);
    return r;
}

#define ASK_SKIP_NOTICE "Sending your message skips these questions"

/* #skipNotice: the ask's warning while sending the composer's draft would skip it, with the key back to it */
static char *skip_notice(void *ud) {
    ui_t *u = ud;
    if (!ui_ask_draft_skips(u)) return NULL;
    const strvec_t *keys = tui_keybindings_get(tui_keybindings_global(), "app.thinking.cycle");
    if (tui_focus(u->tui) == &u->editor->base && keys && keys->n) {
        char *k = ui_key_hint(keys->v[0]), *r = xasprintf(ASK_SKIP_NOTICE " \xc2\xb7 %s answers them", k);
        free(k);
        return r;
    }
    return xstrdup(ASK_SKIP_NOTICE);
}

static void ask_mount(ui_t *u, tui_component_t *ask) {
    g_slot.ask = ask;
    tui_attach(u->tui, ask);
    ui_status_set_awaiting_answer(u, true);
    char *text = tui_editor_get_text(u->editor), *t = str_trim(text);
    bool draft = *t;
    free(t);
    free(text);
    /* the ask takes the keys unless the composer holds a draft, whose sending skips it */
    if (draft || ui_selector_open(u)) tui_request_render(u->tui);
    else ui_ask_focus(u);
}

static void ask_unmount(ui_t *u, tui_component_t *ask) {
    bool had_keys = tui_focus(u->tui) == ask;
    if (g_slot.ask == ask) g_slot.ask = NULL;
    ui_status_set_awaiting_answer(u, g_slot.ask != NULL);
    if (had_keys || !ui_selector_open(u)) focus_composer(u);
    tui_component_destroy(ask);
    tui_request_render(u->tui);
}

void ui_ask_close(ui_t *u, json_t **given) {
    if (given) *given = NULL;
    if (!g_slot.ask) return;
    if (given) *given = ui_form_dialog_given(g_slot.ask);
    ask_unmount(u, g_slot.ask); /* destroying it reports a cancel */
}

void ui_form_dialog_show(ui_t *u, const char *html, int64_t timeout_ms, const char *cwd, ui_form_done_fn done, void *ud) {
    ui_form_opts_t o = {.ui = u, .tui = u->tui, .draft = u->editor, .timeout_ms = timeout_ms, .cwd = (char *)cwd,
                        .done = done, .ud = ud, .skip_notice = skip_notice, .release_keys = focus_composer, .hook_ud = u};
    tui_component_t *c = ui_form_dialog_new(html, &o);
    /* #presentDialog: blocked on the question of the form's first named field while it is shown */
    fd_t *v = (fd_t *)c;
    const char *msg = NULL;
    for (size_t i = 0; i < v->form->fields.n && !msg; i++)
        if (*v->form->fields.v[i]->name) msg = v->form->fields.v[i]->question;
    v->hold = ui_signals_hold_blocked("question", msg);
    if (g_slot.ask) ui_ask_close(u, NULL);
    ask_mount(u, c);
}
