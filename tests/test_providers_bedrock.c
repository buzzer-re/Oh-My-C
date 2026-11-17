/* test_providers_bedrock.c - offline tests for the Bedrock ConverseStream provider: SigV4 against the AWS
 * test suite, event-stream framing, request body mapping and the stream state machine. */
#include "ai/provider_bedrock.h"

#include <math.h>
#include <zlib.h>

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
#define CHECK_INT(a, b)                                                                                           \
    do {                                                                                                          \
        long long a_ = (long long)(a), b_ = (long long)(b);                                                       \
        g_checks++;                                                                                               \
        if (a_ != b_) {                                                                                           \
            g_fail++;                                                                                             \
            fprintf(stderr, "%s:%d: expected %lld, got %lld (%s)\n", __FILE__, __LINE__, b_, a_, #a);             \
        }                                                                                                         \
    } while (0)

/* ---------- catalog stubs ---------- */
ai_effort_t model_clamp_effort(const ai_model_t *m, ai_effort_t e) { return e; }
char *ai_env_api_key(const char *provider) { return NULL; }
bool model_supports_effort(const ai_model_t *m, ai_effort_t e) { return true; }

static json_t *J(const char *s) {
    char *err = NULL;
    json_t *j = json_parse(s, strlen(s), &err);
    if (!j) {
        fprintf(stderr, "bad test json: %s\n%s\n", err ? err : "?", s);
        exit(2);
    }
    return j;
}
static const char *P(const json_t *j, const char *path) { return json_sv(json_path(j, path)); }
static int64_t PI(const json_t *j, const char *path) {
    json_t *v = json_path(j, path);
    return json_is_num(v) ? (int64_t)v->u.num.d : -999999;
}

/* catalog defaults for bedrock-converse-stream (assets/models.json.z "defaults") */
static json_t *g_defaults;

static ai_model_t mk_model(const char *raw_json) {
    json_t *r = J(raw_json);
    ai_model_t m;
    memset(&m, 0, sizeof m);
    m.raw = r;
    m.id = json_get_str(r, "id");
    m.api = "bedrock-converse-stream";
    m.provider = "amazon-bedrock";
    m.base_url = json_get_str(r, "baseUrl") ? json_get_str(r, "baseUrl") : "https://bedrock-runtime.us-east-1.amazonaws.com";
    m.reasoning = json_get_bool(r, "reasoning", false);
    m.input_image = true;
    m.max_tokens = json_get_int(r, "maxTokens", 64000);
    m.context_window = 200000;
    m.cost_input = 3;
    m.cost_output = 15;
    m.compat = json_get_obj(r, "compat");
    m.compat_defaults = g_defaults;
    m.thinking = json_get_obj(r, "thinking");
    m.headers = json_get_obj(r, "headers");
    return m;
}

#define SONNET45                                                                                                   \
    "{\"id\":\"us.anthropic.claude-sonnet-4-5-20250929-v1:0\",\"reasoning\":true,\"maxTokens\":64000,"            \
    "\"thinking\":{\"mode\":\"budget\",\"efforts\":[\"minimal\",\"low\",\"medium\",\"high\"]},"                     \
    "\"compat\":{\"supportsLongPromptCacheRetention\":true}}"
#define OPUS47                                                                                                     \
    "{\"id\":\"us.anthropic.claude-opus-4-7\",\"reasoning\":true,\"maxTokens\":128000,"                            \
    "\"thinking\":{\"mode\":\"anthropic-adaptive\",\"efforts\":[\"low\",\"medium\",\"high\",\"max\"],"              \
    "\"supportsDisplay\":true},\"compat\":{\"supportsSamplingParams\":false}}"
#define OPUS55                                                                                                     \
    "{\"id\":\"us.anthropic.claude-opus-5-5\",\"reasoning\":true,\"maxTokens\":128000,"                            \
    "\"thinking\":{\"mode\":\"anthropic-adaptive\",\"efforts\":[\"low\",\"medium\",\"high\",\"xhigh\",\"max\"],"    \
    "\"supportsDisplay\":true,\"prefixBinding\":true},\"compat\":{\"supportsForcedToolChoice\":false}}"
#define HAIKU55                                                                                                    \
    "{\"id\":\"us.anthropic.claude-haiku-5-5\",\"reasoning\":true,\"maxTokens\":64000,"                            \
    "\"thinking\":{\"mode\":\"anthropic-adaptive\",\"efforts\":[\"low\",\"high\"],\"supportsDisplay\":true,"        \
    "\"prefixBinding\":true},\"compat\":{\"disabledThinking\":\"disabled\"}}"
#define GPT6                                                                                                       \
    "{\"id\":\"us.openai.gpt-6-sol\",\"reasoning\":true,\"maxTokens\":128000,"                                     \
    "\"thinking\":{\"mode\":\"effort\",\"efforts\":[\"low\",\"medium\",\"high\",\"xhigh\",\"max\"]},"               \
    "\"compat\":{\"promptCacheMode\":\"none\",\"promptCacheMaximumCheckpoints\":0},"                               \
    "\"requiresToolResultImageHoisting\":true}"
#define NOVA                                                                                                       \
    "{\"id\":\"us.amazon.nova-pro-v1:0\",\"reasoning\":false,\"maxTokens\":10000,"                                 \
    "\"guardrailIdentifier\":\"arn:aws:bedrock:eu-west-1:123:guardrail/abc\","                                     \
    "\"requestMetadata\":{\"team\":\"core\",\"bad key!\":\"x\",\"ok\":\"v<>\"}}"

static const char *CTX_MESSAGES =
    "[{\"role\":\"user\",\"content\":\"hello\",\"timestamp\":1},"
    "{\"role\":\"user\",\"content\":\"   \",\"timestamp\":1},"
    "{\"role\":\"assistant\",\"api\":\"bedrock-converse-stream\",\"provider\":\"amazon-bedrock\","
    "\"model\":\"us.anthropic.claude-sonnet-4-5-20250929-v1:0\",\"stopReason\":\"toolUse\",\"content\":["
    "{\"type\":\"thinking\",\"thinking\":\"plan it\",\"thinkingSignature\":\"SIG1\"},"
    "{\"type\":\"thinking\",\"thinking\":\"unsigned musing\"},"
    "{\"type\":\"text\",\"text\":\"  \"},"
    "{\"type\":\"text\",\"text\":\"running\"},"
    "{\"type\":\"toolCall\",\"id\":\"call|1\",\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}},"
    "{\"type\":\"toolCall\",\"id\":\"call_2\",\"name\":\"read\",\"arguments\":{\"path\":\"a\"}}]},"
    "{\"role\":\"toolResult\",\"toolCallId\":\"call|1\",\"toolName\":\"bash\",\"isError\":true,\"content\":["
    "{\"type\":\"text\",\"text\":\"boom\"},{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"QUJD\"}]},"
    "{\"role\":\"toolResult\",\"toolCallId\":\"call_2\",\"toolName\":\"read\",\"isError\":false,\"content\":["
    "{\"type\":\"text\",\"text\":\"file\"},{\"type\":\"image\",\"mimeType\":\"image/jpg\",\"data\":\"REVG\"}]},"
    "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"look\"},"
    "{\"type\":\"image\",\"mimeType\":\"image/webp\",\"data\":\"R0hJ\"}],\"timestamp\":2}]";

static const char *CTX_TOOLS =
    "[{\"name\":\"bash\",\"description\":\"Run a command\",\"parameters\":{\"type\":\"object\",\"properties\":"
    "{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}},"
    "{\"name\":\"read\",\"description\":\"\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}]";

/* ======================================================================
 * SigV4 (AWS SigV4 test suite: get-vanilla, post-vanilla, get-vanilla-query-order-key-case; IAM docs example)
 * ====================================================================== */

#define EX_AKID "AKIDEXAMPLE"
#define EX_SECRET "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"
#define EX_DATE "20150830T123600Z"
#define EMPTY_SHA "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

static void test_sigv4_suite(void) {
    http_headers_t h = {0};
    http_headers_add(&h, "Host", "example.amazonaws.com");
    http_headers_add(&h, "X-Amz-Date", EX_DATE);
    char *creq = NULL, *sts = NULL;
    char *auth = aws_sigv4_authorization("GET", "/", NULL, &h, EMPTY_SHA, EX_DATE, "us-east-1", "service", EX_AKID,
                                         EX_SECRET, &creq, &sts);
    CHECK_STR(creq, "GET\n/\n\nhost:example.amazonaws.com\nx-amz-date:20150830T123600Z\n\nhost;x-amz-date\n" EMPTY_SHA);
    CHECK_STR(sts, "AWS4-HMAC-SHA256\n20150830T123600Z\n20150830/us-east-1/service/aws4_request\n"
                   "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63");
    CHECK_STR(auth, "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
                    "SignedHeaders=host;x-amz-date, "
                    "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
    free(auth);
    free(creq);
    free(sts);

    auth = aws_sigv4_authorization("POST", "/", "", &h, EMPTY_SHA, EX_DATE, "us-east-1", "service", EX_AKID, EX_SECRET,
                                   NULL, NULL);
    CHECK(str_ends(auth, "Signature=5da7c1a2acd57cee7505fc6676e4e544621c30862966e37dddb68e92efbe5d6b"));
    free(auth);

    /* get-vanilla-query-order-key-case: parameters sorted after encoding */
    auth = aws_sigv4_authorization("GET", "/", "Param2=value2&Param1=value1", &h, EMPTY_SHA, EX_DATE, "us-east-1",
                                   "service", EX_AKID, EX_SECRET, NULL, NULL);
    CHECK(str_ends(auth, "Signature=b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219ec94cdf2500"));
    free(auth);
    http_headers_free(&h);

    /* IAM user guide example: GET iam ListUsers with a content-type header */
    http_headers_add(&h, "content-type", "application/x-www-form-urlencoded; charset=utf-8");
    http_headers_add(&h, "host", "iam.amazonaws.com");
    http_headers_add(&h, "x-amz-date", EX_DATE);
    auth = aws_sigv4_authorization("GET", "/", "Action=ListUsers&Version=2010-05-08", &h, EMPTY_SHA, EX_DATE,
                                   "us-east-1", "iam", EX_AKID, EX_SECRET, &creq, NULL);
    char *ch = aws_sha256_hex(creq, strlen(creq));
    CHECK_STR(ch, "f536975d06c0309214f805bb90ccff089219ecd68b2577efef23edd43b7e1a59");
    CHECK_STR(auth, "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/iam/aws4_request, "
                    "SignedHeaders=content-type;host;x-amz-date, "
                    "Signature=5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7");
    free(ch);
    free(auth);
    free(creq);
    http_headers_free(&h);

    unsigned char key[32];
    aws_signing_key(EX_SECRET, "20150830", "us-east-1", "iam", key);
    char *kh = hex_encode(key, 32);
    CHECK_STR(kh, "c4afb1cc5771d871763a393e44b703571b55cc28424d1a5e86da6ed3c154a4b9");
    free(kh);
}

static void test_sigv4_helpers(void) {
    char *q = aws_canonical_query("x=1&%7B=2&a=b+c&a=a&flag");
    CHECK_STR(q, "%7B=2&a=a&a=b%2Bc&flag=&x=1"); /* encoded then sorted; '+' is literal */
    free(q);
    char *p = aws_canonical_path("/model/anthropic.claude-v1%3A0/converse-stream");
    CHECK_STR(p, "/model/anthropic.claude-v1%253A0/converse-stream");
    free(p);
    char d[17];
    aws_amz_date(1440938160000LL, d);
    CHECK_STR(d, EX_DATE);
}

/* signRequest as Bedrock uses it: x-amz-* headers added, user-agent unsigned, double-encoded path.
 * Expected signature computed independently with Python hashlib/hmac. */
static void test_sigv4_bedrock_sign(void) {
    aws_creds_t c = {.access_key_id = EX_AKID, .secret_access_key = EX_SECRET, .session_token = "TOKEN"};
    http_headers_t extra = {0}, out = {0};
    http_headers_add(&extra, "User-Agent", "omc/test");
    http_headers_add(&extra, "content-type", "application/json");
    http_headers_add(&extra, "accept", "application/vnd.amazon.eventstream");
    http_headers_add(&extra, "X-Custom", "  a   b ");
    const char *body = "{\"messages\":[]}";
    aws_sign_params_t sp = {.method = "POST",
                            .host = "bedrock-runtime.us-east-1.amazonaws.com",
                            .path = "/model/anthropic.claude-3-5-haiku-20241022-v1%3A0/converse-stream",
                            .body = body,
                            .body_len = strlen(body),
                            .region = "us-east-1",
                            .service = "bedrock",
                            .creds = &c,
                            .headers = &extra,
                            .time_ms = 1440938160000LL};
    aws_sigv4_sign(&sp, &out);
    CHECK_STR(http_headers_get(&out, "x-amz-date"), EX_DATE);
    CHECK_STR(http_headers_get(&out, "x-amz-content-sha256"),
              "5e4ce7b36ba37b78a5d5f9fd08e6b7b54ba6879d651aa46ec9e1d6fa24ebe30a");
    CHECK_STR(http_headers_get(&out, "x-amz-security-token"), "TOKEN");
    CHECK_STR(http_headers_get(&out, "host"), "bedrock-runtime.us-east-1.amazonaws.com");
    CHECK_STR(http_headers_get(&out, "authorization"),
              "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/bedrock/aws4_request, "
              "SignedHeaders=accept;content-type;host;x-amz-content-sha256;x-amz-date;x-amz-security-token;x-custom, "
              "Signature=16e02804922f31fbfb0bc5de57b98e11390dce287057c52cc5ecbe1fb50c5064");
    http_headers_free(&extra);
    http_headers_free(&out);
}

/* ======================================================================
 * event stream framing (frames built here with zlib directly, independent of aws_es_encode)
 * ====================================================================== */

typedef struct {
    sb_t h;
} hb_t;

static void hb_name(hb_t *b, const char *n, int type) {
    sb_putc(&b->h, (char)strlen(n));
    sb_append(&b->h, n);
    sb_putc(&b->h, (char)type);
}
static void hb_be(hb_t *b, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; i--) sb_putc(&b->h, (char)((v >> (8 * i)) & 0xff));
}
static void hb_str(hb_t *b, const char *n, const char *v) {
    hb_name(b, n, 7);
    hb_be(b, strlen(v), 2);
    sb_append(&b->h, v);
}

static unsigned char *frame(const hb_t *b, const char *payload, size_t *len) {
    size_t hl = b ? b->h.len : 0, pl = payload ? strlen(payload) : 0, total = 16 + hl + pl;
    unsigned char *f = xmalloc(total);
    uint32_t v[2] = {(uint32_t)total, (uint32_t)hl};
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 4; i++) f[k * 4 + i] = (unsigned char)(v[k] >> (24 - 8 * i));
    uint32_t pc = (uint32_t)crc32(0, f, 8);
    for (int i = 0; i < 4; i++) f[8 + i] = (unsigned char)(pc >> (24 - 8 * i));
    if (hl) memcpy(f + 12, b->h.s, hl);
    if (pl) memcpy(f + 12 + hl, payload, pl);
    uint32_t mc = (uint32_t)crc32(0, f, total - 4);
    for (int i = 0; i < 4; i++) f[total - 4 + i] = (unsigned char)(mc >> (24 - 8 * i));
    *len = total;
    return f;
}

