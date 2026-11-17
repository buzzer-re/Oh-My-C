/* agent.h - agent runtime: LLM turn loop with tool calling (port of packages/agent). */
#ifndef OMC_AGENT_H
#define OMC_AGENT_H

#include "ai/ai.h"
#include "core/core.h"
#include "core/json.h"

#include <pthread.h>
#include <signal.h>

typedef struct agent agent_t;
typedef struct agent_tool agent_tool_t;

/* ---------- tools ---------- */
typedef struct {
    json_t *content; /* array of {type:text|image} blocks (owned) */
    json_t *details; /* tool-specific details for renderers (owned, may be NULL) */
    bool is_error;
    bool useless;
} tool_result_t;

void tool_result_text(tool_result_t *r, const char *text);         /* appends a text block */
void tool_result_textf(tool_result_t *r, const char *fmt, ...) PRINTF_LIKE(2, 3);
void tool_result_image(tool_result_t *r, const char *b64, const char *mime);
void tool_result_error(tool_result_t *r, const char *fmt, ...) PRINTF_LIKE(2, 3);
void tool_result_free(tool_result_t *r);
char *tool_result_joined_text(const tool_result_t *r);

typedef struct tool_ctx {
    agent_t *agent;
    const char *tool_call_id;
    const char *tool_name;
    volatile sig_atomic_t *cancel; /* poll this */
    /* stream partial output (tool_execution_update) */
    void (*update)(struct tool_ctx *ctx, const tool_result_t *partial);
    void *app; /* application context (coding-agent session) */
    /* xd:// dispatch in progress: partial updates of the device are re-wrapped as {xdev: {tool, mode, args, inner}} */
    void (*xdev_outer_update)(struct tool_ctx *ctx, const tool_result_t *partial);
    const char *xdev_tool;
    const json_t *xdev_args;
    const char *intent; /* the call's `i` intent as the model wrote it (trimmed; may be NULL) */
} tool_ctx_t;

struct agent_tool {
    char *name;
    char *label;
    char *description;
    json_t *parameters; /* JSON schema object (owned) */
    bool exclusive;     /* runs alone when several calls arrive in one turn */
    /* omp `interruptible`: a pure wait that queued steering / a peer interrupt cuts short in either interruptMode;
     * a not-yet-started one is skipped unless `handles_tool_interrupt` (it then runs and drains the wake itself).
     * Every other tool always runs once emitted (agent-loop.ts runTool, #10439). */
    bool interruptible;
    bool handles_tool_interrupt;
    bool hidden;        /* not sent to the model */
    bool lenient_args;  /* skip schema validation errors */
    bool mounted;       /* presented as an xd://<name> device instead of top-level (implies hidden) */
    bool discoverable;  /* upstream loadMode "discoverable": mountable under xd:// */
    bool strict;        /* upstream `strict = true`: providers send the schema in strict mode */
    int intent_mode;    /* INTENT_* : how the `i` intent field is injected into the wire schema */
    char *summary;      /* one-line catalog summary (owned, may be NULL) */
    json_t *examples;   /* [{caption?, call|good+bad|note}] appended to the wire description (owned, may be NULL) */
    /* MCP ownership (omp MCPTool mcpServerName / mcpToolName) for the xd:// route guidance; owned, NULL otherwise */
    char *mcp_server, *mcp_tool;
    /* returns 0; errors are reported via out->is_error */
    int (*execute)(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out);
    void *ud;
    void (*destroy)(agent_tool_t *self);
    /* named doc topics served at xd://<tool>/<topic>: owned object topic -> markdown (NULL: none) */
    json_t *(*doc_topics)(agent_tool_t *self);
};

enum { INTENT_REQUIRE = 0, INTENT_OPTIONAL = 1, INTENT_OMIT = 2 };
#define INTENT_FIELD "i"
#define INTENT_MAX_LENGTH 200

agent_tool_t *agent_tool_new(const char *name, const char *label, const char *description, json_t *params,
                             int (*execute)(agent_tool_t *, tool_ctx_t *, const json_t *, tool_result_t *));
