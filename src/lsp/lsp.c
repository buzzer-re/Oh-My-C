/* lsp.c - Language Server Protocol client manager and the `lsp` tool.
 * Port of omp's lsp/{config,client,diagnostics,edits,utils,servers,tool,workspace-diagnostics,writethrough}.ts:
 * the text returned to the model is byte-identical to omp's for every action. */
#define _GNU_SOURCE
#include "lsp.h"
#include "tui/theme.h"

#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "core/proc.h"
#include "core/rpcframe.h"
#include "text/glob.h"
#include "text/walker.h"
#include "tui/symbols.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------- constants (diagnostics.ts / client.ts) ---------- */

#define SINGLE_DIAGNOSTICS_WAIT_TIMEOUT_MS 3000
#define BATCH_DIAGNOSTICS_WAIT_TIMEOUT_MS 400
#define DIAGNOSTICS_POLL_MS 100
#define BUILD_STATUS_POLL_MS 20
#define DIAGNOSTICS_SETTLE_MS 250
#define DEFERRED_DIAGNOSTICS_WAIT_TIMEOUT_MS 12000
#define MAX_GLOB_DIAGNOSTIC_TARGETS 20
#define WORKSPACE_SYMBOL_LIMIT 200
#define REFERENCE_CONTEXT_LIMIT 50
#define REFERENCES_RETRY_COUNT 2
#define REFERENCES_RETRY_DELAY_MS 250
#define DIAGNOSTIC_MESSAGE_LIMIT 50
#define PROJECT_LOAD_TIMEOUT_MS 15000
#define MAX_RENAME_PAIRS 1000
#define WRITETHROUGH_DIAGNOSTICS_WAIT_MS 3000

/* ---------- types ---------- */

typedef struct {
    int64_t id;
    json_t *resp;
    bool done;
} lpend_t;

typedef struct {
    char *uri;
    char *lang;
    int64_t version;
    uint64_t hash;
    int64_t reopened_at; /* 0 = not reopened */
    int64_t last_used;   /* mono_ms of the last use by an operation; a sweep over every open document is not one */
} openf_t;

typedef struct lsp_client {
    char *name;   /* server name (config key) */
    json_t *cfg;  /* server config (with resolvedCommand) */
    char *root;   /* cwd */
    child_t child;
    pthread_t reader;
    bool reader_started;
    volatile bool alive;
    int exit_code;
    char *close_error; /* reader failure ("LSP connection closed: ..."), owned */
    sb_t stderr_tail;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    VEC(lpend_t *) pending;
    int64_t next_id;
    json_t *caps;         /* server capabilities */
    json_t *diags;        /* path -> {items:[...], version:n|null}: what readers see */
    json_t *pub_diags;    /* path -> record delivered by textDocument/publishDiagnostics */
    json_t *pull_diags;   /* path -> record delivered by a textDocument/diagnostic pull */
    int64_t diag_version; /* bumps on every publishDiagnostics */
    bool publishes_versions;
    json_t *build_status; /* path -> {busy, idle} (clangd.fileStatus) or NULL */
    int64_t build_seq;
    json_t *registrations; /* id -> method */
    json_t *progress;      /* active progress tokens */
    bool project_loaded;
    int64_t project_deadline;
    int64_t started_at;
    VEC(openf_t) open;
    json_t *idle_closed; /* path -> version of documents closed for reopen */
    const char *status;  /* "connecting" | "ready" | "error" */
    /* rust-analyzer only: the crate manifests present at initialize (listCargoManifests), listed off the call's thread */
    pthread_t cargo_th;
    bool cargo_started;
    char *cargo_manifests;
} lsp_client_t;

struct lsp_manager {
    char *cwd;
    json_t *servers; /* name -> config: configured, root markers present, binary resolved (lsp/config.ts loadConfig) */
    /* servers whose root markers are not in the cwd itself (a monorepo package): each joins `servers` the first time a
     * file is looked up below a directory holding one of its markers; and servers whose binary does not resolve */
    json_t *nested, *unresolved;
    VEC(lsp_client_t *) clients;
    json_t *init_failures; /* name -> message */
    json_t *ledger;        /* path -> [identities] (diagnostics-ledger.ts) */
    pthread_mutex_t mu;
};

/* ---------- small helpers ---------- */

typedef struct {
    int64_t sl, sc, el, ec;
    const char *text;
    size_t index;
} tedit_t;
typedef VEC(tedit_t) tedits_t;
typedef VEC(json_t *) jptrs_t;
typedef VEC(size_t) sizes_t;

static const char *sym(const char *key, const char *fallback) {
    /* statusSymbol (omp 290d787845): the theme is only there once the TUI loads it, so headless runs use plain glyphs */
    if (!tui_theme_loaded()) {
        if (!strcmp(key, "status.success")) return "\xe2\x9c\x93";
        if (!strcmp(key, "status.warning")) return "\xe2\x9a\xa0";
        if (!strcmp(key, "status.error")) return "\xe2\x9c\x97";
    }
    const char *s = tui_sym(key);
    return s && *s ? s : fallback;
}

static uint64_t text_hash(const char *s, size_t n) { return hash_fnv1a(s, n); }


/* JS String(value) of a JSON scalar */
static char *js_string(const json_t *j) {
    if (!j) return xstrdup("undefined");
    if (json_is_str(j)) return xstrdup(json_sv(j));
    return json_dump(j, 0);
}

/* path.resolve(cwd, p) after ~ expansion (path-utils.ts resolveToCwd) */
static char *resolve_to_cwd(const char *p, const char *cwd) {
    char *e = path_expand_home(p);
    bool all_slash = *e && strspn(e, "/") == strlen(e);
    char *r = all_slash ? xstrdup(cwd) : path_abs(e, cwd);
    free(e);
    return r;
}

/* formatPathRelativeToCwd: cwd-relative when inside cwd ("." for cwd itself), else absolute */
static char *rel_to_cwd(const char *p, const char *cwd) {
    char *abs = path_abs(p, cwd);
    char *c = path_normalize(cwd);
    size_t cl = strlen(c);
    char *r;
    if (!strcmp(abs, c)) r = xstrdup(".");
    else if (cl == 1 && c[0] == '/') r = xstrdup(abs + 1);
    else if (!strncmp(abs, c, cl) && abs[cl] == '/') r = xstrdup(abs + cl + 1);
    else r = xstrdup(abs);
    free(c);
    free(abs);
    return r;
}

