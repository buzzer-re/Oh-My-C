/* test_toolcmd.c - tool commands (tools/toolcli.c, tools/toolcmd.c): schema-derived argv binding, help, the runner's
 * usage errors, and the command-line analysis (cases from omp's tool-cli.test.ts and tool-commands.test.ts). */
#include "core/core.h"
#include "core/json.h"
#include "tools/toolcmd.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static const char *SCHEMA =
    "{\"type\":\"object\",\"properties\":{"
    "\"mode\":{\"type\":\"string\",\"enum\":[\"fast\",\"safe\"],\"description\":\"Execution mode.\"},"
    "\"count\":{\"type\":\"integer\",\"description\":\"Number of passes.\"},"
    "\"settings\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\",\"description\":\"Nested label.\"},"
    "\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"label\"]},"
    "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"one\",\"two\"]}}},"
    "\"required\":[\"mode\",\"settings\"]}";

/* parse argv (NULL-terminated) against schema; the args as compact JSON, or "ERR" on a usage error */
static char *parse(const char *schema_text, const char *const *argv, const char *stdin_text) {
    json_t *schema = json_parse_str(schema_text);
    strvec_t av = {0};
    for (size_t i = 0; argv[i]; i++) strvec_push(&av, argv[i]);
    toolcli_opts_t o = {.cwd = "/", .stdin_text = stdin_text, .stdin_len = stdin_text ? strlen(stdin_text) : 0};
    char *err = NULL;
    json_t *args = toolcli_parse_args(schema, &av, &o, &err);
    char *r = args ? json_dump(args, 0) : xstrdup("ERR");
    json_free(args);
    free(err);
    strvec_free(&av);
    json_free(schema);
    return r;
}

static bool parses_to(const char *schema, const char *const *argv, const char *stdin_text, const char *want) {
    char *got = parse(schema, argv, stdin_text);
    json_t *a = json_parse_str(got), *b = json_parse_str(want);
    bool ok = !strcmp(want, "ERR") ? !strcmp(got, "ERR") : a && b && json_equal(a, b);
    if (!ok) fprintf(stderr, "  got %s, want %s\n", got, want);
    json_free(a);
    json_free(b);
    free(got);
    return ok;
}

static json_t *g_called;

static void fixture_call(void *ud, json_t *args, toolcli_result_t *res) {
    json_free(g_called);
    g_called = json_clone(args);
    res->out = json_dump(args, 0);
    res->err = xstrdup("");
    res->exit_code = 0;
}

static void run(const char *const *argv, toolcli_result_t *res) {
    json_t *schema = json_parse_str("{\"type\":\"object\",\"properties\":{\"i\":{\"type\":\"string\",\"description\":"
                                    "\"Short present-participle intent for this call.\"},\"text\":{\"type\":\"string\"}},"
                                    "\"required\":[\"i\",\"text\"]}");
    strvec_t av = {0};
    for (size_t i = 0; argv[i]; i++) strvec_push(&av, argv[i]);
    toolcli_opts_t o = {.cwd = "/"};
    toolcli_host_t host = {.description = "Run fixture.", .schema = schema, .call = fixture_call};
    memset(res, 0, sizeof *res);
    toolcli_run("fixture", &av, &o, &host, res);
    strvec_free(&av);
    json_free(schema);
}

