/* conflict.c - git merge-conflict blocks surfaced by `read` and the `conflict://` URLs that resolve them (port of
 * omp tools/conflict-detect.ts, tools/conflict-uri.ts, internal-urls/conflict-protocol.ts and the
 * renderConflictRegion helper of packages/tui/src/tools/conflict-detect.ts).
 *
 *   read <file>            registers each complete block the window shows and appends a warning footer
 *   read <file>:conflicts  scans the whole file (10 MB cap), registers every block, prints a one-line index
 *   read conflict://N[/ours|theirs|base]   one block (or side) with its file line numbers
 *   write conflict://N     splice content over the block (`@ours`/`@theirs`/`@base`/`@both` lines expand)
 *   write conflict://STAR   every registered block, or per-id `<id>: @side` directives
 * Ids are per session (tool_env), reused only for the same block at the same position. */
#include "iurl.h"

#include "config/settings.h"
#include "core/fs.h"
#include "fuzzy.h"
#include "hashline.h"

#include <ctype.h>
#include <sys/stat.h>

#define OURS_PREFIX "<<<<<<<"
#define BASE_PREFIX "|||||||"
#define SEPARATOR "======="
#define THEIRS_PREFIX ">>>>>>>"
#define SCAN_MAX_BYTES (10u * 1024 * 1024)
#define PREVIEW_SIDE_LINES 6
#define MAX_ECHO_LINES 12
#define COLLAPSED_ITEMS 8

typedef struct {
    int64_t start_line, separator_line, end_line, base_line; /* base_line 0: 2-way */
    char *ours_label, *base_label, *theirs_label;           /* NULL: no label */
    strvec_t ours, base, theirs;
    bool has_base;
} cblock_t;

typedef struct {
    int id;
    char *abs, *display;
    cblock_t b;
} centry_t;

typedef VEC(cblock_t) cblocks_t;

struct conflict_history {
    int next_id;
    VEC(centry_t *) entries; /* insertion (Map) order */
};

static void cblock_free(cblock_t *b) {
    free(b->ours_label);
    free(b->base_label);
    free(b->theirs_label);
    strvec_free(&b->ours);
    strvec_free(&b->base);
    strvec_free(&b->theirs);
    memset(b, 0, sizeof *b);
}

static void strvec_copy(strvec_t *dst, const strvec_t *src) {
    for (size_t i = 0; i < src->n; i++) strvec_push(dst, src->v[i]);
}

static void cblock_copy(cblock_t *dst, const cblock_t *src) {
    *dst = (cblock_t){.start_line = src->start_line,
                      .separator_line = src->separator_line,
                      .end_line = src->end_line,
                      .base_line = src->base_line,
                      .has_base = src->has_base,
                      .ours_label = src->ours_label ? xstrdup(src->ours_label) : NULL,
                      .base_label = src->base_label ? xstrdup(src->base_label) : NULL,
                      .theirs_label = src->theirs_label ? xstrdup(src->theirs_label) : NULL};
    strvec_copy(&dst->ours, &src->ours);
    strvec_copy(&dst->base, &src->base);
    strvec_copy(&dst->theirs, &src->theirs);
}

static void cblocks_free(cblocks_t *v) {
    for (size_t i = 0; i < v->n; i++) cblock_free(&v->v[i]);
    vec_free(v);
}

static void centry_free(centry_t *e) {
    if (!e) return;
    free(e->abs);
    free(e->display);
    cblock_free(&e->b);
    free(e);
}

void conflict_history_free(conflict_history_t *h) {
    if (!h) return;
    for (size_t i = 0; i < h->entries.n; i++) centry_free(h->entries.v[i]);
    vec_free(&h->entries);
    free(h);
}

static conflict_history_t *history(tool_env_t *env) {
    pthread_mutex_lock(&env->mu);
    if (!env->conflicts) {
        env->conflicts = xcalloc(1, sizeof *env->conflicts);
        env->conflicts->next_id = 1;
    }
    pthread_mutex_unlock(&env->mu);
    return env->conflicts;
}

/* ---------- scanning ---------- */

static char *strip_cr(const char *s) {
    size_t n = strlen(s);
    return n && s[n - 1] == '\r' ? xstrndup(s, n - 1) : xstrdup(s);
}

/* label after a marker prefix ("" for a bare marker), NULL when `line` is not that marker */
static const char *match_marker(const char *line, const char *prefix) {
    size_t pl = strlen(prefix);
    if (strncmp(line, prefix, pl) != 0) return NULL;
    if (!line[pl]) return "";
    if (line[pl] != ' ') return NULL;
    return line + pl + 1;
}

static char *label_or_null(const char *l) { return l && *l ? xstrdup(l) : NULL; }

/* omp scanConflictLines: completed blocks of `lines` (first_line = 1-based number of lines[0]) */
static void scan_lines(const strvec_t *lines, int64_t first_line, cblocks_t *out) {
    enum { IDLE, OURS, BASE, THEIRS } phase = IDLE;
    cblock_t part = {0};
    bool have = false;
    for (size_t i = 0; i < lines->n; i++) {
        char *line = strip_cr(lines->v[i]);
        int64_t ln = first_line + (int64_t)i;
        const char *lab = match_marker(line, OURS_PREFIX);
        if (lab) {
            if (have) cblock_free(&part);
            part = (cblock_t){.start_line = ln, .ours_label = label_or_null(lab)};
            have = true;
            phase = OURS;
        } else if (phase == IDLE || !have) {
            /* outside a block */
        } else if ((lab = match_marker(line, BASE_PREFIX))) {
            if (phase != OURS) {
                cblock_free(&part);
                have = false;
                phase = IDLE;
            } else {
                part.base_line = ln;
                part.has_base = true;
                part.base_label = label_or_null(lab);
                phase = BASE;
            }
        } else if (!strcmp(line, SEPARATOR)) {
            if (phase == OURS || phase == BASE) {
                part.separator_line = ln;
                phase = THEIRS;
            } else {
                cblock_free(&part);
                have = false;
                phase = IDLE;
            }
        } else if ((lab = match_marker(line, THEIRS_PREFIX))) {
            if (phase == THEIRS && part.separator_line) {
                part.end_line = ln;
                part.theirs_label = label_or_null(lab);
                vec_push(out, part);
            } else {
                cblock_free(&part);
            }
            memset(&part, 0, sizeof part);
            have = false;
            phase = IDLE;
        } else if (phase == OURS) {
            strvec_push(&part.ours, line);
        } else if (phase == BASE) {
            strvec_push(&part.base, line);
        } else if (phase == THEIRS) {
            strvec_push(&part.theirs, line);
        }
        free(line);
    }
    if (have) cblock_free(&part);
}

/* a leading UTF-8 BOM: TextDecoder and `Bun.file().text()` drop it, so scans and splices see the text after it */
static const char *after_bom(const char *t) { return str_starts(t, "\xEF\xBB\xBF") ? t + 3 : t; }

/* omp scanFileForConflicts: *truncated_at = first unscanned line (0 when the whole file was scanned) */
static bool scan_file(const char *abs, cblocks_t *out, int64_t *truncated_at) {
    *truncated_at = 0;
    size_t n = 0;
    bool trunc = false;
    char *text = fs_read_limit(abs, SCAN_MAX_BYTES, &n, &trunc);
    if (!text) return false;
    strvec_t lines = str_split(after_bom(text), "\n");
    scan_lines(&lines, 1, out);
    if (trunc) *truncated_at = (int64_t)lines.n;
    strvec_free(&lines);
    free(text);
    return true;
}

