/* toolcmd.h - session tools as bash commands (port of omp tools/tool-commands.ts, exec/tool-cli.ts,
 * exec/tool-command-host.ts, tools/resolve.ts, tools/report-tool-issue.ts).
 *
 * While the `bash` tool is active and `tools.commands` is on, discoverable tools (and MCP tools) are unmounted from the
 * request's tool list and run as bare commands inside a bash call:
 *   <tool> --help    -> usage derived from the tool's JSON schema
 *   <tool> args...   -> argv parsed against the schema, validated, approved and executed
 * The special tools `resolve`, `reject`, `propose` and `report_issue` (Auto-QA) run as commands too, or are presented
 * top-level when bash is off.
 *
 * omc runs the system's real bash (omp runs its own engine with session commands built in). Each run of a bash call
 * defines one shell function per command name, ahead of the sourced command, that execs `omc __tool-command` with the
 * run's token; that helper hands {name, argv, cwd, stdin} to the omc process over a private unix socket and prints the
 * tool's stdout / stderr / exit status like a command would. Every dispatch is recorded on the bash result as
 * `details.toolCommands`. */
#ifndef OMC_TOOLCMD_H
#define OMC_TOOLCMD_H

#include "agent/agent.h"
#include "core/json.h"

struct tool_env;

/* ---------- schema-derived command line (exec/tool-cli.ts) ---------- */

#define TOOLCLI_INPUT_LIMIT (16 * 1024 * 1024)

typedef struct {
    const char *cwd;        /* the shell's working directory (resolves @FILE) */
    const char *stdin_text; /* stdin read to EOF, only when argv holds a bare `-` */
    size_t stdin_len;
} toolcli_opts_t;

typedef struct {
    char *out, *err; /* owned */
    int exit_code;
    bool help; /* a `--help` answer */
} toolcli_result_t;

typedef struct {
    const char *description;
    const json_t *schema;
    void *ud;
    /* execute with parsed arguments (args borrowed); fills res */
    void (*call)(void *ud, json_t *args, toolcli_result_t *res);
} toolcli_host_t;

/* normalizeToolCliSchema: local $refs resolved, allOf/anyOf/oneOf folded, the protocol intent dropped. Owned. */
json_t *toolcli_normalize_schema(const json_t *schema);
/* renderToolHelp. Owned. */
char *toolcli_render_help(const char *name, const char *description, const json_t *schema);
/* parseToolArgs: owned args object, or NULL with *err (owned usage message) */
json_t *toolcli_parse_args(const json_t *schema, const strvec_t *argv, const toolcli_opts_t *o, char **err);
/* runToolCli: help, usage errors (exit 2) or host->call */
void toolcli_run(const char *name, const strvec_t *argv, const toolcli_opts_t *o, const toolcli_host_t *host,
                 toolcli_result_t *res);
void toolcli_result_free(toolcli_result_t *r);

/* ---------- mounted state (tools/tool-commands.ts) ---------- */

/* isToolCommandName: a command word bash does not reserve */
bool toolcmd_name_ok(const char *name);
/* commandTransportActive: tool commands are on and `bash` is active (set by tools_finalize) */
bool toolcmd_transport_active(agent_t *a);
/* number of mounted tools */
size_t toolcmd_mounted_count(agent_t *a);
/* toolCommandNames: mounted tools plus the special commands while the transport is active (owned list; empty off) */
void toolcmd_names(agent_t *a, strvec_t *out);
/* `{name, summary, dynamic}` entries of the mounted tools in presentation order (owned array) */
json_t *toolcmd_entries(agent_t *a);
/* the `--help` text of one tool (owned) */
char *toolcmd_help(const agent_tool_t *t);
/* the system prompt's tool-command docs under tools.commandDocs (renderToolCommandPromptDocs); tools in
 * `listed_elsewhere` (may be NULL) get no catalog line. Owned; "" when nothing. */
char *toolcmd_prompt_docs(agent_t *a, const strvec_t *listed_elsewhere);
/* toolCommandDocsFor: inlined help sections of the named mounted tools (owned, "" when none) */
char *toolcmd_docs_for(agent_t *a, const strvec_t *names);
/* "## MCP Tool Commands" guidance (mcp-tool-command-guidance.md) for mounted MCP tools; owned, NULL when none.
 * routed_catalog (may be NULL) receives the routed tools whose route line carries their catalog summary. */
