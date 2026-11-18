/* sshcmd.c - /ssh in the TUI (port of omp modes/controllers/ssh-command-controller.ts, v18.14.2): add, list,
 * remove and help over the ssh.json host files (tools/ssh.c, shared with `omc ssh`). Hosts from the legacy
 * <cwd>/ssh.json and <cwd>/.ssh.json files are listed as read-only "Discovered" groups. */
#include "ui/mcpint.h"

#include "coding/commands.h"
#include "tools/tools.h"
#include "ui/tools/tc.h"

#include <ctype.h>

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static const char *SSH_ADD_USAGE = "Usage: /ssh add <name> --host <host> [--user <user>] [--port <port>] [--key <keyPath>] "
                                   "[--desc <description>] [--compat] [--scope project|user]";

static void show_help(ui_t *u) {
    char *c = S(TUI_TC_ACCENT, "Commands:");
    char *t = xasprintf("Manage SSH host configurations for remote command execution.\n"
                        "\n"
                        "%s\n"
                        "  /ssh add <name> --host <host> [--user <user>] [--port <port>] [--key <keyPath>] [--desc <description>] "
                        "[--compat] [--scope project|user]\n"
                        "  /ssh list             List all configured SSH hosts\n"
                        "  /ssh remove <name> [--scope project|user]    Remove an SSH host (default: project)\n"
                        "  /ssh help             Show this help message\n",
                        c);
    mcpui_show_report(u, "SSH Host Management", t);
    free(c);
    free(t);
}

/* the text after "/ssh <verb>" */
static const char *after_verb(const char *text) {
    const char *p = text;
    while (isspace((unsigned char)*p)) p++;
    for (int i = 0; i < 2; i++) {
        while (*p && !isspace((unsigned char)*p)) p++;
        while (isspace((unsigned char)*p)) p++;
    }
    return p;
}

static void handle_add(ui_t *u, const char *rest) {
    if (!*rest) {
        add_notice(u, "error", SSH_ADD_USAGE);
        return;
    }
    strvec_t tok = commands_parse_args(rest);
    if (!tok.n) {
        add_notice(u, "error", SSH_ADD_USAGE);
        strvec_free(&tok);
        return;
    }
    const char *name = NULL, *host = NULL, *user = NULL, *key = NULL, *desc = NULL, *scope = "project";
    long port = 0;
    bool compat = false;
    char *error = NULL;
    size_t i = 0;
    if (tok.v[0][0] != '-') {
        name = tok.v[0];
        i = 1;
    }
    while (i < tok.n && !error) {
        const char *a = tok.v[i], *v = i + 1 < tok.n ? tok.v[i + 1] : NULL;
        const char **dst = !strcmp(a, "--host") ? &host : !strcmp(a, "--user") ? &user : !strcmp(a, "--key") ? &key
                           : !strcmp(a, "--desc") ? &desc : NULL;
        if (dst) {
            if (!v) error = xasprintf("Missing value for %s.", a);
            else *dst = v;
            i += 2;
        } else if (!strcmp(a, "--port")) {
            if (!v) error = xstrdup("Missing value for --port.");
            else {
                /* Number.parseInt: leading digits */
                char *end;
                long pn = strtol(v, &end, 10);
                if (end == v || pn < 1 || pn > 65535) error = xstrdup("Invalid --port value. Must be an integer between 1 and 65535.");
                else port = pn;
            }
            i += 2;
        } else if (!strcmp(a, "--compat")) {
            compat = true;
            i += 1;
        } else if (!strcmp(a, "--scope")) {
            if (!v || (strcmp(v, "project") && strcmp(v, "user"))) error = xstrdup("Invalid --scope value. Use project or user.");
            else scope = !strcmp(v, "user") ? "user" : "project";
            i += 2;
        } else {
            error = xasprintf("Unknown option: %s", a);
        }
    }
    if (!error && !name) error = xstrdup("Host name required. Usage: /ssh add <name> --host <host> ...");
    if (!error && !host) error = xstrdup("--host is required. Usage: /ssh add <name> --host <host> ...");
    if (error) {
        add_notice(u, "error", error);
        free(error);
        strvec_free(&tok);
        return;
    }
    json_t *h = json_obj();
    json_set_str(h, "host", host);
    if (user && *user) json_set_str(h, "username", user);
    if (port) json_set_int(h, "port", port);
    if (key && *key) json_set_str(h, "keyPath", key);
    if (desc && *desc) json_set_str(h, "description", desc);
    if (compat) json_set_bool(h, "compat", true);
    char *path = ssh_config_path(scope, u->app->cwd), *err = NULL;
    if (ssh_config_add(path, name, h, &err) != 0) {
        char *tip = NULL;
        if (strstr(err, "already exists")) {
            char *a = S(TUI_TC_ACCENT, "/ssh remove");
            tip = xasprintf("\n\nTip: Use %s first, or choose a different name.", a);
            free(a);
        }
        char *m = xasprintf("Failed to add host: %s%s", err, tip ? tip : "");
        add_notice(u, "error", m);
        free(m);
        free(tip);
    } else {
        sb_t b = SB_INIT;
        char *raw = xasprintf("+ Added SSH host \"%s\" to %s config", name, scope), *s = S(TUI_TC_SUCCESS, raw);
        sb_appendf(&b, "\n%s\n\n  Host: %s\n", s, host);
        free(raw);
        free(s);
        if (user && *user) sb_appendf(&b, "  User: %s\n", user);
        if (port) sb_appendf(&b, "  Port: %ld\n", port);
        if (key && *key) sb_appendf(&b, "  Key:  %s\n", key);
        if (desc && *desc) sb_appendf(&b, "  Desc: %s\n", desc);
        if (compat) sb_append(&b, "  Compat: true\n");
        char *a = S(TUI_TC_ACCENT, "/ssh list"), *r = xasprintf("Run %s to see all configured hosts.", a), *m = S(TUI_TC_MUTED, r);
        sb_appendf(&b, "\n%s\n", m);
        free(a), free(r), free(m);
        mcpui_show_message(u, sb_cstr(&b));
        sb_free(&b);
    }
    free(err);
    free(path);
    json_free(h);
    strvec_free(&tok);
}

