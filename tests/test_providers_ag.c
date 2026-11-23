/*
 * test_providers_ag.c - offline tests for the Anthropic and Google providers:
 * request shaping (bodies/headers/urls) and recorded SSE streams fed through the event handlers.
 */
#include "ai/anthropic_threads.h"
#include "ai/ai.h"
#include "ai/provider_anthropic.h"
#include "ai/provider_google.h"

#include <math.h>
#include <unistd.h>

/* catalog symbol referenced by the providers' network entry points (not exercised offline) */
char *ai_env_api_key(const char *provider) {
    (void)provider;
    return NULL;
}

static int g_fail, g_pass;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (c) g_pass++;                                                                           \
        else {                                                                                     \
            g_fail++;                                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                  \
        }                                                                                          \
    } while (0)
#define CHECK_STR(a, b)                                                                            \
    do {                                                                                           \
        const char *a_ = (a), *b_ = (b);                                                           \
        if (str_eq(a_, b_)) g_pass++;                                                              \
        else {                                                                                     \
            g_fail++;                                                                              \
            fprintf(stderr, "%s:%d: '%s' != '%s'\n", __FILE__, __LINE__, a_ ? a_ : "(null)",        \
                    b_ ? b_ : "(null)");                                                           \
        }                                                                                          \
    } while (0)
