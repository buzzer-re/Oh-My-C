/* chat.c - the chat transcript (port of omp packages/tui chat/ + chrome/ message views):
 *   - transcript container: blocks separated by one blank row, blank edges trimmed (TranscriptContainer)
 *   - user bubbles (UserMessageComponent), assistant turns with thinking/text/error presentation
 *     (AssistantMessageComponent), status/warning/error notices (showStatus/showWarning/showError),
 *     framed custom messages, compaction/branch summary dividers, TTSR rule notices
 *   - the pinned error banner above the editor (ErrorBannerComponent)
 *   - the working row while the model runs (Loader + shimmer working message)
 *   - transcript replay from the session tree on resume/branch */
#include "tui/motion.h"
#include "ui/ui.h"
#include "modes/interactive.h"
#include "tui/termcaps.h"
#include "ui/signals.h"
#include "ui/tools/tc.h"
#include "ui/usage.h"

#include "config/settings.h"

#include <ctype.h>
#include <math.h>
#include <time.h>

ui_t *g_ui;

const tui_theme_t *TH(void) { return tui_theme(); }

char *fg(tui_tc_t slot, const char *s) { return tui_fgs(TH(), slot, s); }

void push_wrapped(strvec_t *out, const char *text, int width, int indent) {
    strvec_t w = {0};
    tui_wrap_text_with_ansi(text, MAX(10, width - indent), &w);
    char *pad = xmalloc((size_t)indent + 1);
    memset(pad, ' ', (size_t)indent);
    pad[indent] = 0;
    for (size_t i = 0; i < w.n; i++) strvec_push_own(out, xasprintf("%s%s", pad, w.v[i]));
    free(pad);
    strvec_free(&w);
}

/* upstream formatKeyHint for the few keys the transcript names (non-mac: "Ctrl+O") */
static const char *expand_key_hint(void) {
    static char buf[32];
    const char *c = tui_sym("key.ctrl");
    snprintf(buf, sizeof buf, "%s%sO", c && *c ? c : "Ctrl", c && *c && isalpha((unsigned char)c[0]) ? "+" : "");
    return buf;
}

/* "Text" component (pi-tui Text with paddingX 1, paddingY 0): styled text wrapped at width-2, each row
 * indented by one column */
static void text_rows(strvec_t *out, const char *styled, int width) {
    int pad = tui_padding_x(1); /* tui.tight drops the column */
    strvec_t w = {0};
    tui_wrap_text_with_ansi(styled, MAX(1, width - 2 * pad), &w);
    for (size_t i = 0; i < w.n; i++) strvec_push_own(out, xasprintf("%*s%s", pad, "", w.v[i]));
    strvec_free(&w);
}

/* pi-utils formatNumber */
static void format_number(char *buf, size_t n, int64_t v) {
    double d = (double)v;
    if (v < 1000) snprintf(buf, n, "%lld", (long long)v);
    else if (v < 10000) snprintf(buf, n, "%.1fK", d / 1e3);
    else if (v < 1000000) snprintf(buf, n, "%.0fK", d / 1e3);
    else if (v < 10000000) snprintf(buf, n, "%.1fM", d / 1e6);
    else if (v < 1000000000) snprintf(buf, n, "%.0fM", d / 1e6);
    else snprintf(buf, n, "%.1fB", d / 1e9);
    char *dot = strstr(buf, ".0");
    if (dot && !isdigit((unsigned char)dot[2])) memmove(dot, dot + 2, strlen(dot + 2) + 1);
}

/* ---------------------------------------------------------------------- */
/* transcript container                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    tui_container_t c;
    bool lead_gap;
} transcript_t;

static bool plain_blank(const char *s) {
    for (; *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static int mv_unsettled_from(tui_component_t *c, const strvec_t *rows, size_t a, size_t b);

static void transcript_render(tui_component_t *cp, int width, strvec_t *out) {
    transcript_t *t = (transcript_t *)cp;
    bool any = false;
    for (size_t i = 0; i < t->c.children.n; i++) {
        tui_component_t *ch = t->c.children.v[i];
        if (ch->hidden) continue;
        strvec_t rows = {0};
        tui_component_render(ch, width, &rows);
        size_t a = 0, b = rows.n;
        while (a < b && plain_blank(rows.v[a])) a++;
        while (b > a && plain_blank(rows.v[b - 1])) b--;
        if (a < b) {
            if (any || t->lead_gap) strvec_push(out, "");
            /* a block that can still change stays out of native scrollback (omp: unfinalized blocks
             * never retire into history) */
            int unsettled = mv_unsettled_from(ch, &rows, a, b);
            if (unsettled >= 0) tui_hold_history(cp->tui, out->n + (size_t)unsettled);
            for (size_t k = a; k < b; k++) {
                strvec_push_own(out, rows.v[k]);
                rows.v[k] = NULL;
            }
            any = true;
        }
        strvec_free(&rows);
    }
}

static void transcript_invalidate(tui_component_t *cp) {
    transcript_t *t = (transcript_t *)cp;
    for (size_t i = 0; i < t->c.children.n; i++) tui_component_invalidate(t->c.children.v[i]);
}

static void transcript_destroy(tui_component_t *cp) {
    transcript_t *t = (transcript_t *)cp;
    tui_container_destroy_children(&t->c);
    free(t->c.children.v);
    free(t);
}

static const tui_component_vt TRANSCRIPT_VT = {"transcript", transcript_render, NULL, transcript_invalidate,
                                               transcript_destroy};

tui_container_t *ui_transcript_new(bool lead_gap) {
    transcript_t *t = xcalloc(1, sizeof *t);
    tui_container_init(&t->c);
    t->c.base.vt = &TRANSCRIPT_VT;
    t->lead_gap = lead_gap;
    return &t->c;
}

/* ---------------------------------------------------------------------- */
/* message views                                                           */
/* ---------------------------------------------------------------------- */

/* foreground for text over a theme background: explicit theme color, else near-black/near-white by the
 * background's luma (Theme.getFgOnBgAnsi) */
static void fg_on_bg(tui_tc_t fgslot, tui_tc_t bgslot, char out[48]) {
    const tui_theme_t *th = TH();
    const char *f = tui_fg_seq(th, fgslot);
    snprintf(out, 48, "%s", f);
    if (strcmp(f, "\x1b[39m") || !strcmp(tui_bg_seq(th, bgslot), "\x1b[49m") || th->rgb[bgslot] < 0) return;
    int64_t c = th->rgb[bgslot];
    double luma = (0.2126 * (double)((c >> 16) & 255) + 0.7152 * (double)((c >> 8) & 255) + 0.0722 * (double)(c & 255)) / 255.0;
    int r = luma > 0.5 ? 0 : 0xe5, g = luma > 0.5 ? 0 : 0xe5, b = luma > 0.5 ? 0 : 0xe7;
    if (th->mode == TUI_COLOR_TRUECOLOR) snprintf(out, 48, "\x1b[38;2;%d;%d;%dm", r, g, b);
    else snprintf(out, 48, "\x1b[38;5;%dm", tui_rgb_to_256(r, g, b));
}

static void render_user(msgview_t *v, int width, strvec_t *o) {
    char sgr[48];
    /* display-only collapse: `[Image #N, WxH]` markers show as the composer's `📷 #N` chips */
    char *shown = ui_collapse_image_markers(v->text, NULL);
    tui_markdown_t *m = tui_markdown_new(shown, 1, 1);
    if (v->synthetic) snprintf(sgr, sizeof sgr, "%s", tui_fg_seq(TH(), TUI_TC_DIM));
    else fg_on_bg(TUI_TC_USER_MESSAGE_TEXT, TUI_TC_USER_MESSAGE_BG, sgr);
    m->opts.text_sgr = sgr;
    m->opts.bg_slot = TUI_TC_USER_MESSAGE_BG;
    size_t first = o->n;
    tui_component_render(&m->base, width, o);
    tui_component_destroy(&m->base);
    /* magic keywords glow in the sent bubble, statically; code spans keep their own color */
    if (!v->synthetic) {
        for (size_t i = first; i < o->n; i++) {
            char *g = ui_keywords_paint(o->v[i], sgr, 0, sgr);
            if (g) {
                free(o->v[i]);
                o->v[i] = g;
            }
        }
    }
    /* chips keep their identity color (userBubbleColor renderReference), the bubble fg resumes after */
    if (strcmp(shown, v->text) && !v->synthetic) {
        for (int n = 1; n <= 64; n++) {
            char *label = ui_chip_label(n);
            bool any = false;
            for (size_t i = first; i < o->n; i++) any |= strstr(o->v[i], label) != NULL;
            if (any) {
                char *c = ui_attachment_sgr(n);
                size_t ll = strlen(label);
                for (size_t i = first; i < o->n; i++) {
                    if (!strstr(o->v[i], label)) continue;
                    sb_t b = SB_INIT;
                    for (const char *p = o->v[i]; *p;) {
                        const char *hit = strstr(p, label);
                        if (!hit) {
                            sb_append(&b, p);
                            break;
                        }
                        sb_appendn(&b, p, (size_t)(hit - p));
                        p = hit + ll;
                        /* the bubble fg resumes only where prose follows (padding keeps the chip color, as in omp) */
                        bool prose = false;
                        for (const char *q = p; *q && !prose;) {
                            if (*q == '\x1b') {
                                while (*q && !isalpha((unsigned char)*q) && *q != 'm') q++;
                                if (*q) q++;
                            } else prose = *q++ != ' ';
                        }
                        sb_appendf(&b, "%s\x1b[1m%s\x1b[22m%s", c, label, prose ? sgr : "");
                    }
                    free(o->v[i]);
                    o->v[i] = sb_detach(&b);
                }
                free(c);
            }
            free(label);
        }
    }
    free(shown);
}

/* ---------- user-invoked skill (chat/skill-message.ts SkillMessageComponent) ---------- */

/* theme.bgFill: `bg` re-armed after resets, padded to `width` */
static char *bg_fill(const char *line, int width, const char *bg) {
    sb_t b = SB_INIT;
    sb_append(&b, bg);
    for (const char *p = line; *p;) {
        if (!strncmp(p, "\x1b[0m", 4) || !strncmp(p, "\x1b[49m", 5)) {
            size_t l = p[2] == '0' ? 4 : 5;
            sb_appendn(&b, p, l);
            sb_append(&b, bg);
            p += l;
            continue;
        }
        sb_putc(&b, *p++);
    }
    tui_sb_spaces(&b, width - tui_visible_width(line));
    sb_append(&b, "\x1b[49m");
    return sb_detach(&b);
}

/* skillChipStyle: bold customMessageLabel over customMessageBg, then `restore` */
static char *skill_chip(const char *label, const char *restore) {
    const tui_theme_t *th = TH();
    return xasprintf("%s%s\x1b[1m%s\x1b[22m%s", tui_bg_seq(th, TUI_TC_CUSTOM_MESSAGE_BG), tui_fg_seq(th, TUI_TC_CUSTOM_MESSAGE_LABEL),
                     label, restore);
}

/* the rendered prompt's text blocks joined by newlines (#extractText) */
static char *skill_prompt_text(const json_t *msg) {
    const json_t *c = json_get(msg, "content");
    if (json_is_str(c)) return xstrdup(json_sv(c));
    sb_t b = SB_INIT;
    const json_t *blk;
    bool first = true;
    json_arr_foreach(c, i, blk) {
        if (!str_eq(json_get_str(blk, "type"), "text")) continue;
        if (!first) sb_putc(&b, '\n');
        sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
        first = false;
    }
    return sb_detach(&b);
}

/* SkillCallout: rows inside a Box(1, 1) on userMessageBg with the skill-colored rail on the left */
static void skill_callout(const strvec_t *rows, int width, strvec_t *o) {
    const tui_theme_t *th = TH();
    const char *bg = tui_bg_seq(th, TUI_TC_USER_MESSAGE_BG);
    int inner = MAX(1, width - 1);
    char *rail_fg = fg(TUI_TC_CUSTOM_MESSAGE_LABEL, tui_sym("skill.rail"));
    char *rail = xasprintf("%s%s\x1b[49m", bg, rail_fg);
    free(rail_fg);
    strvec_t all = {0};
    strvec_push(&all, "");
    for (size_t i = 0; i < rows->n; i++) {
        char *x = xasprintf(" %s", rows->v[i]);
        strvec_push_own(&all, x);
    }
    strvec_push(&all, "");
    for (size_t i = 0; i < all.n; i++) {
        char *f = bg_fill(all.v[i], inner, bg);
        strvec_push_own(o, xasprintf("%s%s", rail, f));
        free(f);
    }
    strvec_free(&all);
    free(rail);
}

static void skill_markdown(const char *text, int width, const char *sgr, strvec_t *out) {
    tui_md_opts_t mo;
    tui_md_opts_default(&mo);
    mo.text_sgr = sgr;
    mo.bg_slot = TUI_TC_USER_MESSAGE_BG;
    tui_md_render(text, width, &mo, out);
}

