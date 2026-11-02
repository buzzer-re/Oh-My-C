/* autolearn.c - experimental auto-learn: managed skills, `learn` / `manage_skill`, controller + capture. */
#include "autolearn.h"
#include "coding/xdnotice.h"

#include "coding/app.h"
#include "coding/goal.h"
#include "coding/memory.h"
#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "prompt/template.h"
#include "text/yaml.h"
#include "tools/tools.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_MANAGED_SKILL_BYTES 64000

/* ---------- managed skills (autolearn/managed-skills.ts) ---------- */

char *autolearn_managed_dir(void) { return omc_agent_path("managed-skills"); }

bool autolearn_valid_skill_name(const char *n) {
    size_t len = n ? strlen(n) : 0;
    if (len < 1 || len > 64) return false;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(alnum || (i > 0 && c == '-'))) return false;
    }
    return true;
}

char *autolearn_sanitize_skill_name(const char *raw, char **err) {
    char *t = str_trim(raw ? raw : "");
    char *n = str_lower(t);
    free(t);
    if (!autolearn_valid_skill_name(n)) {
        free(n);
        if (err)
            *err = xasprintf("Invalid skill name \"%s\". Use lowercase letters, digits, and hyphens (1-64 chars, starting "
                             "with a letter or digit).",
                             raw ? raw : "");
        return NULL;
    }
    return n;
}

/* per-name serialization of create/update/delete (in-process) */
static pthread_mutex_t g_skill_mu = PTHREAD_MUTEX_INITIALIZER;

static int assert_root_safe(char **err) {
    char *root = autolearn_managed_dir();
    struct stat st;
    int rc = 0;
    if (lstat(root, &st) == 0 && S_ISLNK(st.st_mode)) {
        struct stat t;
        if (stat(root, &t) != 0) {
            *err = xasprintf("The managed-skills root %s is a broken symlink; refusing to write.", root);
            rc = -1;
        } else if (!S_ISDIR(t.st_mode)) {
            *err = xasprintf("The managed-skills root %s links to a non-directory; refusing to write.", root);
            rc = -1;
        }
    }
    free(root);
    return rc;
}

static char *skill_frontmatter(const char *name, const char *description) {
    json_t *o = json_obj();
    json_set_str(o, "name", name);
    char *d = memory_neutralize(description);
    json_set_str(o, "description", d);
    free(d);
    char *y = yaml_dump(o);
    json_free(o);
    size_t n = strlen(y);
    while (n && (y[n - 1] == '\n' || y[n - 1] == ' ')) n--;
    char *r = xasprintf("---\n%.*s\n---\n", (int)n, y);
    free(y);
    return r;
}

