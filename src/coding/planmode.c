/* planmode.c - plan mode state, plan files and approval helpers (ports of omp plan-mode/approved-plan.ts,
 * plan-files.ts, plan-autosave.ts, the plan-mode parts of AgentSession / InteractiveMode and
 * tools/resolve.ts's propose dispatch, plus model-controls.ts getRoleModelCycle / applyRoleModel). */
#include "coding/app.h"

#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#define DEFAULT_PLAN_URL "local://PLAN.md"

/* ---------- titles (approved-plan.ts) ---------- */

char *app_plan_normalize_title(const char *title, char **err) {
    char *t = str_trim(title ? title : "");
    if (!*t) {
        free(t);
        if (err) *err = xstrdup("Plan title is required and must not be empty.");
        return NULL;
    }
    if (strchr(t, '/') || strchr(t, '\\') || strstr(t, "..")) {
        free(t);
        if (err) *err = xstrdup("Plan title must not contain path separators or '..'.");
        return NULL;
    }
    size_t n = strlen(t);
    if (n >= 3 && t[n - 3] == '.' && tolower((unsigned char)t[n - 2]) == 'm' && tolower((unsigned char)t[n - 1]) == 'd')
        t[n - 3] = 0;
    /* \s+ -> "-", [^A-Za-z0-9_-] dropped, -{2,} -> "-", edge hyphens trimmed */
    sb_t b = SB_INIT;
    for (const char *p = t; *p;) {
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*p)) p++;
            sb_putc(&b, '-');
            continue;
        }
        if (isalnum((unsigned char)*p) || *p == '_' || *p == '-') sb_putc(&b, *p);
        p++;
    }
    free(t);
    sb_t c = SB_INIT;
    const char *s = sb_cstr(&b);
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '-' && c.len && c.s[c.len - 1] == '-') continue;
        sb_putc(&c, s[i]);
    }
    sb_free(&b);
    char *r = sb_detach(&c);
    size_t st = 0, en = strlen(r);
    while (st < en && r[st] == '-') st++;
    while (en > st && r[en - 1] == '-') en--;
    char *out = xstrndup(r + st, en - st);
    free(r);
    if (!*out) {
        free(out);
        if (err)
            *err = xstrdup("Plan title must contain at least one letter, number, underscore, or hyphen after sanitization.");
        return NULL;
    }
    return out;
}

/* first `# Heading` on its own line */
static char *first_h1(const char *content) {
    for (const char *l = content; l && *l;) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        size_t i = 0;
        while (i < len && (l[i] == ' ' || l[i] == '\t')) i++;
        if (i < len && l[i] == '#' && i + 1 < len && (l[i + 1] == ' ' || l[i + 1] == '\t')) {
            char *h = xstrndup(l + i + 1, len - i - 1);
            char *t = str_trim(h);
            free(h);
            if (*t) return t;
            free(t);
        }
        l = e ? e + 1 : NULL;
    }
    return xstrdup("");
}

static char *filename_stem(const char *url) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strrchr(p, '/'), *bs = strrchr(p, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
    const char *base = slash ? slash + 1 : p;
    char *s = xstrdup(base);
    size_t n = strlen(s);
    if (n >= 3 && s[n - 3] == '.' && tolower((unsigned char)s[n - 2]) == 'm' && tolower((unsigned char)s[n - 1]) == 'd')
        s[n - 3] = 0;
    return s;
}

char *app_plan_resolve_title(const char *supplied, const char *content, const char *path) {
    char *cands[3] = {supplied ? str_trim(supplied) : xstrdup(""), first_h1(content ? content : ""),
                      filename_stem(path ? path : "")};
    char *r = NULL;
    for (int i = 0; i < 3 && !r; i++)
        if (*cands[i]) r = app_plan_normalize_title(cands[i], NULL);
    for (int i = 0; i < 3; i++) free(cands[i]);
    return r ? r : xstrdup("plan");
}