/* #formatHostDetails */
static char *host_details(const json_t *h) {
    sb_t b = SB_INIT;
    int n = 0;
    if (json_get_str(h, "host") && *json_get_str(h, "host")) sb_appendf(&b, "%s%s", n++ ? ", " : "", json_get_str(h, "host"));
    if (json_get_str(h, "username") && *json_get_str(h, "username")) sb_appendf(&b, "%suser=%s", n++ ? ", " : "", json_get_str(h, "username"));
    int64_t port = json_d2i(json_get_num(h, "port", 0));
    if (port && port != 22) sb_appendf(&b, "%sport=%lld", n++ ? ", " : "", (long long)port);
    char *t = n ? xasprintf("[%s]", sb_cstr(&b)) : xstrdup("");
    sb_free(&b);
    char *r = S(TUI_TC_DIM, t);
    free(t);
    return r;
}

static void list_level(sb_t *b, const char *label, const char *path, const json_t *hosts, const char *suffix) {
    if (!json_len(hosts)) return;
    char *sp = tc_shorten_path(path), *pl = xasprintf(" (%s):", sp), *a = S(TUI_TC_ACCENT, label), *m = S(TUI_TC_MUTED, pl);
    char *sx = suffix ? S(TUI_TC_DIM, suffix) : xstrdup("");
    sb_appendf(b, "%s%s%s\n", a, m, sx);
    free(sp), free(pl), free(a), free(m), free(sx);
    const char *k;
    json_t *v;
    json_obj_foreach(hosts, i, k, v) {
        char *n = S(TUI_TC_ACCENT, k), *d = host_details(v);
        sb_appendf(b, "  %s %s\n", n, d);
        free(n), free(d);
    }
    sb_putc(b, '\n');
}

