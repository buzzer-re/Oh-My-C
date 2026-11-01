/* tc_web.c - web_search card (port of omp tools/web-search.ts). */
#include "ui/tools/tc.h"

#include "tui/ansi.h"
#include "tui/markdown.h"

static const char *const PROVIDER_LABELS[][2] = {
    {"parallel", "Parallel"},   {"perplexity", "Perplexity"}, {"gemini", "Gemini"},       {"anthropic", "Anthropic"},
    {"codex", "OpenAI Codex"},  {"openai", "OpenAI API"},     {"xai", "xAI"},             {"openrouter", "OpenRouter"},
    {"zai", "Z.AI"},            {"exa", "Exa"},               {"tinyfish", "TinyFish"},   {"jina", "Jina"},
    {"kagi", "Kagi"},           {"tavily", "Tavily"},         {"firecrawl", "Firecrawl"}, {"brave", "Brave"},
    {"deepseek", "DeepSeek"},   {"kimi", "Kimi"},             {"synthetic", "Synthetic"}, {"ollama", "Ollama"},
    {"searxng", "SearXNG"},     {"startpage", "Startpage"},   {"duckduckgo", "DuckDuckGo"}, {"ecosia", "Ecosia"},
    {"google", "Google"},       {"mojeek", "Mojeek"},         {"public", "Public Web"},
};

static const char *provider_label(const char *id) {
    for (size_t i = 0; i < ARRAY_LEN(PROVIDER_LABELS); i++)
        if (str_eq(id, PROVIDER_LABELS[i][0])) return PROVIDER_LABELS[i][1];
    return id ? id : "";
}

char *tc_url_domain(const char *url) {
    const char *p = strstr(url ? url : "", "://");
    if (!p) return xstrdup(url ? url : "");
    p += 3;
    const char *at = p;
    size_t n = strcspn(p, "/?#");
    const char *a = memchr(p, '@', n);
    if (a) {
        at = a + 1;
        n -= (size_t)(at - p);
    }
    char *host = xstrndup(at, n);
    char *colon = host[0] == '[' ? NULL : strrchr(host, ':');
    if (colon) *colon = 0;
    for (char *q = host; *q; q++)
        if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
    if (str_starts(host, "www.")) memmove(host, host + 4, strlen(host + 4) + 1);
    return host;
}

char *tc_hyperlink(const char *url, const char *text) {
    if (!url || !tui_md_hyperlinks_supported() || !(str_starts(url, "http://") || str_starts(url, "https://") ||
                                                    str_starts(url, "www.")))
        return xstrdup(text);
    return xasprintf("\x1b]8;;%s%s\a%s" TUI_LINK_CLOSE, str_starts(url, "www.") ? "https://" : "", url, text);
}

char *tc_format_age(int64_t secs) {
    if (secs <= 0) return xstrdup("");
    int64_t mins = secs / 60, hours = mins / 60, days = hours / 24, weeks = days / 7, months = days / 30;
    if (months > 0) return xasprintf("%lldmo ago", (long long)months);
    if (weeks > 0) return xasprintf("%lldw ago", (long long)weeks);
    if (days > 0) return xasprintf("%lldd ago", (long long)days);
    if (hours > 0) return xasprintf("%lldh ago", (long long)hours);
    if (mins > 0) return xasprintf("%lldm ago", (long long)mins);
    return xstrdup("just now");
}

/* renderFallbackText: `⚠ Response` + dim tree of the non-blank lines (6 collapsed) */
static void fallback_text(const char *text, bool expanded, int width, strvec_t *out) {
    strvec_t all = {0}, lines = {0};
    tc_split_lines(text, &all);
    for (size_t i = 0; i < all.n; i++) {
        char *t = str_trim(all.v[i]);
        if (*t) strvec_push_own(&lines, t);
        else free(t);
    }
    size_t shown = expanded ? lines.n : MIN(lines.n, 6);
    size_t remaining = lines.n - shown;
    char *icon = tc_status_icon(TC_ST_WARNING, -1);
    char *resp = tc_fg(TUI_TC_DIM, "Response");
    char *hint = !expanded && remaining > 0 ? tc_expand_hint() : xstrdup("");
    sb_t b = SB_INIT;
    sb_appendf(&b, "%s %s%s", icon, resp, hint);
    if (!shown) {
        char *br = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
        char *m = tc_fg(TUI_TC_MUTED, "No response data");
        sb_appendf(&b, "\n %s %s", br, m);
        free(m);
        free(br);
    }
    for (size_t i = 0; i < shown; i++) {
        bool last = i + 1 == shown && remaining == 0;
        char *br = tc_fg(TUI_TC_DIM, tc_sym(last ? "tree.last" : "tree.branch"));
        char *clip = expanded ? xstrdup(lines.v[i]) : tui_truncate_to_width(lines.v[i], 110, NULL);
        char *l = tc_fg(TUI_TC_DIM, clip);
        sb_appendf(&b, "\n %s %s", br, l);
        free(l);
        free(clip);
        free(br);
    }
    if (!expanded && remaining > 0) {
        char *br = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
        char *mi = tc_more_items((int64_t)remaining, "line");
        char *m = tc_fg(TUI_TC_MUTED, mi);
        sb_appendf(&b, "\n %s %s", br, m);
        free(m);
        free(mi);
        free(br);
    }
    tc_push_wrapped(out, sb_cstr(&b), width);
    sb_free(&b);
    free(hint);
    free(resp);
    free(icon);
    strvec_free(&lines);
    strvec_free(&all);
}

