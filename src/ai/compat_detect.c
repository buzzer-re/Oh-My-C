/* compat_detect.c - runtime model policy for custom and discovered models (see compat_detect.h). */
#define _GNU_SOURCE
#include "ai/compat_detect.h"

#include "core/core.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* ---------- identity (compat/taxonomy.ts classifyModel, lenient) ---------- */

typedef struct {
    char kind; /* 'e' exact 4, 'b' bounded 3, 'n' namespace 2, 'N' bounded namespace 2, 'p' prefix 1, 'g' glob 0 */
    const char *token;
} matcher_t;
typedef struct {
    const char *id, *glob;
    int prio;
} family_t;
typedef struct {
    const char *prefix;
    bool anywhere;
} revpfx_t;
typedef struct {
    const char *id;
    matcher_t m[16];
    family_t f[16];
    revpfx_t r[4];
    const char *skip[4];
} class_t;

/* rules.json taxonomy.classes (omp v18.14.2) */
static const class_t CLASSES[] = {
    {"ai21", {{'b', "jamba"}, {'n', "ai21"}}, {{"jamba", "*jamba*", 0}}, {{0}}, {0}},
    {"amazon", {{'b', "nova"}, {'b', "titan"}, {'N', "amazon"}}, {{"nova", "*nova*", 0}, {"titan", "*titan*", 0}}, {{0}}, {0}},
    {"anthropic",
     {{'N', "anthropic"}, {'b', "anthropic"}, {'b', "claude"}},
     {{"opus", "*opus*", 0}, {"sonnet", "*sonnet*", 0}, {"haiku", "*haiku*", 0}, {"fable", "*fable*", 0}, {"mythos", "*mythos*", 0}},
     {{"claude-", true}},
     {0}},
    {"baidu", {{'b', "ernie"}}, {{"ernie", "*ernie*", 0}}, {{0}}, {0}},
    {"bytedance", {{'b', "doubao"}}, {{"doubao", "*doubao*", 0}}, {{0}}, {0}},
    {"cohere", {{'b', "command"}, {'n', "cohere"}}, {{"command", "*command*", 0}}, {{0}}, {0}},
    {"deepseek",
     {{'N', "deepseek"}, {'b', "deepseek"}},
     {{"r1", "*deepseek-r1*", 0},
      {"reasoner", "*deepseek-reasoner*", 0},
      {"flash", "*deepseek*v4*flash*", 0},
      {"flash", "*deepseek-flash", 0},
      {"pro", "*deepseek*v4*pro*", 0},
      {"v4", "*deepseek-v4*", 0},
      {"v3", "*deepseek-v3*", 0}},
     {{"deepseek-v", true}},
     {0}},
    {"gemini", {{'b', "gemini"}}, {{"lite", "*flash-lite*", 10}, {"flash", "*flash*", 0}, {"pro", "*pro*", 0}}, {{"gemini-", false}}, {0}},
    {"gemma", {{'b', "gemma"}}, {{0}}, {{"gemma-", false}}, {0}},
    {"glm",
     {{'b', "glm"}, {'b', "zai-glm"}},
     {{"flash", "*flash*", 0}, {"air", "*air*", 0}, {"turbo", "*turbo*", 0}, {"vision", "*glm-5v*", 20}},
     {{"glm-", true}},
     {0}},
    {"gpt-oss", {{'N', "gpt-oss"}, {'b', "gpt-oss"}}, {{"120b", "*gpt-oss-120b*", 0}, {"20b", "*gpt-oss-20b*", 0}}, {{0}}, {0}},
    {"kimi",
     {{'n', "moonshotai"}, {'b', "kimi"}, {'e', "k3"}, {'e', "k3-256k"}},
     {{"k2", "*kimi-k2*", 0},
      {"k2.5", "*kimi-k2.5*", 10},
      {"k2.6", "*kimi-k2.6*", 10},
      {"k2.6", "*kimi-k2p6*", 10},
      {"k2.7-code", "*kimi-k2.7-code*", 10},
      {"k2.7-code", "*kimi-k2-7-code*", 10},
      {"k2.7-code", "*kimi-k2p7*code*", 10},
      {"k2-thinking", "*kimi-k2-thinking*", 20},
      {"k3", "*kimi-k3*", 0},
      {"k3", "k3", 0},
      {"k3", "k3-256k", 0}},
     {{0}},
     {0}},
    {"meta",
     {{'b', "muse-spark"}, {'b', "muse-wonder"}, {'b', "llama"}, {'b', "meta-llama"}, {'n', "meta-llama"}},
     {{"llama", "*llama*", 0}, {"muse-spark", "*muse-spark*", 0}, {"muse-wonder", "*muse-wonder*", 0}},
     {{"muse-spark-", false}, {"muse-wonder-", false}},
     {0}},
    {"mimo", {{'b', "mimo"}}, {{"v2", "*mimo-v2*", 0}}, {{0}}, {0}},
    {"minimax",
     {{'n', "minimax"}, {'b', "minimax"}, {'b', "hailuo"}},
     {{"m1", "*minimax-m1*", 0}, {"m2", "*minimax-m2*", 0}, {"m3", "*minimax-m3*", 0}},
     {{0}},
     {0}},
    {"mistral", {{'b', "mistral"}, {'b', "mixtral"}}, {{"mistral", "*mistral*", 0}, {"mixtral", "*mixtral*", 0}}, {{0}}, {0}},
    {"openai",
     {{'n', "openai"},
      {'N', "openai"},
      {'p', "gpt-"},
      {'p', "chatgpt-"},
      {'p', "codex-"},
      {'e', "o1"},
      {'p', "o1-"},
      {'p', "o1."},
      {'e', "o3"},
      {'p', "o3-"},
      {'p', "o3."},
      {'e', "o4"},
      {'p', "o4-"},
      {'p', "o4."}},
     {{"gpt", "gpt-*", 0},
      {"chatgpt", "chatgpt-*", 0},
      {"gpt-image", "gpt-image-*", 0},
      {"gpt-image", "chatgpt-image-*", 0},
      {"o-series", "o1", 0},
      {"o-series", "o1-*", 0},
      {"o-series", "o1.*", 0},
      {"o-series", "o3", 0},
      {"o-series", "o3-*", 0},
      {"o-series", "o3.*", 0},
      {"o-series", "o4", 0},
      {"o-series", "o4-*", 0},
      {"o-series", "o4.*", 0},
      {"codex", "*codex*", 10},
      {"codex-spark", "*codex-spark*", 20}},
     {{"chatgpt-", false}, {"gpt-", true}, {"o", false}},
     {"o1", "o3", "o4"}},
    {"qwen",
     {{'b', "qwen"}, {'b', "deepseek-r1-distill-qwen"}, {'g', "*distill-qwen*"}},
     {{"qwq", "*qwq*", 0},
      {"coder", "*qwen3-coder*", 0},
      {"next", "*qwen3-next*", 0},
      {"omni", "*qwen3-omni*", 0},
      {"vl", "*qwen3-vl*", 0},
      {"qwenlong", "*qwenlong*", 0}},
     {{"qwen", false}},
     {0}},
    {"stepfun", {{'b', "step"}}, {{"step", "*step*", 0}}, {{0}}, {0}},
    {"xai",
     {{'p', "cursor-grok-"}, {'b', "grok"}, {'n', "x-ai"}, {'N', "xai"}},
     {{"grok", "*grok*", 0}},
     {{"cursor-grok-", false}, {"grok-", true}},
     {0}},
};

