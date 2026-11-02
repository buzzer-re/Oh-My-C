/* test_mcp.c - WP-18 MCP / SSH management: server-name and config validation, the mcp.json writer
 * (add / update / remove / disabledServers, `$schema` first), the ssh.json writer, the /mcp and /ssh registry
 * entries (no Smithery verbs advertised), and the manager lifecycle against tests/e2e/mock_mcp.py
 * (connect, state, catalogs, reconnect, disconnect, one-off probes). */
#include "coding/slashcmd.h"
#include "core/fs.h"
#include "mcp/mcp.h"
#include "tools/tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static void test_validation(void) {
    CHECK(!mcp_validate_server_name("github"));
    CHECK(!mcp_validate_server_name("MaaS Slack"));
    CHECK(!mcp_validate_server_name("a.b:c-d_e"));
    CHECK(str_eq(mcp_validate_server_name(""), "Server name cannot be empty"));
    CHECK(mcp_validate_server_name("two  spaces") != NULL);
    CHECK(mcp_validate_server_name(" lead") != NULL);
    CHECK(mcp_validate_server_name("trail ") != NULL);
    CHECK(mcp_validate_server_name("bad!") != NULL);
    char longname[120];
    memset(longname, 'a', 101);
    longname[101] = 0;
    CHECK(str_eq(mcp_validate_server_name(longname), "Server name is too long (max 100 characters)"));

    json_t *c = json_parse_str("{\"type\":\"stdio\"}");
    char *e = mcp_validate_server_config("x", c);
    CHECK(e && str_eq(e, "Server \"x\": stdio server requires \"command\" field"));
    free(e);
    json_free(c);
    c = json_parse_str("{\"type\":\"sse\",\"url\":\"http://a\"}");
    CHECK(!mcp_validate_server_config("x", c));
    json_free(c);
    c = json_parse_str("{\"type\":\"ws\"}");
    e = mcp_validate_server_config("x", c);
    CHECK(e && strstr(e, "unknown server type \"ws\""));
    free(e);
    json_free(c);

    CHECK(!ssh_validate_host_name("box-1.lan"));
    CHECK(str_eq(ssh_validate_host_name("a b"), "Host name can only contain letters, numbers, dash, underscore, and dot"));
}