static void render_skill(msgview_t *v, int width, strvec_t *o) {
    const json_t *det = json_get(v->msg, "details");
    char *name = json_get_str(det, "name") ? str_trim(json_get_str(det, "name")) : xstrdup("");
    if (!*name) {
        free(name);
        name = xstrdup("unknown");
    }
    const char *args = json_get_str(det, "args");
    char *prompt = json_get_str(det, "prompt") ? xstrdup(json_get_str(det, "prompt"))
                   : args                       ? xasprintf("/skill:%s %s", name, args)
                                                : xasprintf("/skill:%s", name);
    char *label = xasprintf("%s %s", tui_sym("icon.extensionSkill"), name);
    /* collapseSkillTokens (display only): just the invoked skill's token becomes its chip */
    sb_t disp = SB_INIT;
    if (skill_tokens_allowed(prompt)) {
        size_t pos = 0, s, e;
        char *nm;
        while (skill_token_next(prompt, pos, &s, &e, &nm)) {
            sb_appendn(&disp, prompt + pos, s - pos);
            sb_append(&disp, strcmp(nm, name) ? "" : label);
            if (strcmp(nm, name)) sb_appendn(&disp, prompt + s, e - s);
            free(nm);
            pos = e;
        }
        sb_append(&disp, prompt + pos);
    } else sb_append(&disp, prompt);
    const char *display = sb_cstr(&disp);
    size_t ll = strlen(label);
    bool leading = str_starts(display, label) && (!display[ll] || isspace((unsigned char)display[ll]));
    char sgr[48];
    fg_on_bg(TUI_TC_USER_MESSAGE_TEXT, TUI_TC_USER_MESSAGE_BG, sgr);
    char *restore = xasprintf("%s%s", sgr, tui_bg_seq(TH(), TUI_TC_USER_MESSAGE_BG));
    bool expanded = g_ui && g_ui->expand_tools;
    int cw = MAX(1, width - 3); /* rail + Box padding */
    /* header: the chip, then the muted prompt size */
    char *chip = skill_chip(label, restore);
    sb_t hb = SB_INIT;
    sb_append(&hb, chip);
    if (json_get(det, "lineCount")) {
        int64_t lc = json_get_int(det, "lineCount", 0);
        char *cnt = xasprintf("%lld %s", (long long)lc, lc == 1 ? "line" : "lines");
        char *m = fg(TUI_TC_MUTED, cnt);
        sb_appendf(&hb, "  %s", m);
        free(m);
        free(cnt);
    }
    free(chip);
    strvec_t rows = {0};
    if (!leading) {
        /* inline: a plain user bubble with the chip, the callout below it once expanded */
        msgview_t tmp = {0};
        tmp.text = (char *)display;
        size_t first = o->n;
        render_user(&tmp, width, o);
        char *styled = skill_chip(label, restore);
        for (size_t i = first; i < o->n; i++) {
            if (!strstr(o->v[i], label)) continue;
            sb_t b = SB_INIT;
            for (const char *p = o->v[i]; *p;) {
                const char *hit = strstr(p, label);
                if (!hit) {
                    sb_append(&b, p);
                    break;
                }
                sb_appendn(&b, p, (size_t)(hit - p));
                sb_append(&b, styled);
                p = hit + ll;
            }
            free(o->v[i]);
            o->v[i] = sb_detach(&b);
        }
        free(styled);
        if (expanded) tui_wrap_text_with_ansi(sb_cstr(&hb), cw, &rows);
    } else {
        tui_wrap_text_with_ansi(sb_cstr(&hb), cw, &rows);
        char *body = str_trim(display + ll);
        if (*body) {
            strvec_push(&rows, "");
            skill_markdown(body, cw, sgr, &rows);
        }
        free(body);
    }
    if (expanded && (leading || rows.n)) { /* #promptSection */
        char *text = skill_prompt_text(v->msg);
        if (*text) {
            strvec_push(&rows, "");
            strvec_push_own(&rows, fg(TUI_TC_MUTED, "prompt"));
            strvec_push(&rows, "");
            skill_markdown(text, cw, sgr, &rows);
        }
        free(text);
    }
    if (rows.n) skill_callout(&rows, width, o);
    strvec_free(&rows);
    free(restore);
    sb_free(&hb);
    sb_free(&disp);
    free(label);
    free(prompt);
    free(name);
}

/* thinking-display.ts canonicalizeMessage: empty when only dots/ellipses/whitespace */
static bool canonical_nonempty(const char *s) {
    if (!s) return false;
    for (const char *p = s; *p;) {
        if (*p == '.' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        if (!strncmp(p, "\xe2\x80\xa6", 3)) {
            p += 3;
            continue;
        }
        return true;
    }
    return false;
}

static char *trim_dup(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

/* formatThinkingForDisplay: drops empty "<!-- -->" sentinel lines outside fences; prose-only mode folds a
 * fenced code block into an ellipsis on the last prose line */
static char *format_thinking(const char *text, bool prose_only) {
    strvec_t lines = str_split_lines(text, strlen(text));
    sb_t out = SB_INIT;
    bool has_comment = strstr(text, "<!--") != NULL;
    bool in_fence = false;
    char fch = 0;
    size_t flen = 0;
    size_t emitted = 0;
    ssize_t tail = -1; /* byte offset of the last non-blank output line */
    for (size_t i = 0; i < lines.n; i++) {
        const char *line = lines.v[i];
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        size_t ind = (size_t)(t - line);
        size_t run = 0;
        char c = *t;
        if (c == '`' || c == '~')
            while (t[run] == c) run++;
        if (in_fence) {
            const char *rest = t + run;
            while (*rest == ' ' || *rest == '\t') rest++;
            if (run >= flen && c == fch && ind <= 3 && !*rest) in_fence = false;
            if (!prose_only) {
                if (emitted++) sb_putc(&out, '\n');
                if (*t) tail = (ssize_t)out.len;
                sb_append(&out, line);
            }
            continue;
        }
        if (has_comment) {
            char *tr = trim_dup(line);
            bool noise = false;
            if (!strncmp(tr, "<!--", 4)) {
                const char *p = tr + 4;
                while (*p && isspace((unsigned char)*p)) p++;
                noise = !strcmp(p, "-->") || (!*p && i + 1 == lines.n);
            }
            free(tr);
            if (noise) continue;
        }
        if (run >= 3 && ind <= 3 && !(c == '`' && strchr(t + run, '`'))) {
            in_fence = true;
            fch = c;
            flen = run;
            if (prose_only) {
                if (tail < 0) {
                    if (emitted++) sb_putc(&out, '\n');
                    tail = (ssize_t)out.len;
                    sb_append(&out, "...");
                } else {
                    /* rewrite the last non-blank line: trailing "." becomes "...", else "..." appended */
                    size_t te = (size_t)tail;
                    while (te < out.len && out.s[te] != '\n') te++;
                    char *last = xstrndup(out.s + tail, te - (size_t)tail);
                    size_t ln = strlen(last);
                    while (ln > 0 && isspace((unsigned char)last[ln - 1])) ln--;
                    last[ln] = 0;
                    char *rep;
                    if (ln >= 3 && !strcmp(last + ln - 3, "...")) rep = xstrdup(last);
                    else if (ln >= 1 && last[ln - 1] == '.') rep = xasprintf("%.*s...", (int)ln - 1, last);
                    else rep = xasprintf("%s...", last);
                    out.len = (size_t)tail;
                    out.s[out.len] = 0;
                    sb_append(&out, rep);
                    emitted = 1; /* blank lines after the rewritten line are dropped with it */
                    free(rep);
                    free(last);
                }
            } else {
                if (emitted++) sb_putc(&out, '\n');
                tail = (ssize_t)out.len;
                sb_append(&out, line);
            }
            continue;
        }
        if (emitted++) sb_putc(&out, '\n');
        if (*t) tail = (ssize_t)out.len;
        sb_append(&out, line);
    }
    strvec_free(&lines);
    return sb_detach(&out);
}

/* displayable thinking text (trimmed) or NULL when the block renders nothing */
static char *thinking_display(const char *raw) {
    if (!raw || !canonical_nonempty(raw)) return NULL;
    char *f = format_thinking(raw, settings_bool("proseOnlyThinking", true));
    char *t = trim_dup(f);
    free(f);
    if (!*t) {
        free(t);
        return NULL;
    }
    return t;
}

static void md_rows(strvec_t *o, const char *text, int width, bool thinking) {
    tui_md_opts_t mo;
    tui_md_opts_default(&mo);
    mo.padding_x = tui_padding_x(1);
    if (thinking) {
        mo.text_slot = TUI_TC_THINKING_TEXT;
        mo.italic = true;
    } else {
        mo.code_indent = 0;
        /* #getProseTextStyle: the optional assistantMessageText token paints prose, not themed spans */
        if (strcmp(tui_fg_seq(NULL, TUI_TC_ASSISTANT_MESSAGE_TEXT), "\x1b[39m")) {
            mo.text_slot = TUI_TC_ASSISTANT_MESSAGE_TEXT;
            mo.prose_fg = true;
        }
    }
    tui_md_render(text, width, &mo, o);
}

/* chrome/error-block.ts formatErrorBlock: rows for a WidthAwareText(paddingX 1) */
static void error_block_rows(strvec_t *o, const char *message, int width, size_t max_rows, bool banner) {
    int pad = tui_padding_x(1);
    int cw = MAX(1, width - 2 * pad);
    int ww = MAX(1, cw - 2);
    char *clean = tui_replace_tabs(message ? message : "");
    strvec_t src = str_split_lines(clean, strlen(clean));
    strvec_t lines = {0};
    for (size_t i = 0; i < src.n; i++) {
        char *t = trim_dup(src.v[i]);
        if (*t) strvec_push_own(&lines, t);
        else free(t);
    }
    if (!lines.n) strvec_push(&lines, "Unknown error");
    strvec_t rows = {0};
    for (size_t i = 0; i < lines.n; i++) {
        char *styled;
        if (i == 0 && banner) {
            char *x = xasprintf("%s %s", tui_sym("status.error"), lines.v[i]);
            char *c = fg(TUI_TC_ERROR, x);
            styled = tui_bold(c);
            free(c);
            free(x);
        } else if (i == 0) {
            char *x = xasprintf("Error: %s", lines.v[i]);
            styled = fg(TUI_TC_ERROR, x);
            free(x);
        } else {
            styled = fg(TUI_TC_ERROR, lines.v[i]);
        }
        strvec_t w = {0};
        tui_wrap_text_with_ansi(styled, ww, &w);
        for (size_t k = 0; k < w.n; k++) strvec_push_own(&rows, xasprintf("%s%s", rows.n ? "  " : "", w.v[k]));
        strvec_free(&w);
        free(styled);
    }
    if (rows.n > max_rows) {
        size_t hidden = rows.n - max_rows;
        while (rows.n > max_rows) free(vec_pop(&rows));
        char *m = xasprintf("  \xe2\x80\xa6 +%zu more line%s (%s to expand)", hidden, hidden == 1 ? "" : "s", expand_key_hint());
        strvec_push_own(&rows, fg(TUI_TC_DIM, m));
        free(m);
    }
    for (size_t i = 0; i < rows.n; i++) {
        strvec_t w = {0};
        tui_wrap_text_with_ansi(rows.v[i], cw, &w);
        for (size_t k = 0; k < w.n; k++) strvec_push_own(o, xasprintf("%*s%s", pad, "", w.v[k]));
        strvec_free(&w);
    }
    strvec_free(&rows);
    strvec_free(&lines);
    strvec_free(&src);
    free(clean);
}

/* turn-ending error/abort label (resolveAssistantErrorPresentation); NULL when nothing renders. In the
 * interactive UI every abort without a custom reason is the user's Esc, which upstream keeps quiet. */
/* shouldRenderAbortReason + resolveAbortLabel: a user interrupt and a silent abort draw nothing; a custom reason shows
 * verbatim; a bare abort (no reason, or the generic "Request was aborted") reads "Operation aborted" */
static const char *abort_label(const json_t *msg) {
    const char *err = json_get_str(msg, "errorMessage");
    if (err && (!strcmp(err, "Interrupted by user") || !strcmp(err, "Stopped by parent agent") || !strcmp(err, "__omp.silent_abort__")))
        return NULL;
    if (!err || !*err || !strcmp(err, "Request was aborted")) return "Operation aborted";
    return err;
}

static bool has_visible_after(const json_t *content, size_t from, bool hide_thinking) {
    for (size_t i = from; i < json_len(content); i++) {
        const json_t *b = json_at(content, i);
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "toolCall")) return false;
        if (str_eq(t, "text") && canonical_nonempty(json_get_str(b, "text"))) return true;
        if (str_eq(t, "image")) return true;
        if (!hide_thinking && str_eq(t, "thinking")) {
            char *d = thinking_display(json_get_str(b, "thinking"));
            bool vis = d != NULL;
            free(d);
            if (vis) return true;
        }
    }
    return false;
}

/* ---- smooth streaming (streaming-reveal.ts): text appears at max(3, backlog/8) units per 33ms frame ---- */

static size_t cp_count(const char *s) {
    size_t n = 0;
    for (; s && *s; s++)
        if (((unsigned char)*s & 0xc0) != 0x80) n++;
    return n;
}

static char *cp_slice(const char *s, size_t units) {
    size_t i = 0, n = 0;
    while (s[i]) {
        if (((unsigned char)s[i] & 0xc0) != 0x80) {
            if (n == units) break;
            n++;
        }
        i++;
    }
    return xstrndup(s, i);
}

