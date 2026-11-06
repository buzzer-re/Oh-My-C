/*
 * provider_anthropic.c - Anthropic Messages API streaming provider (api "anthropic-messages").
 * Port of packages/ai/src/providers/anthropic.ts (+ anthropic-identity.ts, claude-code-fingerprint.ts).
 */
#include "ai/ai_images.h"
#include "ai/provider_anthropic.h"
#include "ai/anthropic_slow.h"

#include "ai/anthropic_threads.h"
#include "coding/turn_recovery.h"

#include <ctype.h>
#include <math.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define DEFAULT_BASE_URL "https://api.anthropic.com"
#define OUTPUT_FALLBACK_BUFFER 4000
#define UNKNOWN_MODEL_MAX_OUTPUT 64000
#define OUTPUT_CAP_WHEN_UNKNOWN 64000
#define MAX_BREAKPOINTS 4
#define DECIMATION_INTERVAL 15
#define STOP_SEQUENCES_MAX 4
#define SCHEMA_MAX_DEPTH 64
#define MAX_STREAM_BLOCKS 4096
#define MAX_TOOL_ID_LEN 64

#define DEFAULT_CLAUDE_CODE_VERSION "2.1.280"
#define CLAUDE_CODE_SDK_VERSION "0.112.1"
#define CLAUDE_CODE_SYSTEM_INSTRUCTION "You are Claude Code, Anthropic's official CLI for Claude."
#define CLAUDE_TOOL_PREFIX "_"
#define BILLING_PREFIX "x-anthropic-billing-header:"
#define CCH_PLACEHOLDER "cch=00000"
#define CCH_SEED 0x4d659218e32a3268ULL
#define NON_VISION_IMAGE_PLACEHOLDER "[image omitted: model does not support vision]"
#define EMPTY_ERROR_TOOL_RESULT_TEXT "Tool failed with no output."

/* betas */
#define BETA_OAUTH "oauth-2025-04-20"
#define BETA_MID_CONV_SYSTEM "mid-conversation-system-2026-04-07"
#define BETA_CONTEXT_MGMT "context-management-2025-06-27"
#define BETA_STRUCTURED_OUTPUTS "structured-outputs-2025-12-15"
#define BETA_THINKING_TOKEN_COUNT "thinking-token-count-2026-05-13"
#define BETA_FALLBACK_CREDIT "fallback-credit-2026-06-01"
#define BETA_PROMPT_CACHING_SCOPE "prompt-caching-scope-2026-01-05"
#define BETA_EXTENDED_CACHE_TTL "extended-cache-ttl-2025-04-11"
#define BETA_FINE_GRAINED_TOOLS "fine-grained-tool-streaming-2025-05-14"
#define BETA_INTERLEAVED "interleaved-thinking-2025-05-14"
#define BETA_FAST_MODE "fast-mode-2026-02-01"
#define BETA_EFFORT "effort-2025-11-24"
#define BETA_COMPACTION "compact-2026-09-04"
#define BETA_CLAUDE_CODE "claude-code-20250219"

/* ======================================================================
 * small helpers
 * ====================================================================== */

static bool is_official_url(const char *url) {
    if (!url || !*url) return true;
    static const char off[] = DEFAULT_BASE_URL;
    size_t n = sizeof(off) - 1;
    if (strncasecmp(url, off, n) != 0) return false;
    return url[n] == '\0' || url[n] == '/';
}

/* trim, strip trailing slashes and a trailing "/v1"; NULL when empty */
static char *normalize_base_url(const char *url) {
    if (!url) return NULL;
    char *t = str_trim(url);
    size_t n = strlen(t);
    while (n && t[n - 1] == '/') t[--n] = '\0';
    if (n >= 3 && strcmp(t + n - 3, "/v1") == 0) t[n - 3] = '\0';
    if (!*t) {
        free(t);
        return NULL;
    }
    return t;
}

static bool url_has(const char *url, const char *needle) { return url && str_istr(url, needle) != NULL; }

/* Non-official URLs that enforce thinking signatures on replay (approximation of isAnthropicSigningProxyUrl). */
static bool is_signing_proxy_url(const char *url) {
    return url_has(url, "githubcopilot") || url_has(url, "copilot-api.") || url_has(url, "zenmux") ||
           (url_has(url, "gateway.ai.cloudflare.com") && url_has(url, "/anthropic")) ||
           (url_has(url, "aiplatform.googleapis.com") && url_has(url, "/publishers/anthropic/")) ||
           url_has(url, "bedrock-runtime.") || url_has(url, "bedrock-mantle.") || url_has(url, ".inference.ai.azure.com") ||
           url_has(url, ".services.ai.azure.com");
}

static bool is_vertex_raw_predict(const char *url) {
    return url_has(url, "aiplatform.googleapis.com") && url_has(url, "/publishers/anthropic/");
}

/* compat with upstream resolveAnthropicPolicy defaults for keys the catalog did not provide */
static bool compat_default(const ai_model_t *m, const char *key) {
    bool official = is_official_url(m->base_url);
    bool signing = official || is_signing_proxy_url(m->base_url);
    if (!strcmp(key, "officialEndpoint")) return official;
    if (!strcmp(key, "signingEndpoint")) return signing;
    if (!strcmp(key, "supportsEagerToolInputStreaming") || !strcmp(key, "supportsLongCacheRetention")) return official;
    if (!strcmp(key, "supportsContextManagement") || !strcmp(key, "supportsOutputEffort") ||
        !strcmp(key, "supportsForcedToolChoice") || !strcmp(key, "supportsSamplingParams") ||
        !strcmp(key, "injectClaudeCodeInstruction"))
        return true;
    if (!strcmp(key, "replayUnsignedThinking")) return !signing && m->reasoning;
    return false;
}

static bool cflag(const ai_model_t *m, const char *key) {
    json_t *v = model_compat_get(m, key);
    if (json_is_bool(v)) return v->u.b;
    return m ? compat_default(m, key) : false;
}

static const char *cstr(const ai_model_t *m, const char *key) {
    return json_sv(model_compat_get(m, key));
}

static const char *thinking_mode(const ai_model_t *m) { return m->thinking ? json_get_str(m->thinking, "mode") : NULL; }

/* -1 unset, 0 false, 1 true */
static int thinking_supports_display(const ai_model_t *m) {
    json_t *v = m->thinking ? json_get(m->thinking, "supportsDisplay") : NULL;
    return json_is_bool(v) ? (v->u.b ? 1 : 0) : -1;
}

static bool efforts_contain(const json_t *arr, const char *name) {
    json_t *it;
    json_arr_foreach(arr, i, it) {
        if (str_eq(json_sv(it), name)) return true;
    }
    return false;
}

/* Clamp to the model's supported efforts: highest supported <= requested, else the lowest supported. */
static ai_effort_t clamp_effort(const ai_model_t *m, ai_effort_t e) {
    json_t *arr = m->thinking ? json_get_arr(m->thinking, "efforts") : NULL;
    if (!arr || json_len(arr) == 0 || efforts_contain(arr, effort_name(e))) return e;
    for (int x = (int)e; x >= EFFORT_MINIMAL; x--)
        if (efforts_contain(arr, effort_name((ai_effort_t)x))) return (ai_effort_t)x;
    for (int x = (int)e; x <= EFFORT_MAX; x++)
        if (efforts_contain(arr, effort_name((ai_effort_t)x))) return (ai_effort_t)x;
    return e;
}

/* mapEffortToAnthropicAdaptiveEffort: effortMap lookup, "minimal" -> "low". Borrowed/static string. */
static const char *adaptive_effort(const ai_model_t *m, ai_effort_t e) {
    e = clamp_effort(m, e);
    const char *mapped = NULL;
    json_t *map = m->thinking ? json_get_obj(m->thinking, "effortMap") : NULL;
    if (map) mapped = json_get_str(map, effort_name(e));
    if (!mapped) mapped = effort_name(e);
    if (!strcmp(mapped, "minimal")) return "low";
    return mapped;
}

static bool uses_adaptive_tag_only(const ai_model_t *m) {
    if (!str_eq(thinking_mode(m), "anthropic-adaptive")) return false;
    json_t *map = json_get_obj(m->thinking, "effortMap");
    json_t *efforts = json_get_arr(m->thinking, "efforts");
    if (!map || !efforts || json_len(efforts) == 0) return false;
    json_t *it;
    json_arr_foreach(efforts, i, it) {
        if (!json_is_str(it) || !str_eq(json_get_str(map, json_sv(it)), "adaptive")) return false;
    }
    return true;
}

static bool uses_budget_thinking(const ai_model_t *m) {
    return !str_eq(thinking_mode(m), "anthropic-adaptive") || cflag(m, "disableAdaptiveThinking");
}

static bool is_adaptive_only(const ai_model_t *m) {
    return str_eq(thinking_mode(m), "anthropic-adaptive") && !cflag(m, "disableAdaptiveThinking") &&
           !uses_adaptive_tag_only(m);
}

static int64_t output_limit(const ai_model_t *m) { return m->max_tokens > 0 ? m->max_tokens : UNKNOWN_MODEL_MAX_OUTPUT; }

static bool str_blank(const char *s) {
    if (!s) return true;
    for (; *s; s++)
        if (!(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')) return false;
    return true;
}

static json_t *text_block(const char *t) {
    json_t *b = json_obj();
    json_set_str(b, "type", "text");
    json_set_str(b, "text", t ? t : "");
    return b;
}

static char *sha256_hex(const void *data, size_t len) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    if (!EVP_Digest(data, len, md, &mdlen, EVP_sha256(), NULL)) return xstrdup("");
    return hex_encode(md, mdlen);
}

/* Anthropic tool_use ids must match ^[a-zA-Z0-9_-]{1,64}$ */
static char *normalize_tool_id(const char *id) {
    if (!id) id = "";
    size_t n = strlen(id);
    if (n > MAX_TOOL_ID_LEN) n = MAX_TOOL_ID_LEN;
    char *out = xmalloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        out[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
    }
    out[n] = '\0';
    return out;
}

/* renderDemotedThinking: thinking rendered as visible text in the target model's dialect */
static char *render_demoted_thinking(const char *model_id, const char *text) {
    if (!text || !*text) return xstrdup("");
    if (str_istr(model_id, "claude")) return xstrdup(text);
    if (str_istr(model_id, "gemini")) return xasprintf("```thinking\n%s\n```", text);
    if (str_istr(model_id, "minimax")) return xasprintf("<thinking>\n%s\n</thinking>", text);
    return xasprintf("<think>\n%s\n</think>", text);
}

/* ======================================================================
 * Claude Code identity
 * ====================================================================== */

static char g_adopted_version[32];

const char *anthropic_claude_code_version(void) {
    const char *env = getenv("PI_AI_CLAUDE_CODE_VERSION");
    if (env && *env) return env;
    if (g_adopted_version[0]) return g_adopted_version;
    return DEFAULT_CLAUDE_CODE_VERSION;
}

static bool parse_semver(const char *s, int v[3]) {
    char tail;
    return s && sscanf(s, "%d.%d.%d%c", &v[0], &v[1], &v[2], &tail) == 3;
}

/* adoptRequiredClaudeCodeVersion: true when the wire version increased (caller retries once). */
static bool adopt_required_version(const char *msg) {
    const char *env = getenv("PI_AI_CLAUDE_CODE_VERSION");
    if ((env && *env) || !msg || !strstr(msg, "claude_code_version_too_old")) return false;
    const char *p = str_istr(msg, "version ");
    while (p) {
        int v[3], cur[3];
        char buf[32];
        const char *q = p + 8;
        size_t n = strspn(q, "0123456789.");
        if (n > 0 && n < sizeof(buf) && str_istr(q + n, " or newer is required") == q + n) {
            memcpy(buf, q, n);
            buf[n] = '\0';
            if (!parse_semver(buf, v) || !parse_semver(anthropic_claude_code_version(), cur)) return false;
            int cmp = v[0] != cur[0] ? v[0] - cur[0] : v[1] != cur[1] ? v[1] - cur[1] : v[2] - cur[2];
            if (cmp <= 0) return false;
            snprintf(g_adopted_version, sizeof(g_adopted_version), "%s", buf);
            return true;
        }
        p = str_istr(p + 1, "version ");
    }
    return false;
}

bool anthropic_is_oauth_token(const char *key) { return key && strstr(key, "sk-ant-oat") != NULL; }

bool anthropic_model_is_oauth(const ai_model_t *m, const char *key) {
    json_t *v = m && m->raw ? json_get(m->raw, "isOAuth") : NULL; /* stream.ts: providerModel.isOAuth before the token shape */
    return json_is_bool(v) ? v->u.b : anthropic_is_oauth_token(key);
}

static const char *stainless_os(void) {
#if defined(__APPLE__)
    return "MacOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#elif defined(_WIN32)
    return "Windows";
#else
    return "Other::unknown";
#endif
}

static const char *stainless_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__i386__)
    return "x86";
#else
    return "other::unknown";
#endif
}

static const char *const STAINLESS_KEYS[] = {"X-Stainless-Arch", "X-Stainless-Lang", "X-Stainless-OS",
                                             "X-Stainless-Package-Version", "X-Stainless-Retry-Count",
                                             "X-Stainless-Runtime", "X-Stainless-Runtime-Version",
                                             "X-Stainless-Timeout"};

static const char *stainless_value(size_t i) {
    switch (i) {
    case 0: return stainless_arch();
    case 1: return "js";
    case 2: return stainless_os();
    case 3: return CLAUDE_CODE_SDK_VERSION;
    case 4: return "0";
    case 5: return "node";
    case 6: return "v26.3.0";
    default: return "600";
    }
}

/* Stable per-install UUID at ~/.omc/install-id (created on first use). Never NULL. */
static const char *install_id(void) {
    static char cached[64];
    if (cached[0]) return cached;
    const char *home = getenv("HOME");
    char *dir = xasprintf("%s/.omc", home && *home ? home : ".");
    char *path = xasprintf("%s/install-id", dir);
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            char *t = str_trim(buf);
            if (strlen(t) == 36) snprintf(cached, sizeof(cached), "%s", t);
            free(t);
        }
        fclose(f);
    }
    if (!cached[0]) {
        char *u = uuid_v4();
        snprintf(cached, sizeof(cached), "%s", u);
        free(u);
        mkdir(dir, 0700);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            ssize_t w = write(fd, cached, strlen(cached));
            (void)w;
            close(fd);
        }
    }
    free(dir);
    free(path);
    return cached;
}

static char *derive_device_id(const char *account_id) {
    const char *iid = install_id();
    if (account_id && *account_id) {
        sb_t b = SB_INIT;
        sb_append(&b, "omp-claude-device-id-v2");
        sb_putc(&b, '\0');
        sb_append(&b, iid);
        sb_putc(&b, '\0');
        sb_append(&b, account_id);
        char *h = sha256_hex(b.s, b.len);
        sb_free(&b);
        return h;
    }
    char *s = xasprintf("omp-claude-device-id-v1:%s", iid);
    char *h = sha256_hex(s, strlen(s));
    free(s);
    return h;
}

