/* test_kiro.c - offline tests for the native Kiro provider (omp v19.1.0 catalog/test/kiro-provider.test.ts,
 * ai/test/kiro-transform.test.ts, kiro-usage.test.ts): region / profile-ARN routing, the credential codec, the Kiro CLI
 * headers, ListAvailableModels mapping, the request transform and budget, and parseKiroUsage. */
#include "ai/kiro.h"
#include "config/usage.h"
#include "core/fs.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        g_checks++;                                                               \
        if (!(c)) {                                                               \
            g_fail++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                         \
    } while (0)
#define CHECK_STR(a, b)                                                                                        \
    do {                                                                                                       \
        const char *a_ = (a), *b_ = (b);                                                                       \
        g_checks++;                                                                                            \
        if (!str_eq(a_, b_)) {                                                                                 \
            g_fail++;                                                                                          \
            fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\" (%s)\n", __FILE__, __LINE__, b_ ? b_ : "(null)", \
                    a_ ? a_ : "(null)", #a);                                                                   \
        }                                                                                                      \
    } while (0)

static json_t *J(const char *s) {
    json_t *j = json_parse_str(s);
    if (!j) {
        fprintf(stderr, "bad test json: %s\n", s);
        exit(2);
    }
    return j;
}
static const char *P(const json_t *j, const char *path) { return json_sv(json_path(j, path)); }
static double PN(const json_t *j, const char *path) {
    json_t *v = json_path(j, path);
    return json_is_num(v) ? v->u.num.d : -1;
}

static void test_routing(void) {
    CHECK(kiro_is_region("us-east-1") && kiro_is_region("us-gov-west-1") && kiro_is_region("eu-central-1"));
    CHECK(!kiro_is_region("us-east") && !kiro_is_region("US-east-1") && !kiro_is_region("us-1") && !kiro_is_region("us-east-1a"));
    CHECK(!kiro_is_region("") && !kiro_is_region("us--1") && !kiro_is_region("evil.com/x-1"));
    char *r = kiro_profile_region("arn:aws:codewhisperer:eu-central-1:123456789012:profile/ABC_def-1");
    CHECK_STR(r, "eu-central-1");
    free(r);
    r = kiro_profile_region("arn:aws-us-gov:codewhisperer:us-gov-west-1:1:profile/X");
    CHECK_STR(r, "us-gov-west-1");
    free(r);
    CHECK(!kiro_profile_region("arn:aws:codewhisperer:us-east-1:12a:profile/X"));
    CHECK(!kiro_profile_region("arn:aws:codewhisperer:us-east-1:1:profile/"));
    CHECK(!kiro_profile_region("arn:aws:s3:us-east-1:1:profile/X"));
    CHECK(!kiro_profile_region("arn:aws:codewhisperer:us-east-1:1:profile/a.b"));
    CHECK_STR(kiro_profile_regions("us-gov-east-1")[0], "us-gov-west-1");
    CHECK_STR(kiro_profile_regions("eu-west-1")[1], "eu-central-1");
    char *u = kiro_runtime_base_url("us-east-1");
    CHECK_STR(u, "https://runtime.us-east-1.kiro.dev/");
    free(u);
    u = kiro_management_base_url("eu-central-1");
    CHECK_STR(u, "https://management.eu-central-1.kiro.dev/");
    free(u);
}