static bool msg_has_tools(const json_t *msg) {
    const json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) if (str_eq(json_get_str(b, "type"), "toolCall")) return true;
    return false;
}

static bool smooth_streaming(void) { return settings_bool("display.smoothStreaming", true); }

static size_t visible_units(const json_t *msg, bool hide) {
    size_t total = 0;
    const json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "text")) total += cp_count(json_get_str(b, "text"));
        else if (str_eq(t, "thinking") && !hide) {
            char *d = thinking_display(json_get_str(b, "thinking"));
            total += cp_count(d);
            free(d);
        }
    }
    return total;
}

static void reveal_stop(msgview_t *v) {
    if (v->reveal_timer && g_ui && g_ui->tui) tui_remove_timer(g_ui->tui, v->reveal_timer);
    v->reveal_timer = 0;
}

static void reveal_tick(void *ud) {
    msgview_t *v = ud;
    size_t total = visible_units(v->msg, g_ui && g_ui->hide_thinking);
    if (v->revealed < total) {
        size_t backlog = total - v->revealed, step = MAX((size_t)3, (backlog + 7) / 8);
        v->revealed = MIN(total, v->revealed + step);
        v->dirty = true;
        if (g_ui) tui_request_render(g_ui->tui);
    }
    if (v->revealed >= total) reveal_stop(v);
}

void ui_assistant_streamed(ui_t *u, msgview_t *v) {
    if (!smooth_streaming() || msg_has_tools(v->msg)) {
        v->revealed = (size_t)-1;
        reveal_stop(v);
        return;
    }
    size_t total = visible_units(v->msg, u->hide_thinking);
    if (v->revealed > total) v->revealed = total;
    if (v->revealed < total && !v->reveal_timer) v->reveal_timer = tui_add_timer(u->tui, 33, true, reveal_tick, v);
}

static const char *const THINKING_FRAMES[] = {"\xe2\x9c\xbb", "\xe2\x9c\xbc", "\xe2\x9d\x89", "\xe2\x9d\x8a",
                                              "\xe2\x9c\xba", "\xe2\x9c\xb9", "\xe2\x9c\xb8", "\xe2\x9c\xb6"};

static const tui_component_vt MV_VT;

bool ui_inline_images(void) { return settings_bool("terminal.showImages", true) && tui_image_protocol() != TUI_IMG_NONE; }

void ui_image_rows(const char *b64, const char *mime, const char *key, int width, strvec_t *out) {
    tui_image_opts_t io = tui_image_default_opts(g_ui && g_ui->tui ? tui_term(g_ui->tui)->rows : 0,
                                                 (int)settings_num("tui.maxInlineImageColumns", 100),
                                                 (int)settings_num("tui.maxInlineImageRows", 20));
    char sgr[48];
    snprintf(sgr, sizeof sgr, "%s", tui_fg_seq(TH(), TUI_TC_TOOL_OUTPUT));
    tui_image_render(b64, mime, key, NULL, width, &io, sgr, out);
}

void ui_read_images(ui_t *u, const char *id, const json_t *content) {
    json_t *imgs = json_arr();
    const json_t *b;
    json_arr_foreach(content, i, b) if (str_eq(json_get_str(b, "type"), "image") && json_get_str(b, "data") &&
                                        json_get_str(b, "mimeType")) json_push(imgs, json_clone(b));
    if (!json_len(imgs) || !id) {
        json_free(imgs);
        return;
    }
    for (size_t i = u->chat->children.n; i-- > 0;) { /* the assistant message that issued the call */
        msgview_t *v = (msgview_t *)u->chat->children.v[i];
        if (v->base.vt != &MV_VT || v->kind != MV_ASSISTANT) continue;
        json_t *blk;
        bool found = false;
        json_arr_foreach(json_get_arr(v->msg, "content"), j, blk) if (str_eq(json_get_str(blk, "id"), id)) found = true;
        if (!found) continue;
        if (!v->tool_images) v->tool_images = json_obj();
        json_set(v->tool_images, id, imgs);
        v->dirty = true;
        tui_request_render(u->tui);
        return;
    }
    json_free(imgs);
}

/* #renderToolImages: a spacer, then each grouped read's images (text placeholders without a protocol) */
static void tool_image_rows(msgview_t *v, int width, strvec_t *o) {
    if (!v->tool_images || !json_len(v->tool_images) || !settings_bool("terminal.showImages", true)) return;
    if (g_ui && g_ui->hide_tool_activity) return; /* setToolResultImagesVisible(false) */
    strvec_push(o, "");
    const char *id;
    json_t *arr;
    json_obj_foreach(v->tool_images, oi, id, arr) {
        const json_t *img;
        json_arr_foreach(arr, k, img) {
            const char *mime = json_get_str(img, "mimeType");
            if (tui_image_protocol() != TUI_IMG_NONE) {
                char *key = xasprintf("%s:%zu", id, k);
                ui_image_rows(json_get_str(img, "data"), mime, key, width, o);
                free(key);
            } else {
                char *t = xasprintf("[Image: %s]", mime);
                char *c = fg(TUI_TC_TOOL_OUTPUT, t);
                text_rows(o, c, width);
                free(c);
                free(t);
            }
        }
    }
}

static void render_assistant_body(msgview_t *v, int width, strvec_t *o) {
    json_t *content = json_get(v->msg, "content");
    bool hide = g_ui && g_ui->hide_thinking;
    json_t *shown = NULL;
    if (v->streaming && v->revealed != (size_t)-1) {
        /* buildDisplayMessage: cut the visible blocks at the reveal position */
        shown = json_clone(content);
        size_t remaining = v->revealed;
        json_t *b;
        json_arr_foreach(shown, i, b) {
            const char *t = json_get_str(b, "type");
            const char *key = str_eq(t, "text") ? "text" : str_eq(t, "thinking") && !hide ? "thinking" : NULL;
            if (!key) continue;
            char *src = !strcmp(key, "text") ? xstrdup(json_get_str(b, "text") ? json_get_str(b, "text") : "")
                                             : thinking_display(json_get_str(b, "thinking"));
            if (!src) continue;
            size_t units = cp_count(src);
            char *cut = remaining >= units ? xstrdup(src) : cp_slice(src, remaining);
            remaining = remaining > units ? remaining - units : 0;
            json_set_str(b, key, cut);
            if (!strcmp(key, "thinking")) json_set_bool(b, "displayFormatted", true);
            free(cut);
            free(src);
        }
        content = shown;
    }
    bool visible_any = has_visible_after(content, 0, hide);
    bool has_tools = false;
    int tail = 0; /* 1 text, 2 thinking: the block the model is producing right now */
    size_t n = json_len(content);
    for (size_t i = 0; i < n; i++) {
        json_t *blk = json_at(content, i);
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "toolCall")) {
            has_tools = true;
            break; /* text after a tool call renders after the tool (splitAssistantMessageToolTimeline) */
        }
        if (str_eq(t, "text")) {
            const char *tx = json_get_str(blk, "text");
            if (!canonical_nonempty(tx)) continue;
            tail = 1;
            char *tr = trim_dup(tx);
            if (v->streaming) tui_md_hold_pending_bullet(tr); /* omp d9382eb30a */
            md_rows(o, tr, width, false);
            free(tr);
        } else if (str_eq(t, "thinking")) {
            if (canonical_nonempty(json_get_str(blk, "thinking"))) tail = 2;
            if (hide) continue;
            char *d = json_get_bool(blk, "displayFormatted", false) ? trim_dup(json_get_str(blk, "thinking"))
                                                                    : thinking_display(json_get_str(blk, "thinking"));
            if (d && !*d) {
                free(d);
                d = NULL;
            }
            if (!d) continue;
            md_rows(o, d, width, true);
            free(d);
            if (has_visible_after(content, i + 1, false)) strvec_push(o, "");
        }
    }
    for (size_t i = 0; i < n && !has_tools; i++)
        if (str_eq(json_get_str(json_at(content, i), "type"), "toolCall")) has_tools = true;
    if (v->streaming && hide && tail == 2 && !has_tools) {
        if (visible_any) strvec_push(o, "");
        int f = (int)((mono_ms() / 150) % (int64_t)ARRAY_LEN(THINKING_FRAMES));
        char *g = fg(TUI_TC_THINKING_TEXT, THINKING_FRAMES[f]);
        char *l = fg(TUI_TC_MUTED, " Thinking");
        strvec_push_own(o, xasprintf(" %s%s", g, l));
        free(g);
        free(l);
    }
    json_free(shown);
    tool_image_rows(v, width, o);
    const char *stop = json_get_str(v->msg, "stopReason");
    if (v->retry_note && !v->streaming) {
        /* resolveAssistantErrorPresentation compact-recovered: a dim note in place of the error */
        strvec_push(o, "");
        char *c = fg(TUI_TC_DIM, v->retry_note);
        text_rows(o, c, width);
        free(c);
        return;
    }
    if (has_tools || v->streaming) return;
    if (str_eq(stop, "aborted")) {
        const char *label = abort_label(v->msg);
        if (label) {
            strvec_push(o, "");
            char *c = fg(TUI_TC_ERROR, label);
            text_rows(o, c, width);
            free(c);
        }
    } else if (str_eq(stop, "error") && (!v->error_pinned || (g_ui && g_ui->expand_tools))) {
        const char *err = json_get_str(v->msg, "errorMessage");
        strvec_push(o, "");
        error_block_rows(o, err && *err ? err : "Error", width, g_ui && g_ui->expand_tools ? (size_t)-1 : 8, false);
    }
}

/* CacheInvalidationMarkerComponent (MessageDividerComponent): a short dim rule and a muted label surrounded
 * by blank rows, `────────── ⊘ cache miss · 50.9K tokens` */
char *ui_cache_miss_divider(int64_t tokens, int width) {
    char num[24];
    format_number(num, sizeof num, tokens);
    const char *icon = tui_sym("icon.cacheMiss");
    char *head = icon && *icon ? xasprintf("%s cache miss", icon) : xstrdup("cache miss");
    char *dot = trim_dup(tui_sym("sep.dot"));
    char *label = tokens > 0 ? xasprintf("%s %s %s tokens", head, dot, num) : xstrdup(head);
    free(dot);
    free(head);
    width = MAX(1, width);
    int rule_w = MIN(10, width - tui_visible_width(label) - 1);
    char *painted = fg(TUI_TC_MUTED, label);
    free(label);
    if (rule_w < 1) return painted; /* truncateWhenNarrow: false keeps the bare label */
    sb_t r = SB_INIT;
    sb_repeat(&r, tui_sym("tree.horizontal"), rule_w);
    char *rule = fg(TUI_TC_DIM, sb_cstr(&r));
    sb_free(&r);
    char *line = xasprintf("%s %s", rule, painted);
    free(rule);
    free(painted);
    return line;
}

static void render_assistant(msgview_t *v, int width, strvec_t *o) {
    render_assistant_body(v, width, o);
    if (v->cache_miss > 0 && !v->streaming) { /* the marker slot trails the message's rows */
        strvec_push(o, "");
        strvec_push_own(o, ui_cache_miss_divider(v->cache_miss, width));
        strvec_push(o, "");
    }
}

static void render_notice(msgview_t *v, int width, strvec_t *o) {
    char *styled;
    if (str_eq(v->level, "styled")) { /* pre-styled TranscriptStatusBlock rows */
        strvec_t rows = str_split(v->text, "\n");
        for (size_t i = 0; i < rows.n; i++) text_rows(o, rows.v[i], width);
        strvec_free(&rows);
        return;
    }
    if (str_eq(v->level, "error")) {
        char *x = xasprintf("Error: %s", v->text);
        styled = fg(TUI_TC_ERROR, x);
        free(x);
    } else if (str_eq(v->level, "warning")) {
        char *x = xasprintf("Warning: %s", v->text);
        styled = fg(TUI_TC_WARNING, x);
        free(x);
    } else {
        styled = fg(TUI_TC_DIM, v->text);
    }
    text_rows(o, styled, width);
    free(styled);
}

/* Box(paddingX 1, paddingY 1) with a round border in `border` color and a background on the interior */
static void boxed(strvec_t *o, const strvec_t *body, int width, tui_tc_t border, int bg) {
    int inner = width - 2;
    const char *h = tui_sym("boxRound.horizontal");
    sb_t b = SB_INIT;
    sb_append(&b, tui_sym("boxRound.topLeft"));
    sb_repeat(&b, h, inner);
    sb_append(&b, tui_sym("boxRound.topRight"));
    strvec_push_own(o, fg(border, sb_cstr(&b)));
    sb_clear(&b);
    char *side = fg(border, tui_sym("boxRound.vertical"));
    for (size_t i = 0; i < body->n + 2; i++) {
        const char *row = i == 0 || i == body->n + 1 ? "" : body->v[i - 1];
        char *x = xasprintf(" %s", row);
        int pad = MAX(0, inner - tui_visible_width(x));
        sb_t r = SB_INIT;
        sb_append(&r, x);
        tui_sb_spaces(&r, pad);
        char *interior = bg >= 0 ? tui_bgs(TH(), (tui_tc_t)bg, sb_cstr(&r)) : sb_detach(&r);
        if (bg >= 0) sb_free(&r);
        strvec_push_own(o, xasprintf("%s%s%s", side, interior, side));
        free(interior);
        free(x);
    }
    sb_append(&b, tui_sym("boxRound.bottomLeft"));
    sb_repeat(&b, h, inner);
    sb_append(&b, tui_sym("boxRound.bottomRight"));
    strvec_push_own(o, fg(border, sb_cstr(&b)));
    sb_free(&b);
    free(side);
}

