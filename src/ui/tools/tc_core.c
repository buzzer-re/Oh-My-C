/* tc_core.c - rendering primitives shared by the tool cards (port of omp packages/tui/src/render:
 * output-block.ts, status-line.ts, tree-list.ts, output-pane.ts, render-utils.ts, tools/json-tree.ts). */
#include "ui/tools/tc.h"

#include <ctype.h>

#include "core/fs.h"
#include "tui/ansi.h"
#include "tui/markdown.h"
#include "tui/symbols.h"

/* ---------- theme ---------- */

char *tc_fg(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s ? s : ""); }
void tc_fg_sb(sb_t *b, tui_tc_t slot, const char *s) { tui_fg(b, NULL, slot, s ? s : ""); }
char *tc_bold(const char *s) { return xasprintf("\x1b[1m%s\x1b[22m", s ? s : ""); }
const char *tc_sym(const char *key) { return tui_sym(key); }
char *tc_styled_sym(const char *key, tui_tc_t slot) { return tc_fg(slot, tui_sym(key)); }

const char *tc_spinner_frame(int frame) {
    const tui_theme_t *th = tui_theme();
    if (th && th->nspinner > 0) return th->spinner[(frame < 0 ? 0 : frame) % th->nspinner];
    const char *const *frames;
    size_t n = tui_sym_spinner("status", &frames);
    return n ? frames[(size_t)(frame < 0 ? 0 : frame) % n] : "";
}

char *tc_status_icon(tc_status_t st, int spinner) {
    switch (st) {
    case TC_ST_SUCCESS: return tc_styled_sym("status.success", TUI_TC_SUCCESS);
    case TC_ST_DONE: return tc_styled_sym("status.done", TUI_TC_SUCCESS);
    case TC_ST_ERROR: return tc_styled_sym("status.error", TUI_TC_ERROR);
    case TC_ST_WARNING: return tc_styled_sym("status.warning", TUI_TC_WARNING);
    case TC_ST_INFO: return tc_styled_sym("status.info", TUI_TC_ACCENT);
    case TC_ST_PENDING: return tc_styled_sym("status.pending", TUI_TC_MUTED);
    case TC_ST_RUNNING:
        if (spinner >= 0) return xstrdup(tc_spinner_frame(spinner));
        return tc_styled_sym("status.running", TUI_TC_ACCENT);
    case TC_ST_ABORTED: return tc_styled_sym("status.aborted", TUI_TC_ERROR);
    default: return xstrdup("");
    }
}

static char *flatten_nl(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\r') {
            if (p[1] == '\n') p++;
            sb_putc(&b, ' ');
        } else if (*p == '\n') sb_putc(&b, ' ');
        else sb_putc(&b, *p);
    }
    return sb_detach(&b);
}

char *tc_status_line(const tc_status_line_t *o) {
    sb_t b = SB_INIT;
    char *icon = o->icon_override ? xstrdup(o->icon_override) : tc_status_icon(o->icon, o->spinner);
    char *title = flatten_nl(o->title);
    if (*icon) {
        sb_append(&b, icon);
        sb_putc(&b, ' ');
    }
    tc_fg_sb(&b, o->title_slot < 0 ? TUI_TC_ACCENT : (tui_tc_t)o->title_slot, title);
    if (o->description && *o->description) {
        char *d = flatten_nl(o->description);
        sb_append(&b, ": ");
        tc_fg_sb(&b, TUI_TC_MUTED, d);
        free(d);
    }
    if (o->badge) {
        char *bl = flatten_nl(o->badge);
        char *t = xasprintf("%s%s%s", tc_sym("format.bracketLeft"), bl, tc_sym("format.bracketRight"));
        sb_putc(&b, ' ');
        tc_fg_sb(&b, o->badge_slot, t);
        free(t);
        free(bl);
    }
    sb_t m = SB_INIT;
    for (size_t i = 0; i < o->nmeta; i++) {
        char *f = flatten_nl(o->meta[i]);
        char *plain = tui_strip_ansi(f);
        bool blank = strspn(plain, " \t") == strlen(plain);
        free(plain);
        if (!blank) {
            if (m.len) sb_append(&m, tc_sym("sep.dot"));
            sb_append(&m, f);
        }
        free(f);
    }
    if (m.len) {
        sb_putc(&b, ' ');
        tc_fg_sb(&b, TUI_TC_DIM, sb_cstr(&m));
    }
    sb_free(&m);
    free(title);
    free(icon);
    return sb_detach(&b);
}

/* ---------- output block ---------- */

static int norm_pad(int v) { return v < 0 ? 1 : v; }

int tc_block_content_width(int width, int pad_left, int pad_right) {
    int l = norm_pad(pad_left), r = pad_right < 0 ? l : pad_right;
    return MAX(1, width - 2 - l - r);
}

static tui_tc_t state_bg(tc_state_t st) {
    return st == TC_STATE_SUCCESS ? TUI_TC_TOOL_SUCCESS_BG : st == TC_STATE_ERROR ? TUI_TC_TOOL_ERROR_BG : TUI_TC_TOOL_PENDING_BG;
}

/* bg wrapper that survives nested resets (\e[0m, \e[m, \e[49m) */
static char *apply_bg(const char *text, const char *bg) {
    sb_t b = SB_INIT;
    sb_append(&b, bg);
    for (const char *p = text; *p;) {
        if (str_starts(p, "\x1b[0m") || str_starts(p, "\x1b[m") || str_starts(p, "\x1b[49m")) {
            size_t n = p[2] == 'm' ? 3 : p[2] == '0' ? 4 : 5;
            sb_appendn(&b, p, n);
            sb_append(&b, bg);
            p += n;
            continue;
        }
        sb_putc(&b, *p++);
    }
    sb_append(&b, "\x1b[49m");
    return sb_detach(&b);
}

static char *trim_end_ws(const char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    return xstrndup(s, n);
}

char *tc_trim_end(const char *s) { return trim_end_ws(s ? s : ""); }

static char *rep(const char *s, int n) {
    sb_t b = SB_INIT;
    for (int i = 0; i < n; i++) sb_append(&b, s);
    return sb_detach(&b);
}

static char *pad_to(const char *text, int width) {
    int w = tui_visible_width(text);
    if (w == width) return xstrdup(text);
    char *fitted = w < width ? NULL : tui_truncate_to_width(text, width, NULL);
    sb_t b = SB_INIT;
    sb_append(&b, fitted ? fitted : text);
    int drift = width - tui_visible_width(sb_cstr(&b));
    if (drift > 0) tui_sb_spaces(&b, drift);
    free(fitted);
    return sb_detach(&b);
}

