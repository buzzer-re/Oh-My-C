#include "ttsr.h"

#include "config/settings.h"
#include "prompt/template.h"
#include "text/glob.h"
#include "text/regex.h"

#include <pthread.h>

typedef struct {
    char *tool;   /* NULL => any tool */
    char *glob;   /* path glob or NULL */
} tool_scope_t;

typedef struct {
    const rule_t *rule;
    rx_t *rx;
    bool text, thinking, any_tool;
    VEC(tool_scope_t) tools;
    const char *interrupt; /* resolved mode */
    bool injected;         /* has an injection record */
    int last_injected_at;  /* the record: the turn its reminder landed in (TtsrManager lastInjectedAt) */
} trule_t;

typedef VEC(const trule_t *) trule_list_t;

typedef struct {
    int index;
    int kind; /* 1 text, 2 thinking, 3 tool */
    sb_t buf;
    char *tool_name;
    char *tool_id;
} tbuf_t;

struct ttsr {
    VEC(trule_t) rules;
    VEC(tbuf_t) bufs;
    trule_list_t pending;                 /* interrupting matches */
    trule_list_t deferred;                /* prose non-interrupting */
    json_t *tool_reminders;               /* tool_call_id -> [rule names] */
    int message_count;                    /* completed turns (TtsrManager #messageCount) */
    pthread_mutex_t mu;
};

/* TtsrManager #canTrigger: a rule's own `repeatMode` wins over ttsr.repeatMode; `once` never repeats, `after-gap`
 * waits ttsr.repeatGap completed turns after the turn its reminder landed in, `always` one (omp 8c71a84a79,
 * ba99634f57, e792797d1c) */
static bool can_trigger(const ttsr_t *t, const trule_t *r) {
    if (!r->injected) return true;
    const char *mode = r->rule->repeat_mode ? r->rule->repeat_mode : settings_str("ttsr.repeatMode", "once");
    if (!str_eq(mode, "after-gap") && !str_eq(mode, "always")) return false;
    int64_t gap = str_eq(mode, "always") ? 1 : settings_int("ttsr.repeatGap", 10);
    return t->message_count - r->last_injected_at >= gap;
}

/* markInjectedByNames: the record is the delivery turn, never earlier than the current one */
static void mark_injected(ttsr_t *t, trule_t *r, int delivery_turn) {
    int at = MAX(delivery_turn, t->message_count);
    r->last_injected_at = r->injected ? MAX(r->last_injected_at, at) : at;
    r->injected = true;
}

static void parse_scope(trule_t *r, const char *scope) {
    if (!scope || !*scope) {
        r->text = true;
        r->any_tool = true;
        return;
    }
    strvec_t parts = str_split(scope, ",");
    for (size_t i = 0; i < parts.n; i++) {
        char *p = str_trim(parts.v[i]);
        if (!strcmp(p, "text")) r->text = true;
        else if (!strcmp(p, "thinking")) r->thinking = true;
        else if (!strcmp(p, "tool") || !strcmp(p, "toolcall")) r->any_tool = true;
        else if (str_starts(p, "tool:")) {
            const char *t = p + 5;
            const char *paren = strchr(t, '(');
            tool_scope_t ts = {0};
            if (paren) {
                ts.tool = xstrndup(t, (size_t)(paren - t));
                const char *close = strrchr(paren, ')');
                ts.glob = xstrndup(paren + 1, close ? (size_t)(close - paren - 1) : strlen(paren + 1));
            } else {
                ts.tool = xstrdup(t);
            }
            vec_push(&r->tools, ts);
        }
        free(p);
    }
    strvec_free(&parts);
}

