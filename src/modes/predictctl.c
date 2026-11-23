/* predictctl.c - composer predictions (port of omp modes/controllers/composer-prediction-controller.ts, v19.1.0).
 *
 * Once a turn completes, an ephemeral side turn on the session's model and context (the same prompt prefix, so it
 * reads the prompt cache) asks for the user's likely next message (system/composer-prediction-user.md), or with
 * `composer.predictionAlternatives` up to four of them; the composer offers them as ghost text while it is empty
 * (tui_editor_set_predictions: Tab or Right inserts one, Tab again lists the alternatives). Nothing is sent. A new
 * message, a turn start, a compaction or another session retires the prediction. The request is booked as
 * `composer-prediction` usage. Not ported: the OpenTelemetry prediction metrics (no observable behaviour), and
 * `minimizeEffort` lowers nothing because omc has no mid-conversation effort controls. */
#include "modes/interactive.h"

#include "config/settings.h"
#include "prompt/template.h"

#include <ctype.h>

#define SKIP_REPLY "NO_PREDICTION"
#define MAX_PREDICTION_LENGTH 500 /* UTF-16 code units, as JS `length` */
#define MAX_PREDICTION_CHOICES 4
#define PREDICTION_MAX_TOKENS 1024
#define PREDICTION_DEADLINE_MS 15000
#define USAGE_PURPOSE "composer-prediction"

typedef struct job {
    ui_t *u;
    pthread_t th;
    volatile sig_atomic_t cancel;
    bool alternatives;
    bool deadline;      /* the 15 s deadline aborted it */
    int timer;          /* deadline timer id */
    char *reply, *err;
    json_t *assistant;
    /* the conversation point it was made for */
    const json_t *anchor;
    size_t count;
    app_t *view;
    bool joined;        /* pred_shutdown waited for the thread */
} job_t;

static struct {
    VEC(job_t *) running; /* every thread not yet joined, cancelled ones too */
    job_t *job;         /* in flight (owned until its done callback runs) */
    strvec_t choices;   /* the offered prediction, most likely first */
    const json_t *anchor;
    size_t count;
    app_t *view;
    bool live;          /* choices are offered */
} P;

/* ---------- reply parsing (parseComposerPrediction / parseComposerPredictions) ---------- */

static size_t utf16_len(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;
        n += *p >= 0xF0 ? 2 : 1;
    }
    return n;
}

/* sanitizeText (ANSI and control characters other than \t \n dropped), then \s+ -> " " and trimmed */
static char *normalize(const char *reply) {
    char *plain = tui_strip_ansi(reply ? reply : "");
    sb_t b = SB_INIT;
    bool space = false;
    for (const unsigned char *p = (const unsigned char *)plain; *p; p++) {
        unsigned char c = *p;
        if (c == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) { /* C1 controls */
            p++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n') {
            space = true;
            continue;
        }
        if (c < 0x20 || c == 0x7F) continue;
        if (space && b.len) sb_putc(&b, ' ');
        space = false;
        sb_putc(&b, (char)c);
    }
    free(plain);
    return sb_detach(&b);
}

/* one pair of "…", “…” or `…` around the whole text */
static char *unquote(char *text) {
    size_t n = strlen(text);
    const char *open = NULL, *close = NULL;
    if (n >= 2 && text[0] == '"' && text[n - 1] == '"') open = close = "\"";
    else if (n >= 2 && text[0] == '`' && text[n - 1] == '`') open = close = "`";
    else if (n >= 6 && !strncmp(text, "\xe2\x80\x9c", 3) && !strcmp(text + n - 3, "\xe2\x80\x9d")) {
        open = "\xe2\x80\x9c";
        close = "\xe2\x80\x9d";
    }
    if (!open) return text;
    size_t ol = strlen(open), cl = strlen(close);
    char *inner = xstrndup(text + ol, n - ol - cl);
    /* the inner text holds no quote of its kind (“ and ” both for the curly pair) */
    bool ok = !strstr(inner, open) && !strstr(inner, close);
    if (ok && ol == 3 && strstr(inner, "\xe2\x80\x9c")) ok = false;
    if (!ok) {
        free(inner);
        return text;
    }
    free(text);
    char *t = str_trim(inner);
    free(inner);
    return t;
}

char *pred_parse_one(const char *reply) {
    char *text = unquote(normalize(reply));
    if (!*text || strstr(text, SKIP_REPLY) || utf16_len(text) > MAX_PREDICTION_LENGTH) {
        free(text);
        return NULL;
    }
    return text;
}

