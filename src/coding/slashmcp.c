/* slashmcp.c - /mcp and /ssh for RPC and ACP (ports of omp slash-commands/helpers/mcp.ts handleMcpAcp and
 * helpers/ssh.ts handleSshAcp, v18.14.2): plain-text output through the runtime, temporary connections for
 * test / resources / prompts, and the reauth flow presented as text. */
#include "coding/slashcmd.h"
#include "coding/xdnotice.h"

#include "coding/commands.h"
#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "mcp/mcp.h"
#include "tools/tools.h"

#include <ctype.h>
#include <strings.h>

static void out(slash_rt_t *rt, const char *text) {
    if (rt->output && text && *text) rt->output(rt->ud, text);
}

static void usage(slash_rt_t *rt, slash_result_t *res, const char *text) {
    out(rt, text);
    res->failed = true;
}

static void usagef(slash_rt_t *rt, slash_result_t *res, char *owned) {
    usage(rt, res, owned);
    free(owned);
}

static bool disabled_cfg(const json_t *c) { return json_is_bool(json_get(c, "enabled")) && !json_get_bool(c, "enabled", true); }

/* getMcpConfiguredServers: enabled project entries, then enabled user entries not seen. [{name, config, scope}] */
static json_t *configured(const char *cwd) {
    char *up = mcp_config_path("user", cwd), *pp = mcp_config_path("project", cwd);
    json_t *uc = mcp_config_read(up, NULL), *pc = mcp_config_read(pp, NULL);
    json_t *outj = json_arr();
    const char *k;
    json_t *v;
    json_obj_foreach(json_get_obj(pc, "mcpServers"), i, k, v) if (json_is_obj(v) && !disabled_cfg(v)) {
        json_t *o = json_obj();
        json_set_str(o, "name", k);
        json_set(o, "config", json_clone(v));
        json_set_str(o, "scope", "project");
        json_push(outj, o);
    }
    json_obj_foreach(json_get_obj(uc, "mcpServers"), i, k, v) {
        if (!json_is_obj(v) || disabled_cfg(v) || json_get_obj(json_get_obj(pc, "mcpServers"), k)) continue;
        json_t *o = json_obj();
        json_set_str(o, "name", k);
        json_set(o, "config", json_clone(v));
        json_set_str(o, "scope", "user");
        json_push(outj, o);
    }
    json_free(uc);
    json_free(pc);
    free(up);
    free(pp);
    return outj;
}

static const char *MCP_HELP_TEXT =
    "MCP server management (ACP mode)\n"
    "  /mcp list                                               List configured servers\n"
    "  /mcp enable <name>                                      Enable a server\n"
    "  /mcp disable <name>                                     Disable a server\n"
    "  /mcp remove <name> [--scope project|user]               Remove a server\n"
    "  /mcp reload                                             Reload MCP runtime\n"
    "  /mcp resources                                          List resources from all servers\n"
    "  /mcp prompts                                            List prompts from all servers\n"
    "  /mcp test <name>                                        Test connection to a server\n"
    "  /mcp reauth <name>                                      Reauthorize OAuth for a server (opens browser)\n"
    "  /mcp add <name> [--scope project|user] [--url <url>]    Add a server (non-interactive)\n"
    "  /mcp add <name> [-- <command...>]                       Add a stdio server\n"
    "  /mcp help                                               Show this help";

#define MCP_ADD_USAGE "Usage: /mcp add <name> [--scope project|user] [--url <url> --transport http|sse] [--token <token>] [-- <command...>]"