ttsr_t *ttsr_new(const discovery_t *disc) {
    ttsr_t *t = xcalloc(1, sizeof(*t));
    pthread_mutex_init(&t->mu, NULL);
    t->tool_reminders = json_obj();
    if (!settings_bool("ttsr.enabled", true) || !disc) return t;
    json_t *disabled = settings_get("ttsr.disabledRules");
    const char *global_mode = settings_str("ttsr.interruptMode", "always");
    for (size_t i = 0; i < disc->rules.n; i++) {
        const rule_t *r = &disc->rules.v[i];
        if (!r->ttsr) continue;
        /* a `question` rule is judged (ttsr.judge): its `condition` is only the judge's prefilter, never a trigger
         * on its own. omc has no TTSR judge, so such a rule never fires (omp without a judge model). */
        if (r->has_question) continue;
        bool off = false;
        json_t *x;
        json_arr_foreach(disabled, k, x) if (str_eq(json_sv(x), r->name)) off = true;
        if (off) continue;
        char *err = NULL;
        rx_t *rx = rx_compile(r->ttsr, 0, &err);
        if (!rx) {
            LOGW("ttsr rule %s: invalid condition: %s", r->name, err ? err : "?");
            free(err);
            continue;
        }
        trule_t tr = {.rule = r, .rx = rx, .interrupt = r->interrupt_mode ? r->interrupt_mode : global_mode};
        parse_scope(&tr, r->scope);
        vec_push(&t->rules, tr);
    }
    return t;
}

static void bufs_clear(ttsr_t *t) {
    for (size_t i = 0; i < t->bufs.n; i++) {
        sb_free(&t->bufs.v[i].buf);
        free(t->bufs.v[i].tool_name);
        free(t->bufs.v[i].tool_id);
    }
    t->bufs.n = 0;
}

void ttsr_free(ttsr_t *t) {
    if (!t) return;
    for (size_t i = 0; i < t->rules.n; i++) {
        rx_free(t->rules.v[i].rx);
        for (size_t k = 0; k < t->rules.v[i].tools.n; k++) {
            free(t->rules.v[i].tools.v[k].tool);
            free(t->rules.v[i].tools.v[k].glob);
        }
        vec_free(&t->rules.v[i].tools);
    }
    vec_free(&t->rules);
    bufs_clear(t);
    vec_free(&t->bufs);
    vec_free(&t->pending);
    vec_free(&t->deferred);
    json_free(t->tool_reminders);
    pthread_mutex_destroy(&t->mu);
    free(t);
}

size_t ttsr_rule_count(const ttsr_t *t) { return t ? t->rules.n : 0; }

void ttsr_reset(ttsr_t *t) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    bufs_clear(t);
    pthread_mutex_unlock(&t->mu);
}

static tbuf_t *buf_for(ttsr_t *t, int index, int kind) {
    for (size_t i = 0; i < t->bufs.n; i++)
        if (t->bufs.v[i].index == index) return &t->bufs.v[i];
    tbuf_t b = {.index = index, .kind = kind};
    vec_push(&t->bufs, b);
    return &t->bufs.v[t->bufs.n - 1];
}

/* paths touched by a tool call: "path" arg or hashline [PATH#TAG] headers */
static void tool_paths(const char *name, const char *args_text, strvec_t *out) {
    json_t *args = json_parse_partial(args_text, strlen(args_text));
    const char *p = json_get_str(args, "path");
    if (p) strvec_push(out, p);
    const char *input = json_get_str(args, "input");
    if (input && str_eq(name, "edit")) {
        for (const char *s = input; (s = strchr(s, '[')); s++) {
            const char *h = strchr(s, '#'), *e = strchr(s, ']'), *nl = strchr(s, '\n');
            if (h && e && h < e && (!nl || e < nl) && (s == input || s[-1] == '\n')) strvec_push_own(out, xstrndup(s + 1, (size_t)(h - s - 1)));
        }
    }
    json_free(args);
}