#define CHECK_INT(a, b)                                                                            \
    do {                                                                                           \
        long long a_ = (long long)(a), b_ = (long long)(b);                                        \
        if (a_ == b_) g_pass++;                                                                    \
        else {                                                                                     \
            g_fail++;                                                                              \
            fprintf(stderr, "%s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_);   \
        }                                                                                          \
    } while (0)

static json_t *J(const char *s) {
    char *err = NULL;
    json_t *j = json_parse(s, strlen(s), &err);
    if (!j) {
        fprintf(stderr, "bad test json: %s\n%s\n", err ? err : "?", s);
        exit(2);
    }
    free(err);
    return j;
}

static const char *P(const json_t *j, const char *path) { return json_sv(json_path(j, path)); }
static int64_t PI(const json_t *j, const char *path) {
    json_t *v = json_path(j, path);
    return json_is_num(v) ? (int64_t)v->u.num.d : -999999;
}

/* ai_model_t from a catalog-shaped object (raw stays owned by the caller) */
static ai_model_t mk_model(json_t *raw) {
    ai_model_t m = {0};
    m.raw = raw;
    m.id = json_get_str(raw, "id");
    m.name = m.id;
    m.api = json_get_str(raw, "api");
    m.provider = json_get_str(raw, "provider");
    m.base_url = json_get_str(raw, "baseUrl");
    m.reasoning = json_get_bool(raw, "reasoning", false);
    m.input_image = false;
    json_t *in;
    json_arr_foreach(json_get(raw, "input"), i, in) {
        if (str_eq(json_sv(in), "image")) m.input_image = true;
    }
    m.cost_input = json_get_num(json_path(raw, "cost"), "input", 0);
    m.cost_output = json_get_num(json_path(raw, "cost"), "output", 0);
    m.cost_cache_read = json_get_num(json_path(raw, "cost"), "cacheRead", 0);
    m.cost_cache_write = json_get_num(json_path(raw, "cost"), "cacheWrite", 0);
    m.context_window = json_get_int(raw, "contextWindow", 0);
    m.max_tokens = json_get_int(raw, "maxTokens", 0);
    m.compat = json_get_obj(raw, "compat");
    m.headers = json_get_obj(raw, "headers");
    m.thinking = json_get_obj(raw, "thinking");
    return m;
}

/* event recorder */
typedef struct {
    sb_t names;
    int n_delta;
} rec_t;
static void rec_cb(void *ud, const ai_event_t *ev) {
    rec_t *r = ud;
    if (ev->type == AI_EV_TEXT_DELTA || ev->type == AI_EV_THINKING_DELTA || ev->type == AI_EV_TOOLCALL_DELTA) {
        r->n_delta++;
        return;
    }
    if (r->names.len) sb_putc(&r->names, ',');
    sb_append(&r->names, ai_event_name(ev->type));
}

/* feed raw SSE text through the shared parser in small chunks; returns the last non-zero callback rc */
static int feed_sse(const char *text, sse_event_cb cb, void *ud) {
    sse_parser_t p;
    sse_init(&p, cb, ud);
    size_t n = strlen(text);
    int rc = 0;
    for (size_t i = 0; i < n && !rc; i += 7) rc = sse_feed(&p, text + i, MIN((size_t)7, n - i));
    if (!rc) rc = sse_finish(&p);
    sse_free(&p);
    return rc;
}

/* ====================================================================== anthropic */

#define SONNET45                                                                                                  \
    "{\"id\":\"claude-sonnet-4-5\",\"api\":\"anthropic-messages\",\"provider\":\"anthropic\","                   \
    "\"baseUrl\":\"https://api.anthropic.com\",\"reasoning\":true,\"input\":[\"text\",\"image\"],"                 \
    "\"cost\":{\"input\":3,\"output\":15,\"cacheRead\":0.3,\"cacheWrite\":3.75},\"contextWindow\":200000,"         \
    "\"maxTokens\":64000,\"thinking\":{\"mode\":\"budget\",\"efforts\":[\"minimal\",\"low\",\"medium\","            \
    "\"high\",\"xhigh\"]},\"compat\":{\"officialEndpoint\":true,\"signingEndpoint\":true,"                         \
    "\"supportsEagerToolInputStreaming\":true,\"supportsLongCacheRetention\":true,\"replayUnsignedThinking\":false}}"

#define OPUS48                                                                                                    \
    "{\"id\":\"claude-opus-4-8\",\"api\":\"anthropic-messages\",\"provider\":\"anthropic\","                     \
    "\"baseUrl\":\"https://api.anthropic.com\",\"reasoning\":true,\"input\":[\"text\",\"image\"],"                 \
    "\"cost\":{\"input\":5,\"output\":25},\"maxTokens\":128000,"                                                    \
    "\"thinking\":{\"mode\":\"anthropic-adaptive\",\"efforts\":[\"low\",\"medium\",\"high\",\"xhigh\",\"max\"],"   \
    "\"supportsDisplay\":true},\"compat\":{\"officialEndpoint\":true,\"signingEndpoint\":true,"                    \
    "\"supportsEagerToolInputStreaming\":true,\"supportsLongCacheRetention\":true,"                                \
    "\"supportsMidConversationSystem\":true}}"

static const char *CTX_MESSAGES =
    "["
    "{\"role\":\"user\",\"content\":\"List the files\",\"timestamp\":1},"
    "{\"role\":\"assistant\",\"api\":\"anthropic-messages\",\"provider\":\"anthropic\",\"model\":\"claude-sonnet-4-5\","
    " \"stopReason\":\"toolUse\",\"content\":["
    "  {\"type\":\"thinking\",\"thinking\":\"I should run ls\",\"thinkingSignature\":\"sigA==\"},"
    "  {\"type\":\"text\",\"text\":\"Running.\"},"
    "  {\"type\":\"toolCall\",\"id\":\"toolu_01\",\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}},"
    "  {\"type\":\"toolCall\",\"id\":\"call|weird.id\",\"name\":\"read\",\"arguments\":{\"path\":\"a.txt\"}}]},"
    "{\"role\":\"toolResult\",\"toolCallId\":\"toolu_01\",\"toolName\":\"bash\",\"isError\":false,"
    " \"content\":[{\"type\":\"text\",\"text\":\"a.txt\\nb.txt\"}]},"
    "{\"role\":\"toolResult\",\"toolCallId\":\"call|weird.id\",\"toolName\":\"read\",\"isError\":true,"
    " \"content\":[]},"
    "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"What is this?\"},"
    " {\"type\":\"image\",\"data\":\"AAAA\",\"mimeType\":\"image/jpg\"}]}"
    "]";

static const char *CTX_TOOLS =
    "[{\"name\":\"bash\",\"description\":\"Run a command\",\"parameters\":{\"type\":\"object\","
    "  \"properties\":{\"command\":{\"type\":\"string\",\"pattern\":\"^[a-z]+\"},"
    "  \"email\":{\"type\":\"string\",\"format\":\"email\"},\"when\":{\"type\":\"string\",\"format\":\"cron\"},"
    "  \"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":3}},"
    "  \"required\":[\"command\"]}},"
    " {\"name\":\"read\",\"description\":\"Read a file\",\"parameters\":{\"type\":\"object\","
    "  \"properties\":{\"path\":{\"type\":\"string\"}}}}]";

static void test_anthropic_request_apikey(void) {
    json_t *raw = J(SONNET45);
    ai_model_t m = mk_model(raw);
    json_t *msgs = J(CTX_MESSAGES), *tools = J(CTX_TOOLS), *sys = J("[\"You are omc.\", \"  \"]");
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_MEDIUM;
    o.temperature = 0.5;
    json_t *meta = J("{\"user_id\":\"u-123\"}");
    o.metadata = meta;
    json_t *p = anthropic_build_request(&m, &ctx, &o, false);

    CHECK_STR(P(p, "model"), "claude-sonnet-4-5");
    CHECK_INT(PI(p, "max_tokens"), 64000);
    CHECK_STR(P(p, "thinking.type"), "enabled");
    CHECK_INT(PI(p, "thinking.budget_tokens"), 8192);
    CHECK_STR(P(p, "thinking.display"), "summarized");
    CHECK(json_get(p, "temperature") == NULL); /* sampling params only with thinking off */
    CHECK_STR(P(p, "context_management.edits.0.type"), "clear_thinking_20251015");
    CHECK(json_get(p, "output_config") == NULL); /* budget mode: no effort */
    CHECK_STR(P(p, "metadata.user_id"), "u-123");
    CHECK(json_get_bool(p, "stream", false));
    /* system: one block (blank dropped), carries the head breakpoint */
    CHECK_INT(json_len(json_get(p, "system")), 1);
    CHECK_STR(P(p, "system.0.text"), "You are omc.");
    CHECK_STR(P(p, "system.0.cache_control.type"), "ephemeral");
    CHECK(json_path(p, "system.0.cache_control.ttl") == NULL); /* short retention for API keys */
    /* tools */
    CHECK_STR(P(p, "tools.0.name"), "bash");
    CHECK(json_get_bool(json_path(p, "tools.0"), "eager_input_streaming", false));
    json_t *is = json_path(p, "tools.0.input_schema");
    CHECK(json_is_bool(json_get(is, "additionalProperties")) && !json_get(is, "additionalProperties")->u.b);
    CHECK_STR(P(is, "properties.command.description"), "{pattern: \"^[a-z]+\"}");
    CHECK_STR(P(is, "properties.email.format"), "email");
    CHECK(json_path(is, "properties.when.format") == NULL);
    CHECK_STR(P(is, "properties.when.description"), "{format: \"cron\"}");
    CHECK(json_path(is, "properties.tags.minItems") == NULL);
    CHECK_STR(P(p, "tools.1.cache_control.type"), "ephemeral");
    CHECK(json_path(p, "tools.0.cache_control") == NULL);
    CHECK(json_is_arr(json_path(p, "tools.1.input_schema.required")));
    /* messages */
    json_t *wm = json_get(p, "messages");
    CHECK_INT(json_len(wm), 4);
    CHECK_STR(P(wm, "0.role"), "user");
    CHECK_STR(P(wm, "0.content"), "List the files");
    CHECK_STR(P(wm, "1.role"), "assistant");
    CHECK_STR(P(wm, "1.content.0.type"), "thinking");
    CHECK_STR(P(wm, "1.content.0.signature"), "sigA==");
    CHECK_STR(P(wm, "1.content.1.type"), "text");
    CHECK_STR(P(wm, "1.content.2.type"), "tool_use");
    CHECK_STR(P(wm, "1.content.2.id"), "toolu_01");
    CHECK_STR(P(wm, "1.content.2.input.command"), "ls");
    CHECK_STR(P(wm, "1.content.3.id"), "call_weird_id");
    /* consecutive tool results merged into one user message */
    CHECK_STR(P(wm, "2.role"), "user");
    CHECK_INT(json_len(json_path(wm, "2.content")), 2);
    CHECK_STR(P(wm, "2.content.0.type"), "tool_result");
    CHECK_STR(P(wm, "2.content.0.tool_use_id"), "toolu_01");
    CHECK_STR(P(wm, "2.content.0.content.0.text"), "a.txt\nb.txt");
    CHECK_STR(P(wm, "2.content.1.tool_use_id"), "call_weird_id");
    CHECK_STR(P(wm, "2.content.1.content"), "Tool failed with no output.");
    CHECK(json_get_bool(json_path(wm, "2.content.1"), "is_error", false));
    /* image normalized jpg -> jpeg; last user block gets the rolling breakpoint */
    CHECK_STR(P(wm, "3.content.1.type"), "image");
    CHECK_STR(P(wm, "3.content.1.source.type"), "base64");
    CHECK_STR(P(wm, "3.content.1.source.media_type"), "image/jpeg");
    CHECK_STR(P(wm, "3.content.1.cache_control.type"), "ephemeral");
    /* second trailing candidate (tool results) also cached */
    CHECK_STR(P(wm, "2.content.1.cache_control.type"), "ephemeral");
    /* no private keys leak */
    char *dump = json_dump(p, 0);
    CHECK(strstr(dump, "omc_conv") == NULL && strstr(dump, "omc_dev") == NULL);
    free(dump);

    /* headers */
    http_headers_t h = {0};
    anthropic_build_headers(&m, &o, &ctx, p, "sk-ant-api03-key", false, &h);
    CHECK_STR(http_headers_get(&h, "X-Api-Key"), "sk-ant-api03-key");
    CHECK(http_headers_get(&h, "Authorization") == NULL);
    CHECK_STR(http_headers_get(&h, "anthropic-version"), "2023-06-01");
    CHECK_STR(http_headers_get(&h, "Accept"), "text/event-stream");
    const char *beta = http_headers_get(&h, "anthropic-beta");
    CHECK(beta && strstr(beta, "effort-2025-11-24"));
    CHECK(beta && strstr(beta, "fallback-credit-2026-06-01"));
    CHECK(beta && strstr(beta, "interleaved-thinking-2025-05-14"));
    CHECK(beta && strstr(beta, "context-management-2025-06-27"));
    CHECK(beta && !strstr(beta, "fine-grained-tool-streaming")); /* eager per-tool streaming instead */
    CHECK(beta && !strstr(beta, "oauth-2025-04-20"));
    http_headers_free(&h);
    char *url = anthropic_request_url(&m, false);
    CHECK_STR(url, "https://api.anthropic.com/v1/messages");
    free(url);

    json_free(p);
    json_free(meta);
    json_free(sys);
    json_free(tools);
    json_free(msgs);
    json_free(raw);
}

static void test_anthropic_request_adaptive(void) {
    json_t *raw = J(OPUS48);
    ai_model_t m = mk_model(raw);
    json_t *msgs = J("[{\"role\":\"user\",\"content\":\"hi\"},"
                     "{\"role\":\"developer\",\"content\":\"Be brief.\"},"
                     "{\"role\":\"assistant\",\"api\":\"anthropic-messages\",\"provider\":\"anthropic\","
                     " \"model\":\"claude-opus-4-8\",\"stopReason\":\"stop\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]},"
                     "{\"role\":\"assistant\",\"api\":\"anthropic-messages\",\"provider\":\"anthropic\","
                     " \"model\":\"claude-opus-4-8\",\"stopReason\":\"stop\",\"content\":[{\"type\":\"text\",\"text\":\"more\"}]}]");
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_HIGH;
    o.cache_retention = "long";
    json_t *p = anthropic_build_request(&m, &ctx, &o, false);
    CHECK_STR(P(p, "thinking.type"), "adaptive");
    CHECK_STR(P(p, "thinking.display"), "summarized");
    CHECK_STR(P(p, "output_config.effort"), "high");
    CHECK_INT(PI(p, "max_tokens"), 128000);
    CHECK(json_get(p, "system") == NULL);
    json_t *wm = json_get(p, "messages");
    /* developer upgraded to system; adjacent assistants repaired; trailing assistant padded */
    CHECK_STR(P(wm, "1.role"), "system");
    CHECK_STR(P(wm, "2.role"), "assistant");
    CHECK_STR(P(wm, "3.role"), "user");
    CHECK_STR(P(wm, "3.content.0.text"), "Continue."); /* interior pad is the 2nd rolling breakpoint */
    CHECK_STR(P(wm, "4.role"), "assistant");
    CHECK_STR(P(wm, "5.content"), "Continue.");
    /* rolling breakpoint anchors on the last real assistant (trailing pad skipped), 1h ttl */
    CHECK_STR(P(wm, "4.content.0.cache_control.ttl"), "1h");
    http_headers_t h = {0};
    anthropic_build_headers(&m, &o, &ctx, p, "k", false, &h);
    const char *beta = http_headers_get(&h, "anthropic-beta");
    CHECK(beta && strstr(beta, "extended-cache-ttl-2025-04-11"));
    CHECK(beta && !strstr(beta, "interleaved-thinking")); /* supportsDisplay models: native interleaving */
    http_headers_free(&h);
    json_free(p);

    /* thinking off on an adaptive-only model: omit thinking, pin low effort, sampling allowed */
    o.reasoning = EFFORT_OFF;
    o.temperature = 0.2;
    o.cache_retention = "none";
    p = anthropic_build_request(&m, &ctx, &o, false);
    CHECK(json_get(p, "thinking") == NULL);
    CHECK_STR(P(p, "output_config.effort"), "low");
    CHECK(json_is_num(json_get(p, "temperature")));
    CHECK(json_get(p, "context_management") == NULL);
    char *dump = json_dump(p, 0);
    CHECK(strstr(dump, "cache_control") == NULL);
    free(dump);
    json_free(p);
    /* a route's (or models.yml's) Off dialect wins: `disabled` is explicit, `omit` sends neither field */
    json_t *mc = json_get(raw, "compat");
    json_set_str(mc, "disabledThinking", "disabled");
    p = anthropic_build_request(&m, &ctx, &o, false);
    CHECK_STR(P(p, "thinking.type"), "disabled");
    CHECK(json_get(p, "output_config") == NULL);
    json_free(p);
    json_set_str(mc, "disabledThinking", "omit");
    p = anthropic_build_request(&m, &ctx, &o, false);
    CHECK(json_get(p, "thinking") == NULL && json_get(p, "output_config") == NULL);
    json_free(p);
    json_del(mc, "disabledThinking");

    /* forced tool choice drops thinking */
    json_t *tools = J(CTX_TOOLS), *tc = json_str("required");
    ctx.tools = tools;
    o.reasoning = EFFORT_HIGH;
    o.tool_choice = tc;
    p = anthropic_build_request(&m, &ctx, &o, false);
    CHECK_STR(P(p, "tool_choice.type"), "any");
    CHECK(json_get(p, "thinking") == NULL);
    CHECK_STR(P(p, "output_config.effort"), "low");
    json_free(p);
    json_free(tc);
    tc = J("{\"type\":\"tool\",\"name\":\"bash\"}");
    o.tool_choice = tc;
    p = anthropic_build_request(&m, &ctx, &o, true);
    CHECK_STR(P(p, "tool_choice.type"), "tool");
    CHECK_STR(P(p, "tool_choice.name"), "_bash");
    json_free(p);
    json_free(tc);
    json_free(tools);
    json_free(msgs);
    json_free(raw);
}

static void test_anthropic_request_oauth(void) {
    json_t *raw = J(SONNET45);
    ai_model_t m = mk_model(raw);
    json_t *msgs = J("[{\"role\":\"user\",\"content\":\"Hello there, Claude Code friend!\"}]");
    json_t *tools = J(CTX_TOOLS), *sys = J("[\"Project prompt\", \"<project-context>cwd</project-context>\"]");
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.session_id = "11111111-2222-4333-8444-555555555555";
    const char *tok = "sk-ant-oat01-abc";
    CHECK(anthropic_is_oauth_token(tok));
    CHECK(!anthropic_is_oauth_token("sk-ant-api03-x"));
    json_t *p = anthropic_build_request(&m, &ctx, &o, true);
    const char *b0 = P(p, "system.0.text");
    char *want = xasprintf("x-anthropic-billing-header: cc_version=%s.", anthropic_claude_code_version());
    CHECK(b0 && str_starts(b0, want));
    CHECK(b0 && strstr(b0, "cc_entrypoint=cli; cch=00000;"));
    free(want);
    CHECK_STR(P(p, "system.1.text"), "You are Claude Code, Anthropic's official CLI for Claude.");
    /* OAuth defaults to 1h retention; head breakpoint moves to the last stable block before the volatile suffix */
    CHECK(json_path(p, "system.1.cache_control") == NULL);
    CHECK_STR(P(p, "system.2.cache_control.ttl"), "1h");
    CHECK(json_path(p, "system.3.cache_control") == NULL);
    CHECK_STR(P(p, "tools.0.name"), "_bash");
    const char *uid = P(p, "metadata.user_id");
    json_t *u = uid ? json_parse_str(uid) : NULL;
    CHECK_STR(json_get_str(u, "session_id"), "11111111-2222-4333-8444-555555555555");
    CHECK(json_get_str(u, "device_id") && strlen(json_get_str(u, "device_id")) == 64);
    json_free(u);
    char *body = anthropic_serialize_body(p);
    CHECK(strstr(body, "x-anthropic-billing-header:") != NULL);
    CHECK(strstr(body, "cch=00000") == NULL); /* attestation patched */
    free(body);
    http_headers_t h = {0};
    anthropic_build_headers(&m, &o, &ctx, p, tok, true, &h);
    CHECK_STR(http_headers_get(&h, "Authorization"), "Bearer sk-ant-oat01-abc");
    CHECK(http_headers_get(&h, "X-Api-Key") == NULL);
    CHECK_STR(http_headers_get(&h, "x-app"), "cli");
    CHECK_STR(http_headers_get(&h, "X-Stainless-Lang"), "js");
    CHECK_STR(http_headers_get(&h, "X-Claude-Code-Session-Id"), "11111111-2222-4333-8444-555555555555");
    const char *ua = http_headers_get(&h, "User-Agent");
    CHECK(ua && str_starts(ua, "claude-cli/"));
    const char *beta = http_headers_get(&h, "anthropic-beta");
    CHECK(beta && str_starts(beta, "claude-code-20250219,oauth-2025-04-20,"));
    CHECK(beta && strstr(beta, "fallback-credit-2026-06-01"));
    http_headers_free(&h);
    char *url = anthropic_request_url(&m, true);
    CHECK_STR(url, "https://api.anthropic.com/v1/messages?beta=true");
    free(url);
    json_free(p);
    json_free(sys);
    json_free(tools);
    json_free(msgs);
    json_free(raw);
}

static void test_anthropic_proxy_headers(void) {
    json_t *raw = J("{\"id\":\"glm-5\",\"api\":\"anthropic-messages\",\"provider\":\"zai\","
                    "\"baseUrl\":\"https://api.z.ai/api/anthropic/v1/\",\"reasoning\":false,\"maxTokens\":32000}");
    ai_model_t m = mk_model(raw);
    json_t *msgs = J("[{\"role\":\"user\",\"content\":\"x\"}]");
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.max_tokens = 1000;
    o.temperature = 0.7;
    json_t *p = anthropic_build_request(&m, &ctx, &o, false);
    CHECK_INT(PI(p, "max_tokens"), 1000);
    CHECK(json_is_num(json_get(p, "temperature")));
    CHECK(json_get(p, "thinking") == NULL);
    http_headers_t h = {0};
    anthropic_build_headers(&m, &o, &ctx, p, "zkey", false, &h);
    CHECK_STR(http_headers_get(&h, "Authorization"), "Bearer zkey");
    CHECK(http_headers_get(&h, "X-Api-Key") == NULL);
    const char *beta = http_headers_get(&h, "anthropic-beta");
    CHECK(!beta || !strstr(beta, "fallback-credit"));
    http_headers_free(&h);
    char *url = anthropic_request_url(&m, false);
    CHECK_STR(url, "https://api.z.ai/api/anthropic/v1/messages");
    free(url);
    json_free(p);
    json_free(msgs);
    json_free(raw);
}

static void test_anthropic_schema(void) {
    json_t *s = J("{\"type\":\"object\",\"oneOf\":[{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"string\"}},"
                  "\"required\":[\"a\"]},{\"type\":\"object\",\"properties\":{\"b\":{\"type\":\"number\"}},"
                  "\"required\":[\"b\"]}],\"properties\":{\"m\":{\"type\":\"object\",\"additionalProperties\":{}}}}");
    json_t *n = anthropic_normalize_tool_schema(s);
    CHECK(json_path(n, "properties.a") != NULL);
    CHECK(json_path(n, "properties.b") != NULL);
    CHECK(json_get(n, "oneOf") == NULL);
    CHECK(P(n, "description") && str_starts(P(n, "description"), "{oneOf: "));
    json_t *ap = json_path(n, "properties.m.additionalProperties");
    CHECK(json_is_bool(ap) && ap->u.b);
    json_free(n);
    json_free(s);
}

static const char *ANTH_SSE =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_01ABC\",\"type\":\"message\",\"role\":\"assistant\","
    "\"content\":[],\"model\":\"claude-sonnet-4-5\",\"usage\":{\"input_tokens\":10,\"cache_read_input_tokens\":5,"
    "\"cache_creation_input_tokens\":3,\"output_tokens\":1,\"cache_creation\":{\"ephemeral_5m_input_tokens\":0,"
    "\"ephemeral_1h_input_tokens\":3}}}}\n\n"
    "event: ping\ndata: {\"type\":\"ping\"}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Let me \"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"check.\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"EqQBCgIY\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"Ag==\"}}\n\n"
    "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"ENCRYPTED\"}}\n\n"
    "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"text_delta\",\"text\":\"I'll list \"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"text_delta\",\"text\":\"the files.\"}}\n\n"
    "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":2}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":3,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01XYZ\",\"name\":\"_bash\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":3,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":3,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"command\\\": \\\"ls\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":3,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\" -la\\\", \\\"n\\\": 2}\"}}\n\n"
    "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":3}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":42}}\n\n"
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

