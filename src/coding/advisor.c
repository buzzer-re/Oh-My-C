/* advisor.c - port of omp's advisor: a second model shadows the primary agent, receives incremental
 * transcript updates, verifies suspicions with read-only tools and emits notes via `advise`. Notes reach the
 * primary as hidden <advisory> messages (steered mid-run, or a continuation when severe enough at run end). */
#include "advisor.h"

#include "config/settings.h"
#include "prompt/template.h"

#include <pthread.h>

typedef struct {
    char *note;
    int severity; /* 0 nit, 1 concern, 2 blocker */
    int turn;
} anote_t;

struct advisor {
    app_t *app;
    agent_t *ag;
    tool_env_t *env;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    bool thread_live;
    bool busy;
    bool stop;
    size_t last_count;  /* primary messages already delivered */
    sb_t pending;       /* rendered updates waiting for the advisor */
    VEC(anote_t) out;   /* notes produced, not yet delivered */
    strvec_t given;     /* every note ever delivered (dedup) */
    int turns;          /* completed primary turns */
    int immune_from;    /* turn index of last interrupting delivery (-1 = none) */
    int updates;        /* reviews requested */
};

static const char *SEV[] = {"nit", "concern", "blocker"};

static int sev_rank(const char *s) {
    if (str_eq(s, "blocker")) return 2;
    if (str_eq(s, "concern")) return 1;
    return 0;
}

static char *xml_text(const char *s) {
    sb_t b = SB_INIT;
    for (; s && *s; s++) {
        if (*s == '&') sb_append(&b, "&amp;");
        else if (*s == '<') sb_append(&b, "&lt;");
        else if (*s == '>') sb_append(&b, "&gt;");
        else sb_putc(&b, *s);
    }
    return sb_detach(&b);
}

/* ---------- transcript rendering ---------- */

static void clip(sb_t *b, const char *s, size_t max) {
    size_t n = strlen(s);
    if (n <= max) {
        sb_append(b, s);
        return;
    }
    sb_appendn(b, s, max / 2);
    sb_appendf(b, "\n… [%zu chars elided] …\n", n - max);
    sb_append(b, s + n - max / 2);
}

static void render_messages(sb_t *b, const json_t *msgs, size_t from, bool thinking) {
    json_t *m, *blk;
    for (size_t i = from; i < json_len(msgs); i++) {
        m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        if (str_eq(json_get_str(m, "customType"), "advisor")) continue;
        if (str_eq(role, "user")) {
            char *t = ai_message_text(m);
            const char *ct = json_get_str(m, "customType");
            if (ct) sb_appendf(b, "**Context (%s):**\n", ct);
            else sb_append(b, "**User:**\n");
            clip(b, t, 8000);
            sb_append(b, "\n\n");
            free(t);
        } else if (str_eq(role, "assistant")) {
            json_arr_foreach(json_get_arr(m, "content"), k, blk) {
                const char *ty = json_get_str(blk, "type");
                if (str_eq(ty, "text") && json_get_str(blk, "text") && *json_get_str(blk, "text")) {
                    sb_append(b, "**Assistant:**\n");
                    clip(b, json_get_str(blk, "text"), 8000);
                    sb_append(b, "\n\n");
                } else if (thinking && str_eq(ty, "thinking") && json_get_str(blk, "thinking")) {
                    sb_append(b, "**Assistant thinking:**\n");
                    clip(b, json_get_str(blk, "thinking"), 4000);
                    sb_append(b, "\n\n");
                } else if (str_eq(ty, "toolCall")) {
                    char *args = json_dump(json_get(blk, "arguments"), 0);
                    sb_appendf(b, "**Tool call** `%s`: ", json_get_str(blk, "name"));
                    clip(b, args, 3000);
                    sb_append(b, "\n\n");
                    free(args);
                }
            }
            const char *stop = json_get_str(m, "stopReason");
            if (str_eq(stop, "error")) sb_appendf(b, "**Assistant error:** %s\n\n", json_get_str(m, "errorMessage"));
        } else if (str_eq(role, "toolResult")) {
            char *t = ai_message_text(m);
            sb_appendf(b, "**Tool result** `%s`%s:\n", json_get_str(m, "toolName"),
                       json_get_bool(m, "isError", false) ? " (error)" : "");
            clip(b, t, 4000);
            sb_append(b, "\n\n");
            free(t);
        }
    }
}

/* ---------- advise tool ---------- */

