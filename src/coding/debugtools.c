/* debugtools.c - /debug system info, report bundles and the artifact cache (see debugtools.h). */
#include "coding/debugtools.h"

#include "core/fs.h"
#include "tools/outsink.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

extern char **environ;

/* ---------- system info (system-info.ts) ---------- */

/* Darwin kernel major -> macOS marketing name */
static const char *macos_name(const char *release) {
    static const struct {
        int major;
        const char *name;
    } NAMES[] = {{25, "Tahoe"}, {24, "Sequoia"}, {23, "Sonoma"}, {22, "Ventura"}, {21, "Monterey"}, {20, "Big Sur"}};
    int major = atoi(release);
    for (size_t i = 0; i < ARRAY_LEN(NAMES); i++)
        if (NAMES[i].major == major) return NAMES[i].name;
    return NULL;
}

/* os.platform() / os.arch() spellings */
static const char *node_platform(const char *sysname) {
    if (str_eq(sysname, "Linux")) return "linux";
    if (str_eq(sysname, "Darwin")) return "darwin";
    if (str_eq(sysname, "FreeBSD")) return "freebsd";
    return sysname;
}

static const char *node_arch(const char *machine) {
    if (str_eq(machine, "aarch64") || str_eq(machine, "arm64")) return "arm64";
    if (str_eq(machine, "x86_64") || str_eq(machine, "amd64")) return "x64";
    if (str_eq(machine, "i386") || str_eq(machine, "i686")) return "ia32";
    return machine;
}

/* the value after "key<ws>:" on a /proc-style line, trimmed (owned) or NULL */
static char *proc_field(const char *data, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = data; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        if (n > kl && !strncmp(p, key, kl) && (p[kl] == ' ' || p[kl] == '\t' || p[kl] == ':')) {
            const char *c = memchr(p, ':', n);
            if (c) {
                char *v = xstrndup(c + 1, n - (size_t)(c + 1 - p));
                char *t = str_trim(v);
                free(v);
                return t;
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    return NULL;
}

/* os.cpus()[0].model: /proc/cpuinfo `model name` (Linux), sysctl brand string (macOS), else "Unknown CPU" */
static char *cpu_model(void) {
    size_t n = 0;
    char *info = fs_read("/proc/cpuinfo", &n);
    if (info) {
        char *m = proc_field(info, "model name");
        if (!m) m = proc_field(info, "Processor");
        free(info);
        if (m && *m) return m;
        free(m);
        return xstrdup("unknown");
    }
    FILE *f = popen("sysctl -n machdep.cpu.brand_string 2>/dev/null", "r");
    if (f) {
        char buf[256] = {0};
        bool got = fgets(buf, sizeof buf, f) != NULL;
        pclose(f);
        char *t = str_trim(buf);
        if (got && *t) return t;
        free(t);
    }
    return xstrdup("Unknown CPU");
}

/* os.totalmem() / os.freemem() (libuv: MemTotal / MemAvailable on Linux) */
static void memory(int64_t *total, int64_t *avail) {
    *total = *avail = 0;
    size_t n = 0;
    char *mi = fs_read("/proc/meminfo", &n);
    if (mi) {
        char *t = proc_field(mi, "MemTotal"), *a = proc_field(mi, "MemAvailable");
        if (!a) a = proc_field(mi, "MemFree");
        if (t) *total = strtoll(t, NULL, 10) * 1024;
        if (a) *avail = strtoll(a, NULL, 10) * 1024;
        free(t), free(a), free(mi);
        return;
    }
    long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) *total = (int64_t)pages * psz;
#ifdef _SC_AVPHYS_PAGES
    long av = sysconf(_SC_AVPHYS_PAGES);
    if (av > 0 && psz > 0) *avail = (int64_t)av * psz;
#endif
}

json_t *debug_system_info(const char *cwd) {
    struct utsname un;
    memset(&un, 0, sizeof un);
    uname(&un);
    json_t *o = json_obj();
    char *os = xasprintf("%s %s (%s)", un.sysname, un.release, node_platform(un.sysname));
    if (str_eq(un.sysname, "Darwin") && macos_name(un.release)) {
        char *t = xasprintf("%s %s", os, macos_name(un.release));
        free(os);
        os = t;
    }
    json_set_str(o, "os", os);
    free(os);
    json_set_str(o, "arch", node_arch(un.machine));
    char *cpu = cpu_model();
    json_set_str(o, "cpu", cpu);
    free(cpu);
    int64_t total, avail;
    memory(&total, &avail);
    json_t *mem = json_obj();
    json_set_int(mem, "total", total);
    json_set_int(mem, "free", avail);
    json_set(o, "memory", mem);
    json_t *ver = json_obj();
    json_set_str(ver, "app", OMC_VERSION);
    json_set(o, "versions", ver);
    json_set_str(o, "cwd", cwd ? cwd : "");
    const char *sh = env_get("SHELL");
    if (!sh) sh = env_get("ComSpec");
    json_set_str(o, "shell", sh ? sh : "unknown");
    const char *term = env_get("TERM_PROGRAM");
    if (!term) term = env_get("TERM");
    if (term) json_set_str(o, "terminal", term);
    return o;
}

char *debug_format_system_info(const json_t *info) {
    const json_t *mem = json_get(info, "memory");
    char *tot = out_format_bytes((int64_t)json_get_num(mem, "total", 0));
    char *fr = out_format_bytes((int64_t)json_get_num(mem, "free", 0));
    sb_t b = SB_INIT;
    sb_append(&b, "System Information\n");
    sb_append(&b, "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
                  "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\n");
    sb_appendf(&b, "OS:      %s\n", json_get_str(info, "os"));
    sb_appendf(&b, "Arch:    %s\n", json_get_str(info, "arch"));
    sb_appendf(&b, "CPU:     %s\n", json_get_str(info, "cpu"));
    sb_appendf(&b, "Memory:  %s (%s free)\n", tot, fr);
    sb_appendf(&b, "App:     omc %s\n", json_get_str(json_get(info, "versions"), "app"));
    sb_appendf(&b, "CWD:     %s\n", json_get_str(info, "cwd"));
    sb_appendf(&b, "Shell:   %s", json_get_str(info, "shell"));
    if (json_get_str(info, "terminal")) sb_appendf(&b, "\nTerminal: %s", json_get_str(info, "terminal"));
    free(tot), free(fr);
    return sb_detach(&b);
}

static bool ci_contains(const char *s, const char *needle) {
    size_t n = strlen(needle);
    for (; *s; s++)
        if (!strncasecmp(s, needle, n)) return true;
    return false;
}

json_t *debug_sanitized_env(void) {
    static const char *const SENSITIVE[] = {"key", "secret", "token", "pass", "auth", "credential", "api", "private"};
    json_t *o = json_obj();
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char *k = xstrndup(*e, (size_t)(eq - *e));
        bool sens = false;
        for (size_t i = 0; i < ARRAY_LEN(SENSITIVE) && !sens; i++) sens = ci_contains(k, SENSITIVE[i]);
        json_set_str(o, k, sens ? "[REDACTED]" : eq + 1);
        free(k);
    }
    return o;
}

