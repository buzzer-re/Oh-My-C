/* auth_engine.h - internal to src/config: the declarative login machinery of omp's registry
 * (packages/ai/src/registry/engine/{common,device-code,api-key,refresh}.ts) and its named hooks
 * (registry/hooks/ and registry/oauth/). auth.c owns storage and the oauth-code callback flow. */
#ifndef OMC_AUTH_ENGINE_H
#define OMC_AUTH_ENGINE_H

#include "config/auth.h"
#include "net/http.h"

#define AUTH_NEVER_EXPIRES 8640000000000000LL /* omp NEVER_EXPIRES (8.64e15 ms) */

/* one running login: the rule's provider id/display name/descriptor and the caller's callbacks */
typedef struct {
    const char *provider;
    const char *label;
    const json_t *d;
    const auth_login_hooks_t *h;
} actx_t;

/* outcome of a login flow: an API key string ("" = nothing to store) or an OAuth credential object */
typedef struct {
    char *api_key;
    json_t *cred;
} alogin_t;

bool actx_aborted(const actx_t *c);
void actx_progress(const actx_t *c, const char *msg);
void actx_auth(const actx_t *c, const char *url, const char *launch, const char *instructions);
/* owned answer; NULL with *err = "Login cancelled" when cancelled */
char *actx_prompt(const actx_t *c, const char *message, const char *placeholder, bool allow_empty, char **err);
char *actx_prompt_ex(const actx_t *c, const auth_prompt_t *p, char **err); /* with the masked-entry flag */
bool actx_sleep(const actx_t *c, int64_t ms); /* false when aborted */

/* HTTP for auth flows: OMC_AUTH_URL_REWRITE ("from=to,from=to", prefix match; test-only) applies to every URL */
char *auth_rewrite_url(const char *url);
int auth_http(const char *method, const char *url, const http_headers_t *h, const char *body, int64_t timeout_ms,
              const actx_t *c, http_resp_t *r);

/* registry/engine/common.ts */
char *auth_resolve_value(const json_t *v, const actx_t *c, char **err); /* env overrides, hook, (base64) literal */
char *auth_template(const char *text, const json_t *vars);              /* {name} -> vars[name] or "" */
json_t *auth_jwt_payload(const char *token);                            /* owned object or NULL */
/* POST a declared token-style request; *body_out is the parsed JSON (or a string node for non-JSON text).
 * kind: "token-exchange", "token-refresh" or "device-auth" (device-auth does not fail on HTTP errors). */
int auth_post_token(const json_t *req, const json_t *std_params, const json_t *vars, const http_headers_t *extra,
                    const char *provider, const char *kind, const actx_t *c, int *status, json_t **body_out, char **err);
json_t *auth_map_credentials(const json_t *map, const json_t *body, const char *provider, const json_t *prev, char **err);
void auth_apply_userinfo(const json_t *ui, json_t *cred, const http_headers_t *extra, const json_t *vars, const actx_t *c);
int auth_headers_hook(const char *name, http_headers_t *h, char **err);
/* after-exchange / after-refresh hook; replaces *cred. phase "login" or "refresh"; stored = refreshed row. */
int auth_after_exchange(const char *hook, const actx_t *c, const char *phase, json_t **cred, const json_t *raw,
                        const json_t *stored, char **err);

/* engines */
int auth_device_code_login(const actx_t *c, json_t **cred, char **err);
int auth_api_key_login(const actx_t *c, char **key, char **err);
/* refresh a stored OAuth credential per the provider's refresh rule; *out owned (identity merged by caller) */
int auth_refresh_credential(const char *provider, const json_t *d, const json_t *stored, json_t **out, char **err);

/* api-key-validation.ts probes (15 s timeout); 0 = accepted, else *err and *status (HTTP status, 0 transport) */
int auth_validate_chat(const char *label, const char *key, const char *base_url, const char *model,
                       const char *max_tokens_field, int max_tokens, bool tolerate_model_denied, const actx_t *c,
                       int *status, char **err);
int auth_validate_anthropic(const char *label, const char *key, const char *base_url, const char *model,
                            const actx_t *c, int *status, char **err);
int auth_validate_models(const char *label, const char *key, const char *url, const http_headers_t *extra,
                         const actx_t *c, int *status, char **err);

/* auth.c: the oauth-code callback flow for a descriptor's `login` object (also used by custom hooks) */
int auth_oauth_code_flow(const actx_t *c, const json_t *login, json_t **cred, char **err);
/* auth_custom.c: Windsurf's code exchange (the flow's `exchangeHook`) */
char *auth_windsurf_exchange(const actx_t *c, const char *code, const char *verifier, char **err);

/* auth_custom.c */
int auth_custom_hook_login(const actx_t *c, const char *hook, alogin_t *out, char **err);
/* auth_kiro.c: the kiro-login / kiro-refresh hooks */
int auth_kiro_login(const actx_t *c, alogin_t *out, char **err);
int auth_kiro_refresh(const json_t *current, json_t **out, char **err);
int auth_custom_hook_refresh(const char *hook, const char *provider, const json_t *stored, json_t **out, char **err);

#endif
