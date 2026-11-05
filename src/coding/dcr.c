/* dcr.c - date/cwd reminder injection (port of omp session/date-cwd-reminder.ts). See dcr.h. */
#include "coding/dcr.h"
#include "prompt/template.h"
#include "tools/gh.h"
#include <stdint.h>

/* marks a transformed message as an inserted control; the message never leaves the request build (WeakSet upstream) */
#define DCR_CONTROL_MARK "_dcrControl"
/* MAX_ROOTS: a compaction, branch switch or side request starts another conversation; the oldest is forgotten first */
#define DCR_MAX_ROOTS 8

static __thread bool tl_side;

void dcr_set_side_thread(bool side) { tl_side = side; }

/* Injection: a reminder already sent with one user turn, and the copy of the turn it was sent on */
typedef struct {
    char *key;
    char *reminder;
    json_t *source;
} inj_t;

/* AnchoredControl: a developer reminder and the key of the turn it was placed after */
typedef struct {
    char *anchor;
    json_t *message;
} ctl_t;

/* RootState: everything the injector remembers about one conversation */
typedef struct {
    char *root;
    char *conversation; /* the journal's name for this conversation: its first turn's key when it began */
    char *reminder;     /* the reminder the conversation was last sent */
    VEC(inj_t) inj;
    VEC(ctl_t) ctl;
    hmap_t seen;
    hmap_t counts; /* how many turns of the previous request shared each key, before ordinals */
} state_t;

struct dcr {
    dcr_journal_t journal;
    bool has_journal;
    char *scope;
    VEC(state_t *) states; /* oldest first */
    VEC(json_t *) pending; /* placements a side thread made, recorded on the next request from the writer's thread */
};

char *dcr_render(const char *date, const char *cwd) {
    json_t *c = json_obj();
    json_set_str(c, "date", date);
    json_set_str(c, "cwd", cwd);
    char *r = prompt_render("coding-agent/prompts/system/date-cwd-reminder.md", c);
    json_free(c);
    char *trim = str_trim(r ? r : "");
    free(r);
    return trim;
}

static void inj_clear(inj_t *i) {
    free(i->key);
    free(i->reminder);
    json_free(i->source);
}

static void ctl_clear(ctl_t *c) {
    free(c->anchor);
    json_free(c->message);
}

static void state_free(state_t *s) {
    if (!s) return;
    free(s->root);
    free(s->conversation);
    free(s->reminder);
    for (size_t i = 0; i < s->inj.n; i++) inj_clear(&s->inj.v[i]);
    free(s->inj.v);
    for (size_t i = 0; i < s->ctl.n; i++) ctl_clear(&s->ctl.v[i]);
    free(s->ctl.v);
    hmap_free(&s->seen, NULL);
    hmap_free(&s->counts, NULL);
    free(s);
}

static state_t *state_new(void) {
    state_t *s = xcalloc(1, sizeof *s);
    hmap_init(&s->seen);
    hmap_init(&s->counts);
    return s;
}

static inj_t *inj_find(state_t *s, const char *key) {
    for (size_t i = 0; i < s->inj.n; i++)
        if (str_eq(s->inj.v[i].key, key)) return &s->inj.v[i];
    return NULL;
}

/* Map.set: an existing key keeps its place */
static void inj_set(state_t *s, const char *key, const char *reminder, const json_t *source) {
    inj_t *i = inj_find(s, key);
    if (!i) {
        inj_t fresh = {.key = xstrdup(key)};
        vec_push(&s->inj, fresh);
        i = &s->inj.v[s->inj.n - 1];
    } else {
        free(i->reminder);
        json_free(i->source);
    }
    i->reminder = xstrdup(reminder);
    i->source = json_clone(source);
}

static json_t *control_new(const char *reminder) {
    json_t *m = json_obj();
    json_set_str(m, "role", "developer");
    json_set_str(m, "content", reminder);
    json_set_bool(m, "synthetic", true);
    json_set_num(m, "timestamp", (double)now_ms());
    json_set_bool(m, DCR_CONTROL_MARK, true);
    return m;
}

bool dcr_is_control(const json_t *message) { return json_get_bool(message, DCR_CONTROL_MARK, false); }

static void ctl_push(state_t *s, const char *anchor, json_t *message) {
    ctl_t c = {.anchor = xstrdup(anchor), .message = message};
    vec_push(&s->ctl, c);
}

