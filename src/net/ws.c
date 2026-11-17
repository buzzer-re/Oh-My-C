/* ws.c - RFC 6455 WebSocket client (see ws.h). */
#define _GNU_SOURCE
#include "ws.h"

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <strings.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_DEFAULT_MAX (16u * 1024u * 1024u)
#define WS_MAX_HANDSHAKE 65536

struct ws {
    net_conn_t *c;
    char *buf; /* received, not yet consumed bytes */
    size_t len, cap;
    size_t max_message;
    bool close_sent, closed;
    sb_t frag;   /* data of a fragmented message received so far: kept across a ws_recv timeout */
    int frag_op; /* its opcode, -1 when no message is in progress */
};

bool ws_is_open(const ws_t *w) { return w && w->c && !w->closed; }
void ws_set_max_message(ws_t *w, size_t max) { w->max_message = max ? max : WS_DEFAULT_MAX; }

char *ws_accept_key(const char *key) {
    char *cat = xasprintf("%s" WS_GUID, key);
    unsigned char dig[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)cat, strlen(cat), dig);
    free(cat);
    return base64_encode(dig, sizeof dig);
}

static void rnd(unsigned char *p, size_t n) {
    if (RAND_bytes(p, (int)n) != 1)
        for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(hash_fnv1a(&i, sizeof i) ^ (uint64_t)now_ms());
}

void ws_encode_frame(sb_t *b, int opcode, bool fin, const unsigned char mk[4], const void *data, size_t len) {
    unsigned char h[14];
    size_t n = 0;
    h[n++] = (unsigned char)((fin ? 0x80 : 0) | (opcode & 0x0f));
    if (len < 126) {
        h[n++] = (unsigned char)(0x80 | len);
    } else if (len <= 0xffff) {
        h[n++] = 0x80 | 126;
        h[n++] = (unsigned char)(len >> 8);
        h[n++] = (unsigned char)len;
    } else {
        h[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) h[n++] = (unsigned char)((uint64_t)len >> (8 * i));
    }
    memcpy(h + n, mk, 4);
    n += 4;
    sb_appendn(b, (const char *)h, n);
    const unsigned char *d = data;
    for (size_t i = 0; i < len; i++) sb_putc(b, (char)(d[i] ^ mk[i & 3]));
}

static int send_frame(ws_t *w, int opcode, const void *data, size_t len) {
    if (!w || !w->c || w->closed) return -1;
    unsigned char mk[4];
    rnd(mk, 4);
    sb_t b = SB_INIT;
    ws_encode_frame(&b, opcode, true, mk, data, len);
    int rc = net_write_all(w->c, b.s, b.len);
    sb_free(&b);
    if (rc != 0) w->closed = true;
    return rc;
}

int ws_send_text(ws_t *w, const char *data, size_t len) { return send_frame(w, WS_OP_TEXT, data, len); }
int ws_send_binary(ws_t *w, const void *data, size_t len) { return send_frame(w, WS_OP_BINARY, data, len); }
int ws_send_ping(ws_t *w, const void *data, size_t len) { return send_frame(w, WS_OP_PING, data, MIN(len, (size_t)125)); }

/* read more bytes: 1 got data, 0 EOF, -1 error, -2 timeout, -4 cancelled */
static int fill(ws_t *w, int64_t deadline, volatile sig_atomic_t *cancel) {
    if (w->cap - w->len < 16384) {
        w->cap = w->cap ? w->cap * 2 : 32768;
        while (w->cap - w->len < 16384) w->cap *= 2;
        w->buf = xrealloc(w->buf, w->cap);
    }
    for (;;) {
        if (cancel && *cancel) return -4;
        int64_t slice = 250;
        if (deadline > 0) {
            int64_t left = deadline - mono_ms();
            if (left <= 0) return -2;
            slice = MIN(slice, left);
        }
        ssize_t r = net_read(w->c, w->buf + w->len, w->cap - w->len, slice);
        if (r > 0) {
            w->len += (size_t)r;
            return 1;
        }
        if (r == 0) return 0;
        if (r == -1) return -1;
        /* -2 / -3: slice elapsed */
    }
}

static void consume(ws_t *w, size_t n) {
    memmove(w->buf, w->buf + n, w->len - n);
    w->len -= n;
}