static unsigned char *event_frame(const char *type, const char *payload, size_t *len) {
    hb_t b = {SB_INIT};
    hb_str(&b, ":event-type", type);
    hb_str(&b, ":content-type", "application/json");
    hb_str(&b, ":message-type", "event");
    unsigned char *f = frame(&b, payload, len);
    sb_free(&b.h);
    return f;
}

static void test_eventstream_decode(void) {
    /* AWS event-stream test suite "empty_message" */
    static const unsigned char empty[] = {0, 0, 0, 0x10, 0, 0, 0, 0, 0x05, 0xc2, 0x48, 0xeb, 0x7d, 0x98, 0xc8, 0xff};
    aws_es_msg_t m;
    char *err = NULL;
    CHECK_INT(aws_es_decode_message(empty, sizeof empty, &m, &err), 0);
    CHECK_INT(m.headers.n, 0);
    CHECK_INT(m.payload_len, 0);
    aws_es_msg_free(&m);

    hb_t b = {SB_INIT};
    hb_name(&b, "t", 0);
    hb_name(&b, "f", 1);
    hb_name(&b, "byte", 2);
    hb_be(&b, 0xfe, 1);
    hb_name(&b, "short", 3);
    hb_be(&b, 0x8000, 2);
    hb_name(&b, "int", 4);
    hb_be(&b, 123456, 4);
    hb_name(&b, "long", 5);
    hb_be(&b, 0xffffffffffffffffULL, 8);
    hb_name(&b, "bytes", 6);
    hb_be(&b, 3, 2);
    sb_append(&b.h, "ABC");
    hb_name(&b, "ts", 8);
    hb_be(&b, 1440938160123ULL, 8);
    hb_name(&b, "uuid", 9);
    for (int i = 0; i < 16; i++) sb_putc(&b.h, (char)i);
    hb_str(&b, ":event-type", "contentBlockDelta");
    size_t fl;
    unsigned char *f = frame(&b, "{\"x\":1}", &fl);
    CHECK_INT(aws_es_decode_message(f, fl, &m, &err), 0);
    CHECK_STR(http_headers_get(&m.headers, "t"), "true");
    CHECK_STR(http_headers_get(&m.headers, "f"), "false");
    CHECK_STR(http_headers_get(&m.headers, "byte"), "-2");
    CHECK_STR(http_headers_get(&m.headers, "short"), "-32768");
    CHECK_STR(http_headers_get(&m.headers, "int"), "123456");
    CHECK_STR(http_headers_get(&m.headers, "long"), "-1");
    CHECK_STR(http_headers_get(&m.headers, "bytes"), "QUJD");
    CHECK_STR(http_headers_get(&m.headers, "ts"), "2015-08-30T12:36:00.123Z");
    CHECK_STR(http_headers_get(&m.headers, "uuid"), "00010203-0405-0607-0809-0a0b0c0d0e0f");
    CHECK_STR(http_headers_get(&m.headers, ":event-type"), "contentBlockDelta");
    CHECK(m.payload_len == 7 && !memcmp(m.payload, "{\"x\":1}", 7));
    aws_es_msg_free(&m);

    /* encoder round trip produces byte-identical frames to the hand-built one */
    const char *n1[] = {":event-type"}, *v1[] = {"contentBlockDelta"};
    hb_t b2 = {SB_INIT};
    hb_str(&b2, ":event-type", "contentBlockDelta");
    size_t l1, l2;
    unsigned char *e1 = aws_es_encode(n1, v1, 1, "{}", 2, &l1);
    unsigned char *e2 = frame(&b2, "{}", &l2);
    CHECK(l1 == l2 && !memcmp(e1, e2, l1));
    free(e1);
    free(e2);
    sb_free(&b2.h);

    /* corrupted prelude CRC */
    unsigned char *bad = xmalloc(fl);
    memcpy(bad, f, fl);
    bad[9] ^= 1;
    CHECK_INT(aws_es_decode_message(bad, fl, &m, &err), -1);
    CHECK_STR(err, "prelude CRC mismatch");
    free(err);
    /* corrupted payload => message CRC */
    memcpy(bad, f, fl);
    bad[fl - 6] ^= 0x40;
    CHECK_INT(aws_es_decode_message(bad, fl, &m, &err), -1);
    CHECK_STR(err, "message CRC mismatch");
    free(err);
    /* corrupted message CRC itself */
    memcpy(bad, f, fl);
    bad[fl - 1] ^= 0xff;
    CHECK_INT(aws_es_decode_message(bad, fl, &m, &err), -1);
    CHECK_STR(err, "message CRC mismatch");
    free(err);
    /* framed length mismatch */
    CHECK_INT(aws_es_decode_message(f, fl - 1, &m, &err), -1);
    CHECK(err && strstr(err, "framed length"));
    free(err);
    free(bad);
    free(f);
    sb_free(&b.h);

    /* headers length larger than the frame (CRCs valid) */
    unsigned char hdr_over[16] = {0, 0, 0, 16, 0, 0, 0, 9};
    uint32_t pc = (uint32_t)crc32(0, hdr_over, 8);
    for (int i = 0; i < 4; i++) hdr_over[8 + i] = (unsigned char)(pc >> (24 - 8 * i));
    uint32_t mc = (uint32_t)crc32(0, hdr_over, 12);
    for (int i = 0; i < 4; i++) hdr_over[12 + i] = (unsigned char)(mc >> (24 - 8 * i));
    CHECK_INT(aws_es_decode_message(hdr_over, 16, &m, &err), -1);
    free(err);
    /* truncated header value (string length past the block) */
    hb_t tb = {SB_INIT};
    hb_name(&tb, "s", 7);
    hb_be(&tb, 50, 2);
    sb_append(&tb.h, "abc");
    f = frame(&tb, NULL, &fl);
    CHECK_INT(aws_es_decode_message(f, fl, &m, &err), -1);
    CHECK(err && strstr(err, "truncated"));
    free(err);
    free(f);
    sb_free(&tb.h);
    /* unknown header type */
    hb_t ub = {SB_INIT};
    hb_name(&ub, "u", 42);
    f = frame(&ub, NULL, &fl);
    CHECK_INT(aws_es_decode_message(f, fl, &m, &err), -1);
    CHECK_STR(err, "unknown header value type 42");
    free(err);
    free(f);
    sb_free(&ub.h);
}