/* user_YYY(64 hex)_account_<uuid>_session_<uuid> */
static bool is_cloaking_user_id(const char *s) {
    static const char hex[] = "0123456789abcdefABCDEF";
    if (!s || strncmp(s, "user_", 5) != 0) return false;
    s += 5;
    for (int i = 0; i < 64; i++, s++)
        if (!*s || !strchr(hex, *s)) return false;
    const char *seps[2] = {"_account_", "_session_"};
    for (int k = 0; k < 2; k++) {
        size_t n = strlen(seps[k]);
        if (strncmp(s, seps[k], n) != 0) return false;
        s += n;
        static const int groups[] = {8, 4, 4, 4, 12};
        for (int g = 0; g < 5; g++) {
            for (int i = 0; i < groups[g]; i++, s++)
                if (!*s || !strchr("0123456789abcdef", *s)) return false;
            if (g < 4) {
                if (*s != '-') return false;
                s++;
            }
        }
    }
    return *s == '\0';
}

static char *json_user_id_session(const char *s) {
    if (!s || s[0] != '{') return NULL;
    json_t *j = json_parse_str(s);
    char *out = NULL;
    if (json_is_obj(j)) {
        const char *sid = json_get_str(j, "session_id");
        if (sid && *sid) out = xstrdup(sid);
    }
    json_free(j);
    return out;
}

/* extractClaudeMetadataSessionId */
static char *metadata_session_id(const char *user_id) {
    if (!user_id) return NULL;
    if (is_cloaking_user_id(user_id)) return xstrdup(strstr(user_id, "_session_") + 9);
    return json_user_id_session(user_id);
}

char *anthropic_session_user_id(const char *session_id, const char *account_uuid) {
    json_t *o = json_obj();
    json_set_str(o, "session_id", session_id ? session_id : "");
    if (!str_empty(account_uuid)) {
        json_set_str(o, "account_uuid", account_uuid);
        json_set(o, "device_id", json_str_own(derive_device_id(account_uuid)));
    }
    char *s = json_dump(o, 0);
    json_free(o);
    return s;
}

static const char *metadata_str(const json_t *md, const char *k) {
    const char *v = md ? json_get_str(md, k) : NULL;
    return v && *v ? v : NULL;
}

/* resolveAnthropicMetadataUserId (owned or NULL) */
static char *resolve_metadata_user_id(const json_t *md, bool oauth, const char *session_id) {
    const char *uid = metadata_str(md, "user_id");
    if (uid) {
        char *sid = NULL;
        if (!oauth || is_cloaking_user_id(uid) || (sid = json_user_id_session(uid))) {
            free(sid);
            return xstrdup(uid);
        }
    }
    if (!oauth) return NULL;
    const char *account = metadata_str(md, "account_uuid");
    if (!account) account = metadata_str(md, "accountId");
    if (!account) account = metadata_str(md, "account_id");
    json_t *o = json_obj();
    json_set(o, "device_id", json_str_own(derive_device_id(account)));
    if (session_id && *session_id) json_set_str(o, "session_id", session_id);
    else json_set(o, "session_id", json_str_own(uuid_v4()));
    if (account) json_set_str(o, "account_uuid", account);
    char *s = json_dump(o, 0);
    json_free(o);
    return s;
}

/* JS `text[i]` semantics (UTF-16 code units); lone surrogate halves hash as U+FFFD like Bun's UTF-8 encode. */
static void utf16_char_at(const char *s, size_t want, sb_t *out) {
    size_t unit = 0, len = strlen(s), i = 0;
    while (i < len) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        size_t units = cp > 0xFFFF ? 2 : 1;
        if (want < unit + units) {
            if (units == 2) sb_append(out, "\xEF\xBF\xBD");
            else sb_appendn(out, s + i, (size_t)n);
            return;
        }
        unit += units;
        i += (size_t)n;
    }
    sb_putc(out, '0');
}

static char *billing_header_text(const char *first_user_text) {
    sb_t k = SB_INIT;
    const char *t = first_user_text ? first_user_text : "";
    utf16_char_at(t, 4, &k);
    utf16_char_at(t, 7, &k);
    utf16_char_at(t, 20, &k);
    const char *ver = anthropic_claude_code_version();
    char *seed = xasprintf("59cf53e54c78%s%s", sb_cstr(&k), ver);
    char *h = sha256_hex(seed, strlen(seed));
    h[3] = '\0';
    char *out = xasprintf("%s cc_version=%s.%s; cc_entrypoint=cli; %s;", BILLING_PREFIX, ver, h, CCH_PLACEHOLDER);
    free(h);
    free(seed);
    sb_free(&k);
    return out;
}

/* extractClaudeCodeFirstWireUserMessageText (borrowed) */
static const char *first_user_text(const json_t *messages) {
    json_t *msg;
    json_arr_foreach(messages, i, msg) {
        const char *role = json_get_str(msg, "role");
        if (!str_eq(role, "user") && !str_eq(role, "developer")) continue;
        json_t *c = json_get(msg, "content");
        if (json_is_str(c)) {
            if (!str_blank(json_sv(c))) return json_sv(c);
            continue;
        }
        if (!json_is_arr(c)) continue;
        bool has_image = false;
        json_t *blk;
        json_arr_foreach(c, j, blk) {
            const char *ty = json_get_str(blk, "type");
            if (str_eq(ty, "text") && !str_blank(json_get_str(blk, "text"))) return json_get_str(blk, "text");
            if (str_eq(ty, "image")) has_image = true;
        }
        if (has_image) return "";
    }
    return "";
}

static bool is_builtin_tool_name(const char *name) {
    static const char *const b[] = {"web_search", "code_execution", "text_editor", "computer"};
    for (size_t i = 0; i < ARRAY_LEN(b); i++)
        if (strcasecmp(name, b[i]) == 0) return true;
    return false;
}

static char *encode_tool_name(const ai_model_t *m, const char *name, bool oauth) {
    if (!name) name = "";
    if (cflag(m, "escapeBuiltinToolNames")) return xasprintf(CLAUDE_TOOL_PREFIX "%s", name);
    if (oauth && !is_builtin_tool_name(name)) return xasprintf(CLAUDE_TOOL_PREFIX "%s", name);
    return xstrdup(name);
}

static const char *decode_tool_name(const ai_model_t *m, const char *name, bool oauth) {
    if (!name) return "";
    if ((oauth || cflag(m, "escapeBuiltinToolNames")) && strncasecmp(name, CLAUDE_TOOL_PREFIX, strlen(CLAUDE_TOOL_PREFIX)) == 0)
        return name + strlen(CLAUDE_TOOL_PREFIX);
    return name;
}

/* ======================================================================
 * option resolution (stream.ts mapOptionsForApi, anthropic branch)
 * ====================================================================== */

static const int64_t ANTHROPIC_THINKING[] = {0, 1024, 4096, 8192, 16384, 32768, 32768};

typedef struct {
    bool thinking_enabled;
    int64_t budget;     /* thinkingBudgetTokens (0 => unset) */
    const char *effort; /* AnthropicEffort ("low".."max" | "adaptive") or NULL */
    int64_t max_tokens; /* options.maxTokens (<0 => unset) */
} anth_ro_t;

static int64_t max_tokens_with_budget(int64_t base, int64_t model_max, int64_t budget) {
    int64_t un = base < 0 ? OUTPUT_CAP_WHEN_UNKNOWN : base + budget;
    return model_max > 0 ? MIN(un, model_max) : un;
}

static void resolve_opts(const ai_model_t *m, const ai_stream_opts_t *o, anth_ro_t *r) {
    int64_t base = o->max_tokens > 0 ? o->max_tokens : (m->max_tokens > 0 ? m->max_tokens : -1);
    memset(r, 0, sizeof(*r));
    r->max_tokens = base;
    ai_effort_t e = o->reasoning;
    if (e == EFFORT_OFF || !m->reasoning || o->disable_reasoning) return;
    e = clamp_effort(m, e);
    if ((int)e < EFFORT_MINIMAL || (int)e > EFFORT_MAX) return;
    int64_t budget = ANTHROPIC_THINKING[e];
    json_t *cb = o->thinking_budgets ? json_get(o->thinking_budgets, effort_name(e)) : NULL;
    if (json_is_num(cb)) budget = (int64_t)cb->u.num.d;
    if (budget <= 0) return;
    const char *mode = thinking_mode(m);
    const char *effort = (str_eq(mode, "anthropic-adaptive") || str_eq(mode, "anthropic-budget-effort"))
                             ? adaptive_effort(m, e)
                             : NULL;
    int64_t with = base < 0 ? -1 : max_tokens_with_budget(base, m->max_tokens, budget);
    if (str_eq(mode, "anthropic-adaptive")) {
        r->max_tokens = with;
        r->thinking_enabled = true;
        r->effort = effort;
        return;
    }
    const char *noil = getenv("PI_NO_INTERLEAVED_THINKING");
    if (!str_eq(noil, "1")) {
        if (m->max_tokens > 0 && m->max_tokens < budget + OUTPUT_FALLBACK_BUFFER) budget = m->max_tokens - OUTPUT_FALLBACK_BUFFER;
        if (budget >= ANTHROPIC_THINKING[EFFORT_MINIMAL]) {
            r->max_tokens = with;
            r->thinking_enabled = true;
            r->budget = budget;
            r->effort = effort;
            return;
        }
    }
    int64_t maxt = max_tokens_with_budget(base, m->max_tokens, budget);
    if (maxt < budget + OUTPUT_FALLBACK_BUFFER) budget = maxt - OUTPUT_FALLBACK_BUFFER;
    if (budget < ANTHROPIC_THINKING[EFFORT_MINIMAL]) return;
    r->max_tokens = maxt;
    r->thinking_enabled = true;
    r->budget = budget;
    r->effort = effort;
}

/* mapAnthropicToolChoice on the opts tool_choice: returns "auto"/"none"/"any" (static) or sets *name. */
static const char *map_tool_choice(const json_t *tc, const char **name) {
    *name = NULL;
    if (!tc) return NULL;
    if (json_is_str(tc)) {
        const char *s = json_sv(tc);
        if (!strcmp(s, "required") || !strcmp(s, "any")) return "any";
        if (!strcmp(s, "auto")) return "auto";
        if (!strcmp(s, "none")) return "none";
        return NULL;
    }
    if (!json_is_obj(tc)) return NULL;
    const char *ty = json_get_str(tc, "type");
    const char *n = NULL;
    if (str_eq(ty, "tool")) n = json_get_str(tc, "name");
    else if (str_eq(ty, "function")) {
        json_t *fn = json_get_obj(tc, "function");
        n = fn ? json_get_str(fn, "name") : json_get_str(tc, "name");
    }
    if (!n || !*n) return NULL;
    *name = n;
    return "tool";
}

/* ======================================================================
 * cache control
 * ====================================================================== */

static json_t *resolve_cache_control(const ai_model_t *m, const ai_stream_opts_t *o, bool oauth) {
    bool long_ok = cflag(m, "supportsLongCacheRetention");
    const char *ret = o->cache_retention;
    if (!ret) {
        const char *env = getenv("PI_CACHE_RETENTION");
        if (str_eq(env, "long") || str_eq(env, "short") || str_eq(env, "none")) ret = env;
    }
    if (!ret) ret = oauth && long_ok ? "long" : "short";
    if (!strcmp(ret, "none")) return NULL;
    json_t *cc = json_obj();
    json_set_str(cc, "type", "ephemeral");
    if (!strcmp(ret, "long") && long_ok) json_set_str(cc, "ttl", "1h");
    return cc;
}

static bool block_cacheable(const json_t *blk) {
    const char *t = json_get_str(blk, "type");
    return !(str_eq(t, "thinking") || str_eq(t, "redacted_thinking") || str_eq(t, "fallback") ||
             str_eq(t, "tool_addition") || str_eq(t, "tool_removal"));
}

static bool apply_cc_to_message(json_t *msg, const json_t *cc) {
    json_t *c = json_get(msg, "content");
    if (json_is_str(c)) {
        json_t *b = text_block(json_sv(c));
        json_set(b, "cache_control", json_clone(cc));
        json_t *arr = json_arr();
        json_push(arr, b);
        json_set(msg, "content", arr);
        return true;
    }
    if (!json_is_arr(c)) return false;
    for (size_t i = json_len(c); i > 0; i--) {
        json_t *blk = json_at(c, i - 1);
        if (!block_cacheable(blk)) continue;
        if (!json_is_null(json_get(blk, "cache_control"))) return false;
        json_set(blk, "cache_control", json_clone(cc));
        return true;
    }
    return false;
}

#define CONV_KEY "\x01omc_conv"
#define DEV_KEY "\x01omc_dev"

static bool is_conv_user(const json_t *msg) { return json_get_bool(msg, CONV_KEY, false); }

static int count_head_breakpoints(const json_t *system, const json_t *tools) {
    int n = 0;
    json_t *it;
    json_arr_foreach(system, i, it) {
        if (!json_is_null(json_get(it, "cache_control"))) n++;
    }
    json_arr_foreach(tools, j, it) {
        if (!json_is_null(json_get(it, "cache_control"))) n++;
    }
    return n;
}

/* applyPromptCaching (no per-call / turn-scoped / tool-control messages exist in omc histories) */
static void apply_prompt_caching(json_t *msgs, const json_t *system, const json_t *tools, const json_t *cc) {
    if (!cc) return;
    int budget = MAX_BREAKPOINTS - count_head_breakpoints(system, tools);
    size_t n = json_len(msgs);
    if (budget <= 0 || n == 0) return;
    size_t trailing = n - 1;
    json_t *tm = json_at(msgs, trailing);
    bool pad = str_eq(json_get_str(tm, "role"), "user") && str_eq(json_sv(json_get(tm, "content")), "Continue.") &&
               !is_conv_user(tm) && trailing > 0 && str_eq(json_get_str(json_at(msgs, trailing - 1), "role"), "assistant");
    long msg_end = pad ? (long)trailing - 1 : (long)trailing;
    long cand[2];
    int ncand = 0;
    long stable_turns = 0;
    for (long i = msg_end; i >= 0; i--) {
        json_t *mm = json_at(msgs, (size_t)i);
        if (ncand < 2) cand[ncand++] = i;
        if (is_conv_user(mm)) stable_turns++;
    }
    int applied = 0;
    if (ncand > 0 && applied < budget && apply_cc_to_message(json_at(msgs, (size_t)cand[0]), cc)) applied++;
    if (stable_turns >= DECIMATION_INTERVAL) {
        long since = stable_turns % DECIMATION_INTERVAL;
        for (long i = msg_end; i >= 0 && applied < budget; i--) {
            json_t *mm = json_at(msgs, (size_t)i);
            if (!is_conv_user(mm)) continue;
            if (since > 0) {
                since--;
                continue;
            }
            since = DECIMATION_INTERVAL - 1;
            if (apply_cc_to_message(mm, cc)) applied++;
        }
    }
    for (int k = 0; k < ncand && applied < budget; k++)
        if (apply_cc_to_message(json_at(msgs, (size_t)cand[k]), cc)) applied++;
}

