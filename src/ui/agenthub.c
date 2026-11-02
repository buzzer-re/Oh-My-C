/* agenthub.c - subagent visibility in the interactive UI (ports of omp's tui overlays/agent-hub*.ts,
 * agent-activity.ts, agent-transcript-viewer.ts, running-subagent-badge.ts, coding-agent activity/index.ts,
 * modes/controllers/session-focus-controller.ts and the subagent HUD of modes/interactive-mode.ts):
 *
 *   - the "Subagents · N running" HUD above the working row (display.pinnedAgents, display.subagentLivePreview)
 *   - the Agent Hub (/hub, Alt+A, ←← on an empty composer): Agents roster + inspector, Activity log, r/x revive/kill
 *   - the transcript viewer (Enter on an activity row): an agent's transcript with a message box that prompts it
 *   - session focus (Enter on a roster row): the main view shows the subagent's live session until Esc / ←←
 *
 * The registry (coding/agents.c) is process-global; its change listener posts one coalesced refresh to the UI loop. */
#include "ui/ui.h"

#include "coding/agents.h"
#include "config/settings.h"
#include "tui/keys.h"

#include <time.h>

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }
static const char *sym(const char *k) { return tui_sym(k); }

/* append styled text (owned helper result) */
static void put(sb_t *b, char *owned) {
    sb_append(b, owned);
    free(owned);
}
static void putc_s(sb_t *b, tui_tc_t slot, const char *s) { put(b, S(slot, s)); }

static char *bold_fg(tui_tc_t slot, const char *s) {
    char *c = S(slot, s), *r = tui_bold(c);
    free(c);
    return r;
}

#include <ctype.h>
#include <math.h>

/* ---------- formatting (pi-utils format.ts) ---------- */

static char *fmt_duration(int64_t ms) {
    if (ms <= 0) return xstrdup("0ms");
    if (ms < 1000) return xasprintf("%lldms", (long long)ms);
    if (ms < 60000) {
        int64_t tenths = (ms + 50) / 100;
        return tenths < 600 ? xasprintf("%.1fs", (double)tenths / 10.0) : xstrdup("1m");
    }
    if (ms < 3600000) {
        int64_t m = ms / 60000, s = (ms % 60000) / 1000;
        return s > 0 ? xasprintf("%lldm%llds", (long long)m, (long long)s) : xasprintf("%lldm", (long long)m);
    }
    if (ms < 86400000) {
        int64_t h = ms / 3600000, m = (ms % 3600000) / 60000;
        return m > 0 ? xasprintf("%lldh%lldm", (long long)h, (long long)m) : xasprintf("%lldh", (long long)h);
    }
    int64_t d = ms / 86400000, h = (ms % 86400000) / 3600000;
    return h > 0 ? xasprintf("%lldd%lldh", (long long)d, (long long)h) : xasprintf("%lldd", (long long)d);
}

static char *trim1(double n) {
    char *s = xasprintf("%.1f", n);
    size_t l = strlen(s);
    if (l > 2 && !strcmp(s + l - 2, ".0")) s[l - 2] = 0;
    return s;
}

static char *fmt_number(int64_t n) {
    if (n < 1000) return xasprintf("%lld", (long long)n);
    char *t, *r;
    if (n < 10000) t = trim1((double)n / 1e3), r = xasprintf("%sK", t);
    else if (n < 1000000) return xasprintf("%lldK", (long long)llround((double)n / 1e3));
    else if (n < 10000000) t = trim1((double)n / 1e6), r = xasprintf("%sM", t);
    else if (n < 1000000000) return xasprintf("%lldM", (long long)llround((double)n / 1e6));
    else if (n < 10000000000LL) t = trim1((double)n / 1e9), r = xasprintf("%sB", t);
    else return xasprintf("%lldB", (long long)llround((double)n / 1e9));
    free(t);
    return r;
}

static char *fmt_age(int64_t secs) {
    if (!secs) return xstrdup("");
    int64_t mins = secs / 60, hours = mins / 60, days = hours / 24, weeks = days / 7, months = days / 30;
    if (months > 0) return xasprintf("%lldmo ago", (long long)months);
    if (weeks > 0) return xasprintf("%lldw ago", (long long)weeks);
    if (days > 0) return xasprintf("%lldd ago", (long long)days);
    if (hours > 0) return xasprintf("%lldh ago", (long long)hours);
    if (mins > 0) return xasprintf("%lldm ago", (long long)mins);
    return xstrdup("just now");
}

static char *age_of(int64_t last_ms) {
    int64_t s = (now_ms() - last_ms + 500) / 1000;
    return fmt_age(MAX(1, s));
}

static char *fmt_cost(double c) {
    if (!(c > 0)) c = 0;
    if (c < 0.01) return xasprintf("$%.4f", c);
    if (c < 1) return xasprintf("$%.3f", c);
    return xasprintf("$%.2f", c);
}