char *app_plan_humanize_title(const char *title) {
    sb_t b = SB_INIT;
    for (const char *p = title; *p;) {
        if (*p == '-' || *p == '_') {
            while (*p == '-' || *p == '_') p++;
            sb_putc(&b, ' ');
            continue;
        }
        sb_putc(&b, *p++);
    }
    char *t = str_trim(sb_cstr(&b));
    sb_free(&b);
    if (*t) t[0] = (char)toupper((unsigned char)t[0]);
    return t;
}

/* ---------- plan files (plan-files.ts) ---------- */

char *app_plan_path(app_t *a, const char *url) { return tool_resolve_path(a->env, url); }

char *app_plan_read(app_t *a, const char *url) {
    char *abs = app_plan_path(a, url);
    size_t n;
    char *c = fs_is_file(abs) ? fs_read(abs, &n) : NULL;
    free(abs);
    return c;
}

int app_plan_write(app_t *a, const char *url, const char *content) {
    char *abs = app_plan_path(a, url);
    int rc = fs_write_atomic(abs, content, strlen(content), 0644);
    free(abs);
    return rc;
}

typedef struct {
    char *url;
    int64_t mtime;
} plan_entry_t;

static int cmp_mtime(const void *x, const void *y) {
    const plan_entry_t *a = x, *b = y;
    return a->mtime < b->mtime ? 1 : a->mtime > b->mtime ? -1 : 0;
}

strvec_t app_plan_list(app_t *a) {
    strvec_t out = {0};
    const char *root = a->env->local_dir;
    DIR *d = root ? opendir(root) : NULL;
    if (!d) return out;
    VEC(plan_entry_t) v = {0};
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t n = strlen(de->d_name);
        if (n < 7 || strcasecmp(de->d_name + n - 7, "plan.md") != 0) continue;
        char *p = path_join(root, de->d_name);
        if (fs_is_file(p)) vec_push(&v, ((plan_entry_t){xasprintf("local://%s", de->d_name), fs_mtime_ms(p)}));
        free(p);
    }
    closedir(d);
    if (v.n) qsort(v.v, v.n, sizeof *v.v, cmp_mtime);
    for (size_t i = 0; i < v.n; i++) strvec_push_own(&out, v.v[i].url);
    vec_free(&v);
    return out;
}

static char *canonical_local(const char *url) {
    /* InternalUrlRouter.normalize: local:/x and local://x name the same file */
    if (str_starts(url, "local:") && !str_starts(url, "local://")) {
        const char *r = url + 6;
        while (*r == '/') r++;
        return xasprintf("local://%s", r);
    }
    return xstrdup(url);
}

/* resolveApprovedPlan: supplied slug, a state plan the scan cannot see, scanned plans newest first, the
 * state plan; the first that exists wins */
int app_plan_resolve(app_t *a, const char *supplied, char **path, char **content, char **title, char **err) {
    strvec_t ordered = {0};
#define CONSIDER(u)                                                                                                    \
    do {                                                                                                               \
        const char *u_ = (u);                                                                                          \
        if (u_ && *u_ && !strvec_contains(&ordered, u_)) strvec_push(&ordered, u_);                                    \
    } while (0)
    if (supplied && *supplied) {
        char *norm = app_plan_normalize_title(supplied, NULL);
        if (norm) {
            size_t n = strlen(norm);
            if (n >= 5 && strcasecmp(norm + n - 5, "-plan") == 0 && n > 5) norm[n - 5] = 0;
            char *u = xasprintf("local://%s-plan.md", norm);
            CONSIDER(u);
            free(u);
            free(norm);
        }
    }
    strvec_t listed = app_plan_list(a);
    const char *state = a->plan_file ? a->plan_file : DEFAULT_PLAN_URL;
    char *cs = canonical_local(state);
    bool in_listed = false;
    for (size_t i = 0; i < listed.n; i++)
        if (!strcmp(listed.v[i], cs)) in_listed = true;
    free(cs);
    if (!in_listed) CONSIDER(state);
    for (size_t i = 0; i < listed.n; i++) CONSIDER(listed.v[i]);
    CONSIDER(state);
#undef CONSIDER
    strvec_free(&listed);
    for (size_t i = 0; i < ordered.n; i++) {
        char *c = app_plan_read(a, ordered.v[i]);
        if (!c) continue;
        *path = xstrdup(ordered.v[i]);
        *content = c;
        *title = app_plan_resolve_title(supplied, c, ordered.v[i]);
        strvec_free(&ordered);
        return 0;
    }
    const char *target = ordered.n ? ordered.v[0] : state;
    *err = xasprintf("Plan file not found at %s. Write the finalized plan to %s before requesting approval.", target, target);
    strvec_free(&ordered);
    return -1;
}

