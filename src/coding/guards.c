/* guards.c - tool-call loop guard, todo reminders, empty/unexpected stop recovery (see guards.h). */
#include "coding/costgate.h"
#include "coding/guards.h"

#include "coding/app.h"
#include "coding/evalhost.h"
#include "config/settings.h"
#include "prompt/template.h"
#include "text/regex.h"
#include "text/rxbt.h"
#include "tools/jobs.h"
#include "tools/toolcmd.h"

#include <ctype.h>
#include <pthread.h>

#define RESULT_SUMMARY_LIMIT 200
#define ARGUMENT_SUMMARY_LIMIT 400
#define MID_RUN_NUDGE_MUTATION_THRESHOLD 12
#define MID_RUN_NUDGE_MAX_PER_CYCLE 2
#define MID_RUN_NUDGE_BACKOFF_DOUBLINGS 4
#define UNEXPECTED_STOP_MAX_RETRIES 3
#define UNEXPECTED_STOP_TIMEOUT_MS 15000
#define EMPTY_STOP_MAX_RETRIES 3
#define REQUEST_CHARS 2000

struct guards {
    pthread_mutex_t mu;
    /* ToolCallLoopGuard */
    char *loop_hash;
    int loop_count;
    /* TodoTracker */
    int todo_reminders;
    bool todo_awaiting;
    int todo_mutations;
    int todo_nudges;
    bool stop_reminders_off;
    /* TurnRecovery */
    int empty_retries;
    int unexpected_retries;
};

guards_t *guards_new(void) {
    guards_t *g = xcalloc(1, sizeof *g);
    pthread_mutex_init(&g->mu, NULL);
    return g;
}

void guards_free(guards_t *g) {
    if (!g) return;
    free(g->loop_hash);
    pthread_mutex_destroy(&g->mu);
    free(g);
}

void guards_reset_cycle(app_t *a) {
    guards_t *g = a->guards;
    if (!g) return;
    pthread_mutex_lock(&g->mu);
    g->todo_reminders = 0;
    g->todo_awaiting = false;
    g->todo_mutations = 0;
    g->todo_nudges = 0;
    g->empty_retries = 0;
    g->unexpected_retries = 0;
    pthread_mutex_unlock(&g->mu);
}

void guards_disable_stop_reminders(app_t *a) {
    if (a->guards) a->guards->stop_reminders_off = true;
}

/* ---------- helpers ---------- */

