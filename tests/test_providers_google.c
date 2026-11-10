/*
 * test_providers_google.c - offline tests for google-vertex ADC and the Cloud Code Assist api (google-gemini-cli /
 * google-antigravity): RS256 JWT signing (verified with libcrypto against a key generated here), ADC token
 * exchange/caching/impersonation, CCA envelopes, SSE parsing, leaked-thinking healing, the CCA schema normalizer
 * and project onboarding. Network flows run against an in-process loopback HTTP server; no real credentials.
 */
#include "ai/ai.h"
#include "ai/google_auth.h"
#include "ai/google_cca.h"
#include "ai/provider_google.h"
#include "ai/think_heal.h"
#include "core/fs.h"

#include <math.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

char *ai_env_api_key(const char *provider) {
    (void)provider;
    return NULL;
}

static int g_fail, g_pass;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (c) g_pass++;                                                                                               \
        else {                                                                                                         \
            g_fail++;                                                                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                      \
        }                                                                                                              \
    } while (0)
#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        const char *a_ = (a), *b_ = (b);                                                                               \
        if (str_eq(a_, b_)) g_pass++;                                                                                  \
        else {                                                                                                         \
            g_fail++;                                                                                                  \
            fprintf(stderr, "%s:%d: '%s' != '%s'\n", __FILE__, __LINE__, a_ ? a_ : "(null)", b_ ? b_ : "(null)");      \
        }                                                                                                              \
    } while (0)
#define CHECK_INT(a, b)                                                                                                \
    do {                                                                                                               \
        long long a_ = (long long)(a), b_ = (long long)(b);                                                            \
        if (a_ == b_) g_pass++;                                                                                        \
        else {                                                                                                         \
            g_fail++;                                                                                                  \
            fprintf(stderr, "%s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_);                      \
        }                                                                                                              \
    } while (0)

/* JSON with single quotes for readability */
static json_t *JQ(const char *s) {
    char *t = xstrdup(s);
    for (char *p = t; *p; p++)
        if (*p == '\'') *p = '"';
    char *err = NULL;
    json_t *j = json_parse(t, strlen(t), &err);
    if (!j) {
        fprintf(stderr, "bad test json: %s\n%s\n", err ? err : "?", t);
        exit(2);
    }
    free(err);
    free(t);
    return j;
}

static const char *P(const json_t *j, const char *path) { return json_sv(json_path(j, path)); }
static int64_t PI(const json_t *j, const char *path) {
    json_t *v = json_path(j, path);
    return json_is_num(v) ? (int64_t)v->u.num.d : -999999;
}

static ai_model_t mk_model(json_t *raw) {
    ai_model_t m = {0};
    m.raw = raw;
    m.id = json_get_str(raw, "id");
    m.name = m.id;
    m.api = json_get_str(raw, "api");
    m.provider = json_get_str(raw, "provider");
    m.base_url = json_get_str(raw, "baseUrl");
    if (!m.base_url) m.base_url = "";
    m.reasoning = json_get_bool(raw, "reasoning", false);
    json_t *in;
    json_arr_foreach(json_get(raw, "input"), i, in) {
        if (str_eq(json_sv(in), "image")) m.input_image = true;
    }
    m.context_window = json_get_int(raw, "contextWindow", 128000);
    m.max_tokens = json_get_int(raw, "maxTokens", 32000);
    m.compat = json_get_obj(raw, "compat");
    m.thinking = json_get_obj(raw, "thinking");
    return m;
}

/* ====================================================================== loopback HTTP server */

typedef struct {
    int status;
    const char *ctype;
    char *body; /* owned */
} reply_t;

typedef reply_t (*route_fn)(const char *method, const char *path, const http_headers_t *h, const char *body);

static int g_port;
static route_fn g_route;
static int g_hits;
static pthread_mutex_t g_srv_mu = PTHREAD_MUTEX_INITIALIZER;

static void *server_main(void *arg) {
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) continue;
        sb_t in = SB_INIT;
        char buf[8192];
        size_t hdr_end = 0, need = 0;
        for (;;) {
            ssize_t n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            sb_appendn(&in, buf, (size_t)n);
            if (!hdr_end) {
                char *e = strstr(in.s, "\r\n\r\n");
                if (e) {
                    hdr_end = (size_t)(e - in.s) + 4;
                    const char *cl = str_istr(in.s, "content-length:");
                    need = cl && cl < e ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
                }
            }
            if (hdr_end && in.len >= hdr_end + need) break;
        }
        if (!hdr_end) {
            sb_free(&in);
            close(c);
            continue;
        }
        char method[16] = {0}, path[2048] = {0};
        sscanf(in.s, "%15s %2047s", method, path);
        http_headers_t h = {0};
        char *line = strstr(in.s, "\r\n") + 2;
        while (line < in.s + hdr_end - 2) {
            char *eol = strstr(line, "\r\n");
            char *colon = memchr(line, ':', (size_t)(eol - line));
            if (colon) {
                char *k = xstrndup(line, (size_t)(colon - line));
                const char *v = colon + 1;
                while (*v == ' ') v++;
                char *val = xstrndup(v, (size_t)(eol - v));
                http_headers_add(&h, k, val);
                free(k);
                free(val);
            }
            line = eol + 2;
        }
        char *body = xstrndup(in.s + hdr_end, need);
        pthread_mutex_lock(&g_srv_mu);
        g_hits++;
        reply_t r = g_route(method, path, &h, body);
        pthread_mutex_unlock(&g_srv_mu);
        const char *rb = r.body ? r.body : "";
        char *head = xasprintf("HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", r.status,
                               r.ctype ? r.ctype : "application/json", strlen(rb));
        if (send(c, head, strlen(head), MSG_NOSIGNAL) < 0 || send(c, rb, strlen(rb), MSG_NOSIGNAL) < 0) {}
        free(head);
        free(r.body);
        free(body);
        http_headers_free(&h);
        sb_free(&in);
        close(c);
    }
    return NULL;
}

static void start_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 16)) {
        perror("bind");
        exit(2);
    }
    socklen_t al = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    pthread_t t;
    pthread_create(&t, NULL, server_main, (void *)(intptr_t)fd);
    pthread_detach(t);
}

static reply_t R(int status, const char *ctype, const char *body) {
    reply_t r = {status, ctype, body ? xstrdup(body) : NULL};
    return r;
}

static char *form_get(const char *body, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = body; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            char *raw = xstrndup(v, strcspn(v, "&"));
            char *d = url_decode(raw);
            free(raw);
            return d;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

/* ====================================================================== JWT */

static EVP_PKEY *g_key;
static char *g_key_pem; /* PKCS#8 */

static void make_key(void) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY_keygen(ctx, &g_key);
    EVP_PKEY_CTX_free(ctx);
    BIO *b = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(b, g_key, NULL, NULL, 0, NULL, NULL);
    char *data;
    long n = BIO_get_mem_data(b, &data);
    g_key_pem = xstrndup(data, (size_t)n);
    BIO_free(b);
}

