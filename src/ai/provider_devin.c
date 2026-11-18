/* provider_devin.c - devin-agent: Codeium/Windsurf Cascade chat over the Connect protocol (HTTP/1.1).
 * Port of omp packages/ai/src/providers/devin.ts + catalog wire/devin.ts, wire/devin-proto.ts and the
 * consumed subset of discovery/devin-proto.ts:
 *   GetUserJwt (unary, application/proto) -> user JWT + optional custom API server URL
 *   AssignModel (unary; router models only, compat.modelRouter) -> concrete model uid + assignment JWT
 *   GetChatMessage (Connect server stream, application/connect+proto, gzip when >= 1 KiB) -> deltas
 * Auth rides inside Metadata.apiKey (bare JWT session tokens get the `devin-session-token$` prefix). */
#define _GNU_SOURCE
#include "ai.h"
#include "ai/cursor_proto.h"
#include "ai/pb.h"
#include "ai/provider_google.h"

#include <ctype.h>
#include <math.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <zlib.h>

#define DEVIN_DEFAULT_BASE_URL "https://server.codeium.com"
#define DEVIN_SESSION_TOKEN_PREFIX "devin-session-token$"
#define CHAT_MESSAGE_PATH "/exa.api_server_pb.ApiServerService/GetChatMessage"
#define ASSIGN_MODEL_PATH "/exa.api_server_pb.ApiServerService/AssignModel"
#define AUTH_PATH "/exa.auth_pb.AuthService/GetUserJwt"
#define TOOL_CALL_ID_MAX 40
#define MAX_FRAME_PAYLOAD (16u * 1024 * 1024)
#define MIN_COMPRESSED_BODY_BYTES 1024
#define LARGE_HISTORY_RECOVERY_BYTES (512 * 1024)
#define MAX_ERROR_DETAIL_CHARS 4096
#define MAX_TRAILER_EVIDENCE_CHARS 2000
#define MAX_INFLATE (64u * 1024 * 1024)

/* enums (exa.*) */
enum { SRC_USER = 1, SRC_SYSTEM = 2, SRC_TOOL = 4 };
enum { REQ_CASCADE = 5, PLANNER_DEFAULT = 1, CACHE_EPHEMERAL = 1, STOP_MAX_TOKENS = 3 };

/* field order follows the upstream generated declarations (protobuf-es writes in declaration order) */
static const char DEVIN_PROTO_DSL[] =
    "Metadata 1:ideName:string 7:ideVersion:string 28:ideType:string 12:extensionName:string"
    " 2:extensionVersion:string 3:apiKey:string 4:locale:string 5:os:string 8:hardware:string"
    " 6:disableTelemetry:bool 10:sessionId:string 16:lsTimestamp:Timestamp 9:requestId:uint64"
    " 11:sourceAddress:string 13:userAgent:string 14:url:string 15:authSource:enum 17:extensionPath:string"
    " 20:userId:string 21:userJwt:string 22:forceTeamId:string 24:deviceFingerprint:string 25:triggerId:string"
    " 26:planName:string 27:id:string 29:impersonateTier:string 30:supportedModelDisplays:enum* 31:f:string"
    " 32:teamId:string\n"
    "Timestamp 1:seconds:int64 2:nanos:int32\n"
    "ImageData 1:base64Data:string 2:mimeType:string 3:caption:string\n"
    "ChatToolCall 1:id:string 2:name:string 3:argumentsJson:string 4:invalidJsonStr:string 5:invalidJsonErr:string"
    " 6:isCustomToolCall:bool\n"
    "PromptCacheOptions 1:type:enum\n"
    "ChatMessagePrompt 1:messageId:string 2:source:enum 3:prompt:string 4:numTokens:uint32"
    " 5:safeForCodeTelemetry:bool 6:toolCalls:ChatToolCall* 7:toolCallId:string"
    " 8:promptCacheOptions:PromptCacheOptions 9:toolResultIsError:bool 10:images:ImageData* 11:thinking:string"
    " 12:signature:string 13:thinkingRedacted:bool 15:outputId:string 16:thinkingId:string"
    " 17:geminiThoughtSignature:bytes 18:signatureType:string 19:phase:string\n"
    "ChatToolChoice 1:optionName:string? 2:toolName:string?\n"
    "ChatToolDefinition 1:name:string 2:description:string 3:jsonSchemaString:string 5:attributionFieldNames:string*"
    " 6:serverName:string 7:readOnlyHint:bool? 9:isCustomTool:bool? 10:customToolGrammar:string?"
    " 11:customToolGrammarSyntax:string? 12:strict:bool\n"
    "CompletionConfiguration 1:numCompletions:uint64 2:maxTokens:uint64 3:maxNewlines:uint64"
    " 4:minLogProbability:double 5:temperature:double 6:firstTemperature:double 7:topK:uint64 8:topP:double"
    " 9:stopPatterns:string* 10:seed:uint64 11:fimEotProbThreshold:double 12:useFimEotThreshold:bool"
    " 13:doNotScoreStopTokens:bool 14:sqrtLenNormalizedLogProbScore:bool 15:lastMessageIsPartial:bool"
    " 16:returnLogprob:bool 17:serviceTier:string\n"
    "GetChatMessageRequest 1:metadata:Metadata 2:prompt:string 3:chatMessagePrompts:ChatMessagePrompt*"
    " 5:useInternalChatModel:bool 6:internalChatModel:enum 21:chatModelUid:string 7:requestType:enum"
    " 8:configuration:CompletionConfiguration 10:tools:ChatToolDefinition* 11:disableParallelToolCalls:bool"
    " 12:toolChoice:ChatToolChoice 13:systemPromptCacheOptions:PromptCacheOptions 14:chatModelName:string"
    " 16:cascadeId:string 17:promptId:string 18:providerSource:enum 19:language:enum 20:plannerMode:enum"
    " 22:executionId:string 24:arenaConvergeCount:int32? 25:arenaAssignmentJwt:string?"
    " 26:modelAssignmentJwt:string?\n"
    "ModelUsageStats 1:modelDeprecated:enum 9:modelUid:string 10:billingModelUid:string"
    " 11:requestedModelUid:string 2:inputTokens:uint64 3:outputTokens:uint64 4:cacheWriteTokens:uint64"
    " 5:cacheReadTokens:uint64 6:apiProvider:enum 7:messageId:string 8:responseHeader:map<string>\n"
    "GetChatMessageResponse 1:messageId:string 2:timestamp:Timestamp 3:deltaText:string 4:deltaTokens:uint32"
    " 5:stopReason:enum 6:deltaToolCalls:ChatToolCall* 7:usage:ModelUsageStats 14:creditCost:int32 8:redact:bool"
    " 9:deltaThinking:string 10:deltaSignature:string 11:thinkingRedacted:bool 12:latency:double"
    " 15:outputId:string 16:thinkingId:string 17:requestId:string 18:committedCreditCost:int32 19:prompt:string"
    " 20:geminiThoughtSignature:bytes 21:deltaSignatureType:string 22:committedAcuCost:double"
    " 23:actualModelUid:string? 24:arenaInvocationCapReached:bool 25:phase:string"
    " 26:committedQuotaCostBasisPoints:int64? 27:committedOverageCostCents:int64?\n"
    "GetUserJwtRequest 1:metadata:Metadata\n"
    "GetUserJwtResponse 1:userJwt:string 2:customApiServerUrl:string\n"
    "AssignModelRequest 1:metadata:Metadata 2:modelRouterUid:string 3:cascadeId:string"
    " 5:chatMessagePrompt:ChatMessagePrompt\n"
    "ModelAssignment 1:assignmentJwt:string 2:modelUid:string 3:harnessUids:string*\n"
    "AssignModelResponse 1:assignment:ModelAssignment\n";

