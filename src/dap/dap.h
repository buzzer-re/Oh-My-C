/* dap.h - Debug Adapter Protocol client and the `debug` tool. */
#ifndef OMC_DAP_H
#define OMC_DAP_H

#include "tools/tools.h"

typedef struct dap_manager dap_manager_t;

dap_manager_t *dap_manager_new(const char *cwd);
void dap_manager_free(dap_manager_t *m);
agent_tool_t *tool_debug_new(tool_env_t *env, dap_manager_t *m);

#endif
