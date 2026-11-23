/* approval.c - tool approval resolution, per-tool approval declarations, remembered grants and the approval
 * prompt (see approval.h for the upstream files ported here). */
#include "approval.h"

#include "config/settings.h"
#include "core/fs.h"
#include "text/regex.h"
#include "text/rxbt.h"
#include "tools/hashline.h"
#include "tools/patch.h"
#include "tools/toolcmd.h"
#include "tui/ansi.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SEP "\x1f"
#define TREE_PREFIX "tree" SEP
#define PROMPT_TRUNCATE_CHARS 2000
#define FOLDER_FULL_LISTING 24
#define FOLDER_COMPACT_LISTING 4
#define FOLDER_SCAN_LIMIT 200

static const char *const TIER_NAMES[] = {"read", "write", "exec"};
static const char *const MODE_NAMES[] = {"always-ask", "always-ask-cached", "write", "yolo"};

const char *approval_tier_name(approval_tier_t t) { return TIER_NAMES[t]; }
const char *approval_mode_name(approval_mode_t m) { return MODE_NAMES[m]; }

int approval_mode_parse(const char *s) {
    for (int i = 0; s && i < 4; i++)
        if (!strcmp(s, MODE_NAMES[i])) return i;
    return -1;
}

/* normalizePolicy: trimmed, case-folded allow | deny | prompt */
static approval_policy_t policy_parse(const json_t *v) {
    if (!json_is_str(v)) return APOL_UNSET;
    char *t = str_trim(json_sv(v));
    approval_policy_t p = APOL_UNSET;
    if (!strcasecmp(t, "allow")) p = APOL_ALLOW;
    else if (!strcasecmp(t, "deny")) p = APOL_DENY;
    else if (!strcasecmp(t, "prompt")) p = APOL_PROMPT;
    free(t);
    return p;
}

void approval_decision_free(approval_decision_t *d) {
    free(d->reason);
    free(d->policy_key);
    memset(d, 0, sizeof *d);
}

void approval_resolved_free(approval_resolved_t *r) {
    free(r->reason);
    free(r->policy_key);
    memset(r, 0, sizeof *r);
}

static void dec_tier(approval_decision_t *d, approval_tier_t t) {
    memset(d, 0, sizeof *d);
    d->tier = t;
}

/* ---------- text helpers ---------- */

/* sanitizeText: strip ANSI sequences, then C0 (but \t \n), DEL and C1 controls */
static char *sanitize(const char *s) {
    char *plain = strchr(s, '\x1b') ? tui_strip_ansi(s) : xstrdup(s);
    sb_t b = SB_INIT;
    size_t n = strlen(plain);
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_decode(plain + i, n - i, &cp);
        bool ctl = (cp <= 0x08) || (cp >= 0x0B && cp <= 0x1F) || (cp >= 0x7F && cp <= 0x9F) || cp == 0xFFFD;
        if (!ctl) sb_appendn(&b, plain + i, (size_t)k);
        i += (size_t)k;
    }
    free(plain);
    return sb_detach(&b);
}

/* truncateForPrompt: UTF-16 units approximated by code points */
char *approval_truncate(const char *s) {
    size_t total = utf8_strlen(s);
    if (total <= PROMPT_TRUNCATE_CHARS) return xstrdup(s);
    size_t i = 0, n = 0, len = strlen(s);
    while (i < len && n < PROMPT_TRUNCATE_CHARS) {
        uint32_t cp;
        i += (size_t)utf8_decode(s + i, len - i, &cp);
        n++;
    }
    return xasprintf("%.*s[…%zuch elided…]", (int)i, s, total - PROMPT_TRUNCATE_CHARS);
}

/* shortenPath: a home-dir prefix becomes ~ */
static char *shorten_path(const char *p) {
    const char *home = home_dir();
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(p, home, hl) && (p[hl] == '/' || p[hl] == 0)) return xasprintf("~%s", p + hl);
    return xstrdup(p);
}

static bool is_readable_url(const char *s) {
    return !strncasecmp(s, "http://", 7) || !strncasecmp(s, "https://", 8) || !strncasecmp(s, "http:/", 6) ||
           !strncasecmp(s, "https:/", 7) || !strncasecmp(s, "www.", 4);
}

/* ---------- internal URL schemes (internal-urls/router.ts) ---------- */

typedef enum { WS_NONE, WS_READ, WS_WRITE, WS_EXEC } write_spec_t;
static const struct {
    const char *scheme;
    write_spec_t write; /* WS_NONE: read-only scheme */
    bool read_exec;     /* spec.readTier exec */
} SCHEMES[] = {
    {"skill", WS_NONE, false},   {"rule", WS_NONE, false},     {"memory", WS_NONE, false},
    {"out", WS_READ, false},     {"agent", WS_READ, false},    {"history", WS_NONE, false},
    {"artifact", WS_NONE, false}, {"local", WS_READ, false},   {"proc", WS_EXEC, false},
    {"cfg", WS_WRITE, false},    {"ssh", WS_EXEC, true},       {"security", WS_NONE, false},
    {"vault", WS_WRITE, false},  {"issue", WS_NONE, false},    {"pr", WS_NONE, false},
    {"mr", WS_NONE, false},      {"mcp", WS_NONE, false},      {"omp", WS_NONE, false},
    {"help", WS_NONE, false},    {"attachment", WS_NONE, false}, {"conflict", WS_WRITE, false},
};

static int scheme_index(const char *name, size_t n) {
    for (size_t i = 0; i < ARRAY_LEN(SCHEMES); i++)
        if (strlen(SCHEMES[i].scheme) == n && !strncasecmp(SCHEMES[i].scheme, name, n)) return (int)i;
    return -1;
}

/* router.normalize: `local:/x` (and a path glued in front of `local://`) -> `local://x`. Owned. */
static char *url_normalize(const char *in) {
    /* URL_PREFIX_RE: not a URL already -> a `/<scheme>://` segment of a single-slash-alias scheme wins */
    bool url_prefixed = false;
    if (isalpha((unsigned char)in[0])) {
        const char *p = in + 1;
        while (isalnum((unsigned char)*p) || *p == '+' || *p == '.' || *p == '-') p++;
        url_prefixed = *p == ':' && !(p == in + 1 && (p[1] == '/' || p[1] == '\\'));
    }
    if (!url_prefixed) {
        for (const char *s = in; *s; s++) {
            if (*s != '/' && *s != '\\') continue;
            const char *p = s + 1;
            if (!isalpha((unsigned char)*p)) continue;
            const char *q = p;
            while (isalnum((unsigned char)*q) || *q == '+' || *q == '.' || *q == '-') q++;
            if (strncmp(q, "://", 3) != 0 || q[3] == '/') continue;
            if ((size_t)(q - p) == 5 && !strncasecmp(p, "local", 5)) return xstrdup(p);
        }
    }
    if (!strncasecmp(in, "local:/", 7) && in[7] != '/') return xasprintf("local://%s", in + 7);
    return xstrdup(in);
}

/* registered hierarchical `scheme://` URL: the scheme index, else -1 */
static int url_registered(const char *normalized) {
    const char *p = normalized;
    if (!isalpha((unsigned char)*p)) return -1;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '.' || *p == '-') p++;
    if (strncmp(p, "://", 3) != 0) return -1;
    return scheme_index(normalized, (size_t)(p - normalized));
}

static bool url_can_handle(const char *s) {
    char *n = url_normalize(s);
    bool r = url_registered(n) >= 0;
    free(n);
    return r;
}

/* router.readTier: max spec.readTier over every registered `scheme://` anywhere in the text */
static approval_tier_t read_tier(const char *text) {
    for (size_t i = 0; i < ARRAY_LEN(SCHEMES); i++) {
        if (!SCHEMES[i].read_exec) continue;
        char *pat = xasprintf("%s://", SCHEMES[i].scheme);
        bool hit = str_istr(text, pat) != NULL;
        free(pat);
        if (hit) return TIER_EXEC;
    }
    return TIER_READ;
}

static void tool_decision_depth(const approval_call_t *c, approval_decision_t *out, int depth);

/* router.writeTier */
static void write_tier(const approval_call_t *c, const char *target, const json_t *content, approval_decision_t *out,
                       int depth) {
    char *n = url_normalize(target);
    int si = url_registered(n);
    if (si < 0) dec_tier(out, TIER_WRITE);
    else {
        switch (SCHEMES[si].write) {
        case WS_NONE:
            dec_tier(out, TIER_WRITE);
            out->policy = APOL_DENY;
            out->reason = xasprintf("%s:// URLs are read-only", SCHEMES[si].scheme);
            break;
        case WS_READ: dec_tier(out, TIER_READ); break;
        case WS_WRITE: dec_tier(out, TIER_WRITE); break;
        case WS_EXEC: dec_tier(out, TIER_EXEC); break;
        }
    }
    free(n);
}

/* strictestApproval: the first deny wins; otherwise the highest tier, starting from read */
static void strictest(approval_decision_t *ds, size_t n, approval_decision_t *out) {
    for (size_t i = 0; i < n; i++) {
        if (ds[i].policy == APOL_DENY) {
            *out = ds[i];
            memset(&ds[i], 0, sizeof ds[i]);
            return;
        }
    }
    approval_tier_t t = TIER_READ;
    for (size_t i = 0; i < n; i++)
        if (ds[i].tier > t) t = ds[i].tier;
    dec_tier(out, t);
}

/* plan-mode-guard.ts unwrapHashlineHeaderPath: `[PATH]` / `[PATH#XXXX]` -> PATH (owned) */
static char *unwrap_header_path(const char *p) {
    size_t n = strlen(p);
    while (n && isspace((unsigned char)p[n - 1])) n--;
    if (n < 2 || p[0] != '[' || p[n - 1] != ']') return xstrdup(p);
    const char *inner = p + 1;
    size_t in = n - 2;
    size_t plen = in;
    bool tagged = in >= 5 && inner[in - 5] == '#' && isxdigit((unsigned char)inner[in - 4]) &&
                  isxdigit((unsigned char)inner[in - 3]) && isxdigit((unsigned char)inner[in - 2]) &&
                  isxdigit((unsigned char)inner[in - 1]);
    if (tagged) plen = in - 5;
    if (plen == 0 || (!tagged && memchr(inner, '#', plen))) return xstrdup(p);
    return xstrndup(inner, plen);
}

/* ---------- filesystem keys (approval-cache.ts / approval-targets.ts) ---------- */

/* resolveEquivalentPath(throughMissing): realpath of the deepest existing ancestor plus the rest */
static char *real_through_missing(const char *abs) {
    char *r = path_real(abs);
    if (r) return r;
    char *norm = path_normalize(abs);
    char *dir = path_dirname(norm);
    char *out;
    if (!strcmp(dir, norm) || !*dir) out = xstrdup(norm);
    else {
        char *rd = real_through_missing(dir);
        out = path_join(rd, path_basename(norm));
        free(rd);
    }
    free(dir);
    free(norm);
    return out;
}

/* approvalCacheKeyForPath */
static char *cache_key_for_path(const char *abs) {
    char *k = real_through_missing(abs);
    size_t n = strlen(k);
    if (n > 1 && k[n - 1] == '/') k[n - 1] = 0;
    return k;
}

static char *dir_key(const char *dir) {
    char *k = cache_key_for_path(dir);
    if (str_ends(k, "/")) return k;
    char *r = xasprintf("%s/", k);
    free(k);
    return r;
}