/* ---------- autosave (plan-autosave.ts) ---------- */

char *app_plan_save_file_name(const char *title) {
    /* NFC, non letter/digit runs -> "_", trim "_", upper case; <= 32 chars cut at a word boundary */
    sb_t b = SB_INIT;
    const unsigned char *p = (const unsigned char *)(title ? title : "");
    while (*p) {
        if (*p >= 0x80) { /* keep UTF-8 letters as they are */
            sb_putc(&b, (char)*p++);
            while (*p && (*p & 0xC0) == 0x80) sb_putc(&b, (char)*p++);
            continue;
        }
        if (isalnum(*p)) {
            sb_putc(&b, (char)toupper(*p++));
            continue;
        }
        if (!b.len || b.s[b.len - 1] != '_') sb_putc(&b, '_');
        p++;
    }
    char *s = sb_detach(&b);
    size_t st = 0, en = strlen(s);
    while (st < en && s[st] == '_') st++;
    while (en > st && s[en - 1] == '_') en--;
    char *stem = xstrndup(s + st, en - st);
    free(s);
    if (strlen(stem) > 32) {
        int cut = -1;
        for (int i = 32; i > 0; i--)
            if (stem[i] == '_') {
                cut = i;
                break;
            }
        stem[cut > 0 ? cut : 32] = 0;
    }
    char *r;
    if (!*stem || !strcmp(stem, "PLAN")) r = xstrdup("PLAN.md");
    else {
        size_t n = strlen(stem);
        r = n >= 5 && !strcmp(stem + n - 5, "_PLAN") ? xasprintf("%s.md", stem) : xasprintf("%s_PLAN.md", stem);
    }
    free(stem);
    return r;
}