/* Bun.pathToFileURL(path.resolve(p)).href */
static char *path_to_uri(const char *p) {
    sb_t b = SB_INIT;
    sb_append(&b, "file://");
    for (const unsigned char *s = (const unsigned char *)p; *s; s++) {
        if (*s <= 0x20 || *s >= 0x7f || strchr("\"#%<>?`{}", *s)) sb_appendf(&b, "%%%02X", *s);
        else sb_putc(&b, (char)*s);
    }
    return sb_detach(&b);
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* uriToFile */
static char *uri_to_path(const char *uri) {
    if (!uri) return xstrdup("");
    if (!str_starts(uri, "file://")) return xstrdup(uri);
    const char *s = uri + 7;
    if (!strncmp(s, "localhost/", 10)) s += 9;
    sb_t b = SB_INIT;
    for (; *s; s++) {
        int h, l;
        if (*s == '%' && (h = hexv(s[1])) >= 0 && (l = hexv(s[2])) >= 0) {
            sb_putc(&b, (char)(h * 16 + l));
            s += 2;
        } else sb_putc(&b, *s);
    }
    return sb_detach(&b);
}

/* EquivalentUriMap key */
static char *uri_key(const char *uri) {
    if (!str_starts(uri, "file://")) return xstrdup(uri);
    char *p = uri_to_path(uri);
    char *n = path_normalize(p);
    free(p);
    return n;
}

/* lang-from-path.ts detectLanguageId */
static const char *language_id(const char *path) {
    static const char *const map[][2] = {
        {"ts", "typescript"}, {"cts", "typescript"}, {"mts", "typescript"}, {"tsx", "typescriptreact"},
        {"js", "javascript"}, {"jsx", "javascriptreact"}, {"mjs", "javascript"}, {"cjs", "javascript"},
        {"rs", "rust"}, {"go", "go"}, {"c", "c"}, {"h", "c"}, {"cpp", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"},
        {"hh", "cpp"}, {"hpp", "cpp"}, {"hxx", "cpp"}, {"cu", "cpp"}, {"cuh", "cpp"}, {"ino", "cpp"}, {"zig", "zig"},
        {"py", "python"}, {"pyi", "python"}, {"rb", "ruby"}, {"rbw", "ruby"}, {"gemspec", "ruby"}, {"erb", "erb"}, {"lua", "lua"},
        {"sh", "shellscript"}, {"bash", "shellscript"}, {"zsh", "shellscript"}, {"ksh", "shellscript"},
        {"bats", "shellscript"}, {"tmux", "shellscript"}, {"cgi", "shellscript"}, {"fcgi", "shellscript"},
        {"command", "shellscript"}, {"tool", "shellscript"}, {"fish", "fish"}, {"pl", "perl"}, {"pm", "perl"},
        {"perl", "perl"}, {"php", "php"}, {"java", "java"}, {"kt", "kotlin"}, {"ktm", "kotlin"}, {"kts", "kotlin"},
        {"scala", "scala"}, {"sc", "scala"}, {"sbt", "scala"}, {"groovy", "groovy"}, {"clj", "clojure"},
        {"cljc", "clojure"}, {"cljs", "clojure"}, {"edn", "clojure"}, {"el", "emacs-lisp"}, {"cs", "csharp"},
        {"fs", "fsharp"}, {"vb", "vb"}, {"html", "html"}, {"htm", "html"}, {"xhtml", "html"}, {"css", "css"},
        {"scss", "scss"}, {"sass", "sass"}, {"less", "less"}, {"vue", "vue"}, {"svelte", "svelte"}, {"astro", "astro"},
        {"json", "json"}, {"jsonc", "jsonc"}, {"yaml", "yaml"}, {"yml", "yaml"}, {"toml", "toml"}, {"xml", "xml"},
        {"xsl", "xml"}, {"xslt", "xml"}, {"svg", "xml"}, {"plist", "xml"}, {"ini", "ini"}, {"md", "markdown"},
        {"markdown", "markdown"}, {"mdx", "markdown"}, {"mdc", "markdown"}, {"mkd", "markdown"}, {"mdown", "markdown"},
        {"rst", "restructuredtext"}, {"adoc", "asciidoc"}, {"tex", "latex"}, {"sql", "sql"}, {"graphql", "graphql"},
        {"gql", "graphql"}, {"proto", "protobuf"}, {"dockerfile", "dockerfile"}, {"containerfile", "dockerfile"},
        {"tf", "terraform"}, {"hcl", "hcl"}, {"tfvars", "hcl"}, {"nix", "nix"}, {"ex", "elixir"}, {"exs", "elixir"},
        {"erl", "erlang"}, {"hrl", "erlang"}, {"hs", "haskell"}, {"ml", "ocaml"}, {"mli", "ocaml"}, {"swift", "swift"},
        {"r", "r"}, {"jl", "julia"}, {"dart", "dart"}, {"elm", "elm"}, {"v", "v"}, {"nim", "nim"}, {"cr", "crystal"},
        {"d", "d"}, {"pas", "pascal"}, {"pp", "pascal"}, {"lisp", "lisp"}, {"lsp", "lisp"}, {"rkt", "racket"},
        {"scm", "scheme"}, {"ps1", "powershell"}, {"psm1", "powershell"}, {"bat", "bat"}, {"cmd", "bat"},
        {"tla", "tlaplus"}, {"tlaplus", "tlaplus"}, {"cmake", "cmake"}};
    const char *base = path_basename(path);
    if (!strcasecmp(base, "dockerfile") || !strncasecmp(base, "dockerfile.", 11) || !strcasecmp(base, "containerfile"))
        return "dockerfile";
    if (!strcasecmp(base, ".emacs")) return "emacs-lisp";
    if (!strcasecmp(base, "makefile") || !strcasecmp(base, "gnumakefile")) return "makefile";
    if (!strcasecmp(base, "justfile")) return "just";
    const char *e = path_ext(path);
    const char *ext = *e == '.' ? e + 1 : "";
    if (!strcasecmp(base, "cmakelists.txt")) return "cmake";
    for (size_t i = 0; i < ARRAY_LEN(map); i++)
        if (!strcasecmp(ext, map[i][0])) return map[i][1];
    return "plaintext";
}

/* ---------- config (lsp/config.ts) ---------- */

static json_t *parse_config_file(const char *path) {
    if (!fs_is_file(path)) return NULL;
    json_t *j = NULL;
    const char *ext = path_ext(path);
    if (!strcasecmp(ext, ".yaml") || !strcasecmp(ext, ".yml")) {
        char *err = NULL;
        extern json_t *yaml_parse_file_json(const char *p, char **e);
        j = yaml_parse_file_json(path, &err);
        free(err);
    } else {
        size_t n;
        char *s = fs_read(path, &n);
        if (s) j = json_parse(s, n, NULL);
        free(s);
    }
    if (!json_is_obj(j)) {
        json_free(j);
        return NULL;
    }
    json_t *servers = json_get_obj(j, "servers");
    json_t *r = servers ? json_clone(servers) : json_clone(j);
    json_del(r, "idleTimeoutMs");
    json_free(j);
    return r;
}

static bool nonempty_str_array(const json_t *a) {
    json_t *x;
    json_arr_foreach(a, i, x) if (json_is_str(x) && *json_sv(x)) return true;
    return false;
}

/* normalizeServerConfig: NULL when command/fileTypes/rootMarkers are missing */
static json_t *normalize_server(const json_t *raw) {
    if (!json_is_obj(raw)) return NULL;
    const char *cmd = json_get_str(raw, "command");
    json_t *ft = json_get(raw, "fileTypes");
    json_t *etl = json_get_obj(raw, "extensionToLanguage");
    bool has_ft = nonempty_str_array(ft) || (etl && json_len(etl));
    bool has_rm = nonempty_str_array(json_get(raw, "rootMarkers")) || etl;
    if (!cmd || !*cmd || !has_ft || !has_rm) return NULL;
    json_t *c = json_clone(raw);
    if (!nonempty_str_array(ft)) {
        json_t *a = json_arr();
        const char *k;
        json_t *v;
        json_obj_foreach(etl, i, k, v) if (*k) json_push_str(a, k);
        json_set(c, "fileTypes", a);
    }
    if (!nonempty_str_array(json_get(raw, "rootMarkers"))) {
        json_t *a = json_arr();
        json_push_str(a, ".");
        json_set(c, "rootMarkers", a);
    }
    if (!json_get_obj(c, "initOptions") && json_get_obj(c, "initializationOptions"))
        json_set(c, "initOptions", json_clone(json_get(c, "initializationOptions")));
    return c;
}

static void merge_servers(json_t *merged, const json_t *overrides) {
    const char *k;
    json_t *v;
    json_obj_foreach(overrides, i, k, v) {
        json_t *cur = json_get(merged, k);
        json_t *cand;
        if (cur) {
            cand = json_clone(cur);
            if (json_is_obj(v)) {
                const char *kk;
                json_t *vv;
                json_obj_foreach(v, j, kk, vv) json_set(cand, kk, json_clone(vv));
            }
        } else cand = json_clone(v);
        json_t *n = normalize_server(cand);
        json_free(cand);
        if (n) json_set(merged, k, n);
    }
}

/* hasRootMarkers: any marker exists directly in dir (globs match one readdir level) */
static bool has_root_markers(const char *dir, const json_t *markers) {
    dirlist_t dl = {0};
    bool listed = false, hit = false;
    json_t *m;
    json_arr_foreach(markers, i, m) {
        const char *mk = json_sv(m);
        if (!mk || hit) continue;
        if (strchr(mk, '*')) {
            if (!listed) {
                listed = true;
                fs_list_dir(dir, &dl);
            }
            for (size_t k = 0; k < dl.n && !hit; k++) hit = glob_match(mk, dl.v[k].name, 0);
        } else {
            char *p = path_join(dir, mk);
            hit = fs_exists(p);
            free(p);
        }
    }
    if (listed) dirlist_free(&dl);
    return hit;
}

/* resolveCommand: project-local bin dirs, then $PATH */
static char *resolve_command(const char *command, const char *cwd) {
    static const char *const py[] = {"pyproject.toml", "ty.toml", "requirements.txt", "setup.py", "setup.cfg",
                                     "Pipfile", "pyrightconfig.json", "ruff.toml", ".ruff.toml", NULL};
    static const char *const node[] = {"package.json", "package-lock.json", "yarn.lock", "pnpm-lock.yaml", NULL};
    static const char *const rb[] = {"Gemfile", "Gemfile.lock", NULL};
    static const char *const go[] = {"go.mod", "go.sum", "go.work", NULL};
    static const struct {
        const char *const *markers;
        const char *bin;
    } local[] = {{node, "node_modules/.bin"}, {py, ".venv/bin"}, {py, ".venv/Scripts"}, {py, "venv/bin"},
                 {py, "venv/Scripts"},     {py, ".env/bin"},   {py, ".env/Scripts"}, {rb, "vendor/bundle/bin"},
                 {rb, "bin"},              {go, "bin"}};
    if (strchr(command, '/')) {
        char *abs = path_abs(command, cwd);
        if (access(abs, X_OK) == 0) return abs;
        free(abs);
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_LEN(local); i++) {
        bool any = false;
        for (const char *const *m = local[i].markers; *m && !any; m++) {
            char *p = path_join(cwd, *m);
            any = fs_exists(p);
            free(p);
        }
        if (!any) continue;
        char *p = path_join3(cwd, local[i].bin, command);
        if (fs_exists(p)) return p;
        free(p);
    }
    return which(command);
}

/* typescriptPackageDir: the npm package a `tsc` belongs to, owned or NULL */
static char *typescript_package_dir(const char *tsc) {
    char *real = path_real(tsc);
    char *bin_dir = path_dirname(tsc);
    char *found = NULL;
    if (real) {
        char *rb = path_dirname(real);
        if (!strcmp(path_basename(rb), "bin")) { /* <pkg>/bin/tsc: symlinked node_modules/.bin and global installs */
            char *pkg = path_dirname(rb), *pj = path_join(pkg, "package.json");
            if (fs_exists(pj)) found = xstrdup(pkg);
            free(pj);
            free(pkg);
        }
        free(rb);
        free(real);
    }
    for (int i = 0; i < 2 && !found; i++) {
        char *pkg = i == 0 ? path_join3(bin_dir, "..", "typescript") : path_join3(bin_dir, "node_modules", "typescript");
        char *pj = path_join(pkg, "package.json");
        if (fs_exists(pj)) found = xstrdup(pkg);
        free(pj);
        free(pkg);
    }
    free(bin_dir);
    return found;
}

/* typescriptSpeaksLsp: TypeScript 7 dropped the JS lib/tsserver.js that typescript-language-server wraps and exposes
 * `tsc --lsp --stdio` instead; older releases reject the flag. Outside an npm layout (e.g. nixpkgs) ask the compiler for its
 * version, run in `cwd` so version-manager shims resolve the project's pinned TypeScript. */
static bool typescript_speaks_lsp(const char *tsc, const char *cwd) {
    char *pkg = typescript_package_dir(tsc);
    if (pkg) {
        char *ts = path_join3(pkg, "lib", "tsserver.js");
        bool speaks = !fs_exists(ts);
        free(ts);
        free(pkg);
        return speaks;
    }
    const char *argv[] = {tsc, "--version", NULL};
    proc_opts_t o = {.cwd = cwd, .timeout_ms = 3000, .max_output = 4096};
    proc_result_t r;
    memset(&r, 0, sizeof r);
    bool speaks = false;
    if (proc_run(argv, &o, &r) == 0 && r.out && !strncmp(r.out, "Version ", 8) && isdigit((unsigned char)r.out[8])) {
        char *end;
        long major = strtol(r.out + 8, &end, 10);
        speaks = *end == '.' && major >= 7;
    }
    proc_result_free(&r);
    return speaks;
}

static json_t *load_config(const char *cwd, json_t **nested_out, json_t **unresolved_out) {
    size_t n;
    const char *d = embed_get("assets/lsp-servers.json", &n);
    json_t *defaults = d ? json_parse(d, n, NULL) : NULL;
    json_t *merged = json_obj();
    if (defaults) merge_servers(merged, defaults);
    json_free(defaults);
    /* configFileSources("lsp"), lowest priority first: home, user dirs, project dirs, cwd */
    static const char *const names[] = {".lsp.yml", "lsp.yml", ".lsp.yaml", "lsp.yaml", ".lsp.json", "lsp.json"};
    strvec_t dirs = {0};
    strvec_push(&dirs, home_dir());
    char *cl = path_join(home_dir(), ".claude");
    strvec_push_own(&dirs, cl);
    strvec_push(&dirs, omc_agent_dir());
    static const char *const pdirs[] = {".claude", ".pi", ".omp", ".omc"};
    for (size_t k = 0; k < ARRAY_LEN(pdirs); k++) strvec_push_own(&dirs, path_join(cwd, pdirs[k]));
    strvec_push(&dirs, cwd);
    for (size_t k = 0; k < dirs.n; k++) {
        for (size_t i = 0; i < ARRAY_LEN(names); i++) {
            char *p = path_join(dirs.v[k], names[i]);
            json_t *s = parse_config_file(p);
            if (s) merge_servers(merged, s);
            json_free(s);
            free(p);
        }
    }
    strvec_free(&dirs);
    json_t *s2 = settings_get("lsp.servers");
    if (json_is_obj(s2)) merge_servers(merged, s2);
    json_t *servers = json_obj(), *nested = json_obj(), *unresolved = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(merged, i, k, v) {
        if (json_get_bool(v, "disabled", false)) continue;
        if (!has_root_markers(cwd, json_get_arr(v, "rootMarkers"))) {
            if (json_len(json_get_arr(v, "rootMarkers"))) json_set(nested, k, json_clone(v));
            continue;
        }
        char *res = resolve_command(json_get_str(v, "command"), cwd);
        if (!res) {
            json_set(unresolved, k, json_clone(v));
            continue;
        }
        json_t *c = json_clone(v);
        json_set_str(c, "resolvedCommand", res);
        free(res);
        json_set(servers, k, c);
    }
    json_free(merged);
    /* selectTypescriptServer: keep one TypeScript server (tsc --lsp needs a TypeScript 7 install) */
    json_t *native = json_get(servers, "typescript-native");
    if (native) {
        const char *tsc = json_get_str(native, "resolvedCommand");
        bool speaks = tsc && typescript_speaks_lsp(tsc, cwd);
        json_del(servers, speaks ? "typescript-language-server" : "typescript-native");
    }
    if (nested_out) *nested_out = nested;
    else json_free(nested);
    if (unresolved_out) *unresolved_out = unresolved;
    else json_free(unresolved);
    return servers;
}

static bool is_linter(const json_t *cfg) { return json_get_bool(cfg, "isLinter", false); }
static bool has_custom_client(const char *name) { return str_eq(name, "biome") || str_eq(name, "swiftlint"); }
/* isProjectAwareLspServer: !createClient && !isLinter */
static bool project_aware(const char *name, const json_t *cfg) { return !has_custom_client(name) && !is_linter(cfg); }

static bool server_handles(const json_t *cfg, const char *path) {
    const char *base = path_basename(path), *ext = path_ext(path);
    const char *extnd = *ext == '.' ? ext + 1 : ext;
    json_t *x;
    json_arr_foreach(json_get_arr(cfg, "fileTypes"), i, x) {
        const char *f = json_sv(x);
        if (!f) continue;
        const char *fnd = *f == '.' ? f + 1 : f;
        if (!strcasecmp(f, ext) || !strcasecmp(f, base) || !strcasecmp(fnd, extnd) || !strcasecmp(fnd, base)) return true;
    }
    return false;
}

typedef VEC(const char *) names_t;

/* the nearest directory from `path` up to (not including) `cwd` that holds one of `markers`, owned or NULL */
static char *nested_marker_dir(const char *path, const char *cwd, const json_t *markers) {
    size_t rl = strlen(cwd);
    char *dir = path_dirname(path);
    while (strcmp(dir, cwd) && !strncmp(dir, cwd, rl) && dir[rl] == '/') {
        if (has_root_markers(dir, markers)) return dir;
        char *up = path_dirname(dir);
        free(dir);
        dir = up;
    }
    free(dir);
    return NULL;
}

/* activateNestedServers (omp 290d787845): enable the nested servers that claim `path` and have a root marker between it
 * and the session cwd, resolving their binary from that project first. Caller holds no lock on servers. */
static void activate_nested(lsp_manager_t *m, const char *path) {
    if (!m->nested || !json_len(m->nested)) return;
    json_t *activated = json_obj();
    char *project = NULL;
    strvec_t done = {0};
    const char *k;
    json_t *v;
    json_obj_foreach(m->nested, i, k, v) {
        if (!server_handles(v, path)) continue;
        char *dir = nested_marker_dir(path, m->cwd, json_get_arr(v, "rootMarkers"));
        if (!dir) continue;
        strvec_push(&done, k);
        char *res = resolve_command(json_get_str(v, "command"), dir);
        if (res && !strchr(json_get_str(v, "command"), '/') && !str_starts(res, dir)) {
            /* the project's own bins first, then the session's (localRoots [markerDir, cwd]) */
            char *local = resolve_command(json_get_str(v, "command"), m->cwd);
            if (local && str_starts(local, m->cwd)) {
                free(res);
                res = local;
            } else {
                free(local);
            }
        }
        if (!res) {
            if (!m->unresolved) m->unresolved = json_obj();
            json_set(m->unresolved, k, json_clone(v));
            free(dir);
            continue;
        }
        json_t *c = json_clone(v);
        json_set_str(c, "resolvedCommand", res);
        free(res);
        json_set(activated, k, c);
        if (!project) project = dir;
        else free(dir);
    }
    for (size_t i = 0; i < done.n; i++) json_del(m->nested, done.v[i]);
    strvec_free(&done);
    if (project) {
        json_t *native = json_get(activated, "typescript-native");
        if (native) {
            const char *tsc = json_get_str(native, "resolvedCommand");
            bool speaks = tsc && typescript_speaks_lsp(tsc, project);
            json_del(activated, speaks ? "typescript-language-server" : "typescript-native");
        }
        json_obj_foreach(activated, i, k, v) json_set(m->servers, k, json_clone(v));
    }
    free(project);
    json_free(activated);
}

/* describeMissingServers: why no server handles `path` (owned) */
static char *describe_missing(lsp_manager_t *m, const char *path) {
    const char *ext = *path_ext(path) ? path_ext(path) : path_basename(path);
    strvec_t reasons = {0};
    const char *k;
    json_t *v;
    json_obj_foreach(m->unresolved, i, k, v) if (server_handles(v, path))
        strvec_push_own(&reasons, xasprintf("%s: `%s` not found on PATH", k, json_get_str(v, "command")));
    json_obj_foreach(m->nested, i, k, v) if (server_handles(v, path)) {
        strvec_t mk = {0};
        json_t *x;
        json_arr_foreach(json_get_arr(v, "rootMarkers"), q, x) if (json_is_str(x)) strvec_push(&mk, json_sv(x));
        char *ms = strvec_join(&mk, ", ");
        strvec_push_own(&reasons, xasprintf("%s: no %s in the file's directory or its parents", k, ms));
        free(ms);
        strvec_free(&mk);
    }
    char *r;
    if (!reasons.n) {
        r = xasprintf("No language server handles %s files", ext);
    } else {
        sb_t b = SB_INIT;
        sb_appendf(&b, "No language server for %s:", ext);
        for (size_t i = 0; i < reasons.n; i++) sb_appendf(&b, "\n  %s", reasons.v[i]);
        r = sb_detach(&b);
    }
    strvec_free(&reasons);
    return r;
}

/* getServersForFile: primary servers first, then linters (stable) */
static void servers_for_file(const json_t *servers, const char *path, names_t *out, bool lsp_only) {
    for (int pass = 0; pass < 2; pass++) {
        const char *k;
        json_t *v;
        json_obj_foreach(servers, i, k, v) {
            if (is_linter(v) != (pass == 1)) continue;
            if (lsp_only && has_custom_client(k)) continue;
            if (server_handles(v, path)) vec_push(out, k);
        }
    }
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

static void respond_error(lsp_client_t *c, const json_t *id, int code, const char *message) {
    json_t *r = json_obj(), *e = json_obj();
    json_set_str(r, "jsonrpc", "2.0");
    json_set(r, "id", json_clone(id));
    json_set_int(e, "code", code);
    json_set_str(e, "message", message);
    json_set(r, "error", e);
    send_json(c, r);
    json_free(r);
}

static int apply_workspace_edit(const json_t *edit, const char *cwd, strvec_t *applied, char **err);

static bool supports_pull(lsp_client_t *c);

static char *diagnostic_fingerprint(const json_t *d) {
    const json_t *r = json_get(d, "range"), *st = json_get(r, "start"), *en = json_get(r, "end");
    const char *src = json_get_str(d, "source"), *msg = json_get_str(d, "message");
    return xasprintf("%lld:%lld:%lld:%lld:%s:%s", (long long)json_get_int(st, "line", 0), (long long)json_get_int(st, "character", 0),
                     (long long)json_get_int(en, "line", 0), (long long)json_get_int(en, "character", 0), src ? src : "",
                     msg ? msg : "undefined");
}

/* mergeDiagnosticLists: primary then secondary, in order, deduplicated by range, source and message (owned) */
static json_t *merge_diagnostic_lists(const json_t *primary, const json_t *secondary) {
    if (!json_len(primary)) return json_is_arr(secondary) ? json_clone(secondary) : json_arr();
    if (!json_len(secondary)) return json_clone(primary);
    json_t *out = json_arr(), *d;
    strvec_t seen = {0};
    const json_t *lists[] = {primary, secondary};
    for (int l = 0; l < 2; l++)
        json_arr_foreach(lists[l], i, d) {
            char *k = diagnostic_fingerprint(d);
            if (strvec_contains(&seen, k)) free(k);
            else {
                strvec_push_own(&seen, k);
                json_push(out, json_clone(d));
            }
        }
    strvec_free(&seen);
    return out;
}

static void handle_notification(lsp_client_t *c, const char *method, json_t *params) {
    if (!strcmp(method, "textDocument/publishDiagnostics")) {
        const char *uri = json_get_str(params, "uri");
        if (!uri) return;
        char *key = uri_key(uri);
        json_t *e = json_obj();
        json_t *items = json_get(params, "diagnostics");
        json_set(e, "items", json_is_arr(items) ? json_clone(items) : json_arr());
        json_t *ver = json_get(params, "version");
        json_set(e, "version", json_is_num(ver) ? json_clone(ver) : json_null());
        bool dual = supports_pull(c);
        pthread_mutex_lock(&c->mu);
        json_set(c->pub_diags, key, json_clone(e));
        if (dual) { /* a dual-mode server: the pulled report plus what was pushed */
            json_t *combined = merge_diagnostic_lists(json_get(json_get(c->pull_diags, key), "items"), json_get(e, "items"));
            json_set(e, "items", combined);
        }
        json_set(c->diags, key, e);
        c->diag_version++;
        if (json_is_num(ver)) c->publishes_versions = true;
        pthread_cond_broadcast(&c->cv);
        pthread_mutex_unlock(&c->mu);
        free(key);
    } else if (!strcmp(method, "$/progress")) {
        json_t *tok = json_get(params, "token");
        const char *kind = json_get_str(json_get(params, "value"), "kind");
        if (!tok || !kind) return;
        char *t = js_string(tok);
        pthread_mutex_lock(&c->mu);
        if (!strcmp(kind, "begin")) json_set_bool(c->progress, t, true);
        else if (!strcmp(kind, "end")) {
            json_del(c->progress, t);
            if (!json_len(c->progress)) c->project_loaded = true;
        }
        pthread_cond_broadcast(&c->cv);
        pthread_mutex_unlock(&c->mu);
        free(t);
    } else if (!strcmp(method, "textDocument/clangd.fileStatus")) {
        const char *uri = json_get_str(params, "uri");
        if (!uri) return;
        char *key = uri_key(uri);
        pthread_mutex_lock(&c->mu);
        if (!c->build_status) c->build_status = json_obj();
        json_t *st = json_get(c->build_status, key);
        if (!st) {
            st = json_obj();
            json_set_int(st, "busy", 0);
            json_set_int(st, "idle", 0);
            json_set(c->build_status, key, st);
        }
        int64_t seq = ++c->build_seq;
        json_set_int(st, str_eq(json_get_str(params, "state"), "idle") ? "idle" : "busy", seq);
        pthread_cond_broadcast(&c->cv);
        pthread_mutex_unlock(&c->mu);
        free(key);
    }
}

static void handle_server_request(lsp_client_t *c, const char *method, json_t *id, json_t *params) {
    if (!strcmp(method, "workspace/configuration")) {
        json_t *res = json_arr(), *it;
        json_t *settings = json_get_obj(c->cfg, "settings");
        json_arr_foreach(json_get_arr(params, "items"), i, it) {
            const char *sec = json_get_str(it, "section");
            json_t *v = settings ? json_get(settings, sec ? sec : "") : NULL;
            json_push(res, v ? json_clone(v) : json_null());
        }
        respond(c, id, res);
    } else if (!strcmp(method, "workspace/workspaceFolders")) {
        json_t *arr = json_arr(), *f = json_obj();
        char *u = path_to_uri(c->root);
        json_set_str(f, "uri", u);
        json_set_str(f, "name", path_basename(c->root));
        free(u);
        json_push(arr, f);
        respond(c, id, arr);
    } else if (!strcmp(method, "workspace/applyEdit")) {
        json_t *edit = json_get(params, "edit");
        json_t *r = json_obj();
        if (!edit) {
            json_set_bool(r, "applied", false);
            json_set_str(r, "failureReason", "No edit provided");
        } else {
            strvec_t applied = {0};
            char *err = NULL;
            int rc = apply_workspace_edit(edit, c->root, &applied, &err);
            json_set_bool(r, "applied", rc == 0);
            if (rc) {
                char *fr = xasprintf("Error: %s", err ? err : "failed");
                json_set_str(r, "failureReason", fr);
                free(fr);
            }
            free(err);
            strvec_free(&applied);
        }
        respond(c, id, r);
    } else if (!strcmp(method, "client/registerCapability") || !strcmp(method, "client/unregisterCapability")) {
        pthread_mutex_lock(&c->mu);
        if (!strcmp(method, "client/registerCapability")) {
            json_t *r;
            json_arr_foreach(json_get_arr(params, "registrations"), i, r) {
                const char *rid = json_get_str(r, "id"), *rm = json_get_str(r, "method");
                if (rid && rm) json_set_str(c->registrations, rid, rm);
            }
        } else {
            json_t *un = json_get_arr(params, "unregisterations");
            if (!un) un = json_get_arr(params, "unregistrations");
            json_t *r;
            json_arr_foreach(un, i, r) if (json_get_str(r, "id")) json_del(c->registrations, json_get_str(r, "id"));
        }
        pthread_mutex_unlock(&c->mu);
        respond(c, id, NULL);
    } else if (!strcmp(method, "window/showDocument")) {
        json_t *r = json_obj();
        json_set_bool(r, "success", false);
        respond(c, id, r);
    } else if (!strcmp(method, "window/workDoneProgress/create") || !strcmp(method, "window/showMessageRequest") ||
               (str_starts(method, "workspace/") && str_ends(method, "/refresh"))) {
        respond(c, id, NULL);
    } else {
        char *m = xasprintf("Method not found: %s", method);
        respond_error(c, id, -32601, m);
        free(m);
    }
}

static void handle_message(lsp_client_t *c, json_t *msg) {
    json_t *id = json_get(msg, "id");
    const char *method = json_get_str(msg, "method");
    if (method) {
        if (id && !json_is_null(id)) handle_server_request(c, method, id, json_get(msg, "params"));
        else handle_notification(c, method, json_get(msg, "params"));
        json_free(msg);
        return;
    }
    if (id && json_is_num(id)) {
        int64_t iv = id->is_int ? id->u.num.i : json_d2i(id->u.num.d);
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
    }
    json_free(msg);
}

static void *reader_main(void *p) {
    lsp_client_t *c = p;
    sb_t buf = SB_INIT;
    char tmp[65536];
    bool out_open = true;
    while (out_open) {
        struct pollfd pf[2] = {{c->child.out_fd, POLLIN, 0}, {c->child.err_fd, POLLIN, 0}};
        int nf = c->child.err_fd >= 0 ? 2 : 1;
        int pr = poll(pf, (nfds_t)nf, 200);
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) {
            if (!c->alive) break;
            continue;
        }
        if (nf == 2 && (pf[1].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(c->child.err_fd, tmp, sizeof tmp);
            if (n > 0) {
                pthread_mutex_lock(&c->mu);
                sb_appendn(&c->stderr_tail, tmp, (size_t)n);
                if (c->stderr_tail.len > 16384) {
                    size_t drop = c->stderr_tail.len - 8192;
                    memmove(c->stderr_tail.s, c->stderr_tail.s + drop, c->stderr_tail.len - drop);
                    sb_truncate(&c->stderr_tail, c->stderr_tail.len - drop);
                }
                pthread_mutex_unlock(&c->mu);
            } else {
                close(c->child.err_fd);
                c->child.err_fd = -1;
            }
        }
        if (!(pf[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        ssize_t n = read(c->child.out_fd, tmp, sizeof tmp);
        if (n <= 0) break;
        sb_appendn(&buf, tmp, (size_t)n);
        for (;;) {
            const char *body;
            size_t blen, used;
            char *ferr = NULL;
            rpcframe_rc_t fr = rpcframe_next(&buf, &body, &blen, &used, &ferr);
            if (fr == RPCFRAME_RESYNC) {
                LOGW("LSP framing resync: header block without Content-Length (%s)", c->name);
                continue;
            }
            if (fr == RPCFRAME_ERROR) {
                /* MessageFramingError: the reader fails and every pending request is rejected with it */
                pthread_mutex_lock(&c->mu);
                free(c->close_error);
                c->close_error = xasprintf("LSP connection closed: MessageFramingError: %s", ferr);
                pthread_mutex_unlock(&c->mu);
                free(ferr);
                out_open = false;
                break;
            }
            if (fr != RPCFRAME_MSG) break;
            json_t *msg = json_parse(body, blen, NULL);
            rpcframe_consume(&buf, used);
            if (msg) handle_message(c, msg);
        }
    }
    sb_free(&buf);
    /* drain stderr and reap so pending requests can report the exit */
    if (c->child.err_fd >= 0) {
        for (int k = 0; k < 20; k++) {
            struct pollfd pe = {c->child.err_fd, POLLIN, 0};
            if (poll(&pe, 1, 10) <= 0) break;
            ssize_t n = read(c->child.err_fd, tmp, sizeof tmp);
            if (n <= 0) break;
            pthread_mutex_lock(&c->mu);
            sb_appendn(&c->stderr_tail, tmp, (size_t)n);
            pthread_mutex_unlock(&c->mu);
        }
    }
    int st = c->alive ? child_wait(&c->child, 100) : -1;
    pthread_mutex_lock(&c->mu);
    c->exit_code = st;
    c->alive = false;
    c->project_loaded = true;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

/* request outcome */
enum { RQ_OK = 0, RQ_ERROR = 1, RQ_TIMEOUT = 2, RQ_DEADLINE = 3, RQ_EXITED = 4 };

typedef struct {
    int64_t deadline; /* call deadline (mono ms); 0 = none */
    bool cancelled;
    volatile sig_atomic_t *cancel;
} budget_t;

/* exit message (client.ts proc.exited handler) */
static char *exit_message(lsp_client_t *c) {
    pthread_mutex_lock(&c->mu);
    if (c->close_error) {
        char *r = xstrdup(c->close_error);
        pthread_mutex_unlock(&c->mu);
        return r;
    }
    strvec_t lines = str_split(sb_cstr(&c->stderr_tail), "\n");
    pthread_mutex_unlock(&c->mu);
    sb_t keep = SB_INIT;
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        if (l[0] == '[' && strlen(l) > 13 && isdigit((unsigned char)l[1]) && l[3] == ':' && l[6] == ':' && l[9] == ' ' &&
            (!strncmp(l + 10, "INF]", 4) || !strncmp(l + 10, "DBG]", 4) || !strncmp(l + 10, "VRB]", 4)))
            continue;
        if (keep.len) sb_putc(&keep, '\n');
        sb_append(&keep, l);
    }
    strvec_free(&lines);
    char *t = str_trim(sb_cstr(&keep));
    sb_free(&keep);
    char *r = *t ? xasprintf("LSP server exited (code %d): %s", c->exit_code, t)
                 : xasprintf("LSP server exited unexpectedly (code %d)", c->exit_code);
    free(t);
    return r;
}

/* sendRequest: result (owned, json null for null) or NULL with *err and the outcome code.
 * timeout_ms > 0 is an explicit request timeout (LspRequestTimeoutError); otherwise the budget deadline bounds it. */
static json_t *request_ex(lsp_client_t *c, const char *method, json_t *params, int64_t timeout_ms, budget_t *bg,
                          char **err, int *code, json_t **err_data, int64_t *err_code) {
    *err = NULL;
    if (code) *code = RQ_OK;
    if (!c->alive) {
        json_free(params);
        *err = exit_message(c);
        if (code) *code = RQ_EXITED;
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
    if (params) json_set(msg, "params", params);
    send_json(c, msg);
    json_free(msg);
    json_t *result = NULL;
    int64_t start = mono_ms();
    int64_t limit = timeout_ms > 0 ? start + timeout_ms : (bg && bg->deadline ? bg->deadline : start + 30000);
    bool budget_limited = timeout_ms <= 0 && bg && bg->deadline;
    pthread_mutex_lock(&c->mu);
    while (!p->done && c->alive && mono_ms() < limit && !(bg && bg->cancel && *bg->cancel)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&c->cv, &c->mu, &ts);
    }
    for (size_t i = 0; i < c->pending.n; i++)
        if (c->pending.v[i] == p) {
            memmove(c->pending.v + i, c->pending.v + i + 1, (c->pending.n - i - 1) * sizeof(void *));
            c->pending.n--;
            break;
        }
    pthread_mutex_unlock(&c->mu);
    if (p->done) {
        json_t *e = json_get(p->resp, "error");
        if (e) {
            json_t *ec = json_get(e, "code");
            const char *m = json_get_str(e, "message");
            if (json_is_num(ec)) *err = xasprintf("LSP error %lld: %s", (long long)json_get_int(e, "code", 0), m ? m : "undefined");
            else *err = xasprintf("LSP error: %s", m ? m : "undefined");
            if (code) *code = RQ_ERROR;
            if (err_data) *err_data = json_clone(json_get(e, "data"));
            if (err_code) *err_code = json_is_num(ec) ? json_get_int(e, "code", 0) : 0;
        } else {
            result = json_take(p->resp, "result");
            if (!result) result = json_null();
        }
    } else if (bg && bg->cancel && *bg->cancel) {
        bg->cancelled = true;
        *err = xstrdup("Operation aborted");
        if (code) *code = RQ_DEADLINE;
    } else if (!c->alive) {
        *err = exit_message(c);
        if (code) *code = RQ_EXITED;
    } else if (budget_limited) {
        *err = xstrdup("Operation aborted");
        if (code) *code = RQ_DEADLINE;
    } else {
        *err = xasprintf("LSP request %s timed out after %lldms", method, (long long)(timeout_ms > 0 ? timeout_ms : 30000));
        if (code) *code = RQ_TIMEOUT;
    }
    json_free(p->resp);
    free(p);
    return result;
}

static json_t *request(lsp_client_t *c, const char *method, json_t *params, budget_t *bg, char **err, int *code) {
    return request_ex(c, method, params, 0, bg, err, code, NULL, NULL);
}

static void notify(lsp_client_t *c, const char *method, json_t *params) {
    json_t *msg = json_obj();
    json_set_str(msg, "jsonrpc", "2.0");
    json_set_str(msg, "method", method);
    if (params) json_set(msg, "params", params);
    send_json(c, msg);
    json_free(msg);
}

/* ---------- client lifecycle ---------- */

static const char *CLIENT_CAPABILITIES =
    "{\"textDocument\":{\"synchronization\":{\"didSave\":true,\"dynamicRegistration\":false,\"willSave\":false,"
    "\"willSaveWaitUntil\":false},\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"],\"dynamicRegistration\":false},"
    "\"definition\":{\"dynamicRegistration\":false,\"linkSupport\":true},\"typeDefinition\":{\"dynamicRegistration\":false,"
    "\"linkSupport\":true},\"implementation\":{\"dynamicRegistration\":false,\"linkSupport\":true},\"references\":{"
    "\"dynamicRegistration\":false},\"documentSymbol\":{\"dynamicRegistration\":false,\"hierarchicalDocumentSymbolSupport\":"
    "true,\"symbolKind\":{\"valueSet\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26]}},"
    "\"rename\":{\"dynamicRegistration\":false,\"prepareSupport\":true},\"codeAction\":{\"dynamicRegistration\":false,"
    "\"codeActionLiteralSupport\":{\"codeActionKind\":{\"valueSet\":[\"quickfix\",\"refactor\",\"refactor.extract\","
    "\"refactor.inline\",\"refactor.rewrite\",\"source\",\"source.organizeImports\",\"source.fixAll\"]}},\"resolveSupport\":"
    "{\"properties\":[\"edit\"]}},\"formatting\":{\"dynamicRegistration\":false},\"rangeFormatting\":{\"dynamicRegistration\":"
    "false},\"publishDiagnostics\":{\"relatedInformation\":true,\"versionSupport\":true,\"tagSupport\":{\"valueSet\":[1,2]},"
    "\"codeDescriptionSupport\":true,\"dataSupport\":true},\"diagnostic\":{\"dynamicRegistration\":true}},\"window\":{"
    "\"workDoneProgress\":true},\"workspace\":{\"applyEdit\":true,\"workspaceEdit\":{\"documentChanges\":true,"
    "\"resourceOperations\":[\"create\",\"rename\",\"delete\"],\"failureHandling\":\"abort\"},\"configuration\":true,"
    "\"workspaceFolders\":true,\"symbol\":{\"dynamicRegistration\":false,\"symbolKind\":{\"valueSet\":[1,2,3,4,5,6,7,8,9,"
    "10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26]}},\"fileOperations\":{\"dynamicRegistration\":false,\"willCreate\":"
    "false,\"didCreate\":false,\"willRename\":true,\"didRename\":true,\"willDelete\":false,\"didDelete\":false}}}";

static bool is_clangd(const json_t *cfg) {
    const char *cmd = json_get_str(cfg, "resolvedCommand");
    if (!cmd) cmd = json_get_str(cfg, "command");
    const char *b = cmd ? path_basename(cmd) : "";
    if (strncasecmp(b, "clangd", 6)) return false;
    const char *r = b + 6;
    if (*r == '-') {
        r++;
        if (!isdigit((unsigned char)*r)) return false;
        while (isdigit((unsigned char)*r)) r++;
    }
    return !*r || !strcasecmp(r, ".exe");
}

static bool is_rust_analyzer(const lsp_client_t *c) {
    const char *keys[] = {"command", "resolvedCommand"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        const char *cmd = json_get_str(c->cfg, keys[i]);
        const char *b = cmd ? path_basename(cmd) : "";
        if (!strcmp(b, "rust-analyzer") || !strcmp(b, "rust-analyzer.exe")) return true;
    }
    return false;
}

/* listCargoManifests: the `Cargo.toml` files rust-analyzer can discover under `root`, sorted and joined, so a reload can
 * tell whether the crate set changed since initialize (gitignored dirs such as `target/` skipped); NULL on failure */
static char *list_cargo_manifests(const char *root) {
    walk_opts_t o = {.types = WALK_FILE};
    walk_list_t l = {0};
    if (walk_collect(root, &o, &l) != 0) return NULL;
    sb_t b = SB_INIT;
    for (size_t i = 0; i < l.n; i++)
        if (!strcmp(path_basename(l.v[i].path), "Cargo.toml")) sb_appendf(&b, "%s\n", l.v[i].path);
    walk_list_free(&l);
    if (!b.s) return xstrdup("");
    return sb_detach(&b);
}

static void *cargo_manifests_thread(void *p) {
    lsp_client_t *c = p;
    c->cargo_manifests = list_cargo_manifests(c->root);
    return NULL;
}

static void client_free(lsp_client_t *c) {
    if (c->cargo_started) pthread_join(c->cargo_th, NULL);
    free(c->cargo_manifests);
    for (size_t i = 0; i < c->open.n; i++) {
        free(c->open.v[i].uri);
        free(c->open.v[i].lang);
    }
    vec_free(&c->open);
    vec_free(&c->pending);
    json_free(c->cfg);
    json_free(c->caps);
    json_free(c->diags);
    json_free(c->pub_diags);
    json_free(c->pull_diags);
    json_free(c->build_status);
    json_free(c->registrations);
    json_free(c->progress);
    json_free(c->idle_closed);
    sb_free(&c->stderr_tail);
    free(c->close_error);
    free(c->name);
    free(c->root);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    free(c);
}

static void client_stop(lsp_client_t *c, bool graceful) {
    if (c->alive && graceful) {
        char *err = NULL;
        budget_t bg = {mono_ms() + 3000, false, NULL};
        json_t *r = request(c, "shutdown", NULL, &bg, &err, NULL);
        json_free(r);
        free(err);
        notify(c, "exit", NULL);
    }
    c->alive = false;
    child_close(&c->child);
    if (c->reader_started) pthread_join(c->reader, NULL);
    client_free(c);
}

/* getOrCreateClient body: spawn + initialize; NULL with *err on failure */
static lsp_client_t *client_start(const char *name, const json_t *cfg, const char *cwd, budget_t *bg, char **err) {
    const char *bin = json_get_str(cfg, "resolvedCommand");
    if (!bin) bin = json_get_str(cfg, "command");
    strvec_t argv = {0};
    strvec_push(&argv, bin);
    json_t *a;
    json_arr_foreach(json_get_arr(cfg, "args"), i, a) {
        if (!json_is_str(a)) continue;
        if (!strcmp(json_sv(a), "$PID") && str_eq(name, "omnisharp")) strvec_push_own(&argv, xasprintf("%d", (int)getpid()));
        else strvec_push(&argv, json_sv(a));
    }
    vec_push(&argv, NULL);
    lsp_client_t *c = xcalloc(1, sizeof(*c));
    c->name = xstrdup(name);
    c->cfg = json_clone(cfg);
    c->root = xstrdup(cwd);
    c->diags = json_obj();
    c->pub_diags = json_obj();
    c->pull_diags = json_obj();
    c->registrations = json_obj();
    c->progress = json_obj();
    c->idle_closed = json_obj();
    c->status = "connecting";
    c->started_at = mono_ms();
    c->project_deadline = c->started_at + PROJECT_LOAD_TIMEOUT_MS;
    c->exit_code = -1;
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    int rc = child_spawn((const char *const *)argv.v, cwd, NULL, true, &c->child);
    argv.n--;
    strvec_free(&argv);
    if (rc != 0) {
        *err = xasprintf("ENOENT: no such file or directory, posix_spawn '%s'", bin);
        client_free(c);
        return NULL;
    }
    c->alive = true;
    if (pthread_create(&c->reader, NULL, reader_main, c) == 0) c->reader_started = true;
    json_t *p = json_obj();
    json_set_int(p, "processId", getpid());
    char *ru = path_to_uri(cwd);
    json_set_str(p, "rootUri", ru);
    json_set_str(p, "rootPath", cwd);
    json_set(p, "capabilities", json_parse_str(CLIENT_CAPABILITIES));
    json_t *io = json_obj();
    if (is_clangd(cfg)) json_set_bool(io, "clangdFileStatus", true);
    json_t *uio = json_get_obj(cfg, "initOptions");
    if (uio) json_merge(io, uio);
    json_set(p, "initializationOptions", io);
    json_t *wf = json_arr(), *f = json_obj();
    json_set_str(f, "uri", ru);
    json_set_str(f, "name", path_basename(cwd));
    json_push(wf, f);
    json_set(p, "workspaceFolders", wf);
    free(ru);
    char *e2 = NULL;
    int code = 0;
    json_t *res = request(c, "initialize", p, bg, &e2, &code);
    if (!res || json_is_null(res)) {
        *err = res ? xstrdup("Failed to initialize LSP: no response") : e2;
        if (res) free(e2);
        json_free(res);
        c->status = "error";
        client_stop(c, false);
        return NULL;
    }
    c->caps = json_take(res, "capabilities");
    json_free(res);
    notify(c, "initialized", json_obj());
    json_t *pp = json_obj();
    json_t *st = json_get(cfg, "settings");
    json_set(pp, "settings", st ? json_clone(st) : json_obj());
    notify(c, "workspace/didChangeConfiguration", pp);
    c->status = "ready";
    if (is_rust_analyzer(c) && pthread_create(&c->cargo_th, NULL, cargo_manifests_thread, c) == 0) c->cargo_started = true;
    return c;
}

lsp_manager_t *lsp_manager_new(const char *cwd) {
    lsp_manager_t *m = xcalloc(1, sizeof(*m));
    m->cwd = xstrdup(cwd);
    m->servers = load_config(cwd, &m->nested, &m->unresolved);
    m->init_failures = json_obj();
    m->ledger = json_obj();
    pthread_mutex_init(&m->mu, NULL);
    return m;
}

/* a new conversation reports every diagnostic again (the diagnostics ledger is conversation state) */
void lsp_manager_reset_ledger(lsp_manager_t *m) {
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    json_free(m->ledger);
    m->ledger = json_obj();
    pthread_mutex_unlock(&m->mu);
}

void lsp_manager_free(lsp_manager_t *m) {
    if (!m) return;
    for (size_t i = 0; i < m->clients.n; i++) client_stop(m->clients.v[i], true);
    vec_free(&m->clients);
    json_free(m->servers);
    json_free(m->nested);
    json_free(m->unresolved);
    json_free(m->init_failures);
    json_free(m->ledger);
    free(m->cwd);
    pthread_mutex_destroy(&m->mu);
    free(m);
}

static lsp_client_t *find_client(lsp_manager_t *m, const char *name) {
    for (size_t i = 0; i < m->clients.n; i++)
        if (!strcmp(m->clients.v[i]->name, name)) return m->clients.v[i];
    return NULL;
}

/* getOrCreateClient: live client for server `name` (started on demand), NULL with *err */
static lsp_client_t *get_client(lsp_manager_t *m, const char *name, budget_t *bg, char **err) {
    *err = NULL;
    pthread_mutex_lock(&m->mu);
    lsp_client_t *c = find_client(m, name);
    if (c && !c->alive) { /* crashed: drop it, the next use respawns */
        for (size_t i = 0; i < m->clients.n; i++)
            if (m->clients.v[i] == c) {
                m->clients.v[i] = m->clients.v[--m->clients.n];
                break;
            }
        client_stop(c, false);
        c = NULL;
    }
    if (!c) {
        json_t *cfg = json_get(m->servers, name);
        const char *fail = json_get_str(m->init_failures, name);
        if (fail) {
            *err = xasprintf("LSP server %s failed to initialize recently: %s", json_get_str(cfg, "command"), fail);
        } else if (cfg) {
            c = client_start(name, cfg, m->cwd, bg, err);
            if (c) vec_push(&m->clients, c);
            else if (*err && !(bg && bg->cancelled) && strcmp(*err, "Operation aborted") != 0)
                json_set_str(m->init_failures, name, *err);
        } else {
            *err = xasprintf("unknown server %s", name);
        }
    }
    pthread_mutex_unlock(&m->mu);
    return c;
}

/* ---------- documents (client.ts ensureFileOpen / reconcileFileFromDisk / refreshDocument) ---------- */

static openf_t *open_find(lsp_client_t *c, const char *uri) {
    for (size_t i = 0; i < c->open.n; i++)
        if (!strcmp(c->open.v[i].uri, uri)) return &c->open.v[i];
    return NULL;
}

static void open_remove(lsp_client_t *c, const char *uri) {
    for (size_t i = 0; i < c->open.n; i++)
        if (!strcmp(c->open.v[i].uri, uri)) {
            free(c->open.v[i].uri);
            free(c->open.v[i].lang);
            c->open.v[i] = c->open.v[--c->open.n];
            return;
        }
}

/* drop the cached report and the pulled one computed against the stale document; `all` also forgets the pushed one
 * (the document closed) */
static void diags_drop(lsp_client_t *c, const char *uri, bool all) {
    char *k = uri_key(uri);
    pthread_mutex_lock(&c->mu);
    json_del(c->diags, k);
    json_del(c->pull_diags, k);
    if (all) json_del(c->pub_diags, k);
    pthread_mutex_unlock(&c->mu);
    free(k);
}

static void diags_delete(lsp_client_t *c, const char *uri) { diags_drop(c, uri, false); }

static char *read_text(const char *path, size_t *n) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    return fs_read(path, n);
}

static void track_opened(lsp_client_t *c, const char *uri, const char *lang, int64_t version, const char *text, size_t n) {
    char *key = uri_key(uri);
    bool reopened = json_get(c->idle_closed, key) != NULL;
    json_del(c->idle_closed, key);
    free(key);
    openf_t o = {xstrdup(uri), xstrdup(lang), version, text_hash(text, n), reopened ? mono_ms() : 0, mono_ms()};
    vec_push(&c->open, o);
}

static void send_did_open(lsp_client_t *c, const char *uri, const char *lang, int64_t version, const char *text) {
    json_t *p = json_obj(), *td = json_obj();
    json_set_str(td, "uri", uri);
    json_set_str(td, "languageId", lang);
    json_set_int(td, "version", version);
    json_set_str(td, "text", text);
    json_set(p, "textDocument", td);
    notify(c, "textDocument/didOpen", p);
}

static void send_did_change(lsp_client_t *c, const char *uri, int64_t version, const char *text, bool want_diags) {
    json_t *p = json_obj(), *td = json_obj(), *ch = json_arr(), *one = json_obj();
    json_set_str(td, "uri", uri);
    json_set_int(td, "version", version);
    json_set(p, "textDocument", td);
    json_set_str(one, "text", text);
    json_push(ch, one);
    json_set(p, "contentChanges", ch);
    if (want_diags) json_set_bool(p, "wantDiagnostics", true);
    notify(c, "textDocument/didChange", p);
}

static void send_simple(lsp_client_t *c, const char *method, const char *uri) {
    json_t *p = json_obj(), *td = json_obj();
    json_set_str(td, "uri", uri);
    json_set(p, "textDocument", td);
    notify(c, method, p);
}

/* textDocumentSync.save.includeText */
static bool save_includes_text(lsp_client_t *c) {
    json_t *sync = json_get(c->caps, "textDocumentSync");
    json_t *save = json_is_obj(sync) ? json_get(sync, "save") : NULL;
    return json_is_obj(save) && json_get_bool(save, "includeText", false);
}

static void send_did_save(lsp_client_t *c, const char *uri, const char *text) {
    json_t *p = json_obj(), *td = json_obj();
    json_set_str(td, "uri", uri);
    json_set(p, "textDocument", td);
    if (text && save_includes_text(c)) json_set_str(p, "text", text);
    notify(c, "textDocument/didSave", p);
}

static void close_document(lsp_client_t *c, const char *uri) {
    send_simple(c, "textDocument/didClose", uri);
    open_remove(c, uri);
    diags_drop(c, uri, true);
}

/* OPEN_FILE_IDLE_MS / closeIdleFiles: documents no operation used for this long leave the server before the next open */
static int64_t open_file_idle_ms(void) {
    const char *v = env_get("OMC_LSP_FILE_IDLE_MS"); /* test hook */
    return v && atoll(v) > 0 ? atoll(v) : 5 * 60 * 1000;
}

static void close_idle_files(lsp_client_t *c) {
    int64_t idle_before = mono_ms() - open_file_idle_ms();
    for (size_t i = 0; i < c->open.n;) {
        openf_t *o = &c->open.v[i];
        if (o->last_used > idle_before) {
            i++;
            continue;
        }
        char *uri = xstrdup(o->uri), *key = uri_key(uri);
        int64_t version = o->version;
        close_document(c, uri);
        json_set_int(c->idle_closed, key, version);
        free(key);
        free(uri);
    }
}

static void ensure_open(lsp_client_t *c, const char *path) {
    char *uri = path_to_uri(path);
    openf_t *found = open_find(c, uri);
    if (found) {
        found->last_used = mono_ms();
        free(uri);
        return;
    }
    size_t n;
    char *text = read_text(path, &n);
    if (!text) {
        free(uri);
        return;
    }
    close_idle_files(c);
    const char *lang = json_get_str(c->cfg, "languageId");
    if (!lang) lang = language_id(path);
    char *key = uri_key(uri);
    int64_t version = json_get_int(c->idle_closed, key, 0) + 1;
    free(key);
    send_did_open(c, uri, lang, version, text);
    track_opened(c, uri, lang, version, text, n);
    free(text);
    free(uri);
}

/* syncOpenFileFromDisk: push disk text for an open document whose overlay diverged from it; true when a didChange went out.
 * Not a use of the document, so a sync of every open one leaves idle ones to close. A file gone from disk is closed; one that
 * exists but cannot be read (a directory now, no permission, a FIFO) is closed with `close_unreadable`, else reported in *err. */
static bool sync_open_file(lsp_client_t *c, const char *path, const char *uri, bool close_unreadable, char **err) {
    openf_t *o = open_find(c, uri);
    if (!o) return false;
    size_t n;
    char *text = read_text(path, &n);
    if (!text) {
        if (close_unreadable || !fs_exists(path)) close_document(c, uri);
        else if (err && !*err) {
            struct stat st;
            *err = stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? xstrdup("EISDIR: illegal operation on a directory, read")
                                                               : xasprintf("EACCES: permission denied, open '%s'", path);
        }
        return false;
    }
    bool changed = false;
    uint64_t h = text_hash(text, n);
    if (h != o->hash) {
        diags_delete(c, uri);
        int64_t v = ++o->version;
        send_did_change(c, uri, v, text, false);
        o->hash = h;
        changed = true;
    }
    free(text);
    return changed;
}

/* reconcileFileFromDisk: true when stale diagnostics were dropped (didChange pushed / document reopened) */
static bool reconcile_file(lsp_client_t *c, const char *path) {
    char *uri = path_to_uri(path);
    openf_t *o = open_find(c, uri);
    if (!o) {
        char *key = uri_key(uri);
        bool reopening = json_get(c->idle_closed, key) != NULL;
        free(key);
        ensure_open(c, path);
        bool r = reopening && open_find(c, uri);
        free(uri);
        return r;
    }
    o->last_used = mono_ms();
    bool changed = sync_open_file(c, path, uri, false, NULL);
    free(uri);
    return changed;
}

/* reconcileOpenFilesFromDisk: `close_unreadable` closes a document whose file can no longer be read, so one broken file does not
 * fail a query about another; otherwise *err (may be NULL) reports it */
static int reconcile_open_files(lsp_client_t *c, const char *exclude_uri, bool close_unreadable, char **err) {
    strvec_t uris = {0};
    for (size_t i = 0; i < c->open.n; i++)
        if (str_starts(c->open.v[i].uri, "file://") && !str_eq(c->open.v[i].uri, exclude_uri)) strvec_push(&uris, c->open.v[i].uri);
    int n = 0;
    for (size_t i = 0; i < uris.n; i++) {
        char *p = uri_to_path(uris.v[i]);
        if (sync_open_file(c, p, uris.v[i], close_unreadable, err)) n++;
        free(p);
    }
    strvec_free(&uris);
    return n;
}

enum { RF_USE, RF_RESYNC, RF_REOPEN, RF_REBUILD };

/* refreshDocument: returns true when a rebuild was sent */
static bool refresh_document(lsp_client_t *c, const char *path, int purpose) {
    char *uri = path_to_uri(path);
    diags_delete(c, uri);
    openf_t *o = open_find(c, uri);
    if (!o) {
        if (purpose != RF_RESYNC) ensure_open(c, path);
        free(uri);
        return false;
    }
    if (purpose != RF_RESYNC) o->last_used = mono_ms();
    size_t n;
    char *text = read_text(path, &n);
    if (!text) {
        if (!fs_exists(path)) close_document(c, uri);
        free(uri);
        return false;
    }
    int64_t version = ++o->version;
    bool rebuilt = false;
    if (purpose == RF_REOPEN) {
        char *lang = xstrdup(o->lang);
        close_document(c, uri);
        char *key = uri_key(uri);
        json_set_int(c->idle_closed, key, version);
        free(key);
        send_did_open(c, uri, lang, version, text);
        track_opened(c, uri, lang, version, text, n);
        send_did_save(c, uri, text);
        free(lang);
    } else if (purpose == RF_REBUILD) {
        char *detour = xasprintf("%s\n", text);
        send_did_change(c, uri, version, detour, true);
        free(detour);
        int64_t restored = ++o->version;
        send_did_change(c, uri, restored, text, true);
        o->hash = text_hash(text, n);
        rebuilt = true;
    } else {
        send_did_change(c, uri, version, text, false);
        send_did_save(c, uri, text);
        o->hash = text_hash(text, n);
    }
    free(text);
    free(uri);
    return rebuilt;
}

static void refresh_open_files(lsp_client_t *c) {
    strvec_t paths = {0};
    for (size_t i = 0; i < c->open.n; i++) strvec_push_own(&paths, uri_to_path(c->open.v[i].uri));
    for (size_t i = 0; i < paths.n; i++) refresh_document(c, paths.v[i], RF_RESYNC);
    strvec_free(&paths);
}

/* syncContent: open with `content` or push a didChange */
static void sync_content(lsp_client_t *c, const char *path, const char *content) {
    char *uri = path_to_uri(path);
    diags_delete(c, uri);
    openf_t *o = open_find(c, uri);
    size_t n = strlen(content);
    if (!o) {
        close_idle_files(c);
        const char *lang = json_get_str(c->cfg, "languageId");
        if (!lang) lang = language_id(path);
        char *key = uri_key(uri);
        int64_t version = json_get_int(c->idle_closed, key, 0) + 1;
        free(key);
        send_did_open(c, uri, lang, version, content);
        track_opened(c, uri, lang, version, content, n);
    } else {
        o->last_used = mono_ms();
        int64_t v = ++o->version;
        send_did_change(c, uri, v, content, false);
        o->hash = text_hash(content, n);
    }
    free(uri);
}

static int64_t open_version(lsp_client_t *c, const char *path) {
    char *uri = path_to_uri(path);
    openf_t *o = open_find(c, uri);
    int64_t v = o ? o->version : -1;
    free(uri);
    return v;
}

/* supportsMethod: the capability is advertised statically or `method` was registered dynamically */
static bool supports_method(lsp_client_t *c, const char *cap, const char *method) {
    if (json_truthy(json_get(c->caps, cap))) return true;
    bool r = false;
    pthread_mutex_lock(&c->mu);
    const char *k;
    json_t *v;
    json_obj_foreach(c->registrations, i, k, v) if (str_eq(json_sv(v), method)) r = true;
    pthread_mutex_unlock(&c->mu);
    return r;
}

static bool supports_pull(lsp_client_t *c) {
    if (json_truthy(json_get(c->caps, "diagnosticProvider"))) return true;
    bool r = false;
    pthread_mutex_lock(&c->mu);
    const char *k;
    json_t *v;
    json_obj_foreach(c->registrations, i, k, v) if (str_eq(json_sv(v), "textDocument/diagnostic")) r = true;
    pthread_mutex_unlock(&c->mu);
    return r;
}

static bool budget_expired(const budget_t *bg) {
    return bg && ((bg->deadline && mono_ms() >= bg->deadline) || (bg->cancel && *bg->cancel));
}


/* waitForProjectLoaded */
static void wait_project_loaded(lsp_client_t *c, const budget_t *bg) {
    pthread_mutex_lock(&c->mu);
    while (!c->project_loaded && c->alive && mono_ms() < c->project_deadline && !budget_expired(bg)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&c->cv, &c->mu, &ts);
    }
    pthread_mutex_unlock(&c->mu);
    if (is_rust_analyzer(c) && !budget_expired(bg)) {
        /* waitForRustAnalyzerWorkspace: poll rust-analyzer/analyzerStatus until it stops reporting a load (best effort) */
        int64_t end = mono_ms() + 5000;
        while (mono_ms() < end && !budget_expired(bg)) {
            char *err = NULL;
            budget_t one = {mono_ms() + 1000, false, NULL};
            json_t *r = request(c, "rust-analyzer/analyzerStatus", json_obj(), &one, &err, NULL);
            bool loading = json_is_str(r) && strstr(json_sv(r), "loading");
            json_free(r);
            free(err);
            if (!loading) break;
            sleep_ms(100);
        }
    }
}

/* ---------- diagnostics wait (diagnostics.ts) ---------- */

/* copy of the published entry for key; *version = -1 when null; returns NULL when none */
static json_t *published_get(lsp_client_t *c, const char *key, int64_t *version) {
    pthread_mutex_lock(&c->mu);
    json_t *e = json_get(c->diags, key);
    json_t *r = e ? json_clone(e) : NULL;
    pthread_mutex_unlock(&c->mu);
    if (r) {
        json_t *v = json_get(r, "version");
        *version = json_is_num(v) ? json_get_int(r, "version", -1) : -1;
    }
    return r;
}

static bool provisional_cold_publish(lsp_client_t *c, const char *uri, const json_t *pub, int64_t pub_version,
                                     int64_t expected) {
    if (expected < 0) return false;
    int64_t now = mono_ms();
    openf_t *o = open_find(c, uri);
    size_t nitems = json_len(json_get(pub, "items"));
    if (o && o->reopened_at && now - o->reopened_at < DEFERRED_DIAGNOSTICS_WAIT_TIMEOUT_MS) {
        if (nitems == 0) return true;
        if (pub_version >= 0 && pub_version < expected) return true;
    }
    if (nitems > 0) return false;
    return pub_version < 0 && now - c->started_at < DEFERRED_DIAGNOSTICS_WAIT_TIMEOUT_MS;
}

/* waitForDiagnostics: items (owned) or NULL when no fresh report; *failed set (owned) on a pull failure */
static json_t *wait_for_diagnostics(lsp_client_t *c, const char *uri, int64_t timeout_ms, const budget_t *bg,
                                    int64_t min_version, int64_t expected, bool after_project_load, char **failed) {
    *failed = NULL;
    char *key = uri_key(uri);
    int64_t deadline = mono_ms() + timeout_ms;
    bool awaiting_load = after_project_load;
    int64_t next_pull = 0; /* -1 once answered/in flight */
    json_t *settled = NULL;
    int64_t settled_at = 0;
    json_t *result = NULL;
    bool have = false, from_pull = false;
    while (mono_ms() < deadline && !budget_expired(bg)) {
        bool pull = supports_pull(c);
        if (awaiting_load && pull) {
            awaiting_load = false;
            wait_project_loaded(c, bg);
            deadline = MAX(deadline, mono_ms() + timeout_ms);
        }
        if (next_pull >= 0 && mono_ms() >= next_pull && pull) {
            next_pull = -1;
            json_t *p = json_obj(), *td = json_obj();
            json_set_str(td, "uri", uri);
            json_set(p, "textDocument", td);
            char *err = NULL;
            int code = 0;
            json_t *data = NULL;
            int64_t ecode = 0;
            json_t *rep = request_ex(c, "textDocument/diagnostic", p, MAX((int64_t)1, deadline - mono_ms()), (budget_t *)bg, &err,
                                     &code, &data, &ecode);
            if (rep) {
                if (str_eq(json_get_str(rep, "kind"), "full") && json_is_arr(json_get(rep, "items"))) {
                    result = json_clone(json_get(rep, "items"));
                    have = from_pull = true;
                    json_free(rep);
                    break;
                }
                /* LSP requires a report, but ruby-lsp answers `null` for a document it does not diagnose (ERB, or
                 * outside its workspace) and when its own diagnostics fail: that is the server's answer, not a failed
                 * attempt, so a publish already on its way gets one settle window, not the budget */
                if (json_is_null(rep)) deadline = MIN(deadline, mono_ms() + DIAGNOSTICS_SETTLE_MS);
                json_free(rep);
            } else if (code == RQ_ERROR) {
                bool retrigger = ecode == -32801 || (ecode == -32802 && !(json_is_obj(data) && json_get(data, "retriggerRequest") &&
                                                                         !json_get_bool(data, "retriggerRequest", true)));
                if (retrigger) next_pull = mono_ms() + DIAGNOSTICS_POLL_MS;
                else if (!*failed) *failed = xstrdup(err);
            } else if (code == RQ_EXITED && !*failed) {
                *failed = xstrdup(err);
            }
            json_free(data);
            free(err);
        }
        pthread_mutex_lock(&c->mu);
        bool version_ok = min_version < 0 || c->diag_version > min_version;
        pthread_mutex_unlock(&c->mu);
        int64_t pv = -1;
        json_t *pub = version_ok && !pull ? published_get(c, key, &pv) : NULL;
        if (pub) {
            if (expected >= 0 && pv == expected) {
                result = json_take(pub, "items");
                have = true;
                json_free(pub);
                break;
            }
            int64_t now = mono_ms();
            if (!settled || !json_equal(settled, pub)) {
                json_free(settled);
                settled = pub;
                pub = NULL;
                settled_at = now;
            } else if (now - settled_at >= DIAGNOSTICS_SETTLE_MS && !provisional_cold_publish(c, uri, pub, pv, expected)) {
                result = json_take(pub, "items");
                have = true;
                json_free(pub);
                break;
            }
            json_free(pub);
        }
        if (!c->alive) break;
        sleep_ms(MIN((int64_t)DIAGNOSTICS_POLL_MS, MAX((int64_t)0, deadline - mono_ms())));
    }
    bool dual = supports_pull(c);
    pthread_mutex_lock(&c->mu);
    bool final_version_ok = min_version < 0 || c->diag_version > min_version;
    pthread_mutex_unlock(&c->mu);
    if (!have) {
        bool version_ok = final_version_ok;
        int64_t pv = -1;
        json_t *pub = version_ok && !dual ? published_get(c, key, &pv) : NULL;
        if (pub) {
            if ((expected >= 0 && pv == expected) ||
                (settled && json_equal(settled, pub) && mono_ms() - settled_at >= DIAGNOSTICS_SETTLE_MS &&
                 !provisional_cold_publish(c, uri, pub, pv, expected))) {
                result = json_take(pub, "items");
                have = true;
            }
            json_free(pub);
        }
    }
    /* a dual-mode server: the pull's report merged with the pushed one, or the push alone standing in for a failed
     * pull, but only a push that arrived after the caller's minVersion */
    if (dual && (from_pull || (!have && final_version_ok))) {
        openf_t *o = open_find(c, uri);
        int64_t doc_version = o ? o->version : -1;
        pthread_mutex_lock(&c->mu);
        json_t *pe = json_get(c->pub_diags, key);
        json_t *pub = pe ? json_clone(pe) : NULL;
        json_t *pulled_rec = from_pull ? NULL : json_clone(json_get(c->pull_diags, key));
        pthread_mutex_unlock(&c->mu);
        int64_t pv = json_is_num(json_get(pub, "version")) ? json_get_int(pub, "version", -1) : -1;
        int64_t ver = expected >= 0 ? expected : doc_version;
        json_t *combined = NULL;
        if (from_pull) {
            /* unversioned flycheck publishes still merge; a push older than the open document does not */
            bool can_merge = pub && (pv < 0 || doc_version < 0 || pv >= doc_version);
            combined = can_merge ? merge_diagnostic_lists(result, json_get(pub, "items")) : json_clone(result);
            json_t *pr = json_obj();
            json_set(pr, "items", result);
            if (ver >= 0) json_set_int(pr, "version", ver);
            else json_set(pr, "version", json_null());
            pthread_mutex_lock(&c->mu);
            json_set(c->pull_diags, key, pr);
            pthread_mutex_unlock(&c->mu);
            result = combined;
        } else if (pub) {
            bool fresh = pv < 0 ? !provisional_cold_publish(c, uri, pub, pv, expected) : doc_version < 0 || pv >= doc_version;
            if (fresh) {
                combined = pulled_rec ? merge_diagnostic_lists(json_get(pulled_rec, "items"), json_get(pub, "items"))
                                      : json_clone(json_get(pub, "items"));
                result = combined;
                have = true;
            }
        }
        if (combined) {
            json_t *e = json_obj();
            json_set(e, "items", json_clone(combined));
            if (ver >= 0) json_set_int(e, "version", ver);
            else json_set(e, "version", json_null());
            pthread_mutex_lock(&c->mu);
            json_set(c->diags, key, e);
            c->diag_version++;
            pthread_mutex_unlock(&c->mu);
        }
        json_free(pub);
        json_free(pulled_rec);
    }
    json_free(settled);
    free(key);
    if (have) {
        free(*failed);
        *failed = NULL;
        if (!result) result = json_arr();
    }
    return result;
}

/* waitForRebuiltDiagnostics (clangd build status) */
static json_t *wait_for_rebuilt_diagnostics(lsp_client_t *c, const char *uri, int64_t version, int64_t since_seq,
                                            int64_t timeout_ms, budget_t *bg) {
    char *key = uri_key(uri);
    int64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&c->mu);
        json_t *st = c->build_status ? json_get(c->build_status, key) : NULL;
        int64_t busy = json_get_int(st, "busy", 0), idle = json_get_int(st, "idle", 0);
        pthread_mutex_unlock(&c->mu);
        if (st && busy > since_seq && idle > busy) break;
        int64_t left = deadline - mono_ms();
        if (left <= 0 || budget_expired(bg) || !c->alive) {
            free(key);
            return NULL;
        }
        sleep_ms(MIN((int64_t)BUILD_STATUS_POLL_MS, left));
    }
    json_t *p = json_obj(), *td = json_obj();
    json_set_str(td, "uri", uri);
    json_set(p, "textDocument", td);
    char *err = NULL;
    json_t *r = request_ex(c, "textDocument/documentSymbol", p, MAX((int64_t)1, deadline - mono_ms()), bg, &err, NULL, NULL, NULL);
    if (!r) {
        free(err);
        free(key);
        return NULL;
    }
    json_free(r);
    int64_t pv = -1;
    json_t *pub = published_get(c, key, &pv);
    json_t *items = pub && pv == version ? json_take(pub, "items") : NULL;
    json_free(pub);
    free(key);
    return items;
}

/* ---------- formatting (lsp/utils.ts) ---------- */

static const char *severity_name(int64_t s) {
    switch (s) {
    case 1: return "error";
    case 2: return "warning";
    case 3: return "info";
    case 4: return "hint";
    default: return "unknown";
    }
}

static int64_t diag_severity(const json_t *d) {
    json_t *s = json_get(d, "severity");
    return json_is_num(s) ? json_get_int(d, "severity", 1) : 1;
}

static int64_t pos_line(const json_t *range, const char *which_) { return json_get_int(json_get(range, which_), "line", 0); }
static int64_t pos_char(const json_t *range, const char *which_) { return json_get_int(json_get(range, which_), "character", 0); }

static int cmp_diag(const void *a, const void *b) {
    const json_t *x = *(json_t *const *)a, *y = *(json_t *const *)b;
    int64_t sx = diag_severity(x), sy = diag_severity(y);
    if (sx != sy) return sx < sy ? -1 : 1;
    json_t *rx = json_get(x, "range"), *ry = json_get(y, "range");
    int64_t lx = pos_line(rx, "start"), ly = pos_line(ry, "start");
    if (lx != ly) return lx < ly ? -1 : 1;
    int64_t cx = pos_char(rx, "start"), cy = pos_char(ry, "start");
    if (cx != cy) return cx < cy ? -1 : 1;
    const char *mx = json_get_str(x, "message"), *my = json_get_str(y, "message");
    return strcoll(mx ? mx : "", my ? my : "");
}

/* stripDiagnosticNoise */
static char *strip_noise(const char *msg) {
    strvec_t lines = str_split(msg ? msg : "", "\n");
    sb_t b = SB_INIT;
    bool first = true;
    for (size_t i = 0; i < lines.n; i++) {
        char *t = str_trim(lines.v[i]);
        bool drop = str_starts(t, "for further information visit") || str_starts(t, "http://") || str_starts(t, "https://");
        free(t);
        if (drop) continue;
        if (!first) sb_putc(&b, '\n');
        first = false;
        sb_append(&b, lines.v[i]);
    }
    strvec_free(&lines);
    char *r = str_trim(sb_cstr(&b));
    sb_free(&b);
    return r;
}

/* formatDiagnostic */
static char *format_diagnostic(const json_t *d, const char *file) {
    json_t *r = json_get(d, "range");
    const char *src = json_get_str(d, "source");
    json_t *code = json_get(d, "code");
    bool code_truthy = code && json_truthy(code);
    char *cs = code_truthy ? js_string(code) : NULL;
    char *msg = strip_noise(json_get_str(d, "message"));
    char *s = xasprintf("%s:%lld:%lld [%s] %s%s%s%s%s%s", file, (long long)pos_line(r, "start") + 1,
                        (long long)pos_char(r, "start") + 1, severity_name(diag_severity(d)), src && *src ? "[" : "",
                        src && *src ? src : "", src && *src ? "] " : "", msg, cs ? " (" : "", cs ? cs : "");
    char *full = cs ? xasprintf("%s)", s) : xstrdup(s);
    free(s);
    free(msg);
    free(cs);
    return full;
}

static void count_parts(sb_t *b, const int counts[4]) {
    static const char *names[] = {"error(s)", "warning(s)", "info(s)", "hint(s)"};
    bool any = false;
    for (int i = 0; i < 4; i++) {
        if (!counts[i]) continue;
        if (any) sb_append(b, ", ");
        sb_appendf(b, "%d %s", counts[i], names[i]);
        any = true;
    }
    if (!any) sb_append(b, "no issues");
}

/* formatDiagnosticsSummary */
static char *diagnostics_summary(json_t *const *ds, size_t n) {
    int counts[4] = {0};
    for (size_t i = 0; i < n; i++) {
        int64_t s = diag_severity(ds[i]);
        if (s >= 1 && s <= 4) counts[s - 1]++;
    }
    sb_t b = SB_INIT;
    count_parts(&b, counts);
    return sb_detach(&b);
}

/* summarizeDiagnosticMessages */
static char *summarize_messages(const strvec_t *msgs, bool *errored) {
    int counts[4] = {0};
    static const char *keys[] = {"[error]", "[warning]", "[info]", "[hint]"};
    for (size_t i = 0; i < msgs->n; i++) {
        const char *best = NULL;
        int bi = -1;
        for (int k = 0; k < 4; k++) {
            const char *p = str_istr(msgs->v[i], keys[k]);
            if (p && (!best || p < best)) {
                best = p;
                bi = k;
            }
        }
        if (bi >= 0) counts[bi]++;
    }
    if (errored) *errored = counts[0] > 0;
    sb_t b = SB_INIT;
    count_parts(&b, counts);
    return sb_detach(&b);
}

/* DIAG_PATH_RE = /^(.+?):(\d+:\d+\s+.*)$/ ; returns split offset of the ':' or -1 */
static ssize_t diag_path_split(const char *m) {
    size_t n = strlen(m);
    for (size_t i = 1; i < n; i++) {
        if (m[i - 1] == '\n') return -1; /* (.+?) cannot cross a newline */
        if (m[i] != ':') continue;
        size_t j = i + 1, d1 = j;
        while (j < n && isdigit((unsigned char)m[j])) j++;
        if (j == d1 || j >= n || m[j] != ':') continue;
        size_t d2 = ++j;
        while (j < n && isdigit((unsigned char)m[j])) j++;
        if (j == d2) continue;
        size_t ws = j;
        while (j < n && isspace((unsigned char)m[j])) j++;
        if (j == ws) continue;
        /* `\s+` may backtrack; `.*$` needs the rest (after some whitespace) to be newline-free */
        bool ok = false;
        for (size_t k = j; k > ws; k--) {
            if (!strchr(m + k, '\n')) {
                ok = true;
                break;
            }
        }
        if (ok) return (ssize_t)i;
    }
    return -1;
}

/* pi-utils path-tree + grouped-file-output formatGroupedFiles (model lines only) */
typedef struct gnode gnode_t;
struct gnode {
    strvec_t file_names;
    VEC(size_t) file_keys;
    strvec_t dir_names;
    VEC(gnode_t *) dirs;
};

static gnode_t *gnode_dir(gnode_t *n, const char *name) {
    for (size_t i = 0; i < n->dir_names.n; i++)
        if (!strcmp(n->dir_names.v[i], name)) return n->dirs.v[i];
    gnode_t *c = xcalloc(1, sizeof *c);
    strvec_push(&n->dir_names, name);
    vec_push(&n->dirs, c);
    return c;
}

static void gnode_free(gnode_t *n) {
    for (size_t i = 0; i < n->dirs.n; i++) gnode_free(n->dirs.v[i]);
    strvec_free(&n->file_names);
    vec_free(&n->file_keys);
    strvec_free(&n->dir_names);
    vec_free(&n->dirs);
    free(n);
}

typedef struct {
    strvec_t *out;
    bool emitted;
    const strvec_t *files;   /* section keys (file paths) */
    const strvec_t *bodies;  /* per section: '\n'-joined model lines */
} gwalk_t;

static void gnode_walk(const gnode_t *n, int depth, gwalk_t *w) {
    for (size_t i = 0; i < n->file_names.n; i++) {
        if (w->emitted && depth == 0) strvec_push(w->out, "");
        w->emitted = true;
        sb_t h = SB_INIT;
        for (int k = 0; k <= depth; k++) sb_putc(&h, '#');
        sb_appendf(&h, " %s", n->file_names.v[i]);
        strvec_push_own(w->out, sb_detach(&h));
        const char *body = w->bodies->v[n->file_keys.v[i]];
        if (*body) {
            strvec_t ls = str_split(body, "\n");
            for (size_t k = 0; k < ls.n; k++) strvec_push(w->out, ls.v[k]);
            strvec_free(&ls);
        }
    }
    for (size_t i = 0; i < n->dirs.n; i++) {
        const gnode_t *d = n->dirs.v[i];
        sb_t name = SB_INIT;
        sb_append(&name, n->dir_names.v[i]);
        while (d->file_names.n == 0 && d->dirs.n == 1) {
            sb_appendf(&name, "/%s", d->dir_names.v[0]);
            d = d->dirs.v[0];
        }
        if (w->emitted) strvec_push(w->out, "");
        w->emitted = true;
        sb_t h = SB_INIT;
        for (int k = 0; k <= depth; k++) sb_putc(&h, '#');
        sb_appendf(&h, " %s/", sb_cstr(&name));
        strvec_push_own(w->out, sb_detach(&h));
        sb_free(&name);
        gnode_walk(d, depth + 1, w);
    }
}

static bool url_like(const char *p) {
    if (!isalpha((unsigned char)*p)) return false;
    const char *s = p + 1;
    while (isalnum((unsigned char)*s) || *s == '+' || *s == '.' || *s == '-') s++;
    return !strncmp(s, "://", 3);
}

/* formatGroupedDiagnosticMessages */
static char *format_grouped_diagnostics(const strvec_t *msgs) {
    strvec_t files = {0}, bodies = {0}, ungrouped = {0};
    VEC(sb_t) bsb = {0};
    for (size_t i = 0; i < msgs->n; i++) {
        ssize_t sp = diag_path_split(msgs->v[i]);
        if (sp < 0) {
            strvec_push(&ungrouped, msgs->v[i]);
            continue;
        }
        char *fp = xstrndup(msgs->v[i], (size_t)sp);
        for (char *q = fp; *q; q++)
            if (*q == '\\') *q = '/';
        size_t k;
        for (k = 0; k < files.n; k++)
            if (!strcmp(files.v[k], fp)) break;
        if (k == files.n) {
            strvec_push(&files, fp);
            sb_t nb = SB_INIT;
            vec_push(&bsb, nb);
        }
        if (bsb.v[k].len) sb_putc(&bsb.v[k], '\n');
        sb_appendf(&bsb.v[k], "  %s", msgs->v[i] + sp + 1);
        free(fp);
    }
    strvec_t out = {0};
    if (files.n) {
        for (size_t k = 0; k < bsb.n; k++) strvec_push(&bodies, sb_cstr(&bsb.v[k]));
        gnode_t *root = xcalloc(1, sizeof *root);
        for (size_t k = 0; k < files.n; k++) {
            const char *p = files.v[k];
            gnode_t *n = root;
            const char *leaf = p;
            if (!url_like(p)) {
                char *t = xstrdup(p);
                size_t tl = strlen(t);
                if (tl && t[tl - 1] == '/') t[tl - 1] = 0;
                strvec_t segs = str_split(t, "/");
                for (size_t s = 0; s + 1 < segs.n; s++) n = gnode_dir(n, segs.v[s]);
                leaf = p + (strrchr(t, '/') ? (size_t)(strrchr(t, '/') - t) + 1 : 0);
                bool dup = false;
                for (size_t f = 0; f < n->file_names.n; f++) dup |= !strcmp(n->file_names.v[f], leaf);
                if (!dup) {
                    char *lf = xstrndup(leaf, strlen(t) - (size_t)(leaf - p));
                    strvec_push_own(&n->file_names, lf);
                    vec_push(&n->file_keys, k);
                }
                strvec_free(&segs);
                free(t);
            } else {
                strvec_push(&n->file_names, p);
                vec_push(&n->file_keys, k);
            }
        }
        gwalk_t w = {&out, false, &files, &bodies};
        gnode_walk(root, 0, &w);
        gnode_free(root);
        if (ungrouped.n) {
            strvec_push(&out, "");
            for (size_t i = 0; i < ungrouped.n; i++) strvec_push(&out, ungrouped.v[i]);
        }
    } else {
        for (size_t i = 0; i < ungrouped.n; i++) strvec_push(&out, ungrouped.v[i]);
    }
    char *r = strvec_join(&out, "\n");
    strvec_free(&out);
    strvec_free(&files);
    bodies.n = 0;
    strvec_free(&bodies);
    for (size_t k = 0; k < bsb.n; k++) sb_free(&bsb.v[k]);
    vec_free(&bsb);
    strvec_free(&ungrouped);
    return r;
}

/* sort + dedupe diagnostics (by range + message) in place: returns borrowed pointers into `all` */
static void dedupe_sort(const json_t *all, jptrs_t *out) {
    json_t *d;
    json_arr_foreach(all, i, d) {
        json_t *r = json_get(d, "range");
        bool dup = false;
        for (size_t k = 0; k < out->n && !dup; k++) {
            json_t *o = out->v[k], *orr = json_get(o, "range");
            dup = pos_line(r, "start") == pos_line(orr, "start") && pos_char(r, "start") == pos_char(orr, "start") &&
                  pos_line(r, "end") == pos_line(orr, "end") && pos_char(r, "end") == pos_char(orr, "end") &&
                  str_eq(json_get_str(d, "message"), json_get_str(o, "message"));
        }
        if (!dup) vec_push(out, d);
    }
    if (out->n > 1) xqsort(out->v, out->n, sizeof(json_t *), cmp_diag);
}

static char *format_location(const json_t *loc, const char *cwd) {
    char *p = uri_to_path(json_get_str(loc, "uri"));
    char *f = rel_to_cwd(p, cwd);
    json_t *r = json_get(loc, "range");
    char *s = xasprintf("%s:%lld:%lld", f, (long long)pos_line(r, "start") + 1, (long long)pos_char(r, "start") + 1);
    free(f);
    free(p);
    return s;
}

/* readLocationContext + formatLocationWithContext */
static char *format_location_ctx(const json_t *loc, const char *cwd) {
    char *head = format_location(loc, cwd);
    char *p = uri_to_path(json_get_str(loc, "uri"));
    int64_t line = pos_line(json_get(loc, "range"), "start") + 1;
    size_t n;
    char *text = read_text(p, &n);
    sb_t b = SB_INIT;
    sb_appendf(&b, "  %s", head);
    if (text) {
        /* split(/\r?\n/) */
        strvec_t lines = {0};
        const char *s = text, *e = text + n;
        for (;;) {
            const char *nl = memchr(s, '\n', (size_t)(e - s));
            size_t len = nl ? (size_t)(nl - s) : (size_t)(e - s);
            if (nl && len && s[len - 1] == '\r') len--;
            strvec_push_own(&lines, xstrndup(s, len));
            if (!nl) break;
            s = nl + 1;
        }
        int64_t target = MAX(line, (int64_t)1);
        int64_t lo = MAX((int64_t)1, target - 1), hi = MIN((int64_t)lines.n, target + 1);
        for (int64_t k = lo; k <= hi; k++) sb_appendf(&b, "\n    %lld: %s", (long long)k, lines.v[k - 1]);
        strvec_free(&lines);
        free(text);
    }
    free(p);
    free(head);
    return sb_detach(&b);
}

static const char *symbol_icon(int64_t kind) {
    const char *bullet = sym("format.bullet", "*");
    switch (kind) {
    case 1: return sym("icon.file", bullet);
    case 2:
    case 3:
    case 23: return sym("icon.folder", bullet);
    case 4: return sym("icon.package", sym("icon.folder", bullet));
    case 5:
    case 11: return sym("icon.model", bullet);
    case 6:
    case 9:
    case 12: return sym("icon.auto", sym("format.dash", bullet));
    default: return bullet;
    }
}

static void format_document_symbol(sb_t *b, const json_t *s, int indent) {
    if (b->len) sb_putc(b, '\n');
    for (int k = 0; k < indent; k++) sb_append(b, "  ");
    const char *detail = json_get_str(s, "detail");
    sb_appendf(b, "%s %s%s%s @ line %lld", symbol_icon(json_get_int(s, "kind", 0)), json_get_str(s, "name") ? json_get_str(s, "name") : "",
               detail && *detail ? " " : "", detail && *detail ? detail : "", (long long)pos_line(json_get(s, "selectionRange"), "start") + 1);
    json_t *ch;
    if (indent < 64) json_arr_foreach(json_get_arr(s, "children"), i, ch) format_document_symbol(b, ch, indent + 1);
}

static char *format_symbol_information(const json_t *s, const char *cwd) {
    char *loc = format_location(json_get(s, "location"), cwd);
    const char *cn = json_get_str(s, "containerName");
    char *r = xasprintf("%s %s%s%s%s @ %s", symbol_icon(json_get_int(s, "kind", 0)), json_get_str(s, "name") ? json_get_str(s, "name") : "",
                        cn && *cn ? " (" : "", cn && *cn ? cn : "", cn && *cn ? ")" : "", loc);
    free(loc);
    return r;
}

static char *format_code_action(const json_t *a, size_t index) {
    const char *kind = json_get_str(a, "kind");
    bool preferred = json_get_bool(a, "isPreferred", false);
    json_t *dis = json_get(a, "disabled");
    char *dtxt = dis && json_truthy(dis) ? xasprintf(" (disabled: %s)", json_get_str(dis, "reason") ? json_get_str(dis, "reason") : "undefined")
                                          : xstrdup("");
    char *r = xasprintf("%zu: [%s] %s%s%s", index, kind && *kind ? kind : "action",
                        json_get_str(a, "title") ? json_get_str(a, "title") : "undefined", preferred ? " (preferred)" : "", dtxt);
    free(dtxt);
    return r;
}

/* formatWorkspaceEdit */
static void format_workspace_edit(const json_t *edit, const char *cwd, strvec_t *out) {
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(edit, "changes"), i, k, v) {
        char *p = uri_to_path(k), *f = rel_to_cwd(p, cwd);
        size_t n = json_len(v);
        strvec_push_own(out, xasprintf("%s: %zu edit%s", f, n, n > 1 ? "s" : ""));
        free(f);
        free(p);
    }
    json_t *dc;
    json_arr_foreach(json_get_arr(edit, "documentChanges"), i, dc) {
        if (json_get(dc, "edits") && json_get(dc, "textDocument")) {
            char *p = uri_to_path(json_get_str(json_get(dc, "textDocument"), "uri")), *f = rel_to_cwd(p, cwd);
            size_t n = json_len(json_get(dc, "edits"));
            strvec_push_own(out, xasprintf("%s: %zu edit%s", f, n, n > 1 ? "s" : ""));
            free(f);
            free(p);
        } else if (json_get_str(dc, "kind")) {
            const char *kind = json_get_str(dc, "kind");
            if (!strcmp(kind, "create") || !strcmp(kind, "delete")) {
                char *p = uri_to_path(json_get_str(dc, "uri")), *f = rel_to_cwd(p, cwd);
                strvec_push_own(out, xasprintf("%s: %s", !strcmp(kind, "create") ? "CREATE" : "DELETE", f));
                free(f);
                free(p);
            } else if (!strcmp(kind, "rename")) {
                char *a = uri_to_path(json_get_str(dc, "oldUri")), *fa = rel_to_cwd(a, cwd);
                char *b = uri_to_path(json_get_str(dc, "newUri")), *fb = rel_to_cwd(b, cwd);
                strvec_push_own(out, xasprintf("RENAME: %s %s %s", fa, sym("nav.cursor", ">"), fb));
                free(a);
                free(fa);
                free(b);
                free(fb);
            }
        }
    }
}