/* injectReminder: an owned copy of the turn carrying the reminder */
static json_t *inject_reminder(const json_t *message, const char *reminder) {
    json_t *c = json_clone(message);
    const json_t *content = json_get(c, "content");
    if (json_is_str(content)) {
        char *t = xasprintf("%s\n\n%s", reminder, json_sv(content));
        json_set_str(c, "content", t);
        free(t);
    } else if (json_is_arr(content)) {
        json_t *part = json_obj();
        json_set_str(part, "type", "text");
        json_set_str(part, "text", reminder);
        json_arr_insert(json_get(c, "content"), 0, part);
    }
    return c;
}

static bool starts_with_reminder(const json_t *message, const char *reminder) {
    const json_t *content = json_get(message, "content");
    if (json_is_str(content)) return str_starts(json_sv(content), reminder);
    const json_t *p0 = json_at(content, 0);
    return p0 && str_eq(json_get_str(p0, "type"), "text") && str_eq(json_get_str(p0, "text"), reminder);
}

/* #keysOf: role and timestamp and, for user and developer turns, attribution (the identity session persistence
 * uses), plus an ordinal for the second and later turns that would otherwise share a key */
static void keys_of(const json_t *messages, strvec_t *keys, hmap_t *counts) {
    const json_t *m;
    hmap_init(counts);
    json_arr_foreach(messages, i, m) {
        const char *role = json_get_str(m, "role");
        sb_t k = SB_INIT;
        sb_append(&k, role ? role : "undefined");
        sb_putc(&k, ':');
        const json_t *ts = json_get(m, "timestamp");
        if (ts && json_is_num(ts)) json_js_num_sb(&k, json_get_num(m, "timestamp", 0));
        else sb_append(&k, "undefined");
        if (str_eq(role, "user") || str_eq(role, "developer")) {
            const char *attr = json_get_str(m, "attribution");
            sb_putc(&k, ':');
            sb_append(&k, attr ? attr : "");
        }
        intptr_t count = (intptr_t)hmap_get(counts, sb_cstr(&k));
        hmap_put(counts, sb_cstr(&k), (void *)(count + 1));
        if (!count) {
            strvec_push(keys, sb_cstr(&k));
        } else {
            strvec_push_own(keys, xasprintf("%s#%lld", sb_cstr(&k), (long long)count));
        }
        sb_free(&k);
    }
}

static void counts_copy(hmap_t *to, const hmap_t *from) {
    hmap_init(to);
    hmap_foreach(from, it) hmap_put(to, it->key, it->val);
}

/* the trailing `#n` that tells apart turns of one request sharing a key: offset of '#', or -1 */
static ssize_t ordinal_at(const char *key, long *ordinal) {
    const char *h = strrchr(key, '#');
    if (!h || !h[1]) return -1;
    for (const char *p = h + 1; *p; p++)
        if (*p < '0' || *p > '9') return -1;
    *ordinal = strtol(h + 1, NULL, 10);
    return h - key;
}

/* #followOrdinals' shift: the key a turn holds now (owned), NULL when it is gone */
static char *ordinal_shift(const char *key, const hmap_t *old_counts, const hmap_t *counts, bool root_changed) {
    long ordinal = 0;
    ssize_t at = ordinal_at(key, &ordinal);
    char *base = at >= 0 ? xstrndup(key, (size_t)at) : xstrdup(key);
    long now = (long)(intptr_t)hmap_get(counts, base);
    long lost = (long)(intptr_t)hmap_get(old_counts, base) - now;
    char *out;
    if (lost <= 0) out = xstrdup(key);
    else if (!root_changed) out = ordinal < now ? xstrdup(key) : NULL;
    else {
        long shifted = ordinal - lost;
        out = shifted < 0 ? NULL : shifted == 0 ? xstrdup(base) : xasprintf("%s#%ld", base, shifted);
    }
    free(base);
    return out;
}

/* #followOrdinals: move a conversation's state to the ordinals its turns hold now. With a new first turn the history
 * lost its start (a compaction), so every survivor's ordinal falls by the number of turns lost; with the same first
 * turn it only lost its end (an aborted turn's messages are not resent). */
