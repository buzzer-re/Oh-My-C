#define _GNU_SOURCE
#include "lsp.h"

#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "core/proc.h"
#include "net/http.h"
#include "text/glob.h"
#include "text/walker.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

/* ---------- types ---------- */

typedef struct {
    int64_t id;
    json_t *resp;
    bool done;
} lpend_t;

typedef struct {
    char *uri;
    int version;
    int64_t mtime;
} opened_t;

typedef struct lsp_client {
    char *name;
    json_t *cfg;
    char *root;
    child_t child;
    pthread_t reader;
    bool reader_started;
    volatile bool alive;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    VEC(lpend_t *) pending;
    int64_t next_id;
    json_t *caps;        /* server capabilities */
    json_t *diags;       /* uri -> {version, items[]} */
    int64_t diag_seq;    /* bumps on every publishDiagnostics */
    VEC(opened_t) opened;
    char *error;
} lsp_client_t;

struct lsp_manager {
    char *cwd;
    json_t *servers; /* name -> config (merged defaults + user) */
    VEC(lsp_client_t *) clients;
    pthread_mutex_t mu;
};

/* ---------- config ---------- */

static void merge_lsp_file(json_t *servers, const char *path) {
    json_t *j = NULL;
    if (str_ends(path, ".json")) {
        size_t n;
        char *s = fs_read(path, &n);
        if (s) j = json_parse(s, n, NULL);
        free(s);
    } else if (fs_is_file(path)) {
        char *err = NULL;
        extern json_t *yaml_parse_file_json(const char *p, char **e);
        j = yaml_parse_file_json(path, &err);
        free(err);
    }
    if (!json_is_obj(j)) {
        json_free(j);
        return;
    }
    json_t *map = json_get_obj(j, "servers");
    if (!map) map = j;
    const char *k;
    json_t *v;
    json_obj_foreach(map, i, k, v) {
        if (!json_is_obj(v)) continue;
        json_t *cur = json_get(servers, k);
        if (json_is_obj(cur)) json_merge(cur, v);
        else json_set(servers, k, json_clone(v));
    }
    json_free(j);
}

static json_t *load_servers(const char *cwd) {
    size_t n;
    const char *d = embed_get("assets/lsp-servers.json", &n);
    json_t *servers = d ? json_parse(d, n, NULL) : NULL;
    if (!servers) servers = json_obj();
    const char *names[] = {"lsp.json", ".lsp.json", "lsp.yaml", ".lsp.yaml", "lsp.yml", ".lsp.yml"};
    for (size_t i = ARRAY_LEN(names); i > 0; i--) {
        char *p = omc_agent_path(names[i - 1]);
        merge_lsp_file(servers, p);
        free(p);
    }
    const char *pdirs[] = {".omp", ".omc"};
    for (size_t k = 0; k < ARRAY_LEN(pdirs); k++) {
        for (size_t i = ARRAY_LEN(names); i > 0; i--) {
            char *p = path_join3(cwd, pdirs[k], names[i - 1]);
            merge_lsp_file(servers, p);
            free(p);
        }
    }
    json_t *s2 = settings_get("lsp.servers");
    if (json_is_obj(s2)) json_merge(servers, s2);
    return servers;
}

static bool server_handles(const json_t *cfg, const char *path) {
    const char *base = path_basename(path), *ext = path_ext(path);
    json_t *ft = json_get_arr(cfg, "fileTypes"), *x;
    json_arr_foreach(ft, i, x) {
        const char *f = json_sv(x);
        if (!f) continue;
        if (f[0] == '.' ? !strcasecmp(f, ext) : (!strcasecmp(f, base) || (ext[0] && !strcasecmp(f, ext + 1)))) return true;
    }
    return false;
}

static char *find_root(const json_t *cfg, const char *file, const char *cwd) {
    char *dir = path_dirname(file);
    json_t *markers = json_get_arr(cfg, "rootMarkers");
    while (dir) {
        json_t *m;
        json_arr_foreach(markers, i, m) {
            const char *mk = json_sv(m);
            if (!mk) continue;
            if (strchr(mk, '*')) {
                dirlist_t dl;
                if (fs_list_dir(dir, &dl) == 0) {
                    bool hit = false;
                    for (size_t k = 0; k < dl.n && !hit; k++) hit = glob_match(mk, dl.v[k].name, 0);
                    dirlist_free(&dl);
                    if (hit) return dir;
                }
            } else {
                char *p = path_join(dir, mk);
                bool e = fs_exists(p);
                free(p);
                if (e) return dir;
            }
        }
        if (!strcmp(dir, "/") || (cwd && !str_starts(dir, cwd) && !str_starts(cwd, dir))) break;
        char *up = path_dirname(dir);
        free(dir);
        dir = up;
    }
    free(dir);
    return xstrdup(cwd);
}

/* ---------- uri / position helpers ---------- */

static char *path_to_uri(const char *p) {
    sb_t b = SB_INIT;
    sb_append(&b, "file://");
    for (const unsigned char *s = (const unsigned char *)p; *s; s++) {
        if (isalnum(*s) || strchr("/-_.~", *s)) sb_putc(&b, (char)*s);
        else sb_appendf(&b, "%%%02X", *s);
    }
    return sb_detach(&b);
}

static char *uri_to_path(const char *uri) {
    if (!str_starts(uri, "file://")) return xstrdup(uri);
    return url_decode(uri + 7);
}

static const char *language_id(const char *path) {
    static const struct {
        const char *ext, *id;
    } map[] = {{".rs", "rust"}, {".c", "c"}, {".h", "c"}, {".cpp", "cpp"}, {".cc", "cpp"}, {".cxx", "cpp"},
               {".hpp", "cpp"}, {".go", "go"}, {".ts", "typescript"}, {".tsx", "typescriptreact"},
               {".js", "javascript"}, {".jsx", "javascriptreact"}, {".mjs", "javascript"}, {".py", "python"},
               {".java", "java"}, {".rb", "ruby"}, {".lua", "lua"}, {".zig", "zig"}, {".swift", "swift"},
               {".kt", "kotlin"}, {".cs", "csharp"}, {".php", "php"}, {".sh", "shellscript"}, {".json", "json"},
               {".yaml", "yaml"}, {".yml", "yaml"}, {".md", "markdown"}, {".html", "html"}, {".css", "css"},
               {".dart", "dart"}, {".ex", "elixir"}, {".hs", "haskell"}, {".ml", "ocaml"}, {".nix", "nix"},
               {".tf", "terraform"}, {".vue", "vue"}, {".svelte", "svelte"}, {".scala", "scala"}, {".m", "objective-c"}};
    const char *e = path_ext(path);
    for (size_t i = 0; i < ARRAY_LEN(map); i++)
        if (!strcasecmp(e, map[i].ext)) return map[i].id;
    return "plaintext";
}

/* byte offset in line -> UTF-16 code units */
static int64_t utf16_col(const char *line, size_t byte_col) {
    int64_t u = 0;
    size_t i = 0, n = strlen(line);
    while (i < byte_col && i < n) {
        uint32_t cp;
        int k = utf8_decode(line + i, n - i, &cp);
        u += cp >= 0x10000 ? 2 : 1;
        i += (size_t)k;
    }
    return u;
}

