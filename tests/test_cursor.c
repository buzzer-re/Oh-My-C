/* test_cursor.c - protobuf codec, Cursor schema and Connect framing. */
#include "ai/cursor_proto.h"
#include "ai/pb.h"

static int fails;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                            \
        }                                                                       \
    } while (0)

static char *hex(const void *p, size_t n) { return hex_encode(p, n); }

static void check_hex(const char *msg, const json_t *val, const char *want) {
    size_t len;
    char *bytes = cursor_encode(msg, val, &len);
    char *h = hex(bytes, len);
    if (strcmp(h, want) != 0) {
        fprintf(stderr, "encode %s: got %s want %s\n", msg, h, want);
        fails++;
    }
    free(h);
    free(bytes);
}

static json_t *parse(const char *s) {
    json_t *j = json_parse_str(s);
    if (!j) die("bad test json: %s", s);
    return j;
}

static void test_varint(void) {
    sb_t b = SB_INIT;
    pb_put_varint(&b, 300);
    char *h = hex(b.s, b.len);
    CHECK(strcmp(h, "ac02") == 0);
    free(h);
    size_t pos = 0;
    uint64_t v = 0;
    CHECK(pb_get_varint((const unsigned char *)b.s, b.len, &pos, &v) == 0 && v == 300 && pos == 2);
    pos = 0;
    CHECK(pb_get_varint((const unsigned char *)b.s, 1, &pos, &v) == -1); /* truncated */
    sb_free(&b);
    unsigned char overlong[11];
    memset(overlong, 0xff, sizeof overlong);
    pos = 0;
    CHECK(pb_get_varint(overlong, sizeof overlong, &pos, &v) == -1);
}

static void test_encode(void) {
    json_t *j;
    /* empty message variant still encodes (explicit presence): field 7, LEN 0 */
    j = parse("{\"clientHeartbeat\":{}}");
    check_hex("AgentClientMessage", j, "3a00");
    json_free(j);
    /* proto3 defaults are skipped */
    j = parse("{\"command\":\"\",\"timeout\":0,\"toolCallId\":\"x\"}");
    check_hex("ShellArgs", j, "220178");
    json_free(j);
    /* negative int32 sign-extends to 10 bytes */
    j = parse("{\"timeout\":-1}");
    check_hex("ShellArgs", j, "18ffffffffffffffffff01");
    json_free(j);
    /* optional int64 at zero is still present */
    j = parse("{\"inputTokens\":0,\"outputTokens\":150}");
    check_hex("TurnEndedUpdate", j, "0800109601");
    json_free(j);
    /* nested: interactionUpdate.textDelta.text = "hi" */
    j = parse("{\"interactionUpdate\":{\"textDelta\":{\"text\":\"hi\"}}}");
    check_hex("AgentServerMessage", j, "0a060a040a026869");
    json_free(j);
    /* map<string,bytes> */
    j = parse("{\"name\":\"t\",\"args\":{\"a\":\"\\u0001\"}}");
    check_hex("McpArgs", j, "0a01741206" "0a0161120101");
    json_free(j);
    /* repeated message */
    j = parse("{\"modelId\":\"m\",\"parameters\":[{\"id\":\"reasoning\",\"value\":\"low\"}]}");
    check_hex("RequestedModel", j, "0a016d1a100a09726561736f6e696e6712036c6f77");
    json_free(j);
}

static void test_roundtrip(void) {
    /* bytes with NULs, unknown fields and nested messages survive decode -> encode */
    json_t *in = parse("{\"id\":7,\"setBlobArgs\":{\"blobId\":\"\\u0000\\u0001\",\"blobData\":\"abc\"}}");
    size_t len;
    char *bytes = cursor_encode("KvServerMessage", in, &len);
    /* append an unknown field 99 (varint 5) */
    sb_t b = SB_INIT;
    sb_appendn(&b, bytes, len);
    pb_put_tag(&b, 99, 0);
    pb_put_varint(&b, 5);
    char *err = NULL;
    json_t *out = cursor_decode("KvServerMessage", b.s, b.len, &err);
    CHECK(out && !err);
    CHECK(json_get_int(out, "id", 0) == 7);
    json_t *blob = json_path(out, "setBlobArgs.blobId");
    CHECK(json_is_str(blob) && blob->u.str.len == 2 && blob->u.str.s[0] == 0 && blob->u.str.s[1] == 1);
    CHECK(json_len(json_get(out, "$unknown")) == 1);
    size_t len2;
    char *again = cursor_encode("KvServerMessage", out, &len2);
    CHECK(len2 == b.len && memcmp(again, b.s, len2) == 0);
    free(again);
    free(bytes);
    sb_free(&b);
    json_free(in);
    json_free(out);

    /* packed repeated + map<string,int32> */
    json_t *tree = parse("{\"absPath\":\"/a\",\"childrenDirs\":[{\"absPath\":\"/a/b\"}],"
                         "\"fullSubtreeExtensionCounts\":{\"c\":3},\"numFiles\":2}");
    bytes = cursor_encode("LsDirectoryTreeNode", tree, &len);
    out = cursor_decode("LsDirectoryTreeNode", bytes, len, &err);
    CHECK(out && json_equal(out, tree));
    free(bytes);
    json_free(out);
    json_free(tree);

    /* malformed input is rejected, not crashed on */
    const char junk[] = {0x0a, 0x7f, 0x01};
    out = cursor_decode("AgentServerMessage", junk, sizeof junk, &err);
    CHECK(!out && err);
    free(err);
    err = NULL;
    const char badwt[] = {0x0f};
    out = cursor_decode("AgentServerMessage", badwt, sizeof badwt, &err);
    CHECK(!out && err);
    free(err);
    err = NULL;
    /* wire-type mismatch on a known field */
    const char mism[] = {0x08, 0x01}; /* field 1 as varint, schema says message */
    out = cursor_decode("AgentServerMessage", mism, sizeof mism, &err);
    CHECK(!out && err);
    free(err);

    /* deep nesting is bounded */
    sb_t deep = SB_INIT;
    for (int i = 0; i < 300; i++) {
        sb_t nb = SB_INIT;
        pb_put_len_bytes(&nb, 2, deep.s, deep.len); /* LsDirectoryTreeNode.childrenDirs */
        sb_free(&deep);
        deep = nb;
    }
    err = NULL;
    out = cursor_decode("LsDirectoryTreeNode", deep.s, deep.len, &err);
    CHECK(!out && err);
    free(err);
    sb_free(&deep);
}