static int count_cb(void *ud, const aws_es_msg_t *m) {
    sb_t *log = ud;
    sb_appendf(log, "%s|", http_headers_get(&m->headers, ":event-type"));
    return 0;
}

static void test_eventstream_streaming(void) {
    sb_t all = SB_INIT, log = SB_INIT;
    const char *types[] = {"messageStart", "contentBlockDelta", "messageStop"};
    for (int i = 0; i < 3; i++) {
        size_t l;
        unsigned char *f = event_frame(types[i], "{}", &l);
        sb_appendn(&all, (char *)f, l);
        free(f);
    }
    aws_es_decoder_t d;
    aws_es_init(&d, count_cb, &log);
    for (size_t i = 0; i < all.len; i++) CHECK_INT(aws_es_feed(&d, all.s + i, 1), 0); /* byte at a time */
    CHECK_INT(aws_es_finish(&d), 0);
    CHECK_STR(sb_cstr(&log), "messageStart|contentBlockDelta|messageStop|");
    aws_es_free(&d);

    /* truncated final frame */
    sb_clear(&log);
    aws_es_init(&d, count_cb, &log);
    CHECK_INT(aws_es_feed(&d, all.s, all.len - 3), 0);
    CHECK_INT(aws_es_finish(&d), -1);
    CHECK_STR(d.error, "truncated message at end of stream");
    CHECK_STR(sb_cstr(&log), "messageStart|contentBlockDelta|");
    aws_es_free(&d);

    /* bad CRC in the middle of a stream stops decoding */
    sb_clear(&log);
    all.s[all.len / 2] ^= 0x10;
    aws_es_init(&d, count_cb, &log);
    CHECK_INT(aws_es_feed(&d, all.s, all.len), -1);
    CHECK(d.error && strstr(d.error, "CRC mismatch"));
    aws_es_free(&d);

    /* the 12-byte prelude is validated before the rest of the frame is buffered (omp b650a08a28): CRC, then the
     * total length (16 B..24 MiB) and the header length (<= 128 KiB, <= total - 16) */
    static const struct {
        uint32_t total, hlen;
        const char *err;
    } bad_preludes[] = {
        {4, 0, "total length 4 below minimum"},
        {0x7f000000u, 0, "total length 2130706432 exceeds maximum 25165824"},
        {1u << 20, 200000, "headers length 200000 exceeds maximum 131072"},
        {64, 49, "headers length 49 exceeds message length 64"},
    };
    for (size_t i = 0; i < ARRAY_LEN(bad_preludes); i++) {
        unsigned char pre[12];
        uint32_t vals[2] = {bad_preludes[i].total, bad_preludes[i].hlen};
        for (int k = 0; k < 2; k++)
            for (int b = 0; b < 4; b++) pre[k * 4 + b] = (unsigned char)(vals[k] >> (24 - 8 * b));
        uint32_t crc = (uint32_t)crc32(0, pre, 8);
        for (int b = 0; b < 4; b++) pre[8 + b] = (unsigned char)(crc >> (24 - 8 * b));
        aws_es_init(&d, count_cb, &log);
        CHECK_INT(aws_es_feed(&d, pre, 11), 0); /* an incomplete prelude waits */
        CHECK_INT(aws_es_feed(&d, pre + 11, 1), -1);
        CHECK_STR(d.error, bad_preludes[i].err);
        aws_es_free(&d);
        if (i == 0) { /* a corrupt prelude CRC is reported before any length */
            pre[11] ^= 1;
            aws_es_init(&d, count_cb, &log);
            CHECK_INT(aws_es_feed(&d, pre, 12), -1);
            CHECK_STR(d.error, "prelude CRC mismatch");
            aws_es_free(&d);
        }
    }
    sb_free(&all);
    sb_free(&log);
}

