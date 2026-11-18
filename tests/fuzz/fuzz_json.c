/* fuzz_json - JSON parser and serializers (src/core/json.c).
 * Properties: parse -> dump (compact and indented, and JSON.stringify form) -> parse is the same tree; partial parse
 * never crashes; path lookups and clone/equal/merge on the parsed tree are safe. */
#include "core/core.h"
#include "core/json.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 1 << 20) return 0;
    char *err = NULL;
    json_t *j = json_parse((const char *)data, size, &err);
    free(err);
    if (j) {
        /* dump -> parse is lossless once the text has been through one dump: the first dump replaces invalid UTF-8
         * with U+FFFD (what JSON.stringify of a JS string can hold), so compare from the reparsed tree on */
        for (int indent = 0; indent <= 2; indent += 2) {
            char *s = json_dump(j, indent);
            char *e2 = NULL;
            json_t *k = json_parse(s, strlen(s), &e2);
            if (!k) fprintf(stderr, "reparse failed (%s): %s\n", e2 ? e2 : "?", s);
            FUZZ_ASSERT(k);
            free(e2);
            char *s2 = json_dump(k, indent);
            json_t *k2 = json_parse(s2, strlen(s2), NULL);
            FUZZ_ASSERT(k2);
            if (!json_equal(k, k2)) fprintf(stderr, "round trip changed the tree:\n%s\n%s\n", s, s2);
            FUZZ_ASSERT(json_equal(k, k2));
            char *s3 = json_dump(k2, indent);
            FUZZ_ASSERT(strcmp(s2, s3) == 0);
            free(s3);
            json_free(k2);
            free(s2);
            json_free(k);
            free(s);
        }
        char *js = json_dump_js(j);
        json_t *k = json_parse(js, strlen(js), NULL);
        FUZZ_ASSERT(k);
        json_free(k);
        free(js);
        json_t *c = json_clone(j);
        FUZZ_ASSERT(json_equal(j, c));
        json_merge(c, j);
        json_free(c);
        (void)json_path(j, "a.0.b");
        if (json_is_obj(j))
            for (size_t i = 0; i < j->u.obj.n; i++) (void)json_get_int(j, j->u.obj.keys[i], 0);
        (void)json_truthy(j);
        json_free(j);
    }
    json_t *p = json_parse_partial((const char *)data, size);
    if (p) {
        char *s = json_dump(p, 0);
        json_t *k = json_parse(s, strlen(s), NULL);
        FUZZ_ASSERT(k);
        json_free(k);
        free(s);
        json_free(p);
    }
    char *str = fuzz_cstr(data, size);
    char *q = json_quote(str);
    json_t *k = json_parse(q, strlen(q), NULL);
    FUZZ_ASSERT(k && json_is_str(k));
    json_free(k);
    free(q);
    free(str);
    return 0;
}
