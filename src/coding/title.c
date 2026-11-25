/* title.c - automatic session titles (port of omp utils/title-card.ts, tiny/text.ts and the
 * agent-session title flow). Under `title.generator: fork` (default) the title is a side request
 * on the session's own model, system prompt, tools and history (so it reads the prompt cache),
 * started as soon as the reply to the operator's message begins; it asks for a card title
 * (`🧪 FLAKY: Fix flaky park tests`); like omp's runEphemeralTurn it carries the streaming reply so far, the
 * side-channel no-tools reminder and the session's thinking level. The title model (`title-system.md`) is the fallback and the
 * only path under `title.generator: tiny`. Results are applied on the agent thread. */
#define _GNU_SOURCE /* memmem, strcasestr, vasprintf: glibc before 2.38 declares them only with it */
#include "title.h"

#include "ai/think_heal.h"
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

static bool is_quote(char c) { return c == '"' || c == '\''; }

/* `.replace(/^["']|["']$/g, "")` in place: one leading and one trailing quote; returns the new start */
static char *unquote_ends(char *b) {
    if (is_quote(*b)) b++;
    size_t n = strlen(b);
    if (n && is_quote(b[n - 1])) b[n - 1] = 0;
    return b;
}

/* UTF-16 length (JS string length) */
static size_t utf16_units(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n += *p >= 0xF0 ? 2 : 1;
    return n;
}

/* isNoTitleSentinel (tiny/text.ts): once unquoted and stripped of one trailing sentence mark, the value is `none` */
bool title_is_no_title(const char *value) {
    char *t = str_trim(value ? value : "");
    char *b = unquote_ends(t);
    size_t n = strlen(b);
    if (n && strchr(".!?", b[n - 1])) b[n - 1] = 0;
    char *bare = str_trim(b);
    bool none = !strcasecmp(bare, "none");
    free(bare);
    free(t);
    return none;
}

/* `^<title\s*\/>$` (case-insensitive) */
static bool self_closed_marker(const char *s) {
    if (strncasecmp(s, "<title", 6)) return false;
    s += 6;
    while (isspace((unsigned char)*s)) s++;
    return !strcmp(s, "/>");
}

/* normalizeGeneratedTitle (tiny/text.ts) */
char *title_normalize(const char *value) {
    if (!value) return NULL;
    char *t = str_trim(value);
    char *nl = strchr(t, '\n'); /* split(/\r?\n/, 1)[0] */
    if (nl) {
        if (nl > t && nl[-1] == '\r') nl--;
        *nl = 0;
    }
    char *first = str_trim(t);
    free(t);
    if (!*first) {
        free(first);
        return NULL;
    }
    char *s = str_trim(unquote_ends(first));
    free(first);
    if (self_closed_marker(s)) {
        free(s);
        return NULL;
    }
    char *b = s;
    if (!strncasecmp(b, "<title>", 7)) b += 7;
    size_t n = strlen(b);
    if (n >= 8 && !strcasecmp(b + n - 8, "</title>")) b[n -= 8] = 0;
    b = unquote_ends(b);
    n = strlen(b);
    if (n && strchr(".!?", b[n - 1])) b[--n] = 0;
    char *r = str_trim(b);
    free(s);
    if (!*r || title_is_no_title(r) || count_words(r) == 0 || utf16_units(r) > MAX_TITLE_CHARS || count_words(r) > MAX_TITLE_WORDS) {
        free(r);
        return NULL;
    }
    return r;
}

/* ---------- the title model's reply (utils/title-generator.ts extractGeneratedTitle) ---------- */

static bool word_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* `<think>` / `<thinking>` / `<reasoning>` opening at s (case-insensitive): its length, *name the tag name */
static size_t think_tag_open(const char *s, const char **name) {
    static const char *const N[] = {"think", "thinking", "reasoning"};
    if (*s != '<') return 0;
    for (size_t i = 0; i < ARRAY_LEN(N); i++) {
        size_t n = strlen(N[i]);
        if (!strncasecmp(s + 1, N[i], n) && s[1 + n] == '>') {
            *name = N[i];
            return n + 2;
        }
    }
    return 0;
}

