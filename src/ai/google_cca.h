/*
 * google_cca.h - Google Cloud Code Assist (Gemini CLI / Antigravity) client identity and project provisioning.
 * Ports catalog/src/wire/gemini-headers.ts, utils/google-validation.ts and the project-discovery hooks of
 * registry/oauth/google-gemini-cli.ts / google-antigravity.ts.
 */
#ifndef OMC_GOOGLE_CCA_H
#define OMC_GOOGLE_CCA_H

#include "core/core.h"
#include "core/json.h"
#include "net/http.h"

#define CCA_GEMINI_CLI_ENDPOINT "https://cloudcode-pa.googleapis.com"
#define CCA_ANTIGRAVITY_DAILY_ENDPOINT "https://daily-cloudcode-pa.googleapis.com"
#define CCA_ANTIGRAVITY_SANDBOX_ENDPOINT "https://daily-cloudcode-pa.sandbox.googleapis.com"

/* `GeminiCLI/<PI_AI_GEMINI_CLI_VERSION|0.46.0>/<model> (<platform>; <arch>; terminal)` (owned) */
char *cca_gemini_cli_user_agent(const char *model_id);
/* User-Agent + Client-Metadata */
void cca_gemini_cli_headers(http_headers_t *h, const char *model_id);

/* Antigravity client version: PI_AI_ANTIGRAVITY_VERSION > manifest-discovered > 2.26.0 (owned). */
char *cca_antigravity_version(void);
/* Resolve the latest Antigravity release once per process (5s timeout; failures retried after 10 min). */
void cca_antigravity_ensure_version(void);
/* Parses an electron-builder manifest `version:` line; owned or NULL. */
char *cca_parse_manifest_version(const char *yaml);
/* `antigravity/hub/<ver> (aidev_client; os_type=<os>; arch=<arch>; cl=<cl>)` (owned) */
char *cca_antigravity_user_agent(void);
/* User-Agent + Accept-Encoding: gzip */
void cca_antigravity_headers(http_headers_t *h);

/* VALIDATION_REQUIRED error body -> validation_url (owned or NULL). */
char *cca_extract_validation_url(const char *error_body);
/* "Account verification required[ for <email>]. Visit <url> to continue, then <next>." (owned) */
char *cca_validation_message(const char *url, const char *next_action, const char *email);

typedef void (*cca_progress_fn)(void *ud, const char *msg);

/* Post-login project discovery/onboarding for "google-gemini-cli" or "google-antigravity".
 * endpoint NULL => the provider's production control plane. Returns 0 and *project_out (owned), or -1 and *err. */
int cca_discover_project(const char *provider, const char *access_token, const char *endpoint, cca_progress_fn progress,
                         void *ud, char **project_out, char **err);

#endif