/* extractHoverText */
static void hover_text(sb_t *b, const json_t *c, int depth) {
    if (json_is_str(c)) sb_append(b, json_sv(c));
    else if (json_is_arr(c)) {
        json_t *x;
        json_arr_foreach(c, i, x) {
            if (i) sb_append(b, "\n\n");
            if (depth < 8) hover_text(b, x, depth + 1);
        }
    } else if (json_is_obj(c) && json_is_str(json_get(c, "value"))) sb_append(b, json_get_str(c, "value"));
    else if (json_is_obj(c)) sb_append(b, "[object Object]");
    else {
        char *s = js_string(c);
        sb_append(b, s);
        free(s);
    }
}

/* ---------- symbol columns (utils.ts resolveSymbolColumn) ---------- */

/* UTF-16 index of byte offset */
static int64_t utf16_index(const char *s, size_t byte_off) {
    int64_t u = 0;
    size_t i = 0, n = strlen(s);
    while (i < byte_off && i < n) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        if (k <= 0) k = 1;
        u += cp >= 0x10000 ? 2 : 1;
        i += (size_t)k;
    }
    return u;
}

static bool ident_char(int c) { return isalnum(c) || c == '_' || c == '$'; }

static bool bare_ident(const char *s) {
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_' || *s == '$')) return false;
    for (const char *p = s + 1; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '$')) return false;
    return true;
}

