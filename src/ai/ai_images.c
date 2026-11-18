/* ai_images.c - model-aware image policy at the provider boundary. See ai_images.h. */
#include "ai/ai_images.h"
#include "img/imgpipe.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <strings.h>

/* ---------- model policy ---------- */

static const char *raw_str(const ai_model_t *m, const char *key) { return m && m->raw ? json_get_str(m->raw, key) : NULL; }

bool ai_model_lacks_webp(const ai_model_t *m) {
    if (!m) return false;
    const char *dec = raw_str(m, "imageInputDecoder");
    if (str_eq(dec, "stb")) return true;
    static const char *const prov[] = {"ollama", "ollama-cloud", "llama.cpp", "lm-studio", "local-server"};
    for (size_t i = 0; i < sizeof prov / sizeof *prov; i++)
        if (str_eq(m->provider, prov[i])) return true;
    return str_eq(m->api, "ollama-chat");
}

bool ai_is_claude_model(const char *id) { return id && str_istr(id, "claude") != NULL; }

/* "claude-opus-4-6" -> 4.6, "claude-3-7-sonnet-2025..." -> 3.7, "claude-sonnet-4" -> 4, "claude-opus-45" -> 45 */
double ai_claude_revision(const char *id) {
    if (!id) return 0;
    const char *p = str_istr(id, "claude");
    p = p ? p + 6 : id;
    while (*p) {
        if (isdigit((unsigned char)*p) && (p == id || !isalnum((unsigned char)p[-1]))) {
            const char *q = p;
            long major = 0;
            int nd = 0;
            while (isdigit((unsigned char)*q) && nd < 9) major = major * 10 + (*q++ - '0'), nd++;
            if (nd >= 6) return 0; /* a date stamp, not a revision */
            double rev = (double)major;
            if ((*q == '-' || *q == '.') && isdigit((unsigned char)q[1])) {
                const char *r = q + 1;
                int md = 0;
                long minor = 0;
                while (isdigit((unsigned char)r[md]) && md < 9) minor = minor * 10 + (r[md] - '0'), md++;
                if (md <= 2) rev += minor / (md == 1 ? 10.0 : 100.0);
            }
            return rev;
        }
        p++;
    }
    return 0;
}

static bool host_is(const char *url, const char *host) {
    if (!url || !*url) return false;
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    size_t n = strlen(host);
    return !strncasecmp(s, host, n) && (s[n] == '\0' || s[n] == '/' || s[n] == ':');
}

bool ai_reaches_budgeted_host(const ai_model_t *m) {
    if (!m) return false;
    if (str_eq(m->provider, "deepseek")) /* catalog-endpoint-budgets: unset baseUrl or the catalog host */
        return !m->base_url || !*m->base_url || host_is(m->base_url, "api.deepseek.com");
    if (str_eq(m->provider, "anthropic") && str_eq(m->api, "anthropic-messages")) {
        const char *env = getenv("ANTHROPIC_BASE_URL");
        const char *url = env && *env ? env : m->base_url;
        return !url || !*url || host_is(url, "api.anthropic.com");
    }
    return model_compat_bool(m, "officialEndpoint", false);
}

int ai_image_count_budget(const ai_model_t *m) {
    if (!m) return 5;
    json_t *own = m->raw ? json_get(m->raw, "imageCountBudget") : NULL;
    if (json_is_num(own) && json_get_num(m->raw, "imageCountBudget", 0) >= 1) return (int)json_d2i(json_get_num(m->raw, "imageCountBudget", 0));
    const char *p = m->provider ? m->provider : "";
    bool official = ai_reaches_budgeted_host(m);
    bool claude = ai_is_claude_model(m->id);
    if (!strcmp(p, "anthropic")) {
        if (!official) return 90;
        double rev = ai_claude_revision(m->id);
        return (rev > 0 && rev < 4.6) || (rev >= 10 && rev < 46) ? 100 : 600;
    }
    if (!strcmp(p, "openai")) return 200;
    if (!strcmp(p, "openai-codex")) return official ? 2000 : 200;
    if (!strcmp(p, "google") || !strcmp(p, "google-vertex") || !strcmp(p, "google-gemini-cli")) return 200;
    if (!strcmp(p, "google-antigravity")) return claude ? 20 : 60;
    if (!strcmp(p, "amazon-bedrock")) return 90;
    if (!strcmp(p, "cursor")) return str_eq(m->id, "default") || claude ? 20 : 60;
    if (!strcmp(p, "openrouter")) return 90;
    if (!strcmp(p, "xai") || !strcmp(p, "xai-oauth")) return 60;
    if (!strcmp(p, "umans")) return 10;
    if (!strcmp(p, "deepseek") && official) return 600;
    return 5;
}

