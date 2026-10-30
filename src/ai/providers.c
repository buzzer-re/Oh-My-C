/* providers.c - registry of built-in API implementations. */
#include "ai.h"

void ai_providers_init(void) {
    static bool done;
    if (done) return;
    done = true;
    ai_register_api("anthropic-messages", provider_anthropic_stream);
    ai_register_api("openai-completions", provider_openai_completions_stream);
    ai_register_api("openrouter", provider_openai_completions_stream);
    ai_register_api("openai-responses", provider_openai_responses_stream);
    ai_register_api("azure-openai-responses", provider_openai_responses_stream);
    ai_register_api("openai-codex-responses", provider_openai_responses_stream);
    ai_register_api("google-generative-ai", provider_google_stream);
    ai_register_api("ollama-chat", provider_ollama_stream);
}
