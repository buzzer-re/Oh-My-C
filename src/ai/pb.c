/* pb.c - minimal schema-driven protobuf wire codec (see pb.h). */
#include "pb.h"

#include <math.h>

typedef enum {
    PB_STRING,
    PB_BYTES,
    PB_BOOL,
    PB_INT32,
    PB_INT64,
    PB_UINT32,
    PB_UINT64,
    PB_ENUM,
    PB_DOUBLE,
    PB_FLOAT,
    PB_MESSAGE,
} pb_kind_t;

typedef struct {
    int no;
    char *name;
    pb_kind_t kind;   /* value kind (map: value kind) */
    bool map;         /* map<string, kind> */
    bool repeated;
    bool presence;    /* optional / oneof member */
    char *type_name;  /* message type (unresolved name) */
    const pb_msg_t *type;
} pb_field_t;

struct pb_msg {
    char *name;
    pb_field_t *fields;
    size_t n;
};

struct pb_schema {
    hmap_t by_name;
    VEC(pb_msg_t *) msgs;
};

#define PB_MAX_DEPTH 100

/* ---------- wire primitives ---------- */

void pb_put_varint(sb_t *out, uint64_t v) {
    char buf[10];
    int n = 0;
    do {
        unsigned char b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        buf[n++] = (char)b;
    } while (v);
    sb_appendn(out, buf, (size_t)n);
}

void pb_put_tag(sb_t *out, int no, int wire_type) { pb_put_varint(out, ((uint64_t)no << 3) | (uint64_t)wire_type); }

void pb_put_len_bytes(sb_t *out, int no, const void *data, size_t len) {
    pb_put_tag(out, no, 2);
    pb_put_varint(out, len);
    if (len) sb_appendn(out, data, len);
}

int pb_get_varint(const unsigned char *p, size_t len, size_t *pos, uint64_t *out) {
    uint64_t v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        if (*pos >= len) return -1;
        unsigned char b = p[(*pos)++];
        if (shift == 63 && (b & 0x7e)) return -1;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
    }
    return -1;
}

static void put_fixed(sb_t *out, uint64_t v, int nbytes) {
    char buf[8];
    for (int i = 0; i < nbytes; i++) buf[i] = (char)((v >> (8 * i)) & 0xff);
    sb_appendn(out, buf, (size_t)nbytes);
}

