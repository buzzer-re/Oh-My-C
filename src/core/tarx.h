/* tarx.h - a strict reader for one member of an untrusted .tar.gz (the `omc update` release archives).
 *
 * The archive is decompressed in memory under a size cap and walked header by header; only a regular file whose
 * full name is exactly the one asked for is returned, and nothing in the archive ever names a path on disk. ustar
 * (with its name prefix), GNU long names and pax `path` / `size` records are understood; checksums are verified;
 * every size is bounds-checked against what is actually there. (src/tools/readfmt.c has the `read` tool's archive
 * browser, which follows in-archive symlinks and indexes every member; this is the narrower reader an installer
 * needs.) */
#ifndef OMC_TARX_H
#define OMC_TARX_H

#include "core/core.h"

/* gunzip `in` (one or more concatenated gzip members) into an owned, NUL-terminated buffer of at most `cap` bytes.
 * NULL with *err (owned) when the data is not gzip, is truncated or corrupt, or inflates past `cap`. */
char *tarx_gunzip(const void *in, size_t n, size_t cap, size_t *out_len, char **err);

/* Find the regular file `name` in the tar image `tar` (n bytes). 0: found, *data / *len point into `tar`;
 * 1: no such regular file (a directory, link or device of that name does not count); -1: the archive is malformed,
 * names `name` more than once, or the member exceeds `max_size` (*err owned). */
int tarx_find(const void *tar, size_t n, const char *name, size_t max_size, const char **data, size_t *len,
              char **err);

#endif