static void set_own(json_t *o, const char *k, char *v) { json_set(o, k, json_str_own(v)); }

static pb_schema_t *g_schema;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void schema_init(void) {
    char *err = NULL;
    g_schema = pb_schema_parse(DEVIN_PROTO_DSL, &err);
    if (!g_schema) {
        LOGE("devin: proto schema: %s", err ? err : "?");
        abort();
    }
}

static const pb_msg_t *dmsg(const char *name) {
    pthread_once(&g_once, schema_init);
    const pb_msg_t *m = pb_schema_msg(g_schema, name);
    if (!m) abort();
    return m;
}

static char *dencode(const char *name, const json_t *v, size_t *len) {
    pthread_once(&g_once, schema_init);
    return pb_encode_named(g_schema, name, v, len);
}

/* ---------- wire identity (catalog wire/devin.ts) ---------- */

static const char *devin_os(void) {
#if defined(__APPLE__)
    return "darwin";
#elif defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

/* encodeDevinCredential: bare JWT-shaped session tokens get the prefix; everything else goes out as stored */
static char *encode_credential(const char *api_key) {
    char *k = api_key ? str_trim(api_key) : xstrdup("");
    if (!*k || str_starts(k, DEVIN_SESSION_TOKEN_PREFIX)) return k;
    int dots = 0;
    for (const char *p = k; *p; p++) dots += *p == '.';
    if (dots == 2) {
        char *r = xasprintf("%s%s", DEVIN_SESSION_TOKEN_PREFIX, k);
        free(k);
        return r;
    }
    return k;
}

/* devinCliMetadata */
static json_t *cli_metadata(const char *api_key, const char *user_jwt) {
    json_t *m = json_obj();
    set_own(m, "apiKey", encode_credential(api_key));
    json_set_str(m, "userJwt", user_jwt ? user_jwt : "");
    json_set_str(m, "ideName", "devin-cli");
    json_set_str(m, "ideType", "chisel");
    json_set_str(m, "ideVersion", "3000.12.57");
    json_set_str(m, "extensionName", "chisel");
    json_set_str(m, "extensionVersion", "3000.12.57");
    json_set_str(m, "locale", "en");
    json_set_str(m, "os", devin_os());
    return m;
}

/* deterministicUuid: leading 128 bits of SHA-256(seed) in 8-4-4-4-12 layout */
static char *deterministic_uuid(const char *seed, size_t n) {
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)seed, n, d);
    char *h = hex_encode(d, 16);
    char *r = xasprintf("%.8s-%.4s-%.4s-%.4s-%.12s", h, h + 8, h + 12, h + 16, h + 20);
    free(h);
    return r;
}

/* ---------- gzip ---------- */

static int gzip_buf(const void *in, size_t n, sb_t *out) {
    z_stream zs = {0};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return -1;
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)n;
    char buf[65536];
    int r;
    do {
        zs.next_out = (Bytef *)buf;
        zs.avail_out = sizeof buf;
        r = deflate(&zs, Z_FINISH);
        if (r == Z_STREAM_ERROR) {
            deflateEnd(&zs);
            return -1;
        }
        sb_appendn(out, buf, sizeof buf - zs.avail_out);
    } while (r != Z_STREAM_END);
    deflateEnd(&zs);
    return 0;
}

static int gunzip_buf(const void *in, size_t n, sb_t *out) {
    z_stream zs = {0};
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return -1;
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)n;
    char buf[65536];
    int r;
    do {
        zs.next_out = (Bytef *)buf;
        zs.avail_out = sizeof buf;
        r = inflate(&zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) {
            inflateEnd(&zs);
            return -1;
        }
        sb_appendn(out, buf, sizeof buf - zs.avail_out);
        if (out->len > MAX_INFLATE) {
            inflateEnd(&zs);
            return -1;
        }
    } while (r != Z_STREAM_END && (zs.avail_in > 0 || zs.avail_out == 0));
    inflateEnd(&zs);
    return r == Z_STREAM_END ? 0 : -1;
}

/* decodeDevinUnaryMessage: bare protobuf, else gzipped protobuf */
static json_t *decode_unary(const char *name, const char *data, size_t len) {
    char *e = NULL;
    json_t *j = pb_decode(dmsg(name), data, len, &e);
    free(e);
    if (j) return j;
    sb_t z = SB_INIT;
    e = NULL;
    if (gunzip_buf(data, len, &z) == 0) j = pb_decode(dmsg(name), z.s ? z.s : "", z.len, &e);
    free(e);
    sb_free(&z);
    return j;
}

/* ---------- HTTP errors (createDevinHttpError / devinErrorDetail) ---------- */

static const char *status_text(int s) {
    switch (s) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return NULL;
    }
}

