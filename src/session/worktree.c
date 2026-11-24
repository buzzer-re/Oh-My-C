/* worktree.c - `/wt` session worktrees (session-worktree.ts) + the shared agent-managed worktree base. See worktree.h. */
#include "session/worktree.h"

#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "tools/gh.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- base dir ---------- */

/* resolveAbsoluteDir: `~`-expanded, must be absolute */
static char *abs_dir(const char *v) {
    if (!v) return NULL;
    char *t = str_trim(v);
    if (!*t) {
        free(t);
        return NULL;
    }
    char *e = path_expand_home(t);
    free(t);
    if (!path_is_absolute(e)) {
        free(e);
        return NULL;
    }
    char *n = path_normalize(e);
    free(e);
    return n;
}

char *wt_base_dir(void) {
    char *d = abs_dir(env_get("OMC_WORKTREE_DIR"));
    if (!d) d = abs_dir(env_get("OMP_WORKTREE_DIR"));
    if (!d) d = abs_dir(settings_str("worktree.base", NULL));
    return d ? d : path_join(omc_config_dir(), "wt");
}

char *wt_hash_path(const char *abs_path) {
    char *n = path_normalize(abs_path);
    uint64_t h = gh_wyhash(n, strlen(n), 0);
    free(n);
    char buf[32];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return xstrdup(buf + 9);
}

/* ---------- git ---------- */

/* run git in cwd: trimmed stdout (owned) on exit 0, else NULL with *err = trimmed stderr (owned) when err */
static char *git(const char *cwd, char **err, const char *first, ...) {
    strvec_t v = {0};
    strvec_push(&v, "git");
    va_list ap;
    va_start(ap, first);
    for (const char *s = first; s; s = va_arg(ap, const char *)) strvec_push(&v, s);
    va_end(ap);
    vec_push(&v, NULL);
    const char *env[] = {"GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=true", "LC_ALL=C", NULL};
    proc_opts_t o = {.cwd = cwd, .env_extra = env, .timeout_ms = 10 * 60 * 1000};
    proc_result_t r = {0};
    char *out = NULL;
    if (proc_run((const char *const *)v.v, &o, &r) == 0 && r.exit_code == 0) {
        out = xstrndup(r.out ? r.out : "", r.out_len);
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    } else if (err) {
        char *t = str_trim(r.err && *r.err ? r.err : r.out ? r.out : "");
        *err = *t ? t : xstrdup("git failed");
        if (!*t) free(t);
    }
    proc_result_free(&r);
    v.n--; /* the NULL terminator is not an owned string */
    strvec_free(&v);
    return out;
}

/* raw stdout (NUL-separated porcelain), owned; NULL on failure */
static char *git_raw(const char *cwd, size_t *len, const char *const *args) {
    proc_opts_t o = {.cwd = cwd, .timeout_ms = 10 * 60 * 1000};
    proc_result_t r = {0};
    char *out = NULL;
    if (proc_run(args, &o, &r) == 0 && r.exit_code == 0) {
        out = xmalloc(r.out_len + 1);
        if (r.out_len) memcpy(out, r.out, r.out_len);
        out[r.out_len] = 0;
        *len = r.out_len;
    }
    proc_result_free(&r);
    return out;
}

static char *repo_root(const char *cwd) { return git(cwd, NULL, "rev-parse", "--show-toplevel", NULL); }

static char *head_sha(const char *cwd) { return git(cwd, NULL, "rev-parse", "--verify", "--quiet", "HEAD^{commit}", NULL); }

static char *primary_root(const char *root) {
    char *common = git(root, NULL, "rev-parse", "--path-format=absolute", "--git-common-dir", NULL);
    if (!common) return NULL;
    char *r = !strcmp(path_basename(common), ".git") ? path_dirname(common) : xstrdup(common);
    free(common);
    return r;
}

static bool is_linked_worktree(const char *root) {
    char *gd = git(root, NULL, "rev-parse", "--path-format=absolute", "--git-dir", NULL);
    char *cd = git(root, NULL, "rev-parse", "--path-format=absolute", "--git-common-dir", NULL);
    bool linked = gd && cd && strcmp(gd, cd) != 0;
    free(gd);
    free(cd);
    return linked;
}

/* ---------- names / summary ---------- */

void session_wt_free(session_wt_t *w) {
    if (!w) return;
    free(w->path);
    free(w->branch);
    free(w->source_cwd);
    free(w->base_commit);
    memset(w, 0, sizeof *w);
}

