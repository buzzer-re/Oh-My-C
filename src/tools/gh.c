/* gh.c - GitHub CLI runner and the shared issue/PR view + diff helpers (ports of omp's utils/github.ts,
 * utils/forge-cli.ts, tools/gh-common.ts, tools/gh-view.ts, tools/gh-pr-diff.ts). The `github` tool itself is
 * gh_tool.c; pr:// and issue:// are github.c. Output text is byte-identical to omp's. */
#include "gh.h"
#include "gh_int.h"

#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"

#include <ctype.h>
#include <pthread.h>

/* ---------- JS string helpers ---------- */

/* ECMAScript WhiteSpace + LineTerminator code points (String.prototype.trim) */
static size_t js_ws_len(const char *s, size_t avail) {
    unsigned char c = (unsigned char)s[0];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') return 1;
    if (c < 0x80 || avail < 2) return 0;
    uint32_t cp;
    int k = utf8_decode(s, avail, &cp);
    if (k <= 0) return 0;
    if (cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
        cp == 0x205F || cp == 0x3000 || cp == 0xFEFF)
        return (size_t)k;
    return 0;
}

static size_t js_ws_end(const char *s, size_t n) {
    /* length after trimming trailing white space */
    while (n > 0) {
        size_t start = n - 1;
        while (start > 0 && ((unsigned char)s[start] & 0xC0) == 0x80) start--;
        size_t k = js_ws_len(s + start, n - start);
        if (k != n - start) break;
        n = start;
    }
    return n;
}

char *gh_js_trim(const char *s) {
    if (!s) return xstrdup("");
    size_t n = strlen(s), i = 0, k;
    while (i < n && (k = js_ws_len(s + i, n - i))) i += k;
    size_t e = js_ws_end(s + i, n - i);
    return xstrndup(s + i, e);
}

char *gh_js_trim_end(const char *s) {
    if (!s) return xstrdup("");
    return xstrndup(s, js_ws_end(s, strlen(s)));
}

static char *norm_ws(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\r') {
            sb_putc(&b, '\n');
            if (p[1] == '\n') p++;
        } else if (*p == '\t') {
            sb_append(&b, "    ");
        } else {
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

char *gh_norm_text(const char *s) {
    char *w = norm_ws(s);
    char *r = gh_js_trim(w);
    free(w);
    return r;
}

char *gh_norm_block(const char *s) {
    char *w = norm_ws(s);
    char *r = gh_js_trim_end(w);
    free(w);
    return r;
}

char *gh_norm_opt(const char *s) {
    if (!s) return NULL;
    char *t = gh_js_trim(s);
    if (!*t) {
        free(t);
        return NULL;
    }
    return t;
}

int64_t gh_parse_pos_int(const char *s) {
    if (!s || !*s) return -1;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p)) return -1;
    if (strlen(s) > 16) return -1; /* beyond Number.MAX_SAFE_INTEGER digits */
    int64_t v = strtoll(s, NULL, 10);
    if (v <= 0 || v > 9007199254740991LL) return -1;
    return v;
}

/* encodeURIComponent */
char *gh_uri_component(const char *s) {
    sb_t b = SB_INIT;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("-_.!~*'()", *p)) sb_putc(&b, (char)*p);
        else sb_appendf(&b, "%%%02X", *p);
    }
    return sb_detach(&b);
}

/* JS `${value}` of a JSON value; NULL (undefined) -> "undefined" */
char *gh_jsv(const json_t *v) {
    if (!v) return xstrdup("undefined");
    switch (v->type) {
    case JSON_NULL: return xstrdup("null");
    case JSON_BOOL: return xstrdup(v->u.b ? "true" : "false");
    case JSON_STRING: return xstrdup(v->u.str.s);
    case JSON_NUMBER: return json_dump(v, 0);
    case JSON_ARRAY: {
        sb_t b = SB_INIT;
        for (size_t i = 0; i < json_len(v); i++) {
            if (i) sb_putc(&b, ',');
            const json_t *e = json_at(v, i);
            if (e && e->type != JSON_NULL) {
                char *s = gh_jsv(e);
                sb_append(&b, s);
                free(s);
            }
        }
        return sb_detach(&b);
    }
    default: return xstrdup("[object Object]");
    }
}

/* JS truthiness of a JSON value (undefined = false) */
bool gh_truthy(const json_t *v) {
    if (!v) return false;
    switch (v->type) {
    case JSON_NULL: return false;
    case JSON_BOOL: return v->u.b;
    case JSON_STRING: return v->u.str.s[0] != 0;
    case JSON_NUMBER: return v->is_int ? v->u.num.i != 0 : (v->u.num.d != 0 && v->u.num.d == v->u.num.d);
    default: return true;
    }
}

/* `v ?? undefined`: JSON null reads as missing */
const json_t *gh_nn(const json_t *v) { return v && v->type == JSON_NULL ? NULL : v; }

/* optional chaining `obj?.key` */
const json_t *gh_oc(const json_t *obj, const char *key) {
    return obj && obj->type == JSON_OBJECT ? json_get(obj, key) : NULL;
}

/* pushLine(lines, label, value): skip undefined and "" */
void gh_push(strvec_t *L, const char *label, const json_t *v) {
    if (!v) return;
    if (v->type == JSON_STRING && !v->u.str.s[0]) return;
    char *s = gh_jsv(v);
    strvec_push_own(L, xasprintf("%s: %s", label, s));
    free(s);
}

void gh_push_s(strvec_t *L, const char *label, const char *v) {
    if (!v || !*v) return;
    strvec_push_own(L, xasprintf("%s: %s", label, v));
}

void gh_push_i(strvec_t *L, const char *label, int64_t v) { strvec_push_own(L, xasprintf("%s: %lld", label, (long long)v)); }

void gh_push_b(strvec_t *L, const char *label, bool v) { strvec_push_own(L, xasprintf("%s: %s", label, v ? "true" : "false")); }

/* lines.join("\n").trim() */
char *gh_join_trim(const strvec_t *L) {
    char *j = strvec_join(L, "\n");
    char *t = gh_js_trim(j);
    free(j);
    return t;
}

/* formatShortSha: first 12 characters or NULL when missing/empty (owned) */
char *gh_short_sha(const char *v) {
    if (!v || !*v) return NULL;
    return xstrndup(v, 12);
}

/* formatAuthor (owned or NULL) */
char *gh_author(const json_t *a) {
    if (!a || a->type != JSON_OBJECT) return NULL;
    const json_t *login = json_get(a, "login");
    if (gh_truthy(login)) {
        char *s = gh_jsv(login), *r = xasprintf("@%s", s);
        free(s);
        return r;
    }
    const json_t *name = json_get(a, "name");
    if (gh_truthy(name)) return gh_jsv(name);
    return NULL;
}

/* formatLabels (owned or NULL) */
char *gh_labels(const json_t *labels) {
    if (!json_is_arr(labels)) return NULL;
    strvec_t names = {0};
    for (size_t i = 0; i < json_len(labels); i++) {
        const json_t *n = gh_oc(json_at(labels, i), "name");
        if (gh_truthy(n)) strvec_push_own(&names, gh_jsv(n));
    }
    char *r = names.n ? strvec_join(&names, ", ") : NULL;
    strvec_free(&names);
    return r;
}