static void test_value(void) {
    json_t *v = parse("{\"a\":[1,2.5,\"x\",true,null],\"b\":{\"c\":\"d\"}}");
    sb_t b = SB_INIT;
    pb_value_encode(v, &b);
    json_t *back = pb_value_decode(b.s, b.len);
    CHECK(back && json_equal(v, back));
    json_free(back);
    sb_free(&b);
    json_free(v);
    sb_t s = SB_INIT;
    json_t *str = json_str("hello");
    pb_value_encode(str, &s);
    char *h = hex(s.s, s.len);
    CHECK(strcmp(h, "1a0568656c6c6f") == 0);
    free(h);
    json_free(str);
    sb_free(&s);
}

static void test_connect(void) {
    sb_t frames = SB_INIT;
    connect_frame(&frames, 0, "abc", 3);
    connect_frame(&frames, CONNECT_FLAG_END_STREAM, "{}", 2);
    char *h = hex(frames.s, 5);
    CHECK(strcmp(h, "0000000003") == 0);
    free(h);
    connect_decoder_t d;
    connect_decoder_init(&d, 0);
    int flags;
    const char *payload;
    size_t len;
    /* byte-by-byte feeding */
    int got = 0;
    for (size_t i = 0; i < frames.len; i++) {
        connect_decoder_feed(&d, frames.s + i, 1);
        int r;
        while ((r = connect_decoder_next(&d, &flags, &payload, &len)) == 1) {
            if (got == 0) CHECK(flags == 0 && len == 3 && memcmp(payload, "abc", 3) == 0);
            if (got == 1) CHECK(flags == CONNECT_FLAG_END_STREAM && len == 2);
            got++;
        }
        CHECK(r == 0);
    }
    CHECK(got == 2);
    connect_decoder_free(&d);
    connect_decoder_init(&d, 2);
    connect_decoder_feed(&d, frames.s, frames.len);
    CHECK(connect_decoder_next(&d, &flags, &payload, &len) == -1 && len == 3);
    connect_decoder_free(&d);
    sb_free(&frames);
}

static void test_errors(void) {
    json_t *e = parse("{\"code\":\"invalid_argument\",\"message\":\"Error\"}");
    char *s = connect_format_end_stream_error(e);
    CHECK(strcmp(s, "Connect error invalid_argument: Error") == 0);
    free(s);
    json_free(e);
    e = parse("{\"code\":\"unavailable\",\"message\":\"Error\",\"details\":[{\"type\":\"aiserver.v1.ErrorDetails\","
              "\"debug\":{\"details\":{\"isRetryable\":true}}}]}");
    s = connect_format_end_stream_error(e);
    CHECK(strcmp(s, "Connect error unavailable: Error [details: aiserver.v1.ErrorDetails: "
                    "{\"details\":{\"isRetryable\":true}}]") == 0);
    CHECK(connect_has_retryable_cursor_detail(json_get(e, "details")));
    free(s);
    json_free(e);
    e = parse("{\"code\":\"internal\",\"message\":\"\",\"reason\":\"boom\"}");
    s = connect_format_end_stream_error(e);
    CHECK(strcmp(s, "Connect error internal: Unknown error [trailer: {\"reason\":\"boom\"}]") == 0);
    free(s);
    json_free(e);
}

int main(void) {
    test_varint();
    test_encode();
    test_roundtrip();
    test_value();
    test_connect();
    test_errors();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