static char *custom_text(const json_t *msg) {
    const json_t *c = json_get(msg, "content");
    if (json_is_str(c)) return xstrdup(json_sv(c));
    sb_t b = SB_INIT;
    const json_t *p;
    bool first = true;
    json_arr_foreach(c, i, p) {
        if (!str_eq(json_get_str(p, "type"), "text")) continue;
        if (!first) sb_putc(&b, '\n');
        sb_append(&b, json_get_str(p, "text") ? json_get_str(p, "text") : "");
        first = false;
    }
    return sb_detach(&b);
}

/* FramedMessageComponent: round-bordered box on customMessageBg, "<icon> <customType>" label, markdown body */
static void render_custom(msgview_t *v, int width, strvec_t *o) {
    if (width < 5) return;
    const char *ctype = json_get_str(v->msg, "customType");
    bool hook = str_eq(json_get_str(v->msg, "role"), "hookMessage");
    strvec_t body = {0};
    int cw = MAX(1, width - 4);
    char *label = xasprintf("%s %s", tui_sym(hook ? "icon.extensionHook" : "icon.package"), ctype ? ctype : "custom-message");
    char *bl = tui_bold(label);
    char *cl = fg(TUI_TC_CUSTOM_MESSAGE_LABEL, bl);
    strvec_t w = {0};
    tui_wrap_text_with_ansi(cl, cw, &w);
    for (size_t i = 0; i < w.n; i++) {
        strvec_push_own(&body, w.v[i]);
        w.v[i] = NULL;
    }
    strvec_free(&w);
    strvec_push(&body, "");
    char *text = custom_text(v->msg);
    if (hook) {
        size_t lines = 1;
        for (const char *p = text; *p; p++) lines += *p == '\n';
        if (!v->done && lines > 5) {
            const char *p = text;
            for (int k = 0; k < 5 && p; k++) {
                p = strchr(p, '\n');
                if (p && k < 4) p++;
            }
            if (p) {
                char *x = xasprintf("%.*s\n\xe2\x80\xa6", (int)(p - text), text);
                free(text);
                text = x;
            }
        }
    }
    tui_md_opts_t mo;
    tui_md_opts_default(&mo);
    mo.text_slot = TUI_TC_CUSTOM_MESSAGE_TEXT;
    tui_md_render(text, cw, &mo, &body);
    boxed(o, &body, width, TUI_TC_BORDER_MUTED, TUI_TC_CUSTOM_MESSAGE_BG);
    strvec_free(&body);
    free(text);
    free(cl);
    free(bl);
    free(label);
}

/* Number.prototype.toLocaleString (en-US): 1234567 -> 1,234,567 */
static void locale_number(char *buf, size_t n, int64_t v) {
    char raw[32];
    snprintf(raw, sizeof raw, "%lld", (long long)(v < 0 ? -v : v));
    size_t len = strlen(raw), k = 0;
    if (v < 0 && k + 1 < n) buf[k++] = '-';
    for (size_t i = 0; i < len && k + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) buf[k++] = ',';
        buf[k++] = raw[i];
    }
    buf[k] = 0;
}

/* compaction / branch summary: a centered divider; Ctrl+O expands the summary into a box */
static void render_summary(msgview_t *v, int width, strvec_t *o) {
    char label[160];
    bool branch = str_eq(v->level, "branch");
    if (branch) {
        snprintf(label, sizeof label, "%s branch", tui_sym("icon.branch"));
    } else {
        /* COMPACTION_METHOD_LABELS; legacy / extension entries read "compacted" */
        const char *method = json_get_str(v->msg, "method");
        const char *name = str_eq(method, "remote")        ? "remote-compacted"
                           : str_eq(method, "soft")        ? "soft-compacted"
                           : str_eq(method, "handoff")     ? "handed-off"
                           : str_eq(method, "snapcompact") ? "snap-compacted"
                           : str_eq(method, "shake")       ? "shaken"
                                                           : "compacted";
        int64_t before = json_get_int(v->msg, "tokensBefore", 0), after = json_get_int(v->msg, "tokensAfter", -1);
        snprintf(label, sizeof label, "%s %s", tui_sym("icon.camera"), name);
        if (after >= 0 && before > 0) {
            char a[24], b[24];
            format_number(a, sizeof a, before);
            format_number(b, sizeof b, after);
            size_t l = strlen(label);
            snprintf(label + l, sizeof label - l, "%s%s\xe2\x86\x92%s", tui_sym("sep.dot"), a, b);
        }
        if (json_get_str(v->msg, "warning")) {
            char *w = fg(TUI_TC_WARNING, tui_sym("icon.warning"));
            size_t l = strlen(label);
            snprintf(label + l, sizeof label - l, " %s", w);
            free(w);
        }
    }
    char *dot = trim_dup(tui_sym("sep.dot"));
    char *hint = xasprintf("%s %s", dot, expand_key_hint());
    free(dot);
    int pw = tui_visible_width(label) + 1 + tui_visible_width(hint);
    int remaining = width - pw - 2;
    strvec_push(o, "");
    if (remaining < 4) {
        strvec_push_own(o, fg(TUI_TC_MUTED, label));
    } else {
        int left = remaining / 2, right = remaining - left;
        const char *rule = tui_sym("tree.horizontal");
        sb_t b = SB_INIT, l = SB_INIT, r = SB_INIT;
        sb_repeat(&l, rule, left);
        sb_repeat(&r, rule, right);
        char *ls = fg(TUI_TC_DIM, sb_cstr(&l)), *rs = fg(TUI_TC_DIM, sb_cstr(&r));
        char *ms = fg(TUI_TC_MUTED, label), *hs = fg(TUI_TC_DIM, hint);
        sb_appendf(&b, "%s %s %s %s", ls, ms, hs, rs);
        strvec_push_own(o, sb_detach(&b));
        free(ls);
        free(rs);
        free(ms);
        free(hs);
        sb_free(&l);
        sb_free(&r);
    }
    strvec_push(o, "");
    free(hint);
    if (g_ui && g_ui->expand_tools) {
        const char *summary = json_get_str(v->msg, "summary");
        char *md;
        if (branch) md = xasprintf("**Branch summary**\n\n%s", summary ? summary : "");
        else {
            /* compactionDetailMarkdown */
            int64_t before = json_get_int(v->msg, "tokensBefore", 0), after = json_get_int(v->msg, "tokensAfter", -1);
            char b[32], a[32];
            locale_number(b, sizeof b, before);
            locale_number(a, sizeof a, after);
            char *line = before > 0 ? (after >= 0 ? xasprintf("Compacted from %s to %s tokens", b, a) : xasprintf("Compacted from %s tokens", b))
                                    : (after >= 0 ? xasprintf("Compacted to %s tokens", a) : xstrdup("Compacted context"));
            const char *warning = json_get_str(v->msg, "warning");
            char *wn = warning ? xasprintf("\n\n%s **Warning:** %s", tui_sym("icon.warning"), warning) : xstrdup("");
            md = xasprintf("**%s**%s\n\n%s", line, wn, summary ? summary : "");
            free(line);
            free(wn);
        }
        tui_md_opts_t mo;
        tui_md_opts_default(&mo);
        mo.text_slot = TUI_TC_CUSTOM_MESSAGE_TEXT;
        mo.bg_slot = TUI_TC_CUSTOM_MESSAGE_BG;
        mo.padding_x = 1;
        char *pad = tui_bgs(TH(), TUI_TC_CUSTOM_MESSAGE_BG, "");
        char *row = tui_pad_to_width("", width);
        char *blank = tui_bgs(TH(), TUI_TC_CUSTOM_MESSAGE_BG, row);
        strvec_push(o, blank);
        tui_md_render(md, width, &mo, o);
        strvec_push(o, blank);
        free(blank);
        free(row);
        free(pad);
        free(md);
    }
}

/* MessageNoticeComponent for TTSR (TtsrNotificationComponent): inverse warning box, padding 1 */
static void notice_box(const strvec_t *body, int width, strvec_t *o);

static void render_ttsr(msgview_t *v, int width, strvec_t *o) {
    strvec_t rules = {0};
    for (const char *p = v->text; p && *p;) {
        const char *e = strstr(p, ", ");
        strvec_push_own(&rules, xstrndup(p, e ? (size_t)(e - p) : strlen(p)));
        p = e ? e + 2 : NULL;
    }
    char *header;
    if (rules.n <= 1) {
        char *b = tui_bold(rules.n ? rules.v[0] : "");
        header = xasprintf("%s Injecting rule: %s  %s", tui_sym("icon.warning"), b, tui_sym("icon.rewind"));
        free(b);
    } else {
        header = xasprintf("%s Injecting %zu rules:  %s", tui_sym("icon.warning"), rules.n, tui_sym("icon.rewind"));
    }
    strvec_t body = {0};
    int cw = MAX(1, width - 2);
    strvec_push(&body, "");
    tui_wrap_text_with_ansi(header, cw, &body);
    if (rules.n > 1) {
        strvec_push(&body, "");
        for (size_t i = 0; i < rules.n && (i < 4 || (g_ui && g_ui->expand_tools)); i++) {
            char *b = tui_bold(rules.v[i]);
            tui_wrap_text_with_ansi(b, cw, &body);
            free(b);
        }
        if (rules.n > 4 && !(g_ui && g_ui->expand_tools)) {
            char *m = xasprintf("\xe2\x80\xa6 +%zu more (%s to expand)", rules.n - 4, expand_key_hint());
            char *it = tui_italic(m);
            tui_wrap_text_with_ansi(it, cw, &body);
            free(it);
            free(m);
        }
    }
    strvec_push(&body, "");
    notice_box(&body, width, o);
    strvec_free(&body);
    free(header);
    strvec_free(&rules);
}

/* MessageNoticeComponent box: inverse(fg(warning)) rows, padding 1 */
static void notice_box(const strvec_t *body, int width, strvec_t *o) {
    const char *warn = tui_fg_seq(TH(), TUI_TC_WARNING);
    for (size_t i = 0; i < body->n; i++) {
        char *x = xasprintf(" %s", body->v[i]);
        char *p = tui_pad_to_width(x, width);
        /* inverse(fg(warning, row)): resets inside the row keep the box colors */
        sb_t r = SB_INIT;
        sb_append(&r, "\x1b[7m");
        sb_append(&r, warn);
        for (const char *q = p; *q;) {
            if (!strncmp(q, "\x1b[0m", 4)) {
                sb_append(&r, "\x1b[0m\x1b[7m");
                sb_append(&r, warn);
                q += 4;
                continue;
            }
            sb_putc(&r, *q++);
        }
        sb_append(&r, "\x1b[39m\x1b[27m");
        strvec_push_own(o, sb_detach(&r));
        free(p);
        free(x);
    }
}

/* TodoReminderComponent: "<warning> N incomplete todos - reminder a/b" and the open items in italics */
static void render_todo_reminder(msgview_t *v, int width, strvec_t *o) {
    json_t *todos = json_get_arr(v->msg, "todos"), *t;
    size_t n = json_len(todos);
    char *header = xasprintf("%s %zu incomplete %s - reminder %lld/%lld", tui_sym("icon.warning"), n, n == 1 ? "todo" : "todos",
                             (long long)json_get_int(v->msg, "attempt", 0), (long long)json_get_int(v->msg, "maxAttempts", 0));
    strvec_t body = {0};
    int cw = MAX(1, width - 2);
    strvec_push(&body, "");
    tui_wrap_text_with_ansi(header, cw, &body);
    strvec_push(&body, "");
    sb_t list = SB_INIT;
    json_arr_foreach(todos, i, t) {
        if (i) sb_putc(&list, '\n');
        sb_appendf(&list, "  %s %s", tui_sym("checkbox.unchecked"), json_get_str(t, "content") ? json_get_str(t, "content") : "");
    }
    strvec_t lines = str_split_lines(sb_cstr(&list), list.len);
    for (size_t i = 0; i < lines.n; i++) {
        char *it = tui_italic(lines.v[i]);
        tui_wrap_text_with_ansi(it, cw, &body);
        free(it);
    }
    strvec_free(&lines);
    sb_free(&list);
    strvec_push(&body, "");
    notice_box(&body, width, o);
    strvec_free(&body);
    free(header);
}

/* buildParentIrcMessageCard: v->msg is the steer's parentIrc {from, body}, v->text the age taken at build time */
static void render_irc(msgview_t *v, int width, strvec_t *o) {
    tc_irc_incoming_card(json_get_str(v->msg, "from"), json_get_str(v->msg, "body"), v->text, g_ui && g_ui->expand_tools,
                         width, o);
}

msgview_t *ui_parent_irc_view(const json_t *m) {
    const json_t *pi = json_get(m, "parentIrc");
    if (!json_is_obj(pi)) return NULL;
    msgview_t *v = mv_new(MV_IRC);
    v->msg = json_clone(pi);
    int64_t ts = json_d2i(json_get_num(m, "timestamp", 0));
    if (ts) {
        int64_t age = (int64_t)llround((double)(now_ms() - ts) / 1000.0);
        v->text = tc_format_age(age < 1 ? 1 : age);
    }
    return v;
}

