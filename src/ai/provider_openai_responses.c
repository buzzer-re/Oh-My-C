/*
 * provider_openai_responses.c - OpenAI Responses API streaming. Handles api "openai-responses",
 * "azure-openai-responses" (api-key header + api-version query, deployment model names) and
 * "openai-codex-responses" (ChatGPT backend, JWT account header, store:false, instructions).
 * Port of openai-responses.ts / azure-openai-responses.ts / openai-codex-responses.ts (SSE transport)
 * and the processResponsesStream decoder in openai-shared.ts.
 */
#include "ai/compat_detect.h"
#include "ai/provider_openai.h"
#include "ai/responses_ws.h"

#include <ctype.h>
#include <math.h>

enum { V_OPENAI = 0, V_AZURE = 1, V_CODEX = 2 };

#define CODEX_BASE_URL "https://chatgpt.com/backend-api"
#define CODEX_CLIENT_VERSION "0.159.0"
#define CODEX_ORIGINATOR "omp" /* upstream OPENAI_HEADER_VALUES.ORIGINATOR_CODEX */
#define NON_VISION_PLACEHOLDER "[image omitted: model does not support vision]"

static int variant_of(const ai_model_t *m) {
    if (str_eq(m->api, "azure-openai-responses")) return V_AZURE;
    if (str_eq(m->api, "openai-codex-responses")) return V_CODEX;
    return V_OPENAI;
}

/* responsesToolNaming + encodeResponsesToolName: the wire name carries compat.toolNamePrefix unless the prefixed name
 * would pass the host's tool-call-name-max-length; without a prefix the name goes out unchanged. Owned. */
static char *wire_tool_name(const ai_model_t *m, const char *name) {
    const char *prefix = model_compat_str(m, "toolNamePrefix");
    if (!name) name = "";
    if (str_empty(prefix)) return xstrdup(name);
    size_t len = strlen(prefix) + strlen(name);
    const json_t *max = json_get(m->raw, "toolCallNameMaxLength"); /* a models.yml row may set it */
    if (!json_is_num(max)) max = compat_catalog_policy(m->provider, m->api, m->id, "toolCallNameMaxLength");
    if (json_is_num(max) && (int64_t)len > (int64_t)(max->is_int ? max->u.num.i : max->u.num.d)) return xstrdup(name);
    return xasprintf("%s%s", prefix, name);
}

static bool blank(const char *s) {
    if (!s) return true;
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return false;
    return true;
}

static char *trunc_item_id(const char *id, const char *prefix) {
    if (strlen(id) <= 64) return xstrdup(id);
    char *h = oai_hash36(id);
    char *r = xasprintf("%s_%s", prefix, h);
    free(h);
    return r;
}

static char *sanitize_call_id(const char *id) {
    sb_t b = SB_INIT;
    for (const char *p = id; *p && b.len < 64; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        sb_putc(&b, ok ? c : '_');
    }
    return sb_detach(&b);
}

/* normalizeResponsesToolCallId: "{call_id}|{item_id}" -> parts (owned) */
static void norm_resp_id(const char *id, char **call, char **item) {
    if (!id) id = "";
    size_t sep = strcspn(id, "|\n");
    if (id[sep] && sep > 0) {
        char *c = xstrndup(id, sep);
        *call = sanitize_call_id(c);
        free(c);
        const char *raw = id + sep + 1;
        size_t pl = 0;
        if ((raw[0] >= 'a' && raw[0] <= 'z') || (raw[0] >= 'A' && raw[0] <= 'Z')) {
            while (raw[pl] && ((raw[pl] >= 'a' && raw[pl] <= 'z') || (raw[pl] >= 'A' && raw[pl] <= 'Z') ||
                               (raw[pl] >= '0' && raw[pl] <= '9')))
                pl++;
            if (raw[pl] != '_') pl = 0;
        }
        bool allowed = (pl == 2 && strncmp(raw, "fc", 2) == 0) || (pl == 3 && strncmp(raw, "fcr", 3) == 0);
        if (allowed) {
            char *pre = xstrndup(raw, pl);
            *item = trunc_item_id(raw, pre);
            free(pre);
        } else {
            char *h = oai_hash36(raw);
            *item = xasprintf("fc_%s", h);
            free(h);
        }
        return;
    }
    *call = sanitize_call_id(id);
    char *h = oai_hash36(id);
    *item = xasprintf("fc_%s", h);
    free(h);
}

static char *encode_tool_id(const char *call_id, const char *item_id) {
    if (!str_empty(item_id)) return xasprintf("%s|%s", call_id ? call_id : "", item_id);
    char *h = oai_hash36(call_id ? call_id : "");
    char *r = xasprintf("%s|fc_%s", call_id ? call_id : "", h);
    free(h);
    return r;
}

/* ====================================================================== */
/* input conversion                                                        */
/* ====================================================================== */

static json_t *input_text(const char *t) {
    json_t *p = json_obj();
    json_set_str(p, "type", "input_text");
    json_set_str(p, "text", t);
    return p;
}

static json_t *input_image(const ai_model_t *m, const json_t *img) {
    json_t *p = json_obj();
    json_set_str(p, "type", "input_image");
    const char *detail = json_get_str(img, "detail");
    if (!detail || (str_eq(detail, "original") && !model_compat_bool(m, "supportsImageDetailOriginal", false)))
        detail = "auto";
    json_set_str(p, "detail", detail);
    json_set(p, "image_url", json_str_own(oai_image_url(img)));
    return p;
}

static json_t *input_content(const ai_model_t *m, const json_t *content) {
    json_t *out = json_arr();
    if (json_is_str(content)) {
        if (!blank(json_sv(content))) json_push(out, input_text(json_sv(content)));
        return out;
    }
    bool images_ok = m->input_image && !model_compat_bool(m, "stripImageInput", false);
    bool omitted = false;
    json_t *b;
    json_arr_foreach(content, i, b) {
        if (str_eq(json_get_str(b, "type"), "text") && !blank(json_get_str(b, "text")))
            json_push(out, input_text(json_get_str(b, "text")));
    }
    json_arr_foreach(content, j, b) {
        if (!str_eq(json_get_str(b, "type"), "image")) continue;
        if (images_ok) json_push(out, input_image(m, b));
        else omitted = true;
    }
    if (omitted) json_push(out, input_text(NON_VISION_PLACEHOLDER));
    return out;
}

/* stripOpenAIResponsesInternalMetadataForReplay: captured output items can carry it; Codex rejects it on input */
#define INTERNAL_METADATA_KEY "internal_chat_message_metadata_passthrough"

static json_t *parse_reasoning_item(const char *sig) {
    if (!sig || sig[0] != '{') return NULL;
    json_t *j = json_parse_str(sig);
    if (!json_is_obj(j) || !str_eq(json_get_str(j, "type"), "reasoning") || !json_get_str(j, "id")) {
        json_free(j);
        return NULL;
    }
    return j;
}

/* parseTextSignature: v1 JSON {v:1,id,phase?} or legacy plain id */
static void parse_text_sig(const char *sig, char **id, char **phase) {
    *id = NULL;
    *phase = NULL;
    if (str_empty(sig)) return;
    if (sig[0] == '{') {
        json_t *j = json_parse_str(sig);
        if (json_is_obj(j) && json_get_int(j, "v", 0) == 1 && json_get_str(j, "id")) {
            *id = xstrdup(json_get_str(j, "id"));
            const char *ph = json_get_str(j, "phase");
            if (str_eq(ph, "commentary") || str_eq(ph, "final_answer")) *phase = xstrdup(ph);
            json_free(j);
            return;
        }
        json_free(j);
    }
    *id = xstrdup(sig);
}

static void convert_assistant(json_t *out, const json_t *msg, const ai_model_t *m, size_t msg_index,
                              bool include_sigs, bool preserve_ids, strvec_t *known, bool wire_names) {
    const char *stop = json_get_str(msg, "stopReason");
    bool errored = str_eq(stop, "error");
    json_t *content = json_get(msg, "content");
    json_t *b;
    bool replayable = false;
    if (include_sigs && !errored) {
        json_arr_foreach(content, i, b) {
            if (!str_eq(json_get_str(b, "type"), "thinking")) continue;
            json_t *it = parse_reasoning_item(json_get_str(b, "thinkingSignature"));
            if (it) replayable = true;
            json_free(it);
        }
    }
    bool diff_model = !str_eq(json_get_str(msg, "model"), m->id) && str_eq(json_get_str(msg, "provider"), m->provider) &&
                      str_eq(json_get_str(msg, "api"), m->api);
    int unsigned_text = 0;
    json_arr_foreach(content, j, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "thinking")) {
            if (errored || !include_sigs) continue;
            json_t *it = parse_reasoning_item(json_get_str(b, "thinkingSignature"));
            if (it) json_push(out, it);
        } else if (str_eq(t, "text")) {
            char *id, *phase;
            parse_text_sig(json_get_str(b, "textSignature"), &id, &phase);
            if (!id) {
                if (replayable) {
                    id = unsigned_text == 0 ? xasprintf("msg_%zu", msg_index)
                                            : xasprintf("msg_%zu_%d", msg_index, unsigned_text);
                    unsigned_text++;
                }
            } else if (!preserve_ids && !replayable) {
                free(id);
                id = NULL;
            } else {
                char *tr = trunc_item_id(id, "msg");
                free(id);
                id = tr;
            }
            json_t *item = json_obj();
            json_set_str(item, "type", "message");
            json_set_str(item, "role", "assistant");
            json_t *parts = json_arr();
            json_t *ot = json_obj();
            json_set_str(ot, "type", "output_text");
            json_set_str(ot, "text", json_get_str(b, "text") ? json_get_str(b, "text") : "");
            json_set(ot, "annotations", json_arr());
            json_push(parts, ot);
            json_set(item, "content", parts);
            json_set_str(item, "status", "completed");
            if (id) json_set(item, "id", json_str_own(id));
            if (phase) json_set(item, "phase", json_str_own(phase));
            json_push(out, item);
        } else if (str_eq(t, "toolCall")) {
            char *call, *item_id;
            norm_resp_id(json_get_str(b, "id"), &call, &item_id);
            bool fc_prefix = str_starts(item_id, "fc_") || str_starts(item_id, "fcr_") || str_starts(item_id, "ctc_");
            if (fc_prefix && (!replayable || diff_model)) {
                free(item_id);
                item_id = NULL;
            }
            if (!strvec_contains(known, call)) strvec_push(known, call);
            json_t *item = json_obj();
            json_set_str(item, "type", "function_call");
            if (item_id) json_set(item, "id", json_str_own(item_id));
            json_set(item, "call_id", json_str_own(call));
            /* rebuilt calls go out under the wire name; the declared tool list uses the same rule */
            json_set(item, "name", wire_names ? json_str_own(wire_tool_name(m, json_get_str(b, "name")))
                                              : json_str(json_get_str(b, "name") ? json_get_str(b, "name") : ""));
            json_t *args = json_get(b, "arguments");
            json_set(item, "arguments", args ? json_str_own(json_dump(args, 0)) : json_str("null"));
            json_push(out, item);
        }
    }
}

