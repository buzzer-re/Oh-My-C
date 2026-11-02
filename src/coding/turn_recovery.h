/* turn_recovery.h - mid-stream failure recovery (omp session/turn-recovery.ts) and the AIError
 * classification it relies on (packages/ai/src/error/flags.ts, pi-utils fetch-retry.ts).
 *
 * A failed assistant turn ends one of three ways:
 *   - replay: the failed turn is dropped and the request is sent again (nothing replay-unsafe was shown);
 *   - preserve: the failed turn and its synthetic tool results stay in context and the agent continues
 *     (a tool turn interrupted by a transport failure, or any retriable failure whose emitted calls never ran);
 *   - stall-continue: a transport failure after committed text keeps the partial text, appends the
 *     stream-stall-continue.md reminder and continues (at most TR_STREAM_STALL_CONTINUE_MAX per prompt). */
#ifndef OMC_TURN_RECOVERY_H
#define OMC_TURN_RECOVERY_H

#include "core/core.h"
#include "core/json.h"

#include <stdint.h>

/* AIError.Flag */
enum {
    AIE_CLASS = 0x1000,
    AIE_CACHE_CONTROL_UNSUPPORTED = 0x0800,
    AIE_EMPTY_RESPONSE = 0x2000,
    AIE_ACCOUNT_POLICY = 0x4000,
    AIE_CONTENT_BLOCKED = 0x8000,
    AIE_THINKING_LOOP = 0x10000,
    AIE_TRANSIENT = 0x20000,
    AIE_TIMEOUT = 0x40000,
    AIE_USAGE_LIMIT = 0x80000,
    AIE_STALE_RESPONSES_ITEM = 0x100000,
    AIE_MALFORMED_FUNCTION_CALL = 0x200000,
    AIE_PROVIDER_FINISH_ERROR = 0x400000,
    AIE_CONTEXT_OVERFLOW = 0x800000,
    AIE_AUTH_FAILED = 0x1000000,
    AIE_SILENT_ABORT = 0x2000000,
    AIE_USER_INTERRUPT = 0x4000000,
    AIE_ABORT = 0x8000000,
    AIE_GRAMMAR = 0x10000000,
    AIE_FAST_MODE_UNSUPPORTED = 0x20000000,
    AIE_OAUTH_EXPIRY = 0x40000000,
};
#define AIE_PAYLOAD_REJECTED 0x80000000u

#define TR_STREAM_STALL_CONTINUE_MAX 3 /* STREAM_STALL_CONTINUE_MAX_RETRIES */

/* AIError.classifyMessage over an assistant message (errorId, errorMessage, errorStatus, errorFlags, api,
 * provider, model): a classified id (AIE_CLASS | kinds) or a bare HTTP status (0 when nothing matched). */
uint32_t aie_classify_message(const json_t *msg);
/* AIError.classifyText over an error string with an optional HTTP status (0: parse it from the text). */
uint32_t aie_classify_text(const char *err, int status, const char *api, const char *provider);
static inline bool aie_is(uint32_t id, uint32_t flag) { return (id & flag) != 0; }
/* AIError.retriable */
bool aie_retriable(uint32_t id);
/* AIError.status over a message text (STATUS_MESSAGE_PATTERNS), 0 when none */
int aie_status_from_text(const char *err);
/* pi-utils isUnexpectedSocketCloseMessage */
bool aie_socket_close_text(const char *err);
/* AIError.isCodexSteerRejection */
bool aie_codex_steer_rejection(const json_t *msg);

/* Turn-recovery predicates. `text_committed` is the host's textOutputCommitted() (false for subagents and
 * print-mode text output, whose streamed text is buffered); `aborting` is a user abort in progress. */

/* #isMidStreamTransportFailure: idle stall, HTTP/2 reset, premature close or truncated envelope */
bool tr_mid_stream_transport_failure(const json_t *msg, uint32_t id, bool aborting);
/* TurnRecovery.isRetryableError (Context overflow is the caller's: compaction, never a retry) */
bool tr_is_retryable_error(const json_t *msg, bool text_committed);
/* #unexecutedToolCallsReplaySafe: every emitted call is paired with an agent-synthesized `executed: false` result
 * (`cursor_resolved` counts calls Cursor executed mid-stream, whose results are real) */
bool tr_unexecuted_tool_calls_replay_safe(const json_t *msg, bool text_committed, int cursor_resolved);
/* classifyResolvedInterruptedToolTurn == "stream-stall": a mid-stream transport failure whose emitted tool calls
 * all have results (in omc every call of a failed turn gets one); the turn is kept and the agent continues */
bool tr_resolved_interrupted_tool_turn(const json_t *msg, bool aborting);
/* handleCommittedTextStreamStall's eligibility (without the per-prompt cap): a socket close, Codex steer
 * rejection or mid-stream transport failure on a text-only turn after its text rendered. *reset is set when the
 * failure is not of that class (omp resets the continue counter then). */
bool tr_committed_text_stall(const json_t *msg, bool text_committed, bool aborting, bool *reset);
/* the stream-stall-continue.md reminder (owned developer message, attribution "agent") */
json_t *tr_stream_stall_reminder(int attempt);

#endif
