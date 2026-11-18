/* ssh.c - ssh://host/path internal URLs: remote UTF-8 text files and directory listings over OpenSSH
 * (shared ControlMaster connection), host aliases from ssh.json files, byte-exact remote writes. */
#include "tools.h"

#include "core/fs.h"
#include "core/proc.h"
#include "net/http.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define SSH_TEXT_MAX (1024 * 1024)
#define SSH_DIR_MAX (1024 * 1024)
#define SSH_TIMEOUT_MS 30000

typedef struct {
    char *name, *host, *user, *key, *desc;
    int port;
    bool openssh; /* a ~/.ssh/config alias: host == name, nothing pinned */
} ssh_host_t;

typedef VEC(ssh_host_t) ssh_hosts_t;

static void hosts_free(ssh_hosts_t *h) {
    for (size_t i = 0; i < h->n; i++) {
        free(h->v[i].name);
        free(h->v[i].host);
        free(h->v[i].user);
        free(h->v[i].key);
        free(h->v[i].desc);
    }
    vec_free(h);
}

/* ${VAR} / $VAR expansion inside ssh.json string values */
static char *expand_env(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s; *p; p++) {
        if (*p == '$' && (p[1] == '{' || isalpha((unsigned char)p[1]) || p[1] == '_')) {
            bool br = p[1] == '{';
            const char *q = p + (br ? 2 : 1), *e = q;
            while (isalnum((unsigned char)*e) || *e == '_') e++;
            if (br && *e != '}') {
                sb_putc(&b, *p);
                continue;
            }
            char *name = xstrndup(q, (size_t)(e - q));
            const char *v = getenv(name);
            free(name);
            if (v) sb_append(&b, v);
            p = br ? e : e - 1;
        } else {
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

static char *jstr_exp(const json_t *o, const char *k) {
    const char *v = json_get_str(o, k);
    return v ? expand_env(v) : NULL;
}

static void load_file(ssh_hosts_t *out, const char *path) {
    char *txt = fs_read(path, NULL);
    if (!txt) return;
    json_t *j = json_parse(txt, strlen(txt), NULL);
    free(txt);
    json_t *hosts = json_get(j, "hosts");
    const char *name;
    json_t *h;
    json_obj_foreach(hosts, hi, name, h) {
        if (!json_is_obj(h) || !*name) continue;
        for (size_t i = 0; i < out->n; i++)
            if (!strcmp(out->v[i].name, name)) goto next; /* first source wins */
        char *host = jstr_exp(h, "host");
        if (!host) goto next;
        ssh_host_t e = {.name = xstrdup(name), .host = host, .user = jstr_exp(h, "username"), .desc = jstr_exp(h, "description")};
        json_t *port = json_get(h, "port");
        if (json_is_num(port)) e.port = (int)json_get_int(h, "port", 0);
        else if (json_get_str(h, "port")) e.port = atoi(json_get_str(h, "port"));
        char *key = jstr_exp(h, "keyPath");
        if (!key) key = jstr_exp(h, "key");
        if (key && key[0] == '~' && (key[1] == '/' || !key[1])) {
            char *k2 = xasprintf("%s%s", env_get("HOME") ? env_get("HOME") : "", key + 1);
            free(key);
            key = k2;
        }
        e.key = key;
        vec_push(out, e);
    next:;
    }
    json_free(j);
}

/* ---------- OpenSSH config aliases (discovery/ssh-config.ts, omp 2cbe2f5f46) ---------- */

#define SSH_INCLUDE_MAX_DEPTH 16 /* OpenSSH READCONF_MAX_DEPTH */

/* directive arguments, unquoted ("…", '…' or bare); an unquoted `#` token starts a trailing comment */
static void split_args(const char *rest, strvec_t *out) {
    const char *p = rest;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            const char *e = strchr(p, q);
            if (!e) { /* an unterminated quote: the regex skips the quote char and reads on */
                continue;
            }
            char *a = xstrndup(p, (size_t)(e - p));
            strvec_push_own(out, a);
            p = e + 1;
            continue;
        }
        const char *b = p;
        while (*p && !isspace((unsigned char)*p) && *p != '"' && *p != '\'') p++;
        if (*b == '#') break;
        strvec_push_own(out, xstrndup(b, (size_t)(p - b)));
    }
}