typedef struct {
    int kind; /* 0 bar, 1 bottom, 2 content */
    const char *lc, *rc;
    const char *label, *meta;
    char *inner;
} brow_t;

void tc_output_block(const tc_block_t *o, strvec_t *out) {
    const char *h = tc_sym("boxRound.horizontal"), *v = tc_sym("boxRound.vertical");
    int line_width = MAX(0, o->width);
    tui_tc_t border_slot = o->border_slot >= 0 ? (tui_tc_t)o->border_slot
                           : o->state == TC_STATE_ERROR                     ? TUI_TC_ERROR
                           : o->state == TC_STATE_WARNING                   ? TUI_TC_WARNING
                           : o->state == TC_STATE_RUNNING || o->state == TC_STATE_PENDING ? TUI_TC_ACCENT
                                                                                           : TUI_TC_DIM;
    const char *bg = NULL;
    if (o->state != TC_STATE_NONE && !o->no_bg) bg = tui_bg_seq(NULL, state_bg(o->state));
    int pl = norm_pad(o->pad_left), pr = o->pad_right < 0 ? pl : o->pad_right;
    int vw = tui_visible_width(v);
    int content_width = MAX(0, line_width - vw - pl - pr - vw);
    char *cap = rep(h, 3);

    VEC(brow_t) rows = {0};
    vec_push(&rows, ((brow_t){0, tc_sym("boxRound.topLeft"), tc_sym("boxRound.topRight"), o->header, o->header_meta, NULL}));
    tc_section_t empty = {0};
    size_t nsec = o->nsections ? o->nsections : 1;
    for (size_t si = 0; si < nsec; si++) {
        const tc_section_t *s = o->nsections ? &o->sections[si] : &empty;
        if (s->label && *s->label)
            vec_push(&rows, ((brow_t){0, tc_sym("boxSharp.teeRight"), tc_sym("boxSharp.teeLeft"), s->label, NULL, NULL}));
        else if (s->separator && si > 0)
            vec_push(&rows, ((brow_t){0, tc_sym("boxSharp.teeRight"), tc_sym("boxSharp.teeLeft"), NULL, NULL, NULL}));
        for (size_t li = 0; li < s->lines.n; li++) {
            strvec_t parts = str_split(s->lines.v[li], "\n");
            for (size_t pi = 0; pi < parts.n; pi++) {
                char *t = trim_end_ws(parts.v[pi]);
                strvec_t wl = {0};
                tui_wrap_text_with_ansi(t, MAX(1, content_width), &wl);
                for (size_t k = 0; k < wl.n; k++) {
                    sb_t ib = SB_INIT;
                    sb_append(&ib, wl.v[k]);
                    tui_sb_spaces(&ib, MAX(0, content_width - tui_visible_width(wl.v[k])));
                    vec_push(&rows, ((brow_t){2, NULL, NULL, NULL, NULL, sb_detach(&ib)}));
                }
                strvec_free(&wl);
                free(t);
            }
            strvec_free(&parts);
        }
    }
    const char *fm = o->footer_meta;
    bool fits = true;
    if (fm) {
        char *probe = xasprintf(" %s ", fm);
        char *corner = xasprintf("%s%s", tc_sym("boxRound.bottomLeft"), cap);
        fits = tui_visible_width(probe) <= line_width - tui_visible_width(corner) - tui_visible_width(tc_sym("boxRound.bottomRight"));
        free(corner);
        free(probe);
        if (!fits) {
            strvec_t wl = {0};
            tui_wrap_text_with_ansi(fm, MAX(1, content_width), &wl);
            for (size_t k = 0; k < wl.n; k++) {
                sb_t ib = SB_INIT;
                sb_append(&ib, wl.v[k]);
                tui_sb_spaces(&ib, MAX(0, content_width - tui_visible_width(wl.v[k])));
                vec_push(&rows, ((brow_t){2, NULL, NULL, NULL, NULL, sb_detach(&ib)}));
            }
            strvec_free(&wl);
        }
    }
    vec_push(&rows, ((brow_t){1, tc_sym("boxRound.bottomLeft"), tc_sym("boxRound.bottomRight"), NULL, fits ? fm : NULL, NULL}));

    bool clip = line_width < MAX(tui_visible_width(cap) + 2, pl + pr + 2);
    for (size_t r = 0; r < rows.n; r++) {
        brow_t *row = &rows.v[r];
        sb_t b = SB_INIT;
        if (row->kind == 2) {
            tc_fg_sb(&b, border_slot, v);
            tui_sb_spaces(&b, pl);
            sb_append(&b, row->inner);
            tui_sb_spaces(&b, pr);
            tc_fg_sb(&b, border_slot, v);
        } else {
            char *left = xasprintf("%s%s", row->lc, cap);
            sb_t lab = SB_INIT;
            if (row->label && *row->label) sb_append(&lab, row->label);
            if (row->meta && *row->meta) {
                if (lab.len) sb_append(&lab, tc_sym("sep.dot"));
                sb_append(&lab, row->meta);
            }
            if (line_width <= 0) {
                tc_fg_sb(&b, border_slot, left);
                tc_fg_sb(&b, border_slot, row->rc);
            } else if (!lab.len) {
                int fill = MAX(0, line_width - tui_visible_width(left) - tui_visible_width(row->rc));
                char *f = rep(h, fill);
                tc_fg_sb(&b, border_slot, left);
                tc_fg_sb(&b, border_slot, f);
                tc_fg_sb(&b, border_slot, row->rc);
                free(f);
            } else {
                char *raw = xasprintf(" %s ", sb_cstr(&lab));
                int lw = tui_visible_width(left), rw = tui_visible_width(row->rc);
                int maxl = MAX(0, line_width - lw - rw);
                char *trimmed = tui_truncate_to_width(raw, maxl, NULL);
                int fill = MAX(0, line_width - lw - tui_visible_width(trimmed) - rw);
                char *f = rep(h, fill);
                tc_fg_sb(&b, border_slot, left);
                sb_append(&b, trimmed);
                tc_fg_sb(&b, border_slot, f);
                tc_fg_sb(&b, border_slot, row->rc);
                free(f);
                free(trimmed);
                free(raw);
            }
            sb_free(&lab);
            free(left);
        }
        char *line = sb_detach(&b);
        if (clip) {
            char *t = tui_truncate_to_width(line, line_width, "");
            free(line);
            line = t;
        }
        char *padded = pad_to(line, line_width);
        free(line);
        if (bg) {
            char *wb = apply_bg(padded, bg);
            free(padded);
            padded = wb;
        }
        strvec_push_own(out, padded);
        free(row->inner);
    }
    vec_free(&rows);
    free(cap);
}

