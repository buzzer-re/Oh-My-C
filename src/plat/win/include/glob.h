/* glob.h for Windows: the subset omc uses (`*`, `?`, `[...]` in any path component, GLOB_NOCHECK, GLOB_TILDE) */
#ifndef OMC_WIN_GLOB_H
#define OMC_WIN_GLOB_H
#include <stddef.h>
typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
} glob_t;
#define GLOB_ERR 0x1
#define GLOB_MARK 0x2
#define GLOB_NOSORT 0x4
#define GLOB_NOCHECK 0x10
#define GLOB_NOESCAPE 0x40
#define GLOB_TILDE 0x800
#define GLOB_BRACE 0x400
#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3
#define glob omc_win_glob
#define globfree omc_win_globfree
int omc_win_glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g);
void omc_win_globfree(glob_t *g);
#endif
