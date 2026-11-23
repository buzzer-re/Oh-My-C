/* xdnotice.c - tool-command mount notices (see xdnotice.h; port of session-tools.ts). */
#include "xdnotice.h"

#include "config/settings.h"
#include "prompt/template.h"
#include "tools/toolcmd.h"

#include <pthread.h>
#include <string.h>

#define NOTICE_TYPE "tool-command-mount-notice"

struct xdnotice {
    pthread_mutex_t mu;
    strvec_t mounted;   /* the mounted set the last sync saw */
    strvec_t added;     /* pending (unannounced) delta, insertion order */
    strvec_t removed;
    strvec_t announced; /* commands the model has been told about */
    bool seeded;        /* announced replayed from persisted notices */
    strvec_t base;      /* commands the current base system prompt lists */
    strvec_t base_routed; /* commands whose route line, with its catalog summary, that prompt renders */
};

static void sv_remove(strvec_t *v, const char *s) {
    for (size_t i = 0; i < v->n; i++) {
        if (strcmp(v->v[i], s)) continue;
        free(v->v[i]);
        memmove(v->v + i, v->v + i + 1, (v->n - i - 1) * sizeof(char *));
        v->n--;
        return;
    }
}

static void sv_add(strvec_t *v, const char *s) {
    if (!strvec_contains(v, s)) strvec_push(v, s);
}

static void sv_copy(strvec_t *dst, const strvec_t *src) {
    for (size_t i = 0; i < src->n; i++) strvec_push(dst, src->v[i]);
}

static struct xdnotice *state(app_t *a) {
    if (!a->xdnotice) {
        a->xdnotice = xcalloc(1, sizeof(*a->xdnotice));
        pthread_mutex_init(&a->xdnotice->mu, NULL);
    }
    return a->xdnotice;
}

static strvec_t mounted_names(agent_t *ag) {
    strvec_t out = {0};
    pthread_mutex_lock(&ag->mu);
    for (size_t i = 0; i < ag->all_tools.n; i++)
        if (ag->all_tools.v[i]->mounted) strvec_push(&out, ag->all_tools.v[i]->name);
    pthread_mutex_unlock(&ag->mu);
    return out;
}

/* #coalesceToolRosterDelta: an unmount cancels a pending mount the model never learned about, and a remount
 * cancels a pending unmount */
static void coalesce(struct xdnotice *x, const strvec_t *added, const strvec_t *removed) {
    for (size_t i = 0; i < added->n; i++) {
        if (strvec_contains(&x->removed, added->v[i])) sv_remove(&x->removed, added->v[i]);
        else sv_add(&x->added, added->v[i]);
    }
    for (size_t i = 0; i < removed->n; i++) {
        if (strvec_contains(&x->added, removed->v[i])) sv_remove(&x->added, removed->v[i]);
        else sv_add(&x->removed, removed->v[i]);
    }
}

void xdnotice_init(app_t *a) {
    struct xdnotice *x = state(a);
    strvec_t cur = mounted_names(a->agent);
    pthread_mutex_lock(&x->mu);
    strvec_free(&x->mounted);
    /* a UI session discovers MCP servers after the session exists (deferMCPDiscoveryForUI): its MCP tools mount
     * through refreshMCPTools, a pending startup delta the first prompt finds in the base prompt */
    strvec_t mcp = {0};
    for (size_t i = 0; i < cur.n; i++) {
        if (a->opts.has_ui && !a->opts.subagent && str_starts(cur.v[i], "mcp__")) strvec_push(&mcp, cur.v[i]);
        else strvec_push(&x->mounted, cur.v[i]);
    }
    strvec_t none = {0};
    coalesce(x, &mcp, &none);
    sv_copy(&x->mounted, &mcp);
    pthread_mutex_unlock(&x->mu);
    strvec_free(&mcp);
    strvec_free(&cur);
}

void xdnotice_sync(app_t *a, char **shown) {
    if (shown) *shown = NULL;
    if (!a->agent) return;
    struct xdnotice *x = state(a);
    strvec_t cur = mounted_names(a->agent);
    strvec_t added = {0}, removed = {0};
    pthread_mutex_lock(&x->mu);
    for (size_t i = 0; i < cur.n; i++)
        if (!strvec_contains(&x->mounted, cur.v[i])) strvec_push(&added, cur.v[i]);
    for (size_t i = 0; i < x->mounted.n; i++)
        if (!strvec_contains(&cur, x->mounted.v[i])) strvec_push(&removed, x->mounted.v[i]);
    strvec_free(&x->mounted);
    x->mounted = cur;
    if (added.n || removed.n) coalesce(x, &added, &removed);
    pthread_mutex_unlock(&x->mu);
    if ((added.n || removed.n) && !settings_bool("startup.quiet", false) && settings_bool("tools.commandNotices", true)) {
        sb_t b = SB_INIT;
        if (added.n) {
            char *j = strvec_join(&added, ", ");
            sb_appendf(&b, "added %s", j);
            free(j);
        }
        if (removed.n) {
            char *j = strvec_join(&removed, ", ");
            sb_appendf(&b, "%sremoved %s", b.len ? "; " : "", j);
            free(j);
        }
        if (shown) *shown = xasprintf("Tool commands: %s", sb_cstr(&b));
        else app_notice(a, "toolCommands", "Tool commands: %s", sb_cstr(&b));
        sb_free(&b);
    }
    strvec_free(&added);
    strvec_free(&removed);
}

