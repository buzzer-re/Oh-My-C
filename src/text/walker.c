/* walker.c - parallel directory walker. See walker.h. */
#define _GNU_SOURCE
#include "text/walker.h"

#include "core/fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define WALK_MAX_IGNORE_FILE (8u << 20)

/* ---------------- ignore chain ---------------- */
typedef struct ignode {
    struct ignode *parent;
    ignore_set_t *set;
    char *strip;  /* dir (relative to walk root) holding the rules; "" for root/ancestors */
    char *prefix; /* path from the rules' dir down to the walk root ("" if none) */
    int refs;
} ignode_t;

static ignode_t *ignode_new(ignode_t *parent, ignore_set_t *set, const char *strip, const char *prefix) {
    ignode_t *n = xcalloc(1, sizeof(*n));
    n->parent = parent;
    if (parent) __atomic_add_fetch(&parent->refs, 1, __ATOMIC_RELAXED);
    n->set = set;
    n->strip = xstrdup(strip ? strip : "");
    n->prefix = xstrdup(prefix ? prefix : "");
    n->refs = 1;
    return n;
}

static void ignode_ref(ignode_t *n) {
    if (n) __atomic_add_fetch(&n->refs, 1, __ATOMIC_RELAXED);
}

static void ignode_unref(ignode_t *n) {
    while (n && __atomic_sub_fetch(&n->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        ignode_t *p = n->parent;
        ignore_set_free(n->set);
        free(n->strip);
        free(n->prefix);
        free(n);
        n = p;
    }
}

/* true if ignored */
static bool ignode_check(const ignode_t *n, const char *rel, bool is_dir) {
    char buf[4096];
    for (; n; n = n->parent) {
        const char *subject = rel;
        size_t sl = strlen(n->strip);
        if (sl) {
            if (strncmp(rel, n->strip, sl) != 0 || rel[sl] != '/') continue;
            subject = rel + sl + 1;
        }
        char *heap = NULL;
        if (n->prefix[0]) {
            int k = snprintf(buf, sizeof(buf), "%s/%s", n->prefix, subject);
            if (k < 0 || (size_t)k >= sizeof(buf)) subject = heap = xasprintf("%s/%s", n->prefix, subject);
            else subject = buf;
        }
        ignore_result_t r = ignore_set_match(n->set, subject, is_dir);
        free(heap);
        if (r == IGNORE_IGNORED) return true;
        if (r == IGNORE_WHITELISTED) return false;
    }
    return false;
}

/* ---------------- walker state ---------------- */
typedef struct job {
    char *abs, *rel;
    int depth;
    bool in_git;
    ignode_t *ign;
    struct job *next;
} job_t;

typedef struct {
    const walk_opts_t *o;
    walk_cb_t cb;
    void *ud;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    job_t *queue;
    size_t pending;
    int idle;
    volatile int stop;
    /* visited dirs when following symlinks */
    pthread_mutex_t vmu;
    uint64_t *visited;
    size_t vcap, vn;
} walker_t;

static bool visit_once(walker_t *w, dev_t dev, ino_t ino) {
    uint64_t key = ((uint64_t)dev * 0x9E3779B97F4A7C15ULL) ^ (uint64_t)ino;
    if (key == 0) key = 1;
    pthread_mutex_lock(&w->vmu);
    if ((w->vn + 1) * 2 > w->vcap) {
        size_t ncap = w->vcap ? w->vcap * 2 : 256;
        uint64_t *nv = xcalloc(ncap, sizeof(uint64_t));
        for (size_t i = 0; i < w->vcap; i++) {
            if (!w->visited[i]) continue;
            size_t j = w->visited[i] & (ncap - 1);
            while (nv[j]) j = (j + 1) & (ncap - 1);
            nv[j] = w->visited[i];
        }
        free(w->visited);
        w->visited = nv;
        w->vcap = ncap;
    }
    size_t j = key & (w->vcap - 1);
    bool fresh = true;
    while (w->visited[j]) {
        if (w->visited[j] == key) { fresh = false; break; }
        j = (j + 1) & (w->vcap - 1);
    }
    if (fresh) { w->visited[j] = key; w->vn++; }
    pthread_mutex_unlock(&w->vmu);
    return fresh;
}

static void push_job(walker_t *w, job_t *j) {
    pthread_mutex_lock(&w->mu);
    j->next = w->queue;
    w->queue = j;
    w->pending++;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static void free_job(job_t *j) {
    free(j->abs);
    free(j->rel);
    ignode_unref(j->ign);
    free(j);
}

static char *read_at(int dfd, const char *name, size_t *len) {
    int fd = openat(dfd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (off_t)WALK_MAX_IGNORE_FILE) {
        close(fd);
        return NULL;
    }
    size_t cap = (size_t)st.st_size + 1, n = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (n + 1 >= cap) break;
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    *len = n;
    return buf;
}

static bool emit_entry(walker_t *w, const char *rel, const char *abs, int type, bool is_link, int depth, int dfd,
                       const char *name) {
    const walk_opts_t *o = w->o;
    int mask = o->types ? o->types : WALK_FILE;
    if (!(type & mask)) return true;
    if (o->include && !globset_match(o->include, rel)) return true;
    walk_entry_t e = {rel, abs, type, is_link, depth, -1, -1};
    if (o->want_stat) {
        struct stat st;
        if (fstatat(dfd, name, &st, o->follow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW) == 0) {
            e.size = (int64_t)st.st_size;
#if defined(__APPLE__)
            e.mtime_ms = (int64_t)st.st_mtimespec.tv_sec * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
            e.mtime_ms = (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
        }
    }
    if (!w->cb(&e, w->ud)) {
        w->stop = 1;
        return false;
    }
    return true;
}

static void process_dir(walker_t *w, job_t *job) {
    const walk_opts_t *o = w->o;
    int dfd = open(job->abs, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return;
    ignode_t *ign = job->ign;
    ignode_ref(ign);
    bool in_git = job->in_git;
    if (!o->no_ignore) {
        struct stat gst;
        if (!in_git && fstatat(dfd, ".git", &gst, 0) == 0) in_git = true;
        ignore_set_t *set = NULL;
        static const char *const files[] = {".gitignore", ".ignore"};
        for (int i = 0; i < 2; i++) {
            if (i == 0 && !in_git) continue;
            size_t len;
            char *data = read_at(dfd, files[i], &len);
            if (!data) continue;
            if (!set) set = ignore_set_new();
            ignore_set_add_text(set, data, len);
            free(data);
        }
        if (set && ignore_set_count(set)) {
            ignode_t *n = ignode_new(ign, set, job->rel, "");
            ignode_unref(ign);
            ign = n;
        } else {
            ignore_set_free(set);
        }
    }
    DIR *d = fdopendir(dfd);
    if (!d) {
        close(dfd);
        ignode_unref(ign);
        return;
    }
    struct dirent *de;
    sb_t rel = SB_INIT, abs = SB_INIT;
    size_t abslen = strlen(job->abs);
    bool abs_slash = abslen > 0 && job->abs[abslen - 1] == '/';
    while (!w->stop && (de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        if (strcmp(name, ".git") == 0) continue;
        if (name[0] == '.' && !o->include_hidden) continue;
        if (o->skip_node_modules && strcmp(name, "node_modules") == 0) continue;
        int type;
        bool is_link = false;
        unsigned char dt = de->d_type;
        if (dt == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
            dt = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : S_ISREG(st.st_mode) ? DT_REG : DT_FIFO;
        }
        struct stat tst;
        bool have_tst = false;
        if (dt == DT_DIR) type = WALK_DIR;
        else if (dt == DT_REG) type = WALK_FILE;
        else if (dt == DT_LNK) {
            is_link = true;
            if (o->follow_symlinks) {
                if (fstatat(dfd, name, &tst, 0) != 0) continue; /* broken link */
                have_tst = true;
                if (S_ISDIR(tst.st_mode)) type = WALK_DIR;
                else if (S_ISREG(tst.st_mode)) type = WALK_FILE;
                else continue;
            } else {
                type = WALK_SYMLINK;
            }
        } else {
            continue; /* fifo, socket, device */
        }
        sb_clear(&rel);
        if (job->rel[0]) { sb_append(&rel, job->rel); sb_putc(&rel, '/'); }
        sb_append(&rel, name);
        bool is_dir = type == WALK_DIR;
        if (!o->no_ignore && ign && ignode_check(ign, rel.s, is_dir)) continue;
        if (o->exclude && globset_match(o->exclude, rel.s)) continue;
        sb_clear(&abs);
        sb_appendn(&abs, job->abs, abslen);
        if (!abs_slash) sb_putc(&abs, '/');
        sb_append(&abs, name);
        int depth = job->depth + 1;
        if (!emit_entry(w, rel.s, abs.s, type, is_link, depth, dfd, name)) break;
        if (is_dir && (o->max_depth <= 0 || depth < o->max_depth)) {
            if (o->follow_symlinks) {
                if (!have_tst && fstatat(dfd, name, &tst, 0) != 0) continue;
                if (!visit_once(w, tst.st_dev, tst.st_ino)) continue;
            }
            job_t *j = xcalloc(1, sizeof(*j));
            j->abs = xstrdup(abs.s);
            j->rel = xstrdup(rel.s);
            j->depth = depth;
            j->in_git = in_git;
            j->ign = ign;
            ignode_ref(ign);
            push_job(w, j);
        }
    }
    sb_free(&rel);
    sb_free(&abs);
    closedir(d);
    ignode_unref(ign);
}

static void *worker_main(void *arg) {
    walker_t *w = arg;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->queue && w->pending > 0 && !w->stop) pthread_cond_wait(&w->cv, &w->mu);
        job_t *j = w->queue;
        if (!j || w->stop) {
            pthread_cond_broadcast(&w->cv);
            pthread_mutex_unlock(&w->mu);
            break;
        }
        w->queue = j->next;
        pthread_mutex_unlock(&w->mu);
        if (!w->stop) process_dir(w, j);
        free_job(j);
        pthread_mutex_lock(&w->mu);
        if (--w->pending == 0) pthread_cond_broadcast(&w->cv);
        pthread_mutex_unlock(&w->mu);
    }
    return NULL;
}

/* ---------------- root setup: repo detection, ancestor & global ignores ---------------- */

/* Add `text` rules to set, dropping lines that would ignore the walk root (`root_rel` relative to the
 * rules' directory). */
static void add_filtered(ignore_set_t *set, const char *text, size_t len, const char *root_rel) {
    size_t i = 0;
    while (i < len) {
        const char *nl = memchr(text + i, '\n', len - i);
        size_t e = nl ? (size_t)(nl - text) : len;
        bool keep = true;
        if (root_rel && root_rel[0]) {
            ignore_set_t *one = ignore_set_new();
            ignore_set_add_line(one, text + i, e - i);
            if (ignore_set_count(one) && ignore_set_match_parents(one, root_rel, true) == IGNORE_IGNORED) keep = false;
            ignore_set_free(one);
        }
        if (keep) ignore_set_add_line(set, text + i, e - i);
        i = e + 1;
    }
}

static char *global_excludes_path(void) {
    const char *home = home_dir();
    if (home) {
        char *cfg = path_join(home, ".gitconfig");
        size_t len;
        char *data = fs_read_limit(cfg, 1u << 20, &len, NULL);
        free(cfg);
        if (data) {
            strvec_t lines = str_split_lines(data, len);
            char *found = NULL;
            for (size_t i = 0; i < lines.n && !found; i++) {
                char *t = str_trim(lines.v[i]);
                if (strncasecmp(t, "excludesfile", 12) == 0) {
                    const char *eq = strchr(t, '=');
                    if (eq) {
                        char *v = str_trim(eq + 1);
                        size_t vl = strlen(v);
                        if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') {
                            memmove(v, v + 1, vl - 2);
                            v[vl - 2] = 0;
                        }
                        found = path_expand_home(v);
                        free(v);
                    }
                }
                free(t);
            }
            strvec_free(&lines);
            free(data);
            if (found) return found;
        }
    }
    const char *xdg = env_get("XDG_CONFIG_HOME");
    if (xdg) return path_join(xdg, "git/ignore");
    if (home) return path_join(home, ".config/git/ignore");
    return NULL;
}

static ignode_t *root_ignores(const char *root_abs, bool *in_git) {
    *in_git = false;
    /* find repo root */
    char *repo = NULL;
    char *cur = xstrdup(root_abs);
    for (int guard = 0; guard < 256; guard++) {
        char *g = path_join(cur, ".git");
        bool has = fs_exists(g);
        free(g);
        if (has) { repo = xstrdup(cur); break; }
        char *parent = path_dirname(cur);
        if (strcmp(parent, cur) == 0) { free(parent); break; }
        free(cur);
        cur = parent;
    }
    free(cur);
    ignode_t *chain = NULL;
    if (repo) {
        *in_git = true;
        char *root_rel = path_relative(repo, root_abs);
        if (!root_rel || strcmp(root_rel, ".") == 0) { free(root_rel); root_rel = xstrdup(""); }
        /* lowest precedence first: global excludes, info/exclude, then ancestors top-down */
        char *gpath = global_excludes_path();
        char *info = path_join(repo, ".git/info/exclude");
        const char *srcs[2] = {gpath, info};
        for (int i = 0; i < 2; i++) {
            if (!srcs[i]) continue;
            size_t len;
            char *data = fs_read_limit(srcs[i], WALK_MAX_IGNORE_FILE, &len, NULL);
            if (!data) continue;
            ignore_set_t *s = ignore_set_new();
            add_filtered(s, data, len, root_rel);
            free(data);
            if (ignore_set_count(s)) {
                ignode_t *n = ignode_new(chain, s, "", root_rel);
                ignode_unref(chain);
                chain = n;
            } else {
                ignore_set_free(s);
            }
        }
        free(gpath);
        free(info);
        if (root_rel[0]) {
            /* ancestors from repo root down to root's parent */
            strvec_t parts = str_split(root_rel, "/");
            char *dir = xstrdup(repo);
            for (size_t i = 0; i < parts.n; i++) {
                /* rules in `dir` see the root as parts[i..] */
                sb_t rr = SB_INIT;
                for (size_t k = i; k < parts.n; k++) {
                    if (k > i) sb_putc(&rr, '/');
                    sb_append(&rr, parts.v[k]);
                }
                ignore_set_t *s = ignore_set_new();
                static const char *const files[] = {".gitignore", ".ignore"};
                for (int f = 0; f < 2; f++) {
                    char *p = path_join(dir, files[f]);
                    size_t len;
                    char *data = fs_read_limit(p, WALK_MAX_IGNORE_FILE, &len, NULL);
                    free(p);
                    if (data) { add_filtered(s, data, len, rr.s); free(data); }
                }
                if (ignore_set_count(s)) {
                    ignode_t *n = ignode_new(chain, s, "", rr.s);
                    ignode_unref(chain);
                    chain = n;
                } else {
                    ignore_set_free(s);
                }
                sb_free(&rr);
                char *nd = path_join(dir, parts.v[i]);
                free(dir);
                dir = nd;
            }
            free(dir);
            strvec_free(&parts);
        }
        free(root_rel);
        free(repo);
    }
    return chain;
}

static int nthreads(const walk_opts_t *o) {
    if (o->threads > 0) return o->threads;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 12) n = 12;
    return (int)n;
}

int walk(const char *root, const walk_opts_t *opts, walk_cb_t cb, void *ud) {
    walk_opts_t defo;
    memset(&defo, 0, sizeof(defo));
    const walk_opts_t *o = opts ? opts : &defo;
    if (!root || !*root) root = ".";
    struct stat st;
    if (stat(root, &st) != 0) return -errno;
    if (!S_ISDIR(st.st_mode)) {
        int mask = o->types ? o->types : WALK_FILE;
        if (!(mask & WALK_FILE) || !S_ISREG(st.st_mode)) return 0;
        walk_entry_t e = {path_basename(root), root, WALK_FILE, false, 0, (int64_t)st.st_size, -1};
        cb(&e, ud);
        return 0;
    }
    walker_t w;
    memset(&w, 0, sizeof(w));
    w.o = o;
    w.cb = cb;
    w.ud = ud;
    pthread_mutex_init(&w.mu, NULL);
    pthread_cond_init(&w.cv, NULL);
    pthread_mutex_init(&w.vmu, NULL);
    bool in_git = false;
    ignode_t *chain = NULL;
    if (!o->no_ignore) {
        char *cwd = get_cwd();
        char *abs = path_abs(root, cwd);
        chain = root_ignores(abs, &in_git);
        free(abs);
        free(cwd);
    }
    if (o->follow_symlinks) visit_once(&w, st.st_dev, st.st_ino);
    job_t *j = xcalloc(1, sizeof(*j));
    j->abs = xstrdup(root);
    j->rel = xstrdup("");
    j->depth = 0;
    j->in_git = in_git;
    j->ign = chain;
    push_job(&w, j);
    int nt = nthreads(o);
    pthread_t *th = xcalloc((size_t)nt, sizeof(pthread_t));
    bool *ok = xcalloc((size_t)nt, sizeof(bool));
    for (int i = 1; i < nt; i++) ok[i] = pthread_create(&th[i], NULL, worker_main, &w) == 0;
    worker_main(&w);
    for (int i = 1; i < nt; i++)
        if (ok[i]) pthread_join(th[i], NULL);
    free(ok);
    free(th);
    /* drain jobs left after a stop */
    while (w.queue) {
        job_t *n = w.queue->next;
        free_job(w.queue);
        w.queue = n;
    }
    free(w.visited);
    pthread_mutex_destroy(&w.mu);
    pthread_cond_destroy(&w.cv);
    pthread_mutex_destroy(&w.vmu);
    return 0;
}

int walk_path_cmp(const char *a, const char *b) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    while (*x && *x == *y) { x++; y++; }
    int cx = *x == '/' ? 1 : (*x ? *x + 1 : 0);
    int cy = *y == '/' ? 1 : (*y ? *y + 1 : 0);
    return cx - cy;
}

typedef struct {
    pthread_mutex_t mu;
    walk_list_t *out;
} collect_t;

static bool collect_cb(const walk_entry_t *e, void *ud) {
    collect_t *c = ud;
    walk_item_t it = {xstrdup(e->path), e->type, e->is_link, e->depth, e->size, e->mtime_ms};
    pthread_mutex_lock(&c->mu);
    vec_push(c->out, it);
    pthread_mutex_unlock(&c->mu);
    return true;
}

static int item_cmp(const void *a, const void *b) {
    return walk_path_cmp(((const walk_item_t *)a)->path, ((const walk_item_t *)b)->path);
}

int walk_collect(const char *root, const walk_opts_t *opts, walk_list_t *out) {
    collect_t c;
    pthread_mutex_init(&c.mu, NULL);
    c.out = out;
    int r = walk(root, opts, collect_cb, &c);
    pthread_mutex_destroy(&c.mu);
    if (out->n > 1) qsort(out->v, out->n, sizeof(walk_item_t), item_cmp);
    return r;
}

void walk_list_free(walk_list_t *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i].path);
    vec_free(l);
}
