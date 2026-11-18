/* fuzz_approval - tool approval on model-chosen arguments (src/tools/approval.c): shell segment splitting and `cd`
 * handling, critical-command patterns, the per-tool decision (bash, edit modes, write, read/search targets, URLs),
 * argument formatting for the prompt. Input: <tool selector byte><JSON args or a shell command>. */
#include "core/core.h"
#include "core/json.h"
#include "tools/tools.h"
#include "tools/approval.h"
#include "fuzz.h"

static const char *const TOOLS[] = {"bash", "edit", "write", "read", "grep", "find", "ast_grep", "ast_edit", "fetch",
                                    "browser", "eval", "task", "ssh", "notebook", "todo_write", "ask"};
static const char *const MODES[] = {"hashline", "replace", "patch", "apply_patch", "sloppy"};
static tool_env_t g_env;

FUZZ_SETUP { g_env.cwd = xstrdup("/tmp/omc-fuzz-work"); }

FUZZ_ENTRY {
    if (size > 1 << 14) return 0;
    unsigned sel = fuzz_byte(&data, &size);
    char *s = fuzz_cstr(data, size);
    strvec_t segs = {0};
    approval_shell_segments(s, &segs);
    strvec_free(&segs);
    (void)approval_bash_critical(s);
    char *t = approval_truncate(s);
    free(t);
    json_t *args = json_parse(s, size, NULL);
    if (!json_is_obj(args)) {
        json_free(args);
        args = json_obj();
        json_set_str(args, "command", s);
        json_set_str(args, "input", s);
        json_set_str(args, "path", s);
    }
    g_env.edit_mode = (char *)MODES[(sel >> 4) % 5];
    approval_call_t c = {.env = &g_env, .agent = NULL, .tool = TOOLS[sel % (sizeof TOOLS / sizeof *TOOLS)], .args = args};
    approval_decision_t d = {0};
    approval_tool_decision(&c, &d);
    approval_decision_free(&d);
    strvec_t rows = {0};
    approval_tool_details(&c, &rows);
    strvec_free(&rows);
    approval_cache_targets(&c, &rows);
    strvec_free(&rows);
    approval_format_arguments(args, sel & 1, &rows);
    strvec_free(&rows);
    json_t *restricted = json_parse_str("{\"bash\":[\"rm *\",\"git push*\"],\"read\":[\"/etc/**\"]}");
    char *r = approval_restricted_reason(&c, restricted, g_env.cwd);
    free(r);
    json_free(restricted);
    json_free(args);
    free(s);
    return 0;
}