/* ---------- text helpers ---------- */

void tc_split_lines(const char *s, strvec_t *out) {
    strvec_t p = str_split(s ? s : "", "\n");
    for (size_t i = 0; i < p.n; i++) {
        size_t n = strlen(p.v[i]);
        if (n && p.v[i][n - 1] == '\r') p.v[i][n - 1] = 0;
        strvec_push_own(out, p.v[i]);
        p.v[i] = NULL;
    }
    vec_free(&p);
}

char *tc_replace_tabs(const char *s) { return tui_replace_tabs(s ? s : ""); }

char *tc_sanitize_line(const char *s) {
    const char *cr = strrchr(s, '\r');
    const char *start = cr ? cr + 1 : s;
    sb_t b = SB_INIT;
    for (const char *p = start; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\t') sb_append(&b, "   ");
        else if (c < 0x20 && c != 0x1b) continue;
        else sb_putc(&b, (char)c);
    }
    return sb_detach(&b);
}

char *tc_shorten_path(const char *p) {
    if (!p) return xstrdup("");
    const char *home = home_dir();
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(p, home, hl) && (p[hl] == '/' || p[hl] == 0)) return xasprintf("~%s", p + hl);
    return xstrdup(p);
}

char *tc_rel_path(const char *p, const char *cwd) {
    if (!p) return xstrdup("");
    if (cwd && p[0] == '/') {
        size_t cl = strlen(cwd);
        if (!strncmp(p, cwd, cl) && p[cl] == '/') return xstrdup(p + cl + 1);
        if (!strcmp(p, cwd)) return xstrdup(".");
    }
    return tc_shorten_path(p);
}

