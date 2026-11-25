/* turn_recovery.c - omp turn-recovery.ts (mid-stream failure recovery) and AIError classification
 * (packages/ai/src/error/flags.ts classifyText/classifyMessage/retriable, pi-utils fetch-retry.ts).
 * The patterns are omp's regular expressions verbatim (one lookahead rewritten as a group; equivalent for a
 * match test). Usage-limit wording uses the transport's matcher (ai_usage_limit_text, retry.c), and the
 * rate-limit reason comes from mr_rate_limit_reason; both are approximations of rate-limit.ts. */
#include "coding/turn_recovery.h"

#include "agent/agent.h"
#include "ai/ai.h"
#include "config/model_resolver.h"
#include "prompt/template.h"
#include "text/regex.h"

#include <ctype.h>
#include <pthread.h>

enum {
    P_SOCK_CLOSE,
    P_SOCK_IS_CLOSED,
    P_STREAM_STALL,
    P_HTTP2_RESET,
    P_PREMATURE_CLOSE,
    P_TIMEOUT,
    P_ENVELOPE,
    P_ENVELOPE_TRUNC,
    P_STREAM_READ,
    P_PY_H2_RESET,
    P_PY_INCOMPLETE_CHUNK,
    P_CODEX_BODY_READ,
    P_TRANSIENT_TRANSPORT,
    P_OMC_TRANSPORT,
    P_AUTH,
    P_MALFORMED,
    P_PROVIDER_FINISH,
    P_EMPTY_RESPONSE,
    P_MALFORMED_RESPONSE_FINISH,
    P_CONTENT_FILTER,
    P_ACCOUNT_POLICY,
    P_ANTHROPIC_ACCOUNT_POLICY,
    P_CURSOR_MARKER,
    P_CURSOR_PLAN,
    P_STALE1,
    P_STALE2,
    P_STALE_DETAIL,
    P_LLAMA_PARSE,
    P_GENERATION_NAN,
    P_ALIYUN,
    P_STREAM_PARSE_DIAG,
    P_STREAM_DROP,
    P_OVERFLOW_NO_BODY,
    P_GENERIC_LIMIT,
    P_DASHSCOPE_DOC, /* isBillingCapUsageLimit (rate-limit.ts) */
    P_DASHSCOPE_MSG,
    P_STATUS_402_QUOTA,
    P_BILLING_CODE,  /* is402BillingCapBody's machine fields (omp 34c8e4ecb1) */
    P_BILLING_FIELD,
    P_OUT_OF_CREDIT,
    P_INSUFFICIENT_BALANCE,
    P_CREDITS_EXHAUSTED,
    P_PREPAID_EXHAUSTED,
    P_SPEND_LIMIT,
    P_CREDITS_REQUIRED,
    P_INSUFFICIENT_QUOTA,
    P_STATUS0, /* STATUS_MESSAGE_PATTERNS (5) */
    P_STATUS4 = P_STATUS0 + 4,
    P_PAYLOAD0, /* PAYLOAD_REJECTION_PATTERNS (5) */
    P_PAYLOAD4 = P_PAYLOAD0 + 4,
    P_OVERFLOW0, /* CONTEXT_OVERFLOW_EVIDENCE_PATTERNS (26) */
    P_OVERFLOW_LAST = P_OVERFLOW0 + 25,
    P_COUNT
};

