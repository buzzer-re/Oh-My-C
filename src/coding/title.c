/* title.c - automatic session titles (port of omp utils/title-card.ts, tiny/text.ts and the
 * agent-session title flow). Under `title.generator: fork` (default) the title is a side request
 * on the session's own model, system prompt, tools and history (so it reads the prompt cache),
 * started as soon as the reply to the operator's message begins; it asks for a card title
 * (`🧪 FLAKY: Fix flaky park tests`); like omp's runEphemeralTurn it carries the streaming reply so far, the
 * side-channel no-tools reminder and the session's thinking level. The title model (`title-system.md`) is the fallback and the
 * only path under `title.generator: tiny`. Results are applied on the agent thread. */
#define _GNU_SOURCE /* memmem, strcasestr, vasprintf: glibc before 2.38 declares them only with it */
#include "title.h"

#include "config/settings.h"
#include "prompt/template.h"

#include <ctype.h>
#include <pthread.h>

#define MAX_TITLE_CHARS 80
#define MAX_TITLE_WORDS 12

static const char *FILLER[] = {"hi", "hii", "hiii", "hiya", "hey", "heya", "hello", "helo", "hullo", "yo", "ya", "sup", "wassup",
                               "whatsup", "howdy", "greetings", "hola", "ciao", "aloha", "gm", "gn", "good", "morning", "afternoon",
                               "evening", "night", "day", "thanks", "thank", "thx", "ty", "tysm", "cheers", "please", "pls", "plz",
                               "ok", "okay", "okey", "k", "kk", "yep", "yes", "yeah", "yup", "nope", "no", "nah", "sure", "cool",
                               "nice", "great", "awesome", "perfect", "lol", "lmao", "haha", "hehe", "test", "tests", "testing",
                               "ping", "pong", "there", "you", "u", "hmm", "hmmm", "um", "uh", "so", "well", "anyway"};

/* word = run of letters/digits; non-ASCII bytes count as letters (unicode words) */
static bool word_byte(unsigned char c) { return isalnum(c) || c >= 0x80; }

static int count_words(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (word_byte(*p)) {
            n++;
            while (*p && word_byte(*p)) p++;
        } else {
            p++;
        }
    }
    return n;
}

bool title_low_signal(const char *msg) {
    if (!msg) return true;
    const unsigned char *p = (const unsigned char *)msg;
    bool any = false;
    while (*p) {
        if (!word_byte(*p)) {
            p++;
            continue;
        }
        const unsigned char *s = p;
        while (*p && word_byte(*p)) p++;
        size_t n = (size_t)(p - s);
        char w[32];
        if (n >= sizeof w) return false;
        for (size_t i = 0; i < n; i++) w[i] = (char)tolower(s[i]);
        w[n] = 0;
        any = true;
        bool digits = strspn(w, "0123456789") == n, filler = false;
        for (size_t i = 0; i < ARRAY_LEN(FILLER) && !filler; i++) filler = !strcmp(w, FILLER[i]);
        if (!digits && !filler) return false;
    }
    UNUSED(any);
    return true;
}

char *title_normalize(const char *value) {
    if (!value) return NULL;
    char *t = str_trim(value);
    char *nl = strpbrk(t, "\r\n");
    if (nl) *nl = 0;
    char *s = str_trim(t);
    free(t);
    size_t n = strlen(s);
    char *b = s;
    if (*b == '"' || *b == '\'') b++, n--;
    if (n && (b[n - 1] == '"' || b[n - 1] == '\'')) b[--n] = 0;
    if (!strncasecmp(b, "<title/>", 8) || !strncasecmp(b, "<title />", 9)) {
        free(s);
        return NULL;
    }
    if (!strncasecmp(b, "<title>", 7)) b += 7;
    n = strlen(b);
    if (n >= 8 && !strcasecmp(b + n - 8, "</title>")) b[n -= 8] = 0;
    if (*b == '"' || *b == '\'') b++;
    n = strlen(b);
    if (n && (b[n - 1] == '"' || b[n - 1] == '\'')) b[--n] = 0;
    if (n && strchr(".!?", b[n - 1])) b[--n] = 0;
    char *r = str_trim(b);
    free(s);
    if (!*r || !strcasecmp(r, "none") || count_words(r) == 0 || strlen(r) > MAX_TITLE_CHARS || count_words(r) > MAX_TITLE_WORDS) {
        free(r);
        return NULL;
    }
    return r;
}

/* a card icon: 1-8 code points, none of them ASCII */
static bool card_icon(const char *s) {
    if (!s || !*s) return false;
    int cps = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x80) return false;
        if ((*p & 0xC0) != 0x80 && ++cps > 8) return false;
    }
    return cps > 0;
}

