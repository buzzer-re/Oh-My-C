/* auth_cursor.c - Cursor login (custom hook "cursor"): browser deep-control login polled with a PKCE verifier,
 * token refresh and account email lookup (port of omp packages/ai/src/registry/oauth/cursor.ts). */
#define _GNU_SOURCE
#include "auth.h"

#include "core/proc.h"
#include "net/http.h"

#include <openssl/sha.h>

#define CURSOR_LOGIN_URL "https://cursor.com/loginDeepControl"
#define CURSOR_API_BASE "https://api2.cursor.sh"
#define CURSOR_CLIENT_ID "KbZUR41cY7W6zRSdpSUJ7I7mLYBKOCmB"
#define POLL_MAX_ATTEMPTS 150
#define POLL_BASE_DELAY 1000
#define POLL_MAX_DELAY 10000
#define CURSOR_PROFILE_TIMEOUT_MS 3000

/* test hook: point the poll/refresh/profile endpoints at a mock server */
static const char *api_base(void) {
    const char *b = env_get("OMC_TEST_CURSOR_API");
    return b ? b : CURSOR_API_BASE;
}

static bool which_exists(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return false;
    strvec_t dirs = str_split(path, ":");
    bool found = false;
    for (size_t i = 0; i < dirs.n && !found; i++) {
        char *p = xasprintf("%s/%s", *dirs.v[i] ? dirs.v[i] : ".", name);
        found = access(p, X_OK) == 0;
        free(p);
    }
    strvec_free(&dirs);
    return found;
}

/* JWT payload (owned JSON) or NULL */
static json_t *jwt_payload(const char *token) {
    if (!token) return NULL;
    const char *a = strchr(token, '.');
    const char *b = a ? strchr(a + 1, '.') : NULL;
    if (!a || !b || strchr(b + 1, '.') || b == a + 1) return NULL;
    sb_t s = SB_INIT;
    for (const char *p = a + 1; p < b; p++) sb_putc(&s, *p == '-' ? '+' : *p == '_' ? '/' : *p);
    while (s.len % 4) sb_putc(&s, '=');
    size_t n = 0;
    unsigned char *raw = base64_decode(s.s, s.len, &n);
    sb_free(&s);
    json_t *j = raw ? json_parse((const char *)raw, n, NULL) : NULL;
    free(raw);
    if (!json_is_obj(j)) {
        json_free(j);
        return NULL;
    }
    return j;
}

/* exp*1000 - 5 min, else now + 1 h (getTokenExpiry) */
static int64_t token_expiry(const char *token) {
    json_t *p = jwt_payload(token);
    json_t *exp = p ? json_get(p, "exp") : NULL;
    int64_t r = json_is_num(exp) ? (int64_t)(json_get_num(p, "exp", 0) * 1000) - 5 * 60 * 1000 : now_ms() + 3600 * 1000;
    json_free(p);
    return r;
}

/* full `sub` claim, owned or NULL */
static char *token_subject(const char *token) {
    json_t *p = jwt_payload(token);
    const char *sub = p ? json_get_str(p, "sub") : NULL;
    char *r = sub ? str_trim(sub) : NULL;
    json_free(p);
    if (r && !*r) {
        free(r);
        r = NULL;
    }
    return r;
}

/* account email from DashboardService/GetMe (Connect unary JSON, bearer auth); NULL on any failure */
static char *account_email(const char *access) {
    char *subject = token_subject(access);
    if (!subject) return NULL;
    char *url = xasprintf("%s/aiserver.v1.DashboardService/GetMe", api_base());
    http_headers_t h = {0};
    char *auth = xasprintf("Bearer %s", access);
    http_headers_add(&h, "Accept", "application/json");
    http_headers_add(&h, "Authorization", auth);
    http_headers_add(&h, "Connect-Protocol-Version", "1");
    http_headers_add(&h, "Content-Type", "application/json");
    http_req_t req = {.method = "POST", .url = url, .headers = &h, .body = "{}", .body_len = 2,
                      .timeout_ms = CURSOR_PROFILE_TIMEOUT_MS, .connect_timeout_ms = CURSOR_PROFILE_TIMEOUT_MS};
    http_resp_t resp;
    char *email = NULL;
    if (http_request(&req, &resp) == 0 && resp.status >= 200 && resp.status < 300 && resp.body) {
        json_t *j = json_parse(resp.body, resp.body_len, NULL);
        const char *auth_id = j ? json_get_str(j, "authId") : NULL;
        const char *e = j ? json_get_str(j, "email") : NULL;
        if (auth_id && e && strcmp(auth_id, subject) == 0) {
            email = str_trim(e);
            if (!*email) {
                free(email);
                email = NULL;
            }
        }
        json_free(j);
    }
    http_resp_free(&resp);
    http_headers_free(&h);
    free(auth);
    free(url);
    free(subject);
    return email;
}

static json_t *make_cred(const char *access, const char *refresh, const char *email) {
    json_t *c = json_obj();
    json_set_str(c, "type", "oauth");
    json_set_str(c, "access", access);
    json_set_str(c, "refresh", refresh ? refresh : "");
    json_set_int(c, "expires", token_expiry(access));
    if (email) json_set_str(c, "email", email);
    return c;
}

