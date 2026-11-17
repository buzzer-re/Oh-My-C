/* fuzz_rpcframe - Content-Length framing of LSP / DAP stdout (src/core/rpcframe.c) and the newline framing of MCP
 * stdio, fed in arbitrary chunks. Input: <chunk size byte><bytes>. */
#include "core/core.h"
#include "core/json.h"
#include "core/rpcframe.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 1 << 20) return 0;
    size_t chunk = fuzz_byte(&data, &size) % 128 + 1;
    sb_t buf = SB_INIT;
    size_t total = 0;
    bool dead = false;
    for (size_t i = 0; i < size && !dead; i += chunk) {
        size_t k = MIN(chunk, size - i);
        sb_appendn(&buf, (const char *)data + i, k);
        total += k;
        for (;;) {
            const char *body;
            size_t bl, used;
            char *err = NULL;
            rpcframe_rc_t rc = rpcframe_next(&buf, &body, &bl, &used, &err);
            if (rc == RPCFRAME_ERROR) {
                FUZZ_ASSERT(err);
                free(err);
                dead = true;
                break;
            }
            FUZZ_ASSERT(!err);
            if (rc == RPCFRAME_RESYNC) continue;
            if (rc == RPCFRAME_NEED) break;
            FUZZ_ASSERT(used <= buf.len && bl <= used && body + bl == buf.s + used);
            json_t *m = json_parse(body, bl, NULL);
            json_free(m);
            rpcframe_consume(&buf, used);
        }
        FUZZ_ASSERT(buf.len <= total);
    }
    sb_free(&buf);
    return 0;
}
