/* plat/win/regex.c - POSIX regcomp/regexec on Windows, over omc's own regex engine (text/regex.h). An extended
 * regex is rewritten into the engine's syntax where they differ: inside a bracket expression a leading `]` and a
 * backslash are literal in POSIX, and `[`, `&`, `~` must not start the engine's nested classes or set operations. */
#include <regex.h>

#include "text/regex.h"

static char *translate(const char *p) {
    sb_t b = SB_INIT;
    while (*p) {
        if (*p == '\\' && p[1]) {
            sb_appendn(&b, p, 2);
            p += 2;
            continue;
        }
        if (*p != '[') {
            sb_putc(&b, *p++);
            continue;
        }
        sb_putc(&b, *p++);
        if (*p == '^') sb_putc(&b, *p++);
        if (*p == ']') {
            sb_append(&b, "\\]");
            p++;
        }
        while (*p && *p != ']') {
            if (p[0] == '[' && p[1] == ':') {
                const char *e = strstr(p + 2, ":]");
                if (e) {
                    sb_appendn(&b, p, (size_t)(e + 2 - p));
                    p = e + 2;
                    continue;
                }
            }
            if (*p == '\\' || *p == '[' || *p == '&' || *p == '~') sb_putc(&b, '\\');
            sb_putc(&b, *p++);
        }
        if (*p == ']') sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

int omc_win_regcomp(regex_t *re, const char *pattern, int cflags) {
    char *t = translate(pattern);
    int flags = 0;
    if (cflags & REG_ICASE) flags |= RX_ICASE;
    if (cflags & REG_NEWLINE) flags |= RX_MULTILINE;
    else flags |= RX_DOTALL;
    char *err = NULL;
    rx_t *rx = rx_compile(t, flags, &err);
    free(t);
    free(err);
    if (!rx) return REG_BADPAT;
    re->rx = rx;
    re->re_nsub = (size_t)(rx_ngroups(rx) - 1);
    return 0;
}

int omc_win_regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t *m, int eflags) {
    (void)eflags;
    rx_match_t rm;
    if (!rx_search(re->rx, s, strlen(s), 0, &rm)) return REG_NOMATCH;
    for (size_t i = 0; i < nmatch; i++) {
        if ((int)i < rm.ngroups && rm.groups[i].start >= 0) {
            m[i].rm_so = (regoff_t)rm.groups[i].start;
            m[i].rm_eo = (regoff_t)rm.groups[i].end;
        } else {
            m[i].rm_so = m[i].rm_eo = -1;
        }
    }
    return 0;
}

void omc_win_regfree(regex_t *re) {
    rx_free(re->rx);
    re->rx = NULL;
}
