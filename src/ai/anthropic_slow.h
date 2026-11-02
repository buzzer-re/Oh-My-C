/* anthropic_slow.h - Anthropic subscription slow mode (port of omp pi-ai providers/anthropic-slow-mode.ts wire
 * contract + coding-agent session/anthropic-slow-mode.ts lane state machine).
 *
 * Past a Claude subscription's 5-hour limit the server may keep serving on a wrap-up allowance
 * (`anthropic-ratelimit-unified-grace-*-utilization` > 0), then offer the slow lane on a 429
 * (`anthropic-ratelimit-unified-slow-offer: treatment`). With `/slow on` (`providers.anthropic.slowMode: auto`)
 * requests then carry `anthropic-usage-limit: slow` until the limit resets; 429 `slot_busy` / 529 answers become
 * server-paced capacity waits bounded by the server's max-wait budget. State is per account lane (`key:<hash>`),
 * process-wide, so subagents and advisors on one account share it. */
#ifndef OMC_ANTHROPIC_SLOW_H
#define OMC_ANTHROPIC_SLOW_H

#include "core/core.h"
#include "core/json.h"
#include "net/http.h"

typedef struct {
    const char *offer;  /* "treatment" | "control" | NULL */
    const char *status; /* "active" "not_needed" "slot_busy" "weekly_limit" "budget_exhausted" "ineligible" "off"
                           "unrecognized" | NULL */
    double retry_after_ms, max_wait_ms, budget_utilization;                        /* < 0: absent */
    double budget_reset_at, unified_reset_at, five_hour_reset_at, weekly_reset_at; /* epoch seconds, < 0: absent */
    bool unified_limit_claim, overage_in_use, overage_allowed;
    bool has_grace;
    double grace_5h, grace_7d;
} anthropic_slow_signal_t;

/* parseAnthropicSlowModeHeaders: false when the response carries none of the headers */
bool anthropic_slow_parse(const http_headers_t *h, anthropic_slow_signal_t *out);

/* per-session wiring (AnthropicSlowModeHookOptions + onLane) */
typedef struct anthropic_slow_hooks {
    bool (*low_priority)(void *ud);                                     /* `/slow on`; NULL = enabled */
    void (*notify)(void *ud, const char *level, const char *message);   /* may be NULL */
    void (*on_lane)(void *ud, const char *lane);                        /* may be NULL */
    void *ud;
} anthropic_slow_hooks_t;

/* lane key of a bearer ("key:<base36 hash>", owned) */
char *anthropic_slow_lane_key(const char *api_key);

/* provider side (first-party OAuth requests to api.anthropic.com only) */
bool anthropic_slow_is_active(const anthropic_slow_hooks_t *h, const char *lane);
void anthropic_slow_observe(const anthropic_slow_hooks_t *h, const char *lane, const anthropic_slow_signal_t *sig);
/* onFailure for a pre-content failure: a delay (ms, >= 0) to resend after, or -1 to let normal error handling run;
 * *capacity_wait is set when the delay counts toward the max-wait budget. sig may be NULL. */
int64_t anthropic_slow_on_failure(const anthropic_slow_hooks_t *h, const char *lane, int http_status, bool overloaded,
                                  const anthropic_slow_signal_t *sig, bool sent_slow, int64_t waited_ms, int attempts,
                                  bool *capacity_wait);

/* session side, by lane key */
void anthropic_slow_accept(const char *lane);
bool anthropic_slow_stop(const char *lane, const char *reason); /* reason: "user" ... */
int64_t anthropic_slow_active_resets_at(const char *lane);       /* epoch seconds, -1 when not active */
/* formatUsageLimitLabel of the lane's status (owned), NULL outside wrap-up and low priority */
char *anthropic_slow_label(const char *lane, bool low_priority);
/* AnthropicSlowModeController.status: the lane's UsageLimitState ({stage: "low_priority", resetsAtSec,
 * allowanceLeftPercent?} | {stage: "wrap_up", resetsAtSec?, extraUsage}), owned; NULL outside both stages */
json_t *anthropic_slow_status(const char *lane, bool low_priority);
/* wrapUpHintKey: window epoch when the model should be told to wrap up, -1 otherwise */
int64_t anthropic_slow_wrap_up_hint_key(const char *lane, bool low_priority);
/* formatSlowModeResetClock (owned) */
char *anthropic_slow_reset_clock(int64_t resets_at_sec);
/* test seam: drop every lane */
void anthropic_slow_reset_all(void);

#endif
