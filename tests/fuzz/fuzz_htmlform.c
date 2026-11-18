/* fuzz_htmlform - the `ask` tool's HTML form parser and value/validation logic (src/text/htmlform.c) on model-written
 * markup. Input: <html>\n\x01\n<answer text>. */
#include "core/core.h"
#include "core/json.h"
#include "text/htmlform.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 1 << 15) return 0;
    char *html, *ans;
    size_t hl, al;
    fuzz_split(data, size, &html, &hl, &ans, &al);
    hf_form_t *f = hf_parse(html);
    if (f) {
        (void)hf_named_count(f);
        json_t *d = hf_default_data(f);
        json_t *a = hf_answers(f, d);
        json_free(a);
        char *dj = hf_data_json(d);
        free(dj);
        json_free(d);
        for (size_t i = 0; i < f->fields.n; i++) {
            hf_field_t *fld = f->fields.v[i];
            json_t *v;
            if (fld->kind == HF_CHOICE) {
                size_t n = fld->opts.n;
                bool *chk = calloc(n + 1, sizeof *chk);
                for (size_t k = 0; k < n; k++) chk[k] = al > k && (ans[k] & 1);
                v = hf_choice_value(fld, chk, al ? ans : NULL);
                free(chk);
                (void)hf_is_array_choice(fld);
            } else {
                v = hf_text_value(fld, ans);
            }
            char *e = hf_field_error(fld, v);
            free(e);
            json_free(v);
            char *q = hf_one_line(fld->question ? fld->question : "");
            free(q);
        }
        for (size_t s = 0; s < f->sections.n; s++) {
            char *t = hf_spans_text(&f->sections.v[s].title);
            free(t);
        }
        hf_form_free(f);
    }
    double num;
    (void)hf_js_number(ans, &num);
    (void)hf_utf16_len(ans);
    free(html);
    free(ans);
    return 0;
}