/* UTF-16 column -> byte offset in line */
static size_t byte_col(const char *line, int64_t u16) {
    int64_t u = 0;
    size_t i = 0, n = strlen(line);
    while (i < n && u < u16) {
        uint32_t cp;
        int k = utf8_decode(line + i, n - i, &cp);
        u += cp >= 0x10000 ? 2 : 1;
        i += (size_t)k;
    }
    return i;
}

/* ---------- protocol ---------- */

static int send_json(lsp_client_t *c, const json_t *msg) {
    char *body = json_dump(msg, 0);
    char *hdr = xasprintf("Content-Length: %zu\r\n\r\n", strlen(body));
    pthread_mutex_lock(&c->mu);
    int rc = 0;
    const char *parts[2] = {hdr, body};
    for (int k = 0; k < 2 && rc == 0; k++) {
        size_t off = 0, len = strlen(parts[k]);
        while (off < len) {
            ssize_t w = write(c->child.in_fd, parts[k] + off, len - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                rc = -1;
                break;
            }
            off += (size_t)w;
        }
    }
    pthread_mutex_unlock(&c->mu);
    free(hdr);
    free(body);
    return rc;
}

static void respond(lsp_client_t *c, const json_t *id, json_t *result) {
    json_t *r = json_obj();
    json_set_str(r, "jsonrpc", "2.0");
    json_set(r, "id", json_clone(id));
    json_set(r, "result", result ? result : json_null());
    send_json(c, r);
    json_free(r);
}

static int apply_workspace_edit(const json_t *edit, sb_t *summary);

static void handle_message(lsp_client_t *c, json_t *msg) {
    json_t *id = json_get(msg, "id");
    const char *method = json_get_str(msg, "method");
    if (!method && id) {
        int64_t iv = json_is_num(id) ? (id->is_int ? id->u.num.i : (int64_t)id->u.num.d) : -1;
        pthread_mutex_lock(&c->mu);
        for (size_t i = 0; i < c->pending.n; i++) {
            if (c->pending.v[i]->id == iv) {
                c->pending.v[i]->resp = msg;
                c->pending.v[i]->done = true;
                msg = NULL;
                pthread_cond_broadcast(&c->cv);
                break;
            }
        }
        pthread_mutex_unlock(&c->mu);
        json_free(msg);
        return;
    }
    if (!method) {
        json_free(msg);
        return;
    }
    json_t *params = json_get(msg, "params");
    if (!strcmp(method, "textDocument/publishDiagnostics")) {
        const char *uri = json_get_str(params, "uri");
        if (uri) {
            pthread_mutex_lock(&c->mu);
            json_set(c->diags, uri, json_clone(json_get(params, "diagnostics")));
            c->diag_seq++;
            pthread_cond_broadcast(&c->cv);
            pthread_mutex_unlock(&c->mu);
        }
    } else if (id) {
        if (!strcmp(method, "workspace/configuration")) {
            json_t *items = json_get_arr(params, "items"), *it;
            json_t *res = json_arr();
            json_t *settings = json_get(c->cfg, "settings");
            json_arr_foreach(items, i, it) {
                const char *sec = json_get_str(it, "section");
                json_t *v = sec && settings ? json_path(settings, sec) : settings;
                json_push(res, v ? json_clone(v) : json_null());
            }
            respond(c, id, res);
        } else if (!strcmp(method, "workspace/applyEdit")) {
            sb_t s = SB_INIT;
            int rc = apply_workspace_edit(json_get(params, "edit"), &s);
            sb_free(&s);
            json_t *r = json_obj();
            json_set_bool(r, "applied", rc == 0);
            respond(c, id, r);
        } else if (!strcmp(method, "workspace/workspaceFolders")) {
            json_t *arr = json_arr();
            json_t *f = json_obj();
            char *u = path_to_uri(c->root);
            json_set_str(f, "uri", u);
            json_set_str(f, "name", path_basename(c->root));
            free(u);
            json_push(arr, f);
            respond(c, id, arr);
        } else {
            respond(c, id, NULL); /* window/workDoneProgress/create, client/registerCapability, ... */
        }
    }
    json_free(msg);
}

static void *reader_main(void *p) {
    lsp_client_t *c = p;
    sb_t buf = SB_INIT;
    char tmp[65536];
    while (c->alive) {
        struct pollfd pf = {c->child.out_fd, POLLIN, 0};
        int pr = poll(&pf, 1, 200);
        if (pr <= 0) continue;
        ssize_t n = read(c->child.out_fd, tmp, sizeof tmp);
        if (n <= 0) break;
        sb_appendn(&buf, tmp, (size_t)n);
        for (;;) {
            char *hdr_end = buf.len ? memmem(buf.s, buf.len, "\r\n\r\n", 4) : NULL;
            if (!hdr_end) break;
            size_t hlen = (size_t)(hdr_end - buf.s) + 4;
            const char *cl = str_istr(buf.s, "Content-Length:");
            if (!cl || cl > hdr_end) {
                memmove(buf.s, buf.s + hlen, buf.len - hlen);
                sb_truncate(&buf, buf.len - hlen);
                continue;
            }
            size_t blen = (size_t)strtoull(cl + 15, NULL, 10);
            if (blen > (256u << 20)) {
                c->alive = false;
                break;
            }
            if (buf.len < hlen + blen) break;
            json_t *msg = json_parse(buf.s + hlen, blen, NULL);
            memmove(buf.s, buf.s + hlen + blen, buf.len - hlen - blen);
            sb_truncate(&buf, buf.len - hlen - blen);
            if (msg) handle_message(c, msg);
        }
    }
    sb_free(&buf);
    pthread_mutex_lock(&c->mu);
    c->alive = false;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

static json_t *request(lsp_client_t *c, const char *method, json_t *params, int64_t timeout_ms, char **err) {
    *err = NULL;
    if (!c->alive) {
        json_free(params);
        *err = xasprintf("%s is not running", c->name);
        return NULL;
    }
    lpend_t *p = xcalloc(1, sizeof(*p));
    pthread_mutex_lock(&c->mu);
    p->id = ++c->next_id;
    vec_push(&c->pending, p);
    pthread_mutex_unlock(&c->mu);
    json_t *msg = json_obj();
    json_set_str(msg, "jsonrpc", "2.0");
    json_set_int(msg, "id", p->id);
    json_set_str(msg, "method", method);
    json_set(msg, "params", params ? params : json_obj());
    send_json(c, msg);
    json_free(msg);
    json_t *result = NULL;
    pthread_mutex_lock(&c->mu);
    int64_t deadline = mono_ms() + timeout_ms;
    while (!p->done && c->alive && mono_ms() < deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&c->cv, &c->mu, &ts);
    }
    if (p->done) {
        json_t *e = json_get(p->resp, "error");
        if (e) *err = xasprintf("%s: %s", method, json_get_str(e, "message") ? json_get_str(e, "message") : "error");
        else {
            result = json_take(p->resp, "result");
            if (!result) result = json_null();
        }
    } else {
        *err = xasprintf("%s: %s timed out after %llds", c->name, method, (long long)(timeout_ms / 1000));
    }
    for (size_t i = 0; i < c->pending.n; i++)
        if (c->pending.v[i] == p) {
            memmove(c->pending.v + i, c->pending.v + i + 1, (c->pending.n - i - 1) * sizeof(void *));
            c->pending.n--;
            break;
        }
    pthread_mutex_unlock(&c->mu);
    json_free(p->resp);
    free(p);
    return result;
}

