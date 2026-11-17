/* leaked_thinking.c - see leaked_thinking.h. Byte offsets stand in for upstream's string lengths: they are only
 * compared against the same text. */
#include "ai/leaked_thinking.h"
#include "ai/think_heal.h"

#include <strings.h>

/* ---------- gate (stream.ts healLeakedThinking, anthropic-state.ts reachesOfficialEndpoint) ---------- */

#define OFFICIAL_ANTHROPIC_URL "https://api.anthropic.com"

/* isOfficialAnthropicApiUrl */
static bool official_anthropic_url(const char *url) {
    if (!url || !*url) return true;
    size_t n = strlen(OFFICIAL_ANTHROPIC_URL);
    return strncasecmp(url, OFFICIAL_ANTHROPIC_URL, n) == 0 && (url[n] == '\0' || url[n] == '/');
}

/* normalizeAnthropicBaseUrl: trimmed, trailing slashes and one trailing /v1 removed; NULL when empty */
static char *normalize_anthropic_url(const char *url) {
    if (!url) return NULL;
    char *t = str_trim(url);
    size_t n = strlen(t);
    while (n && t[n - 1] == '/') t[--n] = '\0';
    if (n >= 3 && strcmp(t + n - 3, "/v1") == 0) t[n - 3] = '\0';
    if (!*t) {
        free(t);
        return NULL;
    }
    return t;
}

bool ai_reaches_official_endpoint(const ai_model_t *m) {
    if (!m || !json_truthy(model_compat_get(m, "officialEndpoint"))) return false;
    if (!str_eq(m->provider, "anthropic") || !str_eq(m->api, "anthropic-messages")) return true;
    /* resolveDirectAnthropicBaseUrl (omc has no Foundry routing) */
    char *configured = normalize_anthropic_url(m->base_url);
    bool official;
    if (configured && !official_anthropic_url(configured)) {
        official = false;
    } else {
        char *env = normalize_anthropic_url(getenv("ANTHROPIC_BASE_URL"));
        official = official_anthropic_url(env ? env : configured ? configured : OFFICIAL_ANTHROPIC_URL);
        free(env);
    }
    free(configured);
    return official;
}

/* ---------- LeakedThinkingProjector ---------- */

typedef struct {
    int src, idx;
} pair_t;

typedef struct {
    ai_event_cb cb;
    void *ud;
    json_t *partial; /* the projected message (owned) */
    int text;        /* open projected text block, -1 when none */
    int thinking;    /* open projected thinking block, -1 when none */
    think_heal_t healer;
    VEC(size_t) fed;   /* visible text consumed per source block (bytes) */
    int active_src;    /* source text block whose held healer output has not crossed a boundary, -1 */
    int apply_src;     /* source index of the healer output being applied */
    VEC(int) anchors;  /* source content index of every projected block */
    char *last_sig;    /* latest text signature (owned), NULL when undefined */
    VEC(pair_t) tools; /* forwarded native tool calls: source -> projected */
    VEC(pair_t) thinks; /* projected native thinking blocks: source -> projected */
    VEC(int) pending;  /* native thinking blocks whose projected thinking_end awaits the source end */
} proj_t;

static json_t *pcontent(proj_t *p) { return json_get(p->partial, "content"); }
static json_t *pblock(proj_t *p, int idx) { return json_at(pcontent(p), (size_t)idx); }
static int pcount(proj_t *p) { return (int)json_len(pcontent(p)); }

static void emit(proj_t *p, ai_event_type_t type, int idx, const char *delta, size_t n, const json_t *blk) {
    if (!p->cb) return;
    ai_event_t ev = {.type = type, .content_index = idx, .delta = delta, .delta_len = n, .block = blk, .partial = p->partial};
    p->cb(p->ud, &ev);
}

static pair_t *pair_find(pair_t *v, size_t n, int src) {
    for (size_t i = 0; i < n; i++)
        if (v[i].src == src) return &v[i];
    return NULL;
}

static bool pending_has(proj_t *p, int idx) {
    for (size_t i = 0; i < p->pending.n; i++)
        if (p->pending.v[i] == idx) return true;
    return false;
}

