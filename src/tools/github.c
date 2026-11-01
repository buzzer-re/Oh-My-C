/* github.c - pr:// and issue:// internal URLs (port of omp's internal-urls/issue-pr-protocol.ts and
 * forge-diff.ts). Single items and PR diffs go through `gh` exactly like omp; listings use `gh issue|pr list`.
 * The rendered markdown is the resource content; read.c applies line selectors. */
#include "gh.h"
#include "gh_int.h"

#include "net/http.h"

#include <ctype.h>

typedef struct {
    char *host;       /* raw authority ("" when none) */
    char *path;       /* raw path after the authority (may be "") */
    json_t *query;    /* decoded search params: name -> first value */
} iurl_t;

static void iurl_free(iurl_t *u) {
    free(u->host);
    free(u->path);
    json_free(u->query);
}

static char *form_decode(const char *s, size_t n) {
    char *t = xstrndup(s, n);
    for (char *p = t; *p; p++)
        if (*p == '+') *p = ' ';
    char *d = url_decode(t);
    free(t);
    return d;
}

static void iurl_parse(const char *url, size_t scheme_len, iurl_t *u) {
    memset(u, 0, sizeof *u);
    const char *rest = url + scheme_len;
    const char *q = strchr(rest, '?');
    const char *h = strchr(rest, '#');
    size_t body = q ? (size_t)(q - rest) : h ? (size_t)(h - rest) : strlen(rest);
    size_t hl = strcspn(rest, "/?#");
    if (hl > body) hl = body;
    u->host = xstrndup(rest, hl);
    u->path = xstrndup(rest + hl, body - hl);
    u->query = json_obj();
    if (q) {
        const char *qs = q + 1;
        size_t ql = h && h > q ? (size_t)(h - qs) : strlen(qs);
        char *qcopy = xstrndup(qs, ql);
        strvec_t parts = str_split(qcopy, "&");
        for (size_t i = 0; i < parts.n; i++) {
            if (!*parts.v[i]) continue;
            char *eq = strchr(parts.v[i], '=');
            char *k = form_decode(parts.v[i], eq ? (size_t)(eq - parts.v[i]) : strlen(parts.v[i]));
            char *v = eq ? form_decode(eq + 1, strlen(eq + 1)) : xstrdup("");
            if (!json_get(u->query, k)) json_set_str(u->query, k, v);
            free(k);
            free(v);
        }
        strvec_free(&parts);
        free(qcopy);
    }
}

static const char *qget(const iurl_t *u, const char *k) { return json_get_str(u->query, k); }

/* splitInternalUrlSegments; false + *err on an empty/unsafe segment */
static bool split_segments(const iurl_t *u, const char *scheme, strvec_t *out, char **err) {
    memset(out, 0, sizeof *out);
    const char *p = u->path;
    if (*p == '/') p++;
    if (!*p) return true;
    strvec_t segs = str_split(p, "/");
    for (size_t i = 0; i < segs.n; i++) {
        char *d = url_decode(segs.v[i]);
        bool bad = !d || !*d || !strcmp(d, ".") || !strcmp(d, "..");
        free(d);
        if (bad) {
            *err = xasprintf("Invalid %s:// URL: empty or unsafe path segment", scheme);
            strvec_free(&segs);
            strvec_free(out);
            return false;
        }
        strvec_push(out, segs.v[i]);
    }
    strvec_free(&segs);
    return true;
}

typedef enum { P_SINGLE, P_DIFF, P_LIST } pkind_t;
typedef struct {
    pkind_t kind;
    char *repo;
    int64_t number;
    bool comments;
    int diff_mode; /* 0 list, 1 all, 2 slice */
    int64_t index;
    int64_t fetched_at; /* -1 none */
    const char *state;
    int64_t limit;
    char *author, *label;
} parsed_t;

static void parsed_free(parsed_t *p) {
    free(p->repo);
    free(p->author);
    free(p->label);
}

static bool parse_list(const iurl_t *u, bool pr, char *repo, parsed_t *p, char **err) {
    p->kind = P_LIST;
    p->repo = repo;
    const char *st = qget(u, "state");
    static const char *const prs[] = {"open", "closed", "merged", "all"}, *const iss[] = {"open", "closed", "all"};
    if (st) {
        bool ok = false;
        for (size_t i = 0; i < (pr ? 4u : 3u); i++)
            if (!strcmp(st, pr ? prs[i] : iss[i])) {
                ok = true;
                p->state = pr ? prs[i] : iss[i];
            }
        if (!ok) {
            *err = xasprintf("Invalid %s:// list state '%s'. Expected one of: %s.", pr ? "pr" : "issue", st,
                             pr ? "open, closed, merged, all" : "open, closed, all");
            return false;
        }
    } else {
        p->state = "open";
    }
    p->limit = 30;
    const char *lim = qget(u, "limit");
    if (lim) {
        int64_t v = gh_parse_pos_int(lim);
        if (v < 0) {
            *err = xasprintf("Invalid %s:// list limit '%s'. Expected a positive integer (max 100).", pr ? "pr" : "issue", lim);
            return false;
        }
        p->limit = v < 100 ? v : 100;
    }
    if (qget(u, "author")) p->author = xstrdup(qget(u, "author"));
    if (qget(u, "label")) p->label = xstrdup(qget(u, "label"));
    return true;
}

