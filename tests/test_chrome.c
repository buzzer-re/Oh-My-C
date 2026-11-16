/* test_chrome.c - screen chrome regression checks against captured omp output.
 *
 * The fixtures under tests/fixtures/chrome/ are `tmux capture-pane -e` captures of omp v18.17.0
 * (tests/tui/compare.sh, 120 columns, titanium theme, truecolor, unicode glyphs, mock model
 * "Mock One" with a 100K window). Each test renders the same rows through omc's welcome,
 * working/idle row, editor top gap and band composer, then compares cell by cell: glyph,
 * foreground, background and attributes. The wordmark rows only compare the logo cells (omc
 * draws `omc` and its own version where omp draws `omp v18.17.0`). */
#include "coding/app.h"
#include "config/settings.h"
#include "core/fs.h"
#include "session/session.h"
#include "tui/color.h"
#include "ui/ui.h"

#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* ---------- SGR cell model ---------- */

typedef struct {
    char ch[16];
    char fg[24], bg[24];
    int attr; /* 1 bold 2 dim 4 italic 8 underline 16 inverse */
} cell_t;

typedef VEC(cell_t) cells_t;

static void apply_sgr(const char *params, cell_t *st) {
    int v[32], n = 0;
    const char *p = params;
    while (*p && n < 32) {
        v[n++] = atoi(p);
        while (*p && *p != ';' && *p != ':') p++;
        if (*p) p++;
    }
    if (!n) v[n++] = 0;
    for (int i = 0; i < n; i++) {
        int c = v[i];
        if (c == 0) st->fg[0] = st->bg[0] = 0, st->attr = 0;
        else if (c == 1) st->attr |= 1;
        else if (c == 2) st->attr |= 2;
        else if (c == 3) st->attr |= 4;
        else if (c == 4) st->attr |= 8;
        else if (c == 7) st->attr |= 16;
        else if (c == 22) st->attr &= ~3;
        else if (c == 23) st->attr &= ~4;
        else if (c == 24) st->attr &= ~8;
        else if (c == 27) st->attr &= ~16;
        else if (c == 39) st->fg[0] = 0;
        else if (c == 49) st->bg[0] = 0;
        else if ((c == 38 || c == 48) && i + 1 < n) {
            char *dst = c == 38 ? st->fg : st->bg;
            if (v[i + 1] == 2 && i + 4 < n) snprintf(dst, 24, "%d,%d,%d", v[i + 2], v[i + 3], v[i + 4]), i += 4;
            else if (v[i + 1] == 5 && i + 2 < n) snprintf(dst, 24, "p%d", v[i + 2]), i += 2;
        } else if ((c >= 30 && c <= 37) || (c >= 90 && c <= 97)) snprintf(st->fg, 24, "b%d", c);
        else if ((c >= 40 && c <= 47) || (c >= 100 && c <= 107)) snprintf(st->bg, 24, "b%d", c);
    }
}