/* `<(think|thinking|reasoning)>\s*[\s\S]*?<\/\1>` matched at t[p]: the match end, 0 for none */
static size_t tag_envelope_at(const char *t, size_t p) {
    const char *name;
    size_t ol = think_tag_open(t + p, &name), nl;
    if (!ol) return 0;
    nl = strlen(name);
    for (const char *q = t + p + ol; (q = strstr(q, "</")); q++)
        if (!strncasecmp(q + 2, name, nl) && q[2 + nl] == '>') return (size_t)(q - t) + nl + 3;
    return 0;
}

/* "```(?:thinking|reasoning)\b[\s\S]*?```" matched at t[p]: the match end, 0 for none */
static size_t fence_envelope_at(const char *t, size_t p) {
    static const char *const W[] = {"thinking", "reasoning"};
    if (strncmp(t + p, "```", 3)) return 0;
    for (size_t i = 0; i < ARRAY_LEN(W); i++) {
        size_t n = strlen(W[i]);
        if (strncasecmp(t + p + 3, W[i], n) || word_char(t[p + 3 + n])) continue;
        const char *c = strstr(t + p + 3 + n, "```");
        return c ? (size_t)(c - t) + 3 : 0;
    }
    return 0;
}

/* isInsideEnvelopeMatchedBy over the tag or the fence envelopes */
static bool inside_envelope(const char *t, size_t index, bool fence) {
    size_t len = strlen(t);
    for (size_t p = 0; p < len;) {
        size_t end = fence ? fence_envelope_at(t, p) : tag_envelope_at(t, p);
        if (!end) {
            p++;
            continue;
        }
        if (index > p && index < end) return true;
        if (p > index) return false;
        p = end;
    }
    return false;
}

static void heal_collect(void *ud, int kind, const char *s, size_t n) {
    if (kind == THINK_HEAL_TEXT) sb_appendn(ud, s, n);
}

/* stripLeakedThinkingMarkup: the visible text StreamMarkupHealing leaves (owned) */
static char *strip_leaked_thinking(const char *s, size_t n) {
    think_heal_t h;
    think_heal_init(&h);
    sb_t out = SB_INIT;
    think_heal_feed(&h, s, n, heal_collect, &out);
    think_heal_flush(&h, heal_collect, &out);
    think_heal_free(&h);
    return sb_detach(&out);
}

#define TITLE_VISIBILITY_SENTINEL "\xEE\x80\x80omp-title-visible\xEE\x80\x80"

/* isVisibleTitleMarker */
static bool visible_marker(const char *t, size_t index) {
    if (inside_envelope(t, index, false) || inside_envelope(t, index, true)) return false;
    sb_t b = SB_INIT;
    sb_appendn(&b, t, index);
    sb_append(&b, TITLE_VISIBILITY_SENTINEL);
    char *v = strip_leaked_thinking(b.s, b.len);
    bool ok = str_ends(v, TITLE_VISIBILITY_SENTINEL);
    free(v);
    sb_free(&b);
    return ok;
}

/* the TITLE_MARKER_GLOBAL_RE alternatives at t[p] (`<title>([\s\S]*?)<\/title>`, `<title\s*\/>`, `<title>\s*$`): the match
 * end (0: none), its trimmed content (owned) and whether it is closed (anything but the bare trailing `<title>`) */
