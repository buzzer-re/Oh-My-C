/* tui_demo.c - interactive manual test for src/tui: editor + streamed markdown + loader.
 *
 * Build:
 *   gcc -std=gnu11 -Isrc -o tui_demo tests/tui_demo.c src/tui/[all].c src/core/core.c \
 *       src/core/json.c src/core/fs.c -lpthread -lm
 *
 * Type a message and press Enter: a fake assistant reply streams in from a worker thread
 * (exercising tui_post). Shift/Alt+Enter inserts a newline, "/" opens command completion,
 * "@" completes file paths, "/select" opens an overlay list, ctrl+z suspends.
 * ctrl+c quits; ctrl+d quits when the editor is empty.
 */
#include "core/core.h"
#include "tui/components.h"
#include "tui/editor.h"
#include "tui/markdown.h"
#include "tui/render.h"

#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    tui_t *tui;
    tui_container_t *chat;
    tui_editor_t *editor;
    tui_loader_t *loader;
    tui_markdown_t *current; /* reply being streamed */
    tui_select_list_t *picker;
    pthread_t worker;
    bool worker_running;
    atomic_bool cancel;
    char *prompt;
} app_t;

static const char *REPLY =
    "## Answer\n\n"
    "You said: **%s**\n\n"
    "Here is some *markdown* with `inline code`, a [link](https://example.com) and a list:\n\n"
    "1. First item\n"
    "2. Second item\n"
    "   - nested bullet\n\n"
    "```c\n"
    "#include <stdio.h>\n"
    "int main(void) {\n"
    "    printf(\"hello %%d\\n\", 42); // comment\n"
    "    return 0;\n"
    "}\n"
    "```\n\n"
    "| Feature | Status |\n"
    "|---------|:------:|\n"
    "| Diff render | ok |\n"
    "| Overlays | ok |\n\n"
    "> Quoted text that is long enough to wrap across multiple lines when the terminal is narrow.\n\n"
    "---\n"
    "Done.";

typedef struct {
    app_t *app;
    char *delta;
    bool done;
} chunk_t;

static void on_chunk(void *ud) {
    chunk_t *c = ud;
    app_t *a = c->app;
    if (a->current && c->delta) tui_markdown_append(a->current, c->delta);
    if (c->done) {
        if (a->loader) {
            tui_loader_stop(a->loader);
            tui_container_remove(a->chat, &a->loader->base);
            tui_component_destroy(&a->loader->base);
            a->loader = NULL;
        }
        a->current = NULL;
        a->editor->disable_submit = false;
    }
    free(c->delta);
    free(c);
}

static void *worker_main(void *ud) {
    app_t *a = ud;
    char *text = xasprintf(REPLY, a->prompt);
    size_t n = strlen(text), i = 0;
    while (i < n && !atomic_load(&a->cancel)) {
        size_t step = 3 + (size_t)(rand() % 8);
        if (i + step > n) step = n - i;
        chunk_t *c = xcalloc(1, sizeof *c);
        c->app = a;
        c->delta = xstrndup(text + i, step);
        i += step;
        tui_post(a->tui, on_chunk, c);
        sleep_ms(15);
    }
    chunk_t *c = xcalloc(1, sizeof *c);
    c->app = a;
    c->done = true;
    tui_post(a->tui, on_chunk, c);
    free(text);
    return NULL;
}

static void join_worker(app_t *a) {
    if (!a->worker_running) return;
    atomic_store(&a->cancel, true);
    pthread_join(a->worker, NULL);
    a->worker_running = false;
    atomic_store(&a->cancel, false);
}

static void picker_select(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    app_t *a = ud;
    char *msg = xasprintf("Picked: %s", it->value);
    tui_container_add(a->chat, &tui_text_new(msg, 1, 0)->base);
    free(msg);
    tui_overlay_hide(a->tui, &l->base);
}

static void picker_cancel(tui_select_list_t *l, void *ud) {
    app_t *a = ud;
    tui_overlay_hide(a->tui, &l->base);
}

static void open_picker(app_t *a) {
    if (!a->picker) {
        a->picker = tui_select_list_new(6);
        const char *models[] = {"claude-opus", "claude-sonnet", "claude-haiku", "gpt-5", "gemini-pro", "llama",
                                "mistral", "qwen"};
        for (size_t i = 0; i < ARRAY_LEN(models); i++) tui_select_list_add(a->picker, models[i], NULL, "a model");
        a->picker->on_select = picker_select;
        a->picker->on_cancel = picker_cancel;
        a->picker->ud = a;
    }
    tui_select_list_set_filter(a->picker, NULL);
    tui_overlay_opts_t o = {TUI_ANCHOR_BOTTOM, -60, 10, 0, 1};
    tui_overlay_show(a->tui, &a->picker->base, &o);
}