/* applyHeadCaching */
static void apply_head_caching(json_t *system, json_t *tools, const json_t *cc) {
    if (!cc) return;
    if (json_len(tools) > 0) {
        bool any = false;
        json_t *t;
        json_arr_foreach(tools, i, t) {
            if (!json_is_null(json_get(t, "cache_control"))) any = true;
        }
        if (!any) {
            for (size_t i = json_len(tools); i > 0; i--) {
                json_t *tool = json_at(tools, i - 1);
                if (json_get_bool(tool, "defer_loading", false)) continue;
                json_set(tool, "cache_control", json_clone(cc));
                break;
            }
        }
    }
    size_t n = json_len(system);
    if (n == 0) return;
    size_t suffix = n;
    for (size_t i = 0; i < n; i++) {
        const char *t = json_get_str(json_at(system, i), "text");
        if (t && (str_starts(t, "<memories>") || str_starts(t, "<project-context>"))) {
            suffix = i;
            break;
        }
    }
    size_t anchor = suffix == 0 ? n - 1 : suffix - 1;
    json_t *a = json_at(system, anchor);
    if (a && json_is_null(json_get(a, "cache_control"))) {
        json_t *b;
        json_arr_foreach(system, i, b) { json_del(b, "cache_control"); }
        json_set(a, "cache_control", json_clone(cc));
    }
}

/* ======================================================================
 * tool schema normalization (normalizeAnthropicToolSchema)
 * ====================================================================== */

static const char *const UNIVERSAL_KEEP[] = {"$ref",  "$defs", "$schema", "definitions", "type",    "anyOf", "allOf",
                                             "enum",  "const", "description", "title",   "default", "nullable"};
static const char *const OBJECT_KEEP[] = {"properties", "required", "additionalProperties"};
static const char *const ARRAY_KEEP[] = {"items", "prefixItems", "minItems"};
static const char *const STRING_FORMATS[] = {"date-time", "time", "date", "duration", "email",
                                             "hostname",  "uri",  "ipv4", "ipv6",     "uuid"};

static bool in_list(const char *k, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(k, list[i])) return true;
    return false;
}

static bool is_combinator(const char *k) { return !strcmp(k, "anyOf") || !strcmp(k, "allOf") || !strcmp(k, "oneOf"); }

static const char *scalar_type(const json_t *type) {
    if (json_is_str(type)) return json_sv(type);
    json_t *it;
    json_arr_foreach(type, i, it) {
        if (json_is_str(it) && strcmp(json_sv(it), "null") != 0) return json_sv(it);
    }
    return NULL;
}

static const char *effective_type(const json_t *s) {
    const char *t = scalar_type(json_get(s, "type"));
    if (t) return t;
    if (json_is_obj(json_get(s, "properties"))) return "object";
    if (json_get(s, "items") || json_is_arr(json_get(s, "prefixItems"))) return "array";
    return NULL;
}

/* spillToDescription "spill" format: `{key: <json>, ...}` appended after "\n\n" */
static void spill_to_description(json_t *node, json_t *spill) {
    if (json_len(spill) == 0) return;
    sb_t b = SB_INIT;
    const char *existing = json_get_str(node, "description");
    if (existing && *existing) {
        sb_append(&b, existing);
        sb_append(&b, "\n\n");
    }
    sb_putc(&b, '{');
    const char *k;
    json_t *v;
    json_obj_foreach(spill, i, k, v) {
        if (i) sb_append(&b, ", ");
        sb_append(&b, k);
        sb_append(&b, ": ");
        json_dump_sb(&b, v, 0);
    }
    sb_putc(&b, '}');
    json_set(node, "description", json_str_own(sb_detach(&b)));
}

static json_t *anth_norm_node(const json_t *s, bool root, int depth);

static json_t *anth_norm_map(const json_t *map, int depth) {
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(map, i, k, v) { json_set(out, k, anth_norm_node(v, false, depth + 1)); }
    return out;
}

static json_t *anth_norm_node(const json_t *s, bool root, int depth) {
    if (depth > SCHEMA_MAX_DEPTH) return json_obj();
    if (json_is_arr(s)) {
        json_t *out = json_arr();
        json_t *it;
        json_arr_foreach(s, i, it) { json_push(out, anth_norm_node(it, false, depth + 1)); }
        return out;
    }
    if (!json_is_obj(s)) return json_clone(s);
    const char *st = effective_type(s);
    json_t *res = json_obj(), *spill = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(s, i, k, v) {
        bool root_comb = root && is_combinator(k);
        bool keep = !root_comb && (in_list(k, UNIVERSAL_KEEP, ARRAY_LEN(UNIVERSAL_KEEP)) ||
                                   (str_eq(st, "object") && in_list(k, OBJECT_KEEP, ARRAY_LEN(OBJECT_KEEP))) ||
                                   (str_eq(st, "array") && in_list(k, ARRAY_KEEP, ARRAY_LEN(ARRAY_KEEP))) ||
                                   (str_eq(st, "string") && !strcmp(k, "format")));
        json_set(keep ? res : spill, k, json_clone(v));
    }
    if (str_eq(st, "string")) {
        const char *fmt = json_get_str(res, "format");
        json_t *f = json_get(res, "format");
        if (f && (!fmt || !in_list(fmt, STRING_FORMATS, ARRAY_LEN(STRING_FORMATS)))) json_set(spill, "format", json_take(res, "format"));
    }
    if (str_eq(st, "array")) {
        json_t *mi = json_get(res, "minItems");
        if (mi && !(json_is_num(mi) && (mi->u.num.d == 0 || mi->u.num.d == 1))) json_set(spill, "minItems", json_take(res, "minItems"));
    }
    if (str_eq(st, "object") && !json_get(res, "additionalProperties")) json_set_bool(res, "additionalProperties", false);
    json_t *p = json_get(res, "properties");
    if (json_is_obj(p)) json_set(res, "properties", anth_norm_map(p, depth));
    json_t *ap = json_get(res, "additionalProperties");
    if (json_is_obj(ap)) {
        json_t *n = anth_norm_node(ap, false, depth + 1);
        if (json_is_obj(n) && json_len(n) == 0) {
            json_free(n);
            json_set_bool(res, "additionalProperties", true);
        } else {
            json_set(res, "additionalProperties", n);
        }
    }
    json_t *items = json_get(res, "items");
    if (json_is_arr(items) || json_is_obj(items)) json_set(res, "items", anth_norm_node(items, false, depth + 1));
    json_t *pi = json_get(res, "prefixItems");
    if (json_is_arr(pi)) json_set(res, "prefixItems", anth_norm_node(pi, false, depth + 1));
    static const char *const combs[] = {"anyOf", "allOf", "oneOf"};
    for (size_t i = 0; i < 3; i++) {
        json_t *vv = json_get(res, combs[i]);
        if (json_is_arr(vv)) json_set(res, combs[i], anth_norm_node(vv, false, depth + 1));
    }
    static const char *const defs[] = {"$defs", "definitions"};
    for (size_t i = 0; i < 2; i++) {
        json_t *d = json_get(res, defs[i]);
        if (json_is_obj(d)) json_set(res, defs[i], anth_norm_map(d, depth));
    }
    spill_to_description(res, spill);
    json_free(spill);
    return res;
}

static bool foldable_variant(const json_t *v, bool conj) {
    if (!json_is_obj(v)) return false;
    const char *t = effective_type(v);
    if (t && strcmp(t, "object") != 0) return false;
    if (json_get(v, "$ref") || json_get(v, "not") || json_get(v, "if")) return false;
    if (json_get(v, "anyOf") || json_get(v, "allOf") || json_get(v, "oneOf")) return false;
    json_t *ap = json_get(v, "additionalProperties");
    return conj || !ap || (json_is_bool(ap) && !ap->u.b);
}

/* foldAnthropicRootCombinators (owned result) */
static json_t *fold_root_combinators(const json_t *schema) {
    const char *rt = effective_type(schema);
    static const char *const keys[] = {"anyOf", "allOf", "oneOf"};
    bool any = false;
    for (int i = 0; i < 3; i++) {
        json_t *a = json_get(schema, keys[i]);
        if (json_is_arr(a) && json_len(a) > 0) any = true;
    }
    if ((rt && strcmp(rt, "object") != 0) || !any) return json_clone(schema);
    json_t *props = json_is_obj(json_get(schema, "properties")) ? json_clone(json_get(schema, "properties")) : json_obj();
    json_t *required = json_arr();
    json_t *it;
    json_arr_foreach(json_get(schema, "required"), i, it) {
        if (json_is_str(it)) json_push_str(required, json_sv(it));
    }
    bool open = false;
    for (int ki = 0; ki < 3; ki++) {
        json_t *vars = json_get(schema, keys[ki]);
        if (!json_is_arr(vars) || json_len(vars) == 0) continue;
        bool conj = !strcmp(keys[ki], "allOf");
        bool all_fold = true;
        json_arr_foreach(vars, i, it) {
            if (!foldable_variant(it, conj)) all_fold = false;
        }
        if (!all_fold) {
            open = true;
            continue;
        }
        json_t *declared = json_obj(); /* name -> [schemas] */
        strvec_t shared = {0};
        bool shared_init = false;
        json_arr_foreach(vars, i, it) {
            const char *pn;
            json_t *ps;
            json_obj_foreach(json_get_obj(it, "properties"), j, pn, ps) {
                json_t *decl = json_get(declared, pn);
                if (!decl) {
                    decl = json_arr();
                    json_set(declared, pn, decl);
                }
                bool dup = false;
                json_t *e;
                json_arr_foreach(decl, q, e) {
                    if (json_equal(e, ps)) dup = true;
                }
                if (!dup) json_push(decl, json_clone(ps));
            }
            strvec_t vr = {0};
            json_t *r;
            json_arr_foreach(json_get(it, "required"), q, r) {
                if (json_is_str(r)) strvec_push(&vr, json_sv(r));
            }
            if (conj) {
                for (size_t q = 0; q < vr.n; q++) {
                    bool have = false;
                    json_t *x;
                    json_arr_foreach(required, z, x) {
                        if (str_eq(json_sv(x), vr.v[q])) have = true;
                    }
                    if (!have) json_push_str(required, vr.v[q]);
                }
            } else if (!shared_init) {
                for (size_t q = 0; q < vr.n; q++) strvec_push(&shared, vr.v[q]);
                shared_init = true;
            } else {
                strvec_t next = {0};
                for (size_t q = 0; q < vr.n; q++)
                    if (strvec_contains(&shared, vr.v[q])) strvec_push(&next, vr.v[q]);
                strvec_free(&shared);
                shared = next;
            }
            strvec_free(&vr);
            json_t *ap = json_get(it, "additionalProperties");
            if (ap && !(json_is_bool(ap) && !ap->u.b)) open = true;
        }
        for (size_t q = 0; q < shared.n; q++) {
            bool have = false;
            json_t *x;
            json_arr_foreach(required, z, x) {
                if (str_eq(json_sv(x), shared.v[q])) have = true;
            }
            if (!have) json_push_str(required, shared.v[q]);
        }
        strvec_free(&shared);
        const char *pn;
        json_t *decl;
        json_obj_foreach(declared, j, pn, decl) {
            if (json_get(props, pn)) continue;
            if (json_len(decl) == 1) {
                json_set(props, pn, json_clone(json_at(decl, 0)));
            } else {
                json_t *w = json_obj();
                json_set(w, conj ? "allOf" : "anyOf", json_clone(decl));
                json_set(props, pn, w);
            }
        }
        json_free(declared);
    }
    json_t *folded = json_clone(schema);
    if (json_len(props) > 0 || json_is_obj(json_get(schema, "properties"))) json_set(folded, "properties", props);
    else json_free(props);
    if (json_len(required) > 0) json_set(folded, "required", required);
    else json_free(required);
    if (open && !json_get(folded, "additionalProperties") && json_is_obj(json_get(folded, "properties")))
        json_set_bool(folded, "additionalProperties", true);
    return folded;
}

json_t *anthropic_normalize_tool_schema(const json_t *schema) {
    json_t *root = json_is_obj(schema) ? fold_root_combinators(schema) : json_clone(schema);
    json_t *out = anth_norm_node(root, true, 0);
    json_free(root);
    return out;
}

/* buildAnthropicBaseToolInputSchema */
static json_t *tool_input_schema(const json_t *params) {
    json_t *s = json_is_obj(params) ? json_clone(params) : json_obj();
    json_set_str(s, "type", "object");
    json_t *p = json_get(s, "properties");
    if (!json_is_obj(p)) json_set(s, "properties", json_obj());
    json_t *req = json_arr(), *it;
    json_arr_foreach(json_get(s, "required"), i, it) {
        if (json_is_str(it)) json_push_str(req, json_sv(it));
    }
    json_set(s, "required", req);
    json_t *out = anthropic_normalize_tool_schema(s);
    json_free(s);
    return out;
}

static bool eager_tool_streaming(const ai_model_t *m, const char *base_url) {
    if (!cflag(m, "supportsEagerToolInputStreaming")) return false;
    if (is_official_url(base_url)) return true;
    return !cflag(m, "officialEndpoint");
}

static json_t *convert_tools(const ai_model_t *m, const json_t *tools, bool oauth, const char *base_url) {
    json_t *out = json_arr();
    bool eager = eager_tool_streaming(m, base_url);
    json_t *t;
    json_arr_foreach(tools, i, t) {
        const char *name = json_get_str(t, "name");
        json_t *w = json_obj();
        json_set(w, "name", json_str_own(encode_tool_name(m, name, oauth)));
        const char *desc = json_get_str(t, "description");
        json_set_str(w, "description", desc ? desc : "");
        json_set(w, "input_schema", tool_input_schema(json_get(t, "parameters")));
        if (eager) json_set_bool(w, "eager_input_streaming", !str_eq(name, "yield"));
        if (json_get_bool(t, "deferLoading", false)) json_set_bool(w, "defer_loading", true);
        json_push(out, w);
    }
    return out;
}

/* ======================================================================
 * message conversion (convertAnthropicMessages)
 * ====================================================================== */

