/* keys.c - terminal input decoder + keybinding matcher. */
#include "tui/keys.h"
#include "core/fs.h"

#include <ctype.h>

void tui_key_free(tui_key_t *k) {
    if (!k) return;
    free(k->text);
    k->text = NULL;
    k->text_len = 0;
}

static void key_set_text(tui_key_t *k, const char *s, size_t n) {
    free(k->text);
    k->text = xstrndup(s, n);
    k->text_len = n;
}

static void key_set_cp_text(tui_key_t *k, uint32_t cp) {
    char b[4];
    int n = utf8_encode(cp, b);
    key_set_text(k, b, (size_t)n);
}

const char *tui_key_text(const tui_key_t *k) {
    if (k->kind == TUI_KEY_PASTE) return k->text;
    if (k->kind != TUI_KEY_CHAR) return NULL;
    if (k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT | TUI_MOD_SUPER)) return NULL;
    return k->text;
}

/* ---------- key ids ---------- */

static const char *SHIFTED_SYMBOLS = "!@#$%^&*()_+{}|:<>?~\"";

static void append_mods(sb_t *b, unsigned mods) {
    if (mods & TUI_MOD_CTRL) sb_append(b, "ctrl+");
    if (mods & TUI_MOD_SHIFT) sb_append(b, "shift+");
    if (mods & TUI_MOD_ALT) sb_append(b, "alt+");
    if (mods & TUI_MOD_SUPER) sb_append(b, "super+");
}

static const char *kind_name(tui_key_kind_t k) {
    switch (k) {
    case TUI_KEY_ENTER: return "enter";
    case TUI_KEY_TAB: return "tab";
    case TUI_KEY_BACKSPACE: return "backspace";
    case TUI_KEY_ESCAPE: return "escape";
    case TUI_KEY_UP: return "up";
    case TUI_KEY_DOWN: return "down";
    case TUI_KEY_LEFT: return "left";
    case TUI_KEY_RIGHT: return "right";
    case TUI_KEY_HOME: return "home";
    case TUI_KEY_END: return "end";
    case TUI_KEY_PAGEUP: return "pageup";
    case TUI_KEY_PAGEDOWN: return "pagedown";
    case TUI_KEY_INSERT: return "insert";
    case TUI_KEY_DELETE: return "delete";
    case TUI_KEY_CLEAR: return "clear";
    default: return NULL;
    }
}

void tui_key_id(const tui_key_t *k, char *buf, size_t cap) {
    if (!cap) return;
    buf[0] = 0;
    sb_t b = SB_INIT;
    unsigned mods = k->mods;
    char base[16] = {0};
    if (k->kind == TUI_KEY_CHAR) {
        uint32_t cp = k->codepoint;
        if (cp >= 'A' && cp <= 'Z') {
            cp = cp - 'A' + 'a';
            mods |= TUI_MOD_SHIFT;
        }
        if (cp == ' ') strcpy(base, "space");
        else {
            int n = utf8_encode(cp, base);
            base[n] = 0;
        }
    } else if (k->kind == TUI_KEY_F) {
        snprintf(base, sizeof base, "f%u", (unsigned)k->codepoint);
    } else {
        const char *nm = kind_name(k->kind);
        if (!nm) return;
        snprintf(base, sizeof base, "%s", nm);
    }
    append_mods(&b, mods);
    sb_append(&b, base);
    snprintf(buf, cap, "%s", sb_cstr(&b));
    sb_free(&b);
}

static bool parse_mod(const char *t, size_t n, unsigned *m) {
    char low[16];
    if (n >= sizeof low) return false;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)t[i]);
    low[n] = 0;
    if (!strcmp(low, "ctrl") || !strcmp(low, "control") || !strcmp(low, "c")) *m |= TUI_MOD_CTRL;
    else if (!strcmp(low, "shift") || !strcmp(low, "s")) *m |= TUI_MOD_SHIFT;
    else if (!strcmp(low, "alt") || !strcmp(low, "meta") || !strcmp(low, "option") || !strcmp(low, "opt") ||
             !strcmp(low, "m"))
        *m |= TUI_MOD_ALT;
    else if (!strcmp(low, "super") || !strcmp(low, "cmd") || !strcmp(low, "win") || !strcmp(low, "command"))
        *m |= TUI_MOD_SUPER;
    else return false;
    return true;
}

