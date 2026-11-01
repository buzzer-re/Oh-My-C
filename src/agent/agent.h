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
} tool_ctx_t;

struct agent_tool {
    char *name;
    char *label;
    char *description;
    json_t *parameters; /* JSON schema object (owned) */
    bool exclusive;     /* runs alone when several calls arrive in one turn */
    bool hidden;        /* not sent to the model */
    bool lenient_args;  /* skip schema validation errors */
    bool mounted;       /* presented as an xd://<name> device instead of top-level (implies hidden) */
    bool discoverable;  /* upstream loadMode "discoverable": mountable under xd:// */
    int intent_mode;    /* INTENT_* : how the `i` intent field is injected into the wire schema */
    char *summary;      /* one-line catalog summary (owned, may be NULL) */
    json_t *examples;   /* [{caption?, call|good+bad|note}] appended to the wire description (owned, may be NULL) */
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
    const json_t *messages;     /* agent_end: messages added in this run */
} agent_event_t;

const char *agent_event_name(agent_event_type_t t);
/* Serialize an event in upstream JSON shape (owned). */
json_t *agent_event_to_json(const agent_event_t *ev);

typedef void (*agent_listener_fn)(void *ud, const agent_event_t *ev);

/* ---------- hooks ---------- */
typedef enum { TOOL_ALLOW, TOOL_BLOCK } tool_gate_t;
typedef struct {
    /* Called before executing a tool call. Return TOOL_BLOCK and set *reason (owned) to refuse. */
    tool_gate_t (*before_tool)(void *ud, agent_t *a, const json_t *tool_call, char **reason);
    /* Called after a tool finished; may modify the result. */
    void (*after_tool)(void *ud, agent_t *a, const json_t *tool_call, tool_result_t *res);
    /* Transform the message list right before an LLM call (compaction, injections). Return owned array or NULL. */
    json_t *(*transform_context)(void *ud, agent_t *a, const json_t *messages);
    /* Fill per-request stream options (api key refresh etc). */
    void (*prepare_request)(void *ud, agent_t *a, ai_stream_opts_t *opts);
    /* Inspect streaming text; return non-zero to abort the stream (TTSR). */
    int (*on_stream_delta)(void *ud, agent_t *a, const ai_event_t *ev);
    /* After an assistant message ends with an error; return true to retry the turn. */
    bool (*should_retry)(void *ud, agent_t *a, const json_t *assistant, int attempt);
    void *ud;
} agent_hooks_t;

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
    volatile sig_atomic_t cancel;
    bool running;
    json_t *streaming_message; /* borrowed while streaming */
    int max_turns;             /* 0 => unlimited */
    int64_t tool_timeout_ms;
    void *app;                 /* application pointer passed to tools */
    pthread_mutex_t ev_mu;     /* serializes event delivery from tool threads */
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
void agent_steer(agent_t *a, json_t *msg);      /* owned */
void agent_follow_up(agent_t *a, json_t *msg);  /* owned */
void agent_clear_queues(agent_t *a);
bool agent_is_running(agent_t *a);

/* helpers to build messages (owned) */
json_t *msg_user_text(const char *text);
json_t *msg_user_content(json_t *content); /* takes ownership of content array */
json_t *msg_developer_text(const char *text);
json_t *msg_tool_result(const char *id, const char *name, const tool_result_t *r);
json_t *text_block(const char *text);
json_t *image_block(const char *b64, const char *mime);
/* last assistant message in an array (borrowed) */
const json_t *last_assistant(const json_t *messages);

/* JSON schema validation + light coercion (string->number/bool, number->string). Returns NULL if ok,
 * else owned error text. May modify args in place for coercion. */
char *schema_validate(const json_t *schema, json_t *args);

#endif