/* reviewed pattern overrides that matter for self-hosted ids (taxonomy/qwen.kdl prismml-bonsai-*) */
static const struct {
    const char *glob, *cls;
    int rev[3];
} OVERRIDES[] = {
    {"*bonsai-27b*", "qwen", {3, 6, 0}},
    {"*bonsai-2-27b*", "qwen", {3, 8, 0}},
};

/* cascade.ts globMatch: `*` wildcards only */
static bool glob_match(const char *pat, const char *s) {
    const char *star = strchr(pat, '*');
    if (!star) return strcmp(pat, s) == 0;
    size_t hl = (size_t)(star - pat);
    if (strncmp(s, pat, hl)) return false;
    const char *rest = s + hl;
    const char *p = star + 1;
    for (;;) {
        const char *next = strchr(p, '*');
        if (!next) { /* last segment: suffix */
            size_t ll = strlen(p), rl = strlen(rest);
            return ll == 0 || (rl >= ll && strcmp(rest + rl - ll, p) == 0);
        }
        size_t sl = (size_t)(next - p);
        if (sl) {
            char seg[128];
            if (sl >= sizeof seg) return false;
            memcpy(seg, p, sl);
            seg[sl] = 0;
            const char *f = strstr(rest, seg);
            if (!f) return false;
            rest = f + sl;
        }
        p = next + 1;
    }
}

static bool bounded_match(const char *v, size_t vn, const char *tok) {
    size_t tn = strlen(tok);
    if (vn < tn || strncmp(v, tok, tn)) return false;
    if (vn == tn) return true;
    char c = v[tn];
    return c == '-' || c == '_' || c == '.' || c == ':' || (c >= '0' && c <= '9');
}

static bool namespace_match(const char *lower, const char *tok, bool bounded) {
    const char *p = lower;
    while (*p) {
        size_t n = bounded ? strcspn(p, "/.:") : strcspn(p, "/");
        if (n && (bounded ? bounded_match(p, n, tok) : (n == strlen(tok) && !strncmp(p, tok, n)))) return true;
        p += n;
        if (*p) p++;
    }
    return false;
}

static bool matcher_matches(const matcher_t *m, const char *lower, const char *bare) {
    switch (m->kind) {
    case 'e': return strcmp(bare, m->token) == 0;
    case 'b': return bounded_match(bare, strlen(bare), m->token);
    case 'n': return namespace_match(lower, m->token, false);
    case 'N': return namespace_match(lower, m->token, true);
    case 'p': return str_starts(bare, m->token);
    case 'g': return glob_match(m->token, bare);
    }
    return false;
}

static int matcher_rank(char k) { return k == 'e' ? 4 : k == 'b' ? 3 : (k == 'n' || k == 'N') ? 2 : k == 'p' ? 1 : 0; }

static int non_wildcard(const char *g) {
    int n = 0;
    for (; *g; g++) n += *g != '*';
    return n;
}

static const char *rank_family(const class_t *c, const char *subject, bool *tied) {
    const char *win = NULL;
    int wp = -1, wb = -1;
    *tied = false;
    for (size_t i = 0; i < ARRAY_LEN(c->f) && c->f[i].id; i++) {
        if (!glob_match(c->f[i].glob, subject)) continue;
        int p = c->f[i].prio, b = non_wildcard(c->f[i].glob);
        if (win && p == wp && b == wb && strcmp(win, c->f[i].id)) {
            *tied = true;
        } else if (!win || wp < p || (wp == p && wb < b)) {
            win = c->f[i].id, wp = p, wb = b;
            *tied = false;
        }
    }
    return win;
}

static const char *classify_family(const class_t *c, const char *bare) {
    bool tied;
    const char *f = rank_family(c, bare, &tied);
    if (!tied) return f;
    /* stripClassNamespace: a `vendor.model` / `vendor:model` id names the product after the class namespace */
    size_t sep = strcspn(bare, ".:");
    if (sep == 0 || !bare[sep] || !bare[sep + 1]) return NULL;
    for (size_t i = 0; i < ARRAY_LEN(c->m) && c->m[i].token; i++) {
        if (strlen(c->m[i].token) == sep && !strncmp(bare, c->m[i].token, sep)) {
            f = rank_family(c, bare + sep + 1, &tied);
            return tied ? NULL : f;
        }
    }
    return NULL;
}

/* revision.ts parseRevisionPrefix: up to three components; a component running into a letter is a size token */
static bool parse_revision_prefix(const char *v, int out[3]) {
    int count = 0;
    size_t i = 0;
    out[0] = out[1] = out[2] = 0;
    while (count < 3) {
        size_t start = i;
        while (isdigit((unsigned char)v[i])) i++;
        bool size_tok = isalpha((unsigned char)v[i]);
        int comp = -1;
        if (!size_tok && i > start) {
            comp = 0;
            for (size_t k = start; k < i && comp >= 0; k++) {
                comp = comp * 10 + (v[k] - '0');
                if (comp > 255) comp = -1;
            }
        }
        if (comp < 0) return count > 0;
        out[count++] = comp;
        char sep = v[i];
        if (!sep) break;
        if ((sep != '.' && sep != '-') || !isdigit((unsigned char)v[i + 1])) break;
        i++;
    }
    return true;
}