static char *resolve_to_cwd(const approval_call_t *c, const char *p) {
    return c->env ? tool_resolve_path(c->env, p) : path_abs(p, ".");
}

/* approvalTargetKey: internal URL (normalized), readable URL (as written), else the resolved path key */
static char *target_key(const approval_call_t *c, const char *target) {
    char *trimmed = str_trim(target);
    if (!*trimmed) {
        free(trimmed);
        return NULL;
    }
    char *n = url_normalize(trimmed);
    if (url_registered(n) >= 0) {
        free(trimmed);
        return n;
    }
    free(n);
    if (is_readable_url(trimmed)) return trimmed;
    free(trimmed);
    char *abs = resolve_to_cwd(c, target);
    char *k = cache_key_for_path(abs);
    free(abs);
    return k;
}

static const char *tree_folder_key(const char *key) { return str_starts(key, TREE_PREFIX) ? key + 5 : NULL; }

/* approvalTargetNamesFile */
static bool target_names_file(const char *target) {
    if (str_starts(target, TREE_PREFIX)) return false;
    if (url_can_handle(target) || is_readable_url(target)) return false;
    struct stat st;
    if (stat(target, &st) != 0) return true;
    return !S_ISDIR(st.st_mode);
}

/* isApprovalPublicUrl */
static bool public_url(const char *url) {
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t n = strcspn(h, "/?#");
    char *host = xstrndup(h, n);
    char *at = strrchr(host, '@');
    if (at) memmove(host, at + 1, strlen(at + 1) + 1);
    for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
    bool v6 = host[0] == '[';
    if (v6) {
        char *e = strchr(host, ']');
        if (e) *e = 0;
        memmove(host, host + 1, strlen(host));
    } else {
        char *colon = strrchr(host, ':');
        if (colon) *colon = 0;
    }
    /* an absolute name (`intranet.`, `localhost.`) resolves to the same host as its relative form (omp 6065e08235) */
    for (size_t hl = strlen(host); hl && host[hl - 1] == '.'; hl--) host[hl - 1] = 0;
    bool ok;
    unsigned a, b, c2, d;
    char tail;
    if (!*host) ok = false;
    else if (v6) {
        ok = !(str_eq(host, "::1") || str_starts(host, "fe80:") || str_starts(host, "fc") || str_starts(host, "fd") ||
               str_starts(host, "::ffff:"));
    } else if (!strchr(host, '.') || str_ends(host, ".local") || str_ends(host, ".localhost") ||
               str_ends(host, ".internal")) {
        ok = false;
    } else if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c2, &d, &tail) == 4) {
        ok = !(a == 0 || a == 10 || a == 127 || a >= 224 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31) ||
               (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127));
    } else {
        ok = true;
    }
    free(host);
    return ok;
}

/* ---------- edit payload inspection (pi-edit ModeEngine::inspect) ---------- */

typedef struct {
    strvec_t paths;
    strvec_t move_from, move_to;
} edit_inspection_t;

static void edit_inspection_free(edit_inspection_t *x) {
    strvec_free(&x->paths);
    strvec_free(&x->move_from);
    strvec_free(&x->move_to);
}

static char *trim_dup_n(const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

/* hashline: `[PATH]` / `[PATH#TAG]` header rows (path noise stripped); REM/MV from the parsed sections */
static void inspect_hashline(const char *input, edit_inspection_t *x) {
    strvec_t lines = str_split_lines(input, strlen(input));
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        while (isspace((unsigned char)*l)) l++;
        if (*l != '[') continue;
        const char *close = strchr(l, ']');
        if (!close || close == l + 1) continue;
        const char *after = close + 1;
        while (isspace((unsigned char)*after)) after++;
        if (*after) continue;
        size_t in = (size_t)(close - l - 1);
        const char *inner = l + 1;
        if (in >= 5 && inner[in - 5] == '#' && isxdigit((unsigned char)inner[in - 4]) && isxdigit((unsigned char)inner[in - 3]) &&
            isxdigit((unsigned char)inner[in - 2]) && isxdigit((unsigned char)inner[in - 1]))
            in -= 5;
        char *path = trim_dup_n(inner, in);
        /* PATH_NOISE: `*** Add/Update/Delete File:` / `*** Move to:` */
        const char *p = path;
        if (str_starts(p, "***")) {
            const char *q = p + 3;
            while (isspace((unsigned char)*q)) q++;
            const char *kw[] = {"Add File", "Update File", "Delete File", "Move to"};
            for (size_t k = 0; k < ARRAY_LEN(kw); k++) {
                size_t kl = strlen(kw[k]);
                if (!strncasecmp(q, kw[k], kl)) {
                    const char *r = q + kl;
                    while (isspace((unsigned char)*r)) r++;
                    if (*r == ':') {
                        char *t = str_trim(r + 1);
                        free(path);
                        path = t;
                    }
                    break;
                }
            }
        }
        if (*path) strvec_push_own(&x->paths, path);
        else free(path);
    }
    strvec_free(&lines);
    char *err = NULL;
    hl_patch_t *hp = hl_patch_parse(input, NULL, &err);
    free(err);
    if (!hp) return;
    for (size_t i = 0; i < hl_patch_count(hp); i++) {
        char *e2 = NULL;
        bool range_err = false;
        if (!hl_section_parse(hp, i, &e2, &range_err)) {
            free(e2);
            continue;
        }
        const char *dest = NULL;
        int op = hl_section_file_op(hp, i, &dest);
        if (op == 2 && dest) {
            strvec_push(&x->move_from, hl_section_path(hp, i));
            strvec_push(&x->move_to, dest);
        }
    }
    hl_patch_free(hp);
}

/* apply_patch: `*** Add/Update/Delete File: PATH` rows; `*** Move to:` after an Update is a move */
static void inspect_apply_patch(const char *input, edit_inspection_t *x) {
    strvec_t lines = str_split_lines(input, strlen(input));
    const char *update = NULL;
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        while (isspace((unsigned char)*l)) l++;
        if (!str_starts(l, "***")) continue;
        const char *q = l + 3;
        if (!isspace((unsigned char)*q)) continue;
        while (isspace((unsigned char)*q)) q++;
        const char *kw[] = {"Add", "Update", "Delete"};
        bool hit = false;
        for (size_t k = 0; k < ARRAY_LEN(kw) && !hit; k++) {
            size_t kl = strlen(kw[k]);
            if (strncmp(q, kw[k], kl) || !isspace((unsigned char)q[kl])) continue;
            const char *r = q + kl;
            while (isspace((unsigned char)*r)) r++;
            if (strncmp(r, "File", 4)) continue;
            r += 4;
            while (isspace((unsigned char)*r)) r++;
            if (*r != ':') continue;
            char *path = str_trim(r + 1);
            hit = true;
            if (*path) {
                strvec_push(&x->paths, path);
                update = k == 1 ? x->paths.v[x->paths.n - 1] : NULL;
            }
            free(path);
        }
        if (!hit && update && !strncmp(q, "Move to", 7)) {
            const char *r = q + 7;
            while (isspace((unsigned char)*r)) r++;
            if (*r == ':') {
                char *to = str_trim(r + 1);
                if (*to) {
                    strvec_push(&x->move_from, update);
                    strvec_push_own(&x->move_to, to);
                } else free(to);
            }
        }
    }
    strvec_free(&lines);
}

static void edit_inspect_mode(const approval_call_t *c, const char *mode, edit_inspection_t *x) {
    memset(x, 0, sizeof *x);
    const char *path = json_get_str(c->args, "path");
    const char *input = json_get_str(c->args, "input");
    if (!strcmp(mode, "replace")) {
        if (path && *path) strvec_push(&x->paths, path);
    } else if (!strcmp(mode, "patch")) {
        if (!path || !*path) return;
        strvec_push(&x->paths, path);
        json_t *e;
        json_arr_foreach(json_get_arr(c->args, "edits"), i, e) {
            const char *op = json_get_str(e, "op"), *rename = json_get_str(e, "rename");
            if (rename && (!op || !strcmp(op, "update"))) {
                strvec_push(&x->move_from, path);
                strvec_push(&x->move_to, rename);
            }
        }
    } else if (!strcmp(mode, "apply_patch")) {
        if (input) inspect_apply_patch(input, x);
    } else if (!strcmp(mode, "sloppy")) {
        sloppy_sections_t secs = {0};
        char *err = NULL;
        if (input && sloppy_split_sections(input, &secs, &err))
            for (size_t i = 0; i < secs.n; i++) strvec_push(&x->paths, secs.v[i].path);
        free(err);
        sloppy_sections_free(&secs);
    } else {
        inspect_hashline(input ? input : "", x);
    }
}

static void edit_inspect(const approval_call_t *c, edit_inspection_t *x) {
    edit_inspect_mode(c, c->env && c->env->edit_mode ? c->env->edit_mode : "hashline", x);
}

/* ---------- search roots (search-approval-targets.ts) ---------- */

/* toPathList: a JSON-encoded string array, else the string itself; arrays keep their strings */
static void path_list(const json_t *raw, strvec_t *out) {
    if (json_is_str(raw)) {
        const char *s = json_sv(raw);
        json_t *arr = s[0] == '[' ? json_parse_str(s) : NULL;
        if (json_is_arr(arr)) {
            json_t *e;
            json_arr_foreach(arr, i, e) if (json_is_str(e)) strvec_push(out, json_sv(e));
        } else {
            strvec_push(out, s);
        }
        json_free(arr);
    } else if (json_is_arr(raw)) {
        json_t *e;
        json_arr_foreach(raw, i, e) if (json_is_str(e)) strvec_push(out, json_sv(e));
    }
}

/* normalizePathLikeInput: trim, strip one pair of outer double quotes */
static char *path_like(const char *s) {
    char *t = str_trim(s);
    size_t n = strlen(t);
    if (n >= 2 && t[0] == '"' && t[n - 1] == '"') {
        memmove(t, t + 1, n - 2);
        t[n - 2] = 0;
    }
    return t;
}

static bool has_glob(const char *s) { return strpbrk(s, "*?[{") != NULL; }

/* the walkable base of a (possibly glob) path: the segments before the first glob segment (owned); *pattern the rest */
static char *glob_base(const char *entry, const char **pattern) {
    if (!has_glob(entry)) {
        *pattern = entry + strlen(entry);
        return xstrdup(entry);
    }
    const char *g = strpbrk(entry, "*?[{");
    const char *slash = g;
    while (slash > entry && slash[-1] != '/') slash--;
    *pattern = slash;
    if (slash == entry) return xstrdup(".");
    if (slash - entry == 1) return xstrdup("/");
    return xstrndup(entry, (size_t)(slash - entry - 1));
}

typedef enum { SR_GREP, SR_GLOB, SR_FIND } search_kind_t;

/* searchRootApprovalKey */
static char *search_root_key(const approval_call_t *c, const char *root, bool recursive) {
    if (!*root) return NULL;
    if (is_readable_url(root)) return public_url(root) ? NULL : target_key(c, root);
    if (url_can_handle(root)) return target_key(c, root);
    if (strstr(root, "://")) return NULL;
    char *abs = resolve_to_cwd(c, root);
    struct stat st;
    char *key = NULL;
    if (stat(abs, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) key = target_key(c, root);
        else {
            char *dk = dir_key(abs);
            key = recursive ? xasprintf(TREE_PREFIX "%s", dk) : xstrdup(dk);
            free(dk);
        }
    }
    free(abs);
    return key;
}

