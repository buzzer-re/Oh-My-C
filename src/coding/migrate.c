/* migrate.c - `/migrate` / `omc migrate`: bring an existing omp setup over to omc.
 *
 * omp keeps its configuration in ~/.omp/agent (or $PI_CODING_AGENT_DIR). Everything there that is
 * configuration — config.yml, models.yml, keybindings.yml (or a legacy keybindings.json, migrated on the
 * first omc start), mcp.json, lsp.json, ssh.json, AGENTS.md/RULES.md/SYSTEM.md,
 * skills/, rules/, agents/, commands/, prompts/, themes/, managed-skills/, memories/, sessions/ (omp's
 * JSONL format is omc's) and any other file — is copied as-is into omc's agent dir. omp-internal
 * state is left behind: its SQLite stores (*.db), caches, native modules, logs and runtime dirs.
 * Two pieces of that SQLite state are converted instead of copied:
 *   agent.db   auth_credentials -> auth.json   (one enabled credential per provider, newest first)
 *   history.db history          -> prompt-history.jsonl (TUI prompt history, oldest first)
 * Existing files in omc's dir are never overwritten unless `force` is set; `dry_run` only reports. */
#include "migrate.h"

#include "core/fs.h"
#include "core/json.h"
#include "tools/readfmt.h"

#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

const char *migrate_omp_dir(void) {
    static char *d;
    if (!d) {
        const char *e = env_get("PI_CODING_AGENT_DIR");
        d = e && *e ? path_expand_home(e) : path_join3(home_dir(), ".omp", "agent");
    }
    return d;
}

/* omp-internal state that is not configuration */
static bool excluded(const char *name) {
    static const char *const SKIP[] = {"blobs", "cache", "predict", "natives", "terminal-sessions", "logs", "run", "ssh-control",
                                       "last-changelog-version", "install-id", "gpu_cache.json", "remote-host"};
    for (size_t i = 0; i < ARRAY_LEN(SKIP); i++)
        if (!strcmp(name, SKIP[i])) return true;
    const char *dbs[] = {".db", ".db-shm", ".db-wal", ".db-journal"};
    for (size_t i = 0; i < ARRAY_LEN(dbs); i++)
        if (str_ends(name, dbs[i])) return true;
    return false;
}

typedef struct {
    const migrate_opts_t *o;
    int copied, skipped, failed;
} counts_t;

static int copy_file(const char *src, const char *dst, const struct stat *sst, counts_t *c, sb_t *rep, const char *rel) {
    mode_t mode = sst->st_mode;
    struct stat st;
    if (lstat(dst, &st) == 0 && !c->o->force) {
        c->skipped++;
        return 0;
    }
    if (c->o->dry_run) {
        c->copied++;
        return 0;
    }
    size_t len = 0;
    char *data = fs_read_limit(src, (size_t)1 << 30, &len, NULL);
    if (!data) {
        c->failed++;
        sb_appendf(rep, "  ! cannot read %s: %s\n", rel, strerror(errno));
        return -1;
    }
    char *dir = path_dirname(dst);
    fs_mkdir_p(dir, 0700);
    free(dir);
    int rc = fs_write_atomic(dst, data, len, mode & 0777);
    free(data);
    if (rc != 0) {
        c->failed++;
        sb_appendf(rep, "  ! cannot write %s: %s\n", rel, strerror(-rc));
        return -1;
    }
    /* keep omp's timestamps: session lists sort and age by modification time */
    struct timespec ts[2] = {sst->st_atim, sst->st_mtim};
    utimensat(AT_FDCWD, dst, ts, 0);
    c->copied++;
    return 0;
}

static void copy_tree(const char *src, const char *dst, const char *rel, counts_t *c, sb_t *rep, int depth) {
    if (depth > 32) return;
    struct stat st;
    if (lstat(src, &st) != 0) return;
    if (S_ISLNK(st.st_mode)) { /* keep links as links (skills are often symlinked checkouts) */
        char target[4096];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n <= 0) return;
        target[n] = 0;
        struct stat dt;
        if (lstat(dst, &dt) == 0 && !c->o->force) {
            c->skipped++;
            return;
        }
        if (!c->o->dry_run) {
            char *dir = path_dirname(dst);
            fs_mkdir_p(dir, 0700);
            free(dir);
            unlink(dst);
            if (symlink(target, dst) != 0) {
                c->failed++;
                sb_appendf(rep, "  ! cannot link %s: %s\n", rel, strerror(errno));
                return;
            }
        }
        c->copied++;
        return;
    }
    if (S_ISREG(st.st_mode)) {
        copy_file(src, dst, &st, c, rep, rel);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    if (!c->o->dry_run) fs_mkdir_p(dst, 0700);
    DIR *d = opendir(src);
    if (!d) return;
    struct dirent *e;
    strvec_t names = {0};
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        strvec_push(&names, e->d_name);
    }
    closedir(d);
    strvec_sort(&names);
    for (size_t i = 0; i < names.n; i++) {
        char *s2 = path_join(src, names.v[i]), *d2 = path_join(dst, names.v[i]), *r2 = path_join(rel, names.v[i]);
        copy_tree(s2, d2, r2, c, rep, depth + 1);
        free(s2);
        free(d2);
        free(r2);
    }
    strvec_free(&names);
}

