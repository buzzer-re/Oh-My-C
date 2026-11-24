/* memory + auto-learn units: job store semantics (storage.ts), redaction, lesson file, prompt block,
 * rollout payload truncation, consolidation parsing/apply, managed skills */
#include "coding/autolearn.h"
#include "coding/memory.h"
#include "coding/memstore.h"
#include "core/fs.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static bool has(const char *s, const char *needle) { return s && strstr(s, needle); }

static void test_redact(void) {
    char *r = memory_redact_secrets("aws AKIAABCDEFGHIJKLMNOP done");
    CHECK(!strcmp(r, "aws [REDACTED] done"));
    free(r);
    r = memory_redact_secrets("tok ghp_abcdefghijklmnopqrstuvwxyz0123 and xoxb-1234567890-abc");
    CHECK(!strcmp(r, "tok [REDACTED] and [REDACTED]"));
    free(r);
    r = memory_redact_secrets("use secret_aB3dEfGh1JkLmN here");
    CHECK(!strcmp(r, "use [REDACTED] here"));
    free(r);
    r = memory_redact_secrets("password-supersecretvalue x");
    CHECK(!strcmp(r, "password-[REDACTED] x") || !strcmp(r, "[REDACTED] x"));
    free(r);
    r = memory_redact_secrets("authentication configuration token_short");
    CHECK(!strcmp(r, "authentication configuration token_short"));
    free(r);
    r = memory_redact_secrets("jwt eyJhbGciOiJIUzI1NiJ9xx.eyJzdWIiOiIxMjM0NTY3ODkw.SflKxwRJSMeKKF2QT4fwpM end");
    CHECK(!strcmp(r, "jwt [REDACTED] end"));
    free(r);
}

static void test_neutralize_truncate(void) {
    char *n = memory_neutralize("  a<b>`c`\t\x01 ~~~ d\xe2\x80\x8b" "e  ");
    CHECK(!strcmp(n, "abc ~ d e"));
    free(n);
    char *t = memory_truncate_tokens("abcdefghij", 2); /* 8 chars: head 4 + tail 4 */
    CHECK(!strcmp(t, "abcd\n\n...[truncated]...\n\nghij"));
    free(t);
    t = memory_truncate_tokens("short", 10);
    CHECK(!strcmp(t, "short"));
    free(t);
    t = memory_truncate_tokens("x", 0);
    CHECK(!strcmp(t, ""));
    free(t);
    char *f = memory_rollout_filename("t1", "Fix: Auth Flow!!");
    CHECK(!strcmp(f, "t1-fix_auth_flow"));
    free(f);
    f = memory_rollout_filename("t1", "!!!");
    CHECK(!strcmp(f, "t1"));
    free(f);
    json_t *j = memory_parse_json_object("noise {\"a\":1} tail");
    CHECK(j && json_get_int(j, "a", 0) == 1);
    json_free(j);
    CHECK(!memory_parse_json_object("[1,2]"));
}

