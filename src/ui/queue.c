/* queue.c - messages queued while the agent works: the band above the working row (port of omp
 * prompt/queued-messages.ts QueuedMessagesBand + ui-helpers updatePendingMessagesDisplay) and the
 * `->` / `=>` / `/queue` list splitting (prompt/queue-input.ts). */
#include "ui/ui.h"

#include <ctype.h>

/* ---------- band ---------- */

static void trunc_row(strvec_t *out, const char *styled, int width) {
    tui_trunc_text_t *t = tui_trunc_text_new(styled, 1);
    tui_component_render(&t->base, width, out);
    tui_component_destroy(&t->base);
}

static void band_group(strvec_t *out, const char *label, const strvec_t *msgs, int width) {
    if (!msgs->n) return;
    char *head = xasprintf("%s%s%zu", label, tui_sym("sep.dot"), msgs->n);
    char *h = fg(TUI_TC_MUTED, head);
    trunc_row(out, h, width);
    free(h);
    free(head);
    for (size_t i = 0; i < msgs->n; i++) {
        char *tabs = tui_replace_tabs(msgs->v[i]);
        sb_t b = SB_INIT;
        sb_appendf(&b, "  %zu. ", i + 1);
        for (const char *p = tabs; *p; p++) {
            if (*p == '\r' && p[1] == '\n') continue;
            if (*p == '\n') sb_append(&b, " \xe2\x86\xb5 ");
            else sb_putc(&b, *p);
        }
        char *row = fg(TUI_TC_DIM, sb_cstr(&b));
        trunc_row(out, row, width);
        free(row);
        sb_free(&b);
        free(tabs);
    }
}

static void band_render(tui_component_t *c, int width, strvec_t *out) {
    ui_t *u = c->user;
    if (!u->q_steer.n && !u->q_follow.n) return;
    strvec_push(out, "");
    band_group(out, "Steering", &u->q_steer, width);
    band_group(out, "After yield", &u->q_follow, width);
    char *key = ui_key_hint("alt+up");
    char *hint = xasprintf("  %s %s to edit", tui_sym("tree.hook"), key);
    char *h = fg(TUI_TC_DIM, hint);
    trunc_row(out, h, width);
    free(h);
    free(hint);
    free(key);
}

static const tui_component_vt BAND_VT = {"queued-messages", band_render, NULL, NULL, NULL};

tui_component_t *ui_queue_band_new(ui_t *u) {
    tui_component_t *c = xcalloc(1, sizeof *c);
    tui_component_init(c, &BAND_VT);
    c->user = u;
    return c;
}

void ui_queue_refresh(ui_t *u) {
    strvec_free(&u->q_steer);
    strvec_free(&u->q_follow);
    agent_queue_texts(u->app->agent, &u->q_steer, &u->q_follow);
    if (u->yield_compaction) strvec_push(&u->q_follow, u->yield_compaction);
    json_t *e;
    json_arr_foreach(u->compaction_queue, i, e) {
        bool steer = str_eq(json_get_str(e, "mode"), "steer");
        strvec_push(steer ? &u->q_steer : &u->q_follow, json_get_str(e, "text") ? json_get_str(e, "text") : "");
    }
    tui_request_render(u->tui);
}

/* ---------- queue-input.ts ---------- */

const char *ui_queue_shorthand(const char *text) {
    if (!strncmp(text, "->", 2) || !strncmp(text, "=>", 2)) return text + 2;
    return NULL;
}

typedef struct {
    size_t line;
    char *indent, *marker;
    char punct;
    const char *content; /* into the line */
} qitem_t;

/* QUEUE_LIST_MARKER_RE = /^([\t ]*)(\d+|[A-Za-z]+)([.)])(?=[\t ]|$)/ */
static bool parse_item(const char *line, size_t idx, qitem_t *it) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    const char *m = p;
    if (isdigit((unsigned char)*p)) while (isdigit((unsigned char)*p)) p++;
    else if (isalpha((unsigned char)*p)) while (isalpha((unsigned char)*p)) p++;
    else return false;
    if (*p != '.' && *p != ')') return false;
    if (p[1] && p[1] != ' ' && p[1] != '\t') return false;
    it->line = idx;
    it->indent = xstrndup(line, (size_t)(m - line));
    it->marker = xstrndup(m, (size_t)(p - m));
    it->punct = *p;
    const char *c = p + 1;
    while (*c && isspace((unsigned char)*c)) c++;
    it->content = c;
    return true;
}

static long long dec_val(const char *m) {
    long long v = 0;
    for (const char *p = m; *p; p++) {
        if (!isdigit((unsigned char)*p) || v > 900719925474099LL) return -1;
        v = v * 10 + (*p - '0');
    }
    return v;
}