static const struct {
    const char *ext, *lang;
} EXT_LANG[] = {
    {"ts", "typescript"}, {"cts", "typescript"}, {"mts", "typescript"}, {"tsx", "tsx"}, {"js", "javascript"},
    {"jsx", "javascript"}, {"mjs", "javascript"}, {"cjs", "javascript"}, {"rs", "rust"}, {"go", "go"}, {"c", "c"},
    {"h", "c"}, {"cpp", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"}, {"hh", "cpp"}, {"hpp", "cpp"}, {"hxx", "cpp"},
    {"cu", "cpp"}, {"cuh", "cpp"}, {"ino", "cpp"}, {"zig", "zig"}, {"py", "python"}, {"pyi", "python"},
    {"rb", "ruby"}, {"rbw", "ruby"}, {"gemspec", "ruby"}, {"lua", "lua"}, {"sh", "bash"}, {"bash", "bash"},
    {"zsh", "bash"}, {"ksh", "bash"}, {"bats", "bash"}, {"tmux", "bash"}, {"cgi", "bash"}, {"fcgi", "bash"},
    {"command", "bash"}, {"tool", "bash"}, {"fish", "fish"}, {"pl", "perl"}, {"pm", "perl"}, {"perl", "perl"},
    {"php", "php"}, {"java", "java"}, {"kt", "kotlin"}, {"ktm", "kotlin"}, {"kts", "kotlin"}, {"scala", "scala"},
    {"sc", "scala"}, {"sbt", "scala"}, {"groovy", "groovy"}, {"clj", "clojure"}, {"cljc", "clojure"},
    {"cljs", "clojure"}, {"edn", "clojure"}, {"el", "emacs-lisp"}, {"cs", "csharp"}, {"fs", "fsharp"}, {"vb", "vb"},
    {"html", "html"}, {"htm", "html"}, {"xhtml", "html"}, {"css", "css"}, {"scss", "scss"}, {"sass", "sass"},
    {"less", "less"}, {"vue", "vue"}, {"svelte", "svelte"}, {"astro", "astro"}, {"json", "json"},
    {"jsonc", "jsonc"}, {"yaml", "yaml"}, {"yml", "yaml"}, {"toml", "toml"}, {"xml", "xml"}, {"xsl", "xml"},
    {"xslt", "xml"}, {"svg", "xml"}, {"plist", "xml"}, {"ini", "ini"}, {"md", "markdown"}, {"markdown", "markdown"},
    {"mdx", "markdown"}, {"mdc", "markdown"}, {"mkd", "markdown"}, {"mdown", "markdown"},
    {"rst", "restructuredtext"}, {"adoc", "asciidoc"}, {"tex", "latex"}, {"sql", "sql"}, {"graphql", "graphql"},
    {"gql", "graphql"}, {"proto", "protobuf"}, {"dockerfile", "dockerfile"}, {"containerfile", "dockerfile"},
    {"tf", "hcl"}, {"hcl", "hcl"}, {"tfvars", "hcl"}, {"nix", "nix"}, {"ex", "elixir"}, {"exs", "elixir"},
    {"erl", "erlang"}, {"hrl", "erlang"}, {"hs", "haskell"}, {"ml", "ocaml"}, {"mli", "ocaml"}, {"swift", "swift"},
    {"r", "r"}, {"jl", "julia"}, {"dart", "dart"}, {"elm", "elm"}, {"v", "verilog"}, {"nim", "nim"},
    {"cr", "crystal"}, {"d", "d"}, {"pas", "pascal"}, {"pp", "pascal"}, {"lisp", "lisp"}, {"lsp", "lisp"},
    {"rkt", "racket"}, {"scm", "scheme"}, {"ps1", "powershell"}, {"psm1", "powershell"}, {"bat", "bat"},
    {"cmd", "bat"}, {"tla", "tlaplus"}, {"tlaplus", "tlaplus"}, {"m", "objc"}, {"mm", "objc"}, {"sol", "solidity"},
    {"odin", "odin"}, {"star", "starlark"}, {"bzl", "starlark"}, {"sv", "verilog"}, {"svh", "verilog"},
    {"vh", "verilog"}, {"vim", "vim"}, {"ipynb", "ipynb"}, {"hbs", "handlebars"}, {"hsb", "handlebars"},
    {"handlebars", "handlebars"}, {"diff", "diff"}, {"patch", "diff"}, {"makefile", "make"}, {"mk", "make"},
    {"mak", "make"}, {"cmake", "cmake"}, {"justfile", "just"}, {"txt", "text"}, {"text", "text"}, {"log", "log"},
    {"csv", "csv"}, {"tsv", "tsv"}, {"cfg", "conf"}, {"conf", "conf"}, {"config", "conf"}, {"properties", "conf"},
    {"env", "env"}, {"gitignore", "conf"}, {"gitattributes", "conf"}, {"gitmodules", "conf"},
    {"editorconfig", "conf"}, {"npmrc", "conf"}, {"prettierrc", "conf"}, {"eslintrc", "conf"},
    {"prettierignore", "conf"}, {"eslintignore", "conf"},
};

const char *tc_lang_from_path(const char *p) {
    if (!p || !*p) return NULL;
    const char *slash = strrchr(p, '/');
    const char *base = slash ? slash + 1 : p;
    char *lb = str_lower(base);
    const char *special = NULL;
    if (str_starts(lb, ".env.")) special = "env";
    else if (!strcmp(lb, "dockerfile") || str_starts(lb, "dockerfile.") || !strcmp(lb, "containerfile")) special = "dockerfile";
    else if (!strcmp(lb, ".emacs")) special = "emacs-lisp";
    else if (!strcmp(lb, "justfile")) special = "just";
    else if (!strcmp(lb, "cmakelists.txt")) special = "cmake";
    free(lb);
    if (special) return special;
    const char *dot = strrchr(p, '.');
    char *key = str_lower(dot ? dot + 1 : p);
    const char *res = NULL;
    for (size_t i = 0; i < ARRAY_LEN(EXT_LANG); i++)
        if (!strcmp(EXT_LANG[i].ext, key)) {
            res = EXT_LANG[i].lang;
            break;
        }
    free(key);
    return res;
}

static const struct {
    const char *k, *sym;
} LANG_ICON[] = {
    {"typescript", "lang.typescript"}, {"ts", "lang.typescript"}, {"tsx", "lang.typescript"},
    {"javascript", "lang.javascript"}, {"js", "lang.javascript"}, {"jsx", "lang.javascript"},
    {"mjs", "lang.javascript"}, {"cjs", "lang.javascript"}, {"python", "lang.python"}, {"py", "lang.python"},
    {"rust", "lang.rust"}, {"rs", "lang.rust"}, {"go", "lang.go"}, {"java", "lang.java"}, {"c", "lang.c"},
    {"cpp", "lang.cpp"}, {"c++", "lang.cpp"}, {"cc", "lang.cpp"}, {"cxx", "lang.cpp"}, {"csharp", "lang.csharp"},
    {"cs", "lang.csharp"}, {"ruby", "lang.ruby"}, {"rb", "lang.ruby"}, {"julia", "lang.julia"}, {"jl", "lang.julia"},
    {"php", "lang.php"}, {"swift", "lang.swift"}, {"kotlin", "lang.kotlin"}, {"kt", "lang.kotlin"},
    {"bash", "lang.shell"}, {"sh", "lang.shell"}, {"zsh", "lang.shell"}, {"fish", "lang.shell"},
    {"powershell", "lang.shell"}, {"just", "lang.shell"}, {"shell", "lang.shell"}, {"html", "lang.html"},
    {"htm", "lang.html"}, {"astro", "lang.html"}, {"vue", "lang.html"}, {"svelte", "lang.html"}, {"css", "lang.css"},
    {"scss", "lang.css"}, {"sass", "lang.css"}, {"less", "lang.css"}, {"json", "lang.json"}, {"yaml", "lang.yaml"},
    {"yml", "lang.yaml"}, {"markdown", "lang.markdown"}, {"md", "lang.markdown"}, {"sql", "lang.sql"},
    {"dockerfile", "lang.docker"}, {"docker", "lang.docker"}, {"lua", "lang.lua"}, {"text", "lang.text"},
    {"txt", "lang.text"}, {"plain", "lang.text"}, {"log", "lang.log"}, {"env", "lang.env"}, {"dotenv", "lang.env"},
    {"toml", "lang.toml"}, {"xml", "lang.xml"}, {"ini", "lang.ini"}, {"conf", "lang.conf"}, {"cfg", "lang.conf"},
    {"config", "lang.conf"}, {"properties", "lang.conf"}, {"csv", "lang.csv"}, {"tsv", "lang.tsv"},
    {"image", "lang.image"}, {"img", "lang.image"}, {"png", "lang.image"}, {"jpg", "lang.image"},
    {"jpeg", "lang.image"}, {"gif", "lang.image"}, {"webp", "lang.image"}, {"svg", "lang.image"},
    {"ico", "lang.image"}, {"bmp", "lang.image"}, {"tiff", "lang.image"}, {"pdf", "lang.pdf"},
    {"zip", "lang.archive"}, {"tar", "lang.archive"}, {"gz", "lang.archive"}, {"tgz", "lang.archive"},
    {"bz2", "lang.archive"}, {"xz", "lang.archive"}, {"7z", "lang.archive"}, {"exe", "lang.binary"},
    {"dll", "lang.binary"}, {"so", "lang.binary"}, {"dylib", "lang.binary"}, {"wasm", "lang.binary"},
    {"bin", "lang.binary"},
};

const char *tc_lang_icon(const char *lang) {
    if (!lang) return tc_sym("lang.default");
    char *l = str_lower(lang);
    const char *res = NULL;
    for (size_t i = 0; i < ARRAY_LEN(LANG_ICON); i++)
        if (!strcmp(LANG_ICON[i].k, l)) {
            res = tc_sym(LANG_ICON[i].sym);
            break;
        }
    free(l);
    return res ? res : tc_sym("lang.default");
}

void tc_highlight(const char *code, const char *lang, strvec_t *out) {
    if (lang && strcmp(lang, "text") && strcmp(lang, "diff") && tui_highlight_supported(lang)) {
        tui_highlight_code(code, lang, NULL, out);
        return;
    }
    tc_split_lines(code, out);
}

char *tc_pluralize(const char *label, int64_t n) {
    if (n == 1) return xstrdup(label);
    size_t l = strlen(label);
    if ((l >= 2 && (!strcmp(label + l - 2, "ch") || !strcmp(label + l - 2, "sh"))) ||
        (l >= 1 && strchr("sxzSXZ", label[l - 1])))
        return xasprintf("%ses", label);
    if (l >= 2 && (label[l - 1] == 'y' || label[l - 1] == 'Y') && !strchr("aeiouAEIOU", label[l - 2]))
        return xasprintf("%.*sies", (int)(l - 1), label);
    return xasprintf("%ss", label);
}

char *tc_format_count(const char *label, int64_t n) {
    char *p = tc_pluralize(label, n);
    char *r = xasprintf("%lld %s", (long long)n, p);
    free(p);
    return r;
}

static char *trim1(double v) {
    char *s = xasprintf("%.1f", v);
    size_t n = strlen(s);
    if (n > 2 && !strcmp(s + n - 2, ".0")) s[n - 2] = 0;
    return s;
}

char *tc_format_number(double n) {
    const char *suf;
    double d;
    if (n < 1e3) {
        if (n == (double)(int64_t)n) return xasprintf("%lld", (long long)n);
        return xasprintf("%g", n);
    }
    if (n < 1e6) d = 1e3, suf = "K";
    else if (n < 1e9) d = 1e6, suf = "M";
    else d = 1e9, suf = "B";
    if (n < d * 10) {
        char *t = trim1(n / d);
        char *r = xasprintf("%s%s", t, suf);
        free(t);
        return r;
    }
    return xasprintf("%.0f%s", (double)(int64_t)(n / d + 0.5), suf);
}

char *tc_italic(const char *s) { return xasprintf("\x1b[3m%s\x1b[23m", s ? s : ""); }

char *tc_format_duration(int64_t ms) {
    const int64_t SEC = 1000, MIN = 60 * SEC, HOUR = 60 * MIN, DAY = 24 * HOUR;
    if (ms <= 0) return xstrdup("0ms");
    if (ms < SEC) return xasprintf("%lldms", (long long)ms);
    if (ms < MIN) {
        int64_t tenths = (ms + 50) / 100;
        return tenths < 600 ? xasprintf("%lld.%llds", (long long)(tenths / 10), (long long)(tenths % 10)) : xstrdup("1m");
    }
    if (ms < HOUR) {
        int64_t m = ms / MIN, s = (ms % MIN) / SEC;
        return s > 0 ? xasprintf("%lldm%llds", (long long)m, (long long)s) : xasprintf("%lldm", (long long)m);
    }
    if (ms < DAY) {
        int64_t hh = ms / HOUR, m = (ms % HOUR) / MIN;
        return m > 0 ? xasprintf("%lldh%lldm", (long long)hh, (long long)m) : xasprintf("%lldh", (long long)hh);
    }
    int64_t d = ms / DAY, hh = (ms % DAY) / HOUR;
    return hh > 0 ? xasprintf("%lldd%lldh", (long long)d, (long long)hh) : xasprintf("%lldd", (long long)d);
}

char *tc_expand_hint(void) {
    char *t = xasprintf("%sCtrl+O: Expand%s", tc_sym("format.bracketLeft"), tc_sym("format.bracketRight"));
    char *r = tc_fg(TUI_TC_DIM, t);
    free(t);
    return r;
}

char *tc_more_items(int64_t n, const char *item) {
    char *p = tc_pluralize(item, n);
    char *r = xasprintf("\xe2\x80\xa6 %lld more %s", (long long)n, p);
    free(p);
    return r;
}

int tc_preview_window_rows(int term_rows) {
    int rows = term_rows > 0 ? term_rows : 30;
    return MAX(6, rows - 20);
}

void tc_cap_preview(strvec_t *lines, bool expanded, int max) {
    if (expanded || (int)lines->n <= max) return;
    size_t keep = max <= 1 ? 0 : (size_t)(max - 1);
    size_t hidden = lines->n - keep;
    char *pl = tc_pluralize("line", (int64_t)hidden);
    char *hint = tc_expand_hint();
    char *marker = xasprintf("\xe2\x80\xa6 %zu earlier %s %s", hidden, pl, hint);
    /* the hint is already styled: dim the label part only, as omp wraps both in dim */
    char *label = xasprintf("\xe2\x80\xa6 %zu earlier %s ", hidden, pl);
    char *lab_dim = tc_fg(TUI_TC_DIM, label);
    strvec_t nv = {0};
    strvec_push_own(&nv, xasprintf("%s%s", lab_dim, hint));
    for (size_t i = hidden; i < lines->n; i++) {
        strvec_push_own(&nv, lines->v[i]);
        lines->v[i] = NULL;
    }
    strvec_free(lines);
    *lines = nv;
    free(lab_dim);
    free(label);
    free(marker);
    free(hint);
    free(pl);
}

void tc_output_pane(const tc_pane_t *p, strvec_t *out) {
    strvec_t styled = {0};
    for (size_t i = 0; i < p->lines->n; i++)
        strvec_push_own(&styled, p->style_plain ? xstrdup(p->lines->v[i]) : tc_fg(p->style, p->lines->v[i]));
    int limit = p->expanded ? (p->expanded_max > 0 ? p->expanded_max : -1) : p->collapsed_max;
    strvec_t visible = {0};
    size_t hidden = 0;
    if (limit >= 0) {
        if (limit == 0) {
            hidden = styled.n;
        } else if (p->visual) {
            strvec_t vis = {0};
            for (size_t i = 0; i < styled.n; i++) tui_wrap_text_with_ansi(styled.v[i], MAX(1, p->width), &vis);
            if (styled.n == 0) vis.n = 0;
            if ((int)vis.n > limit) {
                size_t start = p->tail ? vis.n - (size_t)limit : 0;
                for (size_t i = start; i < start + (size_t)limit; i++) strvec_push(&visible, vis.v[i]);
                hidden = vis.n - (size_t)limit;
            } else {
                for (size_t i = 0; i < vis.n; i++) strvec_push(&visible, vis.v[i]);
            }
            strvec_free(&vis);
        } else if ((int)styled.n > limit) {
            hidden = styled.n - (size_t)limit;
            size_t start = p->tail ? hidden : 0;
            for (size_t i = start; i < start + (size_t)limit; i++) strvec_push(&visible, styled.v[i]);
        } else {
            for (size_t i = 0; i < styled.n; i++) strvec_push(&visible, styled.v[i]);
        }
    } else {
        for (size_t i = 0; i < styled.n; i++) strvec_push(&visible, styled.v[i]);
    }
    if (hidden > 0) {
        char *label = p->tail ? xasprintf("\xe2\x80\xa6 (%zu earlier lines, showing %zu of %zu)", hidden, visible.n, hidden + visible.n)
                              : xasprintf("\xe2\x80\xa6 %zu more line%s", hidden, hidden == 1 ? "" : "s");
        sb_t m = SB_INIT;
        sb_append(&m, label);
        if (!p->expanded) {
            char *t = xasprintf(" %sCtrl+O: Expand%s", tc_sym("format.bracketLeft"), tc_sym("format.bracketRight"));
            sb_append(&m, t);
            free(t);
        }
        char *marker = tc_fg(TUI_TC_DIM, sb_cstr(&m));
        sb_free(&m);
        free(label);
        if (p->tail) strvec_push_own(out, marker);
        for (size_t i = 0; i < visible.n; i++) strvec_push(out, visible.v[i]);
        if (!p->tail) strvec_push_own(out, marker);
    } else {
        for (size_t i = 0; i < visible.n; i++) strvec_push(out, visible.v[i]);
        if (!p->expanded && p->hint_when_uncapped) strvec_push_own(out, tc_expand_hint());
    }
    strvec_free(&visible);
    strvec_free(&styled);
}

static size_t count_rows(const char *s) {
    size_t n = 1;
    for (; *s; s++) n += *s == '\n';
    return n;
}

void tc_tree_list(const strvec_t *items, bool expanded, int max_collapsed, const char *item_type, strvec_t *out) {
    tc_tree_list_ex(items, expanded, max_collapsed, -1, item_type, out);
}

void tc_tree_list_ex(const strvec_t *items, bool expanded, int max_collapsed, int max_lines, const char *item_type, strvec_t *out) {
    size_t shown = expanded ? items->n : MIN(items->n, (size_t)MAX(0, max_collapsed));
    size_t fitted = 0;
    if (!expanded && max_lines >= 0) {
        size_t fit = 0;
        for (size_t i = 0; i < shown; i++) {
            size_t cnt = count_rows(items->v[i]);
            size_t reserve = items->n - (i + 1) > 0 ? 1 : 0;
            if (fitted + cnt + reserve > (size_t)max_lines) break;
            fitted += cnt;
            fit = i + 1;
        }
        shown = fit;
    }
    size_t remaining = items->n - shown;
    bool summary = !expanded && remaining > 0 && (max_lines < 0 || fitted < (size_t)max_lines);
    char *vert = tc_fg(TUI_TC_DIM, tc_sym("tree.vertical"));
    for (size_t i = 0; i < shown; i++) {
        bool last = !summary && i + 1 == shown;
        char *conn = tc_fg(TUI_TC_DIM, last ? tc_sym("tree.last") : tc_sym("tree.branch"));
        strvec_t body = {0};
        tc_split_lines(items->v[i], &body);
        for (size_t k = 0; k < body.n; k++) {
            char *t = tc_replace_tabs(body.v[k]);
            if (k == 0) strvec_push_own(out, xasprintf("%s %s", conn, t));
            else strvec_push_own(out, xasprintf("%s%s", last ? "   " : vert, last ? t : ""));
            if (k > 0 && !last) {
                char *prev = out->v[out->n - 1];
                out->v[out->n - 1] = xasprintf("%s  %s", prev, t);
                free(prev);
            }
            free(t);
        }
        strvec_free(&body);
        free(conn);
    }
    if (summary) {
        char *conn = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
        char *m = tc_more_items((int64_t)remaining, item_type);
        char *mm = tc_fg(TUI_TC_MUTED, m);
        strvec_push_own(out, xasprintf("%s %s", conn, mm));
        free(mm);
        free(m);
        free(conn);
    }
    free(vert);
}

void tc_fit_headered(const char *header, strvec_t *rows, int width, strvec_t *out) {
    tui_wrap_text_with_ansi(header, MAX(1, width), out);
    for (size_t i = 0; i < rows->n; i++) strvec_push_own(out, tui_truncate_to_width(rows->v[i], width, ""));
}

static char *sanitize_error_text(const char *msg) {
    const char *m = msg ? msg : "";
    if (str_starts(m, "Error:")) {
        m += 6;
        while (*m == ' ' || *m == '\t' || *m == '\n') m++;
    }
    char *t = str_trim(m);
    if (!*t) {
        free(t);
        return xstrdup("Unknown error");
    }
    char *tr = tui_truncate_to_width(t, 110, NULL);
    char *r = tc_replace_tabs(tr);
    free(tr);
    free(t);
    return r;
}

char *tc_error_message(const char *msg) {
    char *s = sanitize_error_text(msg);
    char *icon = tc_styled_sym("status.error", TUI_TC_ERROR);
    char *t = xasprintf("Error: %s", s);
    char *e = tc_fg(TUI_TC_ERROR, t);
    char *r = xasprintf("%s %s", icon, e);
    free(e);
    free(t);
    free(icon);
    free(s);
    return r;
}

char *tc_error_detail(const char *msg) {
    char *s = sanitize_error_text(msg);
    char *e = tc_fg(TUI_TC_ERROR, s);
    char *r = xasprintf("  %s", e);
    free(e);
    free(s);
    return r;
}

char *tc_empty_message(const char *msg) {
    char *icon = tc_styled_sym("status.warning", TUI_TC_WARNING);
    char *m = tc_fg(TUI_TC_MUTED, msg);
    char *r = xasprintf("%s %s", icon, m);
    free(m);
    free(icon);
    return r;
}

char *tc_text_of(const json_t *content) {
    json_t *blk;
    if (!json_is_arr(content)) return xstrdup("");
    json_arr_foreach(content, i, blk) {
        if (str_eq(json_get_str(blk, "type"), "text")) return xstrdup(json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
    }
    return xstrdup("");
}

void tc_push_wrapped(strvec_t *out, const char *s, int width) { tui_wrap_text_with_ansi(s, MAX(1, width), out); }

/* ---------- JSON args / tree (tools/json-tree.ts) ---------- */

static bool is_meta_key(const char *k) { return !strcmp(k, "i") || !strcmp(k, "__partialJson"); }

/* formatScalar: quoted/escaped strings, `[N items]`, `{N keys}` */
static char *format_scalar(const json_t *v, int max) {
    if (!v || json_is_null(v)) return xstrdup("null");
    if (json_is_bool(v) || json_is_num(v)) return json_dump(v, 0);
    if (json_is_str(v)) {
        char *a = str_replace(json_sv(v), "\n", "\\n");
        char *b2 = str_replace(a, "\t", "\\t");
        char *t = tui_truncate_to_width(b2, MAX(0, max), NULL);
        char *r = xasprintf("\"%s\"", t);
        free(t);
        free(b2);
        free(a);
        return r;
    }
    if (json_is_arr(v)) return xasprintf("[%zu items]", json_len(v));
    return xasprintf("{%zu keys}", json_len(v));
}

char *tc_json_inline(const json_t *args, int max_width) {
    if (!json_is_obj(args)) return xstrdup("");
    strvec_t keys = {0};
    const char *k;
    json_t *v;
    json_obj_foreach(args, i, k, v) {
        if (!is_meta_key(k)) strvec_push(&keys, k);
    }
    sb_t res = SB_INIT;
    int width = 0;
    for (size_t ki = 0; ki < keys.n; ki++) {
        const char *key = keys.v[ki];
        const char *sep = width > 0 ? ", " : "";
        int sepw = width > 0 ? 2 : 0;
        int current = width + sepw;
        int cap = max_width - current - 1;
        if (cap <= 0) {
            sb_append(&res, "\xe2\x80\xa6");
            break;
        }
        int tail = 0;
        for (size_t j = ki + 1; j < keys.n; j++) tail += 2 + tui_visible_width(keys.v[j]) + 1 + 4;
        int piece_budget = MIN(cap, max_width - current - tail);
        int vmax = MAX(1, piece_budget - tui_visible_width(key) - 3);
        char *vs = format_scalar(json_get(args, key), vmax);
        char *piece = xasprintf("%s=%s", key, vs);
        int pw = tui_visible_width(piece);
        if (pw > piece_budget) {
            char *t = tui_truncate_to_width(piece, cap, NULL);
            sb_append(&res, sep);
            sb_append(&res, t);
            free(t);
            free(piece);
            free(vs);
            break;
        }
        sb_append(&res, sep);
        sb_append(&res, piece);
        width = current + pw;
        free(piece);
        free(vs);
    }
    strvec_free(&keys);
    return sb_detach(&res);
}

/* renderJsonTreeLines (tools/json-tree.ts) with hooked roots: `└ • key: value` root rows, nested rows branching
 * from under the root label; containers whose one-line form fits 60 chars render inline (`key: [1, 2]`),
 * larger ones as `key {N}` / `key [N]` with children (a `…` child past max_depth); multi-line strings
 * hang under their key. Stops at max_lines rows (*truncated set). */

typedef struct {
    int max_depth, max_lines, max_scalar;
    int rows, max_rows;
    strvec_t *out;
    size_t start;
    bool truncated, full;
    int rendered; /* body lines rendered (multi-line string budget) */
} jt_ctx_t;

static bool bare_key(const char *k) {
    if (!*k) return false;
    for (const char *p = k; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '$' || *p == '-')) return false;
    return true;
}

static char *js_number(const json_t *v) { return json_dump(v, 0); }

static size_t u16len(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n += (*p >= 0xF0) ? 2 : 1;
    return n;
}

/* inlineJson: styled one-line form, NULL when it exceeds 60 chars */
typedef struct {
    sb_t b;
    size_t len;
    int last_slot;
    bool over;
} jinl_t;

static void jinl_push(jinl_t *j, const char *text, tui_tc_t slot) {
    if (j->over) return;
    j->len += u16len(text);
    if (j->len > 60) {
        j->over = true;
        return;
    }
    if (j->last_slot != (int)slot) {
        if (j->last_slot >= 0) sb_append(&j->b, "\x1b[39m");
        sb_append(&j->b, tui_fg_seq(NULL, slot));
        j->last_slot = (int)slot;
    }
    sb_append(&j->b, text);
}

static char *jt_quote(const char *s) {
    json_t *t = json_str(s);
    char *r = json_dump(t, 0);
    json_free(t);
    return r;
}

static void jinl_walk(jinl_t *j, const json_t *v) {
    if (j->over) return;
    if (json_is_arr(v)) {
        jinl_push(j, "[", TUI_TC_MUTED);
        for (size_t i = 0; i < json_len(v) && !j->over; i++) {
            if (i) jinl_push(j, ", ", TUI_TC_MUTED);
            jinl_walk(j, json_at(v, i));
        }
        jinl_push(j, "]", TUI_TC_MUTED);
    } else if (json_is_obj(v)) {
        jinl_push(j, "{", TUI_TC_MUTED);
        for (size_t i = 0; i < json_len(v) && !j->over; i++) {
            const char *k = v->u.obj.keys[i];
            char *q = bare_key(k) ? xstrdup(k) : jt_quote(k);
            char *t = xasprintf("%s%s: ", i ? ", " : "", q);
            jinl_push(j, t, TUI_TC_MUTED);
            free(t);
            free(q);
            jinl_walk(j, v->u.obj.vals[i]);
        }
        jinl_push(j, "}", TUI_TC_MUTED);
    } else if (json_is_str(v)) {
        char *q = jt_quote(json_sv(v));
        jinl_push(j, q, TUI_TC_SYNTAX_STRING);
        free(q);
    } else {
        char *s = !v || json_is_null(v) ? xstrdup("null") : js_number(v);
        jinl_push(j, s, json_is_num(v) ? TUI_TC_SYNTAX_NUMBER : TUI_TC_SYNTAX_KEYWORD);
        free(s);
    }
}

static char *inline_json(const json_t *v) {
    if (!json_is_arr(v) && !json_is_obj(v)) return NULL;
    jinl_t j = {.b = SB_INIT, .last_slot = -1};
    jinl_walk(&j, v);
    if (j.over) {
        sb_free(&j.b);
        return NULL;
    }
    if (j.last_slot >= 0) sb_append(&j.b, "\x1b[39m");
    return sb_detach(&j.b);
}

/* one row: body lines for a node */
static void jt_body(jt_ctx_t *c, const json_t *v, const char *key, tui_tc_t kc, bool placeholder, strvec_t *body) {
    if (placeholder) {
        strvec_push_own(body, tc_fg(TUI_TC_DIM, "\xe2\x80\xa6"));
        return;
    }
    char *inl = inline_json(v);
    const char *dk = key ? key : json_is_arr(v) ? "array" : json_is_obj(v) ? "object" : "value";
    char *label = tc_fg(key ? kc : TUI_TC_MUTED, dk);
    if (inl) {
        strvec_push_own(body, xasprintf("%s: %s", label, inl));
    } else if (json_is_arr(v)) {
        char n[32];
        snprintf(n, sizeof n, "[%zu]", json_len(v));
        char *d = tc_fg(TUI_TC_DIM, n);
        strvec_push_own(body, xasprintf("%s %s", label, d));
        free(d);
    } else if (json_is_obj(v)) {
        char n[32];
        snprintf(n, sizeof n, "{%zu}", json_len(v));
        char *d = tc_fg(TUI_TC_DIM, n);
        strvec_push_own(body, xasprintf("%s %s", label, d));
        free(d);
    } else if (json_is_str(v) && strchr(json_sv(v), '\n')) {
        strvec_t src = str_split(json_sv(v), "\n");
        int avail = MAX(1, c->max_lines - c->rendered);
        size_t shown = MIN(src.n, (size_t)MAX(1, avail - 1));
        char *head = xasprintf("%s: ", label);
        char *hp = tui_strip_ansi(head);
        int iw = tui_visible_width(hp) + 1;
        free(hp);
        for (size_t i = 0; i < shown; i++) {
            char *t = tui_truncate_to_width(src.v[i], c->max_scalar, NULL);
            char *q = i == 0 ? xasprintf("\"%s", t) : xstrdup(t);
            char *s = tc_fg(TUI_TC_SYNTAX_STRING, q);
            strvec_push_own(body, i == 0 ? xasprintf("%s%s", head, s) : xasprintf("%*s%s", iw, "", s));
            free(s);
            free(q);
            free(t);
        }
        if (src.n > shown) {
            c->truncated = true;
            char *m = xasprintf("\xe2\x80\xa6(%zu more lines)\"", src.n - shown);
            char *d = tc_fg(TUI_TC_DIM, m);
            strvec_push_own(body, xasprintf("%*s%s", iw, "", d));
            free(d);
            free(m);
        } else {
            char *q = tc_fg(TUI_TC_SYNTAX_STRING, "\"");
            char *last = body->v[body->n - 1];
            body->v[body->n - 1] = xasprintf("%s%s", last, q);
            free(last);
            free(q);
        }
        free(head);
        strvec_free(&src);
    } else {
        char *s = format_scalar(v, c->max_scalar);
        tui_tc_t slot = json_is_str(v) ? TUI_TC_SYNTAX_STRING : json_is_num(v) ? TUI_TC_SYNTAX_NUMBER : TUI_TC_SYNTAX_KEYWORD;
        char *st = tc_fg(slot, s);
        strvec_push_own(body, xasprintf("%s: %s", label, st));
        free(st);
        free(s);
    }
    c->rendered += (int)body->n;
    free(label);
    free(inl);
}

/* ancestors: isLast flags of ancestors below the root level (index 0 = root's child level) */
static void jt_emit(jt_ctx_t *c, const json_t *v, const char *key, tui_tc_t kc, bool placeholder, int depth, bool *anc,
                    int nanc, bool is_last, int sib_index) {
    if (c->rows >= c->max_rows || c->full) {
        c->truncated = true;
        return;
    }
    c->rows++;
    const char *hook = tc_sym("tree.hook"), *bullet = tc_sym("format.bullet");
    int hw = tui_visible_width(hook), bw = tui_visible_width(bullet);
    char *gutter = xasprintf("%*s %*s ", hw, "", bw, "");
    char *first, *cont;
    if (nanc < 0) { /* a root row */
        char *lead = sib_index == 0 ? tc_fg(TUI_TC_DIM, hook) : xasprintf("%*s", hw, "");
        char *bd = tc_fg(TUI_TC_DIM, bullet);
        first = xasprintf("%s %s ", lead, bd);
        cont = xstrdup(gutter);
        free(bd);
        free(lead);
    } else {
        sb_t a = SB_INIT;
        for (int i = 0; i < nanc; i++) sb_append(&a, anc[i] ? "   " : "\xe2\x94\x82  ");
        char *conn = tc_fg(TUI_TC_DIM, tc_sym(is_last ? "tree.last" : "tree.branch"));
        first = xasprintf("%s%s%s ", gutter, sb_cstr(&a), conn);
        cont = xasprintf("%s%s%s", gutter, sb_cstr(&a), is_last ? "   " : "\xe2\x94\x82  ");
        free(conn);
        sb_free(&a);
    }
    strvec_t body = {0};
    jt_body(c, v, key, kc, placeholder, &body);
    for (size_t i = 0; i < body.n; i++) {
        if ((int)(c->out->n - c->start) >= c->max_lines) {
            c->truncated = true;
            c->full = true;
            break;
        }
        char *t = tc_replace_tabs(body.v[i]);
        strvec_push_own(c->out, xasprintf("%s%s", i ? cont : first, t));
        free(t);
    }
    if ((int)(c->out->n - c->start) >= c->max_lines) c->full = true;
    strvec_free(&body);
    free(first);
    free(cont);
    free(gutter);
    /* inline, scalar and placeholder rows have no children */
    if (placeholder || !(json_is_arr(v) || json_is_obj(v))) return;
    char *x = inline_json(v);
    bool inl = x != NULL;
    free(x);
    if (inl) return;
    size_t n = json_len(v);
    if (!n) return;
    bool *nanc_v = xcalloc((size_t)(nanc < 0 ? 0 : nanc) + 1, sizeof(bool));
    int child_nanc = nanc < 0 ? 0 : nanc + 1;
    if (nanc > 0) memcpy(nanc_v, anc, (size_t)nanc * sizeof(bool));
    if (nanc >= 0) nanc_v[nanc] = is_last;
    if (depth >= c->max_depth) {
        jt_emit(c, NULL, NULL, TUI_TC_DIM, true, depth + 1, nanc_v, child_nanc, true, 0);
    } else {
        int w = 1;
        if (json_is_arr(v)) {
            char b[24];
            w = snprintf(b, sizeof b, "%zu", n - 1);
        }
        for (size_t i = 0; i < n; i++) {
            char kb[32];
            const char *ck;
            const json_t *cv;
            tui_tc_t kcol;
            if (json_is_arr(v)) {
                snprintf(kb, sizeof kb, "%*zu", w, i);
                ck = kb;
                cv = json_at(v, i);
                kcol = TUI_TC_DIM;
            } else {
                ck = v->u.obj.keys[i];
                cv = v->u.obj.vals[i];
                kcol = TUI_TC_MUTED;
            }
            jt_emit(c, cv, ck, kcol, false, depth + 1, nanc_v, child_nanc, i + 1 == n, (int)i);
        }
    }
    free(nanc_v);
}

void tc_json_tree(const json_t *v, int max_depth, int max_lines, int max_scalar, bool hide_meta, strvec_t *out, bool *truncated) {
    jt_ctx_t c = {.max_depth = MAX(0, max_depth), .max_lines = MAX(0, max_lines), .max_scalar = MAX(0, max_scalar),
                  .max_rows = MAX(0, max_lines) + 1, .out = out, .start = out->n};
    if (json_is_arr(v) || json_is_obj(v)) {
        size_t n = json_len(v);
        VEC(size_t) idx = {0};
        for (size_t i = 0; i < n; i++)
            if (!(json_is_obj(v) && hide_meta && is_meta_key(v->u.obj.keys[i]))) vec_push(&idx, i);
        int w = 1;
        if (json_is_arr(v) && n) {
            char b[24];
            w = snprintf(b, sizeof b, "%zu", n - 1);
        }
        for (size_t k = 0; k < idx.n; k++) {
            size_t i = idx.v[k];
            char kb[32];
            if (json_is_arr(v)) {
                snprintf(kb, sizeof kb, "%*zu", w, i);
                jt_emit(&c, json_at(v, i), kb, TUI_TC_DIM, false, 1, NULL, -1, k + 1 == idx.n, (int)k);
            } else {
                jt_emit(&c, v->u.obj.vals[i], v->u.obj.keys[i], TUI_TC_MUTED, false, 1, NULL, -1, k + 1 == idx.n, (int)k);
            }
        }
        vec_free(&idx);
    } else {
        jt_emit(&c, v, NULL, TUI_TC_MUTED, false, 0, NULL, -1, true, 0);
    }
    if (truncated) *truncated = c.truncated;
}