int autolearn_write_skill(const char *action, const char *raw_name, const char *raw_desc, const char *raw_body,
                          char **out_path, char **err) {
    *err = NULL;
    if (out_path) *out_path = NULL;
    char *name = autolearn_sanitize_skill_name(raw_name, err);
    if (!name) return -1;
    char *desc = memory_neutralize(raw_desc ? raw_desc : "");
    char *body = str_trim(raw_body ? raw_body : "");
    int rc = -1;
    char *content = NULL, *dir = NULL, *file = NULL;
    if (!*desc) {
        *err = xasprintf("Managed skill \"%s\" needs a non-empty description.", name);
        goto out;
    }
    if (!*body) {
        *err = xasprintf("Managed skill \"%s\" needs a non-empty body.", name);
        goto out;
    }
    char *fm = skill_frontmatter(name, desc);
    content = xasprintf("%s\n%s\n", fm, body);
    free(fm);
    size_t bytes = strlen(content);
    if (bytes > MAX_MANAGED_SKILL_BYTES) {
        *err = xasprintf("Managed skill is %zu bytes; the limit is %d. Trim the body or description.", bytes,
                         MAX_MANAGED_SKILL_BYTES);
        goto out;
    }
    pthread_mutex_lock(&g_skill_mu);
    if (assert_root_safe(err) != 0) goto unlock;
    char *root = autolearn_managed_dir();
    dir = path_join(root, name);
    free(root);
    file = path_join(dir, "SKILL.md");
    struct stat st;
    if (lstat(dir, &st) == 0 && S_ISLNK(st.st_mode)) {
        *err = xasprintf("Managed skill \"%s\" resolves through a symlink; refusing to write outside the managed directory.", name);
        goto unlock;
    }
    if (!strcmp(action, "create")) {
        fs_mkdir_p(dir, 0755);
        int fd = open(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) {
            if (errno == EEXIST) *err = xasprintf("Managed skill \"%s\" already exists. Use action \"update\" to change it.", name);
            else *err = xasprintf("%s: %s", file, strerror(errno));
            goto unlock;
        }
        ssize_t w = write(fd, content, bytes);
        close(fd);
        if (w != (ssize_t)bytes) {
            *err = xasprintf("%s: short write", file);
            goto unlock;
        }
        rc = 0;
        goto unlock;
    }
    if (lstat(file, &st) != 0) {
        *err = xasprintf("Managed skill \"%s\" does not exist. Use action \"create\" to add it.", name);
        goto unlock;
    }
    if (S_ISLNK(st.st_mode)) {
        *err = xasprintf("Managed skill \"%s\" SKILL.md is a symlink; refusing to overwrite it.", name);
        goto unlock;
    }
    if (!S_ISREG(st.st_mode)) {
        *err = xasprintf("Managed skill \"%s\" SKILL.md is not a regular file; refusing to overwrite it.", name);
        goto unlock;
    }
    if (st.st_nlink > 1) {
        *err = xasprintf("Managed skill \"%s\" SKILL.md has %lu hard links; refusing to overwrite a file that may be "
                         "user-authored elsewhere.",
                         name, (unsigned long)st.st_nlink);
        goto unlock;
    }
    int fd = open(file, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ELOOP) *err = xasprintf("Managed skill \"%s\" SKILL.md is a symlink; refusing to overwrite it.", name);
        else *err = xasprintf("%s: %s", file, strerror(errno));
        goto unlock;
    }
    struct stat ost;
    if (fstat(fd, &ost) != 0 || !S_ISREG(ost.st_mode) || ost.st_nlink > 1) {
        close(fd);
        *err = xasprintf("Managed skill \"%s\" SKILL.md is not a regular file; refusing to overwrite it.", name);
        goto unlock;
    }
    if (ftruncate(fd, 0) != 0 || write(fd, content, bytes) != (ssize_t)bytes) {
        close(fd);
        *err = xasprintf("%s: %s", file, strerror(errno));
        goto unlock;
    }
    close(fd);
    rc = 0;
unlock:
    pthread_mutex_unlock(&g_skill_mu);
out:
    if (rc == 0 && out_path) {
        *out_path = file;
        file = NULL;
    }
    free(file);
    free(dir);
    free(content);
    free(name);
    free(desc);
    free(body);
    return rc;
}

int autolearn_delete_skill(const char *raw, char **err) {
    *err = NULL;
    char *name = autolearn_sanitize_skill_name(raw, err);
    if (!name) return -1;
    int rc = -1;
    pthread_mutex_lock(&g_skill_mu);
    if (assert_root_safe(err) == 0) {
        char *root = autolearn_managed_dir();
        char *dir = path_join(root, name);
        free(root);
        struct stat st;
        if (lstat(dir, &st) != 0) *err = xasprintf("Managed skill \"%s\" does not exist.", name);
        else if (S_ISLNK(st.st_mode))
            *err = xasprintf("Managed skill \"%s\" is a symlink; refusing to delete outside the managed directory.", name);
        else if (fs_rm_rf(dir) != 0) *err = xasprintf("%s: %s", dir, strerror(errno));
        else rc = 0;
        free(dir);
    }
    pthread_mutex_unlock(&g_skill_mu);
    free(name);
    return rc;
}

/* ---------- session skill refresh (session.refreshSkills) ---------- */

typedef struct graveyard {
    discovery_t disc;
    struct graveyard *next;
} graveyard_t;

