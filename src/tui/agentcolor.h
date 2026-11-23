/* agentcolor.h - agent identity colours (port of omp tui/overlays/agent-identity.ts): Main takes the theme accent;
 * subagents take up to six colours in arrival order, from the optional subagent1..6 theme tokens, then the theme's
 * own terminal.ansi hues, then accent-derived hues, then a fixed palette, each readable (3:1) on the terminal
 * background and the Task card tints and 0.10 apart in OKLab from accent, error and each other. */
#ifndef OMC_TUI_AGENTCOLOR_H
#define OMC_TUI_AGENTCOLOR_H

#include "tui/theme.h"

#include <stdint.h>

/* agentPalette: main + subagent colours of `th` (NULL: the current theme); returns the number of subagent colours */
size_t tui_agent_palette(const tui_theme_t *th, uint32_t *main_rgb, uint32_t sub[6]);
/* noteAgentArrival: `id` joined at `created_at`; slots go by creation time when a colour is first needed */
void tui_agent_note_arrival(const char *id, int64_t created_at);
/* agentColorHex: the accent for "Main", else the subagent colour of the arrival slot */
uint32_t tui_agent_color(const tui_theme_t *th, const char *id);
/* agentColored: `text` in the agent's colour, the foreground reset after it (owned) */
char *tui_agent_colored(const char *id, const char *text);

#endif
