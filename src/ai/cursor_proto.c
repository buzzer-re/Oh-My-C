/* cursor_proto.c - protobuf schema subset of Cursor's agent.v1 / aiserver.v1 protocol (pb.h DSL), and the
 * Connect envelope helpers used by the Cursor transport (omp connect-frame.ts, connect-error-detail.ts).
 *
 * The schema (cursor_proto_schema.inc) is derived from omp v18.14.2
 * packages/catalog/src/discovery/cursor-proto.ts (MIT; generated there from Cursor's agent.proto and trimmed
 * to the fields omp consumes). Field names keep omp's camelCase spelling and fields keep its declaration
 * order, so encodings are byte-identical to omp's. */
#define _GNU_SOURCE
#include "cursor_proto.h"

#include <pthread.h>

#include "cursor_proto_schema.inc"

static pb_schema_t *g_schema;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void schema_init(void) {
    char *err = NULL;
    g_schema = pb_schema_parse(CURSOR_PROTO_DSL, &err);
    if (!g_schema) die("cursor protocol schema: %s", err ? err : "parse error");
}

const pb_schema_t *cursor_proto(void) {
    pthread_once(&g_once, schema_init);
    return g_schema;
}

const pb_msg_t *cursor_msg(const char *name) {
    const pb_msg_t *m = pb_schema_msg(cursor_proto(), name);
    if (!m) die("cursor protocol: unknown message %s", name);
    return m;
}

char *cursor_encode(const char *name, const json_t *val, size_t *len) {
    return pb_encode_named(cursor_proto(), name, val, len);
}

json_t *cursor_decode(const char *name, const void *data, size_t len, char **err) {
    return pb_decode(cursor_msg(name), data, len, err);
}

/* ---------- Connect envelope ---------- */

void connect_frame(sb_t *out, int flags, const void *data, size_t len) {
    char hdr[5];
    hdr[0] = (char)flags;
    hdr[1] = (char)((len >> 24) & 0xff);
    hdr[2] = (char)((len >> 16) & 0xff);
    hdr[3] = (char)((len >> 8) & 0xff);
    hdr[4] = (char)(len & 0xff);
    sb_appendn(out, hdr, 5);
    if (len) sb_appendn(out, data, len);
}

void connect_decoder_init(connect_decoder_t *d, size_t max_payload) {
    memset(d, 0, sizeof(*d));
    d->max_payload = max_payload;
}

void connect_decoder_free(connect_decoder_t *d) {
    sb_free(&d->buf);
    d->off = 0;
}

void connect_decoder_feed(connect_decoder_t *d, const void *data, size_t len) {
    if (d->off && d->off == d->buf.len) {
        sb_clear(&d->buf);
        d->off = 0;
    } else if (d->off > 65536 && d->off * 2 > d->buf.len) {
        /* compact: drop the consumed prefix once it dominates the buffer */
        memmove(d->buf.s, d->buf.s + d->off, d->buf.len - d->off);
        sb_truncate(&d->buf, d->buf.len - d->off);
        d->off = 0;
    }
    if (len) sb_appendn(&d->buf, data, len);
}

int connect_decoder_next(connect_decoder_t *d, int *flags, const char **payload, size_t *len) {
    size_t avail = d->buf.len - d->off;
    if (avail < 5) return 0;
    const unsigned char *h = (const unsigned char *)d->buf.s + d->off;
    size_t n = ((size_t)h[1] << 24) | ((size_t)h[2] << 16) | ((size_t)h[3] << 8) | (size_t)h[4];
    if (d->max_payload && n > d->max_payload) {
        *len = n;
        return -1;
    }
    if (avail - 5 < n) return 0;
    *flags = h[0];
    *payload = (const char *)h + 5;
    *len = n;
    d->off += 5 + n;
    return 1;
}

/* ---------- Connect end-stream errors ---------- */

#define MAX_EXTRA_DETAIL_CHARS 400

