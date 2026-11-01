/* auth.h - credential storage (auth.json) + generic OAuth (PKCE code flow / refresh) from catalog descriptors. */
#ifndef OMC_AUTH_H
#define OMC_AUTH_H

#include "core/core.h"
#include "core/json.h"

void auth_init(void);
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

/* TUI-friendly variant: never reads stdin unless read_stdin; reports the authorize URL through
 * on_auth (launch_url: optional local /launch shortcut), polls poll_code (owned pasted code / redirect URL, or NULL) and stops when *abort != 0.
 * Runs on the calling (worker) thread; callbacks are invoked on it. */
typedef struct {
    void (*on_auth)(void *ud, const char *url, const char *launch_url, const char *instructions);
    void (*on_progress)(void *ud, const char *msg);
    char *(*poll_code)(void *ud);
    volatile int *abort;
    bool read_stdin;
    void *ud;
    const auth_io_t *legacy_io; /* internal: auth_login's console io */
} auth_login_hooks_t;
int auth_login_hooked(const char *provider, const auth_login_hooks_t *hooks, char **err);
json_t *auth_oauth_providers(void); /* borrowed array of descriptors with login.kind == oauth-code */
/* Store an OAuth credential ({type:"oauth", access, refresh, expires, ...}, owned) for provider. */
int auth_store_credential(const char *provider, json_t *cred);

/* Cursor custom login hook (auth_cursor.c): browser login polled with a PKCE verifier, and token refresh. */
/* on_msg prints a line (URL block, progress); on_url (optional) receives the bare login URL; *abort stops polling. */
int auth_cursor_login(void (*on_msg)(void *ud, const char *msg), void (*on_url)(void *ud, const char *url),
                      volatile int *abort, void *ud, json_t **cred_out, char **err);
json_t *auth_cursor_refresh(const json_t *entry, char **err); /* owned new credential, NULL + *err on failure */

#endif
