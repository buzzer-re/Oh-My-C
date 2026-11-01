/* compaction_point.c - per-model auto-compaction limits (omp compaction-threshold.ts +
 * model-compaction-threshold.ts). Context-window tiers (extended windows) are not modelled: every
 * model has one window, so the "Opens … window" confirmation never triggers. */
#include "compaction_point.h"

#include "config/settings.h"

#include <ctype.h>
#include <math.h>

typedef struct {
    double percent; /* -1 unset */
    int64_t tokens; /* -1 unset */
    bool fixed;
} pair_t;

/* parseThresholdEntry */
static bool parse_entry(const json_t *e, bool allow_fixed, pair_t *out) {
    if (json_is_num(e)) {
        double d = e->u.num.d;
        if (e->is_int) d = (double)e->u.num.i;
        if (d > 0 && d == floor(d) && d <= 9007199254740991.0) {
            *out = (pair_t){-1, (int64_t)d, false};
            return true;
        }
        return false;
    }
    if (!json_is_str(e)) return false;
    char *t = str_trim(json_sv(e));
    bool ok = false;
    if (allow_fixed && (t[0] == 'f' || t[0] == 'F') && t[1]) {
        bool digits = true;
        for (const char *p = t + 1; *p; p++) digits = digits && isdigit((unsigned char)*p);
        if (digits) {
            int64_t n = strtoll(t + 1, NULL, 10);
            if (n > 0) {
                *out = (pair_t){-1, n, true};
                ok = true;
            }
            free(t);
            return ok;
        }
    }
    size_t n = strlen(t);
    if (n >= 2 && t[n - 1] == '%') {
        /* ^(\d+(?:\.\d+)?)%$ */
        bool good = isdigit((unsigned char)t[0]);
        bool dot = false;
        for (size_t i = 0; i + 1 < n && good; i++) {
            if (t[i] == '.') {
                good = !dot && i > 0 && i + 2 < n && isdigit((unsigned char)t[i + 1]);
                dot = true;
            } else if (!isdigit((unsigned char)t[i]))
                good = false;
        }
        double pct = good ? strtod(t, NULL) : NAN;
        if (pct > 0 && pct <= 100) {
            *out = (pair_t){pct, -1, false};
            ok = true;
        }
    }
    free(t);
    return ok;
}

static json_t *model_thresholds(void) {
    if (!settings_bool("compaction.modelThresholdsEnabled", true)) return NULL;
    json_t *t = settings_get("compaction.modelThresholds");
    return json_is_obj(t) ? t : NULL;
}

/* matchModelCompactionThreshold (exact key, else the longest `…*` prefix); skip = key to ignore */
static bool match_threshold(json_t *raw, const ai_model_t *m, const char *skip, char **key_out, pair_t *out) {
    if (!raw || !m) return false;
    char *sel = xasprintf("%s/%s", m->provider, m->id);
    bool found = false;
    json_t *v = json_get(raw, sel);
    if (v && !(skip && !strcmp(skip, sel)) && parse_entry(v, true, out)) {
        if (key_out) *key_out = xstrdup(sel);
        found = true;
    } else {
        const char *k;
        size_t best = 0;
        json_obj_foreach(raw, i, k, v) {
            size_t kl = strlen(k);
            if (!kl || k[kl - 1] != '*' || strncmp(sel, k, kl - 1) || kl <= best) continue;
            pair_t p;
            if (!parse_entry(v, true, &p)) continue;
            best = kl;
            *out = p;
            if (key_out) {
                free(*key_out);
                *key_out = xstrdup(k);
            }
            found = true;
        }
    }
    free(sel);
    return found;
}

static cp_settings_t configured(void) {
    cp_settings_t s;
    s.enabled = settings_bool("compaction.enabled", true);
    s.threshold_percent = settings_num("compaction.thresholdPercent", -1);
    s.threshold_tokens = settings_int("compaction.thresholdTokens", -1);
    s.base_window = 0;
    s.reserve_tokens = settings_int("compaction.reserveTokens", 16384);
    return s;
}

