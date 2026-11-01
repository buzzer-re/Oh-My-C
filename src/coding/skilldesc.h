/* skilldesc.h - compressed skill descriptions for the system prompt (port of omp
 * extensibility/skill-descriptions.ts). A session renders each skill's cached routing hint, or a
 * bounded preview while a background request (smol role, then default, then tiny; never forced onto
 * the foreground model) compresses it for later sessions. The session snapshot never changes, so the
 * system prompt stays byte-stable. Cache: <agent_dir>/skill-descriptions.json. */
#ifndef OMC_SKILLDESC_H
#define OMC_SKILLDESC_H

#include "core/core.h"

typedef struct skilldesc skilldesc_t;

skilldesc_t *skilldesc_new(void);
void skilldesc_free(skilldesc_t *c); /* waits briefly for in-flight compressions */
/* sysprompt_opts_t.skill_desc callback: owned hint (snapshot) */
char *skilldesc_render(void *catalog, const char *name, const char *description);
/* deterministic bounded preview (exported for tests) */
char *skilldesc_preview(const char *description);
char *skilldesc_valid(const char *text); /* NULL when the compression is unusable */

#endif