/* LIST_MARKER: ^\s*(?:[-*•]|\d{1,2}[.)])\s+ */
static const char *strip_marker(const char *line) {
    const char *p = line;
    while (isspace((unsigned char)*p)) p++;
    const char *q = NULL;
    if (*p == '-' || *p == '*') q = p + 1;
    else if (!strncmp(p, "\xe2\x80\xa2", 3)) q = p + 3;
    else if (isdigit((unsigned char)*p)) {
        const char *d = p + 1;
        if (isdigit((unsigned char)*d)) d++;
        if (*d == '.' || *d == ')') q = d + 1;
    }
    if (!q || !isspace((unsigned char)*q)) return line;
    while (isspace((unsigned char)*q)) q++;
    return q;
}

/* text.toLowerCase().replace(/[^\p{L}\p{N}]+/gu, " ").trim() || text (non-ASCII bytes count as letters) */
static char *fold_key(const char *text) {
    sb_t b = SB_INIT;
    bool gap = false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isalnum(*p) || *p >= 0x80) {
            if (gap && b.len) sb_putc(&b, ' ');
            gap = false;
            sb_putc(&b, (char)tolower(*p));
        } else gap = true;
    }
    if (!b.len) {
        sb_free(&b);
        return xstrdup(text);
    }
    return sb_detach(&b);
}

bool pred_parse_many(const char *reply, int max, strvec_t *out) {
    strvec_t raw = str_split(reply ? reply : "", "\n");
    strvec_t lines = {0};
    for (size_t i = 0; i < raw.n; i++) {
        size_t n = strlen(raw.v[i]);
        if (n && raw.v[i][n - 1] == '\r') raw.v[i][n - 1] = 0;
        const char *l = strip_marker(raw.v[i]);
        char *t = str_trim(l);
        bool blank = !*t;
        free(t);
        if (!blank) strvec_push(&lines, l);
    }
    strvec_free(&raw);
    strvec_t seen = {0};
    bool declined = false;
    for (size_t i = 0; i < lines.n && (int)out->n < max; i++) {
        if (!out->n && strstr(lines.v[i], SKIP_REPLY)) {
            declined = true;
            break;
        }
        char *text = pred_parse_one(lines.v[i]);
        if (!text) continue;
        size_t tl = strlen(text);
        if (i == 0 && i < lines.n - 1 && tl && text[tl - 1] == ':') { /* "Here are some options:" */
            free(text);
            continue;
        }
        char *key = fold_key(text);
        bool dup = false;
        for (size_t k = 0; k < seen.n && !dup; k++) dup = !strcmp(seen.v[k], key);
        if (dup) {
            free(key);
            free(text);
            continue;
        }
        strvec_push_own(&seen, key);
        strvec_push_own(out, text);
    }
    strvec_free(&seen);
    strvec_free(&lines);
    if (declined) strvec_free(out);
    return out->n > 0;
}

/* ---------- conversation point ---------- */

/* predictionAnchor: the last message that reaches the model and is no advisor card (borrowed pointer; compared only) */
static const json_t *anchor_of(app_t *a, size_t *count) {
    const json_t *found = NULL;
    pthread_mutex_lock(&a->agent->mu);
    const json_t *msgs = a->agent->messages;
    *count = json_len(msgs);
    for (size_t i = json_len(msgs); i-- > 0;) {
        const json_t *m = json_at(msgs, i);
        if (json_get_bool(m, "excludeFromContext", false)) continue;
        if (str_eq(json_get_str(m, "role"), "custom") && str_eq(json_get_str(m, "customType"), "advisor")) continue;
        found = m;
        break;
    }
    pthread_mutex_unlock(&a->agent->mu);
    return found;
}

static bool is_current(ui_t *u, const json_t *anchor, size_t count, app_t *view) {
    if (u->view != view) return false;
    size_t n = 0;
    const json_t *now = anchor_of(ui_view_app(u), &n);
    return now == anchor && n >= count && (anchor || n == count);
}

/* ---------- the side turn ---------- */

static void *job_main(void *p) {
    job_t *j = p;
    app_t *a = j->u->app;
    json_t *ctx = json_obj();
    json_set_str(ctx, "skip", SKIP_REPLY);
    if (j->alternatives) json_set_int(ctx, "choices", MAX_PREDICTION_CHOICES);
    char *prompt = prompt_render("coding-agent/prompts/system/composer-prediction-user.md", ctx);
    json_free(ctx);
    app_ephemeral_t e = {.prompt_text = prompt ? prompt : "",
                         .cancel = &j->cancel,
                         .usage_purpose = USAGE_PURPOSE,
                         .max_tokens = app_ephemeral_max_tokens_preserves_request(a) ? PREDICTION_MAX_TOKENS : 0,
                         .minimize_effort = true};
    j->assistant = app_ephemeral_turn(a, &e, &j->reply, &j->err);
    free(prompt);
    tui_post(j->u->tui, pred_job_done, j);
    return NULL;
}