static void follow_ordinals(state_t *s, const hmap_t *counts, bool root_changed) {
    size_t w = 0;
    for (size_t i = 0; i < s->inj.n; i++) {
        char *shifted = ordinal_shift(s->inj.v[i].key, &s->counts, counts, root_changed);
        if (!shifted) {
            inj_clear(&s->inj.v[i]);
            continue;
        }
        free(s->inj.v[i].key);
        s->inj.v[i].key = shifted;
        /* Map semantics: a shifted key landing on a kept one overwrites its value in place */
        ssize_t dup = -1;
        for (size_t k = 0; k < w; k++)
            if (str_eq(s->inj.v[k].key, shifted)) dup = (ssize_t)k;
        if (dup >= 0) {
            inj_clear(&s->inj.v[dup]);
            s->inj.v[dup] = s->inj.v[i];
        } else {
            s->inj.v[w++] = s->inj.v[i];
        }
    }
    s->inj.n = w;
    hmap_t seen;
    hmap_init(&seen);
    hmap_foreach(&s->seen, it) {
        char *shifted = ordinal_shift(it->key, &s->counts, counts, root_changed);
        if (shifted) hmap_put(&seen, shifted, (void *)1);
        free(shifted);
    }
    hmap_free(&s->seen, NULL);
    s->seen = seen;
    w = 0;
    for (size_t i = 0; i < s->ctl.n; i++) {
        char *shifted = ordinal_shift(s->ctl.v[i].anchor, &s->counts, counts, root_changed);
        if (!shifted) {
            ctl_clear(&s->ctl.v[i]);
            continue;
        }
        free(s->ctl.v[i].anchor);
        s->ctl.v[i].anchor = shifted;
        s->ctl.v[w++] = s->ctl.v[i];
    }
    s->ctl.n = w;
    hmap_free(&s->counts, NULL);
    counts_copy(&s->counts, counts);
}

/* #carriedReminder: the newest reminder this request's turns still carry (borrowed), NULL when none */
static const char *carried_reminder(state_t *s, const strvec_t *keys) {
    for (size_t i = keys->n; i-- > 0;) {
        const char *key = keys->v[i];
        for (size_t c = s->ctl.n; c-- > 0;)
            if (str_eq(s->ctl.v[c].anchor, key) && json_sv(json_get(s->ctl.v[c].message, "content")))
                return json_sv(json_get(s->ctl.v[c].message, "content"));
        inj_t *in = inj_find(s, key);
        if (in) return in->reminder;
    }
    return NULL;
}

static void set_reminder(state_t *s, const char *r) {
    char *c = r ? xstrdup(r) : NULL;
    free(s->reminder);
    s->reminder = c;
}

/* #predecessorOf: the conversation that sent the most of this request's turns, the most recent on a tie */
static state_t *predecessor_of(dcr_t *d, const strvec_t *keys) {
    state_t *best = NULL;
    size_t best_shared = 0;
    for (size_t i = 0; i < d->states.n; i++) {
        state_t *s = d->states.v[i];
        size_t shared = 0;
        for (size_t k = 0; k < keys->n; k++)
            if (hmap_has(&s->seen, keys->v[k]) || inj_find(s, keys->v[k])) shared++;
        if (shared > 0 && shared >= best_shared) {
            best = s;
            best_shared = shared;
        }
    }
    return best;
}

/* firstTurnDigest: identifies a first turn by what it carries; a re-created copy digests the same */
static char *first_turn_digest(const json_t *message) {
    const char *attr = json_get_str(message, "attribution");
    json_t *pair = json_arr();
    json_push_str(pair, attr ? attr : "");
    const json_t *content = json_get(message, "content");
    json_push(pair, content ? json_clone(content) : json_null());
    char *s = json_dump_js(pair);
    json_free(pair);
    char *hex = xasprintf("%llx", (unsigned long long)gh_wyhash(s, strlen(s), 0));
    free(s);
    return hex;
}

static void record(dcr_t *d, const char *conversation, const char *turn, const char *reminder, bool control,
                   const char *first_turn) {
    if (!d->has_journal) return;
    json_t *p = json_obj();
    json_set_str(p, "conversation", conversation);
    json_set_str(p, "turn", turn);
    json_set_str(p, "reminder", reminder);
    json_set_str(p, "placement", control ? "control" : "prefix");
    if (first_turn) json_set_str(p, "firstTurn", first_turn);
    if (tl_side) vec_push(&d->pending, p);
    else d->journal.record(d->journal.ud, p);
}

