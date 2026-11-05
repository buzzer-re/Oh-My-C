/* jobtext.c - see jobtext.h */
#include "jobtext.h"

#include "core/core.h"
#include "core/fs.h"
#include "outsink.h"

#include <ctype.h>

static bool path_char(unsigned char c) { return isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/' || c >= 0x80; }

char *job_shorten_home(const char *text) {
    const char *home = home_dir();
    size_t hl = home ? strlen(home) : 0;
    while (hl > 1 && home[hl - 1] == '/') hl--;
    if (hl <= 1) return xstrdup(text);
    sb_t b = SB_INIT;
    for (const char *p = text; *p;) {
        bool left = p == text || !path_char((unsigned char)p[-1]);
        if (left && !strncmp(p, home, hl) && (p[hl] == '/' || !p[hl] || !path_char((unsigned char)p[hl]))) {
            sb_putc(&b, '~');
            p += hl;
        } else {
            sb_putc(&b, *p++);
        }
    }
    return sb_detach(&b);
}

/* \s+ -> one space, trimmed; in place */
static void collapse_ws(char *s) {
    char *w = s;
    bool sp = false;
    for (char *r = s; *r; r++) {
        if (isspace((unsigned char)*r)) {
            sp = w != s;
            continue;
        }
        if (sp) *w++ = ' ';
        sp = false;
        *w++ = *r;
    }
    *w = 0;
}

char *job_clean_intent(const char *intent) {
    if (!intent) return NULL;
    size_t n = 0;
    char *san = out_sanitize(intent, strlen(intent), &n);
    collapse_ws(san);
    if (!*san) {
        free(san);
        return NULL;
    }
    char *r = job_shorten_home(san);
    free(san);
    return r;
}

static size_t utf16_len(const char *s) {
    size_t units = 0, n = strlen(s);
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        units += cp >= 0x10000 ? 2 : 1;
        i += (size_t)k;
    }
    return units;
}

char *job_truncate(const char *s, size_t max) {
    if (utf16_len(s) <= max) return xstrdup(s);
    size_t keep = max ? max - 1 : 0, units = 0, n = strlen(s), i = 0;
    while (i < n) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        size_t u = cp >= 0x10000 ? 2 : 1;
        if (units + u > keep) break;
        units += u;
        i += (size_t)k;
    }
    return xasprintf("%.*s\xE2\x80\xA6", (int)i, s);
}

#define GLANCEABLE_COMMAND_CHARS 60

bool job_caption_needed(const char *command) {
    char *t = str_trim(command ? command : "");
    if (utf16_len(t) > GLANCEABLE_COMMAND_CHARS) {
        free(t);
        return true;
    }
    /* trimmed.replace(/'[^']*'|"(?:[^"\\$`]|\\.)*"|\d*>&\d*|&>/g, "") */
    sb_t u = SB_INIT;
    for (const char *p = t; *p;) {
        if (*p == '\'') {
            const char *e = strchr(p + 1, '\'');
            if (e) {
                p = e + 1;
                continue;
            }
        } else if (*p == '"') {
            const char *q = p + 1;
            while (*q && *q != '"' && *q != '$' && *q != '`') {
                if (*q == '\\') {
                    if (!q[1] || q[1] == '\n') break;
                    q += 2;
                } else {
                    q++;
                }
            }
            if (*q == '"') {
                p = q + 1;
                continue;
            }
        } else if (isdigit((unsigned char)*p) || *p == '>') {
            const char *q = p;
            while (isdigit((unsigned char)*q)) q++;
            if (q[0] == '>' && q[1] == '&') {
                q += 2;
                while (isdigit((unsigned char)*q)) q++;
                p = q;
                continue;
            }
        } else if (p[0] == '&' && p[1] == '>') {
            p += 2;
            continue;
        }
        sb_putc(&u, *p++);
    }
    const char *un = sb_cstr(&u);
    bool control = strpbrk(un, "\n;|&`") != NULL || strstr(un, "$(") != NULL;
    sb_free(&u);
    free(t);
    return control;
}

#define MAX_CAPTION_CHARS 200

static bool quote_at(const char *p, size_t *k) {
    if (*p == '"' || *p == '\'' || *p == '`') {
        *k = 1;
        return true;
    }
    if (!strncmp(p, "\xE2\x80\x9C", 3) || !strncmp(p, "\xE2\x80\x9D", 3)) { /* “ ” */
        *k = 3;
        return true;
    }
    return false;
}

char *job_caption_normalize(const char *reply) {
    if (!reply) return NULL;
    /* the first non-empty line */
    const char *p = reply;
    char *line = NULL;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char *raw = xstrndup(p, n), *t = str_trim(raw);
        free(raw);
        if (*t) {
            line = t;
            break;
        }
        free(t);
        p += n + (nl ? 1 : 0);
    }
    if (!line) return NULL;
    size_t sn = 0;
    char *san = out_sanitize(line, strlen(line), &sn);
    free(line);
    const char *c = san;
    /* ^(?:[-*•]\s+|caption:\s*)/i */
    size_t mk = (*c == '-' || *c == '*') ? 1 : !strncmp(c, "\xE2\x80\xA2", 3) ? 3 : 0;
    if (mk && isspace((unsigned char)c[mk])) {
        c += mk;
        while (isspace((unsigned char)*c)) c++;
    } else if (!strncasecmp(c, "caption:", 8)) {
        c += 8;
        while (isspace((unsigned char)*c)) c++;
    }
    size_t k;
    while (quote_at(c, &k)) c += k;
    char *body = xstrdup(c);
    for (;;) {
        size_t bl = strlen(body);
        if (bl && (body[bl - 1] == '"' || body[bl - 1] == '\'' || body[bl - 1] == '`')) body[bl - 1] = 0;
        else if (bl >= 3 && (!strcmp(body + bl - 3, "\xE2\x80\x9C") || !strcmp(body + bl - 3, "\xE2\x80\x9D"))) body[bl - 3] = 0;
        else break;
    }
    collapse_ws(body);
    size_t bl = strlen(body);
    if (bl && body[bl - 1] == '.') body[bl - 1] = 0;
    collapse_ws(body);
    free(san);
    if (!*body) {
        free(body);
        return NULL;
    }
    char *sh = job_shorten_home(body);
    free(body);
    char *r = job_truncate(sh, MAX_CAPTION_CHARS);
    free(sh);
    return r;
}
