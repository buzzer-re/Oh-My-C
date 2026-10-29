/*
 * keys.h - terminal input decoding and keybinding matching.
 *
 * The decoder turns raw stdin bytes into tui_key_t events. It understands UTF-8 text,
 * C0 control keys, CSI/SS3 sequences (arrows, home/end, page up/down, insert/delete,
 * F1-F12, xterm modifier parameters), kitty CSI-u keys (incl. text field and
 * event types), xterm modifyOtherKeys (CSI 27;mod;code~), Alt+key via an ESC prefix,
 * bracketed paste (CSI 200~ ... CSI 201~), focus in/out (CSI I / CSI O) and terminal
 * replies (CSI ? ... u / c, OSC, DCS, APC) which are surfaced as TUI_KEY_RESPONSE.
 *
 * A lone ESC (or a truncated sequence) is held until `TUI_ESC_TIMEOUT_MS` passes without
 * more input; the caller polls `tui_keydec_timeout_ms()` and calls `tui_keydec_next()` again
 * once it expires.
 *
 * Key identifiers ("ctrl+c", "alt+enter", "shift+tab", "ctrl+shift+p", "pageup", "f5")
 * are case-insensitive; modifiers may appear in any order. The canonical form orders
 * modifiers ctrl, shift, alt, super. A typed uppercase ASCII letter is "shift+<letter>".
 */
#ifndef OMC_TUI_KEYS_H
#define OMC_TUI_KEYS_H

#include "core/core.h"
#include "core/json.h"

#define TUI_ESC_TIMEOUT_MS 40
#define TUI_MAX_PASTE_BYTES (64u << 20)

typedef enum {
    TUI_KEY_NONE = 0,
    TUI_KEY_CHAR,      /* printable or ctrl/alt+character; `codepoint` set */
    TUI_KEY_ENTER,
    TUI_KEY_TAB,
    TUI_KEY_BACKSPACE,
    TUI_KEY_ESCAPE,
    TUI_KEY_UP,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
    TUI_KEY_HOME,
    TUI_KEY_END,
    TUI_KEY_PAGEUP,
    TUI_KEY_PAGEDOWN,
    TUI_KEY_INSERT,
    TUI_KEY_DELETE,
    TUI_KEY_CLEAR,     /* keypad 5 / CSI E */
    TUI_KEY_F,         /* function key; codepoint = 1..24 */
    TUI_KEY_PASTE,     /* bracketed paste; text holds the payload */
    TUI_KEY_FOCUS_IN,
    TUI_KEY_FOCUS_OUT,
    TUI_KEY_RESPONSE,  /* terminal reply; text holds the raw sequence */
    TUI_KEY_MOUSE,     /* mouse report (raw in text) */
    TUI_KEY_UNKNOWN,   /* unrecognized sequence (raw in text) */
} tui_key_kind_t;

enum { TUI_MOD_SHIFT = 1, TUI_MOD_ALT = 2, TUI_MOD_CTRL = 4, TUI_MOD_SUPER = 8 };

typedef struct {
    tui_key_kind_t kind;
    uint32_t codepoint; /* TUI_KEY_CHAR: base character (lowercase for ctrl+letter); TUI_KEY_F: number */
    unsigned mods;      /* TUI_MOD_* bitmask */
    bool repeat;        /* kitty repeat event */
    char *text;         /* owned, NUL-terminated: insertable text (CHAR without ctrl/alt/super), paste
                           payload, or raw bytes for RESPONSE/MOUSE/UNKNOWN; NULL otherwise */
    size_t text_len;
} tui_key_t;

void tui_key_free(tui_key_t *k);
/* Canonical id ("ctrl+shift+a", "enter", "f5"); empty string for non-key events. */
void tui_key_id(const tui_key_t *k, char *buf, size_t cap);
/* Does key `k` match `spec` (one chord, e.g. "ctrl+c")? */
bool tui_key_matches(const tui_key_t *k, const char *spec);
/* Canonicalize a chord string; returns false if malformed. */
bool tui_key_canonical(const char *spec, char *buf, size_t cap);
/* Insertable text for this key (NULL for non-text keys). Borrowed. */
const char *tui_key_text(const tui_key_t *k);

/* ---------- decoder ---------- */
typedef struct {
    sb_t buf;          /* pending raw bytes */
    size_t pos;        /* consumed prefix of buf */
    bool in_paste;
    sb_t paste;
    int64_t pending_since; /* mono ms when an incomplete sequence started waiting; 0 = none */
    bool kitty_active;     /* informational: kitty keyboard protocol enabled */
} tui_keydec_t;

void tui_keydec_init(tui_keydec_t *d);
void tui_keydec_free(tui_keydec_t *d);
void tui_keydec_feed(tui_keydec_t *d, const char *data, size_t n);
/* Pop the next decoded event. `now` is mono_ms(); incomplete escape sequences are flushed
 * once they have waited TUI_ESC_TIMEOUT_MS. Returns false when nothing is ready. */
bool tui_keydec_next(tui_keydec_t *d, tui_key_t *out, int64_t now);
/* Milliseconds until a pending incomplete sequence should be flushed; -1 if none. */
int tui_keydec_timeout_ms(const tui_keydec_t *d, int64_t now);
/* Convenience for tests: decode a complete byte string into keys (flushing pending). */
size_t tui_keys_decode_all(const char *data, size_t n, tui_key_t *out, size_t max);

/* ---------- keybindings ---------- */
/*
 * Action id -> list of chords. Defaults follow upstream (tui.editor.*, tui.input.*,
 * tui.select.*). User overrides come from a JSON object {"action": "chord" | ["chord", ...]};
 * an empty array disables the action. Applications may define more actions.
 */
typedef struct tui_keybindings tui_keybindings_t;

tui_keybindings_t *tui_keybindings_new(void); /* pre-populated with TUI defaults */
void tui_keybindings_free(tui_keybindings_t *kb);
/* Define/replace default chords for an action (NULL-terminated list). */
void tui_keybindings_define(tui_keybindings_t *kb, const char *action, const char *const *chords);
/* Apply user overrides; returns number of actions changed. Unknown actions are kept too. */
int tui_keybindings_load_json(tui_keybindings_t *kb, const json_t *obj);
int tui_keybindings_load_file(tui_keybindings_t *kb, const char *path); /* -1 on error */
bool tui_keybindings_matches(const tui_keybindings_t *kb, const tui_key_t *k, const char *action);
const strvec_t *tui_keybindings_get(const tui_keybindings_t *kb, const char *action); /* canonical chords */
/* Process-wide bindings used by built-in components (lazily created). */
tui_keybindings_t *tui_keybindings_global(void);
void tui_keybindings_set_global(tui_keybindings_t *kb); /* takes ownership */
/* Shorthand: tui_keybindings_matches(tui_keybindings_global(), k, action). */
bool tui_kb(const tui_key_t *k, const char *action);

#endif