static json_t *assistant_note(const char *text) {
    json_t *it = json_obj();
    json_set_str(it, "type", "message");
    json_set_str(it, "role", "assistant");
    json_set_str(it, "content", text);
    return it;
}

static void convert_tool_result(json_t *out, const json_t *tr, const ai_model_t *m, bool strict_pairing,
                                const strvec_t *known) {
    bool images_ok = m->input_image && !model_compat_bool(m, "stripImageInput", false);
    sb_t txt = SB_INIT;
    bool has_img = false, first = true;
    json_t *content = json_get(tr, "content");
    json_t *b;
    json_arr_foreach(content, i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "text")) {
            if (!first) sb_putc(&txt, '\n');
            sb_append(&txt, json_get_str(b, "text") ? json_get_str(b, "text") : "");
            first = false;
        } else if (str_eq(t, "image")) {
            has_img = true;
        }
    }
    if (json_is_str(content)) sb_append(&txt, json_sv(content));
    char *text;
    if (has_img && !images_ok) text = txt.len ? xasprintf("%s\n" NON_VISION_PLACEHOLDER, txt.s) : xstrdup(NON_VISION_PLACEHOLDER);
    else text = xstrdup(txt.len ? txt.s : has_img ? "(see attached image)" : "");
    sb_free(&txt);
    char *call, *item;
    norm_resp_id(json_get_str(tr, "toolCallId"), &call, &item);
    free(item);
    if (strict_pairing && !strvec_contains(known, call)) {
        const char *name = json_get_str(tr, "toolName");
        size_t tl = strlen(text);
        char *note = tl > 16000 ? xasprintf("[Orphan %s result; call_id=%s]: %.16000s\n...[truncated]", name ? name : "tool", call, text)
                                : xasprintf("[Orphan %s result; call_id=%s]: %s", name ? name : "tool", call, text);
        json_push(out, assistant_note(note));
        free(note);
        free(text);
        free(call);
        return;
    }
    json_t *o = json_obj();
    json_set_str(o, "type", "function_call_output");
    json_set(o, "call_id", json_str_own(call));
    if (has_img && images_ok) {
        json_t *arr = json_arr();
        json_arr_foreach(content, j, b) {
            const char *t = json_get_str(b, "type");
            if (str_eq(t, "image")) json_push(arr, input_image(m, b));
            else if (str_eq(t, "text")) json_push(arr, input_text(json_get_str(b, "text") ? json_get_str(b, "text") : ""));
        }
        json_set(o, "output", arr);
        free(text);
    } else {
        json_set(o, "output", json_str_own(text));
    }
    json_push(out, o);
}

static json_t *convert_input(const ai_model_t *m, const ai_context_t *ctx, int variant, bool wire_names) {
    json_t *out = json_arr();
    if (variant == V_AZURE) {
        const char *role = m->reasoning && model_compat_bool(m, "supportsDeveloperRole", false) ? "developer" : "system";
        json_t *sys = oai_system_prompts(ctx), *s;
        json_arr_foreach(sys, i, s) {
            json_t *it = json_obj();
            json_set_str(it, "role", role);
            json_set_str(it, "content", json_sv(s));
            json_push(out, it);
        }
        json_free(sys);
    }
    bool include_sigs = variant != V_OPENAI || !model_compat_bool(m, "filterReasoningHistory", false);
    bool preserve_ids = variant == V_AZURE;
    bool strict_pairing = variant == V_AZURE || model_compat_bool(m, "strictResponsesPairing", false);
    strvec_t known = {0};
    json_t *msgs = ai_transform_messages(ctx->messages, m);
    json_t *msg;
    json_arr_foreach(msgs, idx, msg) {
        const char *role = json_get_str(msg, "role");
        if (str_eq(role, "user") || str_eq(role, "developer")) {
            /* a remote compaction's replacement history replays verbatim for its own provider
             * (getOpenAIResponsesHistoryItems on a compaction summary) */
            const json_t *pp = json_get_obj(msg, "providerPayload");
            const char *pprov = json_get_str(pp, "provider");
            if (str_eq(json_get_str(pp, "type"), "openaiResponsesHistory") && json_is_arr(json_get(pp, "items")) &&
                (!pprov || str_eq(pprov, m->provider))) {
                json_t *it;
                json_arr_foreach(json_get(pp, "items"), k, it) {
                    /* sanitizeOpenAIResponsesHistoryItemsForReplay: item ids dropped (computer calls keep theirs),
                     * reasoning reduced to its replayable fields, output-only statuses stripped */
                    if (!json_is_obj(it) || str_eq(json_get_str(it, "type"), "item_reference")) continue;
                    json_t *c;
                    if (str_eq(json_get_str(it, "type"), "reasoning")) {
                        c = json_obj();
                        json_set_str(c, "type", "reasoning");
                        if (json_is_arr(json_get(it, "summary"))) json_set(c, "summary", json_clone(json_get(it, "summary")));
                        if (json_is_arr(json_get(it, "content"))) json_set(c, "content", json_clone(json_get(it, "content")));
                        if (json_get(it, "encrypted_content")) json_set(c, "encrypted_content", json_clone(json_get(it, "encrypted_content")));
                    } else {
                        c = json_clone(it);
                        if (!str_eq(json_get_str(it, "type"), "computer_call")) json_del(c, "id");
                        json_del(c, "status");
                        json_del(c, INTERNAL_METADATA_KEY);
                    }
                    if (str_eq(json_get_str(c, "type"), "function_call") || str_eq(json_get_str(c, "type"), "custom_tool_call"))
                        if (json_get_str(c, "call_id")) strvec_push(&known, json_get_str(c, "call_id"));
                    json_push(out, c);
                }
                continue;
            }
            json_t *c = input_content(m, json_get(msg, "content"));
            if (json_len(c) == 0) {
                json_free(c);
                continue;
            }
            json_t *it = json_obj();
            if (variant == V_CODEX) {
                json_set_str(it, "role", role);
                json_set(it, "content", c);
            } else if (variant == V_AZURE && str_eq(role, "developer") && json_is_str(json_get(msg, "content"))) {
                json_set_str(it, "role", "user");
                json_set(it, "content", json_clone(json_get(msg, "content")));
                json_free(c);
            } else {
                json_set_str(it, "role", "user");
                json_set(it, "content", c);
            }
            json_push(out, it);
        } else if (str_eq(role, "assistant")) {
            convert_assistant(out, msg, m, idx, include_sigs, preserve_ids, &known, wire_names);
        } else if (str_eq(role, "toolResult")) {
            convert_tool_result(out, msg, m, strict_pairing, &known);
        }
    }
    json_free(msgs);
    strvec_free(&known);
    if (variant == V_CODEX) {
        /* store:false backend: drop item ids; backend-only metadata Codex rejects as an unknown input parameter */
        json_t *it;
        json_arr_foreach(out, i, it) {
            if (!str_eq(json_get_str(it, "type"), "computer_call")) json_del(it, "id");
            json_del(it, INTERNAL_METADATA_KEY);
        }
    }
    return out;
}

json_t *oair_convert_input(const ai_model_t *m, const ai_context_t *ctx, int variant) {
    return convert_input(m, ctx, variant, false);
}

/* ====================================================================== */
/* request                                                                 */
/* ====================================================================== */

static json_t *tool_params(const json_t *t) {
    json_t *p = json_get(t, "parameters");
    if (json_is_obj(p)) return json_clone(p);
    json_t *o = json_obj();
    json_set_str(o, "type", "object");
    json_set(o, "properties", json_obj());
    return o;
}

static json_t *convert_tools(const ai_model_t *m, const json_t *tools, int variant) {
    bool no_strict = env_truthy("PI_NO_STRICT");
    bool strict_mode = !no_strict && model_compat_bool(m, "supportsStrictMode", false);
    json_t *out = json_arr();
    json_t *t;
    json_arr_foreach(tools, i, t) {
        json_t *w = json_obj();
        json_set_str(w, "type", "function");
        json_set(w, "name", json_str_own(wire_tool_name(m, json_get_str(t, "name"))));
        json_set_str(w, "description", json_get_str(t, "description") ? json_get_str(t, "description") : "");
        json_t *st = json_get(t, "strict");
        bool expl_false = json_is_bool(st) && !st->u.b;
        bool want;
        if (variant == V_AZURE) want = false;
        else if (variant == V_CODEX) want = !no_strict && json_is_bool(st) && st->u.b;
        else want = strict_mode && !expl_false;
        json_t *adapted = want ? oai_strict_schema(json_get(t, "parameters")) : NULL;
        if (adapted) {
            json_set(w, "parameters", adapted);
            json_set_bool(w, "strict", true);
        } else {
            json_set(w, "parameters", tool_params(t));
            if (variant == V_AZURE) json_set_bool(w, "strict", false);
            else if (expl_false && !no_strict && (variant == V_CODEX || strict_mode)) json_set_bool(w, "strict", false);
        }
        json_push(out, w);
    }
    return out;
}

