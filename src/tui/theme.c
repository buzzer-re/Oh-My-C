/* theme.c - color slots, built-in themes, JSON theme loading. */
#include "tui/theme.h"
#include "tui/color.h"
#include "core/embed.h"
#include "core/fs.h"

static const char *SLOT_NAMES[TUI_TC__COUNT] = {
#define TUI_TC_NAME(id, name) name,
    TUI_THEME_SLOTS(TUI_TC_NAME)
#undef TUI_TC_NAME
};

const char *tui_theme_slot_name(tui_tc_t slot) { return slot < TUI_TC__COUNT ? SLOT_NAMES[slot] : "?"; }

int tui_theme_slot_from_name(const char *name) {
    for (int i = 0; i < TUI_TC__COUNT; i++)
        if (str_eq(SLOT_NAMES[i], name)) return i;
    return -1;
}

static bool slot_optional(int i) {
    return i == TUI_TC_THINKING_MAX || i == TUI_TC_ASSISTANT_MESSAGE_TEXT || i == TUI_TC_LINK;
}

/* Upstream packages/tui/src/theme/dark.json and light.json (vars + colors). */
static const char DARK_JSON[] =
    "{\"name\":\"dark\",\"vars\":{\"cyan\":\"#0088fa\",\"blue\":\"#178fb9\",\"green\":\"#89d281\","
    "\"red\":\"#fc3a4b\",\"yellow\":\"#e4c00f\",\"gray\":\"#777d88\",\"dimGray\":\"#5f6673\","
    "\"darkGray\":\"#3d424a\",\"accent\":\"#febc38\",\"selectedBg\":\"#31363f\",\"userMsgBg\":\"#221d1a\","
    "\"toolPendingBg\":\"#1d2129\",\"toolSuccessBg\":\"#161a1f\",\"toolErrorBg\":\"#291d1d\","
    "\"customMsgBg\":\"#2a2530\"},\"colors\":{\"accent\":\"accent\",\"border\":\"blue\","
    "\"borderAccent\":\"cyan\",\"borderMuted\":\"darkGray\",\"success\":\"green\",\"error\":\"red\","
    "\"warning\":\"yellow\",\"muted\":\"gray\",\"dim\":\"dimGray\",\"text\":\"\",\"thinkingText\":\"gray\","
    "\"selectedBg\":\"selectedBg\",\"userMessageBg\":\"userMsgBg\",\"userMessageText\":\"\","
    "\"customMessageBg\":\"customMsgBg\",\"customMessageText\":\"\",\"customMessageLabel\":\"#b281d6\","
    "\"toolPendingBg\":\"toolPendingBg\",\"toolSuccessBg\":\"toolSuccessBg\",\"toolErrorBg\":\"toolErrorBg\","
    "\"toolTitle\":\"\",\"toolOutput\":\"gray\",\"mdHeading\":\"#febc38\",\"mdLink\":\"#0088fa\","
    "\"mdLinkUrl\":\"dimGray\",\"mdCode\":\"#e5c1ff\",\"mdCodeBlock\":\"#9CDCFE\",\"mdCodeBlockBorder\":\"gray\","
    "\"mdQuote\":\"gray\",\"mdQuoteBorder\":\"darkGray\",\"mdHr\":\"darkGray\",\"mdListBullet\":\"accent\","
    "\"toolDiffAdded\":\"green\",\"toolDiffRemoved\":\"red\",\"toolDiffContext\":\"gray\",\"link\":\"#0088fa\","
    "\"syntaxComment\":\"#6A9955\",\"syntaxKeyword\":\"#569CD6\",\"syntaxFunction\":\"#DCDCAA\","
    "\"syntaxVariable\":\"#9CDCFE\",\"syntaxString\":\"#CE9178\",\"syntaxNumber\":\"#B5CEA8\","
    "\"syntaxType\":\"#4EC9B0\",\"syntaxOperator\":\"#D4D4D4\",\"syntaxPunctuation\":\"#D4D4D4\","
    "\"thinkingOff\":\"darkGray\",\"thinkingMinimal\":\"dimGray\",\"thinkingLow\":\"#178fb9\","
    "\"thinkingMedium\":\"#0088fa\",\"thinkingHigh\":\"#b281d6\",\"thinkingXhigh\":\"#e5c1ff\","
    "\"bashMode\":\"cyan\",\"statusLineBg\":\"#121212\",\"statusLineSep\":244,\"statusLineModel\":\"#d787af\","
    "\"statusLinePath\":\"#00afaf\",\"statusLineGitClean\":\"#5faf5f\",\"statusLineGitDirty\":\"#d7af5f\","
    "\"statusLineContext\":\"#8787af\",\"statusLineSpend\":\"#5fafaf\",\"statusLineStaged\":70,"
    "\"statusLineDirty\":178,\"statusLineUntracked\":39,\"statusLineOutput\":205,\"statusLineCost\":205,"
    "\"statusLineSubagents\":\"accent\",\"pythonMode\":\"yellow\"}}";