char *app_plan_autosave(app_t *a, const char *title, const char *content, char **err) {
    if (!settings_bool("plan.autosave", false)) return NULL;
    bool blank = true;
    for (const char *p = content; *p; p++)
        if (!isspace((unsigned char)*p)) blank = false;
    if (blank) return NULL;
    const char *raw = settings_str("plan.autosaveDir", NULL);
    char *dir;
    char *t = raw ? str_trim(raw) : xstrdup("");
    if (!*t) dir = path_join3(a->cwd, ".omp", "plans");
    else dir = path_abs(t, a->cwd);
    free(t);
    if (fs_mkdir_p(dir, 0755) != 0) {
        *err = xasprintf("EACCES: cannot create %s", dir);
        free(dir);
        return NULL;
    }
    char *fname = app_plan_save_file_name(title);
    const char *ext = path_ext(fname);
    char *stem = xstrndup(fname, strlen(fname) - strlen(ext));
    char *out = NULL;
    for (int i = 0; i < 1000 && !out; i++) {
        char *name = i ? xasprintf("%s-%d%s", stem, i, ext) : xstrdup(fname);
        char *cand = path_join(dir, name);
        free(name);
        int fd = open(cand, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno != EEXIST) {
                *err = xasprintf("%s: %s", strerror(errno), cand);
                free(cand);
                break;
            }
            free(cand);
            continue;
        }
        size_t n = strlen(content), off = 0;
        while (off < n) {
            ssize_t w = write(fd, content + off, n - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        close(fd);
        out = cand;
    }
    free(stem);
    free(fname);
    free(dir);
    return out;
}

/* ---------- plan mode state ---------- */

static void append_mode_change(app_t *a, const char *mode, const char *plan_file) {
    if (!a->session) return;
    json_t *e = json_obj();
    json_set_str(e, "type", "mode_change");
    json_set_str(e, "mode", mode);
    if (plan_file) {
        json_t *d = json_obj();
        json_set_str(d, "planFilePath", plan_file);
        json_set(e, "data", d);
    }
    session_append_entry(a->session, e);
}

static char *propose_review(tool_env_t *env, const char *title, bool *is_error, json_t **details);
static char *propose_auto(tool_env_t *env, const char *title, bool *is_error, json_t **details);
static char *propose_yolo(tool_env_t *env, const char *title, bool *is_error, json_t **details);

static void install_propose(app_t *a) {
    a->env->propose_plan = !a->plan_mode ? NULL
                           : a->plan_yolo_armed ? propose_yolo
                           : a->plan_review_ui ? propose_review
                                               : propose_auto;
}

void app_enter_plan_mode(app_t *a, const char *plan_file) {
    if (a->plan_mode) return;
    const char *pf = plan_file ? plan_file : a->plan_reference_path ? a->plan_reference_path : DEFAULT_PLAN_URL;
    char *p = xstrdup(pf);
    free(a->plan_file);
    a->plan_file = p;
    a->plan_mode = true;
    a->plan_paused = false;
    /* setPlanModeState(enabled): the reference tracks the plan being drafted */
    a->plan_reference_sent = false;
    free(a->plan_reference_path);
    a->plan_reference_path = xstrdup(p);
    a->plan_reentry = a->plan_has_entered;
    a->plan_has_entered = true;
    install_propose(a);
    append_mode_change(a, "plan", p);
    app_rebuild_system_prompt(a);
}

/* setModelTemporary: switch without touching settings (model_change role "temporary") */
static void set_model_temporary(app_t *a, const ai_model_t *m, bool has_thinking, ai_effort_t thinking) {
    a->model = m;
    agent_set_model(a->agent, m);
    char *label = app_model_label(a);
    session_append_model_change(a->session, label, "temporary");
    free(label);
    if (has_thinking) app_set_thinking(a, thinking);
    app_rebuild_system_prompt(a);
}

/* #applyPlanModeModel: drive the session from the `plan` role while planning, remembering the pre-plan model */
void app_plan_apply_model(app_t *a) {
    const char *val = settings_str("modelRoles.plan", NULL);
    if (!val || !*val) return;
    ai_effort_t e = EFFORT_OFF;
    bool has_e = false;
    const ai_model_t *m = app_resolve_model_pattern(val, &e, &has_e);
    if (!m) return;
    if (!a->plan_prev_set && a->model) {
        a->plan_prev_set = true;
        a->plan_prev_model = a->model;
        a->plan_prev_thinking = a->thinking;
    }
    if (m == a->model) {
        if (has_e) app_set_thinking(a, e);
        return;
    }
    char *key = app_api_key_for(m);
    if (!key) {
        if (a->notice) {
            char *msg = xasprintf("Failed to switch to plan model for plan mode: No API key for %s/%s", m->provider, m->id);
            a->notice(a->notice_ud, "warning", msg);
            free(msg);
        }
        return;
    }
    free(key);
    set_model_temporary(a, m, has_e, e);
}

/* #restorePlanPreviousModel */
static void plan_restore_model(app_t *a) {
    if (!a->plan_prev_set) return;
    a->plan_prev_set = false;
    if (a->plan_prev_model == a->model) {
        if (a->thinking != a->plan_prev_thinking) app_set_thinking(a, a->plan_prev_thinking);
        return;
    }
    set_model_temporary(a, a->plan_prev_model, true, a->plan_prev_thinking);
}

void app_exit_plan_mode(app_t *a, bool paused) {
    if (!a->plan_mode) return;
    plan_restore_model(a);
    a->plan_mode = false;
    a->plan_paused = paused;
    a->env->propose_plan = NULL;
    append_mode_change(a, paused ? "plan_paused" : "none", NULL);
    app_rebuild_system_prompt(a);
}

void app_set_plan_mode(app_t *a, bool on) {
    if (on) app_enter_plan_mode(a, NULL);
    else app_exit_plan_mode(a, false);
}

void app_set_plan_file(app_t *a, const char *plan_file) {
    if (!a->plan_mode || str_eq(a->plan_file, plan_file)) return;
    free(a->plan_file);
    a->plan_file = xstrdup(plan_file);
    free(a->plan_reference_path);
    a->plan_reference_path = xstrdup(plan_file);
    append_mode_change(a, "plan", plan_file);
    app_rebuild_system_prompt(a);
}

void app_set_plan_review_ui(app_t *a, bool on) {
    a->plan_review_ui = on;
    install_propose(a);
}

void app_set_plan_reference(app_t *a, const char *path) {
    free(a->plan_reference_path);
    a->plan_reference_path = xstrdup(path ? path : DEFAULT_PLAN_URL);
}

json_t *app_plan_reference_message(app_t *a) {
    if (a->plan_mode || a->plan_reference_sent) return NULL;
    char *c = app_plan_read(a, a->plan_reference_path ? a->plan_reference_path : DEFAULT_PLAN_URL);
    if (!c) return NULL;
    char *t = str_trim(c);
    bool empty = !*t;
    free(t);
    if (empty) {
        free(c);
        return NULL;
    }
    json_t *ctx = json_obj();
    json_set_str(ctx, "planFilePath", a->plan_reference_path ? a->plan_reference_path : DEFAULT_PLAN_URL);
    json_set_str(ctx, "planContent", c);
    char *txt = prompt_render("coding-agent/prompts/system/plan-mode-reference.md", ctx);
    json_free(ctx);
    free(c);
    json_t *m = msg_user_text(txt);
    json_set_bool(m, "synthetic", true);
    json_set_str(m, "customType", "plan-mode-reference");
    free(txt);
    a->plan_reference_sent = true;
    return m;
}

char *app_plan_approved_prompt(app_t *a, const char *path, const char *content, bool preserved) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "contextPreserved", preserved);
    json_set_str(ctx, "planFilePath", path);
    json_set_str(ctx, "planContent", content);
    json_t *tl = json_arr();
    for (size_t i = 0; i < a->agent->tools.n; i++) json_push_str(tl, a->agent->tools.v[i]->name);
    json_set(ctx, "tools", tl);
    char *msg = prompt_render("coding-agent/prompts/system/plan-mode-approved.md", ctx);
    json_free(ctx);
    return msg;
}

