/* readfmt.c - archives (zip/tar/tar.gz/gz), SQLite (on-disk format), PDF text and notebooks for `read`.
 * Ports the behavior of omp's read-archive.ts / sqlite-reader.ts / markit pdf / notebook.rs.
 * Untrusted input throughout: every offset/length is checked, recursion depth and sizes are capped. */
#define _GNU_SOURCE
#include "readfmt.h"

#include "core/fs.h"
#include "core/json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* ---------- shared helpers ---------- */

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64le(const uint8_t *p) { return (uint64_t)rd32le(p) | (uint64_t)rd32le(p + 4) << 32; }
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3]; }

char *rf_fmt_bytes(int64_t n) {
    if (n < 1024) return xasprintf("%lldB", (long long)n);
    if (n < 1024 * 1024) return xasprintf("%.1fKB", n / 1024.0);
    if (n < 1024LL * 1024 * 1024) return xasprintf("%.1fMB", n / (1024.0 * 1024));
    return xasprintf("%.1fGB", n / (1024.0 * 1024 * 1024));
}

bool rf_is_text(const char *data, size_t len) {
    if (memchr(data, 0, len)) return false;
    return utf8_valid(data, len);
}

void rf_splits_free(rf_splits_t *s) {
    for (size_t i = 0; i < s->n; i++) {
        free(s->v[i].container);
        free(s->v[i].sub);
        free(s->v[i].query);
    }
    vec_free(s);
}

static int cmp_split_len(const void *a, const void *b) {
    size_t x = strlen(((const rf_split_t *)a)->container), y = strlen(((const rf_split_t *)b)->container);
    return x < y ? 1 : x > y ? -1 : 0;
}

static bool split_seen(const rf_splits_t *out, const rf_split_t *s) {
    for (size_t i = 0; i < out->n; i++)
        if (!strcmp(out->v[i].container, s->container) && !strcmp(out->v[i].sub, s->sub) &&
            !strcmp(out->v[i].query, s->query))
            return true;
    return false;
}

/* scan for ".<ext>" (case-insensitive, longest alternative first) followed by one of `follow` or end */
static void split_candidates(const char *path, const char *const *exts, size_t nexts, const char *follow, bool sqlite,
                             rf_splits_t *out) {
    char *norm = xstrdup(path);
    for (char *p = norm; *p; p++)
        if (*p == '\\') *p = '/';
    size_t n = strlen(norm);
    size_t i = 0;
    while (i < n) {
        if (norm[i] != '.') {
            i++;
            continue;
        }
        size_t matched = 0;
        for (size_t k = 0; k < nexts; k++) {
            size_t el = strlen(exts[k]);
            if (i + 1 + el > n || strncasecmp(norm + i + 1, exts[k], el) != 0) continue;
            char nx = norm[i + 1 + el];
            if (nx && !strchr(follow, nx)) continue;
            matched = 1 + el;
            break;
        }
        if (!matched) {
            i++;
            continue;
        }
        size_t end = i + matched;
        rf_split_t s;
        s.container = xstrndup(path, end);
        const char *rem = norm + end;
        if (sqlite) {
            const char *q = strchr(rem, '?');
            size_t sublen = q ? (size_t)(q - rem) : strlen(rem);
            while (sublen && *rem == ':') rem++, sublen--;
            s.sub = xstrndup(rem, sublen);
            s.query = xstrdup(q ? q + 1 : "");
        } else {
            while (*rem == ':') rem++;
            s.sub = xstrdup(rem);
            s.query = xstrdup("");
        }
        if (split_seen(out, &s)) {
            free(s.container);
            free(s.sub);
            free(s.query);
        } else {
            vec_push(out, s);
        }
        i = end;
    }
    free(norm);
    if (out->n > 1) qsort(out->v, out->n, sizeof(rf_split_t), cmp_split_len);
}

/* upstream FORMAT_EXTENSIONS, longest first */
static const char *const ARCHIVE_EXTS[] = {
    "tar.bz2", "tar.zst", "tar.gz", "tar.xz", "nupkg", "tar.z", "asar", "cpio", "tbz2", "tzst", "vsix", "lzma", "apk",
    "arj", "bz2", "cab", "cbr", "cbz", "deb", "ear", "iso", "ipa", "jar", "lha", "lib", "lzh", "rar", "rpm", "tar", "tbz",
    "tgz", "txz", "war", "whl", "xpi", "zip", "zst", "7z", "ar", "gz", "xz", "a", "z",
};

void rf_archive_candidates(const char *path, rf_splits_t *out) {
    memset(out, 0, sizeof(*out));
    split_candidates(path, ARCHIVE_EXTS, ARRAY_LEN(ARCHIVE_EXTS), ":", false, out);
}

static const char *const SQLITE_EXTS[] = {"sqlite3", "sqlite", "db3", "db"};

void rf_sqlite_candidates(const char *path, rf_splits_t *out) {
    memset(out, 0, sizeof(*out));
    split_candidates(path, SQLITE_EXTS, ARRAY_LEN(SQLITE_EXTS), ":?", true, out);
}

/* truncate plain text to `maxw` terminal columns, appending "…" when cut (upstream truncateToWidth) */
static char *trunc_width(const char *s, int maxw) {
    size_t len = strlen(s);
    if ((int)str_display_width(s) <= maxw) return xstrdup(s);
    sb_t b = SB_INIT;
    int w = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        int k = utf8_decode(s + i, len - i, &cp);
        int cw = wcwidth_cp(cp);
        if (cw < 0) cw = 0;
        if (w + cw > maxw - 1) break;
        sb_appendn(&b, s + i, (size_t)k);
        w += cw;
        i += (size_t)k;
    }
    if (maxw > 0) sb_append(&b, "…");
    return sb_detach(&b);
}

static char *replace_tabs(const char *s) { return str_replace(s, "\t", "   "); }

/* ---------- archives ---------- */

#define AR_MAX_ENTRIES 1000000
#define AR_MAX_IN_MEMORY (256u << 20)
#define AR_MAX_INDEX (64u << 20)
#define AR_MAX_MEMBER (64u << 20)
#define AR_MAX_PATH 4096
#define AR_MAX_LINK_DEPTH 40

enum { AE_DIR, AE_ZIP, AE_BUF, AE_LINK };

typedef struct {
    char *path;
    int kind;
    bool is_dir;
    int64_t size;
    uint64_t off;   /* zip: local header offset; buf: data offset */
    uint64_t csize; /* zip compressed size */
    uint16_t method, flags;
    uint32_t crc;
    char *link; /* AE_LINK: normalized root-relative target, NULL when it escapes the root */
    char *raw_link;
} ar_ent_t;

struct rf_archive {
    char fmt[16];
    int fd;
    uint64_t fsize;
    int64_t shift; /* zip: bytes prepended before the archive (self-extracting stubs) */
    char *buf;
    size_t buflen;
    VEC(ar_ent_t) ents;
    hmap_t idx; /* path -> (index + 1) */
};

const char *rf_archive_format(const rf_archive_t *a) { return a->fmt; }

/* normalize a path: '\\' -> '/', drop "" and "." segments; NULL when ".." appears */
static char *ar_norm(const char *raw, size_t n) {
    sb_t b = SB_INIT;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && raw[j] != '/' && raw[j] != '\\') j++;
        size_t sl = j - i;
        if (sl == 2 && raw[i] == '.' && raw[i + 1] == '.') {
            sb_free(&b);
            return NULL;
        }
        if (sl && !(sl == 1 && raw[i] == '.')) {
            if (b.len) sb_putc(&b, '/');
            sb_appendn(&b, raw + i, sl);
        }
        i = j + 1;
    }
    return sb_detach(&b);
}

static ar_ent_t *ar_get(rf_archive_t *a, const char *path) {
    uintptr_t v = (uintptr_t)hmap_get(&a->idx, path);
    return v ? &a->ents.v[v - 1] : NULL;
}

static void ar_ent_free(ar_ent_t *e) {
    free(e->path);
    free(e->link);
    free(e->raw_link);
}

/* insert or replace (later members win, like upstream upsertArchiveEntry); takes ownership of e's strings */
static int ar_upsert(rf_archive_t *a, ar_ent_t *e, char **err) {
    ar_ent_t *old = ar_get(a, e->path);
    if (old) {
        /* a directory record never replaces a file, and a synthesized dir never replaces anything */
        if (e->is_dir && !old->is_dir) {
            ar_ent_free(e);
            return 0;
        }
        ar_ent_free(old);
        *old = *e;
        return 0;
    }
    if (a->ents.n >= AR_MAX_ENTRIES) {
        ar_ent_free(e);
        *err = xasprintf("Archive has too many entries (> %d limit)", AR_MAX_ENTRIES);
        return -1;
    }
    vec_push(&a->ents, *e);
    hmap_put(&a->idx, e->path, (void *)(uintptr_t)a->ents.n);
    return 0;
}

static int ar_add(rf_archive_t *a, const char *raw_name, size_t raw_len, ar_ent_t tmpl, char **err) {
    if (raw_len > AR_MAX_PATH) {
        *err = xasprintf("Archive member path exceeds %d bytes", AR_MAX_PATH);
        return -1;
    }
    char *clean = utf8_valid(raw_name, raw_len) ? xstrndup(raw_name, raw_len) : utf8_sanitize(raw_name, raw_len);
    char *p = ar_norm(clean, strlen(clean));
    free(clean);
    if (!p || !*p) { /* unrepresentable (escapes the root / empty): dropped like upstream */
        free(p);
        free(tmpl.link);
        free(tmpl.raw_link);
        return 0;
    }
    tmpl.path = p;
    return ar_upsert(a, &tmpl, err);
}

static void ar_ensure_parents(rf_archive_t *a) {
    for (size_t i = 0; i < a->ents.n; i++) {
        char *p = xstrdup(a->ents.v[i].path);
        for (char *s = strrchr(p, '/'); s; s = strrchr(p, '/')) {
            *s = 0;
            if (ar_get(a, p)) break;
            if (a->ents.n >= AR_MAX_ENTRIES) break;
            ar_ent_t d = {0};
            d.path = xstrdup(p);
            d.kind = AE_DIR;
            d.is_dir = true;
            vec_push(&a->ents, d);
            hmap_put(&a->idx, d.path, (void *)(uintptr_t)a->ents.n);
        }
        free(p);
    }
}

/* resolve symlink aliases component by component; returns owned resolved path ("" = root) or NULL */
static char *ar_resolve(rf_archive_t *a, const char *norm) {
    strvec_t comps = str_split(norm, "/");
    char *cur = xstrdup("");
    int hops = 0;
    for (size_t i = 0; i < comps.n; i++) {
        if (!*comps.v[i]) continue;
        char *next = *cur ? xasprintf("%s/%s", cur, comps.v[i]) : xstrdup(comps.v[i]);
        free(cur);
        cur = next;
        for (;;) {
            ar_ent_t *e = ar_get(a, cur);
            if (!e || e->kind != AE_LINK || !e->link) break;
            if (++hops > AR_MAX_LINK_DEPTH) {
                free(cur);
                strvec_free(&comps);
                return NULL;
            }
            free(cur);
            cur = xstrdup(e->link);
        }
    }
    strvec_free(&comps);
    return cur;
}

int rf_archive_stat(rf_archive_t *a, const char *sub, int64_t *size) {
    char *norm = ar_norm(sub ? sub : "", sub ? strlen(sub) : 0);
    if (!norm) return 0;
    if (size) *size = 0;
    if (!*norm) {
        free(norm);
        return 2;
    }
    char *res = ar_resolve(a, norm);
    free(norm);
    if (!res) return 0;
    if (!*res) {
        free(res);
        return 2;
    }
    ar_ent_t *e = ar_get(a, res);
    free(res);
    if (!e) return 0;
    if (size) *size = e->size;
    return e->is_dir ? 2 : 1;
}

static int cmp_dirent(const void *a, const void *b) {
    const rf_dirent_t *x = a, *y = b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

void rf_dirents_free(rf_dirents_t *d) {
    for (size_t i = 0; i < d->n; i++) free(d->v[i].name);
    vec_free(d);
}

int rf_archive_list(rf_archive_t *a, const char *sub, rf_dirents_t *out, char **err) {
    memset(out, 0, sizeof(*out));
    char *norm = ar_norm(sub ? sub : "", sub ? strlen(sub) : 0);
    if (!norm) {
        *err = xstrdup("Archive path cannot contain '..'");
        return -1;
    }
    char *res = *norm ? ar_resolve(a, norm) : xstrdup("");
    if (!res) {
        *err = xasprintf("Archive path '%s' not found", norm);
        free(norm);
        return -1;
    }
    if (*norm && *res) {
        ar_ent_t *e = ar_get(a, res);
        if (!e || !e->is_dir) {
            *err = xasprintf(e ? "Archive path '%s' is not a directory" : "Archive path '%s' not found", norm);
            free(norm);
            free(res);
            return -1;
        }
    }
    size_t plen = *res ? strlen(res) + 1 : 0;
    hmap_t seen;
    hmap_init(&seen);
    for (size_t i = 0; i < a->ents.n; i++) {
        ar_ent_t *e = &a->ents.v[i];
        if (plen) {
            if (strncmp(e->path, res, plen - 1) || e->path[plen - 1] != '/') continue;
        }
        const char *rel = e->path + plen;
        const char *sl = strchr(rel, '/');
        size_t seglen = sl ? (size_t)(sl - rel) : strlen(rel);
        if (!seglen) continue;
        char *seg = xstrndup(rel, seglen);
        if (hmap_has(&seen, seg)) {
            free(seg);
            continue;
        }
        hmap_put(&seen, seg, NULL);
        char *src = plen ? xasprintf("%s/%s", res, seg) : xstrdup(seg);
        char *rc = ar_resolve(a, src);
        ar_ent_t *ce = rc && *rc ? ar_get(a, rc) : NULL;
        bool is_dir = (rc && !*rc) || (ce && ce->is_dir) || sl != NULL;
        rf_dirent_t d = {seg, is_dir, is_dir ? 0 : (ce ? ce->size : e->size)};
        vec_push(out, d);
        free(src);
        free(rc);
    }
    hmap_free(&seen, NULL);
    if (out->n > 1) qsort(out->v, out->n, sizeof(rf_dirent_t), cmp_dirent);
    free(norm);
    free(res);
    return 0;
}

char *rf_format_dirents(const rf_dirent_t *v, size_t n) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        if (i) sb_putc(&b, '\n');
        if (v[i].is_dir) {
            sb_appendf(&b, "%s/", v[i].name);
        } else if (v[i].size > 0) {
            char *sz = rf_fmt_bytes(v[i].size);
            sb_appendf(&b, "%s (%s)", v[i].name, sz);
            free(sz);
        } else {
            sb_append(&b, v[i].name);
        }
    }
    return sb_detach(&b);
}

static bool pread_full(int fd, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, (off_t)(off + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

/* inflate `in` (raw deflate when raw, else zlib/gzip auto-detect with concatenated gzip members) up to cap */
static char *inflate_buf(const uint8_t *in, size_t n, bool raw, size_t cap, size_t expect, size_t *out_len, char **err) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, raw ? -15 : 15 + 32) != Z_OK) {
        *err = xstrdup("zlib initialization failed");
        return NULL;
    }
    size_t cap_alloc = expect ? expect + 1 : MIN(cap + 1, MAX(n * 4, (size_t)65536));
    char *out = xmalloc(cap_alloc + 1);
    size_t used = 0;
    z.next_in = (Bytef *)in;
    size_t remaining = n;
    int rc = Z_OK;
    for (;;) {
        if (used == cap_alloc) {
            if (cap_alloc > cap) break;
            size_t nc = MIN(cap + 1, cap_alloc * 2);
            if (nc <= cap_alloc) break;
            cap_alloc = nc;
            out = xrealloc(out, cap_alloc + 1);
        }
        uInt chunk_in = (uInt)MIN(remaining, (size_t)1 << 30);
        z.avail_in = chunk_in;
        uInt chunk_out = (uInt)MIN(cap_alloc - used, (size_t)1 << 30);
        z.next_out = (Bytef *)out + used;
        z.avail_out = chunk_out;
        rc = inflate(&z, Z_NO_FLUSH);
        used += chunk_out - z.avail_out;
        remaining -= chunk_in - z.avail_in;
        if (rc == Z_STREAM_END) {
            /* concatenated gzip members */
            if (!raw && remaining >= 2 && z.next_in[0] == 0x1f && z.next_in[1] == 0x8b) {
                inflateReset(&z);
                continue;
            }
            break;
        }
        if (rc == Z_BUF_ERROR && z.avail_out != 0 && remaining == 0) break; /* truncated input */
        if (rc != Z_OK && rc != Z_BUF_ERROR) break;
        if (remaining == 0 && z.avail_out != 0) break;
    }
    inflateEnd(&z);
    if (used > cap) {
        free(out);
        char *lim = rf_fmt_bytes((int64_t)cap);
        *err = xasprintf("Archive is too large to read in memory (> %s limit)", lim);
        free(lim);
        return NULL;
    }
    if (rc != Z_STREAM_END) {
        free(out);
        *err = xstrdup(rc == Z_DATA_ERROR ? "Invalid compressed data" : "Truncated compressed data");
        return NULL;
    }
    out[used] = 0;
    *out_len = used;
    return out;
}

/* --- zip --- */
static int zip_index(rf_archive_t *a, char **err) {
    uint64_t fsz = a->fsize;
    if (fsz < 22) {
        *err = xstrdup("Invalid zip archive: file too small");
        return -1;
    }
    size_t tail = (size_t)MIN(fsz, (uint64_t)(65535 + 22));
    uint8_t *t = xmalloc(tail);
    if (!pread_full(a->fd, t, tail, fsz - tail)) {
        free(t);
        *err = xstrdup("Cannot read zip archive");
        return -1;
    }
    ssize_t eo = -1;
    for (ssize_t i = (ssize_t)tail - 22; i >= 0; i--) {
        if (t[i] == 'P' && t[i + 1] == 'K' && t[i + 2] == 5 && t[i + 3] == 6) {
            uint16_t clen = rd16le(t + i + 20);
            if ((size_t)i + 22 + clen <= tail) {
                eo = i;
                break;
            }
        }
    }
    if (eo < 0) {
        free(t);
        *err = xstrdup("Invalid zip archive: end of central directory not found");
        return -1;
    }
    uint64_t eocd_pos = fsz - tail + (uint64_t)eo;
    uint64_t count = rd16le(t + eo + 10), cd_size = rd32le(t + eo + 12), cd_off = rd32le(t + eo + 16);
    bool z64 = false;
    if (eo >= 20 && rd32le(t + eo - 20) == 0x07064b50) {
        uint64_t e64 = rd64le(t + eo - 20 + 8);
        uint8_t r[56];
        if (e64 + 56 <= fsz && pread_full(a->fd, r, 56, e64) && rd32le(r) == 0x06064b50) {
            count = rd64le(r + 32);
            cd_size = rd64le(r + 40);
            cd_off = rd64le(r + 48);
            z64 = true;
        }
    }
    free(t);
    if (!z64 && cd_off + cd_size < eocd_pos) a->shift = (int64_t)(eocd_pos - cd_off - cd_size); /* SFX stub */
    cd_off += (uint64_t)a->shift;
    if (cd_size > AR_MAX_INDEX) {
        char *s = rf_fmt_bytes((int64_t)cd_size);
        *err = xasprintf("Archive central directory is too large (%s > 64.0MB limit)", s);
        free(s);
        return -1;
    }
    if (cd_off > fsz || cd_size > fsz - cd_off) {
        *err = xstrdup("Invalid zip archive: central directory out of bounds");
        return -1;
    }
    if (count > AR_MAX_ENTRIES) {
        *err = xasprintf("Archive has too many entries (> %d limit)", AR_MAX_ENTRIES);
        return -1;
    }
    uint8_t *cd = xmalloc(cd_size ? cd_size : 1);
    if (cd_size && !pread_full(a->fd, cd, cd_size, cd_off)) {
        free(cd);
        *err = xstrdup("Cannot read zip central directory");
        return -1;
    }
    size_t p = 0;
    int rc = 0;
    for (uint64_t k = 0; k < count && p + 46 <= cd_size; k++) {
        const uint8_t *h = cd + p;
        if (rd32le(h) != 0x02014b50) break;
        uint16_t flags = rd16le(h + 8), method = rd16le(h + 10);
        uint32_t crc = rd32le(h + 16);
        uint64_t csize = rd32le(h + 20), usize = rd32le(h + 24);
        uint16_t nlen = rd16le(h + 28), xlen = rd16le(h + 30), clen = rd16le(h + 32);
        uint16_t made_by = rd16le(h + 4);
        uint32_t eattr = rd32le(h + 38);
        uint64_t lho = rd32le(h + 42);
        if (p + 46 + (size_t)nlen + xlen + clen > cd_size) break;
        const uint8_t *name = h + 46, *x = name + nlen, *xe = x + xlen;
        while (x + 4 <= xe) {
            uint16_t id = rd16le(x), sz = rd16le(x + 2);
            const uint8_t *d = x + 4, *de = d + sz;
            if (de > xe) break;
            if (id == 1) {
                if (usize == 0xFFFFFFFFu && d + 8 <= de) usize = rd64le(d), d += 8;
                if (csize == 0xFFFFFFFFu && d + 8 <= de) csize = rd64le(d), d += 8;
                if (lho == 0xFFFFFFFFu && d + 8 <= de) lho = rd64le(d), d += 8;
            }
            x = de;
        }
        bool is_dir = (nlen && (name[nlen - 1] == '/' || name[nlen - 1] == '\\'));
        unsigned host = made_by >> 8;
        if (host == 3 && ((eattr >> 16) & S_IFMT) == S_IFDIR) is_dir = true;
        if (host == 0 && (eattr & 0x10)) is_dir = true;
        ar_ent_t e = {0};
        e.kind = is_dir ? AE_DIR : AE_ZIP;
        e.is_dir = is_dir;
        e.size = is_dir ? 0 : (int64_t)MIN(usize, (uint64_t)INT64_MAX);
        e.off = lho + (uint64_t)a->shift;
        e.csize = csize;
        e.method = method;
        e.flags = flags;
        e.crc = crc;
        if (ar_add(a, (const char *)name, nlen, e, err) != 0) {
            rc = -1;
            break;
        }
        p += 46 + (size_t)nlen + xlen + clen;
    }
    free(cd);
    return rc;
}

static char *zip_read(rf_archive_t *a, ar_ent_t *e, size_t *len, char **err) {
    if (e->flags & 1) {
        *err = xasprintf("Archive member '%s' is encrypted", e->path);
        return NULL;
    }
    uint8_t lh[30];
    if (e->off + 30 > a->fsize || !pread_full(a->fd, lh, 30, e->off) || rd32le(lh) != 0x04034b50) {
        *err = xasprintf("Invalid zip archive: bad local header for '%s'", e->path);
        return NULL;
    }
    uint64_t data = e->off + 30 + rd16le(lh + 26) + rd16le(lh + 28);
    if (data > a->fsize || e->csize > a->fsize - data) {
        *err = xasprintf("Invalid zip archive: member '%s' is truncated", e->path);
        return NULL;
    }
    size_t usize = (size_t)e->size;
    char *out = NULL;
    if (e->method == 0) {
        if (e->csize != (uint64_t)usize) {
            *err = xasprintf("Invalid zip archive: size mismatch for '%s'", e->path);
            return NULL;
        }
        out = xmalloc(usize + 1);
        if (usize && !pread_full(a->fd, out, usize, data)) {
            free(out);
            *err = xasprintf("Cannot read archive member '%s'", e->path);
            return NULL;
        }
        out[usize] = 0;
    } else if (e->method == 8) {
        if (e->csize > (uint64_t)AR_MAX_IN_MEMORY) {
            *err = xasprintf("Archive member '%s' is too large to extract in memory", e->path);
            return NULL;
        }
        uint8_t *cbuf = xmalloc(e->csize ? e->csize : 1);
        if (e->csize && !pread_full(a->fd, cbuf, e->csize, data)) {
            free(cbuf);
            *err = xasprintf("Cannot read archive member '%s'", e->path);
            return NULL;
        }
        size_t got = 0;
        char *ierr = NULL;
        out = inflate_buf(cbuf, e->csize, true, usize, usize, &got, &ierr);
        free(cbuf);
        if (!out || got != usize) {
            free(out);
            *err = xasprintf("Invalid zip archive: cannot inflate '%s'%s%s", e->path, ierr ? ": " : "", ierr ? ierr : "");
            free(ierr);
            return NULL;
        }
    } else {
        *err = xasprintf("Archive member '%s' uses unsupported compression method %u", e->path, e->method);
        return NULL;
    }
    if ((uint32_t)crc32(0L, (const Bytef *)out, (uInt)usize) != e->crc && usize <= UINT32_MAX) {
        free(out);
        *err = xasprintf("Archive member '%s' failed its CRC check", e->path);
        return NULL;
    }
    *len = usize;
    return out;
}

/* --- tar --- */
static int64_t tar_num(const uint8_t *f, size_t n) {
    if (f[0] & 0x80) { /* base-256 */
        uint64_t v = f[0] & 0x7f;
        for (size_t i = 1; i < n; i++) {
            if (v >> 55) return -1;
            v = v << 8 | f[i];
        }
        return (int64_t)v;
    }
    int64_t v = 0;
    size_t i = 0;
    while (i < n && (f[i] == ' ' || f[i] == 0)) i++;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
        if (v > (INT64_MAX >> 3)) return -1;
        v = v * 8 + (f[i] - '0');
    }
    return v;
}

static bool tar_checksum_ok(const uint8_t *h) {
    int64_t want = tar_num(h + 148, 8);
    uint64_t sum = 0;
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return want >= 0 && (uint64_t)want == sum;
}

static bool tar_sniff(const uint8_t *b, size_t n) { return n >= 512 && tar_checksum_ok(b) && b[0]; }

static size_t field_len(const uint8_t *f, size_t n) {
    const uint8_t *z = memchr(f, 0, n);
    return z ? (size_t)(z - f) : n;
}

/* resolve a symlink target relative to the link's directory; NULL when it escapes the root */
static char *link_target(const char *link_path, const char *target, bool hard) {
    if (hard || target[0] == '/') return ar_norm(target, strlen(target));
    char *dir = xstrdup(link_path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    else dir[0] = 0;
    char *joined = *dir ? xasprintf("%s/%s", dir, target) : xstrdup(target);
    free(dir);
    /* fold ".." lexically */
    strvec_t parts = str_split(joined, "/");
    free(joined);
    strvec_t st = {0};
    bool bad = false;
    for (size_t i = 0; i < parts.n; i++) {
        const char *s = parts.v[i];
        if (!*s || !strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) {
            if (!st.n) {
                bad = true;
                break;
            }
            free(vec_pop(&st));
            continue;
        }
        strvec_push(&st, s);
    }
    char *r = bad ? NULL : strvec_join(&st, "/");
    strvec_free(&st);
    strvec_free(&parts);
    return r;
}

static int tar_index(rf_archive_t *a, char **err) {
    const uint8_t *b = (const uint8_t *)a->buf;
    size_t n = a->buflen, off = 0;
    char *longname = NULL, *longlink = NULL, *pax_path = NULL, *pax_link = NULL;
    int64_t pax_size = -1;
    int rc = 0;
    bool first = true;
    while (off + 512 <= n) {
        const uint8_t *h = b + off;
        bool zero = true;
        for (int i = 0; i < 512 && zero; i++) zero = !h[i];
        if (zero) break;
        if (!tar_checksum_ok(h)) {
            if (first) {
                *err = xstrdup("Invalid tar archive: bad header checksum");
                rc = -1;
            }
            break;
        }
        first = false;
        int64_t size = pax_size >= 0 ? pax_size : tar_num(h + 124, 12);
        char type = (char)h[156];
        size_t data = off + 512;
        if (size < 0 || (uint64_t)size > n - data) {
            *err = xstrdup("Invalid tar archive: truncated member");
            rc = -1;
            break;
        }
        size_t padded = ((size_t)size + 511) & ~(size_t)511;
        size_t next = (padded > n - data) ? n : data + padded;
        if (type == 'L' || type == 'K') {
            if (size > AR_MAX_PATH) {
                *err = xasprintf("Archive member path exceeds %d bytes", AR_MAX_PATH);
                rc = -1;
                break;
            }
            char *s = xstrndup((const char *)b + data, field_len(b + data, (size_t)size));
            if (type == 'L') free(longname), longname = s;
            else free(longlink), longlink = s;
            off = next;
            continue;
        }
        if (type == 'x' || type == 'g') {
            if (type == 'x') {
                size_t p = 0, sz = (size_t)size;
                const char *r = (const char *)b + data;
                while (p < sz) {
                    size_t q = p;
                    size_t reclen = 0;
                    while (q < sz && isdigit((unsigned char)r[q]) && reclen < sz) reclen = reclen * 10 + (size_t)(r[q++] - '0');
                    if (q >= sz || r[q] != ' ' || reclen <= q - p || reclen > sz - p) break;
                    const char *kv = r + q + 1, *end = r + p + reclen;
                    const char *eq = memchr(kv, '=', (size_t)(end - kv));
                    if (eq) {
                        size_t vl = (size_t)(end - eq - 1);
                        if (vl && eq[vl] == '\n') vl--;
                        if (vl <= AR_MAX_PATH) {
                            if ((size_t)(eq - kv) == 4 && !memcmp(kv, "path", 4)) free(pax_path), pax_path = xstrndup(eq + 1, vl);
                            else if ((size_t)(eq - kv) == 8 && !memcmp(kv, "linkpath", 8)) free(pax_link), pax_link = xstrndup(eq + 1, vl);
                            else if ((size_t)(eq - kv) == 4 && !memcmp(kv, "size", 4)) {
                                char *t = xstrndup(eq + 1, vl);
                                char *e2;
                                long long v = strtoll(t, &e2, 10);
                                if (!*e2 && v >= 0) pax_size = v;
                                free(t);
                            }
                        }
                    }
                    p += reclen;
                }
            }
            off = next;
            continue;
        }
        char *name;
        if (longname) name = xstrdup(longname);
        else if (pax_path) name = xstrdup(pax_path);
        else {
            size_t nl = field_len(h, 100);
            if (!memcmp(h + 257, "ustar\0", 6) && h[345]) {
                size_t pl = field_len(h + 345, 155);
                name = xasprintf("%.*s/%.*s", (int)pl, (const char *)h + 345, (int)nl, (const char *)h);
            } else {
                name = xstrndup((const char *)h, nl);
            }
        }
        char *lname = longlink ? xstrdup(longlink) : pax_link ? xstrdup(pax_link) : xstrndup((const char *)h + 157, field_len(h + 157, 100));
        size_t namelen = strlen(name);
        bool dir = type == '5' || (namelen && name[namelen - 1] == '/' && (type == '0' || type == 0));
        ar_ent_t e = {0};
        bool add = true;
        if (dir) {
            e.kind = AE_DIR;
            e.is_dir = true;
        } else if (type == '0' || type == 0 || type == '7') {
            e.kind = AE_BUF;
            e.off = data;
            e.size = size;
        } else if (type == '1' || type == '2') {
            e.kind = AE_LINK;
            e.raw_link = xstrdup(lname);
            char *np = ar_norm(name, namelen);
            e.link = np ? link_target(np, lname, type == '1') : NULL;
            free(np);
        } else {
            add = false; /* devices, fifos, ... */
        }
        if (add && ar_add(a, name, namelen, e, err) != 0) rc = -1;
        free(name);
        free(lname);
        free(longname), longname = NULL;
        free(longlink), longlink = NULL;
        free(pax_path), pax_path = NULL;
        free(pax_link), pax_link = NULL;
        pax_size = -1;
        if (rc) break;
        off = next;
    }
    free(longname);
    free(longlink);
    free(pax_path);
    free(pax_link);
    /* link entries take the kind of their target */
    for (size_t i = 0; i < a->ents.n && !rc; i++) {
        ar_ent_t *e = &a->ents.v[i];
        if (e->kind != AE_LINK || !e->link) continue;
        char *r = ar_resolve(a, e->path);
        ar_ent_t *t = r && *r ? ar_get(a, r) : NULL;
        if ((r && !*r) || (t && t->is_dir)) e->is_dir = true;
        else if (t) e->size = t->size;
        free(r);
    }
    return rc;
}

static const char *ar_format_for(const char *path) {
    static const struct { const char *ext, *fmt; } T[] = {
        {".tar.gz", "tar.gz"}, {".tgz", "tar.gz"}, {".tar", "tar"},  {".gz", "gz"},   {".zip", "zip"},
        {".jar", "zip"},       {".war", "zip"},    {".ear", "zip"},  {".apk", "zip"},  {".whl", "zip"},
        {".ipa", "zip"},       {".xpi", "zip"},    {".vsix", "zip"}, {".nupkg", "zip"}, {".cbz", "zip"},
    };
    size_t n = strlen(path);
    for (size_t i = 0; i < ARRAY_LEN(T); i++) {
        size_t el = strlen(T[i].ext);
        if (n > el && !strcasecmp(path + n - el, T[i].ext)) return T[i].fmt;
    }
    return NULL;
}

static char *read_all_capped(const char *path, size_t cap, size_t *len, char **err) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *err = xasprintf("Cannot open archive: %s", strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size > cap) {
        close(fd);
        char *s = rf_fmt_bytes((int64_t)st.st_size), *l = rf_fmt_bytes((int64_t)cap);
        *err = xasprintf("Archive is too large to read in memory (%s > %s limit)", s, l);
        free(s);
        free(l);
        return NULL;
    }
    size_t n = (size_t)st.st_size;
    char *buf = xmalloc(n + 1);
    bool ok = n == 0 || pread_full(fd, buf, n, 0);
    close(fd);
    if (!ok) {
        free(buf);
        *err = xstrdup("Cannot read archive");
        return NULL;
    }
    buf[n] = 0;
    *len = n;
    return buf;
}