static bool tools_have(const json_t *tools, const char *name) {
    json_t *t;
    json_arr_foreach(tools, i, t) {
        if (str_eq(json_get_str(t, "name"), name)) return true;
    }
    return false;
}

/* mapOpenAIResponsesToolChoiceForTools (function tools only) */
static void apply_tool_choice(json_t *p, const ai_model_t *m, const ai_stream_opts_t *o, int variant) {
    json_t *tools = json_get(p, "tools");
    if (!o->tool_choice || json_len(tools) == 0) return;
    if (variant != V_AZURE && !model_compat_bool(m, "supportsToolChoice", true)) return;
    const char *mode = oai_tool_choice_mode(o->tool_choice);
    const char *name = oai_tool_choice_name(o->tool_choice);
    if (variant != V_AZURE && oai_tool_choice_forced(o->tool_choice) && (mode || name) &&
        !model_compat_bool(m, "supportsForcedToolChoice", true)) {
        json_set_str(p, "tool_choice", "auto");
        return;
    }
    if (mode) {
        json_set_str(p, "tool_choice", mode);
        return;
    }
    if (!name) return;
    /* the declared list carries wire names: the forced choice is matched against and sent under them */
    char *wire = wire_tool_name(m, name);
    if (!tools_have(tools, wire)) {
        free(wire);
        return;
    }
    if (variant != V_AZURE && !model_compat_bool(m, "supportsNamedToolChoice", true)) {
        json_t *f = json_arr();
        json_t *t;
        json_arr_foreach(tools, i, t) {
            if (str_eq(json_get_str(t, "name"), wire)) json_push(f, json_clone(t));
        }
        json_set(p, "tools", f);
        json_set_str(p, "tool_choice", "required");
        free(wire);
        return;
    }
    json_t *tc = json_obj();
    json_set_str(tc, "type", "function");
    json_set(tc, "name", json_str_own(wire));
    json_set(p, "tool_choice", tc);
}

/* toolNamesByWire: wire name -> declared name so a returned call dispatches to its tool; two tools sharing a wire name
 * could not be told apart, so that request is rejected before it is sent. NULL when names go out unchanged. Owned. */
json_t *oair_tool_names_by_wire(const ai_model_t *m, const json_t *tools, char **err) {
    if (str_empty(model_compat_str(m, "toolNamePrefix"))) return NULL;
    json_t *names = json_obj(), *t;
    json_arr_foreach(tools, i, t) {
        const char *name = json_get_str(t, "name") ? json_get_str(t, "name") : "";
        char *wire = wire_tool_name(m, name);
        const char *existing = json_get_str(names, wire);
        if (existing && strcmp(existing, name)) {
            *err = xasprintf("Tool names \"%s\" and \"%s\" both encode as \"%s\"", existing, name, wire);
            free(wire);
            json_free(names);
            return NULL;
        }
        json_set_str(names, wire, name);
        free(wire);
    }
    return names;
}

static void add_include(json_t *p, const char *what) {
    json_t *inc = json_get_arr(p, "include");
    if (!inc) {
        inc = json_arr();
        json_set(p, "include", inc);
    }
    json_t *e;
    json_arr_foreach(inc, i, e) {
        if (str_eq(json_sv(e), what)) return;
    }
    json_push_str(inc, what);
}

/* applyResponsesCompatPolicy */
static void apply_reasoning(json_t *p, const ai_model_t *m, const oai_policy_t *pol, const ai_stream_opts_t *o) {
    if (!pol->model_supported) return;
    if (oai_cbool(m, pol->over, "includeEncryptedReasoning", true)) add_include(p, "reasoning.encrypted_content");
    /* summary: 0 undefined, 1 explicit null */
    int summary = 0;
    if (oai_cbool(m, pol->over, "supportsReasoningSummary", true)) summary = o->hide_thinking_summary ? 1 : 0;
    else summary = pol->requested ? 1 : 0;
    if (pol->disabled) {
        if (str_eq(pol->disable_mode, "openrouter-enabled-false")) {
            json_t *r = json_obj();
            json_set_bool(r, "enabled", false);
            json_set(p, "reasoning", r);
        } else if ((str_eq(pol->disable_mode, "lowest-effort") || str_eq(pol->disable_mode, "none-effort")) &&
                   pol->wire[0] && !pol->omit_effort) {
            json_t *r = json_obj();
            json_set_str(r, "effort", pol->wire);
            json_set(p, "reasoning", r);
        }
        return;
    }
    if (!pol->requested || pol->omit_effort) return;
    json_t *r = json_obj();
    json_set_str(r, "effort", pol->wire[0] ? pol->wire : oai_effort_wire(m, pol->over, pol->requested));
    if (summary != 1) json_set_str(r, "summary", "auto");
    json_set(p, "reasoning", r);
}

static void apply_reasoning_mode(json_t *p, const ai_model_t *m) {
    const char *mode = json_get_str(m->raw, "reasoningMode");
    if (!mode) return;
    json_t *r = json_get_obj(p, "reasoning");
    if (!r) {
        r = json_obj();
        json_set(p, "reasoning", r);
    }
    json_set_str(r, "mode", mode);
}

static void apply_sampling(json_t *p, const ai_model_t *m, const ai_stream_opts_t *o) {
    int64_t cap = oai_output_tokens(m, o, model_compat_bool(m, "clampOutputToModelMax", false));
    if (cap > 0) json_set_int(p, "max_output_tokens", cap);
    if (model_compat_bool(m, "supportsSamplingParams", true)) {
        if (!isnan(o->temperature)) json_set_num(p, "temperature", o->temperature);
        if (!isnan(o->top_p)) json_set_num(p, "top_p", o->top_p);
    }
    if (oai_send_service_tier(m, o->service_tier)) json_set_str(p, "service_tier", o->service_tier);
}

static char *azure_deployment(const ai_model_t *m) {
    const char *map = env_get("AZURE_OPENAI_DEPLOYMENT_NAME_MAP");
    if (map) {
        strvec_t parts = str_split(map, ",");
        char *found = NULL;
        for (size_t i = 0; i < parts.n && !found; i++) {
            char *eq = strchr(parts.v[i], '=');
            if (!eq) continue;
            *eq = 0;
            char *k = str_trim(parts.v[i]), *v = str_trim(eq + 1);
            if (*k && *v && str_eq(k, m->id)) found = xstrdup(v);
            free(k);
            free(v);
        }
        strvec_free(&parts);
        if (found) return found;
    }
    return xstrdup(m->id);
}

static json_t *build_codex(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o) {
    json_t *p = json_obj();
    const char *wire_id = json_get_str(m->raw, "requestModelId");
    json_set_str(p, "model", wire_id ? wire_id : m->id);
    json_t *input = oair_convert_input(m, ctx, V_CODEX);
    json_t *sys = oai_system_prompts(ctx);
    /* extra system prompts ride as leading developer messages */
    json_t *full = json_arr();
    for (size_t i = 1; i < json_len(sys); i++) {
        json_t *it = json_obj();
        json_set_str(it, "type", "message");
        json_set_str(it, "role", "developer");
        json_t *c = json_arr();
        json_push(c, input_text(json_sv(json_at(sys, i))));
        json_set(it, "content", c);
        json_push(full, it);
    }
    for (size_t i = 0; i < input->u.arr.n; i++) json_push(full, input->u.arr.items[i]);
    input->u.arr.n = 0; /* items moved */
    json_free(input);
    /* no visible (non-developer) input: repeat the final instruction as a user turn */
    bool visible = false;
    json_t *it;
    json_arr_foreach(full, i, it) {
        if (!str_eq(json_get_str(it, "role"), "developer")) visible = true;
    }
    const char *final_instr = NULL;
    for (size_t i = json_len(sys); i > 1 && !final_instr; i--) final_instr = json_sv(json_at(sys, i - 1));
    if (!final_instr && json_len(sys)) final_instr = json_sv(json_at(sys, 0));
    if (!visible && final_instr) {
        json_t *u = json_obj();
        json_set_str(u, "type", "message");
        json_set_str(u, "role", "user");
        json_t *c = json_arr();
        json_push(c, input_text(final_instr));
        json_set(u, "content", c);
        json_push(full, u);
    }
    json_set(p, "input", full);
    json_set_bool(p, "stream", true);
    char *key = oai_prompt_cache_key(o, 64, "pc_");
    if (key) json_set(p, "prompt_cache_key", json_str_own(key));
    if (oai_send_service_tier(m, o->service_tier)) json_set_str(p, "service_tier", o->service_tier);
    if (json_len(ctx->tools) > 0) {
        json_set(p, "tools", convert_tools(m, ctx->tools, V_CODEX));
        apply_tool_choice(p, m, o, V_CODEX);
    }
    if (json_len(sys)) json_set_str(p, "instructions", json_sv(json_at(sys, 0)));
    json_free(sys);
    json_set_bool(p, "store", false);
    /* reasoning */
    const char *effort = NULL;
    if (o->force_reasoning_off) {
        effort = "none"; /* codexReasoningControls: reasoningOff */
    } else if (o->disable_reasoning) {
        if (m->reasoning && str_eq(model_compat_str(m, "reasoningDisableMode"), "none-effort")) effort = "none";
        else effort = json_sv(json_at(json_get_arr(m->thinking, "efforts"), 0));
        if (effort && !str_eq(effort, "none")) effort = oai_effort_wire(m, NULL, effort);
    } else if (o->reasoning != EFFORT_OFF && m->reasoning) {
        effort = oai_effort_wire(m, NULL, effort_name(model_clamp_effort(m, o->reasoning)));
    }
    if (effort) {
        json_t *r = json_obj();
        json_set_str(r, "effort", effort);
        if (!o->disable_reasoning && !o->hide_thinking_summary && model_compat_bool(m, "supportsReasoningSummary", true))
            json_set_str(r, "summary", "auto");
        json_set(p, "reasoning", r);
    }
    if (!o->disable_reasoning) apply_reasoning_mode(p, m);
    if (!str_empty(o->text_verbosity)) {
        json_t *t = json_obj();
        json_set_str(t, "verbosity", o->text_verbosity);
        json_set(p, "text", t);
    }
    add_include(p, "reasoning.encrypted_content");
    return p;
}

