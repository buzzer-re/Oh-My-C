/* gh_tool.c - the `github` tool (port of omp's tools/gh.ts, gh-search.ts, gh-view.ts repo_view,
 * gh-pr-checkout.ts, gh-run-watch.ts). Every op shells out to the `gh` CLI with omp's exact argv and renders
 * omp's exact result text. Registered only when `gh` is on PATH and `github.enabled` is on (xd:// device). */
#include "gh.h"
#include "gh_int.h"

#include "artifacts.h"
#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "outsink.h"
#include "session/worktree.h"

#include <ctype.h>
#include <errno.h>
#include "ai/ai_images.h"
#include "img/imgpipe.h"

#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    tool_env_t *env;
} gh_tool_t;

/* ---------- small helpers ---------- */

static const ai_model_t *active_model(tool_env_t *env) {
    if (!env || !env->active_model) return NULL;
    char *sid = NULL;
    const ai_model_t *m = env->active_model(env, &sid);
    free(sid);
    return m;
}

static char *opt_str(const json_t *args, const char *key) { return gh_norm_opt(json_get_str(args, key)); }

static void fail(tool_result_t *out, char *msg) {
    tool_result_error(out, "%s", msg ? msg : "GitHub CLI command failed");
    free(msg);
}

/* buildTextResult with an optional artifact reference */
static void text_result(tool_result_t *out, const char *text, const char *artifact_id, const char *label, bool useless) {
    if (artifact_id) {
        char *t = xasprintf("%s\n\n%s: artifact://%s", text, label ? label : "Saved artifact", artifact_id);
        tool_result_text(out, t);
        free(t);
    } else {
        tool_result_text(out, text);
    }
    if (useless) out->useless = true;
}

static json_t *details_new(void) { return json_obj(); }

static void det_str(json_t *d, const char *k, const char *v) {
    if (v) json_set_str(d, k, v);
}

static bool cancelled(tool_ctx_t *ctx) { return ctx && ctx->cancel && *ctx->cancel; }

/* scheduler.wait with abort: false when cancelled */
static bool wait_ms(tool_ctx_t *ctx, int64_t ms) {
    int64_t end = mono_ms() + ms;
    while (mono_ms() < end) {
        if (cancelled(ctx)) return false;
        usleep(50000);
    }
    return !cancelled(ctx);
}

/* ---------- formatBytes / image sniffing ---------- */

static char *fmt_bytes(int64_t n) {
    if (n < 1024) return xasprintf("%lldB", (long long)n);
    if (n < 1024 * 1024) return xasprintf("%.1fKB", n / 1024.0);
    if (n < 1024LL * 1024 * 1024) return xasprintf("%.1fMB", n / (1024.0 * 1024));
    return xasprintf("%.1fGB", n / (1024.0 * 1024 * 1024));
}

typedef struct {
    const char *mime;
    int64_t w, h; /* -1 unknown */
} gh_img_meta_t;

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* parseImageMetadata (png/jpeg/gif/webp) */
static bool image_meta(const unsigned char *b, size_t n, gh_img_meta_t *m) {
    m->w = m->h = -1;
    if (n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) {
        m->mime = "image/png";
        if (n >= 24 && !memcmp(b + 12, "IHDR", 4)) {
            m->w = be32(b + 16);
            m->h = be32(b + 20);
        }
        return true;
    }
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
        m->mime = "image/jpeg";
        size_t i = 2;
        while (i + 9 < n) {
            if (b[i] != 0xFF) break;
            unsigned char mk = b[i + 1];
            size_t len = (size_t)b[i + 2] << 8 | b[i + 3];
            if ((mk >= 0xC0 && mk <= 0xC3) || (mk >= 0xC5 && mk <= 0xC7) || (mk >= 0xC9 && mk <= 0xCB) || (mk >= 0xCD && mk <= 0xCF)) {
                m->h = (int64_t)b[i + 5] << 8 | b[i + 6];
                m->w = (int64_t)b[i + 7] << 8 | b[i + 8];
                break;
            }
            i += 2 + len;
        }
        return true;
    }
    if (n >= 6 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6))) {
        m->mime = "image/gif";
        if (n >= 10) {
            m->w = b[6] | b[7] << 8;
            m->h = b[8] | b[9] << 8;
        }
        return true;
    }
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) {
        m->mime = "image/webp";
        if (n >= 30 && !memcmp(b + 12, "VP8X", 4)) {
            m->w = 1 + (b[24] | b[25] << 8 | b[26] << 16);
            m->h = 1 + (b[27] | b[28] << 8 | b[29] << 16);
        } else if (n >= 30 && !memcmp(b + 12, "VP8 ", 4)) {
            m->w = (b[26] | b[27] << 8) & 0x3fff;
            m->h = (b[28] | b[29] << 8) & 0x3fff;
        } else if (n >= 25 && !memcmp(b + 12, "VP8L", 4)) {
            uint32_t v = b[21] | b[22] << 8 | b[23] << 16 | (uint32_t)b[24] << 24;
            m->w = 1 + (v & 0x3fff);
            m->h = 1 + ((v >> 14) & 0x3fff);
        }
        return true;
    }
    return false;
}

/* isProbablyBinaryHeader: NUL or invalid UTF-8 in the first 8 KiB (a sequence cut at the sniff boundary is fine) */
static bool probably_binary(const unsigned char *b, size_t n) {
    size_t sniff = n < 8192 ? n : 8192;
    if (memchr(b, 0, sniff)) return true;
    if (utf8_valid((const char *)b, sniff)) return false;
    if (sniff < n)
        for (size_t k = 1; k <= 3 && k < sniff; k++)
            if (utf8_valid((const char *)b, sniff - k)) return false;
    return true;
}

/* ---------- repo_view ---------- */

static const char *GH_REPO_FIELDS = "nameWithOwner,description,url,defaultBranchRef,homepageUrl,forkCount,isArchived,isFork,"
                                    "primaryLanguage,repositoryTopics,stargazerCount,updatedAt,viewerPermission,visibility";

