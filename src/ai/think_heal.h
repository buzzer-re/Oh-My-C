/*
 * think_heal.h - streaming healer for reasoning markup leaked into a model's visible text channel.
 *
 * Port of packages/ai/src/dialect/{thinking,fenced-thinking}.ts (ThinkingInbandScanner). Leaked `<think>`,
 * `<thinking>`, `<scratchpad>`, Gemini ```thinking fences, Gemma and Harmony analysis channels are re-emitted as
 * thinking; Markdown code spans and fences keep a literal tag visible. Partial delimiters at a chunk tail are held
 * until they resolve. StreamMarkupHealing runs it without implied opens; the central leaked-thinking projector
 * (leaked_thinking.c) sets `implied_open`, so a bare `</think>` (the chat template prefilled the opener) is reported
 * as THINK_HEAL_IMPLIED_END instead of passing through as text.
 */
#ifndef OMC_THINK_HEAL_H
#define OMC_THINK_HEAL_H

#include "core/core.h"

enum {
    THINK_HEAL_TEXT,        /* visible text */
    THINK_HEAL_THINKING,    /* reasoning delta */
    THINK_HEAL_IMPLIED_END, /* a bare close of an implied-open tag (implied_open only); s is NULL, n 0 */
};
/* text and thinking chunks are never empty */
typedef void (*think_heal_emit_fn)(void *ud, int kind, const char *s, size_t n);

typedef struct {
    sb_t buf;
    const char *close_tag; /* static; NULL while idle */
    bool fenced;           /* inside a ```thinking block (line-aware close matcher) */
    sb_t fbuf;             /* fenced: partial-line buffer */
    char inner_ch;         /* fenced: fence char ('`'/'~') that opened a nested code block, 0 at top level */
    size_t inner_len;      /* fenced: length of that fence run */
    size_t femitted;       /* fenced: bytes of the leading line already emitted */
    int code_ticks;        /* backticks that opened the Markdown code region we are in, 0 outside code */
    bool code_fenced;
    int line_indent;       /* leading spaces on the current output line, -1 after a non-space */
    bool implied_open;     /* ThinkingInbandScannerOptions.impliedOpen; set after init */
} think_heal_t;

void think_heal_init(think_heal_t *h);
void think_heal_feed(think_heal_t *h, const char *s, size_t n, think_heal_emit_fn emit, void *ud);
void think_heal_flush(think_heal_t *h, think_heal_emit_fn emit, void *ud);
void think_heal_free(think_heal_t *h);

#endif