/* removes the state of `root` without freeing it, NULL when absent */
static state_t *states_take(dcr_t *d, const char *root) {
    for (size_t i = 0; i < d->states.n; i++) {
        if (!str_eq(d->states.v[i]->root, root)) continue;
        state_t *s = d->states.v[i];
        memmove(&d->states.v[i], &d->states.v[i + 1], (d->states.n - i - 1) * sizeof d->states.v[0]);
        d->states.n--;
        return s;
    }
    return NULL;
}

/* #newRootState: state for a first user turn this injector has not served yet. Turns the conversation it continues
 * shares with this one keep the reminders they were sent with. A first turn equal to the one that conversation
 * started with is that turn again under a new timestamp (a context hook building it afresh on every request). */
static state_t *new_root_state(dcr_t *d, const strvec_t *keys, const hmap_t *counts, const char *root,
                               const json_t *first_user, const char *reminder, state_t *previous) {
    inj_t *prev_first = previous ? inj_find(previous, previous->root) : NULL;
    bool recreated = prev_first && str_eq(json_get_str(prev_first->source, "attribution"), json_get_str(first_user, "attribution")) &&
                     json_equal(json_get(prev_first->source, "content"), json_get(first_user, "content"));
    state_t *s = state_new();
    s->root = xstrdup(root);
    s->conversation = xstrdup(recreated ? previous->conversation : root);
    s->reminder = recreated ? (previous->reminder ? xstrdup(previous->reminder) : NULL) : xstrdup(reminder);
    if (previous) {
        for (size_t i = 0; i < previous->inj.n; i++) inj_set(s, previous->inj.v[i].key, previous->inj.v[i].reminder, previous->inj.v[i].source);
        for (size_t i = 0; i < previous->ctl.n; i++) ctl_push(s, previous->ctl.v[i].anchor, json_clone(previous->ctl.v[i].message));
        hmap_foreach(&previous->seen, it) hmap_put(&s->seen, it->key, (void *)1);
        hmap_foreach(&previous->counts, it) hmap_put(&s->counts, it->key, it->val);
    }
    follow_ordinals(s, counts, true);
    hmap_t present;
    hmap_init(&present);
    for (size_t i = 0; i < keys->n; i++) hmap_put(&present, keys->v[i], (void *)1);
    size_t w = 0;
    for (size_t i = 0; i < s->inj.n; i++) {
        if (hmap_has(&present, s->inj.v[i].key)) s->inj.v[w++] = s->inj.v[i];
        else inj_clear(&s->inj.v[i]);
    }
    s->inj.n = w;
    strvec_t gone = {0};
    hmap_foreach(&s->seen, it) if (!hmap_has(&present, it->key)) strvec_push(&gone, it->key);
    for (size_t i = 0; i < gone.n; i++) hmap_del(&s->seen, gone.v[i]);
    strvec_free(&gone);
    w = 0;
    for (size_t i = 0; i < s->ctl.n; i++) {
        if (hmap_has(&present, s->ctl.v[i].anchor)) s->ctl.v[w++] = s->ctl.v[i];
        else ctl_clear(&s->ctl.v[i]);
    }
    s->ctl.n = w;
    hmap_free(&present, NULL);
    if (recreated) {
        inj_set(s, root, prev_first->reminder, first_user);
        const char *carried = carried_reminder(s, keys);
        set_reminder(s, carried ? carried : prev_first->reminder);
        /* one conversation under a new first-turn key: the old key is never sent again */
        state_free(states_take(d, previous->root));
        return s;
    }
    if (!inj_find(s, root) && !starts_with_reminder(first_user, reminder)) inj_set(s, root, reminder, first_user);
    if (d->has_journal) {
        for (size_t i = 0; i < s->inj.n; i++) {
            char *digest = str_eq(s->inj.v[i].key, root) ? first_turn_digest(first_user) : NULL;
            record(d, root, s->inj.v[i].key, s->inj.v[i].reminder, false, digest);
            free(digest);
        }
        for (size_t i = 0; i < s->ctl.n; i++) {
            const char *text = json_sv(json_get(s->ctl.v[i].message, "content"));
            if (text) record(d, root, s->ctl.v[i].anchor, text, true, NULL);
        }
    }
    return s;
}

/* #restoredRootState: state for a conversation an earlier injector served, rebuilt from its journal. Each turn gets
 * back the reminder it was sent with, and the turns up to the last reply count as sent. A journaled turn was sent even
 * when no reply followed it (an interrupted request). NULL for a new conversation or an unjournaled transcript. */