int64_t ai_inline_image_byte_budget(const ai_model_t *m) {
    if (!m || !ai_reaches_budgeted_host(m)) return -1;
    if (str_eq(m->provider, "anthropic") && str_eq(m->api, "anthropic-messages")) return 120000000;
    if (str_eq(m->provider, "openai-codex")) return 24000000;
    if (str_eq(m->provider, "deepseek")) return 41943040;
    if (str_eq(m->provider, "factory-droid")) return 3500000;
    return -1;
}

int64_t ai_request_body_byte_limit(const ai_model_t *m) {
    if (!m || !ai_reaches_budgeted_host(m)) return -1;
    if (str_eq(m->provider, "anthropic") && str_eq(m->api, "anthropic-messages")) return 128000000;
    if (str_eq(m->provider, "factory-droid")) return 4500000;
    return -1;
}

/* ---------- helpers ---------- */

static bool is_image(const json_t *b) { return str_eq(json_get_str(b, "type"), "image"); }

/* content array of a user / developer / toolResult message (the roles whose images are inputs) */
static json_t *input_content(json_t *msg) {
    const char *role = json_get_str(msg, "role");
    if (!str_eq(role, "user") && !str_eq(role, "developer") && !str_eq(role, "toolResult")) return NULL;
    return json_get_arr(msg, "content");
}

static void replace_at(json_t *arr, size_t i, json_t *v) {
    json_free(arr->u.arr.items[i]);
    arr->u.arr.items[i] = v;
}

static json_t *text_block(const char *t) {
    json_t *b = json_obj();
    json_set_str(b, "type", "text");
    json_set_str(b, "text", t);
    return b;
}

/* inline base64 bytes this block puts on the wire (omc never sends image references) */
static size_t inline_bytes(const json_t *img) {
    const char *d = json_get_str(img, "data");
    return d ? strlen(d) : 0;
}

static bool is_webp_block(const json_t *img) {
    const char *mime = json_get_str(img, "mimeType"), *d = json_get_str(img, "data");
    if (mime && !strcasecmp(mime, "image/webp")) return true;
    if (!d) return false;
    size_t n = 0;
    size_t take = MIN(strlen(d), (size_t)16);
    unsigned char *h = base64_decode(d, take - take % 4, &n);
    bool w = h && n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WEBP", 4);
    free(h);
    return w;
}

/* ---------- normalizeModelContextImages ---------- */

#define NORM_SLOTS 128
typedef struct {
    uint64_t hash;
    size_t len;
    int excl;
    bool used;
    char *data, *mime; /* data NULL = undecodable (STB WebP: drop) */
} norm_t;
static norm_t g_norm[NORM_SLOTS];
static pthread_mutex_t g_norm_mu = PTHREAD_MUTEX_INITIALIZER;

/* resizeImage on a base64 block; returns 0 with owned data and mime, -1 when it throws */
static int resize_block(const char *b64, const char *mime, int exclude, char **data, char **out_mime) {
    size_t n = 0;
    unsigned char *bytes = base64_decode(b64, strlen(b64), &n);
    if (!bytes) return -1;
    img_resize_opts_t o = {.exclude_webp = exclude};
    img_resized_t r;
    char *err = NULL;
    int rc = img_resize_image(bytes, n, mime, &o, &r, &err);
    free(err);
    if (rc == 0) {
        *data = (r.len == n && !memcmp(r.buf, bytes, n)) ? xstrdup(b64) : base64_encode(r.buf, r.len);
        *out_mime = xstrdup(r.mime);
        img_resized_free(&r);
    }
    free(bytes);
    return rc;
}

