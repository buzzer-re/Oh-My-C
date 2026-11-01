/* web.c - URL reading (HTML -> markdown) and the web_search tool with a provider chain. */
#include "web.h"

#include "config/settings.h"
#include "core/fs.h"
#include "net/http.h"

#include <ctype.h>

/* ---------- HTML -> markdown ---------- */


/* web_search request timeout (`providers.webSearchTimeoutSeconds`, omp default 60s) */
static int64_t ws_timeout_ms(void) {
    int64_t s = settings_int("providers.webSearchTimeoutSeconds", 60);
    return (s > 0 ? s : 60) * 1000;
}
static const struct {
    const char *name;
    const char *val;
} ENTITIES[] = {
    {"amp", "&"},   {"lt", "<"},     {"gt", ">"},      {"quot", "\""},   {"apos", "'"},    {"nbsp", " "},
    {"mdash", "—"}, {"ndash", "–"},  {"hellip", "…"},  {"copy", "©"},    {"reg", "®"},     {"trade", "™"},
    {"laquo", "«"}, {"raquo", "»"},  {"ldquo", "“"},   {"rdquo", "”"},   {"lsquo", "‘"},   {"rsquo", "’"},
    {"bull", "•"},  {"middot", "·"}, {"times", "×"},   {"divide", "÷"},  {"deg", "°"},     {"euro", "€"},
    {"pound", "£"}, {"yen", "¥"},    {"cent", "¢"},    {"sect", "§"},    {"para", "¶"},    {"rarr", "→"},
    {"larr", "←"},  {"uarr", "↑"},   {"darr", "↓"},    {"le", "≤"},      {"ge", "≥"},      {"ne", "≠"},
};

static size_t decode_entity(const char *s, size_t n, sb_t *out) {
    /* s points at '&' */
    size_t i = 1;
    while (i < n && i < 12 && s[i] != ';' && (isalnum((unsigned char)s[i]) || s[i] == '#')) i++;
    if (i >= n || s[i] != ';') {
        sb_putc(out, '&');
        return 1;
    }
    if (s[1] == '#') {
        uint32_t cp = (s[2] == 'x' || s[2] == 'X') ? (uint32_t)strtoul(s + 3, NULL, 16) : (uint32_t)strtoul(s + 2, NULL, 10);
        if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
        sb_append_utf8(out, cp);
        return i + 1;
    }
    for (size_t k = 0; k < ARRAY_LEN(ENTITIES); k++) {
        size_t l = strlen(ENTITIES[k].name);
        if (l == i - 1 && strncmp(s + 1, ENTITIES[k].name, l) == 0) {
            sb_append(out, ENTITIES[k].val);
            return i + 1;
        }
    }
    sb_appendn(out, s, i + 1);
    return i + 1;
}

typedef struct {
    sb_t out;
    int pre;          /* inside <pre> */
    int skip;         /* inside script/style/etc */
    int list_depth;
    int ol_counter[16];
    bool ol[16];
    char *href;
    size_t link_start;
    bool need_space;
    const char *base;
    char *title;
    bool in_title;
    sb_t title_buf;
} h2m_t;

static void ensure_blank(h2m_t *h) {
    sb_t *o = &h->out;
    while (o->len && (o->s[o->len - 1] == ' ')) o->len--;
    if (!o->len) return;
    if (o->s[o->len - 1] != '\n') sb_putc(o, '\n');
    if (o->len < 2 || o->s[o->len - 2] != '\n') sb_putc(o, '\n');
    o->s[o->len] = 0;
}

static void ensure_nl(h2m_t *h) {
    sb_t *o = &h->out;
    while (o->len && o->s[o->len - 1] == ' ') o->len--;
    if (o->len && o->s[o->len - 1] != '\n') sb_putc(o, '\n');
    if (o->s) o->s[o->len] = 0;
}