static void test_store(const char *dir) {
    char *p = path_join(dir, "memory-state.json");
    mem_thread_t th[] = {{"a", 1000, "/r/a.jsonl", "/proj", "cli"},
                         {"b", 2000, "/r/b.jsonl", "/proj", "cli"},
                         {"c", 3000, "/r/c.jsonl", "/other", "exec"}};
    CHECK(memstore_upsert_threads(p, th, 3) == 0);
    strvec_t ex = {0};
    strvec_push(&ex, "a");
    mem_claim_params_t cp = {.now_sec = 10000, .thread_scan_limit = 300, .max_rollouts_per_startup = 64,
                             .max_rollout_age_days = 30, .min_rollout_idle_hours = 0, .lease_seconds = 120,
                             .running_concurrency_cap = 8, .worker_id = "w", .exclude_thread_ids = &ex};
    mem_claims_t cl;
    CHECK(memstore_claim_stage1(p, &cp, &cl) == 0);
    CHECK(cl.n == 1 && !strcmp(cl.v[0].thread_id, "b")); /* a excluded, c has a non-cli source kind */
    /* a running lease blocks a second claim */
    mem_claims_t cl2;
    memstore_claim_stage1(p, &cp, &cl2);
    CHECK(cl2.n == 0);
    mem_claims_free(&cl2);
    CHECK(!memstore_stage1_succeeded(p, "b", "wrong-token", 2000, "raw", "sum", NULL, 10001, "/proj"));
    CHECK(memstore_stage1_succeeded(p, "b", cl.v[0].ownership_token, 2000, "raw b", "sum b", "slug", 10001, "/proj"));
    json_t *g = memstore_job(p, MEMSTORE_GLOBAL_KIND, "global:/proj");
    CHECK(g && json_get_int(g, "input_watermark", 0) == 2000 && json_get_int(g, "last_success_watermark", -1) == 0);
    json_free(g);
    /* already processed at this watermark: no re-claim */
    memstore_claim_stage1(p, &cp, &cl2);
    CHECK(cl2.n == 0);
    mem_claims_free(&cl2);
    mem_claims_free(&cl);
    char *tok = NULL;
    int64_t iw = 0;
    CHECK(memstore_claim_global(p, "w", 180, 10002, "/proj", &tok, &iw) == MEMSTORE_CLAIMED && iw == 2000);
    char *tok2 = NULL;
    CHECK(memstore_claim_global(p, "w", 180, 10003, "/proj", &tok2, &iw) == MEMSTORE_SKIPPED_RUNNING);
    CHECK(memstore_heartbeat_global(p, tok, 180, 10004, "/proj"));
    mem_outputs_t outs;
    memstore_list_outputs(p, 200, "/proj", &outs);
    CHECK(outs.n == 1 && !strcmp(outs.v[0].raw_memory, "raw b") && !strcmp(outs.v[0].rollout_slug, "slug"));
    mem_outputs_free(&outs);
    memstore_list_outputs(p, 200, "/other", &outs);
    CHECK(outs.n == 0);
    mem_outputs_free(&outs);
    CHECK(memstore_global_succeeded(p, tok, 2000, 10005, "/proj"));
    CHECK(memstore_claim_global(p, "w", 180, 10006, "/proj", &tok2, &iw) == MEMSTORE_SKIPPED_NOT_DIRTY);
    /* forced enqueue makes it dirty again (lsw + 1) */
    CHECK(memstore_enqueue_global(p, 10, "/proj", true) == 0);
    g = memstore_job(p, MEMSTORE_GLOBAL_KIND, "global:/proj");
    CHECK(json_get_int(g, "input_watermark", 0) == 2001);
    json_free(g);
    free(tok);
    CHECK(memstore_claim_global(p, "w", 180, 10007, "/proj", &tok, &iw) == MEMSTORE_CLAIMED && iw == 2001);
    CHECK(memstore_global_failed(p, tok, 180, "boom", 10008, "/proj"));
    g = memstore_job(p, MEMSTORE_GLOBAL_KIND, "global:/proj");
    CHECK(str_eq(json_get_str(g, "status"), "error") && json_get_int(g, "retry_remaining", 0) == 2 &&
          json_get_int(g, "retry_at", 0) == 10188 && str_eq(json_get_str(g, "last_error"), "boom"));
    json_free(g);
    free(tok);
    /* retry delay respected */
    CHECK(memstore_claim_global(p, "w", 180, 10100, "/proj", &tok, &iw) == MEMSTORE_SKIPPED_NOT_DIRTY);
    CHECK(memstore_claim_global(p, "w", 180, 10200, "/proj", &tok, &iw) == MEMSTORE_CLAIMED);
    free(tok);
    /* stage-1 failure decrements retries and sets retry_at */
    mem_thread_t nt = {"d", 5000, "/r/d.jsonl", "/proj", "cli"};
    memstore_upsert_threads(p, &nt, 1);
    cp.now_sec = 20000;
    memstore_claim_stage1(p, &cp, &cl);
    CHECK(cl.n == 1 && !strcmp(cl.v[0].thread_id, "d"));
    CHECK(memstore_stage1_failed(p, "d", cl.v[0].ownership_token, 120, "bad", 20001));
    mem_claims_free(&cl);
    cp.now_sec = 20050;
    memstore_claim_stage1(p, &cp, &cl);
    CHECK(cl.n == 0);
    mem_claims_free(&cl);
    cp.now_sec = 20200;
    memstore_claim_stage1(p, &cp, &cl);
    CHECK(cl.n == 1);
    CHECK(memstore_stage1_no_output(p, "d", cl.v[0].ownership_token, 5000, 20201, "/proj"));
    mem_claims_free(&cl);
    /* clear is scoped to one project */
    CHECK(memstore_clear_cwd(p, "/proj") == 0);
    CHECK(!memstore_job(p, MEMSTORE_GLOBAL_KIND, "global:/proj"));
    CHECK(!memstore_job(p, MEMSTORE_STAGE1_KIND, "b"));
    char *raw = fs_read(p, NULL);
    CHECK(has(raw, "\"c\"") && !has(raw, "\"b\""));
    free(raw);
    strvec_free(&ex);
    free(p);
}