struct autolearn {
    app_t *app;
    pthread_mutex_t mu;
    int tool_calls;
    bool turn_started_in_goal;
    /* private capture */
    pthread_t th;
    bool th_running, in_flight, pending;
    agent_t *capture;
    volatile sig_atomic_t stop;
    graveyard_t *dead; /* replaced skill lists: concurrent skill:// readers may still hold pointers */
};

static void refresh_skills(app_t *a) {
    if (a->opts.no_skills) return;
    discovery_t fresh = {0};
    discover_all(a->cwd, &fresh, true, false, true);
    pthread_mutex_lock(&a->mu);
    graveyard_t *g = xcalloc(1, sizeof *g);
    g->disc.skills = a->disc.skills;
    a->disc.skills = fresh.skills;
    fresh.skills.v = NULL;
    fresh.skills.n = fresh.skills.cap = 0;
    if (a->autolearn) {
        g->next = a->autolearn->dead;
        a->autolearn->dead = g;
    } else {
        discovery_free(&g->disc);
        free(g);
    }
    pthread_mutex_unlock(&a->mu);
    discovery_free(&fresh);
    app_rebuild_system_prompt(a);
}

static bool claimed_by_authored(app_t *a, const char *name) {
    for (size_t i = 0; i < a->disc.skills.n; i++)
        if (!strcmp(a->disc.skills.v[i].name, name) && !a->disc.skills.v[i].managed) return true;
    return false;
}

/* ---------- tools ---------- */

static const char *LEARN_SCHEMA =
    "{\"type\":\"object\",\"properties\":{"
    "\"memory\":{\"type\":\"string\",\"description\":\"the durable, self-contained lesson to remember (what, when, why)\"},"
    "\"context\":{\"type\":\"string\",\"description\":\"optional source context for the lesson\"},"
    "\"skill\":{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"enum\":[\"create\",\"update\"],\"type\":\"string\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"kebab-case skill name\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"one-line description of when to use the skill\"},"
    "\"body\":{\"type\":\"string\",\"description\":\"the SKILL.md body in markdown (no frontmatter)\"}},"
    "\"required\":[\"action\",\"name\",\"description\",\"body\"],"
    "\"description\":\"also create or enhance a managed skill in the same call\",\"additionalProperties\":false}},"
    "\"required\":[\"memory\"],\"additionalProperties\":false}";

static const char *MANAGE_SKILL_SCHEMA =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"enum\":[\"create\",\"update\",\"delete\"],\"type\":\"string\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"kebab-case skill name\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"one-line description of when to use the skill (required for create/update)\"},"
    "\"body\":{\"type\":\"string\",\"description\":\"the SKILL.md body in markdown, no frontmatter (required for create/update)\"}},"
    "\"required\":[\"action\",\"name\"],\"additionalProperties\":false}";

static app_t *tool_app(tool_ctx_t *ctx, agent_tool_t *self) {
    tool_env_t *env = self->ud;
    return ctx && ctx->app ? ctx->app : env ? env->session : NULL;
}

