/* session.h - session persistence (JSONL tree, omp-compatible format v3). */
#ifndef OMC_SESSION_H
#define OMC_SESSION_H

#include "core/core.h"
#include "core/json.h"

#define SESSION_VERSION 3
#define SESSION_TITLE_SLOT_BYTES 256

typedef struct {
    json_t *header;    /* {type:"session", version, id, timestamp, cwd, title?, ...} */
    json_t *entries;   /* array of entries (owned), insertion order */
    char *leaf_id;     /* current leaf (NULL => empty/new root) */
    char *file;        /* absolute path of the .jsonl (NULL for in-memory) */
    char *dir;
    bool persist;
    bool on_disk;      /* header written */
    char *title;
    char *title_source;
    hmap_t by_id;      /* id -> json_t* entry (borrowed) */
} session_t;

/* lifecycle */
session_t *session_create(const char *cwd, const char *session_dir); /* lazy file creation */
session_t *session_in_memory(const char *cwd);
session_t *session_open(const char *path, char **err);
session_t *session_continue_recent(const char *cwd, const char *session_dir); /* NULL if none */
void session_free(session_t *s);

const char *session_id(const session_t *s);
const char *session_cwd(const session_t *s);
const char *session_short_id(const char *id); /* last 8 chars (random part of UUIDv7) */
char *session_dir_for_cwd(const char *cwd); /* ~/.omc/agent/sessions/<encoded> (owned) */

/* appends (return the new entry id, borrowed) */
const char *session_append_message(session_t *s, const json_t *message);
const char *session_append_model_change(session_t *s, const char *provider_model, const char *role);
/* model_change with omp's resolvedModelIsFallback flag (retry fallback hops use role "fallback") */
const char *session_append_model_change_ex(session_t *s, const char *provider_model, const char *role, bool is_fallback);
const char *session_append_thinking_change(session_t *s, const char *level);
/* omp appendThinkingLevelChange(level, configured): NULL writes null; configured "auto" marks auto thinking */
const char *session_append_thinking_change_ex(session_t *s, const char *level, const char *configured);
const char *session_append_compaction(session_t *s, const char *summary, const char *first_kept_id,
                                      int64_t tokens_before, json_t *details);
const char *session_append_branch_summary(session_t *s, const char *from_id, const char *summary);
const char *session_append_custom(session_t *s, const char *custom_type, json_t *data);
const char *session_append_custom_message(session_t *s, const char *custom_type, const char *content, bool display);
const char *session_append_label(session_t *s, const char *target_id, const char *label);
const char *session_append_reset_boundary(session_t *s);
const char *session_append_entry(session_t *s, json_t *entry); /* generic: sets id/parentId/timestamp */
void session_set_title(session_t *s, const char *title, const char *source);
int session_ensure_on_disk(session_t *s);
int session_rewrite(session_t *s); /* rewriteEntries: rewrite the whole file after in-place entry edits */

/* tree navigation */
json_t *session_get(const session_t *s, const char *id); /* borrowed */
void session_branch(session_t *s, const char *entry_id);   /* moves leaf */
void session_reset_leaf(session_t *s);
json_t *session_path_to(const session_t *s, const char *leaf_id); /* owned array of borrowed->cloned entries root..leaf */
json_t *session_children(const session_t *s, const char *id);      /* owned array of entries */

/* context reconstruction: messages for the LLM along the current leaf. Owned array.
 * Also reports latest model/thinking selections (owned strings, may be NULL). */
typedef struct {
    json_t *messages;
    char *model;         /* "provider/id" */
    char *thinking;      /* level name */
    char *thinking_configured; /* configured selector of the latest thinking entry ("auto"), may be NULL */
    int64_t compaction_tokens_before;
} session_context_t;
void session_build_context(const session_t *s, session_context_t *out);
void session_context_free(session_context_t *c);

/* new branch file containing only root..leaf path */
session_t *session_fork(const session_t *s, const char *leaf_id, const char *session_dir);

/* listing */
typedef struct {
    char *path;
    char *id;
    char *cwd;
    char *title;
    char *first_message;
    int64_t mtime_ms;
    int64_t created_ms;
    int message_count;
} session_info_t;
typedef VEC(session_info_t) session_list_t;
int session_list(const char *dir, session_list_t *out);   /* one project dir, sorted newest first */
int session_list_all(session_list_t *out);                /* all projects */
void session_list_free(session_list_t *l);
char *session_resolve(const char *query, const char *cwd); /* id prefix / path -> file path (owned) */

/* export */
char *session_export_markdown(const session_t *s);
char *session_export_html(const session_t *s);

#endif
