#define _GNU_SOURCE
#include "fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pwd.h>
#include <unistd.h>

char *fs_read_limit(const char *path, size_t max, size_t *len, bool *truncated) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        errno = EISDIR;
        return NULL;
    }
    size_t cap = (st.st_size > 0 && (size_t)st.st_size < max) ? (size_t)st.st_size + 1 : 65536;
    if (cap > max + 1) cap = max + 1;
    char *buf = xmalloc(cap);
    size_t n = 0;
    if (truncated) *truncated = false;
    for (;;) {
        if (n + 1 >= cap) {
            if (cap > max) break;
            cap = MIN(cap * 2, max + 1);
            buf = xrealloc(buf, cap);
        }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            free(buf);
            close(fd);
            errno = e;
            return NULL;
        }
        if (r == 0) break;
        n += (size_t)r;
        if (n >= max) {
            if (truncated) {
                char c;
                *truncated = read(fd, &c, 1) > 0;
            }
            n = max;
            break;
        }
    }
    close(fd);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

char *fs_read(const char *path, size_t *len) { return fs_read_limit(path, (size_t)1 << 34, len, NULL); }

static int write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

int fs_write_atomic(const char *path, const void *data, size_t len, mode_t mode) {
    char *dir = path_dirname(path);
    fs_mkdir_p(dir, 0755);
    char *tmp = xasprintf("%s/.%s.omc-tmp-%s", dir, path_basename(path), short_id());
    free(dir);
    struct stat st;
    if (stat(path, &st) == 0) mode = st.st_mode & 07777;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) {
        int e = -errno;
        free(tmp);
        return e;
    }
    int rc = write_all(fd, data, len);
    if (rc == 0 && fsync(fd) != 0) rc = -errno;
    close(fd);
    if (rc == 0 && rename(tmp, path) != 0) rc = -errno;
    if (rc != 0) unlink(tmp);
    free(tmp);
    return rc;
}

int fs_write(const char *path, const void *data, size_t len) {
    char *dir = path_dirname(path);
    fs_mkdir_p(dir, 0755);
    free(dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -errno;
    int rc = write_all(fd, data, len);
    close(fd);
    return rc;
}

int fs_append(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        char *dir = path_dirname(path);
        fs_mkdir_p(dir, 0755);
        free(dir);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) return -errno;
    }
    int rc = write_all(fd, data, len);
    close(fd);
    return rc;
}

int fs_mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) return 0;
    char *p = xstrdup(path);
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, mode) != 0 && errno != EEXIST) {
                int e = -errno;
                free(p);
                return e;
            }
            *s = '/';
        }
    }
    int rc = 0;
    if (mkdir(p, mode) != 0 && errno != EEXIST) rc = -errno;
    free(p);
    return rc;
}

static int rm_cb(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    UNUSED(sb);
    UNUSED(flag);
    UNUSED(ftw);
    return remove(path);
}

int fs_rm_rf(const char *path) { return nftw(path, rm_cb, 64, FTW_DEPTH | FTW_PHYS); }

bool fs_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

bool fs_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool fs_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool fs_is_exec(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

int64_t fs_mtime_ms(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
}

int64_t fs_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

bool fs_is_binary(const char *data, size_t len) {
    size_t n = MIN(len, (size_t)8192);
    if (memchr(data, 0, n)) return true;
    size_t bad = 0, i = 0;
    while (i < n) {
        uint32_t cp;
        int k = utf8_decode(data + i, n - i, &cp);
        if (k == 1 && (unsigned char)data[i] >= 0x80) {
            /* a truncated sequence at the end of the sample is fine */
            if (n - i >= 4) bad++;
        }
        i += (size_t)k;
    }
    return n && bad * 10 > n / 10 + 1; /* >~1% invalid bytes */
}

/* ---------- paths ---------- */

char *path_join(const char *a, const char *b) {
    if (!a || !*a) return xstrdup(b ? b : "");
    if (!b || !*b) return xstrdup(a);
    if (b[0] == '/') return xstrdup(b);
    size_t al = strlen(a);
    if (a[al - 1] == '/') return xasprintf("%s%s", a, b);
    return xasprintf("%s/%s", a, b);
}

char *path_join3(const char *a, const char *b, const char *c) {
    char *ab = path_join(a, b);
    char *r = path_join(ab, c);
    free(ab);
    return r;
}

char *path_dirname(const char *p) {
    const char *s = strrchr(p, '/');
    if (!s) return xstrdup(".");
    if (s == p) return xstrdup("/");
    return xstrndup(p, (size_t)(s - p));
}

const char *path_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

const char *path_ext(const char *p) {
    const char *b = path_basename(p);
    const char *d = strrchr(b, '.');
    return (d && d != b) ? d : "";
}

char *path_normalize(const char *p) {
    bool abs = p[0] == '/';
    strvec_t parts = str_split(p, "/");
    strvec_t out = {0};
    for (size_t i = 0; i < parts.n; i++) {
        char *seg = parts.v[i];
        if (!*seg || strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) {
            if (out.n && strcmp(vec_last(&out), "..") != 0) {
                free(vec_pop(&out));
                continue;
            }
            if (abs) continue;
        }
        strvec_push(&out, seg);
    }
    char *joined = strvec_join(&out, "/");
    char *r = abs ? xasprintf("/%s", joined) : (*joined ? xstrdup(joined) : xstrdup("."));
    free(joined);
    strvec_free(&parts);
    strvec_free(&out);
    return r;
}

const char *home_dir(void) {
    static char *home;
    if (home) return home;
    const char *h = env_get("HOME");
    if (h) {
        home = xstrdup(h);
    } else {
        struct passwd *pw = getpwuid(getuid());
        home = xstrdup(pw && pw->pw_dir ? pw->pw_dir : "/");
    }
    return home;
}

char *path_expand_home(const char *p) {
    if (p[0] == '~' && (p[1] == '/' || p[1] == 0)) return xasprintf("%s%s", home_dir(), p + 1);
    return xstrdup(p);
}

char *get_cwd(void) {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof buf)) return xstrdup(buf);
    const char *pwd = env_get("PWD");
    return xstrdup(pwd ? pwd : "/");
}