rf_archive_t *rf_archive_open(const char *abs_path, char **err) {
    const char *fmt = ar_format_for(abs_path);
    if (!fmt) {
        const char *ext = strrchr(path_basename(abs_path), '.');
        *err = xasprintf("Unsupported archive format '%s' (omc reads zip, tar, tar.gz/tgz and gz archives)",
                         ext ? ext + 1 : abs_path);
        return NULL;
    }
    rf_archive_t *a = xcalloc(1, sizeof *a);
    snprintf(a->fmt, sizeof a->fmt, "%s", fmt);
    a->fd = -1;
    hmap_init(&a->idx);
    int rc;
    if (!strcmp(fmt, "zip")) {
        a->fd = open(abs_path, O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (a->fd < 0 || fstat(a->fd, &st) != 0) {
            *err = xasprintf("Cannot open archive: %s", strerror(errno));
            rf_archive_free(a);
            return NULL;
        }
        a->fsize = (uint64_t)st.st_size;
        rc = zip_index(a, err);
    } else {
        size_t n = 0;
        char *raw = read_all_capped(abs_path, AR_MAX_IN_MEMORY, &n, err);
        if (!raw) {
            rf_archive_free(a);
            return NULL;
        }
        if (!strcmp(fmt, "tar")) {
            a->buf = raw;
            a->buflen = n;
            rc = tar_index(a, err);
        } else {
            size_t m = 0;
            char *inner = inflate_buf((const uint8_t *)raw, n, false, AR_MAX_IN_MEMORY, 0, &m, err);
            free(raw);
            if (!inner) {
                rf_archive_free(a);
                return NULL;
            }
            a->buf = inner;
            a->buflen = m;
            if (tar_sniff((const uint8_t *)inner, m)) {
                rc = tar_index(a, err);
            } else if (!strcmp(fmt, "tar.gz")) {
                *err = xstrdup("Invalid tar archive: bad header checksum");
                rc = -1;
            } else {
                /* single-stream .gz: one member named after the archive stem */
                const char *base = path_basename(abs_path);
                size_t bl = strlen(base);
                char *stem = bl > 3 ? xstrndup(base, bl - 3) : xstrdup("data");
                ar_ent_t e = {0};
                e.kind = AE_BUF;
                e.size = (int64_t)m;
                rc = ar_add(a, stem, strlen(stem), e, err);
                free(stem);
            }
        }
    }
    if (rc != 0) {
        rf_archive_free(a);
        return NULL;
    }
    ar_ensure_parents(a);
    return a;
}

void rf_archive_free(rf_archive_t *a) {
    if (!a) return;
    if (a->fd >= 0) close(a->fd);
    free(a->buf);
    for (size_t i = 0; i < a->ents.n; i++) ar_ent_free(&a->ents.v[i]);
    vec_free(&a->ents);
    hmap_free(&a->idx, NULL);
    free(a);
}

char *rf_archive_read(rf_archive_t *a, const char *sub, size_t *len, char **norm_path, char **err) {
    char *norm = ar_norm(sub ? sub : "", sub ? strlen(sub) : 0);
    if (!norm || !*norm) {
        *err = xstrdup(norm ? "Archive file path is required" : "Archive path cannot contain '..'");
        free(norm);
        return NULL;
    }
    char *res = ar_resolve(a, norm);
    ar_ent_t *e = res && *res ? ar_get(a, res) : NULL;
    char *out = NULL;
    if (res && !*res) {
        *err = xasprintf("Archive path '%s' is a directory", norm);
    } else if (!e) {
        *err = xasprintf("Archive file '%s' not found", norm);
    } else if (e->is_dir) {
        *err = xasprintf("Archive path '%s' is a directory", norm);
    } else if (e->kind == AE_LINK) {
        *err = xasprintf("Archive symlink '%s' cannot be materialized from target '%s'", norm,
                         e->raw_link ? e->raw_link : "");
    } else if (e->size < 0 || e->size > (int64_t)AR_MAX_MEMBER) {
        char *s = rf_fmt_bytes(e->size);
        *err = xasprintf("Archive member '%s' is too large to extract in memory (%s > 64.0MB limit)", norm, s);
        free(s);
    } else if (e->kind == AE_ZIP) {
        out = zip_read(a, e, len, err);
    } else if (e->kind == AE_BUF) {
        if (e->off > a->buflen || (uint64_t)e->size > a->buflen - e->off) {
            *err = xasprintf("Archive member '%s' is truncated", norm);
        } else {
            out = xmalloc((size_t)e->size + 1);
            memcpy(out, a->buf + e->off, (size_t)e->size);
            out[e->size] = 0;
            *len = (size_t)e->size;
        }
    } else {
        *err = xasprintf("Archive file '%s' has no readable storage", norm);
    }
    free(res);
    if (out && norm_path) *norm_path = norm;
    else free(norm);
    return out;
}

/* ---------- SQLite: on-disk format ---------- */

#define SQ_MAX_DEPTH 64
#define SQ_MAX_PAYLOAD (64u << 20)
#define SQ_MAX_FIELDS 32767
#define SQ_DEFAULT_QUERY_LIMIT 20
#define SQ_DEFAULT_SAMPLE 5
#define SQ_MAX_QUERY_LIMIT 500
#define SQ_MAX_RAW_ROWS 1000
#define SQ_ROW_COUNT_PROBE_CAP 50000
#define SQ_MAX_SORT_ROWS 200000
#define SQ_RENDER_WIDTH 120
#define SQ_COL_WIDTH 40
#define SQ_MIN_COL 3
#define SQ_MAX_QUERY_TOKENS 4096

enum { SV_NULL, SV_INT, SV_REAL, SV_TEXT, SV_BLOB };
typedef struct {
    uint8_t t;
    int64_t i;
    double d;
    char *s; /* TEXT (NUL-terminated) / BLOB bytes, owned */
    size_t n;
} sqv_t;

static void sqv_free(sqv_t *v) {
    free(v->s);
    v->s = NULL;
    v->t = SV_NULL;
}
static sqv_t sqv_null(void) { sqv_t v = {0}; return v; }
static sqv_t sqv_int(int64_t i) { sqv_t v = {0}; v.t = SV_INT; v.i = i; return v; }
static sqv_t sqv_real(double d) { sqv_t v = {0}; v.t = SV_REAL; v.d = d; return v; }
static sqv_t sqv_textn(const char *s, size_t n) {
    sqv_t v = {0};
    v.t = SV_TEXT;
    v.s = xmalloc(n + 1);
    memcpy(v.s, s, n);
    v.s[n] = 0;
    v.n = n;
    return v;
}
static sqv_t sqv_text_own(char *s) { sqv_t v = {0}; v.t = SV_TEXT; v.s = s; v.n = strlen(s); return v; }
static sqv_t sqv_copy(const sqv_t *o) {
    sqv_t v = *o;
    if (o->t == SV_TEXT || o->t == SV_BLOB) {
        v.s = xmalloc(o->n + 1);
        if (o->n) memcpy(v.s, o->s, o->n);
        v.s[o->n] = 0;
    } else {
        v.s = NULL;
    }
    return v;
}

typedef struct {
    int fd, walfd;
    uint64_t fsize;
    uint32_t psz, usable, npages;
    int enc;
    /* WAL overlay: open-addressed pgno -> frame data offset */
    uint32_t *wkey;
    uint64_t *woff;
    size_t wcap;
    uint64_t visits, visit_cap;
    char *err;
} sqdb_t;

static void sq_set_err(sqdb_t *db, const char *msg) {
    if (!db->err) db->err = xstrdup(msg);
}

static uint64_t *wal_slot(sqdb_t *db, uint32_t pg, bool insert) {
    if (!db->wcap) return NULL;
    size_t m = db->wcap - 1, h = (pg * 2654435761u) & m;
    while (db->wkey[h]) {
        if (db->wkey[h] == pg) return &db->woff[h];
        h = (h + 1) & m;
    }
    if (!insert) return NULL;
    db->wkey[h] = pg;
    return &db->woff[h];
}

static void wal_cksum(bool be, const uint8_t *d, size_t n, uint32_t *s0, uint32_t *s1) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint32_t x0 = be ? rd32be(d + i) : rd32le(d + i), x1 = be ? rd32be(d + i + 4) : rd32le(d + i + 4);
        *s0 += x0 + *s1;
        *s1 += x1 + *s0;
    }
}

/* overlay committed frames of "<db>-wal" so live WAL-mode databases read current data */
static void sq_load_wal(sqdb_t *db, const char *path) {
    char *wp = xasprintf("%s-wal", path);
    int fd = open(wp, O_RDONLY | O_CLOEXEC);
    free(wp);
    if (fd < 0) return;
    struct stat st;
    uint8_t h[32];
    if (fstat(fd, &st) != 0 || st.st_size < 32 + 24 || !pread_full(fd, h, 32, 0)) {
        close(fd);
        return;
    }
    uint32_t magic = rd32be(h);
    if ((magic & 0xFFFFFFFEu) != 0x377f0682u || rd32be(h + 8) != db->psz) {
        close(fd);
        return;
    }
    bool be = magic & 1;
    uint32_t s0 = 0, s1 = 0;
    wal_cksum(be, h, 24, &s0, &s1);
    if (s0 != rd32be(h + 24) || s1 != rd32be(h + 28)) {
        close(fd);
        return;
    }
    uint32_t salt1 = rd32be(h + 16), salt2 = rd32be(h + 20);
    uint64_t fsz = (uint64_t)st.st_size, frame = 24 + (uint64_t)db->psz;
    uint64_t nframes = (fsz - 32) / frame;
    uint8_t *page = xmalloc(db->psz);
    VEC(uint32_t) pend_pg = {0};
    VEC(uint64_t) pend_off = {0};
    db->wcap = 1024;
    while (db->wcap < nframes * 2 + 2 && db->wcap < ((size_t)1 << 26)) db->wcap <<= 1;
    db->wkey = xcalloc(db->wcap, sizeof(uint32_t));
    db->woff = xcalloc(db->wcap, sizeof(uint64_t));
    size_t used = 0;
    for (uint64_t f = 0; f < nframes; f++) {
        uint64_t off = 32 + f * frame;
        uint8_t fh[24];
        if (!pread_full(fd, fh, 24, off) || !pread_full(fd, page, db->psz, off + 24)) break;
        if (rd32be(fh + 8) != salt1 || rd32be(fh + 12) != salt2) break;
        wal_cksum(be, fh, 8, &s0, &s1);
        wal_cksum(be, page, db->psz, &s0, &s1);
        if (s0 != rd32be(fh + 16) || s1 != rd32be(fh + 20)) break;
        uint32_t pg = rd32be(fh);
        if (!pg) break;
        vec_push(&pend_pg, pg);
        vec_push(&pend_off, off + 24);
        uint32_t commit = rd32be(fh + 4);
        if (commit) {
            for (size_t k = 0; k < pend_pg.n; k++) {
                if (used * 2 + 2 >= db->wcap) break;
                uint64_t *slot = wal_slot(db, pend_pg.v[k], false);
                if (!slot) {
                    slot = wal_slot(db, pend_pg.v[k], true);
                    used++;
                }
                *slot = pend_off.v[k];
            }
            pend_pg.n = pend_off.n = 0;
            db->npages = commit;
        }
    }
    vec_free(&pend_pg);
    vec_free(&pend_off);
    free(page);
    if (!used) {
        free(db->wkey);
        free(db->woff);
        db->wkey = NULL;
        db->woff = NULL;
        db->wcap = 0;
        close(fd);
        return;
    }
    db->walfd = fd;
}

static bool sq_page(sqdb_t *db, uint32_t pg, uint8_t *buf) {
    if (pg == 0 || pg > db->npages) return false;
    uint64_t *w = wal_slot(db, pg, false);
    if (w) return pread_full(db->walfd, buf, db->psz, *w);
    uint64_t off = (uint64_t)(pg - 1) * db->psz;
    if (off + db->psz > db->fsize) return false;
    return pread_full(db->fd, buf, db->psz, off);
}

static void sq_close(sqdb_t *db) {
    if (db->fd >= 0) close(db->fd);
    if (db->walfd >= 0) close(db->walfd);
    free(db->wkey);
    free(db->woff);
    free(db->err);
}

static int sq_open(sqdb_t *db, const char *path, char **err) {
    memset(db, 0, sizeof *db);
    db->fd = db->walfd = -1;
    db->fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    uint8_t h[100];
    if (db->fd < 0 || fstat(db->fd, &st) != 0) {
        *err = xasprintf("unable to open database file: %s", strerror(errno));
        return -1;
    }
    db->fsize = (uint64_t)st.st_size;
    if (db->fsize < 100 || !pread_full(db->fd, h, 100, 0) || memcmp(h, "SQLite format 3", 16) != 0) {
        *err = xstrdup("file is not a database");
        return -1;
    }
    uint32_t psz = rd16be(h + 16);
    if (psz == 1) psz = 65536;
    if (psz < 512 || psz > 65536 || (psz & (psz - 1))) {
        *err = xstrdup("database disk image is malformed (bad page size)");
        return -1;
    }
    db->psz = psz;
    db->usable = psz - h[20];
    if (db->usable < 480) {
        *err = xstrdup("database disk image is malformed (bad reserved size)");
        return -1;
    }
    uint32_t enc = rd32be(h + 56);
    db->enc = enc >= 1 && enc <= 3 ? (int)enc : 1;
    uint64_t by_size = db->fsize / psz;
    uint32_t hdr_pages = rd32be(h + 28);
    db->npages = (hdr_pages && rd32be(h + 92) == rd32be(h + 24) && hdr_pages <= by_size) ? hdr_pages : (uint32_t)MIN(by_size, (uint64_t)UINT32_MAX);
    if (h[18] == 2 || h[19] == 2) sq_load_wal(db, path);
    if (db->npages == 0) {
        *err = xstrdup("database is empty");
        return -1;
    }
    return 0;
}

/* varint: returns bytes consumed, 0 when it runs past end */
static int sq_varint(const uint8_t *p, const uint8_t *end, uint64_t *v) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (p + i >= end) return 0;
        r = (r << 7) | (p[i] & 0x7f);
        if (!(p[i] & 0x80)) {
            *v = r;
            return i + 1;
        }
    }
    if (p + 8 >= end) return 0;
    *v = (r << 8) | p[8];
    return 9;
}

/* assemble a cell payload of P bytes starting at `c` (local part), following overflow pages */
static uint8_t *sq_payload(sqdb_t *db, const uint8_t *page, const uint8_t *c, uint64_t P, bool index, size_t *outlen) {
    uint64_t U = db->usable;
    uint64_t X = index ? ((U - 12) * 64 / 255) - 23 : U - 35;
    uint64_t M = ((U - 12) * 32 / 255) - 23;
    uint64_t local = P;
    if (P > X) {
        uint64_t K = M + (P - M) % (U - 4);
        local = K <= X ? K : M;
    }
    const uint8_t *pend = page + db->usable;
    if (c > pend || local > (uint64_t)(pend - c)) {
        sq_set_err(db, "database disk image is malformed (cell overflows page)");
        return NULL;
    }
    if (P > SQ_MAX_PAYLOAD) {
        sq_set_err(db, "row is too large to read (> 64MB)");
        return NULL;
    }
    uint8_t *out = xmalloc(P + 1);
    memcpy(out, c, local);
    uint64_t got = local;
    if (got < P) {
        if ((uint64_t)(pend - c) < local + 4) {
            free(out);
            sq_set_err(db, "database disk image is malformed (overflow pointer)");
            return NULL;
        }
        uint32_t next = rd32be(c + local);
        uint8_t *ov = xmalloc(db->psz);
        uint64_t guard = P / (U - 4) + 2;
        while (got < P) {
            if (!guard-- || !sq_page(db, next, ov)) {
                free(ov);
                free(out);
                sq_set_err(db, "database disk image is malformed (overflow chain)");
                return NULL;
            }
            uint64_t take = MIN(P - got, U - 4);
            memcpy(out + got, ov + 4, take);
            got += take;
            next = rd32be(ov);
        }
        free(ov);
    }
    out[P] = 0;
    *outlen = (size_t)P;
    return out;
}

static char *utf16_to_utf8(const uint8_t *p, size_t n, bool be) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i] | p[i + 1] << 8);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
            uint32_t l = be ? (uint32_t)(p[i + 2] << 8 | p[i + 3]) : (uint32_t)(p[i + 2] | p[i + 3] << 8);
            if (l >= 0xDC00 && l < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
                i += 2;
            } else {
                u = 0xFFFD;
            }
        } else if (u >= 0xD800 && u < 0xE000) {
            u = 0xFFFD;
        }
        sb_append_utf8(&b, u);
    }
    return sb_detach(&b);
}

typedef VEC(sqv_t) sqvals_t;

static void sqvals_clear(sqvals_t *v) {
    for (size_t i = 0; i < v->n; i++) sqv_free(&v->v[i]);
    v->n = 0;
}

static int sq_record(sqdb_t *db, const uint8_t *p, size_t n, sqvals_t *out) {
    uint64_t hsz;
    int k = sq_varint(p, p + n, &hsz);
    if (!k || hsz > n || hsz < (uint64_t)k) {
        sq_set_err(db, "database disk image is malformed (record header)");
        return -1;
    }
    const uint8_t *hp = p + k, *he = p + hsz, *body = p + hsz, *end = p + n;
    while (hp < he) {
        uint64_t st;
        int m = sq_varint(hp, he, &st);
        if (!m) break;
        hp += m;
        if (out->n >= SQ_MAX_FIELDS) break;
        sqv_t v = {0};
        size_t len = 0;
        switch (st) {
        case 0: break;
        case 1: case 2: case 3: case 4: case 5: case 6: {
            static const size_t L[] = {0, 1, 2, 3, 4, 6, 8};
            len = L[st];
            if ((size_t)(end - body) < len) goto bad;
            uint64_t u = 0;
            for (size_t i = 0; i < len; i++) u = (u << 8) | body[i];
            if (len < 8 && (u >> (len * 8 - 1)) & 1) u |= ~(uint64_t)0 << (len * 8);
            v.t = SV_INT;
            v.i = (int64_t)u;
            break;
        }
        case 7: {
            len = 8;
            if ((size_t)(end - body) < 8) goto bad;
            uint64_t u = 0;
            for (int i = 0; i < 8; i++) u = (u << 8) | body[i];
            v.t = SV_REAL;
            memcpy(&v.d, &u, 8);
            break;
        }
        case 8: v.t = SV_INT; v.i = 0; break;
        case 9: v.t = SV_INT; v.i = 1; break;
        case 10: case 11: goto bad;
        default:
            len = (size_t)((st - 12) / 2);
            if ((uint64_t)(end - body) < (st - 12) / 2) goto bad;
            if (st & 1) {
                v.t = SV_TEXT;
                if (db->enc == 1) {
                    v.s = xstrndup((const char *)body, len);
                    v.n = strlen(v.s);
                } else {
                    v.s = utf16_to_utf8(body, len, db->enc == 3);
                    v.n = strlen(v.s);
                }
            } else {
                v.t = SV_BLOB;
                v.s = xmalloc(len + 1);
                if (len) memcpy(v.s, body, len);
                v.s[len] = 0;
                v.n = len;
            }
        }
        body += len;
        vec_push(out, v);
    }
    return 0;
bad:
    sq_set_err(db, "database disk image is malformed (record body)");
    return -1;
}

/* row callback: values are owned by the scanner (steal by moving them out and setting .s = NULL); return 1 to stop */
typedef int (*sq_cb)(void *ud, int64_t rowid, sqvals_t *rec);

static bool sq_visit(sqdb_t *db) {
    if (++db->visits > db->visit_cap) {
        sq_set_err(db, "database disk image is malformed (b-tree cycle)");
        return false;
    }
    return true;
}

/* in-order walk of a table (rowid) or index (WITHOUT ROWID) b-tree. 0 = done, 1 = stopped, -1 = error */
static int sq_walk(sqdb_t *db, uint32_t pg, int depth, bool index, sq_cb cb, void *ud) {
    if (depth > SQ_MAX_DEPTH || !sq_visit(db)) {
        sq_set_err(db, "database disk image is malformed (b-tree too deep)");
        return -1;
    }
    uint8_t *buf = xmalloc(db->psz);
    int rc = 0;
    sqvals_t rec = {0};
    if (!sq_page(db, pg, buf)) {
        sq_set_err(db, "database disk image is malformed (page out of range)");
        rc = -1;
        goto out;
    }
    size_t hdr = pg == 1 ? 100 : 0;
    uint8_t type = buf[hdr];
    bool interior = type == 2 || type == 5;
    if (index ? (type != 2 && type != 10) : (type != 5 && type != 13)) {
        sq_set_err(db, "database disk image is malformed (unexpected page type)");
        rc = -1;
        goto out;
    }
    size_t hl = interior ? 12 : 8, nc = rd16be(buf + hdr + 3);
    if (hdr + hl + nc * 2 > db->usable) {
        sq_set_err(db, "database disk image is malformed (cell count)");
        rc = -1;
        goto out;
    }
    const uint8_t *end = buf + db->usable;
    for (size_t i = 0; i < nc && !rc; i++) {
        size_t off = rd16be(buf + hdr + hl + 2 * i);
        if (off < hdr + hl || off + 4 > db->usable) {
            sq_set_err(db, "database disk image is malformed (cell pointer)");
            rc = -1;
            break;
        }
        const uint8_t *c = buf + off;
        if (interior) {
            rc = sq_walk(db, rd32be(c), depth + 1, index, cb, ud);
            if (rc || !index) continue;
            c += 4;
        }
        uint64_t P, rowid = 0;
        int k = sq_varint(c, end, &P);
        if (!k) {
            sq_set_err(db, "database disk image is malformed (cell header)");
            rc = -1;
            break;
        }
        c += k;
        if (!index) {
            k = sq_varint(c, end, &rowid);
            if (!k) {
                sq_set_err(db, "database disk image is malformed (rowid)");
                rc = -1;
                break;
            }
            c += k;
        }
        size_t plen;
        uint8_t *pl = sq_payload(db, buf, c, P, index, &plen);
        if (!pl) {
            rc = -1;
            break;
        }
        sqvals_clear(&rec);
        int r = sq_record(db, pl, plen, &rec);
        free(pl);
        if (r) {
            rc = -1;
            break;
        }
        if (cb(ud, (int64_t)rowid, &rec)) rc = 1;
    }
    if (!rc && interior) rc = sq_walk(db, rd32be(buf + hdr + 8), depth + 1, index, cb, ud);
out:
    sqvals_clear(&rec);
    vec_free(&rec);
    free(buf);
    return rc;
}

static int sq_scan(sqdb_t *db, uint32_t root, bool index, sq_cb cb, void *ud) {
    db->visits = 0;
    db->visit_cap = (uint64_t)db->npages + 16;
    return sq_walk(db, root, 0, index, cb, ud);
}

/* count entries without decoding records; stops once *cnt > cap */
static int sq_count_walk(sqdb_t *db, uint32_t pg, int depth, bool index, uint64_t cap, uint64_t *cnt) {
    if (depth > SQ_MAX_DEPTH || !sq_visit(db)) return -1;
    uint8_t *buf = xmalloc(db->psz);
    int rc = 0;
    if (!sq_page(db, pg, buf)) {
        free(buf);
        sq_set_err(db, "database disk image is malformed (page out of range)");
        return -1;
    }
    size_t hdr = pg == 1 ? 100 : 0;
    uint8_t type = buf[hdr];
    size_t nc = rd16be(buf + hdr + 3);
    if (index ? (type != 2 && type != 10) : (type != 5 && type != 13)) {
        free(buf);
        sq_set_err(db, "database disk image is malformed (unexpected page type)");
        return -1;
    }
    if (type == 13 || type == 10 || type == 2) *cnt += nc;
    if (type == 5 || type == 2) {
        if (hdr + 12 + nc * 2 > db->usable) {
            free(buf);
            sq_set_err(db, "database disk image is malformed (cell count)");
            return -1;
        }
        for (size_t i = 0; i <= nc && !rc && *cnt <= cap; i++) {
            uint32_t child;
            if (i == nc) child = rd32be(buf + hdr + 8);
            else {
                size_t off = rd16be(buf + hdr + 12 + 2 * i);
                if (off + 4 > db->usable) {
                    rc = -1;
                    break;
                }
                child = rd32be(buf + off);
            }
            rc = sq_count_walk(db, child, depth + 1, index, cap, cnt);
        }
    }
    free(buf);
    return rc;
}

static int sq_count(sqdb_t *db, uint32_t root, bool index, uint64_t cap, uint64_t *cnt) {
    *cnt = 0;
    db->visits = 0;
    db->visit_cap = (uint64_t)db->npages + 16;
    return sq_count_walk(db, root, 0, index, cap, cnt);
}

/* point lookup by rowid in a table b-tree: 1 found (cb called), 0 absent, -1 error */
static int sq_find_rowid(sqdb_t *db, uint32_t root, int64_t rowid, sq_cb cb, void *ud) {
    uint8_t *buf = xmalloc(db->psz);
    uint32_t pg = root;
    int rc = -1;
    sqvals_t rec = {0};
    for (int depth = 0; depth <= SQ_MAX_DEPTH; depth++) {
        if (!sq_page(db, pg, buf)) break;
        size_t hdr = pg == 1 ? 100 : 0;
        uint8_t type = buf[hdr];
        size_t nc = rd16be(buf + hdr + 3);
        const uint8_t *end = buf + db->usable;
        if (type == 5) {
            if (hdr + 12 + nc * 2 > db->usable) break;
            uint32_t next = rd32be(buf + hdr + 8);
            for (size_t i = 0; i < nc; i++) {
                size_t off = rd16be(buf + hdr + 12 + 2 * i);
                uint64_t key;
                if (off + 4 > db->usable || !sq_varint(buf + off + 4, end, &key)) goto done;
                if ((int64_t)key >= rowid) {
                    next = rd32be(buf + off);
                    break;
                }
            }
            pg = next;
            continue;
        }
        if (type != 13 || hdr + 8 + nc * 2 > db->usable) break;
        rc = 0;
        for (size_t i = 0; i < nc; i++) {
            size_t off = rd16be(buf + hdr + 8 + 2 * i);
            if (off >= db->usable) break;
            const uint8_t *c = buf + off;
            uint64_t P, rid;
            int k = sq_varint(c, end, &P);
            if (!k) break;
            int k2 = sq_varint(c + k, end, &rid);
            if (!k2) break;
            if ((int64_t)rid != rowid) continue;
            size_t plen;
            uint8_t *pl = sq_payload(db, buf, c + k + k2, P, false, &plen);
            if (!pl) {
                rc = -1;
                break;
            }
            int r = sq_record(db, pl, plen, &rec);
            free(pl);
            if (r) {
                rc = -1;
                break;
            }
            cb(ud, rowid, &rec);
            rc = 1;
            break;
        }
        break;
    }
done:
    if (rc < 0) sq_set_err(db, "database disk image is malformed");
    sqvals_clear(&rec);
    vec_free(&rec);
    free(buf);
    return rc;
}

/* ---------- SQL tokens ---------- */

enum { TK_EOF, TK_ID, TK_QID, TK_STR, TK_NUM, TK_BLOB, TK_OP, TK_PARAM };
typedef struct {
    int t;
    size_t pos, len; /* span in source */
    char *v;         /* decoded value (identifier/string text, blob bytes, operator, number text) */
    size_t vn;
} sqtok_t;
typedef VEC(sqtok_t) sqtoks_t;

static void sqtoks_free(sqtoks_t *t) {
    for (size_t i = 0; i < t->n; i++) free(t->v[i].v);
    vec_free(t);
}

static bool is_id_start(unsigned char c) { return isalpha(c) || c == '_' || c >= 0x80; }
static bool is_id_char(unsigned char c) { return isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

static int sql_lex(const char *s, sqtoks_t *out, size_t max_tokens, char **err) {
    memset(out, 0, sizeof *out);
    size_t n = strlen(s), i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) {
            i++;
            continue;
        }
        if (c == '-' && s[i + 1] == '-') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && s[i + 1] == '*') {
            const char *e = strstr(s + i + 2, "*/");
            i = e ? (size_t)(e - s) + 2 : n;
            continue;
        }
        sqtok_t t = {0};
        t.pos = i;
        if ((c == 'x' || c == 'X') && s[i + 1] == '\'') {
            size_t j = i + 2;
            sb_t b = SB_INIT;
            while (j + 1 < n && isxdigit((unsigned char)s[j]) && isxdigit((unsigned char)s[j + 1])) {
                char hx[3] = {s[j], s[j + 1], 0};
                sb_putc(&b, (char)strtol(hx, NULL, 16));
                j += 2;
            }
            if (j >= n || s[j] != '\'') {
                sb_free(&b);
                *err = xasprintf("unrecognized token: \"%.*s\"", (int)MIN(n - i, (size_t)32), s + i);
                sqtoks_free(out);
                return -1;
            }
            t.t = TK_BLOB;
            t.vn = b.len;
            t.v = sb_detach(&b);
            i = j + 1;
        } else if (is_id_start(c)) {
            size_t j = i;
            while (j < n && is_id_char((unsigned char)s[j])) j++;
            t.t = TK_ID;
            t.v = xstrndup(s + i, j - i);
            i = j;
        } else if (c == '\'' || c == '"' || c == '`' || c == '[') {
            char close = c == '[' ? ']' : (char)c;
            size_t j = i + 1;
            sb_t b = SB_INIT;
            bool ok = false;
            while (j < n) {
                if (s[j] == close) {
                    if (close != ']' && s[j + 1] == close) {
                        sb_putc(&b, close);
                        j += 2;
                        continue;
                    }
                    ok = true;
                    break;
                }
                sb_putc(&b, s[j++]);
            }
            if (!ok) {
                sb_free(&b);
                *err = xasprintf("unrecognized token: \"%.*s\"", (int)MIN(n - i, (size_t)32), s + i);
                sqtoks_free(out);
                return -1;
            }
            t.t = c == '\'' ? TK_STR : TK_QID;
            t.vn = b.len;
            t.v = sb_detach(&b);
            i = j + 1;
        } else if (isdigit(c) || (c == '.' && isdigit((unsigned char)s[i + 1]))) {
            size_t j = i;
            if (c == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                j += 2;
                while (j < n && isxdigit((unsigned char)s[j])) j++;
            } else {
                while (j < n && isdigit((unsigned char)s[j])) j++;
                if (j < n && s[j] == '.') {
                    j++;
                    while (j < n && isdigit((unsigned char)s[j])) j++;
                }
                if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                    size_t k = j + 1;
                    if (k < n && (s[k] == '+' || s[k] == '-')) k++;
                    if (k < n && isdigit((unsigned char)s[k])) {
                        while (k < n && isdigit((unsigned char)s[k])) k++;
                        j = k;
                    }
                }
            }
            if (j < n && is_id_char((unsigned char)s[j])) {
                *err = xasprintf("unrecognized token: \"%.*s\"", (int)MIN(n - i, (size_t)32), s + i);
                sqtoks_free(out);
                return -1;
            }
            t.t = TK_NUM;
            t.v = xstrndup(s + i, j - i);
            i = j;
        } else if (c == '?' || ((c == ':' || c == '@' || c == '$') && is_id_start((unsigned char)s[i + 1]))) {
            size_t j = i + 1;
            while (j < n && (is_id_char((unsigned char)s[j]) || isdigit((unsigned char)s[j]))) j++;
            t.t = TK_PARAM;
            t.v = xstrndup(s + i, j - i);
            i = j;
        } else {
            static const char *const OPS2[] = {"||", "<=", ">=", "==", "!=", "<>", "<<", ">>"};
            const char *op = NULL;
            for (size_t k = 0; k < ARRAY_LEN(OPS2); k++)
                if (!strncmp(s + i, OPS2[k], 2)) op = OPS2[k];
            if (!op && strchr("(),;.=<>+-*/%&|~", c)) {
                char one[2] = {(char)c, 0};
                t.v = xstrdup(one);
                i++;
            } else if (op) {
                t.v = xstrdup(op);
                i += 2;
            } else {
                *err = xasprintf("unrecognized token: \"%c\"", c);
                sqtoks_free(out);
                return -1;
            }
            t.t = TK_OP;
        }
        t.len = i - t.pos;
        if (out->n >= max_tokens) { /* bounds AST depth for the recursive evaluator */
            free(t.v);
            sqtoks_free(out);
            *err = xasprintf("SQL is too long for omc's SQLite reader (over %zu tokens)", max_tokens);
            return -1;
        }
        vec_push(out, t);
    }
    sqtok_t eof = {0};
    eof.t = TK_EOF;
    eof.pos = n;
    vec_push(out, eof);
    return 0;
}

