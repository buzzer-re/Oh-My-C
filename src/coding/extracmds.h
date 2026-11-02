/* extracmds.h - session logic behind the WP-28 commands shared by the TUI, RPC and ACP:
 *   /force <tool> [prompt]   AgentSession.setForcedToolChoice + utils/tool-choice.ts buildNamedToolChoice
 *   /skillful [on|off|status] AgentSession.setSkillful / toggleSkillful (session-only `skillful` override)
 * plus the text-mode (`handle`) slash handlers of /changelog, /force and /skillful (slashcmd.c registry). */
#ifndef OMC_EXTRACMDS_H
#define OMC_EXTRACMDS_H

#include "coding/app.h"

/* setForcedToolChoice: 0, or -1 with *err (owned): `Tool "x" is not currently active.` /
 * `Current model does not support forcing a specific tool.` */
int extracmd_force_tool(app_t *a, const char *tool, char **err);
/* hook_prepare: serve the next tool choice of a pending /force sequence */
void extracmd_prepare_request(app_t *a, ai_stream_opts_t *o);
/* `Force: N active tools` / `Force: no active tools` (owned) */
char *extracmd_force_description(app_t *a);

/* cfgSkillful as the session sees it */
bool extracmd_skillful(void);
/* setSkillful (mode 1 / 0) or toggleSkillful (-1); returns the new value */
bool extracmd_set_skillful(app_t *a, int mode);

#endif