static unsigned char *b64url_decode(const char *s, size_t len, size_t *out) {
    char *t = xmalloc(len + 4);
    size_t n = 0;
    for (size_t i = 0; i < len; i++) t[n++] = s[i] == '-' ? '+' : s[i] == '_' ? '/' : s[i];
    while (n % 4) t[n++] = '=';
    unsigned char *d = base64_decode(t, n, out);
    free(t);
    return d;
}

/* verify an RS256 JWT against g_key; returns the decoded claims (owned) or NULL */
static json_t *jwt_verify(const char *jwt, json_t **header_out) {
    const char *d1 = strchr(jwt, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d2) return NULL;
    size_t sl;
    unsigned char *sig = b64url_decode(d2 + 1, strlen(d2 + 1), &sl);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, g_key) == 1 &&
              EVP_DigestVerifyUpdate(md, jwt, (size_t)(d2 - jwt)) == 1 && EVP_DigestVerifyFinal(md, sig, sl) == 1;
    EVP_MD_CTX_free(md);
    free(sig);
    if (!ok) return NULL;
    size_t n;
    unsigned char *h = b64url_decode(jwt, (size_t)(d1 - jwt), &n);
    if (header_out) *header_out = json_parse((char *)h, n, NULL);
    free(h);
    unsigned char *c = b64url_decode(d1 + 1, (size_t)(d2 - d1 - 1), &n);
    json_t *claims = json_parse((char *)c, n, NULL);
    free(c);
    return claims;
}

static void test_jwt(void) {
    json_t *claims = JQ("{'iss':'sa@proj.iam.gserviceaccount.com','scope':'s','aud':'a','exp':2,'iat':1}");
    char *err = NULL;
    char *jwt = google_jwt_rs256(claims, g_key_pem, "kid-1", &err);
    CHECK(jwt != NULL);
    json_t *hdr = NULL;
    json_t *got = jwt ? jwt_verify(jwt, &hdr) : NULL;
    CHECK(got != NULL);
    CHECK(json_equal(got, claims));
    CHECK_STR(json_get_str(hdr, "alg"), "RS256");
    CHECK_STR(json_get_str(hdr, "typ"), "JWT");
    CHECK_STR(json_get_str(hdr, "kid"), "kid-1");
    CHECK(strchr(jwt, '=') == NULL && strchr(jwt, '+') == NULL);
    /* tampering breaks the signature */
    char *bad = xstrdup(jwt);
    bad[strchr(bad, '.') - bad + 3] ^= 1;
    json_t *nope = jwt_verify(bad, NULL);
    CHECK(nope == NULL);
    free(bad);
    json_free(got);
    json_free(hdr);
    free(jwt);
    /* a non-RSA / garbage key is rejected */
    jwt = google_jwt_rs256(claims, "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n", NULL, &err);
    CHECK(jwt == NULL && err != NULL);
    free(err);
    json_free(claims);
}

/* ====================================================================== ADC */

static reply_t adc_route(const char *method, const char *path, const http_headers_t *h, const char *body) {
    const char *auth = http_headers_get(h, "Authorization");
    if (!strcmp(path, "/token")) {
        char *gt = form_get(body, "grant_type");
        reply_t r = R(400, NULL, "{\"error\":\"invalid_grant\"}");
        if (str_eq(gt, "urn:ietf:params:oauth:grant-type:jwt-bearer")) {
            char *as = form_get(body, "assertion");
            json_t *c = as ? jwt_verify(as, NULL) : NULL;
            char *aud = xasprintf("http://127.0.0.1:%d/token", g_port);
            if (c && str_eq(json_get_str(c, "iss"), "sa@proj.iam.gserviceaccount.com") && str_eq(json_get_str(c, "aud"), aud) &&
                str_eq(json_get_str(c, "scope"), GOOGLE_CLOUD_PLATFORM_SCOPE) && json_get_int(c, "exp", 0) - json_get_int(c, "iat", 0) == 3600) {
                free(r.body);
                r = R(200, NULL, "{\"access_token\":\"ya29.sa-token\",\"expires_in\":3600,\"token_type\":\"Bearer\"}");
            }
            free(aud);
            json_free(c);
            free(as);
        } else if (str_eq(gt, "refresh_token")) {
            char *rt = form_get(body, "refresh_token"), *cs = form_get(body, "client_secret");
            if (str_eq(rt, "rt-1") && str_eq(cs, "secret-1")) {
                free(r.body);
                r = R(200, NULL, "{\"access_token\":\"ya29.user-token\",\"expires_in\":3600}");
            }
            free(rt);
            free(cs);
        }
        free(gt);
        return r;
    }
    if (!strcmp(path, "/v1/oauthtoken")) {
        /* Basic base64("workforce-client:workforce-secret"); body exactly grant_type + refresh_token */
        if (!str_eq(auth, "Basic d29ya2ZvcmNlLWNsaWVudDp3b3JrZm9yY2Utc2VjcmV0")) return R(401, NULL, "{\"error\":\"bad client\"}");
        if (str_eq(body, "grant_type=refresh_token&refresh_token=file-refresh"))
            return R(200, NULL, "{\"access_token\":\"first-access\",\"expires_in\":0,\"refresh_token\":\"rotated-refresh\"}");
        if (str_eq(body, "grant_type=refresh_token&refresh_token=rotated-refresh"))
            return R(200, NULL, "{\"access_token\":\"second-access\",\"expires_in\":3600}");
        return R(400, NULL, "{\"error\":\"invalid_grant\"}");
    }
    if (!strcmp(path, "/v1/projects/-/serviceAccounts/target@proj.iam.gserviceaccount.com:generateAccessToken")) {
        json_t *b = json_parse_str(body);
        bool ok = str_eq(auth, "Bearer ya29.user-token") && str_eq(json_get_str(b, "lifetime"), "3600s") &&
                  str_eq(P(b, "scope.0"), GOOGLE_CLOUD_PLATFORM_SCOPE) && str_eq(P(b, "delegates.0"), "d@x");
        json_free(b);
        return ok ? R(200, NULL, "{\"accessToken\":\"ya29.imp-token\",\"expireTime\":\"2099-01-01T00:00:00Z\"}")
                  : R(403, NULL, "{\"error\":{\"message\":\"denied\"}}");
    }
    if (str_starts(path, "/v1/projects/proj-9/locations/us-central1/publishers/google/models/gemini-x:streamGenerateContent")) {
        if (!str_eq(auth, "Bearer ya29.sa-token")) return R(401, NULL, "{\"error\":{\"message\":\"bad token\"}}");
        return R(200, "text/event-stream",
                 "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"vertex says hi\"}]},"
                 "\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":5,\"candidatesTokenCount\":3,"
                 "\"totalTokenCount\":8}}\n\n");
    }
    return R(404, NULL, "{}");
}

static char *g_tmp;

static void write_creds(const char *json) {
    char *p = path_join(g_tmp, "creds.json");
    fs_write(p, json, strlen(json));
    setenv("GOOGLE_APPLICATION_CREDENTIALS", p, 1);
    free(p);
    google_auth_reset_cache();
}