/* read.ts: peel trailing `:selector` tokens unless the literal path exists */
static char *peel_selectors(const approval_call_t *c, const char *p, bool *has_sel) {
    *has_sel = false;
    if (!url_can_handle(p) && !is_readable_url(p)) {
        char *abs = resolve_to_cwd(c, p);
        bool exists = fs_exists(abs);
        free(abs);
        if (exists) return xstrdup(p);
    }
    char *work = xstrdup(p);
    for (int guard = 0; guard < 4; guard++) {
        char *colon = strrchr(work, ':');
        if (!colon || colon == work || (colon[1] == '/' && colon[2] == '/')) break;
        const char *t = colon + 1;
        bool sel = !strcmp(t, "raw") || !strcmp(t, "img") || !strcmp(t, "conflicts");
        if (!sel && *t) {
            sel = true;
            for (const char *q = t; *q && sel; q++)
                sel = isdigit((unsigned char)*q) || strchr("-+,.L:", *q);
        }
        if (!sel) break;
        *colon = 0;
        *has_sel = true;
    }
    return work;
}

static void search_targets(const approval_call_t *c, search_kind_t kind, strvec_t *out) {
    strvec_t roots = {0};
    path_list(json_get(c->args, "path") ? json_get(c->args, "path") : json_get(c->args, "paths"), &roots);
    if (!roots.n) strvec_push(&roots, ".");
    bool exclusion = false;
    for (size_t r = 0; r < roots.n; r++) {
        char *root = path_like(roots.v[r]);
        if (!*root) {
            free(root);
            continue;
        }
        /* approvalRootCandidates: the entry plus its `;`/`,`/whitespace list parts */
        strvec_t cands = {0};
        strvec_push(&cands, root);
        strvec_t parts = {0};
        for (const char *p = root; *p;) {
            size_t sp = strspn(p, ";, \t\n\r");
            p += sp;
            size_t len = strcspn(p, ";, \t\n\r");
            if (len) strvec_push_own(&parts, xstrndup(p, len));
            p += len;
        }
        if (parts.n > 1)
            for (size_t i = 0; i < parts.n; i++) strvec_push(&cands, parts.v[i]);
        strvec_free(&parts);
        for (size_t i = 0; i < cands.n; i++) {
            const char *cand = cands.v[i];
            if (kind == SR_GREP && cand[0] == '!') { /* `!glob` exclusions search nothing */
                exclusion = true;
                continue;
            }
            char *base;
            bool recursive = true;
            if (kind == SR_FIND) base = xstrdup(cand);
            else {
                bool hs = false;
                char *sel_less = kind == SR_GREP ? peel_selectors(c, cand, &hs) : xstrdup(cand);
                const char *pat = NULL;
                base = glob_base(sel_less, &pat);
                if (kind == SR_GLOB) {
                    /* a bare pattern recurses; a one-segment pattern under an explicit base stays in the folder */
                    bool bare = has_glob(sel_less) && !strchr(sel_less, '/');
                    recursive = !*pat || strstr(pat, "**") || bare;
                }
                free(sel_less);
            }
            char *b2 = path_like(base);
            free(base);
            char *key = search_root_key(c, b2, recursive);
            free(b2);
            if (key && !strvec_contains(out, key)) strvec_push_own(out, key);
            else free(key);
        }
        strvec_free(&cands);
        free(root);
    }
    if (exclusion) { /* execution may fall back to cwd: key it too (over-keying only adds a prompt) */
        char *key = search_root_key(c, ".", true);
        if (key && !strvec_contains(out, key)) strvec_push_own(out, key);
        else free(key);
    }
    strvec_free(&roots);
}

/* path-utils.ts searchPathTier */
static approval_tier_t search_path_tier(const json_t *args) {
    strvec_t entries = {0};
    path_list(json_get(args, "path") ? json_get(args, "path") : json_get(args, "paths"), &entries);
    char *joined = strvec_join(&entries, "\n");
    approval_tier_t t = read_tier(joined);
    free(joined);
    strvec_free(&entries);
    return t;
}

/* ---------- bash (tools/bash.ts, tools/shell-tokenize.ts) ---------- */

static const struct {
    const char *re;
    bool icase;
} CRITICAL[] = {
    {"\\brm\\s+(?:-\\S+\\s+)*(?:-[a-z]*[rRfF][a-z]*|--recursive|--force)\\s+(?:-\\S+\\s+)*/", true},
    {"\\brm\\s+(?:-\\S+\\s+)*--no-preserve-root\\b", true},
    {"\\bsudo\\s+rm\\b", true},
    {"\\bchmod\\s+-R\\s+[0-7]+\\s+/", true},
    {"\\bchmod\\s+-R\\s+[ugoa+=rwxXst,-]+\\s+/", false},
    {"\\bchown\\s+-R\\s+\\S+\\s+/", true},
    {":\\(\\)\\s*\\{\\s*:\\s*\\|\\s*:", true},
    {">\\s*/dev/sd[a-z]", true},
    {"\\bmkfs(\\.|\\b)", true},
    {"\\bdd\\s+if=.+of=/dev/(?!(?:null|zero|stdout|stderr|tty|fd/\\d+)(?:[\\s;&|)<>]|$))", true},
    {"\\bshred\\s+/dev/", true},
    {"\\bcryptsetup\\b", true},
    {">\\s*/etc/(?:passwd|shadow|sudoers)\\b", true},
    {"\\btee\\s+(?:-a\\s+)?/etc/(?:passwd|shadow|sudoers)\\b", true},
    {"\\b(?:curl|wget|fetch)\\b[^|]*\\|\\s*(?:bash|sh|zsh|fish)\\b", true},
    {"(?:^|[\\s;&|(])(?:bash|sh|zsh|source|\\.)\\s+<\\(\\s*(?:curl|wget|fetch)\\b", true},
    {"\\beval\\s+[\"'`]?\\$\\(\\s*(?:curl|wget|fetch)\\b|\\beval\\s+`\\s*(?:curl|wget|fetch)\\b", true},
    {"\\bkill\\s+-9\\s+1\\b", false},
    {"(?:^|[\\s;&|(])(?:shutdown|poweroff|reboot|halt)(?:\\s|$|[;|&])", true},
    {"(?:^|[\\s;&|(])init\\s+0\\b", true},
    {"\\bnc\\b[^|;]*\\s-[a-zA-Z]*[ec][a-zA-Z]*\\s", true},
};
static rxbt_t *g_critical[ARRAY_LEN(CRITICAL)];
static pthread_once_t g_critical_once = PTHREAD_ONCE_INIT;

static void critical_init(void) {
    for (size_t i = 0; i < ARRAY_LEN(CRITICAL); i++) {
        char *err = NULL;
        g_critical[i] = rxbt_compile(CRITICAL[i].re, CRITICAL[i].icase ? RX_ICASE : 0, &err);
        if (!g_critical[i]) LOGW("approval: critical pattern %zu: %s", i, err ? err : "?");
        free(err);
    }
}

bool approval_bash_critical(const char *command) {
    if (!command || !*command) return false;
    pthread_once(&g_critical_once, critical_init);
    for (size_t i = 0; i < ARRAY_LEN(CRITICAL); i++)
        if (g_critical[i] && rxbt_search(g_critical[i], command, strlen(command), 0, NULL, 0)) return true;
    return false;
}

/* isShellEnvironmentAssignment: SHELL_ASSIGNMENT `NAME=`, `NAME+=`, `NAME[sub]=` */
static bool shell_assignment_word(const char *w) {
    if (!(isalpha((unsigned char)*w) || *w == '_')) return false;
    w++;
    while (isalnum((unsigned char)*w) || *w == '_') w++;
    if (*w == '[') {
        const char *e = strchr(w, ']');
        if (!e) return false;
        w = e + 1;
    }
    if (*w == '+') w++;
    return *w == '=';
}

/* bashCommandSegments: each segment's words joined, plus the words after its leading `NAME=value` assignments */
static void push_segment(strvec_t *cur, strvec_t *out) {
    if (!cur->n) return;
    strvec_push_own(out, strvec_join(cur, " "));
    size_t first = 0;
    while (first < cur->n && shell_assignment_word(cur->v[first])) first++;
    if (first > 0 && first < cur->n) {
        strvec_t rest = {0};
        for (size_t i = first; i < cur->n; i++) strvec_push(&rest, cur->v[i]);
        strvec_push_own(out, strvec_join(&rest, " "));
        strvec_free(&rest);
    }
    strvec_free(cur);
}

/* lexShellCommand + tokenizeShellSegments (bashCommandSegments) */
void approval_shell_segments(const char *command, strvec_t *out) {
    strvec_t cur = {0};
    sb_t value = SB_INIT;
    bool in_word = false, in_single = false, in_double = false;
    size_t n = strlen(command);
    for (size_t i = 0; i <= n; i++) {
        char ch = i < n ? command[i] : 0;
        if (i < n && in_single) {
            if (ch == '\'') in_single = false;
            else sb_putc(&value, ch);
            continue;
        }
        if (i < n && in_double) {
            if (ch == '\\' && i + 1 < n && strchr("\"\\$`", command[i + 1])) {
                sb_putc(&value, command[++i]);
                continue;
            }
            if (ch == '"') in_double = false;
            else sb_putc(&value, ch);
            continue;
        }
        bool sep = i == n || ch == ' ' || ch == '\t' || ch == '\n' || ch == ';' || ch == '&' || ch == '|' || ch == '(' ||
                   ch == ')';
        if (sep) {
            if (in_word) {
                if (value.len) strvec_push(&cur, sb_cstr(&value));
                sb_clear(&value);
                in_word = false;
            }
            if (i < n && strchr("\n;&|()", ch) && cur.n) push_segment(&cur, out);
            continue;
        }
        in_word = true;
        if (ch == '\'') in_single = true;
        else if (ch == '"') in_double = true;
        else if (ch == '\\' && i + 1 < n) sb_putc(&value, command[++i]);
        else sb_putc(&value, ch);
    }
    if (in_word && value.len) strvec_push(&cur, sb_cstr(&value));
    push_segment(&cur, out);
    strvec_free(&cur);
    sb_free(&value);
}

static bool shell_control_char(char ch) { return ch && strchr("\n\r;&|<>`$()", ch); }

static rxbt_t *g_reinterp;
static pthread_once_t g_reinterp_once = PTHREAD_ONCE_INIT;
static void reinterp_init(void) {
    g_reinterp = rxbt_compile("(?:^|[ \\t])(?:-[^-]*[ce]|--(?:command|eval))(?:[= \\t]|$)", 0, NULL);
}

/* hasBashApprovalShellControl */
static bool has_shell_control(const char *command) {
    char quote = 0;
    bool reinterpretable = false;
    for (size_t i = 0; command[i]; i++) {
        char ch = command[i];
        if (quote == '\'') {
            if (ch == '\'') quote = 0;
            else if (shell_control_char(ch)) reinterpretable = true;
            continue;
        }
        if (ch == '\\') {
            if (shell_control_char(command[i + 1])) reinterpretable = true;
            if (command[i + 1]) i++;
            continue;
        }
        if (quote == '"') {
            if (ch == '"') {
                quote = 0;
                continue;
            }
            if (ch == '`' || ch == '$') return true;
            if (shell_control_char(ch)) reinterpretable = true;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            continue;
        }
        if (shell_control_char(ch)) return true;
    }
    if (!reinterpretable) return false;
    /* BASH_APPROVAL_REINTERPRETED_ARGUMENT_RE */
    pthread_once(&g_reinterp_once, reinterp_init);
    rxbt_t *re = g_reinterp;
    return re && rxbt_search(re, command, strlen(command), 0, NULL, 0);
}