json_t *debug_memory_stats(void) {
    json_t *p = json_obj();
    int64_t rss = 0;
    size_t n = 0;
    char *st = fs_read("/proc/self/statm", &n);
    if (st) {
        long long pages_total = 0, pages_rss = 0;
        if (sscanf(st, "%lld %lld", &pages_total, &pages_rss) == 2) rss = pages_rss * (int64_t)sysconf(_SC_PAGESIZE);
        free(st);
    }
    struct rusage ru;
    int64_t max_rss = 0;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        max_rss = (int64_t)ru.ru_maxrss;
        /* Linux reports KiB, macOS bytes */
        struct utsname un;
        if (uname(&un) == 0 && !str_eq(un.sysname, "Darwin")) max_rss *= 1024;
    }
    if (!rss) rss = max_rss;
    json_set_int(p, "rss", rss);
    json_set_int(p, "maxRss", max_rss);
    json_t *o = json_obj();
    json_set(o, "process", p);
    return o;
}

/* ---------- tar.gz (utils/ar tar.ts encodeTar + gzip) ---------- */

#define TAR_BLOCK 512

static void tar_octal(unsigned char *h, size_t off, size_t len, uint64_t v) {
    char digits[32];
    snprintf(digits, sizeof digits, "%llo", (unsigned long long)v);
    size_t d = strlen(digits);
    if (d > len - 1) d = len - 1; /* callers keep values in range */
    memset(h + off, '0', len - 1 - d);
    memcpy(h + off + len - 1 - d, digits, d);
    h[off + len - 1] = 0;
}

static void tar_header(sb_t *out, const char *name, size_t nl, const char *prefix, size_t pl, uint64_t size, int mode,
                       char type) {
    unsigned char h[TAR_BLOCK];
    memset(h, 0, sizeof h);
    memcpy(h, name, MIN(nl, 100));
    tar_octal(h, 100, 8, (uint64_t)mode);
    tar_octal(h, 108, 8, 0);
    tar_octal(h, 116, 8, 0);
    tar_octal(h, 124, 12, size);
    tar_octal(h, 136, 12, 0);
    memset(h + 148, ' ', 8);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar\0", 6);
    memcpy(h + 263, "00", 2);
    memcpy(h + 345, prefix, MIN(pl, 155));
    unsigned sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++) sum += h[i];
    char cs[8];
    snprintf(cs, sizeof cs, "%06o", sum & 0777777);
    memcpy(h + 148, cs, 6);
    h[154] = 0;
    h[155] = ' ';
    sb_appendn(out, (const char *)h, TAR_BLOCK);
}