static void job_free(job_t *j) {
    free(j->reply);
    free(j->err);
    json_free(j->assistant);
    free(j);
}

static void deadline_cb(void *ud) {
    job_t *j = ud;
    if (P.job != j) return;
    j->timer = 0;
    j->deadline = true;
    j->cancel = 1;
}

void pred_job_done(void *ud) {
    job_t *j = ud;
    if (!j->joined) pthread_join(j->th, NULL);
    for (size_t i = 0; i < P.running.n; i++)
        if (P.running.v[i] == j) {
            P.running.v[i] = P.running.v[--P.running.n];
            break;
        }
    ui_t *u = j->u;
    if (P.job == j) {
        P.job = NULL;
        if (j->timer) tui_remove_timer(u->tui, j->timer);
        /* #offer: still wanted, still the same conversation point, and the model guessed */
        if (!j->cancel && j->assistant && is_current(u, j->anchor, j->count, j->view)) {
            strvec_t choices = {0};
            bool ok;
            if (j->alternatives) ok = pred_parse_many(j->reply, MAX_PREDICTION_CHOICES, &choices);
            else {
                char *one = pred_parse_one(j->reply);
                ok = one != NULL;
                if (one) strvec_push_own(&choices, one);
            }
            if (ok) {
                strvec_free(&P.choices);
                P.choices = choices;
                P.anchor = j->anchor;
                P.count = j->count;
                P.view = j->view;
                P.live = true;
                tui_request_render(u->tui);
            } else strvec_free(&choices);
        }
    }
    job_free(j);
}

void pred_cancel(ui_t *u) {
    if (P.job) {
        P.job->cancel = 1;
        if (P.job->timer) tui_remove_timer(u->tui, P.job->timer);
        P.job->timer = 0;
        P.job = NULL; /* its done callback frees it */
    }
    if (!P.live) return;
    P.live = false;
    strvec_free(&P.choices);
    tui_request_render(u->tui);
}

void pred_request(ui_t *u) {
    pred_cancel(u);
    if (!settings_bool("composer.predictions", true)) return;
    /* a focused subagent's "user" is the parent agent, not the person at the composer */
    if (u->focus_id || u->view) return;
    app_t *a = u->app;
    /* any draft, even whitespace, keeps the ghost hidden; a compaction would rewrite the history it reads */
    char *draft = tui_editor_get_text(u->editor);
    bool typed = draft && *draft;
    free(draft);
    if (!a->model || u->worker_running || u->bg_kind || typed) return;
    job_t *j = xcalloc(1, sizeof *j);
    j->u = u;
    j->alternatives = settings_bool("composer.predictionAlternatives", false);
    j->anchor = anchor_of(a, &j->count);
    j->view = u->view;
    if (pthread_create(&j->th, NULL, job_main, j) != 0) {
        free(j);
        return;
    }
    P.job = j;
    vec_push(&P.running, j);
    j->timer = tui_add_timer(u->tui, PREDICTION_DEADLINE_MS, false, deadline_cb, j);
}

/* the editor's predictions: the offered alternatives while they still fit the conversation */
static const strvec_t *pred_choices(void *ud) {
    ui_t *u = ud;
    if (!P.live || !P.choices.n) return NULL;
    if (!settings_bool("composer.predictions", true)) return NULL;
    /* alternatives already offered keep their list only while the setting stays on */
    if (P.choices.n > 1 && !settings_bool("composer.predictionAlternatives", false)) return NULL;
    if (!is_current(u, P.anchor, P.count, P.view)) return NULL;
    return &P.choices;
}

static void pred_accepted(void *ud, int index) { (void)ud, (void)index; /* telemetry only */ }

static char *tab_hint(void) { return ui_editor_key("tui.input.tab"); }

void pred_attach(ui_t *u) {
    tui_editor_set_predictions(u->editor, pred_choices, pred_accepted, u);
    u->editor->prediction_tab_hint = tab_hint;
}

void pred_shutdown(ui_t *u) {
    pred_cancel(u);
    /* the posted done callbacks never run after teardown: wait for the threads here */
    for (size_t i = 0; i < P.running.n; i++) {
        P.running.v[i]->cancel = 1;
        pthread_join(P.running.v[i]->th, NULL);
        P.running.v[i]->joined = true;
    }
    P.running.n = 0;
}
