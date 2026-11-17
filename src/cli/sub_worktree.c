/* sub_worktree.c - `omc worktree [list|clear|add]` (port of omp cli/worktree-cli.ts): list and garbage-collect the
 * agent-managed worktrees under ~/.omc/wt (PR checkouts made by the github tool, legacy nesting, strays), and
 * `add` a git worktree. omc has no task-isolation sandboxes or clone-first backends: `add` is a plain
 * `git worktree add`, and leftover isolation dirs (m/merged) are reported orphaned. */
#include "cli/cli_int.h"

#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "session/worktree.h"

#include <sys/stat.h>

typedef struct {
    char *path, *kind, *parent, *branch, *orphan;
} wt_entry_t;
typedef VEC(wt_entry_t *) wt_list_t;

static char *worktrees_dir(void) { return wt_base_dir(); }

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static wt_entry_t *classify(const char *dir) {
    char *git = path_join(dir, ".git");
    wt_entry_t *e = NULL;
    if (is_file(git)) {
        e = xcalloc(1, sizeof *e);
        e->path = xstrdup(dir);
        e->kind = xstrdup("pr-checkout");
        char *c = fs_read(git, NULL);
        const char *g = c ? strstr(c, "gitdir:") : NULL;
        if (!c) e->orphan = xstrdup("cannot read .git file");
        else if (!g) e->orphan = xstrdup("malformed .git file (no gitdir line)");
        else {
            g += 7;
            while (*g == ' ' || *g == '\t') g++;
            char *gd = xstrndup(g, strcspn(g, "\r\n"));
            char *t = str_trim(gd);
            free(gd);
            char *d1 = path_dirname(t), *d2 = path_dirname(d1);
            e->parent = path_dirname(d2);
            free(d1);
            free(d2);
            char *head = path_join(t, "HEAD");
            char *h = fs_read(head, NULL);
            if (h) {
                char *ht = str_trim(h);
                if (str_starts(ht, "ref:")) {
                    const char *r = ht + 4;
                    while (*r == ' ') r++;
                    if (str_starts(r, "refs/heads/")) e->branch = xstrdup(r + 11);
                }
                free(ht);
            }
            free(h);
            free(head);
            if (!is_dir(t)) e->orphan = xstrdup("parent repo no longer tracks this worktree");
            else if (!is_dir(e->parent)) e->orphan = xstrdup("parent repo missing");
            free(t);
        }
        free(c);
    } else {
        static const char *const M[] = {"m", "merged", ".omp-isolation-owner"};
        bool iso = false;
        for (size_t i = 0; i < ARRAY_LEN(M) && !iso; i++) {
            char *p = path_join(dir, M[i]);
            iso = i < 2 ? is_dir(p) : fs_exists(p);
            free(p);
        }
        if (iso) {
            e = xcalloc(1, sizeof *e);
            e->path = xstrdup(dir);
            e->kind = xstrdup("task-isolation");
            e->orphan = xstrdup("task-isolation leftover (no live task owns it)");
        }
    }
    free(git);
    return e;
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static strvec_t list_names(const char *dir, bool *ok) {
    strvec_t v = {0};
    dirlist_t dl;
    *ok = fs_list_dir(dir, &dl) == 0;
    if (*ok) {
        for (size_t i = 0; i < dl.n; i++) strvec_push(&v, dl.v[i].name);
        dirlist_free(&dl);
    }
    xqsort(v.v, v.n, sizeof *v.v, name_cmp);
    return v;
}

static void scan(wt_list_t *outp) {
    wt_list_t out = {0};
    char *root = worktrees_dir();
    bool ok;
    strvec_t top = list_names(root, &ok);
    for (size_t i = 0; i < top.n; i++) {
        char *dir = path_join(root, top.v[i]);
        if (!is_dir(dir)) {
            free(dir);
            continue;
        }
        wt_entry_t *d = classify(dir);
        if (d) {
            vec_push(&out, d);
            free(dir);
            continue;
        }
        strvec_t kids = list_names(dir, &ok);
        size_t nested = 0;
        for (size_t k = 0; k < kids.n; k++) {
            char *cd = path_join(dir, kids.v[k]);
            wt_entry_t *c = is_dir(cd) ? classify(cd) : NULL;
            if (c) {
                vec_push(&out, c);
                nested++;
            }
            free(cd);
        }
        if (!nested) {
            wt_entry_t *e = xcalloc(1, sizeof *e);
            e->path = xstrdup(dir);
            e->kind = xstrdup(kids.n ? "stray" : "empty");
            e->orphan = xstrdup(kids.n ? "no recognizable worktree contents" : "empty directory");
            vec_push(&out, e);
        }
        strvec_free(&kids);
        free(dir);
    }
    strvec_free(&top);
    free(root);
    *outp = out;
}

static void entry_free(wt_entry_t *e) {
    free(e->path);
    free(e->kind);
    free(e->parent);
    free(e->branch);
    free(e->orphan);
    free(e);
}

static json_t *entry_json(const wt_entry_t *e) {
    json_t *o = json_obj();
    json_set_str(o, "path", e->path);
    json_set_str(o, "kind", e->kind);
    if (e->parent) json_set_str(o, "parentRepo", e->parent);
    if (e->branch) json_set_str(o, "branch", e->branch);
    if (e->orphan) json_set_str(o, "orphanReason", e->orphan);
    return o;
}

static char *entry_detail(const wt_entry_t *e) {
    sb_t b = SB_INIT;
    if (!strcmp(e->kind, "pr-checkout"))
        sb_appendf(&b, "%s · %s", e->parent ? path_basename(e->parent) : "unknown repo", e->branch ? e->branch : "unknown branch");
    else if (!strcmp(e->kind, "task-isolation")) sb_append(&b, "task-isolation sandbox");
    else if (!strcmp(e->kind, "empty")) sb_append(&b, "legacy project shell");
    else sb_append(&b, "unrecognized contents");
    if (e->orphan) sb_appendf(&b, " — %s", e->orphan);
    return sb_detach(&b);
}

static void print_pretty(json_t *j) {
    char *s = cli_json_pretty(j);
    printf("%s\n", s);
    free(s);
}

static int git_run(const char *cwd, const char *const *argv, proc_result_t *pr) {
    proc_opts_t po = {.cwd = cwd, .timeout_ms = 120000};
    return proc_run(argv, &po, pr);
}

/* addWorktree */
static int wt_add(cparse_t *p) {
    const char *rel = cp_arg(p, "path");
    if (!rel) {
        fprintf(stderr, "Error: Missing required argument: path\n");
        return 1;
    }
    const char *branch = cp_str(p, "branch"), *fbranch = cp_str(p, "force-branch"), *commit = cp_arg(p, "commit");
    bool quiet = cp_bool(p, "quiet");
    if (branch && fbranch) {
        fprintf(stderr, "fatal: options '-b' and '-B' cannot be used together\n");
        return 1;
    }
    char *launch = get_cwd();
    char *cwd = cp_str(p, "cwd") ? path_abs(cp_str(p, "cwd"), launch) : xstrdup(launch);
    free(launch);
    int rc = 0;
    proc_result_t pr = {0};
    const char *top[] = {"git", "rev-parse", "--show-toplevel", NULL};
    bool repo = git_run(cwd, top, &pr) == 0 && pr.exit_code == 0;
    proc_result_free(&pr);
    if (!repo) {
        fprintf(stderr, "fatal: not a git repository (or any of the parent directories): .git\n");
        free(cwd);
        return 1;
    }
    char *wpath = path_abs(rel, cwd);
    bool non_empty = fs_exists(wpath) && !is_dir(wpath);
    dirlist_t dl;
    if (is_dir(wpath) && fs_list_dir(wpath, &dl) == 0) {
        non_empty = dl.n > 0;
        dirlist_free(&dl);
    }
    if (non_empty) {
        fprintf(stderr, "fatal: '%s' already exists\n", rel);
        free(wpath);
        free(cwd);
        return 1;
    }
    char *ref = NULL, *created = NULL;
    bool detach = false;
    if (branch || fbranch) {
        const char *b = branch ? branch : fbranch;
        const char *argv[] = {"git", "branch", fbranch ? "-f" : b, fbranch ? b : (commit ? commit : "HEAD"),
                              fbranch ? (commit ? commit : "HEAD") : NULL, NULL};
        if (git_run(cwd, argv, &pr) != 0 || pr.exit_code != 0) {
            fprintf(stderr, "%s", pr.err ? pr.err : "fatal: could not create branch\n");
            proc_result_free(&pr);
            rc = 1;
            goto out;
        }
        proc_result_free(&pr);
        ref = xstrdup(b);
        created = xstrdup(b);
    } else if (cp_bool(p, "detach")) {
        ref = xstrdup(commit ? commit : "HEAD");
        detach = true;
    } else if (commit) {
        ref = xstrdup(commit);
        char *h = xasprintf("refs/heads/%s", commit);
        const char *argv[] = {"git", "show-ref", "--verify", "--quiet", h, NULL};
        detach = git_run(cwd, argv, &pr) != 0 || pr.exit_code != 0;
        proc_result_free(&pr);
        free(h);
    } else {
        const char *b = path_basename(wpath);
        char *h = xasprintf("refs/heads/%s", b);
        const char *argv[] = {"git", "show-ref", "--verify", "--quiet", h, NULL};
        bool exists = git_run(cwd, argv, &pr) == 0 && pr.exit_code == 0;
        proc_result_free(&pr);
        free(h);
        if (!exists) {
            const char *cb[] = {"git", "branch", b, "HEAD", NULL};
            if (git_run(cwd, cb, &pr) != 0 || pr.exit_code != 0) {
                fprintf(stderr, "%s", pr.err ? pr.err : "fatal: could not create branch\n");
                proc_result_free(&pr);
                rc = 1;
                goto out;
            }
            proc_result_free(&pr);
            created = xstrdup(b);
        }
        ref = xstrdup(b);
    }
    const char *log[] = {"git", "log", "-1", "--format=%H%n%s", ref, "--", NULL};
    if (git_run(cwd, log, &pr) != 0 || pr.exit_code != 0) {
        fprintf(stderr, "fatal: invalid reference: %s\n", ref);
        proc_result_free(&pr);
        rc = 1;
        goto out;
    }
    char *sha = xstrndup(pr.out, strcspn(pr.out, "\n"));
    const char *nl = strchr(pr.out, '\n');
    char *subject = nl ? xstrndup(nl + 1, strcspn(nl + 1, "\n")) : xstrdup("");
    proc_result_free(&pr);
    char shortsha[8];
    snprintf(shortsha, sizeof shortsha, "%.7s", sha);
    if (!quiet) {
        if (created) printf("Preparing worktree (new branch '%s')\n", created);
        else if (detach) printf("Preparing worktree (detached HEAD %s)\n", shortsha);
        else printf("Preparing worktree (checking out '%s')\n", ref);
        fflush(stdout);
    }
    const char *add_d[] = {"git", "worktree", "add", "--quiet", "--detach", wpath, ref, NULL};
    const char *add_b[] = {"git", "worktree", "add", "--quiet", wpath, ref, NULL};
    proc_opts_t po = {.cwd = cwd, .timeout_ms = 600000};
    if (proc_run(detach ? add_d : add_b, &po, &pr) != 0 || pr.exit_code != 0) {
        if (pr.err && *pr.err) fputs(pr.err, stderr);
        rc = pr.exit_code ? pr.exit_code : 1;
    } else {
        if (!quiet) printf("HEAD is now at %s %s\n", shortsha, subject);
        fflush(stdout);
        /* the post-checkout hook's output (git ran it) goes to stderr like omp's runPostCheckoutHook */
        if (pr.out && *pr.out) fputs(pr.out, stderr);
        if (pr.err && *pr.err) fputs(pr.err, stderr);
    }
    proc_result_free(&pr);
    free(sha);
    free(subject);
out:
    free(ref);
    free(created);
    free(wpath);
    free(cwd);
    return rc;
}

int cmd_worktree(cparse_t *p) {
    const char *action = cp_arg(p, "action") ? cp_arg(p, "action") : "list";
    if (!strcmp(action, "add")) return wt_add(p);
    char *cwd = get_cwd();
    settings_init(cwd, NULL);
    free(cwd);
    bool json = cp_bool(p, "json");
    wt_list_t entries;
    scan(&entries);
    int rc = 0;
    if (!strcmp(action, "list")) {
        if (json) {
            json_t *a = json_arr();
            for (size_t i = 0; i < entries.n; i++) json_push(a, entry_json(entries.v[i]));
            print_pretty(a);
            json_free(a);
        } else if (!entries.n) {
            char *root = worktrees_dir(), *m = xasprintf("No agent-managed worktrees found under %s.", root), *d = cli_dim(m);
            printf("%s\n", d);
            free(d);
            free(m);
            free(root);
        } else {
            int live = 0, orphaned = 0;
            for (size_t i = 0; i < entries.n; i++) {
                const wt_entry_t *e = entries.v[i];
                char *tag = e->orphan ? cli_yellow("orphaned") : cli_green("live    ");
                printf("%s  %s\n", tag, e->path);
                free(tag);
                char *det = entry_detail(e), *dd = cli_dim(det);
                printf("          %s\n", dd);
                free(dd);
                free(det);
                if (e->orphan) orphaned++;
                else live++;
            }
            char *m = xasprintf("\n%d live · %d orphaned · %zu total", live, orphaned, entries.n), *d = cli_dim(m);
            printf("%s\n", d);
            free(d);
            free(m);
        }
    } else { /* clear */
        bool all = cp_bool(p, "all"), dry = cp_bool(p, "dry-run");
        wt_list_t targets = {0};
        for (size_t i = 0; i < entries.n; i++)
            if (all || entries.v[i]->orphan) vec_push(&targets, entries.v[i]);
        if (!targets.n) {
            if (json) printf("{\"removed\":0,\"kept\":%zu}\n", entries.n);
            else {
                char *d = cli_dim(all ? "No worktrees to remove." : "No orphaned worktrees to remove.");
                printf("%s\n", d);
                free(d);
            }
        } else if (dry) {
            if (json) {
                json_t *o = json_obj(), *a = json_arr();
                for (size_t i = 0; i < targets.n; i++) json_push_str(a, targets.v[i]->path);
                json_set(o, "wouldRemove", a);
                print_pretty(o);
                json_free(o);
            } else {
                for (size_t i = 0; i < targets.n; i++) {
                    char *y = cli_yellow("would remove");
                    printf("%s  %s\n", y, targets.v[i]->path);
                    free(y);
                }
                char *m = xasprintf("\n%zu dir%s would be removed.", targets.n, targets.n == 1 ? "" : "s"), *d = cli_dim(m);
                printf("%s\n", d);
                free(d);
                free(m);
            }
        } else {
            strvec_t prune = {0};
            json_t *results = json_arr();
            size_t ok = 0, failed = 0;
            for (size_t i = 0; i < targets.n; i++) {
                const wt_entry_t *t = targets.v[i];
                bool done = false;
                proc_result_t pr;
                if (!strcmp(t->kind, "pr-checkout") && t->parent && !t->orphan) {
                    const char *argv[] = {"git", "worktree", "remove", "--force", t->path, NULL};
                    done = git_run(t->parent, argv, &pr) == 0 && pr.exit_code == 0;
                    proc_result_free(&pr);
                }
                int r = done ? 0 : fs_rm_rf(t->path);
                if (!done && t->parent && !strvec_contains(&prune, t->parent)) strvec_push(&prune, t->parent);
                json_t *o = json_obj();
                json_set_str(o, "path", t->path);
                json_set_bool(o, "ok", r == 0);
                if (r) json_set_str(o, "error", strerror(-r));
                json_push(results, o);
                if (r) failed++;
                else ok++;
            }
            for (size_t i = 0; i < prune.n; i++) {
                const char *argv[] = {"git", "worktree", "prune", NULL};
                proc_result_t pr;
                if (git_run(prune.v[i], argv, &pr) == 0) proc_result_free(&pr);
            }
            strvec_free(&prune);
            if (json) {
                json_t *o = json_obj();
                json_set_num(o, "removed", (double)ok);
                json_set_num(o, "failed", (double)failed);
                json_set(o, "results", json_clone(results));
                print_pretty(o);
                json_free(o);
            } else {
                json_t *x;
                json_arr_foreach(results, i, x) {
                    bool good = json_get_bool(x, "ok", false);
                    char *tag = good ? cli_green("removed") : cli_red("failed ");
                    printf("%s  %s\n", tag, json_get_str(x, "path"));
                    free(tag);
                    if (!good && json_get_str(x, "error")) {
                        char *d = cli_dim(json_get_str(x, "error"));
                        printf("          %s\n", d);
                        free(d);
                    }
                }
                char *f = failed ? xasprintf("%zu failed", failed) : NULL, *fr = f ? cli_red(f) : NULL;
                char *m = fr ? xasprintf("\n%zu removed · %s", ok, fr) : xasprintf("\n%zu removed", ok), *d = cli_dim(m);
                printf("%s\n", d);
                free(d);
                free(m);
                free(fr);
                free(f);
            }
            json_free(results);
            if (failed) rc = 1;
        }
        vec_free(&targets);
    }
    for (size_t i = 0; i < entries.n; i++) entry_free(entries.v[i]);
    vec_free(&entries);
    return rc;
}
