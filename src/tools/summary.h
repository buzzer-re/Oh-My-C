/* summary.h - structural summaries of source files for `read` */
#ifndef OMC_SUMMARY_H
#define OMC_SUMMARY_H

#include "core/core.h"
#include "core/fs.h"

typedef enum { SUM_NONE, SUM_BRACE, SUM_INDENT } summary_lang_t;

summary_lang_t summary_lang(const char *path);
/* mark which lines stay visible (declarations, member signatures, closers); keep has lines->n entries */
void summary_build(const strvec_t *lines, summary_lang_t lang, bool *keep);

#endif