static bool card_code(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 6) return false;
    for (size_t i = 0; i < n; i++)
        if (!(isupper((unsigned char)s[i]) || isdigit((unsigned char)s[i]))) return false;
    return true;
}

static char *attr(const char *attrs, size_t len, const char *name) {
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < len; i++) {
        if (strncasecmp(attrs + i, name, nl) || (i && isalnum((unsigned char)attrs[i - 1]))) continue;
        size_t j = i + nl;
        while (j < len && isspace((unsigned char)attrs[j])) j++;
        if (j >= len || attrs[j] != '=') continue;
        j++;
        while (j < len && isspace((unsigned char)attrs[j])) j++;
        if (j >= len || (attrs[j] != '"' && attrs[j] != '\'')) continue;
        char q = attrs[j++];
        const char *e = memchr(attrs + j, q, len - j);
        if (!e) return NULL;
        return xstrndup(attrs + j, (size_t)(e - (attrs + j)));
    }
    return NULL;
}

char *title_parse_card_reply(const char *reply, const char *icons) {
    const char *tag = reply ? strcasestr(reply, "<title") : NULL;
    if (!tag) return NULL;
    const char *gt = strchr(tag, '>');
    if (!gt) return NULL;
    if (gt > tag && gt[-1] == '/') return NULL; /* <title/>: declined */
    const char *attrs = tag + 6;
    size_t alen = (size_t)(gt - attrs);
    const char *end = strcasestr(gt + 1, "</title>");
    if (!end) return NULL;
    char *raw = xstrndup(gt + 1, (size_t)(end - gt - 1));
    /* collapse whitespace */
    sb_t tb = SB_INIT;
    bool sp = false;
    for (const char *p = raw; *p; p++) {
        if (isspace((unsigned char)*p)) sp = tb.len > 0;
        else {
            if (sp) sb_putc(&tb, ' ');
            sp = false;
            sb_putc(&tb, *p);
        }
    }
    free(raw);
    char *text = sb_detach(&tb);
    char *emoji = attr(attrs, alen, "emoji"), *code = attr(attrs, alen, "code");
    char *icon = NULL, *cc = NULL;
    if (code) {
        for (char *p = code; *p; p++) *p = (char)toupper((unsigned char)*p);
        char *c2 = str_trim(code);
        if (card_code(c2) && card_icon(emoji ? emoji : "")) {
            icon = xstrdup(emoji);
            cc = xstrdup(c2);
        }
        free(c2);
    } else {
        /* `🧪 FLAKY: title` line form, or `CODE title` with the emoji as an attribute */
        char *sp1 = strchr(text, ' ');
        if (sp1) {
            char *first = xstrndup(text, (size_t)(sp1 - text));
            char *rest = sp1 + 1, *colon = strstr(rest, ": ");
            if (card_icon(first) && colon) {
                char *c = xstrndup(rest, (size_t)(colon - rest));
                if (card_code(c)) {
                    icon = xstrdup(first);
                    cc = c;
                    memmove(text, colon + 2, strlen(colon + 2) + 1);
                } else {
                    free(c);
                }
            } else if (card_icon(emoji ? emoji : "")) {
                char *c = xstrdup(first);
                size_t cl = strlen(c);
                if (cl && c[cl - 1] == ':') c[--cl] = 0;
                if (cl >= 2 && card_code(c)) {
                    icon = xstrdup(emoji);
                    cc = c;
                    memmove(text, sp1 + 1, strlen(sp1 + 1) + 1);
                } else {
                    free(c);
                }
            }
            free(first);
        }
    }
    char *title = title_normalize(text);
    char *out = NULL;
    if (title) out = icon && cc && !str_eq(icons, "boring") ? xasprintf("%s %s: %s", icon, cc, title) : xstrdup(title);
    free(title);
    free(text);
    free(emoji);
    free(code);
    free(icon);
    free(cc);
    return out;
}

/* ---------- runtime ---------- */

struct title_state {
    pthread_mutex_t mu;
    char *input;    /* operator text the title is named from */
    bool tried;     /* one try per operator message */
    bool running;
    pthread_t th;
    char *result;   /* applied on the agent thread */
};

static title_state_t *ST(app_t *a) {
    if (!a->title) {
        a->title = xcalloc(1, sizeof(title_state_t));
        pthread_mutex_init(&a->title->mu, NULL);
    }
    return a->title;
}

static const char *icons_setting(void) {
    const char *i = settings_str("title.icons", "nf+emoji");
    /* no bundled Nerd Fonts catalog: the emoji is always the icon shown */
    return str_eq(i, "boring") ? "boring" : "emoji";
}

typedef struct {
    app_t *a;
    char *input;
    json_t *streaming; /* the streaming assistant reply when the title forked */
} job_t;

