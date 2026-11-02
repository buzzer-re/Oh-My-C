/* input.c - single-line text input (port of omp packages/tui/src/components/input.ts): readline-style
 * editing keys and the `> ` prompt row with the cursor cell windowed into view. Shared by the BTW
 * follow-up composer (btw.c) and the history search field (histsearch.c). */
#include "ui/ui.h"

#include <ctype.h>

static size_t prev_cp(const char *s, size_t i) {
    if (!i) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    return i;
}

static size_t next_cp(const char *s, size_t n, size_t i) {
    if (i >= n) return n;
    i++;
    while (i < n && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
    return i;
}

static void input_delete(ui_input_t *in, size_t a, size_t b) {
    if (b <= a) return;
    memmove(in->v.s + a, in->v.s + b, in->v.len - b + 1);
    in->v.len -= b - a;
    in->cur = a;
}

void ui_input_insert(ui_input_t *in, const char *t) {
    sb_t n = SB_INIT;
    sb_appendn(&n, in->v.s ? in->v.s : "", in->cur);
    for (const char *p = t; *p; p++) sb_putc(&n, *p == '\n' || *p == '\r' || *p == '\t' ? ' ' : *p);
    size_t cur = n.len;
    if (in->v.s) sb_append(&n, in->v.s + in->cur);
    sb_free(&in->v);
    in->v = n;
    in->cur = cur;
}

void ui_input_set(ui_input_t *in, const char *t) {
    sb_free(&in->v);
    in->cur = 0;
    ui_input_insert(in, t ? t : "");
}

const char *ui_input_value(const ui_input_t *in) { return in->v.s ? in->v.s : ""; }

void ui_input_free(ui_input_t *in) {
    sb_free(&in->v);
    in->cur = 0;
}

static bool is_word(char ch) { return isalnum((unsigned char)ch) || ((unsigned char)ch & 0x80) || ch == '_'; }

static size_t word_left(const char *s, size_t i) {
    while (i > 0 && !is_word(s[i - 1])) i--;
    while (i > 0 && is_word(s[i - 1])) i--;
    return i;
}

static size_t word_right(const char *s, size_t n, size_t i) {
    while (i < n && !is_word(s[i])) i++;
    while (i < n && is_word(s[i])) i++;
    return i;
}

bool ui_input_key(ui_input_t *in, const tui_key_t *k) {
    const char *s = in->v.s ? in->v.s : "";
    size_t n = in->v.len;
    if (k->kind == TUI_KEY_PASTE && k->text) {
        ui_input_insert(in, k->text);
        return true;
    }
    const char *t = tui_key_text(k);
    if (t && *t && (unsigned char)*t >= 0x20) {
        ui_input_insert(in, t);
        return true;
    }
    if (tui_kb(k, "tui.editor.deleteCharBackward") || tui_key_matches(k, "backspace")) input_delete(in, prev_cp(s, in->cur), in->cur);
    else if (tui_kb(k, "tui.editor.deleteCharForward") || tui_key_matches(k, "delete")) input_delete(in, in->cur, next_cp(s, n, in->cur));
    else if (tui_kb(k, "tui.editor.deleteWordBackward") || tui_key_matches(k, "ctrl+w") || tui_key_matches(k, "alt+backspace"))
        input_delete(in, word_left(s, in->cur), in->cur);
    else if (tui_kb(k, "tui.editor.deleteWordForward") || tui_key_matches(k, "alt+d")) input_delete(in, in->cur, word_right(s, n, in->cur));
    else if (tui_key_matches(k, "ctrl+u")) input_delete(in, 0, in->cur);
    else if (tui_key_matches(k, "ctrl+k")) input_delete(in, in->cur, n);
    else if (tui_key_matches(k, "left") || tui_key_matches(k, "ctrl+b")) in->cur = prev_cp(s, in->cur);
    else if (tui_key_matches(k, "right") || tui_key_matches(k, "ctrl+f")) in->cur = next_cp(s, n, in->cur);
    else if (tui_key_matches(k, "alt+left") || tui_key_matches(k, "ctrl+left") || tui_key_matches(k, "alt+b")) in->cur = word_left(s, in->cur);
    else if (tui_key_matches(k, "alt+right") || tui_key_matches(k, "ctrl+right") || tui_key_matches(k, "alt+f")) in->cur = word_right(s, n, in->cur);
    else if (tui_key_matches(k, "home") || tui_key_matches(k, "ctrl+a")) in->cur = 0;
    else if (tui_key_matches(k, "end") || tui_key_matches(k, "ctrl+e")) in->cur = n;
    else return false;
    return true;
}

char *ui_input_render(const ui_input_t *in, const char *prompt, int width, bool focused, bool hw_cursor) {
    int avail = width - tui_visible_width(prompt);
    if (avail <= 0) return tui_truncate_to_width(prompt, width, "");
    const char *s = in->v.s ? in->v.s : "";
    /* window the value around the cursor: the cursor cell (a trailing space at the end) stays visible */
    char *before_all = xstrndup(s, in->cur);
    int bw = tui_visible_width(before_all);
    size_t after_cur = next_cp(s, in->v.len, in->cur);
    char *at = in->cur < in->v.len ? xstrndup(s + in->cur, after_cur - in->cur) : xstrdup(" ");
    int aw = MAX(1, tui_visible_width(at));
    int skip = MAX(0, bw + aw - avail);
    char *before = tui_slice_cols(before_all, skip, bw - skip);
    int rem = MAX(0, avail - tui_visible_width(before) - aw);
    char *after = tui_slice_cols(in->cur < in->v.len ? s + after_cur : "", 0, rem);
    sb_t b = SB_INIT;
    sb_append(&b, prompt);
    sb_append(&b, before);
    if (focused) sb_append(&b, TUI_CURSOR_MARKER);
    if (hw_cursor) sb_append(&b, at);
    else sb_appendf(&b, "\x1b[7m%s\x1b[27m", at);
    sb_append(&b, after);
    int used = tui_visible_width(before) + aw + tui_visible_width(after);
    tui_sb_spaces(&b, MAX(0, avail - used));
    free(before_all), free(at), free(before), free(after);
    return sb_detach(&b);
}