static void test_lessons(const char *agent) {
    char *msg = NULL;
    CHECK(memory_save_lesson(agent, "/proj", "  \x01  ", NULL, &msg) == 0);
    free(msg);
    CHECK(memory_save_lesson(agent, "/proj", "first <lesson>", "ctx", &msg) == 1);
    free(msg);
    CHECK(memory_save_lesson(agent, "/proj", "second", NULL, &msg) == 1);
    free(msg);
    CHECK(memory_save_lesson(agent, "/proj", "first lesson", "ctx", &msg) == 1); /* dedupe moves it to the top */
    free(msg);
    char *root = memory_root(agent, "/proj");
    CHECK(str_ends(root, "/memories/--proj--"));
    char *lp = path_join(root, "learned.md");
    char *t = fs_read(lp, NULL);
    CHECK(t && !strcmp(t, "- first lesson _(context: ctx)_\n- second\n"));
    free(t);
    /* hand-written heading survives; cap at 100 lessons */
    fs_write(lp, "# Notes\n- old\n", 14);
    for (int i = 0; i < 105; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "lesson %d", i);
        memory_save_lesson(agent, "/proj", buf, NULL, &msg);
        free(msg);
    }
    t = fs_read(lp, NULL);
    CHECK(t && str_starts(t, "# Notes\n- lesson 104\n") && !has(t, "- old") && str_count(t, "\n- ") == 100);
    free(t);
    /* prompt block: summary and lessons share one budget */
    char *blk = memory_render_instructions("Sum", "- l1", 5000);
    CHECK(has(blk, "# Memory Guidance") && has(blk, "Memory summary:\nSum") && has(blk, "- l1"));
    free(blk);
    CHECK(!memory_render_instructions("", "", 5000));
    /* learned-only: no summary read path, the rules stay well-formed bullets (omp ecbd5d8b2e, aaa53f05d3) */
    blk = memory_render_instructions("", "- l2", 5000);
    CHECK(blk && !has(blk, "memory_summary.md") && has(blk, "Rules:\n- Memory: heuristics") && has(blk, "- l2"));
    free(blk);
    blk = memory_render_instructions("0123456789abcdef", "- lesson", 4);
    CHECK(blk && !has(blk, "Learned lessons"));
    free(blk);
    free(lp);
    free(root);
}

