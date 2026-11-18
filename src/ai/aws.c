/* aws.c - SigV4, event-stream framing, shared config and the credential chain (see aws.h). */
#define _GNU_SOURCE
#include "aws.h"

#include "core/fs.h"
#include "core/proc.h"

#include <dirent.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define REFRESH_SKEW_MS 60000
#define FILE_SESSION_CREDS_TTL_MS (5 * 60000)
#define SHARED_RESOLVE_TIMEOUT_MS 30000
#define IMDS_TIMEOUT_MS 1000
#define MAX_ROLE_CHAIN 32

void aws_creds_free(aws_creds_t *c) {
    if (!c) return;
    free(c->access_key_id);
    free(c->secret_access_key);
    free(c->session_token);
    memset(c, 0, sizeof(*c));
}

void aws_creds_copy(aws_creds_t *dst, const aws_creds_t *src) {
    dst->access_key_id = src->access_key_id ? xstrdup(src->access_key_id) : NULL;
    dst->secret_access_key = src->secret_access_key ? xstrdup(src->secret_access_key) : NULL;
    dst->session_token = src->session_token ? xstrdup(src->session_token) : NULL;
    dst->expires_at = src->expires_at;
}

/* ======================================================================
 * SigV4
 * ====================================================================== */

static const char HEXL[] = "0123456789abcdef";

static void to_hex(const unsigned char *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = HEXL[d[i] >> 4];
        out[2 * i + 1] = HEXL[d[i] & 15];
    }
    out[2 * n] = '\0';
}

char *aws_sha256_hex(const void *data, size_t len) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)(data ? data : ""), len, md);
    char *out = xmalloc(2 * SHA256_DIGEST_LENGTH + 1);
    to_hex(md, SHA256_DIGEST_LENGTH, out);
    return out;
}

static void hmac256(const void *key, size_t klen, const char *msg, size_t mlen, unsigned char out[32]) {
    unsigned int olen = 32;
    HMAC(EVP_sha256(), key, (int)klen, (const unsigned char *)msg, mlen, out, &olen);
}

void aws_signing_key(const char *secret, const char *short_date, const char *region, const char *service,
                     unsigned char out[32]) {
    char *k = xasprintf("AWS4%s", secret ? secret : "");
    unsigned char kd[32], kr[32], ks[32];
    hmac256(k, strlen(k), short_date, strlen(short_date), kd);
    OPENSSL_cleanse(k, strlen(k));
    free(k);
    hmac256(kd, 32, region, strlen(region), kr);
    hmac256(kr, 32, service, strlen(service), ks);
    hmac256(ks, 32, "aws4_request", 12, out);
}

void aws_amz_date(int64_t ms, char buf[17]) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, 17, "%Y%m%dT%H%M%SZ", &tm);
}

static bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

/* encodeURIComponent + [!'()*] escaped == RFC 3986 unreserved kept */
static void rfc3986(sb_t *b, const char *s, size_t n) {
    static const char HEXU[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (unreserved(c)) sb_putc(b, (char)c);
        else {
            sb_putc(b, '%');
            sb_putc(b, HEXU[c >> 4]);
            sb_putc(b, HEXU[c & 15]);
        }
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* decodeURIComponent ('+' stays '+'); malformed escapes are kept literally */
static char *pct_decode(const char *s, size_t n, size_t *out_len) {
    char *o = xmalloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            o[k++] = (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            o[k++] = s[i];
        }
    }
    o[k] = '\0';
    *out_len = k;
    return o;
}

char *aws_canonical_path(const char *path) {
    sb_t b = SB_INIT;
    if (!path || !*path) return xstrdup("/");
    const char *p = path;
    while (1) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        rfc3986(&b, p, n);
        if (!slash) break;
        sb_putc(&b, '/');
        p = slash + 1;
    }
    return sb_detach(&b);
}

typedef struct {
    char *k, *v;
} kv_t;

static int kv_cmp(const void *a, const void *b) {
    const kv_t *x = a, *y = b;
    int c = strcmp(x->k, y->k);
    return c ? c : strcmp(x->v, y->v);
}

char *aws_canonical_query(const char *query) {
    if (!query || !*query) return xstrdup("");
    VEC(kv_t) pairs = {0};
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t n = amp ? (size_t)(amp - p) : strlen(p);
        if (n) {
            const char *eq = memchr(p, '=', n);
            size_t kl = eq ? (size_t)(eq - p) : n;
            size_t dl;
            char *dk = pct_decode(p, kl, &dl);
            sb_t kb = SB_INIT;
            rfc3986(&kb, dk, dl);
            free(dk);
            sb_t vb = SB_INIT;
            if (eq) {
                char *dv = pct_decode(eq + 1, n - kl - 1, &dl);
                rfc3986(&vb, dv, dl);
                free(dv);
            }
            kv_t kv = {sb_detach(&kb), sb_detach(&vb)};
            vec_push(&pairs, kv);
        }
        if (!amp) break;
        p = amp + 1;
    }
    if (pairs.n) xqsort(pairs.v, pairs.n, sizeof(kv_t), kv_cmp);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < pairs.n; i++) {
        if (i) sb_putc(&b, '&');
        sb_appendf(&b, "%s=%s", pairs.v[i].k, pairs.v[i].v);
        free(pairs.v[i].k);
        free(pairs.v[i].v);
    }
    vec_free(&pairs);
    return sb_detach(&b);
}

/* value.trim().replace(/\s+/g, " ") */
static char *collapse_ws(const char *v) {
    sb_t b = SB_INIT;
    bool ws = false;
    const char *s = v ? v : "";
    while (*s && strchr(" \t\r\n\f\v", *s)) s++;
    for (; *s; s++) {
        if (strchr(" \t\r\n\f\v", *s)) {
            ws = true;
            continue;
        }
        if (ws && b.len) sb_putc(&b, ' ');
        ws = false;
        sb_putc(&b, *s);
    }
    return sb_detach(&b);
}

static int kv_cmp_key(const void *a, const void *b) { return strcmp(((const kv_t *)a)->k, ((const kv_t *)b)->k); }