json_t *oair_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o) {
    int variant = variant_of(m);
    if (variant == V_CODEX) return build_codex(m, ctx, o);
    json_t *p = json_obj();
    if (variant == V_AZURE) {
        json_set(p, "model", json_str_own(azure_deployment(m)));
        json_set(p, "input", oair_convert_input(m, ctx, V_AZURE));
        json_set_bool(p, "stream", true);
        char *key = oai_prompt_cache_key(o, 64, "pc_");
        if (key) json_set(p, "prompt_cache_key", json_str_own(key));
        json_set_bool(p, "store", false);
        apply_sampling(p, m, o);
        if (ctx->tools) {
            json_t *tools = convert_tools(m, ctx->tools, V_AZURE);
            if (json_len(tools)) {
                json_set(p, "tools", tools);
                apply_tool_choice(p, m, o, V_AZURE);
            } else {
                json_free(tools);
            }
        }
        oai_policy_t pol;
        oai_policy_resolve(&pol, m, o, json_get(p, "tool_choice"), json_len(json_get(p, "tools")) > 0);
        apply_reasoning(p, m, &pol, o);
        return p;
    }

    oai_policy_t ipol;
    oai_policy_resolve(&ipol, m, o, o->tool_choice, json_len(ctx->tools) > 0);
    const char *wire_id = json_get_str(m->raw, "requestModelId");
    json_set_str(p, "model", wire_id ? wire_id : m->id);
    json_t *input = convert_input(m, ctx, V_OPENAI, true);
    json_t *sys = oai_system_prompts(ctx);
    if (json_len(sys)) {
        if (ipol.model_supported && oai_cbool(m, ipol.over, "supportsDeveloperRole", false)) {
            for (size_t i = json_len(sys); i > 0; i--) {
                json_t *it = json_obj();
                json_set_str(it, "role", "developer");
                json_set_str(it, "content", json_sv(json_at(sys, i - 1)));
                /* unshift */
                json_push(input, it);
                for (size_t k = json_len(input) - 1; k > 0; k--) {
                    json_t *tmp = input->u.arr.items[k];
                    input->u.arr.items[k] = input->u.arr.items[k - 1];
                    input->u.arr.items[k - 1] = tmp;
                }
            }
        } else {
            sb_t b = SB_INIT;
            json_t *s;
            json_arr_foreach(sys, i, s) {
                if (i) sb_append(&b, "\n\n");
                sb_append(&b, json_sv(s));
            }
            json_set(p, "instructions", json_str_own(sb_detach(&b)));
        }
    }
    json_free(sys);
    json_set(p, "input", input);
    json_set_bool(p, "stream", true);
    char *key = oai_prompt_cache_key(o, 64, "pc_");
    if (key) {
        if (str_eq(o->cache_retention, "long") && model_compat_bool(m, "supportsLongPromptCacheRetention", false) &&
            !model_compat_bool(m, "supportsPromptCacheBreakpoints", false))
            json_set_str(p, "prompt_cache_retention", "24h");
        json_set(p, "prompt_cache_key", json_str_own(key));
    }
    if (model_compat_bool(m, "isOpenRouterHost", false)) {
        char *sid = oai_prompt_cache_key(o, 256, "session_");
        if (sid) json_set(p, "session_id", json_str_own(sid));
    }
    json_set_bool(p, "store", false);
    if (model_compat_bool(m, "supportsObfuscationOptOut", false)) {
        json_t *so = json_obj();
        json_set_bool(so, "include_obfuscation", false);
        json_set(p, "stream_options", so);
    }
    apply_sampling(p, m, o);
    /* settings-stream-fn.ts sends the setting to "openai-responses" and "openai-codex-responses" only */
    if (!str_empty(o->text_verbosity) && str_eq(m->api, "openai-responses") && model_compat_bool(m, "officialEndpoint", false)) {
        json_t *t = json_obj();
        json_set_str(t, "verbosity", o->text_verbosity);
        json_set(p, "text", t);
    }
    if (ctx->tools) {
        json_set(p, "tools", convert_tools(m, ctx->tools, V_OPENAI));
        apply_tool_choice(p, m, o, V_OPENAI);
    }
    if (str_eq(json_sv(json_get(p, "tool_choice")), "auto") &&
        model_compat_bool(m, "disableReasoningOnToolChoice", false) && m->reasoning && ipol.requested &&
        !o->disable_reasoning)
        json_del(p, "tool_choice");
    oai_policy_t pol;
    oai_policy_resolve(&pol, m, o, json_get(p, "tool_choice"), json_len(json_get(p, "tools")) > 0);
    if (o->force_reasoning_off) { /* applyResponsesCompatPolicy forceReasoningOff; no reasoningMode */
        json_t *r = json_obj();
        json_set_str(r, "effort", "none");
        json_set(p, "reasoning", r);
    } else {
        apply_reasoning(p, m, &pol, o);
        apply_reasoning_mode(p, m);
    }
    json_t *routing = model_compat_bool(m, "isOpenRouterHost", false) ? oai_cget(m, NULL, "openRouterRouting") : NULL;
    if (json_is_obj(routing)) json_set(p, "provider", json_clone(routing));
    return p;
}

/* ---------- URL / auth ---------- */

char *oair_codex_account_id(const char *tok) {
    if (!tok) return NULL;
    const char *d1 = strchr(tok, '.');
    const char *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d1 || !d2 || strchr(d2 + 1, '.')) return NULL;
    size_t n = (size_t)(d2 - d1 - 1);
    if (n == 0 || n > 65536) return NULL;
    char *b64 = xmalloc(n + 4);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = d1[1 + i];
        b64[k++] = c == '-' ? '+' : c == '_' ? '/' : c;
    }
    while (k % 4) b64[k++] = '=';
    b64[k] = 0;
    size_t olen = 0;
    unsigned char *raw = base64_decode(b64, k, &olen);
    free(b64);
    if (!raw) return NULL;
    json_t *j = json_parse((const char *)raw, olen, NULL);
    free(raw);
    const char *acc = json_get_str(json_get_obj(j, "https://api.openai.com/auth"), "chatgpt_account_id");
    char *r = acc ? xstrdup(acc) : NULL;
    json_free(j);
    return r;
}

char *oair_url(const ai_model_t *m) {
    int variant = variant_of(m);
    if (variant == V_CODEX) {
        char *base = oai_base_url(m, CODEX_BASE_URL);
        char *r;
        if (str_ends(base, "/codex/responses")) r = xstrdup(base);
        else if (str_ends(base, "/codex")) r = xasprintf("%s/responses", base);
        else r = xasprintf("%s/codex/responses", base);
        free(base);
        return r;
    }
    if (variant == V_AZURE) {
        char *base;
        const char *eb = env_get("AZURE_OPENAI_BASE_URL"), *res = env_get("AZURE_OPENAI_RESOURCE_NAME");
        if (eb) {
            char *t = str_trim(eb);
            base = xstrdup(t);
            free(t);
        } else if (res) {
            base = xasprintf("https://%s.openai.azure.com/openai/v1", res);
        } else {
            base = xstrdup(m->base_url ? m->base_url : "");
        }
        size_t bl = strlen(base);
        while (bl > 0 && base[bl - 1] == '/') base[--bl] = 0;
        const char *ver = env_get("AZURE_OPENAI_API_VERSION") ? env_get("AZURE_OPENAI_API_VERSION") : "v1";
        char *ev = url_encode(ver);
        char *r = xasprintf("%s/responses?api-version=%s", base, ev);
        free(ev);
        free(base);
        return r;
    }
    char *base = oai_base_url(m, env_get("OPENAI_BASE_URL") ? env_get("OPENAI_BASE_URL") : "https://api.openai.com/v1");
    char *r = xasprintf("%s/responses", base);
    free(base);
    return r;
}

static void model_headers(http_headers_t *h, const ai_model_t *m) {
    if (!json_is_obj(m->headers)) return;
    const char *k;
    json_t *v;
    json_obj_foreach(m->headers, i, k, v) if (json_is_str(v)) http_headers_set(h, k, json_sv(v));
}

