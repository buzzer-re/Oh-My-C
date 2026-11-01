/* btw.h - `/btw` side questions, headless (ports of omp session/btw-turn.ts, session/btw-history.ts and
 * tui overlays/btw-history.ts): topic records, the session-local history sidecar and the ephemeral side turn.
 * Shared by the TUI controller (ui/btw.c) and RPC mode.
 *
 * A record is a JSON object in omp's shape, its keys in omp's order:
 *   {question, images?, answer, status, createdAt, updatedAt, error?, id, leafId, title?, followUps?: [turn...]}
 * where a turn is {question, images?, answer, status, createdAt, updatedAt, error?} and status is one of
 * running | complete | cancelled | error | interrupted. `title` (a display heading such as `/sitrep`) lives in
 * the `entry-<id>.title` sidecar, never in the record file nor on the RPC wire. */
#ifndef OMC_BTW_H
#define OMC_BTW_H

#include "coding/app.h"

/* the record's latest turn: its last follow-up, else the record itself (borrowed) */
json_t *btw_latest_turn(const json_t *record);
const char *btw_latest_status(const json_t *record);
/* [record, ...followUps] as borrowed pointers; returns the count (<= cap) */
size_t btw_turns(const json_t *record, const json_t **out, size_t cap);
size_t btw_turn_count(const json_t *record);
/* the topic heading: its title, else the opening question (borrowed) */
const char *btw_topic_label(const json_t *record);
/* the most recent nonblank answer, whitespace preserved (owned) or NULL */
char *btw_copy_text(const json_t *record);

/* beginBtwTurn: a new topic headed by `title` (NULL: none), or `previous` with a follow-up turn appended.
 * `*history` (owned array of borrowed->cloned earlier turns, NULL for a new topic) replays as context; the
 * conversation key is `btw:<id>:<epoch>`, a fresh epoch after the last turn that did not complete. */
typedef struct {
    json_t *record;
    json_t *history;
    char *conversation_key;
} btw_begin_t;
void btw_begin_turn(const char *question, const char *leaf_id, const json_t *previous, const json_t *images,
                    const char *title, btw_begin_t *out);
void btw_begin_free(btw_begin_t *b);
/* patchLatestBtwTurn in place: copies every key of `patch` into the latest turn */
void btw_patch_latest(json_t *record, const json_t *patch);
/* convenience patch: status (required), answer/error when non-NULL, updatedAt = now */
void btw_set_status(json_t *record, const char *status, const char *answer, const char *error);
/* the record without its TUI-only title (owned) */
json_t *btw_wire_record(const json_t *record);

/* runBtwTurn: the side question as an ephemeral turn on the session (btw-user.md prompt, earlier turns of the
 * topic replayed as user/assistant context). Deltas arrive on the calling thread. Returns 0 with *reply
 * (owned, repeated-line collapse, no byte cap) and *assistant (owned), or -1 with *err (owned). */
int btw_run_turn(app_t *a, const char *question, const json_t *images, const json_t *history,
                 const char *conversation_key, const json_t *streaming, void (*on_delta)(void *ud, const char *delta),
                 void *ud, volatile sig_atomic_t *cancel, char **reply, json_t **assistant, char **err);

/* ---------- history store: <artifacts dir>/btw-history[/sessions/<scope>]/entry-<id>.json ---------- */

typedef struct btw_store btw_store_t;
/* reads every record (a `running` one reads back as `interrupted`); artifacts_dir NULL = memory only.
 * NULL + *err on an unreadable directory or record. */
btw_store_t *btw_store_open(const char *artifacts_dir, const char *scope, char **err);
void btw_store_free(btw_store_t *s);
const char *btw_store_dir(const btw_store_t *s); /* NULL for a memory store */
/* snapshot, newest first (owned array of clones) */
json_t *btw_store_records(btw_store_t *s);
json_t *btw_store_find(btw_store_t *s, const char *id); /* owned clone or NULL */
/* checkpoint `record`: compare-and-swap against the revision this store read (a missing or rewritten file is
 * a conflict), title sidecar on the first write, lease (`<file>.lock`) held while the latest turn runs.
 * After a failure every later upsert fails until btw_store_retry succeeds. 0, or -1 + *err (owned). */
int btw_store_upsert(btw_store_t *s, const json_t *record, char **err);
int btw_store_retry(btw_store_t *s, const json_t *record, char **err);
bool btw_store_failed(const btw_store_t *s);

#endif