static bool scope_matches(const trule_t *r, const tbuf_t *b) {
    if (b->kind == 1) return r->text;
    if (b->kind == 2) return r->thinking;
    if (r->any_tool) return true;
    for (size_t i = 0; i < r->tools.n; i++) {
        const tool_scope_t *ts = &r->tools.v[i];
        if (ts->tool && strcmp(ts->tool, "*") != 0 && !str_eq(ts->tool, b->tool_name)) continue;
        if (!ts->glob) return true;
        strvec_t paths = {0};
        tool_paths(b->tool_name ? b->tool_name : "", sb_cstr((sb_t *)&b->buf), &paths);
        bool hit = false;
        for (size_t k = 0; k < paths.n && !hit; k++) {
            const char *base = strrchr(paths.v[k], '/');
            hit = glob_match(ts->glob, paths.v[k], 0) || (base && glob_match(ts->glob, base + 1, 0));
        }
        strvec_free(&paths);
        if (hit) return true;
    }
    return false;
}

static bool mode_interrupts(const char *mode, int kind) {
    if (!mode || !strcmp(mode, "always")) return true;
    if (!strcmp(mode, "never")) return false;
    if (!strcmp(mode, "prose-only")) return kind != 3;
    if (!strcmp(mode, "tool-only")) return kind == 3;
    return true;
}

static bool already(const trule_list_t *v, const trule_t *r) {
    for (size_t i = 0; i < v->n; i++)
        if (v->v[i] == r) return true;
    return false;
}

bool ttsr_on_event(ttsr_t *t, const ai_event_t *ev) {
    if (!t || !t->rules.n) return false;
    int kind = ev->type == AI_EV_TEXT_DELTA ? 1 : ev->type == AI_EV_THINKING_DELTA ? 2 : ev->type == AI_EV_TOOLCALL_DELTA ? 3 : 0;
    if (!kind || !ev->delta) return false;
    pthread_mutex_lock(&t->mu);
    tbuf_t *b = buf_for(t, ev->content_index, kind);
    sb_appendn(&b->buf, ev->delta, ev->delta_len);
    if (kind == 3 && !b->tool_name) {
        const json_t *blk = json_at(json_get(ev->partial, "content"), (size_t)ev->content_index);
        if (json_get_str(blk, "name")) b->tool_name = xstrdup(json_get_str(blk, "name"));
        if (json_get_str(blk, "id")) b->tool_id = xstrdup(json_get_str(blk, "id"));
    }
    /* for tool streams match the decoded argument text, not JSON escapes */
    char *subject = NULL;
    if (kind == 3) {
        json_t *args = json_parse_partial(b->buf.s, b->buf.len);
        sb_t s = SB_INIT;
        const char *k2;
        json_t *v;
        json_obj_foreach(args, i, k2, v) if (json_is_str(v)) {
            sb_append(&s, json_sv(v));
            sb_putc(&s, '\n');
        }
        json_free(args);
        subject = sb_detach(&s);
    }
    const char *text = subject ? subject : b->buf.s;
    size_t tlen = subject ? strlen(subject) : b->buf.len;
    /* only re-scan the tail since the last line start (conditions rarely span lines) */
    size_t from = 0;
    if (!subject && b->buf.len > ev->delta_len) {
        size_t prev = b->buf.len - ev->delta_len;
        while (prev > 0 && text[prev - 1] != '\n') prev--;
        from = prev;
    }
    bool interrupt = false;
    for (size_t i = 0; i < t->rules.n; i++) {
        trule_t *r = &t->rules.v[i];
        if (!can_trigger(t, r) || already(&t->pending, r) || already(&t->deferred, r)) continue;
        if (!scope_matches(r, b)) continue;
        if (!rx_is_match(r->rx, text, tlen, from)) continue;
        if (mode_interrupts(r->interrupt, kind)) {
            vec_push(&t->pending, r);
            interrupt = true;
        } else if (kind == 3 && b->tool_id) {
            json_t *list = json_get_arr(t->tool_reminders, b->tool_id);
            if (!list) {
                list = json_arr();
                json_set(t->tool_reminders, b->tool_id, list);
            }
            json_push_str(list, r->rule->name);
            mark_injected(t, r, t->message_count); /* a tool reminder lands in the triggering turn */
        } else {
            vec_push(&t->deferred, r);
        }
    }
    free(subject);
    pthread_mutex_unlock(&t->mu);
    return interrupt;
}