static void notify(lsp_client_t *c, const char *method, json_t *params) {
    json_t *msg = json_obj();
    json_set_str(msg, "jsonrpc", "2.0");
    json_set_str(msg, "method", method);
    json_set(msg, "params", params ? params : json_obj());
    send_json(c, msg);
    json_free(msg);
}

/* ---------- client lifecycle ---------- */

static lsp_client_t *client_start(const char *name, const json_t *cfg, const char *root, char **err) {
    const char *cmd = json_get_str(cfg, "command");
    char *bin = cmd ? which(cmd) : NULL;
    if (!bin) {
        *err = xasprintf("%s: command `%s` not found", name, cmd ? cmd : "?");
        return NULL;
    }
    strvec_t argv = {0};
    strvec_push_own(&argv, bin);
    json_t *a;
    json_arr_foreach(json_get_arr(cfg, "args"), i, a) {
        if (!json_is_str(a)) continue;
        if (!strcmp(json_sv(a), "$PID")) strvec_push_own(&argv, xasprintf("%d", (int)getpid()));
        else strvec_push(&argv, json_sv(a));
    }
    vec_push(&argv, NULL);
    lsp_client_t *c = xcalloc(1, sizeof(*c));
    c->name = xstrdup(name);
    c->cfg = json_clone(cfg);
    c->root = xstrdup(root);
    c->diags = json_obj();
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    int rc = child_spawn((const char *const *)argv.v, root, NULL, false, &c->child);
    argv.n--;
    strvec_free(&argv);
    if (rc != 0) {
        *err = xasprintf("%s: failed to start: %s", name, strerror(errno));
        json_free(c->cfg);
        json_free(c->diags);
        free(c->name);
        free(c->root);
        free(c);
        return NULL;
    }
    c->alive = true;
    if (pthread_create(&c->reader, NULL, reader_main, c) == 0) c->reader_started = true;
    json_t *p = json_obj();
    json_set_int(p, "processId", getpid());
    char *ru = path_to_uri(root);
    json_set_str(p, "rootUri", ru);
    json_set_str(p, "rootPath", root);
    json_t *wf = json_arr();
    json_t *f = json_obj();
    json_set_str(f, "uri", ru);
    json_set_str(f, "name", path_basename(root));
    json_push(wf, f);
    json_set(p, "workspaceFolders", wf);
    free(ru);
    json_t *ci = json_obj();
    json_set_str(ci, "name", "omc");
    json_set_str(ci, "version", OMC_VERSION);
    json_set(p, "clientInfo", ci);
    json_set(p, "capabilities", json_parse_str(
        "{\"general\":{\"positionEncodings\":[\"utf-16\"]},"
        "\"workspace\":{\"applyEdit\":true,\"workspaceEdit\":{\"documentChanges\":true,\"resourceOperations\":[\"create\",\"rename\",\"delete\"]},"
        "\"configuration\":true,\"workspaceFolders\":true,\"symbol\":{},\"fileOperations\":{\"willRename\":true,\"didRename\":true},"
        "\"didChangeConfiguration\":{\"dynamicRegistration\":false}},"
        "\"textDocument\":{\"synchronization\":{\"didSave\":true},\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]},"
        "\"definition\":{\"linkSupport\":true},\"typeDefinition\":{\"linkSupport\":true},\"implementation\":{\"linkSupport\":true},"
        "\"references\":{},\"documentSymbol\":{\"hierarchicalDocumentSymbolSupport\":true},\"rename\":{\"prepareSupport\":true},"
        "\"codeAction\":{\"codeActionLiteralSupport\":{\"codeActionKind\":{\"valueSet\":[\"\",\"quickfix\",\"refactor\",\"refactor.extract\",\"refactor.inline\",\"refactor.rewrite\",\"source\",\"source.organizeImports\"]}},\"resolveSupport\":{\"properties\":[\"edit\"]}},"
        "\"publishDiagnostics\":{\"relatedInformation\":true}},"
        "\"window\":{\"workDoneProgress\":true}}"));
    json_t *io = json_get(cfg, "initOptions");
    if (io) json_set(p, "initializationOptions", json_clone(io));
    char *e2 = NULL;
    int64_t wt = json_get_int(cfg, "warmupTimeoutMs", 0);
    json_t *res = request(c, "initialize", p, MAX(wt, (int64_t)30000), &e2);
    if (!res) {
        *err = e2;
        c->error = xstrdup(e2 ? e2 : "initialize failed");
        return c; /* keep for status reporting */
    }
    c->caps = json_take(res, "capabilities");
    json_free(res);
    notify(c, "initialized", json_obj());
    json_t *st = json_get(cfg, "settings");
    if (st) {
        json_t *pp = json_obj();
        json_set(pp, "settings", json_clone(st));
        notify(c, "workspace/didChangeConfiguration", pp);
    }
    return c;
}

static void client_stop(lsp_client_t *c) {
    if (c->alive && !c->error) {
        char *err = NULL;
        json_t *r = request(c, "shutdown", NULL, 3000, &err);
        json_free(r);
        free(err);
        notify(c, "exit", NULL);
    }
    c->alive = false;
    child_close(&c->child);
    if (c->reader_started) pthread_join(c->reader, NULL);
    for (size_t i = 0; i < c->opened.n; i++) free(c->opened.v[i].uri);
    vec_free(&c->opened);
    vec_free(&c->pending);
    json_free(c->cfg);
    json_free(c->caps);
    json_free(c->diags);
    free(c->name);
    free(c->root);
    free(c->error);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    free(c);
}

lsp_manager_t *lsp_manager_new(const char *cwd) {
    lsp_manager_t *m = xcalloc(1, sizeof(*m));
    m->cwd = xstrdup(cwd);
    m->servers = load_servers(cwd);
    pthread_mutex_init(&m->mu, NULL);
    return m;
}

void lsp_manager_free(lsp_manager_t *m) {
    if (!m) return;
    for (size_t i = 0; i < m->clients.n; i++) client_stop(m->clients.v[i]);
    vec_free(&m->clients);
    json_free(m->servers);
    free(m->cwd);
    pthread_mutex_destroy(&m->mu);
    free(m);
}

