/* usage.h - provider usage reports (port of omp packages/ai/src/usage.ts, the usage/ fetchers, auth/usage.ts and the shared
 * display helpers of packages/tui/src/overlays/usage-display.ts + prompt/usage-amounts.ts).
 *
 * Reports are json trees with omp's UsageReport shape:
 *   {provider, fetchedAt, limits: [UsageLimit], resetCredits?, notes?: [string], metadata?: {...}, raw?}
 *   UsageLimit {id, label, scope: {provider, accountId?, projectId?, orgId?, modelId?, tier?, windowId?, shared?,
 *               sharedGroup?}, window?: {id, label, durationMs?, resetsAt?, resetLabel?},
 *               amount: {used?, limit?, remaining?, usedFraction?, remainingFraction?, unit}, status?, notes?}
 *
 * One fetcher per provider (anthropic, openai-codex, github-copilot, zai, synthetic, ollama,
 * ollama-cloud, kiro) runs against the credential stored in auth.json (or the provider's runtime / env API
 * key). Every probe goes through OMC_AUTH_URL_REWRITE so tests point them at mock servers. */
#ifndef OMC_USAGE_H
#define OMC_USAGE_H

#include "core/core.h"
#include "core/json.h"

/* ---------- fetchers ---------- */

typedef struct {
    const char *provider;
    const json_t *cred;   /* UsageCredential {type: "api_key"|"oauth", apiKey?, accessToken?, refreshToken?,
                             expiresAt?, accountId?, projectId?, email?, orgId?, orgName?, enterpriseUrl?,
                             apiEndpoint?} */
    const char *base_url; /* the provider's configured base URL, may be NULL */
    int64_t timeout_ms;
    const json_t *previous; /* last report cached for this credential, may be NULL */
} usage_params_t;

typedef struct {
    const char *id;
    bool (*supports)(const usage_params_t *p);
    /* owned report or NULL; *auth_rejected on a definitive 401/403 */
    json_t *(*fetch)(const usage_params_t *p, bool *auth_rejected);
    int64_t failure_backoff_ms; /* 0 = shared 10 s */
} usage_provider_t;

/* usage/kiro.ts parseKiroUsage: the report of a GetUsageLimits payload, or NULL without a readable credit breakdown */
json_t *usage_kiro_parse(const json_t *payload, int64_t fetched_at);

/* built-in fetcher for a provider id, NULL when the provider has no usage endpoint */
const usage_provider_t *usage_provider_for(const char *provider);
size_t usage_provider_count(void);
const usage_provider_t *usage_provider_at(size_t i); /* DEFAULT_USAGE_PROVIDERS probe order */

/* ---------- reports ---------- */

/* Fetch every account's report (cached 5 min per credential, 10 s failure backoff, 10 s request timeout,
 * parallel probes). provider may be NULL. Owned array (never NULL). */
json_t *usage_fetch_reports(const char *provider);
/* drop cached reports (all providers when NULL); the next fetch probes upstream */
void usage_invalidate(const char *provider);
/* newest snapshot from usage_fetch_reports without a network call (owned clone), NULL before the first fetch */
json_t *usage_cached_reports(void);
/* usageCacheIdentity of a UsageCredential (owned) */
char *usage_account_key(const json_t *cred);
/* recorded usage-limit snapshots (usage-history.jsonl, one per account / limit / hour), oldest first; provider may
 * be NULL. Owned [{recordedAt, provider, accountKey, email?, accountId?, limitId, label, windowLabel?,
 * usedFraction?, status?, resetsAt?}] */
json_t *usage_history(const char *provider, int64_t since_ms);
/* test seam: forget everything */
void usage_reset_cache(void);

/* stored accounts of providers with a usage endpoint (selectReportableAccounts over collectStoredAccounts):
 * owned [{provider, type: "oauth"|"api_key", email?, accountId?, projectId?, orgId?, orgName?, enterpriseUrl?,
 * credentialId}]; provider may be NULL */
json_t *usage_stored_accounts(const char *provider);
/* the accounts of `accounts` no report in `reports` is attributable to (collectUnreportedAccounts), owned */
json_t *usage_unreported_accounts(const json_t *reports, const json_t *accounts);
/* accountIdentityLabel: email, accountId, projectId, enterpriseUrl, else "<type> account" (owned) */
char *usage_account_label(const json_t *account);

/* the session's OAuth identity for a provider ({email?, accountId?, projectId?, orgId?, orgName?}, owned) or NULL */
json_t *usage_active_identity(const char *provider);
bool usage_limit_matches_active(const json_t *report, const json_t *limit, const json_t *identity);
bool usage_report_matches_active(const json_t *report, const json_t *identity);
/* formatCodexUsageReportLabel(report, peers, base, undefined, include_plan, "parenthesized"), owned */
char *usage_codex_label(const json_t *report, const json_t *peers, const char *base, bool include_plan);

/* ---------- shared display helpers ---------- */