/* AgentSession.#forkTitle: an ephemeral side turn (runEphemeralTurn) forked from the streaming reply on the session's
 * model, system prompt, tools, history and thinking level, so it extends the main request's cached prefix */
static char *fork_title(job_t *j) {
    app_t *a = j->a;
    const char *icons = icons_setting();
    json_t *c = json_obj();
    json_set_bool(c, "card", !str_eq(icons, "boring"));
    json_set_bool(c, "nerdFonts", false);
    char *req = prompt_render("coding-agent/prompts/system/title-fork.md", c);
    json_free(c);
    app_ephemeral_t e = {.prompt_text = req ? req : "", .streaming = j->streaming, .cancel = &a->agent->cancel};
    char *reply = NULL, *err = NULL;
    json_t *r = app_ephemeral_turn(a, &e, &reply, &err);
    char *out = r && reply ? title_parse_card_reply(reply, icons) : NULL;
    json_free(r);
    free(reply);
    free(err);
    free(req);
    return out;
}

static void *title_thread(void *p) {
    job_t *j = p;
    app_t *a = j->a;
    char *t = NULL;
    if (str_eq(settings_str("title.generator", "fork"), "fork")) t = fork_title(j);
    if (!t) {
        char *raw = app_generate_title_from(a, j->input);
        t = raw ? title_normalize(raw) : NULL;
        if (!t && raw) t = title_parse_card_reply(raw, "boring");
        free(raw);
    }
    title_state_t *s = a->title;
    pthread_mutex_lock(&s->mu);
    free(s->result);
    s->result = t;
    s->running = false;
    pthread_mutex_unlock(&s->mu);
    json_free(j->streaming);
    free(j->input);
    free(j);
    return NULL;
}

static bool titles_enabled(app_t *a) {
    return !a->opts.subagent && !env_truthy("PI_NO_TITLE") && !env_truthy("OMC_NO_TITLE") && a->session && !a->session->title;
}

void title_on_event(app_t *a, const agent_event_t *ev) {
    if (!titles_enabled(a)) return;
    title_state_t *s = ST(a);
    if (ev->type == AGENT_EV_MESSAGE_END && str_eq(json_get_str(ev->message, "role"), "user") &&
        !json_get_bool(ev->message, "synthetic", false)) {
        char *txt = ai_message_text(ev->message);
        pthread_mutex_lock(&s->mu);
        if (!title_low_signal(txt)) {
            free(s->input);
            s->input = txt;
            txt = NULL;
            s->tried = false;
        }
        pthread_mutex_unlock(&s->mu);
        free(txt);
    } else if (ev->type == AGENT_EV_MESSAGE_UPDATE && ev->ai &&
               (ev->ai->type == AI_EV_TEXT_START || ev->ai->type == AI_EV_TOOLCALL_START)) {
        pthread_mutex_lock(&s->mu);
        bool go = s->input && !s->tried && !s->running;
        if (go) {
            s->tried = true;
            s->running = true;
            if (s->th) {
                pthread_join(s->th, NULL);
                s->th = 0;
            }
        }
        pthread_mutex_unlock(&s->mu);
        if (!go) return;
        job_t *j = xcalloc(1, sizeof(*j));
        j->a = a;
        j->input = xstrdup(s->input);
        j->streaming = ev->message ? json_clone(ev->message) : NULL;
        if (pthread_create(&s->th, NULL, title_thread, j) != 0) {
            pthread_mutex_lock(&s->mu);
            s->running = false;
            pthread_mutex_unlock(&s->mu);
            json_free(j->streaming);
            free(j->input);
            free(j);
        }
    }
    title_apply(a);
}

void title_apply(app_t *a) {
    title_state_t *s = a->title;
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    char *t = s->result;
    s->result = NULL;
    pthread_mutex_unlock(&s->mu);
    if (!t) return;
    if (a->session && !a->session->title) {
        session_set_title(a->session, t, "auto");
        if (a->notice) a->notice(a->notice_ud, "title", t);
    }
    free(t);
}

void title_wait(app_t *a, int64_t timeout_ms) {
    title_state_t *s = a->title;
    if (!s) return;
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&s->mu);
        bool running = s->running;
        pthread_mutex_unlock(&s->mu);
        if (!running || now_ms() >= end) break;
        struct timespec ts = {0, 20 * 1000000L};
        nanosleep(&ts, NULL);
    }
}

void title_free(app_t *a) {
    title_state_t *s = a->title;
    if (!s) return;
    if (s->th) pthread_join(s->th, NULL);
    free(s->input);
    free(s->result);
    pthread_mutex_destroy(&s->mu);
    free(s);
    a->title = NULL;
}
