#define _GNU_SOURCE
#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>
#include "core/compat_sys.h"

/* ---------- URL ---------- */

void url_free(url_t *u) {
    free(u->scheme);
    free(u->host);
    free(u->path);
    free(u->userinfo);
    free(u->fragment);
    memset(u, 0, sizeof(*u));
}

int url_parse(const char *s, url_t *u) {
    memset(u, 0, sizeof(*u));
    const char *p = strstr(s, "://");
    if (!p) return -1;
    {
        char *t = xstrndup(s, (size_t)(p - s));
        u->scheme = str_lower(t);
        free(t);
    }
    p += 3;
    const char *auth_end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(auth_end - p));
    if (at) {
        u->userinfo = xstrndup(p, (size_t)(at - p));
        p = at + 1;
    }
    const char *host_end;
    if (*p == '[') {
        const char *rb = memchr(p, ']', (size_t)(auth_end - p));
        if (!rb) { url_free(u); return -1; }
        u->host = xstrndup(p + 1, (size_t)(rb - p - 1));
        host_end = rb + 1;
    } else {
        const char *colon = memchr(p, ':', (size_t)(auth_end - p));
        host_end = colon ? colon : auth_end;
        u->host = xstrndup(p, (size_t)(host_end - p));
    }
    for (char *h = u->host; *h; h++) *h = (char)tolower((unsigned char)*h);
    if (*host_end == ':' && host_end + 1 < auth_end) {
        u->port = atoi(host_end + 1);
        if (u->port <= 0 || u->port > 65535) { url_free(u); return -1; }
    } else {
        bool secure = strcmp(u->scheme, "https") == 0 || strcmp(u->scheme, "wss") == 0;
        u->port = secure ? 443 : 80;
    }
    if (!*u->host) { url_free(u); return -1; }
    const char *rest = auth_end;
    const char *hash = strchr(rest, '#');
    size_t plen = hash ? (size_t)(hash - rest) : strlen(rest);
    if (hash) u->fragment = xstrdup(hash + 1);
    if (plen == 0 || rest[0] != '/') {
        char *tmp = xstrndup(rest, plen);
        u->path = xasprintf("/%s", tmp);
        free(tmp);
    } else {
        u->path = xstrndup(rest, plen);
    }
    return 0;
}

char *url_resolve(const char *base, const char *ref) {
    if (strstr(ref, "://")) return xstrdup(ref);
    url_t b;
    if (url_parse(base, &b) != 0) return xstrdup(ref);
    bool def_port = (strcmp(b.scheme, "https") == 0 && b.port == 443) || (strcmp(b.scheme, "http") == 0 && b.port == 80);
    char *origin = def_port ? xasprintf("%s://%s", b.scheme, b.host) : xasprintf("%s://%s:%d", b.scheme, b.host, b.port);
    char *r;
    if (ref[0] == '/' && ref[1] == '/') {
        r = xasprintf("%s:%s", b.scheme, ref);
    } else if (ref[0] == '/') {
        r = xasprintf("%s%s", origin, ref);
    } else if (ref[0] == '?') {
        char *q = xstrndup(b.path, strcspn(b.path, "?"));
        r = xasprintf("%s%s%s", origin, q, ref);
        free(q);
    } else {
        char *dir = xstrndup(b.path, strcspn(b.path, "?"));
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0;
        r = xasprintf("%s%s%s", origin, dir, ref);
        free(dir);
    }
    free(origin);
    url_free(&b);
    return r;
}

char *url_encode(const char *s) {
    sb_t b = SB_INIT;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') sb_putc(&b, (char)*p);
        else sb_appendf(&b, "%%%02X", *p);
    }
    return sb_detach(&b);
}

char *url_decode(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = {p[1], p[2], 0};
            sb_putc(&b, (char)strtol(h, NULL, 16));
            p += 2;
        } else if (*p == '+') {
            sb_putc(&b, ' ');
        } else {
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

/* ---------- headers ---------- */

void http_headers_add(http_headers_t *h, const char *name, const char *value) {
    http_header_t e = {xstrdup(name), xstrdup(value ? value : "")};
    vec_push(h, e);
}

void http_headers_set(http_headers_t *h, const char *name, const char *value) {
    for (size_t i = 0; i < h->n; i++) {
        if (strcasecmp(h->v[i].name, name) == 0) {
            free(h->v[i].value);
            h->v[i].value = xstrdup(value ? value : "");
            return;
        }
    }
    http_headers_add(h, name, value);
}

void http_headers_del(http_headers_t *h, const char *name) {
    for (size_t i = 0; i < h->n;) {
        if (strcasecmp(h->v[i].name, name) == 0) {
            free(h->v[i].name);
            free(h->v[i].value);
            memmove(&h->v[i], &h->v[i + 1], (h->n - i - 1) * sizeof h->v[0]);
            h->n--;
        } else {
            i++;
        }
    }
}

const char *http_headers_get(const http_headers_t *h, const char *name) {
    if (!h) return NULL;
    for (size_t i = 0; i < h->n; i++)
        if (strcasecmp(h->v[i].name, name) == 0) return h->v[i].value;
    return NULL;
}

void http_headers_free(http_headers_t *h) {
    for (size_t i = 0; i < h->n; i++) {
        free(h->v[i].name);
        free(h->v[i].value);
    }
    vec_free(h);
}

/* ---------- TLS context ---------- */

static SSL_CTX *g_ctx;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void init_ctx(void) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, NULL);
    g_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ctx) die("SSL_CTX_new failed");
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_default_verify_paths(g_ctx);
    const char *extra = env_get("OMC_EXTRA_CA_CERTS");
    if (!extra) extra = env_get("NODE_EXTRA_CA_CERTS");
    if (!extra) extra = env_get("SSL_CERT_FILE");
    if (extra) SSL_CTX_load_verify_locations(g_ctx, extra, NULL);
    SSL_CTX_set_mode(g_ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_session_cache_mode(g_ctx, SSL_SESS_CACHE_CLIENT);
    static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    SSL_CTX_set_alpn_protos(g_ctx, alpn, sizeof alpn);
}