static const struct {
    int id;
    int flags;
    const char *re;
} PATS[] = {
    {P_SOCK_CLOSE, RX_ICASE, "\\b(?:the\\s+)?socket connection (?:was )?closed unexpectedly\\b"},
    {P_SOCK_IS_CLOSED, RX_ICASE, "^(?:error:\\s*)?socket is closed\\.?$"},
    {P_STREAM_STALL, RX_ICASE, "stream stall"},
    {P_HTTP2_RESET, RX_ICASE,
     "stream closed with error code\\s+nghttp2_(?:internal_error|refused_stream)|nghttp2_(?:internal_error|refused_stream)|"
     "HTTP2(?:StreamReset|RefusedStream)"},
    {P_PREMATURE_CLOSE, RX_ICASE,
     "(?:stream closed before a (?:finish_reason|terminal response event)|Codex stream ended before terminal completion "
     "event|Cursor stream ended before turnEnded)"},
    /* omp f241b9824f: "stream stalled" / "stalls" / "stalling" are timeouts too, so a stalled call fails over */
    {P_TIMEOUT, RX_ICASE, "\\b(?:operation\\s+)?timed?\\s*out\\b|\\btimeout\\b|\\bstream stall(?:ed|s|ing)?\\b"},
    {P_ENVELOPE, RX_ICASE, "anthropic stream envelope error:"},
    {P_ENVELOPE_TRUNC, RX_ICASE, "before message_(?:start|delta|stop)"},
    {P_STREAM_READ, RX_ICASE, "stream[_ -]?read[_ -]?error"},
    {P_PY_H2_RESET, 0, "<StreamReset stream_id:\\d+, error_code:(?:2|7), remote_reset:True>"},
    {P_PY_INCOMPLETE_CHUNK, 0,
     "peer closed connection without sending complete message body \\(incomplete chunked read\\)"},
    {P_CODEX_BODY_READ, RX_ICASE, "\\btransport error reading codex response body\\b"},
    {P_TRANSIENT_TRANSPORT, RX_ICASE,
     "\\b(?:no[_ -]?capacity|(?:high|peak)[ _-]?demand|(?:at|over|insufficient)[ _-]?capacity|capacity[ _-]?(?:exceeded|"
     "exhausted)|peak[ _-]?load)\\b|overloaded|provider.?returned.?error|rate.?limit|too many requests|"
     "auth-gateway\\s+5\\d{2}(?:[:\\s]|$)|\\b(?:429|500|502|503|504)\\b|service.?unavailable|temporar(?:y|ily)[ _-]?"
     "unavailable|server.?error|internal.?error|retry your request|network.?error|connection.?error|connection.?refused|"
     "unable.?to.?connect\\.\\s*is the computer able to access the url\\?|other side closed|use of closed network "
     "connection|fetch failed|upstream.?connect|upstream.?request.?failed|reset before headers|socket hang up|timed? out|"
     "timeout|terminated|retry delay|stream stall|no error details in response|HTTP2(?:StreamReset|RefusedStream|"
     "EnhanceYourCalm)|nghttp2_(?:internal_error|refused_stream)|stream closed with error code nghttp2_(?:internal_error|"
     "refused_stream)|\\bE(?:TIMEOUT|NOTFOUND|NETUNREACH|HOSTUNREACH)\\b|\\bFailedToOpenSocket\\b|no internet connection|"
     "couldn.?t reach|couldn.?t connect to|tls certificate verification failed|malformed.?function.?call"},
    /* omc's own connection-level wording (net/http.c) for the failures Bun reports as ConnectionRefused,
     * FailedToOpenSocket, ETIMEOUT, ENOTFOUND, ... (all transient through networkFailureMessage) */
    {P_OMC_TRANSPORT, RX_ICASE,
     "^(?:DNS lookup failed for |connect to \\S+ failed: |TLS handshake (?:with \\S+ failed|timed out)|connection closed "
     "before response$|connection failed$|failed to send request$|proxy CONNECT)"},
    {P_AUTH, RX_ICASE,
     "\\b(?:401|403|unauthorized|forbidden|authentication|auth[_ ]?unavailable|no auth available|(?:invalid|no)[_ ]?api[_ ]"
     "?key)\\b"},
    {P_MALFORMED, RX_ICASE, "\\b(?:malformed[_. ]?(?:function[_. ]?call|response)|unexpected[_. ]?tool[_. ]?call)\\b"},
    {P_PROVIDER_FINISH, RX_ICASE, "\\bProvider (?:returned error finish_reason|finish_reason:\\s*error)\\b"},
    {P_EMPTY_RESPONSE, RX_ICASE, "\\bthought-only response without final output\\b"},
    /* Gemini's MALFORMED_RESPONSE finish is a backend fault: the same request can succeed. Kept narrow so other
     * "malformed response" errors (e.g. an image API's) stay terminal */
    {P_MALFORMED_RESPONSE_FINISH, RX_ICASE, "\\bfinish reason:\\s*MALFORMED_RESPONSE\\b"},
    {P_CONTENT_FILTER, RX_ICASE, "\\b(?:incomplete:\\s*)?content_filter\\b"},
    {P_ACCOUNT_POLICY, RX_ICASE, "\\bcyber_policy\\b|trusted access for cyber"},
    {P_ANTHROPIC_ACCOUNT_POLICY, RX_ICASE,
     "\\b(?:oauth_not_allowed_for_organization|permission_error)\\b|\\bOAuth authentication is currently not allowed for "
     "this organization\\b"},
    {P_CURSOR_MARKER, RX_ICASE, "\\bERROR_RATE_LIMITED_CHANGEABLE\\b"},
    {P_CURSOR_PLAN, RX_ICASE, "\\bNamed models unavailable\\b|\\bModel unavailable on\\b|\\bFree plans can only use\\b"},
    {P_STALE1, RX_ICASE, "\\bItem with id ['\"][^'\"]+['\"] not found\\.?"},
    {P_STALE2, RX_ICASE, "previous[ _]?response"},
    {P_STALE_DETAIL, RX_ICASE, "not[ _]?found|invalid|expired|stale|zero[ _-]?data[ _-]?retention"},
    {P_LLAMA_PARSE, RX_ICASE, "failed to parse tool call arguments as json|\\[json\\.exception\\.parse_error\\.101\\]"},
    {P_GENERATION_NAN, RX_ICASE | RX_DOTALL, "floating[ _-]?point nan\\b.*\\bdetected in generation"},
    {P_ALIYUN, RX_ICASE, "provided url does not appear to be valid|download multimodal file timed out"},
    {P_STREAM_PARSE_DIAG, RX_ICASE,
     "(?:json parse error:\\s*(?:unterminated string|unexpected end of json input|unexpected end of data|unexpected eof|"
     "end of file|eof while parsing|truncated)|json\\.parse:\\s*(?:unterminated string|unexpected end of data)|unexpected "
     "end of json input|unexpected eof|eof while parsing)"},
    {P_STREAM_DROP, RX_ICASE,
     "stream disconnected before completion|(?:stream|websocket) closed before response\\.completed|stream (?:was )?"
     "interrupted|stream ended before (?:terminal (?:chunk|completion event)|message_(?:start|delta|stop))|socket "
     "disconnected before secure tls connection"},
    {P_OVERFLOW_NO_BODY, RX_ICASE, "\\b4(00|13)\\s*(status code)?\\s*\\(no body\\)"},
    {P_GENERIC_LIMIT, RX_ICASE, "exceeds the limit of \\d+"},
    {P_DASHSCOPE_DOC, RX_ICASE, "error-code[^()\\s]*#token-limit"},
    {P_DASHSCOPE_MSG, RX_ICASE, "\\byou exceeded your current quota, please check your plan and billing details\\b"},
    {P_STATUS_402_QUOTA, RX_ICASE,
     "\\b(?:payment(?:\\s+is)?[-_.\\s]*required|deactivated_workspace|insufficient.?(?:balance|credits?|account.?funds)|"
     "requires?\\s+more\\s+credits|billing[-_.\\s]*(?:verification|error|issue)|can only afford)\\b"},
    {P_BILLING_CODE, RX_ICASE, "^billing_(?:error|not_configured)$"},
    {P_BILLING_FIELD, RX_ICASE, "\\b(?:type|code|param)=billing_(?:error|not_configured)(?:[\\s)]|$)"},
    {P_OUT_OF_CREDIT, RX_ICASE, "\\bout of credit|\\badd (?:more )?credit|\\btop ?up\\b"},
    {P_INSUFFICIENT_BALANCE, RX_ICASE, "insufficient.?balance"},
    {P_CREDITS_EXHAUSTED, RX_ICASE, "\\b(?:exceed\\w*|insufficient|not enough)\\b[^\\n]{0,40}\\bcredits?\\b|\\bcredits?\\b[^\\n]{0,40}\\b(?:exhausted|depleted)\\b"},
    {P_PREPAID_EXHAUSTED, RX_ICASE, "\\busage_pricing_required\\b|\\bprepaid balance\\b[^\\n]{0,40}\\b(?:used up|exhausted|depleted)\\b"},
    {P_SPEND_LIMIT, RX_ICASE, "spend(?:ing)?[\\s_-]?(?:limit|cap)\\b"},
    {P_CREDITS_REQUIRED, RX_ICASE, "\\busage credits are required\\b|\\bcredits_required\\b"},
    {P_INSUFFICIENT_QUOTA, RX_ICASE, "\\binsufficient_quota\\b"},
    {P_STATUS0 + 0, RX_ICASE, "\\bstatus(?:_code)?[:=]\\s*(\\d{3})\\b"},
    {P_STATUS0 + 1, RX_ICASE, "\\bstatus\\s+(\\d{3})\\b"},
    {P_STATUS0 + 2, RX_ICASE, "\\bHTTP\\s+(\\d{3})\\b"},
    {P_STATUS0 + 3, RX_ICASE, "\\b(?:error|failed)\\s*[:=]?\\s*(\\d{3})\\b"},
    {P_STATUS0 + 4, 0, "(?:^|\\s)(\\d{3})\\s+(?:[A-Z][a-z]+(?:\\s+[A-Z][a-z]+)*)"},
    {P_PAYLOAD0 + 0, RX_ICASE, "\\b413\\s*(?:status code\\s*)?\\(no body\\)"},
    {P_PAYLOAD0 + 1, RX_ICASE, "\\b413\\b[^.\\n]{0,120}\\b(?:request|payload|entity|body)\\b[^.\\n]{0,60}\\b(?:exceed|too large|limit)"},
    {P_PAYLOAD0 + 2, RX_ICASE, "request_too_large"},
    {P_PAYLOAD0 + 3, RX_ICASE, "(?:payload|entity) too large"},
    {P_PAYLOAD0 + 4, RX_ICASE, "request exceeds the maximum (?:size|number of bytes)"},
    {P_OVERFLOW0 + 0, RX_ICASE, "prompt is too long"},
    {P_OVERFLOW0 + 1, RX_ICASE, "input is too long for requested model"},
    {P_OVERFLOW0 + 2, RX_ICASE, "exceeds the context window"},
    {P_OVERFLOW0 + 3, RX_ICASE, "input token count.*exceeds the maximum"},
    {P_OVERFLOW0 + 4, RX_ICASE, "maximum prompt length is \\d+"},
    {P_OVERFLOW0 + 5, RX_ICASE, "reduce the length of the messages"},
    {P_OVERFLOW0 + 6, RX_ICASE, "maximum context length is \\d+ tokens"},
    {P_OVERFLOW0 + 7, RX_ICASE, "exceeds the available context size"},
    {P_OVERFLOW0 + 8, RX_ICASE,
     "\\bprompt\\s*\\(\\s*\\d+\\s+tokens\\s*\\)\\s*\\+\\s*max\\s+tokens\\s*\\(\\s*\\d+\\s*\\)\\s+exceeds\\s+the\\s+context"
     "\\s*\\(\\s*\\d+\\s*\\)"},
    {P_OVERFLOW0 + 9, RX_ICASE,
     "\\bprompt\\s*\\(\\s*\\d+\\s+tokens\\s*\\)\\s+leaves\\s+no\\s+room\\s+to\\s+answer\\s+in\\s+the\\s+context\\s*\\(\\s*"
     "\\d+\\s*\\)"},
    {P_OVERFLOW0 + 10, RX_ICASE, "requested tokens?.*exceed.*context (window|length|size)"},
    {P_OVERFLOW0 + 11, RX_ICASE, "context (window|length|size).*(exceeded|overflow|too small)"},
    {P_OVERFLOW0 + 12, RX_ICASE, "(prompt|input).*(too long|too large).*(context|n_ctx)"},
    {P_OVERFLOW0 + 13, RX_ICASE, "requested tokens?.*(exceeds?|greater than).*(n_ctx|context)"},
    {P_OVERFLOW0 + 14, RX_ICASE, "greater than the context length"},
    {P_OVERFLOW0 + 15, RX_ICASE, "context window exceeds limit"},
    {P_OVERFLOW0 + 16, RX_ICASE, "exceeded model token limit"},
    {P_OVERFLOW0 + 17, RX_ICASE, "context[_ ]length[_ ]exceeded"},
    {P_OVERFLOW0 + 18, RX_ICASE, "too many tokens"},
    {P_OVERFLOW0 + 19, RX_ICASE, "token limit exceeded"},
    {P_OVERFLOW0 + 20, RX_ICASE, "request_too_large[^\\n]*\\btokens?\\b"},
    {P_OVERFLOW0 + 21, RX_ICASE, "\\btokens?\\b[^\\n]*request_too_large"},
    {P_OVERFLOW0 + 22, RX_ICASE, "model_context_window_exceeded"},
    {P_OVERFLOW0 + 23, RX_ICASE, "prompt filled the context window"},
    {P_OVERFLOW0 + 24, RX_ICASE, "exceeds the limit of \\d+ tokens?\\b"},
    {P_OVERFLOW0 + 25, RX_ICASE, "chat history exceeds the \\d+-message limit"},
};