static size_t cp_count(const char *s) {
    size_t n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

/* summarizeText: collapse whitespace runs, trim, cap at `limit` chars + "…" */
static char *summarize_text(const char *text, size_t limit) {
    sb_t b = SB_INIT;
    for (const char *p = text ? text : ""; *p;) {
        if (isspace((unsigned char)*p)) {
            while (*p && isspace((unsigned char)*p)) p++;
            sb_putc(&b, ' ');
        } else sb_putc(&b, *p++);
    }
    char *s = sb_detach(&b);
    char *t = str_trim(s);
    free(s);
    if (cp_count(t) <= limit) return t;
    size_t n = 0, i = 0;
    for (; t[i]; i++) {
        if (((unsigned char)t[i] & 0xC0) != 0x80 && n++ == limit) break;
    }
    char *r = xasprintf("%.*s…", (int)i, t);
    free(t);
    return r;
}

static json_t *canonicalize(const json_t *v) {
    if (json_is_arr(v)) {
        json_t *out = json_arr(), *x;
        json_arr_foreach(v, i, x) json_push(out, canonicalize(x));
        return out;
    }
    if (!json_is_obj(v)) return json_clone(v);
    strvec_t keys = {0};
    const char *k;
    json_t *x;
    json_obj_foreach(v, i, k, x) if (strcmp(k, "i") && strcmp(k, "__intent")) strvec_push(&keys, k);
    strvec_sort(&keys);
    json_t *out = json_obj();
    for (size_t i = 0; i < keys.n; i++) json_set(out, keys.v[i], canonicalize(json_get(v, keys.v[i])));
    strvec_free(&keys);
    return out;
}

char *guards_canonical_args(const json_t *args) {
    json_t *c = canonicalize(args ? args : json_null());
    char *s = json_dump(c, 0);
    json_free(c);
    return s;
}

static char *result_text(const json_t *msg) {
    sb_t b = SB_INIT;
    bool first = true;
    json_t *blk;
    json_arr_foreach(json_get(msg, "content"), i, blk) {
        if (!str_eq(json_get_str(blk, "type"), "text")) continue;
        if (!first) sb_putc(&b, '\n');
        sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
        first = false;
    }
    return sb_detach(&b);
}

/* hidden agent-attributed custom message (converted to a developer message for the model) */
static json_t *custom_msg(const char *type, const char *content, json_t *details) {
    json_t *m = msg_developer_text(content);
    json_set_str(m, "attribution", "agent");
    json_set_str(m, "customType", type);
    json_set_bool(m, "display", false);
    if (details) json_set(m, "details", details);
    return m;
}

static bool exempt_tool(const char *name) {
    json_t *ex = settings_get("model.toolCallLoopGuard.exemptTools");
    if (!json_is_arr(ex)) return str_eq(name, "wait"); /* DEFAULT_TOOL_CALL_LOOP_EXEMPT_TOOLS */
    json_t *x;
    json_arr_foreach(ex, i, x) if (json_is_str(x) && *json_sv(x) && str_eq(json_sv(x), name)) return true;
    return false;
}

static bool exempt_call(const json_t *call) {
    if (exempt_tool(json_get_str(call, "name"))) return true;
    /* exemptionAlias toolCommandCallName: a `bash` call running an exempt tool command */
    char *cmd = toolcmd_call_name(json_get_str(call, "name"), json_get(call, "arguments"));
    bool r = cmd && exempt_tool(cmd);
    free(cmd);
    return r;
}

static const json_t *find_result(const json_t *results, const char *id) {
    json_t *r;
    json_arr_foreach(results, i, r) if (str_eq(json_get_str(r, "toolCallId"), id)) return r;
    return NULL;
}

/* ---------- tool-call loop guard ---------- */

static void loop_guard_turn(app_t *a, const json_t *msg, const json_t *results) {
    guards_t *g = a->guards;
    if (!settings_bool("model.toolCallLoopGuard.enabled", true)) {
        free(g->loop_hash);
        g->loop_hash = NULL;
        g->loop_count = 0;
        return;
    }
    int64_t threshold = settings_int("model.toolCallLoopGuard.threshold", 5);
    if (threshold < 1) threshold = 1;
    json_t *calls = json_arr(), *blk;
    json_arr_foreach(json_get(msg, "content"), i, blk) if (str_eq(json_get_str(blk, "type"), "toolCall")) json_push(calls, json_clone(blk));
    if (!json_len(calls)) {
        free(g->loop_hash);
        g->loop_hash = NULL;
        g->loop_count = 0;
        json_free(calls);
        return;
    }
    bool all_exempt = true;
    json_t *c;
    json_arr_foreach(calls, i, c) all_exempt = all_exempt && exempt_call(c);
    if (all_exempt) {
        /* an idle exempt poll (every result flagged useless) does not end the repetition */
        bool idle = true;
        json_arr_foreach(calls, i, c) idle = idle && json_get_bool(find_result(results, json_get_str(c, "id")), "useless", false);
        if (!idle) {
            free(g->loop_hash);
            g->loop_hash = NULL;
            g->loop_count = 0;
        }
        json_free(calls);
        return;
    }
    strvec_t canon = {0};
    json_arr_foreach(calls, i, c) {
        char *args = guards_canonical_args(json_get(c, "arguments"));
        json_t *pair = json_arr();
        json_push(pair, json_str(json_get_str(c, "name") ? json_get_str(c, "name") : ""));
        json_push(pair, json_parse(args, strlen(args), NULL));
        strvec_push_own(&canon, json_dump(pair, 0));
        json_free(pair);
        free(args);
    }
    strvec_sort(&canon);
    char *hash = strvec_join(&canon, "\x1f");
    strvec_free(&canon);
    if (g->loop_hash && !strcmp(hash, g->loop_hash)) {
        g->loop_count++;
        free(hash);
    } else {
        free(g->loop_hash);
        g->loop_hash = hash;
        g->loop_count = 1;
    }
    if (g->loop_count < threshold) {
        json_free(calls);
        return;
    }
    const json_t *report = json_at(calls, 0);
    json_arr_foreach(calls, i, c) if (!exempt_call(c)) {
        report = c;
        break;
    }
    char *rtext = result_text(find_result(results, json_get_str(report, "id")));
    char *rsum = summarize_text(rtext, RESULT_SUMMARY_LIMIT);
    free(rtext);
    char *atext = guards_canonical_args(json_get(report, "arguments"));
    char *asum = summarize_text(atext, ARGUMENT_SUMMARY_LIMIT);
    free(atext);
    const char *tool = json_get_str(report, "name") ? json_get_str(report, "name") : "";
    json_t *ctx = json_obj();
    json_set_str(ctx, "tool_name", tool);
    json_set_int(ctx, "count", g->loop_count);
    json_set_str(ctx, "arguments_summary", asum);
    json_set_str(ctx, "result_summary", *rsum ? rsum : "(no text result)");
    char *content = prompt_render("coding-agent/prompts/system/tool-call-loop-redirect.md", ctx);
    json_free(ctx);
    json_t *details = json_obj();
    json_set_str(details, "toolName", tool);
    json_set_int(details, "count", g->loop_count);
    json_set_str(details, "argumentsSummary", asum);
    json_set_str(details, "resultSummary", rsum);
    agent_steer(a->agent, custom_msg("tool-call-loop-redirect", content ? content : "", details));
    free(content);
    free(asum);
    free(rsum);
    json_free(calls);
}

/* ---------- todo tracker ---------- */

static bool mutating_tool(const char *name) {
    return str_eq(name, "bash") || str_eq(name, "powershell") || str_eq(name, "eval") || str_eq(name, "edit") ||
           str_eq(name, "write") || str_eq(name, "ast_edit");
}

static bool todo_active(app_t *a) {
    if (!settings_bool("todo.enabled", true) || !a->agent) return false;
    bool on = false;
    pthread_mutex_lock(&a->agent->mu);
    for (size_t i = 0; i < a->agent->tools.n; i++) on = on || str_eq(a->agent->tools.v[i]->name, "todo");
    pthread_mutex_unlock(&a->agent->mu);
    return on;
}

/* incomplete todos grouped by phase: [{name, tasks:[{content, status}]}] (owned) */
static json_t *incomplete_by_phase(app_t *a, size_t *count) {
    json_t *out = json_arr();
    *count = 0;
    pthread_mutex_lock(&a->env->mu);
    json_t *p;
    json_arr_foreach(a->env->todo.phases, i, p) {
        json_t *tasks = json_arr(), *t;
        json_arr_foreach(json_get(p, "tasks"), k, t) {
            const char *st = json_get_str(t, "status");
            if (!str_eq(st, "pending") && !str_eq(st, "in_progress")) continue;
            json_t *x = json_obj();
            json_set_str(x, "content", json_get_str(t, "content") ? json_get_str(t, "content") : "");
            json_set_str(x, "status", st);
            json_push(tasks, x);
        }
        if (!json_len(tasks)) {
            json_free(tasks);
            continue;
        }
        *count += json_len(tasks);
        json_t *ph = json_obj();
        json_set_str(ph, "name", json_get_str(p, "name") ? json_get_str(p, "name") : "");
        json_set(ph, "tasks", tasks);
        json_push(out, ph);
    }
    pthread_mutex_unlock(&a->env->mu);
    return out;
}

static size_t todo_phase_count(app_t *a) {
    pthread_mutex_lock(&a->env->mu);
    size_t n = json_len(a->env->todo.phases);
    pthread_mutex_unlock(&a->env->mu);
    return n;
}

void guards_mid_run_nudge(app_t *a, const agent_event_t *ev) {
    guards_t *g = a->guards;
    if (!g || ev->type != AGENT_EV_TURN_END || !ev->message) return;
    bool has_tools = false;
    json_t *blk;
    json_arr_foreach(json_get(ev->message, "content"), i, blk) has_tools = has_tools || str_eq(json_get_str(blk, "type"), "toolCall");
    if (!has_tools) return; /* a turn boundary inside the run, not the stop */
    /* todo.midRunNudges (omp a80b985192, dab28d7222): off, capped (twice per cycle at 12) or backoff (12, 24, 48, 96,
     * then every 192 mutating results) */
    const char *mode = settings_str("todo.midRunNudges", "capped");
    if (str_eq(mode, "off")) return;
    bool backoff = str_eq(mode, "backoff");
    if (!backoff && g->todo_nudges >= MID_RUN_NUDGE_MAX_PER_CYCLE) return;
    int threshold = MID_RUN_NUDGE_MUTATION_THRESHOLD << (backoff ? MIN(g->todo_nudges, MID_RUN_NUDGE_BACKOFF_DOUBLINGS) : 0);
    if (g->todo_mutations < threshold) return;
    if (!settings_bool("todo.reminders", true) || a->plan_mode || !todo_active(a)) return;
    size_t n = 0;
    json_t *inc = incomplete_by_phase(a, &n);
    json_free(inc);
    if (!n) return;
    g->todo_mutations = 0;
    g->todo_nudges++;
    json_t *ctx = json_obj(), *refs = json_obj();
    json_set_str(refs, "task", "task");
    json_set_str(refs, "todo", "todo");
    json_set(ctx, "toolRefs", refs);
    json_set_int(ctx, "incompleteCount", (int64_t)n);
    json_set_bool(ctx, "plural", n != 1);
    char *content = prompt_render("coding-agent/prompts/system/mid-run-todo-nudge.md", ctx);
    json_free(ctx);
    agent_steer(a->agent, custom_msg("mid-run-todo-nudge", content ? content : "", NULL));
    free(content);
}

void guards_on_event(app_t *a, const agent_event_t *ev) {
    guards_t *g = a->guards;
    if (!g) return;
    if (ev->type == AGENT_EV_TOOL_END) {
        const char *name = ev->tool_name;
        if (str_eq(name, "todo")) {
            g->todo_mutations = 0;
            const char *op = ev->result ? json_get_str(ev->result->details, "op") : NULL;
            if (!ev->is_error && !str_eq(op, "view")) g->todo_nudges = 0; /* setPhases re-arms the nudge budget */
        } else if (!ev->is_error && mutating_tool(name)) {
            g->todo_mutations++;
        }
        g->todo_awaiting = false;
    } else if (ev->type == AGENT_EV_TURN_END && ev->message && str_eq(json_get_str(ev->message, "role"), "assistant")) {
        loop_guard_turn(a, ev->message, ev->tool_results);
    }
}

/* ----- isAwaitingUserAnswer (todo-tracker.ts) ----- */

static bool rx_test(const char *pat, int flags, const char *s) {
    rxbt_t *r = rxbt_compile(pat, flags, NULL);
    if (!r) return false;
    bool hit = rxbt_search(r, s, strlen(s), 0, NULL, 0);
    rxbt_free(r);
    return hit;
}

/* String.prototype.replace with a regex: `group` > 0 substitutes that capture, 0 removes the match */
static char *rx_sub(const char *pat, int flags, const char *s, bool global, int group) {
    rxbt_t *r = rxbt_compile(pat, flags, NULL);
    if (!r) return xstrdup(s);
    int ng = rxbt_ngroups(r);
    ptrdiff_t *caps = xcalloc((size_t)ng * 2, sizeof *caps);
    size_t len = strlen(s), pos = 0, last = 0;
    sb_t b = SB_INIT;
    while (pos <= len && rxbt_search(r, s, len, pos, caps, ng * 2)) {
        size_t ms = (size_t)caps[0], me = (size_t)caps[1];
        sb_appendn(&b, s + last, ms - last);
        if (group > 0 && group < ng && caps[group * 2] >= 0)
            sb_appendn(&b, s + caps[group * 2], (size_t)(caps[group * 2 + 1] - caps[group * 2]));
        last = me;
        if (!global) break;
        pos = me > ms ? me : me + 1;
    }
    sb_appendn(&b, s + last, len - last);
    free(caps);
    rxbt_free(r);
    return sb_detach(&b);
}

static char *trim_own(char *s) {
    char *t = str_trim(s);
    free(s);
    return t;
}

#define RE_INLINE_EMPHASIS "(\\*\\*|__)(.*?)\\1"
#define RE_MARKDOWN_PREFIX "^(?:>\\s*)?(?:(?:[-*+]|\\d+[.)])\\s+)*"
#define RE_PROMPT_LABEL "^(?:q(?:uestion)?|ask)\\s*\\d*\\s*[:.)-]\\s*"
#define RE_WRAPPED_EMPHASIS "^(\\*\\*\\*|\\*\\*|\\*|___|__|_)([\\s\\S]+)\\1$"
#define RE_OPTION_LINE "^(?:[-*+]|\\d+[.)])\\s+\\S"
#define RE_RECOMMENDATION "^(?:i(?:'d| would)?\\s+recommend|(?:my\\s+)?recommendation)\\b"
#define RE_CHOICE_CONFIRMATION "^(?:go|proceed|continue|stick)\\s+with\\b"
#define RE_QUESTION_PROMPT                                                                                             \
    "^(?:what|which|when|where|why|how|who|whom|whose|do|does|did|can|could|would|will|should|is|are|am|may|shall)\\b"
#define RE_USER_DIRECTED "\\b(?:you|your|we|our)\\b"
#define RE_ADDRESSEE "\\b(?:you|your)\\b"
#define RE_USER_RESPONSE_CUE                                                                                           \
    "^(?:please\\s+)?(?:confirm|reply|choose|pick|decide|advise|answer|approve)\\b|^(?:please\\s+)?(?:let\\s+me\\s+know|(?:" \
    "tell|message|ping)\\s+me)\\b|^(?:if|once|when)\\s+you\\s+(?:confirm|approve|agree|allow|okay|ok|give)\\b"
#define RE_LEADING_CONNECTIVE                                                                                          \
    "^(?:and|or|so|then|also|otherwise|alternatively|separately|finally)\\b,?\\s*(?=(?:please\\b|let\\s+me\\s+know\\b|(?:" \
    "tell|message|ping)\\s+me\\b|(?:if|once|when)\\s+you\\b))"

static bool non_ascii(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return true;
    return false;
}

typedef struct {
    char *text;
    bool had_label;
} pline_t;

static pline_t prompt_line(const char *line) {
    char *t = str_trim(line);
    char *a = rx_sub(RE_INLINE_EMPHASIS, 0, t, true, 2);
    char *b = trim_own(rx_sub(RE_MARKDOWN_PREFIX, 0, a, false, 0));
    char *c = trim_own(rx_sub(RE_PROMPT_LABEL, RX_ICASE, b, false, 0));
    char *d = trim_own(rx_sub(RE_WRAPPED_EMPHASIS, 0, c, false, 2));
    pline_t r = {d, strcmp(c, b) != 0};
    free(t);
    free(a);
    free(b);
    free(c);
    return r;
}

static bool ends_with_question(const char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= 1 && s[n - 1] == '?') return true;
    return n >= 3 && !memcmp(s + n - 3, "\xef\xbc\x9f", 3); /* ？ */
}

