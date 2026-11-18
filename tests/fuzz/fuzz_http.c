/* fuzz_http - the HTTP/1.1 client's response parsing (status line, headers, 1xx, Content-Length, chunked encoding,
 * gzip/deflate, keep-alive) and the SSE parser on top of it (src/net/http.c), plus WebSocket frame decoding after a
 * valid opening handshake (src/net/ws.c). A loopback server thread answers each request with the fuzz input.
 * Input: <selector byte><raw response bytes (HTTP) or raw frames (WebSocket)>. */
#include "core/core.h"
#include "net/http.h"
#include "net/ws.h"
#include "fuzz.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>

static int g_port;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_payload; /* owned copy, replaced under g_mu: a straggling connection may outlive its input */
static size_t g_len;
static bool g_ws;

/* read until the end of the request head (or 2s); returns the head (owned) */
static char *read_head(int fd) {
    sb_t b = SB_INIT;
    char tmp[4096];
    while (!b.len || !memmem(b.s, b.len, "\r\n\r\n", 4)) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 2000) <= 0) break;
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        sb_appendn(&b, tmp, (size_t)n);
        if (b.len > 1 << 16) break;
    }
    return sb_detach(&b);
}

static void *server(void *arg) {
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        char *head = read_head(fd);
        pthread_mutex_lock(&g_mu);
        size_t n = g_len;
        uint8_t *p = xmalloc(n + 1);
        if (n) memcpy(p, g_payload, n);
        bool ws = g_ws;
        pthread_mutex_unlock(&g_mu);
        if (ws) {
            const char *k = str_istr(head, "Sec-WebSocket-Key:");
            char key[128] = "";
            if (k) sscanf(k + 18, " %127[^\r\n]", key);
            char *acc = ws_accept_key(key);
            char *resp = xasprintf("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
            if (send(fd, resp, strlen(resp), MSG_NOSIGNAL) < 0) {}
            free(resp);
            free(acc);
        }
        free(head);
        for (size_t off = 0; off < n;) {
            ssize_t w = send(fd, p + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) break;
            off += (size_t)w;
        }
        free(p);
        shutdown(fd, SHUT_WR);
        /* drain whatever the client still sends (pongs, close frames) so close() never resets the connection */
        char tmp[4096];
        for (;;) {
            struct pollfd pf = {fd, POLLIN, 0};
            if (poll(&pf, 1, 3000) <= 0) break;
            if (recv(fd, tmp, sizeof tmp, 0) <= 0) break;
        }
        close(fd);
    }
    return NULL;
}

FUZZ_SETUP {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 16) != 0) abort();
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    pthread_t t;
    pthread_create(&t, NULL, server, (void *)(intptr_t)ls);
    pthread_detach(t);
}

static int on_event(void *ud, const char *ev, const char *data, size_t len) {
    FUZZ_ASSERT(ev && data && strlen(data) <= len);
    return 0;
}
static int on_data(void *ud, const char *d, size_t n) { return sse_feed(ud, d, n); }

FUZZ_ENTRY {
    if (size > 1 << 16) return 0;
    unsigned sel = fuzz_byte(&data, &size);
    pthread_mutex_lock(&g_mu);
    free(g_payload);
    g_payload = xmalloc(size + 1);
    if (size) memcpy(g_payload, data, size);
    g_len = size;
    g_ws = sel & 0x80;
    pthread_mutex_unlock(&g_mu);
    char url[64];
    if (sel & 0x80) {
        snprintf(url, sizeof url, "ws://127.0.0.1:%d/", g_port);
        int status = 0;
        char *err = NULL;
        ws_t *w = ws_connect(url, NULL, 3000, &status, &err);
        free(err);
        if (w) {
            if (sel & 1) ws_set_max_message(w, 4096);
            for (int i = 0; i < 64; i++) {
                ws_msg_t m = {0};
                char *e2 = NULL;
                int rc = ws_recv(w, (i & 1) ? 1 : 1000, NULL, &m, &e2); /* short waits split fragmented messages */
                free(e2);
                if (rc == -2) continue;
                if (rc != 1) break;
                if (m.opcode != WS_OP_CLOSE) FUZZ_ASSERT(m.data && strlen(m.data) <= m.len);
                bool closed = m.opcode == WS_OP_CLOSE;
                ws_msg_free(&m);
                if (closed) break;
            }
            ws_close(w, 1000, "bye");
        }
        return 0;
    }
    snprintf(url, sizeof url, "http://127.0.0.1:%d/x", g_port);
    sse_parser_t sse;
    sse_init(&sse, on_event, NULL);
    http_req_t req = {.url = url, .method = (sel & 3) == 3 ? "HEAD" : "GET", .timeout_ms = 3000, .idle_timeout_ms = 2000,
                      .no_proxy = true, .no_decompress = sel & 4, .max_body = (sel & 8) ? 100 : 0,
                      .on_data = (sel & 16) ? on_data : NULL, .ud = &sse};
    http_resp_t resp;
    http_request(&req, &resp);
    if (resp.body) FUZZ_ASSERT(resp.body[resp.body_len] == 0);
    if (!resp.error && !req.on_data) FUZZ_ASSERT(resp.body);
    sse_finish(&sse);
    sse_free(&sse);
    http_resp_free(&resp);
    http_pool_clear();
    return 0;
}
