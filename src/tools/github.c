/* github.c - pr:// and issue:// internal URLs via the GitHub REST API. */
#include "tools.h"

#include "core/fs.h"
#include "core/proc.h"
#include "net/http.h"

#include <ctype.h>

static char *gh_token(void) {
    const char *t = env_get("GITHUB_TOKEN");
    if (!t) t = env_get("GH_TOKEN");
    if (t) return xstrdup(t);
    char *gh = which("gh");
    if (!gh) return NULL;
    free(gh);
    return proc_shell_capture("gh auth token 2>/dev/null", NULL, 5000);
}

/* owner/repo from `git remote get-url origin` */
static char *repo_slug(const char *cwd) {
    char *url = proc_shell_capture("git remote get-url origin 2>/dev/null", cwd, 5000);
    if (!url) return NULL;
    char *r = NULL;
    const char *p = strstr(url, "github.com");
    if (p) {
        p += 10;
        if (*p == ':' || *p == '/') p++;
        size_t n = strlen(p);
        if (n > 4 && !strcmp(p + n - 4, ".git")) n -= 4;
        while (n && p[n - 1] == '/') n--;
        r = xstrndup(p, n);
    }
    free(url);
    return r;
}

static json_t *gh_get(const char *path, char **err) {
    char *url = xasprintf("https://api.github.com%s", path);
    http_headers_t h = {0};
    http_headers_add(&h, "Accept", "application/vnd.github+json");
    http_headers_add(&h, "X-GitHub-Api-Version", "2022-11-28");
    char *tok = gh_token();
    if (tok && *tok) {
        char *a = xasprintf("Bearer %s", tok);
        http_headers_add(&h, "Authorization", a);
        free(a);
    }
    free(tok);
    http_resp_t r;
    int rc = http_get(url, &h, 30000, &r);
    http_headers_free(&h);
    free(url);
    json_t *j = NULL;
    if (rc != 0) *err = xstrdup(r.error ? r.error : "request failed");
    else if (r.status >= 400) *err = xasprintf("GitHub API %d: %.300s", r.status, r.body ? r.body : "");
    else j = json_parse(r.body, r.body_len, NULL);
    http_resp_free(&r);
    return j;
}

static void add_comments(sb_t *b, const char *repo, const char *num, const char *kind) {
    char *err = NULL;
    char *p = xasprintf("/repos/%s/issues/%s/comments?per_page=100", repo, num);
    json_t *cs = gh_get(p, &err);
    free(p);
    free(err);
    json_t *c;
    if (json_len(cs)) sb_appendf(b, "\n## Comments (%zu)\n", json_len(cs));
    json_arr_foreach(cs, i, c) {
        sb_appendf(b, "\n### @%s · %s\n%s\n", json_get_str(json_get(c, "user"), "login"), json_get_str(c, "created_at"),
                   json_get_str(c, "body") ? json_get_str(c, "body") : "");
    }
    json_free(cs);
    if (!strcmp(kind, "pr")) {
        p = xasprintf("/repos/%s/pulls/%s/comments?per_page=100", repo, num);
        json_t *rc = gh_get(p, &err);
        free(p);
        free(err);
        if (json_len(rc)) sb_appendf(b, "\n## Review comments (%zu)\n", json_len(rc));
        json_arr_foreach(rc, i, c) {
            sb_appendf(b, "\n### @%s on %s:%lld\n%s\n", json_get_str(json_get(c, "user"), "login"), json_get_str(c, "path"),
                       (long long)json_get_int(c, "line", json_get_int(c, "original_line", 0)),
                       json_get_str(c, "body") ? json_get_str(c, "body") : "");
        }
        json_free(rc);
    }
}

/* parse query string ?state=&limit=&author=&label= */
static char *qparam(const char *q, const char *name) {
    if (!q) return NULL;
    size_t n = strlen(name);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (!strncmp(p, name, n) && p[n] == '=') return xstrndup(p + n + 1, strcspn(p + n + 1, "&"));
    }
    return NULL;
}