static const char *norm_image_mime(const char *mime, char buf[32]) {
    if (!mime) return NULL;
    char *t = str_trim(mime);
    char *l = str_lower(t);
    free(t);
    const char *r = NULL;
    if (!strcmp(l, "image/jpg") || !strcmp(l, "image/jpeg")) r = "image/jpeg";
    else if (!strcmp(l, "image/png")) r = "image/png";
    else if (!strcmp(l, "image/gif")) r = "image/gif";
    else if (!strcmp(l, "image/webp")) r = "image/webp";
    free(l);
    if (r) snprintf(buf, 32, "%s", r);
    return r ? buf : NULL;
}

/* convertContentBlocks: returns array of blocks, or a string (non-vision models). Owned. */
static json_t *convert_content_blocks(const json_t *content, bool supports_images) {
    json_t *blocks = json_arr();
    bool saw_text = false, saw_image = false;
    json_t *blk;
    json_arr_foreach(content, i, blk) {
        const char *ty = json_get_str(blk, "type");
        if (str_eq(ty, "text")) {
            const char *t = json_get_str(blk, "text");
            if (str_blank(t)) continue;
            saw_text = true;
            json_push(blocks, text_block(t));
            continue;
        }
        if (!str_eq(ty, "image")) continue;
        if (!supports_images) {
            json_push(blocks, text_block(NON_VISION_IMAGE_PLACEHOLDER));
            continue;
        }
        json_t *src = json_obj();
        const char *url = json_get_str(blk, "url");
        if (url && *url) {
            json_set_str(src, "type", "url");
            json_set_str(src, "url", url);
        } else {
            char mb[32];
            const char *mt = norm_image_mime(json_get_str(blk, "mimeType"), mb);
            if (!mt) {
                json_free(src);
                char *s = xasprintf("[unsupported image: %s]", json_get_str(blk, "mimeType") ? json_get_str(blk, "mimeType") : "");
                json_push(blocks, text_block(s));
                free(s);
                continue;
            }
            json_set_str(src, "type", "base64");
            json_set_str(src, "media_type", mt);
            const char *d = json_get_str(blk, "data");
            json_set_str(src, "data", d ? d : "");
        }
        saw_image = true;
        json_t *ib = json_obj();
        json_set_str(ib, "type", "image");
        json_set(ib, "source", src);
        json_push(blocks, ib);
    }
    if (!supports_images) {
        sb_t b = SB_INIT;
        json_arr_foreach(blocks, i, blk) {
            if (i) sb_putc(&b, '\n');
            sb_append(&b, json_get_str(blk, "text"));
        }
        json_free(blocks);
        return json_str_own(sb_detach(&b));
    }
    if (saw_image && !saw_text) {
        json_t *nb = json_arr();
        json_push(nb, text_block("(see attached image)"));
        json_arr_foreach(blocks, i, blk) { json_push(nb, json_clone(blk)); }
        json_free(blocks);
        blocks = nb;
    }
    return blocks;
}

static json_t *build_tool_result(const ai_model_t *m, const json_t *msg, json_t *hoisted) {
    json_t *content = convert_content_blocks(json_get(msg, "content"), m->input_image);
    bool is_err = json_get_bool(msg, "isError", false);
    if (is_err && json_is_arr(content)) {
        json_t *texts = json_arr(), *b;
        bool had_img = false;
        json_arr_foreach(content, i, b) {
            if (str_eq(json_get_str(b, "type"), "image")) {
                had_img = true;
                json_push(hoisted, json_clone(b));
            } else {
                json_push(texts, json_clone(b));
            }
        }
        if (had_img) {
            json_free(content);
            content = texts;
        } else {
            json_free(texts);
        }
    }
    if (json_is_arr(content) && json_len(content) == 0) {
        json_free(content);
        content = json_str("");
    }
    if (is_err && ((json_is_str(content) && str_blank(json_sv(content))) || (json_is_arr(content) && json_len(content) == 0))) {
        bool was_str = json_is_str(content);
        json_free(content);
        if (was_str) content = json_str(EMPTY_ERROR_TOOL_RESULT_TEXT);
        else {
            content = json_arr();
            json_push(content, text_block(EMPTY_ERROR_TOOL_RESULT_TEXT));
        }
    }
    json_t *blk = json_obj();
    json_set_str(blk, "type", "tool_result");
    char *id = normalize_tool_id(json_get_str(msg, "toolCallId"));
    json_set_str(blk, "tool_use_id", id);
    json_set(blk, "content", content);
    json_t *ie = json_get(msg, "isError");
    if (json_is_bool(ie)) json_set_bool(blk, "is_error", ie->u.b);
    if (cflag(m, "requiresToolResultId")) json_set_str(blk, "id", id);
    free(id);
    return blk;
}

static json_t *thinking_wire(const char *thinking, const char *sig) {
    json_t *b = json_obj();
    json_set_str(b, "type", "thinking");
    json_set_str(b, "thinking", thinking ? thinking : "");
    json_set_str(b, "signature", sig ? sig : "");
    return b;
}

static json_t *convert_assistant(const ai_model_t *m, const json_t *msg, bool oauth) {
    json_t *content = json_get(msg, "content");
    json_t *blocks = json_arr(), *blk;
    bool has_signed = false;
    json_arr_foreach(content, i, blk) {
        if (str_eq(json_get_str(blk, "type"), "thinking") && !str_blank(json_get_str(blk, "thinkingSignature"))) has_signed = true;
    }
    bool replay_unsigned = cflag(m, "replayUnsignedThinking");
    json_arr_foreach(content, i, blk) {
        const char *ty = json_get_str(blk, "type");
        if (str_eq(ty, "text")) {
            const char *t = json_get_str(blk, "text");
            if (str_blank(t)) continue;
            json_push(blocks, text_block(t));
        } else if (str_eq(ty, "thinking")) {
            const char *th = json_get_str(blk, "thinking");
            const char *sig = json_get_str(blk, "thinkingSignature");
            bool signed_ = !str_blank(sig);
            if (!th) th = "";
            if (str_blank(th) && (!has_signed || !signed_)) continue;
            if (signed_) {
                json_push(blocks, thinking_wire(th, sig));
            } else if (!has_signed && replay_unsigned) {
                json_push(blocks, thinking_wire(th, ""));
            } else {
                char *d = render_demoted_thinking(m->id, th);
                json_push(blocks, text_block(d));
                free(d);
            }
        } else if (str_eq(ty, "redactedThinking")) {
            const char *d = json_get_str(blk, "data");
            if (str_blank(d)) continue;
            json_t *b = json_obj();
            json_set_str(b, "type", "redacted_thinking");
            json_set_str(b, "data", d);
            json_push(blocks, b);
        } else if (str_eq(ty, "toolCall")) {
            json_t *b = json_obj();
            json_set_str(b, "type", "tool_use");
            json_set(b, "id", json_str_own(normalize_tool_id(json_get_str(blk, "id"))));
            json_set(b, "name", json_str_own(encode_tool_name(m, json_get_str(blk, "name"), oauth)));
            json_t *args = json_get(blk, "arguments");
            json_set(b, "input", json_is_obj(args) ? json_clone(args) : json_obj());
            json_push(blocks, b);
        }
    }
    /* stable-partition non-tool_use blocks before tool_use when anything but thinking follows a tool_use */
    bool saw_tool = false, partition = false;
    json_arr_foreach(blocks, i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "tool_use")) saw_tool = true;
        else if (saw_tool && !str_eq(t, "thinking") && !str_eq(t, "redacted_thinking")) partition = true;
    }
    if (partition) {
        json_t *nb = json_arr();
        for (int pass = 0; pass < 2; pass++) {
            json_arr_foreach(blocks, i, blk) {
                bool tu = str_eq(json_get_str(blk, "type"), "tool_use");
                if (tu == (pass == 1)) json_push(nb, json_clone(blk));
            }
        }
        json_free(blocks);
        blocks = nb;
    }
    return blocks;
}

static json_t *wire_msg(const char *role, json_t *content) {
    json_t *p = json_obj();
    json_set_str(p, "role", role);
    json_set(p, "content", content);
    return p;
}

static char *resolve_base_url(const ai_model_t *m);

/* supportsAnthropicCompaction (anthropic-compaction.ts) for the model's own route */
bool anthropic_compaction_supported(const ai_model_t *m) {
    if (!cflag(m, "supportsServerCompaction")) return false;
    const json_t *rc = json_get_obj(m->raw, "remoteCompaction");
    if (rc && json_get(rc, "enabled") && !json_get_bool(rc, "enabled", true)) return false;
    char *route = resolve_base_url(m);
    bool official = is_official_url(route);
    bool ok = false;
    if (cflag(m, "bedrockMessagesApi")) ok = true;
    else if (official || (route && (strstr(route, "aiplatform.googleapis.com") || strstr(route, ".services.ai.azure.com"))))
        ok = cflag(m, "firstPartyProvider") || str_eq(m->provider, "google-vertex") || json_get_bool(rc, "enabled", false);
    free(route);
    return ok;
}

/* a compaction summary replays as the provider's native `compaction` block (compactionBlockParam) */
static json_t *compaction_replay_block(const ai_model_t *m, const json_t *msg) {
    const json_t *pp = json_get_obj(msg, "providerPayload");
    if (!str_eq(json_get_str(pp, "type"), "anthropicCompaction") || !str_eq(json_get_str(pp, "provider"), m->provider)) return NULL;
    const char *content = json_get_str(pp, "content");
    if (!content || !*content || !json_get_str(pp, "signature")) return NULL;
    json_t *b = json_obj();
    json_set_str(b, "type", "compaction");
    json_set_str(b, "content", content);
    json_set_str(b, "signature", json_get_str(pp, "signature"));
    if (json_get_str(pp, "encryptedContent")) json_set_str(b, "encrypted_content", json_get_str(pp, "encryptedContent"));
    if (json_is_arr(json_get(pp, "toolChanges"))) json_set(b, "tool_changes", json_clone(json_get(pp, "toolChanges")));
    return b;
}

static json_t *wire_msg(const char *role, json_t *content);