bool tui_key_canonical(const char *spec, char *buf, size_t cap) {
    if (!spec || !*spec || !cap) return false;
    size_t n = strlen(spec);
    unsigned mods = 0;
    size_t start = 0;
    /* modifiers: every '+'-terminated token before the last one */
    for (;;) {
        const char *plus = memchr(spec + start, '+', n - start);
        if (!plus) break;
        size_t tl = (size_t)(plus - (spec + start));
        if (tl == 0) break;             /* "++" -> base "+" */
        if (plus + 1 == spec + n) break; /* trailing '+': base is "+"? handled below */
        if (!parse_mod(spec + start, tl, &mods)) return false;
        start += tl + 1;
    }
    const char *base = spec + start;
    size_t bl = n - start;
    if (bl > 1 && base[bl - 1] == '+' && base[0] != '+') {
        /* "ctrl+" malformed */
        if (!parse_mod(base, bl - 1, &mods)) return false;
        base = "+";
        bl = 1;
    }
    if (bl == 0) return false;
    char low[32];
    if (bl >= sizeof low) return false;
    for (size_t i = 0; i < bl; i++) low[i] = (char)tolower((unsigned char)base[i]);
    low[bl] = 0;
    if (bl == 1 && base[0] >= 'A' && base[0] <= 'Z') mods |= TUI_MOD_SHIFT;
    const char *b = low;
    if (!strcmp(b, "esc")) b = "escape";
    else if (!strcmp(b, "return")) b = "enter";
    else if (!strcmp(b, "pgup") || !strcmp(b, "page_up")) b = "pageup";
    else if (!strcmp(b, "pgdn") || !strcmp(b, "pgdown") || !strcmp(b, "page_down")) b = "pagedown";
    else if (!strcmp(b, "del")) b = "delete";
    else if (!strcmp(b, "ins")) b = "insert";
    else if (!strcmp(b, " ")) b = "space";
    else if (!strcmp(b, "arrowup")) b = "up";
    else if (!strcmp(b, "arrowdown")) b = "down";
    else if (!strcmp(b, "arrowleft")) b = "left";
    else if (!strcmp(b, "arrowright")) b = "right";
    sb_t o = SB_INIT;
    append_mods(&o, mods);
    sb_append(&o, b);
    snprintf(buf, cap, "%s", sb_cstr(&o));
    bool ok = o.len < cap;
    sb_free(&o);
    return ok;
}

static bool canon_matches(const char *keyid, const char *canon) {
    if (!keyid[0]) return false;
    if (!strcmp(keyid, canon)) return true;
    /* binding on a shifted symbol also matches the same symbol reported with shift */
    const char *base = strrchr(canon, '+');
    base = (base && base[1]) ? base + 1 : canon;
    if (strlen(base) == 1 && strchr(SHIFTED_SYMBOLS, base[0]) && !strstr(canon, "shift+")) {
        char alt[64];
        unsigned mods = 0;
        if (strstr(canon, "ctrl+")) mods |= TUI_MOD_CTRL;
        if (strstr(canon, "alt+")) mods |= TUI_MOD_ALT;
        if (strstr(canon, "super+")) mods |= TUI_MOD_SUPER;
        sb_t o = SB_INIT;
        append_mods(&o, mods | TUI_MOD_SHIFT);
        sb_append(&o, base);
        snprintf(alt, sizeof alt, "%s", sb_cstr(&o));
        sb_free(&o);
        if (!strcmp(keyid, alt)) return true;
    }
    return false;
}

bool tui_key_matches(const tui_key_t *k, const char *spec) {
    char id[64], canon[64];
    if (!tui_key_canonical(spec, canon, sizeof canon)) return false;
    tui_key_id(k, id, sizeof id);
    return canon_matches(id, canon);
}

/* ---------- decoding ---------- */

static void mk(tui_key_t *k, tui_key_kind_t kind, uint32_t cp, unsigned mods) {
    k->kind = kind;
    k->codepoint = cp;
    k->mods = mods;
}

/* Map a kitty / modifyOtherKeys key code to a key. Returns false for ignorable codes. */
static bool map_code(tui_key_t *k, uint32_t code, uint32_t shifted, unsigned mods, const uint32_t *textcps,
                     size_t ntext) {
    mods &= (TUI_MOD_SHIFT | TUI_MOD_ALT | TUI_MOD_CTRL | TUI_MOD_SUPER);
    switch (code) {
    case 13: case 57414: mk(k, TUI_KEY_ENTER, 0, mods); return true;
    case 9: mk(k, TUI_KEY_TAB, 0, mods); return true;
    case 27: mk(k, TUI_KEY_ESCAPE, 0, mods); return true;
    case 127: case 8: mk(k, TUI_KEY_BACKSPACE, 0, mods); return true;
    case 57417: mk(k, TUI_KEY_LEFT, 0, mods); return true;
    case 57418: mk(k, TUI_KEY_RIGHT, 0, mods); return true;
    case 57419: mk(k, TUI_KEY_UP, 0, mods); return true;
    case 57420: mk(k, TUI_KEY_DOWN, 0, mods); return true;
    case 57421: mk(k, TUI_KEY_PAGEUP, 0, mods); return true;
    case 57422: mk(k, TUI_KEY_PAGEDOWN, 0, mods); return true;
    case 57423: mk(k, TUI_KEY_HOME, 0, mods); return true;
    case 57424: mk(k, TUI_KEY_END, 0, mods); return true;
    case 57425: mk(k, TUI_KEY_INSERT, 0, mods); return true;
    case 57426: mk(k, TUI_KEY_DELETE, 0, mods); return true;
    default: break;
    }
    if (code >= 57399 && code <= 57415) {
        static const char kp[] = "0123456789./*-+\0=";
        code = (uint32_t)(unsigned char)kp[code - 57399];
        if (!code) return false;
        shifted = 0;
    } else if (code >= 57344 && code <= 63743) {
        return false; /* other PUA functional keys (modifier-only keys, media keys) */
    }
    if (code < 32 || code == 127 || code > 0x10FFFF) return false;
    uint32_t cp = code;
    if ((mods & TUI_MOD_SHIFT) && shifted && !(shifted >= 'A' && shifted <= 'Z') && shifted != code) cp = shifted;
    mk(k, TUI_KEY_CHAR, cp, mods);
    if (!(mods & (TUI_MOD_CTRL | TUI_MOD_ALT | TUI_MOD_SUPER))) {
        sb_t t = SB_INIT;
        if (ntext) {
            for (size_t i = 0; i < ntext; i++)
                if (textcps[i] >= 32 && textcps[i] != 127 && textcps[i] <= 0x10FFFF) sb_append_utf8(&t, textcps[i]);
        }
        if (!t.len) {
            uint32_t tc = code;
            if (mods & TUI_MOD_SHIFT) {
                if (shifted) tc = shifted;
                else if (code >= 'a' && code <= 'z') tc = code - 32;
            }
            sb_append_utf8(&t, tc);
        }
        k->text = sb_detach(&t);
        k->text_len = strlen(k->text);
        /* report shifted letters as the uppercase character (id becomes shift+x) */
        if ((mods & TUI_MOD_SHIFT) && code >= 'a' && code <= 'z') {
            k->codepoint = code;
        }
    }
    return true;
}