/* resolveUsedFraction; false when unknown */
bool usage_used_fraction(const json_t *limit, double *out);
/* usageStatus thresholds: exhausted >= 1, warning >= 0.9, ok, "unknown" when !has */
const char *usage_status_for(bool has, double fraction);
/* formatLimitTitle: label plus " (tier)" unless the label names the tier (owned) */
char *usage_limit_title(const json_t *limit);
/* collapseSharedUsageReports (owned deep copy) */
json_t *usage_collapse_shared(const json_t *reports);
/* summarizeUsageResetCredits: false when the report has no reset block */
typedef struct {
    int64_t banked, redeemable;
    char *soonest_expiry; /* owned ISO string or NULL */
    char *unavailable_reason; /* owned or NULL */
} usage_reset_summary_t;
bool usage_reset_summary(const json_t *reset_credits, int64_t now, usage_reset_summary_t *out);
void usage_reset_summary_free(usage_reset_summary_t *s);
/* formatUsageResetWindow: "anthropic:5h" -> "Claude 5h" ... (static or the id itself) */
const char *usage_reset_window_name(const char *window_id);
/* usage-amounts.ts */
bool usage_used_only_amount(const json_t *limit);
bool usage_remaining_only_amount(const json_t *limit);
char *usage_remaining_only_total(const json_t *const *limits, size_t n);  /* "100 credits left" or NULL */
char *usage_absolute_only_amount(const json_t *const *limits, size_t n);  /* formatAbsoluteOnlyAmount or NULL */

/* formatProviderName: "openai-codex" -> "Openai Codex" (owned) */
char *usage_provider_name(const char *provider);
/* pi-utils formatDuration (owned) */
char *usage_fmt_duration(double ms);
/* chrome/format.ts formatCoarseDuration (owned) */
char *usage_fmt_coarse(double ms);
/* pi-utils formatNumber(n, maxFractionDigits; < 0 = none) (owned) */
char *usage_fmt_number(double n, int max_frac);
/* Intl.NumberFormat("en-US", {maximumFractionDigits}) with thousands separators (owned) */
char *usage_fmt_grouped(double n, int max_frac);
/* Intl compact notation, maximumFractionDigits 1: 5600 -> "5.6K" (owned) */
char *usage_fmt_compact(double n);
/* sanitizeText + line breaks / tabs folded to one space (owned) */
char *usage_one_line(const char *s);

/* `/usage` text outside the TUI (slash-commands/helpers/usage-report.ts renderUsageReports), owned */
char *usage_report_text(const json_t *reports, int64_t now, const char *active_provider, const json_t *active_identity,
                        const json_t *selectors);
/* getUsageReportingModelSelectors: sorted "provider/model" of the available models the reports map to a
 * quantitative usage scope (owned array) */
json_t *usage_model_selectors(const json_t *reports);
/* the fallback when no provider reported (session-manager tallies): stats {input, output, cacheRead, cacheWrite,
 * totalTokens, premiumRequests, cost} */
char *usage_session_text(const json_t *stats);

/* ---------- saved rate-limit resets (Codex / Claude) ---------- */

/* listResetCredits: owned [{provider, credentialId, email?, accountId?, orgId?, orgName?, active, availableCount,
 * redeemableCount?, nextCreditId?, credits: [], eligible?, reason?, cooldownUntil?, error?}] for every stored
 * Codex / Claude OAuth account; active_provider marks the session's account */
json_t *usage_list_reset_credits(const char *active_provider);
/* redeem one saved reset; target {provider, credentialId, creditId?}; owned outcome {ok, code, provider, reason?,
 * cleared?} */
json_t *usage_redeem_reset_credit(const json_t *target);
/* reset-usage.ts helpers */
json_t *usage_reset_accounts(const json_t *statuses);          /* toResetUsageAccounts (sorted rows), owned */
char *usage_reset_account_line(const json_t *account);         /* formatResetUsageAccountLine, owned */
/* resolveResetUsageTarget: the account (borrowed from accounts) or NULL with *err (owned) */
const json_t *usage_resolve_reset_target(const json_t *accounts, const char *arg, const char *command, char **err);
char *usage_describe_redeem(const json_t *outcome, const char *label); /* describeRedeemOutcome, owned */
const char *usage_reset_provider_label(const char *provider);    /* "Codex" / "Claude" / provider */
/* listResetCredits for one provider ("openai-codex" / "anthropic"), owned */
json_t *usage_list_reset_credits_for(const char *provider, const char *active_provider);
/* claude-reset.ts (usage_providers.c): listClaudeResetCredits -> owned {availableCount, redeemableCount,
 * nextCreditId?, eligible, reason?, cooldownUntil?, credits, orgId?, baseUrl?} or NULL when discovery failed;
 * consumeClaudeResetCredit -> owned {ok, code, status, cleared?, reason?} */
json_t *usage_claude_list_resets(const char *token, const char *org_id, const char *base_url, bool *rate_limited);
json_t *usage_claude_consume_reset(const char *token, const char *org_id, const char *base_url, const json_t *credit);
/* openai-codex-reset.ts / openai-codex-base-url.ts (usage_providers.c) */
json_t *usage_codex_list_resets(const char *token, const char *acc, const char *base_url, int64_t timeout_ms);
json_t *usage_codex_consume_reset(const char *token, const char *acc, const char *base_url, const char *credit_id);
char *usage_codex_base_url(const char *base);

#endif
