/* approval.h - tool approval (port of omp coding-agent tools/approval.ts, approval-cache.ts, approval-targets.ts,
 * search-approval-targets.ts, the per-tool `approval` / `formatApprovalDetails` / `approvalCacheTargets`
 * declarations, bash approval patterns, and the extension wrapper's approval gate).
 *
 * Resolution order (resolveApproval): the tool's own decision (tier, optional policy/override/reason/policyKey),
 * the user's `tools.approval.<tool>` policy, the `tools.approvalMode` tier comparison (or the remembered grants
 * of `always-ask-cached`), then `tools.restrictedFolders`, which turns any non-deny result into a per-call prompt.
 *
 * Remembered grants: a session tier (process memory, keyed by session id) and a global tier. omp keeps the global
 * tier in SQLite (`approval-cache.db`); omc stores the same rows in `<agentDir>/approval-cache.json`
 * (flock-serialized, atomic rewrite). Grant keys pin the capability tier: "<tier>\x1f<subject>" (omp: "\0"). */
#ifndef OMC_APPROVAL_H
#define OMC_APPROVAL_H

#include "core/core.h"
#include "core/json.h"
#include "tools.h"

typedef enum { TIER_READ = 0, TIER_WRITE = 1, TIER_EXEC = 2 } approval_tier_t;
typedef enum { APOL_UNSET = 0, APOL_ALLOW, APOL_DENY, APOL_PROMPT } approval_policy_t;
typedef enum { AMODE_ALWAYS_ASK = 0, AMODE_ALWAYS_ASK_CACHED, AMODE_WRITE, AMODE_YOLO } approval_mode_t;
typedef enum { ASRC_NONE = 0, ASRC_TOOL, ASRC_USER, ASRC_MODE, ASRC_RESTRICTION } approval_source_t;

const char *approval_tier_name(approval_tier_t t);
const char *approval_mode_name(approval_mode_t m);
/* "always-ask" | "always-ask-cached" | "write" | "yolo" -> mode; -1 otherwise */
int approval_mode_parse(const char *s);

/* one tool's ToolApprovalDecision */
typedef struct {
    approval_tier_t tier;
    approval_policy_t policy; /* APOL_UNSET when the tool names none */
    bool override;
    char *reason;     /* owned or NULL */
    char *policy_key; /* owned or NULL: tools.approval.<key> consulted before tools.approval.<tool> */
} approval_decision_t;

typedef struct {
    approval_policy_t policy; /* never APOL_UNSET */
    approval_tier_t tier;
    bool override;
    approval_source_t source;
    char *reason;     /* owned or NULL */
    char *policy_key; /* owned or NULL */
} approval_resolved_t;

void approval_decision_free(approval_decision_t *d);
void approval_resolved_free(approval_resolved_t *r);

/* A tool call as approval sees it. `agent` resolves xd:// devices (may be NULL). */
typedef struct {
    tool_env_t *env;
    agent_t *agent;
    const char *tool;
    const json_t *args;
} approval_call_t;

/* the tool's own approval decision for these args (defaults to exec for tools that declare none) */
void approval_tool_decision(const approval_call_t *c, approval_decision_t *out);
/* formatApprovalDetails rows (blanks dropped) */
void approval_tool_details(const approval_call_t *c, strvec_t *out);
/* approvalCacheTargets keys (blanks dropped; empty when the tool declares none) */
void approval_cache_targets(const approval_call_t *c, strvec_t *out);

/* ---------- remembered grants ---------- */

/* grant key for one capability tier (owned) */
char *approval_grant_key(approval_tier_t tier, const char *subject);
/* which of `keys` the session tier of `session_id` (NULL: none) or the global file hold; marks present[i] */
void approval_grants_present(const char *session_id, const strvec_t *keys, bool *present);
/* record a grant: global (the shared file) or the session tier of `session_id` */
void approval_grants_record(const char *session_id, const char *key, const char *tool, bool global);
/* forget every remembered grant: the global file and every session tier (clearApprovalGrants) */
void approval_grants_clear(void);
/* "approve for this session" answers (ExtensionRunner #sessionApprovals): key = "<subject>\x1f<tier>" */
bool approval_session_has(const char *session_id, const char *key);
void approval_session_grant(const char *session_id, const char *key);