static bool is_question_prompt_line(const char *line) {
    pline_t c = prompt_line(line);
    bool r = ends_with_question(c.text) &&
             (c.had_label || rx_test(RE_QUESTION_PROMPT, RX_ICASE, c.text) || rx_test(RE_CHOICE_CONFIRMATION, RX_ICASE, c.text) ||
              rx_test(RE_USER_DIRECTED, RX_ICASE, c.text) || non_ascii(c.text));
    free(c.text);
    return r;
}

static bool is_response_cue_line(const char *line) {
    pline_t c = prompt_line(line);
    char *a = rx_sub("[.!?。！？]+$", 0, c.text, false, 0);
    char *b = trim_own(rx_sub(RE_LEADING_CONNECTIVE, RX_ICASE, a, false, 0));
    bool r = rx_test(RE_USER_RESPONSE_CUE, RX_ICASE, b);
    free(c.text);
    free(a);
    free(b);
    return r;
}

enum { ASK_NONE, ASK_QUESTION, ASK_REQUEST, ASK_CUE };

static int ask_of(const char *span) {
    if (is_response_cue_line(span)) return ASK_CUE;
    if (!is_question_prompt_line(span)) return ASK_NONE;
    pline_t c = prompt_line(span);
    int r = c.had_label || rx_test(RE_ADDRESSEE, RX_ICASE, c.text) || non_ascii(c.text) ? ASK_REQUEST : ASK_QUESTION;
    free(c.text);
    return r;
}

