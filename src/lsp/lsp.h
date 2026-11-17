/* lsp.h - Language Server Protocol client manager and the `lsp` tool. */
#ifndef OMC_LSP_H
#define OMC_LSP_H

#include "tools/tools.h"

typedef struct lsp_manager lsp_manager_t;

lsp_manager_t *lsp_manager_new(const char *cwd);
void lsp_manager_reset_ledger(lsp_manager_t *m);
void lsp_manager_free(lsp_manager_t *m);
agent_tool_t *tool_lsp_new(tool_env_t *env, lsp_manager_t *m);
/* After a write/edit: sync the file with running servers and return formatted diagnostics (owned) or NULL. */
char *lsp_after_write(lsp_manager_t *m, const char *abs_path, int64_t wait_ms);
/* status lines (owned) */
char *lsp_status(lsp_manager_t *m);

#endif
