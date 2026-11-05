/* google_auth.c - Application Default Credentials for Vertex AI; see google_auth.h. */
#define _GNU_SOURCE
#include "ai/google_auth.h"

#include "core/fs.h"
#include "net/http.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <pthread.h>
#include <time.h>

#define JWT_BEARER_GRANT "urn:ietf:params:oauth:grant-type:jwt-bearer"
#define METADATA_TOKEN_PATH "/computeMetadata/v1/instance/service-accounts/default/token"
#define IAM_CREDENTIALS_BASE "https://iamcredentials.googleapis.com/v1/projects/-/serviceAccounts/"
#define REQUEST_TIMEOUT_MS 30000
#define METADATA_TIMEOUT_MS 2000

/* ---------- JWT ---------- */

static char *b64url_str(const char *s) { return base64url_encode(s, strlen(s)); }

char *google_jwt_rs256(const json_t *claims, const char *pem, const char *key_id, char **err) {
    *err = NULL;
    if (!pem || !*pem) {
        *err = xstrdup("Invalid PEM: empty body");
        return NULL;
    }
    json_t *hdr = json_obj();
    json_set_str(hdr, "alg", "RS256");
    json_set_str(hdr, "typ", "JWT");
    if (key_id && *key_id) json_set_str(hdr, "kid", key_id);
    char *hj = json_dump(hdr, 0), *cj = json_dump(claims, 0);
    json_free(hdr);
    char *h64 = b64url_str(hj), *c64 = b64url_str(cj);
    free(hj);
    free(cj);
    char *signing_input = xasprintf("%s.%s", h64, c64);
    free(h64);
    free(c64);

    char *out = NULL;
    BIO *bio = BIO_new_mem_buf(pem, (int)strlen(pem));
    EVP_PKEY *key = bio ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL) : NULL;
    if (bio) BIO_free(bio);
    if (!key || EVP_PKEY_base_id(key) != EVP_PKEY_RSA) {
        *err = xstrdup("Invalid service account private key (expected an RSA PEM key)");
        if (key) EVP_PKEY_free(key);
        free(signing_input);
        return NULL;
    }
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    size_t siglen = 0;
    unsigned char *sig = NULL;
    bool ok = md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, key) == 1 &&
              EVP_DigestSignUpdate(md, signing_input, strlen(signing_input)) == 1 &&
              EVP_DigestSignFinal(md, NULL, &siglen) == 1;
    if (ok) {
        sig = xmalloc(siglen);
        ok = EVP_DigestSignFinal(md, sig, &siglen) == 1;
    }
    if (ok) {
        char *s64 = base64url_encode(sig, siglen);
        out = xasprintf("%s.%s", signing_input, s64);
        free(s64);
    } else {
        *err = xstrdup("RS256 signing failed");
    }
    free(sig);
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(key);
    free(signing_input);
    return out;
}

/* ---------- helpers ---------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

char *google_user_adc_path(void) { return xasprintf("%s/.config/gcloud/application_default_credentials.json", home_dir()); }

bool google_adc_file_exists(void) {
    const char *gac = env_get("GOOGLE_APPLICATION_CREDENTIALS");
    if (gac) return fs_exists(gac);
    char *p = google_user_adc_path();
    bool ok = fs_exists(p);
    free(p);
    return ok;
}

bool google_vertex_has_credentials(void) {
    if (env_get("GOOGLE_CLOUD_API_KEY")) return true;
    bool project = env_get("GOOGLE_CLOUD_PROJECT") || env_get("GCP_PROJECT") || env_get("GCLOUD_PROJECT");
    bool location = env_get("GOOGLE_VERTEX_LOCATION") || env_get("GOOGLE_CLOUD_LOCATION") || env_get("VERTEX_LOCATION");
    return project && location && google_adc_file_exists();
}

static void form_add(sb_t *b, const char *k, const char *v) {
    char *ek = url_encode(k), *ev = url_encode(v ? v : "");
    sb_appendf(b, "%s%s=%s", b->len ? "&" : "", ek, ev);
    free(ek);
    free(ev);
}

typedef struct {
    char *token;
    int64_t expires_in; /* seconds */
    char *refresh_token; /* replacement refresh token; STS may rotate workforce refresh tokens on exchange */
} token_t;

