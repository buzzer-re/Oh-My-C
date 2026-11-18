/* readsel.c - the read tool's path / selector split (port of omp packages/tui/src/tools/read.ts splitPathAndSel and
 * foldListColons), shared by the tools (grep, glob, read) and the read cards. Pure string code. */
#include "tools/readsel.h"

#include <ctype.h>
#include <strings.h>

/* one chunk of read's RANGE_LIST_SRC: `-N` or `L?N((..|-|+)L?N?)?` ending in a digit, '.' or '-' */
static bool sel_chunk_ok(const char *s, size_t n) {
    if (!n) return false;
    size_t i = 0;
    if (s[0] == '-') {
        for (i = 1; i < n; i++)
            if (!isdigit((unsigned char)s[i])) return false;
        return n > 1;
    }
    if (s[i] == 'L' || s[i] == 'l') i++;
    size_t d = i;
    while (i < n && isdigit((unsigned char)s[i])) i++;
    if (i == d) return false;
    if (i == n) return true;
    if (s[i] == '.' && i + 1 < n && s[i + 1] == '.') i += 2;
    else if (s[i] == '-' || s[i] == '+') i++;
    else return false;
    if (i < n && (s[i] == 'L' || s[i] == 'l')) i++;
    while (i < n && isdigit((unsigned char)s[i])) i++;
    if (i != n) return false;
    char c = s[n - 1];
    return isdigit((unsigned char)c) || c == '.' || c == '-';
}

static bool sel_list_ok(const char *s, size_t n) {
    size_t st = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == ',') {
            if (!sel_chunk_ok(s + st, i - st)) return false;
            st = i + 1;
        }
    }
    return true;
}

static bool word_ieq(const char *s, size_t n, const char *w) { return strlen(w) == n && !strncasecmp(s, w, n); }

static bool bare_line(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

bool read_fold_list_colons(char **basep, char **listp, size_t min_colon) {
    /* comma chunks, last first: the list's first chunk is always the vector's last */
    strvec_t chunks = {0};
    {
        strvec_t parts = str_split(*listp, ",");
        for (size_t i = parts.n; i-- > 0;) strvec_push(&chunks, parts.v[i]);
        strvec_free(&parts);
    }
    char *base = xstrdup(*basep);
    bool dashed = false;
    for (;;) {
        char *colon = strrchr(base, ':');
        if (!colon || (size_t)(colon - base) < min_colon) break;
        const char *head = colon + 1;
        size_t hl = strlen(head);
        if (hl && head[hl - 1] == ',') { /* the selector colon repeated after a comma: `a:1-30,:-30` */
            if (!sel_list_ok(head, hl - 1)) break;
            char *rep = xstrndup(head, hl - 1);
            strvec_t parts = str_split(rep, ",");
            for (size_t i = parts.n; i-- > 0;) strvec_push(&chunks, parts.v[i]);
            strvec_free(&parts);
            free(rep);
        } else { /* a colon written as the range dash: `a:20:40,85:100` */
            const char *lc = strrchr(head, ',');
            const char *start = lc ? lc + 1 : head;
            const char *end = chunks.v[chunks.n - 1];
            if (!bare_line(start, strlen(start)) || !bare_line(end, strlen(end))) break;
            size_t before = lc ? (size_t)(lc - head) : 0;
            if (lc && !sel_list_ok(head, before)) break;
            char *joined = xasprintf("%s-%s", start, end);
            free(chunks.v[chunks.n - 1]);
            chunks.v[chunks.n - 1] = joined;
            if (lc) {
                char *pre = xstrndup(head, before);
                strvec_t parts = str_split(pre, ",");
                for (size_t i = parts.n; i-- > 0;) strvec_push(&chunks, parts.v[i]);
                strvec_free(&parts);
                free(pre);
            }
            dashed = true;
        }
        *colon = 0;
    }
    /* the dash shape folds only into a list with a comma: `clip.mp4:1:30`, `a.ts:12:5` keep their colons */
    bool changed = strcmp(base, *basep) != 0 && !(dashed && chunks.n == 1);
    if (changed) {
        sb_t l = SB_INIT;
        for (size_t i = chunks.n; i-- > 0;) {
            sb_append(&l, chunks.v[i]);
            if (i) sb_putc(&l, ',');
        }
        free(*basep);
        free(*listp);
        *basep = base;
        *listp = sb_detach(&l);
    } else free(base);
    strvec_free(&chunks);
    return changed;
}

/* read's splitPathAndSel (folding list colons) */
void read_split_path_sel(const char *raw, char **path, char **sel) {
    *sel = NULL;
    const char *colon = strrchr(raw, ':');
    if (!colon || colon == raw) {
        *path = xstrdup(raw);
        return;
    }
    const char *cand = colon + 1;
    size_t cl = strlen(cand);
    bool list_ok = sel_list_ok(cand, cl);
    if (!list_ok && !word_ieq(cand, cl, "raw") && !word_ieq(cand, cl, "conflicts") && !word_ieq(cand, cl, "img")) {
        *path = xstrdup(raw);
        return;
    }
    char *base = xstrndup(raw, (size_t)(colon - raw));
    char *list = NULL, *raw_suffix = NULL;
    if (list_ok) {
        list = xstrdup(cand);
    } else if (word_ieq(cand, cl, "raw")) {
        char *ic = strrchr(base, ':');
        if (ic && ic > base && sel_list_ok(ic + 1, strlen(ic + 1))) {
            list = xstrdup(ic + 1);
            raw_suffix = xasprintf(":%s", cand);
            *ic = 0;
        }
    }
    if (!list) {
        *path = base;
        *sel = xstrdup(cand);
        return;
    }
    read_fold_list_colons(&base, &list, 1); /* foldListColons(minColon 1) */
    if (!raw_suffix) {
        char *ic = strrchr(base, ':');
        if (ic && ic > base && word_ieq(ic + 1, strlen(ic + 1), "raw")) {
            *sel = xasprintf("%s:%s", ic + 1, list);
            *ic = 0;
            *path = base;
            free(list);
            return;
        }
    }
    *path = base;
    *sel = xasprintf("%s%s", list, raw_suffix ? raw_suffix : "");
    free(list);
    free(raw_suffix);
}