char *aws_sigv4_authorization(const char *method, const char *path, const char *query, const http_headers_t *headers,
                              const char *payload_hash, const char *amz_date, const char *region,
                              const char *service, const char *access_key_id, const char *secret,
                              char **canonical_request, char **string_to_sign) {
    /* lowercase, dedupe (last wins), sort */
    VEC(kv_t) hs = {0};
    for (size_t i = 0; headers && i < headers->n; i++) {
        char *k = str_lower(headers->v[i].name);
        char *v = collapse_ws(headers->v[i].value);
        size_t j;
        for (j = 0; j < hs.n; j++)
            if (!strcmp(hs.v[j].k, k)) break;
        if (j < hs.n) {
            free(hs.v[j].v);
            hs.v[j].v = v;
            free(k);
        } else {
            kv_t kv = {k, v};
            vec_push(&hs, kv);
        }
    }
    if (hs.n) xqsort(hs.v, hs.n, sizeof(kv_t), kv_cmp_key);
    sb_t canon_h = SB_INIT, signed_h = SB_INIT;
    for (size_t i = 0; i < hs.n; i++) {
        sb_appendf(&canon_h, "%s:%s\n", hs.v[i].k, hs.v[i].v);
        if (i) sb_putc(&signed_h, ';');
        sb_append(&signed_h, hs.v[i].k);
        free(hs.v[i].k);
        free(hs.v[i].v);
    }
    vec_free(&hs);
    char *cpath = aws_canonical_path(path);
    char *cquery = aws_canonical_query(query);
    char *upper = xstrdup(method ? method : "GET");
    for (char *c = upper; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    char *creq = xasprintf("%s\n%s\n%s\n%s\n%s\n%s", upper, cpath, cquery, sb_cstr(&canon_h), sb_cstr(&signed_h),
                           payload_hash);
    free(upper);
    free(cpath);
    free(cquery);
    char short_date[9];
    snprintf(short_date, sizeof short_date, "%.8s", amz_date);
    char *scope = xasprintf("%s/%s/%s/aws4_request", short_date, region, service);
    char *creq_hash = aws_sha256_hex(creq, strlen(creq));
    char *sts = xasprintf("AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, scope, creq_hash);
    free(creq_hash);
    unsigned char key[32], sig[32];
    aws_signing_key(secret, short_date, region, service, key);
    hmac256(key, 32, sts, strlen(sts), sig);
    OPENSSL_cleanse(key, sizeof key);
    char sighex[65];
    to_hex(sig, 32, sighex);
    char *auth = xasprintf("AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
                           access_key_id ? access_key_id : "", scope, sb_cstr(&signed_h), sighex);
    free(scope);
    sb_free(&canon_h);
    sb_free(&signed_h);
    if (canonical_request) *canonical_request = creq;
    else free(creq);
    if (string_to_sign) *string_to_sign = sts;
    else free(sts);
    return auth;
}

static bool unsignable(const char *lk) {
    static const char *const U[] = {"authorization", "cache-control", "connection", "expect", "from",
                                    "keep-alive", "max-forwards", "pragma", "referer", "te",
                                    "trailer", "transfer-encoding", "upgrade", "user-agent", "x-amzn-trace-id"};
    for (size_t i = 0; i < ARRAY_LEN(U); i++)
        if (!strcmp(lk, U[i])) return true;
    return str_starts(lk, "proxy-") || str_starts(lk, "sec-");
}

void aws_sigv4_sign(const aws_sign_params_t *p, http_headers_t *out) {
    int64_t t = p->time_ms ? p->time_ms : now_ms();
    char amz[17];
    aws_amz_date(t, amz);
    char *payload_hash = aws_sha256_hex(p->body, p->body_len);
    http_headers_t sh = {0};
    http_headers_add(&sh, "host", p->host);
    http_headers_add(&sh, "x-amz-date", amz);
    http_headers_add(&sh, "x-amz-content-sha256", payload_hash);
    const char *tok = p->creds->session_token;
    if (tok && *tok) http_headers_add(&sh, "x-amz-security-token", tok);
    for (size_t i = 0; p->headers && i < p->headers->n; i++) {
        char *lk = str_lower(p->headers->v[i].name);
        if (!unsignable(lk)) http_headers_add(&sh, lk, p->headers->v[i].value);
        free(lk);
    }
    char *auth = aws_sigv4_authorization(p->method, p->path, p->query, &sh, payload_hash, amz, p->region,
                                         p->service, p->creds->access_key_id, p->creds->secret_access_key, NULL,
                                         NULL);
    http_headers_free(&sh);
    http_headers_set(out, "host", p->host);
    http_headers_set(out, "x-amz-date", amz);
    http_headers_set(out, "x-amz-content-sha256", payload_hash);
    if (tok && *tok) http_headers_set(out, "x-amz-security-token", tok);
    http_headers_set(out, "authorization", auth);
    free(auth);
    free(payload_hash);
}

/* ======================================================================
 * event stream
 * ====================================================================== */

#define ES_PRELUDE 8
#define ES_HDR_OFF 12
#define ES_MIN 16

uint32_t aws_crc32(const void *data, size_t len) {
    uLong c = crc32(0L, Z_NULL, 0);
    const unsigned char *p = data;
    while (len > 0) {
        uInt n = len > 0x40000000u ? 0x40000000u : (uInt)len;
        c = crc32(c, p, n);
        p += n;
        len -= n;
    }
    return (uint32_t)c;
}

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t be16(const unsigned char *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static int64_t be64s(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return (int64_t)v;
}
static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

void aws_es_msg_free(aws_es_msg_t *m) {
    http_headers_free(&m->headers);
    m->payload = NULL;
    m->payload_len = 0;
}

static int es_headers(const unsigned char *b, size_t n, http_headers_t *out, char **err) {
    size_t p = 0;
#define NEED(k)                                                         \
    do {                                                                \
        if (n - p < (size_t)(k)) {                                      \
            *err = xstrdup("event stream header block truncated");      \
            return -1;                                                  \
        }                                                               \
    } while (0)
    while (p < n) {
        NEED(1);
        size_t nl = b[p++];
        NEED(nl + 1);
        char *name = xstrndup((const char *)b + p, nl);
        p += nl;
        int type = b[p++];
        char *val = NULL;
        switch (type) {
        case 0: val = xstrdup("true"); break;
        case 1: val = xstrdup("false"); break;
        case 2:
            if (n - p < 1) goto trunc;
            val = xasprintf("%d", (int)(int8_t)b[p]);
            p += 1;
            break;
        case 3:
            if (n - p < 2) goto trunc;
            val = xasprintf("%d", (int)(int16_t)be16(b + p));
            p += 2;
            break;
        case 4:
            if (n - p < 4) goto trunc;
            val = xasprintf("%d", (int)(int32_t)be32(b + p));
            p += 4;
            break;
        case 5:
            if (n - p < 8) goto trunc;
            val = xasprintf("%lld", (long long)be64s(b + p));
            p += 8;
            break;
        case 6: {
            if (n - p < 2) goto trunc;
            size_t l = be16(b + p);
            p += 2;
            if (n - p < l) goto trunc;
            val = base64_encode(b + p, l);
            p += l;
            break;
        }
        case 7: {
            if (n - p < 2) goto trunc;
            size_t l = be16(b + p);
            p += 2;
            if (n - p < l) goto trunc;
            val = xstrndup((const char *)b + p, l);
            p += l;
            break;
        }
        case 8: {
            if (n - p < 8) goto trunc;
            int64_t ms = be64s(b + p);
            p += 8;
            time_t t = (time_t)(ms >= 0 ? ms / 1000 : (ms - 999) / 1000);
            int frac = (int)(ms - (int64_t)t * 1000);
            struct tm tm;
            gmtime_r(&t, &tm);
            char buf[40];
            strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
            val = xasprintf("%s.%03dZ", buf, frac);
            break;
        }
        case 9: {
            if (n - p < 16) goto trunc;
            char h[33];
            to_hex(b + p, 16, h);
            val = xasprintf("%.8s-%.4s-%.4s-%.4s-%.12s", h, h + 8, h + 12, h + 16, h + 20);
            p += 16;
            break;
        }
        default:
            *err = xasprintf("unknown header value type %d", type);
            free(name);
            return -1;
        }
        http_headers_set(out, name, val);
        free(name);
        free(val);
        continue;
    trunc:
        free(name);
        *err = xstrdup("event stream header block truncated");
        return -1;
    }
#undef NEED
    return 0;
}

int aws_es_decode_message(const unsigned char *frame, size_t len, aws_es_msg_t *out, char **err) {
    memset(out, 0, sizeof(*out));
    *err = NULL;
    if (len < ES_MIN) {
        *err = xstrdup("frame too short");
        return -1;
    }
    uint32_t total = be32(frame);
    if (total != len) {
        *err = xasprintf("framed length %u != buffer %zu", total, len);
        return -1;
    }
    uint32_t hlen = be32(frame + 4);
    if (aws_crc32(frame, ES_PRELUDE) != be32(frame + 8)) {
        *err = xstrdup("prelude CRC mismatch");
        return -1;
    }
    if (aws_crc32(frame, total - 4) != be32(frame + total - 4)) {
        *err = xstrdup("message CRC mismatch");
        return -1;
    }
    if ((size_t)hlen > (size_t)total - ES_MIN) {
        *err = xasprintf("headers length %u exceeds frame", hlen);
        return -1;
    }
    if (es_headers(frame + ES_HDR_OFF, hlen, &out->headers, err) != 0) {
        http_headers_free(&out->headers);
        return -1;
    }
    out->payload = frame + ES_HDR_OFF + hlen;
    out->payload_len = total - ES_MIN - hlen;
    return 0;
}

unsigned char *aws_es_encode(const char *const *names, const char *const *values, size_t nhdr, const void *payload,
                             size_t payload_len, size_t *out_len) {
    sb_t h = SB_INIT;
    for (size_t i = 0; i < nhdr; i++) {
        size_t nl = strlen(names[i]), vl = strlen(values[i]);
        if (nl > 255) nl = 255;
        if (vl > 65535) vl = 65535;
        sb_putc(&h, (char)nl);
        sb_appendn(&h, names[i], nl);
        sb_putc(&h, 7);
        sb_putc(&h, (char)(vl >> 8));
        sb_putc(&h, (char)(vl & 0xff));
        sb_appendn(&h, values[i], vl);
    }
    size_t total = ES_MIN + h.len + payload_len;
    unsigned char *f = xmalloc(total);
    put32(f, (uint32_t)total);
    put32(f + 4, (uint32_t)h.len);
    put32(f + 8, aws_crc32(f, 8));
    if (h.len) memcpy(f + ES_HDR_OFF, h.s, h.len);
    if (payload_len) memcpy(f + ES_HDR_OFF + h.len, payload, payload_len);
    put32(f + total - 4, aws_crc32(f, total - 4));
    sb_free(&h);
    *out_len = total;
    return f;
}

void aws_es_init(aws_es_decoder_t *d, aws_es_cb cb, void *ud) {
    memset(d, 0, sizeof(*d));
    d->cb = cb;
    d->ud = ud;
}

int aws_es_feed(aws_es_decoder_t *d, const void *data, size_t len) {
    if (d->error) return -1;
    if (len) sb_appendn(&d->buf, data, len);
    size_t off = 0;
    int rc = 0;
    /* declaredMessageLength: the 12-byte prelude is validated (prelude CRC first, then the lengths) before any more of
     * the message is buffered, so a corrupt length prefix never drives buffering */
    while (d->buf.len - off >= ES_HDR_OFF) {
        const unsigned char *p = (const unsigned char *)d->buf.s + off;
        uint32_t total = be32(p), hlen = be32(p + 4);
        if (aws_crc32(p, ES_PRELUDE) != be32(p + 8)) d->error = xstrdup("prelude CRC mismatch");
        else if (total < ES_MIN) d->error = xasprintf("total length %u below minimum", total);
        else if (total > AWS_ES_MAX_MESSAGE) d->error = xasprintf("total length %u exceeds maximum %u", total, AWS_ES_MAX_MESSAGE);
        else if (hlen > AWS_ES_MAX_HEADERS)
            d->error = xasprintf("headers length %u exceeds maximum %u", hlen, AWS_ES_MAX_HEADERS);
        else if (hlen > total - ES_MIN) d->error = xasprintf("headers length %u exceeds message length %u", hlen, total);
        if (d->error) {
            rc = -1;
            break;
        }
        if (d->buf.len - off < total) break;
        aws_es_msg_t m;
        char *err = NULL;
        if (aws_es_decode_message(p, total, &m, &err) != 0) {
            d->error = err;
            rc = -1;
            break;
        }
        off += total;
        rc = d->cb ? d->cb(d->ud, &m) : 0;
        aws_es_msg_free(&m);
        if (rc) break;
    }
    if (off) {
        memmove(d->buf.s, d->buf.s + off, d->buf.len - off);
        sb_truncate(&d->buf, d->buf.len - off);
    }
    return rc;
}

int aws_es_finish(aws_es_decoder_t *d) {
    if (d->error) return -1;
    if (d->buf.len > 0) {
        d->error = xstrdup("truncated message at end of stream");
        return -1;
    }
    return 0;
}

void aws_es_free(aws_es_decoder_t *d) {
    sb_free(&d->buf);
    free(d->error);
    d->error = NULL;
}

/* ======================================================================
 * shared config / profiles
 * ====================================================================== */

json_t *aws_ini_parse(const char *text) {
    json_t *out = json_obj();
    json_t *cur = NULL;
    strvec_t lines = str_split_lines(text ? text : "", text ? strlen(text) : 0);
    for (size_t i = 0; i < lines.n; i++) {
        char *line = str_trim(lines.v[i]);
        size_t n = strlen(line);
        if (!n || line[0] == '#' || line[0] == ';') {
            free(line);
            continue;
        }
        if (line[0] == '[' && line[n - 1] == ']') {
            line[n - 1] = '\0';
            char *name = str_trim(line + 1);
            char *final;
            if (str_starts(name, "profile ")) {
                final = str_trim(name + 8);
            } else {
                final = xstrdup(name);
            }
            if (str_starts(final, "sso-session ")) {
                char *t = str_trim(final + 12);
                char *f2 = xasprintf("sso-session:%s", t);
                free(t);
                free(final);
                final = f2;
            }
            cur = json_get_obj(out, final);
            if (!cur) {
                json_set(out, final, json_obj());
                cur = json_get(out, final);
            }
            free(final);
            free(name);
            free(line);
            continue;
        }
        char *eq = cur ? strchr(line, '=') : NULL;
        if (eq) {
            *eq = '\0';
            char *k = str_trim(line), *v = str_trim(eq + 1);
            json_set(cur, k, json_str_own(v));
            free(k);
        }
        free(line);
    }
    strvec_free(&lines);
    return out;
}

static json_t *read_ini(const char *path) {
    size_t n;
    char *s = path ? fs_read(path, &n) : NULL;
    if (!s) return NULL;
    json_t *j = aws_ini_parse(s);
    free(s);
    return j;
}

const char *aws_profile_name(const char *profile) {
    if (profile && *profile) return profile;
    const char *e = env_get("AWS_PROFILE");
    return e ? e : "default";
}

bool aws_should_load_shared_config(const char *profile) {
    if ((profile && *profile) || env_get("AWS_PROFILE")) return true;
    const char *v = env_get("AWS_SDK_LOAD_CONFIG");
    return v && (!strcmp(v, "1") || !strcasecmp(v, "true"));
}

static char *aws_home_file(const char *env, const char *name) {
    const char *e = env_get(env);
    if (e) return xstrdup(e);
    return path_join3(home_dir(), ".aws", name);
}

char *aws_credentials_path(void) { return aws_home_file("AWS_SHARED_CREDENTIALS_FILE", "credentials"); }
char *aws_config_path(void) { return aws_home_file("AWS_CONFIG_FILE", "config"); }

char *aws_profile_region(const char *profile) {
    if (!aws_should_load_shared_config(profile)) return NULL;
    char *p = aws_config_path();
    json_t *ini = read_ini(p);
    free(p);
    const char *r = json_get_str(json_get_obj(ini, aws_profile_name(profile)), "region");
    char *out = r && *r ? xstrdup(r) : NULL;
    json_free(ini);
    return out;
}

char *aws_ambient_region(const char *profile) {
    const char *e = env_get("AWS_REGION");
    if (!e) e = env_get("AWS_DEFAULT_REGION");
    if (e) return xstrdup(e);
    return aws_profile_region(profile);
}

char *aws_resolve_region(const char *explicit_region, const char *profile) {
    if (explicit_region && *explicit_region) return xstrdup(explicit_region);
    char *r = aws_ambient_region(profile);
    return r ? r : xstrdup("us-east-1");
}

static bool env_is_true(const char *name) {
    const char *v = env_get(name);
    return v && !strcasecmp(v, "true");
}

/* merged view: config section overlaid by credentials section (owned) */
static json_t *merged_profile(const json_t *cred, const json_t *cfg, const char *profile) {
    json_t *m = json_obj();
    json_t *a = json_get_obj(cfg, profile), *b = json_get_obj(cred, profile);
    if (a) json_merge(m, a);
    if (b) json_merge(m, b);
    return m;
}

static bool profile_has_source(const char *profile, const json_t *cred, const json_t *cfg, strvec_t *seen) {
    if (strvec_contains(seen, profile) || seen->n > MAX_ROLE_CHAIN) return false;
    strvec_push(seen, profile);
    json_t *m = merged_profile(cred, cfg, profile);
    bool ok = false;
    if (json_get_str(m, "role_arn")) {
        const char *cs = json_get_str(m, "credential_source"), *sp = json_get_str(m, "source_profile");
        if (json_get_str(m, "web_identity_token_file")) ok = true;
        else if (json_get_str(m, "mfa_serial")) ok = false;
        else if (cs) {
            if (!strcmp(cs, "Environment")) ok = env_get("AWS_ACCESS_KEY_ID") && env_get("AWS_SECRET_ACCESS_KEY");
            else if (!strcmp(cs, "EcsContainer"))
                ok = env_get("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI") || env_get("AWS_CONTAINER_CREDENTIALS_FULL_URI");
            else if (!strcmp(cs, "Ec2InstanceMetadata")) ok = !env_is_true("AWS_EC2_METADATA_DISABLED");
        } else if (sp) {
            ok = profile_has_source(sp, cred, cfg, seen);
        }
    } else if (json_get_str(m, "aws_access_key_id") && json_get_str(m, "aws_secret_access_key")) {
        ok = true;
    } else if (json_get_str(m, "credential_process")) {
        ok = true;
    } else if (json_get_str(m, "sso_account_id") && json_get_str(m, "sso_role_name")) {
        if (json_get_str(m, "sso_start_url") && json_get_str(m, "sso_region")) ok = true;
        else {
            const char *sess = json_get_str(m, "sso_session");
            json_t *s = NULL;
            if (sess) {
                char *k = xasprintf("sso-session:%s", sess);
                s = json_get_obj(cfg, k);
                free(k);
            }
            ok = s && json_get_str(s, "sso_start_url") && json_get_str(s, "sso_region");
        }
    }
    json_free(m);
    return ok;
}

bool aws_has_configured_profile(const char *profile) {
    char *cp = aws_credentials_path(), *fp = aws_config_path();
    json_t *cred = read_ini(cp);
    json_t *cfg = aws_should_load_shared_config(profile) ? read_ini(fp) : NULL;
    strvec_t seen = {0};
    bool ok = profile_has_source(aws_profile_name(profile), cred, cfg, &seen);
    strvec_free(&seen);
    json_free(cred);
    json_free(cfg);
    free(cp);
    free(fp);
    return ok;
}

static int g_ec2 = -1;

static bool is_ec2_host(void) {
    if (g_ec2 >= 0) return g_ec2 == 1;
    static const struct {
        const char *path;
        int kind; /* 0 prefix ec2, 1 prefix ec2 or i-, 2 contains "amazon ec2" */
    } checks[] = {
        {"/sys/hypervisor/uuid", 0},
        {"/sys/devices/virtual/dmi/id/product_uuid", 0},
        {"/sys/devices/virtual/dmi/id/board_asset_tag", 1},
        {"/sys/devices/virtual/dmi/id/sys_vendor", 2},
        {"/sys/devices/virtual/dmi/id/bios_vendor", 2},
    };
    int found = 0;
    for (size_t i = 0; i < ARRAY_LEN(checks) && !found; i++) {
        size_t n;
        bool tr;
        char *s = fs_read_limit(checks[i].path, 4096, &n, &tr);
        if (!s) continue;
        char *t = str_trim(s), *l = str_lower(t);
        if (checks[i].kind == 0) found = str_starts(l, "ec2");
        else if (checks[i].kind == 1) found = str_starts(l, "ec2") || str_starts(l, "i-");
        else found = strstr(l, "amazon ec2") != NULL;
        free(s);
        free(t);
        free(l);
    }
    g_ec2 = found;
    return found;
}

static pthread_mutex_t g_probe_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_probe_key;
static bool g_probe_val;

static void file_stamp(sb_t *b, const char *p) {
    struct stat st;
    if (stat(p, &st) == 0) sb_appendf(b, "%lld.%09ld:%lld", (long long)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec,
                                      (long long)st.st_size);
    else sb_putc(b, '-');
}

static bool cached_profile_probe(void) {
    char *cp = aws_credentials_path(), *fp = aws_config_path();
    sb_t k = SB_INIT;
    const char *ev = env_get("AWS_PROFILE");
    sb_appendf(&k, "%s|%s|%d|%d|%d|%s|", ev ? ev : "", env_get("AWS_SDK_LOAD_CONFIG") ? env_get("AWS_SDK_LOAD_CONFIG") : "",
               env_get("AWS_ACCESS_KEY_ID") && env_get("AWS_SECRET_ACCESS_KEY"),
               env_get("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI") || env_get("AWS_CONTAINER_CREDENTIALS_FULL_URI"),
               env_is_true("AWS_EC2_METADATA_DISABLED"), cp);
    file_stamp(&k, cp);
    sb_appendf(&k, "|%s|", fp);
    file_stamp(&k, fp);
    free(cp);
    free(fp);
    pthread_mutex_lock(&g_probe_mu);
    bool hit = g_probe_key && !strcmp(g_probe_key, sb_cstr(&k));
    bool v = g_probe_val;
    pthread_mutex_unlock(&g_probe_mu);
    if (hit) {
        sb_free(&k);
        return v;
    }
    v = aws_has_configured_profile(NULL);
    pthread_mutex_lock(&g_probe_mu);
    free(g_probe_key);
    g_probe_key = sb_detach(&k);
    g_probe_val = v;
    pthread_mutex_unlock(&g_probe_mu);
    return v;
}

bool aws_has_credential_source(void) {
    if (env_get("AWS_ACCESS_KEY_ID") && env_get("AWS_SECRET_ACCESS_KEY")) return true;
    if (env_get("AWS_BEARER_TOKEN_BEDROCK")) return true;
    if (env_get("AWS_WEB_IDENTITY_TOKEN_FILE") && env_get("AWS_ROLE_ARN")) return true;
    if (env_get("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI") || env_get("AWS_CONTAINER_CREDENTIALS_FULL_URI")) return true;
    bool instance = !env_is_true("AWS_EC2_METADATA_DISABLED") &&
                    (env_get("AWS_EC2_METADATA_SERVICE_ENDPOINT") || is_ec2_host());
    return instance || cached_profile_probe();
}

const char *aws_bearer_token(const char *api_key) {
    if (api_key && *api_key) return api_key;
    return env_get("AWS_BEARER_TOKEN_BEDROCK");
}

/* ======================================================================
 * credential helpers
 * ====================================================================== */

int64_t aws_parse_iso8601(const char *s) {
    if (!s) return -1;
    while (*s == ' ') s++;
    int Y, M, D, h = 0, mi = 0, sec = 0, used = 0;
    if (sscanf(s, "%4d-%2d-%2d%n", &Y, &M, &D, &used) != 3) return -1;
    const char *p = s + used;
    int64_t ms = 0;
    if (*p == 'T' || *p == 't' || *p == ' ') {
        p++;
        int u2 = 0;
        if (sscanf(p, "%2d:%2d:%2d%n", &h, &mi, &sec, &u2) != 3) return -1;
        p += u2;
        if (*p == '.') {
            p++;
            int digits = 0, frac = 0;
            while (*p >= '0' && *p <= '9') {
                if (digits < 3) {
                    frac = frac * 10 + (*p - '0');
                    digits++;
                }
                p++;
            }
            while (digits < 3) {
                frac *= 10;
                digits++;
            }
            ms = frac;
        }
    }
    int off = 0;
    if (*p == 'Z' || *p == 'z') p++;
    else if (*p == '+' || *p == '-') {
        int sign = *p == '-' ? -1 : 1, oh = 0, om = 0;
        p++;
        if (sscanf(p, "%2d:%2d", &oh, &om) == 2 || sscanf(p, "%2d%2d", &oh, &om) == 2) off = sign * (oh * 60 + om);
        else return -1;
    }
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || sec > 60) return -1;
    struct tm tm = {0};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    int64_t t = (int64_t)timegm(&tm);
    return (t - (int64_t)off * 60) * 1000 + ms;
}

char *aws_xml_tag(const char *xml, const char *tag) {
    if (!xml || !tag) return NULL;
    char *open = xasprintf("<%s>", tag), *close = xasprintf("</%s>", tag);
    const char *a = strstr(xml, open);
    char *out = NULL;
    if (a) {
        a += strlen(open);
        const char *b = strstr(a, close);
        if (b && b > a) {
            char *raw = xstrndup(a, (size_t)(b - a));
            char *s1 = str_replace(raw, "&amp;", "&");
            char *s2 = str_replace(s1, "&lt;", "<");
            char *s3 = str_replace(s2, "&gt;", ">");
            char *s4 = str_replace(s3, "&quot;", "\"");
            out = str_replace(s4, "&apos;", "'");
            free(raw);
            free(s1);
            free(s2);
            free(s3);
            free(s4);
        }
    }
    free(open);
    free(close);
    return out;
}

strvec_t aws_tokenize_command(const char *cmd, char **err) {
    strvec_t toks = {0};
    sb_t cur = SB_INIT;
    bool has = false;
    int mode = 0; /* 0 normal, 1 single, 2 double */
    *err = NULL;
    size_t n = cmd ? strlen(cmd) : 0;
    for (size_t i = 0; i < n; i++) {
        char ch = cmd[i];
        if (mode == 0) {
            if (ch == '\'') {
                mode = 1;
                has = true;
            } else if (ch == '"') {
                mode = 2;
                has = true;
            } else if (ch == '\\' && i + 1 < n) {
                sb_putc(&cur, cmd[++i]);
                has = true;
            } else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                if (has) {
                    strvec_push(&toks, sb_cstr(&cur));
                    sb_clear(&cur);
                    has = false;
                }
            } else {
                sb_putc(&cur, ch);
                has = true;
            }
        } else if (mode == 1) {
            if (ch == '\'') mode = 0;
            else sb_putc(&cur, ch);
        } else {
            if (ch == '"') mode = 0;
            else if (ch == '\\' && i + 1 < n) {
                char nx = cmd[i + 1];
                if (nx == '$' || nx == '`' || nx == '"' || nx == '\\') {
                    sb_putc(&cur, nx);
                    i++;
                } else {
                    sb_putc(&cur, ch);
                }
            } else {
                sb_putc(&cur, ch);
            }
        }
    }
    if (mode != 0) {
        *err = xstrdup("AWS credential_process command has an unterminated quote.");
        strvec_free(&toks);
        sb_free(&cur);
        return toks;
    }
    if (has) strvec_push(&toks, sb_cstr(&cur));
    sb_free(&cur);
    return toks;
}

/* URLSearchParams serialization (application/x-www-form-urlencoded) */
static void form_append(sb_t *b, const char *k, const char *v) {
    static const char HEXU[] = "0123456789ABCDEF";
    const char *parts[2] = {k, v};
    if (b->len) sb_putc(b, '&');
    for (int j = 0; j < 2; j++) {
        if (j) sb_putc(b, '=');
        for (const unsigned char *s = (const unsigned char *)parts[j]; *s; s++) {
            unsigned char c = *s;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '*' || c == '-' ||
                c == '.' || c == '_')
                sb_putc(b, (char)c);
            else if (c == ' ') sb_putc(b, '+');
            else {
                sb_putc(b, '%');
                sb_putc(b, HEXU[c >> 4]);
                sb_putc(b, HEXU[c & 15]);
            }
        }
    }
}

