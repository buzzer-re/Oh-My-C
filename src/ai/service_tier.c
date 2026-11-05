/* service_tier.c - service-tier families and wire gating (port of omp pi-ai types.ts serviceTierFamily,
 * resolveModelServiceTier, shouldSendServiceTier, realizesPriorityServiceTier). */
#include "ai.h"

#include <ctype.h>
#include <strings.h>

/* the catalog's identity class; custom models.yml rows carry none, so their id is classified like omp's
 * classifyModel does for the families that matter here (gpt-* / o<N> / codex, claude, gemini) */
static const char *identity_class(const ai_model_t *m) {
    if (!m) return NULL;
    const char *c = m->raw ? json_get_str(json_get_obj(m->raw, "identity"), "class") : NULL;
    if (c || !m->id) return c;
    const char *id = strrchr(m->id, '/');
    id = id ? id + 1 : m->id;
    if (!strncasecmp(id, "gpt-", 4) || !strncasecmp(id, "codex", 5) ||
        ((id[0] == 'o' || id[0] == 'O') && isdigit((unsigned char)id[1])))
        return "openai";
    if (!strncasecmp(id, "claude", 6)) return "anthropic";
    if (!strncasecmp(id, "gemini", 6)) return "gemini";
    return NULL;
}

static bool openai_tier_api(const char *api) {
    return str_eq(api, "openai-completions") || str_eq(api, "openai-responses") || str_eq(api, "openai-codex-responses");
}

/* Fireworks has its own priority-only control; GitHub Copilot rejects OpenAI's `service_tier` field */
static bool openai_tier_model(const ai_model_t *m) {
    return m && !str_eq(m->provider, "fireworks") && !str_eq(m->provider, "github-copilot") && openai_tier_api(m->api) &&
           str_eq(identity_class(m), "openai");
}

const char *ai_service_tier_family(const ai_model_t *m) {
    if (!m) return NULL;
    const char *cls = identity_class(m);
    if (str_eq(m->provider, "openrouter")) {
        if (str_eq(cls, "anthropic")) return "anthropic";
        if (str_eq(cls, "gemini")) return "google";
        if (str_eq(cls, "openai")) return "openai";
        return NULL;
    }
    if (str_eq(m->provider, "openai") || str_eq(m->provider, "openai-codex")) return "openai";
    if (str_eq(m->api, "anthropic-messages")) return "anthropic";
    if (str_eq(m->provider, "google") || str_eq(m->provider, "google-vertex")) return "google";
    if (openai_tier_model(m)) return "openai";
    return NULL;
}

bool ai_service_tier_for_family(const char *family, const char *tier) {
    if (str_empty(tier) || str_eq(tier, "none")) return false;
    if (str_eq(family, "openai"))
        return str_eq(tier, "auto") || str_eq(tier, "default") || str_eq(tier, "flex") || str_eq(tier, "scale") ||
               str_eq(tier, "priority") || str_eq(tier, "ultrafast");
    if (str_eq(family, "anthropic")) return str_eq(tier, "priority");
    if (str_eq(family, "google")) return str_eq(tier, "flex") || str_eq(tier, "priority");
    return false;
}

static bool advertised(const ai_model_t *m, const char *tier, size_t *count) {
    json_t *adv = json_get_arr(m->raw, "serviceTiers"), *e;
    bool listed = false;
    json_arr_foreach(adv, i, e) {
        if (str_eq(json_sv(e), tier)) listed = true;
    }
    *count = json_len(adv);
    return listed;
}

bool ai_should_send_service_tier_for(const char *tier, const char *provider, const ai_model_t *m) {
    if (str_empty(tier) || str_eq(tier, "auto")) return false;
    if (m) provider = m->provider;
    if (m && str_eq(m->api, "openai-codex-responses") && !str_eq(tier, "flex") && !str_eq(tier, "default")) {
        size_t n = 0;
        bool listed = advertised(m, tier, &n);
        if (str_eq(tier, "ultrafast")) return listed;
        if (n > 0) return listed;
    }
    if (str_eq(tier, "ultrafast")) {
        if (!m) return str_eq(provider, "openai") || str_eq(provider, "openai-codex");
        if (str_eq(provider, "openai")) return true;
        /* an OpenAI-family relay gets it only when its service_tiers lists it */
        size_t n = 0;
        return openai_tier_model(m) && advertised(m, tier, &n);
    }
    if (str_eq(provider, "openai") || str_eq(provider, "openai-codex")) return true;
    if (str_eq(provider, "openrouter")) return str_eq(tier, "flex") || str_eq(tier, "scale") || str_eq(tier, "priority");
    if (m && openai_tier_model(m)) return true;
    if (str_eq(provider, "google")) return str_eq(tier, "flex") || str_eq(tier, "priority");
    /* Vertex realizes only priority (via header); flex has no documented control */
    if (str_eq(provider, "google-vertex") || str_eq(provider, "fireworks")) return str_eq(tier, "priority");
    return false;
}

bool ai_should_send_service_tier(const char *tier, const ai_model_t *m) {
    return ai_should_send_service_tier_for(tier, m ? m->provider : NULL, m);
}

bool ai_realizes_priority_tier(const char *tier, const ai_model_t *m) {
    if (!str_eq(tier, "priority") || !m) return false;
    if (str_eq(m->provider, "anthropic")) return true;
    if (str_eq(m->provider, "openrouter")) {
        const char *f = ai_service_tier_family(m);
        return str_eq(f, "openai") || str_eq(f, "google");
    }
    if (str_eq(m->api, "anthropic-messages")) return false;
    return ai_should_send_service_tier(tier, m);
}

const char *ai_resolve_model_service_tier(const ai_service_tiers_t *tiers, const ai_model_t *m) {
    if (!tiers || !m) return NULL;
    if (str_eq(m->provider, "fireworks")) return tiers->fireworks && !ai_fireworks_fast_model_id(m->id) ? tiers->fireworks : NULL;
    const char *f = ai_service_tier_family(m);
    if (str_eq(f, "openai")) return tiers->openai;
    if (str_eq(f, "anthropic")) return tiers->anthropic;
    if (str_eq(f, "google")) return tiers->google;
    return NULL;
}

/* interned tier names so a map can hold borrowed static strings */
const char *ai_service_tier_intern(const char *tier) {
    static const char *const T[] = {"auto", "default", "flex", "scale", "priority", "ultrafast"};
    for (size_t i = 0; i < ARRAY_LEN(T); i++)
        if (str_eq(tier, T[i])) return T[i];
    return NULL;
}

/* Fireworks `-fast` model ids take the Fast serving path (pi-catalog fireworks-model-id.ts isFireworksFastModelId) */
bool ai_fireworks_fast_model_id(const char *id) {
    if (!id) return false;
    size_t n = strlen(id);
    return n >= 5 && !strcmp(id + n - 5, "-fast");
}
