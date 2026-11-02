/* tools.h - built-in coding-agent tools (read, write, edit, bash, grep, glob, todo, ask, web_search, task, ...). */
#ifndef OMC_TOOLS_H
#define OMC_TOOLS_H

#include "agent/agent.h"
#include "coding/discovery.h"
#include "core/core.h"
#include "core/json.h"

#include <pthread.h>

/* ---------- snapshot store (hashline tags + seen lines) ---------- */
typedef struct {
    int64_t lo, hi;
} line_span_t;

typedef struct {
    char *path;     /* absolute */
    char *tag;      /* 4 uppercase hex */
    char *content;  /* full text at snapshot time */
    VEC(line_span_t) seen; /* 1-based inclusive line ranges shown to the model; empty = unrestricted */
} snapshot_t;

typedef struct {
    pthread_mutex_t mu;
    VEC(snapshot_t *) snaps; /* newest last */
    hmap_t registers;        /* named clipboard registers: name -> char* (owned) */
    hmap_t noop;             /* hashline no-op streaks: abs path -> "payloadhash:count" (owned) */
} edit_store_t;

void edit_store_init(edit_store_t *s);
void edit_store_free(edit_store_t *s);
char *file_tag(const char *text, size_t len); /* 4-hex tag of normalized text (owned) */
/* record a snapshot (copies content), returns tag (borrowed from the store) */
const char *edit_store_record(edit_store_t *s, const char *abs_path, const char *content, int64_t seen_lo,
                              int64_t seen_hi);
snapshot_t *edit_store_find(edit_store_t *s, const char *abs_path, const char *tag); /* borrowed */

/* ---------- todo state ---------- */
typedef struct {
    json_t *phases; /* [{name, tasks:[{content, status, note?}]}] */
} todo_state_t;

/* ---------- UI callbacks (interactive mode supplies these; NULL in print/rpc) ---------- */
struct approval_prompt;
typedef struct {
    /* ask the user; returns owned JSON answer object or NULL if unavailable/cancelled */
    json_t *(*ask)(void *ud, const json_t *questions);
    /* a tool call that already returned has a new result frame (async task progress): details owned; called
     * from any thread (the UI posts it to its own) */
    void (*tool_frame)(void *ud, const char *tool_call_id, json_t *details);
    /* tool approval prompt (overlays/approval-dialog.ts) for call `tool_call_id`; `status` is formatApprovalPrompt
     * (the blocked run status). Blocks the calling tool thread; returns an approval_answer_kind_t (tools/approval.h)
     * with *grant / *always set for a grant answer. `cancel` aborts the wait (deny). NULL: no UI can answer. */
    int (*approve)(void *ud, const char *tool_call_id, const char *tool_name, const char *status,
                   const struct approval_prompt *prompt, int *grant, bool *always, volatile sig_atomic_t *cancel);
    void *ud;
} tool_ui_t;

/* ---------- environment shared by all tools of a session ---------- */
typedef struct tool_env {
    char *cwd;
    edit_store_t store;
    todo_state_t todo;
    tool_ui_t ui;
    json_t *settings;          /* borrowed effective settings */
    char *edit_mode;           /* "hashline" | "replace" | "patch" | "apply_patch" | "sloppy" */
    struct artifact_store *artifacts; /* session artifacts (artifact://<id>; shared with subagents; NULL: none) */
    int subagent_depth;
    bool can_prompt_user;      /* a UI can answer `ask` (interactive mode) */
    bool ask_ui;               /* `--mode rpc-ui`: the RPC client answers `ask` (tool UI over the protocol) */
    bool find_enabled;         /* `find` is part of this session (set by tools_register_builtin) */
    const char *delegation_bias; /* model delegation bias ("eager"/"gated"/"restrained"; static) */
    void *session;             /* app session (for task tool) */
    struct shell *shell;       /* persistent bash session (lazy) */
    const discovery_t *disc;   /* skills/rules for skill:// and rule:// (borrowed) */
    /* called after write/edit lands a file; returns owned text appended to the result (e.g. LSP diagnostics) */
    char *(*after_write)(struct tool_env *env, const char *abs_path);
    char *(*after_edit)(struct tool_env *env, const char *abs); /* edit-tool variant (lsp.diagnosticsOnEdit) */
    char *local_dir;           /* root for local:// session artifacts */
    /* one judgment through the judge role chain (find): normalized questions {id: {type, instructions}}; owned
     * answers keyed by id or NULL with *err (owned); usage accumulates [input, output, cost] (set by app) */
    json_t *(*judge)(struct tool_env *env, const json_t *state, const json_t *questions, volatile sig_atomic_t *cancel,
                     double usage[3], char **err);
    /* write to xd://propose (plan approval) with the trimmed plan title; returns owned result text, sets
     * *is_error and *details (owned PlanApprovalDetails {planFilePath, title, planExists} or NULL) */
    char *(*propose_plan)(struct tool_env *env, const char *title, bool *is_error, json_t **details);
    /* spawn a subagent: returns owned final text; used by task tool (set by app) */
    char *(*run_subagent)(struct tool_env *env, const char *agent_name, const char *assignment, const char *context,
                          json_t **out_details, volatile sig_atomic_t *cancel);
    struct jobs *jobs;         /* async background jobs (owned; NULL disables async) */
    struct conflict_history *conflicts; /* conflict:// blocks registered by read (owned, lazy; iurl.h) */
    /* eval kernel helpers that reach the session (completion/judge/judge_batch/budget/...; see coding/evalhost.h):
     * owned value or NULL with *err (owned). cell = calling cell serial, cancel = this call's abort flag. */
    json_t *(*eval_bridge)(struct tool_env *env, const char *name, const json_t *args, int64_t cell,
                           volatile sig_atomic_t *cancel, char **err);
    pthread_mutex_t mu;
} tool_env_t;