void xdnotice_base_rebuilt(app_t *a, bool catalog_delivered) {
    if (!a->agent) return;
    struct xdnotice *x = state(a);
    strvec_t cur = catalog_delivered ? mounted_names(a->agent) : (strvec_t){0};
    strvec_t routed = {0};
    free(toolcmd_mcp_guidance(a->agent, &routed));
    pthread_mutex_lock(&x->mu);
    strvec_free(&x->base);
    x->base = cur;
    strvec_free(&x->base_routed);
    x->base_routed = routed;
    pthread_mutex_unlock(&x->mu);
}

void xdnotice_reset_announced(app_t *a) {
    if (!a->xdnotice) return;
    pthread_mutex_lock(&a->xdnotice->mu);
    strvec_free(&a->xdnotice->announced);
    a->xdnotice->seeded = false;
    pthread_mutex_unlock(&a->xdnotice->mu);
}

/* #ensureAnnouncedMountsSeeded: replay the structured details of persisted notices */
static void seed(struct xdnotice *x, agent_t *ag) {
    if (x->seeded) return;
    x->seeded = true;
    pthread_mutex_lock(&ag->mu);
    json_t *msgs = json_clone(ag->messages);
    pthread_mutex_unlock(&ag->mu);
    json_t *m;
    json_arr_foreach(msgs, i, m) {
        if (!str_eq(json_get_str(m, "customType"), NOTICE_TYPE)) continue;
        const json_t *d = json_get_obj(m, "details");
        const json_t *ad = json_get_arr(d, "added"), *rm = json_get_arr(d, "removed"), *n;
        if (!ad || !rm) continue;
        bool strings = true;
        json_arr_foreach(ad, k, n) strings &= json_is_str(n);
        json_arr_foreach(rm, k, n) strings &= json_is_str(n);
        if (!strings) continue;
        json_arr_foreach(ad, k, n) sv_add(&x->announced, json_sv(n));
        json_arr_foreach(rm, k, n) sv_remove(&x->announced, json_sv(n));
    }
    json_free(msgs);
}

json_t *xdnotice_take(app_t *a) {
    if (!a->xdnotice || !a->agent) return NULL;
    struct xdnotice *x = a->xdnotice;
    pthread_mutex_lock(&x->mu);
    if (!x->added.n && !x->removed.n) {
        pthread_mutex_unlock(&x->mu);
        return NULL;
    }
    seed(x, a->agent);
    /* a pending add the committed base prompt already lists (its catalog, or an MCP route line carrying the catalog
     * summary) needs no notice line; one mounted since keeps its notice even when the live routes list it */
    for (size_t i = 0; i < x->added.n; i++)
        if (strvec_contains(&x->base, x->added.v[i]) || strvec_contains(&x->base_routed, x->added.v[i]))
            sv_add(&x->announced, x->added.v[i]);
    strvec_t added = {0}, removed = {0};
    for (size_t i = 0; i < x->added.n; i++)
        if (!strvec_contains(&x->announced, x->added.v[i])) strvec_push(&added, x->added.v[i]);
    for (size_t i = 0; i < x->removed.n; i++)
        if (strvec_contains(&x->announced, x->removed.v[i])) strvec_push(&removed, x->removed.v[i]);
    for (size_t i = 0; i < added.n; i++) sv_add(&x->announced, added.v[i]);
    for (size_t i = 0; i < removed.n; i++) sv_remove(&x->announced, removed.v[i]);
    strvec_free(&x->added);
    strvec_free(&x->removed);
    pthread_mutex_unlock(&x->mu);
    if (!added.n && !removed.n) return NULL;

    json_t *entries = toolcmd_entries(a->agent), *e;
    json_t *ctx = json_obj(), *ja = json_arr(), *jr = json_arr();
    json_t *det = json_obj(), *da = json_arr(), *dr = json_arr();
    for (size_t i = 0; i < added.n; i++) {
        const char *summary = "";
        json_arr_foreach(entries, k, e) if (str_eq(json_get_str(e, "name"), added.v[i])) summary = json_get_str(e, "summary");
        json_t *o = json_obj();
        json_set_str(o, "name", added.v[i]);
        json_set_str(o, "summary", summary ? summary : "");
        json_push(ja, o);
        json_push_str(da, added.v[i]);
    }
    for (size_t i = 0; i < removed.n; i++) {
        json_t *o = json_obj();
        json_set_str(o, "name", removed.v[i]);
        json_push(jr, o);
        json_push_str(dr, removed.v[i]);
    }
    json_set(ctx, "added", ja);
    json_set(ctx, "removed", jr);
    json_set(ctx, "docs", json_str_own(toolcmd_docs_for(a->agent, &added)));
    char *body = prompt_render("coding-agent/prompts/system/tool-command-mount-notice.md", ctx);
    json_free(ctx);
    json_free(entries);
    json_t *m = msg_developer_text(body);
    free(body);
    json_set_str(m, "customType", NOTICE_TYPE);
    json_set_str(m, "attribution", "agent");
    json_set_bool(m, "display", false);
    json_set(det, "added", da);
    json_set(det, "removed", dr);
    json_set(m, "details", det);
    strvec_free(&added);
    strvec_free(&removed);
    return m;
}

void xdnotice_free(app_t *a) {
    struct xdnotice *x = a->xdnotice;
    if (!x) return;
    strvec_free(&x->mounted);
    strvec_free(&x->added);
    strvec_free(&x->removed);
    strvec_free(&x->announced);
    strvec_free(&x->base);
    strvec_free(&x->base_routed);
    pthread_mutex_destroy(&x->mu);
    free(x);
    a->xdnotice = NULL;
}