static const char LIGHT_JSON[] =
    "{\"name\":\"light\",\"vars\":{\"teal\":\"#5a8080\",\"blue\":\"#547da7\",\"green\":\"#588458\","
    "\"red\":\"#aa5555\",\"yellow\":\"#9a7326\",\"mediumGray\":\"#6c6c6c\",\"dimGray\":\"#767676\","
    "\"lightGray\":\"#b0b0b0\",\"selectedBg\":\"#d0d0e0\",\"userMsgBg\":\"#e8e8e8\","
    "\"toolPendingBg\":\"#e8e8f0\",\"toolSuccessBg\":\"#e8f0e8\",\"toolErrorBg\":\"#f0e8e8\","
    "\"customMsgBg\":\"#ede7f6\"},\"colors\":{\"accent\":\"teal\",\"border\":\"blue\",\"borderAccent\":\"teal\","
    "\"borderMuted\":\"lightGray\",\"success\":\"green\",\"error\":\"red\",\"warning\":\"yellow\","
    "\"muted\":\"mediumGray\",\"dim\":\"dimGray\",\"text\":\"\",\"thinkingText\":\"mediumGray\","
    "\"selectedBg\":\"selectedBg\",\"userMessageBg\":\"userMsgBg\",\"userMessageText\":\"\","
    "\"customMessageBg\":\"customMsgBg\",\"customMessageText\":\"\",\"customMessageLabel\":\"#7e57c2\","
    "\"toolPendingBg\":\"toolPendingBg\",\"toolSuccessBg\":\"toolSuccessBg\",\"toolErrorBg\":\"toolErrorBg\","
    "\"toolTitle\":\"\",\"toolOutput\":\"mediumGray\",\"mdHeading\":\"yellow\",\"mdLink\":\"blue\","
    "\"mdLinkUrl\":\"dimGray\",\"mdCode\":\"teal\",\"mdCodeBlock\":\"green\",\"mdCodeBlockBorder\":\"mediumGray\","
    "\"mdQuote\":\"mediumGray\",\"mdQuoteBorder\":\"mediumGray\",\"mdHr\":\"mediumGray\","
    "\"mdListBullet\":\"green\",\"toolDiffAdded\":\"green\",\"toolDiffRemoved\":\"red\","
    "\"toolDiffContext\":\"mediumGray\",\"syntaxComment\":\"#008000\",\"syntaxKeyword\":\"#0000FF\","
    "\"syntaxFunction\":\"#795E26\",\"syntaxVariable\":\"#001080\",\"syntaxString\":\"#A31515\","
    "\"syntaxNumber\":\"#098658\",\"syntaxType\":\"#267F99\",\"syntaxOperator\":\"#000000\","
    "\"syntaxPunctuation\":\"#000000\",\"thinkingOff\":\"lightGray\",\"thinkingMinimal\":\"#767676\","
    "\"thinkingLow\":\"blue\",\"thinkingMedium\":\"teal\",\"thinkingHigh\":\"#875f87\","
    "\"thinkingXhigh\":\"#8b008b\",\"bashMode\":\"green\",\"statusLineBg\":\"#e0e0e0\","
    "\"statusLineSep\":\"#808080\",\"statusLineModel\":\"#875f87\",\"statusLinePath\":\"#005f87\","
    "\"statusLineGitClean\":\"#005f00\",\"statusLineGitDirty\":\"#af5f00\",\"statusLineContext\":\"#5f5f87\","
    "\"statusLineSpend\":\"#005f5f\",\"statusLineStaged\":28,\"statusLineDirty\":136,"
    "\"statusLineUntracked\":31,\"statusLineOutput\":133,\"statusLineCost\":133,"
    "\"statusLineSubagents\":\"teal\",\"pythonMode\":\"yellow\"}}";

