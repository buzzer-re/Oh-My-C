/* json.h - DOM JSON parser/serializer with ordered objects. */
#ifndef OMC_JSON_H
#define OMC_JSON_H

#include "core.h"

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type_t;

typedef struct json json_t;
struct json {
    json_type_t type;
    bool is_int;    /* for numbers: integral and representable in int64 */
    union {
        bool b;
        struct {
            double d;
            int64_t i;
        } num;
        struct {
            char *s;
            size_t len;
        } str;
        struct {
            json_t **items;
            size_t n, cap;
        } arr;
        struct {
            char **keys;
            json_t **vals;
            size_t n, cap;
        } obj;
    } u;
};

/* parsing */
json_t *json_parse(const char *s, size_t len, char **err);
json_t *json_parse_str(const char *s); /* NULL on error */
/* Best-effort parse of a truncated JSON document (streaming tool args). Never NULL for non-empty input
 * that starts like JSON; returns NULL otherwise. */
json_t *json_parse_partial(const char *s, size_t len);

/* construction (returned values are owned by the caller until inserted into a container) */
json_t *json_null(void);
json_t *json_bool(bool b);
json_t *json_num(double d);
json_t *json_int(int64_t i);
json_t *json_str(const char *s);
json_t *json_strn(const char *s, size_t n);
json_t *json_str_own(char *s);
json_t *json_arr(void);
json_t *json_obj(void);
json_t *json_clone(const json_t *j);
void json_free(json_t *j);

/* object access */
json_t *json_get(const json_t *obj, const char *key);
const char *json_get_str(const json_t *obj, const char *key); /* NULL if missing/not string */
double json_get_num(const json_t *obj, const char *key, double def);
int64_t json_get_int(const json_t *obj, const char *key, int64_t def);
bool json_get_bool(const json_t *obj, const char *key, bool def);
json_t *json_get_obj(const json_t *obj, const char *key); /* NULL if not object */
json_t *json_get_arr(const json_t *obj, const char *key); /* NULL if not array */
void json_set(json_t *obj, const char *key, json_t *val); /* takes ownership, replaces */
void json_set_str(json_t *obj, const char *key, const char *s);
void json_set_int(json_t *obj, const char *key, int64_t i);
void json_set_num(json_t *obj, const char *key, double d);
void json_set_bool(json_t *obj, const char *key, bool b);
json_t *json_take(json_t *obj, const char *key); /* removes and returns (owned) */
void json_del(json_t *obj, const char *key);

/* array access */
size_t json_len(const json_t *j); /* array/object element count, string length */
json_t *json_at(const json_t *arr, size_t i);
void json_push(json_t *arr, json_t *val); /* takes ownership */
void json_push_str(json_t *arr, const char *s);
json_t *json_arr_remove(json_t *arr, size_t i); /* returns owned */
void json_arr_insert(json_t *arr, size_t i, json_t *val); /* takes ownership; i > len appends */

/* type checks */
static inline bool json_is_str(const json_t *j) { return j && j->type == JSON_STRING; }
static inline bool json_is_obj(const json_t *j) { return j && j->type == JSON_OBJECT; }
static inline bool json_is_arr(const json_t *j) { return j && j->type == JSON_ARRAY; }
static inline bool json_is_num(const json_t *j) { return j && j->type == JSON_NUMBER; }
static inline bool json_is_bool(const json_t *j) { return j && j->type == JSON_BOOL; }
static inline bool json_is_null(const json_t *j) { return !j || j->type == JSON_NULL; }
static inline const char *json_sv(const json_t *j) { return json_is_str(j) ? j->u.str.s : NULL; }
bool json_truthy(const json_t *j);
bool json_equal(const json_t *a, const json_t *b);

/* path lookup: "a.b.0.c" */
json_t *json_path(const json_t *j, const char *path);

/* serialization */
char *json_dump(const json_t *j, int indent); /* indent 0 => compact */
void json_dump_sb(sb_t *b, const json_t *j, int indent);
void json_escape_sb(sb_t *b, const char *s, size_t len); /* writes quoted string */
char *json_quote(const char *s);

/* object merge (deep): copies entries from src into dst */
void json_merge(json_t *dst, const json_t *src);

#define json_arr_foreach(A_, IDX_, ITEM_)                                                        \
    for (size_t IDX_ = 0; json_is_arr(A_) && IDX_ < (A_)->u.arr.n && ((ITEM_) = (A_)->u.arr.items[IDX_], 1); \
         IDX_++)
#define json_obj_foreach(O_, IDX_, K_, V_)                                                       \
    for (size_t IDX_ = 0; json_is_obj(O_) && IDX_ < (O_)->u.obj.n &&                             \
                          ((K_) = (O_)->u.obj.keys[IDX_], (V_) = (O_)->u.obj.vals[IDX_], 1);     \
         IDX_++)

#endif