static bool extract_revision(const class_t *c, const char *bare, int rev[3]) {
    for (size_t i = 0; i < ARRAY_LEN(c->skip) && c->skip[i]; i++)
        if (!strcmp(bare, c->skip[i])) return false;
    for (size_t i = 0; i < ARRAY_LEN(c->r) && c->r[i].prefix; i++) {
        const char *tail = NULL;
        if (c->r[i].anywhere) {
            const char *f = strstr(bare, c->r[i].prefix);
            if (f) tail = f + strlen(c->r[i].prefix);
        } else if (str_starts(bare, c->r[i].prefix)) {
            tail = bare + strlen(c->r[i].prefix);
        }
        if (!tail) continue;
        while (*tail && !isdigit((unsigned char)*tail)) tail++;
        if (!*tail) return false;
        return parse_revision_prefix(tail, rev);
    }
    return false;
}

static const class_t *class_by_id(const char *id) {
    for (size_t i = 0; i < ARRAY_LEN(CLASSES); i++)
        if (!strcmp(CLASSES[i].id, id)) return &CLASSES[i];
    return NULL;
}

void model_identity_classify(const char *provider, const char *id, model_identity_t *out) {
    (void)provider;
    memset(out, 0, sizeof(*out));
    out->cls = "unknown";
    if (!id) return;
    char *trimmed = str_trim(id);
    char *lower = str_lower(trimmed);
    free(trimmed);
    const char *slash = strrchr(lower, '/');
    const char *bare = slash ? slash + 1 : lower;
    for (size_t i = 0; i < ARRAY_LEN(OVERRIDES); i++) {
        if (glob_match(OVERRIDES[i].glob, bare)) {
            const class_t *c = class_by_id(OVERRIDES[i].cls);
            out->cls = c->id;
            out->family = classify_family(c, bare);
            memcpy(out->rev, OVERRIDES[i].rev, sizeof out->rev);
            out->has_rev = true;
            free(lower);
            return;
        }
    }
    const class_t *win = NULL;
    int wr = -1, wl = -1;
    bool tied = false;
    for (size_t i = 0; i < ARRAY_LEN(CLASSES); i++) {
        const class_t *c = &CLASSES[i];
        for (size_t k = 0; k < ARRAY_LEN(c->m) && c->m[k].token; k++) {
            if (!matcher_matches(&c->m[k], lower, bare)) continue;
            int r = matcher_rank(c->m[k].kind), l = (int)strlen(c->m[k].token);
            if (win && r == wr && l == wl && win != c) {
                tied = true;
            } else if (!win || wr < r || (wr == r && wl < l)) {
                win = c, wr = r, wl = l;
                tied = false;
            }
        }
    }
    if (win && !tied) {
        out->cls = win->id;
        out->family = classify_family(win, bare);
        out->has_rev = extract_revision(win, bare, out->rev);
    }
    free(lower);
}

static int rev_cmp(const model_identity_t *idn, int a, int b) {
    int want[3] = {a, b, 0};
    for (int i = 0; i < 3; i++)
        if (idn->rev[i] != want[i]) return idn->rev[i] - want[i];
    return 0;
}
static bool is_cls(const model_identity_t *idn, const char *c) { return str_eq(idn->cls, c); }
static bool is_fam(const model_identity_t *idn, const char *f) { return str_eq(idn->family, f); }
static bool rev_gte(const model_identity_t *idn, int a, int b) { return idn->has_rev && rev_cmp(idn, a, b) >= 0; }

/* ---------- hosts (hosts.ts KNOWN_HOSTS) ---------- */

typedef struct {
    const char *name;
    const char *providers[4];
    const char *prefix;
    const char *markers[4];
} host_t;

static const host_t HOSTS[] = {
    {"openai", {"openai"}, NULL, {"api.openai.com"}},
    {"azureOpenAI", {"azure"}, NULL, {".openai.azure.com", "azure.com/openai", "models.inference.ai.azure.com"}},
    {"openrouter", {"openrouter"}, NULL, {"openrouter.ai"}},
    {"vercelAIGateway", {"vercel-ai-gateway"}, NULL, {"ai-gateway.vercel.sh"}},
    {"anthropic", {"anthropic"}, NULL, {"api.anthropic.com"}},
    {"deepseekDirect", {"deepseek"}, NULL, {"api.deepseek.com"}},
    {"deepseekFamily", {"deepseek"}, NULL, {"deepseek.com"}},
    {"cerebras", {"cerebras"}, NULL, {"cerebras.ai"}},
    {"zai", {"zai"}, NULL, {"api.z.ai"}},
    {"zhipu", {"zhipu-coding-plan"}, NULL, {"open.bigmodel.cn"}},
    {"kilo", {"kilo"}, NULL, {"api.kilo.ai"}},
    {"alibabaDashscope", {"alibaba-coding-plan", "alibaba-token-plan"}, NULL, {"dashscope", "token-plan."}},
    {"xiaomi", {"xiaomi"}, "xiaomi-token-plan-", {"xiaomimimo.com"}},
    {"xai", {"xai", "xai-oauth"}, NULL, {"api.x.ai"}},
    {"mistral", {"mistral"}, NULL, {"mistral.ai"}},
    {"together", {"together"}, NULL, {"api.together.xyz"}},
    {"fireworks", {0}, NULL, {"fireworks.ai"}},
    {"groq", {"groq"}, NULL, {"api.groq.com"}},
    {"minimax", {"minimax", "minimax-code", "minimax-code-cn"}, NULL, {"api.minimax.io", "api.minimaxi.com"}},
    {"qwenPortal", {"qwen-portal"}, NULL, {"portal.qwen.ai"}},
    {"nvidia", {"nvidia"}, NULL, {"integrate.api.nvidia.com"}},
    {"venice", {"venice"}, NULL, {"api.venice.ai"}},
    {"moonshotNative", {"moonshot", "kimi-code"}, NULL, {"api.moonshot.ai", "api.kimi.com"}},
    {"googleAistudio", {0}, NULL, {"generativelanguage.googleapis.com"}},
    {"opencode", {"opencode-go", "opencode-zen"}, NULL, {"opencode.ai"}},
    {"chutes", {0}, NULL, {"chutes.ai"}},
};

