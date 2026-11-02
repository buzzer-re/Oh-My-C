/* modelpreset.h - model presets: named snapshots of modelRoles + defaultThinkingLevel (port of omp
 * config/model-presets.ts and the `/modelpreset` runPresetsCommand of slash-commands/builtin-modes.ts).
 * Shared by the Models hub (s / p / P / Ctrl+←/→) and `/modelpreset` (TUI, RPC, ACP). */
#ifndef OMC_MODELPRESET_H
#define OMC_MODELPRESET_H

#include "coding/app.h"

#define MP_NO_PRESETS "No model presets saved. Use /modelpreset save <name> to create one."

/* isValidModelPresetName: a letter, then letters, digits, - or _ */
bool mp_valid_name(const char *name);
/* getModelPresetNames: saved names (project or global layer), localeCompare order */
strvec_t mp_names(void);
/* findActiveModelPreset (owned or NULL) */
char *mp_active(void);
/* settings.getModelRoles(): effective role -> selector (owned object) */
json_t *mp_effective_roles(void);
/* saveModelPreset + modelPresetSavedMessage (owned message) */
char *mp_save(const char *name);
/* applyModelPreset + formatModelPresetSwitch: the message (owned); *clean = switched with nothing shadowed,
 * *switched = the live model was switched, *wrote = roles were written */
char *mp_apply(app_t *a, const char *name, bool *clean, bool *switched, bool *wrote);

/* runPresetsCommand: `args` is everything after "/modelpreset" */
typedef struct {
    char *message;
    bool usage, failed, switched, changed_config;
} mp_outcome_t;
void mp_command(app_t *a, const char *args, mp_outcome_t *out);
void mp_outcome_free(mp_outcome_t *o);

#endif
