/* anthropic_slow.c - Anthropic subscription slow mode lanes. See anthropic_slow.h. */
#include "anthropic_slow.h"

#include <math.h>
#include <pthread.h>
#include <time.h>

/* Server-directed timing is clamped to the same bounds Claude Code applies. */
#define DEFAULT_RETRY_AFTER_MS 20000
#define MIN_RETRY_AFTER_MS 5000
#define MAX_RETRY_AFTER_MS 600000
#define DEFAULT_MAX_WAIT_MS 1200000
#define MIN_MAX_WAIT_MS 60000
#define MAX_MAX_WAIT_MS 21600000
#define MAX_WAIT_COOLOFF_MS (10 * 60000)
#define RESET_GRACE_SEC 60
#define RETRY_JITTER 0.3
#define MAX_BUDGET_BLOCK_SEC 691200
#define SAME_DAY_RESET_MS (20LL * 3600000)

typedef struct {
    bool active;
    int64_t resets_at, accepted_at_ms, served, standard;
} phase_t;

typedef struct {
    bool open;
    bool seven_day;      /* zone: the low-priority lane never covers the weekly one */
    int64_t resets_at;   /* -1 unknown */
    bool extra_usage;
    int64_t epoch;
} wrap_up_t;

typedef struct lane {
    char *key;
    phase_t phase;
    int64_t offer_resets_at;   /* -1 none */
    int64_t stopped_resets_at; /* -1 none */
    int64_t cooling_until_ms;  /* -1 none */
    const char *blocked_reason; /* "weekly" | "budget" | NULL */
    int64_t blocked_until;
    double budget_utilization; /* < 0 unknown */
    int64_t retry_after_ms, max_wait_ms;
    wrap_up_t wrap;
    int64_t wrap_epoch;
    struct lane *next;
} lane_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static lane_t *g_lanes;

static int64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static lane_t *lane_get(const char *key) {
    for (lane_t *l = g_lanes; l; l = l->next)
        if (!strcmp(l->key, key)) return l;
    lane_t *l = xcalloc(1, sizeof *l);
    l->key = xstrdup(key);
    l->offer_resets_at = l->stopped_resets_at = l->cooling_until_ms = -1;
    l->budget_utilization = -1;
    l->retry_after_ms = DEFAULT_RETRY_AFTER_MS;
    l->max_wait_ms = DEFAULT_MAX_WAIT_MS;
    l->next = g_lanes;
    g_lanes = l;
    return l;
}

void anthropic_slow_reset_all(void) {
    pthread_mutex_lock(&g_mu);
    while (g_lanes) {
        lane_t *n = g_lanes->next;
        free(g_lanes->key);
        free(g_lanes);
        g_lanes = n;
    }
    pthread_mutex_unlock(&g_mu);
}

char *anthropic_slow_lane_key(const char *api_key) {
    /* FNV-1a 64 (omp: Bun.hash); only an identity inside this process */
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)(api_key ? api_key : ""); *p; p++) h = (h ^ *p) * 1099511628211ULL;
    char buf[16];
    int n = 0;
    do {
        int d = (int)(h % 36);
        buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        h /= 36;
    } while (h && n < 15);
    sb_t b = SB_INIT;
    sb_append(&b, "key:");
    while (n) sb_putc(&b, buf[--n]);
    return sb_detach(&b);
}

/* ---------- header parsing ---------- */

#define SLOW "anthropic-ratelimit-unified-slow-"

static double read_nonneg(const http_headers_t *h, const char *name) {
    const char *raw = http_headers_get(h, name);
    if (!raw) return -1;
    while (*raw == ' ' || *raw == '\t') raw++;
    if (!*raw) return -1;
    char *end;
    double v = strtod(raw, &end);
    while (*end == ' ' || *end == '\t') end++;
    if (*end || !isfinite(v) || v < 0) return -1;
    return v;
}

