/* toolcmd.c - session tools as bash commands (port of omp tools/tool-commands.ts, exec/tool-command-host.ts,
 * tools/resolve.ts, tools/report-tool-issue.ts, session/session-tools.ts' MCP command guidance). See toolcmd.h. */
#include "toolcmd.h"

#include "approval.h"
#include "config/settings.h"
#include "core/fs.h"
#include "img/img.h"
#include "prompt/template.h"
#include "tools.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DOCS_TOTAL_BUDGET 48000
#define DOCS_PER_TOOL_CAP 10000
#define EXTERNAL_DESCRIPTION_CAP 200
#define MCP_GUIDANCE_DATA_BUDGET 4000
#define MCP_GUIDANCE_MAX_MAPPINGS 64

/* ---------- names ---------- */

/* bash's reserved words and builtins: never reach a session command in command position */
static const char *const RESERVED[] = {
    "!", ".", ":", "[", "[[", "]]", "{", "}", "alias", "bg", "bind", "break", "builtin", "caller", "case", "cd",
    "command", "compgen", "complete", "compopt", "continue", "coproc", "declare", "dirs", "disown", "do", "done",
    "echo", "elif", "else", "enable", "esac", "eval", "exec", "exit", "export", "false", "fc", "fg", "fi", "for",
    "function", "getopts", "hash", "help", "history", "if", "in", "jobs", "kill", "let", "local", "logout",
    "mapfile", "popd", "printf", "pushd", "pwd", "read", "readarray", "readonly", "return", "select", "set",
    "shift", "shopt", "source", "suspend", "test", "then", "time", "times", "trap", "true", "type", "typeset",
    "ulimit", "umask", "unalias", "unset", "until", "wait", "while", "bash", "sh", NULL};

bool toolcmd_name_ok(const char *name) {
    if (!name || !(isalpha((unsigned char)*name) || *name == '_')) return false;
    for (const char *p = name + 1; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '-')) return false;
    for (size_t i = 0; RESERVED[i]; i++)
        if (!strcmp(RESERVED[i], name)) return false;
    return true;
}

bool toolcmd_transport_active(agent_t *a) { return a && a->tool_commands; }

size_t toolcmd_mounted_count(agent_t *a) {
    size_t n = 0;
    for (size_t i = 0; a && i < a->all_tools.n; i++)
        if (a->all_tools.v[i]->mounted) n++;
    return n;
}

static const char *const SPECIAL[] = {"resolve", "reject", "propose", "report_issue", NULL};

bool toolcmd_is_special(const char *name) {
    for (size_t i = 0; SPECIAL[i]; i++)
        if (str_eq(SPECIAL[i], name)) return true;
    return false;
}

static agent_tool_t *special_tool(agent_t *a, const char *name) {
    if (!toolcmd_is_special(name)) return NULL;
    agent_tool_t *t = agent_find_tool(a, name);
    return t;
}

void toolcmd_names(agent_t *a, strvec_t *out) {
    if (!toolcmd_transport_active(a)) return;
    for (size_t i = 0; i < a->all_tools.n; i++)
        if (a->all_tools.v[i]->mounted) strvec_push(out, a->all_tools.v[i]->name);
    for (size_t i = 0; SPECIAL[i]; i++)
        if (special_tool(a, SPECIAL[i]) && !strvec_contains(out, SPECIAL[i])) strvec_push(out, SPECIAL[i]);
}

static bool is_active(agent_t *a, const agent_tool_t *t) {
    for (size_t i = 0; i < a->tools.n; i++)
        if (a->tools.v[i] == t) return true;
    return false;
}

/* a tool's customWireName: `edit` goes on the wire as `apply_patch` in apply_patch mode (edit/index.ts) */
static const char *wire_alias(const agent_tool_t *t) {
    return str_eq(t->name, "edit") && str_eq(tool_edit_mode_setting(), "apply_patch") ? "apply_patch" : NULL;
}

/* resolveToolCommand: a mounted or active top-level tool by canonical name or unambiguous wire alias; an exact
 * registry name shadows aliases even when that tool is disabled */
agent_tool_t *toolcmd_resolve_tool(agent_t *a, const char *name) {
    agent_tool_t *hit = NULL;
    for (size_t i = 0; i < a->all_tools.n && !hit; i++)
        if (strcmp(a->all_tools.v[i]->name, name) == 0) hit = a->all_tools.v[i];
    if (!hit) {
        for (size_t i = 0; i < a->all_tools.n; i++) {
            agent_tool_t *t = a->all_tools.v[i];
            if (!str_eq(wire_alias(t), name)) continue;
            if (hit) return NULL;
            hit = t;
        }
    }
    return hit && (hit->mounted || is_active(a, hit)) ? hit : NULL;
}

/* built-ins are first-party; MCP and extension/custom tools carry third-party metadata (`dynamic`) */
static bool tool_is_builtin(const agent_tool_t *t) {
    return !str_starts(t->name, "mcp__") && !str_eq(t->name, "generate_image") && !str_eq(t->name, "tts");
}

/* summary, else first non-empty description line, else label/name */
static char *tool_summary(const agent_tool_t *t) {
    if (t->summary && *t->summary) return xstrdup(t->summary);
    const char *d = t->description ? t->description : "";
    while (*d) {
        const char *nl = strchr(d, '\n');
        size_t n = nl ? (size_t)(nl - d) : strlen(d);
        char *line = xstrndup(d, n);
        char *tr = str_trim(line);
        free(line);
        if (*tr) return tr;
        free(tr);
        if (!nl) break;
        d = nl + 1;
    }
    return xstrdup(t->label ? t->label : t->name);
}

/* promptCatalogSummary: first line, control chars stripped, bounded in UTF-8 bytes with an ellipsis */
static char *catalog_summary(const agent_tool_t *t, size_t max_bytes) {
    char *full = tool_summary(t);
    char *nl = strchr(full, '\n');
    if (nl) *nl = 0; /* a summary is one line */
    char *s = str_trim(full);
    free(full);
    sb_t b = SB_INIT;
    bool in_ctrl = false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bool ctrl = *p < 0x20 || *p == 0x7f;
        size_t skip = 0;
        if (p[0] == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) ctrl = true, skip = 1;
        else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) ctrl = true, skip = 2;
        if (ctrl) {
            if (!in_ctrl) sb_putc(&b, ' ');
            in_ctrl = true;
            p += skip;
            continue;
        }
        in_ctrl = false;
        sb_putc(&b, (char)*p);
    }
    free(s);
    char *raw = sb_detach(&b);
    char *cleaned = str_trim(raw);
    free(raw);
    if (max_bytes && strlen(cleaned) > max_bytes) {
        size_t cut = max_bytes - 3; /* "…" is 3 bytes */
        while (cut > 0 && ((unsigned char)cleaned[cut] & 0xC0) == 0x80) cut--;
        cleaned[cut] = 0;
        size_t n = strlen(cleaned);
        while (n && isspace((unsigned char)cleaned[n - 1])) cleaned[--n] = 0;
        char *r = xasprintf("%s…", cleaned);
        free(cleaned);
        cleaned = r;
    }
    if (!*cleaned) {
        free(cleaned);
        cleaned = xstrdup(t->name);
    }
    return cleaned;
}

