/* test_commands.c - non-builtin slash commands (omp extensibility/slash-commands.ts, utils/command-args.ts,
 * config/prompt-templates.ts, extensibility/skills.ts): argument parsing and substitution, template
 * rendering, file-command discovery and precedence, prompt templates, /skill: parsing and the
 * user-invocation message. */
#include "coding/commands.h"
#include "config/settings.h"
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static void put(const char *root, const char *rel, const char *text) {
    char *p = path_join(root, rel);
    char *d = path_dirname(p);
    fs_mkdir_p(d, 0755);
    fs_write(p, text, strlen(text));
    free(d);
    free(p);
}

static void test_args(void) {
    strvec_t a = commands_parse_args("Bob \"good day\"  'x y' z");
    CHECK(a.n == 4 && str_eq(a.v[0], "Bob") && str_eq(a.v[1], "good day") && str_eq(a.v[2], "x y") && str_eq(a.v[3], "z"));
    char *s = commands_substitute_args("$1|$2|$9|$@|$ARGUMENTS|$@[2]|$@[2:1]|$@[0]|$x", &a);
    CHECK(str_eq(s, "Bob|good day||Bob good day x y z|Bob good day x y z|good day x y z|good day||$x"));
    free(s);
    strvec_free(&a);
    /* inline placeholders suppress the appended args; otherwise they follow a blank line */
    char *r = commands_render("Greet $1.", "Ann extra");
    CHECK(str_eq(r, "Greet Ann."));
    free(r);
    r = commands_render("Plain body", "a b");
    CHECK(str_eq(r, "Plain body\n\na b"));
    free(r);
    r = commands_render("Body {{arguments}}", "q r");
    CHECK(str_eq(r, "Body q r"));
    free(r);
    r = commands_render("Body", "");
    CHECK(str_eq(r, "Body"));
    free(r);
    char *n, *g;
    CHECK(commands_parse_invocation("/ns:cmd  a b", &n, &g) && str_eq(n, "ns:cmd") && str_eq(g, " a b"));
    free(n);
    free(g);
    CHECK(!commands_parse_invocation("hello", &n, &g));
}

static void test_skill_parse(void) {
    skill_invocation_t inv;
    CHECK(skill_parse_invocation("  /skill:demo fix it  ", &inv));
    CHECK(str_eq(inv.name, "demo") && str_eq(inv.args, "fix it") && str_eq(inv.prompt, "/skill:demo fix it"));
    skill_invocation_free(&inv);
    CHECK(skill_parse_invocation("please /skill:code-review the diff", &inv));
    CHECK(str_eq(inv.name, "code-review") && str_eq(inv.args, "please the diff"));
    skill_invocation_free(&inv);
    CHECK(skill_parse_invocation("use /skill:ns/name", &inv) && str_eq(inv.name, "ns/name") && str_eq(inv.args, "use"));
    skill_invocation_free(&inv);
    CHECK(!skill_parse_invocation("/skill:", &inv));
    CHECK(!skill_parse_invocation("/plan do /skill:x", &inv)); /* another slash command owns the text */
    CHECK(!skill_parse_invocation("!ls /skill:x", &inv));      /* local execution */
    CHECK(!skill_parse_invocation("see a/skill:x", &inv));     /* not delimited */
    CHECK(!skill_parse_invocation("try /skill:a/b/c", &inv));  /* one namespace level only */
    CHECK(skill_tokens_allowed("$HOME /skill:x") && !skill_tokens_allowed("$ echo") && !skill_tokens_allowed("$$ x"));
}