tool_env_t *tool_env_new(const char *cwd);
void tool_env_free(tool_env_t *e);

/* map internal URLs (skill://, rule://, artifact://) to a local path, keeping any :selector suffix.
 * Returns owned path or NULL if not an internal URL; sets *err (owned) when the URL is unknown. */
char *tool_resolve_internal_url(tool_env_t *env, const char *url, char **err);
/* resolve a model-supplied path against env cwd (expands ~), owned */
char *tool_resolve_path(tool_env_t *env, const char *p);
/* truncate output to a byte/line budget keeping the tail; returns owned text + notice */
char *tool_truncate_tail(const char *s, size_t len, size_t max_bytes, size_t max_lines, bool *truncated);
char *tool_truncate_head(const char *s, size_t len, size_t max_bytes, size_t max_lines, bool *truncated);

/* builtin registration */
void tools_register_builtin(agent_t *a, tool_env_t *env, const strvec_t *enabled /* NULL => defaults */);
const char *const *tools_default_names(void); /* canonical built-in order (NULL-terminated) */
/* order tools canonically and mount discoverable ones under xd:// (call after all tools are added) */
void tools_finalize(agent_t *a, const strvec_t *requested);
bool tools_scout_available(void);                     /* `scout` not in task.disabledAgents */
bool tools_eager_delegation(const tool_env_t *env);   /* model delegation bias is "eager" */

/* individual constructors */
agent_tool_t *tool_read_new(tool_env_t *env);
agent_tool_t *tool_write_new(tool_env_t *env);
agent_tool_t *tool_edit_new(tool_env_t *env);
agent_tool_t *tool_bash_new(tool_env_t *env);
agent_tool_t *tool_wait_new(tool_env_t *env);
/* render settled background jobs ([{jobId,label,result}]) as the async-result notice (owned) */
char *jobs_render_results(const json_t *jobs);
agent_tool_t *tool_grep_new(tool_env_t *env);
agent_tool_t *tool_glob_new(tool_env_t *env);
agent_tool_t *tool_todo_new(tool_env_t *env);
/* /todo helpers (tools/todo.ts): one todo op applied to a copy of `phases` (in-progress normalized; errors appended);
 * phasesToMarkdown; markdownToPhases (errors appended). All return owned values. */
json_t *todo_apply_op(const json_t *phases, const char *op, const json_t *args, strvec_t *errors);
char *todo_phases_to_markdown(const json_t *phases);
json_t *todo_markdown_to_phases(const char *md, strvec_t *errors);
agent_tool_t *tool_ask_new(tool_env_t *env);
agent_tool_t *tool_web_search_new(tool_env_t *env);
agent_tool_t *tool_task_new(tool_env_t *env);
agent_tool_t *tool_eval_new(tool_env_t *env); /* NULL if no Python */
agent_tool_t *tool_ast_grep_new(tool_env_t *env);
agent_tool_t *tool_find_new(tool_env_t *env);
agent_tool_t *tool_ast_edit_new(tool_env_t *env);
/* finalize a staged ast_edit proposal (xd://resolve / xd://reject) */
char *astedit_resolve(tool_env_t *env, bool apply, const char *reason, bool *is_error);
/* re-render the eval description with the session tool names its bridge can call */
void eval_set_tool_names(agent_tool_t *t, const json_t *names);

/* agent definitions (bundled + ~/.omc/agent/agents + .omc/agents): owned object name -> {name, description, tools[], model?, body} */
json_t *agent_defs_load(const char *cwd);

/* GitHub pr:// and issue:// resources (github.c): rendered markdown (*content, owned) or -1 with *err (owned) */
int github_resolve(tool_env_t *env, const char *url, char **content, bool *markdown, char **err);
/* ssh://[user@]host[:port]/path (ssh.c): remote UTF-8 text (<=1 MiB) or one-level dir listing; bare ssh:// lists hosts */
int ssh_fetch(tool_env_t *env, const char *url, char **content, size_t *len, bool *is_dir, char **err);
int ssh_store(tool_env_t *env, const char *url, const char *data, size_t len, char **err);
bool ssh_available(void);
char *ssh_quote(const char *s); /* POSIX single-quote */

/* helpers shared across tools */
char *tool_prompt(const char *name, const json_t *ctx); /* renders prompts/coding-agent/prompts/tools/<name>.md */
json_t *schema_obj(void);
/* parse a JSON schema literal (exact upstream wire schema, minus the injected `i`); aborts if malformed */
json_t *schema_lit(const char *json);
void schema_prop(json_t *schema, const char *name, const char *type, const char *desc, bool required);
void schema_prop_json(json_t *schema, const char *name, json_t *prop, bool required);
/* line-numbered rendering used by read & grep */
void format_hashline_header(sb_t *b, const char *display_path, const char *tag);
/* omp grouped-file-output display text (details.displayContent) from
 * [{path, tag?, suffix?, lines: [{line, match, mark?, text}]}] (line 0: verbatim row); grouped = `# dir/` path tree */
char *grep_display_content(const json_t *files, bool grouped);
/* omp formatGroupedPaths: prefix-folded `# dir/` tree of bare names (dirs end with `/`); owned */
char *format_grouped_paths(const strvec_t *paths);

#endif
