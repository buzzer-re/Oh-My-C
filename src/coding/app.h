/* app.h - coding-agent session runtime (port of AgentSession): model, tools, persistence, retry, compaction. */
#ifndef OMC_APP_H
#define OMC_APP_H

#include "agent/agent.h"
#include "coding/discovery.h"
#include "session/session.h"
#include "tools/tools.h"

typedef struct {
    const char *cwd;
    const char *model;          /* --model pattern (may include :effort) */
    const char *provider;       /* --provider */
    const char *api_key;        /* --api-key */
    const char *thinking;       /* --thinking */
    const char *session_dir;
    const char *resume;         /* --resume <id|path> */
    const char *fork;           /* --fork <id|path> */
    bool continue_recent;       /* -c */
    bool no_session;
    const char *system_prompt;
    const char *append_system_prompt;
    const strvec_t *tools;      /* --tools; NULL => defaults */
    bool no_tools;
    bool no_context_files, no_skills, no_rules;
    bool subagent;
    int subagent_depth;
    int max_turns;
    struct mcp_manager *mcp; /* borrowed shared MCP connections (subagents); NULL => discover */
    struct lsp_manager *lsp; /* borrowed shared LSP manager (subagents) */
    bool no_mcp;
} app_opts_t;

typedef struct app app_t;
typedef void (*app_notice_fn)(void *ud, const char *level, const char *text);

struct app {
    agent_t *agent;
    session_t *session;
    tool_env_t *env;
    discovery_t disc;
    const ai_model_t *model;
    ai_effort_t thinking;
    char *cwd;
    app_opts_t opts;
    char *sys_custom, *sys_append;
    double total_cost;
    int64_t last_context_tokens;
    bool compacting;
    bool title_done;
    int retry_count;
    struct ttsr *ttsr;
    struct mcp_manager *mcp;
    bool owns_mcp;
    struct lsp_manager *lsp;
    bool owns_lsp;
    bool plan_mode;
    char *plan_file; /* local:// path of the current plan */
    volatile bool ttsr_interrupting;
    app_notice_fn notice;
    void *notice_ud;
    pthread_mutex_t mu;
};

/* returns NULL and sets *err on failure */
app_t *app_create(const app_opts_t *o, char **err);
void app_free(app_t *a);

/* run a prompt to completion (blocking). images: array of image blocks or NULL. */
int app_prompt(app_t *a, const char *text, json_t *images);
int app_set_model(app_t *a, const char *pattern, char **err);
void app_set_thinking(app_t *a, ai_effort_t e);
/* enter/leave read-only plan mode (rebuilds the system prompt) */
void app_set_plan_mode(app_t *a, bool on);
void app_rebuild_system_prompt(app_t *a);
int app_compact(app_t *a, const char *instructions, char **err);
int app_new_session(app_t *a);
int app_switch_session(app_t *a, const char *path, char **err);
int app_fork_from(app_t *a, const char *entry_id, char **err);
void app_abort(app_t *a);
json_t *app_stats(app_t *a); /* {tokens, cost, messages, contextPercent, ...} owned */
char *app_model_label(app_t *a); /* "provider/id" owned */
const char *app_session_file(app_t *a);
/* run a one-shot completion with no tools (titles, summaries) */
char *app_complete_text(app_t *a, const ai_model_t *m, const char *system, const char *user, ai_effort_t effort);
/* subagent runner (task tool) */
char *app_run_subagent(tool_env_t *env, const char *agent_name, const char *assignment, const char *context,
                       json_t **details, volatile sig_atomic_t *cancel);

/* resolve api key for a model (auth.json / env / --api-key) */
char *app_api_key_for(const ai_model_t *m);
/* pick a default model: settings modelRoles.default, else first provider with credentials */
const ai_model_t *app_default_model(void);
/* parse "pattern:effort" */
const ai_model_t *app_resolve_model_pattern(const char *pattern, ai_effort_t *effort_out, bool *has_effort);

#endif