static double read_util(const http_headers_t *h, const char *name) {
    double v = read_nonneg(h, name);
    return v < 0 ? 0 : MIN(1.0, v);
}

static bool hdr_is(const http_headers_t *h, const char *name, const char *val) {
    const char *raw = http_headers_get(h, name);
    if (!raw) return false;
    char *t = str_trim(raw);
    bool r = !strcmp(t, val);
    free(t);
    return r;
}

static const char *parse_status(const char *raw) {
    if (!raw) return NULL;
    static const char *const S[] = {"active", "not_needed", "slot_busy", "weekly_limit", "budget_exhausted", "ineligible", "off"};
    char *t = str_trim(raw);
    const char *r = "unrecognized";
    for (size_t i = 0; i < ARRAY_LEN(S); i++)
        if (!strcmp(t, S[i])) r = S[i];
    free(t);
    return r;
}

bool anthropic_slow_parse(const http_headers_t *h, anthropic_slow_signal_t *s) {
    memset(s, 0, sizeof *s);
    if (!h) return false;
    s->offer = hdr_is(h, SLOW "offer", "treatment") ? "treatment" : hdr_is(h, SLOW "offer", "control") ? "control" : NULL;
    s->status = parse_status(http_headers_get(h, SLOW "status"));
    double ra = read_nonneg(h, SLOW "retry-after"), mw = read_nonneg(h, SLOW "max-wait");
    s->retry_after_ms = ra >= 0 ? round(ra * 1000) : -1;
    s->max_wait_ms = mw >= 0 ? round(mw * 1000) : -1;
    double bu = read_nonneg(h, SLOW "budget-utilization");
    s->budget_utilization = bu >= 0 ? MIN(1.0, bu) : -1;
    s->budget_reset_at = read_nonneg(h, SLOW "budget-reset");
    s->unified_reset_at = read_nonneg(h, "anthropic-ratelimit-unified-reset");
    s->five_hour_reset_at = read_nonneg(h, "anthropic-ratelimit-unified-5h-reset");
    s->weekly_reset_at = read_nonneg(h, "anthropic-ratelimit-unified-7d-reset");
    const char *claim = http_headers_get(h, "anthropic-ratelimit-unified-representative-claim");
    const char *ovs = http_headers_get(h, "anthropic-ratelimit-unified-overage-status");
    s->unified_limit_claim = (claim && *claim) || (ovs && *ovs);
    s->overage_in_use = hdr_is(h, "anthropic-ratelimit-unified-overage-in-use", "true");
    s->overage_allowed = hdr_is(h, "anthropic-ratelimit-unified-overage-status", "allowed") ||
                         hdr_is(h, "anthropic-ratelimit-unified-overage-status", "allowed_warning");
    if (http_headers_get(h, "anthropic-ratelimit-unified-status")) {
        s->has_grace = true;
        s->grace_5h = read_util(h, "anthropic-ratelimit-unified-grace-5h-utilization");
        s->grace_7d = read_util(h, "anthropic-ratelimit-unified-grace-7d-utilization");
    }
    bool slow_facts = s->offer || s->status || ra >= 0 || mw >= 0 || bu >= 0 || s->budget_reset_at >= 0;
    return slow_facts || s->has_grace || s->unified_limit_claim || s->five_hour_reset_at >= 0 || s->unified_reset_at >= 0;
}

/* ---------- formatting ---------- */

char *anthropic_slow_reset_clock(int64_t resets_at_sec) {
    time_t t = (time_t)resets_at_sec;
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    /* toLocale{,Time}String([], {weekday:"short", hour:"2-digit", minute:"2-digit"}) in en-US */
    strftime(buf, sizeof buf, resets_at_sec * 1000 - wall_ms() > SAME_DAY_RESET_MS ? "%a %I:%M %p" : "%I:%M %p", &tm);
    return xstrdup(buf);
}

/* ---------- state machine (AnthropicSlowModeController) ---------- */

typedef struct {
    const char *level;
    char *message;
} notice_t;