static void to_cells(const char *s, cells_t *out) {
    cell_t st = {0};
    size_t len = strlen(s), i = 0;
    while (i < len) {
        if (s[i] == 0x1b) {
            if (s[i + 1] == '[') {
                size_t j = i + 2;
                while (j < len && !(s[j] >= 0x40 && s[j] <= 0x7e)) j++;
                if (j < len && s[j] == 'm') {
                    char *params = xstrndup(s + i + 2, j - i - 2);
                    apply_sgr(params, &st);
                    free(params);
                }
                i = j + 1;
            } else { /* OSC / APC: skip to BEL or ST */
                size_t j = i + 2;
                while (j < len && s[j] != 0x07 && !(s[j] == 0x1b && s[j + 1] == '\\')) j++;
                i = j < len && s[j] == 0x07 ? j + 1 : j + 2;
            }
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(s + i, len - i, &cp);
        int w = tui_cp_width(cp);
        if (w == 0 && out->n) { /* combining / variation selector: joins the previous cell */
            cell_t *prev = &out->v[out->n - 1];
            size_t pl = strlen(prev->ch);
            if (pl + (size_t)k < sizeof prev->ch) memcpy(prev->ch + pl, s + i, (size_t)k), prev->ch[pl + k] = 0;
        } else if (w > 0) {
            cell_t c = st;
            memcpy(c.ch, s + i, (size_t)k);
            c.ch[k] = 0;
            vec_push(out, c);
            for (int x = 1; x < w; x++) {
                cell_t pad = st;
                strcpy(pad.ch, "");
                vec_push(out, pad);
            }
        }
        i += (size_t)k;
    }
    /* trailing blanks with no background are not drawn */
    while (out->n && !strcmp(out->v[out->n - 1].ch, " ") && !out->v[out->n - 1].bg[0] && !(out->v[out->n - 1].attr & 24)) out->n--;
}

static bool cell_eq(const cell_t *a, const cell_t *b) {
    if (strcmp(a->ch, b->ch)) return false;
    if (strcmp(a->bg, b->bg) || (a->attr & 24) != (b->attr & 24)) return false;
    if (!strcmp(a->ch, " ") || !a->ch[0]) return true; /* a blank's foreground is invisible */
    return !strcmp(a->fg, b->fg) && a->attr == b->attr;
}

/* compare row `got` against `want`, the first `limit` cells (-1 = all) */
static void compare_row(const char *name, int row, const char *want, const char *got, int limit) {
    cells_t a = {0}, b = {0};
    to_cells(want, &a);
    to_cells(got, &b);
    size_t na = limit >= 0 ? MIN(a.n, (size_t)limit) : a.n, nb = limit >= 0 ? MIN(b.n, (size_t)limit) : b.n;
    if (limit >= 0) { /* a cut row may end inside trailing blanks on one side: pad with blanks */
        cell_t blank = {.ch = " "};
        while (a.n < (size_t)limit && na < nb) {
            vec_push(&a, blank);
            na++;
        }
        while (b.n < (size_t)limit && nb < na) {
            vec_push(&b, blank);
            nb++;
        }
    }
    if (na != nb) {
        char *pw = tui_strip_ansi(want), *pg = tui_strip_ansi(got);
        CHECK(false, "%s row %d: %zu cells, want %zu\n  want |%s|\n  got  |%s|", name, row, nb, na, pw, pg);
        free(pw), free(pg);
    } else {
        for (size_t i = 0; i < na; i++) {
            if (!cell_eq(&a.v[i], &b.v[i])) {
                char *pg = tui_strip_ansi(got);
                CHECK(false, "%s row %d col %zu: '%s' fg=%s bg=%s attr=%d, want '%s' fg=%s bg=%s attr=%d\n  got |%s|", name, row, i,
                      b.v[i].ch, b.v[i].fg, b.v[i].bg, b.v[i].attr, a.v[i].ch, a.v[i].fg, a.v[i].bg, a.v[i].attr, pg);
                free(pg);
                break;
            }
        }
    }
    vec_free(&a);
    vec_free(&b);
}

static void load_fixture(const char *name, strvec_t *lines) {
    char *path = xasprintf("tests/fixtures/chrome/%s", name);
    char *d = fs_read(path, NULL);
    if (!d) {
        fprintf(stderr, "missing fixture %s\n", path);
        exit(1);
    }
    for (char *p = d, *nl; *p; p = nl + 1) {
        nl = strchr(p, '\n');
        if (!nl) {
            strvec_push(lines, p);
            break;
        }
        strvec_push_own(lines, xstrndup(p, (size_t)(nl - p)));
    }
    while (lines->n && !*lines->v[lines->n - 1]) free(lines->v[--lines->n]); /* tmux's blank screen rows */
    free(d);
    free(path);
}

static void strip_marker(strvec_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        char *r = str_replace(v->v[i], TUI_CURSOR_MARKER, "");
        free(v->v[i]);
        v->v[i] = r;
    }
}

/* ---------- fixture world ---------- */

static ai_model_t g_model;

static ui_t *make_ui(const char *cwd) {
    app_t *a = xcalloc(1, sizeof *a);
    a->agent = agent_new();
    a->session = session_in_memory(cwd);
    a->cwd = xstrdup(cwd);
    g_model.provider = "mock";
    g_model.id = "mock-1";
    g_model.name = "Mock One";
    g_model.context_window = 100000;
    a->model = &g_model;
    a->thinking = EFFORT_MEDIUM;
    ui_t *u = xcalloc(1, sizeof *u);
    g_ui = u;
    u->app = a;
    int pfd[2];
    if (pipe(pfd) != 0) exit(1);
    u->tui = tui_new(tui_term_new(-1, pfd[1]));
    u->editor = tui_editor_new();
    ui_chrome_setup(u);
    tui_set_focus(u->tui, &u->editor->base);
    return u;
}

