/* test_compaction_point.c - per-model compaction limits (omp compaction-threshold.ts +
 * model-compaction-threshold.ts) and the settings writes the Models hub relies on (keyed global
 * entries, project-layer model roles with null tombstones, omp-style YAML without a trailing newline). */
#include "config/compaction_point.h"
#include "config/settings.h"
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static bool streq_free(char *s, const char *want) {
    bool ok = s && want ? !strcmp(s, want) : s == NULL && want == NULL;
    if (!ok) fprintf(stderr, "  got [%s] want [%s]\n", s ? s : "(null)", want ? want : "(null)");
    free(s);
    return ok;
}

int main(void) {
    char tmpl[] = "/tmp/omc-test-cpoint.XXXXXX";
    char *dir = mkdtemp(tmpl);
    char *agent = path_join(dir, "agent"), *work = path_join(dir, "work");
    fs_mkdir_p(agent, 0755);
    fs_mkdir_p(work, 0755);
    setenv("OMC_CODING_AGENT_DIR", agent, 1);
    settings_init(work, NULL);

    /* input parsing */
    json_t *e;
    char *err;
    CHECK(cp_parse_input("400k", &e, &err) && json_is_num(e) && e->u.num.i == 400000);
    json_free(e);
    CHECK(cp_parse_input("f1.5", &e, &err) == false);
    free(err);
    CHECK(cp_parse_input("F90K", &e, &err) && json_is_str(e) && !strcmp(json_sv(e), "f90000"));
    json_free(e);
    CHECK(cp_parse_input("80%", &e, &err) && json_is_str(e) && !strcmp(json_sv(e), "80%"));
    json_free(e);
    CHECK(cp_parse_input("120%", &e, &err) == false);
    free(err);
    CHECK(cp_parse_input("  ", &e, &err) && e == NULL);

    CHECK(streq_free(cp_format_window(272000), "272K"));
    CHECK(streq_free(cp_format_window(1050000), "1.05M"));
    CHECK(streq_free(cp_format_window(999), "999"));

    /* runtime trigger: reserve policy, percentage, fixed and base entries */
    json_t *raw = json_parse_str("{\"id\":\"m1\",\"provider\":\"p\",\"api\":\"openai-completions\",\"contextWindow\":200000}");
    ai_model_t m = {0};
    m.raw = raw;
    m.id = "m1";
    m.provider = "p";
    m.context_window = 200000;
    CHECK(cp_model_threshold(&m, 200000) == 170000); /* 200k - max(15%, 16384) */
    CHECK(streq_free(cp_preview(&m, "f150k"), "compacts at exactly 150K"));
    CHECK(streq_free(cp_preview(&m, "100k"), "compacts at 83.62K \xc2\xb7 83.6% of 100K base"));
    CHECK(streq_free(cp_preview(&m, "50%"), "compacts at 100K \xc2\xb7 50% of window"));
    CHECK(cp_preview(&m, "300k") == NULL); /* past the window */
    char *status = NULL;
    CHECK(cp_set(&m, "100k", &status, &err) == 0);
    CHECK(streq_free(status, "Compaction limit for p/m1: 100,000-token base \xc2\xb7 compacts at 83.62K \xc2\xb7 83.6% of 100K base"));
    CHECK(cp_model_threshold(&m, 200000) == 83616);
    CHECK(streq_free(cp_draft(&m), "100k"));
    CHECK(streq_free(cp_preview(&m, ""), "resets: compacts at 170K \xc2\xb7 85% of window"));
    CHECK(cp_set(&m, "f150000", &status, &err) == 0);
    free(status);
    CHECK(cp_model_threshold(&m, 200000) == 150000);
    CHECK(streq_free(cp_draft(&m), "f150k"));
    CHECK(cp_set(&m, "", &status, &err) == 0);
    CHECK(streq_free(status, "Compaction limit for p/m1: reset \xc2\xb7 compacts at 170K \xc2\xb7 85% of window"));
    CHECK(cp_draft(&m) == NULL);

    /* keyed global entries keep dots/slashes in the key; YAML has no trailing newline */
    settings_set_global_entry("retry.fallbackChains", "openai/gpt-5.5", json_parse_str("[\"a/b\"]"));
    json_t *ch = json_get(settings_get("retry.fallbackChains"), "openai/gpt-5.5");
    CHECK(json_is_arr(ch) && json_len(ch) == 1);
    size_t n;
    char *text = fs_read(settings_global_path(), &n);
    CHECK(text && n && text[n - 1] != '\n');
    free(text);

    /* project roles: a null tombstone falls back to the global value */
    settings_set_global_entry("modelRoles", "smol", json_str("g/smol"));
    settings_set_project_entry("modelRoles", "smol", json_str("p/smol"));
    CHECK(!strcmp(settings_str("modelRoles.smol", ""), "p/smol"));
    settings_set_project_entry("modelRoles", "smol", json_null());
    CHECK(!strcmp(settings_str("modelRoles.smol", ""), "g/smol"));
    char *pp = settings_project_config_path();
    text = fs_read(pp, &n);
    CHECK(text && !strcmp(text, "modelRoles:\n  smol: null"));
    free(text);
    free(pp);

    json_free(raw);
    char *rm = xasprintf("rm -rf '%s'", dir);
    if (system(rm)) { /* best effort */
    }
    free(rm);
    free(agent);
    free(work);
    if (fails) fprintf(stderr, "test_compaction_point: %d failure(s)\n", fails);
    else printf("compaction point: ok\n");
    return fails ? 1 : 0;
}
