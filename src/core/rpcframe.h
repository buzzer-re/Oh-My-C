/* rpcframe.h - Content-Length message framing of the LSP / DAP base protocol (port of omp
 * jsonrpc/message-framing.ts MessageFramer): `Content-Length: <n>\r\n\r\n` followed by <n> bytes of JSON. */
#ifndef OMC_RPCFRAME_H
#define OMC_RPCFRAME_H
#include "core.h"

#define RPCFRAME_MAX_HEADER (16u * 1024u)
#define RPCFRAME_MAX_CONTENT (256u * 1024u * 1024u)

typedef enum {
    RPCFRAME_NEED = 0,   /* no complete message buffered yet */
    RPCFRAME_MSG = 1,    /* *body / *len: the next message (borrowed from buf until rpcframe_consume) */
    RPCFRAME_RESYNC = 2, /* a header block without Content-Length was dropped (non-protocol noise on stdout) */
    RPCFRAME_ERROR = -1, /* the stream is unusable: *err (owned) holds omp's MessageFramingError text */
} rpcframe_rc_t;

/* Look at the front of `buf`. On RPCFRAME_MSG the message occupies the first *consume bytes of buf (header + body);
 * call rpcframe_consume(buf, *consume) once done with *body. RESYNC already dropped the junk header. */
rpcframe_rc_t rpcframe_next(sb_t *buf, const char **body, size_t *len, size_t *consume, char **err);
void rpcframe_consume(sb_t *buf, size_t n);

#endif