static void test_adc(void) {
    g_route = adc_route;
    char *err = NULL;
    /* explicit token wins and is never cached */
    setenv("GOOGLE_CLOUD_ACCESS_TOKEN", "ya29.explicit", 1);
    char *t = google_vertex_access_token(&err);
    CHECK_STR(t, "ya29.explicit");
    free(t);
    unsetenv("GOOGLE_CLOUD_ACCESS_TOKEN");

    /* service account -> RS256 JWT bearer grant (token_uri honoured), cached */
    json_t *sa = json_obj();
    json_set_str(sa, "type", "service_account");
    json_set_str(sa, "client_email", "sa@proj.iam.gserviceaccount.com");
    json_set_str(sa, "private_key", g_key_pem);
    json_set_str(sa, "private_key_id", "k1");
    char *tu = xasprintf("http://127.0.0.1:%d/token", g_port);
    json_set_str(sa, "token_uri", tu);
    char *saj = json_dump(sa, 0);
    write_creds(saj);
    int hits0 = g_hits;
    t = google_vertex_access_token(&err);
    CHECK_STR(t, "ya29.sa-token");
    if (!t) fprintf(stderr, "  adc err: %s\n", err ? err : "?");
    free(t);
    free(err);
    err = NULL;
    t = google_vertex_access_token(&err);
    CHECK_STR(t, "ya29.sa-token");
    free(t);
    CHECK_INT(g_hits - hits0, 1);

    /* authorized_user -> refresh grant with client secret */
    char *au = xasprintf("{\"type\":\"authorized_user\",\"client_id\":\"cid\",\"client_secret\":\"secret-1\","
                         "\"refresh_token\":\"rt-1\",\"token_uri\":\"%s\"}", tu);
    write_creds(au);
    t = google_vertex_access_token(&err);
    CHECK_STR(t, "ya29.user-token");
    free(t);

    /* impersonated service account: source credential, then iamcredentials generateAccessToken */
    char *imp = xasprintf("{\"type\":\"impersonated_service_account\",\"delegates\":[\"d@x\"],"
                          "\"service_account_impersonation_url\":\"http://127.0.0.1:%d/v1/projects/-/serviceAccounts/"
                          "target@proj.iam.gserviceaccount.com:generateAccessToken\",\"source_credentials\":%s}", g_port, au);
    write_creds(imp);
    t = google_vertex_access_token(&err);
    CHECK_STR(t, "ya29.imp-token");
    free(t);

    /* workforce ADC: refresh at the file's STS token_url with Basic client auth; a rotated token replaces the
     * file's on later refreshes (expires_in 0 forces the second exchange) */
    char *wf = xasprintf("{\"type\":\"external_account_authorized_user\",\"client_id\":\"workforce-client\","
                         "\"client_secret\":\"workforce-secret\",\"refresh_token\":\"file-refresh\","
                         "\"token_url\":\"http://127.0.0.1:%d/v1/oauthtoken\"}", g_port);
    write_creds(wf);
    int wf0 = g_hits;
    t = google_vertex_access_token(&err);
    CHECK_STR(t, "first-access");
    free(t);
    t = google_vertex_access_token(&err);
    CHECK_STR(t, "second-access");
    free(t);
    CHECK_INT(g_hits - wf0, 2);
    free(wf);

    /* unknown ADC types are rejected before any token request */
    write_creds("{\"type\":\"unrecognized_adc\",\"client_id\":\"id\",\"client_secret\":\"secret\",\"refresh_token\":\"token\"}");
    int un0 = g_hits;
    t = google_vertex_access_token(&err);
    CHECK(t == NULL && err && strstr(err, "Unsupported ADC credential type: unrecognized_adc"));
    CHECK_INT(g_hits - un0, 0);
    free(err);
    err = NULL;

    /* missing file named by GOOGLE_APPLICATION_CREDENTIALS */
    setenv("GOOGLE_APPLICATION_CREDENTIALS", "/nonexistent/creds.json", 1);
    google_auth_reset_cache();
    t = google_vertex_access_token(&err);
    CHECK(t == NULL && err && strstr(err, "points to a missing file"));
    free(err);
    err = NULL;

    /* env hook: ADC file + project + location */
    write_creds(saj);
    unsetenv("GOOGLE_CLOUD_API_KEY");
    setenv("GOOGLE_CLOUD_PROJECT", "proj-9", 1);
    unsetenv("GOOGLE_CLOUD_LOCATION");
    CHECK(!google_vertex_has_credentials());
    setenv("GOOGLE_CLOUD_LOCATION", "us-central1", 1);
    CHECK(google_vertex_has_credentials());

    /* full Vertex stream through ADC against a gateway baseUrl */
    char *base = xasprintf("http://127.0.0.1:%d", g_port);
    json_t *raw = JQ("{'id':'gemini-x','api':'google-vertex','provider':'google-vertex','reasoning':false,'maxTokens':1000}");
    json_set_str(raw, "baseUrl", base);
    ai_model_t m = mk_model(raw);
    json_t *msgs = JQ("[{'role':'user','content':'hi'}]");
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.max_retries = 0;
    json_t *msg = provider_google_stream(&m, &ctx, &o, NULL, NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "stop");
    CHECK_STR(P(msg, "content.0.text"), "vertex says hi");
    json_free(msg);
    unsetenv("GOOGLE_CLOUD_PROJECT");
    msg = provider_google_stream(&m, &ctx, &o, NULL, NULL);
    CHECK(strstr(json_get_str(msg, "errorMessage") ? json_get_str(msg, "errorMessage") : "", "requires a project ID"));
    json_free(msg);
    json_free(msgs);
    json_free(raw);
    free(base);
    unsetenv("GOOGLE_CLOUD_LOCATION");
    unsetenv("GOOGLE_APPLICATION_CREDENTIALS");
    free(imp);
    free(au);
    free(saj);
    free(tu);
    json_free(sa);
}

/* ====================================================================== CCA requests */

#define GEMINI_CLI_PRO                                                                                                 \
    "{'id':'gemini-2.5-pro','api':'google-gemini-cli','provider':'google-gemini-cli',"                                 \
    "'baseUrl':'https://cloudcode-pa.googleapis.com','reasoning':true,'maxTokens':65536,"                              \
    "'thinking':{'mode':'budget','efforts':['minimal','low','medium','high'],'requiresEffort':true},"                 \
    "'identity':{'class':'gemini'},'compat':{'thinkingLoopGuard':'gemini'}}"

#define AG_OPUS                                                                                                        \
    "{'id':'claude-opus-4-6','api':'google-gemini-cli','provider':'google-antigravity',"                              \
    "'baseUrl':'https://daily-cloudcode-pa.googleapis.com','reasoning':true,'input':['text','image'],"                 \
    "'maxTokens':64000,'requestModelId':'claude-opus-4-6-thinking',"                                                   \
    "'thinking':{'mode':'budget','efforts':['minimal','low','medium','high']},"                                       \
    "'identity':{'class':'anthropic'},'compat':{'supportsFunctionPartId':true,'dropUnsignedThinking':true,"           \
    "'ccaLegacyParametersSchema':true,'claudeThinkingBetaHeader':true,'antigravityClaudeToolMode':true,"               \
    "'antigravityUsageLabel':'true'}}"