/* find (or start) the client for a file; linters are skipped for intelligence */
static lsp_client_t *client_for(lsp_manager_t *m, const char *abs, bool start, char **err) {
    *err = NULL;
    pthread_mutex_lock(&m->mu);
    lsp_client_t *found = NULL;
    const char *k;
    json_t *v;
    sb_t tried = SB_INIT;
    json_obj_foreach(m->servers, i, k, v) {
        if (found) break;
        if (json_get_bool(v, "disabled", false) || json_get_bool(v, "isLinter", false)) continue;
        if (!server_handles(v, abs)) continue;
        for (size_t c = 0; c < m->clients.n; c++)
            if (!strcmp(m->clients.v[c]->name, k) && !m->clients.v[c]->error && m->clients.v[c]->alive) found = m->clients.v[c];
        if (found || !start) continue;
        const char *cmd = json_get_str(v, "command");
        char *bin = cmd ? which(cmd) : NULL;
        if (!bin) {
            sb_appendf(&tried, "%s%s", tried.len ? ", " : "", cmd ? cmd : k);
            continue;
        }
        free(bin);
        char *root = find_root(v, abs, m->cwd);
        char *e = NULL;
        lsp_client_t *c = client_start(k, v, root, &e);
        free(root);
        if (c) {
            vec_push(&m->clients, c);
            if (!c->error) found = c;
            else {
                free(*err);
                *err = e;
                e = NULL;
            }
        }
        free(e);
    }
    pthread_mutex_unlock(&m->mu);
    if (!found && !*err) {
        *err = tried.len ? xasprintf("No language server installed for %s (tried: %s)", path_basename(abs), sb_cstr(&tried))
                         : xasprintf("No language server configured for %s", path_basename(abs));
    }
    sb_free(&tried);
    return found;
}

/* open or refresh a document; returns the uri (owned) */
static char *sync_doc(lsp_client_t *c, const char *abs) {
    char *uri = path_to_uri(abs);
    int64_t mt = fs_mtime_ms(abs);
    opened_t *o = NULL;
    for (size_t i = 0; i < c->opened.n; i++)
        if (!strcmp(c->opened.v[i].uri, uri)) o = &c->opened.v[i];
    if (o && o->mtime == mt) return uri;
    size_t n;
    char *text = fs_read(abs, &n);
    if (!text) text = xstrdup("");
    if (!o) {
        opened_t no = {xstrdup(uri), 1, mt};
        vec_push(&c->opened, no);
        json_t *p = json_obj(), *td = json_obj();
        json_set_str(td, "uri", uri);
        json_set_str(td, "languageId", json_get_str(c->cfg, "languageId") ? json_get_str(c->cfg, "languageId") : language_id(abs));
        json_set_int(td, "version", 1);
        json_set_str(td, "text", text);
        json_set(p, "textDocument", td);
        notify(c, "textDocument/didOpen", p);
    } else {
        o->version++;
        o->mtime = mt;
        json_t *p = json_obj(), *td = json_obj(), *changes = json_arr(), *ch = json_obj();
        json_set_str(td, "uri", uri);
        json_set_int(td, "version", o->version);
        json_set(p, "textDocument", td);
        json_set_str(ch, "text", text);
        json_push(changes, ch);
        json_set(p, "contentChanges", changes);
        notify(c, "textDocument/didChange", p);
        json_t *sp = json_obj(), *std = json_obj();
        json_set_str(std, "uri", uri);
        json_set(sp, "textDocument", std);
        notify(c, "textDocument/didSave", sp);
    }
    free(text);
    return uri;
}

/* ---------- workspace edits ---------- */

typedef struct {
    int64_t sl, sc, el, ec; /* utf-16 */
    char *text;
} tedit_t;

static int cmp_tedit(const void *a, const void *b) {
    const tedit_t *x = a, *y = b;
    if (x->sl != y->sl) return x->sl < y->sl ? 1 : -1;
    return x->sc < y->sc ? 1 : x->sc > y->sc ? -1 : 0;
}

static int apply_text_edits(const char *path, const json_t *edits, sb_t *summary) {
    size_t n;
    char *text = fs_read(path, &n);
    if (!text) return -1;
    strvec_t lines = str_split(text, "\n");
    bool trailing = n && text[n - 1] == '\n';
    free(text);
    /* rebuild as single string with line offsets for each edit (descending order) */
    VEC(tedit_t) es = {0};
    json_t *e;
    json_arr_foreach(edits, i, e) {
        json_t *r = json_get(e, "range");
        tedit_t t = {json_get_int(json_get(r, "start"), "line", 0), json_get_int(json_get(r, "start"), "character", 0),
                     json_get_int(json_get(r, "end"), "line", 0), json_get_int(json_get(r, "end"), "character", 0),
                     xstrdup(json_get_str(e, "newText") ? json_get_str(e, "newText") : "")};
        vec_push(&es, t);
    }
    if (es.n > 1) qsort(es.v, es.n, sizeof(tedit_t), cmp_tedit);
    for (size_t i = 0; i < es.n; i++) {
        tedit_t *t = &es.v[i];
        if (t->sl >= (int64_t)lines.n) {
            strvec_push(&lines, "");
            t->sl = t->el = (int64_t)lines.n - 1;
        }
        if (t->el >= (int64_t)lines.n) {
            t->el = (int64_t)lines.n - 1;
            t->ec = INT64_MAX / 4;
        }
        const char *sline = lines.v[t->sl], *eline = lines.v[t->el];
        size_t sb2 = byte_col(sline, t->sc), eb = byte_col(eline, t->ec);
        char *merged = xasprintf("%.*s%s%s", (int)sb2, sline, t->text, eline + eb);
        strvec_t nl = str_split(merged, "\n");
        free(merged);
        /* replace lines [sl, el] with nl */
        size_t rm = (size_t)(t->el - t->sl + 1);
        for (size_t k = 0; k < rm; k++) free(lines.v[t->sl + (int64_t)k]);
        size_t tail = lines.n - (size_t)t->sl - rm;
        size_t newn = lines.n - rm + nl.n;
        if (newn > lines.cap) {
            lines.cap = newn + 8;
            lines.v = xrealloc(lines.v, lines.cap * sizeof(char *));
        }
        memmove(lines.v + t->sl + (int64_t)nl.n, lines.v + t->sl + (int64_t)rm, tail * sizeof(char *));
        for (size_t k = 0; k < nl.n; k++) lines.v[t->sl + (int64_t)k] = nl.v[k];
        lines.n = newn;
        free(nl.v);
        free(t->text);
    }
    char *out = strvec_join(&lines, "\n");
    size_t ol = strlen(out);
    if (trailing && (!ol || out[ol - 1] != '\n')) {
        char *o2 = xasprintf("%s", out);
        free(out);
        out = o2;
    }
    int rc = fs_write_atomic(path, out, strlen(out), 0644);
    if (summary) sb_appendf(summary, "  %s: %zu edit(s)\n", path, es.n);
    free(out);
    strvec_free(&lines);
    vec_free(&es);
    return rc;
}

static int apply_workspace_edit(const json_t *edit, sb_t *summary) {
    int rc = 0;
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(edit, "changes"), i, k, v) {
        char *p = uri_to_path(k);
        if (apply_text_edits(p, v, summary) != 0) rc = -1;
        free(p);
    }
    json_t *dc;
    json_arr_foreach(json_get_arr(edit, "documentChanges"), i, dc) {
        const char *kind = json_get_str(dc, "kind");
        if (!kind) {
            char *p = uri_to_path(json_get_str(json_get(dc, "textDocument"), "uri"));
            if (apply_text_edits(p, json_get_arr(dc, "edits"), summary) != 0) rc = -1;
            free(p);
        } else if (!strcmp(kind, "rename")) {
            char *a = uri_to_path(json_get_str(dc, "oldUri")), *b = uri_to_path(json_get_str(dc, "newUri"));
            char *dir = path_dirname(b);
            fs_mkdir_p(dir, 0755);
            free(dir);
            if (rename(a, b) != 0) rc = -1;
            else if (summary) sb_appendf(summary, "  renamed %s -> %s\n", a, b);
            free(a);
            free(b);
        } else if (!strcmp(kind, "create")) {
            char *p = uri_to_path(json_get_str(dc, "uri"));
            if (!fs_exists(p)) fs_write(p, "", 0);
            if (summary) sb_appendf(summary, "  created %s\n", p);
            free(p);
        } else if (!strcmp(kind, "delete")) {
            char *p = uri_to_path(json_get_str(dc, "uri"));
            if (unlink(p) != 0) rc = -1;
            else if (summary) sb_appendf(summary, "  deleted %s\n", p);
            free(p);
        }
    }
    return rc;
}