/* ---------- gh runner (forge-cli.ts runForgeCli + utils/github.ts) ---------- */

#define GH_TIMEOUT_MS (5 * 60 * 1000)
#define GH_OUTPUT_LIMIT (8u * 1024 * 1024)

bool gh_available(void) {
    char *p = which("gh");
    bool ok = p != NULL;
    free(p);
    return ok;
}

void gh_res_free(gh_res_t *r) {
    free(r->out);
    free(r->err);
    memset(r, 0, sizeof *r);
}

static char *args_join(const strvec_t *args) {
    return strvec_join(args, " ");
}

int gh_run(const char *cwd, const strvec_t *args, bool trim, gh_res_t *r, char **err) {
    memset(r, 0, sizeof *r);
    char *bin = which("gh");
    if (!bin) {
        *err = xstrdup("GitHub CLI (gh) is not installed. Install it from https://cli.github.com/.");
        return -1;
    }
    free(bin);
    char *fals = which("false");
    char *ssh_askpass = xasprintf("SSH_ASKPASS=%s", fals ? fals : "false");
    free(fals);
    /* FORGE_NON_INTERACTIVE_ENV + GH_PROMPT_DISABLED; a bare name unsets the variable */
    const char *env[] = {"GIT_ASKPASS=true", "GIT_EDITOR=true", "GIT_TERMINAL_PROMPT=0", "LC_ALL", "LC_MESSAGES=C",
                         ssh_askpass, "GH_PROMPT_DISABLED=1", NULL};
    const char **argv = xcalloc(args->n + 2, sizeof(char *));
    argv[0] = "gh";
    for (size_t i = 0; i < args->n; i++) argv[i + 1] = args->v[i];
    proc_opts_t o = {0};
    o.cwd = cwd;
    o.env_extra = env;
    o.timeout_ms = GH_TIMEOUT_MS;
    o.max_output = GH_OUTPUT_LIMIT;
    o.new_session = true;
    proc_result_t pr;
    int rc = proc_run(argv, &o, &pr);
    free(argv);
    free(ssh_askpass);
    if (rc != 0 || pr.spawn_errno) {
        *err = xasprintf("GitHub CLI command failed: gh %s", "");
        proc_result_free(&pr);
        return -1;
    }
    if (pr.timed_out) {
        char *j = args_join(args);
        *err = xasprintf("GitHub CLI command timed out: gh %s", j);
        free(j);
        proc_result_free(&pr);
        return -1;
    }
    sb_t out = SB_INIT, er = SB_INIT;
    sb_appendn(&out, pr.out ? pr.out : "", pr.out_len);
    sb_appendn(&er, pr.err ? pr.err : "", pr.err_len);
    if (pr.truncated && pr.out_len >= GH_OUTPUT_LIMIT) {
        sb_append(&out, "\n[gh subprocess output truncated after 8 MiB]\n");
        r->out_truncated = true;
    }
    if (pr.truncated && pr.err_len >= GH_OUTPUT_LIMIT) sb_append(&er, "\n[gh subprocess output truncated after 8 MiB]\n");
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

static char *format_failure(const strvec_t *args, const char *out, const char *er, bool repo_provided) {
    char *message = gh_js_trim(er && *er ? er : out);
    if (strstr(message, "gh auth login") || strstr(message, "not logged into any GitHub hosts")) {
        free(message);
        return xstrdup("GitHub CLI is not authenticated. Run `gh auth login`.");
    }
    if (!repo_provided && (strstr(message, "not a git repository") || strstr(message, "no git remotes found") ||
                           strstr(message, "unable to determine current repository"))) {
        free(message);
        return xstrdup("GitHub repository context is unavailable. Pass `repo` explicitly or run the tool inside a GitHub checkout.");
    }
    if (*message) return message;
    free(message);
    char *j = args_join(args);
    char *r = xasprintf("GitHub CLI command failed: gh %s", j);
    free(j);
    return r;
}

/* describeGitHubApiError (owned or NULL) */
static char *describe_api_error(const json_t *v) {
    if (json_is_str(v)) return gh_norm_opt(json_sv(v)) ? gh_js_trim(json_sv(v)) : NULL;
    if (!json_is_obj(v)) return NULL;
    if (json_is_str(json_get(v, "message"))) {
        char *t = gh_js_trim(json_get_str(v, "message"));
        if (*t) return t;
        free(t);
        return NULL;
    }
    char *resource = json_is_str(json_get(v, "resource")) ? gh_js_trim(json_get_str(v, "resource")) : xstrdup("");
    char *field = json_is_str(json_get(v, "field")) ? gh_js_trim(json_get_str(v, "field")) : xstrdup("");
    char *target = *resource && *field ? xasprintf("%s.%s", resource, field) : xstrdup(*resource ? resource : field);
    char *code = json_is_str(json_get(v, "code")) ? gh_js_trim(json_get_str(v, "code")) : xstrdup("");
    char *r = NULL;
    if (*target && *code) r = xasprintf("%s: %s", target, code);
    else if (*target) r = xstrdup(target);
    else if (*code) r = xstrdup(code);
    free(resource);
    free(field);
    free(target);
    free(code);
    return r;
}

static char *format_json_failure(const strvec_t *args, const char *out, const char *er, bool repo_provided) {
    char *raw = gh_js_trim(er && *er ? er : out);
    char *fallback = format_failure(args, out, er, repo_provided);
    if (strcmp(fallback, raw) != 0) {
        free(raw);
        return fallback;
    }
    free(raw);
    json_t *payload = json_parse(out, strlen(out), NULL);
    strvec_t msgs = {0};
    if (json_is_obj(payload)) {
        char *s = describe_api_error(json_get(payload, "message"));
        if (s) strvec_push_own(&msgs, s);
        json_t *errs = json_get_arr(payload, "errors");
        for (size_t i = 0; i < json_len(errs); i++) {
            char *m = describe_api_error(json_at(errs, i));
            if (m && !strvec_contains(&msgs, m)) strvec_push_own(&msgs, m);
            else free(m);
        }
    }
    json_free(payload);
    sb_t b = SB_INIT;
    size_t kept = 0;
    for (size_t i = 0; i < msgs.n; i++) {
        if (strstr(fallback, msgs.v[i])) continue;
        sb_appendf(&b, "%s- %s", kept++ ? "\n" : "", msgs.v[i]);
    }
    strvec_free(&msgs);
    if (!kept) {
        sb_free(&b);
        return fallback;
    }
    char *r = xasprintf("%s\nGitHub details:\n%s", fallback, sb_cstr(&b));
    sb_free(&b);
    free(fallback);
    return r;
}

json_t *gh_json(const char *cwd, const strvec_t *args, bool repo_provided, bool trim, char **err) {
    gh_res_t r;
    if (gh_run(cwd, args, trim, &r, err) != 0) return NULL;
    json_t *j = NULL;
    if (r.exit_code != 0) *err = format_json_failure(args, r.out, r.err, repo_provided);
    else if (!*r.out) *err = xstrdup("GitHub CLI returned empty output.");
    else if (!(j = json_parse(r.out, strlen(r.out), NULL))) *err = xstrdup("GitHub CLI returned invalid JSON output.");
    gh_res_free(&r);
    return j;
}

char *gh_text(const char *cwd, const strvec_t *args, bool repo_provided, bool trim, char **err) {
    gh_res_t r;
    if (gh_run(cwd, args, trim, &r, err) != 0) return NULL;
    char *t = NULL;
    if (r.exit_code != 0) *err = format_failure(args, r.out, r.err, repo_provided);
    else t = xstrdup(r.out);
    gh_res_free(&r);
    return t;
}

/* strvec from a NULL-terminated list */
strvec_t gh_args(const char *first, ...) {
    strvec_t v = {0};
    va_list ap;
    va_start(ap, first);
    for (const char *s = first; s; s = va_arg(ap, const char *)) strvec_push(&v, s);
    va_end(ap);
    return v;
}

/* ---------- repo refs (gh-common.ts) ---------- */

char *gh_default_host(void) {
    const char *h = env_get("GH_HOST");
    char *r = xstrdup(h && *h ? h : "github.com");
    for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}

void gh_parse_ref(const char *repo, gh_ref_t *ref) {
    memset(ref, 0, sizeof *ref);
    const char *a = strchr(repo, '/');
    const char *b = a ? strchr(a + 1, '/') : NULL;
    if (!a || !b || strchr(b + 1, '/')) {
        ref->slug = xstrdup(repo);
        return;
    }
    ref->host = xstrndup(repo, (size_t)(a - repo));
    ref->slug = xstrdup(a + 1);
}

void gh_ref_free(gh_ref_t *ref) {
    free(ref->host);
    free(ref->slug);
}

char *gh_format_ref(const char *host, const char *slug) { return host ? xasprintf("%s/%s", host, slug) : xstrdup(slug); }

void gh_api_host_args(strvec_t *args, const gh_ref_t *ref) {
    if (ref->host) {
        strvec_push(args, "--hostname");
        strvec_push(args, ref->host);
    }
}

/* repoFromUrl: https?://HOST/OWNER/REPO... */
char *gh_repo_from_url(const char *value) {
    char *v = gh_js_trim(value);
    const char *p = v;
    if (str_starts(p, "https://")) p += 8;
    else if (str_starts(p, "http://")) p += 7;
    else {
        free(v);
        return NULL;
    }
    size_t hl = strcspn(p, "/");
    if (!hl || p[hl] != '/') {
        free(v);
        return NULL;
    }
    const char *o = p + hl + 1;
    size_t ol = strcspn(o, "/");
    if (!ol || o[ol] != '/') {
        free(v);
        return NULL;
    }
    const char *r = o + ol + 1;
    size_t rl = strcspn(r, "/?#");
    if (!rl) {
        free(v);
        return NULL;
    }
    char *host = xstrndup(p, hl);
    for (char *q = host; *q; q++) *q = (char)tolower((unsigned char)*q);
    char *slug = xasprintf("%.*s/%.*s", (int)ol, o, (int)rl, r);
    char *def = gh_default_host();
    char *res = strcmp(host, def) ? xasprintf("%s/%s", host, slug) : xstrdup(slug);
    free(def);
    free(host);
    free(slug);
    free(v);
    return res;
}

/* parse https://HOST/OWNER/REPO/<kind>/N(/...)? ; returns number or -1 */
int64_t gh_parse_item_url(const char *value, const char *kind, char **repo) {
    *repo = NULL;
    char *v = gh_norm_opt(value);
    if (!v) return -1;
    int64_t n = -1;
    if (str_starts(v, "https://")) {
        const char *p = v + 8;
        size_t hl = strcspn(p, "/");
        const char *o = p + hl;
        if (hl && *o == '/') {
            o++;
            size_t ol = strcspn(o, "/");
            const char *r = o + ol;
            if (ol && *r == '/') {
                r++;
                size_t rl = strcspn(r, "/");
                const char *k = r + rl;
                size_t kl = strlen(kind);
                if (rl && *k == '/' && !strncmp(k + 1, kind, kl) && k[1 + kl] == '/') {
                    const char *d = k + 2 + kl;
                    size_t dl = strspn(d, "0123456789");
                    if (dl && (d[dl] == 0 || d[dl] == '/')) {
                        n = strtoll(d, NULL, 10);
                        char *host = xstrndup(p, hl), *slug = xasprintf("%.*s/%.*s", (int)ol, o, (int)rl, r);
                        *repo = gh_format_ref(host, slug);
                        free(host);
                        free(slug);
                    }
                }
            }
        }
    }
    free(v);
    return n;
}

static bool host_eq_effective(const gh_ref_t *a, const gh_ref_t *b) {
    char *def = gh_default_host();
    char *ha = xstrdup(a->host ? a->host : def), *hb = xstrdup(b->host ? b->host : def);
    for (char *p = ha; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char *p = hb; *p; p++) *p = (char)tolower((unsigned char)*p);
    bool eq = !strcmp(ha, hb);
    free(ha);
    free(hb);
    free(def);
    return eq;
}

bool gh_repo_slug_equals(const char *left, const char *right) {
    if (!left) return false;
    gh_ref_t l, r;
    gh_parse_ref(left, &l);
    gh_parse_ref(right, &r);
    bool eq = host_eq_effective(&l, &r) && !strcasecmp(l.slug, r.slug);
    gh_ref_free(&l);
    gh_ref_free(&r);
    return eq;
}

/* the output of `git -C cwd <args>` trimmed, owned; NULL on failure or nothing */
static char *git_text(const char *cwd, const char *const *argv) {
    char *r = proc_capture(argv, cwd, 10000);
    if (r && !*r) {
        free(r);
        return NULL;
    }
    return r;
}

/* unrelatedOrigin: the `origin` repository when `gh` picked an unrelated remote over it, else NULL. Without `gh repo
 * set-default`, `gh` takes the first remote ranked `upstream` > `github` > `origin`, assuming `upstream` is the parent of an
 * `origin` fork. When `origin` is not a fork of that pick (`upstream` is a project merged from, not contributed to), PRs
 * and issues live on `origin`. When git or `gh` cannot answer (an `origin` on a non-GitHub host, behind an SSH alias) the
 * pick stays. */
static char *unrelated_origin(const char *cwd, const char *picked) {
    const char *ls[] = {"git", "remote", NULL};
    char *list = git_text(cwd, ls);
    if (!list) return NULL;
    strvec_t remotes = str_split(list, "\n");
    free(list);
    bool has_origin = false, outranks = false, resolved = false;
    for (size_t i = 0; i < remotes.n; i++) {
        const char *n = remotes.v[i];
        if (!strcmp(n, "origin")) has_origin = true;
        if (!strcasecmp(n, "upstream") || !strcasecmp(n, "github")) outranks = true;
    }
    /* `gh repo set-default` marks the chosen remote `gh-resolved` */
    for (size_t i = 0; has_origin && outranks && i < remotes.n && !resolved; i++) {
        char *key = xasprintf("remote.%s.gh-resolved", remotes.v[i]);
        const char *cfg[] = {"git", "config", "--get", key, NULL};
        char *v = git_text(cwd, cfg);
        resolved = v != NULL;
        free(v);
        free(key);
    }
    bool go = has_origin && outranks && !resolved;
    strvec_free(&remotes);
    if (!go) return NULL;
    const char *get[] = {"git", "remote", "get-url", "origin", NULL};
    char *origin_url = git_text(cwd, get);
    if (!origin_url) return NULL;
    strvec_t a = gh_args("repo", "view", origin_url, "--json", "url,parent", NULL);
    char *err = NULL;
    json_t *view = gh_json(cwd, &a, false, false, &err);
    strvec_free(&a);
    free(err);
    free(origin_url);
    if (!view) return NULL;
    const char *url = json_get_str(view, "url");
    char *origin = url ? gh_repo_from_url(url) : NULL;
    if (origin && gh_repo_slug_equals(origin, picked)) {
        free(origin);
        origin = NULL;
    }
    const json_t *parent = json_get_obj(view, "parent");
    if (origin && parent) {
        const char *pname = json_get_str(parent, "name"), *plogin = json_get_str(json_get_obj(parent, "owner"), "login");
        if (pname && plogin) {
            gh_ref_t ref;
            gh_parse_ref(origin, &ref);
            char *slug = xasprintf("%s/%s", plogin, pname), *pref = gh_format_ref(ref.host, slug);
            gh_ref_free(&ref);
            if (gh_repo_slug_equals(pref, picked)) {
                free(origin);
                origin = NULL;
            }
            free(slug);
            free(pref);
        }
    }
    json_free(view);
    return origin;
}

/* resolveRepoFromCwd */
char *gh_repo_from_cwd(const char *cwd, char **err) {
    strvec_t a = gh_args("repo", "view", "--json", "url", "-q", ".url", NULL);
    char *url = gh_text(cwd, &a, false, true, err);
    strvec_free(&a);
    if (!url) return NULL;
    char *norm = gh_norm_opt(url);
    if (!norm) {
        free(url);
        *err = xstrdup("repo must not be empty");
        return NULL;
    }
    char *repo = gh_repo_from_url(norm);
    if (!repo) *err = xasprintf("GitHub CLI returned an unrecognized repository URL: %s", norm);
    free(norm);
    free(url);
    if (repo) {
        char *origin = unrelated_origin(cwd, repo);
        if (origin) {
            free(repo);
            repo = origin;
        }
    }
    return repo;
}

static pthread_mutex_t g_memo_mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_memo; /* abs cwd -> repo */

char *gh_default_repo(const char *cwd, char **err) {
    char *key = path_abs(cwd, cwd);
    pthread_mutex_lock(&g_memo_mu);
    const char *hit = g_memo ? json_get_str(g_memo, key) : NULL;
    char *r = hit ? xstrdup(hit) : NULL;
    pthread_mutex_unlock(&g_memo_mu);
    if (!r) {
        r = gh_repo_from_cwd(cwd, err);
        if (r) {
            pthread_mutex_lock(&g_memo_mu);
            if (!g_memo) g_memo = json_obj();
            json_set_str(g_memo, key, r);
            pthread_mutex_unlock(&g_memo_mu);
        }
    }
    free(key);
    return r;
}

/* resolveGitHubRepo */
char *gh_resolve_repo(const char *cwd, const char *repo, const char *run_repo, char **err) {
    if (repo && run_repo && !gh_repo_slug_equals(repo, run_repo)) {
        *err = xstrdup("run URL repository does not match the provided repo");
        return NULL;
    }
    if (repo) return xstrdup(repo);
    if (run_repo) return xstrdup(run_repo);
    return gh_repo_from_cwd(cwd, err);
}

/* ---------- cache identity (github-cache.ts; omc keeps no view cache) ---------- */

static char *gh_config_dir(void) {
    const char *d = env_get("GH_CONFIG_DIR");
    if (d && *d) return xstrdup(d);
    const char *x = env_get("XDG_CONFIG_HOME");
    if (x && *x) return path_join(x, "gh");
    return path_expand_home("~/.config/gh");
}

gh_cache_status_t gh_cache_status(void) {
    if (!settings_bool("github.cache.enabled", true)) return GH_CACHE_DISABLED;
    const char *vars[] = {"GH_TOKEN", "GITHUB_TOKEN", "GH_ENTERPRISE_TOKEN", "GITHUB_ENTERPRISE_TOKEN"};
    for (size_t i = 0; i < ARRAY_LEN(vars); i++) {
        const char *v = env_get(vars[i]);
        if (v && *v) return GH_CACHE_MISS;
    }
    char *dir = gh_config_dir();
    char *hosts = path_join(dir, "hosts.yml");
    bool has = fs_is_file(hosts);
    free(hosts);
    free(dir);
    return has ? GH_CACHE_MISS : GH_CACHE_DISABLED;
}

const char *gh_freshness_note(gh_cache_status_t st) {
    return st == GH_CACHE_DISABLED ? "Cache disabled; fetched live" : "Fetched live";
}

/* ---------- views (gh-view.ts) ---------- */

static const char *GH_ISSUE_FIELDS = "author,body,comments,createdAt,labels,number,state,stateReason,title,updatedAt,url";
static const char *GH_ISSUE_FIELDS_NO_COMMENTS = "author,body,createdAt,labels,number,state,stateReason,title,updatedAt,url";
static const char *GH_PR_FIELDS = "author,baseRefName,body,comments,createdAt,files,headRefName,isDraft,labels,"
                                  "mergeStateStatus,number,reviews,reviewDecision,state,title,updatedAt,url";
static const char *GH_PR_FIELDS_NO_COMMENTS = "author,baseRefName,body,createdAt,files,headRefName,isDraft,labels,"
                                              "mergeStateStatus,number,reviews,reviewDecision,state,title,updatedAt,url";

/* appendRepoFlag */
void gh_append_repo_flag(strvec_t *args, const char *repo, const char *identifier) {
    if (!repo || (identifier && str_starts(identifier, "https://"))) return;
    strvec_push(args, "--repo");
    strvec_push(args, repo);
}

json_t *gh_issue_json(const char *cwd, const strvec_t *args, bool repo_provided, char **err) {
    json_t *j = gh_json(cwd, args, repo_provided, true, err);
    if (j || !*err) return j;
    if (!strstr(*err, "Unknown JSON field") || !strstr(*err, "stateReason")) return NULL;
    /* drop stateReason from --json and retry */
    strvec_t retry = {0};
    bool dropped = false;
    for (size_t i = 0; i < args->n; i++) {
        if (i > 0 && !strcmp(args->v[i - 1], "--json")) {
            strvec_t f = str_split(args->v[i], ",");
            strvec_t kept = {0};
            for (size_t k = 0; k < f.n; k++) {
                if (!strcmp(f.v[k], "stateReason")) dropped = true;
                else strvec_push(&kept, f.v[k]);
            }
            strvec_push_own(&retry, strvec_join(&kept, ","));
            strvec_free(&kept);
            strvec_free(&f);
        } else {
            strvec_push(&retry, args->v[i]);
        }
    }
    if (dropped) {
        free(*err);
        *err = NULL;
        j = gh_json(cwd, &retry, repo_provided, true, err);
    }
    strvec_free(&retry);
    return j;
}

static void push_section(strvec_t *L, strvec_t *sec) {
    if (!sec->n) return;
    strvec_push(L, "");
    for (size_t i = 0; i < sec->n; i++) strvec_push(L, sec->v[i]);
}

static void comments_section(const json_t *comments, strvec_t *L) {
    if (!json_is_arr(comments) || !json_len(comments)) return;
    size_t visible = 0, n = json_len(comments);
    for (size_t i = 0; i < n; i++)
        if (!gh_truthy(gh_oc(json_at(comments, i), "isMinimized"))) visible++;
    size_t hidden = n - visible;
    if (!visible) {
        strvec_push(L, "## Comments");
        strvec_push(L, "");
        strvec_push_own(L, xasprintf("No visible comments. Minimized comments omitted: %zu.", hidden));
        return;
    }
    strvec_push_own(L, xasprintf("## Comments (%zu)", visible));
    strvec_push(L, "");
    for (size_t i = 0; i < n; i++) {
        const json_t *c = json_at(comments, i);
        if (gh_truthy(gh_oc(c, "isMinimized"))) continue;
        char *author = gh_author(gh_oc(c, "author"));
        const json_t *created = gh_oc(c, "createdAt");
        char *cs = gh_truthy(created) ? gh_jsv(created) : NULL;
        strvec_push_own(L, xasprintf("### %s%s%s", author ? author : "unknown", cs ? " · " : "", cs ? cs : ""));
        free(cs);
        free(author);
        strvec_push(L, "");
        char *body = gh_norm_text(json_sv(gh_oc(c, "body")));
        strvec_push(L, *body ? body : "No comment body.");
        free(body);
        const json_t *url = gh_oc(c, "url");
        if (gh_truthy(url)) {
            strvec_push(L, "");
            char *u = gh_jsv(url);
            strvec_push_own(L, xasprintf("URL: %s", u));
            free(u);
        }
        strvec_push(L, "");
    }
    if (hidden) strvec_push_own(L, xasprintf("Minimized comments omitted: %zu.", hidden));
}

static void reviews_section(const json_t *reviews, strvec_t *L) {
    if (!json_is_arr(reviews) || !json_len(reviews)) return;
    strvec_push_own(L, xasprintf("## Reviews (%zu)", json_len(reviews)));
    strvec_push(L, "");
    for (size_t i = 0; i < json_len(reviews); i++) {
        const json_t *r = json_at(reviews, i);
        char *author = gh_author(gh_oc(r, "author"));
        const json_t *sub = gh_oc(r, "submittedAt"), *state = gh_oc(r, "state");
        char *ss = gh_truthy(sub) ? gh_jsv(sub) : NULL, *st = gh_truthy(state) ? gh_jsv(state) : NULL;
        strvec_push_own(L, xasprintf("### %s%s%s%s%s%s", author ? author : "unknown", ss ? " - " : "", ss ? ss : "",
                                     st ? " [" : "", st ? st : "", st ? "]" : ""));
        free(ss);
        free(st);
        free(author);
        const json_t *oid = gh_oc(gh_oc(r, "commit"), "oid");
        if (gh_truthy(oid)) {
            strvec_push(L, "");
            char *o = gh_jsv(oid);
            strvec_push_own(L, xasprintf("Commit: %.12s", o));
            free(o);
        }
        strvec_push(L, "");
        char *body = gh_norm_text(json_sv(gh_oc(r, "body")));
        strvec_push(L, *body ? body : "No review body.");
        free(body);
        strvec_push(L, "");
    }
}

/* review comments are normalized GhPrReviewComment objects (see fetch_review_comments) */
static void review_comments_section(const json_t *rc, strvec_t *L) {
    if (!json_is_arr(rc) || !json_len(rc)) return;
    strvec_push_own(L, xasprintf("## Review Comments (%zu)", json_len(rc)));
    strvec_push(L, "");
    for (size_t i = 0; i < json_len(rc); i++) {
        const json_t *c = json_at(rc, i);
        char *author = gh_author(gh_oc(c, "author"));
        const char *created = json_get_str(c, "createdAt");
        strvec_push_own(L, xasprintf("### %s%s%s", author ? author : "unknown", created ? " · " : "", created ? created : ""));
        free(author);
        strvec_push(L, "");
        const char *path = json_get_str(c, "path");
        if (path) {
            const json_t *line = json_get(c, "line");
            if (!line) line = json_get(c, "originalLine");
            if (line) strvec_push_own(L, xasprintf("Location: %s:%lld", path, (long long)json_get_int(c, line == json_get(c, "line") ? "line" : "originalLine", 0)));
            else strvec_push_own(L, xasprintf("Location: %s", path));
        }
        gh_push_s(L, "Side", json_get_str(c, "side"));
        if (json_get(c, "inReplyToId")) gh_push_i(L, "Reply to", json_get_int(c, "inReplyToId", 0));
        gh_push_s(L, "URL", json_get_str(c, "url"));
        strvec_push(L, "");
        char *body = gh_norm_text(json_sv(gh_oc(c, "body")));
        strvec_push(L, *body ? body : "No review comment body.");
        free(body);
        strvec_push(L, "");
    }
}

char *gh_format_issue(const json_t *d, const char *issue, bool comments) {
    strvec_t L = {0};
    const json_t *num = gh_nn(json_get(d, "number")), *title = gh_nn(json_get(d, "title"));
    char *ns = num ? gh_jsv(num) : xstrdup(issue), *ts = title ? gh_jsv(title) : xstrdup("Untitled");
    strvec_push_own(&L, xasprintf("# Issue #%s: %s", ns, ts));
    free(ns);
    free(ts);
    strvec_push(&L, "");
    gh_push(&L, "State", json_get(d, "state"));
    gh_push(&L, "State reason", gh_nn(json_get(d, "stateReason")));
    char *a = gh_author(json_get(d, "author"));
    gh_push_s(&L, "Author", a);
    free(a);
    gh_push(&L, "Created", json_get(d, "createdAt"));
    gh_push(&L, "Updated", json_get(d, "updatedAt"));
    char *lb = gh_labels(json_get(d, "labels"));
    gh_push_s(&L, "Labels", lb);
    free(lb);
    gh_push(&L, "URL", json_get(d, "url"));
    strvec_push(&L, "");
    strvec_push(&L, "## Body");
    strvec_push(&L, "");
    char *body = gh_norm_text(json_sv(json_get(d, "body")));
    strvec_push(&L, *body ? body : "No description provided.");
    free(body);
    if (comments && gh_truthy(json_get(d, "comments"))) {
        strvec_t sec = {0};
        comments_section(json_get(d, "comments"), &sec);
        push_section(&L, &sec);
        strvec_free(&sec);
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

#define FILE_PREVIEW_LIMIT 50

char *gh_format_pr(const json_t *d, const char *pr, bool comments) {
    strvec_t L = {0};
    const json_t *num = gh_nn(json_get(d, "number")), *title = gh_nn(json_get(d, "title"));
    char *ns = num ? gh_jsv(num) : xstrdup(pr ? pr : "current"), *ts = title ? gh_jsv(title) : xstrdup("Untitled");
    strvec_push_own(&L, xasprintf("# Pull Request #%s: %s", ns, ts));
    free(ns);
    free(ts);
    strvec_push(&L, "");
    gh_push(&L, "State", json_get(d, "state"));
    gh_push(&L, "Draft", json_get(d, "isDraft"));
    char *a = gh_author(json_get(d, "author"));
    gh_push_s(&L, "Author", a);
    free(a);
    gh_push(&L, "Base", json_get(d, "baseRefName"));
    gh_push(&L, "Head", json_get(d, "headRefName"));
    gh_push(&L, "Review decision", gh_nn(json_get(d, "reviewDecision")));
    gh_push(&L, "Merge state", json_get(d, "mergeStateStatus"));
    gh_push(&L, "Created", json_get(d, "createdAt"));
    gh_push(&L, "Updated", json_get(d, "updatedAt"));
    char *lb = gh_labels(json_get(d, "labels"));
    gh_push_s(&L, "Labels", lb);
    free(lb);
    gh_push(&L, "URL", json_get(d, "url"));
    strvec_push(&L, "");
    strvec_push(&L, "## Body");
    strvec_push(&L, "");
    char *body = gh_norm_text(json_sv(json_get(d, "body")));
    strvec_push(&L, *body ? body : "No description provided.");
    free(body);
    const json_t *files = json_get(d, "files");
    if (json_is_arr(files) && json_len(files)) {
        strvec_t sec = {0};
        strvec_push_own(&sec, xasprintf("## Files (%zu)", json_len(files)));
        strvec_push(&sec, "");
        for (size_t i = 0; i < json_len(files) && i < FILE_PREVIEW_LIMIT; i++) {
            const json_t *f = json_at(files, i);
            const json_t *p = gh_nn(gh_oc(f, "path")), *ct = gh_nn(gh_oc(f, "changeType"));
            const json_t *ad = gh_nn(gh_oc(f, "additions")), *de = gh_nn(gh_oc(f, "deletions"));
            char *ps = p ? gh_jsv(p) : xstrdup("(unknown file)"), *cs = ct ? gh_jsv(ct) : xstrdup("CHANGED");
            char *as = ad ? gh_jsv(ad) : xstrdup("0"), *ds = de ? gh_jsv(de) : xstrdup("0");
            strvec_push_own(&sec, xasprintf("- %s [%s] (+%s -%s)", ps, cs, as, ds));
            free(ps);
            free(cs);
            free(as);
            free(ds);
        }
        if (json_len(files) > FILE_PREVIEW_LIMIT)
            strvec_push_own(&sec, xasprintf("[…%zu files elided…]", json_len(files) - FILE_PREVIEW_LIMIT));
        push_section(&L, &sec);
        strvec_free(&sec);
    }
    if (comments && gh_truthy(json_get(d, "reviews"))) {
        strvec_t sec = {0};
        reviews_section(json_get(d, "reviews"), &sec);
        push_section(&L, &sec);
        strvec_free(&sec);
    }
    if (comments && gh_truthy(json_get(d, "reviewComments"))) {
        strvec_t sec = {0};
        review_comments_section(json_get(d, "reviewComments"), &sec);
        push_section(&L, &sec);
        strvec_free(&sec);
    }
    if (comments && gh_truthy(json_get(d, "comments"))) {
        strvec_t sec = {0};
        comments_section(json_get(d, "comments"), &sec);
        push_section(&L, &sec);
        strvec_free(&sec);
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

/* fetchPrReviewComments -> normalized array (owned) */
static json_t *fetch_review_comments(const char *cwd, const char *repo, int64_t number, char **err) {
    gh_ref_t ref;
    gh_parse_ref(repo, &ref);
    json_t *all = json_arr();
    for (int page = 1;; page++) {
        strvec_t a = gh_args("api", NULL);
        gh_api_host_args(&a, &ref);
        strvec_push(&a, "--method");
        strvec_push(&a, "GET");
        strvec_push_own(&a, xasprintf("/repos/%s/pulls/%lld/comments", ref.slug, (long long)number));
        strvec_push(&a, "-F");
        strvec_push(&a, "per_page=100");
        strvec_push(&a, "-F");
        strvec_push_own(&a, xasprintf("page=%d", page));
        json_t *resp = gh_json(cwd, &a, true, true, err);
        strvec_free(&a);
        if (!resp) {
            json_free(all);
            all = NULL;
            break;
        }
        size_t n = json_len(resp);
        for (size_t i = 0; i < n; i++) {
            const json_t *c = json_at(resp, i);
            if (!json_is_num(json_get(c, "id"))) continue;
            json_t *o = json_obj();
            const json_t *user = json_get(c, "user");
            json_set(o, "author", user ? json_clone(user) : json_null());
            if (json_get(c, "body")) json_set(o, "body", json_clone(json_get(c, "body")));
            char *s;
            if ((s = gh_norm_opt(json_get_str(c, "created_at")))) json_set(o, "createdAt", json_str_own(s));
            json_set(o, "id", json_clone(json_get(c, "id")));
            if (json_is_num(json_get(c, "in_reply_to_id"))) json_set(o, "inReplyToId", json_clone(json_get(c, "in_reply_to_id")));
            if (json_is_num(json_get(c, "line"))) json_set(o, "line", json_clone(json_get(c, "line")));
            if (json_is_num(json_get(c, "original_line"))) json_set(o, "originalLine", json_clone(json_get(c, "original_line")));
            if ((s = gh_norm_opt(json_get_str(c, "path")))) json_set(o, "path", json_str_own(s));
            if ((s = gh_norm_opt(json_get_str(c, "side")))) json_set(o, "side", json_str_own(s));
            if ((s = gh_norm_opt(json_get_str(c, "html_url")))) json_set(o, "url", json_str_own(s));
            json_push(all, o);
        }
        json_free(resp);
        if (n < 100) break;
    }
    gh_ref_free(&ref);
    return all;
}

char *gh_issue_view(const char *cwd, const char *repo, const char *issue, bool comments, char **source_url, char **err) {
    if (source_url) *source_url = NULL;
    strvec_t a = gh_args("issue", "view", issue, NULL);
    gh_append_repo_flag(&a, repo, issue);
    strvec_push(&a, "--json");
    strvec_push(&a, comments ? GH_ISSUE_FIELDS : GH_ISSUE_FIELDS_NO_COMMENTS);
    json_t *d = gh_issue_json(cwd, &a, repo != NULL, err);
    strvec_free(&a);
    if (!d) return NULL;
    char *r = gh_format_issue(d, issue, comments);
    if (source_url && json_get_str(d, "url")) *source_url = xstrdup(json_get_str(d, "url"));
    json_free(d);
    return r;
}

char *gh_pr_view(const char *cwd, const char *repo, int64_t number, bool comments, char **source_url, char **err) {
    if (source_url) *source_url = NULL;
    char *ns = xasprintf("%lld", (long long)number);
    strvec_t a = gh_args("pr", "view", ns, NULL);
    gh_append_repo_flag(&a, repo, ns);
    strvec_push(&a, "--json");
    strvec_push(&a, comments ? GH_PR_FIELDS : GH_PR_FIELDS_NO_COMMENTS);
    json_t *d = gh_json(cwd, &a, true, true, err);
    strvec_free(&a);
    char *r = NULL;
    if (d) {
        bool ok = true;
        if (comments && json_is_num(json_get(d, "number"))) {
            json_t *rc = fetch_review_comments(cwd, repo, json_get_int(d, "number", 0), err);
            if (rc) json_set(d, "reviewComments", rc);
            else ok = false;
        }
        if (ok) {
            r = gh_format_pr(d, ns, comments);
            if (source_url && json_get_str(d, "url")) *source_url = xstrdup(json_get_str(d, "url"));
        }
        json_free(d);
    }
    free(ns);
    return r;
}

/* ---------- PR diffs (gh-pr-diff.ts) ---------- */

static bool parse_quoted(const char *t, size_t at, char **val, size_t *next) {
    if (t[at] != '"') return false;
    sb_t b = SB_INIT;
    for (size_t i = at + 1; t[i]; i++) {
        char c = t[i];
        if (c == '"') {
            *val = sb_detach(&b);
            *next = i + 1;
            return true;
        }
        if (c != '\\') {
            sb_putc(&b, c);
            continue;
        }
        char nx = t[i + 1];
        if (!nx) {
            sb_putc(&b, '\\');
            continue;
        }
        if (nx >= '0' && nx <= '7') {
            size_t e = i + 1;
            while (t[e] && e < i + 4 && t[e] >= '0' && t[e] <= '7') e++;
            char tmp[4] = {0};
            memcpy(tmp, t + i + 1, e - i - 1);
            long v = strtol(tmp, NULL, 8);
            /* String.fromCharCode(v): code unit v; as UTF-8 for v >= 0x80 */
            char u[4];
            int k = utf8_encode((uint32_t)v, u);
            sb_appendn(&b, u, (size_t)k);
            i = e - 1;
            continue;
        }
        char out = nx;
        switch (nx) {
        case 'a': out = '\a'; break;
        case 'b': out = '\b'; break;
        case 'f': out = '\f'; break;
        case 'n': out = '\n'; break;
        case 'r': out = '\r'; break;
        case 't': out = '\t'; break;
        case 'v': out = '\v'; break;
        default: break;
        }
        sb_putc(&b, out);
        i++;
    }
    sb_free(&b);
    return false;
}

static void header_paths(const char *header, char **oldp, char **newp) {
    *oldp = *newp = NULL;
    const char *trail = header + strlen("diff --git ");
    if (trail[0] == '"') {
        char *ov = NULL, *nv = NULL;
        size_t nx = 0;
        if (!parse_quoted(trail, 0, &ov, &nx)) return;
        size_t s = nx;
        while (trail[s] == ' ') s++;
        if (!trail[s]) {
            free(ov);
            return;
        }
        if (trail[s] == '"') {
            size_t n2;
            if (!parse_quoted(trail, s, &nv, &n2)) nv = NULL;
        }
        if (!nv) {
            const char *sp = strchr(trail + s, ' ');
            nv = sp ? xstrndup(trail + s, (size_t)(sp - (trail + s))) : xstrdup(trail + s);
        }
        if (str_starts(ov, "a/")) *oldp = xstrdup(ov + 2);
        if (str_starts(nv, "b/")) *newp = xstrdup(nv + 2);
        free(ov);
        free(nv);
        return;
    }
    const char *b = strstr(trail, " b/");
    if (str_starts(trail, "a/") && b && b > trail) {
        *oldp = xstrndup(trail + 2, (size_t)(b - trail - 2));
        *newp = xstrdup(b + 3);
    }
}

static bool file_header_line(const char *l) {
    return !strcmp(l, "--- /dev/null") || !strcmp(l, "+++ /dev/null") || str_starts(l, "--- a/") || str_starts(l, "+++ b/") ||
           str_starts(l, "--- \"a/") || str_starts(l, "+++ \"b/");
}

static void parse_section(const char *text, size_t start, size_t end, gh_diff_file_t *f) {
    char *sec = xstrndup(text + start, end - start);
    strvec_t lines = str_split(sec, "\n");
    free(sec);
    char *oldp = NULL, *newp = NULL;
    header_paths(lines.n ? lines.v[0] : "", &oldp, &newp);
    const char *ct = "modified";
    bool binary = false, in_hunk = false;
    int64_t add = 0, del = 0;
    for (size_t i = 1; i < lines.n; i++) {
        const char *l = lines.v[i];
        if (str_starts(l, "new file mode")) {
            ct = "added";
            continue;
        }
        if (str_starts(l, "deleted file mode")) {
            ct = "deleted";
            continue;
        }
        if (str_starts(l, "rename from ")) {
            ct = "renamed";
            free(oldp);
            oldp = xstrdup(l + strlen("rename from "));
            continue;
        }
        if (str_starts(l, "rename to ")) {
            free(newp);
            newp = xstrdup(l + strlen("rename to "));
            continue;
        }
        if (str_starts(l, "Binary files ") && str_ends(l, " differ")) {
            binary = true;
            continue;
        }
        if (str_starts(l, "@@ ")) {
            in_hunk = true;
            continue;
        }
        if (!in_hunk && file_header_line(l)) continue;
        if (l[0] == '+') add++;
        else if (l[0] == '-') del++;
    }
    strvec_free(&lines);
    if (binary) {
        if (!strcmp(ct, "modified")) ct = "binary";
        add = del = 0;
    }
    const char *disp = !strcmp(ct, "deleted") ? (oldp ? oldp : newp ? newp : "(unknown)") : (newp ? newp : oldp ? oldp : "(unknown)");
    f->path = xstrdup(disp);
    f->old_path = oldp && strcmp(oldp, disp) ? xstrdup(oldp) : NULL;
    f->additions = add;
    f->deletions = del;
    f->change_type = ct;
    f->start = start;
    f->end = end;
    free(oldp);
    free(newp);
}

void gh_parse_unified_diff(const char *text, gh_diff_files_t *out) {
    memset(out, 0, sizeof *out);
    VEC(size_t) starts = {0};
    for (const char *p = text; *p;) {
        if (str_starts(p, "diff --git ")) vec_push(&starts, (size_t)(p - text));
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    size_t len = strlen(text);
    for (size_t i = 0; i < starts.n; i++) {
        gh_diff_file_t f;
        parse_section(text, starts.v[i], i + 1 < starts.n ? starts.v[i + 1] : len, &f);
        vec_push(out, f);
    }
    vec_free(&starts);
}

void gh_diff_files_free(gh_diff_files_t *f) {
    for (size_t i = 0; i < f->n; i++) {
        free(f->v[i].path);
        free(f->v[i].old_path);
    }
    vec_free(f);
}

static char *synthetic_path(const char *prefix, const char *path) {
    char *pp = xasprintf("%s%s", prefix, path);
    bool needs = false;
    for (const unsigned char *p = (const unsigned char *)pp; *p; p++)
        if (*p < 32 || *p == ' ' || *p == '"' || *p == '\\') needs = true;
    if (!needs) return pp;
    sb_t b = SB_INIT;
    sb_putc(&b, '"');
    for (const unsigned char *p = (const unsigned char *)pp; *p; p++) {
        switch (*p) {
        case '\\': sb_append(&b, "\\\\"); break;
        case '"': sb_append(&b, "\\\""); break;
        case '\n': sb_append(&b, "\\n"); break;
        case '\r': sb_append(&b, "\\r"); break;
        case '\t': sb_append(&b, "\\t"); break;
        default:
            if (*p < 32) sb_appendf(&b, "\\%03o", *p);
            else sb_putc(&b, (char)*p);
        }
    }
    sb_putc(&b, '"');
    free(pp);
    return sb_detach(&b);
}

static char *synthetic_section(const json_t *file) {
    const char *newp = json_get_str(file, "filename");
    if (!newp || !*newp) return NULL;
    const char *status = json_get_str(file, "status") ? json_get_str(file, "status") : "modified";
    const char *oldp = json_get_str(file, "previous_filename") ? json_get_str(file, "previous_filename") : newp;
    char *od = synthetic_path("a/", oldp), *nd = synthetic_path("b/", newp);
    strvec_t L = {0};
    strvec_push_own(&L, xasprintf("diff --git %s %s", od, nd));
    if (!strcmp(status, "added")) strvec_push(&L, "new file mode 100644");
    else if (!strcmp(status, "removed")) strvec_push(&L, "deleted file mode 100644");
    else if (!strcmp(status, "renamed") || json_get_str(file, "previous_filename")) {
        strvec_push_own(&L, xasprintf("rename from %s", oldp));
        strvec_push_own(&L, xasprintf("rename to %s", newp));
    }
    const char *patch = json_get_str(file, "patch");
    if (patch && *patch) {
        strvec_push_own(&L, !strcmp(status, "added") ? xstrdup("--- /dev/null") : xasprintf("--- %s", od));
        strvec_push_own(&L, !strcmp(status, "removed") ? xstrdup("+++ /dev/null") : xasprintf("+++ %s", nd));
        strvec_push(&L, patch);
    } else {
        strvec_push_own(&L, xasprintf("* patch unavailable (binary or too large); additions %lld, deletions %lld",
                                      (long long)json_get_int(file, "additions", 0), (long long)json_get_int(file, "deletions", 0)));
    }
    char *r = strvec_join(&L, "\n");
    strvec_free(&L);
    free(od);
    free(nd);
    return r;
}

static bool diff_too_large(const char *msg) {
    return strstr(msg, "HTTP 406") || str_istr(msg, "exceeded the maximum number of lines") || strstr(msg, "too_large");
}

static char *diff_via_files_api(const char *cwd, const char *repo, int64_t number, char **err) {
    gh_ref_t ref;
    gh_parse_ref(repo, &ref);
    strvec_t a = gh_args("api", NULL);
    gh_api_host_args(&a, &ref);
    strvec_push(&a, "--method");
    strvec_push(&a, "GET");
    strvec_push_own(&a, xasprintf("/repos/%s/pulls/%lld", ref.slug, (long long)number));
    json_t *pull = gh_json(cwd, &a, true, true, err);
    strvec_free(&a);
    char *res = NULL;
    if (!pull) goto done;
    int64_t changed = json_get_int(pull, "changed_files", 0);
    json_free(pull);
    if (changed > 3000) {
        *err = xasprintf("Pull request changes %lld files, exceeding GitHub's 3000-file limit for the per-file diff API.",
                         (long long)changed);
        goto done;
    }
    strvec_t secs = {0};
    for (int page = 1;; page++) {
        strvec_t b = gh_args("api", NULL);
        gh_api_host_args(&b, &ref);
        strvec_push(&b, "--method");
        strvec_push(&b, "GET");
        strvec_push_own(&b, xasprintf("/repos/%s/pulls/%lld/files", ref.slug, (long long)number));
        strvec_push(&b, "-F");
        strvec_push(&b, "per_page=100");
        strvec_push(&b, "-F");
        strvec_push_own(&b, xasprintf("page=%d", page));
        json_t *resp = gh_json(cwd, &b, true, true, err);
        strvec_free(&b);
        if (!resp) {
            strvec_free(&secs);
            goto done;
        }
        for (size_t i = 0; i < json_len(resp); i++) {
            char *s = synthetic_section(json_at(resp, i));
            if (s) strvec_push_own(&secs, s);
        }
        size_t n = json_len(resp);
        json_free(resp);
        if (n < 100) break;
    }
    if (secs.n) {
        char *j = strvec_join(&secs, "\n");
        res = xasprintf("%s\n", j);
        free(j);
    } else {
        res = xstrdup("");
    }
    strvec_free(&secs);
done:
    gh_ref_free(&ref);
    return res;
}

char *gh_pr_diff_text(const char *cwd, const char *repo, int64_t number, char **err) {
    char *ns = xasprintf("%lld", (long long)number);
    strvec_t a = gh_args("pr", "diff", ns, "--color", "never", NULL);
    gh_append_repo_flag(&a, repo, ns);
    char *t = gh_text(cwd, &a, true, false, err);
    strvec_free(&a);
    free(ns);
    if (!t && *err && diff_too_large(*err)) {
        free(*err);
        *err = NULL;
        t = diff_via_files_api(cwd, repo, number, err);
    }
    return t;
}

/* ---------- Bun.hash (Zig std.hash.Wyhash) ---------- */

static const uint64_t WY_SECRET[4] = {0xa0761d6478bd642fULL, 0xe7037ed1a0b428dbULL, 0x8ebc6af09c88c6e3ULL, 0x589965cc75374cc3ULL};

static inline void wy_mum(uint64_t *a, uint64_t *b) {
    __uint128_t x = (__uint128_t)*a * *b;
    *a = (uint64_t)x;
    *b = (uint64_t)(x >> 64);
}
static inline uint64_t wy_mix(uint64_t a, uint64_t b) {
    wy_mum(&a, &b);
    return a ^ b;
}
static inline uint64_t wy_r8(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static inline uint64_t wy_r4(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

uint64_t gh_wyhash(const void *data, size_t len, uint64_t seed) {
    const uint8_t *in = data;
    uint64_t st0 = seed ^ wy_mix(seed ^ WY_SECRET[0], WY_SECRET[1]), st1 = st0, st2 = st0, a, b;
    if (len <= 16) {
        if (len >= 4) {
            size_t end = len - 4, quarter = (len >> 3) << 2;
            a = (wy_r4(in) << 32) | wy_r4(in + quarter);
            b = (wy_r4(in + end) << 32) | wy_r4(in + end - quarter);
        } else if (len > 0) {
            a = ((uint64_t)in[0] << 16) | ((uint64_t)in[len >> 1] << 8) | in[len - 1];
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t i = 0;
        if (len >= 48) {
            while (i + 48 < len) {
                st0 = wy_mix(wy_r8(in + i) ^ WY_SECRET[1], wy_r8(in + i + 8) ^ st0);
                st1 = wy_mix(wy_r8(in + i + 16) ^ WY_SECRET[2], wy_r8(in + i + 24) ^ st1);
                st2 = wy_mix(wy_r8(in + i + 32) ^ WY_SECRET[3], wy_r8(in + i + 40) ^ st2);
                i += 48;
            }
            st0 ^= st1 ^ st2;
        }
        const uint8_t *rest = in + i;
        size_t rl = len - i, j = 0;
        while (j + 16 < rl) {
            st0 = wy_mix(wy_r8(rest + j) ^ WY_SECRET[1], wy_r8(rest + j + 8) ^ st0);
            j += 16;
        }
        a = wy_r8(in + len - 16);
        b = wy_r8(in + len - 8);
    }
    a ^= WY_SECRET[1];
    b ^= st0;
    wy_mum(&a, &b);
    return wy_mix(a ^ WY_SECRET[0] ^ (uint64_t)len, b ^ WY_SECRET[1]);
}