int github_read(tool_env_t *env, const char *url, tool_result_t *out) {
    bool pr = str_starts(url, "pr://");
    const char *rest = url + (pr ? 5 : 8);
    char *body = xstrdup(rest);
    char *q = strchr(body, '?');
    char *query = NULL;
    if (q) {
        query = xstrdup(q + 1);
        *q = 0;
    }
    /* owner/repo/N or N, followed by optional /diff[/i|all] */
    strvec_t seg = str_split(body, "/");
    char *repo = NULL;
    const char *num = NULL;
    size_t si = 0;
    if (seg.n >= 3 && !isdigit((unsigned char)seg.v[0][0])) {
        repo = xasprintf("%s/%s", seg.v[0], seg.v[1]);
        si = 2;
    } else {
        repo = repo_slug(env->cwd);
    }
    if (si < seg.n && *seg.v[si]) num = seg.v[si++];
    if (!repo) {
        tool_result_error(out, "Cannot determine the GitHub repository (no github.com origin remote); use %s://owner/repo/N",
                          pr ? "pr" : "issue");
        goto done;
    }
    sb_t b = SB_INIT;
    char *err = NULL;
    if (!num) {
        char *state = qparam(query, "state"), *limit = qparam(query, "limit"), *author = qparam(query, "author"),
             *label = qparam(query, "label");
        char *path = xasprintf("/repos/%s/%s?state=%s&per_page=%s%s%s", repo, pr ? "pulls" : "issues", state ? state : "open",
                               limit ? limit : "20", label && !pr ? "&labels=" : "", label && !pr ? label : "");
        json_t *list = gh_get(path, &err);
        free(path);
        if (list) {
            sb_appendf(&b, "# %s %s (%s)\n", repo, pr ? "pull requests" : "issues", state ? state : "open");
            json_t *it;
            json_arr_foreach(list, i, it) {
                if (!pr && json_get(it, "pull_request")) continue;
                const char *login = json_get_str(json_get(it, "user"), "login");
                if (author && !str_eq(login, author)) continue;
                sb_appendf(&b, "- #%lld %s (@%s, %s)\n", (long long)json_get_int(it, "number", 0), json_get_str(it, "title"),
                           login ? login : "?", json_get_str(it, "updated_at"));
            }
            json_free(list);
        }
        free(state);
        free(limit);
        free(author);
        free(label);
    } else if (pr && si < seg.n && !strcmp(seg.v[si], "diff")) {
        const char *which_file = si + 1 < seg.n ? seg.v[si + 1] : NULL;
        char *path = xasprintf("/repos/%s/pulls/%s/files?per_page=100", repo, num);
        json_t *files = gh_get(path, &err);
        free(path);
        if (files) {
            json_t *f;
            if (!which_file) {
                sb_appendf(&b, "# PR #%s changed files (%zu)\n", num, json_len(files));
                json_arr_foreach(files, i, f)
                    sb_appendf(&b, "%zu. %s %s (+%lld -%lld)\n", i, json_get_str(f, "status"), json_get_str(f, "filename"),
                               (long long)json_get_int(f, "additions", 0), (long long)json_get_int(f, "deletions", 0));
                sb_appendf(&b, "\nRead one with pr://%s/diff/<i> or all with pr://%s/diff/all", num, num);
            } else {
                bool all = !strcmp(which_file, "all");
                size_t idx = all ? 0 : (size_t)atoi(which_file);
                json_arr_foreach(files, i, f) {
                    if (!all && i != idx) continue;
                    sb_appendf(&b, "diff --git a/%s b/%s\n%s\n", json_get_str(f, "filename"), json_get_str(f, "filename"),
                               json_get_str(f, "patch") ? json_get_str(f, "patch") : "(binary or too large)");
                }
                if (!b.len) err = xasprintf("No file %s in PR #%s", which_file, num);
            }
            json_free(files);
        }
    } else {
        char *path = xasprintf("/repos/%s/%s/%s", repo, pr ? "pulls" : "issues", num);
        json_t *it = gh_get(path, &err);
        free(path);
        if (it) {
            sb_appendf(&b, "# %s #%s: %s\n", pr ? "PR" : "Issue", num, json_get_str(it, "title"));
            sb_appendf(&b, "- State: %s%s\n- Author: @%s\n- Created: %s · Updated: %s\n", json_get_str(it, "state"),
                       json_get_bool(it, "merged", false) ? " (merged)" : json_get_bool(it, "draft", false) ? " (draft)" : "",
                       json_get_str(json_get(it, "user"), "login"), json_get_str(it, "created_at"), json_get_str(it, "updated_at"));
            if (pr) sb_appendf(&b, "- Branch: %s → %s\n- Changes: %lld files, +%lld -%lld (pr://%s/diff)\n",
                               json_get_str(json_get(it, "head"), "label"), json_get_str(json_get(it, "base"), "ref"),
                               (long long)json_get_int(it, "changed_files", 0), (long long)json_get_int(it, "additions", 0),
                               (long long)json_get_int(it, "deletions", 0), num);
            json_t *labels = json_get_arr(it, "labels"), *l;
            if (json_len(labels)) {
                sb_append(&b, "- Labels:");
                json_arr_foreach(labels, i, l) sb_appendf(&b, " %s", json_get_str(l, "name"));
                sb_putc(&b, '\n');
            }
            sb_appendf(&b, "- URL: %s\n\n%s\n", json_get_str(it, "html_url"), json_get_str(it, "body") ? json_get_str(it, "body") : "(no description)");
            add_comments(&b, repo, num, pr ? "pr" : "issue");
            json_free(it);
        }
    }
    if (err) tool_result_error(out, "%s", err);
    else tool_result_text(out, sb_cstr(&b));
    free(err);
    sb_free(&b);
done:
    strvec_free(&seg);
    free(repo);
    free(query);
    free(body);
    return 0;
}
