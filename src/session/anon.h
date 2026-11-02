/* anon.h - allowlist anonymizer for session JSONL (port of omp session/session-anonymizer.ts), used
 * by `/dump anon`. One instance carries a single token table, so equal originals map to equal tokens
 * across every record and file it processes. */
#ifndef OMC_SESSION_ANON_H
#define OMC_SESSION_ANON_H

#include "core/json.h"

#define ANON_REVIEW_NOTE                                                                                       \
    "Turn contents and error text are redacted and paths/literals replaced; metadata such as model names is " \
    "kept \xe2\x80\x94 review before sharing."

typedef struct anon anon_t;

anon_t *anon_new(void);
void anon_free(anon_t *a);
/* anonymize one session record (header or entry); owned */
json_t *anon_entry(anon_t *a, const json_t *record);
/* the JSONL line for a record: JSON.stringify(anonymizer.entry(record)) (owned) */
char *anon_entry_line(anon_t *a, const json_t *record);
/* mock name of one path segment / mock path / shell command / placeholder / redaction marker (owned) */
char *anon_segment(anon_t *a, const char *value);
char *anon_path(anon_t *a, const char *value);
char *anon_command(anon_t *a, const char *value);
char *anon_placeholder(anon_t *a, const char *value);
char *anon_redact(anon_t *a, const char *value);

#endif