static char *clock_hms(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    return xasprintf("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* formatLocalDateTimeWithOffset */
static char *local_datetime(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    long off = tm.tm_gmtoff / 60;
    char sign = off >= 0 ? '+' : '-';
    off = labs(off);
    return xasprintf("%04d-%02d-%02d %02d:%02d %c%02ld:%02ld", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, sign, off / 60, off % 60);
}

/* sanitizeDisplaySingleLine: control bytes dropped, line breaks collapsed to one space */
static char *one_line(const char *s) {
    char *plain = tui_strip_ansi(s ? s : "");
    sb_t b = SB_INIT;
    for (const char *p = plain; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            while (p[1] == '\r' || p[1] == '\n') p++;
            sb_putc(&b, ' ');
        } else if (*p == '\t') sb_putc(&b, ' ');
        else if ((unsigned char)*p >= 0x20 && *p != 0x7f) sb_putc(&b, *p);
    }
    free(plain);
    return sb_detach(&b);
}

/* activityOneLine: whitespace runs collapsed, trimmed */
static char *squash(const char *s) {
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') sp = b.len > 0;
        else {
            if (sp) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

static char *trunc_w(const char *s, int w) { return tui_truncate_to_width(s, MAX(0, w), NULL); }

static char *pad_to(const char *s, int w) {
    int vw = tui_visible_width(s);
    if (vw > w) return tui_truncate_to_width(s, w, "");
    sb_t b = SB_INIT;
    sb_append(&b, s);
    tui_sb_spaces(&b, w - vw);
    return sb_detach(&b);
}

static char *key_hints2(const char *a, const char *b) {
    char *x = ui_key_hint(a), *y = ui_key_hint(b), *r = xasprintf("%s/%s", x, y);
    free(x);
    free(y);
    return r;
}

/* ---------- roster helpers (agent-hub-renderer.ts) ---------- */

static int status_order(const char *st) {
    return str_eq(st, "running") ? 0 : str_eq(st, "idle") ? 1 : str_eq(st, "parked") ? 2 : 3;
}

static tui_tc_t status_tone(const char *st) {
    return str_eq(st, "running") ? TUI_TC_ACCENT : str_eq(st, "idle") ? TUI_TC_SUCCESS : str_eq(st, "parked") ? TUI_TC_MUTED : TUI_TC_ERROR;
}

static const char *status_symbol(const char *st) {
    return sym(str_eq(st, "running") ? "status.running"
               : str_eq(st, "idle")  ? "status.enabled"
               : str_eq(st, "parked") ? "status.shadowed"
                                      : "status.aborted");
}

static char *status_glyph(const char *st) { return S(status_tone(st), status_symbol(st)); }

typedef struct {
    double cost;
    int64_t duration, requests, tools, tokens;
    int64_t ctx_tokens, ctx_window; /* 0 = unknown */
    bool active;                    /* durationKind active */
} metrics_t;

/* hubRowMetrics: observer progress (exact executor usage), else the live session's assistant messages */
static bool row_metrics(const json_t *ref, metrics_t *m) {
    memset(m, 0, sizeof *m);
    const json_t *p = json_get_obj(ref, "progress");
    if (!p) return false;
    m->cost = json_get_num(p, "cost", 0);
    m->duration = json_get_int(p, "durationMs", 0);
    m->requests = json_get_int(p, "requests", 0);
    m->tools = json_get_int(p, "toolCount", 0);
    m->tokens = json_get_int(p, "tokens", 0);
    m->ctx_tokens = json_get_int(p, "contextTokens", 0);
    m->ctx_window = json_get_int(p, "contextWindow", 0);
    m->active = true;
    return true;
}

static char *metric_duration(const metrics_t *m) {
    if (m->duration <= 0) return NULL;
    char *d = fmt_duration(m->duration), *r = xasprintf("%s %s", d, m->active ? "active" : "duration");
    free(d);
    return r;
}

static char *metrics_text(const metrics_t *m) {
    char *c = fmt_cost(m->cost), *d = metric_duration(m), *rq = fmt_number(m->requests), *tl = fmt_number(m->tools),
         *tk = fmt_number(m->tokens);
    const char *dot = sym("sep.dot");
    char *r = xasprintf("%s%s%s%s%s req%s%s tools%s%s tok", c, dot, d ? d : "time —", dot, rq, dot, tl, dot, tk);
    free(c), free(d), free(rq), free(tl), free(tk);
    return r;
}

/* formatMetricColumns: fixed cells 8L 13R 8R 9R 8R 8R, one space apart */
static char *metric_columns(const metrics_t *m, const char *age) {
    char *c = fmt_cost(m->cost), *d = metric_duration(m), *rq = fmt_number(m->requests), *tl = fmt_number(m->tools),
         *tk = fmt_number(m->tokens);
    char *cells[6];
    cells[0] = xstrdup(c);
    cells[1] = xstrdup(d ? d : "—");
    cells[2] = xasprintf("%s req", rq);
    cells[3] = xasprintf("%s tools", tl);
    cells[4] = xasprintf("%s tok", tk);
    cells[5] = xstrdup(age);
    static const int W[6] = {8, 13, 8, 9, 8, 8};
    sb_t b = SB_INIT;
    for (int i = 0; i < 6; i++) {
        int vw = tui_visible_width(cells[i]);
        char *f = i > 0 && vw > W[i] ? tui_truncate_to_width(cells[i], W[i], NULL) : xstrdup(cells[i]);
        int pad = MAX(0, W[i] - tui_visible_width(f));
        if (i) sb_putc(&b, ' ');
        if (i == 0) {
            sb_append(&b, f);
            tui_sb_spaces(&b, pad);
        } else {
            tui_sb_spaces(&b, pad);
            sb_append(&b, f);
        }
        free(f);
        free(cells[i]);
    }
    free(c), free(d), free(rq), free(tl), free(tk);
    return sb_detach(&b);
}

static char *context_gauge(int64_t tokens, int64_t window) {
    double ratio = window > 0 ? (double)tokens / (double)window : 0;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    int filled = (int)lround(ratio * 10);
    sb_t f = SB_INIT, e = SB_INIT, b = SB_INIT;
    for (int i = 0; i < filled; i++) sb_append(&f, "━");
    for (int i = filled; i < 10; i++) sb_append(&e, "─");
    if (f.len) put(&b, S(TUI_TC_ACCENT, f.s));
    if (e.len) put(&b, S(TUI_TC_DIM, e.s));
    char *t = fmt_number(tokens), *w = fmt_number(window);
    sb_appendf(&b, " %s/%s %ld%%", t, w, lround(ratio * 100));
    free(t), free(w);
    sb_free(&f), sb_free(&e);
    return sb_detach(&b);
}

/* modelBadge: the model that produced the row's work (`provider/` dropped) + the thinking level when shown */
static char *model_badge(const json_t *ref) {
    const char *resolved = json_get_str(json_get_obj(ref, "progress"), "resolvedModel");
    if (!resolved) resolved = json_get_str(ref, "model");
    if (!resolved) return NULL;
    const char *slash = strchr(resolved, '/');
    char *label = one_line(slash ? slash + 1 : resolved);
    sb_t b = SB_INIT;
    put(&b, S(TUI_TC_MUTED, label));
    free(label);
    const char *lv = json_get_str(ref, "thinking");
    ai_effort_t e;
    if (lv && json_get_bool(ref, "reasoning", false) && effort_parse(lv, &e) == 0 && e != EFFORT_OFF) {
        char key[40];
        snprintf(key, sizeof key, "thinking.%s", lv);
        const char *disp = *sym(key) ? sym(key) : lv;
        sb_putc(&b, ' ');
        put(&b, S((tui_tc_t)(TUI_TC_THINKING_OFF + (int)e), disp));
    }
    return sb_detach(&b);
}

/* the roster's one-line task: description, else the progress task, else the activity */
static const char *ref_task(const json_t *ref) {
    const json_t *p = json_get_obj(ref, "progress");
    const char *t = json_get_str(p, "description");
    if (!t) t = json_get_str(p, "task");
    if (!t) t = json_get_str(ref, "activity");
    return t;
}

/* ---------- activity rows (agent-activity.ts + activity/index.ts over in-memory transcripts) ---------- */

enum { AK_RESPONSE, AK_TOOL, AK_IRC, AK_LIFECYCLE };
enum { AS_NONE, AS_PENDING, AS_SUCCESS, AS_ERROR, AS_ABORTED };

typedef struct {
    char *id, *agent;
    int64_t ts;
    int kind, status;
    char *title, *summary, *tool;
} arow_t;

static void arow_free(arow_t *r) {
    free(r->id), free(r->agent), free(r->title), free(r->summary), free(r->tool);
}

typedef VEC(arow_t) arows_t;

static void arows_clear(arows_t *v) {
    for (size_t i = 0; i < v->n; i++) arow_free(&v->v[i]);
    v->n = 0;
}

static void arow_push(arows_t *v, const char *id, const char *agent, int64_t ts, int kind, const char *title,
                      const char *summary, int status, const char *tool) {
    arow_t r = {xstrdup(id), xstrdup(agent), ts, kind, status, xstrdup(title ? title : ""), xstrdup(summary ? summary : ""),
                tool ? xstrdup(tool) : NULL};
    vec_push(v, r);
}

static int arow_cmp(const void *a, const void *b) {
    const arow_t *x = a, *y = b;
    if (x->ts != y->ts) return x->ts < y->ts ? -1 : 1;
    return strcmp(x->id, y->id);
}

/* activityRowsFromProgress */
static void live_rows(const json_t *ref, int64_t last_update, arows_t *out) {
    const json_t *p = json_get_obj(ref, "progress");
    if (!p) return;
    const char *id = json_get_str(ref, "id");
    const json_t *recent = json_get_arr(p, "recentTools");
    for (size_t i = json_len(recent); i > 0; i--) {
        const json_t *t = json_at(recent, i - 1);
        int64_t end = json_get_int(t, "endMs", last_update);
        char *rid = xasprintf("live:%s:tool:%lld:%zu", id, (long long)end, i - 1);
        const char *tool = json_get_str(t, "tool");
        const char *args = json_get_str(t, "args");
        arow_push(out, rid, id, end, AK_TOOL, tool, args && *args ? args : tool, AS_SUCCESS, tool);
        free(rid);
    }
    const char *cur = json_get_str(p, "currentTool");
    if (cur) {
        char *rid = xasprintf("live:%s:current-tool", id);
        const char *sum = json_get_str(p, "lastIntent");
        if (!sum) sum = json_get_str(p, "currentToolArgs");
        if (!sum) sum = cur;
        arow_push(out, rid, id, json_get_int(p, "currentToolStartMs", last_update), AK_TOOL, cur, sum, AS_PENDING, cur);
        free(rid);
    }
    const char *st = json_get_str(p, "status");
    char *rid = xasprintf("live:%s:lifecycle", id);
    const char *task = json_get_str(p, "task");
    if (!task) task = json_get_str(p, "description");
    char *sum = task ? squash(task) : xstrdup("Agent activity");
    arow_push(out, rid, id, last_update, AK_LIFECYCLE, st ? st : "running", sum,
              str_eq(st, "completed") ? AS_SUCCESS : str_eq(st, "failed") ? AS_ERROR : str_eq(st, "aborted") ? AS_ABORTED : AS_PENDING, NULL);
    free(sum);
    free(rid);
    sb_t o = SB_INIT;
    const json_t *x;
    json_arr_foreach(json_get_arr(p, "recentOutput"), k, x) if (json_is_str(x)) sb_appendf(&o, "%s%s", o.len ? " " : "", json_sv(x));
    char *resp = squash(o.s ? o.s : "");
    sb_free(&o);
    if (*resp) {
        rid = xasprintf("live:%s:response", id);
        arow_push(out, rid, id, last_update, AK_RESPONSE, "Response", resp,
                  str_eq(st, "failed") ? AS_ERROR : str_eq(st, "aborted") ? AS_ABORTED : AS_PENDING, NULL);
        free(rid);
    }
    free(resp);
}

static char *first_arg(const json_t *args, const char *const *keys) {
    for (; *keys; keys++) {
        const char *v = json_get_str(args, *keys);
        if (v) {
            char *t = squash(v);
            if (*t) return t;
            free(t);
        }
    }
    return NULL;
}

/* argumentsSummary */
static char *activity_args_summary(const char *tool, const json_t *args) {
    static const char *const INTENT[] = {"i", "intent", NULL}, *const PATH[] = {"path", NULL}, *const CMD[] = {"command", NULL},
                             *const PAT[] = {"pattern", NULL},
                             *const COMMON[] = {"path", "query", "name", "task", "title", NULL};
    char *v = first_arg(args, INTENT);
    if (v) return v;
    if (str_eq(tool, "read")) return (v = first_arg(args, PATH)) ? v : xstrdup("Read resource");
    if (str_eq(tool, "grep")) {
        char *p = first_arg(args, PAT), *q = first_arg(args, PATH);
        char *r = p && q ? xasprintf("%s · %s", p, q) : p ? xstrdup(p) : q ? xstrdup(q) : xstrdup("Search text");
        free(p), free(q);
        return r;
    }
    if (str_eq(tool, "glob")) return (v = first_arg(args, PATH)) ? v : xstrdup("Map files");
    if (str_eq(tool, "bash")) return (v = first_arg(args, CMD)) ? v : xstrdup("Run command");
    if (str_eq(tool, "write") || str_eq(tool, "edit"))
        return (v = first_arg(args, PATH)) ? v : xstrdup(str_eq(tool, "write") ? "Write files" : "Edit files");
    if (str_eq(tool, "wait")) return xstrdup("Waiting for background work or a peer");
    if ((v = first_arg(args, COMMON))) return v;
    char *j = args ? json_dump(args, 0) : xstrdup("{}");
    char *s = squash(j);
    free(j);
    if (str_eq(s, "{}")) {
        free(s);
        return xstrdup(tool);
    }
    return s;
}

/* transcript rows: assistant responses and tool calls (status from their results), newest 256 */
static void transcript_rows(const char *agent, const json_t *msgs, arows_t *out) {
    size_t start = out->n;
    const json_t *m;
    json_arr_foreach(msgs, i, m) {
        const char *role = json_get_str(m, "role");
        int64_t ts = json_get_int(m, "timestamp", 0);
        if (str_eq(role, "assistant")) {
            char *text = ai_message_text(m), *resp = squash(text);
            free(text);
            if (*resp) {
                char *rid = xasprintf("%s:response:%zu", agent, i);
                arow_push(out, rid, agent, ts, AK_RESPONSE, "Response", resp,
                          json_get_bool(m, "isError", false) ? AS_ERROR : AS_SUCCESS, NULL);
                free(rid);
            }
            free(resp);
            const json_t *b;
            json_arr_foreach(json_get_arr(m, "content"), k, b) {
                if (!str_eq(json_get_str(b, "type"), "toolCall") || !json_get_str(b, "id") || !json_get_str(b, "name")) continue;
                char *rid = xasprintf("%s:tool:%s", agent, json_get_str(b, "id"));
                char *sum = activity_args_summary(json_get_str(b, "name"), json_get(b, "arguments"));
                arow_push(out, rid, agent, ts, AK_TOOL, json_get_str(b, "name"), sum, AS_PENDING, json_get_str(b, "name"));
                free(sum);
                free(rid);
            }
        } else if (str_eq(role, "toolResult") && json_get_str(m, "toolCallId")) {
            char *rid = xasprintf("%s:tool:%s", agent, json_get_str(m, "toolCallId"));
            for (size_t k = start; k < out->n; k++)
                if (str_eq(out->v[k].id, rid)) {
                    out->v[k].status = json_get_bool(m, "isError", false) ? AS_ERROR : AS_SUCCESS;
                    if (ts > out->v[k].ts) out->v[k].ts = ts;
                }
            free(rid);
        }
    }
    size_t n = out->n - start;
    if (n > 256) {
        for (size_t k = start; k < start + n - 256; k++) arow_free(&out->v[k]);
        memmove(&out->v[start], &out->v[start + n - 256], 256 * sizeof(arow_t));
        out->n = start + 256;
    }
}

/* one agent's merged rows: transcript rows not echoed by a live row, then the live rows; sorted */
static void agent_rows(const json_t *ref, arows_t *out) {
    const char *id = json_get_str(ref, "id");
    arows_t live = {0}, tr = {0};
    live_rows(ref, json_get_int(ref, "lastActivity", now_ms()), &live);
    json_t *msgs = agents_messages(id);
    transcript_rows(id, msgs, &tr);
    json_free(msgs);
    size_t start = out->n;
    for (size_t i = 0; i < tr.n; i++) {
        bool echoed = false;
        for (size_t k = 0; k < live.n && !echoed; k++) {
            const char *lt = live.v[k].tool ? live.v[k].tool : live.v[k].title, *tt = tr.v[i].tool ? tr.v[i].tool : tr.v[i].title;
            echoed = live.v[k].kind == tr.v[i].kind && str_eq(lt, tt) && str_eq(live.v[k].summary, tr.v[i].summary);
        }
        if (echoed) arow_free(&tr.v[i]);
        else vec_push(out, tr.v[i]);
    }
    for (size_t i = 0; i < live.n; i++) vec_push(out, live.v[i]);
    vec_free(&tr);
    vec_free(&live);
    qsort(&out->v[start], out->n - start, sizeof(arow_t), arow_cmp);
}

static char *activity_glyph(const arow_t *r) {
    if (r->status == AS_ERROR) return S(TUI_TC_ERROR, sym("status.error"));
    if (r->status == AS_ABORTED) return S(TUI_TC_WARNING, sym("status.aborted"));
    if (r->status == AS_PENDING) return S(TUI_TC_ACCENT, sym("status.running"));
    switch (r->kind) {
    case AK_RESPONSE: return S(TUI_TC_SUCCESS, "◆");
    case AK_TOOL: return S(TUI_TC_SUCCESS, sym("status.success"));
    case AK_IRC: return S(TUI_TC_ACCENT, "→");
    default: return S(TUI_TC_MUTED, "○");
    }
}

static const char *arow_title(const arow_t *r) { return r->kind == AK_TOOL && r->tool ? r->tool : r->title; }

/* ======================================================================================================== */
/* ---------- subagent HUD ---------- */

typedef struct {
    tui_component_t base;
    ui_t *u;
} hud_t;

/* the pinned list: subagents with an executor run in progress (observer status "active"), registration order */
static bool hud_member(const json_t *ref) {
    const char *ps = json_get_str(json_get_obj(ref, "progress"), "status");
    return str_eq(json_get_str(ref, "status"), "running") && (str_eq(ps, "running") || str_eq(ps, "pending"));
}

static char *format_task_id(const char *id) {
    char *s = one_line(id);
    char *r = str_replace(s, ".", ">");
    free(s);
    return r;
}

static char *hud_preview(const json_t *p, int width) {
    if (!str_eq(json_get_str(p, "status"), "running")) return NULL;
    const char *cur = json_get_str(p, "currentTool");
    const json_t *recent = json_at(json_get_arr(p, "recentTools"), 0);
    const char *tool = cur ? cur : json_get_str(recent, "tool");
    if (!tool) return NULL;
    const char *intent = cur ? json_get_str(p, "currentToolIntent") : json_get_str(recent, "intent");
    const char *args = cur ? json_get_str(p, "currentToolArgs") : json_get_str(recent, "args");
    const char *detail = intent ? intent : args;
    int64_t elapsed = cur && json_get_int(p, "currentToolStartMs", 0) ? now_ms() - json_get_int(p, "currentToolStartMs", 0) : 0;
    char *el = NULL;
    if (elapsed > 5000) {
        char *d = fmt_duration(elapsed), *w = S(TUI_TC_WARNING, d);
        el = xasprintf("%s%s", sym("sep.dot"), w);
        free(d), free(w);
    }
    int elw = el ? tui_visible_width(el) : 0;
    sb_t b = SB_INIT;
    put(&b, S(TUI_TC_DIM, sym("tree.hook")));
    sb_putc(&b, ' ');
    if (!cur && recent) {
        bool err = json_get_bool(recent, "isError", false);
        put(&b, S(err ? TUI_TC_ERROR : TUI_TC_SUCCESS, sym(err ? "status.error" : "status.success")));
        sb_putc(&b, ' ');
    }
    int prefix_w = tui_visible_width(b.s);
    char *rt = tui_replace_tabs(tool);
    char *st = tui_truncate_to_width(rt, MAX(0, width - prefix_w - elw), "");
    free(rt);
    put(&b, S(cur ? TUI_TC_MUTED : TUI_TC_DIM, st));
    int budget = width - prefix_w - tui_visible_width(st) - elw - 2;
    free(st);
    if (detail && budget >= 8) {
        char *one = one_line(detail), *tr = trunc_w(one, MIN(40, budget));
        sb_append(&b, ": ");
        put(&b, S(TUI_TC_DIM, tr));
        free(one), free(tr);
    }
    if (el) sb_append(&b, el);
    free(el);
    char *r = tui_truncate_to_width(b.s, width, "");
    sb_free(&b);
    return r;
}

/* renderSubagentHudLines */
static void hud_lines(ui_t *u, int columns, strvec_t *out) {
    const char *mode = settings_str("display.pinnedAgents", "collapsed");
    if (str_eq(mode, "off")) return;
    json_t *all = agents_list(), *running = json_arr(), *r;
    json_arr_foreach(all, i, r) if (hud_member(r)) json_push(running, json_clone(r));
    json_free(all);
    size_t n = json_len(running);
    if (!n) {
        json_free(running);
        return;
    }
    bool expanded = str_eq(mode, "full");
    bool live_preview = settings_bool("display.subagentLivePreview", false);
    int content = MAX(0, columns - 2);
    size_t items = expanded || n <= 3 ? n : 3;
    const char *outer = " ";
    strvec_push(out, "");
    char *title = bold_fg(TUI_TC_ACCENT, "Subagents");
    char *cnt = xasprintf("%s%zu running", sym("sep.dot"), n), *dc = S(TUI_TC_DIM, cnt);
    char *tt = xasprintf("%s%s", title, dc), *tt2 = trunc_w(tt, content);
    free(title), free(cnt), free(dc), free(tt);
    const strvec_t *hk = tui_keybindings_get(tui_keybindings_global(), "app.agents.hub");
    char *key = hk && hk->n ? ui_key_hint(hk->v[0]) : NULL;
    char *hint = key && *key ? S(TUI_TC_DIM, key) : xstrdup("");
    int gap = content - tui_visible_width(tt2) - tui_visible_width(hint);
    if (*hint && gap >= 2) {
        sb_t h = SB_INIT;
        sb_append(&h, tt2);
        tui_sb_spaces(&h, gap);
        sb_append(&h, hint);
        strvec_push_own(out, xasprintf(" %s", h.s));
        sb_free(&h);
    } else {
        strvec_push_own(out, xasprintf(" %s", tt2));
    }
    free(tt2), free(hint), free(key);
    char *dot = S(TUI_TC_ACCENT, sym("status.done"));
    for (size_t i = 0; i < items; i++) {
        const json_t *ref = json_at(running, i);
        const json_t *p = json_get_obj(ref, "progress");
        bool last = i + 1 == items;
        char *branch = S(TUI_TC_DIM, sym(last ? "tree.last" : "tree.branch"));
        char *prefix = xasprintf("%s ", branch);
        free(branch);
        int prefix_w = tui_visible_width(prefix);
        int row_w = MAX(0, content - 1 - prefix_w);
        char *fid = format_task_id(json_get_str(ref, "id"));
        char *did = tui_truncate_to_width(fid, MAX(0, row_w - 2), NULL);
        free(fid);
        sb_t line = SB_INIT;
        sb_appendf(&line, "%s ", dot);
        put(&line, bold_fg(TUI_TC_ACCENT, did));
        const char *agent = json_get_str(ref, "agent");
        if (agent && !str_eq(agent, "task")) {
            char *br = xasprintf("%s%s%s", sym("format.bracketLeft"), agent, sym("format.bracketRight"));
            char *bd = S(TUI_TC_DIM, br), *bt = xasprintf(" %s", bd);
            char *fit = tui_truncate_to_width(bt, MAX(0, row_w - tui_visible_width(line.s)), NULL);
            sb_append(&line, fit);
            free(br), free(bd), free(bt), free(fit);
        }
        free(did);
        const char *desc = json_get_str(p, "description");
        if (desc && *desc) {
            int budget = MAX(0, row_w - tui_visible_width(line.s) - 2);
            if (budget > 0) {
                char *f = one_line(desc), *t = trunc_w(f, budget);
                put(&line, S(TUI_TC_ACCENT, ":"));
                sb_putc(&line, ' ');
                put(&line, S(TUI_TC_ACCENT, t));
                free(f), free(t);
            }
        } else {
            const char *pv = json_get_str(p, "assignment");
            if (!pv) pv = json_get_str(p, "task");
            char *trim = pv ? str_trim(pv) : NULL;
            if (trim && *trim && !str_eq(trim, json_get_str(ref, "id"))) {
                /* line breaks shown as ↵ */
                sb_t f = SB_INIT;
                for (const char *c = trim; *c; c++) {
                    if (*c == '\r' || *c == '\n') {
                        while (f.len && f.s[f.len - 1] == ' ') f.s[--f.len] = 0;
                        while (c[1] == '\r' || c[1] == '\n' || c[1] == ' ' || c[1] == '\t') c++;
                        sb_append(&f, " ↵ ");
                    } else sb_putc(&f, *c == '\t' ? ' ' : *c);
                }
                int budget = MAX(0, row_w - tui_visible_width(line.s) - 1);
                if (budget > 0) {
                    char *t = trunc_w(f.s ? f.s : "", budget);
                    sb_putc(&line, ' ');
                    put(&line, S(TUI_TC_MUTED, t));
                    free(t);
                }
                sb_free(&f);
            }
            free(trim);
        }
        char *head = tui_truncate_to_width(line.s, row_w, "");
        sb_free(&line);
        char *row = xasprintf("%s%s%s", outer, prefix, head);
        char *fitrow = tui_truncate_to_width(row, content, "");
        strvec_push_own(out, xasprintf(" %s", fitrow));
        free(row), free(fitrow), free(head);
        char *pv = live_preview ? hud_preview(p, row_w) : NULL;
        if (pv) {
            char *cont = S(TUI_TC_DIM, last ? "   " : "│  ");
            char *r2 = xasprintf("%s%s%s", outer, cont, pv), *f2 = tui_truncate_to_width(r2, content, "");
            strvec_push_own(out, xasprintf(" %s", f2));
            free(cont), free(r2), free(f2), free(pv);
        }
        free(prefix);
    }
    free(dot);
    if (!expanded && n > 3) {
        char *t = xasprintf("… %zu more — expand", n - items), *d = S(TUI_TC_DIM, t);
        char *row = xasprintf("%s%s", outer, d), *f = tui_truncate_to_width(row, content, "");
        strvec_push_own(out, xasprintf(" %s", f));
        free(t), free(d), free(row), free(f);
    }
    json_free(running);
    UNUSED(u);
}

static void hud_render(tui_component_t *c, int width, strvec_t *out) {
    hud_t *h = (hud_t *)c;
    hud_lines(h->u, width, out);
}

static const tui_component_vt HUD_VT = {"subagent-hud", hud_render, NULL, NULL, NULL};

tui_component_t *ui_subagent_hud_new(ui_t *u) {
    hud_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &HUD_VT);
    h->u = u;
    u->subagent_hud = &h->base;
    return &h->base;
}

/* ======================================================================================================== */
/* ---------- transcript viewer (agent-transcript-viewer.ts) ---------- */

typedef struct viewer {
    tui_component_t base;
    ui_t *u;
    struct agent_hub *hub;
    char *id;
    tui_editor_t *ed; /* NULL: read-only */
    tui_container_t *body;
    size_t msg_count;  /* messages replayed into body */
    char *last_sig;    /* last message's shape (a streaming/changed tail rebuilds) */
    char *notice;
    char *model;
    int offset;        /* scroll offset into the body (rows from the top) */
    bool follow;       /* stick to the bottom */
    bool expanded;
    int timer;
} viewer_t;

static void viewer_close(viewer_t *v, bool hub_too);

static void viewer_reload(viewer_t *v) {
    json_t *msgs = agents_messages(v->id);
    size_t n = json_len(msgs);
    char *sig = n ? json_dump(json_at(msgs, n - 1), 0) : xstrdup("");
    if (n == v->msg_count && str_eq(sig, v->last_sig ? v->last_sig : "")) {
        free(sig);
        json_free(msgs);
        return;
    }
    free(v->last_sig);
    v->last_sig = sig;
    v->msg_count = n;
    json_t *entries = json_arr(), *m;
    free(v->model);
    v->model = NULL;
    json_arr_foreach(msgs, i, m) {
        json_t *e = json_obj();
        json_set_str(e, "type", "message");
        char *eid = xasprintf("m%zu", i);
        json_set_str(e, "id", eid);
        free(eid);
        json_set(e, "message", json_clone(m));
        json_push(entries, e);
        if (!v->model && str_eq(json_get_str(m, "role"), "assistant") && json_get_str(m, "model")) {
            const char *prov = json_get_str(m, "provider");
            v->model = prov ? xasprintf("%s/%s", prov, json_get_str(m, "model")) : xstrdup(json_get_str(m, "model"));
        }
    }
    tui_container_clear(v->body);
    size_t *ends = xcalloc(json_len(entries) + 1, sizeof *ends);
    ui_chat_replay_into(v->u, v->body, entries, ends);
    free(ends);
    json_free(entries);
    json_free(msgs);
    tui_request_render(v->u->tui);
}

static void viewer_tick(void *ud) { viewer_reload(ud); }

static char *viewer_status_badge(const char *st) {
    tui_tc_t c = str_eq(st, "running") ? TUI_TC_SUCCESS : str_eq(st, "idle") ? TUI_TC_ACCENT : str_eq(st, "parked") ? TUI_TC_MUTED : TUI_TC_ERROR;
    return S(c, st);
}

static void viewer_render(tui_component_t *c, int width, strvec_t *out) {
    viewer_t *v = (viewer_t *)c;
    int frame_w = MAX(0, width);
    int content_w = MAX(1, frame_w - 1), chrome_w = MAX(1, frame_w - 2);
    json_t *ref = agents_get(v->id);
    const char *dot = sym("sep.dot");
    strvec_t header = {0}, footer = {0};
    char *h1 = xasprintf("Agent Hub %s %s", dot, v->id);
    strvec_push_own(&header, S(TUI_TC_ACCENT, h1));
    free(h1);
    if (ref) {
        const char *parent = json_get_str(ref, "parentId");
        sb_t b = SB_INIT;
        put(&b, tui_bold(v->id));
        sb_putc(&b, ' ');
        put(&b, viewer_status_badge(json_get_str(ref, "status")));
        char *kt = parent ? xasprintf(" sub %s of %s", dot, parent) : xstrdup(" sub");
        put(&b, S(TUI_TC_DIM, kt));
        free(kt);
        if (v->model) {
            char *ml = xasprintf("%s%s", dot, v->model);
            put(&b, S(TUI_TC_MUTED, ml));
            free(ml);
        }
        strvec_push_own(&header, sb_detach(&b));
    }
    if (v->notice) {
        char *one = one_line(v->notice), *t = trunc_w(one, chrome_w);
        strvec_push_own(&footer, S(TUI_TC_ERROR, t));
        free(one), free(t);
    }
    if (v->ed) tui_component_render(&v->ed->base, chrome_w, &footer);
    /* stats line */
    const json_t *p = json_get_obj(ref, "progress");
    if (p) {
        strvec_t st = {0};
        int64_t ct = json_get_int(p, "contextTokens", 0), cw = json_get_int(p, "contextWindow", 0);
        if (ct > 0) {
            if (cw > 0) {
                char *w = fmt_number(cw);
                strvec_push_own(&st, xasprintf("%.1f%%/%s", (double)ct / (double)cw * 100.0, w));
                free(w);
            } else strvec_push_own(&st, fmt_number(ct));
        }
        int64_t dur = json_get_int(p, "durationMs", 0);
        if (dur > 0) strvec_push_own(&st, fmt_duration(dur));
        int64_t tools = json_get_int(p, "toolCount", 0);
        sb_t line = SB_INIT;
        if (st.n || tools > 0) {
            sb_t s = SB_INIT;
            if (tools > 0) {
                char *tn = fmt_number(tools);
                sb_appendf(&s, "%s %s", tn, sym("icon.extensionTool"));
                free(tn);
            }
            for (size_t i = 0; i < st.n; i++) sb_appendf(&s, "%s%s", s.len ? dot : "", st.v[i]);
            put(&line, S(TUI_TC_DIM, s.s ? s.s : ""));
            sb_free(&s);
        }
        double cost = json_get_num(p, "cost", 0);
        if (cost > 0) {
            char *cs = xasprintf("$%.2f", cost);
            if (line.len) sb_append(&line, dot);
            put(&line, S(TUI_TC_STATUS_LINE_COST, cs));
            free(cs);
        }
        if (line.len) strvec_push_own(&footer, sb_detach(&line));
        else sb_free(&line);
        strvec_free(&st);
    }
    {
        char *esc = ui_key_hint("escape"), *ex = ui_key_hint("ctrl+o"), *jk = key_hints2("j", "k"), *gg = key_hints2("g", "shift+g");
        char *keys = xasprintf("%s:close  %s:expand  %s%s:scroll  %s:top/bottom", esc, ex, v->ed ? "empty input → " : "", jk, gg);
        char *hint;
        if (v->ed) {
            char *sub = ui_editor_key("tui.input.submit");
            hint = xasprintf("%s:send  %s", sub, keys);
            free(sub);
        } else hint = xstrdup(keys);
        strvec_push_own(&footer, S(TUI_TC_DIM, hint));
        free(esc), free(ex), free(jk), free(gg), free(keys), free(hint);
    }
    /* body */
    strvec_t body = {0};
    if (!tui_container_count(v->body)) {
        char *ph = S(TUI_TC_DIM, "No messages yet.");
        strvec_push_own(&body, xasprintf(" %s", ph));
        free(ph);
    } else {
        tui_component_render(&v->body->base, content_w, &body);
    }
    int height = tui_term(v->u->tui)->rows > 0 ? tui_term(v->u->tui)->rows : 40;
    int chrome = (int)header.n + (int)footer.n + (frame_w > 0 ? 3 : 0);
    int body_h = MAX(3, height - chrome);
    int max_off = MAX(0, (int)body.n - body_h);
    if (v->follow) v->offset = max_off;
    if (v->offset > max_off) v->offset = max_off;
    if (v->offset < 0) v->offset = 0;
    sb_t rule = SB_INIT;
    for (int i = 0; i < frame_w; i++) sb_append(&rule, sym("boxSharp.horizontal"));
    char *border = S(TUI_TC_BORDER, rule.s ? rule.s : "");
    sb_free(&rule);
    if (frame_w > 0) strvec_push(out, border);
    for (size_t i = 0; i < header.n; i++) {
        char *t = trunc_w(header.v[i], MAX(0, frame_w - 1));
        strvec_push_own(out, xasprintf(" %s", t));
        free(t);
    }
    if (frame_w > 0) strvec_push(out, border);
    strvec_t vis = {0};
    int off = v->offset;
    ui_scroll_view(&body, body_h, &off, frame_w, &vis);
    for (size_t i = 0; i < vis.n; i++) strvec_push(out, vis.v[i]);
    strvec_free(&vis);
    for (size_t i = 0; i < footer.n; i++) {
        char *t = trunc_w(footer.v[i], MAX(0, frame_w - 1));
        strvec_push_own(out, xasprintf(" %s", t));
        free(t);
    }
    if (frame_w > 0) strvec_push(out, border);
    free(border);
    strvec_free(&body);
    strvec_free(&header);
    strvec_free(&footer);
    json_free(ref);
}

static void viewer_submit(tui_editor_t *ed, const char *text, void *ud) {
    viewer_t *v = ud;
    char *t = str_trim(text ? text : "");
    tui_editor_clear(ed);
    if (*t) {
        free(v->notice);
        v->notice = NULL;
        char *err = NULL;
        if (!agents_prompt(v->id, t, &err)) v->notice = err;
        v->follow = true;
    }
    free(t);
    tui_request_render(v->u->tui);
}

static bool viewer_key(tui_component_t *c, const tui_key_t *k) {
    viewer_t *v = (viewer_t *)c;
    if (tui_kb(k, "app.agents.hub") || tui_kb(k, "app.session.observe")) {
        viewer_close(v, true);
        return true;
    }
    if (tui_key_matches(k, "escape")) {
        if (v->ed && !tui_editor_is_empty(v->ed)) {
            tui_editor_clear(v->ed);
            tui_request_render(v->u->tui);
            return true;
        }
        viewer_close(v, false);
        return true;
    }
    if (tui_kb(k, "app.tools.expand")) {
        v->expanded = !v->expanded;
        tui_request_render(v->u->tui);
        return true;
    }
    bool empty = !v->ed || tui_editor_is_empty(v->ed);
    if (empty) {
        int page = MAX(1, (tui_term(v->u->tui)->rows > 0 ? tui_term(v->u->tui)->rows : 40) - 10);
        int d = 0;
        bool top = false, bottom = false;
        if (tui_key_matches(k, "j") || tui_kb(k, "tui.select.down")) d = 1;
        else if (tui_key_matches(k, "k") || tui_kb(k, "tui.select.up")) d = -1;
        else if (k->kind == TUI_KEY_PAGEDOWN) d = page;
        else if (k->kind == TUI_KEY_PAGEUP) d = -page;
        else if (tui_key_matches(k, "g")) top = true;
        else if (tui_key_matches(k, "shift+g") || (k->kind == TUI_KEY_CHAR && k->codepoint == 'G')) bottom = true;
        if (d || top || bottom) {
            if (top) v->offset = 0, v->follow = false;
            else if (bottom) v->follow = true;
            else {
                v->offset += d;
                v->follow = false;
                if (d > 0) {
                    /* reaching the end resumes following */
                    strvec_t body = {0};
                    tui_component_render(&v->body->base, MAX(1, tui_term(v->u->tui)->cols - 1), &body);
                    int rows = tui_term(v->u->tui)->rows > 0 ? tui_term(v->u->tui)->rows : 40;
                    if (v->offset >= (int)body.n - MAX(3, rows - 9)) v->follow = true;
                    strvec_free(&body);
                }
            }
            tui_request_render(v->u->tui);
            return true;
        }
    }
    if (v->ed) {
        tui_editor_handle_key(v->ed, k);
        tui_request_render(v->u->tui);
    }
    return true;
}

static void viewer_destroy(tui_component_t *c) {
    viewer_t *v = (viewer_t *)c;
    if (v->timer) tui_remove_timer(v->u->tui, v->timer);
    if (v->ed) tui_component_destroy(&v->ed->base);
    tui_component_destroy(&v->body->base);
    free(v->id), free(v->notice), free(v->model), free(v->last_sig);
    free(v);
}

static const tui_component_vt VIEWER_VT = {"agent-transcript-viewer", viewer_render, viewer_key, NULL, viewer_destroy};

/* ======================================================================================================== */
/* ---------- Agent Hub (agent-hub.ts) ---------- */

typedef struct agent_hub {
    tui_component_t base;
    ui_t *u;
    int section;      /* 0 agents, 1 activity */
    json_t *rows;     /* roster in display order */
    json_t *ranks;    /* id -> [batch, statusOrder, lastActivity] frozen when first seen */
    int spawn_batches;
    bool opened;
    int sel, act_sel;
    bool follow;
    int filter;       /* all, errors, responses, tools */
    int scope;        /* all, agent, subtree */
    bool tree;
    ui_input_t agent_filter, act_search;
    bool agent_filter_editing, act_search_editing;
    char *notice;
    int detail_scroll;
    char *detail_id;
    int64_t last_left_tap;
    bool narrow_details;
    bool split;       /* last render's layout */
    arows_t acts;     /* every agent's merged rows */
    VEC(size_t) act_view; /* filtered rows (indices into acts) */
    json_t *depth, *parent, *last_sib; /* tree projection */
    int max_depth;
    int age_timer;
    viewer_t *viewer;
} hub_t;

static const char *const FILTERS[] = {"all", "errors", "responses", "tools"};

static hub_t *g_sort_hub;
static int rank_cmp(const void *x, const void *y) {
    const json_t *a = *(const json_t *const *)x, *b = *(const json_t *const *)y;
    const json_t *ra = json_get(g_sort_hub->ranks, json_get_str(a, "id")), *rb = json_get(g_sort_hub->ranks, json_get_str(b, "id"));
    int64_t d;
    if ((d = json_get_int(ra, "b", 0) - json_get_int(rb, "b", 0))) return d < 0 ? -1 : 1;
    if ((d = json_get_int(ra, "s", 0) - json_get_int(rb, "s", 0))) return d < 0 ? -1 : 1;
    if ((d = json_get_int(rb, "l", 0) - json_get_int(ra, "l", 0))) return d < 0 ? -1 : 1;
    return strcmp(json_get_str(a, "id"), json_get_str(b, "id"));
}

static bool fuzzy_has(const char *query, const char *text) {
    /* subsequence match, case-insensitive (fuzzyMatch's acceptance) */
    const char *t = text;
    for (const char *q = query; *q; q++) {
        if (*q == ' ') continue;
        char lc = (char)tolower((unsigned char)*q);
        while (*t && tolower((unsigned char)*t) != lc) t++;
        if (!*t) return false;
        t++;
    }
    return true;
}

static const json_t *sel_ref(const hub_t *h) { return json_at(h->rows, (size_t)h->sel); }

static void project_tree(hub_t *h, json_t *rows) {
    /* parent-before-child projection (projectAgentTree without spawn-while-open reordering) */
    json_free(h->depth), json_free(h->parent), json_free(h->last_sib);
    h->depth = json_obj(), h->parent = json_obj(), h->last_sib = json_obj();
    h->max_depth = 0;
    json_t *ids = json_obj(), *r;
    json_arr_foreach(rows, i, r) json_set_bool(ids, json_get_str(r, "id"), true);
    json_t *children = json_obj();
    json_arr_foreach(rows, i, r) {
        const char *p = json_get_str(r, "parentId");
        const char *par = p && !str_eq(p, "Main") && json_get(ids, p) ? p : "Main";
        json_set_str(h->parent, json_get_str(r, "id"), par);
        json_t *sib = json_get(children, par);
        if (!sib) {
            json_set(children, par, json_arr());
            sib = json_get(children, par);
        }
        json_push(sib, json_clone(r));
    }
    const char *k;
    json_t *sibs;
    json_obj_foreach(children, oi, k, sibs) {
        UNUSED(k);
        size_t n = json_len(sibs);
        for (size_t i = 0; i < n; i++) json_set_bool(h->last_sib, json_get_str(json_at(sibs, i), "id"), i + 1 == n);
    }
    json_t *out = json_arr();
    json_t *visited = json_obj();
    /* iterative DFS from Main's children */
    VEC(const json_t *) stack = {0};
    VEC(int) depths = {0};
    json_t *roots = json_get(children, "Main");
    for (size_t pass = 0; pass < 2; pass++) {
        json_t *src = pass == 0 ? roots : rows;
        for (size_t ri = 0; ri < json_len(src); ri++) {
            vec_push(&stack, json_at(src, ri));
            vec_push(&depths, 0);
            while (stack.n) {
                const json_t *cur = stack.v[--stack.n];
                int d = depths.v[--depths.n];
                const char *id = json_get_str(cur, "id");
                if (json_get(visited, id)) continue;
                json_set_bool(visited, id, true);
                json_set_int(h->depth, id, d);
                if (d > h->max_depth) h->max_depth = d;
                json_push(out, json_clone(cur));
                json_t *desc = json_get(children, id);
                for (size_t c = json_len(desc); c > 0; c--) {
                    vec_push(&stack, json_at(desc, c - 1));
                    vec_push(&depths, d + 1);
                }
            }
        }
    }
    vec_free(&stack);
    vec_free(&depths);
    json_free(visited);
    json_free(children);
    json_free(ids);
    /* swap contents */
    while (json_len(rows)) json_free(json_arr_remove(rows, 0));
    json_arr_foreach(out, i, r) json_push(rows, json_clone(r));
    json_free(out);
}

static void refresh_activity_view(hub_t *h);

static void refresh_rows(hub_t *h) {
    const json_t *cur = sel_ref(h);
    char *selected = cur ? xstrdup(json_get_str(cur, "id")) : NULL;
    json_t *refs = agents_list(), *r;
    int batch = 0;
    bool batch_set = false;
    json_arr_foreach(refs, i, r) {
        const char *id = json_get_str(r, "id");
        if (json_get(h->ranks, id)) continue;
        if (h->opened && !batch_set) batch = --h->spawn_batches, batch_set = true;
        json_t *rk = json_obj();
        json_set_int(rk, "b", h->opened ? batch : 0);
        json_set_int(rk, "s", status_order(json_get_str(r, "status")));
        json_set_int(rk, "l", json_get_int(r, "lastActivity", 0));
        json_set(h->ranks, id, rk);
    }
    h->opened = true;
    size_t n = json_len(refs);
    json_t **arr = xcalloc(n + 1, sizeof *arr);
    for (size_t i = 0; i < n; i++) arr[i] = json_at(refs, i);
    g_sort_hub = h;
    qsort(arr, n, sizeof *arr, rank_cmp);
    json_t *ordered = json_arr();
    const char *q = ui_input_value(&h->agent_filter);
    char *qt = str_trim(q ? q : "");
    for (size_t i = 0; i < n; i++) {
        char *hay = xasprintf("%s %s", json_get_str(arr[i], "id"), json_get_str(arr[i], "displayName") ? json_get_str(arr[i], "displayName") : "");
        if (!*qt || fuzzy_has(qt, hay)) json_push(ordered, json_clone(arr[i]));
        free(hay);
    }
    free(qt);
    free(arr);
    json_free(refs);
    if (h->tree) project_tree(h, ordered);
    json_free(h->rows);
    h->rows = ordered;
    int keep = -1;
    json_arr_foreach(h->rows, i, r) if (selected && str_eq(json_get_str(r, "id"), selected)) keep = (int)i;
    int nrows = (int)json_len(h->rows);
    h->sel = keep >= 0 ? keep : MIN(h->sel, MAX(0, nrows - 1));
    free(selected);
    const char *did = sel_ref(h) ? json_get_str(sel_ref(h), "id") : NULL;
    if (!str_eq(did, h->detail_id)) {
        free(h->detail_id);
        h->detail_id = did ? xstrdup(did) : NULL;
        h->detail_scroll = 0;
    }
    /* activity: every listed agent's rows */
    arows_clear(&h->acts);
    json_arr_foreach(h->rows, i, r) agent_rows(r, &h->acts);
    qsort(h->acts.v, h->acts.n, sizeof(arow_t), arow_cmp);
    if (h->section == 1) refresh_activity_view(h);
}

static int count_children(const hub_t *h, const char *id) {
    int n = 0;
    const json_t *r;
    json_arr_foreach(h->rows, i, r) if (str_eq(json_get_str(r, "parentId") ? json_get_str(r, "parentId") : "Main", id)) n++;
    return n;
}

static bool in_scope(const hub_t *h, const char *agent) {
    if (h->scope == 0) return true;
    const json_t *s = sel_ref(h);
    if (!s) return false;
    const char *sid = json_get_str(s, "id");
    if (str_eq(agent, sid)) return true;
    if (h->scope == 1) return false;
    /* subtree: walk the agent's ancestry */
    char *cur = xstrdup(agent);
    for (int guard = 0; guard < 64; guard++) {
        const char *p = NULL;
        const json_t *r;
        json_arr_foreach(h->rows, i, r) if (str_eq(json_get_str(r, "id"), cur)) p = json_get_str(r, "parentId");
        free(cur);
        if (!p) return false;
        if (str_eq(p, sid)) return true;
        cur = xstrdup(p);
    }
    free(cur);
    return false;
}

static void refresh_activity_view(hub_t *h) {
    h->act_view.n = 0;
    const char *q = ui_input_value(&h->act_search);
    char *search = q && *q ? str_lower(q) : NULL;
    char *st = search ? str_trim(search) : NULL;
    for (size_t i = 0; i < h->acts.n; i++) {
        const arow_t *r = &h->acts.v[i];
        if (!in_scope(h, r->agent)) continue;
        if (h->filter == 2 && r->kind != AK_RESPONSE) continue;
        if (h->filter == 3 && r->kind != AK_TOOL) continue;
        if (h->filter == 1 && r->status != AS_ERROR) continue;
        if (st && *st) {
            char *hay = xasprintf("%s %s %s", r->agent, r->title, r->summary), *lh = str_lower(hay);
            bool ok = strstr(lh, st) != NULL;
            free(hay), free(lh);
            if (!ok) continue;
        }
        vec_push(&h->act_view, i);
    }
    free(search), free(st);
    if (h->act_view.n > 2000) {
        size_t drop = h->act_view.n - 2000;
        memmove(h->act_view.v, h->act_view.v + drop, 2000 * sizeof(size_t));
        h->act_view.n = 2000;
    }
    if (!h->act_view.n) h->act_sel = 0;
    else if (h->follow) h->act_sel = (int)h->act_view.n - 1;
    else h->act_sel = MIN(h->act_sel, (int)h->act_view.n - 1);
}

/* the box rows */
static char *hub_row(const char *content, int width) { return ui_box_row(content, width); }

static char *top_split(int width, const char *title, int left_w) {
    const char *H = sym("boxRound.horizontal");
    int dcol = left_w + 3, left_len = MAX(0, dcol - 1), right_len = MAX(0, width - 2 - dcol);
    sb_t b = SB_INIT, r = SB_INIT;
    sb_appendf(&r, "%s%s", sym("boxRound.topLeft"), H);
    putc_s(&b, TUI_TC_BORDER, r.s);
    sb_clear(&r);
    char *t = xasprintf(" %s ", title), *shown = tui_truncate_to_width(t, MAX(0, left_len - 1), NULL);
    free(t);
    int fill = MAX(0, left_len - 1 - tui_visible_width(shown));
    put(&b, bold_fg(TUI_TC_ACCENT, shown));
    free(shown);
    for (int i = 0; i < fill; i++) sb_append(&r, H);
    sb_append(&r, sym("boxSharp.teeDown"));
    for (int i = 0; i < right_len; i++) sb_append(&r, H);
    sb_append(&r, sym("boxRound.topRight"));
    putc_s(&b, TUI_TC_BORDER, r.s);
    sb_free(&r);
    return sb_detach(&b);
}

static char *divider_split(int width, int left_w) {
    const char *H = sym("boxRound.horizontal");
    int dcol = left_w + 3, left_len = MAX(0, dcol - 1), right_len = MAX(0, width - 2 - dcol);
    sb_t r = SB_INIT;
    sb_append(&r, sym("boxSharp.teeRight"));
    for (int i = 0; i < left_len; i++) sb_append(&r, H);
    sb_append(&r, sym("boxSharp.teeUp"));
    for (int i = 0; i < right_len; i++) sb_append(&r, H);
    sb_append(&r, sym("boxSharp.teeLeft"));
    char *res = S(TUI_TC_BORDER, r.s);
    sb_free(&r);
    return res;
}

static char *tab_chip(bool active, const char *label) {
    char *t = xasprintf(" %s ", label);
    char *r;
    if (active) {
        char *b = bold_fg(TUI_TC_ACCENT, t);
        r = tui_bgs(NULL, TUI_TC_SELECTED_BG, b);
        free(b);
    } else r = S(TUI_TC_MUTED, t);
    free(t);
    return r;
}

static char *section_tabs(const hub_t *h) {
    sb_t b = SB_INIT;
    put(&b, tab_chip(h->section == 0, "1 Agents"));
    putc_s(&b, TUI_TC_DIM, sym("sep.dot"));
    put(&b, tab_chip(h->section == 1, "2 Activity"));
    return sb_detach(&b);
}

static char *status_text(const char *st, const char *text) { return S(status_tone(st), text); }

static void summary_lines(hub_t *h, int width, strvec_t *out) {
    const char *dot = sym("sep.dot");
    sb_t hd = SB_INIT;
    put(&hd, tui_bold("Roster"));
    putc_s(&hd, TUI_TC_DIM, dot);
    put(&hd, tab_chip(!h->tree, "Flat"));
    putc_s(&hd, TUI_TC_DIM, "/");
    put(&hd, tab_chip(h->tree, "By parent"));
    static const char *const ST[] = {"running", "idle", "parked", "aborted"};
    int counts[4] = {0};
    const json_t *r;
    json_arr_foreach(h->rows, i, r) counts[status_order(json_get_str(r, "status"))]++;
    bool first = true;
    for (int s = 0; s < 4; s++) {
        if (!counts[s]) continue;
        if (first) putc_s(&hd, TUI_TC_DIM, dot); /* the counts themselves join with a plain separator */
        else sb_append(&hd, dot);
        first = false;
        put(&hd, status_glyph(ST[s]));
        sb_putc(&hd, ' ');
        char *t = xasprintf("%d %s", counts[s], ST[s]);
        put(&hd, status_text(ST[s], t));
        free(t);
    }
    tui_wrap_text_with_ansi(hd.s, MAX(1, width), out);
    sb_free(&hd);
    /* aggregate */
    metrics_t tot = {0};
    int reported = 0, timed = 0;
    json_arr_foreach(h->rows, i, r) {
        metrics_t m;
        if (!row_metrics(r, &m)) continue;
        reported++;
        tot.tokens += m.tokens, tot.requests += m.requests, tot.tools += m.tools, tot.cost += m.cost;
        if (m.active) tot.duration += m.duration, timed++;
    }
    size_t nrows = json_len(h->rows);
    if (!reported) {
        char *t = xasprintf("Usage —%s0/%zu measured", dot, nrows), *d = S(TUI_TC_DIM, t);
        tui_wrap_text_with_ansi(d, MAX(1, width), out);
        free(t), free(d);
        return;
    }
    tot.active = true;
    char *dur = metric_duration(&tot);
    char *c = fmt_cost(tot.cost), *rq = fmt_number(tot.requests), *tl = fmt_number(tot.tools), *tk = fmt_number(tot.tokens);
    sb_t u = SB_INIT;
    put(&u, S(TUI_TC_STATUS_LINE_COST, c));
    char *parts[6];
    parts[0] = dur ? xasprintf("%s agent time", dur) : xstrdup("agent time —");
    parts[1] = xasprintf("%s req", rq);
    parts[2] = xasprintf("%s tools", tl);
    parts[3] = xasprintf("%s tok", tk);
    parts[4] = xasprintf("%d/%d timed", timed, reported);
    parts[5] = xasprintf("%d/%zu measured", reported, nrows);
    for (int i = 0; i < 6; i++) {
        putc_s(&u, TUI_TC_DIM, dot);
        putc_s(&u, TUI_TC_DIM, parts[i]);
        free(parts[i]);
    }
    tui_wrap_text_with_ansi(u.s, MAX(1, width), out);
    sb_free(&u);
    free(dur), free(c), free(rq), free(tl), free(tk);
}

static char *tree_prefix(hub_t *h, const json_t *ref, int max_w, bool cont) {
    const char *id = json_get_str(ref, "id");
    if (json_get_int(h->depth, id, 0) == 0) return xstrdup("");
    bool last = json_get_bool(h->last_sib, id, false);
    strvec_t segs = {0};
    strvec_push(&segs, cont ? (last ? "    " : "│   ") : last ? "└── " : "├── ");
    const char *p = json_get_str(h->parent, id);
    for (int guard = 0; p && !str_eq(p, "Main") && guard < 64; guard++) {
        const char *gp = json_get_str(h->parent, p);
        if (!gp || str_eq(gp, "Main")) break;
        strvec_push(&segs, json_get_bool(h->last_sib, p, false) ? "    " : "│   ");
        p = gp;
    }
    int max_segs = MAX(1, MAX(4, max_w - 2) / 4);
    size_t shown = MIN(segs.n, (size_t)max_segs);
    sb_t b = SB_INIT;
    if (segs.n > shown) sb_append(&b, cont ? "  " : "… ");
    for (size_t i = shown; i > 0; i--) sb_append(&b, segs.v[i - 1]);
    strvec_free(&segs);
    char *r = S(TUI_TC_DIM, b.s ? b.s : "");
    sb_free(&b);
    return r;
}

/* #renderEntry: identity row, task row, metrics row */
static void render_entry(hub_t *h, const json_t *ref, bool selected, int width, strvec_t *out) {
    int max = MAX(1, width);
    char *cursor = selected ? S(TUI_TC_ACCENT, sym("nav.cursor")) : xstrdup(" ");
    char *branch = h->tree ? tree_prefix(h, ref, max, false) : xstrdup("");
    char *id = one_line(json_get_str(ref, "id"));
    char *sid = selected ? bold_fg(TUI_TC_ACCENT, id) : tui_bold(id);
    const char *st = json_get_str(ref, "status");
    char *gl = status_glyph(st);
    sb_t left = SB_INIT;
    sb_appendf(&left, "%s %s%s %s", cursor, branch, gl, sid);
    free(gl), free(sid), free(id), free(cursor);
    const char *pid = json_get_str(ref, "parentId");
    if (!h->tree && pid && !str_eq(pid, "Main")) {
        char *t = xasprintf("↳ %s", pid);
        sb_append(&left, "  ");
        putc_s(&left, TUI_TC_DIM, t);
        free(t);
    }
    char *right = model_badge(ref);
    int lw = tui_visible_width(left.s), rw = right ? tui_visible_width(right) : 0;
    if (rw > 0 && lw + 2 + rw <= max) {
        sb_t l = SB_INIT;
        sb_append(&l, left.s);
        tui_sb_spaces(&l, max - lw - rw);
        sb_append(&l, right);
        strvec_push_own(out, sb_detach(&l));
    } else {
        strvec_push_own(out, tui_truncate_to_width(left.s, max, NULL));
    }
    sb_free(&left);
    free(right);
    int bw = tui_visible_width(branch);
    int detail_indent = MIN(max - 1, 4 + bw);
    char *indent, *meta_prefix;
    if (h->tree) {
        char *contp = tree_prefix(h, ref, max, true);
        char *own = count_children(h, json_get_str(ref, "id")) ? S(TUI_TC_DIM, "│ ") : xstrdup("  ");
        char *cont = xasprintf("  %s%s", contp, own);
        free(contp), free(own);
        int meta_indent = MIN(MAX(0, max - 1), 4 + MAX(0, h->max_depth) * 4);
        if (tui_visible_width(cont) == detail_indent) {
            indent = xstrdup(cont);
            sb_t m = SB_INIT;
            sb_append(&m, cont);
            tui_sb_spaces(&m, meta_indent - detail_indent);
            meta_prefix = sb_detach(&m);
        } else {
            sb_t a = SB_INIT, m = SB_INIT;
            tui_sb_spaces(&a, detail_indent);
            tui_sb_spaces(&m, meta_indent);
            indent = sb_detach(&a);
            meta_prefix = sb_detach(&m);
        }
        free(cont);
    } else {
        sb_t a = SB_INIT;
        tui_sb_spaces(&a, detail_indent);
        indent = sb_detach(&a);
        meta_prefix = xstrdup(indent);
    }
    free(branch);
    int dw = MAX(1, max - detail_indent);
    const char *task = ref_task(ref);
    if (task) {
        char *one = one_line(task), *t = trunc_w(one, dw), *m = S(TUI_TC_MUTED, t);
        strvec_push_own(out, xasprintf("%s%s", indent, m));
        free(one), free(t), free(m);
    }
    char *age = age_of(json_get_int(ref, "lastActivity", now_ms()));
    metrics_t mt;
    char *meta = row_metrics(ref, &mt) ? metric_columns(&mt, age) : xasprintf("usage %s %s", sym("sep.dot"), age);
    char *dm = S(TUI_TC_DIM, meta);
    strvec_push_own(out, xasprintf("%s%s", meta_prefix, dm));
    free(age), free(meta), free(dm), free(indent), free(meta_prefix);
}

static void roster_panel(hub_t *h, int width, int rows, strvec_t *out) {
    strvec_t lines = {0};
    summary_lines(h, width, &lines);
    if (rows >= 8) strvec_push(&lines, "");
    int notice_n = h->notice ? 1 : 0;
    int budget = MAX(0, rows - (int)lines.n - notice_n);
    int n = (int)json_len(h->rows);
    if (!n) {
        const char *empty[3];
        char *a = S(TUI_TC_MUTED, sym("status.shadowed")), *b = tui_bold("No agents in this session");
        char *l0 = xasprintf("%s %s", a, b);
        char *l1 = S(TUI_TC_DIM, "Finished, parked, and killed subagents remain with the session that created them.");
        char *l2 = S(TUI_TC_DIM, "Resume that session with omc --continue, or spawn a task here.");
        empty[0] = l0, empty[1] = l1, empty[2] = l2;
        for (int i = 0; i < 3 && i < budget; i++) strvec_push(&lines, empty[i]);
        free(a), free(b), free(l0), free(l1), free(l2);
    } else if (budget > 0) {
        /* grow a window around the selection */
        strvec_t *ent = xcalloc((size_t)n, sizeof *ent);
        bool *done = xcalloc((size_t)n, sizeof *done);
#define ENTRY(i) (done[i] ? &ent[i] : (render_entry(h, json_at(h->rows, (size_t)(i)), (i) == h->sel, width, &ent[i]), done[i] = true, &ent[i]))
        int start = h->sel, end = h->sel + 1;
        int used = (int)ENTRY(h->sel)->n;
        if (used > budget) {
            for (int k = 0; k < budget; k++) strvec_push(&lines, ENTRY(h->sel)->v[k]);
        } else {
            for (bool grew = true; grew;) {
                grew = false;
                if (end < n && used + (int)ENTRY(end)->n <= budget) used += (int)ENTRY(end)->n, end++, grew = true;
                if (start > 0 && used + (int)ENTRY(start - 1)->n <= budget) start--, used += (int)ENTRY(start)->n, grew = true;
            }
            for (int markers = (start > 0) + (end < n); used + markers > budget && start < end; markers = (start > 0) + (end < n)) {
                if (end - 1 > h->sel) end--, used -= (int)ENTRY(end)->n;
                else if (start < h->sel) used -= (int)ENTRY(start)->n, start++;
                else break;
            }
            bool top = start > 0 && used < budget;
            bool bottom = end < n && used + (top ? 1 : 0) < budget;
            if (top) {
                char *t = xasprintf("… %d more", start);
                strvec_push_own(&lines, S(TUI_TC_DIM, t));
                free(t);
            }
            for (int i = start; i < end; i++)
                for (size_t k = 0; k < ENTRY(i)->n; k++) strvec_push(&lines, ENTRY(i)->v[k]);
            if (bottom) {
                char *t = xasprintf("… %d more", n - end);
                strvec_push_own(&lines, S(TUI_TC_DIM, t));
                free(t);
            }
        }
#undef ENTRY
        for (int i = 0; i < n; i++) strvec_free(&ent[i]);
        free(ent);
        free(done);
    }
    if (h->notice) {
        char *one = one_line(h->notice), *t = trunc_w(one, MAX(10, width));
        strvec_push_own(&lines, S(TUI_TC_ERROR, t));
        free(one), free(t);
    }
    for (size_t i = 0; i < lines.n && (int)i < rows; i++) strvec_push(out, lines.v[i]);
    for (int i = (int)lines.n; i < rows; i++) strvec_push(out, "");
    strvec_free(&lines);
}

static void detail_panel(hub_t *h, int width, int rows, strvec_t *out) {
    const json_t *ref = sel_ref(h);
    if (!ref) {
        strvec_push_own(out, S(TUI_TC_DIM, "Select an agent to inspect"));
        for (int i = 1; i < rows; i++) strvec_push(out, "");
        return;
    }
    const char *dot = sym("sep.dot");
    strvec_t L = {0};
#define ADD(s) strvec_push_own(&L, trunc_w((s), width))
#define SECTION(label, content_rows)                                                         \
    do {                                                                                     \
        if (L.n > 0 && (int)L.n + 1 + (content_rows) < rows) strvec_push(&L, "");            \
        char *sec_ = bold_fg(TUI_TC_ACCENT, label);                                          \
        ADD(sec_);                                                                           \
        free(sec_);                                                                          \
    } while (0)
    const char *st = json_get_str(ref, "status");
    const char *id = json_get_str(ref, "id");
    const char *dn = json_get_str(ref, "displayName");
    {
        char *g = status_glyph(st), *nm = one_line(dn && *dn ? dn : id), *b = tui_bold(nm), *l = xasprintf("%s %s", g, b);
        ADD(l);
        free(g), free(nm), free(b), free(l);
    }
    if (dn && *dn && !str_eq(dn, id)) {
        char *t = S(TUI_TC_DIM, id);
        ADD(t);
        free(t);
    }
    metrics_t m;
    bool has_m = row_metrics(ref, &m);
    {
        char *d = has_m ? metric_duration(&m) : NULL;
        char *age = age_of(json_get_int(ref, "lastActivity", now_ms()));
        char *details = d ? xasprintf("%s%sactive %s", d, dot, age) : xasprintf("active %s", age);
        char *tail = xasprintf("%s%s", dot, details), *dt = S(TUI_TC_DIM, tail), *sw = status_text(st, st);
        char *l = xasprintf("%s%s", sw, dt);
        ADD(l);
        free(d), free(age), free(details), free(tail), free(dt), free(sw), free(l);
    }
    char *badge = model_badge(ref);
    if (badge) {
        ADD(badge);
        free(badge);
    }
    const json_t *p = json_get_obj(ref, "progress");
    const char *task = ref_task(ref);
    if (task) {
        SECTION("Task", 0);
        char *one = one_line(task);
        strvec_t w = {0};
        tui_wrap_text_with_ansi(one, MAX(1, width), &w);
        for (size_t i = 0; i < w.n && i < 2; i++) ADD(w.v[i]);
        strvec_free(&w);
        free(one);
    }
    const char *cur_tool = json_get_str(p, "currentTool");
    char *current = cur_tool ? (json_get_str(p, "currentToolArgs") ? xasprintf("%s%s%s", cur_tool, dot, json_get_str(p, "currentToolArgs"))
                                                                    : xstrdup(cur_tool))
                  : json_get_str(p, "lastIntent") ? xstrdup(json_get_str(p, "lastIntent"))
                  : json_get_str(ref, "activity") ? xstrdup(json_get_str(ref, "activity"))
                                                  : NULL;
    if (current) {
        SECTION("Current", 0);
        char *one = one_line(current);
        strvec_t w = {0};
        tui_wrap_text_with_ansi(one, MAX(1, width), &w);
        for (size_t i = 0; i < w.n && i < 2; i++) ADD(w.v[i]);
        strvec_free(&w);
        free(one);
        free(current);
    }
    SECTION("Usage", 1);
    if (has_m) {
        char *mt = metrics_text(&m);
        strvec_t w = {0};
        tui_wrap_text_with_ansi(mt, MAX(1, width), &w);
        for (size_t i = 0; i < w.n && i < 3; i++) ADD(w.v[i]);
        strvec_free(&w);
        free(mt);
        if (m.ctx_tokens && m.ctx_window) {
            char *g = context_gauge(m.ctx_tokens, m.ctx_window);
            ADD(g);
            free(g);
        }
    } else {
        char *t = S(TUI_TC_DIM, "usage —");
        ADD(t);
        free(t);
    }
    SECTION("Lineage", 0);
    int nchild = count_children(h, id);
    {
        const char *pid = json_get_str(ref, "parentId");
        char *l = nchild ? xasprintf("Spawned by %s%s%d children", pid ? pid : "Main", dot, nchild) : xasprintf("Spawned by %s", pid ? pid : "Main");
        ADD(l);
        free(l);
    }
    if (nchild) {
        sb_t ids = SB_INIT;
        int shown = 0;
        const json_t *r;
        json_arr_foreach(h->rows, i, r) {
            const char *pp = json_get_str(r, "parentId");
            if (!str_eq(pp ? pp : "Main", id)) continue;
            char *cand = ids.len ? xasprintf("%s, %s", ids.s, json_get_str(r, "id")) : xstrdup(json_get_str(r, "id"));
            int rem = nchild - shown - 1;
            char *sfx = rem > 0 ? xasprintf(", … +%d", rem) : xstrdup("");
            char *full = xasprintf("%s%s", cand, sfx);
            bool over = tui_visible_width(full) > width;
            free(full), free(sfx);
            if (over) {
                int omitted = nchild - shown - (ids.len ? 0 : 1);
                char *t = xasprintf("%s%s", ids.len ? ids.s : json_get_str(r, "id"), omitted > 0 ? "" : "");
                char *t2 = omitted > 0 ? xasprintf("%s, … +%d", t, omitted) : xstrdup(t);
                sb_clear(&ids);
                put(&ids, trunc_w(t2, width));
                free(t), free(t2), free(cand);
                shown = nchild;
                break;
            }
            sb_clear(&ids);
            sb_append(&ids, cand);
            free(cand);
            shown++;
        }
        char *d = S(TUI_TC_DIM, ids.s ? ids.s : "");
        ADD(d);
        free(d);
        sb_free(&ids);
    }
    {
        char *dt = local_datetime(json_get_int(ref, "createdAt", now_ms())), *t = xasprintf("Registered %s", dt), *d = S(TUI_TC_DIM, t);
        ADD(d);
        free(dt), free(t), free(d);
    }
    SECTION("Changes", 0);
    {
        char *d = S(TUI_TC_DIM, "Shared workspace · per-agent LoC not attributable");
        ADD(d);
        free(d);
    }
    if ((int)L.n < rows) strvec_push(&L, "");
    if ((int)L.n < rows) {
        char *t = bold_fg(TUI_TC_ACCENT, "Recent activity");
        ADD(t);
        free(t);
    }
    int budget = MAX(0, rows - (int)L.n);
    size_t total = 0;
    for (size_t i = 0; i < h->acts.n; i++)
        if (str_eq(h->acts.v[i].agent, id)) total++;
    if (!total && budget > 0) {
        char *t = S(TUI_TC_MUTED, "No response or tool activity yet");
        ADD(t);
        free(t);
    } else if (budget > 0) {
        size_t skip = total > (size_t)budget ? total - (size_t)budget : 0, seen = 0;
        for (size_t i = 0; i < h->acts.n; i++) {
            const arow_t *a = &h->acts.v[i];
            if (!str_eq(a->agent, id)) continue;
            if (seen++ < skip) continue;
            char *clk = clock_hms(a->ts), *dc = S(TUI_TC_DIM, clk), *gl = activity_glyph(a);
            char *tt = one_line(arow_title(a)), *ttt = trunc_w(tt, width), *mt = S(TUI_TC_MUTED, ttt);
            char *prefix = xasprintf("%s %s %s ", dc, gl, mt);
            char *sm = one_line(a->summary), *smt = trunc_w(sm, MAX(1, width - tui_visible_width(prefix)));
            char *l = xasprintf("%s%s", prefix, smt);
            ADD(l);
            free(clk), free(dc), free(gl), free(tt), free(ttt), free(mt), free(prefix), free(sm), free(smt), free(l);
        }
    }
#undef ADD
#undef SECTION
    int max_scroll = MAX(0, (int)L.n - rows);
    h->detail_scroll = MIN(h->detail_scroll, max_scroll);
    for (int i = 0; i < rows; i++) {
        int k = h->detail_scroll + i;
        strvec_push(out, k < (int)L.n ? L.v[k] : "");
    }
    strvec_free(&L);
}

static char *footer_agents(hub_t *h, bool narrow_details, int avail) {
    const char *next = h->tree ? "flat" : "by parent";
    char *filter;
    if (h->agent_filter_editing) {
        char *in = ui_input_render(&h->agent_filter, "", tui_visible_width(ui_input_value(&h->agent_filter)) + 1, true);
        filter = xasprintf("/%s  ·  ", in);
        free(in);
    } else if (*ui_input_value(&h->agent_filter)) filter = xasprintf("/%s  ·  ", ui_input_value(&h->agent_filter));
    else filter = xstrdup("");
    char *jk = key_hints2("j", "k"), *pg = key_hints2("pageUp", "pageDown"), *en = ui_key_hint("enter"), *t = ui_key_hint("t"),
         *esc = ui_key_hint("escape"), *tab = ui_key_hint("tab"), *rx = key_hints2("r", "x"), *r = ui_key_hint("r"), *x = ui_key_hint("x");
    char *s;
    if (narrow_details)
        s = xasprintf("%s1:agents  2:activity  %s:roster  %s:scroll  %s:open  %s:%s  %s:roster", filter, tab, pg, en, t, next, esc);
    else if (avail < 96)
        s = xasprintf("%s%s:select  %s:open  %s:%s  %s:details  %s:manage  %s:close", filter, jk, en, t, next, tab, rx, esc);
    else
        s = xasprintf("%s1:agents  2:activity  %s/wheel:select  %s:details  %s/click:open  %s:%s  %s:revive  %s:kill  %s:close", filter,
                      jk, pg, en, t, next, r, x, esc);
    char *d = S(TUI_TC_DIM, s);
    free(s), free(filter), free(jk), free(pg), free(en), free(t), free(esc), free(tab), free(rx), free(r), free(x);
    return d;
}

static int term_rows(const ui_t *u) { return tui_term(u->tui)->rows > 0 ? tui_term(u->tui)->rows : 40; }

static void render_agents(hub_t *h, int width, strvec_t *out) {
    int content = MAX(1, term_rows(h->u) - 4);
    /* SplitPane: prefix "│ ", divider " │ ", suffix " │"; left ratio 0.58 min 48, right min 34, split at 96 */
    int avail = MAX(0, width - 7);
    bool split = width >= 96 && avail >= 48 + 34;
    h->split = split;
    int L = 0, R = 0;
    if (split) {
        int desired = (int)floor(width * 0.58);
        L = MAX(0, MIN(avail - 34, MAX(48, desired)));
        R = MAX(0, avail - L);
    }
    const json_t *sel = sel_ref(h);
    if (split) strvec_push_own(out, top_split(width, "Agent Hub", L));
    else if (h->narrow_details && sel) {
        char *t = xasprintf("Agent Hub · %s", json_get_str(sel, "id"));
        strvec_push_own(out, ui_box_top(width, t));
        free(t);
    } else strvec_push_own(out, ui_box_top(width, "Agent Hub"));
    char *bar = S(TUI_TC_BORDER, sym("boxRound.vertical"));
    if (split) {
        strvec_t left = {0}, right = {0};
        roster_panel(h, L, content, &left);
        detail_panel(h, R, content, &right);
        for (int i = 0; i < content; i++) {
            char *l = pad_to(i < (int)left.n ? left.v[i] : "", L), *r = pad_to(i < (int)right.n ? right.v[i] : "", R);
            strvec_push_own(out, xasprintf("%s %s %s %s %s", bar, l, bar, r, bar));
            free(l), free(r);
        }
        strvec_free(&left), strvec_free(&right);
        strvec_push_own(out, divider_split(width, L));
    } else {
        int w = MAX(0, width - 4);
        strvec_t pane = {0};
        if (h->narrow_details) detail_panel(h, w, content, &pane);
        else roster_panel(h, w, content, &pane);
        for (int i = 0; i < content; i++) strvec_push_own(out, hub_row(i < (int)pane.n ? pane.v[i] : "", width));
        strvec_free(&pane);
        strvec_push_own(out, ui_box_divider(width));
    }
    free(bar);
    char *f = footer_agents(h, split ? false : h->narrow_details, MAX(1, width - 4));
    strvec_push_own(out, hub_row(f, width));
    free(f);
    strvec_push_own(out, ui_box_bottom(width));
}

static void render_activity(hub_t *h, int width, strvec_t *out) {
    int inner = MAX(1, width - 4);
    int content = MAX(1, term_rows(h->u) - 4);
    strvec_t body = {0};
    strvec_push_own(&body, section_tabs(h));
    const json_t *s = sel_ref(h);
    const char *sid = s ? json_get_str(s, "id") : NULL;
    char *scope = h->scope == 0 ? xstrdup("all agents") : h->scope == 1 ? xstrdup(sid ? sid : "selected agent")
                                                                           : xasprintf("%s subtree", sid ? sid : "selected");
    char *search;
    if (h->act_search_editing) {
        char *in = ui_input_render(&h->act_search, "", tui_visible_width(ui_input_value(&h->act_search)) + 1, true);
        char *t = xasprintf("search: %s", in);
        search = S(TUI_TC_ACCENT, t);
        free(in), free(t);
    } else if (*ui_input_value(&h->act_search)) search = xasprintf("search: %s", ui_input_value(&h->act_search));
    else search = xstrdup("search: —");
    const char *dot = sym("sep.dot");
    char *st = xasprintf("%s%s%s%s%s%s%s", scope, dot, FILTERS[h->filter], dot, h->follow ? "following" : "paused", dot, search);
    strvec_push_own(&body, S(TUI_TC_DIM, st));
    free(st), free(scope), free(search);
    if (content >= 8) strvec_push(&body, "");
    int budget = MAX(0, content - (int)body.n);
    int n = (int)h->act_view.n;
    if (!n && budget > 0) {
        strvec_push_own(&body, S(TUI_TC_MUTED, *ui_input_value(&h->act_search) ? "No matching activity" : "No agent activity recorded yet"));
    } else if (budget > 0) {
        int sel = MIN(h->act_sel, n - 1);
        int start = h->follow ? MAX(0, n - budget) : MAX(0, MIN(sel - budget / 2, n - budget));
        int end = MIN(n, start + budget);
        if (start > 0) {
            char *t = xasprintf("… %d earlier", start);
            strvec_push_own(&body, S(TUI_TC_DIM, t));
            free(t);
        }
        for (int i = start + (start > 0); i < end; i++) {
            const arow_t *a = &h->acts.v[h->act_view.v[i]];
            char *cursor = i == sel ? S(TUI_TC_ACCENT, sym("nav.cursor")) : xstrdup(" ");
            char *clk = clock_hms(a->ts), *dc = S(TUI_TC_DIM, clk), *gl = activity_glyph(a);
            char *ag1 = one_line(a->agent), *ag = trunc_w(ag1, MAX(8, MIN(18, (int)floor(inner * 0.18)))), *agb = tui_bold(ag);
            char *tt1 = one_line(arow_title(a)), *tt = trunc_w(tt1, inner), *ttc = S(a->kind == AK_RESPONSE ? TUI_TC_SUCCESS : TUI_TC_MUTED, tt);
            char *prefix = xasprintf("%s %s %s %s %s", cursor, dc, gl, agb, ttc);
            int availw = MAX(1, inner - tui_visible_width(prefix) - tui_visible_width(dot));
            char *sm1 = one_line(a->summary), *sm = trunc_w(sm1, availw), *dd = S(TUI_TC_DIM, dot);
            strvec_push_own(&body, xasprintf("%s%s%s", prefix, dd, sm));
            free(cursor), free(clk), free(dc), free(gl), free(ag1), free(ag), free(agb), free(tt1), free(tt), free(ttc), free(prefix),
                free(sm1), free(sm), free(dd);
        }
    }
    strvec_push_own(out, ui_box_top(width, "Agent Hub"));
    for (int i = 0; i < content; i++) strvec_push_own(out, hub_row(i < (int)body.n ? body.v[i] : "", width));
    strvec_free(&body);
    strvec_push_own(out, ui_box_divider(width));
    char *jk = key_hints2("j", "k"), *en = ui_key_hint("enter"), *sp = ui_key_hint("space"), *f = ui_key_hint("f"), *s2 = ui_key_hint("s"),
         *esc = ui_key_hint("escape");
    char *ft = xasprintf("1:agents  %s:select  %s:transcript  %s:follow  %s:filter  %s:scope  /:search  %s:close", jk, en, sp, f, s2, esc);
    char *fd = S(TUI_TC_DIM, ft);
    strvec_push_own(out, hub_row(fd, width));
    free(jk), free(en), free(sp), free(f), free(s2), free(esc), free(ft), free(fd);
    strvec_push_own(out, ui_box_bottom(width));
}

static void hub_render(tui_component_t *c, int width, strvec_t *out) {
    hub_t *h = (hub_t *)c;
    strvec_t frame = {0};
    if (h->section == 1) render_activity(h, width, &frame);
    else render_agents(h, width, &frame);
    int th = term_rows(h->u);
    if ((int)frame.n <= th) {
        for (size_t i = 0; i < frame.n; i++) strvec_push_own(out, tui_truncate_to_width(frame.v[i], MAX(1, width), ""));
    } else {
        int foot = MIN(3, (int)frame.n), body_end = MAX(0, th - foot);
        for (int i = 0; i < body_end; i++) strvec_push(out, frame.v[i]);
        for (int i = (int)frame.n - foot; i < (int)frame.n && (int)out->n < th; i++) strvec_push(out, frame.v[i]);
    }
    strvec_free(&frame);
}

static void hub_age_tick(void *ud) {
    hub_t *h = ud;
    tui_request_render(h->u->tui);
}

static void hub_close(hub_t *h);

static void open_chat(hub_t *h, const char *id) {
    json_t *ref = agents_get(id);
    if (!ref) return;
    json_free(ref);
    free(h->notice);
    h->notice = NULL;
    viewer_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &VIEWER_VT);
    v->u = h->u;
    v->hub = h;
    v->id = xstrdup(id);
    v->follow = true;
    v->body = ui_transcript_new(true);
    v->ed = tui_editor_new();
    tui_editor_set_composer(v->ed, "box");
    v->ed->max_height = 4;
    if (h->u->editor) snprintf(v->ed->text_fg, sizeof v->ed->text_fg, "%s", h->u->editor->text_fg);
    v->ed->base.tui = h->u->tui;
    v->ed->base.focused = true; /* the viewer routes keys to it: draw its caret */
    tui_editor_set_on_submit(v->ed, viewer_submit, v);
    viewer_reload(v);
    v->timer = tui_add_timer(h->u->tui, 250, true, viewer_tick, v);
    h->viewer = v;
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(h->u->tui, &v->base, &o);
    tui_set_focus(h->u->tui, &v->base);
    tui_request_render(h->u->tui);
}

static void viewer_close(viewer_t *v, bool hub_too) {
    hub_t *h = v->hub;
    tui_overlay_hide(v->u->tui, &v->base);
    h->viewer = NULL;
    tui_component_destroy(&v->base);
    if (hub_too) hub_close(h);
    else {
        tui_set_focus(h->u->tui, &h->base);
        tui_request_render(h->u->tui);
    }
}

static void set_notice(hub_t *h, char *owned) {
    free(h->notice);
    h->notice = owned;
}

static void activate(hub_t *h, const json_t *ref) {
    set_notice(h, NULL);
    char *id = xstrdup(json_get_str(ref, "id")), *err = NULL;
    if (ui_focus_agent(h->u, id, &err)) hub_close(h);
    else set_notice(h, err);
    free(id);
    tui_request_render(h->u->tui);
}

static void revive_selected(hub_t *h) {
    const json_t *ref = sel_ref(h);
    if (!ref) return;
    const char *st = json_get_str(ref, "status");
    if (!str_eq(st, "parked") && !str_eq(st, "aborted")) {
        set_notice(h, xasprintf("Agent \"%s\" is %s — only parked or killed agents can be revived.", json_get_str(ref, "id"), st));
        tui_request_render(h->u->tui);
        return;
    }
    set_notice(h, NULL);
    char *err = NULL;
    if (!agents_revive(json_get_str(ref, "id"), &err)) set_notice(h, err);
    refresh_rows(h);
    tui_request_render(h->u->tui);
}

static void kill_selected(hub_t *h) {
    const json_t *ref = sel_ref(h);
    if (!ref) return;
    set_notice(h, NULL);
    char *err = NULL;
    char *id = xstrdup(json_get_str(ref, "id"));
    if (h->u->focus_id && str_eq(h->u->focus_id, id)) ui_unfocus(h->u, NULL);
    if (!agents_kill(id, &err)) set_notice(h, err);
    free(id);
    refresh_rows(h);
    tui_request_render(h->u->tui);
}

static void switch_section(hub_t *h, int s) {
    if (h->section == s) return;
    h->section = s;
    h->narrow_details = false;
    if (s == 1) refresh_activity_view(h);
    tui_request_render(h->u->tui);
}

static void select_row(hub_t *h, int i) {
    if (i != h->sel) h->detail_scroll = 0;
    h->sel = i;
    const json_t *r = sel_ref(h);
    free(h->detail_id);
    h->detail_id = r ? xstrdup(json_get_str(r, "id")) : NULL;
}

static bool is_enter(const tui_key_t *k) { return k->kind == TUI_KEY_ENTER && !k->mods; }
static bool is_char(const tui_key_t *k, char c) {
    return k->kind == TUI_KEY_CHAR && k->codepoint == (uint32_t)c && !(k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT | TUI_MOD_SUPER));
}