static char *attr_val(const char *tag, size_t n, const char *name) {
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < n; i++) {
        if (strncasecmp(tag + i, name, nl) == 0 && (i == 0 || isspace((unsigned char)tag[i - 1]))) {
            size_t j = i + nl;
            while (j < n && isspace((unsigned char)tag[j])) j++;
            if (j >= n || tag[j] != '=') continue;
            j++;
            while (j < n && isspace((unsigned char)tag[j])) j++;
            char q = (j < n && (tag[j] == '"' || tag[j] == '\'')) ? tag[j++] : 0;
            size_t st = j;
            while (j < n && (q ? tag[j] != q : !isspace((unsigned char)tag[j]) && tag[j] != '>')) j++;
            return xstrndup(tag + st, j - st);
        }
    }
    return NULL;
}

static bool tag_is(const char *name, const char *t) { return strcasecmp(name, t) == 0; }

static void handle_tag(h2m_t *h, const char *tag, size_t n) {
    bool closing = n && tag[0] == '/';
    const char *p = tag + (closing ? 1 : 0);
    char name[16];
    size_t k = 0;
    while (k < sizeof name - 1 && p + k < tag + n && (isalnum((unsigned char)p[k]))) {
        name[k] = (char)tolower((unsigned char)p[k]);
        k++;
    }
    name[k] = 0;
    if (!k) return;
    static const char *skip_tags[] = {"script", "style", "noscript", "svg", "template", "iframe", "head"};
    for (size_t i = 0; i < ARRAY_LEN(skip_tags); i++) {
        if (tag_is(name, skip_tags[i])) {
            if (tag_is(name, "head")) return; /* head content handled via title only */
            if (closing) {
                if (h->skip) h->skip--;
            } else if (!(n && tag[n - 1] == '/')) {
                h->skip++;
            }
            return;
        }
    }
    if (tag_is(name, "title")) {
        h->in_title = !closing;
        if (closing && !h->title) h->title = sb_detach(&h->title_buf);
        return;
    }
    if (h->skip) return;
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        ensure_blank(h);
        if (!closing) {
            for (int i = 0; i < name[1] - '0'; i++) sb_putc(&h->out, '#');
            sb_putc(&h->out, ' ');
        }
        return;
    }
    if (tag_is(name, "p") || tag_is(name, "div") || tag_is(name, "section") || tag_is(name, "article") ||
        tag_is(name, "header") || tag_is(name, "footer") || tag_is(name, "table") || tag_is(name, "blockquote") ||
        tag_is(name, "main") || tag_is(name, "figure")) {
        ensure_blank(h);
        if (!closing && tag_is(name, "blockquote")) sb_append(&h->out, "> ");
        return;
    }
    if (tag_is(name, "br")) {
        sb_putc(&h->out, '\n');
        return;
    }
    if (tag_is(name, "hr")) {
        ensure_blank(h);
        sb_append(&h->out, "---\n\n");
        return;
    }
    if (tag_is(name, "tr")) {
        ensure_nl(h);
        if (!closing) sb_append(&h->out, "| ");
        return;
    }
    if (tag_is(name, "td") || tag_is(name, "th")) {
        if (closing) sb_append(&h->out, " | ");
        return;
    }
    if (tag_is(name, "ul") || tag_is(name, "ol")) {
        if (closing) {
            if (h->list_depth) h->list_depth--;
            ensure_blank(h);
        } else {
            ensure_nl(h);
            if (h->list_depth < 15) h->list_depth++;
            h->ol[h->list_depth] = tag_is(name, "ol");
            h->ol_counter[h->list_depth] = 0;
        }
        return;
    }
    if (tag_is(name, "li")) {
        if (closing) return;
        ensure_nl(h);
        for (int i = 1; i < h->list_depth; i++) sb_append(&h->out, "  ");
        if (h->ol[h->list_depth]) sb_appendf(&h->out, "%d. ", ++h->ol_counter[h->list_depth]);
        else sb_append(&h->out, "- ");
        return;
    }
    if (tag_is(name, "pre")) {
        if (closing) {
            if (h->pre) h->pre--;
            ensure_nl(h);
            sb_append(&h->out, "```\n\n");
        } else {
            ensure_blank(h);
            sb_append(&h->out, "```\n");
            h->pre++;
        }
        return;
    }
    if (tag_is(name, "code")) {
        if (!h->pre) sb_putc(&h->out, '`');
        return;
    }
    if (tag_is(name, "strong") || tag_is(name, "b")) {
        sb_append(&h->out, "**");
        return;
    }
    if (tag_is(name, "em") || tag_is(name, "i")) {
        sb_putc(&h->out, '_');
        return;
    }
    if (tag_is(name, "a")) {
        if (!closing) {
            free(h->href);
            h->href = attr_val(tag, n, "href");
            h->link_start = h->out.len;
            if (h->href) sb_putc(&h->out, '[');
        } else if (h->href) {
            if (h->out.len == h->link_start + 1) {
                sb_truncate(&h->out, h->link_start);
            } else if (!str_starts(h->href, "javascript:") && h->href[0] != '#') {
                char *abs = h->base ? url_resolve(h->base, h->href) : xstrdup(h->href);
                sb_appendf(&h->out, "](%s)", abs);
                free(abs);
            } else {
                sb_putc(&h->out, ']');
            }
            free(h->href);
            h->href = NULL;
        }
        return;
    }
    if (tag_is(name, "img") && !closing) {
        char *alt = attr_val(tag, n, "alt");
        if (alt && *alt) sb_appendf(&h->out, "![%s]", alt);
        free(alt);
    }
}

