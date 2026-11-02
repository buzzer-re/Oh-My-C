/* test_dump.c - /dump family: dump text format, concise history, archive report, anonymizer, zip.
 *
 * Fixtures under tests/fixtures/dump/ come from omp 18.14.2 run against the scripted mock
 * (tests/tui/compare.sh): session.jsonl is omp's session file, anon.jsonl its `/dump anon` member,
 * transcript.md the transcript part of its `/dump all` session.md.
 *
 * Tool mode (manual comparison against omp): test_dump anon|transcript|history <session.jsonl> */
#include "session/anon.h"
#include "session/dump.h"
#include "session/session.h"

#include "core/fs.h"

#include <stdio.h>
#include <unistd.h>

static int failures;
#define CHECK(c_)                                                                                    \
    do {                                                                                             \
        if (!(c_)) {                                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c_);                   \
            failures++;                                                                              \
        }                                                                                            \
    } while (0)
#define CHECK_STR(a_, b_)                                                                            \
    do {                                                                                             \
        const char *x_ = (a_), *y_ = (b_);                                                           \
        if (!x_ || !y_ || strcmp(x_, y_)) {                                                          \
            fprintf(stderr, "%s:%d: got\n---\n%s\n---\nwant\n---\n%s\n---\n", __FILE__, __LINE__,      \
                    x_ ? x_ : "(null)", y_ ? y_ : "(null)");                                         \
            failures++;                                                                              \
        }                                                                                            \
    } while (0)

static char *anon_jsonl(const session_t *s) {
    anon_t *a = anon_new();
    sb_t b = SB_INIT;
    json_t *hdr = dump_session_header(s);
    char *h = anon_entry_line(a, hdr);
    json_free(hdr);
    sb_append(&b, h);
    free(h);
    json_t *e;
    json_arr_foreach(s->entries, i, e) {
        char *l = anon_entry_line(a, e);
        sb_putc(&b, '\n');
        sb_append(&b, l);
        free(l);
    }
    sb_putc(&b, '\n');
    anon_free(a);
    return sb_detach(&b);
}

static json_t *branch_messages(const session_t *s) {
    json_t *path = session_path_to(s, s->leaf_id);
    json_t *m = dump_context_messages(path);
    json_free(path);
    return m;
}

/* the transcript part of formatSessionDumpText (no system prompt, no tools) from the "## User" on */
static char *transcript(const session_t *s) {
    json_t *msgs = branch_messages(s);
    char *t = dump_format_text(msgs, NULL, NULL, NULL, NULL, 0);
    json_free(msgs);
    const char *u = strstr(t, "## User");
    char *r = xstrdup(u ? u : "");
    free(t);
    return r;
}

static char *fixture(const char *name) {
    char *p = xasprintf("tests/fixtures/dump/%s", name);
    size_t n;
    char *d = fs_read(p, &n);
    if (!d) fprintf(stderr, "missing fixture %s\n", p);
    free(p);
    return d;
}

static void test_fixture_session(void) {
    char *err = NULL;
    session_t *s = session_open("tests/fixtures/dump/session.jsonl", &err);
    CHECK(s != NULL);
    if (!s) return;
    char *anon = anon_jsonl(s), *want = fixture("anon.jsonl");
    CHECK_STR(anon, want);
    free(anon);
    free(want);
    char *md = transcript(s), *want_md = fixture("transcript.md");
    CHECK_STR(md, want_md);
    free(md);
    free(want_md);
    session_free(s);
}

static json_t *J(const char *s) {
    json_t *j = json_parse_str(s);
    if (!j) fprintf(stderr, "bad json: %s\n", s);
    return j;
}

