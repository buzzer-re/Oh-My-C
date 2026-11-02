/* imgattach.c - the session's addressable image attachments (port of session-provider-boundary.ts
 * getImageAttachments), behind `attachment://N` (tools_common.c). */
#include "coding/app.h"
#include "session/blobs.h"
#include "tools/tools.h"

/* the file behind image `index` (1-based) of the message at `mi`: the image-attachment notice the prompt carried
 * (renderAttachmentSourceNotice details), else NULL */
static const char *notice_path(const json_t *msgs, size_t mi, int index) {
    for (size_t k = mi; k-- > 0;) {
        const json_t *m = json_at(msgs, k);
        if (!str_eq(json_get_str(m, "customType"), "image-attachment")) break;
        const json_t *d = json_get_obj(m, "details");
        if (json_get_int(d, "index", 0) == index && json_get_str(d, "path")) return json_get_str(d, "path");
    }
    return NULL;
}

json_t *app_image_attachments(app_t *a) {
    json_t *out = json_arr();
    if (!a || !a->agent) return out;
    pthread_mutex_lock(&a->agent->mu);
    json_t *msgs = json_clone(a->agent->messages);
    pthread_mutex_unlock(&a->agent->mu);
    for (size_t i = json_len(msgs); i-- > 0;) {
        const json_t *m = json_at(msgs, i);
        const char *role = json_get_str(m, "role");
        bool carries = str_eq(role, "user") || str_eq(role, "developer") ||
                       (str_eq(role, "toolResult") && str_eq(json_get_str(m, "toolName"), "ask"));
        const json_t *content = json_get_arr(m, "content");
        if (!carries || !content) continue;
        int n = 0;
        const json_t *b;
        json_arr_foreach(content, k, b) n += str_eq(json_get_str(b, "type"), "image");
        if (!n) continue;
        int idx = 0;
        json_arr_foreach(content, k, b) {
            if (!str_eq(json_get_str(b, "type"), "image")) continue;
            idx++;
            const char *src = notice_path(msgs, i, idx);
            char *path = NULL;
            if (src) path = str_starts(src, "local://") ? tool_resolve_path(a->env, src) : xstrdup(src);
            else path = session_blob_put_display(json_get_str(b, "data"), json_get_str(b, "mimeType"));
            if (!path) continue; /* failed to materialize: attachment omitted */
            json_t *e = json_obj();
            json_set(e, "label", json_str_own(xasprintf("Image #%d", idx)));
            json_set(e, "uri", json_str_own(xasprintf("attachment://%d", idx)));
            json_set(e, "mimeType", json_str(json_get_str(b, "mimeType") ? json_get_str(b, "mimeType") : ""));
            json_set(e, "sourcePath", json_str_own(path));
            json_push(out, e);
        }
        break;
    }
    json_free(msgs);
    return out;
}

static json_t *env_image_attachments(tool_env_t *env) { return app_image_attachments(env->session); }

void app_bind_image_attachments(app_t *a) { a->env->image_attachments = env_image_attachments; }