/* resources / prompts over temporary connections; unreachable servers are skipped */
static void catalog_cmd(slash_rt_t *rt, bool prompts) {
    json_t *servers = configured(rt->app->cwd), *sv;
    if (!json_len(servers)) {
        out(rt, "No MCP servers configured.");
        json_free(servers);
        return;
    }
    sb_t b = SB_INIT;
    json_arr_foreach(servers, i, sv) {
        const char *name = json_get_str(sv, "name");
        json_t *cfg = json_get_obj(sv, "config");
        char *err = NULL;
        json_t *cat = mcp_probe_catalog(name, cfg, json_get_int(cfg, "timeout", 30000), &err);
        free(err);
        json_t *x;
        json_arr_foreach(json_get_arr(cat, prompts ? "prompts" : "resources"), k, x) {
            if (b.len) sb_putc(&b, '\n');
            if (prompts) {
                const char *d = json_get_str(x, "description");
                sb_appendf(&b, "%s/%s%s%s", name, json_get_str(x, "name") ? json_get_str(x, "name") : "", d && *d ? " — " : "", d && *d ? d : "");
            } else sb_appendf(&b, "%s/%s", name, json_get_str(x, "uri") ? json_get_str(x, "uri") : "");
        }
        json_free(cat);
    }
    out(rt, b.len ? sb_cstr(&b) : prompts ? "No prompts available on connected servers." : "No resources available on connected servers.");
    sb_free(&b);
    json_free(servers);
}

static void test_cmd(const char *rest, slash_rt_t *rt, slash_result_t *res) {
    size_t n = strcspn(rest, " \t\n");
    char *name = xstrndup(rest, n);
    if (!*name) {
        usage(rt, res, "Usage: /mcp test <name>");
        free(name);
        return;
    }
    json_t *servers = configured(rt->app->cwd), *sv, *hit = NULL;
    json_arr_foreach(servers, i, sv) if (!hit && str_eq(json_get_str(sv, "name"), name)) hit = sv;
    if (!hit) {
        usagef(rt, res, xasprintf("Server \"%s\" not found. Run /mcp list to see configured servers.", name));
    } else {
        json_t *cfg = json_get_obj(hit, "config"), *tools = NULL;
        char *err = NULL;
        if (mcp_probe(name, cfg, json_get_int(cfg, "timeout", 30000), NULL, NULL, &tools, &err, NULL, NULL) == 0) {
            sb_t b = SB_INIT;
            sb_appendf(&b, "Server \"%s\" connected (%zu tools).", name, json_len(tools));
            json_t *t;
            json_arr_foreach(tools, i, t) sb_appendf(&b, "\n  - %s", json_get_str(t, "name"));
            out(rt, sb_cstr(&b));
            sb_free(&b);
        } else {
            usagef(rt, res, xasprintf("Connection to \"%s\" failed: %s", name, err ? err : "connection failed"));
        }
        free(err);
        json_free(tools);
    }
    json_free(servers);
    free(name);
}