char *session_wt_default_branch(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return xasprintf("wt/%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                     tm.tm_sec);
}

char *session_wt_summary(const session_wt_t *w, bool cleaned) {
    const char *status = !w->kept_changes ? "started from clean HEAD"
                         : cleaned        ? "uncommitted changes moved, source checkout cleaned"
                                          : "uncommitted changes carried over";
    return xasprintf("Moved to worktree %s on branch %s (checked out, %s).", w->path, w->branch, status);
}

/* /^[^\s~^:?*[\\]+$/, no leading '-', no trailing '/', no ".." */
static bool branch_ok(const char *b) {
    if (!*b || b[0] == '-' || str_ends(b, "/") || strstr(b, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)b; *p; p++)
        if (strchr(" \t\n\r\f\v~^:?*[\\", *p)) return false;
    return true;
}

/* ---------- carrying uncommitted changes (seed_worktree_changes) ---------- */

static int copy_path(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) { /* deleted in the source: delete in the worktree too */
        if (unlink(dst) != 0 && errno != ENOENT) return -errno;
        return 0;
    }
    char *dir = path_dirname(dst);
    fs_mkdir_p(dir, 0755);
    free(dir);
    if (S_ISDIR(st.st_mode)) return 0; /* a nested repository / submodule entry: leave it */
    unlink(dst);
    if (S_ISLNK(st.st_mode)) {
        char buf[4096];
        ssize_t n = readlink(src, buf, sizeof buf - 1);
        if (n < 0) return -errno;
        buf[n] = 0;
        return symlink(buf, dst) == 0 ? 0 : -errno;
    }
    size_t len = 0;
    char *data = fs_read(src, &len);
    if (!data) return -errno;
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
    if (fd < 0) {
        free(data);
        return -errno;
    }
    int rc = 0;
    for (size_t off = 0; off < len;) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) {
            rc = -EIO;
            break;
        }
        off += (size_t)w;
    }
    close(fd);
    free(data);
    return rc;
}

static int seed_changes(const char *src_root, const char *wt_root, char **err) {
    const char *st_args[] = {"git", "status", "--porcelain=v1", "-z", "--untracked-files=all", "--no-renames", NULL};
    size_t len = 0;
    char *st = git_raw(src_root, &len, st_args);
    if (!st) {
        *err = xstrdup("git worktree add: cannot read the source checkout status");
        return -1;
    }
    for (size_t i = 0; i < len;) {
        const char *e = st + i;
        size_t el = strlen(e);
        i += el + 1;
        if (el < 4) continue;
        char *s = path_join(src_root, e + 3), *d = path_join(wt_root, e + 3);
        int rc = copy_path(s, d);
        free(s);
        free(d);
        if (rc != 0) {
            *err = xasprintf("git worktree add: cannot carry %s: %s", e + 3, strerror(-rc));
            free(st);
            return -1;
        }
    }
    free(st);
    /* install the source index so staged hunks stay staged, then refresh its stat cache */
    char *si = git(src_root, NULL, "rev-parse", "--path-format=absolute", "--git-path", "index", NULL);
    char *di = git(wt_root, NULL, "rev-parse", "--path-format=absolute", "--git-path", "index", NULL);
    if (si && di && fs_exists(si)) {
        size_t n = 0;
        char *data = fs_read(si, &n);
        if (data) fs_write_atomic(di, data, n, 0644);
        free(data);
    }
    free(si);
    free(di);
    char *r = git(wt_root, NULL, "update-index", "-q", "--refresh", NULL);
    free(r);
    return 0;
}

/* ---------- create ---------- */