static void find_matches(const char *line, const char *sym_, bool icase, sizes_t *out) {
    size_t sl = strlen(sym_), ll = strlen(line);
    if (!sl) return;
    bool wb = bare_ident(sym_);
    size_t from = 0;
    while (from + sl <= ll) {
        const char *hit = NULL;
        for (size_t i = from; i + sl <= ll; i++)
            if (icase ? !strncasecmp(line + i, sym_, sl) : !strncmp(line + i, sym_, sl)) {
                hit = line + i;
                break;
            }
        if (!hit) break;
        size_t at = (size_t)(hit - line);
        if (wb) {
            bool before = at > 0 && ident_char((unsigned char)line[at - 1]);
            bool after = at + sl < ll && ident_char((unsigned char)line[at + sl]);
            if (before || after) {
                from = at + 1;
                continue;
            }
        }
        vec_push(out, at);
        from = at + sl;
    }
}

/* returns 0 and the UTF-16 column, or -1 with *err (owned; plain Error message) */
static int resolve_symbol_column(const char *path, int64_t line, const char *spec, int64_t *col, char **err) {
    int64_t ln = MAX(line, (int64_t)1);
    size_t n;
    struct stat st;
    if (stat(path, &st) != 0) {
        *err = xasprintf("File not found: %s", path);
        return -1;
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        const char *kind = S_ISFIFO(st.st_mode) ? "FIFO" : S_ISSOCK(st.st_mode) ? "socket"
                           : S_ISCHR(st.st_mode)  ? "character device" : "block device";
        *err = xasprintf("Cannot open '%s': it is a %s, not a regular file or directory.", path, kind);
        return -1;
    }
    char *text = read_text(path, &n);
    if (!text) {
        *err = xasprintf("EISDIR: illegal operation on a directory, read");
        return -1;
    }
    strvec_t lines = str_split(text, "\n");
    free(text);
    const char *target = ln - 1 < (int64_t)lines.n ? lines.v[ln - 1] : "";
    int rc = 0;
    if (!spec) {
        size_t k = 0;
        while (target[k] && isspace((unsigned char)target[k])) k++;
        *col = target[k] ? utf16_index(target, k) : 0;
    } else {
        char *symb = xstrdup(spec);
        int64_t occ = 1;
        char *hash = strrchr(symb, '#');
        if (hash && hash > symb && hash[1] && strspn(hash + 1, "0123456789") == strlen(hash + 1)) {
            occ = MAX(atoll(hash + 1), (long long)1);
            *hash = 0;
        }
        sizes_t idx = {0};
        find_matches(target, symb, false, &idx);
        if (!idx.n) find_matches(target, symb, true, &idx);
        if (!idx.n) {
            *err = xasprintf("Symbol \"%s\" not found on line %lld", symb, (long long)ln);
            rc = -1;
        } else if (occ > (int64_t)idx.n) {
            *err = xasprintf("Symbol \"%s\" occurrence %lld is out of bounds on line %lld (found %zu)", symb, (long long)occ,
                             (long long)ln, idx.n);
            rc = -1;
        } else {
            *col = utf16_index(target, idx.v[occ - 1]);
        }
        vec_free(&idx);
        free(symb);
    }
    strvec_free(&lines);
    return rc;
}

/* ---------- workspace edits (lsp/edits.ts) ---------- */

static int cmp_tedit_desc(const void *a, const void *b) {
    const tedit_t *x = a, *y = b;
    if (x->sl != y->sl) return x->sl < y->sl ? 1 : -1;
    if (x->sc != y->sc) return x->sc < y->sc ? 1 : -1;
    return x->index < y->index ? 1 : -1;
}

/* sortAndValidateTextEdits: 0 or -1 with *err (ToolError text) */
static int sort_validate(const json_t *edits, tedits_t *out, char **err) {
    json_t *e;
    json_arr_foreach(edits, i, e) {
        if (json_get_int(e, "insertTextFormat", 0) == 2) {
            *err = xstrdup("snippet-formatted LSP edits are unsupported");
            return -1;
        }
    }
    tedits_t all = {0};
    json_arr_foreach(edits, i, e) {
        json_t *r = json_get(e, "range");
        if (!r || !json_get(e, "newText")) continue;
        tedit_t t = {pos_line(r, "start"), pos_char(r, "start"), pos_line(r, "end"), pos_char(r, "end"),
                     json_get_str(e, "newText") ? json_get_str(e, "newText") : "", i};
        vec_push(&all, t);
    }
    if (all.n > 1) xqsort(all.v, all.n, sizeof(tedit_t), cmp_tedit_desc);
    for (size_t i = 0; i < all.n; i++) {
        tedit_t *t = &all.v[i];
        if (out->n) {
            tedit_t *p = &out->v[out->n - 1];
            bool empty = t->sl == t->el && t->sc == t->ec;
            if (!empty && p->sl == t->sl && p->sc == t->sc && p->el == t->el && p->ec == t->ec && !strcmp(p->text, t->text))
                continue;
        }
        vec_push(out, *t);
    }
    vec_free(&all);
    for (size_t i = 0; i + 1 < out->n; i++) {
        tedit_t *later = &out->v[i], *earlier = &out->v[i + 1];
        bool overlap = earlier->el > later->sl || (earlier->el == later->sl && earlier->ec > later->sc);
        if (overlap) {
            *err = xasprintf("overlapping LSP edits: %lld:%lld-%lld:%lld conflicts with %lld:%lld-%lld:%lld; LSP produced "
                             "inconsistent edits",
                             (long long)earlier->sl + 1, (long long)earlier->sc + 1, (long long)earlier->el + 1,
                             (long long)earlier->ec + 1, (long long)later->sl + 1, (long long)later->sc + 1,
                             (long long)later->el + 1, (long long)later->ec + 1);
            return -1;
        }
    }
    return 0;
}

