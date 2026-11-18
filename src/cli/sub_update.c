/* sub_update.c - `omc update` (port of omp commands/update.ts + cli/update-cli.ts runUpdateCommand / runBinaryUpdate /
 * updateViaBinaryAt) and the background auto-update worker (cli/auto-update.ts runAutoUpdateWorker), over omc's own
 * GitHub releases instead of omp's build service and npm (src/coding/selfupdate.c). There is one install method, the
 * standalone binary, and one channel. */
#include "cli/cli_int.h"

#include "coding/selfupdate.h"
#include "core/fs.h"
#include "core/tarx.h"
#include "tui/symbols.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define ARCHIVE_CAP ((size_t)256 << 20) /* the downloaded .tar.gz */
#define TAR_CAP ((size_t)512 << 20)     /* the tar it inflates to */
#define BINARY_CAP ((size_t)256 << 20)  /* the omc member */
#define SUMS_CAP ((size_t)1 << 20)
#define CHECK_TIMEOUT_MS 30000              /* RELEASE_METADATA_TIMEOUT_MS */
#define DOWNLOAD_TIMEOUT_MS (15 * 60 * 1000) /* BUILD_DOWNLOAD_TIMEOUT_MS */

typedef enum { UPD_CURRENT, UPD_AVAILABLE, UPD_UNSUPPORTED, UPD_INSTALLED } upd_kind_t;

typedef struct {
    bool force, check;
    /* background runs: download / install progress (phase SU_PH_DOWNLOADING or SU_PH_INSTALLING) */
    void (*progress)(void *ud, su_phase_t phase, const char *version, int64_t received, int64_t total);
    void *ud;
} upd_opts_t;

static void say(char *(*color)(const char *), const char *msg) {
    char *c = color ? color(msg) : xstrdup(msg);
    printf("%s\n", c);
    free(c);
    fflush(stdout);
}

static void sayf(char *(*color)(const char *), const char *fmt, ...) PRINTF_LIKE(2, 3);
static void sayf(char *(*color)(const char *), const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *m = xvasprintf(fmt, ap);
    va_end(ap);
    say(color, m);
    free(m);
}

/* pi-utils formatBytes */
static char *format_bytes(int64_t b) {
    if (b < 1024) return xasprintf("%lldB", (long long)b);
    if (b < 1024 * 1024) return xasprintf("%.1fKB", b / 1024.0);
    if (b < 1024LL * 1024 * 1024) return xasprintf("%.1fMB", b / (1024.0 * 1024));
    return xasprintf("%.1fGB", b / (1024.0 * 1024 * 1024));
}

/* ---------- one download with its announcement and progress ---------- */

typedef struct {
    const upd_opts_t *o;
    const char *name, *version;
    bool announce;
} dl_ctx_t;

static void dl_start(void *ud, int64_t total) {
    dl_ctx_t *c = ud;
    if (!c->announce) return;
    if (total > 0) {
        char *sz = format_bytes(total);
        sayf(cli_dim, "Downloading %s (%s)\xe2\x80\xa6", c->name, sz);
        free(sz);
    } else {
        sayf(cli_dim, "Downloading %s\xe2\x80\xa6", c->name);
    }
}

static void dl_progress(void *ud, const su_progress_t *p) {
    dl_ctx_t *c = ud;
    if (c->announce && c->o->progress) c->o->progress(c->o->ud, SU_PH_DOWNLOADING, c->version, p->received, p->total);
}

/* ---------- install ---------- */