int session_wt_create(const char *cwd, const char *branch, bool keep_changes, session_wt_t *out, char **err) {
    memset(out, 0, sizeof *out);
    *err = NULL;
    char *root = repo_root(cwd);
    if (!root) {
        *err = xasprintf("Not inside a git repository: %s", cwd);
        return -1;
    }
    int rc = -1;
    char *primary = NULL, *base = NULL, *wpath = NULL, *dir = NULL, *slug = NULL, *h = NULL, *list = NULL;
    if (!branch_ok(branch)) {
        *err = xasprintf("Invalid branch name: %s", branch);
        goto out;
    }
    char *ref = xasprintf("refs/heads/%s", branch);
    char *exists = git(root, NULL, "show-ref", "--verify", "--quiet", ref, NULL);
    free(ref);
    if (exists) {
        free(exists);
        *err = xasprintf("Branch '%s' already exists; pick another name.", branch);
        goto out;
    }
    out->base_commit = head_sha(root);
    if (!out->base_commit) {
        *err = xasprintf("Cannot create a worktree: %s has no commits.", cwd);
        goto out;
    }
    primary = primary_root(root);
    if (!primary) primary = xstrdup(root);
    sb_t sl = SB_INIT;
    bool dash = false;
    for (const unsigned char *p = (const unsigned char *)branch; *p; p++) {
        if (isalnum(*p) || *p == '.' || *p == '_' || *p == '-') {
            sb_putc(&sl, (char)*p);
            dash = false;
        } else if (!dash) {
            sb_putc(&sl, '-');
            dash = true;
        }
    }
    slug = sb_detach(&sl);
    h = wt_hash_path(primary);
    dir = wt_base_dir();
    char *seg = xasprintf("%s-%s", slug, h);
    base = path_join(dir, seg);
    free(seg);
    /* resolveAvailableWorktreePath: skip registered worktrees and existing paths */
    list = git(root, NULL, "worktree", "list", "--porcelain", NULL);
    for (int attempt = 0; attempt < 100 && !wpath; attempt++) {
        char *cand = attempt ? xasprintf("%s-%d", base, attempt + 1) : xstrdup(base);
        char *needle = xasprintf("worktree %s\n", cand);
        char *line = xasprintf("worktree %s", cand);
        bool registered = list && (strstr(list, needle) || str_ends(list, line));
        free(needle);
        free(line);
        struct stat st;
        if (!registered && lstat(cand, &st) != 0 && errno == ENOENT) wpath = cand;
        else free(cand);
    }
    if (!wpath) {
        *err = xasprintf("could not find an unused worktree path under %s (tried 100 suffixes)", base);
        goto out;
    }
    char *parent = path_dirname(wpath);
    fs_mkdir_p(parent, 0755);
    free(parent);
    char *gerr = NULL;
    char *r = git(root, &gerr, "branch", "--no-track", branch, out->base_commit, NULL);
    if (!r) {
        *err = xasprintf("git branch: %s", gerr);
        free(gerr);
        goto out;
    }
    free(r);
    r = git(root, &gerr, "worktree", "add", "--quiet", wpath, branch, NULL);
    if (!r) {
        *err = xasprintf("git worktree add: %s", gerr);
        free(gerr);
        goto out;
    }
    free(r);
    if (keep_changes && seed_changes(root, wpath, err) != 0) goto out;
    char *real = path_real(wpath);
    out->path = real ? real : xstrdup(wpath);
    out->branch = xstrdup(branch);
    out->source_cwd = xstrdup(cwd);
    out->kept_changes = keep_changes;
    rc = 0;
out:
    if (rc != 0) session_wt_free(out);
    free(root);
    free(primary);
    free(base);
    free(wpath);
    free(dir);
    free(slug);
    free(h);
    free(list);
    return rc;
}

int session_wt_clean_source(const char *cwd, char **err) {
    *err = NULL;
    if (!settings_bool("worktree.cleanSource", false)) return 0;
    char *r = git(cwd, err, "reset", "--hard", "HEAD", NULL);
    if (!r) return -1;
    free(r);
    r = git(cwd, err, "clean", "-f", "-d", NULL);
    if (!r) return -1;
    free(r);
    return 1;
}

bool session_wt_can_auto_create(const char *cwd) {
    char *root = repo_root(cwd);
    if (!root) return false;
    bool ok = false;
    if (!is_linked_worktree(root)) {
        char *h = head_sha(root);
        ok = h != NULL;
        free(h);
    }
    free(root);
    return ok;
}

/* ---------- exit ---------- */

void session_wt_state_free(session_wt_state_t *st) {
    free(st->fingerprint);
    memset(st, 0, sizeof *st);
}