static bool activity_key(hub_t *h, const tui_key_t *k) {
    if (h->act_search_editing) {
        if (k->kind == TUI_KEY_ESCAPE || is_enter(k)) h->act_search_editing = false;
        else if (!ui_input_key(&h->act_search, k)) return true;
        refresh_activity_view(h);
        tui_request_render(h->u->tui);
        return true;
    }
    if (k->kind == TUI_KEY_ESCAPE) {
        if (*ui_input_value(&h->act_search)) {
            ui_input_set(&h->act_search, "");
            refresh_activity_view(h);
            tui_request_render(h->u->tui);
        } else hub_close(h);
        return true;
    }
    if (k->kind == TUI_KEY_LEFT && !k->mods) {
        switch_section(h, 0);
        return true;
    }
    if (is_char(k, '/')) h->act_search_editing = true;
    else if (is_char(k, ' ')) {
        h->follow = !h->follow;
        if (h->follow && h->act_view.n) h->act_sel = (int)h->act_view.n - 1;
    } else if (is_char(k, 'f')) {
        h->filter = (h->filter + 1) % 4;
        refresh_activity_view(h);
    } else if (is_char(k, 's')) {
        h->scope = (h->scope + 1) % 3;
        refresh_activity_view(h);
    } else if (is_char(k, 'j') || tui_kb(k, "tui.select.down")) {
        if (h->act_view.n) h->follow = false, h->act_sel = MIN(h->act_sel + 1, (int)h->act_view.n - 1);
    } else if (is_char(k, 'k') || tui_kb(k, "tui.select.up")) {
        if (h->act_view.n) h->follow = false, h->act_sel = MAX(h->act_sel - 1, 0);
    } else if (is_enter(k)) {
        if (h->act_sel < (int)h->act_view.n) {
            char *id = xstrdup(h->acts.v[h->act_view.v[h->act_sel]].agent);
            open_chat(h, id);
            free(id);
        }
        return true;
    }
    tui_request_render(h->u->tui);
    return true;
}

