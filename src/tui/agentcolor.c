/* agentcolor.c - agent identity colours (omp tui/overlays/agent-identity.ts) */
#include "tui/agentcolor.h"
#include "tui/color.h"
#include "core/core.h"

#include <math.h>
#include <pthread.h>

#define NSUB 6
#define MIN_CONTRAST 3.0
#define MIN_DISTANCE 0.1
#define MIN_CHROMA 0.04
#define DERIVED_CANDIDATES 24
#define LIGHTNESS_STEP 0.1
#define LIGHTNESS_RINGS 3
#define MIN_LIGHTNESS 0.3
#define MAX_LIGHTNESS 0.9

/* terminal.ansi slots tried after the tokens: hues before brights, red left to errors */
static const int ANSI_SLOTS[] = {4, 2, 3, 5, 6, 12, 10, 11, 13, 14};
static const uint32_t FALLBACK[] = {0x268bd2, 0xd9620f, 0xb58900, 0x4c9a2a, 0x2aa198, 0x8b6fd6, 0xd33682};

static double oklab_distance(uint32_t a, uint32_t b) {
    tui_oklch_t x = tui_rgb_to_oklch(a), y = tui_rgb_to_oklch(b);
    double xa = x.c * cos(x.h * M_PI / 180), xb = x.c * sin(x.h * M_PI / 180);
    double ya = y.c * cos(y.h * M_PI / 180), yb = y.c * sin(y.h * M_PI / 180);
    if (!isfinite(xa)) xa = xb = 0;
    if (!isfinite(ya)) ya = yb = 0;
    return sqrt((x.l - y.l) * (x.l - y.l) + (xa - ya) * (xa - ya) + (xb - yb) * (xb - yb));
}

typedef struct {
    double surfaces[4];
    int nsurf;
    uint32_t taken[2 + NSUB];
    int ntaken;
} ctx_t;

static bool readable(const ctx_t *c, uint32_t rgb) {
    double lum = tui_relative_luminance(rgb);
    for (int i = 0; i < c->nsurf; i++) {
        double s = c->surfaces[i];
        if ((MAX(lum, s) + 0.05) / (MIN(lum, s) + 0.05) < MIN_CONTRAST) return false;
    }
    return true;
}

static double distance(const ctx_t *c, uint32_t rgb) {
    double d = INFINITY;
    for (int i = 0; i < c->ntaken; i++) d = MIN(d, oklab_distance(rgb, c->taken[i]));
    return d;
}

static bool usable(const ctx_t *c, uint32_t rgb) { return readable(c, rgb) && distance(c, rgb) >= MIN_DISTANCE; }

/* getBgHex */
static uint32_t bg_hex(const tui_theme_t *th, tui_tc_t slot) {
    return th->rgb[slot] >= 0 ? (uint32_t)th->rgb[slot] : tui_theme_is_light(th) ? 0xffffff : 0x000000;
}

/* resolveAgentPalette */
size_t tui_agent_palette(const tui_theme_t *th, uint32_t *main_rgb, uint32_t out[6]) {
    if (!th) th = tui_theme();
    uint32_t accent = tui_theme_rgb(th, TUI_TC_ACCENT), error = tui_theme_rgb(th, TUI_TC_ERROR);
    *main_rgb = accent;
    ctx_t c = {.nsurf = 0};
    c.surfaces[c.nsurf++] = tui_relative_luminance(th->surface_rgb >= 0 ? (uint32_t)th->surface_rgb : bg_hex(th, TUI_TC_STATUS_LINE_BG));
    static const tui_tc_t CARD_BG[] = {TUI_TC_TOOL_PENDING_BG, TUI_TC_TOOL_SUCCESS_BG, TUI_TC_TOOL_ERROR_BG};
    for (size_t i = 0; i < ARRAY_LEN(CARD_BG); i++)
        if (th->rgb[CARD_BG[i]] >= 0) c.surfaces[c.nsurf++] = tui_relative_luminance((uint32_t)th->rgb[CARD_BG[i]]);
    c.taken[c.ntaken++] = accent;
    c.taken[c.ntaken++] = error;
    uint32_t subs[NSUB];
    int nsubs = 0;
    /* a declared subagentN keeps slot N; generated colours fill the slots the theme leaves unset, in order */
    int64_t declared[NSUB];
    for (int i = 0; i < NSUB; i++) {
        declared[i] = th->rgb[TUI_TC_SUBAGENT1 + i];
        if (declared[i] >= 0) subs[nsubs++] = (uint32_t)declared[i], c.taken[c.ntaken++] = (uint32_t)declared[i];
    }
    int generated_from = nsubs;
#define PUSH(x) (subs[nsubs++] = (x), c.taken[c.ntaken++] = (x))
    for (size_t i = 0; i < ARRAY_LEN(ANSI_SLOTS); i++) {
        int64_t hex = th->ansi_rgb[ANSI_SLOTS[i]];
        if (nsubs < NSUB && hex >= 0 && tui_rgb_to_oklch((uint32_t)hex).c >= MIN_CHROMA && usable(&c, (uint32_t)hex))
            PUSH((uint32_t)hex);
    }
    if (nsubs < NSUB) {
        /* hues derived from the accent; on light themes they darken until they clear the darkest surface */
        bool light = tui_theme_is_light(th);
        double surf = -1;
        if (light) {
            surf = c.surfaces[0];
            for (int i = 1; i < c.nsurf; i++) surf = MIN(surf, c.surfaces[i]);
        }
        tui_oklch_t hues[DERIVED_CANDIDATES];
        for (int i = 0; i < DERIVED_CANDIDATES; i++)
            hues[i] = tui_rgb_to_oklch(tui_session_accent_at((double)i / DERIVED_CANDIDATES, th, surf));
        double step = light ? -LIGHTNESS_STEP : LIGHTNESS_STEP;
        for (int ring = 0; ring < LIGHTNESS_RINGS && nsubs < NSUB; ring++) {
            uint32_t cand[DERIVED_CANDIDATES];
            int ncand = 0;
            for (int i = 0; i < DERIVED_CANDIDATES; i++) {
                uint32_t hex = tui_oklch_to_rgb((tui_oklch_t){hues[i].l + ring * step, hues[i].c, hues[i].h});
                tui_oklch_t o = tui_rgb_to_oklch(hex);
                if (o.l >= MIN_LIGHTNESS && o.l <= MAX_LIGHTNESS && o.c >= MIN_CHROMA && readable(&c, hex)) cand[ncand++] = hex;
            }
            /* farthest first: each pick is the candidate least like every colour already taken */
            while (nsubs < NSUB) {
                int best = -1;
                double best_d = MIN_DISTANCE;
                for (int i = 0; i < ncand; i++) {
                    double d = distance(&c, cand[i]);
                    if (d >= best_d) best = i, best_d = d;
                }
                if (best < 0) break;
                PUSH(cand[best]);
            }
        }
    }
    for (size_t i = 0; i < ARRAY_LEN(FALLBACK); i++)
        if (nsubs < NSUB && usable(&c, FALLBACK[i])) PUSH(FALLBACK[i]);
#undef PUSH
    /* a theme where no colour clears contrast still needs names told apart: the fallback hues unfiltered */
    if (!nsubs) {
        for (int i = 0; i < NSUB; i++) out[i] = FALLBACK[i + 1];
        return NSUB;
    }
    size_t n = 0;
    int g = generated_from;
    for (int i = 0; i < NSUB; i++) {
        if (declared[i] >= 0) out[n++] = (uint32_t)declared[i];
        else if (g < nsubs) out[n++] = subs[g++];
    }
    return n;
}