static void test_config_writer(const char *dir) {
    char *path = mcp_config_path("project", dir), *err = NULL;
    CHECK(str_ends(path, "/.omc/mcp.json"));
    json_t *c = mcp_config_read(path, &err);
    CHECK(c && json_is_obj(json_get(c, "mcpServers")) && !json_len(json_get(c, "mcpServers")));
    json_free(c);
    json_t *srv = json_parse_str("{\"type\":\"stdio\",\"command\":\"npx\",\"args\":[\"-y\"]}");
    CHECK(mcp_config_add(path, "one", srv, &err) == 0);
    CHECK(mcp_config_add(path, "one", srv, &err) != 0 && strstr(err, "Server \"one\" already exists in"));
    free(err);
    CHECK(mcp_config_add(path, "bad!", srv, &err) != 0 && strstr(err, "Server name can only contain"));
    free(err);
    size_t n;
    char *text = fs_read(path, &n);
    CHECK(text && str_starts(text, "{\n  \"$schema\": \"https://raw.githubusercontent.com/can1357/oh-my-pi/main/"));
    CHECK(text && strstr(text, "\"one\": {\n      \"type\": \"stdio\",\n      \"command\": \"npx\""));
    free(text);
    json_set_bool(srv, "enabled", false);
    CHECK(mcp_config_update(path, "one", srv, &err) == 0);
    c = mcp_config_read(path, &err);
    CHECK(!json_get_bool(json_get_obj(json_get_obj(c, "mcpServers"), "one"), "enabled", true));
    json_free(c);
    CHECK(mcp_config_set_disabled(path, "zed", true, &err) == 0);
    CHECK(mcp_config_set_disabled(path, "alpha", true, &err) == 0);
    json_t *d = mcp_config_disabled(path);
    CHECK(json_len(d) == 2 && str_eq(json_sv(json_at(d, 0)), "alpha")); /* sorted like Array.from(set).sort() */
    json_free(d);
    CHECK(mcp_config_set_disabled(path, "alpha", false, &err) == 0 && mcp_config_set_disabled(path, "zed", false, &err) == 0);
    c = mcp_config_read(path, &err);
    CHECK(!json_get(c, "disabledServers")); /* an empty list is removed */
    json_free(c);
    CHECK(mcp_config_remove(path, "one", &err) == 0);
    CHECK(mcp_config_remove(path, "one", &err) != 0 && strstr(err, "Server \"one\" not found in"));
    free(err);
    /* findMcpServerForAuth: project entry, then the standalone .mcp.json */
    CHECK(mcp_config_add(path, "proj", srv, &err) == 0);
    char *sp = path_join(dir, ".mcp.json");
    const char *legacy = "{\"mcpServers\":{\"legacy\":{\"url\":\"http://x\"}}}";
    fs_write(sp, legacy, strlen(legacy));
    char *fp = NULL;
    const char *scope = NULL;
    json_t *cfg = NULL;
    bool disc = true;
    CHECK(mcp_find_server("proj", dir, NULL, true, &fp, &scope, &cfg, &disc) == 0 && str_eq(scope, "project") && !disc &&
          str_eq(fp, path));
    free(fp), json_free(cfg);
    CHECK(mcp_find_server("legacy", dir, NULL, true, &fp, &scope, &cfg, &disc) == 0 && str_eq(fp, sp));
    free(fp), json_free(cfg);
    CHECK(mcp_find_server("legacy", dir, NULL, false, &fp, &scope, &cfg, &disc) != 0); /* mcp.enableProjectConfig off */
    CHECK(mcp_find_server("nope", dir, NULL, true, &fp, &scope, &cfg, &disc) != 0);
    free(sp);
    json_free(srv);
    free(path);
}

static void test_ssh_writer(const char *dir) {
    char *path = ssh_config_path("project", dir), *err = NULL;
    CHECK(str_ends(path, "/.omc/ssh.json"));
    json_t *h = json_parse_str("{\"host\":\"10.0.0.5\",\"port\":2222}");
    CHECK(ssh_config_add(path, "box", h, &err) == 0);
    CHECK(ssh_config_add(path, "box", h, &err) != 0 && strstr(err, "Host \"box\" already exists in"));
    free(err);
    json_t *empty = json_parse_str("{\"host\":\"\"}");
    CHECK(ssh_config_add(path, "e", empty, &err) != 0 && str_eq(err, "Host address cannot be empty"));
    free(err);
    json_free(empty);
    size_t n;
    char *text = fs_read(path, &n);
    CHECK(text && str_eq(text, "{\n  \"hosts\": {\n    \"box\": {\n      \"host\": \"10.0.0.5\",\n      \"port\": 2222\n    }\n  }\n}"));
    free(text);
    CHECK(ssh_config_remove(path, "box", &err) == 0);
    CHECK(ssh_config_remove(path, "box", &err) != 0 && strstr(err, "Host \"box\" not found in"));
    free(err);
    json_free(h);
    free(path);
}

static void test_registry(void) {
    const slash_cmd_t *m = slash_lookup("mcp"), *s = slash_lookup("ssh");
    CHECK(m && s && m->tui && s->tui && m->handle && s->handle && m->allow_args);
    bool smithery = false, reauth = false;
    for (size_t i = 0; m && i < m->nsubs; i++) {
        smithery |= str_starts(m->subs[i].name, "smithery");
        reauth |= str_eq(m->subs[i].name, "reauth");
    }
    CHECK(!smithery && reauth && m->nsubs == 14);
    CHECK(s && s->nsubs == 4 && str_eq(s->subs[0].name, "add"));
}