static void test_codec(void) {
    unsetenv("KIRO_API_REGION");
    kiro_cred_t c;
    char *err = NULL;
    CHECK(kiro_decode_credential("ksk_abc", &c, &err) == 0 && !c.oauth && !c.region && str_eq(c.token, "ksk_abc"));
    kiro_cred_free(&c);
    setenv("KIRO_API_REGION", "eu-central-1", 1);
    CHECK(kiro_decode_credential("  ksk_abc ", &c, &err) == 0 && str_eq(c.region, "eu-central-1"));
    kiro_cred_free(&c);
    setenv("KIRO_API_REGION", "nowhere", 1);
    CHECK(kiro_decode_credential("ksk_abc", &c, &err) == -1);
    CHECK_STR(err, "KIRO_API_REGION is not a valid AWS region");
    free(err);
    unsetenv("KIRO_API_REGION");
    CHECK(kiro_decode_credential("sk-other", &c, &err) == -1);
    CHECK_STR(err, "Kiro API keys must use the ksk_… format");
    free(err);
    CHECK(kiro_decode_credential("", &c, &err) == -1);
    CHECK_STR(err, "Kiro credential is missing");
    free(err);
    CHECK(kiro_decode_credential("{\"kind\":\"oauth\",\"token\":\"t\",\"region\":\"us-east-1\"}", &c, &err) == -1);
    CHECK_STR(err, "Kiro login is incomplete; run /login kiro again");
    free(err);
    CHECK(kiro_decode_credential("{\"kind\":\"oauth\",\"token\":\"t\",\"region\":\"us-east-1\",\"loginId\":\"L\",\"profileArn\":\"bad\"}",
                                 &c, &err) == -1);
    CHECK_STR(err, "Kiro credential has an invalid profile ARN");
    free(err);
    CHECK(kiro_decode_credential("{\"kind\":\"api_key\",\"token\":\"t\"}", &c, &err) == -1);
    CHECK_STR(err, "Kiro API keys must use the ksk_… format");
    free(err);
    CHECK(kiro_decode_credential("{nope", &c, &err) == -1);
    CHECK_STR(err, "Kiro credential is not valid JSON");
    free(err);

    json_t *row = J("{\"type\":\"oauth\",\"access\":\"acc\",\"refresh\":\"r\",\"expires\":1,\"region\":\"eu-central-1\","
                    "\"accountId\":\"login-1\",\"kiroProfileArn\":\"arn:aws:codewhisperer:eu-central-1:1:profile/P\"}");
    char *enc = kiro_encode_oauth_row(row, &err);
    CHECK_STR(enc, "{\"kind\":\"oauth\",\"token\":\"acc\",\"region\":\"eu-central-1\",\"loginId\":\"login-1\","
                   "\"profileArn\":\"arn:aws:codewhisperer:eu-central-1:1:profile/P\"}");
    CHECK(kiro_decode_credential(enc, &c, &err) == 0 && c.oauth && str_eq(c.login_id, "login-1") && str_eq(c.region, "eu-central-1"));
    char *k1 = kiro_credential_cache_key(&c);
    CHECK(str_starts(k1, "kiro:models-v2:"));
    free(c.profile_arn);
    c.profile_arn = NULL;
    char *k2 = kiro_credential_cache_key(&c);
    CHECK(!str_eq(k1, k2)); /* a login moved to another profile has its own roster */
    free(k1);
    free(k2);
    kiro_cred_free(&c);
    free(enc);
    json_del(row, "accountId");
    CHECK(!kiro_encode_oauth_row(row, &err));
    CHECK_STR(err, "Kiro login is incomplete; run /login kiro again");
    free(err);
    json_free(row);
    kiro_cred_t api = {.token = "ksk_x", .region = "us-east-1"};
    enc = kiro_encode_credential(&api);
    CHECK_STR(enc, "{\"kind\":\"api_key\",\"token\":\"ksk_x\",\"region\":\"us-east-1\"}");
    free(enc);
}

static void test_headers(void) {
    http_headers_t h = {0};
    kiro_service_headers(&h, "GenerateAssistantResponse", "ksk_key", 2, 2);
    CHECK_STR(http_headers_get(&h, "x-amz-target"), "AmazonCodeWhispererStreamingService.GenerateAssistantResponse");
    CHECK_STR(http_headers_get(&h, "content-type"), "application/x-amz-json-1.0");
    CHECK_STR(http_headers_get(&h, "x-amzn-kiro-attempt"), "2;max=2");
    CHECK_STR(http_headers_get(&h, "x-amzn-kiro-fallback"), "0;max=0");
    CHECK_STR(http_headers_get(&h, "tokentype"), "API_KEY");
    CHECK_STR(http_headers_get(&h, "amz-sdk-request"), "attempt=2; max=2");
    CHECK_STR(http_headers_get(&h, "x-amzn-codewhisperer-optout"), "true");
    CHECK(str_starts(http_headers_get(&h, "user-agent"), "aws-sdk-rust/1.6.0 ua/2.1 api/codewhispererstreaming/0.1.19636 os/"));
    CHECK(str_ends(http_headers_get(&h, "user-agent"), " lang/rust/1.94.1 md/appVersion-3.2.0 app/AmazonQ-For-CLI"));
    CHECK(str_ends(http_headers_get(&h, "x-amz-user-agent"), " lang/rust/1.94.1 m/F app/AmazonQ-For-CLI"));
    http_headers_free(&h);
    kiro_service_headers(&h, "ListAvailableModels", "bearer", 1, 1);
    CHECK(!http_headers_get(&h, "tokentype") && !http_headers_get(&h, "x-amzn-kiro-attempt"));
    CHECK_STR(http_headers_get(&h, "authorization"), "Bearer bearer");
    CHECK(str_ends(http_headers_get(&h, "x-amz-user-agent"), " m/F,C app/AmazonQ-For-CLI"));
    http_headers_free(&h);
    kiro_oidc_headers(&h, 1, 3);
    CHECK(strstr(http_headers_get(&h, "user-agent"), "api/ssooidc/1.100.0") &&
          str_ends(http_headers_get(&h, "user-agent"), " m/E,N app/AmazonQ-For-CLI"));
    CHECK_STR(http_headers_get(&h, "amz-sdk-request"), "attempt=1; max=3");
    http_headers_free(&h);
}