int main(void) {
    /* binding: positionals, flags, booleans, arrays, dotted keys */
    CHECK(parses_to(SCHEMA,
                    (const char *const[]){"safe", "--count", "4", "--settings.label=fixture", "--settings.enabled", "--tags",
                                          "one", "--tags=two", NULL},
                    NULL, "{\"mode\":\"safe\",\"count\":4,\"settings\":{\"label\":\"fixture\",\"enabled\":true},\"tags\":[\"one\",\"two\"]}"));
    CHECK(parses_to(SCHEMA, (const char *const[]){"safe", "--settings.label", "fixture", "--no-settings.enabled", NULL}, NULL,
                    "{\"mode\":\"safe\",\"settings\":{\"label\":\"fixture\",\"enabled\":false}}"));
    /* malformed scalars and enum members */
    CHECK(parses_to(SCHEMA, (const char *const[]){"bogus", NULL}, NULL, "ERR"));
    CHECK(parses_to(SCHEMA, (const char *const[]){"--count", "1.5", NULL}, NULL, "ERR"));
    CHECK(parses_to(SCHEMA, (const char *const[]){"--count=abc", NULL}, NULL, "ERR"));
    CHECK(parses_to(SCHEMA, (const char *const[]){"safe", "--settings.enabled=maybe", "--settings.label", "ok", NULL}, NULL, "ERR"));
    const char *num = "{\"type\":\"object\",\"properties\":{\"ratio\":{\"type\":\"number\"}}}";
    CHECK(parses_to(num, (const char *const[]){"--ratio", "x", NULL}, NULL, "ERR"));
    CHECK(parses_to(num, (const char *const[]){"--ratio", "0.5", NULL}, NULL, "{\"ratio\":0.5}"));
    /* negation collisions are JSON-only */
    const char *coll = "{\"type\":\"object\",\"properties\":{\"cache\":{\"type\":\"boolean\"},\"no_cache\":{\"type\":\"boolean\"}}}";
    CHECK(parses_to(coll, (const char *const[]){"--no-cache", NULL}, NULL, "ERR"));
    CHECK(parses_to(coll, (const char *const[]){"--json", "{\"cache\":false,\"no_cache\":true}", NULL}, NULL,
                    "{\"cache\":false,\"no_cache\":true}"));
    CHECK(parses_to("{\"type\":\"object\",\"properties\":{\"no_cache\":{\"type\":\"boolean\"}}}",
                    (const char *const[]){"--no-cache", NULL}, NULL, "{\"no_cache\":true}"));
    /* flag-populated positionals, surplus literals, `--` */
    CHECK(parses_to(SCHEMA, (const char *const[]){"--mode", "fast", "--settings.label", "ok", NULL}, NULL,
                    "{\"mode\":\"fast\",\"settings\":{\"label\":\"ok\"}}"));
    CHECK(parses_to(SCHEMA, (const char *const[]){"fast", "extra", "--settings.label", "ok", NULL}, NULL, "ERR"));
    CHECK(parses_to("{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}",
                    (const char *const[]){"--", "--not-a-flag", NULL}, NULL, "{\"text\":\"--not-a-flag\"}"));
    /* stdin as a literal positional, or a JSON object bound to the sole required free-form object */
    CHECK(parses_to(SCHEMA, (const char *const[]){"-", "--settings.label", "stdin", NULL}, "fast\n",
                    "{\"mode\":\"fast\",\"settings\":{\"label\":\"stdin\"}}"));
    const char *obj = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"object\"},\"limit\":{\"type\":\"integer\"}},"
                      "\"required\":[\"query\"]}";
    CHECK(parses_to(obj, (const char *const[]){"-", "--limit", "5", NULL}, "{\"where\":{\"id\":1}}",
                    "{\"query\":{\"where\":{\"id\":1}},\"limit\":5}"));
    CHECK(parses_to(obj, (const char *const[]){"-", NULL}, "{\"query\":{\"id\":1}}", "{\"query\":{\"id\":1}}"));
    /* only the exact harness intent description is harness-owned */
    CHECK(parses_to("{\"type\":\"object\",\"properties\":{\"i\":{\"type\":\"string\",\"description\":\"Short present-participating "
                    "intent for this call.\"},\"text\":{\"type\":\"string\"}},\"required\":[\"i\",\"text\"]}",
                    (const char *const[]){"one", "two", NULL}, NULL, "{\"i\":\"one\",\"text\":\"two\"}"));
    CHECK(parses_to("{\"type\":\"object\",\"properties\":{\"i\":{\"type\":\"string\",\"description\":\"Short present-participle "
                    "intent for this call.\"},\"text\":{\"type\":\"string\"}},\"required\":[\"i\",\"text\"]}",
                    (const char *const[]){"hello", NULL}, NULL, "{\"text\":\"hello\"}"));

    /* help */
    json_t *hs = json_parse_str("{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to "
                                "handle.\"},\"force\":{\"type\":\"boolean\"},\"tags\":{\"type\":\"array\",\"items\":{\"type\":"
                                "\"string\",\"enum\":[\"one\",\"two\"]}}},\"required\":[\"text\"]}");
    char *help = toolcli_render_help("fixture", NULL, hs);
    CHECK(strstr(help, "Usage:\n  fixture <text> [OPTIONS] [@FILE] [-]"));
    CHECK(strstr(help, "  <text> <STRING>  Text to handle."));
    CHECK(strstr(help, "--force / --no-force"));
    CHECK(strstr(help, "--tags {one|two}..."));
    free(help);
    json_free(hs);

    /* runner */
    toolcli_result_t r;
    run((const char *const[]){"--help", NULL}, &r);
    CHECK(r.exit_code == 0 && r.help && str_starts(r.out, "fixture") && !g_called);
    toolcli_result_free(&r);
    run((const char *const[]){"hello", NULL}, &r);
    json_t *want = json_parse_str("{\"text\":\"hello\",\"i\":\"Invoking fixture\"}");
    CHECK(r.exit_code == 0 && g_called && json_equal(g_called, want));
    json_free(want);
    toolcli_result_free(&r);
    json_free(g_called);
    g_called = NULL;
    run((const char *const[]){NULL}, &r);
    CHECK(r.exit_code == 2 && !strcmp(r.err, "fixture: missing required argument `text`\nfixture: run `fixture --help`\n") && !g_called);
    toolcli_result_free(&r);
    run((const char *const[]){"--bogus", NULL}, &r);
    CHECK(r.exit_code == 2 && !strcmp(r.err, "fixture: unknown flag `--bogus`\nfixture: run `fixture --help`\n"));
    toolcli_result_free(&r);
    run((const char *const[]){"hello", "--help", NULL}, &r);
    CHECK(r.exit_code == 2 && !strcmp(r.err, "fixture: --help cannot be combined with invocation arguments\n"));
    toolcli_result_free(&r);

    /* command names and lines */
    CHECK(toolcmd_name_ok("lsp") && toolcmd_name_ok("mcp__srv__tool") && toolcmd_name_ok("a.b-c"));
    CHECK(!toolcmd_name_ok("cd") && !toolcmd_name_ok("echo") && !toolcmd_name_ok("1x") && !toolcmd_name_ok("a/b"));
    strvec_t names = {0};
    strvec_push(&names, "lsp");
    strvec_push(&names, "resolve");
    CHECK(toolcmd_is_line("lsp status", &names));
    CHECK(toolcmd_is_line("lsp status && resolve 'looks right' | lsp hover", &names));
    CHECK(!toolcmd_is_line("lsp status; rm -rf x", &names));
    CHECK(!toolcmd_is_line("lsp status > out.txt", &names));
    CHECK(!toolcmd_is_line("lsp $(whoami)", &names));
    CHECK(!toolcmd_is_line("", &names));
    char *inv = toolcmd_invoked("cd x && FOO=1 lsp status", &names);
    CHECK(inv && !strcmp(inv, "lsp"));
    free(inv);
    inv = toolcmd_invoked("echo lsp", &names);
    CHECK(!inv);
    free(inv);
    CHECK(toolcmd_starts_with("resolve ok; echo", &names) && !toolcmd_starts_with("echo; resolve ok", &names));
    json_t *args = json_parse_str("{\"command\":\"  report_issue 'read: x'\"}");
    char *cn = toolcmd_call_name("bash", args);
    CHECK(cn && !strcmp(cn, "report_issue"));
    free(cn);
    CHECK(!toolcmd_call_name("eval", args));
    json_free(args);
    strvec_free(&names);

    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("test_toolcmd ok\n");
    return fails ? 1 : 0;
}
