/* thinkloop.h - stream-level loop guard (port of omp pi-ai utils/thinking-loop.ts and the Gemini header-runaway
 * part of session/stream-guards.ts). A thinking/prose stream that repeats an exact cycle (all guarded models) or
 * near-identical / low-information paragraphs (semantic level: Gemini, DeepSeek, Grok) is cut off as a retryable
 * "Thinking loop detected" error; the retry carries the thinking-loop-redirect notice. Claude is never guarded.
 * Settings: model.loopGuard.enabled, model.loopGuard.checkAssistantContent, model.loopGuard.toolCallReminder;
 * PI_NO_THINKING_LOOP_GUARD=1 disables. */
#ifndef OMC_THINKLOOP_H
#define OMC_THINKLOOP_H

#include "ai/ai.h"

typedef struct thinkloop thinkloop_t;

/* stream verdicts */
enum { THINKLOOP_OK = 0, THINKLOOP_LOOP = 1, THINKLOOP_GEMINI_HEADERS = 2 };

thinkloop_t *thinkloop_new(void);
void thinkloop_free(thinkloop_t *t);
/* a new assistant stream for model m */
void thinkloop_reset(thinkloop_t *t, const ai_model_t *m);
/* feed a stream event (deltas, block starts/ends, AI_EV_DONE with the final message as partial). Returns a
 * verdict once; THINKLOOP_LOOP sets the error message, THINKLOOP_GEMINI_HEADERS the header count. */
int thinkloop_on_event(thinkloop_t *t, const ai_event_t *ev);
/* "Thinking loop detected: ... Treating as a stream stall and retrying." (borrowed until the next reset) */
const char *thinkloop_error(const thinkloop_t *t);
int thinkloop_header_count(const thinkloop_t *t);

/* exposed for tests: one detector over a delta sequence ("semantic" heuristics or exact only); owned reason/NULL */
typedef struct tl_detector tl_detector_t;
tl_detector_t *tl_detector_new(bool semantic);
void tl_detector_free(tl_detector_t *d);
char *tl_detector_push(tl_detector_t *d, const char *delta, size_t len);
char *tl_detector_flush(tl_detector_t *d);

#endif