static void test_manager(void) {
    if (access("tests/e2e/mock_mcp.py", R_OK) != 0) return;
    json_t *cfg = json_parse_str("{\"command\":\"python3\",\"args\":[\"-I\",\"tests/e2e/mock_mcp.py\",\"--full\"]}");
    mcp_manager_t *m = mcp_manager_new();
    CHECK(mcp_server_state(m, "mock") == MCP_DISCONNECTED);
    CHECK(mcp_connect_server(m, "mock", cfg, 20000) == MCP_CONNECTED);
    json_t *tools = mcp_server_tools(m, "mock");
    CHECK(json_len(tools) == 2);
    json_free(tools);
    json_t *res = mcp_server_resources(m, "mock");
    CHECK(str_eq(json_get_str(json_at(json_get_arr(res, "resources"), 0), "uri"), "mem://notes"));
    CHECK(str_eq(json_get_str(json_at(json_get_arr(res, "templates"), 0), "uriTemplate"), "mem://item/{id}"));
    json_free(res);
    json_t *pr = mcp_server_prompts(m, "mock");
    CHECK(json_len(pr) == 2);
    json_free(pr);
    json_t *caps = mcp_server_capabilities(m, "mock");
    CHECK(json_get_bool(json_get_obj(caps, "resources"), "subscribe", false));
    json_free(caps);
    mcp_set_notifications(m, true);
    for (int i = 0; i < 100; i++) {
        json_t *sub = mcp_subscriptions(m, "mock");
        bool ok = json_len(sub) == 1;
        json_free(sub);
        if (ok) break;
        usleep(20000);
    }
    json_t *sub = mcp_subscriptions(m, "mock");
    CHECK(json_len(sub) == 1 && str_eq(json_sv(json_at(sub, 0)), "mem://notes"));
    json_free(sub);
    char *err = NULL;
    CHECK(mcp_reconnect_server(m, "mock", 20000, &err) == MCP_CONNECTED);
    free(err);
    json_t *names = mcp_connected_names(m);
    CHECK(json_len(names) == 1);
    json_free(names);
    CHECK(mcp_disconnect_server(m, "mock") && !mcp_has_server(m, "mock"));
    CHECK(mcp_reconnect_server(m, "mock", 1000, &err) == MCP_DISCONNECTED && err && strstr(err, "not found"));
    free(err);
    /* one-off probes */
    json_t *info = NULL;
    tools = NULL;
    CHECK(mcp_probe("p", cfg, 20000, NULL, &info, &tools, &err, NULL, NULL) == 0 && str_eq(json_get_str(info, "name"), "mock") &&
          json_len(tools) == 2);
    json_free(info), json_free(tools);
    json_t *cat = mcp_probe_catalog("p", cfg, 20000, &err);
    CHECK(cat && json_len(json_get_arr(cat, "resources")) == 1 && json_len(json_get_arr(cat, "prompts")) == 2);
    json_free(cat);
    json_t *bad = json_parse_str("{\"command\":\"/nonexistent/omc-no-such-binary\"}");
    bool auth = true;
    CHECK(mcp_probe("p", bad, 2000, NULL, NULL, NULL, &err, &auth, NULL) != 0 && err && !auth);
    free(err);
    json_free(bad);
    mcp_manager_free(m);
    json_free(cfg);
}

int main(void) {
    char tmpl[] = "/tmp/omc-test-mcp-XXXXXX";
    const char *tmp = getenv("TMPDIR");
    char *dir_t = tmp ? xasprintf("%s/omc-test-mcp-XXXXXX", tmp) : xstrdup(tmpl);
    char *dir = mkdtemp(dir_t);
    if (!dir) return 1;
    test_validation();
    test_config_writer(dir);
    test_ssh_writer(dir);
    test_registry();
    test_manager();
    char *rm = xasprintf("rm -rf '%s'", dir);
    if (system(rm) != 0) {}
    free(rm);
    free(dir_t);
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