char *path_abs(const char *p, const char *cwd) {
    char *e = path_expand_home(p);
    char *j;
    if (e[0] == '/') {
        j = e;
    } else {
        char *c = cwd ? xstrdup(cwd) : get_cwd();
        j = path_join(c, e);
        free(c);
        free(e);
    }
    char *n = path_normalize(j);
    free(j);
    return n;
}

char *path_real(const char *p) {
    char buf[PATH_MAX];
    if (!realpath(p, buf)) return NULL;
    return xstrdup(buf);
}

char *path_relative(const char *from, const char *to) {
    strvec_t a = str_split(from, "/"), b = str_split(to, "/");
    size_t i = 0;
    /* skip empty segments uniformly */
    strvec_t fa = {0}, fb = {0};
    for (size_t k = 0; k < a.n; k++) if (*a.v[k]) strvec_push(&fa, a.v[k]);
    for (size_t k = 0; k < b.n; k++) if (*b.v[k]) strvec_push(&fb, b.v[k]);
    while (i < fa.n && i < fb.n && strcmp(fa.v[i], fb.v[i]) == 0) i++;
    sb_t r = SB_INIT;
    for (size_t k = i; k < fa.n; k++) sb_append(&r, r.len ? "/.." : "..");
    for (size_t k = i; k < fb.n; k++) {
        if (r.len) sb_putc(&r, '/');
        sb_append(&r, fb.v[k]);
    }
    strvec_free(&a);
    strvec_free(&b);
    strvec_free(&fa);
    strvec_free(&fb);
    if (!r.len) return xstrdup(".");
    return sb_detach(&r);
}

char *path_display(const char *p, const char *cwd) {
    if (cwd) {
        size_t cl = strlen(cwd);
        if (strncmp(p, cwd, cl) == 0) {
            if (p[cl] == 0) return xstrdup(".");
            if (p[cl] == '/') return xstrdup(p + cl + 1);
        }
    }
    const char *h = home_dir();
    size_t hl = strlen(h);
    if (hl > 1 && strncmp(p, h, hl) == 0 && (p[hl] == '/' || p[hl] == 0)) return xasprintf("~%s", p + hl);
    return xstrdup(p);
}

char *which(const char *prog) {
    if (strchr(prog, '/')) return fs_is_exec(prog) ? xstrdup(prog) : NULL;
    const char *path = env_get("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    strvec_t dirs = str_split(path, ":");
    char *found = NULL;
    for (size_t i = 0; i < dirs.n && !found; i++) {
        if (!*dirs.v[i]) continue;
        char *c = path_join(dirs.v[i], prog);
        if (fs_is_exec(c)) found = c;
        else free(c);
    }
    strvec_free(&dirs);
    return found;
}

const char *omc_config_dir(void) {
    static char *d;
    if (!d) {
        const char *e = env_get("OMC_CONFIG_DIR");
        d = e ? path_expand_home(e) : path_join(home_dir(), ".omc");
    }
    return d;
}

const char *omc_agent_dir(void) {
    static char *d;
    if (!d) {
        const char *e = env_get("OMC_CODING_AGENT_DIR");
        if (!e) e = env_get("PI_CODING_AGENT_DIR");
        d = e ? path_expand_home(e) : path_join(omc_config_dir(), "agent");
    }
    return d;
}

char *omc_agent_path(const char *rel) { return path_join(omc_agent_dir(), rel); }

static int cmp_dirent(const void *a, const void *b) {
    return strcmp(((const dirent_info_t *)a)->name, ((const dirent_info_t *)b)->name);
}

int fs_list_dir(const char *path, dirlist_t *out) {
    memset(out, 0, sizeof(*out));
    DIR *d = opendir(path);
    if (!d) return -errno;
    struct dirent *de;
    int dfd = dirfd(d);
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        dirent_info_t info = {0};
        info.name = xstrdup(de->d_name);
        struct stat st;
        if (fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            info.is_link = S_ISLNK(st.st_mode);
            if (info.is_link) {
                struct stat st2;
                if (fstatat(dfd, de->d_name, &st2, 0) == 0) st = st2;
            }
            info.is_dir = S_ISDIR(st.st_mode);
            info.size = (int64_t)st.st_size;
            info.mtime_ms = (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
        }
        vec_push(out, info);
    }
    closedir(d);
    if (out->n > 1) qsort(out->v, out->n, sizeof(dirent_info_t), cmp_dirent);
    return 0;
}

void dirlist_free(dirlist_t *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i].name);
    vec_free(l);
}

static const char *tmp_root(void) {
    const char *t = env_get("TMPDIR");
    return t ? t : "/tmp";
}

char *fs_mktemp(const char *prefix, const char *suffix) {
    for (int tries = 0; tries < 100; tries++) {
        char *id = rand_hex(8);
        char *p = xasprintf("%s/%s%s%s", tmp_root(), prefix ? prefix : "omc-", id, suffix ? suffix : "");
        free(id);
        int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            close(fd);
            return p;
        }
        free(p);
    }
    return NULL;
}

char *fs_mktempdir(const char *prefix) {
    char *tmpl = xasprintf("%s/%sXXXXXX", tmp_root(), prefix ? prefix : "omc-");
    if (!mkdtemp(tmpl)) {
        free(tmpl);
        return NULL;
    }
    return tmpl;
}