/* ---------- xd://propose handlers ---------- */

/* review-mode hosts (the TUI): AgentSession.preparePlanForReview; the next model request is aborted
 * silently (handlePlanApproval's #abortPlanApprovalTurnSilently) so the model does not loop */
static char *propose_review(tool_env_t *env, const char *title, bool *is_error, json_t **details) {
    app_t *a = env->session;
    if (!a->plan_mode) {
        *is_error = true;
        return xstrdup("Plan mode is not active.");
    }
    char *path = NULL, *content = NULL, *rt = NULL, *err = NULL;
    if (app_plan_resolve(a, title, &path, &content, &rt, &err) != 0) {
        *is_error = true;
        return err;
    }
    *details = json_obj();
    json_set_str(*details, "planFilePath", path);
    json_set_str(*details, "title", rt);
    json_set_bool(*details, "planExists", true);
    agent_abort_next_silently(a->agent);
    free(path);
    free(content);
    free(rt);
    return xstrdup("Plan ready for review.");
}

/* hosts without a review surface (print, RPC, ACP clients without form elicitation): approve
 * automatically so plan mode never strands the agent (acp-agent.ts #handleAcpPlanProposal) */
static char *propose_auto(tool_env_t *env, const char *title, bool *is_error, json_t **details) {
    app_t *a = env->session;
    if (!a->plan_mode) {
        *is_error = true;
        return xstrdup("Plan mode is not active.");
    }
    char *path = NULL, *content = NULL, *rt = NULL, *err = NULL;
    if (app_plan_resolve(a, title, &path, &content, &rt, &err) != 0) {
        *is_error = true;
        return err;
    }
    app_set_plan_reference(a, path);
    a->plan_reference_sent = false;
    a->plan_mode = false;
    a->env->propose_plan = NULL;
    app_rebuild_system_prompt(a);
    char *aerr = NULL;
    char *saved = app_plan_autosave(a, rt, content, &aerr);
    *details = json_obj();
    json_set_str(*details, "planFilePath", path);
    json_set_str(*details, "title", rt);
    json_set_bool(*details, "planExists", true);
    char *r = aerr ? xasprintf("Plan approved at %s. Plan mode exited; proceed with the implementation. (Plan autosave failed; continuing.)", path)
                   : xasprintf("Plan approved at %s. Plan mode exited; proceed with the implementation.", path);
    if (a->plan_mode_changed) a->plan_mode_changed(a->plan_mode_changed_ud);
    free(saved);
    free(aerr);
    free(path);
    free(content);
    free(rt);
    return r;
}

