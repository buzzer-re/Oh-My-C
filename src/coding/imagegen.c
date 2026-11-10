/* imagegen.c - the `generate_image` tool (port of tools/image-gen.ts and the ai/images transports omc can call:
 * openai-images (OpenAI / xAI), openrouter-images, google-generative-ai). Gated by generate_image.enabled; the
 * candidates are the `image` role chain (explicit entries, the session model's imageModel, role defaults).
 * openai-responses hosted image tools ride an openai-responses carrier; the Codex-hosted (streaming), Antigravity and
 * Factory Droid image transports are not ported: those candidates are skipped as unsupported. */
#include "coding/imagegen.h"

#include "config/auth.h"
#include "config/model_resolver.h"
#include "config/settings.h"
#include "core/fs.h"
#include "img/img.h"
#include "net/http.h"
#include "prompt/template.h"

#include <ctype.h>
#include <math.h>

/* MAX_FAILURE_REASON_CHARS: one provider failure as the exhausted-chain error quotes it: whitespace collapsed, cut to 300
 * UTF-16 units (pi-utils truncate, `…`), its closing period dropped (the list adds its own). A gateway's HTML error page
 * must not reach the model whole. */
static char *failure_reason(const char *msg) {
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = msg ? msg : ""; *p; p++) {
        if (isspace((unsigned char)*p)) sp = true;
        else {
            if (sp && b.len) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *p);
        }
    }
    char *t = sb_detach(&b);
    size_t units = 0, cut = 0, n = strlen(t);
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)t[i];
        size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        size_t u = len == 4 ? 2 : 1;
        if (units + u <= 299) cut = i + len;
        units += u;
        i += len;
    }
    if (units > 300) {
        t[cut] = 0;
        char *x = xasprintf("%s…", t);
        free(t);
        t = x;
    }
    size_t l = strlen(t);
    if (l && t[l - 1] == '.') t[l - 1] = 0;
    return t;
}

#define IMAGE_TIMEOUT_MS (3 * 60 * 1000)
#define MAX_IMAGE_SIZE (35 * 1024 * 1024)
#define XAI_MAX_EDIT_IMAGES 3

static const char *const SCHEMA =
    "{\"type\":\"object\",\"properties\":{"
    "\"subject\":{\"type\":\"string\",\"description\":\"main subject\"},"
    "\"action\":{\"type\":\"string\",\"description\":\"what subject is doing\"},"
    "\"scene\":{\"type\":\"string\",\"description\":\"location or environment\"},"
    "\"composition\":{\"type\":\"string\",\"description\":\"camera angle and framing\"},"
    "\"lighting\":{\"type\":\"string\",\"description\":\"lighting setup\"},"
    "\"style\":{\"type\":\"string\",\"description\":\"artistic style\"},"
    "\"text\":{\"type\":\"string\",\"description\":\"text to render\"},"
    "\"changes\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"edits to make\"},"
    "\"aspect_ratio\":{\"type\":\"string\",\"enum\":[\"1:1\",\"3:4\",\"4:3\",\"9:16\",\"16:9\",\"3:2\",\"2:3\"],"
    "\"description\":\"aspect ratio\"},"
    "\"image_size\":{\"type\":\"string\",\"enum\":[\"1024x1024\",\"1536x1024\",\"1024x1536\"],\"description\":\"image size\"},"
    "\"input\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"input image path\"},"
    "\"data\":{\"type\":\"string\",\"description\":\"base64 image data\"},"
    "\"mime_type\":{\"type\":\"string\",\"description\":\"mime type\"}}},\"description\":\"input images\"},"
    "\"model\":{\"type\":\"string\",\"description\":\"image model selector for this request\"}},"
    "\"required\":[\"subject\"]}";

typedef struct {
    char *data, *mime, *size, *quality; /* owned */
} gen_image_t;

typedef struct {
    gen_image_t *v;
    size_t n, cap;
    char *text;  /* owned or NULL */
    char *model; /* the model that ran (owned or NULL) */
    int64_t input_tokens, output_tokens;
    double cost;
} gen_result_t;

static void gen_result_free(gen_result_t *r) {
    for (size_t i = 0; i < r->n; i++) free(r->v[i].data), free(r->v[i].mime), free(r->v[i].size), free(r->v[i].quality);
    free(r->v);
    free(r->text);
    free(r->model);
    memset(r, 0, sizeof *r);
}

/* ---------- prompt / inputs ---------- */

static void strip_trailing_punct(sb_t *b, const char *s) {
    size_t n = strlen(s);
    while (n && strchr(".!,;:", s[n - 1])) n--;
    sb_appendn(b, s, n);
}