ws_t *ws_connect(const char *url, const http_headers_t *headers, int64_t timeout_ms, int *status, char **err) {
    if (status) *status = 0;
    url_t u;
    if (url_parse(url, &u) != 0 || (strcmp(u.scheme, "ws") && strcmp(u.scheme, "wss") && strcmp(u.scheme, "http") &&
                                    strcmp(u.scheme, "https"))) {
        if (u.scheme) url_free(&u);
        *err = xasprintf("invalid WebSocket URL: %s", url);
        return NULL;
    }
    if (timeout_ms <= 0) timeout_ms = 30000;
    net_conn_t *c = net_open(&u, timeout_ms, err);
    if (!c) {
        url_free(&u);
        return NULL;
    }
    unsigned char kb[16];
    rnd(kb, sizeof kb);
    char *key = base64_encode(kb, sizeof kb);
    sb_t req = SB_INIT;
    bool dflt = u.port == ((!strcmp(u.scheme, "wss") || !strcmp(u.scheme, "https")) ? 443 : 80);
    bool v6 = strchr(u.host, ':') != NULL;
    if (dflt) sb_appendf(&req, "GET %s HTTP/1.1\r\nHost: %s%s%s\r\n", u.path, v6 ? "[" : "", u.host, v6 ? "]" : "");
    else sb_appendf(&req, "GET %s HTTP/1.1\r\nHost: %s%s%s:%d\r\n", u.path, v6 ? "[" : "", u.host, v6 ? "]" : "", u.port);
    sb_appendf(&req, "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n", key);
    for (size_t i = 0; headers && i < headers->n; i++) {
        const char *k = headers->v[i].name, *v = headers->v[i].value;
        if (!k || !v || strpbrk(k, "\r\n:") || strpbrk(v, "\r\n")) continue;
        if (!strcasecmp(k, "host") || !strcasecmp(k, "upgrade") || !strcasecmp(k, "connection") ||
            !strncasecmp(k, "sec-websocket-", 14))
            continue;
        sb_appendf(&req, "%s: %s\r\n", k, v);
    }
    sb_append(&req, "\r\n");
    url_free(&u);
    ws_t *w = xcalloc(1, sizeof *w);
    w->c = c;
    w->max_message = WS_DEFAULT_MAX;
    w->frag_op = -1;
    int rc = net_write_all(c, req.s, req.len);
    sb_free(&req);
    if (rc != 0) {
        *err = xstrdup("WebSocket handshake write failed");
        goto fail;
    }
    int64_t deadline = mono_ms() + timeout_ms;
    char *end = NULL;
    while (!(end = w->len ? memmem(w->buf, w->len, "\r\n\r\n", 4) : NULL)) {
        if (w->len > WS_MAX_HANDSHAKE) {
            *err = xstrdup("WebSocket handshake response too large");
            goto fail;
        }
        int f = fill(w, deadline, NULL);
        if (f <= 0) {
            *err = xstrdup(f == -2 ? "WebSocket handshake timed out" : "WebSocket handshake: connection closed");
            goto fail;
        }
    }
    size_t hlen = (size_t)(end - w->buf) + 4;
    char *head = xstrndup(w->buf, hlen);
    consume(w, hlen);
    int st = 0;
    sscanf(head, "HTTP/%*d.%*d %d", &st);
    if (status) *status = st;
    char *accept = NULL;
    bool upgrade = false;
    for (char *line = strstr(head, "\r\n"); line && line[2]; line = strstr(line + 2, "\r\n")) {
        char *l = line + 2, *colon = strchr(l, ':'), *eol = strstr(l, "\r\n");
        if (!colon || !eol || colon > eol) continue;
        char *name = xstrndup(l, (size_t)(colon - l));
        char *val0 = xstrndup(colon + 1, (size_t)(eol - colon - 1));
        char *val = str_trim(val0);
        free(val0);
        if (!strcasecmp(name, "sec-websocket-accept")) {
            free(accept);
            accept = xstrdup(val);
        } else if (!strcasecmp(name, "upgrade") && !strcasecmp(val, "websocket")) {
            upgrade = true;
        }
        free(name);
        free(val);
    }
    char *want = ws_accept_key(key);
    if (st != 101) {
        /* keep a short body excerpt for diagnostics */
        size_t bl = MIN(w->len, (size_t)300);
        *err = bl ? xasprintf("WebSocket upgrade rejected with HTTP %d: %.*s", st, (int)bl, w->buf)
                  : xasprintf("WebSocket upgrade rejected with HTTP %d", st);
    } else if (!upgrade || !accept || strcmp(accept, want) != 0) {
        *err = xstrdup("WebSocket handshake: invalid Sec-WebSocket-Accept");
    }
    free(want);
    free(accept);
    free(head);
    free(key);
    key = NULL;
    if (*err) goto fail;
    return w;
fail:
    free(key);
    net_close(w->c);
    free(w->buf);
    free(w);
    return NULL;
}

void ws_msg_free(ws_msg_t *m) {
    if (!m) return;
    free(m->data);
    free(m->close_reason);
    memset(m, 0, sizeof *m);
}

