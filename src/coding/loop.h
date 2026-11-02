/* loop.h - `/loop` mode model (port of omp modes/loop-limit.ts, modes/loop-condition.ts and
 * status-line/loop.ts, as shipped in omp 18.14.2): argument parsing, the iteration/duration budget, the
 * `--while` / `--until` condition verdicts and the texts the TUI shows. The controller that re-submits the
 * prompt lives in modes/loopctl.c. Times are monotonic milliseconds. */
#ifndef OMC_LOOP_H
#define OMC_LOOP_H

#include "core/core.h"

typedef enum { LOOP_LIMIT_NONE = 0, LOOP_LIMIT_ITERATIONS, LOOP_LIMIT_DURATION } loop_limit_kind_t;

/* LoopLimitRuntime */
typedef struct {
    loop_limit_kind_t kind;
    int64_t initial, remaining;      /* iterations */
    int64_t duration_ms, deadline_ms; /* duration */
} loop_limit_t;

/* LoopConditionConfig: `--until` continues while the command fails, `--while` while it succeeds */
typedef struct {
    char *command;
    bool until;
} loop_cond_t;

/* ParsedLoopArgs */
typedef struct {
    loop_limit_kind_t limit_kind;
    int64_t amount; /* iterations, or the duration in ms */
    bool has_cond;
    loop_cond_t cond;
    char *prompt; /* NULL when none */
} loop_args_t;

/* parseLoopArgs: NULL on success, else the error message (owned) */
char *loop_parse_args(const char *args, loop_args_t *out);
void loop_args_free(loop_args_t *a);

/* the interactive loop state (InteractiveMode loopMode*) */
typedef struct loop_state {
    bool enabled, paused;
    char *prompt;
    loop_limit_t limit;
    bool has_cond;
    loop_cond_t cond;
} loop_state_t;

void loop_limit_init(loop_limit_t *l, loop_limit_kind_t kind, int64_t amount, int64_t now); /* createLoopLimitRuntime */
bool loop_limit_consume(loop_limit_t *l, int64_t now);         /* consumeLoopLimitIteration */
bool loop_limit_duration_expired(const loop_limit_t *l, int64_t now); /* isLoopDurationExpired */
bool loop_limit_exhausted(const loop_limit_t *l, int64_t now); /* isLoopLimitExhausted */

/* owned texts */
char *loop_describe_limit(loop_limit_kind_t kind, int64_t amount); /* describeLoopLimit: "10 iterations", "10 minutes" */
char *loop_describe_limit_runtime(const loop_limit_t *l);          /* "3 of 10 iterations remaining", "10 minutes limit" */
char *loop_describe_condition(const loop_cond_t *c);               /* "until `bun test` succeeds" */
char *loop_summarize_condition(const loop_cond_t *c, int max_width); /* status line: "until: bun test" */
char *loop_format_limit(const loop_limit_t *l, int64_t now);       /* status line: "4/10", "9m59s left"; NULL when none */
/* the mode segment's text without icon ("Loop: next prompt repeats", "Loop running 4/10 until: x", "Loop paused") */
char *loop_status_label(const loop_state_t *s);
char *loop_autocomplete_description(const loop_state_t *s); /* /loop's live description ("Loop: off", ...) */
/* the "Loop mode enabled. …" notice; `interrupt_key` is the app.interrupt hint */
char *loop_enabled_message(const loop_args_t *a, const loop_limit_t *rt, const char *interrupt_key);

typedef enum { LOOP_VERDICT_CONTINUE, LOOP_VERDICT_HALT, LOOP_VERDICT_ERROR, LOOP_VERDICT_ABORTED } loop_verdict_t;
/* evaluateLoopCondition's mapping of one run: exit_code < 0 means no exit status; `spawn_error` set when the
 * command could not start. *message (owned) for HALT / ERROR. */
loop_verdict_t loop_condition_verdict(const loop_cond_t *c, int exit_code, bool timed_out, bool cancelled,
                                      const char *spawn_error, const char *output, int64_t timeout_ms, char **message);

#endif