void http_global_init(void) { pthread_once(&g_once, init_ctx); }

/* ---------- connections ---------- */

struct net_conn {
    int fd;
    SSL *ssl;
    char *key;
    char rbuf[16384];
    size_t rpos, rlen;
    int64_t idle_since;
};

int net_fd(net_conn_t *c) { return c->fd; }

static int wait_fd(int fd, short events, int64_t timeout_ms) {
    struct pollfd pf = {fd, events, 0};
    for (;;) {
        int r = poll(&pf, 1, timeout_ms < 0 ? -1 : (int)MIN(timeout_ms, (int64_t)INT32_MAX));
        if (r < 0 && errno == EINTR) continue;
        return r; /* 0 timeout, >0 ready */
    }
}

static int tcp_connect(const char *host, int port, int64_t timeout_ms, char **err) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    int gai = getaddrinfo(host, ps, &hints, &res);
    if (gai != 0) {
        if (err) *err = xasprintf("DNS lookup failed for %s: %s", host, gai_strerror(gai));
        return -1;
    }
    int fd = -1, last_errno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
        if (fd < 0) continue;
        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            if (wait_fd(fd, POLLOUT, timeout_ms) > 0) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr == 0) r = 0;
                else last_errno = soerr;
            } else {
                last_errno = ETIMEDOUT;
            }
        } else if (r != 0) {
            last_errno = errno;
        }
        if (r == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        if (err) *err = xasprintf("connect to %s:%d failed: %s", host, port, strerror(last_errno ? last_errno : ECONNREFUSED));
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    return fd;
}

static char *ssl_errstr(void) {
    unsigned long e = ERR_get_error();
    char buf[256];
    if (!e) return xstrdup("unknown TLS error");
    ERR_error_string_n(e, buf, sizeof buf);
    ERR_clear_error();
    return xstrdup(buf);
}

/* the error code Bun/Node give a rejected certificate (the X509_V_ERR_* name), which omp's network-cause.ts shows in
 * "TLS certificate verification failed for <host> (<code>)"; NULL for the rare ones, which keep OpenSSL's text */
static const char *tls_verify_code(long vr) {
    switch (vr) {
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT: return "DEPTH_ZERO_SELF_SIGNED_CERT";
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN: return "SELF_SIGNED_CERT_IN_CHAIN";
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY: return "UNABLE_TO_GET_ISSUER_CERT_LOCALLY";
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT: return "UNABLE_TO_GET_ISSUER_CERT";
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE: return "UNABLE_TO_VERIFY_LEAF_SIGNATURE";
    case X509_V_ERR_CERT_HAS_EXPIRED: return "CERT_HAS_EXPIRED";
    case X509_V_ERR_CERT_NOT_YET_VALID: return "CERT_NOT_YET_VALID";
    case X509_V_ERR_CERT_REVOKED: return "CERT_REVOKED";
    case X509_V_ERR_CERT_UNTRUSTED: return "CERT_UNTRUSTED";
    case X509_V_ERR_CERT_REJECTED: return "CERT_REJECTED";
    case X509_V_ERR_CERT_SIGNATURE_FAILURE: return "CERT_SIGNATURE_FAILURE";
    case X509_V_ERR_CERT_CHAIN_TOO_LONG: return "CERT_CHAIN_TOO_LONG";
    case X509_V_ERR_INVALID_CA: return "INVALID_CA";
    case X509_V_ERR_HOSTNAME_MISMATCH: return "ERR_TLS_CERT_ALTNAME_INVALID";
    default: return NULL;
    }
}