/* omp truncate(): keep maxLen chars including a trailing ellipsis (counted in code points here) */
static char *truncate_chars(const char *s, size_t max) {
    if (utf8_strlen(s) <= max) return xstrdup(s);
    sb_t b = SB_INIT;
    size_t keep = max > 1 ? max - 1 : 0, n = 0;
    const char *p = s;
    while (*p && n < keep) {
        uint32_t cp;
        int k = utf8_decode(p, strlen(p), &cp);
        sb_appendn(&b, p, (size_t)k);
        p += k;
        n++;
    }
    sb_append(&b, "…");
    return sb_detach(&b);
}

/* JSON.stringify for non-strings, the string itself otherwise; NULL when empty */
static char *safe_json(const json_t *v) {
    if (!v) return NULL;
    char *s = json_is_str(v) ? xstrdup(json_sv(v)) : json_dump(v, 0);
    if (s && !*s) {
        free(s);
        return NULL;
    }
    return s;
}

char *connect_summarize_error_details(const json_t *details) {
    if (!json_is_arr(details) || !json_len(details)) return NULL;
    strvec_t parts = {0};
    json_t *e;
    json_arr_foreach(details, i, e) {
        if (!json_is_obj(e)) continue;
        const char *type = json_get_str(e, "type");
        if (type && !*type) type = NULL;
        json_t *dbg = json_get(e, "debug"), *val = json_get(e, "value");
        char *diag = dbg ? safe_json(dbg) : NULL;
        if (!diag && val) diag = safe_json(val);
        if (type && diag) strvec_push_own(&parts, xasprintf("%s: %s", type, diag));
        else if (type) strvec_push(&parts, type);
        else if (diag) strvec_push(&parts, diag);
        free(diag);
    }
    if (!parts.n) {
        strvec_free(&parts);
        return NULL;
    }
    char *joined = strvec_join(&parts, "; ");
    strvec_free(&parts);
    char *r = truncate_chars(joined, MAX_EXTRA_DETAIL_CHARS);
    free(joined);
    return r;
}

bool connect_has_retryable_cursor_detail(const json_t *details) {
    json_t *e;
    json_arr_foreach(details, i, e) {
        if (!json_is_obj(e) || !str_eq(json_get_str(e, "type"), "aiserver.v1.ErrorDetails")) continue;
        json_t *dbg = json_get_obj(e, "debug");
        json_t *inner = dbg ? json_get_obj(dbg, "details") : NULL;
        json_t *r = inner ? json_get(inner, "isRetryable") : NULL;
        if (json_is_bool(r) && r->u.b) return true;
    }
    return false;
}

static bool generic_message(const char *m) {
    char *t = str_trim(m ? m : "");
    char *l = str_lower(t);
    static const char *const generic[] = {"", "error", "unknown", "unknown error", "internal", "internal error"};
    bool r = false;
    for (size_t i = 0; i < ARRAY_LEN(generic); i++)
        if (strcmp(l, generic[i]) == 0) r = true;
    free(t);
    free(l);
    return r;
}

char *connect_format_end_stream_error(const json_t *error) {
    const char *code = json_is_obj(error) ? json_get_str(error, "code") : NULL;
    if (!code || !*code) code = "unknown";
    const char *message = json_is_obj(error) ? json_get_str(error, "message") : NULL;
    if (!message) message = "";
    char *detail = connect_summarize_error_details(json_is_obj(error) ? json_get(error, "details") : NULL);
    sb_t b = SB_INIT;
    sb_appendf(&b, "Connect error %s: %s", code, *message ? message : "Unknown error");
    if (detail) {
        sb_appendf(&b, " [details: %s]", detail);
    } else if (generic_message(message) && json_is_obj(error)) {
        json_t *extras = json_obj();
        const char *k;
        json_t *v;
        json_obj_foreach(error, i, k, v) {
            if (strcmp(k, "code") == 0 || strcmp(k, "message") == 0) continue;
            json_set(extras, k, json_clone(v));
        }
        if (json_len(extras)) {
            char *raw = json_dump(extras, 0);
            if (raw && strcmp(raw, "{}") != 0) {
                char *t = truncate_chars(raw, MAX_EXTRA_DETAIL_CHARS);
                sb_appendf(&b, " [trailer: %s]", t);
                free(t);
            }
            free(raw);
        }
        json_free(extras);
    }
    free(detail);
    return sb_detach(&b);
}