/* header + working/idle row + gap + composer, as run_interactive_mode lays them out */
static void render_screen(ui_t *u, tui_component_t *welcome, int width, strvec_t *out) {
    if (welcome) {
        strvec_push(out, "");
        tui_component_render(welcome, width, out);
        strvec_push(out, "");
    }
    tui_loader_t *loader = tui_loader_new(u->tui, "Working\xe2\x80\xa6");
    loader->base.hidden = true;
    tui_component_t *row = ui_status_row_new(u, &loader->base);
    tui_component_t *gap = ui_editor_gap_new(u);
    tui_component_render(row, width, out);
    tui_component_render(gap, width, out);
    tui_component_render(&u->editor->base, width, out);
    strip_marker(out);
    tui_component_destroy(row);
    tui_component_destroy(gap);
}

static void test_band_welcome(ui_t *u) {
    strvec_t want = {0}, got = {0};
    load_fixture("band120.omp.ansi", &want);
    tui_component_t *w = ui_welcome_new();
    ui_welcome_set_tip(w, "Type `/` at the start of your prompt to browse every command");
    /* a fresh session: system prompt + tools estimated at 4.1 chars/token; 7600 tokens is omp's count of its
     * default request (8%) */
    char *sys = xmalloc(31161);
    memset(sys, 'x', 31160);
    sys[31160] = 0;
    agent_set_system_prompt(u->app->agent, sys);
    free(sys);
    render_screen(u, w, 120, &got);
    CHECK(got.n == want.n, "band120: %zu rows, want %zu", got.n, want.n);
    for (size_t i = 0; i < MIN(got.n, want.n); i++)
        compare_row("band120", (int)i, want.v[i], got.v[i], i >= 2 && i <= 5 ? 56 /* logo, not the wordmark */ : -1);
    /* the wordmark keeps omp's half-block style and bold text color */
    CHECK(strstr(got.v[2], "\x1b[1m") && strstr(got.v[2], "\xe2\x96\x84\xe2\x96\x80\xe2\x96\x80\xe2\x96\x84"), "wordmark row");
    tui_component_destroy(w);
    strvec_free(&want);
    strvec_free(&got);
}

static void test_titled(ui_t *u) {
    strvec_t want = {0}, got = {0};
    load_fixture("titled120.omp.ansi", &want);
    free(u->app->session->title);
    u->app->session->title = xstrdup("\xf0\x9f\xa7\xaa PARK: Fix flaky park tests");
    json_push(u->app->agent->messages,
              json_parse_str("{\"role\":\"assistant\",\"stopReason\":\"stop\",\"timestamp\":2,\"content\":[],"
                             "\"usage\":{\"input\":100,\"output\":20,\"cacheRead\":0,\"cacheWrite\":0}}"));
    json_push(u->app->agent->messages, json_parse_str("{\"role\":\"user\",\"content\":\"x\"}"));
    render_screen(u, NULL, 120, &got);
    CHECK(got.n == want.n, "titled120: %zu rows, want %zu", got.n, want.n);
    for (size_t i = 0; i < MIN(got.n, want.n); i++) compare_row("titled120", (int)i, want.v[i], got.v[i], -1);
    strvec_free(&want);
    strvec_free(&got);
}