char *html_to_markdown(const char *html, size_t len, const char *base_url, char **title_out) {
    h2m_t h = {0};
    h.base = base_url;
    size_t i = 0;
    while (i < len) {
        char c = html[i];
        if (c == '<') {
            if (i + 3 < len && strncmp(html + i, "<!--", 4) == 0) {
                const char *e = memmem(html + i + 4, len - i - 4, "-->", 3);
                i = e ? (size_t)(e - html) + 3 : len;
                continue;
            }
            const char *e = memchr(html + i, '>', len - i);
            if (!e) break;
            handle_tag(&h, html + i + 1, (size_t)(e - html - i - 1));
            i = (size_t)(e - html) + 1;
            continue;
        }
        if (h.in_title) {
            if (c == '&') i += decode_entity(html + i, len - i, &h.title_buf);
            else {
                sb_putc(&h.title_buf, isspace((unsigned char)c) ? ' ' : c);
                i++;
            }
            continue;
        }
        if (h.skip) {
            i++;
            continue;
        }
        if (c == '&') {
            i += decode_entity(html + i, len - i, &h.out);
            continue;
        }
        if (!h.pre && isspace((unsigned char)c)) {
            if (h.out.len && !isspace((unsigned char)h.out.s[h.out.len - 1])) sb_putc(&h.out, ' ');
            i++;
            continue;
        }
        sb_putc(&h.out, c);
        i++;
    }
    free(h.href);
    /* collapse 3+ newlines and trim lines */
    strvec_t lines = str_split(sb_cstr(&h.out), "\n");
    sb_t r = SB_INIT;
    int blanks = 0;
    for (size_t k = 0; k < lines.n; k++) {
        char *t = lines.v[k];
        size_t l = strlen(t);
        while (l && (t[l - 1] == ' ' || t[l - 1] == '\t')) t[--l] = 0;
        if (!l) {
            if (++blanks > 1 || !r.len) continue;
        } else {
            blanks = 0;
        }
        sb_append(&r, t);
        sb_putc(&r, '\n');
    }
    strvec_free(&lines);
    sb_free(&h.out);
    if (title_out) {
        *title_out = h.title ? h.title : (h.title_buf.len ? sb_detach(&h.title_buf) : NULL);
        if (*title_out) {
            char *t = str_trim(*title_out);
            free(*title_out);
            *title_out = t;
        }
    } else {
        free(h.title);
    }
    sb_free(&h.title_buf);
    return sb_detach(&r);
}

/* ---------- read URL ---------- */

static const char *BROWSER_UA =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0 Safari/537.36";

