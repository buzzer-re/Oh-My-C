/*
 * yaml.h - YAML -> json_t parser and json_t -> YAML emitter (config.yml, models.yml, frontmatter).
 *
 * Supported: block mappings/sequences (incl. sequences at the key's indent and compact `- k: v`),
 * flow collections, plain / single / double-quoted scalars (escapes, line folding), literal `|` and
 * folded `>` block scalars with chomping and indentation indicators, comments, `---` / `...`,
 * directives (skipped), anchors/aliases (expansion budget-limited), merge keys `<<`, tags `!!str`
 * (others ignored). Scalars resolve per the YAML 1.2 core schema (only true/false are booleans).
 * Non-string keys become their scalar text. Nesting depth is capped.
 */
#ifndef OMC_TEXT_YAML_H
#define OMC_TEXT_YAML_H

#include "core/json.h"

/* Parse the first document. Empty document -> JSON null. On error returns NULL and sets *err
 * (owned, "line L, column C: message"; err may be NULL). */
json_t *yaml_parse(const char *s, size_t len, char **err);
/* Parse every document of a stream into a JSON array. */
json_t *yaml_parse_all(const char *s, size_t len, char **err);
/* Block-style YAML (2-space indent, object key order kept, strings quoted only when needed,
 * multi-line strings as `|` blocks). Owned, ends with "\n". */
char *yaml_dump(const json_t *j);
/* Markdown frontmatter: text starting with `---` (after optional BOM) up to a `---`/`...` line.
 * Returns the parsed value (NULL if absent or on error, with *err set on error); *body_out (may be
 * NULL) points just past the closing fence line, or at text when there is no frontmatter. */
json_t *yaml_frontmatter(const char *text, size_t len, const char **body_out, char **err);

#endif
