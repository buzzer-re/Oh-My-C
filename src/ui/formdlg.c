/* formdlg.c - the ask tool's HTML form in the composer's place (port of omp packages/tui/src/html-form/dialog.ts
 * HtmlFormDialog, row rendering, and html-form/media.ts; hosted like extension-ui-controller #showLocalFormDialog).
 *
 * The whole form is laid out as one column inside a fixed-height "Ask" panel: section titles, context, choice rows,
 * one-row dropdowns, text fields, then a Submit row. A column taller than the panel (70% of the terminal) splits into
 * pages at section boundaries where possible; focus drives the visible page. Keys: ↑/↓ move between choices and
 * fields, Tab/Shift+Tab jump fields, Space toggles (or opens a dropdown), ←/→ change a dropdown or turn pages, Enter
 * picks a radio and moves on, Enter on Submit validates and submits, PgUp/PgDn turn pages, app.ask.hide collapses
 * the form to one row, Esc cancels. With a timeout, inactivity submits the current values.
 *
 * Tern's native description (question tabs, Review, action bar) is not ported: omc has no native rendering. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "config/settings.h"
#include "core/fs.h"
#include "img/img.h"
#include "net/http.h"
#include "text/htmlform.h"
#include "tui/ansi.h"
#include "tui/termcaps.h"
#include "ui/tools/tc.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>

#define DIALOG_HEIGHT_RATIO 0.7
#define MIN_BODY_ROWS 6
#define CHROME_ROWS 4 /* top border, footer divider, footer, bottom border */
#define FIELD_INDENT 4
#define MAX_TEXTAREA_ROWS 10
#define DEFAULT_TITLE "Ask"
#define SUBMIT_LABEL "Submit"
#define CAPTION_MAX_ROWS 4
#define MAX_PICTURE_ROWS 16
#define PICTURE_SHARE 0.35
#define DROPDOWN_PAGE 5
#define OTHER_PLACEHOLDER "type your own"
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

typedef VEC(bool) boolvec_t;

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
    int focus, page;
    bool follow;
    hf_pages_t pages;
    VEC(size_t) stop_atoms;
    char height_key[96];
    int height_rows;
    hf_field_t *open;        /* dropdown showing its options */
    int highlight;
    bool guard, guard_released;
    int64_t deadline;
    int remaining;           /* -1: no countdown */
    int timer_id;
    VEC(media_t) media;
    int media_version;
    bool hidden, closed;
    char ***notes;            /* per field index (choice): per option, the saved note (NULL: none) */
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

static char *trunc_to(const char *s, int w) { return tui_truncate_to_width(s, MAX(0, w), NULL); }

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

static void finish(fd_t *v, json_t *data, json_t *notes, bool timed_out) {
    if (v->closed) {
        json_free(data);
        json_free(notes);
        return;
    }
    v->closed = true;
    stop_countdown(v);
    ui_form_done_fn done = v->o.done;
    void *ud = v->o.ud;
    if (v->o.ui) {
        ui_close_nested_selector(v->o.ui);
        ui_close_selector(v->o.ui); /* destroys v */
    }
    if (done) done(ud, data, notes, timed_out);
    else {
        json_free(data);
        json_free(notes);
    }
}

/* ---------- option notes ---------- */

static bool guard_blocks(const fd_t *v);

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
    if (v->hidden || guard_blocks(v) || (s->kind != STOP_OPTION && s->kind != STOP_SELECT)) return;
    v->follow = true; /* the editor draws under the focused stop, so its page must be the one shown */
    hf_field_t *f = s->field;
    int option = target >= 0 ? target : s->kind == STOP_OPTION ? s->option : v->open ? v->highlight : current(v, f);
    if (option < 0 || option >= (int)f->opts.n || f->opts.v[option].disabled) return;
    v->guard_released = true; /* opening a note hands the keys over, as up/down does */
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

static void set_focus(fd_t *v, int index) {
    v->focus = MAX(0, MIN((int)v->stops.n - 1, index));
    v->follow = true;
}

static void move_stop(fd_t *v, int delta) { set_focus(v, v->focus + delta); }

static const void *field_of(const stop_t *s) { return s->kind == STOP_SUBMIT ? (const void *)s : (const void *)s->field; }