static size_t title_marker_at(const char *t, size_t p, char **content, bool *closed) {
    const char *s = t + p;
    if (strncasecmp(s, "<title", 6)) return 0;
    if (s[6] == '>') {
        const char *c = str_istr(s + 7, "</title>");
        if (c) {
            char *raw = xstrndup(s + 7, (size_t)(c - (s + 7)));
            *content = str_trim(raw);
            free(raw);
            *closed = true;
            return (size_t)(c - t) + 8;
        }
    }
    const char *q = s + 6;
    while (isspace((unsigned char)*q)) q++;
    if (q[0] == '/' && q[1] == '>') {
        *content = xstrdup("");
        *closed = true;
        return (size_t)(q + 2 - t);
    }
    if (s[6] == '>') {
        q = s + 7;
        while (isspace((unsigned char)*q)) q++;
        if (!*q) {
            *content = xstrdup("");
            *closed = false;
            return (size_t)(q - t);
        }
    }
    return 0;
}

/* extractVisibleMarkedTitle: the first marker outside leaked thinking; true with *content (owned) and *closed */
static bool visible_marked_title(const char *t, char **content, bool *closed) {
    size_t len = strlen(t);
    for (size_t p = 0; p < len;) {
        char *c = NULL;
        bool cl = false;
        size_t end = title_marker_at(t, p, &c, &cl);
        if (!end) {
            p++;
            continue;
        }
        if (visible_marker(t, p)) {
            *content = c;
            *closed = cl;
            return true;
        }
        free(c);
        p = end;
    }
    return false;
}

static const char *skip_space(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    return s;
}

/* stripLeadingLeakedThinkingMarkup: LEADING_THINKING_TAG_RE and LEADING_THINKING_FENCE_RE until neither applies */
static const char *strip_leading_thinking(const char *t) {
    for (;;) {
        const char *s = skip_space(t);
        size_t end = tag_envelope_at(s, 0);
        const char *after = end ? skip_space(s + end) : t;
        s = skip_space(after);
        end = fence_envelope_at(s, 0);
        if (end) after = skip_space(s + end);
        if (after == t) return t;
        t = after;
    }
}

/* LEADING_PROSE_THINKING_PREAMBLE_RE: `[ \t]*((here('s|’s| is)[ \t]+(a|the|my)[ \t]+)|my[ \t]+)?(thinking|thought|reasoning)[ \t]+
 * process[ \t]*:?[ \t]*(\r?\n|$)`, case-insensitive */
static bool prose_thinking_preamble(const char *s) {
    static const char *const OPENERS[] = {"here's", "here\xE2\x80\x99s", "here is"};
    static const char *const ARTICLES[] = {"a", "the", "my"};
    static const char *const NOUNS[] = {"thinking", "thought", "reasoning"};
    while (*s == ' ' || *s == '\t') s++;
    const char *p = s;
    bool opened = false;
    for (size_t i = 0; i < ARRAY_LEN(OPENERS) && !opened; i++) {
        size_t n = strlen(OPENERS[i]);
        if (strncasecmp(p, OPENERS[i], n) || (p[n] != ' ' && p[n] != '\t')) continue;
        const char *q = p + n;
        while (*q == ' ' || *q == '\t') q++;
        for (size_t k = 0; k < ARRAY_LEN(ARTICLES); k++) {
            size_t an = strlen(ARTICLES[k]);
            if (!strncasecmp(q, ARTICLES[k], an) && (q[an] == ' ' || q[an] == '\t')) {
                q += an;
                while (*q == ' ' || *q == '\t') q++;
                p = q;
                opened = true;
                break;
            }
        }
    }
    if (!opened && !strncasecmp(p, "my", 2) && (p[2] == ' ' || p[2] == '\t')) {
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
    }
    /* no noun starts with `here` or `my`, so a taken opener never has to be given back */
    for (size_t i = 0; i < ARRAY_LEN(NOUNS); i++) {
        size_t n = strlen(NOUNS[i]);
        if (strncasecmp(p, NOUNS[i], n) || (p[n] != ' ' && p[n] != '\t')) continue;
        const char *q = p + n;
        while (*q == ' ' || *q == '\t') q++;
        if (strncasecmp(q, "process", 7)) continue;
        q += 7;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') q++;
        while (*q == ' ' || *q == '\t') q++;
        return !*q || *q == '\n' || (q[0] == '\r' && q[1] == '\n');
    }
    return false;
}