/* SENTENCE_BREAK_RE /(?<=[.!?…])\s+|(?<=[。！？])/ split, empties dropped */
static strvec_t split_sentences(const char *line) {
    strvec_t out = {0};
    size_t len = strlen(line), p = 0;
    for (size_t q = 0; q < len;) {
        size_t prev_end = q; /* code point ending at q */
        bool western = false, cjk = false;
        if (q >= 1 && strchr(".!?", line[q - 1])) western = true;
        if (q >= 3 && !memcmp(line + q - 3, "…", 3)) western = true;
        if (q >= 3 && (!memcmp(line + q - 3, "。", 3) || !memcmp(line + q - 3, "！", 3) || !memcmp(line + q - 3, "？", 3)))
            cjk = true;
        if (western && isspace((unsigned char)line[q])) {
            size_t e = q;
            while (e < len && isspace((unsigned char)line[e])) e++;
            if (q > p) strvec_push_own(&out, xstrndup(line + p, q - p));
            p = q = e;
            continue;
        }
        if (cjk && prev_end > p) {
            strvec_push_own(&out, xstrndup(line + p, q - p));
            p = q;
        }
        q++;
        while (q < len && ((unsigned char)line[q] & 0xC0) == 0x80) q++;
    }
    if (len > p) strvec_push_own(&out, xstrndup(line + p, len - p));
    return out;
}