static bool html_body(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return strncasecmp(s, "<!doctype html", 14) == 0 || strncasecmp(s, "<html", 5) == 0;
}

static char *error_detail(const char *content_type, const char *payload, size_t n) {
    if (!payload || !n || !utf8_valid(payload, n)) return NULL;
    if (content_type && str_istr(content_type, "text/html")) return NULL;
    char *text = xstrndup(payload, n);
    json_t *j = json_parse(text, n, NULL);
    if (json_is_obj(j)) {
        json_t *e = json_get(j, "error");
        const char *m = NULL;
        if (json_is_obj(e) && json_get_str(e, "message")) m = json_get_str(e, "message");
        else if (json_is_str(e)) m = json_sv(e);
        else if (json_get_str(j, "message")) m = json_get_str(j, "message");
        if (m) {
            free(text);
            text = xstrdup(m);
        }
    }
    json_free(j);
    /* collapse whitespace */
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = text; *p; p++) {
        if (isspace((unsigned char)*p)) {
            sp = b.len > 0;
            continue;
        }
        if (sp) sb_putc(&b, ' ');
        sp = false;
        sb_putc(&b, *p);
    }
    free(text);
    char *norm = sb_detach(&b);
    bool bad = !*norm || html_body(norm) || !utf8_valid(norm, strlen(norm));
    for (const unsigned char *p = (const unsigned char *)norm; !bad && *p; p++) {
        if (*p < 0x20 || *p == 0x7f) bad = true;
        /* C1 controls U+0080..U+009F encode as C2 80..C2 9F */
        if (*p == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f) bad = true;
    }
    if (bad) {
        free(norm);
        return NULL;
    }
    /* cap at MAX_ERROR_DETAIL_CHARS code points */
    size_t cps = 0;
    for (char *p = norm; *p; p++) {
        if (((unsigned char)*p & 0xc0) != 0x80 && cps++ == MAX_ERROR_DETAIL_CHARS) {
            *p = 0;
            break;
        }
    }
    return norm;
}

static char *http_error(const char *op, int status, const char *content_type, const char *payload, size_t n) {
    char *d = error_detail(content_type, payload, n);
    const char *st = status_text(status);
    char *r = xasprintf("Devin %s error %d%s%s%s%s", op, status, st ? " " : "", st ? st : "", d ? ": " : "", d ? d : "");
    free(d);
    return r;
}

/* ---------- unary RPCs ---------- */

static int unary(const char *url, const char *op, const char *resp_name, const json_t *req_obj, const char *req_name,
                 const ai_stream_opts_t *o, json_t **out, char **err) {
    size_t len;
    char *bytes = dencode(req_name, req_obj, &len);
    http_headers_t h = {0};
    http_headers_set(&h, "content-type", "application/proto");
    http_headers_set(&h, "connect-protocol-version", "1");
    http_headers_set(&h, "accept", "*/*");
    char *u = ai_rewrite_url(url);
    http_req_t req = {.method = "POST", .url = u, .headers = &h, .body = bytes, .body_len = len, .timeout_ms = 60000,
                      .cancel = o->cancel};
    http_resp_t r;
    int rc = http_request(&req, &r);
    free(u);
    free(bytes);
    http_headers_free(&h);
    *out = NULL;
    if (rc != 0 || r.error) {
        *err = (o->cancel && *o->cancel) ? xstrdup("Request was aborted")
                                         : xasprintf("Devin %s request failed: %s", op, r.error ? r.error : "network error");
        http_resp_free(&r);
        return o->cancel && *o->cancel ? -2 : -1;
    }
    if (r.status < 200 || r.status >= 300) {
        *err = http_error(op, r.status, http_headers_get(&r.headers, "content-type"), r.body, r.body_len);
        http_resp_free(&r);
        return -1;
    }
    *out = decode_unary(resp_name, r.body ? r.body : "", r.body_len);
    http_resp_free(&r);
    return 0;
}

/* ---------- message history ---------- */

static bool responses_family(const char *api) {
    return str_eq(api, "openai-responses") || str_eq(api, "openai-codex-responses") || str_eq(api, "azure-openai-responses");
}

/* normalizeToolCallId(isResponses ? responsesCallComponent(id) : id, 40) */
static char *normalize_call_id(const char *id, const char *source_api) {
    const char *s = id ? id : "";
    size_t n = strlen(s);
    if (responses_family(source_api)) {
        const char *pipe = strchr(s, '|');
        if (pipe && pipe > s) n = (size_t)(pipe - s);
    }
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n && b.len < TOOL_CALL_ID_MAX; i++) {
        char c = s[i];
        sb_putc(&b, (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_');
    }
    return sb_detach(&b);
}

/* rewrite tool-call ids of assistant turns and their results (in place on the transformed copy) */
static void normalize_tool_ids(json_t *msgs) {
    hmap_t map;
    hmap_init(&map);
    json_t *m;
    json_arr_foreach(msgs, i, m) {
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            json_t *b;
            json_arr_foreach(json_get_arr(m, "content"), k, b) {
                if (!str_eq(json_get_str(b, "type"), "toolCall")) continue;
                const char *id = json_get_str(b, "id");
                char *nid = normalize_call_id(id, json_get_str(m, "api"));
                if (id) free(hmap_put(&map, id, xstrdup(nid)));
                set_own(b, "id", nid);
            }
        } else if (str_eq(role, "toolResult")) {
            const char *id = json_get_str(m, "toolCallId");
            const char *mapped = id ? hmap_get(&map, id) : NULL;
            set_own(m, "toolCallId", mapped ? xstrdup(mapped) : normalize_call_id(id, NULL));
        }
    }
    hmap_free(&map, free);
}

static json_t *image_data(const json_t *part) {
    json_t *im = json_obj();
    json_set_str(im, "base64Data", json_get_str(part, "data") ? json_get_str(part, "data") : "");
    json_set_str(im, "mimeType", json_get_str(part, "mimeType") ? json_get_str(part, "mimeType") : "");
    return im;
}

