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
    strvec_t retry_notes;
    char *model_role; /* role of the last model change made through a role (Ctrl+P cycle), NULL otherwise */ /* recovery notes of this run's retried attempts, applied when a turn succeeds */ /* agent retry attempts spent before the latest fallback switch (fresh budget) */
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
    char *plan_file; /* plan-mode state planFilePath (local://PLAN.md or the agent's local://<slug>-plan.md) */
    bool plan_paused, plan_has_entered, plan_reentry;
    bool plan_review_ui;        /* xd://propose opens a review (TUI) instead of approving automatically */
    char *plan_reference_path;  /* approved plan re-injected (plan-mode-reference.md) after history rewrites */
    bool plan_reference_sent;
    const ai_model_t *plan_prev_model; /* the model before the `plan` role took over (restored on exit) */
    ai_effort_t plan_prev_thinking;
    bool plan_prev_set;
    const ai_model_t *plan_yolo_target; /* --plan-yolo: hand-off model once the plan is proposed */
    ai_effort_t plan_yolo_thinking;
    bool plan_yolo_has_thinking, plan_yolo_armed, plan_yolo_done;
    int plan_reminder_count;    /* plan-mode decision reminders since the last ask/propose or user turn */
    bool plan_reminder_awaiting, plan_force_tool_choice;
    void (*plan_mode_changed)(void *ud); /* plan mode left by an automatic approval (ACP mode update) */
    void *plan_mode_changed_ud;
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

/* app notices beyond info/warning/error: "retry_fallback_applied" (from\x1fto\x1frole\x1freason),
 * "retry_fallback_succeeded" (model\x1frole), "retry-recovered" (notes, one per line), "title" */
char *app_fallback_notice_text(const char *fields); /* "Fallback: from -> to\nreason" (owned) */

/* returns NULL and sets *err on failure */
app_t *app_create(const app_opts_t *o, char **err);
void app_free(app_t *a);

/* run a prompt to completion (blocking). images: array of image blocks or NULL. */
int app_prompt(app_t *a, const char *text, json_t *images);
/* app_prompt plus hidden notices (owned array of messages, may be NULL) placed before the user message */
int app_prompt_ex(app_t *a, const char *text, json_t *images, json_t *notices);
/* composer attachments [{image, path?}] (owned): images normalized for the model, a hidden
 * image-attachment notice naming each source file (omp renderAttachmentSourceNotice) */
int app_prompt_attachments(app_t *a, const char *text, json_t *attachments);
/* steer a running agent with a composer submission and its attachments (owned) */
void app_steer_attachments(app_t *a, const char *text, json_t *attachments);
/* run a hidden prompt (not shown in transcripts), e.g. goal continuations; custom_type tags the session entry */
int app_prompt_hidden(app_t *a, const char *text, const char *custom_type);
int app_set_model(app_t *a, const char *pattern, char **err);
void app_set_thinking(app_t *a, ai_effort_t e);
/* ---------- plan mode (planmode.c) ---------- */
/* enter/leave read-only plan mode (mode_change entries, system prompt rebuild) */
void app_set_plan_mode(app_t *a, bool on);
void app_enter_plan_mode(app_t *a, const char *plan_file); /* NULL: the reference path / local://PLAN.md */
void app_exit_plan_mode(app_t *a, bool paused);
void app_plan_apply_model(app_t *a); /* switch to modelRoles.plan (interactive plan entry) */
void app_set_plan_file(app_t *a, const char *plan_file);   /* promote a reviewed plan into plan-mode state */
void app_set_plan_review_ui(app_t *a, bool on);
/* restore plan mode from the session's last mode_change (resume / switch): 1 plan, 2 paused, 0 none */
int app_plan_restore_from_session(app_t *a);
void app_exit_plan_mode_silently(app_t *a);
bool app_plan_should_start(app_t *a); /* plan.defaultOnStartup applies to this (fresh) session */
void app_set_plan_reference(app_t *a, const char *path);
json_t *app_plan_reference_message(app_t *a); /* hidden plan-mode-reference prompt once per rewrite, or NULL */
char *app_plan_path(app_t *a, const char *url);  /* on-disk path (owned) */
char *app_plan_read(app_t *a, const char *url);  /* owned content or NULL */
int app_plan_write(app_t *a, const char *url, const char *content);
strvec_t app_plan_list(app_t *a);                 /* local:// plan files (name ends in plan.md), newest first */
int app_plan_resolve(app_t *a, const char *supplied, char **path, char **content, char **title, char **err);
char *app_plan_normalize_title(const char *title, char **err);
char *app_plan_resolve_title(const char *supplied, const char *content, const char *path);
char *app_plan_humanize_title(const char *title);
char *app_plan_save_file_name(const char *title);
char *app_plan_autosave(app_t *a, const char *title, const char *content, char **err); /* path, NULL if off */
char *app_plan_approved_prompt(app_t *a, const char *path, const char *content, bool preserved);
int app_prompt_synthetic(app_t *a, const char *text); /* hidden agent-attributed developer prompt (blocking) */
/* plan-mode decision enforcement (#enforcePlanModeDecisionAtSettle): after a run that ended in plain text,
 * append the decision reminder and continue with tool_choice "required" (at most 3 times) */
int app_plan_settle(app_t *a, int rc);
void app_plan_yolo_arm(app_t *a); /* --plan-yolo: enter the plan phase before the first prompt */
void app_plan_note_tool_end(app_t *a, const char *tool, const tool_result_t *res, bool is_error);
void app_plan_note_user_turn(app_t *a);
typedef struct {
    char *role;
    const ai_model_t *model;
    ai_effort_t thinking;
    bool explicit_thinking;
} app_role_model_t;
/* configured role models in cycleOrder (smol, default, slow); *current = the active one's index */
size_t app_role_model_cycle(app_t *a, app_role_model_t **out, int *current);
void app_role_model_cycle_free(app_role_model_t *v, size_t n);
void app_apply_role_model(app_t *a, const app_role_model_t *e);
void app_rebuild_system_prompt(app_t *a);
/* token count at which context maintenance triggers for a window (omp compaction.threshold* semantics) */
int64_t app_compaction_threshold(const ai_model_t *m, int64_t window); /* with compaction.modelThresholds */
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
/* same with a custom system prompt (NULL = title-system.md); owned raw reply or NULL */
char *app_generate_title_with(app_t *a, const char *text, const char *system_prompt);
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
/* cycleOrder setting (default smol, default, slow) */
strvec_t app_cycle_order(void);
/* Ctrl+P role cycling (model-controls.ts cycleRoleModels): switches the session model to the next role
 * model; returns the new role's index in app_cycle_order(), or -1 when one or no role model resolves */
int app_cycle_role_model(app_t *a, bool backward);
/* a role's model (resolveModelRoleValue("@role") over its candidate pool) */
const ai_model_t *app_role_model(const char *role, ai_effort_t *effort_out, bool *has_effort);
/* first answer from `models` (skipping the session model), then the session model; owned or NULL */
char *app_complete_first(app_t *a, const mr_pool_t *models, const char *system, const char *user, ai_effort_t effort);
/* compaction model candidates in attempt order */
void app_compaction_candidates(app_t *a, mr_pool_t *out);
/* parse "pattern:effort" */
const ai_model_t *app_resolve_model_pattern(const char *pattern, ai_effort_t *effort_out, bool *has_effort);

#endif