/* ---------- role model cycle (model-controls.ts) ---------- */

size_t app_role_model_cycle(app_t *a, app_role_model_t **out, int *current) {
    *out = NULL;
    *current = 0;
    if (!a->model) return 0;
    json_t *order = settings_get("cycleOrder");
    static const char *const DEF[] = {"smol", "default", "slow"};
    size_t n = json_is_arr(order) ? json_len(order) : ARRAY_LEN(DEF);
    VEC(app_role_model_t) v = {0};
    for (size_t i = 0; i < n; i++) {
        const char *role = json_is_arr(order) ? json_sv(json_at(order, i)) : DEF[i];
        if (!role) continue;
        char *rk = xasprintf("modelRoles.%s", role);
        const char *val = settings_str(rk, NULL);
        free(rk);
        char *own = NULL;
        if (!val && !strcmp(role, "default")) val = own = xasprintf("%s/%s", a->model->provider, a->model->id);
        if (!val) continue;
        ai_effort_t e = EFFORT_OFF;
        bool has_e = false;
        const ai_model_t *m = app_resolve_model_pattern(val, &e, &has_e);
        free(own);
        if (!m) continue;
        vec_push(&v, ((app_role_model_t){xstrdup(role), m, e, has_e}));
    }
    int cur = -1;
    for (size_t i = 0; i < v.n && cur < 0; i++)
        if (v.v[i].model == a->model) cur = (int)i;
    *current = cur < 0 ? 0 : cur;
    *out = v.v;
    return v.n;
}

void app_role_model_cycle_free(app_role_model_t *v, size_t n) {
    for (size_t i = 0; i < n; i++) free(v[i].role);
    free(v);
}

void app_apply_role_model(app_t *a, const app_role_model_t *e) {
    a->model = e->model;
    agent_set_model(a->agent, e->model);
    char *label = app_model_label(a);
    session_append_model_change(a->session, label, e->role);
    free(label);
    if (e->explicit_thinking) app_set_thinking(a, e->thinking);
    app_rebuild_system_prompt(a);
}

/* ---------- synthetic execution prompt ---------- */

/* session.prompt(text, {synthetic: true}): a hidden agent-attributed developer message */
int app_prompt_synthetic(app_t *a, const char *text) {
    json_t *msgs = json_arr();
    json_t *ref = app_plan_reference_message(a);
    if (ref) json_push(msgs, ref);
    json_t *m = msg_developer_text(text);
    json_set_str(m, "attribution", "agent");
    json_set_bool(m, "synthetic", true);
    json_push(msgs, m);
    return app_plan_settle(a, agent_prompt(a->agent, msgs));
}

/* ---------- plan-mode decision reminder ---------- */

#define PLAN_MODE_REMINDER_MAX 3

void app_plan_note_user_turn(app_t *a) {
    a->plan_reminder_count = 0;
    a->plan_reminder_awaiting = false;
    a->plan_force_tool_choice = false;
}

void app_plan_note_tool_end(app_t *a, const char *tool, const tool_result_t *res, bool is_error) {
    a->plan_reminder_awaiting = false;
    const json_t *x = res && !is_error ? json_get(res->details, "xdev") : NULL;
    if (str_eq(tool, "ask") || (str_eq(tool, "write") && str_eq(json_get_str(x, "tool"), "propose"))) {
        a->plan_reminder_count = 0;
        a->plan_reminder_awaiting = false;
    }
}