void agent_tool_free(agent_tool_t *t);
json_t *agent_tool_wire(const agent_tool_t *t); /* {name, description, parameters}; injects `i` when intent tracing is on */
/* examples as JSDoc `@example` lines (ai dialect renderToolExamplesJsdoc; /dump tool inventory); owned or NULL */
char *agent_tool_examples_jsdoc(const agent_tool_t *t);
/* intent tracing (upstream tools.intentTracing; PI_NO_INTENT=1 disables injection). Process-wide. */
void agent_set_intent_tracing(bool on);
bool agent_intent_tracing(void);
/* schema with the `i` intent field injected per mode (owned clone) */
json_t *agent_schema_inject_intent(const json_t *schema, int mode);
bool schema_declares_property(const json_t *schema, const char *name);

/* ---------- events ---------- */
typedef enum {
    AGENT_EV_AGENT_START,
    AGENT_EV_AGENT_END,
    AGENT_EV_TURN_START,
    AGENT_EV_TURN_END,
    AGENT_EV_MESSAGE_START,
    AGENT_EV_MESSAGE_UPDATE,
    AGENT_EV_MESSAGE_END,
    AGENT_EV_TOOL_START,
    AGENT_EV_TOOL_UPDATE,
    AGENT_EV_TOOL_END,
} agent_event_type_t;

typedef struct {
    agent_event_type_t type;
    const json_t *message;     /* message_* / turn_end */
    const ai_event_t *ai;      /* message_update */
    const char *tool_call_id;  /* tool_* */
    const char *tool_name;
    const json_t *args;
    const char *intent;          /* tool_start: extracted `i` intent (may be NULL) */
    const tool_result_t *result; /* tool_update (partial) / tool_end */
    bool is_error;
    const json_t *tool_results; /* turn_end: array of toolResult messages */
    const json_t *messages;     /* agent_end: messages added in this run; agent_start: resumedToolCallIds (ids) */
} agent_event_t;

const char *agent_event_name(agent_event_type_t t);
/* Serialize an event in upstream JSON shape (owned). */
json_t *agent_event_to_json(const agent_event_t *ev);

typedef void (*agent_listener_fn)(void *ud, const agent_event_t *ev);

/* ---------- hooks ---------- */
typedef enum { TOOL_ALLOW, TOOL_BLOCK } tool_gate_t;
enum { AGENT_STREAM_CONTINUE = 0, AGENT_STREAM_INTERRUPT = 1, AGENT_STREAM_ERROR = 2 };
typedef struct {
    /* Called before executing a tool call. Return TOOL_BLOCK and set *reason (owned) to refuse. */
    tool_gate_t (*before_tool)(void *ud, agent_t *a, const json_t *tool_call, char **reason);
    /* Called once the arguments validated, right before the tool runs (omp's approval gate inside the wrapped
     * `execute`): `args` are the normalized arguments. Return TOOL_BLOCK and set *reason (owned) to refuse. */
    tool_gate_t (*approve_tool)(void *ud, agent_t *a, const json_t *tool_call, const json_t *args, char **reason);
    /* Called after a tool finished; may modify the result. */
    void (*after_tool)(void *ud, agent_t *a, const json_t *tool_call, tool_result_t *res);
    /* Transform the message list right before an LLM call (compaction, injections). Return owned array or NULL. */
    json_t *(*transform_context)(void *ud, agent_t *a, const json_t *messages);
    /* Rewrite a finalized assistant message in place before it reaches the context, the UI or tool dispatch
     * (omp transformAssistantMessage). */
    void (*transform_assistant)(void *ud, agent_t *a, json_t *assistant);
    /* a provider request ended: the host's classification of a failed message, before its first event */
    void (*finalize_error)(void *ud, agent_t *a, json_t *assistant);
    /* Fill per-request stream options (api key refresh etc). */
    void (*prepare_request)(void *ud, agent_t *a, ai_stream_opts_t *opts);
    /* the request about to be streamed (model, final provider context and options), e.g. to arm the prompt-cache
     * warmer; borrowed for the call */
    void (*on_request)(void *ud, agent_t *a, const ai_model_t *model, const ai_context_t *ctx, const ai_stream_opts_t *opts);
    /* Inspect stream events (deltas, block starts, thinking ends, and AI_EV_DONE once the stream finished).
     * Return AGENT_STREAM_INTERRUPT to abort the stream and run should_retry(-1) (TTSR, Gemini header runaway), or
     * AGENT_STREAM_ERROR to end the turn as an empty retryable error carrying agent->stream_error (loop guard). */
    int (*on_stream_delta)(void *ud, agent_t *a, const ai_event_t *ev);
    /* After an assistant message ends with an error (attempt >= 0) or a deliberate stream interrupt (attempt -1):
     * AGENT_RETRY_NONE ends the run, AGENT_RETRY_REPLAY drops the failed turn and requests again,
     * AGENT_RETRY_PRESERVE / _CONTINUE keep the failed turn (and the synthetic results of its tool calls) in context
     * and continue; *append (owned, may stay NULL) is added after the preserved turn (stream-stall reminder). */
    int (*should_retry)(void *ud, agent_t *a, const json_t *assistant, int attempt, json_t **append);
    /* A tool-loop turn ended and the run continues with another model call (omp onTurnEnd willContinue): the
     * host may rewrite the context (mid-turn compaction); the next request snapshots the messages again. */
    void (*turn_boundary)(void *ud, agent_t *a, const json_t *assistant);
    /* The steering / follow-up queue changed (enqueue, delivery, clear). Called without agent locks held. */
    void (*queue_changed)(void *ud, agent_t *a);
    /* Before each model request of the loop (omp addBeforeModelCall): return true to end the run there, before
     * the turn opens (cost cap, --max-time deadline). */
    bool (*before_model_call)(void *ud, agent_t *a);
    /* The loop parked at the process pause gate (paused) or resumed from it (omp Agent.onPauseChange). Called on the
     * loop's thread without agent locks held. */
    void (*pause_change)(void *ud, agent_t *a, bool paused);
    void *ud;
} agent_hooks_t;

