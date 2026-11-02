/* branchsum.h - branch summaries for tree navigation (port of omp agent/compaction/branch-summarization.ts
 * and the file-operation helpers of compaction/utils.ts). The pure parts live here; the model request and
 * the leaf move are app_navigate_tree (app.c). */
#ifndef OMC_BRANCHSUM_H
#define OMC_BRANCHSUM_H

#include "core/json.h"
#include "session/session.h"

/* collectEntriesForBranchSummary: entries from `old_leaf` back to the deepest common ancestor with
 * `target`, chronological (owned clones; empty without an old leaf). *common (optional) receives the
 * common ancestor id (owned, NULL when none). */
json_t *branchsum_collect(const session_t *s, const char *old_leaf, const char *target, char **common);

/* prepareBranchEntries + the summarizer prompt: newest-first within `token_budget` (0 = unlimited),
 * serialized into `<conversation>…</conversation>` followed by `custom` or the branch-summary prompt.
 * NULL when there is nothing to summarize. *fileops (owned) receives {read, written, edited} arrays. */
char *branchsum_prompt(const json_t *entries, int64_t token_budget, const char *custom, const char *dialect,
                       json_t **fileops);

/* the stored summary: preamble + model text + the <files> section; *read_files / *modified_files (owned
 * arrays) receive the sorted file lists. Owned. */
char *branchsum_finish(const char *model_text, const json_t *fileops, json_t **read_files, json_t **modified_files);

/* serializeConversationForSummary of LLM messages in a transcript dialect (ai/dialect.h); thinking is dropped
 * for the anthropic dialect (classifier-cot-extraction). Owned. */
char *branchsum_serialize(const json_t *messages, const char *dialect);
/* preferredDialect for a catalog identity class, else the id's class (ai/dialect.h) */
const char *branchsum_dialect(const char *model_class, const char *model_id);

/* compaction/utils.ts helpers shared with compaction.c: extractFileOpsFromMessage into {read, written, edited},
 * stripReadSelector (owned), and computeFileLists + upsertFileOperations (owned; *read_files / *modified_files
 * owned sorted arrays) */
void branchsum_extract_ops(const json_t *msg, json_t *ops);
char *branchsum_strip_read_selector(const char *path);
char *branchsum_upsert_files(const char *summary, const json_t *fileops, json_t **read_files, json_t **modified_files);

#endif