static void on_submit(tui_editor_t *ed, const char *text, void *ud) {
    app_t *a = ud;
    if (!*text) return;
    if (!strcmp(text, "/quit") || !strcmp(text, "/exit")) {
        tui_stop(a->tui, 0);
        return;
    }
    if (!strcmp(text, "/clear")) {
        join_worker(a);
        a->current = NULL;
        a->loader = NULL;
        tui_container_clear(a->chat);
        return;
    }
    if (!strcmp(text, "/select")) {
        open_picker(a);
        return;
    }
    if (!strcmp(text, "/help")) {
        tui_markdown_t *m = tui_markdown_new(
            "**Keys**: Enter submit, Shift/Alt+Enter newline, Ctrl+C quit, Ctrl+D quit (empty), Ctrl+Z suspend.\n"
            "Commands: `/select`, `/clear`, `/quit`.",
            1, 0);
        tui_container_add(a->chat, &m->base);
        return;
    }
    join_worker(a);
    tui_text_t *user = tui_text_new(text, 1, 1);
    tui_text_set_bg(user, TUI_TC_USER_MESSAGE_BG);
    tui_container_add(a->chat, &tui_spacer_new(1)->base);
    tui_container_add(a->chat, &user->base);
    a->current = tui_markdown_new("", 1, 0);
    tui_container_add(a->chat, &tui_spacer_new(1)->base);
    tui_container_add(a->chat, &a->current->base);
    a->loader = tui_loader_new(a->tui, "Thinking...");
    tui_container_add(a->chat, &a->loader->base);
    tui_loader_start(a->loader);
    free(a->prompt);
    a->prompt = xstrdup(text);
    ed->disable_submit = true;
    a->worker_running = pthread_create(&a->worker, NULL, worker_main, a) == 0;
}

static bool key_hook(tui_t *t, const tui_key_t *k, void *ud) {
    app_t *a = ud;
    if (tui_key_matches(k, "ctrl+c")) {
        if (tui_overlay_visible(t)) return false; /* let the overlay cancel */
        tui_stop(t, 0);
        return true;
    }
    if (tui_key_matches(k, "ctrl+d") && tui_editor_is_empty(a->editor) && !tui_overlay_visible(t)) {
        tui_stop(t, 0);
        return true;
    }
    if (tui_key_matches(k, "ctrl+z")) {
        tui_suspend(t);
        return true;
    }
    return false;
}

int main(void) {
    app_t a;
    memset(&a, 0, sizeof a);
    a.tui = tui_new(NULL);
    tui_term_set_title(tui_term(a.tui), "omc tui demo");

    tui_container_t *root = tui_root(a.tui);
    tui_markdown_t *header =
        tui_markdown_new("**omc tui demo** - type a message, `/help` for commands, Ctrl+C to quit.", 1, 0);
    tui_container_add(root, &header->base);
    a.chat = tui_container_new();
    tui_container_add(root, &a.chat->base);
    tui_container_add(root, &tui_spacer_new(1)->base);

    a.editor = tui_editor_new();
    tui_editor_set_on_submit(a.editor, on_submit, &a);
    tui_editor_set_placeholder(a.editor, "Ask anything...");
    tui_ac_default_t *ac = tui_ac_default_new(NULL);
    tui_ac_default_add_command(ac, "help", "Show help");
    tui_ac_default_add_command(ac, "select", "Open the model picker overlay");
    tui_ac_default_add_command(ac, "clear", "Clear the conversation");
    tui_ac_default_add_command(ac, "quit", "Exit the demo");
    tui_editor_set_autocomplete(a.editor, tui_ac_default_provider, ac);
    tui_container_add(root, &a.editor->base);
    tui_set_focus(a.tui, &a.editor->base);
    tui_set_key_hook(a.tui, key_hook, &a);

    int code = tui_run(a.tui);
    join_worker(&a);
    if (a.picker && tui_overlay_visible(a.tui)) tui_overlay_hide(a.tui, &a.picker->base);
    if (a.picker) tui_component_destroy(&a.picker->base);
    tui_free(a.tui);
    tui_ac_default_free(ac);
    free(a.prompt);
    printf("bye (exit %d)\n", code);
    return code;
}