/* byte offset of a UTF-16 index (JS String.slice semantics) */
static size_t u16_to_byte(const char *s, int64_t u16) {
    int64_t u = 0;
    size_t i = 0, n = strlen(s);
    while (i < n && u < u16) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        if (k <= 0) k = 1;
        u += cp >= 0x10000 ? 2 : 1;
        i += (size_t)k;
    }
    return i;
}

/* applyTextEditsToString */
static char *apply_text_edits_str(const char *content, const tedit_t *es, size_t ne) {
    strvec_t lines = str_split(content, "\n");
    for (size_t i = 0; i < ne; i++) {
        const tedit_t *t = &es[i];
        while ((int64_t)lines.n <= MAX(t->sl, t->el)) strvec_push(&lines, ""); /* lines[n] || "" */
        const char *sline = lines.v[t->sl], *eline = lines.v[t->el];
        size_t sb2 = u16_to_byte(sline, t->sc), eb = u16_to_byte(eline, t->ec);
        if (eb > strlen(eline)) eb = strlen(eline);
        char *merged = xasprintf("%.*s%s%s", (int)sb2, sline, t->text, eline + eb);
        if (t->sl == t->el) {
            free(lines.v[t->sl]);
            lines.v[t->sl] = merged;
            continue;
        }
        strvec_t nl = str_split(merged, "\n");
        free(merged);
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
    }
    char *r = strvec_join(&lines, "\n");
    strvec_free(&lines);
    return r;
}

static int apply_text_edits_file(const char *path, const tedit_t *es, size_t ne, char **err) {
    size_t n;
    char *text = read_text(path, &n);
    if (!text) {
        *err = xasprintf("ENOENT: no such file or directory, open '%s'", path);
        return -1;
    }
    char *out = apply_text_edits_str(text, es, ne);
    free(text);
    int rc = fs_write_atomic(path, out, strlen(out), 0644);
    free(out);
    if (rc) *err = xasprintf("failed to write %s", path);
    return rc ? -1 : 0;
}

/* applyWorkspaceEdit: appends omp's applied summaries; 0 or -1 with *err */
static int apply_workspace_edit(const json_t *edit, const char *cwd, strvec_t *applied, char **err) {
    *err = NULL;
    json_t *dcs = json_get_arr(edit, "documentChanges");
    if (dcs) {
        /* planDocumentChanges: accumulate text edits per uri; flush before resource ops touching them */
        typedef struct {
            int kind; /* 0 text, 1 create, 2 rename, 3 delete */
            char *uri, *uri2;
            json_t *edits;
            const json_t *opts;
        } op_t;
        VEC(op_t) ops = {0};
        json_t *pending = json_obj(); /* uri -> [edits] */
#define FLUSH_URI(U)                                                                 \
    do {                                                                             \
        json_t *pe_ = json_take(pending, (U));                                       \
        if (pe_) {                                                                   \
            op_t o_ = {0, xstrdup(U), NULL, pe_, NULL};                              \
            vec_push(&ops, o_);                                                      \
        }                                                                            \
    } while (0)
        json_t *dc;
        json_arr_foreach(dcs, i, dc) {
            if (json_get(dc, "textDocument") && json_get(dc, "edits")) {
                const char *u = json_get_str(json_get(dc, "textDocument"), "uri");
                if (!u) continue;
                json_t *te = json_arr(), *x;
                json_arr_foreach(json_get_arr(dc, "edits"), k, x) if (json_get(x, "range") && json_get(x, "newText"))
                    json_push(te, json_clone(x));
                if (!json_len(te)) {
                    json_free(te);
                    continue;
                }
                json_t *prev = json_get(pending, u);
                if (prev) {
                    json_t *y;
                    json_arr_foreach(te, k, y) json_push(prev, json_clone(y));
                    json_free(te);
                } else json_set(pending, u, te);
            } else if (json_get_str(dc, "kind")) {
                const char *kind = json_get_str(dc, "kind");
                const char *u = json_get_str(dc, !strcmp(kind, "rename") ? "oldUri" : "uri");
                if (!u) continue;
                if (!strcmp(kind, "create")) {
                    FLUSH_URI(u);
                    op_t o = {1, xstrdup(u), NULL, NULL, json_get(dc, "options")};
                    vec_push(&ops, o);
                } else {
                    const char *u2 = !strcmp(kind, "rename") ? json_get_str(dc, "newUri") : NULL;
                    for (int side = 0; side < (u2 ? 2 : 1); side++) {
                        const char *root = side ? u2 : u;
                        char *prefix = str_ends(root, "/") ? xstrdup(root) : xasprintf("%s/", root);
                        strvec_t hits = {0};
                        const char *pk;
                        json_t *pv;
                        json_obj_foreach(pending, k, pk, pv) if (!strcmp(pk, root) || str_starts(pk, prefix)) strvec_push(&hits, pk);
                        for (size_t h = 0; h < hits.n; h++) FLUSH_URI(hits.v[h]);
                        strvec_free(&hits);
                        free(prefix);
                    }
                    op_t o = {!strcmp(kind, "rename") ? 2 : 3, xstrdup(u), u2 ? xstrdup(u2) : NULL, NULL, json_get(dc, "options")};
                    if (o.kind == 2 && !u2) {
                        free(o.uri);
                        continue;
                    }
                    vec_push(&ops, o);
                }
            }
        }
        {
            strvec_t rest = {0};
            const char *pk;
            json_t *pv;
            json_obj_foreach(pending, k, pk, pv) strvec_push(&rest, pk);
            for (size_t h = 0; h < rest.n; h++) FLUSH_URI(rest.v[h]);
            strvec_free(&rest);
        }
#undef FLUSH_URI
        json_free(pending);
        int rc = 0;
        for (size_t i = 0; i < ops.n && !rc; i++) {
            if (ops.v[i].kind != 0) continue;
            tedits_t es = {0};
            if (sort_validate(ops.v[i].edits, &es, err)) rc = -1;
            vec_free(&es);
        }
        for (size_t i = 0; i < ops.n && !rc; i++) {
            op_t *o = &ops.v[i];
            char *p = uri_to_path(o->uri);
            char *rel = rel_to_cwd(p, cwd);
            if (o->kind == 0) {
                tedits_t es = {0};
                sort_validate(o->edits, &es, err);
                if (apply_text_edits_file(p, es.v, es.n, err)) rc = -1;
                else strvec_push_own(applied, xasprintf("Applied %zu edit(s) to %s", json_len(o->edits), rel));
                vec_free(&es);
            } else if (o->kind == 1) {
                char *dir = path_dirname(p);
                fs_mkdir_p(dir, 0755);
                free(dir);
                bool overwrite = json_get_bool(o->opts, "overwrite", false);
                bool ignore = json_get_bool(o->opts, "ignoreIfExists", false);
                if (!overwrite && fs_exists(p)) {
                    if (!ignore) {
                        *err = xasprintf("EEXIST: file already exists, open '%s'", p);
                        rc = -1;
                    }
                } else if (fs_write_atomic(p, "", 0, 0644) == 0) {
                    strvec_push_own(applied, xasprintf("Created %s", rel));
                } else {
                    *err = xasprintf("failed to create %s", p);
                    rc = -1;
                }
            } else if (o->kind == 2) {
                char *np = uri_to_path(o->uri2), *nrel = rel_to_cwd(np, cwd);
                char *dir = path_dirname(np);
                fs_mkdir_p(dir, 0755);
                free(dir);
                bool skip = false;
                struct stat st;
                if (strcmp(p, np) != 0 && lstat(np, &st) == 0) {
                    if (!json_get_bool(o->opts, "overwrite", false)) {
                        if (json_get_bool(o->opts, "ignoreIfExists", false)) skip = true;
                        else {
                            *err = xasprintf("rename target already exists: %s", nrel);
                            rc = -1;
                        }
                    }
                }
                if (!rc && !skip) {
                    if (strcmp(p, np) != 0 && rename(p, np) != 0) {
                        *err = xasprintf("%s: %s, rename '%s' -> '%s'", errno == ENOENT ? "ENOENT" : "EIO", strerror(errno), p, np);
                        rc = -1;
                    } else {
                        strvec_push_own(applied, xasprintf("Renamed %s %s %s", rel, "\xe2\x86\x92", nrel));
                    }
                }
                free(np);
                free(nrel);
            } else {
                struct stat st;
                if (lstat(p, &st) != 0) {
                    if (!json_get_bool(o->opts, "ignoreIfNotExists", false)) {
                        *err = xasprintf("ENOENT: no such file or directory, lstat '%s'", p);
                        rc = -1;
                    }
                } else {
                    int r2;
                    if (S_ISDIR(st.st_mode) && json_get_bool(o->opts, "recursive", false)) {
                        char *cmd = NULL;
                        const char *argv[] = {"rm", "-rf", "--", p, NULL};
                        proc_result_t pr;
                        proc_opts_t po = {0};
                        r2 = proc_run(argv, &po, &pr) == 0 && pr.exit_code == 0 ? 0 : -1;
                        proc_result_free(&pr);
                        free(cmd);
                    } else r2 = S_ISDIR(st.st_mode) ? rmdir(p) : unlink(p);
                    if (r2) {
                        *err = xasprintf("failed to delete %s", p);
                        rc = -1;
                    } else strvec_push_own(applied, xasprintf("Deleted %s", rel));
                }
            }
            free(rel);
            free(p);
        }
        for (size_t i = 0; i < ops.n; i++) {
            free(ops.v[i].uri);
            free(ops.v[i].uri2);
            json_free(ops.v[i].edits);
        }
        vec_free(&ops);
        return rc;
    }
    json_t *changes = json_get_obj(edit, "changes");
    if (changes) {
        const char *k;
        json_t *v;
        json_obj_foreach(changes, i, k, v) {
            tedits_t es = {0};
            int r = sort_validate(v, &es, err);
            vec_free(&es);
            if (r) return -1;
        }
        json_obj_foreach(changes, i, k, v) {
            if (!json_len(v)) continue;
            tedits_t es = {0};
            sort_validate(v, &es, err);
            char *p = uri_to_path(k), *rel = rel_to_cwd(p, cwd);
            int r = apply_text_edits_file(p, es.v, es.n, err);
            if (!r) strvec_push_own(applied, xasprintf("Applied %zu edit(s) to %s", json_len(v), rel));
            free(p);
            free(rel);
            vec_free(&es);
            if (r) return -1;
        }
    }
    return 0;
}

/* collect uris touched by an edit (for overlay reconciliation) */
static void edit_touched(const json_t *edit, strvec_t *changed, strvec_t *deleted, strvec_t *created) {
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(edit, "changes"), i, k, v) strvec_push(changed, k);
    json_t *dc;
    json_arr_foreach(json_get_arr(edit, "documentChanges"), i, dc) {
        const char *kind = json_get_str(dc, "kind");
        if (!kind) {
            const char *u = json_get_str(json_get(dc, "textDocument"), "uri");
            if (u) strvec_push(changed, u);
        } else if (!strcmp(kind, "create") && json_get_str(dc, "uri")) strvec_push(created, json_get_str(dc, "uri"));
        else if (!strcmp(kind, "delete") && json_get_str(dc, "uri")) strvec_push(deleted, json_get_str(dc, "uri"));
        else if (!strcmp(kind, "rename") && json_get_str(dc, "oldUri") && json_get_str(dc, "newUri")) {
            strvec_push(deleted, json_get_str(dc, "oldUri"));
            strvec_push(created, json_get_str(dc, "newUri"));
        }
    }
}

static void notify_watched(lsp_manager_t *m, const strvec_t *uris, const int *types);

/* applyWorkspaceEditWithLsp: apply, then reconcile every ready client's overlays and watchers */
static int apply_workspace_edit_lsp(lsp_manager_t *m, const json_t *edit, strvec_t *applied, char **err) {
    int rc = apply_workspace_edit(edit, m->cwd, applied, err);
    strvec_t changed = {0}, deleted = {0}, created = {0};
    edit_touched(edit, &changed, &deleted, &created);
    pthread_mutex_lock(&m->mu);
    for (size_t ci = 0; ci < m->clients.n; ci++) {
        lsp_client_t *c = m->clients.v[ci];
        if (!c->alive) continue;
        for (size_t d = 0; d < deleted.n; d++) {
            strvec_t victims = {0};
            char *prefix = str_ends(deleted.v[d], "/") ? xstrdup(deleted.v[d]) : xasprintf("%s/", deleted.v[d]);
            for (size_t i = 0; i < c->open.n; i++)
                if (!strcmp(c->open.v[i].uri, deleted.v[d]) || str_starts(c->open.v[i].uri, prefix)) strvec_push(&victims, c->open.v[i].uri);
            for (size_t i = 0; i < victims.n; i++) close_document(c, victims.v[i]);
            strvec_free(&victims);
            free(prefix);
        }
        strvec_t fin = {0};
        for (size_t i = 0; i < changed.n; i++) strvec_push(&fin, changed.v[i]);
        for (size_t i = 0; i < created.n; i++) strvec_push(&fin, created.v[i]);
        for (size_t i = 0; i < fin.n; i++) {
            if (!open_find(c, fin.v[i])) continue;
            char *p = uri_to_path(fin.v[i]);
            refresh_document(c, p, RF_USE);
            free(p);
        }
        strvec_free(&fin);
    }
    pthread_mutex_unlock(&m->mu);
    strvec_t all = {0};
    VEC(int) types = {0};
    for (size_t i = 0; i < changed.n; i++) {
        strvec_push(&all, changed.v[i]);
        vec_push(&types, 2);
    }
    for (size_t i = 0; i < deleted.n; i++) {
        strvec_push(&all, deleted.v[i]);
        vec_push(&types, 3);
    }
    for (size_t i = 0; i < created.n; i++) {
        strvec_push(&all, created.v[i]);
        vec_push(&types, 1);
    }
    if (rc == 0) notify_watched(m, &all, types.v);
    strvec_free(&all);
    vec_free(&types);
    strvec_free(&changed);
    strvec_free(&deleted);
    strvec_free(&created);
    return rc;
}

/* notifyWorkspaceWatchedFiles: didChangeWatchedFiles to ready clients serving the paths */
static void notify_watched(lsp_manager_t *m, const strvec_t *uris, const int *types) {
    if (!uris->n) return;
    pthread_mutex_lock(&m->mu);
    for (size_t ci = 0; ci < m->clients.n; ci++) {
        lsp_client_t *c = m->clients.v[ci];
        if (!c->alive || strcmp(c->status, "ready")) continue;
        json_t *changes = json_arr();
        bool all_changed = true;
        for (size_t i = 0; i < uris->n; i++) {
            char *p = uri_to_path(uris->v[i]);
            char *rel = rel_to_cwd(p, c->root);
            bool inside = rel[0] != '/' && strncmp(rel, "..", 2) != 0;
            free(rel);
            free(p);
            if (!inside) continue;
            diags_delete(c, uris->v[i]);
            json_t *ch = json_obj();
            json_set_str(ch, "uri", uris->v[i]);
            json_set_int(ch, "type", types[i]);
            json_push(changes, ch);
            if (types[i] != 2) all_changed = false;
        }
        if (!json_len(changes)) {
            json_free(changes);
            continue;
        }
        json_t *p = json_obj();
        json_set(p, "changes", changes);
        notify(c, "workspace/didChangeWatchedFiles", p);
        if (!all_changed) refresh_open_files(c);
    }
    pthread_mutex_unlock(&m->mu);
}

/* ---------- status / after-write ---------- */

/* probeRustupProxy (omp 290d787845): a rustup proxy ($CARGO_HOME/bin/<tool>) resolves on PATH even when the active
 * toolchain lacks the component, then exits at startup. Its error (first two lines joined), or NULL. */
static char *probe_rustup_proxy(const char *command) {
    const char *ch = getenv("CARGO_HOME");
    char *cargo_bin = ch && *ch ? path_join(ch, "bin") : path_join3(home_dir(), ".cargo", "bin");
    char *dir = path_dirname(command);
    bool proxy = !strcmp(dir, cargo_bin);
    free(dir);
    free(cargo_bin);
    if (!proxy) return NULL;
    const char *argv[] = {command, "--version", NULL};
    proc_opts_t o = {.timeout_ms = 5000, .merge_stderr = false};
    proc_result_t r;
    if (proc_run(argv, &o, &r) != 0) return NULL;
    char *res = NULL;
    if (!r.timed_out && !(r.exit_code == 0 && !r.term_signal)) {
        const char *src = r.err && *r.err ? r.err : r.out ? r.out : "";
        char *t = str_trim(src);
        strvec_t ls = str_split(t, "\n");
        sb_t b = SB_INIT;
        for (size_t i = 0; i < ls.n && i < 2; i++) sb_appendf(&b, "%s%s", i ? " " : "", ls.v[i]);
        res = b.len ? sb_detach(&b) : xasprintf("exit code %d", r.exit_code);
        sb_free(&b);
        strvec_free(&ls);
        free(t);
    }
    proc_result_free(&r);
    return res;
}

char *lsp_status(lsp_manager_t *m) {
    sb_t b = SB_INIT;
    pthread_mutex_lock(&m->mu);
    const char *k;
    json_t *v;
    json_obj_foreach(m->servers, i, k, v) {
        lsp_client_t *c = find_client(m, k);
        if (c) {
            sb_appendf(&b, "%s%s (%s)", b.len ? ", " : "", k, c->alive ? c->status : "error");
            continue;
        }
        const char *cmd = json_get_str(v, "resolvedCommand") ? json_get_str(v, "resolvedCommand") : json_get_str(v, "command");
        char *proxy_err = cmd ? probe_rustup_proxy(cmd) : NULL;
        if (proxy_err) sb_appendf(&b, "%s%s (unusable: %s)", b.len ? ", " : "", k, proxy_err);
        else sb_appendf(&b, "%s%s (configured, not started)", b.len ? ", " : "", k);
        free(proxy_err);
    }
    pthread_mutex_unlock(&m->mu);
    if (!b.len) sb_append(&b, "No language servers configured for this project");
    return sb_detach(&b);
}

/* diagnostics-ledger.ts DiagnosticsLedger.reduce: keeps only messages not seen for the file last time */
static void ledger_reduce(lsp_manager_t *m, const char *abs, strvec_t *msgs, char **summary, bool *errored) {
    json_t *prev = json_get(m->ledger, abs);
    json_t *cur = json_arr();
    strvec_t fresh = {0};
    for (size_t i = 0; i < msgs->n; i++) {
        const char *msg = msgs->v[i];
        /* identity: strip /^.*?:\d+:\d+\s+/ */
        const char *id = msg;
        for (const char *p = msg; *p && *p != '\n'; p++) {
            if (*p != ':' || !isdigit((unsigned char)p[1])) continue;
            const char *q = p + 1;
            while (isdigit((unsigned char)*q)) q++;
            if (*q != ':' || !isdigit((unsigned char)q[1])) continue;
            q++;
            while (isdigit((unsigned char)*q)) q++;
            if (!isspace((unsigned char)*q)) continue;
            while (isspace((unsigned char)*q)) q++;
            id = q;
            break;
        }
        bool seen = false, dup = false;
        json_t *x;
        json_arr_foreach(prev, k, x) if (str_eq(json_sv(x), id)) seen = true;
        json_arr_foreach(cur, k, x) if (str_eq(json_sv(x), id)) dup = true;
        if (!dup) json_push_str(cur, id);
        if (!seen) strvec_push(&fresh, msg);
    }
    if (!json_len(cur)) {
        json_del(m->ledger, abs);
        json_free(cur);
    } else json_set(m->ledger, abs, cur);
    if (fresh.n != msgs->n) {
        free(*summary);
        *summary = summarize_messages(&fresh, errored);
        strvec_free(msgs);
        *msgs = fresh;
    } else strvec_free(&fresh);
}

/* writethrough + getDiagnosticsForFile for a file the write/edit tool just committed */
char *lsp_after_write(lsp_manager_t *m, const char *abs, int64_t wait_ms) {
    if (!m) return NULL;
    names_t names = {0};
    activate_nested(m, abs), servers_for_file(m->servers, abs, &names, true);
    if (!names.n) {
        vec_free(&names);
        return NULL;
    }
    if (wait_ms <= 0) wait_ms = WRITETHROUGH_DIAGNOSTICS_WAIT_MS;
    size_t n = 0;
    char *content = read_text(abs, &n);
    if (!content) {
        vec_free(&names);
        return NULL;
    }
    char *uri = path_to_uri(abs);
    json_t *all = json_arr();
    strvec_t reported = {0}, unreported = {0};
    for (size_t i = 0; i < names.n; i++) {
        budget_t init = {mono_ms() + 5000, false, NULL};
        char *err = NULL;
        lsp_client_t *c = get_client(m, names.v[i], &init, &err);
        free(err);
        if (!c) continue;
        pthread_mutex_lock(&c->mu);
        int64_t min_version = c->diag_version;
        pthread_mutex_unlock(&c->mu);
        reconcile_open_files(c, uri, false, NULL);
        bool was_open = open_find(c, uri) != NULL;
        if (!was_open) {
            /* created/changed path announced to the server's watchers */
            json_t *p = json_obj(), *chs = json_arr(), *ch = json_obj();
            json_set_str(ch, "uri", uri);
            json_set_int(ch, "type", 2);
            json_push(chs, ch);
            json_set(p, "changes", chs);
            notify(c, "workspace/didChangeWatchedFiles", p);
        }
        sync_content(c, abs, content);
        if (was_open) {
            json_t *p = json_obj(), *chs = json_arr(), *ch = json_obj();
            json_set_str(ch, "uri", uri);
            json_set_int(ch, "type", 2);
            json_push(chs, ch);
            json_set(p, "changes", chs);
            notify(c, "workspace/didChangeWatchedFiles", p);
        }
        int64_t expected = open_version(c, abs);
        send_did_save(c, uri, content);
        budget_t bg = {mono_ms() + wait_ms + 10000, false, NULL};
        char *failed = NULL;
        json_t *items = wait_for_diagnostics(c, uri, wait_ms, &bg, min_version, expected, project_aware(names.v[i], c->cfg), &failed);
        free(failed);
        if (!items) {
            strvec_push(&unreported, names.v[i]);
            continue;
        }
        strvec_push(&reported, names.v[i]);
        json_t *d;
        json_arr_foreach(items, k, d) json_push(all, json_clone(d));
        json_free(items);
    }
    free(uri);
    free(content);
    vec_free(&names);
    char *out = NULL;
    if (reported.n || unreported.n) {
        jptrs_t uniq = {0};
        dedupe_sort(all, &uniq);
        char *rel = rel_to_cwd(abs, m->cwd);
        strvec_t msgs = {0};
        for (size_t i = 0; i < uniq.n && i < DIAGNOSTIC_MESSAGE_LIMIT; i++) strvec_push_own(&msgs, format_diagnostic(uniq.v[i], rel));
        char *summary = diagnostics_summary(uniq.v, uniq.n);
        bool errored = false;
        if (settings_bool("lsp.diagnosticsDeduplicate", true)) ledger_reduce(m, abs, &msgs, &summary, &errored);
        if (unreported.n) {
            char *names_s = strvec_join(&unreported, ", ");
            char *s2 = msgs.n ? xasprintf("%s; no diagnostics report yet from %s", summary, names_s)
                              : xasprintf("no diagnostics report yet from %s", names_s);
            free(summary);
            summary = s2;
            free(names_s);
        }
        if (msgs.n) {
            char *g = format_grouped_diagnostics(&msgs);
            out = xasprintf("LSP Diagnostics (%s):\n%s", summary, g);
            free(g);
        }
        free(summary);
        strvec_free(&msgs);
        free(rel);
        vec_free(&uniq);
    }
    json_free(all);
    strvec_free(&reported);
    strvec_free(&unreported);
    return out;
}

/* ---------- workspace diagnostics (workspace-diagnostics.ts) ---------- */

static const char *sig_name(int s) {
    switch (s) {
    case SIGTERM: return "SIGTERM";
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGINT: return "SIGINT";
    case SIGBUS: return "SIGBUS";
    default: return "SIGKILL";
    }
}