int session_wt_snapshot(const session_wt_t *w, session_wt_state_t *st, char **err) {
    memset(st, 0, sizeof *st);
    *err = NULL;
    bool moved = true;
    char *tip = NULL;
    if (fs_is_dir(w->source_cwd)) {
        char *ref = xasprintf("refs/heads/%s", w->branch);
        tip = git(w->source_cwd, NULL, "rev-parse", "--verify", "--quiet", ref, NULL);
        free(ref);
        moved = tip && strcmp(tip, w->base_commit) != 0;
    }
    char *root = fs_is_dir(w->path) ? repo_root(w->path) : NULL;
    if (!root) {
        st->moved = moved;
        st->fingerprint = xasprintf("missing%c%s", 0x1f, tip ? tip : "");
        free(tip);
        return 0;
    }
    free(root);
    const char *args[] = {"git", "status", "--porcelain=v1", "-z", "--untracked-files=all", NULL};
    size_t len = 0;
    char *status = git_raw(w->path, &len, args);
    if (!status) {
        *err = xasprintf("Cannot read the status of worktree %s", w->path);
        free(tip);
        return -1;
    }
    char *head = head_sha(w->path);
    sb_t parts = SB_INIT;
    sb_appendf(&parts, "%s%c%s%c", head ? head : "", 0x1f, tip ? tip : "", 0x1f);
    sb_appendn(&parts, status, len);
    for (size_t i = 0; i < len;) {
        const char *e = status + i;
        size_t el = strlen(e);
        i += el + 1;
        if (el < 4) continue;
        if (e[0] == 'R' || e[0] == 'C') i += strlen(status + i) + 1; /* followed by the source path */
        char *p = path_join(w->path, e + 3);
        struct stat sst;
        if (stat(p, &sst) == 0)
            sb_appendf(&parts, "%c%s:%lld:%lld.%09ld", 0x1f, e + 3, (long long)sst.st_size, (long long)sst.st_mtim.tv_sec,
                       sst.st_mtim.tv_nsec);
        else sb_appendf(&parts, "%c%s:-", 0x1f, e + 3);
        free(p);
    }
    char fp[32];
    snprintf(fp, sizeof fp, "%llx", (unsigned long long)gh_wyhash(parts.s, parts.len, 0));
    sb_free(&parts);
    st->present = true;
    st->dirty = len > 0;
    st->moved = moved;
    st->off_branch = head && (!tip || strcmp(head, tip) != 0);
    st->fingerprint = xstrdup(fp);
    free(head);
    free(tip);
    free(status);
    return 0;
}

const char *session_wt_decide_exit(const char *policy, const session_wt_state_t *st) {
    if (str_eq(policy, "keep")) return "keep";
    if (str_eq(policy, "force")) return "remove"; /* omp ef540f208c: every session-created worktree, without asking */
    if (str_eq(policy, "remove") && !st->dirty && !st->moved && !st->off_branch) return "remove";
    return "prompt";
}

char *session_wt_exit_message(const session_wt_t *w, const session_wt_state_t *st) {
    sb_t risk = SB_INIT;
    if (st->dirty) sb_append(&risk, "has uncommitted changes");
    if (st->moved) sb_appendf(&risk, "%shas new commits (branch is kept)", risk.len ? "; " : "");
    if (st->off_branch)
        sb_appendf(&risk, "%sHEAD is not on %s; commits only reachable from HEAD are lost", risk.len ? "; " : "", w->branch);
    char *sp = path_display(w->path, NULL);
    char *m = risk.len ? xasprintf("%s (%s): %s", sp, w->branch, risk.s) : xasprintf("%s (%s)", sp, w->branch);
    free(sp);
    sb_free(&risk);
    return m;
}

char *session_wt_remove_planned(const session_wt_t *w, const char *approved) {
    session_wt_state_t st;
    char *err = NULL;
    if (session_wt_snapshot(w, &st, &err) != 0) return err;
    char *sp = path_display(w->path, NULL);
    char *msg = NULL;
    if (st.present && approved && !str_eq(st.fingerprint, approved)) { /* NULL (force): removed regardless */
        msg = xasprintf("Kept worktree %s: it changed while the session was closing.", sp);
        goto out;
    }
    /* git cannot open the source repo while our cwd is the deleted worktree */
    char *cwd = get_cwd();
    size_t pl = strlen(w->path);
    bool holds = cwd && !strncmp(cwd, w->path, pl) && (cwd[pl] == 0 || cwd[pl] == '/');
    free(cwd);
    if (holds && chdir(w->source_cwd) != 0) {
        msg = xasprintf("Failed to remove worktree %s; branch '%s' kept.", w->path, w->branch);
        goto out;
    }
    char *r = git(w->source_cwd, NULL, "worktree", "remove", "--force", w->path, NULL);
    if (!r) {
        msg = xasprintf("Failed to remove worktree %s; branch '%s' kept.", w->path, w->branch);
        goto out;
    }
    free(r);
    if (!st.moved) {
        r = git(w->source_cwd, NULL, "branch", "-D", w->branch, NULL);
        if (!r) {
            msg = xasprintf("Removed worktree %s but failed to delete branch '%s'.", w->path, w->branch);
            goto out;
        }
        free(r);
    }
    if (holds) {
        char *commits = st.moved ? xasprintf("; its commits are on branch %s", w->branch) : xstrdup("");
        msg = xasprintf("Removed worktree %s. Resuming opens in the directory you launch omc from%s.", sp, commits);
        free(commits);
    }
out:
    free(sp);
    session_wt_state_free(&st);
    return msg;
}
