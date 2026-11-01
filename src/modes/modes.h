/* modes.h - run modes: print (text/json), rpc, interactive (TUI) */
#ifndef OMC_MODES_H
#define OMC_MODES_H

#include "coding/app.h"

typedef struct {
    strvec_t messages;     /* prompts to run in order */
    bool json;             /* --mode json */
    bool print_thoughts;
    bool quiet;
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
int run_acp_mode(const app_opts_t *base); /* one app per ACP session */

/* shared: thread-safe line writer to stdout */
void out_json_line(const json_t *j);

#endif
