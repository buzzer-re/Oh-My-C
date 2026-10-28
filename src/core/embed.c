#include "embed.h"

#include <string.h>

const char *embed_get(const char *name, size_t *len) {
    /* table is sorted by name (generator receives sorted list) */
    size_t lo = 0, hi = omc_embedded_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(name, omc_embedded[mid].name);
        if (c == 0) {
            if (len) *len = omc_embedded[mid].len;
            return (const char *)omc_embedded[mid].data;
        }
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}