static const host_t *host_get(const char *name) {
    for (size_t i = 0; i < ARRAY_LEN(HOSTS); i++)
        if (!strcmp(HOSTS[i].name, name)) return &HOSTS[i];
    abort(); /* programming error: unknown host class */
}

static bool url_has(const char *url, const char *h) {
    if (!url || !*url) return false;
    const host_t *s = host_get(h);
    for (size_t i = 0; i < ARRAY_LEN(s->markers) && s->markers[i]; i++)
        if (strcasestr(url, s->markers[i])) return true;
    return false;
}

static bool model_on(const char *provider, const char *url, const char *h) {
    const host_t *s = host_get(h);
    for (size_t i = 0; i < ARRAY_LEN(s->providers) && s->providers[i]; i++)
        if (str_eq(provider, s->providers[i])) return true;
    if (s->prefix && provider && str_starts(provider, s->prefix)) return true;
    return url_has(url, h);
}

bool compat_local_base_url(const char *base_url) {
    if (!base_url) return false;
    const char *p = strstr(base_url, "://");
    if (!p) return false; /* new URL() throws without a scheme */
    p += 3;
    const char *at = NULL;
    for (const char *q = p; *q && *q != '/' && *q != '?' && *q != '#'; q++)
        if (*q == '@') at = q;
    if (at) p = at + 1;
    char host[256];
    size_t n = 0;
    if (*p == '[') {
        const char *e = strchr(p, ']');
        if (!e) return false;
        n = (size_t)(e - p + 1);
    } else {
        n = strcspn(p, ":/?#");
    }
    if (n == 0 || n >= sizeof host) return false;
    for (size_t i = 0; i < n; i++) host[i] = (char)tolower((unsigned char)p[i]);
    host[n] = 0;
    if (n > 1 && host[n - 1] == '.') host[--n] = 0;
    if (!strcmp(host, "localhost") || !strcmp(host, "127.0.0.1") || !strcmp(host, "0.0.0.0") || !strcmp(host, "[::1]"))
        return true;
    if (str_starts(host, "10.") || str_starts(host, "192.168.")) return true;
    if (str_starts(host, "172.")) {
        int b = atoi(host + 4);
        const char *d = host + 4;
        while (isdigit((unsigned char)*d)) d++;
        if (d > host + 4 && *d == '.' && b >= 16 && b <= 31) return true;
    }
    return str_ends(host, ".local");
}

/* ---------- detected chat-completions baseline (resolve.ts) ---------- */

/* every key the resolved record declares (present even when undefined), in omp's order: models.yml `compat`
 * overrides apply only to these (applyCompatOverrides ignores unknown keys) */
static const char *const COMPAT_KEYS[] = {
    "supportsStore", "supportsDeveloperRole", "supportsMultipleSystemMessages", "supportsReasoningEffort",
    "trustExplicitThinkingOnly", "supportsReasoningParams", "supportsSamplingParams", "supportsPenaltyAndStopParams",
    "reasoningEffortMap", "supportsUsageInStreaming", "alwaysSendMaxTokens", "disableReasoningOnForcedToolChoice",
    "disableReasoningOnToolChoice", "disableReasoningWithTools", "supportsToolChoice", "supportsForcedToolChoice",
    "supportsNamedToolChoice", "maxTokensField", "requiresToolResultName", "requiresAssistantAfterToolResult",
    "requiresThinkingAsText", "requiresMistralToolIds", "thinkingFormat", "kimiApiFormat", "reasoningDisableMode",
    "omitReasoningEffort", "includeEncryptedReasoning", "filterReasoningHistory", "thinkingKeep",
    "reasoningContentField", "mistralReasoningContentParts", "requiresReasoningContentForToolCalls",
    "requiresReasoningContentForAllAssistantTurns", "allowsSyntheticReasoningContentForToolCalls",
    "syntheticReasoningContentFallback", "replayReasoningContent", "qwenPreserveThinking",
    "qwenTemplateReasoningEffort", "requiresAssistantContentForToolCalls", "cacheControlFormat",
    "supportsPromptCacheBreakpoints", "promptCacheBreakpointTtl", "openRouterRouting", "vercelGatewayRouting",
    "isOpenRouterHost", "wireModelIdMode", "isVercelGatewayHost", "supportsStrictMode", "extraBody", "toolStrictMode",
    "toolSchemaFlavor", "streamFirstEventTimeoutMs", "streamIdleTimeoutMs", "stripDeepseekSpecialTokens",
    "stripThinkingHistory", "officialEndpoint", "streamMarkupHealingPattern", "reasoningDeltasMayBeCumulative",
    "emptyLengthFinishIsContextError", "usesOpenAIToolCallIdLimit", "promptCacheSessionHeader",
    "dropThinkingWhenReasoningEffort", "nativeKimiK3Reasoning", "zaiReasoningEffortDialect", "clampOutputToModelMax",
    "stripImageInput", "thinkingLoopGuard", "rejectRootObjectUnion", "retryWithoutStrictOnGrammarError",
    "supportsPromptCacheKey", "streamRevision", "whenThinking"};

static bool known_key(const char *k) {
    for (size_t i = 0; i < ARRAY_LEN(COMPAT_KEYS); i++)
        if (!strcmp(COMPAT_KEYS[i], k)) return true;
    return false;
}

static const char *disable_mode_for(const char *fmt) {
    if (str_eq(fmt, "openrouter")) return "openrouter-enabled-false";
    if (str_eq(fmt, "zai") || str_eq(fmt, "kimi")) return "zai-thinking-disabled";
    if (str_eq(fmt, "qwen")) return "qwen-enable-thinking-false";
    if (str_eq(fmt, "qwen-chat-template")) return "qwen-template-false";
    if (str_eq(fmt, "chat-template")) return "chat-template-thinking-false";
    return "lowest-effort";
}