/* write `data` next to `target` (same directory, so the rename is atomic), executable; owned temp path or NULL */
static char *write_temp(const char *target, const char *data, size_t len, char **err) {
    char *dir = path_dirname(target);
    char *tmp = xasprintf("%s/.%s.%ld.%lld.new", dir, path_basename(target), (long)getpid(), (long long)mono_ms());
    free(dir);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    if (fd < 0) {
        *err = xasprintf("could not write %s: %s", tmp, strerror(errno));
        free(tmp);
        return NULL;
    }
    size_t off = 0;
    bool ok = true;
    while (off < len && ok) {
        ssize_t w = write(fd, data + off, MIN(len - off, (size_t)1 << 20));
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = false;
        else off += (size_t)w;
    }
    if (ok && fchmod(fd, 0755) != 0) ok = false;
    if (ok && fsync(fd) != 0) ok = false;
    int e = errno;
    if (close(fd) != 0 && ok) ok = false, e = errno;
    if (!ok) {
        *err = xasprintf("could not write %s: %s", tmp, strerror(e));
        unlink(tmp);
        free(tmp);
        return NULL;
    }
    return tmp;
}

static void fsync_dir(const char *path) {
    char *dir = path_dirname(path);
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    free(dir);
}

/* download, verify and install `version` over `target` (updateViaBinaryAt); NULL or the failure (owned) */
static char *install_release(const upd_opts_t *o, const char *target, const char *version) {
    bool prebuilt;
    const char *plat = su_platform(&prebuilt);
    if (!prebuilt) {
        char *cmd = su_installer_command(true);
        char *e = xasprintf("no prebuilt omc for %s; build it from source with install.sh --build: %s", plat, cmd);
        free(cmd);
        return e;
    }
    char *dir = path_dirname(target);
    if (access(dir, W_OK) != 0) {
        char *cmd = su_installer_command(false);
        char *e = xasprintf("cannot write to %s (%s): run omc update as a user who can, or reinstall omc somewhere you "
                            "own: %s",
                            dir, strerror(errno), cmd);
        free(cmd);
        free(dir);
        return e;
    }
    free(dir);
    char *asset = su_asset_name(version), *base = su_download_base();
    char *url = xasprintf("%s/v%s/%s", base, version, asset), *sums_url = xasprintf("%s/v%s/SHA256SUMS", base, version);
    char *err = NULL, *archive = NULL, *sums = NULL, *tar = NULL, *tmp = NULL, *got = NULL, *want = NULL, *ran = NULL;
    size_t alen = 0, slen = 0, tlen = 0;
    dl_ctx_t c = {o, asset, version, true};
    archive = su_fetch(url, ARCHIVE_CAP, DOWNLOAD_TIMEOUT_MS, dl_start, dl_progress, &c, &alen, &err);
    if (!archive) goto out;
    dl_ctx_t cs = {o, "SHA256SUMS", version, false};
    sums = su_fetch(sums_url, SUMS_CAP, CHECK_TIMEOUT_MS, dl_start, dl_progress, &cs, &slen, &err);
    if (!sums) goto out;
    want = su_sums_lookup(sums, asset);
    if (!want) {
        err = xasprintf("SHA256SUMS has no entry for %s; nothing was installed", asset);
        goto out;
    }
    got = su_sha256_hex(archive, alen);
    if (strcmp(got, want)) {
        err = xasprintf("checksum mismatch for %s (expected %s, got %s); nothing was installed", asset, want, got);
        goto out;
    }
    sayf(cli_dim, "Verified sha256:%s", got);
    char *zerr = NULL;
    tar = tarx_gunzip(archive, alen, TAR_CAP, &tlen, &zerr);
    if (!tar) {
        err = xasprintf("could not read %s: %s", asset, zerr);
        free(zerr);
        goto out;
    }
    free(archive);
    archive = NULL;
    char *member = xasprintf("omc-%s-%s/omc", version, plat);
    const char *bin;
    size_t blen;
    int fr = tarx_find(tar, tlen, member, BINARY_CAP, &bin, &blen, &zerr);
    if (fr != 0) {
        err = fr > 0 ? xasprintf("%s has no %s", asset, member) : xasprintf("could not read %s: %s", asset, zerr);
        free(zerr);
        free(member);
        goto out;
    }
    free(member);
    if (o->progress) o->progress(o->ud, SU_PH_INSTALLING, version, 0, 0);
    say(cli_dim, "Installing update...");
    tmp = write_temp(target, bin, blen, &err);
    if (!tmp) goto out;
    free(tar);
    tar = NULL;
    ran = su_binary_version(tmp);
    if (!ran || strcmp(ran, version)) {
        char *cmd = su_installer_command(true);
        err = ran ? xasprintf("the downloaded omc reports %s, expected %s; nothing was installed", ran, version)
                  : xasprintf("the downloaded omc does not run on this system; nothing was installed (build it from "
                              "source instead: %s)",
                              cmd);
        free(cmd);
        goto out;
    }
    if (rename(tmp, target) != 0) {
        err = xasprintf("could not replace %s: %s", target, strerror(errno));
        goto out;
    }
    free(tmp);
    tmp = NULL;
    fsync_dir(target);
out:
    if (tmp) unlink(tmp);
    free(tmp);
    free(ran);
    free(got);
    free(want);
    free(tar);
    free(sums);
    free(archive);
    free(sums_url);
    free(url);
    free(base);
    free(asset);
    return err;
}