static void handle_list(ui_t *u) {
    const char *cwd = u->app->cwd;
    char *up = ssh_config_path("user", cwd), *pp = ssh_config_path("project", cwd), *err = NULL;
    json_t *uc = ssh_config_read(up, &err), *pc = err ? NULL : ssh_config_read(pp, &err);
    if (err) {
        char *m = xasprintf("Failed to list hosts: %s", err);
        add_notice(u, "error", m);
        free(m);
    } else {
        json_t *uh = json_get_obj(uc, "hosts"), *ph = json_get_obj(pc, "hosts");
        /* the legacy ssh.json / .ssh.json files (read-only) */
        const char *legacy[] = {"ssh.json", ".ssh.json"};
        json_t *disc[2] = {NULL, NULL};
        char *dpath[2] = {NULL, NULL};
        size_t ndisc = 0;
        for (int i = 0; i < 2; i++) {
            dpath[i] = path_join(cwd, legacy[i]);
            char *e2 = NULL;
            json_t *c = fs_exists(dpath[i]) ? ssh_config_read(dpath[i], &e2) : NULL;
            free(e2);
            json_t *hosts = json_obj(), *v;
            const char *k;
            json_obj_foreach(json_get_obj(c, "hosts"), q, k, v)
                if (json_is_obj(v) && !json_get(uh, k) && !json_get(ph, k)) json_set(hosts, k, json_clone(v));
            json_free(c);
            disc[i] = hosts;
            ndisc += json_len(hosts);
        }
        /* OpenSSH Config (omp 2cbe2f5f46): ~/.ssh/config and /etc/ssh/ssh_config aliases, grouped by the file
         * that defines them, below the ssh.json sources */
        strvec_t pairs = {0}, files = {0};
        ssh_openssh_aliases(&pairs);
        json_t *osh = json_obj(); /* file -> {name: {host}} */
        for (size_t i = 0; i + 1 < pairs.n; i += 2) {
            const char *nm = pairs.v[i], *file = pairs.v[i + 1];
            bool taken = json_get(uh, nm) || json_get(ph, nm) || json_get(disc[0], nm) || json_get(disc[1], nm);
            if (taken) continue;
            json_t *g = json_get_obj(osh, file);
            if (!g) {
                json_set(osh, file, json_obj());
                g = json_get_obj(osh, file);
                strvec_push(&files, file);
            }
            json_t *h = json_obj();
            json_set_str(h, "host", nm);
            json_set(g, nm, h);
            ndisc++;
        }
        strvec_free(&pairs);
        if (!json_len(uh) && !json_len(ph) && !ndisc) {
            char *m = S(TUI_TC_MUTED, "No SSH hosts configured."), *a = S(TUI_TC_ACCENT, "/ssh add");
            char *t = xasprintf("%s\n\nUse %s to add a host.", m, a);
            mcpui_show_report(u, "SSH Hosts", t);
            free(m), free(a), free(t);
        } else {
            sb_t b = SB_INIT;
            list_level(&b, "User level", up, uh, NULL);
            list_level(&b, "Project level", pp, ph, NULL);
            for (int i = 0; i < 2; i++) {
                char *sp = tc_shorten_path(dpath[i]), *lbl = xasprintf("SSH Config: %s", sp);
                if (json_len(disc[i])) {
                    char *a = S(TUI_TC_ACCENT, "Discovered"), *pl = xasprintf(" (%s):", lbl), *m = S(TUI_TC_MUTED, pl), *ro = S(TUI_TC_DIM, " read-only");
                    sb_appendf(&b, "%s%s%s\n", a, m, ro);
                    free(a), free(pl), free(m), free(ro);
                    const char *k;
                    json_t *v;
                    json_obj_foreach(disc[i], q, k, v) {
                        char *n = S(TUI_TC_ACCENT, k), *d = host_details(v);
                        sb_appendf(&b, "  %s %s\n", n, d);
                        free(n), free(d);
                    }
                    sb_putc(&b, '\n');
                }
                free(sp), free(lbl);
            }
            for (size_t f = 0; f < files.n; f++) {
                char *sp = tc_shorten_path(files.v[f]), *lbl = xasprintf("OpenSSH Config: %s", sp);
                char *a = S(TUI_TC_ACCENT, "Discovered"), *pl = xasprintf(" (%s):", lbl), *m = S(TUI_TC_MUTED, pl), *ro = S(TUI_TC_DIM, " read-only");
                sb_appendf(&b, "%s%s%s\n", a, m, ro);
                free(a), free(pl), free(m), free(ro);
                const char *k;
                json_t *v;
                json_obj_foreach(json_get_obj(osh, files.v[f]), q, k, v) {
                    char *n = S(TUI_TC_ACCENT, k), *d = host_details(v);
                    sb_appendf(&b, "  %s %s\n", n, d);
                    free(n), free(d);
                }
                sb_putc(&b, '\n');
                free(sp), free(lbl);
            }
            mcpui_show_report(u, "Configured SSH Hosts", sb_cstr(&b));
            sb_free(&b);
        }
        for (int i = 0; i < 2; i++) json_free(disc[i]), free(dpath[i]);
        json_free(osh);
        strvec_free(&files);
    }
    json_free(uc);
    json_free(pc);
    free(err);
    free(up);
    free(pp);
}

