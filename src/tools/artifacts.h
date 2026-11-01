/* artifacts.h - session artifact storage (port of omp's ArtifactManager, session/artifacts.ts) and the
 * centralized tool-result post-processing (spillLargeResultToArtifact + appendOutputNotice,
 * coding-agent/src/tools/output-meta.ts). */
#ifndef OMC_ARTIFACTS_H
#define OMC_ARTIFACTS_H

#include "agent/agent.h"
#include "tools/outsink.h"

typedef struct artifact_store artifact_store_t;

/* `dir` is the session's artifact directory (session file minus `.jsonl`); NULL for a non-persistent session:
 * then allocation yields no path (streaming tools keep no artifact) and saves land in a private temp dir. */
artifact_store_t *artifacts_new(const char *dir);
artifact_store_t *artifacts_ref(artifact_store_t *s);
void artifacts_unref(artifact_store_t *s);
const char *artifacts_dir(const artifact_store_t *s); /* may be NULL */
/* allocatePath: next sequential id (continuing after the highest existing `<n>.*.log`) and its path
 * `<dir>/<id>.<tool>.log` (owned; *id owned). NULL when the session has no directory. */
char *artifacts_alloc(artifact_store_t *s, const char *tool, char **id);
/* save content as a new artifact; returns its id (owned) or NULL on failure */
char *artifacts_save(artifact_store_t *s, const char *tool, const char *data, size_t len);
/* path of artifact `id` (`<id>.*.log`), owned, or NULL */
char *artifacts_find(artifact_store_t *s, const char *id);
/* copy every artifact of `src_dir` into `dst_dir` without overwriting (session fork) */
void artifacts_copy_dir(const char *src_dir, const char *dst_dir);
/* `<session file minus .jsonl>` or NULL */
char *artifacts_dir_for_session(const char *session_file);

/* OutputSink options for streaming executors from settings (tools.*): spill threshold, head bytes,
 * column cap, artifact cap. Caller fills artifact_path/artifact_id. */
void out_sink_opts_from_settings(out_sink_opts_t *o);
size_t tool_spill_threshold(void); /* tools.artifactSpillThreshold KB -> bytes */

/* wrappedExecute post-processing: spill an oversized text result to an artifact (middle elision) unless the
 * tool already saved one, then append the details.meta notice to the last text block. */
struct tool_env;
void tool_postprocess_result(struct tool_env *env, const char *tool_name, tool_result_t *res);

#endif
