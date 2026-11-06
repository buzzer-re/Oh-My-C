/* test_providers_oai.c - offline tests for the OpenAI-family providers (request building + stream decoding). */
#include "ai/provider_openai.h"

#include <math.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        g_checks++;                                                               \
        if (!(c)) {                                                               \
            g_fail++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                         \
    } while (0)
#define CHECK_STR(a, b)                                                                                   \
    do {                                                                                                  \
        const char *a_ = (a), *b_ = (b);                                                                  \
        g_checks++;                                                                                       \
        if (!str_eq(a_, b_)) {                                                                            \
            g_fail++;                                                                                     \
            fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\" (%s)\n", __FILE__, __LINE__, b_ ? b_ : "(null)", \
                    a_ ? a_ : "(null)", #a);                                                              \
        }                                                                                                 \
    } while (0)

/* ---------- catalog stubs ---------- */
ai_effort_t model_clamp_effort(const ai_model_t *m, ai_effort_t e) { return e; }
char *ai_env_api_key(const char *provider) { return NULL; }
bool model_supports_effort(const ai_model_t *m, ai_effort_t e) { return true; }

/* ---------- helpers ---------- */
static json_t *J(const char *s) {
    char *err = NULL;
    json_t *j = json_parse(s, strlen(s), &err);
    if (!j) {
        fprintf(stderr, "bad test json: %s\n%s\n", err ? err : "?", s);
        exit(2);
    }
    return j;
}

typedef struct {
    json_t *raw;
    ai_model_t m;
} tmodel_t;

static void model_init(tmodel_t *t, const char *json) {
    t->raw = J(json);
    memset(&t->m, 0, sizeof(t->m));
    t->m.raw = t->raw;
    t->m.id = json_get_str(t->raw, "id");
    t->m.api = json_get_str(t->raw, "api");
    t->m.provider = json_get_str(t->raw, "provider");
    t->m.base_url = json_get_str(t->raw, "baseUrl");
    t->m.reasoning = json_get_bool(t->raw, "reasoning", false);
    t->m.input_image = json_get_bool(t->raw, "image", false);
    t->m.max_tokens = json_get_int(t->raw, "maxTokens", 0);
    t->m.context_window = 200000;
    t->m.cost_input = 1.0;
    t->m.cost_output = 2.0;
    t->m.compat = json_get_obj(t->raw, "compat");
    t->m.thinking = json_get_obj(t->raw, "thinking");
    t->m.headers = json_get_obj(t->raw, "headers");
}

typedef struct {
    int counts[AI_EV_ERROR + 1];
    sb_t text, thinking, args;
} evlog_t;

static void on_ev(void *ud, const ai_event_t *ev) {
    evlog_t *l = ud;
    l->counts[ev->type]++;
    if (ev->type == AI_EV_TEXT_DELTA) sb_appendn(&l->text, ev->delta, ev->delta_len);
    if (ev->type == AI_EV_THINKING_DELTA) sb_appendn(&l->thinking, ev->delta, ev->delta_len);
    if (ev->type == AI_EV_TOOLCALL_DELTA && ev->delta) sb_appendn(&l->args, ev->delta, ev->delta_len);
    CHECK(ev->partial != NULL);
}

static void evlog_free(evlog_t *l) {
    sb_free(&l->text);
    sb_free(&l->thinking);
    sb_free(&l->args);
}

static json_t *block_of(const json_t *msg, const char *type, int nth) {
    json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) {
        if (str_eq(json_get_str(b, "type"), type) && nth-- == 0) return b;
    }
    return NULL;
}

static int feed_sse(ai_sse_cb cb, void *st, const char *raw, size_t chunk) {
    sse_parser_t p;
    sse_init(&p, (sse_event_cb)cb, st);
    size_t n = strlen(raw);
    int rc = 0;
    for (size_t off = 0; off < n && !rc; off += chunk) rc = sse_feed(&p, raw + off, MIN(chunk, n - off));
    if (!rc) rc = sse_finish(&p);
    sse_free(&p);
    return rc;
}

static const char *CTX_MESSAGES =
    "["
    "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"look\"},{\"type\":\"image\",\"data\":\"QUJD\",\"mimeType\":\"image/png\"}],\"timestamp\":1},"
    "{\"role\":\"assistant\",\"api\":\"openai-completions\",\"provider\":\"test\",\"model\":\"m1\",\"stopReason\":\"toolUse\","
    " \"content\":[{\"type\":\"thinking\",\"thinking\":\"hmm\",\"thinkingSignature\":\"reasoning_content\"},"
    "  {\"type\":\"text\",\"text\":\"calling\"},"
    "  {\"type\":\"toolCall\",\"id\":\"call_abc-123_xyz\",\"name\":\"read\",\"arguments\":{\"path\":\"a.txt\"}}]},"
    "{\"role\":\"toolResult\",\"toolCallId\":\"call_abc-123_xyz\",\"toolName\":\"read\",\"content\":[{\"type\":\"text\",\"text\":\"file body\"}],\"isError\":false},"
    "{\"role\":\"user\",\"content\":\"thanks\",\"timestamp\":2}"
    "]";

static const char *TOOLS =
    "[{\"name\":\"read\",\"description\":\"Read a file\",\"parameters\":{\"type\":\"object\",\"properties\":"
    "{\"path\":{\"type\":\"string\",\"description\":\"p\"},\"limit\":{\"type\":\"integer\",\"description\":\"max lines\"}},"
    "\"required\":[\"path\"]}}]";

/* ====================================================================== */

static void test_completions_request(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"m1\",\"api\":\"openai-completions\",\"provider\":\"test\",\"reasoning\":true,\"image\":true,"
                   "\"maxTokens\":32000,\"thinking\":{\"efforts\":[\"minimal\",\"low\",\"medium\",\"high\"]},"
                   "\"compat\":{\"supportsDeveloperRole\":true,\"supportsStrictMode\":true,\"maxTokensField\":\"max_completion_tokens\","
                   "\"thinkingFormat\":\"openai\",\"reasoningDisableMode\":\"lowest-effort\",\"supportsUsageInStreaming\":true,"
                   "\"supportsStore\":true,\"replayReasoningContent\":true}}");
    json_t *msgs = J(CTX_MESSAGES), *sys = J("[\"You are helpful.\",\"  \"]"), *tools = J(TOOLS);
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_HIGH;
    o.temperature = 0.5;
    json_t *tc = J("{\"type\":\"tool\",\"name\":\"read\"}");
    o.tool_choice = tc;
    json_t *p = oaic_build_request(&t.m, &ctx, &o);

    CHECK_STR(json_get_str(p, "model"), "m1");
    CHECK(json_get_bool(p, "stream", false));
    CHECK(json_get_bool(json_get(p, "stream_options"), "include_usage", false));
    CHECK(json_get(p, "store") && !json_get_bool(p, "store", true));
    CHECK_STR(json_get_str(p, "reasoning_effort"), "high");
    CHECK(json_get_int(p, "max_completion_tokens", 0) == 32000);
    CHECK(fabs(json_get_num(p, "temperature", 0) - 0.5) < 1e-9);
    CHECK_STR(json_get_str(json_get_obj(json_get_obj(p, "tool_choice"), "function"), "name"), "read");

    json_t *m = json_get(p, "messages");
    CHECK(json_len(m) == 5);
    CHECK_STR(json_get_str(json_at(m, 0), "role"), "developer");
    CHECK_STR(json_get_str(json_at(m, 0), "content"), "You are helpful.");
    json_t *u = json_at(m, 1);
    CHECK_STR(json_get_str(u, "role"), "user");
    CHECK_STR(json_get_str(json_at(json_get(u, "content"), 1), "type"), "image_url");
    CHECK_STR(json_get_str(json_get_obj(json_at(json_get(u, "content"), 1), "image_url"), "url"), "data:image/png;base64,QUJD");
    json_t *a = json_at(m, 2);
    CHECK_STR(json_get_str(a, "role"), "assistant");
    CHECK_STR(json_get_str(a, "content"), "calling");
    CHECK_STR(json_get_str(a, "reasoning_content"), "hmm");
    json_t *call = json_at(json_get(a, "tool_calls"), 0);
    CHECK_STR(json_get_str(call, "id"), "call_abc-123_xyz");
    CHECK_STR(json_get_str(json_get_obj(call, "function"), "arguments"), "{\"path\":\"a.txt\"}");
    json_t *tr = json_at(m, 3);
    CHECK_STR(json_get_str(tr, "role"), "tool");
    CHECK_STR(json_get_str(tr, "tool_call_id"), "call_abc-123_xyz");
    CHECK_STR(json_get_str(tr, "content"), "file body");
    CHECK(json_get(tr, "name") == NULL);
    CHECK_STR(json_get_str(json_at(m, 4), "content"), "thanks");

    json_t *fn = json_get_obj(json_at(json_get(p, "tools"), 0), "function");
    CHECK(json_get_bool(fn, "strict", false));
    json_t *params = json_get_obj(fn, "parameters");
    CHECK(json_get(params, "additionalProperties") && !json_get_bool(params, "additionalProperties", true));
    CHECK(json_len(json_get(params, "required")) == 2);
    json_t *limit = json_get_obj(json_get_obj(params, "properties"), "limit");
    CHECK(json_len(json_get(limit, "anyOf")) == 2);
    CHECK_STR(json_get_str(limit, "description"), "max lines");

    /* caller-disabled reasoning on lowest-effort mode -> lowest effort */
    o.reasoning = EFFORT_OFF;
    o.disable_reasoning = true;
    json_t *p2 = oaic_build_request(&t.m, &ctx, &o);
    CHECK_STR(json_get_str(p2, "reasoning_effort"), "minimal");
    json_free(p2);

    json_free(p);
    json_free(tc);
    json_free(msgs);
    json_free(sys);
    json_free(tools);
    json_free(t.raw);
}