/* runUpdateCommand: 0 with *kind (and *version, owned, for available/installed), or 1 with *failure (owned; already
 * printed for UPD_UNSUPPORTED). *target_out (owned, may be NULL) receives the binary it updated or would update. */
static int run_update(const upd_opts_t *o, upd_kind_t *kind, char **version, char **failure, char **target_out) {
    *kind = UPD_CURRENT;
    *version = NULL;
    *failure = NULL;
    if (target_out) *target_out = NULL;
    bool is_self = false;
    char *err = NULL;
    char *target = su_update_target(&is_self, &err);
    if (!target) {
        *failure = xasprintf("Update failed: %s", err);
        free(err);
        return 1;
    }
    /* a checkout updates through git: replacing its build/omc with a release would be wrong advice */
    if (!o->check && su_is_source_checkout(target)) {
        char *r = xasprintf("%s is built from a source checkout; update it with `git pull && make` in that checkout.",
                            target);
        say(cli_yellow, r);
        *failure = r;
        *kind = UPD_UNSUPPORTED;
        free(target);
        return 1;
    }
    char *current = NULL;
    if (is_self) {
        current = xstrdup(OMC_VERSION);
    } else if (!fs_exists(target)) {
        *failure = xasprintf("Update failed: %s does not exist", target);
        free(target);
        return 1;
    } else if (!(current = su_binary_version(target))) {
        *failure = xasprintf("Update failed: refusing to replace %s: it does not report an omc version when run directly",
                             target);
        free(target);
        return 1;
    }
    sayf(cli_dim, "Current version: %s", current);
    int lock = -2;
    if (!o->check) {
        lock = su_install_lock();
        if (lock == -1) {
            *failure = xstrdup("Another omc update is already running; try again once it finishes.");
            free(current);
            free(target);
            return 1;
        }
    }
    int rc = 0;
    char *latest = su_latest_version(CHECK_TIMEOUT_MS, &err);
    if (!latest) {
        *failure = xasprintf("Failed to check for updates: %s", err);
        free(err);
        rc = 1;
        goto out;
    }
    /* announceOffer */
    int cmp = su_compare_versions(latest, current);
    if (cmp <= 0 && !o->force) {
        sayf(cli_green, "%s Already up to date", tui_sym("status.success"));
        goto out;
    }
    if (cmp > 0) sayf(cli_cyan, "New version available: %s", latest);
    else sayf(cli_yellow, "Forcing reinstall of %s", latest);
    if (o->check) {
        *kind = UPD_AVAILABLE;
        *version = latest;
        latest = NULL;
        goto out;
    }
    char *ierr = install_release(o, target, latest);
    if (ierr) {
        *failure = xasprintf("Update failed: %s", ierr);
        free(ierr);
        rc = 1;
        goto out;
    }
    sayf(cli_green, "\n%s Updated to %s at %s", tui_sym("status.success"), latest, target);
    say(cli_dim, "Restart omc to use the new version");
    *kind = UPD_INSTALLED;
    *version = latest;
    latest = NULL;
out:
    su_install_unlock(lock);
    free(latest);
    free(current);
    if (target_out) *target_out = target;
    else free(target);
    return rc;
}

