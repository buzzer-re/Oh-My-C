/* judgment.h - typed judgments over one state (port of packages/ai/src/judgment text bridge + the eval
 * judgment-bridge validation). A chat model answers keyword prompts rendered from the questions:
 *   choice -> {type:"choice", choice, probabilities:{label:p}, confidence}
 *   bool   -> {type:"bool", bool: P(yes)}
 *   score  -> {type:"score", score, probabilities:{"0":p,...}, confidence}
 * Text completions carry no distribution, so answers are one-hot (probability 1, confidence 1). */
#ifndef OMC_JUDGMENT_H
#define OMC_JUDGMENT_H

#include "core/json.h"

/* Validate a cell-supplied state: non-empty string, JSON object or JSON array. On failure returns false and sets
 * *err (owned, "judge() received invalid arguments: ..."). */
bool judgment_check_state(const json_t *state, char **err);
/* Validate cell questions `{id: {type: choice|bool|score, instructions, criteria}}`; returns an owned normalized copy
 * (criteria kept as given) or NULL with *err (owned). */
json_t *judgment_parse_questions(const json_t *questions, char **err);

/* State rendered as top-level XML fields (nested values as block YAML), as the user text sees it. Owned. */
char *judgment_render_state(const json_t *state);
/* Render the keyword prompt: *system carries the questions, *user the state and the answer cue (both owned). */
void judgment_render_prompt(const json_t *state, const json_t *questions, char **system, char **user);
/* Format-correction system prompt for a retry after an unparseable reply (owned). */
char *judgment_retry_system(const char *system, bool multi);

/* Parse a reply into cell-shaped answers keyed by question id (owned), or NULL with *err (owned) naming the first
 * question that did not resolve. */
json_t *judgment_parse_reply(const json_t *questions, const char *text, char **err);

/* exposed for tests */
int judgment_index_of_word(const char *text, const char *needle);
int judgment_parse_score(const char *text, int levels); /* -1 when absent */
int judgment_parse_yes_no(const char *text);            /* 1 yes, 0 no, -1 neither */

/* Bun YAML.stringify(value, null, 2) output (owned); shared with the /dump tool-call args */
char *bun_yaml_stringify(const json_t *v);

#endif
