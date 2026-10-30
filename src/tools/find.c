/* find tool: behavior/concept search = lexical scan + passage windows + model judge. */
#include "tools.h"

#include "core/fs.h"
#include "text/walker.h"

#include <ctype.h>
#include <math.h>

#define FIND_FILES_READ 20
#define FIND_WINDOW_LINES 60
#define FIND_WINDOWS_PER_FILE 6
#define FIND_MAX_PASSAGES 40
#define FIND_TAU 0.20

static const char *const STOP[] = {"the", "and", "for", "that", "this", "with", "from", "where", "what", "when", "how",
                                   "which", "into", "are", "was", "does", "code", "file", "files", "function", "find",
                                   "about", "there", "their", "them", "then", "than", "have", "has", "not", "all", "any",
                                   "can", "will", "should", "would", "implement", "implemented", "implementation",
                                   "handles", "handle", "logic", "using", "uses", "used", "use", NULL};

static bool is_stop(const char *w) {
    for (size_t i = 0; STOP[i]; i++)
        if (!strcmp(STOP[i], w)) return true;
    return false;
}

static void stem(char *w) {
    size_t n = strlen(w);
    const char *suf[] = {"ing", "ed", "es", "s"};
    for (size_t i = 0; i < 4; i++) {
        size_t l = strlen(suf[i]);
        if (n > l && n - l >= 4 && !strcmp(w + n - l, suf[i])) {
            w[n - l] = 0;
            return;
        }
    }
}

static void add_keyword(strvec_t *kw, const char *raw, bool verbatim) {
    char *w = str_lower(raw);
    if (!verbatim) stem(w);
    if (strlen(w) >= 3 && !is_stop(w) && strspn(w, "0123456789") != strlen(w) && !strvec_contains(kw, w)) strvec_push(kw, w);
    free(w);
}

static void extract_keywords(const char *query, const json_t *extra, strvec_t *kw) {
    /* quoted phrases are kept whole */
    const char *p = query;
    sb_t word = SB_INIT;
    while (*p) {
        if (*p == '"') {
            const char *e = strchr(p + 1, '"');
            if (e) {
                char *ph = xstrndup(p + 1, (size_t)(e - p - 1));
                add_keyword(kw, ph, true);
                free(ph);
                p = e + 1;
                continue;
            }
        }
        if (isalnum((unsigned char)*p) || *p == '_') sb_putc(&word, *p);
        else if (word.len) {
            add_keyword(kw, word.s, false);
            sb_clear(&word);
        }
        p++;
    }
    if (word.len) add_keyword(kw, word.s, false);
    sb_free(&word);
    json_t *x;
    json_arr_foreach(extra, i, x) if (json_is_str(x)) add_keyword(kw, json_sv(x), true);
}

static bool skip_file(const char *rel, int64_t size) {
    static const char *const bad_ext[] = {".lock", ".png", ".jpg", ".jpeg", ".gif", ".ico", ".pdf", ".zip", ".gz", ".tar",
                                          ".so", ".dylib", ".dll", ".exe", ".o", ".a", ".class", ".jar", ".wasm", ".woff",
                                          ".woff2", ".ttf", ".mp4", ".mp3", ".bin", ".pem", ".key", ".min.js", ".map", NULL};
    if (size == 0 || size > (4 << 20)) return true;
    for (size_t i = 0; bad_ext[i]; i++)
        if (str_ends(rel, bad_ext[i])) return true;
    const char *base = path_basename(rel);
    if (!strcmp(base, "package-lock.json") || !strcmp(base, "Cargo.lock") || !strcmp(base, ".env") || str_starts(base, "id_rsa"))
        return true;
    if (strstr(rel, "/dist/") || strstr(rel, "/build/") || str_starts(rel, "dist/") || str_starts(rel, "build/") ||
        strstr(rel, "/target/") || str_starts(rel, "target/"))
        return true;
    return false;
}

typedef struct {
    char *rel;
    double score;
    double name_score;
} cand_t;

typedef struct {
    size_t file;   /* index into cands */
    int64_t start, end;
    double lex;
    double p;
    char *sketch;
} passage_t;

static int cmp_cand(const void *a, const void *b) {
    double x = ((const cand_t *)a)->score, y = ((const cand_t *)b)->score;
    return x < y ? 1 : x > y ? -1 : 0;
}