/* buildUserPrompt */
static json_t *user_prompt(const json_t *msg, const char *message_id) {
    sb_t text = SB_INIT;
    json_t *images = json_arr();
    json_t *c = json_get(msg, "content");
    if (json_is_str(c)) {
        sb_appendn(&text, c->u.str.s, c->u.str.len);
    } else {
        json_t *p;
        json_arr_foreach(c, i, p) {
            const char *t = json_get_str(p, "type");
            if (str_eq(t, "text") && json_get_str(p, "text")) sb_append(&text, json_get_str(p, "text"));
            else if (str_eq(t, "image")) json_push(images, image_data(p));
        }
    }
    json_t *pr = json_obj();
    json_set_str(pr, "messageId", message_id);
    json_set_int(pr, "source", SRC_USER);
    json_set(pr, "prompt", json_strn(text.s ? text.s : "", text.len));
    json_set(pr, "images", images);
    sb_free(&text);
    return pr;
}

static char *seeded_id(const char *cascade, size_t index, const char *role, const char *extra) {
    sb_t s = SB_INIT;
    sb_appendf(&s, "%s", cascade);
    sb_putc(&s, 0);
    sb_appendf(&s, "%zu", index);
    sb_putc(&s, 0);
    sb_append(&s, role);
    if (extra) {
        sb_putc(&s, 0);
        sb_append(&s, extra);
    }
    char *r = deterministic_uuid(s.s, s.len);
    sb_free(&s);
    return r;
}

/* buildChatMessagePrompts */
static json_t *chat_prompts(const json_t *msgs, const char *cascade, const ai_model_t *m) {
    json_t *out = json_arr();
    json_t *msg;
    json_arr_foreach(msgs, index, msg) {
        const char *role = json_get_str(msg, "role");
        if (str_eq(role, "user") || str_eq(role, "developer")) {
            char *id = seeded_id(cascade, index, role, NULL);
            json_push(out, user_prompt(msg, id));
            free(id);
        } else if (str_eq(role, "assistant")) {
            bool native = str_eq(json_get_str(msg, "api"), m->api) && str_eq(json_get_str(msg, "provider"), m->provider) &&
                          str_eq(json_get_str(msg, "model"), m->id);
            sb_t text = SB_INIT, thinking = SB_INIT;
            const char *signature = NULL;
            json_t *calls = json_arr();
            json_t *p;
            json_arr_foreach(json_get_arr(msg, "content"), k, p) {
                const char *t = json_get_str(p, "type");
                if (str_eq(t, "text")) {
                    if (json_get_str(p, "text")) sb_append(&text, json_get_str(p, "text"));
                } else if (str_eq(t, "thinking")) {
                    if (json_get_str(p, "thinking")) sb_append(&thinking, json_get_str(p, "thinking"));
                    const char *sig = json_get_str(p, "thinkingSignature");
                    if (native && !signature && sig && *sig) signature = sig;
                } else if (str_eq(t, "toolCall")) {
                    json_t *tc = json_obj();
                    json_set_str(tc, "id", json_get_str(p, "id") ? json_get_str(p, "id") : "");
                    json_set_str(tc, "name", json_get_str(p, "name") ? json_get_str(p, "name") : "");
                    json_t *args = json_get(p, "arguments");
                    set_own(tc, "argumentsJson", args ? json_dump(args, 0) : xstrdup("{}"));
                    json_push(calls, tc);
                }
            }
            if (!text.len && !thinking.len && !signature && !json_len(calls)) {
                sb_free(&text);
                sb_free(&thinking);
                json_free(calls);
                continue;
            }
            json_t *pr = json_obj();
            const char *rid = json_get_str(msg, "responseId");
            set_own(pr, "messageId", native && rid && *rid ? xstrdup(rid) : seeded_id(cascade, index, "assistant", NULL));
            json_set_int(pr, "source", SRC_SYSTEM);
            json_set(pr, "prompt", json_strn(text.s ? text.s : "", text.len));
            json_set(pr, "thinking", json_strn(thinking.s ? thinking.s : "", thinking.len));
            json_set_str(pr, "signature", signature ? signature : "");
            json_set_str(pr, "signatureType", "");
            json_set(pr, "toolCalls", calls);
            json_push(out, pr);
            sb_free(&text);
            sb_free(&thinking);
        } else if (str_eq(role, "toolResult")) {
            sb_t text = SB_INIT;
            json_t *images = json_arr();
            json_t *p;
            json_arr_foreach(json_get_arr(msg, "content"), k, p) {
                const char *t = json_get_str(p, "type");
                if (str_eq(t, "text") && json_get_str(p, "text")) sb_append(&text, json_get_str(p, "text"));
                else if (str_eq(t, "image")) json_push(images, image_data(p));
            }
            const char *tcid = json_get_str(msg, "toolCallId") ? json_get_str(msg, "toolCallId") : "";
            json_t *pr = json_obj();
            set_own(pr, "messageId", seeded_id(cascade, index, "tool", tcid));
            json_set_int(pr, "source", SRC_TOOL);
            json_set_str(pr, "toolCallId", tcid);
            json_set_bool(pr, "toolResultIsError", json_get_bool(msg, "isError", false));
            json_set(pr, "prompt", json_strn(text.s ? text.s : "", text.len));
            json_set(pr, "images", images);
            json_push(out, pr);
            sb_free(&text);
        }
    }
    return out;
}

/* resolveWireModelId: thinking-effort routing, else requestModelId ?? id */
static char *wire_model_id(const ai_model_t *m, const ai_stream_opts_t *o) {
    const char *effort = "off";
    if (o->reasoning != EFFORT_OFF && !o->disable_reasoning && m->reasoning) effort = effort_name(model_clamp_effort(m, o->reasoning));
    json_t *routing = m->thinking ? json_get_obj(m->thinking, "effortRouting") : NULL;
    const char *t = routing ? json_get_str(routing, effort) : NULL;
    if (t) return xstrdup(t);
    const char *rid = json_get_str(m->raw, "requestModelId");
    return xstrdup(rid ? rid : m->id);
}

static bool uses_google_schema(const ai_model_t *m, const char *uid) {
    json_t *ident = json_get_obj(m->raw, "identity");
    if (str_eq(json_get_str(ident, "class"), "gemini")) return true;
    if (str_starts(uid, "MODEL_GOOGLE_GEMINI_")) return true;
    return str_istr(uid, "gemini") != NULL || str_istr(m->id, "gemini") != NULL;
}

