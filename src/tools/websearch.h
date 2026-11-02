/* websearch.h - the web_search tool (port of omp web/search/{index,provider}.ts): the `web` model role chain over
 * search engines (web/<engine> catalog models) and the hosted search grounding of chat models (`webSearch`).
 * Engines that scrape or call plain search APIs live in web.c; LLM-grounded providers in websearch.c. */
#ifndef OMC_WEBSEARCH_H
#define OMC_WEBSEARCH_H

#include "ai/ai.h"
#include "tools.h"

typedef struct {
    char *title, *url, *snippet; /* snippet "" = none */
    char *date;                  /* publishedDate or NULL */
} sresult_t;
typedef VEC(sresult_t) sresults_t;

void sres_free(sresults_t *v);
void sres_add(sresults_t *v, const char *title, const char *url, const char *snippet);

/* web.c engines: 0 ok (results in out), -1 error (*err owned), -2 not configured */
int ws_search_parallel(const char *q, int limit, const char *recency, sresults_t *out, char **err);
int ws_search_brave(const char *q, int limit, const char *recency, sresults_t *out, char **err);
int ws_search_tavily(const char *q, int limit, const char *recency, sresults_t *out, char **err);
int ws_search_exa(const char *q, int limit, const char *recency, sresults_t *out, char **err);
int ws_search_ddg(const char *q, int limit, const char *recency, sresults_t *out, char **err);

int64_t ws_timeout_ms(void);                        /* providers.webSearchTimeoutSeconds */
char *ws_recency_after_date(const char *recency);   /* recencyAfterDate (owned) or NULL */
json_t *mcp_response_json(const char *body, const char *content_type); /* JSON-RPC reply of a JSON or SSE body */
char *ws_collapse_spaces(const char *s);            /* /\s+/g -> " ", trimmed (owned) */
char *ws_format_age(int64_t age_seconds);           /* formatAge (owned; "" for 0) */
bool ws_parse_js_date(const char *s, int64_t *ms);  /* new Date(s) for ISO dates */
char *ws_truncate(const char *s, size_t max);       /* truncate(s, max): UTF-16 units, "…" (owned) */


/* executeSearch: `model` (NULL = the web role chain) is a web model selector; session_model may be NULL.
 * Returns the tool text (owned) and sets *details ({response, error?}); *is_error for failures. */
char *ws_execute(const json_t *args, const char *model, const ai_model_t *session_model, const char *session_id,
                 volatile sig_atomic_t *cancel, json_t **details, bool *is_error);

#endif
