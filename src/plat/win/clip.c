/* clip.c - the Windows clipboard (what omp's natives clipboard.rs reaches through arboard / clipboard-win): UTF-16
 * text, the registered "PNG" format browsers and screenshot tools put up, and CF_DIB, which Windows synthesizes from
 * any bitmap format. Decoding a DIB is src/img/dib.c's job; this file only moves bytes. */
#include "win.h"

#include <stdlib.h>
#include <string.h>

#define CLIP_MAX_BYTES ((size_t)256 << 20) /* beyond any image omc would attach */

/* another program may hold the clipboard for a moment: retry for about half a second */
static bool clip_open(void) {
    for (int i = 0; i < 20; i++) {
        if (OpenClipboard(NULL)) return true;
        Sleep(25);
    }
    return false;
}

static UINT clip_format(int kind) {
    switch (kind) {
    case OMC_CLIP_TEXT: return CF_UNICODETEXT;
    case OMC_CLIP_PNG: return RegisterClipboardFormatW(L"PNG");
    case OMC_CLIP_DIB: return CF_DIB;
    default: return 0;
    }
}

bool omc_win_clipboard_has(int kind) {
    if (kind == OMC_CLIP_DIB) /* CF_DIB, CF_DIBV5 and CF_BITMAP synthesize from one another */
        return IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5) ||
               IsClipboardFormatAvailable(CF_BITMAP);
    UINT f = clip_format(kind);
    return f && IsClipboardFormatAvailable(f);
}

int omc_win_clipboard_read(int kind, char **out, size_t *len) {
    *out = NULL;
    *len = 0;
    UINT f = clip_format(kind);
    if (!f || !IsClipboardFormatAvailable(f)) return 0;
    if (!clip_open()) return -1;
    int rc = 0;
    HANDLE h = GetClipboardData(f);
    void *p = h ? GlobalLock(h) : NULL;
    if (p) {
        SIZE_T sz = GlobalSize(h);
        if (kind == OMC_CLIP_TEXT) {
            size_t wn = wcsnlen((const wchar_t *)p, sz / sizeof(wchar_t));
            int n = wn ? WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)p, (int)wn, NULL, 0, NULL, NULL) : 0;
            char *s = malloc((size_t)n + 1);
            if (s) {
                if (n) WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)p, (int)wn, s, n, NULL, NULL);
                s[n] = 0;
                *out = s, *len = (size_t)n, rc = 1;
            } else rc = -1;
        } else if (sz > 0 && sz <= CLIP_MAX_BYTES) {
            char *b = malloc(sz);
            if (b) {
                memcpy(b, p, sz);
                *out = b, *len = sz, rc = 1;
            } else rc = -1;
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return rc;
}

int omc_win_clipboard_set_text(const char *utf8, size_t n) {
    int wn = n ? MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, NULL, 0) : 0;
    if (n && wn <= 0) return -1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, ((size_t)wn + 1) * sizeof(wchar_t));
    if (!g) return -1;
    wchar_t *w = GlobalLock(g);
    if (!w) {
        GlobalFree(g);
        return -1;
    }
    if (wn) MultiByteToWideChar(CP_UTF8, 0, utf8, (int)n, w, wn);
    w[wn] = 0;
    GlobalUnlock(g);
    if (!clip_open()) {
        GlobalFree(g);
        return -1;
    }
    int rc = -1;
    if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, g)) rc = 0; /* the clipboard owns g now */
    else GlobalFree(g);
    CloseClipboard();
    return rc;
}