/* memoized STB normalization (WebP -> PNG/JPEG); -1 when undecodable or still WebP */
static int stb_normalize(const char *b64, const char *mime, char **data, char **out_mime) {
    size_t len = strlen(b64);
    uint64_t h = img_hash(b64, len);
    norm_t *s = &g_norm[h % NORM_SLOTS];
    pthread_mutex_lock(&g_norm_mu);
    if (s->used && s->hash == h && s->len == len && s->excl == 1) {
        int rc = s->data ? 0 : -1;
        if (s->data) *data = xstrdup(s->data), *out_mime = xstrdup(s->mime);
        pthread_mutex_unlock(&g_norm_mu);
        return rc;
    }
    pthread_mutex_unlock(&g_norm_mu);
    char *d = NULL, *mm = NULL;
    int rc = resize_block(b64, mime, 1, &d, &mm);
    if (rc == 0 && str_ieq(mm, "image/webp")) rc = -1;
    if (rc != 0) {
        free(d), free(mm);
        d = mm = NULL;
    }
    pthread_mutex_lock(&g_norm_mu);
    free(s->data), free(s->mime);
    s->used = true, s->hash = h, s->len = len, s->excl = 1;
    s->data = d ? xstrdup(d) : NULL;
    s->mime = mm ? xstrdup(mm) : NULL;
    pthread_mutex_unlock(&g_norm_mu);
    if (rc == 0) *data = d, *out_mime = mm;
    return rc;
}

static json_t *normalized_block(const json_t *img, const ai_model_t *m) {
    const char *b64 = json_get_str(img, "data"), *mime = json_get_str(img, "mimeType");
    if (!b64) return json_clone(img);
    bool stb = ai_model_lacks_webp(m);
    char *d = NULL, *mm = NULL;
    int rc = stb && is_webp_block(img) ? stb_normalize(b64, mime, &d, &mm) : resize_block(b64, mime, stb ? 1 : 0, &d, &mm);
    json_t *o = json_clone(img);
    if (rc == 0) {
        json_set(o, "data", json_str_own(d));
        json_set(o, "mimeType", json_str_own(mm));
    }
    return o;
}

json_t *ai_normalize_images(const json_t *images, const ai_model_t *m) {
    if (!json_is_arr(images) || !json_len(images)) return NULL;
    json_t *out = json_arr();
    json_t *img;
    json_arr_foreach(images, i, img) json_push(out, is_image(img) ? normalized_block(img, m) : json_clone(img));
    return out;
}

void ai_normalize_message_images(json_t *message, const ai_model_t *m) {
    json_t *c = json_get_arr(message, "content"), *b;
    json_arr_foreach(c, i, b) if (is_image(b)) replace_at(c, i, normalized_block(b, m));
}

/* ---------- provider-boundary chain ---------- */

typedef struct {
    int64_t drops;
    int64_t inline_bytes;
    int64_t budget; /* INT64_MAX = no byte budget */
} clamp_t;

static bool needs_drop(const clamp_t *s) { return s->drops > 0 || s->inline_bytes > s->budget; }

/* clampContent for one message; returns true when changed */
static bool clamp_message(json_t *msg, clamp_t *s) {
    json_t *c = input_content(msg);
    if (!c || !needs_drop(s)) return false;
    bool changed = false;
    for (size_t i = 0; i < json_len(c);) {
        json_t *b = json_at(c, i);
        if (is_image(b) && needs_drop(s)) {
            int64_t bytes = s->budget == INT64_MAX ? 0 : (int64_t)inline_bytes(b);
            if (s->drops > 0 || (s->inline_bytes > s->budget && bytes > 0)) {
                json_free(json_arr_remove(c, i));
                if (s->drops > 0) s->drops--;
                s->inline_bytes -= bytes;
                changed = true;
                continue;
            }
        }
        i++;
    }
    if (changed && json_len(c) == 0) json_push(c, text_block(AI_IMAGE_OMISSION_TEXT));
    return changed;
}

static int count_images(const json_t *messages) {
    int n = 0;
    json_t *msg, *b;
    json_arr_foreach(messages, i, msg) {
        json_t *c = input_content(msg);
        json_arr_foreach(c, k, b) n += is_image(b);
    }
    return n;
}