static bool table_key(hub_t *h, const tui_key_t *k) {
    int n = (int)json_len(h->rows);
    if (h->agent_filter_editing) {
        if (k->kind == TUI_KEY_ESCAPE || is_enter(k)) {
            h->agent_filter_editing = false;
            if (k->kind == TUI_KEY_ESCAPE) ui_input_set(&h->agent_filter, "");
        } else if (!ui_input_key(&h->agent_filter, k)) return true;
        refresh_rows(h);
        tui_request_render(h->u->tui);
        return true;
    }
    if (k->kind == TUI_KEY_ESCAPE) {
        if (*ui_input_value(&h->agent_filter)) {
            ui_input_set(&h->agent_filter, "");
            refresh_rows(h);
        } else if (h->narrow_details && !h->split) h->narrow_details = false;
        else {
            hub_close(h);
            return true;
        }
        tui_request_render(h->u->tui);
        return true;
    }
    if (is_char(k, '/')) {
        h->agent_filter_editing = true;
    } else if (k->kind == TUI_KEY_TAB && !h->split) {
        if (n) h->narrow_details = !h->narrow_details;
    } else if ((h->split || h->narrow_details) && k->kind == TUI_KEY_PAGEUP) {
        h->detail_scroll = MAX(0, h->detail_scroll - 5);
    } else if ((h->split || h->narrow_details) && k->kind == TUI_KEY_PAGEDOWN) {
        h->detail_scroll += 5;
    } else if (is_char(k, 't')) {
        h->tree = !h->tree;
        refresh_rows(h);
    } else if (k->kind == TUI_KEY_LEFT && !k->mods) {
        if (h->narrow_details && !h->split) h->narrow_details = false;
        else {
            int64_t now = now_ms();
            if (now - h->last_left_tap < 500) {
                h->last_left_tap = 0;
                hub_close(h);
                return true;
            }
            h->last_left_tap = now;
        }
    } else if (is_char(k, 'j') || tui_kb(k, "tui.select.down")) {
        if (n) select_row(h, MIN(h->sel + 1, n - 1));
    } else if (is_char(k, 'k') || tui_kb(k, "tui.select.up")) {
        if (n) select_row(h, MAX(h->sel - 1, 0));
    } else if (is_enter(k)) {
        const json_t *r = sel_ref(h);
        if (r) {
            json_t *ref = json_clone(r);
            activate(h, ref);
            json_free(ref);
        }
        return true;
    } else if (is_char(k, 'r')) {
        revive_selected(h);
        return true;
    } else if (is_char(k, 'x')) {
        kill_selected(h);
        return true;
    }
    tui_request_render(h->u->tui);
    return true;
}