static state_t *restored_root_state(dcr_t *d, const json_t *messages, const strvec_t *keys, const hmap_t *counts, const char *root,
                                    const json_t *first_user) {
    if (!d->has_journal || tl_side) return NULL;
    json_t *placements = d->journal.load(d->journal.ud);
    if (!json_len(placements)) {
        json_free(placements);
        return NULL;
    }
    const json_t *p;
    const char *conversation = NULL;
    json_arr_foreach(placements, i, p) {
        if (str_eq(json_get_str(p, "conversation"), root) && str_eq(json_get_str(p, "turn"), root)) {
            conversation = root;
            break;
        }
    }
    if (!conversation) {
        char *digest = first_turn_digest(first_user);
        for (size_t i = json_len(placements); i-- > 0;) {
            p = json_at(placements, i);
            if (str_eq(json_get_str(p, "firstTurn"), digest)) {
                conversation = json_get_str(p, "conversation");
                break;
            }
        }
        free(digest);
    }
    if (!conversation) {
        json_free(placements);
        return NULL;
    }
    state_t *s = state_new();
    s->root = xstrdup(root);
    s->conversation = xstrdup(conversation);
    hmap_foreach(counts, it) hmap_put(&s->counts, it->key, it->val);
    ssize_t last_sent = -1;
    for (size_t i = json_len(messages); i-- > 0;) {
        if (str_eq(json_get_str(json_at(messages, i), "role"), "assistant")) {
            last_sent = (ssize_t)i;
            break;
        }
    }
    hmap_t index_of; /* key -> index + 1 */
    hmap_init(&index_of);
    for (size_t i = 0; i < keys->n; i++) hmap_put(&index_of, keys->v[i], (void *)(intptr_t)(i + 1));
    json_arr_foreach(placements, i, p) {
        if (!str_eq(json_get_str(p, "conversation"), conversation)) continue;
        /* the conversation's first turn may be back under a new key; it is this request's first turn */
        const char *turn = json_get_str(p, "turn");
        if (str_eq(turn, conversation)) turn = root;
        intptr_t at = (intptr_t)hmap_get(&index_of, turn);
        if (!at) continue;
        size_t index = (size_t)at - 1;
        if ((ssize_t)index > last_sent) last_sent = (ssize_t)index;
        const char *text = json_get_str(p, "reminder");
        if (str_eq(json_get_str(p, "placement"), "control")) {
            ctl_push(s, turn, control_new(text));
            continue;
        }
        const json_t *source = json_at(messages, index);
        if (!str_eq(json_get_str(source, "role"), "user")) continue;
        inj_set(s, turn, text, source);
    }
    hmap_free(&index_of, NULL);
    for (ssize_t i = 0; i <= last_sent; i++) hmap_put(&s->seen, keys->v[i], (void *)1);
    set_reminder(s, carried_reminder(s, keys));
    json_free(placements);
    return s;
}

/* #activate: make the state the most recent conversation, forgetting the oldest over the cap */
static void activate(dcr_t *d, state_t *s) {
    vec_push(&d->states, s);
    while (d->states.n > DCR_MAX_ROOTS) {
        state_free(d->states.v[0]);
        memmove(&d->states.v[0], &d->states.v[1], (d->states.n - 1) * sizeof d->states.v[0]);
        d->states.n--;
    }
}