static int roman_digit(char c) {
    switch (toupper((unsigned char)c)) {
    case 'I': return 1;
    case 'V': return 5;
    case 'X': return 10;
    case 'L': return 50;
    case 'C': return 100;
    case 'D': return 500;
    case 'M': return 1000;
    default: return 0;
    }
}

/* canonical roman numeral (CANONICAL_ROMAN_RE): value, re-encoded, must spell the marker */
static long long roman_val(const char *m) {
    if (!*m) return -1;
    long long v = 0;
    size_t n = strlen(m);
    for (size_t i = 0; i < n; i++) {
        int cur = roman_digit(m[i]);
        if (!cur) return -1;
        int next = i + 1 < n ? roman_digit(m[i + 1]) : 0;
        v += cur < next ? -cur : cur;
    }
    if (v <= 0 || v >= 4000) return -1;
    static const int vals[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char *const syms[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
    sb_t b = SB_INIT;
    long long r = v;
    for (int i = 0; i < 13; i++)
        while (r >= vals[i]) sb_append(&b, syms[i]), r -= vals[i];
    bool ok = !strcasecmp(sb_cstr(&b), m);
    sb_free(&b);
    return ok ? v : -1;
}

static long long alpha_val(const char *m) {
    long long v = 0;
    for (const char *p = m; *p; p++) {
        if (!isalpha((unsigned char)*p) || v > 300000000000000LL) return -1;
        v = v * 26 + (toupper((unsigned char)*p) - 'A' + 1);
    }
    return *m ? v : -1;
}

static bool sequential(qitem_t *items, size_t n, long long (*decode)(const char *)) {
    long long prev = decode(items[0].marker);
    if (prev < 0) return false;
    for (size_t i = 1; i < n; i++) {
        long long cur = decode(items[i].marker);
        if (cur < 0 || cur != prev + 1) return false;
        prev = cur;
    }
    return true;
}

static bool enumerated(qitem_t *items, size_t n) {
    bool all_digits = true, all_upper = true, all_lower = true;
    for (size_t i = 0; i < n; i++) {
        for (const char *p = items[i].marker; *p; p++) {
            if (!isdigit((unsigned char)*p)) all_digits = false;
            if (islower((unsigned char)*p)) all_upper = false;
            if (isupper((unsigned char)*p)) all_lower = false;
        }
    }
    if (all_digits) return sequential(items, n, dec_val);
    if (!all_upper && !all_lower) return false;
    return sequential(items, n, roman_val) || sequential(items, n, alpha_val);
}

static void items_free(qitem_t *items, size_t n) {
    for (size_t i = 0; i < n; i++) free(items[i].indent), free(items[i].marker);
    free(items);
}

void ui_split_queued_messages(const char *text, strvec_t *out) {
    char *src = str_trim(text);
    if (!*src) {
        free(src);
        return;
    }
    strvec_t lines = {0};
    for (const char *p = src;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && p[len - 1] == '\r') len--;
        strvec_push_own(&lines, xstrndup(p, len));
        if (!nl) break;
        p = nl + 1;
    }
    qitem_t *items = xcalloc(lines.n + 1, sizeof *items);
    size_t n = 0;
    bool list = parse_item(lines.v[0], 0, &items[0]);
    if (list) {
        n = 1;
        for (size_t i = 1; i < lines.n; i++) {
            qitem_t it;
            if (!parse_item(lines.v[i], i, &it)) continue;
            if (!strcmp(it.indent, items[0].indent)) items[n++] = it;
            else free(it.indent), free(it.marker);
        }
        if (n < 2) list = false;
        for (size_t i = 0; list && i < n; i++)
            if (items[i].punct != items[0].punct) list = false;
        if (list && !enumerated(items, n)) list = false;
    }
    strvec_t msgs = {0};
    bool ok = list;
    for (size_t i = 0; list && i < n; i++) {
        size_t next = i + 1 < n ? items[i + 1].line : lines.n;
        sb_t b = SB_INIT;
        sb_append(&b, items[i].content);
        for (size_t l = items[i].line + 1; l < next; l++) {
            sb_putc(&b, '\n');
            sb_append(&b, lines.v[l]);
        }
        strvec_push_own(&msgs, str_trim(sb_cstr(&b)));
        sb_free(&b);
    }
    while (msgs.n && !*msgs.v[msgs.n - 1]) free(msgs.v[--msgs.n]);
    for (size_t i = 0; i < msgs.n; i++)
        if (!*msgs.v[i]) ok = false;
    if (ok && msgs.n) {
        for (size_t i = 0; i < msgs.n; i++) strvec_push(out, msgs.v[i]);
    } else {
        strvec_push(out, src);
    }
    strvec_free(&msgs);
    items_free(items, n);
    strvec_free(&lines);
    free(src);
}