static bool hub_key(tui_component_t *c, const tui_key_t *k) {
    hub_t *h = (hub_t *)c;
    if (tui_kb(k, "app.agents.hub") || tui_kb(k, "app.session.observe")) {
        hub_close(h);
        return true;
    }
    if (h->section == 1 && h->act_search_editing) return activity_key(h, k);
    if (!h->agent_filter_editing && is_char(k, '1')) {
        switch_section(h, 0);
        return true;
    }
    if (!h->agent_filter_editing && is_char(k, '2')) {
        switch_section(h, 1);
        return true;
    }
    return h->section == 1 ? activity_key(h, k) : table_key(h, k);
}

static void hub_destroy(tui_component_t *c) {
    hub_t *h = (hub_t *)c;
    json_free(h->rows), json_free(h->ranks), json_free(h->depth), json_free(h->parent), json_free(h->last_sib);
    ui_input_free(&h->agent_filter);
    ui_input_free(&h->act_search);
    free(h->notice), free(h->detail_id);
    arows_clear(&h->acts);
    vec_free(&h->acts);
    vec_free(&h->act_view);
    free(h);
}

static const tui_component_vt HUB_VT = {"agent-hub", hub_render, hub_key, NULL, hub_destroy};

static void hub_close(hub_t *h) {
    ui_t *u = h->u;
    if (h->viewer) {
        viewer_t *v = h->viewer;
        tui_overlay_hide(u->tui, &v->base);
        h->viewer = NULL;
        tui_component_destroy(&v->base);
    }
    if (h->age_timer) tui_remove_timer(u->tui, h->age_timer);
    tui_overlay_hide(u->tui, &h->base);
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    u->hub = NULL;
    tui_component_destroy(&h->base);
    tui_request_render(u->tui);
}

