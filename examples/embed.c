/* embed.c - minimal libomc host: registers a native tool, runs a prompt, then drives the RPC protocol.
 *   usage: embed <prompt> [model]      (model defaults to the configured default)
 *   EMBED_REDACT=<word>: also installs a tool result filter that masks <word> in every tool's result */
#include <omc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int events;

static void on_event(void *ud, const char *json) {
    (void)ud;
    events++;
    if (strstr(json, "\"type\":\"response\"")) printf("event: %s\n", json);
}

static char *greet(void *ud, const char *args_json, bool *is_error) {
    (void)ud;
    *is_error = false;
    const char *p = strstr(args_json, "\"name\":\"");
    char name[64] = "world";
    if (p) sscanf(p + 8, "%63[^\"]", name);
    char *r = malloc(128);
    snprintf(r, 128, "hello from the host app, %s", name);
    return r;
}

/* a tool result filter: every result's text with each `word` masked, or NULL to keep it as it is */
static char *redact(void *ud, const char *tool_name, const char *text) {
    (void)tool_name;
    const char *word = ud;
    size_t wl = strlen(word), hits = 0;
    for (const char *p = strstr(text, word); p; p = strstr(p + wl, word)) hits++;
    if (!hits) return NULL;
    char *out = malloc(strlen(text) + hits * sizeof "[redacted]" + 1), *o = out;
    for (const char *p = text, *hit; (hit = strstr(p, word)); p = hit + wl) {
        memcpy(o, p, (size_t)(hit - p));
        o += hit - p;
        memcpy(o, "[redacted]", sizeof "[redacted]" - 1);
        o += sizeof "[redacted]" - 1;
        text = hit + wl;
    }
    strcpy(o, text);
    return out; /* omc copies it, then frees it with free() (no release callback given) */
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <prompt> [model]\n", argv[0]);
        return 2;
    }
    printf("libomc %s (api %d)\n", omc_version(), omc_api_version());
    omc_options_t o = {.struct_size = sizeof o, .model = argc > 2 ? argv[2] : NULL, .no_session = true, .no_mcp = true};
    char *err = NULL;
    omc_session_t *s = omc_session_new(&o, on_event, NULL, &err);
    if (!s) {
        fprintf(stderr, "omc_session_new: %s\n", err);
        omc_free(err);
        return 1;
    }
    omc_register_tool(s, "greet", "Greet someone from the host application",
                      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}", greet, NULL);
    const char *word = getenv("EMBED_REDACT");
    if (word && *word) omc_set_result_filter(s, redact, NULL, (void *)word);
    char *answer = NULL;
    int rc = omc_prompt(s, argv[1], &answer);
    printf("answer(rc=%d): %s\n", rc, answer ? answer : "");
    omc_free(answer);
    omc_send(s, "{\"id\":\"st\",\"type\":\"get_state\"}");
    omc_wait_idle(s, 5000);
    printf("events: %d\n", events > 0 ? 1 : 0);
    omc_session_free(s);
    return rc;
}
