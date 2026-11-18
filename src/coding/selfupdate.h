/* selfupdate.h - the mechanics behind `omc update`, the startup version check and `update.auto`: omc's own GitHub
 * releases (the assets .github/workflows/release.yml publishes and install.sh installs), in place of omp's build
 * service and npm.
 *
 * Environment (install.sh uses the first two the same way):
 *   OMC_REPO              GitHub repository (default buzzer-re/Oh-My-C)
 *   OMC_DOWNLOAD_URL      base URL holding v<version>/<asset> (default https://github.com/$OMC_REPO/releases/download)
 *   OMC_LATEST_URL        URL whose redirect names the latest release's tag (default
 *                         https://github.com/$OMC_REPO/releases/latest); plain http:// is accepted for local mocks
 *   OMC_UPDATE_TARGET     testing hook: the omc binary `omc update` replaces instead of the running one
 *   OMC_NO_UPDATE_CHECK=1 no startup version check and no background auto-update (the test suites set it)
 */
#ifndef OMC_SELFUPDATE_H
#define OMC_SELFUPDATE_H

#include "core/core.h"

/* omp compareVersions (pi-utils version.ts): numeric dot-separated core parts (missing or non-numeric parts count as
 * 0, any number of digits), then SemVer prerelease order (a plain release sorts above its prereleases); a leading
 * `v` and a `+build` suffix are ignored. <0, 0, >0. */
int su_compare_versions(const char *a, const char *b);

/* "<version>" from `omc --version` output ("omc/18.22.0", a prerelease suffix allowed); owned, NULL otherwise */
char *su_parse_reported_version(const char *output);

const char *su_repo(void);           /* OMC_REPO or buzzer-re/Oh-My-C */
char *su_latest_url(void);           /* owned */
char *su_download_base(void);        /* owned, no trailing '/' */
char *su_installer_command(bool build); /* `curl -fsSL …/install.sh | sh` (`| sh -s -- --build`); owned */
/* "<os>-<arch>" of this build (linux-x86_64, linux-arm64, macos-arm64, ...) and whether a release ships it */
const char *su_platform(bool *prebuilt);
char *su_asset_name(const char *version); /* omc-<v>-<os>-<arch>.tar.gz, owned */

/* The latest published release's version, from the `releases/latest` redirect (no API, no token). Owned; NULL with
 * *err set (owned) on a network failure or when the repository has no release. */
char *su_latest_version(int64_t timeout_ms, char **err);

/* download progress / phase reports */
typedef struct {
    int64_t received, total; /* total 0 = unknown */
} su_progress_t;
typedef void (*su_progress_cb)(void *ud, const su_progress_t *p);
/* the transfer's size became known (after any redirects); total 0 = unknown */
typedef void (*su_start_cb)(void *ud, int64_t total);

/* GET `url` (redirects followed, never downgraded from https to http) into an owned buffer of at most `cap` bytes.
 * NULL with *err: "download failed: <url> (HTTP 404)" and the like. */
char *su_fetch(const char *url, size_t cap, int64_t timeout_ms, su_start_cb on_start, su_progress_cb on_progress,
               void *ud, size_t *len, char **err);

char *su_sha256_hex(const void *data, size_t len); /* lowercase hex, owned */
/* the hex digest SHA256SUMS lists for `asset` (`<hex>  <name>` or `<hex> *<name>`); owned, NULL when absent */
char *su_sums_lookup(const char *sums, const char *asset);

/* the running executable's real path (/proc/self/exe; _NSGetExecutablePath + realpath on macOS); owned or NULL */
char *su_self_exe(void);
/* the binary an update replaces: OMC_UPDATE_TARGET (resolved) or the running executable; *is_self says which */
char *su_update_target(bool *is_self, char **err);
/* `exe` lives in the build/ tree of an omc git work tree (a Makefile, src/core/core.h and .git beside build/):
 * such a binary updates through `git pull && make`, never by replacing it */
bool su_is_source_checkout(const char *exe);
/* the update state dir (<omc config dir>/update: install lock, auto-update status and log); owned */
char *su_state_dir(void);

/* `<path> --version` (timeout 15 s, retried while the file is still busy being written): the reported version or
 * NULL; owned */
char *su_binary_version(const char *path);

/* ---------- locks ---------- */

/* the install lock (<state dir>/install.lock, flock): one installer at a time, `omc update` or the auto-update worker.
 * The fd, -1 when another holds it, -2 when the lock file cannot be created (the caller goes on without it). */
int su_install_lock(void);
void su_install_unlock(int fd);
/* a POSIX record lock on <path>.lock held for a whole run (the auto-update worker's status lock). su_lock_held probes
 * it without taking it, so a worker starting at that moment never meets the probe. */
int su_lock_try(const char *path); /* fd or -1 */
bool su_lock_held(const char *path);
void su_lock_release(int fd);

/* ---------- auto-update status (cli/auto-update.ts AutoUpdateState) ---------- */

typedef enum { SU_PH_CHECKING, SU_PH_DOWNLOADING, SU_PH_INSTALLING, SU_PH_DONE, SU_PH_CURRENT, SU_PH_FAILED } su_phase_t;
typedef struct {
    su_phase_t phase;
    char *target;  /* version the spawning launch's check offered (keys the retry hold-off) */
    char *version; /* version being installed, or (done) the version the target now reports */
    char *error;
    int64_t received, total; /* downloading; total 0 = unknown */
    int64_t at;              /* wall-clock ms of the write */
} su_state_t;

/* a finished attempt at a version holds off the next attempt at it this long (RETRY_AFTER_MS) */
#define SU_RETRY_AFTER_MS (6LL * 60 * 60 * 1000)
/* hard stop for a worker (WORKER_DEADLINE_MS) */
#define SU_WORKER_DEADLINE_MS (30LL * 60 * 1000)

const char *su_phase_name(su_phase_t p);
bool su_phase_final(su_phase_t p); /* done, current, failed end a run */
char *su_auto_state_path(void);    /* <state dir>/auto-update.json, owned */
char *su_auto_log_path(void);      /* <state dir>/auto-update.log, owned */
bool su_state_read(const char *path, su_state_t *out); /* false when absent or malformed (e.g. read mid-write) */
int su_state_write(const char *path, const su_state_t *s);
void su_state_free(su_state_t *s);
/* shouldStartWorker: always for a new version, else once the previous run is SU_RETRY_AFTER_MS old */
bool su_should_start_worker(const su_state_t *last, const char *target, int64_t now);

#endif
