/* test_keybindings.c - keybindings.yml loading: legacy names, legacy JSONC, write-back, profile inherit
 * (config/keybindings.c, port of omp app-keybindings.ts) */
#include "config/keybindings.h"
#include "core/core.h"
#include "core/fs.h"

#include <stdlib.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        checks++;                                                                         \
        if (!(cond)) {                                                                    \
            failures++;                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                                 \
    } while (0)

static char *slurp(const char *dir, const char *name) {
    char *p = path_join(dir, name);
    size_t n = 0;
    char *d = fs_read(p, &n);
    free(p);
    return d;
}

static void put(const char *dir, const char *name, const char *text) {
    char *p = path_join(dir, name);
    fs_write(p, text, strlen(text));
    free(p);
}

static bool has(tui_keybindings_t *kb, const char *action, const char *chord) {
    const strvec_t *v = tui_keybindings_get(kb, action);
    return v && strvec_contains(v, chord);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    char *base = xasprintf("%s/omc-kbtest.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    char *root = mkdtemp(base);
    CHECK(root != NULL);
    if (!root) return 1;
    char *a = path_join(root, "a"), *b = path_join(root, "b"), *c = path_join(root, "c");
    fs_mkdir_p(a, 0755);
    fs_mkdir_p(b, 0755);
    fs_mkdir_p(c, 0755);

    /* legacy names are renamed; invalid values dropped */
    json_t *raw = json_parse_str("{\"retry\": [\"f6\"], \"cursorUp\": \"ctrl+k\", \"app.exit\": \"ctrl+x\", \"bad\": [1]}");
    bool migrated = false;
    json_t *m = kbcfg_migrate_names(raw, &migrated);
    CHECK(migrated);
    CHECK(json_get(m, "app.retry") && json_get(m, "tui.editor.cursorUp") && !json_get(m, "retry") && !json_get(m, "bad"));
    json_free(m);
    json_free(raw);

    /* legacy keybindings.json (JSONC) -> keybindings.yml in registry order, no final newline */
    put(a, "keybindings.json",
        "{\n  // legacy names\n  \"retry\": [\"f6\"], /* block */\n  \"cursorUp\": \"ctrl+k\",\n  \"app.exit\": \"ctrl+x\",\n"
        "  \"custom.thing\": \"alt+z\",\n  \"selectCancel\": [\"escape\", \"ctrl+g\"],\n}\n");
    tui_keybindings_t *kb = tui_keybindings_new();
    kbcfg_apply(kb, a, NULL);
    CHECK(has(kb, "app.retry", "f6") && !has(kb, "app.retry", "f5"));
    CHECK(has(kb, "tui.editor.cursorUp", "ctrl+k"));
    char *yml = slurp(a, "keybindings.yml");
    CHECK(yml && !strcmp(yml, "tui.editor.cursorUp: ctrl+k\ntui.select.cancel:\n  - escape\n  - ctrl+g\napp.exit: ctrl+x\n"
                               "app.retry:\n  - f6\ncustom.thing: alt+z"));
    free(yml);
    char *json_kept = slurp(a, "keybindings.json");
    CHECK(json_kept != NULL); /* the legacy file is left in place */
    free(json_kept);

    /* an up-to-date keybindings.yml is read as is and never rewritten */
    put(b, "keybindings.yml", "# mine\napp.retry: alt+r\n");
    tui_keybindings_set_user(kb, NULL);
    kbcfg_apply(kb, b, NULL);
    CHECK(has(kb, "app.retry", "alt+r") && !has(kb, "app.retry", "f5"));
    yml = slurp(b, "keybindings.yml");
    CHECK(yml && str_starts(yml, "# mine"));
    free(yml);

    /* profile inherit: the default profile's bindings under the profile's own (profile wins); the inherited
     * dir is read only */
    put(c, "keybindings.yaml", "app.plan.toggle: alt+q\nexpandTools: ctrl+e\n");
    put(b, "keybindings.yml", "app.retry: alt+r\napp.plan.toggle: alt+w\n");
    json_t *merged = kbcfg_load_merged(b, c);
    CHECK(str_eq(json_get_str(merged, "app.plan.toggle"), "alt+w"));
    CHECK(str_eq(json_get_str(merged, "app.tools.expand"), "ctrl+e"));
    CHECK(str_eq(json_get_str(merged, "app.retry"), "alt+r"));
    json_free(merged);
    yml = slurp(c, "keybindings.yaml");
    CHECK(yml && strstr(yml, "expandTools")); /* not migrated on disk */
    free(yml);

    tui_keybindings_free(kb);
    char *cmd = xasprintf("rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
    free(cmd);
    free(a);
    free(b);
    free(c);
    free(base);
    printf("test_keybindings: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