/* JS split on a regex (match-at-segment-start empties advance); used for line / paragraph splits */
static strvec_t rx_split(const char *pat, const char *s) {
    strvec_t out = {0};
    rxbt_t *r = rxbt_compile(pat, 0, NULL);
    size_t len = strlen(s), p = 0, q = 0;
    ptrdiff_t caps[2];
    while (r && q < len && rxbt_search(r, s, len, q, caps, 2)) {
        size_t ms = (size_t)caps[0], me = (size_t)caps[1];
        if (ms >= len) break;
        if (me == p || me == ms) {
            q = ms + 1;
            continue;
        }
        strvec_push_own(&out, xstrndup(s + p, ms - p));
        p = q = me;
    }
    strvec_push_own(&out, xstrndup(s + p, len - p));
    if (r) rxbt_free(r);
    return out;
}

static strvec_t closing_sentences(const char *paragraph) {
    strvec_t sentences = {0};
    strvec_t lines = rx_split("\\r?\\n", paragraph);
    for (size_t li = lines.n; li > 0 && sentences.n < 2; li--) {
        char *line = str_trim(lines.v[li - 1]);
        if (rx_test(RE_OPTION_LINE, 0, line)) {
            free(line);
            break;
        }
        strvec_t ls = split_sentences(line);
        free(line);
        /* sentences.unshift(...lineSentences.slice(sentences.length - 2)) */
        size_t take = sentences.n == 0 ? 2 : 1;
        size_t from = ls.n > take ? ls.n - take : 0;
        strvec_t merged = {0};
        for (size_t k = from; k < ls.n; k++) strvec_push(&merged, ls.v[k]);
        for (size_t k = 0; k < sentences.n; k++) strvec_push(&merged, sentences.v[k]);
        strvec_free(&sentences);
        strvec_free(&ls);
        sentences = merged;
    }
    strvec_free(&lines);
    return sentences;
}

static int closing_ask(const char *paragraph) {
    const char *nl = strrchr(paragraph, '\n');
    char *last_line = str_trim(nl ? nl + 1 : paragraph);
    strvec_t s = closing_sentences(paragraph);
    int a0 = ask_of(last_line), a1 = s.n ? ask_of(s.v[s.n - 1]) : ASK_NONE;
    bool foreign = s.n == 2 && non_ascii(s.v[0]) && is_question_prompt_line(s.v[0]);
    free(last_line);
    strvec_free(&s);
    if (a0 == ASK_CUE || a1 == ASK_CUE) return ASK_CUE;
    if (foreign || a0 == ASK_REQUEST || a1 == ASK_REQUEST) return ASK_REQUEST;
    return a0 == ASK_QUESTION || a1 == ASK_QUESTION ? ASK_QUESTION : ASK_NONE;
}

bool guards_awaiting_user_answer(const char *raw) {
    char *text = str_trim(raw ? raw : "");
    if (!*text) {
        free(text);
        return false;
    }
    bool result = false;
    strvec_t paras = rx_split("\\r?\\n\\s*\\r?\\n", text);
    char *lastp = str_trim(paras.n ? paras.v[paras.n - 1] : "");
    if (closing_ask(lastp) != ASK_NONE) {
        result = true;
        goto done;
    }
    if (paras.n >= 2) {
        char *prev = str_trim(paras.v[paras.n - 2]);
        bool cue = *prev && !strchr(lastp, '\n') && !rx_test(RE_OPTION_LINE, 0, lastp) && closing_ask(prev) == ASK_CUE;
        free(prev);
        if (cue) {
            result = true;
            goto done;
        }
    }
    {
        strvec_t lines = rx_split("\\r?\\n", text);
        int options = 0;
        bool rec = false;
        for (size_t i = lines.n; i > 0; i--) {
            char *line = str_trim(lines.v[i - 1]);
            if (!*line) {
                free(line);
                continue;
            }
            if (rx_test(RE_OPTION_LINE, 0, line)) {
                options++;
                free(line);
                continue;
            }
            pline_t c = prompt_line(line);
            if (options > 0) {
                result = options >= 2 && is_question_prompt_line(line) &&
                         (rec || c.had_label || rx_test(RE_USER_DIRECTED, RX_ICASE, c.text) ||
                          rx_test(RE_CHOICE_CONFIRMATION, RX_ICASE, c.text));
                free(c.text);
                free(line);
                break;
            }
            if (rx_test(RE_RECOMMENDATION, RX_ICASE, c.text)) {
                rec = true;
                free(c.text);
                free(line);
                continue;
            }
            free(c.text);
            free(line);
            break;
        }
        strvec_free(&lines);
    }
done:
    free(lastp);
    strvec_free(&paras);
    free(text);
    return result;
}

