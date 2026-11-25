/* toolschema.h - tool parameter schemas as omp puts them on the wire (packages/ai/src/utils/schema/{draft,dereference,
 * wire,normalize}.ts and omptype's local-ref.ts):
 *   - the draft-07 -> 2020-12 upgrade (`definitions` -> `$defs`, tuple `items` -> `prefixItems`, `dependencies` split,
 *     OpenAPI `nullable`),
 *   - an MCP server's input schema normalized like omp's MCPTool (normalizeSchemaForMCP: upgrade, local `$ref`s
 *     inlined, `$schema` / `nullable` dropped, `const` and all-`const` unions as `enum`),
 *   - the wire pass every provider applies to a raw JSON-Schema tool (toolWireSchema: upgrade, nullable-scalar and
 *     const-union rewrites, ArkType key comments, `{}` -> `true`),
 *   - OpenAI strict mode (adaptSchemaForStrict / tryEnforceStrictSchema: unrepresentable open shapes fall back,
 *     non-structural keywords dropped, `$ref` siblings and single `allOf` inlined, type arrays split, closed objects
 *     with every key required and optionals nullable).
 * Every function returns an owned tree and leaves its input untouched. */
#ifndef OMC_AI_TOOLSCHEMA_H
#define OMC_AI_TOOLSCHEMA_H

#include "core/json.h"

json_t *ts_upgrade_2020_12(const json_t *schema);
/* dereferenceJsonSchema: local `$ref`s into the root's `$defs` / `definitions` inlined, those blocks dropped */
json_t *ts_dereference(const json_t *schema);
json_t *ts_mcp_normalize(const json_t *input_schema);
json_t *ts_wire_schema(const json_t *params);
/* tryEnforceStrictSchema: the strict form (*strict true), or the upgraded input when strict mode cannot represent it */
json_t *ts_try_enforce_strict(const json_t *schema, bool *strict);
/* adaptSchemaForStrict: `strict` false -> the upgraded schema; true -> tryEnforceStrictSchema. *effective = strict sent */
json_t *ts_adapt_for_strict(const json_t *schema, bool strict, bool *effective);

/* omptype local-ref.ts: decoded RFC 6901 tokens of a local `#...` ref (false: not local or not decodable), and back */
bool ts_ref_tokens(const char *ref, strvec_t *out);
char *ts_ref_format(const strvec_t *tokens);

#endif
