/* loop.c - `/loop` mode model (modes/loop-limit.ts, modes/loop-condition.ts, status-line/loop.ts). See loop.h. */
#include "coding/loop.h"

#include "tui/ansi.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>

#define LOOP_USAGE                                                                                                     \
    "Usage: /loop [count|duration] [--while|--until '<command>'] [prompt]. Examples: /loop 10, /loop 10m, /loop 20 "   \
    "--until 'bun test' fix the failing tests."
#define TITLE_WIDTH 60 /* TRUNCATE_LENGTHS.TITLE */
#define SHORT_WIDTH 40 /* TRUNCATE_LENGTHS.SHORT */
#define MAX_SAFE_INTEGER 9007199254740991LL

static const struct {
    const char *name;
    int64_t ms;
} UNITS[] = {
    {"s", 1000},        {"sec", 1000},       {"secs", 1000},      {"second", 1000},     {"seconds", 1000},
    {"m", 60000},       {"min", 60000},      {"mins", 60000},     {"minute", 60000},    {"minutes", 60000},
    {"h", 3600000},     {"hr", 3600000},     {"hrs", 3600000},    {"hour", 3600000},    {"hours", 3600000},
};

static int64_t unit_ms(const char *name, size_t n) {
    for (size_t i = 0; i < ARRAY_LEN(UNITS); i++)
        if (strlen(UNITS[i].name) == n && !strncasecmp(UNITS[i].name, name, n)) return UNITS[i].ms;
    return -1;
}

static bool ws(char c) { return isspace((unsigned char)c); }

static char *trim_dup(const char *s, size_t n) {
    while (n && ws(*s)) s++, n--;
    while (n && ws(s[n - 1])) n--;
    return xstrndup(s, n);
}

/* Number(digits) as a safe integer; -1 when not one */
static int64_t safe_int(const char *s, size_t n) {
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (v > MAX_SAFE_INTEGER / 10) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v > MAX_SAFE_INTEGER ? -1 : v;
}

void loop_args_free(loop_args_t *a) {
    free(a->cond.command);
    free(a->prompt);
    memset(a, 0, sizeof *a);
}

/* readShellWord: one shell word (quotes and backslashes), the rest trimmed; 0 ok, 1 none, 2 unterminated */
static int read_shell_word(const char *text, char **value, const char **rest) {
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p) return 1;
    sb_t v = SB_INIT;
    bool sq = false, dq = false;
    for (; *p; p++) {
        char ch = *p;
        if (sq) {
            if (ch == '\'') sq = false;
            else sb_putc(&v, ch);
            continue;
        }
        if (dq) {
            if (ch == '\\' && p[1] && strchr("\"\\$`", p[1])) {
                sb_putc(&v, *++p);
                continue;
            }
            if (ch == '"') dq = false;
            else sb_putc(&v, ch);
            continue;
        }
        if (ch == '\'') sq = true;
        else if (ch == '"') dq = true;
        else if (ch == '\\' && p[1]) sb_putc(&v, *++p);
        else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
        else sb_putc(&v, ch);
    }
    if (sq || dq) {
        sb_free(&v);
        return 2;
    }
    *value = sb_detach(&v);
    while (ws(*p)) p++;
    *rest = p;
    return 0;
}

static char *take_condition(const char *input, loop_args_t *out) {
    char *buf = trim_dup(input, strlen(input));
    const char *rest = buf;
    char *err = NULL;
    while (!strncmp(rest, "--", 2)) {
        /* /^(--[a-z][a-z-]*)(?=[\s=]|$)/ */
        size_t n = 2;
        if (rest[n] >= 'a' && rest[n] <= 'z') {
            n++;
            while ((rest[n] >= 'a' && rest[n] <= 'z') || rest[n] == '-') n++;
        } else n = 0;
        if (n && rest[n] && !ws(rest[n]) && rest[n] != '=') n = 0;
        char *name = n ? xstrndup(rest, n) : NULL;
        int until = !name ? -1 : str_eq(name, "--while") ? 0 : str_eq(name, "--until") ? 1 : -1;
        if (until < 0) {
            char *shown = name;
            if (!shown) {
                size_t k = 0;
                while (rest[k] && !ws(rest[k])) k++;
                shown = xstrndup(rest, k);
            }
            err = xasprintf("Unknown /loop flag %s. %s", shown, LOOP_USAGE);
            free(shown);
            break;
        }
        if (out->has_cond) {
            free(name);
            err = xstrdup("Use only one of --while or --until.");
            break;
        }
        const char *after = rest + n;
        const char *value_text = *after == '=' ? after + 1 : after;
        char *value = NULL;
        const char *vrest = NULL;
        int rc = read_shell_word(value_text, &value, &vrest);
        if (rc == 2) {
            err = xasprintf("%s has an unterminated quote.", name);
            free(name);
            break;
        }
        char *vt = trim_dup(value_text, strlen(value_text));
        char *trimmed = value ? trim_dup(value, strlen(value)) : NULL;
        bool bad = rc == 1 || !*trimmed || vt[0] == '-';
        free(vt);
        free(value);
        if (bad) {
            free(trimmed);
            err = xasprintf("%s needs a shell command. Quote it when it contains spaces: /loop %s 'bun test'.", name, name);
            free(name);
            break;
        }
        out->has_cond = true;
        out->cond.command = trimmed;
        out->cond.until = until == 1;
        free(name);
        rest = vrest;
    }
    if (!err && *rest) out->prompt = xstrdup(rest);
    free(buf);
    return err;
}

