/* app.h - coding-agent session runtime (port of AgentSession): model, tools, persistence, retry, compaction. */
#ifndef OMC_APP_H
#define OMC_APP_H

#include "agent/agent.h"
#include "config/model_resolver.h"
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
    bool allow_no_model; /* interactive: start without a model (pick one with /model or /login) */
    bool has_ui;                /* interactive UI can answer questions (enables `ask`) */
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
    int retry_offset;
    strvec_t retry_notes; /* recovery notes of this run's retried attempts, applied when a turn succeeds */ /* agent retry attempts spent before the latest fallback switch (fresh budget) */
    /* active retry fallback (turn-recovery.ts ActiveRetryFallbackState) */
    struct {
        bool active, pinned, served;
        char *role;     /* chain key that produced it */
        char *original; /* selector "provider/id[:level]" of the primary */
        ai_effort_t original_thinking, last_thinking;
    } fallback;
    struct ttsr *ttsr;
    struct mcp_manager *mcp;
    bool owns_mcp;
    struct lsp_manager *lsp;
    bool owns_lsp;
    struct dap_manager *dap;
    bool plan_mode;
    int64_t checkpoint_index;   /* agent message count at checkpoint (0 = none) */
    char *checkpoint_entry;     /* session entry id of the checkpoint tool result */
    char *pending_rewind;       /* report waiting for turn end */
    bool checkpoint_pending;    /* checkpoint tool ran; boundary captured at its message_end */
    char *plan_file; /* local:// path of the current plan */
    struct goal *goal; /* goal mode state (goal.c) */
    struct title_state *title; /* automatic session title (title.c) */
    struct skilldesc *skilldesc; /* compressed skill descriptions snapshot (skilldesc.c) */
    /* date/cwd reminder injected per request into user turns (never persisted) */
    int64_t dcr_first_ts;
    json_t *dcr_sent; /* "role:ts" -> reminder text the turn was first sent with */
    char *dcr_last;
    struct advisor *advisor; /* shadow reviewer (advisor.c), NULL when off */
    struct evalhost *evalhost; /* eval helpers' host state: completion handles, judge batches (evalhost.c) */
    struct memory_rt *memory;  /* memory backend runtime: prompt snapshot + startup pipeline (memory.c) */
    struct autolearn *autolearn; /* auto-learn controller + private capture runs (autolearn.c) */
    int64_t output_tokens;     /* cumulative assistant output tokens (atomic; eval `budget`) */
    /* `+Nk` / `+Nk!` per-turn budget directive of the latest user prompt (0 = none) */
    int64_t turn_budget_total;
    bool turn_budget_hard;
    int64_t turn_output_baseline;
    /* wake an idle session when a background job settles (modes set this; called from the job thread) */
    void (*wake)(void *ud);
    void *wake_ud;
    json_t *job_batch; /* jobs app_job_results_text took; details of the next async-result prompt */
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
/* run a hidden prompt (not shown in transcripts), e.g. goal continuations; custom_type tags the session entry */
int app_prompt_hidden(app_t *a, const char *text, const char *custom_type);
int app_set_model(app_t *a, const char *pattern, char **err);
void app_set_thinking(app_t *a, ai_effort_t e);
/* enter/leave read-only plan mode (rebuilds the system prompt) */
void app_set_plan_mode(app_t *a, bool on);
void app_rebuild_system_prompt(app_t *a);
/* token count at which context maintenance triggers for a window (omp compaction.threshold* semantics) */
int64_t app_compaction_threshold(int64_t window);
int app_compact(app_t *a, const char *instructions, char **err);
int app_new_session(app_t *a);
/* user `!cmd` / RPC bash: run, record a bashExecution message (session + context unless exclude); owned message
 * (plus BashResult counters) or NULL when the shell could not start */
json_t *app_user_bash(app_t *a, const char *cmd, bool exclude);
int app_switch_session(app_t *a, const char *path, char **err);
int app_fork_from(app_t *a, const char *entry_id, char **err);
void app_abort(app_t *a);
/* /btw: answer a side question from the current context without tools; not added to the history (owned) */
char *app_side_question(app_t *a, const char *question, char **err);
int app_branch_to(app_t *a, const char *entry_id);  /* move the session leaf and reload context */
int app_clear_context(app_t *a);                    /* reset boundary: empty context, same session */
int app_handoff(app_t *a, char **err);              /* handoff document + compact in place */
int app_retry(app_t *a, char **err);                /* drop a failed last assistant turn and continue (blocking) */
size_t app_shake(app_t *a, size_t min_chars);       /* elide large tool results from the live context; returns count */
char *app_generate_title(app_t *a);                 /* owned or NULL */
void app_set_advisor(app_t *a, bool on);
char *app_job_results_text(app_t *a); /* settled background jobs as the async-result notice (owned) or NULL */
/* after a run: hidden follow-up prompt (owned) from the advisor or goal mode, or NULL; *custom_type names it.
 * prev_type is the custom type of the run that just ended (NULL for a user prompt). */
char *app_after_run(app_t *a, const json_t *run_messages, const char *prev_type, const char *mode, const char **custom_type);
json_t *app_stats(app_t *a); /* {tokens, cost, messages, contextPercent, ...} owned */
char *app_model_label(app_t *a); /* "provider/id" owned */
const char *app_session_file(app_t *a);
/* run a one-shot completion with no tools (titles, summaries) */
/* plain title from the title model (title-system.md) for `text`; owned raw reply or NULL */
char *app_generate_title_from(app_t *a, const char *text);
char *app_complete_text(app_t *a, const ai_model_t *m, const char *system, const char *user, ai_effort_t effort);
/* child session for subagent `agent_name` (agents dirs); model_override (pattern) and append_prompt are optional */
app_t *app_subagent_new(app_t *parent, const char *agent_name, const char *model_override, const char *append_prompt,
                        char **err);
/* subagent runner (task tool) */
char *app_run_subagent(tool_env_t *env, const char *agent_name, const char *assignment, const char *context,
                       json_t **details, volatile sig_atomic_t *cancel);

/* resolve api key for a model (auth.json / env / --api-key) */
char *app_api_key_for(const ai_model_t *m);
/* pick a default model: settings modelRoles.default, else first provider with credentials */
const ai_model_t *app_default_model(void);
/* a role's model (resolveModelRoleValue("@role") over its candidate pool) */
const ai_model_t *app_role_model(const char *role, ai_effort_t *effort_out, bool *has_effort);
/* first answer from `models` (skipping the session model), then the session model; owned or NULL */
char *app_complete_first(app_t *a, const mr_pool_t *models, const char *system, const char *user, ai_effort_t effort);
/* compaction model candidates in attempt order */
void app_compaction_candidates(app_t *a, mr_pool_t *out);
/* parse "pattern:effort" */
const ai_model_t *app_resolve_model_pattern(const char *pattern, ai_effort_t *effort_out, bool *has_effort);

#endif