static const char *DEFAULT_SPINNER[] = {"\xe2\xa3\xbe", "\xe2\xa3\xbd", "\xe2\xa3\xbb", "\xe2\xa2\xbf",
                                        "\xe2\xa1\xbf", "\xe2\xa3\x9f", "\xe2\xa3\xaf", "\xe2\xa3\xb7"};

tui_color_mode_t tui_detect_color_mode(void) {
    const char *ct = env_get("COLORTERM");
    if (ct && (str_ieq(ct, "truecolor") || str_ieq(ct, "24bit"))) return TUI_COLOR_TRUECOLOR;
    const char *tp = env_get("TERM_PROGRAM");
    if (tp && (str_eq(tp, "iTerm.app") || str_eq(tp, "WezTerm") || str_eq(tp, "vscode") || str_eq(tp, "ghostty") ||
               str_eq(tp, "Hyper") || str_eq(tp, "tern")))
        return TUI_COLOR_TRUECOLOR;
    const char *term = env_get("TERM");
    if (term && (strstr(term, "kitty") || strstr(term, "direct") || strstr(term, "ghostty") || strstr(term, "wezterm") ||
                 strstr(term, "alacritty") || strstr(term, "foot")))
        return TUI_COLOR_TRUECOLOR;
    if (env_get("WT_SESSION")) return TUI_COLOR_TRUECOLOR;
    return TUI_COLOR_256;
}

const char *tui_detect_theme_name(void) {
    const char *fgbg = env_get("COLORFGBG");
    if (fgbg) {
        const char *semi = strrchr(fgbg, ';');
        int bg = atoi(semi ? semi + 1 : fgbg);
        if (bg == 7 || (bg >= 9 && bg <= 15)) return "light";
    }
    return "dark";
}

int tui_rgb_to_256(int r, int g, int b) {
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    int ci[3], comp[3] = {r, g, b};
    for (int k = 0; k < 3; k++) {
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 6; i++) {
            int d = abs(comp[k] - lv[i]);
            if (d < bd) bd = d, best = i;
        }
        ci[k] = best;
    }
    int cr = lv[ci[0]], cg = lv[ci[1]], cb = lv[ci[2]];
    long dc = (long)(r - cr) * (r - cr) + (long)(g - cg) * (g - cg) + (long)(b - cb) * (b - cb);
    int avg = (r + g + b) / 3;
    int gi = avg < 8 ? 0 : avg > 238 ? 23 : (avg - 8 + 5) / 10;
    if (gi > 23) gi = 23;
    int gv = 8 + 10 * gi;
    long dg = (long)(r - gv) * (r - gv) + (long)(g - gv) * (g - gv) + (long)(b - gv) * (b - gv);
    if (dg < dc) return 232 + gi;
    return 16 + 36 * ci[0] + 6 * ci[1] + ci[2];
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *s, int *r, int *g, int *b) {
    if (!s || s[0] != '#' || strlen(s) != 7) return false;
    int v[6];
    for (int i = 0; i < 6; i++)
        if ((v[i] = hexv(s[1 + i])) < 0) return false;
    *r = v[0] * 16 + v[1];
    *g = v[2] * 16 + v[3];
    *b = v[4] * 16 + v[5];
    return true;
}