static char *assistant_text(const json_t *msg) {
    sb_t b = SB_INIT;
    bool first = true;
    json_t *blk;
    json_arr_foreach(json_get(msg, "content"), i, blk) {
        if (!str_eq(json_get_str(blk, "type"), "text")) continue;
        if (!first) sb_putc(&b, '\n');
        sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
        first = false;
    }
    return sb_detach(&b);
}

/* TodoTracker.checkCompletion: true when a reminder was appended */
static bool todo_check_completion(app_t *a, const json_t *msg) {
    guards_t *g = a->guards;
    if (a->plan_mode) return false;
    if (g->todo_awaiting) return false;
    if (!settings_bool("todo.reminders", true) || !todo_active(a)) {
        g->todo_reminders = 0;
        g->todo_awaiting = false;
        return false;
    }
    int64_t max = settings_int("todo.remindersMax", 3);
    if (g->todo_reminders >= max) return false;
    if (!todo_phase_count(a)) {
        g->todo_reminders = 0;
        g->todo_awaiting = false;
        return false;
    }
    size_t n = 0;
    json_t *phases = incomplete_by_phase(a, &n);
    if (!n) {
        json_free(phases);
        g->todo_reminders = 0;
        g->todo_awaiting = false;
        return false;
    }
    char *text = assistant_text(msg);
    bool awaiting = guards_awaiting_user_answer(text);
    free(text);
    if (awaiting || (a->env->jobs && (jobs_running(a->env->jobs) || jobs_undelivered(a->env->jobs)))) {
        json_free(phases);
        return false;
    }
    g->todo_reminders++;
    sb_t list = SB_INIT;
    json_t *p;
    json_arr_foreach(phases, i, p) {
        if (i) sb_putc(&list, '\n');
        sb_appendf(&list, "- %s", json_get_str(p, "name"));
        json_t *t;
        json_arr_foreach(json_get(p, "tasks"), k, t) sb_appendf(&list, "\n  - %s", json_get_str(t, "content"));
    }
    char *reminder = xasprintf("<system-reminder>\nYou stopped with %zu incomplete todo item(s):\n%s\n\nPlease continue working on "
                               "these tasks or mark them complete if finished.\n(Reminder %d/%lld)\n</system-reminder>",
                               n, sb_cstr(&list), g->todo_reminders, (long long)max);
    sb_free(&list);
    /* todo_reminder session event {todos, attempt, maxAttempts}: the TUI's TodoReminderComponent, RPC's event */
    if (a->notice) {
        json_t *ev = json_obj(), *todos = json_arr();
        json_arr_foreach(phases, i, p) {
            json_t *t;
            json_arr_foreach(json_get(p, "tasks"), k, t) json_push(todos, json_clone(t));
        }
        json_set(ev, "todos", todos);
        json_set_int(ev, "attempt", g->todo_reminders);
        json_set_int(ev, "maxAttempts", max);
        char *js = json_dump(ev, 0);
        json_free(ev);
        a->notice(a->notice_ud, "todo-reminder", js);
        free(js);
    }
    json_free(phases);
    json_t *m = msg_developer_text(reminder);
    free(reminder);
    json_set_str(m, "attribution", "agent");
    g->todo_mutations = 0;
    g->todo_awaiting = true;
    if (a->session) session_append_message(a->session, m);
    agent_append_message(a->agent, m);
    return true;
}

/* ---------- stop recovery ---------- */

static bool non_ws(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return true;
    return false;
}

/* isEmptyAssistantStop */
static bool empty_stop(const json_t *msg) {
    const char *stop = json_get_str(msg, "stopReason");
    bool stop_r = str_eq(stop, "stop"), tool_r = str_eq(stop, "toolUse");
    if (!stop_r && !tool_r) return false;
    json_t *blk;
    json_arr_foreach(json_get(msg, "content"), i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "toolCall")) return false;
        if (str_eq(t, "text") && non_ws(json_get_str(blk, "text"))) return false;
        if (stop_r && str_eq(t, "image")) return false;
    }
    return true;
}

/* isUnexpectedStopCandidate */
static bool unexpected_candidate(const json_t *msg) {
    if (!str_eq(json_get_str(msg, "stopReason"), "stop")) return false;
    bool has = false;
    json_t *blk;
    json_arr_foreach(json_get(msg, "content"), i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "toolCall") || str_eq(t, "image")) return false;
        if (str_eq(t, "text") && non_ws(json_get_str(blk, "text"))) has = true;
        if (str_eq(t, "thinking") && non_ws(json_get_str(blk, "thinkingSignature"))) has = true;
    }
    return has;
}

