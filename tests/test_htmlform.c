/* test_htmlform.c - the ask tool's HTML form: parser, data and the stacked dialog (src/text/htmlform.c,
 * src/ui/formdlg.c, ports of omp html-form/parse.ts, data.ts, dialog.ts; cases follow
 * packages/tui/test/html-form.test.ts). */
#include "core/core.h"
#include "core/json.h"
#include "text/htmlform.h"
#include "tui/termcaps.h"
#include "tui/ansi.h"
#include "tui/keys.h"
#include "ui/overlays.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (cond) g_pass++;                                                                                            \
        else {                                                                                                         \
            g_fail++;                                                                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static char *data_of(const hf_form_t *f) {
    json_t *d = hf_default_data(f);
    char *s = json_dump(d, 0);
    json_free(d);
    return s;
}

static void test_parse(void) {
    hf_form_t *f = hf_parse("<p>outside</p><form><fieldset><legend>Database</legend>"
                            "<label><input type=\"radio\" name=\"db\" value=\"sqlite\" checked> SQLite</label>"
                            "<label><input type=\"radio\" name=\"db\" value=\"pg\"> Postgres</label>"
                            "</fieldset><p>Notes &amp; more</p><textarea name=\"notes\">\nline</textarea></form>");
    CHECK(f->sections.n == 2, "sections %zu", f->sections.n);
    char *t = hf_spans_text(&f->sections.v[0].title);
    CHECK(!strcmp(t, "Database"), "title %s", t);
    free(t);
    CHECK(f->fields.n == 2, "fields %zu", f->fields.n);
    hf_field_t *db = f->fields.v[0];
    CHECK(db->kind == HF_CHOICE && !db->multi && db->opts.n == 2 && db->other, "db group");
    CHECK(!strcmp(db->question, "Database"), "db question %s", db->question);
    hf_field_t *notes = f->fields.v[1];
    CHECK(notes->multiline && !strcmp(notes->value, "line") && !strcmp(notes->question, "notes"), "notes %s / %s",
          notes->value, notes->question);
    char *d = data_of(f);
    CHECK(!strcmp(d, "{\"db\":\"sqlite\",\"notes\":\"line\"}"), "data %s", d);
    free(d);
    hf_form_free(f);

    /* a <p> right before a group is its question; an own "Other" option suppresses the added one */
    f = hf_parse("<form><p>Which  <b>store</b>?</p><label><input type=checkbox name=s value=a>A</label>"
                 "<label><input type=checkbox name=s value=o>Other: tell</label>"
                 "<input type=number name=n value=\" 30 \" min=1 max=5><input type=hidden name=h value=x>"
                 "<select name=r><option disabled>no<option value=e>East<option>West</select></form>");
    CHECK(f->fields.n == 3, "fields %zu", f->fields.n);
    CHECK(!strcmp(f->fields.v[0]->question, "Which store?") && !f->fields.v[0]->other, "q %s", f->fields.v[0]->question);
    d = data_of(f);
    CHECK(!strcmp(d, "{\"h\":\"x\",\"s\":[],\"n\":30,\"r\":\"e\"}"), "data %s", d);
    free(d);
    json_t *v = json_str("9");
    char *e = hf_field_error(f->fields.v[1], v);
    CHECK(e && !strcmp(e, "Must be at most 5"), "err %s", e ? e : "(null)");
    free(e);
    json_free(v);
    hf_form_free(f);

    /* truncated markup (a streaming call) parses what is complete */
    f = hf_parse("<form><label><input type=\"radio\" name=\"a\" value=\"1\"> One</label><label><input type=\"ra");
    CHECK(f->fields.n == 1 && f->fields.v[0]->opts.n == 1, "partial %zu", f->fields.n);
    hf_form_free(f);
}

static void test_answers(void) {
    hf_form_t *f = hf_parse("<form><label><input type=radio name=c value=r>Red</label>"
                            "<label><input type=radio name=c value=b>Blue</label>"
                            "<label>Email <input type=email name=m required></label></form>");
    json_t *data = json_parse_str("{\"c\":\"teal\",\"m\":\"x@y\"}");
    json_t *a = hf_answers(f, data);
    char *s = json_dump(a, 0);
    CHECK(!strcmp(s, "[{\"name\":\"c\",\"question\":\"c\",\"answer\":\"Other: teal\"},"
                     "{\"name\":\"m\",\"question\":\"Email\",\"answer\":\"x@y\"}]"),
          "answers %s", s);
    free(s);
    char *pj = hf_data_json(data);
    CHECK(!strcmp(pj, "{\n  \"c\": \"teal\",\n  \"m\": \"x@y\"\n}"), "json %s", pj);
    free(pj);
    json_t *bad = json_str("nope");
    char *e = hf_field_error(f->fields.v[1], bad);
    CHECK(e && !strcmp(e, "Enter an email address"), "email %s", e ? e : "(null)");
    free(e);
    json_free(bad);
    json_free(a);
    json_free(data);
    hf_form_free(f);
}

/* fieldContexts / formContext (omp bfffc6ffb2, 040ee9d021, 5674262fe2) */
static void test_contexts(void) {
    hf_form_t *f = hf_parse("<form>\n  <p>Both jobs read the same base image.</p>\n"
                            "  <ul><li>build: needs the toolchain</li><li>test: only the runtime</li></ul>\n"
                            "  <pre>FROM debian:12</pre>\n"
                            "  <table><tr><th>Job</th><th>Time</th></tr><tr><td>build</td><td>4m</td></tr></table>\n"
                            "  <p>How should these two jobs use the image?</p>\n"
                            "  <label><input type=\"radio\" name=\"mode\" value=\"share\" checked> Share it</label>\n"
                            "  <label><input type=\"radio\" name=\"mode\" value=\"split\"> Split it</label>\n"
                            "  <fieldset><legend>Caching</legend>\n    <p>Layers are cached per runner.</p>\n"
                            "    <label>Cache size (GB) <input type=\"number\" name=\"cache\"></label>\n  </fieldset>\n"
                            "  <p>Both answers can change later.</p>\n</form>");
    char **c = hf_field_contexts(f);
    CHECK(f->fields.n == 2 && str_eq(c[0], "Both jobs read the same base image.\n\n- build: needs the toolchain\n- test: only "
                                           "the runtime\n\nFROM debian:12\n\nJob | Time\nbuild | 4m"),
          "context 0: %s", c[0]);
    CHECK(str_eq(c[1], "Caching\n\nLayers are cached per runner.\n\nBoth answers can change later."), "context 1: %s", c[1]);
    for (size_t i = 0; i < f->fields.n; i++) free(c[i]);
    free(c);
    strvec_t parts = hf_form_context(f);
    CHECK(parts.n == 7 && str_eq(parts.v[3], "Job | Time\nbuild | 4m") && str_eq(parts.v[4], "Caching") &&
              str_eq(parts.v[6], "Both answers can change later."),
          "form context %zu", parts.n);
    strvec_free(&parts);
    hf_form_free(f);
    /* text before a field that sits between another group's options stays with that field */
    f = hf_parse("<form>\n  <label><input type=\"radio\" name=\"plan\" value=\"basic\" checked> Basic</label>\n"
                 "  <p>Choose a compliant region</p>\n"
                 "  <label>Region <select name=\"region\"><option value=\"us\">US</option><option value=\"eu\">EU</option>"
                 "</select></label>\n  <label><input type=\"radio\" name=\"plan\" value=\"premium\"> Premium</label>\n</form>");
    c = hf_field_contexts(f);
    CHECK(f->fields.n == 2 && str_eq(c[0], "") && str_eq(c[1], "Choose a compliant region"), "intervening: [%s] [%s]", c[0],
          f->fields.n > 1 ? c[1] : "");
    for (size_t i = 0; i < f->fields.n; i++) free(c[i]);
    free(c);
    hf_form_free(f);
    /* text between a group's own options stays on the group */
    f = hf_parse("<form><label><input type=radio name=a value=x> X</label><p>between</p>"
                 "<label><input type=radio name=a value=y> Y</label><p>tail</p>"
                 "<label>Name <input name=n></label></form>");
    c = hf_field_contexts(f);
    CHECK(f->fields.n == 2 && str_eq(c[0], "between") && str_eq(c[1], "tail"), "between: [%s] [%s]", c[0],
          f->fields.n > 1 ? c[1] : "");
    for (size_t i = 0; i < f->fields.n; i++) free(c[i]);
    free(c);
    hf_form_free(f);
}

/* ---------- HtmlFormDialog (src/ui/formdlg.c) ---------- */

static void keys(tui_component_t *c, const char *raw) {
    tui_key_t ks[32];
    size_t n = tui_keys_decode_all(raw, strlen(raw), ks, 32);
    for (size_t i = 0; i < n; i++) {
        c->vt->handle_key(c, &ks[i]);
        tui_key_free(&ks[i]);
    }
}

/* keyHints, plain */
static char *hints(tui_component_t *c) {
    char *h = ui_form_dialog_key_hints(c), *p = tui_strip_ansi(h);
    free(h);
    return p;
}

static char *screen(tui_component_t *c, int width) {
    strvec_t out = {0};
    c->vt->render(c, width, &out);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < out.n; i++) {
        char *s = tui_strip_ansi(out.v[i]);
        sb_appendf(&b, "%s\n", s);
        free(s);
    }
    strvec_free(&out);
    return sb_detach(&b);
}

