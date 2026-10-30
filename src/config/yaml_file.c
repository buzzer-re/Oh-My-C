/* yaml_file.c - glue: load YAML config files into json via text/yaml */
#include "core/fs.h"
#include "core/json.h"
#include "text/yaml.h"

#include <errno.h>

json_t *yaml_parse_file_json(const char *path, char **err) {
    size_t n;
    char *s = fs_read(path, &n);
    if (!s) {
        if (err) *err = xasprintf("cannot read: %s", strerror(errno));
        return NULL;
    }
    json_t *j = yaml_parse(s, n, err);
    free(s);
    if (j && j->type == JSON_NULL) {
        json_free(j);
        j = json_obj();
    }
    return j;
}
