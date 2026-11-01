/* gh_int.h - internals shared by gh.c (runner/views), gh_tool.c (`github` tool) and github.c (pr:// issue://). */
#ifndef OMC_GH_INT_H
#define OMC_GH_INT_H

#include "gh.h"

#include <stdarg.h>

/* JS-semantics helpers over JSON values */
char *gh_jsv(const json_t *v);                       /* `${v}`; NULL -> "undefined" (owned) */
bool gh_truthy(const json_t *v);                     /* Boolean(v); NULL = undefined */
const json_t *gh_nn(const json_t *v);                /* v ?? undefined */
const json_t *gh_oc(const json_t *obj, const char *key); /* obj?.key */
void gh_push(strvec_t *L, const char *label, const json_t *v); /* pushLine */
void gh_push_s(strvec_t *L, const char *label, const char *v);
void gh_push_i(strvec_t *L, const char *label, int64_t v);
void gh_push_b(strvec_t *L, const char *label, bool v);
char *gh_join_trim(const strvec_t *L);
char *gh_short_sha(const char *v);
char *gh_author(const json_t *a);
char *gh_labels(const json_t *labels);
char *gh_uri_component(const char *s); /* encodeURIComponent */
strvec_t gh_args(const char *first, ...); /* NULL-terminated */

typedef struct {
    char *host; /* NULL when the ref names none */
    char *slug; /* OWNER/REPO */
} gh_ref_t;
void gh_parse_ref(const char *repo, gh_ref_t *ref);
void gh_ref_free(gh_ref_t *ref);
char *gh_format_ref(const char *host, const char *slug);
void gh_api_host_args(strvec_t *args, const gh_ref_t *ref);
char *gh_repo_from_url(const char *value);
/* https://HOST/OWNER/REPO/<kind>/N[/...] -> N (and *repo `HOST/OWNER/REPO`), else -1 */
int64_t gh_parse_item_url(const char *value, const char *kind, char **repo);
bool gh_repo_slug_equals(const char *left, const char *right);
char *gh_repo_from_cwd(const char *cwd, char **err);                         /* fresh `gh repo view` */
char *gh_resolve_repo(const char *cwd, const char *repo, const char *run_repo, char **err);
void gh_append_repo_flag(strvec_t *args, const char *repo, const char *identifier);

char *gh_format_issue(const json_t *d, const char *issue, bool comments);
char *gh_format_pr(const json_t *d, const char *pr, bool comments);

#endif
