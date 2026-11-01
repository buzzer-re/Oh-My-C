/* embed.c - minimal libomc host: registers a native tool, runs a prompt, then drives the RPC protocol.
 *   usage: embed <prompt> [model]      (model defaults to the configured default) */
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