static json_t *g_done_data, *g_done_notes;
static bool g_done, g_done_timed_out;
static void on_done(void *ud, json_t *data, json_t *notes, bool timed_out) {
    json_free(g_done_notes);
    g_done_notes = notes;
    (void)ud;
    json_free(g_done_data);
    g_done_data = data;
    g_done = true;
    g_done_timed_out = timed_out;
}

/* per-option notes (omp 54f3265997, 593be268c3): `n` opens a note under the option, Enter saves it, the row shows
 * `✎ note` (chosen) or `✎ draft`; only notes on chosen options are submitted */
static void test_dialog_notes(void) {
    const char *html = "<form><fieldset><legend>Pick a color</legend>"
                       "<label><input type=radio name=color value=red checked> Red</label>"
                       "<label><input type=radio name=color value=blue> Blue</label></fieldset>"
                       "<select name=region><option>us</option><option selected>eu</option></select></form>";
    ui_form_opts_t o = {.term_rows = 40, .done = on_done};
    tui_component_t *c = ui_form_dialog_new(html, &o);
    c->focused = true;
    char *s = hints(c);
    CHECK(strstr(s, "\xe2\x8f\x8e choose \xc2\xb7 n note \xc2\xb7"), "note hint:\n%s", s);
    free(s);
    keys(c, "nwarm  tone");
    s = screen(c, 80);
    CHECK(strstr(s, "Note for Red \xc2\xb7 enter save \xc2\xb7 blank removes \xc2\xb7 esc keep") && strstr(s, "warm  tone"),
          "note editor:\n%s", s);
    free(s);
    keys(c, "\r");
    s = screen(c, 80);
    CHECK(strstr(s, "Red  \xe2\x9c\x8e note"), "saved note:\n%s", s);
    free(s);
    keys(c, "\x1b[Bnx\r");
    s = screen(c, 80);
    CHECK(strstr(s, "Blue  \xe2\x9c\x8e draft"), "draft note:\n%s", s);
    free(s);
    /* on a dropdown the note key is Alt+N: `n` stays type-ahead */
    keys(c, "\t");
    s = hints(c);
    CHECK(strstr(s, "change \xc2\xb7 \xe2\x8f\x8e next \xc2\xb7 Alt+N note"),
          "dropdown note hint:\n%s", s);
    free(s);
    keys(c, "\x1bnfar\r");
    s = screen(c, 80);
    CHECK(strstr(s, "2/2  \xe2\x9c\x8e note"), "dropdown note:\n%s", s);
    free(s);
    keys(c, "\t\r");
    CHECK(g_done && g_done_notes && json_len(g_done_notes) == 2, "submitted notes");
    const json_t *n0 = json_at(g_done_notes, 0);
    CHECK(str_eq(json_get_str(n0, "name"), "color") && str_eq(json_get_str(n0, "question"), "Pick a color") &&
              str_eq(json_get_str(n0, "option"), "Red") && str_eq(json_get_str(n0, "value"), "red") &&
              str_eq(json_get_str(n0, "text"), "warm  tone"),
          "note fields");
    CHECK(str_eq(json_get_str(json_at(g_done_notes, 1), "value"), "eu"), "dropdown note value");
    c->vt->destroy(c);
    json_free(g_done_notes);
    g_done_notes = NULL;
    json_free(g_done_data);
    g_done_data = NULL;
    g_done = false;
}

