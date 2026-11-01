/* chat.c - transcript message views (user, assistant, notices). */
#include "ui/ui.h"

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

static void mv_render(tui_component_t *c, int width, strvec_t *out) {
    msgview_t *v = (msgview_t *)c;
    if (!v->dirty && v->cache_w == width) {
        for (size_t i = 0; i < v->cache.n; i++) strvec_push(out, v->cache.v[i]);
        return;
    }
    strvec_free(&v->cache);
    strvec_t *o = &v->cache;
    switch (v->kind) {
    case MV_USER: {
        strvec_t lines = {0};
        tui_md_opts_t mo;
        tui_md_opts_default(&mo);
        tui_md_render(v->text, width - 2, &mo, &lines);
        strvec_push(o, "");
        for (size_t i = 0; i < lines.n; i++) {
            char *l = xasprintf(" %s", lines.v[i]);
            char *p = tui_pad_to_width(l, width);
            strvec_push_own(o, tui_bgs(TH(), TUI_TC_USER_MESSAGE_BG, p));
            free(p);
            free(l);
        }
        strvec_free(&lines);
        break;
    }
    case MV_ASSISTANT: {
        json_t *content = json_get(v->msg, "content"), *blk;
        bool first = true;
        json_arr_foreach(content, i, blk) {
            const char *t = json_get_str(blk, "type");
            if (str_eq(t, "thinking")) {
                if (g_ui->hide_thinking) continue;
                const char *th = json_get_str(blk, "thinking");
                if (str_empty(th)) continue;
                if (first) strvec_push(o, "");
                first = false;
                strvec_t tl = {0};
                tui_wrap_text_with_ansi(th, width - 2, &tl);
                for (size_t k = 0; k < tl.n; k++) {
                    char *it = tui_italic(tl.v[k]);
                    char *c2 = fg(TUI_TC_THINKING_TEXT, it);
                    strvec_push_own(o, xasprintf(" %s", c2));
                    free(c2);
                    free(it);
                }
                strvec_free(&tl);
            } else if (str_eq(t, "text")) {
                const char *tx = json_get_str(blk, "text");
                if (str_empty(tx)) continue;
                if (first) strvec_push(o, "");
                first = false;
                tui_md_opts_t mo;
                tui_md_opts_default(&mo);
                strvec_t ml = {0};
                tui_md_render(tx, width - 2, &mo, &ml);
                for (size_t k = 0; k < ml.n; k++) strvec_push_own(o, xasprintf(" %s", ml.v[k]));
                strvec_free(&ml);
            }
        }
        const char *stop = json_get_str(v->msg, "stopReason");
        const char *err = json_get_str(v->msg, "errorMessage");
        if ((str_eq(stop, "error") || str_eq(stop, "aborted")) ) {
            char *m = xasprintf(" %s: %s", str_eq(stop, "aborted") ? "Aborted" : "Error", err ? err : stop);
            char *c2 = fg(str_eq(stop, "aborted") ? TUI_TC_WARNING : TUI_TC_ERROR, m);
            push_wrapped(o, c2, width, 0);
            free(c2);
            free(m);
        }
        break;
    }
    case MV_TOOL:
        strvec_push(o, "");
        render_tool(v, width, o);
        break;
    case MV_NOTICE: {
        tui_tc_t slot = str_eq(v->level, "error") ? TUI_TC_ERROR : str_eq(v->level, "warning") ? TUI_TC_WARNING : TUI_TC_MUTED;
        char *c2 = fg(slot, v->text);
        strvec_push(o, "");
        push_wrapped(o, c2, width, 1);
        free(c2);
        break;
    }
    case MV_BASH:
        strvec_push(o, "");
        render_tool(v, width, o);
        break;
    }
    v->cache_w = width;
    v->dirty = false;
    for (size_t i = 0; i < v->cache.n; i++) strvec_push(out, v->cache.v[i]);
}

static void mv_invalidate(tui_component_t *c) { ((msgview_t *)c)->dirty = true; }

static void mv_destroy(tui_component_t *c) {
    msgview_t *v = (msgview_t *)c;
    json_free(v->msg);
    free(v->text);
    free(v->tool_name);
    free(v->tool_id);
    json_free(v->args);
    free(v->output);
    strvec_free(&v->cache);
    free(v);
}

static const tui_component_vt MV_VT = {"message", mv_render, NULL, mv_invalidate, mv_destroy};

msgview_t *mv_new(mv_kind_t k) {
    msgview_t *v = xcalloc(1, sizeof(*v));
    tui_component_init(&v->base, &MV_VT);
    v->kind = k;
    v->dirty = true;
    return v;
}

void add_view(ui_t *u, msgview_t *v) {
    tui_container_add(u->chat, &v->base);
    tui_request_render(u->tui);
}

void add_notice(ui_t *u, const char *level, const char *text) {
    msgview_t *v = mv_new(MV_NOTICE);
    v->level = level;
    v->text = xstrdup(text);
    add_view(u, v);
}

