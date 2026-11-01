/* web.c - URL reading (HTML -> markdown) and the web_search tool with a provider chain. */
#include "web.h"

#include "config/settings.h"
#include "core/fs.h"
#include "net/http.h"

#include <ctype.h>
#include <math.h>

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
    char *title, *url, *snippet; /* snippet "" = none */
    char *date;                  /* publishedDate or NULL */
} sresult_t;
typedef VEC(sresult_t) sresults_t;

static void sres_free(sresults_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        free(v->v[i].title);
        free(v->v[i].url);
        free(v->v[i].snippet);
        free(v->v[i].date);
    }
    vec_free(v);
}

static void sres_add(sresults_t *v, const char *t, const char *u, const char *s) {
    sresult_t r = {xstrdup(t ? t : ""), xstrdup(u ? u : ""), xstrdup(s ? s : ""), NULL};
    vec_push(v, r);
}


/* ---------- Parallel (omp providers/parallel.ts): keyless public MCP, or the REST API with PARALLEL_API_KEY ---------- */

static const int RECENCY_DAYS_N[4] = {1, 7, 30, 365};
static const char *const RECENCY_NAMES[4] = {"day", "week", "month", "year"};

/* recencyAfterDate: UTC YYYY-MM-DD */
static char *recency_after_date(const char *recency) {
    for (int i = 0; i < 4; i++)
        if (str_eq(recency, RECENCY_NAMES[i])) {
            time_t t = time(NULL) - (time_t)RECENCY_DAYS_N[i] * 86400;
            struct tm tm;
            gmtime_r(&t, &tm);
            char buf[16];
            strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
            return xstrdup(buf);
        }
    return NULL;
}

/* the JSON-RPC response of an MCP POST answered as JSON or as an SSE stream */
static json_t *mcp_response_json(const char *body, const char *ctype) {
    if (!ctype || !strstr(ctype, "text/event-stream")) return json_parse_str(body);
    json_t *last = NULL;
    for (const char *p = body; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n > 5 && !strncmp(p, "data:", 5)) {
            char *d = xstrndup(p + 5, n - 5);
            json_t *j = json_parse_str(d);
            free(d);
            if (json_get(j, "result") || json_get(j, "error")) {
                json_free(last);
                last = j;
            } else json_free(j);
        }
        p = nl ? nl + 1 : NULL;
    }
    return last;
}

static void parallel_sources(const json_t *payload, int limit, sresults_t *out) {
    json_t *it;
    json_arr_foreach(json_get_arr(payload, "results"), i, it) {
        const char *url = json_get_str(it, "url");
        if (!url || !*url || (int)out->n >= limit) continue;
        sb_t sn = SB_INIT;
        json_t *ex;
        size_t k = 0;
        json_arr_foreach(json_get_arr(it, "excerpts"), j, ex) {
            if (!json_is_str(ex)) continue;
            if (k++) sb_append(&sn, "\n\n");
            sb_append(&sn, json_sv(ex));
        }
        const char *title = json_get_str(it, "title");
        sres_add(out, title && *title ? title : url, url, sb_cstr(&sn));
        const char *pd = json_get_str(it, "publish_date");
        if (pd && *pd) out->v[out->n - 1].date = xstrdup(pd);
        sb_free(&sn);
    }
}

