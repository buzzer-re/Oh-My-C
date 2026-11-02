/* settings.h - layered settings (defaults < global config.yml < project .omc|.omp/config.yml < --config < runtime). */
#ifndef OMC_SETTINGS_H
#define OMC_SETTINGS_H

#include "core/core.h"
#include "core/json.h"

void settings_init(const char *cwd, const strvec_t *overlay_files);
json_t *settings_get(const char *path);              /* borrowed effective value or NULL */
const char *settings_str(const char *path, const char *def);
int64_t settings_int(const char *path, int64_t def);
double settings_num(const char *path, double def);
bool settings_bool(const char *path, bool def);
bool settings_is_configured(const char *path); /* set in a config layer (not only the built-in default) */
void settings_set_runtime(const char *path, json_t *val); /* in-memory override (takes ownership) */
int settings_set_global(const char *path, json_t *val);   /* persist to config.yml (takes ownership) */
void settings_unset_runtime(const char *path);           /* drop an in-memory override */
int settings_unset_global(const char *path);
json_t *settings_effective(void);  /* borrowed merged tree */
json_t *settings_global(void);     /* borrowed global layer */
const char *settings_global_path(void);
const char *settings_project_dir(void); /* <cwd>/.omc or <cwd>/.omp if present, else NULL */
/* set/delete (val NULL) one key of the object at a dotted parent path (keys may contain dots/slashes) in the
 * global layer, persisted to config.yml; takes ownership of val */
int settings_set_global_entry(const char *parent, const char *key, json_t *val);
json_t *settings_project(void); /* borrowed project layer (null model roles kept: cleared values) */
char *settings_project_config_path(void); /* <project dir>/config.yml, <cwd>/.omc/config.yml when none (owned) */
/* omp project writes (setProjectModelRole / clearProjectModelRole store null): one key of the project
 * config.yml, merged into the re-read file; val NULL deletes; takes ownership of val */
int settings_set_project_entry(const char *parent, const char *key, json_t *val);
/* Setting.listen: fn(ud) runs after every runtime/global set or unset, on the writing thread */
void settings_on_change(void (*fn)(void *ud), void *ud);
void settings_off_change(void (*fn)(void *ud), void *ud);
/* layer that supplies `path` (omp SettingProvenance without the env layer): "runtime" | "overlay" | "project" |
 * "global" | "default" (static string) */
const char *settings_provenance(const char *path);
/* omp Setting.isConfigured: a layer above the defaults holds a non-null value at `path` */
bool settings_configured(const char *path);


#endif