static void mv_render(tui_component_t *c, int width, strvec_t *out) {
    msgview_t *v = (msgview_t *)c;
    if (v->activity && g_ui && g_ui->hide_tool_activity) return; /* ToolActivityComponent hidden: no rows */
    bool animated = v->kind == MV_ASSISTANT && v->streaming && g_ui && g_ui->hide_thinking;
    if (!v->dirty && v->cache_w == width && !animated) {
        for (size_t i = 0; i < v->cache.n; i++) strvec_push(out, v->cache.v[i]);
        return;
    }
    strvec_free(&v->cache);
    strvec_t *o = &v->cache;
    switch (v->kind) {
    case MV_USER: render_user(v, width, o); break;
    case MV_ASSISTANT: render_assistant(v, width, o); break;
    case MV_TOOL:
    case MV_BASH: render_tool(v, width, o); break;
    case MV_NOTICE: render_notice(v, width, o); break;
    case MV_CUSTOM: render_custom(v, width, o); break;
    case MV_SUMMARY: render_summary(v, width, o); break;
    case MV_TTSR: render_ttsr(v, width, o); break;
    case MV_TODO: render_todo_reminder(v, width, o); break;
    case MV_SKILL: render_skill(v, width, o); break;
    case MV_IRC: render_irc(v, width, o); break;
    }
    v->cache_w = width;
    v->dirty = false;
    for (size_t i = 0; i < v->cache.n; i++) strvec_push(out, v->cache.v[i]);
}

static void mv_invalidate(tui_component_t *c) { ((msgview_t *)c)->dirty = true; }

static void mv_destroy(tui_component_t *c) {
    msgview_t *v = (msgview_t *)c;
    toolcard_forget(v);
    reveal_stop(v);
    json_free(v->msg);
    json_free(v->tool_images);
    free(v->text);
    free(v->retry_note);
    free(v->tool_name);
    free(v->tool_id);
    json_free(v->args);
    free(v->output);
    strvec_free(&v->cache);
    free(v);
}

static const tui_component_vt MV_VT = {"message", mv_render, NULL, mv_invalidate, mv_destroy};

/* First row of a transcript block (its kept rows [a, b)) that may still change, or -1 when it is
 * settled: a running tool card or an open read group as a whole; a streaming assistant message from
 * its last block on (rows above the last blank row are finished Markdown blocks, omp's stable
 * prefix that streams into history while the answer continues). */
static int mv_unsettled_from(tui_component_t *c, const strvec_t *rows, size_t a, size_t b) {
    if (c->vt != &MV_VT) return -1;
    msgview_t *v = (msgview_t *)c;
    if (v->kind == MV_TOOL || v->kind == MV_BASH) return !v->done || toolcard_group_open(v) ? 0 : -1;
    if (v->kind != MV_ASSISTANT || (!v->streaming && !v->reveal_timer)) return -1;
    for (size_t i = b; i > a; i--)
        if (plain_blank(rows->v[i - 1])) return (int)(i - a);
    return 0;
}

msgview_t *mv_new(mv_kind_t k) {
    msgview_t *v = xcalloc(1, sizeof(*v));
    tui_component_init(&v->base, &MV_VT);
    v->kind = k;
    v->dirty = true;
    v->activity = k == MV_TOOL || k == MV_TTSR || k == MV_TODO;
    return v;
}

void add_view(ui_t *u, msgview_t *v) {
    tui_container_add(u->chat, &v->base);
    tui_request_render(u->tui);
}

/* display.hideToolActivity: tool activity blocks stay mounted and render no rows while hidden; showing them
 * again collapses tool output (toggleToolActivityVisibility). The change applies to scrollback too. */
void ui_set_tool_activity_hidden(ui_t *u, bool hidden) {
    u->hide_tool_activity = hidden;
    if (!hidden) u->expand_tools = false;
    for (size_t i = 0; i < tui_container_count(u->chat); i++) tui_component_invalidate(u->chat->children.v[i]);
    tui_request_full_render(u->tui);
}

void ui_toggle_tool_activity(ui_t *u) {
    bool hidden = !u->hide_tool_activity;
    settings_set_global("display.hideToolActivity", json_bool(hidden));
    ui_set_tool_activity_hidden(u, hidden);
    add_notice(u, "info", hidden ? "Tool activity: hidden" : "Tool activity: visible");
}

static msgview_t *last_view(ui_t *u) {
    if (!u->chat->children.n) return NULL;
    tui_component_t *c = u->chat->children.v[u->chat->children.n - 1];
    return c->vt == &MV_VT ? (msgview_t *)c : NULL;
}

static int working_mode(ui_t *u);

/* whether `v` is still a transcript view (a rebuild destroys views) */
static bool chat_holds(ui_t *u, msgview_t *v) {
    for (size_t i = 0; i < u->chat->children.n; i++)
        if (u->chat->children.v[i] == &v->base) return true;
    return false;
}

/* attempts a pending retry hid and the saga never accounted for (a cancelled wait): their errors show again */
void ui_retry_restore_hidden(ui_t *u) {
    for (size_t i = 0; i < u->retry_hidden.n; i++) {
        msgview_t *v = u->retry_hidden.v[i];
        if (v == u->pinned_error || !chat_holds(u, v)) continue;
        v->error_pinned = false;
        v->dirty = true;
    }
    u->retry_hidden.n = 0;
}

/* auto_retry_end success: the failed attempts of this run (newest last) take the recovery notes
 * ("\n"-separated, oldest first) and the pinned error banner goes away */
void ui_retry_recovered(ui_t *u, const char *notes) {
    strvec_t v = {0};
    for (const char *s = notes; s && *s;) {
        const char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) : strlen(s);
        char *t = xstrndup(s, n);
        strvec_push(&v, t);
        free(t);
        s = nl ? nl + 1 : NULL;
    }
    size_t k = v.n;
    for (size_t i = u->chat->children.n; i-- > 0 && k > 0;) {
        tui_component_t *c = u->chat->children.v[i];
        if (c->vt != &MV_VT) continue;
        msgview_t *mv = (msgview_t *)c;
        if (mv->kind == MV_USER || mv->kind == MV_SKILL) break;
        if (mv->kind != MV_ASSISTANT || mv->retry_note || !str_eq(json_get_str(mv->msg, "stopReason"), "error")) continue;
        mv->retry_note = xstrdup(v.v[--k]);
        mv->error_pinned = false;
        mv->dirty = true;
        if (u->pinned_error == mv) u->pinned_error = NULL;
    }
    strvec_free(&v);
    u->retry_hidden.n = 0;
    ui_error_banner_clear(u);
}

/* auto_retry_end failure: the run's earlier failed attempts are superseded (removed), the final one shows
 * `final_error` inline instead of the pinned banner, then "Retry failed after N attempts: <error>" */
void ui_retry_failed(ui_t *u, int attempts, const char *final_error, const char *error) {
    msgview_t *last = NULL;
    for (size_t i = u->chat->children.n; i-- > 0;) {
        tui_component_t *c = u->chat->children.v[i];
        if (c->vt != &MV_VT) continue;
        msgview_t *mv = (msgview_t *)c;
        if (mv->kind == MV_USER || mv->kind == MV_SKILL) break;
        if (mv->kind != MV_ASSISTANT || mv->retry_note || !str_eq(json_get_str(mv->msg, "stopReason"), "error")) continue;
        if (!last) {
            last = mv;
            json_set_str(mv->msg, "errorMessage", final_error);
            mv->error_pinned = false;
            mv->dirty = true;
            continue;
        }
        if (u->pinned_error == mv) u->pinned_error = NULL;
        tui_container_remove(u->chat, c);
        tui_component_destroy(c);
    }
    if (last && u->pinned_error == last) u->pinned_error = NULL;
    u->retry_hidden.n = 0;
    ui_error_banner_clear(u);
    char *m = xasprintf("Retry failed after %d attempts: %s", attempts, error);
    add_notice(u, "error", m);
    free(m);
}

void add_notice(ui_t *u, const char *level, const char *text);

/* event-controller.ts #handleAutoCompactionStart / #handleAutoCompactionEnd (the event JSON from compaction.c) */
static void auto_compaction_event(ui_t *u, bool start, const char *text) {
    json_t *ev = json_parse(text, strlen(text), NULL);
    if (!json_is_obj(ev)) {
        json_free(ev);
        return;
    }
    const char *action = json_get_str(ev, "action");
    const char *label = str_eq(action, "remote")        ? "Auto server compaction"
                        : str_eq(action, "handoff")     ? "Auto-handoff"
                        : str_eq(action, "shake")       ? "Auto-shake"
                        : str_eq(action, "snapcompact") ? "Auto-snapcompact"
                                                        : "Auto context-full maintenance";
    if (start) {
        const char *reason = json_get_str(ev, "reason");
        const char *pre = str_eq(reason, "overflow")     ? "Context overflow detected, "
                          : str_eq(reason, "incomplete") ? "Response incomplete, "
                          : str_eq(reason, "idle")       ? "Idle "
                                                         : "";
        char *l = xasprintf("%s%s\xe2\x80\xa6", pre, label);
        ui_working_compaction(u, l);
        free(l);
        json_free(ev);
        return;
    }
    /* a mid-turn pass hands the row back to the running turn; a post-turn pass ends it */
    if (u->worker_running && u->app && agent_is_running(u->app->agent)) ui_working_start(u);
    else ui_working_stop(u);
    const char *em = json_get_str(ev, "errorMessage");
    bool skipped = json_get_bool(ev, "skipped", false);
    if (json_get_bool(ev, "aborted", false)) {
        char *m = xasprintf("%s cancelled", label);
        add_notice(u, "info", m);
        free(m);
    } else if (str_eq(action, "shake")) {
        if (em) {
            if (!skipped) ui_chat_rebuild(u);
            add_notice(u, "warning", em);
        } else if (!skipped) {
            ui_chat_rebuild(u);
            add_notice(u, "info", "Auto-shake completed");
        }
    } else if (json_get_obj(ev, "result")) {
        ui_chat_rebuild(u);
    } else if (em) {
        add_notice(u, "warning", em);
    } else if (str_eq(action, "handoff")) {
        ui_chat_rebuild(u);
        add_notice(u, "info", "Auto-handoff completed");
    } else if (!skipped) {
        char *m = xasprintf("%s failed; continuing without maintenance", str_eq(action, "remote") ? "Auto server compaction"
                                                                         : str_eq(action, "snapcompact") ? "Auto-snapcompact maintenance"
                                                                                                        : "Auto context-full maintenance");
        add_notice(u, "warning", m);
        free(m);
    }
    json_free(ev);
}

/* showStatus / showWarning / showError. Consecutive status lines update in place instead of stacking. */
void add_notice(ui_t *u, const char *level, const char *text) {
    if (!strcmp(level, "todo-reminder")) {
        json_t *j = json_parse(text, strlen(text), NULL);
        if (!json_is_obj(j)) {
            json_free(j);
            return;
        }
        msgview_t *v = mv_new(MV_TODO);
        v->msg = j;
        add_view(u, v);
        return;
    }
    if (!strcmp(level, "warning") && str_starts(text, "Injecting rule: ")) {
        msgview_t *v = mv_new(MV_TTSR);
        v->text = xstrdup(text + strlen("Injecting rule: "));
        add_view(u, v);
        return;
    }
    if (!strcmp(level, "warning") && str_starts(text, "Retrying (")) {
        /* auto-retry: the working row counts down instead of a transcript line */
        int at = 0, max = 0;
        long long secs = 0;
        if (sscanf(text, "Retrying (%d/%d) in %llds", &at, &max, &secs) == 3) {
            /* #handleAutoRetryStart: the retry supersedes the failed turn's never-run calls */
            while (u->synthetic_cards.n) { /* popped before the destroy: toolcard_forget edits the list */
                msgview_t *c = u->synthetic_cards.v[--u->synthetic_cards.n];
                for (size_t k = 0; k < u->chat->children.n; k++) {
                    if (u->chat->children.v[k] != &c->base) continue;
                    tui_container_remove(u->chat, &c->base);
                    tui_component_destroy(&c->base);
                    break;
                }
            }
            /* the failed attempt is not the turn's outcome while a retry is pending: its banner unpins and its inline
             * error stays hidden, so a recovered retry leaves no error in the transcript; the retry row names the
             * reason (omp 5ad164a69e) */
            if (u->pinned_error) {
                vec_push(&u->retry_hidden, u->pinned_error);
                u->pinned_error = NULL;
            }
            ui_error_banner_clear(u);
            const char *colon = strstr(text, "s: ");
            ui_working_retry(u, at, max, secs * 1000, colon ? colon + 3 : NULL);
            return;
        }
    }
    if (!strcmp(level, "auto_compaction_start") || !strcmp(level, "auto_compaction_end")) {
        auto_compaction_event(u, !strcmp(level, "auto_compaction_start"), text);
        return;
    }
    bool status = strcmp(level, "warning") && strcmp(level, "error");
    if (status && !strcmp(text, "Compacting context\xe2\x80\xa6")) {
        /* auto compaction from the agent (a manual /compact sets its own label first) */
        if (working_mode(u) != 3) ui_working_compaction(u, "Auto context-full maintenance\xe2\x80\xa6");
        return;
    }
    if (status && !strcmp(text, "Context compacted")) {
        /* the transcript is rebuilt around the compaction divider */
        if (u->worker_running) ui_working_start(u);
        else ui_working_stop(u);
        ui_chat_rebuild(u);
        return;
    }
    msgview_t *last = last_view(u);
    if (status && last && last->kind == MV_NOTICE && str_eq(last->level, "info")) {
        if (strcmp(last->text, text)) {
            free(last->text);
            last->text = xstrdup(text);
            last->dirty = true;
            tui_request_render(u->tui);
            return;
        }
        tui_container_remove(u->chat, &last->base);
        tui_component_destroy(&last->base);
    }
    msgview_t *v = mv_new(MV_NOTICE);
    v->level = status ? "info" : !strcmp(level, "error") ? "error" : "warning";
    v->text = xstrdup(text);
    add_view(u, v);
}

