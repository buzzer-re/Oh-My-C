/* fuzz_glob - glob compiler/matcher and gitignore sets (src/text/glob.c).
 * Input: <flags byte><pattern lines>\n\x01\n<path>. */
#include "core/core.h"
#include "text/glob.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 4096) return 0;
    int flags = (int)(fuzz_byte(&data, &size) & 0x0f);
    char *pat, *path;
    size_t pl, sl;
    fuzz_split(data, size, &pat, &pl, &path, &sl);
    glob_pat_t *g = glob_compile(pat, flags);
    if (g) {
        (void)glob_pat_match(g, path);
        glob_pat_free(g);
    }
    (void)glob_match(pat, path, flags);
    (void)glob_has_magic(pat);
    (void)glob_depth_bound(pat);
    char *n = glob_normalize(pat, flags & 1);
    free(n);
    ignore_set_t *s = ignore_set_new();
    ignore_set_add_text(s, pat, pl);
    (void)ignore_set_count(s);
    (void)ignore_set_match(s, path, flags & 1);
    (void)ignore_set_match_parents(s, path, flags & 2);
    ignore_set_free(s);
    free(pat);
    free(path);
    return 0;
}