static int search_parallel(const char *q, int limit, const char *recency, sresults_t *out, char **err) {
    const char *key = env_get("PARALLEL_API_KEY");
    char *after = recency ? recency_after_date(recency) : NULL;
    http_headers_t h = {0};
    char *ua = xasprintf("omc/%s", OMC_VERSION);
    http_headers_add(&h, "User-Agent", ua);
    free(ua);
    json_t *b = json_obj(), *queries = json_arr();
    const char *url;
    if (key && *key) {
        url = "https://api.parallel.ai/v1beta/search";
        http_headers_add(&h, "Accept", "application/json");
        http_headers_add(&h, "x-api-key", key);
        http_headers_add(&h, "parallel-beta", "search-extract-2025-10-10");
        json_set_str(b, "objective", q);
        json_push_str(queries, q);
        json_set(b, "search_queries", queries);
        json_set_str(b, "mode", "fast");
        json_t *ex = json_obj();
        json_set_int(ex, "max_chars_per_result", 10000);
        json_set(b, "excerpts", ex);
        if (after) {
            json_t *sp = json_obj();
            json_set_str(sp, "after_date", after);
            json_set(b, "source_policy", sp);
        }
    } else {
        url = "https://search.parallel.ai/mcp";
        http_headers_add(&h, "Accept", "application/json, text/event-stream");
        char *mq = after ? xasprintf("%s after:%s", q, after) : xstrdup(q);
        json_t *args = json_obj();
        json_set_str(args, "objective", q);
        json_push_str(queries, mq);
        json_set(args, "search_queries", queries);
        json_set_str(args, "model_name", "parallel");
        free(mq);
        json_t *params = json_obj();
        json_set_str(params, "name", "web_search");
        json_set(params, "arguments", args);
        char *id = rand_hex(5);
        json_set_str(b, "jsonrpc", "2.0");
        json_set_str(b, "id", id);
        json_set_str(b, "method", "tools/call");
        json_set(b, "params", params);
        free(id);
    }
    free(after);
    char *body = json_dump(b, 0);
    json_free(b);
    http_resp_t r;
    int rc = http_post_json(url, &h, body, ws_timeout_ms(), &r);
    free(body);
    http_headers_free(&h);
    if (rc != 0) {
        *err = xasprintf("%s", r.error ? r.error : "request failed");
        http_resp_free(&r);
        return -1;
    }
    if (r.status < 200 || r.status >= 300) {
        if (key && *key) *err = xasprintf("Parallel API error (%d): %s", r.status, r.body ? r.body : "");
        else if (r.status == 429) *err = xstrdup("parallel: MCP rate limit reached (429); configure a Parallel API key for higher limits");
        else *err = xasprintf("Parallel MCP request failed (%d): %s", r.status, r.body ? r.body : "");
        http_resp_free(&r);
        return -1;
    }
    json_t *j = key && *key ? json_parse_str(r.body) : mcp_response_json(r.body, http_headers_get(&r.headers, "Content-Type"));
    http_resp_free(&r);
    if (!j) {
        *err = xstrdup(key && *key ? "Parallel search returned an invalid response payload." : "Failed to parse Parallel MCP response");
        return -1;
    }
    int ret = 0;
    if (key && *key) {
        parallel_sources(j, limit, out);
    } else if (json_get(j, "error")) {
        *err = xasprintf("Parallel MCP error: %s", json_get_str(json_get(j, "error"), "message") ? json_get_str(json_get(j, "error"), "message") : "");
        ret = -1;
    } else {
        json_t *res = json_get_obj(j, "result");
        if (json_get_bool(res, "isError", false)) {
            const char *msg = NULL;
            json_t *c;
            json_arr_foreach(json_get_arr(res, "content"), i, c) if (!msg && str_eq(json_get_str(c, "type"), "text")) msg = json_get_str(c, "text");
            char *t = msg ? str_trim(msg) : xstrdup("");
            *err = *t ? t : (free(t), xstrdup("Parallel MCP returned an error"));
            ret = -1;
        } else if (json_get(res, "structuredContent")) {
            parallel_sources(json_get(res, "structuredContent"), limit, out);
        } else if (res) {
            json_t *c;
            json_arr_foreach(json_get_arr(res, "content"), i, c) {
                if (!str_eq(json_get_str(c, "type"), "text") || !json_get_str(c, "text")) continue;
                json_t *pl = json_parse_str(json_get_str(c, "text"));
                if (!pl) continue;
                parallel_sources(pl, limit, out);
                json_free(pl);
                break;
            }
        } else {
            *err = xstrdup("Parallel MCP search returned an unexpected response shape.");
            ret = -1;
        }
    }
    json_free(j);
    return ret;
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
    int def_n, max_n; /* clampNumResults bounds */
    const char *label;
} PROVIDERS[] = {
    {"parallel", search_parallel, 10, 40, "Parallel"},
    {"brave", search_brave, 10, 20, "Brave"},
    {"tavily", search_tavily, 5, 20, "Tavily"},
    {"exa", search_exa, 10, 25, "Exa"},
    {"duckduckgo", search_ddg, 10, 30, "DuckDuckGo"},
};

/* /\s+/g -> " ", trimmed */
static char *collapse_spaces(const char *s) {
    sb_t b = SB_INIT;
    bool sp = false;
    for (; *s; s++) {
        if (isspace((unsigned char)*s)) sp = true;
        else {
            if (sp && b.len) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *s);
        }
    }
    return sb_detach(&b);
}