/* clampProviderContextImages */
static bool clamp_count(json_t *messages, const ai_model_t *m) {
    int total = count_images(messages);
    int64_t drops = (int64_t)total - ai_image_count_budget(m);
    if (drops <= 0) return false;
    clamp_t s = {drops, 0, INT64_MAX};
    bool changed = false;
    json_t *msg;
    json_arr_foreach(messages, i, msg) changed |= clamp_message(msg, &s);
    return changed;
}

/* normalizeModelContextMessages: STB-backed models never receive WebP */
static bool stb_rewrite(json_t *messages, const ai_model_t *m) {
    if (!ai_model_lacks_webp(m)) return false;
    bool changed = false;
    json_t *msg, *b;
    json_arr_foreach(messages, i, msg) {
        json_t *c = json_get_arr(msg, "content");
        json_arr_foreach(c, k, b) {
            if (!is_image(b) || !is_webp_block(b) || !json_get_str(b, "data")) continue;
            char *d = NULL, *mm = NULL;
            if (stb_normalize(json_get_str(b, "data"), json_get_str(b, "mimeType"), &d, &mm) == 0) {
                json_set(b, "data", json_str_own(d));
                json_set(b, "mimeType", json_str_own(mm));
            } else {
                replace_at(c, k, text_block(AI_WEBP_OMISSION_TEXT));
            }
            changed = true;
        }
    }
    return changed;
}

/* dropUnreadableContextImages */
static bool drop_unreadable(json_t *messages) {
    bool changed = false;
    json_t *msg, *b;
    json_arr_foreach(messages, i, msg) {
        json_t *c = input_content(msg);
        json_arr_foreach(c, k, b) {
            if (!is_image(b) || !inline_bytes(b)) continue;
            const char *mime = json_get_str(b, "mimeType");
            char *why = img_decode_failure_reason(json_get_str(b, "data"), mime ? mime : "");
            if (!why) continue;
            char *t = xasprintf("[image omitted: undecodable %s data (%s)]", mime ? mime : "image", why);
            replace_at(c, k, text_block(t));
            free(t), free(why);
            changed = true;
        }
    }
    return changed;
}

static int64_t sent_image_bytes(const json_t *msg) {
    int64_t n = 0;
    json_t *b, *c = input_content((json_t *)msg);
    json_arr_foreach(c, k, b) if (is_image(b)) n += (int64_t)inline_bytes(b);
    return n;
}

/* request bytes around the images: every image's base64 emptied, tool-result details dropped */
static int64_t other_request_bytes(const json_t *messages, const json_t *system, const json_t *tools) {
    json_t *copy = json_clone(messages), *msg, *b;
    json_arr_foreach(copy, i, msg) {
        json_del(msg, "details");
        json_t *c = json_get_arr(msg, "content");
        json_arr_foreach(c, k, b) if (is_image(b)) json_set_str(b, "data", "");
    }
    char *s = json_dump(copy, 0);
    int64_t n = (int64_t)strlen(s);
    free(s);
    json_free(copy);
    if (system) {
        s = json_dump(system, 0);
        n += (int64_t)strlen(s);
        free(s);
    }
    if (tools) {
        s = json_dump(tools, 0);
        n += (int64_t)strlen(s);
        free(s);
    }
    return n;
}

#define REQUEST_ENVELOPE_BYTES 64000