#define AG_FLASH                                                                                                       \
    "{'id':'gemini-3.8-flash','api':'google-gemini-cli','provider':'google-antigravity',"                             \
    "'baseUrl':'https://daily-cloudcode-pa.googleapis.com','reasoning':true,'maxTokens':65536,"                        \
    "'requestModelId':'gemini-3.8-flash-low','thinking':{'mode':'google-level',"                                      \
    "'efforts':['minimal','low','medium','high'],'requiresEffort':true,'effortRouting':{'minimal':"                   \
    "'gemini-3.8-flash-low','low':'gemini-3.8-flash-low','medium':'gemini-3.8-flash-medium','high':"                 \
    "'gemini-3.8-flash-high'}},'antigravityWireProfiles':{'gemini-3.8-flash-medium':{'modelEnum':"                    \
    "'MODEL_PLACEHOLDER_M319','maxOutputTokens':65536,'thinkingBudget':4000}},"                                       \
    "'identity':{'class':'gemini'},'compat':{'flashStreamLeakWorkaround':true,'streamFirstEventTimeoutMs':60000}}"

static const char *TOOLS =
    "[{'name':'bash','description':'Run a command','parameters':{'type':'object','properties':{"
    "'command':{'type':'string','minLength':1},'timeout':{'type':['number','null']}},'required':['command','timeout'],"
    "'additionalProperties':false}}]";

static void test_cca_request(void) {
    json_t *raw = JQ(GEMINI_CLI_PRO);
    ai_model_t m = mk_model(raw);
    json_t *msgs = JQ("[{'role':'user','content':'hello'}]");
    json_t *sys = JQ("['You are helpful.','  ']");
    json_t *tools = JQ(TOOLS);
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_HIGH;
    json_t *b = gemini_cli_build_request(&m, &ctx, &o, "proj-1", false, NULL);
    char *dump = json_dump(b, 0);
    CHECK(str_starts(dump, "{\"project\":\"proj-1\",\"model\":\"gemini-2.5-pro\",\"request\":{\"contents\""));
    free(dump);
    CHECK_STR(P(b, "request.contents.0.parts.0.text"), "hello");
    CHECK_STR(P(b, "request.systemInstruction.parts.0.text"), "You are helpful.");
    CHECK(json_path(b, "request.systemInstruction.role") == NULL);
    CHECK(json_len(json_path(b, "request.systemInstruction.parts")) == 1);
    CHECK_INT(PI(b, "request.generationConfig.maxOutputTokens"), 65536);
    CHECK_INT(PI(b, "request.generationConfig.thinkingConfig.thinkingBudget"), 16384);
    CHECK(json_get_bool(json_path(b, "request.generationConfig.thinkingConfig"), "includeThoughts", false));
    CHECK(json_path(b, "request.tools.0.functionDeclarations.0.parametersJsonSchema") != NULL);
    CHECK(json_path(b, "request.toolConfig") == NULL);
    CHECK(json_path(b, "request.labels") == NULL);
    json_free(b);
    /* requiresEffort: "off" clamps to the lowest effort; explicit tool choice */
    o.reasoning = EFFORT_OFF;
    json_t *tc = JQ("'none'");
    o.tool_choice = tc;
    b = gemini_cli_build_request(&m, &ctx, &o, "proj-1", false, NULL);
    CHECK_INT(PI(b, "request.generationConfig.thinkingConfig.thinkingBudget"), 1024);
    CHECK_STR(P(b, "request.toolConfig.functionCallingConfig.mode"), "NONE");
    json_free(b);
    json_free(tc);
    o.tool_choice = NULL;
    json_free(raw);

    /* Antigravity Claude: legacy CCA parameters, VALIDATED mode, labels, profile output cap, role-tagged system */
    raw = JQ(AG_OPUS);
    m = mk_model(raw);
    o.reasoning = EFFORT_MEDIUM;
    cca_session_t st = {0};
    b = gemini_cli_build_request(&m, &ctx, &o, "proj-2", true, &st);
    CHECK_STR(json_get_str(b, "project"), "proj-2");
    CHECK_STR(json_get_str(b, "model"), "claude-opus-4-6-thinking");
    CHECK_STR(json_get_str(b, "userAgent"), "antigravity");
    CHECK(str_starts(json_get_str(b, "requestId"), "agent/"));
    CHECK_STR(P(b, "request.systemInstruction.role"), "user");
    CHECK_STR(P(b, "request.toolConfig.functionCallingConfig.mode"), "VALIDATED");
    CHECK_INT(PI(b, "request.generationConfig.maxOutputTokens"), 64000);
    CHECK_INT(PI(b, "request.generationConfig.thinkingConfig.thinkingBudget"), 8192);
    json_t *params = json_path(b, "request.tools.0.functionDeclarations.0.parameters");
    CHECK(params != NULL && json_path(b, "request.tools.0.functionDeclarations.0.parametersJsonSchema") == NULL);
    CHECK_STR(P(params, "properties.timeout.type"), "number");
    CHECK(json_path(params, "properties.command.minLength") == NULL);
    CHECK_STR(P(params, "properties.command.description"), "{minLength: 1}");
    CHECK(json_get(params, "additionalProperties") == NULL);
    CHECK_STR(P(b, "request.labels.used_claude"), "true");
    CHECK_STR(P(b, "request.labels.used_non_gemini_model"), "true");
    CHECK_STR(P(b, "request.labels.cascade_id"), st.agent_id);
    CHECK_STR(P(b, "request.labels.root_cascade_id"), st.agent_id);
    CHECK_STR(P(b, "request.labels.last_step_index"), "0");
    char *rid0 = xasprintf("%s-0", st.trajectory_id);
    CHECK_STR(P(b, "request.labels.request_id"), rid0);
    free(rid0);
    CHECK_STR(P(b, "request.sessionId"), st.session_id);
    CHECK(st.session_id && st.session_id[0] == '-');
    char *keys = NULL;
    {
        json_t *req = json_get(b, "request");
        sb_t ks = SB_INIT;
        for (size_t i = 0; i < json_len(req); i++) sb_appendf(&ks, "%s,", req->u.obj.keys[i]);
        keys = sb_detach(&ks);
    }
    CHECK_STR(keys, "contents,systemInstruction,tools,toolConfig,labels,generationConfig,sessionId,");
    free(keys);
    json_free(b);
    /* second request on the same conversation advances the step and request counter */
    free(st.last_execution_id);
    st.last_execution_id = xstrdup("resp-prev");
    b = gemini_cli_build_request(&m, &ctx, &o, "proj-2", true, &st);
    char *rid1 = xasprintf("%s-1", st.trajectory_id);
    CHECK_STR(P(b, "request.labels.request_id"), rid1);
    free(rid1);
    CHECK_STR(P(b, "request.labels.last_step_index"), "1");
    CHECK_STR(P(b, "request.labels.last_execution_id"), "resp-prev");
    json_free(b);
    /* no session state: session id derived from the first user text (stable) */
    json_t *b1 = gemini_cli_build_request(&m, &ctx, &o, "p", true, NULL);
    json_t *b2 = gemini_cli_build_request(&m, &ctx, &o, "p", true, NULL);
    CHECK_STR(P(b1, "request.sessionId"), P(b2, "request.sessionId"));
    json_free(b1);
    json_free(b2);
    json_free(raw);

    /* Antigravity Gemini tier SKU: wire id from effort routing, SKU thinking budget, model_enum, forced-tool directive */
    raw = JQ(AG_FLASH);
    m = mk_model(raw);
    tc = JQ("'required'");
    o.tool_choice = tc;
    b = gemini_cli_build_request(&m, &ctx, &o, "proj-3", true, NULL);
    CHECK_STR(json_get_str(b, "model"), "gemini-3.8-flash-medium");
    CHECK_INT(PI(b, "request.generationConfig.thinkingConfig.thinkingBudget"), 4000);
    CHECK(json_path(b, "request.generationConfig.thinkingConfig.thinkingLevel") == NULL);
    CHECK_INT(PI(b, "request.generationConfig.maxOutputTokens"), 65536);
    CHECK_STR(P(b, "request.labels.model_enum"), "MODEL_PLACEHOLDER_M319");
    CHECK_STR(P(b, "request.labels.used_claude"), "false");
    CHECK_STR(P(b, "request.labels.used_non_gemini_model"), "false");
    CHECK_STR(P(b, "request.toolConfig.functionCallingConfig.mode"), "ANY");
    json_t *contents = json_path(b, "request.contents");
    const char *last = P(json_at(contents, json_len(contents) - 1), "parts.0.text");
    CHECK(last && str_starts(last, "TOOL-ONLY TURN."));
    json_free(b);
    json_free(tc);
    o.tool_choice = NULL;
    /* endpoints: auto mode starts on daily, prefers the last good one; a custom baseUrl is used alone */
    strvec_t eps = gemini_cli_endpoints(&m, true, NULL);
    CHECK_INT(eps.n, 2);
    CHECK_STR(eps.v[0], CCA_ANTIGRAVITY_DAILY_ENDPOINT);
    strvec_free(&eps);
    cca_session_t st2 = {.last_good_endpoint = (char *)CCA_ANTIGRAVITY_SANDBOX_ENDPOINT};
    eps = gemini_cli_endpoints(&m, true, &st2);
    CHECK_STR(eps.v[0], CCA_ANTIGRAVITY_SANDBOX_ENDPOINT);
    strvec_free(&eps);
    m.base_url = "http://127.0.0.1:1/";
    eps = gemini_cli_endpoints(&m, true, NULL);
    CHECK_INT(eps.n, 1);
    strvec_free(&eps);
    json_free(raw);

    free(st.agent_id);
    free(st.trajectory_id);
    free(st.session_id);
    free(st.last_execution_id);
    json_free(msgs);
    json_free(sys);
    json_free(tools);

    /* structured credentials */
    cca_creds_t c;
    char *err = NULL;
    CHECK(cca_parse_credentials("{\"token\":\"t\",\"projectId\":\"p\",\"expiresAt\":1700000000,\"email\":\"a@b\"}", &c, &err) == 0);
    CHECK_STR(c.token, "t");
    CHECK_INT(c.expires_at, 1700000000000LL);
    cca_creds_free(&c);
    CHECK(cca_parse_credentials("{\"token\":\"t\"}", &c, &err) != 0 && strstr(err, "Missing token or projectId"));
    free(err);
    CHECK(cca_parse_credentials("ya29.raw", &c, &err) != 0 && strstr(err, "Invalid Google Cloud Code Assist"));
    free(err);
}

