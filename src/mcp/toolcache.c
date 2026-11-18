/* toolcache.c - MCP tool cache (port of omp mcp/tool-cache.ts).
 *
 * Tool definitions per server, keyed by a hash of the server config, with a version and a 30-day TTL. omp keeps
 * them in agent.db's cache table (`mcp_tools:<server>`); omc keeps the same entries in
 * <agentDir>/mcp-tool-cache.json: {"<server>": {"version": 1, "configHash": "<sha256 hex>", "tools": [...],
 * "expiresAt": <unix seconds>}}. */
#include "mcp.h"

#include "core/fs.h"

#include <openssl/evp.h>
#include <pthread.h>

#define CACHE_VERSION 1
#define CACHE_TTL_SEC (30LL * 24 * 60 * 60)
#define CACHE_MAX_BYTES (16u << 20)
#define STABLE_MAX_DEPTH 64

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static char *cache_path(void) { return omc_agent_path("mcp-tool-cache.json"); }

static int key_cmp(const void *x, const void *y) { return strcmp(*(const char *const *)x, *(const char *const *)y); }

/* stableStringifyJson: JSON.stringify with object keys sorted at every level. Top-level keys starting with "_"
 * are omc annotations (_source, _provider, _noOAuth) that omp keeps outside the config, so they are left out. */
static void stable_sb(sb_t *b, const json_t *j, int depth, bool top) {
    if (depth > STABLE_MAX_DEPTH) {
        sb_append(b, "null");
        return;
    }
    if (json_is_arr(j)) {
        sb_putc(b, '[');
        json_t *v;
        json_arr_foreach(j, i, v) {
            if (i) sb_putc(b, ',');
            stable_sb(b, v, depth + 1, false);
        }
        sb_putc(b, ']');
        return;
    }
    if (!json_is_obj(j)) {
        json_dump_js_sb(b, j);
        return;
    }
    size_t n = j->u.obj.n, k = 0;
    const char **keys = xcalloc(n ? n : 1, sizeof(char *));
    for (size_t i = 0; i < n; i++) {
        const char *key = j->u.obj.keys[i];
        if (top && key[0] == '_') continue;
        bool dup = false; /* JSON.parse keeps the last duplicate; json_get returns it */
        for (size_t q = 0; q < k && !dup; q++) dup = !strcmp(keys[q], key);
        if (!dup) keys[k++] = key;
    }
    xqsort(keys, k, sizeof(char *), key_cmp);
    sb_putc(b, '{');
    for (size_t i = 0; i < k; i++) {
        if (i) sb_putc(b, ',');
        json_js_quote_sb(b, keys[i], strlen(keys[i]));
        sb_putc(b, ':');
        stable_sb(b, json_get(j, keys[i]), depth + 1, false);
    }
    sb_putc(b, '}');
    free(keys);
}

char *mcp_tool_cache_hash(const json_t *cfg) {
    sb_t b = SB_INIT;
    stable_sb(&b, cfg, 0, true);
    unsigned char md[32];
    unsigned int mdl = 0;
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    EVP_DigestUpdate(c, b.s ? b.s : "", b.len);
    EVP_DigestFinal_ex(c, md, &mdl);
    EVP_MD_CTX_free(c);
    sb_free(&b);
    char *hex = xmalloc(65);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", md[i]);
    return hex;
}

/* the cache file (re-read on every access: other omc processes share it); {} when missing or unreadable */
static json_t *load_locked(void) {
    char *p = cache_path();
    size_t n = 0;
    char *txt = fs_read_limit(p, CACHE_MAX_BYTES, &n, NULL);
    free(p);
    json_t *j = txt ? json_parse(txt, n, NULL) : NULL;
    free(txt);
    if (!json_is_obj(j)) {
        json_free(j);
        j = json_obj();
    }
    return j;
}

json_t *mcp_tool_cache_get(const char *server, const json_t *cfg) {
    if (!server || !cfg) return NULL;
    pthread_mutex_lock(&g_mu);
    json_t *all = load_locked();
    pthread_mutex_unlock(&g_mu);
    json_t *e = json_get_obj(all, server), *out = NULL;
    /* getCache: only unexpired rows; then version, configHash and tools must check out */
    if (e && json_get_int(e, "expiresAt", 0) > now_ms() / 1000 && json_get_int(e, "version", 0) == CACHE_VERSION &&
        json_is_str(json_get(e, "configHash")) && json_get_arr(e, "tools")) {
        char *h = mcp_tool_cache_hash(cfg);
        if (str_eq(h, json_get_str(e, "configHash"))) out = json_clone(json_get_arr(e, "tools"));
        free(h);
    }
    json_free(all);
    return out;
}

void mcp_tool_cache_set(const char *server, const json_t *cfg, const json_t *tools) {
    if (!server || !cfg || !json_is_arr(tools)) return;
    char *h = mcp_tool_cache_hash(cfg);
    int64_t now = now_ms() / 1000;
    json_t *e = json_obj();
    json_set_int(e, "version", CACHE_VERSION);
    json_set_str(e, "configHash", h);
    json_set(e, "tools", json_clone(tools));
    json_set_int(e, "expiresAt", now + CACHE_TTL_SEC);
    free(h);
    pthread_mutex_lock(&g_mu);
    json_t *all = load_locked();
    /* cleanExpiredCache: drop expired entries while rewriting */
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(all, i, k, v) if (strcmp(k, server) && json_get_int(v, "expiresAt", 0) > now) json_set(out, k, json_clone(v));
    json_set(out, server, e);
    char *dump = json_dump(out, 0);
    char *p = cache_path();
    char *dir = path_dirname(p);
    fs_mkdir_p(dir, 0700);
    if (fs_write_atomic(p, dump, strlen(dump), 0600) != 0) LOGW("MCP tool cache write failed: %s", p);
    free(dir);
    free(p);
    free(dump);
    json_free(out);
    json_free(all);
    pthread_mutex_unlock(&g_mu);
}
