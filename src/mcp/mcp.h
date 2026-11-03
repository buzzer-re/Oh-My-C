/* mcp.h - Model Context Protocol client (stdio, streamable HTTP, legacy SSE) and tool bridge. */
#ifndef OMC_MCP_H
#define OMC_MCP_H

#include "agent/agent.h"
#include "config/auth.h"
#include "core/core.h"
#include "core/json.h"

typedef struct mcp_server mcp_server_t;
typedef struct mcp_manager mcp_manager_t;

/* Discover servers from config files (project first, then user). Owned object name -> config; every
 * entry from a file carries "_source" (its path) and "_provider" (the discovery provider's display name:
 * "OMP", "MCP Config", "Claude Code", "Cursor", "VS Code"). Project-level files are skipped when
 * mcp.enableProjectConfig is off; servers disabled in the user mcp.json (`disabledServers`, or
 * `enabled: false`) are dropped. */
json_t *mcp_discover_config(const char *cwd);

/* connection lifecycle of a managed server (MCPManager.getConnectionStatus) */
typedef enum { MCP_DISCONNECTED = 0, MCP_CONNECTING, MCP_CONNECTED } mcp_state_t;

/* change notifications from servers (MCPManager setOnToolsChanged / setOnPromptsChanged /
 * setOnResourcesChanged). Called on a background thread. */
typedef struct {
    void (*tools_changed)(void *ud, const char *server);   /* a server connected late, or its tool list changed */
    void (*prompts_changed)(void *ud, const char *server); /* prompts/list_changed refreshed the catalog */
    void (*resource_updated)(void *ud, const char *server, const char *uri); /* a subscribed resource changed */
    void (*connected)(void *ud, const char *server); /* a server finished connecting after its caller stopped waiting */
    void *ud;
} mcp_callbacks_t;

mcp_manager_t *mcp_manager_new(void);
void mcp_manager_free(mcp_manager_t *m);
void mcp_set_callbacks(mcp_manager_t *m, const mcp_callbacks_t *cb);
/* Connect every configured server in parallel; waits up to timeout_ms. A server still connecting then
 * keeps connecting in the background (tools_changed fires when it is ready). */
void mcp_connect_all(mcp_manager_t *m, const json_t *config, int64_t timeout_ms);
/* startup discovery window: OMP_MCP_STARTUP_TIMEOUT_MS, else mcp.startupTimeoutMs (default 250 ms); 0 = wait for all */
int64_t mcp_startup_timeout_ms(void);
/* Add (or replace) one server and connect it, waiting up to timeout_ms. Returns its state. */
mcp_state_t mcp_connect_server(mcp_manager_t *m, const char *name, const json_t *cfg, int64_t timeout_ms);
/* MCPManager.reconnectServer: reconnect with the stored config; MCP_DISCONNECTED (+ *err) when it fails */
mcp_state_t mcp_reconnect_server(mcp_manager_t *m, const char *name, int64_t timeout_ms, char **err);
/* drop a server (closes its transport); true when it was known */
bool mcp_disconnect_server(mcp_manager_t *m, const char *name);
void mcp_disconnect_all(mcp_manager_t *m);

mcp_state_t mcp_server_state(mcp_manager_t *m, const char *name);
bool mcp_has_server(mcp_manager_t *m, const char *name);
json_t *mcp_server_names(mcp_manager_t *m);       /* every managed server, config order (owned) */
json_t *mcp_connected_names(mcp_manager_t *m);    /* connected servers, config order (owned) */
json_t *mcp_server_config(mcp_manager_t *m, const char *name); /* clone or NULL */
json_t *mcp_server_tools(mcp_manager_t *m, const char *name);  /* tools/list catalog clone or NULL */
json_t *mcp_server_prompts(mcp_manager_t *m, const char *name); /* prompts catalog clone ([] when none), NULL when not connected */
/* {resources:[...], templates:[...]} of a connected server (owned) or NULL */
json_t *mcp_server_resources(mcp_manager_t *m, const char *name);
json_t *mcp_server_capabilities(mcp_manager_t *m, const char *name); /* initialize capabilities clone or NULL */
/* mcp.notifications: resources/subscribe on every connected server that supports it (and unsubscribe when
 * turned off); the subscribed URIs of a server (owned array, NULL when none) */
void mcp_set_notifications(mcp_manager_t *m, bool enabled);
bool mcp_notifications_enabled(mcp_manager_t *m);
json_t *mcp_subscriptions(mcp_manager_t *m, const char *name);

/* MCPToolCache (omp agent.db cache): turn the tool cache on for this manager (off by default; the main session
 * manager uses it, ACP client servers do not). With it on, every successful tools/list is cached, and a server
 * still connecting when the startup window closes gets its cached tools registered as deferred tools (calls
 * wait for the connection) until the live tools replace them. */
void mcp_set_tool_cache(mcp_manager_t *m, bool on);

/* Register bridged tools (mcp__<server>_<tool>) of connected servers on the agent, plus the cached (deferred)
 * tools of servers that are not connected. Returns count. */
int mcp_register_tools(mcp_manager_t *m, agent_t *a);
/* Replace every mcp__ tool on the agent with the connected servers' current tools (marked discoverable).
 * Call while the agent is idle. Returns the registered count. */
