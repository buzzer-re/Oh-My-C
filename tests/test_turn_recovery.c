/* turn recovery: AIError classification (flags.ts classifyMessage / retriable, isUnexpectedSocketCloseMessage)
 * and the turn-recovery.ts predicates (isRetryableError, #isMidStreamTransportFailure,
 * classifyResolvedInterruptedToolTurn, handleCommittedTextStreamStall) */
#include "coding/turn_recovery.h"
#include "ai/ai.h"
#include "config/model_resolver.h"
#include "session/session.h"
#include "agent/agent.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

#define SOCK "ECONNRESET: The socket connection was closed unexpectedly. For more information, pass `verbose: true` in the second argument to fetch()"
#define STALL "OpenAI completions stream stalled while waiting for the next event"

/* an errored assistant turn: blocks "t" (text), "c" (tool call), "h" (thinking), in order */
static json_t *turn(const char *err, const char *blocks) {
    json_t *m = json_obj();
    json_set_str(m, "role", "assistant");
    json_set_str(m, "api", "openai-completions");
    json_set_str(m, "provider", "mock");
    json_set_str(m, "stopReason", "error");
    if (err) json_set_str(m, "errorMessage", err);
    json_t *c = json_arr();
    for (const char *p = blocks; *p; p++) {
        json_t *b = json_obj();
        if (*p == 't') {
            json_set_str(b, "type", "text");
            json_set_str(b, "text", "partial answer");
        } else if (*p == 'c') {
            json_set_str(b, "type", "toolCall");
            json_set_str(b, "id", "call_1");
            json_set_str(b, "name", "bash");
            json_set(b, "arguments", json_obj());
        } else {
            json_set_str(b, "type", "thinking");
            json_set_str(b, "thinking", "hmm");
        }
        json_push(c, b);
    }
    json_set(m, "content", c);
    return m;
}

static bool retriable(const char *err) { return aie_retriable(aie_classify_text(err, 0, NULL, NULL)); }

/* manual retry fixtures: "u" user, "c" toolUse turn with call_1, "C" toolUse turn with call_1 + call_2, "e"/"r" errored
 * / ok result of call_1, "E" errored result of call_2, "a" aborted boundary (no calls), "f" error turn with call_1,
 * "s" synthetic result of call_1, "j" async-result delivery, "v" eval-state snapshot, "k" user-invoked /skill: prompt,
 * "d" settled stop turn */
static json_t *msgs_of(const char *spec) {
    json_t *arr = json_arr();
    for (const char *p = spec; *p; p++) {
        json_t *m = json_obj();
        switch (*p) {
        case 'u': json_set_str(m, "role", "user"); json_set_str(m, "content", "run"); break;
        case 'j':
        case 'v':
        case 'k':
            json_set_str(m, "role", "user");
            json_set_str(m, "customType", *p == 'j' ? "async-result" : *p == 'v' ? "eval-state-context" : "skill-prompt");
            if (*p == 'k') json_set_str(m, "attribution", "user");
            json_set_str(m, "content", "x");
            break;
        case 'c':
        case 'C':
        case 'f':
        case 'a':
        case 'd': {
            json_free(m);
            m = turn(NULL, *p == 'a' || *p == 'd' ? "" : "c");
            json_set_str(m, "stopReason", *p == 'f' ? "error" : *p == 'a' ? "aborted" : *p == 'd' ? "stop" : "toolUse");
            if (*p == 'C') {
                json_t *b = json_clone(json_at(json_get(m, "content"), 0));
                json_set_str(b, "id", "call_2");
                json_push(json_get(m, "content"), b);
            }
            break;
        }
        default: { /* e r E s */
            json_set_str(m, "role", "toolResult");
            json_set_str(m, "toolCallId", *p == 'E' ? "call_2" : "call_1");
            json_set_bool(m, "isError", *p != 'r');
            if (*p == 's') {
                json_t *d = json_obj();
                json_set_bool(d, "__synthetic", true);
                json_set(m, "details", d);
            }
        }
        }
        json_push(arr, m);
    }
    return arr;
}

static bool tail_of(const char *spec) {
    json_t *m = msgs_of(spec);
    bool r = tr_aborted_tool_call_tail(m);
    json_free(m);
    return r;
}

static ssize_t replay_of(const char *spec) {
    json_t *m = msgs_of(spec);
    ssize_t r = tr_tool_replay_start(m);
    json_free(m);
    return r;
}

static ssize_t turn_end_of(const char *spec) {
    json_t *m = msgs_of(spec);
    ssize_t r = tr_retryable_turn_end(m);
    json_free(m);
    return r;
}