/* ======================================================================
 * request body mapping
 * ====================================================================== */

static json_t *build(const ai_model_t *m, const char *msgs, const char *tools, const ai_stream_opts_t *o,
                     bool *sentinel) {
    json_t *jm = J(msgs), *jt = tools ? J(tools) : NULL, *sys = J("[\"You are omc.\",\"  \"]");
    ai_context_t ctx = {.system_prompt = sys, .messages = jm, .tools = jt};
    bedrock_request_t r;
    bedrock_build_request(m, &ctx, o, &r);
    if (r.error) fprintf(stderr, "build error: %s\n", r.error);
    if (sentinel) *sentinel = r.sentinel_injected;
    json_t *b = r.body;
    r.body = NULL;
    bedrock_request_free(&r);
    json_free(jm);
    json_free(jt);
    json_free(sys);
    return b;
}

static void test_request_budget_model(void) {
    ai_model_t m = mk_model(SONNET45);
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_MEDIUM;
    o.temperature = 0.5;
    json_t *b = build(&m, CTX_MESSAGES, CTX_TOOLS, &o, NULL);
    CHECK(b != NULL);
    if (!b) return;
    char *dump = json_dump(b, 0);
    /* top-level key order mirrors the upstream ConverseStreamRequest literal */
    CHECK(str_starts(dump, "{\"messages\":"));
    CHECK(strstr(dump, "\"system\":") < strstr(dump, "\"inferenceConfig\":"));
    CHECK(strstr(dump, "\"inferenceConfig\":") < strstr(dump, "\"toolConfig\":"));
    CHECK(strstr(dump, "\"toolConfig\":") < strstr(dump, "\"additionalModelRequestFields\":"));
    free(dump);
    /* system: blank prompts dropped, cache point after the last one */
    CHECK_INT(json_len(json_get(b, "system")), 2);
    CHECK_STR(P(b, "system.0.text"), "You are omc.");
    CHECK_STR(P(b, "system.1.cachePoint.type"), "default");
    CHECK(json_path(b, "system.1.cachePoint.ttl") == NULL);
    /* inference config: budget mode keeps the model max; sampling params pass through */
    CHECK_INT(PI(b, "inferenceConfig.maxTokens"), 64000);
    CHECK(fabs(json_get_num(json_get(b, "inferenceConfig"), "temperature", 0) - 0.5) < 1e-9);
    CHECK(json_path(b, "inferenceConfig.topP") == NULL);
    CHECK_STR(P(b, "additionalModelRequestFields.thinking.type"), "enabled");
    CHECK_INT(PI(b, "additionalModelRequestFields.thinking.budget_tokens"), 8192);
    CHECK_STR(P(b, "additionalModelRequestFields.thinking.display"), "summarized");
    CHECK(json_path(b, "additionalModelRequestFields.anthropic_beta") == NULL);
    CHECK(json_get(b, "additionalModelResponseFieldPaths") == NULL);
    /* messages: blank user dropped; assistant thinking signed => reasoningContent, unsigned => demoted text */
    json_t *msgs = json_get(b, "messages");
    CHECK_INT(json_len(msgs), 4);
    CHECK_STR(P(b, "messages.0.role"), "user");
    CHECK_STR(P(b, "messages.0.content.0.text"), "hello");
    CHECK_STR(P(b, "messages.1.role"), "assistant");
    CHECK_STR(P(b, "messages.1.content.0.reasoningContent.reasoningText.text"), "plan it");
    CHECK_STR(P(b, "messages.1.content.0.reasoningContent.reasoningText.signature"), "SIG1");
    CHECK_STR(P(b, "messages.1.content.1.text"), "unsigned musing"); /* claude dialect: raw text */
    CHECK_STR(P(b, "messages.1.content.2.text"), "running");
    CHECK_STR(P(b, "messages.1.content.3.toolUse.toolUseId"), "call_1");
    CHECK_STR(P(b, "messages.1.content.3.toolUse.name"), "bash");
    CHECK_STR(P(b, "messages.1.content.3.toolUse.input.command"), "ls");
    CHECK_INT(json_len(json_path(b, "messages.1.content")), 5);
    /* both tool results grouped in one user message; error-result image hoisted after the results */
    CHECK_STR(P(b, "messages.2.role"), "user");
    CHECK_INT(json_len(json_path(b, "messages.2.content")), 3);
    CHECK_STR(P(b, "messages.2.content.0.toolResult.toolUseId"), "call_1");
    CHECK_STR(P(b, "messages.2.content.0.toolResult.status"), "error");
    CHECK_STR(P(b, "messages.2.content.0.toolResult.content.0.text"), "boom");
    CHECK_STR(P(b, "messages.2.content.0.toolResult.content.1.text"), "(see attached image)");
    CHECK_STR(P(b, "messages.2.content.1.toolResult.status"), "success");
    CHECK_STR(P(b, "messages.2.content.1.toolResult.content.1.image.format"), "jpeg");
    CHECK_STR(P(b, "messages.2.content.1.toolResult.content.1.image.source.bytes"), "REVG");
    CHECK_STR(P(b, "messages.2.content.2.image.format"), "png");
    CHECK_STR(P(b, "messages.2.content.2.image.source.bytes"), "QUJD");
    /* final user message: text + image + cache point */
    CHECK_STR(P(b, "messages.3.content.0.text"), "look");
    CHECK_STR(P(b, "messages.3.content.1.image.format"), "webp");
    CHECK_STR(P(b, "messages.3.content.2.cachePoint.type"), "default");
    /* tools: empty description omitted */
    CHECK_STR(P(b, "toolConfig.tools.0.toolSpec.name"), "bash");
    CHECK_STR(P(b, "toolConfig.tools.0.toolSpec.description"), "Run a command");
    CHECK_STR(P(b, "toolConfig.tools.0.toolSpec.inputSchema.json.properties.command.type"), "string");
    CHECK(json_path(b, "toolConfig.tools.1.toolSpec.description") == NULL);
    CHECK(json_path(b, "toolConfig.toolChoice") == NULL);
    json_free(b);

    /* forced tool choice + thinking: thinking dropped, forced choice kept */
    o.tool_choice = J("\"required\"");
    b = build(&m, CTX_MESSAGES, CTX_TOOLS, &o, NULL);
    CHECK(json_path(b, "toolConfig.toolChoice.any") != NULL);
    CHECK(json_get(b, "additionalModelRequestFields") == NULL);
    json_free(b);
    json_free(o.tool_choice);
    o.tool_choice = J("{\"type\":\"tool\",\"name\":\"read\"}");
    o.reasoning = EFFORT_OFF;
    b = build(&m, CTX_MESSAGES, CTX_TOOLS, &o, NULL);
    CHECK_STR(P(b, "toolConfig.toolChoice.tool.name"), "read");
    CHECK(json_get(b, "additionalModelRequestFields") == NULL);
    json_free(b);
    json_free(o.tool_choice);
    o.tool_choice = NULL;

    /* custom budgets, long retention => 1h cache points, small max_tokens raises output room */
    o.reasoning = EFFORT_HIGH;
    o.thinking_budgets = J("{\"high\":20000}");
    o.cache_retention = "long";
    o.max_tokens = 4000;
    b = build(&m, CTX_MESSAGES, NULL, &o, NULL);
    CHECK_INT(PI(b, "additionalModelRequestFields.thinking.budget_tokens"), 20000);
    CHECK_INT(PI(b, "inferenceConfig.maxTokens"), 21024);
    CHECK_STR(P(b, "system.1.cachePoint.ttl"), "1h");
    CHECK_STR(P(b, "messages.3.content.2.cachePoint.ttl"), "1h");
    /* history has tool blocks but no tools and no explicit "none": no toolConfig */
    CHECK(json_get(b, "toolConfig") == NULL);
    json_free(b);
    json_free(o.thinking_budgets);
    o.thinking_budgets = NULL;

    /* budget does not fit the model max: shrunk to leave MIN_OUTPUT_TOKENS */
    ai_model_t small = mk_model("{\"id\":\"qwen.qwen3-32b-v1:0\",\"reasoning\":true,\"maxTokens\":16384,"
                                "\"thinking\":{\"mode\":\"budget\",\"efforts\":[\"minimal\",\"low\",\"medium\",\"high\"]},"
                                "\"compat\":{\"promptCacheMode\":\"none\",\"promptCacheMaximumCheckpoints\":0}}");
    o.max_tokens = 0;
    o.cache_retention = NULL;
    o.hide_thinking_summary = true;
    b = build(&small, CTX_MESSAGES, NULL, &o, NULL);
    CHECK_INT(PI(b, "inferenceConfig.maxTokens"), 16384);
    CHECK_INT(PI(b, "additionalModelRequestFields.thinking.budget_tokens"), 15360);
    CHECK_STR(P(b, "additionalModelRequestFields.thinking.display"), "omitted");
    CHECK_INT(json_len(json_get(b, "system")), 1); /* promptCacheMode none */
    CHECK_STR(P(b, "messages.1.content.1.text"), "<think>\nunsigned musing\n</think>");
    json_free(b);
    json_free(small.raw);
    json_free(m.raw);
}

