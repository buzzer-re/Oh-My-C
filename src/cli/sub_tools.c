/* sub_tools.c - tool test commands: `omc grep` (cli/grep-cli.ts), `omc read` (cli/read-cli.ts) and
 * `omc search` / `q` / `web-search` (cli/web-search-cli.ts). */
#include "cli/cli_int.h"

#include "config/settings.h"
#include "core/fs.h"
#include "text/search.h"
#include "tools/tools.h"
#include "tools/websearch.h"
#include "tui/theme.h"
#include "ui/tools/tc.h"

#include <sys/ioctl.h>
#include <unistd.h>

static void put_dim(const char *s) {
    char *d = cli_dim(s);
    printf("%s\n", d);
    free(d);
}

static void put_green(const char *s) {
    char *d = cli_green(s);
    printf("%s\n", d);
    free(d);
}

/* ---------- grep ---------- */

int cmd_grep(cparse_t *p) {
    const char *pattern = cp_arg(p, "pattern");
    const char *path = cp_arg(p, "path") ? cp_arg(p, "path") : ".";
    long long limit = 20, context = 2;
    cp_int(p, "limit", &limit);
    cp_int(p, "context", &context);
    bool count = cp_bool(p, "count"), files = cp_bool(p, "files");
    bool gitignore = !cp_bool(p, "no-gitignore");
    const char *mode = count ? "count" : files ? "filesWithMatches" : "content";
    if (!pattern || !*pattern) {
        cli_err_line("Error: Pattern is required");
        return 1;
    }
    char *cwd = get_cwd();
    char *abs = path_abs(path, cwd);
    free(cwd);
    char *line = xasprintf("Searching in: %s", abs);
    put_dim(line);
    free(line);
    line = xasprintf("Pattern: %s", pattern);
    put_dim(line);
    free(line);
    line = xasprintf("Mode: %s, Limit: %lld, Context: %lld, Gitignore: %s", mode, limit, context, gitignore ? "true" : "false");
    put_dim(line);
    free(line);
    printf("\n");
    search_opts_t o = {.pattern = pattern,
                       .path = abs,
                       .glob = cp_str(p, "glob"),
                       .mode = count ? SEARCH_COUNT : files ? SEARCH_FILES : SEARCH_CONTENT,
                       .max_count = (int)limit,
                       .hidden = true,
                       .no_ignore = !gitignore};
    if (!count && !files) o.context_before = o.context_after = (int)context;
    search_result_t r = {0};
    if (search_run(&o, &r) != 0) {
        cli_errf("Error: %s", r.error ? r.error : "search failed");
        search_result_free(&r);
        free(abs);
        return 1;
    }
    if (files && r.limit_reached) /* the native search stops at the limit: only the listed files counted */
        r.total_matches = r.files_with_matches = r.files_searched = (uint32_t)r.hits.n;
    line = xasprintf("Total matches: %u", r.total_matches);
    put_green(line);
    free(line);
    line = xasprintf("Files with matches: %u", r.files_with_matches);
    put_green(line);
    free(line);
    line = xasprintf("Files searched: %u", r.files_searched);
    put_green(line);
    free(line);
    if (r.limit_reached) {
        char *y = cli_yellow("Limit reached: true");
        printf("%s\n", y);
        free(y);
    }
    printf("\n");
    for (size_t i = 0; i < r.hits.n; i++) {
        const search_hit_t *h = &r.hits.v[i];
        char *cp = cli_cyan(h->path);
        if (o.mode == SEARCH_CONTENT) {
            if (h->is_context) {
                char *l = xasprintf("%s-%u- %s", h->path, h->line, h->text ? h->text : "");
                put_dim(l);
                free(l);
            } else {
                char nb[16];
                snprintf(nb, sizeof nb, "%u", h->line);
                char *yl = cli_yellow(nb);
                printf("%s:%s: %s\n", cp, yl, h->text ? h->text : "");
                free(yl);
            }
            const search_hit_t *nx = i + 1 < r.hits.n ? &r.hits.v[i + 1] : NULL;
            if (!nx || nx->block != h->block || strcmp(nx->path, h->path)) printf("\n");
        } else if (o.mode == SEARCH_COUNT) {
            printf("%s: %u matches\n", cp, h->count);
        } else {
            printf("%s\n", cp);
        }
        free(cp);
    }
    search_result_free(&r);
    free(abs);
    return 0;
}

