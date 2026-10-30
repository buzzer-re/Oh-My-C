/* web.h - URL fetching/reading (HTML -> markdown) and web search providers. */
#ifndef OMC_WEB_H
#define OMC_WEB_H

#include "tools.h"

/* fetch a URL and render readable text into out (supports :raw and line selectors in url suffix) */
int web_read_url(tool_env_t *env, const char *url, volatile sig_atomic_t *cancel, tool_result_t *out);
/* convert HTML to markdown-ish readable text (owned) */
char *html_to_markdown(const char *html, size_t len, const char *base_url, char **title_out);

#endif
