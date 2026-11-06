/* app.h - coding-agent session runtime (port of AgentSession): model, tools, persistence, retry, compaction. */
#ifndef OMC_APP_H
#define OMC_APP_H

#include "agent/agent.h"
#include "ai/anthropic_slow.h"
#include "config/model_resolver.h"
#include "coding/commands.h"
#include "coding/discovery.h"
#include "core/proc.h"
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
    const char *session_file;   /* persist the session at this path (subagent transcripts); wins over no_session */
    const char *system_prompt;
    const char *append_system_prompt;
    const char *system_prompt_template; /* --system-prompt-template: Handlebars template text replacing the bundled one */
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
    bool rpc_ui;                /* `--mode rpc-ui`: tool UI (ask) over the RPC protocol */
    bool interactive_prompts;   /* ACP client with form elicitation: `ask` only, no other UI behavior */
    bool auto_approve;          /* --auto-approve / --yolo: every approval resolves in yolo mode */
    const char *service_tier;   /* --service-tier (an OpenAI tier setting value; "none" omits); NULL = not given */
    const ai_service_tiers_t *service_tiers; /* subagents: the resolved per-family map (tier.subagent) */
    const strvec_t *add_dirs;   /* extra workspace roots (--add-dir + workspace.additionalDirectories; a subagent
                                 * gets its parent's); NULL => workspace.additionalDirectories */
    int prewalk;                /* --prewalk / --prewalk-into (1), --no-prewalk (-1), else prewalk.enabled (0) */
    const char *prewalk_into;   /* --prewalk-into */
} app_opts_t;

typedef struct app app_t;
typedef void (*app_notice_fn)(void *ud, const char *level, const char *text);