typedef struct {
    notice_t v[4];
    int n;
} notices_t;

static void push_notice(notices_t *ns, const char *level, char *msg) {
    if (ns->n < 4) ns->v[ns->n++] = (notice_t){level, msg};
    else free(msg);
}

static void flush(const anthropic_slow_hooks_t *h, notices_t *ns) {
    for (int i = 0; i < ns->n; i++) {
        if (h && h->notify) h->notify(h->ud, ns->v[i].level, ns->v[i].message);
        free(ns->v[i].message);
    }
    ns->n = 0;
}

static bool lane_stop(lane_t *l, const char *reason, int64_t now) {
    if (!l->phase.active) {
        if (strcmp(reason, "user")) l->stopped_resets_at = -1;
        return false;
    }
    if (!strcmp(reason, "max_wait")) l->cooling_until_ms = now + MAX_WAIT_COOLOFF_MS;
    if (strcmp(reason, "user") && strcmp(reason, "max_wait")) l->offer_resets_at = -1;
    l->stopped_resets_at = !strcmp(reason, "user") ? l->phase.resets_at : -1;
    memset(&l->phase, 0, sizeof l->phase);
    return true;
}

static bool lane_is_active(lane_t *l, int64_t now) {
    if (!l->phase.active) return false;
    if (now >= (l->phase.resets_at + RESET_GRACE_SEC) * 1000) {
        lane_stop(l, "reset", now);
        return false;
    }
    return true;
}

static wrap_up_t *current_wrap(lane_t *l, int64_t now) {
    if (l->wrap.open && l->wrap.resets_at >= 0 && now >= l->wrap.resets_at * 1000) l->wrap.open = false;
    return l->wrap.open ? &l->wrap : NULL;
}

static bool lane_can_follow(lane_t *l, const wrap_up_t *w, int64_t now) {
    if (w->seven_day) return false;
    if (l->blocked_reason && now < l->blocked_until * 1000) return false;
    if (l->cooling_until_ms >= 0 && now < l->cooling_until_ms) return false;
    return l->stopped_resets_at < 0 || l->stopped_resets_at * 1000 <= now;
}

/* availability: >= 0 resets_at of the window to enter, -1 unavailable */
static int64_t availability(lane_t *l, int64_t now) {
    if (lane_is_active(l, now)) return -1;
    if (l->blocked_reason && now < l->blocked_until * 1000) return -1;
    if (l->cooling_until_ms >= 0 && now < l->cooling_until_ms) return -1;
    if (l->stopped_resets_at >= 0 && l->stopped_resets_at * 1000 > now) return l->stopped_resets_at;
    if (l->offer_resets_at >= 0 && l->offer_resets_at * 1000 > now) return l->offer_resets_at;
    return -1;
}

static void activate(lane_t *l, int64_t resets_at, int64_t now) {
    l->stopped_resets_at = -1;
    l->phase = (phase_t){true, resets_at, now, 0, 0};
}

static bool lane_accept(lane_t *l, int64_t now) {
    int64_t at = availability(l, now);
    if (at < 0) return false;
    activate(l, at, now);
    return true;
}

static void end_lane(lane_t *l, const char *reason, notices_t *ns) {
    if (!lane_stop(l, reason, wall_ms())) return;
    static const struct {
        const char *reason, *level, *message;
    } END[] = {
        {"reset", "info", "Claude session limit reset \xe2\x80\x94 low priority ended."},
        {"weekly", "warning", "Low priority ended: the weekly Claude limit is reached."},
        {"budget", "warning", "Low priority ended: this week's low-priority allowance is used up."},
        {"off", "warning", "Low priority ended: Anthropic turned it off for this account."},
        {"ineligible", "warning", "Low priority ended: this account is no longer eligible."},
        {"wall", "warning", "Low priority ended: Anthropic stopped serving the slow lane."},
        {"max_wait", "warning", "Low priority paused for 10 minutes after waiting too long for spare capacity."},
        {"extra_usage", "info", "Low priority ended: extra usage is now serving requests."},
    };
    for (size_t i = 0; i < ARRAY_LEN(END); i++)
        if (!strcmp(END[i].reason, reason)) push_notice(ns, END[i].level, xstrdup(END[i].message));
}