/* clampProviderContextImageBytes */
static bool clamp_bytes(json_t *messages, const ai_model_t *m, const json_t *system, const json_t *tools) {
    int64_t budget = ai_inline_image_byte_budget(m);
    if (budget < 0) return false;
    int64_t inl = 0;
    json_t *msg;
    json_arr_foreach(messages, i, msg) inl += sent_image_bytes(msg);
    if (inl == 0) return false;
    int64_t body = ai_request_body_byte_limit(m);
    clamp_t s = {0, inl, budget};
    bool changed = false;
    if (body < 0) {
        if (inl <= budget) return false;
        json_arr_foreach(messages, i, msg) changed |= clamp_message(msg, &s);
        return changed;
    }
    int64_t room = body - REQUEST_ENVELOPE_BYTES - other_request_bytes(messages, system, tools);
    s.budget = MIN(budget, room);
    if (inl <= s.budget) return false;
    /* keep the newest user message's and newest image-bearing tool result's images while they fit the room */
    ssize_t keep_user = -1, keep_tool = -1;
    int64_t kept = 0;
    bool have_user = false, have_tool = false;
    for (ssize_t i = (ssize_t)json_len(messages) - 1; i >= 0 && !(have_user && have_tool); i--) {
        json_t *mm = json_at(messages, (size_t)i);
        const char *role = json_get_str(mm, "role");
        if (!json_is_arr(json_get(mm, "content"))) continue;
        bool is_user = !have_user && str_eq(role, "user");
        if (!is_user && (have_tool || !str_eq(role, "toolResult"))) continue;
        int64_t bytes = sent_image_bytes(mm);
        if (is_user) have_user = true;
        else if (bytes > 0) have_tool = true;
        else continue;
        if (bytes == 0 || kept + bytes > room || room <= 0) continue;
        kept += bytes;
        if (is_user) keep_user = i;
        else keep_tool = i;
    }
    json_arr_foreach(messages, i, msg) {
        if ((ssize_t)i == keep_user || (ssize_t)i == keep_tool) continue;
        changed |= clamp_message(msg, &s);
    }
    return changed;
}

bool ai_prepare_context_images(json_t *messages, const ai_model_t *m, const json_t *system, const json_t *tools) {
    if (!json_is_arr(messages) || !m) return false;
    bool changed = false;
    if (m->input_image) changed |= clamp_count(messages, m);
    changed |= stb_rewrite(messages, m);
    changed |= drop_unreadable(messages);
    if (m->input_image) changed |= clamp_bytes(messages, m, system, tools);
    return changed;
}

/* ---------- prepareAnthropicImageContext ---------- */

#define ANTHROPIC_MANY_IMAGE_THRESHOLD 20
#define ANTHROPIC_MANY_IMAGE_MAX_DIMENSION 2000
#define ANTHROPIC_MAX_IMAGE_DIMENSION 8000
#define ANTHROPIC_MAX_IMAGE_PAYLOAD_BYTES (10 * 1024 * 1024)
#define ANTHROPIC_IMAGE_RESIZE_ATTEMPTS 4

#define ARES_SLOTS 64
typedef struct {
    uint64_t hash;
    size_t len;
    int maxdim;
    int64_t budget;
    bool used;
    char *data, *mime; /* NULL data = keep the block */
} ares_t;
static ares_t g_ares[ARES_SLOTS];
static pthread_mutex_t g_ares_mu = PTHREAD_MUTEX_INITIALIZER;

/* resizeAnthropicImageBlock; returns 0 with owned replacement, -1 to keep the block */
static int anthropic_resize(const char *b64, int maxdim, int64_t budget, char **data, char **mime) {
    size_t blen = strlen(b64), n = 0;
    unsigned char *bytes = base64_decode(b64, blen, &n);
    if (!bytes) return -1;
    img_meta_t meta;
    int rc = -1;
    img_t src = {0};
    if (!img_sniff(bytes, n, &meta) || meta.w <= 0 || meta.h <= 0) goto out;
    bool over_dim = meta.w > maxdim || meta.h > maxdim, over_budget = (int64_t)blen > budget;
    if (!over_dim && !over_budget) goto out;
    if (img_decode(bytes, n, &src, NULL, NULL) < 0) goto out;
    int w = src.w, h = src.h;
    double scale = MIN(1.0, MIN((double)maxdim / w, (double)maxdim / h));
    uint8_t *best = NULL;
    size_t best_len = 0;
    const char *best_mime = NULL;
    for (int attempt = 0; attempt < ANTHROPIC_IMAGE_RESIZE_ATTEMPTS; attempt++) {
        int tw = (int)MAX(1L, MIN((long)maxdim, (long)floor(w * scale + 0.5)));
        int th = (int)MAX(1L, MIN((long)maxdim, (long)floor(h * scale + 0.5)));
        img_t t = {0};
        if (img_resize(&src, tw, th, &t) < 0) break;
        size_t pl = 0, jl = 0;
        uint8_t *png = img_png_encode(&t, -1, &pl);
        uint8_t *jpg = img_jpeg_encode(&t, 85, &jl);
        img_free(&t);
        free(best);
        if (png && (!jpg || pl <= jl)) best = png, best_len = pl, best_mime = "image/png", free(jpg);
        else if (jpg) best = jpg, best_len = jl, best_mime = "image/jpeg", free(png);
        else best = NULL, best_len = 0;
        if (!best) break;
        int64_t encoded = (int64_t)((best_len + 2) / 3) * 4;
        if (encoded <= budget) break;
        if (tw == 1 && th == 1) break;
        scale *= MAX(0.25, sqrt((double)budget / (double)encoded) * 0.9);
    }
    if (best) {
        char *enc = base64_encode(best, best_len);
        free(best);
        if (!over_dim && strlen(enc) >= blen) {
            free(enc);
        } else {
            *data = enc;
            *mime = xstrdup(best_mime);
            rc = 0;
        }
    }
out:
    img_free(&src);
    free(bytes);
    return rc;
}

