/* oauth.c - OAuth for remote MCP servers (port of omp mcp/oauth-discovery.ts + oauth-flow.ts core):
 * protected-resource metadata (RFC 9728) -> authorization-server metadata (RFC 8414 / OIDC discovery)
 * -> dynamic client registration (RFC 7591) unless a client id is configured -> authorization code
 * + PKCE through the shared login engine (config/auth.c), with the RFC 8707 `resource` indicator.
 * Credentials live in auth.json under "mcp:<server url>" with the descriptor needed to refresh. */
#define _GNU_SOURCE /* memmem, strcasestr, vasprintf: glibc before 2.38 declares them only with it */
#include "mcp.h"

#include "config/auth.h"
#include "net/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "core/compat_sys.h"

char *mcp_oauth_key(const char *url) { return xasprintf("mcp:%s", url ? url : ""); }

char *mcp_oauth_token(const char *url) {
    char *key = mcp_oauth_key(url);
    char *t = auth_has_credentials(key) ? auth_get_api_key(key) : NULL;
    free(key);
    return t;
}

bool mcp_oauth_has_credential(const char *url) {
    char *key = mcp_oauth_key(url);
    bool has = auth_has_credentials(key);
    free(key);
    return has;
}

int mcp_oauth_logout(const char *url) {
    char *key = mcp_oauth_key(url);
    int rc = auth_logout(key);
    free(key);
    return rc;
}

/* scheme://host[:port] of a URL (owned) and its path (borrowed into the URL) */
static char *origin_of(const char *url, const char **path) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    if (path) *path = slash ? slash : "";
    return xstrndup(url, slash ? (size_t)(slash - url) : strlen(url));
}

static json_t *get_json(const char *url) {
    http_headers_t h = {0};
    http_headers_add(&h, "Accept", "application/json");
    http_headers_add(&h, "MCP-Protocol-Version", "2025-06-18");
    http_resp_t r;
    int rc = http_get(url, &h, 15000, &r);
    http_headers_free(&h);
    json_t *j = NULL;
    if (rc == 0 && r.status >= 200 && r.status < 300 && r.body) j = json_parse(r.body, r.body_len, NULL);
    http_resp_free(&r);
    if (j && !json_is_obj(j)) {
        json_free(j);
        j = NULL;
    }
    return j;
}

/* resource_metadata="..." from a WWW-Authenticate challenge (owned) */
static char *challenge_param(const char *www, const char *name) {
    if (!www) return NULL;
    const char *p = strcasestr(www, name);
    while (p) {
        const char *e = p + strlen(name);
        while (*e == ' ') e++;
        if (*e == '=') {
            e++;
            while (*e == ' ') e++;
            if (*e == '"') {
                const char *q = strchr(e + 1, '"');
                return q ? xstrndup(e + 1, (size_t)(q - e - 1)) : NULL;
            }
            return xstrndup(e, strcspn(e, ", "));
        }
        p = strcasestr(e, name);
    }
    return NULL;
}

static json_t *protected_resource_metadata(const char *server_url, const char *www) {
    char *explicit_url = challenge_param(www, "resource_metadata");
    if (explicit_url) {
        json_t *j = get_json(explicit_url);
        free(explicit_url);
        if (j) return j;
    }
    const char *path;
    char *origin = origin_of(server_url, &path);
    json_t *j = NULL;
    if (*path && strcmp(path, "/")) {
        char *u = xasprintf("%s/.well-known/oauth-protected-resource%s", origin, path);
        j = get_json(u);
        free(u);
    }
    if (!j) {
        char *u = xasprintf("%s/.well-known/oauth-protected-resource", origin);
        j = get_json(u);
        free(u);
    }
    free(origin);
    return j;
}

static json_t *authorization_server_metadata(const char *issuer) {
    const char *path;
    char *origin = origin_of(issuer, &path);
    bool has_path = *path && strcmp(path, "/");
    char *cands[4];
    size_t n = 0;
    if (has_path) cands[n++] = xasprintf("%s/.well-known/oauth-authorization-server%s", origin, path);
    cands[n++] = xasprintf("%s/.well-known/oauth-authorization-server", origin);
    if (has_path) cands[n++] = xasprintf("%s/.well-known/openid-configuration%s", origin, path);
    cands[n++] = has_path ? xasprintf("%s%s/.well-known/openid-configuration", origin, path)
                          : xasprintf("%s/.well-known/openid-configuration", origin);
    json_t *j = NULL;
    for (size_t i = 0; i < n; i++) {
        if (!j) j = get_json(cands[i]);
        free(cands[i]);
    }
    free(origin);
    return j;
}

