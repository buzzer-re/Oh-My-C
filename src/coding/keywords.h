/* keywords.h - magic keywords (port of omp modes/magic-keywords.ts, pi-tui prompt/magic-keywords.ts and
 * prompt/markdown-prose.ts): standalone lowercase prose words in a user prompt that append a hidden,
 * user-attributed `<id>-notice` for that turn. `ultrathink` also lifts `auto` thinking to the model's max. */
#ifndef OMC_KEYWORDS_H
#define OMC_KEYWORDS_H

#include "core/json.h"

struct app;

/* magicKeywords.enabled && magicKeywords.<id> */
bool keywords_enabled(const char *id);
/* `word` stands as prose in text (never inside fenced code, inline code or an HTML/XML section) */
bool keywords_contains(const char *text, const char *word);
/* copy of text with code fences, code spans and HTML/XML sections blanked to spaces (newlines kept) */
char *keywords_mask_non_prose(const char *text);
/* magicKeywordRanges: every registered keyword standing as prose in `text`, whatever its setting (owned array) */
typedef struct {
    size_t start, end;
    int keyword;
} keyword_range_t;
size_t keywords_ranges(const char *text, keyword_range_t **out);
/* the keyword's gradient: an HSL hue sweep in degrees and a lightness sweep in percent */
void keywords_glow_spec(int keyword, double *hue_from, double *hue_to, double *light_from, double *light_to);
/* the hidden notices the prompt triggers, in table order: owned array of developer messages
 * {role, content, attribution:"user", customType:"<id>-notice", display:false, timestamp} */
json_t *keywords_notices(struct app *a, const char *text);

#endif