static void test_discovery(const char *root) {
    char *home = path_join(root, "home"), *proj = path_join(root, "home/proj");
    char *agent = path_join(root, "home/.omc/agent");
    fs_mkdir_p(proj, 0755);
    fs_mkdir_p(agent, 0755);
    put(proj, ".omp/commands/hello.md", "---\ndescription: Say hello\nargument-hint: <name>\nside: true\n---\nHi $1\n");
    put(proj, ".omp/commands/plain.md", "A first line that is definitely longer than sixty characters in total, yes\nmore\n");
    put(proj, ".claude/commands/hello.md", "Claude hello (shadowed)\n");
    put(proj, ".claude/commands/deep/nest.md", "Nested\n");
    put(proj, ".opencode/commands/oc.md", "---\nname: renamed\ndescription: OC desc\nargument-hint: <x>\n---\nOC body $ARGUMENTS\n");
    put(proj, ".agents/commands/ag.md", "Agents dir command\n");
    put(agent, "commands/user.md", "User command\n");
    put(agent, "prompts/tpl.md", "---\ndescription: Tpl\n---\nTemplate $1\n");
    put(agent, "prompts/sub/deep.md", "Deep template\n");
    put(proj, ".omp/prompts/ptpl.md", "Project template\n");
    file_cmds_t c;
    commands_load(proj, &c);
    const file_cmd_t *h = commands_find(&c, "hello");
    CHECK(h && str_eq(h->description, "Say hello") && str_eq(h->argument_hint, "<name>") && h->side);
    CHECK(h && str_eq(h->content, "Hi $1") && str_eq(h->source, "via OMP Project"));
    const file_cmd_t *p = commands_find(&c, "plain");
    CHECK(p && str_eq(p->description, "A first line that is definitely longer than sixty characters..."));
    CHECK(commands_find(&c, "nest") && commands_find(&c, "deep:nest"));
    const file_cmd_t *oc = commands_find(&c, "renamed"); /* frontmatter name; description from the body */
    CHECK(oc && str_eq(oc->description, "OC body $ARGUMENTS") && str_eq(oc->argument_hint, "<x>"));
    CHECK(commands_find(&c, "ag") && commands_find(&c, "user"));
    const file_cmd_t *init = commands_find(&c, "init");
    CHECK(init && str_eq(init->description, "Generate AGENTS.md for current codebase") && !init->path);
    CHECK(str_eq(c.v[0].name, "hello") && str_eq(c.v[c.n - 1].name, "init")); /* native first, bundled last */
    char *x = commands_expand(&c, "/hello Bob");
    CHECK(str_eq(x, "Hi Bob"));
    free(x);
    CHECK(!commands_expand(&c, "/nope"));
    commands_free(&c);
    prompt_tpls_t t;
    prompt_templates_load(proj, &t);
    CHECK(t.n == 3 && str_eq(t.v[0].name, "tpl") && str_eq(t.v[0].description, "Tpl (user)"));
    CHECK(str_eq(t.v[1].name, "deep") && str_eq(t.v[1].description, "Deep template (user:sub)"));
    CHECK(str_eq(t.v[2].description, "Project template (project)"));
    x = prompt_templates_expand(&t, "/tpl one two");
    CHECK(str_eq(x, "Template one"));
    free(x);
    prompt_templates_free(&t);
    /* agents.ts walk: ancestors up to / when no repo and outside home */
    strvec_t w = discovery_project_ancestors(proj);
    CHECK(w.n == 1 && str_eq(w.v[0], proj)); /* stops at home (skipped) */
    strvec_free(&w);
    char *outside = path_join(root, "outside/x");
    fs_mkdir_p(outside, 0755);
    w = discovery_project_ancestors(outside);
    CHECK(w.n >= 3 && str_eq(w.v[0], outside) && str_eq(w.v[w.n - 1], "/"));
    strvec_free(&w);
    free(outside);
    free(home);
    free(proj);
    free(agent);
}

static void test_skill_message(const char *root) {
    char *dir = path_join(root, "skills/demo");
    put(dir, "SKILL.md", "---\nname: demo\ndescription: d\n---\n\n# Demo\n\nRun it.\n\n");
    char *sf = path_join(dir, "SKILL.md");
    skill_t s = {"demo", "d", sf, dir, false, false};
    skill_invocation_t inv = {"demo", "fix it", "/skill:demo fix it"};
    char *msg;
    json_t *det;
    CHECK(skill_build_user_prompt(&s, &inv, &msg, &det, NULL) == 0);
    char *want = xasprintf("[IMPORTANT: User invoked the \"demo\" skill; follow its instructions. Full skill below.]\n\n# Demo\n\nRun it.\n\n"
                           "---\n\n[Skill directory: %s]\nResolve relative paths in this skill (e.g. `scripts/foo.js`, "
                           "`templates/config.yaml`) against this absolute directory; read referenced assets and templates; run "
                           "scripts with the terminal tool when skill instructions call for it.\nUser: fix it",
                           dir);
    CHECK(str_eq(msg, want));
    if (!str_eq(msg, want)) fprintf(stderr, "got:\n%s\n", msg);
    CHECK(str_eq(json_get_str(det, "args"), "fix it") && json_get_int(det, "lineCount", 0) == 3 &&
          str_eq(json_get_str(det, "prompt"), "/skill:demo fix it") && str_eq(json_get_str(det, "path"), sf));
    free(want);
    free(msg);
    json_free(det);
    inv.args = "";
    CHECK(skill_build_user_prompt(&s, &inv, &msg, &det, NULL) == 0);
    CHECK(!strstr(msg, "User:") && !json_get(det, "args"));
    free(msg);
    json_free(det);
    free(sf);
    free(dir);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    char *root = xasprintf("%s/omc-test-commands-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    fs_rm_rf(root);
    char *home = path_join(root, "home");
    fs_mkdir_p(home, 0755);
    setenv("HOME", home, 1);
    char *agent = path_join(home, ".omc/agent");
    setenv("OMC_CODING_AGENT_DIR", agent, 1);
    settings_init(root, NULL);
    test_args();
    test_skill_parse();
    test_discovery(root);
    test_skill_message(root);
    fs_rm_rf(root);
    free(agent);
    free(home);
    free(root);
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