/* normalizeBashApprovalPattern: trim, collapse whitespace runs to one space */
static char *norm_ws(const char *s) {
    sb_t b = SB_INIT;
    bool sp = false;
    char *t = str_trim(s);
    for (const char *p = t; *p; p++) {
        if (isspace((unsigned char)*p)) sp = true;
        else {
            if (sp && b.len) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *p);
        }
    }
    free(t);
    return sb_detach(&b);
}

/* `*` wildcard over the whole (normalized) text; `.` of the upstream regex never crosses a newline */
static bool wild_match(const char *pat, const char *s) {
    const char *star = NULL, *ss = NULL;
    while (*s) {
        if (*pat == '*') {
            star = pat++;
            ss = s;
        } else if (*pat == *s) {
            pat++;
            s++;
        } else if (star && *ss != '\n') {
            pat = star + 1;
            s = ++ss;
        } else {
            return false;
        }
    }
    while (*pat == '*') pat++;
    return !*pat;
}

static bool pattern_matches(const char *command, const char *pattern) {
    char *nc = norm_ws(command);
    bool r = *nc && wild_match(pattern, nc);
    free(nc);
    return r;
}

static bool segment_matches(const char *command, const char *pattern) {
    if (pattern_matches(command, pattern)) return true;
    strvec_t segs = {0};
    approval_shell_segments(command, &segs);
    bool hit = false;
    for (size_t i = 0; i < segs.n && !hit; i++) hit = wild_match(pattern, segs.v[i]);
    strvec_free(&segs);
    return hit;
}

typedef struct {
    char *match; /* normalized */
    approval_policy_t pol;
    strvec_t env; /* leading env assignment names this `allow` rule strips before matching */
} bash_rule_t;

/* normalizeBashAllowedEnvList: the string entries, trimmed, non-empty */
static void env_list_load(const json_t *v, strvec_t *out) {
    const json_t *e;
    json_arr_foreach(json_is_arr(v) ? v : NULL, i, e) {
        if (!json_is_str(e)) continue;
        char *t = str_trim(json_sv(e));
        if (*t) strvec_push_own(out, t);
        else free(t);
    }
}

/* skipShellWord: the end of a shell word (quotes and escapes respected), -1 for incomplete syntax */
static ssize_t skip_shell_word(const char *c, size_t start) {
    bool sq = false, dq = false;
    size_t n = strlen(c);
    for (size_t i = start; i < n; i++) {
        char ch = c[i];
        if (sq) {
            if (ch == '\'') sq = false;
            continue;
        }
        if (dq) {
            if (ch == '\\') {
                if (i + 1 >= n) return -1;
                i++;
                continue;
            }
            if (ch == '"') dq = false;
            continue;
        }
        if (ch == '\'') sq = true;
        else if (ch == '"') dq = true;
        else if (ch == '\\') {
            if (i + 1 >= n) return -1;
            i++;
        } else if (ch == ' ' || ch == '\t') return (ssize_t)i;
    }
    return sq || dq ? -1 : (ssize_t)n;
}

/* withoutLeadingEnvironmentAssignments with an exact-name allowlist (names never glob: `CARGO_*` would admit
 * `CARGO_BUILD_RUSTC_WRAPPER`): the command after its leading allowed `NAME=value` words, or NULL */
static char *strip_env_assignments(const char *command, const strvec_t *a, const strvec_t *b) {
    if (!a->n && !b->n) return NULL;
    size_t i = 0, n = strlen(command);
    bool found = false;
    while (i < n) {
        while (command[i] == ' ' || command[i] == '\t') i++;
        size_t k = i;
        if (!(isalpha((unsigned char)command[k]) || command[k] == '_')) break;
        while (isalnum((unsigned char)command[k]) || command[k] == '_') k++;
        size_t name_end = k;
        if (command[k] == '+') k++;
        if (command[k] != '=') break;
        char *name = xstrndup(command + i, name_end - i);
        bool ok = strvec_contains(a, name) || strvec_contains(b, name);
        free(name);
        if (!ok) break;
        ssize_t end = skip_shell_word(command, k + 1);
        if (end < 0) return NULL;
        found = true;
        i = (size_t)end;
    }
    if (!found) return NULL;
    while (command[i] == ' ' || command[i] == '\t' || command[i] == '\n' || command[i] == '\r') i++;
    return command[i] ? xstrdup(command + i) : NULL;
}

/* getBashApprovalPatternRules */
static size_t bash_rules_load(const json_t *patterns, bash_rule_t **out) {
    size_t n = 0;
    *out = NULL;
    json_t *rule;
    json_arr_foreach(json_is_arr(patterns) ? patterns : NULL, i, rule) {
        if (!json_is_obj(rule) || !json_is_str(json_get(rule, "match"))) continue;
        char *m = norm_ws(json_get_str(rule, "match"));
        approval_policy_t p = policy_parse(json_get(rule, "approval"));
        if (!*m || p == APOL_UNSET) {
            free(m);
            continue;
        }
        *out = xrealloc(*out, (n + 1) * sizeof **out);
        (*out)[n] = (bash_rule_t){m, p, {0}};
        env_list_load(json_get(rule, "env"), &(*out)[n].env);
        n++;
    }
    return n;
}

static void bash_rules_free(bash_rule_t *r, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(r[i].match);
        strvec_free(&r[i].env);
    }
    free(r);
}

/* findNonCompoundBashRestrictionRule: a deny matching any segment (or only the whole line) outranks every prompt */
static const bash_rule_t *bash_restriction_rule(const char *command, const bash_rule_t *rules, size_t n) {
    strvec_t segs = {0};
    approval_shell_segments(command, &segs);
    const bash_rule_t *prompt = NULL, *hit = NULL;
    for (size_t r = 0; r < n && !hit; r++) {
        if (rules[r].pol == APOL_ALLOW || !pattern_matches(command, rules[r].match)) continue;
        bool seg_hit = false;
        for (size_t i = 0; i < segs.n && !seg_hit; i++) seg_hit = pattern_matches(segs.v[i], rules[r].match);
        if (seg_hit) continue;
        if (rules[r].pol == APOL_DENY) hit = &rules[r];
        else if (!prompt) prompt = &rules[r];
    }
    for (size_t i = 0; i < segs.n && !hit; i++) {
        const bash_rule_t *sr = NULL;
        for (size_t r = 0; r < n && !sr; r++)
            if (rules[r].pol != APOL_ALLOW && pattern_matches(segs.v[i], rules[r].match)) sr = &rules[r];
        if (sr && sr->pol == APOL_DENY) hit = sr;
        else if (sr && !prompt) prompt = sr;
    }
    strvec_free(&segs);
    return hit ? hit : prompt;
}

/* bashApprovalRuleMatches for `allow`: the whole command, or the command after leading assignments of the names the
 * rule's `env` and bash.allowedEnvVars allow */
static bool allow_rule_matches(const char *command, const bash_rule_t *r, const strvec_t *allowed) {
    if (pattern_matches(command, r->match)) return true;
    char *stripped = strip_env_assignments(command, &r->env, allowed);
    bool hit = stripped && pattern_matches(stripped, r->match);
    free(stripped);
    return hit;
}

/* findBashApprovalPatternRule */
static const bash_rule_t *bash_pattern_rule(const char *command, const bash_rule_t *rules, size_t n, const strvec_t *allowed) {
    if (has_shell_control(command)) return bash_restriction_rule(command, rules, n);
    for (size_t r = 0; r < n; r++)
        if (rules[r].pol == APOL_ALLOW ? allow_rule_matches(command, &rules[r], allowed) : segment_matches(command, rules[r].match))
            return &rules[r];
    return NULL;
}

/* isToolCommandLine over the session's tool command names */
static bool bash_tool_command_line(const approval_call_t *c, const char *command) {
    if (!c->agent) return false;
    strvec_t names = {0};
    toolcmd_names(c->agent, &names);
    bool r = toolcmd_is_line(command, &names);
    strvec_free(&names);
    return r;
}

static void bash_decision(const approval_call_t *c, const json_t *args, approval_decision_t *out) {
    const char *command = json_get_str(args, "command");
    if (!command) command = "";
    bash_rule_t *rules, *arules;
    size_t n = bash_rules_load(settings_get("bash.patterns"), &rules);
    /* project layers may tighten the operator's bash.patterns, never loosen them: an operator deny or prompt still
     * applies under a project allow (Settings.grantAuthority) */
    json_t *operator_patterns = settings_operator_get("bash.patterns");
    size_t an = bash_rules_load(operator_patterns, &arules);
    json_free(operator_patterns);
    strvec_t env = {0}, aenv = {0};
    env_list_load(settings_get("bash.allowedEnvVars"), &env);
    json_t *operator_env = settings_operator_get("bash.allowedEnvVars");
    env_list_load(operator_env, &aenv);
    json_free(operator_env);
    const bash_rule_t *rule = bash_pattern_rule(command, rules, n, &env);
    const bash_rule_t *arule = bash_pattern_rule(command, arules, an, &aenv);
    strvec_free(&env);
    strvec_free(&aenv);
    const bash_rule_t *denied = arule && arule->pol == APOL_DENY ? arule : rule && rule->pol == APOL_DENY ? rule : NULL;
    const bash_rule_t *prompted =
        arule && arule->pol == APOL_PROMPT ? arule : rule && rule->pol == APOL_PROMPT ? rule : NULL;
    dec_tier(out, TIER_EXEC);
    if (denied) {
        out->override = true;
        out->policy = APOL_DENY;
        out->reason = xasprintf("Blocked by bash pattern: %s", denied->match);
    } else if (prompted) { /* an explicit prompt rule keeps its own reason over a critical pattern (omp 00929d42c2) */
        out->override = true;
        out->policy = APOL_PROMPT;
        out->reason = xasprintf("Prompt required by bash pattern: %s", prompted->match);
    } else if (approval_bash_critical(command)) {
        out->override = true;
        out->reason = xstrdup("Critical pattern detected");
    } else if (bash_tool_command_line(c, command)) {
        out->tier = TIER_READ; /* each tool command passes its own approval when it runs; the line itself only reads */
    } else if (rule && rule->pol == APOL_ALLOW) {
        out->tier = TIER_WRITE;
        out->policy = APOL_ALLOW;
    }
    bash_rules_free(rules, n);
    bash_rules_free(arules, an);
}

/* ---------- per-tool declarations ---------- */

static bool in_list(const char *s, const char *const *list) {
    for (; s && *list; list++)
        if (!strcmp(s, *list)) return true;
    return false;
}

static const char *const GITHUB_READONLY[] = {"repo_view", "file_read", "search_issues", "search_prs", "search_code",
                                              "search_commits", "search_repos", "run_watch", NULL};
static const char *const DEBUG_READONLY[] = {"output", "threads", "stack_trace", "scopes", "variables", "disassemble",
                                             "read_memory", "loaded_sources", "modules", "sessions", NULL};
static const char *const LSP_READONLY[] = {"diagnostics", "definition", "type_definition", "implementation", "references",
                                           "hover", "symbols", "status", "capabilities", NULL};
static const char *const READ_TOOLS[] = {"todo", "ask", "wait", "checkpoint", "rewind", "web_search", NULL};

static char *lower_arg(const json_t *args, const char *key) {
    const char *v = json_get_str(args, key);
    return v ? str_lower(v) : xstrdup("");
}

