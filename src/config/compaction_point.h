/* compaction_point.h - per-model auto-compaction limits (port of omp config/compaction-threshold.ts and
 * session/model-compaction-threshold.ts): `compaction.modelThresholds` entries (`400000` base, `"f400000"`
 * fixed trigger, `"80%"`), the hub's `k` field (parse / preview / save) and the runtime trigger. */
#ifndef OMC_COMPACTION_POINT_H
#define OMC_COMPACTION_POINT_H

#include "ai/ai.h"
#include "core/core.h"
#include "core/json.h"

typedef struct {
    bool enabled;
    double threshold_percent; /* <= 0: unset */
    int64_t threshold_tokens; /* <= 0: unset */
    int64_t base_window;      /* > 0: a model entry's base standing in for the window */
    int64_t reserve_tokens;
} cp_settings_t;

/* cfgCompaction with the model's compaction.modelThresholds entry applied (model may be NULL) */
cp_settings_t cp_settings_for(const ai_model_t *m);
/* resolveThresholdTokens */
int64_t cp_threshold_tokens(int64_t window, const cp_settings_t *s);
/* the trigger for a model on its window (0 when the window is unknown) */
int64_t cp_model_threshold(const ai_model_t *m, int64_t window);

/* parseCompactionPointInput: *out_entry owned json (number, "fN", "N%") or NULL for reset; false + *err owned */
bool cp_parse_input(const char *input, json_t **out_entry, char **err);
/* the model's own exact entry as editable text (`90k`, `f1500k`, `80%`) or NULL (owned) */
char *cp_draft(const ai_model_t *m);
/* previewModelCompactionPoint: one short line or NULL (owned) */
char *cp_preview(const ai_model_t *m, const char *input);
/* setModelCompactionPoint: 0 saved (*status owned: "Compaction limit for p/id: … · …"), -1 error (*err owned) */
int cp_set(const ai_model_t *m, const char *input, char **status, char **err);
/* compact number like Intl compact notation with up to 2 fraction digits: 272K, 1.05M (owned) */
char *cp_format_window(int64_t tokens);

#endif