/* ---------- read ---------- */

int cmd_read(cparse_t *p) {
    const char *path = cp_arg(p, "path");
    if (!path || !*path) {
        char *e = cli_red("error: path is required\n");
        fputs(e, stderr);
        free(e);
        return 1;
    }
    cli_init_runtime(NULL);
    char *cwd = get_cwd();
    tool_env_t *env = tool_env_new(cwd);
    free(cwd);
    agent_tool_t *t = tool_read_new(env);
    json_t *args = json_obj();
    json_set_str(args, "path", path);
    tool_result_t r = {0};
    tool_ctx_t ctx = {.tool_name = "read", .tool_call_id = "omc-read"};
    t->execute(t, &ctx, args, &r);
    int rc = 0;
    if (r.is_error) {
        char *text = tool_result_joined_text(&r);
        cli_err_line(text);
        free(text);
        rc = 1;
    } else {
        json_t *b;
        json_arr_foreach(r.content, i, b) {
            const char *type = json_get_str(b, "type");
            if (str_eq(type, "text")) {
                const char *s = json_get_str(b, "text");
                fputs(s ? s : "", stdout);
                if (!s || !*s || s[strlen(s) - 1] != '\n') fputc('\n', stdout);
            } else if (str_eq(type, "image")) {
                const char *data = json_get_str(b, "data");
                size_t n = 0;
                unsigned char *raw = data ? base64_decode(data, strlen(data), &n) : NULL;
                free(raw);
                char *l = xasprintf("[image content: %s, %zu bytes base64-decoded]", json_get_str(b, "mimeType"), n);
                put_dim(l);
                free(l);
            }
        }
    }
    tool_result_free(&r);
    json_free(args);
    agent_tool_free(t);
    tool_env_free(env);
    return rc;
}

/* ---------- search ---------- */

int cmd_search(cparse_t *p) {
    const strvec_t *q = cp_arg_multi(p, "query");
    char *query = q ? strvec_join(q, " ") : xstrdup("");
    if (!*query) {
        cli_err_line("Error: Query is required");
        free(query);
        return 1;
    }
    cli_init_runtime(NULL);
    const char *model = cp_str(p, "model");
    char *cwd = get_cwd();
    json_t *args = json_obj();
    json_set_str(args, "query", query);
    if (cp_str(p, "recency")) json_set_str(args, "recency", cp_str(p, "recency"));
    long long limit;
    if (cp_int(p, "limit", &limit)) json_set_num(args, "limit", (double)limit);
    /* runSearchQuery: the web role chain, or the `--model` selector as an explicit candidate */
    tool_result_t r = {0};
    bool is_error = false;
    char *text = ws_execute(args, model, NULL, NULL, NULL, &r.details, &is_error);
    tool_result_text(&r, text);
    free(text);
    r.is_error = is_error;
    int width = 100;
    struct winsize ws;
    if (isatty(1) && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) width = ws.ws_col;
    if (width < 60) width = 60;
    tc_call_t call = {.name = "web_search",
                      .args = args,
                      .content = r.content,
                      .details = r.details,
                      .has_result = true,
                      .is_error = r.is_error,
                      .expanded = !cp_bool(p, "compact"),
                      .spinner = -1,
                      .elapsed_ms = -1,
                      .exec_started = true,
                      .cwd = cwd};
    strvec_t lines = {0};
    tc_render(&call, width, &lines);
    for (size_t i = 0; i < lines.n; i++) printf("%s\n", lines.v[i]);
    strvec_free(&lines);
    int rc = r.is_error || json_get_str(r.details, "error") ? 1 : 0;
    tool_result_free(&r);
    json_free(args);
    free(cwd);
    free(query);
    return rc;
}