/* ====================================================================== CCA SSE */

typedef struct {
    sb_t names;
} rec_t;

static void rec_cb(void *ud, const ai_event_t *ev) {
    rec_t *r = ud;
    if (ev->type == AI_EV_TEXT_DELTA || ev->type == AI_EV_THINKING_DELTA) return;
    if (r->names.len) sb_putc(&r->names, ',');
    sb_append(&r->names, ai_event_name(ev->type));
}

static int feed_sse(const char *text, sse_event_cb cb, void *ud) {
    sse_parser_t p;
    sse_init(&p, cb, ud);
    size_t n = strlen(text);
    int rc = 0;
    for (size_t i = 0; i < n && !rc; i += 7) rc = sse_feed(&p, text + i, MIN((size_t)7, n - i));
    if (!rc) rc = sse_finish(&p);
    sse_free(&p);
    return rc;
}

static const char *CCA_SSE =
    "data: {\"response\":{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Let me think\","
    "\"thought\":true,\"thoughtSignature\":\"c2lnMQ==\"}]}}],\"responseId\":\"resp-1\"},\"traceId\":\"t\"}\n\n"
    "data: {\"response\":{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Hello \"}]}}]}}\n\n"
    "data: {\"response\":{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"world\"},"
    "{\"functionCall\":{\"name\":\"bash\",\"args\":{\"command\":\"ls\"},\"id\":\"call_1\"},\"thoughtSignature\":\"c2lnMg==\"}]},"
    "\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":100,\"candidatesTokenCount\":20,"
    "\"thoughtsTokenCount\":5,\"totalTokenCount\":125,\"cachedContentTokenCount\":40}}}\n\n";

static json_t *run_cca(const ai_model_t *m, const json_t *tools, const char *sse, cca_sse_t *keep, rec_t *rec, int *rc_out) {
    cca_sse_t s;
    cca_sse_init(&s, m, tools, rec ? rec_cb : NULL, rec);
    int rc = feed_sse(sse, cca_sse_event, &s);
    if (rc_out) *rc_out = rc;
    cca_sse_end(&s);
    if (keep) {
        keep->stripped_leak = s.stripped_leak;
        keep->saw_finish = s.saw_finish;
        keep->response_id = s.response_id ? xstrdup(s.response_id) : NULL;
    }
    json_t *msg = aib_finish(&s.b, rc == 2 ? "error" : s.stop_reason, rc == 2 ? s.error : NULL);
    cca_sse_free(&s);
    return msg;
}