static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    int port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(fd, (struct sockaddr *)&a, &al) == 0) port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static int register_client(const char *endpoint, const char *redirect, const char *scope, char **client_id, char **client_secret,
                           char **err) {
    json_t *body = json_obj();
    json_set_str(body, "client_name", "omc");
    json_t *ru = json_arr();
    json_push_str(ru, redirect);
    json_set(body, "redirect_uris", ru);
    json_set(body, "grant_types", json_parse_str("[\"authorization_code\",\"refresh_token\"]"));
    json_set(body, "response_types", json_parse_str("[\"code\"]"));
    json_set_str(body, "token_endpoint_auth_method", "none");
    if (scope && *scope) json_set_str(body, "scope", scope);
    char *txt = json_dump(body, 0);
    json_free(body);
    http_headers_t h = {0};
    http_headers_add(&h, "Content-Type", "application/json");
    http_headers_add(&h, "Accept", "application/json");
    http_req_t req = {.method = "POST", .url = endpoint, .headers = &h, .body = txt, .body_len = strlen(txt), .timeout_ms = 15000};
    http_resp_t r;
    int rc = http_request(&req, &r);
    free(txt);
    http_headers_free(&h);
    json_t *j = rc == 0 && r.status >= 200 && r.status < 300 && r.body ? json_parse(r.body, r.body_len, NULL) : NULL;
    if (!json_get_str(j, "client_id")) {
        *err = rc != 0 ? xasprintf("client registration failed: %s", r.error ? r.error : "request failed")
                       : xasprintf("client registration failed: HTTP %d %.200s", r.status, r.body ? r.body : "");
        json_free(j);
        http_resp_free(&r);
        return -1;
    }
    *client_id = xstrdup(json_get_str(j, "client_id"));
    *client_secret = json_get_str(j, "client_secret") ? xstrdup(json_get_str(j, "client_secret")) : NULL;
    json_free(j);
    http_resp_free(&r);
    return 0;
}

json_t *mcp_oauth_descriptor(const char *server_url, const json_t *server_cfg, const char *www_authenticate, char **err) {
    json_t *oc = json_get_obj(server_cfg, "oauth");
    json_t *prm = protected_resource_metadata(server_url, www_authenticate);
    const json_t *iss0 = json_at(json_get_arr(prm, "authorization_servers"), 0);
    const char *issuer = json_is_str(iss0) ? json_sv(iss0) : NULL;
    char *origin = origin_of(server_url, NULL);
    json_t *asm_ = authorization_server_metadata(issuer ? issuer : origin);
    const char *authz = json_get_str(oc, "authorizationUrl");
    if (!authz) authz = json_get_str(asm_, "authorization_endpoint");
    const char *token = json_get_str(oc, "tokenUrl");
    if (!token) token = json_get_str(asm_, "token_endpoint");
    json_t *d = NULL;
    if (!authz || !token) {
        *err = xasprintf("MCP server %s: no OAuth authorization server metadata found", server_url);
        goto out;
    }
    /* scopes: configured, else the resource's, else the server's */
    sb_t scope = SB_INIT;
    json_t *sc = json_get_arr(oc, "scopes");
    if (!sc) sc = json_get_arr(prm, "scopes_supported");
    json_t *x;
    json_arr_foreach(sc, i, x) if (json_is_str(x)) sb_appendf(&scope, "%s%s", scope.len ? " " : "", json_sv(x));
    if (!scope.len && json_get_str(oc, "scope")) sb_append(&scope, json_get_str(oc, "scope"));
    int port = (int)json_get_int(oc, "callbackPort", 0);
    if (!port) port = free_port();
    const char *cpath = json_get_str(oc, "callbackPath") ? json_get_str(oc, "callbackPath") : "/callback";
    char *redirect = xasprintf("http://127.0.0.1:%d%s", port, cpath);
    char *client_id = json_get_str(oc, "clientId") ? xstrdup(json_get_str(oc, "clientId")) : NULL;
    char *client_secret = json_get_str(oc, "clientSecret") ? xstrdup(json_get_str(oc, "clientSecret")) : NULL;
    const char *reg = json_get_str(asm_, "registration_endpoint");
    if (!client_id) {
        if (!reg) {
            *err = xasprintf("MCP server %s supports no dynamic client registration; set oauth.clientId in its config", server_url);
            free(redirect);
            sb_free(&scope);
            goto out;
        }
        if (register_client(reg, redirect, sb_cstr(&scope), &client_id, &client_secret, err) != 0) {
            free(redirect);
            sb_free(&scope);
            goto out;
        }
    }
    const char *resource = json_get_str(prm, "resource") ? json_get_str(prm, "resource") : server_url;
    d = json_obj();
    json_set_str(d, "id", "mcp");
    json_set_str(d, "name", server_url);
    json_t *login = json_obj();
    json_set_str(login, "kind", "oauth-code");
    json_set_str(login, "clientId", client_id);
    json_set_str(login, "authorizeUrl", authz);
    json_t *cb = json_obj();
    json_set_int(cb, "port", port);
    json_set_str(cb, "hostname", "127.0.0.1");
    json_set_str(cb, "path", cpath);
    json_set_str(cb, "redirectUri", redirect);
    json_set(login, "callback", cb);
    json_t *scopes = json_arr();
    json_arr_foreach(sc, i, x) if (json_is_str(x)) json_push_str(scopes, json_sv(x));
    json_set(login, "scopes", scopes);
    json_t *ap = json_obj();
    json_set_str(ap, "resource", resource);
    json_set(login, "authorizeParams", ap);
    json_t *tok = json_obj();
    json_set_str(tok, "url", token);
    json_set_str(tok, "body", "form");
    json_t *tp = json_obj();
    json_set_str(tp, "resource", resource);
    if (client_secret) json_set_str(tp, "client_secret", client_secret);
    json_set(tok, "params", tp);
    json_set(login, "token", tok);
    json_set(login, "credential", json_parse_str("{\"access\":{\"path\":\"access_token\"},\"refresh\":{\"path\":\"refresh_token\"},"
                                                 "\"expires\":{\"path\":\"expires_in\",\"mode\":\"seconds\",\"skewMs\":60000}}"));
    json_set(d, "login", login);
    json_t *rf = json_obj();
    json_set_str(rf, "kind", "request");
    json_set(rf, "token", json_clone(tok));
    json_set(d, "refresh", rf);
    free(redirect);
    free(client_id);
    free(client_secret);
    sb_free(&scope);
out:
    free(origin);
    json_free(prm);
    json_free(asm_);
    return d;
}

