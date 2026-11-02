/* out_url.c - `out://`, the agent's mid-turn reply channel (port of internal-urls/out-protocol.ts and the
 * reply half of internal-urls/url-filesystem.ts).
 *
 * Markdown written to out:// is shown to the user the way a response is shown, without ending the turn. Two
 * transports: `write out://` (the transcript draws the call's content as assistant prose) and a bash redirect
 * (`cmd > out://`), whose text arrives in the bash result's `details.replies`. omp's shell runs redirects through
 * its URL filesystem; omc runs real bash, so an unquoted `> out://…` target is rewritten to a private temp file
 * whose text is collected when the foreground command finishes. Replies are on where outRepliesActive says
 * (by default only the top-level interactive TUI session) and never for subagents. */
#include "iurl.h"

#include "config/settings.h"
#include "core/fs.h"

#include <ctype.h>
#include <unistd.h>

#define OUT_MAX_REPLY_BYTES (64 * 1024)
static const char *const OUT_DISABLED =
    "out:// is off in this session (`out.enabled`; by default on only in the interactive TUI); reply in your final "
    "message instead.";
static const char *const OUT_SUBAGENT =
    "Subagents cannot reply to the user through out://; report through your result instead.";

static bool blank(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static bool has_out_url(const char *s) {
    for (; *s; s++)
        if (!strncasecmp(s, "out:/", 5)) return true;
    return false;
}

bool out_replies_active(bool tui_transcript) {
    return settings_configured("out.enabled") ? settings_bool("out.enabled", true) : tui_transcript;
}

bool out_tui_transcript(const tool_env_t *env) { return env && env->can_prompt_user && env->subagent_depth == 0; }

static const char *out_refusal(const tool_env_t *env) {
    if (env->subagent_depth > 0) return OUT_SUBAGENT;
    if (!out_replies_active(out_tui_transcript(env))) return OUT_DISABLED;
    return NULL;
}

/* `out://…` (or the single-slash alias `out:/…`), scheme case-insensitive */
static bool is_out_url(const char *p, size_t n) {
    return n >= 5 && !strncasecmp(p, "out:/", 5);
}

int out_write(tool_env_t *env, const char *content, tool_result_t *out);
int out_write(tool_env_t *env, const char *content, tool_result_t *out) {
    const char *reason = out_refusal(env);
    if (reason) { /* replyTier denies at the approval gate: the tool-policy block error */
        tool_result_error(out, "Tool \"write\" is blocked by tool policy.\nReason: %s", reason);
        return 0;
    }
    if (!content || blank(content)) {
        tool_result_error(out, "out:// needs non-empty Markdown content.");
        return 0;
    }
    size_t bytes = strlen(content);
    if (bytes > OUT_MAX_REPLY_BYTES) {
        tool_result_error(out, "out:// replies are capped at %d KiB (got %zu bytes); shorten it or split it up.",
                          OUT_MAX_REPLY_BYTES / 1024, bytes);
        return 0;
    }
    tool_result_text(out, "Shown to the user.");
    out->useless = true;
    return 0;
}

int out_read(tool_result_t *out);
int out_read(tool_result_t *out) {
    tool_result_error(out, "out:// is write-only; reading it is not allowed. Write Markdown to it to show the user a reply.");
    return 0;
}

/* ---------- bash `> out://` redirects ---------- */

static bool word_end(char c) { return !c || isspace((unsigned char)c) || strchr(";|&<>()", c); }

char *out_redirect_rewrite(const tool_env_t *env, const char *cmd, strvec_t *files) {
    if (!cmd || !has_out_url(cmd) || env->subagent_depth > 0 || !out_replies_active(out_tui_transcript(env)))
        return NULL;
    sb_t b = SB_INIT;
    bool any = false, sq = false, dq = false;
    const char *p = cmd;
    while (*p) {
        char c = *p;
        if (sq) {
            if (c == '\'') sq = false;
            sb_putc(&b, c);
            p++;
            continue;
        }
        if (c == '\\' && p[1]) {
            sb_appendn(&b, p, 2);
            p += 2;
            continue;
        }
        if (dq) {
            if (c == '"') dq = false;
            sb_putc(&b, c);
            p++;
            continue;
        }
        if (c == '\'') sq = true;
        else if (c == '"') dq = true;
        if (c != '>') {
            sb_putc(&b, c);
            p++;
            continue;
        }
        /* redirect operator: `>`, `>>`, `>|` (any fd / `&` prefix was already copied) */
        const char *q = p + 1;
        if (*q == '>' || *q == '|') q++;
        if (*q == '&') { /* `>&N` duplicates a descriptor */
            sb_appendn(&b, p, (size_t)(q - p));
            p = q;
            continue;
        }
        const char *t = q;
        while (*t == ' ' || *t == '\t') t++;
        /* target word, possibly quoted as a whole */
        char quote = (*t == '\'' || *t == '"') ? *t : 0;
        const char *ws = quote ? t + 1 : t, *we;
        if (quote) {
            we = strchr(ws, quote);
            if (!we) we = ws + strlen(ws);
        } else {
            we = ws;
            while (!word_end(*we)) we++;
        }
        if (!is_out_url(ws, (size_t)(we - ws))) {
            sb_appendn(&b, p, (size_t)(t - p));
            p = t;
            continue;
        }
        char *tmp = fs_mktemp("omc-out-", ".md");
        if (!tmp) {
            sb_appendn(&b, p, (size_t)(t - p));
            p = t;
            continue;
        }
        sb_appendn(&b, p, (size_t)(t - p));
        sb_appendf(&b, "'%s'", tmp);
        strvec_push_own(files, tmp);
        any = true;
        p = quote && *we ? we + 1 : we;
    }
    if (!any) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

void out_redirect_collect(strvec_t *files, tool_result_t *out) {
    json_t *replies = json_arr();
    for (size_t i = 0; i < files->n; i++) {
        size_t n = 0;
        bool trunc = false;
        char *text = fs_read_limit(files->v[i], OUT_MAX_REPLY_BYTES + 1, &n, &trunc);
        /* a truncated reply would read as complete; nothing of it reaches the user (omp EFBIG) */
        if (text && !trunc && n <= OUT_MAX_REPLY_BYTES && !blank(text)) json_push_str(replies, text);
        free(text);
        unlink(files->v[i]);
    }
    if (json_len(replies)) {
        if (!out->details) out->details = json_obj();
        json_set(out->details, "replies", replies);
    } else {
        json_free(replies);
    }
    strvec_free(files);
}

void out_redirect_discard(strvec_t *files) {
    for (size_t i = 0; i < files->n; i++) unlink(files->v[i]);
    strvec_free(files);
}