static int learn_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    app_t *a = tool_app(ctx, self);
    const char *backend = memory_backend();
    if (str_eq(json_get_str(args, "scope"), "global") && strcmp(backend, "mnemopi") != 0) {
        tool_result_error(out, "Global memory scope is only available with the Mnemopi backend.");
        return 0;
    }
    const char *memory_message = "Lesson stored";
    if (!strcmp(backend, "local")) {
        char *msg = NULL;
        int stored = memory_save_lesson(omc_agent_dir(), a->cwd, json_get_str(args, "memory"), json_get_str(args, "context"), &msg);
        free(msg);
        if (stored <= 0) {
            tool_result_error(out, "Lesson was empty after sanitization; nothing stored.");
            return 0;
        }
    } else if (!strcmp(backend, "mnemopi")) {
        tool_result_error(out, "Mnemopi backend is not initialised for this session.");
        return 0;
    } else {
        tool_result_error(out, "Hindsight backend is not initialised for this session.");
        return 0;
    }
    const json_t *skill = json_get_obj(args, "skill");
    if (skill) {
        const char *sname = json_get_str(skill, "name");
        const char *action = json_get_str(skill, "action");
        char *safe = autolearn_sanitize_skill_name(sname, NULL);
        if (str_eq(action, "create") && safe && claimed_by_authored(a, safe)) {
            tool_result_error(out,
                              "%s. Did not create managed skill \"%s\": an authored skill of that name already exists, and "
                              "managed skills cannot override authored ones. Choose a different name.",
                              memory_message, sname);
            out->details = json_obj();
            json_set(out->details, "skill", json_null());
            json_set_bool(out->details, "shadowed", true);
            free(safe);
            return 0;
        }
        free(safe);
        char *err = NULL;
        if (autolearn_write_skill(action ? action : "create", sname, json_get_str(skill, "description"),
                                  json_get_str(skill, "body"), NULL, &err) != 0) {
            tool_result_error(out, "%s, but the managed skill could not be written: %s", memory_message, err);
            free(err);
            return 0;
        }
        refresh_skills(a);
        tool_result_textf(out, "%s. %s managed skill \"%s\".", memory_message, str_eq(action, "create") ? "Created" : "Updated", sname);
        out->details = json_obj();
        json_set_str(out->details, "skill", sname);
        return 0;
    }
    tool_result_textf(out, "%s.", memory_message);
    out->details = json_obj();
    json_set(out->details, "skill", json_null());
    return 0;
}

static int manage_skill_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    app_t *a = tool_app(ctx, self);
    const char *action = json_get_str(args, "action");
    const char *name = json_get_str(args, "name");
    char *err = NULL;
    out->details = json_obj();
    json_set_str(out->details, "action", action);
    json_set_str(out->details, "name", name);
    if (str_eq(action, "delete")) {
        if (autolearn_delete_skill(name, &err) != 0) {
            tool_result_error(out, "%s", err);
            free(err);
            return 0;
        }
        refresh_skills(a);
        tool_result_textf(out, "Deleted managed skill \"%s\".", name);
        return 0;
    }
    const char *desc = json_get_str(args, "description"), *body = json_get_str(args, "body");
    if (!desc || !body) {
        tool_result_error(out, "\"%s\" requires both \"description\" and \"body\".", action);
        return 0;
    }
    char *safe = autolearn_sanitize_skill_name(name, &err);
    if (!safe) {
        tool_result_error(out, "%s", err);
        free(err);
        return 0;
    }
    if (str_eq(action, "create") && claimed_by_authored(a, safe)) {
        tool_result_error(out,
                          "Cannot create managed skill \"%s\": an authored skill of that name already exists, and managed "
                          "skills cannot override authored ones. Choose a different name.",
                          name);
        json_set_bool(out->details, "shadowed", true);
        free(safe);
        return 0;
    }
    free(safe);
    char *path = NULL;
    if (autolearn_write_skill(action, name, desc, body, &path, &err) != 0) {
        tool_result_error(out, "%s", err);
        free(err);
        return 0;
    }
    refresh_skills(a);
    char *root = autolearn_managed_dir();
    char *rel = path_relative(root, path);
    tool_result_textf(out, "%s managed skill \"%s\" (managed-skills/%s).", str_eq(action, "create") ? "Created" : "Updated", name, rel);
    free(rel);
    free(root);
    free(path);
    return 0;
}

static agent_tool_t *learn_tool_new(tool_env_t *env) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "globalScope", false); /* only the (unported) Mnemopi backend has a shared bank */
    char *desc = tool_prompt("learn", ctx);
    json_free(ctx);
    agent_tool_t *t = agent_tool_new("learn", "Learn", desc ? desc : "", schema_lit(LEARN_SCHEMA), learn_execute);
    free(desc);
    t->summary = xstrdup("Capture a reusable lesson to memory (and optionally a managed skill)");
    t->ud = env;
    return t;
}

static agent_tool_t *manage_skill_tool_new(tool_env_t *env) {
    /* readonly description = the raw markdown import (not rendered) */
    size_t len = 0;
    const char *raw = embed_get("prompts/coding-agent/prompts/tools/manage-skill.md", &len);
    char *desc = raw ? xstrndup(raw, len) : tool_prompt("manage-skill", NULL);
    agent_tool_t *t = agent_tool_new("manage_skill", "Manage Skill", desc ? desc : "", schema_lit(MANAGE_SKILL_SCHEMA),
                                     manage_skill_execute);
    free(desc);
    t->summary = xstrdup("Create, update, or delete an isolated managed skill");
    t->ud = env;
    return t;
}