static void test_request_adaptive_models(void) {
    ai_model_t m = mk_model(OPUS47);
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_HIGH;
    o.temperature = 0.2;
    o.top_p = 0.9;
    json_t *b = build(&m, "[{\"role\":\"user\",\"content\":\"hi\"}]", NULL, &o, NULL);
    CHECK_STR(P(b, "additionalModelRequestFields.thinking.type"), "adaptive");
    CHECK_STR(P(b, "additionalModelRequestFields.thinking.display"), "summarized");
    CHECK_STR(P(b, "additionalModelRequestFields.output_config.effort"), "high");
    CHECK_INT(PI(b, "inferenceConfig.maxTokens"), 128000);
    CHECK(json_path(b, "inferenceConfig.temperature") == NULL); /* supportsSamplingParams: false */
    CHECK(json_path(b, "inferenceConfig.topP") == NULL);
    CHECK(json_get(b, "toolConfig") == NULL);
    json_free(b);
    o.max_tokens = 4000;
    o.reasoning = EFFORT_MINIMAL; /* clamped to the lowest supported effort */
    b = build(&m, "[{\"role\":\"user\",\"content\":\"hi\"}]", NULL, &o, NULL);
    CHECK_STR(P(b, "additionalModelRequestFields.output_config.effort"), "low");
    CHECK_INT(PI(b, "inferenceConfig.maxTokens"), 4000 + 2048);
    json_free(b);
    json_free(m.raw);

    /* prefix-bound line without forced tool support */
    m = mk_model(OPUS55);
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_XHIGH;
    o.tool_choice = J("\"required\"");
    b = build(&m, "[{\"role\":\"user\",\"content\":\"hi\"}]", CTX_TOOLS, &o, NULL);
    CHECK(json_path(b, "toolConfig.toolChoice.auto") != NULL);
    CHECK_STR(P(b, "additionalModelRequestFields.thinking.type"), "adaptive");
    CHECK_STR(P(b, "additionalModelRequestFields.thinking.block_binding.prefix_mismatch_behavior"), "drop_block");
    CHECK_STR(P(b, "additionalModelRequestFields.output_config.effort"), "xhigh");
    CHECK_STR(P(b, "additionalModelRequestFields.anthropic_beta.0"), "thinking-binding-controls-2026-08-01");
    CHECK_STR(P(b, "additionalModelResponseFieldPaths.0"), "/input_transformations");
    json_free(b);
    /* thinking off on a prefix-bound line: binding still pins adaptive thinking */
    o.reasoning = EFFORT_OFF;
    b = build(&m, "[{\"role\":\"user\",\"content\":\"hi\"}]", NULL, &o, NULL);
    CHECK_STR(P(b, "additionalModelRequestFields.thinking.type"), "adaptive");
    CHECK(json_path(b, "additionalModelRequestFields.output_config") == NULL);
    json_free(b);
    json_free(o.tool_choice);
    json_free(m.raw);

    /* disabledThinking: explicit disabled block, no binding controls */
    m = mk_model(HAIKU55);
    ai_stream_opts_init(&o);
    o.disable_reasoning = true;
    o.reasoning = EFFORT_HIGH;
    b = build(&m, "[{\"role\":\"user\",\"content\":\"hi\"}]", NULL, &o, NULL);
    CHECK_STR(P(b, "additionalModelRequestFields.thinking.type"), "disabled");
    CHECK_STR(P(b, "additionalModelRequestFields.output_config.effort"), "low");
    CHECK(json_get(b, "additionalModelResponseFieldPaths") == NULL);
    json_free(b);
    json_free(m.raw);
}