void ui_agent_hub_open(ui_t *u, bool activity, bool require_content, bool arm_close) {
    if (u->hub) { /* a repeat request reuses the open hub, still honoring its deep link */
        if (activity) switch_section(u->hub, 1);
        return;
    }
    if (require_content) {
        size_t all = 0;
        agents_counts(NULL, &all);
        if (!all) return;
    }
    hub_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &HUB_VT);
    h->u = u;
    h->section = activity ? 1 : 0;
    h->follow = true;
    h->ranks = json_obj();
    h->depth = json_obj(), h->parent = json_obj(), h->last_sib = json_obj();
    refresh_rows(h);
    if (h->section == 1) refresh_activity_view(h);
    if (arm_close) h->last_left_tap = now_ms();
    u->hub = h;
    h->age_timer = tui_add_timer(u->tui, 5000, true, hub_age_tick, h);
    tui_alt_screen(u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &h->base, &o);
    tui_set_focus(u->tui, &h->base);
    tui_request_render(u->tui);
}

/* ======================================================================================================== */
/* ---------- session focus (session-focus-controller.ts) ---------- */

static void set_working(ui_t *u, app_t *a) {
    if (agent_is_running(a->agent)) {
        ui_chrome_turn(u, true);
        ui_working_start(u);
    } else {
        ui_working_stop(u);
        ui_chrome_turn(u, false);
    }
}

