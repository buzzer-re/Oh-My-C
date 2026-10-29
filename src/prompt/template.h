/* template.h - Handlebars-compatible template engine (subset used by omp prompts) + prompt formatting. */
#ifndef OMC_TEMPLATE_H
#define OMC_TEMPLATE_H

#include "core/core.h"
#include "core/json.h"

/* Render template with a JSON context (noEscape, non-strict), then apply the post-render format pass. */
char *tpl_render(const char *tpl, const json_t *ctx);
/* Render without the format pass. */
char *tpl_render_raw(const char *tpl, const json_t *ctx);
/* omp prompt format pass: trim trailing ws, collapse blank lines, compact tables... */
char *prompt_format(const char *text);

/* Load an embedded prompt by relative path under prompts/ (e.g. "coding-agent/prompts/tools/read.md").
 * Users can override by placing a file at ~/.omc/agent/prompts/<same path>. Returns owned text or NULL. */
char *prompt_load(const char *rel);
/* load + render */
char *prompt_render(const char *rel, const json_t *ctx);

#endif