static void test_anthropic_sse(void) {
    json_t *raw = J(SONNET45);
    ai_model_t m = mk_model(raw);
    rec_t rec = {SB_INIT, 0};
    anthropic_sse_t s;
    anthropic_sse_init(&s, &m, true, rec_cb, &rec);
    int rc = feed_sse(ANTH_SSE, anthropic_sse_event, &s);
    CHECK_INT(rc, 1);
    json_t *msg = anthropic_sse_finish(&s, 0, NULL);
    anthropic_sse_free(&s);
    CHECK_STR(json_get_str(msg, "stopReason"), "toolUse");
    CHECK_STR(json_get_str(msg, "responseId"), "msg_01ABC");
    CHECK(json_get(msg, "errorMessage") == NULL);
    json_t *c = json_get(msg, "content");
    CHECK_INT(json_len(c), 4);
    CHECK_STR(P(c, "0.type"), "thinking");
    CHECK_STR(P(c, "0.thinking"), "Let me check.");
    CHECK_STR(P(c, "0.thinkingSignature"), "EqQBCgIYAg==");
    CHECK_STR(P(c, "1.type"), "redactedThinking");
    CHECK_STR(P(c, "1.data"), "ENCRYPTED");
    CHECK_STR(P(c, "2.text"), "I'll list the files.");
    CHECK_STR(P(c, "3.type"), "toolCall");
    CHECK_STR(P(c, "3.id"), "toolu_01XYZ");
    CHECK_STR(P(c, "3.name"), "bash"); /* OAuth wire prefix stripped */
    CHECK_STR(P(c, "3.arguments.command"), "ls -la");
    CHECK_INT(PI(c, "3.arguments.n"), 2);
    json_t *u = json_get(msg, "usage");
    CHECK_INT(json_get_int(u, "input", -1), 10);
    CHECK_INT(json_get_int(u, "output", -1), 42);
    CHECK_INT(json_get_int(u, "cacheRead", -1), 5);
    CHECK_INT(json_get_int(u, "cacheWrite", -1), 3);
    CHECK_INT(json_get_int(u, "totalTokens", -1), 60);
    CHECK_INT(PI(u, "cttl.ephemeral1h"), 3);
    double cost = json_get_num(json_get(u, "cost"), "total", 0);
    CHECK(fabs(cost - (10 * 3 + 42 * 15 + 5 * 0.3 + 3 * 3.75) / 1e6) < 1e-12);
    CHECK_STR(sb_cstr(&rec.names), "start,thinking_start,thinking_end,text_start,text_end,toolcall_start,toolcall_end,done");
    CHECK(rec.n_delta >= 5);
    sb_free(&rec.names);
    json_free(msg);
    json_free(raw);
}