static json_t *convert_messages(const ai_model_t *m, const json_t *msgs, bool oauth, bool thinking_enabled, bool replay,
                                bool compaction_request) {
    json_t *params = json_arr();
    /* harness file metadata queued behind a replayed compaction block: it lands before the first message created
     * after the summary, or at the end */
    char *pending_files = NULL;
    int64_t pending_after = 0;
    size_t n = json_len(msgs);
    bool pads = thinking_enabled && cflag(m, "requiresThinkingPresence");
    long window_start = -1;
    if (pads) {
        for (size_t i = n; i > 0; i--) {
            json_t *mm = json_at(msgs, i - 1);
            const char *r = json_get_str(mm, "role");
            if (!str_eq(r, "user") && !str_eq(r, "developer")) continue;
            json_t *c = json_get(mm, "content");
            bool has_text = false;
            if (json_is_str(c)) has_text = !str_blank(json_sv(c));
            else {
                json_t *b;
                json_arr_foreach(c, j, b) {
                    if (str_eq(json_get_str(b, "type"), "text") && !str_blank(json_get_str(b, "text"))) has_text = true;
                }
            }
            if (has_text) {
                window_start = (long)(i - 1);
                break;
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        json_t *msg = json_at(msgs, i);
        const char *role = json_get_str(msg, "role");
        if (pending_files && json_get_int(msg, "timestamp", 0) > pending_after && !str_eq(role, "toolResult")) {
            json_push(params, wire_msg("user", json_str(pending_files)));
            free(pending_files);
            pending_files = NULL;
        }
        json_t *cblock = replay && (str_eq(role, "user") || str_eq(role, "developer")) ? compaction_replay_block(m, msg) : NULL;
        if (cblock) {
            json_t *c = json_arr();
            json_push(c, cblock);
            json_push(params, wire_msg("assistant", c));
            const json_t *pp = json_get_obj(msg, "providerPayload");
            const char *ft = json_get_str(pp, "filesText");
            free(pending_files);
            pending_files = ft && *ft ? xstrdup(ft) : NULL;
            pending_after = json_get_int(msg, "timestamp", 0);
            continue;
        }
        if (str_eq(role, "user") || str_eq(role, "developer")) {
            json_t *c = json_get(msg, "content");
            json_t *content;
            if (json_is_str(c)) {
                if (str_blank(json_sv(c))) continue;
                content = json_clone(c);
            } else {
                content = convert_content_blocks(c, m->input_image);
                if ((json_is_str(content) && str_blank(json_sv(content))) || (json_is_arr(content) && json_len(content) == 0)) {
                    json_free(content);
                    continue;
                }
            }
            json_t *p = wire_msg("user", content);
            const char *attr = json_get_str(msg, "attribution");
            if (str_eq(role, "developer") && !str_eq(attr, "user")) json_set_bool(p, DEV_KEY, true);
            bool agent_authored = json_get_bool(msg, "synthetic", false) || str_eq(attr, "agent");
            if (str_eq(role, "user") && !agent_authored) json_set_bool(p, CONV_KEY, true);
            json_push(params, p);
        } else if (str_eq(role, "assistant")) {
            json_t *blocks = convert_assistant(m, msg, oauth);
            if (json_len(blocks) == 0) {
                json_free(blocks);
                continue;
            }
            if (pads && (long)i > window_start) {
                bool has_th = false;
                json_t *b;
                json_arr_foreach(blocks, j, b) {
                    if (str_eq(json_get_str(b, "type"), "thinking")) has_th = true;
                }
                if (!has_th) {
                    json_t *nb = json_arr();
                    json_push(nb, thinking_wire("(no reasoning was returned for this turn)", ""));
                    json_arr_foreach(blocks, j, b) { json_push(nb, json_clone(b)); }
                    json_free(blocks);
                    blocks = nb;
                }
            }
            json_push(params, wire_msg("assistant", blocks));
        } else if (str_eq(role, "toolResult")) {
            json_t *results = json_arr(), *hoisted = json_arr();
            json_push(results, build_tool_result(m, msg, hoisted));
            while (i + 1 < n && str_eq(json_get_str(json_at(msgs, i + 1), "role"), "toolResult")) {
                i++;
                json_push(results, build_tool_result(m, json_at(msgs, i), hoisted));
            }
            if (json_len(hoisted) > 0) {
                json_push(results, text_block("Attached image(s) from the tool result(s) above:"));
                json_t *h;
                json_arr_foreach(hoisted, j, h) { json_push(results, json_clone(h)); }
            }
            json_free(hoisted);
            json_push(params, wire_msg("user", results));
        }
    }
    if (pending_files) {
        json_push(params, wire_msg("user", json_str(pending_files)));
        free(pending_files);
    }
    /* developer -> mid-conversation system upgrade */
    if (cflag(m, "supportsMidConversationSystem")) {
        for (size_t i = json_len(params); i > 0; i--) {
            json_t *p = json_at(params, i - 1);
            if (!json_get_bool(p, DEV_KEY, false)) continue;
            size_t idx = i - 1;
            bool follows_user = idx > 0 && str_eq(json_get_str(json_at(params, idx - 1), "role"), "user");
            bool last_or_asst = idx + 1 >= json_len(params) ||
                                str_eq(json_get_str(json_at(params, idx + 1), "role"), "assistant");
            json_t *c = json_get(p, "content");
            bool compat = json_is_str(c);
            if (!compat) {
                compat = true;
                json_t *b;
                json_arr_foreach(c, j, b) {
                    if (!str_eq(json_get_str(b, "type"), "text")) compat = false;
                }
            }
            if (follows_user && last_or_asst && compat) json_set_str(p, "role", "system");
        }
    }
    /* a replayed compaction block opens the assistant response it was produced in */
    for (size_t i = json_len(params); i-- > 1;) {
        json_t *cur = json_at(params, i - 1), *next = json_at(params, i);
        json_t *cc = json_get(cur, "content"), *nc = json_get(next, "content");
        if (!str_eq(json_get_str(cur, "role"), "assistant") || !str_eq(json_get_str(next, "role"), "assistant") || !json_is_arr(cc) ||
            json_len(cc) != 1 || !str_eq(json_get_str(json_at(cc, 0), "type"), "compaction") || !json_is_arr(nc))
            continue;
        json_arr_insert(nc, 0, json_clone(json_at(cc, 0)));
        json_free(json_arr_remove(params, i - 1));
    }
    /* repair adjacent assistants */
    for (size_t i = json_len(params); i > 1; i--) {
        if (str_eq(json_get_str(json_at(params, i - 1), "role"), "assistant") &&
            str_eq(json_get_str(json_at(params, i - 2), "role"), "assistant")) {
            json_t *na = json_arr();
            size_t len = json_len(params);
            for (size_t k = 0; k < len; k++) {
                if (k == i - 1) json_push(na, wire_msg("user", json_str("Continue.")));
                json_push(na, json_arr_remove(params, 0));
            }
            json_free(params);
            params = na;
        }
    }
    if (compaction_request) return params; /* the request ends inside the conversation */
    size_t np = json_len(params);
    if (np > 0 && str_eq(json_get_str(json_at(params, np - 1), "role"), "assistant"))
        json_push(params, wire_msg("user", json_str("Continue.")));
    return params;
}

/* ======================================================================
 * request params (buildParams)
 * ====================================================================== */

static json_t *build_system_blocks(const ai_model_t *m, const ai_context_t *ctx, bool oauth, const json_t *cc) {
    json_t *prompts = json_arr(), *it;
    bool has_billing = false;
    json_arr_foreach(ctx->system_prompt, i, it) {
        if (json_is_str(it) && !str_blank(json_sv(it))) {
            json_push(prompts, text_block(json_sv(it)));
            if (str_starts(json_sv(it), BILLING_PREFIX)) has_billing = true;
        }
    }
    bool inject = oauth && cflag(m, "injectClaudeCodeInstruction");
    if (inject && !has_billing) {
        json_t *out = json_arr();
        char *billing = billing_header_text(first_user_text(ctx->messages));
        json_push(out, text_block(billing));
        free(billing);
        json_t *id = text_block(CLAUDE_CODE_SYSTEM_INSTRUCTION);
        if (cc) json_set(id, "cache_control", json_clone(cc));
        else {
            json_t *e = json_obj();
            json_set_str(e, "type", "ephemeral");
            json_set(id, "cache_control", e);
        }
        json_push(out, id);
        json_arr_foreach(prompts, i, it) { json_push(out, json_clone(it)); }
        json_free(prompts);
        return out;
    }
    if (json_len(prompts) == 0) {
        json_free(prompts);
        return NULL;
    }
    return prompts;
}

static void strip_private_keys(json_t *msgs) {
    json_t *p;
    json_arr_foreach(msgs, i, p) {
        json_del(p, CONV_KEY);
        json_del(p, DEV_KEY);
    }
}

/* wantsAnthropicFastMode, unless this endpoint+model rejected fast mode earlier in the session (dropFastMode) */
static bool wants_fast_mode_raw(const ai_model_t *m, const ai_stream_opts_t *o) {
    return cflag(m, "fastMode") || (cflag(m, "firstPartyProvider") && str_eq(o->service_tier, "priority"));
}

static bool wants_fast_mode(const ai_model_t *m, const ai_stream_opts_t *o) {
    return wants_fast_mode_raw(m, o) && !(o->fast_mode_disabled && o->fast_mode_disabled(o->fast_mode_ud, m));
}

static json_t *build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, bool oauth,
                             bool *thinking_on_out) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    anth_ro_t ro;
    resolve_opts(m, o, &ro);
    json_t *cc = resolve_cache_control(m, o, oauth);
    char *base = anthropic_request_url(m, false);

    json_t *system = build_system_blocks(m, ctx, oauth, cc);

    json_t *tools = NULL;
    if (json_is_arr(ctx->tools)) tools = convert_tools(m, ctx->tools, oauth, base);
    else if (oauth) tools = json_arr();

    json_t *metadata = NULL;
    char *uid = resolve_metadata_user_id(o->metadata, oauth, o->session_id);
    if (uid) {
        metadata = json_obj();
        json_set(metadata, "user_id", json_str_own(uid));
    }

    /* thinking + output_config effort */
    json_t *thinking = NULL;
    const char *oc_effort = NULL;
    const char *display = o->hide_thinking_summary ? "omitted" : "summarized";
    if (m->reasoning) {
        const char *disabled = cstr(m, "disabledThinking");
        /* "disabled" is treated like unset: see report (the bundled api-default injects it for every model) */
        bool explicit_off_mode = disabled && strcmp(disabled, "disabled") != 0;
        if (!ro.thinking_enabled && explicit_off_mode) {
            if (!strcmp(disabled, "between-tools")) {
                thinking = json_obj();
                json_set_str(thinking, "type", "between_tools");
                oc_effort = cstr(m, "betweenToolsEffort");
            } else if (!strcmp(disabled, "adaptive")) {
                thinking = json_obj();
                json_set_str(thinking, "type", "adaptive");
                if (thinking_supports_display(m) == 1) json_set_str(thinking, "display", display);
            }
        } else if (ro.thinking_enabled || cflag(m, "requiresThinkingEnabled")) {
            const char *effort = ro.effort;
            if (effort && uses_adaptive_tag_only(m)) effort = "adaptive";
            thinking = json_obj();
            if (!uses_budget_thinking(m)) {
                json_set_str(thinking, "type", "adaptive");
                if (thinking_supports_display(m) == 1) json_set_str(thinking, "display", display);
                if (effort && strcmp(effort, "adaptive") != 0) oc_effort = effort;
            } else {
                json_set_str(thinking, "type", "enabled");
                json_set_int(thinking, "budget_tokens", ro.budget > 0 ? ro.budget : 1024);
                if (thinking_supports_display(m) != 0) json_set_str(thinking, "display", display);
                if (str_eq(thinking_mode(m), "anthropic-budget-effort") && effort && strcmp(effort, "adaptive") != 0)
                    oc_effort = effort;
            }
        } else {
            if (cflag(m, "supportsBetweenToolsThinking")) {
                thinking = json_obj();
                json_set_str(thinking, "type", "between_tools");
                oc_effort = cstr(m, "betweenToolsEffort");
            } else if (is_adaptive_only(m)) {
                oc_effort = "low";
            } else {
                thinking = json_obj();
                json_set_str(thinking, "type", "disabled");
            }
        }
    }
    const char *ttype = thinking ? json_get_str(thinking, "type") : NULL;
    bool thinking_on = str_eq(ttype, "adaptive") || str_eq(ttype, "enabled");
    if (thinking_on_out) *thinking_on_out = thinking_on;

    bool compaction_ok = anthropic_compaction_supported(m);
    const json_t *comp_req = compaction_ok ? o->anthropic_compaction : NULL;
    json_t *context_mgmt = NULL;
    if (!comp_req && cflag(m, "supportsContextManagement") && !is_vertex_raw_predict(base) && thinking_on) {
        context_mgmt = json_obj();
        json_t *edits = json_arr(), *e = json_obj();
        json_set_str(e, "type", "clear_thinking_20251015");
        json_set_str(e, "keep", "all");
        json_push(edits, e);
        json_set(context_mgmt, "edits", edits);
    }

    json_t *transformed = ai_transform_messages(ctx->messages, m);
    ai_anthropic_prepare_images(transformed, m); /* Claude's image contract: 8000 px, 10 MB base64 */
    json_t *msgs = convert_messages(m, transformed, oauth, thinking_on, compaction_ok, comp_req != NULL);
    json_free(transformed);

    apply_head_caching(system, tools, cc);

    json_t *p = json_obj();
    json_set_str(p, "model", json_get_str(m->raw, "requestModelId") ? json_get_str(m->raw, "requestModelId") : m->id);
    json_set(p, "messages", msgs);
    if (system) json_set(p, "system", system);
    if (tools) json_set(p, "tools", tools);
    if (metadata) json_set(p, "metadata", metadata);
    int64_t limit = output_limit(m);
    json_set_int(p, "max_tokens", ro.max_tokens > 0 ? MIN(limit, ro.max_tokens) : limit);
    if (thinking) json_set(p, "thinking", thinking);
    if (context_mgmt) json_set(p, "context_management", context_mgmt);
    if (comp_req) {
        json_t *c = json_obj();
        json_set_str(c, "type", "summarize");
        const char *instr = json_get_str(comp_req, "instructions");
        if (instr && *instr) json_set_str(c, "instructions", instr);
        json_set(p, "compaction", c);
    }
    if (oc_effort && cflag(m, "supportsOutputEffort") && !is_vertex_raw_predict(base)) {
        json_t *oc = json_obj();
        json_set_str(oc, "effort", oc_effort);
        json_set(p, "output_config", oc);
    }
    json_set_bool(p, "stream", true);

    bool sampling = cflag(m, "supportsSamplingParams") && (!ttype || !strcmp(ttype, "disabled"));
    if (sampling && !isnan(o->temperature)) json_set_num(p, "temperature", o->temperature);
    if (sampling && !isnan(o->top_p)) json_set_num(p, "top_p", o->top_p);
    if (!comp_req && json_is_arr(o->stop_sequences) && json_len(o->stop_sequences) > 0) {
        json_t *ss = json_arr(), *it;
        json_arr_foreach(o->stop_sequences, i, it) {
            if (json_len(ss) >= STOP_SEQUENCES_MAX) break;
            json_push(ss, json_clone(it));
        }
        json_set(p, "stop_sequences", ss);
    }
    if (wants_fast_mode(m, o)) json_set_str(p, "speed", "fast");

    const char *tc_name = NULL;
    const char *tc = map_tool_choice(o->tool_choice, &tc_name);
    if (tc) {
        json_t *t = json_obj();
        if ((!strcmp(tc, "any") || !strcmp(tc, "tool")) && (comp_req || !cflag(m, "supportsForcedToolChoice"))) {
            json_set_str(t, "type", "auto");
        } else {
            json_set_str(t, "type", tc);
            if (tc_name) json_set(t, "name", json_str_own(encode_tool_name(m, tc_name, oauth)));
        }
        json_set(p, "tool_choice", t);
        /* disableThinkingIfToolChoiceForced */
        const char *ft = json_get_str(t, "type");
        if (str_eq(ft, "any") || str_eq(ft, "tool")) {
            json_del(p, "thinking");
            json_del(p, "context_management");
            if (is_adaptive_only(m) && cflag(m, "supportsOutputEffort")) {
                json_t *oc = json_get_obj(p, "output_config");
                if (!oc) {
                    oc = json_obj();
                    json_set(p, "output_config", oc);
                }
                json_set_str(oc, "effort", "low");
            } else {
                json_t *oc = json_get_obj(p, "output_config");
                if (oc) {
                    json_del(oc, "effort");
                    if (json_len(oc) == 0) json_del(p, "output_config");
                }
            }
        }
    }

    /* ensureMaxTokensForThinking */
    json_t *th = json_get_obj(p, "thinking");
    if (th && str_eq(json_get_str(th, "type"), "enabled")) {
        int64_t bt = json_get_int(th, "budget_tokens", 0);
        if (bt > 0) {
            int64_t cur = MIN(json_get_int(p, "max_tokens", limit), limit);
            int64_t raised = MIN(MAX(cur, bt + OUTPUT_FALLBACK_BUFFER), limit);
            int64_t nb = MIN(bt, raised - OUTPUT_FALLBACK_BUFFER);
            json_set_int(p, "max_tokens", raised);
            if (nb != bt) {
                if (nb <= 0) json_del(p, "thinking"); /* upstream raises a configuration error */
                else json_set_int(th, "budget_tokens", nb);
            }
        }
    }

    apply_prompt_caching(json_get(p, "messages"), json_get(p, "system"), json_get(p, "tools"), cc);
    strip_private_keys(json_get(p, "messages"));
    json_free(cc);
    free(base);
    return p;
}

json_t *anthropic_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, bool oauth) {
    return build_request(m, ctx, o, oauth, NULL);
}

/* replayResponse: the response as the next request's history serializes it, so a message thread can tell whether that
 * request extends this one. Owned, NULL when it serializes to nothing. */
static json_t *replay_response(const ai_model_t *m, const json_t *message, bool oauth, bool thinking_on) {
    json_t *one = json_arr();
    json_push(one, json_clone(message));
    json_t *transformed = ai_transform_messages(one, m);
    json_free(one);
    json_t *params = convert_messages(m, transformed, oauth, thinking_on, anthropic_compaction_supported(m), false);
    json_free(transformed);
    strip_private_keys(params);
    json_t *out = NULL, *it;
    json_arr_foreach(params, i, it) {
        if (str_eq(json_get_str(it, "role"), "assistant")) {
            out = json_clone(it);
            break;
        }
    }
    json_free(params);
    return out;
}

/* ======================================================================
 * URL / headers / body
 * ====================================================================== */

static char *resolve_base_url(const ai_model_t *m) {
    char *configured = normalize_base_url(m->base_url);
    if (cflag(m, "firstPartyProvider")) {
        if (configured && !is_official_url(configured)) return configured;
        char *env = normalize_base_url(getenv("ANTHROPIC_BASE_URL"));
        if (env) {
            free(configured);
            return env;
        }
    }
    return configured ? configured : xstrdup(DEFAULT_BASE_URL);
}

char *anthropic_request_url(const ai_model_t *m, bool oauth) {
    char *base = resolve_base_url(m);
    char *u = xasprintf("%s/v1/messages%s", base, oauth ? "?beta=true" : "");
    free(base);
    return u;
}

static void beta_add(strvec_t *v, const char *b) {
    if (!b || !*b || strvec_contains(v, b)) return;
    strvec_push(v, b);
}

