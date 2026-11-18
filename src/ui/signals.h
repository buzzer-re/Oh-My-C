/*
 * signals.h - turn-lifecycle signals of the interactive UI (ports of omp utils/title-generator.ts terminal
 * title runtime, utils/run-status.ts OSC 7501 program status and modes/controllers/event-controller.ts
 * send{Completion,Error}Notification / tools/ask.ts #sendAskNotification).
 *
 * Terminal title: `π <sep> <label>` where label is the session name, else the cwd basename, and the separator
 * carries the run state: `>` idle (your turn), an 80 ms spinner while working (`tui.titleSpinner`: braille,
 * pulse, dots, line; a static `:` under WSL), `!` when the agent waits on you; `π: label` with
 * `tui.titleState` off. The shell's title is pushed (CSI 22;2t) when the UI claims the terminal and popped
 * (OSC 0 clear + CSI 23;2t) at teardown. Writes are deduplicated and only go to a TTY.
 *
 * Program status (`terminal.programStatus`): `ESC]7501;state=<idle|working|blocked|done|error>[:kind=…]
 * :app=omc[:msg=<base64>]ESC\`, repeated statuses write nothing, `state=clear` at teardown or when the
 * setting turns off.
 *
 * Everything here runs on the UI thread.
 */
#ifndef OMC_UI_SIGNALS_H
#define OMC_UI_SIGNALS_H

#include "core/core.h"
#include "core/json.h"

typedef struct ui ui_t;

typedef enum { RUN_IDLE, RUN_WORKING, RUN_BLOCKED, RUN_DONE, RUN_ERROR } run_state_t;

/* pushTerminalTitle + initTerminalTitleState + initProgramStatus + the first session title */
void ui_signals_init(ui_t *u);
/* disposeTerminalTitleState + disposeProgramStatus + popTerminalTitle (UI teardown) */
void ui_signals_dispose(void);
/* setSessionTerminalTitle from the live session name / cwd (rename, new session, resume, title generated) */
void ui_signals_session_title(void);
/* before ui_signals_init: a command-line prompt runs once startup ends, so report `working` from the first frame */
void ui_signals_set_startup_run(bool on);
/* settleStartupRun: end that hold once the startup prompts are dispatched; a run that reported keeps its status */
void ui_signals_settle_startup(void);
/* setRunStatus; kind ("question" | "permission") and msg only for blocked / error */
void ui_signals_run_status(run_state_t state, const char *kind, const char *msg);
/* back from a suspend (Ctrl+Z / fg): re-send an idle / working / blocked record */
void ui_signals_resumed(void);
/* the settings may have changed (tui.titleState, tui.titleSpinner, terminal.programStatus) */
void ui_signals_settings_changed(void);

/* agent events (event-controller.ts): agent_start -> working; an `ask` tool call blocks on a question until
 * its tool_execution_end */
void ui_signals_agent_start(void);
void ui_signals_tool_start(const char *tool_call_id, const char *tool_name, const json_t *args);
void ui_signals_tool_end(const char *tool_call_id, const char *tool_name);
/* a tool call waits on an approval prompt (#blockingPrompt kind permission, msg formatApprovalPrompt) until its
 * tool_execution_end; Warp permission_request / permission_replied (tool_approval_requested / _resolved) */
void ui_signals_approval_requested(const char *tool_call_id, const char *tool_name, const char *msg);
/* an approval answered: its blocker is released at once (omp 46288e029d / 3dcc5c0106: an approval reports `working` as
 * soon as it is answered, not when its tool ends) */
void ui_signals_approval_resolved(const char *tool_call_id);
/* the run settled (its last agent_end): run status done/error/idle from the run's messages, then the error
 * and completion notifications (`error.notify`, `completion.notify`) */
void ui_signals_run_settled(const json_t *run_messages, bool interrupted);
/* agent_end keeps its messages; the worker's end settles the run with them (continuations of one prompt —
 * stop recoveries, plan reminders — are non-terminal settles in omp) */
void ui_signals_agent_end(const json_t *run_messages);
void ui_signals_settle(void);
/* Warp CLI-agent events (WARP_CLI_AGENT_PROTOCOL_VERSION >= 1): `\x1b]777;notify;warp://cli-agent;{json}\x07`
 * for session_start (also after a session switch), prompt_submit (user-authored message_start), question_asked,
 * tool_complete and the run's stop / stop_failure */
void ui_signals_session_start(void);
void ui_signals_user_message(const json_t *msg);
/* the ask tool is waiting for an answer (`ask.notify`) */
void ui_signals_ask_waiting(void);

/* recap.c - the idle recap (`recap.enabled`, `recap.idleSeconds`) */
void ui_recap_init(ui_t *u);
void ui_recap_replay(ui_t *u);                     /* a transcript replay re-shows the newest journaled recap still current */
void ui_recap_settled(const json_t *run_messages); /* a run settled: arm the recap of this stretch */
void ui_recap_invalidate(void);                    /* new activity / session transition */
void ui_recap_refresh(void);                       /* settings or blockers changed */
void ui_recap_dispose(void);                       /* teardown: cancels and waits for an in-flight request */
bool ui_recap_generated(const json_t *messages);   /* priorMessageWasGenerated */

/* pure helpers (tests) */
char *ui_title_compose(const char *label, run_state_t state, int frame, bool enabled, const char *style, bool static_host);
char *ui_program_status_report(run_state_t state, const char *kind, const char *msg);
char *ui_title_sanitize(const char *s); /* control characters dropped, trimmed; NULL when empty */
size_t ui_title_spinner_frames(const char *style, const char *const **frames);

#endif