static void scan_ssh_config(const char *file, int depth, const char *base_dir, strvec_t *visited, strvec_t *pairs) {
    if (strvec_contains(visited, file)) return;
    strvec_push(visited, file);
    char *txt = fs_read(file, NULL);
    if (!txt) return;
    for (char *line = txt; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
        /* `Keyword args`, `Keyword=args`, `Keyword = args` */
        const char *p = line;
        while (isspace((unsigned char)*p)) p++;
        const char *kw = p;
        while (isalpha((unsigned char)*p)) p++;
        size_t kl = (size_t)(p - kw);
        bool ok = kl > 0 && (!*p || isspace((unsigned char)*p) || *p == '=');
        if (ok) {
            while (isspace((unsigned char)*p)) p++;
            if (*p == '=') p++;
            while (isspace((unsigned char)*p)) p++;
            strvec_t args = {0};
            if (kl == 4 && !strncasecmp(kw, "host", 4)) {
                split_args(p, &args);
                for (size_t i = 0; i < args.n; i++) {
                    const char *nm = args.v[i];
                    if (nm[0] == '!' || strpbrk(nm, "*?")) continue; /* negated / wildcard patterns name no host */
                    bool seen = false;
                    for (size_t k = 0; k + 1 < pairs->n && !seen; k += 2) seen = !strcmp(pairs->v[k], nm);
                    if (seen) continue;
                    strvec_push(pairs, nm);
                    strvec_push(pairs, file);
                }
            } else if (kl == 7 && !strncasecmp(kw, "include", 7)) {
                if (depth >= SSH_INCLUDE_MAX_DEPTH) {
                    LOGW("ssh-config: Include nested deeper than %d levels in %s", SSH_INCLUDE_MAX_DEPTH, file);
                } else {
                    split_args(p, &args);
                    for (size_t i = 0; i < args.n; i++) {
                        char *ex = path_expand_home(args.v[i]);
                        char *full = ex[0] == '/' ? xstrdup(ex) : path_join(base_dir, ex);
                        free(ex);
                        if (!strpbrk(full, "*?[")) scan_ssh_config(full, depth + 1, base_dir, visited, pairs);
                        else {
                            glob_t g;
                            if (glob(full, 0, NULL, &g) == 0)
                                for (size_t k = 0; k < g.gl_pathc; k++) scan_ssh_config(g.gl_pathv[k], depth + 1, base_dir, visited, pairs);
                            globfree(&g);
                        }
                        free(full);
                    }
                }
            }
            strvec_free(&args);
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(txt);
}

/* the concrete `Host` aliases of ~/.ssh/config then /etc/ssh/ssh_config (following Include), first definition wins:
 * name/file pairs */
void ssh_openssh_aliases(strvec_t *pairs) {
    const char *home = home_dir();
    char *udir = path_join(home ? home : "", ".ssh"), *ucfg = path_join(udir, "config");
    strvec_t visited = {0};
    scan_ssh_config(ucfg, 0, udir, &visited, pairs);
    scan_ssh_config("/etc/ssh/ssh_config", 0, "/etc/ssh", &visited, pairs);
    strvec_free(&visited);
    free(ucfg);
    free(udir);
}

static void load_hosts(const char *cwd, ssh_hosts_t *out) {
    memset(out, 0, sizeof(*out));
    char *paths[6];
    size_t n = 0;
    paths[n++] = path_join3(cwd, ".omc", "ssh.json");
    paths[n++] = path_join3(cwd, ".omp", "ssh.json");
    paths[n++] = path_join(omc_agent_dir(), "ssh.json");
    paths[n++] = path_join(cwd, "ssh.json");
    paths[n++] = path_join(cwd, ".ssh.json");
    for (size_t i = 0; i < n; i++) {
        load_file(out, paths[i]);
        free(paths[i]);
    }
    /* priority 3, below ssh.json: an explicit entry wins on a name clash */
    strvec_t pairs = {0};
    ssh_openssh_aliases(&pairs);
    for (size_t i = 0; i + 1 < pairs.n; i += 2) {
        bool dup = false;
        for (size_t k = 0; k < out->n && !dup; k++) dup = !strcmp(out->v[k].name, pairs.v[i]);
        if (dup) continue;
        ssh_host_t e = {.name = xstrdup(pairs.v[i]), .host = xstrdup(pairs.v[i]), .openssh = true};
        vec_push(out, e);
    }
    strvec_free(&pairs);
}

/* ---------- ssh.json config files (ssh/config-writer.ts) ---------- */

char *ssh_config_path(const char *scope, const char *cwd) {
    if (str_eq(scope, "user")) return path_join(omc_agent_dir(), "ssh.json");
    return path_join3(cwd, ".omc", "ssh.json");
}

json_t *ssh_config_read(const char *path, char **err) {
    *err = NULL;
    size_t n;
    char *s = fs_read(path, &n);
    if (!s) {
        json_t *c = json_obj();
        json_set(c, "hosts", json_obj());
        return c;
    }
    char *perr = NULL;
    json_t *j = json_parse(s, n, &perr);
    free(s);
    if (!j) {
        *err = xasprintf("Failed to parse SSH config file %s: %s", path, perr ? perr : "invalid JSON");
        free(perr);
        return NULL;
    }
    free(perr);
    return j;
}

/* writeSSHConfigFile: JSON.stringify(config, null, 2), mode 0600, directory 0700 */
static int ssh_config_write(const char *path, const json_t *cfg, char **err) {
    char *dir = path_dirname(path);
    fs_mkdir_p(dir, 0700);
    free(dir);
    char *s = json_dump(cfg, 2);
    int rc = fs_write_atomic(path, s, strlen(s), 0600);
    free(s);
    if (rc) {
        *err = xasprintf("%s: %s", path, strerror(rc < 0 ? -rc : rc));
        return -1;
    }
    return 0;
}

const char *ssh_validate_host_name(const char *name) {
    if (!name || !*name) return "Host name cannot be empty";
    if (strlen(name) > 100) return "Host name is too long (max 100 characters)";
    for (const char *c = name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '.' || *c == '-'))
            return "Host name can only contain letters, numbers, dash, underscore, and dot";
    return NULL;
}

int ssh_config_add(const char *path, const char *name, const json_t *host, char **err) {
    *err = NULL;
    const char *ne = ssh_validate_host_name(name);
    if (ne) {
        *err = xstrdup(ne);
        return -1;
    }
    if (!json_get_str(host, "host") || !*json_get_str(host, "host")) {
        *err = xstrdup("Host address cannot be empty");
        return -1;
    }
    json_t *cfg = ssh_config_read(path, err);
    if (!cfg) return -1;
    if (!json_is_obj(json_get(cfg, "hosts"))) json_set(cfg, "hosts", json_obj());
    int rc = -1;
    if (json_get(json_get(cfg, "hosts"), name)) *err = xasprintf("Host \"%s\" already exists in %s", name, path);
    else {
        json_set(json_get(cfg, "hosts"), name, json_clone(host));
        rc = ssh_config_write(path, cfg, err);
    }
    json_free(cfg);
    return rc;
}

int ssh_config_remove(const char *path, const char *name, char **err) {
    *err = NULL;
    json_t *cfg = ssh_config_read(path, err);
    if (!cfg) return -1;
    int rc = -1;
    if (!json_get(json_get_obj(cfg, "hosts"), name)) *err = xasprintf("Host \"%s\" not found in %s", name, path);
    else {
        json_del(json_get(cfg, "hosts"), name);
        rc = ssh_config_write(path, cfg, err);
    }
    json_free(cfg);
    return rc;
}

char *ssh_quote(const char *s) {
    sb_t b = SB_INIT;
    sb_putc(&b, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') sb_append(&b, "'\\''");
        else sb_putc(&b, *p);
    }
    sb_putc(&b, '\'');
    return sb_detach(&b);
}

/* strict percent-decoding: NULL on a malformed escape */
static char *pct_decode(const char *s, size_t n) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%') {
            if (i + 2 >= n) goto bad;
            if (!isxdigit((unsigned char)s[i + 1]) || !isxdigit((unsigned char)s[i + 2])) goto bad;
            char h[3] = {s[i + 1], s[i + 2], 0};
            sb_putc(&b, (char)strtol(h, NULL, 16));
            i += 2;
        } else {
            sb_putc(&b, s[i]);
        }
    }
    if (memchr(b.s ? b.s : "", 0, b.len)) goto bad;
    return sb_detach(&b);
