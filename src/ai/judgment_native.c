/* judgment_native.c - native judgment APIs (port of omp packages/ai/src/judgment/{native,systemone}.ts): catalog
 * judge models whose API answers typed questions directly instead of through a chat transcript.
 *   typesafe              POST {baseUrl}/v1/systemone      {state, model, questions[, assets]}
 *   openrouter-decisions  POST {baseUrl}/decisions         same body, images as image_url parts in state
 *   workers-ai-systemone  POST {baseUrl with account}/{model}  {state, model: last segment, questions[, images]},
 *                         answers inside Cloudflare's {result, success, errors} envelope */
#define _GNU_SOURCE
#include "judgment_native.h"

#include "net/http.h"

#define JUDGE_TIMEOUT_MS 10000
#define JUDGE_ATTEMPTS 3

bool ai_is_judgment_api(const char *api) {
    return str_eq(api, "typesafe") || str_eq(api, "openrouter-decisions") || str_eq(api, "workers-ai-systemone");
}

static bool model_has_image_input(const ai_model_t *m) { return m->input_image; }

/* stateWithImages: arrays gain the items, objects under the first free __omp_judgment_images[_N] key, a string root
 * becomes [state, ...items] */
static json_t *state_with_images(const json_t *state, json_t *items) {
    json_t *out;
    if (json_is_arr(state)) {
        out = json_clone(state);
        json_t *it;
        json_arr_foreach(items, i, it) json_push(out, json_clone(it));
    } else if (json_is_obj(state)) {
        out = json_clone(state);
        char *key = xstrdup("__omp_judgment_images");
        for (int n = 1; json_get(out, key); n++) {
            free(key);
            key = xasprintf("__omp_judgment_images_%d", n);
        }
        json_set(out, key, json_clone(items));
        free(key);
    } else {
        out = json_arr();
        json_push(out, json_clone(state));
        json_t *it;
        json_arr_foreach(items, i, it) json_push(out, json_clone(it));
    }
    return out;
}

static char *data_url(const json_t *img) {
    return xasprintf("data:%s;base64,%s", json_get_str(img, "mimeType") ? json_get_str(img, "mimeType") : "image/png",
                     json_get_str(img, "data") ? json_get_str(img, "data") : "");
}

static double usage_num(const json_t *u, const char *k, double def) {
    json_t *v = json_get(u, k);
    return json_is_num(v) ? (v->is_int ? (double)v->u.num.i : v->u.num.d) : def;
}

