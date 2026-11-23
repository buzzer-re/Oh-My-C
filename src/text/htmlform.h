/* htmlform.h - the `ask` tool's HTML form model (port of omp packages/tui/src/html-form/parse.ts and data.ts).
 *
 * hf_parse reduces agent-written form HTML to what a terminal form draws: titled sections (one per top-level
 * `<fieldset>`, plus untitled runs of loose content between them), context blocks (paragraphs, headings, lists,
 * preformatted text, tables, pictures) and fields. Radios and checkboxes join one choice field per `type` + `name`
 * wherever they appear. The HTML is tokenized and tree-built from scratch here (htmlparser2's implied-close, void
 * and raw-text rules, as linkedom uses them); layout-only markup is dropped.
 *
 * Form data (HtmlFormData) is a json object keyed by control name: a string, a number (`number`/`range` inputs) or
 * an array of strings (checkbox groups, multi-selects). */
#ifndef OMC_TEXT_HTMLFORM_H
#define OMC_TEXT_HTMLFORM_H

#include "core/core.h"
#include "core/json.h"

/* ---------- model (parse.ts) ---------- */

typedef enum { HF_PLAIN = 0, HF_CODE, HF_STRONG, HF_EM } hf_style_t;

typedef struct {
    char *text; /* "\n" inside is a hard break (`<br>`) */
    hf_style_t style;
} hf_span_t;
typedef VEC(hf_span_t) hf_spans_t;

typedef struct {
    char *value;
    hf_spans_t label;
    bool checked, disabled;
} hf_option_t;

typedef enum { HF_CHOICE, HF_TEXT } hf_kind_t;

typedef struct hf_field {
    hf_kind_t kind;
    char *name;       /* "" for unnamed controls */
    hf_spans_t label; /* the control's own label; empty for radio/checkbox groups */
    bool required;
    char *question;   /* fieldQuestions: own label, caption before it, section title, lone option, name (one line) */
    size_t index;     /* position in hf_form_t.fields */
    /* choice: a radio group (multi false), checkbox group (multi true) or <select> (select true) */
    bool multi, select;
    bool other;       /* the UI adds a free-text "Other" answer (radio/checkbox groups of 2+ options without one) */
    VEC(hf_option_t) opts;
    /* text: an <input> or a <textarea> */
    char *type;       /* `<input type>` lowercased; "textarea" */
    bool multiline;
    char *value, *placeholder;
    bool has_min, has_max, has_minlength, has_maxlength;
    double min, max, minlength, maxlength;
    char *pattern;    /* NULL: none */
    double rows;      /* textarea `rows` (default 4); 1 for inputs */
} hf_field_t;

typedef enum { HF_B_TEXT, HF_B_LIST, HF_B_PRE, HF_B_TABLE, HF_B_IMAGE, HF_B_SVG, HF_B_FIELD } hf_bkind_t;

typedef VEC(hf_spans_t) hf_row_t; /* table row cells */

typedef struct {
    hf_bkind_t kind;
    hf_spans_t spans;         /* text */
    bool heading;             /* text */
    VEC(hf_spans_t) items;    /* list */
    bool ordered;             /* list */
    char *text;               /* pre */
    VEC(hf_row_t) rows;       /* table */
    size_t header_rows;       /* table */
    char *src, *alt;          /* image: src (http(s), data:, file: URL or path); image/svg: alt */
    char *markup;             /* svg: the element's source */
    hf_field_t *field;        /* field (borrowed from hf_form_t.fields) */
    size_t options_end;       /* field: a group with text between its options: the index just past the last of those
                                 blocks (0: none) */
} hf_block_t;

typedef struct {
    hf_spans_t title;
    VEC(hf_block_t) blocks;
    bool fieldset; /* a top-level `<fieldset>`, legend or not; false for loose content between fieldsets */
} hf_section_t;

typedef struct {
    VEC(hf_section_t) sections;
    VEC(hf_field_t *) fields; /* document order (owned) */
    strvec_t hidden_names, hidden_values; /* <input type="hidden"> pairs */
} hf_form_t;

/* parseHtmlForm: content outside the first `<form>` is ignored when one exists. Never fails. */
hf_form_t *hf_parse(const char *html);
void hf_form_free(hf_form_t *f);
/* spansText (owned) */
char *hf_spans_text(const hf_spans_t *s);
/* text.replace(/\s+/g, " ").trim() with JavaScript's \s (owned) */
char *hf_one_line(const char *s);
/* the number of named fields */
size_t hf_named_count(const hf_form_t *f);

/* ---------- data (data.ts) ---------- */

#define HF_OTHER_LABEL "Other"

/* isArrayChoice: a checkbox group or multi-select of 2+ options reports an array */
bool hf_is_array_choice(const hf_field_t *f);
/* choiceValue: checked[i] per option; `other` the Other text (NULL: not chosen). Owned value or NULL (nothing) */
json_t *hf_choice_value(const hf_field_t *f, const bool *checked, const char *other);
/* textValue: a number for a non-empty numeric `number`/`range` input, else the text (owned) */
json_t *hf_text_value(const hf_field_t *f, const char *text);
/* fieldError: why `value` (NULL: none) is not acceptable, or NULL (owned) */
char *hf_field_error(const hf_field_t *f, const json_t *value);
/* defaultFormData (owned object) */
json_t *hf_default_data(const hf_form_t *f);
/* blockText: plain text of a context block (paragraph, list, pre, table); NULL for fields and pictures (owned) */
char *hf_block_text(const hf_block_t *b);
/* formContext: section titles and context blocks as plain text, leaving out text that is a named field's question;
 * blank parts dropped (owned strings) */
strvec_t hf_form_context(const hf_form_t *f);
/* fieldContexts: per field index, the form text a field-by-field prompt carries (section titles and context since the
 * previous named field, text between its own options, after the last field the rest), parts joined by blank lines;
 * "" without context. Owned array of f->fields.n owned strings (NULL for unnamed fields). */
char **hf_field_contexts(const hf_form_t *f);
/* formAnswers: [{name, question, answer}] for every named field, option values shown as labels (owned array) */
json_t *hf_answers(const hf_form_t *f, const json_t *data);
/* JSON.stringify(data, null, 2) (owned) */
char *hf_data_json(const json_t *data);
/* JavaScript Number(text) for a string; false when NaN */
bool hf_js_number(const char *text, double *out);
/* String.length (UTF-16 code units) */
size_t hf_utf16_len(const char *s);

#endif