/* returns false (and sets *err) when required credentials are missing */
static bool build_headers(http_headers_t *h, const ai_model_t *m, const ai_stream_opts_t *o, const json_t *body,
                          char **err) {
    int variant = variant_of(m);
    if (variant == V_AZURE) {
        char *key = !str_empty(o->api_key) ? xstrdup(o->api_key)
                    : env_get("AZURE_OPENAI_API_KEY") ? xstrdup(env_get("AZURE_OPENAI_API_KEY"))
                                                       : ai_env_api_key(m->provider);
        if (str_empty(key)) {
            free(key);
            *err = xstrdup("Azure OpenAI API key is required. Set AZURE_OPENAI_API_KEY environment variable or pass it as an argument.");
            return false;
        }
        http_headers_set(h, "api-key", key);
        free(key);
        model_headers(h, m);
        ai_apply_headers(h, NULL, o);
        return true;
    }
    char *key = oai_bearer_key(m, o);
    if (variant == V_CODEX) {
        if (str_empty(key)) {
            free(key);
            *err = xstrdup("No OpenAI Codex access token available (log in with ChatGPT first)");
            return false;
        }
        model_headers(h, m);
        ai_apply_headers(h, NULL, o);
        char *auth = xasprintf("Bearer %s", key);
        http_headers_set(h, "Authorization", auth);
        free(auth);
        char *acc = oair_codex_account_id(key);
        if (acc) http_headers_set(h, "chatgpt-account-id", acc);
        free(acc);
        const char *model = json_get_str(body, "model");
        const char *tier = json_get_str(body, "service_tier");
        char *hint = tier ? xasprintf("model=%s;tier=%s", model, tier) : xasprintf("model=%s", model);
        http_headers_set(h, "x-codex-routing-hint", hint);
        free(hint);
        /* codex-rs sends `OpenAI-Beta` only on the WebSocket handshake; HTTP carries none */
        http_headers_set(h, "originator", CODEX_ORIGINATOR);
        http_headers_set(h, "version", CODEX_CLIENT_VERSION);
        http_headers_set(h, "User-Agent", OMC_NAME "/" OMC_VERSION);
        if (!str_empty(o->session_id)) {
            http_headers_set(h, "conversation_id", o->session_id);
            http_headers_set(h, "session_id", o->session_id);
            http_headers_set(h, "x-client-request-id", o->session_id);
        }
        http_headers_set(h, "Accept", "text/event-stream");
        http_headers_set(h, "Content-Type", "application/json");
        free(key);
        return true;
    }
    model_headers(h, m);
    if (str_eq(m->provider, "openrouter")) oai_add_openrouter_headers(h);
    ai_apply_headers(h, NULL, o);
    oai_apply_inference_headers(h, m, o);
    if (key && strcmp(key, AI_NO_AUTH_SENTINEL) != 0 && !http_headers_get(h, "Authorization")) {
        char *auth = xasprintf("Bearer %s", key);
        http_headers_set(h, "Authorization", auth);
        free(auth);
    }
    free(key);
    return true;
}

/* ====================================================================== */
/* stream decoding (processResponsesStream)                                */
/* ====================================================================== */

enum { K_MESSAGE = 1, K_REASONING = 2, K_CALL = 3 };

typedef struct {
    int out_idx;
    char *item_id, *call_id, *name;
    int kind;
    int block;      /* content index, -1 until created */
    sb_t buf;       /* streamed text / thinking / argument JSON */
    strvec_t parts; /* reasoning summary parts or message text parts */
    bool args_done, closed;
} ritem_t;

struct oair_stream {
    ai_builder_t b;
    const ai_model_t *m;
    VEC(ritem_t *) items;
    char *stop;
    char *error;
    bool completed;
    json_t *wire_names; /* borrowed toolNamesByWire or NULL */
};

oair_stream_t *oair_stream_new(const ai_model_t *m, ai_event_cb cb, void *ud) {
    oair_stream_t *s = xcalloc(1, sizeof(*s));
    s->m = m;
    aib_init(&s->b, m, cb, ud);
    return s;
}

void oair_stream_set_wire_names(oair_stream_t *s, const json_t *names) { s->wire_names = (json_t *)names; }

/* the declared tool of a returned call name (toolNamesByWire); unknown names pass through */
static const char *declared_name(const oair_stream_t *s, const char *wire) {
    const char *n = s->wire_names && wire ? json_get_str(s->wire_names, wire) : NULL;
    return n ? n : wire;
}

static json_t *r_content(oair_stream_t *s) { return json_get(s->b.msg, "content"); }

static ritem_t *find_item(oair_stream_t *s, const json_t *ev, int kind) {
    const char *iid = json_get_str(ev, "item_id");
    json_t *oi = json_get(ev, "output_index");
    for (size_t i = s->items.n; i > 0; i--) {
        ritem_t *it = s->items.v[i - 1];
        if (iid && (str_eq(it->item_id, iid) || str_eq(it->call_id, iid))) return it;
        if (iid && it->call_id && str_starts(iid, "fc_") && str_eq(it->call_id, iid + 3)) return it;
    }
    if (json_is_num(oi)) {
        int idx = (int)json_get_int(ev, "output_index", -1);
        for (size_t i = s->items.n; i > 0; i--) {
            ritem_t *it = s->items.v[i - 1];
            if (!it->closed && it->out_idx == idx) return it;
        }
    }
    for (size_t i = s->items.n; i > 0; i--) {
        ritem_t *it = s->items.v[i - 1];
        if (!it->closed && (kind == 0 || it->kind == kind)) return it;
    }
    return NULL;
}

/* append streamed text/thinking for an item */
static void item_delta(oair_stream_t *s, ritem_t *it, const char *d, size_t n) {
    if (!n) return;
    sb_appendn(&it->buf, d, n);
    bool text = it->kind == K_MESSAGE;
    if (it->block >= 0 && s->b.cur_index == it->block) {
        if (text) aib_text_delta(&s->b, d, n);
        else aib_thinking_delta(&s->b, d, n);
    } else if (it->block < 0) {
        if (s->b.cur_index >= 0) aib_end_block(&s->b);
        if (text) aib_text_delta(&s->b, d, n);
        else aib_thinking_delta(&s->b, d, n);
        it->block = s->b.cur_index;
    } else {
        json_t *blk = json_at(r_content(s), (size_t)it->block);
        json_set(blk, text ? "text" : "thinking", json_strn(it->buf.s, it->buf.len));
        oai_emit(&s->b, text ? AI_EV_TEXT_DELTA : AI_EV_THINKING_DELTA, it->block, d, n, NULL);
    }
}

/* finalize a text/thinking item with authoritative text + signature */
static void item_finalize(oair_stream_t *s, ritem_t *it, const char *final, const char *sig_key, const char *sig) {
    bool text = it->kind == K_MESSAGE;
    const char *field = text ? "text" : "thinking";
    aib_start(&s->b);
    if (it->block < 0) {
        if (s->b.cur_index >= 0) aib_end_block(&s->b);
        json_t *blk = json_obj();
        json_set_str(blk, "type", text ? "text" : "thinking");
        json_set_str(blk, field, final);
        if (sig) json_set_str(blk, sig_key, sig);
        json_push(r_content(s), blk);
        it->block = (int)json_len(r_content(s)) - 1;
        oai_emit(&s->b, text ? AI_EV_TEXT_START : AI_EV_THINKING_START, it->block, NULL, 0, NULL);
        if (*final) oai_emit(&s->b, text ? AI_EV_TEXT_DELTA : AI_EV_THINKING_DELTA, it->block, final, strlen(final), NULL);
        oai_emit(&s->b, text ? AI_EV_TEXT_END : AI_EV_THINKING_END, it->block, NULL, 0, blk);
    } else {
        json_t *blk = json_at(r_content(s), (size_t)it->block);
        if (sig) json_set_str(blk, sig_key, sig);
        if (s->b.cur_index == it->block) {
            sb_clear(&s->b.buf);
            sb_append(&s->b.buf, final);
            aib_end_block(&s->b);
        } else {
            json_set_str(blk, field, final);
        }
    }
    it->closed = true;
}

static void call_finalize(oair_stream_t *s, ritem_t *it, json_t *args, const char *id) {
    if (it->block < 0) {
        aib_toolcall_start(&s->b, id, it->name ? it->name : "");
        it->block = s->b.cur_index;
    }
    json_t *blk = json_at(r_content(s), (size_t)it->block);
    if (id) json_set_str(blk, "id", id);
    if (it->name) json_set_str(blk, "name", it->name);
    if (s->b.cur_index == it->block && s->b.cur_kind == 3) {
        aib_toolcall_set_args(&s->b, args);
        aib_end_block(&s->b);
    } else {
        json_set(blk, "arguments", args);
    }
    it->closed = true;
}

static void finalize_pending_calls(oair_stream_t *s) {
    for (size_t i = 0; i < s->items.n; i++) {
        ritem_t *it = s->items.v[i];
        if (it->kind != K_CALL || it->closed) continue;
        char *id = encode_tool_id(it->call_id, it->item_id);
        call_finalize(s, it, oai_parse_args(it->buf.s, it->buf.len), id);
        free(id);
    }
}

static int64_t jint(const json_t *o, const char *k, int64_t def) { return json_is_num(json_get(o, k)) ? json_get_int(o, k, def) : def; }

static void apply_usage(oair_stream_t *s, const json_t *u) {
    if (!json_is_obj(u)) return;
    json_t *d = json_get_obj(u, "input_tokens_details"), *od = json_get_obj(u, "output_tokens_details");
    int64_t cached = jint(d, "cached_tokens", -1);
    if (cached < 0) cached = jint(u, "prompt_cache_hit_tokens", 0);
    int64_t hit = jint(u, "prompt_cache_hit_tokens", -1), miss = jint(u, "prompt_cache_miss_tokens", -1);
    oai_usage_accounting(&s->b, jint(u, "input_tokens", 0), jint(u, "output_tokens", 0), cached,
                         jint(od, "reasoning_tokens", 0), jint(d, "cache_write_tokens", -1), miss, hit >= 0 && miss >= 0);
}

static void set_stop(oair_stream_t *s, const char *stop) {
    free(s->stop);
    s->stop = xstrdup(stop);
}

static void set_error(oair_stream_t *s, char *msg) {
    free(s->error);
    s->error = msg;
}

