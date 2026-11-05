/* jobcaption.c - see jobcaption.h */
#include "coding/jobcaption.h"

#include "ai/ai.h"
#include "coding/costgate.h"
#include "coding/memory.h"
#include "config/model_resolver.h"
#include "config/settings.h"
#include "prompt/template.h"
#include "tools/jobs.h"
#include "tools/jobtext.h"

#include <pthread.h>
#include <stdatomic.h>

#define CAPTION_AFTER_MS 15000 /* JobWidgets authorAfterMs: job age before a caption may be requested */
#define MAX_IN_FLIGHT 2        /* requests in flight at once, across jobs */
#define MAX_COMMAND_CHARS 4000 /* command text sent to the model; past this the head says enough */
#define MAX_INTENT_CHARS 300
#define MAX_CAPTION_TOKENS 120 /* a one-line reply needs a few dozen tokens; the cap only stops a runaway answer */

static atomic_int g_in_flight;
static pthread_mutex_t g_usage_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_usage;

typedef struct {
    job_caption_t *caption;
    char *command, *intent;
    int64_t timeout_sec;
} request_t;

/* resolveWidgetModel: the `widget` role's model with a usable credential: the role (configured, or resolved like `smol`),
 * then the built-in `smol` defaults */
static const ai_model_t *widget_model(void) {
    const ai_model_t *m = mr_resolve_role("widget", NULL);
    char *key = m ? app_api_key_for(m) : NULL;
    if (key) {
        free(key);
        return m;
    }
    mr_pool_t pool = {0};
    mr_pool_available(&pool, NULL);
    strvec_t defaults = mr_role_priority_defaults("smol");
    const ai_model_t *found = NULL;
    for (size_t i = 0; i < defaults.n && !found; i++) {
        mr_result_t r;
        if (!mr_parse_pattern(defaults.v[i], &pool, &r) || !r.model) continue;
        char *k = app_api_key_for(r.model);
        if (k) found = r.model;
        free(k);
    }
    strvec_free(&defaults);
    mr_pool_free(&pool);
    return found;
}

static void *caption_thread(void *p) {
    request_t *rq = p;
    const ai_model_t *m = widget_model();
    if (m && !rq->caption->cancel) {
        /* redactWidgetSample: credential-shaped text never reaches the cheap model */
        char *cmd_t = str_trim(rq->command), *cmd_r = memory_redact_secrets(cmd_t), *cmd = job_truncate(cmd_r, MAX_COMMAND_CHARS);
        free(cmd_t);
        free(cmd_r);
        json_t *tc = json_obj();
        json_set_str(tc, "command", cmd);
        if (rq->intent) {
            char *ir = memory_redact_secrets(rq->intent), *it = job_truncate(ir, MAX_INTENT_CHARS);
            json_set_str(tc, "intent", it);
            free(ir);
            free(it);
        }
        if (rq->timeout_sec > 0) json_set_int(tc, "timeoutSec", rq->timeout_sec);
        char *user = prompt_render("coding-agent/prompts/system/job-caption-user.md", tc);
        char *system = prompt_load_or("coding-agent/prompts/system/job-caption-system.md", "");
        json_free(tc);
        free(cmd);
        json_t *sys = json_arr(), *msgs = json_arr();
        json_push_str(sys, system);
        json_push(msgs, msg_user_text(user ? user : ""));
        ai_context_t ctx = {.system_prompt = sys, .messages = msgs};
        ai_stream_opts_t o;
        ai_stream_opts_init(&o);
        char *key = app_api_key_for(m);
        o.api_key = key;
        o.disable_reasoning = true;
        o.max_tokens = MAX_CAPTION_TOKENS;
        o.cancel = &rq->caption->cancel;
        json_t *r = ai_complete(m, &ctx, &o);
        const char *stop = json_get_str(r, "stopReason");
        if (r && json_get(r, "usage")) {
            json_t *row = json_obj();
            json_set_str(row, "purpose", "job-caption");
            json_set_str(row, "api", json_get_str(r, "api") ? json_get_str(r, "api") : "");
            json_set_str(row, "provider", json_get_str(r, "provider") ? json_get_str(r, "provider") : "");
            json_set_str(row, "model", json_get_str(r, "model") ? json_get_str(r, "model") : "");
            json_set(row, "usage", json_clone(json_get(r, "usage")));
            json_set_str(row, "stopReason", stop ? stop : "");
            pthread_mutex_lock(&g_usage_mu);
            if (!g_usage) g_usage = json_arr();
            json_push(g_usage, row);
            pthread_mutex_unlock(&g_usage_mu);
        }
        if (!str_eq(stop, "error") && !str_eq(stop, "aborted")) {
            /* normalized here, so any reply is as safe to render as the model's */
            char *text = ai_message_text(r), *caption = job_caption_normalize(text);
            pthread_mutex_lock(&rq->caption->mu);
            free(rq->caption->text);
            rq->caption->text = caption;
            pthread_mutex_unlock(&rq->caption->mu);
            free(text);
        }
        json_free(r);
        json_free(sys);
        json_free(msgs);
        free(key);
        free(user);
        free(system);
    }
    atomic_fetch_sub(&g_in_flight, 1);
    job_caption_unref(rq->caption);
    free(rq->command);
    free(rq->intent);
    free(rq);
    return NULL;
}

static bool writers_busy(void) { return atomic_load(&g_in_flight) >= MAX_IN_FLIGHT; }

static void start_request(job_caption_t *c, const char *command, const char *intent, int64_t timeout_sec) {
    request_t *rq = xcalloc(1, sizeof *rq);
    pthread_mutex_lock(&c->mu);
    c->refs++;
    pthread_mutex_unlock(&c->mu);
    rq->caption = c;
    rq->command = xstrdup(command);
    rq->intent = intent ? xstrdup(intent) : NULL;
    rq->timeout_sec = timeout_sec;
    atomic_fetch_add(&g_in_flight, 1);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, caption_thread, rq) != 0) {
        atomic_fetch_sub(&g_in_flight, 1);
        job_caption_unref(c);
        free(rq->command);
        free(rq->intent);
        free(rq);
    }
    pthread_attr_destroy(&attr);
}

void job_captions_request(app_t *a) {
    if (!a || !a->env || !a->env->jobs || !settings_bool("async.widgets", true)) return;
    if (!costgate_admits()) return;
    const char *after = env_get("OMC_JOB_CAPTION_AFTER_MS"); /* tests: the 15 s age gate */
    jobs_request_captions(a->env->jobs, after && *after ? strtoll(after, NULL, 10) : CAPTION_AFTER_MS, writers_busy, start_request);
}

json_t *job_captions_take_usage(void) {
    pthread_mutex_lock(&g_usage_mu);
    json_t *u = g_usage;
    g_usage = NULL;
    pthread_mutex_unlock(&g_usage_mu);
    return u;
}