static char *run_checker(const char *cwd, const char *const *argv, budget_t *bg) {
    proc_opts_t po = {0};
    po.cwd = cwd;
    po.cancel = bg ? bg->cancel : NULL;
    if (bg && bg->deadline) po.timeout_ms = MAX((int64_t)1, bg->deadline - mono_ms());
    proc_result_t pr;
    int rc = proc_run(argv, &po, &pr);
    sb_t cmd = SB_INIT;
    for (const char *const *a = argv; *a; a++) sb_appendf(&cmd, "%s%s", cmd.len ? " " : "", *a);
    char *r;
    if (rc != 0 || pr.spawn_errno) {
        r = xasprintf("Failed to run %s: ENOENT: no such file or directory, posix_spawn '%s'", sb_cstr(&cmd), argv[0]);
    } else {
        char *comb = xasprintf("%s%s", pr.out ? pr.out : "", pr.err ? pr.err : "");
        char *t = str_trim(comb);
        free(comb);
        if (!*t) {
            if (pr.exit_code == 0) r = xstrdup("No issues found");
            else if (pr.term_signal) r = xasprintf("Failed to run %s: the checker was killed by %s without reporting anything, so the workspace was not verified", sb_cstr(&cmd), sig_name(pr.term_signal));
            else r = xasprintf("Failed to run %s: the checker exited with code %d without reporting anything, so the workspace was not verified", sb_cstr(&cmd), pr.exit_code);
        } else {
            strvec_t ls = str_split(t, "\n");
            if (ls.n > 50) {
                sb_t b = SB_INIT;
                for (size_t i = 0; i < 50; i++) sb_appendf(&b, "%s%s", i ? "\n" : "", ls.v[i]);
                sb_appendf(&b, "\n[\xe2\x80\xa6%zuln elided\xe2\x80\xa6]", ls.n - 50);
                r = sb_detach(&b);
            } else r = xstrdup(t);
            strvec_free(&ls);
        }
        free(t);
    }
    sb_free(&cmd);
    proc_result_free(&pr);
    return r;
}

static char *workspace_diagnostics(const char *cwd, budget_t *bg) {
    typedef struct {
        const char *desc;
        const char *argv[5];
    } pt_t;
    pt_t pts[4];
    int n = 0;
#define MARK(f) ({ char *p_ = path_join(cwd, f); bool e_ = fs_exists(p_); free(p_); e_; })
    if (MARK("Cargo.toml")) pts[n++] = (pt_t){"Rust (cargo check)", {"cargo", "check", "--message-format=short", NULL}};
    if (MARK("tsconfig.json")) pts[n++] = (pt_t){"TypeScript (tsc --noEmit)", {"npx", "tsc", "--noEmit", NULL}};
    if (MARK("go.work")) pts[n++] = (pt_t){"Go workspace (go build)", {"go", "build", "./...", NULL}};
    else if (MARK("go.mod")) pts[n++] = (pt_t){"Go (go build)", {"go", "build", "./...", NULL}};
    if (MARK("pyproject.toml") || MARK("pyrightconfig.json")) pts[n++] = (pt_t){"Python (pyright)", {"pyright", NULL}};
#undef MARK
    if (!n) {
        return xstrdup("Workspace diagnostics (Unknown project type):\nCannot detect project type. Supported: Rust (Cargo.toml), "
                       "TypeScript (tsconfig.json), Go (go.work/go.mod), Python (pyproject.toml)");
    }
    sb_t desc = SB_INIT, out = SB_INIT;
    for (int i = 0; i < n; i++) {
        sb_appendf(&desc, "%s%s", i ? " + " : "", pts[i].desc);
        char *o = run_checker(cwd, pts[i].argv, bg);
        if (n == 1) sb_append(&out, o);
        else sb_appendf(&out, "%s=== %s ===\n%s", i ? "\n\n" : "", pts[i].desc, o);
        free(o);
    }
    char *r = xasprintf("Workspace diagnostics (%s):\n%s", sb_cstr(&desc), sb_cstr(&out));
    sb_free(&desc);
    sb_free(&out);
    return r;
}

/* ---------- tool ---------- */

typedef struct {
    tool_env_t *env;
    lsp_manager_t *m;
} lsp_tool_t;

static void details(tool_result_t *out, const char *action, const char *server, bool success, const json_t *request_) {
    json_free(out->details);
    out->details = json_obj();
    if (server) json_set_str(out->details, "serverName", server);
    json_set_str(out->details, "action", action);
    json_set_bool(out->details, "success", success);
    if (request_) json_set(out->details, "request", json_clone(request_));
}

static void reply(tool_result_t *out, const char *text, const char *action, const char *server, bool success,
                  const json_t *request_) {
    tool_result_text(out, text);
    details(out, action, server, success, request_);
}

/* JSC JSON.parse error text for the request payload */
static char *jsc_json_error(const char *s) {
    char *err = NULL;
    json_t *j = json_parse(s, strlen(s), &err);
    json_free(j);
    if (!err) return NULL;
    free(err);
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) return xstrdup("JSON Parse error: Unexpected EOF");
    if (*p == '{') {
        const char *q = p + 1;
        while (isspace((unsigned char)*q)) q++;
        if (!*q) return xstrdup("JSON Parse error: Unexpected EOF");
        if (*q != '"' && *q != '}') return xstrdup("JSON Parse error: Expected '}'");
    }
    if (isalpha((unsigned char)*p)) {
        size_t n = strspn(p, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$");
        return xasprintf("JSON Parse error: Unexpected identifier \"%.*s\"", (int)n, p);
    }
    return xstrdup("JSON Parse error: Unexpected EOF");
}

typedef struct {
    lsp_tool_t *lt;
    const json_t *args;
    tool_result_t *out;
    const char *action;
    int64_t timeout_sec, wait_ms;
    budget_t bg;
    bool aborted;
} call_t;

/* rethrowAbort: true (and the ToolError written) when the call's deadline or a cancel ended a request */
static bool deadline_hit(call_t *k, const char *server) {
    if (!budget_expired(&k->bg) && !k->bg.cancelled) return false;
    if (k->bg.cancel && *k->bg.cancel) {
        tool_result_error(k->out, "Operation aborted");
    } else {
        tool_result_error(k->out,
                          "LSP %s timed out after %llds%s%s. The server may still be indexing; try again or pass timeout=<larger>.",
                          k->action, (long long)k->timeout_sec, server ? " on " : "", server ? server : "");
    }
    k->aborted = true;
    return true;
}

static void do_status(call_t *k) {
    lsp_manager_t *m = k->lt->m;
    char *s;
    if (!json_len(m->servers)) s = xstrdup("No language servers configured for this project");
    else {
        char *l = lsp_status(m);
        s = xasprintf("Language servers: %s\n  note: 'configured, not started' means the binary resolves on PATH but no request "
                      "has spawned it yet; 'ready' means a client process is live for this cwd.",
                      l);
        free(l);
    }
    reply(k->out, s, k->action, NULL, true, k->args);
    free(s);
}

/* collectGlobMatches: Bun.Glob(pattern).scan({cwd}) — files only, dot entries skipped, directory read order */
static void glob_scan(const char *dir, const char *rel, const glob_pat_t *g, int depth, strvec_t *out, bool *truncated) {
    if (*truncated || depth <= 0) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while (!*truncated && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *p = path_join(dir, e->d_name);
        char *r = *rel ? xasprintf("%s/%s", rel, e->d_name) : xstrdup(e->d_name);
        struct stat st;
        if (stat(p, &st) == 0) {
            if (S_ISDIR(st.st_mode)) glob_scan(p, r, g, depth - 1, out, truncated);
            else if (glob_pat_match(g, r)) {
                if (out->n >= MAX_GLOB_DIAGNOSTIC_TARGETS) *truncated = true;
                else strvec_push(out, r);
            }
        }
        free(p);
        free(r);
    }
    closedir(d);
}

static void do_diagnostics(call_t *k) {
    lsp_manager_t *m = k->lt->m;
    const char *file = json_get_str(k->args, "file");
    const char *cwd = k->lt->env->cwd;
    if (str_eq(file, "*")) {
        char *s = workspace_diagnostics(cwd, &k->bg);
        reply(k->out, s, k->action, NULL, true, k->args);
        free(s);
        return;
    }
    if (!file || !*file) {
        reply(k->out, "Error: file parameter required. Use `*` for workspace-wide diagnostics or a path/glob for specific files.",
              k->action, NULL, false, k->args);
        return;
    }
    /* resolveDiagnosticTargets */
    strvec_t targets = {0};
    bool truncated = false;
    char *resolved0 = resolve_to_cwd(file, cwd);
    if (!strpbrk(file, "*?[{") || fs_is_file(resolved0)) {
        strvec_push(&targets, file);
    } else {
        glob_pat_t *g = glob_compile(file, GLOB_PATHNAME);
        int depth = glob_depth_bound(file);
        glob_scan(cwd, "", g, depth < 0 ? 64 : depth, &targets, &truncated);
        glob_pat_free(g);
    }
    free(resolved0);
    if (!targets.n) {
        char *s = xasprintf("No files matched pattern: %s", file);
        reply(k->out, s, k->action, NULL, true, k->args);
        free(s);
        strvec_free(&targets);
        return;
    }
    bool detailed = targets.n > 1 || truncated;
    strvec_t results = {0};
    strvec_t all_servers = {0};
    int attempts = 0, failures = 0;
    if (truncated)
        strvec_push_own(&results, xasprintf("%s Pattern matched more than %d files; showing first %d. Narrow the glob or use "
                                            "workspace diagnostics.",
                                            sym("status.warning", "!"), MAX_GLOB_DIAGNOSTIC_TARGETS, MAX_GLOB_DIAGNOSTIC_TARGETS));
    for (size_t t = 0; t < targets.n; t++) {
        if (deadline_hit(k, NULL)) goto done;
        char *resolved = resolve_to_cwd(targets.v[t], cwd);
        names_t servers = {0};
        activate_nested(m, resolved), servers_for_file(m->servers, resolved, &servers, false);
        if (!servers.n) {
            char *why = describe_missing(m, resolved);
            strvec_push_own(&results, xasprintf("%s %s: %s", sym("status.error", "x"), targets.v[t], why));
            free(why);
            free(resolved);
            vec_free(&servers);
            continue;
        }
        char *uri = path_to_uri(resolved);
        char *rel = rel_to_cwd(resolved, cwd);
        json_t *all = json_arr();
        strvec_t failed = {0}, unreported = {0}, reported = {0};
        int succeeded = 0;
        bool stop = false;
        for (size_t s = 0; s < servers.n; s++) {
            const char *sn = servers.v[s];
            bool known = false;
            for (size_t q = 0; q < all_servers.n; q++) known |= !strcmp(all_servers.v[q], sn);
            if (!known) strvec_push(&all_servers, sn);
            attempts++;
            if (has_custom_client(sn)) { /* custom linter clients (biome/swiftlint CLI) are not ported */
                strvec_push(&failed, sn);
                failures++;
                continue;
            }
            char *err = NULL;
            lsp_client_t *c = get_client(m, sn, &k->bg, &err);
            if (!c) {
                free(err);
                if (deadline_hit(k, sn)) {
                    stop = true;
                    break;
                }
                strvec_push(&failed, sn);
                failures++;
                continue;
            }
            reconcile_open_files(c, uri, false, NULL);
            pthread_mutex_lock(&c->mu);
            int64_t min_version = c->diag_version;
            bool have_build = c->build_status != NULL;
            int64_t since = c->build_seq;
            pthread_mutex_unlock(&c->mu);
            int64_t rebuild_since = -1;
            if (supports_pull(c)) {
                reconcile_file(c, resolved);
            } else if (have_build) {
                if (refresh_document(c, resolved, RF_REBUILD)) rebuild_since = since;
            } else if (c->publishes_versions) {
                refresh_document(c, resolved, RF_REOPEN);
            } else {
                refresh_document(c, resolved, RF_USE);
            }
            int64_t expected = open_version(c, resolved);
            bool pa = project_aware(sn, c->cfg);
            int64_t cap = detailed ? BATCH_DIAGNOSTICS_WAIT_TIMEOUT_MS : pa ? k->wait_ms : SINGLE_DIAGNOSTICS_WAIT_TIMEOUT_MS;
            int64_t tmo = MIN(cap, k->timeout_sec * 1000);
            json_t *items;
            char *pfail = NULL;
            if (rebuild_since >= 0 && expected >= 0) items = wait_for_rebuilt_diagnostics(c, uri, expected, rebuild_since, tmo, &k->bg);
            else items = wait_for_diagnostics(c, uri, tmo, &k->bg, min_version, expected, pa, &pfail);
            if (deadline_hit(k, sn)) {
                json_free(items);
                free(pfail);
                stop = true;
                break;
            }
            if (pfail) {
                free(pfail);
                json_free(items);
                strvec_push(&failed, sn);
                failures++;
                continue;
            }
            if (!items) {
                strvec_push(&unreported, sn);
                continue;
            }
            json_t *d;
            json_arr_foreach(items, q, d) json_push(all, json_clone(d));
            json_free(items);
            succeeded++;
            strvec_push(&reported, sn);
        }
        if (stop) {
            free(uri);
            free(rel);
            json_free(all);
            strvec_free(&failed);
            strvec_free(&unreported);
            strvec_free(&reported);
            vec_free(&servers);
            free(resolved);
            goto done;
        }
        jptrs_t uniq = {0};
        dedupe_sort(all, &uniq);
        strvec_t notes = {0}, lines = {0};
        if (unreported.n) {
            char *names_s = strvec_join(&unreported, ", ");
            strvec_push_own(&notes, xasprintf("%s %s: no diagnostics report yet from %s; %s", sym("status.warning", "!"), rel, names_s,
                                              detailed ? "query the file alone for a longer wait" : "retry once it finishes analyzing"));
            free(names_s);
        }
        if (failed.n && (succeeded > 0 || unreported.n)) {
            char *names_s = strvec_join(&failed, ", ");
            strvec_push_own(&notes, xasprintf("%s %s: some servers failed (%s)", sym("status.warning", "!"), rel, names_s));
            free(names_s);
        }
        if (succeeded == 0 && !unreported.n) {
            char *names_s = strvec_join(&failed, ", ");
            strvec_push_own(&lines, xasprintf("%s %s: all language servers failed (%s)", sym("status.error", "x"), rel, names_s));
            free(names_s);
        } else if (uniq.n) {
            char *summary = diagnostics_summary(uniq.v, uniq.n);
            strvec_push_own(&lines, detailed ? xasprintf("%s %s: %s", sym("status.error", "x"), rel, summary) : xasprintf("%s:", summary));
            free(summary);
            strvec_t msgs = {0};
            for (size_t q = 0; q < uniq.n; q++) strvec_push_own(&msgs, format_diagnostic(uniq.v[q], rel));
            strvec_push_own(&lines, format_grouped_diagnostics(&msgs));
            strvec_free(&msgs);
        } else if (succeeded > 0 && unreported.n) {
            /* clean so far, but not verified until the pending servers report (omp 290d787845) */
            char *names_s = strvec_join(&reported, ", ");
            strvec_push_own(&lines, detailed ? xasprintf("%s %s: no issues from %s", sym("status.warning", "!"), rel, names_s)
                                             : xasprintf("no issues from %s", names_s));
            free(names_s);
        } else if (succeeded > 0) {
            strvec_push_own(&lines, detailed ? xasprintf("%s %s: no issues", sym("status.success", "ok"), rel) : xstrdup("OK"));
        }
        for (size_t q = 0; q < notes.n; q++) strvec_push(&lines, notes.v[q]);
        if (!detailed) {
            char *text = strvec_join(&lines, "\n");
            char *sv = strvec_join(&all_servers, ", ");
            reply(k->out, text, k->action, sv, succeeded > 0 || unreported.n, NULL);
            free(sv);
            free(text);
        } else {
            for (size_t q = 0; q < lines.n; q++) strvec_push(&results, lines.v[q]);
        }
        strvec_free(&notes);
        strvec_free(&lines);
        vec_free(&uniq);
        json_free(all);
        strvec_free(&failed);
        strvec_free(&unreported);
        strvec_free(&reported);
        free(uri);
        free(rel);
        free(resolved);
        vec_free(&servers);
        if (!detailed) goto done;
    }
    {
        char *text = strvec_join(&results, "\n");
        char *sv = strvec_join(&all_servers, ", ");
        reply(k->out, text, k->action, sv, !(attempts > 0 && failures == attempts), NULL);
        free(sv);
        free(text);
    }
done:
    strvec_free(&results);
    strvec_free(&all_servers);
    strvec_free(&targets);
}

/* enumerateRenamePairs */
static int collect_files(const char *dir, const char *rel, strvec_t *out, int depth) {
    dirlist_t dl = {0};
    if (fs_list_dir(dir, &dl) != 0 || depth > 64) return 0;
    for (size_t i = 0; i < dl.n; i++) {
        char *p = path_join(dir, dl.v[i].name);
        char *r = *rel ? path_join(rel, dl.v[i].name) : xstrdup(dl.v[i].name);
        struct stat st;
        if (lstat(p, &st) == 0) {
            if (S_ISDIR(st.st_mode)) collect_files(p, r, out, depth + 1);
            else if (S_ISREG(st.st_mode)) strvec_push(out, r);
        }
        free(p);
        free(r);
        if (out->n > MAX_RENAME_PAIRS) break;
    }
    dirlist_free(&dl);
    return 0;
}

static char *rename_stat_path(const char *p, const char *cwd) {
    char *r = rel_to_cwd(p, cwd);
    if (r[0] == '/') {
        const char *h = home_dir();
        size_t hl = h ? strlen(h) : 0;
        if (hl && !strncmp(p, h, hl) && (p[hl] == '/' || !p[hl])) {
            char *s = xasprintf("~%s", p + hl);
            free(r);
            r = s;
        }
    }
    for (char *q = r; *q; q++)
        if (*q == '\t') *q = ' ';
    return r;
}

static const char *errno_code(int e) {
    switch (e) {
    case EACCES: return "EACCES";
    case ELOOP: return "ELOOP";
    case EIO: return "EIO";
    case ENOTDIR: return "ENOTDIR";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case EPERM: return "EPERM";
    default: return "EIO";
    }
}

static void do_rename_file(call_t *k) {
    lsp_manager_t *m = k->lt->m;
    const char *cwd = k->lt->env->cwd;
    const char *file = json_get_str(k->args, "file"), *new_name = json_get_str(k->args, "new_name");
    if (!file || !*file || !new_name || !*new_name) {
        reply(k->out, "Error: rename_file requires both `file` (source path) and `new_name` (destination path)", k->action, NULL,
              false, k->args);
        return;
    }
    char *source = resolve_to_cwd(file, cwd);
    /* a bare name renames in place: it stays next to the source (omp 290d787845) */
    char *dest;
    if (strpbrk(new_name, "/\\")) {
        dest = resolve_to_cwd(new_name, cwd);
    } else {
        char *dir = path_dirname(source);
        dest = path_join(dir, new_name);
        free(dir);
    }
    /* relevant servers that do not implement workspace/willRenameFiles: they rewrite no imports */
    strvec_t non_renaming = {0};
    strvec_t pairs_old = {0}, pairs_new = {0}, notes = {0}, hard = {0}, responding = {0}, lines = {0};
    json_t *per_server = json_obj(); /* server -> edit */
    char *text = NULL;
    if (!strcmp(source, dest)) {
        reply(k->out, "Error: source and destination paths are identical", k->action, NULL, false, k->args);
        goto out;
    }
    struct stat sst, dst;
    if (stat(source, &sst) != 0) {
        char *rs = rename_stat_path(source, cwd);
        text = errno == ENOENT ? xasprintf("Error: source path does not exist: %s", rs)
                               : xasprintf("Error: cannot read source path %s: %s during stat", rs, errno_code(errno));
        free(rs);
        reply(k->out, text, k->action, NULL, false, k->args);
        goto out;
    }
    if (lstat(dest, &dst) == 0 || errno != ENOENT) {
        char *rs = rename_stat_path(dest, cwd);
        text = errno == 0 || lstat(dest, &dst) == 0 ? xasprintf("Error: destination already exists: %s", rs)
                                                    : xasprintf("Error: cannot read destination path %s: %s during lstat", rs, errno_code(errno));
        free(rs);
        reply(k->out, text, k->action, NULL, false, k->args);
        goto out;
    }
    bool is_dir = S_ISDIR(sst.st_mode);
    if (!is_dir) {
        strvec_push_own(&pairs_old, path_to_uri(source));
        strvec_push_own(&pairs_new, path_to_uri(dest));
    } else {
        strvec_t rels = {0};
        collect_files(source, "", &rels, 0);
        if (rels.n > MAX_RENAME_PAIRS) {
            strvec_free(&rels);
            text = xasprintf("Error: directory contains more than %d files; rename in smaller batches to keep LSP edits accurate",
                             MAX_RENAME_PAIRS);
            reply(k->out, text, k->action, NULL, false, k->args);
            goto out;
        }
        for (size_t i = 0; i < rels.n; i++) {
            char *a = path_join(source, rels.v[i]), *b = path_join(dest, rels.v[i]);
            strvec_push_own(&pairs_old, path_to_uri(a));
            strvec_push_own(&pairs_new, path_to_uri(b));
            free(a);
            free(b);
        }
        strvec_free(&rels);
    }
    if (!pairs_old.n) {
        reply(k->out, "Error: no files to rename", k->action, NULL, false, k->args);
        goto out;
    }
    json_t *files = json_arr();
    for (size_t i = 0; i < pairs_old.n; i++) {
        json_t *f = json_obj();
        json_set_str(f, "oldUri", pairs_old.v[i]);
        json_set_str(f, "newUri", pairs_new.v[i]);
        json_push(files, f);
    }
    /* relevant servers: those handling the source, the destination or any pair path */
    names_t relevant = {0};
    {
        const char *sk;
        json_t *sv;
        json_obj_foreach(m->servers, i, sk, sv) {
            if (has_custom_client(sk)) continue;
            bool rel = server_handles(sv, source) || server_handles(sv, dest);
            for (size_t q = 0; q < pairs_old.n && !rel; q++) {
                char *a = uri_to_path(pairs_old.v[q]), *b = uri_to_path(pairs_new.v[q]);
                rel = server_handles(sv, a) || server_handles(sv, b);
                free(a);
                free(b);
            }
            if (rel) vec_push(&relevant, sk);
        }
    }
    for (size_t s = 0; s < relevant.n; s++) {
        const char *sn = relevant.v[s];
        char *err = NULL;
        lsp_client_t *c = get_client(m, sn, &k->bg, &err);
        if (!c) {
            if (deadline_hit(k, NULL)) {
                free(err);
                json_free(files);
                vec_free(&relevant);
                goto out;
            }
            strvec_push_own(&notes, xasprintf("  %s: %s", sn, err ? err : "failed to start"));
            free(err);
            continue;
        }
        if (project_aware(sn, c->cfg)) wait_project_loaded(c, &k->bg);
        /* edits are computed against the server's documents: sync any changed on disk first */
        reconcile_open_files(c, NULL, false, NULL);
        json_t *p = json_obj();
        json_set(p, "files", json_clone(files));
        int code = 0;
        json_t *r = request(c, "workspace/willRenameFiles", p, &k->bg, &err, &code);
        if (!r) {
            if (deadline_hit(k, NULL)) {
                free(err);
                json_free(files);
                vec_free(&relevant);
                goto out;
            }
            char *low = xstrdup(err ? err : "");
            for (char *q = low; *q; q++) *q = (char)tolower((unsigned char)*q);
            bool mnf = strstr(low, "method not found") || strstr(low, "unhandled method") || strstr(low, "not supported") ||
                       strstr(low, "-32601");
            free(low);
            if (mnf) {
                strvec_push(&non_renaming, sn);
            } else {
                strvec_push_own(&notes, xasprintf("  %s: %s", sn, err));
                strvec_push(&hard, sn);
            }
            free(err);
            continue;
        }
        strvec_push(&responding, sn);
        if (json_is_obj(r) && (json_get(r, "changes") || json_get(r, "documentChanges"))) json_set(per_server, sn, r);
        else json_free(r);
    }
    vec_free(&relevant);
    char *src_label = rel_to_cwd(source, cwd), *dst_label = rel_to_cwd(dest, cwd);
    char *count_label = is_dir ? xasprintf("%zu file%s under %s", pairs_old.n, pairs_old.n != 1 ? "s" : "", src_label) : xstrdup(src_label);
    char *resp_s = strvec_join(&responding, ", ");
    /* nothing will update imports when no relevant server implements willRenameFiles */
    char *imports_note = NULL;
    if (!responding.n) {
        if (non_renaming.n) {
            char *nr = strvec_join(&non_renaming, ", ");
            imports_note = xasprintf("; imports NOT rewritten: %s %s not implement workspace/willRenameFiles", nr,
                                     non_renaming.n == 1 ? "does" : "do");
            free(nr);
        } else {
            imports_note = xstrdup("; imports NOT rewritten: no language server handles these files");
        }
    }
    char *dst_shown = xasprintf("%s%s", dst_label, imports_note ? imports_note : "");
    free(imports_note);
    if (json_get(k->args, "apply") && !json_get_bool(k->args, "apply", true)) {
        strvec_push_own(&lines, xasprintf("Rename preview: %s \xe2\x86\x92 %s", count_label, dst_shown));
        if (!json_len(per_server)) strvec_push(&lines, "  No LSP edits would be applied");
        else {
            const char *sk;
            json_t *ev;
            json_obj_foreach(per_server, i, sk, ev) {
                strvec_t es = {0};
                format_workspace_edit(ev, cwd, &es);
                if (es.n) {
                    strvec_push_own(&lines, xasprintf("  %s:", sk));
                    for (size_t q = 0; q < es.n; q++) strvec_push_own(&lines, xasprintf("    %s", es.v[q]));
                }
                strvec_free(&es);
            }
        }
        if (notes.n) {
            strvec_push(&lines, "  Server notes:");
            for (size_t q = 0; q < notes.n; q++) strvec_push(&lines, notes.v[q]);
        }
        text = strvec_join(&lines, "\n");
        reply(k->out, text, k->action, resp_s, true, k->args);
    } else if (hard.n) {
        char *hs = strvec_join(&hard, ", ");
        strvec_push_own(&lines, xasprintf("Error: aborted rename; workspace/willRenameFiles failed on %s, so semantic references "
                                          "would not be updated. No files were moved.",
                                          hs));
        free(hs);
        strvec_push(&lines, "  Server notes:");
        for (size_t q = 0; q < notes.n; q++) strvec_push(&lines, notes.v[q]);
        text = strvec_join(&lines, "\n");
        reply(k->out, text, k->action, resp_s, false, k->args);
    } else {
        /* coalesce per-uri edits across servers (first server wins on overlap; all are project-aware here) */
        json_t *by_uri = json_obj(); /* uri -> {server, edits[], discarded, conflicts[]} */
        const char *sk;
        json_t *ev;
        json_obj_foreach(per_server, i, sk, ev) {
            json_t *flat = json_obj();
            const char *u;
            json_t *v;
            json_obj_foreach(json_get_obj(ev, "changes"), q, u, v) {
                if (!json_len(v)) continue;
                json_t *prev = json_get(flat, u);
                if (!prev) json_set(flat, u, json_clone(v));
                else {
                    json_t *x;
                    json_arr_foreach(v, z, x) json_push(prev, json_clone(x));
                }
            }
            json_t *dc;
            json_arr_foreach(json_get_arr(ev, "documentChanges"), q, dc) {
                const char *du = json_get_str(json_get(dc, "textDocument"), "uri");
                if (!du || !json_get(dc, "edits")) continue;
                json_t *te = json_arr(), *x;
                json_arr_foreach(json_get_arr(dc, "edits"), z, x) if (json_get(x, "range") && json_get(x, "newText")) json_push(te, json_clone(x));
                if (!json_len(te)) {
                    json_free(te);
                    continue;
                }
                json_t *prev = json_get(flat, du);
                if (!prev) json_set(flat, du, te);
                else {
                    json_arr_foreach(te, z, x) json_push(prev, json_clone(x));
                    json_free(te);
                }
            }
            json_obj_foreach(flat, q, u, v) {
                json_t *bucket = json_get(by_uri, u);
                if (!bucket) {
                    bucket = json_obj();
                    json_set_str(bucket, "server", sk);
                    json_set(bucket, "edits", json_clone(v));
                    json_set_int(bucket, "discarded", 0);
                    json_set(bucket, "conflicts", json_arr());
                    json_set(by_uri, u, bucket);
                    continue;
                }
                json_t *acc = json_get(bucket, "edits");
                int64_t disc = 0;
                json_t *ne;
                json_arr_foreach(v, z, ne) {
                    bool ov = false;
                    json_t *ae;
                    json_t *nr = json_get(ne, "range");
                    json_arr_foreach(acc, w, ae) {
                        json_t *ar = json_get(ae, "range");
                        bool a_lt = pos_line(ar, "start") < pos_line(nr, "end") ||
                                    (pos_line(ar, "start") == pos_line(nr, "end") && pos_char(ar, "start") < pos_char(nr, "end"));
                        bool b_lt = pos_line(nr, "start") < pos_line(ar, "end") ||
                                    (pos_line(nr, "start") == pos_line(ar, "end") && pos_char(nr, "start") < pos_char(ar, "end"));
                        if (a_lt && b_lt) ov = true;
                    }
                    if (ov) disc++;
                    else json_push(acc, json_clone(ne));
                }
                if (disc) {
                    json_push_str(json_get(bucket, "conflicts"), sk);
                    json_set_int(bucket, "discarded", json_get_int(bucket, "discarded", 0) + disc);
                }
            }
            json_free(flat);
        }
        char *verr = NULL;
        const char *u;
        json_t *bucket;
        json_obj_foreach(by_uri, i, u, bucket) {
            tedits_t es = {0};
            if (!verr && sort_validate(json_get(bucket, "edits"), &es, &verr)) {
            }
            vec_free(&es);
        }
        if (verr) {
            tool_result_error(k->out, "%s", verr);
            free(verr);
            json_free(by_uri);
        } else {
            strvec_t summary = {0};
            json_t *backups = json_obj();
            int rc = 0;
            json_obj_foreach(by_uri, i, u, bucket) {
                char *p = uri_to_path(u), *rel = rel_to_cwd(p, cwd);
                size_t n;
                char *orig = read_text(p, &n);
                if (orig) json_set_str(backups, p, orig);
                free(orig);
                tedits_t es = {0};
                sort_validate(json_get(bucket, "edits"), &es, &verr);
                char *aerr = NULL;
                if (!rc && apply_text_edits_file(p, es.v, es.n, &aerr)) rc = -1;
                free(aerr);
                vec_free(&es);
                strvec_push_own(&summary, xasprintf("  %s: applied %zu edit(s) to %s", json_get_str(bucket, "server"),
                                                    json_len(json_get(bucket, "edits")), rel));
                if (json_get_int(bucket, "discarded", 0) > 0) {
                    strvec_t cs = {0};
                    json_t *x;
                    json_arr_foreach(json_get_arr(bucket, "conflicts"), q, x) strvec_push(&cs, json_sv(x));
                    char *others = strvec_join(&cs, ", ");
                    strvec_push_own(&summary, xasprintf("    note: discarded %lld overlapping edit(s) from %s (kept %s)",
                                                        (long long)json_get_int(bucket, "discarded", 0), others,
                                                        json_get_str(bucket, "server")));
                    free(others);
                    strvec_free(&cs);
                }
                free(p);
                free(rel);
            }
            char *ddir = path_dirname(dest);
            fs_mkdir_p(ddir, 0755);
            free(ddir);
            if (rc || rename(source, dest) != 0) {
                int e = errno;
                const char *bk;
                json_t *bv;
                json_obj_foreach(backups, i, bk, bv) fs_write_atomic(bk, json_sv(bv), json_len(bv), 0644);
                tool_result_error(k->out, "%s: %s, rename '%s' -> '%s'", errno_code(e), strerror(e), source, dest);
            } else {
                strvec_push_own(&summary, xasprintf("  Renamed %s \xe2\x86\x92 %s", src_label, dst_label));
                for (size_t s = 0; s < m->clients.n; s++) {
                    lsp_client_t *c = m->clients.v[s];
                    bool relevant2 = false;
                    for (size_t q = 0; q < responding.n; q++) relevant2 |= !strcmp(responding.v[q], c->name);
                    const char *ck;
                    json_t *cv;
                    json_obj_foreach(m->servers, q, ck, cv) if (!strcmp(ck, c->name) && (server_handles(cv, source) || server_handles(cv, dest)))
                        relevant2 = true;
                    if (!relevant2 || !c->alive) continue;
                    for (size_t q = 0; q < pairs_old.n; q++)
                        if (open_find(c, pairs_old.v[q])) {
                            send_simple(c, "textDocument/didClose", pairs_old.v[q]);
                            open_remove(c, pairs_old.v[q]);
                        }
                    json_t *p = json_obj();
                    json_set(p, "files", json_clone(files));
                    notify(c, "workspace/didRenameFiles", p);
                }
                if (notes.n) {
                    strvec_push(&summary, "  Server notes:");
                    for (size_t q = 0; q < notes.n; q++) strvec_push(&summary, notes.v[q]);
                }
                char *body = strvec_join(&summary, "\n");
                text = xasprintf("Renamed %s \xe2\x86\x92 %s\n%s", count_label, dst_shown, body);
                free(body);
                reply(k->out, text, k->action, resp_s, true, k->args);
            }
            free(verr);
            json_free(backups);
            strvec_free(&summary);
            json_free(by_uri);
        }
    }
    json_free(files);
    free(resp_s);
    free(count_label);
    free(src_label);
    free(dst_label);
    free(dst_shown);
out:
    free(text);
    free(source);
    free(dest);
    json_free(per_server);
    strvec_free(&pairs_old);
    strvec_free(&pairs_new);
    strvec_free(&notes);
    strvec_free(&hard);
    strvec_free(&responding);
    strvec_free(&lines);
    strvec_free(&non_renaming);
}