static void test_completions_mistral(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"devstral\",\"api\":\"openai-completions\",\"provider\":\"mistral\",\"reasoning\":false,"
                   "\"maxTokens\":8192,\"compat\":{\"requiresMistralToolIds\":true,\"requiresToolResultName\":true,"
                   "\"requiresAssistantAfterToolResult\":true,\"requiresThinkingAsText\":true,\"maxTokensField\":\"max_tokens\","
                   "\"supportsStrictMode\":false,\"supportsUsageInStreaming\":false}}");
    /* same-api history from the same model so thinking survives transform */
    json_t *msgs = J("[{\"role\":\"user\",\"content\":\"hi\"},"
                     "{\"role\":\"assistant\",\"api\":\"openai-completions\",\"provider\":\"mistral\",\"model\":\"devstral\",\"stopReason\":\"toolUse\","
                     " \"content\":[{\"type\":\"thinking\",\"thinking\":\"plan\"},{\"type\":\"toolCall\",\"id\":\"call_1|fc_x\",\"name\":\"ls\",\"arguments\":{}}]},"
                     "{\"role\":\"toolResult\",\"toolCallId\":\"call_1|fc_x\",\"toolName\":\"ls\",\"content\":[{\"type\":\"text\",\"text\":\"a\"}]},"
                     "{\"role\":\"user\",\"content\":\"next\"}]");
    json_t *tools = J(TOOLS);
    ai_context_t ctx = {.messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    json_t *p = oaic_build_request(&t.m, &ctx, &o);
    CHECK(json_get(p, "stream_options") == NULL);
    CHECK(json_get_int(p, "max_tokens", 0) == 8192);
    CHECK(json_get(p, "max_completion_tokens") == NULL);
    CHECK(json_get(json_get_obj(json_at(json_get(p, "tools"), 0), "function"), "strict") == NULL);
    json_t *m = json_get(p, "messages");
    CHECK(json_len(m) == 5);
    json_t *a = json_at(m, 1);
    CHECK_STR(json_get_str(a, "content"), "<think>\nplan\n</think>");
    const char *id = json_get_str(json_at(json_get(a, "tool_calls"), 0), "id");
    CHECK(id && strlen(id) == 9);
    CHECK_STR(id, "call1fcxA");
    json_t *tr = json_at(m, 2);
    CHECK_STR(json_get_str(tr, "tool_call_id"), id);
    CHECK_STR(json_get_str(tr, "name"), "ls");
    CHECK_STR(json_get_str(json_at(m, 3), "role"), "assistant");
    CHECK_STR(json_get_str(json_at(m, 3), "content"), "I have processed the tool results.");
    CHECK_STR(json_get_str(json_at(m, 4), "role"), "user");
    json_free(p);
    json_free(msgs);
    json_free(tools);
    json_free(t.raw);
}

static void test_completions_dialects(void) {
    json_t *msgs = J("[{\"role\":\"user\",\"content\":\"hi\"}]");
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_MEDIUM;

    tmodel_t orr;
    model_init(&orr, "{\"id\":\"anthropic/claude\",\"api\":\"openrouter\",\"provider\":\"openrouter\",\"reasoning\":true,\"maxTokens\":64000,"
                     "\"compat\":{\"thinkingFormat\":\"openrouter\",\"reasoningDisableMode\":\"openrouter-enabled-false\",\"isOpenRouterHost\":true,"
                     "\"cacheControlFormat\":\"anthropic\"}}");
    json_t *p = oaic_build_request(&orr.m, &ctx, &o);
    CHECK_STR(json_get_str(json_get_obj(p, "reasoning"), "effort"), "medium");
    CHECK(json_get(p, "reasoning_effort") == NULL);
    CHECK(json_get(p, "max_completion_tokens") == NULL); /* openrouter: catalog default omitted */
    json_t *um = json_at(json_get(p, "messages"), 0);
    CHECK_STR(json_get_str(json_get_obj(json_at(json_get(um, "content"), 0), "cache_control"), "type"), "ephemeral");
    json_free(p);
    o.reasoning = EFFORT_OFF;
    o.disable_reasoning = true;
    p = oaic_build_request(&orr.m, &ctx, &o);
    CHECK(json_get(json_get_obj(p, "reasoning"), "enabled") && !json_get_bool(json_get_obj(p, "reasoning"), "enabled", true));
    json_free(p);
    json_free(orr.raw);

    tmodel_t zai;
    model_init(&zai, "{\"id\":\"glm-5\",\"api\":\"openai-completions\",\"provider\":\"zai\",\"reasoning\":true,"
                     "\"compat\":{\"thinkingFormat\":\"zai\",\"reasoningDisableMode\":\"zai-thinking-disabled\",\"supportsReasoningEffort\":false}}");
    o.disable_reasoning = false;
    o.reasoning = EFFORT_HIGH;
    p = oaic_build_request(&zai.m, &ctx, &o);
    CHECK_STR(json_get_str(json_get_obj(p, "thinking"), "type"), "enabled");
    CHECK(json_get(p, "reasoning_effort") == NULL);
    json_free(p);
    o.reasoning = EFFORT_OFF; /* not requested -> implicit disable */
    p = oaic_build_request(&zai.m, &ctx, &o);
    CHECK_STR(json_get_str(json_get_obj(p, "thinking"), "type"), "disabled");
    json_free(p);
    json_free(zai.raw);

    tmodel_t qwen;
    model_init(&qwen, "{\"id\":\"qwen3\",\"api\":\"openai-completions\",\"provider\":\"dashscope\",\"reasoning\":true,"
                      "\"compat\":{\"thinkingFormat\":\"qwen\",\"supportsMultipleSystemMessages\":false}}");
    o.reasoning = EFFORT_LOW;
    json_t *sys = J("[\"a\",\"b\"]");
    ctx.system_prompt = sys;
    p = oaic_build_request(&qwen.m, &ctx, &o);
    CHECK(json_get_bool(p, "enable_thinking", false));
    CHECK_STR(json_get_str(json_at(json_get(p, "messages"), 0), "content"), "a\n\nb");
    CHECK_STR(json_get_str(json_at(json_get(p, "messages"), 0), "role"), "system");
    json_free(p);
    json_free(sys);
    json_free(qwen.raw);
    json_free(msgs);
}

