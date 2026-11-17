/* fuzz_session - session JSONL loading (src/session/session.c): a corrupt or hand-edited session file must open (or be
 * refused) without crashing, and every reader of the tree (context rebuild, paths, children, markdown export, listing)
 * must cope with whatever entries it holds: missing ids, parent cycles, wrong types, odd compactions.
 * Input: the file contents. The file lives in a throwaway sessions dir under $TMPDIR. */
#include "core/core.h"
#include "core/fs.h"
#include "core/json.h"
#include "session/session.h"
#include "fuzz.h"

static char g_dir[600], g_file[700];

static void cleanup(void) {
    unlink(g_file);
    rmdir(g_dir);
}

FUZZ_SETUP {
    const char *t = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/omc-fuzz-session-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(g_dir)) abort();
    snprintf(g_file, sizeof g_file, "%s/2026-01-01T00-00-00-000Z_fuzz.jsonl", g_dir);
    atexit(cleanup);
}

FUZZ_ENTRY {
    if (size > 1 << 18) return 0;
    FILE *f = fopen(g_file, "wb");
    if (!f) return 0;
    fwrite(data, 1, size, f);
    fclose(f);
    char *err = NULL;
    session_t *s = session_open(g_file, &err);
    free(err);
    if (s) {
        s->persist = false; /* readers only: nothing is written back */
        session_context_t c;
        session_build_context(s, &c);
        FUZZ_ASSERT(json_is_arr(c.messages));
        session_context_free(&c);
        session_build_context_as(s, "openai", "openai-responses", &c);
        session_context_free(&c);
        json_t *p = session_current_path(s);
        json_t *e;
        json_arr_foreach(p, i, e) {
            const char *id = json_get_str(e, "id");
            json_t *ch = session_children(s, id);
            json_free(ch);
            json_t *m = json_get(e, "message");
            if (m) {
                json_t *l = session_message_to_llm(m);
                json_free(l);
            }
            if (str_eq(json_get_str(e, "type"), "compaction")) {
                bool oh = false;
                json_t *pl = session_compaction_payload(e, &oh);
                json_free(pl);
                (void)session_compaction_readable(e, "anthropic", "anthropic-messages");
            }
        }
        json_free(p);
        char *leaf = session_leaf_dup(s);
        json_t *pt = session_path_to(s, leaf);
        json_free(pt);
        free(leaf);
        char *md = session_export_markdown(s);
        free(md);
        strvec_t dirs = session_additional_dirs(s);
        strvec_free(&dirs);
        session_free(s);
    }
    session_list_t l = {0};
    if (session_list(g_dir, &l) == 0) session_list_free(&l);
    return 0;
}
