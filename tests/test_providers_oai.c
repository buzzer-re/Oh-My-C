/* test_providers_oai.c - offline tests for the OpenAI-family providers (request building + stream decoding). */
#include <time.h>
#include "ai/leaked_thinking.h"
#include "ai/provider_openai.h"
#include "ai/toolschema.h"
#include "core/fs.h"
#include <unistd.h>

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

/* reasoning_details (omp 5759bdacc9, 4a064201a9, 67ccb2ca07, 25c2fb8211, e1cffa057e): Bifrost streams Gemini thinking
 * only as reasoning.text entries; a plain field arriving after them is a mirror; the entries replay verbatim */
static void test_reasoning_details(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"gemini-3.5-flash\",\"api\":\"openai-completions\",\"provider\":\"bifrost\",\"reasoning\":true,\"compat\":{}}");
    const char *sse =
        "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"index\":0,\"text\":\"Think \"}]}}]}\n\n"
        "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning\":\"mirror\",\"reasoning_details\":[{\"type\":\"reasoning.text\",\"index\":0,\"text\":\"hard.\",\"signature\":\"SIG\"}]}}]}\n\n"
        "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"read\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.encrypted\",\"id\":\"call_1\",\"signature\":\"BLOB\"}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    oaic_stream_t *st = oaic_stream_new(&t.m, NULL, NULL);
    int rc = feed_sse(oaic_stream_on_sse, st, sse, 4096);
    json_t *msg = oaic_stream_finish(st, rc, NULL);
    json_t *th = block_of(msg, "thinking", 0);
    CHECK_STR(json_get_str(th, "thinking"), "Think hard.");
    json_t *env = json_parse_str(json_get_str(th, "thinkingSignature"));
    char *d = json_dump(json_get(env, "reasoningDetails"), 0);
    CHECK_STR(d, "[{\"type\":\"reasoning.text\",\"index\":0,\"text\":\"Think hard.\",\"signature\":\"SIG\"},"
                 "{\"type\":\"reasoning.encrypted\",\"id\":\"call_1\",\"signature\":\"BLOB\"}]");
    free(d);
    CHECK_STR(json_get_str(env, "field"), "reasoning");
    json_free(env);
    CHECK(json_get_str(block_of(msg, "toolCall", 0), "thoughtSignature") != NULL); /* the encrypted entry stays on its call */
    /* replay: the entries go back verbatim on the same model's turn */
    json_set_str(msg, "provider", "bifrost");
    json_t *msgs = json_arr();
    json_push(msgs, J("{\"role\":\"user\",\"content\":\"hi\"}"));
    json_push(msgs, msg);
    json_push(msgs, J("{\"role\":\"toolResult\",\"toolCallId\":\"call_1\",\"toolName\":\"read\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"isError\":false}"));
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    json_t *p = oaic_build_request(&t.m, &ctx, &o);
    json_t *am = json_at(json_get(p, "messages"), 1);
    d = json_dump(json_get(am, "reasoning_details"), 0);
    /* the model's own unsigned thinking text would be dropped where its thinking is not sent; signed text stays */
    CHECK_STR(d, "[{\"type\":\"reasoning.text\",\"index\":0,\"text\":\"Think hard.\",\"signature\":\"SIG\"},"
                 "{\"type\":\"reasoning.encrypted\",\"id\":\"call_1\",\"signature\":\"BLOB\"}]");
    free(d);
    json_free(p);
    json_free(msgs);
    /* an encrypted-only turn stays empty, so the empty-stop retry still runs */
    st = oaic_stream_new(&t.m, NULL, NULL);
    rc = feed_sse(oaic_stream_on_sse, st,
                  "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.encrypted\",\"data\":\"X\"}]},"
                  "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n",
                  4096);
    msg = oaic_stream_finish(st, rc, NULL);
    CHECK(json_len(json_get(msg, "content")) == 0);
    json_free(msg);
    json_free(t.raw);
}

/* one streamed tool-call turn whose first delta is `delta_json`, replayed in the next request: the assistant message */
static json_t *gemini_sig_replay(tmodel_t *t, const char *delta_json, json_t **streamed) {
    char *sse = xasprintf("data: {\"id\":\"chatcmpl-gemini\",\"choices\":[{\"index\":0,\"delta\":%s}]}\n\n"
                          "data: {\"id\":\"chatcmpl-gemini\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
                          "data: [DONE]\n\n",
                          delta_json);
    oaic_stream_t *st = oaic_stream_new(&t->m, NULL, NULL);
    int rc = feed_sse(oaic_stream_on_sse, st, sse, 4096);
    free(sse);
    json_t *msg = oaic_stream_finish(st, rc, NULL);
    *streamed = json_clone(msg);
    json_t *msgs = json_arr();
    json_push(msgs, J("{\"role\":\"user\",\"content\":\"Read README.md\",\"timestamp\":1}"));
    /* the restored history: the turn round-trips through JSON like a session file */
    char *d = json_dump(msg, 0);
    json_free(msg);
    json_push(msgs, J(d));
    free(d);
    json_push(msgs, J("{\"role\":\"toolResult\",\"toolCallId\":\"call_1\",\"toolName\":\"read\",\"content\":[{\"type\":\"text\","
                      "\"text\":\"README contents\"}],\"isError\":false,\"timestamp\":2}"));
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    json_t *p = oaic_build_request(&t->m, &ctx, &o);
    json_t *am = NULL, *x;
    json_arr_foreach(json_get(p, "messages"), i, x) if (!am && str_eq(json_get_str(x, "role"), "assistant")) am = x;
    json_t *out = json_clone(am);
    json_free(p);
    json_free(msgs);
    return out;
}

/* omp 7e3718f535 (openai-completions-thought-signature.test.ts): Gemini signatures on the OpenAI-compatible wire round-trip
 * from each place they arrive (extra_content google/vertex, function.thought_signature, a message-level field), and all
 * three together on one call */
static void test_gemini_thought_signatures(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"gemini-3.7-flash\",\"api\":\"openai-completions\",\"provider\":\"gemini\","
                   "\"baseUrl\":\"https://generativelanguage.googleapis.com/v1beta/openai\",\"reasoning\":true,\"compat\":{}}");
    static const char CALL[] = "\"index\":0,\"id\":\"call_1\",\"type\":\"function\"";
    json_t *st, *am;
    char *delta;
    for (int ns = 0; ns < 2; ns++) {
        const char *n = ns ? "vertex" : "google";
        delta = xasprintf("{\"role\":\"assistant\",\"tool_calls\":[{%s,\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":"
                          "\\\"README.md\\\"}\"},\"extra_content\":{\"%s\":{\"thought_signature\":\"opaque-signature\"}}}]}",
                          CALL, n);
        am = gemini_sig_replay(&t, delta, &st);
        char *want = xasprintf("{\"%s\":{\"thought_signature\":\"opaque-signature\"}}", n);
        char *got = json_dump(json_get(json_at(json_get(am, "tool_calls"), 0), "extra_content"), 0);
        CHECK_STR(got, want);
        free(got);
        free(want);
        json_free(am);
        json_free(st);
        free(delta);
    }
    delta = xasprintf("{\"role\":\"assistant\",\"tool_calls\":[{%s,\"function\":{\"name\":\"read\",\"arguments\":\"{}\","
                      "\"thought_signature\":\"opaque-signature\"}}]}",
                      CALL);
    am = gemini_sig_replay(&t, delta, &st);
    CHECK_STR(json_get_str(block_of(st, "toolCall", 0), "thoughtSignature"), "{\"function\":\"opaque-signature\"}");
    json_t *tc = json_at(json_get(am, "tool_calls"), 0);
    char *fn = json_dump(json_get(tc, "function"), 0);
    CHECK_STR(fn, "{\"name\":\"read\",\"arguments\":\"{}\",\"thought_signature\":\"opaque-signature\"}");
    free(fn);
    CHECK(!json_get(tc, "extra_content") && !json_get(am, "reasoning_details"));
    json_free(am);
    json_free(st);
    free(delta);
    static const char *const fields[] = {"thinking_signature", "thought_signature", "reasoning_signature"};
    for (size_t f = 0; f < ARRAY_LEN(fields); f++) {
        delta = xasprintf("{\"role\":\"assistant\",\"%s\":\"opaque-signature\",\"tool_calls\":[{%s,\"function\":{\"name\":\"read\","
                          "\"arguments\":\"{}\"}}]}",
                          fields[f], CALL);
        am = gemini_sig_replay(&t, delta, &st);
        CHECK_STR(json_get_str(am, fields[f]), "opaque-signature");
        for (size_t g = 0; g < ARRAY_LEN(fields); g++)
            if (g != f) CHECK(!json_get(am, fields[g]));
        json_free(am);
        json_free(st);
        free(delta);
    }
    /* all three on one call: each kept beside the others */
    am = gemini_sig_replay(&t,
                           "{\"role\":\"assistant\",\"thinking_signature\":\"message-sig\",\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
                           "\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{}\",\"thought_signature\":\"function-sig\"},"
                           "\"extra_content\":{\"google\":{\"thought_signature\":\"per-call-sig\"}}}]}",
                           &st);
    CHECK_STR(json_get_str(block_of(st, "toolCall", 0), "thoughtSignature"),
              "{\"perCall\":{\"google\":{\"thought_signature\":\"per-call-sig\"}},\"function\":\"function-sig\","
              "\"message\":{\"thinking_signature\":\"message-sig\"}}");
    CHECK_STR(json_get_str(am, "thinking_signature"), "message-sig");
    tc = json_at(json_get(am, "tool_calls"), 0);
    CHECK_STR(json_get_str(json_get(tc, "function"), "thought_signature"), "function-sig");
    CHECK_STR(json_get_str(json_get(json_get(tc, "extra_content"), "google"), "thought_signature"), "per-call-sig");
    json_free(am);
    json_free(st);
    /* a message-level signature that streams before the call waits for it; a legacy raw extra_content still replays */
    oaic_stream_t *s = oaic_stream_new(&t.m, NULL, NULL);
    int rc = feed_sse(oaic_stream_on_sse, s,
                      "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"reasoning_signature\":\"early\"}}]}\n\n"
                      "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":"
                      "{\"name\":\"read\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n",
                      4096);
    json_t *msg = oaic_stream_finish(s, rc, NULL);
    CHECK_STR(json_get_str(block_of(msg, "toolCall", 0), "thoughtSignature"), "{\"message\":{\"reasoning_signature\":\"early\"}}");
    json_free(msg);
    json_free(t.raw);
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