static char *response_error_message(const json_t *resp) {
    json_t *e = json_get_obj(resp, "error");
    if (!e) e = json_get_obj(json_get_obj(resp, "status_details"), "error");
    json_t *details = json_get_obj(resp, "incomplete_details");
    if (e) {
        const char *code = json_get_str(e, "code"), *msg = json_get_str(e, "message");
        return xasprintf("%s: %s", code && *code ? code : "unknown", msg && *msg ? msg : "no message");
    }
    if (json_get_str(details, "reason")) return xasprintf("incomplete: %s", json_get_str(details, "reason"));
    const char *sd = json_get_str(json_get_obj(resp, "status_details"), "reason");
    if (sd && *sd) return xasprintf("status_details: %s", sd);
    return xstrdup("Unknown error (no error details in response)");
}

static bool calls_complete(oair_stream_t *s) {
    bool any = false;
    for (size_t i = 0; i < s->items.n; i++) {
        ritem_t *it = s->items.v[i];
        if (it->kind != K_CALL) continue;
        any = true;
        if (it->args_done) continue;
        json_t *j = it->buf.len ? json_parse(it->buf.s, it->buf.len, NULL) : NULL;
        bool ok = j != NULL;
        json_free(j);
        if (!ok) return false;
    }
    return any;
}

static void handle_terminal(oair_stream_t *s, const json_t *resp) {
    const char *status = json_get_str(resp, "status");
    const char *inc_reason = json_get_str(json_get_obj(resp, "incomplete_details"), "reason");
    bool promote_incomplete = str_eq(status, "incomplete") && str_eq(inc_reason, "max_output_tokens") && calls_complete(s);
    finalize_pending_calls(s);
    if (json_get_str(resp, "id")) aib_set(&s->b, "responseId", json_str(json_get_str(resp, "id")));
    apply_usage(s, json_get_obj(resp, "usage"));
    const char *stop = "stop";
    if (str_eq(status, "incomplete")) stop = "length";
    else if (str_eq(status, "failed") || str_eq(status, "cancelled")) stop = "error";
    if (str_eq(status, "failed") || str_eq(status, "cancelled")) {
        set_error(s, response_error_message(resp));
    } else if (str_eq(status, "incomplete") && str_eq(inc_reason, "content_filter")) {
        set_error(s, xstrdup("incomplete: content_filter"));
    }
    bool has_call = false;
    json_t *b;
    json_arr_foreach(r_content(s), i, b) {
        if (str_eq(json_get_str(b, "type"), "toolCall")) has_call = true;
    }
    if (has_call && (str_eq(stop, "stop") || (promote_incomplete && str_eq(stop, "length")))) stop = "toolUse";
    set_stop(s, stop);
    s->completed = true;
}

static void on_item_added(oair_stream_t *s, const json_t *ev) {
    json_t *item = json_get_obj(ev, "item");
    const char *type = json_get_str(item, "type");
    int kind = str_eq(type, "message") ? K_MESSAGE : str_eq(type, "reasoning") ? K_REASONING
               : str_eq(type, "function_call")                                ? K_CALL
                                                                              : 0;
    if (!kind) return;
    aib_start(&s->b);
    ritem_t *it = xcalloc(1, sizeof(*it));
    it->kind = kind;
    it->block = -1;
    it->out_idx = (int)json_get_int(ev, "output_index", -1);
    if (json_get_str(item, "id")) it->item_id = xstrdup(json_get_str(item, "id"));
    if (json_get_str(item, "call_id")) it->call_id = xstrdup(json_get_str(item, "call_id"));
    if (json_get_str(item, "name")) it->name = xstrdup(declared_name(s, json_get_str(item, "name")));
    vec_push(&s->items, it);
    if (kind == K_CALL) {
        char *id = encode_tool_id(it->call_id, it->item_id);
        aib_toolcall_start(&s->b, id, it->name ? it->name : "");
        free(id);
        it->block = s->b.cur_index;
        const char *a = json_get_str(item, "arguments");
        if (a && *a) {
            sb_append(&it->buf, a);
            aib_toolcall_delta(&s->b, a, strlen(a));
        }
    }
}

static void on_item_done(oair_stream_t *s, const json_t *ev) {
    json_t *item = json_get_obj(ev, "item");
    const char *type = json_get_str(item, "type");
    json_t *probe = json_obj();
    json_t *oi = json_get(ev, "output_index");
    if (oi) json_set(probe, "output_index", json_clone(oi));
    const char *iid = json_get_str(item, "id");
    if (!iid) iid = json_get_str(item, "call_id");
    if (iid) json_set_str(probe, "item_id", iid);
    int kind = str_eq(type, "message") ? K_MESSAGE : str_eq(type, "reasoning") ? K_REASONING
               : str_eq(type, "function_call")                                ? K_CALL
                                                                              : 0;
    ritem_t *it = kind ? find_item(s, probe, kind) : NULL;
    json_free(probe);
    if (!kind) return;
    if (it && it->kind != kind) it = NULL;
    ritem_t tmp = {.kind = kind, .block = -1};
    if (!it) {
        aib_start(&s->b);
        it = &tmp;
    }
    if (kind == K_REASONING) {
        sb_t fin = SB_INIT;
        json_t *part;
        json_arr_foreach(json_get_arr(item, "summary"), i, part) {
            if (i) sb_append(&fin, "\n\n");
            sb_append(&fin, json_get_str(part, "text") ? json_get_str(part, "text") : "");
        }
        if (!fin.len) {
            json_t *c0 = json_at(json_get_arr(item, "content"), 0);
            if (str_eq(json_get_str(c0, "type"), "reasoning_text")) sb_append(&fin, json_get_str(c0, "text") ? json_get_str(c0, "text") : "");
        }
        if (!fin.len) sb_append(&fin, sb_cstr(&it->buf));
        char *sig = json_dump(item, 0);
        item_finalize(s, it, sb_cstr(&fin), "thinkingSignature", sig);
        free(sig);
        sb_free(&fin);
    } else if (kind == K_MESSAGE) {
        sb_t fin = SB_INIT;
        json_t *parts = json_get_arr(item, "content"), *part;
        json_arr_foreach(parts, i, part) {
            const char *pt = json_get_str(part, "type");
            const char *tx = str_eq(pt, "refusal") ? json_get_str(part, "refusal") : json_get_str(part, "text");
            sb_append(&fin, tx ? tx : "");
        }
        if (json_len(parts) == 0) sb_append(&fin, sb_cstr(&it->buf));
        json_t *sig = json_obj();
        json_set_int(sig, "v", 1);
        json_set_str(sig, "id", json_get_str(item, "id") ? json_get_str(item, "id") : "");
        const char *phase = json_get_str(item, "phase");
        if (phase) json_set_str(sig, "phase", phase);
        char *sigs = json_dump(sig, 0);
        json_free(sig);
        item_finalize(s, it, sb_cstr(&fin), "textSignature", sigs);
        free(sigs);
        sb_free(&fin);
    } else {
        const char *a = json_get_str(item, "arguments");
        json_t *args = a && *a ? oai_parse_args(a, strlen(a)) : oai_parse_args(it->buf.s, it->buf.len);
        if (!it->name && json_get_str(item, "name")) it->name = xstrdup(declared_name(s, json_get_str(item, "name")));
        char *id = encode_tool_id(json_get_str(item, "call_id") ? json_get_str(item, "call_id") : it->call_id,
                                  json_get_str(item, "id") ? json_get_str(item, "id") : it->item_id);
        call_finalize(s, it, args, id);
        free(id);
    }
    if (it == &tmp) {
        sb_free(&tmp.buf);
        free(tmp.name);
    }
}

