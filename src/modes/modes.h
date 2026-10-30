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
int run_interactive_mode(app_t *a, const char *initial_prompt);

/* shared: thread-safe line writer to stdout */
void out_json_line(const json_t *j);

#endif
