/* fs.h - filesystem & path helpers */
#ifndef OMC_FS_H
#define OMC_FS_H

#include "core.h"

#include <sys/stat.h>

/* Read whole file. Returns owned buffer (NUL-terminated) or NULL with errno set. */
char *fs_read(const char *path, size_t *len);
char *fs_read_limit(const char *path, size_t max, size_t *len, bool *truncated);
/* Write file atomically (temp + rename), creating parent dirs. Returns 0 or -errno. */
int fs_write_atomic(const char *path, const void *data, size_t len, mode_t mode);
int fs_write(const char *path, const void *data, size_t len);
int fs_append(const char *path, const void *data, size_t len);
int fs_mkdir_p(const char *path, mode_t mode);
int fs_rm_rf(const char *path);
bool fs_exists(const char *path);
bool fs_is_dir(const char *path);
bool fs_is_file(const char *path);
bool fs_is_exec(const char *path);
int64_t fs_mtime_ms(const char *path);
int64_t fs_size(const char *path);
bool fs_is_binary(const char *data, size_t len); /* heuristic: NUL bytes / invalid utf8 ratio */

/* paths */
char *path_join(const char *a, const char *b);
char *path_join3(const char *a, const char *b, const char *c);
char *path_dirname(const char *p);
const char *path_basename(const char *p); /* borrowed into p */
const char *path_ext(const char *p);      /* ".c" or "" */
char *path_normalize(const char *p);
/* absolute: "/x"; on Windows also "C:/x", "C:\x", "//server/share/x" */
bool path_is_absolute(const char *p);
/* a root with nothing below it: "/"; on Windows also "C:/", "C:", "//server/share/" */
bool path_is_root(const char *p);
/* Node's `<CODE>: <description>, <syscall> '<path>'` text for a failed fs call (libuv descriptions); path NULL leaves
 * out the quoted part ("ENOSPC: no space left on device, write"). Owned. */
char *fs_node_error(int err, const char *syscall, const char *path);      /* lexical: removes . and .., collapses // */
char *path_abs(const char *p, const char *cwd); /* expands ~, joins with cwd, normalizes */
char *path_real(const char *p);           /* realpath or NULL */
char *path_relative(const char *from_dir, const char *to); /* both absolute */
char *path_display(const char *p, const char *cwd); /* relative if under cwd, ~ for home */
char *path_expand_home(const char *p);
const char *home_dir(void);
/* getenv("HOME") as POSIX code reads it (NULL when unset); on Windows home_dir(), since HOME is rarely set there */
const char *home_env(void);
char *get_cwd(void);
char *which(const char *prog); /* search PATH */

/* omc dirs: ~/.omc/agent etc. (overridable by OMC_CODING_AGENT_DIR / PI_CODING_AGENT_DIR) */
const char *omc_config_dir(void);  /* ~/.omc */
const char *omc_agent_dir(void);   /* ~/.omc/agent */
char *omc_agent_path(const char *rel);

/* directory listing */
typedef struct {
    char *name;
    bool is_dir;
    bool is_link;
    int64_t size;
    int64_t mtime_ms;
} dirent_info_t;
typedef VEC(dirent_info_t) dirlist_t;
int fs_list_dir(const char *path, dirlist_t *out); /* sorted by name */
void dirlist_free(dirlist_t *l);

/* temp */
char *fs_mktemp(const char *prefix, const char *suffix); /* creates empty file, returns path */
char *fs_mktempdir(const char *prefix);

#endif