static int advise_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    advisor_t *ad = self->ud;
    UNUSED(ctx);
    const char *note = json_get_str(args, "note");
    char *t = str_trim(note ? note : "");
    if (!*t) {
        free(t);
        tool_result_error(out, "note is required");
        return 0;
    }
    int sev = sev_rank(json_get_str(args, "severity"));
    pthread_mutex_lock(&ad->mu);
    bool dup = strvec_contains(&ad->given, t);
    for (size_t i = 0; i < ad->out.n && !dup; i++) dup = !strcmp(ad->out.v[i].note, t);
    size_t nonblock = 0;
    for (size_t i = 0; i < ad->out.n; i++) nonblock += ad->out.v[i].severity < 2;
    int cap = (int)settings_int("advisor.maxNotesPerUpdate", 4);
    bool capped = sev < 2 && cap > 0 && nonblock >= (size_t)cap;
    if (!dup && !capped) {
        anote_t n = {t, sev, ad->turns};
        vec_push(&ad->out, n);
        t = NULL;
    }
    pthread_mutex_unlock(&ad->mu);
    free(t);
    if (dup) tool_result_text(out, "Suppressed: identical advice was already given.");
    else if (capped) tool_result_text(out, "Suppressed: note budget for this update is spent (blockers are exempt).");
    else tool_result_text(out, "Advice recorded.");
    out->details = json_obj();
    json_set_str(out->details, "note", note);
    json_set_str(out->details, "severity", SEV[sev]);
    return 0;
}

/* ---------- lifecycle ---------- */

/* resolveAdvisorRoleSelection: the model the `@advisor` role alias resolves to, NULL when none */
const ai_model_t *advisor_role_model(void) {
    const ai_model_t *m = NULL;
    mr_pool_t avail = {0};
    mr_pool_available(&avail, "chat");
    mr_result_t r;
    if (mr_resolve_value("@advisor", &avail, &r)) m = r.model;
    mr_result_clear(&r);
    mr_pool_free(&avail);
    return m;
}

static void build_agent(advisor_t *ad) {
    app_t *a = ad->app;
    agent_t *ag = agent_new();
    const ai_model_t *m = advisor_role_model();
    if (!m) m = a->model; /* app_set_advisor only builds an advisor once the role resolves */
    agent_set_model(ag, m);
    ag->thinking = model_clamp_effort(m, a->thinking);
    ad->env = tool_env_new(a->cwd);
    ad->env->settings = a->env->settings;
    agent_add_tool(ag, tool_read_new(ad->env));
    agent_add_tool(ag, tool_grep_new(ad->env));
    agent_add_tool(ag, tool_glob_new(ad->env));
    char *desc = prompt_load("coding-agent/prompts/advisor/advise-tool.md");
    json_t *s = schema_obj();
    schema_prop(s, "note", "string", "One concrete piece of advice for the agent you are watching. Terse, specific, actionable.", true);
    json_t *sev = json_obj();
    json_set_str(sev, "type", "string");
    json_set(sev, "enum", json_parse_str("[\"nit\",\"concern\",\"blocker\"]"));
    json_set_str(sev, "description", "How strongly to weigh this. Omit for a plain nit.");
    schema_prop_json(s, "severity", sev, false);
    agent_tool_t *t = agent_tool_new("advise", "Advise", desc ? desc : "Send advice to the watched agent.", s, advise_execute);
    t->ud = ad;
    agent_add_tool(ag, t);
    free(desc);
    json_t *c = json_obj();
    json_set_int(c, "max_notes_per_update", settings_int("advisor.maxNotesPerUpdate", 4));
    char *sys = prompt_render("coding-agent/prompts/advisor/system.md", c);
    json_free(c);
    json_t *parts = json_arr();
    json_push_str(parts, sys ? sys : "You review another coding agent's work and advise it via the advise tool.");
    /* project context files help the advisor judge against standing rules */
    for (size_t i = 0; i < a->disc.files.n; i++) {
        char *x = xasprintf("<context_file path=\"%s\">\n%s\n</context_file>", a->disc.files.v[i].path, a->disc.files.v[i].content);
        json_push_str(parts, x);
        free(x);
    }
    agent_set_system_prompt_parts(ag, parts);
    free(sys);
    ag->hooks.prepare_request = NULL;
    free(ag->api_key);
    ag->api_key = app_api_key_for(m);
    ag->max_turns = 8;
    ad->ag = ag;
}

