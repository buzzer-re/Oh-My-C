/* selfupdate.c - omc's GitHub releases as the update source (see selfupdate.h). */
#include "coding/selfupdate.h"

#include "core/fs.h"
#include "core/json.h"
#include "core/proc.h"
#include "net/http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __APPLE__
int _NSGetExecutablePath(char *buf, uint32_t *bufsize); /* <mach-o/dyld.h> */
#endif

#define SU_DEFAULT_REPO "buzzer-re/Oh-My-C"

#if defined(__APPLE__)
#define SU_OS "macos"
#elif defined(__linux__)
#define SU_OS "linux"
#else
#define SU_OS "unknown"
#endif
#if defined(__x86_64__)
#define SU_ARCH "x86_64"
#elif defined(__aarch64__) || defined(__arm64__)
#define SU_ARCH "arm64"
#elif defined(__i386__)
#define SU_ARCH "i386"
#elif defined(__arm__)
#define SU_ARCH "arm"
#else
#define SU_ARCH "unknown"
#endif

/* ---------- versions (pi-utils version.ts) ---------- */

static bool all_digits(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

/* exact integer comparison of digit strings */
static int cmp_digits(const char *a, size_t al, const char *b, size_t bl) {
    while (al > 1 && *a == '0') a++, al--;
    while (bl > 1 && *b == '0') b++, bl--;
    if (al != bl) return al < bl ? -1 : 1;
    int c = memcmp(a, b, al);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

typedef struct {
    const char *s;
    size_t n;
} span_t;

/* split s[0..n) at '.' into at most `max` spans; returns the count (empty input = one empty span) */
static size_t split_dots(const char *s, size_t n, span_t *out, size_t max) {
    size_t k = 0, start = 0;
    for (size_t i = 0; i <= n && k < max; i++) {
        if (i == n || s[i] == '.') {
            out[k++] = (span_t){s + start, i - start};
            start = i + 1;
        }
    }
    return k;
}

int su_compare_versions(const char *a, const char *b) {
    const char *v[2] = {a ? a : "", b ? b : ""};
    span_t core[2][32], pre[2][32];
    size_t ncore[2], npre[2];
    bool has_pre[2];
    for (int k = 0; k < 2; k++) {
        const char *s = v[k];
        while (isspace((unsigned char)*s)) s++;
        size_t n = strlen(s);
        while (n && isspace((unsigned char)s[n - 1])) n--;
        if (n && (*s == 'v' || *s == 'V')) s++, n--;
        const char *plus = memchr(s, '+', n);
        if (plus) n = (size_t)(plus - s);
        const char *dash = memchr(s, '-', n);
        has_pre[k] = dash != NULL;
        size_t cn = dash ? (size_t)(dash - s) : n;
        ncore[k] = split_dots(s, cn, core[k], 32);
        npre[k] = dash ? split_dots(dash + 1, n - cn - 1, pre[k], 32) : 0;
    }
    size_t len = MAX(ncore[0], ncore[1]);
    for (size_t i = 0; i < len; i++) {
        span_t x = i < ncore[0] && all_digits(core[0][i].s, core[0][i].n) ? core[0][i] : (span_t){"0", 1};
        span_t y = i < ncore[1] && all_digits(core[1][i].s, core[1][i].n) ? core[1][i] : (span_t){"0", 1};
        int c = cmp_digits(x.s, x.n, y.s, y.n);
        if (c) return c;
    }
    if (!has_pre[0] || !has_pre[1]) return has_pre[0] == has_pre[1] ? 0 : !has_pre[0] ? 1 : -1;
    len = MAX(npre[0], npre[1]);
    for (size_t i = 0; i < len; i++) {
        if (i >= npre[0]) return -1;
        if (i >= npre[1]) return 1;
        span_t x = pre[0][i], y = pre[1][i];
        bool xn = all_digits(x.s, x.n), yn = all_digits(y.s, y.n);
        if (xn && yn) {
            int c = cmp_digits(x.s, x.n, y.s, y.n);
            if (c) return c;
        } else if (xn != yn) {
            return xn ? -1 : 1;
        } else {
            size_t m = MIN(x.n, y.n);
            int c = memcmp(x.s, y.s, m);
            if (c) return c < 0 ? -1 : 1;
            if (x.n != y.n) return x.n < y.n ? -1 : 1;
        }
    }
    return 0;
}

/* X.Y.Z with an optional -prerelease ([0-9A-Za-z.-]+) */
static bool version_ok(const char *s) {
    int dots = 0;
    const char *p = s;
    for (int part = 0; part < 3; part++) {
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
        if (part < 2) {
            if (*p != '.') return false;
            p++;
            dots++;
        }
    }
    if (!*p) return dots == 2;
    if (*p != '-' || !p[1]) return false;
    for (p++; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return false;
    return true;
}

char *su_parse_reported_version(const char *output) {
    if (!output) return NULL;
    char *t = str_trim(output);
    char *r = NULL;
    if (str_starts(t, "omc/") && strlen(t) < 64 && version_ok(t + 4)) r = xstrdup(t + 4);
    free(t);
    return r;
}

/* ---------- where releases live ---------- */

const char *su_repo(void) {
    const char *r = env_get("OMC_REPO");
    return r ? r : SU_DEFAULT_REPO;
}

char *su_latest_url(void) {
    const char *u = env_get("OMC_LATEST_URL");
    return u ? xstrdup(u) : xasprintf("https://github.com/%s/releases/latest", su_repo());
}

char *su_download_base(void) {
    const char *u = env_get("OMC_DOWNLOAD_URL");
    char *b = u ? xstrdup(u) : xasprintf("https://github.com/%s/releases/download", su_repo());
    size_t n = strlen(b);
    while (n > 1 && b[n - 1] == '/') b[--n] = 0;
    return b;
}

char *su_installer_command(bool build) {
    return xasprintf("curl -fsSL https://raw.githubusercontent.com/%s/main/install.sh | sh%s", su_repo(),
                     build ? " -s -- --build" : "");
}

const char *su_platform(bool *prebuilt) {
    static const char p[] = SU_OS "-" SU_ARCH;
    if (prebuilt)
        *prebuilt = !strcmp(p, "linux-x86_64") || !strcmp(p, "linux-arm64") || !strcmp(p, "macos-arm64");
    return p;
}

char *su_asset_name(const char *version) { return xasprintf("omc-%s-%s.tar.gz", version, su_platform(NULL)); }

/* ---------- latest release ---------- */

/* `…/tag/v<version>[?#/…]` -> "<version>"; NULL when the URL names no tag */
static char *tag_version(const char *url) {
    const char *t = url ? strstr(url, "/tag/") : NULL;
    if (!t) return NULL;
    t += 5;
    if (*t == 'v' || *t == 'V') t++;
    size_t n = strcspn(t, "?#/");
    if (!n || n > 64) return NULL;
    char *v = xstrndup(t, n);
    for (char *p = v; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '+') {
            free(v);
            return NULL;
        }
    if (!isdigit((unsigned char)v[0])) {
        free(v);
        return NULL;
    }
    return v;
}

char *su_latest_version(int64_t timeout_ms, char **err) {
    *err = NULL;
    char *url = su_latest_url();
    /* the redirect alone names the tag: the release page behind it is never fetched */
    http_req_t req = {.method = "GET", .url = url, .timeout_ms = timeout_ms, .connect_timeout_ms = timeout_ms,
                      .max_redirects = 0, .max_body = 1 << 20};
    http_resp_t resp;
    char *v = NULL;
    if (http_request(&req, &resp) != 0) {
        *err = xasprintf("could not find the latest release of %s: %s", su_repo(),
                         resp.error ? resp.error : "request failed");
    } else {
        const char *loc = http_headers_get(&resp.headers, "Location");
        if (resp.status >= 300 && resp.status < 400 && loc) {
            char *abs = url_resolve(url, loc);
            v = tag_version(abs);
            free(abs);
        } else if (resp.status == 200) {
            v = tag_version(resp.final_url);
        }
        if (!v && (resp.status == 404 || resp.status >= 500 || resp.status == 403 || resp.status == 429))
            *err = xasprintf("could not find the latest release of %s (HTTP %d)", su_repo(), resp.status);
        else if (!v) *err = xasprintf("could not find the latest release of %s (is it published?)", su_repo());
    }
    http_resp_free(&resp);
    free(url);
    return v;
}

/* ---------- downloads ---------- */

typedef struct {
    sb_t buf;
    size_t cap;
    int status;
    bool https, downgrade, over, started;
    int64_t total, last_report;
    su_start_cb on_start;
    su_progress_cb on_progress;
    void *ud;
} dl_t;

static int dl_headers(void *ud, int status, const http_headers_t *h) {
    dl_t *d = ud;
    d->status = status;
    if (status >= 300 && status < 400) {
        const char *loc = http_headers_get(h, "Location");
        if (d->https && loc && !strncasecmp(loc, "http:", 5)) {
            d->downgrade = true;
            return 1;
        }
        return 0;
    }
    if (status != 200) return 0;
    const char *cl = http_headers_get(h, "Content-Length");
    char *e = NULL;
    long long n = cl ? strtoll(cl, &e, 10) : 0;
    d->total = cl && e && !*e && n > 0 ? n : 0;
    if (d->total > 0 && (uint64_t)d->total > d->cap) {
        d->over = true;
        return 1;
    }
    d->started = true;
    if (d->on_start) d->on_start(d->ud, d->total);
    return 0;
}

static int dl_data(void *ud, const char *data, size_t len) {
    dl_t *d = ud;
    if (d->status != 200) return 0; /* a redirect's or an error page's body */
    if (len > d->cap - d->buf.len) {
        d->over = true;
        return 1;
    }
    sb_appendn(&d->buf, data, len);
    int64_t now = mono_ms();
    if (d->on_progress && (now - d->last_report >= 100 || (d->total && (int64_t)d->buf.len >= d->total))) {
        d->last_report = now;
        su_progress_t p = {(int64_t)d->buf.len, d->total};
        d->on_progress(d->ud, &p);
    }
    return 0;
}

char *su_fetch(const char *url, size_t cap, int64_t timeout_ms, su_start_cb on_start, su_progress_cb on_progress,
               void *ud, size_t *len, char **err) {
    *err = NULL;
    *len = 0;
    dl_t d = {.buf = SB_INIT, .cap = cap, .https = !strncasecmp(url, "https:", 6), .on_start = on_start,
              .on_progress = on_progress, .ud = ud};
    http_req_t req = {.method = "GET", .url = url, .timeout_ms = timeout_ms, .idle_timeout_ms = 60000,
                      .max_redirects = 10, .no_decompress = true, .on_headers = dl_headers, .on_data = dl_data, .ud = &d};
    http_resp_t resp;
    int rc = http_request(&req, &resp);
    if (d.downgrade) *err = xasprintf("download failed: %s (redirected from https to plain http)", url);
    else if (d.over) *err = xasprintf("download failed: %s (larger than %zu bytes)", url, cap);
    else if (rc != 0) *err = xasprintf("download failed: %s (%s)", url, resp.error ? resp.error : "request failed");
    else if (resp.status != 200) *err = xasprintf("download failed: %s (HTTP %d)", url, resp.status);
    else if (d.total && (int64_t)d.buf.len != d.total)
        *err = xasprintf("download failed: %s (got %zu of %lld bytes)", url, d.buf.len, (long long)d.total);
    http_resp_free(&resp);
    if (*err) {
        sb_free(&d.buf);
        return NULL;
    }
    if (d.on_progress && d.buf.len) {
        su_progress_t p = {(int64_t)d.buf.len, d.total ? d.total : (int64_t)d.buf.len};
        d.on_progress(d.ud, &p);
    }
    *len = d.buf.len;
    if (!d.buf.s) return xstrdup("");
    return sb_detach(&d.buf);
}

char *su_sha256_hex(const void *data, size_t len) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (EVP_Digest(data, len, md, &n, EVP_sha256(), NULL) != 1) n = 0;
    char *hex = xmalloc(2 * n + 1);
    for (unsigned i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
    hex[2 * n] = 0;
    return hex;
}

char *su_sums_lookup(const char *sums, const char *asset) {
    if (!sums || !asset || !*asset) return NULL;
    size_t al = strlen(asset);
    for (const char *line = sums; *line;) {
        const char *eol = strchr(line, '\n');
        size_t ll = eol ? (size_t)(eol - line) : strlen(line);
        while (ll && (line[ll - 1] == '\r' || line[ll - 1] == ' ' || line[ll - 1] == '\t')) ll--;
        size_t h = 0;
        while (h < ll && isxdigit((unsigned char)line[h])) h++;
        if (h == 64 && h < ll && (line[h] == ' ' || line[h] == '\t')) {
            size_t p = h;
            while (p < ll && (line[p] == ' ' || line[p] == '\t')) p++;
            if (p < ll && line[p] == '*') p++;
            if (ll - p == al && !memcmp(line + p, asset, al)) {
                char *r = xstrndup(line, 64);
                for (char *c = r; *c; c++) *c = (char)tolower((unsigned char)*c);
                return r;
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
    return NULL;
}

/* ---------- the binary on disk ---------- */

char *su_self_exe(void) {
#ifdef __APPLE__
    uint32_t n = 0;
    _NSGetExecutablePath(NULL, &n);
    if (!n) return NULL;
    char *buf = xmalloc(n + 1);
    if (_NSGetExecutablePath(buf, &n) != 0) {
        free(buf);
        return NULL;
    }
    char *r = path_real(buf);
    free(buf);
    return r;
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return NULL;
    buf[n] = 0;
    /* replaced while running (an update installed over it): the path still names where it lives */
    if (n > 10 && !strcmp(buf + n - 10, " (deleted)")) buf[n - 10] = 0;
    char *r = path_real(buf);
    return r ? r : xstrdup(buf);
#endif
}

char *su_update_target(bool *is_self, char **err) {
    *err = NULL;
    char *self = su_self_exe();
    const char *forced = env_get("OMC_UPDATE_TARGET");
    char *target = NULL;
    if (forced) {
        char *cwd = get_cwd();
        char *abs = path_abs(forced, cwd);
        free(cwd);
        target = path_real(abs);
        if (!target) target = abs;
        else free(abs);
    } else {
        target = self ? xstrdup(self) : NULL;
    }
    if (!target) *err = xstrdup("could not resolve the path of the running omc binary");
    *is_self = target && self && !strcmp(target, self);
    free(self);
    return target;
}

bool su_is_source_checkout(const char *exe) {
    if (!exe) return false;
    char *d = path_dirname(exe);
    bool hit = false;
    for (int depth = 0; depth < 8 && !hit && d && strcmp(d, "/") && *d; depth++) {
        if (!strcmp(path_basename(d), "build")) {
            char *root = path_dirname(d);
            char *mk = path_join(root, "Makefile"), *core = path_join(root, "src/core/core.h"),
                 *git = path_join(root, ".git");
            hit = fs_is_file(mk) && fs_is_file(core) && fs_exists(git);
            free(mk);
            free(core);
            free(git);
            free(root);
        }
        char *up = path_dirname(d);
        if (up && !strcmp(up, d)) {
            free(up);
            break;
        }
        free(d);
        d = up;
    }
    free(d);
    return hit;
}

char *su_state_dir(void) { return path_join(omc_config_dir(), "update"); }

char *su_binary_version(const char *path) {
    const char *argv[] = {path, "--version", NULL};
    static const char *const env[] = {"OMC_NO_UPDATE_CHECK=1", NULL};
    for (int attempt = 0; attempt < 10; attempt++) {
        proc_opts_t po = {.timeout_ms = 15000, .max_output = 4096, .env_extra = env};
        proc_result_t pr;
        if (proc_run(argv, &po, &pr) != 0) return NULL;
        int busy = pr.spawn_errno == ETXTBSY;
        char *v = !pr.spawn_errno && pr.exit_code == 0 ? su_parse_reported_version(pr.out) : NULL;
        proc_result_free(&pr);
        if (!busy) return v;
        free(v);
        usleep(100 * 1000); /* another thread's fork still holds the just-written file open */
    }
    return NULL;
}

/* ---------- locks ---------- */

int su_install_lock(void) {
    char *dir = su_state_dir(), *path = path_join(dir, "install.lock");
    fs_mkdir_p(dir, 0700);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(dir);
    free(path);
    if (fd < 0) return -2;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void su_install_unlock(int fd) {
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

static int lock_open(const char *path) {
    char *lp = xasprintf("%s.lock", path), *dir = path_dirname(lp);
    fs_mkdir_p(dir, 0700);
    int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(lp);
    free(dir);
    return fd;
}

int su_lock_try(const char *path) {
    int fd = lock_open(path);
    if (fd < 0) return -1;
    struct flock fl = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (fcntl(fd, F_SETLK, &fl) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool su_lock_held(const char *path) {
    int fd = lock_open(path);
    if (fd < 0) return false;
    struct flock fl = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    bool held = fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type != F_UNLCK;
    close(fd);
    return held;
}

void su_lock_release(int fd) {
    if (fd >= 0) close(fd); /* closing drops the process's record lock */
}

/* ---------- auto-update status ---------- */

static const char *const PHASES[] = {"checking", "downloading", "installing", "done", "current", "failed"};

const char *su_phase_name(su_phase_t p) { return (unsigned)p < ARRAY_LEN(PHASES) ? PHASES[p] : "failed"; }
bool su_phase_final(su_phase_t p) { return p == SU_PH_DONE || p == SU_PH_CURRENT || p == SU_PH_FAILED; }

char *su_auto_state_path(void) {
    char *d = su_state_dir(), *p = path_join(d, "auto-update.json");
    free(d);
    return p;
}

char *su_auto_log_path(void) {
    char *d = su_state_dir(), *p = path_join(d, "auto-update.log");
    free(d);
    return p;
}

static char *opt_str(const json_t *o, const char *k) {
    const char *s = json_get_str(o, k);
    return s ? xstrdup(s) : NULL;
}

bool su_state_read(const char *path, su_state_t *out) {
    memset(out, 0, sizeof *out);
    size_t n = 0;
    char *text = fs_read_limit(path, 64 * 1024, &n, NULL);
    json_t *j = text ? json_parse(text, n, NULL) : NULL;
    free(text);
    const char *ph = json_get_str(j, "phase");
    const json_t *at = json_get(j, "at");
    int k = -1;
    for (size_t i = 0; ph && i < ARRAY_LEN(PHASES); i++)
        if (!strcmp(ph, PHASES[i])) k = (int)i;
    if (k < 0 || !json_is_num(at)) {
        json_free(j);
        return false;
    }
    out->phase = (su_phase_t)k;
    out->at = (int64_t)json_get_num(j, "at", 0);
    out->target = opt_str(j, "target");
    out->version = opt_str(j, "version");
    out->error = opt_str(j, "error");
    out->received = (int64_t)json_get_num(j, "received", 0);
    out->total = (int64_t)json_get_num(j, "total", 0);
    json_free(j);
    return true;
}

int su_state_write(const char *path, const su_state_t *s) {
    json_t *j = json_obj();
    json_set_str(j, "phase", su_phase_name(s->phase));
    if (s->target) json_set_str(j, "target", s->target);
    if (s->version) json_set_str(j, "version", s->version);
    if (s->phase == SU_PH_DOWNLOADING) {
        json_set_int(j, "received", s->received);
        if (s->total) json_set_int(j, "total", s->total);
    }
    if (s->error) json_set_str(j, "error", s->error);
    json_set_int(j, "at", s->at);
    char *text = json_dump(j, 0);
    json_free(j);
    int rc = fs_write_atomic(path, text, strlen(text), 0600);
    free(text);
    return rc;
}

void su_state_free(su_state_t *s) {
    free(s->target);
    free(s->version);
    free(s->error);
    memset(s, 0, sizeof *s);
}

bool su_should_start_worker(const su_state_t *last, const char *target, int64_t now) {
    if (!last || !str_eq(last->target, target)) return true;
    return now - last->at >= SU_RETRY_AFTER_MS;
}