static void test_map_model(void) {
    json_t *raw = J("{\"modelId\":\"claude-x\",\"modelName\":\"Claude X\",\"tokenLimits\":{\"maxInputTokens\":200000,"
                    "\"maxOutputTokens\":64000},\"supportedInputTypes\":[\"TEXT\",\"IMAGE\"],\"rateUnit\":\"Credit\",\"rateMultiplier\":1.3,"
                    "\"additionalModelRequestFieldsSchema\":{\"properties\":{\"thinking\":{\"properties\":{\"type\":{\"enum\":"
                    "[\"adaptive\",\"disabled\"]}}},\"output_config\":{\"properties\":{\"effort\":{\"enum\":[\"low\",\"medium\",\"high\","
                    "\"max\"],\"default\":\"high\"}}},\"max_tokens\":{\"maximum\":128000}}}}");
    json_t *m = kiro_map_model(raw, "https://runtime.us-east-1.kiro.dev/");
    char *s = json_dump(m, 0);
    CHECK_STR(s, "{\"id\":\"claude-x\",\"name\":\"Claude X\",\"api\":\"kiro-api\",\"provider\":\"kiro\",\"baseUrl\":"
                 "\"https://runtime.us-east-1.kiro.dev/\",\"reasoning\":true,\"thinking\":{\"mode\":\"anthropic-adaptive\",\"efforts\":"
                 "[\"low\",\"medium\",\"high\",\"max\"],\"defaultLevel\":\"high\",\"effortMap\":{\"minimal\":\"low\"},\"supportsDisplay\":true,"
                 "\"suppressWhenOff\":true},\"input\":[\"text\",\"image\"],\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,"
                 "\"cacheWrite\":0},\"premiumMultiplier\":1.3,\"contextWindow\":200000,\"maxTokens\":128000}");
    free(s);
    json_free(m);
    json_free(raw);
    raw = J("{\"modelId\":\"gpt-y\",\"tokenLimits\":{\"maxInputTokens\":400000,\"maxOutputTokens\":32000},"
            "\"additionalModelRequestFieldsSchema\":{\"properties\":{\"reasoning\":{\"properties\":{\"effort\":{\"enum\":"
            "[\"low\",\"high\"],\"default\":\"low\"}}}}}}");
    m = kiro_map_model(raw, "u");
    CHECK_STR(P(m, "name"), "gpt-y");
    CHECK_STR(P(m, "thinking.mode"), "effort");
    CHECK(json_get_bool(json_get(m, "thinking"), "requiresEffort", false) && !json_get(json_get(m, "thinking"), "effortMap"));
    CHECK_STR(P(m, "thinking.defaultLevel"), "low");
    CHECK(PN(m, "maxTokens") == 32000 && !json_get(m, "premiumMultiplier"));
    json_free(m);
    json_free(raw);
    raw = J("{\"modelId\":\"z\",\"tokenLimits\":{\"maxInputTokens\":0,\"maxOutputTokens\":1}}");
    CHECK(!kiro_map_model(raw, "u"));
    json_free(raw);
    raw = J("{\"modelId\":\"weird\",\"tokenLimits\":{\"maxInputTokens\":10,\"maxOutputTokens\":1},"
            "\"additionalModelRequestFieldsSchema\":{\"properties\":{}}}");
    m = kiro_map_model(raw, "u");
    CHECK(m && !json_get_bool(m, "reasoning", true) && !json_get(m, "thinking"));
    json_free(m);
    json_free(raw);
}

static ai_model_t *model(const char *thinking, int64_t window) {
    char *s = xasprintf("{\"id\":\"kiro-test-model\",\"name\":\"Kiro test model\",\"api\":\"kiro-api\",\"provider\":\"kiro\","
                        "\"baseUrl\":\"https://runtime.us-east-1.kiro.dev/\",\"reasoning\":%s,\"input\":[\"text\",\"image\"],"
                        "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0},\"contextWindow\":%lld,\"maxTokens\":4096}",
                        thinking ? "true" : "false", (long long)window);
    json_t *raw = J(s);
    free(s);
    if (thinking) json_set(raw, "thinking", J(thinking));
    return catalog_model_variant(NULL, raw, true);
}

static json_t *build(const ai_model_t *m, json_t *msgs, json_t *sys, json_t *tools, const ai_stream_opts_t *o, char **err, bool *ov) {
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t d;
    ai_stream_opts_init(&d);
    d.session_id = "conv-1";
    json_t *r = kiro_build_request(m, &ctx, o ? o : &d, err, ov);
    return r;
}

