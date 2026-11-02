/* changelog.c - see changelog.h */
#include "coding/changelog.h"

#include "core/core.h"
#include "core/embed.h"

#include <ctype.h>
#include <pthread.h>
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
    for (size_t i = n; i-- > 0;) { /* oldest first */
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