static json_t *run_anth(const ai_model_t *m, const char *sse, int *rc_out) {
    anthropic_sse_t s;
    anthropic_sse_init(&s, m, false, NULL, NULL);
    int rc = feed_sse(sse, anthropic_sse_event, &s);
    if (rc_out) *rc_out = rc;
    json_t *msg = anthropic_sse_finish(&s, rc == 2 ? 2 : 0, NULL);
    anthropic_sse_free(&s);
    return msg;
}

#define MSG_START "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"usage\":{\"input_tokens\":1,\"output_tokens\":0}}}\n\n"
#define TEXT_BLOCK(t)                                                                                              \
    "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n" \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" t "\"}}\n\n"

static void test_anthropic_sse_edges(void) {
    json_t *raw = J(SONNET45);
    ai_model_t m = mk_model(raw);
    int rc;
    /* in-stream error frame */
    json_t *msg = run_anth(&m, MSG_START TEXT_BLOCK("partial")
                           "event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n",
                           &rc);
    CHECK_INT(rc, 2);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    CHECK_STR(json_get_str(msg, "errorMessage"), "Anthropic stream error (overloaded_error): Overloaded");
    CHECK_STR(P(msg, "content.0.text"), "partial");
    json_free(msg);
    /* refusal with stop_details */
    msg = run_anth(&m, MSG_START
                   "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"refusal\","
                   "\"stop_details\":{\"type\":\"refusal\",\"category\":\"cyber\",\"explanation\":\" nope \"}},\"usage\":{\"output_tokens\":3}}\n\n"
                   "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
                   NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    CHECK_STR(json_get_str(msg, "errorMessage"), "Refusal (cyber): nope");
    json_free(msg);
    /* truncated stream */
    msg = run_anth(&m, MSG_START TEXT_BLOCK("cut"), NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    CHECK_STR(json_get_str(msg, "errorMessage"), "Anthropic stream ended before message_stop");
    json_free(msg);
    /* max_tokens; message_stop missing but stop_reason arrived */
    msg = run_anth(&m, MSG_START TEXT_BLOCK("long")
                   "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"},\"usage\":{\"output_tokens\":9}}\n\n",
                   NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "length");
    CHECK_INT(json_get_int(json_get(msg, "usage"), "output", -1), 9);
    json_free(msg);
    /* signature-only thinking (display omitted) */
    msg = run_anth(&m, MSG_START
                   "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
                   "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"SIG\"}}\n\n"
                   "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                   "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                   "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
                   "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
                   "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
                   "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
                   NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "stop");
    CHECK_STR(P(msg, "content.0.type"), "thinking");
    CHECK_STR(P(msg, "content.0.thinkingSignature"), "SIG");
    CHECK_STR(P(msg, "content.1.text"), "hi");
    json_free(msg);
    /* transport failure / abort */
    anthropic_sse_t s;
    anthropic_sse_init(&s, &m, false, NULL, NULL);
    msg = anthropic_sse_finish(&s, -1, "401 invalid x-api-key");
    anthropic_sse_free(&s);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    CHECK_STR(json_get_str(msg, "errorMessage"), "401 invalid x-api-key");
    json_free(msg);
    anthropic_sse_init(&s, &m, false, NULL, NULL);
    msg = anthropic_sse_finish(&s, -2, "Request was aborted");
    anthropic_sse_free(&s);
    CHECK_STR(json_get_str(msg, "stopReason"), "aborted");
    json_free(msg);
    CHECK_STR(anthropic_map_stop_reason("stop_sequence"), "stop");
    CHECK_STR(anthropic_map_stop_reason("pause_turn"), "stop");
    CHECK_STR(anthropic_map_stop_reason("model_context_window_exceeded"), "length");
    json_free(raw);
}

/* ====================================================================== google */

#define FLASH25                                                                                                   \
    "{\"id\":\"gemini-2.5-flash\",\"api\":\"google-generative-ai\",\"provider\":\"google\","                     \
    "\"baseUrl\":\"https://generativelanguage.googleapis.com/v1beta\",\"reasoning\":true,"                         \
    "\"input\":[\"text\",\"image\"],\"cost\":{\"input\":0.3,\"output\":2.5,\"cacheRead\":0.03},"                    \
    "\"maxTokens\":65536,\"thinking\":{\"mode\":\"budget\",\"efforts\":[\"minimal\",\"low\",\"medium\",\"high\"],"  \
    "\"effortBudgets\":{\"minimal\":128,\"low\":2048,\"medium\":8192,\"high\":24576}},\"compat\":{}}"

#define GEMINI3                                                                                                   \
    "{\"id\":\"gemini-3-pro-preview\",\"api\":\"google-generative-ai\",\"provider\":\"google\","                 \
    "\"reasoning\":true,\"input\":[\"text\"],\"maxTokens\":65536,"                                                 \
    "\"thinking\":{\"mode\":\"google-level\",\"efforts\":[\"low\",\"high\"],\"requiresEffort\":true},"             \
    "\"compat\":{\"supportsFunctionPartId\":true,\"requiresSkipThoughtSignature\":true,"                          \
    "\"multimodalFunctionResponse\":true}}"

static void test_google_request(void) {
    json_t *raw = J(FLASH25);
    ai_model_t m = mk_model(raw);
    json_t *msgs = J(
        "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"look\"},{\"type\":\"image\",\"data\":\"QUJD\",\"mimeType\":\"image/png\"}]},"
        "{\"role\":\"assistant\",\"api\":\"google-generative-ai\",\"provider\":\"google\",\"model\":\"gemini-2.5-flash\","
        " \"stopReason\":\"toolUse\",\"content\":["
        "  {\"type\":\"thinking\",\"thinking\":\"hmm\",\"thinkingSignature\":\"c2lnMQ==\"},"
        "  {\"type\":\"toolCall\",\"id\":\"c1\",\"name\":\"read\",\"arguments\":{\"path\":\"a\"},\"thoughtSignature\":\"c2lnMg==\"},"
        "  {\"type\":\"toolCall\",\"id\":\"c2\",\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}]},"
        "{\"role\":\"toolResult\",\"toolCallId\":\"c2\",\"toolName\":\"bash\",\"content\":[{\"type\":\"text\",\"text\":\"out2\"}]},"
        "{\"role\":\"toolResult\",\"toolCallId\":\"c1\",\"toolName\":\"read\",\"isError\":true,"
        " \"content\":[{\"type\":\"text\",\"text\":\"err1\"},{\"type\":\"image\",\"data\":\"WFla\",\"mimeType\":\"image/png\"}]}]");
    json_t *tools = J("[{\"name\":\"bash\",\"description\":\"Run\",\"parameters\":{\"$schema\":\"x\",\"type\":\"object\","
                      "\"additionalProperties\":false,\"properties\":{\"command\":{\"type\":\"string\",\"pattern\":\"^x\"},"
                      "\"cwd\":{\"type\":[\"string\",\"null\"]},\"mode\":{\"const\":\"fast\"},"
                      "\"val\":{\"type\":[\"string\",\"array\"],\"items\":{\"type\":\"string\"}},"
                      "\"ref\":{\"$ref\":\"#/$defs/R\"}},\"required\":[\"command\"],"
                      "\"$defs\":{\"R\":{\"type\":\"integer\",\"minimum\":1}}}}]");
    json_t *sys = J("[\"sys A\",\"sys B\"]");
    json_t *tc = json_str("required");
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_MEDIUM;
    o.tool_choice = tc;
    o.temperature = 1.0;
    json_t *b = google_build_request(&m, &ctx, &o);

    CHECK_STR(P(b, "systemInstruction.parts.1.text"), "sys B");
    CHECK_STR(P(b, "generationConfig.thinkingConfig.includeThoughts") ? NULL : "ok", "ok");
    CHECK(json_get_bool(json_path(b, "generationConfig.thinkingConfig"), "includeThoughts", false));
    CHECK_INT(PI(b, "generationConfig.thinkingConfig.thinkingBudget"), 8192);
    CHECK_INT(PI(b, "generationConfig.maxOutputTokens"), 65536);
    CHECK(json_is_num(json_path(b, "generationConfig.temperature")));
    CHECK_STR(P(b, "toolConfig.functionCallingConfig.mode"), "ANY");
    /* schema */
    json_t *s = json_path(b, "tools.0.functionDeclarations.0.parametersJsonSchema");
    CHECK(s != NULL);
    CHECK(json_get(s, "additionalProperties") == NULL);
    CHECK(json_get(s, "$schema") == NULL && json_get(s, "$defs") == NULL);
    CHECK_STR(P(s, "properties.command.description"), "{pattern: \"^x\"}");
    CHECK_STR(P(s, "properties.cwd.type"), "string");
    CHECK(json_get_bool(json_path(s, "properties.cwd"), "nullable", false));
    CHECK_STR(P(s, "properties.mode.enum.0"), "fast");
    CHECK_STR(P(s, "properties.mode.type"), "string");
    CHECK_STR(P(s, "properties.val.anyOf.0.type"), "string");
    CHECK_STR(P(s, "properties.val.anyOf.1.type"), "array");
    CHECK(json_path(s, "properties.val.anyOf.1.items") != NULL);
    CHECK_STR(P(s, "properties.ref.type"), "integer");
    CHECK_STR(P(s, "properties.ref.description"), "{minimum: 1}");
    CHECK_STR(P(s, "propertyOrdering.0"), "command");
    /* contents */
    json_t *c = json_get(b, "contents");
    CHECK_STR(P(c, "0.role"), "user");
    CHECK_STR(P(c, "0.parts.1.inlineData.mimeType"), "image/png");
    CHECK_STR(P(c, "1.role"), "model");
    CHECK(json_get_bool(json_path(c, "1.parts.0"), "thought", false));
    CHECK_STR(P(c, "1.parts.0.thoughtSignature"), "c2lnMQ==");
    CHECK_STR(P(c, "1.parts.1.functionCall.name"), "read");
    CHECK_STR(P(c, "1.parts.1.thoughtSignature"), "c2lnMg==");
    CHECK(json_path(c, "1.parts.1.functionCall.id") == NULL);
    CHECK(json_path(c, "1.parts.2.thoughtSignature") == NULL);
    /* responses merged into one turn and restored to call order */
    CHECK_STR(P(c, "2.role"), "user");
    CHECK_INT(json_len(json_path(c, "2.parts")), 2);
    CHECK_STR(P(c, "2.parts.0.functionResponse.name"), "read");
    CHECK_STR(P(c, "2.parts.0.functionResponse.response.error"), "err1");
    CHECK_STR(P(c, "2.parts.1.functionResponse.response.output"), "out2");
    /* Gemini < 3: tool images in a separate user turn */
    CHECK_STR(P(c, "3.parts.0.text"), "Tool result image:");
    CHECK_STR(P(c, "3.parts.1.inlineData.data"), "WFla");
    char *dump = json_dump(b, 0);
    CHECK(strstr(dump, "omc_ord") == NULL);
    free(dump);
    char *url = google_request_url(&m);
    CHECK_STR(url, "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse");
    free(url);
    json_free(b);

    /* thinking off on a budget model: explicit zero budget */
    o.reasoning = EFFORT_OFF;
    b = google_build_request(&m, &ctx, &o);
    CHECK_INT(PI(b, "generationConfig.thinkingConfig.thinkingBudget"), 0);
    CHECK(!json_get_bool(json_path(b, "generationConfig.thinkingConfig"), "includeThoughts", true));
    json_free(b);
    json_free(tc);
    json_free(sys);
    json_free(tools);
    json_free(msgs);
    json_free(raw);
}

static void test_google_request_gemini3(void) {
    json_t *raw = J(GEMINI3);
    ai_model_t m = mk_model(raw);
    json_t *msgs = J("[{\"role\":\"user\",\"content\":\"hi\"},"
                     "{\"role\":\"assistant\",\"api\":\"openai-responses\",\"provider\":\"openai\",\"model\":\"gpt\","
                     " \"stopReason\":\"toolUse\",\"content\":[{\"type\":\"toolCall\",\"id\":\"x1\",\"name\":\"bash\",\"arguments\":{}}]},"
                     "{\"role\":\"toolResult\",\"toolCallId\":\"x1\",\"toolName\":\"bash\",\"content\":[{\"type\":\"image\",\"data\":\"AA\",\"mimeType\":\"image/png\"}]}]");
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_OFF; /* requiresEffort: clamps to the lowest supported effort */
    o.hide_thinking_summary = true;
    json_t *b = google_build_request(&m, &ctx, &o);
    CHECK_STR(P(b, "generationConfig.thinkingConfig.thinkingLevel"), "LOW");
    CHECK(!json_get_bool(json_path(b, "generationConfig.thinkingConfig"), "includeThoughts", true));
    CHECK_INT(PI(b, "generationConfig.maxOutputTokens"), 65536);
    json_t *c = json_get(b, "contents");
    CHECK_STR(P(c, "1.parts.0.thoughtSignature"), "skip_thought_signature_validator");
    CHECK_STR(P(c, "1.parts.0.functionCall.id"), "x1");
    CHECK_STR(P(c, "2.parts.0.functionResponse.id"), "x1");
    /* ai_transform_messages already swaps images for its own note on non-vision models */
    CHECK_STR(P(c, "2.parts.0.functionResponse.response.output"), "[image omitted: model does not support vision]");
    CHECK(json_get(b, "tools") == NULL && json_get(b, "toolConfig") == NULL);
    json_free(b);
    /* base64SkipThoughtSignature: the sentinel goes out base64-encoded for gateways that decode it as bytes */
    json_set_bool(json_get(raw, "compat"), "base64SkipThoughtSignature", true);
    b = google_build_request(&m, &ctx, &o);
    CHECK_STR(P(json_get(b, "contents"), "1.parts.0.thoughtSignature"), "c2tpcF90aG91Z2h0X3NpZ25hdHVyZV92YWxpZGF0b3I=");
    json_free(b);
    json_del(json_get(raw, "compat"), "base64SkipThoughtSignature");
    o.reasoning = EFFORT_MEDIUM; /* unsupported -> clamps to low */
    o.hide_thinking_summary = false;
    b = google_build_request(&m, &ctx, &o);
    CHECK_STR(P(b, "generationConfig.thinkingConfig.thinkingLevel"), "LOW");
    CHECK(json_get_bool(json_path(b, "generationConfig.thinkingConfig"), "includeThoughts", false));
    json_free(b);
    json_free(msgs);
    json_free(raw);
}

static const char *GOOGLE_SSE =
    "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Thinking about \",\"thought\":true}]}}],"
    "\"usageMetadata\":{\"promptTokenCount\":100}}\r\n\r\n"
    "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"it.\",\"thought\":true,\"thoughtSignature\":\"VFNJRw==\"}]}}]}\r\n\r\n"
    "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Hello \"}]}}]}\r\n\r\n"
    "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"world\"},{\"text\":\"\",\"thoughtSignature\":\"VFhU\"}]}}]}\r\n\r\n"
    "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"functionCall\":{\"name\":\"bash\",\"args\":{\"command\":\"ls\"}},"
    "\"thoughtSignature\":\"RkM=\"},{\"functionCall\":{\"name\":\"bash\",\"args\":{\"command\":\"pwd\"}}}]},"
    "\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":100,\"candidatesTokenCount\":20,"
    "\"thoughtsTokenCount\":30,\"cachedContentTokenCount\":40,\"totalTokenCount\":150}}\r\n\r\n";

static void test_google_sse(void) {
    json_t *raw = J(FLASH25);
    ai_model_t m = mk_model(raw);
    rec_t rec = {SB_INIT, 0};
    google_sse_t s;
    google_sse_init(&s, &m, rec_cb, &rec);
    int rc = feed_sse(GOOGLE_SSE, google_sse_event, &s);
    CHECK_INT(rc, 0);
    CHECK(!google_sse_empty_stop(&s, rc));
    json_t *msg = google_sse_finish(&s, rc, NULL);
    google_sse_free(&s);
    CHECK_STR(json_get_str(msg, "stopReason"), "toolUse");
    json_t *c = json_get(msg, "content");
    CHECK_INT(json_len(c), 4);
    CHECK_STR(P(c, "0.type"), "thinking");
    CHECK_STR(P(c, "0.thinking"), "Thinking about it.");
    CHECK_STR(P(c, "0.thinkingSignature"), "VFNJRw==");
    CHECK_STR(P(c, "1.text"), "Hello world");
    CHECK_STR(P(c, "1.textSignature"), "VFhU");
    CHECK_STR(P(c, "2.type"), "toolCall");
    CHECK_STR(P(c, "2.name"), "bash");
    CHECK_STR(P(c, "2.arguments.command"), "ls");
    CHECK_STR(P(c, "2.thoughtSignature"), "RkM=");
    CHECK(P(c, "2.id") && str_starts(P(c, "2.id"), "bash_"));
    CHECK(!str_eq(P(c, "2.id"), P(c, "3.id")));
    CHECK_STR(P(c, "3.arguments.command"), "pwd");
    json_t *u = json_get(msg, "usage");
    CHECK_INT(json_get_int(u, "input", -1), 60);
    CHECK_INT(json_get_int(u, "output", -1), 50);
    CHECK_INT(json_get_int(u, "cacheRead", -1), 40);
    CHECK_INT(json_get_int(u, "cacheWrite", -1), 0);
    CHECK_INT(json_get_int(u, "reasoningTokens", -1), 30);
    CHECK_STR(sb_cstr(&rec.names), "start,thinking_start,thinking_end,text_start,text_end,toolcall_start,toolcall_end,"
                                   "toolcall_start,toolcall_end,done");
    sb_free(&rec.names);
    json_free(msg);
    json_free(raw);
}

static json_t *run_google(const ai_model_t *m, const char *sse, bool *empty) {
    google_sse_t s;
    google_sse_init(&s, m, NULL, NULL);
    int rc = feed_sse(sse, google_sse_event, &s);
    if (empty) *empty = google_sse_empty_stop(&s, rc);
    json_t *msg = google_sse_finish(&s, rc, NULL);
    google_sse_free(&s);
    return msg;
}

static void test_google_sse_edges(void) {
    json_t *raw = J(FLASH25);
    ai_model_t m = mk_model(raw);
    bool empty = false;
    json_t *msg = run_google(&m, "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"x\"}]},\"finishReason\":\"SAFETY\"}]}\n\n", NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    CHECK_STR(json_get_str(msg, "errorMessage"), "Generation failed with finish reason: SAFETY");
    json_free(msg);
    msg = run_google(&m, "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"x\"}]}}]}\n\n", NULL);
    CHECK_STR(json_get_str(msg, "errorMessage"),
              "Google API stream ended without a finish reason (connection dropped or response truncated)");
    json_free(msg);
    msg = run_google(&m, "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\" \"}]},\"finishReason\":\"STOP\"}]}\n\n", &empty);
    CHECK(empty);
    json_free(msg);
    msg = run_google(&m, "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"long\"}]},\"finishReason\":\"MAX_TOKENS\"}]}\n\n", &empty);
    CHECK(!empty);
    CHECK_STR(json_get_str(msg, "stopReason"), "length");
    json_free(msg);
    msg = run_google(&m, "data: {\"error\":{\"code\":429,\"message\":\"Quota\",\"status\":\"RESOURCE_EXHAUSTED\"}}\n\n", NULL);
    CHECK_STR(json_get_str(msg, "stopReason"), "error");
    CHECK_STR(json_get_str(msg, "errorMessage"), "Google API stream error: Quota (RESOURCE_EXHAUSTED)");
    json_free(msg);
    msg = run_google(&m, "data: {\"promptFeedback\":{\"blockReason\":\"SAFETY\"}}\n\n", NULL);
    CHECK_STR(json_get_str(msg, "errorMessage"), "Request blocked by Google (SAFETY)");
    json_free(msg);
    json_free(raw);
}

/* message threads: continue only past an extended conversation, within the window, with the fixed fields unchanged;
 * 16 chains per endpoint and model; refused continues replay, then stop threading */
static json_t *thread_params(const char *msgs_json, const char *system) {
    char *j = xasprintf("{\"model\":\"claude-x\",\"messages\":%s,\"system\":\"%s\",\"tools\":[],\"stream\":true}", msgs_json, system);
    json_t *p = json_parse_str(j);
    free(j);
    return p;
}

#define T_U1 "{\"role\":\"user\",\"content\":\"one\"}"
#define T_A1 "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}"
#define T_U2 "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"two\",\"cache_control\":{\"type\":\"ephemeral\"}}]}"

static void test_message_threads(void) {
    json_t *a1 = json_parse_str(T_A1);
    bool cont = false;
    anth_thread_turn_t *t = anth_thread_turn_new("https://api.anthropic.com", "tm", "s1", false);
    json_t *p = thread_params("[" T_U1 "]", "sys");
    json_t *w = anth_thread_plan(t, p, "b,a", 1000, &cont);
    CHECK(w && !cont);
    CHECK_STR(json_get_str(json_get(w, "thread"), "type"), "create");
    CHECK(json_get(w, "system") != NULL);
    json_free(w);
    json_free(p);
    anth_thread_settle(t, "msg_1", a1, 1000);
    anth_thread_turn_free(t);

    /* extended, same fixed fields (beta order aside), the string message equals its cached single-block form */
    t = anth_thread_turn_new("https://api.anthropic.com", "tm", "s1", false);
    p = thread_params("[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"one\",\"cache_control\":{\"type\":\"ephemeral\"}}]}," T_A1 "," T_U2 "]", "sys");
    w = anth_thread_plan(t, p, "a,b,a", 2000, &cont);
    CHECK(w && cont);
    CHECK_STR(json_get_str(json_get(w, "thread"), "previous_message_id"), "msg_1");
    CHECK(json_len(json_get(w, "messages")) == 1);
    CHECK(json_get(w, "system") == NULL && json_get(w, "tools") == NULL);
    CHECK(json_get(json_at(json_get(json_at(json_get(w, "messages"), 0), "content"), 0), "cache_control") != NULL);
    json_free(w);
    json_free(p);
    /* a transient retry of the same turn keeps continuing the claimed id */
    p = thread_params("[" T_U1 "," T_A1 "," T_U2 "]", "sys");
    w = anth_thread_plan(t, p, "a,b", 2000, &cont);
    CHECK(w && cont);
    json_free(w);
    /* a refused continue replays, a second one stops threading */
    CHECK(anth_thread_recover(t, 500, "{}") == ANTH_THREAD_NO_RECOVERY);
    CHECK(anth_thread_recover(t, 404, "{\"error\":{\"type\":\"not_found_error\",\"message\":\"model: nope\"}}") == ANTH_THREAD_NO_RECOVERY);
    CHECK(anth_thread_recover(t, 404, "{\"error\":{\"details\":{\"error_code\":\"thread_not_found\"}}}") == ANTH_THREAD_REPLAY);
    w = anth_thread_plan(t, p, "a,b", 2000, &cont);
    CHECK(w && !cont);
    CHECK_STR(json_get_str(json_get(w, "thread"), "type"), "create");
    json_free(w);
    json_free(p);
    anth_thread_settle(t, "msg_2", a1, 2000);
    anth_thread_turn_free(t);

    /* the error text of a codeless thread 400, and what is not one */
    t = anth_thread_turn_new("https://api.anthropic.com", "tm", "s-text", false);
    p = thread_params("[" T_U1 "]", "sys");
    w = anth_thread_plan(t, p, "", 1000, &cont);
    json_free(w);
    CHECK(anth_thread_recover(t, 400, "{\"error\":{\"type\":\"invalid_request_error\",\"message\":\"x.diagnostics.previous_message_id bad\"}}") == ANTH_THREAD_NO_RECOVERY);
    CHECK(anth_thread_recover(t, 400, "{\"error\":{\"type\":\"invalid_request_error\",\"message\":\"threading is off\"}}") == ANTH_THREAD_NO_RECOVERY);
    CHECK(anth_thread_recover(t, 400, "{\"error\":{\"type\":\"invalid_request_error\",\"message\":\"thread: nope\"}}") == ANTH_THREAD_UNTHREADED);
    w = anth_thread_plan(t, p, "", 1000, &cont); /* a refused create stops threading the session */
    CHECK(w == NULL);
    json_free(p);
    anth_thread_turn_free(t);

    /* past the reuse window, changed fixed fields or rewritten history start a new thread */
    static const struct {
        const char *msgs, *system;
        const char *beta;
        int64_t now;
    } fresh[] = {{"[" T_U1 "," T_A1 "," T_U2 "]", "sys", "", 1000 + 270000},
                 {"[" T_U1 "," T_A1 "," T_U2 "]", "other", "", 2000},
                 {"[" T_U1 "," T_A1 "," T_U2 "]", "sys", "new-beta", 2000},
                 {"[{\"role\":\"user\",\"content\":\"edited\"}," T_A1 "," T_U2 "]", "sys", "", 2000},
                 {"[" T_U1 "," T_A1 "]", "sys", "", 2000}};
    for (size_t i = 0; i < ARRAY_LEN(fresh); i++) {
        char sid[32];
        snprintf(sid, sizeof sid, "s-fresh-%zu", i);
        t = anth_thread_turn_new("https://api.anthropic.com", "tm2", sid, false);
        p = thread_params("[" T_U1 "]", "sys");
        w = anth_thread_plan(t, p, "", 1000, &cont);
        json_free(w);
        json_free(p);
        anth_thread_settle(t, "msg_f", a1, 1000);
        anth_thread_turn_free(t);
        t = anth_thread_turn_new("https://api.anthropic.com", "tm2", sid, false);
        p = thread_params(fresh[i].msgs, fresh[i].system);
        w = anth_thread_plan(t, p, fresh[i].beta, fresh[i].now, &cont);
        CHECK(w && !cont);
        json_free(w);
        json_free(p);
        anth_thread_turn_free(t);
        anth_threads_release_session(sid);
    }

    /* OAuth traffic keeps system and tools on a continue */
    t = anth_thread_turn_new("https://api.anthropic.com", "tm3", "s-oauth", true);
    p = thread_params("[" T_U1 "]", "sys");
    w = anth_thread_plan(t, p, "", 1000, &cont);
    json_free(w);
    json_free(p);
    anth_thread_settle(t, "msg_o", a1, 1000);
    anth_thread_turn_free(t);
    t = anth_thread_turn_new("https://api.anthropic.com", "tm3", "s-oauth", true);
    p = thread_params("[" T_U1 "," T_A1 "," T_U2 "]", "sys");
    w = anth_thread_plan(t, p, "", 2000, &cont);
    CHECK(w && cont && json_get(w, "system") && json_get(w, "tools"));
    json_free(w);
    json_free(p);
    anth_thread_turn_free(t);

    /* an unsuitable body goes without a thread and leaves the anchor alone */
    t = anth_thread_turn_new("https://api.anthropic.com", "tm4", "s-c", false);
    json_t *cp = json_parse_str("{\"model\":\"m\",\"messages\":[],\"compaction\":{\"type\":\"summarize\"}}");
    CHECK(anth_thread_plan(t, cp, "", 1000, &cont) == NULL);
    json_free(cp);
    anth_thread_turn_free(t);

    /* at most 16 chains per endpoint and model: the least recently used one goes first */
    for (int i = 0; i < 17; i++) {
        char sid[32];
        snprintf(sid, sizeof sid, "s-lru-%d", i);
        t = anth_thread_turn_new("https://api.anthropic.com", "tm5", sid, false);
        p = thread_params("[" T_U1 "]", "sys");
        w = anth_thread_plan(t, p, "", 1000, &cont);
        json_free(w);
        json_free(p);
        anth_thread_settle(t, "msg_l", a1, 1000);
        anth_thread_turn_free(t);
    }
    p = thread_params("[" T_U1 "," T_A1 "," T_U2 "]", "sys");
    t = anth_thread_turn_new("https://api.anthropic.com", "tm5", "s-lru-16", false);
    w = anth_thread_plan(t, p, "", 2000, &cont);
    CHECK(w && cont);
    json_free(w);
    anth_thread_turn_free(t);
    t = anth_thread_turn_new("https://api.anthropic.com", "tm5", "s-lru-0", false);
    w = anth_thread_plan(t, p, "", 2000, &cont);
    CHECK(w && !cont);
    json_free(w);
    anth_thread_turn_free(t);
    json_free(p);
    anth_threads_reset();
    json_free(a1);
}

/* omp 7f6b4a096f: an empty conversation is named before it reaches Anthropic's bare 400 */
static void test_anthropic_empty_request(void) {
    json_t *raw = J(SONNET45);
    ai_model_t m = mk_model(raw);
    json_t *msgs = J("[]");
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.api_key = "sk-ant-api03-test";
    json_t *r = provider_anthropic_stream(&m, &ctx, &o, NULL, NULL);
    CHECK_STR(P(r, "stopReason"), "error");
    CHECK_STR(P(r, "errorMessage"), "anthropic: refusing to send a request with no messages for model claude-sonnet-4-5 (provider "
                                    "anthropic). Context had 0 input messages; none survived encoding.");
    json_free(r);
    json_free(msgs);
    json_free(raw);
}

int main(void) {
    /* isolate the OAuth install-id file */
    const char *tmp = getenv("TMPDIR");
    char *home = xasprintf("%s/omc-test-home-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    setenv("HOME", home, 1);
    unsetenv("PI_CACHE_RETENTION");
    unsetenv("ANTHROPIC_BASE_URL");
    unsetenv("PI_AI_CLAUDE_CODE_VERSION");

    test_message_threads();
    test_anthropic_empty_request();
    test_anthropic_request_apikey();
    test_anthropic_request_adaptive();
    test_anthropic_request_oauth();
    test_anthropic_proxy_headers();
    test_anthropic_schema();
    test_anthropic_sse();
    test_anthropic_sse_edges();
    test_google_request();
    test_google_request_gemini3();
    test_google_sse();
    test_google_sse_edges();

    char *idf = xasprintf("%s/.omc/install-id", home);
    unlink(idf);
    char *dir = xasprintf("%s/.omc", home);
    rmdir(dir);
    rmdir(home);
    free(idf);
    free(dir);
    free(home);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
