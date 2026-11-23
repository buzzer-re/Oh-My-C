/* jobtext.h - one-line text of background jobs: the model's intent and the cheap model's caption (ports of omp
 * job-widgets/intent.ts, job-widgets/caption.ts and render-utils shortenEmbeddedPaths). Pure; no session state. */
#ifndef OMC_JOBTEXT_H
#define OMC_JOBTEXT_H

#include <stdbool.h>
#include <stddef.h>

/* home directory occurrences in running text as `~` (owned) */
char *job_shorten_home(const char *text);
/* the `i` intent of a call as one line of plain text with home paths shortened (owned); NULL when empty */
char *job_clean_intent(const char *intent);
/* needsJobCaption: a command longer than a glance or using shell control flow. Redirections like `2>&1` and quoted
 * text are not control flow; a double-quoted span that substitutes still is. */
bool job_caption_needed(const char *command);
/* normalizeJobCaption: the first non-empty line of a reply as plain text with home paths shortened, unquoted, without
 * list markers or a final period, at most 200 characters (owned); NULL when none */
char *job_caption_normalize(const char *reply);
/* toolIntentLabel: the activity label before the first colon followed by whitespace or the end (`[Brief label]:
 * detail` -> `[Brief label]`), the whole intent when none or when the label is blank (owned) */
char *tool_intent_label(const char *intent);
/* jobDisplayLabel: the job's intent label, else its label (owned) */
char *job_display_label(const char *intent, const char *label);
/* omp truncate(): at most `max` UTF-16 units including the `…` (owned) */
char *job_truncate(const char *s, size_t max);

#endif