void autolearn_register_tools(app_t *a, agent_t *ag) {
    if (!settings_bool("autolearn.enabled", false)) return;
    const strvec_t *req = a->opts.tools;
    bool top = !a->opts.subagent;
    /* allowed: top-level, or an explicit tool list (tools/index.ts isToolAllowed) */
    if (!top && !req) return;
    bool want_ms = !req || strvec_contains(req, "manage_skill") || top;
    if (want_ms) agent_add_tool(ag, manage_skill_tool_new(a->env));
    const char *b = memory_backend();
    /* hindsight / mnemopi are not ported: omc treats them like "off" (no learn tool) */
    if (!strcmp(b, "local") && (!req || strvec_contains(req, "learn") || top)) agent_add_tool(ag, learn_tool_new(a->env));
}

void autolearn_reconcile(app_t *a) {
    if (!a || !a->agent || a->opts.no_tools || agent_is_running(a->agent)) return;
    agent_remove_tool(a->agent, "learn");
    agent_remove_tool(a->agent, "manage_skill");
    autolearn_register_tools(a, a->agent);
    tools_finalize(a->agent, a->opts.tools);
    memory_start(a);
    app_rebuild_system_prompt(a);
    xdnotice_sync(a, NULL);
}

char *autolearn_instructions(agent_t *ag) {
    bool ms = false, learn = false;
    for (size_t i = 0; i < ag->tools.n; i++) {
        if (!strcmp(ag->tools.v[i]->name, "manage_skill")) ms = true;
        if (!strcmp(ag->tools.v[i]->name, "learn")) learn = true;
    }
    if (!ms) return NULL;
    char *g = prompt_load("coding-agent/prompts/system/autolearn-guidance.md");
    char *gt = str_trim(g ? g : "");
    free(g);
    if (!learn) return gt;
    char *l = prompt_load("coding-agent/prompts/system/autolearn-guidance-learn.md");
    char *lt = str_trim(l ? l : "");
    free(l);
    char *r = xasprintf("%s\n\n%s", gt, lt);
    free(gt);
    free(lt);
    return r;
}

/* ---------- controller + private capture (autolearn/controller.ts, sdk.ts createAutoLearnCaptureRunner) ---------- */

autolearn_t *autolearn_new(app_t *a) {
    autolearn_t *al = xcalloc(1, sizeof *al);
    al->app = a;
    pthread_mutex_init(&al->mu, NULL);
    return al;
}

static json_t *capture_messages(app_t *a) {
    pthread_mutex_lock(&a->agent->mu);
    json_t *src = json_clone(a->agent->messages);
    pthread_mutex_unlock(&a->agent->mu);
    json_t *m;
    json_arr_foreach(src, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            json_del(m, "responseId");
            json_del(m, "providerPayload");
        } else if (str_eq(role, "user") || str_eq(role, "developer")) {
            json_del(m, "providerPayload");
        }
    }
    return src;
}