/* ---------- formatting ---------- */

static char *line_of_file(const char *path, int64_t line0) {
    size_t n;
    char *t = fs_read_limit(path, 8u << 20, &n, NULL);
    if (!t) return NULL;
    char *p = t;
    for (int64_t i = 0; i < line0 && p; i++) {
        p = strchr(p, '\n');
        if (p) p++;
    }
    char *r = p ? xstrndup(p, strcspn(p, "\r\n")) : NULL;
    free(t);
    return r;
}

static void fmt_location(sb_t *b, const char *cwd, const json_t *loc) {
    const char *uri = json_get_str(loc, "uri");
    json_t *range = json_get(loc, "range");
    if (!uri) {
        uri = json_get_str(loc, "targetUri");
        range = json_get(loc, "targetSelectionRange");
    }
    if (!uri) return;
    char *p = uri_to_path(uri);
    char *d = path_display(p, cwd);
    int64_t l = json_get_int(json_get(range, "start"), "line", 0), ch = json_get_int(json_get(range, "start"), "character", 0);
    char *src = line_of_file(p, l);
    int64_t col = src ? (int64_t)byte_col(src, ch) + 1 : ch + 1;
    char *trim = src ? str_trim(src) : NULL;
    sb_appendf(b, "%s:%lld:%lld%s%s\n", d, (long long)l + 1, (long long)col, trim ? "  " : "", trim ? trim : "");
    free(trim);
    free(src);
    free(d);
    free(p);
}

static const char *SEV[] = {"", "error", "warning", "info", "hint"};

static size_t fmt_diags(sb_t *b, const char *display, const json_t *items, int *errors) {
    json_t *d;
    size_t n = 0;
    json_arr_foreach(items, i, d) {
        int64_t sev = json_get_int(d, "severity", 1);
        if (sev < 1 || sev > 4) sev = 1;
        if (sev == 1 && errors) (*errors)++;
        json_t *st = json_get(json_get(d, "range"), "start");
        const char *src = json_get_str(d, "source");
        json_t *code = json_get(d, "code");
        char *cs = code ? (json_is_str(code) ? xstrdup(json_sv(code)) : json_dump(code, 0)) : NULL;
        char *msg = xstrndup(json_get_str(d, "message") ? json_get_str(d, "message") : "", 400);
        for (char *p = msg; *p; p++)
            if (*p == '\n') *p = ' ';
        char *tag = src || cs ? xasprintf(" [%s%s%s]", src ? src : "", src && cs ? " " : "", cs ? cs : "") : xstrdup("");
        sb_appendf(b, "%s:%lld:%lld %s%s: %s\n", display, (long long)json_get_int(st, "line", 0) + 1,
                   (long long)json_get_int(st, "character", 0) + 1, SEV[sev], tag, msg);
        free(tag);
        free(cs);
        free(msg);
        n++;
    }
    return n;
}

/* wait until diagnostics for uri change from seq (or timeout) */
static json_t *wait_diags(lsp_client_t *c, const char *uri, int64_t since_seq, int64_t wait_ms) {
    pthread_mutex_lock(&c->mu);
    int64_t deadline = mono_ms() + wait_ms;
    while (c->alive && mono_ms() < deadline) {
        json_t *d = json_get(c->diags, uri);
        if (d && c->diag_seq > since_seq) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&c->cv, &c->mu, &ts);
    }
    json_t *r = json_clone(json_get(c->diags, uri));
    pthread_mutex_unlock(&c->mu);
    return r;
}

char *lsp_after_write(lsp_manager_t *m, const char *abs, int64_t wait_ms) {
    if (!m) return NULL;
    char *err = NULL;
    lsp_client_t *c = client_for(m, abs, true, &err);
    free(err);
    if (!c) return NULL;
    pthread_mutex_lock(&c->mu);
    int64_t seq = c->diag_seq;
    pthread_mutex_unlock(&c->mu);
    char *uri = sync_doc(c, abs);
    json_t *items = wait_diags(c, uri, seq, wait_ms);
    char *out = NULL;
    int errors = 0;
    if (json_len(items)) {
        sb_t b = SB_INIT;
        char *disp = path_display(abs, m->cwd);
        sb_appendf(&b, "LSP diagnostics (%s):\n", c->name);
        fmt_diags(&b, disp, items, &errors);
        free(disp);
        out = sb_detach(&b);
    }
    json_free(items);
    free(uri);
    return out;
}

char *lsp_status(lsp_manager_t *m) {
    sb_t b = SB_INIT;
    pthread_mutex_lock(&m->mu);
    for (size_t i = 0; i < m->clients.n; i++) {
        lsp_client_t *c = m->clients.v[i];
        sb_appendf(&b, "%s %s (root %s)%s%s\n", c->error ? "✗" : c->alive ? "✓" : "○", c->name, c->root,
                   c->error ? ": " : "", c->error ? c->error : "");
    }
    pthread_mutex_unlock(&m->mu);
    if (!b.len) sb_append(&b, "No language servers running (they start on first use).\n");
    sb_append(&b, "Available:");
    const char *k;
    json_t *v;
    json_obj_foreach(m->servers, i, k, v) {
        char *bin = json_get_str(v, "command") ? which(json_get_str(v, "command")) : NULL;
        if (bin) sb_appendf(&b, " %s", k);
        free(bin);
    }
    return sb_detach(&b);
}

/* ---------- tool ---------- */

typedef struct {
    tool_env_t *env;
    lsp_manager_t *m;
} lsp_tool_t;