static bool is_decision_call(const json_t *blk) {
    const char *name = json_get_str(blk, "name");
    if (str_eq(name, "ask")) return true;
    if (!str_eq(name, "write")) return false;
    const json_t *args = json_get(blk, "arguments");
    const char *path = json_get_str(args, "path");
    if (!path) path = json_get_str(args, "file_path");
    return path && !strcasecmp(path, "xd://propose");
}

static bool plan_decision_reminder(app_t *a) {
    if (!a->plan_mode) return false;
    json_t *last = NULL;
    pthread_mutex_lock(&a->agent->mu);
    for (size_t i = json_len(a->agent->messages); i > 0 && !last; i--) {
        json_t *m = json_at(a->agent->messages, i - 1);
        if (str_eq(json_get_str(m, "role"), "assistant")) last = json_clone(m);
    }
    pthread_mutex_unlock(&a->agent->mu);
    if (!last) return false;
    bool fire = false;
    const char *stop = json_get_str(last, "stopReason");
    if (!str_eq(stop, "error") && !str_eq(stop, "aborted")) {
        bool decision = false, any_tool = false;
        const json_t *blk;
        json_arr_foreach(json_get(last, "content"), i, blk) {
            if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
            any_tool = true;
            if (is_decision_call(blk)) decision = true;
        }
        if (decision) {
            a->plan_reminder_count = 0;
            a->plan_reminder_awaiting = false;
        } else if (!any_tool && !a->plan_reminder_awaiting && a->plan_reminder_count < PLAN_MODE_REMINDER_MAX &&
                   agent_find_tool(a->agent, "ask") && agent_find_tool(a->agent, "write")) {
            fire = true;
        }
    }
    json_free(last);
    if (!fire) return false;
    a->plan_reminder_count++;
    a->plan_reminder_awaiting = true;
    a->plan_force_tool_choice = true;
    /* upstream renders only askToolName: the write tool name comes out empty */
    json_t *ctx = json_obj();
    json_set_str(ctx, "askToolName", "ask");
    char *txt = prompt_render("coding-agent/prompts/system/plan-mode-tool-decision-reminder.md", ctx);
    json_free(ctx);
    json_t *m = msg_developer_text(txt);
    free(txt);
    json_set_str(m, "attribution", "agent");
    if (a->session) session_append_message(a->session, m);
    agent_append_message(a->agent, m);
    return true;
}

int app_plan_settle(app_t *a, int rc) {
    while (rc == 0 && !a->agent->cancel && plan_decision_reminder(a)) rc = agent_continue(a->agent);
    a->plan_force_tool_choice = false;
    return rc;
}

/* #reconcileModeFromSession (plan part): the last mode_change on the branch decides */
int app_plan_restore_from_session(app_t *a) {
    if (!a->session) return 0;
    json_t *path = session_path_to(a->session, a->session->leaf_id);
    const json_t *last = NULL, *e;
    json_arr_foreach(path, i, e) if (str_eq(json_get_str(e, "type"), "mode_change")) last = e;
    const char *mode = last ? json_get_str(last, "mode") : NULL;
    int r = 0;
    if (str_eq(mode, "plan") || str_eq(mode, "plan_paused")) {
        if (!settings_bool("plan.enabled", true)) {
            json_t *n = json_obj();
            json_set_str(n, "type", "mode_change");
            json_set_str(n, "mode", "none");
            session_append_entry(a->session, n);
        } else if (str_eq(mode, "plan")) {
            const char *pf = json_get_str(json_get(last, "data"), "planFilePath");
            if (a->plan_mode) app_exit_plan_mode_silently(a);
            app_enter_plan_mode(a, pf);
            r = 1;
        } else {
            if (a->plan_mode) app_exit_plan_mode_silently(a);
            a->plan_paused = true;
            a->plan_has_entered = true;
            r = 2;
        }
    } else if (a->plan_mode || a->plan_paused) {
        app_exit_plan_mode_silently(a);
        a->plan_paused = false;
        a->plan_has_entered = false;
    }
    json_free(path);
    return r;
}