static int tls_handshake(net_conn_t *c, const char *host, int64_t timeout_ms, char **err) {
    http_global_init();
    c->ssl = SSL_new(g_ctx);
    SSL_set_fd(c->ssl, c->fd);
    bool is_ip = strchr(host, ':') != NULL || strspn(host, "0123456789.") == strlen(host);
    if (!is_ip) SSL_set_tlsext_host_name(c->ssl, host);
    if (env_truthy("OMC_INSECURE_TLS")) {
        SSL_set_verify(c->ssl, SSL_VERIFY_NONE, NULL);
    } else if (is_ip) {
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(c->ssl), host);
    } else {
        SSL_set1_host(c->ssl, host);
    }
    int64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        int r = SSL_connect(c->ssl);
        if (r == 1) return 0;
        int e = SSL_get_error(c->ssl, r);
        int64_t left = deadline - mono_ms();
        if (left <= 0) {
            if (err) *err = xstrdup("TLS handshake timed out");
            return -1;
        }
        if (e == SSL_ERROR_WANT_READ) wait_fd(c->fd, POLLIN, left);
        else if (e == SSL_ERROR_WANT_WRITE) wait_fd(c->fd, POLLOUT, left);
        else {
            long vr = SSL_get_verify_result(c->ssl);
            if (err) {
                if (vr != X509_V_OK) {
                    const char *code = tls_verify_code(vr);
                    *err = xasprintf("TLS certificate verification failed for %s: %s", host, code ? code : X509_verify_cert_error_string(vr));
                }
                else {
                    char *s = ssl_errstr();
                    *err = xasprintf("TLS handshake with %s failed: %s", host, s);
                    free(s);
                }
            }
            return -1;
        }
    }
}

static net_conn_t *conn_new(int fd) {
    net_conn_t *c = xcalloc(1, sizeof(*c));
    c->fd = fd;
    return c;
}

ssize_t net_read(net_conn_t *c, void *buf, size_t n, int64_t timeout_ms) {
    if (c->rpos < c->rlen) {
        size_t k = MIN(n, c->rlen - c->rpos);
        memcpy(buf, c->rbuf + c->rpos, k);
        c->rpos += k;
        return (ssize_t)k;
    }
    int64_t deadline = timeout_ms >= 0 ? mono_ms() + timeout_ms : 0;
    for (;;) {
        if (c->ssl) {
            int r = SSL_read(c->ssl, buf, (int)MIN(n, (size_t)INT32_MAX));
            if (r > 0) return r;
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN) return 0;
            if (e == SSL_ERROR_SYSCALL && r == 0) { ERR_clear_error(); return 0; }
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) { ERR_clear_error(); return -1; }
            int64_t left = timeout_ms >= 0 ? deadline - mono_ms() : 1000;
            if (timeout_ms >= 0 && left <= 0) return -2;
            int w = wait_fd(c->fd, e == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN, MIN(left, 250));
            if (w < 0) return -1;
            if (w == 0) return -3; /* poll slice elapsed: let caller check cancellation */
        } else {
            ssize_t r = recv(c->fd, buf, n, 0);
            if (r >= 0) return r;
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
            int64_t left = timeout_ms >= 0 ? deadline - mono_ms() : 1000;
            if (timeout_ms >= 0 && left <= 0) return -2;
            int w = wait_fd(c->fd, POLLIN, MIN(left, 250));
            if (w < 0) return -1;
            if (w == 0) return -3;
        }
    }
}

int net_write_all(net_conn_t *c, const void *buf, size_t n) {
    const char *p = buf;
    int64_t deadline = mono_ms() + 60000;
    while (n) {
        if (c->ssl) {
            int r = SSL_write(c->ssl, p, (int)MIN(n, (size_t)INT32_MAX));
            if (r > 0) { p += r; n -= (size_t)r; continue; }
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
                if (mono_ms() > deadline) return -1;
                wait_fd(c->fd, e == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN, 1000);
                continue;
            }
            ERR_clear_error();
            return -1;
        }
        ssize_t r = send(c->fd, p, n, MSG_NOSIGNAL);
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (mono_ms() > deadline) return -1;
            wait_fd(c->fd, POLLOUT, 1000);
            continue;
        }
        return -1;
    }
    return 0;
}

void net_close(net_conn_t *c) {
    if (!c) return;
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->fd >= 0) close(c->fd);
    free(c->key);
    free(c);
}

/* ---------- proxy ---------- */

static bool host_matches_no_proxy(const char *host) {
    const char *np = env_get("NO_PROXY");
    if (!np) np = env_get("no_proxy");
    if (!np) return false;
    strvec_t parts = str_split(np, ",");
    bool match = false;
    for (size_t i = 0; i < parts.n && !match; i++) {
        char *t = str_trim(parts.v[i]);
        char *e = t;
        if (*e == '.') e++;
        if (strcmp(t, "*") == 0) match = true;
        else if (*e && (strcasecmp(host, e) == 0 || (str_ends(host, e) && strlen(host) > strlen(e) &&
                                                     host[strlen(host) - strlen(e) - 1] == '.')))
            match = true;
        free(t);
    }
    strvec_free(&parts);
    return match;
}