static void test_format(void) {
    json_t *msgs = J("["
                     "{\"role\":\"user\",\"content\":\"hi\",\"timestamp\":1793582128080},"
                     "{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"<thinking>deep</thinking>\"},"
                     "{\"type\":\"thinking\",\"thinking\":\" ... \"},{\"type\":\"text\",\"text\":\"ok\"},"
                     "{\"type\":\"toolCall\",\"id\":\"c1\",\"name\":\"bash\",\"arguments\":{\"i\":\"Listing files\\nnow\",\"command\":\"ls\",\"timeout\":5}}],"
                     "\"timestamp\":1793582128179,\"duration\":61234,\"ttft\":800},"
                     "{\"role\":\"toolResult\",\"toolCallId\":\"c1\",\"toolName\":\"bash\",\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"boom\\nmore\"},{\"type\":\"image\",\"data\":\"x\",\"mimeType\":\"image/png\"}]},"
                     "{\"role\":\"custom\",\"customType\":\"lsp-notice\",\"content\":\"<system-notice>a ```` b</system-notice>\",\"display\":false},"
                     "{\"role\":\"custom\",\"customType\":\"plain\",\"content\":[{\"type\":\"text\",\"text\":\"x\"},{\"type\":\"image\"}]},"
                     "{\"role\":\"compactionSummary\",\"summary\":\"S\",\"tokensBefore\":1234},"
                     "{\"role\":\"branchSummary\",\"summary\":\"B\",\"fromId\":\"abc\"},"
                     "{\"role\":\"bashExecution\",\"command\":\"echo x\",\"output\":\"x\",\"exitCode\":2,\"cancelled\":false,\"excludeFromContext\":false},"
                     "{\"role\":\"bashExecution\",\"command\":\"secret\",\"output\":\"\",\"excludeFromContext\":true}"
                     "]");
    json_t *sp = J("[\"one\",\"\",\"two\"]");
    char *t = dump_format_text(msgs, sp, NULL, NULL, NULL, 0);
    const char *want =
        "## System Prompt\n\n### System Prompt 1\n\none\n\n\n### System Prompt 2\n\ntwo\n\n\n## Configuration\n\nModel: (not selected)\n"
        "Thinking Level: \n\n\n## User \xc2\xb7 2026-11-02T01:15:28.080Z\n\nhi\n\n\n"
        "## Assistant \xc2\xb7 2026-11-02T01:15:28.179Z (took 1m1s, ttft 800ms)\n\n<thinking>\ndeep\n</thinking>\n\nok\n### Tool Call: bash\n"
        "// Listing files\n// now\n```yaml\ncommand: ls\ntimeout: 5\n```\n\n\n"
        "### Tool Result: bash\n(error)\n```\nboom\nmore\n```\n[Image output]\n\n"
        "## System Notice: LSP\n\n`````xml\n<system-notice>a ```` b</system-notice>\n`````\n\n\n"
        "## plain\n\nx\n[Image]\n\n\n## Compaction Summary\n\n(1234 tokens before compaction)\n\nS\n\n\n"
        "## Branch Summary\n\n(from branch: abc)\n\nB\n\n\n## Bash Execution\n\nRan `echo x`\n```\nx\n```\n\nCommand exited with code 2";
    CHECK_STR(t, want);
    free(t);

    char *h = dump_history_markdown(msgs);
    const char *want_h = "## user\n\nhi\n\n## assistant\n\nok\n\xe2\x86\x92 bash(ls) \xe2\x87\x92 error \xc2\xb7 3 lines \xe2\x80\x94 boom\n\n"
                         "[plain] x [image]\n\n[compaction] S\n\n[branch] from abc: B\n\n"
                         "\xe2\x86\x92 user-bash! echo x \xe2\x87\x92 error \xc2\xb7 exit 2 \xc2\xb7 1 line\n";
    CHECK_STR(h, want_h);
    free(h);
    json_free(sp);
    json_free(msgs);

    char *d;
    CHECK_STR(d = dump_format_duration(0), "0ms");
    free(d);
    CHECK_STR(d = dump_format_duration(59960), "1m");
    free(d);
    CHECK_STR(d = dump_format_duration(3600000 * 25.0), "1d1h");
    free(d);
    CHECK_STR(d = dump_format_duration(1500), "1.5s");
    free(d);
}