static void add_cmd(const char *rest, slash_rt_t *rt, slash_result_t *res) {
    if (!*rest) {
        usage(rt, res, MCP_ADD_USAGE);
        return;
    }
    strvec_t tok = commands_parse_args(rest);
    char *name = NULL, *url = NULL, *token = NULL, *error = NULL;
    const char *scope = "project", *transport = "http";
    strvec_t cmd = {0};
    size_t i = 0;
    if (tok.n && tok.v[0][0] != '-') {
        name = xstrdup(tok.v[0]);
        i = 1;
    }
    while (i < tok.n && !error) {
        const char *a = tok.v[i], *v = i + 1 < tok.n ? tok.v[i + 1] : NULL;
        if (!strcmp(a, "--")) {
            for (size_t k = i + 1; k < tok.n; k++) strvec_push(&cmd, tok.v[k]);
            break;
        }
        if (!strcmp(a, "--scope")) {
            if (!v || (strcmp(v, "project") && strcmp(v, "user"))) error = xstrdup("Invalid --scope value. Use project or user.");
            else scope = !strcmp(v, "user") ? "user" : "project";
        } else if (!strcmp(a, "--url")) {
            if (!v) error = xstrdup("Missing value for --url.");
            else url = xstrdup(v);
        } else if (!strcmp(a, "--transport")) {
            if (!v || (strcmp(v, "http") && strcmp(v, "sse"))) error = xstrdup("Invalid --transport value. Use http or sse.");
            else transport = !strcmp(v, "sse") ? "sse" : "http";
        } else if (!strcmp(a, "--token")) {
            if (!v) error = xstrdup("Missing value for --token.");
            else token = xstrdup(v);
        } else error = xasprintf("Unknown option: %s", a);
        i += 2;
    }
    if (!error) { /* validateParsedMcpAddArgs */
        if (!cmd.n && !url) error = xstrdup("Provide --url or -- <command...> for non-interactive add. " MCP_ADD_USAGE);
        else if (!name) error = xstrdup("Server name required. Usage: /mcp add <name> ...");
        else if (cmd.n && url) error = xstrdup("Use either --url or -- <command...>, not both.");
        else if (token && !url) error = xstrdup("--token requires --url (HTTP/SSE transport).");
    }
    if (error) usage(rt, res, error);
    else {
        json_t *cfg = json_obj();
        if (cmd.n) {
            json_set_str(cfg, "type", "stdio");
            json_set_str(cfg, "command", cmd.v[0]);
            if (cmd.n > 1) {
                json_t *args = json_arr();
                for (size_t k = 1; k < cmd.n; k++) json_push_str(args, cmd.v[k]);
                json_set(cfg, "args", args);
            }
        } else {
            json_set_str(cfg, "type", transport);
            char *nu = strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8) ? xasprintf("https://%s", url) : xstrdup(url);
            json_set_str(cfg, "url", nu);
            free(nu);
            if (token) {
                json_t *h = json_obj();
                char *bt = xasprintf("Bearer %s", token);
                json_set_str(h, "Authorization", bt);
                free(bt);
                json_set(cfg, "headers", h);
            }
        }
        char *path = mcp_config_path(scope, rt->app->cwd), *err = NULL;
        if (mcp_config_add(path, name, cfg, &err) == 0) {
            char *m = xasprintf("Added MCP server \"%s\" (%s).", name, scope);
            out(rt, m);
            free(m);
        } else usagef(rt, res, xasprintf("Failed to add server: %s", err));
        free(err);
        free(path);
        json_free(cfg);
    }
    free(error);
    free(name);
    free(url);
    free(token);
    strvec_free(&cmd);
    strvec_free(&tok);
}

