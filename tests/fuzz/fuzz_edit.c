/* fuzz_edit - the edit engines on model-written payloads (src/tools/patch.c, hashline.c, sloppy.c, fuzzy.c, diff.c):
 * replace, patch (unified hunks), apply_patch (Codex envelope), sloppy and hashline, applied to an in-memory file
 * system through pe_opts_t's remote hooks (nothing reaches the disk).
 * Input: <mode byte><file text>\n\x01\n<edit input>. The edit input is the tool's JSON args when it parses as an
 * object, else the `input` payload; "@TAG@" in it becomes the file's current hashline tag, "@P@" its path. */
#include "core/core.h"
#include "core/json.h"
#include "tools/tools.h"
#include "tools/patch.h"
#include "tools/hashline.h"
#include "tools/diff.h"
#include "fuzz.h"

#include <errno.h>

#define FPATH "/fuzz/a.txt"
static hmap_t g_fs;
static edit_store_t g_store;

static void free_val(void *v) { free(v); }

static bool fs_remote(void *ud, const char *abs) { return true; }
static int fs_read(void *ud, const char *abs, char **data, size_t *len, char **err) {
    const char *v = hmap_get(&g_fs, abs);
    if (!v) return ENOENT;
    *data = xstrdup(v);
    *len = strlen(v);
    return 0;
}
static int fs_write(void *ud, const char *abs, const char *data, size_t len, char **err) {
    if (g_fs.n > 64) {
        *err = xstrdup("too many files");
        return -1;
    }
    free(hmap_put(&g_fs, abs, xstrndup(data, len)));
    return 0;
}

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 1 << 15) return 0;
    unsigned sel = fuzz_byte(&data, &size);
    char *file, *in;
    size_t fl, il;
    fuzz_split(data, size, &file, &fl, &in, &il);
    hmap_init(&g_fs);
    edit_store_init(&g_store);
    hmap_put(&g_fs, FPATH, xstrdup(file));
    int64_t lines = (int64_t)str_count(file, "\n") + 1;
    const char *tag = edit_store_record(&g_store, FPATH, file, 1, lines);
    char *t1 = str_replace(in, "@TAG@", tag ? tag : "0000");
    char *input = str_replace(t1, "@P@", FPATH);
    free(t1);
    pe_mode_t mode = (pe_mode_t)(sel % 5);
    json_t *args = json_parse(input, strlen(input), NULL);
    if (!json_is_obj(args)) {
        json_free(args);
        args = json_obj();
        json_set_str(args, "input", input);
        json_set_str(args, "path", FPATH);
        if (mode == PE_REPLACE) {
            const char *nl = strchr(input, '\n');
            json_set(args, "old_string", nl ? json_strn(input, (size_t)(nl - input)) : json_str(input));
            json_set_str(args, "new_string", nl ? nl + 1 : "");
            json_set_bool(args, "replace_all", sel & 0x80);
        }
    }
    pe_opts_t o = {.cwd = "/fuzz", .allow_fuzzy = sel & 0x20, .fuzzy_threshold = 0.95, .remote = fs_remote,
                   .remote_read = fs_read, .remote_write = fs_write, .store = &g_store, .enforce_seen = sel & 0x40,
                   .max_columns = 512};
    pe_result_t r = {0};
    pe_run(mode, args, &o, &r);
    FUZZ_ASSERT(r.text);
    pe_result_free(&r);
    /* the diff renderers on before/after */
    const char *after = hmap_get(&g_fs, FPATH);
    if (after && strlen(file) < 4096 && strlen(after) < 4096) {
        char *d = diff_unified(file, after, "a", "b", 3);
        free(d);
        int64_t first = 0;
        char *n = diff_numbered(file, after, 2, &first);
        size_t ad = 0, rm = 0;
        char *cp = diff_compact_preview(n, &ad, &rm);
        free(cp);
        free(n);
    }
    json_free(args);
    free(input);
    edit_store_free(&g_store);
    hmap_free(&g_fs, free_val);
    free(file);
    free(in);
    return 0;
}
