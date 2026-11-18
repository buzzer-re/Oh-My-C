/* tarx.c - one member out of an untrusted .tar.gz (see tarx.h). */
#include "core/tarx.h"

#include <zlib.h>

#define TARX_BLOCK 512
#define TARX_MAX_NAME 4096

char *tarx_gunzip(const void *in, size_t n, size_t cap, size_t *out_len, char **err) {
    *out_len = 0;
    *err = NULL;
    const unsigned char *src = in;
    if (n < 18 || src[0] != 0x1f || src[1] != 0x8b) {
        *err = xstrdup("not a gzip file");
        return NULL;
    }
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, 15 + 16) != Z_OK) {
        *err = xstrdup("zlib initialization failed");
        return NULL;
    }
    size_t alloc = MIN(cap, MAX(n * 3, (size_t)1 << 16)), used = 0;
    char *out = xmalloc(alloc + 1);
    size_t remaining = n;
    z.next_in = (Bytef *)src;
    bool ended = false;
    for (;;) {
        if (used == alloc) {
            if (alloc >= cap) {
                *err = xasprintf("archive inflates past the %zu-byte limit", cap);
                break;
            }
            alloc = MIN(cap, alloc * 2);
            out = xrealloc(out, alloc + 1);
        }
        uInt in_chunk = (uInt)MIN(remaining, (size_t)1 << 30), out_chunk = (uInt)MIN(alloc - used, (size_t)1 << 30);
        z.avail_in = in_chunk;
        z.next_out = (Bytef *)out + used;
        z.avail_out = out_chunk;
        int rc = inflate(&z, Z_NO_FLUSH);
        used += out_chunk - z.avail_out;
        remaining -= in_chunk - z.avail_in;
        if (rc == Z_STREAM_END) {
            /* another gzip member follows: keep going; anything else after the last member is ignored */
            if (remaining >= 2 && z.next_in[0] == 0x1f && z.next_in[1] == 0x8b) {
                inflateReset(&z);
                continue;
            }
            ended = true;
            break;
        }
        if (rc == Z_OK) continue;
        if (rc == Z_BUF_ERROR && z.avail_out == 0) continue; /* output full: grow */
        *err = xstrdup(rc == Z_BUF_ERROR ? "the archive is truncated" : "the archive is corrupt");
        break;
    }
    inflateEnd(&z);
    if (!ended) {
        if (!*err) *err = xstrdup("the archive is truncated");
        free(out);
        return NULL;
    }
    out[used] = 0;
    *out_len = used;
    return out;
}

/* octal (NUL/space padded) or base-256 numeric field; -1 when malformed or out of range */
static int64_t tar_num(const unsigned char *f, size_t n) {
    if (f[0] & 0x80) {
        if (f[0] != 0x80) return -1; /* negative or > 2^63 */
        uint64_t v = 0;
        for (size_t i = 1; i < n; i++) {
            if (v >> 55) return -1;
            v = v << 8 | f[i];
        }
        return v > INT64_MAX ? -1 : (int64_t)v;
    }
    size_t i = 0;
    while (i < n && f[i] == ' ') i++;
    int64_t v = 0;
    if (i < n && f[i] && (f[i] < '0' || f[i] > '7')) return -1;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
        if (v > (INT64_MAX >> 3)) return -1;
        v = v * 8 + (f[i] - '0');
    }
    for (; i < n; i++)
        if (f[i] != ' ' && f[i] != 0) return -1;
    return v;
}

