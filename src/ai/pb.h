/*
 * pb.h - minimal schema-driven protobuf (proto3) wire codec.
 *
 * Messages are json_t objects keyed by the (camelCase) field names of a schema; this mirrors the
 * protobuf-es objects omp's codec builds, so ports read naturally:
 *
 *   scalar        string / bytes -> JSON string (binary-safe, json_t carries the length)
 *                 int32/int64/uint32/uint64/enum -> JSON integer, double/float -> number, bool -> bool
 *   message       JSON object (absent => not encoded)
 *   repeated      JSON array (scalars packed)
 *   map<string,V> JSON object
 *   oneof         each variant is an ordinary field marked with explicit presence; the decoded object
 *                 carries exactly the variant that was on the wire (check with json_get).
 *   unknown       fields the schema does not name are kept in "$unknown": [{no, wt, data}] (data = raw
 *                 bytes after the tag) and re-emitted on encode, so messages round-trip losslessly.
 *
 * Proto3 implicit presence: a non-optional scalar equal to its default (0, "", false) is not encoded;
 * fields marked '?' (optional / oneof members) are encoded whenever present in the object.
 *
 * Schema DSL (one message per line):  Name no:field:type[*][?] ...
 *   type = string|bytes|bool|int32|int64|uint32|uint64|enum|double|float|<Message>|map<V>
 *   '*' repeated, '?' explicit presence.
 */
#ifndef OMC_PB_H
#define OMC_PB_H

#include "core/core.h"
#include "core/json.h"

typedef struct pb_schema pb_schema_t;
typedef struct pb_msg pb_msg_t;

/* Parse a schema DSL text. Returns NULL and sets *err (owned) on malformed input or unresolved types. */
pb_schema_t *pb_schema_parse(const char *dsl, char **err);
void pb_schema_free(pb_schema_t *s);
const pb_msg_t *pb_schema_msg(const pb_schema_t *s, const char *name); /* NULL if unknown */
const char *pb_msg_name(const pb_msg_t *m);

/* Encode `val` (object) as message `m`, appending the wire bytes to `out`. Returns 0, or -1 with *err set
 * (owned) when a value has the wrong JSON type. */
int pb_encode(const pb_msg_t *m, const json_t *val, sb_t *out, char **err);
/* Convenience: encode by message name; aborts on schema misuse (programming error). */
char *pb_encode_named(const pb_schema_t *s, const char *name, const json_t *val, size_t *len);
/* Decode wire bytes as message `m`. Returns an owned object, or NULL with *err set (owned). */
json_t *pb_decode(const pb_msg_t *m, const void *data, size_t len, char **err);

/* google.protobuf.Value <-> JSON */
void pb_value_encode(const json_t *v, sb_t *out);
json_t *pb_value_decode(const void *data, size_t len); /* NULL on malformed input */

/* wire primitives (exposed for tests and hand-built frames) */
void pb_put_varint(sb_t *out, uint64_t v);
void pb_put_tag(sb_t *out, int no, int wire_type);
void pb_put_len_bytes(sb_t *out, int no, const void *data, size_t len);
/* reads a varint at *pos (< len); returns 0 ok, -1 truncated/overlong */
int pb_get_varint(const unsigned char *p, size_t len, size_t *pos, uint64_t *out);

#endif
