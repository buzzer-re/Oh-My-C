/* power.c - sleep prevention while a turn runs (see power.h). */
#define _GNU_SOURCE 1
#include "coding/power.h"

#include "config/settings.h"
#include "core/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define REASON "omc agent session" /* omp: "omp agent session" */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_refs;
#ifndef _WIN32
static bool g_unavailable;
static pid_t g_pid = -1; /* caffeinate */
static int g_pipe = -1;  /* write end of systemd-inhibit's `cat` stdin */
#else
static bool g_unavailable; /* never set: SetThreadExecutionState is always there */
#endif

strvec_t power_command(const char *mode) {
    strvec_t v = {0};
    if (!mode || str_eq(mode, "off")) return v;
    bool display = str_eq(mode, "display") || str_eq(mode, "system");
    bool system = str_eq(mode, "system");
#ifdef __APPLE__
    strvec_push(&v, "caffeinate");
    strvec_push(&v, "-i");
    if (system) strvec_push(&v, "-s");
    if (system) strvec_push(&v, "-u");
    if (display) strvec_push(&v, "-d");
    strvec_push(&v, "-w");
    strvec_push_own(&v, xasprintf("%d", (int)getpid()));
#else
    (void)display;
    strvec_push(&v, "systemd-inhibit");
    strvec_push(&v, system ? "--what=idle:sleep" : "--what=idle");
    strvec_push(&v, "--who=omc");
    strvec_push(&v, "--why=" REASON);
    strvec_push(&v, "--mode=block");
    strvec_push(&v, "--no-ask-password");
    strvec_push(&v, "cat");
#endif
    return v;
}

#if !defined(__APPLE__) && !defined(_WIN32)
/* start_login1's "no D-Bus at all" case: the system bus socket does not exist */
static bool system_bus_present(void) {
    const char *addr = env_get("DBUS_SYSTEM_BUS_ADDRESS");
    if (addr && str_starts(addr, "unix:path=")) {
        char *p = xstrndup(addr + 10, strcspn(addr + 10, ","));
        bool ok = access(p, F_OK) == 0;
        free(p);
        return ok;
    }
    return access("/run/dbus/system_bus_socket", F_OK) == 0 || access("/var/run/dbus/system_bus_socket", F_OK) == 0;
}
#endif

#ifdef _WIN32
/* natives power.rs (Windows): SetThreadExecutionState on a dedicated thread, which holds it until released (the state
 * belongs to the thread that set it). Windows has no separate system-sleep assertion: ES_SYSTEM_REQUIRED stands for
 * both idle and system; display adds ES_DISPLAY_REQUIRED. */
#include <windows.h>

static HANDLE g_power_thread, g_power_release;

static DWORD WINAPI power_thread(void *arg) {
    EXECUTION_STATE flags = (EXECUTION_STATE)(uintptr_t)arg;
    if (!SetThreadExecutionState(flags)) return 1;
    WaitForSingleObject(g_power_release, INFINITE);
    SetThreadExecutionState(ES_CONTINUOUS);
    return 0;
}

static void start_locked(void) {
    const char *mode = settings_str("power.sleepPrevention", "idle");
    if (!mode || str_eq(mode, "off")) return;
    EXECUTION_STATE flags = ES_CONTINUOUS | ES_SYSTEM_REQUIRED;
    if (str_eq(mode, "display") || str_eq(mode, "system")) flags |= ES_DISPLAY_REQUIRED;
    g_power_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_power_thread = g_power_release ? CreateThread(NULL, 64 * 1024, power_thread, (void *)(uintptr_t)flags, 0, NULL) : NULL;
    if (!g_power_thread && g_power_release) {
        CloseHandle(g_power_release);
        g_power_release = NULL;
    }
}

static void stop_locked(void) {
    if (!g_power_thread) return;
    SetEvent(g_power_release);
    WaitForSingleObject(g_power_thread, 5000);
    CloseHandle(g_power_thread);
    CloseHandle(g_power_release);
    g_power_thread = g_power_release = NULL;
}
#else
static void dev_null_stdio(int in_fd) {
    int fd = open("/dev/null", O_RDWR);
    if (fd < 0) return;
    dup2(in_fd >= 0 ? in_fd : fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
}

static void start_locked(void) {
    char *mode = xstrdup(settings_str("power.sleepPrevention", "idle"));
    strvec_t cmd = power_command(mode);
    free(mode);
    if (!cmd.n) return;
    char *bin = which(cmd.v[0]);
#if !defined(__APPLE__) && !defined(_WIN32)
    if (!bin || !system_bus_present()) {
        g_unavailable = true; /* the backend does not appear mid-process: stop retrying */
        free(bin);
        strvec_free(&cmd);
        return;
    }
#else
    if (!bin) {
        g_unavailable = true;
        strvec_free(&cmd);
        return;
    }
#endif
    char **av = xcalloc(cmd.n + 1, sizeof *av);
    for (size_t i = 0; i < cmd.n; i++) av[i] = cmd.v[i];
#ifdef __APPLE__
    pid_t pid = fork();
    if (pid == 0) {
        dev_null_stdio(-1);
        execv(bin, av);
        _exit(127);
    }
    if (pid > 0) g_pid = pid;
#else
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == 0) { /* close-on-exec: no other child may keep the inhibitor's stdin open */
        pid_t pid = fork();
        if (pid == 0) { /* detach: the grandchild is reparented, never our zombie */
            setsid();
            signal(SIGPIPE, SIG_DFL);
            if (fork() != 0) _exit(0);
            dev_null_stdio(fds[0]);
            close(fds[0]);
            execv(bin, av);
            _exit(127);
        }
        close(fds[0]);
        if (pid > 0) {
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            g_pipe = fds[1];
        } else {
            close(fds[1]);
        }
    }
#endif
    free(av);
    free(bin);
    strvec_free(&cmd);
}

static void stop_locked(void) {
    if (g_pipe >= 0) close(g_pipe); /* `cat` sees EOF; systemd-inhibit exits and logind drops the inhibitor */
    g_pipe = -1;
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        int st;
        while (waitpid(g_pid, &st, 0) < 0 && errno == EINTR) {}
    }
    g_pid = -1;
}
#endif

void power_acquire(void) {
    pthread_mutex_lock(&g_mu);
    if (g_refs++ == 0 && !g_unavailable) start_locked();
    pthread_mutex_unlock(&g_mu);
}

void power_release(void) {
    pthread_mutex_lock(&g_mu);
    if (g_refs > 0 && --g_refs == 0) stop_locked();
    pthread_mutex_unlock(&g_mu);
}
