/* spawn.h for Windows: posix_spawn/posix_spawnp over plat/win/proc.c (CreateProcessW). File actions are applied to
 * the child's stdin/stdout/stderr (open, dup2, close of fds 0-2); POSIX_SPAWN_SETSID starts the child detached from this
 * console's process group and job. Signal masks and defaults do not exist on Windows and are accepted as no-ops. */
#ifndef OMC_WIN_SPAWN_H
#define OMC_WIN_SPAWN_H
#include <stddef.h>
#define POSIX_SPAWN_RESETIDS 0x01
#define POSIX_SPAWN_SETPGROUP 0x02
#define POSIX_SPAWN_SETSIGDEF 0x04
#define POSIX_SPAWN_SETSIGMASK 0x08
#define POSIX_SPAWN_SETSID 0x80
typedef struct {
    int n;
    struct {
        int op; /* 0 open, 1 dup2, 2 close */
        int fd, src, oflag, mode;
        char *path;
    } a[8];
} posix_spawn_file_actions_t;
typedef struct {
    short flags;
} posix_spawnattr_t;
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, int mode);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawnattr_init(posix_spawnattr_t *at);
int posix_spawnattr_destroy(posix_spawnattr_t *at);
int posix_spawnattr_setflags(posix_spawnattr_t *at, short flags);
static inline int posix_spawnattr_setsigmask(posix_spawnattr_t *at, const void *s) { (void)at; (void)s; return 0; }
static inline int posix_spawnattr_setsigdefault(posix_spawnattr_t *at, const void *s) { (void)at; (void)s; return 0; }
static inline int posix_spawnattr_setpgroup(posix_spawnattr_t *at, int p) { (void)at; (void)p; return 0; }
int posix_spawn(int *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                char *const argv[], char *const envp[]);
int posix_spawnp(int *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                 char *const argv[], char *const envp[]);
#endif