static bool parse_url(const iurl_t *u, bool pr, parsed_t *p, char **err) {
    memset(p, 0, sizeof *p);
    p->fetched_at = -1;
    const char *scheme = pr ? "pr" : "issue";
    strvec_t parts;
    if (!split_segments(u, scheme, &parts, err)) return false;
    const char *host = u->host;
    char *repo_host = NULL;
    size_t off = 0; /* parts consumed by a host prefix */
    bool dotted = strchr(host, '.') != NULL;
    bool ok = false;
    char *repo = NULL;
    const char *number_part = NULL;
    size_t diff_from = 0, diff_n = 0;
    if (dotted && parts.n < 2) {
        *err = xasprintf("Invalid %s:// URL. Expected %s://<host>/<owner>/<repo> or %s://<host>/<owner>/<repo>/<number>", scheme,
                         scheme, scheme);
        goto out;
    }
    bool prefixed = dotted ? parts.n >= 2 : parts.n >= 3 && gh_parse_pos_int(parts.v[2]) > 0;
    if (prefixed) {
        repo_host = xstrdup(host);
        host = parts.v[0];
        off = 1;
    }
    size_t np = parts.n - off;
    const char **pv = (const char **)parts.v + off;
    if (!*host && np == 0) {
        ok = parse_list(u, pr, NULL, p, err);
        goto out;
    }
    if (*host && np == 0) {
        number_part = host;
    } else if (np > 0 && !strcmp(pv[0], "diff") && gh_parse_pos_int(host) > 0) {
        number_part = host;
        diff_from = off;
        diff_n = np;
    } else if (*host && np == 1) {
        char *slug = xasprintf("%s/%s", host, pv[0]);
        repo = gh_format_ref(repo_host, slug);
        free(slug);
        ok = parse_list(u, pr, repo, p, err);
        repo = NULL;
        goto out;
    } else if (*host && np >= 2) {
        char *slug = xasprintf("%s/%s", host, pv[0]);
        repo = gh_format_ref(repo_host, slug);
        free(slug);
        number_part = pv[1];
        diff_from = off + 2;
        diff_n = np - 2;
    } else {
        *err = xasprintf("Invalid %s:// URL. Expected %s://, %s://<number>, %s://<owner>/<repo>, or %s://<owner>/<repo>/<number>",
                         scheme, scheme, scheme, scheme, scheme);
        goto out;
    }
    if (diff_n > 0) {
        if (!pr) {
            *err = xstrdup("Invalid issue:// URL. Issue views do not have a diff; use pr://<owner>/<repo>/<n>/diff for pull requests.");
            goto out;
        }
        if (strcmp(parts.v[diff_from], "diff") || diff_n > 2) {
            *err = xstrdup("Invalid pr:// URL. Expected pr://<n>, pr://<n>/diff, pr://<n>/diff/all, or pr://<n>/diff/<i>");
            goto out;
        }
    }
    int64_t num = gh_parse_pos_int(number_part);
    if (num < 0) {
        *err = xasprintf("Invalid %s:// number: %s", scheme, number_part ? number_part : "(missing)");
        goto out;
    }
    p->repo = repo;
    repo = NULL;
    p->number = num;
    if (!diff_n) {
        const char *c = qget(u, "comments");
        p->kind = P_SINGLE;
        p->comments = !c || !(!strcmp(c, "0") || !strcasecmp(c, "false"));
        ok = true;
        goto out;
    }
    p->kind = P_DIFF;
    const char *at = qget(u, "at");
    if (at) {
        int64_t v = gh_parse_pos_int(at);
        if (v < 0) {
            *err = xasprintf("Invalid pr:// diff snapshot '%s'. Expected a positive integer cache generation.", at);
            goto out;
        }
        p->fetched_at = v;
    }
    if (diff_n == 1) {
        p->diff_mode = 0;
    } else {
        const char *sub = parts.v[diff_from + 1];
        if (!strcmp(sub, "all")) p->diff_mode = 1;
        else {
            int64_t idx = gh_parse_pos_int(sub);
            if (idx < 0) {
                *err = xasprintf("Invalid pr:// diff sub-path '%s'. Use 'all' or a 1-indexed file number.", sub);
                goto out;
            }
            p->diff_mode = 2;
            p->index = idx;
        }
    }
    ok = true;
out:
    free(repo);
    free(repo_host);
    strvec_free(&parts);
    return ok;
}