json_t *toolcmd_entries(agent_t *a) {
    json_t *arr = json_arr();
    for (size_t i = 0; a && i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (!t->mounted) continue;
        bool dynamic = !tool_is_builtin(t);
        json_t *e = json_obj();
        json_set_str(e, "name", t->name);
        json_set(e, "summary", json_str_own(catalog_summary(t, dynamic ? EXTERNAL_DESCRIPTION_CAP : 0)));
        json_set_bool(e, "dynamic", dynamic);
        json_push(arr, e);
    }
    return arr;
}

/* JS String.prototype.slice(0, n) on UTF-16 units, approximated on code points (astral counts 2) */
static size_t utf16_prefix_bytes(const char *s, size_t units) {
    size_t u = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        size_t w = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 1;
        size_t cu = w == 4 ? 2 : 1;
        if (u + cu > units) break;
        u += cu;
        p += w;
    }
    return (size_t)(p - (const unsigned char *)s);
}

static size_t js_len(const char *s) { /* UTF-16 length */
    size_t u = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) u += *p >= 0xF0 ? 2 : 1;
    return u;
}

/* renderHelp: the description optionally capped for prompt inlining */
static char *render_help(const agent_tool_t *t, size_t description_cap) {
    char *desc = t->description ? xstrdup(t->description) : NULL;
    if (desc && description_cap && js_len(desc) > description_cap) {
        desc[utf16_prefix_bytes(desc, description_cap)] = 0;
        size_t n = strlen(desc);
        while (n && isspace((unsigned char)desc[n - 1])) desc[--n] = 0;
        json_t *ctx = json_obj();
        json_set_str(ctx, "name", t->name);
        char *cut = prompt_render("coding-agent/prompts/system/tool-command-description-cut.md", ctx);
        json_free(ctx);
        size_t cl = strlen(cut);
        while (cl && (cut[cl - 1] == '\n' || cut[cl - 1] == ' ')) cut[--cl] = 0;
        char *r = xasprintf("%s%s", desc, cut);
        free(cut);
        free(desc);
        desc = r;
    }
    char *h = toolcli_render_help(t->name, desc, t->parameters);
    free(desc);
    return h;
}

char *toolcmd_help(const agent_tool_t *t) { return render_help(t, 0); }

/* `## <name>\n\n<help, trimmed at the end>` */
static char *render_help_section(const agent_tool_t *t, size_t cap) {
    char *h = render_help(t, cap);
    size_t n = strlen(h);
    while (n && isspace((unsigned char)h[n - 1])) h[--n] = 0;
    char *r = xasprintf("## %s\n\n%s", t->name, h);
    free(h);
    return r;
}

static bool glob_match_simple(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (const char *p = s;; p++) {
            if (glob_match_simple(pat + 1, p)) return true;
            if (!*p) return false;
        }
    }
    if (*pat == '?') return *s && glob_match_simple(pat + 1, s + 1);
    return *s == *pat && glob_match_simple(pat + 1, s + 1);
}

static bool should_inline(const agent_tool_t *t, const char *mode) {
    if (str_eq(mode, "catalog")) return false;
    if (str_eq(mode, "inline") || tool_is_builtin(t)) return true;
    json_t *globs = settings_get("tools.commandInline"), *g;
    json_arr_foreach(globs, i, g) if (json_sv(g) && *json_sv(g) && glob_match_simple(json_sv(g), t->name)) return true;
    return false;
}

static const char *docs_mode(void) {
    const char *m = settings_str("tools.commandDocs", "catalog");
    return str_eq(m, "inline") || str_eq(m, "builtins") ? m : "catalog";
}

/* planToolCommandPromptDocs: inlined help sections, then catalog entries ({name: summary}, presentation order) */
static void plan_docs(agent_t *a, strvec_t *sections, json_t *catalog) {
    const char *mode = docs_mode();
    size_t used = 0;
    for (size_t i = 0; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (!t->mounted) continue;
        size_t cap = tool_is_builtin(t) ? 0 : EXTERNAL_DESCRIPTION_CAP;
        if (should_inline(t, mode)) {
            char *sec = render_help_section(t, cap);
            size_t n = js_len(sec);
            if (n <= DOCS_PER_TOOL_CAP && used + n <= DOCS_TOTAL_BUDGET) {
                used += n;
                strvec_push_own(sections, sec);
                continue;
            }
            free(sec);
        }
        json_set(catalog, t->name, json_str_own(catalog_summary(t, cap)));
    }
}

char *toolcmd_prompt_docs(agent_t *a, const strvec_t *listed_elsewhere) {
    strvec_t sections = {0};
    json_t *catalog = json_obj();
    plan_docs(a, &sections, catalog);
    json_t *entries = json_arr();
    const char *k;
    json_t *v;
    json_obj_foreach(catalog, i, k, v) {
        if (listed_elsewhere && strvec_contains(listed_elsewhere, k)) continue;
        json_t *e = json_obj();
        json_set_str(e, "name", k);
        json_set_str(e, "summary", json_sv(v));
        json_push(entries, e);
    }
    json_free(catalog);
    if (json_len(entries)) {
        json_t *ctx = json_obj();
        json_set(ctx, "entries", entries);
        char *cat = prompt_render("coding-agent/prompts/system/tool-command-catalog.md", ctx);
        json_free(ctx);
        strvec_push_own(&sections, cat);
    } else {
        json_free(entries);
    }
    char *r = strvec_join(&sections, "\n\n");
    strvec_free(&sections);
    return r;
}

char *toolcmd_docs_for(agent_t *a, const strvec_t *names) {
    const char *mode = docs_mode();
    strvec_t sections = {0};
    size_t used = 0;
    for (size_t k = 0; names && k < names->n; k++) {
        agent_tool_t *t = NULL;
        for (size_t i = 0; i < a->all_tools.n && !t; i++)
            if (a->all_tools.v[i]->mounted && str_eq(a->all_tools.v[i]->name, names->v[k])) t = a->all_tools.v[i];
        if (!t || !should_inline(t, mode)) continue;
        char *sec = render_help_section(t, tool_is_builtin(t) ? 0 : EXTERNAL_DESCRIPTION_CAP);
        size_t n = js_len(sec);
        if (n <= DOCS_PER_TOOL_CAP && used + n <= DOCS_TOTAL_BUDGET) {
            used += n;
            strvec_push_own(&sections, sec);
        } else {
            free(sec);
        }
    }
    char *r = strvec_join(&sections, "\n\n");
    strvec_free(&sections);
    return r;
}

