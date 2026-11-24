/* regex.h for Windows: POSIX extended regexes (REG_EXTENDED) over omc's own engine (text/regex.h). Bracket
 * expressions are translated (a leading `]` is literal in POSIX), the rest of ERE is a subset of what rx_compile takes. */
#ifndef OMC_WIN_REGEX_H
#define OMC_WIN_REGEX_H
#include <stddef.h>
typedef long regoff_t;
typedef struct {
    size_t re_nsub;
    void *rx;
} regex_t;
typedef struct {
    regoff_t rm_so, rm_eo;
} regmatch_t;
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NOSUB 4
#define REG_NEWLINE 8
#define REG_NOTBOL 1
#define REG_NOTEOL 2
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define regcomp omc_win_regcomp
#define regexec omc_win_regexec
#define regfree omc_win_regfree
int omc_win_regcomp(regex_t *re, const char *pattern, int cflags);
int omc_win_regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t *m, int eflags);
void omc_win_regfree(regex_t *re);
#endif
