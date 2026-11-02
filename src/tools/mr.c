/* mr.c - `mr://` GitLab merge request diffs via `glab` (port of internal-urls/mr-protocol.ts, tools/glab-mr.ts,
 * utils/gitlab.ts and the diff-family renderer of internal-urls/forge-diff.ts).
 *
 *   mr://<host>/<group…>/<project>/<iid>/diff        changed-file listing
 *   …/diff/all                                       full unified diff
 *   …/diff/<i>                                       one file (1-indexed)
 *   mr://<iid>/diff[/…]                              project from the session cwd (`glab repo view`)
 *
 * omc keeps no forge view cache (like pr://), so every read runs `glab mr diff`. */
#include "tools/mr.h"

#include "core/fs.h"
#include "core/proc.h"
#include "text/yaml.h"
#include "tools/gh.h"

#include <ctype.h>
#include <pthread.h>

#define GLAB_TIMEOUT_MS (5 * 60 * 1000)
#define GLAB_OUTPUT_LIMIT (8u * 1024 * 1024)

static const char *const INVALID_MR_URL = "Invalid mr:// URL. Expected mr://<iid>/diff or "
                                          "mr://<host>/<group>/<project>/<iid>/diff, optionally followed by /all or /<i>.";

bool mr_glab_available(void) {
    char *p = which("glab");
    bool ok = p != NULL;
    free(p);
    return ok;
}

/* GITLAB_PATH_SEGMENT_PATTERN /^[A-Za-z0-9_.-]+$/ */
static bool path_segment_ok(const char *s) {
    if (!*s) return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("_.-", *s)) return false;
    return true;
}

/* GITLAB_HOST_PATTERN /^[A-Za-z0-9.-]+(?::\d+)?$/ */
static bool host_ok(const char *s) {
    const char *p = s;
    while (*p && (isalnum((unsigned char)*p) || *p == '.' || *p == '-')) p++;
    if (p == s) return false;
    if (!*p) return true;
    if (*p != ':' || !p[1]) return false;
    for (p++; *p; p++)
        if (!isdigit((unsigned char)*p)) return false;
    return true;
}

/* gitlab.run: `glab args…` in cwd with the non-interactive forge env; -1 + *err when glab is missing / timed out */
static int glab_run(const char *cwd, const strvec_t *args, bool trim, gh_res_t *r, char **err) {
    memset(r, 0, sizeof *r);
    if (!mr_glab_available()) {
        *err = xstrdup("GitLab CLI (glab) is not installed. Install it from https://gitlab.com/gitlab-org/cli.");
        return -1;
    }
    char *fals = which("false");
    char *ssh_askpass = xasprintf("SSH_ASKPASS=%s", fals ? fals : "false");
    free(fals);
    const char *env[] = {"GIT_ASKPASS=true", "GIT_EDITOR=true", "GIT_TERMINAL_PROMPT=0", "LC_ALL", "LC_MESSAGES=C",
                         ssh_askpass, "GLAB_NO_PROMPT=1", "GLAB_CHECK_UPDATE=false", "NO_COLOR=1", NULL};
    const char **argv = xcalloc(args->n + 2, sizeof(char *));
    argv[0] = "glab";
    for (size_t i = 0; i < args->n; i++) argv[i + 1] = args->v[i];
    proc_opts_t o = {0};
    o.cwd = cwd;
    o.env_extra = env;
    o.timeout_ms = GLAB_TIMEOUT_MS;
    o.max_output = GLAB_OUTPUT_LIMIT;
    o.new_session = true;
    proc_result_t pr;
    int rc = proc_run(argv, &o, &pr);
    free(argv);
    free(ssh_askpass);
    char *joined = strvec_join(args, " ");
    if (rc != 0 || pr.spawn_errno) {
        *err = xasprintf("GitLab CLI command failed: glab %s", joined);
        free(joined);
        proc_result_free(&pr);
        return -1;
    }
    if (pr.timed_out) {
        *err = xasprintf("GitLab CLI command timed out: glab %s", joined);
        free(joined);
        proc_result_free(&pr);
        return -1;
    }
    free(joined);
    sb_t out = SB_INIT, er = SB_INIT;
    sb_appendn(&out, pr.out ? pr.out : "", pr.out_len);
    sb_appendn(&er, pr.err ? pr.err : "", pr.err_len);
    if (pr.truncated && pr.out_len >= GLAB_OUTPUT_LIMIT) {
        sb_append(&out, "\n[glab subprocess output truncated after 8 MiB]\n");
        r->out_truncated = true;
    }
    if (pr.truncated && pr.err_len >= GLAB_OUTPUT_LIMIT) sb_append(&er, "\n[glab subprocess output truncated after 8 MiB]\n");
    r->exit_code = pr.exit_code < 0 ? 1 : pr.exit_code;
    if (trim) {
        r->out = gh_js_trim(sb_cstr(&out));
        r->err = gh_js_trim(sb_cstr(&er));
        sb_free(&out);
        sb_free(&er);
    } else {
        r->out = sb_detach(&out);
        r->err = sb_detach(&er);
    }
    proc_result_free(&pr);
    return 0;
}