static const char *proxy_for(const char *scheme, const char *host) {
    if (host_matches_no_proxy(host)) return NULL;
    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0) return NULL;
    bool tls = strcmp(scheme, "https") == 0 || strcmp(scheme, "wss") == 0;
    const char *p = tls ? env_get("HTTPS_PROXY") : env_get("HTTP_PROXY");
    if (!p) p = tls ? env_get("https_proxy") : env_get("http_proxy");
    if (!p) p = env_get("ALL_PROXY");
    if (!p) p = env_get("all_proxy");
    return p;
}

/* ---------- pool ---------- */

#define POOL_MAX 16
static net_conn_t *g_pool[POOL_MAX];
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;

static net_conn_t *pool_take(const char *key) {
    net_conn_t *found = NULL;
    pthread_mutex_lock(&g_pool_mu);
    int64_t now = mono_ms();
    for (int i = 0; i < POOL_MAX; i++) {
        net_conn_t *c = g_pool[i];
        if (!c) continue;
        if (now - c->idle_since > 50000) {
            g_pool[i] = NULL;
            net_close(c);
            continue;
        }
        if (!found && strcmp(c->key, key) == 0) {
            g_pool[i] = NULL;
            /* stale check: readable while idle means closed or garbage */
            struct pollfd pf = {c->fd, POLLIN, 0};
            if (poll(&pf, 1, 0) != 0) net_close(c);
            else found = c;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    return found;
}

static void pool_put(net_conn_t *c) {
    c->idle_since = mono_ms();
    c->rpos = c->rlen = 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < POOL_MAX; i++) {
        if (!g_pool[i]) {
            g_pool[i] = c;
            pthread_mutex_unlock(&g_pool_mu);
            return;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    net_close(c);
}

void http_pool_clear(void) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < POOL_MAX; i++) {
        net_close(g_pool[i]);
        g_pool[i] = NULL;
    }
    pthread_mutex_unlock(&g_pool_mu);
}

/* buffered line reading for header parsing */
static int conn_fill(net_conn_t *c, int64_t timeout_ms, volatile sig_atomic_t *cancel) {
    if (c->rpos >= c->rlen) c->rpos = c->rlen = 0;
    if (c->rlen == sizeof c->rbuf) {
        if (c->rpos == 0) return -1;
        memmove(c->rbuf, c->rbuf + c->rpos, c->rlen - c->rpos);
        c->rlen -= c->rpos;
        c->rpos = 0;
    }
    int64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        if (cancel && *cancel) return -4;
        int64_t left = deadline - mono_ms();
        if (left <= 0) return -2;
        size_t save_rpos = c->rpos, save_rlen = c->rlen;
        c->rpos = c->rlen = 0; /* bypass buffer in net_read */
        ssize_t r = net_read(c, c->rbuf + save_rlen, sizeof c->rbuf - save_rlen, left);
        c->rpos = save_rpos;
        c->rlen = save_rlen;
        if (r == -3) continue;
        if (r < 0) return (int)r;
        if (r == 0) return 0;
        c->rlen += (size_t)r;
        return (int)r;
    }
}

net_conn_t *net_connect(const char *host, int port, bool tls, int64_t timeout_ms, char **err) {
    int fd = tcp_connect(host, port, timeout_ms, err);
    if (fd < 0) return NULL;
    net_conn_t *c = conn_new(fd);
    if (tls && tls_handshake(c, host, timeout_ms, err) != 0) {
        net_close(c);
        return NULL;
    }
    return c;
}

static net_conn_t *open_conn(const url_t *u, bool tls, const char *proxy, bool *via_http_proxy, int64_t timeout_ms,
                             char **err) {
    *via_http_proxy = false;
    if (!proxy) return net_connect(u->host, u->port, tls, timeout_ms, err);
    url_t pu;
    char *pstr = strstr(proxy, "://") ? xstrdup(proxy) : xasprintf("http://%s", proxy);
    if (url_parse(pstr, &pu) != 0) {
        free(pstr);
        return net_connect(u->host, u->port, tls, timeout_ms, err);
    }
    free(pstr);
    net_conn_t *c = net_connect(pu.host, pu.port, strcmp(pu.scheme, "https") == 0, timeout_ms, err);
    if (!c) {
        url_free(&pu);
        return NULL;
    }
    if (!tls) {
        *via_http_proxy = true;
        url_free(&pu);
        return c;
    }
    sb_t req = SB_INIT;
    sb_appendf(&req, "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n", u->host, u->port, u->host, u->port);
    if (pu.userinfo) {
        char *dec = url_decode(pu.userinfo);
        char *b = base64_encode(dec, strlen(dec));
        sb_appendf(&req, "Proxy-Authorization: Basic %s\r\n", b);
        free(b);
        free(dec);
    }
    sb_append(&req, "\r\n");
    url_free(&pu);
    if (net_write_all(c, req.s, req.len) != 0) {
        sb_free(&req);
        net_close(c);
        if (err) *err = xstrdup("proxy CONNECT write failed");
        return NULL;
    }
    sb_free(&req);
    /* read until \r\n\r\n */
    for (;;) {
        char *end = memmem(c->rbuf, c->rlen, "\r\n\r\n", 4);
        if (end) {
            int status = 0;
            sscanf(c->rbuf, "HTTP/%*d.%*d %d", &status);
            if (status != 200) {
                if (err) *err = xasprintf("proxy CONNECT failed with status %d", status);
                net_close(c);
                return NULL;
            }
            c->rpos = c->rlen = 0;
            break;
        }
        int r = conn_fill(c, timeout_ms, NULL);
        if (r <= 0) {
            if (err) *err = xstrdup("proxy CONNECT: connection closed");
            net_close(c);
            return NULL;
        }
    }
    if (tls_handshake(c, u->host, timeout_ms, err) != 0) {
        net_close(c);
        return NULL;
    }
    return c;
}