typedef struct {
    volatile sig_atomic_t *cancel;
    int64_t deadline; /* mono ms */
} rctx_t;

static int64_t remaining(const rctx_t *r, int64_t cap) {
    int64_t left = r->deadline - mono_ms();
    if (left < 1) left = 1;
    return cap > 0 && cap < left ? cap : left;
}

static int do_http(const rctx_t *r, const char *method, const char *url, const http_headers_t *h, const char *body,
                   int64_t timeout_cap, bool no_proxy, http_resp_t *resp) {
    int64_t t = remaining(r, timeout_cap);
    http_req_t req = {.method = method,
                      .url = url,
                      .headers = h,
                      .body = body,
                      .body_len = body ? strlen(body) : 0,
                      .timeout_ms = t,
                      .connect_timeout_ms = t,
                      .no_proxy = no_proxy,
                      .max_body = 1 << 20,
                      .cancel = r->cancel};
    return http_request(&req, resp);
}

static char *sts_endpoint(const char *region) {
    return xasprintf("https://sts.%s.%s/", region, str_starts(region, "cn-") ? "amazonaws.com.cn" : "amazonaws.com");
}

static int parse_sts(const char *xml, const char *source, aws_creds_t *out, char **err) {
    char *ak = aws_xml_tag(xml, "AccessKeyId"), *sk = aws_xml_tag(xml, "SecretAccessKey");
    char *tok = aws_xml_tag(xml, "SessionToken"), *exp = aws_xml_tag(xml, "Expiration");
    int rc = 0;
    if (!ak || !sk || !tok) {
        *err = xasprintf("%s response is missing credentials.", source);
        rc = -1;
    } else {
        int64_t e = aws_parse_iso8601(exp);
        if (e < 0) {
            *err = xasprintf("%s response has a missing or invalid Expiration.", source);
            rc = -1;
        } else {
            out->access_key_id = ak;
            out->secret_access_key = sk;
            out->session_token = tok;
            out->expires_at = e;
            ak = sk = tok = NULL;
        }
    }
    free(ak);
    free(sk);
    free(tok);
    free(exp);
    return rc;
}

