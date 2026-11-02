/* commands.h - non-builtin slash commands: file commands (`commands/<name>.md`, port of omp
 * capability/slash-command.ts + extensibility/slash-commands.ts + the native/Claude/Codex/agents/OpenCode/
 * Claude-plugin providers), prompt templates (config/prompt-templates.ts), `/skill:<name>` invocations
 * (extensibility/skills.ts parseSkillInvocation / renderSkillPromptMessage, tui prompt/skill-tokens.ts) and
 * the shared argument substitution (utils/command-args.ts). */
#ifndef OMC_COMMANDS_H
#define OMC_COMMANDS_H

#include "coding/discovery.h"
#include "core/core.h"
#include "core/json.h"

typedef struct {
    char *name;
    char *description;   /* frontmatter description, else the body's first non-empty line (60 chars + "...") */
    char *argument_hint; /* `argument-hint` / `argumentHint`, or NULL */
    char *content;       /* template body (frontmatter stripped) */
    char *source;        /* "via <provider> <Level>" or "bundled" */
    char *path;          /* source file, NULL for bundled commands */
    bool side;           /* `side: true`: the TUI answers it as a /btw side question */
} file_cmd_t;

typedef VEC(file_cmd_t) file_cmds_t;

typedef struct {
    char *name;
    char *description; /* with the "(user)" / "(project:sub)" source suffix */
    char *content;
} prompt_tpl_t;

typedef VEC(prompt_tpl_t) prompt_tpls_t;

/* loadSlashCommands: every provider in priority order (first name wins), then the bundled /init */
void commands_load(const char *cwd, file_cmds_t *out);
void commands_free(file_cmds_t *c);
const file_cmd_t *commands_find(const file_cmds_t *c, const char *name);

/* loadPromptTemplates: <agentDir>/prompts then <cwd>/.omc|.omp/prompts (recursive <name>.md) */
void prompt_templates_load(const char *cwd, prompt_tpls_t *out);
void prompt_templates_free(prompt_tpls_t *t);

/* parseCommandArgs (non-strict): whitespace split with '...' / "..." quoting */
strvec_t commands_parse_args(const char *s);
/* substituteArgs: $1.., $@, $ARGUMENTS, $@[start:len] */
char *commands_substitute_args(const char *content, const strvec_t *args);
/* renderFileSlashCommand / expandPromptTemplate body: substitute, render the template with
 * {args, ARGUMENTS, arguments}, append the args when the template has no inline placeholder (owned) */
char *commands_render(const char *content, const char *args_string);
/* parseFileSlashInvocation: "/name args" -> name (whole first space-delimited token) + raw args */
bool commands_parse_invocation(const char *text, char **name, char **args);
/* expandSlashCommand: owned expansion, or NULL when `text` names no file command */
char *commands_expand(const file_cmds_t *c, const char *text);
/* expandPromptTemplate: owned expansion, or NULL when `text` names no template */
char *prompt_templates_expand(const prompt_tpls_t *t, const char *text);

/* ---------- /skill:<name> ---------- */

typedef struct {
    char *name;   /* bare skill name */
    char *args;   /* everything outside the token */
    char *prompt; /* the draft as submitted (trimmed) */
} skill_invocation_t;

/* allowsSkillTokens: tokens are invocations (leading `/skill:`, or no other slash command / `!` / `$ ` sigil) */
bool skill_tokens_allowed(const char *text);
/* parseSkillInvocation: leading `/skill:<name> args` or a mid-prompt `/skill:<name>` token */
bool skill_parse_invocation(const char *text, skill_invocation_t *out);
void skill_invocation_free(skill_invocation_t *inv);
/* the first `/skill:<name>` token (SKILL_TOKEN_RE) in `text` at or after byte `from`: start..end span the token
 * (without the leading delimiter), *name owned; false when none */
bool skill_token_next(const char *text, size_t from, size_t *start, size_t *end, char **name);
/* renderSkillPromptMessage(..., "user"): the user-invocation message (owned) and its details object
 * {name, path, args?, prompt?, lineCount} (owned); -1 with *err when the skill file cannot be read */
int skill_build_user_prompt(const skill_t *s, const skill_invocation_t *inv, char **message, json_t **details, char **err);

#endif