/* resolve column (utf-16) of `symbol` on 1-based line; "#N" picks the Nth match */
static int resolve_position(const char *path, int64_t line1, const char *symbol, int64_t *col16, char **err) {
    char *ltext = line_of_file(path, line1 - 1);
    if (!ltext) {
        *err = xasprintf("line %lld is out of range", (long long)line1);
        return -1;
    }
    if (!symbol || !*symbol) {
        size_t k = strspn(ltext, " \t");
        *col16 = utf16_col(ltext, k);
        free(ltext);
        return 0;
    }
    char *sym = xstrdup(symbol);
    int nth = 1;
    char *hash = strrchr(sym, '#');
    if (hash && hash[1] && strspn(hash + 1, "0123456789") == strlen(hash + 1)) {
        nth = atoi(hash + 1);
        *hash = 0;
    }
    bool ident = true;
    for (const char *p = sym; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '$')) ident = false;
    const char *found = NULL;
    int count = 0;
    for (int pass = 0; pass < 2 && !found; pass++) {
        count = 0;
        for (const char *p = ltext; (p = pass ? str_istr(p, sym) : strstr(p, sym)); p++) {
            if (ident) {
                bool lb = p == ltext || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
                const char *e = p + strlen(sym);
                bool rb = !(isalnum((unsigned char)*e) || *e == '_');
                if (!lb || !rb) continue;
            }
            if (++count == nth) {
                found = p;
                break;
            }
        }
    }
    if (!found) {
        *err = xasprintf("symbol `%s` not found on line %lld: %s", sym, (long long)line1, ltext);
        free(sym);
        free(ltext);
        return -1;
    }
    *col16 = utf16_col(ltext, (size_t)(found - ltext));
    free(sym);
    free(ltext);
    return 0;
}

static json_t *pos_params(const char *uri, int64_t line1, int64_t col16) {
    json_t *p = json_obj(), *td = json_obj(), *pos = json_obj();
    json_set_str(td, "uri", uri);
    json_set(p, "textDocument", td);
    json_set_int(pos, "line", line1 - 1);
    json_set_int(pos, "character", col16);
    json_set(p, "position", pos);
    return p;
}

static void fmt_symbols(sb_t *b, const json_t *syms, int depth, const char *cwd) {
    static const char *KINDS[] = {"", "file", "module", "namespace", "package", "class", "method", "property", "field",
                                  "constructor", "enum", "interface", "function", "variable", "constant", "string",
                                  "number", "boolean", "array", "object", "key", "null", "enum member", "struct",
                                  "event", "operator", "type parameter"};
    json_t *s;
    json_arr_foreach(syms, i, s) {
        int64_t kind = json_get_int(s, "kind", 0);
        const char *kn = kind > 0 && kind < (int64_t)ARRAY_LEN(KINDS) ? KINDS[kind] : "symbol";
        json_t *range = json_get(s, "selectionRange");
        if (!range) range = json_get(s, "range");
        if (!range) range = json_get(json_get(s, "location"), "range");
        for (int k = 0; k < depth; k++) sb_append(b, "  ");
        sb_appendf(b, "%s %s", kn, json_get_str(s, "name"));
        if (json_get_str(s, "detail") && *json_get_str(s, "detail")) sb_appendf(b, " — %s", json_get_str(s, "detail"));
        const char *uri = json_get_str(json_get(s, "location"), "uri");
        if (uri) {
            char *p = uri_to_path(uri);
            char *d = path_display(p, cwd);
            sb_appendf(b, " (%s:%lld)", d, (long long)json_get_int(json_get(range, "start"), "line", 0) + 1);
            free(d);
            free(p);
        } else {
            sb_appendf(b, " :%lld", (long long)json_get_int(json_get(range, "start"), "line", 0) + 1);
        }
        sb_putc(b, '\n');
        if (depth < 8) fmt_symbols(b, json_get_arr(s, "children"), depth + 1, cwd);
    }
}

/* tool-timeouts.ts resolveLspTimeouts: call = max(20, lsp.requestTimeout), wait = lsp.requestTimeout (10) */
static double lsp_default_call_sec(void) {
    double rt = (double)settings_int("lsp.requestTimeout", 10);
    return rt > 20 ? (rt > 300 ? 300 : rt) : 20;
}