struct app {
    agent_t *agent;
    session_t *session;
    tool_env_t *env;
    discovery_t disc;
    file_cmds_t file_cmds;     /* `commands/<name>.md` slash commands + bundled /init (commands.c) */
    prompt_tpls_t prompt_tpls; /* prompts/<name>.md templates */
    const ai_model_t *model;
    ai_effort_t thinking;
    char *cwd;
    app_opts_t opts;
    char *sys_custom, *sys_append;
    double total_cost;
    int64_t last_context_tokens;
    bool compacting;
    struct cmp_spec *cmp_spec; /* background speculative compaction run (compaction.c), NULL when idle */
    bool title_done;
    int retry_count;
    int retry_offset;
    int retry_saga; /* attempt of the last auto_retry_start (0: no retry saga open) */
    char *prompt_refusal; /* why the last APP_PROMPT_REFUSED prompt was refused (owned) */
    int stall_continues; /* committed-text stream-stall continuations this prompt (turn_recovery.h) */
    bool text_buffered;  /* streamed text is not committed to an output sink yet (subagents, print-mode text):
                          * omp setTextOutputCommitted(false), a failed turn's partial text is replay-safe */
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
    json_t *rewind_graveyard;   /* messages a rewind cut during TURN_END; freed at the next turn/run start
                                 * (the event and later listeners still point at the turn's assistant) */
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
    /* /force (setForcedToolChoice): the user-force sequence [named choice, "none"] served one per model request
     * (extracmds.c); forced_step 0 idle, 1 the named choice next, 2 "none" next */
    json_t *forced_choice;
    int forced_step;
    bool think_force; /* externalThinking: the next request forces the `think` scratchpad (once per user turn) */
    void (*plan_mode_changed)(void *ud); /* plan mode left by an automatic approval (ACP mode update) */
    /* ACP clients with form elicitation confirm a proposed plan (#requestAcpPlanApprovalChoice): true approves,
     * false keeps planning (refine). NULL approves automatically. Uses plan_mode_changed_ud. */
    bool (*plan_approval_choice)(void *ud, const char *title, const char *content);
    void *plan_mode_changed_ud;
    struct goal *goal; /* goal mode state (goal.c) */
    struct prewalk *prewalk; /* one-shot model handoff (prewalk.c); top-level sessions only */
    struct title_state *title; /* automatic session title (title.c) */
    struct skilldesc *skilldesc; /* compressed skill descriptions snapshot (skilldesc.c) */
    /* date/cwd reminder injected per request into user turns (never persisted) */
    struct dcr *dcr;      /* the main loop's date/cwd reminder injector, journaled in the session */
    struct dcr *dcr_side; /* the injector of another agent on the main transform (unjournaled) */
    struct advisor *advisor; /* shadow reviewer (advisor.c), NULL when off or no advisor model */
    bool advisor_enabled;    /* the session's advisor setting (active only with a model for the 'advisor' role) */
    struct evalhost *evalhost; /* eval helpers' host state: completion handles, judge batches (evalhost.c) */
    struct memory_rt *memory;  /* memory backend runtime: prompt snapshot + startup pipeline (memory.c) */
    struct autolearn *autolearn; /* auto-learn controller + private capture runs (autolearn.c) */
    struct xdnotice *xdnotice;   /* xd:// mount-delta notices (xdnotice.c) */
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
    /* structured session events beyond the agent's own (omp AgentSessionEvent: model_changed,
     * thinking_level_changed, goal_updated, queue_update, auto/manual_compaction_*, auto_retry_*); RPC sets it.
     * Called from any thread with a borrowed event. */
    void (*session_event)(void *ud, const json_t *ev);
    void *session_event_ud;
    json_t *queue_snapshot; /* last queue_update {steering, followUp} sent (coalescing) */
    /* `auto` thinking (autothink.c): per-turn classification; `thinking` holds the provisional/resolved effort */
    bool auto_thinking;
    int auto_resolved;      /* last classified effort, -1 before the first turn */
    int abort_gen;          /* bumped by app_abort: a prompt superseded during auto-thinking setup is dropped */
    struct guards *guards;  /* loop guard, todo reminders, stop recovery (guards.c) */
    struct thinkloop *thinkloop; /* stream loop guard of the current assistant stream (thinkloop.c) */
    int gemini_interrupt;   /* planning headers counted when the Gemini header guard interrupted the stream */
    /* outermost permission gate (ACP client permission, omp #wrapToolForAcpPermission), run before the approval
     * gate: TOOL_BLOCK with *reason, or TOOL_ALLOW with *client_approved when the client approved this call */
    tool_gate_t (*permission_gate)(void *ud, app_t *a, const json_t *call, const json_t *args, bool *client_approved,
                                   char **reason);
    void *permission_gate_ud;
    /* service tiers, fast/slow mode (modelctl.c) */
    ai_service_tiers_t tiers;      /* live per-family map (guarded by tier_mu) */
    pthread_mutex_t tier_mu;
    json_t *fast_disabled;         /* Anthropic endpoint+model keys whose fast mode was rejected (tier_mu) */
    char *slow_lane;               /* account lane of the latest Anthropic subscription request (tier_mu) */
    int64_t wrap_up_hinted;        /* wrap-up window already steered (-1 none) */
    anthropic_slow_hooks_t slow_hooks;
    struct cache_warmer *warmer;   /* prompt cache warmer (cachewarm.c), NULL for subagents */
    strvec_t provider_states; /* provider session-state keys requests created since the last /fresh (under mu) */
    pthread_mutex_t mu;
};

/* app notices beyond info/warning/error: "retry_fallback_applied" (from\x1fto\x1frole\x1freason),
 * "retry_fallback_succeeded" (model\x1frole), "retry-recovered" (notes, one per line), "title" */
char *app_fallback_notice_text(const char *fields); /* "Fallback: from -> to\nreason" (owned) */
/* the turn-recovery transient-error classification (a retry may succeed) */
/* hand a structured session event (owned) to app->session_event; true when a listener took it (the caller then
 * skips the plain-text notice the TUI renders for the same transition) */
bool app_emit(app_t *a, json_t *ev);
/* thinkingLevel as omp reports it: NULL (undefined) without a reasoning model, else the clamped effort name */
const char *app_wire_thinking(app_t *a);
/* model_changed / thinking_level_changed after a model mutation (prev_thinking = app_wire_thinking before) */
void app_model_state_changed(app_t *a, const struct ai_model *prev_model, const char *prev_thinking);
/* select an exact catalog model like an explicit /model pick (session entry, thinking re-clamp, events) */
void app_use_model(app_t *a, const struct ai_model *m);
void app_apply_model(app_t *a, const struct ai_model *m, bool has_effort, ai_effort_t effort);
/* queue_update when the displayable steering / follow-up queue differs from the last one sent */
void app_queue_changed(app_t *a);
/* AgentSession.getQueuedMessages(): owned {steering: [text], followUp: [text], liveSteered: 0} */
json_t *app_queued_messages(app_t *a);