static char *xml_error_message(const char *xml) {
    char *m = aws_xml_tag(xml, "Message");
    if (m) return m;
    return xstrndup(xml ? xml : "", MIN(strlen(xml ? xml : ""), (size_t)200));
}

static char *session_name(const char *configured) {
    if (configured && *configured) return xstrdup(configured);
    return xasprintf("omp-%d", (int)getpid());
}

static int assume_role_web_identity(const rctx_t *r, const char *role_arn, const char *token_file,
                                    const char *sess, const char *region, aws_creds_t *out, char **err) {
    size_t n;
    char *raw = fs_read(token_file, &n);
    if (!raw) {
        *err = xasprintf("Unable to read AWS web identity token file: %s", token_file);
        return -1;
    }
    char *tok = str_trim(raw);
    free(raw);
    if (!*tok) {
        free(tok);
        *err = xstrdup("AWS web identity token file is empty.");
        return -1;
    }
    char *sn = session_name(sess);
    sb_t body = SB_INIT;
    form_append(&body, "Action", "AssumeRoleWithWebIdentity");
    form_append(&body, "Version", "2011-06-15");
    form_append(&body, "RoleArn", role_arn);
    form_append(&body, "RoleSessionName", sn);
    form_append(&body, "WebIdentityToken", tok);
    free(sn);
    free(tok);
    char *url = sts_endpoint(region);
    http_headers_t h = {0};
    http_headers_add(&h, "content-type", "application/x-www-form-urlencoded");
    http_resp_t resp;
    int rc = do_http(r, "POST", url, &h, sb_cstr(&body), 0, false, &resp);
    http_headers_free(&h);
    free(url);
    sb_free(&body);
    int ret;
    if (rc != 0) {
        *err = xasprintf("AWS AssumeRoleWithWebIdentity failed: %s", resp.error ? resp.error : "network error");
        ret = -1;
    } else if (resp.status < 200 || resp.status >= 300) {
        char *m = xml_error_message(resp.body);
        *err = xasprintf("AWS AssumeRoleWithWebIdentity failed: %d %s", resp.status, m);
        free(m);
        ret = -1;
    } else {
        ret = parse_sts(resp.body, "AWS web identity", out, err);
    }
    http_resp_free(&resp);
    return ret;
}

