/* iurl.h - router-handled internal URLs of the read/write tools that are not file-backed:
 *   out://       the agent's mid-turn reply channel (port of internal-urls/out-protocol.ts; out_url.c)
 *   conflict://  git merge-conflict blocks registered by `read` (internal-urls/conflict-protocol.ts,
 *                tools/conflict-detect.ts, tools/conflict-uri.ts; conflict.c)
 *   cfg://       the agent's view of the settings registry (internal-urls/cfg-protocol.ts; cfg_url.c) */
#ifndef OMC_IURL_H
#define OMC_IURL_H

#include "tools.h"

/* Read a router URL handled here (out://, conflict://; cfg:// renders through read.c's resource pipeline with
 * cfg_resolve). Returns false when `path` is not one of them; otherwise fills `out` (errors included). */
bool iurl_read(tool_env_t *env, const char *path, tool_result_t *out);
/* Write a router URL handled here; false when `path` is not one of them. `content` may be NULL. */
bool iurl_write(tool_env_t *env, const char *path, const char *content, tool_result_t *out);

/* ---------- out:// ---------- */
/* omp outRepliesActive: an explicitly configured `out.enabled` wins; unset, replies follow the interactive TUI */
bool out_replies_active(bool tui_transcript);
/* the session's TUI transcript draws replies (interactive top-level session) */
bool out_tui_transcript(const tool_env_t *env);
/* Rewrite unquoted `> out://…` redirects of a foreground bash command to private temp files (paths pushed to
 * *files, owned). Returns the rewritten command (owned) or NULL when the command has none or replies are off. */
char *out_redirect_rewrite(const tool_env_t *env, const char *cmd, strvec_t *files);
/* Collect the reply text written to the redirect files into out->details.replies and remove the files. */
void out_redirect_collect(strvec_t *files, tool_result_t *out);
/* Remove the redirect files without collecting (the command was backgrounded). */
void out_redirect_discard(strvec_t *files);

/* ---------- conflict:// ---------- */
typedef struct conflict_history conflict_history_t;
void conflict_history_free(conflict_history_t *h);
/* `read` of a mutable text window: register its complete blocks and return the warning footer (owned, starts with
 * "\n") or NULL when the window has none. lines are the shown file lines, first_line their 1-based number. */
char *conflict_on_read(tool_env_t *env, const char *abs, const char *display, const strvec_t *lines, int64_t first_line,
                       int *count);
/* `<path>:conflicts` selector: scan the whole file, register every block, return the summary text (owned). */
char *conflict_read_summary(tool_env_t *env, const char *abs, const char *display, int *count);
/* `<file>:conflict://N` -> `conflict://N` plus the note (owned) the model sees; NULL when no prefix */
char *conflict_recover_prefix(const char *raw, char **note);

/* ---------- cfg:// ---------- */
/* render a cfg:// URL (no line selector) as text (owned) with details {cfg: {path, count, modified}}; NULL with *err */
char *cfg_resolve(const char *url, json_t **details, char **err);
/* settings approval UI: the top-level interactive session (omp settingsApproval) */
bool cfg_settings_approval(const tool_env_t *env);

#endif