static void test_completions_stream(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"m1\",\"api\":\"openai-completions\",\"provider\":\"test\",\"reasoning\":true,\"compat\":{}}");
    const char *sse =
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"Let me \"}}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"think.\"}}]}\n\n"
        ": keepalive\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello \"}}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"world\"}}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_A\",\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]}}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"x.c\\\"}\"}}]}}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call_B\",\"function\":{\"name\":\"ls\",\"arguments\":\"{\\\"dir\\\"\"}}]}}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\"}}]}}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"function\":{\"arguments\":\":\\\"src\\\"}\"}}]}}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: {\"id\":\"chatcmpl-1\",\"choices\":[],\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":20,"
        "\"prompt_tokens_details\":{\"cached_tokens\":40},\"completion_tokens_details\":{\"reasoning_tokens\":7}}}\n\n"
        "data: [DONE]\n\n";
    size_t chunks[] = {1, 7, 4096};
    for (int ci = 0; ci < 3; ci++) {
        evlog_t l = {0};
        oaic_stream_t *st = oaic_stream_new(&t.m, on_ev, &l);
        int rc = feed_sse(oaic_stream_on_sse, st, sse, chunks[ci]);
        CHECK(rc == OAI_STREAM_STOP);
        json_t *msg = oaic_stream_finish(st, rc, NULL);
        CHECK_STR(json_get_str(msg, "stopReason"), "toolUse");
        CHECK_STR(json_get_str(msg, "responseId"), "chatcmpl-1");
        json_t *th = block_of(msg, "thinking", 0);
        CHECK_STR(json_get_str(th, "thinking"), "Let me think.");
        CHECK_STR(json_get_str(th, "thinkingSignature"), "reasoning_content");
        CHECK_STR(json_get_str(block_of(msg, "text", 0), "text"), "Hello world");
        json_t *a = block_of(msg, "toolCall", 0), *b = block_of(msg, "toolCall", 1);
        CHECK_STR(json_get_str(a, "id"), "call_A");
        CHECK_STR(json_get_str(a, "name"), "read");
        CHECK_STR(json_get_str(json_get(a, "arguments"), "path"), "x.c");
        CHECK_STR(json_get_str(b, "id"), "call_B");
        CHECK_STR(json_get_str(json_get(b, "arguments"), "dir"), "src");
        json_t *u = json_get(msg, "usage");
        CHECK(json_get_int(u, "input", -1) == 60);
        CHECK(json_get_int(u, "cacheRead", -1) == 40);
        CHECK(json_get_int(u, "output", -1) == 20);
        CHECK(json_get_int(u, "reasoningTokens", -1) == 7);
        CHECK(json_get_int(u, "totalTokens", -1) == 120);
        CHECK(fabs(json_get_num(json_get(u, "cost"), "input", 0) - 60e-6) < 1e-12);
        CHECK(l.counts[AI_EV_START] == 1 && l.counts[AI_EV_DONE] == 1 && l.counts[AI_EV_ERROR] == 0);
        CHECK(l.counts[AI_EV_TOOLCALL_START] == 2 && l.counts[AI_EV_TOOLCALL_END] == 2);
        CHECK(l.counts[AI_EV_TEXT_END] == 1 && l.counts[AI_EV_THINKING_END] == 1);
        CHECK_STR(sb_cstr(&l.text), "Hello world");
        json_free(msg);
        evlog_free(&l);
    }
    json_free(t.raw);
}

static void test_completions_stream_errors(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"m1\",\"api\":\"openai-completions\",\"provider\":\"test\",\"compat\":{}}");
    struct {
        const char *sse;
        const char *stop;
        const char *err_sub;
    } cases[] = {
        {"data: {\"choices\":[{\"delta\":{\"content\":\"x\"},\"finish_reason\":\"content_filter\"}]}\n\ndata: [DONE]\n\n", "error",
         "content_filter"},
        {"data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\ndata: {\"error\":{\"message\":\"overloaded\",\"code\":503}}\n\n",
         "error", "503 overloaded"},
        {"data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n", "error", "before a finish_reason"},
        {"data: {\"choices\":[{\"delta\":{\"content\":\"long\"},\"finish_reason\":\"LENGTH\"}]}\n\n", "length", NULL},
        {"data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\ndata: [DONE]\n\n", "stop", NULL},
        {"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"c1\",\"function\":{\"name\":\"f\",\"arguments\":{\"a\":1}}}]},\"finish_reason\":\"stop\"}]}\n\n",
         "toolUse", NULL},
    };
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        evlog_t l = {0};
        oaic_stream_t *st = oaic_stream_new(&t.m, on_ev, &l);
        int rc = feed_sse(oaic_stream_on_sse, st, cases[i].sse, 4096);
        json_t *msg = oaic_stream_finish(st, rc, NULL);
        CHECK_STR(json_get_str(msg, "stopReason"), cases[i].stop);
        if (cases[i].err_sub) {
            const char *em = json_get_str(msg, "errorMessage");
            CHECK(em && strstr(em, cases[i].err_sub));
            CHECK(l.counts[AI_EV_ERROR] == 1);
        }
        if (i == 5) CHECK(json_get_int(json_get(block_of(msg, "toolCall", 0), "arguments"), "a", 0) == 1);
        json_free(msg);
        evlog_free(&l);
    }
    /* transport failure / abort */
    oaic_stream_t *st = oaic_stream_new(&t.m, NULL, NULL);
    json_t *msg = oaic_stream_finish(st, -1, "429 rate limited");
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    CHECK_STR(json_get_str(msg, "errorMessage"), "429 rate limited");
    json_free(msg);
    st = oaic_stream_new(&t.m, NULL, NULL);
    msg = oaic_stream_finish(st, -2, "Request was aborted");
    CHECK_STR(json_get_str(msg, "stopReason"), "aborted");
    json_free(msg);
    json_free(t.raw);
}

/* ====================================================================== */

static const char *RESP_HISTORY =
    "[{\"role\":\"user\",\"content\":\"hi\"},"
    "{\"role\":\"assistant\",\"api\":\"openai-responses\",\"provider\":\"openai\",\"model\":\"gpt-5\",\"stopReason\":\"toolUse\","
    " \"content\":[{\"type\":\"thinking\",\"thinking\":\"sum\",\"thinkingSignature\":\"{\\\"type\\\":\\\"reasoning\\\",\\\"id\\\":\\\"rs_1\\\",\\\"summary\\\":[],\\\"encrypted_content\\\":\\\"ENC\\\"}\"},"
    "  {\"type\":\"text\",\"text\":\"ok\",\"textSignature\":\"{\\\"v\\\":1,\\\"id\\\":\\\"msg_1\\\",\\\"phase\\\":\\\"commentary\\\"}\"},"
    "  {\"type\":\"toolCall\",\"id\":\"call_9|fc_9\",\"name\":\"read\",\"arguments\":{\"path\":\"x\"}}]},"
    "{\"role\":\"toolResult\",\"toolCallId\":\"call_9|fc_9\",\"toolName\":\"read\",\"content\":[{\"type\":\"text\",\"text\":\"data\"}]},"
    "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"img\"},{\"type\":\"image\",\"data\":\"QQ==\",\"mimeType\":\"image/jpeg\"}]}]";

/* #17847: a call cut by the output limit still replays next to its synthetic result; omc rebuilds every call from the
 * assistant blocks, so no native item can omit it (omp's pickUnrecordedResponsesToolCalls) */