#define CSI_MAX_FIELDS 6
#define CSI_MAX_SUB 4

typedef struct {
    int v[CSI_MAX_FIELDS][CSI_MAX_SUB];
    int nf;
    int nsub[CSI_MAX_FIELDS];
} csi_params_t;

static void parse_params(const char *p, size_t n, csi_params_t *cp) {
    memset(cp, 0, sizeof *cp);
    for (int f = 0; f < CSI_MAX_FIELDS; f++)
        for (int s = 0; s < CSI_MAX_SUB; s++) cp->v[f][s] = -1;
    int f = 0, s = 0;
    bool any = false;
    for (size_t i = 0; i < n && f < CSI_MAX_FIELDS; i++) {
        char c = p[i];
        if (c >= '0' && c <= '9') {
            if (s < CSI_MAX_SUB) {
                int *d = &cp->v[f][s];
                if (*d < 0) *d = 0;
                if (*d < 10000000) *d = *d * 10 + (c - '0');
            }
            any = true;
        } else if (c == ':') {
            s++;
            any = true;
        } else if (c == ';') {
            cp->nsub[f] = s + 1;
            f++;
            s = 0;
            any = true;
        }
    }
    if (f < CSI_MAX_FIELDS) {
        cp->nsub[f] = s + 1;
        cp->nf = any ? f + 1 : 0;
    } else {
        cp->nf = CSI_MAX_FIELDS;
    }
}

static unsigned mods_from(int v) { return v > 1 ? (unsigned)(v - 1) & 0x0f : 0; }

static void handle_csi(const char *seq, size_t len, tui_key_t *k) {
    const char *p = seq + 2;
    size_t pn = len - 3;
    char fin = seq[len - 1];
    if (pn > 0 && (p[0] == '<' || p[0] == '?' || p[0] == '>' || p[0] == '=')) {
        mk(k, p[0] == '<' && (fin == 'M' || fin == 'm') ? TUI_KEY_MOUSE : TUI_KEY_RESPONSE, 0, 0);
        key_set_text(k, seq, len);
        return;
    }
    /* intermediate bytes (e.g. "$y" replies) -> response */
    for (size_t i = 0; i < pn; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c >= 0x20 && c <= 0x2f) {
            mk(k, TUI_KEY_RESPONSE, 0, 0);
            key_set_text(k, seq, len);
            return;
        }
    }
    csi_params_t P;
    parse_params(p, pn, &P);
    int a0 = P.nf > 0 ? P.v[0][0] : -1;
    int m1 = P.nf > 1 ? P.v[1][0] : -1;
    int ev = P.nf > 1 ? P.v[1][1] : -1;
    unsigned mods = mods_from(m1);
    if (ev == 3) { /* key release */
        mk(k, TUI_KEY_NONE, 0, 0);
        return;
    }
    k->repeat = ev == 2;
    switch (fin) {
    case 'u': {
        uint32_t code = a0 < 0 ? 0 : (uint32_t)a0;
        uint32_t shifted = P.v[0][1] > 0 ? (uint32_t)P.v[0][1] : 0;
        uint32_t text[CSI_MAX_SUB];
        size_t nt = 0;
        if (P.nf > 2)
            for (int s = 0; s < CSI_MAX_SUB && s < P.nsub[2]; s++)
                if (P.v[2][s] > 0) text[nt++] = (uint32_t)P.v[2][s];
        if (!map_code(k, code, shifted, mods, text, nt)) mk(k, TUI_KEY_NONE, 0, 0);
        return;
    }
    case '~': {
        switch (a0) {
        case 1: case 7: mk(k, TUI_KEY_HOME, 0, mods); return;
        case 2: mk(k, TUI_KEY_INSERT, 0, mods); return;
        case 3: mk(k, TUI_KEY_DELETE, 0, mods); return;
        case 4: case 8: mk(k, TUI_KEY_END, 0, mods); return;
        case 5: mk(k, TUI_KEY_PAGEUP, 0, mods); return;
        case 6: mk(k, TUI_KEY_PAGEDOWN, 0, mods); return;
        case 11: case 12: case 13: case 14: case 15: mk(k, TUI_KEY_F, (uint32_t)(a0 - 10), mods); return;
        case 17: case 18: case 19: case 20: case 21: mk(k, TUI_KEY_F, (uint32_t)(a0 - 11), mods); return;
        case 23: case 24: case 25: case 26: mk(k, TUI_KEY_F, (uint32_t)(a0 - 12), mods); return;
        case 28: case 29: mk(k, TUI_KEY_F, (uint32_t)(a0 - 13), mods); return;
        case 31: case 32: case 33: case 34: mk(k, TUI_KEY_F, (uint32_t)(a0 - 14), mods); return;
        case 27: { /* modifyOtherKeys: CSI 27 ; mod ; code ~ */
            int code = P.nf > 2 ? P.v[2][0] : -1;
            if (code < 0 || !map_code(k, (uint32_t)code, 0, mods, NULL, 0)) mk(k, TUI_KEY_NONE, 0, 0);
            return;
        }
        default: break;
        }
        break;
    }
    case 'A': mk(k, TUI_KEY_UP, 0, mods); return;
    case 'B': mk(k, TUI_KEY_DOWN, 0, mods); return;
    case 'C': mk(k, TUI_KEY_RIGHT, 0, mods); return;
    case 'D': mk(k, TUI_KEY_LEFT, 0, mods); return;
    case 'H': mk(k, TUI_KEY_HOME, 0, mods); return;
    case 'F': mk(k, TUI_KEY_END, 0, mods); return;
    case 'E': mk(k, TUI_KEY_CLEAR, 0, mods); return;
    case 'P': mk(k, TUI_KEY_F, 1, mods); return;
    case 'Q': mk(k, TUI_KEY_F, 2, mods); return;
    case 'R': mk(k, TUI_KEY_F, 3, mods); return;
    case 'S': mk(k, TUI_KEY_F, 4, mods); return;
    case 'Z': mk(k, TUI_KEY_TAB, 0, mods | TUI_MOD_SHIFT); return;
    case 'I':
        if (pn == 0) { mk(k, TUI_KEY_FOCUS_IN, 0, 0); return; }
        break;
    case 'O':
        if (pn == 0) { mk(k, TUI_KEY_FOCUS_OUT, 0, 0); return; }
        break;
    default: break;
    }
    mk(k, TUI_KEY_UNKNOWN, 0, 0);
    key_set_text(k, seq, len);
}