static rx_t *g_rx[P_COUNT];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void compile_all(void) {
    for (size_t i = 0; i < ARRAY_LEN(PATS); i++) {
        char *err = NULL;
        g_rx[PATS[i].id] = rx_compile(PATS[i].re, PATS[i].flags, &err);
        if (!g_rx[PATS[i].id]) LOGW("turn_recovery: pattern %d: %s", PATS[i].id, err ? err : "?");
        free(err);
    }
}

static bool m(int id, const char *s) {
    pthread_once(&g_once, compile_all);
    return s && g_rx[id] && rx_is_match(g_rx[id], s, strlen(s), 0);
}

bool aie_socket_close_text(const char *err) {
    if (!err) return false;
    if (m(P_SOCK_CLOSE, err)) return true;
    while (isspace((unsigned char)*err)) err++;
    size_t n = strlen(err);
    while (n && isspace((unsigned char)err[n - 1])) n--;
    char *t = xstrndup(err, n);
    bool r = m(P_SOCK_IS_CLOSED, t);
    free(t);
    return r;
}

int aie_status_from_text(const char *err) {
    if (!err) return 0;
    pthread_once(&g_once, compile_all);
    for (int i = P_STATUS0; i <= P_STATUS4; i++) {
        rx_match_t mm;
        if (!g_rx[i] || !rx_search(g_rx[i], err, strlen(err), 0, &mm) || mm.ngroups < 2 || mm.groups[1].start < 0) continue;
        int code = atoi(err + mm.groups[1].start);
        if (code >= 100 && code <= 599) return code;
    }
    return 0;
}