static void apply_timing(lane_t *l, const anthropic_slow_signal_t *s) {
    if (!s) return;
    if (s->retry_after_ms >= 0) l->retry_after_ms = (int64_t)MIN((double)MAX_RETRY_AFTER_MS, MAX((double)MIN_RETRY_AFTER_MS, round(s->retry_after_ms)));
    if (s->max_wait_ms >= 0) l->max_wait_ms = (int64_t)MIN((double)MAX_MAX_WAIT_MS, MAX((double)MIN_MAX_WAIT_MS, round(s->max_wait_ms)));
    if (s->budget_utilization >= 0) l->budget_utilization = s->budget_utilization;
}

static bool low_priority(const anthropic_slow_hooks_t *h) { return !h || !h->low_priority || h->low_priority(h->ud); }

static void observe_wrap_up(lane_t *l, const anthropic_slow_signal_t *s, const anthropic_slow_hooks_t *h, int64_t now,
                            notices_t *ns) {
    if (!s->has_grace) return;
    if (MAX(s->grace_5h, s->grace_7d) <= 0) {
        l->wrap.open = false;
        return;
    }
    int64_t five = s->five_hour_reset_at >= 0 && s->five_hour_reset_at * 1000 > now ? (int64_t)s->five_hour_reset_at : -1;
    int64_t week = s->weekly_reset_at >= 0 && s->weekly_reset_at * 1000 > now ? (int64_t)s->weekly_reset_at : -1;
    bool seven = s->grace_7d > 0 && !(s->grace_5h > 0 && five >= 0 && (week < 0 || five > week));
    wrap_up_t *prev = current_wrap(l, now);
    bool had = prev != NULL;
    int64_t epoch = had ? prev->epoch : ++l->wrap_epoch;
    l->wrap = (wrap_up_t){true, seven, seven ? week : five, s->overage_in_use || s->overage_allowed, epoch};
    if (had) return;
    char *resets = NULL;
    if (l->wrap.resets_at >= 0) {
        char *c = anthropic_slow_reset_clock(l->wrap.resets_at);
        resets = xasprintf(" (resets %s)", c);
        free(c);
    }
    const char *r = resets ? resets : "";
    char *msg;
    if (l->wrap.extra_usage)
        msg = xasprintf("Claude usage limit reached%s \xe2\x80\x94 a short wrap-up allowance runs first, then extra usage.", r);
    else if (low_priority(h) && lane_can_follow(l, &l->wrap, now))
        msg = xasprintf("Claude usage limit reached%s \xe2\x80\x94 finishing on a short wrap-up allowance from your weekly limit; low "
                        "priority takes over if Anthropic offers it.",
                        r);
    else
        msg = xasprintf("Claude usage limit reached%s \xe2\x80\x94 finishing on a short wrap-up allowance from your weekly limit; the "
                        "agent will wrap up its current step.",
                        r);
    push_notice(ns, "info", msg);
    free(resets);
}

bool anthropic_slow_is_active(const anthropic_slow_hooks_t *h, const char *lane) {
    if (!low_priority(h)) return false;
    pthread_mutex_lock(&g_mu);
    bool r = lane_is_active(lane_get(lane), wall_ms());
    pthread_mutex_unlock(&g_mu);
    return r;
}

