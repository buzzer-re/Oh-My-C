/* compaction.h - context maintenance (port of omp coding-agent session/session-maintenance.ts, compaction-methods.ts,
 * compact-modes.ts, session-handoff.ts and agent compaction/{compaction,shake,pruning}.ts): the
 * compaction.methodOrder ladder for manual and automatic passes, the summary ("soft") method with omp's
 * preparation (cut point, split turns, file operations, short summary), provider-native ("remote") compaction
 * (OpenAI Responses /responses/compact, the Anthropic compaction beta), handoff documents, shake (fenced/XML
 * regions, recent-tail protection, artifact offload) and per-turn pruning (supersedeReads, dropUseless).
 * The app.h entry points app_compact / app_compact_mode / app_handoff / app_shake are implemented here. */
#ifndef OMC_COMPACTION_H
#define OMC_COMPACTION_H

#include "coding/app.h"

typedef enum { CMP_THRESHOLD, CMP_OVERFLOW, CMP_IDLE, CMP_INCOMPLETE } cmp_reason_t;

/* CompactionCheckResult bits of an automatic pass */
enum { CMP_NONE = 0, CMP_COMMITTED = 1, CMP_REWRITTEN = 2, CMP_BLOCKED = 4 };

/* runAutoCompaction: the first usable method of compaction.methodOrder, falling through on failure.
 * trigger_tokens (<= 0: unknown) is the context size that fired the pass. */
int cmp_auto(app_t *a, cmp_reason_t reason, bool will_retry, int64_t trigger_tokens);
/* post-turn maintenance (checkCompaction threshold case): stale-result pruning, tool-output pruning, then
 * the threshold check */
void cmp_after_turn(app_t *a);
/* maintainContextMidRun: called between tool-loop turns (the agent continues) */
void cmp_mid_turn(app_t *a, const json_t *assistant);

/* resolveCompactionMethodOrder of compaction.methodOrder (owned strings) */
strvec_t cmp_method_order(void);
/* canUseRemoteCompaction for omc's transports (OpenAI Responses V1, Anthropic compaction beta) */
bool cmp_remote_supported(const ai_model_t *m);
/* rough per-message token estimate (omp uses its tokenizer) */
int64_t cmp_message_tokens(const json_t *msg);

/* pure shake region scan: [start, end) byte ranges of fenced code blocks and top-level XML elements
 * (scanTextForBlockRanges + mergeRanges); returns the count, ranges in out (owned, 2 per range) */
size_t cmp_block_ranges(const char *text, size_t **out);
/* readToolSupersedeKey (owned; NULL when exempt). "\x01" / "\x02" stand for omp's U+0000 / U+0001 separators. */
char *cmp_read_supersede_key(const char *tool, const json_t *args);

#endif