bad:
    sb_free(&b);
    return NULL;
}

typedef struct {
    char *name; /* cache key */
    char *host;
    char *user;
    char *key;
    int port;
} target_t;

static void target_free(target_t *t) {
    free(t->name);
    free(t->host);
    free(t->user);
    free(t->key);
}

/* parse ssh://[user@]host[:port]/path -> target + raw path (still encoded). */
static int parse_url(const char *cwd, const char *url, target_t *t, char **raw_path, char **err) {
    memset(t, 0, sizeof(*t));
    *raw_path = NULL;
    const char *a = url + 6;
    if (strpbrk(a, "?#")) {
        *err = xasprintf(strchr(a, '?') && (!strchr(a, '#') || strchr(a, '?') < strchr(a, '#'))
                             ? "ssh:// does not support URL query strings; percent-encode a literal '?' as %%3F in the path: %s"
                             : "ssh:// does not support URL fragments; percent-encode a literal '#' as %%23 in the path: %s",
                         url);
        return -1;
    }
    const char *slash = strchr(a, '/');
    size_t alen = slash ? (size_t)(slash - a) : strlen(a);
    char *auth = xstrndup(a, alen);
    *raw_path = xstrdup(slash ? slash : "");
    if (!*auth) {
        free(auth);
        return 1; /* host index */
    }
    char *user = NULL, *hostport = auth;
    char *at = strrchr(auth, '@');
    if (at) {
        *at = 0;
        if (!*auth) {
            *err = xasprintf("ssh://: empty username in \"%s\"; drop the leading '@' or provide a username before it", url);
            free(auth);
            return -1;
        }
        if (strchr(auth, ':')) {
            *err = xstrdup("ssh://: password authentication is not supported; ssh:// uses key/agent auth — drop the ':<password>' from the URL");
            free(auth);
            return -1;
        }
        user = auth;
        hostport = at + 1;
    }
    int port = 0;
    bool has_port = false;
    char *host;
    if (hostport[0] == '[') {
        char *rb = strchr(hostport, ']');
        if (!rb) goto badauth;
        host = xstrndup(hostport + 1, (size_t)(rb - hostport - 1));
        if (rb[1] == ':') {
            has_port = true;
            hostport = rb + 2;
        } else if (rb[1]) {
            free(host);
            goto badauth;
        } else {
            hostport = NULL;
        }
    } else {
        char *colon = strchr(hostport, ':');
        host = xstrndup(hostport, colon ? (size_t)(colon - hostport) : strlen(hostport));
        if (colon) {
            has_port = true;
            hostport = colon + 1;
        } else {
            hostport = NULL;
        }
    }
    if (has_port) {
        if (!*hostport) {
            *err = xasprintf("ssh://: empty port in \"%s\"; use ssh://host:<1-65535>/<path> or drop the colon", url);
            free(host);
            free(auth);
            return -1;
        }
        char *end;
        long p = strtol(hostport, &end, 10);
        if (*end || p < 0 || p > 65535) {
            free(host);
            goto badauth;
        }
        if (p == 0) {
            *err = xstrdup("ssh://: port 0 is not a valid SSH port; use ssh://host:<1-65535>/<path> or omit the port");
            free(host);
            free(auth);
            return -1;
        }
        port = (int)p;
    }
    char *dhost = pct_decode(host, strlen(host));
    char *duser = user ? pct_decode(user, strlen(user)) : NULL;
    free(host);
    if (!dhost || (user && !duser) || !*dhost) {
        free(dhost);
        free(duser);
        *err = xasprintf("ssh://: invalid percent-escape in authority \"%s\"", url);
        free(auth);
        return -1;
    }
    if (dhost[0] == '-' || (duser && duser[0] == '-')) {
        *err = xasprintf("Invalid SSH %s \"%s\": an SSH %s must not begin with \"-\" (argument-injection guard)",
                         dhost[0] == '-' ? "host" : "username", dhost[0] == '-' ? dhost : duser,
                         dhost[0] == '-' ? "destination" : "username");
        free(dhost);
        free(duser);
        free(auth);
        return -1;
    }
    ssh_hosts_t hosts;
    load_hosts(cwd, &hosts);
    ssh_host_t *match = NULL;
    for (size_t i = 0; i < hosts.n; i++)
        if (!strcmp(hosts.v[i].name, dhost)) match = &hosts.v[i];
    /* an override is ambiguous only on an entry that pins its destination; a ~/.ssh/config alias (host == name,
     * nothing pinned) takes it the way `ssh -p 2222 root@alias` does */
    bool pinned = match && !(!strcmp(match->host, match->name) && !match->user && !match->port && !match->key);
    if ((duser || port) && pinned) {
        *err = xasprintf("ssh://: user/port overrides are not allowed for the configured host \"%s\"; use ssh://%s/<path> or an "
                         "unconfigured hostname",
                         dhost, dhost);
        free(dhost);
        free(duser);
        free(auth);
        hosts_free(&hosts);
        return -1;
    }
    if (match && !((duser || port) && !pinned)) {
        t->name = xstrdup(match->name);
        t->host = xstrdup(match->host);
        t->user = match->user ? xstrdup(match->user) : NULL;
        t->key = match->key ? xstrdup(match->key) : NULL;
        t->port = match->port;
        free(dhost);
        free(duser);
    } else {
        t->name = port ? xasprintf("%s%s%s:%d", duser ? duser : "", duser ? "@" : "", dhost, port)
                       : xasprintf("%s%s%s", duser ? duser : "", duser ? "@" : "", dhost);
        t->host = dhost;
        t->user = duser;
        t->port = port;
    }
    if (t->host[0] == '-' || (t->user && t->user[0] == '-')) {
        *err = xasprintf("Invalid SSH host \"%s\": an SSH destination must not begin with \"-\" (argument-injection guard)", t->host);
        hosts_free(&hosts);
        free(auth);
        return -1;
    }
    hosts_free(&hosts);
    free(auth);
    return 0;
badauth:
    *err = xasprintf("ssh://: invalid host or port in \"%s\"; use ssh://host[:1-65535]/<absolute-path>", url);
    free(auth);
    return -1;
}