static bool token_overflow_evidence(const char *s) {
    for (int i = P_OVERFLOW0; i <= P_OVERFLOW_LAST; i++)
        if (m(i, s)) return true;
    return false;
}

static bool payload_rejection_text(const char *s) {
    bool any = false;
    for (int i = P_PAYLOAD0; i <= P_PAYLOAD4 && !any; i++) any = m(i, s);
    return any && !token_overflow_evidence(s);
}

static bool overflow_text(const char *s) {
    return token_overflow_evidence(s) || m(P_GENERIC_LIMIT, s) || m(P_OVERFLOW_NO_BODY, s);
}

static bool envelope_truncation_text(const char *s) { return m(P_ENVELOPE, s) && m(P_ENVELOPE_TRUNC, s); }

static bool transient_text(const char *s) {
    return aie_socket_close_text(s) || m(P_STREAM_READ, s) || m(P_PY_H2_RESET, s) || m(P_PY_INCOMPLETE_CHUNK, s) ||
           envelope_truncation_text(s) || m(P_TRANSIENT_TRANSPORT, s) || m(P_OMC_TRANSPORT, s);
}

static bool codex_steer_text(const char *api, const char *err) {
    return str_eq(api, "openai-codex-responses") && err && strstr(err, "code=unsupported_native_inflight_message");
}

bool aie_codex_steer_rejection(const json_t *msg) {
    return codex_steer_text(json_get_str(msg, "api"), json_get_str(msg, "errorMessage"));
}

