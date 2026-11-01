/* memstore.h - local memory job/output store (port of omp memories/storage.ts).
 *
 * omp keeps this state in SQLite tables (threads, stage1_outputs, jobs) inside agent.db. omc has no
 * SQLite writer, so the same three tables live in one JSON document (<agentDir>/memory-state.json):
 *   {"version":1, "threads":{id:{...}}, "stage1_outputs":{threadId:{...}}, "jobs":{kind:{jobKey:{...}}}}
 * Every operation is a transaction: it takes an exclusive flock on <path>.lock, loads the document,
 * applies the statement semantics of storage.ts (SQL NULL = JSON null / missing), and writes the file
 * back atomically (temp + rename) when it changed. Concurrent omc processes therefore serialize the
 * same way SQLite's busy-timeout writers do. */
#ifndef OMC_MEMSTORE_H
#define OMC_MEMSTORE_H

#include "core/core.h"
#include "core/json.h"

#define MEMSTORE_STAGE1_KIND "memory_stage1"
#define MEMSTORE_GLOBAL_KIND "memory_consolidate_global"
#define MEMSTORE_DEFAULT_RETRY_REMAINING 3

typedef struct {
    char *id;
    int64_t updated_at;
    char *rollout_path;
    char *cwd;
    char *source_kind;
} mem_thread_t;

typedef struct {
    char *thread_id;
    char *ownership_token;
    int64_t input_watermark;
    int64_t source_updated_at;
    char *rollout_path;
    char *cwd;
} mem_stage1_claim_t;
typedef VEC(mem_stage1_claim_t) mem_claims_t;

typedef struct {
    char *thread_id;
    int64_t source_updated_at;
    char *raw_memory;
    char *rollout_summary;
    char *rollout_slug; /* NULL = null */
    int64_t generated_at;
    char *cwd;
} mem_output_t;
typedef VEC(mem_output_t) mem_outputs_t;

typedef struct {
    int64_t now_sec;
    int64_t thread_scan_limit;
    int64_t max_rollouts_per_startup;
    double max_rollout_age_days;
    double min_rollout_idle_hours;
    int64_t lease_seconds;
    int64_t running_concurrency_cap;
    const char *worker_id;
    const strvec_t *exclude_thread_ids;
} mem_claim_params_t;

enum { MEMSTORE_CLAIMED = 0, MEMSTORE_SKIPPED_NOT_DIRTY = 1, MEMSTORE_SKIPPED_RUNNING = 2 };

/* case-fold the scope key on case-insensitive filesystems (win32 only upstream; identity here) */
char *memstore_normalize_cwd(const char *cwd);

/* open/create the store (validates it is readable/writable). 0 or -1 with *err (owned). */
int memstore_check(const char *path, char **err);
int memstore_clear_cwd(const char *path, const char *cwd);
int memstore_upsert_threads(const char *path, const mem_thread_t *threads, size_t n);
int memstore_claim_stage1(const char *path, const mem_claim_params_t *p, mem_claims_t *out);
int memstore_enqueue_global(const char *path, int64_t source_updated_at, const char *cwd, bool force_dirty);
bool memstore_stage1_succeeded(const char *path, const char *thread_id, const char *token, int64_t source_updated_at,
                               const char *raw_memory, const char *rollout_summary, const char *rollout_slug,
                               int64_t now_sec, const char *cwd);
bool memstore_stage1_no_output(const char *path, const char *thread_id, const char *token, int64_t source_updated_at,
                               int64_t now_sec, const char *cwd);
bool memstore_stage1_failed(const char *path, const char *thread_id, const char *token, int64_t retry_delay,
                            const char *reason, int64_t now_sec);
/* returns MEMSTORE_CLAIMED (fills *token owned, *input_watermark) or a SKIPPED_* code; -1 on I/O error */
int memstore_claim_global(const char *path, const char *worker_id, int64_t lease_seconds, int64_t now_sec,
                          const char *cwd, char **token, int64_t *input_watermark);
bool memstore_heartbeat_global(const char *path, const char *token, int64_t lease_seconds, int64_t now_sec,
                               const char *cwd);
int memstore_list_outputs(const char *path, int64_t limit, const char *cwd, mem_outputs_t *out);
bool memstore_global_succeeded(const char *path, const char *token, int64_t new_watermark, int64_t now_sec,
                               const char *cwd);
bool memstore_global_failed(const char *path, const char *token, int64_t retry_delay, const char *reason,
                            int64_t now_sec, const char *cwd);
bool memstore_global_failed_unowned(const char *path, int64_t retry_delay, const char *reason, int64_t now_sec,
                                    const char *cwd);
/* borrowed-free snapshot of one job row (owned clone) or NULL; for tests and diagnostics */
json_t *memstore_job(const char *path, const char *kind, const char *key);

void mem_claims_free(mem_claims_t *c);
void mem_outputs_free(mem_outputs_t *o);

#endif