static void tool_decision_depth(const approval_call_t *c, approval_decision_t *out, int depth) {
    const char *t = c->tool ? c->tool : "";
    const json_t *args = c->args;
    if (!strcmp(t, "read")) {
        const char *p = json_get_str(args, "path");
        if (!p) p = "";
        approval_tier_t ut = read_tier(p);
        if (ut != TIER_READ) {
            dec_tier(out, ut);
            return;
        }
        /* the legacy PDF image-member read (`file.pdf:<member>`) renders a page: exec, unless a line selector */
        const char *pdf = NULL;
        for (const char *q = p; (q = str_istr(q, ".pdf:")); q++) pdf = q;
        bool pdf_image = false;
        if (pdf) {
            const char *m = pdf + 5;
            bool sel = *m != 0;
            for (const char *q = m; *q && sel; q++) sel = isdigit((unsigned char)*q) || strchr("-+,.L:", *q);
            pdf_image = !sel && strcmp(m, "raw") != 0;
        }
        dec_tier(out, pdf_image ? TIER_EXEC : TIER_READ);
    } else if (!strcmp(t, "write")) {
        const char *p = json_get_str(args, "path");
        if (!p) {
            dec_tier(out, TIER_WRITE);
            return;
        }
        char *u = unwrap_header_path(p);
        write_tier(c, u, json_get(args, "content"), out, depth);
        free(u);
    } else if (!strcmp(t, "edit")) {
        edit_inspection_t x;
        edit_inspect(c, &x);
        strvec_t targets = {0};
        for (size_t i = 0; i < x.paths.n; i++) strvec_push(&targets, x.paths.v[i]);
        for (size_t i = 0; i < x.move_to.n; i++) strvec_push(&targets, x.move_to.v[i]);
        if (!targets.n) dec_tier(out, TIER_WRITE);
        else {
            approval_decision_t *ds = xcalloc(targets.n, sizeof *ds);
            for (size_t i = 0; i < targets.n; i++) write_tier(c, targets.v[i], NULL, &ds[i], depth);
            strictest(ds, targets.n, out);
            for (size_t i = 0; i < targets.n; i++) approval_decision_free(&ds[i]);
            free(ds);
        }
        strvec_free(&targets);
        edit_inspection_free(&x);
    } else if (!strcmp(t, "ast_edit")) {
        json_t *paths = json_get(args, "paths");
        if (!json_is_arr(paths) || !json_len(paths)) {
            dec_tier(out, TIER_WRITE);
            return;
        }
        size_t n = json_len(paths);
        approval_decision_t *ds = xcalloc(n, sizeof *ds);
        bool bad = false;
        for (size_t i = 0; i < n && !bad; i++) {
            json_t *e = json_at(paths, i);
            if (!json_is_str(e)) bad = true;
            else write_tier(c, json_sv(e), NULL, &ds[i], depth);
        }
        if (bad) dec_tier(out, TIER_EXEC);
        else strictest(ds, n, out);
        for (size_t i = 0; i < n; i++) approval_decision_free(&ds[i]);
        free(ds);
    } else if (!strcmp(t, "bash")) {
        bash_decision(c, args, out);
    } else if (!strcmp(t, "grep") || !strcmp(t, "glob") || !strcmp(t, "ast_grep") || !strcmp(t, "find")) {
        dec_tier(out, search_path_tier(args));
    } else if (!strcmp(t, "github")) {
        dec_tier(out, in_list(json_get_str(args, "op"), GITHUB_READONLY) ? TIER_READ : TIER_EXEC);
    } else if (!strcmp(t, "debug")) {
        char *a = lower_arg(args, "action");
        dec_tier(out, in_list(a, DEBUG_READONLY) ? TIER_READ : TIER_EXEC);
        free(a);
    } else if (!strcmp(t, "lsp")) {
        char *a = lower_arg(args, "action");
        dec_tier(out, in_list(a, LSP_READONLY) ? TIER_READ : TIER_WRITE);
        free(a);
    } else if (!strcmp(t, "goal")) {
        dec_tier(out, str_eq(json_get_str(args, "op"), "get") ? TIER_READ : TIER_WRITE);
    } else if (!strcmp(t, "learn")) {
        bool w = json_truthy(json_get(args, "skill")) || str_eq(json_get_str(args, "scope"), "global") ||
                 str_eq(settings_str("memory.backend", "off"), "local");
        dec_tier(out, w ? TIER_WRITE : TIER_READ);
    } else if (!strcmp(t, "manage_skill") || !strcmp(t, "report_issue")) {
        dec_tier(out, TIER_WRITE);
    } else if (!strcmp(t, "resolve") || !strcmp(t, "reject") || !strcmp(t, "propose")) {
        dec_tier(out, TIER_READ);
    } else if (in_list(t, READ_TOOLS)) {
        dec_tier(out, TIER_READ);
    } else if (str_starts(t, "mcp__")) {
        dec_tier(out, TIER_WRITE);
    } else {
        dec_tier(out, TIER_EXEC); /* eval, task and every tool that declares no approval */
    }
}

void approval_tool_decision(const approval_call_t *c, approval_decision_t *out) { tool_decision_depth(c, out, 0); }

/* ---------- formatApprovalArguments ---------- */

static void js_str(sb_t *b, const char *s) {
    sb_t q = SB_INIT;
    json_js_quote_sb(&q, s, strlen(s));
    char *san = sanitize(sb_cstr(&q));
    sb_append(b, san);
    free(san);
    sb_free(&q);
}

static void payload_value(sb_t *b, const json_t *v, int indent, int depth) {
    if (depth > 64) {
        sb_append(b, "…");
        return;
    }
    if (json_is_null(v)) sb_append(b, "null");
    else if (json_is_bool(v)) sb_append(b, v->u.b ? "true" : "false");
    else if (json_is_num(v)) json_js_num_sb(b, v->u.num.d);
    else if (json_is_str(v)) {
        char *lf = str_replace(json_sv(v), "\r\n", "\n");
        char *norm = str_replace(lf, "\r", "\n");
        char *tabs = tui_replace_tabs(norm);
        free(lf);
        free(norm);
        if (strchr(tabs, '\n')) {
            strvec_t lines = str_split(tabs, "\n");
            sb_append(b, "\"\n");
            for (size_t i = 0; i < lines.n; i++) {
                sb_t q = SB_INIT;
                json_js_quote_sb(&q, lines.v[i], strlen(lines.v[i]));
                const char *qs = sb_cstr(&q);
                char *body = xstrndup(qs + 1, q.len >= 2 ? q.len - 2 : 0);
                char *san = sanitize(body);
                sb_repeat(b, " ", indent + 2);
                sb_append(b, san);
                sb_putc(b, '\n');
                free(san);
                free(body);
                sb_free(&q);
            }
            sb_repeat(b, " ", indent);
            sb_putc(b, '"');
            strvec_free(&lines);
        } else {
            js_str(b, tabs);
        }
        free(tabs);
    } else if (json_is_arr(v)) {
        if (!json_len(v)) {
            sb_append(b, "[]");
            return;
        }
        sb_append(b, "[\n");
        for (size_t i = 0; i < json_len(v); i++) {
            sb_repeat(b, " ", indent + 2);
            payload_value(b, json_at(v, i), indent + 2, depth + 1);
            if (i + 1 < json_len(v)) sb_putc(b, ',');
            sb_putc(b, '\n');
        }
        sb_repeat(b, " ", indent);
        sb_putc(b, ']');
    } else if (json_is_obj(v)) {
        if (!v->u.obj.n) {
            sb_append(b, "{}");
            return;
        }
        sb_append(b, "{\n");
        for (size_t i = 0; i < v->u.obj.n; i++) {
            sb_repeat(b, " ", indent + 2);
            js_str(b, v->u.obj.keys[i]);
            sb_append(b, ": ");
            payload_value(b, v->u.obj.vals[i], indent + 2, depth + 1);
            if (i + 1 < v->u.obj.n) sb_putc(b, ',');
            sb_putc(b, '\n');
        }
        sb_repeat(b, " ", indent);
        sb_putc(b, '}');
    }
}

void approval_format_arguments(const json_t *args, bool show_empty, strvec_t *out) {
    if (!args) return;
    if (!show_empty && (json_is_null(args) || ((json_is_obj(args) || json_is_arr(args)) && !json_len(args)))) return;
    sb_t b = SB_INIT;
    payload_value(&b, args, 0, 0);
    char *t = approval_truncate(sb_cstr(&b));
    strvec_push_own(out, xasprintf("Arguments:\n%s", t));
    free(t);
    sb_free(&b);
}

/* ---------- formatApprovalDetails ---------- */

static void push_tr(strvec_t *out, const char *label, const char *v) {
    char *t = approval_truncate(v);
    strvec_push_own(out, xasprintf("%s%s", label, t));
    free(t);
}

static void details_raw(const approval_call_t *c, strvec_t *out) {
    const char *t = c->tool ? c->tool : "";
    const json_t *args = c->args;
    if (!strcmp(t, "bash")) {
        const char *cmd = json_get_str(args, "command");
        push_tr(out, "Command: ", cmd ? cmd : "(missing)");
    } else if (!strcmp(t, "write")) {
        const char *p = json_get_str(args, "path");
        const char *target = p ? p : "(missing)";
        const char *content = json_get_str(args, "content");
        push_tr(out, "Path: ", target);
        push_tr(out, "Content:\n", content ? content : "");
    } else if (!strcmp(t, "edit")) {
        edit_inspection_t x;
        edit_inspect(c, &x);
        for (size_t i = 0; i < x.paths.n; i++) push_tr(out, "File: ", x.paths.v[i]);
        for (size_t i = 0; i < x.move_to.n; i++) {
            char *a = approval_truncate(x.move_from.v[i]), *b = approval_truncate(x.move_to.v[i]);
            strvec_push_own(out, xasprintf("Move: %s → %s", a, b));
            free(a);
            free(b);
        }
        if (!out->n) strvec_push(out, "File: (unknown)");
        edit_inspection_free(&x);
    } else if (!strcmp(t, "ast_edit")) {
        json_t *ops = json_get_arr(args, "ops");
        json_t *first = json_at(ops, 0);
        if (first) {
            const char *pat = json_get_str(first, "pat"), *o = json_get_str(first, "out");
            push_tr(out, "Pattern: ", pat ? pat : "undefined");
            push_tr(out, "Replacement: ", o ? o : "undefined");
            size_t n = json_len(ops);
            if (n > 1) strvec_push_own(out, xasprintf("+%zu more op%s", n - 1, n == 2 ? "" : "s"));
        }
        json_t *paths = json_get_arr(args, "paths");
        if (json_len(paths)) {
            strvec_t ps = {0};
            json_t *e;
            json_arr_foreach(paths, i, e) strvec_push_own(&ps, json_is_str(e) ? xstrdup(json_sv(e)) : json_dump(e, 0));
            char *j = strvec_join(&ps, ", ");
            push_tr(out, "Paths: ", j);
            free(j);
            strvec_free(&ps);
        }
    } else if (!strcmp(t, "eval")) {
        const char *lang = json_get_str(args, "language");
        const char *shown = !lang ? "javascript (default)" : (!strcmp(lang, "py") || !strcmp(lang, "python")) ? "python"
                            : (!strcmp(lang, "js") || !strcmp(lang, "javascript")) ? "javascript" : lang;
        strvec_push_own(out, xasprintf("Language: %s", shown));
        const char *code = json_get_str(args, "code");
        push_tr(out, "Code:\n", code ? code : "");
    } else if (!strcmp(t, "debug")) {
        const char *a = json_get_str(args, "action"), *prog = json_get_str(args, "program");
        strvec_push_own(out, xasprintf("Action: %s", a ? a : "(missing)"));
        if (prog && *prog) push_tr(out, "Program: ", prog);
    } else if (!strcmp(t, "lsp")) {
        const char *a = json_get_str(args, "action"), *f = json_get_str(args, "file");
        strvec_push_own(out, xasprintf("Action: %s", a ? a : "(missing)"));
        if (f && *f) push_tr(out, "File: ", f);
    } else if (str_starts(t, "mcp__")) {
        approval_format_arguments(args, false, out);
    }
}

