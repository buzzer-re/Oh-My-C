/* ui.h - shared state and building blocks of the interactive coding-agent UI (port of omp's
 * packages/tui chat/chrome/status-line/prompt layers). The controller lives in modes/interactive.c;
 * each visual area has its own module here:
 *   chat.c        transcript (blank-row separated blocks), message views (user, assistant, notices,
 *                 custom/summary/TTSR), pinned error banner, retry/compaction rows, transcript replay
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
#include "ui/overlays.h"

#include <pthread.h>


typedef enum { MV_USER, MV_ASSISTANT, MV_TOOL, MV_NOTICE, MV_BASH, MV_CUSTOM, MV_SUMMARY, MV_TTSR } mv_kind_t;

typedef struct {
    tui_component_t base;
    mv_kind_t kind;
    json_t *msg;        /* assistant / custom message or summary entry (owned clone) */
    char *text;         /* user/notice text */
    const char *level;  /* notice level ("info", "warning", "error"); summary kind ("compaction", "branch") */
    bool synthetic;     /* user: agent-attributed input (dim) */
    bool streaming;     /* assistant: still receiving deltas */
    bool error_pinned;  /* assistant: its error is shown in the banner instead of inline */
    char *retry_note;   /* assistant: failed attempt recovered by a retry ("switched model; retried") */
    size_t revealed;    /* assistant: smooth streaming reveal position (codepoints of visible text) */
    int reveal_timer;
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
    tui_container_t *chat;    /* ui_transcript_new() */
    tui_container_t *banner;  /* pinned error banner, between the transcript and the working row */
    tui_component_t *working; /* ui_working_new(): retry countdown / compaction rows (chat.c) */
    tui_editor_t *editor;
    tui_loader_t *loader;     /* the "Working…" row, drawn by the status row (statusline.c) */
    tui_component_t *status;
    msgview_t *cur_assistant;
    msgview_t *pinned_error; /* assistant view whose error the banner shows */
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
/* transcript container: children render as blocks (blank edge rows trimmed) separated by one blank row;
 * lead_gap also puts a blank row above the first block */
tui_container_t *ui_transcript_new(bool lead_gap);
msgview_t *mv_new(mv_kind_t k);
void add_view(ui_t *u, msgview_t *v);
/* "info" -> dim status line (consecutive ones update in place), "warning" -> "Warning: …",
 * "error" -> "Error: …"; "Injecting rule: …" becomes a TTSR notice, "Retrying (n/m) in Ns…" the retry row */
void add_notice(ui_t *u, const char *level, const char *text);
/* `• Background job completed …` rows for a delivered async-result prompt (no-op for other messages) */
void ui_async_result(ui_t *u, const json_t *msg);
void ui_retry_recovered(ui_t *u, const char *notes); /* "\n"-separated notes for the failed attempts */
void ui_assistant_streamed(ui_t *u, msgview_t *v); /* message_update applied: drive the smooth reveal */
void ui_assistant_end(ui_t *u, msgview_t *v);      /* message_end: pin a provider error in the banner */
void ui_tool_started(ui_t *u, const char *tool_call_id); /* tool card added: post-tool text follows */
void ui_agent_start(ui_t *u);                 /* agent_start: unpin/clear the banner, start the working row */
void ui_error_banner_show(ui_t *u, const char *message);
void ui_error_banner_clear(ui_t *u);
tui_component_t *ui_working_new(ui_t *u);
void ui_working_start(ui_t *u);
void ui_working_stop(ui_t *u);
void ui_working_set_message(ui_t *u, const char *message);
void ui_working_retry(ui_t *u, int attempt, int max, int64_t delay_ms);
void ui_working_compaction(ui_t *u, const char *label);
void ui_chat_rebuild(ui_t *u); /* replay the transcript from the session tree (resume, branch, reload) */

/* toolcards.c - tool cards (MV_TOOL views keep their card state in `msg`) */
void render_tool(msgview_t *v, int width, strvec_t *out);
char *args_summary(const char *tool, const json_t *args);
/* tool_execution_start: creates the card (or joins the current read group); takes `args`. */
msgview_t *toolcard_start(ui_t *u, const char *id, const char *name, json_t *args);
/* partial/final result {content, details} for call `id` on card `v`; takes `result`. */
void toolcard_result(ui_t *u, msgview_t *v, const char *id, json_t *result, bool partial, bool is_error);
/* a visible text/thinking block, user message or non-read tool ends the current read group */
void toolcard_break_group(ui_t *u);
/* streamed/final assistant message: a new visible text/thinking block ends the read group */
void toolcard_note_message(ui_t *u, const json_t *msg, bool final);
/* transcript rebuild: cards for an assistant message's toolCall blocks, results from toolResult messages */
void toolcard_replay(ui_t *u, const json_t *msg);
/* a later result frame of a background call (async task progress); takes `details`. A frame whose
 * details.async.state is running keeps the card live; completed/failed/cancelled settles it. */
void toolcard_frame(ui_t *u, const char *id, json_t *details);
/* history rebuild in progress: persisted background results render settled, not live */
void toolcard_set_replaying(bool on);
/* a tool call streaming in an assistant message: draws (or updates) its pending card before execution;
 * toolcard_start then adopts that card */
void toolcard_preview(ui_t *u, const char *id, const char *name, const json_t *args);

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
