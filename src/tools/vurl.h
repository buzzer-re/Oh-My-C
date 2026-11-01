/* vurl.h - device-backed internal URLs served by the read/write tools: proc:// (jobs, agents, services),
 * agent:// (subagent outputs + peer messages) and history:// (subagent transcripts). */
#ifndef OMC_VURL_H
#define OMC_VURL_H

#include "tools.h"

bool vurl_is(const char *path);
/* read; always handles a vurl_is() path (errors are reported in out) */
int vurl_read(tool_env_t *env, const char *path, tool_result_t *out);
/* write; content may be NULL for proc://<id>/kill */
int vurl_write(tool_env_t *env, const char *path, const char *content, tool_result_t *out);

#endif