int web_read_url(tool_env_t *env, const char *url_in, volatile sig_atomic_t *cancel, tool_result_t *out) {
    UNUSED(env);
    /* selector after the last '/' segment: https://x/y:raw or :10-20 */
    char *url = xstrdup(url_in);
    bool raw = false;
    long lo = 0, hi = 0;
    for (int g = 0; g < 2; g++) {
        char *colon = strrchr(url, ':');
        char *slash = strrchr(url, '/');
        if (!colon || !slash || colon < slash) break;
        const char *sel = colon + 1;
        if (strcmp(sel, "raw") == 0) raw = true;
        else if (isdigit((unsigned char)*sel)) {
            char *e;
            lo = strtol(sel, &e, 10);
            hi = 0;
            if (*e == '-') hi = strtol(e + 1, NULL, 10);
            else if (*e == '+') hi = lo + strtol(e + 1, NULL, 10) - 1;
        } else break;
        *colon = 0;
    }
    http_headers_t h = {0};
    http_headers_add(&h, "User-Agent", BROWSER_UA);
    http_headers_add(&h, "Accept", "text/html,application/xhtml+xml,application/json;q=0.9,text/plain;q=0.8,*/*;q=0.5");
    http_headers_add(&h, "Accept-Language", "en-US,en;q=0.9");
    http_req_t req = {.method = "GET", .url = url, .headers = &h, .timeout_ms = 30000, .max_redirects = 10,
                      .max_body = 20u << 20, .cancel = cancel};
    http_resp_t r;
    int rc = http_request(&req, &r);
    http_headers_free(&h);
    if (rc != 0) {
        tool_result_error(out, "Failed to fetch %s: %s", url, r.error ? r.error : "unknown error");
        http_resp_free(&r);
        free(url);
        return 0;
    }
    const char *ct = http_headers_get(&r.headers, "Content-Type");
    if (!ct) ct = "";
    json_t *details = json_obj();
    json_set_str(details, "kind", "url");
    json_set_str(details, "url", url);
    json_set_str(details, "finalUrl", r.final_url ? r.final_url : url);
    json_set_str(details, "contentType", ct);
    if (r.status >= 400) {
        tool_result_error(out, "HTTP %d fetching %s\n%.2000s", r.status, url, r.body ? r.body : "");
        out->details = details;
        http_resp_free(&r);
        free(url);
        return 0;
    }
    if (str_starts(ct, "image/")) {
        char *b64 = base64_encode(r.body, r.body_len);
        char *mime = xstrndup(ct, strcspn(ct, ";"));
        tool_result_textf(out, "Fetched image %s (%zu bytes)", url, r.body_len);
        tool_result_image(out, b64, mime);
        free(mime);
        free(b64);
        out->details = details;
        http_resp_free(&r);
        free(url);
        return 0;
    }
    char *text;
    char *title = NULL;
    bool is_html = str_istr(ct, "html") != NULL || (r.body_len > 15 && str_istr(r.body, "<html") != NULL);
    if (is_html && !raw) text = html_to_markdown(r.body, r.body_len, r.final_url ? r.final_url : url, &title);
    else if (fs_is_binary(r.body, r.body_len)) text = xasprintf("[Binary content (%s, %zu bytes)]", ct, r.body_len);
    else text = utf8_sanitize(r.body, r.body_len);
    sb_t b = SB_INIT;
    if (title && *title) sb_appendf(&b, "# %s\n", title);
    sb_appendf(&b, "Source: %s\n\n", r.final_url ? r.final_url : url);
    if (lo > 0) {
        strvec_t lines = str_split(text, "\n");
        long end = hi > 0 ? MIN(hi, (long)lines.n) : MIN((long)lines.n, lo + 299);
        for (long i = lo; i <= end; i++) sb_appendf(&b, "%s\n", lines.v[i - 1]);
        if (end < (long)lines.n) sb_appendf(&b, "\n[%ld more lines. Use :%ld to continue]", (long)lines.n - end, end + 1);
        strvec_free(&lines);
    } else {
        size_t n = strlen(text);
        size_t cap = 100000;
        if (n > cap) {
            size_t k = cap;
            while (k && ((unsigned char)text[k] & 0xC0) == 0x80) k--;
            sb_appendn(&b, text, k);
            size_t total_lines = str_count(text, "\n") + 1, shown = str_count(sb_cstr(&b), "\n");
            sb_appendf(&b, "\n\n[Content truncated; %zu lines total. Use :%zu to continue]", total_lines, shown);
        } else {
            sb_append(&b, text);
        }
    }
    tool_result_text(out, sb_cstr(&b));
    out->details = details;
    sb_free(&b);
    free(text);
    free(title);
    http_resp_free(&r);
    free(url);
    return 0;
}

