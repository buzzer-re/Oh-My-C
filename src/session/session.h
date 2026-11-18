/* session.h - session persistence (JSONL tree, omp-compatible format v3). */
#ifndef OMC_SESSION_H
#define OMC_SESSION_H

#include <pthread.h>

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
    bool rewrite_required; /* an append failed: the next persist rewrites the whole file (omp #rewriteRequired) */
    bool disk_failed;      /* a failure since the last successful write was reported (omp #diskFailure) */
    char *persist_error;   /* failure text not yet taken by session_take_persist_error (owned) */
    char *last_persist_error; /* the latest failure's text (owned) */
    pthread_mutex_t *mu; /* recursive; see "Threads" below */
} session_t;

/* Threads: one thread writes a session at a time (its prompt worker, or the UI thread while no run is going) and may
 * read the fields above directly. Another thread (the UI while the worker runs: a transcript rebuild after a
 * compaction, the status line, /btw) reads only through the locked calls: session_current_path, session_leaf_dup,
 * session_title_dup, session_path_to, session_get, session_build_context*, ... Every mutation below holds the lock;
 * code that edits entries in place (compaction prune / shake) wraps the edit in session_lock/session_unlock. */
void session_lock(const session_t *s);
void session_unlock(const session_t *s);
json_t *session_current_path(const session_t *s); /* owned clone of root..leaf (empty array without a leaf) */
char *session_leaf_dup(const session_t *s);       /* owned copy of leaf_id, or NULL */
char *session_title_dup(const session_t *s);      /* owned copy of title, or NULL (NULL session: NULL) */

/* lifecycle */
session_t *session_create(const char *cwd, const char *session_dir); /* lazy file creation */
session_t *session_in_memory(const char *cwd);
session_t *session_create_file(const char *cwd, const char *file); /* persisted at `file` (lazy, like session_create) */
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
/* appendCompaction with every optional field (NULL / -1 / false omit it); clones details and preserve_data */
typedef struct {
    const char *summary, *short_summary, *first_kept_id, *method, *replay_through_id;
    int64_t tokens_before, tokens_after;
    const json_t *details, *preserve_data;
    bool from_extension;
} session_compaction_t;
const char *session_append_compaction_ex(session_t *s, const session_compaction_t *c);
const char *session_append_branch_summary(session_t *s, const char *from_id, const char *summary);
const char *session_append_custom(session_t *s, const char *custom_type, json_t *data);
const char *session_append_custom_message(session_t *s, const char *custom_type, const char *content, bool display);
const char *session_append_label(session_t *s, const char *target_id, const char *label);
const char *session_append_reset_boundary(session_t *s);
const char *session_append_entry(session_t *s, json_t *entry); /* generic: sets id/parentId/timestamp */
void session_set_title(session_t *s, const char *title, const char *source);
int session_ensure_on_disk(session_t *s);
int session_rewrite(session_t *s); /* rewriteEntries: rewrite the whole file after in-place entry edits */
/* the first write failure since the session last reached the disk (disk full, read-only dir, ...), once: owned text or
 * NULL. Entries stay in memory and the next append rewrites the whole file (omp onPersistenceError). */
char *session_take_persist_error(session_t *s);
/* dispose: retry a pending rewrite once; owned error text when the store is still failing, else NULL */
char *session_persist_retry(session_t *s);

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
    bool has_service_tier;     /* the path has a service_tier_change entry */
    json_t *service_tier;      /* its serviceTier ({family: tier} or null), owned, may be NULL */
    int64_t compaction_tokens_before;
} session_context_t;
void session_build_context(const session_t *s, session_context_t *out);
/* buildSessionContext({model}): the context as a reader (provider + api) reads it. A compaction whose OpenAI remote
 * payload the reader cannot replay (another provider, or a non-Responses API) summarizes nothing for it, so the
 * context stands on the newest compaction it can read and carries the raw history behind the unreadable one.
 * NULL provider: the newest compaction. */
void session_build_context_as(const session_t *s, const char *provider, const char *api, session_context_t *out);
/* getReadableCompactionEntry: id of the newest compaction on the current branch the reader can read (borrowed from
 * the session's entries), or NULL */
const char *session_readable_compaction(const session_t *s, const char *provider, const char *api);
/* canReplayRemoteCompaction: whether a reader can replay a compaction entry's provider-native payload */
bool session_compaction_readable(const json_t *compaction, const char *provider, const char *api);
/* the same over an explicit root..leaf entry array (projections over a pending entry) */
void session_build_context_for(const json_t *path, session_context_t *out);
/* convertToLlm for a persisted message (custom roles: bash/python execution, hook messages); owned or NULL */
json_t *session_message_to_llm(const json_t *msg);
/* the provider replay payload of a compaction entry: {type:"openaiResponsesHistory", provider, items}
 * (*openai_history set) or {type:"anthropicCompaction", ...}; owned or NULL */
json_t *session_compaction_payload(const json_t *compaction, bool *openai_history);
void session_context_free(session_context_t *c);

/* new branch file containing only root..leaf path */
session_t *session_fork(const session_t *s, const char *leaf_id, const char *session_dir);

/* multi-root workspace: header.additionalDirectories, the roots beyond cwd (session-workspace.ts) */
char *session_normalize_workspace_dir(const char *dir, const char *base); /* ~ expanded, resolved (owned) */
strvec_t session_additional_dirs(const session_t *s);                     /* owned copy, in order */
/* addWorkspaceDirectory: 1 added, 0 already present (*resolved set either way, owned), -1 with *err (the cwd) */
int session_add_workspace_dir(session_t *s, const char *dir, char **resolved, char **err);
/* removeWorkspaceDirectory: 1 removed, 0 not an extra root; *resolved owned */
int session_remove_workspace_dir(session_t *s, const char *dir, char **resolved);
/* setAdditionalDirectories: normalized, deduped, cwd dropped; rewrites only when the list changed */
void session_set_additional_dirs(session_t *s, const strvec_t *dirs);
/* moveTo: relocate the session to `new_cwd` (its file, artifacts and local:// root move to that cwd's session
 * bucket; header cwd + previousSessionFiles updated). 0, or -1 with *err (owned) and nothing moved */
int session_move(session_t *s, const char *new_cwd, char **err);

/* session pins (<agent>/session-pins.json; session-pins.ts) */
bool session_is_pinned(const char *id);
int session_pin_toggle(const char *id, char **err); /* 1 pinned, 0 unpinned, -1 with *err */
int session_pin_clear(const char *id, char **err);  /* unpinSession: 1 it was pinned, 0 it was not, -1 with *err */
void session_pins_sync(void);                       /* reload the cached set from disk, picking up other instances' pins */
bool session_pin_cached(const char *id);            /* the cached set: cheap enough for a frame */

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
/* the resume picker's first pass: titled sessions only, from each file's header, without a content scan */
int session_list_titled(const char *dir, session_list_t *out);
int session_list_all_titled(session_list_t *out);
bool session_dir_has_sessions(const char *dir); /* stops at the first valid session */
bool session_any_sessions(void);                /* any project */
void session_list_remove(session_list_t *l, size_t i);
char *session_resolve(const char *query, const char *cwd); /* id prefix / path -> file path (owned) */

/* export */
char *session_export_markdown(const session_t *s);
char *session_export_html(const session_t *s); /* omp's export document (session/export_html.c) */

#endif