static void handle_remove(ui_t *u, const char *rest) {
    strvec_t tok = commands_parse_args(rest);
    const char *name = NULL, *scope = "project";
    char *error = NULL;
    size_t i = 0;
    if (tok.n && tok.v[0][0] != '-') {
        name = tok.v[0];
        i = 1;
    }
    while (i < tok.n && !error) {
        if (!strcmp(tok.v[i], "--scope")) {
            const char *v = i + 1 < tok.n ? tok.v[i + 1] : NULL;
            if (!v || (strcmp(v, "project") && strcmp(v, "user"))) error = xstrdup("Invalid --scope value. Use project or user.");
            else scope = !strcmp(v, "user") ? "user" : "project";
            i += 2;
        } else error = xasprintf("Unknown option: %s", tok.v[i]);
    }
    if (!error && !name) error = xstrdup("Host name required. Usage: /ssh remove <name> [--scope project|user]");
    if (error) {
        add_notice(u, "error", error);
        free(error);
        strvec_free(&tok);
        return;
    }
    char *path = ssh_config_path(scope, u->app->cwd), *err = NULL;
    json_t *c = ssh_config_read(path, &err);
    if (!c) {
        char *m = xasprintf("Failed to remove host: %s", err);
        add_notice(u, "error", m);
        free(m);
    } else if (!json_get(json_get_obj(c, "hosts"), name)) {
        char *m = xasprintf("Host \"%s\" not found in %s config.", name, scope);
        add_notice(u, "error", m);
        free(m);
    } else if (ssh_config_remove(path, name, &err) != 0) {
        char *m = xasprintf("Failed to remove host: %s", err);
        add_notice(u, "error", m);
        free(m);
    } else {
        char *raw = xasprintf("- Removed SSH host \"%s\" from %s config", name, scope), *s = S(TUI_TC_SUCCESS, raw);
        char *t = xasprintf("\n%s\n", s);
        mcpui_show_message(u, t);
        free(raw), free(s), free(t);
    }
    json_free(c);
    free(err);
    free(path);
    strvec_free(&tok);
}

void ui_ssh_command(ui_t *u, const char *text) {
    char *tt = str_trim(text);
    /* text.trim().split(/\s+/)[1] */
    const char *p = tt;
    while (*p && !isspace((unsigned char)*p)) p++;
    while (isspace((unsigned char)*p)) p++;
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n])) n++;
    char *sub = xstrndup(p, n);
    for (char *c = sub; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *rest = after_verb(tt);
    if (!*sub || !strcmp(sub, "help")) show_help(u);
    else if (!strcmp(sub, "add")) handle_add(u, rest);
    else if (!strcmp(sub, "list")) handle_list(u);
    else if (!strcmp(sub, "remove") || !strcmp(sub, "rm")) handle_remove(u, rest);
    else {
        char *m = xasprintf("Unknown subcommand: %s. Type /ssh help for usage.", sub);
        add_notice(u, "error", m);
        free(m);
    }
    free(sub);
    free(tt);
}