static bool official_openai(const char *provider, const char *url) {
    if (!str_eq(provider, "openai")) return false;
    if (!url || !*url) return true;
    const char *p = strstr(url, "://");
    if (!p) return false;
    p += 3;
    size_t n = strcspn(p, ":/?#");
    return n == strlen("api.openai.com") && !strncasecmp(p, "api.openai.com", n);
}

typedef struct {
    const char *efforts[6];
    bool has_efforts, requires_effort, has_requires;
    const char *mode;
    json_t *wire; /* compat keys the rules assign (applyWireAxes) */
} rules_t;

static void rule_efforts(rules_t *r, const char *a, const char *b, const char *c, const char *d) {
    memset(r->efforts, 0, sizeof r->efforts);
    r->efforts[0] = a, r->efforts[1] = b, r->efforts[2] = c, r->efforts[3] = d;
    r->has_efforts = true;
}

static bool token_in(const char *bare, const char *tok) {
    size_t tl = strlen(tok);
    for (const char *p = bare; *p;) {
        size_t n = strcspn(p, "-_.:/");
        if (n == tl && !strncmp(p, tok, n)) return true;
        p += n;
        if (*p) p++;
    }
    return false;
}

/* the KDL rules that reach a custom or local provider id: local backends (providers/{llama.cpp,lm-studio,vllm}.kdl
 * and the qwen class rules scoped to them) and the provider-agnostic gpt-oss / deepseek / kimi class rules */
static void apply_rules(rules_t *r, const char *provider, const model_identity_t *idn, const char *id) {
    json_t *w = r->wire;
    bool qwen = is_cls(idn, "qwen"), q38 = qwen && rev_gte(idn, 3, 8);
    if (str_eq(provider, "llama.cpp")) {
        json_set_bool(w, "supportsStore", false);
        json_set_bool(w, "supportsDeveloperRole", false);
        json_set_bool(w, "supportsReasoningEffort", false);
        json_set_bool(w, "supportsNamedToolChoice", false);
        if (qwen) {
            r->mode = "effort";
            rule_efforts(r, "high", NULL, NULL, NULL);
            json_set_str(w, "thinkingFormat", "qwen");
            json_set_str(w, "reasoningDisableMode", "qwen-enable-thinking-false");
            json_set_bool(w, "qwenPreserveThinking", true);
        }
    } else if (str_eq(provider, "lm-studio")) {
        json_set_bool(w, "supportsNamedToolChoice", false);
        if (q38) json_set_str(w, "thinkingFormat", "qwen");
    } else if (str_eq(provider, "vllm")) {
        if (qwen) json_set_str(w, "thinkingFormat", "qwen-chat-template");
    }
    if (q38 && (str_eq(provider, "llama.cpp") || str_eq(provider, "lm-studio") || str_eq(provider, "vllm"))) {
        json_set_bool(w, "supportsReasoningEffort", true);
        json_set_bool(w, "qwenTemplateReasoningEffort", true);
        rule_efforts(r, "low", "medium", "xhigh", NULL);
        r->requires_effort = r->has_requires = true;
    }
    if (is_cls(idn, "gpt-oss")) rule_efforts(r, "low", "medium", "high", NULL);
    if (is_cls(idn, "deepseek")) {
        if (is_fam(idn, "pro") || is_fam(idn, "flash") || is_fam(idn, "v4")) rule_efforts(r, "low", "high", "max", NULL);
        else if (is_fam(idn, "r1") || is_fam(idn, "v3") || is_fam(idn, "reasoner")) rule_efforts(r, "high", "max", NULL, NULL);
        char *low = str_lower(id);
        const char *slash = strrchr(low, '/');
        const char *bare = slash ? slash + 1 : low;
        bool keep = token_in(bare, "ocr") || token_in(bare, "vision") || glob_match("*v4.1-flash*", bare) ||
                    glob_match("*deepseek-flash", bare);
        json_set_bool(w, "stripImageInput", !keep);
        free(low);
    }
    if (is_cls(idn, "kimi")) json_set_str(w, "streamRevision", "possible");
}