static const trule_t *find_rule(ttsr_t *t, const char *name) {
    for (size_t i = 0; i < t->rules.n; i++)
        if (!strcmp(t->rules.v[i].rule->name, name)) return &t->rules.v[i];
    return NULL;
}

static void render_rules(sb_t *b, const char *tpl, const trule_t *const *rules, size_t n) {
    for (size_t i = 0; i < n; i++) {
        json_t *ctx = json_obj();
        json_set_str(ctx, "name", rules[i]->rule->name);
        json_set_str(ctx, "path", rules[i]->rule->path);
        json_set_str(ctx, "content", rules[i]->rule->content);
        json_set_bool(ctx, "builtin", rules[i]->rule->builtin);
        char *r = prompt_render(tpl, ctx);
        if (b->len) sb_append(b, "\n\n");
        sb_append(b, r);
        free(r);
        json_free(ctx);
    }
}

char *ttsr_take_interrupt(ttsr_t *t, json_t **names_out) {
    if (!t) return NULL;
    pthread_mutex_lock(&t->mu);
    char *out = NULL;
    if (t->pending.n) {
        sb_t b = SB_INIT;
        render_rules(&b, "coding-agent/prompts/system/ttsr-interrupt.md", t->pending.v, t->pending.n);
        json_t *names = json_arr();
        for (size_t i = 0; i < t->pending.n; i++) {
            mark_injected(t, (trule_t *)t->pending.v[i], t->message_count + 1); /* lands in the interrupt's retry */
            json_push_str(names, t->pending.v[i]->rule->name);
        }
        if (names_out) *names_out = names;
        else json_free(names);
        t->pending.n = 0;
        out = sb_detach(&b);
    }
    bufs_clear(t);
    pthread_mutex_unlock(&t->mu);
    return out;
}

char *ttsr_take_tool_reminder(ttsr_t *t, const char *id) {
    if (!t || !id) return NULL;
    pthread_mutex_lock(&t->mu);
    json_t *list = json_take(t->tool_reminders, id);
    char *out = NULL;
    if (list) {
        trule_list_t rs = {0};
        json_t *x;
        json_arr_foreach(list, i, x) {
            const trule_t *r = find_rule(t, json_sv(x));
            if (r) vec_push(&rs, r);
        }
        sb_t b = SB_INIT;
        render_rules(&b, "coding-agent/prompts/system/ttsr-tool-reminder.md", rs.v, rs.n);
        out = b.len ? sb_detach(&b) : (sb_free(&b), NULL);
        vec_free(&rs);
        json_free(list);
    }
    pthread_mutex_unlock(&t->mu);
    return out;
}

char *ttsr_take_deferred(ttsr_t *t) {
    if (!t) return NULL;
    pthread_mutex_lock(&t->mu);
    char *out = NULL;
    if (t->deferred.n) {
        /* the rules matched output they did not interrupt: the notice must claim neither an interruption nor a
         * completed output (omp 3e3ca57a34) */
        sb_t b = SB_INIT;
        render_rules(&b, "coding-agent/prompts/system/ttsr-prose-notice.md", t->deferred.v, t->deferred.n);
        /* delivered after the triggering turn ended: the continuation it starts is the turn it lands in */
        for (size_t i = 0; i < t->deferred.n; i++) mark_injected(t, (trule_t *)t->deferred.v[i], t->message_count + 1);
        t->deferred.n = 0;
        out = sb_detach(&b);
    }
    pthread_mutex_unlock(&t->mu);
    return out;
}

void ttsr_turn_end(ttsr_t *t) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    t->message_count++;
    pthread_mutex_unlock(&t->mu);
}