/* ---------- resolution ---------- */

typedef struct {
    approval_mode_t mode;
    const json_t *user_policies; /* tools.approval (borrowed object or NULL) */
    const char *session_id;      /* always-ask-cached grants (NULL: none remembered) */
    bool use_grants;             /* mode == always-ask-cached */
    const json_t *restricted;    /* tools.restrictedFolders (borrowed array or NULL) */
    const char *cwd;
} approval_ctx_t;

/* resolveApprovalFromContext: settings + --auto-approve (forces yolo) -> mode, policies, restrictions */
void approval_ctx_from_settings(approval_ctx_t *ctx, bool auto_approve, const char *session_id, const char *cwd);
void approval_resolve(const approval_call_t *c, const approval_ctx_t *ctx, approval_resolved_t *out);
/* restrictedFolderReason: "Restricted folder: <folder>" (owned) or NULL */
char *approval_restricted_reason(const approval_call_t *c, const json_t *restricted, const char *cwd);
/* sessionApprovalScope: the subject (tool or device) and key a session answer covers; false when per-call */
bool approval_session_scope(const approval_resolved_t *r, const char *tool, char **subject, char **key);
/* denyError text (owned) */
char *approval_deny_error(const approval_resolved_t *r, const char *tool);

/* ---------- prompt ---------- */

typedef struct {
    char *label, *always_label; /* owned */
    bool folder;                /* scope "folder" (else "target") */
} approval_grant_choice_t;

typedef struct approval_prompt {
    char *full;     /* header + details + folder block (owned) */
    char *compact;  /* header + details summary + compact folder block (owned) */
    char *session;  /* "Approve <subject> for this session" (owned) or NULL */
    approval_grant_choice_t grants[2];
    size_t ngrants;
    char *folder_key; /* the folder a directory answer would grant (owned) or NULL */
} approval_prompt_t;

/* requestApproval's prompt: `session_subject` offers the session answer, `grants` the always-ask-cached grants */
void approval_prompt_build(const approval_call_t *c, const char *reason, const char *session_subject, bool grants,
                           approval_prompt_t *out);
void approval_prompt_free(approval_prompt_t *p);
/* formatApprovalPrompt (the blocked run status / RPC select title): header + details (owned) */
char *approval_format_prompt(const approval_call_t *c, const char *reason);
/* recordApprovalGrant for a grant answer (folder or exact targets) */
void approval_record_grant(const approval_call_t *c, const char *session_id, approval_tier_t tier, bool folder,
                           bool always);
/* truncateForPrompt (2000 chars) */
char *approval_truncate(const char *s);
/* formatApprovalArguments: "Arguments:\n<layout>" row(s) appended (none for undefined/empty unless show_empty) */
void approval_format_arguments(const json_t *args, bool show_empty, strvec_t *out);

/* answer of an approval prompt */
typedef enum { APPROVE_DENY = 0, APPROVE_ONCE, APPROVE_SESSION, APPROVE_GRANT } approval_answer_kind_t;
typedef struct {
    approval_answer_kind_t kind;
    int grant;   /* APPROVE_GRANT: index into prompt grants */
    bool always; /* APPROVE_GRANT: remembered across sessions */
} approval_answer_t;

/* labels of approvalChoiceLabels (select hosts: RPC/ACP): grants, always-grants, once, session, Deny */
void approval_choice_labels(const approval_prompt_t *p, strvec_t *out);
approval_answer_t approval_answer_for_choice(const approval_prompt_t *p, const char *choice);

/* ---------- bash ---------- */

/* the shell tokenizer's command segments (tokenizeShellSegments), joined by single spaces */
void approval_shell_segments(const char *command, strvec_t *out);
/* CRITICAL_BASH_PATTERNS match */
bool approval_bash_critical(const char *command);

/* ---------- ACP permission gate (session/acp-permission-gate.ts) ---------- */

/* getPermissionIntent: title, cache key and paths of a gated call; false when the call needs no permission */
bool approval_acp_intent(tool_env_t *env, const char *tool, const json_t *args, char **title, char **cache_key,
                         strvec_t *paths);
/* PERMISSION_REQUIRED_TOOLS */
bool approval_acp_tool_gated(const char *tool);

#endif
