/* web.h - URL fetching/reading (HTML -> markdown) and web search providers. */
#ifndef OMC_WEB_H
#define OMC_WEB_H

#include "tools.h"

/* fetch a URL and render readable text into out (supports :raw and line selectors in url suffix) */
int web_read_url(tool_env_t *env, const char *url, volatile sig_atomic_t *cancel, tool_result_t *out);
/* botWallOf: the bot protection (static name) whose challenge or block page an HTML response is, or NULL */
const char *web_bot_wall(int status, const char *content_type, const char *cf_mitigated, const char *body, size_t n);
/* convert HTML to markdown-ish readable text (owned) */
char *html_to_markdown(const char *html, size_t len, const char *base_url, char **title_out);

#endif
