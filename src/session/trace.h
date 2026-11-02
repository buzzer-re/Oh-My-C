/* trace.h - offline session trace (port of omp packages/stats/src/trace.ts
 * buildSessionTraceFromSnapshots): span tree of turns, model calls, tool calls, background jobs and
 * subagents built from session entries, embedded in `/export` HTML files. */
#ifndef OMC_SESSION_TRACE_H
#define OMC_SESSION_TRACE_H

#include "core/json.h"

/* `root_header`: the session header object (may be NULL); `root_entries`: the non-header entries;
 * `sub_sessions`: object of slash-joined key -> {parent, header, entries, ...} (collectSubSessions
 * shape, may be NULL). Returns the SessionTrace JSON (owned) with omp's key order. */
json_t *session_trace_build(const json_t *root_header, const json_t *root_entries, const json_t *sub_sessions);

/* JS `Date.parse` for ISO-8601 strings; false when absent/unparseable */
bool trace_parse_iso_ms(const char *s, double *out);

#endif