static strvec_t recorded_region(const cblock_t *b) {
    strvec_t out = {0};
    strvec_push_own(&out, b->ours_label ? xasprintf(OURS_PREFIX " %s", b->ours_label) : xstrdup(OURS_PREFIX));
    strvec_copy(&out, &b->ours);
    if (b->has_base) {
        strvec_push_own(&out, b->base_label ? xasprintf(BASE_PREFIX " %s", b->base_label) : xstrdup(BASE_PREFIX));
        strvec_copy(&out, &b->base);
    }
    strvec_push(&out, SEPARATOR);
    strvec_copy(&out, &b->theirs);
    strvec_push_own(&out, b->theirs_label ? xasprintf(THEIRS_PREFIX " %s", b->theirs_label) : xstrdup(THEIRS_PREFIX));
    return out;
}

static bool sections_equal(const strvec_t *a, const strvec_t *b) {
    if (a->n != b->n) return false;
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->v[i], b->v[i])) return false;
    return true;
}

static bool regions_equal(const cblock_t *a, const cblock_t *b) {
    strvec_t ra = recorded_region(a), rb = recorded_region(b);
    bool eq = sections_equal(&ra, &rb);
    strvec_free(&ra);
    strvec_free(&rb);
    return eq;
}

/* ---------- history ---------- */

static void remove_at(conflict_history_t *h, size_t i) {
    centry_free(h->entries.v[i]);
    memmove(&h->entries.v[i], &h->entries.v[i + 1], (h->entries.n - i - 1) * sizeof h->entries.v[0]);
    h->entries.n--;
}

static centry_t *find_id(conflict_history_t *h, int id) {
    for (size_t i = 0; i < h->entries.n; i++)
        if (h->entries.v[i]->id == id) return h->entries.v[i];
    return NULL;
}

static void invalidate_id(conflict_history_t *h, int id) {
    for (size_t i = 0; i < h->entries.n; i++)
        if (h->entries.v[i]->id == id) {
            remove_at(h, i);
            return;
        }
}

static void invalidate_path(conflict_history_t *h, const char *abs) {
    for (size_t i = 0; i < h->entries.n;) {
        if (!strcmp(h->entries.v[i]->abs, abs)) remove_at(h, i);
        else i++;
    }
}

/* omp ConflictHistory.register: reuse an id only for the same block at the same position */
static centry_t *history_register(conflict_history_t *h, const char *abs, const char *display, const cblock_t *b) {
    for (size_t i = 0; i < h->entries.n;) {
        centry_t *e = h->entries.v[i];
        if (!strcmp(e->abs, abs) && e->b.start_line == b->start_line) {
            if (!regions_equal(&e->b, b)) {
                remove_at(h, i);
                continue;
            }
            free(e->display);
            e->display = xstrdup(display);
            cblock_free(&e->b);
            cblock_copy(&e->b, b);
            return e;
        }
        i++;
    }
    centry_t *e = xcalloc(1, sizeof *e);
    e->id = h->next_id++;
    e->abs = xstrdup(abs);
    e->display = xstrdup(display);
    cblock_copy(&e->b, b);
    vec_push(&h->entries, e);
    return e;
}