static const char *control_dir(void) {
    static char dir[128];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!dir[0]) {
        snprintf(dir, sizeof dir, "/tmp/omc-ssh-%u", (unsigned)getuid());
        mkdir(dir, 0700);
        struct stat st;
        if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) dir[0] = 0;
    }
    pthread_mutex_unlock(&mu);
    return dir;
}

/* argv for `ssh <opts> <dest> <remote command>` (owned strings in *keep) */
static char **build_argv(const target_t *t, const char *remote, bool allow_stdin, strvec_t *keep) {
    strvec_push(keep, "ssh");
    if (!allow_stdin) strvec_push(keep, "-n");
    const char *cd = control_dir();
    if (*cd) {
        strvec_push(keep, "-o");
        strvec_push(keep, "ControlMaster=auto");
        char *cp = xasprintf("ControlPath=%s/%%C", cd);
        strvec_push(keep, "-o");
        strvec_push(keep, cp);
        free(cp);
        strvec_push(keep, "-o");
        strvec_push(keep, "ControlPersist=3600");
    }
    strvec_push(keep, "-o");
    strvec_push(keep, "BatchMode=yes");
    strvec_push(keep, "-o");
    strvec_push(keep, "StrictHostKeyChecking=accept-new");
    if (t->port) {
        char *p = xasprintf("%d", t->port);
        strvec_push(keep, "-p");
        strvec_push(keep, p);
        free(p);
    }
    if (t->key) {
        strvec_push(keep, "-i");
        strvec_push(keep, t->key);
    }
    char *dest = t->user ? xasprintf("%s@%s", t->user, t->host) : xstrdup(t->host);
    strvec_push(keep, dest);
    free(dest);
    strvec_push(keep, remote);
    vec_push(keep, NULL); /* argv terminator; strvec_free tolerates it */
    return keep->v;
}

