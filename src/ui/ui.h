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


typedef enum { MV_USER, MV_ASSISTANT, MV_TOOL, MV_NOTICE, MV_BASH, MV_CUSTOM, MV_SUMMARY, MV_TTSR, MV_TODO, MV_SKILL } mv_kind_t;

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
    tui_container_t *model_cycle; /* Ctrl+P role track (modelCycleContainer), cleared after 4 s */
    int model_cycle_timer;
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
    int64_t last_ctrl_c;      /* app.clear double-press window (500 ms) */
    int64_t last_escape;      /* app.interrupt double-press window (doubleEscapeAction) */
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
    json_t *att_images; /* pending composer image attachments [{image, path?, w?, h?}] (attach.c) */
    struct btw_ctl *btw;      /* /btw controller (btw.c), created on first use */
    /* message queue and background input work (queue.c, modes/interactive.c) */
    tui_container_t *pending_shell; /* `!`/`$` runs started while streaming (omp pendingShellContainer) */
    tui_component_t *queue_band;    /* ui_queue_band_new */
    strvec_t q_steer, q_follow;     /* band snapshot (ui_queue_refresh) */
    json_t *compaction_queue;       /* [{text, mode: steer|followUp}] typed while compacting / behind a yield compaction */
    char *yield_compaction;         /* `/compact …` queued with the follow-up key while streaming */
    char *compact_pending;          /* `/compact` sent with Enter mid-run: runs once the aborted turn unwinds */
    strvec_t optimistic;            /* submitted prompts already drawn: their user message_start is not drawn again */
    int bg_kind;                    /* running maintenance op: 0 none, 1 compaction, 2 handoff */
    /* iq_compact_then: runs once the manual compaction settles (ok: committed or nothing to compact) */
    void (*after_compact)(ui_t *u, bool ok, void *ud);
    void *after_compact_ud;
    pthread_t bg_thread;
    struct ui_bash *bash;           /* running `!cmd` */
    struct ui_bash *python;         /* running `$ code` */
    json_t *deferred_bash;          /* `!cmd` / `$ code` results that finished mid-run: recorded at the run's end */
    /* session focus (agenthub.c, SessionFocusController): the view shows a subagent's live session */
    app_t *view;                    /* pinned subagent session the transcript/status line show; NULL = main */
    char *focus_id;                 /* its registry id */
    struct agent_hub *hub;          /* open Agent Hub overlay (agenthub.c) */
    tui_component_t *subagent_hud;  /* "Subagents · N running" block above the editor */
    int64_t last_left_tap;          /* ←← double-tap detector (LEFT_DOUBLE_TAP_*) */
    int left_taps;
    bool agents_dirty;              /* registry changed: badge / HUD / hub refresh pending (posted once) */
};

/* the session the view shows: the focused subagent's, else the main one */
static inline app_t *ui_view_app(const ui_t *u) { return u->view ? u->view : u->app; }


/* attach.c - composer image attachments (pasted / dropped image paths, clipboard images, chips band) */
tui_component_t *ui_attach_band_new(ui_t *u); /* also installs the editor paste hook */
bool ui_attach_paste_clipboard(ui_t *u);      /* app.clipboard.pasteImage (ctrl+v) */
void ui_attach_paste_raw(ui_t *u);            /* app.clipboard.pasteTextRaw (ctrl+shift+v): clipboard text inline */
char *ui_paste_attachment_sgr(int n);         /* attachmentSgr("paste", n) */
/* Drop unreferenced attachments, renumber `[Image #N...]` markers densely; returns the kept entries
 * ([{image, path?}], NULL when none) and the rewritten text (owned). Clears the pending list. */
json_t *ui_attach_take(ui_t *u, const char *text, char **out_text);
void ui_attach_clear(ui_t *u);
bool ui_extract_image_paths(const char *text, strvec_t *out);
char *ui_attachment_sgr(int n);
char *ui_chip_label(int n);
/* `[Image #N, WxH]` markers as chips; `reset` (may be NULL: plain labels) resumes the surrounding color */
char *ui_collapse_image_markers(const char *text, const char *reset);