/* detectOpenAI + detectOpenAICompat + applyWireAxes + applyCompatOverrides + fixupOpenAICompat */
static json_t *detect_openai(const char *provider, const char *url, const model_identity_t *idn, bool reasoning,
                             const json_t *user, rules_t *rules) {
    const char *u = url ? url : "";
#define ON(h) model_on(provider, u, h)
#define URL(h) url_has(u, h)
    bool is_zai = ON("zai"), is_zhipu = ON("zhipu"), moonshot = ON("moonshotNative"), xiaomi_host = ON("xiaomi");
    bool ds_direct = ON("deepseekDirect");
    bool ds_family = ON("deepseekFamily") || is_cls(idn, "deepseek");
    bool ds_reasoning = ds_family && reasoning;
    bool local_provider = str_eq(provider, "llama.cpp") || str_eq(provider, "lm-studio") || str_eq(provider, "vllm") ||
                          str_eq(provider, "ollama");
    bool local_compat = !str_eq(provider, "litellm") && (local_provider || compat_local_base_url(u));
    bool local_serving = local_compat || compat_local_base_url(u);
    bool opencode_host = ON("opencode");
    bool opencode_provider = str_eq(provider, "opencode-go") || str_eq(provider, "opencode-zen");
    bool ds_direct_reasoning = ds_direct && ds_reasoning;
    bool venice = ON("venice");
    bool kimi = is_cls(idn, "kimi");
    bool requires_enabled_thinking = moonshot && kimi && is_fam(idn, "k2.7-code");
    bool xiaomi_mimo = xiaomi_host && is_cls(idn, "mimo");
    bool openrouter = ON("openrouter");

    bool cerebras = ON("cerebras"), cerebras_host = URL("cerebras"), kilo = ON("kilo"), alibaba = ON("alibabaDashscope");
    bool grok = ON("xai"), mistral = ON("mistral"), aistudio = URL("googleAistudio"), openai_host = ON("openai");
    bool azure = ON("azureOpenAI"), vercel = ON("vercelAIGateway"), together = ON("together"), fireworks = URL("fireworks");
    bool groq = ON("groq"), minimax = ON("minimax"), qwen_portal = ON("qwenPortal");
    bool moonshot_kimi = kimi && moonshot, moonshot_k3 = moonshot_kimi && is_fam(idn, "k3");
    bool preserved_kimi = moonshot_kimi && is_fam(idn, "k2.6");
    bool anthropic_model = ON("anthropic") || is_cls(idn, "anthropic");
    bool qwen = is_cls(idn, "qwen");
    bool zai_effort = (is_zai || is_zhipu) && is_cls(idn, "glm") && rev_gte(idn, 5, 2);
    bool nonstandard = cerebras || grok || mistral || aistudio || URL("chutes") || URL("deepseekFamily") || fireworks ||
                       alibaba || is_zai || is_zhipu || kilo || qwen || xiaomi_host || moonshot || opencode_host;
    bool use_max_tokens = mistral || moonshot || is_zai || is_zhipu || URL("chutes") || fireworks || ds_direct;
    bool cache_bp = official_openai(provider, u) && is_cls(idn, "openai") && rev_gte(idn, 5, 6);
    bool multi_sys = !minimax && !alibaba && !qwen_portal && !qwen &&
                     (openai_host || azure || openrouter || cerebras || together || fireworks || groq || ds_family ||
                      mistral || grok || is_zai || is_zhipu || str_eq(provider, "github-copilot") || str_eq(provider, "zenmux"));
    int64_t idle = -1;
    if (is_cls(idn, "glm") && rev_gte(idn, 5, 0) && !is_fam(idn, "vision") && (URL("zai") || URL("zhipu") || URL("opencode")))
        idle = 600000;
    else if (is_cls(idn, "mimo") && URL("xiaomi")) idle = 300000;
    else if (reasoning && kimi && (is_fam(idn, "k3") || is_fam(idn, "k2.7-code")) && URL("moonshotNative")) idle = 300000;
    else if (reasoning && is_cls(idn, "deepseek") && URL("deepseekDirect")) idle = 300000;
    else if (local_serving) idle = 300000;
    const char *fmt = (kimi && !is_fam(idn, "k3") && URL("moonshotNative")) || URL("zai") || URL("zhipu") ||
                              (is_cls(idn, "mimo") && URL("xiaomi"))
                          ? "zai"
                      : URL("openrouter")                   ? "openrouter"
                      : qwen && URL("nvidia")               ? "qwen-chat-template"
                      : qwen && (fireworks || URL("venice")) ? "openai"
                      : URL("alibabaDashscope") || qwen     ? "qwen"
                                                            : "openai";
    const char *heal = (str_eq(provider, "kimi-code") || str_eq(provider, "moonshot") ||
                        (kimi && idn->family && str_starts(idn->family, "k2")))
                           ? "kimi"
                       : is_cls(idn, "deepseek")      ? "dsml"
                       : official_openai(provider, u) ? NULL
                                                      : "thinking";

    json_t *c = json_obj();
    json_set_bool(c, "supportsStore", !nonstandard);
    json_set_bool(c, "supportsDeveloperRole", openai_host || azure);
    json_set_bool(c, "supportsMultipleSystemMessages", multi_sys);
    json_set_bool(c, "supportsReasoningEffort", !grok && !xiaomi_mimo && (!(is_zai || is_zhipu) || zai_effort));
    json_set_bool(c, "supportsReasoningParams", true);
    json_set_bool(c, "supportsSamplingParams", true);
    json_set_bool(c, "supportsPenaltyAndStopParams", !(grok && reasoning));
    json_set(c, "reasoningEffortMap", json_obj());
    json_set_bool(c, "supportsUsageInStreaming", !cerebras_host);
    json_set_bool(c, "alwaysSendMaxTokens", kimi);
    json_set_bool(c, "disableReasoningOnForcedToolChoice", (kimi && !moonshot_k3) || anthropic_model);
    json_set_bool(c, "disableReasoningOnToolChoice", ds_family && reasoning && !openrouter);
    json_set_bool(c, "disableReasoningWithTools", false);
    json_set_bool(c, "supportsToolChoice", !ds_direct_reasoning);
    json_set_bool(c, "supportsForcedToolChoice", !requires_enabled_thinking && !(opencode_host && ds_reasoning));
    json_set_bool(c, "supportsNamedToolChoice", true);
    json_set_str(c, "maxTokensField", use_max_tokens ? "max_tokens" : "max_completion_tokens");
    json_set_bool(c, "requiresToolResultName", mistral);
    json_set_bool(c, "requiresAssistantAfterToolResult", mistral);
    json_set_bool(c, "requiresThinkingAsText", mistral);
    json_set_bool(c, "requiresMistralToolIds", mistral);
    json_set_str(c, "thinkingFormat", fmt);
    json_set_str(c, "reasoningDisableMode", venice ? "venice-disable-thinking" : disable_mode_for(fmt));
    json_set_bool(c, "omitReasoningEffort", false);
    json_set_bool(c, "includeEncryptedReasoning", true);
    json_set_bool(c, "filterReasoningHistory", openrouter && anthropic_model);
    if (preserved_kimi) json_set_str(c, "thinkingKeep", "all");
    json_set_str(c, "reasoningContentField", "reasoning_content");
    json_set_bool(c, "requiresReasoningContentForToolCalls",
                  (kimi && !opencode_provider) || ds_reasoning || xiaomi_mimo || (openrouter && reasoning));
    json_set_bool(c, "requiresReasoningContentForAllAssistantTurns", (ds_reasoning || xiaomi_mimo) && !openrouter);
    json_set_bool(c, "allowsSyntheticReasoningContentForToolCalls", (!ds_family || !reasoning) && !xiaomi_mimo);
    json_set_bool(c, "replayReasoningContent", local_compat);
    json_set_bool(c, "qwenPreserveThinking", (str_eq(fmt, "qwen") || str_eq(fmt, "qwen-chat-template")) && local_compat);
    json_set_bool(c, "qwenTemplateReasoningEffort", false);
    json_set_bool(c, "requiresAssistantContentForToolCalls", kimi || ds_direct_reasoning);
    if (openrouter && anthropic_model) json_set_str(c, "cacheControlFormat", "anthropic");
    json_set_bool(c, "supportsPromptCacheBreakpoints", cache_bp);
    if (cache_bp) json_set_str(c, "promptCacheBreakpointTtl", "30m");
    json_set_bool(c, "isOpenRouterHost", openrouter);
    json_set_str(c, "wireModelIdMode", URL("openrouter") ? "openrouter" : "raw");
    json_set_bool(c, "isVercelGatewayHost", vercel);
    json_set_bool(c, "supportsStrictMode", URL("openai") || URL("azureOpenAI") || URL("cerebras") || URL("together") ||
                                               URL("openrouter") || URL("deepseekFamily"));
    json_set_str(c, "toolStrictMode", cerebras_host ? "all_strict" : "mixed");
    if (moonshot || kimi) json_set_str(c, "toolSchemaFlavor", "moonshot-mfjs");
    else if (local_compat) json_set_str(c, "toolSchemaFlavor", "grammar");
    if (local_serving) json_set_int(c, "streamFirstEventTimeoutMs", 0);
    if (idle > 0) json_set_int(c, "streamIdleTimeoutMs", idle);
    json_set_bool(c, "stripDeepseekSpecialTokens", false);
    if (official_openai(provider, u)) json_set_bool(c, "officialEndpoint", true);
    if (heal) json_set_str(c, "streamMarkupHealingPattern", heal);
    json_set_bool(c, "reasoningDeltasMayBeCumulative", false);
    json_set_bool(c, "emptyLengthFinishIsContextError", false);
    json_set_bool(c, "usesOpenAIToolCallIdLimit", false);
    if (URL("xai")) json_set_str(c, "promptCacheSessionHeader", "x-grok-conv-id");
    json_set_bool(c, "dropThinkingWhenReasoningEffort", false);
    json_set_bool(c, "nativeKimiK3Reasoning", false);
    json_set_bool(c, "zaiReasoningEffortDialect", false);
    json_set_bool(c, "clampOutputToModelMax", local_compat);
    json_set_bool(c, "stripImageInput", false);
    json_set_bool(c, "rejectRootObjectUnion", false);
    json_set_bool(c, "retryWithoutStrictOnGrammarError", false);
    json_set_bool(c, "supportsPromptCacheKey", false);
#undef ON
#undef URL

    /* applyWireAxes, then the models.yml overrides */
    const char *k;
    json_t *v;
    json_obj_foreach(rules->wire, i, k, v) json_set(c, k, json_clone(v));
    json_obj_foreach(user, j, k, v) {
        if (known_key(k) && !json_is_null(v)) json_set(c, k, json_clone(v));
    }

    /* fixupOpenAICompat */
    if (!json_get(user, "reasoningDisableMode") && !json_get(rules->wire, "reasoningDisableMode")) {
        json_set_str(c, "reasoningDisableMode", requires_enabled_thinking ? "omit"
                                                : ds_direct_reasoning     ? "zai-thinking-disabled"
                                                : venice                  ? "venice-disable-thinking"
                                                                          : disable_mode_for(json_get_str(c, "thinkingFormat")));
    }
    if (!json_get(user, "omitReasoningEffort") && !json_get(rules->wire, "omitReasoningEffort") &&
        !json_get_bool(c, "supportsReasoningEffort", true))
        json_set_bool(c, "omitReasoningEffort", true);
    json_t *wt = reasoning ? json_get_obj(user, "whenThinking") : NULL;
    if (wt) { /* the whenThinking variant: the full record with the policy's overrides applied */
        json_t *var = json_clone(c);
        json_del(var, "whenThinking");
        json_obj_foreach(wt, i2, k, v) {
            if (known_key(k) && !json_is_null(v)) json_set(var, k, json_clone(v));
        }
        if (!json_get(wt, "reasoningDisableMode"))
            json_set_str(var, "reasoningDisableMode",
                         venice ? "venice-disable-thinking" : disable_mode_for(json_get_str(var, "thinkingFormat")));
        if (!json_get(wt, "omitReasoningEffort") && !json_get_bool(var, "supportsReasoningEffort", true))
            json_set_bool(var, "omitReasoningEffort", true);
        json_set(c, "whenThinking", var);
    } else {
        json_del(c, "whenThinking");
    }
    return c;
}