/* assemblePrompt */
static char *assemble_prompt(const json_t *args) {
    sb_t subj = SB_INIT;
    sb_append(&subj, json_get_str(args, "subject") ? json_get_str(args, "subject") : "");
    if (json_get_str(args, "action")) sb_appendf(&subj, ", %s", json_get_str(args, "action"));
    if (json_get_str(args, "scene")) sb_appendf(&subj, ", %s", json_get_str(args, "scene"));
    const char *parts[4] = {sb_cstr(&subj), json_get_str(args, "composition"), json_get_str(args, "lighting"),
                            json_get_str(args, "style")};
    sb_t b = SB_INIT;
    bool first = true;
    for (size_t i = 0; i < 4; i++) {
        if (i && !parts[i]) continue;
        if (!first) sb_append(&b, ". ");
        strip_trailing_punct(&b, parts[i]);
        first = false;
    }
    sb_putc(&b, '.');
    if (json_get_str(args, "text") && *json_get_str(args, "text")) sb_appendf(&b, "\n\nText: %s", json_get_str(args, "text"));
    const json_t *ch = json_get_arr(args, "changes"), *c;
    if (json_len(ch)) {
        sb_append(&b, "\n\nChanges:");
        json_arr_foreach(ch, i, c) sb_appendf(&b, "\n- %s", json_sv(c) ? json_sv(c) : "");
    }
    sb_free(&subj);
    return sb_detach(&b);
}

/* resolveInputImage: {data, mimeType} (owned json) or NULL + *err */
static json_t *resolve_input(const json_t *in, const char *cwd, char **err) {
    const char *path = json_get_str(in, "path"), *data = json_get_str(in, "data");
    if (path && *path) {
        char *abs = path_abs(path, cwd);
        struct stat st;
        json_t *r = NULL;
        if (stat(abs, &st) != 0) *err = xasprintf("Image file not found: %s", path);
        else if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
            *err = xasprintf("Cannot load '%s': it is a %s, not a regular file or directory.", path,
                             S_ISFIFO(st.st_mode) ? "FIFO" : S_ISSOCK(st.st_mode) ? "socket" : S_ISCHR(st.st_mode) ? "character device" : "block device");
        else if (st.st_size > MAX_IMAGE_SIZE) *err = xasprintf("Image file too large: %s", path);
        else {
            size_t n = 0;
            char *raw = fs_read_limit(abs, MAX_IMAGE_SIZE + 1, &n, NULL);
            img_meta_t meta;
            if (!raw) *err = xasprintf("Image file not found: %s", path);
            else if (n > MAX_IMAGE_SIZE) *err = xasprintf("Image file too large: %s", path);
            else if (!img_sniff((const uint8_t *)raw, n, &meta) || !img_fmt_mime(meta.fmt))
                *err = xasprintf("Unsupported image type: %s", path);
            else {
                r = json_obj();
                json_set(r, "data", json_str_own(base64_encode(raw, n)));
                json_set_str(r, "mimeType", img_fmt_mime(meta.fmt));
            }
            free(raw);
        }
        free(abs);
        return r;
    }
    if (data && *data) {
        char *t = str_trim(data);
        const char *mime = json_get_str(in, "mime_type");
        char *payload = t, *dmime = NULL;
        if (str_starts(t, "data:")) { /* ^data:([^;]+);base64,(.+)$ */
            const char *semi = strchr(t + 5, ';');
            if (semi && semi > t + 5 && str_starts(semi, ";base64,") && semi[8]) {
                dmime = xstrndup(t + 5, (size_t)(semi - t - 5));
                payload = (char *)semi + 8;
            }
        }
        json_t *r = NULL;
        const char *m = dmime ? dmime : mime;
        if (!m || !*m) *err = xstrdup("mime_type is required when providing raw base64 data.");
        else if (!*payload) *err = xstrdup("Image data is empty.");
        else {
            r = json_obj();
            json_set_str(r, "data", payload);
            json_set_str(r, "mimeType", m);
        }
        free(dmime);
        free(t);
        return r;
    }
    *err = xstrdup("input entries must include either path or data.");
    return NULL;
}

/* ---------- transports ---------- */

/* errorMessage: detail / error.message of a JSON body, else the raw text */
static char *error_message(const char *body) {
    json_t *j = json_parse(body ? body : "", body ? strlen(body) : 0, NULL);
    const char *d = json_get_str(j, "detail");
    if (!d) d = json_get_str(json_get_obj(j, "error"), "message");
    char *r = xstrdup(d ? d : body ? body : "");
    json_free(j);
    return r;
}

static void model_headers(const ai_model_t *m, http_headers_t *h) {
    const char *k;
    json_t *v;
    json_obj_foreach(m->headers, i, k, v) if (json_sv(v)) http_headers_set(h, k, json_sv(v));
}

static char *base_url(const ai_model_t *m, char **err) {
    if (!m->base_url || !*m->base_url) {
        *err = xasprintf("Image model %s/%s has no base URL", m->provider, m->id);
        return NULL;
    }
    char *u = xstrdup(m->base_url);
    size_t n = strlen(u);
    while (n && u[n - 1] == '/') u[--n] = 0;
    return u;
}