/* hooksel.c - showHookSelector with option descriptions and a custom helpText (ui_hook_select in planreview.c
 * covers plain label lists): options in the composer's place; `cb` gets the chosen index, -1 on cancel. `help`
 * replaces the default controls hint (NULL = default). Title lines after the first are accent detail rows. */
void ui_hook_select_desc(ui_t *u, const char *title, const char *const *labels, const char *const *descs, size_t n,
                         const char *help, void (*cb)(void *ud, int choice), void *ud);

/* histsearch.c - Ctrl+R prompt history search (overlays/history-search.ts) over prompt-history.jsonl */
void ui_history_search(ui_t *u);
typedef struct {
    char *prompt;
    int64_t created_at; /* unix seconds of the latest submission */
    char *cwd;
} ui_hist_entry_t;
typedef VEC(ui_hist_entry_t) ui_hist_list_t;
/* HistoryStorage over the jsonl lines: unique normalized prompts, latest submission first. `query` NULL/blank:
 * getRecent; else search (every token a case-insensitive substring). At most `limit` entries. */
void ui_hist_query(const char *jsonl, const char *query, int limit, ui_hist_list_t *out);
void ui_hist_list_free(ui_hist_list_t *l);

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
void ui_open_in_browser(const char *url_or_path); /* openPath: xdg-open/open, detached; OMC_NO_BROWSER skips */
/* chrome/segment-track.ts renderSegmentTrack (modelhub.c); owned */
char *ui_segment_track(const char *const *labels, size_t n, int active);
/* resolveSegmentPalette: up to min(n, 12) distinct theme colors (tui_tc_t) into out[12]; returns the count */
size_t ui_segment_palette(size_t n, int *out);
/* `• Background job completed …` rows for a delivered async-result prompt (no-op for other messages) */
void ui_async_result(ui_t *u, const json_t *msg);
void ui_retry_recovered(ui_t *u, const char *notes); /* "\n"-separated notes for the failed attempts */
void ui_retry_failed(ui_t *u, int attempts, const char *final_error, const char *error);
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
bool ui_working_busy(ui_t *u); /* a retry countdown or compaction row replaces the working/title row */
void ui_chat_rebuild(ui_t *u); /* replay the transcript from the session tree (resume, branch, reload) */
/* replay transcript entries (message / custom_message, one path) into `dest`, a container apart from the
 * live transcript (rewind selector replica); ends[i] receives dest's child count after entries[i] */
void ui_chat_replay_into(ui_t *u, tui_container_t *dest, const json_t *entries, size_t *ends);
/* toolcards.c: the open read group (omp #lastReadGroup); returns the previous one */
msgview_t *toolcard_group_swap(msgview_t *group);

/* toolcards.c - tool cards (MV_TOOL views keep their card state in `msg`) */
void render_tool(msgview_t *v, int width, strvec_t *out);
char *args_summary(const char *tool, const json_t *args);
/* tool_execution_start: creates the card (or joins the current read group); takes `args`. */
msgview_t *toolcard_start(ui_t *u, const char *id, const char *name, json_t *args);
/* partial/final result {content, details} for call `id` on card `v`; takes `result`. */
void toolcard_result(ui_t *u, msgview_t *v, const char *id, json_t *result, bool partial, bool is_error);
/* a visible text/thinking block, user message or non-read tool ends the current read group */
void toolcard_break_group(ui_t *u);
/* the read group later reads still join (omp: a ReadToolGroup is unfinalized until the run breaks) */
bool toolcard_group_open(const msgview_t *v);
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
/* $VISUAL / $EDITOR on a temp file with the TUI stopped (interactive.c); owned result, NULL when it failed or
 * exited non-zero; *no_editor when none is configured */
char *ui_external_edit(ui_t *u, const char *text, const char *ext, bool *no_editor);
char *ui_editor_command(void); /* getEditorCommand: trimmed $VISUAL, else $EDITOR; owned or NULL */
/* openEditorOnPath: `ed` on `path` with the TUI handed over; the editor's exit code (-1 when it did not start) */
int ui_editor_on_path(ui_t *u, const char *ed, const char *path);
/* move.c: the /move overlay (move-overlay.ts); done(dir or NULL on cancel) runs once on the UI thread */
tui_component_t *ui_move_overlay_new(const char *cwd, void (*done)(void *ud, const char *dir), void *ud);
/* editDialogExternally: openInEditor(text, ".md") with the trailing newline trimmed; NULL when no editor is
 * configured or it exited non-zero (no warning) */