static void test_cut_tool_calls_replay(void) {
    static const char *const apis[] = {"openai-responses", "openai-codex-responses"};
    for (int v = 0; v < 2; v++) {
        tmodel_t t;
        char *mj = xasprintf("{\"id\":\"m1\",\"api\":\"%s\",\"provider\":\"p1\",\"compat\":{\"strictResponsesPairing\":true}}", apis[v]);
        model_init(&t, mj);
        free(mj);
        char *hist = xasprintf(
            "[{\"role\":\"user\",\"content\":\"Write two files\"},"
            "{\"role\":\"assistant\",\"api\":\"%s\",\"provider\":\"p1\",\"model\":\"m1\",\"stopReason\":\"length\",\"content\":["
            "{\"type\":\"text\",\"text\":\"Writing files\"},"
            "{\"type\":\"toolCall\",\"id\":\"call_A|fc_A\",\"name\":\"write\",\"arguments\":{\"path\":\"a\"}},"
            "{\"type\":\"toolCall\",\"id\":\"call_B|fc_B\",\"name\":\"write\",\"arguments\":{\"path\":\"b\"}}]},"
            "{\"role\":\"toolResult\",\"toolCallId\":\"call_A|fc_A\",\"toolName\":\"write\",\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"Stopped at output limit\"}]},"
            "{\"role\":\"toolResult\",\"toolCallId\":\"call_B|fc_B\",\"toolName\":\"write\",\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"Stopped at output limit\"}]}]",
            apis[v]);
        json_t *msgs = J(hist);
        free(hist);
        ai_context_t ctx = {.messages = msgs};
        json_t *in = oair_convert_input(&t.m, &ctx, v == 0 ? 0 : 2);
        sb_t order = SB_INIT;
        json_t *it;
        json_arr_foreach(in, i, it) {
            const char *ty = json_get_str(it, "type");
            if (str_eq(ty, "function_call") || str_eq(ty, "function_call_output")) sb_appendf(&order, "%s:%s ", ty, json_get_str(it, "call_id"));
        }
        CHECK_STR(sb_cstr(&order), "function_call:call_A function_call:call_B function_call_output:call_A function_call_output:call_B ");
        CHECK_STR(json_get_str(json_at(in, json_len(in) - 1), "type"), "function_call_output");
        sb_free(&order);
        json_free(in);
        json_free(msgs);
        json_free(t.raw);
    }
}

/* compat.toolNamePrefix (Muse Code): declared tools, rebuilt calls and a forced choice use the wire name, a name the
 * prefix would push past the limit stays bare, and returned calls map back to the declared tool */
static void test_tool_name_prefix(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"muse-spark\",\"api\":\"openai-responses\",\"provider\":\"muse-code\",\"toolCallNameMaxLength\":64,"
                   "\"compat\":{\"toolNamePrefix\":\"_\",\"supportsToolChoice\":true,\"supportsNamedToolChoice\":true}}");
    char long_name[65];
    memset(long_name, 'a', 64);
    long_name[64] = 0;
    char *tools_json = xasprintf("[{\"name\":\"bash\",\"description\":\"d\",\"parameters\":{\"type\":\"object\"}},"
                                 "{\"name\":\"%s\",\"description\":\"d\",\"parameters\":{\"type\":\"object\"}}]", long_name);
    json_t *tools = J(tools_json);
    free(tools_json);
    json_t *msgs = J("[{\"role\":\"user\",\"content\":\"hi\"},"
                     "{\"role\":\"assistant\",\"api\":\"openai-responses\",\"provider\":\"muse-code\",\"model\":\"muse-spark\","
                     "\"stopReason\":\"toolUse\",\"content\":[{\"type\":\"toolCall\",\"id\":\"call_1|fc_1\",\"name\":\"bash\","
                     "\"arguments\":{}}]},"
                     "{\"role\":\"toolResult\",\"toolCallId\":\"call_1|fc_1\",\"toolName\":\"bash\",\"content\":[{\"type\":\"text\",\"text\":\"x\"}]}]");
    ai_context_t ctx = {.messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    json_t *tc = J("{\"type\":\"tool\",\"name\":\"bash\"}");
    o.tool_choice = tc;
    json_t *p = oair_build_request(&t.m, &ctx, &o);
    CHECK_STR(json_get_str(json_at(json_get(p, "tools"), 0), "name"), "_bash");
    CHECK_STR(json_get_str(json_at(json_get(p, "tools"), 1), "name"), long_name);
    CHECK_STR(json_get_str(json_get_obj(p, "tool_choice"), "name"), "_bash");
    json_t *it;
    bool saw_call = false;
    json_arr_foreach(json_get(p, "input"), i, it) {
        if (!str_eq(json_get_str(it, "type"), "function_call")) continue;
        saw_call = true;
        CHECK_STR(json_get_str(it, "name"), "_bash");
    }
    CHECK(saw_call);
    json_free(p);

    /* a forced choice of a name that is not declared (only its prefixed form exists) selects nothing */
    json_t *tc2 = J("{\"type\":\"tool\",\"name\":\"_bash\"}");
    o.tool_choice = tc2;
    p = oair_build_request(&t.m, &ctx, &o);
    CHECK(json_get(p, "tool_choice") == NULL);
    json_free(p);

    char *err = NULL;
    json_t *names = oair_tool_names_by_wire(&t.m, tools, &err);
    CHECK(names && !err);
    evlog_t l = {0};
    oair_stream_t *st = oair_stream_new(&t.m, on_ev, &l);
    oair_stream_set_wire_names(st, names);
    int rc = feed_sse(oair_stream_on_sse, st,
                      "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"id\":\"fc_2\",\"call_id\":\"call_2\",\"name\":\"_bash\",\"arguments\":\"\"}}\n\n"
                      "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"id\":\"fc_2\",\"call_id\":\"call_2\",\"name\":\"_bash\",\"arguments\":\"{}\"}}\n\n"
                      "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n", 4096);
    json_t *msg = oair_stream_finish(st, rc, NULL);
    CHECK_STR(json_get_str(block_of(msg, "toolCall", 0), "name"), "bash");
    CHECK_STR(json_get_str(msg, "stopReason"), "toolUse");
    json_free(msg);
    json_free(names);
    evlog_free(&l);

    /* the prefixed form meets a bare 64-character name */
    err = NULL;
    char u63[65] = "_";
    memset(u63 + 1, 'c', 63);
    u63[64] = 0;
    char c63[64];
    memset(c63, 'c', 63);
    c63[63] = 0;
    char *cs = xasprintf("[{\"name\":\"%s\"},{\"name\":\"%s\"}]", c63, u63);
    json_t *clash = J(cs);
    free(cs);
    names = oair_tool_names_by_wire(&t.m, clash, &err);
    CHECK(!names && err && strstr(err, "both encode as"));
    free(err);
    json_free(clash);

    json_free(tc);
    json_free(tc2);
    json_free(tools);
    json_free(msgs);
    json_free(t.raw);
}