/* getImageAttachments (imgattach.c): the newest user/developer message (or `ask` result) with images as
 * [{label "Image #N", uri "attachment://N", mimeType, sourcePath}] (owned); app_bind_image_attachments wires it
 * into the tools' env for attachment:// */
json_t *app_image_attachments(app_t *a);
void app_bind_image_attachments(app_t *a);

/* returns NULL and sets *err on failure */
app_t *app_create(const app_opts_t *o, char **err);
void app_free(app_t *a);

/* run a prompt to completion (blocking). images: array of image blocks or NULL. */
/* a prompt refused before it reached the agent (no model credential and no fallback); the error was reported */
#define APP_PROMPT_REFUSED (-3)
int app_prompt(app_t *a, const char *text, json_t *images);
/* app_prompt plus hidden notices (owned array of messages, may be NULL) placed before the user message */
int app_prompt_ex(app_t *a, const char *text, json_t *images, json_t *notices);
/* composer attachments [{image, path?}] (owned): images normalized for the model, a hidden
 * image-attachment notice naming each source file (omp renderAttachmentSourceNotice) */
int app_prompt_attachments(app_t *a, const char *text, json_t *attachments);
/* steer a running agent with a composer submission and its attachments (owned) */
void app_steer_attachments(app_t *a, const char *text, json_t *attachments);
/* ---------- non-builtin slash commands (AgentSession.prompt expansion, skill commands) ---------- */
/* #tryExecuteCustomCommand (MCP prompt commands `<server>:<prompt> key=value`), expandSlashCommand (file
 * commands), expandPromptTemplate. Returns the text to send (owned; a copy of `text` when nothing matched),
 * or NULL when a command consumed the input (an MCP prompt that produced no text). `mcp` false skips MCP
 * prompt commands (UI-thread previews). */
char *app_expand_input(app_t *a, const char *text, bool mcp);
/* steer()/followUp(): only prompt templates expand (owned) */
char *app_expand_templates(app_t *a, const char *text);
/* the MCP prompt command named by "/<name> ..." exists */
bool app_is_mcp_prompt_command(app_t *a, const char *text);
/* ["skill:<name>" ...] are runnable (skills.enableSkillCommands) and `text` invokes a loaded skill: the skill
 * (borrowed) and the parsed invocation (owned, free with skill_invocation_free); NULL otherwise */
const skill_t *app_skill_command(app_t *a, const char *text, skill_invocation_t *inv);
/* the user-invoked `skill-prompt` custom message for `text` (owned; images appended to its content), or
 * NULL with *err ("Failed to load skill" cause) */
json_t *app_skill_message(app_t *a, const char *text, json_t *images, char **err);
/* promptCustomMessage for a skill message (owned): runs the turn (blocking) */
int app_prompt_skill(app_t *a, json_t *msg);
/* run a hidden prompt (not shown in transcripts), e.g. goal continuations; custom_type tags the session entry */
int app_prompt_hidden(app_t *a, const char *text, const char *custom_type);
int app_resume(app_t *a, char **err); /* the `.`/`c` continue shortcut; -1 + *err "Nothing to resume" */
int app_set_model(app_t *a, const char *pattern, char **err);
void app_set_thinking(app_t *a, ai_effort_t e);
/* `auto`: classify each user turn (provisional level until the first); persists configured "auto" */
void app_set_thinking_auto(app_t *a);
/* parse "auto" or an effort name and apply it; false for an unknown level */
bool app_set_thinking_level(app_t *a, const char *level);
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
/* CompactionResult of the latest compaction entry on the current path (owned; {} when none) */
json_t *app_compaction_result(app_t *a);
/* manual /compact with a mode (NULL, "soft", "remote", "snapcompact": compact-modes.ts) */
int app_compact_mode(app_t *a, const char *mode, const char *instructions, char **err);
int app_new_session(app_t *a);
/* user `!cmd` / RPC bash: run, record a bashExecution message (session + context unless exclude); owned message
 * (plus BashResult counters) or NULL when the shell could not start */
json_t *app_user_bash(app_t *a, const char *cmd, bool exclude);
/* app_user_bash in two steps: run (streams merged output to on_output, stops when *cancel turns non-zero)
 * without touching the session, then record the result (session + context unless excluded). */
json_t *app_user_bash_run(app_t *a, const char *cmd, bool exclude, proc_output_cb on_output, void *ud,
                          volatile sig_atomic_t *cancel);
