/* sub_models.c - `omc models [ls|find|refresh|<provider>] [pattern]` (port of omp cli/models-cli.ts): one box
 * table per provider of the available models of a catalog kind, or JSON. */
#include "cli/cli_int.h"

#include "ai/ai.h"
#include "config/auth.h"
#include "config/model_resolver.h"
#include "config/settings.h"

#include <unistd.h>

/* pi-utils formatNumber */
static char *format_number(double n) {
    if (n < 1000) return xasprintf("%.0f", n);
    char buf[32];
    const char *suf;
    double v;
    if (n < 1e6) suf = "K", v = n / 1e3;
    else if (n < 1e9) suf = "M", v = n / 1e6;
    else suf = "B", v = n / 1e9;
    bool small = (n < 1e4) || (n >= 1e6 && n < 1e7) || (n >= 1e9 && n < 1e10);
    if (small) {
        snprintf(buf, sizeof buf, "%.1f", v);
        size_t l = strlen(buf);
        if (l > 2 && !strcmp(buf + l - 2, ".0")) buf[l - 2] = 0;
    } else snprintf(buf, sizeof buf, "%.0f", v);
    return xasprintf("%s%s", buf, suf);
}

static char *format_limit(int64_t n) { return n <= 0 ? xstrdup("-") : format_number((double)n); }

static int by_provider_then_id(const void *x, const void *y) {
    const ai_model_t *a = *(const ai_model_t *const *)x, *b = *(const ai_model_t *const *)y;
    int c = strcasecmp(a->provider, b->provider);
    if (!c) c = strcmp(a->provider, b->provider);
    if (c) return c;
    c = strcasecmp(a->id, b->id);
    return c ? c : strcmp(a->id, b->id);
}

/* getSupportedEfforts: the model's thinking efforts (comma list), or NULL when it does not think */
static json_t *supported_efforts(const ai_model_t *m) {
    json_t *e = json_get_arr(m->thinking, "efforts");
    if (json_len(e)) return json_clone(e);
    if (!m->reasoning) return NULL;
    json_t *def = json_arr();
    static const char *const D[] = {"minimal", "low", "medium", "high", "xhigh"};
    for (size_t i = 0; i < ARRAY_LEN(D); i++) json_push_str(def, D[i]);
    return def;
}

/* sendsImageInputOnWire */
static bool images_on_wire(const ai_model_t *m) {
    if (!m->input_image) return false;
    json_t *v = model_compat_get(m, "stripImageInput");
    return !(json_is_bool(v) && v->u.b);
}

static json_t *model_json(const ai_model_t *m) {
    json_t *o = json_obj();
    json_set_str(o, "provider", m->provider);
    json_set_str(o, "kind", catalog_model_kind(m));
    json_set_str(o, "id", m->id);
    char *sel = xasprintf("%s/%s", m->provider, m->id);
    json_set_str(o, "selector", sel);
    free(sel);
    json_set_str(o, "name", m->name ? m->name : m->id);
    if (m->context_window > 0) json_set_num(o, "contextWindow", (double)m->context_window);
    else json_set(o, "contextWindow", json_null());
    if (m->max_tokens > 0) json_set_num(o, "maxTokens", (double)m->max_tokens);
    else json_set(o, "maxTokens", json_null());
    json_set_bool(o, "reasoning", m->reasoning);
    json_t *eff = supported_efforts(m);
    json_set(o, "thinking", eff ? eff : json_null());
    json_t *in = json_arr();
    json_push_str(in, "text");
    if (m->input_image) json_push_str(in, "image");
    json_set(o, "input", in);
    json_t *cost = json_obj();
    json_set_num(cost, "input", m->cost_input);
    json_set_num(cost, "output", m->cost_output);
    json_set_num(cost, "cacheRead", m->cost_cache_read);
    json_set_num(cost, "cacheWrite", m->cost_cache_write);
    json_set(o, "cost", cost);
    bool fixed = m->cost_input || m->cost_output || m->cost_cache_read || m->cost_cache_write;
    const char *ps = json_get_str(m->raw, "pricingStatus");
    json_set_str(o, "pricingStatus", fixed ? "fixed" : ps ? ps : "unknown");
    return o;
}

static void box_table(const char *const *headers, const bool *right, size_t ncol, const strvec_t *rows, size_t nrows) {
    size_t w[8] = {0};
    for (size_t c = 0; c < ncol; c++) {
        w[c] = str_display_width(headers[c]);
        for (size_t r = 0; r < nrows; r++) w[c] = MAX(w[c], str_display_width(rows[r].v[c]));
    }
    char *bar = cli_dim("│");
    sb_t seg = SB_INIT;
    const char *joins[3][3] = {{"┌", "┬", "┐"}, {"├", "┼", "┤"}, {"└", "┴", "┘"}};
    char *rules[3];
    for (int k = 0; k < 3; k++) {
        sb_clear(&seg);
        sb_append(&seg, joins[k][0]);
        for (size_t c = 0; c < ncol; c++) {
            if (c) sb_append(&seg, joins[k][1]);
            sb_repeat(&seg, "─", (int)w[c] + 2);
        }
        sb_append(&seg, joins[k][2]);
        rules[k] = cli_dim(sb_cstr(&seg));
    }
    sb_free(&seg);
    printf("%s\n", rules[0]);
    for (size_t r = 0; r <= nrows; r++) {
        sb_t line = SB_INIT;
        sb_appendf(&line, "%s ", bar);
        for (size_t c = 0; c < ncol; c++) {
            const char *cell = r == 0 ? headers[c] : rows[r - 1].v[c];
            sb_t pc = SB_INIT;
            size_t pad = w[c] - str_display_width(cell);
            if (right[c]) {
                for (size_t k = 0; k < pad; k++) sb_putc(&pc, ' ');
                sb_append(&pc, cell);
            } else {
                sb_append(&pc, cell);
                for (size_t k = 0; k < pad; k++) sb_putc(&pc, ' ');
            }
            if (c) sb_appendf(&line, " %s ", bar);
            if (r == 0) {
                char *b = cli_bold(sb_cstr(&pc));
                sb_append(&line, b);
                free(b);
            } else sb_append(&line, sb_cstr(&pc));
            sb_free(&pc);
        }
        sb_appendf(&line, " %s", bar);
        printf("%s\n", sb_cstr(&line));
        sb_free(&line);
        if (r == 0) printf("%s\n", rules[1]);
    }
    printf("%s\n", rules[2]);
    for (int k = 0; k < 3; k++) free(rules[k]);
    free(bar);
}