int main(void) {
    /* isUnexpectedSocketCloseMessage */
    CHECK(aie_socket_close_text(SOCK));
    CHECK(aie_socket_close_text("The socket connection closed unexpectedly"));
    CHECK(aie_socket_close_text("  Error: socket is closed. "));
    CHECK(!aie_socket_close_text("socket is closed by peer"));

    /* AIError.retriable over classifyText */
    CHECK(retriable(SOCK));
    CHECK(retriable(STALL));
    CHECK(retriable("Anthropic stream timed out while waiting for the first event"));
    CHECK(!retriable("OpenAI completions stream closed before a finish_reason was received")); /* text alone */
    CHECK(retriable("503 Service Unavailable"));
    CHECK(retriable("429 Rate limit exceeded for mock-flaky"));
    CHECK(retriable("Couldn't connect to 127.0.0.1:1 (connection refused). Check that the server is running and reachable."));
    CHECK(retriable("DNS lookup failed for api.example.com: Temporary failure in name resolution"));
    CHECK(retriable("connect to 10.0.0.1:443 failed: Network is unreachable"));
    CHECK(retriable("Anthropic stream envelope error: stream ended before message_stop"));
    /* networkFailureMessage over net/http.c's wording; the sentences stay retryable */
    static const struct { const char *in, *url, *out; } nf[] = {
        {"connect to 127.0.0.1:1 failed: Connection refused", "http://127.0.0.1:1/v1/chat",
         "Couldn't connect to 127.0.0.1:1 (connection refused). Check that the server is running and reachable."},
        {"DNS lookup failed for my-llm.lan: Name or service not known", "http://my-llm.lan:11434/v1",
         "Couldn't reach my-llm.lan:11434 (getaddrinfo ENOTFOUND). Check the URL, your network, or proxy."},
        {"DNS lookup failed for api.anthropic.com: Temporary failure in name resolution", "https://api.anthropic.com/v1/messages",
         "No internet connection: couldn't reach api.anthropic.com (getaddrinfo EAI_AGAIN). Check your network or proxy and try again."},
        {"connect to 10.0.0.1:443 failed: Network is unreachable", "https://10.0.0.1/x",
         "No internet connection: couldn't reach 10.0.0.1 (ENETUNREACH). Check your network or proxy and try again."},
        {"TLS certificate verification failed for x.test: self-signed certificate", "https://x.test/v1",
         "TLS certificate verification failed for x.test (self-signed certificate). If you're behind a TLS-inspecting proxy, its CA may need to be trusted."},
        {"connection reset", "https://x.test/v1", "connection reset"},
    };
    for (size_t i = 0; i < sizeof nf / sizeof nf[0]; i++) {
        char *m = ai_network_failure_message(nf[i].in, nf[i].url);
        if (strcmp(m, nf[i].out)) fprintf(stderr, "got: %s\n", m);
        CHECK(!strcmp(m, nf[i].out));
        if (i + 1 < sizeof nf / sizeof nf[0]) CHECK(retriable(m));
        free(m);
    }
    CHECK(!retriable("400 invalid_request_error: messages: field required"));
    CHECK(!retriable("401 Unauthorized"));
    /* transport wording on a terminal 4xx stays terminal */
    CHECK(!aie_retriable(aie_classify_text("unexpected EOF", 400, NULL, NULL)));
    CHECK(aie_retriable(aie_classify_text("unexpected EOF", 0, NULL, NULL)));
    CHECK(!retriable("incomplete: content_filter"));
    /* Gemini's MALFORMED_RESPONSE finish is transient on every transport; other malformed responses stay terminal */
    CHECK(retriable("Generation failed with finish reason: MALFORMED_RESPONSE"));
    CHECK(aie_is(aie_classify_text("Generation failed with finish reason: MALFORMED_RESPONSE", 0, "google-gemini-cli", "google-gemini-cli"),
                 AIE_TRANSIENT));
    CHECK(!aie_is(aie_classify_text("Image API returned a malformed response", 400, NULL, NULL), AIE_TRANSIENT));
    CHECK(!retriable("413 request_too_large: request exceeds the maximum size"));
    CHECK(aie_status_from_text("Request failed with status 502") == 502);
    CHECK(aie_status_from_text("HTTP 404 page") == 404);
    CHECK(aie_status_from_text("no status here") == 0);
    CHECK(aie_is(aie_classify_text("prompt is too long: 210000 tokens > 200000", 400, NULL, NULL), AIE_CONTEXT_OVERFLOW));
    /* a 429/402 worded like an overflow is a throughput cap (Bedrock), not window excess */
    static const char BEDROCK_429[] = "429 Too many tokens, please wait before trying again. (type=rate_limit_error param=rate_limit_exceeded)";
    CHECK(!aie_is(aie_classify_text(BEDROCK_429, 0, NULL, NULL), AIE_CONTEXT_OVERFLOW));
    CHECK(retriable(BEDROCK_429));
    CHECK(!aie_is(aie_classify_text("Too many tokens, please wait before trying again.", 429, NULL, NULL), AIE_CONTEXT_OVERFLOW));
    CHECK(!aie_is(aie_classify_text("Too many tokens in this request", 402, NULL, NULL), AIE_CONTEXT_OVERFLOW));
    CHECK(aie_is(aie_classify_text("Too many tokens in this request", 400, NULL, NULL), AIE_CONTEXT_OVERFLOW));
    CHECK(aie_is(aie_classify_text("Too many tokens in this request", 0, NULL, NULL), AIE_CONTEXT_OVERFLOW));

    /* #isMidStreamTransportFailure: stall / premature close / envelope truncation; not a socket close */
    json_t *m = turn(STALL, "t");
    CHECK(tr_mid_stream_transport_failure(m, aie_classify_message(m), false));
    json_free(m);
    m = turn("OpenAI completions stream closed before a finish_reason was received", "t");
    CHECK(tr_mid_stream_transport_failure(m, aie_classify_message(m), false));
    CHECK(!tr_mid_stream_transport_failure(m, aie_classify_message(m), true)); /* a user abort owns the stream */
    json_free(m);
    m = turn(SOCK, "t");
    CHECK(!tr_mid_stream_transport_failure(m, aie_classify_message(m), false));
    json_free(m);

    /* isRetryableError: committed text is never replayed; buffered text (print mode, subagents) and thinking are */
    m = turn(STALL, "t");
    CHECK(!tr_is_retryable_error(m, true));
    CHECK(tr_is_retryable_error(m, false));
    json_free(m);
    m = turn(STALL, "h");
    CHECK(tr_is_retryable_error(m, true));
    json_free(m);
    m = turn(STALL, "");
    CHECK(tr_is_retryable_error(m, true));
    json_free(m);
    m = turn("prompt is too long", "");
    CHECK(!tr_is_retryable_error(m, true)); /* overflow: compaction, not retry */
    json_free(m);
    /* tool calls paired with synthetic `executed: false` results are replay-safe for a retriable error */
    m = turn(SOCK, "c");
    CHECK(tr_is_retryable_error(m, true));
    CHECK(tr_unexecuted_tool_calls_replay_safe(m, true, 0));
    CHECK(!tr_unexecuted_tool_calls_replay_safe(m, true, 1));
    json_free(m);
    m = turn(SOCK, "tc");
    CHECK(!tr_is_retryable_error(m, true)); /* committed text beside the call */
    json_free(m);

    /* classifyResolvedInterruptedToolTurn == "stream-stall" */
    m = turn(STALL, "tc");
    CHECK(tr_resolved_interrupted_tool_turn(m, false));
    json_free(m);
    m = turn(SOCK, "c");
    CHECK(!tr_resolved_interrupted_tool_turn(m, false)); /* a socket close is retried through isRetryableError */
    json_free(m);
    m = turn(STALL, "t");
    CHECK(!tr_resolved_interrupted_tool_turn(m, false));
    json_free(m);

    /* handleCommittedTextStreamStall eligibility */
    bool reset;
    m = turn(SOCK, "t");
    CHECK(tr_committed_text_stall(m, true, false, &reset) && !reset);
    CHECK(!tr_committed_text_stall(m, false, false, &reset)); /* buffered text replays instead */
    json_free(m);
    m = turn(STALL, "ht");
    CHECK(tr_committed_text_stall(m, true, false, &reset));
    json_free(m);
    m = turn(STALL, "tc");
    CHECK(!tr_committed_text_stall(m, true, false, &reset) && !reset); /* tool turns resume instead */
    json_free(m);
    m = turn(STALL, "h");
    CHECK(!tr_committed_text_stall(m, true, false, &reset)); /* no text shown */
    json_free(m);
    m = turn("400 invalid_request_error: bad", "t");
    CHECK(!tr_committed_text_stall(m, true, false, &reset) && reset);
    json_free(m);

    json_t *r = tr_stream_stall_reminder(2);
    CHECK(str_eq(json_get_str(r, "role"), "developer"));
    CHECK(str_eq(json_get_str(r, "attribution"), "agent"));
    const char *txt = json_get_str(json_at(json_get(r, "content"), 0), "text");
    CHECK(txt && strstr(txt, "The provider connection dropped mid-response") && strstr(txt, "Attempt #2/3"));
    json_free(r);

    /* isBillingCapUsageLimit (rate-limit-utils.test.ts) */
    const char *lithos = "402 the organization is out of credit, add more to continue (type=insufficient_quota param=insufficient_quota)";
    CHECK(tr_billing_cap_usage_limit(402, lithos));
    CHECK(tr_billing_cap_usage_limit(0, lithos));
    CHECK(tr_billing_cap_usage_limit(429, "Insufficient balance in account"));
    /* omp 3efa3bab9e: a structured INSUFFICIENT_G1_CREDITS_BALANCE is a billing cap */
    CHECK(tr_billing_cap_usage_limit(429, "{\"error\":{\"status\":\"RESOURCE_EXHAUSTED\",\"message\":\"Resource exhausted\","
                                          "\"details\":[{\"@type\":\"type.googleapis.com/google.rpc.ErrorInfo\","
                                          "\"reason\":\"INSUFFICIENT_G1_CREDITS_BALANCE\"}]}}"));
    /* a zero x-ratelimit-limit-* allowance is a billing wall: no wait or replay can succeed (omp 03a02d0926) */
    const char *zero = "429 Rate limit exceeded (x-ratelimit-limit-req-minute: 0 \xe2\x80\x94 this plan grants no allowance "
                       "for the model; switch models or upgrade the plan)";
    CHECK(tr_billing_cap_usage_limit(429, zero));
    CHECK(ai_usage_limit_text(zero, 429));
    CHECK(str_eq(mr_rate_limit_reason(zero), "QUOTA_EXHAUSTED"));
    CHECK(!ai_zero_allowance_text("x-ratelimit-limit-req-minute: 0.5"));
    CHECK(!ai_zero_allowance_text("x-ratelimit-limit-req-minute: 10"));
    CHECK(!ai_zero_allowance_text("myx-ratelimit-limit-requests: 0"));
    CHECK(ai_zero_allowance_text("X-RateLimit-Limit-Requests:0"));
    CHECK(tr_billing_cap_usage_limit(429, "OpenRouter: Insufficient credits"));
    CHECK(tr_billing_cap_usage_limit(403, "Your monthly spend limit has been reached"));
    CHECK(!tr_billing_cap_usage_limit(429, "429 You exceeded your current quota, please check your plan and billing details. For details, see: "
                                           "https://help.aliyun.com/zh/model-studio/error-code#token-limit"));
    CHECK(!tr_billing_cap_usage_limit(429, "Rate limit exceeded, too many requests"));
    CHECK(!tr_billing_cap_usage_limit(402, "A subscription is required for this endpoint"));
    CHECK(!tr_billing_cap_usage_limit(0, "503 service unavailable"));
    CHECK(!tr_billing_cap_usage_limit(0, NULL));

    /* manual retry tail rules (agent-session-manual-retry.test.ts) */
    CHECK(tail_of("ucea"));   /* Esc during tool execution: errored result right before the boundary */
    CHECK(!tail_of("ucra"));  /* the tool completed; the abort only killed the next model call */
    CHECK(!tail_of("ua"));    /* abort with no tool activity */
    CHECK(tail_of("ufs"));    /* a stream that died mid-tool-call: synthetic results trail the failed turn */
    CHECK(tail_of("uceaj"));  /* a background-job result folded in after the interrupt */
    CHECK(!tail_of("ucerd")); /* a settled turn */
    CHECK(turn_end_of("ufs") == 2 && turn_end_of("ucea") == 4 && turn_end_of("uceaj") == 4);
    CHECK(turn_end_of("uf") == 2 && turn_end_of("ud") == -1 && turn_end_of("ucer") == -1);
    CHECK(turn_end_of("ufk") == -1); /* a user-invoked /skill: prompt is conversation, not a trailing delivery */
    CHECK(replay_of("ucea") == 2);   /* the failed batch, behind the aborted boundary */
    CHECK(replay_of("uce") == 2);    /* a restored context without the boundary */
    CHECK(replay_of("uceav") == 2);  /* eval snapshots go with the strip */
    CHECK(replay_of("uceaj") == -1); /* a delivery stays in context: a fresh model call instead */
    CHECK(replay_of("ufs") == -1);   /* placeholders anchor on the failed turn itself */
    CHECK(replay_of("ucra") == -1);  /* a completed call never re-runs */
    CHECK(replay_of("uCeEa") == 2 && replay_of("uCera") == -1 && replay_of("uCea") == -1);
    CHECK(replay_of("ua") == -1 && replay_of("uf") == -1);
    { /* a failed turn of its own calls is no boundary */
        json_t *own = msgs_of("uce");
        json_t *b = turn(NULL, "c");
        json_set_str(b, "stopReason", "aborted");
        json_push(own, b);
        CHECK(tr_tool_replay_start(own) == -1);
        json_free(own);
    }

    { /* a retry-fallback hop's own thinking level does not outlive it on resume (omp ca42dc150a) */
        json_t *path = json_parse_str("[{\"type\":\"model_change\",\"model\":\"p/a\"},"
                                      "{\"type\":\"thinking_level_change\",\"thinkingLevel\":\"high\"},"
                                      "{\"type\":\"model_change\",\"model\":\"p/b\",\"resolvedModelIsFallback\":true},"
                                      "{\"type\":\"thinking_level_change\",\"thinkingLevel\":\"xhigh\"}]");
        session_context_t c;
        session_build_context_for(path, &c);
        CHECK(c.thinking && !strcmp(c.thinking, "high"));
        session_context_free(&c);
        /* a level changed after the hop is kept */
        json_push(path, json_parse_str("{\"type\":\"custom\",\"customType\":\"x\"}"));
        json_push(path, json_parse_str("{\"type\":\"thinking_level_change\",\"thinkingLevel\":\"low\"}"));
        session_build_context_for(path, &c);
        CHECK(c.thinking && !strcmp(c.thinking, "low"));
        session_context_free(&c);
        json_free(path);
        /* a non-fallback model change drops the snapshot */
        path = json_parse_str("[{\"type\":\"thinking_level_change\",\"thinkingLevel\":\"high\"},"
                              "{\"type\":\"model_change\",\"model\":\"p/b\",\"resolvedModelIsFallback\":true},"
                              "{\"type\":\"model_change\",\"model\":\"p/c\"},"
                              "{\"type\":\"thinking_level_change\",\"thinkingLevel\":\"medium\"}]");
        session_build_context_for(path, &c);
        CHECK(c.thinking && !strcmp(c.thinking, "medium"));
        session_context_free(&c);
        json_free(path);
    }

    /* replay-policy.ts toProviderReplayMessage: refusals dropped, provider-error placeholders lose the error text */
    {
        json_t *msgs = json_parse_str(
            "[{\"role\":\"user\",\"content\":\"hi\"},"
            "{\"role\":\"assistant\",\"stopReason\":\"error\",\"stopDetails\":{\"type\":\"refusal\"},"
            "\"errorMessage\":\"refused\",\"content\":[{\"type\":\"toolCall\",\"id\":\"c1\",\"name\":\"read\"}]},"
            "{\"role\":\"toolResult\",\"toolCallId\":\"c1\",\"toolName\":\"read\",\"isError\":true,"
            "\"content\":[{\"type\":\"text\",\"text\":\"Tool call was not executed because the provider stream ended with "
            "an error before the tool could run: refused\"}],"
            "\"details\":{\"__synthetic\":true,\"source\":\"assistant_stop_error\",\"upstreamError\":\"refused\"}},"
            "{\"role\":\"assistant\",\"stopReason\":\"error\",\"errorMessage\":\"boom\",\"content\":[]}]");
        json_t *rp = agent_provider_replay(msgs);
        CHECK(rp && json_len(rp) == 3);
        if (rp && json_len(rp) == 3) {
            CHECK(!strcmp(json_get_str(json_at(rp, 1), "role"), "toolResult"));
            CHECK(!strcmp(json_get_str(json_at(json_get(json_at(rp, 1), "content"), 0), "text"),
                          "Tool call was not executed because the provider stream ended with an error before the tool could run."));
            CHECK(!strcmp(json_get_str(json_get(json_at(rp, 1), "details"), "upstreamError"), "refused"));
            CHECK(!strcmp(json_get_str(json_at(rp, 2), "errorMessage"), "boom")); /* a plain error turn stays */
        }
        json_free(rp);
        json_t *plain = json_parse_str("[{\"role\":\"user\",\"content\":\"hi\"}]");
        CHECK(agent_provider_replay(plain) == NULL);
        json_free(plain);
        json_free(msgs);
    }

    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails != 0;
}