static int run_ssh(const target_t *t, const char *remote, const char *in, size_t in_len, proc_result_t *r, char **err) {
    char *bin = which("ssh");
    if (!bin) {
        *err = xstrdup("ssh binary not found on PATH");
        return -1;
    }
    free(bin);
    strvec_t keep = {0};
    char **argv = build_argv(t, remote, in != NULL, &keep);
    proc_opts_t o = {.timeout_ms = SSH_TIMEOUT_MS, .stdin_data = in, .stdin_len = in_len, .new_session = true,
                     .max_output = SSH_DIR_MAX + 1};
    int rc = proc_run((const char *const *)argv, &o, r);
    strvec_free(&keep);
    if (rc != 0 || r->spawn_errno) {
        *err = xasprintf("ssh: failed to start: %s", strerror(r->spawn_errno ? r->spawn_errno : errno));
        return -1;
    }
    if (r->timed_out) {
        *err = xasprintf("ssh://: %s timed out after %ds", t->name, SSH_TIMEOUT_MS / 1000);
        return -1;
    }
    if (r->exit_code == 255) {
        /* ssh itself failed to connect: omp reports its control-master start (`ssh -M -N -f`) */
        char *e = str_trim(r->err ? r->err : "");
        *err = xasprintf("Failed to start SSH master for %s%s%s%s%s", t->user ? t->user : "", t->user ? "@" : "", t->host,
                         *e ? ": " : "", e);
        free(e);
        return -1;
    }
    if (r->exit_code != 0) {
        /* omp ptree NonZeroExitError */
        *err = xasprintf("Process exited with code %d:\n%s", r->exit_code, r->err ? r->err : "");
        return -1;
    }
    return 0;
}

/* ---------- host probe cache ---------- */