/* formatMCPToolCommandGuidanceLabel: JSON.stringify with backtick, U+2028 and U+2029 escaped */
static char *guidance_label(const char *name) {
    sb_t q = SB_INIT;
    json_js_quote_sb(&q, name, strlen(name));
    char *enc = sb_detach(&q);
    sb_t b = SB_INIT;
    for (const char *p = enc; *p; p++) {
        if (*p == '`') sb_append(&b, "\\u0060");
        else if (!strncmp(p, "\xe2\x80\xa8", 3)) sb_append(&b, "\\u2028"), p += 2;
        else if (!strncmp(p, "\xe2\x80\xa9", 3)) sb_append(&b, "\\u2029"), p += 2;
        else sb_putc(&b, *p);
    }
    free(enc);
    return sb_detach(&b);
}

char *toolcmd_mcp_guidance(agent_t *a, strvec_t *routed_catalog) {
    strvec_t sec = {0};
    json_t *catalog = json_obj();
    plan_docs(a, &sec, catalog);
    strvec_free(&sec);
    json_t *tools = json_arr();
    size_t remaining = MCP_GUIDANCE_DATA_BUDGET, n = 0;
    bool omitted = false, catalog_only = false;
    for (size_t i = 0; i < a->all_tools.n; i++) {
        agent_tool_t *t = a->all_tools.v[i];
        if (!t->mounted || !t->mcp_server || !t->mcp_tool) continue;
        char *label = guidance_label(t->mcp_tool);
        size_t len = js_len(label) + js_len(t->name);
        if (n >= MCP_GUIDANCE_MAX_MAPPINGS || len > remaining) {
            omitted = true;
            free(label);
            continue;
        }
        remaining -= len;
        n++;
        json_t *e = json_obj();
        json_set(e, "mcpToolName", json_str_own(label));
        json_set_str(e, "path", t->name);
        const char *summary = json_get_str(catalog, t->name);
        if (summary) {
            json_set_str(e, "summary", summary);
            catalog_only = true;
            if (routed_catalog) strvec_push(routed_catalog, t->name);
        }
        json_push(tools, e);
    }
    json_free(catalog);
    if (!n && !omitted) {
        json_free(tools);
        return NULL;
    }
    json_t *ctx = json_obj();
    json_set(ctx, "tools", tools);
    json_set_bool(ctx, "hasCatalogOnlyTools", catalog_only);
    json_set_bool(ctx, "hasOmittedTools", omitted);
    char *r = prompt_render("coding-agent/prompts/system/mcp-tool-command-guidance.md", ctx);
    json_free(ctx);
    char *t = str_trim(r);
    free(r);
    return t;
}

/* ---------- special tools ---------- */

bool toolcmd_autoqa_enabled(void) {
    const char *env = getenv("PI_AUTO_QA");
    if (env && *env) return !(str_eq(env, "0") || str_ieq(env, "false") || str_ieq(env, "off"));
    json_t *configured = settings_get("dev.autoqa");
    if (configured) return json_truthy(configured);
    return !str_eq(settings_str("dev.autoqaConsent", "unset"), "denied");
}

static const char *REPORT_USAGE = "Expected `<tool>: <concise description>`, or the tool name on line 1 with the report "
                                  "body below.";

/* parseReportIssueBody: `<tool>: <desc>` (a colon followed by whitespace) or `tool\nbody` */
static bool parse_report(const char *text, char **tool, char **report, char **err) {
    char *body = str_trim(text);
    *tool = *report = *err = NULL;
    if (!*body) {
        *err = xasprintf("Empty report. %s", REPORT_USAGE);
        free(body);
        return false;
    }
    char *nl = strchr(body, '\n');
    if (nl) {
        char *t0 = xstrndup(body, (size_t)(nl - body));
        char *t = str_trim(t0), *r = str_trim(nl + 1);
        free(t0);
        if (*t && *r) {
            *tool = t;
            *report = r;
            free(body);
            return true;
        }
        free(t);
        free(r);
    }
    for (char *p = body; *p; p++) {
        if (*p == ':' && p[1] && isspace((unsigned char)p[1])) {
            if (p == body) break;
            char *t0 = xstrndup(body, (size_t)(p - body));
            char *t = str_trim(t0), *r = str_trim(p + 1);
            free(t0);
            if (*t && *r) {
                *tool = t;
                *report = r;
                free(body);
                return true;
            }
            free(t);
            free(r);
            break;
        }
    }
    free(body);
    *err = xasprintf("Invalid report format. %s", REPORT_USAGE);
    return false;
}

/* Records only after explicit consent (`dev.autoqaConsent: granted`, or PI_AUTO_QA_PUSH=1), to a local JSONL file.
 * omc has no collector endpoint, so nothing ever leaves the machine. */
static void record_issue(tool_ctx_t *ctx, const char *tool, const char *report) {
    const char *force = getenv("PI_AUTO_QA_PUSH");
    bool consent = (force && str_eq(force, "1")) || str_eq(settings_str("dev.autoqaConsent", "unset"), "granted");
    if (!consent) return;
    char *path = xasprintf("%s/autoqa.jsonl", omc_agent_dir());
    json_t *row = json_obj();
    json_set_str(row, "model", ctx && ctx->agent && ctx->agent->model ? ctx->agent->model->id : "unknown");
    json_set_str(row, "tool", str_starts(tool, "proxy_") ? tool + 6 : tool);
    json_set_str(row, "report", report);
    json_set_int(row, "created_at", (int64_t)time(NULL));
    char *line = json_dump(row, 0);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    free(line);
    json_free(row);
    free(path);
}

static int report_issue_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    UNUSED(self);
    const char *text = json_get_str(args, "report");
    if (toolcmd_autoqa_enabled()) {
        char *tool = NULL, *report = NULL, *err = NULL;
        if (!parse_report(text ? text : "", &tool, &report, &err)) {
            tool_result_error(out, "%s", err);
            free(err);
            return 0;
        }
        record_issue(ctx, tool, report);
        free(tool);
        free(report);
    }
    tool_result_text(out, "Noted, thanks!");
    return 0;
}

/* resolvePendingAction: apply or discard the pending staged action */
static void resolve_pending(tool_env_t *env, bool apply, const char *raw_reason, tool_result_t *out) {
    char *reason = str_trim(raw_reason ? raw_reason : "");
    bool is_error = false;
    json_t *details = NULL;
    char *r = astedit_resolve(env, apply, reason, &is_error, &details);
    if (r) {
        tool_result_text(out, r);
        out->is_error = is_error;
        out->details = details;
        free(r);
        free(reason);
        return;
    }
    json_free(details);
    const char *hint = env && env->plan_mode_active && env->plan_mode_active(env)
                           ? " To submit the plan for approval, use `propose` with its title instead."
                           : "";
    if (!apply) {
        tool_result_textf(out, "Nothing to reject; no pending action remains.%s", hint);
        out->details = json_obj();
        json_set_str(out->details, "action", "discard");
        json_set_str(out->details, "reason", reason);
    } else {
        tool_result_error(out, "No pending action to apply \xe2\x80\x94 `resolve` is only valid while a staged preview is pending.%s",
                          hint);
    }
    free(reason);
}

