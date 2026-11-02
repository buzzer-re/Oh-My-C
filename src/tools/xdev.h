/* xdev.h - `xd://` virtual tool devices (port of tools/xdev.ts + internal-urls/xd-protocol.ts).
 *
 * Discoverable tools are unmounted from the request's tool list and driven through read/write:
 *   read  xd://          -> mounted device listing
 *   read  xd://<tool>    -> docs + parameter schema (rendered as a TypeScript type)
 *   write xd://<tool>    -> execute; `content` is the JSON args object
 *   write xd://report_issue -> automated-QA grievance (recorded locally after consent; never pushed)
 * Mounted tools live in the agent as hidden tools with `mounted` set (see tools_finalize). */
#ifndef OMC_XDEV_H
#define OMC_XDEV_H

#include "agent/agent.h"
#include "core/json.h"

/* Simplified TypeScript rendering of a JSON schema (port of ai/utils/schema/typescript.ts). Owned. */
char *json_schema_to_ts(const json_t *schema);
/* harmony: OpenAI-Harmony flavor (`//` comments, `,` delimiters, no indentation; ai jsonSchemaToTypeScript style) */
char *json_schema_to_ts_style(const json_t *schema, bool harmony);

/* Number of mounted devices on an agent. */
size_t xdev_mounted_count(agent_t *a);
/* `{name, summary, dynamic}` entries for mounted devices in presentation order (owned array). */
json_t *xdev_entries(agent_t *a);
/* `read xd://` listing (owned). */
char *xdev_listing(agent_t *a);
/* Mounted-device section for the system prompt under tools.xdevDocs (owned; "" when nothing). */
char *xdev_prompt_docs(agent_t *a);

/* read handler: returns true when `path` is an xd:// URL (result filled). */
bool xdev_read(agent_t *a, const char *path, tool_result_t *out);
/* write handler: returns true when `path` is an xd:// device URL handled here (result filled). */
bool xdev_write(tool_ctx_t *ctx, const char *path, const char *content, tool_result_t *out);

/* Whether automated QA (`xd://report_issue`) is enabled (dev.autoqa + consent, PI_AUTO_QA). */
bool xdev_autoqa_enabled(void);

#endif