/* Connection for a non-HTTP protocol (websockets): TLS for https/wss, CONNECT tunnel through the configured
 * proxy for TLS targets (a plain ws:// target connects directly). */
net_conn_t *net_open(const url_t *u, int64_t timeout_ms, char **err) {
    http_global_init();
    bool tls = strcmp(u->scheme, "https") == 0 || strcmp(u->scheme, "wss") == 0;
    const char *proxy = proxy_for(u->scheme, u->host);
    if (!tls) proxy = NULL;
    bool via = false;
    return open_conn(u, tls, proxy, &via, timeout_ms, err);
}

/* ---------- body decoding pipeline ---------- */

typedef struct {
    const http_req_t *req;
    http_resp_t *resp;
    sb_t buf;
    size_t max_body;
    z_stream zs;
    bool inflating;
    bool aborted;
} body_sink_t;

static int sink_emit(body_sink_t *s, const char *data, size_t len) {
    if (!len) return 0;
    if (s->req->on_data) {
        if (s->req->on_data(s->req->ud, data, len) != 0) {
            s->aborted = true;
            return -1;
        }
        return 0;
    }
    if (s->buf.len + len > s->max_body) {
        len = s->max_body > s->buf.len ? s->max_body - s->buf.len : 0;
        s->resp->truncated = true;
    }
    sb_appendn(&s->buf, data, len);
    return 0;
}

static int sink_write(body_sink_t *s, const char *data, size_t len) {
    if (!s->inflating) return sink_emit(s, data, len);
    char out[32768];
    s->zs.next_in = (Bytef *)data;
    s->zs.avail_in = (uInt)len;
    while (s->zs.avail_in) {
        s->zs.next_out = (Bytef *)out;
        s->zs.avail_out = sizeof out;
        int r = inflate(&s->zs, Z_NO_FLUSH);
        size_t produced = sizeof out - s->zs.avail_out;
        if (produced && sink_emit(s, out, produced) != 0) return -1;
        if (r == Z_STREAM_END) {
            inflateReset(&s->zs);
            if (!s->zs.avail_in) break;
            continue;
        }
        if (r != Z_OK && r != Z_BUF_ERROR) return -1;
        if (r == Z_BUF_ERROR && produced == 0) break;
    }
    return 0;
}

/* ---------- request ---------- */

static int parse_status_headers(const char *hdr, size_t len, http_resp_t *resp) {
    strvec_t lines = str_split_lines(hdr, len);
    if (!lines.n) {
        strvec_free(&lines);
        return -1;
    }
    int status = 0;
    if (sscanf(lines.v[0], "HTTP/%*d.%*d %d", &status) != 1 && sscanf(lines.v[0], "HTTP/%*d %d", &status) != 1) {
        strvec_free(&lines);
        return -1;
    }
    resp->status = status;
    http_headers_free(&resp->headers);
    for (size_t i = 1; i < lines.n; i++) {
        char *colon = strchr(lines.v[i], ':');
        if (!colon) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *name = str_trim(lines.v[i]);
        char *val = str_trim(v);
        http_headers_add(&resp->headers, name, val);
        free(name);
        free(val);
    }
    strvec_free(&lines);
    return 0;
}

typedef enum { RR_OK, RR_RETRYABLE, RR_FAIL } rr_t;