static int resolve_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    UNUSED(ctx);
    resolve_pending(self->ud, true, json_get_str(args, "reason"), out);
    return 0;
}

static int reject_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    UNUSED(ctx);
    resolve_pending(self->ud, false, json_get_str(args, "reason"), out);
    return 0;
}

static int propose_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    UNUSED(ctx);
    tool_env_t *env = self->ud;
    if (!env || !env->propose_plan || !(env->plan_mode_active && env->plan_mode_active(env))) {
        tool_result_error(out, "No plan is awaiting approval \xe2\x80\x94 `propose` only accepts a plan title while plan mode is active.");
        return 0;
    }
    char *title = str_trim(json_get_str(args, "title") ? json_get_str(args, "title") : "");
    bool err = false;
    json_t *inner = NULL;
    char *r = env->propose_plan(env, title, &err, &inner);
    tool_result_text(out, r ? r : "");
    out->is_error = err;
    out->details = inner;
    free(r);
    free(title);
    return 0;
}

static agent_tool_t *special_new(tool_env_t *env, const char *name, const char *label, const char *summary,
                                 const char *description, const char *field, const char *field_desc,
                                 int (*execute)(agent_tool_t *, tool_ctx_t *, const json_t *, tool_result_t *)) {
    json_t *s = json_obj(), *props = json_obj(), *f = json_obj(), *req = json_arr();
    json_set_str(s, "type", "object");
    json_set_str(f, "type", "string");
    json_set_str(f, "description", field_desc);
    json_set(props, field, f);
    json_set(s, "properties", props);
    json_push_str(req, field);
    json_set(s, "required", req);
    json_set_bool(s, "additionalProperties", false);
    agent_tool_t *t = agent_tool_new(name, label, description, s, execute);
    t->summary = xstrdup(summary);
    t->strict = true;
    t->intent_mode = INTENT_OMIT;
    t->hidden = true; /* presented by tools_finalize: a command while bash is on, top-level otherwise */
    t->ud = env;
    return t;
}

void toolcmd_register_special(agent_t *a, tool_env_t *env) {
    if (!agent_find_tool(a, "resolve"))
        agent_add_tool(a, special_new(env, "resolve", "Resolve", "Apply the pending staged action",
                                      "Apply the pending staged action (e.g. a tool preview), giving a one-sentence reason.",
                                      "reason", "One-sentence reason for applying the pending staged action.",
                                      resolve_execute));
    if (!agent_find_tool(a, "reject"))
        agent_add_tool(a, special_new(env, "reject", "Reject", "Discard the pending staged action",
                                      "Discard the pending staged action (e.g. a tool preview), giving a one-sentence reason.",
                                      "reason", "One-sentence reason for discarding the pending staged action.",
                                      reject_execute));
    if (!agent_find_tool(a, "propose"))
        agent_add_tool(a, special_new(env, "propose", "Propose", "Submit the plan for approval",
                                      "Submit the plan for approval by its <slug> (matching local://<slug>-plan.md). Valid only "
                                      "while plan mode is active.",
                                      "title", "Plan slug matching local://<slug>-plan.md.", propose_execute));
    if (toolcmd_autoqa_enabled() && !agent_find_tool(a, "report_issue"))
        agent_add_tool(a, special_new(env, "report_issue", "Report Tool Issue",
                                      "Report unexpected tool behavior for automated QA",
                                      "Report unexpected tool behavior for automated QA as `<tool>: <concise description>`. A "
                                      "two-line form also works: tool name on line 1, report body below.",
                                      "report", "Report in `<tool>: <concise description>` form.", report_issue_execute));
}

char *toolcmd_resolve_reminder(agent_t *a, const char *tool_name) {
    json_t *ctx = json_obj();
    json_set_str(ctx, "toolName", tool_name);
    json_set_bool(ctx, "isCommand", toolcmd_transport_active(a));
    json_set_str(ctx, "bashToolName", "bash");
    char *r = prompt_render("coding-agent/prompts/system/resolve-reminder.md", ctx);
    json_free(ctx);
    char *t = str_trim(r);
    free(r);
    return t;
}

/* ---------- command-line analysis ---------- */

typedef struct {
    bool word;
    char *raw, *value;
} shtok_t;

typedef VEC(shtok_t) shtoks_t;

static void shtoks_free(shtoks_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        free(v->v[i].raw);
        free(v->v[i].value);
    }
    free(v->v);
}

/* lexShellCommand (tools/shell-tokenize.ts) */
static void lex_shell(const char *cmd, shtoks_t *out) {
    sb_t value = SB_INIT;
    ssize_t word_start = -1, sep_start = -1;
    bool in_single = false, in_double = false;
    size_t n = strlen(cmd);
    for (size_t i = 0; i < n; i++) {
        char ch = cmd[i];
        if (in_single) {
            if (ch == '\'') in_single = false;
            else sb_putc(&value, ch);
            continue;
        }
        if (in_double) {
            if (ch == '\\' && i + 1 < n) {
                char nx = cmd[i + 1];
                if (nx == '"' || nx == '\\' || nx == '$' || nx == '`') {
                    sb_putc(&value, nx);
                    i++;
                    continue;
                }
            }
            if (ch == '"') in_double = false;
            else sb_putc(&value, ch);
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == ';' || ch == '&' || ch == '|' || ch == '(' || ch == ')') {
            if (word_start >= 0) {
                shtok_t t = {true, xstrndup(cmd + word_start, i - (size_t)word_start), xstrdup(sb_cstr(&value))};
                vec_push(out, t);
                sb_truncate(&value, 0);
                word_start = -1;
            }
            if (sep_start < 0) sep_start = (ssize_t)i;
            continue;
        }
        if (word_start < 0) {
            if (sep_start >= 0) {
                shtok_t t = {false, xstrndup(cmd + sep_start, i - (size_t)sep_start), NULL};
                vec_push(out, t);
                sep_start = -1;
            }
            word_start = (ssize_t)i;
        }
        if (ch == '\'') in_single = true;
        else if (ch == '"') in_double = true;
        else if (ch == '\\' && i + 1 < n) sb_putc(&value, cmd[++i]);
        else sb_putc(&value, ch);
    }
    if (word_start >= 0) {
        shtok_t t = {true, xstrdup(cmd + word_start), xstrdup(sb_cstr(&value))};
        vec_push(out, t);
    }
    if (sep_start >= 0) {
        shtok_t t = {false, xstrdup(cmd + sep_start), NULL};
        vec_push(out, t);
    }
    sb_free(&value);
}

typedef VEC(strvec_t) segments_t;

