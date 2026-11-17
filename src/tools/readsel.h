/* readsel.h - the read tool's path / selector split (omp tools/read.ts splitPathAndSel, foldListColons) */
#ifndef OMC_TOOLS_READSEL_H
#define OMC_TOOLS_READSEL_H

#include "core/core.h"

/* foldListColons: chunks split off by a colon inside one comma list join the `list` that follows them, either the
 * selector colon repeated after a comma (`a:1-30,:-30` -> `a` + `1-30,-30`) or a colon written as the range dash
 * (`a:20:40,85:100` -> `a` + `20-40,85-100`, only into a list with a comma). Colons before `min_colon` stay. True (and
 * *base / *list replaced, owned) when something folded. */
bool read_fold_list_colons(char **base, char **list, size_t min_colon);
/* splitPathAndSel: the path and its trailing selector (*sel NULL: none), list colons folded (owned) */
void read_split_path_sel(const char *raw, char **path, char **sel);

#endif
