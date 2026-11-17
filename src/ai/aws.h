/*
 * aws.h - AWS plumbing for the Bedrock provider (port of omp aws-sigv4.ts, aws-eventstream.ts,
 * aws-credentials.ts, utils/aws-profile.ts and registry/aws.ts).
 *
 *  - SigV4 header signing (HMAC-SHA256 via libcrypto), matching @smithy/signature-v4 for header auth
 *    with a full payload hash.
 *  - application/vnd.amazon.eventstream framing (prelude/message CRC32 via zlib).
 *  - Shared config/credentials INI files, profile + region resolution.
 *  - Credential chain: env keys, web identity, profile (static / SSO / credential_process / role_arn
 *    chains), ECS container endpoint, EC2 IMDSv2. Cached process-wide, refreshed 60s before expiry.
 */
#ifndef OMC_AWS_H
#define OMC_AWS_H

#include "core/core.h"
#include "core/json.h"
#include "net/http.h"

#include <signal.h>

typedef struct {
    char *access_key_id;
    char *secret_access_key;
    char *session_token; /* NULL when absent */
    int64_t expires_at;  /* wall-clock ms; 0 => never expires */
} aws_creds_t;

void aws_creds_free(aws_creds_t *c);
void aws_creds_copy(aws_creds_t *dst, const aws_creds_t *src);

/* ---------- SigV4 ---------- */

char *aws_sha256_hex(const void *data, size_t len); /* owned lowercase hex */
/* kSecret -> kDate -> kRegion -> kService -> kSigning */
void aws_signing_key(const char *secret, const char *short_date, const char *region, const char *service,
                     unsigned char out[32]);
/* "YYYYMMDDTHHMMSSZ" for a wall-clock ms timestamp (buf >= 17). */
void aws_amz_date(int64_t ms, char buf[17]);
/* RFC 3986 per-segment path canonicalization ('/' kept). Owned. */
char *aws_canonical_path(const char *path);
/* Encoded-then-sorted query canonicalization. Owned ("" for NULL/empty). */
char *aws_canonical_query(const char *query);

/*
 * Low-level signer: signs exactly `headers` (names lowercased, values trimmed + whitespace-collapsed,
 * sorted). Returns the Authorization header value (owned). The canonical request and string to sign
 * are returned when the out-pointers are non-NULL (owned).
 */
char *aws_sigv4_authorization(const char *method, const char *path, const char *query, const http_headers_t *headers,
                              const char *payload_hash, const char *amz_date, const char *region,
                              const char *service, const char *access_key_id, const char *secret,
                              char **canonical_request, char **string_to_sign);

typedef struct {
    const char *method;
    const char *host;  /* host[:port] as sent in the Host header */
    const char *path;  /* URI path, e.g. /model/<id>/converse-stream */
    const char *query; /* without '?', may be NULL */
    const void *body;
    size_t body_len;
    const char *region;
    const char *service;
    const aws_creds_t *creds;
    const http_headers_t *headers; /* extra headers to sign (unsignable ones skipped) */
    int64_t time_ms;               /* 0 => now */
} aws_sign_params_t;

/* signRequest: appends host, x-amz-date, x-amz-content-sha256, [x-amz-security-token], authorization to `out`. */
void aws_sigv4_sign(const aws_sign_params_t *p, http_headers_t *out);

/* ---------- event stream ---------- */

#define AWS_ES_MAX_MESSAGE (24u * 1024u * 1024u)
#define AWS_ES_MAX_HEADERS (128u * 1024u) /* encoded header-block limit of the Amazon Event Stream specification */

typedef struct {
    http_headers_t headers;        /* all typed values surfaced as strings */
    const unsigned char *payload;  /* borrowed from the frame */
    size_t payload_len;
} aws_es_msg_t;

uint32_t aws_crc32(const void *data, size_t len);
/* Decode one fully buffered message. 0 ok (free with aws_es_msg_free); -1 with *err (owned). */
int aws_es_decode_message(const unsigned char *frame, size_t len, aws_es_msg_t *out, char **err);
void aws_es_msg_free(aws_es_msg_t *m);
/* Encode a message with string-typed headers (tests / mocks). Owned buffer. */
unsigned char *aws_es_encode(const char *const *hdr_names, const char *const *hdr_values, size_t nhdr,
                             const void *payload, size_t payload_len, size_t *out_len);

/* Streaming decoder: frames may span chunks, chunks may carry many frames. */
typedef int (*aws_es_cb)(void *ud, const aws_es_msg_t *m);
typedef struct {
    sb_t buf;
    aws_es_cb cb;
    void *ud;
    char *error; /* framing error (owned) */
} aws_es_decoder_t;

void aws_es_init(aws_es_decoder_t *d, aws_es_cb cb, void *ud);
/* Returns 0, the callback's non-zero return, or -1 on a framing error (d->error set). */
int aws_es_feed(aws_es_decoder_t *d, const void *data, size_t len);
/* -1 when a partial message remains (truncated stream). */
int aws_es_finish(aws_es_decoder_t *d);
void aws_es_free(aws_es_decoder_t *d);

/* ---------- shared config / profile ---------- */

/* INI -> {section: {key: value}}; `[profile x]` -> "x", `[sso-session y]` -> "sso-session:y". Owned. */
json_t *aws_ini_parse(const char *text);
const char *aws_profile_name(const char *profile); /* profile || AWS_PROFILE || "default" (borrowed) */
bool aws_should_load_shared_config(const char *profile);
char *aws_credentials_path(void); /* AWS_SHARED_CREDENTIALS_FILE || ~/.aws/credentials (owned) */
char *aws_config_path(void);      /* AWS_CONFIG_FILE || ~/.aws/config (owned) */
char *aws_profile_region(const char *profile);   /* owned or NULL */
char *aws_ambient_region(const char *profile);   /* AWS_REGION || AWS_DEFAULT_REGION || profile region */
char *aws_resolve_region(const char *explicit_region, const char *profile); /* ... || "us-east-1" */
bool aws_has_configured_profile(const char *profile);

/* hasAwsCredentialSource: cheap probe used for "has credentials" marks. */
bool aws_has_credential_source(void);
/* resolveAwsBearerToken: api key (unless the auth marker) || AWS_BEARER_TOKEN_BEDROCK. Borrowed or NULL. */
const char *aws_bearer_token(const char *api_key);

/* ---------- credential chain ---------- */

/* 0 ok (out filled, owned); -1 with *err (owned). */
int aws_resolve_credentials(const char *profile, const char *region, volatile sig_atomic_t *cancel, aws_creds_t *out,
                            char **err);
void aws_invalidate_credentials(const char *profile, const char *region);
void aws_clear_credential_cache(void);

/* Exposed for tests. */
strvec_t aws_tokenize_command(const char *cmd, char **err); /* POSIX credential_process tokenizer */
int64_t aws_parse_iso8601(const char *s);                  /* ms, or -1 */
char *aws_xml_tag(const char *xml, const char *tag);       /* owned, entity-decoded, NULL if missing/empty */

#endif