static json_t *inject(dcr_t *d, const json_t *messages, const char *reminder) {
    ssize_t first_index = -1;
    const json_t *m;
    json_arr_foreach(messages, i, m) {
        if (str_eq(json_get_str(m, "role"), "user")) {
            first_index = (ssize_t)i;
            break;
        }
    }
    if (first_index < 0) return NULL;
    const json_t *first_user = json_at(messages, (size_t)first_index);
    strvec_t keys = {0};
    hmap_t counts;
    keys_of(messages, &keys, &counts);
    const char *root = keys.v[first_index];
    state_t *state = states_take(d, root);
    if (state) {
        follow_ordinals(state, &counts, false);
        const char *carried = carried_reminder(state, &keys);
        if (carried) set_reminder(state, carried);
    } else {
        /* a conversation this injector serves already shares turns with this one (a compaction, a re-created first
         * turn): continue it in memory. Only a conversation it has never seen reads the journal. */
        state_t *predecessor = predecessor_of(d, &keys);
        if (!predecessor) state = restored_root_state(d, messages, &keys, &counts, root, first_user);
        if (!state) state = new_root_state(d, &keys, &counts, root, first_user, reminder, predecessor);
    }
    activate(d, state);
    if (!str_eq(state->reminder, reminder)) {
        /* only a turn appended after everything already sent takes the changed reminder: an unsent key earlier in the
         * history is a turn a context hook builds afresh for every request */
        ssize_t last_sent = -1;
        size_t n = json_len(messages);
        for (size_t i = n; i-- > 0;) {
            if (hmap_has(&state->seen, keys.v[i])) {
                last_sent = (ssize_t)i;
                break;
            }
        }
        ssize_t new_user = -1;
        for (ssize_t i = (ssize_t)n - 1; i > last_sent; i--) {
            if (str_eq(json_get_str(json_at(messages, (size_t)i), "role"), "user")) {
                new_user = i;
                break;
            }
        }
        size_t turn_index = new_user >= 0 ? (size_t)new_user : n - 1;
        if (new_user >= 0) inj_set(state, keys.v[new_user], reminder, json_at(messages, (size_t)new_user));
        else ctl_push(state, keys.v[turn_index], control_new(reminder));
        /* the journal names the first turn after its conversation: a hook may re-create it under a new key */
        record(d, state->conversation, turn_index == (size_t)first_index ? state->conversation : keys.v[turn_index], reminder,
               new_user < 0, NULL);
        set_reminder(state, reminder);
    }
    bool changed = false;
    json_t *out = json_arr();
    json_arr_foreach(messages, i, m) {
        const char *key = keys.v[i];
        inj_t *in = inj_find(state, key);
        if (in && str_eq(json_get_str(m, "role"), "user")) {
            /* a new copy of an injected turn gets the reminder that turn was first sent with */
            if (!json_equal(in->source, m)) {
                json_free(in->source);
                in->source = json_clone(m);
            }
            json_push(out, inject_reminder(m, in->reminder));
            changed = true;
        } else {
            json_push(out, json_clone(m));
        }
        for (size_t c = 0; c < state->ctl.n; c++) {
            if (!str_eq(state->ctl.v[c].anchor, key)) continue;
            json_push(out, json_clone(state->ctl.v[c].message));
            changed = true;
        }
        hmap_put(&state->seen, key, (void *)1);
    }
    strvec_free(&keys);
    hmap_free(&counts, NULL);
    if (!changed) {
        json_free(out);
        return NULL;
    }
    return out;
}

static void states_clear(dcr_t *d) {
    for (size_t i = 0; i < d->states.n; i++) state_free(d->states.v[i]);
    d->states.n = 0;
}

static void pending_clear(dcr_t *d, bool flush) {
    for (size_t i = 0; i < d->pending.n; i++) {
        if (flush) d->journal.record(d->journal.ud, d->pending.v[i]);
        else json_free(d->pending.v[i]);
    }
    d->pending.n = 0;
}

dcr_t *dcr_new(const dcr_journal_t *journal) {
    dcr_t *d = xcalloc(1, sizeof *d);
    if (journal) {
        d->journal = *journal;
        d->has_journal = true;
        if (d->journal.scope) d->scope = d->journal.scope(d->journal.ud);
    }
    return d;
}

void dcr_free(dcr_t *d) {
    if (!d) return;
    states_clear(d);
    free(d->states.v);
    pending_clear(d, false);
    free(d->pending.v);
    free(d->scope);
    if (d->has_journal && d->journal.destroy) d->journal.destroy(d->journal.ud);
    free(d);
}

json_t *dcr_transform(dcr_t *d, const json_t *messages, const char *date, const char *cwd) {
    if (!json_len(messages)) return NULL;
    if (d->has_journal && !tl_side) {
        char *scope = d->journal.scope ? d->journal.scope(d->journal.ud) : NULL;
        if (!str_eq(scope, d->scope)) {
            /* the history was replaced rather than continued; its turns were sent with its own reminders */
            states_clear(d);
            pending_clear(d, false);
            free(d->scope);
            d->scope = scope;
        } else {
            free(scope);
            pending_clear(d, true);
        }
    }
    char *reminder = dcr_render(date, cwd);
    json_t *out = inject(d, messages, reminder);
    free(reminder);
    return out;
}

