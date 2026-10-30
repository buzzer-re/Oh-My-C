/*
 * regex.h - linear-time regex engine with Rust `regex` crate syntax (as used by ripgrep).
 *
 * Supported: literals/escapes (\n \t \xHH \x{H..} \u{..}), `.`, classes `[...]` with ranges, negation,
 * nested classes, POSIX `[[:alpha:]]`, set ops `&&` `--` `~~`, Perl classes \d \w \s (Unicode-aware,
 * approximate tables), \p{..}/\P{..} for common categories/scripts, anchors ^ $ \A \z, word boundaries
 * \b \B \< \> \b{start} \b{end}, groups (capturing, (?:..), (?P<n>..), (?<n>..)), alternation,
 * greedy/lazy quantifiers * + ? {n} {n,} {n,m}, inline flags (?imsxU) and scoped (?i:...).
 * Matching is over UTF-8 code points (byte-level automaton); leftmost-first semantics like Rust.
 *
 * Engine: Thompson NFA compiled to bytes; lazy DFA (forward for the match end, reverse for the
 * start) plus a Pike VM for capture groups. Literal fast paths (memmem/memchr) and a required-literal
 * prefilter. All searches run in O(n * m) worst case.
 *
 * Not supported: look-around and backreferences (compile error).
 * Word boundaries treat every non-ASCII byte as a word character (approximation of Unicode \b).
 *
 * A compiled rx_t is immutable and may be shared between threads (per-thread scratch is pooled).
 */
#ifndef OMC_TEXT_REGEX_H
#define OMC_TEXT_REGEX_H

#include "core/core.h"

#include <stddef.h>

enum {
    RX_ICASE = 1 << 0,     /* case-insensitive (ASCII + simple Unicode folding) */
    RX_MULTILINE = 1 << 1, /* ^ and $ match at line boundaries */
    RX_DOTALL = 1 << 2,    /* . matches \n */
    RX_LITERAL = 1 << 3,   /* pattern is a fixed string */
    RX_EXTENDED = 1 << 4,  /* (?x): ignore whitespace and # comments in the pattern */
    RX_UNGREEDY = 1 << 5,  /* (?U): swap greedy and lazy quantifiers */
    RX_LINE = 1 << 6,      /* line-oriented: no match may contain '\n' (classes never match it) */
};

#define RX_MAX_GROUPS 32

typedef struct rx rx_t;

typedef struct {
    ptrdiff_t start, end; /* byte offsets; -1 when the group did not participate */
} rx_span_t;

typedef struct {
    size_t start, end; /* overall match */
    int ngroups;       /* entries filled in groups[] incl. group 0: min(rx_ngroups(rx), RX_MAX_GROUPS) */
    rx_span_t groups[RX_MAX_GROUPS];
} rx_match_t;

/* Compile. Returns NULL and sets *err (owned, may pass NULL) on syntax error. */
rx_t *rx_compile(const char *pattern, int flags, char **err);
/* ripgrep/omp-style lenient compile: on an unbalanced-parenthesis error retries with literal
 * parens escaped, then falls back to a fixed-string search. Unsupported features (look-around,
 * backreferences) still return an error. */
rx_t *rx_compile_grep(const char *pattern, int flags, char **err);
void rx_free(rx_t *rx);

/* Leftmost-first search in s[start..len). Context before `start` is visible to assertions. */
bool rx_search(const rx_t *rx, const char *s, size_t len, size_t start, rx_match_t *m);
/* Same without capture groups (faster). */
bool rx_find(const rx_t *rx, const char *s, size_t len, size_t start, size_t *mstart, size_t *mend);
/* True if any match exists in s[start..len) (stops at the earliest match end). */
bool rx_is_match(const rx_t *rx, const char *s, size_t len, size_t start);
static inline bool rx_matches(const rx_t *rx, const char *s) { return rx_is_match(rx, s, strlen(s), 0); }

int rx_ngroups(const rx_t *rx);                       /* capture groups incl. group 0 */
int rx_group_index(const rx_t *rx, const char *name); /* -1 if no such named group */
const char *rx_group_name(const rx_t *rx, int i);     /* NULL if unnamed */
int rx_flags(const rx_t *rx);

/* ripgrep smart-case: true if the pattern contains an uppercase literal character
 * (escape sequences like \S or \p{Lu} and group names don't count). */
bool rx_has_uppercase(const char *pattern);
/* Escape all regex metacharacters. Owned. */
char *rx_escape(const char *s);
/* Replace matches (all if `all`, else first) with `rep`, expanding $1 ${1} ${name} $$. Owned. */
char *rx_replace(const rx_t *rx, const char *s, size_t len, const char *rep, bool all);

#endif