/* drop the settled assistant turn from the live context and the persisted branch */
static void drop_assistant_turn(app_t *a, const json_t *msg) {
    int64_t ts = json_get_int(msg, "timestamp", -1);
    pthread_mutex_lock(&a->agent->mu);
    size_t n = json_len(a->agent->messages);
    json_t *last = n ? json_at(a->agent->messages, n - 1) : NULL;
    if (last && str_eq(json_get_str(last, "role"), "assistant") && json_get_int(last, "timestamp", -2) == ts)
        json_free(json_arr_remove(a->agent->messages, n - 1));
    pthread_mutex_unlock(&a->agent->mu);
    session_t *s = a->session;
    if (!s || !s->leaf_id) return;
    json_t *e = session_get(s, s->leaf_id);
    json_t *em = json_get(e, "message");
    if (!str_eq(json_get_str(e, "type"), "message") || !str_eq(json_get_str(em, "role"), "assistant") ||
        json_get_int(em, "timestamp", -2) != ts)
        return;
    const char *pid = json_get_str(e, "parentId");
    if (pid) session_branch(s, pid);
    else session_reset_leaf(s);
}

static void append_dev(app_t *a, const char *tpl, int count, int max) {
    json_t *ctx = json_obj();
    json_set_int(ctx, "retryCount", count);
    json_set_int(ctx, "maxRetries", max);
    char *txt = prompt_render(tpl, ctx);
    json_free(ctx);
    json_t *m = msg_developer_text(txt ? txt : "");
    free(txt);
    json_set_str(m, "attribution", "agent");
    agent_append_message(a->agent, m);
}

static const char *or_undef(const char *s) { return s ? s : "undefined"; }

/* 0 = not an empty stop, 1 = continuation scheduled, 2 = terminal (retry cap) */
static int handle_empty_stop(app_t *a, const json_t *msg) {
    guards_t *g = a->guards;
    if (!empty_stop(msg)) {
        g->empty_retries = 0;
        return 0;
    }
    g->empty_retries++;
    if (g->empty_retries > EMPTY_STOP_MAX_RETRIES) {
        int attempts = g->empty_retries - 1;
        /* the exhausted count stays in the log; the budget starts over so a later wake (an async result) can retry its
         * own empty replies (omp 2c708d3772) */
        g->empty_retries = 0;
        int64_t out = json_get_int(json_get(msg, "usage"), "output", 0);
        int64_t reasoning = json_get_int(json_get(msg, "usage"), "reasoningTokens", -1);
        char *err;
        if (out - MAX(0, reasoning) > 0 && json_len(json_get(msg, "content")) == 0) {
            char *rd = reasoning < 0 ? xstrdup("")
                                     : xasprintf(" (%lld reasoning token%s)", (long long)reasoning, reasoning == 1 ? "" : "s");
            err = xasprintf("Assistant returned an empty stop after retry cap; provider reported %lld output token%s%s, but no "
                            "text or tool calls were delivered. Usage alone does not establish the cause",
                            (long long)out, out == 1 ? "" : "s", rd);
            free(rd);
        } else {
            err = xstrdup("Assistant returned empty stop after retry cap; try switching models or `/shake images` to remove "
                          "archived frames");
        }
        LOGW("%s attempts=%d model=%s provider=%s outputTokens=%lld", err, attempts, or_undef(json_get_str(msg, "model")),
             or_undef(json_get_str(msg, "provider")), (long long)out);
        if (a->notice) a->notice(a->notice_ud, "error", err);
        free(err);
        drop_assistant_turn(a, msg);
        return 2;
    }
    drop_assistant_turn(a, msg);
    append_dev(a, "coding-agent/prompts/system/empty-stop-retry.md", g->empty_retries, EMPTY_STOP_MAX_RETRIES);
    /* one line per scheduled recovery, with the reply's model and stop metadata (omp 238cb78b25, 4745bf5c2b) */
    const json_t *v;
    char dur[24] = "undefined", ttft[24] = "undefined", status[24] = "undefined";
    if ((v = json_get(msg, "duration")) && json_is_num(v)) snprintf(dur, sizeof dur, "%lld", (long long)json_get_int(msg, "duration", 0));
    if ((v = json_get(msg, "ttft")) && json_is_num(v)) snprintf(ttft, sizeof ttft, "%lld", (long long)json_get_int(msg, "ttft", 0));
    if ((v = json_get(msg, "errorStatus")) && json_is_num(v))
        snprintf(status, sizeof status, "%lld", (long long)json_get_int(msg, "errorStatus", 0));
    LOGI("Empty-stop retry scheduled attempt=%d maxAttempts=%d model=%s provider=%s stopReason=%s durationMs=%s ttftMs=%s "
         "errorMessage=%s errorStatus=%s outputTokens=%lld",
         g->empty_retries, EMPTY_STOP_MAX_RETRIES, or_undef(json_get_str(msg, "model")), or_undef(json_get_str(msg, "provider")),
         or_undef(json_get_str(msg, "stopReason")), dur, ttft, or_undef(json_get_str(msg, "errorMessage")), status,
         (long long)json_get_int(json_get(msg, "usage"), "output", 0));
    return 1;
}

typedef struct {
    volatile sig_atomic_t flag;
    volatile int done;
    int64_t deadline;
    volatile sig_atomic_t *outer;
} watchdog_t;

static void *watchdog(void *p) {
    watchdog_t *w = p;
    while (!w->done) {
        if (mono_ms() >= w->deadline || (w->outer && *w->outer)) {
            w->flag = 1;
            break;
        }
        sleep_ms(20);
    }
    return NULL;
}