static void test_transform(void) {
    ai_model_t *m = model(NULL, 32000);
    char *err = NULL;
    bool ov = false;
    json_t *msgs = J("[{\"role\":\"user\",\"content\":\"Earlier question\",\"timestamp\":0},"
                     "{\"role\":\"assistant\",\"content\":[{\"type\":\"toolCall\",\"id\":\"tooluse_1\",\"name\":\"lookup\",\"arguments\":"
                     "{\"path\":\"tooluse_1\"}}],\"stopReason\":\"toolUse\",\"api\":\"kiro-api\",\"provider\":\"kiro\",\"model\":"
                     "\"kiro-test-model\",\"timestamp\":1},"
                     "{\"role\":\"toolResult\",\"toolCallId\":\"tooluse_1\",\"toolName\":\"lookup\",\"content\":[{\"type\":\"text\",\"text\":"
                     "\"shot\"},{\"type\":\"image\",\"data\":\"aGVsbG8=\",\"mimeType\":\"image/png\"}],\"isError\":false,\"timestamp\":2},"
                     "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Current question\"},{\"type\":\"image\",\"data\":\"aGk=\","
                     "\"mimeType\":\"image/jpeg\"}],\"timestamp\":3}]");
    json_t *sys = J("[\"Follow the system instruction.\"]");
    json_t *tools = J("[{\"name\":\"lookup\",\"description\":\"Look up a path.\",\"parameters\":{\"type\":\"object\",\"properties\":"
                      "{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},{\"name\":\"other\",\"description\":\"d\"}]");
    json_t *r = build(m, msgs, sys, tools, NULL, &err, &ov);
    CHECK(r != NULL);
    CHECK_STR(P(r, "agentMode"), "vibe");
    CHECK_STR(P(r, "conversationState.conversationId"), "conv-1");
    CHECK_STR(P(r, "conversationState.chatTriggerType"), "MANUAL");
    json_t *hist = json_path(r, "conversationState.history");
    CHECK(json_len(hist) == 2);
    CHECK_STR(P(json_at(hist, 0), "userInputMessage.content"), "Follow the system instruction.\n\nEarlier question");
    CHECK_STR(P(json_at(hist, 0), "userInputMessage.origin"), "KIRO_CLI");
    CHECK_STR(P(json_at(hist, 1), "assistantResponseMessage.content"), "");
    CHECK_STR(P(json_at(json_path(json_at(hist, 1), "assistantResponseMessage.toolUses"), 0), "toolUseId"), "tooluse_1");
    json_t *cur = json_path(r, "conversationState.currentMessage.userInputMessage");
    CHECK_STR(P(cur, "content"), "Current question");
    CHECK_STR(P(cur, "modelId"), "kiro-test-model");
    char *s = json_dump(json_path(cur, "userInputMessageContext.toolResults"), 0);
    CHECK_STR(s, "[{\"content\":[{\"text\":\"shot\"}],\"status\":\"success\",\"toolUseId\":\"tooluse_1\"}]");
    free(s);
    s = json_dump(json_get(cur, "images"), 0);
    CHECK_STR(s, "[{\"format\":\"jpeg\",\"source\":{\"bytes\":\"aGk=\"}},{\"format\":\"png\",\"source\":{\"bytes\":\"aGVsbG8=\"}}]");
    free(s);
    json_t *specs = json_path(cur, "userInputMessageContext.tools");
    CHECK(json_len(specs) == 2);
    CHECK_STR(P(json_at(specs, 0), "toolSpecification.inputSchema.json.properties.path.type"), "string");
    CHECK_STR(P(json_at(specs, 1), "toolSpecification.inputSchema.json.type"), NULL); /* no parameters: {} */
    CHECK(!json_get(r, "additionalModelRequestFields") && !json_get(r, "profileArn"));
    json_free(r);
    json_free(msgs);
    json_free(tools);

    /* an unsupported image type is an error */
    msgs = J("[{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"data\":\"Qk0=\",\"mimeType\":\"image/bmp\"}],\"timestamp\":0}]");
    CHECK(!build(m, msgs, NULL, NULL, NULL, &err, &ov));
    CHECK_STR(err, "Kiro does not support image type: image/bmp");
    free(err);
    json_free(msgs);

    /* the continuation prompt for empty and assistant-last turns; a retired tool is declared as a placeholder */
    msgs = J("[{\"role\":\"user\",\"content\":\" \\t\\n\",\"timestamp\":0}]");
    r = build(m, msgs, NULL, NULL, NULL, &err, &ov);
    CHECK_STR(P(r, "conversationState.currentMessage.userInputMessage.content"), "Please proceed with the task.");
    CHECK(!json_path(r, "conversationState.history"));
    json_free(r);
    json_free(msgs);
    msgs = J("[{\"role\":\"user\",\"content\":\"t1\",\"timestamp\":0},"
             "{\"role\":\"assistant\",\"content\":[{\"type\":\"toolCall\",\"id\":\"a.b\",\"name\":\"retired\",\"arguments\":{}}],"
             "\"stopReason\":\"toolUse\",\"api\":\"kiro-api\",\"provider\":\"kiro\",\"model\":\"kiro-test-model\",\"timestamp\":1},"
             "{\"role\":\"toolResult\",\"toolCallId\":\"a.b\",\"toolName\":\"retired\",\"content\":[{\"type\":\"text\",\"text\":\"r\"}],"
             "\"isError\":true,\"timestamp\":2},"
             "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"t1 done\"}],\"stopReason\":\"stop\",\"api\":\"kiro-api\","
             "\"provider\":\"kiro\",\"model\":\"kiro-test-model\",\"timestamp\":3}]");
    r = build(m, msgs, NULL, NULL, NULL, &err, &ov);
    hist = json_path(r, "conversationState.history");
    CHECK(json_len(hist) == 4);
    CHECK_STR(P(json_at(json_path(json_at(hist, 2), "userInputMessage.userInputMessageContext.toolResults"), 0), "status"), "error");
    CHECK_STR(P(json_at(json_path(json_at(hist, 2), "userInputMessage.userInputMessageContext.toolResults"), 0), "toolUseId"), "a_b");
    CHECK_STR(P(json_at(hist, 2), "userInputMessage.content"), "");
    cur = json_path(r, "conversationState.currentMessage.userInputMessage");
    CHECK_STR(P(cur, "content"), "Please proceed with the task.");
    specs = json_path(cur, "userInputMessageContext.tools");
    CHECK(json_len(specs) == 1);
    CHECK_STR(P(json_at(specs, 0), "toolSpecification.name"), "retired");
    CHECK_STR(P(json_at(specs, 0), "toolSpecification.description"), "Tool used in conversation history.");
    json_free(r);
    json_free(msgs);
    json_free(sys);
    catalog_model_free(m);

    /* thinking: adaptive effort with the output cap, GPT effort, thinking off sends the advertised off value */
    ai_model_t *ad = model("{\"mode\":\"anthropic-adaptive\",\"efforts\":[\"low\",\"high\"],\"defaultLevel\":\"high\","
                           "\"supportsDisplay\":true,\"suppressWhenOff\":true}",
                           32000);
    ai_model_t *gpt = model("{\"mode\":\"effort\",\"efforts\":[\"minimal\",\"low\",\"high\"],\"defaultLevel\":\"high\","
                            "\"effortMap\":{\"minimal\":\"none\"},\"suppressWhenOff\":true}",
                            32000);
    msgs = J("[{\"role\":\"user\",\"content\":\"Think\",\"timestamp\":0}]");
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_MEDIUM;
    o.hide_thinking_summary = true;
    o.max_tokens = 9999;
    r = build(ad, msgs, NULL, NULL, &o, &err, &ov);
    s = json_dump(json_get(r, "additionalModelRequestFields"), 0);
    CHECK_STR(s, "{\"thinking\":{\"type\":\"adaptive\",\"display\":\"omitted\"},\"output_config\":{\"effort\":\"low\"},\"max_tokens\":4096}");
    free(s);
    json_free(r);
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_LOW;
    r = build(gpt, msgs, NULL, NULL, &o, &err, &ov);
    s = json_dump(json_get(r, "additionalModelRequestFields"), 0);
    CHECK_STR(s, "{\"reasoning\":{\"mode\":\"standard\",\"effort\":\"low\"}}");
    free(s);
    json_free(r);
    for (int k = 0; k < 2; k++) {
        ai_stream_opts_init(&o);
        if (k) o.reasoning = EFFORT_HIGH, o.disable_reasoning = true;
        r = build(ad, msgs, NULL, NULL, &o, &err, &ov);
        s = json_dump(json_get(r, "additionalModelRequestFields"), 0);
        CHECK_STR(s, "{\"thinking\":{\"type\":\"disabled\"}}");
        free(s);
        json_free(r);
        r = build(gpt, msgs, NULL, NULL, &o, &err, &ov);
        s = json_dump(json_get(r, "additionalModelRequestFields"), 0);
        CHECK_STR(s, "{\"reasoning\":{\"mode\":\"standard\",\"effort\":\"none\"}}");
        free(s);
        json_free(r);
    }
    json_free(msgs);
    catalog_model_free(ad);
    catalog_model_free(gpt);
}