void anthropic_slow_observe(const anthropic_slow_hooks_t *h, const char *lane, const anthropic_slow_signal_t *s) {
    if (h && h->on_lane) h->on_lane(h->ud, lane);
    notices_t ns = {0};
    pthread_mutex_lock(&g_mu);
    lane_t *l = lane_get(lane);
    int64_t now = wall_ms();
    observe_wrap_up(l, s, h, now, &ns);
    if (l->phase.active) {
        if (s->five_hour_reset_at >= 0 && s->five_hour_reset_at >= l->phase.resets_at + RESET_GRACE_SEC) end_lane(l, "reset", &ns);
        else if (str_eq(s->status, "active")) {
            apply_timing(l, s);
            l->phase.served++;
        } else if (str_eq(s->status, "not_needed")) {
            apply_timing(l, s);
            l->phase.standard++;
        } else if (str_eq(s->status, "ineligible") && s->overage_in_use) {
            end_lane(l, "extra_usage", &ns);
        }
    }
    pthread_mutex_unlock(&g_mu);
    flush(h, &ns);
}

/* #endReasonFor: NULL keeps going */
static const char *end_reason_for(const anthropic_slow_signal_t *s) {
    const char *st = s ? s->status : NULL;
    if (str_eq(st, "weekly_limit")) return "weekly";
    if (str_eq(st, "budget_exhausted")) return "budget";
    if (str_eq(st, "off")) return "off";
    if (str_eq(st, "ineligible")) return s->overage_in_use ? "extra_usage" : "ineligible";
    if (str_eq(st, "slot_busy")) return NULL;
    return s && s->unified_limit_claim ? "wall" : NULL;
}

int64_t anthropic_slow_on_failure(const anthropic_slow_hooks_t *h, const char *lane, int http_status, bool overloaded,
                                  const anthropic_slow_signal_t *sig, bool sent_slow, int64_t waited_ms, int attempts,
                                  bool *capacity_wait) {
    if (h && h->on_lane) h->on_lane(h->ud, lane);
    *capacity_wait = false;
    notices_t ns = {0};
    int64_t ret = -1;
    int64_t now = wall_ms();
    bool rate_limited = http_status == 429;
    bool lp = low_priority(h);
    pthread_mutex_lock(&g_mu);
    lane_t *l = lane_get(lane);
    if (!sent_slow) {
        if (!rate_limited) goto out;
        bool after_wrap = current_wrap(l, now) != NULL;
        if (sig && (sig->unified_limit_claim || sig->offer)) l->wrap.open = false;
        if (lane_is_active(l, now)) {
            if (lp) ret = 0;
            goto out;
        }
        if (!sig || !str_eq(sig->offer, "treatment") || sig->unified_reset_at < 0) goto out;
        if (sig->unified_reset_at * 1000 <= now) goto out;
        int64_t reset = (int64_t)sig->unified_reset_at;
        l->offer_resets_at = reset; /* recorded even with /slow off, so a later /slow on enters it */
        if (!lp) goto out;
        if (l->stopped_resets_at == reset) goto out; /* an explicit /slow off holds for the window */
        if (!lane_accept(l, now)) goto out;
        apply_timing(l, sig);
        char *until = anthropic_slow_reset_clock(reset);
        push_notice(&ns, "info",
                    xasprintf("%s \xe2\x80\x94 continuing at low priority until %s. Your weekly limit still applies and responses "
                              "may pause while waiting for spare capacity. Run /slow off to stop.",
                              after_wrap ? "Wrap-up allowance used" : "Claude session limit reached", until));
        free(until);
        ret = 0;
        goto out;
    }
    if (!lane_is_active(l, now)) goto out;
    if (rate_limited) {
        const char *reason = end_reason_for(sig);
        if (reason) {
            if (!strcmp(reason, "budget") || !strcmp(reason, "weekly")) {
                double rs = !strcmp(reason, "budget") ? (sig && sig->budget_reset_at >= 0 ? sig->budget_reset_at : sig ? sig->weekly_reset_at : -1)
                                                      : (sig && sig->weekly_reset_at >= 0 ? sig->weekly_reset_at : sig ? sig->unified_reset_at : -1);
                l->blocked_reason = !strcmp(reason, "budget") ? "budget" : "weekly";
                l->blocked_until = rs > 0 ? (int64_t)round(MIN(rs, now / 1000.0 + MAX_BUDGET_BLOCK_SEC))
                                          : (int64_t)round(now / 1000.0 + MAX_BUDGET_BLOCK_SEC);
            }
            end_lane(l, reason, &ns);
            goto out;
        }
    }
    apply_timing(l, sig);
    bool slot_busy = rate_limited && sig && str_eq(sig->status, "slot_busy");
    bool cap_busy = overloaded && (!sig || !sig->status || str_eq(sig->status, "active"));
    if (!slot_busy && !cap_busy) goto out;
    int64_t remaining = l->max_wait_ms - waited_ms;
    if (remaining <= 0) {
        end_lane(l, "max_wait", &ns);
        goto out;
    }
    double jitter = 1 + ((double)rand() / RAND_MAX * 2 - 1) * RETRY_JITTER;
    int64_t delay = MIN(remaining, (int64_t)round((double)l->retry_after_ms * jitter));
    if (attempts == 0)
        push_notice(&ns, "info",
                    xasprintf("Working at low priority \xe2\x80\x94 waiting for spare capacity (next try in %llds).",
                              (long long)MAX(1, llround(delay / 1000.0))));
    *capacity_wait = true;
    ret = delay;
out:
    pthread_mutex_unlock(&g_mu);
    flush(h, &ns);
    return ret;
}