static void tar_payload(sb_t *out, const char *data, size_t n) {
    sb_appendn(out, data, n);
    size_t pad = (TAR_BLOCK - n % TAR_BLOCK) % TAR_BLOCK;
    for (size_t i = 0; i < pad; i++) sb_putc(out, 0);
}

/* splitUstarPath: [name, prefix] or false when it needs a PAX record */
static bool tar_split(const char *p, size_t n, size_t *name_off) {
    if (n <= 100) {
        *name_off = 0;
        return true;
    }
    for (size_t i = n - 1; i > 0; i--) {
        if (p[i] != '/') continue;
        if (i <= 155 && n - i - 1 > 0 && n - i - 1 <= 100) {
            *name_off = i + 1;
            return true;
        }
    }
    return false;
}

static void tar_entry(sb_t *out, const char *path, const char *data, size_t n, bool dir, int seq) {
    char *full = dir ? xasprintf("%s/", path) : xstrdup(path);
    size_t fl = strlen(full), off = 0;
    bool split = tar_split(full, fl, &off);
    if (!split) {
        /* makePaxRecord("path", ...): "<len> path=<value>\n" with len counting itself */
        char *body = xasprintf("path=%s\n", full);
        size_t bl = strlen(body), len = bl + 2;
        for (;;) {
            char d[24];
            size_t next = (size_t)snprintf(d, sizeof d, "%zu", len) + 1 + bl;
            if (next == len) break;
            len = next;
        }
        char *rec = xasprintf("%zu %s", len, body);
        char pax[32];
        snprintf(pax, sizeof pax, "PaxHeaders/%08d", seq);
        tar_header(out, pax, strlen(pax), "", 0, strlen(rec), 0644, 'x');
        tar_payload(out, rec, strlen(rec));
        free(rec), free(body);
        char pf[32];
        snprintf(pf, sizeof pf, "PaxFile/%08d", seq);
        tar_header(out, pf, strlen(pf), "", 0, dir ? 0 : n, dir ? 0755 : 0644, dir ? '5' : '0');
    } else if (off == 0) {
        tar_header(out, full, fl, "", 0, dir ? 0 : n, dir ? 0755 : 0644, dir ? '5' : '0');
    } else {
        tar_header(out, full + off, fl - off, full, off - 1, dir ? 0 : n, dir ? 0755 : 0644, dir ? '5' : '0');
    }
    if (!dir) tar_payload(out, data, n);
    free(full);
}

int debug_write_tar_gz(const char *path, const char *const *names, const char *const *data, const size_t *lens,
                       size_t n, char **err) {
    sb_t tar = SB_INIT;
    strvec_t dirs = {0};
    int seq = 0;
    for (size_t i = 0; i < n; i++) {
        /* parent directories first, once each */
        for (const char *s = strchr(names[i], '/'); s; s = strchr(s + 1, '/')) {
            char *parent = xstrndup(names[i], (size_t)(s - names[i]));
            if (!strvec_contains(&dirs, parent)) {
                strvec_push(&dirs, parent);
                tar_entry(&tar, parent, NULL, 0, true, seq++);
            }
            free(parent);
        }
        tar_entry(&tar, names[i], data[i], lens[i], false, seq++);
    }
    for (int i = 0; i < 2 * TAR_BLOCK; i++) sb_putc(&tar, 0);
    strvec_free(&dirs);
    gzFile gz = gzopen(path, "wb");
    int rc = 0;
    if (!gz) {
        *err = xasprintf("%s: %s", path, strerror(errno ? errno : EIO));
        rc = -1;
    } else {
        size_t done = 0;
        while (done < tar.len) {
            unsigned chunk = (unsigned)MIN(tar.len - done, (size_t)1 << 20);
            if (gzwrite(gz, sb_cstr(&tar) + done, chunk) != (int)chunk) {
                *err = xasprintf("%s: write failed", path);
                rc = -1;
                break;
            }
            done += chunk;
        }
        if (gzclose(gz) != Z_OK && rc == 0) {
            *err = xasprintf("%s: write failed", path);
            rc = -1;
        }
    }
    sb_free(&tar);
    return rc;
}

/* ---------- report bundle (report-bundle.ts createReportBundle) ---------- */

typedef struct {
    strvec_t names;
    VEC(char *) data;
    VEC(size_t) lens;
} members_t;

static void add_member(members_t *m, const char *name, char *data, size_t len) {
    strvec_push(&m->names, name);
    vec_push(&m->data, data);
    vec_push(&m->lens, len);
}