void approval_tool_details(const approval_call_t *c, strvec_t *out) {
    strvec_t raw = {0};
    details_raw(c, &raw);
    for (size_t i = 0; i < raw.n; i++)
        if (*raw.v[i]) strvec_push(out, raw.v[i]);
    strvec_free(&raw);
}

/* ---------- approvalCacheTargets ---------- */

static void cache_targets_raw(const approval_call_t *c, strvec_t *out) {
    const char *t = c->tool ? c->tool : "";
    const json_t *args = c->args;
    if (!strcmp(t, "read")) {
        const char *raw = json_get_str(args, "path");
        if (!raw || !*raw) return;
        bool has_sel = false;
        char *sel_less = peel_selectors(c, raw, &has_sel);
        if (is_readable_url(sel_less) && public_url(sel_less)) {
            free(sel_less);
            return;
        }
        char *key = target_key(c, sel_less);
        free(sel_less);
        if (!key) return;
        /* a directory read keys as the folder grant */
        if (key[0] == '/' && !target_names_file(key)) {
            char *dk = dir_key(key);
            free(key);
            key = dk;
        }
        strvec_push_own(out, key);
    } else if (!strcmp(t, "write")) {
        const char *p = json_get_str(args, "path");
        if (!p || !*p) return;
        char *u = unwrap_header_path(p);
        char *key = target_key(c, u);
        free(u);
        if (key) strvec_push_own(out, key);
    } else if (!strcmp(t, "edit")) {
        edit_inspection_t x;
        edit_inspect(c, &x);
        for (size_t i = 0; i < x.paths.n + x.move_to.n; i++) {
            const char *p = i < x.paths.n ? x.paths.v[i] : x.move_to.v[i - x.paths.n];
            char *key = target_key(c, p);
            if (key) strvec_push_own(out, key);
        }
        edit_inspection_free(&x);
    } else if (!strcmp(t, "grep")) {
        search_targets(c, SR_GREP, out);
    } else if (!strcmp(t, "glob")) {
        search_targets(c, SR_GLOB, out);
    } else if (!strcmp(t, "ast_grep")) {
        search_targets(c, SR_GREP, out);
    } else if (!strcmp(t, "find")) {
        search_targets(c, SR_FIND, out);
    }
}

void approval_cache_targets(const approval_call_t *c, strvec_t *out) {
    strvec_t raw = {0};
    cache_targets_raw(c, &raw);
    for (size_t i = 0; i < raw.n; i++)
        if (*raw.v[i]) strvec_push(out, raw.v[i]);
    strvec_free(&raw);
}

/* ---------- remembered grants ---------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static hmap_t g_session_grants;    /* session id -> strvec_t* of grant keys */
static hmap_t g_session_approvals; /* session id -> strvec_t* of session-approval keys */
static bool g_maps_init;

static strvec_t *session_set(hmap_t *m, const char *sid, bool create) {
    if (!g_maps_init) {
        hmap_init(&g_session_grants);
        hmap_init(&g_session_approvals);
        g_maps_init = true;
    }
    strvec_t *v = hmap_get(m, sid);
    if (!v && create) {
        v = xcalloc(1, sizeof *v);
        hmap_put(m, sid, v);
    }
    return v;
}

char *approval_grant_key(approval_tier_t tier, const char *subject) {
    return xasprintf("%s" SEP "%s", TIER_NAMES[tier], subject);
}

static char *cache_path(void) { return omc_agent_path("approval-cache.json"); }

/* the global grants file: {"grants":[{"key","tool","approvedAt"}]} (owned; empty object when absent) */
static json_t *cache_load(const char *path) {
    char *s = fs_read(path, NULL);
    json_t *j = s ? json_parse_str(s) : NULL;
    free(s);
    if (!json_is_obj(j) || !json_is_arr(json_get(j, "grants"))) {
        json_free(j);
        j = json_obj();
        json_set(j, "grants", json_arr());
    }
    return j;
}

static int cache_lock(const char *path) {
    char *lp = xasprintf("%s.lock", path);
    char *dir = path_dirname(path);
    fs_mkdir_p(dir, 0700);
    free(dir);
    int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(lp);
    if (fd >= 0) flock(fd, LOCK_EX);
    return fd;
}