/* ---------- the session journal (createSessionDateCwdReminderJournal) ---------- */

typedef struct {
    session_t **sp;
    char *session;
    char *leaf;
    long generation;
    hmap_t written; /* placements this journal wrote; any other on the path since the last request has another writer */
} sess_journal_t;

/* placementOfEntry: an owned copy of the entry's data, NULL when it is no valid placement */
static json_t *placement_of_entry(const json_t *entry) {
    if (!str_eq(json_get_str(entry, "type"), "custom") || !str_eq(json_get_str(entry, "customType"), DCR_CUSTOM_TYPE)) return NULL;
    const json_t *data = json_get(entry, "data");
    if (!json_is_obj(data)) return NULL;
    const char *conversation = json_get_str(data, "conversation"), *turn = json_get_str(data, "turn");
    const char *reminder = json_get_str(data, "reminder"), *placement = json_get_str(data, "placement");
    if (!conversation || !turn || !reminder) return NULL;
    if (!str_eq(placement, "prefix") && !str_eq(placement, "control")) return NULL;
    json_t *p = json_obj();
    json_set_str(p, "conversation", conversation);
    json_set_str(p, "turn", turn);
    json_set_str(p, "reminder", reminder);
    json_set_str(p, "placement", placement);
    if (json_get_str(data, "firstTurn")) json_set_str(p, "firstTurn", json_get_str(data, "firstTurn"));
    return p;
}

static json_t *sj_load(void *ud) {
    sess_journal_t *j = ud;
    session_t *s = *j->sp;
    if (!s) return NULL;
    json_t *branch = session_path_to(s, s->leaf_id);
    /* the model's history starts after the latest /clear; earlier placements belong to a history it no longer sees */
    size_t start = 0;
    for (size_t i = json_len(branch); i-- > 0;) {
        if (str_eq(json_get_str(json_at(branch, i), "type"), "reset_boundary")) {
            start = i + 1;
            break;
        }
    }
    json_t *out = json_arr();
    for (size_t i = start; i < json_len(branch); i++) {
        json_t *p = placement_of_entry(json_at(branch, i));
        if (p) json_push(out, p);
    }
    json_free(branch);
    return out;
}

static void sj_record(void *ud, json_t *placement) {
    sess_journal_t *j = ud;
    session_t *s = *j->sp;
    if (!s) {
        json_free(placement);
        return;
    }
    const char *id = session_append_custom(s, DCR_CUSTOM_TYPE, placement);
    if (id) hmap_put(&j->written, id, (void *)1);
}

/* whether the branch ending at `id` continues the one ending at `ancestor` with entries only this journal's injector
 * placed reminders in; a placement another writer appended means turns this injector never sent */
static bool sj_continues(sess_journal_t *j, session_t *s, const char *id, const char *ancestor) {
    if (!ancestor) return true;
    for (const char *cursor = id; cursor;) {
        if (str_eq(cursor, ancestor)) return true;
        const json_t *entry = session_get(s, cursor);
        if (entry) {
            json_t *p = placement_of_entry(entry);
            bool foreign = p && !hmap_has(&j->written, cursor);
            json_free(p);
            if (foreign) return false;
        }
        cursor = entry ? json_get_str(entry, "parentId") : NULL;
    }
    return false;
}

static char *sj_scope(void *ud) {
    sess_journal_t *j = ud;
    session_t *s = *j->sp;
    if (!s) return NULL;
    const char *id = session_id(s);
    const char *current = s->leaf_id;
    if (!str_eq(id, j->session) || !sj_continues(j, s, current, j->leaf)) j->generation++;
    char *si = xstrdup(id);
    free(j->session);
    j->session = si;
    char *le = current ? xstrdup(current) : NULL;
    free(j->leaf);
    j->leaf = le;
    return xasprintf("%s#%ld", si, j->generation);
}

static void sj_destroy(void *ud) {
    sess_journal_t *j = ud;
    free(j->session);
    free(j->leaf);
    hmap_free(&j->written, NULL);
    free(j);
}

dcr_journal_t dcr_session_journal(session_t **sp) {
    sess_journal_t *j = xcalloc(1, sizeof *j);
    j->sp = sp;
    hmap_init(&j->written);
    return (dcr_journal_t){.ud = j, .load = sj_load, .record = sj_record, .scope = sj_scope, .destroy = sj_destroy};
}