/* ---------- table metadata (CREATE TABLE parsing) ---------- */

enum { AFF_NONE, AFF_BLOB, AFF_TEXT, AFF_NUMERIC, AFF_INT, AFF_REAL };

typedef struct {
    char *name;
    char *type;
    int aff;
    bool pk_inline;
    bool gen_virtual;
    bool has_dflt;
    sqv_t dflt;
    int rec_idx; /* field index in the stored record, -1 when not stored */
} sqcol_t;

typedef struct {
    char *name, *sql;
    uint32_t root;
    bool without_rowid, is_virtual;
    VEC(sqcol_t) cols;
    VEC(int) pk;
    int ipk; /* INTEGER PRIMARY KEY (rowid alias) column or -1 */
} sqtab_t;

typedef struct {
    char *type, *name, *tbl, *sql;
    int64_t root;
} sqschema_t;
typedef VEC(sqschema_t) sqschemas_t;

static int type_affinity(const char *type) {
    if (!type || !*type) return AFF_BLOB;
    char *u = str_lower(type);
    int a = strstr(u, "int") ? AFF_INT
            : (strstr(u, "char") || strstr(u, "clob") || strstr(u, "text")) ? AFF_TEXT
            : strstr(u, "blob") ? AFF_BLOB
            : (strstr(u, "real") || strstr(u, "floa") || strstr(u, "doub")) ? AFF_REAL
                                                                              : AFF_NUMERIC;
    free(u);
    return a;
}

static void sqtab_free(sqtab_t *t) {
    free(t->name);
    free(t->sql);
    for (size_t i = 0; i < t->cols.n; i++) {
        free(t->cols.v[i].name);
        free(t->cols.v[i].type);
        sqv_free(&t->cols.v[i].dflt);
    }
    vec_free(&t->cols);
    vec_free(&t->pk);
}

static bool tok_kw(const sqtok_t *t, const char *kw) { return t->t == TK_ID && !strcasecmp(t->v, kw); }
static bool tok_op(const sqtok_t *t, const char *op) { return t->t == TK_OP && !strcmp(t->v, op); }
static bool tok_name(const sqtok_t *t) { return t->t == TK_ID || t->t == TK_QID || t->t == TK_STR; }

static int find_col(const sqtab_t *t, const char *name) {
    for (size_t i = 0; i < t->cols.n; i++)
        if (!strcasecmp(t->cols.v[i].name, name)) return (int)i;
    return -1;
}

static bool parse_literal_default(const sqtok_t *tk, size_t i, size_t e, sqv_t *out) {
    bool neg = false;
    if (i < e && (tok_op(&tk[i], "-") || tok_op(&tk[i], "+"))) neg = tok_op(&tk[i++], "-");
    if (i >= e) return false;
    const sqtok_t *t = &tk[i];
    if (t->t == TK_NUM) {
        char *end;
        errno = 0;
        long long v = strtoll(t->v, &end, 0);
        if (!*end && !errno) *out = sqv_int(neg ? -v : v);
        else *out = sqv_real(neg ? -strtod(t->v, NULL) : strtod(t->v, NULL));
        return true;
    }
    if (neg) return false;
    if (t->t == TK_STR) {
        *out = sqv_textn(t->v, t->vn);
        return true;
    }
    if (t->t == TK_BLOB) {
        sqv_t v = sqv_textn(t->v, t->vn);
        v.t = SV_BLOB;
        *out = v;
        return true;
    }
    if (tok_kw(t, "null")) return *out = sqv_null(), true;
    if (tok_kw(t, "true")) return *out = sqv_int(1), true;
    if (tok_kw(t, "false")) return *out = sqv_int(0), true;
    return false;
}

static const char *const COL_CONSTRAINT_KW[] = {"constraint", "primary", "not",      "null",      "unique", "check",
                                                 "default",    "collate", "references", "generated", "as"};

static bool is_col_constraint(const sqtok_t *t) {
    if (t->t != TK_ID) return false;
    for (size_t i = 0; i < ARRAY_LEN(COL_CONSTRAINT_KW); i++)
        if (!strcasecmp(t->v, COL_CONSTRAINT_KW[i])) return true;
    return false;
}

static void parse_column_def(sqtab_t *tab, const sqtok_t *tk, size_t s, size_t e) {
    sqcol_t c = {0};
    c.name = xstrdup(tk[s].v);
    c.rec_idx = -1;
    sb_t ty = SB_INIT;
    size_t j = s + 1;
    int depth = 0;
    while (j < e && (depth > 0 || !is_col_constraint(&tk[j]))) {
        if (tok_op(&tk[j], "(")) depth++;
        if (tok_op(&tk[j], ")")) depth--;
        if (ty.len && !tok_op(&tk[j], "(") && !tok_op(&tk[j], ")") && !tok_op(&tk[j], ",") && ty.s[ty.len - 1] != '(')
            sb_putc(&ty, ' ');
        sb_append(&ty, tk[j].v);
        j++;
    }
    c.type = sb_detach(&ty);
    c.aff = type_affinity(c.type);
    for (; j < e; j++) {
        if (tok_kw(&tk[j], "primary") && j + 1 < e && tok_kw(&tk[j + 1], "key")) c.pk_inline = true;
        else if (tok_kw(&tk[j], "default") && j + 1 < e) c.has_dflt = parse_literal_default(tk, j + 1, e, &c.dflt);
        else if (tok_kw(&tk[j], "as") && j + 1 < e && tok_op(&tk[j + 1], "(")) {
            int d = 0;
            size_t k = j + 1;
            for (; k < e; k++) {
                if (tok_op(&tk[k], "(")) d++;
                if (tok_op(&tk[k], ")") && --d == 0) break;
            }
            c.gen_virtual = !(k + 1 < e && tok_kw(&tk[k + 1], "stored"));
            j = k;
        }
    }
    vec_push(&tab->cols, c);
}

static int parse_create_table(sqtab_t *tab, const char *sql, char **err) {
    sqtoks_t tk;
    if (sql_lex(sql, &tk, (size_t)1 << 21, err) != 0) return -1;
    size_t i = 0;
    int rc = -1;
    if (!tok_kw(&tk.v[i], "create")) goto bad;
    i++;
    if (tok_kw(&tk.v[i], "temp") || tok_kw(&tk.v[i], "temporary")) i++;
    if (tok_kw(&tk.v[i], "virtual")) {
        tab->is_virtual = true;
        rc = 0;
        goto done;
    }
    if (!tok_kw(&tk.v[i], "table")) goto bad;
    i++;
    if (tok_kw(&tk.v[i], "if") && tok_kw(&tk.v[i + 1], "not") && tok_kw(&tk.v[i + 2], "exists")) i += 3;
    if (!tok_name(&tk.v[i])) goto bad;
    i++;
    if (tok_op(&tk.v[i], ".") && tok_name(&tk.v[i + 1])) i += 2;
    if (!tok_op(&tk.v[i], "(")) goto bad;
    i++;
    size_t close = 0;
    {
        int depth = 1;
        size_t s = i;
        for (size_t j = i; tk.v[j].t != TK_EOF; j++) {
            if (tok_op(&tk.v[j], "(")) depth++;
            else if (tok_op(&tk.v[j], ")")) depth--;
            if ((depth == 1 && tok_op(&tk.v[j], ",")) || depth == 0) {
                if (j > s) {
                    const sqtok_t *f = &tk.v[s];
                    size_t ds = s;
                    if (tok_kw(f, "constraint") && j > s + 2) ds = s + 2;
                    const sqtok_t *g = &tk.v[ds];
                    if (tok_kw(g, "primary") || tok_kw(g, "unique") || tok_kw(g, "check") || tok_kw(g, "foreign") ||
                        ds != s) {
                        if (tok_kw(g, "primary")) {
                            size_t k = ds + 1;
                            while (k < j && !tok_op(&tk.v[k], "(")) k++;
                            bool at_item = true;
                            int d2 = 0;
                            for (k++; k < j; k++) {
                                if (tok_op(&tk.v[k], "(")) d2++;
                                else if (tok_op(&tk.v[k], ")")) {
                                    if (d2-- == 0) break;
                                } else if (d2 == 0 && tok_op(&tk.v[k], ",")) {
                                    at_item = true;
                                    continue;
                                }
                                if (at_item && tok_name(&tk.v[k])) {
                                    int ci = find_col(tab, tk.v[k].v);
                                    if (ci >= 0) vec_push(&tab->pk, ci);
                                }
                                at_item = false;
                            }
                        }
                    } else if (tok_name(f)) {
                        parse_column_def(tab, tk.v, s, j);
                    }
                }
                s = j + 1;
                if (depth == 0) {
                    close = j;
                    break;
                }
            }
        }
    }
    if (!close) goto bad;
    for (size_t j = close; tk.v[j].t != TK_EOF; j++)
        if (tok_kw(&tk.v[j], "without") && tok_kw(&tk.v[j + 1], "rowid")) tab->without_rowid = true;
    if (!tab->pk.n)
        for (size_t c = 0; c < tab->cols.n; c++)
            if (tab->cols.v[c].pk_inline) vec_push(&tab->pk, (int)c);
    rc = 0;
    goto done;
bad:
    *err = xstrdup("unsupported CREATE TABLE statement");
done:
    sqtoks_free(&tk);
    return rc;
}

static void table_layout(sqtab_t *t) {
    t->ipk = -1;
    if (!t->without_rowid && t->pk.n == 1 && !strcasecmp(t->cols.v[t->pk.v[0]].type, "integer")) t->ipk = t->pk.v[0];
    int k = 0;
    if (t->without_rowid) {
        for (size_t i = 0; i < t->pk.n; i++) t->cols.v[t->pk.v[i]].rec_idx = k++;
        for (size_t c = 0; c < t->cols.n; c++) {
            bool in_pk = false;
            for (size_t i = 0; i < t->pk.n; i++) in_pk |= t->pk.v[i] == (int)c;
            if (!in_pk && !t->cols.v[c].gen_virtual) t->cols.v[c].rec_idx = k++;
        }
    } else {
        for (size_t c = 0; c < t->cols.n; c++)
            if (!t->cols.v[c].gen_virtual) t->cols.v[c].rec_idx = k++;
    }
}

static int schema_cb(void *ud, int64_t rowid, sqvals_t *rec) {
    sqschemas_t *s = ud;
    if (rec->n < 5) return 0;
    sqschema_t e = {0};
    e.type = xstrdup(rec->v[0].t == SV_TEXT ? rec->v[0].s : "");
    e.name = xstrdup(rec->v[1].t == SV_TEXT ? rec->v[1].s : "");
    e.tbl = xstrdup(rec->v[2].t == SV_TEXT ? rec->v[2].s : "");
    e.root = rec->v[3].t == SV_INT ? rec->v[3].i : 0;
    e.sql = rec->v[4].t == SV_TEXT ? xstrdup(rec->v[4].s) : NULL;
    vec_push(s, e);
    return 0;
}

static void schemas_free(sqschemas_t *s) {
    for (size_t i = 0; i < s->n; i++) {
        free(s->v[i].type);
        free(s->v[i].name);
        free(s->v[i].tbl);
        free(s->v[i].sql);
    }
    vec_free(s);
}

static bool is_internal_name(const char *n) { return strlen(n) >= 7 && !strncasecmp(n, "sqlite", 6); }

static bool is_master_name(const char *n) {
    return !strcasecmp(n, "sqlite_master") || !strcasecmp(n, "sqlite_schema") || !strcasecmp(n, "sqlite_temp_master") ||
           !strcasecmp(n, "sqlite_temp_schema");
}

/* table metadata by exact name; `allow_internal` admits sqlite_* tables (raw queries). */
static int load_table(sqdb_t *db, sqschemas_t *sch, const char *name, bool allow_internal, sqtab_t *t, char **err) {
    memset(t, 0, sizeof *t);
    if (allow_internal && is_master_name(name)) {
        static const char *const C[] = {"type", "name", "tbl_name", "rootpage", "sql"};
        static const char *const T[] = {"text", "text", "text", "int", "text"};
        t->name = xstrdup(name);
        t->sql = xstrdup("CREATE TABLE sqlite_schema(type text, name text, tbl_name text, rootpage int, sql text)");
        t->root = 1;
        for (size_t i = 0; i < 5; i++) {
            sqcol_t c = {0};
            c.name = xstrdup(C[i]);
            c.type = xstrdup(T[i]);
            c.aff = type_affinity(T[i]);
            vec_push(&t->cols, c);
        }
        table_layout(t);
        return 0;
    }
    for (size_t i = 0; i < sch->n; i++) {
        sqschema_t *e = &sch->v[i];
        if (strcmp(e->type, "table") || strcmp(e->name, name)) continue;
        if (!allow_internal && is_internal_name(e->name)) break;
        t->name = xstrdup(e->name);
        t->sql = e->sql ? xstrdup(e->sql) : NULL;
        if (!e->sql) {
            *err = xasprintf("SQLite schema for table '%s' is unavailable", name);
            return -1;
        }
        char *perr = NULL;
        if (parse_create_table(t, e->sql, &perr) != 0) {
            *err = xasprintf("SQLite table '%s': %s", name, perr);
            free(perr);
            return -1;
        }
        if (t->is_virtual || e->root <= 0) {
            *err = xasprintf("SQLite table '%s' is a virtual table; omc's built-in reader cannot query it", name);
            return -1;
        }
        if (e->root > (int64_t)db->npages) {
            *err = xasprintf("database disk image is malformed (table '%s' root page)", name);
            return -1;
        }
        t->root = (uint32_t)e->root;
        table_layout(t);
        return 0;
    }
    *err = xasprintf("SQLite table '%s' not found", name);
    return -1;
}

/* map a stored record onto declared columns (rowid alias, missing trailing fields -> DEFAULT) */
static void map_row(const sqtab_t *t, int64_t rowid, sqvals_t *rec, sqv_t *row) {
    for (size_t c = 0; c < t->cols.n; c++) {
        const sqcol_t *col = &t->cols.v[c];
        if ((int)c == t->ipk) {
            row[c] = sqv_int(rowid);
        } else if (col->rec_idx >= 0 && (size_t)col->rec_idx < rec->n) {
            row[c] = rec->v[col->rec_idx];
            rec->v[col->rec_idx].s = NULL;
            rec->v[col->rec_idx].t = SV_NULL;
        } else if (col->has_dflt) {
            row[c] = sqv_copy(&col->dflt);
        } else {
            row[c] = sqv_null();
        }
    }
}

/* ---------- value semantics ---------- */

static void fmt_js_number(sb_t *b, double d) {
    if (isnan(d)) {
        sb_append(b, "NaN");
        return;
    }
    if (isinf(d)) {
        sb_append(b, d < 0 ? "-Infinity" : "Infinity");
        return;
    }
    if (d == 0) {
        sb_putc(b, '0');
        return;
    }
    char buf[64];
    int prec;
    for (prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, d);
        if (strtod(buf, NULL) == d) break;
    }
    char digits[32];
    size_t k = 0;
    const char *p = buf;
    bool neg = *p == '-';
    if (neg) p++;
    for (; *p && *p != 'e'; p++)
        if (isdigit((unsigned char)*p) && k < sizeof digits - 1) digits[k++] = *p;
    digits[k] = 0;
    while (k > 1 && digits[k - 1] == '0') digits[--k] = 0;
    int e = *p == 'e' ? atoi(p + 1) : 0;
    int n = e + 1;
    if (neg) sb_putc(b, '-');
    if ((int)k <= n && n <= 21) {
        sb_append(b, digits);
        for (int i = (int)k; i < n; i++) sb_putc(b, '0');
    } else if (0 < n && n <= 21) {
        sb_appendn(b, digits, (size_t)n);
        sb_putc(b, '.');
        sb_append(b, digits + n);
    } else if (-6 < n && n <= 0) {
        sb_append(b, "0.");
        for (int i = 0; i < -n; i++) sb_putc(b, '0');
        sb_append(b, digits);
    } else {
        sb_putc(b, digits[0]);
        if (k > 1) {
            sb_putc(b, '.');
            sb_append(b, digits + 1);
        }
        sb_appendf(b, "e%c%d", n - 1 < 0 ? '-' : '+', abs(n - 1));
    }
}

/* upstream stringifySqliteValue */
static char *sqv_display(const sqv_t *v) {
    switch (v->t) {
    case SV_NULL: return xstrdup("NULL");
    case SV_INT: return xasprintf("%lld", (long long)v->i);
    case SV_REAL: {
        sb_t b = SB_INIT;
        fmt_js_number(&b, v->d);
        return sb_detach(&b);
    }
    case SV_TEXT: return xstrdup(v->s);
    default: {
        char *sz = rf_fmt_bytes((int64_t)v->n);
        char *r = xasprintf("<BLOB %s>", sz);
        free(sz);
        return r;
    }
    }
}

/* SQLite's text form of a value (CAST AS TEXT / || / text affinity) */
static char *sqv_sqltext(const sqv_t *v) {
    switch (v->t) {
    case SV_INT: return xasprintf("%lld", (long long)v->i);
    case SV_REAL: {
        char buf[64];
        snprintf(buf, sizeof buf, "%.15g", v->d);
        if (!strpbrk(buf, ".eEn")) strcat(buf, ".0");
        return xstrdup(buf);
    }
    case SV_TEXT: case SV_BLOB: return xstrndup(v->s, v->n);
    default: return xstrdup("");
    }
}

/* parse text as a number; `whole` requires the entire string (after trimming) to be numeric */
static bool text_to_num(const char *s, bool whole, sqv_t *out) {
    while (isspace((unsigned char)*s)) s++;
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p) && !(*p == '.' && isdigit((unsigned char)p[1]))) {
        if (whole) return false;
        *out = sqv_int(0);
        return true;
    }
    char *e1, *e2;
    errno = 0;
    long long iv = strtoll(s, &e1, 10);
    bool int_ok = !errno;
    double dv = strtod(s, &e2);
    const char *end = e2 > e1 ? e2 : e1;
    if (whole) {
        const char *t = end;
        while (isspace((unsigned char)*t)) t++;
        if (*t) return false;
    }
    if (e2 > e1 || !int_ok) *out = sqv_real(dv);
    else *out = sqv_int(iv);
    return true;
}

static void apply_numeric_aff(sqv_t *v) {
    if (v->t != SV_TEXT) return;
    sqv_t n;
    if (text_to_num(v->s, true, &n)) {
        if (n.t == SV_REAL && n.d == floor(n.d) && fabs(n.d) < 9.2e18) n = sqv_int((int64_t)n.d);
        sqv_free(v);
        *v = n;
    }
}

static void apply_text_aff(sqv_t *v) {
    if (v->t != SV_INT && v->t != SV_REAL) return;
    char *s = sqv_sqltext(v);
    *v = sqv_text_own(s);
}

static int sv_class(const sqv_t *v) {
    return v->t == SV_NULL ? 0 : (v->t == SV_INT || v->t == SV_REAL) ? 1 : v->t == SV_TEXT ? 2 : 3;
}

static int sqv_cmp(const sqv_t *a, const sqv_t *b) {
    int ca = sv_class(a), cb = sv_class(b);
    if (ca != cb) return ca < cb ? -1 : 1;
    if (ca == 0) return 0;
    if (ca == 1) {
        if (a->t == SV_INT && b->t == SV_INT) return a->i < b->i ? -1 : a->i > b->i;
        double x = a->t == SV_INT ? (double)a->i : a->d, y = b->t == SV_INT ? (double)b->i : b->d;
        return x < y ? -1 : x > y;
    }
    int c = memcmp(a->s, b->s, MIN(a->n, b->n));
    if (c) return c < 0 ? -1 : 1;
    return a->n < b->n ? -1 : a->n > b->n;
}

static sqv_t to_numeric(const sqv_t *v) {
    if (v->t == SV_INT || v->t == SV_REAL) return *v;
    if (v->t == SV_TEXT) {
        sqv_t n;
        text_to_num(v->s, false, &n);
        return n;
    }
    if (v->t == SV_BLOB) {
        char *s = xstrndup(v->s, v->n);
        sqv_t n;
        text_to_num(s, false, &n);
        free(s);
        return n;
    }
    return sqv_null();
}

/* -1 = NULL, 0 = false, 1 = true */
static int sqv_truth(const sqv_t *v) {
    if (v->t == SV_NULL) return -1;
    sqv_t n = to_numeric(v);
    return n.t == SV_INT ? n.i != 0 : n.d != 0;
}

/* LIKE (ASCII case-insensitive, % and _) and GLOB (*, ?, [...]) matching on UTF-8 */
static bool pat_match(const char *pat, const char *str, bool like, uint32_t esc) {
    const char *p = pat, *s = str, *star_p = NULL, *star_s = NULL;
    for (;;) {
        if (*p) {
            uint32_t pc;
            int pk = utf8_decode(p, strlen(p), &pc);
            if (like && esc && pc == esc && p[pk]) {
                uint32_t lit;
                int lk = utf8_decode(p + pk, strlen(p + pk), &lit);
                uint32_t sc;
                if (*s) {
                    int sk = utf8_decode(s, strlen(s), &sc);
                    if (sc == lit || (lit < 128 && sc < 128 && tolower((int)lit) == tolower((int)sc))) {
                        p += pk + lk;
                        s += sk;
                        continue;
                    }
                }
            } else if ((like && pc == '%') || (!like && pc == '*')) {
                star_p = p += pk;
                star_s = s;
                continue;
            } else if (*s) {
                uint32_t sc;
                int sk = utf8_decode(s, strlen(s), &sc);
                bool ok;
                int adv = pk;
                if ((like && pc == '_') || (!like && pc == '?')) ok = true;
                else if (!like && pc == '[') {
                    const char *q = p + 1;
                    bool negate = false, hit = false;
                    if (*q == '^') negate = true, q++;
                    bool first = true;
                    while (*q && (first || *q != ']')) {
                        uint32_t lo;
                        int lk = utf8_decode(q, strlen(q), &lo);
                        q += lk;
                        uint32_t hi = lo;
                        if (*q == '-' && q[1] && q[1] != ']') {
                            int hk = utf8_decode(q + 1, strlen(q + 1), &hi);
                            q += 1 + hk;
                        }
                        if (sc >= lo && sc <= hi) hit = true;
                        first = false;
                    }
                    if (*q != ']') return false;
                    adv = (int)(q + 1 - p);
                    ok = hit != negate;
                } else if (like) ok = pc == sc || (pc < 128 && sc < 128 && tolower((int)pc) == tolower((int)sc));
                else ok = pc == sc;
                if (ok) {
                    p += adv;
                    s += sk;
                    continue;
                }
            }
        } else if (!*s) {
            return true;
        }
        if (!star_p || !*star_s) return false;
        uint32_t sc;
        star_s += utf8_decode(star_s, strlen(star_s), &sc);
        s = star_s;
        p = star_p;
    }
}

/* ---------- expressions ---------- */

enum { X_LIT, X_COL, X_ROWID, X_NEG, X_NOT, X_BITNOT, X_BIN, X_IS, X_ISNULL, X_IN, X_BETWEEN, X_LIKE, X_FUNC, X_AGG, X_CAST };

typedef struct sqx {
    int k;
    char op[4];
    bool neg; /* NOT variants */
    sqv_t lit;
    int col, aff;
    char *fname;
    struct sqx *a, *b, *c;
    VEC(struct sqx *) args;
    bool star;
    /* aggregate state */
    int64_t cnt, isum;
    double rsum;
    bool sum_real, sum_any, overflow, has_best;
    sqv_t best;
} sqx_t;

static void sqx_free(sqx_t *x) {
    if (!x) return;
    sqv_free(&x->lit);
    sqv_free(&x->best);
    free(x->fname);
    sqx_free(x->a);
    sqx_free(x->b);
    sqx_free(x->c);
    for (size_t i = 0; i < x->args.n; i++) sqx_free(x->args.v[i]);
    vec_free(&x->args);
    free(x);
}

typedef struct {
    const char *src;
    sqtok_t *t;
    size_t n, i;
    const sqtab_t *tab;
    char *err;
    bool allow_agg;
    int depth;
    VEC(sqx_t *) aggs;
} sqp_t;

static sqtok_t *pk_peek(sqp_t *p) { return &p->t[p->i < p->n ? p->i : p->n - 1]; }
static bool pk_kw(sqp_t *p, const char *kw) {
    if (!tok_kw(pk_peek(p), kw)) return false;
    p->i++;
    return true;
}
static bool pk_op(sqp_t *p, const char *op) {
    if (!tok_op(pk_peek(p), op)) return false;
    p->i++;
    return true;
}
static sqx_t *pk_fail(sqp_t *p, const char *fmt, const char *arg) {
    if (!p->err) p->err = xasprintf(fmt, arg);
    return NULL;
}
static sqx_t *mk(int k) {
    sqx_t *x = xcalloc(1, sizeof *x);
    x->k = k;
    x->col = -1;
    return x;
}
static sqx_t *mk2(int k, const char *op, sqx_t *a, sqx_t *b) {
    sqx_t *x = mk(k);
    snprintf(x->op, sizeof x->op, "%s", op);
    x->a = a;
    x->b = b;
    return x;
}

static sqx_t *px_expr(sqp_t *p);

static const char *const SQ_FUNCS[] = {"lower", "upper", "length", "abs", "coalesce", "ifnull", "nullif", "typeof",
                                       "trim", "ltrim", "rtrim", "substr", "substring", "instr", "replace", "round",
                                       "min", "max", "hex", "unicode", "char", "iif"};
static const char *const SQ_AGGS[] = {"count", "sum", "total", "avg", "min", "max"};

static sqx_t *px_primary(sqp_t *p) {
    sqtok_t *t = pk_peek(p);
    if (t->t == TK_EOF) return pk_fail(p, "incomplete input%s", "");
    if (t->t == TK_PARAM) return pk_fail(p, "SQLite raw queries do not support bound parameters%s", "");
    if (t->t == TK_NUM) {
        p->i++;
        sqx_t *x = mk(X_LIT);
        char *end;
        errno = 0;
        long long v = strtoll(t->v, &end, 0);
        if (!*end && !errno) x->lit = sqv_int(v);
        else x->lit = sqv_real(strtod(t->v, NULL));
        return x;
    }
    if (t->t == TK_STR || t->t == TK_BLOB) {
        p->i++;
        sqx_t *x = mk(X_LIT);
        x->lit = sqv_textn(t->v, t->vn);
        if (t->t == TK_BLOB) x->lit.t = SV_BLOB;
        return x;
    }
    if (tok_op(t, "(")) {
        p->i++;
        if (tok_kw(pk_peek(p), "select")) return pk_fail(p, "subqueries are not supported by omc's SQLite reader%s", "");
        sqx_t *x = px_expr(p);
        if (!x) return NULL;
        if (!pk_op(p, ")")) {
            sqx_free(x);
            return pk_fail(p, "near \"%s\": syntax error", pk_peek(p)->v ? pk_peek(p)->v : "");
        }
        return x;
    }
    if (t->t == TK_ID || t->t == TK_QID) {
        p->i++;
        if (t->t == TK_ID) {
            if (!strcasecmp(t->v, "null")) return mk(X_LIT);
            if (!strcasecmp(t->v, "true") || !strcasecmp(t->v, "false")) {
                sqx_t *x = mk(X_LIT);
                x->lit = sqv_int(!strcasecmp(t->v, "true"));
                return x;
            }
            if (!strcasecmp(t->v, "cast") && tok_op(pk_peek(p), "(")) {
                p->i++;
                sqx_t *x = mk(X_CAST);
                x->a = px_expr(p);
                if (!x->a || !pk_kw(p, "as")) {
                    sqx_free(x);
                    return pk_fail(p, "near \"CAST\": syntax error%s", "");
                }
                sb_t ty = SB_INIT;
                while (pk_peek(p)->t == TK_ID || pk_peek(p)->t == TK_NUM || (tok_op(pk_peek(p), "(") || tok_op(pk_peek(p), ",") ||
                                                                             (tok_op(pk_peek(p), ")") && p->t[p->i + 1].t != TK_EOF && !tok_op(&p->t[p->i + 1], ")") && 0))) {
                    sb_append(&ty, pk_peek(p)->v);
                    sb_putc(&ty, ' ');
                    p->i++;
                }
                x->aff = type_affinity(sb_cstr(&ty));
                sb_free(&ty);
                if (!pk_op(p, ")")) {
                    sqx_free(x);
                    return pk_fail(p, "near \"CAST\": syntax error%s", "");
                }
                return x;
            }
            if (!strcasecmp(t->v, "case") || !strcasecmp(t->v, "exists") || !strcasecmp(t->v, "select"))
                return pk_fail(p, "%s expressions are not supported by omc's SQLite reader", t->v);
        }
        if (t->t == TK_ID && tok_op(pk_peek(p), "(")) {
            p->i++;
            char *fname = str_lower(t->v);
            sqx_t *x = mk(X_FUNC);
            x->fname = fname;
            if (pk_op(p, "*")) {
                x->star = true;
            } else if (!tok_op(pk_peek(p), ")")) {
                if (pk_kw(p, "distinct")) {
                    sqx_free(x);
                    return pk_fail(p, "DISTINCT aggregates are not supported by omc's SQLite reader%s", "");
                }
                do {
                    sqx_t *a = px_expr(p);
                    if (!a) {
                        sqx_free(x);
                        return NULL;
                    }
                    vec_push(&x->args, a);
                } while (pk_op(p, ","));
            }
            if (!pk_op(p, ")")) {
                sqx_free(x);
                return pk_fail(p, "near \"%s\": syntax error", pk_peek(p)->v ? pk_peek(p)->v : "");
            }
            bool is_agg = false;
            for (size_t i = 0; i < ARRAY_LEN(SQ_AGGS); i++)
                if (!strcmp(fname, SQ_AGGS[i])) is_agg = true;
            if ((!strcmp(fname, "min") || !strcmp(fname, "max")) && x->args.n != 1) is_agg = false;
            if (is_agg) {
                if (!p->allow_agg) {
                    sqx_free(x);
                    return pk_fail(p, "misuse of aggregate function %s()", fname);
                }
                if ((!x->star && x->args.n != 1) || (x->star && strcmp(fname, "count"))) {
                    sqx_free(x);
                    return pk_fail(p, "wrong number of arguments to function %s()", fname);
                }
                x->k = X_AGG;
                vec_push(&p->aggs, x);
                return x;
            }
            bool known = false;
            for (size_t i = 0; i < ARRAY_LEN(SQ_FUNCS); i++)
                if (!strcmp(fname, SQ_FUNCS[i])) known = true;
            if (!known || x->star) {
                char *nm = xstrdup(fname);
                sqx_free(x);
                pk_fail(p, "no such function: %s", nm);
                free(nm);
                return NULL;
            }
            return x;
        }
        /* qualified name: tbl.col */
        const char *name = t->v;
        if (tok_op(pk_peek(p), ".") && (p->t[p->i + 1].t == TK_ID || p->t[p->i + 1].t == TK_QID)) {
            p->i++;
            name = pk_peek(p)->v;
            p->i++;
        }
        int ci = p->tab ? find_col(p->tab, name) : -1;
        if (ci >= 0) {
            sqx_t *x = mk(X_COL);
            x->col = ci;
            x->aff = p->tab->cols.v[ci].aff;
            return x;
        }
        if (p->tab && !p->tab->without_rowid &&
            (!strcasecmp(name, "rowid") || !strcasecmp(name, "_rowid_") || !strcasecmp(name, "oid"))) {
            sqx_t *x = mk(X_ROWID);
            x->aff = AFF_INT;
            return x;
        }
        if (t->t == TK_QID && t->len && p->src[t->pos] == '"') { /* "string" fallback */
            sqx_t *x = mk(X_LIT);
            x->lit = sqv_textn(t->v, t->vn);
            return x;
        }
        return pk_fail(p, "no such column: %s", name);
    }
    return pk_fail(p, "near \"%s\": syntax error", t->v ? t->v : "");
}