static int cmp_pass(const void *a, const void *b) {
    double x = ((const passage_t *)a)->p, y = ((const passage_t *)b)->p;
    return x < y ? 1 : x > y ? -1 : 0;
}

static size_t count_ci(const char *hay, const char *needle) {
    size_t c = 0, n = strlen(needle);
    for (const char *p = hay; (p = str_istr(p, needle)); p += n) c++;
    return c;
}

static int find_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    const char *query = json_get_str(args, "query");
    if (!query || strspn(query, " \t\n") == strlen(query)) {
        tool_result_error(out, "query must not be empty");
        return 0;
    }
    int64_t t0 = mono_ms();
    strvec_t kw = {0};
    extract_keywords(query, json_get_arr(args, "grep_keywords"), &kw);
    const char *patharg = json_get_str(args, "path");
    char *root = patharg ? tool_resolve_path(env, patharg) : xstrdup(env->cwd);
    if (!fs_exists(root)) {
        tool_result_error(out, "Path not found: %s", patharg);
        free(root);
        strvec_free(&kw);
        return 0;
    }
    /* 1. lexical scan */
    walk_opts_t wo = {0};
    wo.types = WALK_FILE;
    wo.want_stat = true;
    wo.skip_node_modules = true;
    walk_list_t wl = {0};
    bool single = fs_is_file(root);
    if (!single) walk_collect(root, &wo, &wl);
    VEC(cand_t) cands = {0};
    size_t nfiles = single ? 1 : wl.n;
    double *df = xcalloc(kw.n + 1, sizeof(double));
    VEC(double *) tf = {0};
    for (size_t i = 0; i < nfiles; i++) {
        if (ctx->cancel && *ctx->cancel) break;
        const char *rel = single ? path_basename(root) : wl.v[i].path;
        int64_t size = single ? fs_size(root) : wl.v[i].size;
        if (!single && skip_file(rel, size)) continue;
        char *abs = single ? xstrdup(root) : path_join(root, rel);
        size_t n;
        char *data = fs_read_limit(abs, 1u << 20, &n, NULL);
        free(abs);
        if (!data || fs_is_binary(data, n)) {
            free(data);
            continue;
        }
        double *counts = xcalloc(kw.n + 1, sizeof(double));
        double name_bonus = 0;
        bool any = false;
        for (size_t k = 0; k < kw.n; k++) {
            counts[k] = (double)count_ci(data, kw.v[k]);
            if (counts[k] > 0) {
                df[k] += 1;
                any = true;
            }
            if (str_istr(rel, kw.v[k])) name_bonus += 1;
        }
        free(data);
        if (!any && name_bonus == 0) {
            free(counts);
            continue;
        }
        cand_t c = {xstrdup(rel), 0, name_bonus};
        vec_push(&cands, c);
        vec_push(&tf, counts);
    }
    double N = (double)MAX(nfiles, (size_t)1);
    for (size_t i = 0; i < cands.n; i++) {
        double s = 0;
        for (size_t k = 0; k < kw.n; k++) {
            if (tf.v[i][k] <= 0) continue;
            double idf = log(N / (1.0 + df[k]));
            if (idf < 0.5) idf = 0.5;
            if (idf > 6) idf = 6;
            s += idf * (1 + log(tf.v[i][k]));
        }
        cands.v[i].score = s + 1.5 * cands.v[i].name_score;
        free(tf.v[i]);
    }
    vec_free(&tf);
    free(df);
    if (cands.n > 1) qsort(cands.v, cands.n, sizeof(cand_t), cmp_cand);
    /* 2. passages from the top files */
    VEC(passage_t) ps = {0};
    size_t nread = MIN(cands.n, (size_t)FIND_FILES_READ);
    size_t bytes_sent = 0;
    for (size_t i = 0; i < nread; i++) {
        char *abs = single ? xstrdup(root) : path_join(root, cands.v[i].rel);
        size_t n;
        char *data = fs_read_limit(abs, 4u << 20, &n, NULL);
        free(abs);
        if (!data) continue;
        strvec_t lines = str_split_lines(data, n);
        free(data);
        VEC(passage_t) local = {0};
        for (size_t s = 0; s < lines.n; s += FIND_WINDOW_LINES / 2) {
            size_t e = MIN(lines.n, s + FIND_WINDOW_LINES);
            double score = 0;
            for (size_t l = s; l < e; l++)
                for (size_t k = 0; k < kw.n; k++)
                    if (str_istr(lines.v[l], kw.v[k])) score += 1;
            if (score > 0 || lines.n <= FIND_WINDOW_LINES) {
                passage_t pp = {i, (int64_t)s + 1, (int64_t)e, score, 0, NULL};
                vec_push(&local, pp);
            }
            if (e == lines.n) break;
        }
        /* keep the best windows per file */
        for (size_t a = 1; a < local.n; a++)
            for (size_t b2 = a; b2 > 0 && local.v[b2].lex > local.v[b2 - 1].lex; b2--) {
                passage_t t = local.v[b2];
                local.v[b2] = local.v[b2 - 1];
                local.v[b2 - 1] = t;
            }
        for (size_t a = 0; a < local.n && a < FIND_WINDOWS_PER_FILE; a++) {
            passage_t pp = local.v[a];
            /* sketch: keyword-dense lines first, bounded */
            sb_t sk = SB_INIT;
            for (int64_t l = pp.start; l <= pp.end && sk.len < 600; l++) {
                const char *ln = lines.v[l - 1];
                bool hit = false;
                for (size_t k = 0; k < kw.n && !hit; k++) hit = str_istr(ln, kw.v[k]) != NULL;
                if (hit || l - pp.start < 3) {
                    char *t = str_trim(ln);
                    if (*t) sb_appendf(&sk, "L%lld| %.*s\n", (long long)l, 160, t);
                    free(t);
                }
            }
            pp.sketch = sb_detach(&sk);
            vec_push(&ps, pp);
        }
        vec_free(&local);
        strvec_free(&lines);
    }
    if (ps.n > FIND_MAX_PASSAGES) {
        for (size_t a = 1; a < ps.n; a++)
            for (size_t b2 = a; b2 > 0 && ps.v[b2].lex > ps.v[b2 - 1].lex; b2--) {
                passage_t t = ps.v[b2];
                ps.v[b2] = ps.v[b2 - 1];
                ps.v[b2 - 1] = t;
            }
        for (size_t a = FIND_MAX_PASSAGES; a < ps.n; a++) free(ps.v[a].sketch);
        ps.n = FIND_MAX_PASSAGES;
    }
    /* 3. judge */
    bool judged = false;
    int requests = 0, failures = 0;
    if (ps.n && env->complete) {
        sb_t u = SB_INIT;
        sb_appendf(&u, "Question: which passages implement or directly answer this?\n\"%s\"\n\n", query);
        for (size_t a = 0; a < ps.n; a++)
            sb_appendf(&u, "<passage id=\"%zu\" file=\"%s\" lines=\"%lld-%lld\">\n%s</passage>\n", a, cands.v[ps.v[a].file].rel,
                       (long long)ps.v[a].start, (long long)ps.v[a].end, ps.v[a].sketch);
        sb_append(&u, "\nAnswer ONLY with a JSON object mapping every passage id to the probability (0-1) that it is relevant, "
                      "e.g. {\"0\":0.9,\"1\":0.05}.");
        bytes_sent = u.len;
        requests = 1;
        char *ans = env->complete(env, "You judge code search relevance. Output only JSON.", sb_cstr(&u));
        sb_free(&u);
        json_t *j = NULL;
        if (ans) {
            const char *a0 = strchr(ans, '{'), *a1 = strrchr(ans, '}');
            if (a0 && a1 && a1 > a0) j = json_parse(a0, (size_t)(a1 - a0 + 1), NULL);
        }
        if (json_is_obj(j)) {
            judged = true;
            for (size_t a = 0; a < ps.n; a++) {
                char key[24];
                snprintf(key, sizeof key, "%zu", a);
                json_t *v = json_get(j, key);
                ps.v[a].p = json_is_num(v) ? v->u.num.d : 0;
            }
        } else {
            failures = 1;
        }
        json_free(j);
        free(ans);
    }
    if (!judged) {
        double maxl = 0;
        for (size_t a = 0; a < ps.n; a++) maxl = MAX(maxl, ps.v[a].lex);
        for (size_t a = 0; a < ps.n; a++) ps.v[a].p = maxl > 0 ? 0.25 + 0.6 * ps.v[a].lex / maxl : 0;
    }
    if (ps.n > 1) qsort(ps.v, ps.n, sizeof(passage_t), cmp_pass);
    /* 4. output grouped per file, strongest first */
    sb_t b = SB_INIT;
    size_t hits = 0;
    bool *done = xcalloc(cands.n + 1, sizeof(bool));
    sb_t body = SB_INIT;
    json_t *hits_j = json_arr();
    for (size_t a = 0; a < ps.n; a++) {
        if (ps.v[a].p < FIND_TAU) break;
        size_t f = ps.v[a].file;
        if (done[f]) continue;
        done[f] = true;
        hits++;
        sb_appendf(&body, "%s  %.2f\n", cands.v[f].rel, ps.v[a].p);
        json_t *h = json_obj();
        json_set_str(h, "rel", cands.v[f].rel);
        json_t *ranges = json_arr();
        int shown = 0;
        for (size_t c = a; c < ps.n && shown < 3; c++) {
            if (ps.v[c].file != f || ps.v[c].p < FIND_TAU) continue;
            char *first = xstrndup(ps.v[c].sketch, strcspn(ps.v[c].sketch, "\n"));
            sb_appendf(&body, "  %s:%lld-%lld  %.2f  %s\n", cands.v[f].rel, (long long)ps.v[c].start, (long long)ps.v[c].end,
                       ps.v[c].p, first);
            json_t *r = json_obj();
            json_set_int(r, "start", ps.v[c].start);
            json_set_int(r, "end", ps.v[c].end);
            json_set_num(r, "p", ps.v[c].p);
            json_push(ranges, r);
            free(first);
            shown++;
        }
        json_set(h, "ranges", ranges);
        json_push(hits_j, h);
    }
    char *scope = patharg ? xasprintf(" in %s/", patharg) : xstrdup("");
    if (hits) sb_appendf(&b, "%zu hit(s) for \"%s\"%s (τ %.2f), strongest first\n%s", hits, query, scope, FIND_TAU, sb_cstr(&body));
    else {
        sb_appendf(&b, "no hits for \"%s\"%s (τ %.2f)\n", query, scope, FIND_TAU);
        out->useless = true;
    }
    sb_appendf(&b, "[listed %zu, judged %zu passages in %zu files, %s%s, %d request(s), %zu bytes sent, %lldms]", nfiles, ps.n,
               nread, judged ? "model-judged" : "lexical only", failures ? " (judge failed)" : "", requests, bytes_sent,
               (long long)(mono_ms() - t0));
    tool_result_text(out, sb_cstr(&b));
    out->details = json_obj();
    json_set_str(out->details, "query", query);
    json_t *kj = json_arr();
    for (size_t k = 0; k < kw.n; k++) json_push_str(kj, kw.v[k]);
    json_set(out->details, "keywords", kj);
    json_set_num(out->details, "threshold", FIND_TAU);
    json_set(out->details, "hits", hits_j);
    json_set_str(out->details, "coverage", judged ? "complete" : env->complete ? "unavailable" : "lexical");
    free(scope);
    free(done);
    sb_free(&body);
    sb_free(&b);
    for (size_t a = 0; a < ps.n; a++) free(ps.v[a].sketch);
    vec_free(&ps);
    for (size_t i = 0; i < cands.n; i++) free(cands.v[i].rel);
    vec_free(&cands);
    walk_list_free(&wl);
    strvec_free(&kw);
    free(root);
    return 0;
}

agent_tool_t *tool_find_new(tool_env_t *env) {
    char *desc = tool_prompt("find", NULL);
    json_t *s = schema_obj();
    schema_prop(s, "query", "string", "Plain-language description of the behavior or concept", true);
    schema_prop_json(s, "grep_keywords", json_parse_str("{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"), true);
    schema_prop(s, "path", "string", "Directory or file to search (default: workspace root)", false);
    agent_tool_t *t = agent_tool_new("find", "Find", desc, s, find_execute);
    free(desc);
    t->ud = env;
    return t;
}