/* POST form -> {access_token, expires_in, refresh_token?}; authorization: optional Authorization header */
static int post_for_token(const char *url, const char *form, const char *what, const char *authorization, token_t *out,
                          char **err) {
    http_headers_t h = {0};
    http_headers_set(&h, "Content-Type", "application/x-www-form-urlencoded");
    if (authorization) http_headers_set(&h, "Authorization", authorization);
    http_req_t req = {.method = "POST", .url = url, .headers = &h, .body = form, .body_len = strlen(form),
                      .timeout_ms = REQUEST_TIMEOUT_MS};
    http_resp_t r;
    int rc = http_request(&req, &r);
    http_headers_free(&h);
    if (rc != 0) {
        *err = xasprintf("%s request to %s failed: %s", what, url, r.error ? r.error : "network error");
        http_resp_free(&r);
        return -1;
    }
    if (r.status < 200 || r.status >= 300) {
        *err = xasprintf("%s failed (%d): %s", what, r.status, r.body ? r.body : "");
        http_resp_free(&r);
        return -1;
    }
    json_t *j = json_parse_str(r.body ? r.body : "");
    http_resp_free(&r);
    const char *tok = json_get_str(j, "access_token");
    if (!tok || !*tok) {
        json_free(j);
        *err = xasprintf("%s returned no access_token", what);
        return -1;
    }
    out->token = xstrdup(tok);
    out->expires_in = json_get_int(j, "expires_in", 0);
    const char *rotated = json_get_str(j, "refresh_token");
    if (rotated && *rotated) out->refresh_token = xstrdup(rotated);
    json_free(j);
    return 0;
}

static const char *token_uri(const json_t *creds) {
    const char *u = json_get_str(creds, "token_uri");
    return u && *u ? u : GOOGLE_OAUTH_TOKEN_URL;
}

static int exchange_jwt(const json_t *creds, token_t *out, char **err) {
    const char *email = json_get_str(creds, "client_email");
    const char *key = json_get_str(creds, "private_key");
    if (!email || !key) {
        *err = xstrdup("service_account credentials are missing client_email or private_key");
        return -1;
    }
    int64_t now = now_ms() / 1000;
    json_t *claims = json_obj();
    json_set_str(claims, "iss", email);
    json_set_str(claims, "scope", GOOGLE_CLOUD_PLATFORM_SCOPE);
    json_set_str(claims, "aud", token_uri(creds));
    json_set_int(claims, "exp", now + 3600);
    json_set_int(claims, "iat", now);
    char *jwt = google_jwt_rs256(claims, key, json_get_str(creds, "private_key_id"), err);
    json_free(claims);
    if (!jwt) return -1;
    sb_t f = SB_INIT;
    form_add(&f, "grant_type", JWT_BEARER_GRANT);
    form_add(&f, "assertion", jwt);
    free(jwt);
    int rc = post_for_token(token_uri(creds), sb_cstr(&f), "Google OAuth token exchange", NULL, out, err);
    sb_free(&f);
    return rc;
}

static int exchange_refresh(const json_t *creds, token_t *out, char **err) {
    sb_t f = SB_INIT;
    form_add(&f, "client_id", json_get_str(creds, "client_id"));
    form_add(&f, "client_secret", json_get_str(creds, "client_secret"));
    form_add(&f, "refresh_token", json_get_str(creds, "refresh_token"));
    form_add(&f, "grant_type", "refresh_token");
    int rc = post_for_token(token_uri(creds), sb_cstr(&f), "Google OAuth token exchange", NULL, out, err);
    sb_free(&f);
    return rc;
}

/* Latest workforce refresh token, keyed by the token stored in the ADC file. The file is never rewritten when STS
 * rotates the token, so later refreshes must use the replacement; a re-login writes a new file token and bypasses
 * the stale entry. Guarded by g_mu (held across resolution). */
static hmap_t g_rotated_refresh;

/* external_account_authorized_user: refresh at the file's STS token URL with Basic client authentication */
static int exchange_workforce_refresh(const json_t *creds, token_t *out, char **err) {
    const char *file_token = json_get_str(creds, "refresh_token");
    const char *rotated = file_token ? hmap_get(&g_rotated_refresh, file_token) : NULL;
    sb_t f = SB_INIT;
    form_add(&f, "grant_type", "refresh_token");
    form_add(&f, "refresh_token", rotated ? rotated : file_token);
    const char *id = json_get_str(creds, "client_id"), *secret = json_get_str(creds, "client_secret");
    char *pair = xasprintf("%s:%s", id ? id : "", secret ? secret : "");
    char *b64 = base64_encode(pair, strlen(pair));
    char *authorization = xasprintf("Basic %s", b64);
    free(pair);
    free(b64);
    const char *url = json_get_str(creds, "token_url");
    int rc = post_for_token(url ? url : "", sb_cstr(&f), "Google OAuth token exchange", authorization, out, err);
    free(authorization);
    sb_free(&f);
    if (rc == 0 && out->refresh_token && file_token) {
        free(hmap_put(&g_rotated_refresh, file_token, out->refresh_token));
        out->refresh_token = NULL;
    }
    return rc;
}