/* formatGlabFailure */
static char *glab_failure(const strvec_t *args, const char *out, const char *err) {
    char *msg = gh_js_trim(err && *err ? err : out ? out : "");
    if (strstr(msg, "glab auth login") || strstr(msg, "401 Unauthorized") || strstr(msg, "has not been authenticated")) {
        free(msg);
        return xstrdup("GitLab CLI is not authenticated. Run `glab auth login`.");
    }
    if (*msg) return msg;
    free(msg);
    char *j = strvec_join(args, " ");
    char *r = xasprintf("GitLab CLI command failed: glab %s", j);
    free(j);
    return r;
}

/* gitlab.text: stdout, or NULL + *err on a non-zero exit */
static char *glab_text(const char *cwd, const strvec_t *args, bool trim, char **err) {
    gh_res_t r;
    if (glab_run(cwd, args, trim, &r, err) != 0) return NULL;
    char *out = NULL;
    if (r.exit_code != 0) *err = glab_failure(args, r.out, r.err);
    else out = xstrdup(r.out ? r.out : "");
    gh_res_free(&r);
    return out;
}

/* parseGitlabProjectWebUrl: `<host>/<namespace…>/<project>` (owned) or NULL */
static char *project_from_web_url(const char *url) {
    if (!url) return NULL;
    const char *p = str_starts(url, "https://") ? url + 8 : str_starts(url, "http://") ? url + 7 : NULL;
    if (!p) return NULL;
    const char *slash = strchr(p, '/');
    if (!slash || slash == p) return NULL;
    char *host = xstrndup(p, (size_t)(slash - p));
    if (strchr(host, '@')) { /* userinfo is not part of URL.host */
        char *h = xstrdup(strrchr(host, '@') + 1);
        free(host);
        host = h;
    }
    for (char *c = host; *c; c++) *c = (char)tolower((unsigned char)*c);
    /* drop a default port like URL.host does */
    size_t hl = strlen(host);
    if (hl > 4 && !strcmp(host + hl - 4, ":443") && str_starts(url, "https://")) host[hl - 4] = 0;
    else if (hl > 3 && !strcmp(host + hl - 3, ":80") && str_starts(url, "http://")) host[hl - 3] = 0;
    const char *path = slash;
    size_t pl = strcspn(path, "?#");
    char *pathc = xstrndup(path, pl);
    strvec_t segs = str_split(pathc, "/");
    free(pathc);
    sb_t b = SB_INIT;
    sb_append(&b, host);
    size_t n = 0;
    bool ok = true;
    for (size_t i = 0; i < segs.n; i++) {
        if (!*segs.v[i]) continue;
        if (!path_segment_ok(segs.v[i])) ok = false;
        sb_appendf(&b, "/%s", segs.v[i]);
        n++;
    }
    strvec_free(&segs);
    free(host);
    if (!ok || n < 2) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

/* resolveDefaultGitlabProjectMemoized: `glab repo view -F json` for cwd, memoized per cwd */
static pthread_mutex_t g_memo_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_memo; /* cwd -> project */

static char *default_project(const char *cwd, char **err) {
    pthread_mutex_lock(&g_memo_mu);
    const char *hit = json_get_str(g_memo, cwd);
    char *r = hit ? xstrdup(hit) : NULL;
    pthread_mutex_unlock(&g_memo_mu);
    if (r) return r;
    strvec_t a = {0};
    strvec_push(&a, "repo");
    strvec_push(&a, "view");
    strvec_push(&a, "-F");
    strvec_push(&a, "json");
    gh_res_t res;
    int rc = glab_run(cwd, &a, true, &res, err);
    if (rc == 0) {
        if (res.exit_code != 0) *err = glab_failure(&a, res.out, res.err);
        else if (!res.out || !*res.out) *err = xstrdup("GitLab CLI returned empty output.");
        else {
            json_t *v = json_parse(res.out, strlen(res.out), NULL);
            if (!json_is_obj(v)) *err = xstrdup("GitLab CLI returned invalid JSON output.");
            else {
                const char *web = json_get_str(v, "web_url");
                r = project_from_web_url(web);
                const char *http = json_get_str(v, "http_url_to_repo");
                if (!r && http) {
                    char *h = xstrdup(http);
                    size_t n = strlen(h);
                    if (n > 4 && !strcmp(h + n - 4, ".git")) h[n - 4] = 0;
                    r = project_from_web_url(h);
                    free(h);
                }
                if (!r) *err = xasprintf("GitLab CLI returned an unrecognized project URL: %s", web ? web : "undefined");
            }
            json_free(v);
        }
        gh_res_free(&res);
    }
    strvec_free(&a);
    if (r) {
        pthread_mutex_lock(&g_memo_mu);
        if (!g_memo) g_memo = json_obj();
        json_set_str(g_memo, cwd, r);
        pthread_mutex_unlock(&g_memo_mu);
    }
    return r;
}

/* getGlabConfigPath */
static char *glab_config_path(void) {
    const char *ov = getenv("GLAB_CONFIG_DIR");
    if (ov && *ov) return path_join(ov, "config.yml");
    const char *home = getenv("HOME") ? getenv("HOME") : "";
    char *first = path_join3(home, ".config/glab-cli", "config.yml");
    if (fs_exists(first)) return first;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char *second = xdg && *xdg ? path_join3(xdg, "glab-cli", "config.yml") : NULL;
#ifdef __APPLE__
    if (!second) second = path_join3(home, "Library/Application Support/glab-cli", "config.yml");
#endif
    if (second && fs_exists(second)) {
        free(first);
        return second;
    }
    free(second);
    return first;
}

static void add_trusted_host(strvec_t *t, const char *value) {
    char *v = gh_js_trim(value);
    if (!*v) {
        free(v);
        return;
    }
    const char *p = strstr(v, "://");
    p = p ? p + 3 : v;
    size_t n = strcspn(p, "/?#");
    char *h = xstrndup(p, n);
    for (char *c = h; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (*h && !strvec_contains(t, h)) strvec_push(t, h);
    free(h);
    free(v);
}

/* assertGitlabHostTrustedForEnvToken: GitLab token env vars only reach hosts the user already trusts */
static bool host_trusted(const char *host, const char *cwd, char **err) {
    static const char *const TOKENS[] = {"GITLAB_TOKEN", "GITLAB_ACCESS_TOKEN", "OAUTH_TOKEN"};
    bool any = false;
    for (size_t i = 0; i < ARRAY_LEN(TOKENS); i++) any |= getenv(TOKENS[i]) && *getenv(TOKENS[i]);
    if (!any) return true;
    strvec_t t = {0};
    strvec_push(&t, "gitlab.com");
    static const char *const HOSTS[] = {"GITLAB_HOST", "GITLAB_URI", "GL_HOST"};
    for (size_t i = 0; i < ARRAY_LEN(HOSTS); i++)
        if (getenv(HOSTS[i])) add_trusted_host(&t, getenv(HOSTS[i]));
    bool ok = strvec_contains(&t, host);
    if (!ok) {
        char *cp = glab_config_path();
        size_t len = 0;
        char *text = fs_read(cp, &len);
        free(cp);
        if (text) {
            json_t *cfg = yaml_parse(text, len, NULL);
            const char *k;
            json_t *v;
            json_obj_foreach(json_get_obj(cfg, "hosts"), i, k, v) {
                char *lk = xstrdup(k);
                for (char *c = lk; *c; c++) *c = (char)tolower((unsigned char)*c);
                if (!strvec_contains(&t, lk)) strvec_push(&t, lk);
                free(lk);
            }
            json_free(cfg);
            free(text);
        }
        ok = strvec_contains(&t, host);
    }
    if (!ok) {
        char *e = NULL;
        char *own = default_project(cwd, &e);
        free(e);
        if (own) {
            size_t hl = strcspn(own, "/");
            ok = strlen(host) == hl && !strncmp(own, host, hl);
            free(own);
        }
    }
    strvec_free(&t);
    if (!ok)
        *err = xasprintf("Refusing to send GitLab token environment variables to %s: it is not gitlab.com, GITLAB_HOST, a host in "
                         "glab's config, or this checkout's GitLab host. Run `glab auth login --hostname %s` or set "
                         "GITLAB_HOST to trust it.",
                         host, host);
    return ok;
}

/* fetchMrDiffFresh: `glab mr diff <iid> -R https://<project> --raw --color never` */
static char *fetch_diff(const char *cwd, const char *project, int64_t iid, char **err) {
    char *iids = xasprintf("%lld", (long long)iid);
    char *repo = xasprintf("https://%s", project);
    strvec_t a = {0};
    strvec_push(&a, "mr");
    strvec_push(&a, "diff");
    strvec_push(&a, iids);
    strvec_push(&a, "-R");
    strvec_push(&a, repo);
    strvec_t raw = {0};
    for (size_t i = 0; i < a.n; i++) strvec_push(&raw, a.v[i]);
    strvec_push(&raw, "--raw");
    strvec_push(&raw, "--color");
    strvec_push(&raw, "never");
    char *e = NULL;
    char *text = glab_text(cwd, &raw, false, &e);
    if (!text) {
        if (strstr(e, "unknown flag: --raw")) {
            free(e);
            e = xstrdup("GitLab CLI is too old for `glab mr diff --raw`; upgrade glab.");
        } else if (strstr(e, "could not obtain raw diff") && strstr(e, "404")) {
            /* instances without the raw_diffs endpoint: glab's rendered diff still carries `diff --git` headers */
            free(e);
            e = NULL;
            strvec_push(&a, "--color");
            strvec_push(&a, "never");
            text = glab_text(cwd, &a, false, &e);
        }
    }
    strvec_free(&raw);
    strvec_free(&a);
    free(iids);
    free(repo);
    if (!text) {
        *err = e;
        return NULL;
    }
    return text;
}

/* parseMrUrl over the URL after "mr://" (no selector) */
typedef struct {
    char *project; /* NULL for the short form */
    int64_t iid;
    int mode; /* 0 list, 1 all, 2 slice */
    int64_t index;
} mr_url_t;

static bool parse_mr(const char *rest, mr_url_t *u, char **err) {
    memset(u, 0, sizeof *u);
    size_t n = strcspn(rest, "?#");
    char *body = xstrndup(rest, n);
    strvec_t all = str_split(body, "/");
    free(body);
    /* splitInternalUrlSegments: empty, `.` and `..` path segments are unsafe */
    for (size_t i = 1; i < all.n; i++) {
        if (i == all.n - 1 && !*all.v[i] && all.n > 1) { /* `…/diff/` keeps a trailing empty segment */
            free(all.v[--all.n]);
            break;
        }
        if (!*all.v[i] || !strcmp(all.v[i], ".") || !strcmp(all.v[i], "..")) {
            *err = xstrdup("Invalid mr:// URL: empty or unsafe path segment");
            strvec_free(&all);
            return false;
        }
    }
    size_t last = all.n - 1;
    long iid_index;
    if (!strcmp(all.v[last], "diff")) {
        u->mode = 0;
        iid_index = (long)last - 1;
    } else if (all.n >= 2 && !strcmp(all.v[last - 1], "diff")) {
        iid_index = (long)last - 2;
        if (!strcmp(all.v[last], "all")) u->mode = 1;
        else {
            u->index = gh_parse_pos_int(all.v[last]);
            if (u->index < 0) {
                *err = xasprintf("Invalid mr:// diff sub-path '%s'. Use 'all' or a 1-indexed file number.", all.v[last]);
                strvec_free(&all);
                return false;
            }
            u->mode = 2;
        }
    } else {
        *err = xstrdup(INVALID_MR_URL);
        strvec_free(&all);
        return false;
    }
    u->iid = iid_index >= 0 ? gh_parse_pos_int(all.v[iid_index]) : -1;
    if (u->iid < 0) {
        *err = xstrdup(INVALID_MR_URL);
        strvec_free(&all);
        return false;
    }
    if (iid_index > 0) {
        bool ok = host_ok(all.v[0]) && iid_index - 1 >= 2;
        for (long i = 1; ok && i < iid_index; i++) ok = path_segment_ok(all.v[i]);
        if (!ok) {
            *err = xstrdup(INVALID_MR_URL);
            strvec_free(&all);
            return false;
        }
        sb_t b = SB_INIT;
        for (const char *c = all.v[0]; *c; c++) sb_putc(&b, (char)tolower((unsigned char)*c));
        for (long i = 1; i < iid_index; i++) sb_appendf(&b, "/%s", all.v[i]);
        u->project = sb_detach(&b);
    }
    strvec_free(&all);
    return true;
}

/* renderDiffFamilyResource */
static char *render(const char *project, int64_t iid, int mode, int64_t index, const char *text, char **err) {
    char *base = xasprintf("mr://%s/%lld", project, (long long)iid);
    gh_diff_files_t files;
    gh_parse_unified_diff(text, &files);
    char *fc = xasprintf("%zu file%s", files.n, files.n == 1 ? "" : "s");
    char *content = NULL;
    if (mode == 1) content = xstrdup(text);
    else if (mode == 2) {
        if (index < 1 || index > (int64_t)files.n)
            *err = xasprintf("%s/diff/%lld is out of range; MR has %s. Use %s/diff to list available indices.", base,
                             (long long)index, fc, base);
        else {
            const gh_diff_file_t *f = &files.v[index - 1];
            content = xstrndup(text + f->start, f->end - f->start);
        }
    } else {
        sb_t b = SB_INIT;
        sb_appendf(&b, "# Merge Request Diff: %s!%lld (%s)\n\n", project, (long long)iid, fc);
        if (!files.n) sb_append(&b, "_No file changes._");
        for (size_t i = 0; i < files.n; i++) {
            const gh_diff_file_t *f = &files.v[i];
            if (i) sb_append(&b, "\n\n");
            char *stats = !strcmp(f->change_type, "binary") ? xstrdup("(binary)")
                                                            : xasprintf("+%lld -%lld", (long long)f->additions, (long long)f->deletions);
            sb_appendf(&b, "%zu. %s  %s  [%s]", i + 1, f->path, stats, f->change_type);
            if (f->old_path) sb_appendf(&b, "  (renamed from %s)", f->old_path);
            sb_appendf(&b, "\n   %s/diff/%zu", base, i + 1);
            free(stats);
        }
        sb_appendf(&b, "\n\n---\nRead all: `%s/diff/all`. Each file is also available as `%s/diff/<i>`.", base, base);
        content = sb_detach(&b);
    }
    free(fc);
    gh_diff_files_free(&files);
    free(base);
    return content;
}

int mr_resolve(tool_env_t *env, const char *url, char **content, bool *markdown, char **err) {
    *content = NULL;
    *err = NULL;
    *markdown = false;
    mr_url_t u;
    if (!parse_mr(url + 5, &u, err)) return -1;
    *markdown = u.mode == 0;
    char *e = NULL;
    char *project = u.project ? xstrdup(u.project) : NULL;
    if (!project) {
        char *pe = NULL;
        project = default_project(env->cwd, &pe);
        if (!project)
            e = xasprintf("mr://%lld/diff could not resolve a default GitLab project from the current session: %s\nUse "
                          "mr://<host>/<group>/<project>/%lld/diff.",
                          (long long)u.iid, pe ? pe : "", (long long)u.iid);
        free(pe);
    }
    if (project) {
        size_t hl = strcspn(project, "/");
        char *host = xstrndup(project, hl);
        if (host_trusted(host, env->cwd, &e)) {
            char *text = fetch_diff(env->cwd, project, u.iid, &e);
            if (text) {
                gh_diff_files_t files;
                gh_parse_unified_diff(text, &files);
                bool blank = true;
                for (const char *c = text; *c && blank; c++) blank = isspace((unsigned char)*c);
                if (!files.n && !blank)
                    e = xstrdup("GitLab CLI returned a diff without `diff --git` file headers; upgrade glab.");
                else *content = render(project, u.iid, u.mode, u.index, text, &e);
                gh_diff_files_free(&files);
                free(text);
            }
        }
        free(host);
    }
    free(project);
    free(u.project);
    if (*content) return 0;
    *err = xasprintf("mr:// diff resolution failed: %s", e ? e : "unknown error");
    free(e);
    return -1;
}