char *loop_parse_args(const char *args, loop_args_t *out) {
    memset(out, 0, sizeof *out);
    char *input = trim_dup(args ? args : "", strlen(args ? args : ""));
    if (!*input) {
        free(input);
        return NULL;
    }
    char *err = NULL;
    const char *rest = input;
    size_t tl = 0;
    while (input[tl] && !ws(input[tl])) tl++;
    const char *tok = input;
    const char *after = input[tl] ? input + tl + 1 : input + tl;
    while (ws(*after)) after++;
    bool sign = tok[0] == '+' || tok[0] == '-';
    if (isdigit((unsigned char)tok[sign ? 1 : 0]) && tl > (size_t)(sign ? 1 : 0)) {
        bool digits = true;
        for (size_t i = 0; i < tl; i++) digits &= isdigit((unsigned char)tok[i]) != 0;
        if (digits) {
            size_t ul = 0;
            while (after[ul] && !ws(after[ul])) ul++;
            int64_t ms = *after ? unit_ms(after, ul) : -1;
            int64_t amount = safe_int(tok, tl);
            if (ms > 0) {
                if (amount <= 0) err = xstrdup("Loop duration must be positive.");
                else {
                    out->limit_kind = LOOP_LIMIT_DURATION;
                    out->amount = amount * ms;
                    rest = after + ul;
                }
            } else if (amount <= 0) err = xstrdup("Loop count must be a positive integer.");
            else {
                out->limit_kind = LOOP_LIMIT_ITERATIONS;
                out->amount = amount;
                rest = after;
            }
        } else {
            /* compact / compound duration: 10m, 90s, 1h30m (the token is matched lowercased) */
            bool shaped = !sign && tl > 0;
            size_t i = 0;
            while (shaped && i < tl) {
                size_t d = i;
                while (i < tl && isdigit((unsigned char)tok[i])) i++;
                size_t l = i;
                while (i < tl && isalpha((unsigned char)tok[i])) i++;
                if (d == l || l == i || (i < tl && !isdigit((unsigned char)tok[i]))) shaped = false;
            }
            if (!shaped) err = xstrdup(LOOP_USAGE);
            else {
                int64_t total = 0;
                for (i = 0; i < tl && !err;) {
                    size_t d = i;
                    while (isdigit((unsigned char)tok[i])) i++;
                    size_t l = i;
                    while (i < tl && isalpha((unsigned char)tok[i])) i++;
                    int64_t ms = unit_ms(tok + l, i - l), amount = safe_int(tok + d, l - d);
                    if (ms < 0) err = xstrdup("Loop duration unit must be seconds, minutes, or hours.");
                    else if (amount <= 0) err = xstrdup("Loop duration must be positive.");
                    else total += amount * ms;
                }
                if (!err && total <= 0) err = xstrdup("Loop duration must be positive.");
                if (!err) {
                    out->limit_kind = LOOP_LIMIT_DURATION;
                    out->amount = total;
                    rest = after;
                }
            }
        }
    }
    if (!err) err = take_condition(rest, out);
    if (err) loop_args_free(out);
    free(input);
    return err;
}

/* ---------- runtime budget ---------- */

void loop_limit_init(loop_limit_t *l, loop_limit_kind_t kind, int64_t amount, int64_t now) {
    memset(l, 0, sizeof *l);
    l->kind = kind;
    if (kind == LOOP_LIMIT_ITERATIONS) l->initial = l->remaining = amount;
    else if (kind == LOOP_LIMIT_DURATION) l->duration_ms = amount, l->deadline_ms = now + amount;
}

