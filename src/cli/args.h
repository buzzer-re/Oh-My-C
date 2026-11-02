/* args.h - launch argument parsing (port of omp cli/args.ts parseArgs + cli/flag-tables.ts) and the top-level
 * argv routing of cli-commands.ts resolveCliArgv. */
#ifndef OMC_CLI_ARGS_H
#define OMC_CLI_ARGS_H

#include "core/core.h"

typedef struct {
    const char *cwd;
    strvec_t add_dir;
    const char *provider, *model, *smol, *slow, *plan;
    strvec_t config;
    const char *goal;
    bool plan_yolo;
    const char *plan_yolo_into;
    bool has_max_time, has_warn_cost, has_max_cost;
    double max_time, warn_cost, max_cost;
    const char *api_key, *system_prompt, *system_prompt_template, *append_system_prompt;
    const char *thinking;
    bool hide_thinking, advisor;
    bool continue_recent;
    bool resume_set;        /* --resume / -r / --session / --restore given */
    const char *resume;     /* NULL with resume_set = open the picker */
    bool restore;
    bool help, version;
    const char *mode;       /* text|json|rpc|acp|rpc-ui or NULL */
    bool no_session;
    const char *session_dir;
    const char *fork;
    bool has_models;
    strvec_t models;
    bool has_tools;
    strvec_t tools;
    bool no_tools, no_lsp;
    bool print, print_thoughts;
    const char *export_path;
    bool no_skills;
    bool has_skills;
    strvec_t skills;
    bool no_rules, no_context_files, no_title;
    bool auto_approve;
    const char *approval_mode;
    const char *service_tier; /* --service-tier (an OpenAI tier setting value) */
    /* omc extras */
    bool plan_mode;
    int max_turns;
    strvec_t messages;
    strvec_t file_args;
    strvec_t unrecognized;
    strvec_t invalid_values;
} launch_args_t;

/* Parse launch argv (without the program name / `launch` token). Returns 0, or 2 after reporting a CliUsageError
 * (`Error: <msg>` + the `--help` hint) on stderr. */
int launch_parse_args(int argc, char **argv, launch_args_t *out);
void launch_args_free(launch_args_t *a);

/* report helpers (reportUnrecognizedFlags / reportInvalidFlagValues / reportCliUsageError): true when reported */
bool launch_report_unrecognized(const launch_args_t *a);
bool launch_report_invalid(const launch_args_t *a);
void launch_report_usage_error(const char *msg);

/* resolveCliArgv: rewrites argv into [command, args...]. Returns the new argv (owned strvec) or NULL with *err set
 * (owned message, printed as `error: <msg>` with exit 1). */
bool resolve_cli_argv(int argc, char **argv, strvec_t *out, char **err);

#endif