/* one POST; returns the parsed JSON body, or NULL with *err (*http_status > 0 for a provider HTTP error) */
static json_t *post(const ai_model_t *m, const char *url, const char *key, const char *ctype, const char *body, size_t len,
                    bool google, int *http_status, char **err) {
    http_headers_t h = {0};
    model_headers(m, &h);
    if (google) {
        http_headers_set(&h, "Content-Type", "application/json");
        http_headers_set(&h, "x-goog-api-key", key);
    } else {
        char *auth = xasprintf("Bearer %s", key);
        http_headers_set(&h, "Authorization", auth);
        free(auth);
        if (ctype) http_headers_set(&h, "Content-Type", ctype);
        http_headers_set(&h, "User-Agent", OMC_NAME "/" OMC_VERSION);
    }
    http_req_t rq = {.method = "POST", .url = url, .headers = &h, .body = body, .body_len = len, .timeout_ms = IMAGE_TIMEOUT_MS};
    http_resp_t rs = {0};
    json_t *out = NULL;
    *http_status = 0;
    if (http_request(&rq, &rs) != 0 || rs.error) {
        *err = xstrdup(rs.error ? rs.error : "image request failed");
    } else if (rs.status < 200 || rs.status >= 300) {
        char *msg = error_message(rs.body);
        *err = xasprintf("%s/%s image request failed (%d): %s", m->provider, m->id, rs.status, msg);
        free(msg);
        *http_status = rs.status;
    } else {
        out = json_parse(rs.body ? rs.body : "", rs.body_len, NULL);
        if (!out) *err = xstrdup(google ? "Gemini image API returned malformed JSON" : "Image API returned malformed JSON");
    }
    http_resp_free(&rs);
    http_headers_free(&h);
    return out;
}

static void push_image(gen_result_t *r, const char *data, const char *mime) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 4;
        r->v = xrealloc(r->v, r->cap * sizeof(*r->v));
    }
    r->v[r->n++] = (gen_image_t){xstrdup(data), xstrdup(mime), NULL, NULL};
}

static char *sniff_b64(const char *b64) {
    char head[65];
    snprintf(head, sizeof head, "%s", b64);
    size_t n = 0;
    unsigned char *raw = base64_decode(head, strlen(head) & ~(size_t)3, &n);
    img_meta_t meta;
    const char *mime = raw && img_sniff(raw, n, &meta) ? img_fmt_mime(meta.fmt) : NULL;
    free(raw);
    return xstrdup(mime ? mime : "image/png");
}

/* decodeImageResponse: {data: [{b64_json|url, media_type?}], usage?} */
static int decode_images(json_t *v, gen_result_t *r, int *http_status, char **err) {
    if (!json_is_obj(v)) {
        *err = xstrdup("Image API returned a malformed response");
        return -1;
    }
    const json_t *data = json_get(v, "data"), *it;
    if (!json_is_arr(data)) {
        *err = xstrdup("Image API response is missing data");
        return -1;
    }
    json_arr_foreach(data, i, it) {
        const char *b64 = json_get_str(it, "b64_json"), *url = json_get_str(it, "url");
        if (b64 && *b64) {
            const char *mt = json_get_str(it, "media_type");
            char *mime = mt ? xstrdup(mt) : sniff_b64(b64);
            push_image(r, b64, mime);
            free(mime);
        } else if (url && *url) { /* imageFromUrl */
            http_resp_t rs = {0};
            if (http_get(url, NULL, IMAGE_TIMEOUT_MS, &rs) != 0 || rs.error) {
                *err = xstrdup(rs.error ? rs.error : "Image download failed");
                http_resp_free(&rs);
                return -1;
            }
            if (rs.status < 200 || rs.status >= 300) {
                *err = xasprintf("Image download failed (%d): %s", rs.status, rs.body ? rs.body : "");
                *http_status = rs.status;
                http_resp_free(&rs);
                return -1;
            }
            const char *ct = http_headers_get(&rs.headers, "content-type");
            char *mime = ct ? xstrndup(ct, strcspn(ct, ";")) : NULL;
            if (!mime || !str_starts(mime, "image/")) {
                *err = xasprintf("Image URL returned unsupported content type: %s", mime ? mime : "missing");
                free(mime);
                http_resp_free(&rs);
                return -1;
            }
            char *b = base64_encode(rs.body, rs.body_len);
            push_image(r, b, mime);
            free(b), free(mime);
            http_resp_free(&rs);
        }
    }
    const json_t *u = json_get_obj(v, "usage");
    r->input_tokens = json_get_int(u, "input_tokens", 0) ? json_get_int(u, "input_tokens", 0) : json_get_int(u, "prompt_tokens", 0);
    r->output_tokens =
        json_get_int(u, "output_tokens", 0) ? json_get_int(u, "output_tokens", 0) : json_get_int(u, "completion_tokens", 0);
    r->cost = json_get_num(u, "cost", 0);
    return 0;
}

/* resolveOpenAIImageSize */
static const char *openai_size(const char *aspect, const char *size) {
    if (size) return size;
    if (!aspect) return NULL;
    if (!strcmp(aspect, "1:1")) return "1024x1024";
    if (!strcmp(aspect, "2:3") || !strcmp(aspect, "3:4") || !strcmp(aspect, "9:16")) return "1024x1536";
    if (!strcmp(aspect, "3:2") || !strcmp(aspect, "4:3") || !strcmp(aspect, "16:9")) return "1536x1024";
    return NULL;
}

static char *data_url(const json_t *img) {
    return xasprintf("data:%s;base64,%s", json_get_str(img, "mimeType"), json_get_str(img, "data"));
}

static bool is_gpt_image(const ai_model_t *m) {
    const json_t *id = json_get_obj(m->raw, "identity");
    if (id) return str_eq(json_get_str(id, "family"), "gpt-image");
    return str_starts(m->id, "gpt-image");
}

