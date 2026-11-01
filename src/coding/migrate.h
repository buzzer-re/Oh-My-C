/* migrate.h - copy an existing omp configuration into omc's agent dir (`/migrate`, `omc migrate`). */
#ifndef OMC_MIGRATE_H
#define OMC_MIGRATE_H

#include "core/core.h"

typedef struct {
    bool force;    /* overwrite files that already exist in omc's dir */
    bool dry_run;  /* report only */
    bool sessions; /* also copy sessions/ (default on) */
} migrate_opts_t;

/* omp's agent dir: $PI_CODING_AGENT_DIR, else ~/.omp/agent */
const char *migrate_omp_dir(void);
/* Copy src (omp agent dir) into dst (omc agent dir); appends a human-readable report to *rep.
 * Returns 0 on success, 1 when some files failed, -1 with *err (owned) when nothing could be done. */
int omc_migrate(const char *src, const char *dst, const migrate_opts_t *o, sb_t *rep, char **err);

#endif
