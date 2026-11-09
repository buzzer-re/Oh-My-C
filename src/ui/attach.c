/* attach.c - composer image attachments (ports of omp's input-controller image paste flow,
 * prompt/custom-editor.ts path detection, prompt/composer-attachments.ts chips/markers and
 * prompt/attachment-chips.ts band).
 *
 * A pasted / dropped image path, an empty bracketed paste or ctrl+v with an image on the clipboard
 * becomes a pending attachment: the image is normalized for the model (images.autoResize), the buffer gets
 * an atomic `📷 #N` chip that expands to `[Image #N, WxH]` on submit, and a card per referenced chip is
 * drawn above the composer. On submit unreferenced attachments are dropped and the rest renumbered in
 * order (compactImageMarkers); the source path of each reaches the model as a hidden image-attachment
 * notice (app_prompt_ex).
 */
#include "ui.h"
#include "config/settings.h"
#include "core/fs.h"
#include "core/proc.h"
#include "img/imgpipe.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define INNER_COLS 12
#define INNER_ROWS 4
#define CARD_COLS (INNER_COLS + 2)
#define CARD_GAP 2

static const uint8_t PALETTE[6][3] = {{255, 179, 102}, {125, 207, 255}, {189, 147, 249},
                                      {105, 220, 158}, {255, 141, 188}, {240, 223, 120}};

/* attachmentSgr("image", n) */
char *ui_attachment_sgr(int n) {
    const uint8_t *c = PALETTE[(size_t)(n - 1) % 6];
    return xasprintf("\x1b[38;2;%d;%d;%dm", c[0], c[1], c[2]);
}

/* attachmentSgr("paste", n): the paste palette starts two entries further on */
char *ui_paste_attachment_sgr(int n) {
    const uint8_t *c = PALETTE[(size_t)(n + 2) % 6];
    return xasprintf("\x1b[38;2;%d;%d;%dm", c[0], c[1], c[2]);
}

/* chipLabel("image", n) */
char *ui_chip_label(int n) { return xasprintf("%s #%d", tui_sym("chip.image"), n); }

/* ---------- pasted path detection (custom-editor.ts) ---------- */