static const char *wire_id(const ai_model_t *m) {
    const char *r = json_get_str(m->raw, "requestModelId");
    return r ? r : m->id;
}

static int gen_openai(const ai_model_t *m, const char *key, const char *prompt, const json_t *inputs, const char *aspect,
                      const char *size_in, gen_result_t *r, int *st, char **err) {
    const char *size = openai_size(aspect, size_in);
    bool gpt = is_gpt_image(m);
    bool xai = str_eq(m->provider, "xai") || str_eq(m->provider, "xai-oauth");
    json_t *gen = json_obj();
    json_set_str(gen, "model", wire_id(m));
    json_set_str(gen, "prompt", prompt);
    if (xai) {
        json_set_str(gen, "aspect_ratio", aspect ? aspect : "1:1");
        json_set_str(gen, "resolution", !size_in || !strcmp(size_in, "1024x1024") ? "1k" : "2k");
        json_set_int(gen, "n", 1);
        if (!gpt) json_set_str(gen, "response_format", "b64_json");
    } else {
        json_set_int(gen, "n", 1);
        if (!gpt) json_set_str(gen, "response_format", "b64_json");
        if (size) json_set_str(gen, "size", size);
    }
    size_t ninputs = json_len(inputs);
    if (xai && ninputs > XAI_MAX_EDIT_IMAGES) {
        *err = xasprintf("%s image edits accept up to %d reference images; got %zu", m->provider, XAI_MAX_EDIT_IMAGES, ninputs);
        json_free(gen);
        return -1;
    }
    char *base = base_url(m, err);
    if (!base) {
        json_free(gen);
        return -1;
    }
    json_t *resp = NULL;
    if (!ninputs) {
        char *url = xasprintf("%s/images/generations", base);
        char *body = json_dump(gen, 0);
        resp = post(m, url, key, "application/json", body, strlen(body), false, st, err);
        free(body), free(url);
    } else {
        json_t *edit = json_clone(gen);
        json_t *refs = json_arr(), *img;
        json_arr_foreach(inputs, i, img) {
            json_t *ref = json_obj();
            json_set_str(ref, "type", "image_url");
            json_set(ref, "url", json_str_own(data_url(img)));
            json_push(refs, ref);
        }
        if (!xai) json_set(edit, "input_references", refs);
        else if (json_len(refs) == 1) {
            json_set(edit, "image", json_clone(json_at(refs, 0)));
            json_free(refs);
        } else json_set(edit, "images", refs);
        char *url = xasprintf("%s/images/edits", base);
        if (str_eq(m->provider, "openai")) { /* multipart edit with raw bytes */
            char boundary[48];
            char *sf = snowflake_next();
            snprintf(boundary, sizeof boundary, "----omcformdata%s", sf);
            free(sf);
            sb_t b = SB_INIT;
#define FIELD(n_, v_) sb_appendf(&b, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", boundary, n_, v_)
            FIELD("model", wire_id(m));
            FIELD("prompt", prompt);
            FIELD("n", "1");
            if (!gpt) FIELD("response_format", "b64_json");
            if (size) FIELD("size", size);
#undef FIELD
            json_arr_foreach(inputs, i, img) {
                size_t n = 0;
                const char *d = json_get_str(img, "data");
                unsigned char *raw = base64_decode(d, strlen(d), &n);
                sb_appendf(&b, "--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"image\"\r\nContent-Type: %s\r\n\r\n",
                           boundary, json_get_str(img, "mimeType"));
                sb_appendn(&b, (const char *)raw, n);
                sb_append(&b, "\r\n");
                free(raw);
            }
            sb_appendf(&b, "--%s--\r\n", boundary);
            char *ct = xasprintf("multipart/form-data; boundary=%s", boundary);
            resp = post(m, url, key, ct, b.s, b.len, false, st, err);
            free(ct);
            sb_free(&b);
        } else {
            char *body = json_dump(edit, 0);
            resp = post(m, url, key, "application/json", body, strlen(body), false, st, err);
            free(body);
        }
        if (!resp && *st == 404) { /* edits endpoint missing: the generations route takes the references */
            free(*err);
            *err = NULL;
            *st = 0;
            char *gu = xasprintf("%s/images/generations", base);
            char *body = json_dump(edit, 0);
            resp = post(m, gu, key, "application/json", body, strlen(body), false, st, err);
            free(body), free(gu);
        }
        free(url);
        json_free(edit);
    }
    free(base);
    json_free(gen);
    if (!resp) return -1;
    int rc = decode_images(resp, r, st, err);
    json_free(resp);
    return rc;
}

static int gen_openrouter(const ai_model_t *m, const char *key, const char *prompt, const json_t *inputs, const char *aspect,
                          const char *size, gen_result_t *r, int *st, char **err) {
    json_t *body = json_obj(), *img;
    json_set_str(body, "model", wire_id(m));
    json_set_str(body, "prompt", prompt);
    json_set_int(body, "n", 1);
    json_set_str(body, "response_format", "b64_json");
    if (aspect) json_set_str(body, "aspect_ratio", aspect);
    if (size) json_set_str(body, "image_size", size);
    if (json_len(inputs)) {
        json_t *refs = json_arr();
        json_arr_foreach(inputs, i, img) {
            json_t *ref = json_obj(), *iu = json_obj();
            json_set_str(ref, "type", "image_url");
            json_set(iu, "url", json_str_own(data_url(img)));
            json_set(ref, "image_url", iu);
            json_push(refs, ref);
        }
        json_set(body, "input_references", refs);
    }
    char *base = base_url(m, err);
    if (!base) {
        json_free(body);
        return -1;
    }
    char *url = xasprintf("%s/images", base);
    char *s = json_dump(body, 0);
    json_t *resp = post(m, url, key, "application/json", s, strlen(s), false, st, err);
    free(s), free(url), free(base);
    json_free(body);
    if (!resp) return -1;
    int rc = decode_images(resp, r, st, err);
    json_free(resp);
    return rc;
}