static void *review_thread(void *p) {
    advisor_t *ad = p;
    pthread_mutex_lock(&ad->mu);
    for (;;) {
        while (!ad->stop && !ad->pending.len) pthread_cond_wait(&ad->cv, &ad->mu);
        if (ad->stop) break;
        char *upd = sb_detach(&ad->pending);
        ad->busy = true;
        pthread_mutex_unlock(&ad->mu);
        json_t *msgs = json_arr();
        json_push(msgs, msg_user_text(upd));
        free(upd);
        agent_prompt(ad->ag, msgs);
        pthread_mutex_lock(&ad->mu);
        ad->busy = false;
        pthread_cond_broadcast(&ad->cv);
    }
    pthread_mutex_unlock(&ad->mu);
    return NULL;
}

advisor_t *advisor_new(app_t *a) {
    advisor_t *ad = xcalloc(1, sizeof(*ad));
    ad->app = a;
    ad->immune_from = -1;
    pthread_mutex_init(&ad->mu, NULL);
    pthread_cond_init(&ad->cv, NULL);
    ad->last_count = json_len(a->agent->messages);
    build_agent(ad);
    ad->thread_live = pthread_create(&ad->th, NULL, review_thread, ad) == 0;
    return ad;
}

void advisor_free(advisor_t *ad) {
    if (!ad) return;
    pthread_mutex_lock(&ad->mu);
    ad->stop = true;
    pthread_cond_broadcast(&ad->cv);
    pthread_mutex_unlock(&ad->mu);
    agent_abort(ad->ag);
    if (ad->thread_live) pthread_join(ad->th, NULL);
    agent_free(ad->ag);
    tool_env_free(ad->env);
    for (size_t i = 0; i < ad->out.n; i++) free(ad->out.v[i].note);
    vec_free(&ad->out);
    strvec_free(&ad->given);
    sb_free(&ad->pending);
    pthread_mutex_destroy(&ad->mu);
    pthread_cond_destroy(&ad->cv);
    free(ad);
}

/* queue the transcript delta since the last review */
static void enqueue_delta(advisor_t *ad, bool wip) {
    app_t *a = ad->app;
    sb_t b = SB_INIT;
    pthread_mutex_lock(&a->agent->mu);
    size_t n = json_len(a->agent->messages);
    if (n < ad->last_count) ad->last_count = 0; /* context was rewritten (compaction, branch): replay */
    if (n > ad->last_count) render_messages(&b, a->agent->messages, ad->last_count, settings_bool("advisor.includeThinking", true));
    ad->last_count = n;
    pthread_mutex_unlock(&a->agent->mu);
    if (!b.len) {
        sb_free(&b);
        return;
    }
    pthread_mutex_lock(&ad->mu);
    if (ad->pending.len) sb_append(&ad->pending, "\n\n");
    sb_appendf(&ad->pending, "### Session update\n\n%s", sb_cstr(&b));
    if (wip) sb_append(&ad->pending, "\n\n---\n\n[in progress — more steps follow]");
    ad->updates++;
    pthread_cond_broadcast(&ad->cv);
    pthread_mutex_unlock(&ad->mu);
    sb_free(&b);
}

/* format and drain notes at or above min severity (others stay queued) */
static char *take_notes(advisor_t *ad, int min_sev, int *max_sev) {
    sb_t b = SB_INIT;
    *max_sev = -1;
    pthread_mutex_lock(&ad->mu);
    size_t w = 0;
    for (size_t i = 0; i < ad->out.n; i++) {
        anote_t *n = &ad->out.v[i];
        if (n->severity < min_sev) {
            ad->out.v[w++] = *n;
            continue;
        }
        char *esc = xml_text(n->note);
        sb_appendf(&b, "%s<advisory%s%s%s guidance=\"weigh, don't blindly obey\">\n%s\n</advisory>", b.len ? "\n" : "",
                   n->severity ? " severity=\"" : "", n->severity ? SEV[n->severity] : "", n->severity ? "\"" : "", esc);
        free(esc);
        if (n->severity > *max_sev) *max_sev = n->severity;
        strvec_push_own(&ad->given, n->note);
    }
    ad->out.n = w;
    pthread_mutex_unlock(&ad->mu);
    return b.len ? sb_detach(&b) : (sb_free(&b), NULL);
}

static void deliver_live(advisor_t *ad) {
    app_t *a = ad->app;
    bool immune = ad->immune_from >= 0 && ad->turns < ad->immune_from + (int)settings_int("advisor.immuneTurns", 3);
    int msev;
    /* concerns/blockers interrupt (steer); during the post-interrupt cooldown only blockers do */
    char *txt = take_notes(ad, immune ? 2 : 1, &msev);
    if (!txt) txt = take_notes(ad, 0, &msev); /* nits ride along at the next step boundary */
    else ad->immune_from = ad->turns;
    if (!txt) return;
    json_t *m = msg_user_text(txt);
    json_set_bool(m, "synthetic", true);
    json_set_str(m, "customType", "advisor");
    agent_steer(a->agent, m);
    if (a->notice) {
        char *s = xasprintf("Advisor: %s", txt);
        a->notice(a->notice_ud, "advisor", s);
        free(s);
    }
    free(txt);
}