static sqx_t *px_unary(sqp_t *p) {
    if (++p->depth > 200) return pk_fail(p, "expression nested too deeply%s", "");
    sqx_t *r;
    if (pk_op(p, "-")) {
        sqx_t *a = px_unary(p);
        r = a ? mk2(X_NEG, "-", a, NULL) : NULL;
    } else if (pk_op(p, "+")) {
        r = px_unary(p);
    } else if (pk_op(p, "~")) {
        sqx_t *a = px_unary(p);
        r = a ? mk2(X_BITNOT, "~", a, NULL) : NULL;
    } else {
        r = px_primary(p);
        if (r && pk_kw(p, "collate")) {
            sqtok_t *c = pk_peek(p);
            if (!tok_name(c) || strcasecmp(c->v, "binary")) {
                sqx_free(r);
                return pk_fail(p, "COLLATE %s is not supported by omc's SQLite reader", c->v ? c->v : "");
            }
            p->i++;
        }
    }
    p->depth--;
    return r;
}

#define BINLEVEL(name, next, ...)                                                  \
    static sqx_t *name(sqp_t *p) {                                                 \
        static const char *const ops[] = {__VA_ARGS__};                            \
        sqx_t *l = next(p);                                                        \
        while (l) {                                                                \
            const char *hit = NULL;                                                \
            for (size_t i = 0; i < ARRAY_LEN(ops); i++)                            \
                if (tok_op(pk_peek(p), ops[i])) hit = ops[i];                      \
            if (!hit) break;                                                       \
            p->i++;                                                                \
            sqx_t *r = next(p);                                                    \
            if (!r) {                                                              \
                sqx_free(l);                                                       \
                return NULL;                                                       \
            }                                                                      \
            l = mk2(X_BIN, hit, l, r);                                             \
        }                                                                          \
        return l;                                                                  \
    }
BINLEVEL(px_concat, px_unary, "||")
BINLEVEL(px_mul, px_concat, "*", "/", "%")
BINLEVEL(px_add, px_mul, "+", "-")
BINLEVEL(px_bit, px_add, "&", "|", "<<", ">>")
BINLEVEL(px_rel, px_bit, "<", "<=", ">", ">=")

static sqx_t *px_eq(sqp_t *p) {
    sqx_t *l = px_rel(p);
    while (l) {
        sqtok_t *t = pk_peek(p);
        if (tok_op(t, "=") || tok_op(t, "==") || tok_op(t, "!=") || tok_op(t, "<>")) {
            p->i++;
            sqx_t *r = px_rel(p);
            if (!r) goto fail;
            l = mk2(X_BIN, (tok_op(t, "!=") || tok_op(t, "<>")) ? "!=" : "=", l, r);
            continue;
        }
        if (pk_kw(p, "is")) {
            bool neg = pk_kw(p, "not");
            sqx_t *r = px_rel(p);
            if (!r) goto fail;
            l = mk2(X_IS, "is", l, r);
            l->neg = neg;
            continue;
        }
        if (pk_kw(p, "isnull") || pk_kw(p, "notnull")) {
            bool notnull = tok_kw(&p->t[p->i - 1], "notnull");
            l = mk2(X_ISNULL, "", l, NULL);
            l->neg = notnull;
            continue;
        }
        size_t save = p->i;
        bool neg = pk_kw(p, "not");
        if (neg && pk_kw(p, "null")) {
            l = mk2(X_ISNULL, "", l, NULL);
            l->neg = true;
            continue;
        }
        if (pk_kw(p, "in")) {
            if (!pk_op(p, "(")) {
                sqx_free(l);
                return pk_fail(p, "IN requires a parenthesized list in omc's SQLite reader%s", "");
            }
            if (tok_kw(pk_peek(p), "select")) {
                sqx_free(l);
                return pk_fail(p, "subqueries are not supported by omc's SQLite reader%s", "");
            }
            sqx_t *x = mk2(X_IN, "in", l, NULL);
            x->neg = neg;
            l = x;
            if (!tok_op(pk_peek(p), ")")) {
                do {
                    sqx_t *a = px_expr(p);
                    if (!a) goto fail;
                    vec_push(&x->args, a);
                } while (pk_op(p, ","));
            }
            if (!pk_op(p, ")")) {
                sqx_free(l);
                return pk_fail(p, "near \"%s\": syntax error", pk_peek(p)->v ? pk_peek(p)->v : "");
            }
            continue;
        }
        if (pk_kw(p, "between")) {
            sqx_t *lo = px_rel(p);
            if (!lo) goto fail;
            if (!pk_kw(p, "and")) {
                sqx_free(lo);
                sqx_free(l);
                return pk_fail(p, "BETWEEN requires AND%s", "");
            }
            sqx_t *hi = px_rel(p);
            if (!hi) {
                sqx_free(lo);
                goto fail;
            }
            sqx_t *x = mk2(X_BETWEEN, "btw", l, lo);
            x->c = hi;
            x->neg = neg;
            l = x;
            continue;
        }
        if (tok_kw(pk_peek(p), "like") || tok_kw(pk_peek(p), "glob")) {
            bool like = tok_kw(pk_peek(p), "like");
            p->i++;
            sqx_t *r = px_rel(p);
            if (!r) goto fail;
            sqx_t *x = mk2(X_LIKE, like ? "lk" : "gb", l, r);
            x->neg = neg;
            if (like && pk_kw(p, "escape")) {
                x->c = px_rel(p);
                if (!x->c) {
                    sqx_free(x);
                    return NULL;
                }
            }
            l = x;
            continue;
        }
        if (tok_kw(pk_peek(p), "match") || tok_kw(pk_peek(p), "regexp")) {
            sqx_free(l);
            return pk_fail(p, "%s is not supported by omc's SQLite reader", pk_peek(p)->v);
        }
        p->i = save;
        break;
    }
    return l;
fail:
    sqx_free(l);
    return NULL;
}

static sqx_t *px_not(sqp_t *p) {
    if (++p->depth > 200) return pk_fail(p, "expression nested too deeply%s", "");
    sqx_t *r;
    if (pk_kw(p, "not")) {
        sqx_t *a = px_not(p);
        r = a ? mk2(X_NOT, "not", a, NULL) : NULL;
    } else {
        r = px_eq(p);
    }
    p->depth--;
    return r;
}

static sqx_t *px_and(sqp_t *p) {
    sqx_t *l = px_not(p);
    while (l && pk_kw(p, "and")) {
        sqx_t *r = px_not(p);
        if (!r) {
            sqx_free(l);
            return NULL;
        }
        l = mk2(X_BIN, "and", l, r);
    }
    return l;
}

static sqx_t *px_expr(sqp_t *p) {
    if (++p->depth > 200) return pk_fail(p, "expression nested too deeply%s", "");
    sqx_t *l = px_and(p);
    while (l && pk_kw(p, "or")) {
        sqx_t *r = px_and(p);
        if (!r) {
            sqx_free(l);
            l = NULL;
            break;
        }
        l = mk2(X_BIN, "or", l, r);
    }
    p->depth--;
    return l;
}

/* evaluation context */
typedef struct {
    const sqv_t *row;
    int64_t rowid;
} sqctx_t;

static sqv_t ev(const sqx_t *x, const sqctx_t *c);

static bool numeric_aff(int a) { return a == AFF_INT || a == AFF_REAL || a == AFF_NUMERIC; }

static int expr_aff(const sqx_t *x) { return (x->k == X_COL || x->k == X_ROWID || x->k == X_CAST) ? x->aff : AFF_NONE; }

/* apply comparison affinities (SQLite datatype rules 4.2) to evaluated operands */
static void cmp_affinity(const sqx_t *xa, const sqx_t *xb, sqv_t *a, sqv_t *b) {
    int fa = expr_aff(xa), fb = expr_aff(xb);
    if (numeric_aff(fa) && !numeric_aff(fb)) apply_numeric_aff(b);
    else if (numeric_aff(fb) && !numeric_aff(fa)) apply_numeric_aff(a);
    else if (fa == AFF_TEXT && fb == AFF_NONE) apply_text_aff(b);
    else if (fb == AFF_TEXT && fa == AFF_NONE) apply_text_aff(a);
}

static sqv_t arith(const char *op, const sqv_t *x, const sqv_t *y) {
    if (x->t == SV_NULL || y->t == SV_NULL) return sqv_null();
    sqv_t a = to_numeric(x), b = to_numeric(y);
    if (strchr("&|<>", op[0])) {
        int64_t ia = a.t == SV_INT ? a.i : (int64_t)a.d, ib = b.t == SV_INT ? b.i : (int64_t)b.d;
        if (op[0] == '&') return sqv_int(ia & ib);
        if (op[0] == '|') return sqv_int(ia | ib);
        if (ib >= 64 || ib <= -64) return sqv_int(op[0] == '<' ? 0 : (ib < 0 ? 0 : (ia < 0 ? -1 : 0)));
        if (op[0] == '<') return ib >= 0 ? sqv_int((int64_t)((uint64_t)ia << ib)) : sqv_int(ia >> -ib);
        return ib >= 0 ? sqv_int(ia >> ib) : sqv_int((int64_t)((uint64_t)ia << -ib));
    }
    if (op[0] == '%') {
        int64_t ia = a.t == SV_INT ? a.i : (int64_t)a.d, ib = b.t == SV_INT ? b.i : (int64_t)b.d;
        if (ib == 0) return sqv_null();
        if (ib == -1) return sqv_int(0);
        if (a.t == SV_REAL || b.t == SV_REAL) return sqv_real((double)(ia % ib));
        return sqv_int(ia % ib);
    }
    if (a.t == SV_INT && b.t == SV_INT) {
        int64_t r;
        switch (op[0]) {
        case '+': if (!__builtin_add_overflow(a.i, b.i, &r)) return sqv_int(r); break;
        case '-': if (!__builtin_sub_overflow(a.i, b.i, &r)) return sqv_int(r); break;
        case '*': if (!__builtin_mul_overflow(a.i, b.i, &r)) return sqv_int(r); break;
        case '/':
            if (b.i == 0) return sqv_null();
            if (a.i == INT64_MIN && b.i == -1) return sqv_real(-(double)INT64_MIN);
            return sqv_int(a.i / b.i);
        }
    }
    double da = a.t == SV_INT ? (double)a.i : a.d, db = b.t == SV_INT ? (double)b.i : b.d;
    switch (op[0]) {
    case '+': return sqv_real(da + db);
    case '-': return sqv_real(da - db);
    case '*': return sqv_real(da * db);
    default: return db == 0 ? sqv_null() : sqv_real(da / db);
    }
}

static sqv_t ev_func(const sqx_t *x, const sqctx_t *c) {
    const char *f = x->fname;
    size_t n = x->args.n;
    sqv_t a[3] = {{0}};
    if (!strcmp(f, "coalesce") || !strcmp(f, "ifnull")) {
        for (size_t i = 0; i < n; i++) {
            sqv_t v = ev(x->args.v[i], c);
            if (v.t != SV_NULL) return v;
        }
        return sqv_null();
    }
    if (!strcmp(f, "iif")) {
        if (n != 3) return sqv_null();
        sqv_t cond = ev(x->args.v[0], c);
        int t = sqv_truth(&cond);
        sqv_free(&cond);
        return ev(x->args.v[t == 1 ? 1 : 2], c);
    }
    if (!strcmp(f, "min") || !strcmp(f, "max")) {
        sqv_t best = sqv_null();
        bool have = false;
        for (size_t i = 0; i < n; i++) {
            sqv_t v = ev(x->args.v[i], c);
            if (v.t == SV_NULL) {
                sqv_free(&best);
                return v;
            }
            int cmp = have ? sqv_cmp(&v, &best) : 0;
            if (!have || (f[1] == 'i' ? cmp < 0 : cmp > 0)) {
                sqv_free(&best);
                best = v;
                have = true;
            } else {
                sqv_free(&v);
            }
        }
        return best;
    }
    for (size_t i = 0; i < n && i < 3; i++) a[i] = ev(x->args.v[i], c);
    sqv_t r = sqv_null();
    if (!strcmp(f, "typeof")) {
        static const char *const T[] = {"null", "integer", "real", "text", "blob"};
        r = sqv_text_own(xstrdup(n ? T[a[0].t] : "null"));
    } else if (n >= 1 && a[0].t == SV_NULL && strcmp(f, "nullif") && strcmp(f, "char")) {
        r = sqv_null();
    } else if (!strcmp(f, "lower") || !strcmp(f, "upper")) {
        char *s = sqv_sqltext(&a[0]);
        for (char *q = s; *q; q++) *q = (char)(f[0] == 'l' ? tolower((unsigned char)*q) : toupper((unsigned char)*q));
        r = sqv_text_own(s);
    } else if (!strcmp(f, "length")) {
        if (a[0].t == SV_BLOB) r = sqv_int((int64_t)a[0].n);
        else {
            char *s = sqv_sqltext(&a[0]);
            r = sqv_int((int64_t)utf8_strlen(s));
            free(s);
        }
    } else if (!strcmp(f, "abs")) {
        sqv_t v = to_numeric(&a[0]);
        r = v.t == SV_INT ? (v.i == INT64_MIN ? sqv_real(9.223372036854775808e18) : sqv_int(v.i < 0 ? -v.i : v.i)) : sqv_real(fabs(v.d));
    } else if (!strcmp(f, "nullif")) {
        if (n == 2 && a[0].t != SV_NULL && a[1].t != SV_NULL && sqv_cmp(&a[0], &a[1]) == 0) r = sqv_null();
        else r = sqv_copy(&a[0]);
    } else if (!strcmp(f, "trim") || !strcmp(f, "ltrim") || !strcmp(f, "rtrim")) {
        char *s = sqv_sqltext(&a[0]);
        char *set = n > 1 && a[1].t != SV_NULL ? sqv_sqltext(&a[1]) : xstrdup(" ");
        size_t lo = 0, hi = strlen(s);
        if (f[0] != 'r')
            while (lo < hi && strchr(set, s[lo]) && s[lo]) lo++;
        if (f[0] != 'l')
            while (hi > lo && strchr(set, s[hi - 1]) && s[hi - 1]) hi--;
        r = sqv_textn(s + lo, hi - lo);
        free(s);
        free(set);
    } else if (!strcmp(f, "substr") || !strcmp(f, "substring")) {
        char *s = sqv_sqltext(&a[0]);
        int64_t len = (int64_t)utf8_strlen(s);
        sqv_t st = to_numeric(&a[1]);
        int64_t start = st.t == SV_INT ? st.i : (int64_t)st.d;
        int64_t cnt = len;
        if (n > 2) {
            sqv_t cv = to_numeric(&a[2]);
            cnt = cv.t == SV_INT ? cv.i : (int64_t)cv.d;
        }
        if (start < 0) start = len + start + 1;
        else if (start == 0) {
            start = 1;
            cnt--;
        }
        if (cnt < 0) {
            start += cnt;
            cnt = -cnt;
            if (start < 1) {
                cnt += start - 1;
                start = 1;
            }
        }
        if (start < 1) {
            cnt += start - 1;
            start = 1;
        }
        sb_t b = SB_INIT;
        int64_t ci = 1;
        for (size_t i = 0; s[i] && cnt > 0;) {
            uint32_t cp;
            int k = utf8_decode(s + i, strlen(s + i), &cp);
            if (ci >= start) {
                sb_appendn(&b, s + i, (size_t)k);
                cnt--;
            }
            ci++;
            i += (size_t)k;
        }
        free(s);
        r = sqv_text_own(sb_detach(&b));
    } else if (!strcmp(f, "instr")) {
        char *s = sqv_sqltext(&a[0]), *q = n > 1 ? sqv_sqltext(&a[1]) : xstrdup("");
        if (n > 1 && a[1].t == SV_NULL) r = sqv_null();
        else {
            char *hit = strstr(s, q);
            r = sqv_int(0);
            if (hit) {
                char *pre = xstrndup(s, (size_t)(hit - s));
                r = sqv_int((int64_t)utf8_strlen(pre) + 1);
                free(pre);
            }
        }
        free(s);
        free(q);
    } else if (!strcmp(f, "replace")) {
        if (n == 3 && a[1].t != SV_NULL && a[2].t != SV_NULL) {
            char *s = sqv_sqltext(&a[0]), *from = sqv_sqltext(&a[1]), *to = sqv_sqltext(&a[2]);
            r = sqv_text_own(*from ? str_replace(s, from, to) : xstrdup(s));
            free(s);
            free(from);
            free(to);
        }
    } else if (!strcmp(f, "round")) {
        sqv_t v = to_numeric(&a[0]);
        int64_t digits = 0;
        if (n > 1) {
            sqv_t dv = to_numeric(&a[1]);
            digits = dv.t == SV_INT ? dv.i : (int64_t)dv.d;
            if (digits < 0) digits = 0;
            if (digits > 15) digits = 15;
        }
        double d = v.t == SV_INT ? (double)v.i : v.d, m = pow(10, (double)digits);
        r = sqv_real(round(d * m) / m);
    } else if (!strcmp(f, "hex")) {
        char *s = a[0].t == SV_BLOB ? xstrndup(a[0].s, a[0].n) : sqv_sqltext(&a[0]);
        size_t sl = a[0].t == SV_BLOB ? a[0].n : strlen(s);
        char *h = hex_encode(s, sl);
        for (char *q = h; *q; q++) *q = (char)toupper((unsigned char)*q);
        r = sqv_text_own(h);
        free(s);
    } else if (!strcmp(f, "unicode")) {
        char *s = sqv_sqltext(&a[0]);
        uint32_t cp = 0;
        if (*s) utf8_decode(s, strlen(s), &cp);
        r = *s ? sqv_int(cp) : sqv_null();
        free(s);
    } else if (!strcmp(f, "char")) {
        sb_t b = SB_INIT;
        for (size_t i = 0; i < n; i++) {
            sqv_t v = ev(x->args.v[i], c);
            sqv_t nv = to_numeric(&v);
            int64_t cp = nv.t == SV_INT ? nv.i : (int64_t)nv.d;
            if (cp >= 0 && cp <= 0x10FFFF) sb_append_utf8(&b, (uint32_t)cp);
            sqv_free(&v);
        }
        r = sqv_text_own(sb_detach(&b));
    }
    for (size_t i = 0; i < 3; i++) sqv_free(&a[i]);
    return r;
}

static sqv_t ev(const sqx_t *x, const sqctx_t *c) {
    switch (x->k) {
    case X_LIT: return sqv_copy(&x->lit);
    case X_COL: return c->row ? sqv_copy(&c->row[x->col]) : sqv_null();
    case X_ROWID: return sqv_int(c->rowid);
    case X_AGG: {
        if (!strcmp(x->fname, "count")) return sqv_int(x->cnt);
        if (!strcmp(x->fname, "min") || !strcmp(x->fname, "max")) return x->has_best ? sqv_copy(&x->best) : sqv_null();
        if (!strcmp(x->fname, "avg")) {
            if (!x->cnt) return sqv_null();
            return sqv_real((x->sum_real || x->overflow ? x->rsum : (double)x->isum) / (double)x->cnt);
        }
        if (!strcmp(x->fname, "total")) return sqv_real(x->sum_real || x->overflow ? x->rsum : (double)x->isum);
        if (!x->sum_any) return sqv_null();
        return x->sum_real || x->overflow ? sqv_real(x->rsum) : sqv_int(x->isum);
    }
    case X_CAST: {
        sqv_t v = ev(x->a, c);
        if (v.t == SV_NULL) return v;
        if (x->aff == AFF_TEXT) {
            char *s = sqv_sqltext(&v);
            sqv_free(&v);
            return sqv_text_own(s);
        }
        if (x->aff == AFF_INT || x->aff == AFF_REAL || x->aff == AFF_NUMERIC) {
            sqv_t n = to_numeric(&v);
            sqv_free(&v);
            if (x->aff == AFF_INT) return sqv_int(n.t == SV_INT ? n.i : (int64_t)n.d);
            if (x->aff == AFF_REAL) return sqv_real(n.t == SV_INT ? (double)n.i : n.d);
            return n;
        }
        if (v.t == SV_TEXT) v.t = SV_BLOB;
        return v;
    }
    case X_NEG: {
        sqv_t v = ev(x->a, c);
        if (v.t == SV_NULL) return v;
        sqv_t n = to_numeric(&v);
        sqv_free(&v);
        if (n.t == SV_INT) return n.i == INT64_MIN ? sqv_real(-(double)n.i) : sqv_int(-n.i);
        return sqv_real(-n.d);
    }
    case X_BITNOT: {
        sqv_t v = ev(x->a, c);
        if (v.t == SV_NULL) return v;
        sqv_t n = to_numeric(&v);
        sqv_free(&v);
        return sqv_int(~(n.t == SV_INT ? n.i : (int64_t)n.d));
    }
    case X_NOT: {
        sqv_t v = ev(x->a, c);
        int t = sqv_truth(&v);
        sqv_free(&v);
        return t < 0 ? sqv_null() : sqv_int(!t);
    }
    case X_ISNULL: {
        sqv_t v = ev(x->a, c);
        bool isnull = v.t == SV_NULL;
        sqv_free(&v);
        return sqv_int(x->neg ? !isnull : isnull);
    }
    case X_IS: {
        sqv_t a = ev(x->a, c), b = ev(x->b, c);
        cmp_affinity(x->a, x->b, &a, &b);
        bool eq = (a.t == SV_NULL || b.t == SV_NULL) ? a.t == b.t : sqv_cmp(&a, &b) == 0;
        sqv_free(&a);
        sqv_free(&b);
        return sqv_int(x->neg ? !eq : eq);
    }
    case X_IN: {
        sqv_t a = ev(x->a, c);
        if (a.t == SV_NULL) return a;
        bool hit = false, saw_null = false;
        for (size_t i = 0; i < x->args.n && !hit; i++) {
            sqv_t b = ev(x->args.v[i], c), av = sqv_copy(&a);
            if (b.t == SV_NULL) saw_null = true;
            else {
                cmp_affinity(x->a, x->args.v[i], &av, &b);
                hit = sqv_cmp(&av, &b) == 0;
            }
            sqv_free(&av);
            sqv_free(&b);
        }
        sqv_free(&a);
        if (hit) return sqv_int(!x->neg);
        if (saw_null) return sqv_null();
        return sqv_int(x->neg);
    }
    case X_BETWEEN: {
        sqv_t v = ev(x->a, c), lo = ev(x->b, c), hi = ev(x->c, c), v2 = sqv_copy(&v);
        cmp_affinity(x->a, x->b, &v, &lo);
        cmp_affinity(x->a, x->c, &v2, &hi);
        int r1 = (v.t == SV_NULL || lo.t == SV_NULL) ? -1 : sqv_cmp(&v, &lo) >= 0;
        int r2 = (v2.t == SV_NULL || hi.t == SV_NULL) ? -1 : sqv_cmp(&v2, &hi) <= 0;
        sqv_free(&v);
        sqv_free(&v2);
        sqv_free(&lo);
        sqv_free(&hi);
        int r = (r1 == 0 || r2 == 0) ? 0 : (r1 < 0 || r2 < 0) ? -1 : 1;
        if (r < 0) return sqv_null();
        return sqv_int(x->neg ? !r : r);
    }
    case X_LIKE: {
        sqv_t a = ev(x->a, c), b = ev(x->b, c);
        sqv_t e = x->c ? ev(x->c, c) : sqv_null();
        sqv_t r = sqv_null();
        if (a.t != SV_NULL && b.t != SV_NULL && (!x->c || e.t != SV_NULL)) {
            char *s = sqv_sqltext(&a), *pat = sqv_sqltext(&b);
            uint32_t esc = 0;
            if (x->c) {
                char *es = sqv_sqltext(&e);
                if (*es) utf8_decode(es, strlen(es), &esc);
                free(es);
            }
            bool m = pat_match(pat, s, x->op[0] == 'l', esc);
            r = sqv_int(x->neg ? !m : m);
            free(s);
            free(pat);
        }
        sqv_free(&a);
        sqv_free(&b);
        sqv_free(&e);
        return r;
    }
    case X_FUNC: return ev_func(x, c);
    case X_BIN: {
        if (!strcmp(x->op, "and") || !strcmp(x->op, "or")) {
            sqv_t a = ev(x->a, c);
            int ta = sqv_truth(&a);
            sqv_free(&a);
            bool is_and = x->op[0] == 'a';
            if (is_and && ta == 0) return sqv_int(0);
            if (!is_and && ta == 1) return sqv_int(1);
            sqv_t b = ev(x->b, c);
            int tb = sqv_truth(&b);
            sqv_free(&b);
            if (is_and) return (tb == 0) ? sqv_int(0) : (ta < 0 || tb < 0) ? sqv_null() : sqv_int(1);
            return (tb == 1) ? sqv_int(1) : (ta < 0 || tb < 0) ? sqv_null() : sqv_int(0);
        }
        sqv_t a = ev(x->a, c), b = ev(x->b, c);
        sqv_t r;
        if (!strcmp(x->op, "||")) {
            if (a.t == SV_NULL || b.t == SV_NULL) r = sqv_null();
            else {
                char *s1 = sqv_sqltext(&a), *s2 = sqv_sqltext(&b);
                r = sqv_text_own(xasprintf("%s%s", s1, s2));
                free(s1);
                free(s2);
            }
        } else if (strchr("=!<>", x->op[0]) && strcmp(x->op, "<<") && strcmp(x->op, ">>")) {
            cmp_affinity(x->a, x->b, &a, &b);
            if (a.t == SV_NULL || b.t == SV_NULL) r = sqv_null();
            else {
                int cmp = sqv_cmp(&a, &b);
                bool v = !strcmp(x->op, "=")    ? cmp == 0
                         : !strcmp(x->op, "!=") ? cmp != 0
                         : !strcmp(x->op, "<")  ? cmp < 0
                         : !strcmp(x->op, "<=") ? cmp <= 0
                         : !strcmp(x->op, ">")  ? cmp > 0
                                                : cmp >= 0;
                r = sqv_int(v);
            }
        } else {
            r = arith(x->op, &a, &b);
        }
        sqv_free(&a);
        sqv_free(&b);
        return r;
    }
    }
    return sqv_null();
}

static void agg_step(sqx_t *x, const sqctx_t *c) {
    if (x->star) {
        x->cnt++;
        return;
    }
    sqv_t v = ev(x->args.v[0], c);
    if (v.t == SV_NULL) return;
    x->cnt++;
    if (!strcmp(x->fname, "min") || !strcmp(x->fname, "max")) {
        int cmp = x->has_best ? sqv_cmp(&v, &x->best) : 0;
        if (!x->has_best || (x->fname[1] == 'i' ? cmp < 0 : cmp > 0)) {
            sqv_free(&x->best);
            x->best = v;
            x->has_best = true;
            return;
        }
    } else if (strcmp(x->fname, "count")) {
        sqv_t n = to_numeric(&v);
        x->sum_any = true;
        if (n.t == SV_REAL) x->sum_real = true;
        x->rsum += n.t == SV_INT ? (double)n.i : n.d;
        if (n.t == SV_INT && !x->overflow && __builtin_add_overflow(x->isum, n.i, &x->isum)) x->overflow = true;
    }
    sqv_free(&v);
}

/* ---------- SQLite: selectors, query execution and rendering (upstream sqlite-reader.ts) ---------- */

typedef struct {
    char *k, *v;
} qparam_t;
typedef VEC(qparam_t) qparams_t;

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* application/x-www-form-urlencoded decoding (URLSearchParams) */
static char *url_decode(const char *s, size_t n) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '+') sb_putc(&b, ' ');
        else if (s[i] == '%' && i + 2 < n && hexval((unsigned char)s[i + 1]) >= 0 && hexval((unsigned char)s[i + 2]) >= 0) {
            sb_putc(&b, (char)(hexval((unsigned char)s[i + 1]) * 16 + hexval((unsigned char)s[i + 2])));
            i += 2;
        } else sb_putc(&b, s[i]);
    }
    char *r = sb_detach(&b);
    if (!utf8_valid(r, strlen(r))) {
        char *t = utf8_sanitize(r, strlen(r));
        free(r);
        r = t;
    }
    return r;
}

static void qparse(const char *qs, qparams_t *out) {
    memset(out, 0, sizeof *out);
    const char *p = qs;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t n = amp ? (size_t)(amp - p) : strlen(p);
        if (n) {
            const char *eq = memchr(p, '=', n);
            qparam_t q;
            q.k = url_decode(p, eq ? (size_t)(eq - p) : n);
            q.v = eq ? url_decode(eq + 1, n - (size_t)(eq - p) - 1) : xstrdup("");
            vec_push(out, q);
        }
        p += n;
        if (*p == '&') p++;
    }
}

static void qparams_free(qparams_t *q) {
    for (size_t i = 0; i < q->n; i++) {
        free(q->v[i].k);
        free(q->v[i].v);
    }
    vec_free(q);
}

static const char *qget(const qparams_t *q, const char *k) {
    for (size_t i = 0; i < q->n; i++)
        if (!strcmp(q->v[i].k, k)) return q->v[i].v;
    return NULL;
}

/* URLSearchParams serialization of one component */
static void url_encode(sb_t *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '*' || *p == '-' || *p == '.' || *p == '_') sb_putc(b, (char)*p);
        else if (*p == ' ') sb_putc(b, '+');
        else sb_appendf(b, "%%%02X", *p);
    }
}

/* JS Number.parseInt(value, 10); false when NaN */
static bool js_parse_int(const char *v, long long *out) {
    while (isspace((unsigned char)*v)) v++;
    bool neg = false;
    if (*v == '+' || *v == '-') neg = *v++ == '-';
    if (!isdigit((unsigned char)*v)) return false;
    long long r = 0;
    for (; isdigit((unsigned char)*v); v++) r = r > (LLONG_MAX - 9) / 10 ? LLONG_MAX : r * 10 + (*v - '0');
    *out = neg ? -r : r;
    return true;
}

