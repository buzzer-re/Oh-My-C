/* tokrate.c - TokenRateMeter (see tokrate.h). */
#include "coding/tokrate.h"

#include <math.h>

static const double HALF_LIVES[TOKRATE_SCALES] = {5000, 20000, 80000};
#define MIN_TOKENS 200.0
#define MIN_TIME_MS 4000.0
#define BUCKET_MS 250.0
#define CARRY_MAX_CHARS 32
#define HIDDEN_DECAY 0.8
#define HIDDEN_PRIOR_MS 10000.0

static size_t default_count(const char *text, size_t len) {
    (void)text;
    return (len + 3) / 4;
}

static void sums_advance(tokrate_sums_t *s, double half, double dt, bool inflight, double hidden_rate) {
    if (dt <= 0) return;
    double f = pow(2, -dt / half);
    s->tokens *= f;
    s->time *= f;
    if (inflight) {
        double integral = (half / M_LN2) * (1 - f);
        s->time += integral;
        s->tokens += hidden_rate * integral;
    }
}

static void clear_inflight(tokrate_t *m) {
    memset(m->inflight, 0, sizeof m->inflight);
    m->started_at = -1;
    m->inflight_local = 0;
    m->inflight_hidden_rate = 0;
    m->pending_index = -1;
    sb_clear(&m->pending);
}

void tokrate_init(tokrate_t *m) {
    memset(m, 0, sizeof *m);
    pthread_mutex_init(&m->mu, NULL);
    m->count = default_count;
    clear_inflight(m);
}

void tokrate_destroy(tokrate_t *m) {
    sb_free(&m->pending);
    pthread_mutex_destroy(&m->mu);
}

static void advance(tokrate_t *m, double now) {
    double dt = now - m->advanced_to;
    if (dt <= 0) return;
    m->advanced_to = now;
    for (int k = 0; k < TOKRATE_SCALES; k++) {
        sums_advance(&m->history[k], HALF_LIVES[k], dt, false, 0);
        sums_advance(&m->inflight[k], HALF_LIVES[k], dt, true, m->inflight_hidden_rate);
    }
}

/* tokenize the pending bucket into the in-flight sums, optionally holding back a trailing partial word */
static void flush_pending(tokrate_t *m, bool carry, double now) {
    if (m->pending_index < 0 || !m->pending.len) return;
    size_t len = m->pending.len, cut = len;
    if (carry) {
        const char *s = sb_cstr(&m->pending);
        size_t sp = 0;
        bool found = false;
        for (size_t i = len; i-- > 0;)
            if (s[i] == ' ' || s[i] == '\n') {
                sp = i;
                found = true;
                break;
            }
        if (found && sp > 0 && len - sp <= CARRY_MAX_CHARS) cut = sp;
    }
    char *text = xstrndup(sb_cstr(&m->pending), cut);
    char *tail = xstrdup(sb_cstr(&m->pending) + cut);
    sb_clear(&m->pending);
    sb_append(&m->pending, tail);
    free(tail);
    if (*text) {
        advance(m, now);
        double tokens = (double)m->count(text, strlen(text));
        for (int k = 0; k < TOKRATE_SCALES; k++) m->inflight[k].tokens += tokens;
        m->inflight_local += tokens;
    }
    free(text);
}

static void begin_locked(tokrate_t *m, double now) {
    clear_inflight(m);
    m->started_at = now;
    m->advanced_to = now;
    m->inflight_hidden_rate = fmax(0, m->hidden_tokens / (m->hidden_span_ms + HIDDEN_PRIOR_MS));
}

void tokrate_begin(tokrate_t *m, double now) {
    pthread_mutex_lock(&m->mu);
    begin_locked(m, now);
    pthread_mutex_unlock(&m->mu);
}

void tokrate_push(tokrate_t *m, const char *text, size_t len, double now) {
    if (!len) return;
    pthread_mutex_lock(&m->mu);
    if (m->started_at < 0) begin_locked(m, now);
    long index = (long)floor((now - m->started_at) / BUCKET_MS);
    if (index != m->pending_index) {
        flush_pending(m, true, now);
        m->pending_index = index;
    }
    sb_appendn(&m->pending, text, len);
    pthread_mutex_unlock(&m->mu);
}

void tokrate_end(tokrate_t *m, double output_tokens, double now) {
    pthread_mutex_lock(&m->mu);
    if (m->started_at < 0) {
        pthread_mutex_unlock(&m->mu);
        return;
    }
    flush_pending(m, false, now);
    advance(m, now);
    double span = now - m->started_at;
    double extra = 0;
    if (output_tokens > 0 && isfinite(output_tokens)) {
        double hidden = output_tokens - m->inflight_local;
        m->hidden_tokens = m->hidden_tokens * HIDDEN_DECAY + hidden;
        m->hidden_span_ms = m->hidden_span_ms * HIDDEN_DECAY + span;
        extra = hidden - m->inflight_hidden_rate * span;
    }
    for (int k = 0; k < TOKRATE_SCALES; k++) {
        const tokrate_sums_t *live = &m->inflight[k];
        double corrected = live->tokens + (span > 0 ? extra * live->time / span : 0);
        m->history[k].tokens += fmax(0, corrected);
        m->history[k].time += live->time;
    }
    clear_inflight(m);
    pthread_mutex_unlock(&m->mu);
}

void tokrate_reset(tokrate_t *m) {
    pthread_mutex_lock(&m->mu);
    clear_inflight(m);
    memset(m->history, 0, sizeof m->history);
    pthread_mutex_unlock(&m->mu);
}

void tokrate_seed(tokrate_t *m, double output_tokens, double duration_ms) {
    if (!(output_tokens > 0) || !(duration_ms > 0) || !isfinite(output_tokens) || !isfinite(duration_ms)) {
        tokrate_reset(m);
        return;
    }
    pthread_mutex_lock(&m->mu);
    clear_inflight(m);
    double scale = fmax(1, fmax(MIN_TOKENS / output_tokens, MIN_TIME_MS / duration_ms));
    for (int k = 0; k < TOKRATE_SCALES; k++) {
        m->history[k].tokens = output_tokens * scale;
        m->history[k].time = duration_ms * scale;
    }
    pthread_mutex_unlock(&m->mu);
}

double tokrate_rate(tokrate_t *m, double now) {
    pthread_mutex_lock(&m->mu);
    double dt = m->started_at < 0 ? 0 : now - m->advanced_to;
    double pending = m->pending.len ? (double)m->count(sb_cstr(&m->pending), m->pending.len) : 0;
    double tokens = 0, time = 0, ev_tokens = 0, ev_time = 0;
    for (int k = 0; k < TOKRATE_SCALES; k++) {
        double f = pow(2, -dt / HALF_LIVES[k]);
        double integral = (HALF_LIVES[k] / M_LN2) * (1 - f);
        ev_tokens = (m->history[k].tokens + m->inflight[k].tokens) * f + m->inflight_hidden_rate * integral + pending;
        ev_time = (m->history[k].time + m->inflight[k].time) * f + integral;
        tokens += ev_tokens;
        time += ev_time;
    }
    pthread_mutex_unlock(&m->mu);
    if (ev_tokens < MIN_TOKENS || ev_time < MIN_TIME_MS) return -1;
    return tokens * 1000 / time;
}