/* Resolve a color value to escapes. Returns false if invalid. */
static bool resolve_color(tui_theme_t *t, int slot, const json_t *val, const json_t *vars, int depth, char **err) {
    if (depth > 8) {
        *err = xasprintf("color '%s': variable reference too deep", SLOT_NAMES[slot]);
        return false;
    }
    if (json_is_num(val)) {
        int idx = (int)json_d2i(val->u.num.d);
        if (idx < 0 || idx > 255) {
            *err = xasprintf("color '%s': palette index out of range", SLOT_NAMES[slot]);
            return false;
        }
        snprintf(t->fg[slot], sizeof t->fg[slot], "\x1b[38;5;%dm", idx);
        snprintf(t->bg[slot], sizeof t->bg[slot], "\x1b[48;5;%dm", idx);
        t->rgb[slot] = tui_palette_rgb(idx);
        return true;
    }
    const char *s = json_sv(val);
    if (!s) {
        *err = xasprintf("color '%s': expected string or number", SLOT_NAMES[slot]);
        return false;
    }
    if (!*s) {
        strcpy(t->fg[slot], "\x1b[39m");
        strcpy(t->bg[slot], "\x1b[49m");
        t->rgb[slot] = -1;
        return true;
    }
    int r, g, b;
    if (parse_hex(s, &r, &g, &b)) {
        if (t->mode == TUI_COLOR_TRUECOLOR) {
            snprintf(t->fg[slot], sizeof t->fg[slot], "\x1b[38;2;%d;%d;%dm", r, g, b);
            snprintf(t->bg[slot], sizeof t->bg[slot], "\x1b[48;2;%d;%d;%dm", r, g, b);
        } else {
            int idx = tui_rgb_to_256(r, g, b);
            snprintf(t->fg[slot], sizeof t->fg[slot], "\x1b[38;5;%dm", idx);
            snprintf(t->bg[slot], sizeof t->bg[slot], "\x1b[48;5;%dm", idx);
        }
        t->rgb[slot] = (r << 16) | (g << 8) | b;
        return true;
    }
    const json_t *ref = vars ? json_get(vars, s) : NULL;
    if (!ref) {
        *err = xasprintf("color '%s': unknown variable or invalid color '%s'", SLOT_NAMES[slot], s);
        return false;
    }
    return resolve_color(t, slot, ref, vars, depth + 1, err);
}

/* fgAnsi(terminal.ansi[index] ?? index): var references resolve like resolveVarRefs; a bad entry keeps the index */
static void resolve_ansi_palette(tui_theme_t *t, const json_t *terminal, const json_t *vars) {
    const json_t *ansi = terminal ? json_get(terminal, "ansi") : NULL;
    for (int i = 0; i < 16; i++) {
        snprintf(t->ansi_fg[i], sizeof t->ansi_fg[i], "\x1b[38;5;%dm", i);
        const json_t *v = json_is_arr(ansi) ? json_at(ansi, (size_t)i) : NULL;
        for (int depth = 0; v && json_is_str(v) && depth < 8; depth++) {
            const char *s = json_sv(v);
            int r, g, b;
            if (!*s) {
                strcpy(t->ansi_fg[i], "\x1b[39m");
                v = NULL;
            } else if (parse_hex(s, &r, &g, &b)) {
                if (t->mode == TUI_COLOR_TRUECOLOR) snprintf(t->ansi_fg[i], sizeof t->ansi_fg[i], "\x1b[38;2;%d;%d;%dm", r, g, b);
                else snprintf(t->ansi_fg[i], sizeof t->ansi_fg[i], "\x1b[38;5;%dm", tui_rgb_to_256(r, g, b));
                v = NULL;
            } else {
                v = vars ? json_get(vars, s) : NULL;
            }
        }
        if (v && json_is_num(v)) {
            int idx = (int)json_d2i(v->u.num.d);
            if (idx >= 0 && idx <= 255) snprintf(t->ansi_fg[i], sizeof t->ansi_fg[i], "\x1b[38;5;%dm", idx);
        }
    }
}

void tui_theme_free(tui_theme_t *t) {
    if (!t) return;
    free(t->name);
    for (int i = 0; i < t->nspinner; i++) free(t->spinner[i]);
    free(t);
}

