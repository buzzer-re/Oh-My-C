/* think.c - the private `think` scratchpad (port of tools/think.ts): offered with `externalThinking` on a
 * model whose transport can suppress native reasoning; the first request of each user turn forces it and
 * carries forceReasoningOff (coding/app.c). */
#include "tools.h"

static bool thinking_flag(const ai_model_t *m, const char *key) {
    const json_t *v = json_get(m->thinking, key);
    return json_is_bool(v) && json_get_bool(m->thinking, key, false);
}

bool think_supported(const ai_model_t *m) {
    if (!m) return false;
    const char *api = m->api ? m->api : "";
    bool requires_thinking = str_eq(api, "anthropic-messages") && model_compat_bool(m, "requiresThinkingEnabled", false);
    if (m->reasoning && (requires_thinking || (thinking_flag(m, "requiresEffort") && !thinking_flag(m, "suppressWhenOff"))))
        return false;
    /* transports that reject `reasoning.effort` cannot honour forceReasoningOff */
    if (m->reasoning && (model_compat_bool(m, "omitReasoningEffort", false) || !model_compat_bool(m, "supportsReasoningEffort", true)))
        return false;
    if (str_eq(api, "google-generative-ai") || str_eq(api, "google-gemini-cli") || str_eq(api, "google-vertex"))
        return !m->reasoning || str_eq(json_get_str(m->thinking, "mode"), "budget") || thinking_flag(m, "suppressWhenOff");
    return str_eq(api, "openai-responses") || str_eq(api, "azure-openai-responses") || str_eq(api, "openai-codex-responses") ||
           str_eq(api, "anthropic-messages");
}

static int think_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_result_text(out, "------");
    out->details = json_obj();
    json_set_bool(out->details, "recorded", true);
    return 0;
}

agent_tool_t *tool_think_new(void) {
    agent_tool_t *t = agent_tool_new(
        "think", "Think", "private scratchpad; not shown to user",
        schema_lit("{\"type\":\"object\",\"properties\":{\"thoughts\":{\"type\":\"string\",\"description\":\"private "
                   "scratchpad; not shown to user\"}},\"required\":[\"thoughts\"],\"additionalProperties\":false,"
                   "\"description\":\"private scratchpad; not shown to user\"}"),
        think_execute);
    t->summary = xstrdup("Record private scratchpad thoughts");
    t->intent_mode = INTENT_OMIT;
    t->strict = TOOL_STRICT;
    return t;
}