static void claude_code_betas(strvec_t *v, bool agent, bool thinking, bool no_strict, bool cm) {
    if (agent) {
        static const char *const a[] = {BETA_CLAUDE_CODE,          BETA_OAUTH,        BETA_INTERLEAVED,
                                        BETA_THINKING_TOKEN_COUNT, BETA_CONTEXT_MGMT, BETA_PROMPT_CACHING_SCOPE,
                                        BETA_MID_CONV_SYSTEM};
        for (size_t i = 0; i < ARRAY_LEN(a); i++) {
            if (!cm && !strcmp(a[i], BETA_CONTEXT_MGMT)) continue;
            beta_add(v, a[i]);
        }
        if (thinking) beta_add(v, BETA_EFFORT);
        beta_add(v, BETA_FALLBACK_CREDIT);
        return;
    }
    static const char *const u[] = {BETA_OAUTH,        BETA_INTERLEAVED,          BETA_THINKING_TOKEN_COUNT,
                                    BETA_CONTEXT_MGMT, BETA_PROMPT_CACHING_SCOPE, BETA_STRUCTURED_OUTPUTS};
    for (size_t i = 0; i < ARRAY_LEN(u); i++) {
        if (no_strict && !strcmp(u[i], BETA_STRUCTURED_OUTPUTS)) continue;
        if (!cm && !strcmp(u[i], BETA_CONTEXT_MGMT)) continue;
        beta_add(v, u[i]);
    }
}

static const char *const ENFORCED_HEADERS[] = {
    "accept", "accept-encoding", "connection", "content-type", "anthropic-version",
    "anthropic-dangerous-direct-browser-access", "anthropic-beta", "user-agent", "x-app", "authorization",
    "x-api-key", "x-claude-code-session-id", "x-client-request-id", "cf-aig-authorization"};

static bool is_stainless(const char *k) {
    for (size_t i = 0; i < ARRAY_LEN(STAINLESS_KEYS); i++)
        if (strcasecmp(k, STAINLESS_KEYS[i]) == 0) return true;
    return false;
}

static bool is_enforced(const char *k) {
    for (size_t i = 0; i < ARRAY_LEN(ENFORCED_HEADERS); i++)
        if (strcasecmp(k, ENFORCED_HEADERS[i]) == 0) return true;
    return false;
}

void anthropic_build_headers(const ai_model_t *m, const ai_stream_opts_t *o, const ai_context_t *ctx,
                             const json_t *params, const char *api_key, bool oauth, http_headers_t *h) {
    char *base_url = resolve_base_url(m);
    bool official = is_official_url(base_url);
    anth_ro_t ro;
    resolve_opts(m, o, &ro);

    /* caller headers: model, then options */
    http_headers_t caller = {0};
    ai_apply_headers(&caller, m, o);
    const char *in_ua = http_headers_get(&caller, "User-Agent");
    const char *in_auth = http_headers_get(&caller, "Authorization");
    const char *in_key = http_headers_get(&caller, "X-Api-Key");

    /* betas */
    bool has_tools = json_len(ctx ? ctx->tools : NULL) > 0, buffered = false;
    json_t *t;
    json_arr_foreach(ctx ? ctx->tools : NULL, i, t) {
        if (str_eq(json_get_str(t, "name"), "yield")) buffered = true;
    }
    const char *tc_name;
    const char *tc = map_tool_choice(o->tool_choice, &tc_name);
    bool forced = tc && (!strcmp(tc, "any") || !strcmp(tc, "tool"));
    bool adaptive_pin = is_adaptive_only(m) && (!ro.thinking_enabled || (cflag(m, "supportsForcedToolChoice") && forced));
    json_t *eb = model_compat_get(m, "effortBeta");
    bool emits_effort = json_is_bool(eb) ? (eb->u.b && ro.effort != NULL)
                                         : (m->reasoning && cflag(m, "supportsOutputEffort") &&
                                            ((ro.thinking_enabled && !str_eq(ro.effort, "adaptive")) || adaptive_pin));
    strvec_t betas = {0};
    if (oauth)
        claude_code_betas(&betas, has_tools || ro.thinking_enabled, ro.thinking_enabled, cflag(m, "disableStrictTools"),
                          cflag(m, "supportsContextManagement"));
    if (wants_fast_mode(m, o)) beta_add(&betas, BETA_FAST_MODE);
    if (emits_effort) beta_add(&betas, BETA_EFFORT);
    json_t *cc = resolve_cache_control(m, o, oauth);
    json_t *eb2 = model_compat_get(m, "extendedCacheTtlBeta");
    if (!oauth && !is_vertex_raw_predict(base_url) && !(json_is_bool(eb2) && !eb2->u.b) && cc &&
        str_eq(json_get_str(cc, "ttl"), "1h"))
        beta_add(&betas, BETA_EXTENDED_CACHE_TTL);
    json_free(cc);
    if (!oauth && official) beta_add(&betas, BETA_FALLBACK_CREDIT);
    if (has_tools && !buffered && official && !eager_tool_streaming(m, base_url)) beta_add(&betas, BETA_FINE_GRAINED_TOOLS);
    bool signing_route = is_signing_proxy_url(base_url) || (cflag(m, "signingEndpoint") && !cflag(m, "officialEndpoint"));
    if (thinking_supports_display(m) != 1 ||
        (!official && signing_route && !is_vertex_raw_predict(base_url) && !url_has(base_url, "githubcopilot") &&
         !url_has(base_url, "copilot-api.")))
        beta_add(&betas, BETA_INTERLEAVED);
    /* per-request: context-management rides with the clear_thinking edit; effort with a pinned effort */
    json_t *cm = json_get_obj(params, "context_management");
    json_t *e;
    json_arr_foreach(cm ? json_get(cm, "edits") : NULL, i, e) {
        if (str_eq(json_get_str(e, "type"), "clear_thinking_20251015") && !is_vertex_raw_predict(base_url))
            beta_add(&betas, BETA_CONTEXT_MGMT);
    }
    json_t *oc = json_get_obj(params, "output_config");
    if (json_is_bool(eb) && eb->u.b && oc && json_get_str(oc, "effort")) beta_add(&betas, BETA_EFFORT);
    /* the compaction beta: an on-demand compaction request or a replayed signed compaction block */
    if (!is_vertex_raw_predict(base_url) && anthropic_compaction_supported(m)) {
        bool replays = json_get(params, "compaction") != NULL;
        const json_t *pm, *pb;
        json_arr_foreach(json_get(params, "messages"), i, pm) json_arr_foreach(json_get_arr(pm, "content"), k, pb) if (
            str_eq(json_get_str(pb, "type"), "compaction") && json_get_str(pb, "signature")) replays = true;
        if (replays) beta_add(&betas, BETA_COMPACTION);
    }
    char *beta_hdr = strvec_join(&betas, ",");
    strvec_free(&betas);

    if (str_eq(m->provider, "github-copilot")) {
        /* buildAnthropicClientOptions copilot branch: the proxy rejects Anthropic betas, so only caller headers
         * (model headers, dynamic Copilot identity, options) ride on the base set */
        http_headers_set(h, "Accept", "text/event-stream");
        http_headers_set(h, "Content-Type", "application/json");
        http_headers_set(h, "anthropic-version", "2023-06-01");
        http_headers_set(h, "Anthropic-Dangerous-Direct-Browser-Access", "true");
        char *bearer = xasprintf("Bearer %s", api_key ? api_key : "");
        http_headers_set(h, "Authorization", bearer);
        free(bearer);
        for (size_t i = 0; i < caller.n; i++) http_headers_set(h, caller.v[i].name, caller.v[i].value);
        if (!str_empty(o->session_id)) http_headers_set(h, "X-Claude-Code-Session-Id", o->session_id);
        free(beta_hdr);
        http_headers_free(&caller);
        free(base_url);
        return;
    }
    if (str_eq(m->provider, "cloudflare-ai-gateway") && !oauth) {
        /* buildAnthropicHeaders isCloudflare: the gateway authenticates via cf-aig-authorization; caller
         * Authorization / X-Api-Key are dropped so they cannot leak upstream */
        for (size_t i = 0; i < caller.n; i++)
            if (!is_enforced(caller.v[i].name)) http_headers_set(h, caller.v[i].name, caller.v[i].value);
        http_headers_set(h, "Accept", "text/event-stream");
        http_headers_set(h, "Connection", "keep-alive");
        http_headers_set(h, "Content-Type", "application/json");
        http_headers_set(h, "anthropic-dangerous-direct-browser-access", "true");
        http_headers_set(h, "x-app", "cli");
        http_headers_set(h, "anthropic-version", "2023-06-01");
        if (in_ua) http_headers_set(h, "User-Agent", in_ua);
        if (*beta_hdr) http_headers_set(h, "anthropic-beta", beta_hdr);
        char *cf = xasprintf("Bearer %s", api_key ? api_key : "");
        http_headers_set(h, "cf-aig-authorization", cf);
        free(cf);
        if (!str_empty(o->session_id)) http_headers_set(h, "X-Claude-Code-Session-Id", o->session_id);
        free(beta_hdr);
        http_headers_free(&caller);
        free(base_url);
        return;
    }
    /* non-enforced caller headers pass through (stainless keys too on non-OAuth requests) */
    for (size_t i = 0; i < caller.n; i++) {
        const char *k = caller.v[i].name;
        if (is_enforced(k) || (oauth && is_stainless(k))) continue;
        http_headers_set(h, k, caller.v[i].value);
    }
    if (oauth) {
        const char *sid = o->session_id;
        char *msid = NULL;
        if (!sid) sid = msid = metadata_session_id(metadata_str(o->metadata, "user_id"));
        http_headers_set(h, "Accept", "application/json");
        http_headers_set(h, "Content-Type", "application/json");
        char *ua = (in_ua && strncasecmp(in_ua, "claude-cli", 10) == 0)
                       ? xstrdup(in_ua)
                       : xasprintf("claude-cli/%s (external, cli)", anthropic_claude_code_version());
        http_headers_set(h, "User-Agent", ua);
        free(ua);
        if (sid && *sid) http_headers_set(h, "X-Claude-Code-Session-Id", sid);
        free(msid);
        for (size_t i = 0; i < ARRAY_LEN(STAINLESS_KEYS); i++) http_headers_set(h, STAINLESS_KEYS[i], stainless_value(i));
        if (*beta_hdr) http_headers_set(h, "anthropic-beta", beta_hdr);
        http_headers_set(h, "anthropic-dangerous-direct-browser-access", "true");
        http_headers_set(h, "anthropic-version", "2023-06-01");
        char *bearer = xasprintf("Bearer %s", api_key ? api_key : "");
        http_headers_set(h, "Authorization", bearer);
        free(bearer);
        http_headers_set(h, "x-app", "cli");
        http_headers_set(h, "Connection", "keep-alive");
        if (in_key) http_headers_set(h, "X-Api-Key", in_key);
    } else {
        bool x_api_key_provider = str_eq(m->provider, "opencode-go") || str_eq(m->provider, "opencode-zen") ||
                                  str_eq(m->provider, "umans");
        http_headers_set(h, "Accept", "text/event-stream");
        if (!official) {
            char *bearer = NULL;
            if (in_auth) bearer = xstrdup(in_auth);
            else if (api_key && *api_key && strcmp(api_key, "N/A") != 0) bearer = xasprintf("Bearer %s", api_key);
            if (bearer && !x_api_key_provider) http_headers_set(h, "Authorization", bearer);
            free(bearer);
        }
        http_headers_set(h, "Content-Type", "application/json");
        http_headers_set(h, "anthropic-dangerous-direct-browser-access", "true");
        http_headers_set(h, "x-app", "cli");
        http_headers_set(h, "Connection", "keep-alive");
        http_headers_set(h, "anthropic-version", "2023-06-01");
        if (in_ua) http_headers_set(h, "User-Agent", in_ua);
        if (*beta_hdr) http_headers_set(h, "anthropic-beta", beta_hdr);
        if (official) {
            if (in_auth) http_headers_set(h, "Authorization", in_auth);
            http_headers_set(h, "X-Api-Key", in_key ? in_key : (api_key ? api_key : ""));
        } else if (in_key) {
            http_headers_set(h, "X-Api-Key", in_key);
        } else if (x_api_key_provider || (!in_auth && api_key && *api_key && strcmp(api_key, "N/A") != 0 &&
                                          !http_headers_get(h, "Authorization"))) {
            http_headers_set(h, "X-Api-Key", api_key ? api_key : "");
        }
    }
    free(beta_hdr);
    http_headers_free(&caller);
    free(base_url);
}

static const char BILLING_MARKER[] = "\"system\":[{\"type\":\"text\",\"text\":\"" BILLING_PREFIX;

char *anthropic_serialize_body(const json_t *params) {
    char *body = json_dump(params, 0);
    char *mk = strstr(body, BILLING_MARKER);
    if (!mk) return body;
    char *from = mk + strlen(BILLING_MARKER);
    char *ph = strstr(from, CCH_PLACEHOLDER);
    if (!ph || ph - from > 150) {
        LOGW("anthropic: cch billing placeholder present but not patched; sending unattested request");
        return body;
    }
    uint64_t hv = hash_xx64(body, strlen(body), CCH_SEED);
    char cch[8];
    snprintf(cch, sizeof(cch), "%05llx", (unsigned long long)(hv & 0xfffffULL));
    memcpy(ph + 4, cch, 5);
    return body;
}

/* ======================================================================
 * SSE state machine
 * ====================================================================== */

const char *anthropic_map_stop_reason(const char *r) {
    if (!r) return "stop";
    if (!strcmp(r, "end_turn") || !strcmp(r, "stop_sequence") || !strcmp(r, "pause_turn") || !strcmp(r, "compaction"))
        return "stop";
    if (!strcmp(r, "max_tokens") || !strcmp(r, "model_context_window_exceeded")) return "length";
    if (!strcmp(r, "tool_use")) return "toolUse";
    if (!strcmp(r, "refusal") || !strcmp(r, "sensitive")) return "error";
    LOGD("anthropic: unhandled stop reason: %s", r);
    return "stop";
}

void anthropic_sse_init(anthropic_sse_t *s, const ai_model_t *m, bool oauth, ai_event_cb cb, void *ud) {
    memset(s, 0, sizeof(*s));
    aib_init(&s->b, m, cb, ud);
    s->model = m;
    s->oauth = oauth;
    s->stop_reason = "stop";
}

void anthropic_sse_free(anthropic_sse_t *s) {
    json_free(s->compaction);
    s->compaction = NULL;
    for (size_t i = 0; i < s->blocks.n; i++) sb_free(&s->blocks.v[i].sig);
    vec_free(&s->blocks);
    free(s->error);
    s->error = NULL;
    aib_free(&s->b);
}

static anthropic_block_t *find_block(anthropic_sse_t *s, int64_t idx) {
    for (size_t i = s->blocks.n; i > 0; i--)
        if (s->blocks.v[i - 1].idx == idx && s->blocks.v[i - 1].open) return &s->blocks.v[i - 1];
    return NULL;
}