static bool pending_delete(proj_t *p, int idx) {
    for (size_t i = 0; i < p->pending.n; i++) {
        if (p->pending.v[i] != idx) continue;
        p->pending.v[i] = p->pending.v[--p->pending.n];
        return true;
    }
    return false;
}

static void anchor(proj_t *p, int idx, int src) {
    while (p->anchors.n <= (size_t)idx) vec_push(&p->anchors, -1);
    p->anchors.v[idx] = src;
}

static size_t *fed_slot(proj_t *p, int src) {
    while (p->fed.n <= (size_t)src) vec_push(&p->fed, (size_t)0);
    return &p->fed.v[src];
}

static void set_sig(proj_t *p, const char *sig) {
    free(p->last_sig);
    p->last_sig = sig ? xstrdup(sig) : NULL;
}

/* append to a string field in place (deltas would otherwise copy the whole block each time) */
static void str_append(json_t *blk, const char *key, const char *s, size_t n) {
    json_t *v = json_get(blk, key);
    if (!json_is_str(v)) {
        json_set(blk, key, json_strn(s, n));
        return;
    }
    v->u.str.s = xrealloc(v->u.str.s, v->u.str.len + n + 1);
    memcpy(v->u.str.s + v->u.str.len, s, n);
    v->u.str.len += n;
    v->u.str.s[v->u.str.len] = '\0';
}

/* adoptThinkingState: the replay-relevant state a source thinking block has gained so far */
static void adopt_thinking_state(json_t *target, const json_t *source) {
    if (!source) return;
    json_t *sig = json_get(source, "thinkingSignature");
    if (sig && !json_is_null(sig)) json_set(target, "thinkingSignature", json_clone(sig));
    json_t *sum = json_get(source, "summary");
    if (sum && !json_is_null(sum)) json_set(target, "summary", json_clone(sum));
}

static proj_t *proj_new(ai_event_cb cb, void *ud, const json_t *seed) {
    proj_t *p = xcalloc(1, sizeof(*p));
    p->cb = cb;
    p->ud = ud;
    p->text = p->thinking = p->active_src = p->apply_src = -1;
    think_heal_init(&p->healer);
    p->healer.implied_open = true;
    p->partial = json_is_obj(seed) ? json_clone(seed) : json_obj();
    json_set(p->partial, "content", json_arr());
    emit(p, AI_EV_START, -1, NULL, 0, NULL);
    return p;
}

static void proj_free(proj_t *p) {
    if (!p) return;
    json_free(p->partial);
    think_heal_free(&p->healer);
    vec_free(&p->fed);
    vec_free(&p->anchors);
    vec_free(&p->tools);
    vec_free(&p->thinks);
    vec_free(&p->pending);
    free(p->last_sig);
    free(p);
}

/* sync: follow the source message's non-content fields (usage and the like change while streaming) */
static void proj_sync(proj_t *p, const json_t *source) {
    const char *k;
    json_t *v;
    json_obj_foreach((json_t *)source, i, k, v) {
        if (strcmp(k, "content") != 0) json_set(p->partial, k, json_clone(v));
    }
}

static void emit_thinking_end(proj_t *p, int idx) {
    json_t *blk = pblock(p, idx);
    emit(p, AI_EV_THINKING_END, idx, NULL, 0, blk);
}

static void close_text(proj_t *p) {
    if (p->text < 0) return;
    int idx = p->text;
    p->text = -1;
    emit(p, AI_EV_TEXT_END, idx, NULL, 0, pblock(p, idx));
}

static void close_thinking(proj_t *p) {
    if (p->thinking < 0) return;
    int idx = p->thinking;
    p->thinking = -1;
    if (pending_has(p, idx)) return;
    emit_thinking_end(p, idx);
}

static int open_thinking(proj_t *p, int src) {
    close_text(p);
    if (p->thinking < 0) {
        json_t *blk = json_obj();
        json_set_str(blk, "type", "thinking");
        json_set_str(blk, "thinking", "");
        json_push(pcontent(p), blk);
        p->thinking = pcount(p) - 1;
        anchor(p, p->thinking, src);
        emit(p, AI_EV_THINKING_START, p->thinking, NULL, 0, NULL);
    }
    return p->thinking;
}

