/* artifacts.h - session artifact storage (port of omp's ArtifactManager, session/artifacts.ts) and the
 * centralized tool-result post-processing (spillLargeResultToArtifact + appendOutputNotice,
 * coding-agent/src/tools/output-meta.ts). */
#ifndef OMC_ARTIFACTS_H
#define OMC_ARTIFACTS_H

#include "agent/agent.h"
#include "tools/outsink.h"

typedef struct artifact_store artifact_store_t;

/* Session spill storage (session/artifacts.ts ArtifactManager): spill N is the file `<root>/<N>.log` at the session's
 * `local://` root, read back as `local://N.log`. `dir` is the session's directory (`<session>` beside
 * `<session>.jsonl`; NULL without a session file), which other features (agent outputs, subagent transcripts) use;
 * the spill root is bound separately (artifacts_set_root) once the local:// root is known. Subagents adopt the
 * parent's store, so the tree shares one id space and one root. */
artifact_store_t *artifacts_new(const char *dir);
artifact_store_t *artifacts_ref(artifact_store_t *s);
void artifacts_unref(artifact_store_t *s);
const char *artifacts_dir(const artifact_store_t *s); /* may be NULL */
void artifacts_set_root(artifact_store_t *s, const char *root);
/* allocatePath: claim the next spill id (past every `<N>.log` at the root) with an exclusively created empty file;
 * returns its path (owned; *id owned). NULL when no root is bound. `tool` is unused (spills are untyped). */
char *artifacts_alloc(artifact_store_t *s, const char *tool, char **id);
/* save content as a new spill; returns its id (owned) or NULL on failure */
char *artifacts_save(artifact_store_t *s, const char *tool, const char *data, size_t len);
/* path of spill `id` (`<root>/<id>.log`, a regular file), owned, or NULL */
char *artifacts_find(artifact_store_t *s, const char *id);
/* the URL of spill `id` (spillUrl: `local://<id>.log`; owned) */
char *artifacts_spill_url(const char *id);
/* parseSpillUrl: the spill id `text` names when it is `local://N.log`, optionally followed by a `:selector`; -1 else */
int64_t artifacts_parse_spill_url(const char *text);
/* copy `src_dir` into `dst_dir` recursively without overwriting (copySessionArtifacts: session fork/move) */
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