static void test_pieces(void) {
    char *k = ui_key_hint("shift+tab");
    CHECK(str_eq(k, "\xe2\x87\xa7\xe2\x87\xa5"), "shift+tab hint: %s", k);
    free(k);
    k = ui_double_tap_hint("left");
    CHECK(str_eq(k, "\xe2\x86\x90\xe2\x86\x90"), "double tap: %s", k);
    free(k);
    /* session accent: same hue as omp for the same title on titanium */
    uint32_t acc = tui_session_accent("\xf0\x9f\xa7\xaa PARK: Fix flaky park tests", tui_theme());
    CHECK(acc == 0xff9400, "accent %06x", acc);
    /* tip wrapping keeps `Tip:` with the body and drops trailing blanks */
    strvec_t tl = {0};
    ui_welcome_tip_lines("Press {key:shift+tab} to cycle through reasoning effort levels", 72, 0, &tl);
    CHECK(tl.n == 1, "tip lines %zu", tl.n);
    char *plain = tl.n ? tui_strip_ansi(tl.v[0]) : xstrdup("");
    CHECK(str_eq(plain, "Tip: Press \xe2\x87\xa7\xe2\x87\xa5 to cycle through reasoning effort levels"), "tip text: %s", plain);
    free(plain);
    strvec_free(&tl);
}

/* A transcript rebuild (the auto_compaction_end of a compaction, a subagent focus switch) or a superseded retry
 * destroys tool cards that are still in the repaint ticker's list. Every registry must let go of a destroyed card:
 * the user's crashes were the ticker reading a freed card's tool name (strcmp on a dangling pointer) a tick after a
 * remote compaction rebuilt the transcript. Under ASan the timer loop below reports the use-after-free if not. */
static void run_timers_for(ui_t *u, int ms) {
    int64_t end = mono_ms() + ms;
    while (mono_ms() < end) tui_run_once(u->tui, 50);
}

static void test_rebuild_drops_live_cards(ui_t *u) {
    u->chat = ui_transcript_new(false);
    /* a running generic card (an unknown tool such as `exec`: spinner + elapsed clock) and a running bash card */
    msgview_t *exec = toolcard_start(u, "call_exec", "exec", json_parse_str("{\"cmd\":\"ls\"}"));
    hmap_put(&u->tools, "call_exec", exec);
    toolcard_run_track("call_exec", exec);
    msgview_t *bash = toolcard_start(u, "call_bash", "bash", json_parse_str("{\"command\":\"sleep 9\"}"));
    vec_push(&u->synthetic_cards, bash);
    CHECK(toolcard_tracked(exec) && toolcard_tracked(bash), "running cards are tracked");
    ui_chat_rebuild(u); /* the maintenance-end rebuild */
    CHECK(!toolcard_tracked(exec) && !toolcard_tracked(bash), "rebuilt transcript: destroyed cards still tracked");
    CHECK(!hmap_get(&u->tools, "call_exec") && u->synthetic_cards.n == 0, "call-id map / synthetic cards keep a destroyed card");
    run_timers_for(u, 1300); /* the clock ticker runs once a second */
    /* a superseded retry removes its synthetic cards (add_notice "Retrying (…)") while they still tick */
    msgview_t *a = toolcard_start(u, "call_a", "exec", json_obj()), *b = toolcard_start(u, "call_b", "shell", json_obj());
    vec_push(&u->synthetic_cards, a);
    vec_push(&u->synthetic_cards, b);
    size_t before = tui_container_count(u->chat);
    add_notice(u, "warning", "Retrying (1/3) in 2s");
    CHECK(tui_container_count(u->chat) == before - 2, "retry removed %zu of 2 synthetic cards", before - tui_container_count(u->chat));
    CHECK(!toolcard_tracked(a) && !toolcard_tracked(b), "retry: removed cards still tracked");
    run_timers_for(u, 1300);
    ui_working_stop(u);
}

int main(void) {
    char *dir = fs_mktempdir("omc-chrome");
    setenv("OMC_CODING_AGENT_DIR", dir, 1);
    setenv("TMPDIR", "/tmp", 1);
    setenv("COLORTERM", "truecolor", 1);
    const char *cwd = "/tmp/claude-501/-Users-neo-Documents-research-core-projects-ohmyc/879821bf-68dd-4be4-b028-c3f225dea888/"
                      "scratchpad/cmp-chrome/work";
    settings_init(dir, NULL);
    tui_theme_set(tui_theme_builtin("titanium", TUI_COLOR_TRUECOLOR));
    tui_sym_set_preset("unicode");
    ui_t *u = make_ui(cwd);
    test_pieces();
    test_band_welcome(u);
    test_titled(u);
    test_rebuild_drops_live_cards(u);
    fs_rm_rf(dir);
    free(dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
