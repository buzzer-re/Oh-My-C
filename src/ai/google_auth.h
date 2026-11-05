/*
 * google_auth.h - Google Application Default Credentials for Vertex AI (port of providers/google-auth.ts).
 *
 * Sources, in priority order:
 *   0. GOOGLE_CLOUD_ACCESS_TOKEN / CLOUDSDK_AUTH_ACCESS_TOKEN (explicit bearer, never cached)
 *   1. GOOGLE_APPLICATION_CREDENTIALS -> service_account (RS256 JWT bearer grant), authorized_user (refresh token),
 *      external_account_authorized_user (workforce: refresh at the file's token_url with Basic client auth, rotated
 *      refresh tokens kept in memory) or impersonated_service_account (source credential -> iamcredentials
 *      generateAccessToken)
 *   2. gcloud user ADC: ~/.config/gcloud/application_default_credentials.json
 *   3. GCE / Cloud Run metadata server (GCE_METADATA_HOST overrides metadata.google.internal)
 * Tokens are cached process-wide and refreshed GOOGLE_VERTEX_REFRESH_SKEW_MS (default 60s) before expiry;
 * concurrent callers share one in-flight resolution.
 */
#ifndef OMC_GOOGLE_AUTH_H
#define OMC_GOOGLE_AUTH_H

#include "core/core.h"
#include "core/json.h"

#define GOOGLE_OAUTH_TOKEN_URL "https://oauth2.googleapis.com/token"
#define GOOGLE_CLOUD_PLATFORM_SCOPE "https://www.googleapis.com/auth/cloud-platform"

/* RS256 JWT `header.claims.signature` (header {"alg":"RS256","typ":"JWT"[,"kid"]}); PEM is PKCS#8 or PKCS#1.
 * Owned, or NULL with *err set. */
char *google_jwt_rs256(const json_t *claims, const char *private_key_pem, const char *key_id, char **err);

/* Bearer access token for Vertex AI calls. Owned, or NULL with *err set (owned). */
char *google_vertex_access_token(char **err);

/* gcloud's user ADC path (owned). */
char *google_user_adc_path(void);
/* GOOGLE_APPLICATION_CREDENTIALS (or the user ADC file) exists: the `google-vertex-adc` env hook's file test. */
bool google_adc_file_exists(void);
/* Vertex is usable: GOOGLE_CLOUD_API_KEY, or an ADC file plus project and location env vars. */
bool google_vertex_has_credentials(void);

/* Test seam: forget cached tokens. */
void google_auth_reset_cache(void);

#endif