/* ---------- web search ---------- */

typedef struct {
    char *title, *url, *snippet;
} sresult_t;
typedef VEC(sresult_t) sresults_t;

static void sres_free(sresults_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        free(v->v[i].title);
        free(v->v[i].url);
        free(v->v[i].snippet);
    }
    vec_free(v);
}

static void sres_add(sresults_t *v, const char *t, const char *u, const char *s) {
    sresult_t r = {xstrdup(t ? t : ""), xstrdup(u ? u : ""), xstrdup(s ? s : "")};
    vec_push(v, r);
}

static int search_brave(const char *q, int limit, const char *recency, sresults_t *out, char **err) {
    const char *key = env_get("BRAVE_API_KEY");
    if (!key) return -2;
    char *eq = url_encode(q);
    const char *fresh = !recency ? "" : !strcmp(recency, "day") ? "&freshness=pd" : !strcmp(recency, "week") ? "&freshness=pw"
                                      : !strcmp(recency, "month") ? "&freshness=pm" : "&freshness=py";
    char *url = xasprintf("https://api.search.brave.com/res/v1/web/search?q=%s&count=%d%s", eq, limit, fresh);
    free(eq);
    http_headers_t h = {0};
    http_headers_add(&h, "X-Subscription-Token", key);
    http_headers_add(&h, "Accept", "application/json");
    http_resp_t r;
    int rc = http_get(url, &h, ws_timeout_ms(), &r);
    http_headers_free(&h);
    free(url);
    if (rc != 0 || r.status != 200) {
        *err = xasprintf("brave: %s", rc ? r.error : r.body);
        http_resp_free(&r);
        return -1;
    }
    json_t *j = json_parse_str(r.body);
    json_t *arr = json_get_arr(json_get(j, "web"), "results"), *it;
    json_arr_foreach(arr, i, it) sres_add(out, json_get_str(it, "title"), json_get_str(it, "url"), json_get_str(it, "description"));
    json_free(j);
    http_resp_free(&r);
    return 0;
}

static int search_tavily(const char *q, int limit, const char *recency, sresults_t *out, char **err) {
    const char *key = env_get("TAVILY_API_KEY");
    if (!key) return -2;
    json_t *b = json_obj();
    json_set_str(b, "query", q);
    json_set_int(b, "max_results", limit);
    if (recency) json_set_str(b, "time_range", recency);
    char *body = json_dump(b, 0);
    json_free(b);
    http_headers_t h = {0};
    char *auth = xasprintf("Bearer %s", key);
    http_headers_add(&h, "Authorization", auth);
    free(auth);
    http_resp_t r;
    int rc = http_post_json("https://api.tavily.com/search", &h, body, ws_timeout_ms(), &r);
    free(body);
    http_headers_free(&h);
    if (rc != 0 || r.status != 200) {
        *err = xasprintf("tavily: %s", rc ? r.error : r.body);
        http_resp_free(&r);
        return -1;
    }
    json_t *j = json_parse_str(r.body);
    json_t *arr = json_get_arr(j, "results"), *it;
    json_arr_foreach(arr, i, it) sres_add(out, json_get_str(it, "title"), json_get_str(it, "url"), json_get_str(it, "content"));
    json_free(j);
    http_resp_free(&r);
    return 0;
}