/* applyCompactionThresholdPair */
static cp_settings_t apply_pair(cp_settings_t s, const pair_t *p) {
    if (p->tokens > 0 && !p->fixed) {
        s.threshold_tokens = -1;
        s.base_window = p->tokens;
    } else {
        s.threshold_percent = p->percent;
        s.threshold_tokens = p->tokens;
    }
    return s;
}

cp_settings_t cp_settings_for(const ai_model_t *m) {
    cp_settings_t s = configured();
    pair_t p;
    if (match_threshold(model_thresholds(), m, NULL, NULL, &p)) s = apply_pair(s, &p);
    return s;
}

static int64_t prompt_budget(int64_t window, const cp_settings_t *s) {
    int64_t reserve = MAX((int64_t)floor((double)window * 0.15), s->reserve_tokens);
    if (reserve >= window) reserve = MAX(1, (int64_t)floor((double)window * 0.15));
    return MAX(0, MIN(window - 1, window - reserve));
}

int64_t cp_threshold_tokens(int64_t window, const cp_settings_t *s) {
    if (window <= 0) return 0;
    if (s->threshold_tokens > 0) {
        if (s->threshold_tokens >= window) return prompt_budget(window, s);
        return MIN(window - 1, MAX(1, s->threshold_tokens));
    }
    if (s->base_window > 0) window = MIN(window, s->base_window);
    if (!(s->threshold_percent > 0)) return prompt_budget(window, s);
    double pct = MIN(99.0, MAX(1.0, s->threshold_percent));
    return (int64_t)floor((double)window * (pct / 100));
}

int64_t cp_model_threshold(const ai_model_t *m, int64_t window) {
    cp_settings_t s = cp_settings_for(m);
    return cp_threshold_tokens(window, &s);
}

/* ---------- formatting ---------- */

static char *trim_fraction(double v, int digits) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", digits, v);
    if (strchr(buf, '.')) {
        size_t l = strlen(buf);
        while (l && buf[l - 1] == '0') buf[--l] = 0;
        if (l && buf[l - 1] == '.') buf[--l] = 0;
    }
    return xstrdup(buf);
}

char *cp_format_window(int64_t t) {
    static const struct {
        double scale;
        const char *suf;
    } U[] = {{1e12, "T"}, {1e9, "B"}, {1e6, "M"}, {1e3, "K"}};
    double v = (double)t;
    for (size_t i = 0; i < ARRAY_LEN(U); i++) {
        if (fabs(v) < U[i].scale) continue;
        double x = v / U[i].scale;
        /* Intl compact keeps 2 significant-ish fraction digits but rounds to the unit's precision */
        x = round(x * 100) / 100;
        if (x >= 1000 && i > 0) {
            x /= 1000;
            char *n = trim_fraction(x, 2), *r = xasprintf("%s%s", n, U[i - 1].suf);
            free(n);
            return r;
        }
        char *n = trim_fraction(x, 2), *r = xasprintf("%s%s", n, U[i].suf);
        free(n);
        return r;
    }
    return xasprintf("%lld", (long long)t);
}

static char *grouped(int64_t n) {
    char raw[32], out[48];
    snprintf(raw, sizeof raw, "%lld", (long long)n);
    size_t l = strlen(raw), j = 0;
    for (size_t i = 0; i < l; i++) {
        if (i && (l - i) % 3 == 0 && raw[i - 1] != '-') out[j++] = ',';
        out[j++] = raw[i];
    }
    out[j] = 0;
    return xstrdup(out);
}

/* ---------- input ---------- */

