/* compat_detect.h - runtime model policy for models the bundled catalog does not carry (models.yml custom models and
 * discovered local servers). Port of omp catalog compat/taxonomy.ts classifyModel (identity), compat/resolve.ts
 * detectOpenAI / detectOpenAICompat / fixupOpenAICompat (the detected chat-completions baseline: loopback and LAN
 * endpoints, Qwen thinking dialects, DeepSeek / Kimi reasoning-content contracts, host quirks), hosts.ts, the
 * thinking ladder of resolveThinkingPolicy, and the subset of the KDL rule cascade that reaches custom provider ids:
 * the local backends (providers/llama.cpp.kdl, lm-studio.kdl, vllm.kdl, the qwen class rules scoped to them) and the
 * provider-agnostic class rules of gpt-oss and deepseek. The bundled catalog rows keep omp's precomputed policy. */
#ifndef OMC_COMPAT_DETECT_H
#define OMC_COMPAT_DETECT_H

#include "core/json.h"

typedef struct {
    const char *cls;    /* taxonomy class ("qwen", "glm", ...; "unknown") - static */
    const char *family; /* static or NULL */
    int rev[3];
    bool has_rev;
} model_identity_t;

/* classifyModel(provider, id, {lenient: true}) */
void model_identity_classify(const char *provider, const char *id, model_identity_t *out);

/* omp hasLocalLoopbackBaseUrl: localhost, 127.0.0.1, 0.0.0.0, ::1, 10/8, 192.168/16, 172.16/12, *.local */
bool compat_local_base_url(const char *base_url);

/* Resolve a custom or discovered model object in place, the way omp's buildModel does: normalize legacy thinking
 * (`minLevel`/`maxLevel`/`levels` -> `efforts`), set `identity`, and for openai-completions / openrouter models
 * replace `compat` with the detected baseline (the object's own `compat` overrides applied on top, unknown keys
 * ignored) and fill the thinking ladder. Other APIs keep their compat and get only the thinking normalization. */
void compat_resolve_model(json_t *model);

/* sanitizeSchemaForGrammar: widen bare boolean subschemas for grammar-constrained local backends (llama.cpp's
 * json-schema-to-grammar rejects `true`). Owned copy. */
json_t *compat_schema_for_grammar(const json_t *schema);

#endif