/* a one-question form submits on Enter, opening on its preselected answer (omp 05c0e53513, 6b213b6d3b) */
static void test_dialog_sole(void) {
    const char *html = "<form><fieldset><legend>Size</legend>"
                       "<label><input type=radio name=size value=s> S</label>"
                       "<label><input type=radio name=size value=m checked> M</label></fieldset></form>";
    ui_form_opts_t o = {.term_rows = 40, .done = on_done};
    tui_component_t *c = ui_form_dialog_new(html, &o);
    c->focused = true;
    char *s = screen(c, 80), *h = hints(c);
    CHECK(strstr(s, "\xe2\x9d\xaf \xe2\x97\x89 M") && strstr(h, "\xe2\x8f\x8e submit"), "sole layout:\n%s%s", s, h);
    free(s);
    free(h);
    keys(c, "\r");
    CHECK(g_done && str_eq(json_get_str(g_done_data, "size"), "m"), "Enter submits the preselected answer");
    c->vt->destroy(c);
    json_free(g_done_notes);
    g_done_notes = NULL;
    json_free(g_done_data);
    g_done_data = NULL;
    g_done = false;
}

/* table cells word-wrap within their fitted widths instead of truncating, keeping <br> breaks (omp e701ef8cbe) */
static void test_dialog_table_wrap(void) {
    const char *html = "<form><table><tr><th>Step</th><th>Notes</th></tr>"
                       "<tr><td>build</td><td>compiles every module of the project in parallel then links them</td></tr>"
                       "<tr><td>a<br>b</td><td>x</td></tr></table>"
                       "<input name=ok value=y></form>";
    ui_form_opts_t o = {.term_rows = 40, .done = on_done};
    tui_component_t *c = ui_form_dialog_new(html, &o);
    char *s = screen(c, 40);
    CHECK(strstr(s, "compiles every") && strstr(s, "links them"), "wrapped cells:\n%s", s);
    const char *a = strstr(s, "a  "), *b = a ? strstr(a, "\n") : NULL;
    CHECK(a && b && strstr(b, "b"), "br break kept:\n%s", s);
    free(s);
    c->vt->destroy(c);
}