static int exchange_source(const json_t *creds, token_t *out, char **err) {
    const char *type = json_get_str(creds, "type");
    if (str_eq(type, "service_account")) return exchange_jwt(creds, out, err);
    if (str_eq(type, "authorized_user")) return exchange_refresh(creds, out, err);
    *err = xasprintf("Unsupported ADC credential type: %s", type ? type : "undefined");
    return -1;
}

static int64_t parse_rfc3339_ms(const char *s) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    const char *rest = s ? strptime(s, "%Y-%m-%dT%H:%M:%S", &tm) : NULL;
    if (!rest) return 0;
    int64_t ms = (int64_t)timegm(&tm) * 1000;
    if (*rest == '.') {
        double frac = strtod(rest, NULL);
        ms += (int64_t)(frac * 1000);
    }
    return ms;
}

static int exchange_impersonated(const json_t *creds, token_t *out, char **err) {
    const char *url = json_get_str(creds, "service_account_impersonation_url");
    const char *colon = url ? strrchr(url, ':') : NULL;
    const char *slash = NULL;
    if (colon) {
        for (const char *p = colon; p > url; p--)
            if (p[-1] == '/') {
                slash = p;
                break;
            }
    }
    if (!colon || !slash || slash == colon ||
        (strcmp(colon, ":generateAccessToken") != 0 && strcmp(colon, ":generateIdToken") != 0)) {
        *err = xasprintf("Cannot extract target principal from %s", url ? url : "(missing service_account_impersonation_url)");
        return -1;
    }
    char *target = xstrndup(slash, (size_t)(colon - slash));
    token_t src = {0};
    if (exchange_source(json_get_obj(creds, "source_credentials"), &src, err) != 0) {
        free(target);
        return -1;
    }
    /* the file's own generateAccessToken URL is the canonical endpoint; build it for generateIdToken files */
    char *endpoint = !strcmp(colon, ":generateAccessToken") ? xstrdup(url)
                                                            : xasprintf(IAM_CREDENTIALS_BASE "%s:generateAccessToken", target);
    free(target);
    json_t *body = json_obj();
    json_t *dl = json_get_arr(creds, "delegates");
    json_set(body, "delegates", dl ? json_clone(dl) : json_arr());
    json_t *scope = json_arr();
    json_push_str(scope, GOOGLE_CLOUD_PLATFORM_SCOPE);
    json_set(body, "scope", scope);
    json_set_str(body, "lifetime", "3600s");
    char *bj = json_dump(body, 0);
    json_free(body);
    http_headers_t h = {0};
    http_headers_set(&h, "Content-Type", "application/json");
    char *bearer = xasprintf("Bearer %s", src.token);
    http_headers_set(&h, "Authorization", bearer);
    free(bearer);
    free(src.token);
    free(src.refresh_token);
    http_req_t req = {.method = "POST", .url = endpoint, .headers = &h, .body = bj, .body_len = strlen(bj),
                      .timeout_ms = REQUEST_TIMEOUT_MS};
    http_resp_t r;
    int rc = http_request(&req, &r);
    http_headers_free(&h);
    free(bj);
    int ret = -1;
    if (rc != 0) {
        *err = xasprintf("Google Impersonation request to %s failed: %s", endpoint, r.error ? r.error : "network error");
    } else if (r.status < 200 || r.status >= 300) {
        *err = xasprintf("Google Impersonation token exchange failed (%d): %s", r.status, r.body ? r.body : "");
    } else {
        json_t *j = json_parse_str(r.body ? r.body : "");
        const char *tok = json_get_str(j, "accessToken");
        if (tok && *tok) {
            out->token = xstrdup(tok);
            int64_t left = parse_rfc3339_ms(json_get_str(j, "expireTime")) - now_ms();
            out->expires_in = left > 0 ? left / 1000 : 0;
            ret = 0;
        } else {
            *err = xstrdup("Google Impersonation token exchange returned no accessToken");
        }
        json_free(j);
    }
    http_resp_free(&r);
    free(endpoint);
    return ret;
}