/* omp 02eb912b76 (openrouter-llama-split-tool-calls.test.ts): compat splitToolCalls replays each call of a multi-call turn
 * directly before its own output, text on the first call, without the turn's native items (results in any order) */
static void test_split_tool_calls(void) {
    static const char HIST[] =
        "[{\"role\":\"user\",\"content\":\"Compare a.ts and b.ts.\",\"timestamp\":1},"
        "{\"role\":\"assistant\",\"api\":\"openai-responses\",\"provider\":\"openrouter\",\"model\":\"meta-llama/llama-3.1-8b-instruct\","
        "\"stopReason\":\"toolUse\",\"timestamp\":1,\"content\":[{\"type\":\"text\",\"text\":\"Reading both.\"},"
        "{\"type\":\"toolCall\",\"id\":\"call_1\",\"name\":\"read\",\"arguments\":{\"path\":\"a.ts\"}},"
        "{\"type\":\"toolCall\",\"id\":\"call_2\",\"name\":\"read\",\"arguments\":{\"path\":\"b.ts\"}}],"
        "\"providerPayload\":{\"type\":\"openaiResponsesHistory\",\"provider\":\"openrouter\",\"items\":["
        "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Reading both.\"}]},"
        "{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"a.ts\\\"}\"},"
        "{\"type\":\"function_call\",\"call_id\":\"call_2\",\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"b.ts\\\"}\"}]}},"
        "{\"role\":\"toolResult\",\"toolCallId\":\"call_2\",\"toolName\":\"read\",\"content\":[{\"type\":\"text\",\"text\":\"export const b = 2;\"}],"
        "\"isError\":false,\"timestamp\":2},"
        "{\"role\":\"toolResult\",\"toolCallId\":\"call_1\",\"toolName\":\"read\",\"content\":[{\"type\":\"text\",\"text\":\"export const a = 1;\"}],"
        "\"isError\":false,\"timestamp\":2},"
        "{\"role\":\"user\",\"content\":\"Which is larger?\",\"timestamp\":3}]";
    for (int split = 0; split < 2; split++) {
        tmodel_t t;
        char *mj = xasprintf("{\"id\":\"meta-llama/llama-3.1-8b-instruct\",\"api\":\"openai-responses\",\"provider\":\"openrouter\","
                             "\"baseUrl\":\"https://openrouter.ai/api/v1\",\"compat\":{\"isOpenRouterHost\":true,\"splitToolCalls\":%s}}",
                             split ? "true" : "false");
        model_init(&t, mj);
        free(mj);
        json_t *msgs = J(HIST);
        ai_context_t ctx = {.messages = msgs};
        json_t *in = oair_convert_input(&t.m, &ctx, 0);
        sb_t order = SB_INIT;
        json_t *it;
        json_arr_foreach(in, i, it) {
            const char *ty = json_get_str(it, "type");
            if (str_eq(ty, "reasoning")) continue;
            if (!ty || str_eq(ty, "message")) sb_appendf(&order, "message:%s ", json_get_str(it, "role"));
            else sb_appendf(&order, "%s:%s ", ty, json_get_str(it, "call_id"));
        }
        CHECK_STR(sb_cstr(&order), split ? "message:user message:assistant function_call:call_1 function_call_output:call_1 "
                                           "function_call:call_2 function_call_output:call_2 message:user "
                                         : "message:user message:assistant function_call:call_1 function_call:call_2 "
                                           "function_call_output:call_2 function_call_output:call_1 message:user ");
        sb_free(&order);
        json_free(in);
        json_free(msgs);
        json_free(t.raw);
    }
    /* chat completions: one tool call per assistant message, the text on the first */
    tmodel_t t;
    model_init(&t, "{\"id\":\"llama3.1-8b\",\"api\":\"openai-completions\",\"provider\":\"cerebras\",\"compat\":{\"splitToolCalls\":true}}");
    json_t *msgs = J(HIST);
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    json_t *p = oaic_build_request(&t.m, &ctx, &o);
    sb_t order = SB_INIT;
    json_t *m;
    json_arr_foreach(json_get(p, "messages"), i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            json_t *tcs = json_get(m, "tool_calls");
            sb_appendf(&order, "assistant(%s,%zu:%s) ", json_is_str(json_get(m, "content")) ? json_get_str(m, "content") : "",
                       json_len(tcs), json_get_str(json_at(tcs, 0), "id"));
        } else if (str_eq(role, "tool")) sb_appendf(&order, "tool:%s ", json_get_str(m, "tool_call_id"));
        else sb_appendf(&order, "%s ", role);
    }
    CHECK_STR(sb_cstr(&order), "user assistant(Reading both.,1:call_1) tool:call_1 assistant(,1:call_2) tool:call_2 user ");
    sb_free(&order);
    json_free(p);
    json_free(msgs);
    json_free(t.raw);
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
    /* moveCodexInstructionsToInput: no top-level instructions; the base instructions lead the input (no cache key: no id) */
    CHECK(json_get(p, "instructions") == NULL);
    CHECK(json_get(p, "store") && !json_get_bool(p, "store", true));
    CHECK(json_get(p, "max_output_tokens") == NULL);
    CHECK(json_get(p, "temperature") == NULL);
    CHECK_STR(json_get_str(json_get_obj(p, "reasoning"), "effort"), "medium");
    CHECK_STR(json_get_str(json_get_obj(p, "reasoning"), "summary"), "auto");
    CHECK_STR(json_sv(json_at(json_get(p, "include"), 0)), "reasoning.encrypted_content");
    json_t *in = json_get(p, "input"), *it;
    CHECK_STR(json_get_str(json_at(in, 0), "role"), "developer");
    CHECK_STR(json_get_str(json_at(json_get(json_at(in, 0), "content"), 0), "text"), "base instructions");
    CHECK_STR(json_sv(json_at(json_get(json_get(json_at(in, 0), "internal_chat_message_metadata_passthrough"), "content_item_kinds"), 0)),
              "model.base_instructions");
    CHECK_STR(json_get_str(json_at(in, 1), "role"), "developer");
    CHECK_STR(json_get_str(json_at(json_get(json_at(in, 1), "content"), 0), "text"), "extra dev");
    json_arr_foreach(in, i, it) CHECK(json_get(it, "id") == NULL);
    /* the item id: uuid5(uuid5(NAMESPACE_OID, thread), text), as codex-rs build_responses_request computes it */
    json_t *mv = J("{\"instructions\":\"sys\",\"prompt_cache_key\":\"thread-1\",\"input\":[{\"role\":\"user\"}]}");
    oair_codex_instructions_to_input(mv);
    char *mvs = json_dump(mv, 0);
    CHECK_STR(mvs, "{\"prompt_cache_key\":\"thread-1\",\"input\":[{\"id\":\"msg_6e6ddc8f-32b3-592b-a5b6-b5f2aface174\","
                   "\"type\":\"message\",\"role\":\"developer\",\"content\":[{\"type\":\"input_text\",\"text\":\"sys\"}],"
                   "\"internal_chat_message_metadata_passthrough\":{\"content_item_kinds\":[\"model.base_instructions\"]}},"
                   "{\"role\":\"user\"}]}");
    free(mvs);
    json_free(mv);
    mv = J("{\"instructions\":\"\",\"input\":[]}");
    oair_codex_instructions_to_input(mv);
    mvs = json_dump(mv, 0);
    CHECK_STR(mvs, "{\"input\":[]}");
    free(mvs);
    json_free(mv);
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
    CHECK(strstr(pms, "turn_id") == NULL); /* only the base-instructions item carries the passthrough key */
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
    {
        /* a refused encrypted reasoning replay (omp 80ad1d6b2e): turns before the floor marker (and every turn of the
         * retrying request) replay no reasoning item; the floor turn and later ones keep theirs */
        json_t *hist = json_arr();
        json_push(hist, J("{\"role\":\"user\",\"content\":\"q1\"}"));
        json_push(hist, json_clone(msg));
        json_push(hist, J("{\"role\":\"user\",\"content\":\"q2\"}"));
        json_t *floor_turn = json_clone(msg);
        json_set_bool(floor_turn, "encryptedReasoningFloor", true);
        json_push(hist, floor_turn);
        json_push(hist, J("{\"role\":\"user\",\"content\":\"q3\"}"));
        ai_context_t ctx = {.messages = hist};
        json_t *in = oair_convert_input(&t.m, &ctx, 0);
        int reasoning_at = -1, n_reasoning = 0;
        json_t *it;
        json_arr_foreach(in, i, it) if (str_eq(json_get_str(it, "type"), "reasoning")) {
            n_reasoning++;
            reasoning_at = (int)i;
        }
        CHECK(n_reasoning == 1 && reasoning_at > 2);
        json_free(in);
        ctx.drop_encrypted_reasoning = true;
        in = oair_convert_input(&t.m, &ctx, 0);
        n_reasoning = 0;
        json_arr_foreach(in, i, it) n_reasoning += str_eq(json_get_str(it, "type"), "reasoning");
        CHECK(n_reasoning == 0);
        json_free(in);
        json_free(hist);
        CHECK(oair_refused_encrypted_reasoning("400 The encrypted content was not issued to this caller (code=invalid_encrypted_content)"));
        CHECK(oair_refused_encrypted_reasoning("reasoning `encrypted_content` has expired"));
        CHECK(oair_refused_encrypted_reasoning("gateway: Encrypted content could not be verified"));
        CHECK(!oair_refused_encrypted_reasoning("encrypted content\nwas not issued to this caller"));
        CHECK(!oair_refused_encrypted_reasoning("400 bad request"));
    }
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