static void test_cca_sse(void) {
    json_t *raw = JQ(GEMINI_CLI_PRO);
    ai_model_t m = mk_model(raw);
    rec_t rec = {SB_INIT};
    cca_sse_t k = {0};
    json_t *msg = run_cca(&m, NULL, CCA_SSE, &k, &rec, NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "toolUse");
    CHECK_STR(P(msg, "content.0.type"), "thinking");
    CHECK_STR(P(msg, "content.0.thinking"), "Let me think");
    CHECK_STR(P(msg, "content.0.thinkingSignature"), "c2lnMQ==");
    CHECK(json_get_bool(json_path(msg, "content.0"), "summary", false));
    CHECK_STR(P(msg, "content.1.text"), "Hello world");
    CHECK_STR(P(msg, "content.2.id"), "call_1");
    CHECK_STR(P(msg, "content.2.arguments.command"), "ls");
    CHECK_STR(P(msg, "content.2.thoughtSignature"), "c2lnMg==");
    CHECK_INT(PI(msg, "usage.input"), 60);
    CHECK_INT(PI(msg, "usage.output"), 25);
    CHECK_INT(PI(msg, "usage.cacheRead"), 40);
    CHECK_INT(PI(msg, "usage.reasoningTokens"), 5);
    CHECK_STR(k.response_id, "resp-1");
    CHECK_STR(sb_cstr(&rec.names), "start,thinking_start,thinking_end,text_start,text_end,toolcall_start,toolcall_delta,toolcall_end,done");
    free(k.response_id);
    sb_free(&rec.names);
    json_free(msg);

    /* in-band error frame */
    int rc = 0;
    msg = run_cca(&m, NULL,
                  "data: {\"error\":{\"code\":429,\"message\":\"Quota\",\"status\":\"RESOURCE_EXHAUSTED\",\"details\":[{\"reason\":\"x\"}]}}\n\n",
                  NULL, NULL, &rc);
    CHECK_INT(rc, 2);
    CHECK_STR(json_get_str(msg, "errorMessage"),
              "Cloud Code Assist stream error: Quota (RESOURCE_EXHAUSTED) {\"error\":{\"status\":\"RESOURCE_EXHAUSTED\",\"details\":[{\"reason\":\"x\"}]}}");
    json_free(msg);
    /* error finish reason, blocked prompt */
    msg = run_cca(&m, NULL, "data: {\"response\":{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"x\"}]},\"finishReason\":\"MALFORMED_FUNCTION_CALL\"}]}}\n\n", NULL, NULL, NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    json_free(msg);
    msg = run_cca(&m, NULL, "data: {\"response\":{\"promptFeedback\":{\"blockReason\":\"SAFETY\",\"blockReasonMessage\":\"no\"}}}\n\n", NULL, NULL, &rc);
    CHECK_STR(json_get_str(msg, "errorMessage"), "Request blocked by Google (SAFETY): no");
    json_free(msg);

    /* leaked reasoning markup in visible text is healed into thinking */
    msg = run_cca(&m, NULL,
                  "data: {\"response\":{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"<thi\"}]}}]}}\n\n"
                  "data: {\"response\":{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"nk>hidden</think>visible\"}]},\"finishReason\":\"STOP\"}]}}\n\n",
                  NULL, NULL, NULL);
    CHECK_STR(P(msg, "content.0.type"), "thinking");
    CHECK_STR(P(msg, "content.0.thinking"), "hidden");
    CHECK_STR(P(msg, "content.1.text"), "visible");
    json_free(msg);
    json_free(raw);

    /* flash planning leak: a leading {"thought": ...} object is buffered and stripped */
    raw = JQ(AG_FLASH);
    m = mk_model(raw);
    json_t *tools = JQ(TOOLS);
    k = (cca_sse_t){0};
    msg = run_cca(&m, tools,
                  "data: {\"response\":{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"{\\\"thought\\\": \\\"I will\"}]}}]}}\n\n"
                  "data: {\"response\":{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\" do it\\\"}Real answer\"}]},\"finishReason\":\"STOP\"}]}}\n\n",
                  &k, NULL, NULL);
    CHECK(k.stripped_leak);
    CHECK_STR(P(msg, "content.0.text"), "Real answer");
    CHECK_INT(json_len(json_get(msg, "content")), 1);
    json_free(msg);
    /* ordinary JSON answers stay visible */
    k = (cca_sse_t){0};
    msg = run_cca(&m, tools, "data: {\"response\":{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"{\\\"a\\\": 1}\"}]},\"finishReason\":\"STOP\"}]}}\n\n", &k, NULL, NULL);
    CHECK(!k.stripped_leak);
    CHECK_STR(P(msg, "content.0.text"), "{\"a\": 1}");
    json_free(msg);
    json_free(tools);
    json_free(raw);
}

/* ====================================================================== think_heal */

typedef struct {
    sb_t text, thinking;
} heal_rec_t;

static void heal_cb(void *ud, bool thinking, const char *s, size_t n) {
    heal_rec_t *r = ud;
    sb_appendn(thinking ? &r->thinking : &r->text, s, n);
}

static void heal(const char *const *chunks, const char *want_text, const char *want_thinking) {
    think_heal_t h;
    think_heal_init(&h);
    heal_rec_t r = {SB_INIT, SB_INIT};
    for (size_t i = 0; chunks[i]; i++) think_heal_feed(&h, chunks[i], strlen(chunks[i]), heal_cb, &r);
    think_heal_flush(&h, heal_cb, &r);
    CHECK_STR(sb_cstr(&r.text), want_text);
    CHECK_STR(sb_cstr(&r.thinking), want_thinking);
    think_heal_free(&h);
    sb_free(&r.text);
    sb_free(&r.thinking);
}

static void test_think_heal(void) {
    heal((const char *[]){"<thi", "nk>abc</th", "ink>tail", NULL}, "tail", "abc");
    heal((const char *[]){"plain text only", NULL}, "plain text only", "");
    heal((const char *[]){"use `<think>` literally", NULL}, "use `<think>` literally", "");
    heal((const char *[]){"```thinking\nreason\n", "```\nreply", NULL}, "\nreply", "reason\n");
    heal((const char *[]){"```thinking\na\n```rs\nfn x(){}\n```\nb\n```\nafter", NULL}, "\nafter", "a\n```rs\nfn x(){}\n```\nb\n");
    heal((const char *[]){"```c\n<think>x</think>\n```\n", NULL}, "```c\n<think>x</think>\n```\n", "");
    heal((const char *[]){"<thinking>unterminated", NULL}, "", "unterminated");
    heal((const char *[]){"a <", NULL}, "a <", "");
}

/* ====================================================================== CCA schema */

static void test_cca_schema(void) {
    json_t *s = JQ("{'type':'object','properties':{'a':{'type':['string','null']},'b':{'anyOf':[{'type':'string'},{'type':'null'}]},"
                   "'c':{'type':'string','format':'uri'}},'required':['a','b','c'],'additionalProperties':false}");
    json_t *n = google_normalize_schema_cca(s);
    CHECK_STR(P(n, "properties.a.type"), "string");
    CHECK_STR(P(n, "properties.b.type"), "string");
    CHECK(json_path(n, "properties.b.anyOf") == NULL);
    CHECK_STR(P(n, "properties.c.description"), "{format: \"uri\"}");
    CHECK(json_get(n, "additionalProperties") == NULL);
    CHECK_INT(json_len(json_get(n, "required")), 3);
    json_free(n);
    json_free(s);
    /* object variants merge; only fields every branch requires stay required */
    s = JQ("{'anyOf':[{'type':'object','properties':{'p':{'type':'string'},'r':{'type':'string'}},'required':['p','r']},"
           "{'type':'object','properties':{'q':{'type':'number'},'r':{'type':'string'}},'required':['q','r']}]}");
    n = google_normalize_schema_cca(s);
    CHECK_STR(json_get_str(n, "type"), "object");
    CHECK(json_path(n, "properties.p") && json_path(n, "properties.q"));
    CHECK_INT(json_len(json_get(n, "required")), 1);
    CHECK_STR(P(n, "required.0"), "r");
    json_free(n);
    json_free(s);
    /* an unrepresentable `not` falls back to the empty object schema */
    s = JQ("{'type':'object','properties':{'x':{'not':{'type':'string'}}}}");
    n = google_normalize_schema_cca(s);
    char *d = json_dump(n, 0);
    CHECK_STR(d, "{\"type\":\"object\",\"properties\":{}}");
    free(d);
    json_free(n);
    json_free(s);
    /* string enums across same-type branches are unioned */
    s = JQ("{'oneOf':[{'type':'string','enum':['a']},{'type':'string','enum':['b']}]}");
    n = google_normalize_schema_cca(s);
    CHECK_INT(json_len(json_get(n, "enum")), 2);
    json_free(n);
    json_free(s);
}

/* ====================================================================== project discovery */

static int g_ag_onboarded, g_ag_polls;

static reply_t cca_route(const char *method, const char *path, const http_headers_t *h, const char *body) {
    json_t *b = json_parse_str(body);
    reply_t r = {404, NULL, NULL};
    const char *ua = http_headers_get(h, "User-Agent");
    if (!str_eq(http_headers_get(h, "Authorization"), "Bearer at-1")) {
        r = R(401, NULL, "{}");
    } else if (!strcmp(path, "/gem/v1internal:loadCodeAssist")) {
        bool ok = ua && str_starts(ua, "GeminiCLI/") && str_eq(P(b, "metadata.pluginType"), "GEMINI");
        r = ok ? R(200, NULL, "{\"allowedTiers\":[{\"id\":\"free-tier\",\"isDefault\":true}]}") : R(400, NULL, "{}");
    } else if (!strcmp(path, "/gem/v1internal:onboardUser")) {
        r = str_eq(json_get_str(b, "tierId"), "free-tier")
                ? R(200, NULL, "{\"done\":true,\"response\":{\"cloudaicompanionProject\":{\"id\":\"gem-proj\"}}}")
                : R(400, NULL, "{}");
    } else if (!strcmp(path, "/val/v1internal:loadCodeAssist")) {
        r = R(403, NULL, "{\"error\":{\"code\":403,\"status\":\"PERMISSION_DENIED\",\"details\":[{\"reason\":\"VALIDATION_REQUIRED\","
                         "\"metadata\":{\"validation_url\":\"https://accounts.google.com/verify\"}}]}}");
    } else if (!strcmp(path, "/ag/v1internal:loadCodeAssist")) {
        bool ok = ua && str_starts(ua, "antigravity/hub/") && str_eq(P(b, "metadata.ideType"), "ANTIGRAVITY");
        if (!ok) r = R(400, NULL, "{}");
        else if (!g_ag_onboarded) r = R(200, NULL, "{\"allowedTiers\":[{\"id\":\"free-tier\"}]}");
        else r = R(200, NULL, "{\"currentTier\":{\"id\":\"free-tier\"},\"paidTier\":null,\"cloudaicompanionProject\":\"ag-proj\"}");
    } else if (!strcmp(path, "/ag/v1internal:onboardUser")) {
        r = R(200, NULL, "{\"name\":\"operations/op-1\",\"done\":false}");
    } else if (!strcmp(path, "/ag/v1internal/operations/op-1") && !strcmp(method, "GET")) {
        g_ag_polls++;
        g_ag_onboarded = 1;
        r = R(200, NULL, "{\"name\":\"operations/op-1\",\"done\":true,\"response\":{\"@type\":\"x\"}}");
    }
    json_free(b);
    return r;
}

static void test_discovery(void) {
    g_route = cca_route;
    unsetenv("GOOGLE_CLOUD_PROJECT");
    unsetenv("GOOGLE_CLOUD_PROJECT_ID");
    char *ep = xasprintf("http://127.0.0.1:%d/gem", g_port);
    char *proj = NULL, *err = NULL;
    CHECK(cca_discover_project("google-gemini-cli", "at-1", ep, NULL, NULL, &proj, &err) == 0);
    CHECK_STR(proj, "gem-proj");
    free(proj);
    free(err);
    free(ep);
    ep = xasprintf("http://127.0.0.1:%d/ag", g_port);
    proj = err = NULL;
    CHECK(cca_discover_project("google-antigravity", "at-1", ep, NULL, NULL, &proj, &err) == 0);
    CHECK_STR(proj, "ag-proj");
    CHECK_INT(g_ag_polls, 1);
    if (err) fprintf(stderr, "  discovery err: %s\n", err);
    free(proj);
    free(err);
    free(ep);
    ep = xasprintf("http://127.0.0.1:%d/val", g_port);
    proj = err = NULL;
    CHECK(cca_discover_project("google-gemini-cli", "at-1", ep, NULL, NULL, &proj, &err) != 0);
    char *vu = cca_extract_validation_url(err);
    CHECK_STR(vu, "https://accounts.google.com/verify");
    char *vm = cca_validation_message(vu, "sign in again", "me@x");
    CHECK_STR(vm, "Account verification required for me@x. Visit https://accounts.google.com/verify to continue, then sign in again.");
    free(vm);
    free(vu);
    free(err);
    free(ep);
}

/* ====================================================================== CCA stream end to end */

static int g_stream_hits;

static reply_t stream_route(const char *method, const char *path, const http_headers_t *h, const char *body) {
    if (strcmp(path, "/v1internal:streamGenerateContent?alt=sse") != 0) return R(404, NULL, "{}");
    g_stream_hits++;
    if (!str_eq(http_headers_get(h, "Authorization"), "Bearer cca-tok")) return R(401, NULL, "{\"error\":{\"message\":\"unauth\"}}");
    json_t *b = json_parse_str(body);
    const char *text = P(b, "request.contents.0.parts.0.text");
    reply_t r;
    if (str_eq(text, "verify me")) {
        r = R(403, NULL, "{\"error\":{\"code\":403,\"details\":[{\"reason\":\"VALIDATION_REQUIRED\",\"metadata\":"
                         "{\"validation_url\":\"https://v.example/x\"}}]}}");
    } else if (str_eq(text, "empty")) {
        r = R(200, "text/event-stream", "data: {\"response\":{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\" \"}]},\"finishReason\":\"STOP\"}]}}\n\n");
    } else if (str_eq(json_get_str(b, "project"), "proj-x") && str_eq(http_headers_get(h, "Accept"), "text/event-stream") &&
               str_starts(http_headers_get(h, "User-Agent"), "GeminiCLI/0.46.0/gemini-2.5-pro (")) {
        r = R(200, "text/event-stream", CCA_SSE);
    } else {
        r = R(400, NULL, "{\"error\":{\"message\":\"bad envelope\"}}");
    }
    json_free(b);
    return r;
}

static void test_cca_stream(void) {
    g_route = stream_route;
    json_t *raw = JQ(GEMINI_CLI_PRO);
    char *base = xasprintf("http://127.0.0.1:%d", g_port);
    json_set_str(raw, "baseUrl", base);
    ai_model_t m = mk_model(raw);
    json_t *msgs = JQ("[{'role':'user','content':'hi'}]");
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_LOW;
    o.api_key = "{\"token\":\"cca-tok\",\"projectId\":\"proj-x\",\"expiresAt\":99999999999999}";
    json_t *msg = provider_gemini_cli_stream(&m, &ctx, &o, NULL, NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "toolUse");
    CHECK_STR(P(msg, "content.1.text"), "Hello world");
    CHECK_STR(json_get_str(msg, "api"), "google-gemini-cli");
    json_free(msg);
    /* validation-required errors name the verification URL */
    json_free(msgs);
    msgs = JQ("[{'role':'user','content':'verify me'}]");
    ctx.messages = msgs;
    o.api_key = "{\"token\":\"cca-tok\",\"projectId\":\"proj-x\",\"email\":\"me@x\"}";
    msg = provider_gemini_cli_stream(&m, &ctx, &o, NULL, NULL);
    CHECK_STR(json_get_str(msg, "errorMessage"),
              "Cloud Code Assist API error (403): Account verification required for me@x. Visit https://v.example/x to continue, then retry your request.");
    json_free(msg);
    /* empty STOP responses are retried, then reported */
    json_free(msgs);
    msgs = JQ("[{'role':'user','content':'empty'}]");
    ctx.messages = msgs;
    g_stream_hits = 0;
    msg = provider_gemini_cli_stream(&m, &ctx, &o, NULL, NULL);
    CHECK_STR(json_get_str(msg, "errorMessage"), "Cloud Code Assist API returned an empty response");
    CHECK_INT(g_stream_hits, 3);
    json_free(msg);
    /* expired / missing credentials */
    o.api_key = "{\"token\":\"cca-tok\",\"projectId\":\"proj-x\",\"expiresAt\":1000}";
    msg = provider_gemini_cli_stream(&m, &ctx, &o, NULL, NULL);
    CHECK(strstr(json_get_str(msg, "errorMessage"), "OAuth token expired before request"));
    json_free(msg);
    o.api_key = NULL;
    msg = provider_gemini_cli_stream(&m, &ctx, &o, NULL, NULL);
    CHECK_STR(json_get_str(msg, "errorMessage"), "Google Cloud Code Assist requires OAuth authentication. Use /login to authenticate.");
    json_free(msg);
    json_free(msgs);
    json_free(raw);
    free(base);
}

static void test_identity(void) {
    setenv("PI_AI_ANTIGRAVITY_VERSION", "9.9.9", 1);
    char *ua = cca_antigravity_user_agent();
    CHECK(str_starts(ua, "antigravity/hub/9.9.9 (aidev_client; os_type="));
    CHECK(strstr(ua, "; cl=1005016104)") != NULL);
    free(ua);
    ua = cca_gemini_cli_user_agent("gemini-3-pro-preview");
    CHECK(str_starts(ua, "GeminiCLI/0.46.0/gemini-3-pro-preview ("));
    CHECK(str_ends(ua, "; terminal)"));
    free(ua);
    char *v = cca_parse_manifest_version("files:\n  - url: x\nversion: 2.31.4\npath: y\n");
    CHECK_STR(v, "2.31.4");
    free(v);
    v = cca_parse_manifest_version("version: '2.40.0' # latest\n");
    CHECK_STR(v, "2.40.0");
    free(v);
    v = cca_parse_manifest_version("version: banana\n");
    CHECK(v == NULL);
}

/* MALFORMED_RESPONSE: an error stop that keeps the chunk's usage; the shared classifier marks its finish message
 * transient on every transport (test_turn_recovery), so the stream attaches no errorId */
static void test_malformed_response_finish(void) {
    json_t *raw = json_parse_str("{\"id\":\"gemini-x\",\"api\":\"google-generative-ai\",\"provider\":\"google\"}");
    ai_model_t m;
    memset(&m, 0, sizeof m);
    m.id = "gemini-x";
    m.api = "google-generative-ai";
    m.provider = "google";
    m.raw = raw;
    static const char *const reasons[] = {"MALFORMED_RESPONSE", "MALFORMED_FUNCTION_CALL"};
    for (int i = 0; i < 2; i++) {
        google_sse_t s;
        google_sse_init(&s, &m, NULL, NULL);
        char *data = xasprintf("{\"candidates\":[{\"finishReason\":\"%s\"}],\"usageMetadata\":{\"promptTokenCount\":7,"
                               "\"candidatesTokenCount\":3,\"totalTokenCount\":10}}", reasons[i]);
        google_sse_event(&s, NULL, data, strlen(data));
        free(data);
        json_t *msg = google_sse_finish(&s, 0, NULL);
        CHECK(str_eq(json_get_str(msg, "stopReason"), "error"));
        char *want = xasprintf("Generation failed with finish reason: %s", reasons[i]);
        CHECK(str_eq(json_get_str(msg, "errorMessage"), want));
        free(want);
        CHECK(json_get_int(json_get(msg, "usage"), "input", 0) == 7);
        CHECK(json_get_int(json_get(msg, "usage"), "output", 0) == 3);
        CHECK(!json_get(msg, "errorId"));
        json_free(msg);
        google_sse_free(&s);
    }
    json_free(raw);
}

int main(void) {
    /* never touch real Google credentials */
    static const char *const vars[] = {"GOOGLE_APPLICATION_CREDENTIALS", "GOOGLE_CLOUD_ACCESS_TOKEN", "CLOUDSDK_AUTH_ACCESS_TOKEN",
                                       "GOOGLE_CLOUD_API_KEY", "GOOGLE_CLOUD_PROJECT", "GCP_PROJECT", "GCLOUD_PROJECT",
                                       "GOOGLE_VERTEX_LOCATION", "GOOGLE_CLOUD_LOCATION", "VERTEX_LOCATION",
                                       "PI_AI_GEMINI_CLI_VERSION", "PI_AI_ANTIGRAVITY_VERSION", "HTTPS_PROXY", "HTTP_PROXY",
                                       "https_proxy", "http_proxy", "ALL_PROXY", "all_proxy"};
    for (size_t i = 0; i < ARRAY_LEN(vars); i++) unsetenv(vars[i]);
    g_tmp = fs_mktempdir("omc-gtest");
    setenv("HOME", g_tmp, 1);
    setenv("GCE_METADATA_HOST", "127.0.0.1:1", 1);
    http_global_init();
    make_key();
    start_server();
    test_malformed_response_finish();
    test_jwt();
    test_adc();
    test_identity();
    test_cca_request();
    test_cca_sse();
    test_think_heal();
    test_cca_schema();
    test_discovery();
    test_cca_stream();
    fs_rm_rf(g_tmp);
    printf("test_providers_google: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
