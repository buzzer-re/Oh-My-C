/* discovery.h - context files (AGENTS.md, CLAUDE.md, ...), skills, rules; system prompt assembly. */
#ifndef OMC_DISCOVERY_H
#define OMC_DISCOVERY_H

#include "core/core.h"
#include "core/json.h"

typedef struct {
    char *path;
    char *content;
    int depth; /* 0 = user, higher = closer to cwd */
} ctx_file_t;

typedef struct {
    char *name;
    char *description;
    char *path; /* SKILL.md */
    char *dir;
    bool hide;
} skill_t;

typedef struct {
    char *name;
    char *description;
    char *path;
    char *content;
    strvec_t globs;
    bool always;
    char *ttsr;   /* stream-trigger regex `condition` (time-traveling rules), optional */
    char *scope;  /* TTSR scope: "text", "thinking", "tool", "tool:edit(*.rs)", comma-separated */
    char *interrupt_mode; /* always | prose-only | tool-only | never (NULL = setting) */
    bool builtin; /* bundled builtin-defaults rule */
} rule_t;

typedef struct {
    VEC(ctx_file_t) files;
    VEC(skill_t) skills;
    VEC(rule_t) rules;
} discovery_t;

void discover_all(const char *cwd, discovery_t *out, bool no_context, bool no_skills, bool no_rules);
void discovery_free(discovery_t *d);
const skill_t *discovery_find_skill(const discovery_t *d, const char *name);
const rule_t *discovery_find_rule(const discovery_t *d, const char *name);

/* parse "---\nkey: value\n---\nbody" frontmatter; returns owned object (empty if none), *body points into text */
json_t *frontmatter_parse(const char *text, const char **body);

char *find_repo_root(const char *cwd); /* nearest dir with .git, or NULL */

typedef struct {
    const char *cwd;
    const char *model_name;           /* "provider/id" for <workstation> */
    const strvec_t *tool_names;       /* active tools */
    const discovery_t *disc;
    const char *custom_prompt;        /* --system-prompt */
    const char *append_prompt;        /* --append-system-prompt */
    bool subagent;
    /* optional: routing hint shown for a skill (compressed description catalog); owned result or NULL */
    char *(*skill_desc)(void *ud, const char *name, const char *description);
    void *skill_desc_ud;
} sysprompt_opts_t;

/* returns owned array of system prompt blocks (strings) */
json_t *build_system_prompt(const sysprompt_opts_t *o);

#endif