json_t *ai_native_judge(const ai_model_t *m, const char *key, const json_t *state, const json_t *questions,
                        const json_t *images, volatile sig_atomic_t *cancel, double *cost, char **model_out, char **err) {
    *err = NULL;
    if (model_out) *model_out = NULL;
    const char *api = m->api;
    const char *wire_model = json_get_str(m->raw, "requestModelId") ? json_get_str(m->raw, "requestModelId") : m->id;
    char *label = xasprintf("%s/%s", m->provider, wire_model);
    char *base = xstrdup(m->base_url ? m->base_url : "");
    for (size_t n = strlen(base); n > 0 && base[n - 1] == '/';) base[--n] = 0;
    bool want_images = json_len(images) > 0;
    if (want_images && !model_has_image_input(m)) {
        *err = xasprintf("Native judgment backend %s does not support image inputs", label);
        free(label);
        free(base);
        return NULL;
    }
    /* target + body */
    char *url = NULL, *token = NULL;
    json_t *body = json_obj();
    http_headers_t h = {0};
    if (json_is_obj(m->headers)) {
        const char *k;
        json_t *v;
        json_obj_foreach(m->headers, i, k, v) if (json_is_str(v)) http_headers_set(&h, k, json_sv(v));
    }
    if (str_eq(api, "workers-ai-systemone")) {
        char *tok = NULL, *acct = NULL, *gw = NULL;
        bool ok = ai_cloudflare_credential(key, &tok, &acct, &gw);
        free(gw);
        const char *a = acct ? acct : env_get("CLOUDFLARE_ACCOUNT_ID");
        char *at = a ? str_trim(a) : NULL;
        if (!at || !*at) {
            *err = xstrdup("Cloudflare account ID is required: log in to Cloudflare Workers AI again or set CLOUDFLARE_ACCOUNT_ID");
        } else {
            sb_t b = SB_INIT;
            const char *ph = strstr(base, "<account>");
            if (ph) {
                sb_appendn(&b, base, (size_t)(ph - base));
                sb_append(&b, at);
                sb_append(&b, ph + strlen("<account>"));
            } else {
                sb_append(&b, base);
            }
            sb_appendf(&b, "/%s", wire_model);
            url = sb_detach(&b);
            token = xstrdup(ok && tok ? tok : key);
        }
        free(at);
        free(tok);
        free(acct);
        json_set(body, "state", json_clone(state));
        const char *slash = strrchr(wire_model, '/');
        json_set_str(body, "model", slash ? slash + 1 : wire_model);
        json_set(body, "questions", json_clone(questions));
        if (want_images) {
            json_t *arr = json_arr(), *img;
            json_arr_foreach(images, i, img) {
                char *d = data_url(img);
                json_push_str(arr, d);
                free(d);
            }
            json_set(body, "images", arr);
        }
    } else if (str_eq(api, "openrouter-decisions")) {
        url = xasprintf("%s/decisions", base);
        token = xstrdup(key);
        json_t *parts = json_arr(), *img;
        json_arr_foreach(images, i, img) {
            json_t *p = json_obj(), *iu = json_obj();
            char *d = data_url(img);
            json_set_str(iu, "url", d);
            free(d);
            const char *det = json_get_str(img, "detail");
            if (det) json_set_str(iu, "detail", str_eq(det, "original") ? "auto" : det);
            json_set_str(p, "type", "image_url");
            json_set(p, "image_url", iu);
            json_push(parts, p);
        }
        json_set(body, "state", want_images ? state_with_images(state, parts) : json_clone(state));
        json_free(parts);
        json_set_str(body, "model", wire_model);
        json_set(body, "questions", json_clone(questions));
    } else {
        url = xasprintf("%s/v1/systemone", base);
        token = xstrdup(key);
        json_t *assets = want_images ? json_obj() : NULL, *refs = json_arr(), *img;
        json_arr_foreach(images, i, img) {
            char *ref = uuid_v4();
            json_t *a = json_obj();
            const char *mt = json_get_str(img, "mimeType");
            json_set_str(a, "type", "image");
            json_set_str(a, "format", mt && !strcasecmp(mt, "image/png") ? "png" : "jpeg");
            json_set_str(a, "data_base64", json_get_str(img, "data") ? json_get_str(img, "data") : "");
            json_set(assets, ref, a);
            json_push_str(refs, ref);
            free(ref);
        }
        json_set(body, "state", want_images ? state_with_images(state, refs) : json_clone(state));
        json_free(refs);
        json_set_str(body, "model", wire_model);
        json_set(body, "questions", json_clone(questions));
        if (assets) {
            json_set(body, "assets", assets);
            http_headers_set(&h, "TypeSafe-Danger-Zone-Images", "true");
        }
    }
    if (str_eq(api, "openrouter-decisions")) {
        /* openRouterDecisionsBody: the configured compat in the chat vocabulary, merged over the encoded body */
        const json_t *cfg = json_get_obj(m->raw, "compatConfig");
        const json_t *routing = json_get(cfg, "openRouterRouting");
        if (json_is_obj(routing)) json_set(body, "provider", json_clone(routing));
        const json_t *extra = json_get_obj(cfg, "extraBody");
        const char *ek;
        json_t *ev;
        json_obj_foreach(extra, xi, ek, ev) json_set(body, ek, json_clone(ev));
    }
    json_t *answers = NULL;
    if (!*err) {
        char *auth = xasprintf("Bearer %s", token ? token : "");
        http_headers_set(&h, "Authorization", auth);
        free(auth);
        http_headers_set(&h, "Accept", "application/json");
        http_headers_set(&h, "Content-Type", "application/json");
        if (!http_headers_get(&h, "User-Agent")) http_headers_set(&h, "User-Agent", OMC_NAME "/" OMC_VERSION);
        char *payload = json_dump(body, 0);
        char *u = ai_rewrite_url(url);
        json_t *resp = NULL;
        for (int attempt = 0; attempt < JUDGE_ATTEMPTS && !*err && !resp; attempt++) {
            if (cancel && *cancel) {
                *err = xstrdup("judgment aborted");
                break;
            }
            http_req_t req = {.method = "POST", .url = u, .headers = &h, .body = payload, .body_len = strlen(payload),
                              .timeout_ms = JUDGE_TIMEOUT_MS, .cancel = cancel};
            http_resp_t r;
            int rc = http_request(&req, &r);
            bool last = attempt + 1 >= JUDGE_ATTEMPTS;
            if (rc != 0) {
                if (last || (cancel && *cancel)) *err = xasprintf("%s request failed: %s", label, r.error ? r.error : "network error");
            } else if (r.status >= 200 && r.status < 300) {
                resp = json_parse_str(r.body ? r.body : "");
                if (!resp) *err = xasprintf("%s returned a malformed judgment response", label);
            } else {
                bool transient = r.status == 408 || r.status == 429 || r.status >= 500;
                if (!transient || last) *err = xasprintf("%s API error (%d): %s", label, r.status, r.body ? r.body : "");
            }
            http_resp_free(&r);
            if (!resp && !*err) sleep_ms(MIN(500LL << attempt, 5000));
        }
        free(u);
        free(payload);
        if (resp) {
            const json_t *res = str_eq(api, "workers-ai-systemone") ? json_get(resp, "result") : resp;
            const json_t *ans = json_get_obj(res, "answers");
            if (!json_is_obj(res) || !ans) {
                *err = xasprintf("%s returned a malformed judgment response", label);
            } else {
                answers = json_obj();
                const char *qid;
                json_t *q;
                json_obj_foreach(questions, i, qid, q) {
                    const char *type = json_get_str(q, "type");
                    json_t *a = json_get(ans, qid);
                    if (!json_is_obj(a) || !str_eq(json_get_str(a, "type"), type)) {
                        *err = xasprintf("%s response is missing a \"%s\" answer for question \"%s\"", label, type ? type : "?", qid);
                        break;
                    }
                    json_set(answers, qid, json_clone(a));
                }
                if (*err) {
                    json_free(answers);
                    answers = NULL;
                } else {
                    const json_t *usage = json_get_obj(res, "usage");
                    double in = usage_num(usage, "input_tokens", 0);
                    double c = usage_num(usage, "cost", -1);
                    if (cost) *cost += c >= 0 ? c : in * m->cost_input / 1e6;
                    if (model_out) {
                        const char *rm = json_get_str(res, "model");
                        *model_out = xasprintf("%s/%s", m->provider, rm ? rm : wire_model);
                    }
                }
            }
            json_free(resp);
        }
    }
    http_headers_free(&h);
    json_free(body);
    free(url);
    free(token);
    free(label);
    free(base);
    return answers;
}
