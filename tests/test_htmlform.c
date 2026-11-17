/* test_htmlform.c - the ask tool's HTML form: parser, data and pagination (src/text/htmlform.c, ports of omp
 * html-form/parse.ts, data.ts, paginate.ts; cases follow packages/tui/test/html-form.test.ts). */
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

static void test_paginate(void) {
    hf_atoms_t a = {0};
    for (int i = 0; i < 6; i++) {
        hf_atom_t at = {0};
        strvec_push(&at.lines, "x");
        strvec_push(&at.lines, "y");
        at.gap = i ? 1 : 0;
        at.break_rank = i == 3 ? HF_BREAK_SECTION : HF_BREAK_INSIDE;
        vec_push(&a, at);
    }
    hf_pages_t p = {0};
    hf_paginate(&a, 10, &p);
    CHECK(p.n == 2 && p.v[0].end == 3, "pages %zu end %zu", p.n, p.n ? p.v[0].end : 0);
    free(p.v);
    hf_atoms_free(&a);
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

static json_t *g_done_data;
static bool g_done, g_done_timed_out;
static void on_done(void *ud, json_t *data, bool timed_out) {
    (void)ud;
    json_free(g_done_data);
    g_done_data = data;
    g_done = true;
    g_done_timed_out = timed_out;
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
    CHECK(strstr(s, "Ask (30s)") && strstr(s, "Paint it") && strstr(s, "\xe2\x9d\xaf \xe2\x97\x89 Red") &&
              strstr(s, "Other: type your own") && strstr(s, "\xe2\x80\xb9 eu-west-1 \xe2\x80\xba  2/2") &&
              strstr(s, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2") && !strstr(s, "abc") && strstr(s, "Submit"),
          "dialog layout:\n%s", s);
    free(s);
    /* Tab to the dropdown, Space opens it, ↑ highlights us-east-1, Enter picks */
    keys(c, "\t ");
    s = screen(c, 60);
    CHECK(strstr(s, "\xe2\x95\xad") && strstr(s, "eu-west-1 \xe2\x9c\x93") && strstr(s, "highlight"), "dropdown open:\n%s", s);
    free(s);
    keys(c, "\x1b[A\r");
    json_t *d = ui_form_dialog_data(c);
    CHECK(str_eq(json_get_str(d, "region"), "us-east-1") && str_eq(json_get_str(d, "pw"), "abc"), "dropdown pick");
    json_free(d);
    /* Alt+H hides the form to one row */
    keys(c, "\x1bh");
    s = screen(c, 100);
    CHECK(strstr(s, "Ask (30s) hidden: region (+2 more)") && strchr(s, '\n') == s + strlen(s) - 1, "hidden row:\n%s", s);
    free(s);
    keys(c, "\x1bh");
    /* the deadline submits the current values */
    ui_form_dialog_tick(c, 0);
    CHECK(g_done && g_done_timed_out && str_eq(json_get_str(g_done_data, "color"), "red") &&
              str_eq(json_get_str(g_done_data, "region"), "us-east-1"),
          "timeout submit");
    c->vt->destroy(c);

    /* a long form pages at section boundaries; the page shows in the title */
    sb_t long_html = SB_INIT;
    sb_append(&long_html, "<form>");
    for (int i = 0; i < 6; i++)
        sb_appendf(&long_html, "<fieldset><legend>Q%d</legend><label><input type=radio name=q%d value=a> A</label>"
                               "<label><input type=radio name=q%d value=b> B</label></fieldset>", i, i, i);
    sb_append(&long_html, "</form>");
    ui_form_opts_t o2 = {.term_rows = 20};
    c = ui_form_dialog_new(sb_cstr(&long_html), &o2);
    c->focused = true;
    s = screen(c, 120);
    CHECK(strstr(s, "Ask \xc2\xb7 1/") && strstr(s, "PgUp/PgDn page") == NULL && strstr(s, "\xe2\x86\x90/\xe2\x86\x92 page"),
          "paged:\n%s", s);
    free(s);
    keys(c, "\x1b[C");
    s = screen(c, 60);
    CHECK(strstr(s, "Ask \xc2\xb7 2/"), "page 2:\n%s", s);
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
    test_paginate();
    test_dialog();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
