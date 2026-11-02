/* plugins.h - installed plugin roots (Claude Code + omc registries) and their skills
 * (port of omp discovery/helpers listClaudePluginRoots, agent-plugins.ts, claude-plugins.ts). */
#ifndef OMC_PLUGINS_H
#define OMC_PLUGINS_H

#include "coding/discovery.h"
#include "core/core.h"

typedef struct {
    char *id;          /* "plugin@marketplace" */
    char *plugin;      /* plugin name */
    char *marketplace;
    char *path;        /* install root */
    bool project;      /* project scope (else user) */
    bool from_claude;  /* Claude Code registry (else omc registry) */
} plugin_root_t;

typedef VEC(plugin_root_t) plugin_roots_t;

/* Installed plugin roots for cwd: Claude Code registry (honoring enabledPlugins and project-bound
 * installs), ~/.omc/plugins registry (authoritative per plugin id), nearest project registry. */
void plugin_roots_list(const char *cwd, plugin_roots_t *out);
void plugin_roots_free(plugin_roots_t *r);

/* Claude Code config dir: $CLAUDE_CONFIG_DIR or ~/.claude (owned) */
char *claude_config_dir(void);

/* Whether a foreign user-level provider ("claude", "claude-plugins", "cursor", ...) is opted in via
 * `enabledProviders` (and not listed in `disabledProviders`). Native providers are always enabled. */
bool provider_user_source_enabled(const char *id);

/* Agent Plugins standard skills (root plugin.json with the agent-plugins.org schema) */
void plugin_skills_standard(const plugin_roots_t *roots, discovery_t *d);
/* Legacy Claude Code plugin skills (`skills/` + manifest-declared dirs) for non-standard roots */
void plugin_skills_legacy(const plugin_roots_t *roots, discovery_t *d);

/* claude-plugins slash-command sources: for each admitted root, every commands dir (or single .md file)
 * named by the manifest's `commands` / `slash-commands`, else <root>/commands; `plugin` prefixes names */
typedef void (*plugin_cmd_fn)(void *ud, const char *path, const char *plugin, bool project);
void plugin_commands_each(const plugin_roots_t *roots, plugin_cmd_fn fn, void *ud);

#endif