int auth_cursor_login(void (*on_msg)(void *ud, const char *msg), void (*on_url)(void *ud, const char *url),
                      volatile int *abort, void *ud, json_t **cred_out, char **err) {
    *cred_out = NULL;
    *err = NULL;
    unsigned char vb[96];
    rand_bytes(vb, sizeof vb);
    char *verifier = base64url_encode(vb, sizeof vb);
    unsigned char dig[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)verifier, strlen(verifier), dig);
    char *challenge = base64url_encode(dig, sizeof dig);
    char *uuid = uuid_v4();
    char *e_ch = url_encode(challenge);
    char *login_url = xasprintf("%s?challenge=%s&uuid=%s&mode=login&redirectTarget=cli", CURSOR_LOGIN_URL, e_ch, uuid);
    free(e_ch);
    if (on_url) on_url(ud, login_url);
    if (on_msg && !on_url) {
        char *m = xasprintf("Open this URL to log in to Cursor:\n\n%s\n", login_url);
        on_msg(ud, m);
        free(m);
    }
    const char *opener = env_truthy("OMC_NO_BROWSER") ? NULL
                         : which_exists("xdg-open")   ? "xdg-open"
                         : which_exists("open")       ? "open"
                                                      : NULL;
    if (opener) {
        const char *argv[] = {opener, login_url, NULL};
        proc_opts_t po = {.timeout_ms = 5000, .new_session = true};
        proc_result_t pr;
        if (proc_run(argv, &po, &pr) == 0) proc_result_free(&pr);
    }
    if (on_msg) on_msg(ud, "Waiting for browser authentication...");

    int64_t delay = POLL_BASE_DELAY;
    int consecutive_errors = 0;
    char *access = NULL, *refresh = NULL;
    for (int attempt = 0; attempt < POLL_MAX_ATTEMPTS && !access; attempt++) {
        for (int64_t t = 0; t < delay && !(abort && *abort); t += 50) sleep_ms(50);
        if (abort && *abort) {
            *err = xstrdup("Login cancelled");
            break;
        }
        char *url = xasprintf("%s/auth/poll?uuid=%s&verifier=%s", api_base(), uuid, verifier);
        http_resp_t resp;
        int rc = http_get(url, NULL, 30000, &resp);
        free(url);
        if (rc == 0 && resp.status == 404) {
            consecutive_errors = 0;
            delay = MIN((int64_t)(delay * 1.2), POLL_MAX_DELAY);
        } else if (rc == 0 && resp.status >= 200 && resp.status < 300) {
            json_t *j = resp.body ? json_parse(resp.body, resp.body_len, NULL) : NULL;
            const char *a = j ? json_get_str(j, "accessToken") : NULL;
            if (a) {
                access = xstrdup(a);
                const char *r = json_get_str(j, "refreshToken");
                refresh = xstrdup(r ? r : "");
            } else {
                consecutive_errors++;
            }
            json_free(j);
        } else {
            consecutive_errors++;
        }
        http_resp_free(&resp);
        if (!access && consecutive_errors >= 3) {
            *err = xstrdup("Too many consecutive errors during Cursor auth polling");
            break;
        }
    }
    if (!access && !*err) *err = xstrdup("Cursor authentication polling timeout");
    if (access) {
        char *email = account_email(access);
        *cred_out = make_cred(access, refresh, email);
        free(email);
    }
    free(access);
    free(refresh);
    free(login_url);
    free(uuid);
    free(challenge);
    free(verifier);
    return *cred_out ? 0 : -1;
}

json_t *auth_cursor_refresh(const json_t *entry, char **err) {
    *err = NULL;
    const char *rt = json_get_str(entry, "refresh");
    if (!rt || !*rt) {
        *err = xstrdup("no refresh token stored");
        return NULL;
    }
    json_t *body = json_obj();
    json_set_str(body, "grant_type", "refresh_token");
    json_set_str(body, "client_id", CURSOR_CLIENT_ID);
    json_set_str(body, "refresh_token", rt);
    char *b = json_dump(body, 0);
    json_free(body);
    char *url = xasprintf("%s/oauth/token", api_base());
    http_headers_t h = {0};
    http_headers_add(&h, "Content-Type", "application/json");
    http_resp_t resp;
    int rc = http_post_json(url, &h, b, 10000, &resp);
    free(url);
    free(b);
    http_headers_free(&h);
    json_t *cred = NULL;
    if (rc != 0 || resp.status < 200 || resp.status >= 300) {
        *err = xasprintf("Cursor token refresh failed: %s", rc != 0 ? (resp.error ? resp.error : "network error")
                                                                    : (resp.body ? resp.body : ""));
    } else {
        json_t *j = resp.body ? json_parse(resp.body, resp.body_len, NULL) : NULL;
        json_t *logout = j ? json_get(j, "shouldLogout") : NULL;
        const char *at = j ? json_get_str(j, "access_token") : NULL;
        const char *nr = j ? json_get_str(j, "refresh_token") : NULL;
        if (json_is_bool(logout) && logout->u.b) {
            *err = xstrdup("invalid_grant: Cursor ended this session; run /login cursor again");
        } else if (!at || !*at) {
            *err = xstrdup("Cursor token refresh returned no access token");
        } else {
            /* Cursor answers only an access token; keep the stored refresh token unless rotated */
            const char *email = json_get_str(entry, "email");
            char *fetched = email ? NULL : account_email(at);
            cred = make_cred(at, nr && *nr ? nr : rt, email ? email : fetched);
            free(fetched);
        }
        json_free(j);
    }
    http_resp_free(&resp);
    return cred;
}
