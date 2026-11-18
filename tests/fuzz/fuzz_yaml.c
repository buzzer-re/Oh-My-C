/* fuzz_yaml - YAML parser (src/text/yaml.c): config.yml, models.yml, frontmatter of skills/rules/agents. */
#include "core/core.h"
#include "core/json.h"
#include "text/yaml.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 1 << 18) return 0;
    char *s = fuzz_cstr(data, size);
    char *err = NULL;
    json_t *j = yaml_parse(s, size, &err);
    free(err);
    err = NULL;
    if (j) {
        char *y = yaml_dump(j);
        json_t *k = yaml_parse(y, strlen(y), &err);
        free(err);
        err = NULL;
        json_free(k);
        free(y);
        json_free(j);
    }
    j = yaml_parse_all(s, size, &err);
    free(err);
    err = NULL;
    json_free(j);
    const char *body = NULL;
    j = yaml_frontmatter(s, size, &body, &err);
    if (body) FUZZ_ASSERT(body >= s && body <= s + size);
    free(err);
    json_free(j);
    free(s);
    return 0;
}