/* ---------- session side ---------- */

void anthropic_slow_accept(const char *lane) {
    pthread_mutex_lock(&g_mu);
    lane_accept(lane_get(lane), wall_ms());
    pthread_mutex_unlock(&g_mu);
}

bool anthropic_slow_stop(const char *lane, const char *reason) {
    pthread_mutex_lock(&g_mu);
    bool r = lane_stop(lane_get(lane), reason, wall_ms());
    pthread_mutex_unlock(&g_mu);
    return r;
}

int64_t anthropic_slow_active_resets_at(const char *lane) {
    pthread_mutex_lock(&g_mu);
    lane_t *l = lane_get(lane);
    int64_t r = lane_is_active(l, wall_ms()) ? l->phase.resets_at : -1;
    pthread_mutex_unlock(&g_mu);
    return r;
}

char *anthropic_slow_label(const char *lane, bool lp) {
    pthread_mutex_lock(&g_mu);
    lane_t *l = lane_get(lane);
    int64_t now = wall_ms();
    char *r = NULL;
    int64_t resets = lp && lane_is_active(l, now) ? l->phase.resets_at : -1;
    if (resets >= 0) {
        char *c = anthropic_slow_reset_clock(resets);
        if (l->budget_utilization >= 0) {
            long left = MIN(100, MAX(0, 100 - lround(l->budget_utilization * 100)));
            r = xasprintf("low priority until %s \xc2\xb7 %ld%% left", c, left);
        } else {
            r = xasprintf("low priority until %s", c);
        }
        free(c);
    } else {
        wrap_up_t *w = current_wrap(l, now);
        if (w && w->extra_usage) r = xstrdup("limit reached \xc2\xb7 wrap-up, then extra usage");
        else if (w && w->resets_at >= 0) {
            char *c = anthropic_slow_reset_clock(w->resets_at);
            r = xasprintf("limit reached \xc2\xb7 wrapping up \xc2\xb7 resets %s", c);
            free(c);
        } else if (w) {
            r = xstrdup("limit reached \xc2\xb7 wrapping up");
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

int64_t anthropic_slow_wrap_up_hint_key(const char *lane, bool lp) {
    pthread_mutex_lock(&g_mu);
    lane_t *l = lane_get(lane);
    int64_t now = wall_ms();
    int64_t r = -1;
    wrap_up_t *w = current_wrap(l, now);
    if (w && !w->extra_usage && !(lp && (lane_is_active(l, now) || lane_can_follow(l, w, now)))) r = w->epoch;
    pthread_mutex_unlock(&g_mu);
    return r;
}