/* jump to the first stop of the next (or previous) field; Submit counts as a field */
static void move_field(fd_t *v, int delta) {
    const void *cur = field_of(&v->stops.v[v->focus]);
    int i = v->focus;
    while (i + delta >= 0 && i + delta < (int)v->stops.n && field_of(&v->stops.v[i]) == cur) i += delta;
    const void *target = field_of(&v->stops.v[i]);
    while (delta < 0 && i > 0 && field_of(&v->stops.v[i - 1]) == target) i--;
    set_focus(v, i);
}

static int page_of_atom(const fd_t *v, size_t atom) {
    for (size_t i = 0; i < v->pages.n; i++)
        if (atom >= v->pages.v[i].start && atom < v->pages.v[i].end) return (int)i;
    return 0;
}

static void turn_page(fd_t *v, int delta) {
    if (v->pages.n < 2) return;
    int target = MAX(0, MIN((int)v->pages.n - 1, v->page + delta));
    hf_page_t p = v->pages.v[target];
    for (size_t s = 0; s < v->stop_atoms.n; s++)
        if (v->stop_atoms.v[s] >= p.start && v->stop_atoms.v[s] < p.end) {
            set_focus(v, (int)s);
            return;
        }
    v->page = target;
    v->follow = false;
}

static bool guard_blocks(const fd_t *v) {
    return v->guard && !v->guard_released && v->o.draft && !tui_editor_is_empty(v->o.draft);
}

static char *guard_hint(void) {
    char *ud = editor_keys2("tui.select.up", "tui.select.down"), *r = xasprintf("%s answer \xc2\xb7 typing edits your prompt", ud);
    free(ud);
    return r;
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
            move_field(v, 1);
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
        move_field(v, 1);
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
        move_field(v, 1);
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
        move_field(v, 1);
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

static bool fd_key(tui_component_t *c, const tui_key_t *k) {
    fd_t *v = (fd_t *)c;
    if (v->closed) return true;
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
    if (tui_kb(k, "tui.select.cancel")) {
        stop_countdown(v);
        finish(v, NULL, NULL, false);
        return true;
    }
    if (guard_blocks(v)) {
        if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) v->guard_released = true;
        else tui_editor_handle_key(v->o.draft, k);
        request(v);
        return true;
    }
    if (v->hidden) {
        if (is_confirm(k)) set_hidden(v, false);
        request(v);
        return true;
    }
    if (tui_kb(k, "tui.select.pageDown") || tui_kb(k, "tui.select.pageUp")) {
        turn_page(v, tui_kb(k, "tui.select.pageDown") ? 1 : -1);
        request(v);
        return true;
    }
    if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab")) {
        move_field(v, tui_key_matches(k, "shift+tab") ? -1 : 1);
        request(v);
        return true;
    }
    stop_t *s = &v->stops.v[v->focus];
    /* focus sits on another page: ←/→ keep paging, and the keys that would act on it bring it into view */
    if (!v->follow && v->pages.n > 1) {
        if (tui_key_matches(k, "left") || tui_key_matches(k, "right")) turn_page(v, tui_key_matches(k, "right") ? 1 : -1);
        else if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down") || tui_key_matches(k, "space") || is_confirm(k))
            v->follow = true;
        request(v);
        return true;
    }
    if (s->kind == STOP_TEXT) text_key(v, s->field, k);
    else if (s->kind == STOP_SELECT) select_key(v, s->field, k);
    else if (s->kind == STOP_OTHER) other_key(v, s->field, k);
    else if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) move_stop(v, tui_kb(k, "tui.select.down") ? 1 : -1);
    else if (tui_key_matches(k, "left") || tui_key_matches(k, "right")) turn_page(v, tui_key_matches(k, "right") ? 1 : -1);
    else if (s->kind == STOP_SUBMIT) {
        if (is_confirm(k)) {
            submit(v);
            return true; /* v may be gone */
        }
    } else if (matches_note_key(k, false)) start_note(v, -1);
    else if (tui_key_matches(k, "space")) toggle(v, s->field, s->option);
    else if (is_confirm(k)) {
        if (!s->field->multi) toggle(v, s->field, s->option);
        move_field(v, 1);
    }
    request(v);
    return true;
}

/* ---------- rows ---------- */

