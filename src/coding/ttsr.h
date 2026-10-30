/* ttsr.h - Time Traveling Stream Rules: regex rules that interrupt a streaming response and inject guidance. */
#ifndef OMC_TTSR_H
#define OMC_TTSR_H

#include "ai/ai.h"
#include "coding/discovery.h"

typedef struct ttsr ttsr_t;

ttsr_t *ttsr_new(const discovery_t *disc);
void ttsr_free(ttsr_t *t);
size_t ttsr_rule_count(const ttsr_t *t);

/* Reset stream buffers (call at the start of each assistant attempt). */
void ttsr_reset(ttsr_t *t);
/* Feed a streaming event. Returns true when the stream must be interrupted now. */
bool ttsr_on_event(ttsr_t *t, const ai_event_t *ev);
/* After an interrupt: owned injection text (ttsr-interrupt.md for each pending rule) or NULL; clears pending
 * and marks the rules injected. *names_out receives an owned array of rule names. */
char *ttsr_take_interrupt(ttsr_t *t, json_t **names_out);
/* Non-interrupting tool matches: owned reminder text for a tool call id (ttsr-tool-reminder.md), or NULL. */
char *ttsr_take_tool_reminder(ttsr_t *t, const char *tool_call_id);
/* Non-interrupting prose matches queued for after the message: owned text or NULL. */
char *ttsr_take_deferred(ttsr_t *t);

#endif
