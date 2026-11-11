/* test_htmlform.c - the ask tool's HTML form: parser, data and pagination (src/text/htmlform.c, ports of omp
 * html-form/parse.ts, data.ts, paginate.ts; cases follow packages/tui/test/html-form.test.ts). */
#include "core/core.h"
#include "core/json.h"
#include "text/htmlform.h"

#include <stdio.h>

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

int main(void) {
    test_parse();
    test_answers();
    test_paginate();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
