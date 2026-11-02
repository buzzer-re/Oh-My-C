/* mcpint.h - internals shared by the /mcp controller (mcpcmd.c) and the add wizard (mcpwizard.c). */
#ifndef OMC_UI_MCPINT_H
#define OMC_UI_MCPINT_H

#include "mcp/mcp.h"
#include "ui/ui.h"

/* showCommandMessage: the styled text between two full-width rules in the transcript (UI thread) */
void mcpui_show_message(ui_t *u, const char *text);
/* showCommandReport with Text(text.trim(), 0, 0) (UI thread) */
void mcpui_show_report(ui_t *u, const char *title, const char *text);

/* run `fn(arg)` on a detached worker thread */
void mcpui_spawn(void *(*fn)(void *), void *arg);

/* MCP OAuth flow on the calling worker thread (MCPCommandController #handleOAuthFlow): presents the
 * "OAuth Authorization Required" block, progress rows and the completion row in the transcript, opens the
 * browser, accepts `/login <value>` as the pasted code. `abort` stops it; with `editor_esc` Esc on the
 * editor sets it. Returns 0, or -1 with owned *err (*cancelled for a user cancel: "OAuth flow cancelled"). */
int mcpui_oauth_flow(ui_t *u, const char *server_url, const json_t *cfg, const char *www_auth, volatile int *abort,
                     bool editor_esc, char **err, bool *cancelled);

/* #handleWizardComplete: save the server, reload, report (UI thread; the work runs on a worker) */
void mcpui_add_server(ui_t *u, const char *name, const json_t *cfg, const char *scope);
/* #handleWizardCancel message (UI thread) */
void mcpui_wizard_cancelled(ui_t *u);

/* mcpwizard.c: open the add wizard in the composer's place (initial name may be NULL) */
void ui_mcp_wizard_open(ui_t *u, const char *initial_name);
bool ui_mcp_wizard_active(void);

#endif