static void cache_unlock(int fd) {
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

void approval_grants_present(const char *session_id, const strvec_t *keys, bool *present) {
    for (size_t i = 0; i < keys->n; i++) present[i] = false;
    pthread_mutex_lock(&g_mu);
    strvec_t *sv = session_id ? session_set(&g_session_grants, session_id, false) : NULL;
    bool rest = false;
    for (size_t i = 0; i < keys->n; i++) {
        present[i] = sv && strvec_contains(sv, keys->v[i]);
        if (!present[i]) rest = true;
    }
    pthread_mutex_unlock(&g_mu);
    if (!rest) return;
    char *path = cache_path();
    if (fs_is_file(path)) { /* checks never create the file */
        json_t *j = cache_load(path);
        json_t *g;
        json_arr_foreach(json_get_arr(j, "grants"), k, g) {
            const char *key = json_get_str(g, "key");
            for (size_t i = 0; key && i < keys->n; i++)
                if (!present[i] && !strcmp(keys->v[i], key)) present[i] = true;
        }
        json_free(j);
    }
    free(path);
}

void approval_grants_record(const char *session_id, const char *key, const char *tool, bool global) {
    if (!global) {
        if (!session_id) return;
        pthread_mutex_lock(&g_mu);
        strvec_t *sv = session_set(&g_session_grants, session_id, true);
        if (!strvec_contains(sv, key)) strvec_push(sv, key);
        pthread_mutex_unlock(&g_mu);
        return;
    }
    char *path = cache_path();
    int fd = cache_lock(path);
    json_t *j = cache_load(path);
    json_t *grants = json_get_arr(j, "grants"), *g;
    bool have = false;
    json_arr_foreach(grants, i, g) if (str_eq(json_get_str(g, "key"), key)) have = true;
    if (!have) { /* ON CONFLICT(key) DO NOTHING */
        json_t *row = json_obj();
        json_set_str(row, "key", key);
        json_set_str(row, "tool", tool ? tool : "");
        char ts[40];
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S.000Z", &tm);
        json_set_str(row, "approvedAt", ts);
        json_push(grants, row);
        char *s = json_dump(j, 1);
        if (fs_write_atomic(path, s, strlen(s), 0600) != 0) LOGW("approval cache: write failed (%s)", path);
        free(s);
    }
    json_free(j);
    cache_unlock(fd);
    free(path);
}

static void free_strvec(void *p) {
    strvec_free(p);
    free(p);
}

void approval_grants_clear(void) {
    pthread_mutex_lock(&g_mu);
    if (g_maps_init) {
        hmap_free(&g_session_grants, free_strvec);
        hmap_init(&g_session_grants);
    }
    pthread_mutex_unlock(&g_mu);
    char *path = cache_path();
    if (fs_is_file(path)) { /* a missing database has nothing to forget */
        int fd = cache_lock(path);
        json_t *j = cache_load(path);
        json_set(j, "grants", json_arr());
        char *s = json_dump(j, 1);
        if (fs_write_atomic(path, s, strlen(s), 0600) != 0) LOGW("approval cache: clear failed (%s)", path);
        free(s);
        json_free(j);
        cache_unlock(fd);
    }
    free(path);
}

bool approval_session_has(const char *session_id, const char *key) {
    if (!session_id) return false;
    pthread_mutex_lock(&g_mu);
    strvec_t *sv = session_set(&g_session_approvals, session_id, false);
    bool r = sv && strvec_contains(sv, key);
    pthread_mutex_unlock(&g_mu);
    return r;
}

void approval_session_grant(const char *session_id, const char *key) {
    if (!session_id) return;
    pthread_mutex_lock(&g_mu);
    strvec_t *sv = session_set(&g_session_approvals, session_id, true);
    if (!strvec_contains(sv, key)) strvec_push(sv, key);
    pthread_mutex_unlock(&g_mu);
}

/* ---------- resolution ---------- */

void approval_ctx_from_settings(approval_ctx_t *ctx, bool auto_approve, const char *session_id, const char *cwd) {
    memset(ctx, 0, sizeof *ctx);
    json_t *pol = settings_get("tools.approval");
    ctx->user_policies = json_is_obj(pol) ? pol : NULL;
    int m = approval_mode_parse(settings_str("tools.approvalMode", "yolo"));
    ctx->mode = auto_approve ? AMODE_YOLO : m < 0 ? AMODE_YOLO : (approval_mode_t)m;
    ctx->use_grants = ctx->mode == AMODE_ALWAYS_ASK_CACHED;
    ctx->session_id = session_id;
    json_t *rf = settings_get("tools.restrictedFolders");
    ctx->restricted = json_is_arr(rf) && json_len(rf) ? rf : NULL;
    ctx->cwd = cwd;
}

/* coveringFolderKey: a directory key is its own folder, an absolute file key its parent's */
static char *covering_folder(const char *target) {
    if (target[0] != '/') return NULL;
    if (str_ends(target, "/")) return xstrdup(target);
    char *parent = path_dirname(target);
    if (str_ends(parent, "/")) return parent;
    char *r = xasprintf("%s/", parent);
    free(parent);
    return r;
}

/* resolveCachedModeApproval */
static void cached_mode(const approval_call_t *c, const approval_ctx_t *ctx, const approval_decision_t *d,
                        approval_resolved_t *out) {
    out->tier = d->tier;
    out->source = ASRC_MODE;
    out->policy_key = d->policy_key ? xstrdup(d->policy_key) : NULL;
    strvec_t targets = {0};
    approval_cache_targets(c, &targets);
    if (!targets.n) {
        if (d->tier == TIER_READ) out->policy = APOL_ALLOW;
        else {
            out->policy = APOL_PROMPT;
            out->reason = d->reason ? xstrdup(d->reason) : NULL;
        }
        strvec_free(&targets);
        return;
    }
    /* coveringGrantKeys: grants at this tier or above, on the target or (a file) the folder holding it */
    strvec_t keys = {0};
    size_t *owner = NULL;
    size_t nowner = 0;
    for (size_t i = 0; i < targets.n; i++) {
        char *folder = covering_folder(targets.v[i]);
        for (int tier = d->tier; tier <= TIER_EXEC; tier++) {
            strvec_push_own(&keys, approval_grant_key((approval_tier_t)tier, targets.v[i]));
            owner = xrealloc(owner, (nowner + 2) * sizeof *owner);
            owner[nowner++] = i;
            if (folder && strcmp(folder, targets.v[i]) != 0) {
                strvec_push_own(&keys, approval_grant_key((approval_tier_t)tier, folder));
                owner[nowner++] = i;
            }
        }
        free(folder);
    }
    bool *present = xcalloc(keys.n + 1, sizeof *present);
    approval_grants_present(ctx->session_id, &keys, present);
    bool all = true;
    for (size_t i = 0; i < targets.n && all; i++) {
        bool any = false;
        for (size_t k = 0; k < keys.n && !any; k++) any = owner[k] == i && present[k];
        all = any;
    }
    if (all) out->policy = APOL_ALLOW;
    else {
        out->policy = APOL_PROMPT;
        out->reason = xstrdup(d->reason ? d->reason : "target not yet approved");
    }
    free(present);
    free(owner);
    strvec_free(&keys);
    strvec_free(&targets);
}

static void resolve_policy(const approval_call_t *c, const approval_ctx_t *ctx, approval_resolved_t *out) {
    memset(out, 0, sizeof *out);
    approval_decision_t d;
    approval_tool_decision(c, &d);
    const char *tool = c->tool ? c->tool : "";
    const char *policy_key = d.policy_key ? d.policy_key : tool;
    const json_t *uc = ctx->user_policies;
    approval_policy_t user = json_get(uc, policy_key) ? policy_parse(json_get(uc, policy_key)) : APOL_UNSET;
    approval_policy_t fallback = strcmp(policy_key, tool) != 0 && user == APOL_UNSET && json_get(uc, tool)
                                     ? policy_parse(json_get(uc, tool))
                                     : APOL_UNSET;
    approval_policy_t eff = user != APOL_UNSET ? user : fallback;
    const char *user_key = user != APOL_UNSET ? policy_key : tool;
    out->tier = d.tier;
    if (d.policy == APOL_DENY) {
        out->policy = APOL_DENY;
        out->override = d.override;
        out->source = ASRC_TOOL;
        out->policy_key = d.policy_key ? xstrdup(d.policy_key) : NULL;
        out->reason = d.reason ? xstrdup(d.reason) : NULL;
    } else if (eff == APOL_DENY) {
        out->policy = APOL_DENY;
        out->override = d.override;
        out->source = ASRC_USER;
        out->policy_key = xstrdup(user_key);
    } else if (ctx->mode == AMODE_YOLO) {
        if (d.policy != APOL_UNSET) {
            out->policy = d.policy;
            out->source = ASRC_TOOL;
            out->policy_key = d.policy_key ? xstrdup(d.policy_key) : NULL;
            out->reason = d.reason ? xstrdup(d.reason) : NULL;
        } else {
            out->policy = eff != APOL_UNSET ? eff : APOL_ALLOW;
            out->source = eff != APOL_UNSET ? ASRC_USER : ASRC_MODE;
            out->policy_key = eff != APOL_UNSET ? xstrdup(user_key) : NULL;
        }
    } else if (d.override) {
        out->policy = d.policy == APOL_ALLOW ? APOL_ALLOW : APOL_PROMPT;
        out->override = true;
        out->source = ASRC_TOOL;
        out->policy_key = d.policy_key ? xstrdup(d.policy_key) : NULL;
        out->reason = d.reason ? xstrdup(d.reason) : NULL;
    } else if (d.policy == APOL_ALLOW || d.policy == APOL_PROMPT) {
        out->policy = d.policy;
        out->source = ASRC_TOOL;
        out->policy_key = d.policy_key ? xstrdup(d.policy_key) : NULL;
        out->reason = d.reason ? xstrdup(d.reason) : NULL;
    } else if (eff != APOL_UNSET) {
        out->policy = eff;
        out->source = ASRC_USER;
        out->policy_key = xstrdup(user_key);
    } else if (ctx->mode == AMODE_ALWAYS_ASK_CACHED && ctx->use_grants) {
        cached_mode(c, ctx, &d, out);
    } else {
        approval_tier_t max = ctx->mode == AMODE_WRITE ? TIER_WRITE : TIER_READ;
        out->source = ASRC_MODE;
        if (d.tier <= max) out->policy = APOL_ALLOW;
        else {
            out->policy = APOL_PROMPT;
            out->policy_key = d.policy_key ? xstrdup(d.policy_key) : NULL;
            out->reason = d.reason ? xstrdup(d.reason) : NULL;
        }
    }
    approval_decision_free(&d);
}

/* isWithinFolder over normalized keys: `..name` is a child, only a `..` segment leaves the root */
static bool within(const char *root, const char *cand) {
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    if (strncmp(root, cand, n) != 0) return false;
    if (n == 1 && root[0] == '/') return true;
    return cand[n] == 0 || cand[n] == '/';
}

char *approval_restricted_reason(const approval_call_t *c, const json_t *restricted, const char *cwd) {
    if (!json_is_arr(restricted) || !json_len(restricted)) return NULL;
    char *base = cwd ? xstrdup(cwd) : get_cwd();
    strvec_t keys = {0}, labels = {0};
    json_t *e;
    json_arr_foreach(restricted, i, e) {
        if (!json_is_str(e)) continue;
        char *t = str_trim(json_sv(e));
        if (*t) {
            char *label = path_abs(t, base); /* expands ~, resolves against the call's directory */
            strvec_push_own(&keys, cache_key_for_path(label));
            strvec_push_own(&labels, label);
        }
        free(t);
    }
    char *reason = NULL;
    if (keys.n) {
        /* restrictionSubjects */
        strvec_t subj = {0};
        VEC(bool) rec = {0};
        strvec_t targets = {0};
        approval_cache_targets(c, &targets);
        if (targets.n) {
            for (size_t i = 0; i < targets.n; i++) {
                const char *tf = tree_folder_key(targets.v[i]);
                const char *s = tf ? tf : targets.v[i];
                if (s[0] != '/') continue;
                strvec_push_own(&subj, cache_key_for_path(s));
                vec_push(&rec, tf != NULL);
            }
        } else if (cwd) {
            char *wd = xstrdup(cwd);
            const char *req = json_get_str(c->args, "cwd");
            if (req && *req) {
                char *t = str_trim(req);
                if (*t && !strstr(t, "://")) {
                    free(wd);
                    wd = path_abs(t, cwd);
                }
                free(t);
            }
            strvec_push_own(&subj, cache_key_for_path(wd));
            vec_push(&rec, false);
            free(wd);
        }
        for (size_t s = 0; s < subj.n && !reason; s++)
            for (size_t k = 0; k < keys.n && !reason; k++)
                if (within(keys.v[k], subj.v[s]) || (rec.v[s] && within(subj.v[s], keys.v[k]))) {
                    char *sh = shorten_path(labels.v[k]), *san = sanitize(sh);
                    reason = xasprintf("Restricted folder: %s", san);
                    free(sh);
                    free(san);
                }
        strvec_free(&targets);
        strvec_free(&subj);
        vec_free(&rec);
    }
    strvec_free(&keys);
    strvec_free(&labels);
    free(base);
    return reason;
}

void approval_resolve(const approval_call_t *c, const approval_ctx_t *ctx, approval_resolved_t *out) {
    resolve_policy(c, ctx, out);
    if (out->policy == APOL_DENY) return;
    char *reason = approval_restricted_reason(c, ctx->restricted, ctx->cwd);
    if (!reason) return;
    out->policy = APOL_PROMPT;
    out->override = true;
    out->source = ASRC_RESTRICTION;
    free(out->reason);
    out->reason = reason;
}

bool approval_session_scope(const approval_resolved_t *r, const char *tool, char **subject, char **key) {
    if (r->policy != APOL_PROMPT || r->source != ASRC_MODE) return false;
    const char *s = r->policy_key ? r->policy_key : tool;
    if (subject) *subject = xstrdup(s);
    if (key) *key = xasprintf("%s" SEP "%s", s, TIER_NAMES[r->tier]);
    return true;
}

char *approval_deny_error(const approval_resolved_t *r, const char *tool) {
    if (r->source == ASRC_TOOL)
        return r->reason ? xasprintf("Tool \"%s\" is blocked by tool policy.\nReason: %s", tool, r->reason)
                         : xasprintf("Tool \"%s\" is blocked by tool policy.", tool);
    const char *k = r->policy_key ? r->policy_key : tool;
    return xasprintf("Tool \"%s\" is blocked by user policy.\nTo allow: remove \"tools.approval.%s: deny\" from config.", k, k);
}

/* ---------- prompt ---------- */

static void approval_header(const approval_call_t *c, const char *reason, strvec_t *out) {
    strvec_push_own(out, xasprintf("Allow tool: %s", c->tool));
    /* tool.approval === undefined: every MCP tool omc mounts declares "write" */
    if (reason) strvec_push_own(out, xasprintf("Reason: %s", reason));
}

char *approval_format_prompt(const approval_call_t *c, const char *reason) {
    strvec_t rows = {0};
    approval_header(c, reason, &rows);
    approval_tool_details(c, &rows);
    char *r = strvec_join(&rows, "\n");
    strvec_free(&rows);
    return r;
}

/* approvalSummary: each detail's first line joined by ` · `, `…` when any has more lines */
static char *approval_summary(const strvec_t *details) {
    if (!details->n) return NULL;
    bool omitted = false;
    strvec_t heads = {0};
    for (size_t i = 0; i < details->n; i++) {
        strvec_t lines = str_split(details->v[i], "\n");
        char *head = xstrdup(lines.v[0]);
        size_t hl = strlen(head);
        while (hl && isspace((unsigned char)head[hl - 1])) head[--hl] = 0;
        bool label_only = hl && head[hl - 1] == ':' && lines.n > 1;
        size_t shown = lines.n > 3 ? 3 : lines.n;
        if (shown > (label_only ? 2u : 1u)) omitted = true;
        if (label_only) {
            char *t = str_trim(lines.v[1]);
            strvec_push_own(&heads, xasprintf("%s %s", head, t));
            free(t);
            free(head);
        } else {
            strvec_push_own(&heads, head);
        }
        strvec_free(&lines);
    }
    char *row = strvec_join(&heads, " · ");
    strvec_free(&heads);
    if (!omitted) return row;
    char *r = xasprintf("%s …", row);
    free(row);
    return r;
}

typedef struct {
    strvec_t entries; /* sorted, folders with a trailing slash */
    size_t folders, total;
    bool truncated, ok;
} folder_listing_t;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* approvalFolderListing: one level, at most FOLDER_SCAN_LIMIT entries scanned */
static void folder_listing(const char *dir, folder_listing_t *l) {
    memset(l, 0, sizeof *l);
    DIR *d = opendir(dir);
    if (!d) return;
    l->ok = true;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (l->entries.n >= FOLDER_SCAN_LIMIT) {
            l->truncated = true;
            break;
        }
        bool isdir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) {
            char *p = path_join(dir, de->d_name);
            struct stat st;
            isdir = lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
            free(p);
        }
        if (isdir) l->folders++;
        l->total++;
        strvec_push_own(&l->entries, isdir ? xasprintf("%s/", de->d_name) : xstrdup(de->d_name));
    }
    closedir(d);
    xqsort(l->entries.v, l->entries.n, sizeof *l->entries.v, cmp_str);
}

static char *count_str(size_t n, const char *one, const char *many) { return xasprintf("%zu %s", n, n == 1 ? one : many); }