bool ui_focus_agent(ui_t *u, const char *id, char **err) {
    *err = NULL;
    if (!id || str_eq(id, "Main")) {
        ui_unfocus(u, "Returned to main session");
        return true;
    }
    if (u->focus_id && str_eq(u->focus_id, id)) return true;
    if (!agents_revive(id, err)) return false;
    app_t *child = agents_pin(id);
    if (!child) {
        *err = xasprintf("Subagent not running: %s", id);
        return false;
    }
    ix_view_subscribe(u, ui_view_app(u), false);
    if (u->view) agents_unpin(u->view);
    u->view = child;
    free(u->focus_id);
    u->focus_id = xstrdup(id);
    ix_view_subscribe(u, child, true);
    set_working(u, child);
    ui_chat_rebuild(u);
    /* omp's "Viewing agent <id> — ⎋ returns to main, ←← hops to parent" status never reaches the rebuilt
     * transcript (showStatus lands before the replay settles), so nothing is added here */
    tui_request_render(u->tui);
    return true;
}

void ui_unfocus(ui_t *u, const char *status) {
    if (!u->view) return;
    ix_view_subscribe(u, u->view, false);
    agents_unpin(u->view);
    u->view = NULL;
    free(u->focus_id);
    u->focus_id = NULL;
    ix_view_subscribe(u, u->app, true);
    set_working(u, u->app);
    ui_chat_rebuild(u);
    UNUSED(status); /* omp's "Returned to main session" status is not visible after the rebuild either */
    tui_request_render(u->tui);
}