static void do_capabilities(call_t *k) {
    lsp_manager_t *m = k->lt->m;
    const char *file = json_get_str(k->args, "file");
    names_t list = {0};
    if (file && *file && strcmp(file, "*") != 0) {
        char *resolved = resolve_to_cwd(file, k->lt->env->cwd);
        activate_nested(m, resolved), servers_for_file(m->servers, resolved, &list, true);
        free(resolved);
        if (!list.n) {
            reply(k->out, "No language server found for this file", k->action, NULL, false, k->args);
            vec_free(&list);
            return;
        }
    } else {
        const char *sk;
        json_t *sv;
        json_obj_foreach(m->servers, i, sk, sv) if (!has_custom_client(sk)) vec_push(&list, sk);
    }
    if (!list.n) {
        reply(k->out, "No language servers configured", k->action, NULL, false, k->args);
        vec_free(&list);
        return;
    }
    strvec_t sections = {0}, responding = {0};
    for (size_t i = 0; i < list.n; i++) {
        char *err = NULL;
        lsp_client_t *c = get_client(m, list.v[i], &k->bg, &err);
        if (!c) {
            if (deadline_hit(k, NULL)) {
                free(err);
                goto done;
            }
            strvec_push_own(&sections, xasprintf("%s: failed to start (%s)", list.v[i], err ? err : "unknown error"));
            free(err);
            continue;
        }
        strvec_push(&responding, list.v[i]);
        char *caps = c->caps ? json_dump(c->caps, 2) : xstrdup("{}");
        char *ind = str_replace(caps, "\n", "\n  ");
        strvec_push_own(&sections, xasprintf("%s:", list.v[i]));
        strvec_push_own(&sections, xasprintf("  capabilities: %s", ind));
        free(ind);
        free(caps);
    }
    {
        char *text = strvec_join(&sections, "\n");
        char *rs = strvec_join(&responding, ", ");
        reply(k->out, text, k->action, rs, true, k->args);
        free(rs);
        free(text);
    }
done:
    strvec_free(&sections);
    strvec_free(&responding);
    vec_free(&list);
}

static void do_request(call_t *k) {
    lsp_manager_t *m = k->lt->m;
    const char *cwd = k->lt->env->cwd;
    const char *file = json_get_str(k->args, "file");
    char *method = json_get_str(k->args, "query") ? str_trim(json_get_str(k->args, "query")) : xstrdup("");
    char *resolved = NULL;
    const char *chosen = NULL;
    json_t *params = NULL;
    if (!*method) {
        reply(k->out, "Error: action=request requires `query` to specify the LSP method name (e.g., 'rust-analyzer/expandMacro')",
              k->action, NULL, false, k->args);
        goto out;
    }
    if (file && *file && strcmp(file, "*") != 0) {
        resolved = resolve_to_cwd(file, cwd);
        names_t l = {0};
        activate_nested(m, resolved), servers_for_file(m->servers, resolved, &l, true);
        chosen = l.n ? l.v[0] : NULL;
        vec_free(&l);
        if (!chosen) {
            reply(k->out, "No language server found for this file", k->action, NULL, false, k->args);
            goto out;
        }
    } else {
        const char *sk;
        json_t *sv;
        json_obj_foreach(m->servers, i, sk, sv) if (!chosen && !has_custom_client(sk)) chosen = sk;
        if (!chosen) {
            reply(k->out, "No language servers configured", k->action, NULL, false, k->args);
            goto out;
        }
    }
    const char *payload = json_get_str(k->args, "payload");
    if (payload) {
        char *perr = jsc_json_error(payload);
        if (perr) {
            char *t = xasprintf("Error: invalid JSON in payload: %s", perr);
            reply(k->out, t, k->action, chosen, false, k->args);
            free(t);
            free(perr);
            goto out;
        }
        params = json_parse_str(payload);
    } else if (resolved) {
        char *uri = path_to_uri(resolved);
        params = json_obj();
        json_t *td = json_obj();
        json_set_str(td, "uri", uri);
        json_set(params, "textDocument", td);
        free(uri);
        if (json_get(k->args, "line")) {
            int64_t col = 0;
            char *err = NULL;
            if (resolve_symbol_column(resolved, json_d2i(json_get_num(k->args, "line", 1)), json_get_str(k->args, "symbol"), &col, &err)) {
                tool_result_error(k->out, "%s", err);
                free(err);
                goto out;
            }
            json_t *pos = json_obj();
            json_set_int(pos, "line", json_d2i(json_get_num(k->args, "line", 1)) - 1);
            json_set_int(pos, "character", col);
            json_set(params, "position", pos);
        }
    } else {
        params = json_obj();
    }
    {
        char *err = NULL;
        lsp_client_t *c = get_client(m, chosen, &k->bg, &err);
        json_t *res = NULL;
        int code = 0;
        if (c) {
            if (resolved) reconcile_file(c, resolved);
            res = request(c, method, params ? json_clone(params) : NULL, &k->bg, &err, &code);
        }
        if (!res) {
            if (k->bg.cancel && *k->bg.cancel) {
                tool_result_error(k->out, "Operation aborted");
                free(err);
                goto out;
            }
            char *prev = json_dump(params ? params : json_null(), 0);
            if (strlen(prev) > 400) {
                prev[397] = 0;
                char *t = xasprintf("%s...", prev);
                free(prev);
                prev = t;
            }
            char *t = xasprintf("LSP error from %s on %s: %s\n  params: %s", chosen, method,
                                code == RQ_DEADLINE ? "The operation timed out." : (err ? err : "unknown error"), prev);
            reply(k->out, t, k->action, chosen, false, k->args);
            free(t);
            free(prev);
            free(err);
            goto out;
        }
        char *formatted = json_is_null(res) ? xstrdup("null") : json_is_str(res) ? xstrdup(json_sv(res)) : json_dump(res, 2);
        char *t = xasprintf("%s \xe2\x86\x90 %s:\n%s", chosen, method, formatted);
        reply(k->out, t, k->action, chosen, true, k->args);
        free(t);
        free(formatted);
        json_free(res);
    }
out:
    json_free(params);
    free(resolved);
    free(method);
}