static void test_request_other_models(void) {
    ai_model_t m = mk_model(GPT6);
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_XHIGH;
    json_t *b = build(&m, CTX_MESSAGES, CTX_TOOLS, &o, NULL);
    CHECK_STR(P(b, "additionalModelRequestFields.reasoning.effort"), "xhigh");
    CHECK(json_path(b, "additionalModelRequestFields.thinking") == NULL);
    CHECK_INT(json_len(json_get(b, "system")), 1); /* no cache points */
    CHECK_INT(json_len(json_path(b, "messages.3.content")), 2);
    /* requiresToolResultImageHoisting: success results hoist too */
    CHECK_STR(P(b, "messages.2.content.1.toolResult.content.1.text"), "(see attached image)");
    CHECK_INT(json_len(json_path(b, "messages.2.content")), 4);
    /* signed thinking from a Claude turn is replayed as reasoningContent regardless of the target */
    CHECK_STR(P(b, "messages.1.content.1.text"), "<think>\nunsigned musing\n</think>");
    json_free(b);
    json_free(m.raw);

    m = mk_model(NOVA);
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_HIGH;
    o.temperature = 0.7;
    o.tool_choice = J("\"none\"");
    bool sentinel = false;
    b = build(&m, CTX_MESSAGES, NULL, &o, &sentinel);
    CHECK(json_get(b, "additionalModelRequestFields") == NULL);
    CHECK(json_path(b, "inferenceConfig.temperature") != NULL);
    CHECK_INT(PI(b, "inferenceConfig.maxTokens"), 10000);
    CHECK(sentinel);
    CHECK_STR(P(b, "toolConfig.tools.0.toolSpec.name"), "__no_tools__");
    CHECK(json_path(b, "toolConfig.toolChoice.auto") != NULL);
    CHECK_STR(P(b, "guardrailConfig.guardrailIdentifier"), "arn:aws:bedrock:eu-west-1:123:guardrail/abc");
    CHECK_STR(P(b, "guardrailConfig.guardrailVersion"), "DRAFT");
    CHECK_STR(P(b, "requestMetadata.team"), "core");
    CHECK_INT(json_len(json_get(b, "requestMetadata")), 1);
    json_free(b);
    /* "none" with real tools offered: tools kept, no choice */
    b = build(&m, CTX_MESSAGES, CTX_TOOLS, &o, &sentinel);
    CHECK(!sentinel);
    CHECK_INT(json_len(json_path(b, "toolConfig.tools")), 2);
    CHECK(json_path(b, "toolConfig.toolChoice") == NULL);
    json_free(b);
    /* "none" without tool history: no toolConfig at all */
    b = build(&m, "[{\"role\":\"user\",\"content\":\"hi\"}]", CTX_TOOLS, &o, &sentinel);
    CHECK(json_get(b, "toolConfig") == NULL);
    json_free(b);
    json_free(o.tool_choice);

    /* unknown image types are rejected */
    json_t *jm = J("[{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"mimeType\":\"image/bmp\",\"data\":\"x\"}]}]");
    ai_context_t ctx = {.messages = jm};
    bedrock_request_t r;
    bedrock_build_request(&m, &ctx, NULL, &r);
    CHECK(r.body == NULL);
    CHECK_STR(r.error, "Unknown image type: image/bmp");
    bedrock_request_free(&r);
    json_free(jm);
    json_free(m.raw);
}

static void test_region_and_url(void) {
    unsetenv("AWS_PROFILE");
    unsetenv("AWS_SDK_LOAD_CONFIG");
    unsetenv("AWS_DEFAULT_REGION");
    setenv("AWS_REGION", "eu-central-1", 1);
    ai_model_t eu = mk_model("{\"id\":\"eu.anthropic.claude-opus-4-7\"}");
    ai_model_t us = mk_model("{\"id\":\"us.anthropic.claude-opus-4-7\"}");
    ai_model_t jp = mk_model("{\"id\":\"jp.anthropic.claude-opus-4-7\"}");
    ai_model_t plain = mk_model("{\"id\":\"anthropic.claude-3-5-haiku-20241022-v1:0\"}");
    ai_model_t arn = mk_model("{\"id\":\"arn:aws:bedrock:ap-south-1:123:application-inference-profile/x\"}");
    char *r = bedrock_resolve_region(&eu, NULL);
    CHECK_STR(r, "eu-central-1");
    free(r);
    r = bedrock_resolve_region(&us, NULL);
    CHECK_STR(r, "us-east-1"); /* ambient eu region cannot serve a us. profile */
    free(r);
    r = bedrock_resolve_region(&us, "arn:aws:bedrock:us-west-2:1:guardrail/g");
    CHECK_STR(r, "us-west-2");
    free(r);
    r = bedrock_resolve_region(&plain, NULL);
    CHECK_STR(r, "eu-central-1");
    free(r);
    r = bedrock_resolve_region(&arn, NULL);
    CHECK_STR(r, "ap-south-1");
    free(r);
    setenv("AWS_REGION", "ap-northeast-3", 1);
    r = bedrock_resolve_region(&jp, NULL);
    CHECK_STR(r, "ap-northeast-3");
    free(r);
    unsetenv("AWS_REGION");
    r = bedrock_resolve_region(&jp, NULL);
    CHECK_STR(r, "ap-northeast-1");
    free(r);
    r = bedrock_resolve_region(&plain, NULL);
    CHECK_STR(r, "us-east-1");
    free(r);
    /* the region of AWS's own regional host in baseUrl comes after the ambient and guardrail regions (omp dc0709337a) */
    ai_model_t regional = mk_model("{\"id\":\"anthropic.claude-3-5-haiku-20241022-v1:0\",\"baseUrl\":"
                                   "\"https://bedrock-runtime.eu-west-3.amazonaws.com\"}");
    r = bedrock_resolve_region(&regional, NULL);
    CHECK_STR(r, "eu-west-3");
    free(r);
    r = bedrock_resolve_region(&regional, "arn:aws:bedrock:us-west-2:1:guardrail/g");
    CHECK_STR(r, "us-west-2");
    free(r);
    setenv("AWS_REGION", "ca-central-1", 1);
    r = bedrock_resolve_region(&regional, NULL);
    CHECK_STR(r, "ca-central-1");
    free(r);
    unsetenv("AWS_REGION");
    json_free(regional.raw);

    char *u = bedrock_request_url(&eu, "eu-west-1");
    CHECK_STR(u, "https://bedrock-runtime.eu-west-1.amazonaws.com/model/eu.anthropic.claude-opus-4-7/converse-stream");
    free(u);
    u = bedrock_request_url(&plain, "us-east-1");
    CHECK_STR(u, "https://bedrock-runtime.us-east-1.amazonaws.com/model/anthropic.claude-3-5-haiku-20241022-v1%3A0/"
                 "converse-stream");
    free(u);
    ai_model_t gw = mk_model("{\"id\":\"m.x\",\"baseUrl\":\"http://127.0.0.1:9999/proxy/?key=v\"}");
    u = bedrock_request_url(&gw, "us-east-1");
    CHECK_STR(u, "http://127.0.0.1:9999/proxy/model/m.x/converse-stream?key=v");
    free(u);
    ai_model_t vpc = mk_model("{\"id\":\"m.x\",\"baseUrl\":\"https://vpce-1.bedrock-runtime.us-east-1.vpce.amazonaws.com\"}");
    u = bedrock_request_url(&vpc, "eu-west-1");
    CHECK_STR(u, "https://vpce-1.bedrock-runtime.us-east-1.vpce.amazonaws.com/model/m.x/converse-stream");
    free(u);
    ai_model_t *all[] = {&eu, &us, &jp, &plain, &arn, &gw, &vpc};
    for (size_t i = 0; i < ARRAY_LEN(all); i++) json_free(all[i]->raw);
}

/* ======================================================================
 * stream state machine
 * ====================================================================== */

static sb_t g_events;
static void rec_cb(void *ud, const ai_event_t *ev) {
    sb_appendf(&g_events, "%s", ai_event_name(ev->type));
    if (ev->content_index >= 0) sb_appendf(&g_events, ":%d", ev->content_index);
    if (ev->delta) sb_appendf(&g_events, "(%.*s)", (int)ev->delta_len, ev->delta);
    sb_putc(&g_events, ' ');
}

static void feed_event(bedrock_stream_t *s, const char *type, const char *payload) {
    size_t l;
    unsigned char *f = event_frame(type, payload, &l);
    /* split each frame in two to exercise reassembly */
    bedrock_stream_feed(s, (char *)f, l / 2);
    bedrock_stream_feed(s, (char *)f + l / 2, l - l / 2);
    free(f);
}