/* remove every `<title>` / `</title>` (case-insensitive) */
static char *drop_title_tags(const char *s) {
    sb_t b = SB_INIT;
    while (*s) {
        if (!strncasecmp(s, "<title>", 7)) s += 7;
        else if (!strncasecmp(s, "</title>", 8)) s += 8;
        else sb_putc(&b, *s++);
    }
    return sb_detach(&b);
}

/* unwrapJsonTitle: `{"title": "..."}` (optionally fenced) to the bare title; owned. *threw: a salvaged title value that
 * is not a JSON string (JSON.parse throws, which fails this reply) */
static char *unwrap_json_title(const char *candidate, bool *threw) {
    const char *s = candidate;
    if (!strncmp(s, "```", 3)) {
        s += 3;
        if (!strncasecmp(s, "json", 4)) s += 4;
        s = skip_space(s);
    }
    char *t = xstrdup(s);
    size_t n = strlen(t);
    if (n >= 3 && !strcmp(t + n - 3, "```")) t[n - 3] = 0;
    char *text = str_trim(t);
    free(t);
    char *out = NULL;
    if (*text == '{') {
        json_t *j = json_parse(text, strlen(text), NULL);
        if (j) {
            if (json_is_obj(j) && json_get_str(j, "title")) out = str_trim(json_get_str(j, "title"));
        } else {
            /* /"title"\s*:\s*("(?:[^"\\]|\\.)*")/ */
            for (const char *k = strstr(text, "\"title\""); k && !out && !*threw; k = strstr(k + 1, "\"title\"")) {
                const char *q = skip_space(k + 7);
                if (*q != ':') continue;
                q = skip_space(q + 1);
                if (*q != '"') continue;
                const char *e = q + 1;
                while (*e && *e != '"') {
                    if (*e == '\\') {
                        if (!e[1] || e[1] == '\n' || e[1] == '\r') break;
                        e++;
                    }
                    e++;
                }
                if (*e != '"') continue;
                json_t *v = json_parse(q, (size_t)(e + 1 - q), NULL);
                if (json_is_str(v)) out = str_trim(json_sv(v));
                else *threw = true;
                json_free(v);
            }
        }
        json_free(j);
    }
    free(text);
    return out ? out : xstrdup(candidate);
}

int title_extract_generated(const char *text, char **out) {
    *out = NULL;
    if (!text) text = "";
    char *content = NULL, *candidate;
    bool closed = false;
    bool marked = visible_marked_title(text, &content, &closed);
    if (marked) {
        candidate = content;
    } else {
        char *dropped = drop_title_tags(strip_leading_thinking(text));
        candidate = str_trim(dropped);
        free(dropped);
        if (prose_thinking_preamble(candidate)) {
            free(candidate);
            *out = xstrdup("");
            return 1;
        }
    }
    bool threw = false;
    char *title = unwrap_json_title(candidate, &threw);
    free(candidate);
    if (threw) {
        free(title);
        return -1;
    }
    char *trimmed = str_trim(title);
    bool declined = (marked && closed && !*trimmed) || title_is_no_title(title);
    free(trimmed);
    if (declined) {
        free(title);
        return 0;
    }
    *out = title;
    return 1;
}

int title_judge_reply(const char *reply, char **title) {
    *title = NULL;
    char *x = NULL;
    int r = title_extract_generated(reply, &x);
    if (r == 0) return TITLE_DECLINED;
    char *t = r > 0 ? title_normalize(x) : NULL;
    free(x);
    /* omc: a card-shaped reply (`<title emoji=… code=…>`) that the plain path cannot use */
    if (!t && reply) t = title_parse_card_reply(reply, "boring");
    *title = t;
    return t ? TITLE_OK : TITLE_INVALID;
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
    app_ephemeral_t e = {.prompt_text = req ? req : "", .streaming = j->streaming, .cancel = &a->agent->cancel,
                         .usage_purpose = "title"};
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
    if (!t) t = app_generate_title_from(a, j->input);
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