/* ---------- leaked-thinking healing (leaked-thinking-stream.ts, StreamMarkupHealing) ---------- */

/* event trace: "type idx [delta]" lines */
static void trace_ev(void *ud, const ai_event_t *ev) {
    sb_t *b = ud;
    sb_appendf(b, "%s", ai_event_name(ev->type));
    if (ev->content_index >= 0) sb_appendf(b, " %d", ev->content_index);
    if (ev->delta && ev->delta_len) sb_appendf(b, " %.*s", (int)ev->delta_len, ev->delta);
    sb_append(b, "|");
}

typedef enum { SRC_TEXT, SRC_THINK, SRC_SIG, SRC_TOOL, SRC_END } src_kind_t;
typedef struct {
    src_kind_t kind;
    const char *s;
} src_step_t;

/* drive the projector with an ai_builder as the provider; returns the healed message (owned) */
static json_t *project(const ai_model_t *m, const src_step_t *steps, sb_t *trace) {
    leaked_thinking_t *w = leaked_thinking_new(trace_ev, trace);
    ai_builder_t b;
    aib_init(&b, m, leaked_thinking_cb, w);
    aib_start(&b);
    for (const src_step_t *st = steps; st->kind != SRC_END; st++) {
        if (st->kind == SRC_TEXT) aib_text_delta(&b, st->s, strlen(st->s));
        else if (st->kind == SRC_THINK) aib_thinking_delta(&b, st->s, strlen(st->s));
        else if (st->kind == SRC_SIG) aib_set_thinking_signature(&b, st->s);
        else {
            aib_toolcall_start(&b, "call_1", st->s);
            aib_toolcall_delta(&b, "{}", 2);
        }
    }
    json_t *msg = leaked_thinking_finish(w, aib_finish(&b, "stop", NULL));
    leaked_thinking_free(w);
    aib_free(&b);
    return msg;
}

static void check_projection(const ai_model_t *m, const src_step_t *steps, const char *want_trace, const char *want_content) {
    sb_t tr = SB_INIT;
    json_t *msg = project(m, steps, &tr);
    CHECK_STR(sb_cstr(&tr), want_trace);
    char *c = json_dump(json_get(msg, "content"), 0);
    CHECK_STR(c, want_content);
    free(c);
    json_free(msg);
    sb_free(&tr);
}

static void test_leaked_thinking(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"m1\",\"api\":\"openai-completions\",\"provider\":\"test\",\"compat\":{}}");
    /* a fence split across chunks becomes a thinking block live */
    check_projection(&t.m, (src_step_t[]){{SRC_TEXT, "<thi"}, {SRC_TEXT, "nk>abc</th"}, {SRC_TEXT, "ink>tail"}, {SRC_END, 0}},
                     "start|thinking_start 0|thinking_delta 0 abc|thinking_end 0|text_start 1|text_delta 1 tail|text_end 1|done|",
                     "[{\"type\":\"thinking\",\"thinking\":\"abc\"},{\"type\":\"text\",\"text\":\"tail\"}]");
    /* prefilled opener: only the close streams; the leading text block is re-projected in place */
    check_projection(&t.m, (src_step_t[]){{SRC_TEXT, "plan it"}, {SRC_TEXT, "</think>"}, {SRC_TEXT, "Answer"}, {SRC_END, 0}},
                     "start|text_start 0|text_delta 0 plan it|text_end 0|thinking_start 0|thinking_delta 0 plan it|thinking_end 0|"
                     "text_start 1|text_delta 1 Answer|text_end 1|done|",
                     "[{\"type\":\"thinking\",\"thinking\":\"plan it\"},{\"type\":\"text\",\"text\":\"Answer\"}]");
    /* a stray close after other content is dropped */
    check_projection(&t.m, (src_step_t[]){{SRC_TEXT, "<thinking>a</thinking>b</think>c"}, {SRC_END, 0}},
                     "start|thinking_start 0|thinking_delta 0 a|thinking_end 0|text_start 1|text_delta 1 b|text_delta 1 c|text_end 1|done|",
                     "[{\"type\":\"thinking\",\"thinking\":\"a\"},{\"type\":\"text\",\"text\":\"bc\"}]");
    /* a literal tag inside a code span stays visible; plain text passes through unchanged */
    check_projection(&t.m, (src_step_t[]){{SRC_TEXT, "use `<think>` ok"}, {SRC_END, 0}},
                     "start|text_start 0|text_delta 0 use |text_delta 0 `|text_delta 0 <think>`|text_delta 0  ok|text_end 0|done|",
                     "[{\"type\":\"text\",\"text\":\"use `<think>` ok\"}]");
    /* native thinking keeps its signature; held text is released before a tool call */
    check_projection(&t.m,
                     (src_step_t[]){{SRC_THINK, "native"}, {SRC_SIG, "sig1"}, {SRC_TEXT, "x <"}, {SRC_TOOL, "read"}, {SRC_END, 0}},
                     "start|thinking_start 0|thinking_delta 0 native|thinking_end 0|text_start 1|text_delta 1 x |text_delta 1 <|"
                     "text_end 1|toolcall_start 2|toolcall_delta 2 {}|toolcall_end 2|done|",
                     "[{\"type\":\"thinking\",\"thinking\":\"native\",\"thinkingSignature\":\"sig1\"},{\"type\":\"text\",\"text\":\"x <\"},"
                     "{\"type\":\"toolCall\",\"id\":\"call_1\",\"name\":\"read\",\"arguments\":{}}]");
    /* an unterminated leak ends as thinking */
    check_projection(&t.m, (src_step_t[]){{SRC_TEXT, "```thinking\nstill going"}, {SRC_END, 0}},
                     "start|thinking_start 0|thinking_delta 0 still going|thinking_end 0|done|",
                     "[{\"type\":\"thinking\",\"thinking\":\"still going\"}]");

    /* an error with empty content is forwarded as is */
    {
        sb_t tr = SB_INIT;
        leaked_thinking_t *w = leaked_thinking_new(trace_ev, &tr);
        ai_builder_t b;
        aib_init(&b, &t.m, leaked_thinking_cb, w);
        aib_start(&b);
        json_t *msg = leaked_thinking_finish(w, aib_finish(&b, "error", "boom"));
        CHECK_STR(sb_cstr(&tr), "start|error|");
        CHECK_STR(json_get_str(msg, "errorMessage"), "boom");
        CHECK(json_len(json_get(msg, "content")) == 0);
        json_free(msg);
        leaked_thinking_free(w);
        aib_free(&b);
        sb_free(&tr);
    }

    /* the gate: official first-party endpoints are not wrapped */
    CHECK(!ai_reaches_official_endpoint(&t.m));
    tmodel_t off;
    model_init(&off, "{\"id\":\"gpt\",\"api\":\"openai-responses\",\"provider\":\"openai\",\"compat\":{\"officialEndpoint\":true}}");
    CHECK(ai_reaches_official_endpoint(&off.m));
    json_free(off.raw);
    model_init(&off, "{\"id\":\"c\",\"api\":\"anthropic-messages\",\"provider\":\"anthropic\",\"compat\":{\"officialEndpoint\":true}}");
    unsetenv("ANTHROPIC_BASE_URL");
    CHECK(ai_reaches_official_endpoint(&off.m));
    setenv("ANTHROPIC_BASE_URL", "https://proxy.example.com/v1", 1); /* rerouted at request time */
    CHECK(!ai_reaches_official_endpoint(&off.m));
    setenv("ANTHROPIC_BASE_URL", "https://api.anthropic.com/v1/", 1);
    CHECK(ai_reaches_official_endpoint(&off.m));
    unsetenv("ANTHROPIC_BASE_URL");
    json_free(off.raw);

    /* StreamMarkupHealing in the chat-completions provider: healed thinking is the model's trace (summary:false),
     * dropped once explicit reasoning streamed */
    json_free(t.raw);
    model_init(&t, "{\"id\":\"m1\",\"api\":\"openai-completions\",\"provider\":\"test\",\"compat\":{\"streamMarkupHealingPattern\":\"thinking\"}}");
    const char *sse =
        "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"<thin\"}}]}\n\n"
        "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"king>why</thinking>Hi\"}}]}\n\n"
        "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    oaic_stream_t *st = oaic_stream_new(&t.m, NULL, NULL);
    int rc = feed_sse(oaic_stream_on_sse, st, sse, 5);
    json_t *msg = oaic_stream_finish(st, rc, NULL);
    char *c = json_dump(json_get(msg, "content"), 0);
    CHECK_STR(c, "[{\"type\":\"thinking\",\"thinking\":\"why\",\"summary\":false},{\"type\":\"text\",\"text\":\"Hi\"}]");
    free(c);
    json_free(msg);
    sse = "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"real\"}}]}\n\n"
          "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"<think>leak</think>Hi\"}}]}\n\n"
          "data: {\"id\":\"c\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
          "data: [DONE]\n\n";
    st = oaic_stream_new(&t.m, NULL, NULL);
    rc = feed_sse(oaic_stream_on_sse, st, sse, 4096);
    msg = oaic_stream_finish(st, rc, NULL);
    c = json_dump(json_get(msg, "content"), 0);
    CHECK_STR(c, "[{\"type\":\"thinking\",\"thinking\":\"real\",\"thinkingSignature\":\"reasoning_content\",\"summary\":false},"
                 "{\"type\":\"text\",\"text\":\"Hi\"}]");
    free(c);
    json_free(msg);
    json_free(t.raw);
}