static int search_exa(const char *q, int limit, const char *recency, sresults_t *out, char **err) {
    const char *key = env_get("EXA_API_KEY");
    if (!key) return -2;
    UNUSED(recency);
    json_t *b = json_obj();
    json_set_str(b, "query", q);
    json_set_int(b, "numResults", limit);
    json_t *contents = json_obj();
    json_set_bool(contents, "text", false);
    json_t *hl = json_obj();
    json_set_int(hl, "numSentences", 3);
    json_set(contents, "highlights", hl);
    json_set(b, "contents", contents);
    char *body = json_dump(b, 0);
    json_free(b);
    http_headers_t h = {0};
    http_headers_add(&h, "x-api-key", key);
    http_resp_t r;
    int rc = http_post_json("https://api.exa.ai/search", &h, body, ws_timeout_ms(), &r);
    free(body);
    http_headers_free(&h);
    if (rc != 0 || r.status != 200) {
        *err = xasprintf("exa: %s", rc ? r.error : r.body);
        http_resp_free(&r);
        return -1;
    }
    json_t *j = json_parse_str(r.body);
    json_t *arr = json_get_arr(j, "results"), *it;
    json_arr_foreach(arr, i, it) {
        json_t *hls = json_get_arr(it, "highlights");
        sb_t s = SB_INIT;
        json_t *x;
        json_arr_foreach(hls, k, x) {
            if (s.len) sb_putc(&s, ' ');
            sb_append(&s, json_sv(x));
        }
        sres_add(out, json_get_str(it, "title"), json_get_str(it, "url"), sb_cstr(&s));
        sb_free(&s);
    }
    json_free(j);
    http_resp_free(&r);
    return 0;
}

/* DuckDuckGo HTML frontend scraper (credential-free) */
static int search_ddg(const char *q, int limit, const char *recency, sresults_t *out, char **err) {
    char *eq = url_encode(q);
    const char *df = !recency ? "" : !strcmp(recency, "day") ? "&df=d" : !strcmp(recency, "week") ? "&df=w"
                                   : !strcmp(recency, "month") ? "&df=m" : "&df=y";
    char *url = xasprintf("https://html.duckduckgo.com/html/?q=%s%s", eq, df);
    free(eq);
    http_headers_t h = {0};
    http_headers_add(&h, "User-Agent", BROWSER_UA);
    http_headers_add(&h, "Accept", "text/html");
    http_resp_t r;
    int rc = http_get(url, &h, ws_timeout_ms(), &r);
    http_headers_free(&h);
    free(url);
    if (rc != 0 || r.status != 200) {
        *err = xasprintf("duckduckgo: %s", rc ? r.error : "HTTP error");
        http_resp_free(&r);
        return -1;
    }
    const char *p = r.body;
    while ((int)out->n < limit && (p = strstr(p, "class=\"result__a\""))) {
        const char *tag_start = p;
        while (tag_start > r.body && *tag_start != '<') tag_start--;
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        char *href = attr_val(tag_start + 1, (size_t)(tag_end - tag_start - 1), "href");
        const char *close = strstr(tag_end, "</a>");
        if (!close) {
            free(href);
            break;
        }
        char *title = html_to_markdown(tag_end + 1, (size_t)(close - tag_end - 1), NULL, NULL);
        char *snippet = NULL;
        const char *sn = strstr(close, "class=\"result__snippet\"");
        const char *next = strstr(close, "class=\"result__a\"");
        if (sn && (!next || sn < next)) {
            const char *se = strchr(sn, '>');
            const char *sc = se ? strstr(se, "</a>") : NULL;
            if (se && sc) snippet = html_to_markdown(se + 1, (size_t)(sc - se - 1), NULL, NULL);
        }
        /* unwrap //duckduckgo.com/l/?uddg=<encoded> */
        char *real = NULL;
        if (href) {
            const char *u = strstr(href, "uddg=");
            if (u) {
                char *enc = xstrndup(u + 5, strcspn(u + 5, "&"));
                real = url_decode(enc);
                free(enc);
            } else {
                real = xstrdup(href);
            }
        }
        char *tt = str_trim(title), *st = snippet ? str_trim(snippet) : xstrdup("");
        if (real && !strstr(real, "duckduckgo.com/y.js")) sres_add(out, tt, real, st);
        free(tt);
        free(st);
        free(title);
        free(snippet);
        free(real);
        free(href);
        p = close;
    }
    http_resp_free(&r);
    if (!out->n) {
        *err = xstrdup("duckduckgo: no results");
        return -1;
    }
    return 0;
}