/* tokenizeShellSegments: the words of each segment (`;`, `&`, `|`, `(`, `)` and newlines split) */
static void shell_segments(const char *cmd, segments_t *out) {
    shtoks_t toks = {0};
    lex_shell(cmd, &toks);
    strvec_t cur = {0};
    for (size_t i = 0; i < toks.n; i++) {
        if (toks.v[i].word) {
            if (*toks.v[i].value) strvec_push(&cur, toks.v[i].value);
        } else if (strpbrk(toks.v[i].raw, "\n;&|()") && cur.n) {
            vec_push(out, cur);
            memset(&cur, 0, sizeof cur);
        }
    }
    if (cur.n) vec_push(out, cur);
    else strvec_free(&cur);
    shtoks_free(&toks);
}

static void segments_free(segments_t *v) {
    for (size_t i = 0; i < v->n; i++) strvec_free(&v->v[i]);
    free(v->v);
}

/* isShellEnvironmentAssignment: NAME=, NAME+=, NAME[x]= */
static bool env_assignment(const char *w) {
    if (!(isalpha((unsigned char)*w) || *w == '_')) return false;
    const char *p = w + 1;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    if (*p == '[') {
        const char *e = strchr(p, ']');
        if (!e) return false;
        p = e + 1;
    }
    if (*p == '+') p++;
    return *p == '=';
}

static const char *const PREFIX_WORDS[] = {"!", "{", "if", "then", "elif", "else", "do", "while", "until", "time", NULL};

static bool is_prefix_word(const char *w) {
    for (size_t i = 0; PREFIX_WORDS[i]; i++)
        if (!strcmp(PREFIX_WORDS[i], w)) return true;
    return false;
}

char *toolcmd_invoked(const char *cmd, const strvec_t *names) {
    if (!names || !names->n || !cmd) return NULL;
    segments_t segs = {0};
    shell_segments(cmd, (void *)&segs);
    char *hit = NULL;
    for (size_t s = 0; s < segs.n && !hit; s++) {
        for (size_t w = 0; w < segs.v[s].n; w++) {
            const char *word = segs.v[s].v[w];
            if (env_assignment(word) || is_prefix_word(word)) continue;
            if (strvec_contains(names, word)) hit = xstrdup(word);
            break;
        }
    }
    segments_free((void *)&segs);
    return hit;
}

bool toolcmd_starts_with(const char *cmd, const strvec_t *names) {
    if (!names || !names->n || !cmd) return false;
    segments_t segs = {0};
    shell_segments(cmd, (void *)&segs);
    bool r = segs.n && segs.v[0].n && strvec_contains(names, segs.v[0].v[0]);
    segments_free((void *)&segs);
    return r;
}

bool toolcmd_is_line(const char *cmd, const strvec_t *names) {
    if (!names || !names->n || !cmd) return false;
    shtoks_t toks = {0};
    lex_shell(cmd, &toks);
    bool start = true, ok = true;
    size_t commands = 0;
    for (size_t i = 0; i < toks.n && ok; i++) {
        shtok_t *t = &toks.v[i];
        if (!t->word) {
            sb_t op = SB_INIT;
            for (const char *p = t->raw; *p; p++)
                if (*p != ' ' && *p != '\t' && *p != '\n') sb_putc(&op, *p);
            const char *o = sb_cstr(&op);
            ok = !*o || str_eq(o, ";") || str_eq(o, "&&") || str_eq(o, "||") || str_eq(o, "|") || str_eq(o, "|&");
            sb_free(&op);
            if (strpbrk(t->raw, "\n;&|")) start = true;
            continue;
        }
        if (strpbrk(t->raw, "<>`$\r") || t->raw[0] == '#') {
            ok = false;
            break;
        }
        if (start) {
            if (!strvec_contains(names, t->value)) {
                ok = false;
                break;
            }
            commands++;
            start = false;
        }
    }
    shtoks_free(&toks);
    return ok && commands > 0;
}

char *toolcmd_call_name(const char *tool_name, const json_t *args) {
    if (!str_eq(tool_name, "bash")) return NULL;
    const char *cmd = json_get_str(args, "command");
    if (!cmd) return NULL;
    const char *p = cmd;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t n = strcspn(p, " \t\n\r\f\v;&|<>()");
    if (!n) return NULL;
    char *w = xstrndup(p, n);
    if (!toolcmd_name_ok(w)) {
        free(w);
        return NULL;
    }
    return w;
}

const json_t *toolcmd_dispatches(const char *tool_name, const json_t *details) {
    if (!str_eq(tool_name, "bash")) return NULL;
    return json_get_arr(details, "toolCommands");
}

const json_t *toolcmd_propose_details(const char *tool_name, const json_t *details, bool is_error) {
    if (str_eq(tool_name, "propose")) return is_error ? NULL : details;
    const json_t *d = toolcmd_dispatches(tool_name, details);
    for (size_t i = json_len(d); i-- > 0;) {
        const json_t *e = json_at(d, i);
        if (str_eq(json_get_str(e, "tool"), "propose") && str_eq(json_get_str(e, "mode"), "execute") &&
            json_get_int(e, "exitCode", 1) == 0)
            return json_get(e, "inner");
    }
    return NULL;
}

static char *invoked_command_name(const json_t *call) {
    const char *name = json_get_str(call, "name");
    char *c = toolcmd_call_name(name, json_get(call, "arguments"));
    return c ? c : xstrdup(name ? name : "");
}

bool toolcmd_is_resolution_call(const json_t *call) {
    char *n = invoked_command_name(call);
    bool r = str_eq(n, "resolve") || str_eq(n, "reject");
    free(n);
    return r;
}

bool toolcmd_is_propose_call(const json_t *call) {
    char *n = invoked_command_name(call);
    bool r = str_eq(n, "propose");
    free(n);
    return r;
}

/* ---------- images on stdout (utils/terminal-graphics.ts encodeTerminalImage) ---------- */

#define KITTY_CHUNK_BYTES 3072

/* a Kitty direct PNG transmission of an image block; NULL when it is no usable image */
static char *encode_terminal_image(const json_t *block) {
    const char *data = json_get_str(block, "data"), *mime = json_get_str(block, "mimeType");
    if (!data) return NULL;
    size_t n = 0;
    unsigned char *bytes = base64_decode(data, strlen(data), &n);
    if (!bytes) return NULL;
    uint8_t *png = NULL;
    size_t plen = 0;
    if (str_eq(mime, "image/png") && n >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8)) {
        png = bytes;
        plen = n;
        bytes = NULL;
    } else {
        img_t im = {0};
        char *err = NULL;
        if (img_decode(bytes, n, &im, NULL, &err) == 0) {
            png = img_png_encode(&im, 6, &plen);
            img_free(&im);
        }
        free(err);
    }
    free(bytes);
    if (!png) return NULL;
    sb_t b = SB_INIT;
    size_t count = plen ? (plen + KITTY_CHUNK_BYTES - 1) / KITTY_CHUNK_BYTES : 1;
    for (size_t i = 0; i < count; i++) {
        size_t off = i * KITTY_CHUNK_BYTES, len = MIN(KITTY_CHUNK_BYTES, plen - off);
        char *payload = base64_encode(png + off, len);
        int more = i + 1 < count ? 1 : 0;
        if (i == 0) sb_appendf(&b, "\x1b_Ga=T,t=d,f=100,q=2,m=%d;%s\x1b\\", more, payload);
        else sb_appendf(&b, "\x1b_Gm=%d;%s\x1b\\", more, payload);
        free(payload);
    }
    free(png);
    return sb_detach(&b);
}