static char *q2(const char *s);

/* tryEnforceStrictSchema result as compact JSON plus the strict flag ("S:" strict, "N:" fallback); owned */
static char *strict_of(const char *in) {
    char *qi = q2(in);
    json_t *s = J(qi);
    bool st = false;
    json_t *r = ts_try_enforce_strict(s, &st);
    char *d = json_dump(r, 0), *out = xasprintf("%s%s", st ? "S:" : "N:", d);
    free(d);
    json_free(r);
    json_free(s);
    free(qi);
    return out;
}

static void check_strict(const char *in, const char *want, int line) {
    char *got = strict_of(in), *qw = q2(want);
    g_checks++;
    if (strcmp(got, qw)) {
        g_fail++;
        fprintf(stderr, "%s:%d: strict\n  got  %s\n  want %s\n", __FILE__, line, got, qw);
    }
    free(got);
    free(qw);
}
#define STRICT(in, want) check_strict(in, want, __LINE__)

static void check_ts(json_t *(*fn)(const json_t *), const char *in, const char *want, int line) {
    char *qi = q2(in), *qw = q2(want);
    json_t *s = J(qi), *w = J(qw), *r = fn(s);
    char *got = json_dump(r, 0), *exp = json_dump(w, 0);
    g_checks++;
    if (strcmp(got, exp)) {
        g_fail++;
        fprintf(stderr, "%s:%d: schema\n  got  %s\n  want %s\n", __FILE__, line, got, exp);
    }
    free(got);
    free(exp);
    json_free(r);
    json_free(s);
    json_free(w);
    free(qi);
    free(qw);
}
#define TS(fn, in, want) check_ts(fn, in, want, __LINE__)

/* omp's tool-schema pipeline (toolschema.c): upstream's cases from schema-strict-mode.test.ts, schema-normalization and
 * the draft/dereference/wire tests, with omp's key order (built by hand from omp's spreads and assignments) */