/* ---------------------------------------------------------------------- */
/* pinned error banner                                                     */
/* ---------------------------------------------------------------------- */

typedef struct {
    tui_component_t base;
    char *message;
} banner_t;

static void banner_render(tui_component_t *c, int width, strvec_t *out) {
    banner_t *b = (banner_t *)c;
    sb_t rule = SB_INIT;
    sb_repeat(&rule, tui_sym("boxRound.horizontal"), MAX(1, width));
    char *line = fg(TUI_TC_ERROR, sb_cstr(&rule));
    strvec_push(out, "");
    strvec_push(out, line);
    error_block_rows(out, b->message, width, 4, true);
    char *d = fg(TUI_TC_DIM, "Dismissed when you send your next message.");
    text_rows(out, d, width);
    strvec_push(out, line);
    free(d);
    free(line);
    sb_free(&rule);
}

static void banner_destroy(tui_component_t *c) {
    banner_t *b = (banner_t *)c;
    free(b->message);
    free(b);
}

static const tui_component_vt BANNER_VT = {"error-banner", banner_render, NULL, NULL, banner_destroy};

void ui_error_banner_show(ui_t *u, const char *message) {
    if (!u->banner) return;
    tui_container_clear(u->banner);
    banner_t *b = xcalloc(1, sizeof *b);
    tui_component_init(&b->base, &BANNER_VT);
    char *plain = tui_strip_ansi(message ? message : "");
    b->message = trim_dup(plain);
    free(plain);
    tui_container_add(u->banner, &b->base);
    tui_request_render(u->tui);
}

void ui_error_banner_clear(ui_t *u) {
    if (!u->banner || !u->banner->children.n) return;
    tui_container_clear(u->banner);
    tui_request_render(u->tui);
}

/* message_end for an assistant turn: a provider error is pinned above the editor and its inline block is
 * suppressed until the next turn starts */
/* text that follows a tool call in the same assistant message renders right below that tool's card
 * (splitAssistantMessageToolTimeline): segments wait here, keyed by tool call id, until the card exists */
static json_t *g_post_tool;

void ui_tool_started(ui_t *u, const char *tool_call_id) {
    json_t *seg = g_post_tool && tool_call_id ? json_get(g_post_tool, tool_call_id) : NULL;
    if (!seg) return;
    msgview_t *v = mv_new(MV_ASSISTANT);
    v->msg = json_clone(seg);
    json_del(g_post_tool, tool_call_id);
    add_view(u, v);
}

static void collect_post_tool(const json_t *msg) {
    const json_t *b;
    const char *call = NULL;
    json_t *seg = NULL;
    json_arr_foreach(json_get(msg, "content"), i, b) {
        if (str_eq(json_get_str(b, "type"), "toolCall")) {
            call = json_get_str(b, "id");
            seg = NULL;
            continue;
        }
        if (!call) continue;
        if (!seg) {
            if (!g_post_tool) g_post_tool = json_obj();
            seg = json_obj();
            json_set_str(seg, "role", "assistant");
            json_set_str(seg, "stopReason", "stop");
            json_set(seg, "content", json_arr());
            json_set(g_post_tool, call, seg);
        }
        json_push(json_get(seg, "content"), json_clone(b));
    }
}

/* ---------- cache-miss marker (display.cacheMissMarker, cache-invalidation-marker.ts) ---------- */

#define MIN_CACHE_FOOTPRINT 2048
static json_t *g_last_usage; /* lastAssistantUsage: the newest assistant usage that touched the prompt */

/* detectCacheInvalidation: tokens the cold turn reprocessed after a warm cache went cold, else 0 */
int64_t ui_detect_cache_invalidation(const json_t *prev, const json_t *cur) {
    if (!prev || !cur) return 0;
    if (json_get_num(prev, "cacheRead", 0) < MIN_CACHE_FOOTPRINT) return 0;
    /* the prefix survived when this turn read back at least half of what the previous one did (a cold turn can still read the
     * shared system/tool block back) */
    if (json_get_num(cur, "cacheRead", 0) * 2 >= json_get_num(prev, "cacheRead", 0)) return 0;
    if (json_get_num(cur, "cacheWrite", 0) <= 0) return 0;
    double reprocessed = json_get_num(cur, "cacheWrite", 0) + json_get_num(cur, "input", 0);
    if (reprocessed < MIN_CACHE_FOOTPRINT) return 0;
    return (int64_t)reprocessed;
}

/* an assistant message's usage arrived (live message_end or replay): mark the turn when it lost the cache
 * (unless a compaction / model / plan-mode change explains the miss), then remember the usage */
static void cache_usage_seen(msgview_t *v, const json_t *msg, bool explained) {
    const json_t *us = json_get_obj(msg, "usage");
    if (!us || json_get_num(us, "cacheRead", 0) + json_get_num(us, "cacheWrite", 0) + json_get_num(us, "input", 0) <= 0)
        return;
    if (!explained && settings_bool("display.cacheMissMarker", false))
        v->cache_miss = ui_detect_cache_invalidation(g_last_usage, us);
    json_free(g_last_usage);
    g_last_usage = json_clone(us);
}

/* buildSessionContext's transcript reset tracking (cacheMissExplainedAt): a compaction, a model change or a
 * plan-mode transition explains the next assistant turn's cache miss, as does a model switch between turns */
static struct {
    bool pending_reset;
    bool plan;
    char *last_model;
} g_rt;

static void rt_reset(void) {
    free(g_rt.last_model);
    memset(&g_rt, 0, sizeof g_rt);
}

static void rt_entry(const json_t *e) {
    const char *t = json_get_str(e, "type");
    if (str_eq(t, "compaction") || str_eq(t, "model_change")) g_rt.pending_reset = true;
    else if (str_eq(t, "mode_change")) {
        bool plan = str_eq(json_get_str(e, "mode"), "plan");
        if (plan != g_rt.plan) g_rt.pending_reset = true;
        g_rt.plan = plan;
    }
}

static bool rt_message(const json_t *m) {
    if (!str_eq(json_get_str(m, "role"), "assistant")) return false;
    char *model = xasprintf("%s/%s", json_get_str(m, "provider") ? json_get_str(m, "provider") : "undefined",
                            json_get_str(m, "model") ? json_get_str(m, "model") : "undefined");
    bool changed = g_rt.last_model && strcmp(g_rt.last_model, model) != 0;
    free(g_rt.last_model);
    g_rt.last_model = model;
    bool explained = g_rt.pending_reset || changed;
    g_rt.pending_reset = false;
    return explained;
}

void ui_assistant_end(ui_t *u, msgview_t *v) {
    v->streaming = false;
    reveal_stop(v);
    collect_post_tool(v->msg);
    cache_usage_seen(v, v->msg, false);
    v->dirty = true;
    const char *stop = json_get_str(v->msg, "stopReason");
    const char *err = json_get_str(v->msg, "errorMessage");
    if (str_eq(stop, "error") && err && *err) {
        if (u->pinned_error) u->pinned_error->error_pinned = false;
        v->error_pinned = true;
        u->pinned_error = v;
        ui_error_banner_show(u, err);
    }
}

void ui_agent_start(ui_t *u) {
    if (u->pinned_error) {
        u->pinned_error->error_pinned = false;
        u->pinned_error->dirty = true;
        u->pinned_error = NULL;
    }
    ui_error_banner_clear(u);
    ui_working_start(u);
}

/* ---------------------------------------------------------------------- */
/* working row                                                             */
/* ---------------------------------------------------------------------- */

typedef struct {
    tui_component_t base;
    ui_t *u;
    int timer;
    int mode; /* 0 idle, 1 working, 2 retry, 3 compaction */
    int frame;
    int64_t started;
    char *message;
    int retry_attempt, retry_max;
    int64_t retry_delay;
    char *retry_reason; /* previewLine(sanitizeText(errorMessage), 80); NULL when blank */
    char *compaction_label;
} working_t;

static void format_duration(char *buf, size_t n, int64_t ms) {
    if (ms <= 0) snprintf(buf, n, "0ms");
    else if (ms < 1000) snprintf(buf, n, "%lldms", (long long)ms);
    else if (ms < 60000) {
        long long tenths = (ms + 50) / 100;
        if (tenths < 600) snprintf(buf, n, "%lld.%llds", tenths / 10, tenths % 10);
        else snprintf(buf, n, "1m");
    } else if (ms < 3600000) {
        long long m = ms / 60000, s = (ms % 60000) / 1000;
        if (s) snprintf(buf, n, "%lldm%llds", m, s);
        else snprintf(buf, n, "%lldm", m);
    } else {
        long long h = ms / 3600000, m = (ms % 3600000) / 60000;
        if (m) snprintf(buf, n, "%lldh%lldm", h, m);
        else snprintf(buf, n, "%lldh", h);
    }
}

static void working_render(tui_component_t *c, int width, strvec_t *out) {
    working_t *w = (working_t *)c;
    if (!w->mode) return;
    const tui_theme_t *th = TH();
    sb_t b = SB_INIT;
    sb_putc(&b, ' ');
    if (w->mode == 3) {
        const char *const *frames;
        size_t nf = tui_sym_spinner("activity", &frames);
        tui_fg(&b, th, TUI_TC_ACCENT, nf ? frames[(size_t)w->frame % nf] : "*");
        sb_putc(&b, ' ');
        char *msg = xasprintf("%s (%s to cancel)", w->compaction_label ? w->compaction_label : "", tui_sym("key.esc"));
        tui_fg(&b, th, TUI_TC_MUTED, msg);
        free(msg);
    } else if (w->mode == 2) {
        const char *const *frames;
        size_t nf = tui_sym_spinner("activity", &frames);
        const char *frame = nf ? frames[(size_t)w->frame % nf] : "*";
        tui_fg(&b, th, TUI_TC_WARNING, frame);
        sb_putc(&b, ' ');
        char d[32];
        int64_t left = w->retry_delay - (mono_ms() - w->started);
        format_duration(d, sizeof d, left);
        char *msg = w->retry_reason ? xasprintf("Retrying (%d/%d) in %s \xc2\xb7 %s (%s to cancel)", w->retry_attempt, w->retry_max, d,
                                                w->retry_reason, tui_sym("key.esc"))
                                    : xasprintf("Retrying (%d/%d) in %s\xe2\x80\xa6 (%s to cancel)", w->retry_attempt, w->retry_max, d,
                                                tui_sym("key.esc"));
        tui_fg(&b, th, TUI_TC_MUTED, msg);
        free(msg);
    } else {
        sb_free(&b);
        return;
    }
    char *line = sb_detach(&b);
    if (tui_visible_width(line) > width) {
        char *t = tui_truncate_to_width(line, width, "");
        free(line);
        line = t;
    }
    strvec_push(out, "");
    strvec_push_own(out, line);
}

static void working_tick(void *ud) {
    working_t *w = ud;
    w->frame = tui_spinner_tick();
    if (w->u && w->u->tui) tui_request_render(w->u->tui);
}

static void working_timer(working_t *w, int interval) {
    if (w->timer && w->u && w->u->tui) tui_remove_timer(w->u->tui, w->timer);
    w->timer = interval > 0 && w->u && w->u->tui ? tui_add_clock_timer(w->u->tui, NULL, working_tick, w) : 0;
}

static void working_destroy(tui_component_t *c) {
    working_t *w = (working_t *)c;
    working_timer(w, 0);
    free(w->message);
    free(w->retry_reason);
    free(w->compaction_label);
    free(w);
}

static const tui_component_vt WORKING_VT = {"working", working_render, NULL, NULL, working_destroy};

static int working_mode(ui_t *u) { return u->working ? ((working_t *)u->working)->mode : 0; }

bool ui_working_busy(ui_t *u) { return working_mode(u) >= 2; }

tui_component_t *ui_working_new(ui_t *u) {
    working_t *w = xcalloc(1, sizeof *w);
    tui_component_init(&w->base, &WORKING_VT);
    w->u = u;
    w->message = xstrdup("Working\xe2\x80\xa6");
    return &w->base;
}

/* the plain working row is the chrome's status row around u->loader; this component only draws the retry
 * countdown and compaction rows (which replace it while they run) */
static void loader_visible(ui_t *u, bool on) {
    if (!u->loader) return;
    u->loader->base.hidden = !on;
    if (on) tui_loader_start(u->loader);
    else tui_loader_stop(u->loader);
}