static void append_line(sb_t *b, const char *text) {
    if (!text || !*text) return;
    sb_append(b, text);
    if (text[strlen(text) - 1] != '\n') sb_putc(b, '\n');
}

/* renderToolResult: text to stdout (stderr with exit 1 on error), images as terminal graphics on stdout */
static void render_tool_result(const tool_result_t *r, toolcli_result_t *res) {
    sb_t text = SB_INIT, images = SB_INIT;
    json_t *blk;
    json_arr_foreach(r->content, i, blk) {
        if (str_eq(json_get_str(blk, "type"), "text")) append_line(&text, json_get_str(blk, "text"));
        else if (str_eq(json_get_str(blk, "type"), "image")) {
            char *g = encode_terminal_image(blk);
            append_line(&images, g);
            free(g);
        }
    }
    if (r->is_error) {
        res->out = sb_detach(&images);
        res->err = sb_detach(&text);
        res->exit_code = 1;
    } else {
        sb_append(&text, sb_cstr(&images));
        sb_free(&images);
        res->out = sb_detach(&text);
        res->err = xstrdup("");
        res->exit_code = 0;
    }
}

/* ---------- runs ---------- */

struct toolcmd_run {
    char id[33];
    agent_t *agent;
    void *app;
    tool_env_t *env;
    char *tool_call_id;
    volatile sig_atomic_t *cancel; /* the agent's abort flag (agent lifetime) or own_cancel */
    volatile sig_atomic_t own_cancel;
    bool live, ended;
    int refs;
    unsigned invocations;
    json_t *records; /* index -> dispatch (null placeholders) */
    json_t *images;  /* image blocks the commands printed (omp decodes their terminal graphics back into images) */
    VEC(int) finished;
    void (*on_change)(void *ud);
    void *on_change_ud;
    pthread_mutex_t mu;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static VEC(toolcmd_run_t *) g_runs;
static char *g_sock_dir, *g_sock_path;
static int g_listen_fd = -1;
static char *g_helper;

static void run_unref(toolcmd_run_t *r) {
    pthread_mutex_lock(&r->mu);
    bool last = --r->refs == 0;
    pthread_mutex_unlock(&r->mu);
    if (!last) return;
    json_free(r->records);
    json_free(r->images);
    free(r->finished.v);
    free(r->tool_call_id);
    pthread_mutex_destroy(&r->mu);
    free(r);
}

void toolcmd_run_ref(toolcmd_run_t *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mu);
    r->refs++;
    pthread_mutex_unlock(&r->mu);
}

void toolcmd_run_unref(toolcmd_run_t *r) {
    if (r) run_unref(r);
}

static toolcmd_run_t *run_find(const char *id) {
    toolcmd_run_t *hit = NULL;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_runs.n && !hit; i++)
        if (!strcmp(g_runs.v[i]->id, id)) hit = g_runs.v[i];
    if (hit) {
        pthread_mutex_lock(&hit->mu);
        hit->refs++;
        pthread_mutex_unlock(&hit->mu);
    }
    pthread_mutex_unlock(&g_mu);
    return hit;
}

static void sock_cleanup(void) {
    if (g_sock_path) unlink(g_sock_path);
    if (g_sock_dir) rmdir(g_sock_dir);
}

static void *serve_main(void *p);

/* start the process-wide listener (lazy); false when no socket could be made */
static bool server_ensure(void) {
    if (g_listen_fd >= 0) return true;
    char *self = NULL;
#if defined(__APPLE__)
    extern char *su_self_exe(void);
    self = su_self_exe();
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        self = xstrdup(buf);
    }
#endif
    if (!self) return false;
    const char *tmp = env_get("TMPDIR");
    char *dir = xasprintf("%s/omc-tc-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (strlen(dir) + 3 >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        free(dir);
        dir = xstrdup("/tmp/omc-tc-XXXXXX");
    }
    if (!mkdtemp(dir)) {
        free(dir);
        free(self);
        return false;
    }
    char *path = xasprintf("%s/s", dir);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 64) != 0) {
        if (fd >= 0) close(fd);
        rmdir(dir);
        free(dir);
        free(path);
        free(self);
        return false;
    }
    g_sock_dir = dir;
    g_sock_path = path;
    g_helper = self;
    g_listen_fd = fd;
    atexit(sock_cleanup);
    pthread_t th;
    if (pthread_create(&th, NULL, serve_main, NULL) == 0) pthread_detach(th);
    return true;
}

toolcmd_run_t *toolcmd_run_begin(tool_ctx_t *ctx, tool_env_t *env) {
    agent_t *a = ctx ? ctx->agent : NULL;
    if (!a || !toolcmd_transport_active(a)) return NULL;
    pthread_mutex_lock(&g_mu);
    bool ok = server_ensure();
    pthread_mutex_unlock(&g_mu);
    if (!ok) return NULL;
    toolcmd_run_t *r = xcalloc(1, sizeof *r);
    char *hex = rand_hex(16);
    snprintf(r->id, sizeof r->id, "%s", hex);
    free(hex);
    r->agent = a;
    r->app = ctx->app;
    r->env = env;
    r->tool_call_id = xstrdup(ctx->tool_call_id ? ctx->tool_call_id : "");
    r->cancel = ctx->cancel == &a->cancel ? ctx->cancel : &r->own_cancel;
    r->live = true;
    r->refs = 1;
    r->records = json_arr();
    r->images = json_arr();
    pthread_mutex_init(&r->mu, NULL);
    pthread_mutex_lock(&g_mu);
    vec_push(&g_runs, r);
    pthread_mutex_unlock(&g_mu);
    return r;
}

static char *sh_squote(const char *s) {
    sb_t b = SB_INIT;
    sb_putc(&b, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') sb_append(&b, "'\\''");
        else sb_putc(&b, *p);
    }
    sb_putc(&b, '\'');
    return sb_detach(&b);
}

char *toolcmd_run_preamble(toolcmd_run_t *r) {
    if (!r) return xstrdup("");
    strvec_t names = {0};
    toolcmd_names(r->agent, &names);
    sb_t b = SB_INIT;
    char *qh = sh_squote(g_helper), *qs = sh_squote(g_sock_path);
    for (size_t i = 0; i < names.n; i++)
        sb_appendf(&b, "%s() { %s __tool-command %s %s %s \"$@\"; }; ", names.v[i], qh, qs, r->id, names.v[i]);
    free(qh);
    free(qs);
    strvec_free(&names);
    return sb_detach(&b);
}