static void test_strict_schema(void) {
    /* draft-07 -> 2020-12 */
    TS(ts_upgrade_2020_12,
       "{'$schema':'http://json-schema.org/draft-07/schema#','type':'object','definitions':{'value':{'type':'string'}},"
       "'properties':{'value':{'$ref':'#%2Fdefinitions%2Fvalue'}},'required':['value']}",
       "{'$schema':'https://json-schema.org/draft/2020-12/schema','type':'object','$defs':{'value':{'type':'string'}},"
       "'properties':{'value':{'$ref':'#/$defs/value'}},'required':['value']}");
    TS(ts_upgrade_2020_12, "{'type':'array','items':[{'type':'string'},{'type':'number'}],'additionalItems':false}",
       "{'type':'array','prefixItems':[{'type':'string'},{'type':'number'}],'items':false}");
    TS(ts_upgrade_2020_12, "{'type':'object','properties':{'a':{'type':'string'}},'dependencies':{'a':['b'],'c':{'required':['d']}}}",
       "{'type':'object','properties':{'a':{'type':'string'}},'dependentRequired':{'a':['b']},'dependentSchemas':{'c':{'required':['d']}}}");
    TS(ts_upgrade_2020_12,
       "{'type':'object','properties':{'n':{'type':'string','nullable':true},'o':{'nullable':true,'properties':{}}}}",
       "{'type':'object','properties':{'n':{'type':['string','null']},'o':{'anyOf':[{'properties':{}},{'type':'null'}]}}}");
    TS(ts_upgrade_2020_12, "{'type':'object','properties':{'a':{'type':'string','enum':['x']}}}",
       "{'type':'object','properties':{'a':{'type':'string','enum':['x']}}}");
    /* local `$ref` pointers: percent-decoding, ~1 / ~0, malformed and external refs */
    strvec_t tok = {0};
    CHECK(ts_ref_tokens("#", &tok) && tok.n == 0);
    CHECK(ts_ref_tokens("#/$defs/a%252Fb%20~1~01%20%E7%A9%BA%0A", &tok) && tok.n == 2 && !strcmp(tok.v[0], "$defs") &&
          !strcmp(tok.v[1], "a%2Fb /~1 \xe7\xa9\xba\n"));
    strvec_free(&tok);
    CHECK(!ts_ref_tokens("#/$defs/%E0%A4%A", &tok) && !ts_ref_tokens("https://example.com/schema#/$defs/value", &tok));
    CHECK(!ts_ref_tokens("#$defs", &tok) && !ts_ref_tokens("#/$defs/%C0%80", &tok));
    strvec_push(&tok, "$defs");
    strvec_push(&tok, "a/b~ c\xe7\xa9\xba");
    char *f = ts_ref_format(&tok);
    CHECK_STR(f, "#/$defs/a~1b~0%20c%E7%A9%BA");
    free(f);
    strvec_free(&tok);
    /* dereferencing inlines local refs (siblings merged over the def) and drops the definition blocks */
    TS(ts_dereference,
       "{'type':'object','$defs':{'a%2Fb /~1 x':{'type':'string'},'':{'type':'integer'}},'properties':{'encoded':{'$ref':"
       "'#/$defs/a%252Fb%20~1~01%20x','description':'encoded'},'empty':{'$ref':'#/$defs/','description':'empty'},"
       "'ext':{'$ref':'https://example.com/s','description':'ext'}},'required':['encoded']}",
       "{'type':'object','properties':{'encoded':{'type':'string','description':'encoded'},'empty':{'type':'integer',"
       "'description':'empty'},'ext':{'$ref':'https://example.com/s','description':'ext'}},'required':['encoded']}");
    TS(ts_dereference, "{'type':'object','$defs':{'N':{'type':'object','properties':{'next':{'$ref':'#/$defs/N'}}}},"
                       "'properties':{'head':{'$ref':'#/$defs/N'}}}",
       "{'type':'object','properties':{'head':{'type':'object','properties':{'next':{}}}}}");
    /* MCP input schemas: $schema and nullable dropped, const and all-const unions as typed enums */
    TS(ts_mcp_normalize,
       "{'$schema':'http://json-schema.org/draft-07/schema#','type':'object','properties':{'mode':{'oneOf':[{'const':'a'},"
       "{'const':'b'}],'description':'m'},'fixed':{'const':5},'opt':{'type':'string','nullable':true},'mixed':{'anyOf':"
       "[{'const':1},{'const':null}]}},'required':['mode']}",
       "{'type':'object','properties':{'mode':{'enum':['a','b'],'type':'string','description':'m'},'fixed':{'enum':[5],"
       "'type':'number'},'opt':{'type':['string','null']},'mixed':{'enum':[1,null],'type':'number'}},'required':['mode']}");
    /* the wire pass: nullable scalar unions as type arrays, described const unions collapsed, bare enums typed, {} -> true */
    TS(ts_wire_schema,
       "{'type':'object','properties':{'g':{'anyOf':[{'type':'string','enum':['x','y'],'description':'guide'},{'type':'null'}],"
       "'default':null,'description':'guide'},'k':{'anyOf':[{'const':'a','description':'kind'},{'const':'b','description':"
       "'kind'}],'description':'kind'},'e':{'enum':[1,2]},'m':{}},'additionalProperties':{}}",
       "{'type':'object','properties':{'g':{'default':null,'description':'guide','enum':['x','y',null],'type':['string','null']},"
       "'k':{'description':'kind','type':'string','enum':['a','b']},'e':{'enum':[1,2],'type':'number'},'m':true},"
       "'additionalProperties':true}");
    TS(ts_wire_schema, "{'type':'object','properties':{'path /** where to look */':{'type':'string'}},'required':['path /** where to look */']}",
       "{'type':'object','properties':{'path':{'type':'string','description':'where to look'}},'required':['path']}");
    /* strict mode: non-structural keywords dropped, defaults folded into descriptions, type last, objects closed */
    STRICT("{'type':'object','properties':{'path':{'type':'string'},'limit':{'type':'integer','minimum':1,'maximum':200}},"
           "'additionalProperties':false}",
           "S:{'properties':{'path':{'anyOf':[{'type':'string'},{'type':'null'}]},'limit':{'anyOf':[{'type':'integer'},"
           "{'type':'null'}]}},'type':'object','additionalProperties':false,'required':['path','limit']}");
    STRICT("{'type':'object','properties':{'retries':{'type':'number','description':'retry count','default':3},'name':{'type':"
           "'string','format':'email','pattern':'^a'}},'required':['name']}",
           "S:{'properties':{'retries':{'anyOf':[{'type':'number'},{'type':'null'}],'description':'retry count (default: 3)'},"
           "'name':{'type':'string'}},'required':['retries','name'],'type':'object','additionalProperties':false}");
    STRICT("{'type':['string','null'],'enum':['javascript','python',null],'description':'guide'}",
           "S:{'anyOf':[{'enum':['javascript','python'],'type':'string'},{'enum':[null],'type':'null'}],'description':'guide'}");
    STRICT("{'type':'object','properties':{'guide':{'default':null,'description':'guide','enum':['javascript','python',null],"
           "'type':['string','null']}},'required':['guide'],'additionalProperties':false}",
           "S:{'properties':{'guide':{'anyOf':[{'enum':['javascript','python'],'type':'string'},{'enum':[null],'type':'null'}],"
           "'description':'guide (default: null)'}},'required':['guide'],'type':'object','additionalProperties':false}");
    /* 35e42d9e8a: an object whose shape comes from allOf or $ref stays strict, the shape inlined */
    STRICT("{'type':'object','properties':{'merged':{'type':'object','allOf':[{'type':'object','properties':{'a':{'type':"
           "'string'}},'required':['a'],'additionalProperties':false}]},'referenced':{'type':'object','$ref':'#/$defs/Shape'}},"
           "'required':['merged','referenced'],'additionalProperties':false,'$defs':{'Shape':{'type':'object','properties':"
           "{'a':{'type':'string'}},'required':['a'],'additionalProperties':false}}}",
           "S:{'properties':{'merged':{'properties':{'a':{'type':'string'}},'required':['a'],'type':'object',"
           "'additionalProperties':false},'referenced':{'properties':{'a':{'type':'string'}},'required':['a'],'type':'object',"
           "'additionalProperties':false}},'required':['merged','referenced'],'$defs':{'Shape':{'properties':{'a':{'type':"
           "'string'}},'required':['a'],'type':'object','additionalProperties':false}},'type':'object','additionalProperties':false}");
    /* a $ref with siblings is unravelled (siblings win); a bare $ref stays */
    STRICT("{'type':'object','$defs':{'Star':{'type':'object','properties':{'name':{'type':'string','description':'The name of the "
           "star.'}},'required':['name']}},'properties':{'largest_star':{'$ref':'#/$defs/Star','description':'The largest star in "
           "the galaxy.'},'foo':{'$ref':'#/$defs/Star'}},'required':['largest_star','foo']}",
           "S:{'$defs':{'Star':{'properties':{'name':{'description':'The name of the star.','type':'string'}},'required':['name'],"
           "'type':'object','additionalProperties':false}},'properties':{'largest_star':{'properties':{'name':{'description':"
           "'The name of the star.','type':'string'}},'required':['name'],'description':'The largest star in the galaxy.',"
           "'type':'object','additionalProperties':false},'foo':{'$ref':'#/$defs/Star'}},'required':['largest_star','foo'],"
           "'type':'object','additionalProperties':false}");
    /* single-entry allOf inlined with its keys winning; oneOf and unresolved refs stay (omp keeps them strict) */
    STRICT("{'type':'string','description':'outer','allOf':[{'description':'inner'}]}", "S:{'description':'inner','type':'string'}");
    STRICT("{'type':'object','properties':{'x':{'oneOf':[{'type':'string'}]}},'required':['x']}",
           "S:{'properties':{'x':{'oneOf':[{'type':'string'}]}},'required':['x'],'type':'object','additionalProperties':false}");
    /* nested pure unions splice into the parent; an optional union gains null at the end */
    STRICT("{'type':'object','properties':{'value':{'anyOf':[{'anyOf':[{'type':'string'},{'type':'number'}],'description':'inner "
           "union'},{'type':'boolean'}]},'opt':{'anyOf':[{'type':'string'},{'type':'number'}]}},'required':['value']}",
           "S:{'properties':{'value':{'anyOf':[{'type':'string'},{'type':'number'},{'type':'boolean'}],'description':'inner union'},"
           "'opt':{'anyOf':[{'type':'string'},{'type':'number'},{'type':'null'}]}},'required':['value','opt'],'type':'object',"
           "'additionalProperties':false}");
    /* enum / const roots get an inferred type; unrepresentable shapes fall back to the (upgraded) input */
    STRICT("{'enum':['draft','published']}", "S:{'enum':['draft','published'],'type':'string'}");
    STRICT("{'const':7}", "S:{'enum':[7],'type':'number'}");
    STRICT("{'enum':[1,'two',null]}", "N:{'enum':[1,'two',null]}");
    STRICT("{'const':{'a':1}}", "N:{'const':{'a':1}}");
    STRICT("{'type':'object'}", "N:{'type':'object'}");
    STRICT("{'type':'object','properties':{'meta':{'type':'object'}}}", "N:{'type':'object','properties':{'meta':{'type':'object'}}}");
    STRICT("{'type':'object','properties':{'xs':{'type':'array','items':true}},'required':['xs']}",
           "N:{'type':'object','properties':{'xs':{'type':'array','items':true}},'required':['xs']}");
    STRICT("{'type':'object','properties':{'a':{'anyOf':[true,{'type':'string'}]}}}",
           "N:{'type':'object','properties':{'a':{'anyOf':[true,{'type':'string'}]}}}");
    STRICT("{'type':'object','properties':{},'patternProperties':{'^x':{'type':'string'}}}",
           "N:{'type':'object','properties':{},'patternProperties':{'^x':{'type':'string'}}}");
    STRICT("{'type':'object','properties':{'empty':{'type':'object','properties':{}},'closed':{'type':'object',"
           "'additionalProperties':false}},'required':['empty','closed'],'additionalProperties':false}",
           "S:{'properties':{'empty':{'properties':{},'type':'object','additionalProperties':false,'required':[]},'closed':"
           "{'type':'object','additionalProperties':false,'properties':{},'required':[]}},'required':['empty','closed'],"
           "'type':'object','additionalProperties':false}");
    /* a recursive def with siblings is cut where it re-enters itself (omp's circular graph): non-strict */
    STRICT("{'type':'object','$defs':{'N':{'type':'object','properties':{'next':{'type':'object','properties':{'child':{'$ref':"
           "'#/$defs/N','description':'c'}}}}}},'properties':{'head':{'$ref':'#/$defs/N'}},'required':['head']}",
           "N:{'type':'object','$defs':{'N':{'type':'object','properties':{'next':{'type':'object','properties':{'child':{'$ref':"
           "'#/$defs/N','description':'c'}}}}}},'properties':{'head':{'$ref':'#/$defs/N'}},'required':['head']}");
    /* adaptSchemaForStrict without strict is the upgraded schema */
    json_t *s = J("{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\",\"nullable\":true}}}");
    bool eff = true;
    json_t *r = ts_adapt_for_strict(s, false, &eff);
    char *d = json_dump(r, 0);
    CHECK(!eff);
    CHECK_STR(d, "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":[\"string\",\"null\"]}}}");
    free(d);
    json_free(r);
    json_free(s);
}

/* JSON with single quotes for double quotes (test readability); owned */
static char *q2(const char *s) {
    char *d = xstrdup(s);
    for (char *p = d; *p; p++)
        if (*p == '\'') *p = '"';
    return d;
}

static void check_sanitized(const char *in, bool repair, const char *want, int line) {
    char *qi = q2(in), *qw = q2(want);
    json_t *s = J(qi), *w = J(qw);
    json_t *r = oai_responses_sanitize_schema(s, repair);
    char *got = json_dump(r, 0), *exp = json_dump(w, 0);
    g_checks++;
    if (strcmp(got, exp)) {
        g_fail++;
        fprintf(stderr, "%s:%d: sanitized\n  got  %s\n  want %s\n", __FILE__, line, got, exp);
    }
    free(got);
    free(exp);
    json_free(r);
    json_free(s);
    json_free(w);
    free(qi);
    free(qw);
}
#define SANITIZED(in, repair, want) check_sanitized(in, repair, want, __LINE__)

/* sanitizeSchemaForOpenAIResponses: upstream's cases (schema-normalization.test.ts, openai-codex.test.ts,
 * openai-array-tool-schema.test.ts), with omp's key order; omp e48b03993b, 008e054ba1, 35e42d9e8a */
static void test_responses_sanitizer(void) {
    /* object nodes get `properties`, a bare one says it is open; oneOf becomes anyOf; literals stay */
    SANITIZED("{'type':'object','properties':{'nested':{'type':'object'},'union':{'oneOf':[{'type':'object'}]}},"
              "'oneOf':[{'type':'object'}],'enum':[{'type':'object','oneOf':[{'const':'literal'}]}],"
              "'const':{'type':'object','oneOf':[{'const':'literal'}]},'default':{'type':'object'},'examples':[{'type':'object'}]}",
              false,
              "{'type':'object','properties':{'nested':{'type':'object','properties':{},'additionalProperties':true},"
              "'union':{'anyOf':[{'type':'object','properties':{},'additionalProperties':true}]}},"
              "'enum':[{'type':'object','oneOf':[{'const':'literal'}]}],'const':{'type':'object','oneOf':[{'const':'literal'}]},"
              "'default':{'type':'object'},'examples':[{'type':'object'}],"
              "'anyOf':[{'type':'object','properties':{},'additionalProperties':true}]}");
    SANITIZED("{'type':'object','properties':{'body':{'type':'string','contentSchema':{'type':'object'}}},"
              "'dependencies':{'body':{'type':'object'},'other':['body']}}",
              false,
              "{'type':'object','properties':{'body':{'type':'string','contentSchema':{'type':'object','properties':{},"
              "'additionalProperties':true}}},'dependencies':{'body':{'type':'object','properties':{},'additionalProperties':true},"
              "'other':['body']}}");
    SANITIZED("{'type':['object','null']}", false, "{'type':['object','null'],'properties':{},'additionalProperties':true}");
    /* lookarounds: pattern values dropped, patternProperties keys folded under '.*', property names untouched */
    SANITIZED("{'type':'object','properties':{'fileKey':{'type':'string','pattern':'^(?!undefined$|null$)'},"
              "'ending':{'type':'string','pattern':'(?<=/)node$'},'slug':{'type':'string','pattern':'^[a-z0-9_-]+$'},"
              "'literal':{'type':'string','pattern':'\\\\(?!literal'},'patternOnly':{'pattern':'^(?!bad$)'},"
              "'^(?!property-name)':{'type':'string'}},'patternProperties':{'^(?!secret_)':{'type':'string'},"
              "'(?<=/)node$':{'type':'string'},'^x-':{'type':'object'},'\\\\(?!literal':{'type':'string'}},"
              "'propertyNames':{'pattern':'^(?!invalid$)'}}",
              false,
              "{'type':'object','properties':{'fileKey':{'type':'string'},'ending':{'type':'string'},"
              "'slug':{'type':'string','pattern':'^[a-z0-9_-]+$'},'literal':{'type':'string','pattern':'\\\\(?!literal'},"
              "'patternOnly':true,'^(?!property-name)':{'type':'string'}},'patternProperties':{'.*':{'anyOf':[{'type':'string'},"
              "{'type':'string'}]},'^x-':{'type':'object','properties':{},'additionalProperties':true},"
              "'\\\\(?!literal':{'type':'string'}},'propertyNames':true}");
    /* an unescaped `[` inside a character class (#20137) */
    SANITIZED("{'type':'object','properties':{'nested':{'type':'string','pattern':'^[^[\\\\]]*$'},"
              "'escaped':{'type':'string','pattern':'^[^\\\\[\\\\]]*$'},'outside':{'type':'string','pattern':'^\\\\[[a-z]+\\\\]$'},"
              "'lookaheadLiteral':{'type':'string','pattern':'^[(?=]+$'}},'patternProperties':{'^[[]':{'type':'string'},"
              "'^\\\\[x\\\\]':{'type':'number'}}}",
              false,
              "{'type':'object','properties':{'nested':{'type':'string'},'escaped':{'type':'string','pattern':'^[^\\\\[\\\\]]*$'},"
              "'outside':{'type':'string','pattern':'^\\\\[[a-z]+\\\\]$'},'lookaheadLiteral':{'type':'string','pattern':'^[(?=]+$'}},"
              "'patternProperties':{'.*':{'type':'string'},'^\\\\[x\\\\]':{'type':'number'}}}");
    CHECK(oai_unsupported_regex("^[^[\\]]*$") && oai_unsupported_regex("(?<!a)b") && oai_unsupported_regex("a(?=b)"));
    CHECK(!oai_unsupported_regex("^[^\\[\\]]*$") && !oai_unsupported_regex("\\(?!x") && !oai_unsupported_regex("^[(?=]+$"));
    CHECK(!oai_unsupported_regex("(?:x)") && !oai_unsupported_regex("(?<name>x)") && !oai_unsupported_regex("trailing\\"));
    /* a non-array oneOf is kept verbatim; exclusive-required unions are left to provider handling */
    SANITIZED("{'type':'object','oneOf':{'type':'object'}}", false,
              "{'type':'object','oneOf':{'type':'object'},'properties':{},'additionalProperties':true}");
    SANITIZED("{'type':'object','properties':{'project':{'type':'string'},'paths':{'type':'array','items':{'type':'string'}}},"
              "'required':['project'],'anyOf':[{'required':['paths']},{'required':['scopes']}]}",
              false,
              "{'type':'object','properties':{'project':{'type':'string'},'paths':{'type':'array','items':{'type':'string'}}},"
              "'required':['project'],'anyOf':[{'required':['paths']},{'required':['scopes']}]}");
    /* `{}` is `true`; boolean and non-empty schemas stay */
    SANITIZED("{'type':'object','additionalProperties':{}}", false, "{'type':'object','additionalProperties':true,'properties':{}}");
    SANITIZED("{'type':'array','items':{}}", false, "{'type':'array','items':true}");
    SANITIZED("{'type':'object','properties':{'meta':{}},'required':['meta']}", false,
              "{'type':'object','properties':{'meta':true},'required':['meta']}");
    SANITIZED("{'anyOf':[{},{'type':'string'}]}", false, "{'anyOf':[true,{'type':'string'}]}");
    SANITIZED("{'type':'object','additionalProperties':{'type':'string'},'unevaluatedProperties':false}", false,
              "{'type':'object','additionalProperties':{'type':'string'},'unevaluatedProperties':false,'properties':{}}");
    /* 35e42d9e8a: a $ref or combinator supplies the shape, so the placeholder does not mark the node open */
    SANITIZED("{'type':'object','$ref':'#/$defs/x'}", false, "{'type':'object','$ref':'#/$defs/x','properties':{}}");
    SANITIZED("{'type':'object','allOf':[{'type':'object','properties':{'a':{'type':'string'}}}]}", false,
              "{'type':'object','allOf':[{'type':'object','properties':{'a':{'type':'string'}}}],'properties':{}}");
    /* Codex (openai-codex.test.ts): no-argument objects, lookarounds, patternProperties, nested classes */
    SANITIZED("{'type':'object'}", true, "{'type':'object','properties':{},'additionalProperties':true}");
    SANITIZED("{'type':'object','properties':{'fileKey':{'type':'string','pattern':'^(?!undefined$|null$)'}},"
              "'propertyNames':{'pattern':'^(?!undefined$|null$)'}}",
              true, "{'type':'object','properties':{'fileKey':{'type':'string'}},'propertyNames':true}");
    SANITIZED("{'type':'object','patternProperties':{'^(?!secret_)':{'type':'string'},'^public_':{'type':'string'}},"
              "'additionalProperties':false}",
              true,
              "{'type':'object','patternProperties':{'.*':{'type':'string'},'^public_':{'type':'string'}},"
              "'additionalProperties':false,'properties':{}}");
    /* Codex's open items for itemless arrays (and only for them), off with unevaluatedItems anywhere */
    static const char *const itemless[][2] = {
        {"{'type':'array'}", "{'type':'array','items':true}"},
        {"{'type':['array','null']}", "{'type':['array','null'],'items':true}"},
        {"{'type':'object','properties':{'listeners':{'type':'array'}}}",
         "{'type':'object','properties':{'listeners':{'type':'array','items':true}}}"},
        {"{'anyOf':[{'type':'array'},{'type':'null'}]}", "{'anyOf':[{'type':'array','items':true},{'type':'null'}]}"},
        {"{'type':'array','items':{'type':'array'}}", "{'type':'array','items':{'type':'array','items':true}}"},
        {"{'type':'array','prefixItems':[{'type':'string'}]}", "{'type':'array','prefixItems':[{'type':'string'}],'items':true}"},
        {"{'type':'array','prefixItems':[]}", "{'type':'array','prefixItems':[],'items':true}"},
        {"{'prefixItems':[{'type':'string'}]}", "{'prefixItems':[{'type':'string'}],'items':true}"},
        {"{'prefixItems':[]}", "{'prefixItems':[],'items':true}"},
    };
    for (size_t i = 0; i < ARRAY_LEN(itemless); i++) {
        SANITIZED(itemless[i][0], false, itemless[i][0]);
        SANITIZED(itemless[i][0], true, itemless[i][1]);
        SANITIZED(itemless[i][1], true, itemless[i][1]);
        char *qs = q2(itemless[i][1]);
        json_t *s = J(qs);
        bool st = true;
        json_t *r = ts_try_enforce_strict(s, &st);
        CHECK(!st); /* the open tail is not representable in strict mode */
        json_free(r);
        json_free(s);
        free(qs);
    }
    static const char *const kept[] = {"{'type':'array','items':true}", "{'type':'array','items':false}",
                                       "{'type':'array','prefixItems':[{'type':'string'}],'items':{'type':'number'}}",
                                       "{'type':'array','prefixItems':[],'items':false}",
                                       "{'type':'array','prefixItems':[{'type':'string'}],'unevaluatedItems':false}",
                                       "{'type':'object','properties':{'closed':{'type':'array','allOf':[{'type':'array',"
                                       "'prefixItems':[{'type':'string'}]}],'unevaluatedItems':false},'open':{'type':'array',"
                                       "'prefixItems':[{'type':'string'}]}}}",
                                       "{'type':'string','enum':[{'type':'array'}],'const':{'type':'array'},"
                                       "'default':{'type':'array'},'examples':[{'type':'array'}]}"};
    for (size_t i = 0; i < ARRAY_LEN(kept); i++) SANITIZED(kept[i], true, kept[i]);
    SANITIZED("{'type':'object','$defs':{'unevaluatedItems':{'type':'string'}},'properties':{'unevaluatedItems':{'type':'string'},"
              "'listeners':{'type':'array'}}}",
              true,
              "{'type':'object','$defs':{'unevaluatedItems':{'type':'string'}},'properties':{'unevaluatedItems':{'type':'string'},"
              "'listeners':{'type':'array','items':true}}}");
    SANITIZED("{'type':'object','properties':{'listeners':{'type':'array'}},'const':{'unevaluatedItems':false}}", true,
              "{'type':'object','properties':{'listeners':{'type':'array','items':true}},'const':{'unevaluatedItems':false}}");
}

/* the Responses / Codex / Azure tool converters send sanitized parameters (an MCP lookaround pattern no longer 400s the
 * request); strict adaptation runs on the sanitized schema */
static void test_responses_tool_schemas(void) {
    static const char *const apis[] = {"openai-responses", "openai-codex-responses", "azure-openai-responses"};
    char *qt = q2("[{'name':'mcp__figma_get_design_context','description':'Get Figma design context','parameters':{'type':'object',"
                  "'properties':{'fileKey':{'type':'string','pattern':'^(?!undefined$|null$)'},'meta':{'type':'object'},"
                  "'mode':{'oneOf':[{'const':'a'},{'const':'b'}]}},'required':['fileKey']}}]");
    for (int v = 0; v < 3; v++) {
        tmodel_t t;
        char *mj = xasprintf("{\"id\":\"gpt-5.5\",\"api\":\"%s\",\"provider\":\"p1\",\"baseUrl\":\"https://example.test/v1\","
                             "\"compat\":{\"supportsStrictMode\":true}}",
                             apis[v]);
        model_init(&t, mj);
        free(mj);
        json_t *tools = J(qt), *msgs = J("[{\"role\":\"user\",\"content\":\"hi\",\"timestamp\":1}]");
        ai_context_t ctx = {.messages = msgs, .tools = tools};
        ai_stream_opts_t o;
        ai_stream_opts_init(&o);
        json_t *p = oair_build_request(&t.m, &ctx, &o);
        json_t *tool = json_at(json_get(p, "tools"), 0);
        char *got = json_dump(json_get(tool, "parameters"), 0);
        char *want = q2("{'type':'object','properties':{'fileKey':{'type':'string'},'meta':{'type':'object','properties':{},"
                        "'additionalProperties':true},'mode':{'anyOf':[{'const':'a'},{'const':'b'}]}},'required':['fileKey']}");
        CHECK_STR(got, want);
        /* the open `meta` object keeps the tool non-strict, as in omp */
        CHECK(!json_get_bool(tool, "strict", false));
        free(got);
        free(want);
        json_free(p);
        json_free(tools);
        json_free(msgs);
        json_free(t.raw);
    }
    free(qt);
}

/* a tool's own `strict` across routes on a strict-capable model: `true` and unset go strict where the route allows
 * (Codex only for `true`), `false` never does and is kept on the wire (omp MCPTool / TaskTool / generate_image) */
static void test_tool_strict_flags(void) {
    char *qt = q2("[{'name':'a','description':'','parameters':{'type':'object','properties':{'x':{'type':'string','description':"
                  "'d','default':'v'}},'required':['x']},'strict':true},{'name':'b','description':'','parameters':{'type':"
                  "'object','properties':{'x':{'type':'string','description':'d','default':'v'}},'required':['x']}},{'name':'c',"
                  "'description':'','parameters':{'type':'object','properties':{'x':{'type':'string','description':'d','default':"
                  "'v'}},'required':['x']},'strict':false}]");
    char *strict_form = q2("{'properties':{'x':{'description':'d (default: v)','type':'string'}},'required':['x'],'type':'object',"
                           "'additionalProperties':false}");
    char *plain_form = q2("{'type':'object','properties':{'x':{'type':'string','description':'d','default':'v'}},'required':['x']}");
    /* per api: the flag each of a / b / c gets ("T" true, "F" false, "-" absent); "T" also means the strict form */
    static const struct {
        const char *api, *want;
    } routes[] = {{"openai-completions", "TTF"}, {"openai-responses", "TTF"}, {"openai-codex-responses", "T-F"},
                  {"azure-openai-responses", "FFF"}};
    for (size_t r = 0; r < ARRAY_LEN(routes); r++) {
        tmodel_t t;
        char *mj = xasprintf("{\"id\":\"gpt-5.5\",\"api\":\"%s\",\"provider\":\"p1\",\"reasoning\":false,\"baseUrl\":"
                             "\"https://example.test/v1\",\"compat\":{\"supportsStrictMode\":true}}",
                             routes[r].api);
        model_init(&t, mj);
        free(mj);
        json_t *tools = J(qt), *msgs = J("[{\"role\":\"user\",\"content\":\"hi\",\"timestamp\":1}]");
        ai_context_t ctx = {.messages = msgs, .tools = tools};
        ai_stream_opts_t o;
        ai_stream_opts_init(&o);
        bool cc = !strcmp(routes[r].api, "openai-completions");
        json_t *p = cc ? oaic_build_request(&t.m, &ctx, &o) : oair_build_request(&t.m, &ctx, &o);
        for (size_t i = 0; i < 3; i++) {
            json_t *w = json_at(json_get(p, "tools"), i);
            if (cc) w = json_get_obj(w, "function");
            const json_t *st = json_get(w, "strict");
            char got = !st ? '-' : json_is_bool(st) && st->u.b ? 'T' : 'F';
            char *params = json_dump(json_get(w, "parameters"), 0);
            g_checks++;
            if (got != routes[r].want[i] || strcmp(params, got == 'T' ? strict_form : plain_form)) {
                g_fail++;
                fprintf(stderr, "%s:%d: %s tool %zu: strict %c (want %c), parameters %s\n", __FILE__, __LINE__, routes[r].api, i,
                        got, routes[r].want[i], params);
            }
            free(params);
        }
        json_free(p);
        json_free(tools);
        json_free(msgs);
        json_free(t.raw);
    }
    free(qt);
    free(strict_form);
    free(plain_form);
}

/* ---------- transport retry helpers (fetch-retry.ts, retry-after.ts, retryable.ts) ---------- */

/* omp bd075c82b9: the host's affinity header mirrors the normalized prompt-cache key, none when retention is off */
static void test_prompt_cache_session_header(void) {
    tmodel_t t;
    model_init(&t, "{\"id\":\"m\",\"api\":\"openai-completions\",\"provider\":\"morph\","
                   "\"compat\":{\"promptCacheSessionHeader\":\"x-session-id\"}}");
    ai_stream_opts_t o = {.session_id = "sess-1"};
    http_headers_t h = {0};
    oai_apply_prompt_cache_session_header(&h, &t.m, &o);
    CHECK_STR(http_headers_get(&h, "x-session-id"), "sess-1");
    http_headers_free(&h);
    o.prompt_cache_key = "shared-key";
    oai_apply_prompt_cache_session_header(&h, &t.m, &o);
    CHECK_STR(http_headers_get(&h, "x-session-id"), "shared-key");
    http_headers_free(&h);
    o.prompt_cache_key = NULL;
    o.session_id = "0123456789012345678901234567890123456789012345678901234567890123456789";
    oai_apply_prompt_cache_session_header(&h, &t.m, &o);
    CHECK(str_starts(http_headers_get(&h, "x-session-id"), "pc_"));
    http_headers_free(&h);
    o.cache_retention = "none";
    oai_apply_prompt_cache_session_header(&h, &t.m, &o);
    CHECK(!http_headers_get(&h, "x-session-id"));
    http_headers_free(&h);
    json_free(t.raw);
}

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
    /* Cloud Code Assist quota formats (omp e482002caf, 6d091aba7f): "Resets in 71h48m47s", "quotaResetDelay" (with
     * millisecond units), and the absolute "quotaResetTimeStamp" */
    CHECK(ai_extract_retry_hint(NULL, "Individual quota reached. Resets in 71h48m47s.") == 258527000);
    CHECK(ai_extract_retry_hint(NULL, "{\"quotaResetDelay\": \"17h48m25.5s\"}") == 64105500);
    CHECK(ai_extract_retry_hint(NULL, "{\"quotaResetDelay\":\"835ms\"}") == 835);
    CHECK(ai_extract_retry_hint(NULL, "{\"quotaResetDelay\": \"5mx\"}") == -1);
    /* omp 3efa3bab9e: structured Go durations win over earlier display text; fractional ms floor */
    CHECK(ai_extract_retry_hint(NULL, "{\"metadata\":{\"quotaResetDelay\":\"835.150299ms\"}}") == 835);
    {
        static const struct { const char *d; int64_t ms; } gd[] = {{"30m", 1800000}, {"1h", 3600000}, {"1.5h", 5400000}, {"1h30m", 5400000}};
        for (size_t i = 0; i < sizeof gd / sizeof gd[0]; i++) {
            char body[300];
            snprintf(body, sizeof body,
                     "Cloud Code Assist API error (429): {\"error\":{\"status\":\"RESOURCE_EXHAUSTED\",\"message\":\"Resets in "
                     "3h0m0s.\",\"details\":[{\"metadata\":{\"quotaResetDelay\":\"%s\"}}]}}",
                     gd[i].d);
            CHECK(ai_extract_retry_hint(NULL, body) == gd[i].ms);
        }
    }
    {
        time_t future = time(NULL) + 7200;
        char stamp[40], body[120];
        strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", gmtime(&future));
        snprintf(body, sizeof body, "{\"quotaResetTimeStamp\": \"%s\"}", stamp);
        int64_t wait_ms = ai_extract_retry_hint(NULL, body);
        CHECK(wait_ms > 7100000 && wait_ms <= 7200000);
        stamp[19] = 0; /* naive (no Z): not this grammar */
        snprintf(body, sizeof body, "{\"quotaResetTimeStamp\": \"%s\"}", stamp);
        CHECK(ai_extract_retry_hint(NULL, body) == -1);
    }
    /* omp 6d091aba7f: QUOTA_RESET_PATTERN takes ms as well as s, and covers "resets in" and quotaResetDelay */
    CHECK(ai_extract_retry_hint(NULL, "{\"quotaResetDelay\": \"835ms\"}") == 835);
    CHECK(ai_extract_retry_hint(NULL, "{\"quotaResetDelay\": \"17h48m25.5s\"}") == 64105500);
    CHECK(ai_extract_retry_hint(NULL, "Resets in 71h48m47s") == 258527000);
    CHECK(ai_extract_retry_hint(NULL, "quota will reset after 10ms") == 10);
    CHECK(ai_extract_retry_hint(NULL, "reset after 2m5ms") == 120005);
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

/* omp f54745ac23: the Codex client version tracks npm through a record in the agent dir, never below the pin */
static void test_codex_client_version(void) {
    setenv("OMC_PROVIDER_URL_REWRITE", "https://registry.npmjs.org=http://127.0.0.1:1", 1); /* never the real registry */
    setenv("PI_AI_CODEX_CLIENT_VERSION", "9.8.7", 1);
    CHECK_STR(ai_codex_client_version(), "9.8.7");
    unsetenv("PI_AI_CODEX_CLIENT_VERSION");
    CHECK(ai_codex_semver_cmp("0.170.0", "0.169.0") > 0);
    CHECK(ai_codex_semver_cmp("0.99.0", "0.169.0") < 0);
    CHECK(!ai_codex_semver_valid("0.170.0-alpha.1"));
    CHECK(!ai_codex_semver_valid("1.2"));
    const char *dir = omc_agent_dir();
    fs_mkdir_p(dir, 0755);
    char *path = omc_agent_path("codex-client-version.json");
    struct {
        const char *version, *want;
    } cases[] = {{"0.200.1", "0.200.1"}, {"0.100.0", CODEX_CLIENT_VERSION_PIN}, {"0.300.0-beta", CODEX_CLIENT_VERSION_PIN}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *rec = xasprintf("{\"version\":\"%s\",\"checkedAt\":%lld}", cases[i].version, (long long)now_ms());
        fs_write(path, rec, strlen(rec));
        free(rec);
        ai_codex_client_version_reset_for_tests();
        CHECK_STR(ai_codex_client_version(), cases[i].want);
    }
    /* fixed for the life of the process */
    fs_write(path, "{\"version\":\"0.400.0\",\"checkedAt\":1}", 37);
    CHECK_STR(ai_codex_client_version(), CODEX_CLIENT_VERSION_PIN);
    unlink(path);
    free(path);
    ai_codex_client_version_reset_for_tests();
    setenv("PI_AI_CODEX_CLIENT_VERSION", CODEX_CLIENT_VERSION_PIN, 1);
}

/* omp 6c57f9091e: codex-rs routing fields lead the body, the rest keeps its order */
static void test_codex_routing_order(void) {
    json_t *b = json_parse_str("{\"input\":[1],\"instructions\":\"x\",\"stream\":true,\"tools\":[],\"service_tier\":\"priority\","
                               "\"model\":\"gpt-6-luna\",\"store\":false}");
    oair_codex_routing_fields_first(b);
    char *d = json_dump(b, 0);
    CHECK_STR(d, "{\"model\":\"gpt-6-luna\",\"stream\":true,\"service_tier\":\"priority\",\"input\":[1],\"instructions\":\"x\","
                 "\"tools\":[],\"store\":false}");
    free(d);
    json_free(b);
}

int main(void) {
    test_codex_routing_order();
    test_codex_client_version();
    test_prompt_cache_session_header();
    test_retry_hints();
    test_replay_safe();
    test_completions_request();
    test_completions_mistral();
    test_completions_dialects();
    test_completions_stream();
    test_reasoning_details();
    test_gemini_thought_signatures();
    test_split_tool_calls();
    test_completions_stream_errors();
    test_responses_request();
    test_tool_name_prefix();
    test_cut_tool_calls_replay();
    test_codex_azure();
    test_responses_stream();
    test_ollama();
    test_strict_schema();
    test_tool_strict_flags();
    test_responses_sanitizer();
    test_responses_tool_schemas();
    test_leaked_thinking();
    printf("test_providers_oai: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