bool cp_parse_input(const char *input, json_t **out, char **err) {
    *out = NULL;
    *err = NULL;
    char *t = str_trim(input ? input : "");
    if (!*t) {
        free(t);
        return true;
    }
    size_t n = strlen(t);
    if (t[n - 1] == '%') {
        json_t *s = json_str(t);
        pair_t p;
        bool ok = parse_entry(s, false, &p);
        if (ok) *out = s;
        else {
            json_free(s);
            *err = xasprintf("Invalid compaction point \"%s\": percentage must be in (0, 100]", t);
        }
        free(t);
        return ok;
    }
    /* ^(f)?(\d+)([kmb])?$ (any case) */
    const char *p = t;
    bool fixed = false;
    if (*p == 'f' || *p == 'F') fixed = true, p++;
    const char *ds = p;
    while (isdigit((unsigned char)*p)) p++;
    bool ok = p > ds;
    double scale = 1;
    if (ok && *p) {
        char c = (char)tolower((unsigned char)*p);
        scale = c == 'k' ? 1e3 : c == 'm' ? 1e6 : c == 'b' ? 1e9 : 0;
        ok = scale > 0 && !p[1];
    }
    if (ok) {
        double tokens = strtod(ds, NULL) * scale;
        if (tokens > 0 && tokens <= 9007199254740991.0) {
            *out = fixed ? json_str_own(xasprintf("f%.0f", tokens)) : json_int((int64_t)tokens);
            free(t);
            return true;
        }
    }
    *err = xasprintf("Invalid compaction point \"%s\": use a token count (90000, 90k, 1m), a fixed trigger (f400k) or a "
                     "percentage (80%%)",
                     t);
    free(t);
    return false;
}

static char *format_token_count(int64_t tokens) {
    static const struct {
        int64_t scale;
        const char *suf;
    } S[] = {{1000000000, "b"}, {1000000, "m"}, {1000, "k"}};
    for (size_t i = 0; i < ARRAY_LEN(S); i++)
        if (tokens % S[i].scale == 0) return xasprintf("%lld%s", (long long)(tokens / S[i].scale), S[i].suf);
    return xasprintf("%lld", (long long)tokens);
}

char *cp_draft(const ai_model_t *m) {
    char *key = NULL;
    pair_t p;
    char *sel = xasprintf("%s/%s", m->provider, m->id);
    char *r = NULL;
    if (match_threshold(model_thresholds(), m, NULL, &key, &p) && !strcmp(key, sel)) {
        if (p.tokens > 0) {
            char *c = format_token_count(p.tokens);
            r = xasprintf("%s%s", p.fixed ? "f" : "", c);
            free(c);
        } else {
            char *pc = trim_fraction(p.percent, 10);
            r = xasprintf("%s%%", pc);
            free(pc);
        }
    }
    free(key);
    free(sel);
    return r;
}

typedef struct {
    cp_settings_t settings;
    int64_t window;
    char *error;
} plan_t;

/* planModelCompactionEntry (no window tiers) */
static plan_t plan_entry(const ai_model_t *m, const json_t *entry) {
    plan_t pl = {configured(), m->context_window, NULL};
    pair_t p;
    bool have = false;
    if (entry) have = parse_entry(entry, true, &p);
    else {
        char *sel = xasprintf("%s/%s", m->provider, m->id);
        have = match_threshold(model_thresholds(), m, sel, NULL, &p);
        free(sel);
    }
    if (have) pl.settings = apply_pair(pl.settings, &p);
    if (entry && have && p.tokens > 0 && m->context_window > 0) {
        char *w = cp_format_window(m->context_window);
        if (p.fixed && p.tokens >= m->context_window) pl.error = xasprintf("Must be below the %s window", w);
        if (!p.fixed && p.tokens > m->context_window) pl.error = xasprintf("Must not exceed the %s window", w);
        free(w);
    }
    return pl;
}

static char *share_text(double share) {
    char *n = trim_fraction(round(share * 10) / 10, 1), *r = xasprintf("%s%%", n);
    free(n);
    return r;
}