static char *rep(const char *unit, size_t n) {
    size_t u = strlen(unit);
    char *s = xmalloc(u * n + 1);
    for (size_t i = 0; i < n; i++) memcpy(s + i * u, unit, u);
    s[u * n] = 0;
    return s;
}

static json_t *tool_turn(json_t *msgs, const char *id, const char *text, bool done) {
    json_t *u = J("{\"role\":\"user\",\"timestamp\":0}");
    char *t = xasprintf("Turn %s", id);
    json_set_str(u, "content", t);
    free(t);
    json_push(msgs, u);
    json_t *a = J("{\"role\":\"assistant\",\"content\":[{\"type\":\"toolCall\",\"name\":\"lookup\",\"arguments\":{}}],\"stopReason\":"
                  "\"toolUse\",\"api\":\"kiro-api\",\"provider\":\"kiro\",\"model\":\"kiro-test-model\",\"timestamp\":1}");
    json_set_str(json_at(json_get(a, "content"), 0), "id", id);
    json_push(msgs, a);
    json_t *r = J("{\"role\":\"toolResult\",\"toolName\":\"lookup\",\"isError\":false,\"timestamp\":2,\"content\":[{\"type\":\"text\"}]}");
    json_set_str(r, "toolCallId", id);
    json_set_str(json_at(json_get(r, "content"), 0), "text", text);
    json_push(msgs, r);
    if (done) {
        json_t *d = J("{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Done\"}],\"stopReason\":\"stop\",\"api\":"
                      "\"kiro-api\",\"provider\":\"kiro\",\"model\":\"kiro-test-model\",\"timestamp\":3}");
        json_push(msgs, d);
    }
    return msgs;
}