static void test_responses_request(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"gpt-5\",\"api\":\"openai-responses\",\"provider\":\"openai\",\"reasoning\":true,\"image\":true,"
                   "\"maxTokens\":128000,\"compat\":{\"supportsDeveloperRole\":true,\"supportsStrictMode\":true,"
                   "\"supportsReasoningSummary\":true,\"includeEncryptedReasoning\":true,\"officialEndpoint\":true,"
                   "\"supportsSamplingParams\":false}}");
    json_t *msgs = J(RESP_HISTORY), *sys = J("[\"sys prompt\"]"), *tools = J(TOOLS);
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_HIGH;
    o.session_id = "sess-123";
    o.text_verbosity = "low";
    o.service_tier = "priority";
    o.temperature = 0.2;
    json_t *tc = J("\"required\"");
    o.tool_choice = tc;
    json_t *p = oair_build_request(&t.m, &ctx, &o);
    CHECK_STR(json_get_str(p, "model"), "gpt-5");
    CHECK(json_get(p, "store") && !json_get_bool(p, "store", true));
    CHECK_STR(json_get_str(p, "prompt_cache_key"), "sess-123");
    CHECK_STR(json_get_str(json_get_obj(p, "reasoning"), "effort"), "high");
    CHECK_STR(json_get_str(json_get_obj(p, "reasoning"), "summary"), "auto");
    CHECK_STR(json_sv(json_at(json_get(p, "include"), 0)), "reasoning.encrypted_content");
    CHECK_STR(json_get_str(json_get_obj(p, "text"), "verbosity"), "low");
    CHECK_STR(json_get_str(p, "service_tier"), "priority");
    CHECK(json_get(p, "temperature") == NULL);
    CHECK(json_get_int(p, "max_output_tokens", 0) == 64000);
    CHECK_STR(json_get_str(p, "tool_choice"), "required");
    CHECK(json_get(p, "instructions") == NULL);
    json_t *in = json_get(p, "input");
    CHECK_STR(json_get_str(json_at(in, 0), "role"), "developer");
    CHECK_STR(json_get_str(json_at(in, 0), "content"), "sys prompt");
    CHECK_STR(json_get_str(json_at(in, 1), "role"), "user");
    CHECK_STR(json_get_str(json_at(json_get(json_at(in, 1), "content"), 0), "type"), "input_text");
    json_t *rs = json_at(in, 2);
    CHECK_STR(json_get_str(rs, "type"), "reasoning");
    CHECK_STR(json_get_str(rs, "encrypted_content"), "ENC");
    json_t *mi = json_at(in, 3);
    CHECK_STR(json_get_str(mi, "type"), "message");
    CHECK_STR(json_get_str(mi, "id"), "msg_1");
    CHECK_STR(json_get_str(mi, "phase"), "commentary");
    CHECK_STR(json_get_str(json_at(json_get(mi, "content"), 0), "type"), "output_text");
    json_t *fc = json_at(in, 4);
    CHECK_STR(json_get_str(fc, "type"), "function_call");
    CHECK_STR(json_get_str(fc, "call_id"), "call_9");
    CHECK_STR(json_get_str(fc, "id"), "fc_9");
    CHECK_STR(json_get_str(fc, "arguments"), "{\"path\":\"x\"}");
    json_t *fo = json_at(in, 5);
    CHECK_STR(json_get_str(fo, "type"), "function_call_output");
    CHECK_STR(json_get_str(fo, "call_id"), "call_9");
    CHECK_STR(json_get_str(fo, "output"), "data");
    json_t *ui = json_at(in, 6);
    CHECK_STR(json_get_str(json_at(json_get(ui, "content"), 1), "type"), "input_image");
    CHECK_STR(json_get_str(json_at(json_get(ui, "content"), 1), "image_url"), "data:image/jpeg;base64,QQ==");
    json_t *tool = json_at(json_get(p, "tools"), 0);
    CHECK_STR(json_get_str(tool, "type"), "function");
    CHECK_STR(json_get_str(tool, "name"), "read");
    CHECK(json_get_bool(tool, "strict", false));
    char *url = oair_url(&t.m);
    CHECK_STR(url, "https://api.openai.com/v1/responses");
    free(url);

    /* hidden summary + non-developer endpoint -> instructions, no summary */
    json_set_bool(t.m.compat, "supportsDeveloperRole", false);
    o.hide_thinking_summary = true;
    json_t *p2 = oair_build_request(&t.m, &ctx, &o);
    CHECK_STR(json_get_str(p2, "instructions"), "sys prompt");
    CHECK(json_get(json_get_obj(p2, "reasoning"), "summary") == NULL);
    json_free(p2);

    json_free(p);
    json_free(tc);
    json_free(msgs);
    json_free(sys);
    json_free(tools);
    json_free(t.raw);
}

static void test_codex_azure(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"gpt-5.6-luna\",\"api\":\"openai-codex-responses\",\"provider\":\"openai-codex\",\"reasoning\":true,"
                   "\"baseUrl\":\"https://chatgpt.com/backend-api/\",\"maxTokens\":128000,"
                   "\"thinking\":{\"efforts\":[\"low\",\"medium\",\"high\"]},"
                   "\"compat\":{\"supportsReasoningSummary\":true,\"supportsStrictMode\":false}}");
    json_t *msgs = J(RESP_HISTORY), *sys = J("[\"base instructions\",\"extra dev\"]"), *tools = J(TOOLS);
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_MEDIUM;
    o.max_tokens = 1000;
    o.temperature = 0.3;
    /* history comes from api openai-responses; re-label so ids would be replayable */
    json_t *p = oair_build_request(&t.m, &ctx, &o);
    CHECK_STR(json_get_str(p, "instructions"), "base instructions");
    CHECK(json_get(p, "store") && !json_get_bool(p, "store", true));
    CHECK(json_get(p, "max_output_tokens") == NULL);
    CHECK(json_get(p, "temperature") == NULL);
    CHECK_STR(json_get_str(json_get_obj(p, "reasoning"), "effort"), "medium");
    CHECK_STR(json_get_str(json_get_obj(p, "reasoning"), "summary"), "auto");
    CHECK_STR(json_sv(json_at(json_get(p, "include"), 0)), "reasoning.encrypted_content");
    json_t *in = json_get(p, "input"), *it;
    CHECK_STR(json_get_str(json_at(in, 0), "role"), "developer");
    CHECK_STR(json_get_str(json_at(json_get(json_at(in, 0), "content"), 0), "text"), "extra dev");
    json_arr_foreach(in, i, it) CHECK(json_get(it, "id") == NULL);
    CHECK(json_get(json_at(json_get(p, "tools"), 0), "strict") == NULL);
    char *url = oair_url(&t.m);
    CHECK_STR(url, "https://chatgpt.com/backend-api/codex/responses");
    free(url);
    /* JWT: header.payload.sig with payload {"https://api.openai.com/auth":{"chatgpt_account_id":"acct_42"}} */
    const char *payload = "{\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"acct_42\"}}";
    char *b64 = base64url_encode(payload, strlen(payload));
    for (char *c = b64; *c; c++)
        if (*c == '=') *c = 0;
    char *jwt = xasprintf("eyJhbGciOiJub25lIn0.%s.sig", b64);
    char *acc = oair_codex_account_id(jwt);
    CHECK_STR(acc, "acct_42");
    free(acc);
    CHECK(oair_codex_account_id("not-a-jwt") == NULL);
    free(jwt);
    free(b64);
    /* disable reasoning -> lowest effort */
    o.reasoning = EFFORT_OFF;
    o.disable_reasoning = true;
    json_t *p2 = oair_build_request(&t.m, &ctx, &o);
    CHECK_STR(json_get_str(json_get_obj(p2, "reasoning"), "effort"), "low");
    CHECK(json_get(json_get_obj(p2, "reasoning"), "summary") == NULL);
    json_free(p2);
    json_free(p);

    /* backend-only metadata on captured output items never reaches the Codex input */
    json_t *meta_msgs = J(
        "[{\"role\":\"user\",\"content\":\"s\",\"providerPayload\":{\"type\":\"openaiResponsesHistory\",\"items\":["
        "{\"type\":\"message\",\"role\":\"assistant\",\"id\":\"msg_1\",\"status\":\"completed\","
        "\"content\":[],\"internal_chat_message_metadata_passthrough\":{\"turn_id\":\"t\"}}]}},"
        "{\"role\":\"assistant\",\"api\":\"openai-codex-responses\",\"provider\":\"openai-codex\",\"model\":\"gpt-5.6-luna\","
        "\"stopReason\":\"stop\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"x\",\"thinkingSignature\":"
        "\"{\\\"type\\\":\\\"reasoning\\\",\\\"id\\\":\\\"rs_1\\\",\\\"summary\\\":[],"
        "\\\"internal_chat_message_metadata_passthrough\\\":{\\\"turn_id\\\":\\\"t\\\"}}\"},"
        "{\"type\":\"text\",\"text\":\"done\"}]},"
        "{\"role\":\"user\",\"content\":\"next\"}]");
    ai_context_t meta_ctx = {.system_prompt = sys, .messages = meta_msgs};
    json_t *pm = oair_build_request(&t.m, &meta_ctx, &o);
    char *pms = json_dump(json_get(pm, "input"), 0);
    CHECK(strstr(pms, "internal_chat_message_metadata_passthrough") == NULL);
    CHECK(strstr(pms, "\"reasoning\"") != NULL);
    free(pms);
    json_free(pm);
    json_free(meta_msgs);
    json_free(t.raw);

    tmodel_t az;
    model_init(&az, "{\"id\":\"gpt-4.1\",\"api\":\"azure-openai-responses\",\"provider\":\"azure\",\"reasoning\":false,"
                    "\"baseUrl\":\"https://res.openai.azure.com/openai/v1/\",\"maxTokens\":32768,\"compat\":{}}");
    ai_stream_opts_init(&o);
    p = oair_build_request(&az.m, &ctx, &o);
    CHECK_STR(json_get_str(p, "model"), "gpt-4.1");
    in = json_get(p, "input");
    CHECK_STR(json_get_str(json_at(in, 0), "role"), "system");
    CHECK_STR(json_get_str(json_at(in, 0), "content"), "base instructions");
    CHECK(json_get(json_at(json_get(p, "tools"), 0), "strict") && !json_get_bool(json_at(json_get(p, "tools"), 0), "strict", true));
    CHECK(json_get(p, "reasoning") == NULL);
    url = oair_url(&az.m);
    if (!env_get("AZURE_OPENAI_BASE_URL") && !env_get("AZURE_OPENAI_RESOURCE_NAME") && !env_get("AZURE_OPENAI_API_VERSION"))
        CHECK_STR(url, "https://res.openai.azure.com/openai/v1/responses?api-version=v1");
    free(url);
    json_free(p);
    json_free(az.raw);
    json_free(msgs);
    json_free(sys);
    json_free(tools);
}