/* omp ConflictHistory.reconcilePath: refresh registered blocks of `abs` against a scan (no new registrations) */
static void reconcile_path(conflict_history_t *h, const char *abs, const cblocks_t *scan, int64_t truncated_at) {
    size_t n = 0;
    for (size_t i = 0; i < h->entries.n; i++) n += !strcmp(h->entries.v[i]->abs, abs);
    if (!n) return;
    centry_t **ents = xcalloc(n, sizeof *ents);
    ssize_t *match = xcalloc(n, sizeof *match);
    bool *taken = xcalloc(scan->n + 1, sizeof *taken);
    size_t k = 0;
    for (size_t i = 0; i < h->entries.n; i++)
        if (!strcmp(h->entries.v[i]->abs, abs)) ents[k++] = h->entries.v[i];
    for (size_t i = 0; i < n; i++) {
        match[i] = -1;
        for (size_t j = 0; j < scan->n; j++) {
            if (taken[j]) continue;
            if (scan->v[j].start_line == ents[i]->b.start_line && regions_equal(&ents[i]->b, &scan->v[j])) {
                match[i] = (ssize_t)j;
                taken[j] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (match[i] >= 0) continue;
        int64_t best = INT64_MAX;
        for (size_t j = 0; j < scan->n; j++) {
            if (taken[j]) continue;
            int64_t d = llabs(scan->v[j].start_line - ents[i]->b.start_line);
            if (d < best && regions_equal(&ents[i]->b, &scan->v[j])) {
                best = d;
                match[i] = (ssize_t)j;
            }
        }
        if (match[i] >= 0) taken[match[i]] = true;
    }
    for (size_t i = 0; i < n; i++) {
        if (match[i] >= 0) {
            cblock_free(&ents[i]->b);
            cblock_copy(&ents[i]->b, &scan->v[match[i]]);
        } else if (!truncated_at || ents[i]->b.end_line < truncated_at) {
            invalidate_id(h, ents[i]->id);
        }
    }
    free(ents);
    free(match);
    free(taken);
}

/* omp reconcileRegisteredConflicts (all files, or only `abs`): unchanged blocks follow line shifts, changed or
 * vanished ones retire, a deleted file retires all of its ids */
static void reconcile_registered(conflict_history_t *h, const char *only_abs) {
    strvec_t files = {0};
    for (size_t i = 0; i < h->entries.n; i++) {
        const char *a = h->entries.v[i]->abs;
        if ((!only_abs || !strcmp(a, only_abs)) && !strvec_contains(&files, a)) strvec_push(&files, a);
    }
    for (size_t f = 0; f < files.n; f++) {
        char *text = fs_read(files.v[f], NULL);
        if (!text) {
            invalidate_path(h, files.v[f]);
            continue;
        }
        strvec_t lines = str_split(after_bom(text), "\n");
        cblocks_t all = {0}, found = {0};
        scan_lines(&lines, 1, &all);
        /* only blocks equal to a registered region count (findRegisteredBlocks) */
        for (size_t j = 0; j < all.n; j++) {
            bool known = false;
            for (size_t i = 0; i < h->entries.n && !known; i++)
                known = !strcmp(h->entries.v[i]->abs, files.v[f]) && regions_equal(&h->entries.v[i]->b, &all.v[j]);
            if (known) {
                vec_push(&found, all.v[j]);
                memset(&all.v[j], 0, sizeof all.v[j]);
            }
        }
        reconcile_path(h, files.v[f], &found, 0);
        cblocks_free(&all);
        cblocks_free(&found);
        strvec_free(&lines);
        free(text);
    }
    strvec_free(&files);
}

/* ---------- formatting ---------- */

static const char *pick_label(centry_t *const *es, size_t n, int which) {
    for (size_t i = 0; i < n; i++) {
        const cblock_t *b = &es[i]->b;
        const char *l = which == 0 ? b->ours_label : which == 1 ? b->theirs_label : b->has_base ? b->base_label : NULL;
        if (l) {
            for (const char *p = l; *p; p++)
                if (!isspace((unsigned char)*p)) return l;
        }
    }
    return NULL;
}

static void label_lines(sb_t *b, centry_t *const *es, size_t n) {
    const char *ours = pick_label(es, n, 0), *theirs = pick_label(es, n, 1), *base = pick_label(es, n, 2);
    bool any_base = false;
    for (size_t i = 0; i < n; i++) any_base |= es[i]->b.has_base;
    if (ours) sb_appendf(b, "\n- ours = %s", ours);
    if (theirs) sb_appendf(b, "\n- theirs = %s", theirs);
    if (any_base) sb_appendf(b, "\n- base = %s", base ? base : "(no label)");
}

static char *scan_cap_note(const char *display, int64_t line) {
    return xasprintf("- note: the conflict scan stopped at the byte cap at line %lld; blocks from there on are not counted or "
                     "registered here — range-read `%s:%lld-` to show and register them.",
                     (long long)line, display, (long long)line);
}

static void append_body(sb_t *b, const strvec_t *s) {
    if (!s->n) {
        sb_append(b, "\n(empty)");
        return;
    }
    size_t shown = MIN(s->n, (size_t)PREVIEW_SIDE_LINES);
    for (size_t i = 0; i < shown; i++) sb_appendf(b, "\n%s", s->v[i]);
    size_t hidden = s->n - shown;
    if (hidden) sb_appendf(b, "\n… (%zu more line%s)", hidden, hidden == 1 ? "" : "s");
}

static void range_text(sb_t *b, const cblock_t *x) {
    if (x->start_line == x->end_line) sb_appendf(b, "L%lld", (long long)x->start_line);
    else sb_appendf(b, "L%lld-%lld", (long long)x->start_line, (long long)x->end_line);
}

/* omp formatConflictWarning (starts with "\n") */
static char *format_warning(centry_t *const *es, size_t n, int64_t total, const char *display, int64_t truncated_at) {
    sb_t b = SB_INIT;
    const char *word = total == 1 ? "conflict" : "conflicts";
    if (total > (int64_t)n)
        sb_appendf(&b, "\n⚠ %zu of %lld unresolved %s visible in this window; this read registered only the blocks below, so "
                       "don't infer ids for the others — read `%s:conflicts` to list and register the file's blocks.",
                   n, (long long)total, word, display);
    else sb_appendf(&b, "\n⚠ %lld unresolved %s detected", (long long)total, word);
    if (truncated_at) {
        char *note = scan_cap_note(display, truncated_at);
        sb_appendf(&b, "\n%s", note);
        free(note);
    }
    label_lines(&b, es, n);
    sb_append(&b,
              "\nNOTICE: Inspect a block by reading `conflict://<N>` (add `/ours` / `/theirs` / `/base` to render a single "
              "side). Resolve with `write({ path: \"conflict://<N>\", content })`, or bulk-resolve every registered conflict "
              "with `write({ path: \"conflict://*\", content })`. Writes replace ONLY the marker block (markers + all sides) "
              "— never repeat the lines before/after it; they stay in place.");
    sb_append(&b,
              "\n`content` shorthand: a line that is exactly `@ours` / `@theirs` / `@base` / `@both` expands to that recorded "
              "section. `@both` is ours-then-theirs with no separator — only for additive conflicts where each side adds "
              "something different; NEVER for competing edits of the same lines (pick a side or write the combined text). "
              "Lines that are not a token pass through verbatim, so `\"// keep both\\n@ours\\n@theirs\"` literally writes the "
              "comment, then ours, then theirs.");
    sb_append(&b,
              "\nPer-id bulk: `write({ path: \"conflict://*\", content: \"1: @ours\\n2: @theirs\\n…\" })` resolves each listed id "
              "with that side in ONE call — the cheapest way through many pick-one conflicts; unlisted ids stay registered.");
    sb_append(&b,
              "\nResolve each block faithfully: keep one side (`@ours`/`@theirs`), or combine them when both intents apply — "
              "never invent content beyond the recorded sides, and never stack both sides of competing edits. Resolve "
              "several conflicts in a single turn by issuing multiple `write` calls at once; ids stay valid as earlier blocks "
              "are resolved.");
    for (size_t i = 0; i < n; i++) {
        const cblock_t *x = &es[i]->b;
        sb_appendf(&b, "\n\n──── #%d  ", es[i]->id);
        range_text(&b, x);
        sb_append(&b, " ────");
        sb_append(&b, "\n<<< ours");
        append_body(&b, &x->ours);
        if (x->has_base) {
            if (sections_equal(&x->base, &x->ours)) sb_append(&b, "\n=== base ≡ ours");
            else if (sections_equal(&x->base, &x->theirs)) sb_append(&b, "\n=== base ≡ theirs");
            else {
                sb_append(&b, "\n=== base");
                append_body(&b, &x->base);
            }
        }
        if (sections_equal(&x->theirs, &x->ours)) sb_append(&b, "\n>>> theirs ≡ ours");
        else {
            sb_append(&b, "\n>>> theirs");
            append_body(&b, &x->theirs);
        }
    }
    return sb_detach(&b);
}

/* omp formatConflictSummary */
static char *format_summary(centry_t *const *es, size_t n, const char *display, int64_t truncated_at) {
    sb_t b = SB_INIT;
    const char *disp = display && *display ? display : "<file>";
    sb_appendf(&b, "⚠ %zu unresolved %s in %s", n, n == 1 ? "conflict" : "conflicts", disp);
    if (truncated_at) {
        char *note = scan_cap_note(disp, truncated_at);
        sb_appendf(&b, "\n%s", note);
        free(note);
    }
    label_lines(&b, es, n);
    sb_append(&b,
              "\nNOTICE: Bulk-resolve with `write({ path: \"conflict://*\", content })`, or address a single block with "
              "`write({ path: \"conflict://<N>\", content })`. Inspect a block by reading `conflict://<N>` (add `/ours` / "
              "`/theirs` / `/base` for a single side).");
    sb_append(&b,
              "\n`content` shorthand: `@ours` / `@theirs` / `@base` / `@both` lines expand to the recorded sections; `@both` = "
              "ours-then-theirs (additive conflicts only — never for competing edits of the same lines). Per-id bulk: content "
              "of `<id>: @side` lines (e.g. \"1: @ours\\n2: @theirs\") resolves each listed id in one call. Non-token lines "
              "pass through verbatim. Writes replace ONLY the marker block — never repeat the surrounding lines. Keep one "
              "side or combine faithfully; never invent content beyond the recorded sides.");
    sb_append(&b, "\n");
    char w[24];
    int idw = snprintf(w, sizeof w, "%d", n ? es[n - 1]->id : 1);
    for (size_t i = 0; i < n; i++) {
        sb_appendf(&b, "\n#%*d  ", idw, es[i]->id);
        range_text(&b, &es[i]->b);
        if (es[i]->b.has_base) sb_append(&b, "  (3-way)");
    }
    return sb_detach(&b);
}

/* ---------- read integration ---------- */

static char *rel_display(const tool_env_t *env, const char *abs) {
    size_t cl = strlen(env->cwd);
    if (!strcmp(abs, env->cwd)) return xstrdup(".");
    if (!strncmp(abs, env->cwd, cl) && abs[cl] == '/') return xstrdup(abs + cl + 1);
    return xstrdup(abs);
}


char *conflict_on_read(tool_env_t *env, const char *abs, const char *display, const strvec_t *lines, int64_t first_line,
                       int *count) {
    *count = 0;
    cblocks_t blocks = {0};
    scan_lines(lines, first_line, &blocks);
    if (!blocks.n) {
        vec_free(&blocks);
        return NULL;
    }
    char *own_display = display ? NULL : rel_display(env, abs);
    if (own_display) display = own_display;
    conflict_history_t *h = history(env);
    pthread_mutex_lock(&env->mu);
    centry_t **es = xcalloc(blocks.n, sizeof *es);
    for (size_t i = 0; i < blocks.n; i++) es[i] = history_register(h, abs, display, &blocks.v[i]);
    /* best-effort whole-file count, only once the window showed a conflict */
    int64_t total = (int64_t)blocks.n, truncated_at = 0;
    cblocks_t all = {0};
    if (scan_file(abs, &all, &truncated_at)) total = MAX(total, (int64_t)all.n);
    cblocks_free(&all);
    char *w = format_warning(es, blocks.n, total, display, truncated_at);
    pthread_mutex_unlock(&env->mu);
    *count = (int)blocks.n;
    free(es);
    cblocks_free(&blocks);
    free(own_display);
    return w;
}

char *conflict_read_summary(tool_env_t *env, const char *abs, const char *display, int *count) {
    *count = 0;
    cblocks_t blocks = {0};
    int64_t truncated_at = 0;
    char *own_display = display ? NULL : rel_display(env, abs);
    if (own_display) display = own_display;
    scan_file(abs, &blocks, &truncated_at);
    conflict_history_t *h = history(env);
    pthread_mutex_lock(&env->mu);
    reconcile_path(h, abs, &blocks, truncated_at);
    centry_t **es = xcalloc(blocks.n + 1, sizeof *es);
    for (size_t i = 0; i < blocks.n; i++) es[i] = history_register(h, abs, display, &blocks.v[i]);
    char *s;
    if (blocks.n) {
        s = format_summary(es, blocks.n, display, truncated_at);
    } else if (truncated_at) {
        char *note = scan_cap_note(display, truncated_at);
        s = xasprintf("No unresolved git merge conflicts before line %lld of %s.\n%s", (long long)truncated_at, display, note);
        free(note);
    } else {
        s = xasprintf("No unresolved git merge conflicts in %s.", display);
    }
    pthread_mutex_unlock(&env->mu);
    *count = (int)blocks.n;
    free(es);
    cblocks_free(&blocks);
    free(own_display);
    return s;
}

char *conflict_recover_prefix(const char *raw, char **note) {
    *note = NULL;
    /* greedy prefix: the LAST `:conflict://` wins */
    const char *hit = NULL;
    for (const char *p = raw; (p = strstr(p, ":conflict://")); p++) hit = p;
    if (!hit || hit == raw || !hit[12]) return NULL;
    char *prefix = xstrndup(raw, (size_t)(hit - raw));
    const char *url = hit + 1;
    *note = xasprintf("Note: stripped erroneous '%s:' prefix from path; conflict URIs are global (use `%s`, not `<file>:%s`).",
                      prefix, url, url);
    free(prefix);
    return xstrdup(url);
}

/* ---------- conflict:// URIs ---------- */

typedef struct {
    int id;           /* 0 = wildcard `*` */
    const char *scope; /* "ours" | "theirs" | "base" | NULL */
} curi_t;

/* omp parseConflictUri; false with *err (owned) */
static bool parse_uri(const char *raw, curi_t *u, char **err) {
    *err = NULL;
    u->id = 0;
    u->scope = NULL;
    if (strncmp(raw, "conflict://", 11) != 0 || !raw[11]) {
        *err = xasprintf("Invalid conflict URI '%s': must be 'conflict://<N>', 'conflict://<N>/<scope>', or 'conflict://*'.", raw);
        return false;
    }
    const char *tail = raw + 11, *slash = strchr(tail, '/');
    char *idp = slash ? xstrndup(tail, (size_t)(slash - tail)) : xstrdup(tail);
    const char *scope = slash ? slash + 1 : NULL;
    bool ok = false;
    if (!strcmp(idp, "*")) {
        if (scope) {
            *err = xasprintf("Invalid conflict URI '%s': wildcard 'conflict://*' does not accept a scope segment. Drop '/%s' or "
                             "use a numeric id.",
                             raw, scope);
        } else ok = true;
        free(idp);
        return ok;
    }
    bool digits = *idp != 0;
    for (const char *p = idp; *p; p++) digits &= isdigit((unsigned char)*p) != 0;
    if (!digits) {
        *err = xasprintf("Invalid conflict URI '%s': must be 'conflict://<N>', 'conflict://<N>/<scope>', or 'conflict://*' "
                         "where N is a positive integer surfaced by a prior `read`.",
                         raw);
        free(idp);
        return false;
    }
    long long id = strtoll(idp, NULL, 10);
    free(idp);
    if (id < 1 || id > INT32_MAX) {
        *err = xasprintf("Invalid conflict URI '%s': id must be ≥ 1.", raw);
        return false;
    }
    u->id = (int)id;
    if (scope) {
        if (strcmp(scope, "ours") && strcmp(scope, "theirs") && strcmp(scope, "base")) {
            *err = xasprintf("Invalid conflict URI '%s': scope must be one of 'ours', 'theirs', 'base', or omitted (e.g. "
                             "'conflict://%d/theirs').",
                             raw, u->id);
            return false;
        }
        u->scope = scope;
    }
    return true;
}


/* omp requireEntry (caller holds env->mu); NULL with *err (owned) */
static centry_t *require_entry(tool_env_t *env, conflict_history_t *h, int id, char **err) {
    centry_t *reg = find_id(h, id);
    if (reg) {
        char *a = xstrdup(reg->abs);
        reconcile_registered(h, a);
        free(a);
    }
    centry_t *e = find_id(h, id);
    if (e) return e;
    strvec_t paths = {0};
    for (size_t i = 0; i < h->entries.n; i++)
        if (!strvec_contains(&paths, h->entries.v[i]->abs)) strvec_push(&paths, h->entries.v[i]->abs);
    sb_t known = SB_INIT;
    if (paths.n) {
        sb_append(&known, "Registered: ");
        for (size_t f = 0; f < paths.n && f < COLLAPSED_ITEMS; f++) {
            if (f) sb_append(&known, "; ");
            size_t cnt = 0, shown = 0;
            for (size_t i = 0; i < h->entries.n; i++) {
                if (strcmp(h->entries.v[i]->abs, paths.v[f])) continue;
                cnt++;
                if (shown < COLLAPSED_ITEMS) sb_appendf(&known, "%s#%d", shown++ ? ", " : "", h->entries.v[i]->id);
            }
            if (cnt > COLLAPSED_ITEMS) sb_appendf(&known, " (+%zu more)", cnt - COLLAPSED_ITEMS);
            char *d = rel_display(env, paths.v[f]);
            sb_appendf(&known, " in %s", d);
            free(d);
        }
        if (paths.n > COLLAPSED_ITEMS) sb_appendf(&known, " (+%zu more files)", paths.n - COLLAPSED_ITEMS);
        sb_append(&known, ".");
    } else {
        sb_append(&known, "No conflicts are registered.");
    }
    *err = xasprintf("Conflict #%d not found: it was resolved, became stale, or no `read` has shown its block yet (a ranged "
                     "read registers only the blocks it shows, so unseen blocks' ids can't be guessed). Read "
                     "`<path>:conflicts` on the file to list and register its blocks. %s",
                     id, sb_cstr(&known));
    sb_free(&known);
    strvec_free(&paths);
    return NULL;
}

/* trailing `:N`, `:N-M`, `:N+K`, `:raw` selector of a conflict:// read */
static char *split_selector(const char *path, int64_t *lo, int64_t *hi, bool *raw) {
    *lo = *hi = 0;
    *raw = false;
    char *p = xstrdup(path);
    for (int pass = 0; pass < 2; pass++) {
        char *c = strrchr(p + 11, ':');
        if (!c) break;
        if (!strcmp(c + 1, "raw")) {
            *raw = true;
            *c = 0;
            continue;
        }
        char *end;
        long long a = strtoll(c + 1, &end, 10), b = a;
        if (end == c + 1 || a < 1) break;
        if (*end == '-') b = end[1] ? strtoll(end + 1, &end, 10) : 0;
        else if (*end == '+') b = a + strtoll(end + 1, &end, 10) - 1;
        if (*end) break;
        *lo = a;
        *hi = b;
        *c = 0;
    }
    return p;
}

static int conflict_read(tool_env_t *env, const char *path, tool_result_t *out) {
    int64_t lo, hi;
    bool raw;
    char *url = split_selector(path, &lo, &hi, &raw);
    curi_t u;
    char *err = NULL;
    if (!parse_uri(url, &u, &err)) {
        tool_result_error(out, "%s", err);
        free(err);
        free(url);
        return 0;
    }
    char scope_buf[8] = "";
    if (u.scope) snprintf(scope_buf, sizeof scope_buf, "%s", u.scope);
    u.scope = u.scope ? scope_buf : NULL;
    free(url);
    if (!u.id) {
        tool_result_error(out, "Reading `conflict://*` is not supported — wildcards are write-only. Use the `<path>:conflicts` "
                               "read selector for the full list of conflicts in a file, or read `conflict://<N>` to inspect "
                               "a single block.");
        return 0;
    }
    conflict_history_t *h = history(env);
    pthread_mutex_lock(&env->mu);
    centry_t *e = require_entry(env, h, u.id, &err);
    if (!e) {
        pthread_mutex_unlock(&env->mu);
        tool_result_error(out, "%s", err);
        free(err);
        return 0;
    }
    /* renderConflictRegion */
    strvec_t lines = {0};
    int64_t start;
    const cblock_t *b = &e->b;
    if (u.scope && !strcmp(u.scope, "ours")) {
        strvec_copy(&lines, &b->ours);
        start = b->start_line + 1;
    } else if (u.scope && !strcmp(u.scope, "theirs")) {
        strvec_copy(&lines, &b->theirs);
        start = b->separator_line + 1;
    } else if (u.scope) {
        if (!b->has_base) {
            tool_result_error(out, "Conflict #%d has no base section (2-way merge). 'conflict://%d/base' is only valid for diff3 "
                                   "conflicts.",
                              e->id, e->id);
            pthread_mutex_unlock(&env->mu);
            return 0;
        }
        strvec_copy(&lines, &b->base);
        start = b->base_line + 1;
    } else {
        lines = recorded_region(b);
        start = b->start_line;
    }
    char *abs = xstrdup(e->abs), *display = xstrdup(e->display);
    pthread_mutex_unlock(&env->mu);
    int64_t total = (int64_t)lines.n;
    int64_t from = lo ? lo : 1, to = hi ? MIN(hi, total) : total;
    sb_t body = SB_INIT;
    bool hashline = !raw && str_eq(env->edit_mode, "hashline");
    bool numbered = hashline || (!raw && settings_bool("readLineNumbers", false));
    if (from > total) {
        sb_appendf(&body, "Line %lld is beyond end of resource (%lld lines total). %s", (long long)from, (long long)total,
                   total ? "Use :1 to read from the start." : "The resource is empty.");
    } else {
        if (hashline) {
            char *text = fs_read(abs, NULL);
            if (text) {
                char *lf = fz_normalize_to_lf(text);
                const char *tag = edit_store_record(&env->store, abs, lf, start + from - 1, start + to - 1);
                format_hashline_header(&body, display, tag);
                free(lf);
                free(text);
            }
        }
        for (int64_t i = from; i <= to; i++) {
            if (numbered) sb_appendf(&body, "%lld:", (long long)(start + i - 1));
            sb_append(&body, lines.v[i - 1]);
            if (i < to) sb_putc(&body, '\n');
        }
    }
    tool_result_text(out, sb_cstr(&body));
    out->details = json_obj();
    json_set_str(out->details, "resolvedPath", abs);
    json_set_str(out->details, "contentType", "text/plain");
    json_set_int(out->details, "totalLines", total);
    if (raw) json_set_bool(out->details, "raw", true);
    sb_free(&body);
    strvec_free(&lines);
    free(abs);
    free(display);
    return 0;
}

/* ---------- splicing ---------- */

static bool matches_at(const strvec_t *lines, size_t at, const strvec_t *exp) {
    if (at + exp->n > lines->n) return false;
    for (size_t i = 0; i < exp->n; i++) {
        char *l = strip_cr(lines->v[at + i]);
        bool eq = !strcmp(l, exp->v[i]);
        free(l);
        if (!eq) return false;
    }
    return true;
}

static bool locate_region(const strvec_t *lines, const strvec_t *exp, int64_t preferred, size_t *s, size_t *e) {
    if (!exp->n || exp->n > lines->n) return false;
    if (preferred >= 0 && matches_at(lines, (size_t)preferred, exp)) {
        *s = (size_t)preferred;
        *e = *s + exp->n - 1;
        return true;
    }
    int64_t best = -1, bestd = INT64_MAX;
    for (size_t i = 0; i + exp->n <= lines->n; i++) {
        if (!matches_at(lines, i, exp)) continue;
        int64_t d = llabs((int64_t)i - preferred);
        if (d < bestd) {
            best = (int64_t)i;
            bestd = d;
        }
    }
    if (best < 0) return false;
    *s = (size_t)best;
    *e = *s + exp->n - 1;
    return true;
}

static int delim_balance(const strvec_t *l, size_t from, size_t to) {
    int bal = 0;
    for (size_t i = from; i < to; i++)
        for (const char *p = l->v[i]; *p; p++) {
            if (*p == '{' || *p == '(' || *p == '[') bal++;
            else if (*p == '}' || *p == ')' || *p == ']') bal--;
        }
    return bal;
}

/* omp trimBoundaryEcho over rep[lo, hi) (in place by adjusting lo/hi) */
static void trim_echo(const strvec_t *rep, size_t *lo, size_t *hi, const strvec_t *file, size_t ms, size_t me,
                      const cblock_t *b, size_t typed_leading, size_t typed_trailing, int *leading, int *trailing) {
    *leading = *trailing = 0;
    int ob = delim_balance(&b->ours, 0, b->ours.n);
    bool have_exp = ob == delim_balance(&b->theirs, 0, b->theirs.n);
#define JUSTIFIED(a, z, wa, wz) \
    (have_exp && delim_balance(rep, a, z) != ob && delim_balance(rep, wa, wz) == ob)
    strvec_t after = {0}, before = {0};
    for (size_t i = me + 1; i < file->n && after.n < MAX_ECHO_LINES; i++) strvec_push_own(&after, strip_cr(file->v[i]));
    size_t len = *hi - *lo;
    for (size_t k = MIN(MIN(after.n, len ? len - 1 : 0), typed_trailing); k >= 1; k--) {
        bool eq = true;
        for (size_t i = 0; i < k && eq; i++) eq = !strcmp(rep->v[*hi - k + i], after.v[i]);
        if (!eq) continue;
        if (k >= 2 || JUSTIFIED(*lo, *hi, *lo, *hi - 1)) {
            *trailing = (int)k;
            *hi -= k;
        }
        break;
    }
    for (size_t i = ms; i > 0 && before.n < MAX_ECHO_LINES; i--) strvec_push_own(&before, strip_cr(file->v[i - 1]));
    /* before is reversed: before.v[0] is the line directly above the region */
    len = *hi - *lo;
    for (size_t k = MIN(MIN(before.n, len ? len - 1 : 0), typed_leading); k >= 1; k--) {
        bool eq = true;
        for (size_t i = 0; i < k && eq; i++) eq = !strcmp(rep->v[*lo + i], before.v[k - 1 - i]);
        if (!eq) continue;
        if (k >= 2 || JUSTIFIED(*lo, *hi, *lo + 1, *hi)) {
            *leading = (int)k;
            *lo += k;
        }
        break;
    }
#undef JUSTIFIED
    strvec_free(&after);
    strvec_free(&before);
}

/* omp ConflictReplacement: the lines replacing a region; only the lines the caller typed before the first and after
 * the last token-expanded line may be dropped as a boundary echo */
typedef struct {
    strvec_t lines;
    size_t typed_leading, typed_trailing;
} crep_t;

/* omp spliceConflict: new text (owned) or NULL with *err. Empty `lines` remove the region and leave no line behind. */
static char *splice(const char *original, const centry_t *e, const crep_t *r, int *echo, char **err) {
    strvec_t lines = str_split(original, "\n");
    strvec_t exp = recorded_region(&e->b);
    size_t ms, me;
    if (!locate_region(&lines, &exp, e->b.start_line - 1, &ms, &me)) {
        *err = xasprintf("Conflict #%d no longer present in '%s': the recorded marker block can't be located. The file changed "
                         "since the conflict was registered — re-read it to re-register conflicts.",
                         e->id, e->display);
        strvec_free(&lines);
        strvec_free(&exp);
        return NULL;
    }
    const strvec_t *rep = &r->lines;
    size_t lo = 0, hi = rep->n;
    int lead, trail;
    trim_echo(rep, &lo, &hi, &lines, ms, me, &e->b, r->typed_leading, r->typed_trailing, &lead, &trail);
    *echo += lead + trail;
    size_t ll = strlen(lines.v[ms]);
    bool crlf = ll && lines.v[ms][ll - 1] == '\r';
    bool following = me + 1 < lines.n;
    /* removing a region that ends the file without a trailing newline makes the line above it the last line; it must
     * not keep the \r of its old CRLF */
    if (lo == hi && !following && ms > 0) {
        char *t = strip_cr(lines.v[ms - 1]);
        free(lines.v[ms - 1]);
        lines.v[ms - 1] = t;
    }
    sb_t b = SB_INIT;
    size_t parts = 0;
    for (size_t i = 0; i < ms; i++, parts++) sb_appendf(&b, "%s%s", parts ? "\n" : "", lines.v[i]);
    for (size_t i = lo; i < hi; i++, parts++) {
        bool cr = crlf && (i + 1 < hi || following);
        sb_appendf(&b, "%s%s%s", parts ? "\n" : "", rep->v[i], cr ? "\r" : "");
    }
    for (size_t i = me + 1; i < lines.n; i++, parts++) sb_appendf(&b, "%s%s", parts ? "\n" : "", lines.v[i]);
    strvec_free(&lines);
    strvec_free(&exp);
    return sb_detach(&b);
}

/* omp expandContentTokens: write content as replacement lines (one trailing newline ends the last line), `@ours` /
 * `@theirs` / `@base` / `@both` lines expanded to the recorded sections (an empty section adds no line, while a typed
 * empty line, empty content included, stays one ""); false with *err for `@base` on a 2-way block */
static bool expand_tokens(const char *content, const centry_t *e, crep_t *out, char **err) {
    memset(out, 0, sizeof *out);
    size_t cl = strlen(content);
    char *norm = cl >= 2 && !strcmp(content + cl - 2, "\r\n") ? xstrndup(content, cl - 2)
                 : cl && content[cl - 1] == '\n'                ? xstrndup(content, cl - 1)
                                                                 : xstrdup(content);
    strvec_t in = str_split(norm, "\n");
    free(norm);
    ssize_t first = -1;
    size_t last_end = 0;
#define SECTION(sv)                                                     \
    do {                                                                \
        if ((sv).n) {                                                   \
            if (first < 0) first = (ssize_t)out->lines.n;               \
            for (size_t k = 0; k < (sv).n; k++) strvec_push(&out->lines, (sv).v[k]); \
            last_end = out->lines.n;                                    \
        }                                                               \
    } while (0)
    for (size_t i = 0; i < in.n; i++) {
        char *line = strip_cr(in.v[i]);
        if (!strcmp(line, "@ours")) {
            SECTION(e->b.ours);
        } else if (!strcmp(line, "@theirs")) {
            SECTION(e->b.theirs);
        } else if (!strcmp(line, "@base")) {
            if (!e->b.has_base) {
                *err = xasprintf("Conflict #%d has no base section (2-way merge). `@base` is only valid for diff3 conflicts.",
                                 e->id);
                free(line);
                strvec_free(&in);
                strvec_free(&out->lines);
                return false;
            }
            SECTION(e->b.base);
        } else if (!strcmp(line, "@both")) {
            SECTION(e->b.ours);
            SECTION(e->b.theirs);
        } else {
            strvec_push(&out->lines, line);
        }
        free(line);
    }
#undef SECTION
    strvec_free(&in);
    out->typed_leading = first < 0 ? out->lines.n : (size_t)first;
    out->typed_trailing = out->lines.n - last_end;
    return true;
}

static const char *const STRIPPED_NOTE = "Note: auto-stripped hashline display prefixes from content before writing.";

static char *snapshot_header(tool_env_t *env, const char *abs, const char *display, const char *text) {
    if (!str_eq(env->edit_mode, "hashline")) return NULL;
    char *lf = fz_normalize_to_lf(text);
    const char *tag = edit_store_record(&env->store, abs, lf, 0, 0);
    free(lf);
    return xasprintf("[%s#%s]", display, tag);
}

static bool write_file(const char *abs, const char *text) {
    struct stat st;
    mode_t mode = stat(abs, &st) == 0 ? (st.st_mode & 07777) : 0644;
    return fs_write_atomic(abs, text, strlen(text), mode) == 0;
}

static void resolve_one(tool_env_t *env, conflict_history_t *h, centry_t *e, const char *content, bool stripped,
                        tool_result_t *out) {
    if (!fs_exists(e->abs)) {
        tool_result_error(out, "Conflict #%d target '%s' no longer exists.", e->id, e->display);
        return;
    }
    char *err = NULL;
    crep_t exp;
    if (!expand_tokens(content, e, &exp, &err)) {
        tool_result_error(out, "%s", err);
        free(err);
        return;
    }
    /* decode verbatim and keep a leading BOM across the write (omp fca084bcdd) */
    char *orig = fs_read(e->abs, NULL);
    int echo = 0;
    bool bom = orig && after_bom(orig) != orig;
    char *spliced = orig ? splice(after_bom(orig), e, &exp, &echo, &err) : NULL;
    char *next = spliced && bom ? xasprintf("\xEF\xBB\xBF%s", spliced) : spliced;
    if (next != spliced) free(spliced);
    free(orig);
    strvec_free(&exp.lines);
    if (!next) {
        tool_result_error(out, "%s", err ? err : "cannot read file");
        free(err);
        return;
    }
    if (!write_file(e->abs, next)) {
        tool_result_error(out, "Cannot write '%s'", e->display);
        free(next);
        return;
    }
    char *abs = xstrdup(e->abs), *display = xstrdup(e->display);
    cblock_t rec;
    cblock_copy(&rec, &e->b);
    int id = e->id;
    invalidate_id(h, id);
    strvec_t nl = str_split(after_bom(next), "\n");
    cblocks_t scan = {0};
    scan_lines(&nl, 1, &scan);
    reconcile_path(h, abs, &scan, 0);
    cblocks_free(&scan);
    strvec_free(&nl);
    sb_t b = SB_INIT;
    char *header = snapshot_header(env, abs, display, next);
    if (header) sb_appendf(&b, "%s\n", header);
    free(header);
    if (rec.start_line == rec.end_line) sb_appendf(&b, "Resolved conflict #%d at line %lld in %s.", id, (long long)rec.start_line, display);
    else
        sb_appendf(&b, "Resolved conflict #%d at lines %lld\xe2\x80\x93%lld in %s.", id, (long long)rec.start_line,
                   (long long)rec.end_line, display);
    if (stripped) sb_appendf(&b, "\n%s", STRIPPED_NOTE);
    if (echo > 0)
        sb_appendf(&b, "\nNote: dropped %d content line(s) that duplicated the code adjacent to the conflict region — writes "
                       "replace only the marker block; surrounding lines stay in place.",
                   echo);
    tool_result_text(out, sb_cstr(&b));
    out->details = json_obj();
    json_set_str(out->details, "resolvedPath", abs);
    sb_free(&b);
    cblock_free(&rec);
    free(abs);
    free(display);
    free(next);
}

/* `<id>: @side` (optionally behind a copied `LINE:` prefix): id, side; false otherwise */
static bool directive_line(const char *line, int *id, const char **side, bool head_only) {
    const char *p = line;
    if (isdigit((unsigned char)*p)) { /* optional `\d+:` copied line prefix, only when an id follows */
        const char *q = p;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == ':' && (q[1] == '#' || isdigit((unsigned char)q[1]))) {
            const char *r = q + 1 + (q[1] == '#');
            const char *s = r;
            while (isdigit((unsigned char)*s)) s++;
            while (*s == ' ' || *s == '\t') s++;
            if (s > r && (*s == ':' || *s == '=')) p = q + 1;
        }
    }
    if (*p == '#') p++;
    if (!isdigit((unsigned char)*p)) return false;
    long long v = strtoll(p, (char **)&p, 10);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':' && *p != '=') return false;
    p++;
    *id = v > INT32_MAX ? INT32_MAX : (int)v;
    if (head_only) return true;
    while (*p == ' ' || *p == '\t') p++;
    static const char *const sides[] = {"@ours", "@theirs", "@base", "@both"};
    for (size_t i = 0; i < 4; i++)
        if (!strcmp(p, sides[i])) {
            *side = sides[i];
            return true;
        }
    return false;
}