static void test_rollout_items(const char *dir) {
    char *p = path_join(dir, "rollout.jsonl");
    sb_t b = SB_INIT;
    sb_append(&b, "{\"type\":\"session\",\"id\":\"x\",\"cwd\":\"/p\"}\n");
    sb_append(&b, "{\"type\":\"message\",\"message\":{\"role\":\"user\",\"content\":\"hello\"}}\n");
    sb_append(&b, "{\"type\":\"message\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"t\"},{\"type\":\"text\",\"text\":\"hi\"}],\"providerPayload\":{\"x\":1}}}\n");
    sb_append(&b, "{\"type\":\"message\",\"message\":{\"role\":\"toolResult\",\"toolName\":\"write\",\"content\":[{\"type\":\"text\",\"text\":\"w\"}]}}\n");
    sb_append(&b, "{\"type\":\"message\",\"message\":{\"role\":\"toolResult\",\"toolName\":\"bash\",\"content\":[{\"type\":\"text\",\"text\":\"out\"}]}}\n");
    fs_write(p, b.s, b.len);
    char *items = memory_build_rollout_items(p, 1000);
    CHECK(items && !strcmp(items, "[{\"role\":\"user\",\"content\":\"hello\"},{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]},"
                                  "{\"role\":\"toolResult\",\"toolName\":\"bash\",\"content\":[{\"type\":\"text\",\"text\":\"out\"}]}]"));
    free(items);
    for (int i = 0; i < 200; i++)
        sb_appendf(&b, "{\"type\":\"message\",\"message\":{\"role\":\"user\",\"content\":\"msg %03d padding padding\"}}\n", i);
    fs_write(p, b.s, b.len);
    items = memory_build_rollout_items(p, 100); /* 400 chars: 240 head + 160 tail */
    CHECK(items && str_starts(items, "[{\"role\":\"user\",\"content\":\"hello\"}") && has(items, "\n\n...[truncated]...\n\n") &&
          str_ends(items, "\"msg 199 padding padding\"}]"));
    const char *tail = strstr(items, "...[truncated]...\n\n");
    CHECK(tail && strlen(tail + 19) == 160);
    free(items);
    CHECK(!memory_build_rollout_items("/nonexistent/x.jsonl", 10));
    sb_free(&b);
    free(p);
}

static void test_consolidation(const char *dir) {
    char *root = path_join(dir, "croot");
    char *err = NULL;
    json_t *bad = json_parse_str("{\"memory_md\":\"m\",\"memory_summary\":\"s\"}");
    CHECK(memory_apply_consolidation(root, bad, &err) != 0 && has(err, "schema validation"));
    free(err);
    json_free(bad);
    json_t *ok = json_parse_str("{\"memory_md\":\"# M\",\"memory_summary\":\"S\",\"skills\":[{\"name\":\"My Skill!\",\"content\":\"body\","
                                "\"scripts\":[{\"path\":\"a/run.sh\",\"content\":\"echo\"},{\"path\":\"../x\",\"content\":\"no\"},{\"path\":\"/abs\",\"content\":\"no\"}]}]}");
    CHECK(memory_apply_consolidation(root, ok, &err) == 0);
    char *p = path_join(root, "skills/my-skill/scripts/a/run.sh");
    char *t = fs_read(p, NULL);
    CHECK(t && !strcmp(t, "echo\n"));
    free(t);
    free(p);
    p = path_join(root, "memory_summary.md");
    t = fs_read(p, NULL);
    CHECK(t && !strcmp(t, "S\n"));
    free(t);
    free(p);
    json_free(ok);
    /* a later pass prunes skills and files no longer listed */
    ok = json_parse_str("{\"memory_md\":\"# M\",\"memory_summary\":\"S2\",\"skills\":[]}");
    CHECK(memory_apply_consolidation(root, ok, &err) == 0);
    p = path_join(root, "skills/my-skill");
    CHECK(!fs_exists(p));
    free(p);
    json_free(ok);
    free(root);
}