static int gen_google(const ai_model_t *m, const char *key, const char *prompt, const json_t *inputs, const char *aspect,
                      const char *size, gen_result_t *r, int *st, char **err) {
    json_t *body = json_obj(), *contents = json_arr(), *turn = json_obj(), *parts = json_arr(), *img;
    json_arr_foreach(inputs, i, img) {
        json_t *p = json_obj(), *inl = json_obj();
        json_set_str(inl, "data", json_get_str(img, "data"));
        json_set_str(inl, "mimeType", json_get_str(img, "mimeType"));
        json_set(p, "inlineData", inl);
        json_push(parts, p);
    }
    json_t *tp = json_obj();
    json_set_str(tp, "text", prompt);
    json_push(parts, tp);
    json_set_str(turn, "role", "user");
    json_set(turn, "parts", parts);
    json_push(contents, turn);
    json_set(body, "contents", contents);
    json_t *gc = json_obj(), *mods = json_arr();
    json_push_str(mods, "IMAGE");
    json_set(gc, "responseModalities", mods);
    if (aspect || size) {
        json_t *ic = json_obj();
        if (aspect) json_set_str(ic, "aspectRatio", aspect);
        if (size) json_set_str(ic, "imageSize", size);
        json_set(gc, "imageConfig", ic);
    }
    json_set_int(gc, "candidateCount", 1);
    json_set(body, "generationConfig", gc);
    char *base = base_url(m, err);
    if (!base) {
        json_free(body);
        return -1;
    }
    char *enc = url_encode(wire_id(m));
    char *url = xasprintf("%s/models/%s:generateContent", base, enc);
    char *s = json_dump(body, 0);
    json_t *resp = post(m, url, key, NULL, s, strlen(s), true, st, err);
    free(s), free(url), free(enc), free(base);
    json_free(body);
    if (!resp) return -1;
    /* geminiImageResult */
    sb_t text = SB_INIT;
    const json_t *c, *p;
    json_arr_foreach(json_get_arr(resp, "candidates"), i, c) {
        bool first = true;
        json_arr_foreach(json_get_arr(json_get_obj(c, "content"), "parts"), k, p) {
            const char *t = json_get_str(p, "text");
            if (t && *t) {
                if (!first) sb_putc(&text, '\n');
                sb_append(&text, t);
                first = false;
            }
            const json_t *inl = json_get_obj(p, "inlineData");
            if (json_get_str(inl, "data") && *json_get_str(inl, "data") && json_get_str(inl, "mimeType"))
                push_image(r, json_get_str(inl, "data"), json_get_str(inl, "mimeType"));
        }
    }
    char *t = str_trim(sb_cstr(&text));
    sb_free(&text);
    if (*t) r->text = t;
    else free(t);
    const json_t *u = json_get_obj(resp, "usageMetadata");
    r->input_tokens = json_get_int(u, "promptTokenCount", 0);
    r->output_tokens = json_get_int(u, "candidatesTokenCount", 0);
    json_free(resp);
    return 0;
}