static bool tar_checksum_ok(const unsigned char *h) {
    int64_t want = tar_num(h + 148, 8);
    uint64_t sum = 0;
    for (int i = 0; i < TARX_BLOCK; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return want >= 0 && (uint64_t)want == sum;
}

static size_t field_len(const unsigned char *f, size_t n) {
    const unsigned char *z = memchr(f, 0, n);
    return z ? (size_t)(z - f) : n;
}

/* the name without leading "./" segments */
static const char *strip_dot(const char *s) {
    while (s[0] == '.' && s[1] == '/') s += 2;
    return s;
}

/* pax extended header records ("<len> <key>=<value>\n"): `path` and `size` override the next header */
static int pax_parse(const char *r, size_t sz, char **path, int64_t *size) {
    size_t p = 0;
    while (p < sz) {
        size_t q = p, reclen = 0;
        while (q < sz && r[q] >= '0' && r[q] <= '9') {
            if (reclen > sz) return -1;
            reclen = reclen * 10 + (size_t)(r[q++] - '0');
        }
        if (q == p || q >= sz || r[q] != ' ' || reclen <= q - p + 1 || reclen > sz - p) return -1;
        const char *kv = r + q + 1, *end = r + p + reclen;
        if (end[-1] != '\n') return -1;
        const char *eq = memchr(kv, '=', (size_t)(end - kv));
        if (!eq) return -1;
        size_t kl = (size_t)(eq - kv), vl = (size_t)(end - 1 - (eq + 1));
        if (kl == 4 && !memcmp(kv, "path", 4)) {
            if (vl > TARX_MAX_NAME || memchr(eq + 1, 0, vl)) return -1;
            free(*path);
            *path = xstrndup(eq + 1, vl);
        } else if (kl == 4 && !memcmp(kv, "size", 4)) {
            int64_t v = 0;
            if (!vl || vl > 18) return -1;
            for (size_t i = 0; i < vl; i++) {
                char c = eq[1 + i];
                if (c < '0' || c > '9') return -1;
                v = v * 10 + (c - '0');
            }
            *size = v;
        }
        p += reclen;
    }
    return 0;
}

int tarx_find(const void *tar, size_t n, const char *name, size_t max_size, const char **data, size_t *len,
              char **err) {
    const unsigned char *b = tar;
    *data = NULL;
    *len = 0;
    *err = NULL;
    const char *want = strip_dot(name);
    char *longname = NULL, *pax_path = NULL;
    int64_t pax_size = -1;
    bool found = false, ended = false;
    size_t off = 0;
    while (off + TARX_BLOCK <= n) {
        const unsigned char *h = b + off;
        bool zero = true;
        for (int i = 0; i < TARX_BLOCK && zero; i++) zero = !h[i];
        if (zero) {
            ended = true;
            break;
        }
        if (!tar_checksum_ok(h)) {
            *err = xstrdup("bad tar header checksum");
            break;
        }
        int64_t size = tar_num(h + 124, 12);
        if (pax_size >= 0) size = pax_size;
        size_t at = off + TARX_BLOCK;
        if (size < 0 || (uint64_t)size > n - at) {
            *err = xstrdup("truncated tar member");
            break;
        }
        size_t padded = ((size_t)size + TARX_BLOCK - 1) / TARX_BLOCK * TARX_BLOCK;
        size_t next = padded > n - at ? n : at + padded;
        char type = (char)h[156];
        if (type == 'L' || type == 'x') {
            if (size > (type == 'L' ? TARX_MAX_NAME : 1 << 20)) {
                *err = xstrdup(type == 'L' ? "tar member name too long" : "tar pax header too large");
                break;
            }
            if (type == 'L') {
                free(longname);
                longname = xstrndup((const char *)b + at, field_len(b + at, (size_t)size));
            } else if (pax_parse((const char *)b + at, (size_t)size, &pax_path, &pax_size) != 0) {
                *err = xstrdup("malformed tar pax header");
                break;
            }
            off = next;
            continue;
        }
        if (type == 'K' || type == 'g') { /* long link name / global pax: nothing this reader needs */
            off = next;
            continue;
        }
        char *member;
        if (longname) member = longname, longname = NULL;
        else if (pax_path) member = pax_path, pax_path = NULL;
        else if (!memcmp(h + 257, "ustar", 5) && h[345])
            member = xasprintf("%.*s/%.*s", (int)field_len(h + 345, 155), (const char *)h + 345,
                               (int)field_len(h, 100), (const char *)h);
        else member = xstrndup((const char *)h, field_len(h, 100));
        free(pax_path);
        pax_path = NULL;
        pax_size = -1;
        bool regular = type == '0' || type == 0 || type == '7';
        bool hit = !strcmp(strip_dot(member), want);
        free(member);
        if (hit && regular) {
            if (found) {
                *err = xasprintf("the archive holds %s more than once", want);
                break;
            }
            if ((uint64_t)size > max_size) {
                *err = xasprintf("%s in the archive is larger than %zu bytes", want, max_size);
                break;
            }
            found = true;
            *data = (const char *)b + at;
            *len = (size_t)size;
        }
        off = next;
    }
    free(longname);
    free(pax_path);
    if (!*err && !ended && off < n) *err = xstrdup("truncated tar header");
    if (*err) {
        *data = NULL;
        *len = 0;
        return -1;
    }
    return found ? 0 : 1;
}