static uint64_t get_fixed(const unsigned char *p, int nbytes) {
    uint64_t v = 0;
    for (int i = 0; i < nbytes; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* skip one field value of `wt` at *pos; -1 on truncation / bad wire type */
static int skip_value(const unsigned char *p, size_t len, size_t *pos, int wt, int depth) {
    uint64_t v;
    switch (wt) {
    case 0: return pb_get_varint(p, len, pos, &v);
    case 1:
        if (len - *pos < 8) return -1;
        *pos += 8;
        return 0;
    case 5:
        if (len - *pos < 4) return -1;
        *pos += 4;
        return 0;
    case 2:
        if (pb_get_varint(p, len, pos, &v) || v > len - *pos) return -1;
        *pos += (size_t)v;
        return 0;
    case 3: /* deprecated groups: skip to the matching end tag */
        if (depth > PB_MAX_DEPTH) return -1;
        for (;;) {
            uint64_t tag;
            if (pb_get_varint(p, len, pos, &tag)) return -1;
            if ((tag & 7) == 4) return 0;
            if (skip_value(p, len, pos, (int)(tag & 7), depth + 1)) return -1;
        }
    default: return -1;
    }
}

/* ---------- schema ---------- */

static bool scalar_kind(const char *s, size_t n, pb_kind_t *k) {
    static const struct {
        const char *name;
        pb_kind_t kind;
    } tab[] = {{"string", PB_STRING}, {"bytes", PB_BYTES},   {"bool", PB_BOOL},     {"int32", PB_INT32},
               {"int64", PB_INT64},   {"uint32", PB_UINT32}, {"uint64", PB_UINT64}, {"enum", PB_ENUM},
               {"double", PB_DOUBLE}, {"float", PB_FLOAT}};
    for (size_t i = 0; i < ARRAY_LEN(tab); i++) {
        if (strlen(tab[i].name) == n && memcmp(tab[i].name, s, n) == 0) {
            *k = tab[i].kind;
            return true;
        }
    }
    return false;
}

static void msg_free(pb_msg_t *m) {
    if (!m) return;
    for (size_t i = 0; i < m->n; i++) {
        free(m->fields[i].name);
        free(m->fields[i].type_name);
    }
    free(m->fields);
    free(m->name);
    free(m);
}

void pb_schema_free(pb_schema_t *s) {
    if (!s) return;
    for (size_t i = 0; i < s->msgs.n; i++) msg_free(s->msgs.v[i]);
    vec_free(&s->msgs);
    hmap_free(&s->by_name, NULL);
    free(s);
}

static int parse_field(const char *tok, size_t n, pb_field_t *f, char **err) {
    memset(f, 0, sizeof(*f));
    const char *c1 = memchr(tok, ':', n);
    const char *c2 = c1 ? memchr(c1 + 1, ':', n - (size_t)(c1 + 1 - tok)) : NULL;
    if (!c1 || !c2) {
        *err = xasprintf("bad field \"%.*s\"", (int)n, tok);
        return -1;
    }
    char *end;
    long no = strtol(tok, &end, 10);
    if (end != c1 || no <= 0 || no > 536870911) {
        *err = xasprintf("bad field number \"%.*s\"", (int)n, tok);
        return -1;
    }
    f->no = (int)no;
    f->name = xstrndup(c1 + 1, (size_t)(c2 - c1 - 1));
    const char *t = c2 + 1, *te = tok + n;
    while (te > t && (te[-1] == '*' || te[-1] == '?')) {
        if (te[-1] == '*') f->repeated = true;
        else f->presence = true;
        te--;
    }
    if (te - t > 5 && memcmp(t, "map<", 4) == 0 && te[-1] == '>') {
        f->map = true;
        t += 4;
        te--;
    }
    if (te <= t) {
        *err = xasprintf("missing type in \"%.*s\"", (int)n, tok);
        return -1;
    }
    if (!scalar_kind(t, (size_t)(te - t), &f->kind)) {
        f->kind = PB_MESSAGE;
        f->type_name = xstrndup(t, (size_t)(te - t));
    }
    return 0;
}

pb_schema_t *pb_schema_parse(const char *dsl, char **err) {
    *err = NULL;
    pb_schema_t *s = xcalloc(1, sizeof(*s));
    hmap_init(&s->by_name);
    const char *p = dsl;
    while (*p) {
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        const char *q = p;
        while (q < eol && (*q == ' ' || *q == '\t')) q++;
        if (q < eol && *q != '#') {
            const char *ne = q;
            while (ne < eol && *ne != ' ') ne++;
            pb_msg_t *m = xcalloc(1, sizeof(*m));
            m->name = xstrndup(q, (size_t)(ne - q));
            VEC(pb_field_t) fs = {0};
            const char *t = ne;
            while (t < eol) {
                while (t < eol && *t == ' ') t++;
                if (t >= eol) break;
                const char *te = t;
                while (te < eol && *te != ' ') te++;
                pb_field_t f;
                if (parse_field(t, (size_t)(te - t), &f, err)) {
                    for (size_t i = 0; i < fs.n; i++) {
                        free(fs.v[i].name);
                        free(fs.v[i].type_name);
                    }
                    vec_free(&fs);
                    msg_free(m);
                    pb_schema_free(s);
                    return NULL;
                }
                vec_push(&fs, f);
                t = te;
            }
            m->fields = fs.v;
            m->n = fs.n;
            if (hmap_get(&s->by_name, m->name)) {
                *err = xasprintf("duplicate message %s", m->name);
                msg_free(m);
                pb_schema_free(s);
                return NULL;
            }
            hmap_put(&s->by_name, m->name, m);
            vec_push(&s->msgs, m);
        }
        p = *eol ? eol + 1 : eol;
    }
    for (size_t i = 0; i < s->msgs.n; i++) {
        pb_msg_t *m = s->msgs.v[i];
        for (size_t k = 0; k < m->n; k++) {
            pb_field_t *f = &m->fields[k];
            if (f->kind != PB_MESSAGE) continue;
            f->type = hmap_get(&s->by_name, f->type_name);
            if (!f->type) {
                *err = xasprintf("%s.%s: unknown type %s", m->name, f->name, f->type_name);
                pb_schema_free(s);
                return NULL;
            }
        }
    }
    return s;
}

const pb_msg_t *pb_schema_msg(const pb_schema_t *s, const char *name) { return s ? hmap_get(&s->by_name, name) : NULL; }
const char *pb_msg_name(const pb_msg_t *m) { return m ? m->name : NULL; }

/* ---------- encode ---------- */

static int wire_type_of(pb_kind_t k) {
    switch (k) {
    case PB_DOUBLE: return 1;
    case PB_FLOAT: return 5;
    case PB_STRING:
    case PB_BYTES:
    case PB_MESSAGE: return 2;
    default: return 0;
    }
}

static int encode_msg(const pb_msg_t *m, const json_t *val, sb_t *out, char **err, int depth);

static int num_of(const json_t *v, const pb_field_t *f, int64_t *iv, double *dv, char **err) {
    if (json_is_num(v)) {
        *iv = v->is_int ? v->u.num.i : (int64_t)v->u.num.d;
        *dv = v->is_int ? (double)v->u.num.i : v->u.num.d;
        return 0;
    }
    if (json_is_str(v)) { /* int64 values may arrive as decimal strings (protobuf JSON) */
        char *end;
        *iv = strtoll(json_sv(v), &end, 10);
        *dv = (double)*iv;
        if (*end == 0 && end != json_sv(v)) return 0;
    }
    *err = xasprintf("field %s: expected number", f->name);
    return -1;
}

/* true when v is the proto3 default for kind k */
static bool is_default(const pb_field_t *f, const json_t *v) {
    switch (f->kind) {
    case PB_STRING:
    case PB_BYTES: return json_is_str(v) && v->u.str.len == 0;
    case PB_BOOL: return json_is_bool(v) && !v->u.b;
    case PB_MESSAGE: return json_is_null(v);
    default:
        if (json_is_num(v)) return v->is_int ? v->u.num.i == 0 : v->u.num.d == 0;
        return false;
    }
}

/* writes the bare value (no tag) */
static int encode_value(const pb_field_t *f, const json_t *v, sb_t *out, char **err, int depth) {
    int64_t iv = 0;
    double dv = 0;
    switch (f->kind) {
    case PB_STRING:
    case PB_BYTES:
        if (!json_is_str(v)) {
            *err = xasprintf("field %s: expected string", f->name);
            return -1;
        }
        pb_put_varint(out, v->u.str.len);
        sb_appendn(out, v->u.str.s, v->u.str.len);
        return 0;
    case PB_BOOL:
        if (!json_is_bool(v)) {
            *err = xasprintf("field %s: expected bool", f->name);
            return -1;
        }
        pb_put_varint(out, v->u.b ? 1 : 0);
        return 0;
    case PB_INT32:
    case PB_ENUM:
        if (num_of(v, f, &iv, &dv, err)) return -1;
        pb_put_varint(out, (uint64_t)(int64_t)(int32_t)iv); /* negative int32 sign-extends to 10 bytes */
        return 0;
    case PB_INT64:
    case PB_UINT64:
        if (num_of(v, f, &iv, &dv, err)) return -1;
        pb_put_varint(out, (uint64_t)iv);
        return 0;
    case PB_UINT32:
        if (num_of(v, f, &iv, &dv, err)) return -1;
        pb_put_varint(out, (uint32_t)iv);
        return 0;
    case PB_DOUBLE: {
        if (num_of(v, f, &iv, &dv, err)) return -1;
        uint64_t bits;
        memcpy(&bits, &dv, 8);
        put_fixed(out, bits, 8);
        return 0;
    }
    case PB_FLOAT: {
        if (num_of(v, f, &iv, &dv, err)) return -1;
        float fv = (float)dv;
        uint32_t bits;
        memcpy(&bits, &fv, 4);
        put_fixed(out, bits, 4);
        return 0;
    }
    case PB_MESSAGE: {
        if (!json_is_obj(v)) {
            *err = xasprintf("field %s: expected object", f->name);
            return -1;
        }
        sb_t sub = SB_INIT;
        if (encode_msg(f->type, v, &sub, err, depth + 1)) {
            sb_free(&sub);
            return -1;
        }
        pb_put_varint(out, sub.len);
        if (sub.len) sb_appendn(out, sub.s, sub.len);
        sb_free(&sub);
        return 0;
    }
    }
    return 0;
}

static int encode_field(const pb_field_t *f, const json_t *v, sb_t *out, char **err, int depth) {
    if (!v) return 0;
    if (f->map) {
        if (!json_is_obj(v)) {
            *err = xasprintf("field %s: expected map object", f->name);
            return -1;
        }
        const char *k;
        json_t *ev;
        json_obj_foreach(v, i, k, ev) {
            sb_t entry = SB_INIT;
            if (*k) pb_put_len_bytes(&entry, 1, k, strlen(k));
            if (!is_default(f, ev)) {
                pb_put_tag(&entry, 2, wire_type_of(f->kind));
                if (encode_value(f, ev, &entry, err, depth)) {
                    sb_free(&entry);
                    return -1;
                }
            }
            pb_put_len_bytes(out, f->no, entry.s, entry.len);
            sb_free(&entry);
        }
        return 0;
    }
    if (f->repeated) {
        if (!json_is_arr(v)) {
            *err = xasprintf("field %s: expected array", f->name);
            return -1;
        }
        if (!json_len(v)) return 0;
        int wt = wire_type_of(f->kind);
        json_t *it;
        if (wt != 2) { /* packed */
            sb_t packed = SB_INIT;
            json_arr_foreach(v, i, it) {
                if (encode_value(f, it, &packed, err, depth)) {
                    sb_free(&packed);
                    return -1;
                }
            }
            pb_put_len_bytes(out, f->no, packed.s, packed.len);
            sb_free(&packed);
            return 0;
        }
        json_arr_foreach(v, i, it) {
            pb_put_tag(out, f->no, wt);
            if (encode_value(f, it, out, err, depth)) return -1;
        }
        return 0;
    }
    if (json_is_null(v)) return 0;
    if (!f->presence && is_default(f, v)) return 0;
    pb_put_tag(out, f->no, wire_type_of(f->kind));
    return encode_value(f, v, out, err, depth);
}

static int encode_msg(const pb_msg_t *m, const json_t *val, sb_t *out, char **err, int depth) {
    if (depth > PB_MAX_DEPTH) {
        *err = xstrdup("message nesting too deep");
        return -1;
    }
    for (size_t i = 0; i < m->n; i++) {
        if (encode_field(&m->fields[i], json_get(val, m->fields[i].name), out, err, depth)) return -1;
    }
    json_t *unk = json_get_arr(val, "$unknown"), *u;
    json_arr_foreach(unk, i, u) {
        json_t *data = json_get(u, "data");
        int64_t no = json_get_int(u, "no", 0), wt = json_get_int(u, "wt", -1);
        if (no <= 0 || wt < 0 || wt > 5 || !json_is_str(data)) continue;
        pb_put_tag(out, (int)no, (int)wt);
        sb_appendn(out, data->u.str.s, data->u.str.len);
    }
    return 0;
}

int pb_encode(const pb_msg_t *m, const json_t *val, sb_t *out, char **err) {
    *err = NULL;
    if (!m) {
        *err = xstrdup("unknown message type");
        return -1;
    }
    if (!json_is_obj(val)) {
        *err = xasprintf("%s: expected object", m->name);
        return -1;
    }
    return encode_msg(m, val, out, err, 0);
}

char *pb_encode_named(const pb_schema_t *s, const char *name, const json_t *val, size_t *len) {
    const pb_msg_t *m = pb_schema_msg(s, name);
    if (!m) die("pb: unknown message %s", name);
    sb_t out = SB_INIT;
    char *err = NULL;
    if (pb_encode(m, val, &out, &err)) die("pb: encoding %s: %s", name, err);
    *len = out.len;
    if (!out.s) return xcalloc(1, 1);
    return out.s; /* sb buffer is NUL-terminated and owned */
}

/* ---------- decode ---------- */

static const pb_field_t *field_by_no(const pb_msg_t *m, int no) {
    for (size_t i = 0; i < m->n; i++)
        if (m->fields[i].no == no) return &m->fields[i];
    return NULL;
}

static json_t *decode_msg(const pb_msg_t *m, const unsigned char *p, size_t len, char **err, int depth);

/* decode one value of field f with wire type wt at *pos */
static json_t *decode_value(const pb_field_t *f, const unsigned char *p, size_t len, size_t *pos, int wt, char **err,
                            int depth) {
    int want = wire_type_of(f->kind);
    if (wt != want) {
        *err = xasprintf("field %s: wire type %d, expected %d", f->name, wt, want);
        return NULL;
    }
    uint64_t v;
    switch (f->kind) {
    case PB_STRING:
    case PB_BYTES:
    case PB_MESSAGE:
        if (pb_get_varint(p, len, pos, &v) || v > len - *pos) break;
        {
            const unsigned char *d = p + *pos;
            *pos += (size_t)v;
            if (f->kind == PB_MESSAGE) return decode_msg(f->type, d, (size_t)v, err, depth + 1);
            return json_strn((const char *)d, (size_t)v);
        }
    case PB_BOOL:
        if (pb_get_varint(p, len, pos, &v)) break;
        return json_bool(v != 0);
    case PB_INT32:
    case PB_ENUM:
        if (pb_get_varint(p, len, pos, &v)) break;
        return json_int((int32_t)(uint32_t)v);
    case PB_UINT32:
        if (pb_get_varint(p, len, pos, &v)) break;
        return json_int((uint32_t)v);
    case PB_INT64:
    case PB_UINT64:
        if (pb_get_varint(p, len, pos, &v)) break;
        return json_int((int64_t)v);
    case PB_DOUBLE: {
        if (len - *pos < 8) break;
        uint64_t bits = get_fixed(p + *pos, 8);
        *pos += 8;
        double d;
        memcpy(&d, &bits, 8);
        return json_num(isfinite(d) ? d : 0);
    }
    case PB_FLOAT: {
        if (len - *pos < 4) break;
        uint32_t bits = (uint32_t)get_fixed(p + *pos, 4);
        *pos += 4;
        float fv;
        memcpy(&fv, &bits, 4);
        return json_num(isfinite(fv) ? fv : 0);
    }
    }
    if (!*err) *err = xasprintf("field %s: truncated", f->name);
    return NULL;
}

static json_t *default_of(const pb_field_t *f) {
    switch (f->kind) {
    case PB_STRING:
    case PB_BYTES: return json_str("");
    case PB_BOOL: return json_bool(false);
    case PB_DOUBLE:
    case PB_FLOAT: return json_num(0);
    case PB_MESSAGE: return json_obj();
    default: return json_int(0);
    }
}

static int decode_map_entry(const pb_field_t *f, json_t *msg, const unsigned char *p, size_t len, char **err,
                            int depth) {
    size_t pos = 0;
    char *key = NULL;
    json_t *val = NULL;
    while (pos < len) {
        uint64_t tag;
        if (pb_get_varint(p, len, &pos, &tag)) goto bad;
        int no = (int)(tag >> 3), wt = (int)(tag & 7);
        if (no == 1 && wt == 2) {
            uint64_t n;
            if (pb_get_varint(p, len, &pos, &n) || n > len - pos) goto bad;
            free(key);
            key = xstrndup((const char *)p + pos, (size_t)n);
            pos += (size_t)n;
        } else if (no == 2) {
            json_t *v = decode_value(f, p, len, &pos, wt, err, depth);
            if (!v) goto fail;
            json_free(val);
            val = v;
        } else if (skip_value(p, len, &pos, wt, depth)) {
            goto bad;
        }
    }
    {
        json_t *map = json_get(msg, f->name);
        if (!json_is_obj(map)) {
            map = json_obj();
            json_set(msg, f->name, map);
        }
        json_set(map, key ? key : "", val ? val : default_of(f));
    }
    free(key);
    return 0;
bad:
    if (!*err) *err = xasprintf("field %s: malformed map entry", f->name);
fail:
    free(key);
    json_free(val);
    return -1;
}

static json_t *decode_msg(const pb_msg_t *m, const unsigned char *p, size_t len, char **err, int depth) {
    if (depth > PB_MAX_DEPTH) {
        *err = xstrdup("message nesting too deep");
        return NULL;
    }
    json_t *msg = json_obj();
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag;
        if (pb_get_varint(p, len, &pos, &tag)) goto truncated;
        int no = (int)(tag >> 3), wt = (int)(tag & 7);
        if (no <= 0 || wt == 6 || wt == 7) {
            *err = xasprintf("%s: invalid tag %llu", m->name, (unsigned long long)tag);
            goto fail;
        }
        const pb_field_t *f = field_by_no(m, no);
        if (!f || wt == 3 || wt == 4) {
            size_t start = pos;
            if (wt == 4 || skip_value(p, len, &pos, wt, depth)) goto truncated;
            json_t *unk = json_get_arr(msg, "$unknown");
            if (!unk) {
                unk = json_arr();
                json_set(msg, "$unknown", unk);
            }
            json_t *u = json_obj();
            json_set_int(u, "no", no);
            json_set_int(u, "wt", wt);
            json_set(u, "data", json_strn((const char *)p + start, pos - start));
            json_push(unk, u);
            continue;
        }
        if (f->map) {
            uint64_t n;
            if (wt != 2 || pb_get_varint(p, len, &pos, &n) || n > len - pos) goto truncated;
            if (decode_map_entry(f, msg, p + pos, (size_t)n, err, depth)) goto fail;
            pos += (size_t)n;
            continue;
        }
        if (f->repeated) {
            json_t *arr = json_get_arr(msg, f->name);
            if (!arr) {
                arr = json_arr();
                json_set(msg, f->name, arr);
            }
            int want = wire_type_of(f->kind);
            if (wt == 2 && want != 2) { /* packed */
                uint64_t n;
                if (pb_get_varint(p, len, &pos, &n) || n > len - pos) goto truncated;
                size_t end = pos + (size_t)n;
                while (pos < end) {
                    json_t *v = decode_value(f, p, end, &pos, want, err, depth);
                    if (!v) goto fail;
                    json_push(arr, v);
                }
                continue;
            }
            json_t *v = decode_value(f, p, len, &pos, wt, err, depth);
            if (!v) goto fail;
            json_push(arr, v);
            continue;
        }
        json_t *v = decode_value(f, p, len, &pos, wt, err, depth);
        if (!v) goto fail;
        json_set(msg, f->name, v);
    }
    return msg;
truncated:
    if (!*err) *err = xasprintf("%s: truncated message", m->name);
fail:
    json_free(msg);
    return NULL;
}

json_t *pb_decode(const pb_msg_t *m, const void *data, size_t len, char **err) {
    *err = NULL;
    if (!m) {
        *err = xstrdup("unknown message type");
        return NULL;
    }
    return decode_msg(m, data, len, err, 0);
}

/* ---------- google.protobuf.Value ---------- */

static void value_encode(const json_t *v, sb_t *out, int depth) {
    if (depth > PB_MAX_DEPTH || json_is_null(v)) {
        pb_put_tag(out, 1, 0);
        pb_put_varint(out, 0);
        return;
    }
    switch (v->type) {
    case JSON_NUMBER: {
        double d = v->is_int ? (double)v->u.num.i : v->u.num.d;
        uint64_t bits;
        memcpy(&bits, &d, 8);
        pb_put_tag(out, 2, 1);
        put_fixed(out, bits, 8);
        return;
    }
    case JSON_STRING: pb_put_len_bytes(out, 3, v->u.str.s, v->u.str.len); return;
    case JSON_BOOL:
        pb_put_tag(out, 4, 0);
        pb_put_varint(out, v->u.b ? 1 : 0);
        return;
    case JSON_ARRAY: {
        sb_t list = SB_INIT;
        json_t *it;
        json_arr_foreach(v, i, it) {
            sb_t item = SB_INIT;
            value_encode(it, &item, depth + 1);
            pb_put_len_bytes(&list, 1, item.s, item.len);
            sb_free(&item);
        }
        pb_put_len_bytes(out, 6, list.s, list.len);
        sb_free(&list);
        return;
    }
    case JSON_OBJECT: {
        sb_t st = SB_INIT;
        const char *k;
        json_t *ev;
        json_obj_foreach(v, i, k, ev) {
            sb_t entry = SB_INIT, val = SB_INIT;
            pb_put_len_bytes(&entry, 1, k, strlen(k));
            value_encode(ev, &val, depth + 1);
            pb_put_len_bytes(&entry, 2, val.s, val.len);
            pb_put_len_bytes(&st, 1, entry.s, entry.len);
            sb_free(&entry);
            sb_free(&val);
        }
        pb_put_len_bytes(out, 5, st.s, st.len);
        sb_free(&st);
        return;
    }
    default: break;
    }
}

void pb_value_encode(const json_t *v, sb_t *out) { value_encode(v, out, 0); }

static json_t *value_decode(const unsigned char *p, size_t len, int depth);

static json_t *struct_decode(const unsigned char *p, size_t len, int depth) {
    json_t *obj = json_obj();
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag, n;
        if (pb_get_varint(p, len, &pos, &tag)) goto bad;
        if ((tag >> 3) != 1 || (tag & 7) != 2) {
            if (skip_value(p, len, &pos, (int)(tag & 7), depth)) goto bad;
            continue;
        }
        if (pb_get_varint(p, len, &pos, &n) || n > len - pos) goto bad;
        const unsigned char *e = p + pos;
        size_t el = (size_t)n, ep = 0;
        pos += (size_t)n;
        char *key = NULL;
        json_t *val = NULL;
        while (ep < el) {
            uint64_t t2, n2;
            if (pb_get_varint(e, el, &ep, &t2)) break;
            if ((t2 & 7) != 2) {
                if (skip_value(e, el, &ep, (int)(t2 & 7), depth)) break;
                continue;
            }
            if (pb_get_varint(e, el, &ep, &n2) || n2 > el - ep) break;
            if ((t2 >> 3) == 1) {
                free(key);
                key = xstrndup((const char *)e + ep, (size_t)n2);
            } else if ((t2 >> 3) == 2) {
                json_free(val);
                val = value_decode(e + ep, (size_t)n2, depth + 1);
            }
            ep += (size_t)n2;
        }
        json_set(obj, key ? key : "", val ? val : json_null());
        free(key);
    }
    return obj;
bad:
    json_free(obj);
    return NULL;
}