/* renderToolInventory with JSDoc examples (shape and text of omp's `ask` inventory entry) */
static void test_inventory(void) {
    json_t *params = J("{\"type\":\"object\",\"properties\":{\"questions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
                       "\"required\":[\"questions\"]}");
    agent_tool_t *t = agent_tool_new("ask", NULL, "Ask the user.\nTrailing  ", params, NULL);
    t->examples = J("[{\"caption\":\"Choice\",\"call\":{\"questions\":[{\"id\":\"storage\",\"question\":\"Database?\","
                    "\"options\":[{\"label\":\"SQLite\"},{\"label\":\"Postgres\"}],\"recommended\":0}]}}]");
    agent_tool_t *e = agent_tool_new("noop", NULL, "", J("{\"type\":\"object\",\"properties\":{}}"), NULL);
    agent_tool_t *tools[] = {t, e};
    char *inv = dump_tool_inventory(tools, 2);
    CHECK_STR(inv, "## functions\n\nnamespace functions {\n\n// Ask the user.\n// Trailing\n//\n// @example \"Choice\"\n"
                   "// ask(questions=[{\"id\": \"storage\", \"question\": \"Database?\", \"options\": [{\"label\": \"SQLite\"}, "
                   "{\"label\": \"Postgres\"}], \"recommended\": 0}])\ntype ask = (_: {\nquestions: string[],\n});\n\n"
                   "type noop = ();\n\n} // namespace functions");
    free(inv);
    agent_tool_free(t);
    agent_tool_free(e);
}

static void test_report(void) {
    dump_archive_t ar = {0};
    ar.path = xstrdup("/tmp/omp-dump-1.zip");
    strvec_push(&ar.files, "session.md");
    strvec_push(&ar.files, "llm-request.json");
    char *r = dump_archive_report(&ar);
    CHECK_STR(r, "Session dump archive: /tmp/omp-dump-1.zip\nContains 2 files (0 subagent transcripts):\n  session.md\n  llm-request.json\n"
                 "This archive persists on disk and may contain raw context/secrets \xe2\x80\x94 treat accordingly.");
    free(r);
    ar.anonymized = true;
    ar.subagent_count = 1;
    ar.subagent_error = xstrdup("EACCES");
    strvec_push(&ar.malformed, "session.jsonl\t1");
    strvec_push(&ar.unreadable, "subagents/seg3.jsonl");
    r = dump_archive_report(&ar);
    CHECK(strstr(r, "(1 subagent transcript):") != NULL);
    CHECK(strstr(r, "\nSubagent transcripts unavailable: EACCES\nSkipped 1 malformed record in session.jsonl\n"
                    "Not exported (no readable session header): subagents/seg3.jsonl\nTurn contents") != NULL);
    free(r);
    dump_archive_free(&ar);
}

static void test_anon_units(void) {
    anon_t *a = anon_new();
    char *s;
    CHECK_STR(s = anon_command(a, "git log -3 --oneline > out.txt && cd src/foo && FOO=bar ./run.sh --secret=x \"$HOME\""),
              "git log -3 --oneline > out.txt && cd src/seg1 && PLACEHOLDER_2=PLACEHOLDER_3 ./seg4.sh PLACEHOLDER_5 \"$HOME\"");
    free(s);
    CHECK_STR(s = anon_path(a, "https://example.com/a/b.ts:10-20?q=1"), "https://seg7/seg8/seg9.ts:10-20?PLACEHOLDER_6");
    free(s);
    CHECK_STR(s = anon_segment(a, "*.test.ts"), "*.test.ts");
    free(s);
    CHECK_STR(s = anon_redact(a, "two\nlines"), "[redacted #10: 9 chars, 2 lines]");
    free(s);
    json_t *rec = J("{\"type\":\"message\",\"id\":\"ab12cd34\",\"zzz\":1,\"message\":{\"role\":\"toolResult\",\"toolName\":\"mcp__x_y\","
                    "\"isError\":false,\"content\":[{\"type\":\"text\",\"text\":\"hi\",\"extra\":true}],\"details\":{\"a\":1}}}");
    char *line = anon_entry_line(a, rec);
    CHECK_STR(line, "{\"type\":\"message\",\"id\":\"ab12cd34\",\"PLACEHOLDER_11\":\"[redacted #12: 1 chars, 1 line]\",\"message\":{\"role\":"
                    "\"toolResult\",\"toolName\":\"PLACEHOLDER_13\",\"isError\":false,\"content\":[{\"type\":\"text\",\"text\":\"[redacted #14: 2 chars, 1 line]\","
                    "\"PLACEHOLDER_15\":\"[redacted #16: 4 chars, 1 line]\"}],\"details\":\"[redacted #17: 7 chars, 1 line]\"}}");
    free(line);
    json_free(rec);
    anon_free(a);
}

