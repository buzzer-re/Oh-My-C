/* changelog.c - see changelog.h */
#include "coding/changelog.h"

#include "core/core.h"
#include "core/embed.h"
#include "core/fs.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <zlib.h>

#define RECENT_LIMIT 3 /* RECENT_CHANGELOG_ENTRY_LIMIT */

static strvec_t g_entries; /* newest first, each "## [x.y.z] ..." + body, trimmed */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static char *inflate_asset(size_t *out_len) {
    size_t len;
    const char *z = embed_get("assets/CHANGELOG.md.z", &len);
    if (!z) return NULL;
    z_stream zs = {0};
    if (inflateInit(&zs) != Z_OK) return NULL;
    sb_t b = SB_INIT;
    char out[65536];
    zs.next_in = (Bytef *)z;
    zs.avail_in = (uInt)len;
    int r;
    do {
        zs.next_out = (Bytef *)out;
        zs.avail_out = sizeof out;
        r = inflate(&zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) {
            inflateEnd(&zs);
            sb_free(&b);
            return NULL;
        }
        sb_appendn(&b, out, sizeof out - zs.avail_out);
    } while (r != Z_STREAM_END);
    inflateEnd(&zs);
    *out_len = b.len;
    return sb_detach(&b);
}

/* /##\s+\[?(\d+)\.(\d+)\.(\d+)\]?/ anywhere in the line (the line starts with "## ") */
static bool version_heading(const char *line, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (line[i] != '#' || line[i + 1] != '#') continue;
        size_t j = i + 2;
        if (j >= n || !isspace((unsigned char)line[j])) continue;
        while (j < n && isspace((unsigned char)line[j])) j++;
        if (j < n && line[j] == '[') j++;
        int parts = 0;
        for (;;) {
            size_t d = j;
            while (j < n && isdigit((unsigned char)line[j])) j++;
            if (j == d) break;
            if (++parts == 3) return true;
            if (j >= n || line[j] != '.') break;
            j++;
        }
    }
    return false;
}

static char *trim_dupn(const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

/* parseChangelogContent */
static void load(void) {
    size_t len = 0;
    char *text = inflate_asset(&len);
    if (!text) return;
    sb_t cur = SB_INIT;
    bool in_version = false;
    const char *p = text, *end = text + len;
    while (p <= end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (n >= 3 && !memcmp(p, "## ", 3)) {
            if (in_version && cur.len) strvec_push_own(&g_entries, trim_dupn(cur.s, cur.len));
            sb_clear(&cur);
            in_version = version_heading(p, n);
            if (in_version) sb_appendn(&cur, p, n);
        } else if (in_version) {
            sb_putc(&cur, '\n');
            sb_appendn(&cur, p, n);
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (in_version && cur.len) strvec_push_own(&g_entries, trim_dupn(cur.s, cur.len));
    sb_free(&cur);
    free(text);
}

size_t changelog_entry_count(void) {
    pthread_once(&g_once, load);
    return g_entries.n;
}

bool changelog_parse_view(const char *args, changelog_view_t *v, char **err) {
    *err = NULL;
    char *t = str_trim(args ? args : "");
    for (char *q = t; *q; q++) *q = (char)tolower((unsigned char)*q);
    bool ok = true;
    if (!*t) *v = (changelog_view_t){CHANGELOG_RECENT, RECENT_LIMIT};
    else if (!strcmp(t, "full")) *v = (changelog_view_t){CHANGELOG_FULL, 0};
    else if (!strncmp(t, "last", 4) && (!t[4] || isspace((unsigned char)t[4]))) {
        /* /^last(?:\s+(\d+))?$/ */
        const char *q = t + 4;
        while (*q && isspace((unsigned char)*q)) q++;
        if (!*q) *v = (changelog_view_t){CHANGELOG_LAST, 1};
        else {
            const char *d = q;
            while (isdigit((unsigned char)*q)) q++;
            if (q == d || *q) ok = false;
            else {
                /* Number.parseInt of a long digit run saturates; any count past the list shows everything */
                long long c = 0;
                for (const char *x = d; x < q && c < 1000000; x++) c = c * 10 + (*x - '0');
                if (c < 1) *err = xstrdup(CHANGELOG_USAGE " (N must be a positive integer)");
                else *v = (changelog_view_t){CHANGELOG_LAST, (int)c};
            }
        }
    } else ok = false;
    if (!ok) *err = xstrdup(CHANGELOG_USAGE);
    free(t);
    return *err == NULL;
}

char *changelog_markdown(const changelog_view_t *v, size_t *shown) {
    size_t total = changelog_entry_count();
    size_t n = v->kind == CHANGELOG_FULL ? total : MIN(total, (size_t)MAX(v->count, 0));
    if (shown) *shown = n;
    if (!n) return NULL;
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) { /* newest first, like CHANGELOG.md (omp 38523878d4) */
        if (b.len) sb_append(&b, "\n\n");
        sb_append(&b, g_entries.v[i]);
    }
    return sb_detach(&b);
}

const char *changelog_title(const changelog_view_t *v, size_t shown, char *buf, size_t bufsz) {
    if (v->kind == CHANGELOG_FULL) return "Full Changelog";
    if (v->kind == CHANGELOG_RECENT) return "Recent Changes";
    size_t c = shown ? shown : (size_t)v->count;
    if (c == 1) return "Last Release";
    snprintf(buf, bufsz, "Last %zu Releases", c);
    return buf;
}

/* ---------- startup release notes (resolveStartupChangelogForDisplay) ---------- */

#define STARTUP_MAX_BYTES (64 * 1024)
#define STARTUP_FULL_HINT "Use `/changelog full` to view the complete changelog."

/* parseChangelogVersion: exactly "x.y.z" */
static bool parse_version(const char *s, int v[3]) {
    if (!s) return false;
    const char *p = s;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*p)) return false;
        long x = 0;
        while (isdigit((unsigned char)*p)) {
            if (x < 100000000) x = x * 10 + (*p - '0');
            p++;
        }
        v[i] = (int)x;
        if (i < 2 && *p++ != '.') return false;
    }
    return *p == 0;
}

