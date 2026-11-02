/* xdnotice.h - `xd://` mount notices (port of session-tools.ts #notifyXdevMountDelta and the
 * peek/takePendingXdevMountNotice projection).
 *
 * When the mounted device set changes mid-session (MCP enable/disable/reload/reconnect, tool refreshes), the
 * delta is coalesced into a pending set and announced in the UI (`xdev: xd://: mounted …; unmounted …`,
 * tools.xdevNotices, startup.quiet). The model learns of it with the next user prompt through a hidden
 * `xdev-mount-notice` custom message (prompts/system/xdev-mount-notice.md), placed right before the prompt.
 * Devices the outgoing base prompt already lists (its xd:// catalog or the MCP route guidance) are recorded as
 * announced without a notice line; re-mounts of announced devices and unmounts of never-announced ones are
 * suppressed. Announced state is seeded from persisted notices on resume. */
#ifndef OMC_XDNOTICE_H
#define OMC_XDNOTICE_H

#include "coding/app.h"

/* after the initial tool install: snapshot the mounted set (a UI session's MCP tools count as a pending
 * startup mount, like omp's deferred MCP discovery) */
void xdnotice_init(app_t *a);
/* after tools_finalize on a live session: coalesce the mount delta and show the notice ("xd://: mounted …"):
 * through app_notice level "xdev", or into *shown (owned, NULL when nothing to show) for a caller on the UI
 * thread that presents it in order */
void xdnotice_sync(app_t *a, char **shown);
/* app_rebuild_system_prompt: the devices the new base prompt lists (catalog_delivered: the prompt carries the
 * xd:// section) */
void xdnotice_base_rebuilt(app_t *a, bool catalog_delivered);
/* the hidden notice for the next user prompt (owned message, NULL when nothing to announce) */
json_t *xdnotice_take(app_t *a);
/* transcript replaced (/new, resume, branch): re-seed announced devices from the new history */
void xdnotice_reset_announced(app_t *a);
void xdnotice_free(app_t *a);

#endif