typedef struct {
    char *key;
    char shell[8];
    bool windows;
} hostinfo_t;
static VEC(hostinfo_t) g_info;
static pthread_mutex_t g_info_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *posix_shell(const target_t *t, char **err) {
    char *key = xasprintf("%s\x1f%s\x1f%s\x1f%d\x1f%s", t->name, t->host, t->user ? t->user : "", t->port, t->key ? t->key : "");
    pthread_mutex_lock(&g_info_mu);
    for (size_t i = 0; i < g_info.n; i++)
        if (!strcmp(g_info.v[i].key, key)) {
            hostinfo_t *h = &g_info.v[i];
            pthread_mutex_unlock(&g_info_mu);
            free(key);
            if (h->windows) {
                *err = xasprintf("ssh://: %s is a Windows host; ssh:// supports POSIX remotes only (head/cat/mv) — use `bash` with a "
                                 "remote SSH command for Windows hosts",
                                 t->name);
                return NULL;
            }
            return h->shell;
        }
    pthread_mutex_unlock(&g_info_mu);
    hostinfo_t h = {.key = key};
    const char *cands[] = {"sh", "bash", "zsh"};
    char *first_err = NULL;
    for (size_t i = 0; i < ARRAY_LEN(cands) && !h.shell[0]; i++) {
        char *remote = xasprintf("%s -lc 'printf \"OMC_SSH_PROBE\"; uname -s 2>/dev/null || true'", cands[i]);
        proc_result_t r = {0};
        char *e = NULL;
        if (run_ssh(t, remote, NULL, 0, &r, &e) == 0 && r.out && str_starts(r.out, "OMC_SSH_PROBE")) {
            snprintf(h.shell, sizeof h.shell, "%s", cands[i]);
            const char *un = r.out + 13;
            h.windows = strstr(un, "MINGW") || strstr(un, "MSYS") || strstr(un, "CYGWIN") || strstr(un, "Windows");
        } else if (!first_err) {
            first_err = e;
            e = NULL;
        }
        free(e);
        free(remote);
        bool conn_fail = r.exit_code == 255;
        proc_result_free(&r);
        /* a connection failure (exit 255) will fail the same way for every candidate */
        if (!h.shell[0] && first_err && conn_fail) break;
    }
    if (!h.shell[0]) {
        *err = first_err ? first_err
                         : xasprintf("ssh://: %s has no verified POSIX shell for ssh:// read/write — none of sh/bash/zsh round-tripped "
                                     "a capability probe (use `bash` with a remote SSH command for this host)",
                                     t->name);
        free(key);
        return NULL;
    }
    free(first_err);
    pthread_mutex_lock(&g_info_mu);
    vec_push(&g_info, h);
    const char *sh = g_info.v[g_info.n - 1].shell;
    pthread_mutex_unlock(&g_info_mu);
    return h.windows ? (*err = xasprintf("ssh://: %s is a Windows host; ssh:// supports POSIX remotes only", t->name), NULL) : sh;
}

static int run_posix(const target_t *t, const char *cmd, const char *in, size_t in_len, proc_result_t *r, char **err) {
    const char *sh = posix_shell(t, err);
    if (!sh) return -1;
    char *q = ssh_quote(cmd);
    char *remote = xasprintf("%s -c %s", sh, q);
    free(q);
    int rc = run_ssh(t, remote, in, in_len, r, err);
    free(remote);
    return rc;
}

/* resolve the decoded remote absolute path (handles /~ HOME) */
static char *remote_path(const target_t *t, const char *raw, char **err) {
    bool home_rel = !strcmp(raw, "/~") || str_starts(raw, "/~/");
    char *dec = pct_decode(raw, strlen(raw));
    if (!dec) {
        *err = xstrdup("Invalid URL encoding in ssh:// path");
        return NULL;
    }
    if (!*dec) {
        free(dec);
        *err = xstrdup("ssh:// requires an absolute path, e.g. ssh://host/etc/hosts or ssh://host/ for the root directory");
        return NULL;
    }
    if (home_rel) {
        proc_result_t r = {0};
        if (run_posix(t, "printf '%s' \"$HOME\"", NULL, 0, &r, err) != 0) {
            proc_result_free(&r);
            free(dec);
            return NULL;
        }
        const char *home = r.out ? r.out : "";
        if (home[0] != '/') {
            *err = xasprintf("ssh://: %s did not return an absolute remote HOME", t->name);
            proc_result_free(&r);
            free(dec);
            return NULL;
        }
        size_t hl = strlen(home);
        while (hl > 1 && home[hl - 1] == '/') hl--;
        char *p = xasprintf("%.*s%s", (int)hl, home, dec + 2);
        if (!*p) {
            free(p);
            p = xstrdup("/");
        }
        proc_result_free(&r);
        free(dec);
        return p;
    }
    if (str_starts(raw, "/~")) {
        free(dec);
        *err = xstrdup("ssh://: named-user /~user paths are unsupported; use /~/ for the connected account's HOME");
        return NULL;
    }
    return dec;
}

static char *host_index(const char *cwd) {
    ssh_hosts_t h;
    load_hosts(cwd, &h);
    sb_t b = SB_INIT;
    if (!h.n) {
        sb_append(&b, "# SSH hosts\n\nNo SSH hosts are configured. Add hosts to an `ssh.json` capability file or as `Host` "
                      "entries in `~/.ssh/config`, or read `ssh://<host>/<path>` with any destination OpenSSH can resolve.\n");
    } else {
        sb_appendf(&b, "# SSH hosts\n\n%zu configured host%s:\n\n", h.n, h.n == 1 ? "" : "s");
        for (size_t i = 0; i < h.n; i++) {
            ssh_host_t *e = &h.v[i];
            char *addr = e->port ? xasprintf("%s%s%s:%d", e->user ? e->user : "", e->user ? "@" : "", e->host, e->port)
                                 : xasprintf("%s%s%s", e->user ? e->user : "", e->user ? "@" : "", e->host);
            char *enc = url_encode(e->name);
            sb_appendf(&b, "- [%s](ssh://%s/)", e->name, enc);
            if (strcmp(addr, e->name)) sb_appendf(&b, " — `%s`", addr);
            if (e->desc) sb_appendf(&b, " (%s)", e->desc);
            sb_putc(&b, '\n');
            free(enc);
            free(addr);
        }
    }
    hosts_free(&h);
    return sb_detach(&b);
}