/* buildDevinChatRequest */
static json_t *build_chat_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, const char *api_key,
                                  const char *user_jwt, const char *cascade, const json_t *msgs, const json_t *assignment,
                                  const char *routed_uid) {
    const char *assigned = json_get_str(assignment, "modelUid");
    const char *uid = assigned && *assigned ? assigned : routed_uid;
    bool gschema = uses_google_schema(m, uid);
    json_t *r = json_obj();
    json_set(r, "metadata", cli_metadata(api_key, user_jwt));
    sb_t sys = SB_INIT;
    json_t *s;
    json_arr_foreach(ctx ? ctx->system_prompt : NULL, i, s) {
        const char *t = json_sv(s);
        if (!t) continue;
        bool blank = true;
        for (const char *p = t; *p && blank; p++) blank = isspace((unsigned char)*p);
        if (blank) continue;
        if (sys.len) sb_append(&sys, "\n\n");
        sb_append(&sys, t);
    }
    json_set(r, "prompt", json_strn(sys.s ? sys.s : "", sys.len));
    sb_free(&sys);
    json_set(r, "chatMessagePrompts", chat_prompts(msgs, cascade, m));
    json_set_str(r, "chatModelUid", uid);
    if (assignment) json_set_str(r, "modelAssignmentJwt", json_get_str(assignment, "assignmentJwt"));
    json_set_int(r, "requestType", REQ_CASCADE);
    json_set_int(r, "plannerMode", PLANNER_DEFAULT);
    json_t *tc = json_obj();
    json_set_str(tc, "optionName", "auto");
    json_set(r, "toolChoice", tc);
    json_t *cache = json_obj();
    json_set_int(cache, "type", CACHE_EPHEMERAL);
    json_set(r, "systemPromptCacheOptions", cache);
    json_set_bool(r, "disableParallelToolCalls", !model_compat_bool(m, "supportsParallelToolCalls", false));
    json_set_str(r, "cascadeId", cascade);
    set_own(r, "executionId", uuid_v4());
    json_t *cfg = json_obj();
    json_set_int(cfg, "numCompletions", 1);
    json_set_int(cfg, "maxTokens", o->max_tokens > 0 ? o->max_tokens : m->max_tokens > 0 ? m->max_tokens : 64000);
    json_set_int(cfg, "maxNewlines", 400);
    /* clampDevinTemperature: Cascade answers temperature 0 with an opaque invalid_argument while any positive value
     * works, so an explicit temperature is raised to the catalog's temperatureFloor (unset: wire default 1) */
    double temp = isnan(o->temperature) ? 1.0 : o->temperature;
    const json_t *tfloor = model_compat_get(m, "temperatureFloor");
    if (json_is_num(tfloor) && temp < tfloor->u.num.d) temp = tfloor->u.num.d;
    json_set_num(cfg, "temperature", temp);
    json_set_int(cfg, "topK", 40);
    json_set_num(cfg, "topP", (double)(float)(isnan(o->top_p) ? 0.95 : o->top_p));
    json_t *stops = json_arr(), *sp;
    json_arr_foreach(o->stop_sequences, i, sp) if (json_is_str(sp)) json_push(stops, json_clone(sp));
    json_set(cfg, "stopPatterns", stops);
    json_set(r, "configuration", cfg);
    json_t *tools = json_arr(), *t;
    json_arr_foreach(ctx ? ctx->tools : NULL, i, t) {
        json_t *d = json_obj();
        json_set_str(d, "name", json_get_str(t, "name") ? json_get_str(t, "name") : "");
        json_set_str(d, "description", json_get_str(t, "description") ? json_get_str(t, "description") : "");
        json_t *schema = json_get(t, "parameters");
        json_t *norm = gschema && schema ? google_normalize_schema(schema) : NULL;
        if (norm || schema) set_own(d, "jsonSchemaString", json_dump(norm ? norm : schema, 0));
        else json_set_str(d, "jsonSchemaString", "{}");
        json_free(norm);
        json_set_bool(d, "strict", json_get_bool(t, "strict", false));
        json_push(tools, d);
    }
    json_set(r, "tools", tools);
    return r;
}

/* ---------- Connect trailer (readConnectTrailerError) ---------- */

static char *trunc_evidence(const char *s) {
    size_t n = strlen(s);
    return n > MAX_TRAILER_EVIDENCE_CHARS ? xasprintf("%.*s…", MAX_TRAILER_EVIDENCE_CHARS, s) : xstrdup(s);
}

static char *summarize_details(const json_t *details) {
    if (!json_is_arr(details) || !json_len(details)) return NULL;
    sb_t sum = SB_INIT;
    json_t *e;
    json_arr_foreach(details, i, e) {
        if (!json_is_obj(e)) continue;
        const char *type = json_get_str(e, "type"), *value = json_get_str(e, "value");
        if (type && !*type) type = NULL;
        if (value && !*value) value = NULL;
        char *debug = NULL;
        json_t *dj = json_get(e, "debug");
        if (dj) debug = json_is_str(dj) ? xstrdup(json_sv(dj)) : json_dump(dj, 0);
        char *bt = type ? trunc_evidence(type) : NULL;
        const char *ev = debug ? debug : value;
        char *be = ev ? trunc_evidence(ev) : NULL;
        char *part = bt && be ? xasprintf("%s: %s", bt, be) : bt ? xstrdup(bt) : be ? xstrdup(be) : NULL;
        free(bt);
        free(be);
        free(debug);
        if (!part) continue;
        char *next = sum.len ? xasprintf("%s; %s", sum.s, part) : xstrdup(part);
        free(part);
        if (strlen(next) > MAX_TRAILER_EVIDENCE_CHARS) {
            char *t = trunc_evidence(next);
            free(next);
            sb_free(&sum);
            return t;
        }
        sb_clear(&sum);
        sb_append(&sum, next);
        free(next);
    }
    if (!sum.len) {
        sb_free(&sum);
        return NULL;
    }
    return sb_detach(&sum);
}