static int sts_assume_role(const rctx_t *r, const aws_creds_t *base, const char *role_arn, const char *region,
                           const json_t *prof, aws_creds_t *out, char **err) {
    char *sn = session_name(json_get_str(prof, "role_session_name"));
    sb_t body = SB_INIT;
    form_append(&body, "Action", "AssumeRole");
    form_append(&body, "Version", "2011-06-15");
    form_append(&body, "RoleArn", role_arn);
    form_append(&body, "RoleSessionName", sn);
    free(sn);
    const char *dur = json_get_str(prof, "duration_seconds"), *ext = json_get_str(prof, "external_id");
    if (dur && *dur) form_append(&body, "DurationSeconds", dur);
    if (ext && *ext) form_append(&body, "ExternalId", ext);
    char *url = sts_endpoint(region);
    url_t u;
    if (url_parse(url, &u) != 0) {
        free(url);
        sb_free(&body);
        *err = xstrdup("invalid STS endpoint");
        return -1;
    }
    http_headers_t extra = {0};
    http_headers_add(&extra, "content-type", "application/x-www-form-urlencoded");
    http_headers_t h = {0};
    aws_sign_params_t sp = {.method = "POST",
                            .host = u.host,
                            .path = "/",
                            .body = body.s,
                            .body_len = body.len,
                            .region = region,
                            .service = "sts",
                            .creds = base,
                            .headers = &extra};
    aws_sigv4_sign(&sp, &h);
    http_headers_set(&h, "content-type", "application/x-www-form-urlencoded");
    http_headers_free(&extra);
    url_free(&u);
    http_resp_t resp;
    int rc = do_http(r, "POST", url, &h, sb_cstr(&body), 0, false, &resp);
    http_headers_free(&h);
    free(url);
    sb_free(&body);
    int ret;
    if (rc != 0) {
        *err = xasprintf("AWS AssumeRole failed: %s", resp.error ? resp.error : "network error");
        ret = -1;
    } else if (resp.status < 200 || resp.status >= 300) {
        char *m = xml_error_message(resp.body);
        *err = xasprintf("AWS AssumeRole failed: %d %s", resp.status, m);
        free(m);
        ret = -1;
    } else {
        ret = parse_sts(resp.body, "AWS AssumeRole", out, err);
    }
    http_resp_free(&resp);
    return ret;
}

/* ---------- env / web identity ---------- */

static bool read_env_creds(aws_creds_t *out) {
    const char *ak = env_get("AWS_ACCESS_KEY_ID"), *sk = env_get("AWS_SECRET_ACCESS_KEY");
    if (!ak || !sk) return false;
    const char *tok = env_get("AWS_SESSION_TOKEN");
    out->access_key_id = xstrdup(ak);
    out->secret_access_key = xstrdup(sk);
    out->session_token = tok ? xstrdup(tok) : NULL;
    out->expires_at = 0;
    return true;
}

/* ---------- container ---------- */

static bool local_or_metadata_host(const char *h) {
    if (!h) return false;
    if (!strcasecmp(h, "localhost") || str_starts(h, "127.") || !strcmp(h, "::1") || !strcmp(h, "[::1]")) return true;
    return !strcmp(h, "169.254.170.2") || !strcmp(h, "169.254.170.23") || !strcmp(h, "169.254.169.254") ||
           !strcasecmp(h, "fd00:ec2::23") || !strcasecmp(h, "fd00:ec2::254");
}