typedef struct {
    char *name;
    bool dir;
} dent_t;

static int dent_cmp(const void *a, const void *b) {
    const dent_t *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcmp(x->name, y->name);
}

static char *list_dir(const target_t *t, const char *path, const char *shell, char **err) {
    const char *classify = "for p do if [ -L \"$p\" ]; then t=symlink; elif [ -d \"$p\" ]; then t=dir; elif [ -f \"$p\" ]; then t=file; "
                           "elif [ -p \"$p\" ]; then t=fifo; elif [ -S \"$p\" ]; then t=socket; elif [ -c \"$p\" ]; then t=char; "
                           "elif [ -b \"$p\" ]; then t=block; else continue; fi; printf '%s\\000%s\\000' \"$t\" \"${p#./}\"; done";
    char *qp = ssh_quote(path), *qc = ssh_quote(classify);
    char *cmd = xasprintf("cd %s && LC_ALL=C find . ! -name . -prune -exec %s -c %s %s {} +", qp, shell, qc, shell);
    free(qp);
    free(qc);
    proc_result_t r = {0};
    int rc = run_posix(t, cmd, NULL, 0, &r, err);
    free(cmd);
    if (rc != 0) {
        proc_result_free(&r);
        return NULL;
    }
    if (r.out_len > SSH_DIR_MAX) {
        proc_result_free(&r);
        *err = xstrdup("ssh://: directory listing exceeds the 1 MiB limit; use a narrower directory scope");
        return NULL;
    }
    VEC(dent_t) ents = {0};
    size_t i = 0;
    bool bad = false;
    while (i < r.out_len) {
        const char *ty = r.out + i;
        size_t tl = strnlen(ty, r.out_len - i);
        if (i + tl >= r.out_len) {
            bad = true;
            break;
        }
        i += tl + 1;
        const char *nm = r.out + i;
        size_t nl = strnlen(nm, r.out_len - i);
        if (i + nl >= r.out_len) {
            bad = true;
            break;
        }
        i += nl + 1;
        if (!nl || memchr(nm, '/', nl) || !strcmp(nm, ".") || !strcmp(nm, "..")) {
            bad = true;
            break;
        }
        dent_t d = {xstrndup(nm, nl), !strcmp(ty, "dir")};
        vec_push(&ents, d);
    }
    proc_result_free(&r);
    char *res = NULL;
    if (bad) {
        *err = xstrdup("ssh://: malformed directory listing (expected NUL-delimited type/name pairs)");
    } else if (!ents.n) {
        res = xstrdup("(empty directory)");
    } else {
        xqsort(ents.v, ents.n, sizeof(dent_t), dent_cmp);
        sb_t b = SB_INIT;
        for (size_t k = 0; k < ents.n; k++) sb_appendf(&b, "%s%s%s", k ? "\n" : "", ents.v[k].name, ents.v[k].dir ? "/" : "");
        res = sb_detach(&b);
    }
    for (size_t k = 0; k < ents.n; k++) free(ents.v[k].name);
    vec_free(&ents);
    return res;
}

#define SSH_REMOTE_MISSING_EXIT 66 /* sysexits EX_NOINPUT */

int ssh_fetch(tool_env_t *env, const char *url, char **content, size_t *len, bool *is_dir, char **err) {
    return ssh_fetch_ex(env, url, content, len, is_dir, NULL, err);
}