/* returns owned formatted message or NULL; *code_out / *msg_out borrowed from j */
static char *trailer_error(const json_t *j, const char **code_out, const char **msg_out) {
    json_t *err = json_is_obj(j) ? json_get(j, "error") : NULL;
    if (!json_is_obj(err)) return NULL;
    const char *code = json_get_str(err, "code") ? json_get_str(err, "code") : "";
    const char *msg = json_get_str(err, "message") ? json_get_str(err, "message") : "";
    if (!*code && !*msg) return NULL;
    *code_out = code;
    *msg_out = msg;
    sb_t b = SB_INIT;
    sb_appendf(&b, "Devin stream error%s%s: %s", *code ? " " : "", code, msg);
    char *d = summarize_details(json_get(err, "details"));
    if (d) sb_appendf(&b, " [details: %s]", d);
    free(d);
    return sb_detach(&b);
}

/* ---------- stream state ---------- */

typedef struct {
    char *id;
    sb_t json;
} dtool_t;

typedef struct {
    ai_builder_t b;
    const ai_model_t *m;
    connect_decoder_t dec;
    int status;
    char *content_type;
    sb_t errbody;
    char *error;        /* owned, terminal error */
    char *trailer_code; /* owned, invalid_argument recovery check */
    char *trailer_msg;
    bool got_token;
    bool have_response_id;
    VEC(dtool_t) tools;
    char *active_tool;
    int stop_reason;
    double credit_cost, committed_credit, acu_cost;
} dstream_t;

static dtool_t *find_tool(dstream_t *s, const char *id) {
    for (size_t i = 0; i < s->tools.n; i++)
        if (str_eq(s->tools.v[i].id, id)) return &s->tools.v[i];
    return NULL;
}

static void handle_response(dstream_t *s, const json_t *msg) {
    const char *mid = json_get_str(msg, "messageId");
    if (mid && *mid && !s->have_response_id) {
        aib_set(&s->b, "responseId", json_str(mid));
        s->have_response_id = true;
    }
    json_t *amu = json_get(msg, "actualModelUid");
    if (json_is_str(amu) && amu->u.str.len) aib_set(&s->b, "upstreamModel", json_clone(amu));
    json_t *th = json_get(msg, "deltaThinking");
    if (json_is_str(th) && th->u.str.len) {
        s->got_token = true;
        bool fresh = s->b.cur_kind != 2;
        aib_thinking_delta(&s->b, th->u.str.s, th->u.str.len);
        if (fresh) json_set_bool(json_at(json_get(s->b.msg, "content"), (size_t)s->b.cur_index), "summary", false);
        const char *sig = json_get_str(msg, "deltaSignature");
        if (sig && *sig) aib_set_thinking_signature(&s->b, sig);
    }
    json_t *tx = json_get(msg, "deltaText");
    if (json_is_str(tx) && tx->u.str.len) {
        s->got_token = true;
        aib_text_delta(&s->b, tx->u.str.s, tx->u.str.len);
    }
    json_t *tc;
    json_arr_foreach(json_get_arr(msg, "deltaToolCalls"), i, tc) {
        s->got_token = true;
        const char *id = json_get_str(tc, "id");
        if (!id || !*id) id = s->active_tool;
        if (!id) continue;
        const char *name = json_get_str(tc, "name");
        dtool_t *t = find_tool(s, id);
        if (!t) {
            dtool_t nt = {xstrdup(id), SB_INIT};
            vec_push(&s->tools, nt);
            t = &s->tools.v[s->tools.n - 1];
            aib_toolcall_start(&s->b, id, name ? name : "");
        } else if (name && *name) {
            aib_toolcall_set_field(&s->b, "name", name);
        }
        if (s->active_tool != t->id) s->active_tool = t->id;
        json_t *aj = json_get(tc, "argumentsJson");
        if (!json_is_str(aj) || !aj->u.str.len) continue;
        const char *a = aj->u.str.s;
        size_t an = aj->u.str.len;
        size_t prev = t->json.len;
        if (an >= prev && memcmp(a, t->json.s ? t->json.s : "", prev) == 0) {
            sb_appendn(&t->json, a + prev, an - prev);
            aib_toolcall_delta(&s->b, a + prev, an - prev);
        } else {
            sb_appendn(&t->json, a, an);
            aib_toolcall_delta(&s->b, a, an);
        }
    }
    int64_t sr = json_get_int(msg, "stopReason", 0);
    if (sr) s->stop_reason = (int)sr;
    json_t *u = json_get_obj(msg, "usage");
    if (u)
        aib_usage(&s->b, json_get_int(u, "inputTokens", 0), json_get_int(u, "outputTokens", 0), json_get_int(u, "cacheReadTokens", 0),
                  json_get_int(u, "cacheWriteTokens", 0));
    double cc = json_get_num(msg, "creditCost", 0), ccc = json_get_num(msg, "committedCreditCost", 0),
           acu = json_get_num(msg, "committedAcuCost", 0);
    if (cc || ccc || acu) {
        json_t *usage = json_get(s->b.msg, "usage");
        json_t *cr = json_get_obj(usage, "credits");
        if (!cr) {
            json_set(usage, "credits", json_obj());
            cr = json_get_obj(usage, "credits");
        }
        if (cc) json_set_num(cr, "cost", cc);
        if (ccc) json_set_num(cr, "committedCost", ccc);
        if (acu) json_set_num(cr, "acuCost", acu);
    }
}

static int on_headers(void *ud, int status, const http_headers_t *h) {
    dstream_t *s = ud;
    s->status = status;
    const char *ct = http_headers_get(h, "content-type");
    s->content_type = ct ? xstrdup(ct) : NULL;
    if (status >= 200 && status < 300) aib_start(&s->b);
    return 0;
}

