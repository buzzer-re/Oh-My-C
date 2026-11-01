/* symbols.c - glyph presets from the embedded upstream symbol table. */
#include "tui/symbols.h"

#include "core/embed.h"
#include "core/json.h"

#include <pthread.h>

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static json_t *g_table;
static const json_t *g_active;   /* presets.<name> */
static const json_t *g_spinners; /* spinners.<name> */
static char g_preset[16] = "unicode";
static VEC(const char *) g_frames[2];

static void load(void) {
    size_t len = 0;
    const char *data = embed_get("assets/tui/symbols.json", &len);
    g_table = data ? json_parse(data, len, NULL) : json_obj();
    g_active = json_get(json_get(g_table, "presets"), g_preset);
    g_spinners = json_get(json_get(g_table, "spinners"), g_preset);
}

static void rebuild_frames(void) {
    const char *types[2] = {"status", "activity"};
    for (int t = 0; t < 2; t++) {
        g_frames[t].n = 0;
        json_t *x;
        json_arr_foreach(json_get_arr(g_spinners, types[t]), i, x) if (json_is_str(x)) vec_push(&g_frames[t], json_sv(x));
    }
}

bool tui_sym_set_preset(const char *preset) {
    pthread_once(&g_once, load);
    const json_t *p = json_get(json_get(g_table, "presets"), preset ? preset : "");
    if (!p) return false;
    snprintf(g_preset, sizeof g_preset, "%s", preset);
    g_active = p;
    g_spinners = json_get(json_get(g_table, "spinners"), preset);
    rebuild_frames();
    return true;
}

const char *tui_sym_preset(void) { return g_preset; }

const char *tui_sym(const char *key) {
    pthread_once(&g_once, load);
    const char *v = json_get_str(g_active, key);
    return v ? v : "";
}

size_t tui_sym_spinner(const char *type, const char *const **frames) {
    pthread_once(&g_once, load);
    int t = str_eq(type, "activity") ? 1 : 0;
    if (!g_frames[t].n) rebuild_frames();
    *frames = g_frames[t].v;
    return g_frames[t].n;
}
