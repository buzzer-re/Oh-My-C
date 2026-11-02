/* mcp.h - Model Context Protocol client (stdio, streamable HTTP, legacy SSE) and tool bridge. */
#ifndef OMC_MCP_H
#define OMC_MCP_H

#include "agent/agent.h"
#include "config/auth.h"
#include "core/core.h"
#include "core/json.h"

typedef struct mcp_server mcp_server_t;
typedef struct mcp_manager mcp_manager_t;

/* Discover servers from config files (project first, then user). Owned object name -> config. */
json_t *mcp_discover_config(const char *cwd);

mcp_manager_t *mcp_manager_new(void);
void mcp_manager_free(mcp_manager_t *m);
/* Connect every configured server in parallel; waits up to timeout_ms. */
void mcp_connect_all(mcp_manager_t *m, const json_t *config, int64_t timeout_ms);
/* Register bridged tools (mcp__<server>_<tool>) on the agent. Returns count. */
int mcp_register_tools(mcp_manager_t *m, agent_t *a);
/* Server instructions for the system prompt (owned, "" if none). */
char *mcp_instructions(mcp_manager_t *m);
/* Human-readable status lines (owned). */
char *mcp_status(mcp_manager_t *m);
size_t mcp_server_count(mcp_manager_t *m);
json_t *mcp_connected(mcp_manager_t *m); /* connected servers [{name, tools}] in config order (owned) */
json_t *mcp_failed(mcp_manager_t *m);    /* failed servers [{name, error, transport}] in config order (owned) */

/* OAuth for remote servers (oauth.c). Credentials are stored as "mcp:<url>" in auth.json. */
char *mcp_oauth_key(const char *url);
char *mcp_oauth_token(const char *url); /* refreshed access token or NULL */
int mcp_oauth_logout(const char *url);
json_t *mcp_oauth_descriptor(const char *server_url, const json_t *server_cfg, const char *www_authenticate, char **err);
int mcp_oauth_login(const char *server_url, const json_t *server_cfg, const char *www_authenticate, const auth_io_t *io, char **err);
/* the URL a config entry connects to (url / serverUrl), or NULL for stdio servers */
const char *mcp_config_url(const json_t *cfg);
/* The most recently created live manager (NULL if none); used by mcp:// reads. */
mcp_manager_t *mcp_default_manager(void);
/* Read an MCP resource by URI from the connected server advertising it (exact URI first, then the
 * best-matching URI template). Returns 0 with owned *text (text parts joined by "\n---\n") and
 * *server, or -1 with an owned *err listing the available resources. */
int mcp_read_resource(mcp_manager_t *m, const char *uri, char **text, char **server, char **err);

/* buildMCPPromptCommands: [{name: "<server>:<prompt>", description, server, prompt}] for every connected
 * server's prompts/list catalog (owned) */
json_t *mcp_prompt_commands(mcp_manager_t *m);
/* executePrompt: prompts/get with `args` (owned object, may be NULL); the text / resource-text parts of
 * every message joined by blank lines (owned), or NULL (*err set on a request failure) */
char *mcp_run_prompt(mcp_manager_t *m, const char *server, const char *prompt, json_t *args, char **err);

/* low-level */
json_t *mcp_request(mcp_server_t *s, const char *method, json_t *params, int64_t timeout_ms, char **err);
char *mcp_tool_name(const char *server, const char *tool); /* owned */

#endif
