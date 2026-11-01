/* gh.h - GitHub CLI (`gh`) runner, formatters and the `github` tool (ports of omp's utils/github.ts,
 * utils/forge-cli.ts, tools/gh*.ts). Shared with the pr:// / issue:// readers in github.c. */
#ifndef OMC_GH_H
#define OMC_GH_H

#include "tools/tools.h"

/* `gh` on PATH (github.available()) */
bool gh_available(void);

typedef struct {
    int exit_code;
    char *out, *err; /* owned */
    bool out_truncated;
} gh_res_t;
void gh_res_free(gh_res_t *r);

/* github.run: spawn `gh args...` in cwd with the non-interactive forge env; never fails on a non-zero exit.
 * Returns -1 with *err (owned) when gh is missing or the command timed out. */
int gh_run(const char *cwd, const strvec_t *args, bool trim, gh_res_t *r, char **err);
/* github.json / github.text: NULL with *err (owned ToolError message) on failure */
json_t *gh_json(const char *cwd, const strvec_t *args, bool repo_provided, bool trim, char **err);
char *gh_text(const char *cwd, const strvec_t *args, bool repo_provided, bool trim, char **err);

/* JS String.prototype.trim / trimEnd (ECMAScript white space + line terminators), owned */
char *gh_js_trim(const char *s);
char *gh_js_trim_end(const char *s);
/* normalizeText / normalizeBlock (CRLF/CR -> LF, tab -> 4 spaces, trim / trimEnd), owned; NULL input = "" */
char *gh_norm_text(const char *s);
char *gh_norm_block(const char *s);
/* normalizeOptionalString: trimmed copy or NULL when empty/NULL */
char *gh_norm_opt(const char *s);
/* parsePositiveDecimalInt: digits only, safe integer > 0; else -1 */
int64_t gh_parse_pos_int(const char *s);
/* `[HOST/]OWNER/REPO` helpers */
char *gh_default_host(void); /* lowercase GH_HOST or github.com (owned) */
/* gh repo view for cwd, memoized per absolute cwd (resolveDefaultRepoMemoized); NULL + *err on failure */
char *gh_default_repo(const char *cwd, char **err);

/* cache-aware views (github-cache.ts getOrFetchView with the cache bypassed); rendered markdown (owned) */
typedef enum { GH_CACHE_MISS, GH_CACHE_DISABLED } gh_cache_status_t;
gh_cache_status_t gh_cache_status(void); /* "disabled" when github.cache.enabled is off or no gh credential is visible */
const char *gh_freshness_note(gh_cache_status_t st);
char *gh_issue_view(const char *cwd, const char *repo, const char *issue, bool comments, char **source_url, char **err);
char *gh_pr_view(const char *cwd, const char *repo, int64_t number, bool comments, char **source_url, char **err);
/* `gh pr diff` text (falls back to the per-file API when GitHub rejects a too-large diff) */
char *gh_pr_diff_text(const char *cwd, const char *repo, int64_t number, char **err);
/* runs `gh --json` for issue data, retrying without stateReason on older gh releases */
json_t *gh_issue_json(const char *cwd, const strvec_t *args, bool repo_provided, char **err);

/* PR diff file index (gh-pr-diff.ts parsePrUnifiedDiff) */
typedef struct {
    char *path, *old_path; /* old_path NULL unless renamed/deleted away from path */
    int64_t additions, deletions;
    const char *change_type; /* modified|added|deleted|renamed|binary */
    size_t start, end;       /* byte offsets in the unified text */
} gh_diff_file_t;
typedef VEC(gh_diff_file_t) gh_diff_files_t;
void gh_parse_unified_diff(const char *text, gh_diff_files_t *out);
void gh_diff_files_free(gh_diff_files_t *f);

agent_tool_t *tool_github_new(tool_env_t *env);
/* Bun.hash (Zig std.hash.Wyhash, seed 0) */
uint64_t gh_wyhash(const void *data, size_t len, uint64_t seed);

#endif