int cmd_models(cparse_t *p) {
    const char *first = cp_arg(p, "action"), *second = cp_arg(p, "pattern");
    const char *action = "ls", *pattern = first;
    if (first && (!strcmp(first, "ls") || !strcmp(first, "list") || !strcmp(first, "find") || !strcmp(first, "refresh"))) {
        action = !strcmp(first, "list") ? "ls" : first;
        pattern = second;
    }
    bool json = cp_bool(p, "json");
    const char *kind = cp_str(p, "kind") ? cp_str(p, "kind") : "chat";
    if (!strcmp(action, "find")) {
        char *t = pattern ? str_trim(pattern) : NULL;
        bool empty = !t || !*t;
        free(t);
        if (empty) {
            fprintf(stderr, "`%s models find` requires a search substring, e.g. `%s models find minimax`\n", CLI_BIN, CLI_BIN);
            return 1;
        }
    }
    cli_init_runtime(cp_strs(p, "config"));
    if (!strcmp(action, "refresh") && !json && isatty(2)) fprintf(stderr, "Refreshing models from all providers…\n");
    catalog_discover_local(!strcmp(action, "refresh"));
    VEC(const ai_model_t *) avail = {0}, filtered = {0};
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (!catalog_model_available(m)) continue;
        if (strcmp(kind, "all") && strcmp(catalog_model_kind(m), kind)) continue;
        vec_push(&avail, m);
    }
    char *needle = pattern ? str_lower(pattern) : NULL;
    if (needle && *needle) {
        bool exact = false;
        if (strcmp(action, "find")) {
            for (size_t i = 0; i < avail.n; i++)
                if (str_ieq(avail.v[i]->provider, needle)) {
                    vec_push(&filtered, avail.v[i]);
                    exact = true;
                }
        }
        if (!exact) {
            for (size_t i = 0; i < avail.n; i++) {
                const ai_model_t *m = avail.v[i];
                char *sel = xasprintf("%s/%s", m->provider, m->id);
                if (str_istr(m->id, needle) || str_istr(m->provider, needle) || str_istr(sel, needle) ||
                    (m->name && str_istr(m->name, needle)))
                    vec_push(&filtered, m);
                free(sel);
            }
        }
    } else {
        for (size_t i = 0; i < avail.n; i++) vec_push(&filtered, avail.v[i]);
    }
    if (filtered.n > 1) qsort(filtered.v, filtered.n, sizeof *filtered.v, by_provider_then_id);
    int rc = 0;
    if (json) {
        json_t *out = json_obj(), *arr = json_arr();
        for (size_t i = 0; i < filtered.n; i++) json_push(arr, model_json(filtered.v[i]));
        json_set(out, "models", arr);
        char *s = json_dump(out, 0);
        printf("%s\n", s);
        free(s);
        json_free(out);
    } else if (!avail.n) {
        printf("No models available. Set API keys in environment variables.\n");
    } else if (!filtered.n) {
        printf("No models matching \"%s\"\n", pattern);
    } else {
        static const char *const H[] = {"model", "context", "max-out", "thinking", "images"};
        static const bool R[] = {false, true, true, false, false};
        for (size_t i = 0; i < filtered.n;) {
            size_t j = i;
            while (j < filtered.n && !strcmp(filtered.v[j]->provider, filtered.v[i]->provider)) j++;
            if (i) printf("\n");
            char *hp = xasprintf("%s", filtered.v[i]->provider), *cnt = xasprintf("(%zu)", j - i);
            char *bc = cli_cyan(hp), *b = cli_bold(bc), *d = cli_dim(cnt);
            printf("%s %s\n", b, d);
            free(hp);
            free(cnt);
            free(bc);
            free(b);
            free(d);
            strvec_t *rows = xcalloc(j - i, sizeof *rows);
            for (size_t k = i; k < j; k++) {
                const ai_model_t *m = filtered.v[k];
                strvec_t *row = &rows[k - i];
                strvec_push(row, m->id);
                strvec_push_own(row, format_limit(m->context_window));
                strvec_push_own(row, format_limit(m->max_tokens));
                json_t *eff = supported_efforts(m);
                if (eff) {
                    sb_t e = SB_INIT;
                    json_t *x;
                    json_arr_foreach(eff, q, x) sb_appendf(&e, "%s%s", e.len ? "," : "", json_sv(x));
                    strvec_push_own(row, sb_detach(&e));
                } else strvec_push(row, m->reasoning ? "yes" : "-");
                json_free(eff);
                strvec_push(row, images_on_wire(m) ? "yes" : "no");
            }
            box_table(H, R, 5, rows, j - i);
            for (size_t k = 0; k < j - i; k++) strvec_free(&rows[k]);
            free(rows);
            i = j;
        }
    }
    free(needle);
    vec_free(&avail);
    vec_free(&filtered);
    return rc;
}