/* formatListItem */
static void list_item(sb_t *b, bool pr, const char *repo, const json_t *it) {
    const json_t *num = gh_nn(json_get(it, "number")), *title = gh_nn(json_get(it, "title"));
    char *ns = num ? gh_jsv(num) : xstrdup("?"), *ts = title ? gh_jsv(title) : xstrdup("(no title)");
    const json_t *st = gh_nn(json_get(it, "state"));
    char *ss = st ? gh_jsv(st) : xstrdup("?");
    if (st)
        for (char *p = ss; *p; p++) *p = (char)tolower((unsigned char)*p);
    const json_t *login = gh_nn(gh_oc(gh_nn(json_get(it, "author")), "login"));
    char *as = login ? gh_jsv(login) : xstrdup("?");
    const json_t *upd = gh_nn(json_get(it, "updatedAt"));
    if (!upd) upd = gh_nn(json_get(it, "createdAt"));
    char *us = upd ? gh_jsv(upd) : xstrdup("");
    bool draft = pr && gh_truthy(json_get(it, "isDraft"));
    char *labels = gh_labels(json_get(it, "labels"));
    sb_appendf(b, "- [%s%s] #%s  @%s  %s\n    %s%s%s\n    ", ss, draft ? " [draft]" : "", ns, as, us, ts, labels ? "  labels: " : "",
               labels ? labels : "");
    if (!strcmp(ns, "?")) sb_appendf(b, "%s://%s", pr ? "pr" : "issue", repo);
    else sb_appendf(b, "%s://%s/%s", pr ? "pr" : "issue", repo, ns);
    free(ns);
    free(ts);
    free(ss);
    free(as);
    free(us);
    free(labels);
}

static char *render_list(tool_env_t *env, bool pr, const parsed_t *p, char **err) {
    const char *scheme = pr ? "pr" : "issue";
    char *repo = p->repo ? xstrdup(p->repo) : NULL;
    if (!repo) {
        char *e = NULL;
        repo = gh_default_repo(env->cwd, &e);
        if (!repo) {
            *err = xasprintf("%s:// could not resolve a default repo from the current session: %s\nUse %s://<owner>/<repo> instead.",
                             scheme, e, scheme);
            free(e);
            return NULL;
        }
    }
    char *lim = xasprintf("%lld", (long long)p->limit);
    strvec_t a = gh_args(scheme, "list", "--repo", repo, "--state", p->state, "--limit", lim, "--json",
                         pr ? "number,title,state,isDraft,author,baseRefName,headRefName,labels,createdAt,updatedAt,url"
                            : "number,title,state,author,labels,createdAt,updatedAt,url",
                         NULL);
    free(lim);
    if (p->author) {
        strvec_push(&a, "--author");
        strvec_push(&a, p->author);
    }
    if (p->label) {
        strvec_push(&a, "--label");
        strvec_push(&a, p->label);
    }
    json_t *items = pr ? gh_json(env->cwd, &a, true, true, err) : gh_issue_json(env->cwd, &a, true, err);
    strvec_free(&a);
    if (!items) {
        free(repo);
        return NULL;
    }
    sb_t b = SB_INIT;
    if (pr) sb_appendf(&b, "# Pull Requests in %s (%s, up to %lld)\n\n", repo, p->state, (long long)p->limit);
    else sb_appendf(&b, "# Issues in %s (%s, up to %lld)\n\n", repo, p->state, (long long)p->limit);
    if (!json_len(items)) sb_append(&b, "_No matches._");
    for (size_t i = 0; i < json_len(items); i++) {
        if (i) sb_append(&b, "\n\n");
        list_item(&b, pr, repo, json_at(items, i));
    }
    sb_appendf(&b, "\n\n---\nRead a specific item: `%s://%s/<N>` (or `%s://<N>` for the current repo).", scheme, repo, scheme);
    json_free(items);
    free(repo);
    return sb_detach(&b);
}