static bool word_ch(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* `w` as a whole word at s+i (\bw\b, ASCII case-insensitive): its length, else 0 */
static size_t word_at(const char *s, size_t i, const char *w) {
    size_t n = strlen(w);
    if ((i > 0 && word_ch(s[i - 1])) || strncasecmp(s + i, w, n) || word_ch(s[i + n])) return 0;
    return n;
}

/* rate-limit.ts isOpaqueStatusBody, Latin part (omc does not classify the Han quota wording): with 429/402, HTTP framing
 * words and "(no body)" removed, no run of three letters or digits is left */
static bool opaque_status_body(const char *s) {
    static const char *const frame[] = {"429", "402", "http", "https", "status", "error", "code", "response", "message"};
    sb_t c = SB_INIT;
    for (size_t i = 0; s[i];) {
        size_t skip = 0;
        for (size_t k = 0; k < ARRAY_LEN(frame) && !skip; k++) skip = word_at(s, i, frame[k]);
        if (!skip) { /* \(?\bno body\b\)? */
            size_t at = s[i] == '(' ? i + 1 : i;
            size_t nb = word_at(s, at, "no body");
            if (nb) skip = at - i + nb + (s[at + nb] == ')');
        }
        if (skip) i += skip;
        else sb_putc(&c, s[i++]);
    }
    int run = 0;
    bool opaque = true;
    for (size_t i = 0; i < c.len && opaque; i++) {
        run = isalnum((unsigned char)c.s[i]) ? run + 1 : 0;
        opaque = run < 3;
    }
    sb_free(&c);
    return opaque;
}

/* is402BillingCapBody's structured arm: the JSON object embedded in the message (parseJsonBody: first `{` to last `}`)
 * names billing_error / billing_not_configured as its error's (or its own) type, code or param */
static bool billing_error_json(const char *msg) {
    const char *open = strchr(msg, '{'), *close = strrchr(msg, '}');
    if (!open || !close || close < open) return false;
    json_t *body = json_parse(open, (size_t)(close - open) + 1, NULL);
    bool hit = false;
    if (json_is_obj(body)) {
        const json_t *err = json_get_obj(body, "error");
        if (!err) err = body;
        static const char *const keys[] = {"type", "code", "param"};
        for (size_t i = 0; i < ARRAY_LEN(keys) && !hit; i++) {
            const json_t *v = json_get(err, keys[i]);
            hit = json_is_str(v) && m(P_BILLING_CODE, json_sv(v));
        }
    }
    json_free(body);
    return hit;
}

/* rate-limit.ts is402BillingCapBody: a 402 that is opaque, names a billing failure (machine fields in prose or JSON,
 * omp 34c8e4ecb1), or is payment/balance/credits-worded, or a quota-exhausted / concurrency reason */
bool tr_402_billing_cap_body(const char *msg) {
    if (!msg || opaque_status_body(msg)) return true;
    if (m(P_BILLING_CODE, msg) || m(P_BILLING_FIELD, msg) || billing_error_json(msg)) return true;
    if (m(P_STATUS_402_QUOTA, msg)) return true;
    const char *reason = mr_rate_limit_reason(msg);
    return str_eq(reason, "QUOTA_EXHAUSTED") || str_eq(reason, "CONCURRENT_LIMIT");
}

/* rate-limit.ts isBillingCapUsageLimit: topping the account up clears the error, waiting never does */
bool tr_billing_cap_usage_limit(int status, const char *msg) {
    if (!msg) return false;
    const char *reason = mr_rate_limit_reason(msg);
    if (str_eq(reason, "CONCURRENT_LIMIT") && status != 402) return false; /* isConcurrencyCapExclusion */
    if (m(P_DASHSCOPE_DOC, msg) && m(P_DASHSCOPE_MSG, msg)) return false;  /* isDashScopeTokenLimitText */
    if (status == 402) return tr_402_billing_cap_body(msg);
    /* parseGoogleRpcRateLimitReason: a RESOURCE_EXHAUSTED ErrorInfo reason INSUFFICIENT_G1_CREDITS_BALANCE */
    if (strstr(msg, "RESOURCE_EXHAUSTED") && strstr(msg, "INSUFFICIENT_G1_CREDITS_BALANCE")) return true;
    return m(P_OUT_OF_CREDIT, msg) || m(P_INSUFFICIENT_BALANCE, msg) || m(P_CREDITS_EXHAUSTED, msg) ||
           m(P_PREPAID_EXHAUSTED, msg) || m(P_SPEND_LIMIT, msg) || m(P_CREDITS_REQUIRED, msg) || m(P_INSUFFICIENT_QUOTA, msg) ||
           ai_zero_allowance_text(msg);
}

static bool terminal_client_status(int st) { return st >= 400 && st < 500 && st != 408 && st != 429; }

static uint32_t create(uint32_t kinds) { return kinds | AIE_CLASS; }

static uint32_t classify_text(const char *msg, int err_status, bool prior_overflow, const char *api, const char *provider) {
    uint32_t kinds = 0;
    if (msg && *msg) {
        /* a 429/402 is a throughput or quota decision, never window excess, even when worded like one (Bedrock:
         * "429 Too many tokens, please wait…"): overflow would skip retry and the fallback chain */
        int st_clean = err_status ? err_status : aie_status_from_text(msg);
        if (overflow_text(msg) && st_clean != 429 && st_clean != 402) kinds |= AIE_CONTEXT_OVERFLOW;
        if (payload_rejection_text(msg)) kinds |= AIE_PAYLOAD_REJECTED;
        if (m(P_MALFORMED, msg)) kinds |= AIE_MALFORMED_FUNCTION_CALL;
        if (m(P_PROVIDER_FINISH, msg)) kinds |= AIE_PROVIDER_FINISH_ERROR;
        if (m(P_EMPTY_RESPONSE, msg)) kinds |= AIE_EMPTY_RESPONSE | AIE_TRANSIENT;
        if (m(P_MALFORMED_RESPONSE_FINISH, msg)) kinds |= AIE_TRANSIENT;
        if (m(P_CONTENT_FILTER, msg)) kinds |= AIE_CONTENT_BLOCKED;
        int st = err_status ? err_status : aie_status_from_text(msg);
        if (m(P_ACCOUNT_POLICY, msg) || (str_eq(provider, "cursor") && m(P_CURSOR_MARKER, msg) && m(P_CURSOR_PLAN, msg)))
            kinds |= AIE_ACCOUNT_POLICY | AIE_CONTENT_BLOCKED;
        if ((!provider || str_eq(provider, "anthropic")) && (st == 0 || st == 403) && m(P_ANTHROPIC_ACCOUNT_POLICY, msg))
            kinds |= AIE_ACCOUNT_POLICY;
        if (m(P_AUTH, msg)) kinds |= AIE_AUTH_FAILED;
        const char *reason = mr_rate_limit_reason(msg);
        bool concurrency_excluded = str_eq(reason, "CONCURRENT_LIMIT") && st != 402;
        /* is402BillingCap: a 402 billing-cap body is an account usage limit, so retry rotates and walks the chain */
        if (!concurrency_excluded && (ai_usage_limit_text(msg, st) || (st == 402 && tr_402_billing_cap_body(msg))))
            kinds |= AIE_USAGE_LIMIT;
        if (!terminal_client_status(st)) {
            if (m(P_TIMEOUT, msg)) kinds |= AIE_TRANSIENT | AIE_TIMEOUT;
            else if (transient_text(msg)) kinds |= AIE_TRANSIENT;
            if (m(P_STREAM_PARSE_DIAG, msg) || m(P_STREAM_DROP, msg) || m(P_CODEX_BODY_READ, msg)) kinds |= AIE_TRANSIENT;
        }
        if (str_eq(reason, "CONCURRENT_LIMIT")) kinds |= AIE_TRANSIENT;
        if ((str_eq(api, "openai-responses") || str_eq(api, "openai-codex-responses")) &&
            (m(P_STALE1, msg) || (m(P_STALE2, msg) && m(P_STALE_DETAIL, msg))))
            kinds |= AIE_STALE_RESPONSES_ITEM;
        if (codex_steer_text(api, msg)) kinds |= AIE_TRANSIENT;
        if (st == 400 && (m(P_GENERATION_NAN, msg) || m(P_ALIYUN, msg))) kinds |= AIE_TRANSIENT;
    }
    int evidence = err_status ? err_status : aie_status_from_text(msg);
    if (evidence == 413 && !prior_overflow && !(msg && token_overflow_evidence(msg))) kinds |= AIE_PAYLOAD_REJECTED;
    if (evidence == 402 && (!msg || opaque_status_body(msg))) kinds |= AIE_USAGE_LIMIT;
    if (kinds) return create(kinds);
    if (evidence == 401 || evidence == 403) return create(AIE_AUTH_FAILED);
    return (uint32_t)evidence;
}

uint32_t aie_classify_text(const char *err, int status, const char *api, const char *provider) {
    return classify_text(err, status, false, api, provider);
}

#define KIND_MASK                                                                                                       \
    (AIE_THINKING_LOOP | AIE_TRANSIENT | AIE_TIMEOUT | AIE_USAGE_LIMIT | AIE_STALE_RESPONSES_ITEM |                     \
     AIE_MALFORMED_FUNCTION_CALL | AIE_PROVIDER_FINISH_ERROR | AIE_EMPTY_RESPONSE | AIE_CONTENT_BLOCKED |               \
     AIE_ACCOUNT_POLICY | AIE_CONTEXT_OVERFLOW | AIE_AUTH_FAILED | AIE_PAYLOAD_REJECTED | AIE_SILENT_ABORT |            \
     AIE_USER_INTERRUPT | AIE_ABORT | AIE_GRAMMAR | AIE_FAST_MODE_UNSUPPORTED | AIE_CACHE_CONTROL_UNSUPPORTED |          \
     AIE_OAUTH_EXPIRY)

uint32_t aie_classify_message(const json_t *msg) {
    uint32_t existing = (uint32_t)json_get_int(msg, "errorId", 0);
    json_t *fl;
    json_arr_foreach(json_get(msg, "errorFlags"), i, fl) /* omc providers flag opaque overflow errors */
        if (str_eq(json_sv(fl), "contextOverflow")) existing |= AIE_CLASS | AIE_CONTEXT_OVERFLOW;
    bool classified = (existing & AIE_CLASS) != 0;
    int status = (int)json_get_int(msg, "errorStatus", 0);
    if (!status && existing && !classified) status = (int)existing;
    bool overflow_only = classified && (existing & AIE_CONTEXT_OVERFLOW) && !(existing & AIE_PAYLOAD_REJECTED);
    const char *text = json_get_str(msg, "errorClassificationMessage");
    if (!text) text = json_get_str(msg, "errorMessage");
    uint32_t tid = classify_text(text, status, overflow_only, json_get_str(msg, "api"), json_get_str(msg, "provider"));
    uint32_t kinds = ((classified ? existing : 0) | ((tid & AIE_CLASS) ? tid : 0)) & KIND_MASK;
    /* omp providers throw a premature close as ProviderResponseError kind "incomplete-stream", which attaches
     * Flag.Transient to the message's errorId; omc's providers carry no errorId, so its wording stands in */
    if (!classified && text && m(P_PREMATURE_CLOSE, text)) kinds |= AIE_TRANSIENT;
    if (status == 413 && text && token_overflow_evidence(text) && !(tid & AIE_PAYLOAD_REJECTED)) kinds &= ~AIE_PAYLOAD_REJECTED;
    if (text && m(P_LLAMA_PARSE, text)) kinds &= ~(uint32_t)AIE_TRANSIENT;
    if (kinds) return create(kinds);
    if (tid && !(tid & AIE_CLASS)) return tid;
    if (existing && !classified) return existing;
    return (uint32_t)status;
}

bool aie_retriable(uint32_t id) {
    if (id & AIE_CONTENT_BLOCKED) return false;
    if (id & AIE_PAYLOAD_REJECTED) return false;
    if (id & AIE_MALFORMED_FUNCTION_CALL) return true;
    return (id & (AIE_TRANSIENT | AIE_USAGE_LIMIT | AIE_THINKING_LOOP | AIE_STALE_RESPONSES_ITEM | AIE_PROVIDER_FINISH_ERROR |
                  AIE_EMPTY_RESPONSE)) != 0;
}

/* ---------- turn recovery ---------- */

static bool has_non_ws(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return true;
    return false;
}

static bool is_error_stop(const json_t *msg) { return str_eq(json_get_str(msg, "stopReason"), "error"); }

bool tr_mid_stream_transport_failure(const json_t *msg, uint32_t id, bool aborting) {
    if (!is_error_stop(msg) || !aie_retriable(id)) return false;
    const char *e = json_get_str(msg, "errorMessage");
    if (!e) e = "";
    if (m(P_STREAM_STALL, e)) return true;
    if (aborting) return false;
    return envelope_truncation_text(e) || m(P_HTTP2_RESET, e) || m(P_PY_H2_RESET, e) || m(P_PY_INCOMPLETE_CHUNK, e) ||
           m(P_PREMATURE_CLOSE, e);
}

/* #hasReplayUnsafeOutput */
static bool replay_unsafe_output(const json_t *msg, bool text_committed) {
    json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "toolCall") || str_eq(t, "image") || str_eq(t, "anthropicServerTool")) return true;
        if (str_eq(t, "text") && text_committed && has_non_ws(json_get_str(b, "text"))) return true;
    }
    return false;
}

