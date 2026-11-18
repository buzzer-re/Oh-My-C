/* rpcframe.c - Content-Length framing for the LSP and DAP stdio clients (omp jsonrpc/message-framing.ts). */
#define _GNU_SOURCE /* memmem: glibc declares it only with _GNU_SOURCE (build-tools/check_apple.sh) */
#include "rpcframe.h"

#include <ctype.h>

void rpcframe_consume(sb_t *buf, size_t n) {
    if (n >= buf->len) {
        sb_clear(buf);
        return;
    }
    memmove(buf->s, buf->s + n, buf->len - n);
    sb_truncate(buf, buf->len - n);
}

/* `^Content-Length:[ \t]*([^\r\n]*)$` (multiline, case-insensitive) over the header block [h, h+n): number of
 * matches, the first value in *v / *vl */
static int find_lengths(const char *h, size_t n, const char **v, size_t *vl) {
    static const char key[] = "content-length:";
    const size_t kl = sizeof key - 1;
    int count = 0;
    for (size_t i = 0; i < n;) {
        /* i is at a line start: after the block start, '\r' or '\n' */
        if (n - i >= kl && strncasecmp(h + i, key, kl) == 0) {
            size_t j = i + kl;
            while (j < n && (h[j] == ' ' || h[j] == '\t')) j++;
            size_t e = j;
            while (e < n && h[e] != '\r' && h[e] != '\n') e++;
            if (!count) {
                *v = h + j;
                *vl = e - j;
            }
            count++;
        }
        while (i < n && h[i] != '\r' && h[i] != '\n') i++;
        if (i < n) i++;
    }
    return count;
}

rpcframe_rc_t rpcframe_next(sb_t *buf, const char **body, size_t *len, size_t *consume, char **err) {
    *body = NULL;
    *len = *consume = 0;
    if (err) *err = NULL;
    /* header terminator, scanned only through the first MAX_HEADER bytes */
    size_t scan = MIN(buf->len, (size_t)RPCFRAME_MAX_HEADER);
    const char *he = scan >= 4 ? memmem(buf->s, scan, "\r\n\r\n", 4) : NULL;
    if (!he) {
        if (buf->len >= RPCFRAME_MAX_HEADER) {
            if (err) *err = xasprintf("JSON-RPC header exceeds %u-byte limit", RPCFRAME_MAX_HEADER);
            return RPCFRAME_ERROR;
        }
        return RPCFRAME_NEED;
    }
    size_t hl = (size_t)(he - buf->s);
    const char *v = NULL;
    size_t vl = 0;
    int nlen = find_lengths(buf->s, hl, &v, &vl);
    if (nlen == 0) {
        rpcframe_consume(buf, hl + 4);
        return RPCFRAME_RESYNC;
    }
    /* trim, then /^\d+$/ */
    while (vl && isspace((unsigned char)*v)) v++, vl--;
    while (vl && isspace((unsigned char)v[vl - 1])) vl--;
    bool digits = vl > 0;
    for (size_t i = 0; i < vl; i++)
        if (v[i] < '0' || v[i] > '9') digits = false;
    if (nlen != 1 || !digits) {
        if (err) *err = xstrdup("Invalid or duplicate JSON-RPC Content-Length");
        return RPCFRAME_ERROR;
    }
    uint64_t cl = 0;
    bool over = false;
    for (size_t i = 0; i < vl && !over; i++) {
        cl = cl * 10 + (uint64_t)(v[i] - '0');
        if (cl > RPCFRAME_MAX_CONTENT) over = true;
    }
    if (over) {
        if (err) *err = xasprintf("JSON-RPC Content-Length exceeds %u-byte limit", RPCFRAME_MAX_CONTENT);
        return RPCFRAME_ERROR;
    }
    size_t start = hl + 4;
    if (buf->len - start < cl) return RPCFRAME_NEED;
    *body = buf->s + start;
    *len = (size_t)cl;
    *consume = start + (size_t)cl;
    return RPCFRAME_MSG;
}