bool loop_limit_consume(loop_limit_t *l, int64_t now) {
    if (l->kind == LOOP_LIMIT_NONE) return true;
    if (l->kind == LOOP_LIMIT_DURATION) return now < l->deadline_ms;
    if (l->remaining <= 0) return false;
    l->remaining--;
    return true;
}

bool loop_limit_duration_expired(const loop_limit_t *l, int64_t now) {
    return l->kind == LOOP_LIMIT_DURATION && now >= l->deadline_ms;
}

bool loop_limit_exhausted(const loop_limit_t *l, int64_t now) {
    if (l->kind == LOOP_LIMIT_NONE) return false;
    if (l->kind == LOOP_LIMIT_DURATION) return now >= l->deadline_ms;
    return l->remaining <= 0;
}

/* ---------- texts ---------- */

/* formatLoopBudget: one spelled-out unit at full precision */
static char *budget(int64_t ms) {
    if (ms % 3600000 == 0) return xasprintf("%lld %s", (long long)(ms / 3600000), ms == 3600000 ? "hour" : "hours");
    if (ms % 60000 == 0) return xasprintf("%lld %s", (long long)(ms / 60000), ms == 60000 ? "minute" : "minutes");
    if (ms % 1000 == 0) return xasprintf("%lld %s", (long long)(ms / 1000), ms == 1000 ? "second" : "seconds");
    return xasprintf("%g seconds", (double)ms / 1000.0);
}

char *loop_describe_limit(loop_limit_kind_t kind, int64_t amount) {
    if (kind == LOOP_LIMIT_ITERATIONS) return xasprintf("%lld %s", (long long)amount, amount == 1 ? "iteration" : "iterations");
    return budget(amount);
}

char *loop_describe_limit_runtime(const loop_limit_t *l) {
    if (l->kind == LOOP_LIMIT_ITERATIONS)
        return xasprintf("%lld of %lld %s remaining", (long long)l->remaining, (long long)l->initial,
                         l->initial == 1 ? "iteration" : "iterations");
    char *b = budget(l->duration_ms), *r = xasprintf("%s limit", b);
    free(b);
    return r;
}

/* sanitizeStatusText: escapes stripped, controls/newlines/tabs -> space, space runs collapsed, trimmed */
static char *sanitize(const char *s) {
    char *plain = tui_strip_ansi(s ? s : "");
    sb_t b = SB_INIT;
    bool sp = false;
    for (const unsigned char *c = (const unsigned char *)plain; *c; c++) {
        if (*c == ' ' || *c < 0x20 || *c == 0x7f) {
            sp = b.len > 0;
            continue;
        }
        if (sp) sb_putc(&b, ' ');
        sp = false;
        sb_putc(&b, (char)*c);
    }
    free(plain);
    return sb_detach(&b);
}

static char *truncate_plain(const char *s, int width) {
    char *t = tui_truncate_to_width(s, MAX(1, width), NULL), *p = tui_strip_ansi(t);
    free(t);
    return p;
}

static char *quote_command(const char *command) {
    char *s = sanitize(command), *t = truncate_plain(s, TITLE_WIDTH), *r = xasprintf("`%s`", t);
    free(s), free(t);
    return r;
}

char *loop_describe_condition(const loop_cond_t *c) {
    char *q = quote_command(c->command), *r = xasprintf("%s %s succeeds", c->until ? "until" : "while", q);
    free(q);
    return r;
}

char *loop_summarize_condition(const loop_cond_t *c, int max_width) {
    const char *label = c->until ? "until" : "while";
    char *s = sanitize(c->command), *t = truncate_plain(s, MAX(1, max_width - (int)strlen(label) - 2));
    char *r = xasprintf("%s: %s", label, t);
    free(s), free(t);
    return r;
}

char *loop_format_limit(const loop_limit_t *l, int64_t now) {
    if (l->kind == LOOP_LIMIT_NONE) return NULL;
    if (l->kind == LOOP_LIMIT_ITERATIONS) return xasprintf("%lld/%lld", (long long)l->remaining, (long long)l->initial);
    int64_t left = l->deadline_ms - now;
    int64_t total = left <= 0 ? 0 : (left + 999) / 1000;
    int64_t h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0) return m > 0 ? xasprintf("%lldh%lldm left", (long long)h, (long long)m) : xasprintf("%lldh left", (long long)h);
    if (m > 0) return s > 0 ? xasprintf("%lldm%llds left", (long long)m, (long long)s) : xasprintf("%lldm left", (long long)m);
    return xasprintf("%llds left", (long long)s);
}