/* 0 ok, 1 not configured, -1 error */
static int read_container_creds(const rctx_t *r, aws_creds_t *out, char **err) {
    const char *rel = env_get("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI"), *full = env_get("AWS_CONTAINER_CREDENTIALS_FULL_URI");
    if (!rel && !full) return 1;
    char *url;
    if (rel) {
        if (rel[0] != '/' || rel[1] == '/') {
            *err = xstrdup("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI must be a single-host absolute path.");
            return -1;
        }
        url = xasprintf("http://169.254.170.2%s", rel);
    } else {
        url_t u;
        if (url_parse(full, &u) != 0) {
            *err = xasprintf("AWS_CONTAINER_CREDENTIALS_FULL_URI is invalid: %s", full);
            return -1;
        }
        bool ok = !strcmp(u.scheme, "https") || local_or_metadata_host(u.host);
        url_free(&u);
        if (!ok) {
            *err = xstrdup("AWS_CONTAINER_CREDENTIALS_FULL_URI must use HTTPS or a local metadata host.");
            return -1;
        }
        url = xstrdup(full);
    }
    char *auth = NULL;
    const char *a = env_get("AWS_CONTAINER_AUTHORIZATION_TOKEN");
    if (a) auth = xstrdup(a);
    else if ((a = env_get("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE"))) {
        size_t n;
        char *raw = fs_read(a, &n);
        if (!raw) {
            free(url);
            *err = xasprintf("Unable to read AWS container authorization token file: %s", a);
            return -1;
        }
        auth = str_trim(raw);
        free(raw);
    }
    http_headers_t h = {0};
    if (auth) http_headers_add(&h, "authorization", auth);
    free(auth);
    http_resp_t resp;
    int rc = do_http(r, "GET", url, &h, NULL, 0, true, &resp);
    http_headers_free(&h);
    free(url);
    int ret = -1;
    if (rc != 0) {
        *err = xasprintf("AWS container credential endpoint failed: %s", resp.error ? resp.error : "network error");
    } else if (resp.status < 200 || resp.status >= 300) {
        *err = xasprintf("AWS container credential endpoint failed: %d %.200s", resp.status, resp.body ? resp.body : "");
    } else {
        json_t *j = json_parse_str(resp.body);
        const char *ak = json_get_str(j, "AccessKeyId"), *sk = json_get_str(j, "SecretAccessKey");
        const char *tok = json_get_str(j, "Token");
        if (!ak || !*ak || !sk || !*sk || !tok || !*tok) {
            *err = xstrdup("AWS container credential response is missing AccessKeyId/SecretAccessKey/Token.");
        } else {
            int64_t e = aws_parse_iso8601(json_get_str(j, "Expiration"));
            if (e < 0) {
                *err = xstrdup("AWS container credential response has a missing or invalid Expiration.");
            } else {
                out->access_key_id = xstrdup(ak);
                out->secret_access_key = xstrdup(sk);
                out->session_token = xstrdup(tok);
                out->expires_at = e;
                ret = 0;
            }
        }
        json_free(j);
    }
    http_resp_free(&resp);
    return ret;
}

/* ---------- IMDSv2 ---------- */

static bool read_imds_creds(const rctx_t *r, aws_creds_t *out) {
    const char *mode = env_get("AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE");
    const char *ep = env_get("AWS_EC2_METADATA_SERVICE_ENDPOINT");
    char *base = xstrdup(ep ? ep : (mode && !strcasecmp(mode, "ipv6")) ? "http://[fd00:ec2::254]/" : "http://169.254.169.254/");
    if (!str_ends(base, "/")) {
        char *b2 = xasprintf("%s/", base);
        free(base);
        base = b2;
    }
    bool ok = false;
    char *token = NULL, *role = NULL;
    http_resp_t resp;
    http_headers_t h = {0};
    char *url = xasprintf("%slatest/api/token", base);
    http_headers_add(&h, "x-aws-ec2-metadata-token-ttl-seconds", "21600");
    int rc = do_http(r, "PUT", url, &h, NULL, IMDS_TIMEOUT_MS, true, &resp);
    free(url);
    http_headers_free(&h);
    if (rc == 0 && resp.status >= 200 && resp.status < 300) token = xstrdup(resp.body ? resp.body : "");
    http_resp_free(&resp);
    if (!token) goto done;
    url = xasprintf("%slatest/meta-data/iam/security-credentials/", base);
    http_headers_add(&h, "x-aws-ec2-metadata-token", token);
    rc = do_http(r, "GET", url, &h, NULL, IMDS_TIMEOUT_MS, true, &resp);
    free(url);
    if (rc == 0 && resp.status >= 200 && resp.status < 300) role = str_trim(resp.body ? resp.body : "");
    http_resp_free(&resp);
    if (!role || !*role) goto done;
    {
        sb_t enc = SB_INIT;
        rfc3986(&enc, role, strlen(role));
        url = xasprintf("%slatest/meta-data/iam/security-credentials/%s", base, sb_cstr(&enc));
        sb_free(&enc);
    }
    rc = do_http(r, "GET", url, &h, NULL, IMDS_TIMEOUT_MS, true, &resp);
    free(url);
    if (rc == 0 && resp.status >= 200 && resp.status < 300) {
        json_t *j = json_parse_str(resp.body);
        const char *ak = json_get_str(j, "AccessKeyId"), *sk = json_get_str(j, "SecretAccessKey");
        const char *tok = json_get_str(j, "Token");
        int64_t e = aws_parse_iso8601(json_get_str(j, "Expiration"));
        if (ak && *ak && sk && *sk && tok && *tok && e >= 0) {
            out->access_key_id = xstrdup(ak);
            out->secret_access_key = xstrdup(sk);
            out->session_token = xstrdup(tok);
            out->expires_at = e;
            ok = true;
        }
        json_free(j);
    }
    http_resp_free(&resp);
done:
    http_headers_free(&h);
    free(token);
    free(role);
    free(base);
    return ok;
}

/* ---------- SSO ---------- */

static char *sha1_hex(const char *s) {
    unsigned char md[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)s, strlen(s), md);
    char *o = xmalloc(2 * SHA_DIGEST_LENGTH + 1);
    to_hex(md, SHA_DIGEST_LENGTH, o);
    return o;
}

