/* mcpcfg.c - MCP config files (port of omp mcp/config-writer.ts, validateServerConfig from mcp/config.ts and
 * findMcpServerForAuth from mcp/oauth-reauth.ts). The user file is <agent dir>/mcp.json, the project file
 * <cwd>/.omc/mcp.json (omp's getMCPConfigPath with omc's directory name). Writes are atomic (temp file +
 * rename, mode 0600) and keep `$schema` as the first key like writeMCPConfigFile. */
#include "mcp.h"

#include "core/fs.h"

#include <errno.h>

#define MCP_CONFIG_SCHEMA_URL "https://raw.githubusercontent.com/can1357/oh-my-pi/main/packages/coding-agent/src/config/mcp-schema.json"

char *mcp_config_path(const char *scope, const char *cwd) {
    if (str_eq(scope, "user")) return path_join(omc_agent_dir(), "mcp.json");
    return path_join3(cwd, ".omc", "mcp.json");
}

json_t *mcp_config_read(const char *path, char **err) {
    if (err) *err = NULL;
    size_t n;
    char *s = fs_read(path, &n);
    if (!s) {
        if (errno != ENOENT && fs_exists(path)) {
            if (err) *err = xasprintf("%s: %s", path, strerror(errno));
            return NULL;
        }
        json_t *c = json_obj();
        json_set(c, "mcpServers", json_obj());
        return c;
    }
    char *perr = NULL;
    json_t *j = json_parse(s, n, &perr);
    free(s);
    if (!j) {
        if (err) *err = xasprintf("JSON Parse error: %s", perr ? perr : "invalid JSON");
        free(perr);
        return NULL;
    }
    free(perr);
    return j;
}

/* writeMCPConfigFile: { $schema (kept or default), ...config } */
static int config_write(const char *path, const json_t *cfg, char **err) {
    json_t *out = json_obj();
    const char *schema = json_get_str(cfg, "$schema");
    json_set_str(out, "$schema", schema ? schema : MCP_CONFIG_SCHEMA_URL);
    const char *k;
    json_t *v;
    json_obj_foreach(cfg, i, k, v) if (strcmp(k, "$schema")) json_set(out, k, json_clone(v));
    char *dir = path_dirname(path);
    fs_mkdir_p(dir, 0700);
    free(dir);
    char *s = json_dump(out, 2);
    json_free(out);
    int rc = fs_write_atomic(path, s, strlen(s), 0600);
    free(s);
    if (rc != 0) {
        if (err) *err = xasprintf("%s: %s", path, strerror(rc < 0 ? -rc : errno));
        return -1;
    }
    return 0;
}

const char *mcp_validate_server_name(const char *name) {
    if (!name || !*name) return "Server name cannot be empty";
    if (strlen(name) > 100) return "Server name is too long (max 100 characters)";
    /* ^[a-zA-Z0-9_.:-]+(?: [a-zA-Z0-9_.:-]+)*$ */
    bool prev_space = true;
    for (const char *c = name; *c; c++) {
        bool word = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' ||
                    *c == '.' || *c == ':' || *c == '-';
        if (word) prev_space = false;
        else if (*c == ' ' && !prev_space) prev_space = true;
        else return "Server name can only contain letters, numbers, dash, underscore, dot, colon, and single spaces";
    }
    if (prev_space) return "Server name can only contain letters, numbers, dash, underscore, dot, colon, and single spaces";
    return NULL;
}

char *mcp_validate_server_config(const char *name, const json_t *cfg) {
    strvec_t errs = {0};
    const char *type = json_get_str(cfg, "type") ? json_get_str(cfg, "type") : "stdio";
    const char *cmd = json_get_str(cfg, "command"), *url = json_get_str(cfg, "url");
    if (cmd && *cmd && url && *url)
        strvec_push_own(&errs, xasprintf("Server \"%s\": both \"command\" and \"url\" are set - server should be either stdio "
                                         "(command) OR http/sse (url), not both",
                                         name));
    if (!strcmp(type, "stdio")) {
        if (!cmd || !*cmd) strvec_push_own(&errs, xasprintf("Server \"%s\": stdio server requires \"command\" field", name));
    } else if (!strcmp(type, "http") || !strcmp(type, "sse")) {
        if (!url || !*url) strvec_push_own(&errs, xasprintf("Server \"%s\": %s server requires \"url\" field", name, type));
    } else {
        strvec_push_own(&errs, xasprintf("Server \"%s\": unknown server type \"%s\"", name, type));
    }
    if (!errs.n) return NULL;
    sb_t b = SB_INIT;
    for (size_t i = 0; i < errs.n; i++) sb_appendf(&b, "%s%s", i ? "; " : "", errs.v[i]);
    strvec_free(&errs);
    return sb_detach(&b);
}

static int check_entry(const char *name, const json_t *server, char **err) {
    const char *ne = mcp_validate_server_name(name);
    if (ne) {
        *err = xstrdup(ne);
        return -1;
    }
    char *ce = mcp_validate_server_config(name, server);
    if (ce) {
        *err = xasprintf("Invalid server config: %s", ce);
        free(ce);
        return -1;
    }
    return 0;
}