void ui_working_start(ui_t *u) {
    u->retry_hint = false; /* the loader takes the status row (disposeChildren) */
    working_t *w = (working_t *)u->working;
    if (w) {
        w->mode = 0;
        working_timer(w, 0);
    }
    loader_visible(u, true);
    tui_request_render(u->tui);
}

void ui_working_stop(ui_t *u) {
    working_t *w = (working_t *)u->working;
    if (w) {
        w->mode = 0;
        working_timer(w, 0);
    }
    loader_visible(u, false);
    tui_request_render(u->tui);
}

void ui_working_set_message(ui_t *u, const char *message) {
    if (u->loader) tui_loader_set_message(u->loader, message && *message ? message : "Working\xe2\x80\xa6");
    tui_request_render(u->tui);
}

void ui_working_compaction(ui_t *u, const char *label) {
    u->retry_hint = false; /* the loader takes the status row (disposeChildren) */
    working_t *w = (working_t *)u->working;
    if (!w) return;
    free(w->compaction_label);
    w->compaction_label = xstrdup(label);
    w->mode = 3;
    loader_visible(u, false);
    working_timer(w, 80);
    tui_request_render(u->tui);
}

void ui_working_retry(ui_t *u, int attempt, int max, int64_t delay_ms, const char *reason) {
    u->retry_hint = false; /* the loader takes the status row (disposeChildren) */
    working_t *w = (working_t *)u->working;
    if (!w) return;
    w->mode = 2;
    free(w->retry_reason);
    w->retry_reason = NULL;
    if (reason) { /* previewLine: flattened to one line, at most 80 columns */
        char *san = tc_sanitize_line(reason);
        for (char *q = san; *q; q++)
            if (*q == '\n' || *q == '\r') *q = ' ';
        char *t = str_trim(san);
        free(san);
        if (*t) w->retry_reason = tui_truncate_to_width(t, 80, NULL);
        free(t);
    }
    loader_visible(u, false);
    w->retry_attempt = attempt;
    w->retry_max = max;
    w->retry_delay = delay_ms;
    w->started = mono_ms();
    working_timer(w, 80);
    tui_request_render(u->tui);
}

/* BackgroundCompactionHud (progress-hud.ts): a background compaction (/handoff) leaves the composer free. While no
 * turn holds the status row it shows its working row there; beside a turn, a right-aligned HUD row above the
 * working line, `<dim label> <elapsed>`, instead, so neither replaces the other. */
typedef struct {
    tui_component_t base;
    ui_t *u;
    int timer;
    int64_t started;
    char *label; /* NULL: no background compaction */
} bghud_t;

static char *bg_cancel_hint(void) {
    char *k = ui_editor_key("app.interrupt");
    char *h = xasprintf("(%s to cancel)", k);
    free(k);
    return h;
}

static void bghud_render(tui_component_t *c, int width, strvec_t *out) {
    bghud_t *h = (bghud_t *)c;
    if (!h->label || !h->u->worker_running || width <= 0) return;
    const tui_theme_t *th = TH();
    char d[32];
    format_duration(d, sizeof d, mono_ms() - h->started);
    sb_t took = SB_INIT;
    tui_fg(&took, th, TUI_TC_TEXT, d);
    char *tail = sb_detach(&took);
    int remaining = width - tui_visible_width(tail);
    if (remaining > 1) {
        char *hint = bg_cancel_hint();
        char *full = xasprintf("%s %s", h->label, hint);
        char *lab = tui_truncate_to_width(full, remaining - 1, "");
        if (*lab) {
            sb_t b = SB_INIT;
            tui_fg(&b, th, TUI_TC_DIM, lab);
            sb_putc(&b, ' ');
            sb_append(&b, tail);
            free(tail);
            tail = sb_detach(&b);
        }
        free(lab), free(full), free(hint);
    }
    if (tui_visible_width(tail) > width) {
        free(tail);
        sb_t b = SB_INIT;
        tui_fg(&b, th, TUI_TC_TEXT, d);
        char *fb = sb_detach(&b);
        tail = tui_truncate_to_width(fb, width, "");
        free(fb);
    }
    int pad = width - tui_visible_width(tail);
    sb_t row = SB_INIT;
    for (int i = 0; i < pad; i++) sb_putc(&row, ' ');
    sb_append(&row, tail);
    free(tail);
    strvec_push_own(out, sb_detach(&row));
}

static void bghud_destroy(tui_component_t *c) {
    bghud_t *h = (bghud_t *)c;
    if (h->timer && h->u && h->u->tui) tui_remove_timer(h->u->tui, h->timer);
    free(h->label);
    free(h);
}

static const tui_component_vt BGHUD_VT = {"bg-compaction-hud", bghud_render, NULL, NULL, bghud_destroy};

tui_component_t *ui_bg_compaction_hud_new(ui_t *u) {
    bghud_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &BGHUD_VT);
    h->u = u;
    u->bg_hud = &h->base;
    return &h->base;
}

/* the idle working row: `<label>… (Esc to cancel)` while nothing else holds the status row */
static void bghud_idle_row(bghud_t *h) {
    ui_t *u = h->u;
    if (!h->label || u->worker_running || working_mode(u) != 0) return;
    char *l = xasprintf("%s\xe2\x80\xa6", h->label);
    ui_working_compaction(u, l);
    free(l);
}

static void bghud_tick(void *ud) {
    bghud_t *h = ud;
    bghud_idle_row(h);
    if (h->u && h->u->tui) tui_request_render(h->u->tui);
}

void ui_bg_compaction_start(ui_t *u, const char *label) {
    bghud_t *h = (bghud_t *)u->bg_hud;
    if (!h) return;
    free(h->label);
    h->label = xstrdup(label);
    h->started = mono_ms();
    if (!h->timer && u->tui) h->timer = tui_add_clock_timer(u->tui, NULL, bghud_tick, h);
    bghud_idle_row(h);
    tui_request_render(u->tui);
}

void ui_bg_compaction_stop(ui_t *u) {
    bghud_t *h = (bghud_t *)u->bg_hud;
    if (!h || !h->label) return;
    free(h->label);
    h->label = NULL;
    if (h->timer && u->tui) tui_remove_timer(u->tui, h->timer);
    h->timer = 0;
    if (!u->worker_running && working_mode(u) == 3) ui_working_stop(u);
    tui_request_render(u->tui);
}

/* ---------------------------------------------------------------------- */
/* transcript replay                                                       */
/* ---------------------------------------------------------------------- */


/* omp buildAsyncResultBlock: one `✔ Background job completed [type] id (duration)` row per delivered job */
void ui_async_result(ui_t *u, const json_t *m) {
    if (!str_eq(json_get_str(m, "customType"), "async-result")) return;
    const json_t *jobs = json_get_arr(json_get_obj(m, "details"), "jobs");
    if (!json_len(jobs)) return;
    sb_t b = SB_INIT;
    json_t *j;
    json_arr_foreach(jobs, i, j) {
        char *head = xasprintf("%s Background job completed", tui_sym("status.success"));
        char *type = xasprintf("[%s]", json_get_str(j, "type") ? json_get_str(j, "type") : "job");
        char *h = fg(TUI_TC_SUCCESS, head), *t = fg(TUI_TC_DIM, type);
        char *id = fg(TUI_TC_ACCENT, json_get_str(j, "jobId") ? json_get_str(j, "jobId") : "unknown");
        if (b.len) sb_putc(&b, '\n');
        sb_appendf(&b, "%s %s %s", h, t, id);
        if (json_get(j, "durationMs")) {
            char *du = tc_format_duration(json_get_int(j, "durationMs", 0));
            char *dp = xasprintf("(%s)", du);
            char *dd = fg(TUI_TC_DIM, dp);
            sb_appendf(&b, " %s", dd);
            free(dd);
            free(dp);
            free(du);
        }
        free(id);
        free(t);
        free(h);
        free(type);
        free(head);
    }
    msgview_t *v = mv_new(MV_NOTICE);
    v->level = "styled";
    v->text = sb_detach(&b);
    v->activity = true; /* buildAsyncResultBlock wraps its rows in a ToolActivityContainer */
    add_view(u, v);
}

/* ---------- per-turn usage row (display.showTokenUsage, usage-row.ts) ---------- */

static double g_turn_started;      /* prompt->yield anchor (the turn's user prompt timestamp), 0 = unknown */
static json_t *g_pending_usage;    /* replay: the last assistant message whose row waits for its tool results */

void ui_usage_turn_start(double ts) { g_turn_started = ts; }

/* assistantUsageIsBilled */
static bool usage_billed(const json_t *us) {
    return json_get_num(us, "input", 0) > 0 || json_get_num(us, "output", 0) > 0 || json_get_num(us, "cacheRead", 0) > 0 ||
           json_get_num(us, "cacheWrite", 0) > 0 || json_get_num(us, "premiumRequests", 0) > 0;
}

static msgview_t *usage_row_view(const json_t *msg, double turn_start) {
    const json_t *us = json_get_obj(msg, "usage");
    double turn = 0;
    if (settings_bool("display.showTurnTime", false) && turn_start > 0 && json_is_num(json_get(msg, "completedAt"))) {
        double el = json_get_num(msg, "completedAt", 0) - turn_start;
        turn = el > 0 ? floor(el + 0.5) : 0;
    }
    char *row = ui_usage_row(us, json_get_num(msg, "duration", 0), json_get_num(msg, "ttft", 0), json_get_num(msg, "timestamp", 0), turn);
    msgview_t *v = mv_new(MV_NOTICE);
    v->level = "styled";
    v->text = fg(TUI_TC_DIM, row);
    free(row);
    return v;
}

/* live message_end of an assistant message: the metric strip below the turn */
void ui_usage_row_live(ui_t *u, const json_t *msg) {
    if (!settings_bool("display.showTokenUsage", false) || !usage_billed(json_get_obj(msg, "usage"))) return;
    add_view(u, usage_row_view(msg, g_turn_started));
}

static void flush_pending_usage(ui_t *u) {
    if (!g_pending_usage) return;
    msgview_t *v = usage_row_view(g_pending_usage, json_get_num(g_pending_usage, "_turnStartedAt", 0));
    tui_container_add(u->chat, &v->base);
    json_free(g_pending_usage);
    g_pending_usage = NULL;
}

static void replay_message(ui_t *u, const json_t *m, hmap_t *pending) {
    const char *role = json_get_str(m, "role");
    if (!str_eq(role, "toolResult")) flush_pending_usage(u);
    if (str_eq(role, "user") && !str_eq(json_get_str(m, "attribution"), "agent")) g_turn_started = json_get_num(m, "timestamp", 0);
    else if (str_eq(role, "developer") && json_get_bool(m, "synthetic", false))
        g_turn_started = json_get_bool(m, "userInitiated", false) ? json_get_num(m, "timestamp", 0) : 0;
    if (str_eq(role, "user") && str_eq(json_get_str(m, "customType"), "skill-prompt") && json_get_bool(m, "display", false)) {
        toolcard_break_group(u);
        msgview_t *v = mv_new(MV_SKILL);
        v->msg = json_clone(m);
        tui_container_add(u->chat, &v->base);
    } else if (str_eq(role, "user") && json_is_obj(json_get(m, "parentIrc"))) {
        toolcard_break_group(u);
        tui_container_add(u->chat, &ui_parent_irc_view(m)->base);
    } else if (str_eq(role, "user")) {
        toolcard_break_group(u);
        char *text = ai_message_text(m);
        if (text && *text) {
            msgview_t *v = mv_new(MV_USER);
            v->text = text;
            if (json_get_bool(m, "synthetic", false)) { /* hidden prompts: goal/advisor continuations, rules */
                tui_component_destroy(&v->base);
                ui_async_result(u, m);
                return;
            }
            v->synthetic = str_eq(json_get_str(m, "attribution"), "agent");
            tui_container_add(u->chat, &v->base);
        } else free(text);
    } else if (str_eq(role, "assistant")) {
        toolcard_note_message(u, m, true); /* keeps read grouping identical to the live transcript */
        msgview_t *v = mv_new(MV_ASSISTANT);
        v->msg = json_clone(m);
        cache_usage_seen(v, m, rt_message(m));
        tui_container_add(u->chat, &v->base);
        const json_t *content = json_get(m, "content"), *b;
        bool seen_tool = false;
        json_t *after = NULL;
        json_arr_foreach(content, i, b) {
            if (str_eq(json_get_str(b, "type"), "toolCall")) {
                if (after) {
                    msgview_t *pv = mv_new(MV_ASSISTANT);
                    pv->msg = after;
                    after = NULL;
                    tui_container_add(u->chat, &pv->base);
                }
                const char *tid = json_get_str(b, "id") ? json_get_str(b, "id") : "";
                json_t *targs = json_get(b, "arguments");
                msgview_t *tv = toolcard_start(u, tid, json_get_str(b, "name") ? json_get_str(b, "name") : "",
                                               targs ? json_clone(targs) : json_obj());
                hmap_put(pending, tid, tv);
                seen_tool = true;
            } else if (seen_tool) {
                /* text after a tool call renders below that tool */
                if (!after) {
                    after = json_obj();
                    json_set_str(after, "role", "assistant");
                    json_set_str(after, "stopReason", "stop");
                    json_set(after, "content", json_arr());
                }
                json_push(json_get(after, "content"), json_clone(b));
            }
        }
        if (after) {
            msgview_t *pv = mv_new(MV_ASSISTANT);
            pv->msg = after;
            tui_container_add(u->chat, &pv->base);
        }
        if (settings_bool("display.showTokenUsage", false) && usage_billed(json_get_obj(m, "usage"))) {
            g_pending_usage = json_clone(m);
            json_set_num(g_pending_usage, "_turnStartedAt", g_turn_started);
        }
    } else if (str_eq(role, "toolResult")) {
        const char *id = json_get_str(m, "toolCallId");
        msgview_t *tv = id ? hmap_get(pending, id) : NULL;
        if (tv) {
            /* same path as a live result: footer facts (wall time, exit) and grouping come from details */
            json_t *r = json_obj();
            json_t *content = json_get(m, "content");
            json_set(r, "content", content ? json_clone(content) : json_arr());
            if (json_get(m, "details")) json_set(r, "details", json_clone(json_get(m, "details")));
            toolcard_result(u, tv, id, r, false, json_get_bool(m, "isError", false));
            hmap_del(pending, id);
        }
    } else if (str_eq(role, "bashExecution") || str_eq(role, "pythonExecution")) {
        bool py = str_eq(role, "pythonExecution");
        msgview_t *v = mv_new(MV_BASH);
        v->tool_name = xstrdup(py ? "eval" : "bash");
        v->tool_id = xstrdup("");
        v->args = json_obj();
        const char *cmd = json_get_str(m, py ? "code" : "command");
        json_set_str(v->args, "command", cmd ? cmd : "");
        if (py) json_set_bool(v->args, "python", true);
        if (json_get_bool(m, "excludeFromContext", false)) json_set_bool(v->args, "exclude", true);
        if (json_get_bool(m, "cancelled", false)) json_set_bool(v->args, "cancelled", true);
        if (json_get(m, "exitCode")) json_set_int(v->args, "exitCode", json_get_int(m, "exitCode", 0));
        v->output = xstrdup(json_get_str(m, "output") ? json_get_str(m, "output") : "");
        v->done = true;
        v->is_error = json_get_bool(m, "cancelled", false) || json_get_int(m, "exitCode", 0) != 0;
        tui_container_add(u->chat, &v->base);
    } else if (str_eq(role, "custom") || str_eq(role, "hookMessage")) {
        if (!json_get_bool(m, "display", false)) return;
        msgview_t *v = mv_new(MV_CUSTOM);
        v->msg = json_clone(m);
        tui_container_add(u->chat, &v->base);
    }
}