static int on_data(void *ud, const char *data, size_t len) {
    dstream_t *s = ud;
    if (s->status < 200 || s->status >= 300) {
        if (s->errbody.len < 65536) sb_appendn(&s->errbody, data, len);
        return 0;
    }
    connect_decoder_feed(&s->dec, data, len);
    int flags, rc;
    const char *payload;
    size_t plen;
    while ((rc = connect_decoder_next(&s->dec, &flags, &payload, &plen)) == 1) {
        sb_t z = SB_INIT;
        if (flags & CONNECT_FLAG_COMPRESSED) {
            if (gunzip_buf(payload, plen, &z) != 0) {
                sb_free(&z);
                s->error = xstrdup("Devin stream error: failed to decompress a Connect frame");
                return 1;
            }
            payload = z.s ? z.s : "";
            plen = z.len;
        }
        if (flags & CONNECT_FLAG_END_STREAM) {
            char *t = xstrndup(payload, plen);
            char *tt = str_trim(t);
            free(t);
            json_t *j = *tt ? json_parse(tt, strlen(tt), NULL) : NULL;
            const char *code = NULL, *msg = NULL;
            char *f = trailer_error(j, &code, &msg);
            if (f) {
                LOGW("devin: stream rejected via Connect trailer: %s", tt);
                s->error = f;
                s->trailer_code = xstrdup(code);
                s->trailer_msg = xstrdup(msg);
            }
            json_free(j);
            free(tt);
            sb_free(&z);
            if (f) return 1;
            continue;
        }
        char *derr = NULL;
        json_t *msg = pb_decode(dmsg("GetChatMessageResponse"), payload, plen, &derr);
        sb_free(&z);
        if (!msg) {
            s->error = xasprintf("Devin stream error: malformed GetChatMessageResponse: %s", derr ? derr : "?");
            free(derr);
            return 1;
        }
        handle_response(s, msg);
        json_free(msg);
    }
    if (rc < 0) {
        s->error = xasprintf("Devin Connect frame length %zu exceeds %u-byte cap", plen, MAX_FRAME_PAYLOAD);
        return 1;
    }
    return 0;
}

static bool is_word_internal_error(const char *m) {
    for (const char *p = m; (p = str_istr(p, "internal error")); p++) {
        bool lb = p == m || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        char c = p[14];
        bool rb = !(isalnum((unsigned char)c) || c == '_');
        if (lb && rb) return true;
    }
    return false;
}

static json_t *fail(ai_builder_t *b, const char *reason, const char *msg) {
    json_t *r = aib_finish(b, reason, msg);
    aib_free(b);
    return r;
}

/* ---------- entry point ---------- */