static void test_managed_skills(const char *agent) {
    char *err = NULL, *path = NULL;
    {
        /* capture turn: the primary's tools in order, refused except the capture tools */
        agent_t *ag = agent_new();
        agent_tool_t *read = agent_tool_new("read", "Read", "reads", NULL, NULL);
        read->summary = xstrdup("s");
        agent_tool_t *learn = agent_tool_new("learn", "Learn", "primary learn", NULL, NULL);
        agent_tool_t *primary[] = {read, learn};
        agent_tool_t *cap_learn = agent_tool_new("learn", "Learn", "capture learn", NULL, NULL);
        agent_tool_t *cap_skill = agent_tool_new("manage_skill", "Skill", "capture skill", NULL, NULL);
        agent_tool_t *capture[] = {cap_learn, cap_skill};
        autolearn_capture_tool_list(ag, primary, 2, capture, 2);
        CHECK(ag->tools.n == 3 && str_eq(ag->tools.v[0]->name, "read") && str_eq(ag->tools.v[1]->name, "learn") &&
              str_eq(ag->tools.v[2]->name, "manage_skill"));
        CHECK(ag->tools.v[1] == cap_learn && str_eq(ag->tools.v[0]->description, "reads") && str_eq(ag->tools.v[0]->summary, "s"));
        tool_result_t out = {0};
        CHECK(ag->tools.v[0]->execute(ag->tools.v[0], NULL, NULL, &out) == 0 && out.is_error);
        char *text = tool_result_joined_text(&out);
        CHECK(str_eq(text, "This tool is not available during an auto-learn capture turn, and nothing was run. Use `learn` or `manage_skill` to capture something reusable, or stop."));
        free(text);
        tool_result_free(&out);
        agent_tool_free(read);
        agent_tool_free(learn);
        agent_free(ag);
    }
    CHECK(!autolearn_sanitize_skill_name("../evil", &err) && has(err, "Invalid skill name"));
    free(err);
    char *n = autolearn_sanitize_skill_name("  My-Skill ", NULL);
    CHECK(n && !strcmp(n, "my-skill"));
    free(n);
    CHECK(autolearn_write_skill("update", "nope", "d", "b", NULL, &err) != 0 && has(err, "does not exist"));
    free(err);
    CHECK(autolearn_write_skill("create", "demo", "When <x> happens", "Body", &path, &err) == 0);
    char *t = fs_read(path, NULL);
    CHECK(t && !strcmp(t, "---\nname: demo\ndescription: When x happens\n---\n\nBody\n"));
    free(t);
    CHECK(autolearn_write_skill("create", "demo", "d", "b", NULL, &err) != 0 && has(err, "already exists"));
    free(err);
    CHECK(autolearn_write_skill("update", "demo", "New", "Body2", NULL, &err) == 0);
    CHECK(autolearn_write_skill("create", "empty", "\x01", "b", NULL, &err) != 0 && has(err, "non-empty description"));
    free(err);
    /* refuse a hard-linked SKILL.md */
    char *other = path_join(agent, "elsewhere.md");
    CHECK(link(path, other) == 0);
    CHECK(autolearn_write_skill("update", "demo", "x", "y", NULL, &err) != 0 && has(err, "hard links"));
    free(err);
    unlink(other);
    free(other);
    /* refuse a symlinked skill dir */
    char *root = autolearn_managed_dir();
    char *ld = path_join(root, "linked");
    int sl = symlink(agent, ld);
#ifdef _WIN32
    if (sl != 0 && errno == EPERM) fprintf(stderr, "  symlink check skipped (symlinks need Developer Mode or admin on Windows)\n");
    else
#endif
    {
        CHECK(sl == 0);
        CHECK(autolearn_delete_skill("linked", &err) != 0 && has(err, "symlink"));
        free(err);
    }
    CHECK(autolearn_delete_skill("demo", &err) == 0);
    CHECK(autolearn_delete_skill("demo", &err) != 0 && has(err, "does not exist"));
    free(err);
    free(ld);
    free(root);
    free(path);
}

int main(void) {
    char *dir = fs_mktempdir("omc-memtest");
    setenv("OMC_CODING_AGENT_DIR", dir, 1);
    test_redact();
    test_neutralize_truncate();
    test_store(dir);
    test_lessons(dir);
    test_rollout_items(dir);
    test_consolidation(dir);
    test_managed_skills(dir);
    fs_rm_rf(dir);
    free(dir);
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