static bool blank(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static int parse_limit(const char *v, int64_t fallback, int64_t *out, char **err) {
    if (!v || blank(v)) {
        *out = fallback;
        return 0;
    }
    long long n;
    if (!js_parse_int(v, &n) || n < 1) {
        *err = xasprintf("SQLite limit must be a positive integer; got '%s'", v);
        return -1;
    }
    *out = MIN(n, (long long)SQ_MAX_QUERY_LIMIT);
    return 0;
}

static int parse_offset(const char *v, int64_t *out, char **err) {
    if (!v || blank(v)) {
        *out = 0;
        return 0;
    }
    long long n;
    if (!js_parse_int(v, &n) || n < 0) {
        *err = xasprintf("SQLite offset must be a non-negative integer; got '%s'", v);
        return -1;
    }
    *out = n;
    return 0;
}

#define WHERE_COMMENT_ERR "SQLite 'where' clause must not contain comments or statement terminators; use '?q=SELECT ...' for raw SQL"
#define WHERE_KEYWORD_ERR                                                                                              \
    "SQLite 'where' clause must not contain LIMIT/OFFSET/UNION/INTERSECT/EXCEPT/ATTACH/DETACH/PRAGMA; use '?q=SELECT ...' for raw SQL"

/* upstream findWhereClauseViolation */
static const char *where_violation(const char *sql) {
    static const char *const KW[] = {"limit", "offset", "union", "intersect", "except", "attach", "detach", "pragma"};
    bool sq = false, dq = false, kwbad = false;
    size_t n = strlen(sql), ts = SIZE_MAX;
    for (size_t i = 0; i <= n; i++) {
        char c = i < n ? sql[i] : 0, nx = i + 1 < n ? sql[i + 1] : 0;
        if (sq) {
            if (c == '\'' && nx == '\'') i++;
            else if (c == '\'') sq = false;
            continue;
        }
        if (dq) {
            if (c == '"' && nx == '"') i++;
            else if (c == '"') dq = false;
            continue;
        }
        if (c && (isalnum((unsigned char)c) || c == '_')) {
            if (ts == SIZE_MAX) ts = i;
            continue;
        }
        if (ts != SIZE_MAX) {
            if (!kwbad)
                for (size_t k = 0; k < ARRAY_LEN(KW); k++)
                    if (i - ts == strlen(KW[k]) && !strncasecmp(sql + ts, KW[k], i - ts)) kwbad = true;
            ts = SIZE_MAX;
        }
        if (!c) break;
        if (c == '\'') sq = true;
        else if (c == '"') dq = true;
        else if (c == ';') return WHERE_COMMENT_ERR;
        else if ((c == '-' && nx == '-') || (c == '/' && nx == '*') || (c == '*' && nx == '/')) return WHERE_COMMENT_ERR;
    }
    return kwbad ? WHERE_KEYWORD_ERR : NULL;
}

/* trimmed where clause (owned) or NULL when empty; *err on violation */
static char *validate_where(const char *w, char **err) {
    if (!w) return NULL;
    char *t = str_trim(w);
    if (!*t) {
        free(t);
        return NULL;
    }
    const char *v = where_violation(t);
    if (v) {
        free(t);
        *err = xstrdup(v);
        return NULL;
    }
    return t;
}

enum { SEL_LIST, SEL_SCHEMA, SEL_ROW, SEL_QUERY, SEL_RAW };
typedef struct {
    int kind;
    char *table, *key, *where, *order, *sql;
    int64_t limit, offset;
    bool page;
} sqsel_t;

static void sqsel_free(sqsel_t *s) {
    free(s->table);
    free(s->key);
    free(s->where);
    free(s->order);
    free(s->sql);
}

/* upstream parseSqliteSelector */
static int parse_selector(const char *sub, const char *query, sqsel_t *s, char **err) {
    memset(s, 0, sizeof *s);
    while (*sub == ':') sub++;
    char *norm = str_trim(sub);
    qparams_t qp;
    qparse(query ? query : "", &qp);
    int rc = -1;
    const char *q = qget(&qp, "q");
    if (q) {
        if (*norm) {
            *err = xasprintf("SQLite ?q= queries run against the whole database; drop ':%s' from the path", norm);
            goto out;
        }
        for (size_t i = 0; i < qp.n; i++)
            if (strcmp(qp.v[i].k, "q") && strcmp(qp.v[i].k, "limit") && strcmp(qp.v[i].k, "offset")) {
                *err = xasprintf("SQLite ?q= queries take only limit and offset; put '%s' into the SQL", qp.v[i].k);
                goto out;
            }
        if (blank(q)) {
            *err = xstrdup("SQLite query parameter 'q' cannot be empty");
            goto out;
        }
        s->kind = SEL_RAW;
        s->sql = xstrdup(q);
        if (qget(&qp, "limit") || qget(&qp, "offset")) {
            s->page = true;
            if (parse_limit(qget(&qp, "limit"), SQ_DEFAULT_QUERY_LIMIT, &s->limit, err) ||
                parse_offset(qget(&qp, "offset"), &s->offset, err))
                goto out;
        }
        rc = 0;
        goto out;
    }
    if (!*norm) {
        if (qp.n) {
            *err = xstrdup("SQLite query parameters require a table selector or q=SELECT...");
            goto out;
        }
        s->kind = SEL_LIST;
        rc = 0;
        goto out;
    }
    char *sep = strchr(norm, ':');
    s->table = sep ? xstrndup(norm, (size_t)(sep - norm)) : xstrdup(norm);
    const char *key = sep ? sep + 1 : NULL;
    if (!*s->table) {
        *err = xstrdup("SQLite selectors must include a table name");
        goto out;
    }
    if (key && *key) {
        if (qp.n) {
            *err = xstrdup("SQLite row lookups cannot be combined with query parameters");
            goto out;
        }
        s->kind = SEL_ROW;
        s->key = xstrdup(key);
        rc = 0;
        goto out;
    }
    s->where = validate_where(qget(&qp, "where"), err);
    if (*err) goto out;
    const char *ord = qget(&qp, "order");
    if (ord) {
        char *t = str_trim(ord);
        if (*t) s->order = t;
        else free(t);
    }
    if (qget(&qp, "limit") || qget(&qp, "offset") || s->order || s->where) {
        for (size_t i = 0; i < qp.n; i++)
            if (strcmp(qp.v[i].k, "limit") && strcmp(qp.v[i].k, "offset") && strcmp(qp.v[i].k, "order") &&
                strcmp(qp.v[i].k, "where")) {
                *err = xasprintf("Unsupported SQLite query parameter '%s'", qp.v[i].k);
                goto out;
            }
        s->kind = SEL_QUERY;
        if (parse_limit(qget(&qp, "limit"), SQ_DEFAULT_QUERY_LIMIT, &s->limit, err) ||
            parse_offset(qget(&qp, "offset"), &s->offset, err))
            goto out;
        rc = 0;
        goto out;
    }
    if (qp.n) {
        *err = xasprintf("Unsupported SQLite query parameter '%s'", qp.v[0].k);
        goto out;
    }
    s->kind = SEL_SCHEMA;
    s->limit = SQ_DEFAULT_SAMPLE;
    rc = 0;
out:
    qparams_free(&qp);
    free(norm);
    return rc;
}

/* ---- table scans with declared-column rows ---- */

typedef int (*row_cb)(void *ud, sqv_t *row, int64_t rowid); /* row values freed after the call unless stolen */
typedef struct {
    const sqtab_t *t;
    row_cb cb;
    void *ud;
    sqv_t *row;
} tabwalk_t;

static int tabwalk_cb(void *ud, int64_t rowid, sqvals_t *rec) {
    tabwalk_t *w = ud;
    map_row(w->t, rowid, rec, w->row);
    int r = w->cb(w->ud, w->row, rowid);
    for (size_t c = 0; c < w->t->cols.n; c++) sqv_free(&w->row[c]);
    return r;
}

static int tab_scan(sqdb_t *db, const sqtab_t *t, row_cb cb, void *ud) {
    tabwalk_t w = {t, cb, ud, xcalloc(t->cols.n + 1, sizeof(sqv_t))};
    int rc = sq_scan(db, t->root, t->without_rowid, tabwalk_cb, &w);
    free(w.row);
    return rc;
}

/* ---- query engine: one table, WHERE, projection/aggregates, ORDER BY, skip/take ---- */

typedef VEC(sqv_t *) sqrows_t;

static void rows_free(sqrows_t *r, size_t ncols) {
    for (size_t i = 0; i < r->n; i++) {
        if (!r->v[i]) continue;
        for (size_t c = 0; c < ncols; c++) sqv_free(&r->v[i][c]);
        free(r->v[i]);
    }
    vec_free(r);
}

typedef struct {
    const sqtab_t *t; /* NULL: SELECT without FROM */
    sqx_t *where;
    bool star;
    VEC(sqx_t *) proj;
    VEC(sqx_t *) aggs; /* borrowed nodes inside proj */
    VEC(sqx_t *) order;
    VEC(int) order_proj; /* projection index, or -1 to evaluate the expression */
    VEC(bool) desc;
    int64_t skip, take; /* take < 0: unlimited */
    size_t ncols;
    sqrows_t out, keys;
    int64_t skipped;
    char *err;
} qx_t;

static sqv_t *qx_project(qx_t *q, const sqv_t *row, int64_t rowid) {
    sqv_t *r = xcalloc(q->ncols + 1, sizeof(sqv_t));
    if (q->star) {
        for (size_t c = 0; c < q->ncols; c++) r[c] = sqv_copy(&row[c]);
    } else {
        sqctx_t cx = {row, rowid};
        for (size_t c = 0; c < q->proj.n; c++) r[c] = ev(q->proj.v[c], &cx);
    }
    return r;
}

static int qx_row(void *ud, sqv_t *row, int64_t rowid) {
    qx_t *q = ud;
    sqctx_t cx = {row, rowid};
    if (q->where) {
        sqv_t w = ev(q->where, &cx);
        int t = sqv_truth(&w);
        sqv_free(&w);
        if (t != 1) return 0;
    }
    if (q->aggs.n) {
        for (size_t i = 0; i < q->aggs.n; i++) agg_step(q->aggs.v[i], &cx);
        return 0;
    }
    if (!q->order.n) {
        if (q->skipped < q->skip) {
            q->skipped++;
            return 0;
        }
        if (q->take >= 0 && (int64_t)q->out.n >= q->take) return 1;
        vec_push(&q->out, qx_project(q, row, rowid));
        return q->take >= 0 && (int64_t)q->out.n >= q->take;
    }
    if (q->out.n >= SQ_MAX_SORT_ROWS) {
        q->err = xasprintf("ORDER BY over more than %d rows is not supported by omc's built-in SQLite reader; narrow the "
                           "query with a WHERE clause",
                           SQ_MAX_SORT_ROWS);
        return 1;
    }
    sqv_t *pr = qx_project(q, row, rowid);
    sqv_t *k = xcalloc(q->order.n + 1, sizeof(sqv_t));
    for (size_t i = 0; i < q->order.n; i++)
        k[i] = q->order_proj.v[i] >= 0 ? sqv_copy(&pr[q->order_proj.v[i]]) : ev(q->order.v[i], &cx);
    vec_push(&q->out, pr);
    vec_push(&q->keys, k);
    return 0;
}

static int qx_cmp(const qx_t *q, size_t a, size_t b) {
    for (size_t i = 0; i < q->order.n; i++) {
        int c = sqv_cmp(&q->keys.v[a][i], &q->keys.v[b][i]);
        if (c) return q->desc.v[i] ? -c : c;
    }
    return a < b ? -1 : a > b; /* stable */
}

/* bottom-up merge sort of an index permutation (no global comparator state) */
static void qx_sort(const qx_t *q, size_t *idx, size_t n) {
    size_t *tmp = xmalloc((n + 1) * sizeof(size_t));
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = MIN(lo + w, n), hi = MIN(lo + 2 * w, n), i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = qx_cmp(q, idx[i], idx[j]) <= 0 ? idx[i++] : idx[j++];
            while (i < mid) tmp[k++] = idx[i++];
            while (j < hi) tmp[k++] = idx[j++];
        }
        memcpy(idx, tmp, n * sizeof(size_t));
    }
    free(tmp);
}

/* apply skip/take to an already materialized list (aggregate / FROM-less / sorted results) */
static void qx_window(qx_t *q, sqrows_t *all, const size_t *order) {
    sqrows_t res = {0};
    for (size_t i = 0; i < all->n; i++) {
        size_t k = order ? order[i] : i;
        bool keep = (int64_t)i >= q->skip && (q->take < 0 || (int64_t)res.n < q->take);
        if (keep) {
            vec_push(&res, all->v[k]);
            all->v[k] = NULL;
        }
    }
    rows_free(all, q->ncols);
    *all = res;
}

static int qx_run(sqdb_t *db, qx_t *q) {
    if (q->t) {
        int rc = tab_scan(db, q->t, qx_row, q);
        if (q->err) return -1;
        if (rc < 0) {
            q->err = xstrdup(db->err ? db->err : "database disk image is malformed");
            return -1;
        }
    } else {
        sqctx_t cx = {NULL, 0};
        bool pass = true;
        if (q->where) {
            sqv_t w = ev(q->where, &cx);
            pass = sqv_truth(&w) == 1;
            sqv_free(&w);
        }
        if (pass && !q->aggs.n) vec_push(&q->out, qx_project(q, NULL, 0));
        if (pass && q->aggs.n)
            for (size_t i = 0; i < q->aggs.n; i++) agg_step(q->aggs.v[i], &cx);
    }
    if (q->aggs.n) {
        vec_push(&q->out, qx_project(q, NULL, 0));
        qx_window(q, &q->out, NULL);
    } else if (q->order.n) {
        size_t n = q->out.n, *idx = xmalloc((n + 1) * sizeof(size_t));
        for (size_t i = 0; i < n; i++) idx[i] = i;
        qx_sort(q, idx, n);
        qx_window(q, &q->out, idx);
        free(idx);
    } else if (!q->t) {
        qx_window(q, &q->out, NULL);
    }
    return 0;
}

static void qx_free(qx_t *q) {
    sqx_free(q->where);
    for (size_t i = 0; i < q->proj.n; i++) sqx_free(q->proj.v[i]);
    vec_free(&q->proj);
    vec_free(&q->aggs);
    for (size_t i = 0; i < q->order.n; i++)
        if (q->order_proj.v[i] < 0) sqx_free(q->order.v[i]);
    vec_free(&q->order);
    vec_free(&q->order_proj);
    vec_free(&q->desc);
    rows_free(&q->out, q->ncols);
    rows_free(&q->keys, q->order.n ? q->order.n : 1);
    free(q->err);
}

/* parse a standalone expression (where=) against a table */
static sqx_t *parse_expr_text(const char *src, const sqtab_t *tab, char **err) {
    sqtoks_t tk;
    if (sql_lex(src, &tk, SQ_MAX_QUERY_TOKENS, err) != 0) return NULL;
    sqp_t p = {0};
    p.src = src;
    p.t = tk.v;
    p.n = tk.n;
    p.tab = tab;
    sqx_t *x = px_expr(&p);
    if (x && pk_peek(&p)->t != TK_EOF) {
        sqx_free(x);
        x = NULL;
        if (!p.err) p.err = xasprintf("near \"%s\": syntax error", pk_peek(&p)->v ? pk_peek(&p)->v : "");
    }
    if (!x) *err = p.err ? p.err : xstrdup("syntax error");
    else free(p.err);
    vec_free(&p.aggs);
    sqtoks_free(&tk);
    return x;
}

static int count_rows_cb(void *ud, sqv_t *row, int64_t rowid) {
    void **u = ud;
    const sqx_t *w = u[0];
    sqctx_t cx = {row, rowid};
    sqv_t v = ev(w, &cx);
    if (sqv_truth(&v) == 1) (*(uint64_t *)u[1])++;
    sqv_free(&v);
    return 0;
}

static int count_matching(sqdb_t *db, const sqtab_t *t, sqx_t *where, uint64_t *out) {
    *out = 0;
    if (!where) return sq_count(db, t->root, t->without_rowid, UINT64_MAX, out) < 0 ? -1 : 0;
    void *u[2] = {where, out};
    return tab_scan(db, t, count_rows_cb, u) < 0 ? -1 : 0;
}

/* upstream queryRows: SELECT * [WHERE] [ORDER BY col dir] LIMIT ? OFFSET ? + total count */
static int query_rows(sqdb_t *db, const sqtab_t *t, const char *where_sql, const char *order, int64_t limit,
                      int64_t offset, qx_t *q, uint64_t *total, char **err) {
    memset(q, 0, sizeof *q);
    q->t = t;
    q->star = true;
    q->ncols = t->cols.n;
    if (where_sql) {
        q->where = parse_expr_text(where_sql, t, err);
        if (!q->where) return -1;
    }
    if (order && *order) {
        const char *colon = strrchr(order, ':');
        char *col = colon ? xstrndup(order, (size_t)(colon - order)) : xstrdup(order);
        char *dir = colon ? str_trim(colon + 1) : xstrdup("asc");
        for (char *p = dir; *p; p++) *p = (char)tolower((unsigned char)*p);
        int ci = -1;
        for (size_t c = 0; c < t->cols.n; c++)
            if (!strcmp(t->cols.v[c].name, col)) ci = (int)c;
        if (ci < 0) *err = xasprintf("SQLite order column '%s' not found in table schema", col);
        else if (strcmp(dir, "asc") && strcmp(dir, "desc"))
            *err = xasprintf("SQLite order direction must be 'asc' or 'desc'; got '%s'", dir);
        bool d = !strcmp(dir, "desc");
        free(col);
        free(dir);
        if (ci < 0 || *err) return -1;
        vec_push(&q->order, NULL);
        vec_push(&q->order_proj, ci);
        vec_push(&q->desc, d);
    }
    q->skip = offset;
    q->take = limit + 1;
    if (qx_run(db, q) != 0) {
        *err = xstrdup(q->err);
        return -1;
    }
    bool more = (int64_t)q->out.n > limit;
    if (more) {
        for (size_t i = (size_t)limit; i < q->out.n; i++) {
            for (size_t c = 0; c < q->ncols; c++) sqv_free(&q->out.v[i][c]);
            free(q->out.v[i]);
        }
        q->out.n = (size_t)limit;
    }
    bool ends = !more && (q->out.n > 0 || offset == 0);
    if (ends) *total = (uint64_t)offset + q->out.n;
    else if (count_matching(db, t, q->where, total) != 0) {
        *err = xstrdup(db->err ? db->err : "database disk image is malformed");
        return -1;
    }
    return 0;
}

/* ---- rendering ---- */

static char *sanitize_cell(const char *s) {
    char *a = replace_tabs(s), *b = str_replace(a, "\r\n", "\\n"), *c = str_replace(b, "\n", "\\n");
    free(a);
    free(b);
    return c;
}

static char *pad_cell(const char *v, int width) {
    char *san = sanitize_cell(v);
    char *t = trunc_width(san, MAX(width, SQ_MIN_COL));
    free(san);
    int w = (int)str_display_width(t);
    if (w >= width) return t;
    sb_t b = SB_INIT;
    sb_append(&b, t);
    for (int i = w; i < width; i++) sb_putc(&b, ' ');
    free(t);
    return sb_detach(&b);
}

static void push_line(strvec_t *lines, sb_t *line) {
    char *r = replace_tabs(sb_cstr(line));
    strvec_push_own(lines, trunc_width(r, SQ_RENDER_WIDTH));
    free(r);
    sb_clear(line);
}

/* upstream buildAsciiTable (+ buildVerticalBlocks fallback beyond 19 columns) */
static char *ascii_table(const strvec_t *cols, char ***cells, size_t nrows) {
    size_t nc = cols->n;
    if (!nc) return xstrdup(nrows ? "(rows returned without named columns)" : "(no rows)");
    if ((size_t)SQ_MIN_COL * nc + 3 * nc + 1 > SQ_RENDER_WIDTH) {
        if (!nrows) return xstrdup("(no rows)");
        int nw = SQ_MIN_COL;
        for (size_t c = 0; c < nc; c++) {
            char *s = sanitize_cell(cols->v[c]);
            nw = MAX(nw, (int)str_display_width(s));
            free(s);
        }
        nw = MIN(SQ_COL_WIDTH, nw);
        sb_t b = SB_INIT;
        for (size_t r = 0; r < nrows; r++) {
            if (r) sb_append(&b, "\n\n");
            sb_appendf(&b, "── Row %zu ──", r + 1);
            for (size_t c = 0; c < nc; c++) {
                char *name = pad_cell(cols->v[c], nw), *val = sanitize_cell(cells[r][c]);
                char *line = xasprintf("%s: %s", name, val);
                char *t = trunc_width(line, SQ_RENDER_WIDTH);
                sb_putc(&b, '\n');
                sb_append(&b, t);
                free(t);
                free(line);
                free(name);
                free(val);
            }
        }
        return sb_detach(&b);
    }
    int *w = xcalloc(nc, sizeof(int));
    for (size_t c = 0; c < nc; c++) {
        char *s = sanitize_cell(cols->v[c]);
        w[c] = MAX(SQ_MIN_COL, MIN(SQ_COL_WIDTH, (int)str_display_width(s)));
        free(s);
    }
    for (size_t r = 0; r < nrows; r++)
        for (size_t c = 0; c < nc; c++) {
            char *s = sanitize_cell(cells[r][c]);
            w[c] = MAX(w[c], MIN(SQ_COL_WIDTH, (int)str_display_width(s)));
            free(s);
        }
    int overhead = (int)nc * 3 + 1, total = overhead;
    for (size_t c = 0; c < nc; c++) total += w[c];
    while (total > SQ_RENDER_WIDTH) {
        int wi = -1, ww = SQ_MIN_COL;
        for (size_t c = 0; c < nc; c++)
            if (w[c] > ww) wi = (int)c, ww = w[c];
        if (wi < 0) break;
        w[wi] = MAX(SQ_MIN_COL, w[wi] - 1);
        total--;
    }
    strvec_t lines = {0};
    sb_t line = SB_INIT;
    sb_append(&line, "| ");
    for (size_t c = 0; c < nc; c++) {
        char *p = pad_cell(cols->v[c], w[c]);
        if (c) sb_append(&line, " | ");
        sb_append(&line, p);
        free(p);
    }
    sb_append(&line, " |");
    push_line(&lines, &line);
    sb_append(&line, "| ");
    for (size_t c = 0; c < nc; c++) {
        if (c) sb_append(&line, " | ");
        for (int i = 0; i < MAX(w[c], SQ_MIN_COL); i++) sb_putc(&line, '-');
    }
    sb_append(&line, " |");
    push_line(&lines, &line);
    if (!nrows) {
        sb_append(&line, "(no rows)");
        push_line(&lines, &line);
    }
    for (size_t r = 0; r < nrows; r++) {
        sb_append(&line, "| ");
        for (size_t c = 0; c < nc; c++) {
            char *p = pad_cell(cells[r][c], w[c]);
            if (c) sb_append(&line, " | ");
            sb_append(&line, p);
            free(p);
        }
        sb_append(&line, " |");
        push_line(&lines, &line);
    }
    sb_free(&line);
    free(w);
    char *out = strvec_join(&lines, "\n");
    strvec_free(&lines);
    return out;
}

static char ***display_cells(const sqrows_t *rows, size_t nc) {
    char ***cells = xcalloc(rows->n + 1, sizeof(char **));
    for (size_t r = 0; r < rows->n; r++) {
        cells[r] = xcalloc(nc + 1, sizeof(char *));
        for (size_t c = 0; c < nc; c++) cells[r][c] = sqv_display(&rows->v[r][c]);
    }
    return cells;
}

static void cells_free(char ***cells, size_t nrows, size_t nc) {
    for (size_t r = 0; r < nrows; r++) {
        for (size_t c = 0; c < nc; c++) free(cells[r][c]);
        free(cells[r]);
    }
    free(cells);
}

static char *render_rows(const strvec_t *cols, const sqrows_t *rows) {
    char ***cells = display_cells(rows, cols->n);
    char *t = ascii_table(cols, cells, rows->n);
    cells_free(cells, rows->n, cols->n);
    return t;
}

static void table_cols(const sqtab_t *t, strvec_t *cols) {
    memset(cols, 0, sizeof *cols);
    for (size_t c = 0; c < t->cols.n; c++) strvec_push(cols, t->cols.v[c].name);
}

/* ---- selectors ---- */

static int load_schema(sqdb_t *db, sqschemas_t *sch, char **err) {
    memset(sch, 0, sizeof *sch);
    if (sq_scan(db, 1, false, schema_cb, sch) < 0) {
        *err = xstrdup(db->err ? db->err : "database disk image is malformed");
        return -1;
    }
    return 0;
}

static int cmp_nocase(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int c = strcasecmp(x, y);
    return c ? c : strcmp(x, y);
}

static int stat1_cb(void *ud, sqv_t *row, int64_t rowid) {
    hmap_t *m = ud;
    if (row[0].t != SV_TEXT || row[2].t != SV_TEXT) return 0;
    long long n;
    if (!js_parse_int(row[2].s, &n)) return 0;
    intptr_t prev = (intptr_t)hmap_get(m, row[0].s);
    if (!prev || n + 1 > prev) hmap_put(m, row[0].s, (void *)(intptr_t)(n + 1));
    return 0;
}

static char *sq_list(sqdb_t *db, sqschemas_t *sch) {
    strvec_t names = {0};
    for (size_t i = 0; i < sch->n; i++)
        if (!strcmp(sch->v[i].type, "table") && !is_internal_name(sch->v[i].name)) strvec_push(&names, sch->v[i].name);
    if (!names.n) return xstrdup("(no tables)");
    qsort(names.v, names.n, sizeof(char *), cmp_nocase);
    hmap_t est;
    hmap_init(&est);
    sqtab_t st;
    char *e2 = NULL;
    for (size_t i = 0; i < sch->n; i++)
        if (!strcmp(sch->v[i].type, "table") && !strcmp(sch->v[i].name, "sqlite_stat1")) {
            if (load_table(db, sch, "sqlite_stat1", true, &st, &e2) == 0 && st.cols.n >= 3) tab_scan(db, &st, stat1_cb, &est);
            sqtab_free(&st);
            free(e2);
            break;
        }
    size_t shown = MIN(names.n, (size_t)500);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < shown; i++) {
        sqtab_t t;
        char *err = NULL, *line;
        intptr_t e = (intptr_t)hmap_get(&est, names.v[i]);
        if (load_table(db, sch, names.v[i], false, &t, &err) != 0) {
            line = t.is_virtual ? xasprintf("%s (virtual table)", names.v[i]) : xasprintf("%s (unreadable: %s)", names.v[i], err);
        } else if (e && e - 1 > SQ_ROW_COUNT_PROBE_CAP) {
            line = xasprintf("%s (~%lld rows)", names.v[i], (long long)(e - 1));
        } else {
            uint64_t cnt = 0;
            if (sq_count(db, t.root, t.without_rowid, SQ_ROW_COUNT_PROBE_CAP, &cnt) < 0)
                line = xasprintf("%s (unreadable: %s)", names.v[i], db->err ? db->err : "malformed");
            else if (cnt > SQ_ROW_COUNT_PROBE_CAP) line = xasprintf("%s (%d+ rows)", names.v[i], SQ_ROW_COUNT_PROBE_CAP);
            else line = xasprintf("%s (%llu rows)", names.v[i], (unsigned long long)cnt);
        }
        free(db->err);
        db->err = NULL;
        char *r = replace_tabs(line), *tw = trunc_width(r, SQ_RENDER_WIDTH);
        if (i) sb_putc(&b, '\n');
        sb_append(&b, tw);
        free(tw);
        free(r);
        free(line);
        free(err);
        sqtab_free(&t);
    }
    if (names.n >= 500) sb_append(&b, "\n\n[500 results limit reached. Use limit=1000 for more]");
    hmap_free(&est, NULL);
    strvec_free(&names);
    return sb_detach(&b);
}

static char *sq_schema(sqdb_t *db, sqschemas_t *sch, const sqsel_t *s, char **err) {
    sqtab_t t;
    if (load_table(db, sch, s->table, false, &t, err) != 0) {
        sqtab_free(&t);
        return NULL;
    }
    qx_t q;
    uint64_t total = 0;
    char *out = NULL;
    if (query_rows(db, &t, NULL, NULL, s->limit, 0, &q, &total, err) == 0) {
        sb_t b = SB_INIT;
        char *sql = replace_tabs(t.sql);
        strvec_t lines = str_split(sql, "\n");
        for (size_t i = 0; i < lines.n; i++) {
            char *tw = trunc_width(lines.v[i], SQ_RENDER_WIDTH);
            if (i) sb_putc(&b, '\n');
            sb_append(&b, tw);
            free(tw);
        }
        strvec_free(&lines);
        free(sql);
        strvec_t cols;
        table_cols(&t, &cols);
        char *tab = render_rows(&cols, &q.out);
        sb_appendf(&b, "\n\nSample rows:\n%s", tab);
        free(tab);
        strvec_free(&cols);
        if (q.out.n < total)
            sb_appendf(&b, "\n[%llu more rows; append :%s?limit=20&offset=%zu to the database path to continue]",
                       (unsigned long long)(total - q.out.n), s->table, q.out.n);
        out = sb_detach(&b);
    }
    qx_free(&q);
    sqtab_free(&t);
    return out;
}

typedef struct {
    int col;
    sqv_t key;
    int aff;
    bool found;
    sqv_t *row;
    size_t ncols;
} pkfind_t;

static int pkfind_cb(void *ud, sqv_t *row, int64_t rowid) {
    pkfind_t *f = ud;
    sqv_t a = sqv_copy(&row[f->col]), k = sqv_copy(&f->key);
    if (numeric_aff(f->aff)) apply_numeric_aff(&k);
    else if (f->aff == AFF_TEXT) apply_text_aff(&k);
    bool eq = a.t != SV_NULL && sqv_cmp(&a, &k) == 0;
    sqv_free(&a);
    sqv_free(&k);
    if (!eq) return 0;
    f->found = true;
    f->row = xcalloc(f->ncols + 1, sizeof(sqv_t));
    for (size_t c = 0; c < f->ncols; c++) f->row[c] = sqv_copy(&row[c]);
    return 1;
}

typedef struct {
    const sqtab_t *t;
    sqv_t *row;
} rowid_find_t;

static int rowid_find_cb(void *ud, int64_t rowid, sqvals_t *rec) {
    rowid_find_t *f = ud;
    f->row = xcalloc(f->t->cols.n + 1, sizeof(sqv_t));
    map_row(f->t, rowid, rec, f->row);
    return 1;
}

/* /^-?\d+$/ after trimming */
static bool parse_int_key(const char *key, int64_t *out) {
    char *t = str_trim(key);
    const char *p = t;
    if (*p == '-') p++;
    bool ok = *p != 0;
    for (; *p; p++)
        if (!isdigit((unsigned char)*p)) ok = false;
    if (ok) {
        errno = 0;
        long long v = strtoll(t, NULL, 10);
        ok = !errno;
        *out = v;
    }
    free(t);
    return ok;
}

static char *sq_row(sqdb_t *db, sqschemas_t *sch, const sqsel_t *s, char **err) {
    sqtab_t t;
    if (load_table(db, sch, s->table, false, &t, err) != 0) {
        sqtab_free(&t);
        return NULL;
    }
    sqv_t *row = NULL;
    char *out = NULL;
    if (t.pk.n > 1) {
        *err = xasprintf("SQLite table '%s' has a composite primary key; use '?where=' instead", s->table);
    } else if (t.pk.n == 1 && t.pk.v[0] != t.ipk) {
        const sqcol_t *col = &t.cols.v[t.pk.v[0]];
        char *ut = str_lower(col->type);
        pkfind_t f = {t.pk.v[0], sqv_null(), col->aff, false, NULL, t.cols.n};
        int64_t iv;
        if (strstr(ut, "int")) {
            if (!parse_int_key(s->key, &iv)) *err = xasprintf("Primary key '%s' must be an integer; got '%s'", s->key, s->key);
            else f.key = sqv_int(iv);
        } else if (strstr(ut, "real") || strstr(ut, "floa") || strstr(ut, "doub")) {
            char *e;
            double d = strtod(s->key, &e);
            f.key = (*s->key && blank(e) && isfinite(d)) ? sqv_real(d) : sqv_text_own(xstrdup(s->key));
        } else {
            f.key = sqv_text_own(xstrdup(s->key));
        }
        free(ut);
        if (!*err) {
            if (tab_scan(db, &t, pkfind_cb, &f) < 0) *err = xstrdup(db->err ? db->err : "database disk image is malformed");
            row = f.row;
        }
        sqv_free(&f.key);
    } else {
        if (t.without_rowid) {
            *err = xasprintf("SQLite table '%s' does not expose ROWID; use '?where=' instead", s->table);
        } else if (t.pk.n == 0 && find_col(&t, "rowid") >= 0 && find_col(&t, "_rowid_") >= 0 && find_col(&t, "oid") >= 0) {
            *err = xasprintf("SQLite table '%s' shadows all ROWID aliases; row-key access is unavailable", s->table);
        } else {
            int64_t rid;
            if (!parse_int_key(s->key, &rid)) {
                *err = t.pk.n ? xasprintf("Primary key '%s' must be an integer; got '%s'", s->key, s->key)
                              : xasprintf("SQLite ROWID must be an integer; got '%s'", s->key);
            } else {
                rowid_find_t f = {&t, NULL};
                if (sq_find_rowid(db, t.root, rid, rowid_find_cb, &f) < 0)
                    *err = xstrdup(db->err ? db->err : "database disk image is malformed");
                row = f.row;
            }
        }
    }
    if (!*err) {
        if (!row) {
            out = xasprintf("No row found in table '%s' for key '%s'.", s->table, s->key);
        } else if (!t.cols.n) {
            out = xstrdup("(no columns)");
        } else {
            sb_t b = SB_INIT;
            for (size_t c = 0; c < t.cols.n; c++) {
                char *v = sqv_display(&row[c]);
                char *line = xasprintf("%s: %s", t.cols.v[c].name, v);
                char *r = replace_tabs(line), *tw = trunc_width(r, SQ_RENDER_WIDTH);
                if (c) sb_putc(&b, '\n');
                sb_append(&b, tw);
                free(tw);
                free(r);
                free(line);
                free(v);
            }
            out = sb_detach(&b);
        }
    }
    if (row) {
        for (size_t c = 0; c < t.cols.n; c++) sqv_free(&row[c]);
        free(row);
    }
    sqtab_free(&t);
    return out;
}

