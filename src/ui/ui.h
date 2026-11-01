/* ui.h - shared state and building blocks of the interactive coding-agent UI (port of omp's
 * packages/tui chat/chrome/status-line/prompt layers). The controller lives in modes/interactive.c;
 * each visual area has its own module here:
 *   chat.c        transcript message views (user, assistant, notices)
 *   toolcards.c   tool execution cards
 *   statusline.c  status band / bottom bar, composer wiring, working-row title, key hints
 *   welcome.c     welcome banner (gradient logo + intro, wordmark, version, tip)
 */
#ifndef OMC_UI_H
#define OMC_UI_H

#include "coding/app.h"
#include "coding/goal.h"
#include "core/fs.h"
#include "tui/ansi.h"
#include "tui/components.h"
#include "tui/editor.h"
#include "tui/markdown.h"
#include "tui/render.h"
#include "tui/symbols.h"
#include "tui/theme.h"

#include <pthread.h>


typedef enum { MV_USER, MV_ASSISTANT, MV_TOOL, MV_NOTICE, MV_BASH } mv_kind_t;

typedef struct {
    tui_component_t base;
    mv_kind_t kind;
    json_t *msg;        /* assistant message (owned clone) */
    char *text;         /* user/notice text */
    const char *level;  /* notice level */
    /* tool */
    char *tool_name, *tool_id;
    json_t *args;
    char *output;
    bool done, is_error;
    /* cache */
    int cache_w;
    strvec_t cache;
    bool dirty;
} msgview_t;

typedef struct ui ui_t;
extern ui_t *g_ui; /* the running interactive UI (one per process) */

struct ui {
    app_t *app;
    tui_t *tui;
    tui_container_t *chat;
    tui_editor_t *editor;
    tui_loader_t *loader;
    tui_component_t *status;
    msgview_t *cur_assistant;
    hmap_t tools; /* toolCallId -> msgview_t* */
    pthread_t worker;
    bool worker_running;
    bool expand_tools;
    bool hide_thinking;
    int64_t last_ctrl_c;
    /* coalesced streaming updates */
    pthread_mutex_t mu;
    json_t *pending_partial;
    bool partial_posted;
    /* ask bridge */
    pthread_cond_t ask_cv;
    json_t *ask_questions;
    json_t *ask_answers;
    size_t ask_index;
    bool ask_done;
    tui_select_list_t *picker;
    char *goal_cont; /* pending hidden continuation prompt (goal / advisor) */
    const char *cont_type;
    int picker_kind; /* 1 model, 2 session, 3 ask, 4 thinking */
    json_t *picker_data;
    /* chrome (welcome.c / statusline.c) */
    tui_component_t *welcome;
    bool status_row_occupied; /* the working/idle-title row rendered lines this frame */
};


/* theme helpers */
const tui_theme_t *TH(void);
char *fg(tui_tc_t slot, const char *s); /* owned */
void push_wrapped(strvec_t *out, const char *text, int width, int indent);

/* chat.c */
msgview_t *mv_new(mv_kind_t k);
void add_view(ui_t *u, msgview_t *v);
void add_notice(ui_t *u, const char *level, const char *text);

/* toolcards.c */
void render_tool(msgview_t *v, int width, strvec_t *out);
char *args_summary(const char *tool, const json_t *args);

/* statusline.c */
tui_component_t *ui_status_new(ui_t *u);   /* standalone bottom bar (pi/borderless/claude/rule/field/rail) */
void ui_chrome_setup(ui_t *u);              /* composer shape, chrome colors, status top, hint, cursor */
void ui_chrome_turn(ui_t *u, bool active);  /* agent turn start/end: brand spinner, time_spent */
tui_component_t *ui_status_row_new(ui_t *u, tui_component_t *loader); /* loader, else idle title row */
tui_component_t *ui_editor_gap_new(ui_t *u); /* blank row above the editor (collapses under band) */
char *ui_title_trailer(ui_t *u);            /* band: dim italic session title for the working row */
const char *ui_composer_shape(void);        /* composer.shape setting (default band) */
char *ui_status_render(ui_t *u, int width, const char *layout); /* box|band|plain-full|plain-left|plain-right */
void ui_hint_record(const char *id);        /* composer hint gesture used ("effort", "agents") */
char *ui_key_hint(const char *key);         /* "shift+tab" -> "⇧⇥" (active symbol preset) */
char *ui_double_tap_hint(const char *key);

/* welcome.c */
tui_component_t *ui_welcome_new(void);
void ui_welcome_play_intro(tui_component_t *w, tui_t *tui);
void ui_welcome_set_tip(tui_component_t *w, const char *tip); /* fixed tip (tests); NULL = random */
void ui_welcome_tip_lines(const char *tip, int width, double phase, strvec_t *out);

#endif