static rr_t do_once(const http_req_t *req, const char *url_s, const char *method, const char *body, size_t body_len,
                    http_resp_t *resp, bool *keepalive_ok) {
    url_t u;
    if (url_parse(url_s, &u) != 0) {
        resp->error = xasprintf("invalid URL: %s", url_s);
        return RR_FAIL;
    }
    bool tls = strcmp(u.scheme, "https") == 0;
    if (!tls && strcmp(u.scheme, "http") != 0) {
        resp->error = xasprintf("unsupported URL scheme: %s", u.scheme);
        url_free(&u);
        return RR_FAIL;
    }
    const char *proxy = req->no_proxy ? NULL : proxy_for(u.scheme, u.host);
    char *key = xasprintf("%s|%s|%d|%s", tls ? "s" : "p", u.host, u.port, proxy ? proxy : "");
    int64_t connect_to = req->connect_timeout_ms ? req->connect_timeout_ms : 30000;
    int64_t idle_to = req->idle_timeout_ms ? req->idle_timeout_ms : 300000;
    int64_t deadline = req->timeout_ms ? mono_ms() + req->timeout_ms : 0;

    bool reused = false, via_http_proxy = false;
    net_conn_t *c = pool_take(key);
    if (c) {
        reused = true;
        via_http_proxy = proxy && !tls;
    } else {
        char *err = NULL;
        c = open_conn(&u, tls, proxy, &via_http_proxy, connect_to, &err);
        if (!c) {
            resp->error = err ? err : xstrdup("connection failed");
            free(key);
            url_free(&u);
            return RR_FAIL;
        }
        c->key = xstrdup(key);
    }
    free(key);

    /* build request */
    sb_t r = SB_INIT;
    if (via_http_proxy) sb_appendf(&r, "%s %s HTTP/1.1\r\n", method, url_s);
    else sb_appendf(&r, "%s %s HTTP/1.1\r\n", method, u.path);
    bool def_port = (tls && u.port == 443) || (!tls && u.port == 80);
    bool v6 = strchr(u.host, ':') != NULL;
    if (!http_headers_get(req->headers, "Host")) {
        if (def_port) sb_appendf(&r, "Host: %s%s%s\r\n", v6 ? "[" : "", u.host, v6 ? "]" : "");
        else sb_appendf(&r, "Host: %s%s%s:%d\r\n", v6 ? "[" : "", u.host, v6 ? "]" : "", u.port);
    }
    if (!http_headers_get(req->headers, "User-Agent")) sb_append(&r, "User-Agent: omc/" OMC_VERSION "\r\n");
    if (!http_headers_get(req->headers, "Accept")) sb_append(&r, "Accept: */*\r\n");
    if (!req->no_decompress && !http_headers_get(req->headers, "Accept-Encoding"))
        sb_append(&r, "Accept-Encoding: gzip, deflate\r\n");
    if (u.userinfo && !http_headers_get(req->headers, "Authorization")) {
        char *dec = url_decode(u.userinfo);
        char *b = base64_encode(dec, strlen(dec));
        sb_appendf(&r, "Authorization: Basic %s\r\n", b);
        free(b);
        free(dec);
    }
    if (req->headers) {
        for (size_t i = 0; i < req->headers->n; i++) {
            const char *n = req->headers->v[i].name, *v = req->headers->v[i].value;
            /* reject header injection */
            if (strpbrk(n, "\r\n:") || strpbrk(v, "\r\n")) continue;
            sb_appendf(&r, "%s: %s\r\n", n, v);
        }
    }
    if (body || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0)
        sb_appendf(&r, "Content-Length: %zu\r\n", body_len);
    sb_append(&r, "\r\n");
    url_free(&u);

    int wr = net_write_all(c, r.s, r.len);
    sb_free(&r);
    if (wr == 0 && body_len) wr = net_write_all(c, body, body_len);
    if (wr != 0) {
        net_close(c);
        if (reused) return RR_RETRYABLE;
        resp->error = xstrdup("failed to send request");
        return RR_FAIL;
    }

    /* read headers (skip 1xx) */
    size_t hdr_end = 0;
    for (;;) {
        char *e = c->rlen > c->rpos ? memmem(c->rbuf + c->rpos, c->rlen - c->rpos, "\r\n\r\n", 4) : NULL;
        if (e) {
            hdr_end = (size_t)(e - c->rbuf) + 4;
            if (parse_status_headers(c->rbuf + c->rpos, (size_t)(e - (c->rbuf + c->rpos)), resp) != 0) {
                net_close(c);
                resp->error = xstrdup("malformed HTTP response");
                return RR_FAIL;
            }
            c->rpos = hdr_end;
            if (resp->status >= 100 && resp->status < 200 && resp->status != 101) continue;
            break;
        }
        int64_t to = idle_to;
        if (deadline) to = MIN(to, deadline - mono_ms());
        int fr = conn_fill(c, to, req->cancel);
        if (fr <= 0) {
            bool got_any = c->rlen > 0;
            net_close(c);
            if (fr == -4) {
                resp->aborted = true;
                resp->error = xstrdup("request aborted");
                return RR_FAIL;
            }
            if (reused && !got_any && fr == 0) return RR_RETRYABLE;
            resp->error = xstrdup(fr == -2 ? "timed out waiting for response" : "connection closed before response");
            return RR_FAIL;
        }
    }

    if (req->on_headers && req->on_headers(req->ud, resp->status, &resp->headers) != 0) {
        resp->aborted = true;
        net_close(c);
        return RR_OK;
    }

    body_sink_t sink = {.req = req, .resp = resp, .max_body = req->max_body ? req->max_body : (64u << 20)};
    const char *ce = http_headers_get(&resp->headers, "Content-Encoding");
    if (ce && !req->no_decompress && (strcasecmp(ce, "gzip") == 0 || strcasecmp(ce, "deflate") == 0 || strcasecmp(ce, "x-gzip") == 0)) {
        if (inflateInit2(&sink.zs, 32 + MAX_WBITS) == Z_OK) sink.inflating = true;
    }

    const char *te = http_headers_get(&resp->headers, "Transfer-Encoding");
    const char *cl = http_headers_get(&resp->headers, "Content-Length");
    const char *conn_hdr = http_headers_get(&resp->headers, "Connection");
    bool chunked = te && str_istr(te, "chunked");
    bool no_body = strcmp(method, "HEAD") == 0 || resp->status == 204 || resp->status == 304;
    int64_t remaining = cl && !chunked ? strtoll(cl, NULL, 10) : -1;
    bool until_close = !chunked && remaining < 0 && !no_body;
    bool ok = true;
    int fail_code = 0;

#define AVAIL (c->rlen - c->rpos)
#define FILL()                                                                   \
    do {                                                                         \
        int64_t to_ = idle_to;                                                   \
        if (deadline) to_ = MIN(to_, deadline - mono_ms());                      \
        int fr_ = conn_fill(c, to_, req->cancel);                                \
        if (fr_ <= 0) { fail_code = fr_ ? fr_ : -5; goto body_done; }           \
    } while (0)

    if (no_body) {
        /* nothing */
    } else if (chunked) {
        for (;;) {
            char *nl;
            while (!(nl = memmem(c->rbuf + c->rpos, AVAIL, "\r\n", 2))) FILL();
            /* chunk-size [; ext] CRLF, parsed inside the line: strtoll skipped leading whitespace (an empty line's CRLF)
             * and read on past the line end, into bytes not yet received */
            long long sz = 0;
            const char *q = c->rbuf + c->rpos;
            while (q < nl && (*q == ' ' || *q == '\t')) q++;
            int nd = 0;
            for (; q < nl && isxdigit((unsigned char)*q) && sz <= (1LL << 56); q++, nd++)
                sz = sz * 16 + (isdigit((unsigned char)*q) ? *q - '0' : (tolower((unsigned char)*q) - 'a' + 10));
            c->rpos = (size_t)(nl - c->rbuf) + 2;
            if (!nd || sz > (1LL << 56)) { ok = false; fail_code = -5; goto body_done; }
            if (sz == 0) {
                /* trailers until empty line */
                for (;;) {
                    while (!(nl = memmem(c->rbuf + c->rpos, AVAIL, "\r\n", 2))) FILL();
                    bool empty = nl == c->rbuf + c->rpos;
                    c->rpos = (size_t)(nl - c->rbuf) + 2;
                    if (empty) break;
                }
                break;
            }
            while (sz > 0) {
                if (!AVAIL) FILL();
                size_t k = (size_t)MIN((long long)AVAIL, sz);
                if (sink_write(&sink, c->rbuf + c->rpos, k) != 0) { ok = false; c->rpos += k; goto body_done; }
                c->rpos += k;
                sz -= (long long)k;
            }
            while (AVAIL < 2) FILL();
            c->rpos += 2;
        }
    } else {
        while (remaining != 0) {
            if (!AVAIL) {
                int64_t to_ = idle_to;
                if (deadline) to_ = MIN(to_, deadline - mono_ms());
                int fr_ = conn_fill(c, to_, req->cancel);
                if (fr_ == 0 && until_close) break;
                if (fr_ <= 0) { fail_code = fr_ ? fr_ : -5; goto body_done; } /* -5: EOF before the body ended */
            }
            size_t k = remaining < 0 ? AVAIL : (size_t)MIN((int64_t)AVAIL, remaining);
            if (sink_write(&sink, c->rbuf + c->rpos, k) != 0) { ok = false; c->rpos += k; goto body_done; }
            c->rpos += k;
            if (remaining > 0) remaining -= (int64_t)k;
        }
    }
    *keepalive_ok = ok && !until_close && !(conn_hdr && strcasecmp(conn_hdr, "close") == 0);

body_done:
#undef AVAIL
#undef FILL
    if (sink.inflating) inflateEnd(&sink.zs);
    if (fail_code != 0) {
        ok = false;
        if (fail_code == -4) {
            resp->aborted = true;
            resp->error = xstrdup("request aborted");
        } else {
            /* a peer close (FIN before the last chunk / Content-Length, or RST) reads as Bun's fetch error, which
             * omp's turn recovery classifies (isUnexpectedSocketCloseMessage) */
            resp->error = xstrdup(fail_code == -2 ? "timed out reading response body"
                                                  : "ECONNRESET: The socket connection was closed unexpectedly. For more "
                                                    "information, pass `verbose: true` in the second argument to fetch()");
        }
        *keepalive_ok = false;
    }
    if (sink.aborted) resp->aborted = true;
    if (!req->on_data) {
        resp->body_len = sink.buf.len;
        resp->body = sink.buf.s ? sink.buf.s : xstrdup("");
    } else {
        sb_free(&sink.buf);
    }
    if (ok && *keepalive_ok) pool_put(c);
    else net_close(c);
    return RR_OK;
}