static char *format_repo_view(const json_t *d, const char *repo, const char *branch) {
    strvec_t L = {0};
    const json_t *nwo = gh_nn(json_get(d, "nameWithOwner"));
    char *name = nwo ? gh_jsv(nwo) : xstrdup(repo ? repo : "GitHub Repository");
    strvec_push_own(&L, xasprintf("# %s", name));
    free(name);
    strvec_push(&L, "");
    char *desc = gh_norm_text(json_sv(json_get(d, "description")));
    strvec_push(&L, *desc ? desc : "No description provided.");
    free(desc);
    strvec_push(&L, "");
    gh_push(&L, "URL", json_get(d, "url"));
    gh_push(&L, "Default branch", gh_oc(gh_nn(json_get(d, "defaultBranchRef")), "name"));
    gh_push_s(&L, "Branch", branch);
    gh_push(&L, "Visibility", gh_nn(json_get(d, "visibility")));
    gh_push(&L, "Viewer permission", gh_nn(json_get(d, "viewerPermission")));
    gh_push(&L, "Primary language", gh_oc(gh_nn(json_get(d, "primaryLanguage")), "name"));
    gh_push(&L, "Stars", json_get(d, "stargazerCount"));
    gh_push(&L, "Forks", json_get(d, "forkCount"));
    gh_push(&L, "Archived", json_get(d, "isArchived"));
    gh_push(&L, "Fork", json_get(d, "isFork"));
    gh_push(&L, "Updated", json_get(d, "updatedAt"));
    gh_push(&L, "Homepage", gh_nn(json_get(d, "homepageUrl")));
    const json_t *topics = json_get(d, "repositoryTopics");
    if (json_is_arr(topics)) {
        strvec_t names = {0};
        for (size_t i = 0; i < json_len(topics); i++) {
            const json_t *t = json_at(topics, i);
            const json_t *n = gh_nn(gh_oc(t, "name"));
            if (!n) n = gh_oc(gh_nn(gh_oc(t, "topic")), "name");
            if (gh_truthy(n)) strvec_push_own(&names, gh_jsv(n));
        }
        char *j = strvec_join(&names, ", ");
        gh_push_s(&L, "Topics", j);
        free(j);
        strvec_free(&names);
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

static void op_repo_view(tool_env_t *env, const json_t *args, tool_result_t *out) {
    char *repo = opt_str(args, "repo"), *branch = opt_str(args, "branch"), *err = NULL;
    strvec_t a = gh_args("repo", "view", NULL);
    if (repo) strvec_push(&a, repo);
    if (branch) {
        strvec_push(&a, "--branch");
        strvec_push(&a, branch);
    }
    strvec_push(&a, "--json");
    strvec_push(&a, GH_REPO_FIELDS);
    json_t *d = gh_json(env->cwd, &a, repo != NULL, true, &err);
    if (!d) fail(out, err);
    else {
        char *t = format_repo_view(d, repo, branch);
        text_result(out, t, NULL, NULL, false);
        free(t);
    }
    json_free(d);
    strvec_free(&a);
    free(repo);
    free(branch);
}

/* ---------- file_read ---------- */

static void op_file_read(tool_env_t *env, const json_t *args, tool_result_t *out) {
    char *err = NULL, *prepo = opt_str(args, "repo");
    char *repo = gh_resolve_repo(env->cwd, prepo, NULL, &err);
    free(prepo);
    if (!repo) return fail(out, err);
    char *path = opt_str(args, "path");
    char *branch = NULL, *endpoint = NULL, *src = NULL;
    json_t *resp = NULL;
    unsigned char *bytes = NULL;
    gh_ref_t ref = {0};
    if (!path) {
        fail(out, xstrdup("path must not be empty"));
        goto done;
    }
    if (path[0] == '/') {
        fail(out, xstrdup("path must be repository-relative"));
        goto done;
    }
    branch = opt_str(args, "branch");
    {
        strvec_t segs = str_split(path, "/");
        sb_t b = SB_INIT;
        for (size_t i = 0; i < segs.n; i++) {
            char *e = gh_uri_component(segs.v[i]);
            sb_appendf(&b, "%s%s", i ? "/" : "", e);
            free(e);
        }
        strvec_free(&segs);
        endpoint = sb_detach(&b);
    }
    gh_parse_ref(repo, &ref);
    strvec_t a = gh_args("api", NULL);
    gh_api_host_args(&a, &ref);
    strvec_push_own(&a, xasprintf("/repos/%s/contents/%s", ref.slug, endpoint));
    strvec_push(&a, "--method");
    strvec_push(&a, "GET");
    strvec_push(&a, "-H");
    strvec_push(&a, "Accept: application/vnd.github+json");
    strvec_push(&a, "-H");
    strvec_push(&a, "Accept-Encoding: identity");
    if (branch) {
        strvec_push(&a, "-f");
        strvec_push_own(&a, xasprintf("ref=%s", branch));
    }
    resp = gh_json(env->cwd, &a, true, false, &err);
    strvec_free(&a);
    if (!resp) {
        fail(out, xasprintf("GitHub file read failed for '%s@%s:%s': %s", repo, branch ? branch : "HEAD", path, err));
        free(err);
        goto done;
    }
    if (!json_is_obj(resp) || !str_eq(json_get_str(resp, "type"), "file")) {
        fail(out, xasprintf("GitHub path '%s' is not a file.", path));
        goto done;
    }
    {
        char *def = gh_default_host();
        char *eb = gh_uri_component(branch ? branch : "HEAD");
        char *fallback = xasprintf("https://%s/%s/blob/%s/%s", ref.host ? ref.host : def, ref.slug, eb, endpoint);
        free(eb);
        free(def);
        const json_t *hu = json_get(resp, "html_url");
        src = gh_truthy(hu) ? gh_jsv(hu) : xstrdup(fallback);
        free(fallback);
    }
    const json_t *content = json_get(resp, "content");
    if (!str_eq(json_get_str(resp, "encoding"), "base64") || !json_is_str(content)) {
        const json_t *sz = json_get(resp, "size");
        char *size = json_is_num(sz) && json_get_num(resp, "size", -1) >= 0 ? fmt_bytes(json_d2i(json_get_num(resp, "size", 0)))
                                                                             : xstrdup("unknown size");
        char *t = xasprintf("[GitHub did not return file bytes for '%s' (%s). Open %s to view it.]", path, size, src);
        text_result(out, t, NULL, NULL, false);
        free(t);
        free(size);
        goto done;
    }
    {
        sb_t enc = SB_INIT;
        for (const char *p = json_sv(content); *p; p++)
            if (!isspace((unsigned char)*p)) sb_putc(&enc, *p);
        size_t n = 0;
        bytes = base64_decode(enc.s ? enc.s : "", enc.len, &n);
        if (!bytes) {
            bytes = (unsigned char *)xstrdup("");
            n = 0;
        }
        gh_img_meta_t im;
        if (image_meta(bytes, n, &im)) {
            char *sz = fmt_bytes((int64_t)n);
            char *dims = im.w >= 0 && im.h >= 0 ? xasprintf("\nDimensions: %lldx%lld", (long long)im.w, (long long)im.h) : xstrdup("");
            /* loadImageAttachmentInput: the size and encoder policy of file-backed image inputs */
            img_loaded_t l;
            char *lerr = NULL;
            const ai_model_t *model = active_model(env);
            if (img_load_input(bytes, n, im.mime, src, "Read image attachment", settings_bool("images.autoResize", true),
                               ai_model_lacks_webp(model) ? 1 : 0, &l, &lerr) < 0) {
                tool_result_error(out, "%s", lerr);
            } else {
                char *t = xasprintf("Image file: %s\nMIME: %s\nSize: %s%s", path, l.mime, sz, dims);
                tool_result_text(out, t);
                tool_result_image(out, l.data, l.mime);
                free(t);
                img_loaded_free(&l);
            }
            free(lerr);
            free(dims);
            free(sz);
            sb_free(&enc);
            goto done;
        }
        sb_free(&enc);
        if (probably_binary(bytes, n) || !utf8_valid((const char *)bytes, n)) {
            char *sz = fmt_bytes((int64_t)n);
            char *t = xasprintf("[Cannot read binary file '%s' (%s); not valid UTF-8 text. Open %s to view it.]", path, sz, src);
            text_result(out, t, NULL, NULL, false);
            free(t);
            free(sz);
            goto done;
        }
        /* TextDecoder drops a leading BOM */
        const char *txt = (const char *)bytes;
        size_t off = n >= 3 && !memcmp(txt, "\xEF\xBB\xBF", 3) ? 3 : 0;
        char *t = xstrndup(txt + off, n - off);
        text_result(out, t, NULL, NULL, false);
        free(t);
    }
done:
    free(bytes);
    json_free(resp);
    gh_ref_free(&ref);
    free(src);
    free(endpoint);
    free(branch);
    free(path);
    free(repo);
}

/* ---------- search ---------- */

#define SEARCH_LIMIT_DEFAULT 10
#define SEARCH_LIMIT_MAX 50

static int64_t search_limit(const json_t *args, char **err) {
    const json_t *v = json_get(args, "limit");
    if (!v) return SEARCH_LIMIT_DEFAULT;
    double d = json_get_num(args, "limit", 0);
    if (!json_is_num(v) || !isfinite(d) || d <= 0) {
        *err = xstrdup("limit must be a positive number");
        return -1;
    }
    double f = floor(d);
    return f < SEARCH_LIMIT_MAX ? (int64_t)f : SEARCH_LIMIT_MAX;
}

static char *iso_date_utc(time_t t) {
    struct tm tm;
    gmtime_r(&t, &tm);
    return xasprintf("%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* Date.parse for the ISO-ish shapes (date-time forms; offsets; local time without one); seconds since epoch */
static bool parse_datetime(const char *s, double *out) {
    int Y = 0, M = 1, D = 1, h = 0, mi = 0, sec = 0, n = 0;
    double frac = 0;
    if (sscanf(s, "%4d%n", &Y, &n) != 1 || n != 4) return false;
    const char *p = s + 4;
    bool date_only = true;
    if (*p == '-') {
        if (sscanf(p, "-%2d%n", &M, &n) != 1 || n != 3) return false;
        p += 3;
        if (*p == '-') {
            if (sscanf(p, "-%2d%n", &D, &n) != 1 || n != 3) return false;
            p += 3;
        }
    }
    bool has_tz = false;
    int tzoff = 0;
    if (*p == 'T' || *p == 't' || *p == ' ') {
        date_only = false;
        p++;
        if (sscanf(p, "%2d:%2d%n", &h, &mi, &n) != 2 || n != 5) return false;
        p += 5;
        if (*p == ':') {
            if (sscanf(p, ":%2d%n", &sec, &n) != 1 || n != 3) return false;
            p += 3;
            if (*p == '.') {
                p++;
                double scale = 0.1;
                if (!isdigit((unsigned char)*p)) return false;
                while (isdigit((unsigned char)*p)) {
                    frac += (*p - '0') * scale;
                    scale /= 10;
                    p++;
                }
            }
        }
        if (*p == 'Z' || *p == 'z') {
            has_tz = true;
            p++;
        } else if (*p == '+' || *p == '-') {
            int oh = 0, om = 0, sign = *p == '+' ? 1 : -1;
            if (sscanf(p + 1, "%2d:%2d%n", &oh, &om, &n) == 2 && n == 5) p += 6;
            else if (sscanf(p + 1, "%2d%2d%n", &oh, &om, &n) == 2 && n == 4) p += 5;
            else return false;
            tzoff = (oh * 60 + om) * sign;
            has_tz = true;
        }
    }
    if (*p) return false;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 24 || mi > 59 || sec > 59) return false;
    struct tm tm = {0};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    double t;
    if (date_only || has_tz) t = (double)timegm(&tm) - tzoff * 60.0;
    else {
        tm.tm_isdst = -1;
        t = (double)mktime(&tm);
    }
    *out = t + frac;
    return true;
}

/* parseSearchDateBound (owned) */
static char *date_bound(const char *raw, char **err) {
    char *t = gh_js_trim(raw);
    if (!*t) {
        free(t);
        *err = xstrdup("date bound must not be empty");
        return NULL;
    }
    const char *p = t;
    size_t nd = strspn(p, "0123456789");
    if (nd) {
        const char *u = p + nd;
        while (*u && isspace((unsigned char)*u)) u++;
        char unit[3] = {0};
        size_t ul = strlen(u);
        if (ul >= 1 && ul <= 2) {
            for (size_t i = 0; i < ul; i++) unit[i] = (char)tolower((unsigned char)u[i]);
            if (!strcmp(unit, "m") || !strcmp(unit, "h") || !strcmp(unit, "d") || !strcmp(unit, "w") || !strcmp(unit, "mo") ||
                !strcmp(unit, "y")) {
                long long count = strtoll(p, NULL, 10);
                time_t now = time(NULL);
                time_t bound;
                if (!strcmp(unit, "mo") || !strcmp(unit, "y")) {
                    struct tm tm;
                    gmtime_r(&now, &tm);
                    if (unit[0] == 'm') tm.tm_mon -= (int)count;
                    else tm.tm_year -= (int)count;
                    bound = timegm(&tm);
                } else {
                    long long ms = unit[0] == 'm' ? 60 : unit[0] == 'h' ? 3600 : unit[0] == 'd' ? 86400 : 7 * 86400;
                    bound = now - (time_t)(count * ms);
                }
                free(t);
                return iso_date_utc(bound);
            }
        }
    }
    if (strlen(t) == 10 && isdigit((unsigned char)t[0]) && t[4] == '-' && t[7] == '-' && strspn(t, "0123456789-") == 10) return t;
    double secs;
    if (parse_datetime(t, &secs)) {
        time_t tt = (time_t)floor(secs);
        struct tm tm;
        gmtime_r(&tt, &tm);
        free(t);
        return xasprintf("%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                         tm.tm_sec);
    }
    *err = xasprintf("invalid date bound: %s. Expected a relative duration like \"3d\", \"12h\", \"2w\", an ISO date "
                     "\"YYYY-MM-DD\", or an ISO datetime.",
                     raw);
    free(t);
    return NULL;
}

/* buildSearchDateQualifier; *ok false on error */
static char *date_qualifier(const char *field, const char *since, const char *until, char **err, bool *ok) {
    *ok = true;
    char *s = NULL, *u = NULL;
    if (since && *since && !(s = date_bound(since, err))) {
        *ok = false;
        return NULL;
    }
    if (until && *until && !(u = date_bound(until, err))) {
        free(s);
        *ok = false;
        return NULL;
    }
    char *r = NULL;
    if (s && u) r = xasprintf("%s:%s..%s", field, s, u);
    else if (s) r = xasprintf("%s:>=%s", field, s);
    else if (u) r = xasprintf("%s:<=%s", field, u);
    free(s);
    free(u);
    return r;
}

/* composeSearchQuery over up to 4 parts */
static char *compose_query(const char *a, const char *b, const char *c, char **err) {
    const char *parts[3] = {a, b, c};
    strvec_t v = {0};
    for (int i = 0; i < 3; i++) {
        if (!parts[i]) continue;
        char *t = gh_js_trim(parts[i]);
        if (*t) strvec_push_own(&v, t);
        else free(t);
    }
    if (!v.n) {
        *err = xstrdup("query is required (or pass since/until to filter by date)");
        return NULL;
    }
    char *r = strvec_join(&v, " ");
    strvec_free(&v);
    return r;
}

/* REPO_SCOPE_QUALIFIER_PATTERN /(?:^|\s)-?(?:repo|org|user|owner):\S/i */
static bool has_scope_qualifier(const char *q) {
    for (const char *p = q; *p; p++) {
        if (p != q && !isspace((unsigned char)p[-1])) continue;
        const char *s = p;
        if (*s == '-') s++;
        const char *kw[] = {"repo:", "org:", "user:", "owner:"};
        for (int i = 0; i < 4; i++) {
            size_t l = strlen(kw[i]);
            if (!strncasecmp(s, kw[i], l) && s[l] && !isspace((unsigned char)s[l])) return true;
        }
    }
    return false;
}

static char *search_scope_repo(tool_env_t *env, const char *repo, const char *query) {
    if (repo) return xstrdup(repo);
    if (query && has_scope_qualifier(query)) return NULL;
    char *err = NULL;
    char *r = gh_default_repo(env->cwd, &err);
    free(err);
    return r;
}

static strvec_t search_args(const char *endpoint, const char *query, int64_t limit, const char *host, const char *header) {
    strvec_t a = gh_args("api", NULL);
    if (host) {
        strvec_push(&a, "--hostname");
        strvec_push(&a, host);
    }
    strvec_push(&a, "-X");
    strvec_push(&a, "GET");
    strvec_push_own(&a, xasprintf("/search/%s", endpoint));
    strvec_push(&a, "-f");
    strvec_push_own(&a, xasprintf("q=%s", query));
    strvec_push(&a, "-F");
    strvec_push_own(&a, xasprintf("per_page=%lld", (long long)limit));
    if (header) {
        strvec_push(&a, "-H");
        strvec_push(&a, header);
    }
    return a;
}

/* apiUserToGhUser -> author object or NULL (owned) */
static json_t *api_user(const json_t *u) {
    if (!json_is_obj(u)) return NULL;
    const json_t *login = gh_nn(json_get(u, "login")), *name = gh_nn(json_get(u, "name"));
    if (!login && !name) return NULL;
    json_t *o = json_obj();
    if (login) json_set(o, "login", json_clone(login));
    if (name) json_set(o, "name", json_clone(name));
    return o;
}

static void push_author(strvec_t *L, const char *label, const json_t *api_u, const json_t *fallback) {
    json_t *u = api_user(api_u);
    char *a = gh_author(u);
    json_free(u);
    if (a) gh_push_s(L, label, a);
    else gh_push(L, label, fallback);
    free(a);
}

static char *repo_from_repository_url(const char *v) {
    if (!v) return NULL;
    const char *p = strstr(v, "/repos/");
    while (p) {
        const char *rest = p + 7;
        const char *sl = strchr(rest, '/');
        if (sl && sl > rest && sl[1] && !strchr(sl + 1, '/')) return xstrdup(rest);
        p = strstr(p + 1, "/repos/");
    }
    return NULL;
}

static char *format_issue_search(const char *kind, const char *query, const char *repo, const json_t *items) {
    strvec_t L = {0};
    strvec_push_own(&L, xasprintf("# GitHub %s search", kind));
    strvec_push(&L, "");
    strvec_push_own(&L, xasprintf("Query: %s", query));
    gh_push_s(&L, "Repository", repo);
    gh_push_i(&L, "Results", (int64_t)json_len(items));
    if (!json_len(items)) {
        strvec_push(&L, "");
        strvec_push_own(&L, xasprintf("No %s found.", kind));
    }
    for (size_t i = 0; i < json_len(items); i++) {
        const json_t *it = json_at(items, i);
        strvec_push(&L, "");
        const json_t *num = gh_nn(json_get(it, "number")), *title = gh_nn(json_get(it, "title"));
        char *ns = num ? gh_jsv(num) : xstrdup("?"), *ts = title ? gh_jsv(title) : xstrdup("Untitled");
        strvec_push_own(&L, xasprintf("- #%s %s", ns, ts));
        free(ns);
        free(ts);
        char *rp = repo_from_repository_url(json_get_str(it, "repository_url"));
        gh_push_s(&L, "  Repo", rp);
        free(rp);
        bool merged = gh_truthy(gh_oc(gh_nn(json_get(it, "pull_request")), "merged_at"));
        if (merged) gh_push_s(&L, "  State", "merged");
        else gh_push(&L, "  State", json_get(it, "state"));
        push_author(&L, "  Author", json_get(it, "user"), NULL);
        json_t *labels = json_arr();
        const json_t *al = json_get(it, "labels");
        for (size_t k = 0; json_is_arr(al) && k < json_len(al); k++) {
            json_t *o = json_obj();
            const json_t *n = gh_oc(json_at(al, k), "name");
            if (n) json_set(o, "name", json_clone(n));
            json_push(labels, o);
        }
        char *lb = gh_labels(labels);
        json_free(labels);
        gh_push_s(&L, "  Labels", lb);
        free(lb);
        gh_push(&L, "  Created", json_get(it, "created_at"));
        gh_push(&L, "  Updated", json_get(it, "updated_at"));
        gh_push(&L, "  URL", json_get(it, "html_url"));
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

static char *first_line_norm(const char *s) {
    char *n = gh_norm_text(s);
    char *nl = strchr(n, '\n');
    if (nl) *nl = 0;
    return n;
}

static char *format_code_search(const char *query, const char *repo, const json_t *items) {
    strvec_t L = {0};
    strvec_push(&L, "# GitHub code search");
    strvec_push(&L, "");
    strvec_push_own(&L, xasprintf("Query: %s", query));
    gh_push_s(&L, "Repository", repo);
    gh_push_i(&L, "Results", (int64_t)json_len(items));
    if (!json_len(items)) {
        strvec_push(&L, "");
        strvec_push(&L, "No code matches found.");
    }
    for (size_t i = 0; i < json_len(items); i++) {
        const json_t *it = json_at(items, i);
        strvec_push(&L, "");
        const json_t *p = gh_nn(json_get(it, "path"));
        char *ps = p ? gh_jsv(p) : xstrdup("(unknown path)");
        strvec_push_own(&L, xasprintf("- %s", ps));
        free(ps);
        gh_push(&L, "  Repo", gh_oc(gh_nn(json_get(it, "repository")), "full_name"));
        char *sha = gh_short_sha(json_get_str(it, "sha"));
        gh_push_s(&L, "  Commit", sha);
        free(sha);
        gh_push(&L, "  URL", json_get(it, "html_url"));
        const json_t *tm = json_get(it, "text_matches");
        for (size_t k = 0; json_is_arr(tm) && k < json_len(tm); k++) {
            const json_t *f = gh_oc(json_at(tm, k), "fragment");
            if (gh_truthy(f)) {
                char *fl = first_line_norm(json_sv(f));
                gh_push_s(&L, "  Match", fl);
                free(fl);
                break;
            }
        }
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

static char *format_commit_search(const char *query, const char *repo, const json_t *items) {
    strvec_t L = {0};
    strvec_push(&L, "# GitHub commits search");
    strvec_push(&L, "");
    strvec_push_own(&L, xasprintf("Query: %s", query));
    gh_push_s(&L, "Repository", repo);
    gh_push_i(&L, "Results", (int64_t)json_len(items));
    if (!json_len(items)) {
        strvec_push(&L, "");
        strvec_push(&L, "No commits found.");
    }
    for (size_t i = 0; i < json_len(items); i++) {
        const json_t *it = json_at(items, i);
        strvec_push(&L, "");
        char *sha = gh_short_sha(json_get_str(it, "sha"));
        const json_t *commit = gh_nn(json_get(it, "commit"));
        const char *msg = json_get_str(commit, "message");
        char *subject = msg && *msg ? first_line_norm(msg) : NULL;
        if (subject && !*subject) {
            free(subject);
            subject = NULL;
        }
        strvec_push_own(&L, xasprintf("- %s %s", sha ? sha : "(unknown sha)", subject ? subject : "(no commit message)"));
        free(sha);
        free(subject);
        gh_push(&L, "  Repo", gh_oc(gh_nn(json_get(it, "repository")), "full_name"));
        push_author(&L, "  Author", json_get(it, "author"), gh_oc(gh_nn(gh_oc(commit, "author")), "name"));
        const json_t *date = gh_nn(gh_oc(gh_nn(gh_oc(commit, "author")), "date"));
        if (!date) date = gh_oc(gh_nn(gh_oc(commit, "committer")), "date");
        gh_push(&L, "  Date", date);
        gh_push(&L, "  URL", json_get(it, "html_url"));
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

static char *format_repo_search(const char *query, const json_t *items) {
    strvec_t L = {0};
    strvec_push(&L, "# GitHub repositories search");
    strvec_push(&L, "");
    strvec_push_own(&L, xasprintf("Query: %s", query));
    gh_push_i(&L, "Results", (int64_t)json_len(items));
    if (!json_len(items)) {
        strvec_push(&L, "");
        strvec_push(&L, "No repositories found.");
    }
    for (size_t i = 0; i < json_len(items); i++) {
        const json_t *it = json_at(items, i);
        strvec_push(&L, "");
        const json_t *fn = gh_nn(json_get(it, "full_name"));
        char *fs = fn ? gh_jsv(fn) : xstrdup("(unknown repository)");
        strvec_push_own(&L, xasprintf("- %s", fs));
        free(fs);
        char *d = first_line_norm(json_sv(json_get(it, "description")));
        if (*d) gh_push_s(&L, "  Description", d);
        free(d);
        gh_push(&L, "  Language", gh_nn(json_get(it, "language")));
        gh_push(&L, "  Stars", json_get(it, "stargazers_count"));
        gh_push(&L, "  Forks", json_get(it, "forks_count"));
        gh_push(&L, "  Open issues", json_get(it, "open_issues_count"));
        gh_push(&L, "  Visibility", gh_nn(json_get(it, "visibility")));
        gh_push(&L, "  Archived", json_get(it, "archived"));
        gh_push(&L, "  Fork", json_get(it, "fork"));
        gh_push(&L, "  Updated", json_get(it, "updated_at"));
        gh_push(&L, "  URL", json_get(it, "html_url"));
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

typedef enum { S_ISSUES, S_PRS, S_CODE, S_COMMITS, S_REPOS } search_kind_t;

static void op_search(tool_env_t *env, const json_t *args, search_kind_t kind, tool_result_t *out) {
    char *err = NULL, *display = NULL, *repo = NULL, *api = NULL, *query = NULL, *qual = NULL;
    const json_t *items = NULL;
    json_t *resp = NULL;
    int64_t limit;
    gh_ref_t ref = {0};
    char *qualifier = NULL;
    if (kind == S_CODE) {
        query = opt_str(args, "query");
        if (!query) {
            fail(out, xstrdup("query must not be empty"));
            goto done;
        }
        char *s = opt_str(args, "since"), *u = opt_str(args, "until");
        bool dated = s || u;
        free(s);
        free(u);
        if (dated) {
            fail(out, xstrdup("search_code does not support since/until; GitHub code search has no date qualifier."));
            goto done;
        }
        if ((limit = search_limit(args, &err)) < 0) {
            fail(out, err);
            goto done;
        }
        display = xstrdup(query);
    } else {
        if ((limit = search_limit(args, &err)) < 0) {
            fail(out, err);
            goto done;
        }
        const char *df = json_get_str(args, "dateField");
        const char *field = kind == S_COMMITS ? "committer-date"
                            : kind == S_REPOS && str_eq(df, "updated") ? "pushed"
                            : df ? df : "created";
        bool ok;
        qual = date_qualifier(field, json_get_str(args, "since"), json_get_str(args, "until"), &err, &ok);
        if (!ok) {
            fail(out, err);
            goto done;
        }
        if (!(display = compose_query(json_get_str(args, "query"), qual, NULL, &err))) {
            fail(out, err);
            goto done;
        }
    }
    if (kind != S_REPOS) {
        char *prepo = opt_str(args, "repo");
        repo = search_scope_repo(env, prepo, display);
        free(prepo);
        if (repo) {
            gh_parse_ref(repo, &ref);
            qualifier = xasprintf("repo:%s", ref.slug);
        }
    }
    api = compose_query(display, qualifier, kind == S_ISSUES ? "is:issue" : kind == S_PRS ? "is:pr" : NULL, &err);
    {
        const char *ep = kind == S_CODE ? "code" : kind == S_COMMITS ? "commits" : kind == S_REPOS ? "repositories" : "issues";
        strvec_t a = search_args(ep, api, limit, ref.host, kind == S_CODE ? "Accept: application/vnd.github.text-match+json" : NULL);
        resp = gh_json(env->cwd, &a, false, true, &err);
        strvec_free(&a);
    }
    if (!resp) {
        fail(out, err);
        goto done;
    }
    items = json_get_arr(resp, "items");
    json_t *empty = NULL;
    if (!items) items = empty = json_arr();
    char *text = kind == S_CODE      ? format_code_search(query, repo, items)
                 : kind == S_COMMITS ? format_commit_search(display, repo, items)
                 : kind == S_REPOS   ? format_repo_search(display, items)
                                     : format_issue_search(kind == S_ISSUES ? "issues" : "pull requests", display, repo, items);
    text_result(out, text, NULL, NULL, json_len(items) == 0);
    free(text);
    json_free(empty);
done:
    json_free(resp);
    gh_ref_free(&ref);
    free(qualifier);
    free(qual);
    free(api);
    free(display);
    free(repo);
    free(query);
}

/* ---------- git helpers (pr_checkout / pr_push) ---------- */

static char *git_out(const char *cwd, char **err, const char *first, ...) {
    strvec_t v = {0};
    strvec_push(&v, "git");
    va_list ap;
    va_start(ap, first);
    for (const char *s = first; s; s = va_arg(ap, const char *)) strvec_push(&v, s);
    va_end(ap);
    vec_push(&v, NULL);
    const char *env[] = {"GIT_ASKPASS=true", "GIT_EDITOR=true", "GIT_TERMINAL_PROMPT=0", "LC_ALL", "LC_MESSAGES=C", NULL};
    proc_opts_t o = {0};
    o.cwd = cwd;
    o.env_extra = env;
    o.timeout_ms = 30 * 60 * 1000;
    o.new_session = true;
    proc_result_t r;
    int rc = proc_run((const char *const *)v.v, &o, &r);
    v.n--;
    strvec_free(&v);
    char *res = NULL;
    if (rc == 0 && !r.spawn_errno && r.exit_code == 0) {
        res = xstrndup(r.out ? r.out : "", r.out_len);
        size_t n = strlen(res);
        while (n && (res[n - 1] == '\n' || res[n - 1] == '\r')) res[--n] = 0;
    } else if (err) {
        char *m = gh_js_trim(r.err && r.err_len ? r.err : r.out ? r.out : "");
        *err = *m ? m : (free(m), xstrdup("git command failed"));
    }
    proc_result_free(&r);
    return res;
}

static char *git_config_get(const char *root, const char *key) {
    char *v = git_out(root, NULL, "config", "--get", key, NULL);
    return v;
}

static bool git_ref_exists(const char *root, const char *ref) {
    char *v = git_out(root, NULL, "rev-parse", "--verify", "--quiet", ref, NULL);
    bool ok = v != NULL;
    free(v);
    return ok;
}

static char *git_repo_root(const char *cwd) { return git_out(cwd, NULL, "rev-parse", "--show-toplevel", NULL); }

static char *git_primary_root(const char *root) {
    char *common = git_out(root, NULL, "rev-parse", "--path-format=absolute", "--git-common-dir", NULL);
    if (!common) return NULL;
    char *r;
    if (!strcmp(path_basename(common), ".git")) r = path_dirname(common);
    else r = xstrdup(common);
    free(common);
    return r;
}

static char *git_current_branch(const char *root) { return git_out(root, NULL, "symbolic-ref", "--quiet", "--short", "HEAD", NULL); }

typedef struct {
    char *path, *branch;
} wt_entry_t;
typedef VEC(wt_entry_t) wt_list_t;

static void git_worktrees(const char *root, wt_list_t *out) {
    memset(out, 0, sizeof *out);
    char *t = git_out(root, NULL, "worktree", "list", "--porcelain", NULL);
    if (!t) return;
    strvec_t lines = str_split(t, "\n");
    wt_entry_t cur = {0};
    for (size_t i = 0; i <= lines.n; i++) {
        const char *l = i < lines.n ? lines.v[i] : "";
        if (!*l) {
            if (cur.path) vec_push(out, cur);
            memset(&cur, 0, sizeof cur);
            continue;
        }
        if (str_starts(l, "worktree ")) cur.path = xstrdup(l + 9);
        else if (str_starts(l, "branch ")) cur.branch = xstrdup(l + 7);
    }
    strvec_free(&lines);
    free(t);
}

static void wt_list_free(wt_list_t *l) {
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i].path);
        free(l->v[i].branch);
    }
    vec_free(l);
}

/* getWorktreesDir / hashPath (session/worktree.c, shared with `omc worktree` and `/wt`) */
static char *worktrees_dir(void) { return wt_base_dir(); }
static char *hash_path(const char *abs) { return wt_hash_path(abs); }

/* ---------- pr_checkout ---------- */

static const char *GH_PR_CHECKOUT_FIELDS =
    "baseRefName,headRefName,headRefOid,headRepository,headRepositoryOwner,isCrossRepository,maintainerCanModify,number,title,url";

typedef struct {
    json_t *data;
    char *local_branch, *worktree, *remote, *remote_url, *head;
    bool reused;
} checkout_t;

static void checkout_free(checkout_t *c) {
    json_free(c->data);
    free(c->local_branch);
    free(c->worktree);
    free(c->remote);
    free(c->remote_url);
    free(c->head);
}

static char *slugify(const char *s) {
    /* pi-utils slugify: lowercase, non-alphanumerics -> '-', collapse, trim '-' */
    sb_t b = SB_INIT;
    bool dash = false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p)) {
            if (dash && b.len) sb_putc(&b, '-');
            dash = false;
            sb_putc(&b, (char)tolower(*p));
        } else {
            dash = true;
        }
    }
    return sb_detach(&b);
}

static bool ensure_remote(const char *root, const json_t *d, char **name, char **url, char **err) {
    if (!gh_truthy(json_get(d, "isCrossRepository"))) {
        char *u = git_out(root, NULL, "remote", "get-url", "origin", NULL);
        if (!u || !*u) {
            free(u);
            *err = xstrdup("origin remote is unavailable for this repository.");
            return false;
        }
        *name = xstrdup("origin");
        *url = u;
        return true;
    }
    char *head_repo = gh_norm_opt(json_sv(gh_oc(gh_nn(json_get(d, "headRepository")), "nameWithOwner")));
    if (!head_repo) {
        *err = xstrdup("head repository must not be empty");
        return false;
    }
    char *pull_repo = NULL;
    gh_parse_item_url(json_get_str(d, "url"), "pull", &pull_repo);
    gh_ref_t pr = {0};
    if (pull_repo) gh_parse_ref(pull_repo, &pr);
    char *target = gh_format_ref(pr.host, head_repo);
    strvec_t a = gh_args("repo", "view", target, "--json", "nameWithOwner,sshUrl,url", NULL);
    json_t *summary = gh_json(root, &a, true, true, err);
    strvec_free(&a);
    free(target);
    gh_ref_free(&pr);
    free(pull_repo);
    if (!summary) {
        free(head_repo);
        return false;
    }
    char *origin = git_out(root, NULL, "remote", "get-url", "origin", NULL);
    char *ru = NULL;
    char *hu = gh_norm_opt(json_get_str(summary, "url")), *su = gh_norm_opt(json_get_str(summary, "sshUrl"));
    if (origin && (str_starts(origin, "http://") || str_starts(origin, "https://"))) ru = hu ? xstrdup(hu) : su ? xstrdup(su) : xstrdup("");
    else ru = su ? xstrdup(su) : hu ? xstrdup(hu) : xstrdup("");
    free(hu);
    free(su);
    free(origin);
    json_free(summary);
    if (!*ru) {
        *err = xasprintf("Could not determine a clone URL for %s.", head_repo);
        free(ru);
        free(head_repo);
        return false;
    }
    char *remotes = git_out(root, NULL, "remote", NULL);
    strvec_t rl = remotes ? str_split(remotes, "\n") : (strvec_t){0};
    free(remotes);
    for (size_t i = 0; i < rl.n; i++) {
        if (!*rl.v[i]) continue;
        char *u = git_out(root, NULL, "remote", "get-url", rl.v[i], NULL);
        bool hit = u && !strcmp(u, ru);
        free(u);
        if (hit) {
            *name = xstrdup(rl.v[i]);
            *url = ru;
            strvec_free(&rl);
            free(head_repo);
            return true;
        }
    }
    const char *owner = json_sv(gh_oc(gh_nn(json_get(d, "headRepositoryOwner")), "login"));
    char *own = owner ? xstrdup(owner) : xstrndup(head_repo, strcspn(head_repo, "/"));
    char *sl = slugify(own);
    char *pref = *sl ? xasprintf("fork-%s", sl) : xstrdup("fork");
    free(sl);
    free(own);
    char *rn = xstrdup(pref);
    for (int suffix = 2; strvec_contains(&rl, rn); suffix++) {
        free(rn);
        rn = xasprintf("%s-%d", pref, suffix);
    }
    free(pref);
    strvec_free(&rl);
    char *e2 = NULL;
    char *ok = git_out(root, &e2, "remote", "add", rn, ru, NULL);
    if (!ok) {
        *err = e2;
        free(rn);
        free(ru);
        free(head_repo);
        return false;
    }
    free(ok);
    *name = rn;
    *url = ru;
    free(head_repo);
    return true;
}

static bool checkout_one(tool_env_t *env, const char *pr_ref, const char *repo, bool force, checkout_t *c, char **err) {
    memset(c, 0, sizeof *c);
    if (pr_ref && pr_ref[0] == '-') {
        *err = xasprintf("invalid PR identifier: %s. Pass a PR number, URL, or branch name.", pr_ref);
        return false;
    }
    strvec_t a = gh_args("pr", "view", NULL);
    if (pr_ref) strvec_push(&a, pr_ref);
    gh_append_repo_flag(&a, repo, pr_ref);
    strvec_push(&a, "--json");
    strvec_push(&a, GH_PR_CHECKOUT_FIELDS);
    c->data = gh_json(env->cwd, &a, repo != NULL, true, err);
    strvec_free(&a);
    if (!c->data) return false;
    if (!json_is_num(json_get(c->data, "number"))) {
        *err = xstrdup("GitHub CLI did not return a pull request number.");
        return false;
    }
    int64_t num = json_get_int(c->data, "number", 0);
    char *head = gh_norm_opt(json_get_str(c->data, "headRefName"));
    if (!head) {
        *err = xstrdup("head branch must not be empty");
        return false;
    }
    c->head = head;
    char *oid = gh_norm_opt(json_get_str(c->data, "headRefOid"));
    if (!oid) {
        *err = xstrdup("head commit must not be empty");
        return false;
    }
    char *root = git_repo_root(env->cwd);
    char *primary = root ? git_primary_root(root) : NULL;
    bool ok = false;
    wt_list_t wts = {0};
    char *wt_base = NULL, *lref = NULL, *rtrack = NULL;
    if (!root || !primary) {
        *err = xstrdup("Current git repository is unavailable.");
        goto out;
    }
    c->local_branch = xasprintf("pr-%lld", (long long)num);
    {
        char *dir = worktrees_dir(), *h = hash_path(primary);
        char *seg = xasprintf("%lld-%s", (long long)num, h);
        wt_base = path_join(dir, seg);
        free(seg);
        free(h);
        free(dir);
    }
    git_worktrees(root, &wts);
    lref = xasprintf("refs/heads/%s", c->local_branch);
    const wt_entry_t *existing = NULL;
    for (size_t i = 0; i < wts.n; i++)
        if (str_eq(wts.v[i].branch, lref)) existing = &wts.v[i];
    if (!ensure_remote(root, c->data, &c->remote, &c->remote_url, err)) goto out;
    rtrack = xasprintf("refs/remotes/%s/%s", c->remote, head);
    {
        char *spec = xasprintf("+refs/heads/%s:%s", head, rtrack);
        char *r = git_out(root, err, "fetch", "--quiet", "--no-tags", c->remote, spec, NULL);
        free(spec);
        if (!r) goto out;
        free(r);
    }
    if (!existing) {
        if (git_ref_exists(root, lref)) {
            char *cur = git_out(root, NULL, "rev-parse", "--verify", lref, NULL);
            if (!cur || strcmp(cur, oid)) {
                if (!force) {
                    char *s = gh_short_sha(cur);
                    *err = xasprintf("local branch %s already exists at %s; pass force=true to reset it", c->local_branch,
                                     s ? s : cur ? cur : "unknown commit");
                    free(s);
                    free(cur);
                    goto out;
                }
                char *r = git_out(root, err, "branch", "--no-track", "-f", c->local_branch, rtrack, NULL);
                if (!r) {
                    free(cur);
                    goto out;
                }
                free(r);
            }
            free(cur);
        } else {
            char *r = git_out(root, err, "branch", "--no-track", c->local_branch, rtrack, NULL);
            if (!r) goto out;
            free(r);
        }
    }
    {
        char *pfx = xasprintf("branch.%s.", c->local_branch);
        char *merge = xasprintf("refs/heads/%s", head);
        const char *url = json_get_str(c->data, "url");
        const char *kv[][2] = {{"remote", c->remote},
                               {"merge", merge},
                               {"pushRemote", c->remote},
                               {"ompPrHeadRef", head},
                               {"ompPrUrl", url ? url : ""},
                               {"ompPrIsCrossRepository", gh_truthy(json_get(c->data, "isCrossRepository")) ? "true" : "false"},
                               {"ompPrMaintainerCanModify", gh_truthy(json_get(c->data, "maintainerCanModify")) ? "true" : "false"}};
        bool cfg_ok = true;
        for (size_t i = 0; i < ARRAY_LEN(kv) && cfg_ok; i++) {
            char *k = xasprintf("%s%s", pfx, kv[i][0]);
            char *r = git_out(root, err, "config", k, kv[i][1], NULL);
            cfg_ok = r != NULL;
            free(r);
            free(k);
        }
        free(pfx);
        free(merge);
        if (!cfg_ok) goto out;
    }
    char *final = existing ? xstrdup(existing->path) : NULL;
    if (!existing) {
        for (int attempt = 0; attempt < 100 && !final; attempt++) {
            char *cand = attempt ? xasprintf("%s-%d", wt_base, attempt + 1) : xstrdup(wt_base);
            char *norm = path_normalize(cand);
            bool registered = false;
            for (size_t i = 0; i < wts.n; i++) {
                char *np = path_normalize(wts.v[i].path);
                if (!strcmp(np, norm)) registered = true;
                free(np);
            }
            struct stat st;
            if (!registered && lstat(norm, &st) != 0 && errno == ENOENT) final = xstrdup(cand);
            free(norm);
            free(cand);
        }
        if (!final) {
            *err = xasprintf("could not find an unused worktree path under %s (tried 100 suffixes)", wt_base);
            goto out;
        }
        char *parent = path_dirname(final);
        fs_mkdir_p(parent, 0755);
        free(parent);
        char *r = git_out(root, err, "worktree", "add", "--quiet", final, c->local_branch, NULL);
        if (!r) {
            free(final);
            goto out;
        }
        free(r);
    }
    char *real = path_real(final);
    c->worktree = real ? real : xstrdup(final);
    free(final);
    c->reused = existing != NULL;
    ok = true;
out:
    wt_list_free(&wts);
    free(wt_base);
    free(lref);
    free(rtrack);
    free(root);
    free(primary);
    free(oid);
    return ok;
}

static char *format_checkout(const checkout_t *c) {
    strvec_t L = {0};
    const json_t *num = gh_nn(json_get(c->data, "number"));
    char *ns = num ? gh_jsv(num) : xstrdup("?");
    strvec_push_own(&L, c->reused ? xasprintf("# Pull Request #%s Worktree", ns) : xasprintf("# Checked Out Pull Request #%s", ns));
    free(ns);
    strvec_push(&L, "");
    gh_push(&L, "Title", gh_nn(json_get(c->data, "title")));
    gh_push(&L, "URL", json_get(c->data, "url"));
    gh_push(&L, "Base", json_get(c->data, "baseRefName"));
    gh_push(&L, "Head", json_get(c->data, "headRefName"));
    gh_push_s(&L, "Local branch", c->local_branch);
    gh_push_s(&L, "Worktree", c->worktree);
    gh_push_s(&L, "Remote", c->remote);
    gh_push_s(&L, "Remote URL", c->remote_url);
    gh_push(&L, "Cross repository", json_get(c->data, "isCrossRepository"));
    gh_push(&L, "Maintainer can modify", json_get(c->data, "maintainerCanModify"));
    strvec_push(&L, "");
    strvec_push(&L, c->reused ? "Reused the existing PR worktree."
                              : "Created a dedicated worktree for this PR and configured the local branch to push back to the PR head branch.");
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

static json_t *checkout_summary(const checkout_t *c) {
    json_t *o = json_obj();
    if (json_is_num(json_get(c->data, "number"))) json_set(o, "prNumber", json_clone(json_get(c->data, "number")));
    det_str(o, "url", json_get_str(c->data, "url"));
    det_str(o, "branch", c->local_branch);
    det_str(o, "worktreePath", c->worktree);
    det_str(o, "remote", c->remote);
    det_str(o, "remoteBranch", c->head);
    json_set_bool(o, "reused", c->reused);
    return o;
}

static void sections_join(strvec_t *L, char **sections, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (i) {
            strvec_push(L, "");
            strvec_push(L, "---");
            strvec_push(L, "");
        }
        strvec_push(L, sections[i]);
    }
}

static void op_pr_checkout(tool_env_t *env, const json_t *args, tool_result_t *out) {
    char *repo = opt_str(args, "repo");
    bool force = json_get_bool(args, "force", false);
    strvec_t refs = {0};
    const json_t *pr = json_get(args, "pr");
    if (json_is_str(pr)) {
        char *t = gh_norm_opt(json_sv(pr));
        if (t) strvec_push_own(&refs, t);
    } else if (json_is_arr(pr)) {
        for (size_t i = 0; i < json_len(pr); i++) {
            char *t = gh_norm_opt(json_sv(json_at(pr, i)));
            if (t) strvec_push_own(&refs, t);
        }
    }
    size_t n = refs.n ? refs.n : 1;
    checkout_t *cs = xcalloc(n, sizeof *cs);
    char **errs = xcalloc(n, sizeof(char *));
    size_t okn = 0, failn = 0;
    for (size_t i = 0; i < n; i++) {
        if (checkout_one(env, refs.n ? refs.v[i] : NULL, repo, force, &cs[i], &errs[i])) okn++;
        else {
            failn++;
            if (!errs[i]) errs[i] = xstrdup("checkout failed");
        }
    }
    json_t *det = details_new();
    det_str(det, "repo", repo);
    if (failn) {
        if (!okn) {
            if (failn == 1) tool_result_error(out, "%s", errs[0]);
            else {
                sb_t b = SB_INIT;
                sb_appendf(&b, "all %zu PR checkouts failed:", failn);
                for (size_t i = 0; i < n; i++) sb_appendf(&b, "\n- %s: %s", refs.n ? refs.v[i] : "(current branch)", errs[i]);
                tool_result_error(out, "%s", sb_cstr(&b));
                sb_free(&b);
            }
            json_free(det);
            det = NULL;
        } else {
            strvec_t L = {0};
            strvec_push_own(&L, xasprintf("# %zu/%zu Pull Request Worktrees checked out (%zu failed)", okn, n, failn));
            strvec_push(&L, "");
            char **secs = xcalloc(okn, sizeof(char *));
            json_t *sums = json_arr();
            size_t k = 0;
            for (size_t i = 0; i < n; i++)
                if (!errs[i]) {
                    secs[k++] = format_checkout(&cs[i]);
                    json_push(sums, checkout_summary(&cs[i]));
                }
            sections_join(&L, secs, okn);
            strvec_push(&L, "");
            strvec_push(&L, "## Failed");
            for (size_t i = 0; i < n; i++)
                if (errs[i]) strvec_push_own(&L, xasprintf("- %s: %s", refs.n ? refs.v[i] : "(current branch)", errs[i]));
            char *t = gh_join_trim(&L);
            text_result(out, t, NULL, NULL, false);
            free(t);
            for (size_t i = 0; i < okn; i++) free(secs[i]);
            free(secs);
            strvec_free(&L);
            json_set(det, "checkouts", sums);
        }
    } else if (n == 1) {
        char *t = format_checkout(&cs[0]);
        text_result(out, t, NULL, NULL, false);
        free(t);
        const char *hr = json_sv(gh_oc(gh_nn(json_get(cs[0].data, "headRepository")), "nameWithOwner"));
        if (!repo && hr) json_set_str(det, "repo", hr);
        det_str(det, "branch", cs[0].local_branch);
        det_str(det, "worktreePath", cs[0].worktree);
        det_str(det, "remote", cs[0].remote);
        det_str(det, "remoteBranch", cs[0].head);
        json_t *sums = json_arr();
        json_push(sums, checkout_summary(&cs[0]));
        json_set(det, "checkouts", sums);
    } else {
        size_t reused = 0;
        char **secs = xcalloc(n, sizeof(char *));
        json_t *sums = json_arr();
        for (size_t i = 0; i < n; i++) {
            secs[i] = format_checkout(&cs[i]);
            json_push(sums, checkout_summary(&cs[i]));
            if (cs[i].reused) reused++;
        }
        strvec_t parts = {0};
        if (n - reused) strvec_push_own(&parts, xasprintf("%zu checked out", n - reused));
        if (reused) strvec_push_own(&parts, xasprintf("%zu reused", reused));
        char *pj = strvec_join(&parts, ", ");
        strvec_t L = {0};
        strvec_push_own(&L, xasprintf("# %zu Pull Request Worktrees (%s)", n, pj));
        strvec_push(&L, "");
        sections_join(&L, secs, n);
        char *t = gh_join_trim(&L);
        text_result(out, t, NULL, NULL, false);
        free(t);
        free(pj);
        strvec_free(&parts);
        strvec_free(&L);
        for (size_t i = 0; i < n; i++) free(secs[i]);
        free(secs);
        json_set(det, "checkouts", sums);
    }
    if (det) out->details = det;
    for (size_t i = 0; i < n; i++) {
        checkout_free(&cs[i]);
        free(errs[i]);
    }
    free(cs);
    free(errs);
    strvec_free(&refs);
    free(repo);
}

/* ---------- pr_push ---------- */

static void op_pr_push(tool_env_t *env, const json_t *args, tool_result_t *out) {
    char *root = git_repo_root(env->cwd);
    if (!root) {
        tool_result_error(out, "Current git repository is unavailable.");
        return;
    }
    char *local = opt_str(args, "branch");
    if (!local) local = git_current_branch(root);
    char *lref = NULL, *head = NULL, *push_remote = NULL, *remote = NULL, *pr_url = NULL, *rurl = NULL, *cur = NULL;
    if (!local || !*local) {
        tool_result_error(out, "Current git branch is unavailable. Pass `branch` or `run` explicitly.");
        goto done;
    }
    lref = xasprintf("refs/heads/%s", local);
    if (!git_ref_exists(root, lref)) {
        tool_result_error(out, "local branch %s does not exist", local);
        goto done;
    }
    {
        char *k = xasprintf("branch.%s.ompPrHeadRef", local);
        head = git_config_get(root, k);
        free(k);
        k = xasprintf("branch.%s.pushRemote", local);
        push_remote = git_config_get(root, k);
        free(k);
        k = xasprintf("branch.%s.remote", local);
        remote = git_config_get(root, k);
        free(k);
        k = xasprintf("branch.%s.ompPrUrl", local);
        pr_url = git_config_get(root, k);
        free(k);
    }
    if (!head) {
        tool_result_error(out, "branch %s has no PR push metadata; check it out via op: pr_checkout first", local);
        goto done;
    }
    const char *rn = push_remote ? push_remote : remote;
    if (!rn) {
        tool_result_error(out, "branch %s has no configured push remote", local);
        goto done;
    }
    rurl = git_out(root, NULL, "remote", "get-url", rn, NULL);
    cur = git_current_branch(root);
    {
        char *spec = xasprintf("%s:refs/heads/%s", str_eq(cur, local) ? "HEAD" : lref, head);
        bool lease = json_get_bool(args, "forceWithLease", false);
        char *err = NULL;
        char *r = lease ? git_out(root, &err, "push", "--quiet", "--force-with-lease", rn, spec, NULL)
                        : git_out(root, &err, "push", "--quiet", rn, spec, NULL);
        free(spec);
        if (!r) {
            fail(out, err);
            goto done;
        }
        free(r);
        strvec_t L = {0};
        strvec_push(&L, "# Pushed Pull Request Branch");
        strvec_push(&L, "");
        gh_push_s(&L, "Local branch", local);
        gh_push_s(&L, "Remote", rn);
        gh_push_s(&L, "Remote branch", head);
        gh_push_s(&L, "Remote URL", rurl);
        gh_push_s(&L, "PR", pr_url);
        gh_push_b(&L, "Force with lease", lease);
        strvec_push(&L, "");
        strvec_push_own(&L, xasprintf("Pushed %s to %s:%s.", local, rn, head));
        char *t = gh_join_trim(&L);
        text_result(out, t, NULL, NULL, false);
        free(t);
        strvec_free(&L);
        json_t *det = details_new();
        det_str(det, "branch", local);
        det_str(det, "remote", rn);
        det_str(det, "remoteBranch", head);
        out->details = det;
    }
done:
    free(cur);
    free(rurl);
    free(lref);
    free(head);
    free(push_remote);
    free(remote);
    free(pr_url);
    free(local);
    free(root);
}

/* ---------- pr_create ---------- */

static const char *GH_PR_CREATE_FIELDS = "author,baseRefName,createdAt,headRefName,isDraft,labels,number,state,title,url";

static void push_list(strvec_t *a, const char *flag, const json_t *v) {
    for (size_t i = 0; json_is_arr(v) && i < json_len(v); i++) {
        char *t = gh_norm_opt(json_sv(json_at(v, i)));
        if (!t) continue;
        strvec_push(a, flag);
        strvec_push_own(a, t);
    }
}

static void op_pr_create(tool_env_t *env, const json_t *args, tool_result_t *out) {
    char *repo = opt_str(args, "repo"), *title = opt_str(args, "title"), *base = opt_str(args, "base"), *head = opt_str(args, "head");
    const json_t *bodyv = json_get(args, "body");
    const char *body = json_sv(bodyv);
    bool draft = json_get_bool(args, "draft", false), fill = json_get_bool(args, "fill", false);
    char *body_dir = NULL, *err = NULL, *output = NULL, *url = NULL, *prepo = NULL;
    json_t *view = NULL;
    if (!fill && !title) {
        tool_result_error(out, "title is required unless fill is true");
        goto done;
    }
    if (fill && (title || bodyv)) {
        tool_result_error(out, "fill is mutually exclusive with title and body");
        goto done;
    }
    strvec_t a = gh_args("pr", "create", NULL);
    gh_append_repo_flag(&a, repo, NULL);
    if (title) {
        strvec_push(&a, "--title");
        strvec_push(&a, title);
    }
    if (base) {
        strvec_push(&a, "--base");
        strvec_push(&a, base);
    }
    if (head) {
        strvec_push(&a, "--head");
        strvec_push(&a, head);
    }
    if (draft) strvec_push(&a, "--draft");
    if (fill) strvec_push(&a, "--fill");
    push_list(&a, "--reviewer", json_get(args, "reviewer"));
    push_list(&a, "--assignee", json_get(args, "assignee"));
    push_list(&a, "--label", json_get(args, "label"));
    if (!fill) {
        if (body && *body) {
            const char *tmp = env_get("TMPDIR");
            char *tmpl = xasprintf("%s/gh-pr-body-XXXXXX", tmp && *tmp ? tmp : "/tmp");
            if (mkdtemp(tmpl)) {
                body_dir = tmpl;
                char *bf = path_join(body_dir, "body.md");
                fs_write(bf, body, strlen(body));
                strvec_push(&a, "--body-file");
                strvec_push_own(&a, bf);
            } else {
                free(tmpl);
                strvec_push(&a, "--body");
                strvec_push(&a, body);
            }
        } else {
            strvec_push(&a, "--body");
            strvec_push(&a, "");
        }
    }
    output = gh_text(env->cwd, &a, repo != NULL, true, &err);
    strvec_free(&a);
    if (!output) {
        fail(out, err);
        goto done;
    }
    {
        strvec_t lines = str_split(output, "\n");
        for (size_t i = 0; i < lines.n && !url; i++) {
            char *t = gh_js_trim(lines.v[i]);
            if (str_starts(t, "https://")) url = t;
            else free(t);
        }
        strvec_free(&lines);
        if (!url) url = gh_js_trim(output);
    }
    int64_t num = gh_parse_item_url(url, "pull", &prepo);
    const char *resolved = repo ? repo : prepo;
    if (resolved && num > 0) {
        char *ns = xasprintf("%lld", (long long)num);
        strvec_t v = gh_args("pr", "view", ns, "--repo", resolved, "--json", GH_PR_CREATE_FIELDS, NULL);
        char *e2 = NULL;
        view = gh_json(env->cwd, &v, true, true, &e2);
        free(e2);
        strvec_free(&v);
        free(ns);
    }
    {
        const json_t *vn = gh_nn(gh_oc(view, "number")), *vt = gh_nn(gh_oc(view, "title"));
        char *ns = num > 0 ? xasprintf("%lld", (long long)num) : vn ? gh_jsv(vn) : NULL;
        char *ht = vt ? gh_jsv(vt) : xstrdup(title ? title : "Untitled");
        strvec_t L = {0};
        strvec_push_own(&L, ns ? xasprintf("# Created Pull Request #%s: %s", ns, ht) : xasprintf("# Created Pull Request: %s", ht));
        free(ns);
        free(ht);
        strvec_push(&L, "");
        if (*url) gh_push_s(&L, "URL", url);
        else gh_push(&L, "URL", gh_oc(view, "url"));
        gh_push(&L, "State", gh_oc(view, "state"));
        const json_t *dv = gh_nn(gh_oc(view, "isDraft"));
        if (dv) gh_push(&L, "Draft", dv);
        else gh_push_b(&L, "Draft", draft);
        const json_t *bv = gh_nn(gh_oc(view, "baseRefName"));
        if (bv) gh_push(&L, "Base", bv);
        else gh_push_s(&L, "Base", base);
        const json_t *hv = gh_nn(gh_oc(view, "headRefName"));
        if (hv) gh_push(&L, "Head", hv);
        else gh_push_s(&L, "Head", head);
        char *au = gh_author(gh_oc(view, "author"));
        gh_push_s(&L, "Author", au);
        free(au);
        gh_push(&L, "Created", gh_oc(view, "createdAt"));
        char *lb = gh_labels(gh_oc(view, "labels"));
        gh_push_s(&L, "Labels", lb);
        free(lb);
        char *t = gh_join_trim(&L);
        text_result(out, t, NULL, NULL, false);
        free(t);
        strvec_free(&L);
    }
done:
    if (body_dir) {
        char *bf = path_join(body_dir, "body.md");
        unlink(bf);
        free(bf);
        rmdir(body_dir);
    }
    json_free(view);
    free(prepo);
    free(url);
    free(output);
    free(body_dir);
    free(repo);
    free(title);
    free(base);
    free(head);
}

/* ---------- run_watch (gh-run-watch.ts) ---------- */

#define RUN_WATCH_INTERVAL_DEFAULT 3
#define RUN_WATCH_INTERVAL_SLOW 15
#define RUN_WATCH_FAST_WINDOW_MS 60000
#define RUN_WATCH_NO_RUNS_GIVE_UP_MS 90000
#define RUN_WATCH_MAX_POLL_FAILURES 5
#define RUN_WATCH_GRACE 5
#define RUN_WATCH_TAIL_DEFAULT 15
#define RUN_WATCH_TAIL_MAX 200

/* GhRunSnapshot as JSON: {id, workflowName?, displayTitle?, status?, conclusion?, branch?, headSha?, createdAt?,
 * updatedAt?, url?, jobs:[{id,name,status?,conclusion?,startedAt?,completedAt?,url?}]} */

static void set_opt(json_t *o, const char *k, const json_t *src, const char *sk) {
    char *v = gh_norm_opt(json_get_str(src, sk));
    if (v) json_set(o, k, json_str_own(v));
}

static double ts_ms(const char *v);

/* normalizeGithubTimestamp: a trimmed timestamp, absent when unset; steps that have not run report Go's zero time
 * (`0001-01-01T00:00:00Z`) */
static void set_ts(json_t *o, const char *k, const json_t *src, const char *sk) {
    char *v = gh_norm_opt(json_get_str(src, sk));
    if (v && ts_ms(v) > 0) json_set(o, k, json_str_own(v));
    else free(v);
}

/* normalizeRunStep: {number, name, status?, conclusion?, startedAt?, completedAt?}; NULL without a positive number */
static json_t *norm_step(const json_t *st) {
    const json_t *num = json_get(st, "number");
    if (!json_is_num(num) || !num->is_int || num->u.num.i < 1) return NULL;
    int64_t n = num->u.num.i;
    json_t *o = json_obj();
    json_set_int(o, "number", n);
    char *name = gh_norm_opt(json_get_str(st, "name"));
    json_set(o, "name", json_str_own(name ? name : xasprintf("step-%lld", (long long)n)));
    set_opt(o, "status", st, "status");
    set_opt(o, "conclusion", st, "conclusion");
    set_ts(o, "startedAt", st, "started_at");
    set_ts(o, "completedAt", st, "completed_at");
    return o;
}

static json_t *norm_job(const json_t *j) {
    if (!json_is_num(json_get(j, "id"))) return NULL;
    json_t *o = json_obj();
    json_set(o, "id", json_clone(json_get(j, "id")));
    char *name = gh_norm_opt(json_get_str(j, "name"));
    json_set(o, "name", json_str_own(name ? name : xasprintf("job-%lld", (long long)json_get_int(j, "id", 0))));
    set_opt(o, "status", j, "status");
    set_opt(o, "conclusion", j, "conclusion");
    set_ts(o, "startedAt", j, "started_at");
    set_ts(o, "completedAt", j, "completed_at");
    set_opt(o, "url", j, "html_url");
    /* the ordered steps the same jobs API call returned (omp 4c73c16e0b) */
    const json_t *steps = json_get(j, "steps");
    if (json_is_arr(steps)) {
        json_t *sa = json_arr();
        for (size_t i = 0; i < json_len(steps); i++) {
            json_t *so = norm_step(json_at(steps, i));
            if (so) json_push(sa, so);
        }
        json_set(o, "steps", sa);
    }
    return o;
}

/* FAILURE_CONCLUSIONS (tui tools/github.ts) */
static bool failure_conclusion(const char *c) {
    static const char *const F[] = {"failure", "timed_out", "cancelled", "action_required", "startup_failure"};
    for (size_t i = 0; c && i < ARRAY_LEN(F); i++)
        if (!strcmp(c, F[i])) return true;
    return false;
}

/* sanitizeDisplayLine: sanitizeText, tabs expanded, newline runs collapsed to one space, trimmed */
static char *display_line(const char *s) {
    char *c = out_sanitize(s ? s : "", strlen(s ? s : ""), NULL);
    sb_t b = SB_INIT;
    for (const char *p = c; *p; p++) {
        if (*p == '\t') sb_append(&b, "   ");
        else if (*p == '\r' || *p == '\n') {
            while (p[1] == '\r' || p[1] == '\n') p++;
            sb_putc(&b, ' ');
        } else sb_putc(&b, *p);
    }
    free(c);
    char *t = str_trim(sb_cstr(&b));
    sb_free(&b);
    return t;
}

static json_t *norm_run(const json_t *r, json_t *jobs, char **err) {
    if (!json_is_num(json_get(r, "id"))) {
        *err = xstrdup("GitHub Actions run response did not include a run ID.");
        json_free(jobs);
        return NULL;
    }
    json_t *o = json_obj();
    json_set(o, "id", json_clone(json_get(r, "id")));
    set_opt(o, "workflowName", r, "name");
    set_opt(o, "displayTitle", r, "display_title");
    set_opt(o, "status", r, "status");
    set_opt(o, "conclusion", r, "conclusion");
    set_opt(o, "branch", r, "head_branch");
    set_opt(o, "headSha", r, "head_sha");
    set_opt(o, "createdAt", r, "created_at");
    set_opt(o, "updatedAt", r, "updated_at");
    set_opt(o, "url", r, "html_url");
    json_set(o, "jobs", jobs);
    return o;
}

static bool in_set(const char *v, const char *const *set) {
    for (; v && *set; set++)
        if (!strcmp(v, *set)) return true;
    return false;
}
static const char *const SUCCESS_C[] = {"success", "neutral", "skipped", NULL};
static const char *const FAILURE_C[] = {"failure", "timed_out", "cancelled", "action_required", "startup_failure", NULL};
static const char *const JOB_FAILURE_C[] = {"failure", "timed_out", "cancelled", "action_required", NULL};

static bool job_failed(const json_t *j) { return in_set(json_get_str(j, "conclusion"), JOB_FAILURE_C); }

/* "success" | "failure" | "pending" */
static const char *run_outcome(const char *conclusion) {
    if (!conclusion) return "pending";
    if (in_set(conclusion, SUCCESS_C)) return "success";
    if (in_set(conclusion, FAILURE_C)) return "failure";
    return "pending";
}

static const char *runs_outcome(const json_t *runs) {
    if (!json_len(runs)) return "pending";
    bool pending = false;
    for (size_t i = 0; i < json_len(runs); i++) {
        const json_t *r = json_at(runs, i), *jobs = json_get(r, "jobs");
        for (size_t k = 0; k < json_len(jobs); k++)
            if (job_failed(json_at(jobs, k))) return "failure";
        const char *o = str_eq(json_get_str(r, "status"), "completed") ? run_outcome(json_get_str(r, "conclusion")) : "pending";
        if (!strcmp(o, "failure")) return "failure";
        if (!strcmp(o, "pending")) pending = true;
    }
    return pending ? "pending" : "success";
}

static json_t *fetch_run_jobs(const char *cwd, const gh_ref_t *ref, int64_t run_id, char **err) {
    json_t *jobs = json_arr();
    for (int page = 1;; page++) {
        strvec_t a = gh_args("api", NULL);
        gh_api_host_args(&a, ref);
        strvec_push(&a, "--method");
        strvec_push(&a, "GET");
        strvec_push_own(&a, xasprintf("/repos/%s/actions/runs/%lld/jobs", ref->slug, (long long)run_id));
        strvec_push(&a, "-F");
        strvec_push(&a, "per_page=100");
        strvec_push(&a, "-F");
        strvec_push_own(&a, xasprintf("page=%d", page));
        json_t *resp = gh_json(cwd, &a, true, true, err);
        strvec_free(&a);
        if (!resp) {
            json_free(jobs);
            return NULL;
        }
        const json_t *raw = json_get_arr(resp, "jobs");
        for (size_t i = 0; i < json_len(raw); i++) {
            json_t *j = norm_job(json_at(raw, i));
            if (j) json_push(jobs, j);
        }
        size_t rn = json_len(raw);
        int64_t total = json_get_int(resp, "total_count", 0);
        json_free(resp);
        if (rn < 100 || total <= (int64_t)json_len(jobs)) break;
    }
    return jobs;
}

static json_t *fetch_run_snapshot(const char *cwd, const char *repo, int64_t run_id, char **err) {
    gh_ref_t ref;
    gh_parse_ref(repo, &ref);
    json_t *jobs = fetch_run_jobs(cwd, &ref, run_id, err);
    json_t *res = NULL;
    if (jobs) {
        strvec_t a = gh_args("api", NULL);
        gh_api_host_args(&a, &ref);
        strvec_push(&a, "--method");
        strvec_push(&a, "GET");
        strvec_push_own(&a, xasprintf("/repos/%s/actions/runs/%lld", ref.slug, (long long)run_id));
        json_t *run = gh_json(cwd, &a, true, true, err);
        strvec_free(&a);
        if (run) res = norm_run(run, jobs, err);
        else json_free(jobs);
        json_free(run);
    }
    gh_ref_free(&ref);
    return res;
}

static json_t *fetch_runs_for_commit(const char *cwd, const char *repo, const char *sha, json_t *cache, char **err) {
    gh_ref_t ref;
    gh_parse_ref(repo, &ref);
    strvec_t a = gh_args("api", NULL);
    gh_api_host_args(&a, &ref);
    strvec_push(&a, "--method");
    strvec_push(&a, "GET");
    strvec_push_own(&a, xasprintf("/repos/%s/actions/runs", ref.slug));
    strvec_push(&a, "-F");
    strvec_push_own(&a, xasprintf("head_sha=%s", sha));
    strvec_push(&a, "-F");
    strvec_push(&a, "per_page=100");
    json_t *resp = gh_json(cwd, &a, true, true, err);
    strvec_free(&a);
    json_t *runs = NULL;
    if (resp) {
        runs = json_arr();
        const json_t *wr = json_get_arr(resp, "workflow_runs");
        for (size_t i = 0; i < json_len(wr); i++) {
            const json_t *r = json_at(wr, i);
            if (!json_is_num(json_get(r, "id"))) continue;
            char key[32];
            snprintf(key, sizeof key, "%lld", (long long)json_get_int(r, "id", 0));
            bool completed = str_eq(json_get_str(r, "status"), "completed");
            if (!completed) json_del(cache, key);
            json_t *jobs = completed && json_get(cache, key) ? json_clone(json_get(cache, key)) : NULL;
            if (!jobs) {
                jobs = fetch_run_jobs(cwd, &ref, json_get_int(r, "id", 0), err);
                if (!jobs) {
                    json_free(runs);
                    runs = NULL;
                    break;
                }
                if (completed) json_set(cache, key, json_clone(jobs));
            }
            json_t *snap = norm_run(r, jobs, err);
            if (!snap) {
                json_free(runs);
                runs = NULL;
                break;
            }
            json_push(runs, snap);
        }
        json_free(resp);
    }
    gh_ref_free(&ref);
    return runs;
}

static void render_jobs(strvec_t *L, const json_t *jobs) {
    if (!json_len(jobs)) {
        strvec_push(L, "## Jobs");
        strvec_push(L, "");
        strvec_push(L, "No jobs reported yet.");
        return;
    }
    strvec_push_own(L, xasprintf("## Jobs (%zu)", json_len(jobs)));
    strvec_push(L, "");
    for (size_t i = 0; i < json_len(jobs); i++) {
        const json_t *j = json_at(jobs, i);
        const char *st = json_get_str(j, "conclusion") ? json_get_str(j, "conclusion")
                         : json_get_str(j, "status")   ? json_get_str(j, "status")
                                                       : "unknown";
        strvec_push_own(L, xasprintf("- [%s] %s", st, json_get_str(j, "name")));
        /* the step a failed job broke on: the first that concluded in failure */
        const json_t *steps = json_get(j, "steps");
        for (size_t k = 0; k < json_len(steps); k++) {
            const json_t *sp = json_at(steps, k);
            if (!failure_conclusion(json_get_str(sp, "conclusion"))) continue;
            char *nm = display_line(json_get_str(sp, "name"));
            strvec_push_own(L, xasprintf("  Failed step: #%lld %s", (long long)json_get_int(sp, "number", 0), nm));
            free(nm);
            break;
        }
        gh_push_s(L, "  Started", json_get_str(j, "startedAt"));
        gh_push_s(L, "  Completed", json_get_str(j, "completedAt"));
        gh_push_s(L, "  URL", json_get_str(j, "url"));
    }
}

static void render_run_section(strvec_t *L, const json_t *r) {
    const char *wf = json_get_str(r, "workflowName");
    int64_t id = json_get_int(r, "id", 0);
    strvec_push_own(L, wf ? xasprintf("### Run #%lld - %s", (long long)id, wf) : xasprintf("### Run #%lld", (long long)id));
    strvec_push(L, "");
    gh_push_s(L, "Title", json_get_str(r, "displayTitle"));
    gh_push_s(L, "Branch", json_get_str(r, "branch"));
    char *s = gh_short_sha(json_get_str(r, "headSha"));
    gh_push_s(L, "Commit", s);
    free(s);
    gh_push_s(L, "Status", json_get_str(r, "status"));
    gh_push_s(L, "Conclusion", json_get_str(r, "conclusion"));
    gh_push_s(L, "Created", json_get_str(r, "createdAt"));
    gh_push_s(L, "Updated", json_get_str(r, "updatedAt"));
    gh_push_s(L, "URL", json_get_str(r, "url"));
    strvec_push(L, "");
    render_jobs(L, json_get(r, "jobs"));
}

/* failed job log: {run, job, full?, tail?, available, unavailableReason?, truncated} */
static void render_failed_logs(strvec_t *L, const json_t *logs, bool full, int64_t tail) {
    if (!json_len(logs)) return;
    strvec_push(L, "## Failed Jobs");
    strvec_push(L, "");
    for (size_t i = 0; i < json_len(logs); i++) {
        const json_t *e = json_at(logs, i), *job = json_get(e, "job"), *run = json_get(e, "run");
        const char *c = json_get_str(job, "conclusion");
        strvec_push_own(L, xasprintf("### %s [%s]", json_get_str(job, "name"), c ? c : "failed"));
        strvec_push_own(L, xasprintf("Run: #%lld", (long long)json_get_int(run, "id", 0)));
        gh_push_s(L, "Workflow", json_get_str(run, "workflowName"));
        gh_push_s(L, "Started", json_get_str(job, "startedAt"));
        gh_push_s(L, "Completed", json_get_str(job, "completedAt"));
        gh_push_s(L, "URL", json_get_str(job, "url"));
        strvec_push(L, "");
        const char *txt = json_get_str(e, full ? "full" : "tail");
        bool truncated = json_get_bool(e, "truncated", false);
        if (json_get_bool(e, "available", false) && txt && *txt) {
            if (full) strvec_push(L, truncated ? "Partial log (capture truncated):" : "Full log:");
            else
                strvec_push_own(L, truncated ? xasprintf("Last %lld captured log lines (log capture truncated):", (long long)tail)
                                             : xasprintf("Last %lld log lines:", (long long)tail));
            strvec_push(L, "```text");
            strvec_push(L, txt);
            strvec_push(L, "```");
        } else {
            strvec_push(L, full ? "Full log unavailable." : "Log tail unavailable.");
            gh_push_s(L, "Reason", json_get_str(e, "unavailableReason"));
        }
        strvec_push(L, "");
    }
}

static size_t count_failed(const json_t *jobs) {
    size_t n = 0;
    for (size_t i = 0; i < json_len(jobs); i++)
        if (job_failed(json_at(jobs, i))) n++;
    return n;
}

static void run_header(strvec_t *L, const char *repo, const json_t *run) {
    gh_push_s(L, "Repository", repo);
    gh_push_s(L, "Workflow", json_get_str(run, "workflowName"));
    gh_push_s(L, "Title", json_get_str(run, "displayTitle"));
    gh_push_s(L, "Branch", json_get_str(run, "branch"));
    gh_push_s(L, "Status", json_get_str(run, "status"));
    gh_push_s(L, "Conclusion", json_get_str(run, "conclusion"));
    gh_push_s(L, "Created", json_get_str(run, "createdAt"));
    gh_push_s(L, "Updated", json_get_str(run, "updatedAt"));
    gh_push_s(L, "URL", json_get_str(run, "url"));
}

static char *format_run_snapshot(const char *repo, const json_t *run, int poll, const char *note, bool outcome) {
    strvec_t L = {0};
    size_t failed = count_failed(json_get(run, "jobs"));
    strvec_push_own(&L, xasprintf("# Watching GitHub Actions Run #%lld", (long long)json_get_int(run, "id", 0)));
    strvec_push(&L, "");
    run_header(&L, repo, run);
    gh_push_i(&L, "Poll", poll);
    if (failed) gh_push_i(&L, "Failed jobs", (int64_t)failed);
    if (note) {
        strvec_push(&L, "");
        strvec_push_own(&L, xasprintf("Note: %s", note));
    }
    strvec_push(&L, "");
    render_jobs(&L, json_get(run, "jobs"));
    if (outcome) {
        strvec_push(&L, "");
        strvec_push(&L, failed ? "Failures detected." : "All jobs passed.");
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

static char *format_run_result(const char *repo, const json_t *run, const json_t *logs, int64_t tail, bool full) {
    strvec_t L = {0};
    size_t failed = count_failed(json_get(run, "jobs"));
    strvec_push_own(&L, xasprintf("# GitHub Actions Run #%lld", (long long)json_get_int(run, "id", 0)));
    strvec_push(&L, "");
    run_header(&L, repo, run);
    strvec_push(&L, "");
    render_jobs(&L, json_get(run, "jobs"));
    if (failed) {
        strvec_push(&L, "");
        render_failed_logs(&L, logs, full, tail);
        strvec_push(&L, "Run failed.");
    } else if (!strcmp(run_outcome(json_get_str(run, "conclusion")), "success")) {
        strvec_push(&L, "");
        strvec_push(&L, "All jobs passed.");
    } else {
        strvec_push(&L, "");
        strvec_push(&L, "Run completed without successful jobs, but no failed job logs were available.");
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

static char *format_commit_snapshot(const char *repo, const char *sha, const char *branch, const json_t *runs, int poll,
                                    const char *note) {
    strvec_t L = {0};
    size_t failed = 0, completed = 0;
    for (size_t i = 0; i < json_len(runs); i++) {
        failed += count_failed(json_get(json_at(runs, i), "jobs"));
        if (str_eq(json_get_str(json_at(runs, i), "status"), "completed")) completed++;
    }
    char *ss = gh_short_sha(sha);
    strvec_push_own(&L, xasprintf("# Watching GitHub Actions for %s", ss ? ss : sha));
    free(ss);
    strvec_push(&L, "");
    gh_push_s(&L, "Repository", repo);
    gh_push_s(&L, "Branch", branch);
    gh_push_s(&L, "Commit", sha);
    gh_push_i(&L, "Poll", poll);
    gh_push_i(&L, "Runs", (int64_t)json_len(runs));
    strvec_push_own(&L, xasprintf("Completed runs: %zu/%zu", completed, json_len(runs)));
    if (failed) gh_push_i(&L, "Failed jobs", (int64_t)failed);
    if (note) {
        strvec_push(&L, "");
        strvec_push_own(&L, xasprintf("Note: %s", note));
    }
    if (!json_len(runs)) {
        strvec_push(&L, "");
        strvec_push(&L, "Waiting for workflow runs for this commit.");
    }
    for (size_t i = 0; i < json_len(runs); i++) {
        strvec_push(&L, "");
        render_run_section(&L, json_at(runs, i));
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

static char *format_commit_result(const char *repo, const char *sha, const char *branch, const json_t *runs, const json_t *logs,
                                  int64_t tail, bool full) {
    strvec_t L = {0};
    const char *outcome = runs_outcome(runs);
    char *ss = gh_short_sha(sha);
    strvec_push_own(&L, xasprintf("# GitHub Actions for %s", ss ? ss : sha));
    free(ss);
    strvec_push(&L, "");
    gh_push_s(&L, "Repository", repo);
    gh_push_s(&L, "Branch", branch);
    gh_push_s(&L, "Commit", sha);
    gh_push_i(&L, "Runs", (int64_t)json_len(runs));
    for (size_t i = 0; i < json_len(runs); i++) {
        strvec_push(&L, "");
        render_run_section(&L, json_at(runs, i));
    }
    if (json_len(logs)) {
        strvec_push(&L, "");
        render_failed_logs(&L, logs, full, tail);
        strvec_push(&L, "Workflow runs for this commit failed.");
    } else if (!strcmp(outcome, "success")) {
        strvec_push(&L, "");
        strvec_push(&L, "All workflow runs for this commit passed.");
    } else {
        strvec_push(&L, "");
        strvec_push(&L, "Workflow runs for this commit did not complete successfully.");
    }
    char *r = gh_join_trim(&L);
    strvec_free(&L);
    return r;
}

/* Date.parse of an ISO timestamp in ms, or -1 */
static double ts_ms(const char *v) {
    double s;
    return v && parse_datetime(v, &s) ? s * 1000.0 : -1;
}

static json_t *watch_run_details(const json_t *run, double observed) {
    json_t *o = json_obj();
    json_set(o, "id", json_clone(json_get(run, "id")));
    const char *keys[] = {"workflowName", "displayTitle", "status", "conclusion", "branch", "headSha", "url"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) det_str(o, keys[i], json_get_str(run, keys[i]));
    json_t *jobs = json_arr();
    const json_t *rj = json_get(run, "jobs");
    for (size_t i = 0; i < json_len(rj); i++) {
        const json_t *j = json_at(rj, i);
        json_t *jo = json_obj();
        json_set(jo, "id", json_clone(json_get(j, "id")));
        det_str(jo, "name", json_get_str(j, "name"));
        det_str(jo, "status", json_get_str(j, "status"));
        det_str(jo, "conclusion", json_get_str(j, "conclusion"));
        double st = ts_ms(json_get_str(j, "startedAt"));
        if (st >= 0) {
            double ct = ts_ms(json_get_str(j, "completedAt"));
            if (ct < 0) ct = observed;
            double d = floor((ct - st) / 1000.0);
            json_set_int(jo, "durationSeconds", (int64_t)(d > 0 ? d : 0));
        }
        const json_t *steps = json_get(j, "steps");
        if (json_is_arr(steps)) {
            json_t *sa = json_arr();
            for (size_t k = 0; k < json_len(steps); k++) {
                const json_t *sp = json_at(steps, k);
                json_t *so = json_obj();
                json_set_int(so, "number", json_get_int(sp, "number", 0));
                det_str(so, "name", json_get_str(sp, "name"));
                det_str(so, "status", json_get_str(sp, "status"));
                det_str(so, "conclusion", json_get_str(sp, "conclusion"));
                double sst = ts_ms(json_get_str(sp, "startedAt"));
                if (sst >= 0) {
                    double sct = ts_ms(json_get_str(sp, "completedAt"));
                    if (sct < 0) sct = observed;
                    double d = floor((sct - sst) / 1000.0);
                    json_set_int(so, "durationSeconds", (int64_t)(d > 0 ? d : 0));
                }
                json_push(sa, so);
            }
            json_set(jo, "steps", sa);
        }
        det_str(jo, "url", json_get_str(j, "url"));
        json_push(jobs, jo);
    }
    json_set(o, "jobs", jobs);
    return o;
}

static json_t *failed_log_details(const json_t *logs) {
    json_t *a = json_arr();
    for (size_t i = 0; i < json_len(logs); i++) {
        const json_t *e = json_at(logs, i), *run = json_get(e, "run"), *job = json_get(e, "job");
        json_t *o = json_obj();
        json_set(o, "runId", json_clone(json_get(run, "id")));
        det_str(o, "workflowName", json_get_str(run, "workflowName"));
        det_str(o, "jobName", json_get_str(job, "name"));
        det_str(o, "conclusion", json_get_str(job, "conclusion"));
        det_str(o, "tail", json_get_str(e, "tail"));
        json_set_bool(o, "available", json_get_bool(e, "available", false));
        det_str(o, "unavailableReason", json_get_str(e, "unavailableReason"));
        json_push(a, o);
    }
    return a;
}

static json_t *run_watch_details(const char *repo, const json_t *run, const char *state, int poll, const char *note,
                                 const json_t *logs) {
    json_t *d = json_obj();
    json_set_str(d, "repo", repo);
    det_str(d, "branch", json_get_str(run, "branch"));
    det_str(d, "headSha", json_get_str(run, "headSha"));
    json_set(d, "runId", json_clone(json_get(run, "id")));
    json_t *ids = json_arr();
    json_push(ids, json_clone(json_get(run, "id")));
    json_set(d, "runIds", ids);
    det_str(d, "status", json_get_str(run, "status"));
    det_str(d, "conclusion", json_get_str(run, "conclusion"));
    json_t *fj = json_arr();
    const json_t *jobs = json_get(run, "jobs");
    for (size_t i = 0; i < json_len(jobs); i++)
        if (job_failed(json_at(jobs, i))) json_push_str(fj, json_get_str(json_at(jobs, i), "name"));
    json_set(d, "failedJobs", fj);
    json_t *w = json_obj();
    json_set_str(w, "mode", "run");
    json_set_str(w, "state", state);
    json_set_str(w, "repo", repo);
    det_str(w, "branch", json_get_str(run, "branch"));
    det_str(w, "headSha", json_get_str(run, "headSha"));
    if (poll > 0) json_set_int(w, "pollCount", poll);
    det_str(w, "note", note);
    json_set(w, "run", watch_run_details(run, (double)time(NULL) * 1000.0));
    json_set(w, "failedLogs", logs ? failed_log_details(logs) : json_arr());
    json_set(d, "watch", w);
    return d;
}

static json_t *commit_watch_details(const char *repo, const char *sha, const char *branch, const json_t *runs, const char *state,
                                    int poll, const char *note, const json_t *logs) {
    json_t *d = json_obj();
    json_set_str(d, "repo", repo);
    det_str(d, "branch", branch);
    det_str(d, "headSha", sha);
    json_t *ids = json_arr();
    bool all_done = json_len(runs) > 0;
    json_t *fj = json_arr();
    for (size_t i = 0; i < json_len(runs); i++) {
        const json_t *r = json_at(runs, i);
        json_push(ids, json_clone(json_get(r, "id")));
        if (!str_eq(json_get_str(r, "status"), "completed")) all_done = false;
        const json_t *jobs = json_get(r, "jobs");
        for (size_t k = 0; k < json_len(jobs); k++)
            if (job_failed(json_at(jobs, k))) {
                const char *wf = json_get_str(r, "workflowName");
                char *label = wf ? xstrdup(wf) : xasprintf("run %lld", (long long)json_get_int(r, "id", 0));
                json_push(fj, json_str_own(xasprintf("%s: %s", label, json_get_str(json_at(jobs, k), "name"))));
                free(label);
            }
    }
    json_set(d, "runIds", ids);
    json_set_str(d, "status", all_done ? "completed" : "in_progress");
    json_set_str(d, "conclusion", runs_outcome(runs));
    json_set(d, "failedJobs", fj);
    json_t *w = json_obj();
    json_set_str(w, "mode", "commit");
    json_set_str(w, "state", state);
    json_set_str(w, "repo", repo);
    det_str(w, "branch", branch);
    det_str(w, "headSha", sha);
    if (poll > 0) json_set_int(w, "pollCount", poll);
    det_str(w, "note", note);
    json_t *rs = json_arr();
    double now = (double)time(NULL) * 1000.0;
    for (size_t i = 0; i < json_len(runs); i++) json_push(rs, watch_run_details(json_at(runs, i), now));
    json_set(w, "runs", rs);
    json_set(w, "failedLogs", logs ? failed_log_details(logs) : json_arr());
    json_set(d, "watch", w);
    return d;
}

static void emit_update(tool_ctx_t *ctx, char *text, json_t *details) {
    if (ctx && ctx->update) {
        tool_result_t p = {0};
        tool_result_text(&p, text);
        p.details = details;
        ctx->update(ctx, &p);
        tool_result_free(&p);
    } else {
        json_free(details);
    }
    free(text);
}

/* fetchFailedJobLogs: pairs [{run, job}] -> logs */
static json_t *fetch_failed_logs(const char *cwd, const char *repo, json_t *pairs, int64_t tail) {
    gh_ref_t ref;
    gh_parse_ref(repo, &ref);
    json_t *logs = json_arr();
    for (size_t i = 0; i < json_len(pairs); i++) {
        const json_t *p = json_at(pairs, i);
        strvec_t a = gh_args("api", NULL);
        gh_api_host_args(&a, &ref);
        strvec_push_own(&a, xasprintf("/repos/%s/actions/jobs/%lld/logs", ref.slug, (long long)json_get_int(json_get(p, "job"), "id", 0)));
        gh_res_t r;
        char *err = NULL;
        json_t *e = json_obj();
        json_set(e, "run", json_clone(json_get(p, "run")));
        json_set(e, "job", json_clone(json_get(p, "job")));
        /* raw escapes in the log; an older gh that rejects the flag gets the call again without it (omp 2c94ff91cd) */
        strvec_t ae = {0};
        for (size_t k = 0; k < a.n; k++) strvec_push(&ae, a.v[k]);
        strvec_push(&ae, "--allow-escape-sequences");
        int rc = gh_run(cwd, &ae, true, &r, &err);
        strvec_free(&ae);
        if (rc == 0 && r.exit_code != 0 && r.err && str_starts(r.err, "unknown flag: --allow-escape-sequences") &&
            (r.err[38] == 0 || r.err[38] == '\n' || (r.err[38] == '\r' && r.err[39] == '\n'))) {
            gh_res_free(&r);
            rc = gh_run(cwd, &a, true, &r, &err);
        }
        if (rc == 0) {
            char *full = r.exit_code == 0 ? gh_norm_block(r.out) : NULL;
            if (full && !*full) {
                free(full);
                full = NULL;
            }
            if (full) {
                strvec_t lines = str_split(full, "\n");
                size_t from = lines.n > (size_t)tail ? lines.n - (size_t)tail : 0;
                sb_t b = SB_INIT;
                for (size_t k = from; k < lines.n; k++) sb_appendf(&b, "%s%s", k > from ? "\n" : "", lines.v[k]);
                strvec_free(&lines);
                char *t = gh_js_trim_end(sb_cstr(&b));
                sb_free(&b);
                json_set_str(e, "full", full);
                if (*t) json_set_str(e, "tail", t);
                free(t);
                json_set_bool(e, "available", true);
            } else {
                char *er = gh_norm_text(r.err);
                char *reason = r.exit_code == 0 ? xstrdup("gh returned empty log content (exit code 0).")
                                                : *er ? xasprintf("gh failed with exit code %d: %s", r.exit_code, er)
                                                      : xasprintf("gh failed with exit code %d.", r.exit_code);
                if (strlen(reason) > 512) {
                    size_t cut = 509;
                    while (cut && ((unsigned char)reason[cut] & 0xC0) == 0x80) cut--;
                    char *t = xasprintf("%.*s…", (int)cut, reason);
                    free(reason);
                    reason = t;
                }
                json_set_bool(e, "available", false);
                json_set_str(e, "unavailableReason", reason);
                free(reason);
                free(er);
            }
            free(full);
            if (r.out_truncated) json_set_bool(e, "truncated", true);
            gh_res_free(&r);
        } else {
            json_set_bool(e, "available", false);
            json_set_str(e, "unavailableReason", err);
            free(err);
        }
        strvec_free(&a);
        json_push(logs, e);
    }
    gh_ref_free(&ref);
    return logs;
}

static const char *logs_artifact_label(const json_t *logs) {
    bool any = false, all = true;
    for (size_t i = 0; i < json_len(logs); i++) {
        const json_t *e = json_at(logs, i);
        bool full = json_get_bool(e, "available", false) && json_get_str(e, "full");
        if (full) any = true;
        if (!full || json_get_bool(e, "truncated", false)) all = false;
    }
    if (!any) return "Failed-job log diagnostics";
    return all ? "Full failed-job logs" : "Partial failed-job logs";
}

static char *save_artifact(tool_env_t *env, const char *text) {
    if (!env->artifacts) return NULL;
    char *path = NULL, *id = NULL;
    path = artifacts_alloc(env->artifacts, "github", &id);
    if (!path) {
        free(id);
        return NULL;
    }
    fs_write(path, text, strlen(text));
    free(path);
    return id;
}

static bool rate_limited(const char *err) {
    return err && (str_istr(err, "rate limit") || strstr(err, "HTTP 429") || str_istr(err, "abuse detection"));
}

static void op_run_watch(tool_env_t *env, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    char *branch_in = opt_str(args, "branch"), *explicit_repo = opt_str(args, "repo"), *run_s = opt_str(args, "run");
    char *run_repo = NULL, *repo = NULL, *err = NULL, *branch = NULL, *sha = NULL;
    int64_t run_id = -1;
    json_t *cache = NULL;
    if (run_s) {
        if (strspn(run_s, "0123456789") == strlen(run_s)) run_id = strtoll(run_s, NULL, 10);
        else {
            /* https://HOST/OWNER/REPO/actions/runs/N[/...] */
            char *r = NULL;
            int64_t n = -1;
            if (str_starts(run_s, "https://")) {
                char *tmp = str_replace(run_s, "/actions/runs/", "/actionsruns/");
                n = gh_parse_item_url(tmp, "actionsruns", &r);
                free(tmp);
            }
            if (n < 0) {
                tool_result_error(out, "run must be a numeric workflow run ID or a full GitHub Actions run URL");
                goto done;
            }
            run_repo = r;
            run_id = n;
        }
    }
    if (!(repo = gh_resolve_repo(env->cwd, explicit_repo, run_repo, &err))) {
        fail(out, err);
        err = NULL;
        goto done;
    }
    int64_t tail = RUN_WATCH_TAIL_DEFAULT;
    if (json_get(args, "tail")) {
        double t = json_get_num(args, "tail", 0);
        if (!isfinite(t) || t <= 0) {
            tool_result_error(out, "tail must be a positive number");
            goto done;
        }
        tail = (int64_t)floor(t) < RUN_WATCH_TAIL_MAX ? (int64_t)floor(t) : RUN_WATCH_TAIL_MAX;
    }
    int64_t start = mono_ms();
    int failures = 0;
#define INTERVAL() ((mono_ms() - start < RUN_WATCH_FAST_WINDOW_MS ? RUN_WATCH_INTERVAL_DEFAULT : RUN_WATCH_INTERVAL_SLOW) * 1000)
#define POLL_ERROR()                                                                                                           \
    do {                                                                                                                       \
        if (cancelled(ctx)) {                                                                                                  \
            tool_result_error(out, "Operation aborted");                                                                       \
            goto done;                                                                                                         \
        }                                                                                                                      \
        failures++;                                                                                                            \
        if (!rate_limited(err) || failures > RUN_WATCH_MAX_POLL_FAILURES) {                                                    \
            fail(out, err);                                                                                                    \
            err = NULL;                                                                                                        \
            goto done;                                                                                                         \
        }                                                                                                                      \
        free(err);                                                                                                             \
        err = NULL;                                                                                                            \
        if (!wait_ms(ctx, RUN_WATCH_INTERVAL_SLOW * 1000)) {                                                                   \
            tool_result_error(out, "Operation aborted");                                                                       \
            goto done;                                                                                                         \
        }                                                                                                                      \
    } while (0)
    if (run_id >= 0) {
        for (int poll = 1;; poll++) {
            if (cancelled(ctx)) {
                tool_result_error(out, "Operation aborted");
                goto done;
            }
            json_t *run = fetch_run_snapshot(env->cwd, repo, run_id, &err);
            if (!run) {
                POLL_ERROR();
                continue;
            }
            failures = 0;
            emit_update(ctx, format_run_snapshot(repo, run, poll, NULL, false), run_watch_details(repo, run, "watching", poll, NULL, NULL));
            size_t failed = count_failed(json_get(run, "jobs"));
            bool completed = str_eq(json_get_str(run, "status"), "completed");
            if (failed) {
                if (!completed) {
                    char *note = xasprintf("Failure detected. Waiting %ds to capture concurrent failures before fetching logs.",
                                           RUN_WATCH_GRACE);
                    emit_update(ctx, format_run_snapshot(repo, run, poll, note, false),
                                run_watch_details(repo, run, "watching", poll, note, NULL));
                    free(note);
                    if (!wait_ms(ctx, RUN_WATCH_GRACE * 1000)) {
                        json_free(run);
                        tool_result_error(out, "Operation aborted");
                        goto done;
                    }
                    char *e2 = NULL;
                    json_t *re = fetch_run_snapshot(env->cwd, repo, run_id, &e2);
                    free(e2);
                    if (re && count_failed(json_get(re, "jobs"))) {
                        json_free(run);
                        run = re;
                    } else {
                        json_free(re);
                    }
                }
                json_t *pairs = json_arr();
                const json_t *jobs = json_get(run, "jobs");
                for (size_t i = 0; i < json_len(jobs); i++)
                    if (job_failed(json_at(jobs, i))) {
                        json_t *p = json_obj();
                        json_set(p, "run", json_clone(run));
                        json_set(p, "job", json_clone(json_at(jobs, i)));
                        json_push(pairs, p);
                    }
                json_t *logs = fetch_failed_logs(env->cwd, repo, pairs, tail);
                json_free(pairs);
                char *full = format_run_result(repo, run, logs, tail, true);
                char *aid = save_artifact(env, full);
                free(full);
                char *t = format_run_result(repo, run, logs, tail, false);
                text_result(out, t, aid, logs_artifact_label(logs), false);
                free(t);
                out->details = run_watch_details(repo, run, "completed", 0, NULL, logs);
                if (aid) json_set_str(out->details, "artifactId", aid);
                free(aid);
                json_free(logs);
                json_free(run);
                goto done;
            }
            if (completed) {
                char *t = format_run_result(repo, run, NULL, tail, false);
                text_result(out, t, NULL, NULL, false);
                free(t);
                out->details = run_watch_details(repo, run, "completed", 0, NULL, NULL);
                json_free(run);
                goto done;
            }
            json_free(run);
            if (!wait_ms(ctx, INTERVAL())) {
                tool_result_error(out, "Operation aborted");
                goto done;
            }
        }
    }
    if (branch_in) {
        branch = xstrdup(branch_in);
        gh_ref_t ref;
        gh_parse_ref(repo, &ref);
        char *eb = gh_uri_component(branch);
        strvec_t a = gh_args("api", NULL);
        gh_api_host_args(&a, &ref);
        strvec_push(&a, "--method");
        strvec_push(&a, "GET");
        strvec_push_own(&a, xasprintf("/repos/%s/branches/%s", ref.slug, eb));
        free(eb);
        json_t *resp = gh_json(env->cwd, &a, true, true, &err);
        strvec_free(&a);
        gh_ref_free(&ref);
        if (!resp) {
            fail(out, err);
            err = NULL;
            goto done;
        }
        sha = gh_norm_opt(json_sv(gh_oc(gh_nn(json_get(resp, "commit")), "sha")));
        json_free(resp);
        if (!sha) {
            tool_result_error(out, "head SHA for branch %s must not be empty", branch);
            goto done;
        }
    } else {
        char *e2 = NULL;
        char *cwd_repo = gh_repo_from_cwd(env->cwd, &e2);
        free(e2);
        if (!gh_repo_slug_equals(cwd_repo, repo)) {
            tool_result_error(out,
                              "Cannot infer the watched commit for %s: current checkout is %s. Pass `branch` or `run` to scope the watch.",
                              repo, cwd_repo ? cwd_repo : "not a GitHub repository");
            free(cwd_repo);
            goto done;
        }
        free(cwd_repo);
        branch = git_out(env->cwd, NULL, "symbolic-ref", "--quiet", "--short", "HEAD", NULL);
        if (!branch || !*branch) {
            tool_result_error(out, "Current git branch is unavailable. Pass `branch` or `run` explicitly.");
            goto done;
        }
        sha = git_out(env->cwd, NULL, "rev-parse", "HEAD", NULL);
        if (!sha || !*sha) {
            tool_result_error(out, "Current git HEAD is unavailable. Pass `run` explicitly.");
            goto done;
        }
    }
    cache = json_obj();
    char *settled = NULL;
    bool ever = false;
    for (int poll = 1;; poll++) {
        if (cancelled(ctx)) {
            tool_result_error(out, "Operation aborted");
            free(settled);
            goto done;
        }
        json_t *runs = fetch_runs_for_commit(env->cwd, repo, sha, cache, &err);
        if (!runs) {
            if (cancelled(ctx)) {
                free(settled);
                tool_result_error(out, "Operation aborted");
                goto done;
            }
            failures++;
            if (!rate_limited(err) || failures > RUN_WATCH_MAX_POLL_FAILURES) {
                fail(out, err);
                err = NULL;
                free(settled);
                goto done;
            }
            free(err);
            err = NULL;
            if (!wait_ms(ctx, RUN_WATCH_INTERVAL_SLOW * 1000)) {
                free(settled);
                tool_result_error(out, "Operation aborted");
                goto done;
            }
            continue;
        }
        failures = 0;
        if (json_len(runs)) ever = true;
        emit_update(ctx, format_commit_snapshot(repo, sha, branch, runs, poll, NULL),
                    commit_watch_details(repo, sha, branch, runs, "watching", poll, NULL, NULL));
        const char *outcome = runs_outcome(runs);
        if (!strcmp(outcome, "failure")) {
            char *note = xasprintf("Failure detected. Waiting %ds to capture concurrent failures before fetching logs.", RUN_WATCH_GRACE);
            emit_update(ctx, format_commit_snapshot(repo, sha, branch, runs, poll, note),
                        commit_watch_details(repo, sha, branch, runs, "watching", poll, note, NULL));
            free(note);
            if (!wait_ms(ctx, RUN_WATCH_GRACE * 1000)) {
                json_free(runs);
                free(settled);
                tool_result_error(out, "Operation aborted");
                goto done;
            }
            char *e2 = NULL;
            json_t *re = fetch_runs_for_commit(env->cwd, repo, sha, cache, &e2);
            free(e2);
            size_t refailed = 0;
            for (size_t i = 0; re && i < json_len(re); i++) refailed += count_failed(json_get(json_at(re, i), "jobs"));
            if (re && refailed) {
                json_free(runs);
                runs = re;
            } else {
                json_free(re);
            }
            json_t *pairs = json_arr();
            for (size_t i = 0; i < json_len(runs); i++) {
                const json_t *r = json_at(runs, i), *jobs = json_get(r, "jobs");
                for (size_t k = 0; k < json_len(jobs); k++)
                    if (job_failed(json_at(jobs, k))) {
                        json_t *p = json_obj();
                        json_set(p, "run", json_clone(r));
                        json_set(p, "job", json_clone(json_at(jobs, k)));
                        json_push(pairs, p);
                    }
            }
            json_t *logs = fetch_failed_logs(env->cwd, repo, pairs, tail);
            json_free(pairs);
            char *full = format_commit_result(repo, sha, branch, runs, logs, tail, true);
            char *aid = save_artifact(env, full);
            free(full);
            char *t = format_commit_result(repo, sha, branch, runs, logs, tail, false);
            text_result(out, t, aid, logs_artifact_label(logs), false);
            free(t);
            out->details = commit_watch_details(repo, sha, branch, runs, "completed", 0, NULL, logs);
            if (aid) json_set_str(out->details, "artifactId", aid);
            free(aid);
            json_free(logs);
            json_free(runs);
            free(settled);
            goto done;
        }
        if (!strcmp(outcome, "success")) {
            /* signature: sorted run ids */
            VEC(int64_t) ids = {0};
            for (size_t i = 0; i < json_len(runs); i++) vec_push(&ids, json_get_int(json_at(runs, i), "id", 0));
            for (size_t i = 1; i < ids.n; i++)
                for (size_t k = i; k > 0 && ids.v[k - 1] > ids.v[k]; k--) {
                    int64_t t = ids.v[k];
                    ids.v[k] = ids.v[k - 1];
                    ids.v[k - 1] = t;
                }
            sb_t sig = SB_INIT;
            for (size_t i = 0; i < ids.n; i++) sb_appendf(&sig, "%s%lld", i ? "," : "", (long long)ids.v[i]);
            vec_free(&ids);
            if (settled && !strcmp(settled, sb_cstr(&sig))) {
                char *t = format_commit_result(repo, sha, branch, runs, NULL, tail, false);
                text_result(out, t, NULL, NULL, false);
                free(t);
                out->details = commit_watch_details(repo, sha, branch, runs, "completed", 0, NULL, NULL);
                sb_free(&sig);
                json_free(runs);
                free(settled);
                goto done;
            }
            free(settled);
            settled = sb_detach(&sig);
            int64_t confirm = INTERVAL() / 1000;
            char *note = xasprintf("All known workflow runs completed successfully. Waiting %llds to ensure no additional runs appear "
                                   "for this commit.",
                                   (long long)confirm);
            emit_update(ctx, format_commit_snapshot(repo, sha, branch, runs, poll, note),
                        commit_watch_details(repo, sha, branch, runs, "watching", poll, note, NULL));
            free(note);
            json_free(runs);
            if (!wait_ms(ctx, confirm * 1000)) {
                free(settled);
                tool_result_error(out, "Operation aborted");
                goto done;
            }
            continue;
        }
        free(settled);
        settled = NULL;
        if (!ever && mono_ms() - start >= RUN_WATCH_NO_RUNS_GIVE_UP_MS) {
            char *ss = gh_short_sha(sha);
            char *t = xasprintf("No workflow runs found for %s@%s after %llds (%d polls). The commit may not trigger any GitHub "
                                "Actions workflows, or Actions may be disabled for this repository. Pass `run` to watch a specific run.",
                                repo, ss ? ss : sha, (long long)llround((double)(mono_ms() - start) / 1000.0), poll);
            free(ss);
            text_result(out, t, NULL, NULL, true);
            free(t);
            out->details = commit_watch_details(repo, sha, branch, runs, "completed", poll, NULL, NULL);
            json_free(runs);
            goto done;
        }
        json_free(runs);
        if (!wait_ms(ctx, INTERVAL())) {
            tool_result_error(out, "Operation aborted");
            goto done;
        }
    }
#undef INTERVAL
#undef POLL_ERROR
done:
    json_free(cache);
    free(err);
    free(branch);
    free(sha);
    free(repo);
    free(run_repo);
    free(run_s);
    free(branch_in);
    free(explicit_repo);
}

/* ---------- tool ---------- */

static int github_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    gh_tool_t *g = self->ud;
    tool_env_t *env = g->env;
    const char *op = json_get_str(args, "op");
    if (str_eq(op, "repo_view")) op_repo_view(env, args, out);
    else if (str_eq(op, "file_read")) op_file_read(env, args, out);
    else if (str_eq(op, "pr_create")) op_pr_create(env, args, out);
    else if (str_eq(op, "pr_checkout")) op_pr_checkout(env, args, out);
    else if (str_eq(op, "pr_push")) op_pr_push(env, args, out);
    else if (str_eq(op, "search_issues")) op_search(env, args, S_ISSUES, out);
    else if (str_eq(op, "search_prs")) op_search(env, args, S_PRS, out);
    else if (str_eq(op, "search_code")) op_search(env, args, S_CODE, out);
    else if (str_eq(op, "search_commits")) op_search(env, args, S_COMMITS, out);
    else if (str_eq(op, "search_repos")) op_search(env, args, S_REPOS, out);
    else if (str_eq(op, "run_watch")) op_run_watch(env, ctx, args, out);
    else tool_result_error(out, "Unknown github op: %s", op ? op : "(missing)");
    return 0;
}

static void github_destroy(agent_tool_t *t) { free(t->ud); }

agent_tool_t *tool_github_new(tool_env_t *env) {
    if (!gh_available()) return NULL;
    char *desc = tool_prompt("github", NULL);
    json_t *s = schema_lit(
        "{\"type\":\"object\",\"properties\":{"
        "\"op\":{\"type\":\"string\",\"enum\":[\"repo_view\",\"file_read\",\"pr_create\",\"pr_checkout\",\"pr_push\",\"search_issues\","
        "\"search_prs\",\"search_code\",\"search_commits\",\"search_repos\",\"run_watch\"],\"description\":\"github operation\"},"
        "\"repo\":{\"type\":\"string\",\"description\":\"owner/repo\"},"
        "\"branch\":{\"type\":\"string\",\"description\":\"branch\"},"
        "\"path\":{\"type\":\"string\",\"description\":\"repository-relative file path\"},"
        "\"pr\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"array\",\"items\":{\"type\":\"string\"}}],\"description\":\"pr number, url, or branch\"},"
        "\"force\":{\"type\":\"boolean\",\"description\":\"reset existing local branch\"},"
        "\"forceWithLease\":{\"type\":\"boolean\",\"description\":\"force-with-lease push\"},"
        "\"title\":{\"type\":\"string\",\"description\":\"pr title\"},"
        "\"body\":{\"type\":\"string\",\"description\":\"pr body markdown\"},"
        "\"base\":{\"type\":\"string\",\"description\":\"pr base branch\"},"
        "\"head\":{\"type\":\"string\",\"description\":\"pr head branch\"},"
        "\"draft\":{\"type\":\"boolean\",\"description\":\"open pr as draft\"},"
        "\"fill\":{\"type\":\"boolean\",\"description\":\"auto-fill pr title/body from commits\"},"
        "\"reviewer\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"reviewers\"},"
        "\"assignee\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"assignees\"},"
        "\"label\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"labels\"},"
        "\"query\":{\"type\":\"string\",\"description\":\"search query\"},"
        "\"since\":{\"type\":\"string\",\"description\":\"lower-bound date filter\"},"
        "\"until\":{\"type\":\"string\",\"description\":\"upper-bound date filter\"},"
        "\"dateField\":{\"type\":\"string\",\"enum\":[\"created\",\"updated\"],\"description\":\"date field\"},"
        "\"limit\":{\"type\":\"number\",\"description\":\"max results\"},"
        "\"run\":{\"type\":\"string\",\"description\":\"actions run id or url\"},"
        "\"tail\":{\"type\":\"number\",\"description\":\"log lines per failed job\"}"
        "},\"required\":[\"op\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("github", "GitHub", desc, s, github_execute);
    free(desc);
    gh_tool_t *g = xcalloc(1, sizeof *g);
    g->env = env;
    t->ud = g;
    t->destroy = github_destroy;
    return t;
}
