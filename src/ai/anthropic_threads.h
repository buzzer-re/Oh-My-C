/* anthropic_threads.h - Anthropic message threads (`message-threads-2026-08-12`), port of omp
 * packages/ai/src/providers/anthropic-threads.ts. A thread keeps a conversation on the server for 5 minutes after its
 * last stored response, so later turns send only what they append.
 *
 * The client stays the source of truth: a request continues only when its full body extends the conversation the
 * server holds and the thread's fixed fields are unchanged; anything else is a create (a full upload), and a refused
 * continue is replayed as a create. Doubt costs an upload, never a conversation that diverges from the client's. */
#ifndef OMC_ANTHROPIC_THREADS_H
#define OMC_ANTHROPIC_THREADS_H

#include "ai/ai.h"

#define ANTHROPIC_THREADS_BETA "message-threads-2026-08-12"

typedef struct anth_thread_turn anth_thread_turn_t;

typedef enum {
    ANTH_THREAD_NO_RECOVERY = 0, /* not a thread refusal: leave the error to the rest of the retry ladder */
    ANTH_THREAD_REPLAY,          /* resend the conversation as a create */
    ANTH_THREAD_UNTHREADED,      /* resend without `thread` */
} anth_thread_recovery_t;

/* wantsAnthropicThreads: the caller's statefulResponses, then PI_ANTHROPIC_MESSAGE_THREADS, then the model policy on
 * the official endpoint (a reroute through ANTHROPIC_BASE_URL leaves the policy flag on for a gateway that may not
 * know the field). */
bool anth_threads_wanted(const ai_model_t *m, const ai_stream_opts_t *o, const char *base_url);

/* One provider call's use of a session thread. resend_system_tools keeps `system` and `tools` on continues (Claude
 * Code subscription traffic); the documented API contract rejects them. */
anth_thread_turn_t *anth_thread_turn_new(const char *base_url, const char *model_id, const char *session_id,
                                         bool resend_system_tools);
void anth_thread_turn_free(anth_thread_turn_t *t);

/* The wire body for `params`, or NULL to send params without a thread. Owned. *is_continue tells the shape.
 * beta_header is the request's anthropic-beta without the threads beta. */
json_t *anth_thread_plan(anth_thread_turn_t *t, const json_t *params, const char *beta_header, int64_t now,
                         bool *is_continue);
/* a pre-stream failure of the planned request (status, raw error body) */
anth_thread_recovery_t anth_thread_recover(anth_thread_turn_t *t, int status, const char *body);
/* resend without `thread` after a 400 nothing else explained; if that carries the turn the chain is disabled */
bool anth_thread_drop_after_400(anth_thread_turn_t *t, int status, bool context_overflow);
/* the completed turn; message (borrowed, may be NULL) is the response as the next request serializes it when the
 * server keeps it continuable, otherwise the next request starts a new thread */
void anth_thread_settle(anth_thread_turn_t *t, const char *message_id, const json_t *message, int64_t now);

/* drop one routing session's chains (a side request's one-shot session id) */
void anth_threads_release_session(const char *session_id);
/* resetAnthropicThreads: a thread belongs to the credential that created it */
void anth_threads_reset(void);

#endif