static int utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Decode one event at s. Returns 1 with *used set, or 0 if more bytes are needed. */
static int decode_one(const char *s, size_t n, tui_key_t *k, bool force, size_t *used, int depth) {
    unsigned char c = (unsigned char)s[0];
    memset(k, 0, sizeof *k);
    if (c == 0x1b) {
        if (n == 1) {
            if (!force) return 0;
            mk(k, TUI_KEY_ESCAPE, 0, 0);
            *used = 1;
            return 1;
        }
        unsigned char c1 = (unsigned char)s[1];
        if (c1 == '[') {
            size_t i = 2;
            while (i < n && i < 256 && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7e)) i++;
            if (i >= 256) { /* absurd sequence: swallow what we scanned */
                mk(k, TUI_KEY_UNKNOWN, 0, 0);
                *used = i;
                return 1;
            }
            if (i >= n) {
                if (!force) return 0;
                /* stale partial sequence: report ESC, rest decodes as text */
                mk(k, TUI_KEY_ESCAPE, 0, 0);
                *used = 1;
                return 1;
            }
            handle_csi(s, i + 1, k);
            *used = i + 1;
            return 1;
        }
        if (c1 == 'O') {
            if (n < 3) {
                if (!force) return 0;
                mk(k, TUI_KEY_CHAR, 'O', TUI_MOD_ALT);
                *used = 2;
                return 1;
            }
            unsigned char c2 = (unsigned char)s[2];
            *used = 3;
            switch (c2) {
            case 'A': mk(k, TUI_KEY_UP, 0, 0); return 1;
            case 'B': mk(k, TUI_KEY_DOWN, 0, 0); return 1;
            case 'C': mk(k, TUI_KEY_RIGHT, 0, 0); return 1;
            case 'D': mk(k, TUI_KEY_LEFT, 0, 0); return 1;
            case 'H': mk(k, TUI_KEY_HOME, 0, 0); return 1;
            case 'F': mk(k, TUI_KEY_END, 0, 0); return 1;
            case 'E': mk(k, TUI_KEY_CLEAR, 0, 0); return 1;
            case 'M': mk(k, TUI_KEY_ENTER, 0, 0); return 1;
            case 'P': case 'Q': case 'R': case 'S': mk(k, TUI_KEY_F, (uint32_t)(c2 - 'P' + 1), 0); return 1;
            default:
                if (c2 >= 'j' && c2 <= 'y') { /* keypad characters in application mode */
                    static const char kp[] = "*+,-./0123456789";
                    mk(k, TUI_KEY_CHAR, (uint32_t)(unsigned char)kp[c2 - 'j'], 0);
                    key_set_cp_text(k, k->codepoint);
                    return 1;
                }
                mk(k, TUI_KEY_UNKNOWN, 0, 0);
                key_set_text(k, s, 3);
                return 1;
            }
        }
        if (c1 == ']' || c1 == 'P' || c1 == '_' || c1 == '^' || c1 == 'X') {
            /* string sequence terminated by BEL or ST */
            size_t l = 0;
            for (size_t i = 2; i < n; i++) {
                if (s[i] == '\a') { l = i + 1; break; }
                if (s[i] == '\x1b' && i + 1 < n && s[i + 1] == '\\') { l = i + 2; break; }
                if (s[i] == '\x1b' && i + 1 < n) { l = i; break; } /* aborted by a new escape */
            }
            if (!l) {
                if (!force && n < 65536) return 0;
                l = n;
            }
            mk(k, TUI_KEY_RESPONSE, 0, 0);
            key_set_text(k, s, l);
            *used = l;
            return 1;
        }
        if (depth == 0) {
            /* Alt + key: decode the following key and add ALT */
            tui_key_t inner;
            size_t iu = 0;
            if (c1 == 0x1b && n >= 3 && (s[2] == '[' || s[2] == 'O')) {
                int r = decode_one(s + 1, n - 1, &inner, force, &iu, 1);
                if (!r) return 0;
            } else if (c1 == 0x1b) {
                mk(k, TUI_KEY_ESCAPE, 0, TUI_MOD_ALT);
                *used = 2;
                return 1;
            } else {
                int r = decode_one(s + 1, n - 1, &inner, force, &iu, 1);
                if (!r) return 0;
            }
            if (inner.kind == TUI_KEY_NONE || inner.kind == TUI_KEY_RESPONSE || inner.kind == TUI_KEY_UNKNOWN ||
                inner.kind == TUI_KEY_MOUSE) {
                tui_key_free(&inner);
                mk(k, TUI_KEY_ESCAPE, 0, 0);
                *used = 1;
                return 1;
            }
            *k = inner;
            k->mods |= TUI_MOD_ALT;
            if (k->kind == TUI_KEY_CHAR) {
                free(k->text);
                k->text = NULL;
                k->text_len = 0;
            }
            *used = 1 + iu;
            return 1;
        }
        mk(k, TUI_KEY_ESCAPE, 0, 0);
        *used = 1;
        return 1;
    }
    *used = 1;
    switch (c) {
    case 0x0d: mk(k, TUI_KEY_ENTER, 0, 0); return 1;
    case 0x0a: mk(k, TUI_KEY_CHAR, 'j', TUI_MOD_CTRL); return 1;
    case 0x09: mk(k, TUI_KEY_TAB, 0, 0); return 1;
    case 0x7f: case 0x08: mk(k, TUI_KEY_BACKSPACE, 0, 0); return 1;
    case 0x00: mk(k, TUI_KEY_CHAR, ' ', TUI_MOD_CTRL); return 1;
    case 0x1c: mk(k, TUI_KEY_CHAR, '\\', TUI_MOD_CTRL); return 1;
    case 0x1d: mk(k, TUI_KEY_CHAR, ']', TUI_MOD_CTRL); return 1;
    case 0x1e: mk(k, TUI_KEY_CHAR, '^', TUI_MOD_CTRL); return 1;
    case 0x1f: mk(k, TUI_KEY_CHAR, '_', TUI_MOD_CTRL); return 1;
    default: break;
    }
    if (c < 0x20) {
        mk(k, TUI_KEY_CHAR, (uint32_t)('a' + c - 1), TUI_MOD_CTRL);
        return 1;
    }
    int need = utf8_seq_len(c);
    if ((size_t)need > n) {
        if (!force) return 0;
        need = (int)n;
    }
    uint32_t cp;
    int used_n = utf8_decode(s, (size_t)need, &cp);
    *used = (size_t)used_n;
    mk(k, TUI_KEY_CHAR, cp, 0);
    if (cp >= 0x80 && cp < 0xa0) {
        mk(k, TUI_KEY_UNKNOWN, 0, 0);
        return 1;
    }
    key_set_text(k, s, (size_t)used_n);
    if (cp == 0xFFFD && used_n == 1) key_set_cp_text(k, 0xFFFD);
    return 1;
}