/* the palette, resolved once per theme instance */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_pal_valid;
static int64_t g_pal_key[TUI_TC__COUNT + 17]; /* the theme inputs the palette was resolved from */
static uint32_t g_main, g_subs[NSUB];
static size_t g_nsubs;

typedef struct {
    char *id;
    int64_t created;
} arrival_t;
static VEC(char *) g_slots;           /* arrival slot = index; never released */
static VEC(arrival_t) g_pending;      /* registered, not yet given a slot */

static bool slot_of_locked(const char *id, size_t *slot) {
    for (size_t i = 0; i < g_slots.n; i++)
        if (!strcmp(g_slots.v[i], id)) {
            *slot = i;
            return true;
        }
    return false;
}

void tui_agent_note_arrival(const char *id, int64_t created_at) {
    if (!id || !strcmp(id, "Main")) return;
    pthread_mutex_lock(&g_mu);
    size_t s;
    bool known = slot_of_locked(id, &s);
    for (size_t i = 0; i < g_pending.n && !known; i++)
        if (!strcmp(g_pending.v[i].id, id)) {
            g_pending.v[i].created = created_at;
            known = true;
        }
    if (!known) vec_push(&g_pending, ((arrival_t){xstrdup(id), created_at}));
    pthread_mutex_unlock(&g_mu);
}

static int arrival_cmp(const void *a, const void *b) {
    const arrival_t *x = a, *y = b;
    return x->created < y->created ? -1 : x->created > y->created;
}

uint32_t tui_agent_color(const tui_theme_t *th, const char *id) {
    if (!th) th = tui_theme();
    pthread_mutex_lock(&g_mu);
    int64_t key[TUI_TC__COUNT + 17];
    memcpy(key, th->rgb, sizeof th->rgb);
    memcpy(key + TUI_TC__COUNT, th->ansi_rgb, sizeof th->ansi_rgb);
    key[TUI_TC__COUNT + 16] = th->surface_rgb;
    if (!g_pal_valid || memcmp(key, g_pal_key, sizeof key)) {
        g_nsubs = tui_agent_palette(th, &g_main, g_subs);
        memcpy(g_pal_key, key, sizeof key);
        g_pal_valid = true;
    }
    uint32_t r = g_main;
    if (id && strcmp(id, "Main") && g_nsubs) {
        size_t slot;
        if (!slot_of_locked(id, &slot)) {
            if (g_pending.n > 1) xqsort(g_pending.v, g_pending.n, sizeof(arrival_t), arrival_cmp);
            for (size_t i = 0; i < g_pending.n; i++) vec_push(&g_slots, g_pending.v[i].id);
            g_pending.n = 0;
            /* never registered here (a foreign id): it arrives when first drawn */
            if (!slot_of_locked(id, &slot)) {
                vec_push(&g_slots, xstrdup(id));
                slot = g_slots.n - 1;
            }
        }
        r = g_subs[slot % g_nsubs];
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

char *tui_agent_colored(const char *id, const char *text) {
    const tui_theme_t *th = tui_theme();
    char buf[32];
    return xasprintf("%s%s\x1b[39m", tui_rgb_fg(th, tui_agent_color(th, id), buf, sizeof buf), text);
}