/* addDirectoryToArchive: every regular file under dir, recursively (depth-capped) */
static void add_dir(members_t *m, const char *dir, const char *prefix, int depth) {
    if (depth > 32) return;
    dirlist_t l;
    if (fs_list_dir(dir, &l) != 0) return;
    for (size_t i = 0; i < l.n; i++) {
        char *p = path_join(dir, l.v[i].name), *ap = xasprintf("%s/%s", prefix, l.v[i].name);
        struct stat st;
        if (!l.v[i].is_link && l.v[i].is_dir) add_dir(m, p, ap, depth + 1);
        else if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            size_t n = 0;
            char *d = fs_read(p, &n);
            if (d) add_member(m, ap, d, n);
        }
        free(p), free(ap);
    }
    dirlist_free(&l);
}

static char *iso_stamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t t = tv.tv_sec;
    gmtime_r(&t, &tm);
    return xasprintf("%04d-%02d-%02dT%02d-%02d-%02d-%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
}

static char *pretty(const json_t *j) { return json_dump(j, 2); }

char *debug_report_bundle(const char *reports_dir, const char *session_file, const char *cwd, const json_t *settings,
                          const json_t *memory_stats, strvec_t *files, char **err) {
    char *dir = reports_dir ? xstrdup(reports_dir) : path_join(omc_config_dir(), "reports");
    fs_mkdir_p(dir, 0755);
    char *stamp = iso_stamp(), *name = xasprintf("omp-report-%s.tar.gz", stamp);
    char *out = path_join(dir, name);
    free(stamp), free(name), free(dir);
    members_t m = {0};
    json_t *sys = debug_system_info(cwd);
    char *s = pretty(sys);
    add_member(&m, "system.json", s, strlen(s));
    json_free(sys);
    json_t *env = debug_sanitized_env();
    s = pretty(env);
    add_member(&m, "env.json", s, strlen(s));
    json_free(env);
    if (settings) {
        s = pretty(settings);
        add_member(&m, "config.json", s, strlen(s));
    }
    if (session_file) {
        size_t n = 0;
        char *d = fs_read(session_file, &n);
        if (d) add_member(&m, "session.jsonl", d, n);
        size_t sl = strlen(session_file);
        if (sl > 6) {
            char *adir = xstrndup(session_file, sl - 6); /* same path without .jsonl */
            add_dir(&m, adir, "artifacts", 0);
            free(adir);
        }
    }
    if (memory_stats) {
        s = pretty(memory_stats);
        add_member(&m, "memory.json", s, strlen(s));
    }
    int rc = debug_write_tar_gz(out, (const char *const *)m.names.v, (const char *const *)m.data.v, m.lens.v, m.names.n, err);
    for (size_t i = 0; i < m.names.n; i++) {
        if (rc == 0) strvec_push(files, m.names.v[i]);
        free(m.data.v[i]);
    }
    strvec_free(&m.names);
    vec_free(&m.data);
    vec_free(&m.lens);
    if (rc != 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* ---------- artifact cache ---------- */

void debug_artifact_cache_stats(const char *sessions_dir, debug_cache_stats_t *out) {
    memset(out, 0, sizeof *out);
    dirlist_t l;
    if (fs_list_dir(sessions_dir, &l) != 0) return;
    for (size_t i = 0; i < l.n; i++) {
        if (!l.v[i].is_dir || l.v[i].is_link) continue;
        char *p = path_join(sessions_dir, l.v[i].name);
        struct stat st;
        dirlist_t sub;
        if (stat(p, &st) == 0 && fs_list_dir(p, &sub) == 0) {
            for (size_t k = 0; k < sub.n; k++) {
                char *f = path_join(p, sub.v[k].name);
                struct stat fs;
                if (stat(f, &fs) == 0 && S_ISREG(fs.st_mode)) {
                    out->count++;
                    out->total_size += fs.st_size;
                }
                free(f);
            }
            dirlist_free(&sub);
            int64_t mt = (int64_t)st.st_mtime * 1000;
            if (!out->oldest_ms || mt < out->oldest_ms) out->oldest_ms = mt;
        }
        free(p);
    }
    dirlist_free(&l);
}

int debug_clear_artifact_cache(const char *sessions_dir, int days_old) {
    int removed = 0;
    time_t cutoff = time(NULL) - (time_t)days_old * 86400;
    dirlist_t l;
    if (fs_list_dir(sessions_dir, &l) != 0) return 0;
    for (size_t i = 0; i < l.n; i++) {
        if (!l.v[i].is_dir || l.v[i].is_link) continue;
        char *p = path_join(sessions_dir, l.v[i].name);
        struct stat st;
        if (stat(p, &st) == 0 && st.st_mtime < cutoff && fs_rm_rf(p) == 0) removed++;
        free(p);
    }
    dirlist_free(&l);
    return removed;
}
