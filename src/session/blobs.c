/* blobs.c - content-addressed image blob store for session files (port of omp session/blob-store.ts +
 * session-persistence.ts externalization and session-loader.ts resolution).
 *
 * Image payloads in persisted entries (`content` image blocks and `images[]` entries, base64 of at least
 * 1024 chars) are written once to `<agentDir>/blobs/<sha256-hex>` (hash of the raw bytes) with a typed
 * sidecar link `<hash>.<ext>`, and the session line carries `blob:sha256:<hex>` instead. Loading resolves
 * the references back to base64; a missing blob keeps its reference (the provider boundary then degrades
 * it to an "[image omitted: ...]" note). Only canonical 64-hex hashes are accepted, so a reference can
 * never escape the blob directory. */
#include "session/blobs.h"
#include "core/fs.h"
#include <errno.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOB_PREFIX "blob:sha256:"
#define BLOB_EXTERNALIZE_THRESHOLD 1024

static bool is_hash(const char *h) {
    for (int i = 0; i < 64; i++)
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f'))) return false;
    return h[64] == 0;
}

static const char *mime_ext(const char *mime) {
    if (!mime) return NULL;
    if (!strcasecmp(mime, "image/png")) return "png";
    if (!strcasecmp(mime, "image/jpeg") || !strcasecmp(mime, "image/jpg")) return "jpg";
    if (!strcasecmp(mime, "image/gif")) return "gif";
    if (!strcasecmp(mime, "image/webp")) return "webp";
    if (!strcasecmp(mime, "image/svg+xml")) return "svg";
    return NULL;
}

static char *blobs_dir(void) {
    const char *env = getenv("OMC_BLOBS_DIR");
    return env && *env ? xstrdup(env) : omc_agent_path("blobs");
}

/* memo of payloads already written: (len, fnv) -> hash */
#define MEMO_SLOTS 256
typedef struct {
    size_t len;
    uint64_t fnv;
    char hash[65];
} memo_t;
static memo_t g_memo[MEMO_SLOTS];
static pthread_mutex_t g_memo_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t fnv(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
    return h;
}

/* write base64 `b64` as a blob; returns the owned "blob:sha256:<hex>" ref or NULL */
static char *put_blob(const char *b64, const char *mime) {
    size_t blen = strlen(b64);
    uint64_t f = fnv(b64, blen);
    memo_t *m = &g_memo[f % MEMO_SLOTS];
    pthread_mutex_lock(&g_memo_mu);
    if (m->len == blen && m->fnv == f && m->hash[0]) {
        char *r = xasprintf(BLOB_PREFIX "%s", m->hash);
        pthread_mutex_unlock(&g_memo_mu);
        return r;
    }
    pthread_mutex_unlock(&g_memo_mu);
    size_t n = 0;
    unsigned char *raw = base64_decode(b64, blen, &n);
    if (!raw || !n) {
        free(raw);
        return NULL;
    }
    unsigned char dig[SHA256_DIGEST_LENGTH];
    SHA256(raw, n, dig);
    char hash[65];
    for (int i = 0; i < 32; i++) snprintf(hash + i * 2, 3, "%02x", dig[i]);
    char *dir = blobs_dir();
    fs_mkdir_p(dir, 0700);
    char *path = path_join(dir, hash);
    struct stat st;
    int rc = 0;
    if (stat(path, &st) != 0 || (size_t)st.st_size != n) rc = fs_write_atomic(path, raw, n, 0644);
    const char *ext = mime_ext(mime);
    if (rc == 0 && ext) {
        char *side = xasprintf("%s.%s", path, ext);
        if (stat(side, &st) != 0 && link(path, side) != 0) fs_write_atomic(side, raw, n, 0644);
        free(side);
    }
    free(path), free(dir), free(raw);
    if (rc != 0) return NULL;
    pthread_mutex_lock(&g_memo_mu);
    m->len = blen, m->fnv = f;
    memcpy(m->hash, hash, 65);
    pthread_mutex_unlock(&g_memo_mu);
    return xasprintf(BLOB_PREFIX "%s", hash);
}

static bool image_payload(const json_t *v) {
    if (!json_is_obj(v) || !json_get_str(v, "data")) return false;
    if (str_eq(json_get_str(v, "type"), "image")) return true;
    const char *mt = json_get_str(v, "mimeType");
    return mt && !strncasecmp(mt, "image/", 6);
}

/* isExternalizableImagePosition: a `content` image block or an `images[]` entry */
static void walk(json_t *v, const char *key, bool store) {
    if (json_is_arr(v)) {
        json_t *x;
        json_arr_foreach(v, i, x) {
            bool pos = (str_eq(key, "content") && json_is_obj(x) && str_eq(json_get_str(x, "type"), "image")) ||
                       (str_eq(key, "images") && image_payload(x));
            if (pos) {
                const char *d = json_get_str(x, "data");
                if (store) {
                    if (!str_starts(d, BLOB_PREFIX) && strlen(d) >= BLOB_EXTERNALIZE_THRESHOLD) {
                        char *ref = put_blob(d, json_get_str(x, "mimeType"));
                        if (ref) json_set(x, "data", json_str_own(ref));
                    }
                } else if (str_starts(d, BLOB_PREFIX) && is_hash(d + strlen(BLOB_PREFIX))) {
                    char *dir = blobs_dir();
                    char *path = path_join(dir, d + strlen(BLOB_PREFIX));
                    size_t n = 0;
                    char *raw = fs_read_limit(path, 64u << 20, &n, NULL);
                    if (raw) json_set(x, "data", json_str_own(base64_encode(raw, n)));
                    else LOGW("blob not found for persisted image: %s", path);
                    free(raw), free(path), free(dir);
                }
                continue;
            }
            walk(x, key, store);
        }
        return;
    }
    if (!json_is_obj(v)) return;
    const char *k;
    json_t *x;
    json_obj_foreach(v, i, k, x) walk(x, k, store);
}

json_t *session_blobs_externalize(const json_t *entry) {
    json_t *c = json_clone(entry);
    walk(c, NULL, true);
    return c;
}

void session_blobs_resolve(json_t *entry) { walk(entry, NULL, false); }

char *session_blob_put_display(const char *b64, const char *mime) {
    char *ref = b64 ? put_blob(b64, mime) : NULL;
    if (!ref) return NULL;
    char *dir = blobs_dir();
    char *path = path_join(dir, ref + strlen(BLOB_PREFIX));
    const char *ext = mime_ext(mime);
    char *r = ext ? xasprintf("%s.%s", path, ext) : xstrdup(path);
    free(path), free(dir), free(ref);
    return r;
}