static json_t *content_arr(anthropic_sse_t *s) { return json_get(s->b.msg, "content"); }

static void apply_usage(anthropic_sse_t *s, const json_t *u, bool all) {
    if (!json_is_obj(u)) return;
#define UF(k) (json_is_num(json_get(u, k)) ? json_get_int(u, k, 0) : (all ? 0 : -1))
    aib_usage(&s->b, UF("input_tokens"), UF("output_tokens"), UF("cache_read_input_tokens"), UF("cache_creation_input_tokens"));
#undef UF
    json_t *usage = json_get(s->b.msg, "usage");
    json_t *cc = json_get_obj(u, "cache_creation");
    if (cc) {
        int64_t f = json_get_int(cc, "ephemeral_5m_input_tokens", 0), h = json_get_int(cc, "ephemeral_1h_input_tokens", 0);
        if (f > 0 || h > 0) {
            json_t *c = json_obj();
            if (f > 0) json_set_int(c, "ephemeral5m", f);
            if (h > 0) json_set_int(c, "ephemeral1h", h);
            json_set(usage, "cttl", c);
        } else {
            json_del(usage, "cttl");
        }
    }
    json_t *st = json_get_obj(u, "server_tool_use");
    if (st) {
        int64_t ws = json_get_int(st, "web_search_requests", 0), wf = json_get_int(st, "web_fetch_requests", 0);
        if (ws > 0 || wf > 0) {
            json_t *c = json_obj();
            if (ws > 0) json_set_int(c, "webSearch", ws);
            if (wf > 0) json_set_int(c, "webFetch", wf);
            json_set(usage, "server", c);
        } else {
            json_del(usage, "server");
        }
    }
}

static void set_error(anthropic_sse_t *s, char *msg) {
    if (s->error) {
        free(msg);
        return;
    }
    s->error = msg;
}

static void on_block_start(anthropic_sse_t *s, const json_t *ev) {
    if (s->saw_terminal) return;
    int64_t idx = json_get_int(ev, "index", -1);
    json_t *cb = json_get_obj(ev, "content_block");
    if (find_block(s, idx) || s->blocks.n >= MAX_STREAM_BLOCKS) return;
    anthropic_block_t blk = {.idx = idx, .kind = 0, .ci = -1, .open = true, .sig = SB_INIT};
    const char *ty = cb ? json_get_str(cb, "type") : NULL;
    if (str_eq(ty, "text")) {
        blk.kind = 1;
        const char *t = json_get_str(cb, "text");
        if (t && *t) {
            aib_text_delta(&s->b, t, strlen(t));
            blk.ci = s->b.cur_index;
        }
    } else if (str_eq(ty, "thinking")) {
        blk.kind = 2;
        const char *t = json_get_str(cb, "thinking");
        if (t && *t) {
            aib_thinking_delta(&s->b, t, strlen(t));
            blk.ci = s->b.cur_index;
        }
        const char *sig = json_get_str(cb, "signature");
        if (sig) sb_append(&blk.sig, sig);
    } else if (str_eq(ty, "redacted_thinking")) {
        blk.kind = 3;
        aib_redacted_thinking(&s->b, json_get_str(cb, "data"));
        blk.ci = aib_block_count(&s->b) - 1;
    } else if (str_eq(ty, "compaction")) {
        /* complete at content_block_start; held until the stop reason confirms compaction */
        const char *content = json_get_str(cb, "content");
        if (content && *content) {
            json_free(s->compaction);
            s->compaction = json_obj();
            json_set_str(s->compaction, "type", "anthropicCompaction");
            json_set_str(s->compaction, "provider", s->model->provider);
            json_set_str(s->compaction, "content", content);
            if (json_get_str(cb, "signature")) json_set_str(s->compaction, "signature", json_get_str(cb, "signature"));
            if (json_get_str(cb, "encrypted_content")) json_set_str(s->compaction, "encryptedContent", json_get_str(cb, "encrypted_content"));
            if (json_is_arr(json_get(cb, "tool_changes"))) json_set(s->compaction, "toolChanges", json_clone(json_get(cb, "tool_changes")));
        }
    } else if (str_eq(ty, "tool_use")) {
        blk.kind = 4;
        aib_toolcall_start(&s->b, json_get_str(cb, "id"), decode_tool_name(s->model, json_get_str(cb, "name"), s->oauth));
        blk.ci = s->b.cur_index;
        json_t *in = json_get_obj(cb, "input");
        if (in && json_len(in) > 0) aib_toolcall_set_args(&s->b, json_clone(in));
    }
    vec_push(&s->blocks, blk);
}

static void on_block_delta(anthropic_sse_t *s, const json_t *ev) {
    if (s->saw_terminal) return;
    anthropic_block_t *blk = find_block(s, json_get_int(ev, "index", -1));
    json_t *d = json_get_obj(ev, "delta");
    if (!blk || !blk->kind || !d) return;
    const char *dt = json_get_str(d, "type");
    /* deltas only apply to the block currently open in the builder (Anthropic streams blocks sequentially) */
    bool current = blk->ci < 0 || blk->ci == s->b.cur_index;
    if (str_eq(dt, "text_delta") && blk->kind == 1 && current) {
        json_t *t = json_get(d, "text");
        if (json_is_str(t) && t->u.str.len) {
            aib_text_delta(&s->b, t->u.str.s, t->u.str.len);
            blk->ci = s->b.cur_index;
        }
    } else if (str_eq(dt, "thinking_delta") && blk->kind == 2 && current) {
        json_t *t = json_get(d, "thinking");
        if (json_is_str(t) && t->u.str.len) {
            aib_thinking_delta(&s->b, t->u.str.s, t->u.str.len);
            blk->ci = s->b.cur_index;
        }
    } else if (str_eq(dt, "signature_delta") && blk->kind == 2) {
        const char *sig = json_get_str(d, "signature");
        if (sig) sb_append(&blk->sig, sig);
    } else if (str_eq(dt, "input_json_delta") && blk->kind == 4 && blk->ci == s->b.cur_index && s->b.cur_kind == 3) {
        json_t *pj = json_get(d, "partial_json");
        if (json_is_str(pj) && pj->u.str.len) aib_toolcall_delta(&s->b, pj->u.str.s, pj->u.str.len);
    }
}

static void on_block_stop(anthropic_sse_t *s, const json_t *ev) {
    if (s->saw_terminal) return;
    anthropic_block_t *blk = find_block(s, json_get_int(ev, "index", -1));
    if (!blk) return;
    blk->open = false;
    if (blk->kind == 2) {
        if (blk->ci < 0 && blk->sig.len) {
            /* signature-only thinking (display "omitted"): materialize an empty signed block */
            aib_start(&s->b);
            aib_end_block(&s->b);
            json_t *tb = json_obj();
            json_set_str(tb, "type", "thinking");
            json_set_str(tb, "thinking", "");
            json_set(tb, "thinkingSignature", json_strn(blk->sig.s, blk->sig.len));
            json_push(content_arr(s), tb);
            blk->ci = aib_block_count(&s->b) - 1;
            return;
        }
        if (blk->ci >= 0 && blk->sig.len)
            json_set(json_at(content_arr(s), (size_t)blk->ci), "thinkingSignature", json_strn(blk->sig.s, blk->sig.len));
    }
    if (blk->ci >= 0 && blk->ci == s->b.cur_index) aib_end_block(&s->b);
}

static void on_message_delta(anthropic_sse_t *s, const json_t *ev) {
    if (s->saw_terminal) return;
    json_t *d = json_get_obj(ev, "delta");
    const char *raw = d ? json_get_str(d, "stop_reason") : NULL;
    if (raw) {
        s->stop_reason = anthropic_map_stop_reason(raw);
        s->saw_terminal = true;
        if (!strcmp(raw, "compaction")) s->compaction_stop = true;
    }
    if (str_eq(s->stop_reason, "error") && !s->error) {
        json_t *sd = d ? json_get_obj(d, "stop_details") : NULL;
        if (sd && str_eq(json_get_str(sd, "type"), "refusal")) {
            const char *cat = json_get_str(sd, "category");
            char *label = cat ? xasprintf("Refusal (%s)", cat) : xstrdup("Refusal");
            char *expl = json_get_str(sd, "explanation") ? str_trim(json_get_str(sd, "explanation")) : NULL;
            set_error(s, expl && *expl ? xasprintf("%s: %s", label, expl) : xstrdup(label));
            free(expl);
            free(label);
        } else if (str_eq(raw, "refusal")) {
            set_error(s, xstrdup("Refusal (no details provided)"));
        } else if (str_eq(raw, "sensitive")) {
            set_error(s, xstrdup("Content flagged by safety filters"));
        } else {
            set_error(s, xasprintf("Anthropic stream ended with stop_reason: %s", raw ? raw : "unknown"));
        }
    }
    apply_usage(s, json_get(ev, "usage"), false);
}

static char *stream_error_message(const char *data, size_t len) {
    json_t *j = json_parse(data, len, NULL);
    json_t *e = json_get_obj(j, "error");
    const char *type = e ? json_get_str(e, "type") : NULL;
    const char *msg = e ? json_get_str(e, "message") : NULL;
    char *out;
    if (msg) out = type ? xasprintf("Anthropic stream error (%s): %s", type, msg) : xasprintf("Anthropic stream error: %s", msg);
    else out = len > 600 ? xasprintf("%.600s…", data) : xstrndup(data, len);
    json_free(j);
    return out;
}

int anthropic_sse_event(void *ud, const char *event, const char *data, size_t len) {
    anthropic_sse_t *s = ud;
    if (str_eq(event, "error")) {
        set_error(s, stream_error_message(data, len));
        return 2;
    }
    if (str_eq(event, "ping")) return 0;
    json_t *ev = json_parse(data, len, NULL);
    if (!json_is_obj(ev)) {
        json_free(ev);
        return 0;
    }
    const char *type = json_get_str(ev, "type");
    if (!type) type = event;
    int rc = 0;
    if (str_eq(type, "error")) {
        json_free(ev);
        set_error(s, stream_error_message(data, len));
        return 2;
    }
    if (str_eq(type, "message_start")) {
        if (!s->saw_start) {
            s->saw_start = true;
            aib_start(&s->b);
            json_t *msg = json_get_obj(ev, "message");
            const char *id = msg ? json_get_str(msg, "id") : NULL;
            if (id) aib_set(&s->b, "responseId", json_str(id));
            if (msg) apply_usage(s, json_get(msg, "usage"), true);
        }
    } else if (!s->saw_start) {
        /* events before message_start are ignored */
    } else if (str_eq(type, "content_block_start")) {
        on_block_start(s, ev);
    } else if (str_eq(type, "content_block_delta")) {
        on_block_delta(s, ev);
    } else if (str_eq(type, "content_block_stop")) {
        on_block_stop(s, ev);
    } else if (str_eq(type, "message_delta")) {
        on_message_delta(s, ev);
    } else if (str_eq(type, "message_stop")) {
        s->saw_terminal = true;
        s->saw_stop = true;
        rc = 1;
    }
    json_free(ev);
    return rc;
}

json_t *anthropic_sse_finish(anthropic_sse_t *s, int rc, const char *err) {
    json_t *msg;
    /* close dangling blocks so signatures land */
    for (size_t i = 0; i < s->blocks.n; i++) {
        anthropic_block_t *b = &s->blocks.v[i];
        if (b->open && b->kind == 2 && b->ci >= 0 && b->sig.len)
            json_set(json_at(content_arr(s), (size_t)b->ci), "thinkingSignature", json_strn(b->sig.s, b->sig.len));
    }
    if (rc == -2) msg = aib_finish(&s->b, "aborted", err ? err : "Request was aborted");
    else if (s->error) msg = aib_finish(&s->b, "error", s->error);
    else if (rc < 0 || (rc > 2)) msg = aib_finish(&s->b, "error", err ? err : "request failed");
    else if (!s->saw_start) msg = aib_finish(&s->b, "error", "Anthropic stream ended before message_start");
    else if (!s->saw_terminal) msg = aib_finish(&s->b, "error", "Anthropic stream ended before message_stop");
    else msg = aib_finish(&s->b, s->stop_reason, NULL);
    if (s->compaction_stop && str_eq(json_get_str(msg, "stopReason"), "stop")) {
        /* on-demand compaction ends with no ordinary assistant content */
        json_t *sd = json_obj();
        json_set_str(sd, "type", "compaction");
        json_set(msg, "stopDetails", sd);
        if (s->compaction) json_set(msg, "providerPayload", json_clone(s->compaction));
    }
    return msg;
}

/* ======================================================================
 * entry point
 * ====================================================================== */

json_t *provider_anthropic_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                                  ai_event_cb cb, void *ud) {
    return provider_anthropic_stream_ex(m, ctx, o, -1, cb, ud);
}

/* AIError.classify for dropAfterUnexplained400; the provider-only test binaries link without the classifier */
extern uint32_t aie_classify_text(const char *err, int status, const char *api, const char *provider) __attribute__((weak));

/* ---------- per-request failure hooks: fast-mode fallback and subscription slow mode ---------- */

typedef struct {
    const ai_model_t *m;
    const ai_stream_opts_t *o;
    char *lane;              /* slow-mode account lane, NULL when slow mode does not apply */
    bool sent_slow;          /* the attempt carried `anthropic-usage-limit: slow` */
    bool fast_wanted;        /* the attempt carried `speed: "fast"` */
    bool fast_unsupported;   /* the endpoint rejected fast mode */
    bool have_sig;
    anthropic_slow_signal_t sig; /* slow-mode headers of the latest failed attempt */
    int64_t wait_since;      /* first capacity wait (ms), -1 none */
    int attempts;            /* capacity waits taken */
} anth_hook_t;

static bool fast_dropped(void *ud, const ai_model_t *m) {
    UNUSED(ud), UNUSED(m);
    return true;
}

static bool has_word_ci(const char *s, const char *w) {
    size_t n = strlen(w);
    for (const char *p = s; p && *p; p++) {
        if (strncasecmp(p, w, n)) continue;
        bool left = p == s || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        bool right = !(isalnum((unsigned char)p[n]) || p[n] == '_');
        if (left && right) return true;
    }
    return false;
}

/* error/flags.ts matchesFastModeUnsupported */
static bool fast_mode_unsupported(int status, const char *body) {
    if (!body) return false;
    if (status == 400)
        return str_istr(body, "invalid_request_error") && has_word_ci(body, "speed") && str_istr(body, "not support");
    return status == 429 && str_istr(body, "rate_limit_error") && str_istr(body, "fast mode");
}