void advisor_on_event(advisor_t *ad, const agent_event_t *ev) {
    if (!ad) return;
    if (ev->type == AGENT_EV_TURN_END) {
        ad->turns++;
        int every = (int)settings_int("advisor.reviewInterval", 1);
        if (str_eq(settings_str("advisor.reviewMode", "turn"), "turn") && (every <= 1 || ad->turns % every == 0)) enqueue_delta(ad, true);
        deliver_live(ad);
    }
}

char *advisor_after_run(advisor_t *ad, bool *is_continuation) {
    *is_continuation = false;
    if (!ad) return NULL;
    enqueue_delta(ad, false);
    /* wait for the final review so the run's last word is reviewed before anyone calls it done */
    pthread_mutex_lock(&ad->mu);
    int64_t deadline = mono_ms() + settings_int("advisor.finalReviewTimeoutMs", 120000);
    while ((ad->busy || ad->pending.len) && !ad->stop && mono_ms() < deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&ad->cv, &ad->mu, &ts);
    }
    pthread_mutex_unlock(&ad->mu);
    int min = sev_rank(settings_str("advisor.continueSeverity", "concern"));
    int msev;
    char *txt = take_notes(ad, min, &msev);
    if (txt) {
        *is_continuation = true;
        if (ad->app->notice) {
            char *s = xasprintf("Advisor: %s", txt);
            ad->app->notice(ad->app->notice_ud, "advisor", s);
            free(s);
        }
        return txt;
    }
    /* below the continue threshold: surface as a visible note only */
    char *rest = take_notes(ad, 0, &msev);
    if (rest && ad->app->notice) {
        char *s = xasprintf("Advisor (not sent): %s", rest);
        ad->app->notice(ad->app->notice_ud, "advisor", s);
        free(s);
    }
    free(rest);
    return NULL;
}

void advisor_reset(advisor_t *ad) {
    if (!ad) return;
    pthread_mutex_lock(&ad->mu);
    ad->last_count = json_len(ad->app->agent->messages);
    sb_clear(&ad->pending);
    pthread_mutex_unlock(&ad->mu);
}

json_t *advisor_stats(advisor_t *ad) {
    json_t *o = json_obj();
    int64_t in = 0, out = 0, cr = 0, cw = 0, ctx = 0, user = 0, asst = 0, total = 0;
    double cost = 0;
    const ai_model_t *m = ad && ad->ag ? ad->ag->model : NULL;
    if (ad && ad->ag) {
        pthread_mutex_lock(&ad->ag->mu);
        json_t *msg;
        json_arr_foreach(ad->ag->messages, i, msg) {
            const char *role = json_get_str(msg, "role");
            total++;
            if (str_eq(role, "user")) user++;
            if (!str_eq(role, "assistant")) continue;
            asst++;
            json_t *u = json_get(msg, "usage");
            in += json_get_int(u, "input", 0);
            out += json_get_int(u, "output", 0);
            cr += json_get_int(u, "cacheRead", 0);
            cw += json_get_int(u, "cacheWrite", 0);
            cost += json_get_num(json_get(u, "cost"), "total", 0);
            int64_t t = json_get_int(u, "totalTokens", 0);
            if (t > 0) ctx = t;
        }
        pthread_mutex_unlock(&ad->ag->mu);
    }
    if (m) {
        json_t *mm = json_obj();
        json_set_str(mm, "provider", m->provider);
        json_set_str(mm, "id", m->id);
        json_set(o, "model", mm);
    }
    json_set_int(o, "contextTokens", ctx);
    json_set_int(o, "contextWindow", m ? m->context_window : 0);
    json_t *ms = json_obj();
    json_set_int(ms, "user", user);
    json_set_int(ms, "assistant", asst);
    json_set_int(ms, "total", total);
    json_set(o, "messages", ms);
    json_t *tk = json_obj();
    json_set_int(tk, "input", in);
    json_set_int(tk, "output", out);
    json_set_int(tk, "cacheRead", cr);
    json_set_int(tk, "cacheWrite", cw);
    json_set(o, "tokens", tk);
    json_set_num(o, "cost", cost);
    return o;
}

struct agent *advisor_agent(advisor_t *ad) { return ad ? ad->ag : NULL; }