void tui_keydec_init(tui_keydec_t *d) { memset(d, 0, sizeof *d); }

void tui_keydec_free(tui_keydec_t *d) {
    sb_free(&d->buf);
    sb_free(&d->paste);
    memset(d, 0, sizeof *d);
}

static void compact(tui_keydec_t *d) {
    if (d->pos == 0) return;
    if (d->pos >= d->buf.len) {
        sb_clear(&d->buf);
        d->pos = 0;
    } else if (d->pos > 4096 || d->pos * 2 > d->buf.len) {
        memmove(d->buf.s, d->buf.s + d->pos, d->buf.len - d->pos);
        d->buf.len -= d->pos;
        d->buf.s[d->buf.len] = 0;
        d->pos = 0;
    }
}

void tui_keydec_feed(tui_keydec_t *d, const char *data, size_t n) {
    compact(d);
    sb_appendn(&d->buf, data, n);
}

static void paste_add(tui_keydec_t *d, const char *s, size_t n) {
    if (d->paste.len >= TUI_MAX_PASTE_BYTES) return;
    if (d->paste.len + n > TUI_MAX_PASTE_BYTES) n = TUI_MAX_PASTE_BYTES - d->paste.len;
    sb_appendn(&d->paste, s, n);
}

static const char *find_mem(const char *h, size_t hn, const char *nd, size_t nn) {
    if (nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == nd[0] && !memcmp(h + i, nd, nn)) return h + i;
    return NULL;
}