static char *trim_dup(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

static bool id_known(const centry_t *const *es, size_t n, int id) {
    for (size_t i = 0; i < n; i++)
        if (es[i]->id == id) return true;
    return false;
}

/* omp parseBulkDirectives: 1 = directives (json {id: side}), 0 = uniform mode, -1 = error (*err) */
static int parse_directives(const char *content, centry_t *const *es, size_t n, json_t **map, char **err) {
    *map = json_obj();
    strvec_t stray = {0}, raw = str_split(content, "\n");
    bool saw = false;
    int rc = 1;
    for (size_t i = 0; i < raw.n; i++) {
        char *line = trim_dup(raw.v[i]);
        int id;
        const char *side = NULL;
        if (!*line) {
            free(line);
            continue;
        }
        if (!directive_line(line, &id, &side, false)) {
            strvec_push_own(&stray, line);
            continue;
        }
        saw = true;
        char key[16];
        snprintf(key, sizeof key, "%d", id);
        if (json_get(*map, key)) {
            *err = xasprintf("Bulk directive lists conflict #%d twice — each id may appear once.", id);
            free(line);
            rc = -1;
            goto done;
        }
        json_set_str(*map, key, side);
        free(line);
    }
    if (!saw) {
        bool first_known = false, all_known_or_none = true;
        size_t reg_heads = 0, heads = 0;
        strvec_t uniq = {0};
        for (size_t i = 0; i < stray.n; i++) {
            int id;
            bool has = directive_line(stray.v[i], &id, NULL, true);
            if (i == 0) first_known = has && id_known((const centry_t *const *)es, n, id);
            if (!has) continue;
            heads++;
            if (id_known((const centry_t *const *)es, n, id)) {
                reg_heads++;
                char k[16];
                snprintf(k, sizeof k, "%d", id);
                if (!strvec_contains(&uniq, k)) strvec_push(&uniq, k);
            } else {
                all_known_or_none = false;
            }
        }
        /* heads.every(id => undefined || registered) && (registeredHeads.length === heads.length || unique >= 2) */
        bool literal_block = first_known && all_known_or_none && (reg_heads == stray.n || uniq.n >= 2);
        UNUSED(heads);
        strvec_free(&uniq);
        if (!literal_block) {
            rc = 0;
            goto done;
        }
    }
    if (stray.n) {
        int id;
        const char *sample = stray.v[0];
        char *trunc = strlen(sample) > 60 ? xasprintf("%.57s…", sample) : xstrdup(sample);
        *err = xasprintf("Malformed `conflict://*` per-id block: %zu line(s) are not `<id>: @side` directives (first: `%s`). "
                         "%sLiteral or multi-line replacement content isn't supported in a per-id block — resolve those "
                         "blocks with individual `write({ path: \"conflict://<N>\", content })` calls (you can issue several "
                         "at once). For a pure pick-a-side pass, make every non-empty line `<id>: @ours` (or "
                         "@theirs/@base/@both).",
                         stray.n, trunc,
                         directive_line(sample, &id, NULL, true)
                             ? "Per-id bulk only accepts the tokens @ours/@theirs/@base/@both — one side per id, single line. "
                             : "");
        free(trunc);
        rc = -1;
    }
done:
    strvec_free(&stray);
    strvec_free(&raw);
    if (rc != 1) {
        json_free(*map);
        *map = NULL;
    }
    return rc;
}

static void resolve_all(tool_env_t *env, conflict_history_t *h, const char *clean, bool stripped, const char *raw,
                        tool_result_t *out) {
    reconcile_registered(h, NULL);
    size_t n = h->entries.n;
    if (!n) {
        tool_result_error(out, "`conflict://*` has nothing to resolve — no conflicts are currently registered. Re-read the "
                               "file(s) with conflicts first.");
        return;
    }
    centry_t **es = xcalloc(n, sizeof *es);
    memcpy(es, h->entries.v, n * sizeof *es);
    json_t *dirs = NULL;
    char *err = NULL;
    /* resolveBulkDirectives: raw content first (its `<id>:` heads look like hashline prefixes) */
    int rc = parse_directives(raw, es, n, &dirs, &err);
    if (strcmp(raw, clean) != 0) {
        if (rc == -1) {
            char *e2 = NULL;
            json_t *fb = NULL;
            if (parse_directives(clean, es, n, &fb, &e2) == 1) {
                free(err);
                err = NULL;
                dirs = fb;
                rc = 1;
            }
            free(e2);
        } else if (rc == 0) {
            rc = parse_directives(clean, es, n, &dirs, &err);
        }
    }
    if (rc == -1) {
        tool_result_error(out, "%s", err);
        free(err);
        free(es);
        return;
    }
    if (dirs) {
        sb_t unk = SB_INIT;
        const char *k;
        json_t *v;
        json_obj_foreach(dirs, i, k, v) {
            if (!id_known((const centry_t *const *)es, n, atoi(k))) sb_appendf(&unk, "%s#%s", unk.len ? ", " : "", k);
        }
        if (unk.len) {
            sb_t reg = SB_INIT;
            for (size_t i = 0; i < n; i++) sb_appendf(&reg, "%s#%d", i ? ", " : "", es[i]->id);
            tool_result_error(out, "Bulk directive references unknown conflict id(s) %s. Currently registered: %s.", sb_cstr(&unk),
                              sb_cstr(&reg));
            sb_free(&reg);
            sb_free(&unk);
            json_free(dirs);
            free(es);
            return;
        }
        sb_free(&unk);
    }
    /* selected entries grouped by file (first-seen file order) */
    strvec_t files = {0};
    for (size_t i = 0; i < n; i++) {
        char k[16];
        snprintf(k, sizeof k, "%d", es[i]->id);
        if (dirs && !json_get(dirs, k)) continue;
        if (!strvec_contains(&files, es[i]->abs)) strvec_push(&files, es[i]->abs);
    }
    sb_t ok = SB_INIT, bad = SB_INIT;
    strvec_t headers = {0};
    size_t ok_files = 0, bad_files = 0;
    int total_resolved = 0, echo_total = 0;
    for (size_t f = 0; f < files.n; f++) {
        VEC(centry_t *) fe = {0};
        for (size_t i = 0; i < n; i++) {
            char k[16];
            snprintf(k, sizeof k, "%d", es[i]->id);
            if (strcmp(es[i]->abs, files.v[f]) || (dirs && !json_get(dirs, k))) continue;
            vec_push(&fe, es[i]);
        }
        /* bottom-up by recorded start line so earlier anchors stay valid */
        for (size_t i = 1; i < fe.n; i++)
            for (size_t j = i; j > 0 && fe.v[j]->b.start_line > fe.v[j - 1]->b.start_line; j--) {
                centry_t *t = fe.v[j];
                fe.v[j] = fe.v[j - 1];
                fe.v[j - 1] = t;
            }
        const char *disp = fe.v[0]->display;
        size_t cnt = fe.n;
        char *failure = NULL;
        char *text = fs_exists(files.v[f]) ? fs_read(files.v[f], NULL) : NULL;
        if (!fs_exists(files.v[f])) failure = xstrdup("file no longer exists");
        else if (!text) failure = xstrdup("cannot read file");
        /* splice the BOM-stripped text; the write puts a leading BOM back (omp fca084bcdd) */
        bool bom = text && after_bom(text) != text;
        if (bom) memmove(text, text + 3, strlen(text + 3) + 1);
        int echo = 0;
        for (size_t i = 0; i < fe.n && !failure; i++) {
            char k[16];
            snprintf(k, sizeof k, "%d", fe.v[i]->id);
            const char *c = dirs ? json_get_str(dirs, k) : clean;
            char *e2 = NULL;
            crep_t exp;
            bool expanded = expand_tokens(c, fe.v[i], &exp, &e2);
            char *next = expanded ? splice(text, fe.v[i], &exp, &echo, &e2) : NULL;
            if (expanded) strvec_free(&exp.lines);
            if (!next) {
                failure = e2;
                break;
            }
            free(text);
            text = next;
        }
        if (failure) {
            sb_appendf(&bad, "\n  %s: %zu %s (%s)", disp, cnt, cnt == 1 ? "conflict" : "conflicts", failure);
            bad_files++;
            free(failure);
            free(text);
            vec_free(&fe);
            continue;
        }
        bool wrote;
        if (bom) {
            char *with = xasprintf("\xEF\xBB\xBF%s", text);
            wrote = write_file(files.v[f], with);
            free(with);
        } else {
            wrote = write_file(files.v[f], text);
        }
        if (!wrote) {
            sb_appendf(&bad, "\n  %s: %zu %s (cannot write file)", disp, cnt, cnt == 1 ? "conflict" : "conflicts");
            bad_files++;
            free(text);
            vec_free(&fe);
            continue;
        }
        echo_total += echo;
        char *display = xstrdup(disp), *abs = xstrdup(files.v[f]);
        for (size_t i = 0; i < fe.n; i++) invalidate_id(h, fe.v[i]->id);
        strvec_t nl = str_split(text, "\n");
        cblocks_t scan = {0};
        scan_lines(&nl, 1, &scan);
        reconcile_path(h, abs, &scan, 0);
        cblocks_free(&scan);
        strvec_free(&nl);
        char *header = snapshot_header(env, abs, display, text);
        if (header) strvec_push_own(&headers, header);
        sb_appendf(&ok, "\n  %s: %zu %s", display, cnt, cnt == 1 ? "conflict" : "conflicts");
        ok_files++;
        total_resolved += (int)cnt;
        free(display);
        free(abs);
        free(text);
        vec_free(&fe);
    }
    sb_t s = SB_INIT;
    if (ok_files)
        sb_appendf(&s, "Resolved %d %s across %zu %s:%s", total_resolved, total_resolved == 1 ? "conflict" : "conflicts", ok_files,
                   ok_files == 1 ? "file" : "files", sb_cstr(&ok));
    if (dirs) {
        sb_t rem = SB_INIT;
        size_t rn = 0;
        for (size_t i = 0; i < h->entries.n; i++) {
            char k[16];
            snprintf(k, sizeof k, "%d", h->entries.v[i]->id);
            if (json_get(dirs, k)) continue;
            sb_appendf(&rem, "%s#%d", rn++ ? ", " : "", h->entries.v[i]->id);
        }
        if (rn)
            sb_appendf(&s, "%sDirective mode: %zu unlisted %s still registered (%s).", s.len ? "\n" : "", rn,
                       rn == 1 ? "conflict" : "conflicts", sb_cstr(&rem));
        sb_free(&rem);
    }
    if (echo_total > 0)
        sb_appendf(&s, "%sNote: dropped %d content line(s) that duplicated code adjacent to conflict regions — writes replace "
                       "only the marker block; surrounding lines stay in place.",
                   s.len ? "\n" : "", echo_total);
    if (bad_files)
        sb_appendf(&s, "%sFailed to resolve %zu %s — registered entries left intact for retry:%s", s.len ? "\n" : "", bad_files,
                   bad_files == 1 ? "file" : "files", sb_cstr(&bad));
    if (headers.n) {
        sb_appendf(&s, "%sSnapshots:", s.len ? "\n" : "");
        for (size_t i = 0; i < headers.n; i++) sb_appendf(&s, "\n  %s", headers.v[i]);
    }
    if (stripped && !dirs) sb_appendf(&s, "%s%s", s.len ? "\n" : "", STRIPPED_NOTE);
    if (bad_files && !ok_files) {
        tool_result_error(out, "%s", sb_cstr(&s));
    } else {
        tool_result_text(out, sb_cstr(&s));
        out->details = json_obj();
        if (bad_files) out->is_error = true;
    }
    sb_free(&s);
    sb_free(&ok);
    sb_free(&bad);
    strvec_free(&headers);
    strvec_free(&files);
    json_free(dirs);
    free(es);
}

static int conflict_write(tool_env_t *env, const char *path, const char *content, tool_result_t *out) {
    curi_t u;
    char *err = NULL;
    if (!parse_uri(path, &u, &err)) {
        tool_result_error(out, "%s", err);
        free(err);
        return 0;
    }
    if (u.scope) {
        tool_result_error(out,
                          "Conflict URI scope '/%s' is read-only — read `conflict://%d/%s` to inspect that side. To write, drop "
                          "the scope (`conflict://%d`) and put the chosen content (or shorthand like `@%s`) in `content`.",
                          u.scope, u.id, u.scope, u.id, u.scope);
        return 0;
    }
    const char *raw = content ? content : "";
    bool stripped = false;
    char *clean = str_eq(env->edit_mode, "hashline") ? hl_strip_write_content(raw, &stripped) : xstrdup(raw);
    conflict_history_t *h = history(env);
    pthread_mutex_lock(&env->mu);
    if (!u.id) {
        resolve_all(env, h, clean, stripped, raw, out);
    } else {
        centry_t *e = require_entry(env, h, u.id, &err);
        if (!e) {
            tool_result_error(out, "%s", err);
            free(err);
        } else {
            resolve_one(env, h, e, clean, stripped, out);
        }
    }
    pthread_mutex_unlock(&env->mu);
    free(clean);
    return 0;
}

/* ---------- router entry points (out:// and cfg:// live in out_url.c / cfg_url.c) ---------- */

int out_write(tool_env_t *env, const char *content, tool_result_t *out);
int out_read(tool_result_t *out);
int cfg_write(tool_env_t *env, const char *path, const char *content, tool_result_t *out);

static const char *scheme_of(const char *p, size_t *len) {
    size_t i = 0;
    while (isalnum((unsigned char)p[i]) || p[i] == '+' || p[i] == '.' || p[i] == '-') i++;
    if (!i || p[i] != ':' || p[i + 1] != '/') return NULL;
    *len = i;
    return p;
}

static bool scheme_is(const char *path, const char *want) {
    size_t n;
    return scheme_of(path, &n) && n == strlen(want) && !strncasecmp(path, want, n);
}

bool iurl_read(tool_env_t *env, const char *path, tool_result_t *out) {
    if (!path) return false;
    if (scheme_is(path, "out")) return out_read(out), true;
    /* read drops a `<file>:` prefix glued to a conflict URI silently (only write reports it) */
    char *note = NULL, *rec = conflict_recover_prefix(path, &note);
    const char *p = rec ? rec : path;
    bool hit = !strncmp(p, "conflict://", 11);
    if (hit) conflict_read(env, p, out);
    free(rec);
    free(note);
    return hit;
}

bool iurl_write(tool_env_t *env, const char *path, const char *content, tool_result_t *out) {
    if (!path) return false;
    if (scheme_is(path, "out")) return out_write(env, content, out), true;
    if (scheme_is(path, "cfg")) return cfg_write(env, path, content, out), true;
    char *note = NULL, *rec = conflict_recover_prefix(path, &note);
    const char *p = rec ? rec : path;
    bool hit = !strncmp(p, "conflict://", 11);
    if (hit) {
        conflict_write(env, p, content, out);
        if (note && !out->is_error) { /* appendNoteToResult: only a handler result carries it */
            json_t *first = json_at(out->content, 0);
            const char *t = json_get_str(first, "text");
            if (first && t) {
                char *joined = *t ? xasprintf("%s\n%s", t, note) : xstrdup(note);
                json_set_str(first, "text", joined);
                free(joined);
            } else {
                tool_result_text(out, note);
            }
        }
    }
    free(rec);
    free(note);
    return hit;
}
