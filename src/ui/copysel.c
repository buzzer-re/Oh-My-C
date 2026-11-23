/* copysel.c - the `/copy` picker (port of omp overlays/copy-selector.ts and overlays/copy-targets.ts, as
 * shipped in omp 18.14.2).
 *
 * A fullscreen page on the alternate screen over the transcript itself: the current branch replayed as the
 * transcript renders it (the rewind selector's engine, ui_tx_* in rewind.c), with a green dotted outline
 * on the turn Enter would copy. The outline's top rule advertises Right's descent into the turn's inner
 * blocks (`3 blocks →`): fenced code, `>`-quotes, bash/eval commands, tool output and http(s) links, each
 * shown as a captioned, highlighted 12-row preview whose outline steps per block. Enter copies the
 * outlined turn or block, `o` opens a link block, Left/Esc ascend, Esc closes. Captions carry clickable
 * `⧉ copy` / `↗ open` controls (SGR mouse on the alternate screen). `a` replays turns older than the
 * 600-entry window, the expand key toggles tool output, PgUp/PgDn/Home/End/Shift+arrows and the wheel
 * scroll without moving the selection.
 *
 * The copy-targets helpers (`/copy code|cmd|link`, `/open`) work on the agent's messages. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include <ctype.h>
#include <limits.h>

#define BLOCK_PREVIEW_LINES 12
#define OUTLINE_TC TUI_TC_SUCCESS

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static bool blank(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

/* ---------- copy-targets.ts ---------- */

void ui_md_blocks_free(ui_md_blocks_t *b) {
    for (size_t i = 0; i < b->n; i++) free(b->v[i].kind), free(b->v[i].lang), free(b->v[i].text);
    vec_free(b);
}

static void push_block(ui_md_blocks_t *out, const char *kind, const char *lang, char *text) {
    ui_md_block_t blk = {xstrdup(kind), lang ? xstrdup(lang) : NULL, text};
    vec_push(out, blk);
}

static void join_lines(const strvec_t *lines, size_t from, size_t to, sb_t *b) {
    for (size_t k = from; k < to; k++) {
        if (k > from) sb_putc(b, '\n');
        sb_append(b, lines->v[k]);
    }
}

static void take_block(void *ud, bool code, const char *lang, char *text) {
    push_block(ud, code ? "code" : "quote", code ? lang : NULL, text);
}

/* extractBlocks: fenced code and blockquotes in document order, split the way the message renderer draws them
 * (lexDocument): fences nested in list items count, an unclosed fence runs to the end, quotes take lazy lines */
void ui_md_extract_blocks(const char *text, ui_md_blocks_t *out) { tui_md_extract_blocks(text, take_block, out); }

/* extractLinks: the hyperlinks the renderer draws (its OSC 8 spans), http(s) only, deduplicated by href */
void ui_md_extract_links(const char *text, strvec_t *hrefs, strvec_t *labels) {
    if (!text || !strstr(text, ":")) return;
    tui_md_opts_t o;
    tui_md_opts_default(&o);
    o.hyperlinks = true;
    strvec_t rows = {0};
    tui_md_render(text, 4096, &o, &rows);
    for (size_t r = 0; r < rows.n; r++) {
        const char *p = rows.v[r];
        while ((p = strstr(p, "\x1b]8;;"))) {
            p += 5;
            const char *e = p;
            while (*e && *e != '\x07' && !(e[0] == '\x1b' && e[1] == '\\')) e++;
            if (!*e) break;
            char *href = xstrndup(p, (size_t)(e - p));
            p = *e == '\x07' ? e + 1 : e + 2;
            if (!*href) { /* a closing span */
                free(href);
                continue;
            }
            const char *close = strstr(p, TUI_LINK_CLOSE);
            char *raw = close ? xstrndup(p, (size_t)(close - p)) : xstrdup(p);
            char *plain = tui_strip_ansi(raw);
            /* the label flattened to one display row */
            sb_t lb = SB_INIT;
            bool sp = false;
            for (const char *c = plain; *c; c++) {
                if (isspace((unsigned char)*c)) sp = lb.len > 0;
                else {
                    if (sp) sb_putc(&lb, ' ');
                    sp = false;
                    sb_putc(&lb, *c);
                }
            }
            bool http = !strncasecmp(href, "http://", 7) || !strncasecmp(href, "https://", 8);
            if (http && !strvec_contains(hrefs, href)) {
                strvec_push(hrefs, href);
                strvec_push(labels, lb.len ? sb_cstr(&lb) : href);
            }
            sb_free(&lb);
            free(plain), free(raw), free(href);
            if (close) p = close + strlen(TUI_LINK_CLOSE);
        }
    }
    strvec_free(&rows);
}

