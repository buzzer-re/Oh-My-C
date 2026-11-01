/* test_ws.c - WebSocket client: accept key, frame encoding, and a loopback session against a tiny in-process server
 * (fragmentation + interleaved ping, server close, masked-frame / oversized-frame rejection, upgrade refusal). */
#define _GNU_SOURCE
#include "net/ws.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
#define CHECK(c, ...)                                                                                                   \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                        \
            fprintf(stderr, __VA_ARGS__);                                                                               \
            fprintf(stderr, "\n");                                                                                      \
            failures++;                                                                                                 \
        }                                                                                                               \
    } while (0)

static int g_lfd, g_port;
static int g_mode; /* 0 normal session, 1 masked server frame, 2 oversized, 3 refuse upgrade */
static char g_got[256];

static void wr(int fd, const void *p, size_t n) {
    if (write(fd, p, n) != (ssize_t)n) {}
}

static size_t rd_frame(int fd, unsigned char *out, size_t cap, int *op) {
    unsigned char h[2];
    if (recv(fd, h, 2, MSG_WAITALL) != 2) return 0;
    *op = h[0] & 15;
    size_t n = h[1] & 127;
    if (n == 126) {
        unsigned char e[2];
        recv(fd, e, 2, MSG_WAITALL);
        n = (size_t)e[0] << 8 | e[1];
    }
    unsigned char mk[4];
    recv(fd, mk, 4, MSG_WAITALL);
    if (n > cap) n = cap;
    recv(fd, out, n, MSG_WAITALL);
    for (size_t i = 0; i < n; i++) out[i] ^= mk[i & 3];
    return n;
}

static void *server(void *ud) {
    (void)ud;
    int c = accept(g_lfd, NULL, NULL);
    char req[4096] = {0};
    size_t n = 0;
    while (n < sizeof req - 1 && !strstr(req, "\r\n\r\n")) {
        ssize_t r = recv(c, req + n, sizeof req - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
    }
    if (g_mode == 3) {
        const char *resp = "HTTP/1.1 403 Forbidden\r\nContent-Length: 6\r\n\r\ndenied";
        wr(c, resp, strlen(resp));
        close(c);
        return NULL;
    }
    char *k = strstr(req, "Sec-WebSocket-Key: ");
    char key[64] = {0};
    if (k) sscanf(k + 19, "%63s", key);
    char *acc = ws_accept_key(key);
    char resp[512];
    snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    free(acc);
    wr(c, resp, strlen(resp));
    unsigned char buf[256];
    int op = 0;
    size_t m = rd_frame(c, buf, sizeof buf, &op); /* client hello */
    snprintf(g_got, sizeof g_got, "%d:%.*s", op, (int)m, buf);
    if (g_mode == 1) {
        unsigned char f[] = {0x81, 0x82, 1, 2, 3, 4, 'h' ^ 1, 'i' ^ 2};
        wr(c, f, sizeof f);
    } else if (g_mode == 2) {
        unsigned char f[] = {0x82, 126, 0x10, 0x00}; /* 4096 bytes announced */
        wr(c, f, sizeof f);
    } else {
        unsigned char f1[] = {0x01, 5, 'h', 'e', 'l', 'l', 'o'};
        unsigned char ping[] = {0x89, 2, 'p', 'g'};
        unsigned char f2[] = {0x80, 6, ' ', 'w', 'o', 'r', 'l', 'd'};
        wr(c, f1, sizeof f1);
        wr(c, ping, sizeof ping);
        wr(c, f2, sizeof f2);
        m = rd_frame(c, buf, sizeof buf, &op); /* pong */
        if (op != 10 || m != 2 || memcmp(buf, "pg", 2)) snprintf(g_got, sizeof g_got, "bad pong %d", op);
        unsigned char cl[] = {0x88, 6, 0x0f, 0xa3, 'b', 'y', 'e', '!'}; /* 4003 */
        wr(c, cl, sizeof cl);
        rd_frame(c, buf, sizeof buf, &op);
    }
    usleep(100000);
    close(c);
    return NULL;
}

static ws_t *start(int mode, pthread_t *th, char **err) {
    g_mode = mode;
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(g_lfd, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(g_lfd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    listen(g_lfd, 1);
    pthread_create(th, NULL, server, NULL);
    char *url = xasprintf("ws://127.0.0.1:%d/path?q=1", g_port);
    http_headers_t h = {0};
    http_headers_set(&h, "authorization", "Bearer t");
    int st = 0;
    ws_t *w = ws_connect(url, &h, 5000, &st, err);
    http_headers_free(&h);
    free(url);
    if (w) ws_send_text(w, "hi there", 8);
    return w;
}

static void stop(pthread_t th) {
    pthread_join(th, NULL);
    close(g_lfd);
}

int main(void) {
    char *a = ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(!strcmp(a, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "accept key %s", a);
    free(a);
    sb_t b = SB_INIT;
    unsigned char mk[4] = {0x37, 0xfa, 0x21, 0x3d};
    ws_encode_frame(&b, WS_OP_TEXT, true, mk, "Hello", 5);
    const unsigned char want[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58}; /* RFC 6455 5.7 */
    CHECK(b.len == sizeof want && !memcmp(b.s, want, sizeof want), "masked frame encoding");
    sb_clear(&b);
    char big[300];
    memset(big, 'x', sizeof big);
    ws_encode_frame(&b, WS_OP_BINARY, true, mk, big, sizeof big);
    CHECK(b.len == 4 + 4 + 300 && (unsigned char)b.s[1] == (0x80 | 126) && (unsigned char)b.s[2] == 1 && (unsigned char)b.s[3] == 44,
          "16-bit length");
    sb_free(&b);

    pthread_t th;
    char *err = NULL;
    ws_t *w = start(0, &th, &err);
    CHECK(w != NULL, "connect: %s", err ? err : "");
    if (w) {
        ws_msg_t m;
        int rc = ws_recv(w, 3000, NULL, &m, &err);
        CHECK(rc == 1 && m.opcode == WS_OP_TEXT && !strcmp(m.data, "hello world"), "fragmented text: %d %s", rc, m.data ? m.data : "");
        ws_msg_free(&m);
        rc = ws_recv(w, 3000, NULL, &m, &err);
        CHECK(rc == 1 && m.opcode == WS_OP_CLOSE && m.close_code == 4003 && str_eq(m.close_reason, "bye!"), "close frame");
        ws_msg_free(&m);
        CHECK(!ws_is_open(w), "closed after close frame");
        ws_close(w, 1000, NULL);
    }
    stop(th);
    CHECK(!strcmp(g_got, "1:hi there"), "client text frame: %s", g_got);

    w = start(1, &th, &err);
    if (w) {
        ws_msg_t m;
        int rc = ws_recv(w, 3000, NULL, &m, &err);
        CHECK(rc == -1 && err && strstr(err, "masked server frame"), "masked server frame rejected");
        free(err);
        err = NULL;
        ws_close(w, 1002, NULL);
    }
    stop(th);

    w = start(2, &th, &err);
    if (w) {
        ws_set_max_message(w, 1024);
        ws_msg_t m;
        int rc = ws_recv(w, 3000, NULL, &m, &err);
        CHECK(rc == -1 && err && strstr(err, "exceeds 1024 bytes"), "oversized frame rejected");
        free(err);
        err = NULL;
        ws_close(w, 0, NULL);
    }
    stop(th);

    w = start(3, &th, &err);
    CHECK(!w && err && strstr(err, "HTTP 403: denied"), "upgrade refused: %s", err ? err : "");
    free(err);
    stop(th);

    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