static void test_responses_stream(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"gpt-5\",\"api\":\"openai-responses\",\"provider\":\"openai\",\"reasoning\":true,\"compat\":{}}");
    const char *sse =
        "event: response.created\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\",\"status\":\"in_progress\"}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[]}}\n\n"
        "data: {\"type\":\"response.reasoning_summary_part.added\",\"item_id\":\"rs_1\",\"output_index\":0,\"summary_index\":0,\"part\":{\"type\":\"summary_text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"rs_1\",\"output_index\":0,\"summary_index\":0,\"delta\":\"Plan\"}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"rs_1\",\"output_index\":0,\"summary_index\":0,\"delta\":\"ning\"}\n\n"
        "data: {\"type\":\"response.reasoning_summary_part.done\",\"item_id\":\"rs_1\",\"output_index\":0,\"summary_index\":0}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[{\"type\":\"summary_text\",\"text\":\"Planning\"}],\"encrypted_content\":\"gAAA\"}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"type\":\"message\",\"id\":\"msg_1\",\"role\":\"assistant\",\"content\":[]}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\",\"output_index\":1,\"content_index\":0,\"delta\":\"Hi \"}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\",\"output_index\":1,\"content_index\":0,\"delta\":\"there\"}\n\n"
        "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_1\",\"output_index\":1,\"content_index\":0,\"text\":\"Hi there!\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"type\":\"message\",\"id\":\"msg_1\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"Hi there!\"}]}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":2,\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":\"call_1\",\"name\":\"read\",\"arguments\":\"\"}}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"output_index\":2,\"delta\":\"{\\\"path\\\":\"}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"output_index\":2,\"delta\":\"\\\"a.c\\\"}\"}\n\n"
        "data: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"fc_1\",\"output_index\":2,\"arguments\":\"{\\\"path\\\":\\\"a.c\\\"}\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":2,\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":\"call_1\",\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"a.c\\\"}\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\",\"status\":\"completed\",\"usage\":{\"input_tokens\":500,"
        "\"input_tokens_details\":{\"cached_tokens\":200},\"output_tokens\":50,\"output_tokens_details\":{\"reasoning_tokens\":30},\"total_tokens\":550}}}\n\n";
    size_t chunks[] = {3, 4096};
    for (int ci = 0; ci < 2; ci++) {
        evlog_t l = {0};
        oair_stream_t *st = oair_stream_new(&t.m, on_ev, &l);
        int rc = feed_sse(oair_stream_on_sse, st, sse, chunks[ci]);
        CHECK(rc == OAI_STREAM_STOP);
        json_t *msg = oair_stream_finish(st, rc, NULL);
        CHECK_STR(json_get_str(msg, "stopReason"), "toolUse");
        CHECK_STR(json_get_str(msg, "responseId"), "resp_1");
        CHECK(json_len(json_get(msg, "content")) == 3);
        json_t *th = block_of(msg, "thinking", 0);
        CHECK_STR(json_get_str(th, "thinking"), "Planning");
        json_t *sig = json_parse_str(json_get_str(th, "thinkingSignature"));
        CHECK_STR(json_get_str(sig, "encrypted_content"), "gAAA");
        CHECK_STR(json_get_str(sig, "id"), "rs_1");
        json_free(sig);
        json_t *tx = block_of(msg, "text", 0);
        CHECK_STR(json_get_str(tx, "text"), "Hi there!");
        CHECK_STR(json_get_str(tx, "textSignature"), "{\"v\":1,\"id\":\"msg_1\",\"phase\":\"final_answer\"}");
        json_t *tc = block_of(msg, "toolCall", 0);
        CHECK_STR(json_get_str(tc, "id"), "call_1|fc_1");
        CHECK_STR(json_get_str(json_get(tc, "arguments"), "path"), "a.c");
        json_t *u = json_get(msg, "usage");
        CHECK(json_get_int(u, "input", -1) == 300);
        CHECK(json_get_int(u, "cacheRead", -1) == 200);
        CHECK(json_get_int(u, "output", -1) == 50);
        CHECK(json_get_int(u, "reasoningTokens", -1) == 30);
        CHECK(l.counts[AI_EV_THINKING_END] == 1 && l.counts[AI_EV_TEXT_END] == 1 && l.counts[AI_EV_TOOLCALL_END] == 1);
        CHECK(l.counts[AI_EV_DONE] == 1);
        CHECK_STR(sb_cstr(&l.text), "Hi there!");
        CHECK_STR(sb_cstr(&l.thinking), "Planning\n\n");

        /* the produced message replays as Responses input */
        json_t *hist = json_arr();
        json_t *um = J("{\"role\":\"user\",\"content\":\"q\"}");
        json_push(hist, um);
        json_push(hist, msg);
        ai_context_t ctx = {.messages = hist};
        json_t *in = oair_convert_input(&t.m, &ctx, 0);
        CHECK_STR(json_get_str(json_at(in, 1), "type"), "reasoning");
        CHECK_STR(json_get_str(json_at(in, 2), "id"), "msg_1");
        CHECK_STR(json_get_str(json_at(in, 3), "call_id"), "call_1");
        CHECK_STR(json_get_str(json_at(in, 3), "id"), "fc_1");
        json_free(in);
        json_free(hist);
        evlog_free(&l);
    }

    /* encrypted-only reasoning (no deltas), incomplete status, failed + error events */
    const char *sse2 =
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"id\":\"rs_2\"}}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"reasoning\",\"id\":\"rs_2\",\"summary\":[],\"encrypted_content\":\"E\"}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"type\":\"message\",\"id\":\"msg_2\"}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_2\",\"delta\":\"cut\"}\n\n"
        "data: {\"type\":\"response.incomplete\",\"response\":{\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":\"max_output_tokens\"},\"usage\":{\"input_tokens\":5,\"output_tokens\":9}}}\n\n";
    oair_stream_t *st = oair_stream_new(&t.m, NULL, NULL);
    int rc = feed_sse(oair_stream_on_sse, st, sse2, 4096);
    json_t *msg = oair_stream_finish(st, rc, NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "length");
    CHECK(json_len(json_get(msg, "content")) == 2);
    CHECK_STR(json_get_str(block_of(msg, "thinking", 0), "thinking"), "");
    CHECK(json_get_str(block_of(msg, "thinking", 0), "thinkingSignature") != NULL);
    CHECK_STR(json_get_str(block_of(msg, "text", 0), "text"), "cut");
    json_free(msg);

    const char *sse3 = "data: {\"type\":\"response.failed\",\"response\":{\"status\":\"failed\",\"error\":{\"code\":\"server_error\",\"message\":\"boom\"}}}\n\n";
    st = oair_stream_new(&t.m, NULL, NULL);
    rc = feed_sse(oair_stream_on_sse, st, sse3, 4096);
    msg = oair_stream_finish(st, rc, NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    CHECK_STR(json_get_str(msg, "errorMessage"), "server_error: boom");
    json_free(msg);

    const char *sse4 = "event: error\ndata: {\"type\":\"error\",\"code\":\"rate_limit_exceeded\",\"message\":\"slow down\"}\n\n";
    st = oair_stream_new(&t.m, NULL, NULL);
    rc = feed_sse(oair_stream_on_sse, st, sse4, 4096);
    msg = oair_stream_finish(st, rc, NULL);
    CHECK_STR(json_get_str(msg, "errorMessage"), "Error Code rate_limit_exceeded: slow down");
    json_free(msg);

    const char *sse5 = "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"message\",\"id\":\"m\"}}\n\n";
    st = oair_stream_new(&t.m, NULL, NULL);
    rc = feed_sse(oair_stream_on_sse, st, sse5, 4096);
    msg = oair_stream_finish(st, rc, NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    json_free(msg);
    json_free(t.raw);
}

/* ====================================================================== */

static void test_ollama(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"qwen3:8b\",\"api\":\"ollama-chat\",\"provider\":\"ollama\",\"reasoning\":true,\"image\":true,"
                   "\"baseUrl\":\"http://localhost:11434/api/\",\"maxTokens\":4096,\"compat\":{}}");
    json_t *msgs = J("[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"what\"},{\"type\":\"image\",\"data\":\"QUJD\",\"mimeType\":\"image/png\"}]},"
                     "{\"role\":\"assistant\",\"api\":\"ollama-chat\",\"provider\":\"ollama\",\"model\":\"qwen3:8b\",\"stopReason\":\"toolUse\","
                     " \"content\":[{\"type\":\"thinking\",\"thinking\":\"t\"},{\"type\":\"toolCall\",\"id\":\"ollama:1:ls\",\"name\":\"ls\",\"arguments\":{\"d\":\".\"}}]},"
                     "{\"role\":\"toolResult\",\"toolCallId\":\"ollama:1:ls\",\"toolName\":\"ls\",\"content\":[{\"type\":\"text\",\"text\":\"a b\"}]}]");
    json_t *sys = J("[\"sys\"]"), *tools = J(TOOLS);
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_XHIGH;
    json_t *p = ollama_build_request(&t.m, &ctx, &o);
    CHECK_STR(json_get_str(p, "think"), "high");
    CHECK(json_get_int(json_get(p, "options"), "num_predict", 0) == 4096);
    json_t *m = json_get(p, "messages");
    CHECK(json_len(m) == 4);
    CHECK_STR(json_get_str(json_at(m, 0), "role"), "system");
    CHECK_STR(json_sv(json_at(json_get(json_at(m, 1), "images"), 0)), "QUJD");
    CHECK(json_get(json_at(m, 2), "thinking") == NULL);
    CHECK_STR(json_get_str(json_get(json_get_obj(json_at(json_get(json_at(m, 2), "tool_calls"), 0), "function"), "arguments"), "d"), ".");
    CHECK_STR(json_get_str(json_at(m, 3), "role"), "tool");
    CHECK_STR(json_get_str(json_at(m, 3), "tool_name"), "ls");
    CHECK(json_len(json_get(p, "tools")) == 1);
    json_free(p);
    o.reasoning = EFFORT_OFF;
    o.disable_reasoning = true;
    p = ollama_build_request(&t.m, &ctx, &o);
    CHECK(json_get(p, "think") && !json_get_bool(p, "think", true));
    json_free(p);

    const char *nd =
        "{\"model\":\"qwen3\",\"message\":{\"role\":\"assistant\",\"content\":\"\",\"thinking\":\"Hm\"},\"done\":false}\n"
        "{\"model\":\"qwen3\",\"message\":{\"role\":\"assistant\",\"content\":\"\",\"thinking\":\"m.\"},\"done\":false}\n"
        "{\"model\":\"qwen3\",\"message\":{\"role\":\"assistant\",\"content\":\"Ok \"},\"done\":false}\r\n"
        "{\"model\":\"qwen3\",\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"function\":{\"name\":\"read\",\"arguments\":{\"path\":\"z\"}}}]},\"done\":false}\n"
        "{\"model\":\"qwen3\",\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true,\"done_reason\":\"stop\",\"prompt_eval_count\":30,\"prompt_eval_cached_count\":10,\"eval_count\":12}";
    size_t chunks[] = {1, 13, 100000};
    for (int ci = 0; ci < 3; ci++) {
        evlog_t l = {0};
        ollama_stream_t *st = ollama_stream_new(&t.m, on_ev, &l);
        int rc = 0;
        size_t n = strlen(nd);
        for (size_t off = 0; off < n && !rc; off += chunks[ci]) rc = ollama_stream_on_data(st, nd + off, MIN(chunks[ci], n - off));
        json_t *msg = ollama_stream_finish(st, rc, NULL);
        CHECK_STR(json_get_str(msg, "stopReason"), "toolUse");
        CHECK_STR(json_get_str(block_of(msg, "thinking", 0), "thinking"), "Hmm.");
        CHECK_STR(json_get_str(block_of(msg, "text", 0), "text"), "Ok ");
        json_t *tc = block_of(msg, "toolCall", 0);
        CHECK_STR(json_get_str(tc, "name"), "read");
        CHECK_STR(json_get_str(json_get(tc, "arguments"), "path"), "z");
        json_t *u = json_get(msg, "usage");
        CHECK(json_get_int(u, "input", -1) == 20 && json_get_int(u, "cacheRead", -1) == 10 && json_get_int(u, "output", -1) == 12);
        CHECK(l.counts[AI_EV_DONE] == 1);
        json_free(msg);
        evlog_free(&l);
    }
    ollama_stream_t *st = ollama_stream_new(&t.m, NULL, NULL);
    const char *bad = "{\"error\":\"model not found\"}\n";
    int rc = ollama_stream_on_data(st, bad, strlen(bad));
    json_t *msg = ollama_stream_finish(st, rc, NULL);
    CHECK_STR(json_get_str(msg, "errorMessage"), "Ollama stream error: model not found");
    json_free(msg);
    st = ollama_stream_new(&t.m, NULL, NULL);
    const char *empty_len = "{\"message\":{\"content\":\"\"},\"done\":true,\"done_reason\":\"length\"}\n";
    rc = ollama_stream_on_data(st, empty_len, strlen(empty_len));
    msg = ollama_stream_finish(st, rc, NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    json_free(msg);

    json_free(msgs);
    json_free(sys);
    json_free(tools);
    json_free(t.raw);
}

static void test_strict_schema(void) {
    json_t *s = J("{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\",\"default\":\"x\"},"
                  "\"b\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"number\"}]},"
                  "\"c\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"k\":{\"type\":\"string\"}},\"required\":[\"k\"]}}},"
                  "\"required\":[\"a\"]}");
    json_t *r = oai_strict_schema(s);
    CHECK(r != NULL);
    json_t *props = json_get(r, "properties");
    CHECK(json_get(json_get(props, "a"), "default") == NULL);
    CHECK(json_len(json_get(json_get(props, "b"), "anyOf")) == 3);
    CHECK(json_len(json_get(json_get(props, "c"), "anyOf")) == 2);
    json_t *inner = json_get(json_at(json_get(json_get(props, "c"), "anyOf"), 0), "items");
    CHECK(json_get(inner, "additionalProperties") && !json_get_bool(inner, "additionalProperties", true));
    json_free(r);
    json_free(s);
    const char *bad[] = {"{\"type\":\"object\"}", "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":true}",
                         "{\"type\":\"object\",\"properties\":{\"x\":{\"oneOf\":[{\"type\":\"string\"}]}}}",
                         "{\"type\":\"object\",\"properties\":{\"x\":{\"$ref\":\"#/$defs/y\",\"description\":\"d\"}}}"};
    for (size_t i = 0; i < ARRAY_LEN(bad); i++) {
        s = J(bad[i]);
        r = oai_strict_schema(s);
        CHECK(r == NULL);
        json_free(r);
        json_free(s);
    }
    /* deep nesting is capped */
    sb_t deep = SB_INIT;
    for (int i = 0; i < 40; i++) sb_append(&deep, "{\"type\":\"array\",\"items\":");
    sb_append(&deep, "{\"type\":\"string\"}");
    for (int i = 0; i < 40; i++) sb_putc(&deep, '}');
    s = J(deep.s);
    CHECK(oai_strict_schema(s) == NULL);
    json_free(s);
    sb_free(&deep);
}