static void test_zip(void) {
    const char *dir = getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char *path = xasprintf("%s/omc-test-dump-%d.zip", dir, (int)getpid());
    const char *names[] = {"a.txt", "sub/b.md", "empty"};
    char big[4000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)('a' + i % 3);
    const char *data[] = {"hello", big, ""};
    size_t lens[] = {5, sizeof big, 0};
    char *err = NULL;
    CHECK(dump_zip_write(path, names, data, lens, 3, &err) == 0);
    size_t n;
    char *z = fs_read(path, &n);
    CHECK(z && n > 22 && !memcmp(z, "PK\x03\x04", 4));
    /* end of central directory: 3 entries */
    CHECK(z && (unsigned char)z[n - 22] == 0x50 && (unsigned char)z[n - 12] == 3);
    /* the first member is stored (deflate would not shrink 5 bytes), the second deflated */
    CHECK(z && z[8] == 0 && z[30 + 5 + 5 + 8] == 8);
    free(z);
    char *cmd = xasprintf("python3 -c \"import zipfile,sys;z=zipfile.ZipFile(sys.argv[1]);assert z.testzip() is None;"
                          "assert z.read('sub/b.md')==bytes(ord('a')+i%%3 for i in range(4000));assert z.namelist()==['a.txt','sub/b.md','empty']\" %s",
                          path);
    CHECK(system(cmd) == 0);
    free(cmd);
    unlink(path);
    free(path);
    free(err);
}

/* tool mode: the tool inventory for the tools of an omp llm-request.json (no examples there) */
static int inventory_from_request(const char *path) {
    size_t n;
    char *d = fs_read(path, &n);
    json_t *req = d ? json_parse(d, n, NULL) : NULL;
    free(d);
    if (!req) return 2;
    VEC(agent_tool_t *) tools = {0};
    json_t *t;
    json_arr_foreach(json_get(req, "tools"), i, t) {
        agent_tool_t *at = agent_tool_new(json_get_str(t, "name"), NULL, json_get_str(t, "description"),
                                          json_clone(json_get(t, "parameters")), NULL);
        vec_push(&tools, at);
    }
    char *inv = dump_tool_inventory(tools.v, tools.n);
    puts(inv);
    free(inv);
    for (size_t i = 0; i < tools.n; i++) agent_tool_free(tools.v[i]);
    vec_free(&tools);
    json_free(req);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "inventory")) return inventory_from_request(argv[2]);
    if (argc == 3) {
        char *err = NULL;
        session_t *s = session_open(argv[2], &err);
        if (!s) {
            fprintf(stderr, "%s\n", err ? err : "open failed");
            return 2;
        }
        char *out = NULL;
        if (!strcmp(argv[1], "anon")) out = anon_jsonl(s);
        else if (!strcmp(argv[1], "transcript")) out = transcript(s);
        else if (!strcmp(argv[1], "history")) {
            json_t *m = branch_messages(s);
            out = dump_history_markdown(m);
            json_free(m);
        }
        if (out) fputs(out, stdout);
        free(out);
        session_free(s);
        return out ? 0 : 2;
    }
    test_format();
    test_report();
    test_inventory();
    test_anon_units();
    test_zip();
    test_fixture_session();
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