static void emit_text(proj_t *p, const char *s, size_t n, int src) {
    if (!n) return;
    close_thinking(p);
    if (p->text < 0) {
        json_t *blk = json_obj();
        json_set_str(blk, "type", "text");
        json_set_str(blk, "text", "");
        if (p->last_sig) json_set_str(blk, "textSignature", p->last_sig);
        json_push(pcontent(p), blk);
        p->text = pcount(p) - 1;
        anchor(p, p->text, src);
        emit(p, AI_EV_TEXT_START, p->text, NULL, 0, NULL);
    } else if (p->last_sig) {
        json_set_str(pblock(p, p->text), "textSignature", p->last_sig);
    }
    str_append(pblock(p, p->text), "text", s, n);
    emit(p, AI_EV_TEXT_DELTA, p->text, s, n, NULL);
}

/* healed (leaked) thinking carries no signature, matching the source fence */
static void emit_healed_thinking(proj_t *p, const char *s, size_t n, int src) {
    if (!n) return;
    int idx = open_thinking(p, src);
    str_append(pblock(p, idx), "thinking", s, n);
    emit(p, AI_EV_THINKING_DELTA, idx, s, n, NULL);
}

static bool blank(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') return false;
    }
    return true;
}

/* A bare reasoning close with no open in this stream. When the open text block is the message's only content, all
 * of it was reasoning behind a template-prefilled opener: re-project it as a closed thinking block at the same index.
 * Anywhere else the tag is a stray and is dropped. */
static void close_implied_thinking(proj_t *p, int src) {
    if (p->text != 0 || pcount(p) != 1) return;
    json_t *tv = json_get(pblock(p, 0), "text");
    if (!json_is_str(tv) || blank(tv->u.str.s, tv->u.str.len)) return;
    close_text(p);
    json_t *old = json_arr_remove(pcontent(p), 0);
    json_t *ts = json_take(old, "text");
    json_free(old);
    json_t *blk = json_obj();
    json_set_str(blk, "type", "thinking");
    json_set(blk, "thinking", ts);
    json_arr_insert(pcontent(p), 0, blk);
    anchor(p, 0, src);
    emit(p, AI_EV_THINKING_START, 0, NULL, 0, NULL);
    emit(p, AI_EV_THINKING_DELTA, 0, ts->u.str.s, ts->u.str.len, NULL);
    emit_thinking_end(p, 0);
}

static void heal_emit(void *ud, int kind, const char *s, size_t n) {
    proj_t *p = ud;
    if (kind == THINK_HEAL_TEXT) emit_text(p, s, n, p->apply_src);
    else if (kind == THINK_HEAL_THINKING) emit_healed_thinking(p, s, n, p->apply_src);
    else close_implied_thinking(p, p->apply_src);
}

static void heal_feed(proj_t *p, const char *s, size_t n, int src) {
    p->apply_src = src;
    think_heal_feed(&p->healer, s, n, heal_emit, p);
}

static void flush_healer(proj_t *p) {
    if (p->active_src >= 0) {
        p->apply_src = p->active_src;
        think_heal_flush(&p->healer, heal_emit, p);
    }
    p->active_src = -1;
}

/* release held-back text and close open blocks at a content boundary */
static void boundary(proj_t *p) {
    flush_healer(p);
    close_text(p);
    close_thinking(p);
}

/* feed a visible-text delta through the healer, splitting leaked fences live */
static void proj_text(proj_t *p, int src, const char *delta, size_t n, const char *sig) {
    bool starts = p->active_src != src;
    if (p->active_src >= 0 && starts) boundary(p);
    p->active_src = src;
    *fed_slot(p, src) += n;
    if (starts || sig) set_sig(p, sig);
    heal_feed(p, delta, n, src);
}

/* forward a native thinking delta, keeping its source block identity, signature and summary state */
static void proj_thinking(proj_t *p, int src, const char *delta, size_t n, const json_t *source) {
    pair_t *e = pair_find(p->thinks.v, p->thinks.n, src);
    int idx;
    if (!e) {
        if (p->thinking >= 0 && pending_has(p, p->thinking)) close_thinking(p);
        idx = open_thinking(p, src);
        pair_t np = {src, idx};
        vec_push(&p->thinks, np);
        vec_push(&p->pending, idx);
    } else {
        idx = e->idx;
    }
    json_t *blk = pblock(p, idx);
    str_append(blk, "thinking", delta ? delta : "", n);
    adopt_thinking_state(blk, source);
    emit(p, AI_EV_THINKING_DELTA, idx, delta, n, NULL);
}