/* extractEvalCode: the cells' code joined by blank lines; the first cell's language (js -> javascript) */
static char *eval_code(const json_t *args, const char **lang) {
    const json_t *cells = json_get(args, "cells");
    json_t *one = NULL;
    if (!json_is_arr(cells)) {
        if (!json_is_str(json_get(args, "code"))) return NULL;
        one = json_arr();
        json_push(one, json_clone(args));
        cells = one;
    }
    sb_t b = SB_INIT;
    bool resolved = false;
    *lang = "python";
    const json_t *c;
    json_arr_foreach(cells, i, c) {
        const char *code = json_get_str(c, "code");
        if (!code || !*code) continue;
        if (b.len) sb_append(&b, "\n\n");
        sb_append(&b, code);
        if (!resolved) {
            *lang = str_eq(json_get_str(c, "language"), "js") ? "javascript" : "python";
            resolved = true;
        }
    }
    json_free(one);
    if (!b.len) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

/* commandFromToolCall: a bash command or eval code; *kind "bash" / "eval", *lang its highlight language */
static char *tool_call_command(const json_t *tc, const char **kind, const char **lang) {
    const char *name = json_get_str(tc, "name");
    const json_t *args = json_get(tc, "arguments");
    if (str_eq(name, "bash") && json_is_str(json_get(args, "command"))) {
        *kind = "bash", *lang = "bash";
        return xstrdup(json_get_str(args, "command"));
    }
    if (str_eq(name, "eval")) {
        char *code = eval_code(args, lang);
        if (code) *kind = "eval";
        return code;
    }
    return NULL;
}

/* assistantText: concatenated text blocks of an assistant message, verbatim; NULL when blank */
static char *assistant_text(const json_t *m) {
    if (!str_eq(json_get_str(m, "role"), "assistant")) return NULL;
    sb_t b = SB_INIT;
    const json_t *c;
    json_arr_foreach(json_get_arr(m, "content"), i, c) if (str_eq(json_get_str(c, "type"), "text"))
        sb_append(&b, json_get_str(c, "text") ? json_get_str(c, "text") : "");
    if (blank(sb_cstr(&b))) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

char *ui_copy_last_code(const json_t *messages) {
    for (size_t i = json_len(messages); i-- > 0;) {
        char *t = assistant_text(json_at(messages, i));
        if (!t) continue;
        ui_md_blocks_t bl = {0};
        ui_md_extract_blocks(t, &bl);
        free(t);
        char *code = NULL;
        for (size_t k = bl.n; k-- > 0 && !code;)
            if (str_eq(bl.v[k].kind, "code")) code = xstrdup(bl.v[k].text);
        ui_md_blocks_free(&bl);
        if (code) return code;
    }
    return NULL;
}

char *ui_copy_last_command(const json_t *messages, const char **kind) {
    for (size_t i = json_len(messages); i-- > 0;) {
        const json_t *m = json_at(messages, i);
        if (!str_eq(json_get_str(m, "role"), "assistant")) continue;
        const json_t *content = json_get_arr(m, "content");
        for (size_t k = json_len(content); k-- > 0;) {
            const json_t *c = json_at(content, k);
            if (!str_eq(json_get_str(c, "type"), "toolCall")) continue;
            const char *lang;
            char *code = tool_call_command(c, kind, &lang);
            if (code) return code;
        }
    }
    return NULL;
}

char *ui_copy_last_link(const json_t *messages) {
    for (size_t i = json_len(messages); i-- > 0;) {
        char *t = assistant_text(json_at(messages, i));
        if (!t) continue;
        strvec_t hrefs = {0}, labels = {0};
        ui_md_extract_links(t, &hrefs, &labels);
        free(t);
        char *href = hrefs.n ? xstrdup(hrefs.v[hrefs.n - 1]) : NULL;
        strvec_free(&hrefs), strvec_free(&labels);
        if (href) return href;
    }
    return NULL;
}

/* ---------- transcript entries (copy-selector.ts collectBlocks / targetCopy) ---------- */

typedef struct {
    char *label, *content, *lang, *href;
    const char *kind; /* "code" | "quote" | "command" | NULL */
} block_t;
typedef VEC(block_t) blocks_t;

static void blocks_free(blocks_t *b) {
    for (size_t i = 0; i < b->n; i++) free(b->v[i].label), free(b->v[i].content), free(b->v[i].lang), free(b->v[i].href);
    vec_free(b);
}

static void add_block(blocks_t *out, char *label, char *content, const char *kind, const char *lang, const char *href) {
    block_t b = {label, content, lang && *lang ? xstrdup(lang) : NULL, href ? xstrdup(href) : NULL, kind};
    vec_push(out, b);
}

/* transcriptEntryMessage: a `custom_message` entry reads as a custom message */
static const char *entry_role(const json_t *e) {
    if (str_eq(json_get_str(e, "type"), "custom_message")) return "custom";
    return json_get_str(json_get(e, "message"), "role");
}

static const json_t *entry_msg(const json_t *e) {
    return str_eq(json_get_str(e, "type"), "custom_message") ? e : json_get(e, "message");
}

/* textContent with a separator between text blocks */
static char *text_join(const json_t *content, const char *sep) {
    if (json_is_str(content)) return xstrdup(json_sv(content));
    sb_t b = SB_INIT;
    bool first = true;
    const json_t *c;
    json_arr_foreach(content, i, c) {
        if (!str_eq(json_get_str(c, "type"), "text")) continue;
        if (!first) sb_append(&b, sep);
        sb_append(&b, json_get_str(c, "text") ? json_get_str(c, "text") : "");
        first = false;
    }
    return sb_detach(&b);
}

static char *trimmed(char *s) {
    char *t = str_trim(s);
    free(s);
    return t;
}

/* isReplyWrite: a `write` to an out:// URL; replyWriteText: its non-blank content */
static const char *reply_write_text(const json_t *call) {
    if (!str_eq(json_get_str(call, "name"), "write")) return NULL;
    const json_t *a = json_get(call, "arguments");
    const char *p = json_get_str(a, "path");
    if (!p) p = json_get_str(a, "file_path");
    if (!p || strncasecmp(p, "out:/", 5)) return NULL;
    const char *t = json_get_str(a, "content");
    return t && !blank(t) ? t : NULL;
}

static bool is_reply_write(const json_t *call) {
    if (!str_eq(json_get_str(call, "name"), "write")) return false;
    const json_t *a = json_get(call, "arguments");
    const char *p = json_get_str(a, "path");
    if (!p) p = json_get_str(a, "file_path");
    return p && !strncasecmp(p, "out:/", 5);
}

/* joinProse: text blocks concatenate verbatim, an out:// reply forms its own paragraph */
static void join_piece(sb_t *b, const char *text, bool reply, bool *after_reply) {
    if (!text || !*text) return;
    if ((reply || *after_reply) && b->len && b->s[b->len - 1] != '\n') sb_append(b, "\n\n");
    sb_append(b, text);
    *after_reply = reply;
}

/* assistantProsePieces: text blocks and out:// reply writes, in document order (omp 0ef71cbc5c) */
static void assistant_pieces(sb_t *b, const json_t *m, bool *after_reply) {
    const json_t *c;
    json_arr_foreach(json_get_arr(m, "content"), i, c) {
        const char *type = json_get_str(c, "type");
        if (str_eq(type, "text")) join_piece(b, json_get_str(c, "text"), false, after_reply);
        else if (str_eq(type, "toolCall")) join_piece(b, reply_write_text(c), true, after_reply);
    }
}

/* the prose of one assistant message (joinProse(assistantProsePieces)), trimmed */
static char *assistant_visible(const json_t *m) {
    sb_t b = SB_INIT;
    bool after = false;
    assistant_pieces(&b, m, &after);
    return trimmed(sb_detach(&b));
}

/* turnProse: the assistant message's text and replies, then the replies its tool results carry (`bash > out://`)
 * (omp a2f81bc721) */
static char *turn_prose(const json_t *entries, const ui_tx_target_t *t) {
    sb_t b = SB_INIT;
    bool after = false;
    const json_t *first = json_at(entries, t->ents.v[0]);
    if (str_eq(entry_role(first), "assistant")) assistant_pieces(&b, entry_msg(first), &after);
    for (size_t k = 0; k < t->ents.n; k++) {
        const json_t *e = json_at(entries, t->ents.v[k]);
        if (!str_eq(entry_role(e), "toolResult")) continue;
        const json_t *r;
        json_arr_foreach(json_get_arr(json_get(entry_msg(e), "details"), "replies"), i, r)
            if (json_is_str(r)) join_piece(&b, json_sv(r), true, &after);
    }
    return trimmed(sb_detach(&b));
}

/* titleTextFromSkillPrompt / userTurnDraft for a user-invoked skill prompt; NULL otherwise */
static char *skill_draft(const json_t *m) {
    if (!str_eq(json_get_str(m, "customType"), "skill-prompt") || !str_eq(json_get_str(m, "attribution"), "user")) return NULL;
    const json_t *d = json_get(m, "details");
    const char *chip = json_get_str(d, "__queueChipText"), *prompt = json_get_str(d, "prompt");
    char *t;
    if ((t = str_trim(chip ? chip : "")) && *t) return t;
    free(t);
    if ((t = str_trim(prompt ? prompt : "")) && *t) return t;
    free(t);
    char *name = str_trim(json_get_str(d, "name") ? json_get_str(d, "name") : "");
    char *args = str_trim(json_get_str(d, "args") ? json_get_str(d, "args") : "");
    char *r = *name && *args ? xasprintf("/skill:%s %s", name, args) : *name ? xasprintf("/skill:%s", name) : xstrdup(args);
    free(name), free(args);
    if (!*r) {
        free(r);
        return NULL;
    }
    return r;
}

static void push_markdown_blocks(blocks_t *out, const char *text) {
    ui_md_blocks_t bl = {0};
    ui_md_extract_blocks(text, &bl);
    for (size_t i = 0; i < bl.n; i++) {
        if (str_eq(bl.v[i].kind, "code")) {
            const char *lang = bl.v[i].lang ? bl.v[i].lang : "";
            add_block(out, *lang ? xasprintf("%s code", lang) : xstrdup("code"), xstrdup(bl.v[i].text), "code", lang, NULL);
        } else add_block(out, xstrdup("quote"), xstrdup(bl.v[i].text), "quote", NULL, NULL);
    }
    ui_md_blocks_free(&bl);
    /* links follow the message's blocks; the preview shows the whole URL on one row */
    strvec_t hrefs = {0}, labels = {0};
    ui_md_extract_links(text, &hrefs, &labels);
    for (size_t i = 0; i < hrefs.n; i++) {
        char *label = str_eq(labels.v[i], hrefs.v[i]) ? xstrdup("link") : xasprintf("link%s%s", tui_sym("sep.dot"), labels.v[i]);
        add_block(out, label, xstrdup(hrefs.v[i]), NULL, NULL, hrefs.v[i]);
    }
    strvec_free(&hrefs), strvec_free(&labels);
}

/* collectBlocks: markdown code/quotes, commands and tool output of a turn's entries */
static void collect_blocks(const json_t *entries, const ui_tx_target_t *t, blocks_t *out) {
    strvec_t reply_ids = {0}; /* reply writes */
    for (size_t k = 0; k < t->ents.n; k++) {
        const json_t *e = json_at(entries, t->ents.v[k]);
        const json_t *m = entry_msg(e);
        const char *role = entry_role(e);
        if (str_eq(role, "user")) {
            char *raw = text_join(json_get(m, "content"), "\n");
            push_markdown_blocks(out, raw);
            free(raw);
        } else if (str_eq(role, "assistant")) {
            char *vis = assistant_visible(m);
            push_markdown_blocks(out, vis);
            free(vis);
            const json_t *c;
            json_arr_foreach(json_get_arr(m, "content"), i, c) {
                if (!str_eq(json_get_str(c, "type"), "toolCall")) continue;
                if (is_reply_write(c)) { /* its text came from the call; the result holds a placeholder */
                    if (json_get_str(c, "id")) strvec_push(&reply_ids, json_get_str(c, "id"));
                    continue;
                }
                const char *kind = NULL, *lang = NULL;
                char *code = tool_call_command(c, &kind, &lang);
                if (code)
                    add_block(out, xstrdup(str_eq(kind, "bash") ? "bash command" : "eval code"), code, "command", lang, NULL);
            }
        } else if (str_eq(role, "toolResult")) {
            const char *tcid = json_get_str(m, "toolCallId");
            if (!json_get_bool(m, "isError", false) && tcid && strvec_contains(&reply_ids, tcid)) continue;
            const json_t *r;
            json_arr_foreach(json_get_arr(json_get(m, "details"), "replies"), i, r) if (json_is_str(r)) push_markdown_blocks(out, json_sv(r));
            char *text = trimmed(text_join(json_get(m, "content"), "\n"));
            if (*text) add_block(out, xasprintf("%s result", json_get_str(m, "toolName") ? json_get_str(m, "toolName") : ""), text, NULL, NULL, NULL);
            else free(text);
        } else if (str_eq(role, "bashExecution") || str_eq(role, "pythonExecution")) {
            bool py = str_eq(role, "pythonExecution");
            const char *src = json_get_str(m, py ? "code" : "command"), *output = json_get_str(m, "output");
            add_block(out, xstrdup(py ? "eval code" : "command"), xstrdup(src ? src : ""), NULL, py ? "python" : "bash", NULL);
            if (!blank(output)) add_block(out, xstrdup("output"), xstrdup(output), NULL, NULL, NULL);
        }
    }
    strvec_free(&reply_ids);
}

/* targetCopy: the clipboard payload of a whole turn, falling back to its blocks when it has no prose */
static void target_copy(const json_t *entries, const ui_tx_target_t *t, const blocks_t *blocks, char **content, char **label) {
    const json_t *e = json_at(entries, t->ents.v[0]);
    const json_t *m = entry_msg(e);
    const char *role = entry_role(e);
    *content = *label = NULL;
    if (str_eq(role, "user")) {
        *content = text_join(json_get(m, "content"), "\n");
        *label = xstrdup("user message");
    } else if (str_eq(role, "assistant")) {
        char *vis = turn_prose(entries, t);
        if (*vis) *content = vis, *label = xstrdup("assistant message");
        else free(vis);
    } else if (str_eq(role, "toolResult")) {
        sb_t b = SB_INIT;
        const json_t *r;
        json_arr_foreach(json_get_arr(json_get(m, "details"), "replies"), i, r) if (json_is_str(r) && !blank(json_sv(r)))
            sb_appendf(&b, "%s%s", b.len ? "\n\n" : "", json_sv(r));
        char *text = trimmed(text_join(json_get(m, "content"), "\n"));
        if (*text) sb_appendf(&b, "%s%s", b.len ? "\n\n" : "", text);
        free(text);
        if (b.len) *content = sb_detach(&b), *label = xasprintf("%s result", json_get_str(m, "toolName") ? json_get_str(m, "toolName") : "");
        else sb_free(&b);
    } else if (str_eq(role, "bashExecution") || str_eq(role, "pythonExecution")) {
        bool py = str_eq(role, "pythonExecution");
        const char *src = json_get_str(m, py ? "code" : "command"), *output = json_get_str(m, "output");
        sb_t b = SB_INIT;
        if (!blank(src)) sb_append(&b, src);
        if (!blank(output)) {
            if (b.len) sb_putc(&b, '\n');
            sb_append(&b, output);
        }
        *content = sb_detach(&b);
        *label = xstrdup(py ? "eval execution" : "bash execution");
    } else if (str_eq(role, "compactionSummary") || str_eq(role, "branchSummary")) {
        *content = xstrdup(json_get_str(m, "summary") ? json_get_str(m, "summary") : "");
        *label = xstrdup("summary");
    } else if (str_eq(role, "custom") || str_eq(role, "hookMessage")) {
        char *draft = str_eq(role, "custom") ? skill_draft(m) : NULL;
        if (draft) *content = draft, *label = xstrdup("user message");
        else {
            char *text = text_join(json_get(m, "content"), "\n");
            if (!blank(text)) *content = text, *label = xstrdup("message");
            else free(text);
        }
    }
    if (*content) return;
    sb_t b = SB_INIT;
    for (size_t i = 0; i < blocks->n; i++) {
        if (i) sb_append(&b, "\n\n");
        sb_append(&b, blocks->v[i].content);
    }
    *content = sb_detach(&b);
    *label = xstrdup("turn content");
}

/* ---------- the selector ---------- */

typedef struct {
    bool open; /* the ↗ open control */
    size_t block;
    int start, end;
} region_t;

typedef struct {
    tui_component_t base;
    ui_t *u;
    ui_copy_cb_t cb;
    json_t *entries;  /* the whole branch */
    json_t *replayed; /* the replayed entries (the tail, or the whole branch) */
    bool truncated;
    tui_container_t *main;
    ui_tx_targets_t targets;
    size_t selected;
    bool *visible;
    bool expanded;
    blocks_t *cache; /* per target */
    bool *collected;
    bool descended;
    size_t block_sel;
    VEC(region_t) regions;
    VEC(int) region_lines; /* composed line index of each region */
    ui_tx_view_t view;
    int body_top, body_rows, body_total;
    bool done;
} copysel_t;

static void cache_free(copysel_t *c) {
    for (size_t i = 0; c->cache && i < c->targets.n; i++) blocks_free(&c->cache[i]);
    free(c->cache);
    free(c->collected);
    c->cache = NULL;
    c->collected = NULL;
}

static void load(copysel_t *c, json_t *entries) {
    if (c->main) tui_component_destroy(&c->main->base);
    cache_free(c);
    ui_tx_targets_free(&c->targets);
    memset(&c->targets, 0, sizeof c->targets);
    json_free(c->replayed);
    c->replayed = entries;
    c->main = tui_container_new();
    ui_tx_replay(c->u, c->main, entries, &c->targets);
    c->cache = xcalloc(MAX(1, c->targets.n), sizeof *c->cache);
    c->collected = xcalloc(MAX(1, c->targets.n), sizeof *c->collected);
    free(c->visible);
    c->visible = NULL;
}

static blocks_t *blocks_for(copysel_t *c, size_t i) {
    if (!c->collected[i]) collect_blocks(c->replayed, &c->targets.v[i], &c->cache[i]);
    c->collected[i] = true;
    return &c->cache[i];
}

/* loadFullHistory: replay the whole branch, keeping the outline on the same turn */
static void load_full(copysel_t *c) {
    if (!c->truncated) return;
    char *sel = c->targets.n ? xstrdup(c->targets.v[c->selected].turn_id) : NULL;
    load(c, json_clone(c->entries));
    c->truncated = false;
    ssize_t found = -1;
    for (size_t i = 0; sel && i < c->targets.n; i++)
        if (str_eq(c->targets.v[i].turn_id, sel)) found = (ssize_t)i;
    c->selected = found >= 0 ? (size_t)found : c->targets.n ? c->targets.n - 1 : 0;
    c->descended = false;
    c->block_sel = 0;
    free(sel);
}

static int term_rows(copysel_t *c) {
    int rows = c->u->tui ? tui_term(c->u->tui)->rows : 40;
    return rows > 0 ? rows : 40;
}

static char *plural_lines(size_t n) { return xasprintf("%zu line%s", n, n == 1 ? "" : "s"); }

/* #composeBlocks: the selected turn exploded into captioned block previews, the selected one outlined;
 * caption control spans are recorded per composed line (offset by `line_off`) for clicks */
static void compose_blocks(copysel_t *c, const blocks_t *blocks, int col_w, int line_off, ui_tx_composed_t *out) {
    int inner = MAX(10, col_w - 4);
    memset(out, 0, sizeof *out);
    out->sel_start = out->sel_end = -1;
    const char *dot = tui_sym("sep.dot");
    for (size_t i = 0; i < blocks->n; i++) {
        const block_t *b = &blocks->v[i];
        bool sel = i == c->block_sel;
        /* preview rows: the first 12 lines, highlighted, width-cut, then "… +N more lines" */
        strvec_t raw = {0}, rows = {0};
        const char *p = b->content;
        for (;;) {
            const char *nl = strchr(p, '\n');
            strvec_push_own(&raw, nl ? xstrndup(p, (size_t)(nl - p)) : xstrdup(p));
            if (!nl) break;
            p = nl + 1;
        }
        size_t shown = MIN(raw.n, (size_t)BLOCK_PREVIEW_LINES);
        strvec_t styled = {0};
        if (b->lang) {
            sb_t src = SB_INIT;
            join_lines(&raw, 0, shown, &src);
            tui_highlight_code(sb_cstr(&src), b->lang, NULL, &styled);
            sb_free(&src);
        } else
            for (size_t k = 0; k < shown; k++) strvec_push(&styled, raw.v[k]);
        for (size_t k = 0; k < styled.n; k++) {
            char *rt = tui_replace_tabs(styled.v[k]);
            strvec_push_own(&rows, tui_truncate_to_width(rt, inner, NULL));
            free(rt);
        }
        if (raw.n > shown) {
            char *m = xasprintf("\xe2\x80\xa6 +%zu more lines", raw.n - shown);
            strvec_push_own(&rows, S(TUI_TC_DIM, m));
            free(m);
        }
        /* caption: two-space gutter, summary, then the controls, each preceded by two spaces */
        char *copy_t = xasprintf("%s copy", tui_sym("cmd.copy"));
        char *open_t = b->href ? xasprintf("%s open", tui_sym("cmd.share")) : NULL;
        int controls_w = tui_visible_width(copy_t) + 2 + (open_t ? tui_visible_width(open_t) + 2 : 0);
        char *lc = plural_lines(raw.n);
        char *sum_raw = xasprintf("%zu/%zu%s%s%s%s", i + 1, blocks->n, dot, b->label, dot, lc);
        char *summary = tui_truncate_to_width(sum_raw, MAX(4, inner - controls_w), NULL);
        char *cap = S(sel ? OUTLINE_TC : TUI_TC_DIM, summary);
        int cursor = 2 + tui_visible_width(summary);
        sb_t caption = SB_INIT;
        sb_append(&caption, cap);
        const char *ctl[2] = {copy_t, open_t};
        for (int k = 0; k < 2; k++) {
            if (!ctl[k]) continue;
            cursor += 2;
            region_t rg = {k == 1, i, cursor, cursor + tui_visible_width(ctl[k])};
            vec_push(&c->regions, rg);
            vec_push(&c->region_lines, line_off + (int)out->lines.n + 1);
            char *a = S(TUI_TC_ACCENT, ctl[k]);
            sb_appendf(&caption, "  %s", a);
            free(a);
            cursor += tui_visible_width(ctl[k]);
        }
        strvec_push(&out->lines, "");
        char *line = xasprintf("  %s", sb_cstr(&caption));
        if (sel) {
            out->sel_start = (int)out->lines.n;
            strvec_push_own(&out->lines, line);
            ui_tx_outline_rows(&rows, 0, rows.n, inner, OUTLINE_TC, NULL, &out->lines);
            out->sel_end = (int)out->lines.n;
        } else {
            strvec_push_own(&out->lines, line);
            for (size_t k = 0; k < rows.n; k++) strvec_push_own(&out->lines, *rows.v[k] ? xasprintf("  %s", rows.v[k]) : xstrdup(""));
        }
        sb_free(&caption);
        free(copy_t), free(open_t), free(lc), free(sum_raw), free(summary), free(cap);
        strvec_free(&raw), strvec_free(&rows), strvec_free(&styled);
    }
    strvec_push(&out->lines, "");
}

static char *header_line(void) {
    char *b = tui_bold("Copy"), *d = S(TUI_TC_DIM, "pick what to put on the clipboard");
    char *r = xasprintf("%s %s%s%s", tui_sym("cmd.copy"), b, tui_sym("sep.dot"), d);
    free(b), free(d);
    return r;
}

static void copysel_render(tui_component_t *cp, int width, strvec_t *out) {
    copysel_t *c = (copysel_t *)cp;
    int content_w = MAX(1, width - 1);
    int child_w = MAX(10, content_w - 4);
    ui_tx_rows_t rows = {0};
    ui_tx_render_children(c->u, c->expanded, c->main, 0, c->main->children.n, child_w, &rows);
    /* prepareOutline: a selection that shows nothing moves to the nearest item that does */
    free(c->visible);
    c->visible = xcalloc(MAX(1, c->targets.n), sizeof *c->visible);
    for (size_t i = 0; i < c->targets.n; i++) c->visible[i] = ui_tx_target_visible(&rows, 0, &c->targets.v[i]);
    if (c->targets.n && !c->visible[c->selected]) {
        ssize_t pick = -1;
        for (ssize_t i = (ssize_t)c->selected - 1; i >= 0 && pick < 0; i--)
            if (c->visible[i]) pick = i;
        for (size_t i = c->selected + 1; i < c->targets.n && pick < 0; i++)
            if (c->visible[i]) pick = (ssize_t)i;
        if (pick >= 0) {
            c->selected = (size_t)pick;
            c->descended = false;
            c->block_sel = 0;
        }
    }
    const ui_tx_target_t *t = c->targets.n ? &c->targets.v[c->selected] : NULL;
    blocks_t *blocks = t ? blocks_for(c, c->selected) : NULL;
    size_t nblocks = blocks ? blocks->n : 0;
    c->regions.n = c->region_lines.n = 0;
    ui_tx_composed_t comp;
    if (c->descended && t) {
        /* the turn's rendered region is replaced by its block stack */
        ui_tx_composed_t before, stack, after;
        ui_tx_compose(&rows, 0, 0, t->start, NULL, -1, content_w, NULL, OUTLINE_TC, NULL, &before);
        compose_blocks(c, blocks, content_w, (int)before.lines.n, &stack);
        ui_tx_compose(&rows, 0, t->end, c->main->children.n, NULL, -1, content_w, NULL, OUTLINE_TC, NULL, &after);
        comp = before;
        comp.sel_start = stack.sel_start >= 0 ? (int)before.lines.n + stack.sel_start : -1;
        comp.sel_end = stack.sel_end >= 0 ? (int)before.lines.n + stack.sel_end : -1;
        for (size_t i = 0; i < stack.lines.n; i++) strvec_push(&comp.lines, stack.lines.v[i]);
        for (size_t i = 0; i < after.lines.n; i++) strvec_push(&comp.lines, after.lines.v[i]);
        strvec_free(&stack.lines), strvec_free(&after.lines);
    } else {
        char *caption = NULL;
        if (nblocks) {
            char *kr = ui_key_hint("right");
            caption = xasprintf("%zu block%s %s", nblocks, nblocks == 1 ? "" : "s", kr);
            free(kr);
        }
        ui_tx_compose(&rows, 0, 0, c->main->children.n, c->targets.v, t ? (ssize_t)c->selected : -1, content_w, NULL,
                      OUTLINE_TC, caption, &comp);
        free(caption);
    }
    ui_tx_rows_free(&rows);

    char *upd, *kenter = ui_key_hint("enter"), *cancel = ui_editor_key("tui.select.cancel");
    {
        char *a = ui_editor_key("tui.select.up"), *b = ui_editor_key("tui.select.down");
        upd = xasprintf("%s/%s", a, b);
        free(a), free(b);
    }
    char *hint;
    if (c->descended && blocks) {
        const block_t *b = c->block_sel < blocks->n ? &blocks->v[c->block_sel] : NULL;
        char *ko = ui_key_hint("o"), *kl = ui_key_hint("left");
        char *open = b && b->href ? xasprintf("  %s open", ko) : xstrdup("");
        hint = xasprintf("%zu/%zu  %s block  %s/%s back  %s copy%s  click %s/%s", c->block_sel + 1, blocks->n, upd, kl, cancel,
                         kenter, open, tui_sym("cmd.copy"), tui_sym("cmd.share"));
        free(ko), free(kl), free(open);
    } else {
        char *pos = c->targets.n ? xasprintf("%zu/%zu  ", c->selected + 1, c->targets.n) : xstrdup("");
        char *kr = ui_key_hint("right"), *ka = ui_key_hint("a"), *kx = ui_editor_key("app.tools.expand");
        if (!*kx) free(kx), kx = ui_key_hint("ctrl+o");
        char *right = nblocks ? xasprintf("%s blocks  ", kr) : xstrdup("");
        char *earlier = c->truncated ? xasprintf("%s earlier turns  ", ka) : xstrdup("");
        hint = xasprintf("%s%s step  %s%s copy  %s%s expand  %s close", pos, upd, right, kenter, earlier, kx, cancel);
        free(pos), free(kr), free(ka), free(kx), free(right), free(earlier);
    }
    char *footer = S(TUI_TC_DIM, hint);
    char *anchor = NULL;
    if (t && comp.sel_start >= 0)
        anchor = c->descended ? xasprintf("copy:%s:block:%zu", t->turn_id, c->block_sel) : xasprintf("copy:%s", t->turn_id);
    char *hdr = header_line();
    ui_tx_frame(&c->view, width, term_rows(c), hdr, footer, &comp.lines, anchor, comp.sel_start, comp.sel_end, out);
    c->body_top = 3;
    c->body_rows = ui_tx_body_height(term_rows(c));
    c->body_total = (int)comp.lines.n;
    free(hdr), free(anchor), free(footer), free(hint), free(upd), free(kenter), free(cancel);
    strvec_free(&comp.lines);
}

/* ---------- input ---------- */

static void finish_pick(copysel_t *c, const char *content, const char *label) {
    if (c->done) return;
    c->done = true;
    if (c->cb.on_pick) c->cb.on_pick(c->cb.ud, content, label);
}

static void finish_open(copysel_t *c, const char *href, const char *label) {
    if (c->done || !c->cb.on_open) return;
    c->done = true;
    c->cb.on_open(c->cb.ud, href, label);
}

static void pick_outlined(copysel_t *c) {
    if (!c->targets.n) return;
    blocks_t *blocks = blocks_for(c, c->selected);
    if (c->descended) {
        if (c->block_sel < blocks->n) finish_pick(c, blocks->v[c->block_sel].content, blocks->v[c->block_sel].label);
        return;
    }
    char *content, *label;
    target_copy(c->replayed, &c->targets.v[c->selected], blocks, &content, &label);
    finish_pick(c, content, label);
    free(content), free(label);
}

static void open_selected(copysel_t *c) {
    if (!c->descended || !c->targets.n) return;
    blocks_t *blocks = blocks_for(c, c->selected);
    if (c->block_sel < blocks->n && blocks->v[c->block_sel].href)
        finish_open(c, blocks->v[c->block_sel].href, blocks->v[c->block_sel].label);
}

static void move_vertical(copysel_t *c, int delta) {
    if (c->descended) {
        blocks_t *blocks = blocks_for(c, c->selected);
        ssize_t next = (ssize_t)c->block_sel + delta;
        if (next >= 0 && (size_t)next < blocks->n) c->block_sel = (size_t)next;
        return;
    }
    for (ssize_t i = (ssize_t)c->selected + delta; i >= 0 && i < (ssize_t)c->targets.n; i += delta) {
        if (!c->visible || c->visible[i]) {
            c->selected = (size_t)i;
            return;
        }
    }
    /* stepping above the replayed tail continues into the earlier history */
    if (delta < 0 && c->truncated) {
        load_full(c);
        move_vertical(c, delta);
    }
}

static void click(copysel_t *c, int row, int col) {
    if (!c->descended) return;
    int local = row - c->body_top;
    if (local < 0 || local >= c->body_rows) return;
    int line = c->view.offset + local;
    if (line >= c->body_total) return;
    for (size_t i = 0; i < c->regions.n; i++) {
        const region_t *rg = &c->regions.v[i];
        if (c->region_lines.v[i] != line || col < rg->start || col >= rg->end) continue;
        blocks_t *blocks = blocks_for(c, c->selected);
        if (rg->block >= blocks->n) return;
        c->block_sel = rg->block;
        if (rg->open) finish_open(c, blocks->v[rg->block].href, blocks->v[rg->block].label);
        else finish_pick(c, blocks->v[rg->block].content, blocks->v[rg->block].label);
        return;
    }
}

static bool copysel_key(tui_component_t *cp, const tui_key_t *k) {
    copysel_t *c = (copysel_t *)cp;
    if (c->done) return true;
    const char *txt = tui_key_text(k);
    if (k->kind == TUI_KEY_MOUSE) {
        int row, col;
        if (ui_tx_mouse(&c->view, k, &row, &col)) click(c, row, col);
    } else if (tui_kb(k, "tui.select.cancel") || tui_key_matches(k, "escape")) {
        if (c->descended) c->descended = false, c->block_sel = 0;
        else {
            c->done = true;
            if (c->cb.on_cancel) c->cb.on_cancel(c->cb.ud);
            return true;
        }
    } else if (tui_kb(k, "app.tools.expand")) c->expanded = !c->expanded;
    else if (tui_kb(k, "tui.select.up")) move_vertical(c, -1);
    else if (tui_kb(k, "tui.select.down")) move_vertical(c, 1);
    else if (tui_key_matches(k, "right")) {
        if (!c->descended && c->targets.n && blocks_for(c, c->selected)->n) c->descended = true, c->block_sel = 0;
    } else if (tui_key_matches(k, "left")) {
        if (c->descended) c->descended = false, c->block_sel = 0;
    } else if ((str_eq(txt, "a") || str_eq(txt, "A")) && !c->descended) load_full(c);
    else if (str_eq(txt, "o") || str_eq(txt, "O")) {
        open_selected(c);
        return true;
    } else if (tui_key_matches(k, "enter") || tui_key_matches(k, "ctrl+j")) {
        pick_outlined(c);
        return true;
    } else ui_tx_scroll_key(&c->view, term_rows(c), k);
    tui_component_request_render(cp);
    return true;
}

static void copysel_destroy(tui_component_t *cp) {
    copysel_t *c = (copysel_t *)cp;
    if (c->main) tui_component_destroy(&c->main->base);
    cache_free(c);
    ui_tx_targets_free(&c->targets);
    free(c->visible);
    vec_free(&c->regions);
    vec_free(&c->region_lines);
    ui_tx_view_free(&c->view);
    json_free(c->entries);
    json_free(c->replayed);
    free(c);
}

static const tui_component_vt COPYSEL_VT = {"copy-selector", copysel_render, copysel_key, NULL, copysel_destroy};

tui_component_t *ui_copy_selector_new(ui_t *u, const ui_copy_cb_t *cb) {
    json_t *entries = ui_tx_branch_entries(u);
    if (!entries) return NULL;
    copysel_t *c = xcalloc(1, sizeof *c);
    tui_component_init(&c->base, &COPYSEL_VT);
    c->base.wants_mouse = true;
    c->u = u;
    if (cb) c->cb = *cb;
    c->entries = entries;
    json_t *tail = ui_tx_recent_entries(entries);
    c->truncated = json_len(tail) < json_len(entries);
    load(c, tail);
    if (!c->targets.n) {
        copysel_destroy(&c->base);
        return NULL;
    }
    c->selected = c->targets.n - 1;
    return &c->base;
}
