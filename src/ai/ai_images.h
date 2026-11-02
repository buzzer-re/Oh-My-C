/* ai_images.h - model-aware image policy at the provider boundary.
 *
 * Ports of omp's
 *   packages/tui/src/chat/image-loading.ts            modelLacksWebpSupport
 *   packages/catalog/src/compat/request-size.ts + rules/providers/<provider>.kdl
 *                                                     image-count / inline-image-byte / request-body budgets
 *   packages/coding-agent/src/utils/image-loading.ts  normalizeModelContextImages, normalizeModelContextMessages
 *   packages/coding-agent/src/session/provider-image-budget.ts
 *                                                     clampProviderContextImages, dropUnreadableContextImages,
 *                                                     clampProviderContextImageBytes
 *   packages/ai/src/providers/anthropic-images.ts     prepareAnthropicImageContext
 * Message arrays use the omp shapes of src/ai/ai.h; the context functions edit an owned array in place.
 */
#ifndef OMC_AI_IMAGES_H
#define OMC_AI_IMAGES_H
#include "ai/ai.h"

#define AI_IMAGE_OMISSION_TEXT "[image omitted: provider image limit]"
#define AI_WEBP_OMISSION_TEXT "[image omitted: WebP could not be decoded for this model]"

/* llama.cpp / stb_image backends (Ollama, LM Studio, local servers, `imageInputDecoder: stb`) */
bool ai_model_lacks_webp(const ai_model_t *m);
/* whether the catalog's host budgets apply (official endpoint / catalog endpoint) */
bool ai_reaches_budgeted_host(const ai_model_t *m);
int ai_image_count_budget(const ai_model_t *m);            /* default 5 */
int64_t ai_inline_image_byte_budget(const ai_model_t *m);  /* -1 none */
int64_t ai_request_body_byte_limit(const ai_model_t *m);   /* -1 none */

/* normalizeModelContextImages: each image block of `images` (array) resized/re-encoded for the model;
 * undecodable ones are kept. Returns an owned array (one slot per input image) or NULL when empty. */
json_t *ai_normalize_images(const json_t *images, const ai_model_t *m);
/* Normalize the image blocks inside a message's content array in place (normalizeAgentMessageImages). */
void ai_normalize_message_images(json_t *message, const ai_model_t *m);

/* The provider-boundary chain sdk.ts runs before every request: count clamp, WebP rewrite for STB models,
 * undecodable-image replacement, byte-budget clamp. Edits `messages` (owned) in place; system/tools only
 * feed the request-size estimate (may be NULL). Returns true when anything changed. */
bool ai_prepare_context_images(json_t *messages, const ai_model_t *m, const json_t *system, const json_t *tools);
/* prepareAnthropicImageContext: clamp every image a Claude reader receives to 8000 px per side (2000 px past
 * 20 images) and 90% of the 10 MiB base64 cap (compat maxImageDimension / maxImagePayloadBytes override).
 * Edits `messages` in place; true when anything changed. */
bool ai_anthropic_prepare_images(json_t *messages, const ai_model_t *m);
/* Claude-family model ids (class "anthropic") and their revision (4.6 for claude-opus-4-6; 0 unknown). */
bool ai_is_claude_model(const char *id);
double ai_claude_revision(const char *id);

#endif