/* latest operator request: the newest user-typed message's text */
static char *latest_request(app_t *a) {
    char *out = NULL;
    pthread_mutex_lock(&a->agent->mu);
    for (size_t i = json_len(a->agent->messages); i > 0 && !out; i--) {
        const json_t *m = json_at(a->agent->messages, i - 1);
        if (!str_eq(json_get_str(m, "role"), "user") || json_get_str(m, "customType") || json_get_bool(m, "synthetic", false))
            continue;
        out = ai_message_text(m);
    }
    pthread_mutex_unlock(&a->agent->mu);
    return out;
}

/* classifyUnexpectedStop: 1 yes, 0 no, -1 no judge answered */
static int classify_unexpected(app_t *a, const char *message) {
    char *raw = prompt_load("coding-agent/prompts/judge/unexpected-stop.json");
    json_t *file = raw ? json_parse(raw, strlen(raw), NULL) : NULL;
    free(raw);
    json_t *spec = json_get_obj(file, "stopped");
    if (!spec) {
        json_free(file);
        return -1;
    }
    json_t *q = json_obj();
    json_set_str(q, "type", "bool");
    json_set_str(q, "instructions", json_get_str(spec, "instructions"));
    if (json_get_obj(spec, "criteria")) json_set(q, "criteria", json_clone(json_get_obj(spec, "criteria")));
    json_t *qs = json_obj();
    json_set(qs, "stopped", q);
    json_free(file);
    json_t *state = json_obj();
    char *req = latest_request(a);
    char *rt = str_trim(req ? req : "");
    free(req);
    if (*rt) {
        size_t n = 0, i = 0;
        for (; rt[i]; i++)
            if (((unsigned char)rt[i] & 0xC0) != 0x80 && n++ == REQUEST_CHARS) break;
        rt[i] = 0;
        json_set_str(state, "request", rt);
    }
    free(rt);
    json_set_str(state, "message", message);
    watchdog_t w = {.deadline = mono_ms() + UNEXPECTED_STOP_TIMEOUT_MS, .outer = &a->agent->cancel};
    pthread_t th;
    bool have = pthread_create(&th, NULL, watchdog, &w) == 0;
    double usage[3] = {0};
    char *err = NULL;
    json_t *ans = evalhost_judge(a, state, qs, &w.flag, usage, &err);
    w.done = 1;
    if (have) pthread_join(th, NULL);
    costgate_add(a, usage[2]);
    int r = -1;
    json_t *st = json_get(ans, "stopped");
    if (st && !w.flag) r = json_get_num(st, "bool", 0) >= 0.5 ? 1 : 0;
    json_free(ans);
    json_free(state);
    json_free(qs);
    free(err);
    return r;
}

static bool handle_unexpected_stop(app_t *a, const json_t *msg) {
    guards_t *g = a->guards;
    const char *mode = settings_str("features.unexpectedStopDetection", "mechanical");
    if (str_eq(mode, "none")) return false;
    if (!unexpected_candidate(msg)) {
        g->unexpected_retries = 0;
        return false;
    }
    char *text = assistant_text(msg);
    if (non_ws(text)) {
        if (!str_eq(mode, "smart") || g->unexpected_retries > 0) {
            g->unexpected_retries = 0;
            free(text);
            return false;
        }
        int v = classify_unexpected(a, text);
        if (v != 1 || a->agent->cancel) {
            g->unexpected_retries = 0;
            free(text);
            return false;
        }
    }
    free(text);
    g->unexpected_retries++;
    if (g->unexpected_retries > UNEXPECTED_STOP_MAX_RETRIES) {
        g->unexpected_retries = 0;
        return false;
    }
    append_dev(a, "coding-agent/prompts/system/unexpected-stop-retry.md", g->unexpected_retries, UNEXPECTED_STOP_MAX_RETRIES);
    return true;
}

/* the settled run's last message when it is an assistant turn (owned clone) */
static json_t *last_assistant_clone(app_t *a) {
    if (!a->guards || !a->agent) return NULL;
    pthread_mutex_lock(&a->agent->mu);
    size_t n = json_len(a->agent->messages);
    json_t *last = n ? json_clone(json_at(a->agent->messages, n - 1)) : NULL;
    pthread_mutex_unlock(&a->agent->mu);
    if (last && !str_eq(json_get_str(last, "role"), "assistant")) {
        json_free(last);
        last = NULL;
    }
    return last;
}

int guards_stop_recovery(app_t *a) {
    json_t *last = last_assistant_clone(a);
    if (!last) return 0;
    int empty = handle_empty_stop(a, last);
    int r = empty == 1 ? 1 : empty == 2 ? 2 : handle_unexpected_stop(a, last) ? 1 : 0;
    json_free(last);
    return r;
}

bool guards_todo_reminder(app_t *a) {
    json_t *last = last_assistant_clone(a);
    if (!last) return false;
    const char *stop = json_get_str(last, "stopReason");
    bool has_tools = false, cont = false;
    json_t *blk;
    json_arr_foreach(json_get(last, "content"), i, blk) has_tools = has_tools || str_eq(json_get_str(blk, "type"), "toolCall");
    if (!has_tools && !str_eq(stop, "error") && !str_eq(stop, "aborted") && !a->guards->stop_reminders_off)
        cont = todo_check_completion(a, last);
    json_free(last);
    return cont;
}