bool tui_keydec_next(tui_keydec_t *d, tui_key_t *out, int64_t now) {
    memset(out, 0, sizeof *out);
    for (;;) {
        size_t avail = d->buf.len - d->pos;
        const char *s = d->buf.s ? d->buf.s + d->pos : "";
        if (d->in_paste) {
            static const char END[] = "\x1b[201~";
            const char *e = find_mem(s, avail, END, 6);
            if (e) {
                paste_add(d, s, (size_t)(e - s));
                d->pos += (size_t)(e - s) + 6;
                d->in_paste = false;
                out->kind = TUI_KEY_PASTE;
                out->text_len = d->paste.len;
                out->text = sb_detach(&d->paste);
                compact(d);
                return true;
            }
            if (avail > 5) {
                paste_add(d, s, avail - 5);
                d->pos += avail - 5;
            }
            compact(d);
            return false;
        }
        if (avail == 0) {
            d->pending_since = 0;
            compact(d);
            return false;
        }
        if (avail >= 6 && !memcmp(s, "\x1b[200~", 6)) {
            d->in_paste = true;
            sb_clear(&d->paste);
            d->pos += 6;
            d->pending_since = 0;
            continue;
        }
        bool force = d->pending_since && now - d->pending_since >= TUI_ESC_TIMEOUT_MS;
        size_t used = 0;
        int r = decode_one(s, avail, out, force, &used, 0);
        if (!r) {
            if (!d->pending_since) d->pending_since = now > 0 ? now : 1;
            return false;
        }
        d->pending_since = 0;
        d->pos += used;
        if (out->kind == TUI_KEY_NONE) {
            tui_key_free(out);
            memset(out, 0, sizeof *out);
            continue;
        }
        compact(d);
        return true;
    }
}

int tui_keydec_timeout_ms(const tui_keydec_t *d, int64_t now) {
    if (!d->pending_since) return -1;
    int64_t left = d->pending_since + TUI_ESC_TIMEOUT_MS - now;
    return left < 0 ? 0 : (int)left;
}

size_t tui_keys_decode_all(const char *data, size_t n, tui_key_t *out, size_t max) {
    tui_keydec_t d;
    tui_keydec_init(&d);
    tui_keydec_feed(&d, data, n);
    size_t cnt = 0;
    int64_t now = 1000;
    while (cnt < max) {
        if (tui_keydec_next(&d, &out[cnt], now)) {
            cnt++;
            continue;
        }
        if (d.pending_since) {
            now += TUI_ESC_TIMEOUT_MS;
            continue;
        }
        break;
    }
    tui_keydec_free(&d);
    return cnt;
}

/* ---------- keybindings ---------- */

/* Default chords in upstream registry order: TUI_KEYBINDINGS (keybindings.ts), then the app actions
 * of KEYBINDINGS (app-keybindings.ts). The order is also the order of a migrated keybindings.yml. */
static const struct {
    const char *action;
    const char *chords[6];
} DEFAULTS[] = {
    {"tui.editor.cursorUp", {"up"}},
    {"tui.editor.cursorDown", {"down"}},
    {"tui.editor.cursorLeft", {"left", "ctrl+b"}},
    {"tui.editor.cursorRight", {"right", "ctrl+f"}},
    {"tui.editor.cursorWordLeft", {"alt+left", "ctrl+left", "alt+b"}},
    {"tui.editor.cursorWordRight", {"alt+right", "ctrl+right", "alt+f"}},
    {"tui.editor.cursorLineStart", {"home", "ctrl+a"}},
    {"tui.editor.cursorLineEnd", {"end", "ctrl+e"}},
    {"tui.editor.jumpForward", {"ctrl+]"}},
    {"tui.editor.jumpBackward", {"ctrl+alt+]"}},
    {"tui.editor.pageUp", {"pageup"}},
    {"tui.editor.pageDown", {"pagedown"}},
    {"tui.editor.deleteCharBackward", {"backspace"}},
    {"tui.editor.deleteCharForward", {"delete", "ctrl+d"}},
    {"tui.editor.deleteWordBackward", {"ctrl+w", "alt+backspace", "ctrl+backspace", "super+alt+backspace"}},
    {"tui.editor.deleteWordForward", {"alt+delete", "alt+d", "ctrl+delete", "super+alt+delete", "super+alt+d"}},
    {"tui.editor.deleteToLineStart", {"ctrl+u"}},
    {"tui.editor.deleteToLineEnd", {"ctrl+k"}},
    {"tui.editor.yank", {"ctrl+y"}},
    {"tui.editor.yankPop", {"alt+y"}},
    {"tui.editor.undo", {"ctrl+-", "ctrl+_"}},
    {"tui.editor.spellingSuggestions", {"ctrl+."}},
    {"tui.input.newLine", {"shift+enter", "ctrl+j"}},
    {"tui.input.submit", {"enter"}},
    {"tui.input.tab", {"tab"}},
    {"tui.input.copy", {"ctrl+c"}},
    {"tui.select.up", {"up"}},
    {"tui.select.down", {"down"}},
    {"tui.select.pageUp", {"pageup"}},
    {"tui.select.pageDown", {"pagedown"}},
    {"tui.select.confirm", {"enter"}},
    {"tui.select.cancel", {"escape", "ctrl+c"}},
    {"app.interrupt", {"escape"}},
    {"app.clear", {"ctrl+c"}},
    {"app.exit", {"ctrl+d"}},
    {"app.suspend", {"ctrl+z"}},
    {"app.display.reset", {"alt+l"}},
    {"app.thinking.cycle", {"shift+tab"}},
    {"app.thinking.toggle", {"ctrl+t"}},
    {"app.model.cycleForward", {"ctrl+p"}},
    {"app.model.cycleBackward", {"shift+ctrl+p"}},
    {"app.model.select", {"alt+m"}},
    {"app.model.selectTemporary", {"alt+p"}},
    {"app.tools.expand", {"ctrl+o"}},
    {"app.tools.toggleVisibility", {"ctrl+shift+o"}},
    {"app.ask.hide", {"alt+h"}},
    {"app.approval.approve", {"ctrl+enter", "ctrl+q"}},
    {"app.editor.external", {"ctrl+g"}},
    {"app.local.open", {"alt+g"}},
    {"app.message.followUp", {"ctrl+q", "ctrl+enter"}},
    {"app.retry", {"f5", "alt+r"}},
    {"app.message.dequeue", {"alt+up", "shift+up"}},
#ifdef __APPLE__
    {"app.clipboard.pasteImage", {"ctrl+v", "super+v"}}, /* getDefaultPasteImageKeys("darwin") */
#else
    {"app.clipboard.pasteImage", {"ctrl+v"}},
#endif
    {"app.clipboard.pasteTextRaw", {"ctrl+shift+v", "alt+shift+v"}},
    {"app.clipboard.copyLine", {"alt+shift+l"}},
    {"app.clipboard.copyPrompt", {"alt+shift+c"}},
    {"app.session.new", {NULL}},
    {"app.session.tree", {NULL}},
    {"app.session.fork", {NULL}},
    {"app.session.resume", {NULL}},
    {"app.agents.hub", {"alt+a"}},
    {"app.session.observe", {"ctrl+s"}},
    {"app.session.togglePath", {"ctrl+p"}},
    {"app.session.toggleSort", {"ctrl+s"}},
    {"app.session.rename", {"ctrl+r"}},
    {"app.session.delete", {"ctrl+d"}},
    {"app.session.deleteNoninvasive", {"ctrl+backspace"}},
    {"app.session.pin", {"alt+k"}},
    {"app.session.unpin", {"alt+shift+k"}},
    {"app.tree.previousBranch", {NULL}},
    {"app.tree.nextBranch", {NULL}},
    {"app.plan.toggle", {"alt+shift+p"}},
    {"app.history.search", {"ctrl+r"}},
    {"app.links.hint", {"alt+o"}},
    {"app.stt.pushToTalk", {"space"}},
    {"app.stt.toggle", {NULL}},
    {"app.live.toggle", {"ctrl+l"}},
};

