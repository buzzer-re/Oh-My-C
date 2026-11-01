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
json_t *auth_oauth_providers(void); /* borrowed array of descriptors with login.kind == oauth-code */

#endif