tui_theme_t *tui_theme_from_json(const json_t *j, tui_color_mode_t mode, char **err) {
    char *e = NULL;
    if (!json_is_obj(j)) {
        if (err) *err = xstrdup("theme must be a JSON object");
        return NULL;
    }
    const json_t *colors = json_get_obj(j, "colors");
    const json_t *vars = json_get_obj(j, "vars");
    if (!colors) {
        if (err) *err = xstrdup("theme is missing 'colors'");
        return NULL;
    }
    tui_theme_t *t = xcalloc(1, sizeof *t);
    t->mode = mode;
    for (int i = 0; i < TUI_TC__COUNT; i++) t->rgb[i] = -1;
    t->name = xstrdup(json_get_str(j, "name") ? json_get_str(j, "name") : "custom");
    sb_t missing = SB_INIT;
    for (int i = 0; i < TUI_TC__COUNT; i++) {
        const json_t *v = json_get(colors, SLOT_NAMES[i]);
        if (!v) {
            if (!slot_optional(i)) {
                if (missing.len) sb_append(&missing, ", ");
                sb_append(&missing, SLOT_NAMES[i]);
            }
            continue;
        }
        if (!resolve_color(t, i, v, vars, 0, &e)) break;
    }
    if (!e && missing.len) e = xasprintf("theme '%s' is missing required colors: %s", t->name, missing.s);
    sb_free(&missing);
    if (e) {
        if (err) *err = e;
        else free(e);
        tui_theme_free(t);
        return NULL;
    }
    /* optional fallbacks */
    if (!json_get(colors, "thinkingMax")) {
        memcpy(t->fg[TUI_TC_THINKING_MAX], t->fg[TUI_TC_THINKING_XHIGH], sizeof t->fg[0]);
        memcpy(t->bg[TUI_TC_THINKING_MAX], t->bg[TUI_TC_THINKING_XHIGH], sizeof t->bg[0]);
        t->rgb[TUI_TC_THINKING_MAX] = t->rgb[TUI_TC_THINKING_XHIGH];
    }
    if (!json_get(colors, "assistantMessageText")) {
        strcpy(t->fg[TUI_TC_ASSISTANT_MESSAGE_TEXT], "\x1b[39m");
        strcpy(t->bg[TUI_TC_ASSISTANT_MESSAGE_TEXT], "\x1b[49m");
    }
    if (!json_get(colors, "link")) {
        memcpy(t->fg[TUI_TC_LINK], t->fg[TUI_TC_MD_LINK], sizeof t->fg[0]);
        memcpy(t->bg[TUI_TC_LINK], t->bg[TUI_TC_MD_LINK], sizeof t->bg[0]);
        t->rgb[TUI_TC_LINK] = t->rgb[TUI_TC_MD_LINK];
    }
    resolve_ansi_palette(t, json_get_obj(j, "terminal"), vars);
    /* spinner frames: symbols.spinnerFrames = [..] | {"status": [..]} */
    const json_t *sym = json_get_obj(j, "symbols");
    const json_t *sf = sym ? json_get(sym, "spinnerFrames") : NULL;
    if (json_is_obj(sf)) sf = json_get(sf, "status");
    if (json_is_arr(sf)) {
        const json_t *it;
        json_arr_foreach(sf, i, it) {
            if (json_is_str(it) && t->nspinner < TUI_MAX_SPINNER_FRAMES) t->spinner[t->nspinner++] = xstrdup(json_sv(it));
        }
    }
    t->spinner_custom = t->nspinner > 0;
    if (!t->nspinner)
        for (size_t i = 0; i < ARRAY_LEN(DEFAULT_SPINNER); i++) t->spinner[t->nspinner++] = xstrdup(DEFAULT_SPINNER[i]);
    return t;
}

tui_theme_t *tui_theme_builtin(const char *name, tui_color_mode_t mode) {
    /* upstream omp themes (dark, light and the 100 bundled ones) are embedded verbatim */
    if (name && *name && !strchr(name, '/') && !strstr(name, "..")) {
        char *rel = xasprintf("assets/tui/themes/%s.json", name);
        size_t len = 0;
        const char *data = embed_get(rel, &len);
        free(rel);
        if (data) {
            json_t *j = json_parse(data, len, NULL);
            tui_theme_t *t = j ? tui_theme_from_json(j, mode, NULL) : NULL;
            json_free(j);
            if (t) return t;
        }
    }
    const char *src = str_eq(name, "dark") ? DARK_JSON : str_eq(name, "light") ? LIGHT_JSON : NULL;
    if (!src) return NULL;
    json_t *j = json_parse_str(src);
    if (!j) die("tui: built-in theme '%s' failed to parse", name);
    tui_theme_t *t = tui_theme_from_json(j, mode, NULL);
    json_free(j);
    if (!t) die("tui: built-in theme '%s' is invalid", name);
    return t;
}