json_t *toolcmd_run_images(toolcmd_run_t *r) {
    if (!r) return json_arr();
    pthread_mutex_lock(&r->mu);
    json_t *out = json_clone(r->images);
    pthread_mutex_unlock(&r->mu);
    return out;
}

json_t *toolcmd_run_records(toolcmd_run_t *r) {
    json_t *out = json_arr(), *e;
    if (!r) return out;
    pthread_mutex_lock(&r->mu);
    json_arr_foreach(r->records, i, e) if (!json_is_null(e)) json_push(out, json_clone(e));
    pthread_mutex_unlock(&r->mu);
    return out;
}

void toolcmd_run_on_change(toolcmd_run_t *r, void (*fn)(void *ud), void *ud) {
    if (!r) return;
    pthread_mutex_lock(&r->mu);
    r->on_change = fn;
    r->on_change_ud = ud;
    pthread_mutex_unlock(&r->mu);
}

void toolcmd_run_detach(toolcmd_run_t *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mu);
    r->live = false;
    r->on_change = NULL;
    pthread_mutex_unlock(&r->mu);
}

void toolcmd_run_end(toolcmd_run_t *r) {
    if (!r) return;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_runs.n; i++)
        if (g_runs.v[i] == r) {
            g_runs.v[i] = g_runs.v[--g_runs.n];
            break;
        }
    pthread_mutex_unlock(&g_mu);
    toolcmd_run_detach(r);
    run_unref(r);
}

/* ---------- dispatch (dispatchToolCommand + createToolCommandHandler) ---------- */

typedef struct {
    toolcmd_run_t *run;
    unsigned index;
    agent_tool_t *tool;
    char invocation_id[256];
    json_t *dispatch; /* {tool, mode, exitCode, args?, tier?} once the args validated (owned) */
    tool_ctx_t ctx;
} dispatch_t;

static void run_set_record(toolcmd_run_t *r, unsigned index, json_t *rec, bool finish) {
    pthread_mutex_lock(&r->mu);
    bool done = false;
    for (size_t i = 0; i < r->finished.n; i++) done |= r->finished.v[i] == (int)index;
    if (done && !finish) {
        pthread_mutex_unlock(&r->mu);
        json_free(rec);
        return;
    }
    while (json_len(r->records) <= index) json_push(r->records, json_null());
    json_t *slot = json_at(r->records, index);
    if (!finish && json_is_obj(slot)) { /* progress: only the inner details move */
        json_set(slot, "inner", json_take(rec, "inner"));
        json_free(rec);
    } else {
        json_free(json_arr_remove(r->records, index));
        json_arr_insert(r->records, index, rec);
    }
    if (finish) vec_push(&r->finished, (int)index);
    void (*fn)(void *) = r->live ? r->on_change : NULL;
    void *ud = r->on_change_ud;
    pthread_mutex_unlock(&r->mu);
    if (fn) fn(ud);
}

static void dispatch_progress(tool_ctx_t *ctx, const tool_result_t *partial) {
    dispatch_t *d = (dispatch_t *)((char *)ctx - offsetof(dispatch_t, ctx));
    if (!d->dispatch) return;
    json_t *rec = json_clone(d->dispatch);
    if (partial->details) json_set(rec, "inner", json_clone(partial->details));
    run_set_record(d->run, d->index, rec, false);
}

static json_t *dispatch_obj(const char *tool, const char *mode, int exit_code) {
    json_t *o = json_obj();
    json_set_str(o, "tool", tool);
    json_set_str(o, "mode", mode);
    json_set_int(o, "exitCode", exit_code);
    return o;
}

/* host.call: validate, approve, execute */
static void dispatch_call(void *ud, json_t *args, toolcli_result_t *res) {
    dispatch_t *d = ud;
    toolcmd_run_t *r = d->run;
    agent_tool_t *t = d->tool;
    tool_result_t out = {0};
    json_t *validated = json_clone(args);
    if (json_get(validated, INTENT_FIELD) && !schema_declares_property(t->parameters, INTENT_FIELD))
        json_del(validated, INTENT_FIELD);
    char *recv = NULL;
    char *verr = schema_validate_args(t->parameters, validated, &recv);
    if (verr && t->lenient_args) {
        free(verr);
        verr = NULL;
        json_del(validated, "__parseError");
        json_del(validated, "__rawJson");
    }
    if (verr) {
        char *help = toolcmd_help(t);
        tool_result_error(&out, "Invalid arguments for %s: Validation failed for tool \"%s\":\n%s\n\nReceived arguments:\n%s\n\n%s",
                          t->name, t->name, verr, recv ? recv : "{}", help);
        free(help);
        free(verr);
        free(recv);
        json_free(validated);
        d->dispatch = dispatch_obj(t->name, "execute", 1);
        render_tool_result(&out, res);
        tool_result_free(&out);
        return;
    }
    free(recv);
    /* the wrapped tool's approval tier for these args (prewalk/renderers); best effort */
    approval_call_t ac = {r->env, r->agent, t->name, validated};
    approval_decision_t dec;
    approval_tool_decision(&ac, &dec);
    const char *tier = approval_tier_name(dec.tier);
    d->dispatch = dispatch_obj(t->name, "execute", 1);
    /* PR creation renders only its operation and repository */
    if (str_eq(t->name, "github") && str_eq(json_get_str(validated, "op"), "pr_create")) {
        json_t *ra = json_obj();
        json_set_str(ra, "op", "pr_create");
        json_t *repo = json_get(validated, "repo");
        if (repo) json_set(ra, "repo", json_clone(repo));
        json_set(d->dispatch, "args", ra);
    } else {
        json_set(d->dispatch, "args", json_clone(validated));
    }
    json_set_str(d->dispatch, "tier", tier);
    approval_decision_free(&dec);
    /* each tool command passes its own approval when it runs */
    char *reason = NULL;
    agent_t *a = r->agent;
    bool blocked = false;
    if (a->hooks.approve_tool) {
        json_t *call = json_obj();
        json_set_str(call, "type", "toolCall");
        json_set_str(call, "id", d->invocation_id);
        json_set_str(call, "name", t->name);
        json_set(call, "arguments", json_clone(validated));
        blocked = a->hooks.approve_tool(a->hooks.ud, a, call, validated, &reason) == TOOL_BLOCK;
        json_free(call);
    }
    if (blocked) {
        tool_result_error(&out, "%s", reason ? reason : "Tool call was blocked");
    } else {
        d->ctx = (tool_ctx_t){.agent = a,
                              .tool_call_id = d->invocation_id,
                              .tool_name = t->name,
                              .cancel = r->cancel,
                              .update = dispatch_progress,
                              .app = r->app};
        t->execute(t, &d->ctx, validated, &out);
        if (!out.content) out.content = json_arr();
        json_set_int(d->dispatch, "exitCode", out.is_error ? 1 : 0);
        if (out.details) json_set(d->dispatch, "inner", json_clone(out.details));
    }
    free(reason);
    json_free(validated);
    /* the images the command prints as terminal graphics reach the bash result as image blocks */
    json_t *blk;
    pthread_mutex_lock(&r->mu);
    json_arr_foreach(out.content, i, blk) if (str_eq(json_get_str(blk, "type"), "image")) json_push(r->images, json_clone(blk));
    pthread_mutex_unlock(&r->mu);
    render_tool_result(&out, res);
    tool_result_free(&out);
}