bool tr_unexecuted_tool_calls_replay_safe(const json_t *msg, bool text_committed, int cursor_resolved) {
    int calls = 0;
    json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "toolCall")) {
            calls++;
            continue;
        }
        if (str_eq(t, "image") || str_eq(t, "anthropicServerTool")) return false;
        if (str_eq(t, "text") && text_committed && has_non_ws(json_get_str(b, "text"))) return false;
    }
    return calls > 0 && cursor_resolved == 0;
}

static bool classifier_refusal(const json_t *msg) {
    const char *sd = json_get_str(json_get_obj(msg, "stopDetails"), "type");
    return is_error_stop(msg) && (str_eq(sd, "refusal") || str_eq(sd, "sensitive"));
}

bool tr_is_retryable_error(const json_t *msg, bool text_committed) {
    if (!is_error_stop(msg)) return false;
    const char *e = json_get_str(msg, "errorMessage");
    if (e && str_starts(e, "Usage preflight blocked:")) return false;
    uint32_t id = aie_classify_message(msg);
    if (id & AIE_CONTEXT_OVERFLOW) return false;
    bool refusal = classifier_refusal(msg);
    bool replay_safe_tools = (refusal || (id & AIE_MALFORMED_FUNCTION_CALL) || aie_retriable(id)) &&
                             tr_unexecuted_tool_calls_replay_safe(msg, text_committed, 0);
    if (replay_unsafe_output(msg, text_committed) && !replay_safe_tools) return false;
    if ((id & AIE_ACCOUNT_POLICY) || refusal) return true;
    return aie_retriable(id);
}