tui_theme_t *tui_theme_load(const char *name, tui_color_mode_t mode, char **err) {
    tui_theme_t *t = tui_theme_builtin(name, mode);
    if (t) return t;
    if (!name || !*name || strchr(name, '/') || strstr(name, "..")) {
        if (err) *err = xstrdup("invalid theme name");
        return NULL;
    }
    char *rel = xasprintf("themes/%s.json", name);
    char *path = omc_agent_path(rel);
    free(rel);
    size_t len;
    char *data = fs_read_limit(path, 1 << 20, &len, NULL);
    if (!data) {
        if (err) *err = xasprintf("theme not found: %s", path);
        free(path);
        return NULL;
    }
    char *perr = NULL;
    json_t *j = json_parse(data, len, &perr);
    free(data);
    if (!j) {
        if (err) *err = xasprintf("%s: %s", path, perr ? perr : "invalid JSON");
        free(perr);
        free(path);
        return NULL;
    }
    free(path);
    t = tui_theme_from_json(j, mode, err);
    json_free(j);
    return t;
}

static tui_theme_t *g_theme;

const tui_theme_t *tui_theme(void) {
    if (!g_theme) { /* upstream defaults: theme.dark = titanium, theme.light = light */
        const char *n = tui_detect_theme_name();
        g_theme = tui_theme_builtin(str_eq(n, "dark") ? "titanium" : n, tui_detect_color_mode());
    }
    return g_theme;
}

void tui_theme_set(tui_theme_t *t) {
    if (g_theme && g_theme != t) tui_theme_free(g_theme);
    g_theme = t;
}

const char *tui_fg_seq(const tui_theme_t *th, tui_tc_t slot) {
    if (!th) th = tui_theme();
    return slot < TUI_TC__COUNT && th->fg[slot][0] ? th->fg[slot] : "\x1b[39m";
}

const char *tui_bg_seq(const tui_theme_t *th, tui_tc_t slot) {
    if (!th) th = tui_theme();
    return slot < TUI_TC__COUNT && th->bg[slot][0] ? th->bg[slot] : "\x1b[49m";
}

const char *tui_ansi_fg_seq(const tui_theme_t *th, int ansi_slot) {
    if (!th) th = tui_theme();
    return ansi_slot >= 0 && ansi_slot < 16 && th->ansi_fg[ansi_slot][0] ? th->ansi_fg[ansi_slot] : "\x1b[39m";
}

/* upstream wrapForeground: the color is restored after every nested reset (\e[0m / \e[39m) in `text` */
void tui_fg(sb_t *b, const tui_theme_t *th, tui_tc_t slot, const char *text) {
    const char *seq = tui_fg_seq(th, slot);
    sb_append(b, seq);
    const char *p = text ? text : "";
    if (strstr(p, "\x1b[")) {
        for (;;) {
            const char *r0 = strstr(p, "\x1b[0m"), *r39 = strstr(p, "\x1b[39m");
            const char *r = r0 && (!r39 || r0 < r39) ? r0 : r39;
            if (!r) break;
            size_t len = r == r0 ? 4 : 5;
            sb_appendn(b, p, (size_t)(r - p) + len);
            sb_append(b, seq);
            p = r + len;
        }
    }
    sb_append(b, p);
    sb_append(b, "\x1b[39m");
}

void tui_bg(sb_t *b, const tui_theme_t *th, tui_tc_t slot, const char *text) {
    sb_append(b, tui_bg_seq(th, slot));
    sb_append(b, text ? text : "");
    sb_append(b, "\x1b[49m");
}

char *tui_fgs(const tui_theme_t *th, tui_tc_t slot, const char *text) {
    sb_t b = SB_INIT;
    tui_fg(&b, th, slot, text);
    return sb_detach(&b);
}

char *tui_bgs(const tui_theme_t *th, tui_tc_t slot, const char *text) {
    sb_t b = SB_INIT;
    tui_bg(&b, th, slot, text);
    return sb_detach(&b);
}

static char *wrap_attr(const char *on, const char *text, const char *off) {
    return xasprintf("%s%s%s", on, text ? text : "", off);
}
char *tui_bold(const char *text) { return wrap_attr("\x1b[1m", text, "\x1b[22m"); }
char *tui_italic(const char *text) { return wrap_attr("\x1b[3m", text, "\x1b[23m"); }
char *tui_underline(const char *text) { return wrap_attr("\x1b[4m", text, "\x1b[24m"); }
char *tui_dim(const char *text) { return wrap_attr("\x1b[2m", text, "\x1b[22m"); }
char *tui_inverse(const char *text) { return wrap_attr("\x1b[7m", text, "\x1b[27m"); }