static bool is_sep(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static bool abs_prefix(const char *p) {
    if (p[0] == '/') return true;
    if (p[0] == '~' && p[1] == '/') return true;
    if (!strncasecmp(p, "file://", 7)) return true;
    if (p[0] == '\\' && p[1] == '\\') return true;
    return isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}

static bool has_uri_scheme(const char *p) {
    if (!isalpha((unsigned char)*p)) return false;
    const char *q = p + 1;
    while (isalnum((unsigned char)*q) || *q == '+' || *q == '.' || *q == '-') q++;
    return *q == ':';
}

static bool explicit_path(const char *p) {
    if (abs_prefix(p) || (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) ||
        (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\')))
        return true;
    if (has_uri_scheme(p)) return false;
    return strchr(p, '\\') != NULL;
}

/* BRACKETED_IMAGE_PATH_REGEX: .png .jpg .jpeg .gif .webp (case-insensitive) */
static bool image_ext(const char *p) {
    const char *dot = strrchr(p, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".gif") ||
           !strcasecmp(dot, ".webp");
}

static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* normalizePastedPath: trim, strip one level of matching quotes, file:// URL -> path, shell unescape */
static char *normalize_pasted_path(const char *in) {
    char *t = str_trim(in);
    size_t n = strlen(t);
    if (n > 1 && (t[0] == '"' || t[0] == '\'') && t[n - 1] == t[0]) {
        memmove(t, t + 1, n - 2);
        t[n - 2] = 0;
    }
    sb_t b = SB_INIT;
    if (!strncasecmp(t, "file://", 7)) {
        const char *p = t + 7;
        if (!strncasecmp(p, "localhost/", 10)) p += 9;
        for (; *p; p++) {
            if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) {
                sb_putc(&b, (char)(hexv(p[1]) * 16 + hexv(p[2])));
                p += 2;
            } else {
                sb_putc(&b, *p);
            }
        }
        free(t);
        return sb_detach(&b);
    }
    const char *p = t;
    if (p[0] == '\\' && p[1] == '~' && p[2] == '/') p++;
    static const char esc[] = "\\ \t\n\r'\"()[]{}&;<>|?*!$`";
    for (; *p; p++) {
        if (*p == '\\' && p[1] && strchr(esc, p[1])) {
            sb_putc(&b, p[1]);
            p++;
        } else {
            sb_putc(&b, *p);
        }
    }
    free(t);
    return sb_detach(&b);
}

/* splitPastedPathSegments: whitespace-separated, quotes and escapes kept; false on an open quote/escape */
static bool split_segments(const char *s, strvec_t *out) {
    sb_t seg = SB_INIT;
    char quote = 0;
    bool escaped = false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (escaped) {
            sb_putc(&seg, c);
            escaped = false;
        } else if (c == '\\') {
            sb_putc(&seg, c);
            escaped = true;
        } else if (quote) {
            sb_putc(&seg, c);
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            sb_putc(&seg, c);
            quote = c;
        } else if (is_sep(c)) {
            if (seg.len) strvec_push(out, sb_cstr(&seg)), sb_clear(&seg);
        } else {
            sb_putc(&seg, c);
        }
    }
    if (escaped || quote) {
        sb_free(&seg);
        return false;
    }
    if (seg.len) strvec_push(out, sb_cstr(&seg));
    sb_free(&seg);
    return out->n > 0;
}

/* extractExplicitPathSegments */
static bool explicit_segments(const char *text, strvec_t *paths) {
    char *t = str_trim(text);
    strvec_t segs = {0};
    bool ok = *t && split_segments(t, &segs);
    for (size_t i = 0; ok && i < segs.n; i++) {
        char *p = normalize_pasted_path(segs.v[i]);
        if (!*p || !explicit_path(p)) ok = false;
        else strvec_push(paths, p);
        free(p);
    }
    strvec_free(&segs);
    free(t);
    if (!ok) strvec_free(paths);
    return ok;
}

/* INTERIOR_PATH_ANCHOR_REGEX: unescaped whitespace followed by another path anchor */
static bool interior_anchor(const char *s) {
    for (const char *p = s + 1; *p; p++) {
        if (!isspace((unsigned char)*p) || p[-1] == '\\') continue;
        const char *q = p + 1;
        if (abs_prefix(q) || (q[0] == '.' && (q[1] == '/' || q[1] == '\\')) ||
            (q[0] == '.' && q[1] == '.' && (q[2] == '/' || q[2] == '\\')))
            return true;
    }
    return false;
}

/* extractImagePastePathsFromText: every segment an image path, or the whole text one anchored image path */
bool ui_extract_image_paths(const char *text, strvec_t *out) {
    strvec_t paths = {0};
    if (explicit_segments(text, &paths)) {
        bool all = true;
        for (size_t i = 0; i < paths.n; i++) all &= image_ext(paths.v[i]);
        if (all) *out = paths;
        else strvec_free(&paths);
        return all;
    }
    char *t = str_trim(text);
    bool ok = *t && !strpbrk(t, "\r\n") && abs_prefix(t) && !interior_anchor(t);
    if (ok) {
        char *p = normalize_pasted_path(t);
        ok = *p && explicit_path(p) && image_ext(p);
        if (ok) strvec_push(out, p);
        free(p);
    }
    free(t);
    return ok;
}

/* ---------- pending attachments ---------- */

static int pending_count(ui_t *u) { return (int)json_len(u->att_images); }

/* insertPendingImage: store the normalized image and put its chip in the buffer */
static void insert_pending(ui_t *u, const char *b64, const char *mime, const char *source) {
    if (!u->att_images) u->att_images = json_arr();
    json_t *e = json_obj();
    json_t *img = json_obj();
    json_set_str(img, "type", "image");
    json_set_str(img, "data", b64);
    json_set_str(img, "mimeType", mime);
    json_set(e, "image", img);
    if (source) json_set_str(e, "path", source);
    size_t n = 0;
    unsigned char *bytes = base64_decode(b64, strlen(b64), &n);
    img_meta_t m;
    bool dims = bytes && img_sniff(bytes, n, &m) && m.w > 0 && m.h > 0;
    free(bytes);
    if (dims) {
        json_set_int(e, "w", m.w);
        json_set_int(e, "h", m.h);
    }
    json_push(u->att_images, e);
    int num = pending_count(u);
    char *label = ui_chip_label(num);
    char *marker = dims ? xasprintf("[Image #%d, %dx%d]", num, m.w, m.h) : xasprintf("[Image #%d]", num);
    char *sgr = ui_attachment_sgr(num);
    tui_editor_insert_atom(u->editor, label, marker, sgr);
    free(label), free(marker), free(sgr);
    tui_request_render(u->tui);
}

/* #normalizePastedImage + insert: images.autoResize re-encodes for the model (no model-specific policy) */
static bool attach_bytes(ui_t *u, const uint8_t *bytes, size_t n, const char *source, const char *unsupported) {
    img_meta_t m;
    if (!img_sniff(bytes, n, &m)) {
        add_notice(u, "info", unsupported);
        return false;
    }
    char *b64 = NULL;
    const char *mime = img_fmt_mime(m.fmt);
    if (settings_bool("images.autoResize", true)) {
        img_resized_t r;
        if (img_resize_image(bytes, n, mime, NULL, &r, NULL) == 0) {
            b64 = base64_encode(r.buf, r.len);
            mime = r.mime;
            img_resized_free(&r);
        }
    }
    if (!b64) b64 = base64_encode(bytes, n);
    insert_pending(u, b64, mime, source);
    free(b64);
    return true;
}

/* ---------- clipboard ---------- */

#ifndef __APPLE__ /* only the Linux clipboard readers below use it */
static bool have_cmd(const char *name) {
    const char *argv[] = {"sh", "-c", "command -v \"$0\" >/dev/null 2>&1", name, NULL};
    proc_opts_t o = {.timeout_ms = 3000};
    proc_result_t r;
    if (proc_run(argv, &o, &r) != 0) return false;
    bool ok = r.exit_code == 0;
    proc_result_free(&r);
    return ok;
}
#endif

static bool run_capture(const char *const *argv, char **out, size_t *len) {
    proc_opts_t o = {.timeout_ms = 5000, .max_output = IMG_MAX_INPUT_BYTES + 1};
    proc_result_t r;
    if (proc_run(argv, &o, &r) != 0) return false;
    bool ok = r.exit_code == 0 && !r.truncated && r.out_len > 0;
    if (ok) {
        *out = r.out;
        *len = r.out_len;
        r.out = NULL;
    }
    proc_result_free(&r);
    return ok;
}

/* readImageFromClipboard: wl-paste (Wayland), xclip (X11), osascript (macOS) */
static bool clipboard_image(char **data, size_t *len) {
    if (getenv("TERMUX_VERSION")) return false;
#ifdef __APPLE__
    char *tmp = xasprintf("%s/omc-clip-%d.png", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    char *script = xasprintf("set f to (open for access POSIX file \"%s\" with write permission)\n"
                             "try\nwrite (the clipboard as «class PNGf») to f\nend try\nclose access f",
                             tmp);
    const char *argv[] = {"osascript", "-e", script, NULL};
    char *o = NULL;
    size_t ol = 0;
    run_capture(argv, &o, &ol);
    free(o);
    free(script);
    *data = fs_read_limit(tmp, IMG_MAX_INPUT_BYTES + 1, len, NULL);
    unlink(tmp);
    free(tmp);
    if (*data && *len > 0) return true;
    free(*data);
    *data = NULL;
    return false;
#else
    static const char *const mimes[] = {"image/png", "image/jpeg", "image/gif", "image/webp"};
    if (getenv("WAYLAND_DISPLAY") && have_cmd("wl-paste")) {
        const char *lt[] = {"wl-paste", "--list-types", NULL};
        char *types = NULL;
        size_t tl = 0;
        if (run_capture(lt, &types, &tl)) {
            for (size_t i = 0; i < 4; i++) {
                if (!strstr(types, mimes[i])) continue;
                const char *argv[] = {"wl-paste", "--type", mimes[i], NULL};
                if (run_capture(argv, data, len)) {
                    free(types);
                    return true;
                }
            }
            free(types);
        }
    }
    if (!getenv("DISPLAY") || !have_cmd("xclip")) return false;
    const char *lt[] = {"xclip", "-selection", "clipboard", "-t", "TARGETS", "-o", NULL};
    char *types = NULL;
    size_t tl = 0;
    if (!run_capture(lt, &types, &tl)) return false;
    bool ok = false;
    for (size_t i = 0; i < 4 && !ok; i++) {
        if (!strstr(types, mimes[i])) continue;
        const char *argv[] = {"xclip", "-selection", "clipboard", "-t", mimes[i], "-o", NULL};
        ok = run_capture(argv, data, len);
    }
    free(types);
    return ok;
#endif
}

static char *clipboard_text(void) {
    char *out = NULL;
    size_t n = 0;
#ifdef __APPLE__
    const char *argv[] = {"pbpaste", NULL};
    if (run_capture(argv, &out, &n)) return out;
#else
    if (getenv("WAYLAND_DISPLAY") && have_cmd("wl-paste")) {
        const char *argv[] = {"wl-paste", "--type", "text", "--no-newline", NULL};
        if (run_capture(argv, &out, &n)) return out;
    }
    if (getenv("DISPLAY") && have_cmd("xclip")) {
        const char *argv[] = {"xclip", "-selection", "clipboard", "-o", NULL};
        if (run_capture(argv, &out, &n)) return out;
    }
#endif
    return NULL;
}

/* #clipboardIsTerminalLocal: over SSH the readable clipboard is the remote host's */
static bool over_ssh(void) { return getenv("SSH_CONNECTION") || getenv("SSH_TTY") || getenv("SSH_CLIENT"); }

/* #persistPastedImage: commit clipboard bytes to the session's local:// root; returns the local:// URL */
static char *persist_pasted(ui_t *u, const char *data, size_t n, const char *mime) {
    const char *root = u->app->env ? u->app->env->local_dir : NULL;
    if (!root) return NULL;
    const char *ext = !strcmp(mime, "image/jpeg") ? "jpg" : strchr(mime, '/') + 1;
    char *name = xasprintf("pasted-image-%016llx.%s", (unsigned long long)img_hash(data, n), ext);
    char *path = path_join(root, name);
    fs_mkdir_p(root, 0755);
    char *url = fs_write(path, data, n) == 0 ? xasprintf("local://%s", name) : NULL;
    free(path), free(name);
    return url;
}

static bool try_clipboard_image(ui_t *u) {
    if (over_ssh()) return false;
    char *data = NULL;
    size_t n = 0;
    if (!clipboard_image(&data, &n)) return false;
    img_meta_t m;
    const char *mime = img_sniff((const uint8_t *)data, n, &m) ? img_fmt_mime(m.fmt) : "image/png";
    char *msg = xasprintf("Unsupported clipboard image format: %s", mime);
    char *src = persist_pasted(u, data, n, mime);
    attach_bytes(u, (const uint8_t *)data, n, src, msg);
    free(src), free(msg), free(data);
    return true;
}

/* #pasteImagePath */
static void paste_image_path(ui_t *u, const char *path) {
    char *abs = path_abs(path, u->app->cwd);
    struct stat st;
    if (stat(abs, &st) != 0) {
        if (try_clipboard_image(u)) goto out;
        /* untrusted terminal input: control chars dropped, home as ~, bounded to 80 columns */
        char *home = path_display(path, NULL);
        sb_t cl = SB_INIT;
        for (const char *p = home; *p; p++) sb_putc(&cl, (unsigned char)*p < 0x20 || *p == 0x7f ? ' ' : *p);
        free(home);
        char *disp = tui_truncate_to_width(sb_cstr(&cl), 80, NULL);
        sb_free(&cl);
        char *m = over_ssh() ? xasprintf("Image not found at %s. Over SSH this path is local to your terminal — paste the "
                                         "image directly (clipboard image-paste shortcut) to send its bytes.",
                                         disp)
                             : xasprintf("Image not found at %s", disp);
        add_notice(u, "info", m);
        free(m), free(disp);
        goto out;
    }
    size_t hn = 0;
    char *head = fs_read_limit(abs, 256u << 10, &hn, NULL);
    img_meta_t meta;
    bool is_img = head && img_sniff((const uint8_t *)head, hn, &meta);
    free(head);
    if (!is_img) {
        if (try_clipboard_image(u)) goto out;
        tui_editor_paste(u->editor, path);
        add_notice(u, "info", "Pasted path is not a supported image");
        goto out;
    }
    if (st.st_size > (off_t)IMG_MAX_INPUT_BYTES) {
        char *a = img_format_bytes((uint64_t)st.st_size), *b = img_format_bytes(IMG_MAX_INPUT_BYTES);
        char *m = xasprintf("Image file too large: %s exceeds %s limit.", a, b);
        tui_editor_paste(u->editor, path);
        add_notice(u, "info", m);
        free(a), free(b), free(m);
        goto out;
    }
    size_t n = 0;
    char *data = fs_read_limit(abs, IMG_MAX_INPUT_BYTES + 1, &n, NULL);
    img_loaded_t l;
    char *err = NULL;
    const char *mime = img_fmt_mime(meta.fmt);
    if (!data || img_load_input((const uint8_t *)data, n, mime, abs, "Read image file", false, 0, &l, &err) < 0) {
        free(err);
        free(data);
        if (try_clipboard_image(u)) goto out;
        tui_editor_paste(u->editor, path);
        add_notice(u, "info", "Failed to read pasted image path");
        goto out;
    }
    char *msg = xasprintf("Unsupported pasted image format: %s", l.mime);
    attach_bytes(u, (const uint8_t *)data, n, abs, msg);
    free(msg);
    img_loaded_free(&l);
    free(data);
out:
    free(abs);
}

/* bracketed paste router: empty paste -> clipboard image; image paths -> attachments */
static bool paste_hook(void *ud, tui_editor_t *ed, const char *text) {
    ui_t *u = ud;
    if (!*text) {
        ui_attach_paste_clipboard(u);
        return true;
    }
    strvec_t paths = {0};
    if (!ui_extract_image_paths(text, &paths)) return false;
    for (size_t i = 0; i < paths.n; i++) paste_image_path(u, paths.v[i]);
    strvec_free(&paths);
    tui_request_render(u->tui);
    return true;
}

/* ---------- large pastes: text-attachment chips and the large-paste menu ---------- */

/* editor.insertTextAttachment: `📄 #N` chip in the buffer, a card in the band, `expansion` (NULL = text) on submit */
static void insert_text_chip(ui_t *u, const char *text, const char *expansion) {
    char *sgr = ui_paste_attachment_sgr(u->editor->text_counter + 1);
    tui_editor_insert_text_attachment(u->editor, text, expansion, sgr);
    free(sgr);
    tui_request_render(u->tui);
}

typedef struct {
    ui_t *u;
    char *text;
    size_t lines;
} paste_menu_t;

static int g_paste_counter; /* InputController #pasteCounter */

/* #attachPasteAsFile: save to the session's local:// root, reference `local://paste-N.md` */
static void attach_paste_as_file(ui_t *u, const char *text, size_t lines) {
    const char *root = u->app->env ? u->app->env->local_dir : NULL;
    char *name = NULL, *path = NULL;
    bool ok = false;
    if (root && fs_mkdir_p(root, 0755) == 0) {
        do {
            free(name), free(path);
            name = xasprintf("paste-%d.md", ++g_paste_counter);
            path = path_join(root, name);
        } while (fs_exists(path));
        ok = fs_write(path, text, strlen(text)) == 0;
    }
    if (ok) {
        char *ref = xasprintf("local://%s ", name);
        tui_editor_insert_text(u->editor, ref);
        char *m = xasprintf("Saved %zu pasted lines to local://%s", lines, name);
        add_notice(u, "info", m);
        free(m), free(ref);
    } else {
        insert_text_chip(u, text, NULL);
        add_notice(u, "error", "Failed to save paste to a file — attached as a text chip instead");
    }
    free(name), free(path);
}

static void paste_menu_done(void *ud, int choice) {
    paste_menu_t *m = ud;
    ui_t *u = m->u;
    if (choice == 0) { /* wrapPasteInAttachmentBlock */
        char *wrapped = xasprintf("<attachment>\n%s\n</attachment>", m->text);
        insert_text_chip(u, m->text, wrapped);
        free(wrapped);
    } else if (choice == 1) {
        attach_paste_as_file(u, m->text, m->lines);
    } else { /* "Paste inline" and Esc both stage the text-attachment chip */
        insert_text_chip(u, m->text, NULL);
    }
    tui_request_render(u->tui);
    free(m->text);
    free(m);
}

/* handleLargePaste: below paste.largeMenuThreshold lines the paste becomes a chip, otherwise the menu decides */
static bool large_paste_hook(void *ud, tui_editor_t *ed, const char *text, size_t lines) {
    ui_t *u = ud;
    if (ed != u->editor) return false;
    double threshold = settings_num("paste.largeMenuThreshold", 100);
    if (!(threshold > 0) || (double)lines < threshold) {
        insert_text_chip(u, text, NULL);
        return true;
    }
    static const char *const labels[] = {"Attach as a wrapped block", "Attach as local file", "Paste inline"};
    static const char *const descs[] = {"Wrap the text in <attachment> tags, collapsed to a marker",
                                        "Save the text to a local://paste file",
                                        "Collapse the text to an inline paste marker"};
    paste_menu_t *m = xcalloc(1, sizeof *m);
    m->u = u;
    m->text = xstrdup(text);
    m->lines = lines;
    char *title = xasprintf("Pasted %zu lines", lines);
    char *esc = ui_editor_key("tui.select.cancel"), *help = xasprintf("%s to paste inline", esc);
    ui_hook_select_desc(u, title, labels, descs, 3, help, paste_menu_done, m);
    free(title), free(esc), free(help);
    return true;
}

/* handleClipboardTextRawPaste (app.clipboard.pasteTextRaw): the clipboard text inserted as typed, no marker */
void ui_attach_paste_raw(ui_t *u) {
    if (over_ssh()) {
        add_notice(u, "info", "Clipboard here belongs to the SSH host — use your terminal's paste shortcut to paste from your machine");
        return;
    }
    char *text = clipboard_text();
    if (text && *text) {
        tui_editor_insert_text(u->editor, text);
        tui_request_render(u->tui);
    } else {
        add_notice(u, "info", "No text in clipboard to paste raw");
    }
    free(text);
}

/* handleImagePaste (app.clipboard.pasteImage, ctrl+v): image, else the clipboard text (an image path attaches) */
bool ui_attach_paste_clipboard(ui_t *u) {
    if (over_ssh()) {
        add_notice(u, "info", "Clipboard here belongs to the SSH host \u2014 use your terminal's paste shortcut to paste from your machine");
        return false;
    }
    if (try_clipboard_image(u)) return true;
    char *text = clipboard_text();
    if (!text || !*text) {
        free(text);
        add_notice(u, "info", "Clipboard is empty");
        return false;
    }
    strvec_t paths = {0};
    if (ui_extract_image_paths(text, &paths) && paths.n == 1) {
        paste_image_path(u, paths.v[0]);
    } else {
        tui_editor_paste(u->editor, text);
    }
    strvec_free(&paths);
    free(text);
    tui_request_render(u->tui);
    return true;
}

/* ---------- submit: compactImageMarkers ---------- */

/* `[Image #N` + optional `,...` + `]` at p; sets *num and *len */
static bool vision_marker_at(const char *p, int *num, size_t *len) {
    if (strncmp(p, "[Image #", 8)) return false;
    const char *q = p + 8;
    if (*q < '1' || *q > '9') return false;
    long v = 0;
    while (isdigit((unsigned char)*q) && v < 100000000) v = v * 10 + (*q++ - '0');
    if (*q == ',')
        while (*q && *q != ']' && *q != '\n') q++;
    if (*q != ']') return false;
    *num = (int)v;
    *len = (size_t)(q + 1 - p);
    return true;
}

/* takes `attachments`, the entries ui_attach_take returned, back as the pending list */
void ui_attach_restore(ui_t *u, json_t *attachments) {
    json_free(u->att_images);
    u->att_images = attachments;
}

json_t *ui_attach_take(ui_t *u, const char *text, char **out_text) {
    int count = pending_count(u);
    *out_text = xstrdup(text);
    if (count == 0) {
        json_free(u->att_images);
        u->att_images = NULL;
        return NULL;
    }
    int keep[256];
    int nkeep = 0;
    bool seen[257] = {false};
    for (const char *p = text; *p; p++) {
        int num;
        size_t len;
        if (vision_marker_at(p, &num, &len) && num <= count && num <= 256 && !seen[num]) {
            seen[num] = true;
            keep[nkeep++] = num;
        }
    }
    for (int i = 1; i < nkeep; i++) /* ascending (byAppearance off) */
        for (int j = i; j > 0 && keep[j - 1] > keep[j]; j--) {
            int t = keep[j];
            keep[j] = keep[j - 1];
            keep[j - 1] = t;
        }
    int remap[257] = {0};
    for (int i = 0; i < nkeep; i++) remap[keep[i]] = i + 1;
    sb_t b = SB_INIT;
    for (const char *p = text; *p;) {
        int num;
        size_t len;
        if (vision_marker_at(p, &num, &len) && num <= 256 && remap[num]) {
            const char *tail = p + 8;
            while (isdigit((unsigned char)*tail)) tail++;
            sb_appendf(&b, "[Image #%d%.*s", remap[num], (int)(p + len - tail), tail);
            p += len;
        } else {
            sb_putc(&b, *p++);
        }
    }
    free(*out_text);
    *out_text = sb_detach(&b);
    json_t *out = json_arr();
    for (int i = 0; i < nkeep; i++) json_push(out, json_clone(json_at(u->att_images, (size_t)keep[i] - 1)));
    json_free(u->att_images);
    u->att_images = NULL;
    if (!json_len(out)) {
        json_free(out);
        return NULL;
    }
    return out;
}

void ui_attach_clear(ui_t *u) {
    json_free(u->att_images);
    u->att_images = NULL;
}

/* ---------- transcript chips ---------- */

/* collapseImageMarkers + userBubbleColor: `[Image #N, WxH]` -> styled `📷 #N`, `reset` resumes the bubble fg */
char *ui_collapse_image_markers(const char *text, const char *reset) {
    sb_t b = SB_INIT;
    for (const char *p = text; *p;) {
        int num;
        size_t len;
        if (vision_marker_at(p, &num, &len)) {
            char *sgr = ui_attachment_sgr(num), *label = ui_chip_label(num);
            if (reset) sb_appendf(&b, "%s\x1b[1m%s\x1b[22m%s", sgr, label, reset);
            else sb_append(&b, label);
            free(sgr), free(label);
            p += len;
        } else {
            sb_putc(&b, *p++);
        }
    }
    return sb_detach(&b);
}

/* ---------- band ---------- */

static void border_row(sb_t *row, const char *sgr, const char *caption, bool top) {
    const char *left = tui_sym(top ? "boxRound.topLeft" : "boxRound.bottomLeft");
    const char *right = tui_sym(top ? "boxRound.topRight" : "boxRound.bottomRight");
    const char *h = tui_sym("boxRound.horizontal");
    sb_append(row, sgr);
    sb_append(row, left);
    if (!caption || !*caption) {
        sb_repeat(row, h, INNER_COLS);
    } else {
        char *cut = tui_truncate_to_width(caption, INNER_COLS - 2, NULL);
        int fill = INNER_COLS - tui_visible_width(cut) - 2;
        int lf = MAX(0, fill / 2), rf = MAX(0, fill - lf);
        sb_repeat(row, h, lf);
        sb_appendf(row, " \x1b[1m%s\x1b[22m ", cut);
        sb_repeat(row, h, rf);
        free(cut);
    }
    sb_append(row, right);
    sb_append(row, "\x1b[39m");
}

/* `needle` occurs in `text` not followed by another digit (`📷 #1` must not match `📷 #10`) */
static bool token_in(const char *text, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = strstr(text, needle); p; p = strstr(p + 1, needle))
        if (!isdigit((unsigned char)p[n])) return true;
    return false;
}

/* one 14x6 chip card: rounded border in the identity color, title / caption inset, 4 interior rows of 12 cells */
static void card_rows(sb_t rows[6], int x, const char *sgr, const char *title, const char *caption,
                      char *const interior[INNER_ROWS]) {
    for (int r = 0; r < 6; r++) {
        if (x > 0) sb_appendf(&rows[r], "%*s", CARD_GAP, "");
        if (r == 0) border_row(&rows[r], sgr, title, true);
        else if (r == 5) border_row(&rows[r], sgr, caption, false);
        else {
            sb_appendf(&rows[r], "%s%s\x1b[39m", sgr, tui_sym("boxRound.vertical"));
            sb_append(&rows[r], interior[r - 1]);
            sb_appendf(&rows[r], "%s%s\x1b[39m", sgr, tui_sym("boxRound.vertical"));
        }
    }
}

static void band_render(tui_component_t *c, int width, strvec_t *out) {
    ui_t *u = c->user;
    tui_editor_t *ed = u->editor;
    if (!pending_count(u) && !ed->texts.n) return;
    char *text = tui_editor_get_text(ed);
    sb_t rows[6] = {SB_INIT, SB_INIT, SB_INIT, SB_INIT, SB_INIT, SB_INIT};
    int x = 0, cards = 0;
    bool full = false;
    for (int i = 0; i < pending_count(u) && !full; i++) {
        int num = i + 1;
        char *label = ui_chip_label(num);
        bool referenced = token_in(text, label);
        if (!referenced) { /* also an expanded marker typed / restored into the buffer */
            char *mk = xasprintf("[Image #%d", num);
            referenced = token_in(text, mk);
            free(mk);
        }
        if (!referenced) {
            free(label);
            continue;
        }
        if (x + CARD_COLS > width) {
            free(label);
            full = true;
            break;
        }
        const json_t *e = json_at(u->att_images, (size_t)i);
        int64_t w = json_get_int(e, "w", 0), h = json_get_int(e, "h", 0);
        char *caption = w && h ? xasprintf("%lldx%lld", (long long)w, (long long)h) : xstrdup("");
        char *sgr = ui_attachment_sgr(num);
        const char *icon = tui_sym("chip.image");
        int pad = INNER_COLS - tui_visible_width(icon);
        char *muted = fg(TUI_TC_MUTED, icon);
        char *interior[INNER_ROWS];
        for (int r = 0; r < INNER_ROWS; r++)
            interior[r] = r == 1 ? xasprintf("%*s%s%*s", pad / 2, "", muted, pad - pad / 2, "")
                                 : xasprintf("%*s", INNER_COLS, "");
        card_rows(rows, x, sgr, label, caption, interior);
        for (int r = 0; r < INNER_ROWS; r++) free(interior[r]);
        x += (x > 0 ? CARD_GAP : 0) + CARD_COLS;
        cards++;
        free(muted), free(sgr), free(caption), free(label);
    }
    /* text pastes: the leading 4 rows x 12 cells, muted; `+N lines` or `N chars` */
    for (size_t i = 0; i < ed->texts.n && !full; i++) {
        const tui_editor_text_att_t *t = &ed->texts.v[i];
        if (!token_in(text, t->label)) continue;
        if (x + CARD_COLS > width) break;
        char *caption = t->line_count > 1 ? xasprintf("+%zu lines", t->line_count) : xasprintf("%zu chars", t->char_count);
        char *sgr = ui_paste_attachment_sgr(t->n);
        char *interior[INNER_ROWS];
        const char *p = t->content;
        for (int r = 0; r < INNER_ROWS; r++) {
            const char *nl = p ? strchr(p, '\n') : NULL;
            char *line = p ? (nl ? xstrndup(p, (size_t)(nl - p)) : xstrdup(p)) : xstrdup("");
            p = p && nl ? nl + 1 : NULL;
            char *tabs = tui_replace_tabs(line), *cut = tui_truncate_to_width(tabs, INNER_COLS, NULL);
            char *m = fg(TUI_TC_MUTED, cut);
            interior[r] = xasprintf("%s%*s", m, MAX(0, INNER_COLS - tui_visible_width(cut)), "");
            free(line), free(tabs), free(cut), free(m);
        }
        card_rows(rows, x, sgr, t->label, caption, interior);
        for (int r = 0; r < INNER_ROWS; r++) free(interior[r]);
        x += (x > 0 ? CARD_GAP : 0) + CARD_COLS;
        cards++;
        free(sgr), free(caption);
    }
    free(text);
    for (int r = 0; r < 6; r++) {
        if (cards) strvec_push(out, sb_cstr(&rows[r]));
        sb_free(&rows[r]);
    }
}

static const tui_component_vt BAND_VT = {"attachment-chips", band_render, NULL, NULL, NULL};

tui_component_t *ui_attach_band_new(ui_t *u) {
    tui_component_t *c = xcalloc(1, sizeof *c);
    tui_component_init(c, &BAND_VT);
    c->user = u;
    tui_editor_set_paste_hook(u->editor, paste_hook, u);
    tui_editor_set_large_paste_hook(u->editor, large_paste_hook, u);
    return c;
}