bool tr_resolved_interrupted_tool_turn(const json_t *msg, bool aborting) {
    if (!tr_mid_stream_transport_failure(msg, aie_classify_message(msg), aborting)) return false;
    json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) if (str_eq(json_get_str(b, "type"), "toolCall")) return true;
    return false;
}

bool tr_committed_text_stall(const json_t *msg, bool text_committed, bool aborting, bool *reset) {
    *reset = false;
    uint32_t id = aie_classify_message(msg);
    bool socket_closed = is_error_stop(msg) && aie_retriable(id) && aie_socket_close_text(json_get_str(msg, "errorMessage"));
    if (!socket_closed && !aie_codex_steer_rejection(msg) && !tr_mid_stream_transport_failure(msg, id, aborting)) {
        *reset = true;
        return false;
    }
    if (aborting || !text_committed) return false;
    bool has_text = false;
    json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "toolCall") || str_eq(t, "image") || str_eq(t, "anthropicServerTool")) return false;
        if (str_eq(t, "text") && has_non_ws(json_get_str(b, "text"))) has_text = true;
    }
    return has_text;
}

json_t *tr_stream_stall_reminder(int attempt) {
    json_t *ctx = json_obj();
    json_set_int(ctx, "retryCount", attempt);
    json_set_int(ctx, "maxRetries", TR_STREAM_STALL_CONTINUE_MAX);
    char *txt = prompt_render("coding-agent/prompts/system/stream-stall-continue.md", ctx);
    json_free(ctx);
    json_t *msg = msg_developer_text(txt ? txt : "");
    json_set_str(msg, "attribution", "agent");
    free(txt);
    return msg;
}