/* openai-hosted.ts generateHostedImage over an openai-responses carrier (non-streaming JSON) */
static int gen_hosted(const ai_model_t *m, const ai_model_t *carrier, const char *key, const char *session_id,
                      const char *prompt, const json_t *inputs, const char *aspect, const char *size_in, gen_result_t *r,
                      int *st, char **err) {
    json_t *content = json_arr(), *img;
    json_t *tp = json_obj();
    json_set_str(tp, "type", "input_text");
    json_set_str(tp, "text", prompt);
    json_push(content, tp);
    json_arr_foreach(inputs, i, img) {
        json_t *ii = json_obj();
        json_set_str(ii, "type", "input_image");
        json_set_str(ii, "detail", "auto");
        json_set(ii, "image_url", json_str_own(data_url(img)));
        json_push(content, ii);
    }
    const char *size = openai_size(aspect, size_in);
    json_t *tool = json_obj();
    json_set_str(tool, "type", "image_generation");
    json_set_str(tool, "action", json_len(content) > 1 ? "edit" : "generate");
    json_set_str(tool, "output_format", "webp");
    if (size) json_set_str(tool, "size", size);
    if (str_eq(m->api, "openai-responses") && str_eq(json_get_str(m->raw, "kind"), "image")) json_set_str(tool, "model", wire_id(m));
    json_t *body = json_obj(), *input = json_arr(), *turn = json_obj(), *tools = json_arr(), *tc = json_obj();
    json_set_str(body, "model", wire_id(carrier));
    json_set_str(turn, "role", "user");
    json_set(turn, "content", content);
    json_push(input, turn);
    json_set(body, "input", input);
    json_push(tools, tool);
    json_set(body, "tools", tools);
    json_set_str(tc, "type", "image_generation");
    json_set(body, "tool_choice", tc);
    json_set_bool(body, "store", false);
    char *base = xstrdup(carrier->base_url && *carrier->base_url ? carrier->base_url : "https://api.openai.com/v1");
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') base[--bl] = 0;
    char *url = xasprintf("%s/responses", base);
    free(base);
    http_headers_t h = {0};
    model_headers(carrier, &h);
    http_headers_set(&h, "Content-Type", "application/json");
    char *auth = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Authorization", auth);
    free(auth);
    const char *sh = model_compat_str(carrier, "sessionHeader");
    if (sh && session_id && !http_headers_get(&h, sh)) http_headers_set(&h, sh, session_id);
    char *s = json_dump(body, 0);
    json_free(body);
    http_req_t rq = {.method = "POST", .url = url, .headers = &h, .body = s, .body_len = strlen(s), .timeout_ms = IMAGE_TIMEOUT_MS};
    http_resp_t rs = {0};
    int rc = -1;
    if (http_request(&rq, &rs) != 0 || rs.error) *err = xstrdup(rs.error ? rs.error : "OpenAI image request failed");
    else if (rs.status < 200 || rs.status >= 300) {
        char *msg = error_message(rs.body);
        *err = xasprintf("OpenAI image request failed (%d): %s", rs.status, msg);
        free(msg);
        *st = rs.status;
    } else {
        json_t *v = json_parse(rs.body ? rs.body : "", rs.body_len, NULL);
        if (json_get_obj(v, "error"))
            *err = xstrdup(json_get_str(json_get_obj(v, "error"), "message") ? json_get_str(json_get_obj(v, "error"), "message")
                                                                             : "OpenAI image request failed");
        else if (!json_is_arr(json_get(v, "output"))) *err = xstrdup("OpenAI image request returned a malformed response");
        else { /* collectResponse */
            sb_t text = SB_INIT;
            const json_t *o, *p;
            json_arr_foreach(json_get(v, "output"), i, o) {
                const char *ty = json_get_str(o, "type");
                if (str_eq(ty, "image_generation_call") && json_get_str(o, "result") && *json_get_str(o, "result")) {
                    const char *res = json_get_str(o, "result");
                    char head[65];
                    snprintf(head, sizeof head, "%s", res);
                    size_t n = 0;
                    unsigned char *raw = base64_decode(head, strlen(head) & ~(size_t)3, &n);
                    img_meta_t meta;
                    const char *mime = raw && img_sniff(raw, n, &meta) && img_fmt_mime(meta.fmt) ? img_fmt_mime(meta.fmt) : "image/webp";
                    free(raw);
                    push_image(r, res, mime);
                    if (json_get_str(o, "size")) r->v[r->n - 1].size = xstrdup(json_get_str(o, "size"));
                    if (json_get_str(o, "quality")) r->v[r->n - 1].quality = xstrdup(json_get_str(o, "quality"));
                }
                if (str_eq(ty, "message")) json_arr_foreach(json_get_arr(o, "content"), k, p) {
                        const char *pt = json_get_str(p, "type");
                        const char *t = str_eq(pt, "output_text") ? json_get_str(p, "text") : str_eq(pt, "refusal") ? json_get_str(p, "refusal") : NULL;
                        if (t && *t) sb_appendf(&text, "%s%s", text.len ? "\n" : "", t);
                    }
            }
            char *t = str_trim(sb_cstr(&text));
            sb_free(&text);
            if (*t) r->text = t;
            else free(t);
            const json_t *tl;
            json_arr_foreach(json_get_arr(v, "tools"), i, tl) if (!r->model && str_eq(json_get_str(tl, "type"), "image_generation") &&
                                                                  json_get_str(tl, "model")) r->model = xstrdup(json_get_str(tl, "model"));
            const json_t *u = json_get_obj(v, "usage");
            r->input_tokens = json_get_int(u, "input_tokens", 0);
            r->output_tokens = json_get_int(u, "output_tokens", 0);
            rc = 0;
        }
        json_free(v);
    }
    http_resp_free(&rs);
    http_headers_free(&h);
    free(s);
    free(url);
    return rc;
}

/* resolveHostedImageCarrier: the session model when it carries the hosted tool on the same provider, else the
 * cheapest available hosted-image model there */
static const ai_model_t *hosted_carrier(const ai_model_t *selected, const ai_model_t *active) {
    if (active && str_eq(active->provider, selected->provider) && json_get_bool(active->raw, "hostedImage", false)) return active;
    mr_pool_t avail = {0};
    mr_pool_available(&avail, "all");
    const ai_model_t *best = NULL;
    for (size_t i = 0; i < avail.n; i++) {
        const ai_model_t *m = avail.v[i];
        if (!str_eq(m->provider, selected->provider) || !json_get_bool(m->raw, "hostedImage", false)) continue;
        if (!best || m->cost_input < best->cost_input) best = m;
    }
    mr_pool_free(&avail);
    return best;
}

/* ---------- candidates ---------- */