int ssh_fetch_ex(tool_env_t *env, const char *url, char **content, size_t *len, bool *is_dir, bool *missing, char **err) {
    if (missing) *missing = false;
    *content = NULL;
    *len = 0;
    *is_dir = false;
    target_t t;
    char *raw = NULL;
    int pr = parse_url(env->cwd, url, &t, &raw, err);
    if (pr < 0) {
        free(raw);
        target_free(&t);
        return -1;
    }
    if (pr == 1) {
        if (*raw && strcmp(raw, "/")) {
            *err = xasprintf("ssh:// requires a host before the path: ssh://<host>%s (host-less ssh://%s is not valid)", raw, raw);
            free(raw);
            return -1;
        }
        free(raw);
        *content = host_index(env->cwd);
        *len = strlen(*content);
        *is_dir = true;
        return 0;
    }
    int rc = -1;
    char *path = remote_path(&t, raw, err);
    if (!path) goto out;
    const char *sh = posix_shell(&t, err);
    if (!sh) goto out;
    char *qp = ssh_quote(path);
    char *cmd = xasprintf("if [ -d %s ]; then echo directory; elif [ -f %s ]; then echo file; elif [ -e %s ]; then echo other; else echo "
                          "missing; fi",
                          qp, qp, qp);
    proc_result_t r = {0};
    char *serr = NULL;
    const char *kind = "missing";
    if (run_posix(&t, cmd, NULL, 0, &r, &serr) == 0 && r.out) {
        if (str_starts(r.out, "directory")) kind = "directory";
        else if (str_starts(r.out, "file")) kind = "file";
        else if (str_starts(r.out, "other")) kind = "other";
    }
    free(serr);
    proc_result_free(&r);
    free(cmd);
    if (!strcmp(kind, "directory")) {
        free(qp);
        *content = list_dir(&t, path, sh, err);
        if (*content) {
            *len = strlen(*content);
            *is_dir = true;
            rc = 0;
        }
        goto out;
    }
    if (!strcmp(kind, "other")) {
        free(qp);
        *err = xasprintf("ssh://: %s is not a regular file (FIFO, socket, or device); ssh:// reads UTF-8 text files only — use `bash` "
                         "with a remote SSH command for special files",
                         path);
        goto out;
    }
    /* nothing at the path (not even a dangling symlink) exits with a reserved status, which login-shell stdout noise cannot fake */
    cmd = xasprintf("head -c %d %s || { s=$?; if [ ! -e %s ] && [ ! -L %s ]; then exit %d; fi; exit $s; }", SSH_TEXT_MAX + 1, qp, qp, qp,
                    SSH_REMOTE_MISSING_EXIT);
    free(qp);
    memset(&r, 0, sizeof r);
    if (run_posix(&t, cmd, NULL, 0, &r, err) != 0) {
        if (missing && r.exit_code == SSH_REMOTE_MISSING_EXIT) *missing = true;
        free(cmd);
        proc_result_free(&r);
        goto out;
    }
    free(cmd);
    if (r.out_len > SSH_TEXT_MAX) {
        *err = xasprintf("ssh://: %s exceeds the 1 MiB limit; ssh:// supports text files up to 1 MiB — use an sshfs mount for larger files",
                         path);
        proc_result_free(&r);
        goto out;
    }
    if (memchr(r.out ? r.out : "", 0, r.out_len) || !utf8_valid(r.out ? r.out : "", r.out_len)) {
        *err = xasprintf("ssh://: %s is a binary or non-UTF-8 file; ssh:// supports UTF-8 text only — use `bash` with a remote SSH "
                         "command or an `sshfs` mount",
                         path);
        proc_result_free(&r);
        goto out;
    }
    *content = r.out ? r.out : xstrdup("");
    *len = r.out_len;
    r.out = NULL;
    proc_result_free(&r);
    rc = 0;
out:
    free(path);
    free(raw);
    target_free(&t);
    return rc;
}

int ssh_store(tool_env_t *env, const char *url, const char *data, size_t len, char **err) {
    target_t t;
    char *raw = NULL;
    int pr = parse_url(env->cwd, url, &t, &raw, err);
    if (pr != 0) {
        if (pr == 1) *err = xstrdup("ssh:// write requires a host: ssh://<host>/<absolute-path>");
        free(raw);
        target_free(&t);
        return -1;
    }
    int rc = -1;
    char *path = remote_path(&t, raw, err);
    if (!path) goto out;
    if (path[strlen(path) - 1] == '/') {
        *err = xstrdup("ssh://: destination is a directory path (trailing '/'); ssh:// write requires a file path");
        goto out;
    }
    char *u = uuid_v4();
    char *tmpn = xasprintf("%s.omc-tmp.%s", path, u);
    free(u);
    char *dest = ssh_quote(path), *tmp = ssh_quote(tmpn);
    free(tmpn);
    char *cmd = xasprintf("t=%s; trap 'rm -f -- \"$t\"' 0; mkdir -p -- \"$(dirname \"$t\")\" && cat > \"$t\" && { "
                          "if [ -d %s ]; then echo 'ssh://: destination is a directory' >&2; exit 1; "
                          "elif [ -f %s ] && [ ! -L %s ]; then cat \"$t\" > %s || exit 1; "
                          "elif [ -e %s ] && [ ! -L %s ]; then echo 'ssh://: destination is a special file (not a regular file)' >&2; exit 1; "
                          "else mv \"$t\" %s; fi; }",
                          tmp, dest, dest, dest, dest, dest, dest, dest);
    free(dest);
    free(tmp);
    proc_result_t r = {0};
    rc = run_posix(&t, cmd, data ? data : "", len, &r, err);
    proc_result_free(&r);
    free(cmd);
out:
    free(path);
    free(raw);
    target_free(&t);
    return rc;
}

bool ssh_available(void) {
    char *b = which("ssh");
    free(b);
    return b != NULL;
}
