/* cursor_proto.h - Cursor agent protocol schema (protobuf subset) and Connect framing helpers. */
#ifndef OMC_CURSOR_PROTO_H
#define OMC_CURSOR_PROTO_H

#include "ai/pb.h"
#include "core/core.h"
#include "core/json.h"

/* Process-wide parsed schema (lazily built, never freed). */
const pb_schema_t *cursor_proto(void);
const pb_msg_t *cursor_msg(const char *name); /* aborts on unknown names (programming error) */
/* encode / decode by message name */
char *cursor_encode(const char *name, const json_t *val, size_t *len); /* owned bytes */
json_t *cursor_decode(const char *name, const void *data, size_t len, char **err);

/* ---------- Connect protocol envelope: 1 flag byte + 4-byte big-endian length + payload ---------- */
#define CONNECT_FLAG_COMPRESSED 0x01
#define CONNECT_FLAG_END_STREAM 0x02

/* Append one framed message to `out`. */
void connect_frame(sb_t *out, int flags, const void *data, size_t len);

typedef struct {
    sb_t buf;           /* buffered bytes */
    size_t off;         /* consumed prefix of buf */
    size_t max_payload; /* 0 => unlimited */
} connect_decoder_t;

void connect_decoder_init(connect_decoder_t *d, size_t max_payload);
void connect_decoder_free(connect_decoder_t *d);
void connect_decoder_feed(connect_decoder_t *d, const void *data, size_t len);
/* Next complete frame: returns 1 and sets flags, payload and len (payload valid until the next feed),
 * 0 when more bytes are needed, -1 when a length prefix exceeds max_payload (len = announced length). */
int connect_decoder_next(connect_decoder_t *d, int *flags, const char **payload, size_t *len);

/* "Connect error code: message [details: ...]" (omp formatConnectEndStreamError). Owned. */
char *connect_format_end_stream_error(const json_t *error);
/* Summary of Connect error detail entries, NULL when nothing usable. Owned. */
char *connect_summarize_error_details(const json_t *details);
/* aiserver.v1.ErrorDetails entry whose debug.details.isRetryable is true */
bool connect_has_retryable_cursor_detail(const json_t *details);

#endif