/* new URL(raw): origin + path (query and userinfo dropped), "(hidden)" when unparseable */
static char *redact_url(const char *raw) {
    const char *p = strstr(raw, "://");
    if (!p || p == raw) return xstrdup("(hidden)");
    for (const char *c = raw; c < p; c++)
        if (!isalnum((unsigned char)*c) && *c != '+' && *c != '-' && *c != '.') return xstrdup("(hidden)");
    char *scheme = xstrndup(raw, (size_t)(p - raw));
    for (char *c = scheme; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *auth = p + 3;
    size_t alen = strcspn(auth, "/?#");
    char *authority = xstrndup(auth, alen);
    char *at = strrchr(authority, '@');
    const char *hp = at ? at + 1 : authority;
    if (!*hp) {
        free(scheme);
        free(authority);
        return xstrdup("(hidden)");
    }
    char *host = xstrdup(hp);
    for (char *c = host; *c; c++) *c = (char)tolower((unsigned char)*c);
    char *colon = strrchr(host, ':');
    if (colon && !strchr(colon, ']') &&
        ((!strcmp(scheme, "http") && !strcmp(colon + 1, "80")) || (!strcmp(scheme, "https") && !strcmp(colon + 1, "443"))))
        *colon = 0;
    const char *path = auth + alen;
    size_t plen = *path == '/' ? strcspn(path, "?#") : 0;
    char *pathonly = plen > 1 ? xstrndup(path, plen) : xstrdup("");
    char *r = xasprintf("%s://%s%s", scheme, host, pathonly);
    free(scheme), free(authority), free(host), free(pathonly);
    return r;
}

static void list_cmd(slash_rt_t *rt) {
    const char *cwd = rt->app->cwd;
    char *up = mcp_config_path("user", cwd), *pp = mcp_config_path("project", cwd), *err = NULL;
    json_t *uc = mcp_config_read(up, &err), *pc = err ? NULL : mcp_config_read(pp, &err);
    if (err) {
        char *m = xasprintf("Failed to list MCP servers: %s", err);
        out(rt, m);
        free(m);
    } else {
        json_t *dis = mcp_config_disabled(up);
        sb_t b = SB_INIT;
        json_t *seen = json_obj();
        for (int pass = 0; pass < 2; pass++) {
            const char *k;
            json_t *v;
            json_obj_foreach(json_get_obj(pass ? pc : uc, "mcpServers"), i, k, v) {
                if (json_get(seen, k)) continue;
                json_set_bool(seen, k, true);
                bool off = disabled_cfg(v);
                json_t *x;
                json_arr_foreach(dis, q, x) off = off || str_eq(json_sv(x), k);
                const char *type = json_get_str(v, "type") ? json_get_str(v, "type") : "stdio";
                char *loc = NULL;
                if (str_eq(type, "http") || str_eq(type, "sse")) {
                    const char *raw = json_get_str(v, "url");
                    if (raw && *raw) loc = redact_url(raw);
                } else if (json_get_str(v, "command")) loc = xstrdup(json_get_str(v, "command"));
                if (b.len) sb_putc(&b, '\n');
                sb_appendf(&b, "%s | %s | %s | %s [%s]", k, type, off ? "disabled" : "enabled", loc ? loc : "(unknown)", pass ? "project" : "user");
                free(loc);
            }
        }
        out(rt, b.len ? sb_cstr(&b) : "No MCP servers configured.");
        sb_free(&b);
        json_free(seen);
        json_free(dis);
    }
    json_free(uc);
    json_free(pc);
    free(err);
    free(up);
    free(pp);
}

static void enable_cmd(bool on, const char *rest, slash_rt_t *rt, slash_result_t *res) {
    size_t n = strcspn(rest, " \t\n");
    char *name = xstrndup(rest, n);
    if (!*name) {
        usagef(rt, res, xasprintf("Usage: /mcp %s <name>", on ? "enable" : "disable"));
        free(name);
        return;
    }
    const char *cwd = rt->app->cwd;
    char *up = mcp_config_path("user", cwd), *pp = mcp_config_path("project", cwd), *err = NULL;
    json_t *uc = mcp_config_read(up, &err), *pc = err ? NULL : mcp_config_read(pp, &err);
    json_t *hit;
    if (err) {
        usagef(rt, res, xasprintf("Failed to %s MCP server: %s", on ? "enable" : "disable", err));
    } else if ((hit = json_get_obj(json_get_obj(pc, "mcpServers"), name)) || (hit = json_get_obj(json_get_obj(uc, "mcpServers"), name))) {
        bool project = json_get_obj(json_get_obj(pc, "mcpServers"), name) != NULL;
        json_t *c = json_clone(hit);
        json_set_bool(c, "enabled", on);
        if (mcp_config_update(project ? pp : up, name, c, &err) == 0) {
            char *m = xasprintf("Server \"%s\" %s (%s config).", name, on ? "enabled" : "disabled", project ? "project" : "user");
            out(rt, m);
            free(m);
        } else usagef(rt, res, xasprintf("Failed to %s MCP server: %s", on ? "enable" : "disable", err));
        json_free(c);
    } else {
        json_t *dl = mcp_config_disabled(up), *x;
        bool listed = false;
        json_arr_foreach(dl, i, x) listed = listed || str_eq(json_sv(x), name);
        json_free(dl);
        if (!on || listed) {
            if (mcp_config_set_disabled(up, name, !on, &err) == 0) {
                char *m = xasprintf("Server \"%s\" %s.", name, on ? "enabled" : "disabled");
                out(rt, m);
                free(m);
            } else usagef(rt, res, xasprintf("Failed to %s MCP server: %s", on ? "enable" : "disable", err));
        } else usagef(rt, res, xasprintf("Server \"%s\" not found in user or project config.", name));
    }
    json_free(uc);
    json_free(pc);
    free(err);
    free(up);
    free(pp);
    free(name);
}

/* parseNamedScopeArgs (whitespace split) */
static bool named_scope(const char *rest, char **name, const char **scope, char **err) {
    *name = NULL;
    *scope = "project";
    *err = NULL;
    strvec_t tok = {0};
    char *dup = xstrdup(rest), *save = NULL;
    for (char *t = strtok_r(dup, " \t\n", &save); t; t = strtok_r(NULL, " \t\n", &save)) strvec_push(&tok, t);
    free(dup);
    size_t i = 0;
    if (tok.n && tok.v[0][0] != '-') {
        *name = xstrdup(tok.v[0]);
        i = 1;
    }
    while (i < tok.n && !*err) {
        if (strcmp(tok.v[i], "--scope")) *err = xasprintf("Unknown option: %s", tok.v[i]);
        else {
            const char *v = i + 1 < tok.n ? tok.v[i + 1] : NULL;
            if (!v || (strcmp(v, "project") && strcmp(v, "user"))) *err = xstrdup("Invalid --scope value. Use project or user.");
            else *scope = !strcmp(v, "user") ? "user" : "project";
        }
        i += 2;
    }
    strvec_free(&tok);
    return *err == NULL;
}

static void remove_cmd(const char *rest, slash_rt_t *rt, slash_result_t *res) {
    char *name, *err;
    const char *scope;
    if (!named_scope(rest, &name, &scope, &err)) usage(rt, res, err);
    else if (!name) usage(rt, res, "Usage: /mcp remove <name> [--scope project|user]");
    else {
        char *path = mcp_config_path(scope, rt->app->cwd), *e2 = NULL;
        if (mcp_config_remove(path, name, &e2) == 0) {
            char *m = xasprintf("Removed server \"%s\" from %s config.", name, scope);
            out(rt, m);
            free(m);
        } else usagef(rt, res, xasprintf("Failed to remove MCP server: %s", e2));
        free(e2);
        free(path);
    }
    free(err);
    free(name);
}

/* ---- /mcp reauth as text ---- */

static void ra_auth(void *ud, const char *url, const char *launch, const char *instructions) {
    UNUSED(launch);
    slash_rt_t *rt = ud;
    char *m = xasprintf("━━━ OAuth Authorization Required ━━━\nOpening your browser to authorize. If it did not open:\n%s%s%s", url,
                        instructions && *instructions ? "\n\n" : "", instructions && *instructions ? instructions : "");
    out(rt, m);
    free(m);
    if (!env_truthy("OMC_NO_BROWSER")) {
        const char *opener = which("xdg-open") ? "xdg-open" : which("open") ? "open" : NULL;
        if (opener) {
            const char *argv[] = {opener, url, NULL};
            child_t ch;
            if (child_spawn(argv, NULL, NULL, false, &ch) == 0) child_close(&ch);
        }
    }
}

static void ra_progress(void *ud, const char *msg) { out(ud, msg); }

static void reauth_cmd(const char *rest, slash_rt_t *rt, slash_result_t *res) {
    char *name = str_trim(rest);
    if (!*name) {
        usage(rt, res, "Usage: /mcp reauth <name>");
        free(name);
        return;
    }
    app_t *a = rt->app;
    char *path = NULL, *err = NULL;
    const char *scope = "user";
    json_t *cfg = NULL;
    bool disc = false, acp_owned = false;
    /* preferManager: client-declared ACP servers live in the session's manager, in no config file */
    if (a->opts.no_mcp && a->mcp && (cfg = mcp_server_config(a->mcp, name))) {
        acp_owned = true;
        disc = true;
        path = mcp_config_path("user", a->cwd);
    } else if (mcp_find_server(name, a->cwd, a->mcp, settings_bool("mcp.enableProjectConfig", true), &path, &scope, &cfg, &disc) != 0) {
        usagef(rt, res, xasprintf("Server \"%s\" not found. Run /mcp list to see configured servers.", name));
        free(name);
        return;
    }
    const char *url = mcp_config_url(cfg);
    if (disabled_cfg(cfg)) {
        usagef(rt, res, xasprintf("Server \"%s\" is disabled. Run /mcp enable %s first.", name, name));
        goto out;
    }
    if (!url) {
        const char *remote = NULL;
        bool via = json_get_str(cfg, "command") && strstr(json_get_str(cfg, "command"), "mcp-remote");
        json_t *x;
        json_arr_foreach(json_get_arr(cfg, "args"), i, x) {
            if (!json_is_str(x)) continue;
            if (!remote && (str_starts(json_sv(x), "http://") || str_starts(json_sv(x), "https://"))) remote = json_sv(x);
            if (strstr(json_sv(x), "mcp-remote")) via = true;
        }
        char *hint = xasprintf("{ \"type\": \"http\", \"url\": \"%s\" }", remote ? remote : "<remote url>");
        usagef(rt, res, via ? xasprintf("this server proxies OAuth through mcp-remote, which caches tokens machine-wide in ~/.mcp-auth "
                                        "(shared across every OMP profile). Clear ~/.mcp-auth to force a fresh login, or replace the "
                                        "proxy with %s so OMP manages OAuth per profile.",
                                        hint)
                            : xasprintf("stdio servers manage their own credentials, so OMP has no OAuth to reauthorize. If the service "
                                        "supports OAuth over HTTP, configure it as %s instead.",
                                        hint));
        free(hint);
        goto out;
    }
    {
        json_t *probe = json_clone(cfg);
        json_del(probe, "auth");
        json_set_bool(probe, "_noOAuth", true);
        char *perr = NULL, *www = NULL;
        bool auth = false;
        int prc = mcp_probe(name, probe, json_get_int(cfg, "timeout", 30000), NULL, NULL, NULL, &perr, &auth, &www);
        json_free(probe);
        free(perr);
        json_t *ep = mcp_oauth_discover(url, prc == 0 ? NULL : www);
        if (!ep) {
            if (prc == 0) usage(rt, res, "Server connection succeeded without OAuth; reauthorization is not required.");
            else usagef(rt, res, xasprintf("Failed to reauthorize server \"%s\": Could not discover OAuth endpoints from server response.", name));
            free(www);
            goto out;
        }
        json_free(ep);
        char *m = xasprintf("Reauthorizing \"%s\"… opening your browser (5 minute timeout).", name);
        out(rt, m);
        free(m);
        json_t *flow = json_clone(cfg);
        json_del(flow, "auth");
        json_t *h = json_get_obj(flow, "headers");
        for (size_t i = json_len(h); h && i > 0; i--)
            if (!strcasecmp(h->u.obj.keys[i - 1], "authorization")) json_del(h, h->u.obj.keys[i - 1]);
        auth_login_hooks_t hk = {0};
        hk.on_auth = ra_auth;
        hk.on_progress = ra_progress;
        hk.ud = rt;
        char *ferr = NULL;
        int rc = mcp_oauth_login_hooked(url, flow, www, &hk, &ferr);
        free(www);
        if (rc != 0) {
            usagef(rt, res, xasprintf("Failed to reauthorize server \"%s\": OAuth authentication failed: %s", name, ferr ? ferr : "error"));
            free(ferr);
            json_free(flow);
            goto out;
        }
        if (!acp_owned && !disc && (json_get(cfg, "auth") || json_len(json_get_obj(cfg, "headers")) != json_len(h))) {
            char *werr = NULL;
            mcp_config_update(path, name, flow, &werr);
            free(werr);
        }
        json_free(flow);
        /* reconnect the live entry when the session keeps one (ACP) */
        const char *suffix = " Credentials apply on the next connection; restart the session if the server stays disconnected.";
        if (a->opts.no_mcp && a->mcp && mcp_has_server(a->mcp, name)) {
            mcp_state_t st = mcp_reconnect_server(a->mcp, name, 30000, NULL);
            suffix = st == MCP_CONNECTED    ? " Server reconnected."
                     : st == MCP_CONNECTING ? " Server reconnecting…"
                                            : " Server still disconnected; no background retry is armed for a server that never connected — "
                                              "restart the session (or reconnect it interactively) to use the fresh credential.";
            if (st == MCP_CONNECTED && a->agent && !agent_is_running(a->agent)) {
                mcp_sync_tools(a->mcp, a->agent);
                tools_finalize(a->agent, a->opts.tools);
                app_rebuild_system_prompt(a);
                xdnotice_sync(a, NULL);
            }
        }
        char *done = xasprintf("✓ Reauthorized \"%s\" (%s config).%s", name, scope, suffix);
        out(rt, done);
        free(done);
    }
out:
    json_free(cfg);
    free(path);
    free(err);
    free(name);
}

void slash_mcp_text(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *verb, *rest;
    slash_subcommand(c->args, &verb, &rest);
    if (!*verb || !strcmp(verb, "help")) out(rt, MCP_HELP_TEXT);
    else if (!strcmp(verb, "notifications"))
        usage(rt, res, "MCP notifications require the TUI client (live MCPManager). Use /mcp list to see server status.");
    else if (!strcmp(verb, "unauth") || !strcmp(verb, "reconnect"))
        usagef(rt, res, xasprintf("/mcp %s requires OAuth or browser flows only available in the TUI client.", verb));
    else if (!strcmp(verb, "resources")) catalog_cmd(rt, false);
    else if (!strcmp(verb, "prompts")) catalog_cmd(rt, true);
    else if (!strcmp(verb, "test")) test_cmd(rest, rt, res);
    else if (!strcmp(verb, "reauth")) reauth_cmd(rest, rt, res);
    else if (!strcmp(verb, "add")) add_cmd(rest, rt, res);
    else if (!strcmp(verb, "reload")) out(rt, "MCP runtime reload requested.");
    else if (!strcmp(verb, "list")) list_cmd(rt);
    else if (!strcmp(verb, "enable") || !strcmp(verb, "disable")) enable_cmd(!strcmp(verb, "enable"), rest, rt, res);
    else if (!strcmp(verb, "remove") || !strcmp(verb, "rm")) remove_cmd(rest, rt, res);
    else usagef(rt, res, xasprintf("Unknown /mcp subcommand: %s. Use /mcp help for available subcommands.", verb));
    free(verb);
    free(rest);
}

/* ---------- /ssh ---------- */

#define SSH_ADD_USAGE "Usage: /ssh add <name> --host <host> [--user <user>] [--port <port>] [--key <keyPath>] [--scope project|user]"

static const char *SSH_HELP_TEXT =
    "SSH host management (ACP mode)\n"
    "  /ssh add <name> --host <host> [--user <user>] [--port <port>] [--key <keyPath>] [--scope project|user]\n"
    "  /ssh list                                       List configured SSH hosts\n"
    "  /ssh remove <name> [--scope project|user]       Remove an SSH host\n"
    "  /ssh help                                        Show this help";

static void ssh_list(slash_rt_t *rt) {
    const char *cwd = rt->app->cwd;
    char *up = ssh_config_path("user", cwd), *pp = ssh_config_path("project", cwd), *err = NULL;
    json_t *uc = ssh_config_read(up, &err), *pc = err ? NULL : ssh_config_read(pp, &err);
    if (err) {
        char *m = xasprintf("Failed to list SSH hosts: %s", err);
        out(rt, m);
        free(m);
    } else {
        sb_t b = SB_INIT;
        json_t *seen = json_obj();
        for (int pass = 0; pass < 2; pass++) {
            const char *k;
            json_t *v;
            json_obj_foreach(json_get_obj(pass ? uc : pc, "hosts"), i, k, v) {
                if (json_get(seen, k)) continue;
                json_set_bool(seen, k, true);
                int64_t port = json_is_num(json_get(v, "port")) ? json_d2i(json_get_num(v, "port", 22)) : 22;
                if (b.len) sb_putc(&b, '\n');
                sb_appendf(&b, "%s | %s | %s | %lld [%s]", k, json_get_str(v, "host") ? json_get_str(v, "host") : "undefined",
                           json_get_str(v, "username") ? json_get_str(v, "username") : "-", (long long)port, pass ? "user" : "project");
            }
        }
        out(rt, b.len ? sb_cstr(&b) : "No SSH hosts configured.");
        sb_free(&b);
        json_free(seen);
    }
    json_free(uc);
    json_free(pc);
    free(err);
    free(up);
    free(pp);
}

static void ssh_add(const char *rest, slash_rt_t *rt, slash_result_t *res) {
    if (!*rest) {
        usage(rt, res, SSH_ADD_USAGE);
        return;
    }
    strvec_t tok = commands_parse_args(rest);
    const char *name = NULL, *host = NULL, *user = NULL, *key = NULL, *scope = "project";
    long port = 0;
    char *error = NULL;
    size_t i = 0;
    if (tok.n && tok.v[0][0] != '-') {
        name = tok.v[0];
        i = 1;
    }
    while (i < tok.n && !error) {
        const char *a = tok.v[i], *v = i + 1 < tok.n ? tok.v[i + 1] : NULL;
        if (!strcmp(a, "--host") || !strcmp(a, "--user") || !strcmp(a, "--key")) {
            if (!v) error = xasprintf("Missing value for %s.", a);
            else if (!strcmp(a, "--host")) host = v;
            else if (!strcmp(a, "--user")) user = v;
            else key = v;
        } else if (!strcmp(a, "--port")) {
            if (!v) error = xstrdup("Missing value for --port.");
            else {
                bool digits = *v != 0;
                for (const char *q = v; *q; q++) digits = digits && isdigit((unsigned char)*q);
                long pn = digits && strlen(v) < 9 ? strtol(v, NULL, 10) : 0;
                if (!digits || pn < 1 || pn > 65535) error = xstrdup("Invalid --port value. Must be an integer between 1 and 65535.");
                else port = pn;
            }
        } else if (!strcmp(a, "--scope")) {
            if (!v || (strcmp(v, "project") && strcmp(v, "user"))) error = xstrdup("Invalid --scope value. Use project or user.");
            else scope = !strcmp(v, "user") ? "user" : "project";
        } else error = xasprintf("Unknown option: %s", a);
        i += 2;
    }
    if (!error && !name) error = xstrdup("Host name required. Usage: /ssh add <name> --host <host> ...");
    if (!error && !host) error = xstrdup("--host is required. Usage: /ssh add <name> --host <host> ...");
    if (error) usage(rt, res, error);
    else {
        json_t *h = json_obj();
        json_set_str(h, "host", host);
        if (user && *user) json_set_str(h, "username", user);
        if (port) json_set_int(h, "port", port);
        if (key && *key) json_set_str(h, "keyPath", key);
        char *path = ssh_config_path(scope, rt->app->cwd), *err = NULL;
        if (ssh_config_add(path, name, h, &err) == 0) {
            char *m = xasprintf("Added SSH host \"%s\" (%s).", name, scope);
            out(rt, m);
            free(m);
        } else usagef(rt, res, xasprintf("Failed to add SSH host: %s", err));
        free(err);
        free(path);
        json_free(h);
    }
    free(error);
    strvec_free(&tok);
}

static void ssh_remove(const char *rest, slash_rt_t *rt, slash_result_t *res) {
    char *name, *err;
    const char *scope;
    if (!named_scope(rest, &name, &scope, &err)) usage(rt, res, err);
    else if (!name) usage(rt, res, "Usage: /ssh remove <name> [--scope project|user]");
    else {
        char *path = ssh_config_path(scope, rt->app->cwd), *e2 = NULL;
        if (ssh_config_remove(path, name, &e2) == 0) {
            char *m = xasprintf("Removed SSH host \"%s\" from %s config.", name, scope);
            out(rt, m);
            free(m);
        } else usagef(rt, res, xasprintf("Failed to remove SSH host: %s", e2));
        free(e2);
        free(path);
    }
    free(err);
    free(name);
}

void slash_ssh_text(const slash_parsed_t *c, slash_rt_t *rt, slash_result_t *res) {
    char *verb, *rest;
    slash_subcommand(c->args, &verb, &rest);
    if (!*verb || !strcmp(verb, "help")) out(rt, SSH_HELP_TEXT);
    else if (!strcmp(verb, "list")) ssh_list(rt);
    else if (!strcmp(verb, "remove") || !strcmp(verb, "rm")) ssh_remove(rest, rt, res);
    else if (!strcmp(verb, "add")) ssh_add(rest, rt, res);
    else usagef(rt, res, xasprintf("Unknown /ssh subcommand: %s. Use /ssh help for available subcommands.", verb));
    free(verb);
    free(rest);
}
