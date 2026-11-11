/* htmlform.h - the `ask` tool's HTML form model (port of omp packages/tui/src/html-form/parse.ts, data.ts and
 * paginate.ts).
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
} hf_block_t;

typedef struct {
    hf_spans_t title;
    VEC(hf_block_t) blocks;
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
/* formAnswers: [{name, question, answer}] for every named field, option values shown as labels (owned array) */
json_t *hf_answers(const hf_form_t *f, const json_t *data);
/* JSON.stringify(data, null, 2) (owned) */
char *hf_data_json(const json_t *data);
/* JavaScript Number(text) for a string; false when NaN */
bool hf_js_number(const char *text, double *out);
/* String.length (UTF-16 code units) */
size_t hf_utf16_len(const char *s);

/* ---------- pagination (paginate.ts) ---------- */

enum { HF_BREAK_NEVER = 0, HF_BREAK_INSIDE = 1, HF_BREAK_BLOCK = 2, HF_BREAK_SECTION = 3 };

typedef struct {
    strvec_t lines;      /* owned */
    int gap;             /* blank rows before it; dropped when it starts a page */
    int break_rank;      /* how good a page break before it is */
    char *continuation;  /* title repeated atop a page starting here (`X (cont.)`), NULL none (owned) */
} hf_atom_t;
typedef VEC(hf_atom_t) hf_atoms_t;

typedef struct {
    size_t start, end;
} hf_page_t;
typedef VEC(hf_page_t) hf_pages_t;

int hf_page_height(const hf_atoms_t *a, size_t start, size_t end);
/* paginate: pages of at most `rows` rows; always at least one page */
void hf_paginate(const hf_atoms_t *a, int rows, hf_pages_t *out);
void hf_atoms_free(hf_atoms_t *a);

#endif
