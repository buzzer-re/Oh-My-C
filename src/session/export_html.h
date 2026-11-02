/* export_html.h - `/export` / `omc export`: omp's standalone session HTML (port of
 * packages/coding-agent/src/export/html/index.ts + web-palette.ts). The file is omp's template.html.txt
 * with the session embedded as base64(JSON.stringify(sessionData)); the interactive viewer itself is
 * loaded by the page from omp's integrity-pinned release URL, exactly as the compiled omp binary does. */
#ifndef OMC_SESSION_EXPORT_HTML_H
#define OMC_SESSION_EXPORT_HTML_H

#include "core/json.h"

struct app;

/* the omp release whose hosted viewer the page loads (assets/export/viewer.generated.json is pinned to
 * it; the upstream pin in UPDATE.md) */
#define EXPORT_VIEWER_VERSION "18.14.2"

/* Load a session JSONL like omp's parseSessionContent: the fixed-width title slot folded into the header,
 * malformed lines skipped. *header (owned, NULL when the file has no valid session header), *entries
 * (owned array of the non-header entries, in file order). Returns -1 with *err when unreadable. */
int export_load_session_file(const char *path, json_t **header, json_t **entries, char **err);

/* collectSubSessions(file, {includeAdvisors, includeCompressed}): {key: {agentId, parent, header, entries,
 * leafId, aborted, malformedRecords}} for the transcripts under `<file without .jsonl>/` (owned; empty
 * object when none). Compressed `.jsonl.gz` transcripts are read too. */
json_t *export_collect_sub_sessions(const char *session_file);

/* the export document for a SessionData object ({header, entries, leafId, systemPrompt?, tools?,
 * subSessions?}); `trace` is added here. `theme_styles`: NULL for the web palette, else the CSS rules
 * of export_theme_styles. Owned. */
char *export_render_html(json_t *session_data, const char *theme_styles);

/* generateThemeStyles("theme", {dark, light}): dark/light/auto CSS custom-property rules from TUI themes
 * (built-in or <agent dir>/themes/<name>.json). NULL with *err for an unknown theme. Owned. */
char *export_theme_styles(const char *dark_theme, const char *light_theme, char **err);

/* exportFromFile (`omp --export <file> [out]`): header/entries/leafId (+ sub-sessions, trace).
 * output_path NULL = `<app>-session-<basename>.html` in the cwd. 0 and *written_path (owned), or -1 and
 * *err ("File not found: <path>", ...). */
int export_html_from_file(const char *session_file, const char *output_path, char **written_path, char **err);

/* AgentSession.exportToHtml(outputPath, useUserThemes): the live session (entries, leaf, system prompt,
 * tools) with the web palette, or the configured theme.dark/theme.light pair when use_user_themes.
 * -1 and *err "Cannot export in-memory session to HTML" for a session without a file. */
int app_export_html(struct app *a, const char *output_path, bool use_user_themes, char **written_path, char **err);

/* default output name: `<app>-session-<session file basename without .jsonl>.html` (owned) */
char *export_default_name(const char *session_file);

#endif