size_t tui_keybindings_action_count(void) { return ARRAY_LEN(DEFAULTS); }
const char *tui_keybindings_action_at(size_t i) { return i < ARRAY_LEN(DEFAULTS) ? DEFAULTS[i].action : NULL; }

struct tui_keybindings {
    hmap_t defaults; /* action -> strvec_t* of canonical default chords */
    strvec_t order;  /* defined actions, definition order */
    json_t *user;    /* user overrides {"action": "chord" | ["chord", ...]} (object, owned) */
    hmap_t map;      /* action -> strvec_t* of effective canonical chords */
};

static void free_sv(void *p) {
    strvec_t *v = p;
    strvec_free(v);
    free(v);
}

/* Canonical chord for a user-supplied key id: omp lowercases ids before matching (normalizeKeyId), so
 * "ctrl+A" binds ctrl+a, not ctrl+shift+a. */
static bool user_canonical(const char *spec, char *buf, size_t cap) {
    char *low = str_lower(spec);
    bool ok = tui_key_canonical(low, buf, cap);
    free(low);
    return ok;
}

/* Effective chords from a user value (string or string array); NULL when the value is neither. */
static strvec_t *user_keys(const json_t *val) {
    if (!json_is_str(val) && !json_is_arr(val)) return NULL;
    strvec_t *v = xcalloc(1, sizeof *v);
    char c[64];
    if (json_is_str(val)) {
        if (user_canonical(json_sv(val), c, sizeof c)) strvec_push(v, c);
        return v;
    }
    json_t *it;
    json_arr_foreach(val, j, it) {
        if (!json_is_str(it)) { /* omp ignores arrays with non-string members entirely */
            free_sv(v);
            return NULL;
        }
        if (user_canonical(json_sv(it), c, sizeof c) && !strvec_contains(v, c)) strvec_push(v, c);
    }
    return v;
}

/* app.message.followUp / app.message.dequeue default fallback keys (Windows Terminal ctrl+q, macOS
 * Terminal.app shift+up) are dropped when the user bound that key to another known action
 * (app-keybindings.ts getKeys / userBindingClaimsKey). */
static const char *fallback_key(const char *action) {
    if (!strcmp(action, "app.message.followUp")) return "ctrl+q";
    if (!strcmp(action, "app.message.dequeue")) return "shift+up";
    return NULL;
}

static bool user_claims(const tui_keybindings_t *kb, const char *key, const char *except) {
    const char *name;
    json_t *val;
    json_obj_foreach(kb->user, i, name, val) {
        if (!strcmp(name, except) || !hmap_get(&kb->defaults, name)) continue;
        strvec_t *v = user_keys(val);
        bool hit = v && strvec_contains(v, key);
        if (v) free_sv(v);
        if (hit) return true;
    }
    return false;
}