/* ---------- manual retry tail rules (TurnRecovery.retry / hasAbortedToolCallTail) ---------- */

static const char *role_of(const json_t *m) { return json_get_str(m, "role"); }

/* isUserTurnInitiator: a user-invoked /skill: prompt (or a writable collab prompt) */
static bool user_turn_initiator(const json_t *m) {
    const char *ct = json_get_str(m, "customType");
    return (str_eq(ct, "skill-prompt") || str_eq(ct, "collab-prompt")) && str_eq(json_get_str(m, "attribution"), "user");
}

static bool is_custom(const json_t *m) {
    const char *r = role_of(m);
    return str_eq(r, "custom") || ((str_eq(r, "user") || str_eq(r, "developer")) && json_get_str(m, "customType"));
}

static bool is_eval_state_context(const json_t *m) {
    return is_custom(m) && str_eq(json_get_str(m, "customType"), "eval-state-context");
}

/* isSyntheticToolResultMessage */
static bool is_synthetic_result(const json_t *m) {
    return str_eq(role_of(m), "toolResult") && json_get_bool(json_get_obj(m, "details"), "__synthetic", false);
}

static bool failed_stop(const json_t *m) {
    const char *st = json_get_str(m, "stopReason");
    return str_eq(st, "error") || str_eq(st, "aborted");
}

static bool has_tool_call(const json_t *m) {
    const json_t *b;
    json_arr_foreach(json_get_arr(m, "content"), i, b) if (str_eq(json_get_str(b, "type"), "toolCall")) return true;
    return false;
}

size_t tr_conversation_end(const json_t *msgs) {
    size_t end = json_len(msgs);
    while (end > 0 && is_custom(json_at(msgs, end - 1)) && !user_turn_initiator(json_at(msgs, end - 1))) end--;
    return end;
}

ssize_t tr_retryable_turn_end(const json_t *msgs) {
    size_t end = tr_conversation_end(msgs);
    while (end > 0 && is_synthetic_result(json_at(msgs, end - 1))) end--;
    const json_t *m = end > 0 ? json_at(msgs, end - 1) : NULL;
    if (!str_eq(role_of(m), "assistant") || !failed_stop(m)) return -1;
    return (ssize_t)end;
}

bool tr_aborted_tool_call_tail(const json_t *msgs) {
    ssize_t end = tr_retryable_turn_end(msgs);
    if (end < 0) return false;
    if ((size_t)end < json_len(msgs) && is_synthetic_result(json_at(msgs, (size_t)end))) return true;
    const json_t *before = end >= 2 ? json_at(msgs, (size_t)end - 2) : NULL;
    return str_eq(role_of(before), "toolResult") && json_get_bool(before, "isError", false);
}

ssize_t tr_tool_replay_start(const json_t *msgs) {
    size_t n = json_len(msgs), end = tr_conversation_end(msgs);
    for (size_t i = end; i < n; i++)
        if (!is_eval_state_context(json_at(msgs, i))) return -1;
    const json_t *tail = end > 0 ? json_at(msgs, end - 1) : NULL;
    if (str_eq(role_of(tail), "assistant")) {
        if (!failed_stop(tail) || has_tool_call(tail)) return -1;
        end--;
    }
    size_t start = end;
    while (start > 0 && str_eq(role_of(json_at(msgs, start - 1)), "toolResult")) start--;
    if (start == end) return -1;
    const json_t *anchor = start > 0 ? json_at(msgs, start - 1) : NULL;
    if (!str_eq(role_of(anchor), "assistant")) return -1;
    const char *st = json_get_str(anchor, "stopReason");
    if (!str_eq(st, "toolUse") && !str_eq(st, "stop")) return -1;
    strvec_t ids = {0};
    const json_t *b;
    json_arr_foreach(json_get_arr(anchor, "content"), i, b) {
        const char *id = json_get_str(b, "id");
        if (str_eq(json_get_str(b, "type"), "toolCall") && id && !strvec_contains(&ids, id)) strvec_push(&ids, id);
    }
    bool ok = end - start == ids.n;
    for (size_t i = start; ok && i < end; i++) {
        const json_t *r = json_at(msgs, i);
        ok = str_eq(role_of(r), "toolResult") && json_get_bool(r, "isError", false) &&
             strvec_contains(&ids, json_get_str(r, "toolCallId") ? json_get_str(r, "toolCallId") : "");
    }
    strvec_free(&ids);
    return ok ? (ssize_t)start : -1;
}