static void test_budget(void) {
    char *err = NULL;
    bool ov = false;
    /* a result over the 800 KB service limit is cut with an omitted-bytes marker */
    ai_model_t *m = model(NULL, 1000000);
    char *big = rep("\xc3\xa9", 450000); /* 900,000 UTF-8 bytes */
    json_t *msgs = tool_turn(json_arr(), "big", big, false);
    json_t *r = build(m, msgs, NULL, NULL, NULL, &err, &ov);
    const char *res = P(json_at(json_path(r, "conversationState.currentMessage.userInputMessage.userInputMessageContext.toolResults"), 0),
                        "content");
    (void)res;
    json_t *tr = json_at(json_path(r, "conversationState.currentMessage.userInputMessage.userInputMessageContext.toolResults"), 0);
    const char *text = json_get_str(json_at(json_get(tr, "content"), 0), "text");
    const char *mark = strstr(text, "\n\n[");
    CHECK(strlen(text) <= 800000 && mark && (size_t)(mark - text) > 799000 && !strncmp(big, text, (size_t)(mark - text)));
    CHECK(strstr(mark, "900000") != NULL);
    CHECK(strstr(mark, "[Tool result truncated: only the first ") == mark + 2);
    json_free(r);
    json_free(msgs);
    free(big);
    catalog_model_free(m);

    /* the oldest results shrink to 25 KB until the request fits */
    m = model(NULL, 200000);
    char *texts[4];
    const char *ids[4] = {"h1", "h2", "h3", "current"};
    char *x = rep("x", 300000);
    msgs = json_arr();
    for (int i = 0; i < 4; i++) {
        texts[i] = xasprintf("%s:%s", ids[i], x);
        tool_turn(msgs, ids[i], texts[i], i < 3);
    }
    r = build(m, msgs, NULL, NULL, NULL, &err, &ov);
    CHECK(r != NULL);
    json_t *hist = json_path(r, "conversationState.history"), *e;
    int k = 0;
    json_arr_foreach(hist, i, e) {
        json_t *rs = json_path(e, "userInputMessage.userInputMessageContext.toolResults");
        if (!rs) continue;
        const char *t = json_get_str(json_at(json_get(json_at(rs, 0), "content"), 0), "text");
        if (k < 2) {
            const char *mk = strstr(t, "\n\n[");
            CHECK(strlen(t) <= 25000 && mk && !strncmp(texts[k], t, (size_t)(mk - t)));
        } else CHECK_STR(t, texts[2]);
        k++;
    }
    CHECK(k == 3);
    tr = json_at(json_path(r, "conversationState.currentMessage.userInputMessage.userInputMessageContext.toolResults"), 0);
    CHECK_STR(json_get_str(json_at(json_get(tr, "content"), 0), "text"), texts[3]);
    json_free(r);
    json_free(msgs);
    for (int i = 0; i < 4; i++) free(texts[i]);
    free(x);
    catalog_model_free(m);

    /* a 20k-token window allows 85,000 bytes: still over after shrinking -> context overflow */
    m = model(NULL, 20000);
    x = rep("x", 100000);
    msgs = J("[{\"role\":\"user\",\"timestamp\":0}]");
    json_set_str(json_at(msgs, 0), "content", x);
    CHECK(!build(m, msgs, NULL, NULL, NULL, &err, &ov) && ov);
    CHECK_STR(err, "Kiro request exceeds local limit of 85000 UTF-8 bytes");
    free(err);
    json_free(msgs);
    free(x);
    catalog_model_free(m);
}