static int lsp_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    lsp_tool_t *lt = self->ud;
    lsp_manager_t *m = lt->m;
    UNUSED(ctx);
    const char *action = json_get_str(args, "action");
    const char *file = json_get_str(args, "file");
    double tsec = json_get_num(args, "timeout", lsp_default_call_sec());
    if (tsec < 5) tsec = 5;
    if (tsec > 300) tsec = 300;
    int64_t to = (int64_t)(tsec * 1000);
    if (!action) {
        tool_result_error(out, "action is required");
        return 0;
    }
    if (!strcmp(action, "status")) {
        char *s = lsp_status(m);
        tool_result_text(out, s);
        free(s);
        return 0;
    }
    if (!strcmp(action, "reload")) {
        pthread_mutex_lock(&m->mu);
        for (size_t i = 0; i < m->clients.n; i++) client_stop(m->clients.v[i]);
        m->clients.n = 0;
        json_free(m->servers);
        m->servers = load_servers(m->cwd);
        pthread_mutex_unlock(&m->mu);
        tool_result_text(out, "Language servers stopped; they restart on next use (config re-read).");
        return 0;
    }
    if (!strcmp(action, "capabilities")) {
        /* lsp/tool.ts: "<server>:\n  capabilities: <json, indented>" per server (file's servers, or all running) */
        sb_t b = SB_INIT;
        VEC(lsp_client_t *) list = {0};
        if (file && *file && strcmp(file, "*") != 0) {
            char *abs = path_abs(file, lt->env->cwd);
            char *err = NULL;
            lsp_client_t *c = client_for(m, abs, true, &err);
            free(abs);
            if (!c) {
                tool_result_text(out, "No language server found for this file");
                free(err);
                return 0;
            }
            free(err);
            vec_push(&list, c);
        } else {
            pthread_mutex_lock(&m->mu);
            for (size_t i = 0; i < m->clients.n; i++) vec_push(&list, m->clients.v[i]);
            pthread_mutex_unlock(&m->mu);
        }
        if (!list.n) {
            tool_result_text(out, "No language servers configured");
            return 0;
        }
        for (size_t i = 0; i < list.n; i++) {
            char *caps = list.v[i]->caps ? json_dump(list.v[i]->caps, 2) : xstrdup("{}");
            char *ind = str_replace(caps, "\n", "\n  ");
            sb_appendf(&b, "%s%s:\n  capabilities: %s", i ? "\n" : "", list.v[i]->name, ind);
            free(ind);
            free(caps);
        }
        vec_free(&list);
        tool_result_text(out, b.s);
        sb_free(&b);
        return 0;
    }
    if (!file || !*file) {
        tool_result_error(out, "file is required for %s", action);
        return 0;
    }
    if (!strcmp(action, "diagnostics") && (glob_has_magic(file) || !strcmp(file, "*"))) {
        /* diagnostics over a glob: open matching files and collect */
        walk_opts_t wo = {0};
        globset_t gs = {0};
        if (strcmp(file, "*") != 0) globset_add(&gs, file, 0);
        wo.include = strcmp(file, "*") ? &gs : NULL;
        wo.types = WALK_FILE;
        walk_list_t wl;
        walk_collect(lt->env->cwd, &wo, &wl);
        sb_t b = SB_INIT;
        int errors = 0;
        size_t total = 0, files = 0;
        for (size_t i = 0; i < wl.n && files < 200; i++) {
            char *abs = path_join(lt->env->cwd, wl.v[i].path);
            char *err = NULL;
            lsp_client_t *c = client_for(m, abs, true, &err);
            free(err);
            if (c) {
                files++;
                pthread_mutex_lock(&c->mu);
                int64_t seq = c->diag_seq;
                pthread_mutex_unlock(&c->mu);
                char *uri = sync_doc(c, abs);
                json_t *items = wait_diags(c, uri, seq, 3000);
                total += fmt_diags(&b, wl.v[i].path, items, &errors);
                json_free(items);
                free(uri);
            }
            free(abs);
        }
        walk_list_free(&wl);
        globset_free(&gs);
        if (!files) tool_result_text(out, "No files with a language server matched.");
        else if (!total) tool_result_textf(out, "No diagnostics in %zu file(s).", files);
        else {
            sb_appendf(&b, "%zu diagnostic(s), %d error(s) in %zu file(s)", total, errors, files);
            tool_result_text(out, sb_cstr(&b));
        }
        sb_free(&b);
        return 0;
    }
    if (!strcmp(action, "symbols") && !strcmp(file, "*")) {
        lsp_client_t *c = NULL;
        pthread_mutex_lock(&m->mu);
        if (m->clients.n) c = m->clients.v[0];
        pthread_mutex_unlock(&m->mu);
        if (!c) {
            tool_result_error(out, "No language server running; open a file first (e.g. symbols on a file)");
            return 0;
        }
        json_t *p = json_obj();
        json_set_str(p, "query", json_get_str(args, "query") ? json_get_str(args, "query") : "");
        char *err = NULL;
        json_t *res = request(c, "workspace/symbol", p, to, &err);
        if (!res) {
            tool_result_error(out, "%s", err);
            free(err);
            return 0;
        }
        sb_t b = SB_INIT;
        fmt_symbols(&b, res, 0, lt->env->cwd);
        tool_result_text(out, b.len ? sb_cstr(&b) : "No symbols found");
        sb_free(&b);
        json_free(res);
        return 0;
    }
    char *abs = tool_resolve_path(lt->env, file);
    if (!fs_is_file(abs)) {
        tool_result_error(out, "File not found: %s", file);
        free(abs);
        return 0;
    }
    char *err = NULL;
    lsp_client_t *c = client_for(m, abs, true, &err);
    if (!c) {
        tool_result_error(out, "%s", err ? err : "no language server");
        free(err);
        free(abs);
        return 0;
    }
    char *disp = path_display(abs, lt->env->cwd);
    pthread_mutex_lock(&c->mu);
    int64_t seq = c->diag_seq;
    pthread_mutex_unlock(&c->mu);
    char *uri = sync_doc(c, abs);
    int64_t line1 = json_get_int(args, "line", 1);
    sb_t b = SB_INIT;
    if (!strcmp(action, "diagnostics")) {
        json_t *items = wait_diags(c, uri, seq, MIN(to, (int64_t)10000));
        int errors = 0;
        size_t n = fmt_diags(&b, disp, items, &errors);
        if (!n) tool_result_textf(out, "No diagnostics for %s (%s)", disp, c->name);
        else tool_result_text(out, sb_cstr(&b));
        json_free(items);
    } else if (!strcmp(action, "definition") || !strcmp(action, "references") || !strcmp(action, "hover") ||
               !strcmp(action, "type_definition") || !strcmp(action, "implementation")) {
        int64_t col;
        if (resolve_position(abs, line1, json_get_str(args, "symbol"), &col, &err) != 0) {
            tool_result_error(out, "%s", err);
            goto done;
        }
        const char *method = !strcmp(action, "definition") ? "textDocument/definition"
                             : !strcmp(action, "references") ? "textDocument/references"
                             : !strcmp(action, "hover") ? "textDocument/hover"
                             : !strcmp(action, "type_definition") ? "textDocument/typeDefinition"
                                                                  : "textDocument/implementation";
        json_t *p = pos_params(uri, line1, col);
        if (!strcmp(action, "references")) json_set(p, "context", json_parse_str("{\"includeDeclaration\":true}"));
        json_t *res = request(c, method, p, to, &err);
        if (!res) {
            tool_result_error(out, "%s", err);
            goto done;
        }
        if (!strcmp(action, "hover")) {
            json_t *ct = json_get(res, "contents");
            if (json_is_str(ct)) sb_append(&b, json_sv(ct));
            else if (json_is_obj(ct)) sb_append(&b, json_get_str(ct, "value") ? json_get_str(ct, "value") : "");
            else if (json_is_arr(ct)) {
                json_t *x;
                json_arr_foreach(ct, i, x) {
                    if (b.len) sb_append(&b, "\n\n");
                    sb_append(&b, json_is_str(x) ? json_sv(x) : json_get_str(x, "value") ? json_get_str(x, "value") : "");
                }
            }
            tool_result_text(out, b.len ? sb_cstr(&b) : "No hover information");
        } else {
            if (json_is_obj(res)) fmt_location(&b, lt->env->cwd, res);
            json_t *x;
            json_arr_foreach(res, i, x) fmt_location(&b, lt->env->cwd, x);
            if (!b.len) tool_result_textf(out, "No %s found", action);
            else {
                size_t cnt = str_count(b.s, "\n");
                char *hdr = xasprintf("%zu %s:\n", cnt, cnt == 1 ? "location" : "locations");
                tool_result_textf(out, "%s%s", hdr, sb_cstr(&b));
                free(hdr);
            }
        }
        json_free(res);
    } else if (!strcmp(action, "symbols")) {
        json_t *p = json_obj(), *td = json_obj();
        json_set_str(td, "uri", uri);
        json_set(p, "textDocument", td);
        json_t *res = request(c, "textDocument/documentSymbol", p, to, &err);
        if (!res) {
            tool_result_error(out, "%s", err);
            goto done;
        }
        fmt_symbols(&b, res, 0, lt->env->cwd);
        tool_result_text(out, b.len ? sb_cstr(&b) : "No symbols");
        json_free(res);
    } else if (!strcmp(action, "rename")) {
        const char *nn = json_get_str(args, "new_name");
        const char *sym = json_get_str(args, "symbol");
        if (!nn || !sym) {
            tool_result_error(out, "rename requires symbol and new_name");
            goto done;
        }
        int64_t col;
        if (resolve_position(abs, line1, sym, &col, &err) != 0) {
            tool_result_error(out, "%s", err);
            goto done;
        }
        json_t *p = pos_params(uri, line1, col);
        json_set_str(p, "newName", nn);
        json_t *res = request(c, "textDocument/rename", p, to, &err);
        if (!res || json_is_null(res)) {
            tool_result_error(out, "%s", err ? err : "rename not possible here");
            json_free(res);
            goto done;
        }
        bool apply = json_get_bool(args, "apply", true);
        if (apply) {
            int rc = apply_workspace_edit(res, &b);
            tool_result_textf(out, "%s %s → %s:\n%s", rc == 0 ? "Renamed" : "Partially renamed", sym, nn, sb_cstr(&b));
            out->is_error = rc != 0;
        } else {
            char *d = json_dump(res, 2);
            tool_result_textf(out, "Rename preview (not applied):\n%s", d);
            free(d);
        }
        json_free(res);
    } else if (!strcmp(action, "rename_file")) {
        const char *nn = json_get_str(args, "new_name");
        if (!nn) {
            tool_result_error(out, "rename_file requires new_name");
            goto done;
        }
        char *dest = tool_resolve_path(lt->env, nn);
        char *duri = path_to_uri(dest);
        json_t *p = json_obj(), *files = json_arr(), *f = json_obj();
        json_set_str(f, "oldUri", uri);
        json_set_str(f, "newUri", duri);
        json_push(files, f);
        json_set(p, "files", files);
        json_t *res = request(c, "workspace/willRenameFiles", p, to, &err);
        free(err);
        err = NULL;
        if (json_is_obj(res)) apply_workspace_edit(res, &b);
        json_free(res);
        char *dir = path_dirname(dest);
        fs_mkdir_p(dir, 0755);
        free(dir);
        if (rename(abs, dest) != 0) tool_result_error(out, "rename failed: %s", strerror(errno));
        else {
            json_t *p2 = json_obj(), *files2 = json_arr(), *f2 = json_obj();
            json_set_str(f2, "oldUri", uri);
            json_set_str(f2, "newUri", duri);
            json_push(files2, f2);
            json_set(p2, "files", files2);
            notify(c, "workspace/didRenameFiles", p2);
            tool_result_textf(out, "Moved %s -> %s%s%s", disp, nn, b.len ? "\nUpdated references:\n" : "", sb_cstr(&b));
        }
        free(duri);
        free(dest);
    } else if (!strcmp(action, "code_actions")) {
        json_t *p = json_obj(), *td = json_obj(), *range = json_obj(), *st = json_obj(), *en = json_obj();
        json_set_str(td, "uri", uri);
        json_set(p, "textDocument", td);
        json_set_int(st, "line", line1 - 1);
        json_set_int(st, "character", 0);
        json_set_int(en, "line", line1 - 1);
        json_set_int(en, "character", 10000);
        json_set(range, "start", st);
        json_set(range, "end", en);
        json_set(p, "range", range);
        json_t *ctxj = json_obj();
        json_t *diags = json_arr(), *all = wait_diags(c, uri, seq, 1500), *d;
        json_arr_foreach(all, i, d) {
            if (json_get_int(json_get(json_get(d, "range"), "start"), "line", -1) == line1 - 1) json_push(diags, json_clone(d));
        }
        json_free(all);
        json_set(ctxj, "diagnostics", diags);
        json_set(p, "context", ctxj);
        json_t *res = request(c, "textDocument/codeAction", p, to, &err);
        if (!res) {
            tool_result_error(out, "%s", err);
            goto done;
        }
        const char *q = json_get_str(args, "query");
        bool apply = json_get_bool(args, "apply", false);
        json_t *x, *chosen = NULL;
        json_arr_foreach(res, i, x) {
            const char *title = json_get_str(x, "title");
            sb_appendf(&b, "%zu. %s%s%s\n", i, title ? title : "?", json_get_str(x, "kind") ? " [" : "",
                       json_get_str(x, "kind") ? json_get_str(x, "kind") : "");
            if (json_get_str(x, "kind")) {
                b.len--;
                sb_append(&b, "]\n");
            }
            if (q && !chosen && ((title && str_istr(title, q)) || (strspn(q, "0123456789") == strlen(q) && (size_t)atoi(q) == i))) chosen = x;
        }
        if (!apply) {
            tool_result_text(out, b.len ? sb_cstr(&b) : "No code actions available here");
        } else if (!chosen) {
            tool_result_error(out, "No code action matches query `%s`:\n%s", q ? q : "", sb_cstr(&b));
        } else {
            sb_t s = SB_INIT;
            json_t *edit = json_get(chosen, "edit");
            json_t *resolved = NULL;
            if (!edit && json_get(json_get(c->caps, "codeActionProvider"), "resolveProvider")) {
                char *e3 = NULL;
                resolved = request(c, "codeAction/resolve", json_clone(chosen), to, &e3);
                free(e3);
                edit = json_get(resolved, "edit");
            }
            int rc = edit ? apply_workspace_edit(edit, &s) : 0;
            json_t *cmd = json_get(chosen, "command");
            if (json_is_obj(cmd)) {
                char *e3 = NULL;
                json_t *pp = json_obj();
                json_set_str(pp, "command", json_get_str(cmd, "command"));
                json_set(pp, "arguments", json_clone(json_get(cmd, "arguments")));
                json_t *r3 = request(c, "workspace/executeCommand", pp, to, &e3);
                json_free(r3);
                free(e3);
            }
            tool_result_textf(out, "Applied: %s\n%s", json_get_str(chosen, "title"), sb_cstr(&s));
            out->is_error = rc != 0;
            sb_free(&s);
            json_free(resolved);
        }
        json_free(res);
    } else if (!strcmp(action, "request")) {
        const char *method = json_get_str(args, "query");
        if (!method) {
            tool_result_error(out, "request requires query (the LSP method)");
            goto done;
        }
        json_t *p = NULL;
        const char *payload = json_get_str(args, "payload");
        if (payload) {
            p = json_parse_str(payload);
            if (!p) {
                tool_result_error(out, "payload is not valid JSON");
                goto done;
            }
        } else {
            int64_t col = 0;
            resolve_position(abs, line1, json_get_str(args, "symbol"), &col, &err);
            free(err);
            err = NULL;
            p = pos_params(uri, line1, col);
        }
        json_t *res = request(c, method, p, to, &err);
        if (!res) tool_result_error(out, "%s", err);
        else {
            char *d = json_dump(res, 2);
            tool_result_text(out, d);
            free(d);
        }
        json_free(res);
    } else {
        tool_result_error(out, "Unknown action: %s", action);
    }