static char *sq_query(sqdb_t *db, sqschemas_t *sch, const sqsel_t *s, char **err) {
    sqtab_t t;
    if (load_table(db, sch, s->table, false, &t, err) != 0) {
        sqtab_free(&t);
        return NULL;
    }
    qx_t q;
    uint64_t total = 0;
    char *out = NULL;
    if (query_rows(db, &t, s->where, s->order, s->limit, s->offset, &q, &total, err) == 0) {
        strvec_t cols;
        table_cols(&t, &cols);
        sb_t b = SB_INIT;
        char *tab = render_rows(&cols, &q.out);
        sb_append(&b, tab);
        free(tab);
        strvec_free(&cols);
        uint64_t shown = MIN(total, (uint64_t)s->offset + q.out.n);
        if (shown < total) {
            sb_t p = SB_INIT;
            sb_appendf(&p, "[%llu more rows; append :%s?limit=%lld&offset=%llu", (unsigned long long)(total - shown),
                       s->table, (long long)s->limit, (unsigned long long)(s->offset + q.out.n));
            if (s->where) {
                sb_append(&p, "&where=");
                url_encode(&p, s->where);
            }
            if (s->order) {
                sb_append(&p, "&order=");
                url_encode(&p, s->order);
            }
            sb_append(&p, " to the database path to continue]");
            char *r = replace_tabs(sb_cstr(&p));
            sb_appendf(&b, "\n%s", r);
            free(r);
            sb_free(&p);
        }
        out = sb_detach(&b);
    }
    qx_free(&q);
    sqtab_free(&t);
    return out;
}

#define RAW_UNSUPPORTED                                                                                                \
    "omc's built-in SQLite reader supports single-table queries only: SELECT <columns|expressions|aggregates> "      \
    "[FROM <table>] [WHERE ...] [ORDER BY ...] [LIMIT n [OFFSET m]]"

/* end (exclusive token index) of an expression starting at i: stops at a top-level ',' or clause keyword */
static size_t expr_span_end(const sqtok_t *t, size_t i) {
    int depth = 0;
    for (; t[i].t != TK_EOF; i++) {
        if (tok_op(&t[i], "(")) depth++;
        else if (tok_op(&t[i], ")")) {
            if (!depth) break;
            depth--;
        } else if (!depth && (tok_op(&t[i], ",") || tok_op(&t[i], ";") || tok_kw(&t[i], "from") || tok_kw(&t[i], "where") ||
                              tok_kw(&t[i], "order") || tok_kw(&t[i], "limit") || tok_kw(&t[i], "group") ||
                              tok_kw(&t[i], "having") || tok_kw(&t[i], "union") || tok_kw(&t[i], "as")))
            break;
    }
    return i;
}

static int64_t eval_const_int(sqp_t *p, bool *ok) {
    sqx_t *x = px_expr(p);
    *ok = x != NULL;
    if (!x) return 0;
    sqctx_t cx = {NULL, 0};
    sqv_t v = ev(x, &cx), n = to_numeric(&v);
    sqv_free(&v);
    sqx_free(x);
    return n.t == SV_INT ? n.i : (int64_t)n.d;
}

static char *sq_raw(sqdb_t *db, sqschemas_t *sch, const sqsel_t *s, char **err) {
    sqtoks_t tk;
    if (sql_lex(s->sql, &tk, SQ_MAX_QUERY_TOKENS, err) != 0) return NULL;
    sqtok_t *t = tk.v;
    sqtab_t tab;
    memset(&tab, 0, sizeof tab);
    bool have_tab = false;
    qx_t q;
    memset(&q, 0, sizeof q);
    strvec_t cols = {0};
    char *out = NULL;
    sqp_t p = {0};
    p.src = s->sql;
    p.t = t;
    p.n = tk.n;
    if (!tok_kw(&t[0], "select")) {
        *err = xstrdup(tok_kw(&t[0], "with") || tok_kw(&t[0], "pragma") || tok_kw(&t[0], "explain") ||
                               tok_kw(&t[0], "values")
                           ? RAW_UNSUPPORTED
                           : "attempt to write a readonly database (omc's SQLite reader runs SELECT queries only)");
        goto out;
    }
    /* locate FROM at depth 0 to load the table before resolving column names */
    size_t from = 0;
    int depth = 0;
    for (size_t i = 1; t[i].t != TK_EOF; i++) {
        if (tok_op(&t[i], "(")) depth++;
        else if (tok_op(&t[i], ")")) depth--;
        else if (!depth && tok_kw(&t[i], "from")) {
            from = i;
            break;
        }
    }
    if (from) {
        if (!tok_name(&t[from + 1])) {
            *err = xstrdup(RAW_UNSUPPORTED);
            goto out;
        }
        size_t ni = from + 1;
        if (tok_op(&t[ni + 1], ".") && tok_name(&t[ni + 2])) ni += 2; /* main.tbl */
        if (load_table(db, sch, t[ni].v, true, &tab, err) != 0) {
            if (!strncmp(*err, "SQLite table '", 14)) {
                free(*err);
                *err = xasprintf("no such table: %s", t[ni].v);
            }
            goto out;
        }
        have_tab = true;
        q.t = &tab;
        p.tab = &tab;
    }
    p.i = 1;
    p.allow_agg = true;
    if (pk_kw(&p, "distinct")) {
        *err = xstrdup("SELECT DISTINCT is not supported by omc's SQLite reader");
        goto out;
    }
    pk_kw(&p, "all");
    /* result columns */
    for (;;) {
        size_t st = p.i;
        if (tok_op(pk_peek(&p), "*") || (tok_name(pk_peek(&p)) && tok_op(&t[p.i + 1], ".") && tok_op(&t[p.i + 2], "*"))) {
            p.i += tok_op(pk_peek(&p), "*") ? 1 : 3;
            if (!have_tab) {
                *err = xstrdup("no tables specified");
                goto out;
            }
            for (size_t c = 0; c < tab.cols.n; c++) {
                sqx_t *x = mk(X_COL);
                x->col = (int)c;
                x->aff = tab.cols.v[c].aff;
                vec_push(&q.proj, x);
                strvec_push(&cols, tab.cols.v[c].name);
            }
        } else {
            size_t e = expr_span_end(t, st);
            sqx_t *x = px_expr(&p);
            if (!x) {
                *err = p.err ? p.err : xstrdup("syntax error");
                p.err = NULL;
                goto out;
            }
            vec_push(&q.proj, x);
            char *name = NULL;
            if (pk_kw(&p, "as")) {
                if (!tok_name(pk_peek(&p))) {
                    *err = xstrdup("near \"AS\": syntax error");
                    goto out;
                }
                name = xstrdup(pk_peek(&p)->v);
                p.i++;
            } else if ((pk_peek(&p)->t == TK_ID && !tok_kw(pk_peek(&p), "from") && !tok_kw(pk_peek(&p), "where") &&
                        !tok_kw(pk_peek(&p), "order") && !tok_kw(pk_peek(&p), "limit") && !tok_kw(pk_peek(&p), "group")) ||
                       pk_peek(&p)->t == TK_QID) {
                name = xstrdup(pk_peek(&p)->v);
                p.i++;
            }
            if (!name) {
                size_t last = MAX(e, st + 1) - 1;
                size_t a = t[st].pos, b = t[last].pos + t[last].len;
                name = xstrndup(s->sql + a, b > a ? b - a : 0);
            }
            strvec_push_own(&cols, name);
        }
        if (!pk_op(&p, ",")) break;
    }
    for (size_t i = 0; i < p.aggs.n; i++) vec_push(&q.aggs, p.aggs.v[i]);
    p.allow_agg = false;
    q.ncols = q.proj.n;
    if (from) {
        if (p.i != from) {
            *err = p.err ? p.err : xasprintf("near \"%s\": syntax error", pk_peek(&p)->v ? pk_peek(&p)->v : "");
            p.err = NULL;
            goto out;
        }
        p.i = from + 2;
        if (tok_op(pk_peek(&p), ".")) p.i += 2;
        if (pk_kw(&p, "as")) p.i++;
        else if (pk_peek(&p)->t == TK_ID && !tok_kw(pk_peek(&p), "where") && !tok_kw(pk_peek(&p), "order") &&
                 !tok_kw(pk_peek(&p), "limit") && !tok_kw(pk_peek(&p), "group") && !tok_kw(pk_peek(&p), "join") &&
                 !tok_kw(pk_peek(&p), "natural") && !tok_kw(pk_peek(&p), "left") && !tok_kw(pk_peek(&p), "inner") &&
                 !tok_kw(pk_peek(&p), "cross") && !tok_kw(pk_peek(&p), "union"))
            p.i++;
    }
    if (pk_kw(&p, "where")) {
        q.where = px_expr(&p);
        if (!q.where) {
            *err = p.err ? p.err : xstrdup("syntax error");
            p.err = NULL;
            goto out;
        }
    }
    if (pk_kw(&p, "order")) {
        if (!pk_kw(&p, "by")) {
            *err = xstrdup("near \"ORDER\": syntax error");
            goto out;
        }
        do {
            sqtok_t *ot = pk_peek(&p);
            int pi = -1;
            if (ot->t == TK_NUM && (tok_op(&t[p.i + 1], ",") || t[p.i + 1].t == TK_EOF || tok_kw(&t[p.i + 1], "asc") ||
                                    tok_kw(&t[p.i + 1], "desc") || tok_kw(&t[p.i + 1], "limit") || tok_op(&t[p.i + 1], ";"))) {
                long long n = strtoll(ot->v, NULL, 10);
                if (n < 1 || n > (long long)q.proj.n) {
                    *err = xasprintf("ORDER BY term out of range - should be between 1 and %zu", q.proj.n);
                    goto out;
                }
                pi = (int)n - 1;
                p.i++;
            } else if (tok_name(ot) && (!have_tab || find_col(&tab, ot->v) < 0)) {
                for (size_t c = 0; c < cols.n; c++)
                    if (!strcasecmp(cols.v[c], ot->v)) pi = (int)c;
                if (pi >= 0) p.i++;
            }
            sqx_t *x = NULL;
            if (pi < 0) {
                x = px_expr(&p);
                if (!x) {
                    *err = p.err ? p.err : xstrdup("syntax error");
                    p.err = NULL;
                    goto out;
                }
            }
            vec_push(&q.order, x);
            vec_push(&q.order_proj, pi);
            bool d = false;
            if (pk_kw(&p, "desc")) d = true;
            else pk_kw(&p, "asc");
            if (pk_kw(&p, "nulls")) {
                *err = xstrdup("NULLS FIRST/LAST is not supported by omc's SQLite reader");
                goto out;
            }
            vec_push(&q.desc, d);
        } while (pk_op(&p, ","));
    }
    int64_t lim = -1, off = 0;
    if (pk_kw(&p, "limit")) {
        bool ok;
        lim = eval_const_int(&p, &ok);
        if (ok && pk_kw(&p, "offset")) off = eval_const_int(&p, &ok);
        else if (ok && pk_op(&p, ",")) {
            off = lim;
            lim = eval_const_int(&p, &ok);
        }
        if (!ok) {
            *err = p.err ? p.err : xstrdup("syntax error");
            p.err = NULL;
            goto out;
        }
        if (off < 0) off = 0;
    }
    pk_op(&p, ";");
    if (pk_peek(&p)->t != TK_EOF) {
        sqtok_t *bad = pk_peek(&p);
        bool unsupported = tok_kw(bad, "group") || tok_kw(bad, "having") || tok_kw(bad, "join") || tok_kw(bad, "union") ||
                           tok_kw(bad, "intersect") || tok_kw(bad, "except") || tok_op(bad, ",") || tok_kw(bad, "natural") ||
                           tok_kw(bad, "left") || tok_kw(bad, "inner") || tok_kw(bad, "cross") || tok_kw(bad, "window");
        *err = unsupported ? xstrdup(RAW_UNSUPPORTED) : xasprintf("near \"%s\": syntax error", bad->v ? bad->v : "");
        goto out;
    }
    if (q.aggs.n) /* bare columns next to aggregates are evaluated without a row */
        for (size_t i = 0; i < q.order.n; i++)
            if (q.order_proj.v[i] < 0) {
                *err = xstrdup("ORDER BY with aggregates is not supported by omc's SQLite reader");
                goto out;
            }
    /* window: SQL LIMIT/OFFSET, then the read tool's page or row cap */
    int64_t cap = s->page ? s->limit : SQ_MAX_RAW_ROWS;
    int64_t poff = s->page ? s->offset : 0;
    q.skip = off + poff;
    int64_t avail = lim < 0 ? -1 : MAX((int64_t)0, lim - poff);
    q.take = avail < 0 ? cap + 1 : MIN(avail, cap + 1);
    if (qx_run(db, &q) != 0) {
        *err = xstrdup(q.err);
        goto out;
    }
    bool truncated = (int64_t)q.out.n > cap;
    if (truncated) {
        for (size_t i = (size_t)cap; i < q.out.n; i++) {
            for (size_t c = 0; c < q.ncols; c++) sqv_free(&q.out.v[i][c]);
            free(q.out.v[i]);
        }
        q.out.n = (size_t)cap;
    }
    sb_t b = SB_INIT;
    char *tabtxt = render_rows(&cols, &q.out);
    sb_append(&b, tabtxt);
    free(tabtxt);
    if (truncated && s->page) {
        sb_append(&b, "\n[More rows; append ?q=");
        url_encode(&b, s->sql);
        sb_appendf(&b, "&limit=%lld&offset=%lld to the database path to continue]", (long long)s->limit,
                   (long long)(s->offset + (int64_t)q.out.n));
    } else if (truncated) {
        sb_appendf(&b, "\n[Output capped at %d rows; add a LIMIT/OFFSET clause to the query to page through more]",
                   SQ_MAX_RAW_ROWS);
    }
    out = sb_detach(&b);
out:
    free(p.err);
    vec_free(&p.aggs);
    qx_free(&q);
    strvec_free(&cols);
    if (have_tab || tab.name) sqtab_free(&tab);
    sqtoks_free(&tk);
    return out;
}

bool rf_sqlite_sniff(const char *abs_path) {
    int fd = open(abs_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    char h[16];
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && pread_full(fd, h, 16, 0) && !memcmp(h, "SQLite format 3", 16);
    close(fd);
    return ok;
}

char *rf_sqlite_read(const char *abs_path, const char *sub, const char *query, bool *is_error) {
    *is_error = false;
    sqsel_t s;
    char *err = NULL, *out = NULL;
    if (parse_selector(sub ? sub : "", query ? query : "", &s, &err) != 0) {
        sqsel_free(&s);
        *is_error = true;
        return err;
    }
    sqdb_t db;
    if (sq_open(&db, abs_path, &err) != 0) {
        sq_close(&db);
        sqsel_free(&s);
        *is_error = true;
        return err;
    }
    sqschemas_t sch;
    if (load_schema(&db, &sch, &err) == 0) {
        switch (s.kind) {
        case SEL_LIST: out = sq_list(&db, &sch); break;
        case SEL_SCHEMA: out = sq_schema(&db, &sch, &s, &err); break;
        case SEL_ROW: out = sq_row(&db, &sch, &s, &err); break;
        case SEL_QUERY: out = sq_query(&db, &sch, &s, &err); break;
        default: out = sq_raw(&db, &sch, &s, &err); break;
        }
    }
    schemas_free(&sch);
    sq_close(&db);
    sqsel_free(&s);
    if (!out) {
        *is_error = true;
        return err ? err : xstrdup("SQLite read failed");
    }
    free(err);
    return out;
}

/* ---------- Jupyter notebooks (upstream pi-edit notebook.rs notebook_to_editable_text) ---------- */

/* ^# %%+ \[(code|markdown|raw)\]( cell:\d+)?$ */
static bool nb_marker_like(const char *l, size_t n) {
    if (n < 5 || memcmp(l, "# %%", 4)) return false;
    size_t i = 4;
    while (i < n && l[i] == '%') i++;
    if (i >= n || l[i] != ' ') return false;
    i++;
    static const char *const T[] = {"[code]", "[markdown]", "[raw]"};
    size_t tl = 0;
    for (size_t k = 0; k < 3; k++)
        if (n - i >= strlen(T[k]) && !memcmp(l + i, T[k], strlen(T[k]))) tl = strlen(T[k]);
    if (!tl) return false;
    i += tl;
    if (i == n) return true;
    if (n - i < 7 || memcmp(l + i, " cell:", 6)) return false;
    for (i += 6; i < n; i++)
        if (!isdigit((unsigned char)l[i])) return false;
    return true;
}

char *rf_ipynb_text(const char *json, size_t len, const char *display, char **err) {
    if (len >= 3 && !memcmp(json, "\xEF\xBB\xBF", 3)) json += 3, len -= 3;
    char *perr = NULL;
    json_t *nb = json_parse(json, len, &perr);
    free(perr);
    if (!nb) {
        *err = xasprintf("Invalid JSON in notebook: %s", display);
        return NULL;
    }
    char *out = NULL;
    json_t *cells = json_get_arr(nb, "cells");
    if (!json_is_obj(nb)) *err = xasprintf("Invalid notebook structure (expected object): %s", display);
    else if (!cells) *err = xasprintf("Invalid notebook structure (missing cells array): %s", display);
    else {
        size_t n = json_len(cells);
        for (size_t i = 0; i < n && !*err; i++) {
            const char *ty = json_get_str(json_at(cells, i), "cell_type");
            if (!json_is_obj(json_at(cells, i)) || !ty || (strcmp(ty, "code") && strcmp(ty, "markdown") && strcmp(ty, "raw")))
                *err = xasprintf("Invalid notebook cell %zu in %s", i, display);
        }
        if (!*err) {
            sb_t b = SB_INIT;
            for (size_t i = 0; i < n; i++) {
                json_t *c = json_at(cells, i), *src = json_get(c, "source");
                sb_t s = SB_INIT;
                if (json_is_str(src)) sb_append(&s, json_sv(src));
                else if (json_is_arr(src))
                    for (size_t k = 0; k < json_len(src); k++)
                        if (json_is_str(json_at(src, k))) sb_append(&s, json_sv(json_at(src, k)));
                if (i) sb_putc(&b, '\n');
                sb_appendf(&b, "# %%%% [%s] cell:%zu", json_get_str(c, "cell_type"), i);
                if (s.len) {
                    sb_putc(&b, '\n');
                    const char *p = sb_cstr(&s);
                    bool any = strstr(p, "# %%") != NULL;
                    while (*p) {
                        const char *nl = strchr(p, '\n');
                        size_t ln = nl ? (size_t)(nl - p) : strlen(p);
                        if (any && nb_marker_like(p, ln)) sb_append(&b, "# %%"), sb_appendn(&b, p + 3, ln - 3);
                        else sb_appendn(&b, p, ln);
                        if (!nl) break;
                        sb_putc(&b, '\n');
                        p = nl + 1;
                    }
                }
                sb_free(&s);
            }
            out = sb_detach(&b);
        }
    }
    json_free(nb);
    return out;
}

/* ---------- PDF text extraction ----------
 * Objects are located by scanning for "N G obj" (later definitions win, like incremental updates) plus the
 * members of object streams; the catalog comes from the last trailer / XRef stream dictionary with /Root.
 * Content streams are interpreted for text operators with font decoding (ToUnicode CMaps, simple-font
 * encodings with /Differences) and glyph widths to place spaces and line breaks. */

#define PDF_MAX_DEPTH 48
#define PDF_MAX_OBJNUM 4000000
#define PDF_MAX_STREAM (64u << 20)
#define PDF_MAX_DECODE_TOTAL (768u << 20)
#define PDF_MAX_ARENA (768u << 20)
#define PDF_MAX_PAGES 50000
#define PDF_MAX_OPS 5000000
#define PDF_MAX_FORM_DEPTH 8
#define PDF_MAX_TU 400000

enum { PO_NULL, PO_BOOL, PO_NUM, PO_STR, PO_NAME, PO_ARR, PO_DICT, PO_REF, PO_STREAM, PO_KW };

typedef struct pobj {
    uint8_t t;
    bool b, is_int;
    double num;
    char *s; /* STR bytes / NAME / KW (NUL-terminated) */
    size_t n;
    struct pobj **v; /* ARR items; DICT alternating key (NAME) / value */
    size_t nv;
    int rn;
    struct pobj *dict; /* STREAM */
    const uint8_t *data;
    size_t dlen;
} pobj_t;

typedef struct ablk {
    struct ablk *next;
    size_t used, cap;
    max_align_t data[];
} ablk_t;

typedef struct {
    uint64_t off; /* direct: offset just past "obj"; compressed: index inside the object stream */
    uint64_t pos; /* file position used for precedence */
    uint32_t stm;
    uint8_t kind; /* 0 none, 1 direct, 2 in object stream */
    bool busy;
    pobj_t *cache;
    uint8_t *dec; /* object stream: decoded bytes (owned) */
    size_t declen;
    size_t *offs; /* object stream: member offsets relative to /First (owned) */
    size_t noffs, first;
} pxref_t;

typedef struct pfont pfont_t;

typedef struct {
    const uint8_t *buf;
    size_t len;
    pxref_t *x;
    size_t nx;
    ablk_t *arena;
    size_t arena_total;
    size_t decoded;
    VEC(pobj_t *) font_keys;
    VEC(pfont_t *) fonts;
    pfont_t *default_font;
    bool encrypted;
    pobj_t oom; /* scratch object handed out when the arena is exhausted */
} pdf_t;

static void *pa_alloc(pdf_t *d, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!d->arena || d->arena->cap - d->arena->used < n) {
        size_t cap = MAX(n, (size_t)256 << 10);
        if (d->arena_total + cap > PDF_MAX_ARENA) return NULL;
        ablk_t *b = xmalloc(sizeof(ablk_t) + cap);
        b->next = d->arena;
        b->used = 0;
        b->cap = cap;
        d->arena = b;
        d->arena_total += cap;
    }
    void *p = (char *)d->arena->data + d->arena->used;
    d->arena->used += n;
    memset(p, 0, n);
    return p;
}

static pobj_t PDF_NULLOBJ = {0};

static pobj_t *po_new(pdf_t *d, int t) {
    pobj_t *o = pa_alloc(d, sizeof(pobj_t));
    if (!o) {
        memset(&d->oom, 0, sizeof d->oom);
        return &d->oom;
    }
    o->t = (uint8_t)t;
    return o;
}

static char *pa_strndup(pdf_t *d, const char *s, size_t n) {
    char *r = pa_alloc(d, n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* --- lexer --- */
typedef struct {
    const uint8_t *s;
    size_t n, i;
} plex_t;

static bool pws(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static bool pdelim(int c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }
static bool preg(int c) { return !pws(c) && !pdelim(c); }

static void pskip(plex_t *L) {
    while (L->i < L->n) {
        int c = L->s[L->i];
        if (pws(c)) L->i++;
        else if (c == '%') {
            while (L->i < L->n && L->s[L->i] != '\n' && L->s[L->i] != '\r') L->i++;
        } else break;
    }
}

static void lex_litstr(plex_t *L, sb_t *b) {
    int depth = 1;
    L->i++; /* '(' */
    while (L->i < L->n) {
        int c = L->s[L->i++];
        if (c == '(') depth++;
        else if (c == ')') {
            if (--depth == 0) return;
        } else if (c == '\\') {
            if (L->i >= L->n) return;
            int e = L->s[L->i++];
            switch (e) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '\r':
                if (L->i < L->n && L->s[L->i] == '\n') L->i++;
                continue;
            case '\n': continue;
            default:
                if (e >= '0' && e <= '7') {
                    int v = e - '0';
                    for (int k = 0; k < 2 && L->i < L->n && L->s[L->i] >= '0' && L->s[L->i] <= '7'; k++) v = v * 8 + (L->s[L->i++] - '0');
                    c = v & 0xff;
                } else c = e;
            }
        }
        sb_putc(b, (char)c);
    }
}

static void lex_hexstr(plex_t *L, sb_t *b) {
    L->i++; /* '<' */
    int hi = -1;
    while (L->i < L->n) {
        int c = L->s[L->i++];
        if (c == '>') break;
        int v = hexval(c);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else {
            sb_putc(b, (char)(hi * 16 + v));
            hi = -1;
        }
    }
    if (hi >= 0) sb_putc(b, (char)(hi * 16));
}

static char *lex_name(plex_t *L, size_t *len) {
    L->i++; /* '/' */
    sb_t b = SB_INIT;
    while (L->i < L->n && preg(L->s[L->i])) {
        int c = L->s[L->i++];
        if (c == '#' && L->i + 1 < L->n && hexval(L->s[L->i]) >= 0 && hexval(L->s[L->i + 1]) >= 0) {
            c = hexval(L->s[L->i]) * 16 + hexval(L->s[L->i + 1]);
            L->i += 2;
        }
        sb_putc(&b, (char)c);
    }
    *len = b.len;
    return sb_detach(&b);
}

/* read a regular token [start,end) */
static size_t lex_regular(plex_t *L) {
    size_t st = L->i;
    while (L->i < L->n && preg(L->s[L->i])) L->i++;
    return st;
}

static bool tok_number(const uint8_t *s, size_t n, double *v, bool *is_int) {
    if (!n || n > 63) return false;
    char buf[64];
    memcpy(buf, s, n);
    buf[n] = 0;
    size_t k = 0;
    if (buf[k] == '+' || buf[k] == '-') k++;
    bool digits = false, dot = false;
    for (; k < n; k++) {
        if (isdigit((unsigned char)buf[k])) digits = true;
        else if (buf[k] == '.' && !dot) dot = true;
        else if (buf[k] == '-' && k > 0) { /* "0.00-1" style garbage: keep the prefix */
            buf[k] = 0;
            break;
        } else return false;
    }
    if (!digits) return false;
    *v = strtod(buf, NULL);
    *is_int = !dot;
    return true;
}

static pobj_t *pobj_resolve(pdf_t *d, pobj_t *o);
static pobj_t *pdf_get(pdf_t *d, size_t num);

/* parse one object; NULL at end of input. Each call consumes at least one byte. */
static pobj_t *pparse(pdf_t *d, plex_t *L, int depth) {
    pskip(L);
    if (L->i >= L->n) return NULL;
    int c = L->s[L->i];
    if (depth > PDF_MAX_DEPTH) {
        L->i++;
        return &PDF_NULLOBJ;
    }
    if (c == '/') {
        size_t n;
        char *s = lex_name(L, &n);
        pobj_t *o = po_new(d, PO_NAME);
        if (o != &d->oom) {
            o->s = pa_strndup(d, s, n);
            o->n = n;
        }
        free(s);
        return o;
    }
    if (c == '(' || (c == '<' && (L->i + 1 >= L->n || L->s[L->i + 1] != '<'))) {
        sb_t b = SB_INIT;
        if (c == '(') lex_litstr(L, &b);
        else lex_hexstr(L, &b);
        pobj_t *o = po_new(d, PO_STR);
        if (o != &d->oom) {
            o->s = pa_strndup(d, b.s ? b.s : "", b.len);
            o->n = b.len;
        }
        sb_free(&b);
        return o;
    }
    if (c == '[' || c == '<') {
        bool dict = c == '<';
        L->i += dict ? 2 : 1;
        VEC(pobj_t *) items = {0};
        for (;;) {
            pskip(L);
            if (L->i >= L->n) break;
            if (!dict && L->s[L->i] == ']') {
                L->i++;
                break;
            }
            if (dict && L->s[L->i] == '>' && L->i + 1 < L->n && L->s[L->i + 1] == '>') {
                L->i += 2;
                break;
            }
            pobj_t *it = pparse(d, L, depth + 1);
            if (!it) break;
            if (it->t == PO_KW && (!strcmp(it->s ? it->s : "", "endobj") || !strcmp(it->s ? it->s : "", "stream"))) {
                L->i -= strlen(it->s); /* malformed: let the caller see it */
                break;
            }
            if (items.n < 4000000) vec_push(&items, it);
        }
        pobj_t *o = po_new(d, dict ? PO_DICT : PO_ARR);
        if (dict && items.n % 2) items.n--;
        if (o != &d->oom && items.n) {
            o->v = pa_alloc(d, items.n * sizeof(pobj_t *));
            if (o->v) {
                memcpy(o->v, items.v, items.n * sizeof(pobj_t *));
                o->nv = items.n;
            }
        }
        vec_free(&items);
        return o;
    }
    if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
        L->i += (c == '>' && L->i + 1 < L->n && L->s[L->i + 1] == '>') ? 2 : 1;
        pobj_t *o = po_new(d, PO_KW);
        if (o != &d->oom) o->s = pa_strndup(d, (const char *)&L->s[L->i - 1], 1);
        return o;
    }
    size_t st = lex_regular(L);
    size_t tl = L->i - st;
    double v;
    bool is_int;
    if (tok_number(L->s + st, tl, &v, &is_int)) {
        if (is_int && v >= 0) { /* "N G R" */
            size_t save = L->i;
            pskip(L);
            size_t s2 = lex_regular(L);
            double g;
            bool gi;
            if (L->i > s2 && tok_number(L->s + s2, L->i - s2, &g, &gi) && gi && g >= 0) {
                pskip(L);
                if (L->i < L->n && L->s[L->i] == 'R' && (L->i + 1 >= L->n || !preg(L->s[L->i + 1]))) {
                    L->i++;
                    pobj_t *o = po_new(d, PO_REF);
                    o->rn = v > PDF_MAX_OBJNUM ? -1 : (int)v;
                    return o;
                }
            }
            L->i = save;
        }
        pobj_t *o = po_new(d, PO_NUM);
        o->num = v;
        o->is_int = is_int;
        return o;
    }
    if (tl == 4 && !memcmp(L->s + st, "true", 4)) {
        pobj_t *o = po_new(d, PO_BOOL);
        o->b = true;
        return o;
    }
    if (tl == 5 && !memcmp(L->s + st, "false", 5)) return po_new(d, PO_BOOL);
    if (tl == 4 && !memcmp(L->s + st, "null", 4)) return &PDF_NULLOBJ;
    pobj_t *o = po_new(d, PO_KW);
    if (o != &d->oom) o->s = pa_strndup(d, (const char *)L->s + st, tl ? tl : 1);
    if (!tl) L->i++;
    return o;
}

static pobj_t *dict_raw(pobj_t *dict, const char *key) {
    if (!dict) return NULL;
    if (dict->t == PO_STREAM) dict = dict->dict;
    if (!dict || dict->t != PO_DICT) return NULL;
    for (size_t i = 0; i + 1 < dict->nv; i += 2)
        if (dict->v[i]->t == PO_NAME && dict->v[i]->s && !strcmp(dict->v[i]->s, key)) return dict->v[i + 1];
    return NULL;
}

static pobj_t *pobj_resolve(pdf_t *d, pobj_t *o) {
    for (int k = 0; o && o->t == PO_REF && k < 16; k++) o = o->rn >= 0 ? pdf_get(d, (size_t)o->rn) : NULL;
    if (o && o->t == PO_REF) return NULL;
    return o;
}

static pobj_t *dget(pdf_t *d, pobj_t *dict, const char *key) { return pobj_resolve(d, dict_raw(dict, key)); }

static bool name_is(pobj_t *o, const char *n) { return o && o->t == PO_NAME && o->s && !strcmp(o->s, n); }
static double num_or(pobj_t *o, double def) { return o && o->t == PO_NUM ? o->num : def; }

/* after a dictionary in an indirect object: "stream" EOL data "endstream" */
static pobj_t *maybe_stream(pdf_t *d, plex_t *L, pobj_t *dict) {
    size_t save = L->i;
    pskip(L);
    if (L->n - L->i < 6 || memcmp(L->s + L->i, "stream", 6)) {
        L->i = save;
        return dict;
    }
    size_t p = L->i + 6;
    if (p < L->n && L->s[p] == '\r') p++;
    if (p < L->n && L->s[p] == '\n') p++;
    pobj_t *lenobj = dict_raw(dict, "Length");
    double ln = -1;
    if (lenobj && lenobj->t == PO_NUM) ln = lenobj->num;
    else if (lenobj && lenobj->t == PO_REF) {
        pobj_t *r = pobj_resolve(d, lenobj);
        ln = num_or(r, -1);
    }
    size_t dlen = 0;
    bool ok = false;
    if (ln >= 0 && ln <= (double)(L->n - p)) {
        size_t e = p + (size_t)ln, k = e;
        while (k < L->n && k < e + 4 && pws(L->s[k])) k++;
        if (L->n - k >= 9 && !memcmp(L->s + k, "endstream", 9)) {
            dlen = (size_t)ln;
            ok = true;
        }
    }
    if (!ok) {
        const uint8_t *es = memmem(L->s + p, L->n - p, "endstream", 9);
        size_t e = es ? (size_t)(es - L->s) : L->n;
        if (e > p && L->s[e - 1] == '\n') e--;
        if (e > p && L->s[e - 1] == '\r') e--;
        dlen = e - p;
    }
    pobj_t *s = po_new(d, PO_STREAM);
    if (s == &d->oom) return dict;
    s->dict = dict;
    s->data = L->s + p;
    s->dlen = dlen;
    L->i = MIN(L->n, p + dlen);
    return s;
}

/* --- filters --- */

/* tolerant zlib inflate: keeps whatever decoded before an error */
static uint8_t *pz_inflate(const uint8_t *in, size_t n, size_t cap, size_t *outlen) {
    z_stream z;
    memset(&z, 0, sizeof z);
    bool raw = n < 2 || ((in[0] & 0x0f) != 8) || (((unsigned)in[0] << 8 | in[1]) % 31);
    if (inflateInit2(&z, raw ? -15 : 15) != Z_OK) return NULL;
    size_t acap = MIN(cap, MAX(n * 4, (size_t)4096)), used = 0;
    uint8_t *out = xmalloc(acap + 1);
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)MIN(n, (size_t)UINT32_MAX);
    for (;;) {
        if (used == acap) {
            if (acap >= cap) break;
            acap = MIN(cap, acap * 2);
            out = xrealloc(out, acap + 1);
        }
        z.next_out = out + used;
        z.avail_out = (uInt)MIN(acap - used, (size_t)UINT32_MAX);
        size_t before = z.avail_out;
        int rc = inflate(&z, Z_NO_FLUSH);
        used += before - z.avail_out;
        if (rc == Z_STREAM_END || (rc != Z_OK && rc != Z_BUF_ERROR)) break;
        if (rc == Z_BUF_ERROR && z.avail_out) break;
        if (!z.avail_in && z.avail_out) break;
    }
    inflateEnd(&z);
    *outlen = used;
    return out;
}