static bool supported_api(const char *api) {
    return str_eq(api, "openai-images") || str_eq(api, "openrouter-images") || str_eq(api, "google-generative-ai") ||
           str_eq(api, "openai-responses");
}

static bool known_image_api(const char *api) { /* IMAGE_GENERATION_APIS */
    return supported_api(api) || str_eq(api, "google-gemini-cli") || str_eq(api, "openai-responses") ||
           str_eq(api, "openai-codex-responses");
}

typedef VEC(const ai_model_t *) model_vec_t;

static void push_unique(model_vec_t *v, const ai_model_t *m) {
    if (!m) return;
    for (size_t i = 0; i < v->n; i++)
        if (str_eq(v->v[i]->provider, m->provider) && str_eq(v->v[i]->id, m->id)) return;
    vec_push(v, m);
}

static int ig_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    app_t *a = self->ud;
    mr_pool_t pool = {0};
    mr_pool_role(&pool, "image");
    model_vec_t cands = {0};
    const char *sel = json_get_str(args, "model");
    if (sel) {
        const ai_model_t *m = mr_resolve_string(sel, &pool);
        if (!m) {
            tool_result_error(out, "Image model selector did not match an available image model: %s", sel);
            mr_pool_free(&pool);
            return 0;
        }
        vec_push(&cands, m);
    } else { /* defaultImageCandidates */
        mr_chain_t chain = {0};
        mr_role_chain("image", &pool, &chain);
        for (size_t i = 0; i < chain.n; i++)
            if (chain.v[i].explicit_) push_unique(&cands, chain.v[i].model);
        const ai_model_t *sm = a->model;
        const char *im = sm ? json_get_str(sm->raw, "imageModel") : NULL;
        if (im) {
            const ai_model_t *t = strchr(im, '/') ? mr_resolve_string(im, &pool) : NULL;
            if (!t) {
                char *q = xasprintf("%s/%s", sm->provider, im);
                t = mr_resolve_string(q, &pool);
                free(q);
            }
            push_unique(&cands, t);
        }
        if (sm && json_get_bool(sm->raw, "hostedImage", false)) push_unique(&cands, sm);
        for (size_t i = 0; i < chain.n; i++)
            if (!chain.v[i].explicit_) push_unique(&cands, chain.v[i].model);
        mr_chain_free(&chain);
    }
    char *prompt = assemble_prompt(args);
    json_t *inputs = NULL;
    sb_t skipped = SB_INIT, failures = SB_INIT;
    gen_result_t res = {0};
    const ai_model_t *ran = NULL;
    char *fatal = NULL;
    for (size_t i = 0; i < cands.n && !ran && !fatal; i++) {
        const ai_model_t *m = cands.v[i];
        if (!known_image_api(m->api)) {
            sb_appendf(&skipped, "%s%s/%s (unsupported %s)", skipped.len ? ", " : "", m->provider, m->id, m->api);
            continue;
        }
        char *key = app_api_key_for(m);
        if (!key || !*key) {
            sb_appendf(&skipped, "%s%s/%s (credentials unavailable)", skipped.len ? ", " : "", m->provider, m->id);
            free(key);
            continue;
        }
        if (!supported_api(m->api)) { /* Codex-hosted / Antigravity transports are not ported */
            sb_appendf(&skipped, "%s%s/%s (unsupported %s)", skipped.len ? ", " : "", m->provider, m->id, m->api);
            free(key);
            continue;
        }
        const ai_model_t *carrier = NULL;
        if (str_eq(m->api, "openai-responses")) {
            carrier = json_get_bool(m->raw, "hostedImage", false) ? m : hosted_carrier(m, a->model);
            if (!carrier) {
                sb_appendf(&skipped, "%s%s/%s (hosted chat carrier unavailable)", skipped.len ? ", " : "", m->provider, m->id);
                free(key);
                continue;
            }
            char *ck = app_api_key_for(carrier);
            if (!ck || !*ck) {
                sb_appendf(&skipped, "%s%s/%s (carrier credentials unavailable)", skipped.len ? ", " : "", m->provider, m->id);
                free(ck);
                free(key);
                continue;
            }
            free(key);
            key = ck;
        }
        if (!inputs) {
            inputs = json_arr();
            const json_t *in;
            json_arr_foreach(json_get_arr(args, "input"), k, in) {
                char *e = NULL;
                json_t *img = resolve_input(in, a->cwd, &e);
                if (!img) {
                    fatal = e;
                    break;
                }
                json_push(inputs, img);
            }
            if (fatal) {
                free(key);
                break;
            }
        }
        const char *aspect = json_get_str(args, "aspect_ratio"), *size = json_get_str(args, "image_size");
        int st = 0;
        char *err = NULL;
        int rc = carrier ? gen_hosted(m, carrier, key, a->agent ? a->agent->session_id : NULL, prompt, inputs, aspect, size, &res, &st, &err)
                 : str_eq(m->api, "openai-images")     ? gen_openai(m, key, prompt, inputs, aspect, size, &res, &st, &err)
                 : str_eq(m->api, "openrouter-images") ? gen_openrouter(m, key, prompt, inputs, aspect, size, &res, &st, &err)
                                                       : gen_google(m, key, prompt, inputs, aspect, size, &res, &st, &err);
        free(key);
        if (rc == 0) ran = m;
        else if (st > 0) { /* ProviderHttpError: try the next candidate */
            char *reason = failure_reason(err);
            sb_appendf(&failures, "%s%s", failures.len ? "; " : "", reason);
            free(reason);
            free(err);
            gen_result_free(&res);
        } else fatal = err;
        if (ctx->cancel && *ctx->cancel && !ran) {
            free(fatal);
            fatal = xstrdup("Operation aborted");
        }
    }
    if (fatal) tool_result_error(out, "%s", fatal);
    else if (!ran) {
        sb_t att = SB_INIT;
        for (size_t i = 0; i < cands.n; i++) sb_appendf(&att, "%s%s/%s", i ? ", " : "", cands.v[i]->provider, cands.v[i]->id);
        tool_result_error(out, "Image generation exhausted the resolved image chain%s%s.%s%s%s%s%s%s", att.len ? ": " : "",
                          sb_cstr(&att), failures.len ? " Failures: " : "", sb_cstr(&failures), failures.len ? "." : "",
                          skipped.len ? " Skipped: " : "", sb_cstr(&skipped), skipped.len ? "." : "");
        sb_free(&att);
    } else {
        /* buildToolResult: images saved to the temp dir */
        json_t *paths = json_arr(), *images = json_arr();
        const char *tmp = getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
        for (size_t i = 0; i < res.n; i++) {
            const char *mt = res.v[i].mime;
            const char *ext = str_eq(mt, "image/jpeg") ? "jpg" : str_eq(mt, "image/gif") ? "gif" : str_eq(mt, "image/webp") ? "webp" : "png";
            char *sf = snowflake_next();
            char *p = xasprintf("%s/omc-image-%s.%s", tmp, sf, ext);
            free(sf);
            size_t n = 0;
            unsigned char *raw = base64_decode(res.v[i].data, strlen(res.v[i].data), &n);
            if (raw) fs_write_atomic(p, raw, n, 0644);
            free(raw);
            json_push_str(paths, p);
            json_t *gi = json_obj();
            json_set_str(gi, "data", res.v[i].data);
            json_set_str(gi, "mimeType", mt);
            json_push(images, gi);
            free(p);
        }
        const char *model_ran = res.model ? res.model : ran->id;
        sb_t t = SB_INIT;
        if (!res.n) sb_appendf(&t, "No image data returned.%s%s", res.text ? "\n\n" : "", res.text ? res.text : "");
        else {
            sb_appendf(&t, "Provider: %s\n", ran->provider);
            if (str_eq(model_ran, ran->id)) sb_appendf(&t, "Model: %s\n", model_ran);
            else sb_appendf(&t, "Model: %s (catalog entry %s/%s)\n", model_ran, ran->provider, ran->id);
            sb_appendf(&t, "Generated %zu image(s):", res.n);
            const json_t *p;
            json_arr_foreach(paths, k, p) {
                const gen_image_t *gi = &res.v[k];
                sb_appendf(&t, "\n  %s", json_sv(p));
                if (gi->size || gi->quality)
                    sb_appendf(&t, " (%s%s%s%s)", gi->size ? gi->size : "", gi->size && gi->quality ? ", " : "",
                               gi->quality ? "quality " : "", gi->quality ? gi->quality : "");
            }
            if (res.text) {
                char *tt = str_trim(res.text);
                sb_appendf(&t, "\n\n%s", tt);
                free(tt);
            }
        }
        tool_result_text(out, sb_cstr(&t));
        sb_free(&t);
        json_t *d = json_obj();
        json_set_str(d, "provider", ran->provider);
        json_set_str(d, "model", model_ran);
        json_set_int(d, "imageCount", (int64_t)res.n);
        json_set(d, "imagePaths", paths);
        json_set(d, "images", images);
        if (res.text) json_set_str(d, "responseText", res.text);
        json_t *u = json_obj();
        json_set_int(u, "input", res.input_tokens);
        json_set_int(u, "output", res.output_tokens);
        json_set_int(u, "cacheRead", 0);
        json_set_int(u, "cacheWrite", 0);
        json_set_int(u, "totalTokens", res.input_tokens + res.output_tokens);
        json_t *cost = json_obj();
        json_set_int(cost, "input", 0), json_set_int(cost, "output", 0), json_set_int(cost, "cacheRead", 0);
        json_set_int(cost, "cacheWrite", 0);
        json_set_num(cost, "total", res.cost);
        json_set(u, "cost", cost);
        json_set(d, "usage", u);
        out->details = d;
    }
    gen_result_free(&res);
    free(fatal);
    json_free(inputs);
    free(prompt);
    sb_free(&skipped);
    sb_free(&failures);
    free(cands.v);
    mr_pool_free(&pool);
    return 0;
}

agent_tool_t *imagegen_tool_new(app_t *a) {
    char *desc = prompt_render("coding-agent/prompts/tools/image-gen.md", NULL);
    char *d = str_trim(desc);
    free(desc);
    agent_tool_t *t = agent_tool_new("generate_image", "GenerateImage", d, schema_lit(SCHEMA), ig_execute);
    free(d);
    t->ud = a;
    t->exclusive = true; /* approval "write" */
    t->discoverable = true; /* custom tools mount under xd:// */
    return t;
}
