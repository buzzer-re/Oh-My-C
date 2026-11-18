/* fuzz_tarx - the strict tar.gz member reader `omc update` runs over downloaded release archives (src/core/tarx.c).
 * Input: <selector byte><bytes>; an even selector feeds the bytes to tarx_gunzip first (1 MiB cap) and the result to
 * tarx_find, an odd one gives tarx_find the bytes as a tar image. */
#include "core/core.h"
#include "core/tarx.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 1 << 20) return 0;
    unsigned sel = fuzz_byte(&data, &size);
    const char *tar = (const char *)data;
    size_t n = size;
    char *inflated = NULL, *err = NULL;
    if (!(sel & 1)) {
        inflated = tarx_gunzip(data, size, 1 << 20, &n, &err);
        if (!inflated) {
            FUZZ_ASSERT(err);
            free(err);
            return 0;
        }
        FUZZ_ASSERT(!err && n <= 1 << 20 && inflated[n] == 0);
        tar = inflated;
    }
    static const char *const names[] = {"omc-9.9.9-linux-arm64/omc", "omc", "./a/b", "x"};
    const char *d;
    size_t len;
    size_t max = (sel >> 1) & 1 ? 64 : 1 << 20;
    int rc = tarx_find(tar, n, names[(sel >> 2) & 3], max, &d, &len, &err);
    FUZZ_ASSERT(rc >= -1 && rc <= 1 && (rc == -1) == (err != NULL));
    if (rc == 0) FUZZ_ASSERT(d >= tar && len <= max && (size_t)(d - tar) <= n && len <= n - (size_t)(d - tar));
    else FUZZ_ASSERT(!d && !len);
    free(err);
    free(inflated);
    return 0;
}
