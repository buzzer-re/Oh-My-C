/* interactive.h - private interface between the interactive controller (interactive.c) and its input
 * queue controller (iqueue.c: message queue, follow-up/dequeue/interrupt keys, `!cmd`, /compact,
 * /handoff and /queue off the UI thread). Everything here runs on the UI thread. */
#ifndef OMC_MODES_INTERACTIVE_H
#define OMC_MODES_INTERACTIVE_H

#include "ui/ui.h"

/* interactive.c */
void ix_start_prompt(ui_t *u, const char *text);      /* draw the user message, start a run */
bool ix_handle_command(ui_t *u, const char *line);    /* built-in slash command; false if unknown */
void ix_start_queued_run(ui_t *u);                    /* run that delivers the agent's queued messages */
void ix_start_hidden(ui_t *u, const char *text, const char *ctype); /* synthetic prompt (not drawn) */
void ix_history_add(ui_t *u, const char *text);       /* editor + persisted prompt history */

/* iqueue.c */
void iq_submit(ui_t *u, const char *text);            /* Enter (text already trimmed and taken from the editor) */
void iq_follow_up(ui_t *u);                           /* app.message.followUp (ctrl+q / ctrl+enter) */
void iq_dequeue(ui_t *u);                             /* app.message.dequeue (alt+up / shift+up) */
bool iq_escape(ui_t *u);                              /* app.interrupt; false when Esc has nothing to do here */
bool iq_run_end(ui_t *u);                             /* a run's worker finished; true if it started follow-on work */
void iq_user_message_start(ui_t *u, const json_t *msg); /* user message_start: draw it, refresh the band */
void iq_compact(ui_t *u, const char *arg);            /* /compact [instructions] (Enter) */
void iq_handoff(ui_t *u, const char *arg);            /* /handoff */
void iq_queue_command(ui_t *u, const char *text);     /* /queue <message> and the `->` / `=>` shorthand */
bool iq_busy(ui_t *u);                                /* a compaction/handoff runs (input queues behind it) */
void iq_shutdown(ui_t *u);                            /* cancel and join background work */

#endif
