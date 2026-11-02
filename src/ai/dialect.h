/* dialect.h - model transcript dialects for summary input (port of omp packages/ai/src/dialect: each
 * dialect's renderTranscript, plus catalog identity/dialect.ts preferredDialect and compaction/utils.ts
 * serializeConversationForSummary). Only the rendering side is ported; the in-band tool-call scanners are not
 * used by omc. */
#ifndef OMC_DIALECT_H
#define OMC_DIALECT_H

#include "ai/ai.h"
#include "core/json.h"

/* "xml" (fallback), "anthropic", "glm", "gemini", "gemma", "kimi", "qwen3", "deepseek", "minimax", "harmony",
 * "hermes" */
const char *dialect_for_class(const char *model_class);
/* the model class: the catalog identity class, else a lenient classification of the id */
const char *dialect_model_class(const ai_model_t *m, const char *model_id);
/* preferredDialect(model.id) */
const char *dialect_for_model(const ai_model_t *m);
/* resolveCatalogPolicy(model).classifierCotExtraction (the anthropic class) */
bool dialect_drops_thinking(const ai_model_t *m);

/* getDialectDefinition(dialect).renderTranscript(messages) for LLM messages. Owned. */
char *dialect_render_transcript(const char *dialect, const json_t *messages);

/* serializeConversationForSummary: useless tool results (and their calls) dropped, thinking dropped when
 * `drop_thinking`, tool results truncated to 2000 chars, rendered in `dialect`, harmony control tokens and
 * conversation/previous-summary boundary tags escaped. Owned. */
char *dialect_serialize_for_summary(const json_t *messages, const char *dialect, bool drop_thinking);

/* escapeSummaryBoundaryTags. Owned. */
char *dialect_escape_summary_tags(const char *text);
/* truncateToolResultForSummary (2000 UTF-16 units). Owned. */
char *dialect_truncate_tool_result(const char *text);

#endif
