/* providers.c - registry of built-in API implementations. */
#include "ai.h"

/* stream.ts routes provider "gitlab-duo" to streamGitLabDuo whatever the row's api: the chat APIs its rows use
 * dispatch through these wrappers (direct-access token exchange + Cloud Connector proxy). */
static json_t *anthropic_dispatch(const ai_model_t *m, const ai_context_t *c, const ai_stream_opts_t *o, ai_event_cb cb, void *ud) {
    if (str_eq(m->provider, "gitlab-duo")) return provider_gitlab_duo_stream(m, c, o, cb, ud);
    return provider_anthropic_stream(m, c, o, cb, ud);
}
static json_t *completions_dispatch(const ai_model_t *m, const ai_context_t *c, const ai_stream_opts_t *o, ai_event_cb cb, void *ud) {
    if (str_eq(m->provider, "gitlab-duo")) return provider_gitlab_duo_stream(m, c, o, cb, ud);
    return provider_openai_completions_stream(m, c, o, cb, ud);
}
static json_t *responses_dispatch(const ai_model_t *m, const ai_context_t *c, const ai_stream_opts_t *o, ai_event_cb cb, void *ud) {
    if (str_eq(m->provider, "gitlab-duo")) return provider_gitlab_duo_stream(m, c, o, cb, ud);
    return provider_openai_responses_stream(m, c, o, cb, ud);
}

/* stream.ts: OpenRouter speaks the Responses API unless PI_OPENROUTER_RESPONSES=0 */
static json_t *openrouter_dispatch(const ai_model_t *m, const ai_context_t *c, const ai_stream_opts_t *o, ai_event_cb cb, void *ud) {
    if (str_eq(getenv("PI_OPENROUTER_RESPONSES"), "0")) return provider_openai_completions_stream(m, c, o, cb, ud);
    return provider_openai_responses_stream(m, c, o, cb, ud);
}

void ai_providers_init(void) {
    static bool done;
    if (done) return;
    done = true;
    ai_register_api("anthropic-messages", anthropic_dispatch);
    ai_register_api("openai-completions", completions_dispatch);
    ai_register_api("openrouter", openrouter_dispatch);
    ai_register_api("openai-responses", responses_dispatch);
    ai_register_api("azure-openai-responses", provider_openai_responses_stream);
    ai_register_api("openai-codex-responses", provider_openai_responses_stream);
    ai_register_api("google-generative-ai", provider_google_stream);
    ai_register_api("google-vertex", provider_google_stream);
    ai_register_api("google-gemini-cli", provider_gemini_cli_stream);
    ai_register_api("ollama-chat", provider_ollama_stream);
    ai_register_api("bedrock-converse-stream", provider_bedrock_stream);
    ai_register_api("cursor-agent", provider_cursor_stream);
    ai_register_api("devin-agent", provider_devin_stream);
    ai_register_api("gitlab-duo-agent", provider_gitlab_duo_workflow_stream);
}