static void test_dialog(void) {
    const char *html = "<form><p>Paint it</p><fieldset><legend>Pick a color</legend>"
                       "<label><input type=radio name=color value=red checked> Red</label>"
                       "<label><input type=radio name=color value=blue> Blue</label></fieldset>"
                       "<select name=region><option>us-east-1</option><option selected>eu-west-1</option></select>"
                       "<label>Secret <input type=password name=pw value=abc></label></form>";
    ui_form_opts_t o = {.term_rows = 40, .done = on_done, .timeout_ms = 30000};
    tui_component_t *c = ui_form_dialog_new(html, &o);
    c->focused = true;
    char *s = screen(c, 60);
    CHECK(strstr(s, "Ask \xc2\xb7 3 of 3 answered (30s)") && strstr(s, "Paint it") && strstr(s, "\xe2\x9d\xaf \xe2\x97\x89 Red") &&
              strstr(s, "Other: type your own") && strstr(s, "\xe2\x9c\x93 region") && strstr(s, "\xe2\x9c\x93 eu-west-1 ") &&
              strstr(s, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2") && !strstr(s, "abc") && strstr(s, "Submit"),
          "dialog layout:\n%s", s);
    free(s);
    /* Tab to the dropdown, Space opens it, ↑ highlights us-east-1, Enter picks */
    keys(c, "\t ");
    s = screen(c, 60);
    char *h = hints(c);
    CHECK(strstr(s, "\xe2\x95\xad") && strstr(s, "eu-west-1 \xe2\x9c\x93") && strstr(h, "highlight"), "dropdown open:\n%s%s", s, h);
    free(s);
    free(h);
    keys(c, "\x1b[A\r");
    json_t *d = ui_form_dialog_data(c);
    CHECK(str_eq(json_get_str(d, "region"), "us-east-1") && str_eq(json_get_str(d, "pw"), "abc"), "dropdown pick");
    json_free(d);
    /* Alt+H hides the form to one row */
    keys(c, "\x1bh");
    s = screen(c, 100);
    CHECK(strstr(s, "Ask \xc2\xb7 3 of 3 answered (30s) hidden: region (+2 more)") && strstr(s, "composer") &&
              strchr(s, '\n') == s + strlen(s) - 1,
          "hidden row:\n%s", s);
    free(s);
    keys(c, "\x1bh");
    /* the deadline submits the current values */
    ui_form_dialog_tick(c, 0);
    CHECK(g_done && g_done_timed_out && str_eq(json_get_str(g_done_data, "color"), "red") &&
              str_eq(json_get_str(g_done_data, "region"), "us-east-1"),
          "timeout submit");
    c->vt->destroy(c);

    /* every question stacks in one scrolling panel: the focused one open, the rest folded; ←/→ open the next or
     * previous question; the panel keeps a fixed height that leaves the composer its reserved rows */
    sb_t long_html = SB_INIT;
    sb_append(&long_html, "<form>");
    for (int i = 0; i < 6; i++)
        sb_appendf(&long_html, "<fieldset><legend>Q%d</legend><label><input type=radio name=q%d value=a> A</label>"
                               "<label><input type=radio name=q%d value=b> B</label></fieldset>", i, i, i);
    sb_append(&long_html, "</form>");
    ui_form_opts_t o2 = {.term_rows = 20};
    c = ui_form_dialog_new(sb_cstr(&long_html), &o2);
    c->focused = true;
    s = screen(c, 60);
    size_t height = 0;
    for (const char *p = s; *p; p++) height += *p == '\n';
    CHECK(strstr(s, "Ask \xc2\xb7 0 of 6 answered") && strstr(s, "\xe2\x9d\xaf \xe2\x97\x8b A") &&
              strstr(s, "\xe2\x97\x8b Q1") && height <= 20 - 8 && strstr(s, "\xe2\x96\x88"),
          "stacked:\n%s", s);
    free(s);
    h = hints(c);
    CHECK(strstr(h, "\xe2\x86\x90/\xe2\x86\x92 question") && strstr(h, " composer"), "question hints: %s", h);
    free(h);
    keys(c, "\x1b[C\x1b[C\x1b[C\x1b[C\x1b[C");
    s = screen(c, 60);
    size_t height5 = 0;
    for (const char *p = s; *p; p++) height5 += *p == '\n';
    CHECK(strstr(s, "\xe2\x94\x82 Q5 ") && strstr(s, "\xe2\x9d\xaf \xe2\x97\x8b A") && height5 == height, "Q5 open:\n%s", s);
    free(s);
    keys(c, " \x1b[C"); /* Space picks Q5's A, → past the last question: Submit, every question folded */
    s = screen(c, 60);
    CHECK(strstr(s, "\xe2\x9d\xaf Submit") && strstr(s, "\xe2\x9c\x93 Q5") && strstr(s, "1 of 6 answered"), "submit:\n%s", s);
    free(s);
    c->vt->destroy(c);
    sb_free(&long_html);

    /* validation on Submit */
    ui_form_opts_t o3 = {.term_rows = 40};
    c = ui_form_dialog_new("<form><label>Email <input type=email name=e value=nope required></label></form>", &o3);
    c->focused = true;
    keys(c, "\t\r");
    s = screen(c, 60);
    CHECK(strstr(s, "Enter an email address") && strstr(s, "1 to fix"), "validation:\n%s", s);
    free(s);
    c->vt->destroy(c);

    /* pictures: a data: URI PNG draws (text fallback without a graphics protocol); a missing file says why */
    static const char PNG[] = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
    char *ih = xasprintf("<form><img src=\"data:image/png;base64,%s\" alt=\"dot\"><img src=\"missing.png\" alt=\"gone\">"
                         "<label><input type=checkbox name=ok> OK</label></form>",
                         PNG);
    ui_form_opts_t o4 = {.term_rows = 40, .cwd = "/nonexistent-omc-dir"};
    c = ui_form_dialog_new(ih, &o4);
    s = screen(c, 70);
    CHECK(strstr(s, "[Image: dot") && strstr(s, "\xe2\x96\xa3 gone \xe2\x80\x94 couldn't load: ENOENT"), "pictures:\n%s", s);
    free(s);
    c->vt->destroy(c);
    /* on Kitty a picture is an id'd placement that repainting never removes: it is deleted when the form closes
     * (omp 27ed6a1307, 98162c0960) */
    tui_set_image_protocol(TUI_IMG_KITTY);
    c = ui_form_dialog_new(ih, &o4);
    s = screen(c, 70);
    free(s);
    free(tui_image_take_pending());
    c->vt->destroy(c);
    char *pend = tui_image_take_pending();
    CHECK(pend && strstr(pend, "\x1b_Ga=d,d=I,i="), "release: %s", pend ? pend : "(none)");
    free(pend);
    tui_set_image_protocol(TUI_IMG_NONE);
    free(ih);
    json_free(g_done_data);
    g_done_data = NULL;
}

static int g_released;
static void on_release(void *ud) {
    (void)ud;
    g_released++;
}
static const char *g_skip;
static char *on_skip(void *ud) {
    (void)ud;
    return g_skip ? xstrdup(g_skip) : NULL;
}

/* the redesigned ask (omp 093f499464, 708fccb73c): folded questions over their answer or option chips, Esc and Tab
 * past Submit hand the keys to the composer, the skip notice takes the body's last row, givenAnswers */
static void test_dialog_stacked(void) {
    const char *html = "<form><label>Owner <input name=owner placeholder=\"who ships it\"></label><p>Rollout?</p>"
                       "<label><input type=radio name=when value=v0> Behind a feature flag for a week</label>"
                       "<label><input type=radio name=when value=v1> Default on</label>"
                       "<label><input type=radio name=when value=v2> Next minor</label>"
                       "<label><input type=radio name=when value=v3> After a soak</label>"
                       "<label><input type=radio name=when value=v4> Never</label>"
                       "<label><input type=radio name=when value=v5> Ask me</label>"
                       "<fieldset><legend>Targets</legend><label><input type=checkbox name=t value=linux checked> Linux</label>"
                       "<label><input type=checkbox name=t value=mac> macOS</label></fieldset></form>";
    ui_form_opts_t o = {.term_rows = 40, .done = on_done, .release_keys = on_release, .skip_notice = on_skip};
    g_done = false;
    tui_component_t *c = ui_form_dialog_new(html, &o);
    c->focused = true;
    char *s = screen(c, 80);
    /* the first four options as chips, the first cut to twenty cells, then how many more; a preselected answer stays
     * among its options */
    CHECK(strstr(s, "\xe2\x97\x8b Rollout?") &&
              strstr(s, " Behind a feature fl\xe2\x80\xa6   Default on   Next minor   After a soak   +2 ") &&
              strstr(s, "\xe2\x9c\x93 Targets") && strstr(s, " \xe2\x9c\x93 Linux   macOS ") &&
              strstr(s, "Ask \xc2\xb7 1 of 3 answered") && !strstr(s, "composer"),
          "folded:\n%s", s);
    size_t height = 0;
    for (const char *p = s; *p; p++) height += *p == '\n';
    free(s);
    /* the skip notice is the panel's last row, the panel keeping its height */
    g_skip = "Sending your message skips these questions";
    s = screen(c, 80);
    size_t h2 = 0;
    for (const char *p = s; *p; p++) h2 += *p == '\n';
    const char *warn = strstr(s, "\xe2\x9a\xa0 Sending your message skips these questions");
    CHECK(warn && h2 == height && strstr(warn, "\n\xe2\x95\xb0") == strchr(warn, '\n'), "skip notice:\n%s", s);
    free(s);
    g_skip = NULL;
    /* Esc hands the keys to the composer; the form stays */
    keys(c, "\x1b");
    CHECK(g_released == 1 && !g_done, "Esc releases the keys");
    keys(c, "ada\t\x1b[B"); /* typed owner, Tab to Rollout, ↓ to Default on */
    keys(c, " ");
    json_t *given = ui_form_dialog_given(c);
    CHECK(given && str_eq(json_get_str(json_get(given, "data"), "owner"), "ada") &&
              str_eq(json_get_str(json_get(given, "data"), "when"), "v1") && !json_get(json_get(given, "data"), "t"),
          "given answers: %s", given ? json_dump(given, 0) : "(none)");
    json_free(given);
    s = screen(c, 80);
    CHECK(strstr(s, "\xe2\x9c\x93 Owner ") && strstr(s, "\xe2\x94\x82   ada "), "answered fold:\n%s", s);
    free(s);
    /* Tab past Submit hands the keys over too */
    keys(c, "\t\t");
    s = screen(c, 80);
    CHECK(strstr(s, "\xe2\x9d\xaf Submit") && strstr(s, "3 of 3 answered"), "on submit:\n%s", s);
    free(s);
    keys(c, "\t");
    CHECK(g_released == 2 && !g_done, "Tab past Submit releases the keys");
    c->vt->destroy(c);
    CHECK(g_done && !g_done_data, "torn down: cancelled");
    g_done = false;
    /* an untouched form has given nothing */
    c = ui_form_dialog_new(html, &o);
    CHECK(!ui_form_dialog_given(c), "nothing given");
    c->vt->destroy(c);
    g_done = false;
}

int main(void) {
    const char *td = getenv("TMPDIR");
    char *tmpl = xasprintf("%s/omc-htmlform-XXXXXX", td && *td ? td : "/tmp");
    char *dir = mkdtemp(tmpl);
    if (dir) {
        setenv("HOME", dir, 1);
        setenv("OMC_CODING_AGENT_DIR", dir, 1);
    }
    test_parse();
    test_answers();
    test_contexts();
    test_dialog();
    test_dialog_notes();
    test_dialog_sole();
    test_dialog_table_wrap();
    test_dialog_stacked();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
