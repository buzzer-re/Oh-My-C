/* perf.c - measured per-model speed (port of omp session/agent-storage.ts model_perf + the model browser's
 * #perfCell formatting). omp keeps the aggregates in agent.db; omc keeps the same running sums in
 * <agent>/model-perf.json: {"provider/id": {samples, outputTokens, genMs, ttftSamples, ttftMs, updatedAt}}.
 * Each record re-reads the file, folds the sample and writes it back atomically, so concurrent sessions
 * merge instead of clobbering each other. */
#include "ai.h"
#include "core/fs.h"

#include <math.h>
#include <pthread.h>

#define PERF_DECAY_AT 256

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_perf;   /* in-process view (borrowed rows handed out under g_mu only) */
static int64_t g_loaded; /* mtime of the file g_perf was read from */

static char *perf_path(void) { return omc_agent_path("model-perf.json"); }

static json_t *perf_read(const char *path) {
    size_t n;
    char *s = fs_read(path, &n);
    json_t *j = s ? json_parse(s, n, NULL) : NULL;
    free(s);
    if (!json_is_obj(j)) {
        json_free(j);
        j = json_obj();
    }
    return j;
}

/* refresh the in-process view when the file changed (another session recorded) */
static void perf_sync(void) {
    char *path = perf_path();
    int64_t mt = fs_exists(path) ? fs_mtime_ms(path) : 0;
    if (!g_perf || mt != g_loaded) {
        json_free(g_perf);
        g_perf = perf_read(path);
        g_loaded = mt;
    }
    free(path);
}

void model_perf_record(const char *key, int64_t output_tokens, int64_t duration_ms, int64_t ttft_ms) {
    /* normalizeModelPerfSample: no tokens / no duration measure nothing; TTFT >= duration is bogus latency */
    if (!key || !*key || output_tokens <= 0 || duration_ms <= 0) return;
    bool has_ttft = ttft_ms > 0 && ttft_ms < duration_ms;
    pthread_mutex_lock(&g_mu);
    char *path = perf_path();
    json_t *all = perf_read(path);
    json_t *row = json_get_obj(all, key);
    if (!row) {
        row = json_obj();
        json_set(all, key, row);
    }
    double samples = json_get_num(row, "samples", 0), out = json_get_num(row, "outputTokens", 0);
    double gen = json_get_num(row, "genMs", 0), ts = json_get_num(row, "ttftSamples", 0);
    double tm = json_get_num(row, "ttftMs", 0);
    if (samples >= PERF_DECAY_AT) {
        samples /= 2;
        out *= 0.5;
        gen *= 0.5;
        ts *= 0.5;
        tm *= 0.5;
    }
    json_set_num(row, "samples", samples + 1);
    json_set_num(row, "outputTokens", out + (double)output_tokens);
    json_set_num(row, "genMs", gen + (double)duration_ms);
    json_set_num(row, "ttftSamples", ts + (has_ttft ? 1 : 0));
    json_set_num(row, "ttftMs", tm + (has_ttft ? (double)ttft_ms : 0));
    json_set_int(row, "updatedAt", now_ms() / 1000);
    char *dir = path_dirname(path);
    fs_mkdir_p(dir, 0700);
    char *s = json_dump(all, 0);
    if (fs_write_atomic(path, s, strlen(s), 0600) == 0) {
        json_free(g_perf);
        g_perf = all;
        g_loaded = fs_mtime_ms(path);
        all = NULL;
    }
    json_free(all);
    free(s);
    free(dir);
    free(path);
    pthread_mutex_unlock(&g_mu);
}

void model_perf_record_message(const json_t *assistant) {
    const char *stop = json_get_str(assistant, "stopReason");
    if (!assistant || str_eq(stop, "error")) return;
    json_t *dur = json_get(assistant, "duration");
    const char *prov = json_get_str(assistant, "provider"), *model = json_get_str(assistant, "model");
    if (!json_is_num(dur) || !prov || !model) return;
    char *key = xasprintf("%s/%s", prov, model);
    const char *tier = json_get_str(assistant, "serviceTier");
    if (tier && *tier && !str_eq(tier, "auto") && !str_eq(tier, "default")) {
        char *k2 = xasprintf("%s@%s", key, tier);
        free(key);
        key = k2;
    }
    model_perf_record(key, json_get_int(json_get(assistant, "usage"), "output", 0), json_get_int(assistant, "duration", 0),
                      json_get_int(assistant, "ttft", 0));
    free(key);
}

static bool row_stats(const json_t *row, double *tps, double *ttft, double *samples) {
    double out = json_get_num(row, "outputTokens", 0), gen = json_get_num(row, "genMs", 0);
    if (!row || gen <= 0 || out <= 0) return false;
    double ts = json_get_num(row, "ttftSamples", 0);
    if (tps) *tps = out * 1000 / gen;
    if (ttft) *ttft = ts > 0 ? json_get_num(row, "ttftMs", 0) / ts : -1;
    if (samples) *samples = json_get_num(row, "samples", 0);
    return true;
}

bool model_perf_get(const char *key, double *tps, double *ttft_ms, double *samples) {
    if (!key) return false;
    pthread_mutex_lock(&g_mu);
    perf_sync();
    bool ok = row_stats(json_get_obj(g_perf, key), tps, ttft_ms, samples);
    pthread_mutex_unlock(&g_mu);
    return ok;
}

/* `118t/s`; one decimal below 10 t/s */
static char *fmt_tps(double tps) { return tps >= 10 ? xasprintf("%.0ft/s", round(tps)) : xasprintf("%.1ft/s", tps); }

/* `0.9s`; whole seconds from 10 s up */
static char *fmt_ttft(double ms) {
    double s = ms / 1000;
    return s >= 10 ? xasprintf("%.0fs", round(s)) : xasprintf("%.1fs", s);
}

char *model_perf_cell(const ai_model_t *m, bool full) {
    if (!m) return xstrdup("");
    char *key = xasprintf("%s/%s", m->provider, m->id);
    double tps = 0, ttft = -1;
    char *tier = NULL;
    pthread_mutex_lock(&g_mu);
    perf_sync();
    bool ok = row_stats(json_get_obj(g_perf, key), &tps, &ttft, NULL);
    if (!ok) { /* fastest `key@tier` aggregate (first wins ties) */
        size_t kl = strlen(key);
        const char *k;
        json_t *v;
        json_obj_foreach(g_perf, i, k, v) {
            double t2, f2;
            if (strncmp(k, key, kl) || k[kl] != '@' || !row_stats(v, &t2, &f2, NULL)) continue;
            if (!ok || t2 > tps) {
                ok = true;
                tps = t2;
                ttft = f2;
                free(tier);
                tier = xstrdup(k + kl + 1);
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
    free(key);
    if (!ok) {
        json_t *ct = json_get(m->raw, "tps");
        if (json_is_num(ct) && ct->u.num.d > 0 && isfinite(ct->u.num.d)) {
            char *t = fmt_tps(ct->u.num.d), *r = xasprintf("~%s", t);
            free(t);
            return r;
        }
        return xstrdup("");
    }
    char *t = fmt_tps(tps), *r;
    if (full && ttft >= 0) {
        char *f = fmt_ttft(ttft);
        r = xasprintf("%s %s%s%s", f, t, tier ? " " : "", tier ? tier : "");
        free(f);
    } else {
        r = xasprintf("%s%s%s", t, tier ? " " : "", tier ? tier : "");
    }
    free(t);
    free(tier);
    return r;
}