static void anth_on_headers(void *ud, int status, const http_headers_t *h) {
    anth_hook_t *k = ud;
    if (!k->lane) return;
    anthropic_slow_signal_t sig;
    bool any = anthropic_slow_parse(h, &sig);
    if (status >= 200 && status < 300) {
        k->have_sig = false;
        if (any) anthropic_slow_observe(k->o->slow_hooks, k->lane, &sig);
    } else {
        k->have_sig = any;
        k->sig = sig;
    }
}

static int64_t anth_on_failure(void *ud, int status, const char *body, http_headers_t *headers) {
    anth_hook_t *k = ud;
    if (k->fast_wanted && fast_mode_unsupported(status, body)) {
        k->fast_unsupported = true;
        return -2;
    }
    if (!k->lane || (k->o->cancel && *k->o->cancel)) return -1;
    bool overloaded = status == 529 || (body && strstr(body, "overloaded_error"));
    bool capacity = false;
    int64_t waited = k->wait_since < 0 ? 0 : mono_ms() - k->wait_since;
    int64_t d = anthropic_slow_on_failure(k->o->slow_hooks, k->lane, status, overloaded, k->have_sig ? &k->sig : NULL, k->sent_slow,
                                          waited, k->attempts, &capacity);
    if (d < 0) return -1;
    if (capacity) {
        if (k->wait_since < 0) k->wait_since = mono_ms();
        k->attempts++;
    }
    k->sent_slow = anthropic_slow_is_active(k->o->slow_hooks, k->lane);
    if (k->sent_slow) http_headers_set(headers, "anthropic-usage-limit", "slow");
    else http_headers_del(headers, "anthropic-usage-limit");
    return d;
}

static json_t *anthropic_stream_once(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, int is_oauth,
                                     ai_event_cb cb, void *ud) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    char *env_key = NULL;
    const char *key = o->api_key;
    if (!key || !*key) key = env_key = ai_env_api_key(m->provider);
    if (!key || !*key) {
        char *e = xasprintf("No API key for provider %s", m->provider ? m->provider : "anthropic");
        ai_builder_t b;
        aib_init(&b, m, cb, ud);
        json_t *msg = aib_finish(&b, "error", e);
        aib_free(&b);
        free(e);
        free(env_key);
        return msg;
    }
    bool oauth = is_oauth >= 0 ? is_oauth != 0 : anthropic_model_is_oauth(m, key);
    json_t *result = NULL;
    bool thinking_on = false;
    ai_stream_opts_t lo = *o; /* lo.fast_mode_disabled is pinned once this request drops fast mode */
    /* the provider loop owns transport retries (PROVIDER_MAX_RETRIES, calculateAnthropicRetryDelayMs) */
    lo.retry_policy = AI_RETRY_ANTHROPIC;
    lo.max_retries = 10;
    /* subscription slow mode: first-party OAuth requests to the official endpoint only */
    char *base_url = resolve_base_url(m);
    anth_hook_t hk = {.m = m, .o = &lo, .wait_since = -1};
    if (o->slow_hooks && cflag(m, "firstPartyProvider") && oauth && is_official_url(base_url))
        hk.lane = anthropic_slow_lane_key(key);
    /* message threads: only against a session with provider state. Per-call context messages are rebuilt next request,
     * so a thread could never continue past them; those callers leave provider_session_state off. */
    anth_thread_turn_t *thread = lo.provider_session_state && !str_empty(lo.session_id) && anth_threads_wanted(m, &lo, base_url)
                                     ? anth_thread_turn_new(base_url, m->id, lo.session_id, oauth)
                                     : NULL;
    free(base_url);
    bool version_retried = false;
    /* bounded: each retry below is taken at most once, a refused continue at most twice */
    for (int guard = 0; guard < 8; guard++) {
        json_t *params = build_request(m, ctx, &lo, oauth, &thinking_on);
        if (o->on_payload) o->on_payload(o->payload_ud, params);
        http_headers_t h = {0};
        anthropic_build_headers(m, &lo, ctx, params, key, oauth, &h);
        hk.fast_wanted = json_get_str(params, "speed") != NULL;
        hk.fast_unsupported = false;
        hk.sent_slow = hk.lane && anthropic_slow_is_active(o->slow_hooks, hk.lane);
        if (hk.sent_slow) http_headers_set(&h, "anthropic-usage-limit", "slow");
        char *url = anthropic_request_url(m, oauth);
        /* `params` stays the full body; only the wire sees the thread shape, with its beta */
        json_t *wire = thread ? anth_thread_plan(thread, params, http_headers_get(&h, "anthropic-beta"), now_ms(), NULL) : NULL;
        if (wire) {
            const char *sent = http_headers_get(&h, "anthropic-beta");
            char *merged = !str_empty(sent) ? xasprintf("%s,%s", sent, ANTHROPIC_THREADS_BETA) : xstrdup(ANTHROPIC_THREADS_BETA);
            http_headers_set(&h, "anthropic-beta", merged);
            free(merged);
        }
        char *body = anthropic_serialize_body(wire ? wire : params);
        json_free(wire);
        json_free(params);
        anthropic_sse_t s;
        anthropic_sse_init(&s, m, oauth, cb, ud);
        int status = 0;
        char *err = NULL;
        ai_post_ex_t ex = {.on_headers = anth_on_headers, .on_failure = anth_on_failure, .hook_ud = &hk};
        int rc = ai_sse_post_ex(url, &h, body, &lo, anthropic_sse_event, &s, &status, &err, &ex);
        free(url);
        free(body);
        http_headers_free(&h);
        /* a thread refusal goes first; an undocumented 400 falls back to an unthreaded resend after the other rungs */
        bool retry = false;
        if (thread && rc == -1 && !s.saw_start) {
            anth_thread_recovery_t rec = anth_thread_recover(thread, status, ex.err_body);
            if (rec != ANTH_THREAD_NO_RECOVERY) {
                LOGW("%s", rec == ANTH_THREAD_REPLAY ? "anthropic: thread continue refused, replaying the full conversation"
                                                     : "anthropic: thread refused, resending without thread");
                retry = true;
            }
        }
        if (!retry && oauth && !version_retried && rc == -1 && !s.saw_start && adopt_required_version(err)) {
            retry = true;
            version_retried = true;
        }
        if (!retry && rc == -1 && !s.saw_start && hk.fast_unsupported && lo.fast_mode_disabled != fast_dropped) {
            /* fast mode unsupported: drop `speed` for this endpoint+model and resend once */
            if (o->on_fast_mode_unsupported) o->on_fast_mode_unsupported(o->fast_mode_ud, m);
            lo.fast_mode_disabled = fast_dropped;
            retry = true;
        }
        if (!retry && thread && rc == -1 && !s.saw_start &&
            anth_thread_drop_after_400(thread, status, aie_classify_text && (aie_classify_text(err, status, m->api, m->provider) & AIE_CONTEXT_OVERFLOW))) {
            LOGW("anthropic: 400 on a thread request, resending without thread");
            retry = true;
        }
        free(ex.err_body);
        if (retry) {
            free(err);
            anthropic_sse_free(&s);
            continue;
        }
        result = anthropic_sse_finish(&s, rc, err);
        free(err);
        anthropic_sse_free(&s);
        break;
    }
    if (thread && result) {
        /* the server keeps a finished turn from the requested model; a paused or truncated turn, or one a fallback model
         * wrote, starts a new thread next. A failed turn settles nothing. */
        const char *stop = json_get_str(result, "stopReason");
        if (!str_eq(stop, "error") && !str_eq(stop, "aborted")) {
            bool continuable = (str_eq(stop, "stop") || str_eq(stop, "toolUse")) &&
                               !str_eq(json_get_str(json_get_obj(result, "stopDetails"), "type"), "pause_turn") &&
                               str_eq(json_get_str(result, "model"), m->id);
            const char *id = json_get_str(result, "responseId");
            json_t *replay = continuable && id ? replay_response(m, result, oauth, thinking_on) : NULL;
            anth_thread_settle(thread, id, replay, now_ms());
            json_free(replay);
        }
    }
    anth_thread_turn_free(thread);
    free(hk.lane);
    free(env_key);
    return result;
}

typedef struct {
    const ai_model_t *m;
    const ai_context_t *ctx;
    const ai_stream_opts_t *o;
    int is_oauth;
} anthropic_attempt_t;

static json_t *anthropic_attempt(void *self, ai_event_cb cb, void *ud) {
    anthropic_attempt_t *at = self;
    return anthropic_stream_once(at->m, at->ctx, at->o, at->is_oauth, cb, ud);
}

/* streamAnthropic: benign empty completions are retried (twice) before they reach the agent loop */
json_t *provider_anthropic_stream_ex(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, int is_oauth,
                                     ai_event_cb cb, void *ud) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    anthropic_attempt_t at = {m, ctx, o, is_oauth};
    return ai_replay_safe_stream(anthropic_attempt, &at, o, cb, ud, true, 0);
}

/* ---------- hosted web search (coding-agent web/search/providers/anthropic.ts callSearch) ---------- */

char *anthropic_search_request(const ai_model_t *m, const char *key, const char *query, const char *system_prompt,
                               int64_t max_tokens, double temperature, const char *session_id, http_headers_t *h,
                               char **url_out) {
    bool oauth = anthropic_is_oauth_token(key);
    /* buildAnthropicAuthConfig: the model's base URL, else ANTHROPIC_BASE_URL, else the default */
    char *base = normalize_base_url(m->base_url);
    if (!base) base = normalize_base_url(getenv("ANTHROPIC_BASE_URL"));
    if (!base) base = xstrdup(DEFAULT_BASE_URL);
    bool official = is_official_url(base);
    *url_out = xasprintf("%s/v1/messages?beta=true", base);

    /* buildAnthropicSearchHeaders -> buildAnthropicHeaders({extraBetas: [web-search], stream: false}) */
    strvec_t betas = {0};
    if (oauth) claude_code_betas(&betas, true, true, false, true);
    beta_add(&betas, "web-search-2025-03-05");
    char *beta_hdr = strvec_join(&betas, ",");
    strvec_free(&betas);
    http_headers_t caller = {0};
    ai_apply_headers(&caller, m, NULL);
    const char *in_ua = http_headers_get(&caller, "User-Agent");
    const char *in_auth = http_headers_get(&caller, "Authorization");
    const char *in_key = http_headers_get(&caller, "X-Api-Key");
    for (size_t i = 0; i < caller.n; i++) {
        const char *k = caller.v[i].name;
        if (is_enforced(k) || (oauth && is_stainless(k))) continue;
        http_headers_set(h, k, caller.v[i].value);
    }
    http_headers_set(h, "Accept", "application/json");
    if (oauth) {
        http_headers_set(h, "Content-Type", "application/json");
        char *ua = (in_ua && strncasecmp(in_ua, "claude-cli", 10) == 0)
                       ? xstrdup(in_ua)
                       : xasprintf("claude-cli/%s (external, cli)", anthropic_claude_code_version());
        http_headers_set(h, "User-Agent", ua);
        free(ua);
        for (size_t i = 0; i < ARRAY_LEN(STAINLESS_KEYS); i++) http_headers_set(h, STAINLESS_KEYS[i], stainless_value(i));
        http_headers_set(h, "anthropic-beta", beta_hdr);
        http_headers_set(h, "anthropic-dangerous-direct-browser-access", "true");
        http_headers_set(h, "anthropic-version", "2023-06-01");
        char *bearer = xasprintf("Bearer %s", key ? key : "");
        http_headers_set(h, "Authorization", bearer);
        free(bearer);
        http_headers_set(h, "x-app", "cli");
        http_headers_set(h, "Connection", "keep-alive");
        if (in_key) http_headers_set(h, "X-Api-Key", in_key);
    } else {
        if (!official) {
            char *bearer = in_auth ? xstrdup(in_auth)
                                   : (key && strcmp(key, AI_NO_AUTH_SENTINEL) != 0 ? xasprintf("Bearer %s", key) : NULL);
            if (bearer) http_headers_set(h, "Authorization", bearer);
            free(bearer);
        }
        http_headers_set(h, "Connection", "keep-alive");
        http_headers_set(h, "Content-Type", "application/json");
        http_headers_set(h, "anthropic-dangerous-direct-browser-access", "true");
        http_headers_set(h, "x-app", "cli");
        http_headers_set(h, "anthropic-version", "2023-06-01");
        if (in_ua) http_headers_set(h, "User-Agent", in_ua);
        http_headers_set(h, "anthropic-beta", beta_hdr);
        if (official) {
            if (in_auth) http_headers_set(h, "Authorization", in_auth);
            http_headers_set(h, "X-Api-Key", in_key ? in_key : (key ? key : ""));
        } else if (in_key) {
            http_headers_set(h, "X-Api-Key", in_key);
        }
    }
    free(beta_hdr);
    http_headers_free(&caller);
    free(base);

    json_t *body = json_obj();
    json_set_str(body, "model", m->id);
    json_set_int(body, "max_tokens", max_tokens > 0 ? max_tokens : 4096);
    json_t *msgs = json_arr(), *um = json_obj();
    json_set_str(um, "role", "user");
    json_set_str(um, "content", query);
    json_push(msgs, um);
    json_set(body, "messages", msgs);
    json_t *tools = json_arr(), *tool = json_obj();
    json_set_str(tool, "type", "web_search_20250305");
    json_set_str(tool, "name", "web_search");
    json_push(tools, tool);
    json_set(body, "tools", tools);
    /* resolveAnthropicMetadataUserId(sessionId, isOAuth, sessionId): API keys forward the session id verbatim */
    json_t *md = json_obj();
    if (session_id && *session_id) json_set_str(md, "user_id", session_id);
    char *uid = resolve_metadata_user_id(md, oauth, session_id);
    json_free(md);
    if (uid) {
        json_t *meta = json_obj();
        json_set_str(meta, "user_id", uid);
        json_set(body, "metadata", meta);
        free(uid);
    }
    if (!isnan(temperature) && cflag(m, "supportsSamplingParams")) json_set_num(body, "temperature", temperature);
    /* buildSystemBlocks: the Claude Code fingerprint and the extra instruction only on OAuth requests */
    json_t *sys = json_arr();
    if (oauth && cflag(m, "injectClaudeCodeInstruction")) {
        char *billing = billing_header_text("");
        json_push(sys, text_block(billing));
        free(billing);
        json_t *id = text_block(CLAUDE_CODE_SYSTEM_INSTRUCTION);
        json_t *e = json_obj();
        json_set_str(e, "type", "ephemeral");
        json_set(id, "cache_control", e);
        json_push(sys, id);
    }
    if (oauth) json_push(sys, text_block("You are a helpful AI assistant with web search capabilities."));
    if (system_prompt && !str_blank(system_prompt)) json_push(sys, text_block(system_prompt));
    if (json_len(sys)) json_set(body, "system", sys);
    else json_free(sys);
    char *s = anthropic_serialize_body(body);
    json_free(body);
    return s;
}
