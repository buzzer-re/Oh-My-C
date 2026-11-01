/* blobs.h - session image blob store (omp blob:sha256 references; see blobs.c) */
#ifndef OMC_SESSION_BLOBS_H
#define OMC_SESSION_BLOBS_H
#include "core/core.h"
#include "core/json.h"

/* persisted copy of `entry` (owned) with image payloads >= 1024 base64 chars moved to the blob store */
json_t *session_blobs_externalize(const json_t *entry);
/* replace `blob:sha256:<hex>` image payloads in a loaded entry with their base64 (in place) */
void session_blobs_resolve(json_t *entry);

#endif
