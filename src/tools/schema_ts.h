/* schema_ts.h - JSON schema rendered as a TypeScript type (port of ai/utils/schema/typescript.ts). */
#ifndef OMC_SCHEMA_TS_H
#define OMC_SCHEMA_TS_H

#include "core/json.h"

/* Simplified TypeScript rendering of a JSON schema. Owned. */
char *json_schema_to_ts(const json_t *schema);
/* harmony: OpenAI-Harmony flavor (`//` comments, `,` delimiters, no indentation; ai jsonSchemaToTypeScript style) */
char *json_schema_to_ts_style(const json_t *schema, bool harmony);

#endif