done:
    free(err);
    sb_free(&b);
    free(uri);
    free(disp);
    free(abs);
    return 0;
}

static void lsp_tool_destroy(agent_tool_t *t) { free(t->ud); }

agent_tool_t *tool_lsp_new(tool_env_t *env, lsp_manager_t *m) {
    json_t *ctx = json_obj();
    json_set_int(ctx, "defaultTimeoutSec", (int64_t)lsp_default_call_sec());
    json_set_int(ctx, "defaultWaitSec", settings_int("lsp.requestTimeout", 10));
    json_set_int(ctx, "maxTimeoutSec", 300);
    char *desc = tool_prompt("lsp", ctx);
    json_free(ctx);
    json_t *s = schema_lit(
        "{\"type\":\"object\",\"properties\":{\"action\":{\"enum\":[\"diagnostics\",\"definition\",\"references\","
        "\"hover\",\"symbols\",\"rename\",\"rename_file\",\"code_actions\",\"type_definition\",\"implementation\","
        "\"status\",\"reload\",\"capabilities\",\"request\"],\"type\":\"string\"},\"file\":{\"type\":\"string\"},"
        "\"line\":{\"type\":\"number\"},\"symbol\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"},"
        "\"new_name\":{\"type\":\"string\"},\"apply\":{\"type\":\"boolean\"},\"timeout\":{\"type\":\"number\","
        "\"minimum\":5,\"maximum\":300,\"description\":\"Timeout in seconds; raises or lowers the default (range "
        "5\u2013300).\"},\"payload\":{\"type\":\"string\"}},\"required\":[\"action\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("lsp", "LSP", desc, s, lsp_execute);
    free(desc);
    lsp_tool_t *lt = xcalloc(1, sizeof(*lt));
    lt->env = env;
    lt->m = m;
    t->ud = lt;
    t->destroy = lsp_tool_destroy;
    return t;
}
