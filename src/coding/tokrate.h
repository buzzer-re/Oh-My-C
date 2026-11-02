/* tokrate.h - live generation throughput (port of omp coding-agent utils/token-rate.ts TokenRateMeter).
 *
 * A kernel-weighted tokens/s over recent stream time: decayed token and time sums on three half-lives
 * (5 s, 20 s, 80 s). Streamed deltas are counted locally per 250 ms bucket; what the provider bills but never
 * streams (hidden reasoning, tool-call envelopes) is modelled as a hidden-token rate estimated from recent
 * messages' billed `usage.output`. The readout is blank until the longest scale holds 200 tokens over 4 s.
 *
 * omp counts tokens with its BPE tokenizer; omc estimates one token per 4 bytes of streamed text (the billed
 * count corrects the estimate as each message ends). Thread-safe: the agent worker feeds it, the UI reads it.
 */
#ifndef OMC_CODING_TOKRATE_H
#define OMC_CODING_TOKRATE_H

#include "core/core.h"

#include <pthread.h>

#define TOKRATE_SCALES 3

typedef struct {
    double tokens, time;
} tokrate_sums_t;

typedef struct {
    pthread_mutex_t mu;
    tokrate_sums_t history[TOKRATE_SCALES], inflight[TOKRATE_SCALES];
    double started_at; /* <0: no message in flight */
    double advanced_to;
    double inflight_local, inflight_hidden_rate;
    long pending_index;
    sb_t pending;
    double hidden_tokens, hidden_span_ms;
    size_t (*count)(const char *text, size_t len); /* token counter (default: bytes / 4, rounded up) */
} tokrate_t;

void tokrate_init(tokrate_t *m);
void tokrate_destroy(tokrate_t *m);
void tokrate_begin(tokrate_t *m, double now_ms);
void tokrate_push(tokrate_t *m, const char *text, size_t len, double now_ms);
/* close the in-flight message; output_tokens <= 0: unbilled (aborted), the local estimate stands */
void tokrate_end(tokrate_t *m, double output_tokens, double now_ms);
void tokrate_reset(tokrate_t *m);
void tokrate_seed(tokrate_t *m, double output_tokens, double duration_ms);
/* tokens per second, or -1 while there is not enough evidence */
double tokrate_rate(tokrate_t *m, double now_ms);

#endif