/* ---------- transport retry helpers (fetch-retry.ts, retry-after.ts, retryable.ts) ---------- */

static void test_retry_hints(void) {
    http_headers_t h = {0};
    CHECK(ai_extract_retry_hint(NULL, NULL) == -1);
    http_headers_set(&h, "retry-after-ms", "0");
    CHECK(ai_extract_retry_hint(&h, NULL) == 0); /* retry now */
    CHECK(ai_retry_after_from_headers(&h) == -1); /* no " retry-after-ms=" suffix for a non-positive hint */
    http_headers_set(&h, "retry-after-ms", "1500");
    CHECK(ai_extract_retry_hint(&h, NULL) == 1500);
    CHECK(ai_retry_after_from_headers(&h) == 1500);
    http_headers_free(&h);
    http_headers_set(&h, "retry-after", "2");
    http_headers_set(&h, "x-ratelimit-reset-ms", "7000");
    CHECK(ai_extract_retry_hint(&h, NULL) == 2000);       /* first header source wins */
    CHECK(ai_retry_after_from_headers(&h) == 7000);       /* longest header timing */
    http_headers_free(&h);
    CHECK(ai_extract_retry_hint(NULL, "Please retry in 250ms") == 250);
    CHECK(ai_extract_retry_hint(NULL, "{\"retryDelay\": \"34.5s\"}") == 34500);
    CHECK(ai_extract_retry_hint(NULL, "Rate limited, try again in ~5 min.") == 300000);
    CHECK(ai_extract_retry_hint(NULL, "Your quota will reset after 1h2m3s") == 3723000);
    CHECK(ai_extract_retry_hint(NULL, "Resets in 2hr 15min") == 8100000);
    CHECK(ai_extract_retry_hint(NULL, "limit will reset in 3 days") == 3 * 86400000LL);
    CHECK(ai_extract_retry_hint(NULL, "429 slow down retry-after-ms=98497") == 98497);
    CHECK(ai_extract_retry_hint(NULL, "try again in 2s, or reset after 10s") == 10000); /* longest wins */
    CHECK(ai_extract_retry_hint(NULL, "retry-after: 0") == 0);
    CHECK(ai_extract_retry_hint(NULL, "try again later") == -1);
    /* isProviderRetryableError */
    CHECK(ai_provider_retryable_error("503 try again later", 0));
    CHECK(ai_provider_retryable_error("429 Rate limit exceeded for mock-flaky", 0));
    CHECK(ai_provider_retryable_error("connection closed before response", 0));
    CHECK(!ai_provider_retryable_error("400 bad things", 0));
    CHECK(!ai_provider_retryable_error("401 invalid api key", 0));
    CHECK(!ai_provider_retryable_error("429 You exceeded your current quota: insufficient_quota", 0));
    CHECK(!ai_provider_retryable_error("content_filter blocked", 0));
}

