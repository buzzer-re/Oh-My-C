/* auth.h - credential storage (auth.json) + generic OAuth (PKCE code flow / refresh) from catalog descriptors. */
#ifndef OMC_AUTH_H
#define OMC_AUTH_H

#include "core/core.h"
#include "core/json.h"

void auth_init(void);
void auth_reload(void); /* re-read auth.json (another process may have changed it) */
void auth_set_runtime_key(const char *provider, const char *key); /* --api-key override */
/* Resolve a usable key/token for provider (refreshing OAuth if needed). Owned or NULL. */
char *auth_get_api_key(const char *provider);
bool auth_has_credentials(const char *provider);
int auth_set_api_key(const char *provider, const char *key);
int auth_logout(const char *provider);
json_t *auth_list(void); /* owned: [{provider, type, expires?}] */

/* interactive OAuth login; io callbacks let the caller print/prompt. Returns 0 on success, sets *err. */
typedef struct {
    void (*print)(void *ud, const char *msg);
    char *(*prompt)(void *ud, const char *question); /* owned answer or NULL */
    void *ud;
} auth_io_t;
int auth_login(const char *provider, const auth_io_t *io, char **err);
/* OAuth code+PKCE login from a runtime descriptor (same shape as catalog auth descriptors); the
 * credential is stored under `key` together with the descriptor so it can refresh itself. */
int auth_login_descriptor(const char *key, const json_t *descriptor, const auth_io_t *io, char **err);

/* A question the flow asks (OAuthPrompt): message, optional placeholder ("e.g., ..."), empty answers allowed,
 * masked entry. */
typedef struct {
    const char *message;
    const char *placeholder;
    bool allow_empty;
    bool secret;
} auth_prompt_t;

/* TUI-friendly variant: never reads stdin unless read_stdin; reports the authorize / verification URL through
 * on_auth (launch_url: optional local /launch shortcut; instructions: e.g. "Enter code: ABCD-1234" for device
 * flows), polls poll_code (owned pasted code / redirect URL, or NULL), asks questions through prompt (blocks
 * the worker; owned answer, NULL = cancelled) and stops when *abort != 0.
 * Runs on the calling (worker) thread; callbacks are invoked on it. */
typedef struct {
    void (*on_auth)(void *ud, const char *url, const char *launch_url, const char *instructions);
    void (*on_progress)(void *ud, const char *msg);
    char *(*poll_code)(void *ud);
    char *(*prompt)(void *ud, const auth_prompt_t *p);
    volatile int *abort;
    bool read_stdin;
    void *ud;
    const auth_io_t *legacy_io; /* internal: auth_login's console io */
} auth_login_hooks_t;
/* Runs provider's login (oauth-code, device-code, api-key or custom hook) and stores the result under the
 * provider's storeAs id. Returns 0 on success (an empty optional api key stores nothing), -1 and *err. */
int auth_login_hooked(const char *provider, const auth_login_hooks_t *hooks, char **err);
json_t *auth_oauth_providers(void); /* borrowed array of catalog auth descriptors (login providers have `login`) */
/* provider id a login stores its credential under (descriptor `storeAs`, e.g. openai-codex-device -> openai-codex) */
const char *auth_store_id(const char *provider);

#endif