static json_t *servers_of(json_t *cfg) {
    json_t *m = json_get(cfg, "mcpServers");
    if (!json_is_obj(m)) {
        json_set(cfg, "mcpServers", json_obj());
        m = json_get(cfg, "mcpServers");
    }
    return m;
}

int mcp_config_add(const char *path, const char *name, const json_t *server, char **err) {
    *err = NULL;
    if (check_entry(name, server, err)) return -1;
    json_t *cfg = mcp_config_read(path, err);
    if (!cfg) return -1;
    json_t *m = servers_of(cfg);
    int rc = -1;
    if (json_get(m, name)) *err = xasprintf("Server \"%s\" already exists in %s", name, path);
    else {
        json_set(m, name, json_clone(server));
        rc = config_write(path, cfg, err);
    }
    json_free(cfg);
    return rc;
}

int mcp_config_update(const char *path, const char *name, const json_t *server, char **err) {
    *err = NULL;
    if (check_entry(name, server, err)) return -1;
    json_t *cfg = mcp_config_read(path, err);
    if (!cfg) return -1;
    json_set(servers_of(cfg), name, json_clone(server));
    int rc = config_write(path, cfg, err);
    json_free(cfg);
    return rc;
}

int mcp_config_remove(const char *path, const char *name, char **err) {
    *err = NULL;
    json_t *cfg = mcp_config_read(path, err);
    if (!cfg) return -1;
    json_t *m = json_get(cfg, "mcpServers");
    int rc = -1;
    if (!json_is_obj(m) || !json_get(m, name)) *err = xasprintf("Server \"%s\" not found in %s", name, path);
    else {
        json_del(m, name);
        rc = config_write(path, cfg, err);
    }
    json_free(cfg);
    return rc;
}

json_t *mcp_config_disabled(const char *path) {
    json_t *cfg = mcp_config_read(path, NULL);
    json_t *out = json_arr(), *x;
    json_arr_foreach(json_get_arr(cfg, "disabledServers"), i, x) if (json_is_str(x)) json_push_str(out, json_sv(x));
    json_free(cfg);
    return out;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int mcp_config_set_disabled(const char *path, const char *name, bool disabled, char **err) {
    *err = NULL;
    json_t *cfg = mcp_config_read(path, err);
    if (!cfg) return -1;
    strvec_t set = {0};
    json_t *x;
    json_arr_foreach(json_get_arr(cfg, "disabledServers"), i, x)
        if (json_is_str(x) && !strvec_contains(&set, json_sv(x))) strvec_push(&set, json_sv(x));
    if (disabled && !strvec_contains(&set, name)) strvec_push(&set, name);
    if (!disabled)
        for (size_t i = 0; i < set.n; i++)
            if (!strcmp(set.v[i], name)) {
                free(set.v[i]);
                memmove(set.v + i, set.v + i + 1, (set.n - i - 1) * sizeof(char *));
                set.n--;
                break;
            }
    qsort(set.v, set.n, sizeof(char *), cmp_str);
    if (set.n) {
        json_t *arr = json_arr();
        for (size_t i = 0; i < set.n; i++) json_push_str(arr, set.v[i]);
        json_set(cfg, "disabledServers", arr);
    } else {
        json_del(cfg, "disabledServers");
    }
    strvec_free(&set);
    int rc = config_write(path, cfg, err);
    json_free(cfg);
    return rc;
}

int mcp_find_server(const char *name, const char *cwd, mcp_manager_t *m, bool enable_project, char **path,
                    const char **scope, json_t **cfg, bool *discovered) {
    *path = NULL;
    *cfg = NULL;
    *discovered = false;
    *scope = "user";
    char *up = mcp_config_path("user", cwd), *pp = mcp_config_path("project", cwd);
    json_t *uc = mcp_config_read(up, NULL), *pc = enable_project ? mcp_config_read(pp, NULL) : NULL;
    json_t *hit;
    int rc = -1;
    if (enable_project && (hit = json_get_obj(json_get_obj(pc, "mcpServers"), name))) {
        *path = xstrdup(pp);
        *scope = "project";
        *cfg = json_clone(hit);
        rc = 0;
    } else if ((hit = json_get_obj(json_get_obj(uc, "mcpServers"), name))) {
        *path = xstrdup(up);
        *cfg = json_clone(hit);
        rc = 0;
    } else if (enable_project) {
        const char *standalone[] = {"mcp.json", ".mcp.json"};
        for (size_t i = 0; i < 2 && rc; i++) {
            char *sp = path_join(cwd, standalone[i]);
            json_t *sc = mcp_config_read(sp, NULL);
            if ((hit = json_get_obj(json_get_obj(sc, "mcpServers"), name))) {
                *path = xstrdup(sp);
                *scope = "project";
                *cfg = json_clone(hit);
                rc = 0;
            }
            json_free(sc);
            free(sp);
        }
    }
    if (rc && m) {
        json_t *mc = mcp_server_config(m, name);
        if (mc) {
            json_del(mc, "_source");
            json_del(mc, "_provider");
            *path = xstrdup(up);
            *cfg = mc;
            *discovered = true;
            rc = 0;
        }
    }
    json_free(uc);
    json_free(pc);
    free(up);
    free(pp);
    return rc;
}