typedef struct {
    const ai_model_t *m;
    int calls;
    const char *const *script; /* per attempt: NULL = empty stop, "!<msg>" = error, else text */
} fake_attempt_t;

static json_t *fake_attempt(void *self, ai_event_cb cb, void *ud) {
    fake_attempt_t *f = self;
    const char *step = f->script[f->calls++];
    ai_builder_t b;
    aib_init(&b, f->m, cb, ud);
    aib_start(&b);
    json_t *msg;
    if (step && step[0] == '!') msg = aib_finish(&b, "error", step + 1);
    else {
        if (step) aib_text_delta(&b, step, strlen(step));
        msg = aib_finish(&b, "stop", NULL);
    }
    aib_free(&b);
    return msg;
}

static void test_replay_safe(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"m1\",\"api\":\"openai-completions\",\"provider\":\"test\",\"compat\":{}}");
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    /* a retryable provider error before any output is replayed once; the consumer sees one clean attempt */
    const char *s1[] = {"!503 try again later", "hello"};
    fake_attempt_t f = {&t.m, 0, s1};
    evlog_t l = {0};
    int64_t t0 = mono_ms();
    json_t *msg = ai_replay_safe_stream(fake_attempt, &f, &o, on_ev, &l, true, 1);
    CHECK(f.calls == 2 && mono_ms() - t0 >= 450);
    CHECK_STR(json_get_str(msg, "stopReason"), "stop");
    CHECK(l.counts[AI_EV_START] == 1 && l.counts[AI_EV_ERROR] == 0 && l.counts[AI_EV_DONE] == 1);
    CHECK_STR(sb_cstr(&l.text), "hello");
    json_free(msg);
    evlog_free(&l);
    /* the budget is one retry: a second failure surfaces */
    const char *s2[] = {"!503 a", "!503 b"};
    f = (fake_attempt_t){&t.m, 0, s2};
    memset(&l, 0, sizeof l);
    msg = ai_replay_safe_stream(fake_attempt, &f, &o, on_ev, &l, true, 1);
    CHECK(f.calls == 2 && l.counts[AI_EV_START] == 1 && l.counts[AI_EV_ERROR] == 1);
    CHECK_STR(json_get_str(msg, "errorMessage"), "503 b");
    json_free(msg);
    evlog_free(&l);
    /* terminal 4xx errors are never replayed */
    const char *s3[] = {"!400 bad request"};
    f = (fake_attempt_t){&t.m, 0, s3};
    memset(&l, 0, sizeof l);
    msg = ai_replay_safe_stream(fake_attempt, &f, &o, on_ev, &l, true, 1);
    CHECK(f.calls == 1 && l.counts[AI_EV_ERROR] == 1);
    json_free(msg);
    evlog_free(&l);
    /* a benign empty completion is retried; acceptEmptyResponse turns that off */
    const char *s4[] = {NULL, "late"};
    f = (fake_attempt_t){&t.m, 0, s4};
    memset(&l, 0, sizeof l);
    msg = ai_replay_safe_stream(fake_attempt, &f, &o, on_ev, &l, true, 0);
    CHECK(f.calls == 2 && l.counts[AI_EV_START] == 1);
    CHECK_STR(sb_cstr(&l.text), "late");
    json_free(msg);
    evlog_free(&l);
    o.accept_empty_response = true;
    f = (fake_attempt_t){&t.m, 0, s4};
    memset(&l, 0, sizeof l);
    msg = ai_replay_safe_stream(fake_attempt, &f, &o, on_ev, &l, true, 0);
    CHECK(f.calls == 1 && l.counts[AI_EV_DONE] == 1);
    json_free(msg);
    evlog_free(&l);
    /* no event callback (ai_complete without the stream watchdog): held events and the retry are still handled */
    o.accept_empty_response = false;
    f = (fake_attempt_t){&t.m, 0, s1};
    msg = ai_replay_safe_stream(fake_attempt, &f, &o, NULL, NULL, true, 1);
    CHECK(f.calls == 2);
    CHECK_STR(json_get_str(msg, "stopReason"), "stop");
    json_free(msg);
    json_free(t.raw);
}

int main(void) {
    test_retry_hints();
    test_replay_safe();
    test_completions_request();
    test_completions_mistral();
    test_completions_dialects();
    test_completions_stream();
    test_completions_stream_errors();
    test_responses_request();
    test_tool_name_prefix();
    test_cut_tool_calls_replay();
    test_codex_azure();
    test_responses_stream();
    test_ollama();
    test_strict_schema();
    printf("test_providers_oai: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