static void replay_entry(ui_t *u, const json_t *e, hmap_t *pending) {
    const char *t = json_get_str(e, "type");
    rt_entry(e);
    if (str_eq(t, "message")) {
        replay_message(u, json_get(e, "message"), pending);
    } else if (str_eq(t, "custom_message")) {
        if (!json_get_bool(e, "display", true)) {
            ui_async_result(u, e);
            return;
        }
        bool skill = str_eq(json_get_str(e, "customType"), "skill-prompt");
        if (skill) toolcard_break_group(u);
        msgview_t *v = mv_new(skill ? MV_SKILL : MV_CUSTOM);
        v->msg = json_clone(e);
        json_set_str(v->msg, "role", "custom");
        tui_container_add(u->chat, &v->base);
    } else if (str_eq(t, "branch_summary")) {
        msgview_t *v = mv_new(MV_SUMMARY);
        v->level = "branch";
        v->msg = json_clone(e);
        tui_container_add(u->chat, &v->base);
    }
}

/* display.collapseCompacted off (buildSessionContext transcript && !collapseCompactedHistory): every path entry
 * in chronological order, reset boundaries included; each compaction draws its divider where it fired, and
 * the ones a newer compaction replaced carry omp's superseded summary */
static void replay_full_history(ui_t *u, const json_t *path, hmap_t *pending) {
    size_t n = json_len(path);
    const char *latest = NULL;
    for (size_t i = 0; i < n; i++)
        if (str_eq(json_get_str(json_at(path, i), "type"), "compaction")) latest = json_get_str(json_at(path, i), "id");
    for (size_t i = 0; i < n; i++) {
        const json_t *e = json_at(path, i);
        if (!str_eq(json_get_str(e, "type"), "compaction")) {
            replay_entry(u, e, pending);
            continue;
        }
        rt_entry(e);
        const json_t *next = i + 1 < n ? json_at(path, i + 1) : NULL;
        if (next && str_eq(json_get_str(next, "type"), "compaction") && str_eq(json_get_str(e, "method"), "snapcompact") &&
            str_eq(json_get_str(next, "method"), "snapcompact") &&
            str_eq(json_get_str(next, "parentId"), json_get_str(e, "id")) &&
            str_eq(json_get_str(next, "firstKeptEntryId"), json_get_str(e, "firstKeptEntryId")) &&
            json_get_num(next, "tokensBefore", -1) == json_get_num(e, "tokensBefore", -2))
            continue; /* an immediate frame-rescue replacement is the same compaction point */
        msgview_t *v = mv_new(MV_SUMMARY);
        v->level = "compaction";
        v->msg = json_clone(e);
        if (!str_eq(json_get_str(e, "id"), latest)) {
            json_set_str(v->msg, "summary", "[Superseded compaction summary elided after a newer compaction]");
            json_set_str(v->msg, "shortSummary", "Superseded compaction elided");
        }
        tui_container_add(u->chat, &v->base);
    }
}

void ui_chat_replay_into(ui_t *u, tui_container_t *dest, const json_t *entries, size_t *ends) {
    tui_container_t *live = u->chat;
    hmap_t tools = u->tools;
    msgview_t *cur = u->cur_assistant, *pinned = u->pinned_error;
    msgview_t *group = toolcard_group_swap(NULL);
    u->chat = dest;
    hmap_init(&u->tools);
    u->cur_assistant = NULL;
    u->pinned_error = NULL;
    hmap_t pending;
    hmap_init(&pending);
    toolcard_set_replaying(true);
    json_t *live_usage = g_last_usage; /* the replica keeps its own cache-miss tracking */
    g_last_usage = NULL;
    typeof(g_rt) live_rt = g_rt;
    memset(&g_rt, 0, sizeof g_rt);
    const json_t *e;
    json_arr_foreach(entries, i, e) {
        replay_entry(u, e, &pending);
        ends[i] = dest->children.n;
    }
    flush_pending_usage(u);
    rt_reset();
    g_rt = live_rt;
    json_free(g_last_usage);
    g_last_usage = live_usage;
    toolcard_set_replaying(false);
    hmap_free(&pending, NULL);
    hmap_free(&u->tools, NULL);
    u->tools = tools;
    u->cur_assistant = cur;
    u->pinned_error = pinned;
    u->chat = live;
    toolcard_group_swap(group);
}

const char *ui_session_title(ui_t *u) {
    /* the worker (auto title) may replace the title mid-frame: read a locked copy, keeping every copy handed out alive
     * (titles change a handful of times per process) so a borrowed pointer stays valid however long a frame holds it */
    static strvec_t seen;
    char *t = u && u->app ? session_title_dup(u->app->session) : NULL;
    if (!t) return NULL;
    for (size_t i = seen.n; i-- > 0;)
        if (!strcmp(seen.v[i], t)) {
            free(t);
            return seen.v[i];
        }
    strvec_push_own(&seen, t);
    return t;
}

/* Rebuild the transcript from the session tree (renderSessionContext): the path root..leaf after the last
 * reset boundary; a compaction shows the kept tail, its divider, then everything after it (with
 * display.collapseCompacted off: the whole path, a divider at each compaction). Without a session file the
 * agent's live context is replayed instead. */
void ui_chat_rebuild(ui_t *u) {
    pred_cancel(u); /* EventController reset: the transcript changed under any prediction */
    tui_container_clear(u->chat);
    ui_report_clear(u);
    hmap_free(&u->tools, NULL);
    hmap_init(&u->tools);
    u->cur_assistant = NULL;
    u->pinned_error = NULL;
    hmap_t pending;
    hmap_init(&pending);
    toolcard_set_replaying(true);
    session_t *s = ui_view_app(u)->session; /* a focused subagent view replays its session */
    /* a focused subagent's session grows on its job thread: replay a locked copy of its messages instead. The main
     * session's worker may still be writing too (a rebuild after a mid-run compaction): copy its branch locked. */
    json_t *path = !u->view && s ? session_current_path(s) : NULL;
    json_free(g_last_usage);
    g_last_usage = NULL;
    rt_reset();
    if (path && json_len(path) && !settings_bool("display.collapseCompacted", true)) {
        replay_full_history(u, path, &pending);
    } else if (path && json_len(path)) {
        size_t n = json_len(path), start = 0;
        ssize_t compaction = -1;
        for (size_t i = 0; i < n; i++) {
            const char *t = json_get_str(json_at(path, i), "type");
            if (str_eq(t, "reset_boundary")) {
                start = i + 1;
                compaction = -1;
            } else if (str_eq(t, "compaction")) compaction = (ssize_t)i;
        }
        if (compaction >= 0) {
            /* the collapsed display transcript: the kept tail from its first user-initiated turn, then the
             * divider at the chronological compaction point, then everything after it */
            json_t *c = json_at(path, (size_t)compaction);
            const char *first_kept = json_get_str(c, "firstKeptEntryId");
            const char *through = json_get_str(c, "providerReplayThroughEntryId");
            ssize_t retained = -1;
            for (size_t i = start; first_kept && *first_kept && i < (size_t)compaction && retained < 0; i++)
                if (str_eq(json_get_str(json_at(path, i), "id"), first_kept)) retained = (ssize_t)i;
            for (size_t i = start; retained < 0 && first_kept && !*first_kept && through && i < (size_t)compaction; i++)
                if (str_eq(json_get_str(json_at(path, i), "id"), through)) retained = (ssize_t)i + 1;
            if (retained >= 0 && retained < compaction) {
                size_t from = (size_t)retained;
                for (size_t i = (size_t)retained; i < (size_t)compaction; i++) {
                    const json_t *e = json_at(path, i);
                    const char *t = json_get_str(e, "type");
                    const char *role = json_get_str(json_get(e, "message"), "role");
                    bool turn = str_eq(t, "branch_summary") ||
                                (str_eq(t, "custom_message") && str_eq(json_get_str(e, "attribution"), "user")) ||
                                (str_eq(t, "message") && (str_eq(role, "user") || str_eq(role, "bashExecution")));
                    if (turn) {
                        from = i;
                        break;
                    }
                }
                for (size_t k = (size_t)retained; k < from; k++) { /* hidden entries still feed reset tracking */
                    const json_t *e = json_at(path, k);
                    rt_entry(e);
                    if (str_eq(json_get_str(e, "type"), "message")) rt_message(json_get(e, "message"));
                }
                for (size_t k = from; k < (size_t)compaction; k++) replay_entry(u, json_at(path, k), &pending);
            }
            rt_entry(c); /* the first post-compaction turn, not a kept one, carries the explained miss */
            msgview_t *v = mv_new(MV_SUMMARY);
            v->level = "compaction";
            v->msg = json_clone(c);
            tui_container_add(u->chat, &v->base);
            start = (size_t)compaction + 1;
        }
        for (size_t i = start; i < n; i++) replay_entry(u, json_at(path, i), &pending);
    } else {
        agent_t *ag = ui_view_app(u)->agent;
        pthread_mutex_lock(&ag->mu);
        json_t *msgs = json_clone(ag->messages);
        pthread_mutex_unlock(&ag->mu);
        json_t *m;
        json_arr_foreach(msgs, i, m) replay_message(u, m, &pending);
        json_free(msgs);
    }
    flush_pending_usage(u);
    /* a compaction (or model / plan-mode switch) after the last assistant turn rewrote the prompt prefix: the next
     * live turn's miss is expected (omp 5649620e0e nextCacheMissExplained) */
    if (g_rt.pending_reset) {
        json_free(g_last_usage);
        g_last_usage = NULL;
    }
    json_free(path);
    hmap_free(&pending, NULL);
    toolcard_set_replaying(false);
    if (!u->view) ui_recap_replay(u);
    if (!u->view && !u->worker_running) { /* #reseedTokenRate: the last completed turn after a history swap */
        agent_t *ag = u->app->agent;
        double out = 0, dur = 0;
        pthread_mutex_lock(&ag->mu);
        for (size_t i = json_len(ag->messages); i-- > 0;) {
            const json_t *m = json_at(ag->messages, i);
            if (!str_eq(json_get_str(m, "role"), "assistant") || !json_is_num(json_get(m, "duration"))) continue;
            out = json_get_num(json_get_obj(m, "usage"), "output", 0);
            dur = json_get_num(m, "duration", 0);
            break;
        }
        pthread_mutex_unlock(&ag->mu);
        tokrate_seed(&u->tokrate, out, dur);
    }
    if (!u->view) {
        ui_signals_session_title(); /* resume / switch / branch: the title follows the session */
        ui_recap_invalidate();
        ui_signals_session_start();
    }
    ui_sync_retry_hint(u); /* renderInitialMessages: resume, /tree, session switch */
    toolcard_prior_reset(u);
    tui_request_full_render(u->tui);
}