int mcp_sync_tools(mcp_manager_t *m, agent_t *a);
/* Server instructions for the system prompt (owned, "" if none). */
char *mcp_instructions(mcp_manager_t *m);
/* Human-readable status lines (owned). */
char *mcp_status(mcp_manager_t *m);
size_t mcp_server_count(mcp_manager_t *m);
json_t *mcp_connected(mcp_manager_t *m); /* connected servers [{name, tools}] in config order (owned) */
/* failed servers [{name, error, transport}] in config order (owned); a server still connecting reports
 * "connection timed out" */
json_t *mcp_failed(mcp_manager_t *m);

/* One-off connection (connectToServer + listTools + disconnect) for /mcp test and the add wizard.
 * Returns 0 with owned *info ({name, version} serverInfo) and *tools (tools/list catalog), or -1 with
 * owned *err; *auth_required is set for an HTTP 401/403 and *www_auth (owned, may be NULL) holds the
 * WWW-Authenticate challenge. Any out pointer may be NULL. *cancel (may be NULL) aborts the wait. */
int mcp_probe(const char *name, const json_t *cfg, int64_t timeout_ms, volatile int *cancel, json_t **info, json_t **tools,
              char **err, bool *auth_required, char **www_auth);

/* the same one-off connection, returning {info, tools, resources, prompts} (owned) or NULL with *err */
json_t *mcp_probe_catalog(const char *name, const json_t *cfg, int64_t timeout_ms, char **err);

/* ---------- config files (mcp/config-writer.ts; mcpcfg.c) ---------- */

/* getMCPConfigPath: user -> <agent dir>/mcp.json, project -> <cwd>/.omc/mcp.json (owned) */
char *mcp_config_path(const char *scope, const char *cwd);
/* readMCPConfigFile: {mcpServers:{}} when missing; NULL with *err for unreadable JSON */
json_t *mcp_config_read(const char *path, char **err);
/* validateServerName: NULL when valid, else the static message */
const char *mcp_validate_server_name(const char *name);
/* validateServerConfig: "; "-joined messages (owned) or NULL */
char *mcp_validate_server_config(const char *name, const json_t *cfg);
/* addMCPServer / updateMCPServer / removeMCPServer / setServerDisabled (atomic writes with $schema first);
 * 0, or -1 with owned *err carrying upstream's message */
int mcp_config_add(const char *path, const char *name, const json_t *server, char **err);
int mcp_config_update(const char *path, const char *name, const json_t *server, char **err);
int mcp_config_remove(const char *path, const char *name, char **err);
int mcp_config_set_disabled(const char *path, const char *name, bool disabled, char **err);
json_t *mcp_config_disabled(const char *path); /* readDisabledServers (owned array) */

/* findMcpServerForAuth: the config entry for `name` (project, user, then cwd mcp.json / .mcp.json, then a
 * discovered runtime server). Returns 0 with owned *path, *scope ("user"/"project", static), *cfg and
 * *discovered, or -1 when unknown. */
int mcp_find_server(const char *name, const char *cwd, mcp_manager_t *m, bool enable_project, char **path,
                    const char **scope, json_t **cfg, bool *discovered);

/* ---------- OAuth for remote servers (oauth.c). Credentials are stored as "mcp:<url>" in auth.json. ---------- */
char *mcp_oauth_key(const char *url);
char *mcp_oauth_token(const char *url); /* refreshed access token or NULL */
int mcp_oauth_logout(const char *url);
bool mcp_oauth_has_credential(const char *url);
/* OAuth endpoints for a server (RFC 9728 resource metadata from the challenge or well-known paths, then
 * RFC 8414 / OIDC metadata): {authorizationUrl, tokenUrl, registrationUrl?, scopes?, resource?, issuerUrl?}
 * (owned) or NULL */
json_t *mcp_oauth_discover(const char *server_url, const char *www_authenticate);
json_t *mcp_oauth_descriptor(const char *server_url, const json_t *server_cfg, const char *www_authenticate, char **err);
int mcp_oauth_login(const char *server_url, const json_t *server_cfg, const char *www_authenticate, const auth_io_t *io, char **err);
/* the same flow through TUI hooks (auth_login_hooked) */
int mcp_oauth_login_hooked(const char *server_url, const json_t *server_cfg, const char *www_authenticate,
                           const auth_login_hooks_t *hooks, char **err);
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

/* ---------- tool cache (toolcache.c; <agentDir>/mcp-tool-cache.json) ---------- */
/* sha256 hex of the config's stable JSON (sorted keys; omc's top-level "_" annotations left out) (owned) */
char *mcp_tool_cache_hash(const json_t *cfg);
/* cached tools/list catalog for a server whose config hash still matches, unexpired (owned) or NULL */
json_t *mcp_tool_cache_get(const char *server, const json_t *cfg);
/* store a server's tools/list catalog (30-day TTL; expired entries pruned) */
void mcp_tool_cache_set(const char *server, const json_t *cfg, const json_t *tools);

/* low-level */
json_t *mcp_request(mcp_server_t *s, const char *method, json_t *params, int64_t timeout_ms, char **err);
char *mcp_tool_name(const char *server, const char *tool); /* owned */

#endif