/* ←← on an empty composer: from main open the hub (when there is something to show); focused: back to main */
void ui_left_tap(ui_t *u) {
    int64_t now = now_ms(), since = now - u->last_left_tap;
    u->last_left_tap = now;
    if (since >= 500) {
        u->left_taps = 1;
        return;
    }
    u->left_taps++;
    if (u->left_taps == 2 && since >= 40) {
        u->left_taps = 0;
        u->last_left_tap = 0;
        if (u->focus_id) ui_unfocus(u, "Returned to main session");
        else {
            ui_hint_record("agents");
            ui_agent_hub_open(u, false, true, true);
        }
    }
}

/* ======================================================================================================== */
/* ---------- registry changes -> badge, HUD, hub, focus ---------- */

static void agents_sync(void *ud) {
    ui_t *u = ud;
    pthread_mutex_lock(&u->mu);
    u->agents_dirty = false;
    pthread_mutex_unlock(&u->mu);
    if (u->focus_id) {
        json_t *ref = agents_get(u->focus_id);
        const char *st = ref ? json_get_str(ref, "status") : NULL;
        if (!ref || str_eq(st, "parked") || str_eq(st, "aborted")) {
            char *id = xstrdup(u->focus_id);
            char *m = xasprintf("Agent %s is %s; returned to main session", id, ref ? st : "gone");
            ui_unfocus(u, m);
            free(m), free(id);
        }
        json_free(ref);
    }
    if (u->hub) refresh_rows(u->hub);
    if (u->subagent_hud) tui_component_invalidate(u->subagent_hud);
    tui_request_render(u->tui);
}

static void agents_changed(void *ud) {
    ui_t *u = ud;
    pthread_mutex_lock(&u->mu);
    bool post = !u->agents_dirty;
    u->agents_dirty = true;
    pthread_mutex_unlock(&u->mu);
    if (post) tui_post(u->tui, agents_sync, u);
}

void ui_agents_init(ui_t *u) { agents_listen(agents_changed, u); }

void ui_agents_shutdown(ui_t *u) {
    agents_unlisten(agents_changed, u);
    if (u->view) {
        ix_view_subscribe(u, u->view, false);
        agents_unpin(u->view);
        u->view = NULL;
    }
    free(u->focus_id);
    u->focus_id = NULL;
}

/* ---------- unit-test hooks (tests/test_agenthub.c) ---------- */

char *ui_hub_format(const char *what, int64_t v) {
    if (str_eq(what, "duration")) return fmt_duration(v);
    if (str_eq(what, "number")) return fmt_number(v);
    if (str_eq(what, "age")) return fmt_age(v);
    if (str_eq(what, "cost")) return fmt_cost((double)v / 1e6);
    return NULL;
}

/* formatMetricColumns of a roster ref's progress (NULL without one); owned */
char *ui_hub_metric_columns(const json_t *ref, const char *age) {
    metrics_t m;
    return row_metrics(ref, &m) ? metric_columns(&m, age) : NULL;
}

/* one agent's activity rows as [{source, kind, title, summary, status}]: transcript rows, then live rows */
json_t *ui_hub_activity_rows(const json_t *ref, const json_t *messages) {
    arows_t live = {0}, tr = {0};
    live_rows(ref, 1000, &live);
    transcript_rows(json_get_str(ref, "id"), messages, &tr);
    json_t *out = json_arr();
    static const char *const K[] = {"response", "tool", "irc", "lifecycle"}, *const ST[] = {"", "pending", "success", "error", "aborted"};
    arows_t *src[2] = {&tr, &live};
    for (int s = 0; s < 2; s++)
        for (size_t i = 0; i < src[s]->n; i++) {
            const arow_t *a = &src[s]->v[i];
            json_t *o = json_obj();
            json_set_str(o, "source", s ? "live" : "transcript");
            json_set_str(o, "kind", K[a->kind]);
            json_set_str(o, "title", arow_title(a));
            json_set_str(o, "summary", a->summary);
            json_set_str(o, "status", ST[a->status]);
            json_push(out, o);
        }
    arows_clear(&live), arows_clear(&tr);
    vec_free(&live);
    vec_free(&tr);
    return out;
}
