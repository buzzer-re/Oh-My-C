/* compat_detect.h - runtime model policy for models the bundled catalog does not carry (models.yml custom models and
 * discovered local servers): omp's buildModel / finalizeCustomModel. Ports catalog compat/taxonomy.ts classifyModel
 * (identity), compat/cascade.ts (the KDL rule cascade, evaluated over assets/compat_rules.json.z from
 * build-tools/slim_compat_rules.py), compat/resolve.ts (the per-API compat records, thinking policy), build.ts
 * (catalog assignments and corrections), hosts.ts, model-tokenizer.ts and identity/reference.ts (proxy reference lookup).
 * The bundled catalog rows keep omp's precomputed policy. */
#ifndef OMC_COMPAT_DETECT_H
#define OMC_COMPAT_DETECT_H

#include "core/json.h"

typedef struct {
    const char *cls;    /* taxonomy class ("qwen", "glm", ...; "unknown"): static or rule-owned */
    const char *family; /* rule-owned or NULL */
    const char *effort; /* effort tier collapsed out of the id: rule-owned or NULL */
    bool thinking_variant;
    int rev[3];
    bool has_rev;
    char rev_str[16];  /* "" without a revision */
    char logical[256]; /* logical id when it differs from the wire id, else "" */
} model_identity_t;

/* classifyModel(provider, id, {lenient: true}) */
void model_identity_classify(const char *provider, const char *id, model_identity_t *out);

/* omp hasLocalLoopbackBaseUrl: localhost, 127.0.0.1, 0.0.0.0, ::1, 10/8, 192.168/16, 172.16/12, *.local */
bool compat_local_base_url(const char *base_url);

/* the API has a resolved compat record (axes.ts API_COMPAT_RECORDS): the model's `compat` then is complete, and the
 * catalog's per-API defaults must not fill its unset keys */
bool compat_api_has_record(const char *api);

/* buildModel in place, for a discovered model: legacy thinking normalized (`minLevel`/`maxLevel`/`levels` ->
 * `efforts`), `identity`, `tokenizer`, the resolved `compat` (the object's own `compat`, kept as `compatConfig`, applied
 * on top; unknown keys ignored), the thinking ladder, and the rule cascade's catalog corrections. */
void compat_resolve_model(json_t *model);
/* resolveCatalogPolicy(model)[key] for a model: bundled rows carry their wire compat, the catalog axes resolve at
 * runtime. Borrowed (cached for the process), NULL when the rules set none. */
const json_t *compat_catalog_policy(const char *provider, const char *api, const char *id, const char *key);
/* the dated `effective-rates` cards of a Factory Droid model's `credit-rates` rule (borrowed), NULL when none */
const json_t *compat_factory_droid_credit_cards(const char *model_id);

/* finalizeCustomModel in place, for a models.yml model: `reference` (or NULL) fills what the entry leaves out (cost,
 * input, supportsTools, reasoning, same-provider thinking, context window, max tokens, omitMaxOutputTokens, compat), else
 * omp's defaults (free, text only, not reasoning, 128000 / 16384); then buildModel. Explicit prices, limits, kind and
 * promptCache beat the rule cascade. */
void compat_finalize_custom(json_t *model, const json_t *reference);

/* mergeCompat: objects merge recursively, otherwise the override wins. Owned (NULL when both are NULL). */
json_t *compat_merge(const json_t *base, const json_t *over);

/* getBundledModelReferenceIndex + resolveModelReference: a custom or proxied id ("[Kiro] claude-opus-4-8", "gpt-5.4:cloud",
 * "vendor/claude-sonnet-4-6-thinking") to the bundled row with its metadata. Rows are borrowed and must outlive the index. */
typedef struct compat_ref_index compat_ref_index_t;
compat_ref_index_t *compat_ref_index_new(void);
void compat_ref_index_add(compat_ref_index_t *x, const json_t *bundled_model);
void compat_ref_index_finish(compat_ref_index_t *x); /* after the last add */
const json_t *compat_ref_lookup(const compat_ref_index_t *x, const char *model_id); /* borrowed or NULL */

/* sanitizeSchemaForGrammar: widen bare boolean subschemas for grammar-constrained local backends (llama.cpp's
 * json-schema-to-grammar rejects `true`). Owned copy. */
json_t *compat_schema_for_grammar(const json_t *schema);

#endif
