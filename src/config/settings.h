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
void settings_set_runtime(const char *path, json_t *val); /* in-memory override (takes ownership) */
int settings_set_global(const char *path, json_t *val);   /* persist to config.yml (takes ownership) */
void settings_unset_runtime(const char *path);           /* drop an in-memory override */
int settings_unset_global(const char *path);
json_t *settings_effective(void);  /* borrowed merged tree */
json_t *settings_global(void);     /* borrowed global layer */
const char *settings_global_path(void);
const char *settings_project_dir(void); /* <cwd>/.omc or <cwd>/.omp if present, else NULL */


#endif