static void test_stream_full(void) {
    ai_model_t m = mk_model(SONNET45);
    sb_clear(&g_events);
    bedrock_stream_t s;
    bedrock_stream_init(&s, &m, false, rec_cb, NULL);
    feed_event(&s, "messageStart", "{\"role\":\"assistant\"}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"reasoningContent\":{\"text\":\"Let me\"}}}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"reasoningContent\":{\"text\":\" think\"}}}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"reasoningContent\":{\"signature\":\"AB\"}}}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"reasoningContent\":{\"signature\":\"CD\"}}}");
    feed_event(&s, "contentBlockStop", "{\"contentBlockIndex\":0}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":1,\"delta\":{\"text\":\"Hello\"}}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":1,\"delta\":{\"text\":\" world\"}}");
    feed_event(&s, "contentBlockStop", "{\"contentBlockIndex\":1}");
    feed_event(&s, "contentBlockStart",
               "{\"contentBlockIndex\":2,\"start\":{\"toolUse\":{\"toolUseId\":\"tooluse.abc\",\"name\":\"bash\"}}}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":2,\"delta\":{\"toolUse\":{\"input\":\"{\\\"comm\"}}}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":2,\"delta\":{\"toolUse\":{\"input\":\"and\\\":\\\"ls\\\"}\"}}}");
    feed_event(&s, "contentBlockStop", "{\"contentBlockIndex\":2}");
    feed_event(&s, "messageStop", "{\"stopReason\":\"tool_use\"}");
    feed_event(&s, "metadata", "{\"usage\":{\"inputTokens\":100,\"outputTokens\":20,\"cacheReadInputTokens\":1000,"
                               "\"cacheWriteInputTokens\":10,\"totalTokens\":1130},\"metrics\":{\"latencyMs\":5}}");
    json_t *msg = bedrock_stream_finish(&s, 0, NULL, false);
    bedrock_stream_free(&s);
    CHECK_STR(sb_cstr(&g_events),
              "start thinking_start:0 thinking_delta:0(Let me) thinking_delta:0( think) thinking_end:0 "
              "text_start:1 text_delta:1(Hello) text_delta:1( world) text_end:1 toolcall_start:2 "
              "toolcall_delta:2({\"comm) toolcall_delta:2(and\":\"ls\"}) toolcall_end:2 done ");
    CHECK_STR(P(msg, "api"), "bedrock-converse-stream");
    CHECK_STR(P(msg, "stopReason"), "toolUse");
    CHECK_STR(P(msg, "content.0.type"), "thinking");
    CHECK_STR(P(msg, "content.0.thinking"), "Let me think");
    CHECK_STR(P(msg, "content.0.thinkingSignature"), "ABCD");
    CHECK_STR(P(msg, "content.1.text"), "Hello world");
    CHECK_STR(P(msg, "content.2.id"), "tooluse_abc");
    CHECK_STR(P(msg, "content.2.name"), "bash");
    CHECK_STR(P(msg, "content.2.arguments.command"), "ls");
    CHECK_INT(PI(msg, "usage.input"), 100);
    CHECK_INT(PI(msg, "usage.output"), 20);
    CHECK_INT(PI(msg, "usage.cacheRead"), 1000);
    CHECK_INT(PI(msg, "usage.cacheWrite"), 10);
    CHECK_INT(PI(msg, "usage.totalTokens"), 1130);
    CHECK(json_get_num(json_get(msg, "usage"), "cost", 0) == 0); /* cost is an object */
    CHECK(fabs(json_get_num(json_path(msg, "usage.cost"), "input", 0) - 100 * 3 / 1e6) < 1e-12);
    json_free(msg);
    json_free(m.raw);
}

static void test_stream_errors(void) {
    ai_model_t m = mk_model(SONNET45);
    bedrock_stream_t s;
    /* in-stream exception frame */
    sb_clear(&g_events);
    bedrock_stream_init(&s, &m, false, rec_cb, NULL);
    feed_event(&s, "messageStart", "{\"role\":\"assistant\"}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"partial\"}}");
    hb_t b = {SB_INIT};
    hb_str(&b, ":exception-type", "throttlingException");
    hb_str(&b, ":content-type", "application/json");
    hb_str(&b, ":message-type", "exception");
    size_t l;
    unsigned char *f = frame(&b, "{\"message\":\"Too many requests, please wait.\"}", &l);
    CHECK_INT(bedrock_stream_feed(&s, (char *)f, l), 2);
    free(f);
    sb_free(&b.h);
    json_t *msg = bedrock_stream_finish(&s, 2, NULL, false);
    bedrock_stream_free(&s);
    CHECK_STR(P(msg, "stopReason"), "error");
    CHECK_STR(P(msg, "errorMessage"), "throttlingException: Too many requests, please wait.");
    CHECK_STR(P(msg, "content.0.text"), "partial");
    CHECK(str_ends(sb_cstr(&g_events), "text_end:0 error "));
    json_free(msg);
    CHECK_INT(bedrock_exception_status("throttlingException"), 429);
    CHECK_INT(bedrock_exception_status("InternalServerException"), 500);
    CHECK_INT(bedrock_exception_status(" modelStreamErrorException "), 424);
    CHECK_INT(bedrock_exception_status("SomethingNew"), 400);

    /* error frame with header message */
    bedrock_stream_init(&s, &m, false, NULL, NULL);
    hb_t e = {SB_INIT};
    hb_str(&e, ":message-type", "error");
    hb_str(&e, ":error-code", "InternalFailure");
    hb_str(&e, ":error-message", "oops");
    f = frame(&e, NULL, &l);
    CHECK_INT(bedrock_stream_feed(&s, (char *)f, l), 2);
    free(f);
    sb_free(&e.h);
    msg = bedrock_stream_finish(&s, 2, NULL, false);
    CHECK_STR(P(msg, "errorMessage"), "InternalFailure: oops");
    json_free(msg);
    bedrock_stream_free(&s);

    /* guardrail stop reason */
    bedrock_stream_init(&s, &m, false, NULL, NULL);
    feed_event(&s, "messageStart", "{\"role\":\"assistant\"}");
    feed_event(&s, "messageStop", "{\"stopReason\":\"guardrail_intervened\"}");
    msg = bedrock_stream_finish(&s, 0, NULL, false);
    CHECK_STR(P(msg, "stopReason"), "error");
    CHECK_STR(P(msg, "errorMessage"),
              "Response blocked by Amazon Bedrock guardrail (stop reason: guardrail_intervened).");
    json_free(msg);
    bedrock_stream_free(&s);

    /* user role in messageStart */
    bedrock_stream_init(&s, &m, false, NULL, NULL);
    feed_event(&s, "messageStart", "{\"role\":\"user\"}");
    msg = bedrock_stream_finish(&s, 2, NULL, false);
    CHECK_STR(P(msg, "errorMessage"), "Unexpected assistant message start but got user message start instead");
    json_free(msg);
    bedrock_stream_free(&s);

    /* corrupt frame mid-stream */
    bedrock_stream_init(&s, &m, false, NULL, NULL);
    f = event_frame("messageStart", "{\"role\":\"assistant\"}", &l);
    f[l - 2] ^= 1;
    CHECK_INT(bedrock_stream_feed(&s, (char *)f, l), 2);
    free(f);
    msg = bedrock_stream_finish(&s, 2, NULL, false);
    CHECK_STR(P(msg, "errorMessage"), "message CRC mismatch");
    json_free(msg);
    bedrock_stream_free(&s);

    /* truncated stream */
    bedrock_stream_init(&s, &m, false, NULL, NULL);
    f = event_frame("messageStart", "{\"role\":\"assistant\"}", &l);
    bedrock_stream_feed(&s, (char *)f, l - 5);
    free(f);
    msg = bedrock_stream_finish(&s, 0, NULL, false);
    CHECK_STR(P(msg, "errorMessage"), "truncated message at end of stream");
    json_free(msg);
    bedrock_stream_free(&s);

    /* whole frames, but the body ended before messageStop: a retryable incomplete stream, not a clean stop
     * (omp 3f9a64f2eb) */
    bedrock_stream_init(&s, &m, false, NULL, NULL);
    feed_event(&s, "messageStart", "{\"role\":\"assistant\"}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"half a rep\"}}");
    msg = bedrock_stream_finish(&s, 0, NULL, false);
    CHECK_STR(P(msg, "stopReason"), "error");
    CHECK_STR(P(msg, "errorMessage"), "Bedrock stream closed before a terminal response event (messageStop) was received");
    json_free(msg);
    bedrock_stream_free(&s);

    /* aborted */
    bedrock_stream_init(&s, &m, false, NULL, NULL);
    msg = bedrock_stream_finish(&s, -2, "Request was aborted", true);
    CHECK_STR(P(msg, "stopReason"), "aborted");
    json_free(msg);
    bedrock_stream_free(&s);

    /* sentinel tool call dropped; tool_use stop becomes stop */
    bedrock_stream_init(&s, &m, true, NULL, NULL);
    feed_event(&s, "messageStart", "{\"role\":\"assistant\"}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"text\":\"answer\"}}");
    feed_event(&s, "contentBlockStop", "{\"contentBlockIndex\":0}");
    feed_event(&s, "contentBlockStart",
               "{\"contentBlockIndex\":1,\"start\":{\"toolUse\":{\"toolUseId\":\"t1\",\"name\":\"__no_tools__\"}}}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":1,\"delta\":{\"toolUse\":{\"input\":\"{}\"}}}");
    feed_event(&s, "contentBlockStop", "{\"contentBlockIndex\":1}");
    feed_event(&s, "messageStop", "{\"stopReason\":\"tool_use\"}");
    msg = bedrock_stream_finish(&s, 0, NULL, false);
    CHECK_STR(P(msg, "stopReason"), "stop");
    CHECK_INT(json_len(json_get(msg, "content")), 1);
    json_free(msg);
    bedrock_stream_free(&s);

    /* max_tokens + input transformations + signature-only reasoning */
    bedrock_stream_init(&s, &m, false, NULL, NULL);
    feed_event(&s, "messageStart", "{\"role\":\"assistant\"}");
    feed_event(&s, "contentBlockDelta", "{\"contentBlockIndex\":0,\"delta\":{\"reasoningContent\":{\"signature\":\"S\"}}}");
    feed_event(&s, "contentBlockStop", "{\"contentBlockIndex\":0}");
    feed_event(&s, "messageStop", "{\"stopReason\":\"max_tokens\",\"additionalModelResponseFields\":{"
                                  "\"input_transformations\":[{\"type\":\"drop\",\"reason\":\"prefix_binding_mismatch\","
                                  "\"path\":\"/messages/1\"},{\"nope\":1}]}}");
    msg = bedrock_stream_finish(&s, 0, NULL, false);
    CHECK_STR(P(msg, "stopReason"), "length");
    CHECK_STR(P(msg, "content.0.type"), "thinking");
    CHECK_STR(P(msg, "content.0.thinking"), "");
    CHECK_STR(P(msg, "content.0.thinkingSignature"), "S");
    CHECK_INT(json_len(json_get(msg, "inputTransformations")), 1);
    CHECK_STR(P(msg, "inputTransformations.0.path"), "/messages/1");
    json_free(msg);
    bedrock_stream_free(&s);

    CHECK_STR(bedrock_map_stop_reason("end_turn"), "stop");
    CHECK_STR(bedrock_map_stop_reason("stop_sequence"), "stop");
    CHECK_STR(bedrock_map_stop_reason("model_context_window_exceeded"), "length");
    CHECK_STR(bedrock_map_stop_reason("content_filtered"), "error");
    CHECK(bedrock_is_prefix_binding_error(
        "Bedrock HTTP 400: messages.1.content.0: Invalid `signature` in `thinking` block: bound to a different conversation"));
    CHECK(!bedrock_is_prefix_binding_error("Invalid signature in thinking block"));
    CHECK(!bedrock_is_prefix_binding_error(
        "compaction_block: invalid signature in thinking block, prefix_mismatch_behavior"));
    json_free(m.raw);
}