int oair_stream_on_sse(void *ud, const char *event, const char *data, size_t len) {
    oair_stream_t *s = ud;
    if (!len || (len == 6 && memcmp(data, "[DONE]", 6) == 0)) return 0;
    json_t *ev = json_parse(data, len, NULL);
    if (!json_is_obj(ev)) {
        json_free(ev);
        return 0;
    }
    const char *type = json_get_str(ev, "type");
    if (!type) type = event;
    int rc = 0;
    if (str_eq(type, "response.created")) {
        const char *id = json_get_str(json_get_obj(ev, "response"), "id");
        if (id) aib_set(&s->b, "responseId", json_str(id));
        aib_start(&s->b);
    } else if (str_eq(type, "response.output_item.added")) {
        on_item_added(s, ev);
    } else if (str_eq(type, "response.reasoning_summary_part.added")) {
        ritem_t *it = find_item(s, ev, K_REASONING);
        if (it && it->kind == K_REASONING) strvec_push(&it->parts, "");
    } else if (str_eq(type, "response.reasoning_summary_text.delta")) {
        ritem_t *it = find_item(s, ev, K_REASONING);
        json_t *d = json_get(ev, "delta");
        if (it && it->kind == K_REASONING && json_is_str(d) && d->u.str.len) {
            if (!it->parts.n) strvec_push(&it->parts, "");
            char **last = &it->parts.v[it->parts.n - 1];
            char *nl = xasprintf("%s%s", *last, d->u.str.s);
            free(*last);
            *last = nl;
            item_delta(s, it, d->u.str.s, d->u.str.len);
        }
    } else if (str_eq(type, "response.reasoning_summary_text.done")) {
        ritem_t *it = find_item(s, ev, K_REASONING);
        const char *text = json_get_str(ev, "text");
        if (it && it->kind == K_REASONING && text) {
            const char *prev = it->parts.n ? it->parts.v[it->parts.n - 1] : "";
            if (str_starts(text, prev) && strlen(text) > strlen(prev)) {
                const char *suffix = text + strlen(prev);
                item_delta(s, it, suffix, strlen(suffix));
            }
            if (!it->parts.n) strvec_push(&it->parts, text);
            else {
                free(it->parts.v[it->parts.n - 1]);
                it->parts.v[it->parts.n - 1] = xstrdup(text);
            }
        }
    } else if (str_eq(type, "response.reasoning_summary_part.done")) {
        ritem_t *it = find_item(s, ev, K_REASONING);
        if (it && it->kind == K_REASONING && it->parts.n) item_delta(s, it, "\n\n", 2);
    } else if (str_eq(type, "response.reasoning_text.delta") || str_eq(type, "response.output_text.delta") ||
               str_eq(type, "response.refusal.delta")) {
        int kind = str_eq(type, "response.reasoning_text.delta") ? K_REASONING : K_MESSAGE;
        ritem_t *it = find_item(s, ev, kind);
        json_t *d = json_get(ev, "delta");
        if (it && it->kind == kind && json_is_str(d)) item_delta(s, it, d->u.str.s, d->u.str.len);
    } else if (str_eq(type, "response.output_text.done") || str_eq(type, "response.refusal.done")) {
        ritem_t *it = find_item(s, ev, K_MESSAGE);
        const char *text = json_get_str(ev, str_eq(type, "response.refusal.done") ? "refusal" : "text");
        if (it && it->kind == K_MESSAGE && text) {
            const char *cur = sb_cstr(&it->buf);
            if (str_starts(text, cur) && strlen(text) > it->buf.len) {
                const char *suffix = text + it->buf.len;
                item_delta(s, it, suffix, strlen(suffix));
            }
        }
    } else if (str_eq(type, "response.function_call_arguments.delta")) {
        ritem_t *it = find_item(s, ev, K_CALL);
        json_t *d = json_get(ev, "delta");
        if (it && it->kind == K_CALL && !it->args_done && json_is_str(d) && d->u.str.len) {
            sb_appendn(&it->buf, d->u.str.s, d->u.str.len);
            if (it->block >= 0 && s->b.cur_index == it->block && s->b.cur_kind == 3) {
                aib_toolcall_delta(&s->b, d->u.str.s, d->u.str.len);
            } else if (it->block >= 0) {
                json_t *pa = json_parse_partial(it->buf.s, it->buf.len);
                if (json_is_obj(pa)) json_set(json_at(r_content(s), (size_t)it->block), "arguments", pa);
                else json_free(pa);
                oai_emit(&s->b, AI_EV_TOOLCALL_DELTA, it->block, d->u.str.s, d->u.str.len, NULL);
            }
        }
    } else if (str_eq(type, "response.function_call_arguments.done")) {
        ritem_t *it = find_item(s, ev, K_CALL);
        const char *a = json_get_str(ev, "arguments");
        if (it && it->kind == K_CALL && a && !blank(a)) {
            sb_clear(&it->buf);
            sb_append(&it->buf, a);
            it->args_done = true;
        }
    } else if (str_eq(type, "response.output_item.done")) {
        on_item_done(s, ev);
    } else if (str_eq(type, "response.completed") || str_eq(type, "response.incomplete") || str_eq(type, "response.done")) {
        handle_terminal(s, json_get_obj(ev, "response"));
        rc = OAI_STREAM_STOP;
    } else if (str_eq(type, "response.failed")) {
        json_t *resp = json_get_obj(ev, "response");
        apply_usage(s, json_get_obj(resp, "usage"));
        set_error(s, response_error_message(resp));
        set_stop(s, "error");
        rc = OAI_STREAM_STOP;
    } else if (str_eq(type, "error")) {
        json_t *e = json_get_obj(ev, "error");
        if (!e) e = ev;
        json_t *code = json_get(e, "code");
        char *cs = json_is_str(code) ? xstrdup(json_sv(code)) : json_is_num(code) ? json_dump(code, 0) : xstrdup("unknown");
        const char *msg = json_get_str(e, "message");
        set_error(s, xasprintf("Error Code %s: %s", cs, msg ? msg : "no message"));
        free(cs);
        set_stop(s, "error");
        rc = OAI_STREAM_STOP;
    }
    json_free(ev);
    return rc;
}

json_t *oair_stream_finish(oair_stream_t *s, int rc, const char *terr) {
    const char *stop = s->stop ? s->stop : "stop";
    const char *err = NULL;
    if (rc == -2) {
        stop = "aborted";
        err = terr ? terr : "Request was aborted";
    } else if (rc != 0 && rc != OAI_STREAM_STOP) {
        stop = "error";
        err = terr ? terr : "request failed";
    } else if (s->error) {
        stop = "error";
        err = s->error;
    } else if (!s->completed) {
        stop = "error";
        err = "OpenAI responses stream ended before a terminal response event was received";
    }
    if (strcmp(stop, "error") == 0 || strcmp(stop, "aborted") == 0) finalize_pending_calls(s);
    json_t *msg = aib_finish(&s->b, stop, err);
    for (size_t i = 0; i < s->items.n; i++) {
        ritem_t *it = s->items.v[i];
        free(it->item_id);
        free(it->call_id);
        free(it->name);
        sb_free(&it->buf);
        strvec_free(&it->parts);
        free(it);
    }
    vec_free(&s->items);
    free(s->stop);
    free(s->error);
    aib_free(&s->b);
    free(s);
    return msg;
}

/* ====================================================================== */
/* entry point                                                             */
/* ====================================================================== */

typedef struct {
    const ai_model_t *m;
    const ai_context_t *ctx;
    const ai_stream_opts_t *o;
    int mode;             /* ResponsesAttemptContext.mode: 0 undecided, else WS_MODE_* */
    bool retry_empty;     /* retryEmptyCompletion: an SSE attempt; a successful empty WS response submits once */
} oair_attempt_t;

enum { WS_MODE_SSE = 1, WS_MODE_WEBSOCKET = 2 };

/* resolveResponsesWebSocketPreference: capability is rule-owned, callers must opt in, an explicit false vetoes */
static bool ws_preferred(const ai_model_t *m, const ai_stream_opts_t *o) {
    const json_t *cap = compat_catalog_policy(m->provider, m->api, m->id, "supportsWebsockets");
    if (!json_is_bool(cap) || !cap->u.b) return false;
    const json_t *pref = json_get(m->raw, "preferWebsockets");
    bool model_true = json_is_bool(pref) && pref->u.b, model_false = json_is_bool(pref) && !pref->u.b;
    if (o->prefer_websockets < 0 || model_false) return false;
    return o->prefer_websockets > 0 || model_true;
}

/* isOpenAIResponsesWebSocketChainEnabled: socket-local continuation needs no stored responses, so only the explicit
 * opt-outs apply: a gateway that ignores previous_response_id would silently drop prior turns */
static bool ws_chain_enabled(const ai_model_t *m, const ai_stream_opts_t *o) {
    if (o->stateful_responses) return o->stateful_responses > 0;
    const char *env = env_get("PI_OPENAI_STATEFUL");
    if (!str_empty(env)) return env_truthy("PI_OPENAI_STATEFUL");
    const json_t *c = model_compat_get(m, "statefulResponses");
    return json_is_bool(c) ? c->u.b : true;
}

/* the WebSocket half of streamOpenAIResponsesOnce. Returns the final message, or NULL to fall back to HTTP/SSE for this
 * request (a pre-send transport failure, or an endpoint that refuses the upgrade). */
static json_t *oair_ws_attempt(oair_attempt_t *at, const json_t *body, const http_headers_t *h, oair_stream_t *st) {
    const ai_model_t *m = at->m;
    const ai_stream_opts_t *o = at->o;
    char *url = oair_url(m);
    char *rewritten = ai_rewrite_url(url);
    free(url);
    const char *session = o->provider_session_state ? o->session_id : NULL;
    if (rws_is_sse_only(rewritten, m->id, session)) {
        free(rewritten);
        at->mode = WS_MODE_SSE;
        return NULL;
    }
    int64_t first = o->first_event_timeout_ms;
    rws_lease_t *lease = rws_acquire(rewritten, m->id, session, h, MIN((int64_t)30000, first > 0 ? first : 30000), !str_empty(o->session_id));
    free(rewritten);
    int status = 0;
    char *err = NULL;
    bool caller_stop = o->cancel && *o->cancel;
    if (caller_stop) {
        rws_release(lease);
        return oair_stream_finish(st, -2, "Request was aborted");
    }
    if (rws_connect(lease, &status, &err) != 0) {
        /* a definite upgrade rejection proves the endpoint lacks WS; a network failure may be a blip */
        bool definite = status == 404 || status == 405 || status == 426 || status == 501;
        if (status && !definite) {
            rws_release(lease);
            json_t *msg = oair_stream_finish(st, -1, err);
            json_set_int(msg, "errorStatus", status);
            free(err);
            return msg;
        }
        if (definite) rws_mark_sse_only(lease);
        else rws_record_connect_failure(lease);
        LOGW("responses websocket unavailable, falling back to SSE: %s", err ? err : "connect failed");
        free(err);
        rws_release(lease);
        at->mode = WS_MODE_SSE;
        return NULL;
    }
    at->retry_empty = false;
    bool chain = ws_chain_enabled(m, o) && !rws_chain_disabled(lease);
    int64_t idle = o->idle_timeout_ms == 0 || o->first_event_timeout_ms == 0 ? 24LL * 3600 * 1000 : MAX((int64_t)300000, MAX(o->idle_timeout_ms, o->first_event_timeout_ms));
    rws_result_t r = rws_exchange(lease, body, chain, idle, o->cancel, oair_stream_on_sse, st, &status, &err);
    json_t *msg;
    switch (r) {
    case RWS_DONE:
    case RWS_STOPPED: msg = oair_stream_finish(st, 0, NULL); break;
    case RWS_ABORTED:
        /* the stream watchdog cancels too: a stream timeout is a post-send transport failure */
        if (!(*(o->caller_cancel ? o->caller_cancel : o->cancel))) {
            rws_mark_sse_only(lease);
            rws_close_socket(lease);
        }
        msg = oair_stream_finish(st, -2, "Request was aborted");
        break;
    case RWS_CLOSED:
        rws_mark_sse_only(lease);
        msg = oair_stream_finish(st, -1, "OpenAI responses stream closed before a terminal response event was received");
        break;
    case RWS_REJECTED: /* a definite pre-generation rejection: the frame already reached the decoder, the socket is fine */
        msg = oair_stream_finish(st, 0, NULL);
        if (status) json_set_int(msg, "errorStatus", status);
        break;
    default:
        msg = oair_stream_finish(st, -1, err ? err : "request failed");
        break;
    }
    free(err);
    rws_release(lease);
    return msg;
}