static void ws_result(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    const char *err = json_get_str(d, "error");
    const json_t *resp = json_get_obj(d, "response");
    if (err) {
        const char *pv = json_get_str(resp, "provider");
        tc_status_line_t sl = {.icon = TC_ST_ERROR, .spinner = -1, .title = "Web Search", .title_slot = -1,
                               .description = pv && !str_eq(pv, "none") ? provider_label(pv) : NULL};
        char *h = tc_status_line(&sl);
        char *t = tc_replace_tabs(err);
        char *e = xasprintf("Error: %s", t);
        tc_section_t sec = {0};
        strvec_push_own(&sec.lines, tc_fg(TUI_TC_ERROR, e));
        tc_block_t blk = {.header = h, .state = TC_STATE_ERROR, .sections = &sec, .nsections = 1, .width = width,
                          .pad_left = -1, .pad_right = -1, .border_slot = -1};
        tc_output_block(&blk, out);
        strvec_free(&sec.lines);
        free(e);
        free(t);
        free(h);
        return;
    }
    char *raw0 = tc_text_of(c->content);
    char *raw = str_trim(raw0);
    free(raw0);
    if (!resp) {
        fallback_text(raw, c->expanded, width, out);
        free(raw);
        return;
    }
    const json_t *sources = json_get_arr(resp, "sources");
    size_t nsrc = json_len(sources);
    const char *provider = json_get_str(resp, "provider");
    const char *answer = json_get_str(resp, "answer");
    char *ans = answer ? str_trim(answer) : xstrdup("");
    const char *content_text = *ans ? ans : raw;
    const char *plabel = provider && !str_eq(provider, "none") ? provider_label(provider) : "None";
    const char *q = json_get_str(c->args, "query");
    if (!q || !*q) {
        const json_t *sq = json_get_arr(resp, "searchQueries");
        json_t *e;
        q = NULL;
        json_arr_foreach(sq, i, e) if (!q && json_is_str(e)) q = json_sv(e);
    }
    char *qprev = q && *q ? tui_truncate_to_width(q, 80, NULL) : NULL;
    char *cnt = tc_format_count("source", (int64_t)nsrc);
    const char *meta[] = {cnt};
    char *ov = nsrc ? tc_styled_sym("tool.webSearch", TUI_TC_ACCENT) : NULL;
    tc_status_line_t sl = {.icon = TC_ST_WARNING, .icon_override = ov, .spinner = -1, .title = "Web Search", .title_slot = -1,
                           .description = plabel, .meta = meta, .nmeta = 1};
    char *header = tc_status_line(&sl);

    const char *auth = json_get_str(resp, "authMode");
    const char *auth_short = str_eq(auth, "oauth") ? "OAuth" : str_eq(auth, "api_key") ? "API" : auth;
    sb_t pi = SB_INIT;
    if (json_get_str(resp, "model")) sb_appendf(&pi, "%s @ %s", json_get_str(resp, "model"), plabel);
    else sb_append(&pi, plabel);
    if (auth_short && *auth_short) sb_appendf(&pi, " (%s)", auth_short);

    int cw = tc_block_content_width(width, -1, -1);
    tc_section_t secs[4] = {0};
    char *labels[3] = {tc_fg(TUI_TC_TOOL_TITLE, "Answer"), tc_fg(TUI_TC_TOOL_TITLE, "Sources"), tc_fg(TUI_TC_TOOL_TITLE, "Metadata")};
    size_t ns = 0;
    if (qprev) {
        char *a = tc_fg(TUI_TC_MUTED, "Query:"), *b = tc_fg(TUI_TC_TEXT, qprev);
        strvec_push_own(&secs[ns].lines, xasprintf("%s %s", a, b));
        free(a);
        free(b);
        ns++;
    }
    /* answer */
    secs[ns].label = labels[0];
    if (*content_text) {
        tui_md_opts_t mo;
        tui_md_opts_default(&mo);
        tui_md_render(content_text, cw, &mo, &secs[ns].lines);
    }
    if (!secs[ns].lines.n) strvec_push_own(&secs[ns].lines, tc_fg(TUI_TC_MUTED, "No answer text returned"));
    ns++;
    /* sources */
    secs[ns].label = labels[1];
    {
        strvec_t items = {0};
        json_t *src;
        json_arr_foreach(sources, i, src) {
            const char *t = json_get_str(src, "title"), *url = json_get_str(src, "url");
            char *tt = t ? str_trim(t) : xstrdup(""), *ut = url ? str_trim(url) : xstrdup("");
            const char *title = *tt ? t : *ut ? url : "Untitled";
            char *domain = url && *url ? tc_url_domain(url) : xstrdup("");
            char *age = tc_format_age(json_get_int(src, "ageSeconds", 0));
            if (!*age && json_get_str(src, "publishedDate")) {
                free(age);
                age = xstrdup(json_get_str(src, "publishedDate"));
            }
            sb_t ms = SB_INIT;
            if (*domain) {
                char *dd = xasprintf("(%s)", domain);
                char *x = tc_fg(TUI_TC_DIM, dd);
                sb_appendf(&ms, " %s", x);
                free(x);
                free(dd);
            }
            if (*age) {
                if (ms.len) {
                    char *sep = tc_fg(TUI_TC_DIM, tc_sym("sep.dot"));
                    sb_append(&ms, sep);
                    free(sep);
                } else sb_putc(&ms, ' ');
                char *x = tc_fg(TUI_TC_MUTED, age);
                sb_append(&ms, x);
                free(x);
            }
            int line_budget = MAX(24, width - 6);
            int title_budget = MAX(12, line_budget - tui_visible_width(sb_cstr(&ms)));
            char *tr = tui_truncate_to_width(title, title_budget, NULL);
            char *ta = tc_fg(TUI_TC_ACCENT, tr);
            char *linked = url && *url ? tc_hyperlink(url, ta) : xstrdup(ta);
            strvec_push_own(&items, xasprintf("%s%s", linked, sb_cstr(&ms)));
            free(linked);
            free(ta);
            free(tr);
            sb_free(&ms);
            free(age);
            free(domain);
            free(tt);
            free(ut);
        }
        if (items.n) tc_tree_list(&items, c->expanded, 8, "source", &secs[ns].lines);
        else strvec_push_own(&secs[ns].lines, tc_fg(TUI_TC_MUTED, "No sources returned"));
        strvec_free(&items);
    }
    ns++;
    /* metadata */
    secs[ns].label = labels[2];
    {
        char *a = tc_fg(TUI_TC_MUTED, "Provider:"), *b = tc_fg(TUI_TC_TEXT, sb_cstr(&pi));
        strvec_push_own(&secs[ns].lines, xasprintf("%s %s", a, b));
        free(a);
        free(b);
        const json_t *u = json_get_obj(resp, "usage");
        if (u) {
            strvec_t parts = {0};
            static const char *const UK[][2] = {{"inputTokens", "in"}, {"outputTokens", "out"}, {"totalTokens", "total"},
                                                {"searchRequests", "search"}};
            for (size_t k = 0; k < 4; k++)
                if (json_get(u, UK[k][0])) {
                    char *nv = json_dump(json_get(u, UK[k][0]), 0);
                    strvec_push_own(&parts, xasprintf("%s %s", UK[k][1], nv));
                    free(nv);
                }
            if (parts.n) {
                char *j = strvec_join(&parts, tc_sym("sep.dot"));
                char *ua = tc_fg(TUI_TC_MUTED, "Usage:"), *ub = tc_fg(TUI_TC_TEXT, j);
                strvec_push_own(&secs[ns].lines, xasprintf("%s %s", ua, ub));
                free(ua);
                free(ub);
                free(j);
            }
            strvec_free(&parts);
        }
    }
    ns++;
    tc_block_t blk = {.header = header, .state = nsrc ? TC_STATE_SUCCESS : TC_STATE_WARNING, .sections = secs, .nsections = ns,
                      .width = width, .pad_left = -1, .pad_right = -1, .border_slot = -1};
    tc_output_block(&blk, out);
    for (size_t i = 0; i < ns; i++) strvec_free(&secs[i].lines);
    for (int i = 0; i < 3; i++) free(labels[i]);
    sb_free(&pi);
    free(header);
    free(ov);
    free(cnt);
    free(qprev);
    free(ans);
    free(raw);
}

void tc_web_search(const tc_call_t *c, int width, strvec_t *out) {
    if (c->has_result) {
        ws_result(c, width, out);
        return;
    }
    char *q = tui_truncate_to_width(json_get_str(c->args, "query") ? json_get_str(c->args, "query") : "", 80, NULL);
    tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "Web Search", .title_slot = -1, .description = q};
    char *h = tc_status_line(&sl);
    tc_push_wrapped(out, h, width);
    free(h);
    free(q);
}