/* emit only finalized native thinking that did not arrive as deltas */
static void append_thinking_tail(proj_t *p, int idx, const json_t *source) {
    json_t *blk = pblock(p, idx);
    json_t *have = json_get(blk, "thinking");
    json_t *src = source ? json_get(source, "thinking") : NULL;
    if (!json_is_str(src) || !json_is_str(have)) return;
    size_t hl = have->u.str.len;
    if (src->u.str.len <= hl || memcmp(src->u.str.s, have->u.str.s, hl) != 0) return;
    size_t n = src->u.str.len - hl;
    char *delta = xstrndup(src->u.str.s + hl, n);
    str_append(blk, "thinking", delta, n);
    emit(p, AI_EV_THINKING_DELTA, idx, delta, n, NULL);
    free(delta);
}

/* project a completed signature-bearing thinking block whose deltas never reached the projector */
static void project_signed_thinking(proj_t *p, int src, const json_t *source) {
    boundary(p);
    json_t *blk = json_obj();
    json_set_str(blk, "type", "thinking");
    json_set_str(blk, "thinking", "");
    adopt_thinking_state(blk, source);
    json_push(pcontent(p), blk);
    int idx = pcount(p) - 1;
    anchor(p, idx, src);
    pair_t np = {src, idx};
    vec_push(&p->thinks, np);
    emit(p, AI_EV_THINKING_START, idx, NULL, 0, NULL);
    append_thinking_tail(p, idx, source);
    emit_thinking_end(p, idx);
}

static bool has_signature(const json_t *source) {
    json_t *sig = source ? json_get(source, "thinkingSignature") : NULL;
    return json_truthy(sig) && !json_is_null(sig);
}

/* finalize a native thinking block by source identity: its projected end waits for this event */
static void proj_thinking_end(proj_t *p, int src, const json_t *source) {
    pair_t *e = pair_find(p->thinks.v, p->thinks.n, src);
    if (!e) {
        if (has_signature(source)) project_signed_thinking(p, src, source);
        return;
    }
    int idx = e->idx;
    append_thinking_tail(p, idx, source);
    adopt_thinking_state(pblock(p, idx), source);
    if (!pending_delete(p, idx)) return;
    if (p->thinking == idx) p->thinking = -1;
    emit_thinking_end(p, idx);
}

/* stamp a source text block's completed signature onto every block projected from it */
static void proj_text_end(proj_t *p, int src, const char *sig) {
    if (!sig) return;
    if (p->active_src == src) set_sig(p, sig);
    for (int i = 0; i < pcount(p); i++) {
        json_t *blk = pblock(p, i);
        if ((size_t)i < p->anchors.n && p->anchors.v[i] == src && str_eq(json_get_str(blk, "type"), "text"))
            json_set_str(blk, "textSignature", sig);
    }
}

static void proj_tool_start(proj_t *p, int src, const json_t *source) {
    if (!source) return;
    boundary(p);
    json_push(pcontent(p), json_clone(source));
    int idx = pcount(p) - 1;
    anchor(p, idx, src);
    pair_t np = {src, idx};
    vec_push(&p->tools, np);
    emit(p, AI_EV_TOOLCALL_START, idx, NULL, 0, NULL);
}

/* syncToolCall: Object.assign, so fields the source dropped since stay on the projected block */
static void assign_block(proj_t *p, int idx, const json_t *source) {
    json_t *blk = pblock(p, idx);
    const char *k;
    json_t *v;
    json_obj_foreach((json_t *)source, i, k, v) json_set(blk, k, json_clone(v));
}

static void proj_tool_delta(proj_t *p, int src, const char *delta, size_t n, const json_t *source) {
    pair_t *e = pair_find(p->tools.v, p->tools.n, src);
    if (!e && source) {
        proj_tool_start(p, src, source);
        e = pair_find(p->tools.v, p->tools.n, src);
    }
    if (!e) return;
    int idx = e->idx;
    if (source) assign_block(p, idx, source);
    emit(p, AI_EV_TOOLCALL_DELTA, idx, delta, n, NULL);
}

