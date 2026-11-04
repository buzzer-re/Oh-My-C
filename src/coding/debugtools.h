/* debugtools.h - the session-independent parts of the `/debug` tools (port of omp debug/system-info.ts,
 * debug/report-bundle.ts, utils/ar tar.gz writer). The selector itself lives in modes/debugctl.c.
 *
 * omc has no JS runtime, process log files or raw-SSE capture buffer, so the system info has no Bun/Node
 * rows and report bundles carry no logs.txt / session-logs.txt / raw-sse.txt / CPU or work profiles. */
#ifndef OMC_DEBUGTOOLS_H
#define OMC_DEBUGTOOLS_H

#include "core/core.h"
#include "core/json.h"

/* collectSystemInfo: {os, arch, cpu, memory:{total, free}, versions:{app}, cwd, shell, terminal?} (owned) */
json_t *debug_system_info(const char *cwd);
/* formatSystemInfo (owned) */
char *debug_format_system_info(const json_t *info);
/* sanitizeEnv over `environ`: keys matching key/secret/token/pass/auth/credential/api/private (any case) are
 * "[REDACTED]" (owned object) */
json_t *debug_sanitized_env(void);
/* numeric process memory counters: {process:{rss, maxRss}} (bytes; owned) */
json_t *debug_memory_stats(void);

/* createReportBundle: `<reports>/omp-report-<iso timestamp>.tar.gz` with system.json, env.json, config.json
 * (`settings`, may be NULL), session.jsonl and artifacts/ (the session's artifact subtree) when `session_file`
 * is set, memory.json (`memory`, may be NULL). Returns the archive path (owned) and the member files in
 * `files`, or NULL with *err (owned). `reports_dir` NULL = ~/.omc/reports. */
char *debug_report_bundle(const char *reports_dir, const char *session_file, const char *cwd, const json_t *settings,
                          const json_t *memory, strvec_t *files, char **err);

/* encodeTar (deterministic ustar: mtime 0, 0644 files / 0755 parent directories, PAX path records for long
 * paths) gzip-compressed; 0 or -1 with *err (owned) */
int debug_write_tar_gz(const char *path, const char *const *names, const char *const *data, const size_t *lens,
                       size_t n, char **err);

/* getArtifactCacheStats / clearArtifactCache over the sessions directory's subdirectories */
typedef struct {
    int count;
    int64_t total_size;
    int64_t oldest_ms; /* 0 = none */
} debug_cache_stats_t;
void debug_artifact_cache_stats(const char *sessions_dir, debug_cache_stats_t *out);
int debug_clear_artifact_cache(const char *sessions_dir, int days_old); /* directories removed */

#endif