static json_t *oair_stream_once(void *self, ai_event_cb cb, void *ud) {
    oair_attempt_t *at = self;
    const ai_model_t *m = at->m;
    const ai_context_t *ctx = at->ctx;
    const ai_stream_opts_t *o = at->o;
    json_t *body = oair_build_request(m, ctx, o);
    if (o->on_payload) o->on_payload(o->payload_ud, body);
    http_headers_t h = {0};
    char *err = NULL;
    oair_stream_t *st = oair_stream_new(m, cb, ud);
    json_t *msg = NULL;
    json_t *wire_names = oair_tool_names_by_wire(m, ctx->tools, &err);
    oair_stream_set_wire_names(st, wire_names);
    if (!at->mode) at->mode = variant_of(m) == V_OPENAI && ws_preferred(m, o) ? WS_MODE_WEBSOCKET : WS_MODE_SSE;
    bool headers_ok = !err && build_headers(&h, m, o, body, &err);
    if (headers_ok && at->mode == WS_MODE_WEBSOCKET) {
        msg = oair_ws_attempt(at, body, &h, st);
        if (!msg) {
            /* a different transport is a different candidate: the hook replaces that body too */
            json_free(body);
            body = oair_build_request(m, ctx, o);
            if (o->on_payload) o->on_payload(o->payload_ud, body);
        }
    }
    if (msg) {
        /* answered over the socket */
    } else if (!headers_ok) {
        msg = oair_stream_finish(st, -1, err);
    } else {
        char *url = oair_url(m);
        char *payload = json_dump(body, 0);
        int status = 0;
        int rc = ai_sse_post(url, &h, payload, o, oair_stream_on_sse, st, &status, &err);
        msg = oair_stream_finish(st, rc, err);
        free(payload);
        free(url);
    }
    free(err);
    http_headers_free(&h);
    json_free(body);
    json_free(wire_names);
    return msg;
}

/* streamOpenAIResponses / streamAzureOpenAIResponses / the Codex SSE transport: 6 transport attempts
 * (postOpenAIStream, or Codex's 500 ms*(n+1) within a 5 min budget); Responses retries benign empty completions,
 * Azure one replay-safe provider error */
json_t *provider_openai_responses_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                                         ai_event_cb cb, void *ud) {
    ai_stream_opts_t lo;
    if (o) lo = *o;
    else ai_stream_opts_init(&lo);
    int v = variant_of(m);
    lo.retry_policy = v == V_CODEX ? AI_RETRY_CODEX : AI_RETRY_OPENAI;
    lo.max_retries = 5;
    if (v == V_CODEX) lo.max_retry_delay_ms = 5 * 60 * 1000; /* CODEX_RATE_LIMIT_BUDGET_MS */
    oair_attempt_t at = {.m = m, .ctx = ctx, .o = &lo, .retry_empty = v == V_OPENAI};
    if (v == V_CODEX) return oair_stream_once(&at, cb, ud);
    return ai_replay_safe_stream_ex(oair_stream_once, &at, &lo, cb, ud, &at.retry_empty, v == V_AZURE ? 1 : 0);
}

/* ====================================================================== */
/* remote compaction (agent compaction/openai.ts V1)                       */
/* ====================================================================== */

/* resolveOpenAiCompactEndpoint */
static char *compact_endpoint(const ai_model_t *m) {
    const json_t *rc = json_get_obj(m->raw, "remoteCompaction");
    const char *ep = json_get_str(rc, "endpoint");
    const char *api = json_get_str(rc, "api") ? json_get_str(rc, "api") : m->api;
    if (ep && *ep) return xstrdup(ep);
    if (str_eq(m->provider, "openai-codex") || str_eq(api, "openai-codex-responses")) {
        char *base = xstrdup(m->base_url && *m->base_url ? m->base_url : CODEX_BASE_URL);
        size_t n = strlen(base);
        while (n && base[n - 1] == '/') base[--n] = 0;
        const char *codex = strstr(base, "/codex");
        bool ends_codex = codex && (!codex[6] || (codex[6] == '/' && codex[7] == 'v' && isdigit((unsigned char)codex[8])));
        char *r = ends_codex ? xasprintf("%s/responses/compact", base) : xasprintf("%s/codex/responses/compact", base);
        free(base);
        return r;
    }
    char *base = xstrdup(m->base_url && *m->base_url ? m->base_url : "https://api.openai.com/v1");
    size_t n = strlen(base);
    if (n && base[n - 1] == '/') base[--n] = 0;
    char *r = str_ends(base, "/v1") ? xasprintf("%s/responses/compact", base) : xasprintf("%s/v1/responses/compact", base);
    free(base);
    return r;
}

static const char *http_status_text(int st) {
    switch (st) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
    }
}

/* shouldKeepOpenAiCompactOutputItem */
static bool keep_compact_item(const json_t *it) {
    const char *t = json_get_str(it, "type");
    if (str_eq(t, "compaction") || str_eq(t, "compaction_summary")) return true;
    return str_eq(t, "message") && (str_eq(json_get_str(it, "role"), "assistant") || str_eq(json_get_str(it, "role"), "user"));
}

json_t *ai_openai_remote_compact(const ai_model_t *m, const char *api_key, const json_t *messages, const json_t *previous,
                                 const char *instructions, const char *session_id, volatile sig_atomic_t *cancel, char **err,
                                 int *status) {
    *err = NULL;
    if (status) *status = 0;
    const json_t *rc = json_get_obj(m->raw, "remoteCompaction");
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.api_key = api_key;
    o.session_id = session_id;
    o.cancel = cancel;
    ai_context_t ctx = {.messages = (json_t *)messages};
    int variant = variant_of(m) == V_AZURE ? V_AZURE : V_OPENAI;
    json_t *input = json_arr(), *it;
    json_arr_foreach(previous, i, it) {
        json_t *c = json_clone(it);
        json_del(c, INTERNAL_METADATA_KEY);
        json_push(input, c);
    }
    json_t *conv = oair_convert_input(m, &ctx, variant);
    json_arr_foreach(conv, i, it) {
        json_t *c;
        if (!json_get_str(it, "type") && json_get_str(it, "role")) {
            /* native history items are typed messages */
            c = json_obj();
            json_set_str(c, "type", "message");
            const char *k;
            json_t *v;
            json_obj_foreach(it, x, k, v) json_set(c, k, json_clone(v));
        } else c = json_clone(it);
        json_del(c, "status"); /* stripOpenAIResponsesOutputOnlyStatusesForReplay */
        json_del(c, INTERNAL_METADATA_KEY);
        json_push(input, c);
    }
    json_free(conv);
    json_t *body = json_obj();
    const char *model = json_get_str(rc, "model") ? json_get_str(rc, "model")
                        : json_get_str(m->raw, "requestModelId") ? json_get_str(m->raw, "requestModelId")
                                                                 : m->id;
    json_set_str(body, "model", model);
    json_set(body, "input", input);
    json_set_str(body, "instructions", instructions ? instructions : "");
    char *key = oai_prompt_cache_key(&o, 64, "pc_");
    if (key) json_set(body, "prompt_cache_key", json_str_own(key));
    http_headers_t h = {0};
    json_t *out = NULL;
    if (!build_headers(&h, m, &o, body, err)) goto done;
    http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "Accept", "*/*");
    char *url = compact_endpoint(m);
    char *payload = json_dump(body, 0);
    http_req_t req = {.method = "POST", .url = url, .headers = &h, .body = payload, .body_len = strlen(payload),
                      .timeout_ms = 300000, .cancel = cancel};
    http_resp_t resp = {0};
    int hr = http_request(&req, &resp);
    if (status) *status = resp.status;
    if (cancel && *cancel) *err = xstrdup("Remote compaction aborted");
    else if (hr != 0 || resp.error) *err = xasprintf("Remote compaction failed: %s", resp.error ? resp.error : "request failed");
    else if (resp.status < 200 || resp.status >= 300) {
        LOGW("OpenAI remote compaction failed: %s %d %.500s", url, resp.status, resp.body ? resp.body : "");
        *err = xasprintf("Remote compaction failed (%d %s)", resp.status, http_status_text(resp.status));
    } else {
        json_t *data = json_parse(resp.body ? resp.body : "", resp.body_len, NULL);
        json_t *hist = json_arr(), *item = NULL;
        json_arr_foreach(json_get_arr(data, "output"), i, it) {
            if (!json_is_obj(it) || !keep_compact_item(it)) continue;
            json_push(hist, json_clone(it));
        }
        json_arr_foreach(hist, i, it) {
            const char *t = json_get_str(it, "type");
            if ((str_eq(t, "compaction") && json_get_str(it, "encrypted_content")) || str_eq(t, "compaction_summary")) item = it;
        }
        if (!item) {
            *err = xstrdup("Remote compaction response missing compaction item");
            json_free(hist);
        } else {
            out = json_obj();
            json_set(out, "compactionItem", json_clone(item));
            json_set(out, "replacementHistory", hist);
        }
        json_free(data);
    }
    http_resp_free(&resp);
    free(payload);
    free(url);
done:
    http_headers_free(&h);
    json_free(body);
    return out;
}
