/* mr.h - `mr://` GitLab merge request diffs through the `glab` CLI (see mr.c) */
#ifndef OMC_MR_H
#define OMC_MR_H

#include "tools/tools.h"

/* `glab` on PATH (gitlab.available(); mr:// is advertised only then) */
bool mr_glab_available(void);
/* resolve `mr://…` (selector already stripped): 0 with *content (owned; *markdown for the file listing), or -1 with
 * *err (owned, omp's wording) */
int mr_resolve(tool_env_t *env, const char *url, char **content, bool *markdown, char **err);

#endif
