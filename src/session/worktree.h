/* worktree.h - `/wt` session worktrees (port of omp session/session-worktree.ts) and the agent-managed worktree
 * base shared with `omc worktree` (cli/sub_worktree.c) and the github tool's PR checkouts (tools/gh_tool.c).
 *
 * omc has no clone-first backends (`worktree.clone`, `isolation.backend`): a worktree is always a plain
 * `git worktree add` checkout (its hooks held back: like omp's own checkout, `/wt` runs `post-checkout` afterwards with
 * session_wt_run_hook), and carrying uncommitted changes copies the source's dirty, staged and untracked files over and
 * installs the source index (pi-vcs seed_worktree_changes). */
#ifndef OMC_SESSION_WORKTREE_H
#define OMC_SESSION_WORKTREE_H

#include "core/core.h"

/* getWorktreesDir: $OMC_WORKTREE_DIR / $OMP_WORKTREE_DIR (absolute or ~), then `worktree.base`, else ~/.omc/wt
 * (owned) */
char *wt_base_dir(void);
/* hashPath: Bun.hash(path.resolve(p)) as 16 hex digits, last 7 (owned) */
char *wt_hash_path(const char *abs_path);

typedef struct {
    char *path;        /* absolute, realpath'd worktree root */
    char *branch;
    char *source_cwd;  /* checkout the worktree was forked from */
    char *base_commit; /* source HEAD the branch was created at */
    bool kept_changes; /* uncommitted source changes were carried over */
} session_wt_t;

void session_wt_free(session_wt_t *w);

/* defaultSessionWorktreeBranch: wt/<yyyymmdd-hhmmss> (local time) (owned) */
char *session_wt_default_branch(void);
/* formatSessionWorktreeSummary (owned) */
char *session_wt_summary(const session_wt_t *w, bool source_cleaned);
/* createSessionWorktree: 0, or -1 with *err (owned, user-facing) */
int session_wt_create(const char *cwd, const char *branch, bool keep_changes, session_wt_t *out, char **err);
/* runSessionWorktreeHook's hook half (omp 911a80022a): run the new worktree's `post-checkout` hook as `git worktree add`
 * does (`git hook run --ignore-missing post-checkout -- <null> <base> 1`, or the hook file itself for git before 2.36)
 * and append display-ready warnings (`Worktree created, but post-checkout hook exited with N: <last line>` / `... could
 * not run: ...`, sanitized and bounded like sanitizeDisplayWarnings). Silent without a `git` on PATH. Blocks until the
 * hook ends (it cannot be cancelled); the caller rediscovers skills and commands afterwards. */
void session_wt_run_hook(const char *path, const char *base_commit, strvec_t *warnings);
/* cleanSourceCheckoutIfConfigured (`worktree.cleanSource`): 1 cleaned, 0 not configured, -1 with *err (owned) */
int session_wt_clean_source(const char *cwd, char **err);
/* canAutoCreateWorktree: a primary (not linked) git checkout with commits */
bool session_wt_can_auto_create(const char *cwd);

/* exit policy (`worktree.onExit`: keep | ask | remove) */
typedef struct {
    bool dirty, moved, off_branch, present;
    char *fingerprint; /* owned */
} session_wt_state_t;
/* snapshotSessionWorktree: 0, or -1 with *err (owned) */
int session_wt_snapshot(const session_wt_t *w, session_wt_state_t *st, char **err);
void session_wt_state_free(session_wt_state_t *st);
/* decideWorktreeExit: "keep" | "remove" | "prompt" (static) */
const char *session_wt_decide_exit(const char *policy, const session_wt_state_t *st);
/* the exit prompt's message: `<short path> (<branch>)[: <risks>]` (owned) */
char *session_wt_exit_message(const session_wt_t *w, const session_wt_state_t *st);
/* removeExitWorktrees for one planned worktree: NULL when removed quietly, else the message to show (owned) */
char *session_wt_remove_planned(const session_wt_t *w, const char *approved_fingerprint); /* NULL fingerprint: force */

#endif
