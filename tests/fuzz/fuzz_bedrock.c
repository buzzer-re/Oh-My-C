/* fuzz_bedrock - AWS event-stream binary framing (src/ai/aws.c) and the Bedrock ConverseStream state machine
 * (src/ai/provider_bedrock.c). Input: <chunk size byte><raw event-stream bytes>; selector bit 0x80 of the first byte
 * instead decodes the remainder as one buffered message. */
#include "core/core.h"
#include "core/json.h"
#include "ai/ai.h"
#include "ai/aws.h"
#include "ai/provider_bedrock.h"
#include "fuzz.h"

static json_t *g_raw;
static ai_model_t g_m;

FUZZ_SETUP {
    g_raw = json_parse_str("{\"id\":\"anthropic.claude-sonnet-4-5\",\"api\":\"bedrock-converse-stream\","
                           "\"provider\":\"amazon-bedrock\",\"baseUrl\":\"https://bedrock-runtime.us-east-1.amazonaws.com\"}");
    g_m.raw = g_raw;
    g_m.id = json_get_str(g_raw, "id");
    g_m.name = g_m.id;
    g_m.api = json_get_str(g_raw, "api");
    g_m.provider = json_get_str(g_raw, "provider");
    g_m.base_url = json_get_str(g_raw, "baseUrl");
    g_m.reasoning = true;
    g_m.context_window = 200000;
    g_m.max_tokens = 64000;
}

FUZZ_ENTRY {
    if (size > 1 << 18) return 0;
    unsigned sel = fuzz_byte(&data, &size);
    if (sel & 0x80) {
        aws_es_msg_t m;
        char *err = NULL;
        if (aws_es_decode_message(data, size, &m, &err) == 0) {
            FUZZ_ASSERT(m.payload_len <= size);
            if (m.payload_len) FUZZ_ASSERT(m.payload >= data && m.payload + m.payload_len <= data + size);
            aws_es_msg_free(&m);
        }
        free(err);
        return 0;
    }
    size_t chunk = sel % 64 + 1;
    bedrock_stream_t s;
    bedrock_stream_init(&s, &g_m, sel & 0x40, NULL, NULL);
    int rc = 0;
    for (size_t i = 0; i < size && !rc; i += chunk) rc = bedrock_stream_feed(&s, (const char *)data + i, MIN(chunk, size - i));
    json_t *msg = bedrock_stream_finish(&s, rc, NULL, false);
    FUZZ_ASSERT(msg && json_get_str(msg, "stopReason"));
    json_free(msg);
    bedrock_stream_free(&s);
    return 0;
}
