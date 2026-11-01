/* omc.h - public C API of libomc (oh-my-c), for embedding the coding agent in native apps.
 *
 * Build:   make lib            -> build/libomc.a, build/libomc.so (+ build/omc.pc)
 * Link:    cc app.c -Iinclude build/libomc.a -lssl -lcrypto -lz -lpthread -lm
 *          (or: pkg-config --cflags --libs omc)
 *
 * Only symbols declared here are exported; everything else is internal. All strings are UTF-8.
 * Strings returned to you are heap-allocated: release them with omc_free().
 *
 * Protocol: a session speaks the same JSON protocol as `omc --mode rpc` (omp-compatible). You send
 * command objects ({"type":"prompt","message":"..."}, {"type":"abort"}, {"type":"get_state"}, ...)
 * with omc_send(); responses and agent events (message_update, tool_execution_start, agent_end, ...)
 * arrive as one JSON object per callback. The callback runs on omc worker threads: keep it short
 * and do not call omc_session_free() from inside it.
 *
 * Process-wide state: configuration (~/.omc/agent or agent_dir), credentials and the model
 * catalog are shared by all sessions of a process; the first session's agent_dir wins.
 */
#ifndef OMC_H
#define OMC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define OMC_API
#else
#define OMC_API __attribute__((visibility("default")))
#endif

#define OMC_API_VERSION 1

/* library version string, e.g. "0.1.0" */
OMC_API const char *omc_version(void);
/* OMC_API_VERSION the library was built with (compare with the header you compiled against) */
OMC_API int omc_api_version(void);

/* Run the complete omc command line in-process (interactive TUI, print/json/rpc/acp modes,
 * subcommands) exactly as the `omc` executable does. Returns the exit status. */
OMC_API int omc_main(int argc, char **argv);

typedef struct omc_session omc_session_t;

typedef struct {
    size_t struct_size;               /* set to sizeof(omc_options_t) (ABI evolution) */
    const char *cwd;                  /* working directory for tools (NULL => process cwd) */
    const char *agent_dir;            /* config/credentials/sessions root (NULL => ~/.omc/agent) */
    const char *model;                /* "provider/id[:effort]", fuzzy pattern or @role (NULL => default) */
    const char *provider;             /* restrict model resolution to a provider */
    const char *api_key;              /* explicit API key for the chosen provider */
    const char *thinking;             /* off|minimal|low|medium|high|xhigh|max */
    const char *system_prompt;        /* replace the built-in system prompt */
    const char *append_system_prompt; /* append to the system prompt */
    const char *session_dir;          /* where session JSONL files are written */
    const char *resume;               /* session id/path to resume (NULL => new session) */
    bool no_session;                  /* keep the session in memory only */
    const char *const *tools;         /* NULL-terminated allow-list of built-in tools (NULL => defaults) */
    bool no_tools;                    /* disable all built-in tools */
    bool no_context_files, no_skills, no_rules, no_mcp;
    bool plan_mode;                   /* start in read-only plan mode */
} omc_options_t;

/* One JSON object (no trailing newline). Valid only for the duration of the call. */
typedef void (*omc_event_fn)(void *ud, const char *json);

/* Create a session. on_event may be NULL. On failure returns NULL and, if error is non-NULL,
 * stores a message to release with omc_free(). Emits {"type":"ready",...} first. */
OMC_API omc_session_t *omc_session_new(const omc_options_t *opts, omc_event_fn on_event, void *ud, char **error);
/* Abort any running turn, wait for it, persist, and release everything. */
OMC_API void omc_session_free(omc_session_t *s);
OMC_API const char *omc_session_id(omc_session_t *s); /* borrowed */

/* Send one RPC command (JSON object text). Asynchronous: the response arrives via on_event.
 * Returns 0 if the command was dispatched, -1 if it is not valid JSON. */
OMC_API int omc_send(omc_session_t *s, const char *command_json);

/* Convenience: run a prompt to completion on the calling thread. Events still reach on_event.
 * On success returns 0 and, if answer is non-NULL, the final assistant text (omc_free()).
 * Returns -1 if a turn is already running, 1 if the turn ended in error/abort. */
OMC_API int omc_prompt(omc_session_t *s, const char *text, char **answer);

/* Interrupt the running turn (also pauses an active goal, like Esc in the TUI). */
OMC_API void omc_abort(omc_session_t *s);
/* Wait until no turn is running. timeout_ms < 0 waits forever. Returns 0 when idle, 1 on timeout. */
OMC_API int omc_wait_idle(omc_session_t *s, int timeout_ms);
OMC_API bool omc_busy(omc_session_t *s);

/* Host-implemented tool. args_json is the model's arguments object. Return heap text for the
 * result (malloc; omc frees it) and set *is_error for failures. Called on a worker thread. */
typedef char *(*omc_tool_fn)(void *ud, const char *args_json, bool *is_error);
/* Register (or replace) a tool. parameters_json is a JSON Schema object (NULL => no params). */
OMC_API int omc_register_tool(omc_session_t *s, const char *name, const char *description, const char *parameters_json,
                              omc_tool_fn fn, void *ud);

OMC_API void omc_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* OMC_H */