/* should_retry outcomes; AGENT_RETRY_CONTINUE preserves like AGENT_RETRY_PRESERVE without spending a retry
 * attempt (committed-text stream-stall continuation) */
enum { AGENT_RETRY_NONE = 0, AGENT_RETRY_REPLAY = 1, AGENT_RETRY_PRESERVE = 2, AGENT_RETRY_CONTINUE = 3 };

/* ---------- agent ---------- */
struct agent {
    pthread_mutex_t mu;
    const ai_model_t *model;
    ai_effort_t thinking;
    json_t *system_prompt; /* array of strings */
    json_t *messages;      /* full history (owned) */
    VEC(agent_tool_t *) tools;
    VEC(agent_tool_t *) all_tools; /* includes hidden tools */
    VEC(struct { agent_listener_fn fn; void *ud; }) listeners;
    agent_hooks_t hooks;
    ai_stream_opts_t base_opts;  /* template options (headers, session id, ...) */
    char *api_key;
    char *session_id;
    json_t *steering;   /* queued user messages delivered mid-run */
    json_t *follow_up;  /* queued messages delivered after the run would stop */
    json_t *asides;     /* quiet records (IRC broadcast legs, progress notes): read at the next step boundary, never interrupt */
    int64_t run_started_mono; /* mono_ms when the current or latest run started (AgentSession.runStartedAt) */
    /* omp steeringMode / followUpMode ("one-at-a-time" default: one queued message per turn; "all") and
     * interruptMode ("immediate" default: queued steering cuts interruptible waits short and raises the cooperative
     * steering signal for the other running tools; "wait": only interruptible waits are cut short) */
    bool steer_all, follow_up_all, interrupt_wait;
    /* the running tool batch's interrupt state (agent-loop.ts interruptState / steeringSoftController), under `mu`:
     * `batch_source` is the first interrupt's origin ("user", "agent", "system", "unknown" or "irc"; NULL: none yet) */
    const char *batch_source;
    bool batch_steered, batch_soft;
    volatile sig_atomic_t cancel;
    volatile sig_atomic_t silent_abort_next; /* abort the next model request as an internal transition */
    char *mcp_scope;                         /* this session's view of a shared MCP manager (NULL: the owner) */
    volatile sig_atomic_t mcp_tools_dirty;   /* an MCP call granted this session tools: re-register at the next boundary */
    volatile sig_atomic_t deliver_queued;    /* an empty Enter forced the queue into the continuation (see below) */
    volatile sig_atomic_t abort_user;        /* the abort carries USER_INTERRUPT_LABEL (agent_abort_user) */
    bool running;
    json_t *streaming_message; /* borrowed while streaming */
    int max_turns;             /* 0 => unlimited */
    int64_t tool_timeout_ms;
    void *app;                 /* application pointer passed to tools */
    pthread_mutex_t ev_mu;     /* serializes event delivery from tool threads */
    char *stream_error;        /* errorMessage for an AGENT_STREAM_ERROR abort (owned, set by the hook) */
    int pause_parked;          /* waits of this loop parked at the pause gate (guarded by the gate's lock) */
};