typedef int (*search_fn)(const char *, int, const char *, sresults_t *, char **);
static const struct {
    const char *name;
    search_fn fn;
} PROVIDERS[] = {
    {"brave", search_brave},
    {"tavily", search_tavily},
    {"exa", search_exa},
    {"duckduckgo", search_ddg},
};

static int web_search_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    const char *q = json_get_str(args, "query");
    if (!q || !*q) {
        tool_result_error(out, "query is required");
        return 0;
    }
    /* `num_search_results` sizes the result set when `limit` is absent (codex/gemini semantics); max_tokens and
     * temperature only steer LLM-backed providers, which omc does not ship, so they are accepted and unused */
    int limit = (int)json_get_int(args, "limit", json_get_int(args, "num_search_results", 10));
    if (limit < 1) limit = 1;
    if (limit > 30) limit = 30;
    const char *recency = json_get_str(args, "recency");
    const char *pref = settings_str("providers.webSearch", NULL);
    sb_t errs = SB_INIT;
    sresults_t res = {0};
    const char *used = NULL;
    for (int pass = 0; pass < 2 && !used; pass++) {
        for (size_t i = 0; i < ARRAY_LEN(PROVIDERS) && !used; i++) {
            if (pass == 0 && (!pref || strcmp(pref, PROVIDERS[i].name) != 0)) continue;
            if (pass == 1 && pref && strcmp(pref, PROVIDERS[i].name) == 0) continue;
            if (ctx->cancel && *ctx->cancel) break;
            char *err = NULL;
            int rc = PROVIDERS[i].fn(q, limit, recency, &res, &err);
            if (rc == 0 && res.n) used = PROVIDERS[i].name;
            else if (err) {
                sb_appendf(&errs, "- %s\n", err);
                sres_free(&res);
            }
            free(err);
        }
    }
    if (!used) {
        tool_result_error(out, "Web search failed:\n%s", errs.len ? sb_cstr(&errs) : "no provider available");
        out->details = json_obj();
        json_t *resp = json_obj();
        json_set_str(resp, "provider", "none");
        json_set(resp, "sources", json_arr());
        json_set(out->details, "response", resp);
        char *em = tool_result_joined_text(out);
        json_set_str(out->details, "error", em);
        free(em);
        sb_free(&errs);
        return 0;
    }
    sb_t b = SB_INIT;
    sb_appendf(&b, "Search results for \"%s\" (%s):\n\n", q, used);
    json_t *sources = json_arr();
    for (size_t i = 0; i < res.n && (int)i < limit; i++) {
        sb_appendf(&b, "%zu. %s\n   %s\n", i + 1, res.v[i].title, res.v[i].url);
        if (*res.v[i].snippet) sb_appendf(&b, "   %s\n", res.v[i].snippet);
        sb_putc(&b, '\n');
        json_t *s = json_obj();
        json_set_str(s, "title", res.v[i].title);
        json_set_str(s, "url", res.v[i].url);
        json_set_str(s, "snippet", res.v[i].snippet);
        json_push(sources, s);
    }
    tool_result_text(out, sb_cstr(&b));
    out->details = json_obj(); /* omp SearchRenderDetails: {response: {provider, sources}} */
    json_t *resp = json_obj();
    json_set_str(resp, "provider", used);
    json_set(resp, "sources", sources);
    json_set(out->details, "response", resp);
    sb_free(&b);
    sb_free(&errs);
    sres_free(&res);
    return 0;
}

agent_tool_t *tool_web_search_new(tool_env_t *env) {
    json_t *ctx = json_obj();
    char *desc = tool_prompt("web-search", ctx);
    json_free(ctx);
    json_t *s = schema_lit("{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"recency\":{\"enum\":"
                           "[\"day\",\"week\",\"month\",\"year\"],\"type\":\"string\"},\"limit\":{\"type\":\"number\"},"
                           "\"max_tokens\":{\"type\":\"number\"},\"temperature\":{\"type\":\"number\"},"
                           "\"num_search_results\":{\"type\":\"number\"}},\"required\":[\"query\"],"
                           "\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("web_search", "Web Search", desc, s, web_search_execute);
    free(desc);
    t->ud = env;
    return t;
}