/* one invocation: name, argv, cwd, stdin -> stdout/stderr/exit */
static void handle_request(toolcmd_run_t *r, const char *name, const strvec_t *argv, const char *cwd, const char *stdin_text,
                           size_t stdin_len, toolcli_result_t *res) {
    pthread_mutex_lock(&r->mu);
    unsigned index = r->invocations++;
    pthread_mutex_unlock(&r->mu);
    bool help_only = argv->n == 1 && (str_eq(argv->v[0], "-h") || str_eq(argv->v[0], "--help"));
    agent_t *a = r->agent;
    agent_tool_t *tool = NULL;
    if (toolcmd_transport_active(a)) tool = special_tool(a, name);
    if (!tool) {
        agent_tool_t *t = agent_find_tool(a, name);
        if (t && t->mounted) tool = t;
    }
    if (!tool || (r->cancel && *r->cancel)) {
        run_set_record(r, index, dispatch_obj(name, help_only ? "help" : "execute", 1), true);
        res->out = xstrdup("");
        res->err = xasprintf("%s: command is no longer available\n", name);
        res->exit_code = 1;
        return;
    }
    dispatch_t d = {.run = r, .index = index, .tool = tool};
    snprintf(d.invocation_id, sizeof d.invocation_id, "%s:cmd:%u", r->tool_call_id, index);
    toolcli_host_t host = {.description = tool->description, .schema = tool->parameters, .ud = &d, .call = dispatch_call};
    toolcli_opts_t o = {.cwd = cwd, .stdin_text = stdin_text, .stdin_len = stdin_len};
    toolcli_run(tool->name, argv, &o, &host, res);
    json_t *rec = d.dispatch ? d.dispatch : dispatch_obj(name, help_only ? "help" : "execute", 1);
    json_set_int(rec, "exitCode", res->exit_code);
    /* key order of the spread {...dispatch, exitCode}: exitCode keeps its place */
    run_set_record(r, index, rec, true);
}

/* ---------- socket protocol ---------- */

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

#define MAX_REQUEST (64u << 20)

static char *read_until_eof(int fd, size_t cap, size_t *len) {
    sb_t b = SB_INIT;
    char buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sb_appendn(&b, buf, (size_t)n);
        if (b.len > cap) break;
    }
    *len = b.len;
    return b.s ? sb_detach(&b) : xstrdup("");
}

static void *conn_main(void *p) {
    int fd = (int)(intptr_t)p;
    size_t len = 0;
    /* the client half-closes after its request */
    char *req = read_until_eof(fd, MAX_REQUEST, &len);
    json_t *j = len <= MAX_REQUEST ? json_parse(req, len, NULL) : NULL;
    free(req);
    toolcli_result_t res = {0};
    const char *name = json_get_str(j, "name");
    toolcmd_run_t *r = j ? run_find(json_get_str(j, "run") ? json_get_str(j, "run") : "") : NULL;
    if (!j || !name) {
        res.out = xstrdup("");
        res.err = xstrdup("tool command: malformed request\n");
        res.exit_code = 2;
    } else if (!r) {
        res.out = xstrdup("");
        res.err = xasprintf("%s: command is no longer available\n", name);
        res.exit_code = 1;
    } else {
        strvec_t argv = {0};
        json_t *e;
        json_arr_foreach(json_get_arr(j, "argv"), i, e) strvec_push(&argv, json_sv(e) ? json_sv(e) : "");
        json_t *si = json_get(j, "stdin");
        handle_request(r, name, &argv, json_get_str(j, "cwd"), json_sv(si), json_is_str(si) ? si->u.str.len : 0, &res);
        strvec_free(&argv);
        run_unref(r);
    }
    json_t *resp = json_obj();
    json_set_str(resp, "stdout", res.out ? res.out : "");
    json_set_str(resp, "stderr", res.err ? res.err : "");
    json_set_int(resp, "exit", res.exit_code);
    char *s = json_dump(resp, 0);
    write_all(fd, s, strlen(s));
    free(s);
    json_free(resp);
    toolcli_result_free(&res);
    json_free(j);
    close(fd);
    return NULL;
}

static void *serve_main(void *p) {
    UNUSED(p);
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    for (;;) {
        int c = accept(g_listen_fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            usleep(10000);
            continue;
        }
        fcntl(c, F_SETFD, FD_CLOEXEC);
        pthread_t th;
        if (pthread_create(&th, NULL, conn_main, (void *)(intptr_t)c) == 0) pthread_detach(th);
        else close(c);
    }
    return NULL;
}

/* ---------- client ---------- */

int toolcmd_client_main(int argc, char **argv) {
    /* argv: omc __tool-command <socket> <run> <name> [args...] */
    if (argc < 5) {
        fprintf(stderr, "omc __tool-command: missing arguments\n");
        return 2;
    }
    const char *sock = argv[2], *run = argv[3], *name = argv[4];
    bool want_stdin = false;
    for (int i = 5; i < argc; i++) want_stdin |= !strcmp(argv[i], "-");
    json_t *req = json_obj();
    json_set_str(req, "run", run);
    json_set_str(req, "name", name);
    json_t *av = json_arr();
    for (int i = 5; i < argc; i++) json_push(av, json_str_own(utf8_sanitize(argv[i], strlen(argv[i]))));
    json_set(req, "argv", av);
    char cwd[4096];
    json_set_str(req, "cwd", getcwd(cwd, sizeof cwd) ? cwd : ".");
    if (want_stdin) {
        size_t n = 0;
        char *in = read_until_eof(0, TOOLCLI_INPUT_LIMIT + 1, &n);
        json_set(req, "stdin", json_str_own(utf8_sanitize(in, n)));
        free(in);
    }
    char *msg = json_dump(req, 0);
    json_free(req);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || write_all(fd, msg, strlen(msg)) != 0) {
        free(msg);
        fprintf(stderr, "%s: command is no longer available\n", name);
        return 1;
    }
    free(msg);
    shutdown(fd, SHUT_WR);
    size_t n = 0;
    char *resp = read_until_eof(fd, (size_t)-1, &n);
    close(fd);
    json_t *j = json_parse(resp, n, NULL);
    free(resp);
    if (!j) {
        fprintf(stderr, "%s: command is no longer available\n", name);
        return 1;
    }
    json_t *o = json_get(j, "stdout"), *e = json_get(j, "stderr");
    if (json_is_str(o)) write_all(1, json_sv(o), o->u.str.len);
    if (json_is_str(e)) write_all(2, json_sv(e), e->u.str.len);
    int code = (int)json_get_int(j, "exit", 1);
    json_free(j);
    return code & 0xff;
}
