/* mcperr.h - MCP tool failure diagnostics (port of omp mcp/errors.ts). An MCP request that fails fills an mcp_fail_t
 * (MCPTransportError's fields); the tool bridge renders it with mcp_format_tool_failure (formatMCPToolFailure). */
#ifndef OMC_MCPERR_H
#define OMC_MCPERR_H
#include "core/core.h"
#include "core/json.h"

typedef struct {
    bool set;
    const char *transport; /* "stdio" | "http" | "unknown" (static) */
    const char *stage;     /* "connect" | "send" | "receive" | "decode" | "protocol" */
    const char *failure;   /* "connect" | "timeout" | "eof" | "reset" | "malformed_response" | "json_rpc" | "http_status" |
                            * "closed" | "unknown" */
    bool retryable;
    char *message, *code, *data, *trace_id; /* owned; code/data/trace_id may be NULL */
} mcp_fail_t;

void mcp_fail_free(mcp_fail_t *f);
/* new MCPTransportError({...}): the message is sanitized and bounded; `code` copied (NULL: none) */
void mcp_fail_set(mcp_fail_t *f, const char *transport, const char *stage, const char *failure, bool retryable,
                  const char *message, const char *code);
/* createMCPJsonRpcError(transport, {code, message, data}) */
void mcp_fail_json_rpc(mcp_fail_t *f, const char *transport, const json_t *error);
/* normalizeMCPTransportError: classify a transport exception by its code and message */
void mcp_fail_normalize(mcp_fail_t *f, const char *transport, const char *stage, const char *message, const char *code);
/* isRetriableConnectionError for a classified failure: reconnect and replay once */
bool mcp_fail_reconnectable(const mcp_fail_t *f);
/* formatMCPToolFailure. Owned. */
char *mcp_format_tool_failure(const mcp_fail_t *f, const char *server, const char *tool);

/* helpers (exposed for tests) */
char *mcp_sanitize_diag(const char *value, size_t max_chars); /* sanitizeDiagnosticText */
char *mcp_serialize_data(const json_t *data);                 /* serializeData: NULL when absent */
char *mcp_find_trace_id(const json_t *v, int depth);          /* findTraceId */
#endif