static void test_usage_parse(void) {
    const char *base = "{\"overageConfiguration\":{\"overageStatus\":\"%s\"},\"subscriptionInfo\":{\"subscriptionTitle\":\"KIRO PRO\"},"
                       "\"userInfo\":{\"email\":\"someone@example.com\"},\"usageBreakdownList\":[{\"resourceType\":\"CREDIT\","
                       "\"displayNamePlural\":\"Credits\",\"currentUsageWithPrecision\":%s,\"usageLimitWithPrecision\":%s,"
                       "\"nextDateReset\":1796083200,\"currentOveragesWithPrecision\":12.5,\"overageCapWithPrecision\":10000,"
                       "\"overageRate\":0.04,\"overageCharges\":0.5,\"bonuses\":%s}]}";
    const char *bonuses = "[{\"bonusType\":\"FIRST_UPGRADE_TRIAL\",\"displayName\":\"Trial credits\",\"currentUsage\":0,\"usageLimit\":500,"
                          "\"expiresAt\":1825966765.739,\"status\":\"ACTIVE\"},{\"bonusType\":\"PROMO\",\"displayName\":\"Old promo\","
                          "\"currentUsage\":10,\"usageLimit\":10,\"status\":\"EXPIRED\"}]";
    char *s = xasprintf(base, "DISABLED", "0.32", "638", bonuses);
    json_t *p = J(s), *r = usage_kiro_parse(p, 1000);
    free(s);
    json_t *limits = json_get(r, "limits");
    CHECK(json_len(limits) == 2);
    CHECK_STR(P(json_at(limits, 0), "id"), "kiro:credits");
    CHECK_STR(P(json_at(limits, 0), "label"), "Kiro Pro credits");
    CHECK_STR(P(json_at(limits, 0), "status"), "ok");
    CHECK(PN(json_at(limits, 0), "amount.used") == 0.32 && PN(json_at(limits, 0), "amount.limit") == 638);
    CHECK(PN(json_at(limits, 0), "amount.remaining") > 637.679 && PN(json_at(limits, 0), "amount.remaining") < 637.681);
    CHECK(PN(json_at(limits, 0), "window.resetsAt") == 1796083200000.0);
    CHECK_STR(P(json_at(limits, 1), "id"), "kiro:bonus:0");
    CHECK_STR(P(json_at(limits, 1), "label"), "Trial credits");
    CHECK_STR(P(json_at(limits, 1), "window.resetLabel"), "expires");
    CHECK(PN(json_at(limits, 1), "window.resetsAt") == 1825966765739.0);
    CHECK_STR(P(r, "metadata.planType"), "Kiro Pro");
    char *dump = json_dump(r, 0);
    CHECK(!strstr(dump, "someone@example.com"));
    free(dump);
    json_free(r);
    json_free(p);

    s = xasprintf(base, "DISABLED", "638", "638", "[]");
    p = J(s);
    r = usage_kiro_parse(p, 1000);
    CHECK(json_len(json_get(r, "limits")) == 1);
    CHECK_STR(P(json_at(json_get(r, "limits"), 0), "status"), "exhausted");
    json_free(r);
    json_free(p);
    free(s);
    s = xasprintf(base, "DISABLED", "638", "638", bonuses);
    p = J(s);
    r = usage_kiro_parse(p, 1000);
    CHECK_STR(P(json_at(json_get(r, "limits"), 0), "status"), "warning");
    CHECK_STR(json_sv(json_at(json_path(json_at(json_get(r, "limits"), 0), "notes"), 0)), "Plan credits used up; drawing on bonus credits");
    json_free(r);
    json_free(p);
    free(s);
    s = xasprintf(base, "ENABLED", "638", "638", "[]");
    p = J(s);
    r = usage_kiro_parse(p, 1000);
    limits = json_get(r, "limits");
    CHECK_STR(P(json_at(limits, 0), "status"), "warning");
    CHECK_STR(P(json_at(limits, 1), "id"), "kiro:overage");
    CHECK_STR(P(json_at(limits, 1), "status"), "ok");
    CHECK_STR(json_sv(json_at(json_path(json_at(limits, 1), "notes"), 0)), "Free credits used up; drawing paid overage");
    CHECK_STR(json_sv(json_at(json_path(json_at(limits, 1), "notes"), 1)), "Overage charges: $0.50 ($0.04 per credit)");
    json_free(r);
    json_free(p);
    free(s);
    /* add-ons: the current list wins over the legacy one; expired ones are skipped */
    p = J("{\"usageBreakdownList\":[{\"resourceType\":\"CREDIT\",\"currentUsage\":5,\"usageLimit\":50,\"addOnMetadata\":{\"addOns\":"
          "[{\"currentUsageWithPrecision\":1.5,\"usageLimitWithPrecision\":100,\"expiresAt\":2000},{\"currentUsage\":0,"
          "\"usageLimit\":10,\"expiresAt\":1}]},\"overageCredits\":[{\"currentUsage\":0,\"usageLimit\":999}]}]}");
    r = usage_kiro_parse(p, 1500 * 1000);
    limits = json_get(r, "limits");
    CHECK(json_len(limits) == 2);
    CHECK_STR(P(json_at(limits, 0), "label"), "Kiro credits");
    CHECK_STR(P(json_at(limits, 1), "id"), "kiro:add-on:0");
    CHECK_STR(P(json_at(limits, 1), "label"), "Add-on credits");
    CHECK(PN(json_at(limits, 1), "amount.limit") == 100 && PN(json_at(limits, 1), "window.resetsAt") == 2000000);
    CHECK(!json_get(r, "metadata"));
    json_free(r);
    json_free(p);
    p = J("{\"usageBreakdownList\":\"nope\"}");
    CHECK(!usage_kiro_parse(p, 1));
    json_free(p);
}