static int metadata_token(token_t *out) {
    const char *host = env_get("GCE_METADATA_HOST");
    char *url = xasprintf("http://%s" METADATA_TOKEN_PATH, host ? host : "metadata.google.internal");
    http_headers_t h = {0};
    http_headers_set(&h, "Metadata-Flavor", "Google");
    http_req_t req = {.method = "GET", .url = url, .headers = &h, .timeout_ms = METADATA_TIMEOUT_MS,
                      .connect_timeout_ms = METADATA_TIMEOUT_MS, .no_proxy = true};
    http_resp_t r;
    int rc = http_request(&req, &r);
    http_headers_free(&h);
    free(url);
    int ret = -1;
    if (rc == 0 && r.status >= 200 && r.status < 300) {
        json_t *j = json_parse_str(r.body ? r.body : "");
        const char *tok = json_get_str(j, "access_token");
        if (tok && *tok) {
            out->token = xstrdup(tok);
            out->expires_in = json_get_int(j, "expires_in", 0);
            ret = 0;
        }
        json_free(j);
    }
    http_resp_free(&r);
    return ret;
}

/* ---------- resolution + cache ---------- */

static char *g_token;
static int64_t g_expires_ms;

void google_auth_reset_cache(void) {
    pthread_mutex_lock(&g_mu);
    free(g_token);
    g_token = NULL;
    g_expires_ms = 0;
    hmap_free(&g_rotated_refresh, free);
    hmap_init(&g_rotated_refresh);
    pthread_mutex_unlock(&g_mu);
}

static int64_t refresh_skew_ms(void) {
    const char *v = env_get("GOOGLE_VERTEX_REFRESH_SKEW_MS");
    if (v) {
        char *end;
        long long n = strtoll(v, &end, 10);
        if (end != v && n > 0) return n;
    }
    return 60000;
}

static int resolve_uncached(token_t *out, char **err) {
    const char *gac = env_get("GOOGLE_APPLICATION_CREDENTIALS");
    char *path = gac ? xstrdup(gac) : google_user_adc_path();
    size_t n;
    char *raw = fs_read(path, &n);
    if (!raw && gac) {
        *err = xasprintf("GOOGLE_APPLICATION_CREDENTIALS points to a missing file: %s", gac);
        free(path);
        return -1;
    }
    if (raw) {
        json_t *creds = json_parse(raw, n, NULL);
        free(raw);
        int rc;
        if (!json_is_obj(creds)) {
            *err = xasprintf("Invalid Google credentials file %s (not a JSON object)", path);
            rc = -1;
        } else if (str_eq(json_get_str(creds, "type"), "impersonated_service_account")) {
            rc = exchange_impersonated(creds, out, err);
        } else if (str_eq(json_get_str(creds, "type"), "external_account_authorized_user")) {
            rc = exchange_workforce_refresh(creds, out, err);
        } else {
            rc = exchange_source(creds, out, err);
        }
        json_free(creds);
        free(path);
        return rc;
    }
    free(path);
    if (metadata_token(out) == 0) return 0;
    *err = xstrdup("Vertex AI requires Application Default Credentials. Set GOOGLE_APPLICATION_CREDENTIALS, run `gcloud auth "
                   "application-default login`, or run on a GCE/Cloud Run instance with a service account.");
    return -1;
}

char *google_vertex_access_token(char **err) {
    *err = NULL;
    const char *explicit_tok = env_get("GOOGLE_CLOUD_ACCESS_TOKEN");
    if (!explicit_tok) explicit_tok = env_get("CLOUDSDK_AUTH_ACCESS_TOKEN");
    if (explicit_tok) return xstrdup(explicit_tok);
    /* one resolver at a time: waiting callers find the fresh token in the cache */
    pthread_mutex_lock(&g_mu);
    char *r = NULL;
    if (g_token && g_expires_ms - refresh_skew_ms() > now_ms()) {
        r = xstrdup(g_token);
    } else {
        free(g_token);
        g_token = NULL;
        token_t t = {0};
        if (resolve_uncached(&t, err) == 0) {
            free(t.refresh_token);
            g_token = t.token;
            g_expires_ms = now_ms() + MAX((int64_t)0, t.expires_in * 1000);
            r = xstrdup(g_token);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}
