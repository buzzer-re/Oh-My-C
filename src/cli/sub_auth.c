/* sub_auth.c - credential commands: `omc login` (cli/login-cli.ts + oauth-terminal.ts), `omc token`
 * (commands/token.ts) and `omc auth export|import` (commands/auth.ts). omc keeps one credential per provider in
 * <agent dir>/auth.json (omp: agent.db rows). */
#include "cli/cli_int.h"

#include "ai/ai.h"
#include "config/auth.h"
#include "core/fs.h"
#include "core/proc.h"
#include "ui/ui.h"

#include <errno.h>
#include <unistd.h>

/* ---------- login ---------- */

static char *read_line_stdin(const char *question, bool *eof) {
    fputs(question, stdout);
    fflush(stdout);
    /* unbuffered: the OAuth paste reader shares fd 0 */
    sb_t b = SB_INIT;
    char c;
    bool any = false;
    for (;;) {
        ssize_t r = read(0, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        any = true;
        if (c == '\n') break;
        sb_putc(&b, c);
    }
    *eof = !any;
    return sb_detach(&b);
}

static void on_auth(void *ud, const char *url, const char *launch_url, const char *instructions) {
    UNUSED(ud);
    printf("\nOpen this URL in your browser:\n%s\n", url);
    if (launch_url && strcmp(launch_url, url)) printf("Local shortcut (this machine only): %s\n", launch_url);
    if (instructions && *instructions) printf("%s\n", instructions);
    printf("\n");
    fflush(stdout);
    ui_open_in_browser(url);
}

static void on_progress(void *ud, const char *msg) {
    UNUSED(ud);
    printf("%s\n", msg);
    fflush(stdout);
}

static char *on_prompt(void *ud, const auth_prompt_t *p) {
    UNUSED(ud);
    char *q = p->placeholder ? xasprintf("%s (%s): ", p->message, p->placeholder) : xasprintf("%s: ", p->message);
    bool eof;
    char *a = read_line_stdin(q, &eof);
    free(q);
    if (eof) {
        free(a);
        return NULL;
    }
    return a;
}

int cmd_login(cparse_t *p) {
    cli_init_runtime(NULL);
    const char *provider = cp_arg(p, "provider");
    json_t *all = auth_oauth_providers(), *d;
    VEC(const json_t *) providers = {0};
    json_arr_foreach(all, i, d) if (json_get_obj(d, "login")) vec_push(&providers, d);
    char *picked = NULL, *err = NULL;
    int rc = 0;
    if (!provider) {
        if (!providers.n) {
            err = xstrdup("No OAuth providers registered");
            goto fail;
        }
        printf("Select a provider:\n\n");
        for (size_t i = 0; i < providers.n; i++) {
            const char *nm = json_get_str(providers.v[i], "name");
            printf("  %zu. %s\n", i + 1, nm ? nm : json_get_str(providers.v[i], "id"));
        }
        printf("\n");
        char *q = xasprintf("Enter number (1-%zu): ", providers.n);
        bool eof;
        char *ans = read_line_stdin(q, &eof);
        free(q);
        if (eof) {
            free(ans);
            err = xstrdup("Login cancelled: stdin closed");
            goto fail;
        }
        char *end;
        long idx = strtol(ans, &end, 10) - 1;
        if (end == ans || idx < 0 || idx >= (long)providers.n) {
            err = xasprintf("Invalid selection: %s", ans);
            free(ans);
            goto fail;
        }
        free(ans);
        picked = xstrdup(json_get_str(providers.v[idx], "id"));
        provider = picked;
    }
    const json_t *info = NULL;
    for (size_t i = 0; i < providers.n; i++)
        if (str_eq(json_get_str(providers.v[i], "id"), provider)) info = providers.v[i];
    if (!info) {
        err = xasprintf("Unknown OAuth provider '%s'. Run `%s login` to pick one.", provider, CLI_BIN);
        goto fail;
    }
    auth_login_hooks_t hooks = {.on_auth = on_auth, .on_progress = on_progress, .prompt = on_prompt, .read_stdin = true};
    if (auth_login_hooked(provider, &hooks, &err) != 0) goto fail;
    const char *store = auth_store_id(provider);
    const char *name = json_get_str(info, "name") ? json_get_str(info, "name") : provider;
    auth_reload();
    json_t *l = auth_list(), *e;
    const char *who = NULL, *type = NULL;
    json_arr_foreach(l, i, e) if (str_eq(json_get_str(e, "provider"), store)) {
        who = json_get_str(e, "email");
        type = json_get_str(e, "type");
    }
    if (!type) {
        char *y = xasprintf("No credentials were stored for %s.", name), *c = cli_yellow(y);
        printf("%s\n", c);
        free(c);
        free(y);
    } else {
        char *s = who && str_eq(type, "oauth") ? xasprintf("\nLogged in to %s as %s", name, who) : xasprintf("\nLogged in to %s", name);
        char *g = cli_green(s);
        printf("%s\n", g);
        free(g);
        free(s);
        char *path = omc_agent_path("auth.json");
        char *m = xasprintf("Credentials saved to %s", path), *dm = cli_dim(m);
        printf("%s\n", dm);
        free(dm);
        free(m);
        free(path);
    }
    json_free(l);
    goto done;
fail: {
    char *m = xasprintf("Login failed: %s", err ? err : "unknown error");
    cli_err_line(m);
    free(m);
    rc = 1;
}
done:
    free(err);
    free(picked);
    vec_free(&providers);
    return rc;
}

/* ---------- token ---------- */

static json_t *load_store(void) {
    char *p = omc_agent_path("auth.json");
    size_t n;
    char *s = fs_read(p, &n);
    json_t *j = s ? json_parse(s, n, NULL) : NULL;
    free(s);
    free(p);
    if (!json_is_obj(j)) {
        json_free(j);
        j = json_obj();
    }
    return j;
}

int cmd_token(cparse_t *p) {
    cli_init_runtime(NULL);
    const char *given = cp_arg(p, "provider");
    bool managed_mcp = str_starts(given, "mcp_oauth:");
    char *provider = managed_mcp ? xstrdup(given) : str_lower(given);
    long long account = 0;
    bool has_account = cp_int(p, "account", &account);
    int rc = 0;
    if (cp_bool(p, "list") || has_account) {
        json_t *store = load_store();
        json_t *cred = json_get_obj(store, provider);
        bool oauth = cred && str_eq(json_get_str(cred, "type"), "oauth");
        if (!oauth) {
            char *m = xasprintf("No OAuth accounts found for provider \"%s\".", given);
            cli_err_line(m);
            free(m);
            fprintf(stderr, "--account/--list select among OAuth accounts; this provider has none stored.\n");
            rc = 1;
        } else if (cp_bool(p, "list")) {
            const char *base = json_get_str(cred, "email");
            if (!base) base = json_get_str(cred, "accountId");
            if (!base) base = json_get_str(cred, "projectId");
            if (!base) base = json_get_str(cred, "enterpriseUrl");
            const char *org = json_get_str(cred, "orgName") ? json_get_str(cred, "orgName") : json_get_str(cred, "orgId");
            char *b = base ? xstrdup(base) : xstrdup("credential #1");
            if (org && strcmp(org, b)) printf("1. %s (%s)\n", b, org);
            else printf("1. %s\n", b);
            free(b);
        } else if (account < 1 || account > 1) {
            char *m = xasprintf("Invalid --account %lld.", account), *r = cli_red(m);
            fprintf(stderr, "%s Provider \"%s\" has 1 OAuth account(s) (1-1).\n", r, given);
            free(r);
            free(m);
            rc = 1;
        } else {
            char *key = auth_get_api_key(provider);
            if (key) printf("%s\n", key);
            else {
                char *m = xasprintf("Could not get token for account 1 of \"%s\": no OAuth credential available", given);
                cli_err_line(m);
                free(m);
                rc = 1;
            }
            free(key);
        }
        json_free(store);
        free(provider);
        return rc;
    }
    char *key = auth_get_api_key(provider);
    if (!key || !*key) {
        char *m = xasprintf("No active credential found for provider \"%s\".", given);
        cli_err_line(m);
        free(m);
        /* every provider with a key source: stored credentials or environment */
        strvec_t active = {0};
        json_t *store = load_store();
        const char *k;
        json_t *v;
        json_obj_foreach(store, i, k, v) if (!strvec_contains(&active, k)) strvec_push(&active, k);
        json_free(store);
        for (size_t i = 0; i < catalog_count(); i++) {
            const char *pr = catalog_at(i)->provider;
            /* the registry's key sources: models.yml providers are not registry providers */
            if (strvec_contains(&active, pr) || json_get_str(catalog_provider_info(pr), "configApiKey")) continue;
            char *ek = ai_env_api_key(pr);
            if (ek) strvec_push(&active, pr);
            free(ek);
        }
        if (active.n) {
            strvec_sort(&active);
            char *j = strvec_join(&active, ", ");
            fprintf(stderr, "Configured providers: %s\n", j);
            free(j);
        }
        strvec_free(&active);
        free(key);
        free(provider);
        return 1;
    }
    if (!cp_bool(p, "raw")) {
        json_t *j = json_parse_str(key);
        const char *tok = json_is_obj(j) ? json_get_str(j, "token") : NULL;
        if (tok) {
            printf("%s\n", tok);
            json_free(j);
            free(key);
            free(provider);
            return 0;
        }
        json_free(j);
    }
    printf("%s\n", key);
    free(key);
    free(provider);
    return 0;
}

/* ---------- auth export | import ---------- */

static int auth_error(const char *fmt, const char *a) {
    char *m = xasprintf(fmt, a);
    char *e = xasprintf("error: %s", m);
    cli_err_line(e);
    free(e);
    free(m);
    return 1;
}

/* parseCredential; NULL with *err (owned) */
static json_t *parse_credential(const json_t *v, const char *where, char **err) {
    if (!json_is_obj(v)) {
        *err = xasprintf("%s: credential is not an object.", where);
        return NULL;
    }
    const char *type = json_get_str(v, "type");
    if (str_eq(type, "api_key")) {
        const char *key = json_get_str(v, "key");
        if (!key || !*key) {
            *err = xasprintf("%s: API key credential has no key.", where);
            return NULL;
        }
        json_t *c = json_obj();
        json_set_str(c, "type", "api_key");
        json_set_str(c, "key", key);
        if (str_eq(json_get_str(v, "source"), "login")) json_set_str(c, "source", "login");
        return c;
    }
    if (str_eq(type, "oauth")) {
        if (!json_get_str(v, "access") || !json_get_str(v, "refresh") || !json_is_num(json_get(v, "expires"))) {
            *err = xasprintf("%s: OAuth credential needs string access/refresh tokens and a numeric expires.", where);
            return NULL;
        }
        if (str_eq(json_get_str(v, "refresh"), "__remote__")) {
            *err = xasprintf("%s: OAuth credential has no refresh token (it comes from an auth-broker snapshot).", where);
            return NULL;
        }
        return json_clone(v);
    }
    char *t = json_get(v, "type") ? json_dump(json_get(v, "type"), 0) : xstrdup("undefined");
    *err = xasprintf("%s: unknown credential type %s.", where, t);
    free(t);
    return NULL;
}

int cmd_auth(cparse_t *p) {
    const char *action = cp_arg(p, "action");
    if (!strcmp(action, "export") && p->pos.n > 1)
        return cp_usage_error(p, "Unexpected argument \"%s\". Usage: %s auth export", p->pos.v[1], CLI_BIN);
    if (!strcmp(action, "import") && !cp_arg(p, "file"))
        return cp_usage_error(p, "Missing credential file. Usage: %s auth import <file|->", CLI_BIN);
    if (!strcmp(action, "import") && p->pos.n > 2)
        return cp_usage_error(p, "Unexpected argument \"%s\". Usage: %s auth import <file|->", p->pos.v[2], CLI_BIN);
    if (!strcmp(action, "export")) {
        json_t *store = load_store();
        json_t *doc = json_obj(), *creds = json_arr();
        json_set_num(doc, "version", 1);
        const char *k;
        json_t *v;
        int oauth = 0;
        json_obj_foreach(store, i, k, v) {
            json_t *e = json_obj();
            json_set_str(e, "provider", k);
            json_set(e, "credential", json_clone(v));
            json_push(creds, e);
            if (str_eq(json_get_str(v, "type"), "oauth")) oauth++;
        }
        json_set(doc, "credentials", creds);
        if (oauth > 0) {
            char *w = xasprintf("warning: this output holds live secrets, including %d OAuth refresh token(s). Providers "
                                "that rotate refresh tokens on use accept only the newest one: once the importing machine "
                                "refreshes, this machine may be signed out of those accounts.",
                                oauth);
            char *y = cli_yellow(w);
            fprintf(stderr, "%s\n", y);
            free(y);
            free(w);
        }
        char *s = cli_json_pretty(doc);
        printf("%s\n", s);
        free(s);
        json_free(doc);
        json_free(store);
        return 0;
    }
    const char *file = cp_arg(p, "file");
    char *text = NULL;
    size_t n = 0;
    if (!strcmp(file, "-")) {
        sb_t b = SB_INIT;
        char buf[65536];
        ssize_t r;
        while ((r = read(0, buf, sizeof buf)) > 0) sb_appendn(&b, buf, (size_t)r);
        n = b.len;
        text = sb_detach(&b);
    } else {
        text = fs_read(file, &n);
        if (!text) return auth_error("credential file not found: %s", file);
    }
    const char *source = !strcmp(file, "-") ? "stdin" : file;
    json_t *doc = json_parse(text, n, NULL);
    free(text);
    char *err = NULL;
    json_t *parsed = json_arr();
    if (!doc) err = xasprintf("%s is not valid JSON.", source);
    else if (!json_is_obj(doc)) err = xasprintf("%s is not an `%s auth export` document.", source, CLI_BIN);
    else if (!json_is_num(json_get(doc, "version")) || json_get_num(doc, "version", 0) != 1) {
        char *v = json_get(doc, "version") ? json_dump(json_get(doc, "version"), 0) : xstrdup("undefined");
        err = xasprintf("%s has export version %s; this %s reads version 1.", source, v, CLI_BIN);
        free(v);
    } else if (!json_is_arr(json_get(doc, "credentials"))) err = xasprintf("%s has no credentials array.", source);
    else {
        json_t *e;
        json_arr_foreach(json_get(doc, "credentials"), i, e) {
            char *where = xasprintf("%s: credentials[%zu]", source, i);
            const char *prov = json_get_str(e, "provider");
            if (!json_is_obj(e) || !prov || !*prov) {
                err = xasprintf("%s has no provider.", where);
                free(where);
                break;
            }
            char *w2 = xasprintf("%s (%s)", where, prov);
            json_t *c = parse_credential(json_get(e, "credential"), w2, &err);
            free(w2);
            free(where);
            if (!c) break;
            json_t *x = json_obj();
            json_set_str(x, "provider", prov);
            json_set(x, "credential", c);
            json_push(parsed, x);
        }
    }
    json_free(doc);
    if (err) {
        int rc = auth_error("%s", err);
        free(err);
        json_free(parsed);
        return rc;
    }
    if (!json_len(parsed)) {
        fprintf(stderr, "No credentials to import.\n");
        json_free(parsed);
        return 0;
    }
    /* merge: an imported credential replaces the provider's stored one */
    json_t *store = load_store();
    strvec_t provs = {0};
    json_t *e;
    json_arr_foreach(parsed, i, e) {
        const char *prov = json_get_str(e, "provider");
        json_set(store, prov, json_clone(json_get(e, "credential")));
        if (!strvec_contains(&provs, prov)) strvec_push(&provs, prov);
    }
    char *path = omc_agent_path("auth.json");
    char *s = json_dump(store, 2);
    int wrc = fs_write_atomic(path, s, strlen(s), 0600);
    free(s);
    free(path);
    int rc = 0;
    if (wrc) {
        rc = auth_error("could not write the credential store: %s", strerror(-wrc));
    } else {
        char *j = strvec_join(&provs, ", ");
        fprintf(stderr, "Imported %zu credential(s) for %s.\n", json_len(parsed), j);
        free(j);
    }
    strvec_free(&provs);
    json_free(store);
    json_free(parsed);
    return rc;
}