static char *title_text(const fd_t *v) {
    const char *t = v->o.title ? v->o.title : DEFAULT_TITLE;
    return v->remaining < 0 ? xstrdup(t) : xasprintf("%s (%ds)", t, v->remaining);
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

static void push_atom(hf_atoms_t *atoms, strvec_t lines, int gap, int rank) {
    hf_atom_t a = {.lines = lines, .gap = gap, .break_rank = rank};
    vec_push(atoms, a);
}

/* atoms of one field (each flagged when it is a focus stop); stops are numbered from first_stop */
static void field_atoms(fd_t *v, hf_field_t *f, int width, int focused_stop, int first_stop, hf_atoms_t *atoms,
                        boolvec_t *stops) {
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
        push_atom(atoms, lines, 0, HF_BREAK_INSIDE);
        vec_push(stops, has_stop);
        return;
    }
    other_chosen(v, f); /* settle typing into a radio group's Other before drawing its options */
    if (f->label.n) {
        strvec_t lines = {0};
        base_t base = {TUI_TC_TEXT, false};
        hang("  ", &f->label, base, width, &lines);
        push_atom(atoms, lines, 0, HF_BREAK_INSIDE);
        vec_push(stops, false);
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
        base_t base = {color, false};
        int tw;
        char *tag = note_tag(v, f, (int)i, &tw);
        hang(prefix, &o->label, base, MAX(1, width - tw), &lines);
        if (tag) {
            if (!lines.n) strvec_push(&lines, "");
            char *l0 = xasprintf("%s%s", lines.v[0], tag);
            free(lines.v[0]);
            lines.v[0] = l0;
            free(tag);
        }
        push_atom(atoms, lines, 0, HF_BREAK_INSIDE);
        vec_push(stops, !o->disabled);
        free(prefix);
        free(mk);
        free(cur);
    }
    if (f->other) {
        strvec_t lines = {0};
        strvec_push_own(&lines, other_line(v, f, focused_stop == stop, width));
        push_atom(atoms, lines, 0, HF_BREAK_INSIDE);
        vec_push(stops, true);
    }
    if (err_lines.n) {
        push_atom(atoms, err_lines, 0, HF_BREAK_NEVER);
        vec_push(stops, false);
    } else strvec_free(&err_lines);
}

/* renderTableRow over fitted columns */
static void table_atoms(const hf_block_t *blk, int width, hf_atoms_t *atoms) {
    size_t count = 0;
    for (size_t r = 0; r < blk->rows.n; r++) count = MAX(count, blk->rows.v[r].n);
    VEC(char **) cells = {0};
    int *w = xcalloc(count + 1, sizeof *w);
    for (size_t c = 0; c < count; c++) w[c] = 1;
    for (size_t r = 0; r < blk->rows.n; r++) {
        char **row = xcalloc(count + 1, sizeof *row);
        for (size_t c = 0; c < count; c++) {
            if (c < blk->rows.v[r].n) {
                char *t = hf_spans_text(&blk->rows.v[r].v[c]);
                row[c] = sanitize_line(t);
                free(t);
            } else row[c] = xstrdup("");
            w[c] = MAX(w[c], tui_visible_width(row[c]));
        }
        vec_push(&cells, row);
    }
    /* fitWidths: shrink from the last truncating column down to minWidth 4 */
    int gap = 2, total = 0;
    for (size_t c = 0; c < count; c++) total += w[c];
    int overflow = total + (count ? (int)(count - 1) * gap : 0) - width;
    for (size_t c = count; c-- > 0 && overflow > 0;) {
        int minimum = MIN(w[c], 4), shrink = MIN(overflow, w[c] - minimum);
        w[c] -= shrink;
        overflow -= shrink;
    }
    for (size_t r = 0; r < cells.n; r++) {
        bool head = r < blk->header_rows;
        sb_t line = SB_INIT;
        for (size_t c = 0; c < count; c++) {
            char *t = cells.v[r][c];
            char *fit = tui_visible_width(t) > w[c] ? trunc_to(t, w[c]) : xstrdup(t);
            char *padded = xasprintf("%s%*s", fit, MAX(0, w[c] - tui_visible_width(fit)), "");
            char *styled = head ? fg_bold(TUI_TC_TEXT, padded) : fg(TUI_TC_MUTED, padded);
            if (c) sb_append(&line, "  ");
            sb_append(&line, styled);
            free(styled);
            free(padded);
            free(fit);
        }
        strvec_t lines = {0};
        strvec_push_own(&lines, sb_detach(&line));
        push_atom(atoms, lines, 0, HF_BREAK_INSIDE);
        for (size_t c = 0; c < count; c++) free(cells.v[r][c]);
        free(cells.v[r]);
    }
    free(cells.v);
    free(w);
}