/* summarizePlan */
static char *summarize(const plan_t *pl) {
    if (!pl->settings.enabled) return xstrdup("auto-compaction is off");
    if (pl->window <= 0) return NULL;
    const cp_settings_t *s = &pl->settings;
    int64_t tokens = cp_threshold_tokens(pl->window, s);
    char *tw = cp_format_window(tokens), *r;
    if (s->threshold_tokens > 0) {
        if (tokens < s->threshold_tokens) {
            char *cf = cp_format_window(s->threshold_tokens);
            r = xasprintf("compacts at %s \xc2\xb7 fixed %s, capped by window", tw, cf);
            free(cf);
        } else
            r = xasprintf("compacts at exactly %s", tw);
    } else {
        bool from_base = s->base_window > 0 && s->base_window < pl->window;
        int64_t scaled = from_base ? s->base_window : pl->window;
        double share = s->threshold_percent > 0 ? MIN(99.0, MAX(1.0, s->threshold_percent)) : (double)tokens / (double)scaled * 100;
        char *sh = share_text(share);
        if (from_base) {
            char *b = cp_format_window(scaled);
            r = xasprintf("compacts at %s \xc2\xb7 %s of %s base", tw, sh, b);
            free(b);
        } else
            r = xasprintf("compacts at %s \xc2\xb7 %s of window", tw, sh);
        free(sh);
    }
    free(tw);
    return r;
}

char *cp_preview(const ai_model_t *m, const char *input) {
    json_t *entry;
    char *err;
    if (!cp_parse_input(input, &entry, &err)) {
        free(err);
        return NULL;
    }
    plan_t pl = plan_entry(m, entry);
    char *r = NULL;
    if (!pl.error) {
        char *s = summarize(&pl);
        if (!entry && s) {
            r = xasprintf("resets: %s", s);
            free(s);
        } else
            r = s;
    }
    free(pl.error);
    json_free(entry);
    return r;
}

int cp_set(const ai_model_t *m, const char *input, char **status, char **err) {
    *status = NULL;
    *err = NULL;
    json_t *entry;
    if (!cp_parse_input(input, &entry, err)) return -1;
    char *key = xasprintf("%s/%s", m->provider, m->id);
    json_t *proj = json_get(json_get(settings_project(), "compaction"), "modelThresholds");
    if (json_is_obj(proj) && json_get(proj, key)) {
        *err = xasprintf("%s is set in the project config; edit compaction.modelThresholds there", key);
        free(key);
        json_free(entry);
        return -1;
    }
    plan_t pl = plan_entry(m, entry);
    if (pl.error) {
        *err = pl.error;
        free(key);
        json_free(entry);
        return -1;
    }
    char *described;
    pair_t p;
    if (entry && parse_entry(entry, true, &p)) {
        if (p.tokens <= 0) {
            char *pc = trim_fraction(p.percent, 10);
            described = xasprintf("%s%% of the window", pc);
            free(pc);
        } else {
            char *g = grouped(p.tokens);
            described = p.fixed ? xasprintf("fixed at %s tokens", g) : xasprintf("%s-token base", g);
            free(g);
        }
    } else
        described = xstrdup("reset");
    settings_set_global_entry("compaction.modelThresholds", key, entry ? json_clone(entry) : NULL);
    json_t *eff = json_get(json_get(json_get(settings_effective(), "compaction"), "modelThresholds"), key);
    bool effective_ok = entry ? (eff && json_equal(eff, entry)) : (!eff || json_is_null(eff));
    if (!effective_ok) {
        *err = xasprintf("%s is overridden by a higher-priority config layer; the global entry was saved but has no effect",
                         key);
        free(described);
        free(key);
        json_free(entry);
        return -1;
    }
    char *sum = summarize(&pl);
    *status = sum ? xasprintf("Compaction limit for %s: %s \xc2\xb7 %s", key, described, sum)
                  : xasprintf("Compaction limit for %s: %s", key, described);
    free(sum);
    free(described);
    free(key);
    json_free(entry);
    return 0;
}
