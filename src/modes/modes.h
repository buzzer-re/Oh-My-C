/* modes.h - run modes: print (text/json), rpc, interactive (TUI) */
#ifndef OMC_MODES_H
#define OMC_MODES_H

#include "coding/app.h"

typedef struct {
    strvec_t messages;     /* prompts to run in order */
    bool json;             /* --mode json */
    bool print_thoughts;
    bool quiet;
    const json_t *initial_images; /* @image attachments of messages[0] (borrowed, may be NULL) */
} print_opts_t;

int run_print_mode(app_t *a, const print_opts_t *o);
int run_rpc_mode(app_t *a);

/* embeddable RPC endpoint (same protocol as --mode rpc): responses/events go to sink (NULL => stdout) */
typedef void (*rpc_sink_fn)(void *ud, const json_t *msg);
typedef struct rpc rpc_t;
rpc_t *rpc_attach(app_t *a, rpc_sink_fn sink, void *ud); /* emits "ready" */
void rpc_handle_json(rpc_t *r, json_t *cmd);              /* borrowed cmd */
void rpc_handle_line(rpc_t *r, const char *line);
bool rpc_busy(rpc_t *r);
void rpc_detach(rpc_t *r); /* aborts a running turn, joins, unsubscribes */
int run_interactive_mode(app_t *a, const char *initial_prompt);
/* the launch command's startup inputs (omp runInteractiveMode initialMessage/initialImages/initialMessages) */
typedef struct {
    const char *initial;      /* first prompt (@file text + first message, or piped input), may be NULL */
    json_t *images;           /* its @image attachments (owned; consumed), may be NULL */
    const strvec_t *more;     /* further positional messages, each its own prompt after the first */
    bool resume_picker;       /* `--resume` without a value: open the session picker first */
    bool restore;             /* `--restore`: go on with the resumed session's interrupted turn */
    const char *notice;       /* startup info notice (`Model scope: …`), may be NULL */
} interactive_start_t;
int run_interactive_mode_ex(app_t *a, const interactive_start_t *st);
/* /restart closed the TUI: true once, with *resume = the session file to resume (owned, NULL when the session
 * never reached disk) */
bool interactive_restart_requested(char **resume);
int run_acp_mode(const app_opts_t *base); /* one app per ACP session */

/* shared: thread-safe line writer to stdout */
void out_json_line(const json_t *j);

#endif