/* atoms of a non-field block: one per wrapped line, list item or table row */
static void context_atoms(fd_t *v, const hf_block_t *blk, int width, hf_atoms_t *atoms) {
    size_t first = atoms->n;
    switch (blk->kind) {
    case HF_B_TEXT: {
        strvec_t lines = {0};
        base_t base = blk->heading ? (base_t){TUI_TC_TEXT, true} : (base_t){TUI_TC_MUTED, false};
        hang("", &blk->spans, base, width, &lines);
        for (size_t i = 0; i < lines.n; i++) {
            strvec_t one = {0};
            strvec_push(&one, lines.v[i]);
            push_atom(atoms, one, 0, HF_BREAK_INSIDE);
        }
        strvec_free(&lines);
        break;
    }
    case HF_B_LIST:
        for (size_t i = 0; i < blk->items.n; i++) {
            char *b = blk->ordered ? xasprintf("%zu. ", i + 1) : xstrdup("\xe2\x80\xa2 ");
            char *bullet = fg(TUI_TC_MD_LIST_BULLET, b);
            strvec_t lines = {0};
            hang(bullet, &blk->items.v[i], (base_t){TUI_TC_MUTED, false}, width, &lines);
            push_atom(atoms, lines, 0, HF_BREAK_INSIDE);
            free(bullet);
            free(b);
        }
        break;
    case HF_B_PRE: {
        char *s = sanitize(blk->text);
        strvec_t ls = str_split(s, "\n");
        for (size_t i = 0; i < ls.n; i++) {
            char *t = trunc_to(ls.v[i], MAX(1, width - 2)), *c = fg(TUI_TC_MD_CODE_BLOCK, t);
            strvec_t lines = {0};
            strvec_push_own(&lines, xasprintf("  %s", c));
            push_atom(atoms, lines, 0, HF_BREAK_INSIDE);
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
        strvec_t lines = {0};
        if (m) media_rows(v, blk, m, width, &lines);
        if (!lines.n && m) {
            char *n = media_note(blk, m), *t = trunc_to(n, width);
            strvec_push_own(&lines, fg(TUI_TC_DIM, t));
            free(n);
            free(t);
        }
        push_atom(atoms, lines, 0, HF_BREAK_INSIDE);
        break;
    }
    case HF_B_TABLE: {
        table_atoms(blk, width, atoms);
        size_t made = atoms->n - first;
        if (blk->header_rows > 0 && blk->header_rows < made) {
            sb_t r = SB_INIT;
            sb_repeat(&r, "\xe2\x94\x80", width);
            char *rule = fg(TUI_TC_DIM, sb_cstr(&r));
            sb_free(&r);
            strvec_t lines = {0};
            strvec_push_own(&lines, rule);
            hf_atom_t sep = {.lines = lines, .gap = 0, .break_rank = HF_BREAK_NEVER};
            vec_push(atoms, sep);
            /* move it into place after the header rows */
            size_t at = first + blk->header_rows;
            hf_atom_t tmp = atoms->v[atoms->n - 1];
            memmove(&atoms->v[at + 1], &atoms->v[at], (atoms->n - 1 - at) * sizeof *atoms->v);
            atoms->v[at] = tmp;
        }
        break;
    }
    case HF_B_FIELD:
        break;
    }
}

typedef struct {
    hf_atoms_t atoms;
    VEC(size_t) stop_atoms;
    bool has_dropdown;
    size_t dd_atom;
    int dd_row;
} layout_t;

static void layout_free(layout_t *l) {
    hf_atoms_free(&l->atoms);
    free(l->stop_atoms.v);
}

static void build_layout(fd_t *v, int width, layout_t *L) {
    memset(L, 0, sizeof *L);
    int stop = 0;
    int focused_stop = v->base.focused ? v->focus : -1;
    for (size_t si = 0; si < v->form->sections.n; si++) {
        const hf_section_t *sec = &v->form->sections.v[si];
        char *tt = hf_spans_text(&sec->title), *title = sanitize_line(tt);
        free(tt);
        int gap = si == 0 ? 0 : 1, rank = HF_BREAK_SECTION;
        if (*title) {
            strvec_t lines = {0};
            hang("", &sec->title, (base_t){TUI_TC_TEXT, true}, width, &lines);
            push_atom(&L->atoms, lines, gap, rank);
            gap = 0;
            rank = HF_BREAK_NEVER;
        }
        bool have_caption = false;
        size_t cap_start = 0, cap_end = 0;
        for (size_t bi = 0; bi < sec->blocks.n; bi++) {
            const hf_block_t *blk = &sec->blocks.v[bi];
            hf_atoms_t ba = {0};
            boolvec_t stops = {0};
            if (blk->kind == HF_B_FIELD) {
                /* models ask a group's question in a <p> right before it: keep the two as one unit */
                if (have_caption && cap_end - cap_start <= CAPTION_MAX_ROWS) {
                    for (size_t i = cap_start + 1; i < cap_end; i++) L->atoms.v[i].break_rank = HF_BREAK_NEVER;
                    gap = 0;
                    rank = HF_BREAK_NEVER;
                }
                hf_field_t *f = blk->field;
                field_atoms(v, f, width, focused_stop, stop, &ba, &stops);
                for (size_t i = 0; i < ba.n; i++)
                    if (stops.v[i]) {
                        size_t at = L->atoms.n + i;
                        vec_push(&L->stop_atoms, at);
                        stop++;
                    }
                if (f->kind == HF_CHOICE && v->open == f) {
                    L->has_dropdown = true;
                    L->dd_atom = L->atoms.n;
                    L->dd_row = f->label.n ? (int)ba.v[0].lines.n - 1 : 0;
                }
            } else context_atoms(v, blk, width, &ba);
            free(stops.v);
            if (!ba.n) {
                free(ba.v);
                continue;
            }
            ba.v[0].gap = gap;
            ba.v[0].break_rank = rank;
            /* no orphans or widows: a split block keeps at least two atoms on each page */
            if (ba.n > 1) {
                ba.v[1].break_rank = HF_BREAK_NEVER;
                ba.v[ba.n - 1].break_rank = HF_BREAK_NEVER;
            }
            have_caption = blk->kind == HF_B_TEXT;
            cap_start = L->atoms.n;
            cap_end = L->atoms.n + ba.n;
            for (size_t i = 0; i < ba.n; i++) {
                if (*title) ba.v[i].continuation = xstrdup(title);
                vec_push(&L->atoms, ba.v[i]);
            }
            free(ba.v);
            gap = 1;
            rank = HF_BREAK_BLOCK;
        }
        free(title);
    }
    bool submit_focused = focused_stop == (int)v->stops.n - 1;
    char *sub;
    if (submit_focused) {
        char *t = xasprintf("%s %s", tui_sym("nav.cursor"), SUBMIT_LABEL), *b = tui_bold(t);
        sub = fg(TUI_TC_ACCENT, b);
        free(t);
        free(b);
    } else sub = fg(TUI_TC_MUTED, "  " SUBMIT_LABEL);
    char *pending = NULL;
    if (v->nerrors) {
        char *t = xasprintf("  %zu to fix", v->nerrors);
        pending = fg(TUI_TC_ERROR, t);
        free(t);
    } else pending = xstrdup("");
    vec_push(&L->stop_atoms, L->atoms.n);
    strvec_t lines = {0};
    strvec_push_own(&lines, xasprintf("%s%s", sub, pending));
    push_atom(&L->atoms, lines, 1, HF_BREAK_BLOCK);
    free(sub);
    free(pending);
}

/* body rows: the whole form's natural height, capped by the terminal; fixed per width, height, error count and
 * media version, so moving focus or typing never resizes the panel */
static int body_rows(fd_t *v, int width, const hf_atoms_t *atoms) {
    int trows = term_rows(v);
    char key[96];
    snprintf(key, sizeof key, "%d:%d:%zu:%d", width, trows, v->nerrors, v->media_version);
    if (!strcmp(key, v->height_key)) return v->height_rows;
    int cap = MAX(MIN_BODY_ROWS, (int)floor(trows * DIALOG_HEIGHT_RATIO) - CHROME_ROWS);
    int rows = MIN(cap, MAX(1, hf_page_height(atoms, 0, atoms->n)));
    snprintf(v->height_key, sizeof v->height_key, "%s", key);
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
                char *cl = cl0;
                free(cs0), free(tag);
                char *padding = pad(inner - 3 - tui_visible_width(lab) - tw);
                char *lv = xasprintf("%s ", tui_sym("boxRound.vertical")), *rv = xasprintf(" %s", tui_sym("boxRound.vertical"));
                char *ls = fg(TUI_TC_ACCENT, lv), *rs = fg(TUI_TC_ACCENT, rv);
                strvec_push_own(out, xasprintf("%s%s%s%s%s", ls, cs, padding, mark, rs));
                free(c), free(mark), free(lab), free(cl), free(cs), free(padding), free(lv), free(rv), free(ls), free(rs);
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

static void render_page(fd_t *v, const layout_t *L, hf_page_t page, int rows, int width, strvec_t *out) {
    strvec_t lines = {0};
    const hf_atoms_t *a = &L->atoms;
    if (page.start < a->n && a->v[page.start].continuation) {
        char *t = xasprintf("%s (cont.)", a->v[page.start].continuation), *tr = trunc_to(t, width);
        strvec_push_own(&lines, fg(TUI_TC_DIM, tr));
        free(t);
        free(tr);
    }
    bool anchored = false;
    int anchor_top = 0, anchor_row = 0, note_row = -1;
    size_t focus_atom = (size_t)v->focus < v->stop_atoms.n ? v->stop_atoms.v[v->focus] : (size_t)-1;
    for (size_t i = page.start; i < page.end; i++) {
        if (i > page.start)
            for (int g = 0; g < a->v[i].gap; g++) strvec_push(&lines, "");
        if (L->has_dropdown && L->dd_atom == i) {
            anchored = true;
            anchor_top = (int)lines.n;
            anchor_row = (int)lines.n + L->dd_row;
        }
        if (v->note_ed && i == focus_atom) note_row = (int)(lines.n + a->v[i].lines.n) - 1;
        for (size_t k = 0; k < a->v[i].lines.n; k++) strvec_push(&lines, a->v[i].lines.v[k]);
    }
    /* like the dropdown, the note editor covers the rows under its option rather than repaginating */
    if (v->note_ed && note_row >= 0) {
        int inner = MAX(1, width - FIELD_INDENT);
        int start = MAX(0, note_row - MAX(0, rows - 3));
        strvec_t shown = {0};
        for (int i = start; i < (int)lines.n && i < start + rows; i++) strvec_push(&shown, lines.v[i]);
        int top = note_row - start + 1;
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
        /* splice(top, editing.length, ...editing) */
        strvec_t out2 = {0};
        for (int i = 0; i < top && i < (int)shown.n; i++) strvec_push(&out2, shown.v[i]);
        for (size_t i = 0; i < editing.n; i++) strvec_push_own(&out2, xasprintf("%s%s", ind, editing.v[i]));
        for (int i = top + (int)editing.n; i < (int)shown.n; i++) strvec_push(&out2, shown.v[i]);
        free(ind);
        for (size_t i = 0; i < out2.n && (int)i < rows; i++) strvec_push(out, out2.v[i]);
        strvec_free(&out2);
        strvec_free(&editing);
        strvec_free(&shown);
        strvec_free(&lines);
        return;
    }
    while ((int)lines.n > rows) free(lines.v[--lines.n]);
    if (anchored && v->open && anchor_row < rows) {
        int below = rows - anchor_row - 1, above = anchor_top;
        int wanted = (int)v->open->opts.n + 2;
        bool place_below = below >= MIN(wanted, 5) || below >= above;
        strvec_t box = {0};
        dropdown_box(v, width, place_below ? below : above, &box);
        if (box.n) {
            int start = place_below ? anchor_row + 1 : anchor_top - (int)box.n;
            while ((int)lines.n < start + (int)box.n) strvec_push(&lines, "");
            char *ind = pad(FIELD_INDENT - 2);
            for (size_t i = 0; i < box.n; i++) {
                free(lines.v[start + i]);
                lines.v[start + i] = xasprintf("%s%s", ind, box.v[i]);
            }
            free(ind);
        }
        strvec_free(&box);
    }
    for (size_t i = 0; i < lines.n; i++) strvec_push(out, lines.v[i]);
    strvec_free(&lines);
}

static char *footer_text(fd_t *v) {
    if (v->note_ed) return note_hint(v);
    char *hide = ui_editor_key("app.ask.hide"), *esc = ui_editor_key("tui.select.cancel");
    char *cancel = *hide ? xasprintf("%s cancel \xc2\xb7 %s hide", esc, hide) : xasprintf("%s cancel", esc);
    char *r;
    if (guard_blocks(v)) {
        char *g = guard_hint();
        r = xasprintf("%s \xc2\xb7 %s", g, cancel);
        free(g);
        free(hide), free(esc), free(cancel);
        return r;
    }
    char *enter = ui_key_hint("enter"), *updown = editor_keys2("tui.select.up", "tui.select.down");
    const stop_t *s = &v->stops.v[v->focus];
    strvec_t nk = note_keys(s->kind == STOP_SELECT);
    char *nkh = nk.n ? ui_key_hint(nk.v[0]) : NULL;
    char *note = nkh ? xasprintf(" \xc2\xb7 %s note", nkh) : xstrdup("");
    free(nkh);
    strvec_free(&nk);
    if (v->open) {
        r = xasprintf("%s highlight \xc2\xb7 %s choose%s \xc2\xb7 %s close", updown, enter, note, esc);
        free(hide), free(esc), free(cancel), free(enter), free(updown), free(note);
        return r;
    }
    char *space = ui_key_hint("space"), *left = ui_key_hint("left"), *right = ui_key_hint("right"), *tab = ui_key_hint("tab");
    char *action;
    if (s->kind == STOP_SUBMIT) action = xasprintf("%s submit", enter);
    else if (s->kind == STOP_TEXT) action = xasprintf("%s next", enter);
    else if (s->kind == STOP_OTHER) action = xasprintf("type your own \xc2\xb7 %s next", enter);
    else if (s->kind == STOP_SELECT)
        action = xasprintf("%s open \xc2\xb7 %s/%s change \xc2\xb7 %s next", space, left, right, enter);
    else if (s->field->multi) action = xasprintf("%s toggle \xc2\xb7 %s next", space, enter);
    else action = xasprintf("%s choose", enter);
    if (s->kind == STOP_OPTION || s->kind == STOP_SELECT) {
        char *a2 = xasprintf("%s%s", action, note);
        free(action);
        action = a2;
    }
    free(note);
    sb_t b = SB_INIT;
    sb_appendf(&b, "%s \xc2\xb7 %s move \xc2\xb7 %s next field \xc2\xb7 %s", action, updown, tab, cancel);
    if (v->pages.n > 1) {
        char *keys = s->kind == STOP_TEXT || s->kind == STOP_SELECT ? editor_keys2("tui.select.pageUp", "tui.select.pageDown")
                                                                     : xasprintf("%s/%s", left, right);
        sb_appendf(&b, " \xc2\xb7 %s page", keys);
        free(keys);
    }
    free(action), free(space), free(left), free(right), free(tab);
    free(hide), free(esc), free(cancel), free(enter), free(updown);
    return sb_detach(&b);
}

/* the question in focus (else the first) for the hidden row, with how many more there are */
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

static void render_hidden(fd_t *v, int width, strvec_t *out) {
    char *hide = ui_editor_key("app.ask.hide"), *enter = ui_key_hint("enter"), *esc = ui_editor_key("tui.select.cancel");
    bool blocked = guard_blocks(v);
    char *show = blocked ? xstrdup(hide) : *hide ? xasprintf("%s/%s", hide, enter) : xstrdup(enter);
    sb_t h = SB_INIT;
    if (blocked) {
        char *g = guard_hint();
        sb_appendf(&h, "%s \xc2\xb7 ", g);
        free(g);
    }
    if (*show) sb_appendf(&h, "%s show \xc2\xb7 ", show);
    sb_appendf(&h, "%s cancel", esc);
    char *ht = xasprintf(" \xc2\xb7 %s", sb_cstr(&h)), *hint = fg(TUI_TC_DIM, ht);
    char *tt = title_text(v), *lt = xasprintf("%s hidden:", tt), *label = fg(TUI_TC_ACCENT, lt);
    int room = MAX(1, width - tui_visible_width(label) - tui_visible_width(hint) - 1);
    char *sum = hidden_summary(v), *clip = trunc_to(sum, room), *cm = fg(TUI_TC_MUTED, clip);
    char *line = xasprintf("%s %s%s", label, cm, hint);
    strvec_push_own(out, trunc_to(line, width));
    free(line), free(cm), free(clip), free(sum), free(label), free(lt), free(tt), free(hint), free(ht);
    sb_free(&h);
    free(show), free(hide), free(enter), free(esc);
}

static void fd_render(tui_component_t *c, int width, strvec_t *out) {
    fd_t *v = (fd_t *)c;
    if (v->o.draft) v->o.draft->base.focused = guard_blocks(v);
    if (v->hidden) render_hidden(v, width, out);
    else {
        int inner = MAX(1, width - 4);
        layout_t L;
        build_layout(v, inner, &L);
        int rows = body_rows(v, inner, &L.atoms);
        hf_paginate(&L.atoms, rows, &v->pages);
        v->stop_atoms.n = 0;
        for (size_t i = 0; i < L.stop_atoms.n; i++) vec_push(&v->stop_atoms, L.stop_atoms.v[i]);
        size_t fa = (size_t)v->focus < L.stop_atoms.n ? L.stop_atoms.v[v->focus] : 0;
        int focus_page = page_of_atom(v, fa);
        if (v->follow) v->page = focus_page;
        v->page = MIN(v->page, (int)v->pages.n - 1);
        if (v->page == focus_page) v->follow = true;
        char *tt = title_text(v), *title;
        if (v->pages.n > 1) title = xasprintf("%s \xc2\xb7 %d/%zu", tt, v->page + 1, v->pages.n);
        else title = xstrdup(tt);
        strvec_t body = {0};
        render_page(v, &L, v->pages.v[v->page], rows, inner, &body);
        while ((int)body.n < rows) strvec_push(&body, "");
        char *ft = footer_text(v), *fs = fg(TUI_TC_DIM, ft);
        strvec_push_own(out, ui_box_top(width, title));
        for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
        strvec_push_own(out, ui_box_divider(width));
        strvec_push_own(out, ui_box_row(fs, width));
        strvec_push_own(out, ui_box_bottom(width));
        free(ft), free(fs), free(title), free(tt);
        strvec_free(&body);
        layout_free(&L);
    }
    if (v->guard && v->o.draft) { /* the guarded draft stays visible beneath the form while typing edits it */
        tui_component_t *ed = &v->o.draft->base;
        ed->vt->render(ed, width, out);
    }
}

static void fd_invalidate(tui_component_t *c) {
    fd_t *v = (fd_t *)c;
    v->height_key[0] = 0;
}

static void fd_destroy(tui_component_t *c) {
    fd_t *v = (fd_t *)c;
    stop_countdown(v);
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
    free(v->pages.v);
    free(v->stop_atoms.v);
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
    if (o->draft && !tui_editor_is_empty(o->draft)) v->guard = true;
    if (o->timeout_ms > 0) {
        v->deadline = mono_ms() + o->timeout_ms;
        v->remaining = (int)((o->timeout_ms + 999) / 1000);
        if (o->tui) v->timer_id = tui_add_timer(o->tui, 1000, true, on_tick, v);
    }
    return &v->base;
}

void ui_form_dialog_show(ui_t *u, const char *html, int64_t timeout_ms, const char *cwd, ui_form_done_fn done, void *ud) {
    ui_form_opts_t o = {.ui = u, .tui = u->tui, .draft = u->editor, .timeout_ms = timeout_ms, .cwd = (char *)cwd,
                        .done = done, .ud = ud};
    tui_component_t *c = ui_form_dialog_new(html, &o);
    ui_show_selector(u, c, c);
}

json_t *ui_form_dialog_data(tui_component_t *c) { return ui_form_is_dialog(c) ? form_data((fd_t *)c) : NULL; }

void ui_form_dialog_tick(tui_component_t *c, int64_t now_ms) {
    if (!ui_form_is_dialog(c)) return;
    fd_t *v = (fd_t *)c;
    if (v->remaining < 0) return;
    v->deadline = now_ms; /* tests: the deadline passes */
    on_tick(v);
}