agent_t *agent_new(void);
void agent_free(agent_t *a);
void agent_set_model(agent_t *a, const ai_model_t *m);
void agent_set_system_prompt(agent_t *a, const char *prompt); /* single string; NULL clears */
void agent_set_system_prompt_parts(agent_t *a, json_t *parts); /* takes ownership (array) */
void agent_add_tool(agent_t *a, agent_tool_t *t);              /* takes ownership */
agent_tool_t *agent_find_tool(agent_t *a, const char *name);
void agent_remove_tool(agent_t *a, const char *name);
void agent_set_active_tools(agent_t *a, const strvec_t *names); /* NULL => all non-hidden */
void agent_subscribe(agent_t *a, agent_listener_fn fn, void *ud);
void agent_unsubscribe(agent_t *a, agent_listener_fn fn, void *ud);
void agent_replace_messages(agent_t *a, json_t *messages); /* takes ownership */
void agent_append_message(agent_t *a, json_t *msg);       /* takes ownership, no events */

/* Run the loop with new user-side messages (array, owned). Blocks until done. Returns 0, or -1 if busy. */
int agent_prompt(agent_t *a, json_t *new_messages);
/* Continue from current history without new messages (e.g., retry/resume). */
int agent_continue(agent_t *a);
void agent_abort(agent_t *a);
/* session.abort({reason: USER_INTERRUPT_LABEL}): the user stopped the run (Esc, a cancelled ask form, RPC abort). The
 * aborted assistant message reads "Interrupted by user" (errorId Flag.UserInterrupt) instead of the bare
 * "Request was aborted". */
void agent_abort_user(agent_t *a);
#define AGENT_USER_INTERRUPT_LABEL "Interrupted by user"
/* Abort the next model request of the running loop as an expected internal transition (omp
 * markPlanInternalAbortPending + abort): the aborted assistant message is stamped with
 * errorMessage "__omp.silent_abort__" so no abort line renders. */
void agent_abort_next_silently(agent_t *a);
void agent_steer(agent_t *a, json_t *msg);      /* owned */
/* For tools running in a batch (non-consuming peeks of the steering queue, latched until the batch ends):
 * agent_tool_steering_signal is omp's cooperative `ctx.steeringSignal` (queued steering or a peer interrupt, only when
 * interruptMode is not "wait": backgroundable work may step aside); agent_tool_interrupted is an interruptible tool's
 * wake (queued steering or a peer interrupt, in either interruptMode). */
bool agent_tool_steering_signal(agent_t *a);
bool agent_tool_interrupted(agent_t *a);
const char *agent_tool_interrupt_source(agent_t *a); /* the latched interrupt's origin, NULL: none (static string) */
/* agent-loop.ts createSkippedToolResult into `r` (replaced): source words the reason; `started`: it was running */
void agent_skipped_tool_result(tool_result_t *r, const char *source, bool started);
void agent_follow_up(agent_t *a, json_t *msg);  /* owned */
void agent_aside(agent_t *a, json_t *msg);      /* owned: joins the next step boundary without skipping any tool call */
/* Take the steering and aside records `is_record` selects, steering first. With `wakes`, takes nothing unless one
 * record satisfies it (IrcBridge.drainInboxMessages ifAny). Owned array, possibly empty. */
