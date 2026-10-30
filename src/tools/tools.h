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
typedef struct {
    /* ask the user; returns owned JSON answer object or NULL if unavailable/cancelled */
    json_t *(*ask)(void *ud, const json_t *questions);
    void *ud;
} tool_ui_t;

/* ---------- environment shared by all tools of a session ---------- */
typedef struct tool_env {
    char *cwd;
    edit_store_t store;
    todo_state_t todo;
    tool_ui_t ui;
    json_t *settings;          /* borrowed effective settings */
    char *edit_mode;           /* "hashline" | "replace" | "patch" | "apply_patch" */
    char *artifacts_dir;       /* where spilled outputs go */
    int subagent_depth;
    void *session;             /* app session (for task tool) */
    struct shell *shell;       /* persistent bash session (lazy) */
    const discovery_t *disc;   /* skills/rules for skill:// and rule:// (borrowed) */
    /* called after write/edit lands a file; returns owned text appended to the result (e.g. LSP diagnostics) */
    char *(*after_write)(struct tool_env *env, const char *abs_path);
    char *local_dir;           /* root for local:// session artifacts */
    /* write to xd://propose (plan approval); returns owned result text, sets *is_error */
    char *(*propose_plan)(struct tool_env *env, const char *content, bool *is_error);
    /* spawn a subagent: returns owned final text; used by task tool (set by app) */
    char *(*run_subagent)(struct tool_env *env, const char *agent_name, const char *assignment, const char *context,
                          json_t **out_details, volatile sig_atomic_t *cancel);
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
const char *const *tools_default_names(void);

/* individual constructors */
agent_tool_t *tool_read_new(tool_env_t *env);
agent_tool_t *tool_write_new(tool_env_t *env);
agent_tool_t *tool_edit_new(tool_env_t *env);
agent_tool_t *tool_bash_new(tool_env_t *env);
agent_tool_t *tool_grep_new(tool_env_t *env);
agent_tool_t *tool_glob_new(tool_env_t *env);
agent_tool_t *tool_todo_new(tool_env_t *env);
agent_tool_t *tool_ask_new(tool_env_t *env);
agent_tool_t *tool_web_search_new(tool_env_t *env);
agent_tool_t *tool_task_new(tool_env_t *env);
agent_tool_t *tool_eval_new(tool_env_t *env); /* NULL if no Python */

/* agent definitions (bundled + ~/.omc/agent/agents + .omc/agents): owned object name -> {name, description, tools[], model?, body} */
json_t *agent_defs_load(const char *cwd);

/* GitHub pr:// and issue:// reads */
int github_read(tool_env_t *env, const char *url, tool_result_t *out);

/* helpers shared across tools */
char *tool_prompt(const char *name, const json_t *ctx); /* renders prompts/coding-agent/prompts/tools/<name>.md */
json_t *schema_obj(void);
void schema_prop(json_t *schema, const char *name, const char *type, const char *desc, bool required);
void schema_prop_json(json_t *schema, const char *name, json_t *prop, bool required);
/* edit-engine entry (hashline) used by edit tool and tests */
int hashline_apply(tool_env_t *env, const char *input, tool_result_t *out);
int replace_apply(tool_env_t *env, const char *path, const char *old_s, const char *new_s, bool all, tool_result_t *out);
/* line-numbered rendering used by read & grep */
void format_hashline_header(sb_t *b, const char *display_path, const char *tag);

#endif