/* omp formatAge */
static char *format_age(int64_t age) {
    if (!age) return xstrdup("");
    int64_t mins = age / 60, hours = mins / 60, days = hours / 24, weeks = days / 7, months = days / 30;
    if (age < 0) { /* Math.floor of negatives: never > 0 */
        return xstrdup("just now");
    }
    if (months > 0) return xasprintf("%lldmo ago", (long long)months);
    if (weeks > 0) return xasprintf("%lldw ago", (long long)weeks);
    if (days > 0) return xasprintf("%lldd ago", (long long)days);
    if (hours > 0) return xasprintf("%lldh ago", (long long)hours);
    if (mins > 0) return xasprintf("%lldm ago", (long long)mins);
    return xstrdup("just now");
}

/* JS `new Date(s)` for ISO dates (date-only = UTC, date-time without zone = local); false when unparseable */
static bool parse_js_date(const char *s, int64_t *ms_out) {
    int Y, M, D, h = 0, m = 0, sec = 0, n = 0;
    if (sscanf(s, "%4d-%2d-%2d%n", &Y, &M, &D, &n) != 3 || n != 10) return false;
    const char *p = s + 10;
    bool date_only = *p == 0, utc = date_only;
    int off = 0;
    double frac = 0;
    if (!date_only) {
        if (*p != 'T' && *p != ' ') return false;
        int k = 0;
        if (sscanf(p + 1, "%2d:%2d%n", &h, &m, &k) != 2) return false;
        p += 1 + k;
        if (*p == ':') {
            if (sscanf(p + 1, "%2d%n", &sec, &k) != 1) return false;
            p += 1 + k;
            if (*p == '.') {
                char *e;
                frac = strtod(p, &e);
                p = e;
            }
        }
        if (*p == 'Z') utc = true, p++;
        else if (*p == '+' || *p == '-') {
            int oh, om;
            if (sscanf(p + 1, "%2d:%2d", &oh, &om) != 2) return false;
            off = (*p == '-' ? -1 : 1) * (oh * 60 + om);
            utc = true;
            p += 6;
        }
        if (*p) return false;
    }
    struct tm tm = {.tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D, .tm_hour = h, .tm_min = m, .tm_sec = sec, .tm_isdst = -1};
    time_t t = utc ? timegm(&tm) : mktime(&tm);
    if (t == (time_t)-1 && !(Y == 1969 && M == 12 && D == 31)) return false;
    *ms_out = (int64_t)t * 1000 - (int64_t)off * 60000 + (int64_t)(frac * 1000);
    return true;
}

/* omp truncate(str, 240): UTF-16 length, `…` */
static char *truncate_utf16(const char *s, size_t max) {
    size_t units = 0, len = strlen(s);
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        size_t k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += k == 4 ? 2 : 1;
        i += k;
    }
    if (units <= max) return xstrdup(s);
    size_t keep = max - 1, i = 0;
    units = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        size_t u = k == 4 ? 2 : 1;
        if (units + u > keep) break;
        units += u;
        i += k;
    }
    return xasprintf("%.*s\xe2\x80\xa6", (int)i, s);
}

static const char *provider_label(const char *name) {
    for (size_t i = 0; i < ARRAY_LEN(PROVIDERS); i++)
        if (!strcmp(PROVIDERS[i].name, name)) return PROVIDERS[i].label;
    return name;
}

