/* omc.c - public library API (include/omc.h) over the app runtime and the embeddable RPC endpoint. */
#include "../../include/omc.h"

#include "ai/ai.h"
#include "coding/skilldesc.h"
#include "coding/app.h"
#include "config/auth.h"
#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "modes/modes.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

struct omc_session {
    app_t *app;
    rpc_t *rpc;
    omc_event_fn on_event;
    void *ud;
    pthread_mutex_t mu;
    bool prompting; /* omc_prompt in progress */
};

const char *omc_version(void) { return OMC_VERSION; }
int omc_api_version(void) { return OMC_API_VERSION; }
void omc_free(void *p) { free(p); }

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static char *g_init_cwd;

static void global_init(void) {
    proc_ignore_sigpipe();
    settings_init(g_init_cwd, NULL);
    catalog_init();
    auth_init();
}

static void sink(void *ud, const json_t *msg) {
    omc_session_t *s = ud;
    if (!s->on_event) return;
    char *txt = json_dump(msg, 0);
    s->on_event(s->ud, txt);
    free(txt);
}

omc_session_t *omc_session_new(const omc_options_t *o, omc_event_fn on_event, void *ud, char **error) {
    omc_options_t def = {.struct_size = sizeof(omc_options_t)};
    omc_options_t opt = def;
    if (o) memcpy(&opt, o, o->struct_size && o->struct_size < sizeof opt ? o->struct_size : sizeof opt);
    if (error) *error = NULL;
    char *cwd = opt.cwd ? path_abs(opt.cwd, NULL) : get_cwd();
    if (!fs_is_dir(cwd)) {
        if (error) *error = xasprintf("cwd is not a directory: %s", cwd);
        free(cwd);
        return NULL;
    }
    if (opt.agent_dir && !env_get("OMC_CODING_AGENT_DIR")) setenv("OMC_CODING_AGENT_DIR", opt.agent_dir, 1);
    if (!g_init_cwd) g_init_cwd = xstrdup(cwd);
    pthread_once(&g_once, global_init);

    strvec_t tools = {0};
    for (const char *const *t = opt.tools; t && *t; t++) strvec_push(&tools, *t);
    app_opts_t ao = {.cwd = cwd,
                     .model = opt.model,
                     .provider = opt.provider,
                     .api_key = opt.api_key,
                     .thinking = opt.thinking,
                     .session_dir = opt.session_dir,
                     .resume = opt.resume,
                     .no_session = opt.no_session,
                     .system_prompt = opt.system_prompt,
                     .append_system_prompt = opt.append_system_prompt,
                     .tools = opt.tools ? &tools : NULL,
                     .no_tools = opt.no_tools,
                     .no_context_files = opt.no_context_files,
                     .no_skills = opt.no_skills,
                     .no_rules = opt.no_rules,
                     .no_mcp = opt.no_mcp};
    char *err = NULL;
    app_t *app = app_create(&ao, &err);
    if (app) skilldesc_start_compression(app->skilldesc);
    strvec_free(&tools);
    free(cwd);
    if (!app) {
        if (error) *error = err ? err : xstrdup("failed to create session");
        else free(err);
        return NULL;
    }
    if (opt.plan_mode) app_set_plan_mode(app, true);
    omc_session_t *s = xcalloc(1, sizeof(*s));
    s->app = app;
    s->on_event = on_event;
    s->ud = ud;
    pthread_mutex_init(&s->mu, NULL);
    s->rpc = rpc_attach(app, sink, s);
    return s;
}

void omc_session_free(omc_session_t *s) {
    if (!s) return;
    if (omc_busy(s)) omc_abort(s);
    omc_wait_idle(s, -1);
    rpc_detach(s->rpc);
    app_free(s->app);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

const char *omc_session_id(omc_session_t *s) { return s && s->app->session ? session_id(s->app->session) : NULL; }

int omc_send(omc_session_t *s, const char *command_json) {
    if (!s || !command_json) return -1;
    json_t *cmd = json_parse_str(command_json);
    if (!json_is_obj(cmd)) {
        json_free(cmd);
        rpc_handle_line(s->rpc, command_json); /* reports the parse error through on_event */
        return -1;
    }
    rpc_handle_json(s->rpc, cmd);
    json_free(cmd);
    return 0;
}

bool omc_busy(omc_session_t *s) {
    if (!s) return false;
    pthread_mutex_lock(&s->mu);
    bool p = s->prompting;
    pthread_mutex_unlock(&s->mu);
    return p || rpc_busy(s->rpc) || agent_is_running(s->app->agent);
}

int omc_prompt(omc_session_t *s, const char *text, char **answer) {
    if (answer) *answer = NULL;
    if (!s || !text) return -1;
    pthread_mutex_lock(&s->mu);
    if (s->prompting || rpc_busy(s->rpc) || agent_is_running(s->app->agent)) {
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    s->prompting = true;
    pthread_mutex_unlock(&s->mu);
    bool refused = app_prompt(s->app, text, NULL) == APP_PROMPT_REFUSED;
    const json_t *last = last_assistant(s->app->agent->messages);
    const char *stop = json_get_str(last, "stopReason");
    int rc = refused || str_eq(stop, "error") || str_eq(stop, "aborted") ? 1 : 0;
    if (answer) *answer = last ? ai_message_text(last) : xstrdup("");
    pthread_mutex_lock(&s->mu);
    s->prompting = false;
    pthread_mutex_unlock(&s->mu);
    return rc;
}

void omc_abort(omc_session_t *s) {
    if (s) app_abort(s->app);
}

int omc_wait_idle(omc_session_t *s, int timeout_ms) {
    if (!s) return 0;
    int64_t deadline = timeout_ms < 0 ? 0 : now_ms() + timeout_ms;
    while (omc_busy(s)) {
        if (deadline && now_ms() >= deadline) return 1;
        struct timespec ts = {0, 10 * 1000000L};
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* ---------- host tools ---------- */

typedef struct {
    omc_tool_fn fn;
    void *ud;
} host_tool_t;

static int lib_tool_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    host_tool_t *h = self->ud;
    UNUSED(ctx);
    char *a = json_dump(args ? args : json_obj(), 0);
    bool is_err = false;
    char *r = h->fn(h->ud, a, &is_err);
    free(a);
    tool_result_text(out, r ? r : "");
    out->is_error = is_err;
    free(r);
    return 0;
}

int omc_register_tool(omc_session_t *s, const char *name, const char *description, const char *parameters_json, omc_tool_fn fn,
                      void *ud) {
    if (!s || !name || !*name || !fn) return -1;
    json_t *params = parameters_json ? json_parse_str(parameters_json) : json_parse_str("{\"type\":\"object\",\"properties\":{}}");
    if (!json_is_obj(params)) {
        json_free(params);
        return -1;
    }
    if (omc_busy(s)) {
        json_free(params);
        return -1; /* the tool set is fixed while a turn runs */
    }
    if (agent_find_tool(s->app->agent, name)) agent_remove_tool(s->app->agent, name);
    agent_tool_t *t = agent_tool_new(name, name, description ? description : "", params, lib_tool_execute);
    host_tool_t *h = xcalloc(1, sizeof(*h)); /* lives as long as the process: tools may be re-registered */
    h->fn = fn;
    h->ud = ud;
    t->ud = h;
    agent_add_tool(s->app->agent, t);
    app_rebuild_system_prompt(s->app);
    return 0;
}