/* approval-folder.md */
static char *folder_block(const char *folder, const folder_listing_t *l, size_t limit) {
    if (!folder) return NULL;
    sb_t b = SB_INIT;
    char *f = sanitize(folder);
    sb_appendf(&b, "Folder: %s", f);
    free(f);
    size_t shown = 0;
    if (l && l->ok) {
        if (l->truncated) sb_appendf(&b, " — %zu+ entries", l->total);
        else {
            char *e = count_str(l->total, "entry", "entries"), *fo = count_str(l->folders, "folder", "folders"),
                 *fi = count_str(l->total - l->folders, "file", "files");
            sb_appendf(&b, " — %s (%s, %s)", e, fo, fi);
            free(e);
            free(fo);
            free(fi);
        }
    }
    sb_putc(&b, '\n');
    if (l && l->ok) {
        for (size_t i = 0; i < l->entries.n && i < limit; i++, shown++) {
            char *s = sanitize(l->entries.v[i]);
            sb_appendf(&b, "  - %s\n", s);
            free(s);
        }
        size_t hidden = l->total - shown;
        if (hidden > 0) sb_appendf(&b, "  … %zu more\n", hidden);
    }
    while (b.len && isspace((unsigned char)b.s[b.len - 1])) sb_truncate(&b, b.len - 1);
    if (!b.len) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

/* approvalFolderCandidate: the one folder whose grant covers every target */
static bool folder_candidate(const approval_call_t *c, char **key, char **label) {
    strvec_t targets = {0};
    approval_cache_targets(c, &targets);
    char *k = NULL;
    bool ok = true;
    for (size_t i = 0; i < targets.n && ok; i++) {
        char *f = covering_folder(targets.v[i]);
        if (!f || (k && strcmp(f, k) != 0)) {
            ok = false;
            free(f);
        } else if (!k) k = f;
        else free(f);
    }
    strvec_free(&targets);
    if (!ok || !k) {
        free(k);
        return false;
    }
    char *trimmed = xstrdup(k);
    if (strcmp(trimmed, "/") != 0) {
        size_t n = strlen(trimmed);
        while (n > 1 && trimmed[n - 1] == '/') trimmed[--n] = 0;
    }
    char *sh = shorten_path(trimmed);
    *label = sanitize(sh);
    free(sh);
    free(trimmed);
    *key = k;
    return true;
}

void approval_prompt_build(const approval_call_t *c, const char *reason, const char *session_subject, bool grants,
                           approval_prompt_t *out) {
    memset(out, 0, sizeof *out);
    strvec_t header = {0}, details = {0};
    approval_header(c, reason, &header);
    approval_tool_details(c, &details);
    if (session_subject) out->session = xasprintf("Approve %s for this session", session_subject);
    char *flabel = NULL;
    if (grants && folder_candidate(c, &out->folder_key, &flabel)) {
        approval_grant_choice_t *g = &out->grants[out->ngrants++];
        g->folder = true;
        g->label = xasprintf("Approve Directory: %s", flabel);
        g->always_label = xasprintf("Always Approve Directory: %s", flabel);
    }
    if (grants) {
        strvec_t targets = {0};
        approval_cache_targets(c, &targets);
        bool other = false, files = true;
        for (size_t i = 0; i < targets.n; i++) {
            if (!out->folder_key || strcmp(targets.v[i], out->folder_key) != 0) other = true;
            if (!target_names_file(targets.v[i])) files = false;
        }
        if (other) {
            approval_grant_choice_t *g = &out->grants[out->ngrants++];
            g->label = xstrdup(files ? "Approve File" : "Approve");
            g->always_label = xstrdup(files ? "Always Approve File" : "Always Approve");
        }
        strvec_free(&targets);
    }
    folder_listing_t l = {0};
    if (out->folder_key) folder_listing(out->folder_key, &l);
    char *full_block = folder_block(flabel, out->folder_key ? &l : NULL, FOLDER_FULL_LISTING);
    char *compact_block = folder_block(flabel, out->folder_key ? &l : NULL, FOLDER_COMPACT_LISTING);
    strvec_t full = {0}, compact = {0};
    for (size_t i = 0; i < header.n; i++) {
        strvec_push(&full, header.v[i]);
        strvec_push(&compact, header.v[i]);
    }
    for (size_t i = 0; i < details.n; i++) strvec_push(&full, details.v[i]);
    if (full_block) strvec_push(&full, full_block);
    char *summary = approval_summary(&details);
    if (summary) strvec_push_own(&compact, summary);
    if (compact_block) strvec_push(&compact, compact_block);
    out->full = strvec_join(&full, "\n");
    out->compact = strvec_join(&compact, "\n");
    strvec_free(&full);
    strvec_free(&compact);
    free(full_block);
    free(compact_block);
    strvec_free(&l.entries);
    free(flabel);
    strvec_free(&header);
    strvec_free(&details);
}

void approval_prompt_free(approval_prompt_t *p) {
    free(p->full);
    free(p->compact);
    free(p->session);
    for (size_t i = 0; i < p->ngrants; i++) {
        free(p->grants[i].label);
        free(p->grants[i].always_label);
    }
    free(p->folder_key);
    memset(p, 0, sizeof *p);
}

void approval_record_grant(const approval_call_t *c, const char *session_id, approval_tier_t tier, bool folder,
                           bool always) {
    if (folder) {
        char *key = NULL, *label = NULL;
        if (folder_candidate(c, &key, &label)) {
            char *gk = approval_grant_key(tier, key);
            approval_grants_record(session_id, gk, c->tool, always);
            free(gk);
            free(key);
            free(label);
            return;
        }
    }
    strvec_t targets = {0};
    approval_cache_targets(c, &targets);
    for (size_t i = 0; i < targets.n; i++) {
        char *gk = approval_grant_key(tier, targets.v[i]);
        approval_grants_record(session_id, gk, c->tool, always);
        free(gk);
        const char *tf = tree_folder_key(targets.v[i]);
        if (tf) {
            gk = approval_grant_key(tier, tf);
            approval_grants_record(session_id, gk, c->tool, always);
            free(gk);
        }
    }
    strvec_free(&targets);
}

static const char *once_label(const approval_prompt_t *p) { return p->ngrants ? "Approve Once" : "Approve"; }

void approval_choice_labels(const approval_prompt_t *p, strvec_t *out) {
    for (size_t i = 0; i < p->ngrants; i++) strvec_push(out, p->grants[i].label);
    for (size_t i = 0; i < p->ngrants; i++) strvec_push(out, p->grants[i].always_label);
    strvec_push(out, once_label(p));
    if (p->session) strvec_push(out, p->session);
    strvec_push(out, "Deny");
}

approval_answer_t approval_answer_for_choice(const approval_prompt_t *p, const char *choice) {
    approval_answer_t a = {APPROVE_DENY, 0, false};
    if (!choice) return a;
    if (!strcmp(choice, once_label(p))) a.kind = APPROVE_ONCE;
    else if (p->session && !strcmp(choice, p->session)) a.kind = APPROVE_SESSION;
    else
        for (size_t i = 0; i < p->ngrants; i++) {
            if (!strcmp(choice, p->grants[i].label)) return (approval_answer_t){APPROVE_GRANT, (int)i, false};
            if (!strcmp(choice, p->grants[i].always_label)) return (approval_answer_t){APPROVE_GRANT, (int)i, true};
        }
    return a;
}

/* ---------- ACP permission gate ---------- */

bool approval_acp_tool_gated(const char *tool) {
    return str_eq(tool, "bash") || str_eq(tool, "edit") || str_eq(tool, "delete") || str_eq(tool, "move");
}

bool approval_acp_intent(tool_env_t *env, const char *tool, const json_t *args, char **title, char **cache_key,
                         strvec_t *paths) {
    *title = *cache_key = NULL;
    if (str_eq(tool, "bash")) {
        const char *cmd = json_get_str(args, "command");
        if (cmd && *cmd) {
            /* .slice(0, 80): UTF-16 units approximated by code points */
            size_t i = 0, n = 0, len = strlen(cmd);
            while (i < len && n < 80) {
                uint32_t cp;
                i += (size_t)utf8_decode(cmd + i, len - i, &cp);
                n++;
            }
            *title = xstrndup(cmd, i);
        } else {
            *title = xstrdup(tool);
        }
        *cache_key = xstrdup(tool);
        return true;
    }
    if (str_eq(tool, "delete")) {
        const char *p = json_get_str(args, "path");
        *title = p ? xasprintf("Delete %s", p) : xstrdup(tool);
        if (p) strvec_push(paths, p);
        *cache_key = xstrdup(tool);
        return true;
    }
    if (str_eq(tool, "move")) {
        const char *from = json_get_str(args, "oldPath");
        if (!from) from = json_get_str(args, "path");
        if (!from) from = json_get_str(args, "from");
        const char *to = json_get_str(args, "newPath");
        if (!to) to = json_get_str(args, "to");
        if (!to) to = json_get_str(args, "destination");
        if (from && to) {
            *title = xasprintf("Move %s to %s", from, to);
            strvec_push(paths, from);
            strvec_push(paths, to);
        } else {
            *title = from ? xasprintf("Move %s", from) : xstrdup(tool);
            if (from) strvec_push(paths, from);
        }
        *cache_key = xstrdup(tool);
        return true;
    }
    if (str_eq(tool, "edit")) {
        /* getEditDestructiveIntent: the first delete (else move) file op of a patch / hashline / apply_patch payload */
        approval_call_t c = {env, NULL, tool, args};
        const char *modes[3] = {NULL, NULL, NULL};
        if (json_is_arr(json_get(args, "edits"))) modes[0] = "patch";
        else {
            modes[0] = "hashline";
            modes[1] = "apply_patch";
        }
        for (size_t m = 0; m < 2 && modes[m]; m++) {
            const json_t *in = json_get(args, "input");
            /* deletes: patch `op: delete`, hashline REM sections, apply_patch `*** Delete File:` */
            char *del = NULL;
            if (!strcmp(modes[m], "patch")) {
                json_t *e;
                const char *p = json_get_str(args, "path");
                json_arr_foreach(json_get_arr(args, "edits"), i, e) if (p && !del && str_eq(json_get_str(e, "op"), "delete")) del = xstrdup(p);
            } else if (json_is_str(in) && !strcmp(modes[m], "hashline")) {
                char *err = NULL;
                hl_patch_t *hp = hl_patch_parse(json_sv(in), NULL, &err);
                free(err);
                for (size_t i = 0; hp && i < hl_patch_count(hp) && !del; i++) {
                    char *e2 = NULL;
                    bool re = false;
                    if (hl_section_parse(hp, i, &e2, &re) && hl_section_file_op(hp, i, NULL) == 1)
                        del = xstrdup(hl_section_path(hp, i));
                    free(e2);
                }
                if (hp) hl_patch_free(hp);
            } else if (json_is_str(in)) {
                strvec_t lines = str_split_lines(json_sv(in), strlen(json_sv(in)));
                for (size_t i = 0; i < lines.n && !del; i++) {
                    const char *l = lines.v[i];
                    while (isspace((unsigned char)*l)) l++;
                    if (str_starts(l, "*** Delete File:")) {
                        char *t = str_trim(l + 16);
                        if (*t) del = t;
                        else free(t);
                    }
                }
                strvec_free(&lines);
            }
            if (del) {
                *title = xasprintf("Delete %s", del);
                strvec_push_own(paths, del);
                *cache_key = xstrdup("edit:delete");
                return true;
            }
            edit_inspection_t x;
            edit_inspect_mode(&c, modes[m], &x);
            if (x.move_to.n) {
                *title = xasprintf("Move %s to %s", x.move_from.v[0], x.move_to.v[0]);
                strvec_push(paths, x.move_from.v[0]);
                strvec_push(paths, x.move_to.v[0]);
                *cache_key = xstrdup("edit:move");
                edit_inspection_free(&x);
                return true;
            }
            edit_inspection_free(&x);
        }
    }
    return false;
}