int http_request(const http_req_t *req, http_resp_t *resp) {
    http_global_init();
    memset(resp, 0, sizeof(*resp));
    char *url = xstrdup(req->url);
    const char *method = req->method ? req->method : (req->body ? "POST" : "GET");
    const char *body = req->body;
    size_t body_len = req->body_len;
    char *method_own = NULL;
    for (int redirects = 0;; redirects++) {
        bool ka = false;
        if (req->cancel && *req->cancel) { /* cancelled before dispatch: nothing goes on the wire */
            resp->error = xstrdup("request cancelled");
            free(url);
            free(method_own);
            return -1;
        }
        rr_t rr = do_once(req, url, method, body, body_len, resp, &ka);
        if (rr == RR_RETRYABLE) {
            http_headers_free(&resp->headers);
            rr = do_once(req, url, method, body, body_len, resp, &ka);
            if (rr == RR_RETRYABLE) {
                resp->error = xstrdup("connection reset");
                rr = RR_FAIL;
            }
        }
        if (rr == RR_FAIL) {
            free(url);
            free(method_own);
            return -1;
        }
        int st = resp->status;
        const char *loc = http_headers_get(&resp->headers, "Location");
        if (redirects < req->max_redirects && loc && (st == 301 || st == 302 || st == 303 || st == 307 || st == 308)) {
            char *next = url_resolve(url, loc);
            free(url);
            url = next;
            if (st == 303 || ((st == 301 || st == 302) && strcmp(method, "POST") == 0)) {
                free(method_own);
                method_own = xstrdup("GET");
                method = method_own;
                body = NULL;
                body_len = 0;
            }
            http_headers_free(&resp->headers);
            free(resp->body);
            resp->body = NULL;
            resp->body_len = 0;
            continue;
        }
        break;
    }
    resp->final_url = url;
    free(method_own);
    return 0;
}