void app_record_bash(app_t *a, const json_t *result);
/* `$ code`: run Python in the eval tool's shared kernel (NULL when the eval tool is unavailable), then record
 * the pythonExecution message like app_record_bash */
json_t *app_user_python_run(app_t *a, const char *code, bool exclude, volatile sig_atomic_t *cancel);
void app_record_python(app_t *a, const json_t *result);
/* apply the steeringMode / followUpMode / interruptMode settings to the agent */
void app_sync_queue_modes(app_t *a);
int app_switch_session(app_t *a, const char *path, char **err);
int app_fork_from(app_t *a, const char *entry_id, char **err);
/* /btw branch: fork the session at `leaf_id` (which must still be the leaf of `session_id`) and append the side
 * question and its answer (branchFromBtw). 0, or -1 + *err (owned). */
/* AgentSession.navigateTree: move the leaf to `target_id` in the same session file. A user request
 * rewinds past itself (leaf = its parent, its text in *editor_text); other custom messages likewise;
 * any other entry becomes the leaf. With `summarize`, the abandoned branch (old leaf back to the common
 * ancestor) is summarized by the current model (branch-summary prompt, or `custom_instructions`) and a
 * branch_summary entry is recorded at the new position. Blocking (run off the UI thread when summarizing).
 * Returns 0 ok, 1 aborted (app_abort_branch_summary), -1 error (*err owned). */
typedef struct {
    char *editor_text;   /* owned, NULL when none */
    bool summarized;     /* a branch_summary entry was written */
    bool moved;          /* the leaf changed (false: already there) */
} app_nav_result_t;
int app_navigate_tree(app_t *a, const char *target_id, bool summarize, const char *custom_instructions,
                      app_nav_result_t *out, char **err);
void app_abort_branch_summary(app_t *a);
int app_branch_from_btw(app_t *a, const char *question, const json_t *images, const json_t *assistant,
                        const char *leaf_id, const char *session_id, char **err);
void app_abort(app_t *a);
/* AgentSession.runEphemeralTurn: one side-channel turn on the current model, system prompt, tool catalog and
 * history (plus `streaming`, the in-flight assistant reply, and the no-tools reminder), never recorded. */
typedef struct {
    const char *prompt_text;
    const json_t *prompt_images;  /* image blocks or NULL */
    const json_t *history;        /* detached side messages placed before the prompt, or NULL */
    const char *conversation_key; /* provider lineage of a keyed side conversation; NULL: one-off */
    const json_t *streaming;      /* snapshot of the streaming assistant message, or NULL */
    bool dedupe;                  /* dedupeEphemeralReply the reply */
    size_t reply_max_bytes;       /* with dedupe: byte cap, 0 = unlimited */
    void (*on_text_delta)(void *ud, const char *delta); /* called on the requesting thread */
    void *ud;
    volatile sig_atomic_t *cancel;
} app_ephemeral_t;
/* Returns the final assistant message (toolCall blocks dropped, owned) and *reply (owned: trimmed, deduped when
 * asked), or NULL with *err (owned; "Request was aborted" after a cancel). */
json_t *app_ephemeral_turn(app_t *a, const app_ephemeral_t *e, char **reply, char **err);
char *app_dedupe_ephemeral_reply(const char *text, size_t max_bytes); /* owned */
int app_branch_to(app_t *a, const char *entry_id);  /* move the session leaf and reload context */
int app_clear_context(app_t *a);                    /* reset boundary: empty context, same session */
/* AgentSession.freshSession: rotate the provider session id and drop provider stream state; -1 while streaming,
 * else the number of provider states pruned */
int app_fresh_session(app_t *a);
/* /move: relocate the session to `dir` (an existing directory) and re-scope the process, settings, discovery,
 * commands and the system prompt to it (InteractiveMode.applyCwdChange). 0, or -1 with *err (owned); a failure
 * leaves the session where it was */
int app_move(app_t *a, const char *dir, char **err);
/* multi-root workspace edits applied to the live session (system prompt refreshed) */
int app_add_workspace_dir(app_t *a, const char *dir, char **resolved, char **err); /* session_add_workspace_dir */
int app_remove_workspace_dir(app_t *a, const char *dir, char **resolved);          /* session_remove_workspace_dir */
/* formatWorkspaceDirectories: "Workspace directories:\n  <cwd> (working directory)\n  <extra>..." with an
 * optional leading note line (owned) */