/* clear the live plan state without journaling (the session being left/entered already says so) */
void app_exit_plan_mode_silently(app_t *a) {
    a->plan_mode = false;
    a->plan_paused = false;
    a->env->propose_plan = NULL;
    app_rebuild_system_prompt(a);
}

/* ---------- --plan-yolo (session/prewalk.ts armPlanYoloIfNeeded / #finalizePlanYoloProposal) ---------- */

static void yolo_notice(app_t *a, const char *level, const char *text) {
    if (a->notice) a->notice(a->notice_ud, level, text);
}

/* lazily enables the plan phase before the first prompt is built */
void app_plan_yolo_arm(app_t *a) {
    if (!a->plan_yolo_target || a->plan_yolo_armed || a->plan_yolo_done) return;
    a->plan_yolo_armed = true;
    free(a->plan_file);
    a->plan_file = xstrdup(a->plan_reference_path ? a->plan_reference_path : DEFAULT_PLAN_URL);
    a->plan_mode = true;
    a->plan_reference_sent = false;
    install_propose(a);
    app_rebuild_system_prompt(a);
}

static char *propose_yolo(tool_env_t *env, const char *title, bool *is_error, json_t **details) {
    app_t *a = env->session;
    if (!a->plan_yolo_target || !a->plan_mode) {
        *is_error = true;
        return xstrdup("Plan mode is not active.");
    }
    char *path = NULL, *content = NULL, *rt = NULL, *err = NULL;
    if (app_plan_resolve(a, title, &path, &content, &rt, &err) != 0) {
        *is_error = true;
        return err;
    }
    char *aerr = NULL;
    char *saved = app_plan_autosave(a, rt, content, &aerr);
    if (saved) {
        char *m = xasprintf("Plan autosaved to %s.", saved);
        yolo_notice(a, "info", m);
        free(m);
    } else if (aerr) {
        char *m = xasprintf("Plan autosave failed: %s Continuing with implementation.", aerr);
        yolo_notice(a, "warning", m);
        free(m);
    }
    free(saved);
    free(aerr);
    a->plan_mode = false;
    a->plan_yolo_armed = false;
    a->plan_yolo_done = true;
    a->env->propose_plan = NULL;
    const ai_model_t *target = a->plan_yolo_target;
    a->plan_yolo_target = NULL;
    a->model = target;
    agent_set_model(a->agent, target);
    char *label = app_model_label(a);
    session_append_model_change(a->session, label, NULL);
    free(label);
    if (a->plan_yolo_has_thinking) app_set_thinking(a, a->plan_yolo_thinking);
    app_rebuild_system_prompt(a);
    char *m = xasprintf("Plan-yolo: plan approved, switched to %s/%s to implement \"%s\".", target->provider, target->id, rt);
    yolo_notice(a, "info", m);
    free(m);
    json_t *ctx = json_obj();
    json_set_str(ctx, "planFilePath", path);
    json_set_str(ctx, "title", rt);
    char *handoff = prompt_render("coding-agent/prompts/system/plan-yolo-handoff.md", ctx);
    json_free(ctx);
    json_t *hm = msg_user_text(handoff);
    json_set_bool(hm, "synthetic", true);
    json_set_str(hm, "customType", "plan-yolo-handoff");
    agent_steer(a->agent, hm);
    free(handoff);
    *details = json_obj();
    json_set_str(*details, "planFilePath", path);
    json_set_str(*details, "title", rt);
    json_set_bool(*details, "planExists", true);
    char *r = xasprintf("Plan approved. Implementing now with %s.", target->id);
    free(path);
    free(content);
    free(rt);
    return r;
}

/* shouldEnterPlanModeOnStartup: a fresh session (no context, no mode_change) with plan.defaultOnStartup */
bool app_plan_should_start(app_t *a) {
    if (!settings_bool("plan.defaultOnStartup", false) || !settings_bool("plan.enabled", true)) return false;
    if (json_len(a->agent->messages) > 0) return false;
    const json_t *e;
    if (a->session) json_arr_foreach(a->session->entries, i, e) if (str_eq(json_get_str(e, "type"), "mode_change")) return false;
    return true;
}