/* agent.db auth_credentials -> auth.json */
static void import_credentials(const char *src, const char *dst, const migrate_opts_t *o, sb_t *rep) {
    char *db = path_join(src, "agent.db");
    if (!fs_is_file(db)) {
        free(db);
        return;
    }
    json_t *rows = NULL;
    char *err = NULL;
    if (rf_sqlite_table_json(db, "auth_credentials", &rows, &err) != 0) {
        sb_appendf(rep, "  ! credentials: cannot read agent.db: %s\n", err ? err : "?");
        free(err);
        free(db);
        return;
    }
    free(db);
    char *ap = path_join(dst, "auth.json");
    char *txt = fs_read(ap, NULL);
    json_t *store = txt ? json_parse_str(txt) : NULL;
    free(txt);
    if (!json_is_obj(store)) {
        json_free(store);
        store = json_obj();
    }
    /* newest enabled credential per provider wins */
    json_t *best = json_obj(), *r;
    json_arr_foreach(rows, i, r) {
        const char *prov = json_get_str(r, "provider");
        if (!prov || json_get_str(r, "disabled_cause")) continue;
        json_t *cur = json_get(best, prov);
        int64_t ts = json_get_int(r, "updated_at", 0) * 1000000 + json_get_int(r, "id", 0);
        if (!cur || ts > json_get_int(cur, "_rank", 0)) {
            json_t *c = json_clone(r);
            json_set_int(c, "_rank", ts);
            json_set(best, prov, c);
        }
    }
    strvec_t added = {0}, kept = {0};
    const char *prov;
    json_t *c;
    json_obj_foreach(best, k, prov, c) {
        json_t *data = json_parse_str(json_get_str(c, "data") ? json_get_str(c, "data") : "");
        if (!json_is_obj(data)) {
            json_free(data);
            continue;
        }
        if (json_get(store, prov) && !o->force) {
            strvec_push(&kept, prov);
            json_free(data);
            continue;
        }
        const char *type = json_get_str(c, "credential_type");
        json_t *entry = json_obj();
        json_set_str(entry, "type", str_eq(type, "oauth") ? "oauth" : "api_key");
        const char *fk;
        json_t *fv;
        json_obj_foreach(data, j, fk, fv) if (strcmp(fk, "type")) json_set(entry, fk, json_clone(fv));
        json_set(store, prov, entry);
        strvec_push(&added, prov);
        json_free(data);
    }
    if (added.n && !o->dry_run) {
        fs_mkdir_p(dst, 0700);
        char *out = json_dump(store, 2);
        if (fs_write_atomic(ap, out, strlen(out), 0600) != 0) sb_appendf(rep, "  ! cannot write auth.json\n");
        free(out);
    }
    strvec_sort(&added);
    strvec_sort(&kept);
    if (added.n) {
        char *l = strvec_join(&added, ", ");
        sb_appendf(rep, "  credentials: %s %zu (%s)\n", o->dry_run ? "would import" : "imported", added.n, l);
        free(l);
    }
    if (kept.n) {
        char *l = strvec_join(&kept, ", ");
        sb_appendf(rep, "  credentials: kept omc's existing %s (use --force to replace)\n", l);
        free(l);
    }
    strvec_free(&added);
    strvec_free(&kept);
    json_free(best);
    json_free(store);
    json_free(rows);
    free(ap);
}

static int cmp_created(const void *a, const void *b) {
    int64_t x = json_get_int(*(json_t *const *)a, "created_at", 0), y = json_get_int(*(json_t *const *)b, "created_at", 0);
    return x < y ? -1 : x > y;
}

