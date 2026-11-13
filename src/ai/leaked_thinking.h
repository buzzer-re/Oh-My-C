/*
 * leaked_thinking.h - central live healing of reasoning markup leaked into the visible text channel.
 *
 * Port of packages/ai/src/utils/leaked-thinking-stream.ts (wrapLeakedThinkingStream / LeakedThinkingProjector) and
 * its gate in stream.ts (healLeakedThinking: every provider stream except one that reaches the model's official
 * endpoint, providers/anthropic-state.ts reachesOfficialEndpoint). ai_stream runs every provider's events through
 * it: leaked ```thinking fences, <think>, <thinking>, <scratchpad>, Gemma/Harmony channels become `thinking` blocks
 * live (think_heal.c with implied opens), and a bare `</think>` whose opener the chat template prefilled turns the
 * leading text block, the message's only content so far, into a closed thinking block at the same index.
 *
 * The projector owns the message: it re-projects the provider's events (its own content indexes, text_start /
 * thinking_start at its own block boundaries), forwards native thinking (signature and summary state) and tool
 * calls, and rebuilds the final message's content. Blocks no event announced (redactedThinking, ...) are left out
 * of the healed content, as upstream's projector does.
 */
#ifndef OMC_LEAKED_THINKING_H
#define OMC_LEAKED_THINKING_H

#include "ai/ai.h"

/* reachesOfficialEndpoint: compat.officialEndpoint, and for a first-party Anthropic model the request-time base URL
 * (model baseUrl, else ANTHROPIC_BASE_URL) is the official API */
bool ai_reaches_official_endpoint(const ai_model_t *m);

typedef struct leaked_thinking leaked_thinking_t;

/* a wrapper forwarding healed events to cb/ud (cb may be NULL); pass leaked_thinking_cb with the wrapper as ud to
 * the provider */
leaked_thinking_t *leaked_thinking_new(ai_event_cb cb, void *ud);
void leaked_thinking_cb(void *ud, const ai_event_t *ev);
/* after the provider returned `msg`: the healed content goes into msg (projected now when no terminal event came
 * through), unless the terminal was an error with empty content. Returns msg. */
json_t *leaked_thinking_finish(leaked_thinking_t *w, json_t *msg);
void leaked_thinking_free(leaked_thinking_t *w);

#endif
