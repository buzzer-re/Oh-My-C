/* fuzz_regex - the linear regex engine (src/text/regex.c) and the backtracking fallback (src/text/rxbt.c).
 * Input: <flags byte><pattern>\n\x01\n<subject>. Patterns come from the model (grep/ast tools, ttsr rules, settings). */
#include "core/core.h"
#include "text/regex.h"
#include "text/rxbt.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 8192) return 0;
    unsigned fl = fuzz_byte(&data, &size);
    int flags = (int)(fl & 0x7f);
    char *pat, *subj;
    size_t pl, sl;
    fuzz_split(data, size, &pat, &pl, &subj, &sl);
    char *err = NULL;
    rx_t *rx = (fl & 0x80) ? rx_compile_grep(pat, flags, &err) : rx_compile(pat, flags, &err);
    free(err);
    err = NULL;
    if (rx) {
        rx_match_t m;
        size_t pos = 0;
        for (int k = 0; k < 64 && pos <= sl; k++) {
            if (!rx_search(rx, subj, sl, pos, &m)) break;
            FUZZ_ASSERT(m.start <= m.end && m.end <= sl && m.start >= pos);
            for (int g = 0; g < m.ngroups; g++)
                if (m.groups[g].start >= 0) FUZZ_ASSERT(m.groups[g].start <= m.groups[g].end && (size_t)m.groups[g].end <= sl);
            size_t s2, e2;
            FUZZ_ASSERT(rx_find(rx, subj, sl, pos, &s2, &e2));
            pos = m.end > m.start ? m.end : m.end + 1;
        }
        (void)rx_is_match(rx, subj, sl, 0);
        char *r = rx_replace(rx, subj, sl, "<$1|${name}|$$>", true);
        free(r);
        rx_free(rx);
    }
    rxbt_t *bt = rxbt_compile(pat, flags & ~RX_LITERAL, &err);
    free(err);
    if (bt) {
        ptrdiff_t caps[64];
        int ng = rxbt_ngroups(bt);
        int nc = ng * 2 < 64 ? ng * 2 : 64;
        if (rxbt_search(bt, subj, sl, 0, caps, nc))
            for (int i = 0; i + 1 < nc; i += 2)
                if (caps[i] >= 0) FUZZ_ASSERT(caps[i] <= caps[i + 1] && (size_t)caps[i + 1] <= sl);
        rxbt_free(bt);
    }
    (void)rx_has_uppercase(pat);
    /* an escaped string matches itself (patterns are UTF-8 text: invalid bytes read as U+FFFD, as in omp) */
    char *esc = rx_escape(subj);
    rx_t *lit = rx_compile(esc, 0, NULL);
    if (lit) {
        if (sl == strlen(subj) && utf8_valid(subj, sl)) FUZZ_ASSERT(rx_is_match(lit, subj, sl, 0));
        rx_free(lit);
    }
    free(esc);
    free(pat);
    free(subj);
    return 0;
}