/* compat foldUserAfterToolResult (omp ec588c2710): user text after a tool result folds into it, at every position */
static void test_fold_user(void) {
    json_t *raw = J("{\"id\":\"g4\",\"api\":\"openai-completions\",\"provider\":\"openrouter\",\"baseUrl\":\"u\",\"input\":[\"text\"],"
                    "\"compat\":{\"foldUserAfterToolResult\":true}}");
    ai_model_t *m = catalog_model_variant(NULL, raw, true);
    json_t *msgs = J("[{\"role\":\"user\",\"content\":\"go\",\"timestamp\":0},"
                     "{\"role\":\"assistant\",\"content\":[{\"type\":\"toolCall\",\"id\":\"c1\",\"name\":\"t\",\"arguments\":{}}],"
                     "\"stopReason\":\"toolUse\",\"api\":\"openai-completions\",\"provider\":\"openrouter\",\"model\":\"g4\",\"timestamp\":1},"
                     "{\"role\":\"toolResult\",\"toolCallId\":\"c1\",\"toolName\":\"t\",\"content\":[{\"type\":\"text\",\"text\":\"r\"}],"
                     "\"isError\":false,\"timestamp\":2},{\"role\":\"user\",\"content\":\"more\",\"timestamp\":3},"
                     "{\"role\":\"developer\",\"content\":[{\"type\":\"text\",\"text\":\"note\"}],\"timestamp\":4},"
                     "{\"role\":\"user\",\"content\":\"  \",\"timestamp\":5}]");
    json_t *out = ai_transform_messages(msgs, m);
    CHECK(json_len(out) == 3);
    char *s = json_dump(json_get(json_at(out, 2), "content"), 0);
    CHECK_STR(s, "[{\"type\":\"text\",\"text\":\"r\"},{\"type\":\"text\",\"text\":\"more\"},{\"type\":\"text\",\"text\":\"note\"}]");
    free(s);
    json_free(out);
    json_set(m->raw, "compat", J("{\"foldUserAfterToolResult\":false}"));
    catalog_model_free(m);
    raw = J("{\"id\":\"g4\",\"api\":\"openai-completions\",\"provider\":\"openrouter\",\"baseUrl\":\"u\",\"input\":[\"text\"]}");
    m = catalog_model_variant(NULL, raw, true);
    out = ai_transform_messages(msgs, m);
    CHECK(json_len(out) == 6);
    json_free(out);
    json_free(msgs);
    catalog_model_free(m);
}

int main(void) {
    char tmpl[] = "/tmp/omc-kiro-XXXXXX";
    const char *td = getenv("TMPDIR");
    char *home = td ? xasprintf("%s/omc-kiro-XXXXXX", td) : xstrdup(tmpl);
    if (!mkdtemp(home)) return 2;
    setenv("HOME", home, 1);
    char *agent = xasprintf("%s/agent", home);
    setenv("OMC_CODING_AGENT_DIR", agent, 1);
    test_routing();
    test_codec();
    test_headers();
    test_map_model();
    test_transform();
    test_budget();
    test_usage_parse();
    test_fold_user();
    free(agent);
    free(home);
    printf("test_kiro: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
