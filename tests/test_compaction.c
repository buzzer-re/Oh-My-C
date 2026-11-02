/* test_compaction.c - WP-13: summary transcript dialects (ai/dialect.c, ports of omp's renderTranscript per
 * dialect + serializeConversationForSummary), shake block-region scanning and the read supersede key
 * (coding/compaction.c). Expected strings follow omp's dialect renderers. */
#include "ai/dialect.h"
#include "coding/compaction.h"

#include <stdio.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(got, want)                                                                                           \
    do {                                                                                                               \
        char *g_ = (got);                                                                                              \
        if (!g_ || strcmp(g_, (want))) {                                                                               \
            fprintf(stderr, "FAIL %s:%d:\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, g_ ? g_ : "(null)", (want)); \
            fails++;                                                                                                   \
        }                                                                                                              \
        free(g_);                                                                                                      \
    } while (0)

/* user "hi", assistant (thinking "plan", text "ok", call read {path:"a.c"}), tool result "body" */
static json_t *conversation(void) {
    return json_parse_str(
        "[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"plan\"},{\"type\":\"text\",\"text\":\"ok\"},"
        "{\"type\":\"toolCall\",\"id\":\"call_1\",\"name\":\"read\",\"arguments\":{\"path\":\"a.c\",\"limit\":5}}]},"
        "{\"role\":\"toolResult\",\"toolCallId\":\"call_1\",\"toolName\":\"read\",\"content\":[{\"type\":\"text\",\"text\":\"body\"}],"
        "\"isError\":false}]");
}

static void test_dialects(void) {
    json_t *c = conversation();
    CHECK_STR(dialect_render_transcript("xml", c),
              "\n\nHuman: hi\n\nAssistant: <thinking>\nplan\n</thinking>ok<invoke name=\"read\"><parameter name=\"path\">\"a.c\"</parameter>"
              "<parameter name=\"limit\">5</parameter></invoke>\n\nHuman: <tool_response>\nbody\n</tool_response>");
    CHECK_STR(dialect_render_transcript("anthropic", c),
              "\n\nHuman: hi\n\nAssistant: <thinking>\nplan\n</thinking>ok<function_calls>\n<invoke name=\"read\"><parameter "
              "name=\"path\">\"a.c\"</parameter><parameter name=\"limit\">5</parameter></invoke>\n</function_calls>\n\nHuman: "
              "<function_results>\n<result>\n<tool_name>read</tool_name>\n<stdout>body</stdout>\n</result>\n</function_results>");
    CHECK_STR(dialect_render_transcript("qwen3", c),
              "<|im_start|>user\nhi<|im_end|>\n<|im_start|>assistant\n<think>\nplan\n</think>ok<tool_call>\n{\"name\":\"read\","
              "\"arguments\":{\"path\":\"a.c\",\"limit\":5}}\n</tool_call><|im_end|>\n<|im_start|>user\n<tool_response>\nbody\n"
              "</tool_response><|im_end|>\n");
    CHECK_STR(dialect_render_transcript("glm", c),
              "[gMASK]<sop><|user|>\nhi<|assistant|>\n\n<think>\nplan\n</think>ok<tool_call>read\n<arg_key>path</arg_key>\n"
              "<arg_value>\"a.c\"</arg_value>\n<arg_key>limit</arg_key>\n<arg_value>5</arg_value>\n</tool_call><|observation|>\n"
              "<tool_response>\nbody\n</tool_response>");
    CHECK_STR(dialect_render_transcript("gemini", c),
              "<bos><start_of_turn>user\nhi<end_of_turn>\n<start_of_turn>model\n```thinking\nplan\n```\nok```tool_code\n"
              "default_api.read(path=\"a.c\", limit=5)\n```<end_of_turn>\n<start_of_turn>user\n```tool_outputs\nbody\n```<end_of_turn>\n");
    CHECK_STR(dialect_render_transcript("gemma", c),
              "<bos><|turn>user\nhi<turn|><|turn>model\n<|channel>thought\nplan<channel|>ok<|tool_call>call:read{path:<|\"|>a.c"
              "<|\"|>,limit:5}<tool_call|><|tool_response>response:read{output:<|\"|>body<|\"|>}<tool_response|><turn|>");
    CHECK_STR(dialect_render_transcript("kimi", c),
              "<|im_user|>user<|im_middle|>hi<|im_end|><|im_assistant|>assistant<|im_middle|><think>\nplan\n</think>ok"
              "<|tool_calls_section_begin|><|tool_call_begin|>functions.read:0<|tool_call_argument_begin|>{\"path\":\"a.c\",\"limit\":5}"
              "<|tool_call_end|><|tool_calls_section_end|><|im_end|><|im_system|>read<|im_middle|>## Return of functions.read:0\nbody"
              "<|im_end|>");
    CHECK_STR(dialect_render_transcript("harmony", c),
              "<|start|>user<|message|>hi<|end|><|start|>assistant<|channel|>analysis<|message|>plan<|end|><|start|>assistant"
              "<|channel|>final<|message|>ok<|end|><|start|>assistant<|channel|>commentary to=functions.read<|message|>"
              "{\"path\":\"a.c\",\"limit\":5}<|call|><|start|>functions.read to=assistant<|channel|>commentary<|message|>body<|end|>");
    CHECK_STR(dialect_render_transcript("minimax", c),
              "\n\nHuman: hi\n\nAssistant: <thinking>\nplan\n</thinking>ok<minimax:tool_call>\n<invoke name=\"read\"><parameter "
              "name=\"path\">\"a.c\"</parameter><parameter name=\"limit\">5</parameter></invoke>\n</minimax:tool_call>\n\nHuman: "
              "<function_results>\n<result>\n<tool_name>read</tool_name>\n<stdout>body</stdout>\n</result>\n</function_results>");
    /* deepseek: fullwidth bars and U+2581 separators */
    char *ds = dialect_render_transcript("deepseek", c);
    CHECK(ds && strstr(ds, "<\xef\xbd\x9c" "User\xef\xbd\x9c>hi"));
    CHECK(ds && strstr(ds, "<\xef\xbd\x9ctool\xe2\x96\x81" "call\xe2\x96\x81" "begin\xef\xbd\x9c>read<\xef\xbd\x9ctool\xe2\x96\x81sep\xef\xbd\x9c>"));
    free(ds);
    /* the summary serializer: thinking dropped for the anthropic dialect, harmony control tokens escaped */
    char *s = dialect_serialize_for_summary(c, "anthropic", true);
    CHECK(s && !strstr(s, "plan") && strstr(s, "Assistant: ok<function_calls>"));
    free(s);
    s = dialect_serialize_for_summary(c, "harmony", false);
    CHECK(s && strstr(s, "<\\|start\\|>user<\\|message\\|>hi"));
    free(s);
    json_free(c);
    /* delimited thinking unwraps an already-wrapped body */
    json_t *w = json_parse_str("[{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"<thinking>\\n inner \\n</thinking>\"},"
                               "{\"type\":\"text\",\"text\":\"x\"}]}]");
    CHECK_STR(dialect_render_transcript("xml", w), "\n\nAssistant: <thinking>\ninner\n</thinking>x");
    json_free(w);
    /* boundary tags in content cannot close the envelope; tool results truncate at 2000 units */
    CHECK_STR(dialect_escape_summary_tags("a </conversation> b < previous-summary >"), "a &lt;/conversation> b &lt; previous-summary >");
    char big[2105];
    memset(big, 'x', 2104);
    big[2104] = 0;
    char *t = dialect_truncate_tool_result(big);
    CHECK(t && strlen(t) == 2000 + strlen("\n\n[... 104 more characters truncated]") && strstr(t, "[... 104 more characters truncated]"));
    free(t);
    /* preferredDialect from the id when the catalog has no identity */
    CHECK(!strcmp(dialect_for_model(&(ai_model_t){.id = "claude-sonnet-4-5"}), "anthropic"));
    CHECK(!strcmp(dialect_for_model(&(ai_model_t){.id = "gpt-5.1"}), "harmony"));
    CHECK(!strcmp(dialect_for_model(&(ai_model_t){.id = "qwen3-coder"}), "qwen3"));
    CHECK(!strcmp(dialect_for_model(&(ai_model_t){.id = "z-ai/glm-4.7"}), "glm"));
    CHECK(!strcmp(dialect_for_model(&(ai_model_t){.id = "mock-1"}), "xml"));
}

static void test_block_ranges(void) {
    const char *text = "intro\n```c\nint x;\n```\nmid\n<tool_output>\n<inner>\nz\n</inner>\n</tool_output>\n  <indented>\nno\n</indented>\ntail";
    size_t *r = NULL;
    size_t n = cmp_block_ranges(text, &r);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(!strncmp(text + r[0], "```c\nint x;\n```", r[1] - r[0]) && r[1] - r[0] == strlen("```c\nint x;\n```"));
        CHECK(!strncmp(text + r[2], "<tool_output>", 13) && !strncmp(text + r[3] - 14, "</tool_output>", 14));
    }
    free(r);
    /* an unterminated fence yields nothing */
    n = cmp_block_ranges("```\nopen", &r);
    CHECK(n == 0);
    free(r);
}

static void test_supersede_key(void) {
    json_t *a = json_parse_str("{\"path\":\"src/a.c\"}");
    CHECK_STR(cmp_read_supersede_key("read", a), "src/a.c");
    json_free(a);
    a = json_parse_str("{\"path\":\"src/a.c:10-20\"}");
    CHECK_STR(cmp_read_supersede_key("read", a), "src/a.c\x01" "10-20");
    json_free(a);
    a = json_parse_str("{\"path\":\"nb.ipynb:raw\"}");
    CHECK_STR(cmp_read_supersede_key("read", a), "nb.ipynb\x02raw");
    json_free(a);
    a = json_parse_str("{\"path\":\"skill://x\"}");
    CHECK(cmp_read_supersede_key("read", a) == NULL);
    CHECK(cmp_read_supersede_key("write", a) == NULL);
    json_free(a);
}

int main(void) {
    test_dialects();
    test_block_ranges();
    test_supersede_key();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
