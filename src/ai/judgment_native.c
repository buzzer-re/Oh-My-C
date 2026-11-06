/* judgment_native.c - native judgment APIs (port of omp packages/ai/src/judgment/{native,systemone}.ts): catalog
 * judge models whose API answers typed questions directly instead of through a chat transcript.
 *   typesafe              POST {baseUrl}/v1/systemone      {state, model, questions[, assets]}
 *   openrouter-decisions  POST {baseUrl}/decisions         same body, images as image_url parts in state
 *   openai-decisions      POST {baseUrl}/decisions         openai-decisions.ts: questions as an array, translated both ways
 *   workers-ai-systemone  POST {baseUrl with account}/{model}  {state, model: last segment, questions[, images]},
 *                         answers inside Cloudflare's {result, success, errors} envelope */
#define _GNU_SOURCE
#include "judgment_native.h"

#include "net/http.h"

#define JUDGE_TIMEOUT_MS 10000
#define JUDGE_ATTEMPTS 3

bool ai_is_judgment_api(const char *api) {
    return str_eq(api, "typesafe") || str_eq(api, "openrouter-decisions") || str_eq(api, "openai-decisions") ||
           str_eq(api, "workers-ai-systemone");
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

/* judgment-bridge.ts: callers speak `bool`, the wires `noul` (owned copy) */
static json_t *wire_questions(const json_t *questions) {
    json_t *w = json_clone(questions);
    const char *id;
    json_t *q;
    json_obj_foreach(w, i, id, q) if (str_eq(json_get_str(q, "type"), "bool")) json_set_str(q, "type", "noul");
    return w;
}

/* openai-decisions.ts encodeQuestion; NULL with *err for a box question, which the route has no type for */
static json_t *decisions_question(const char *name, const json_t *q, char **err) {
    const char *type = json_get_str(q, "type");
    const char *instr = json_get_str(q, "instructions");
    const json_t *crit = json_get(q, "criteria");
    json_t *o = json_obj();
    if (str_eq(type, "noul")) {
        json_set_str(o, "type", "predicate");
        json_set_str(o, "name", name);
        const char *yes = json_get_str(crit, "true"), *no = json_get_str(crit, "false");
        sb_t b = SB_INIT;
        sb_append(&b, instr ? instr : "");
        if (json_is_obj(crit)) { /* openai-decisions-predicate.md */
            if (yes && *yes) sb_appendf(&b, "\n\nTrue means: %s", yes);
            if (no && *no) sb_appendf(&b, "\n\nFalse means: %s", no);
        }
        json_set(o, "instructions", json_str_own(sb_detach(&b)));
    } else if (str_eq(type, "choice")) {
        json_set_str(o, "type", "choice");
        json_set_str(o, "name", name);
        json_set_str(o, "instructions", instr ? instr : "");
        json_t *choices = json_arr();
        const char *value;
        json_t *d;
        json_obj_foreach(crit, i, value, d) {
            json_t *c = json_obj();
            json_set_str(c, "value", value);
            if (json_is_str(d)) json_set_str(c, "description", json_sv(d));
            json_push(choices, c);
        }
        json_set(o, "choices", choices);
    } else if (str_eq(type, "score")) {
        json_set_str(o, "type", "score");
        json_set_str(o, "name", name);
        json_set_str(o, "instructions", instr ? instr : "");
        json_t *levels = json_arr(), *l;
        json_arr_foreach(crit, i, l) {
            json_t *lv = json_obj();
            json_set_str(lv, "label", json_sv(l) ? json_sv(l) : "");
            json_push(levels, lv);
        }
        json_set(o, "levels", levels);
    } else {
        *err = xasprintf("OpenAI Decisions has no %s question type (question \"%s\")", type ? type : "?", name);
        json_free(o);
        return NULL;
    }
    return o;
}

/* openai-decisions.ts probabilityMap: `[{value, probability}]` keyed by the stringified value; NULL when malformed */
static json_t *decisions_probabilities(const json_t *v) {
    if (!json_is_arr(v)) return NULL;
    json_t *map = json_obj(), *e;
    json_arr_foreach(v, i, e) {
        const json_t *val = json_get(e, "value");
        if (!json_is_num(json_get(e, "probability")) || !val || json_is_null(val) || json_is_obj(val) || json_is_arr(val)) {
            json_free(map);
            return NULL;
        }
        char *key = json_is_str(val) ? xstrdup(json_sv(val)) : json_dump(val, 0);
        json_set(map, key, json_clone(json_get(e, "probability")));
        free(key);
    }
    return map;
}

/* openai-decisions.ts decode: the route's answers array as the System One answers object, in place. A refused or
 * malformed answer stays missing, which the caller reports for its question. */
static void decisions_decode(json_t *resp) {
    json_t *arr = json_get_arr(resp, "answers");
    json_t *answers = json_obj(), *e;
    json_arr_foreach(arr, i, e) {
        const char *name = json_get_str(e, "name"), *type = json_get_str(e, "type");
        if (!name) continue;
        json_t *a = NULL;
        if (str_eq(type, "predicate") && json_is_num(json_get(e, "probability"))) {
            a = json_obj();
            json_set_str(a, "type", "noul");
            json_set(a, "noul", json_clone(json_get(e, "probability")));
        } else if (str_eq(type, "choice") || str_eq(type, "score")) {
            json_t *p = decisions_probabilities(json_get(e, "probabilities"));
            const json_t *v = json_get(e, type);
            bool ok = p && json_is_num(json_get(e, "confidence")) && (str_eq(type, "choice") ? (json_is_str(v) || json_is_bool(v)) : json_is_num(v));
            if (ok) {
                a = json_obj();
                json_set_str(a, "type", type);
                if (str_eq(type, "choice")) json_set(a, "choice", json_is_str(v) ? json_clone(v) : json_str_own(json_dump(v, 0)));
                else json_set(a, "score", json_clone(v));
                json_set(a, "probabilities", p);
                json_set(a, "confidence", json_clone(json_get(e, "confidence")));
            } else {
                json_free(p);
            }
        }
        if (a) json_set(answers, name, a);
    }
    json_set(resp, "answers", answers);
}

json_t *ai_native_judge(const ai_model_t *m, const char *key, const json_t *state, const json_t *questions,
                        const json_t *images, volatile sig_atomic_t *cancel, double *cost, int64_t tokens[2], char **model_out, char **err) {
    json_t *wire_q = wire_questions(questions);
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
        json_free(wire_q);
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
        json_set(body, "questions", json_clone(wire_q));
        if (want_images) {
            json_t *arr = json_arr(), *img;
            json_arr_foreach(images, i, img) {
                char *d = data_url(img);
                json_push_str(arr, d);
                free(d);
            }
            json_set(body, "images", arr);
        }
    } else if (str_eq(api, "openai-decisions")) {
        url = xasprintf("%s/decisions", base);
        token = xstrdup(key);
        char *st = json_is_str(state) ? xstrdup(json_sv(state)) : json_dump(state, 0);
        json_set_str(body, "model", wire_model);
        if (want_images) {
            json_t *in = json_arr(), *msg = json_obj(), *parts = json_arr(), *txt = json_obj(), *img;
            json_set_str(txt, "type", "input_text");
            json_set_str(txt, "text", st);
            json_push(parts, txt);
            json_arr_foreach(images, i, img) {
                json_t *p = json_obj();
                char *d = data_url(img);
                json_set_str(p, "type", "input_image");
                json_set_str(p, "image_url", d);
                free(d);
                const char *det = json_get_str(img, "detail");
                if (det) json_set_str(p, "detail", det);
                json_push(parts, p);
            }
            json_set_str(msg, "role", "user");
            json_set(msg, "content", parts);
            json_push(in, msg);
            json_set(body, "input", in);
        } else {
            json_set_str(body, "input", st);
        }
        free(st);
        json_t *qs = json_arr();
        const char *qn;
        json_t *qv;
        json_obj_foreach(wire_q, qi, qn, qv) {
            json_t *eq = decisions_question(qn, qv, err);
            if (!eq) break;
            json_push(qs, eq);
        }
        json_set(body, "questions", qs);
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
        json_set(body, "questions", json_clone(wire_q));
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
        json_set(body, "questions", json_clone(wire_q));
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
            if (str_eq(api, "openai-decisions") && json_get_arr(resp, "answers")) decisions_decode(resp);
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
                    bool is_bool = str_eq(type, "bool");
                    json_t *a = json_get(ans, qid);
                    if (!json_is_obj(a) || !str_eq(json_get_str(a, "type"), is_bool ? "noul" : type)) {
                        *err = xasprintf("%s response is missing a \"%s\" answer for question \"%s\"", label, type ? type : "?", qid);
                        break;
                    }
                    json_t *ac = json_clone(a);
                    if (is_bool) {
                        json_set_str(ac, "type", "bool");
                        json_set(ac, "bool", json_clone(json_get(a, "noul")));
                        json_del(ac, "noul");
                    }
                    json_set(answers, qid, ac);
                }
                if (*err) {
                    json_free(answers);
                    answers = NULL;
                } else {
                    const json_t *usage = json_get_obj(res, "usage");
                    double in = usage_num(usage, "input_tokens", 0);
                    double c = usage_num(usage, "cost", -1);
                    if (cost) *cost += c >= 0 ? c : in * m->cost_input / 1e6;
                    if (tokens) {
                        tokens[0] = (int64_t)in;
                        tokens[1] = (int64_t)usage_num(usage, "output_tokens", 0);
                    }
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
    json_free(wire_q);
    free(url);
    free(token);
    free(label);
    free(base);
    return answers;
}