/* the version of an entry's "## [x.y.z]" heading */
static void entry_version(const char *e, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    const char *p = e;
    while (*p && !isdigit((unsigned char)*p) && *p != '\n') p++;
    for (int i = 0; i < 3 && isdigit((unsigned char)*p); i++) {
        v[i] = atoi(p);
        while (isdigit((unsigned char)*p)) p++;
        if (*p == '.') p++;
    }
}

static int cmp_version(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}

const char *changelog_current_version(void) {
    static char buf[48];
    if (changelog_entry_count() == 0) return NULL;
    int v[3];
    entry_version(g_entries.v[0], v);
    snprintf(buf, sizeof buf, "%d.%d.%d", v[0], v[1], v[2]);
    return buf;
}

static void cat_add(changelog_startup_t *s, const char *name) {
    for (size_t i = 0; i < s->categories.n; i++)
        if (!strcmp(s->categories.v[i], name)) {
            s->counts.v[i]++;
            return;
        }
    strvec_push(&s->categories, name);
    vec_push(&s->counts, 1);
}

/* summarizeChangelogEntries: top-level list items per `###` section ("Other" before any heading); a bare marker
 * announces nothing */
static void summarize(changelog_startup_t *s, const char *entry) {
    char *cat = xstrdup("Other");
    bool fence = false;
    for (const char *p = entry; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xstrndup(p, n);
        size_t ind = 0;
        while (line[ind] == ' ') ind++;
        if (ind < 4 && (!strncmp(line + ind, "```", 3) || !strncmp(line + ind, "~~~", 3))) fence = !fence;
        else if (!fence && !strncmp(line, "### ", 4)) {
            free(cat);
            cat = str_trim(line + 4);
            if (!*cat) {
                free(cat);
                cat = xstrdup("Other");
            }
        } else if (!fence && ind == 0 && (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
                   (line[1] == ' ' || line[1] == '\t' || !line[1])) {
            char *t = str_trim(line + 1);
            bool task = !strncmp(t, "[ ]", 3) || !strncasecmp(t, "[x]", 3);
            if (*t || task) {
                cat_add(s, cat);
                s->change_count++;
            }
            free(t);
        }
        free(line);
        if (!nl) break;
        p = nl + 1;
    }
    free(cat);
}

static char *read_marker(const char *path) {
    size_t n = 0;
    char *d = fs_read(path, &n);
    if (!d) return NULL;
    char *t = str_trim(d);
    free(d);
    if (!*t) {
        free(t);
        return NULL;
    }
    return t;
}

void changelog_startup_free(changelog_startup_t *s) {
    free(s->markdown);
    strvec_free(&s->categories);
    vec_free(&s->counts);
    memset(s, 0, sizeof *s);
}

bool changelog_startup_resolve(const char *mode, const char *marker_path, changelog_startup_t *out) {
    memset(out, 0, sizeof *out);
    const char *current = changelog_current_version();
    if (!current) return false;
    char *last = read_marker(marker_path);
    int lv[3], cv[3];
    if (!parse_version(last, lv)) {
        fs_write(marker_path, current, strlen(current));
        free(last);
        return false;
    }
    if (!strcmp(last, current)) { /* steady state */
        free(last);
        return false;
    }
    free(last);
    if (mode && !strcmp(mode, "hidden")) { /* only an upgrade advances the marker */
        if (parse_version(current, cv) && cmp_version(cv, lv) > 0) fs_write(marker_path, current, strlen(current));
        return false;
    }
    /* parseStartupChangelogFile: newest first, stop at the first seen version */
    size_t sel[RECENT_LIMIT], nsel = 0;
    for (size_t i = 0; i < g_entries.n; i++) {
        int ev[3];
        entry_version(g_entries.v[i], ev);
        if (cmp_version(ev, lv) <= 0) break;
        out->total_unseen++;
        if (nsel < RECENT_LIMIT) sel[nsel++] = i;
    }
    if (!nsel) return false;
    /* renderChangelogEntries(oldestFirst: false): newest first, capped at 64 KB with the full-changelog hint */
    sb_t b = SB_INIT;
    for (size_t i = 0; i < nsel; i++) {
        if (i) sb_append(&b, "\n\n");
        sb_append(&b, g_entries.v[sel[i]]);
        summarize(out, g_entries.v[sel[i]]);
    }
    if (b.len > STARTUP_MAX_BYTES) {
        const char *suffix = "\n\n\xe2\x80\xa6\n\n" STARTUP_FULL_HINT;
        size_t keep = STARTUP_MAX_BYTES - strlen(suffix);
        while (keep && ((unsigned char)b.s[keep] & 0xC0) == 0x80) keep--; /* never split a UTF-8 sequence */
        b.len = keep;
        b.s[keep] = 0;
        sb_append(&b, suffix);
        out->truncated = true;
    }
    out->markdown = sb_detach(&b);
    out->selected = (int)nsel;
    int v[3];
    entry_version(g_entries.v[sel[0]], v);
    snprintf(out->latest, sizeof out->latest, "%d.%d.%d", v[0], v[1], v[2]);
    fs_write(marker_path, current, strlen(current));
    return true;
}

static const char *const CATEGORY_ORDER[] = {"Breaking Changes", "Added", "Changed", "Deprecated", "Removed", "Fixed", "Security"};

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static bool known_category(const char *c) {
    for (size_t k = 0; k < ARRAY_LEN(CATEGORY_ORDER); k++)
        if (!strcmp(c, CATEGORY_ORDER[k])) return true;
    return false;
}

char *changelog_startup_summary(const changelog_startup_t *s) {
    if (!s->latest[0] || s->selected == 0) return xstrdup("Updated omp. Use /changelog for recent changes.");
    sb_t b = SB_INIT;
    const char *cw = s->change_count == 1 ? "change" : "changes";
    if (s->selected == 1) sb_appendf(&b, "Updated to v%s \xc2\xb7 %d %s in 1 release", s->latest, s->change_count, cw);
    else sb_appendf(&b, "Updated to v%s \xc2\xb7 %d %s across %d releases", s->latest, s->change_count, cw, s->selected);
    /* the known categories in order, then the others sorted */
    strvec_t order = {0}, extra = {0};
    for (size_t k = 0; k < ARRAY_LEN(CATEGORY_ORDER); k++)
        for (size_t i = 0; i < s->categories.n; i++)
            if (!strcmp(s->categories.v[i], CATEGORY_ORDER[k]) && s->counts.v[i]) strvec_push(&order, CATEGORY_ORDER[k]);
    for (size_t i = 0; i < s->categories.n; i++)
        if (!known_category(s->categories.v[i])) strvec_push(&extra, s->categories.v[i]);
    if (extra.n) qsort(extra.v, extra.n, sizeof *extra.v, cmp_str);
    for (size_t i = 0; i < extra.n; i++) strvec_push(&order, extra.v[i]);
    sb_t br = SB_INIT;
    for (size_t k = 0; k < order.n; k++) {
        int c = 0;
        for (size_t i = 0; i < s->categories.n; i++)
            if (!strcmp(s->categories.v[i], order.v[k])) c = s->counts.v[i];
        char *label;
        if (!strcmp(order.v[k], "Breaking Changes")) label = xstrdup(c == 1 ? "breaking change" : "breaking changes");
        else {
            label = xstrdup(order.v[k]);
            for (char *q = label; *q; q++) *q = (char)tolower((unsigned char)*q);
        }
        sb_appendf(&br, "%s%d %s", k ? " \xc2\xb7 " : "", c, label);
        free(label);
    }
    int omitted = s->total_unseen - s->selected;
    char *hint = omitted > 0 ? xasprintf("+%d earlier %s \xc2\xb7 Use /changelog full for history.", omitted, omitted == 1 ? "release" : "releases")
                             : xstrdup("Use /changelog for details.");
    if (br.len) sb_appendf(&b, "\n%s \xc2\xb7 %s", sb_cstr(&br), hint);
    else sb_appendf(&b, "\n%s", hint);
    free(hint);
    sb_free(&br);
    strvec_free(&order);
    strvec_free(&extra);
    return sb_detach(&b);
}