char *loop_status_label(const loop_state_t *st) {
    const char *state = st->paused ? "paused" : st->prompt ? "running" : "waiting";
    bool waiting = str_eq(state, "waiting");
    sb_t b = SB_INIT;
    sb_appendf(&b, "Loop%s %s", waiting ? ":" : "", waiting ? "next prompt repeats" : state);
    return sb_detach(&b);
}

char *loop_autocomplete_description(const loop_state_t *s) {
    if (!s || !s->enabled) return xstrdup("Loop: off");
    if (s->paused) return xstrdup("Loop: paused");
    sb_t b = SB_INIT;
    if (s->limit.kind != LOOP_LIMIT_NONE) {
        char *d = loop_describe_limit_runtime(&s->limit);
        sb_append(&b, d);
        free(d);
    }
    if (s->has_cond) {
        char *d = loop_describe_condition(&s->cond);
        if (b.len) sb_append(&b, ", ");
        sb_append(&b, d);
        free(d);
    }
    if (b.len) {
        char *r = xasprintf("Loop: on (%s)", sb_cstr(&b));
        sb_free(&b);
        return r;
    }
    sb_free(&b);
    return xstrdup(s->prompt ? "Loop: on (repeating prompt)" : "Loop: on (waiting for next prompt)");
}

char *loop_enabled_message(const loop_args_t *a, const loop_limit_t *rt, const char *interrupt_key) {
    sb_t b = SB_INIT;
    sb_append(&b, "Loop mode enabled.");
    if (a->limit_kind != LOOP_LIMIT_NONE) {
        char *d = loop_describe_limit(a->limit_kind, a->amount), *r = loop_describe_limit_runtime(rt);
        sb_appendf(&b, " Limited to %s. %s.", d, r);
        free(d), free(r);
    }
    if (a->has_cond) {
        char *d = loop_describe_condition(&a->cond);
        sb_appendf(&b, " Continuing %s.", d);
        free(d);
    }
    sb_appendf(&b, " %s %s suspends the ongoing loop; /loop again to disable.",
               a->prompt ? "Repeating it after each turn." : "Your next prompt will repeat after each turn.", interrupt_key);
    return sb_detach(&b);
}

loop_verdict_t loop_condition_verdict(const loop_cond_t *c, int exit_code, bool timed_out, bool cancelled,
                                      const char *spawn_error, const char *output, int64_t timeout_ms, char **message) {
    *message = NULL;
    char *q = quote_command(c->command);
    loop_verdict_t v;
    if (spawn_error) {
        char *s = sanitize(spawn_error), *t = truncate_plain(s, TITLE_WIDTH);
        *message = xasprintf("Loop condition %s could not run: %s. Loop mode disabled.", q, t);
        free(s), free(t);
        v = LOOP_VERDICT_ERROR;
    } else if (timed_out) {
        char *to = timeout_ms % 1000 == 0 ? xasprintf("%llds", (long long)(timeout_ms / 1000)) : xasprintf("%lldms", (long long)timeout_ms);
        *message = xasprintf("Loop condition %s timed out after %s. Loop mode disabled.", q, to);
        free(to);
        v = LOOP_VERDICT_ERROR;
    } else if (cancelled) {
        v = LOOP_VERDICT_ABORTED;
    } else if (exit_code == 0) {
        v = c->until ? LOOP_VERDICT_HALT : LOOP_VERDICT_CONTINUE;
        if (c->until) *message = xasprintf("Loop condition %s is now satisfied. Loop mode disabled.", q);
    } else if (exit_code == 1) {
        v = c->until ? LOOP_VERDICT_CONTINUE : LOOP_VERDICT_HALT;
        if (!c->until) *message = xasprintf("Loop condition %s no longer holds. Loop mode disabled.", q);
    } else {
        /* exit > 1 (or none): the condition is broken, not answering */
        char *detail = xstrdup("");
        const char *p = output ? output : "";
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t n = nl ? (size_t)(nl - p) : strlen(p);
            char *line = trim_dup(p, n);
            if (*line) {
                char *s = sanitize(line), *t = truncate_plain(s, TITLE_WIDTH);
                free(detail);
                detail = *t ? xasprintf(": %s", t) : xstrdup("");
                free(s), free(t), free(line);
                break;
            }
            free(line);
            if (!nl) break;
            p = nl + 1;
        }
        char *status = exit_code < 0 ? xstrdup("no exit status") : xasprintf("exit %d", exit_code);
        *message = xasprintf("Loop condition %s failed (%s)%s. Loop mode disabled.", q, status, detail);
        free(status), free(detail);
        v = LOOP_VERDICT_ERROR;
    }
    free(q);
    return v;
}
