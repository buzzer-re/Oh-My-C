/* fuzz_pb - protobuf decoding of Cursor's Connect stream (src/ai/pb.c, src/ai/cursor_proto.c): Connect envelope
 * framing, then every message type of the schema and google.protobuf.Value. Input: <selector byte><bytes>. */
#include "core/core.h"
#include "core/json.h"
#include "ai/pb.h"
#include "ai/cursor_proto.h"
#include "fuzz.h"

static const char *const MSGS[] = {"AgentServerMessage", "ErrorDetails", "InteractionUpdate", "ExecServerMessage",
                                   "KvServerMessage", "ConversationStateStructure", "AgentClientMessage"};

FUZZ_SETUP { (void)cursor_proto(); }

FUZZ_ENTRY {
    if (size > 1 << 18) return 0;
    unsigned sel = fuzz_byte(&data, &size);
    if (sel & 0x80) {
        connect_decoder_t d;
        connect_decoder_init(&d, 1 << 16);
        size_t chunk = sel % 32 + 1;
        for (size_t i = 0; i < size; i += chunk) {
            connect_decoder_feed(&d, data + i, MIN(chunk, size - i));
            int fl;
            const char *p;
            size_t n;
            int rc;
            while ((rc = connect_decoder_next(&d, &fl, &p, &n)) == 1) {
                char *de = NULL;
                json_t *v = cursor_decode(MSGS[0], p, n, &de);
                free(de);
                json_free(v);
                if (fl & 2) {
                    json_t *j = json_parse(p, n, NULL);
                    if (j) {
                        char *e = connect_format_end_stream_error(json_get(j, "error"));
                        free(e);
                        char *sm = connect_summarize_error_details(json_get(json_get(j, "error"), "details"));
                        free(sm);
                        json_free(j);
                    }
                }
            }
            if (rc < 0) break;
        }
        connect_decoder_free(&d);
        return 0;
    }
    if (sel & 0x40) {
        json_t *v = pb_value_decode(data, size);
        if (v) {
            sb_t out = SB_INIT;
            pb_value_encode(v, &out);
            sb_free(&out);
            json_free(v);
        }
        return 0;
    }
    const char *name = MSGS[sel % (sizeof MSGS / sizeof *MSGS)];
    const pb_msg_t *m = pb_schema_msg(cursor_proto(), name);
    if (!m) return 0;
    char *err = NULL;
    json_t *v = pb_decode(m, data, size, &err);
    free(err);
    if (v) {
        sb_t out = SB_INIT;
        char *e2 = NULL;
        if (pb_encode(m, v, &out, &e2) == 0) {
            char *e3 = NULL;
            json_t *v2 = pb_decode(m, out.s ? out.s : "", out.len, &e3);
            free(e3);
            json_free(v2);
        }
        free(e2);
        sb_free(&out);
        json_free(v);
    }
    return 0;
}