static int strcmp_p(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* returns owned token json and *file (owned basename) or NULL */
static json_t *load_sso_cached_token(const char *start_url, const char *session, char **file) {
    char *dir = path_join3(home_dir(), ".aws", "sso/cache");
    DIR *d = opendir(dir);
    if (!d) {
        free(dir);
        return NULL;
    }
    strvec_t entries = {0};
    struct dirent *de;
    while ((de = readdir(d))) {
        if (str_ends(de->d_name, ".json")) strvec_push(&entries, de->d_name);
    }
    closedir(d);
    if (entries.n) xqsort(entries.v, entries.n, sizeof(char *), strcmp_p);
    char *hash = sha1_hex(session && *session ? session : start_url);
    char *hfile = xasprintf("%s.json", hash);
    free(hash);
    strvec_t cand = {0};
    strvec_push(&cand, hfile);
    for (size_t i = 0; i < entries.n; i++)
        if (!strvec_contains(&cand, entries.v[i])) strvec_push(&cand, entries.v[i]);
    json_t *found = NULL;
    for (size_t i = 0; i < cand.n && !found; i++) {
        if (!strvec_contains(&entries, cand.v[i])) continue;
        char *p = path_join(dir, cand.v[i]);
        size_t n;
        char *s = fs_read_limit(p, 1 << 20, &n, &(bool){false});
        free(p);
        json_t *j = s ? json_parse(s, n, NULL) : NULL;
        free(s);
        if (json_is_obj(j) && (str_eq(json_get_str(j, "startUrl"), start_url) ||
                               (session && *session && !strcmp(cand.v[i], hfile)))) {
            found = j;
            *file = xstrdup(cand.v[i]);
        } else {
            json_free(j);
        }
    }
    strvec_free(&cand);
    strvec_free(&entries);
    free(hfile);
    free(dir);
    return found;
}

static char *iso_no_ms(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return xstrdup(buf);
}

/* refreshSsoToken: returns new access token (owned) or NULL */
static char *refresh_sso_token(const rctx_t *r, json_t *tok, const char *file, const char *region) {
    const char *rt = json_get_str(tok, "refreshToken"), *cid = json_get_str(tok, "clientId");
    const char *cs = json_get_str(tok, "clientSecret");
    if (!rt || !*rt || !cid || !*cid || !cs || !*cs) return NULL;
    int64_t reg = aws_parse_iso8601(json_get_str(tok, "registrationExpiresAt"));
    if (reg >= 0 && reg <= now_ms()) return NULL;
    json_t *req = json_obj();
    json_set_str(req, "clientId", cid);
    json_set_str(req, "clientSecret", cs);
    json_set_str(req, "grantType", "refresh_token");
    json_set_str(req, "refreshToken", rt);
    char *body = json_dump(req, 0);
    json_free(req);
    char *url = xasprintf("https://oidc.%s.amazonaws.com/token", region);
    http_headers_t h = {0};
    http_headers_add(&h, "content-type", "application/json");
    http_resp_t resp;
    int rc = do_http(r, "POST", url, &h, body, 0, false, &resp);
    http_headers_free(&h);
    free(url);
    free(body);
    char *out = NULL;
    if (rc == 0 && resp.status >= 200 && resp.status < 300) {
        json_t *j = json_parse_str(resp.body);
        const char *at = json_get_str(j, "accessToken");
        if (at && *at) {
            out = xstrdup(at);
            json_set_str(tok, "accessToken", at);
            char *exp = iso_no_ms(time_add_ms(now_ms(), json_get_num(j, "expiresIn", 0) * 1000));
            json_set(tok, "expiresAt", json_str_own(exp));
            const char *nrt = json_get_str(j, "refreshToken");
            if (nrt && *nrt) json_set_str(tok, "refreshToken", nrt);
            char *target = path_join3(home_dir(), ".aws/sso/cache", file);
            char *data = json_dump(tok, 0);
            if (fs_write_atomic(target, data, strlen(data), 0600) != 0)
                LOGD("aws-credentials: failed to persist refreshed SSO token %s", file);
            free(data);
            free(target);
        }
        json_free(j);
    } else {
        LOGD("aws-credentials: SSO token refresh failed (status %d)", resp.status);
    }
    http_resp_free(&resp);
    return out;
}

/* 0 ok, 1 not applicable, -1 error */
static int read_sso_creds(const rctx_t *r, const json_t *prof, const json_t *cfg, aws_creds_t *out, char **err) {
    const char *start = json_get_str(prof, "sso_start_url"), *region = json_get_str(prof, "sso_region");
    const char *sess = json_get_str(prof, "sso_session");
    if (sess && cfg) {
        char *k = xasprintf("sso-session:%s", sess);
        json_t *s = json_get_obj(cfg, k);
        free(k);
        if (s) {
            if (!start || !*start) start = json_get_str(s, "sso_start_url");
            if (!region || !*region) region = json_get_str(s, "sso_region");
        }
    }
    if (!start || !*start || !region || !*region) return 1;
    char *file = NULL;
    json_t *tok = load_sso_cached_token(start, sess, &file);
    const char *at = json_get_str(tok, "accessToken");
    if (!at || !*at) {
        *err = xasprintf("AWS SSO token for %s not found in ~/.aws/sso/cache. Run 'aws sso login' first.", start);
        json_free(tok);
        free(file);
        return -1;
    }
    char *access = xstrdup(at);
    const char *exps = json_get_str(tok, "expiresAt");
    int64_t exp = exps ? aws_parse_iso8601(exps) : -1;
    if (exp >= 0 && exp - REFRESH_SKEW_MS <= now_ms()) {
        char *nt = refresh_sso_token(r, tok, file, region);
        if (nt) {
            free(access);
            access = nt;
        } else if (exp <= now_ms()) {
            *err = xasprintf("AWS SSO token for %s has expired. Run 'aws sso login' to refresh.", start);
            free(access);
            json_free(tok);
            free(file);
            return -1;
        }
    }
    json_free(tok);
    free(file);
    sb_t url = SB_INIT;
    sb_appendf(&url, "https://portal.sso.%s.amazonaws.com/federation/credentials?account_id=", region);
    const char *acct = json_get_str(prof, "sso_account_id"), *role = json_get_str(prof, "sso_role_name");
    rfc3986(&url, acct, strlen(acct));
    sb_append(&url, "&role_name=");
    rfc3986(&url, role, strlen(role));
    http_headers_t h = {0};
    http_headers_add(&h, "x-amz-sso_bearer_token", access);
    free(access);
    http_resp_t resp;
    int rc = do_http(r, "GET", sb_cstr(&url), &h, NULL, 0, false, &resp);
    http_headers_free(&h);
    sb_free(&url);
    int ret = -1;
    if (rc != 0) {
        *err = xasprintf("AWS SSO GetRoleCredentials failed: %s", resp.error ? resp.error : "network error");
    } else if (resp.status < 200 || resp.status >= 300) {
        *err = xasprintf("AWS SSO GetRoleCredentials failed: %d %.200s", resp.status, resp.body ? resp.body : "");
    } else {
        json_t *j = json_parse_str(resp.body);
        json_t *rcj = json_get_obj(j, "roleCredentials");
        if (!rcj) {
            *err = xstrdup("AWS SSO GetRoleCredentials: missing roleCredentials in response");
        } else {
            const char *ak = json_get_str(rcj, "accessKeyId"), *sk = json_get_str(rcj, "secretAccessKey");
            const char *st = json_get_str(rcj, "sessionToken");
            out->access_key_id = xstrdup(ak ? ak : "");
            out->secret_access_key = xstrdup(sk ? sk : "");
            out->session_token = st ? xstrdup(st) : NULL;
            out->expires_at = json_get_int(rcj, "expiration", 0);
            ret = 0;
        }
        json_free(j);
    }
    http_resp_free(&resp);
    return ret;
}

/* ---------- credential_process ---------- */

static int read_credential_process(const rctx_t *r, const char *profile, const char *command, aws_creds_t *out,
                                   char **err) {
    char *terr = NULL;
    strvec_t argv = aws_tokenize_command(command, &terr);
    if (terr) {
        *err = terr;
        return -1;
    }
    if (!argv.n) {
        *err = xasprintf("AWS credential_process for profile '%s' is empty.", profile);
        strvec_free(&argv);
        return -1;
    }
    const char **av = xcalloc(argv.n + 1, sizeof(char *));
    for (size_t i = 0; i < argv.n; i++) av[i] = argv.v[i];
    proc_opts_t po = {.timeout_ms = remaining(r, 0), .cancel = r->cancel, .max_output = 1 << 20};
    proc_result_t res;
    memset(&res, 0, sizeof res);
    int prc = proc_run(av, &po, &res);
    free(av);
    strvec_free(&argv);
    int ret = -1;
    if (prc != 0 || res.spawn_errno || res.exit_code != 0) {
        char *e = str_trim(res.err ? res.err : ""), *o = str_trim(res.out ? res.out : "");
        const char *tail = *e ? e : *o ? o : "(no output)";
        size_t tl = strlen(tail);
        *err = xasprintf("AWS credential_process for profile '%s' exited %d: %s", profile,
                         res.spawn_errno ? 127 : res.exit_code, tail + (tl > 512 ? tl - 512 : 0));
        free(e);
        free(o);
    } else {
        char *perr = NULL;
        json_t *j = json_parse(res.out ? res.out : "", res.out_len, &perr);
        if (!j) {
            *err = xasprintf("AWS credential_process for profile '%s' did not emit valid JSON: %s", profile,
                             perr ? perr : "parse error");
        } else if (json_get_int(j, "Version", -1) != 1 || !json_is_num(json_get(j, "Version"))) {
            json_t *v = json_get(j, "Version");
            char *vs = v ? json_dump(v, 0) : xstrdup("<missing>");
            *err = xasprintf("AWS credential_process for profile '%s' returned unsupported Version %s; expected 1.",
                             profile, vs);
            free(vs);
        } else {
            const char *ak = json_get_str(j, "AccessKeyId"), *sk = json_get_str(j, "SecretAccessKey");
            if (!ak || !*ak || !sk || !*sk) {
                *err = xasprintf("AWS credential_process for profile '%s' returned envelope without "
                                 "AccessKeyId/SecretAccessKey.",
                                 profile);
            } else {
                const char *st = json_get_str(j, "SessionToken"), *exp = json_get_str(j, "Expiration");
                out->access_key_id = xstrdup(ak);
                out->secret_access_key = xstrdup(sk);
                out->session_token = st && *st ? xstrdup(st) : NULL;
                out->expires_at = 0;
                if ((st && *st) || (exp && *exp)) {
                    /* dynamicCredentialExpiration: missing/malformed disables caching */
                    int64_t e = exp && *exp ? aws_parse_iso8601(exp) : -1;
                    out->expires_at = e >= 0 ? e : now_ms();
                }
                ret = 0;
            }
        }
        free(perr);
        json_free(j);
    }
    proc_result_free(&res);
    return ret;
}

/* ---------- profile chain ---------- */

typedef struct {
    json_t *cred, *cfg;
    const char *region;
    const rctx_t *r;
} pctx_t;

static int resolve_profile_chain(const char *profile, const pctx_t *c, strvec_t *seen, aws_creds_t *out, char **err);

static int credential_source(const pctx_t *c, const char *src, aws_creds_t *out, char **err) {
    if (!strcmp(src, "Environment")) return read_env_creds(out) ? 0 : 1;
    if (!strcmp(src, "Ec2InstanceMetadata")) {
        if (env_is_true("AWS_EC2_METADATA_DISABLED")) return 1;
        return read_imds_creds(c->r, out) ? 0 : 1;
    }
    if (!strcmp(src, "EcsContainer")) return read_container_creds(c->r, out, err);
    *err = xasprintf("Unsupported AWS credential_source '%s'.", src);
    return -1;
}

static int assume_role_from_profile(const char *profile, const json_t *m, const pctx_t *c, strvec_t *seen,
                                    aws_creds_t *out, char **err) {
    const char *arn = json_get_str(m, "role_arn");
    const char *wif = json_get_str(m, "web_identity_token_file");
    if (wif) return assume_role_web_identity(c->r, arn, wif, json_get_str(m, "role_session_name"), c->region, out, err);
    if (json_get_str(m, "mfa_serial")) {
        *err = xasprintf("AWS profile '%s' requires MFA (mfa_serial), which is not supported for non-interactive "
                         "credential resolution.",
                         profile);
        return -1;
    }
    aws_creds_t base = {0};
    const char *sp = json_get_str(m, "source_profile"), *cs = json_get_str(m, "credential_source");
    if (sp) {
        int rc = resolve_profile_chain(sp, c, seen, &base, err);
        if (rc < 0) return -1;
        if (rc > 0) {
            *err = xasprintf("AWS profile '%s' references source_profile '%s', which has no usable credentials.",
                             profile, sp);
            return -1;
        }
    } else if (cs) {
        int rc = credential_source(c, cs, &base, err);
        if (rc < 0) return -1;
        if (rc > 0) {
            *err = xasprintf("AWS profile '%s' credential_source '%s' produced no credentials.", profile, cs);
            return -1;
        }
    } else {
        *err = xasprintf("AWS profile '%s' sets role_arn without source_profile, credential_source, or "
                         "web_identity_token_file.",
                         profile);
        return -1;
    }
    int rc = sts_assume_role(c->r, &base, arn, c->region, m, out, err);
    aws_creds_free(&base);
    return rc;
}

/* 0 ok, 1 nothing usable, -1 error */
static int resolve_profile_chain(const char *profile, const pctx_t *c, strvec_t *seen, aws_creds_t *out, char **err) {
    if (strvec_contains(seen, profile) || seen->n > MAX_ROLE_CHAIN) {
        *err = xasprintf("AWS profile role chain contains a cycle at '%s'.", profile);
        return -1;
    }
    strvec_push(seen, profile);
    json_t *m = merged_profile(c->cred, c->cfg, profile);
    int rc = 1;
    if (json_len(m) == 0) rc = 1;
    else if (json_get_str(m, "role_arn")) rc = assume_role_from_profile(profile, m, c, seen, out, err);
    else if (json_get_str(m, "aws_access_key_id") && json_get_str(m, "aws_secret_access_key")) {
        out->access_key_id = xstrdup(json_get_str(m, "aws_access_key_id"));
        out->secret_access_key = xstrdup(json_get_str(m, "aws_secret_access_key"));
        const char *st = json_get_str(m, "aws_session_token");
        if (st) {
            out->session_token = xstrdup(st);
            out->expires_at = now_ms() + FILE_SESSION_CREDS_TTL_MS;
        }
        rc = 0;
    } else if (json_get_str(m, "sso_account_id") && json_get_str(m, "sso_role_name")) {
        rc = read_sso_creds(c->r, m, c->cfg, out, err);
    } else if (json_get_str(m, "credential_process")) {
        rc = read_credential_process(c->r, profile, json_get_str(m, "credential_process"), out, err);
    }
    json_free(m);
    return rc;
}

static int resolve_fresh(const char *profile, const char *region, bool load_cfg, const rctx_t *r, aws_creds_t *out,
                         char **err) {
    if (read_env_creds(out)) return 0;
    const char *wtf = env_get("AWS_WEB_IDENTITY_TOKEN_FILE"), *arn = env_get("AWS_ROLE_ARN");
    if (wtf && arn) return assume_role_web_identity(r, arn, wtf, env_get("AWS_ROLE_SESSION_NAME"), region, out, err);
    char *cp = aws_credentials_path(), *fp = aws_config_path();
    pctx_t pc = {.cred = read_ini(cp), .cfg = load_cfg ? read_ini(fp) : NULL, .region = region, .r = r};
    free(cp);
    free(fp);
    strvec_t seen = {0};
    int rc = resolve_profile_chain(profile, &pc, &seen, out, err);
    strvec_free(&seen);
    json_free(pc.cred);
    json_free(pc.cfg);
    if (rc <= 0) return rc;
    rc = read_container_creds(r, out, err);
    if (rc <= 0) return rc;
    if (!env_is_true("AWS_EC2_METADATA_DISABLED") && read_imds_creds(r, out)) return 0;
    *err = xstrdup("Unable to resolve AWS credentials. Configure static environment keys, web identity, an AWS "
                   "profile, ECS credentials, or an EC2 instance role.");
    return -1;
}

/* ---------- cache ---------- */

typedef struct {
    char *key;
    aws_creds_t creds;
} cache_ent_t;

static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_resolve_mu = PTHREAD_MUTEX_INITIALIZER;
static VEC(cache_ent_t) g_cache;

static char *cache_key(const char *profile, const char *region) {
    const char *p = aws_profile_name(profile);
    char *r = aws_resolve_region(region, profile);
    char *k = xasprintf("%s\x1f%s\x1f%s", p, r, aws_should_load_shared_config(profile) ? "config" : "credentials");
    free(r);
    return k;
}

static bool cache_lookup(const char *key, aws_creds_t *out) {
    bool hit = false;
    pthread_mutex_lock(&g_cache_mu);
    for (size_t i = 0; i < g_cache.n; i++) {
        if (strcmp(g_cache.v[i].key, key)) continue;
        int64_t e = g_cache.v[i].creds.expires_at;
        if (e == 0 || e - REFRESH_SKEW_MS > now_ms()) {
            aws_creds_copy(out, &g_cache.v[i].creds);
            hit = true;
        }
        break;
    }
    pthread_mutex_unlock(&g_cache_mu);
    return hit;
}

static void cache_drop_locked(const char *key) {
    for (size_t i = 0; i < g_cache.n; i++) {
        if (strcmp(g_cache.v[i].key, key)) continue;
        free(g_cache.v[i].key);
        aws_creds_free(&g_cache.v[i].creds);
        g_cache.v[i] = g_cache.v[g_cache.n - 1];
        g_cache.n--;
        return;
    }
}

int aws_resolve_credentials(const char *profile, const char *region, volatile sig_atomic_t *cancel, aws_creds_t *out,
                            char **err) {
    memset(out, 0, sizeof(*out));
    *err = NULL;
    char *key = cache_key(profile, region);
    if (cache_lookup(key, out)) {
        free(key);
        return 0;
    }
    /* single-flight: concurrent cold callers wait for one resolution, then hit the cache */
    pthread_mutex_lock(&g_resolve_mu);
    if (cache_lookup(key, out)) {
        pthread_mutex_unlock(&g_resolve_mu);
        free(key);
        return 0;
    }
    rctx_t r = {.cancel = cancel, .deadline = mono_ms() + SHARED_RESOLVE_TIMEOUT_MS};
    char *reg = aws_resolve_region(region, profile);
    int rc = resolve_fresh(aws_profile_name(profile), reg, aws_should_load_shared_config(profile), &r, out, err);
    free(reg);
    if (rc == 0) {
        pthread_mutex_lock(&g_cache_mu);
        cache_drop_locked(key);
        cache_ent_t e = {xstrdup(key), {0}};
        aws_creds_copy(&e.creds, out);
        vec_push(&g_cache, e);
        pthread_mutex_unlock(&g_cache_mu);
    } else {
        aws_creds_free(out);
        if (!*err) *err = xstrdup("Unable to resolve AWS credentials.");
    }
    pthread_mutex_unlock(&g_resolve_mu);
    free(key);
    return rc == 0 ? 0 : -1;
}

void aws_invalidate_credentials(const char *profile, const char *region) {
    char *key = cache_key(profile, region);
    pthread_mutex_lock(&g_cache_mu);
    cache_drop_locked(key);
    pthread_mutex_unlock(&g_cache_mu);
    free(key);
}

void aws_clear_credential_cache(void) {
    pthread_mutex_lock(&g_cache_mu);
    for (size_t i = 0; i < g_cache.n; i++) {
        free(g_cache.v[i].key);
        aws_creds_free(&g_cache.v[i].creds);
    }
    vec_free(&g_cache);
    pthread_mutex_unlock(&g_cache_mu);
}