static char *render_diff(tool_env_t *env, const parsed_t *p, char **err) {
    char *repo = p->repo ? xstrdup(p->repo) : NULL;
    if (!repo) {
        char *e = NULL;
        repo = gh_default_repo(env->cwd, &e);
        if (!repo) {
            *err = xasprintf("pr://%lld/diff could not resolve a default repo from the current session: %s\nUse "
                             "pr://<owner>/<repo>/%lld/diff.",
                             (long long)p->number, e, (long long)p->number);
            free(e);
            return NULL;
        }
    }
    if (p->fetched_at >= 0) {
        /* omc keeps no GitHub view cache, so no pinned /review snapshot exists */
        *err = xasprintf("pr://%s/%lld/diff?at=%lld is not available: the pinned snapshot expired, the GitHub cache is disabled, or "
                         "it was saved under another GitHub account. Run /review again.",
                         repo, (long long)p->number, (long long)p->fetched_at);
        free(repo);
        return NULL;
    }
    char *text = gh_pr_diff_text(env->cwd, repo, p->number, err);
    if (!text) {
        free(repo);
        return NULL;
    }
    char *base = xasprintf("pr://%s/%lld", repo, (long long)p->number);
    const char *query = "";
    gh_diff_files_t files;
    gh_parse_unified_diff(text, &files);
    char *content = NULL;
    char *fc = xasprintf("%zu file%s", files.n, files.n == 1 ? "" : "s");
    if (p->diff_mode == 1) {
        content = xstrdup(text);
    } else if (p->diff_mode == 2) {
        if (p->index < 1 || p->index > (int64_t)files.n) {
            *err = xasprintf("%s/diff/%lld is out of range; PR has %s. Use %s/diff to list available indices.", base,
                             (long long)p->index, fc, base);
        } else {
            const gh_diff_file_t *f = &files.v[p->index - 1];
            content = xstrndup(text + f->start, f->end - f->start);
        }
    } else {
        sb_t b = SB_INIT;
        sb_appendf(&b, "# Pull Request Diff: %s#%lld (%s)\n\n", repo, (long long)p->number, fc);
        if (!files.n) sb_append(&b, "_No file changes._");
        for (size_t i = 0; i < files.n; i++) {
            const gh_diff_file_t *f = &files.v[i];
            if (i) sb_append(&b, "\n\n");
            char *stats = !strcmp(f->change_type, "binary") ? xstrdup("(binary)")
                                                            : xasprintf("+%lld -%lld", (long long)f->additions, (long long)f->deletions);
            sb_appendf(&b, "%zu. %s  %s  [%s]", i + 1, f->path, stats, f->change_type);
            if (f->old_path) sb_appendf(&b, "  (renamed from %s)", f->old_path);
            sb_appendf(&b, "\n   %s/diff/%zu%s", base, i + 1, query);
            free(stats);
        }
        sb_appendf(&b, "\n\n---\nRead all: `%s/diff/all%s`. Each file is also available as `%s/diff/<i>%s`.", base, query, base, query);
        content = sb_detach(&b);
    }
    free(fc);
    gh_diff_files_free(&files);
    free(base);
    free(text);
    free(repo);
    return content;
}

int github_resolve(tool_env_t *env, const char *url, char **content, bool *markdown, char **err) {
    *content = NULL;
    *err = NULL;
    *markdown = true;
    bool pr = str_starts(url, "pr://");
    const char *scheme = pr ? "pr" : "issue";
    iurl_t u;
    iurl_parse(url, pr ? 5 : 8, &u);
    parsed_t p;
    if (!parse_url(&u, pr, &p, err)) {
        iurl_free(&u);
        return -1;
    }
    char *e = NULL;
    if (p.kind == P_LIST) {
        *content = render_list(env, pr, &p, &e);
        if (!*content) *err = xasprintf("%s:// listing failed: %s", scheme, e);
    } else if (p.kind == P_DIFF) {
        *markdown = p.diff_mode == 0; /* the file listing is markdown, diff text is text/plain */
        *content = render_diff(env, &p, &e);
        if (!*content) *err = xasprintf("pr:// diff resolution failed: %s", e);
    } else if (!pr) {
        char *repo = p.repo ? xstrdup(p.repo) : NULL;
        if (!repo) {
            char *e2 = NULL;
            repo = gh_default_repo(env->cwd, &e2);
            free(e2);
        }
        char *ns = xasprintf("%lld", (long long)p.number);
        *content = gh_issue_view(env->cwd, repo, ns, p.comments, NULL, &e);
        if (!*content) *err = xasprintf("issue:// resolution failed: %s", e);
        free(ns);
        free(repo);
    } else {
        char *repo = p.repo ? xstrdup(p.repo) : NULL;
        if (!repo) {
            char *e2 = NULL;
            repo = gh_default_repo(env->cwd, &e2);
            if (!repo) {
                *err = xasprintf("pr://%lld could not resolve a default repo from the current session: %s\nUse pr://<owner>/<repo>/%lld.",
                                 (long long)p.number, e2, (long long)p.number);
                free(e2);
            }
        }
        if (repo) {
            *content = gh_pr_view(env->cwd, repo, p.number, p.comments, NULL, &e);
            if (!*content) *err = xasprintf("pr:// resolution failed: %s", e);
        }
        free(repo);
    }
    free(e);
    parsed_free(&p);
    iurl_free(&u);
    return *content ? 0 : -1;
}