static void do_workspace_symbols(call_t *k) {
    lsp_manager_t *m = k->lt->m;
    char *q = json_get_str(k->args, "query") ? str_trim(json_get_str(k->args, "query")) : xstrdup("");
    if (!*q) {
        reply(k->out, "Error: query parameter required for workspace symbol search", k->action, NULL, false, k->args);
        free(q);
        return;
    }
    names_t servers = {0};
    const char *sk;
    json_t *sv;
    json_obj_foreach(m->servers, i, sk, sv) if (!has_custom_client(sk)) vec_push(&servers, sk);
    if (!servers.n) {
        reply(k->out, "No language server found for this action", k->action, NULL, false, k->args);
        free(q);
        vec_free(&servers);
        return;
    }
    json_t *agg = json_arr();
    strvec_t responding = {0}, failures = {0};
    char *needle = xstrdup(q);
    for (char *p = needle; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (size_t s = 0; s < servers.n; s++) {
        char *err = NULL;
        lsp_client_t *c = get_client(m, servers.v[s], &k->bg, &err);
        json_t *res = NULL;
        if (c) {
            json_t *p = json_obj();
            json_set_str(p, "query", q);
            /* results point into every open document, so sync those edited outside omc first */
            reconcile_open_files(c, NULL, true, NULL);
            res = request(c, "workspace/symbol", p, &k->bg, &err, NULL);
        }
        if (!res) {
            if (deadline_hit(k, NULL)) {
                free(err);
                goto done;
            }
            char *e2 = str_replace(err ? err : "unknown error", "\t", "   ");
            strvec_push_own(&failures, xasprintf("  %s: %s", servers.v[s], e2));
            free(e2);
            free(err);
            continue;
        }
        strvec_push(&responding, servers.v[s]);
        json_t *x;
        json_arr_foreach(res, i, x) {
            const char *fields[3] = {json_get_str(x, "name"), json_get_str(x, "containerName"), NULL};
            char *fp = uri_to_path(json_get_str(json_get(x, "location"), "uri"));
            fields[2] = fp;
            bool hit = false;
            for (int f = 0; f < 3 && !hit; f++) {
                if (!fields[f]) continue;
                char *low = xstrdup(fields[f]);
                for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
                hit = strstr(low, needle) != NULL;
                free(low);
            }
            free(fp);
            if (hit) json_push(agg, json_clone(x));
        }
        json_free(res);
    }
    {
        char *fsec = NULL;
        if (failures.n) {
            char *j = strvec_join(&failures, "\n");
            fsec = xasprintf("\nServer failures:\n%s", j);
            free(j);
        } else fsec = xstrdup("");
        char *rs = strvec_join(&responding, ", ");
        if (!responding.n) {
            char *t = xasprintf("Workspace symbol search failed: all language servers failed%s", fsec);
            reply(k->out, t, k->action, "", false, k->args);
            free(t);
        } else {
            /* dedupeWorkspaceSymbols */
            jptrs_t uniq = {0};
            strvec_t keys = {0};
            json_t *x;
            json_arr_foreach(agg, i, x) {
                json_t *loc = json_get(x, "location");
                json_t *r = json_get(loc, "range");
                char *kind = js_string(json_get(x, "kind"));
                char *key = xasprintf("%s:%s:%s:%s:%lld:%lld", json_get_str(x, "name") ? json_get_str(x, "name") : "undefined",
                                      json_get_str(x, "containerName") ? json_get_str(x, "containerName") : "", kind,
                                      json_get_str(loc, "uri") ? json_get_str(loc, "uri") : "undefined", (long long)pos_line(r, "start"),
                                      (long long)pos_char(r, "start"));
                free(kind);
                bool seen = false;
                for (size_t q2 = 0; q2 < keys.n && !seen; q2++) seen = !strcmp(keys.v[q2], key);
                if (seen) free(key);
                else {
                    strvec_push_own(&keys, key);
                    vec_push(&uniq, x);
                }
            }
            strvec_free(&keys);
            if (!uniq.n) {
                char *t = xasprintf("No symbols matching \"%s\"%s", q, fsec);
                reply(k->out, t, k->action, rs, true, k->args);
                free(t);
            } else {
                sb_t b = SB_INIT;
                sb_appendf(&b, "Found %zu symbol(s) matching \"%s\":", uniq.n, q);
                for (size_t i = 0; i < uniq.n && i < WORKSPACE_SYMBOL_LIMIT; i++) {
                    char *l = format_symbol_information(uniq.v[i], k->lt->env->cwd);
                    sb_appendf(&b, "\n  %s", l);
                    free(l);
                }
                if (uniq.n > WORKSPACE_SYMBOL_LIMIT) sb_appendf(&b, "\n[\xe2\x80\xa6%zu symbols elided\xe2\x80\xa6]", uniq.n - WORKSPACE_SYMBOL_LIMIT);
                sb_append(&b, fsec);
                reply(k->out, sb_cstr(&b), k->action, rs, true, k->args);
                sb_free(&b);
            }
            vec_free(&uniq);
        }
        free(rs);
        free(fsec);
    }
done:
    free(needle);
    json_free(agg);
    strvec_free(&responding);
    strvec_free(&failures);
    vec_free(&servers);
    free(q);
}

/* reloadServer: owned result text. Gated on the binary: some servers (Roslyn) crash on an unknown method. reloadWorkspace
 * refreshes only the projects found at initialize, so a new crate needs a restart: the next request cold-starts a client */
static char *reload_server(lsp_manager_t *m, lsp_client_t *c, const char *server, budget_t *bg) {
    if (is_rust_analyzer(c) || !strcmp(server, "rust-analyzer")) {
        if (c->cargo_started) {
            pthread_join(c->cargo_th, NULL);
            c->cargo_started = false;
        }
        char *current = list_cargo_manifests(c->root);
        bool same = c->cargo_manifests && current && !strcmp(c->cargo_manifests, current);
        free(current);
        if (!same) {
            pthread_mutex_lock(&m->mu);
            for (size_t i = 0; i < m->clients.n; i++)
                if (m->clients.v[i] == c) {
                    m->clients.v[i] = m->clients.v[--m->clients.n];
                    break;
                }
            client_stop(c, true);
            pthread_mutex_unlock(&m->mu);
            return xasprintf("Restarted %s", server);
        }
        char *err = NULL;
        int code = 0;
        json_t *r = request(c, "rust-analyzer/reloadWorkspace", NULL, bg, &err, &code);
        free(err);
        if (r) {
            json_free(r);
            return xasprintf("Reloaded %s", server);
        }
    }
    json_t *pp = json_obj();
    json_t *st = json_get(c->cfg, "settings");
    json_set(pp, "settings", st ? json_clone(st) : json_obj());
    notify(c, "workspace/didChangeConfiguration", pp);
    refresh_open_files(c);
    return xasprintf("Reloaded %s", server);
}

static void do_reload_all(call_t *k) {
    lsp_manager_t *m = k->lt->m;
    pthread_mutex_lock(&m->mu);
    json_t *fresh_nested = NULL, *fresh_unresolved = NULL;
    json_t *fresh = load_config(m->cwd, &fresh_nested, &fresh_unresolved);
    strvec_t stopped = {0};
    for (size_t i = 0; i < m->clients.n;) {
        lsp_client_t *c = m->clients.v[i];
        json_t *nc = json_get(fresh, c->name);
        bool same = nc && json_equal(json_get(nc, "args"), json_get(c->cfg, "args")) &&
                    json_equal(json_get(nc, "initOptions"), json_get(c->cfg, "initOptions")) &&
                    json_equal(json_get(nc, "settings"), json_get(c->cfg, "settings")) &&
                    str_eq(json_get_str(nc, "resolvedCommand"), json_get_str(c->cfg, "resolvedCommand"));
        if (same) {
            i++;
            continue;
        }
        strvec_push(&stopped, json_get_str(c->cfg, "command"));
        m->clients.v[i] = m->clients.v[--m->clients.n];
        client_stop(c, true);
    }
    json_free(m->servers);
    m->servers = fresh;
    json_free(m->nested);
    m->nested = fresh_nested;
    json_free(m->unresolved);
    m->unresolved = fresh_unresolved;
    pthread_mutex_unlock(&m->mu);
    names_t servers = {0};
    const char *sk;
    json_t *sv;
    json_obj_foreach(m->servers, i, sk, sv) if (!has_custom_client(sk)) vec_push(&servers, sk);
    if (!servers.n) {
        reply(k->out, "No language server found for this action", k->action, NULL, false, k->args);
        strvec_free(&stopped);
        vec_free(&servers);
        return;
    }
    strvec_t outputs = {0};
    if (stopped.n) {
        char *j = strvec_join(&stopped, ", ");
        strvec_push_own(&outputs, xasprintf("Stopped %zu server(s) with superseded configuration: %s", stopped.n, j));
        free(j);
    }
    for (size_t s = 0; s < servers.n; s++) {
        json_del(m->init_failures, servers.v[s]);
        char *err = NULL;
        lsp_client_t *c = get_client(m, servers.v[s], &k->bg, &err);
        if (!c) {
            if (deadline_hit(k, NULL)) {
                free(err);
                goto done;
            }
            strvec_push_own(&outputs, xasprintf("Failed to reload %s: %s", servers.v[s], err ? err : "unknown error"));
            free(err);
            continue;
        }
        strvec_push_own(&outputs, reload_server(m, c, servers.v[s], &k->bg));
    }
    {
        char *text = strvec_join(&outputs, "\n");
        sb_t names = SB_INIT;
        for (size_t s = 0; s < servers.n; s++) sb_appendf(&names, "%s%s", s ? ", " : "", servers.v[s]);
        reply(k->out, text, k->action, sb_cstr(&names), true, k->args);
        sb_free(&names);
        free(text);
    }
done:
    strvec_free(&outputs);
    strvec_free(&stopped);
    vec_free(&servers);
}

static json_t *pos_params(const char *uri, int64_t line0, int64_t col) {
    json_t *p = json_obj(), *td = json_obj(), *pos = json_obj();
    json_set_str(td, "uri", uri);
    json_set(p, "textDocument", td);
    json_set_int(pos, "line", line0);
    json_set_int(pos, "character", col);
    json_set(p, "position", pos);
    return p;
}

/* normalizeLocationResult: owned array of {uri, range} */
static json_t *normalize_locations(const json_t *r) {
    json_t *out = json_arr();
    if (!r || json_is_null(r)) return out;
    json_t *one = NULL;
    if (!json_is_arr(r)) {
        one = json_arr();
        json_push(one, json_clone(r));
        r = one;
    }
    json_t *x;
    json_arr_foreach(r, i, x) {
        if (json_get(x, "uri")) json_push(out, json_clone(x));
        else if (json_get(x, "targetUri")) {
            json_t *l = json_obj();
            json_set_str(l, "uri", json_get_str(x, "targetUri"));
            json_t *rg = json_get(x, "targetSelectionRange");
            if (!rg) rg = json_get(x, "targetRange");
            json_set(l, "range", rg ? json_clone(rg) : json_obj());
            json_push(out, l);
        }
    }
    json_free(one);
    return out;
}

static bool only_queried_declaration(const json_t *locs, const char *uri, int64_t line, int64_t ch) {
    if (json_len(locs) != 1) return false;
    json_t *l = json_at(locs, 0);
    if (!str_eq(json_get_str(l, "uri"), uri)) return false;
    json_t *r = json_get(l, "range");
    int64_t sl = pos_line(r, "start"), sc = pos_char(r, "start"), el = pos_line(r, "end"), ec = pos_char(r, "end");
    bool after_start = sl < line || (sl == line && sc <= ch);
    bool before_end = line < el || (line == el && ch <= ec);
    return after_start && before_end;
}

static bool has_rust_workspace_ancestor(const char *file) {
    char *dir = path_dirname(file);
    for (;;) {
        char *a = path_join(dir, "Cargo.toml"), *b = path_join(dir, "rust-analyzer.toml");
        bool hit = fs_exists(a) || fs_exists(b);
        free(a);
        free(b);
        if (hit) {
            free(dir);
            return true;
        }
        char *up = path_dirname(dir);
        if (!strcmp(up, dir)) {
            free(up);
            free(dir);
            return false;
        }
        free(dir);
        dir = up;
    }
}

/* file-scoped actions: definition, type_definition, implementation, references, hover, code_actions, symbols, rename,
 * reload <file> */
/* the server capability and request method each single-server action needs (omp 290d787845 ACTION_PROVIDERS) */
static const struct {
    const char *action, *cap, *method;
} ACTION_PROVIDERS[] = {
    {"definition", "definitionProvider", "textDocument/definition"},
    {"type_definition", "typeDefinitionProvider", "textDocument/typeDefinition"},
    {"implementation", "implementationProvider", "textDocument/implementation"},
    {"references", "referencesProvider", "textDocument/references"},
    {"hover", "hoverProvider", "textDocument/hover"},
    {"code_actions", "codeActionProvider", "textDocument/codeAction"},
    {"symbols", "documentSymbolProvider", "textDocument/documentSymbol"},
    {"rename", "renameProvider", "textDocument/rename"},
};

static bool method_not_found(const char *err, int code) {
    if (code == -32601) return true;
    char *low = xstrdup(err ? err : "");
    for (char *q = low; *q; q++) *q = (char)tolower((unsigned char)*q);
    bool mnf = strstr(low, "method not found") || strstr(low, "unhandled method") || strstr(low, "not supported") ||
               strstr(low, "-32601") || strstr(low, "unknown request");
    free(low);
    return mnf;
}

/* "No configured language server for .ext supports <action>:" with each (unique) note indented */
static char *unsupported_text(const char *resolved, const char *action, const strvec_t *notes) {
    const char *ext = *path_ext(resolved) ? path_ext(resolved) : path_basename(resolved);
    sb_t b = SB_INIT;
    sb_appendf(&b, "No configured language server for %s supports %s:", ext, action);
    for (size_t i = 0; i < notes->n; i++) {
        bool dup = false;
        for (size_t j = 0; j < i && !dup; j++) dup = !strcmp(notes->v[i], notes->v[j]);
        if (!dup) sb_appendf(&b, "\n  %s", notes->v[i]);
    }
    return sb_detach(&b);
}

static void do_file_action(call_t *k, const char *resolved) {
    lsp_manager_t *m = k->lt->m;
    const char *cwd = k->lt->env->cwd;
    const char *action = k->action;
    names_t l = {0};
    activate_nested(m, resolved), servers_for_file(m->servers, resolved, &l, true);
    if (!l.n) {
        char *why = describe_missing(m, resolved);
        reply(k->out, why, action, NULL, false, NULL);
        free(why);
        vec_free(&l);
        return;
    }
    const char *server = l.v[0];
    if (!strcmp(action, "reload")) json_del(m->init_failures, server);
    const char *pcap = NULL, *pmethod = NULL;
    for (size_t i = 0; i < ARRAY_LEN(ACTION_PROVIDERS); i++)
        if (!strcmp(action, ACTION_PROVIDERS[i].action)) {
            pcap = ACTION_PROVIDERS[i].cap;
            pmethod = ACTION_PROVIDERS[i].method;
        }
    /* an applied code action also sends resolve/executeCommand, whose method-not-found is not the action's */
    const char *mnf_method = !strcmp(action, "code_actions") && json_get_bool(k->args, "apply", false) ? NULL : pmethod;
    char *err = NULL;
    char *output = NULL;
    bool useless = false;
    char *uri = NULL;
    /* selectClientForAction: the first server for the file that advertises the action, starting each candidate in
     * rank order; when none does, the first that started still gets the request */
    strvec_t skipped = {0};
    lsp_client_t *c = NULL, *fallback = NULL;
    const char *fallback_name = NULL;
    for (size_t i = 0; i < l.n && !c; i++) {
        char *e = NULL;
        lsp_client_t *ci = get_client(m, l.v[i], &k->bg, &e);
        if (!ci) {
            if (l.n == 1 || deadline_hit(k, l.v[i])) {
                if (!deadline_hit(k, l.v[i])) {
                    output = xasprintf("LSP error: %s", e ? e : "unknown error");
                    reply(k->out, output, action, l.v[i], false, k->args);
                }
                free(e);
                vec_free(&l);
                server = NULL;
                goto done;
            }
            strvec_push_own(&skipped, xasprintf("%s: failed to start (%s)", l.v[i], e ? e : "unknown error"));
            free(e);
            continue;
        }
        if (!pcap || !ci->caps || supports_method(ci, pcap, pmethod)) {
            c = ci;
            server = l.v[i];
            break;
        }
        if (!fallback) {
            fallback = ci;
            fallback_name = l.v[i];
        }
        strvec_push_own(&skipped, xasprintf("%s: does not implement %s", l.v[i], pmethod));
    }
    if (!c && fallback) {
        c = fallback;
        server = fallback_name;
    }
    if (!c) {
        output = unsupported_text(resolved, action, &skipped);
        reply(k->out, output, action, NULL, false, k->args);
        vec_free(&l);
        goto done;
    }
    vec_free(&l);
    const json_t *args = k->args;
    bool has_line = json_get(args, "line") != NULL;
    int64_t line = has_line ? json_d2i(json_get_num(args, "line", 1)) : 1;
    const char *symbol = json_get_str(args, "symbol");
    bool pa = project_aware(server, c->cfg);
    bool ra = is_rust_analyzer(c) || !strcmp(server, "rust-analyzer");
    bool needs_index = pa && (!strcmp(action, "definition") || !strcmp(action, "type_definition") ||
                              !strcmp(action, "implementation") || !strcmp(action, "references") ||
                              !strcmp(action, "rename") || !strcmp(action, "hover"));
    bool reconciled = reconcile_file(c, resolved);
    if (needs_index && ra && has_rust_workspace_ancestor(resolved)) wait_project_loaded(c, &k->bg);
    if (has_line && !symbol && pa &&
        (!strcmp(action, "references") || !strcmp(action, "rename") || !strcmp(action, "definition"))) {
        tool_result_error(k->out, "symbol is required for project-aware %s; pass symbol=<name>, optionally symbol#N for repeated occurrences",
                          action);
        goto done;
    }
    uri = path_to_uri(resolved);
    int64_t col = 0;
    if (resolve_symbol_column(resolved, line, symbol, &col, &err)) {
        output = xasprintf("LSP error: %s", err);
        reply(k->out, output, action, server, false, k->args);
        goto done;
    }
    int64_t line0 = line - 1;
    if (needs_index && !ra) wait_project_loaded(c, &k->bg);
    if (deadline_hit(k, server)) goto done;
    /* Answers that span files are computed from every open document, and one edited outside omc still holds its stale overlay:
     * sync them all right before the request. An action that writes edits fails on an open file it cannot read; a read-only query
     * closes that file and goes on. */
    if (!strcmp(action, "definition") || !strcmp(action, "type_definition") || !strcmp(action, "implementation") ||
        !strcmp(action, "references") || !strcmp(action, "rename") || !strcmp(action, "hover") || !strcmp(action, "code_actions")) {
        bool writes = !strcmp(action, "rename") || (!strcmp(action, "code_actions") && json_get_bool(args, "apply", false));
        char *serr = NULL;
        reconcile_open_files(c, NULL, !writes, writes ? &serr : NULL);
        if (serr) {
            output = xasprintf("LSP error: %s", serr);
            free(serr);
            reply(k->out, output, action, server, false, k->args);
            goto done;
        }
    }
    int code = 0;
#define REQ(method, params)                                                                \
    ({                                                                                     \
        free(err);                                                                         \
        err = NULL;                                                                        \
        json_t *r_ = request(c, method, params, &k->bg, &err, &code);                      \
        if (!r_) {                                                                         \
            if (!deadline_hit(k, server)) {                                                \
                char *o_;                                                                  \
                if (mnf_method && !strcmp(method, mnf_method) && method_not_found(err, code)) { \
                    strvec_push_own(&skipped, xasprintf("%s: does not implement %s", server, mnf_method)); \
                    o_ = unsupported_text(resolved, action, &skipped);                     \
                } else {                                                                   \
                    o_ = xasprintf("LSP error: %s", err ? err : "unknown error");          \
                }                                                                          \
                reply(k->out, o_, action, server, false, k->args);                         \
                free(o_);                                                                  \
            }                                                                              \
            goto done;                                                                     \
        }                                                                                  \
        r_;                                                                                \
    })
    if (!strcmp(action, "definition") || !strcmp(action, "type_definition") || !strcmp(action, "implementation")) {
        const char *method = !strcmp(action, "definition")        ? "textDocument/definition"
                             : !strcmp(action, "type_definition") ? "textDocument/typeDefinition"
                                                                  : "textDocument/implementation";
        const char *what = !strcmp(action, "definition") ? "definition" : !strcmp(action, "type_definition") ? "type definition" : "implementation";
        json_t *r = REQ(method, pos_params(uri, line0, col));
        json_t *locs = normalize_locations(r);
        json_free(r);
        if (!json_len(locs)) {
            output = xasprintf("No %s found", what);
            useless = true;
        } else {
            sb_t b = SB_INIT;
            sb_appendf(&b, "Found %zu %s(s):", json_len(locs), what);
            json_t *x;
            json_arr_foreach(locs, i, x) {
                char *s = format_location_ctx(x, cwd);
                sb_appendf(&b, "\n%s", s);
                free(s);
            }
            output = sb_detach(&b);
        }
        json_free(locs);
    } else if (!strcmp(action, "references")) {
        json_t *r = NULL;
        for (int attempt = 0; attempt <= REFERENCES_RETRY_COUNT; attempt++) {
            json_free(r);
            json_t *p = pos_params(uri, line0, col), *ctx = json_obj();
            json_set_bool(ctx, "includeDeclaration", true);
            json_set(p, "context", ctx);
            r = REQ("textDocument/references", p);
            if (!pa || attempt == REFERENCES_RETRY_COUNT) break;
            if (json_len(r) > 0 && !only_queried_declaration(r, uri, line0, col)) break;
            wait_project_loaded(c, &k->bg);
            if (deadline_hit(k, server)) {
                json_free(r);
                goto done;
            }
            sleep_ms(REFERENCES_RETRY_DELAY_MS);
        }
        if (!json_is_arr(r) || !json_len(r)) {
            output = xstrdup("No references found");
            useless = true;
        } else {
            sb_t b = SB_INIT;
            size_t n = json_len(r);
            sb_appendf(&b, "Found %zu reference(s):", n);
            for (size_t i = 0; i < n && i < REFERENCE_CONTEXT_LIMIT; i++) {
                char *s = format_location_ctx(json_at(r, i), cwd);
                sb_appendf(&b, "\n%s", s);
                free(s);
            }
            if (n > REFERENCE_CONTEXT_LIMIT) {
                sb_appendf(&b, "\n  ... %zu additional reference(s) shown without context", n - REFERENCE_CONTEXT_LIMIT);
                for (size_t i = REFERENCE_CONTEXT_LIMIT; i < n; i++) {
                    char *s = format_location(json_at(r, i), cwd);
                    sb_appendf(&b, "\n  %s", s);
                    free(s);
                }
            }
            output = sb_detach(&b);
        }
        json_free(r);
    } else if (!strcmp(action, "hover")) {
        json_t *r = REQ("textDocument/hover", pos_params(uri, line0, col));
        json_t *contents = json_get(r, "contents");
        if (!contents || !json_truthy(contents)) output = xstrdup("No hover information");
        else {
            sb_t b = SB_INIT;
            hover_text(&b, contents, 0);
            output = sb_detach(&b);
        }
        json_free(r);
    } else if (!strcmp(action, "code_actions")) {
        char *key = uri_key(uri);
        int64_t pv = -1;
        json_t *pub = published_get(c, key, &pv);
        json_t *diags = pub ? json_take(pub, "items") : json_arr();
        json_free(pub);
        free(key);
        if (reconciled) {
            char *pfail = NULL;
            int64_t tmo = MIN(pa ? k->wait_ms : (int64_t)SINGLE_DIAGNOSTICS_WAIT_TIMEOUT_MS, k->timeout_sec * 1000);
            json_t *rep = wait_for_diagnostics(c, uri, tmo, &k->bg, -1, open_version(c, resolved), false, &pfail);
            json_free(diags);
            if (pfail) {
                key = uri_key(uri);
                pub = published_get(c, key, &pv);
                diags = pub ? json_take(pub, "items") : json_arr();
                json_free(pub);
                free(key);
                json_free(rep);
            } else diags = rep ? rep : json_arr();
            free(pfail);
            if (deadline_hit(k, server)) {
                json_free(diags);
                goto done;
            }
        }
        const char *query = json_get_str(args, "query");
        bool apply = json_get_bool(args, "apply", false) && json_is_bool(json_get(args, "apply"));
        json_t *p = json_obj(), *td = json_obj(), *range = json_obj(), *st = json_obj(), *en = json_obj(), *ctx = json_obj();
        json_set_str(td, "uri", uri);
        json_set(p, "textDocument", td);
        json_set_int(st, "line", line0);
        json_set_int(st, "character", col);
        json_set_int(en, "line", line0);
        json_set_int(en, "character", col);
        json_set(range, "start", st);
        json_set(range, "end", en);
        json_set(p, "range", range);
        json_set(ctx, "diagnostics", diags);
        bool kind_query = !apply && query && *query;
        if (kind_query) {
            json_t *only = json_arr();
            json_push_str(only, query);
            json_set(ctx, "only", only);
        }
        json_set_int(ctx, "triggerKind", 1);
        json_set(p, "context", ctx);
        /* Servers like typescript-language-server return source actions (organize imports, fix all) only when
         * `context.only` asks for them, as VS Code does. The unfiltered set backs both listing and apply (by title or
         * index), so it also requests `source` kinds; a kind query narrows listing to that kind. */
        json_t *r;
        if (kind_query) {
            r = REQ("textDocument/codeAction", p);
        } else {
            json_t *ps = json_clone(p), *only = json_arr();
            json_push_str(only, "source");
            json_set(json_get(ps, "context"), "only", only);
            free(err);
            err = NULL;
            json_t *r1 = request(c, "textDocument/codeAction", p, &k->bg, &err, &code);
            if (!r1) { /* REQ's failure path */
                json_free(ps);
                if (!deadline_hit(k, server)) {
                    char *o_ = xasprintf("LSP error: %s", err ? err : "unknown error");
                    reply(k->out, o_, action, server, false, k->args);
                    free(o_);
                }
                goto done;
            }
            if (!json_is_arr(r1)) {
                json_free(r1);
                r1 = json_arr();
            }
            r = request(c, "textDocument/codeAction", ps, &k->bg, &err, &code);
            if (!r) {
                json_free(r1);
                if (!deadline_hit(k, server)) {
                    char *o_ = xasprintf("LSP error: %s", err ? err : "unknown error");
                    reply(k->out, o_, action, server, false, k->args);
                    free(o_);
                }
                goto done;
            }
            /* mergeCodeActions: append the source actions that do not already appear identically */
            strvec_t seen = {0};
            json_t *x;
            json_arr_foreach(r1, i, x) strvec_push_own(&seen, json_dump(x, 0));
            json_arr_foreach(json_is_arr(r) ? r : NULL, i, x) {
                char *d = json_dump(x, 0);
                if (!strvec_contains(&seen, d)) json_push(r1, json_clone(x));
                free(d);
            }
            strvec_free(&seen);
            json_free(r);
            r = r1;
        }
        size_t n = json_is_arr(r) ? json_len(r) : 0;
        if (!n) output = xstrdup("No code actions available");
        else if (apply && query && *query) {
            char *nq = str_trim(query);
            const json_t *sel = NULL;
            if (!*nq) output = xstrdup("Error: query parameter required when apply=true for code_actions");
            else {
                if (strspn(nq, "0123456789") == strlen(nq)) {
                    size_t idx = (size_t)strtoull(nq, NULL, 10);
                    sel = idx < n ? json_at(r, idx) : NULL;
                } else {
                    char *lq = xstrdup(nq);
                    for (char *q = lq; *q; q++) *q = (char)tolower((unsigned char)*q);
                    json_t *x;
                    json_arr_foreach(r, i, x) {
                        if (sel) break;
                        char *lt = xstrdup(json_get_str(x, "title") ? json_get_str(x, "title") : "");
                        for (char *q = lt; *q; q++) *q = (char)tolower((unsigned char)*q);
                        if (strstr(lt, lq)) sel = x;
                        free(lt);
                    }
                    free(lq);
                }
                if (!sel) {
                    sb_t b = SB_INIT;
                    sb_appendf(&b, "No code action matches \"%s\". Available actions:", nq);
                    json_t *x;
                    json_arr_foreach(r, i, x) {
                        char *s = format_code_action(x, i);
                        sb_appendf(&b, "\n  %s", s);
                        free(s);
                    }
                    output = sb_detach(&b);
                } else {
                    /* applyCodeAction */
                    strvec_t edits = {0}, cmds = {0};
                    json_t *resolved_action = json_clone(sel);
                    const char *title = json_get_str(sel, "title") ? json_get_str(sel, "title") : "undefined";
                    char *title_c = xstrdup(title);
                    bool is_command = json_is_str(json_get(sel, "command"));
                    char *aerr = NULL;
                    if (is_command) {
                        json_t *ep = json_obj();
                        json_set_str(ep, "command", json_get_str(sel, "command"));
                        json_t *ar = json_get(sel, "arguments");
                        json_set(ep, "arguments", ar ? json_clone(ar) : json_arr());
                        json_t *er = REQ("workspace/executeCommand", ep);
                        json_free(er);
                        strvec_push(&cmds, json_get_str(sel, "command"));
                    } else {
                        if (!json_get(resolved_action, "edit")) {
                            char *e2 = NULL;
                            json_t *rr = request(c, "codeAction/resolve", json_clone(resolved_action), &k->bg, &e2, NULL);
                            free(e2);
                            if (rr && json_is_obj(rr)) {
                                json_free(resolved_action);
                                resolved_action = rr;
                            } else json_free(rr);
                        }
                        if (json_get(resolved_action, "edit")) {
                            if (apply_workspace_edit_lsp(m, json_get(resolved_action, "edit"), &edits, &aerr)) {
                                tool_result_error(k->out, "%s", aerr ? aerr : "failed to apply workspace edit");
                                free(aerr);
                                json_free(resolved_action);
                                free(title_c);
                                strvec_free(&edits);
                                strvec_free(&cmds);
                                free(nq);
                                json_free(r);
                                goto done;
                            }
                        }
                        json_t *cmd = json_get(resolved_action, "command");
                        if (json_is_obj(cmd)) {
                            json_t *ep = json_obj();
                            json_set_str(ep, "command", json_get_str(cmd, "command"));
                            json_t *ar = json_get(cmd, "arguments");
                            json_set(ep, "arguments", ar ? json_clone(ar) : json_arr());
                            json_t *er = REQ("workspace/executeCommand", ep);
                            json_free(er);
                            strvec_push(&cmds, json_get_str(cmd, "command") ? json_get_str(cmd, "command") : "undefined");
                        }
                        if (json_get_str(resolved_action, "title")) {
                            free(title_c);
                            title_c = xstrdup(json_get_str(resolved_action, "title"));
                        }
                    }
                    if (!edits.n && !cmds.n) output = xasprintf("Action \"%s\" has no workspace edit or command to apply", title);
                    else {
                        sb_t b = SB_INIT;
                        sb_appendf(&b, "Applied \"%s\":", title_c);
                        bool first = true;
                        if (edits.n) {
                            sb_appendf(&b, "%s  Workspace edit:", first ? "\n" : "\n");
                            first = false;
                            for (size_t i = 0; i < edits.n; i++) sb_appendf(&b, "\n    %s", edits.v[i]);
                        }
                        if (cmds.n) {
                            sb_append(&b, "\n  Executed command(s):");
                            for (size_t i = 0; i < cmds.n; i++) sb_appendf(&b, "\n    %s", cmds.v[i]);
                        }
                        output = sb_detach(&b);
                    }
                    free(title_c);
                    json_free(resolved_action);
                    strvec_free(&edits);
                    strvec_free(&cmds);
                }
            }
            free(nq);
        } else {
            sb_t b = SB_INIT;
            sb_appendf(&b, "%zu code action(s):", n);
            json_t *x;
            json_arr_foreach(r, i, x) {
                char *s = format_code_action(x, i);
                sb_appendf(&b, "\n  %s", s);
                free(s);
            }
            output = sb_detach(&b);
        }
        json_free(r);
    } else if (!strcmp(action, "symbols")) {
        json_t *p = json_obj(), *td = json_obj();
        json_set_str(td, "uri", uri);
        json_set(p, "textDocument", td);
        json_t *r = REQ("textDocument/documentSymbol", p);
        if (!json_is_arr(r) || !json_len(r)) {
            output = xstrdup("No symbols found");
            useless = true;
        } else {
            char *rel = rel_to_cwd(resolved, cwd);
            sb_t b = SB_INIT;
            if (json_get(json_at(r, 0), "selectionRange")) {
                json_t *x;
                json_arr_foreach(r, i, x) format_document_symbol(&b, x, 0);
            } else {
                json_t *x;
                json_arr_foreach(r, i, x) {
                    if (b.len) sb_putc(&b, '\n');
                    sb_appendf(&b, "%s %s @ line %lld", symbol_icon(json_get_int(x, "kind", 0)),
                               json_get_str(x, "name") ? json_get_str(x, "name") : "",
                               (long long)pos_line(json_get(json_get(x, "location"), "range"), "start") + 1);
                }
            }
            output = xasprintf("Symbols in %s:\n%s", rel, sb_cstr(&b));
            sb_free(&b);
            free(rel);
        }
        json_free(r);
    } else if (!strcmp(action, "rename")) {
        const char *new_name = json_get_str(args, "new_name");
        if (!new_name || !*new_name) {
            reply(k->out, "Error: new_name parameter required for rename", action, server, false, NULL);
            goto done;
        }
        json_t *p = pos_params(uri, line0, col);
        json_set_str(p, "newName", new_name);
        json_t *r = REQ("textDocument/rename", p);
        if (json_is_null(r)) output = xstrdup("Rename returned no edits");
        else if (json_get(args, "apply") && !json_get_bool(args, "apply", true)) {
            strvec_t pv = {0};
            format_workspace_edit(r, cwd, &pv);
            sb_t b = SB_INIT;
            sb_append(&b, "Rename preview:");
            for (size_t i = 0; i < pv.n; i++) sb_appendf(&b, "\n  %s", pv.v[i]);
            output = sb_detach(&b);
            strvec_free(&pv);
        } else {
            strvec_t applied = {0};
            char *aerr = NULL;
            if (apply_workspace_edit_lsp(m, r, &applied, &aerr)) {
                tool_result_error(k->out, "%s", aerr ? aerr : "failed to apply workspace edit");
                free(aerr);
                strvec_free(&applied);
                json_free(r);
                goto done;
            }
            sb_t b = SB_INIT;
            sb_append(&b, "Applied rename:");
            for (size_t i = 0; i < applied.n; i++) sb_appendf(&b, "\n  %s", applied.v[i]);
            output = sb_detach(&b);
            strvec_free(&applied);
        }
        json_free(r);
    } else if (!strcmp(action, "reload")) {
        output = reload_server(k->lt->m, c, server, &k->bg);
    } else {
        output = xasprintf("Unknown action: %s", action);
    }
#undef REQ
    reply(k->out, output, action, server, true, k->args);
    k->out->useless = useless;
done:
    free(output);
    free(err);
    free(uri);
    strvec_free(&skipped);
}

/* tool-timeouts.ts resolveLspTimeouts */
static void lsp_timeouts(const json_t *args, int64_t *call, int64_t *wait) {
    double rt = (double)settings_int("lsp.requestTimeout", 10);
    double maxt = (double)settings_int("tools.maxTimeout", 0);
#define CLAMP(v) ({ double c_ = (v); if (maxt > 0 && c_ > maxt) c_ = maxt; c_ < 5 ? 5 : c_ > 300 ? 300 : c_; })
    json_t *t = json_get(args, "timeout");
    if (json_is_num(t)) {
        *call = *wait = (int64_t)CLAMP(json_get_num(args, "timeout", 20));
    } else {
        *call = (int64_t)CLAMP(rt > 20 ? rt : 20);
        *wait = (int64_t)CLAMP(rt);
    }
#undef CLAMP
}

static int lsp_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    lsp_tool_t *lt = self->ud;
    call_t k = {.lt = lt, .args = args, .out = out, .action = json_get_str(args, "action")};
    if (!k.action) {
        tool_result_error(out, "action is required");
        return 0;
    }
    int64_t wait_s = 10;
    lsp_timeouts(args, &k.timeout_sec, &wait_s);
    k.wait_ms = wait_s * 1000;
    k.bg.deadline = mono_ms() + k.timeout_sec * 1000;
    k.bg.cancel = ctx ? ctx->cancel : NULL;
    const char *a = k.action;
    const char *file = json_get_str(args, "file");
    if (!strcmp(a, "status")) do_status(&k);
    else if (!strcmp(a, "diagnostics")) do_diagnostics(&k);
    else if (!strcmp(a, "rename_file")) do_rename_file(&k);
    else if (!strcmp(a, "capabilities")) do_capabilities(&k);
    else if (!strcmp(a, "request")) do_request(&k);
    else {
        bool workspace = str_eq(file, "*");
        if ((!file || !*file) && strcmp(a, "reload") != 0) {
            tool_result_text(out, "Error: file parameter required. Use `*` for workspace scope where supported.");
            details(out, a, NULL, false, NULL);
        } else if (!strcmp(a, "symbols") && workspace) {
            do_workspace_symbols(&k);
        } else if (!strcmp(a, "reload") && (workspace || !file || !*file)) {
            do_reload_all(&k);
        } else if (workspace) {
            reply(out, "No language server found for this action", a, NULL, false, NULL);
        } else {
            char *resolved = resolve_to_cwd(file, lt->env->cwd);
            do_file_action(&k, resolved);
            free(resolved);
        }
    }
    return 0;
}

static void lsp_tool_destroy(agent_tool_t *t) { free(t->ud); }

agent_tool_t *tool_lsp_new(tool_env_t *env, lsp_manager_t *m) {
    int64_t call = 20, wait = 10;
    lsp_timeouts(NULL, &call, &wait);
    json_t *ctx = json_obj();
    json_set_int(ctx, "defaultTimeoutSec", call);
    json_set_int(ctx, "defaultWaitSec", wait);
    int64_t maxt = settings_int("tools.maxTimeout", 0);
    json_set_int(ctx, "maxTimeoutSec", maxt > 0 && maxt < 300 ? MAX(maxt, (int64_t)5) : 300);
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
