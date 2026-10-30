/* mcp.h - Model Context Protocol client (stdio, streamable HTTP, legacy SSE) and tool bridge. */
#ifndef OMC_MCP_H
#define OMC_MCP_H

#include "agent/agent.h"
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

/* low-level */
json_t *mcp_request(mcp_server_t *s, const char *method, json_t *params, int64_t timeout_ms, char **err);
char *mcp_tool_name(const char *server, const char *tool); /* owned */

#endif