static int web_search_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    const char *q = json_get_str(args, "query");
    /* `num_search_results` (else `limit`) sizes the result set per provider (clampNumResults); max_tokens and
     * temperature only steer LLM-backed providers, which omc does not ship, so they are accepted and unused */
    double want = json_get_num(args, "num_search_results", json_get_num(args, "limit", 0));
    const char *recency = json_get_str(args, "recency");
    const char *pref = settings_str("providers.webSearch", NULL);
    if (pref && (!*pref || !strcmp(pref, "auto"))) pref = NULL;
    strvec_t fail_ids = {0}, fail_msgs = {0};
    sresults_t res = {0};
    const char *used = NULL;
    for (int pass = 0; pass < 2 && !used; pass++) {
        for (size_t i = 0; i < ARRAY_LEN(PROVIDERS) && !used; i++) {
            if (pass == 0 && (!pref || strcmp(pref, PROVIDERS[i].name) != 0)) continue;
            if (pass == 1 && pref && strcmp(pref, PROVIDERS[i].name) == 0) continue;
            if (ctx->cancel && *ctx->cancel) break;
            int n = !want || isnan(want) ? PROVIDERS[i].def_n : (int)MIN((double)PROVIDERS[i].max_n, MAX(1.0, want));
            char *err = NULL;
            int rc = PROVIDERS[i].fn(q, n, recency, &res, &err);
            if (rc == 0 && res.n) used = PROVIDERS[i].name;
            else if (rc != -2) {
                strvec_push(&fail_ids, PROVIDERS[i].name);
                strvec_push_own(&fail_msgs, err ? err : xasprintf("%s returned no renderable search content.", PROVIDERS[i].name));
                err = NULL;
                sres_free(&res);
            }
            free(err);
        }
    }
    if (!used) {
        char *msg;
        if (!fail_ids.n) msg = xstrdup("No web search model configured.");
        else if (fail_ids.n == 1) msg = xstrdup(fail_msgs.v[0]);
        else {
            sb_t b = SB_INIT;
            sb_append(&b, "All web search providers failed: ");
            for (size_t i = 0; i < fail_ids.n; i++) sb_appendf(&b, "%s%s: %s", i ? "; " : "", fail_ids.v[i], fail_msgs.v[i]);
            msg = sb_detach(&b);
        }
        tool_result_textf(out, "Error: %s", msg);
        out->details = json_obj();
        json_t *resp = json_obj();
        json_set_str(resp, "provider", fail_ids.n ? fail_ids.v[fail_ids.n - 1] : "none");
        json_set(resp, "sources", json_arr());
        json_set(out->details, "response", resp);
        json_set_str(out->details, "error", msg);
        free(msg);
        strvec_free(&fail_ids);
        strvec_free(&fail_msgs);
        return 0;
    }
    /* formatForLLM: fallback note, then `[N] title (age)\n    url\n    snippet` */
    sb_t b = SB_INIT;
    if (fail_ids.n) {
        sb_append(&b, "Note: answered by ");
        sb_append(&b, provider_label(used));
        sb_append(&b, " after ");
        for (size_t i = 0; i < fail_ids.n; i++) {
            char *reason = collapse_spaces(fail_msgs.v[i]);
            size_t rl = strlen(reason);
            if (rl && reason[rl - 1] == '.') reason[rl - 1] = 0;
            char *tr = truncate_utf16(reason, 240);
            sb_appendf(&b, "%s%s failed (%s)", i ? " and " : "", provider_label(fail_ids.v[i]), tr);
            free(tr);
            free(reason);
        }
    }
    json_t *sources = json_arr();
    int64_t now = (int64_t)time(NULL) * 1000;
    for (size_t i = 0; i < res.n; i++) {
        const sresult_t *r = &res.v[i];
        int64_t ms;
        bool has_age = r->date && parse_js_date(r->date, &ms);
        int64_t age = has_age ? (now - ms) / 1000 - ((now - ms) % 1000 < 0 ? 1 : 0) : 0;
        char *ag = has_age ? format_age(age) : xstrdup("");
        const char *shown = *ag ? ag : r->date;
        if (b.len) sb_putc(&b, '\n');
        sb_appendf(&b, "[%zu] %s", i + 1, r->title);
        if (shown && *shown) sb_appendf(&b, " (%s)", shown);
        sb_appendf(&b, "\n    %s", r->url);
        if (*r->snippet) {
            char *t = truncate_utf16(r->snippet, 240);
            sb_appendf(&b, "\n    %s", t);
            free(t);
        }
        free(ag);
        json_t *s = json_obj();
        json_set_str(s, "title", r->title);
        json_set_str(s, "url", r->url);
        if (*r->snippet) json_set_str(s, "snippet", r->snippet);
        if (r->date) json_set_str(s, "publishedDate", r->date);
        if (has_age) json_set_int(s, "ageSeconds", age);
        json_push(sources, s);
    }
    tool_result_text(out, sb_cstr(&b));
    out->details = json_obj(); /* omp SearchRenderDetails: {response: {provider, sources}} */
    json_t *resp = json_obj();
    json_set_str(resp, "provider", used);
    json_set(resp, "sources", sources);
    json_set(out->details, "response", resp);
    sb_free(&b);
    strvec_free(&fail_ids);
    strvec_free(&fail_msgs);
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