json_t *agent_take_records(agent_t *a, bool (*is_record)(const json_t *msg), bool (*wakes)(const json_t *msg));
void agent_clear_queues(agent_t *a);
void agent_queue_changed(agent_t *a); /* run hooks.queue_changed (after an external queue edit) */
bool agent_is_running(agent_t *a);
/* A queued message the user typed (role user, not synthetic): the ones queue editing touches. */
bool agent_msg_user_authored(const json_t *msg);
/* Display text of a queued message (string content or its first text block; "[Image]" for image-only). Owned. */
char *agent_queued_text(const json_t *msg);
/* Texts of the user-authored queued messages (omp getQueuedMessages). */
void agent_queue_texts(agent_t *a, strvec_t *steering, strvec_t *follow_up);
/* Remove and return the last user-authored queued message, steering first (omp popLastQueuedMessage). Owned/NULL. */
json_t *agent_pop_last_queued(agent_t *a);
/* Remove every user-authored queued message; returns {steering:[...], followUp:[...]} (omp clearQueue). Owned. */
json_t *agent_take_user_queued(agent_t *a);
size_t agent_queued_count(agent_t *a);
size_t agent_steering_count(agent_t *a);
/* omp agent.prioritizeSteering: move steering[from..] (messages queued after a snapshot) to the queue head */
void agent_prioritize_steering(agent_t *a, size_t from);
bool agent_steering_has(agent_t *a, const json_t *msg); /* `msg` (by identity) is still queued */
const json_t *agent_steering_at(agent_t *a, size_t i);  /* borrowed identity token; NULL past the end */
/* AgentSession.abort({deliverQueuedMessages}): abort the run and deliver the queue once it unwinds. The run's owner
 * calls agent_take_forced_delivery when the loop is idle: true when delivery was forced, with the queued user
 * follow-ups moved behind the steers so they enter the continuation instead of waiting for a later yield. */
void agent_abort_deliver_queued(agent_t *a);
bool agent_has_interruptible_input(agent_t *a); /* omp hasInterruptibleInput: queued input the band shows */
bool agent_take_forced_delivery(agent_t *a);

/* Start a run that delivers queued messages (steering first, per mode) when idle. -1 if busy or nothing queued. */
int agent_run_queued(agent_t *a);

/* Process-wide pause gate (pi-agent-core pause.ts agentPauseGate, the TUI `/pause`): every agent loop of the
 * process parks before its next model call and before starting each tool; nothing is aborted. A run's own
 * cancel releases its park without releasing the gate. */
bool agent_pause_gate_pause(void);          /* engage; false (no-op) when already paused */
int64_t agent_pause_gate_resume(void);      /* release; the hold in ms, -1 when not paused */
bool agent_pause_gate_paused(void);
int64_t agent_pause_gate_paused_at(void);   /* epoch ms of the current pause, 0 when running */
/* agent loops currently running and not parked at the gate (pauseWorkStatus unsettledTurns over every agent
 * in the process: main, subagents, advisor) */
int agent_unsettled_loops(void);

/* helpers to build messages (owned) */
json_t *msg_user_text(const char *text);
json_t *msg_user_content(json_t *content); /* takes ownership of content array */
json_t *msg_user_typed(json_t *content);   /* msg_user_content with attribution "user" (input the user typed) */
json_t *msg_developer_text(const char *text);
json_t *msg_tool_result(const char *id, const char *name, const tool_result_t *r);
json_t *text_block(const char *text);
json_t *image_block(const char *b64, const char *mime);
/* last assistant message in an array (borrowed) */
const json_t *last_assistant(const json_t *messages);
/* the call is the only tool call of the latest assistant message (wait.ts isOnlyCallOfTurn) */
bool agent_tool_call_alone(agent_t *a, const char *tool_call_id);

/* Tool-argument validation (src/agent/validate.c, port of omp validateToolArguments with omptype messages).
 * tool_args_validate: on success returns NULL and replaces *args with the normalized/coerced value; on failure
 * returns the issue lines ("  - path/0/key: key[0].x must be a string (was missing)") and, when received is
 * non-NULL, the received-arguments JSON (2-space; {original, normalized} when a repair changed them). */
char *tool_args_validate(const json_t *schema, json_t **args, char **received);
/* same, updating the object args in place on success */
char *schema_validate_args(const json_t *schema, json_t *args, char **received);
/* schema_validate_args without the received text */
char *schema_validate(const json_t *schema, json_t *args);
/* omptype `summary` of value's schema errors without any repair (NULL when valid) */
char *schema_summary(const json_t *schema, const json_t *value);

#endif