/* history.db history -> prompt-history.jsonl (appended, de-duplicated) */
static void import_history(const char *src, const char *dst, const migrate_opts_t *o, sb_t *rep) {
    char *db = path_join(src, "history.db");
    if (!fs_is_file(db)) {
        free(db);
        return;
    }
    json_t *rows = NULL;
    char *err = NULL;
    int rc = rf_sqlite_table_json(db, "history", &rows, &err);
    free(db);
    if (rc != 0) {
        sb_appendf(rep, "  ! prompt history: cannot read history.db: %s\n", err ? err : "?");
        free(err);
        return;
    }
    char *hp = path_join(dst, "prompt-history.jsonl");
    char *existing = fs_read(hp, NULL);
    strvec_t have = {0};
    if (existing) {
        strvec_t lines = str_split_lines(existing, strlen(existing));
        for (size_t i = 0; i < lines.n; i++) {
            json_t *j = json_parse_str(lines.v[i]);
            if (json_get_str(j, "text")) strvec_push(&have, json_get_str(j, "text"));
            json_free(j);
        }
        strvec_free(&lines);
    }
    size_t n = json_len(rows);
    json_t **v = xcalloc(n ? n : 1, sizeof(json_t *));
    for (size_t i = 0; i < n; i++) v[i] = json_at(rows, i);
    qsort(v, n, sizeof(json_t *), cmp_created);
    sb_t out = SB_INIT;
    size_t added = 0;
    for (size_t i = 0; i < n; i++) {
        const char *p = json_get_str(v[i], "prompt");
        if (!p || !*p || strvec_contains(&have, p)) continue;
        json_t *j = json_obj();
        json_set_str(j, "text", p);
        if (json_get_str(v[i], "cwd")) json_set_str(j, "cwd", json_get_str(v[i], "cwd"));
        json_set_int(j, "ts", json_get_int(v[i], "created_at", 0) * 1000);
        char *line = json_dump(j, 0);
        sb_appendf(&out, "%s\n", line);
        free(line);
        json_free(j);
        strvec_push(&have, p);
        added++;
    }
    if (added && !o->dry_run) {
        fs_mkdir_p(dst, 0700);
        sb_t all = SB_INIT;
        if (existing && *existing) {
            sb_append(&all, existing);
            if (existing[strlen(existing) - 1] != '\n') sb_putc(&all, '\n');
        }
        sb_append(&all, sb_cstr(&out));
        if (fs_write_atomic(hp, all.s, all.len, 0600) != 0) sb_appendf(rep, "  ! cannot write prompt-history.jsonl\n");
        sb_free(&all);
    }
    if (added) sb_appendf(rep, "  prompt history: %s %zu entr%s\n", o->dry_run ? "would import" : "imported", added, added == 1 ? "y" : "ies");
    sb_free(&out);
    free(v);
    free(existing);
    free(hp);
    strvec_free(&have);
    json_free(rows);
}

int omc_migrate(const char *src, const char *dst, const migrate_opts_t *o, sb_t *rep, char **err) {
    *err = NULL;
    if (!src || !fs_is_dir(src)) {
        *err = xasprintf("No omp configuration found at %s (set PI_CODING_AGENT_DIR or pass --from <dir>)", src ? src : "?");
        return -1;
    }
    char *rs = realpath(src, NULL), *rd = realpath(dst, NULL);
    bool same = rs && rd && !strcmp(rs, rd);
    free(rs);
    free(rd);
    if (same) {
        *err = xasprintf("omp and omc share the same agent dir (%s); nothing to migrate", dst);
        return -1;
    }
    sb_appendf(rep, "%s omp configuration\n  from %s\n  to   %s\n", o->dry_run ? "Would migrate" : "Migrating", src, dst);
    DIR *d = opendir(src);
    if (!d) {
        *err = xasprintf("cannot open %s: %s", src, strerror(errno));
        return -1;
    }
    strvec_t names = {0}, skipped_state = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (excluded(e->d_name)) {
            strvec_push(&skipped_state, e->d_name);
            continue;
        }
        if (!strcmp(e->d_name, "sessions") && !o->sessions) {
            strvec_push(&skipped_state, "sessions (--no-sessions)");
            continue;
        }
        strvec_push(&names, e->d_name);
    }
    closedir(d);
    strvec_sort(&names);
    int total_failed = 0;
    for (size_t i = 0; i < names.n; i++) {
        counts_t c = {.o = o};
        char *s2 = path_join(src, names.v[i]), *d2 = path_join(dst, names.v[i]);
        copy_tree(s2, d2, names.v[i], &c, rep, 0);
        free(s2);
        free(d2);
        total_failed += c.failed;
        struct stat st;
        char *sp = path_join(src, names.v[i]);
        bool dir = stat(sp, &st) == 0 && S_ISDIR(st.st_mode);
        free(sp);
        if (!c.copied && !c.skipped && !c.failed) continue;
        sb_appendf(rep, "  %s%s: ", names.v[i], dir ? "/" : "");
        if (dir) {
            sb_appendf(rep, "%d file%s %s", c.copied, c.copied == 1 ? "" : "s", o->dry_run ? "to copy" : "copied");
            if (c.skipped) sb_appendf(rep, ", %d already present", c.skipped);
        } else {
            sb_append(rep, c.copied ? (o->dry_run ? "to copy" : "copied") : c.skipped ? "already present (kept omc's)" : "failed");
        }
        sb_putc(rep, '\n');
    }
    import_credentials(src, dst, o, rep);
    import_history(src, dst, o, rep);
    if (skipped_state.n) {
        strvec_sort(&skipped_state);
        char *l = strvec_join(&skipped_state, ", ");
        sb_appendf(rep, "  left behind (omp-internal state): %s\n", l);
        free(l);
    }
    if (!o->force) sb_append(rep, "  existing omc files were kept; run with --force to overwrite them\n");
    strvec_free(&names);
    strvec_free(&skipped_state);
    return total_failed ? 1 : 0;
}