static void proj_tool_end(proj_t *p, int src, const json_t *tool_call) {
    if (!tool_call) return;
    pair_t *e = pair_find(p->tools.v, p->tools.n, src);
    if (e) {
        int idx = e->idx;
        assign_block(p, idx, tool_call);
        *e = p->tools.v[--p->tools.n];
        emit(p, AI_EV_TOOLCALL_END, idx, NULL, 0, pblock(p, idx));
        return;
    }
    /* an end without a matching start: release held text, then forward it whole */
    boundary(p);
    json_push(pcontent(p), json_clone(tool_call));
    int idx = pcount(p) - 1;
    anchor(p, idx, src);
    emit(p, AI_EV_TOOLCALL_START, idx, NULL, 0, NULL);
    emit(p, AI_EV_TOOLCALL_END, idx, NULL, 0, pblock(p, idx));
}

typedef struct {
    json_t *block;
    int source, order;
} anchored_t;

static int anchored_cmp(const void *a, const void *b) {
    const anchored_t *x = a, *y = b;
    if (x->source != y->source) return x->source < y->source ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* finish: replay any un-streamed visible-text tail from the final message, flush held-back fragments, close open
 * blocks; returns the healed content (owned), ordered by source position. omc never streams anthropicServerTool
 * history blocks, so upstream's #mergeServerToolHistory reduces to that ordering. */
static json_t *proj_finish(proj_t *p, const json_t *message) {
    json_t *mc = json_get(message, "content");
    size_t mn = json_is_arr(mc) ? json_len(mc) : 0;
    for (size_t i = 0; i < p->thinks.n; i++) {
        int src = p->thinks.v[i].src;
        json_t *blk = (size_t)src < mn ? json_at(mc, (size_t)src) : NULL;
        proj_thinking_end(p, src, str_eq(json_get_str(blk, "type"), "thinking") ? blk : NULL);
    }
    for (size_t src = 0; src < mn; src++) {
        json_t *blk = json_at(mc, src);
        if (!str_eq(json_get_str(blk, "type"), "thinking") || !has_signature(blk)) continue;
        if (pair_find(p->thinks.v, p->thinks.n, (int)src)) continue;
        project_signed_thinking(p, (int)src, blk);
    }
    for (size_t src = 0; src < mn; src++) {
        json_t *blk = json_at(mc, src);
        if (!str_eq(json_get_str(blk, "type"), "text")) continue;
        const char *sig = json_get_str(blk, "textSignature");
        proj_text_end(p, (int)src, sig);
        json_t *tv = json_get(blk, "text");
        size_t len = json_is_str(tv) ? tv->u.str.len : 0;
        size_t fed = *fed_slot(p, (int)src);
        if (len <= fed) continue;
        if (p->active_src >= 0 && p->active_src != (int)src) boundary(p);
        p->active_src = (int)src;
        set_sig(p, sig);
        heal_feed(p, tv->u.str.s + fed, len - fed, (int)src);
    }
    boundary(p);
    json_t *pc = pcontent(p);
    size_t n = json_len(pc);
    anchored_t *v = xcalloc(n ? n : 1, sizeof(*v));
    for (size_t i = 0; i < n; i++) {
        int a = i < p->anchors.n ? p->anchors.v[i] : -1;
        v[i] = (anchored_t){json_at(pc, i), a >= 0 ? a : (int)(mn + i), (int)i};
    }
    xqsort(v, n, sizeof(*v), anchored_cmp);
    json_t *out = json_arr();
    for (size_t i = 0; i < n; i++) json_push(out, json_clone(v[i].block));
    free(v);
    return out;
}

/* ---------- wrapLeakedThinkingStream ---------- */

struct leaked_thinking {
    ai_event_cb cb;
    void *ud;
    proj_t *p;
    bool terminal; /* a done/error went through: later events are not consumed */
    json_t *healed; /* healed content for the returned message (owned), NULL to leave it as is */
};

leaked_thinking_t *leaked_thinking_new(ai_event_cb cb, void *ud) {
    leaked_thinking_t *w = xcalloc(1, sizeof(*w));
    w->cb = cb;
    w->ud = ud;
    return w;
}

void leaked_thinking_free(leaked_thinking_t *w) {
    if (!w) return;
    proj_free(w->p);
    json_free(w->healed);
    free(w);
}

static proj_t *ensure(leaked_thinking_t *w, const json_t *seed) {
    if (!w->p) w->p = proj_new(w->cb, w->ud, seed);
    return w->p;
}

static const json_t *source_block(const ai_event_t *ev, const char *type) {
    json_t *blk = ev->content_index >= 0 ? json_at(json_get(ev->partial, "content"), (size_t)ev->content_index) : NULL;
    return str_eq(json_get_str(blk, "type"), type) ? blk : NULL;
}

static void terminal(leaked_thinking_t *w, const ai_event_t *ev) {
    proj_t *p = ensure(w, ev->partial);
    json_t *content = proj_finish(p, ev->partial);
    json_t *final = json_is_obj(ev->partial) ? json_clone(ev->partial) : json_obj();
    json_set(final, "content", json_clone(content));
    w->healed = content;
    if (w->cb) {
        ai_event_t e = {.type = ev->type, .content_index = -1, .partial = final, .reason = ev->reason};
        w->cb(w->ud, &e);
    }
    json_free(final);
}

void leaked_thinking_cb(void *ud, const ai_event_t *ev) {
    leaked_thinking_t *w = ud;
    if (w->terminal) return;
    bool end = ev->type == AI_EV_DONE || ev->type == AI_EV_ERROR;
    if (w->p && ev->partial && !end) proj_sync(w->p, ev->partial);
    int src = ev->content_index;
    switch (ev->type) {
    case AI_EV_START:
        proj_free(w->p);
        w->p = proj_new(w->cb, w->ud, ev->partial);
        break;
    case AI_EV_TEXT_DELTA: {
        proj_t *p = ensure(w, ev->partial);
        const json_t *blk = source_block(ev, "text");
        proj_text(p, src, ev->delta ? ev->delta : "", ev->delta_len, blk ? json_get_str(blk, "textSignature") : NULL);
        break;
    }
    case AI_EV_TEXT_END:
        if (w->p) {
            const json_t *blk = source_block(ev, "text");
            if (!blk && str_eq(json_get_str(ev->block, "type"), "text")) blk = ev->block;
            proj_text_end(w->p, src, blk ? json_get_str(blk, "textSignature") : NULL);
        }
        break;
    case AI_EV_THINKING_DELTA:
        proj_thinking(ensure(w, ev->partial), src, ev->delta, ev->delta_len, source_block(ev, "thinking"));
        break;
    case AI_EV_THINKING_END:
        if (w->p) {
            const json_t *blk = source_block(ev, "thinking");
            if (!blk && str_eq(json_get_str(ev->block, "type"), "thinking")) blk = ev->block;
            proj_thinking_end(w->p, src, blk);
        }
        break;
    case AI_EV_TOOLCALL_START:
        proj_tool_start(ensure(w, ev->partial), src, source_block(ev, "toolCall"));
        break;
    case AI_EV_TOOLCALL_DELTA:
        if (w->p) proj_tool_delta(w->p, src, ev->delta, ev->delta_len, source_block(ev, "toolCall"));
        break;
    case AI_EV_TOOLCALL_END:
        if (w->p) proj_tool_end(w->p, src, ev->block ? ev->block : source_block(ev, "toolCall"));
        break;
    case AI_EV_DONE:
        w->terminal = true;
        terminal(w, ev);
        break;
    case AI_EV_ERROR:
        w->terminal = true;
        /* an error with empty content means the provider discarded the partial turn: forward it as is rather than
         * resurrect what was discarded */
        if (w->p && json_len(json_get(ev->partial, "content")) == 0) {
            if (w->cb) w->cb(w->ud, ev);
            break;
        }
        terminal(w, ev);
        break;
    default: /* text_start / thinking_start: the projector owns block boundaries */
        break;
    }
}

json_t *leaked_thinking_finish(leaked_thinking_t *w, json_t *msg) {
    if (!json_is_obj(msg)) return msg;
    if (!w->terminal) { /* the provider ended without a terminal event */
        proj_t *p = ensure(w, msg);
        json_set(msg, "content", proj_finish(p, msg));
        w->terminal = true;
    } else if (w->healed) {
        json_set(msg, "content", w->healed);
        w->healed = NULL;
    }
    return msg;
}