static json_t *value_decode(const unsigned char *p, size_t len, int depth) {
    if (depth > PB_MAX_DEPTH) return NULL;
    json_t *value = json_null();
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag, v;
        if (pb_get_varint(p, len, &pos, &tag)) goto bad;
        int no = (int)(tag >> 3), wt = (int)(tag & 7);
        json_t *nv = NULL;
        if (no == 1 && wt == 0) {
            if (pb_get_varint(p, len, &pos, &v)) goto bad;
            nv = json_null();
        } else if (no == 2 && wt == 1) {
            if (len - pos < 8) goto bad;
            uint64_t bits = get_fixed(p + pos, 8);
            pos += 8;
            double d;
            memcpy(&d, &bits, 8);
            if (isfinite(d) && d == floor(d) && fabs(d) < 9007199254740992.0) nv = json_int((int64_t)d);
            else nv = json_num(isfinite(d) ? d : 0);
        } else if (no == 4 && wt == 0) {
            if (pb_get_varint(p, len, &pos, &v)) goto bad;
            nv = json_bool(v != 0);
        } else if ((no == 3 || no == 5 || no == 6) && wt == 2) {
            if (pb_get_varint(p, len, &pos, &v) || v > len - pos) goto bad;
            const unsigned char *d = p + pos;
            pos += (size_t)v;
            if (no == 3) nv = json_strn((const char *)d, (size_t)v);
            else if (no == 5) nv = struct_decode(d, (size_t)v, depth + 1);
            else {
                nv = json_arr();
                size_t lp = 0;
                while (lp < v) {
                    uint64_t t2, n2;
                    if (pb_get_varint(d, (size_t)v, &lp, &t2)) break;
                    if ((t2 & 7) != 2) {
                        if (skip_value(d, (size_t)v, &lp, (int)(t2 & 7), depth)) break;
                        continue;
                    }
                    if (pb_get_varint(d, (size_t)v, &lp, &n2) || n2 > v - lp) break;
                    if ((t2 >> 3) == 1) {
                        json_t *item = value_decode(d + lp, (size_t)n2, depth + 1);
                        json_push(nv, item ? item : json_null());
                    }
                    lp += (size_t)n2;
                }
            }
            if (!nv) goto bad;
        } else {
            if (skip_value(p, len, &pos, wt, depth)) goto bad;
            continue;
        }
        json_free(value);
        value = nv;
    }
    return value;
bad:
    json_free(value);
    return NULL;
}

json_t *pb_value_decode(const void *data, size_t len) { return value_decode(data, len, 0); }
