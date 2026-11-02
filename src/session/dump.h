/* dump.h - `/dump` family (port of omp session/session-dump-format.ts, session-history-format.ts and
 * AgentSession formatSessionAsText / dumpLlmRequestToTmpDir / dumpSessionArchiveToTmpDir /
 * dumpAnonymizedArchiveToTmpDir / formatAdvisorHistoryAsText).
 *
 * File names follow omp (`$TMPDIR/omp-llm-request-<snowflake>.json`, `omp-dump-<snowflake>.zip`,
 * `omp-dump-anon-<snowflake>.zip`); `$TMPDIR` falls back to /tmp like os.tmpdir(). */
#ifndef OMC_SESSION_DUMP_H
#define OMC_SESSION_DUMP_H

#include "coding/app.h"
#include "session/session.h"

/* ---- formatting (pure) ---- */

/* the session's AgentMessages along `path` (root..leaf entries) as buildSessionContext yields them:
 * compaction/branch summaries and custom messages keep omp's roles (`compactionSummary`, `custom`, ...),
 * user `!cmd` stays `bashExecution` (owned array) */
json_t *dump_context_messages(const json_t *path);

/* formatSessionDumpText: system prompt, configuration, tool inventory, markdown transcript (owned).
 * `thinking_level` NULL prints an empty level (omp's unset `thinkingLevel`). */
char *dump_format_text(const json_t *messages, const json_t *system_prompt, const ai_model_t *model, const char *thinking_level,
                       agent_tool_t *const *tools, size_t ntools);
/* formatSubagentDumpText for a persisted subagent (no live state) (owned) */
char *dump_format_subagent_text(const char *key, const json_t *messages, const char *model, const char *thinking_level, bool aborted);
/* formatSessionHistoryMarkdown with default options (`/advisor dump`) (owned, ends in "\n") */
char *dump_history_markdown(const json_t *messages);
/* renderToolInventory (`## functions` namespace block) (owned, "" for no tools) */
char *dump_tool_inventory(agent_tool_t *const *tools, size_t ntools);
/* formatDuration (pi-utils) (owned) */
char *dump_format_duration(double ms);

/* the session header as omp's loader yields it (title slot folded in: title/titleSource) (owned) */
json_t *dump_session_header(const session_t *s);

/* ---- archives ---- */

typedef struct {
    char *path;
    strvec_t files;       /* member names in write order */
    int subagent_count;
    char *subagent_error; /* NULL unless discovery failed */
    bool anonymized;
    strvec_t malformed;   /* "member\tcount" */
    strvec_t unreadable;
} dump_archive_t;

void dump_archive_free(dump_archive_t *ar);
/* formatDumpArchiveReport lines joined with "\n" (owned) */
char *dump_archive_report(const dump_archive_t *ar);

/* ZIP writer (utils/ar/zip.ts encodeZip: deflate when smaller, DOS time 0/date 1980-01-01, UTF-8 names,
 * Unix 0644) - 0 or -1 with *err */
int dump_zip_write(const char *path, const char *const *names, const char *const *data, const size_t *lens, size_t n, char **err);

/* ---- session-level operations (the caller prints the texts listed below) ---- */

/* formatSessionAsText; NULL when the session has no messages ("No messages to dump yet.") (owned) */
char *app_format_session_text(app_t *a);
/* dumpLlmRequestToTmpDir: the request-JSON sidecar; returns its path (owned), NULL when there are no
 * messages (*err NULL) or on failure (*err set) */
char *app_dump_llm_request(app_t *a, char **err);
/* `/dump all` (anon false) / `/dump anon` (anon true): 1 written (*out filled), 0 no messages,
 * -1 failure (*err set) */
int app_dump_archive(app_t *a, bool anon, dump_archive_t *out, char **err);
/* formatAdvisorHistoryAsText({compact}): NULL when no advisor is active (owned) */
char *app_advisor_history_text(app_t *a, bool compact);

#endif