/* ---------- thinking (resolveThinkingPolicy) ---------- */

static const char *const EFFORT_ORDER[] = {"minimal", "low", "medium", "high", "xhigh", "max"};

static int effort_idx(const char *e) {
    for (int i = 0; i < 6; i++)
        if (str_eq(e, EFFORT_ORDER[i])) return i;
    return -1;
}

/* models-config-schema-bundle.ts ModelThinkingSchema pipe: `efforts` beats legacy `levels`, which beats the
 * `minLevel`..`maxLevel` range; the legacy keys never survive */
static void normalize_thinking(json_t *t) {
    if (!json_is_obj(t)) return;
    json_t *levels = json_take(t, "levels");
    json_t *minl = json_take(t, "minLevel"), *maxl = json_take(t, "maxLevel");
    if (!json_get_arr(t, "efforts")) {
        json_t *eff = NULL;
        if (json_is_arr(levels)) {
            eff = levels;
            levels = NULL;
        } else if (json_sv(minl) && json_sv(maxl) && effort_idx(json_sv(minl)) >= 0 && effort_idx(json_sv(maxl)) >= 0) {
            int lo = effort_idx(json_sv(minl)), hi = MAX(lo, effort_idx(json_sv(maxl)));
            eff = json_arr();
            for (int i = lo; i <= hi; i++) json_push_str(eff, EFFORT_ORDER[i]);
        }
        if (eff) { /* keep omp's key order: mode, efforts, then the rest */
            json_t *rest = json_clone(t);
            json_t *mode = json_take(rest, "mode");
            while (json_len(t)) json_free(json_take(t, t->u.obj.keys[0]));
            if (mode) json_set(t, "mode", mode);
            json_set(t, "efforts", eff);
            const char *k;
            json_t *v;
            json_obj_foreach(rest, i, k, v) json_set(t, k, json_clone(v));
            json_free(rest);
        }
    }
    json_free(levels);
    json_free(minl);
    json_free(maxl);
}

