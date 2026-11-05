/* dcr.h - date/cwd reminder injection (port of omp session/date-cwd-reminder.ts).
 * The system prompt stays byte-stable; the volatile date/cwd line rides on the first user turn of each request.
 * Every turn keeps the reminder it was first sent with, a changed reminder is only ever appended, and each placement
 * can be journaled in the session so a resumed session or a revived subagent resends the same bytes. */
#ifndef OMC_DCR_H
#define OMC_DCR_H

#include "core/json.h"
#include "session/session.h"

#define DCR_CUSTOM_TYPE "date-cwd-reminder"

typedef struct dcr dcr_t;

/* DateCwdReminderJournal: where an injector records the reminders it places, and reads back earlier ones */
typedef struct {
    void *ud;
    json_t *(*load)(void *ud);                   /* owned array of placements since the last /clear, oldest first */
    void (*record)(void *ud, json_t *placement); /* takes placement */
    char *(*scope)(void *ud);                    /* optional: names the history the journal reads and writes (owned) */
    void (*destroy)(void *ud);                   /* optional */
} dcr_journal_t;

char *dcr_render(const char *date, const char *cwd); /* renderDateCwdReminder (owned) */

dcr_t *dcr_new(const dcr_journal_t *journal); /* journal copied; NULL for an unjournaled injector */
void dcr_free(dcr_t *d);

/* DateCwdReminderInjector.transform over a request's messages: owned new array, NULL when nothing changes.
 * The caller serializes calls on one injector. */
json_t *dcr_transform(dcr_t *d, const json_t *messages, const char *date, const char *cwd);

/* whether a transformed message is a developer turn the injector inserted */
bool dcr_is_control(const json_t *message);

/* createSessionDateCwdReminderJournal over the session `*sp` currently points at (it may be swapped or NULL) */
dcr_journal_t dcr_session_journal(session_t **sp);

/* marks the calling thread as running a side request next to the session's writer: it neither reads the session nor
 * writes it; the injector's placements wait for the next request on the writer's thread */
void dcr_set_side_thread(bool side);

#endif