void http_resp_free(http_resp_t *r) {
    http_headers_free(&r->headers);
    free(r->body);
    free(r->error);
    free(r->final_url);
    memset(r, 0, sizeof(*r));
}

int http_get(const char *url, const http_headers_t *headers, int64_t timeout_ms, http_resp_t *resp) {
    http_req_t req = {.method = "GET", .url = url, .headers = headers, .timeout_ms = timeout_ms, .max_redirects = 10};
    return http_request(&req, resp);
}

int http_post_json(const char *url, const http_headers_t *headers, const char *json, int64_t timeout_ms,
                   http_resp_t *resp) {
    http_headers_t h = {0};
    if (headers)
        for (size_t i = 0; i < headers->n; i++) http_headers_add(&h, headers->v[i].name, headers->v[i].value);
    if (!http_headers_get(&h, "Content-Type")) http_headers_add(&h, "Content-Type", "application/json");
    http_req_t req = {.method = "POST", .url = url, .headers = &h, .body = json, .body_len = strlen(json),
                      .timeout_ms = timeout_ms};
    int rc = http_request(&req, resp);
    http_headers_free(&h);
    return rc;
}

/* ---------- SSE ---------- */

void sse_init(sse_parser_t *p, sse_event_cb cb, void *ud) {
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->ud = ud;
}

static int sse_dispatch(sse_parser_t *p) {
    int rc = 0;
    if (p->has_data) {
        size_t n = p->data.len;
        if (n && p->data.s[n - 1] == '\n') sb_truncate(&p->data, n - 1);
        rc = p->cb(p->ud, p->event ? p->event : "message", sb_cstr(&p->data), p->data.len);
    }
    sb_clear(&p->data);
    free(p->event);
    p->event = NULL;
    p->has_data = false;
    return rc;
}

static int sse_line(sse_parser_t *p, const char *line, size_t len) {
    if (len == 0) return sse_dispatch(p);
    if (line[0] == ':') return 0;
    const char *colon = memchr(line, ':', len);
    size_t fl = colon ? (size_t)(colon - line) : len;
    const char *val = colon ? colon + 1 : line + len;
    size_t vl = colon ? len - fl - 1 : 0;
    if (vl && *val == ' ') { val++; vl--; }
    if (fl == 4 && memcmp(line, "data", 4) == 0) {
        sb_appendn(&p->data, val, vl);
        sb_putc(&p->data, '\n');
        p->has_data = true;
    } else if (fl == 5 && memcmp(line, "event", 5) == 0) {
        free(p->event);
        p->event = xstrndup(val, vl);
    }
    return 0;
}

int sse_feed(sse_parser_t *p, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];
        if (ch == '\n') {
            size_t l = p->line.len;
            if (l && p->line.s[l - 1] == '\r') l--;
            int rc = sse_line(p, p->line.s ? p->line.s : "", l);
            sb_clear(&p->line);
            if (rc) return rc;
        } else {
            sb_putc(&p->line, ch);
        }
    }
    return 0;
}

int sse_finish(sse_parser_t *p) {
    int rc = 0;
    if (p->line.len) {
        rc = sse_line(p, p->line.s, p->line.len);
        sb_clear(&p->line);
    }
    if (!rc) rc = sse_dispatch(p);
    return rc;
}

void sse_free(sse_parser_t *p) {
    sb_free(&p->line);
    sb_free(&p->data);
    free(p->event);
}