json_t *provider_devin_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb,
                              void *ud) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    ai_builder_t b;
    aib_init(&b, m, cb, ud);
    char *env_key = NULL;
    const char *key = o->api_key;
    if (!key || !*key) key = env_key = ai_env_api_key(m->provider);
    if (!key || !*key) {
        free(env_key);
        char *e = xasprintf("No API key for provider %s", m->provider);
        json_t *r = fail(&b, "error", e);
        free(e);
        return r;
    }
    char *base = xstrdup(!str_empty(m->base_url) ? m->base_url : DEVIN_DEFAULT_BASE_URL);
    for (size_t n = strlen(base); n > 0 && base[n - 1] == '/';) base[--n] = 0;
    json_t *result = NULL;
    char *err = NULL, *user_jwt = NULL, *chat_base = NULL, *cascade = NULL, *routed = NULL;
    json_t *msgs = NULL, *assign_resp = NULL, *request = NULL;
    sb_t body = SB_INIT, raw = SB_INIT, gz = SB_INIT;
    dstream_t s = {0};

    /* GetUserJwt */
    json_t *jreq = json_obj();
    json_set(jreq, "metadata", cli_metadata(key, NULL));
    char *url = xasprintf("%s%s", base, AUTH_PATH);
    json_t *jresp = NULL;
    int rc = unary(url, "auth", "GetUserJwtResponse", jreq, "GetUserJwtRequest", o, &jresp, &err);
    free(url);
    json_free(jreq);
    if (rc != 0) {
        result = fail(&b, rc == -2 ? "aborted" : "error", err);
        goto out;
    }
    const char *jwt = json_get_str(jresp, "userJwt");
    if (!jwt || !*jwt) {
        json_free(jresp);
        result = fail(&b, "error", "Devin auth error: GetUserJwt returned an empty user JWT");
        goto out;
    }
    user_jwt = xstrdup(jwt);
    char *custom = json_get_str(jresp, "customApiServerUrl") ? str_trim(json_get_str(jresp, "customApiServerUrl")) : xstrdup("");
    json_free(jresp);
    if (*custom) {
        for (size_t n = strlen(custom); n > 0 && custom[n - 1] == '/';) custom[--n] = 0;
        chat_base = custom;
    } else {
        free(custom);
        chat_base = xstrdup(base);
    }

    cascade = !str_empty(o->session_id) ? xstrdup(o->session_id) : uuid_v4();
    msgs = ai_transform_messages(ctx ? ctx->messages : NULL, m);
    normalize_tool_ids(msgs);
    routed = wire_model_id(m, o);

    /* AssignModel for router models */
    json_t *assignment = NULL;
    if (model_compat_bool(m, "modelRouter", false)) {
        json_t *ar = json_obj();
        json_set(ar, "metadata", cli_metadata(key, NULL));
        const char *rid = json_get_str(m->raw, "requestModelId");
        json_set_str(ar, "modelRouterUid", rid ? rid : m->id);
        json_set_str(ar, "cascadeId", cascade);
        for (size_t i = json_len(msgs); i > 0; i--) {
            json_t *mm = json_at(msgs, i - 1);
            const char *role = json_get_str(mm, "role");
            if (str_eq(role, "user") || str_eq(role, "developer")) {
                json_set(ar, "chatMessagePrompt", user_prompt(mm, ""));
                break;
            }
        }
        url = xasprintf("%s%s", chat_base, ASSIGN_MODEL_PATH);
        rc = unary(url, "AssignModel", "AssignModelResponse", ar, "AssignModelRequest", o, &assign_resp, &err);
        free(url);
        json_free(ar);
        if (rc != 0) {
            result = fail(&b, rc == -2 ? "aborted" : "error", err);
            goto out;
        }
        assignment = json_get_obj(assign_resp, "assignment");
        const char *aj = json_get_str(assignment, "assignmentJwt"), *au = json_get_str(assignment, "modelUid");
        if (!aj || !*aj || !au || !*au) {
            result = fail(&b, "error", "Devin AssignModel error: response carried no assignment JWT and model uid");
            goto out;
        }
        aib_set(&b, "upstreamModel", json_str(au));
    }

    request = build_chat_request(m, ctx, o, key, user_jwt, cascade, msgs, assignment, routed);
    if (o->on_payload) o->on_payload(o->payload_ud, request);
    char *perr = NULL;
    if (pb_encode(dmsg("GetChatMessageRequest"), request, &raw, &perr) != 0) {
        char *e = xasprintf("Devin request encoding failed: %s", perr ? perr : "?");
        free(perr);
        result = fail(&b, "error", e);
        free(e);
        goto out;
    }
    bool compress = raw.len >= MIN_COMPRESSED_BODY_BYTES && gzip_buf(raw.s, raw.len, &gz) == 0;
    if (compress) connect_frame(&body, CONNECT_FLAG_COMPRESSED, gz.s, gz.len);
    else connect_frame(&body, 0, raw.s ? raw.s : "", raw.len);
    LOGD("devin: sending chat request model=%s tools=%zu requestBytes=%zu compressedBytes=%zu", m->id,
         json_len(ctx ? ctx->tools : NULL), raw.len, compress ? gz.len : 0);

    http_headers_t h = {0};
    http_headers_set(&h, "content-type", "application/connect+proto");
    http_headers_set(&h, "connect-protocol-version", "1");
    if (compress) http_headers_set(&h, "connect-content-encoding", "gzip");
    http_headers_set(&h, "accept-encoding", "identity");
    http_headers_set(&h, "user-agent", "connect-go/1.18.1 (go1.26.3)");
    http_headers_set(&h, "connect-accept-encoding", "gzip");
    ai_apply_headers(&h, NULL, o);
    char *curl = xasprintf("%s%s", chat_base, CHAT_MESSAGE_PATH);
    char *rurl = ai_rewrite_url(curl);
    free(curl);
    s.b = b;
    s.m = m;
    connect_decoder_init(&s.dec, MAX_FRAME_PAYLOAD);
    http_req_t req = {.method = "POST", .url = rurl, .headers = &h, .body = body.s, .body_len = body.len,
                      .idle_timeout_ms = 300000, .no_decompress = true, .on_headers = on_headers, .on_data = on_data,
                      .ud = &s, .cancel = o->cancel};
    http_resp_t resp;
    rc = http_request(&req, &resp);
    free(rurl);
    http_headers_free(&h);
    b = s.b; /* builder lives in the stream state during the transfer */
    bool aborted = o->cancel && *o->cancel;
    if (aborted) {
        result = fail(&b, "aborted", "Request was aborted");
    } else if (s.error) {
        if (!s.got_token && (s.trailer_code && str_ieq(s.trailer_code, "invalid_argument")) && is_word_internal_error(s.trailer_msg ? s.trailer_msg : "")) {
            /* history-size heuristic: re-encode only the shrinkable history prompts */
            json_t *prompts = json_get_arr(request, "chatMessagePrompts");
            size_t n = json_len(prompts), tail = 0;
            const char *last = json_get_str(json_at(msgs, json_len(msgs) ? json_len(msgs) - 1 : 0), "role");
            if (json_len(msgs) && (str_eq(last, "user") || str_eq(last, "developer"))) {
                tail = 1;
                if (str_eq(last, "developer"))
                    for (size_t i = json_len(msgs) - 1; i > 0; i--) {
                        const char *r = json_get_str(json_at(msgs, i - 1), "role");
                        if (!str_eq(r, "user") && !str_eq(r, "developer")) break;
                        tail++;
                    }
            }
            json_t *hist = json_obj(), *arr = json_arr();
            for (size_t i = 0; i + tail < n; i++) json_push(arr, json_clone(json_at(prompts, i)));
            json_set(hist, "chatMessagePrompts", arr);
            size_t hl = 0;
            char *hb = dencode("GetChatMessageRequest", hist, &hl);
            free(hb);
            json_free(hist);
            if (hl >= LARGE_HISTORY_RECOVERY_BYTES) {
                LOGW("devin: treating large-history invalid_argument as context overflow (historyBytes=%zu)", hl);
                aib_set(&b, "errorFlags", json_parse_str("[\"contextOverflow\"]"));
            }
        }
        result = fail(&b, "error", s.error);
    } else if (rc != 0 || resp.error) {
        char *e = xasprintf("Devin API request failed: %s", resp.error ? resp.error : "network error");
        result = fail(&b, "error", e);
        free(e);
    } else if (s.status < 200 || s.status >= 300) {
        char *e = http_error("API", s.status, s.content_type, s.errbody.s, s.errbody.len);
        result = fail(&b, "error", e);
        free(e);
    } else {
        aib_end_block(&b);
        /* final argument parse for every streamed call (toolcall_end) */
        for (size_t i = 0; i < s.tools.n; i++) {
            json_t *c = json_get(b.msg, "content"), *blk;
            json_arr_foreach(c, k, blk) {
                if (!str_eq(json_get_str(blk, "type"), "toolCall") || !str_eq(json_get_str(blk, "id"), s.tools.v[i].id)) continue;
                json_t *args = s.tools.v[i].json.len ? json_parse(s.tools.v[i].json.s, s.tools.v[i].json.len, NULL) : NULL;
                if (!json_is_obj(args)) {
                    json_free(args);
                    args = s.tools.v[i].json.len ? json_parse_partial(s.tools.v[i].json.s, s.tools.v[i].json.len) : NULL;
                    if (!json_is_obj(args)) {
                        json_free(args);
                        args = json_obj();
                    }
                }
                json_set(blk, "arguments", args);
            }
        }
        const char *reason = s.tools.n ? "toolUse" : s.stop_reason == STOP_MAX_TOKENS ? "length" : "stop";
        result = fail(&b, reason, NULL);
    }
    http_resp_free(&resp);

out:
    free(err);
    free(env_key);
    free(base);
    free(user_jwt);
    free(chat_base);
    free(cascade);
    free(routed);
    json_free(msgs);
    json_free(assign_resp);
    json_free(request);
    sb_free(&body);
    sb_free(&raw);
    sb_free(&gz);
    connect_decoder_free(&s.dec);
    free(s.content_type);
    sb_free(&s.errbody);
    free(s.error);
    free(s.trailer_code);
    free(s.trailer_msg);
    for (size_t i = 0; i < s.tools.n; i++) {
        free(s.tools.v[i].id);
        sb_free(&s.tools.v[i].json);
    }
    vec_free(&s.tools);
    return result;
}
