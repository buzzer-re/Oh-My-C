/* slashcmd.h - the builtin slash-command registry shared by the TUI (autocomplete + dispatch), RPC and
 * ACP (port of omp's slash-commands/builtin-registry.ts, acp-builtins.ts, available-commands.ts).
 *
 * Metadata (registry order, aliases, icons, descriptions, ACP copy, inline hints, subcommands, allowArgs)
 * comes from assets/tui/slash-commands.json (build-tools/extract_slash_commands.py). Only commands omc
 * implements are registered, and only the subcommands omc implements are offered; omc-only commands
 * (/migrate) follow the upstream ones. The TUI handlers live in modes/interactive.c (`handleTui`); the
 * text-mode handlers (`handle`, used by RPC and ACP) live here. */
#ifndef OMC_SLASHCMD_H
#define OMC_SLASHCMD_H

#include "coding/app.h"

typedef struct {
    const char *name, *description, *usage; /* usage may be NULL */
} slash_sub_t;

typedef struct slash_cmd slash_cmd_t;

/* parsed "/name args": the separator is the earliest whitespace or ':'; args are trimmed */
typedef struct {
    char *name;
    char *args;
    const char *text;
} slash_parsed_t;

/* result of a text-mode (RPC/ACP) dispatch */
typedef enum { SLASH_NONE = 0, SLASH_CONSUMED, SLASH_PROMPT } slash_kind_t;
typedef struct {
    slash_kind_t kind;
    bool failed;        /* consumed but did not accomplish what was asked (usage error, refused) */
    bool agent_invoked; /* a real agent turn ran (e.g. /retry) */
    char *prompt;       /* SLASH_PROMPT: text to send as the prompt (owned) */
} slash_result_t;

typedef void (*slash_output_fn)(void *ud, const char *text);

typedef struct {
    app_t *app;
    slash_output_fn output;
    void *ud;
} slash_rt_t;

typedef void (*slash_handle_fn)(const slash_parsed_t *cmd, slash_rt_t *rt, slash_result_t *res);

struct slash_cmd {
    const char *name;           /* canonical name */
    strvec_t aliases;
    const char *icon;           /* SlashCommandIconName, NULL = default */
    const char *description;    /* may hold `{key:<id>}` placeholders (formatKeyHint), see slash_format_description */
    const char *acp_description;
    const char *inline_hint;
    const char *acp_input_hint;
    bool allow_args;
    bool sub_optional;
    slash_sub_t *subs;          /* the subcommands omc implements, upstream order */
    size_t nsubs;
    bool tui;                   /* has a TUI handler in modes/interactive.c */
    slash_handle_fn handle;     /* text-mode handler (RPC/ACP), NULL = TUI only */
};

/* registry in upstream order (omc-implemented commands only) */
size_t slash_count(void);
const slash_cmd_t *slash_at(size_t i);
const slash_cmd_t *slash_lookup(const char *name_or_alias);

/* parse "/name args"; false when `text` is not a slash invocation */
bool slash_parse(const char *text, slash_parsed_t *out);
void slash_parsed_free(slash_parsed_t *p);
/* executeBuiltinSlashCommand's match: a builtin by name/alias that accepts the given args (a command
 * without allowArgs never matches when args are present: the text goes to the model as a prompt) */
const slash_cmd_t *slash_match(const char *text, slash_parsed_t *out);

/* description with `{key:<id>}` placeholders replaced via `key_fmt` (NULL: the raw key id) (owned) */
char *slash_format_description(const char *description, char *(*key_fmt)(const char *key));

/* executeAcpBuiltinSlashCommand: run a builtin's text-mode handler. res->kind == SLASH_NONE when no
 * builtin with a text handler matched (the caller sends the text as a prompt). */
void slash_execute_text(app_t *a, const char *text, slash_output_fn output, void *ud, slash_result_t *res);
void slash_result_free(slash_result_t *res);
/* resolveSessionModelSelector (/switch, /prewalk): fuzzy ids, provider/id, @role, `:level`; the explicit level
 * selector in *level (owned or NULL) */
const ai_model_t *slash_resolve_session_model(const char *selector, char **level);
/* whether running builtin `name` now starts an agent turn (/retry after a failed turn): RPC's agentInvoked */
bool slash_invokes_agent(app_t *a, const char *name);

/* buildAvailableSlashCommands (RPC `get_available_commands` / `available_commands_update`):
 * [{name, aliases?, description, input?:{hint}, subcommands?, source}] for builtins with a text handler
 * ("builtin"), then `skill:<name>` ("skill", skills.enableSkillCommands), MCP prompts ("mcp_prompt") and
 * file commands ("file"); the first name wins and builtin aliases are reserved (owned) */
json_t *slash_available_commands(app_t *a);
/* toAcpAvailableCommands: [{name, description, input?}] (owned) */
json_t *slash_acp_available_commands(app_t *a);

/* slashmcp.c: /mcp and /ssh text-mode handlers (helpers/mcp.ts handleMcpAcp, helpers/ssh.ts handleSshAcp) */
void slash_mcp_text(const slash_parsed_t *cmd, slash_rt_t *rt, slash_result_t *res);
void slash_ssh_text(const slash_parsed_t *cmd, slash_rt_t *rt, slash_result_t *res);

/* ---- shared argument parsing / formatting (used by the TUI handlers too) ---- */

/* parseSubcommand: split "<verb> <rest>" on the first whitespace, verb lowercased (both owned) */
void slash_subcommand(const char *args, char **verb, char **rest);

/* compact-modes.ts parseCompactArgs: mode NULL/"soft"/"remote"/"snapcompact" (static), instructions owned
 * or NULL; returns false with *err (owned) for a rejected combination */
bool slash_parse_compact_args(const char *args, const char **mode, char **instructions, char **err);

/* /shake mode: "elide" (empty), "images", "thinking"; NULL with *err (owned) otherwise */
const char *slash_parse_shake_mode(const char *args, char **err);

/* /effort: the selectors the active model accepts ("off" + supported levels), in order */
void slash_effort_selectors(app_t *a, strvec_t *out);
/* resolveThinkingArgument: 0 and *level (selector name, owned; apply with app_set_thinking_level), or -1
 * with *err (owned) */
int slash_resolve_effort(app_t *a, const char *args, char **level, char **err);
const char *slash_configured_thinking(app_t *a); /* configuredThinkingLevel() ?? "model default" */
char *slash_no_thinking_message(app_t *a); /* "<p>/<id> has no adjustable thinking level." (owned) */

/* advisor: toggle/on/off; returns the TUI/text status line (static) */
const char *slash_advisor_set(app_t *a, int mode /* -1 toggle, 0 off, 1 on */);
char *slash_advisor_status_text(app_t *a); /* formatAdvisorStatus (owned) */
char *slash_locale_int(int64_t v);          /* toLocaleString(): en-US digit grouping (owned) */

#endif
