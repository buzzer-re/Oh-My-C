/* memory.h - memory backends (port of omp memory-backend/, memories/): backend selection, the local
 * rollout-summary pipeline (phase 1 extraction + phase 2 consolidation into memory_summary.md /
 * MEMORY.md / skills/), `learn` lessons in learned.md, the system-prompt memory block, `/memory`, and
 * memory:// URLs. Backends hindsight / mnemopi / sharpshooter are recognized but not implemented. */
#ifndef OMC_MEMORY_H
#define OMC_MEMORY_H

#include "core/core.h"
#include "core/json.h"

struct app;
typedef struct memory_rt memory_rt_t;

/* effective `memory.backend` ("off" | "local" | "hindsight" | "mnemopi" | "sharpshooter"; legacy
 * `memories.enabled` maps to local/off when memory.backend is unset). Static string. */
const char *memory_backend(void);

/* <agentDir>/memories/--<encoded cwd>-- (owned) */
char *memory_root(const char *agent_dir, const char *cwd);
/* <agentDir>/memory-state.json: omc's stand-in for omp's agent.db memory tables (owned) */
char *memory_state_path(const char *agent_dir);

/* credential-shaped token redaction shared by memory writes (memory-backend/redact.ts); owned */
char *memory_redact_secrets(const char *s);
/* truncateByApproxTokens (4 UTF-16 units per token, 60/40 head/tail split); owned */
char *memory_truncate_tokens(const char *s, int64_t token_limit);

/* append a lesson to <root>/learned.md (newest-first, deduped, capped, redacted, neutralized).
 * Returns stored count (0 = empty after sanitization), -1 on I/O error; *msg owned. */
int memory_save_lesson(const char *agent_dir, const char *cwd, const char *content, const char *context, char **msg);

/* per-session runtime (instruction snapshot cache + background startup task) */
memory_rt_t *memory_rt_new(void);
void memory_rt_free(memory_rt_t *m); /* cancels and joins the startup task */

/* backend.start(): local backend kicks off the phase1/phase2 startup pipeline in the background */
void memory_start(struct app *a);
/* backend.buildDeveloperInstructions(): markdown for the system-prompt append (owned) or NULL */
char *memory_instructions(struct app *a);
/* adopt a summary refreshed by startup while no request went out yet; true if the prompt changed */
bool memory_take_prompt_refresh(struct app *a);

/* /memory <view|stats|diagnose|clear|reset|enqueue|rebuild|queue|sync|mm ...> (command-controller.ts).
 * level: "info" | "warning" | "error" | "panel"; for "panel" *title names the report and *text is markdown. */
typedef struct {
    const char *level;
    char *title;
    char *text;
} memory_cmd_result_t;
void memory_command(struct app *a, const char *args, memory_cmd_result_t *out);
void memory_cmd_result_free(memory_cmd_result_t *r);

/* memory://root[/path] -> absolute file path for the read tool (owned) or NULL with *err (owned) */
char *memory_url_resolve(const char *cwd, const char *url, char **err);

/* exposed for unit tests */
char *memory_neutralize(const char *s);
char *memory_build_rollout_items(const char *rollout_path, int64_t token_limit);
json_t *memory_parse_json_object(const char *text); /* parseJsonObject: owned object or NULL */
char *memory_rollout_filename(const char *thread_id, const char *slug);
/* render the read-path block from a summary/learned snapshot (owned or NULL) */
char *memory_render_instructions(const char *summary, const char *learned, int64_t token_limit);
/* phase 2: write consolidation output (validated object) under root; 0 or -1 with *err */
int memory_apply_consolidation(const char *root, const json_t *parsed, char **err);

#endif