static void resolve_thinking(json_t *model, const json_t *compat, const rules_t *rules, bool openai_chat) {
    bool reasoning = json_get_bool(model, "reasoning", false);
    json_t *t = json_get_obj(model, "thinking");
    if (!reasoning) { /* unverified: neither an explicit ladder nor a rule makes a non-reasoning model think */
        json_del(model, "thinking");
        return;
    }
    if (!openai_chat) return;
    bool qwen_template = json_get_bool(compat, "qwenTemplateReasoningEffort", false);
    bool requires = rules->has_requires ? rules->requires_effort : qwen_template;
    if (t && json_len(json_get_arr(t, "efforts")) > 0) { /* fillExplicitThinking */
        if (requires && !json_get(t, "requiresEffort")) json_set_bool(t, "requiresEffort", true);
        return;
    }
    json_t *cfg = json_obj();
    json_set_str(cfg, "mode", rules->mode ? rules->mode : "effort");
    json_t *eff = json_arr();
    if (rules->has_efforts) {
        for (int i = 0; i < 6 && rules->efforts[i]; i++) json_push_str(eff, rules->efforts[i]);
    } else { /* fallbackEfforts */
        bool xhigh = str_eq(json_get_str(compat, "thinkingFormat"), "openai") &&
                     json_get_bool(compat, "supportsReasoningEffort", false);
        for (int i = 0; i < (xhigh ? 5 : 4); i++) json_push_str(eff, EFFORT_ORDER[i]);
    }
    json_set(cfg, "efforts", eff);
    if (requires) json_set_bool(cfg, "requiresEffort", true);
    json_set(model, "thinking", cfg);
}

void compat_resolve_model(json_t *model) {
    if (!json_is_obj(model)) return;
    const char *id = json_get_str(model, "id");
    const char *provider = json_get_str(model, "provider");
    const char *api = json_get_str(model, "api");
    normalize_thinking(json_get_obj(model, "thinking"));
    model_identity_t idn;
    model_identity_classify(provider, id, &idn);
    if (!json_get_obj(model, "identity")) {
        json_t *o = json_obj();
        json_set_str(o, "class", idn.cls);
        if (idn.family) json_set_str(o, "family", idn.family);
        if (idn.has_rev) {
            char r[32];
            snprintf(r, sizeof r, "%d.%d.%d", idn.rev[0], idn.rev[1], idn.rev[2]);
            json_set_str(o, "revision", r);
        }
        json_set(model, "identity", o);
    }
    bool openai_chat = str_eq(api, "openai-completions") || str_eq(api, "openrouter");
    rules_t rules = {.wire = json_obj()};
    json_t *compat = NULL;
    if (openai_chat && id && provider) {
        apply_rules(&rules, provider, &idn, id);
        compat = detect_openai(provider, json_get_str(model, "baseUrl"), &idn, json_get_bool(model, "reasoning", false),
                               json_get_obj(model, "compat"), &rules);
        json_set(model, "compat", compat);
    }
    resolve_thinking(model, compat, &rules, openai_chat);
    json_free(rules.wire);
}

/* ---------- grammar schema flavor (utils/schema/normalize.ts sanitizeSchemaForGrammar) ---------- */

static bool in_list(const char *k, const char *const *l) {
    for (; *l; l++)
        if (!strcmp(k, *l)) return true;
    return false;
}

static const char *const SUB_ARRAY[] = {"anyOf", "oneOf", "allOf", "prefixItems", NULL};
static const char *const SUB_MAP[] = {"properties", "patternProperties", "dependencies", "dependentSchemas", "$defs",
                                      "definitions", NULL};
static const char *const GRAMMAR_VALUE[] = {"items", "additionalItems", "contains", "contentSchema", "propertyNames", "if",
                                            "then", "else", "not", "unevaluatedItems", NULL};

static json_t *open_widening(void) {
    json_t *any = json_arr();
    static const char *const types[] = {"string", "number", "boolean", "object", "array", "null"};
    for (size_t i = 0; i < ARRAY_LEN(types); i++) {
        json_t *o = json_obj();
        json_set_str(o, "type", types[i]);
        json_push(any, o);
    }
    json_t *w = json_obj();
    json_set(w, "anyOf", any);
    return w;
}

static json_t *grammar_node(const json_t *v, bool sub, int depth) {
    if (depth > 64) return json_clone(v);
    if (json_is_bool(v) && sub) {
        if (v->u.b) return open_widening();
        json_t *n = json_obj();
        json_set(n, "not", open_widening());
        return n;
    }
    if (json_is_arr(v)) {
        json_t *a = json_arr(), *it;
        json_arr_foreach(v, i, it) json_push(a, grammar_node(it, sub, depth + 1));
        return a;
    }
    if (!json_is_obj(v)) return json_clone(v);
    json_t *o = json_obj();
    const char *k;
    json_t *c;
    json_obj_foreach(v, i, k, c) {
        json_t *n;
        if (in_list(k, SUB_MAP) && json_is_obj(c)) {
            n = json_obj();
            const char *ck;
            json_t *cv;
            json_obj_foreach(c, j, ck, cv) json_set(n, ck, grammar_node(cv, true, depth + 1));
        } else if (in_list(k, SUB_ARRAY) && json_is_arr(c)) {
            n = json_arr();
            json_t *it;
            json_arr_foreach(c, j, it) json_push(n, grammar_node(it, true, depth + 1));
        } else if (in_list(k, GRAMMAR_VALUE)) {
            n = grammar_node(c, true, depth + 1);
        } else if ((!strcmp(k, "additionalProperties") || !strcmp(k, "unevaluatedProperties")) && !json_is_bool(c)) {
            n = grammar_node(c, true, depth + 1);
        } else {
            n = json_clone(c);
        }
        json_set(o, k, n);
    }
    return o;
}

json_t *compat_schema_for_grammar(const json_t *schema) { return grammar_node(schema, true, 0); }