int cmd_update(cparse_t *p) {
    upd_opts_t o = {.force = cp_bool(p, "force"), .check = cp_bool(p, "check")};
    upd_kind_t kind;
    char *version = NULL, *failure = NULL;
    int rc = run_update(&o, &kind, &version, &failure, NULL);
    if (rc && failure && kind != UPD_UNSUPPORTED) cli_err_line(failure);
    free(version);
    free(failure);
    return rc;
}

/* ---------- the background auto-update worker (`omc __omc_worker_auto_update`) ---------- */

typedef struct {
    const char *path, *target;
    su_phase_t phase;
    int64_t written_at;
    char *version; /* the version the install reported last, kept for a failure that names none */
} writer_t;

static void put(writer_t *w, const su_state_t *s) {
    su_state_t st = *s;
    st.target = (char *)w->target;
    st.at = now_ms();
    w->phase = st.phase;
    w->written_at = mono_ms();
    su_state_write(w->path, &st);
}

/* StatusWriter.progress: download progress throttled to one write per 200 ms per phase */
static void worker_progress(void *ud, su_phase_t phase, const char *version, int64_t received, int64_t total) {
    writer_t *w = ud;
    if (version && !str_eq(w->version, version)) {
        free(w->version);
        w->version = xstrdup(version);
    }
    bool complete = phase == SU_PH_DOWNLOADING && total && received >= total;
    if (phase == w->phase && !complete && mono_ms() - w->written_at < 200) return;
    su_state_t s = {.phase = phase, .version = (char *)version, .received = received, .total = total};
    put(w, &s);
}

int cmd_auto_update_worker_raw(int argc, char **argv) {
    char *path = su_auto_state_path();
    int lock = su_lock_try(path);
    if (lock < 0) { /* another worker runs */
        free(path);
        return 0;
    }
    /* launches open the log for appending; the run that owns the lock starts it afresh */
    char *log = su_auto_log_path();
    if (truncate(log, 0) != 0) {}
    free(log);
    alarm((unsigned)(SU_WORKER_DEADLINE_MS / 1000)); /* the deadline: no final status, so the launch shows a failure */
    writer_t w = {.path = path, .target = env_get("OMC_AUTO_UPDATE_TARGET"), .phase = SU_PH_FAILED};
    su_state_t s = {.phase = SU_PH_CHECKING};
    put(&w, &s);
    upd_opts_t o = {.progress = worker_progress, .ud = &w};
    upd_kind_t kind;
    char *version = NULL, *failure = NULL, *target = NULL;
    run_update(&o, &kind, &version, &failure, &target); /* the target is resolved before the install replaces it */
    su_state_t fin = {0};
    char *actual = NULL, *why = NULL;
    if (failure) { /* settle: a thrown update is a failed run */
        fin.phase = SU_PH_FAILED;
        fin.version = w.version;
        fin.error = failure;
    } else if (kind == UPD_INSTALLED) {
        /* an install counts once the target reports a version newer than this build */
        actual = target ? su_binary_version(target) : NULL;
        if (actual && su_compare_versions(actual, OMC_VERSION) > 0) {
            fin.phase = SU_PH_DONE;
            fin.version = actual;
        } else {
            fin.phase = SU_PH_FAILED;
            fin.version = version;
            why = xasprintf("%s still reports %s", target ? target : "omc", actual ? actual : "no version");
            fin.error = why;
        }
    } else {
        fin.phase = SU_PH_CURRENT;
    }
    put(&w, &fin);
    /* stdout is the worker log (auto-update.log), after the update's own output */
    printf("auto-update: %s%s%s%s%s\n", su_phase_name(fin.phase), fin.version ? " " : "", fin.version ? fin.version : "",
           fin.error ? " " : "", fin.error ? fin.error : "");
    fflush(stdout);
    int rc = fin.phase == SU_PH_FAILED ? 1 : 0;
    su_lock_release(lock);
    free(actual);
    free(why);
    free(version);
    free(failure);
    free(w.version);
    free(target);
    free(path);
    return rc;
}