static uint8_t *png_unpredict(const uint8_t *in, size_t n, int colors, int bpc, int columns, size_t *outlen) {
    size_t bpp = (size_t)MAX(1, colors * bpc / 8);
    size_t row = ((size_t)colors * (size_t)bpc * (size_t)columns + 7) / 8;
    if (!row || row > (1u << 24)) return NULL;
    size_t rows = n / (row + 1);
    uint8_t *out = xmalloc(rows * row + 1), *prev = xcalloc(row, 1);
    for (size_t r = 0; r < rows; r++) {
        const uint8_t *src = in + r * (row + 1);
        uint8_t ft = src[0], *dst = out + r * row;
        for (size_t i = 0; i < row; i++) {
            int a = i >= bpp ? dst[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = src[1 + i], v;
            switch (ft) {
            case 1: v = x + a; break;
            case 2: v = x + b; break;
            case 3: v = x + (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                v = x + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
                break;
            }
            default: v = x;
            }
            dst[i] = (uint8_t)v;
        }
        memcpy(prev, dst, row);
    }
    free(prev);
    *outlen = rows * row;
    return out;
}

static uint8_t *a85_decode(const uint8_t *in, size_t n, size_t *outlen) {
    sb_t b = SB_INIT;
    uint32_t tuple = 0;
    int cnt = 0;
    for (size_t i = 0; i < n; i++) {
        int c = in[i];
        if (c == '~') break;
        if (pws(c)) continue;
        if (c == 'z' && cnt == 0) {
            sb_appendn(&b, "\0\0\0\0", 4);
            continue;
        }
        if (c < '!' || c > 'u') continue;
        tuple = tuple * 85 + (uint32_t)(c - '!');
        if (++cnt == 5) {
            char o[4] = {(char)(tuple >> 24), (char)(tuple >> 16), (char)(tuple >> 8), (char)tuple};
            sb_appendn(&b, o, 4);
            tuple = 0;
            cnt = 0;
        }
    }
    if (cnt > 1) {
        for (int k = cnt; k < 5; k++) tuple = tuple * 85 + 84;
        char o[4] = {(char)(tuple >> 24), (char)(tuple >> 16), (char)(tuple >> 8), (char)tuple};
        sb_appendn(&b, o, (size_t)cnt - 1);
    }
    *outlen = b.len;
    return (uint8_t *)sb_detach(&b);
}

static uint8_t *lzw_decode(const uint8_t *in, size_t n, int early, size_t cap, size_t *outlen) {
    uint16_t prefix[4096];
    uint8_t suffix[4096], first[4096];
    uint16_t lens[4096];
    sb_t b = SB_INIT;
    uint8_t stack[4096];
    int width = 9, next = 258, prev = -1;
    uint32_t acc = 0;
    int bits = 0;
    for (int i = 0; i < 256; i++) prefix[i] = 0, suffix[i] = (uint8_t)i, first[i] = (uint8_t)i, lens[i] = 1;
    for (size_t i = 0; i < n && b.len < cap;) {
        while (bits < width && i < n) acc = (acc << 8) | in[i++], bits += 8;
        if (bits < width) break;
        int code = (int)((acc >> (bits - width)) & ((1u << width) - 1));
        bits -= width;
        if (code == 256) {
            width = 9;
            next = 258;
            prev = -1;
            continue;
        }
        if (code == 257) break;
        int cur;
        if (code < next) cur = code;
        else if (code == next && prev >= 0) cur = prev;
        else break;
        int sp = 0, k = cur;
        while (k >= 256 && sp < 4095) {
            stack[sp++] = suffix[k];
            k = prefix[k];
        }
        stack[sp++] = (uint8_t)k;
        uint8_t f = stack[sp - 1];
        while (sp) sb_putc(&b, (char)stack[--sp]);
        if (code == next) sb_putc(&b, (char)f);
        if (prev >= 0 && next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = f;
            first[next] = first[prev];
            lens[next] = (uint16_t)MIN(4095, lens[prev] + 1);
            next++;
        }
        prev = code;
        if (next + early >= (1 << width) && width < 12) width++;
    }
    *outlen = b.len;
    return (uint8_t *)sb_detach(&b);
}

/* decode a stream through its filter chain; owned bytes or NULL (unsupported / budget exhausted) */
static uint8_t *pdf_stream(pdf_t *d, pobj_t *s, size_t *outlen) {
    if (!s || s->t != PO_STREAM) return NULL;
    pobj_t *f = dget(d, s, "Filter"), *dp = dget(d, s, "DecodeParms");
    if (!dp) dp = dget(d, s, "DP");
    size_t nf = !f ? 0 : f->t == PO_ARR ? f->nv : 1;
    uint8_t *cur = xmalloc(s->dlen + 1);
    memcpy(cur, s->data, s->dlen);
    size_t clen = s->dlen;
    for (size_t k = 0; k < nf && cur; k++) {
        pobj_t *fn = f->t == PO_ARR ? pobj_resolve(d, f->v[k]) : f;
        pobj_t *parm = dp && dp->t == PO_ARR ? (k < dp->nv ? pobj_resolve(d, dp->v[k]) : NULL) : dp;
        const char *nm = fn && fn->t == PO_NAME && fn->s ? fn->s : "";
        uint8_t *nx = NULL;
        size_t nl = 0;
        size_t budget = MIN((size_t)PDF_MAX_STREAM, PDF_MAX_DECODE_TOTAL - MIN(d->decoded, (size_t)PDF_MAX_DECODE_TOTAL));
        if (!budget) {
            free(cur);
            return NULL;
        }
        if (!strcmp(nm, "FlateDecode") || !strcmp(nm, "Fl")) nx = pz_inflate(cur, clen, budget, &nl);
        else if (!strcmp(nm, "LZWDecode") || !strcmp(nm, "LZW"))
            nx = lzw_decode(cur, clen, (int)num_or(dget(d, parm, "EarlyChange"), 1) ? 1 : 0, budget, &nl);
        else if (!strcmp(nm, "ASCIIHexDecode") || !strcmp(nm, "AHx")) {
            sb_t b = SB_INIT;
            plex_t L = {cur, clen, 0};
            uint8_t *tmp = xmalloc(clen + 2);
            tmp[0] = '<';
            memcpy(tmp + 1, cur, clen);
            tmp[clen + 1] = '>';
            L.s = tmp;
            L.n = clen + 2;
            lex_hexstr(&L, &b);
            free(tmp);
            nl = b.len;
            nx = (uint8_t *)sb_detach(&b);
        } else if (!strcmp(nm, "ASCII85Decode") || !strcmp(nm, "A85")) nx = a85_decode(cur, clen, &nl);
        else if (!strcmp(nm, "RunLengthDecode") || !strcmp(nm, "RL")) {
            sb_t b = SB_INIT;
            for (size_t i = 0; i < clen && b.len < budget;) {
                int l = cur[i++];
                if (l == 128) break;
                if (l < 128) {
                    size_t m = MIN((size_t)l + 1, clen - i);
                    sb_appendn(&b, (const char *)cur + i, m);
                    i += m;
                } else if (i < clen) {
                    for (int r = 0; r < 257 - l; r++) sb_putc(&b, (char)cur[i]);
                    i++;
                }
            }
            nl = b.len;
            nx = (uint8_t *)sb_detach(&b);
        } else if (!strcmp(nm, "Crypt")) {
            nx = cur;
            nl = clen;
            cur = NULL;
        } else {
            free(cur);
            return NULL; /* image codecs etc. carry no text */
        }
        free(cur);
        cur = nx;
        clen = nl;
        if (cur && (!strcmp(nm, "FlateDecode") || !strcmp(nm, "Fl") || !strcmp(nm, "LZWDecode") || !strcmp(nm, "LZW"))) {
            int pred = (int)num_or(dget(d, parm, "Predictor"), 1);
            if (pred >= 10) {
                size_t pl = 0;
                uint8_t *pd = png_unpredict(cur, clen, (int)MAX(1, MIN(num_or(dget(d, parm, "Colors"), 1), 32)),
                                            (int)MAX(1, MIN(num_or(dget(d, parm, "BitsPerComponent"), 8), 16)),
                                            (int)MAX(1, MIN(num_or(dget(d, parm, "Columns"), 1), 1 << 20)), &pl);
                free(cur);
                cur = pd;
                clen = pl;
            }
        }
        if (cur) d->decoded += clen;
    }
    if (cur) cur[clen] = 0, *outlen = clen;
    return cur;
}

/* --- objects --- */

static pobj_t *parse_objstm_member(pdf_t *d, pxref_t *x) {
    if (x->stm >= d->nx) return NULL;
    pxref_t *sx = &d->x[x->stm];
    if (!sx->dec || x->off >= sx->noffs) return NULL;
    size_t at = sx->first + sx->offs[x->off];
    if (at >= sx->declen) return NULL;
    plex_t M = {sx->dec, sx->declen, at};
    return pparse(d, &M, 0);
}

static pobj_t *pdf_get(pdf_t *d, size_t num) {
    if (num >= d->nx) return NULL;
    pxref_t *x = &d->x[num];
    if (x->cache) return x->cache;
    if (!x->kind || x->busy) return NULL;
    x->busy = true;
    pobj_t *o = NULL;
    if (x->kind == 1) {
        plex_t L = {d->buf, d->len, (size_t)x->off};
        o = pparse(d, &L, 0);
        if (o && o->t == PO_DICT) o = maybe_stream(d, &L, o);
    } else {
        o = parse_objstm_member(d, x);
    }
    x->busy = false;
    x->cache = o ? o : &PDF_NULLOBJ;
    return x->cache;
}

static void xref_set(pdf_t *d, size_t num, uint8_t kind, uint64_t off, uint64_t pos, uint32_t stm) {
    if (num > PDF_MAX_OBJNUM) return;
    if (num >= d->nx) {
        size_t nn = MAX(num + 1, d->nx * 2);
        nn = MIN(nn, (size_t)PDF_MAX_OBJNUM + 1);
        d->x = xrealloc(d->x, nn * sizeof(pxref_t));
        memset(d->x + d->nx, 0, (nn - d->nx) * sizeof(pxref_t));
        d->nx = nn;
    }
    pxref_t *x = &d->x[num];
    if (x->kind && x->pos > pos) return;
    x->kind = kind;
    x->off = off;
    x->pos = pos;
    x->stm = stm;
}

/* scan "N G obj" headers */
static void pdf_scan_objects(pdf_t *d) {
    const uint8_t *b = d->buf;
    size_t n = d->len, i = 0;
    while (i + 3 <= n) {
        const uint8_t *hit = memmem(b + i, n - i, "obj", 3);
        if (!hit) break;
        size_t k = (size_t)(hit - b);
        i = k + 3;
        if (k + 3 < n && preg(b[k + 3])) continue;
        size_t j = k;
        size_t ws = 0;
        while (j > 0 && pws(b[j - 1])) j--, ws++;
        if (!ws) continue;
        size_t ge = j;
        while (j > 0 && isdigit(b[j - 1]) && ge - j < 6) j--;
        if (j == ge) continue;
        ws = 0;
        while (j > 0 && pws(b[j - 1])) j--, ws++;
        if (!ws) continue;
        size_t ne = j;
        while (j > 0 && isdigit(b[j - 1]) && ne - j < 8) j--;
        if (j == ne || (j > 0 && preg(b[j - 1]))) continue;
        size_t num = 0;
        for (size_t q = j; q < ne; q++) num = num * 10 + (size_t)(b[q] - '0');
        xref_set(d, num, 1, k + 3, j, 0);
    }
}

static bool window_has(pdf_t *d, uint64_t off, const char *needle) {
    if (off >= d->len) return false;
    size_t w = MIN((size_t)4096, d->len - (size_t)off);
    const uint8_t *s = d->buf + off;
    const uint8_t *st = memmem(s, w, "stream", 6);
    if (st) w = (size_t)(st - s);
    return memmem(s, w, needle, strlen(needle)) != NULL;
}

static void pdf_index_objstms(pdf_t *d) {
    size_t n0 = d->nx;
    for (size_t num = 0; num < n0; num++) {
        if (d->x[num].kind != 1 || !window_has(d, d->x[num].off, "/ObjStm")) continue;
        pobj_t *so = pdf_get(d, num);
        if (!so || so->t != PO_STREAM || !name_is(dget(d, so, "Type"), "ObjStm")) continue;
        size_t len = 0;
        uint8_t *dec = pdf_stream(d, so, &len);
        if (!dec) continue;
        pxref_t *sx = &d->x[num];
        free(sx->dec);
        sx->dec = dec;
        sx->declen = len;
        double cnt = num_or(dget(d, so, "N"), 0), first = num_or(dget(d, so, "First"), 0);
        if (first < 0 || first > (double)len) continue;
        sx->first = (size_t)first;
        plex_t L = {dec, (size_t)first, 0};
        uint64_t pos = sx->pos;
        size_t ncnt = (size_t)MAX(0, MIN(cnt, 1000000));
        size_t *offs = xmalloc((ncnt + 1) * sizeof(size_t)), got = 0;
        VEC(size_t) nums = {0};
        for (size_t k = 0; k < ncnt; k++) {
            pobj_t *a = pparse(d, &L, 0), *b = pparse(d, &L, 0);
            if (!a || !b || a->t != PO_NUM || b->t != PO_NUM || b->num < 0) break;
            offs[got++] = (size_t)b->num;
            vec_push(&nums, a->num >= 0 && a->num <= PDF_MAX_OBJNUM && a->num != (double)num ? (size_t)a->num : SIZE_MAX);
        }
        free(sx->offs);
        sx->offs = offs;
        sx->noffs = got;
        for (size_t k = 0; k < nums.n; k++) /* may grow d->x: no pxref_t pointers held past here */
            if (nums.v[k] != SIZE_MAX) xref_set(d, nums.v[k], 2, k, pos, (uint32_t)num);
        vec_free(&nums);
    }
}

/* the catalog: last trailer or XRef-stream dictionary carrying /Root (also notes /Encrypt) */
static pobj_t *pdf_catalog(pdf_t *d) {
    pobj_t *best = NULL;
    size_t best_pos = 0;
    const uint8_t *b = d->buf;
    size_t i = 0;
    while (i < d->len) {
        const uint8_t *hit = memmem(b + i, d->len - i, "trailer", 7);
        if (!hit) break;
        size_t k = (size_t)(hit - b);
        i = k + 7;
        plex_t L = {b, d->len, k + 7};
        pobj_t *t = pparse(d, &L, 0);
        if (t && t->t == PO_DICT && dict_raw(t, "Root") && k >= best_pos) {
            best = t;
            best_pos = k;
        }
    }
    for (size_t num = 0; num < d->nx; num++) {
        if (d->x[num].kind != 1 || !window_has(d, d->x[num].off, "/XRef")) continue;
        pobj_t *o = pdf_get(d, num);
        if (o && o->t == PO_STREAM && dict_raw(o, "Root") && d->x[num].pos >= best_pos) {
            best = o->dict;
            best_pos = d->x[num].pos;
        }
    }
    if (best && dict_raw(best, "Encrypt")) d->encrypted = true;
    pobj_t *root = best ? dget(d, best, "Root") : NULL;
    if (root && root->t == PO_DICT) return root;
    for (size_t num = 0; num < d->nx; num++) { /* fallback: any /Type /Catalog */
        if (!d->x[num].kind) continue;
        if (d->x[num].kind == 1 && !window_has(d, d->x[num].off, "/Catalog")) continue;
        pobj_t *o = pdf_get(d, num);
        if (o && o->t == PO_DICT && name_is(dget(d, o, "Type"), "Catalog")) return o;
    }
    return NULL;
}

typedef struct {
    pobj_t *page, *res;
} ppage_t;
typedef VEC(ppage_t) ppages_t;

static void collect_pages(pdf_t *d, pobj_t *node, pobj_t *res, int depth, uint8_t *seen, ppages_t *out) {
    if (!node || node->t != PO_DICT || depth > 64 || out->n >= PDF_MAX_PAGES) return;
    pobj_t *r = dget(d, node, "Resources");
    if (r && r->t == PO_DICT) res = r;
    pobj_t *kids = dget(d, node, "Kids");
    if (kids && kids->t == PO_ARR && !name_is(dget(d, node, "Type"), "Page")) {
        for (size_t i = 0; i < kids->nv; i++) {
            pobj_t *k = kids->v[i];
            if (k->t == PO_REF) {
                if (k->rn < 0 || (size_t)k->rn >= d->nx || seen[k->rn]) continue;
                seen[k->rn] = 1;
            }
            collect_pages(d, pobj_resolve(d, k), res, depth + 1, seen, out);
        }
        return;
    }
    ppage_t p = {node, res};
    vec_push(out, p);
}

/* --- fonts --- */

typedef struct {
    uint32_t code;
    uint8_t nb;
    char *u; /* owned UTF-8 */
} tumap_t;

typedef struct {
    uint8_t nb;
    uint8_t lo[4], hi[4];
} csrange_t;

typedef struct {
    uint32_t lo, hi;
    double w;
} cidw_t;

struct pfont {
    bool composite;
    VEC(csrange_t) cs;
    VEC(tumap_t) tu;
    const char *enc[256];
    char *enc_own[256];
    double widths[256];
    bool has_widths;
    double missing, dw, scale;
    VEC(cidw_t) w;
};

static void pfont_free(pfont_t *f) {
    for (size_t i = 0; i < f->tu.n; i++) free(f->tu.v[i].u);
    vec_free(&f->tu);
    vec_free(&f->cs);
    vec_free(&f->w);
    for (int i = 0; i < 256; i++) free(f->enc_own[i]);
    free(f);
}

/* Adobe Glyph List subset: names outside ASCII single letters */
static const struct {
    const char *n;
    uint32_t u;
} GLYPHS[] = {
    {"space", 0x20}, {"exclam", 0x21}, {"quotedbl", 0x22}, {"numbersign", 0x23}, {"dollar", 0x24}, {"percent", 0x25},
    {"ampersand", 0x26}, {"quotesingle", 0x27}, {"parenleft", 0x28}, {"parenright", 0x29}, {"asterisk", 0x2a},
    {"plus", 0x2b}, {"comma", 0x2c}, {"hyphen", 0x2d}, {"period", 0x2e}, {"slash", 0x2f}, {"zero", 0x30}, {"one", 0x31},
    {"two", 0x32}, {"three", 0x33}, {"four", 0x34}, {"five", 0x35}, {"six", 0x36}, {"seven", 0x37}, {"eight", 0x38},
    {"nine", 0x39}, {"colon", 0x3a}, {"semicolon", 0x3b}, {"less", 0x3c}, {"equal", 0x3d}, {"greater", 0x3e},
    {"question", 0x3f}, {"at", 0x40}, {"bracketleft", 0x5b}, {"backslash", 0x5c}, {"bracketright", 0x5d},
    {"asciicircum", 0x5e}, {"underscore", 0x5f}, {"grave", 0x60}, {"braceleft", 0x7b}, {"bar", 0x7c},
    {"braceright", 0x7d}, {"asciitilde", 0x7e}, {"exclamdown", 0xa1}, {"cent", 0xa2}, {"sterling", 0xa3},
    {"currency", 0xa4}, {"yen", 0xa5}, {"brokenbar", 0xa6}, {"section", 0xa7}, {"dieresis", 0xa8}, {"copyright", 0xa9},
    {"ordfeminine", 0xaa}, {"guillemotleft", 0xab}, {"logicalnot", 0xac}, {"registered", 0xae}, {"macron", 0xaf},
    {"degree", 0xb0}, {"plusminus", 0xb1}, {"twosuperior", 0xb2}, {"threesuperior", 0xb3}, {"acute", 0xb4},
    {"mu", 0xb5}, {"paragraph", 0xb6}, {"periodcentered", 0xb7}, {"cedilla", 0xb8}, {"onesuperior", 0xb9},
    {"ordmasculine", 0xba}, {"guillemotright", 0xbb}, {"onequarter", 0xbc}, {"onehalf", 0xbd}, {"threequarters", 0xbe},
    {"questiondown", 0xbf}, {"Agrave", 0xc0}, {"Aacute", 0xc1}, {"Acircumflex", 0xc2}, {"Atilde", 0xc3},
    {"Adieresis", 0xc4}, {"Aring", 0xc5}, {"AE", 0xc6}, {"Ccedilla", 0xc7}, {"Egrave", 0xc8}, {"Eacute", 0xc9},
    {"Ecircumflex", 0xca}, {"Edieresis", 0xcb}, {"Igrave", 0xcc}, {"Iacute", 0xcd}, {"Icircumflex", 0xce},
    {"Idieresis", 0xcf}, {"Eth", 0xd0}, {"Ntilde", 0xd1}, {"Ograve", 0xd2}, {"Oacute", 0xd3}, {"Ocircumflex", 0xd4},
    {"Otilde", 0xd5}, {"Odieresis", 0xd6}, {"multiply", 0xd7}, {"Oslash", 0xd8}, {"Ugrave", 0xd9}, {"Uacute", 0xda},
    {"Ucircumflex", 0xdb}, {"Udieresis", 0xdc}, {"Yacute", 0xdd}, {"Thorn", 0xde}, {"germandbls", 0xdf},
    {"agrave", 0xe0}, {"aacute", 0xe1}, {"acircumflex", 0xe2}, {"atilde", 0xe3}, {"adieresis", 0xe4}, {"aring", 0xe5},
    {"ae", 0xe6}, {"ccedilla", 0xe7}, {"egrave", 0xe8}, {"eacute", 0xe9}, {"ecircumflex", 0xea}, {"edieresis", 0xeb},
    {"igrave", 0xec}, {"iacute", 0xed}, {"icircumflex", 0xee}, {"idieresis", 0xef}, {"eth", 0xf0}, {"ntilde", 0xf1},
    {"ograve", 0xf2}, {"oacute", 0xf3}, {"ocircumflex", 0xf4}, {"otilde", 0xf5}, {"odieresis", 0xf6}, {"divide", 0xf7},
    {"oslash", 0xf8}, {"ugrave", 0xf9}, {"uacute", 0xfa}, {"ucircumflex", 0xfb}, {"udieresis", 0xfc}, {"yacute", 0xfd},
    {"thorn", 0xfe}, {"ydieresis", 0xff}, {"dotlessi", 0x131}, {"Lslash", 0x141}, {"lslash", 0x142}, {"OE", 0x152},
    {"oe", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Ydieresis", 0x178}, {"Zcaron", 0x17d}, {"zcaron", 0x17e},
    {"florin", 0x192}, {"circumflex", 0x2c6}, {"caron", 0x2c7}, {"breve", 0x2d8}, {"dotaccent", 0x2d9}, {"ring", 0x2da},
    {"ogonek", 0x2db}, {"tilde", 0x2dc}, {"hungarumlaut", 0x2dd}, {"endash", 0x2013}, {"emdash", 0x2014},
    {"quoteleft", 0x2018}, {"quoteright", 0x2019}, {"quotesinglbase", 0x201a}, {"quotedblleft", 0x201c},
    {"quotedblright", 0x201d}, {"quotedblbase", 0x201e}, {"dagger", 0x2020}, {"daggerdbl", 0x2021}, {"bullet", 0x2022},
    {"ellipsis", 0x2026}, {"perthousand", 0x2030}, {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203a},
    {"fraction", 0x2044}, {"Euro", 0x20ac}, {"trademark", 0x2122}, {"minus", 0x2212}, {"fi", 0xfb01}, {"fl", 0xfb02},
    {"ff", 0xfb00}, {"ffi", 0xfb03}, {"ffl", 0xfb04}, {"nbspace", 0xa0}, {"sfthyphen", 0xad}, {"periodcentered", 0xb7},
    {"lessequal", 0x2264}, {"greaterequal", 0x2265}, {"notequal", 0x2260}, {"infinity", 0x221e}, {"arrowright", 0x2192},
    {"arrowleft", 0x2190}, {"checkmark", 0x2713}, {"Delta", 0x394}, {"Omega", 0x3a9}, {"pi", 0x3c0}, {"mu1", 0xb5},
};

/* glyph name -> UTF-8 (owned) or NULL */
static char *glyph_utf8(const char *name) {
    char *base = xstrdup(name);
    char *dot = strchr(base, '.');
    if (dot && dot != base) *dot = 0;
    sb_t b = SB_INIT;
    strvec_t parts = str_split(base, "_");
    bool ok = parts.n > 0;
    for (size_t p = 0; p < parts.n && ok; p++) {
        const char *g = parts.v[p];
        size_t gl = strlen(g);
        if (gl == 1 && isalpha((unsigned char)g[0])) {
            sb_putc(&b, g[0]);
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < ARRAY_LEN(GLYPHS); i++)
            if (!strcmp(GLYPHS[i].n, g)) {
                sb_append_utf8(&b, GLYPHS[i].u);
                found = true;
                break;
            }
        if (found) continue;
        if (gl >= 7 && !strncmp(g, "uni", 3) && (gl - 3) % 4 == 0) {
            for (size_t k = 3; k + 4 <= gl; k += 4) {
                char hx[5] = {g[k], g[k + 1], g[k + 2], g[k + 3], 0};
                char *e;
                unsigned long v = strtoul(hx, &e, 16);
                if (*e) ok = false;
                else sb_append_utf8(&b, (uint32_t)v);
            }
            continue;
        }
        if (gl >= 5 && gl <= 7 && g[0] == 'u') {
            char *e;
            unsigned long v = strtoul(g + 1, &e, 16);
            if (!*e && v <= 0x10FFFF) {
                sb_append_utf8(&b, (uint32_t)v);
                continue;
            }
        }
        ok = false;
    }
    strvec_free(&parts);
    free(base);
    if (!ok || !b.len) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

/* WinAnsi 0x80-0x9F, MacRoman 0x80-0xFF, StandardEncoding differences */
static const uint16_t WIN_80[32] = {0x20ac, 0,      0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160,
                                    0x2039, 0x152,  0,      0x17d,  0,      0,      0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
                                    0x2013, 0x2014, 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0,      0x17e,  0x178};
static const uint16_t MAC_80[128] = {
    0xc4,  0xc5,  0xc7,   0xc9,   0xd1,   0xd6,   0xdc,   0xe1,   0xe0,   0xe2,   0xe4,   0xe3,   0xe5,   0xe7,   0xe9,
    0xe8,  0xea,  0xeb,   0xed,   0xec,   0xee,   0xef,   0xf1,   0xf3,   0xf2,   0xf4,   0xf6,   0xf5,   0xfa,   0xf9,
    0xfb,  0xfc,  0x2020, 0xb0,   0xa2,   0xa3,   0xa7,   0x2022, 0xb6,   0xdf,   0xae,   0xa9,   0x2122, 0xb4,   0xa8,
    0x2260, 0xc6, 0xd8,   0x221e, 0xb1,   0x2264, 0x2265, 0xa5,   0xb5,   0x2202, 0x2211, 0x220f, 0x3c0,  0x222b, 0xaa,
    0xba,  0x3a9, 0xe6,   0xf8,   0xbf,   0xa1,   0xac,   0x221a, 0x192,  0x2248, 0x2206, 0xab,   0xbb,   0x2026, 0xa0,
    0xc0,  0xc3,  0xd5,   0x152,  0x153,  0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0xf7,   0x25ca, 0xff,   0x178,
    0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02, 0x2021, 0xb7,  0x201a, 0x201e, 0x2030, 0xc2,   0xca,   0xc1,   0xcb,
    0xc8,  0xcd,  0xce,   0xcf,   0xcc,   0xd3,   0xd4,   0xf8ff, 0xd2,   0xda,   0xdb,   0xd9,   0x131,  0x2c6,  0x2dc,
    0xaf,  0x2d8, 0x2d9,  0x2da,  0xb8,   0x2dd,  0x2db,  0x2c7};
static const struct {
    uint8_t c;
    uint16_t u;
} STD_DIFF[] = {{0x27, 0x2019}, {0x60, 0x2018}, {0xa1, 0xa1},   {0xa2, 0xa2},   {0xa3, 0xa3},   {0xa4, 0x2044},
                {0xa5, 0xa5},   {0xa6, 0x192},  {0xa7, 0xa7},   {0xa8, 0xa4},   {0xa9, 0x27},   {0xaa, 0x201c},
                {0xab, 0xab},   {0xac, 0x2039}, {0xad, 0x203a}, {0xae, 0xfb01}, {0xaf, 0xfb02}, {0xb1, 0x2013},
                {0xb2, 0x2020}, {0xb3, 0x2021}, {0xb4, 0xb7},   {0xb6, 0xb6},   {0xb7, 0x2022}, {0xb8, 0x201a},
                {0xb9, 0x201e}, {0xba, 0x201d}, {0xbb, 0xbb},   {0xbc, 0x2026}, {0xbd, 0x2030}, {0xbf, 0xbf},
                {0xc1, 0x60},   {0xc2, 0xb4},   {0xc3, 0x2c6},  {0xc4, 0x2dc},  {0xc5, 0xaf},   {0xc6, 0x2d8},
                {0xc7, 0x2d9},  {0xc8, 0xa8},   {0xca, 0x2da},  {0xcb, 0xb8},   {0xcd, 0x2dd},  {0xce, 0x2db},
                {0xcf, 0x2c7},  {0xd0, 0x2014}, {0xe1, 0xc6},   {0xe3, 0xaa},   {0xe8, 0x141},  {0xe9, 0xd8},
                {0xea, 0x152},  {0xeb, 0xba},   {0xf1, 0xe6},   {0xf5, 0x131},  {0xf8, 0x142},  {0xf9, 0xf8},
                {0xfa, 0x153},  {0xfb, 0xdf}};

static void set_enc(pfont_t *f, int code, uint32_t u) {
    free(f->enc_own[code]);
    f->enc_own[code] = NULL;
    if (!u) {
        f->enc[code] = NULL;
        return;
    }
    sb_t b = SB_INIT;
    sb_append_utf8(&b, u);
    f->enc_own[code] = sb_detach(&b);
    f->enc[code] = f->enc_own[code];
}

static void base_encoding(pfont_t *f, const char *name) {
    bool mac = name && !strcmp(name, "MacRomanEncoding"), std = name && !strcmp(name, "StandardEncoding");
    for (int c = 0; c < 256; c++) {
        uint32_t u = 0;
        if (c >= 0x20 && c < 0x7f) u = (uint32_t)c;
        else if (c >= 0x80) {
            if (mac) u = MAC_80[c - 0x80];
            else if (std) u = 0;
            else u = c < 0xa0 ? WIN_80[c - 0x80] : (uint32_t)c;
        }
        set_enc(f, c, u);
    }
    if (std) {
        for (int c = 0x80; c < 256; c++) set_enc(f, c, 0);
        for (size_t i = 0; i < ARRAY_LEN(STD_DIFF); i++) set_enc(f, STD_DIFF[i].c, STD_DIFF[i].u);
        set_enc(f, 0x27, 0x2019);
        set_enc(f, 0x60, 0x2018);
    }
}

static int cmp_tu(const void *a, const void *b) {
    const tumap_t *x = a, *y = b;
    if (x->nb != y->nb) return x->nb < y->nb ? -1 : 1;
    return x->code < y->code ? -1 : x->code > y->code;
}

static char *utf16be_to_utf8(const uint8_t *p, size_t n) { return utf16_to_utf8(p, n, true); }

static void tu_add(pfont_t *f, uint32_t code, uint8_t nb, char *u) {
    if (f->tu.n >= PDF_MAX_TU) {
        free(u);
        return;
    }
    tumap_t m = {code, nb, u};
    vec_push(&f->tu, m);
}

static uint32_t bytes_code(const char *s, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n && i < 4; i++) v = v << 8 | (uint8_t)s[i];
    return v;
}

/* parse codespace ranges and bf mappings from a CMap stream */
static void parse_cmap(pdf_t *d, pobj_t *stream, pfont_t *f, bool mappings) {
    size_t len = 0;
    uint8_t *data = pdf_stream(d, stream, &len);
    if (!data) return;
    plex_t L = {data, len, 0};
    VEC(pobj_t *) ops = {0};
    int mode = 0; /* 1 codespace, 2 bfchar, 3 bfrange */
    for (int guard = 0; guard < 4000000; guard++) {
        pobj_t *o = pparse(d, &L, 0);
        if (!o) break;
        if (o->t != PO_KW) {
            if (ops.n < 4096) vec_push(&ops, o);
            if (mode == 1 && ops.n == 2) {
                pobj_t *a = ops.v[0], *b = ops.v[1];
                if (a->t == PO_STR && b->t == PO_STR && a->n == b->n && a->n >= 1 && a->n <= 4) {
                    csrange_t r = {(uint8_t)a->n, {0}, {0}};
                    memcpy(r.lo, a->s, a->n);
                    memcpy(r.hi, b->s, b->n);
                    if (f->cs.n < 256) vec_push(&f->cs, r);
                }
                ops.n = 0;
            } else if (mode == 2 && ops.n == 2) {
                pobj_t *a = ops.v[0], *b = ops.v[1];
                if (mappings && a->t == PO_STR && a->n >= 1 && a->n <= 4) {
                    char *u = b->t == PO_STR ? utf16be_to_utf8((const uint8_t *)b->s, b->n) : b->t == PO_NAME ? glyph_utf8(b->s) : NULL;
                    if (u) tu_add(f, bytes_code(a->s, a->n), (uint8_t)a->n, u);
                }
                ops.n = 0;
            } else if (mode == 3 && ops.n == 3) {
                pobj_t *a = ops.v[0], *b = ops.v[1], *c = ops.v[2];
                if (mappings && a->t == PO_STR && b->t == PO_STR && a->n == b->n && a->n >= 1 && a->n <= 4) {
                    uint32_t lo = bytes_code(a->s, a->n), hi = bytes_code(b->s, b->n);
                    if (hi >= lo && hi - lo <= 65535) {
                        for (uint32_t k = 0; k <= hi - lo; k++) {
                            char *u = NULL;
                            if (c->t == PO_STR && c->n >= 1 && c->n <= 64) {
                                uint8_t tmp[64];
                                memcpy(tmp, c->s, c->n);
                                /* increment the last UTF-16 code unit by k */
                                if (c->n >= 2) {
                                    uint32_t last = (uint32_t)tmp[c->n - 2] << 8 | tmp[c->n - 1];
                                    last += k;
                                    tmp[c->n - 2] = (uint8_t)(last >> 8);
                                    tmp[c->n - 1] = (uint8_t)last;
                                } else tmp[0] = (uint8_t)(tmp[0] + k);
                                u = utf16be_to_utf8(tmp, c->n);
                            } else if (c->t == PO_ARR && k < c->nv) {
                                pobj_t *e = c->v[k];
                                if (e->t == PO_STR) u = utf16be_to_utf8((const uint8_t *)e->s, e->n);
                            }
                            if (u) tu_add(f, lo + k, (uint8_t)a->n, u);
                        }
                    }
                }
                ops.n = 0;
            }
            continue;
        }
        const char *kw = o->s ? o->s : "";
        ops.n = 0;
        if (!strcmp(kw, "begincodespacerange")) mode = 1;
        else if (!strcmp(kw, "beginbfchar")) mode = 2;
        else if (!strcmp(kw, "beginbfrange")) mode = 3;
        else if (!strncmp(kw, "end", 3)) mode = 0;
        else if (!strcmp(kw, "[") || !strcmp(kw, "]")) { /* stray tokens */ }
    }
    vec_free(&ops);
    free(data);
    if (f->tu.n > 1) {
        /* stable sort then keep the last definition of each code */
        for (size_t i = 0; i < f->tu.n; i++) f->tu.v[i].nb = (uint8_t)(f->tu.v[i].nb & 7);
        tumap_t *v = f->tu.v;
        size_t n = f->tu.n;
        /* tag insertion order in the high bits of a temp array */
        size_t *ord = xmalloc(n * sizeof(size_t));
        for (size_t i = 0; i < n; i++) ord[i] = i;
        /* simple merge sort by (nb, code, order) */
        size_t *tmp = xmalloc(n * sizeof(size_t));
        for (size_t w = 1; w < n; w *= 2)
            for (size_t lo = 0; lo < n; lo += 2 * w) {
                size_t mid = MIN(lo + w, n), hi = MIN(lo + 2 * w, n), i = lo, j = mid, k = lo;
                while (i < mid && j < hi) tmp[k++] = cmp_tu(&v[ord[i]], &v[ord[j]]) <= 0 ? ord[i++] : ord[j++];
                while (i < mid) tmp[k++] = ord[i++];
                while (j < hi) tmp[k++] = ord[j++];
                memcpy(ord + lo, tmp + lo, (hi - lo) * sizeof(size_t));
            }
        tumap_t *sorted = xmalloc(n * sizeof(tumap_t));
        size_t m = 0;
        for (size_t i = 0; i < n; i++) {
            tumap_t e = v[ord[i]];
            if (m && sorted[m - 1].nb == e.nb && sorted[m - 1].code == e.code) {
                free(sorted[m - 1].u);
                sorted[m - 1] = e;
            } else sorted[m++] = e;
        }
        free(tmp);
        free(ord);
        free(f->tu.v);
        f->tu.v = sorted;
        f->tu.n = m;
        f->tu.cap = n;
    }
}

static const char *tu_lookup(const pfont_t *f, uint32_t code, uint8_t nb) {
    size_t lo = 0, hi = f->tu.n;
    tumap_t key = {code, nb, NULL};
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = cmp_tu(&f->tu.v[mid], &key);
        if (!c) return f->tu.v[mid].u;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static pfont_t *load_font(pdf_t *d, pobj_t *fd) {
    for (size_t i = 0; i < d->font_keys.n; i++)
        if (d->font_keys.v[i] == fd) return d->fonts.v[i];
    pfont_t *f = xcalloc(1, sizeof *f);
    f->scale = 0.001;
    f->dw = 1000;
    pobj_t *sub = dget(d, fd, "Subtype");
    f->composite = name_is(sub, "Type0");
    if (f->composite) {
        pobj_t *enc = dget(d, fd, "Encoding");
        if (enc && enc->t == PO_STREAM) parse_cmap(d, enc, f, false);
        if (!f->cs.n) {
            csrange_t r = {2, {0, 0}, {0xff, 0xff}};
            vec_push(&f->cs, r);
        }
        pobj_t *df = dget(d, fd, "DescendantFonts");
        pobj_t *cid = df && df->t == PO_ARR && df->nv ? pobj_resolve(d, df->v[0]) : NULL;
        f->dw = num_or(dget(d, cid, "DW"), 1000);
        pobj_t *w = dget(d, cid, "W");
        if (w && w->t == PO_ARR) {
            for (size_t i = 0; i + 1 < w->nv && f->w.n < 200000;) {
                pobj_t *a = pobj_resolve(d, w->v[i]), *b = pobj_resolve(d, w->v[i + 1]);
                if (!a || a->t != PO_NUM) break;
                if (b && b->t == PO_ARR) {
                    for (size_t k = 0; k < b->nv; k++) {
                        cidw_t e = {(uint32_t)a->num + (uint32_t)k, (uint32_t)a->num + (uint32_t)k, num_or(pobj_resolve(d, b->v[k]), f->dw)};
                        vec_push(&f->w, e);
                    }
                    i += 2;
                } else if (i + 2 < w->nv) {
                    pobj_t *c = pobj_resolve(d, w->v[i + 2]);
                    cidw_t e = {(uint32_t)a->num, (uint32_t)num_or(b, a->num), num_or(c, f->dw)};
                    vec_push(&f->w, e);
                    i += 3;
                } else break;
            }
        }
    } else {
        pobj_t *enc = dget(d, fd, "Encoding");
        const char *base = name_is(sub, "TrueType") ? "WinAnsiEncoding" : "StandardEncoding";
        pobj_t *desc = dget(d, fd, "FontDescriptor");
        int flags = (int)num_or(dget(d, desc, "Flags"), 0);
        if ((flags & 4) && !enc) base = "WinAnsiEncoding"; /* symbolic: codes usually map straight through */
        if (enc && enc->t == PO_NAME) base = enc->s;
        else if (enc && enc->t == PO_DICT) {
            pobj_t *be = dget(d, enc, "BaseEncoding");
            if (be && be->t == PO_NAME) base = be->s;
        }
        base_encoding(f, base);
        pobj_t *diff = enc && enc->t == PO_DICT ? dget(d, enc, "Differences") : NULL;
        if (diff && diff->t == PO_ARR) {
            int code = 0;
            for (size_t i = 0; i < diff->nv; i++) {
                pobj_t *e = pobj_resolve(d, diff->v[i]);
                if (!e) continue;
                if (e->t == PO_NUM) code = (int)e->num;
                else if (e->t == PO_NAME && code >= 0 && code < 256) {
                    char *u = glyph_utf8(e->s ? e->s : "");
                    free(f->enc_own[code]);
                    f->enc_own[code] = u;
                    f->enc[code] = u;
                    code++;
                }
            }
        }
        pobj_t *ws = dget(d, fd, "Widths");
        int fc = (int)num_or(dget(d, fd, "FirstChar"), 0);
        f->missing = num_or(dget(d, desc, "MissingWidth"), 0);
        if (ws && ws->t == PO_ARR) {
            f->has_widths = true;
            for (int c = 0; c < 256; c++) f->widths[c] = f->missing;
            for (size_t i = 0; i < ws->nv; i++) {
                long c = fc + (long)i;
                if (c >= 0 && c < 256) f->widths[c] = num_or(pobj_resolve(d, ws->v[i]), f->missing);
            }
        }
        if (name_is(sub, "Type3")) {
            pobj_t *fm = dget(d, fd, "FontMatrix");
            if (fm && fm->t == PO_ARR && fm->nv) f->scale = fabs(num_or(pobj_resolve(d, fm->v[0]), 0.001));
            if (f->scale <= 0 || f->scale > 1) f->scale = 0.001;
        }
    }
    pobj_t *tu = dget(d, fd, "ToUnicode");
    if (tu && tu->t == PO_STREAM) {
        size_t had = f->cs.n;
        parse_cmap(d, tu, f, true);
        if (f->composite && had && f->cs.n > had) { /* prefer the ToUnicode codespace when it was given */
            memmove(f->cs.v, f->cs.v + had, (f->cs.n - had) * sizeof(csrange_t));
            f->cs.n -= had;
        }
    }
    vec_push(&d->font_keys, fd);
    vec_push(&d->fonts, f);
    return f;
}

static double glyph_width(const pfont_t *f, uint32_t code) {
    if (f->composite) {
        for (size_t i = 0; i < f->w.n; i++)
            if (code >= f->w.v[i].lo && code <= f->w.v[i].hi) return f->w.v[i].w;
        return f->dw;
    }
    if (f->has_widths && code < 256) return f->widths[code];
    return code == 32 ? 250 : 500;
}

/* --- content interpretation --- */

typedef struct {
    double ctm[6], tc, tw, th, tl, fs;
    pfont_t *font;
} gstate_t;

typedef struct {
    pdf_t *d;
    sb_t *out;
    gstate_t gs, stack[64];
    int sp;
    double tm[6], tlm[6];
    bool have_last;
    double last_x, last_y, last_sz;
    long ops;
    pobj_t *forms[PDF_MAX_FORM_DEPTH];
    int nforms;
} tx_t;

static void mat_mul(const double *a, const double *b, double *r) {
    double t[6] = {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2], a[2] * b[1] + a[3] * b[3],
                   a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5]};
    memcpy(r, t, sizeof t);
}

static void mat_id(double *m) {
    m[0] = m[3] = 1;
    m[1] = m[2] = m[4] = m[5] = 0;
}

static bool out_ends(sb_t *b, const char *s) {
    size_t n = strlen(s);
    return b->len >= n && !memcmp(b->s + b->len - n, s, n);
}

/* place a text run starting at the current text position: line break / space heuristics */
static void place_run(tx_t *t) {
    double m[6];
    mat_mul(t->tm, t->gs.ctm, m);
    double x = m[4], y = m[5];
    double sz = fabs(t->gs.fs) * hypot(m[2], m[3]);
    if (!(sz > 0.01) || !isfinite(sz)) sz = fabs(t->gs.fs) > 0 ? fabs(t->gs.fs) : 1;
    if (t->have_last && t->out->len) {
        double ref = MAX(sz, t->last_sz), dy = y - t->last_y, dx = x - t->last_x;
        if (fabs(dy) > 0.5 * ref) {
            if (fabs(dy) > 1.9 * ref && !out_ends(t->out, "\n\n")) sb_append(t->out, out_ends(t->out, "\n") ? "\n" : "\n\n");
            else if (!out_ends(t->out, "\n")) sb_putc(t->out, '\n');
        } else if ((dx > 0.2 * ref || dx < -2 * ref) && !out_ends(t->out, " ") && !out_ends(t->out, "\n")) {
            sb_putc(t->out, ' ');
        }
    }
    t->last_sz = sz;
}

static void end_run(tx_t *t) {
    double m[6];
    mat_mul(t->tm, t->gs.ctm, m);
    t->last_x = m[4];
    t->last_y = m[5];
    t->have_last = true;
}

static void append_text(sb_t *b, const char *u) {
    for (const unsigned char *p = (const unsigned char *)u; *p; p++)
        if (*p >= 0x20 || *p == '\t') sb_putc(b, (char)*p);
        else if (*p == '\n' || *p == '\r') sb_putc(b, ' ');
}

static void show_string(tx_t *t, const uint8_t *s, size_t n) {
    pfont_t *f = t->gs.font ? t->gs.font : t->d->default_font;
    place_run(t);
    for (size_t i = 0; i < n;) {
        size_t nb = 1;
        if (f->composite) {
            nb = 0;
            for (size_t len = 1; len <= 4 && !nb; len++)
                for (size_t r = 0; r < f->cs.n; r++) {
                    const csrange_t *cr = &f->cs.v[r];
                    if (cr->nb != len || i + len > n) continue;
                    bool in = true;
                    for (size_t k = 0; k < len; k++)
                        if (s[i + k] < cr->lo[k] || s[i + k] > cr->hi[k]) in = false;
                    if (in) {
                        nb = len;
                        break;
                    }
                }
            if (!nb) nb = MIN((size_t)2, n - i);
        }
        uint32_t code = bytes_code((const char *)s + i, nb);
        const char *u = tu_lookup(f, code, (uint8_t)nb);
        if (!u && !f->composite) u = f->enc[code & 0xff];
        if (u) append_text(t->out, u);
        double adv = (glyph_width(f, code) * f->scale * t->gs.fs + t->gs.tc + (nb == 1 && code == 32 ? t->gs.tw : 0)) * t->gs.th;
        t->tm[4] += adv * t->tm[0];
        t->tm[5] += adv * t->tm[1];
        i += nb;
    }
    end_run(t);
}

/* content-stream operand */
enum { CT_NUM, CT_STR, CT_NAME, CT_ARR, CT_OTHER };
typedef struct ctok {
    int t;
    double num;
    char *s;
    size_t n;
    struct ctok *items;
    size_t nitems;
} ctok_t;

static void ctok_free(ctok_t *c) {
    free(c->s);
    for (size_t i = 0; i < c->nitems; i++) ctok_free(&c->items[i]);
    free(c->items);
    memset(c, 0, sizeof *c);
}

/* 0 = end, 1 = operand in *c, 2 = operator (c->s) */
static int ct_next(plex_t *L, ctok_t *c, int depth) {
    memset(c, 0, sizeof *c);
    pskip(L);
    if (L->i >= L->n) return 0;
    int ch = L->s[L->i];
    if (ch == '/') {
        c->t = CT_NAME;
        c->s = lex_name(L, &c->n);
        return 1;
    }
    if (ch == '(' || (ch == '<' && (L->i + 1 >= L->n || L->s[L->i + 1] != '<'))) {
        sb_t b = SB_INIT;
        if (ch == '(') lex_litstr(L, &b);
        else lex_hexstr(L, &b);
        c->t = CT_STR;
        c->n = b.len;
        c->s = sb_detach(&b);
        return 1;
    }
    if (ch == '<') { /* inline dictionary (BDC properties, inline image params): skip balanced */
        int dd = 0;
        while (L->i < L->n) {
            if (L->s[L->i] == '<' && L->i + 1 < L->n && L->s[L->i + 1] == '<') dd++, L->i += 2;
            else if (L->s[L->i] == '>' && L->i + 1 < L->n && L->s[L->i + 1] == '>') {
                L->i += 2;
                if (--dd <= 0) break;
            } else if (L->s[L->i] == '(') {
                sb_t b = SB_INIT;
                lex_litstr(L, &b);
                sb_free(&b);
            } else L->i++;
        }
        c->t = CT_OTHER;
        return 1;
    }
    if (ch == '[') {
        L->i++;
        c->t = CT_ARR;
        VEC(ctok_t) items = {0};
        for (;;) {
            pskip(L);
            if (L->i >= L->n) break;
            if (L->s[L->i] == ']') {
                L->i++;
                break;
            }
            ctok_t it;
            int r = depth < 8 ? ct_next(L, &it, depth + 1) : (L->i++, 0);
            if (r == 0) break;
            if (r == 2) { /* operator inside an array: malformed, drop it */
                ctok_free(&it);
                continue;
            }
            if (items.n < 100000) vec_push(&items, it);
            else ctok_free(&it);
        }
        c->items = items.v;
        c->nitems = items.n;
        return 1;
    }
    if (ch == ']' || ch == '>' || ch == ')' || ch == '{' || ch == '}') {
        L->i++;
        c->t = CT_OTHER;
        return 1;
    }
    size_t st = lex_regular(L);
    bool is_int;
    if (tok_number(L->s + st, L->i - st, &c->num, &is_int)) {
        c->t = CT_NUM;
        return 1;
    }
    if (L->i == st) {
        L->i++;
        c->t = CT_OTHER;
        return 1;
    }
    c->s = xstrndup((const char *)L->s + st, L->i - st);
    return 2;
}

static void interpret(tx_t *t, const uint8_t *data, size_t len, pobj_t *res);

static void do_xobject(tx_t *t, const char *name, pobj_t *res) {
    pobj_t *xd = dget(t->d, res, "XObject");
    pobj_t *xo = dget(t->d, xd, name);
    if (!xo || xo->t != PO_STREAM || !name_is(dget(t->d, xo, "Subtype"), "Form")) return;
    if (t->nforms >= PDF_MAX_FORM_DEPTH) return;
    for (int i = 0; i < t->nforms; i++)
        if (t->forms[i] == xo) return;
    size_t len = 0;
    uint8_t *data = pdf_stream(t->d, xo, &len);
    if (!data) return;
    t->forms[t->nforms++] = xo;
    gstate_t saved = t->gs;
    double stm[6], stlm[6];
    memcpy(stm, t->tm, sizeof stm);
    memcpy(stlm, t->tlm, sizeof stlm);
    pobj_t *mx = dget(t->d, xo, "Matrix");
    if (mx && mx->t == PO_ARR && mx->nv == 6) {
        double m[6];
        for (int i = 0; i < 6; i++) m[i] = num_or(pobj_resolve(t->d, mx->v[i]), i == 0 || i == 3);
        mat_mul(m, t->gs.ctm, t->gs.ctm);
    }
    pobj_t *fres = dget(t->d, xo, "Resources");
    interpret(t, data, len, fres && fres->t == PO_DICT ? fres : res);
    t->gs = saved;
    memcpy(t->tm, stm, sizeof stm);
    memcpy(t->tlm, stlm, sizeof stlm);
    t->nforms--;
    free(data);
}

static void interpret(tx_t *t, const uint8_t *data, size_t len, pobj_t *res) {
    plex_t L = {data, len, 0};
    ctok_t ops[16];
    size_t nops = 0;
    for (;;) {
        ctok_t c;
        int r = ct_next(&L, &c, 0);
        if (r == 0) break;
        if (r == 1) {
            if (nops == ARRAY_LEN(ops)) {
                ctok_free(&ops[0]);
                memmove(ops, ops + 1, (nops - 1) * sizeof(ctok_t));
                nops--;
            }
            ops[nops++] = c;
            continue;
        }
        if (++t->ops > PDF_MAX_OPS) {
            free(c.s);
            break;
        }
        const char *op = c.s;
#define NUM(i) (nops > (i) && ops[nops - 1 - (i)].t == CT_NUM ? ops[nops - 1 - (i)].num : 0)
        if (!strcmp(op, "BT")) {
            mat_id(t->tm);
            mat_id(t->tlm);
        } else if (!strcmp(op, "Tf") && nops >= 2) {
            t->gs.fs = NUM(0);
            ctok_t *nm = &ops[nops - 2];
            pobj_t *fonts = dget(t->d, res, "Font");
            pobj_t *fd = nm->t == CT_NAME ? dget(t->d, fonts, nm->s) : NULL;
            t->gs.font = fd && fd->t == PO_DICT ? load_font(t->d, fd) : NULL;
        } else if (!strcmp(op, "Tc")) t->gs.tc = NUM(0);
        else if (!strcmp(op, "Tw")) t->gs.tw = NUM(0);
        else if (!strcmp(op, "Tz")) t->gs.th = NUM(0) / 100.0;
        else if (!strcmp(op, "TL")) t->gs.tl = NUM(0);
        else if (!strcmp(op, "Td") || !strcmp(op, "TD")) {
            double tx = NUM(1), ty = NUM(0);
            if (op[1] == 'D') t->gs.tl = -ty;
            double m[6] = {1, 0, 0, 1, tx, ty};
            mat_mul(m, t->tlm, t->tlm);
            memcpy(t->tm, t->tlm, sizeof t->tm);
        } else if (!strcmp(op, "Tm") && nops >= 6) {
            for (int i = 0; i < 6; i++) t->tlm[i] = NUM(5 - i);
            memcpy(t->tm, t->tlm, sizeof t->tm);
        } else if (!strcmp(op, "T*") || !strcmp(op, "'") || !strcmp(op, "\"")) {
            if (op[0] == '"') {
                t->gs.tw = NUM(2);
                t->gs.tc = NUM(1);
            }
            double m[6] = {1, 0, 0, 1, 0, -t->gs.tl};
            mat_mul(m, t->tlm, t->tlm);
            memcpy(t->tm, t->tlm, sizeof t->tm);
            if (op[0] != 'T' && nops && ops[nops - 1].t == CT_STR) show_string(t, (const uint8_t *)ops[nops - 1].s, ops[nops - 1].n);
        } else if (!strcmp(op, "Tj")) {
            if (nops && ops[nops - 1].t == CT_STR) show_string(t, (const uint8_t *)ops[nops - 1].s, ops[nops - 1].n);
        } else if (!strcmp(op, "TJ")) {
            if (nops && ops[nops - 1].t == CT_ARR) {
                ctok_t *a = &ops[nops - 1];
                for (size_t i = 0; i < a->nitems; i++) {
                    if (a->items[i].t == CT_STR) show_string(t, (const uint8_t *)a->items[i].s, a->items[i].n);
                    else if (a->items[i].t == CT_NUM) {
                        double adv = -a->items[i].num / 1000.0 * t->gs.fs * t->gs.th;
                        t->tm[4] += adv * t->tm[0];
                        t->tm[5] += adv * t->tm[1];
                    }
                }
            }
        } else if (!strcmp(op, "cm") && nops >= 6) {
            double m[6];
            for (int i = 0; i < 6; i++) m[i] = NUM(5 - i);
            mat_mul(m, t->gs.ctm, t->gs.ctm);
        } else if (!strcmp(op, "q")) {
            if (t->sp < (int)ARRAY_LEN(t->stack)) t->stack[t->sp++] = t->gs;
        } else if (!strcmp(op, "Q")) {
            if (t->sp > 0) t->gs = t->stack[--t->sp];
        } else if (!strcmp(op, "Do")) {
            if (nops && ops[nops - 1].t == CT_NAME) do_xobject(t, ops[nops - 1].s, res);
        } else if (!strcmp(op, "ID")) { /* inline image data up to whitespace + "EI" */
            size_t i = L.i + 1;
            while (i + 2 <= L.n) {
                const uint8_t *e = memmem(L.s + i, L.n - i, "EI", 2);
                if (!e) {
                    i = L.n;
                    break;
                }
                size_t k = (size_t)(e - L.s);
                if (k > 0 && pws(L.s[k - 1]) && (k + 2 >= L.n || !preg(L.s[k + 2]))) {
                    i = k + 2;
                    break;
                }
                i = k + 2;
            }
            L.i = MIN(i, L.n);
        }
#undef NUM
        free(c.s);
        for (size_t i = 0; i < nops; i++) ctok_free(&ops[i]);
        nops = 0;
    }
    for (size_t i = 0; i < nops; i++) ctok_free(&ops[i]);
}

/* tidy page text: trim lines, collapse inner space runs and blank-line runs */
static char *tidy_text(const char *s) {
    strvec_t lines = str_split(s, "\n");
    sb_t b = SB_INIT;
    int blanks = 0;
    for (size_t i = 0; i < lines.n; i++) {
        char *l = str_trim(lines.v[i]);
        if (!*l) {
            blanks++;
            free(l);
            continue;
        }
        if (b.len) sb_append(&b, blanks ? "\n\n" : "\n");
        blanks = 0;
        for (const char *p = l; *p; p++) {
            if (*p == ' ' && p[1] == ' ') continue;
            sb_putc(&b, *p);
        }
        free(l);
    }
    strvec_free(&lines);
    char *r = sb_detach(&b);
    if (!utf8_valid(r, strlen(r))) {
        char *t = utf8_sanitize(r, strlen(r));
        free(r);
        r = t;
    }
    return r;
}

static void pdf_free(pdf_t *d) {
    for (size_t i = 0; i < d->nx; i++) free(d->x[i].dec), free(d->x[i].offs);
    free(d->x);
    while (d->arena) {
        ablk_t *n = d->arena->next;
        free(d->arena);
        d->arena = n;
    }
    for (size_t i = 0; i < d->fonts.n; i++) pfont_free(d->fonts.v[i]);
    vec_free(&d->fonts);
    vec_free(&d->font_keys);
    if (d->default_font) pfont_free(d->default_font);
}

int rf_pdf_extract(const char *data, size_t len, strvec_t *pages, char **err) {
    memset(pages, 0, sizeof *pages);
    const uint8_t *hdr = len >= 5 ? memmem(data, MIN(len, (size_t)1024), "%PDF-", 5) : NULL;
    if (!hdr) {
        *err = xstrdup("not a PDF file (missing %PDF- header)");
        return -1;
    }
    pdf_t d;
    memset(&d, 0, sizeof d);
    d.buf = (const uint8_t *)data;
    d.len = len;
    d.default_font = xcalloc(1, sizeof(pfont_t));
    d.default_font->scale = 0.001;
    base_encoding(d.default_font, "WinAnsiEncoding");
    pdf_scan_objects(&d);
    pdf_index_objstms(&d);
    pobj_t *cat = pdf_catalog(&d);
    if (d.encrypted) {
        pdf_free(&d);
        *err = xstrdup("encrypted PDFs are not supported");
        return -1;
    }
    ppages_t pl = {0};
    uint8_t *seen = xcalloc(d.nx + 1, 1);
    if (cat) collect_pages(&d, dget(&d, cat, "Pages"), NULL, 0, seen, &pl);
    if (!pl.n) { /* damaged page tree: every /Type /Page object in number order */
        for (size_t num = 0; num < d.nx && pl.n < PDF_MAX_PAGES; num++) {
            if (!d.x[num].kind || (d.x[num].kind == 1 && !window_has(&d, d.x[num].off, "/Page"))) continue;
            pobj_t *o = pdf_get(&d, num);
            if (o && o->t == PO_DICT && name_is(dget(&d, o, "Type"), "Page")) {
                ppage_t p = {o, dget(&d, o, "Resources")};
                vec_push(&pl, p);
            }
        }
    }
    free(seen);
    if (!pl.n) {
        vec_free(&pl);
        pdf_free(&d);
        *err = xstrdup("no pages found (damaged or unsupported PDF)");
        return -1;
    }
    for (size_t i = 0; i < pl.n; i++) {
        sb_t out = SB_INIT;
        tx_t t;
        memset(&t, 0, sizeof t);
        t.d = &d;
        t.out = &out;
        mat_id(t.gs.ctm);
        t.gs.th = 1;
        mat_id(t.tm);
        mat_id(t.tlm);
        pobj_t *contents = dget(&d, pl.v[i].page, "Contents");
        VEC(pobj_t *) streams = {0};
        if (contents && contents->t == PO_STREAM) vec_push(&streams, contents);
        else if (contents && contents->t == PO_ARR)
            for (size_t k = 0; k < contents->nv && k < 10000; k++) {
                pobj_t *s = pobj_resolve(&d, contents->v[k]);
                if (s && s->t == PO_STREAM) vec_push(&streams, s);
            }
        /* concatenate: an operator may straddle stream boundaries */
        sb_t all = SB_INIT;
        for (size_t k = 0; k < streams.n; k++) {
            size_t n = 0;
            uint8_t *c = pdf_stream(&d, streams.v[k], &n);
            if (!c) continue;
            if (all.len + n <= PDF_MAX_STREAM) {
                sb_appendn(&all, (const char *)c, n);
                sb_putc(&all, '\n');
            }
            free(c);
        }
        vec_free(&streams);
        if (all.len) interpret(&t, (const uint8_t *)all.s, all.len, pl.v[i].res);
        sb_free(&all);
        char *txt = tidy_text(sb_cstr(&out));
        sb_free(&out);
        strvec_push_own(pages, txt);
    }
    vec_free(&pl);
    pdf_free(&d);
    return 0;
}

char *rf_pdf_render(const strvec_t *pages, int first, int last, char **err) {
    if (!pages->n) {
        *err = xstrdup("Conversion produced no output");
        return NULL;
    }
    if (first <= 0) first = 1;
    if (last <= 0 || last > (int)pages->n) last = (int)pages->n;
    if (first > last) {
        *err = xasprintf("Page %d is beyond end of document (%zu pages total)", first, pages->n);
        return NULL;
    }
    sb_t b = SB_INIT, empty = SB_INIT;
    bool any = false, markers = pages->n > 1;
    int nempty = 0, first_empty = 0;
    for (int p = first; p <= last; p++) {
        const char *txt = pages->v[p - 1];
        if (!*txt) {
            if (nempty++) sb_append(&empty, ", ");
            else first_empty = p;
            sb_appendf(&empty, "%d", p);
        } else any = true;
    }
    if (any)
        for (int p = first; p <= last; p++) {
            const char *txt = pages->v[p - 1];
            if (b.len) sb_append(&b, "\n\n");
            if (markers) sb_appendf(&b, "<!-- Page %d -->%s", p, *txt ? "\n" : "");
            sb_append(&b, txt);
        }
    if (nempty) {
        if (b.len) sb_append(&b, "\n\n");
        sb_appendf(&b,
                   "Text extraction is incomplete for PDF page%s %s. %s no extractable text (scanned images or fonts "
                   "without a Unicode mapping); omc cannot render PDF pages as images (e.g. page %d).",
                   nempty > 1 ? "s" : "", sb_cstr(&empty), nempty > 1 ? "Those pages have" : "That page has", first_empty);
    }
    sb_free(&empty);
    return sb_detach(&b);
}
