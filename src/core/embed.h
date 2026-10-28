/* embed.h - access to files compiled into the binary (prompts/, assets/) */
#ifndef OMC_EMBED_H
#define OMC_EMBED_H

#include <stddef.h>

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t len;
} omc_embed_t;

extern const omc_embed_t omc_embedded[];
extern const size_t omc_embedded_count;

/* Look up an embedded file by path, e.g. "prompts/system/system-prompt.md". NUL-terminated data. */
const char *embed_get(const char *name, size_t *len);

#endif