json_t *mcp_oauth_discover(const char *server_url, const char *www_authenticate) {
    if (!server_url) return NULL;
    json_t *prm = protected_resource_metadata(server_url, www_authenticate);
    const json_t *iss0 = json_at(json_get_arr(prm, "authorization_servers"), 0);
    const char *issuer = json_is_str(iss0) ? json_sv(iss0) : NULL;
    char *origin = origin_of(server_url, NULL);
    json_t *asm_ = authorization_server_metadata(issuer ? issuer : origin);
    json_t *out = NULL;
    const char *authz = json_get_str(asm_, "authorization_endpoint"), *token = json_get_str(asm_, "token_endpoint");
    if (authz && token) {
        out = json_obj();
        json_set_str(out, "authorizationUrl", authz);
        json_set_str(out, "tokenUrl", token);
        if (json_get_str(asm_, "registration_endpoint")) json_set_str(out, "registrationUrl", json_get_str(asm_, "registration_endpoint"));
        if (json_get_str(asm_, "issuer")) json_set_str(out, "issuerUrl", json_get_str(asm_, "issuer"));
        sb_t sc = SB_INIT;
        json_t *x;
        json_arr_foreach(json_get_arr(prm, "scopes_supported"), i, x)
            if (json_is_str(x)) sb_appendf(&sc, "%s%s", sc.len ? " " : "", json_sv(x));
        if (sc.len) json_set_str(out, "scopes", sb_cstr(&sc));
        sb_free(&sc);
        if (json_get_str(prm, "resource")) json_set_str(out, "resource", json_get_str(prm, "resource"));
    }
    free(origin);
    json_free(prm);
    json_free(asm_);
    return out;
}

int mcp_oauth_login_hooked(const char *server_url, const json_t *server_cfg, const char *www_authenticate,
                           const auth_login_hooks_t *hooks, char **err) {
    json_t *d = mcp_oauth_descriptor(server_url, server_cfg, www_authenticate, err);
    if (!d) return -1;
    char *key = mcp_oauth_key(server_url);
    int rc = auth_login_descriptor_hooked(key, d, hooks, err);
    free(key);
    json_free(d);
    return rc;
}

int mcp_oauth_login(const char *server_url, const json_t *server_cfg, const char *www_authenticate, const auth_io_t *io, char **err) {
    json_t *d = mcp_oauth_descriptor(server_url, server_cfg, www_authenticate, err);
    if (!d) return -1;
    char *key = mcp_oauth_key(server_url);
    int rc = auth_login_descriptor(key, d, io, err);
    free(key);
    json_free(d);
    return rc;
}
