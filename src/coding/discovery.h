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
    bool managed; /* auto-learn managed skill (<agentDir>/managed-skills), lowest priority */
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
    bool has_condition;     /* any non-empty `condition` (string or list; omc's TTSR runs string regexes only) */
    bool has_ast_condition; /* `astCondition` (AST TTSR trigger; omc does not evaluate these) */
    bool has_question;      /* `question` (judged TTSR trigger) */
} rule_t;

typedef struct {
    VEC(ctx_file_t) files;
    VEC(skill_t) skills;
    VEC(rule_t) rules;
} discovery_t;

void discover_all(const char *cwd, discovery_t *out, bool no_context, bool no_skills, bool no_rules);
void discovery_free(discovery_t *d);
const skill_t *discovery_find_skill(const discovery_t *d, const char *name);
void discovery_free_skills(discovery_t *d);
/* replace dst's skills with a deep copy of src's (the system prompt's skill snapshot) */
void discovery_copy_skills(discovery_t *dst, const discovery_t *src);
/* both lists hold the same skills by name and SKILL.md path (session-tools.ts #adoptNewSkills) */
bool discovery_same_skills(const discovery_t *a, const discovery_t *b);
/* add a skill unless one with the same name exists (first wins); copies all strings */
bool discovery_add_skill(discovery_t *d, const char *name, const char *description, const char *path, const char *dir,
                         bool hide);
const rule_t *discovery_find_rule(const discovery_t *d, const char *name);

/* parse "---\nkey: value\n---\nbody" frontmatter; returns owned object (empty if none), *body points into text */
json_t *frontmatter_parse(const char *text, const char **body);

char *find_repo_root(const char *cwd); /* nearest dir with .git, or NULL */
/* getProjectPathCandidates' walk: cwd up to the repo root (else home, else /), home itself skipped (owned) */
strvec_t discovery_project_ancestors(const char *cwd);

typedef struct {
    const char *cwd;
    const char *model_name;           /* "provider/id" for <workstation> */
    const strvec_t *tool_names;       /* active tools */
    const discovery_t *disc;
    const char *custom_prompt;        /* --system-prompt */
    const char *custom_template;      /* --system-prompt-template (template text) */
    const char *append_prompt;        /* --append-system-prompt */
    bool subagent;
    /* optional: routing hint shown for a skill (compressed description catalog); owned result or NULL */
    char *(*skill_desc)(void *ud, const char *name, const char *description);
    void *skill_desc_ud;
    /* tool presentation (all optional) */
    const json_t *tool_labels;        /* object: tool name -> label for the Tool Inventory */
    const json_t *xdev_tools;         /* array of {name, summary, dynamic} mounted xd:// devices */
    const char *xdev_docs;            /* rendered xd:// device docs/catalog section */
    bool auto_qa;                     /* xd://report_issue automated QA advertised */
    const json_t *tool_refs;          /* object: tool name -> name used in prompt references (e.g. edit -> apply_patch) */
    const char *delegation_bias;      /* model_delegation_bias() of the session model */
    bool irc_enabled;                 /* `write agent://<id>` peer messaging available */
    const char *generated_append;     /* memory + auto-learn blocks composed ahead of the user append */
    bool tui_transcript;              /* top-level interactive TUI session: out:// replies and cfg:// writes reach the user */
    const strvec_t *additional_roots; /* multi-root workspace: extra roots beyond cwd (<workspace-roots>, their context files) */
} sysprompt_opts_t;

/* Whether a rule renders in the system prompt (<domain-rules> / <generic-rules>). Rules with a TTSR
 * trigger (regex `condition`, `astCondition`, `question`) are stream rules while `ttsr.enabled`;
 * `ttsr.disabledRules` drops a rule entirely (omp capability/rule-buckets.ts bucketRules). */
bool discovery_rule_listed(const rule_t *r);

/* Session facts the internal-URL listing depends on (omp SchemeHost + handler availability). */
typedef struct {
    bool skill_uri_access; /* a skill-reading tool is active and skills exist */
    size_t rule_count;     /* rulebook rules listed in the prompt */
    bool memory_local;     /* memory.backend=local owns the file-backed memory://root namespace */
    bool out_replies;      /* omp outRepliesActive: `out://` writes reach the user */
    bool settings_approval; /* omp settingsApproval: `cfg://` writes can be approved (top-level TUI session) */
    bool subagent;         /* omp SchemeHost.subagent: `agent://all` broadcasts are refused, so they go unadvertised */
} sysprompt_url_host_t;

/* "- " lines for the "# Internal URLs" section in omp registration order (owned) */
strvec_t internal_url_docs(const sysprompt_url_host_t *h);

/* returns owned array of system prompt blocks (strings) */
json_t *build_system_prompt(const sysprompt_opts_t *o);

#endif
