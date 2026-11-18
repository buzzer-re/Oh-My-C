/* fuzz_sse - the SSE parser (src/net/http.c) feeding each provider's stream state machine: Anthropic Messages,
 * OpenAI Chat Completions, OpenAI Responses, Google generateContent, plus Ollama's NDJSON stream.
 * Input: <selector byte><chunk size byte><stream bytes>. Every decoder must end in a well-formed assistant message. */
#include "core/core.h"
#include "core/json.h"
#include "net/http.h"
#include "ai/ai.h"
#include "ai/provider_anthropic.h"
#include "ai/provider_openai.h"
#include "ai/provider_google.h"
#include "fuzz.h"

static json_t *g_raw[4];
static ai_model_t g_m[4];

static void mk(int i, const char *js) {
    g_raw[i] = json_parse_str(js);
    ai_model_t *m = &g_m[i];
    memset(m, 0, sizeof *m);
    m->raw = g_raw[i];
    m->id = json_get_str(g_raw[i], "id");
    m->name = m->id;
    m->api = json_get_str(g_raw[i], "api");
    m->provider = json_get_str(g_raw[i], "provider");
    m->base_url = json_get_str(g_raw[i], "baseUrl");
    m->reasoning = true;
    m->input_image = true;
    m->cost_input = 3;
    m->cost_output = 15;
    m->context_window = 200000;
    m->max_tokens = 64000;
    m->compat = json_get_obj(g_raw[i], "compat");
    m->thinking = json_get_obj(g_raw[i], "thinking");
}

FUZZ_SETUP {
    mk(0, "{\"id\":\"claude-sonnet-4-5\",\"api\":\"anthropic-messages\",\"provider\":\"anthropic\","
          "\"baseUrl\":\"https://api.anthropic.com\",\"thinking\":{\"mode\":\"budget\"},\"compat\":{}}");
    mk(1, "{\"id\":\"m1\",\"api\":\"openai-completions\",\"provider\":\"test\",\"baseUrl\":\"http://x\",\"compat\":{}}");
    mk(2, "{\"id\":\"gpt-5\",\"api\":\"openai-responses\",\"provider\":\"openai\",\"baseUrl\":\"http://x\",\"compat\":{}}");
    mk(3, "{\"id\":\"gemini-2.5-flash\",\"api\":\"google-generative-ai\",\"provider\":\"google\",\"baseUrl\":\"http://x\"}");
}

static size_t g_events;
static void on_ev(void *ud, const ai_event_t *ev) {
    (void)ud;
    (void)ev;
    g_events++;
}

static int feed(sse_parser_t *p, const uint8_t *d, size_t n, size_t chunk) {
    int rc = 0;
    for (size_t i = 0; i < n && !rc; i += chunk) rc = sse_feed(p, (const char *)d + i, MIN(chunk, n - i));
    if (!rc) rc = sse_finish(p);
    return rc;
}

static void check_msg(json_t *msg) {
    FUZZ_ASSERT(msg);
    FUZZ_ASSERT(json_get_str(msg, "stopReason"));
    FUZZ_ASSERT(json_is_arr(json_get(msg, "content")));
    char *s = json_dump(msg, 0);
    json_t *k = json_parse(s, strlen(s), NULL);
    FUZZ_ASSERT(k);
    json_free(k);
    free(s);
    json_free(msg);
}

FUZZ_ENTRY {
    if (size > 1 << 18) return 0;
    unsigned sel = fuzz_byte(&data, &size);
    size_t chunk = fuzz_byte(&data, &size) % 64 + 1;
    bool cb = sel & 0x10;
    sse_parser_t p;
    switch (sel % 5) {
    case 0: {
        anthropic_sse_t s;
        anthropic_sse_init(&s, &g_m[0], sel & 0x20, cb ? on_ev : NULL, NULL);
        sse_init(&p, anthropic_sse_event, &s);
        int rc = feed(&p, data, size, chunk);
        sse_free(&p);
        check_msg(anthropic_sse_finish(&s, rc == 2 ? 2 : 0, NULL));
        anthropic_sse_free(&s);
        break;
    }
    case 1: {
        oaic_stream_t *s = oaic_stream_new(&g_m[1], cb ? on_ev : NULL, NULL);
        sse_init(&p, oaic_stream_on_sse, s);
        int rc = feed(&p, data, size, chunk);
        sse_free(&p);
        check_msg(oaic_stream_finish(s, rc == OAI_STREAM_STOP ? rc : 0, NULL));
        break;
    }
    case 2: {
        oair_stream_t *s = oair_stream_new(&g_m[2], cb ? on_ev : NULL, NULL);
        sse_init(&p, oair_stream_on_sse, s);
        int rc = feed(&p, data, size, chunk);
        sse_free(&p);
        check_msg(oair_stream_finish(s, rc == OAI_STREAM_STOP ? rc : 0, NULL));
        break;
    }
    case 3: {
        google_sse_t s;
        google_sse_init(&s, &g_m[3], cb ? on_ev : NULL, NULL);
        sse_init(&p, google_sse_event, &s);
        int rc = feed(&p, data, size, chunk);
        sse_free(&p);
        (void)google_sse_empty_stop(&s, rc);
        check_msg(google_sse_finish(&s, rc, NULL));
        google_sse_free(&s);
        break;
    }
    case 4: {
        ollama_stream_t *s = ollama_stream_new(&g_m[1], cb ? on_ev : NULL, NULL);
        int rc = 0;
        for (size_t i = 0; i < size && !rc; i += chunk) rc = ollama_stream_on_data(s, (const char *)data + i, MIN(chunk, size - i));
        check_msg(ollama_stream_finish(s, rc == OAI_STREAM_STOP ? rc : 0, NULL));
        break;
    }
    }
    return 0;
}
