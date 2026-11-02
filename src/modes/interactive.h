/* interactive.h - private interface between the interactive controller (interactive.c) and its input
 * queue controller (iqueue.c: message queue, follow-up/dequeue/interrupt keys, `!cmd`, /compact,
 * /handoff and /queue off the UI thread). Everything here runs on the UI thread. */
#ifndef OMC_MODES_INTERACTIVE_H
#define OMC_MODES_INTERACTIVE_H

#include "ui/ui.h"

/* interactive.c */
void ix_start_prompt(ui_t *u, const char *text);      /* draw the user message, start a run */
void ix_start_prompt_with(ui_t *u, const char *text, json_t *attachments); /* ... with staged images (owned) */
bool ix_handle_command(ui_t *u, const char *line);    /* built-in slash command; false if unknown */
void ix_start_queued_run(ui_t *u);                    /* run that delivers the agent's queued messages */
void ix_start_hidden(ui_t *u, const char *text, const char *ctype); /* synthetic prompt (not drawn) */
void ix_history_add(ui_t *u, const char *text);       /* editor + persisted prompt history */
/* #runSideFileCommand: a `side: true` file command answers as a /btw side question; false otherwise */
bool ix_side_file_command(ui_t *u, const char *text);
bool ix_is_skill_command(ui_t *u, const char *text);  /* isKnownSkillCommand */
/* #invokeSkillCommand: build the skill-prompt message and run it (or queue it as a steer / follow-up while
 * streaming); a skill that fails to load reports "Failed to load skill: ..." and restores the draft */
void ix_invoke_skill(ui_t *u, const char *text, json_t *attachments, bool follow_up);

/* iqueue.c */
/* Enter (text already trimmed and taken from the editor; attachments: staged images [{image, path?}], owned) */
void iq_submit(ui_t *u, const char *text, json_t *attachments);
/* ... `loop_body`: the inline prompt of `/loop … <prompt>` (submittedMode "loop": it becomes the loop prompt
 * even when it steers a running turn or queues behind a compaction) */
void iq_submit_mode(ui_t *u, const char *text, json_t *attachments, bool loop_body);
void iq_follow_up(ui_t *u);                           /* app.message.followUp (ctrl+q / ctrl+enter) */
void iq_dequeue(ui_t *u);                             /* app.message.dequeue (alt+up / shift+up) */
bool iq_escape(ui_t *u);                              /* app.interrupt; false when Esc has nothing to do here */
void iq_flush_deferred(ui_t *u);                      /* record `!`/`$` results that finished mid-run */
bool iq_run_end(ui_t *u);                             /* a run's worker finished; true if it started follow-on work */
void iq_user_message_start(ui_t *u, const json_t *msg); /* user message_start: draw it, refresh the band */
/* interactive-mode.ts showModelCycleTrack: the cycleOrder segment track above the composer for 4 s */
void ui_show_model_cycle_track(ui_t *u, int active);
void iq_compact(ui_t *u, const char *arg);            /* /compact [instructions] (Enter) */
/* handleCompactCommand(..., onBeforeFlush): compact with the current model, then then(u, ok, ud) on the UI thread
 * (ok: the compaction committed or there was nothing to compact; false when it failed, was cancelled or busy) */
void iq_compact_then(ui_t *u, void (*then)(ui_t *u, bool ok, void *ud), void *ud);
void iq_handoff(ui_t *u, const char *arg);            /* /handoff */
void iq_queue_command(ui_t *u, const char *text);     /* /queue <message> and the `->` / `=>` shorthand */
bool iq_busy(ui_t *u);                                /* a compaction/handoff runs (input queues behind it) */
void iq_shutdown(ui_t *u);                            /* cancel and join background work */
/* hold typed input behind other background work (branch summary) like a compaction; release delivers it */
void iq_hold(ui_t *u, bool on);

/* treectl.c (SelectorController.showTreeSelector / showUserMessageSelector) */
void tc_tree_open(ui_t *u);   /* /tree: session tree selector, optional branch summary */
void tc_rewind_open(ui_t *u); /* /branch, /rewind, doubleEscapeAction: fullscreen rewind selector */
bool tc_escape(ui_t *u);      /* app.interrupt while a branch summary runs: abort it */
void tc_shutdown(ui_t *u);    /* join a running branch summary */

/* loopctl.c (InteractiveMode loop mode: /loop, setLoopPrompt, pauseLoop, the auto-resubmit) */
char *lp_command(ui_t *u, const char *args);     /* handleLoopCommand: toggle; the inline prompt (owned) or NULL */
bool lp_enabled(ui_t *u);
void lp_set_prompt(ui_t *u, const char *prompt); /* setLoopPrompt (loop mode on only) */
bool lp_escape(ui_t *u);                         /* app.interrupt with loop mode on: abort the turn, pause */
bool lp_run_end(ui_t *u);                        /* a run ended idle: schedule the next iteration; true when loop mode is on */
char *lp_autocomplete_description(ui_t *u);      /* /loop's live description (owned) */
void lp_shutdown(ui_t *u);

/* copyctl.c (builtin-collaboration.ts copy / open, SelectorController.showCopySelector) */
void cc_copy_command(ui_t *u, const char *arg); /* /copy [code|cmd|link]: picker or the latest target */
void cc_open_command(ui_t *u, const char *arg); /* /open [link] */
void cc_shutdown(ui_t *u);

#endif