/* ======================================================================
 * credentials helpers (offline parts)
 * ====================================================================== */

static void test_aws_config_helpers(void) {
    json_t *ini = aws_ini_parse("# c\n[default]\nregion = us-west-2\n[profile dev]\nsso_session=corp\n"
                                "  aws_access_key_id = AK \n;x\n[sso-session corp]\nsso_region=eu-west-1\n"
                                "[default]\noutput=json\n");
    CHECK_STR(P(ini, "default.region"), "us-west-2");
    CHECK_STR(P(ini, "default.output"), "json");
    CHECK_STR(P(ini, "dev.aws_access_key_id"), "AK");
    CHECK_STR(json_get_str(json_get(ini, "sso-session:corp"), "sso_region"), "eu-west-1");
    json_free(ini);

    char *err = NULL;
    strvec_t t = aws_tokenize_command("/bin/tool --a 'x y' \"q\\\"z\\n\" a\\ b", &err);
    CHECK(err == NULL);
    CHECK_INT(t.n, 5);
    if (t.n == 5) {
        CHECK_STR(t.v[2], "x y");
        CHECK_STR(t.v[3], "q\"z\\n");
        CHECK_STR(t.v[4], "a b");
    }
    strvec_free(&t);
    t = aws_tokenize_command("tool 'open", &err);
    CHECK(err != NULL);
    free(err);
    strvec_free(&t);

    CHECK_INT(aws_parse_iso8601("2015-08-30T12:36:00Z"), 1440938160000LL);
    CHECK_INT(aws_parse_iso8601("2015-08-30T12:36:00.5Z"), 1440938160500LL);
    CHECK_INT(aws_parse_iso8601("2015-08-30T14:36:00+02:00"), 1440938160000LL);
    CHECK_INT(aws_parse_iso8601("garbage"), -1);
    char *x = aws_xml_tag("<R><Credentials><AccessKeyId>AK</AccessKeyId><SessionToken>a&amp;b</SessionToken>"
                          "</Credentials></R>",
                          "SessionToken");
    CHECK_STR(x, "a&b");
    free(x);

    /* env credentials are resolved without touching the network and cached */
    setenv("AWS_ACCESS_KEY_ID", "AKENV", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "SKENV", 1);
    setenv("AWS_SESSION_TOKEN", "TOK", 1);
    aws_creds_t c;
    CHECK_INT(aws_resolve_credentials(NULL, "us-east-1", NULL, &c, &err), 0);
    CHECK_STR(c.access_key_id, "AKENV");
    CHECK_STR(c.session_token, "TOK");
    aws_creds_free(&c);
    CHECK(aws_has_credential_source());
    unsetenv("AWS_ACCESS_KEY_ID");
    unsetenv("AWS_SECRET_ACCESS_KEY");
    unsetenv("AWS_SESSION_TOKEN");
    aws_clear_credential_cache();
    setenv("AWS_BEARER_TOKEN_BEDROCK", "BT", 1);
    CHECK_STR(aws_bearer_token(NULL), "BT");
    CHECK_STR(aws_bearer_token("explicit"), "explicit");
    unsetenv("AWS_BEARER_TOKEN_BEDROCK");
}

int main(void) {
    g_defaults = J("{\"promptCacheMaximumCheckpoints\":4,\"promptCacheMinimumTokens\":0,\"promptCacheMode\":\"explicit\","
                   "\"streamRevision\":\"possible\",\"supportsForcedToolChoice\":true,"
                   "\"supportsLongPromptCacheRetention\":false,\"supportsServerCompaction\":false,"
                   "\"supportsThinkingBindingControls\":true}");
    unsetenv("PI_CACHE_RETENTION");
    unsetenv("AWS_BEDROCK_FORCE_CACHE");
    test_sigv4_suite();
    test_sigv4_helpers();
    test_sigv4_bedrock_sign();
    test_eventstream_decode();
    test_eventstream_streaming();
    test_request_budget_model();
    test_request_adaptive_models();
    test_request_other_models();
    test_region_and_url();
    test_stream_full();
    test_stream_errors();
    test_aws_config_helpers();
    sb_free(&g_events);
    json_free(g_defaults);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