char *app_workspace_dirs_text(app_t *a, const char *note);
/* resolveToCwd for a user-typed path (~, absolute, "/" = cwd) (owned) */
char *app_resolve_user_path(app_t *a, const char *p);
int app_handoff(app_t *a, const char *focus, char **err); /* handoff document (+ focus instructions) + compact in place */
int app_retry(app_t *a, char **err);                /* drop a failed last assistant turn and continue (blocking) */
/* /shake (shake-types.ts ShakeResult): mode "elide" | "images" | "thinking"; rewrites the branch on disk */
typedef struct {
    const char *mode;
    int tool_results_dropped, blocks_dropped, images_dropped, thinking_dropped;
    int64_t tokens_freed;
} app_shake_result_t;
void app_shake(app_t *a, const char *mode, app_shake_result_t *out);
char *app_shake_summary(const app_shake_result_t *r); /* formatShakeSummary (owned) */
char *app_generate_title(app_t *a);                 /* owned or NULL */
bool app_set_advisor(app_t *a, bool on);           /* returns whether an advisor is now active */
char *app_job_results_text(app_t *a); /* settled background jobs as the async-result notice (owned) or NULL */
/* a user interrupt ended the wait on background jobs: settled results fold into context without a turn (the next
 * prompt sees them; omp #recordWithoutTurn). Returns a clone of the recorded message for the transcript, or NULL. */
json_t *app_fold_job_results(app_t *a);
/* hasPendingAsyncWork: a background job is running or a settled one is not yet delivered */
bool app_pending_async_work(app_t *a);
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
/* … whose transcript persists at `session_file` (`<artifacts>/<id>.jsonl`; NULL = in memory) */
app_t *app_subagent_new_at(app_t *parent, const char *agent_name, const char *model_override, const char *append_prompt,
                           const char *session_file, char **err);
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
/* getRoleModelCycle: cycleOrder roles that resolve over the available chat models (owned array, free with
 * app_role_cycle_free) and, in *current, the live one's index (recorded role while its model is live, else
 * the first entry running the session model, else 0) */
typedef struct {
    char *role;
    int order_index; /* index in app_cycle_order() */
    const ai_model_t *model;
    const char *level; /* static or NULL */
    bool explicit_level;
} app_role_entry_t;
size_t app_role_cycle(app_t *a, app_role_entry_t **out, int *current);
void app_role_cycle_free(app_role_entry_t *v, size_t n);
/* applyRoleModel: the role's model (model_change role = the role) and its explicit thinking level */
void app_apply_role_entry(app_t *a, const app_role_entry_t *e);
/* setModelTemporary: session-only switch (model_change role "temporary" unless `role`), never persisted;
 * `level` an explicit thinking selector or NULL (keep / re-clamp). -1 + *err "No API key for p/id" */
int app_set_model_temporary(app_t *a, const ai_model_t *m, const char *level, const char *role, char **err);
/* resolveTemporaryModelThinkingLevel: explicit level of a configured role that resolves to `m` (static) */
const char *app_temporary_thinking(const ai_model_t *m);
/* a role's model (resolveModelRoleValue("@role") over its candidate pool) */
const ai_model_t *app_role_model(const char *role, ai_effort_t *effort_out, bool *has_effort);
/* first answer from `models` (skipping the session model), then the session model; owned or NULL */
char *app_complete_first(app_t *a, const mr_pool_t *models, const char *system, const char *user, ai_effort_t effort);
/* helpers shared with compaction.c: a notice through a->notice, the agent context rebuilt from the session
 * (load_session_into_agent), and the per-request options of the live loop (prepareSimpleStreamOptions) */
void app_notice(app_t *a, const char *level, const char *fmt, ...) PRINTF_LIKE(3, 4);
void app_reload_context(app_t *a);
void app_fill_request_opts(app_t *a, ai_stream_opts_t *o);
const json_t *app_session_metadata(app_t *a, const char *provider);

/* record paid cache-warm requests (cachewarm.c) as model_usage entries; call from the session's own thread */
void app_flush_warm_usage(app_t *a);
/* compaction model candidates in attempt order */
void app_compaction_candidates(app_t *a, mr_pool_t *out);
/* parse "pattern:effort" */
const ai_model_t *app_resolve_model_pattern(const char *pattern, ai_effort_t *effort_out, bool *has_effort);

#endif