static void rebuild(tui_keybindings_t *kb) {
    hmap_free(&kb->map, free_sv);
    hmap_init(&kb->map);
    for (size_t i = 0; i < kb->order.n; i++) {
        const char *a = kb->order.v[i];
        strvec_t *v = user_keys(json_get(kb->user, a));
        if (!v) {
            const strvec_t *d = hmap_get(&kb->defaults, a);
            v = xcalloc(1, sizeof *v);
            for (size_t j = 0; d && j < d->n; j++) strvec_push(v, d->v[j]);
            const char *fb = fallback_key(a);
            if (fb && strvec_contains(v, fb) && user_claims(kb, fb, a)) {
                strvec_t keep = {0};
                for (size_t j = 0; j < v->n; j++)
                    if (strcmp(v->v[j], fb)) strvec_push(&keep, v->v[j]);
                strvec_free(v);
                *v = keep;
            }
        }
        hmap_put(&kb->map, a, v);
    }
    /* user entries for actions nobody defined still resolve (applications may look them up) */
    const char *name;
    json_t *val;
    json_obj_foreach(kb->user, i, name, val) {
        if (hmap_get(&kb->defaults, name)) continue;
        strvec_t *v = user_keys(val);
        if (v) {
            strvec_t *old = hmap_put(&kb->map, name, v);
            if (old) free_sv(old);
        }
    }
}

void tui_keybindings_define(tui_keybindings_t *kb, const char *action, const char *const *chords) {
    strvec_t *v = xcalloc(1, sizeof *v);
    for (size_t i = 0; chords && chords[i]; i++) {
        char c[64];
        if (tui_key_canonical(chords[i], c, sizeof c) && !strvec_contains(v, c)) strvec_push(v, c);
    }
    strvec_t *old = hmap_put(&kb->defaults, action, v);
    if (old) free_sv(old);
    else strvec_push(&kb->order, action);
    rebuild(kb);
}

tui_keybindings_t *tui_keybindings_new(void) {
    tui_keybindings_t *kb = xcalloc(1, sizeof *kb);
    hmap_init(&kb->defaults);
    hmap_init(&kb->map);
    kb->user = json_obj();
    for (size_t i = 0; i < ARRAY_LEN(DEFAULTS); i++) {
        strvec_t *v = xcalloc(1, sizeof *v);
        for (size_t j = 0; j < 6 && DEFAULTS[i].chords[j]; j++) {
            char c[64];
            if (tui_key_canonical(DEFAULTS[i].chords[j], c, sizeof c) && !strvec_contains(v, c)) strvec_push(v, c);
        }
        hmap_put(&kb->defaults, DEFAULTS[i].action, v);
        strvec_push(&kb->order, DEFAULTS[i].action);
    }
    rebuild(kb);
    return kb;
}

void tui_keybindings_free(tui_keybindings_t *kb) {
    if (!kb) return;
    hmap_free(&kb->map, free_sv);
    hmap_free(&kb->defaults, free_sv);
    strvec_free(&kb->order);
    json_free(kb->user);
    free(kb);
}

void tui_keybindings_set_user(tui_keybindings_t *kb, const json_t *cfg) {
    json_free(kb->user);
    kb->user = json_obj();
    const char *name;
    json_t *val;
    if (json_is_obj(cfg)) json_obj_foreach(cfg, i, name, val) if (json_is_str(val) || json_is_arr(val))
        json_set(kb->user, name, json_clone(val));
    rebuild(kb);
}

const json_t *tui_keybindings_user(const tui_keybindings_t *kb) { return kb->user; }

int tui_keybindings_load_json(tui_keybindings_t *kb, const json_t *obj) {
    if (!json_is_obj(obj)) return 0;
    int changed = 0;
    const char *key;
    json_t *val;
    json_obj_foreach(obj, i, key, val) {
        if (!json_is_str(val) && !json_is_arr(val)) continue;
        json_set(kb->user, key, json_clone(val));
        changed++;
    }
    rebuild(kb);
    return changed;
}

int tui_keybindings_load_file(tui_keybindings_t *kb, const char *path) {
    size_t len;
    char *data = fs_read(path, &len);
    if (!data) return -1;
    json_t *j = json_parse(data, len, NULL);
    free(data);
    if (!j) return -1;
    int n = tui_keybindings_load_json(kb, j);
    json_free(j);
    return n;
}

const strvec_t *tui_keybindings_get(const tui_keybindings_t *kb, const char *action) {
    return hmap_get(&kb->map, action);
}

bool tui_keybindings_key_bound(const tui_keybindings_t *kb, const char *chord, const char *action) {
    const strvec_t *v = hmap_get(&kb->map, action);
    char c[64];
    if (!v || !tui_key_canonical(chord, c, sizeof c)) return false;
    for (size_t i = 0; i < v->n; i++)
        if (canon_matches(c, v->v[i])) return true;
    return false;
}

bool tui_keybindings_matches(const tui_keybindings_t *kb, const tui_key_t *k, const char *action) {
    const strvec_t *v = hmap_get(&kb->map, action);
    if (!v || !v->n) return false;
    char id[64];
    tui_key_id(k, id, sizeof id);
    if (!id[0]) return false;
    for (size_t i = 0; i < v->n; i++)
        if (canon_matches(id, v->v[i])) return true;
    return false;
}

static tui_keybindings_t *g_kb;

tui_keybindings_t *tui_keybindings_global(void) {
    if (!g_kb) g_kb = tui_keybindings_new();
    return g_kb;
}

void tui_keybindings_set_global(tui_keybindings_t *kb) {
    if (g_kb && g_kb != kb) tui_keybindings_free(g_kb);
    g_kb = kb;
}

bool tui_kb(const tui_key_t *k, const char *action) {
    return tui_keybindings_matches(tui_keybindings_global(), k, action);
}