static void *capture_thread(void *p) {
    autolearn_t *al = p;
    app_t *a = al->app;
    for (;;) {
        agent_t *ag = agent_new();
        ag->app = a;
        agent_set_model(ag, a->model);
        ag->thinking = a->thinking;
        pthread_mutex_lock(&a->agent->mu);
        agent_set_system_prompt_parts(ag, json_clone(a->agent->system_prompt));
        pthread_mutex_unlock(&a->agent->mu);
        ag->hooks.prepare_request = a->agent->hooks.prepare_request;
        ag->hooks.transform_context = a->agent->hooks.transform_context;
        ag->hooks.ud = a;
        free(ag->session_id);
        ag->session_id = uuid_v7();
        /* capture tools resolve per run, so live autolearn.enabled / backend changes apply */
        if (settings_bool("autolearn.enabled", false)) {
            agent_add_tool(ag, manage_skill_tool_new(a->env));
            if (str_eq(memory_backend(), "local")) agent_add_tool(ag, learn_tool_new(a->env));
        }
        if (ag->tools.n && a->model && !al->stop) {
            agent_replace_messages(ag, capture_messages(a));
            char *nudge = prompt_load("coding-agent/prompts/system/autolearn-nudge-autocontinue.md");
            char *nt = str_trim(nudge ? nudge : "");
            free(nudge);
            json_t *msgs = json_arr();
            json_t *m = msg_user_text(nt);
            json_set_bool(m, "synthetic", true);
            json_set_str(m, "customType", "autolearn-nudge");
            json_push(msgs, m);
            free(nt);
            pthread_mutex_lock(&al->mu);
            al->capture = ag;
            bool stop = al->stop;
            pthread_mutex_unlock(&al->mu);
            if (!stop) agent_prompt(ag, msgs);
            else json_free(msgs);
            pthread_mutex_lock(&al->mu);
            al->capture = NULL;
            pthread_mutex_unlock(&al->mu);
        }
        agent_free(ag);
        pthread_mutex_lock(&al->mu);
        bool again = al->pending && !al->stop;
        al->pending = false;
        if (!again) al->in_flight = false;
        pthread_mutex_unlock(&al->mu);
        if (!again) break;
    }
    return NULL;
}

static void start_capture(autolearn_t *al) {
    /* called with al->mu held */
    if (al->th_running) {
        pthread_mutex_unlock(&al->mu);
        pthread_join(al->th, NULL);
        pthread_mutex_lock(&al->mu);
        al->th_running = false;
    }
    al->in_flight = true;
    if (pthread_create(&al->th, NULL, capture_thread, al) == 0) al->th_running = true;
    else al->in_flight = false;
}

void autolearn_on_event(autolearn_t *al, const agent_event_t *ev) {
    if (!al) return;
    app_t *a = al->app;
    if (ev->type == AGENT_EV_AGENT_START) {
        al->turn_started_in_goal = a->goal && goal_enabled(a->goal);
        return;
    }
    if (ev->type == AGENT_EV_TOOL_END) {
        __atomic_add_fetch(&al->tool_calls, 1, __ATOMIC_RELAXED);
        return;
    }
    if (ev->type != AGENT_EV_AGENT_END) return;
    int calls = __atomic_exchange_n(&al->tool_calls, 0, __ATOMIC_RELAXED);
    bool started_goal = al->turn_started_in_goal;
    al->turn_started_in_goal = false;
    const json_t *last = last_assistant(ev->messages);
    if (last && str_eq(json_get_str(last, "stopReason"), "aborted")) return;
    if (!settings_bool("autolearn.enabled", false)) return;
    if (calls < settings_int("autolearn.minToolCalls", 5)) return;
    if (a->plan_mode) return;
    if (started_goal || (a->goal && goal_enabled(a->goal))) return;
    if (!settings_bool("autolearn.autoContinue", false)) return;
    pthread_mutex_lock(&al->mu);
    if (al->in_flight) al->pending = true;
    else if (!al->stop) start_capture(al);
    pthread_mutex_unlock(&al->mu);
}

void autolearn_wait(autolearn_t *al, int64_t timeout_ms) {
    if (!al) return;
    int64_t end = mono_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&al->mu);
        bool busy = al->in_flight;
        pthread_mutex_unlock(&al->mu);
        if (!busy || mono_ms() >= end) return;
        sleep_ms(20);
    }
}

void autolearn_free(autolearn_t *al) {
    if (!al) return;
    pthread_mutex_lock(&al->mu);
    al->stop = 1;
    al->pending = false;
    if (al->capture) agent_abort(al->capture);
    bool join = al->th_running;
    pthread_mutex_unlock(&al->mu);
    if (join) pthread_join(al->th, NULL);
    while (al->dead) {
        graveyard_t *g = al->dead;
        al->dead = g->next;
        discovery_free(&g->disc);
        free(g);
    }
    pthread_mutex_destroy(&al->mu);
    free(al);
}