static int anthropic_resize_cached(const char *b64, int maxdim, int64_t budget, char **data, char **mime) {
    size_t len = strlen(b64);
    uint64_t h = img_hash(b64, len);
    ares_t *s = &g_ares[h % ARES_SLOTS];
    pthread_mutex_lock(&g_ares_mu);
    if (s->used && s->hash == h && s->len == len && s->maxdim == maxdim && s->budget == budget) {
        int rc = s->data ? 0 : -1;
        if (s->data) *data = xstrdup(s->data), *mime = xstrdup(s->mime);
        pthread_mutex_unlock(&g_ares_mu);
        return rc;
    }
    pthread_mutex_unlock(&g_ares_mu);
    char *d = NULL, *mm = NULL;
    int rc = anthropic_resize(b64, maxdim, budget, &d, &mm);
    pthread_mutex_lock(&g_ares_mu);
    free(s->data), free(s->mime);
    s->used = true, s->hash = h, s->len = len, s->maxdim = maxdim, s->budget = budget;
    s->data = rc == 0 ? xstrdup(d) : NULL;
    s->mime = rc == 0 ? xstrdup(mm) : NULL;
    pthread_mutex_unlock(&g_ares_mu);
    if (rc == 0) *data = d, *mime = mm;
    return rc;
}

bool ai_anthropic_prepare_images(json_t *messages, const ai_model_t *m) {
    if (!m || !m->input_image || !json_is_arr(messages)) return false;
    int count = count_images(messages);
    if (count == 0) return false;
    json_t *cd = model_compat_get(m, "maxImageDimension"), *cp = model_compat_get(m, "maxImagePayloadBytes");
    double cdv = json_is_num(cd) ? cd->u.num.d : 0, cpv = json_is_num(cp) ? cp->u.num.d : 0;
    int host_dim = cdv >= 1 ? (int)cdv : ANTHROPIC_MAX_IMAGE_DIMENSION;
    int64_t max_payload = cpv >= 1 ? (int64_t)cpv
                          : str_eq(m->provider, "google-antigravity") ? 5242880 /* max-image-payload-bytes (Vertex Anthropic) */
                                                                       : ANTHROPIC_MAX_IMAGE_PAYLOAD_BYTES;
    int maxdim = count > ANTHROPIC_MANY_IMAGE_THRESHOLD ? MIN(ANTHROPIC_MANY_IMAGE_MAX_DIMENSION, host_dim) : host_dim;
    int64_t budget = (int64_t)floor((double)max_payload * 0.9);
    bool changed = false;
    json_t *msg, *b;
    json_arr_foreach(messages, i, msg) {
        json_t *c = input_content(msg);
        json_arr_foreach(c, k, b) {
            const char *d = is_image(b) ? json_get_str(b, "data") : NULL;
            if (!d || !*d) continue;
            char *nd = NULL, *nm = NULL;
            if (anthropic_resize_cached(d, maxdim, budget, &nd, &nm) != 0) continue;
            json_t *o = json_obj();
            json_set_str(o, "type", "image");
            json_set(o, "data", json_str_own(nd));
            json_set(o, "mimeType", json_str_own(nm));
            replace_at(c, k, o);
            changed = true;
        }
    }
    return changed;
}