int ws_recv(ws_t *w, int64_t timeout_ms, volatile sig_atomic_t *cancel, ws_msg_t *out, char **err) {
    memset(out, 0, sizeof *out);
    if (err) *err = NULL;
    if (!w || !w->c || w->closed) {
        if (err) *err = xstrdup("WebSocket is closed");
        return -1;
    }
    sb_t msg = w->frag; /* a fragmented message a timed-out call left in progress */
    int msg_op = w->frag_op;
    w->frag = (sb_t)SB_INIT;
    w->frag_op = -1;
    int64_t deadline = timeout_ms >= 0 ? mono_ms() + timeout_ms : 0;
    for (;;) {
        /* parse one frame if complete */
        if (w->len >= 2) {
            unsigned char *p = (unsigned char *)w->buf;
            bool fin = p[0] & 0x80;
            int op = p[0] & 0x0f;
            bool masked = p[1] & 0x80;
            uint64_t plen = p[1] & 0x7f;
            size_t hl = 2;
            if (plen == 126) {
                if (w->len < 4) goto more;
                plen = ((uint64_t)p[2] << 8) | p[3];
                hl = 4;
            } else if (plen == 127) {
                if (w->len < 10) goto more;
                plen = 0;
                for (int i = 0; i < 8; i++) plen = (plen << 8) | p[2 + i];
                hl = 10;
            }
            if (p[0] & 0x70) {
                if (err) *err = xstrdup("WebSocket protocol error: reserved bits set");
                goto proto;
            }
            if (masked) {
                if (err) *err = xstrdup("WebSocket protocol error: masked server frame");
                goto proto;
            }
            bool control = op >= 8;
            if (control && (plen > 125 || !fin)) {
                if (err) *err = xstrdup("WebSocket protocol error: invalid control frame");
                goto proto;
            }
            if (!control && op != WS_OP_CONT && op != WS_OP_TEXT && op != WS_OP_BINARY) {
                if (err) *err = xasprintf("WebSocket protocol error: unknown opcode %d", op);
                goto proto;
            }
            if (plen > w->max_message || msg.len + plen > w->max_message) {
                if (err) *err = xasprintf("WebSocket message exceeds %zu bytes", w->max_message);
                send_frame(w, WS_OP_CLOSE, "\x03\xf1", 2); /* 1009 message too big */
                w->close_sent = true;
                goto proto;
            }
            if (w->len < hl + plen) goto more;
            const char *pl = w->buf + hl;
            size_t n = (size_t)plen;
            if (op == WS_OP_PING) {
                send_frame(w, WS_OP_PONG, pl, n);
                consume(w, hl + n);
                continue;
            }
            if (op == WS_OP_PONG) {
                consume(w, hl + n);
                continue;
            }
            if (op == WS_OP_CLOSE) {
                out->opcode = WS_OP_CLOSE;
                out->close_code = n >= 2 ? (((unsigned char)pl[0] << 8) | (unsigned char)pl[1]) : 1005;
                if (n > 2) out->close_reason = xstrndup(pl + 2, n - 2);
                if (!w->close_sent) {
                    send_frame(w, WS_OP_CLOSE, pl, n >= 2 ? 2 : 0);
                    w->close_sent = true;
                }
                consume(w, hl + n);
                w->closed = true;
                sb_free(&msg);
                return 1;
            }
            if (op == WS_OP_CONT) {
                if (msg_op < 0) {
                    if (err) *err = xstrdup("WebSocket protocol error: unexpected continuation frame");
                    goto proto;
                }
            } else {
                if (msg_op >= 0) {
                    if (err) *err = xstrdup("WebSocket protocol error: interleaved data frame");
                    goto proto;
                }
                msg_op = op;
            }
            sb_appendn(&msg, pl, n);
            consume(w, hl + n);
            if (fin) {
                out->opcode = msg_op;
                out->len = msg.len;
                out->data = sb_detach(&msg);
                if (!out->data) out->data = xstrdup("");
                return 1;
            }
            continue;
        }
    more:;
        int f = fill(w, deadline, cancel);
        if (f == 1) continue;
        if (f == -2 || f == -4) {
            /* timed out or cancelled between frames: keep the fragments consumed so far for the next call */
            w->frag = msg;
            w->frag_op = msg_op;
            return f;
        }
        sb_free(&msg);
        w->closed = true;
        if (err) *err = xstrdup(f == 0 ? "WebSocket connection closed without a close frame" : "WebSocket read failed");
        return -1;
    }
proto:
    sb_free(&msg);
    w->closed = true;
    return -1;
}

void ws_close(ws_t *w, int code, const char *reason) {
    if (!w) return;
    if (w->c && !w->close_sent && !w->closed && code > 0) {
        sb_t b = SB_INIT;
        sb_putc(&b, (char)((code >> 8) & 0xff));
        sb_putc(&b, (char)(code & 0xff));
        if (reason) sb_appendn(&b, reason, MIN(strlen(reason), (size_t)120));
        send_frame(w, WS_OP_CLOSE, b.s, b.len);
        sb_free(&b);
    }
    net_close(w->c);
    free(w->buf);
    sb_free(&w->frag);
    free(w);
}