char *toolcmd_mcp_guidance(agent_t *a, strvec_t *routed_catalog);
/* a mounted tool or an active top-level tool by canonical name or unambiguous wire alias (borrowed), NULL when none */
agent_tool_t *toolcmd_resolve_tool(agent_t *a, const char *name);

/* ---------- special tools (tools/resolve.ts, tools/report-tool-issue.ts) ---------- */

#define TOOLCMD_PREVIEW_PENDING_NOTICE                                                                                 \
    "Staged as a proposal \xe2\x80\x94 files NOT modified yet. To apply: `resolve <one-sentence reason>`. To discard: " \
    "`reject <one-sentence reason>`."

/* register resolve / reject / propose (and report_issue when Auto-QA is on) as hidden special tools */
void toolcmd_register_special(agent_t *a, struct tool_env *env);
bool toolcmd_is_special(const char *name);
/* Whether automated QA (`report_issue`) is enabled (dev.autoqa + consent, PI_AUTO_QA). */
bool toolcmd_autoqa_enabled(void);
/* the resolve-reminder custom message text for a staged preview of `tool_name` (owned) */
char *toolcmd_resolve_reminder(agent_t *a, const char *tool_name);

/* ---------- command-line analysis (tools/bash.ts, tools/tool-commands.ts) ---------- */

/* toolCommandCallName: the leading command word of a `bash` call's command when it is a valid command word; owned or
 * NULL (any other tool, or no such word) */
char *toolcmd_call_name(const char *tool_name, const json_t *args);
/* invokedToolCommand: the first session tool command a simple command of `cmd` starts with (owned or NULL) */
char *toolcmd_invoked(const char *cmd, const strvec_t *names);
/* startsWithToolCommand: the first simple command is a session tool command */
bool toolcmd_starts_with(const char *cmd, const strvec_t *names);
/* isToolCommandLine: every simple command invokes one of `names`, joined only by ; && || | and newlines */
bool toolcmd_is_line(const char *cmd, const strvec_t *names);
/* the dispatch records (details.toolCommands) of a finished `bash` result's details; borrowed array or NULL */
const json_t *toolcmd_dispatches(const char *tool_name, const json_t *details);
/* proposeDispatchDetails: the inner details of the last successful `propose` (direct or as a bash command); borrowed */
const json_t *toolcmd_propose_details(const char *tool_name, const json_t *details, bool is_error);
/* isPreviewResolutionToolCall / isProposeToolCall on an assistant tool call {name, arguments} */
bool toolcmd_is_resolution_call(const json_t *call);
bool toolcmd_is_propose_call(const json_t *call);

/* ---------- runs (exec/tool-command-host.ts) ---------- */

typedef struct toolcmd_run toolcmd_run_t;

/* a run of one bash call: NULL when the session runs no tool commands. The run outlives the call while the shell
 * still runs (an auto-backgrounded command); toolcmd_run_end releases it. */
toolcmd_run_t *toolcmd_run_begin(tool_ctx_t *ctx, struct tool_env *env);
/* shell text defining the run's command functions (owned; "" without a run) */
char *toolcmd_run_preamble(toolcmd_run_t *r);
/* the dispatches so far, in invocation order (owned array; empty when none) */
json_t *toolcmd_run_records(toolcmd_run_t *r);
/* the image blocks the run's commands printed, in order (owned array; empty when none). omp's shell decodes a command's
 * terminal graphics back into images of the bash result; omc's bash output drops the escape sequences, so the run hands
 * them over directly */
json_t *toolcmd_run_images(toolcmd_run_t *r);
/* hook for live bash frames: called (from a dispatch thread) after each dispatch progress or finish; fn NULL clears it,
 * waiting out a call in flight */
void toolcmd_run_on_change(toolcmd_run_t *r, void (*fn)(void *ud), void *ud);
/* an extra reference (a bash call that hands the run to a job but still reads its records) */
void toolcmd_run_ref(toolcmd_run_t *r);
void toolcmd_run_unref(toolcmd_run_t *r);
/* the bash call returned (its ctx is gone); dispatches keep running with the run's own state */
void toolcmd_run_detach(toolcmd_run_t *r);
/* the shell run is over: unregister and release */
void toolcmd_run_end(toolcmd_run_t *r);

/* `omc __tool-command <socket> <run> <name> [args...]`: the helper the command functions exec. Returns the exit code. */
int toolcmd_client_main(int argc, char **argv);

#endif
