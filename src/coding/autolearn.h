/* autolearn.h - experimental auto-learn (port of omp autolearn/{controller,managed-skills,settings}.ts,
 * tools/learn.ts, tools/manage-skill.ts): the `learn` / `manage_skill` tools, isolated managed skills in
 * <agentDir>/managed-skills, the standing system-prompt guidance, and the post-stop private capture turn. */
#ifndef OMC_AUTOLEARN_H
#define OMC_AUTOLEARN_H

#include "agent/agent.h"
#include "core/core.h"

struct app;
typedef struct autolearn autolearn_t;

/* <agentDir>/managed-skills (owned) */
char *autolearn_managed_dir(void);
/* sanitizeSkillName: trimmed + lowercased name matching ^[a-z0-9][a-z0-9-]{0,63}$ (owned) or NULL with *err */
char *autolearn_sanitize_skill_name(const char *raw, char **err);
bool autolearn_valid_skill_name(const char *name);
/* create|update a managed SKILL.md; 0 (and *path owned) or -1 with *err (owned) */
int autolearn_write_skill(const char *action, const char *name, const char *description, const char *body, char **path,
                          char **err);
int autolearn_delete_skill(const char *name, char **err);

/* register `manage_skill` / `learn` per autolearn.enabled + memory.backend + session depth (tools/index.ts) */
void autolearn_register_tools(struct app *a, agent_t *ag);
/* buildAutoLearnInstructions from the tools present; owned or NULL */
char *autolearn_instructions(agent_t *ag);

/* controller: installed for top-level sessions; reads autolearn.* live at every stop */
autolearn_t *autolearn_new(struct app *a);
void autolearn_on_event(autolearn_t *al, const agent_event_t *ev);
void autolearn_free(autolearn_t *al); /* aborts and joins a running capture */
/* wait for an in-flight capture (tests / print mode); returns immediately when idle */
void autolearn_wait(autolearn_t *al, int64_t timeout_ms);

#endif
