/*
 * keybindings.h - user keybinding overrides from the agent dir (omp app-keybindings.ts
 * KeybindingsManager.create / loadMergedKeybindingsConfig).
 *
 * Read order in an agent dir: keybindings.yml, else keybindings.yaml, else the legacy keybindings.json
 * (JSONC). Legacy action names (`interrupt`, `cursorUp`, `selectModel`, ...) are renamed to their
 * namespaced ids; a renamed config, or one read from the legacy .json, is written back as
 * keybindings.yml (upstream registry key order). A named profile inherits the default profile's
 * bindings read-only (profile keys win); omc has no profiles yet, so callers pass NULL.
 */
#ifndef OMC_CONFIG_KEYBINDINGS_H
#define OMC_CONFIG_KEYBINDINGS_H

#include "core/json.h"
#include "tui/keys.h"

/* Rename legacy action names; drops values that are neither a string nor a string array.
 * Owned object; *migrated is set when a legacy name was seen. */
json_t *kbcfg_migrate_names(const json_t *raw, bool *migrated);
/* Load (and migrate on disk when `write_back`) one agent dir's config. Owned object ({} when absent or
 * unreadable). *path_out (may be NULL) receives the file the config persists in (owned). */
json_t *kbcfg_load_dir(const char *agent_dir, bool write_back, char **path_out);
/* Profile config merged over the inherited dir's (NULL or same dir = none). Owned object. */
json_t *kbcfg_load_merged(const char *agent_dir, const char *inherited_dir);
/* Apply the merged user config of `agent_dir` to `kb`. */
void kbcfg_apply(tui_keybindings_t *kb, const char *agent_dir, const char *inherited_dir);
/* Load the running agent dir's bindings into the global table (KeybindingsManager.create()). */
void kbcfg_init_global(void);

#endif