char *ui_dialog_external_edit(ui_t *u, const char *text);
int64_t ui_context_used(app_t *a);         /* the context gauge's used tokens (anchored on provider usage) */

/* btw.c - /btw side questions: inline answer panel, BTW history, follow-ups, copy, branch */
tui_component_t *ui_btw_container_new(ui_t *u); /* the inline panel's slot (between transcript and banner) */
/* /btw <question>: ask (true when it started); an empty question opens the BTW history. `title` heads the
 * panel instead of `/btw <question>` (side commands); images may be NULL. */
bool ui_btw_command(ui_t *u, const char *question, const json_t *images, const char *title);
bool ui_btw_key(ui_t *u, const tui_key_t *k); /* Esc while the panel shows; c/f/b on an empty editor */
void ui_btw_dispose(ui_t *u);                 /* session switch / fork / exit: cancel, hide, forget */
void ui_btw_free(ui_t *u);
/* queue.c */
tui_component_t *ui_queue_band_new(ui_t *u); /* "Steering · N" / "After yield · N" band */
void ui_queue_refresh(ui_t *u);              /* re-read the agent + compaction queues into the band */
const char *ui_queue_shorthand(const char *text); /* body after a leading `->` / `=>`, else NULL */
void ui_split_queued_messages(const char *text, strvec_t *out); /* queue-input.ts splitQueuedMessages */

/* agenthub.c - subagent HUD, Agent Hub (/hub, Alt+A, ←←), transcript viewer, session focus */
tui_component_t *ui_subagent_hud_new(ui_t *u);   /* "Subagents · N running" block (display.pinnedAgents) */
/* open the hub (Agents, or the Activity tab); `require_content`: stay closed without subagents (←←);
 * `arm_close`: the opening ←← also arms the hub's own ←← close */
void ui_agent_hub_open(ui_t *u, bool activity, bool require_content, bool arm_close);
bool ui_focus_agent(ui_t *u, const char *id, char **err); /* view a subagent's live session (revives it) */
void ui_unfocus(ui_t *u, const char *status);              /* back to main; `status` (may be NULL) is shown */
void ui_left_tap(ui_t *u);                                 /* ← on an empty composer (double tap gesture) */
void ui_agents_init(ui_t *u);                              /* registry listener: badge / HUD / hub / focus refresh */
void ui_agents_shutdown(ui_t *u);
size_t ui_background_job_count(ui_t *u);                   /* statusline.c: running jobs outside the subagent badge */
/* test hooks: pi-utils formatDuration / formatNumber / formatAge (v = ms / n / s), hub cost (v = micro-dollars);
 * formatMetricColumns of a roster ref; an agent's activity rows */
char *ui_hub_format(const char *what, int64_t v);
char *ui_hub_metric_columns(const json_t *ref, const char *age);
json_t *ui_hub_activity_rows(const json_t *ref, const json_t *messages);
/* interactive.c: (un)subscribe the transcript's agent-event listener on a session */
void ix_view_subscribe(ui_t *u, app_t *a, bool on);

/* mcpcmd.c - /mcp (MCPCommandController) and sshcmd.c - /ssh (SSHCommandController); `text` is the whole line */
void ui_mcp_command(ui_t *u, const char *text);
void ui_ssh_command(ui_t *u, const char *text);
void ui_mcp_install(ui_t *u);                         /* live MCP tool / prompt refresh, mcp.notifications */
bool ui_mcp_escape(ui_t *u);                          /* Esc: stop an MCP OAuth wait / cancel /mcp test */
bool ui_mcp_manual_input(ui_t *u, const char *value); /* `/login <value>` for a waiting MCP OAuth flow */

/* welcome.c */
tui_component_t *ui_welcome_new(void);
void ui_welcome_play_intro(tui_component_t *w, tui_t *tui);
void ui_welcome_set_tip(tui_component_t *w, const char *tip); /* fixed tip (tests); NULL = random */
void ui_welcome_tip_lines(const char *tip, int width, double phase, strvec_t *out);

#endif
