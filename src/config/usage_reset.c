/* usage_reset.c - saved rate-limit resets (ports of omp packages/ai/src/auth/resets.ts for OpenAI Codex and Claude,
 * and coding-agent slash-commands/helpers/reset-usage.ts). omc keeps one auth.json credential per provider; its
 * credentialId is the entry's 1-based position in auth.json. */
#define _GNU_SOURCE
#include "usage.h"

#include "ai/ai.h"
#include "config/auth.h"
#include "session/trace.h"

#include <math.h>

static const char *provider_base_url(const char *provider) {
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (str_eq(m->provider, provider) && m->base_url && *m->base_url) return m->base_url;
    }
    return NULL;
}

/* the stored OAuth account of provider: its credentialId, 0 when none is stored */
static int64_t stored_oauth_id(const char *provider) {
    json_t *all = auth_list(), *e;
    int64_t id = 0, found = 0;
    json_arr_foreach(all, i, e) {
        id++;
        if (str_eq(json_get_str(e, "provider"), provider) && str_eq(json_get_str(e, "type"), "oauth")) {
            found = id;
            break;
        }
    }
    json_free(all);
    return found;
}

static json_t *list_account(const char *provider, const char *active_provider) {
    int64_t cid = stored_oauth_id(provider);
    if (!cid) return NULL;
    json_t *cred = auth_get_credential(provider);
    json_t *st = json_obj();
    json_set_int(st, "credentialId", cid);
    static const char *const keys[] = {"accountId", "email", "orgId", "orgName"};
    for (size_t k = 0; k < ARRAY_LEN(keys); k++)
        if (json_get_str(cred, keys[k]) && *json_get_str(cred, keys[k])) json_set_str(st, keys[k], json_get_str(cred, keys[k]));
    json_set_bool(st, "active", str_eq(active_provider, provider));
    json_set_str(st, "provider", provider);
    const char *token = json_get_str(cred, "access");
    bool claude = str_eq(provider, "anthropic");
    json_t *list = !token ? NULL
                   : claude ? usage_claude_list_resets(token, json_get_str(cred, "orgId"), provider_base_url(provider), NULL)
                            : usage_codex_list_resets(token, json_get_str(cred, "accountId"), provider_base_url(provider), 10000);
    if (!token || !list) {
        json_set_int(st, "availableCount", 0);
        json_set(st, "credits", json_arr());
        json_set_str(st, "error", !token ? "Account no longer available" : "Failed to load saved resets");
    } else {
        const char *k;
        json_t *v;
        json_obj_foreach(list, i, k, v) if (strcmp(k, "baseUrl") && strcmp(k, "report")) json_set(st, k, json_clone(v));
    }
    json_free(list);
    json_free(cred);
    return st;
}

json_t *usage_list_reset_credits(const char *active_provider) {
    json_t *out = json_arr(), *st;
    if ((st = list_account("openai-codex", active_provider))) json_push(out, st);
    if ((st = list_account("anthropic", active_provider))) json_push(out, st);
    return out;
}

json_t *usage_list_reset_credits_for(const char *provider, const char *active_provider) {
    json_t *out = json_arr(), *st;
    if ((st = list_account(provider, active_provider))) json_push(out, st);
    return out;
}

/* pickSoonestExpiringCredit */
static const json_t *soonest_credit(const json_t *credits) {
    const json_t *best = NULL, *undated = NULL, *c;
    double best_exp = INFINITY;
    json_arr_foreach(credits, i, c) {
        const char *s = json_get_str(c, "status");
        if (s && strcmp(s, "available")) continue;
        double e;
        const char *x = json_get_str(c, "expiresAt");
        if (!x || !trace_parse_iso_ms(x, &e)) {
            if (!undated) undated = c;
            continue;
        }
        if (e < best_exp) best = c, best_exp = e;
    }
    return best ? best : undated ? undated : json_at(credits, 0);
}

static json_t *outcome(json_t *out, bool ok, const char *code) {
    json_set_bool(out, "ok", ok);
    json_set_str(out, "code", code);
    return out;
}

json_t *usage_redeem_reset_credit(const json_t *target) {
    const char *provider = json_get_str(target, "provider");
    json_t *out = json_obj();
    json_set_str(out, "provider", provider ? provider : "");
    bool claude = str_eq(provider, "anthropic");
    if (!claude && !str_eq(provider, "openai-codex")) return outcome(out, false, "unsupported_provider");
    int64_t cid = stored_oauth_id(provider);
    if (!cid || cid != json_get_int(target, "credentialId", -1)) return outcome(out, false, "no_account");
    json_t *cred = auth_get_credential(provider);
    const char *token = json_get_str(cred, "access"), *acc = json_get_str(cred, "accountId");
    if (acc) json_set_str(out, "accountId", acc);
    if (json_get_str(cred, "email")) json_set_str(out, "email", json_get_str(cred, "email"));
    if (json_get_str(cred, "orgId")) json_set_str(out, "orgId", json_get_str(cred, "orgId"));
    const char *base = provider_base_url(provider);
    if (!token) {
        json_free(cred);
        return outcome(out, false, "account_unavailable");
    }
    char *credit = json_get_str(target, "creditId") ? xstrdup(json_get_str(target, "creditId")) : NULL;
    if (claude) {
        json_t *list = usage_claude_list_resets(token, json_get_str(cred, "orgId"), base, NULL);
        if (!list) {
            free(credit);
            json_free(cred);
            return outcome(out, false, "credit_list_failed");
        }
        const char *next = json_get_str(list, "nextCreditId");
        const json_t *sel = NULL, *c;
        json_arr_foreach(json_get_arr(list, "credits"), i, c) if (next && str_eq(json_get_str(c, "id"), next)) sel = c;
        if (credit && !str_eq(credit, next)) {
            json_set_str(out, "creditId", credit);
            outcome(out, false, "offer_changed");
        } else if (!json_get_bool(list, "eligible", false) || !sel || !json_get_bool(sel, "usable", false) ||
                   json_get_num(list, "redeemableCount", 0) < 1) {
            outcome(out, false, json_get_int(list, "availableCount", 0) > 0 ? "ineligible" : "no_credit");
            if (json_get_str(list, "reason")) json_set_str(out, "reason", json_get_str(list, "reason"));
        } else {
            const char *org = json_get_str(list, "orgId") ? json_get_str(list, "orgId") : json_get_str(cred, "orgId");
            const char *lb = json_get_str(list, "baseUrl");
            /* the discovery base is the OAuth API root (…/api/oauth); consume resolves the API root from it */
            json_t *r = usage_claude_consume_reset(token, org, lb ? lb : base, sel);
            outcome(out, json_get_bool(r, "ok", false), json_get_str(r, "code"));
            if (json_get_str(r, "reason")) json_set_str(out, "reason", json_get_str(r, "reason"));
            if (json_get(r, "cleared")) json_set(out, "cleared", json_clone(json_get(r, "cleared")));
            json_set_str(out, "creditId", json_get_str(sel, "id"));
            json_free(r);
        }
        json_free(list);
    } else {
        if (!credit) {
            json_t *list = usage_codex_list_resets(token, acc, base, 15000);
            if (!list) {
                json_free(cred);
                return outcome(out, false, "credit_list_failed");
            }
            const json_t *c = soonest_credit(json_get_arr(list, "credits"));
            if (c) credit = xstrdup(json_get_str(c, "id"));
            json_free(list);
            if (!credit) {
                json_free(cred);
                return outcome(out, false, "no_credit");
            }
        }
        json_t *r = usage_codex_consume_reset(token, acc, base, credit);
        outcome(out, json_get_bool(r, "ok", false), json_get_str(r, "code"));
        json_set_str(out, "creditId", credit);
        json_free(r);
    }
    if (json_get_bool(out, "ok", false)) usage_invalidate(provider);
    free(credit);
    json_free(cred);
    return out;
}

/* ---------- reset-usage.ts ---------- */

const char *usage_reset_provider_label(const char *provider) {
    if (str_eq(provider, "openai-codex")) return "Codex";
    if (str_eq(provider, "anthropic")) return "Claude";
    return provider;
}

static json_t *reset_account(const json_t *st) {
    const char *provider = json_get_str(st, "provider");
    usage_reset_summary_t s;
    bool has = usage_reset_summary(st, now_ms(), &s);
    int64_t avail = json_get_int(st, "availableCount", 0);
    int64_t advertised = json_is_num(json_get(st, "redeemableCount")) ? json_get_int(st, "redeemableCount", 0) : avail;
    int64_t redeemable = str_eq(provider, "anthropic") && !json_get_str(st, "nextCreditId") ? 0 : advertised;
    const char *identity = json_get_str(st, "email") ? json_get_str(st, "email") : json_get_str(st, "accountId") ? json_get_str(st, "accountId") : "account";
    const char *org = json_get_str(st, "orgName") ? json_get_str(st, "orgName") : json_get_str(st, "orgId");
    json_t *a = json_obj();
    char *label = org ? xasprintf("%s \xc2\xb7 %s", identity, org) : xstrdup(identity);
    json_set_str(a, "label", label);
    free(label);
    json_set_str(a, "provider", provider);
    json_set_str(a, "providerLabel", usage_reset_provider_label(provider));
    json_set_int(a, "availableCount", avail);
    json_set_int(a, "redeemableCount", redeemable);
    json_t *t = json_obj();
    json_set_int(t, "credentialId", json_get_int(st, "credentialId", 0));
    json_set_str(t, "provider", provider);
    static const char *const keys[] = {"accountId", "email", "orgId"};
    for (size_t k = 0; k < ARRAY_LEN(keys); k++)
        if (json_get_str(st, keys[k])) json_set_str(t, keys[k], json_get_str(st, keys[k]));
    if (json_get_str(st, "nextCreditId")) json_set_str(t, "creditId", json_get_str(st, "nextCreditId"));
    json_set(a, "target", t);
    json_set_bool(a, "active", json_get_bool(st, "active", false));
    if (json_get_str(st, "error")) json_set_str(a, "error", json_get_str(st, "error"));
    const char *reason = has ? s.unavailable_reason : NULL;
    if (!reason && str_eq(provider, "anthropic") && advertised > 0 && !json_get_str(st, "nextCreditId"))
        reason = "the provider did not identify a grant that can be safely spent";
    if (!reason && avail > 0 && redeemable == 0) reason = "not usable right now";
    if (reason) json_set_str(a, "unavailableReason", reason);
    if (has && s.soonest_expiry) json_set_str(a, "expiresAt", s.soonest_expiry);
    usage_reset_summary_free(&s);
    return a;
}

static int cmp_accounts(const void *pa, const void *pb) {
    const json_t *a = *(json_t *const *)pa, *b = *(json_t *const *)pb;
    bool aa = json_get_bool(a, "active", false), ba = json_get_bool(b, "active", false);
    if (aa != ba) return aa ? -1 : 1;
    int64_t ar = json_get_int(a, "redeemableCount", 0), br = json_get_int(b, "redeemableCount", 0);
    if (ar != br) return br > ar ? 1 : -1;
    int64_t av = json_get_int(a, "availableCount", 0), bv = json_get_int(b, "availableCount", 0);
    if (av != bv) return bv > av ? 1 : -1;
    int c = strcmp(json_get_str(a, "provider"), json_get_str(b, "provider"));
    if (c) return c;
    return strcmp(json_get_str(a, "label"), json_get_str(b, "label"));
}

json_t *usage_reset_accounts(const json_t *statuses) {
    size_t n = json_len(statuses);
    json_t **v = xcalloc(n ? n : 1, sizeof *v);
    for (size_t i = 0; i < n; i++) v[i] = reset_account(json_at(statuses, i));
    qsort(v, n, sizeof *v, cmp_accounts);
    json_t *out = json_arr();
    for (size_t i = 0; i < n; i++) json_push(out, v[i]);
    free(v);
    return out;
}

char *usage_reset_account_line(const json_t *a) {
    sb_t d = SB_INIT;
    if (json_get_str(a, "error")) {
        char *e = usage_one_line(json_get_str(a, "error"));
        sb_appendf(&d, "unavailable (%s)", e);
        free(e);
    } else {
        sb_appendf(&d, "%lld saved, %lld usable now", (long long)json_get_int(a, "availableCount", 0),
                   (long long)json_get_int(a, "redeemableCount", 0));
        if (json_get_str(a, "expiresAt")) {
            char *e = usage_one_line(json_get_str(a, "expiresAt"));
            sb_appendf(&d, ", expires %s", e);
            free(e);
        }
        if (json_get_int(a, "redeemableCount", 0) == 0 && json_get_str(a, "unavailableReason")) {
            char *e = usage_one_line(json_get_str(a, "unavailableReason"));
            sb_appendf(&d, " (%s)", e);
            free(e);
        }
    }
    char *label = usage_one_line(json_get_str(a, "label")), *pl = usage_one_line(json_get_str(a, "providerLabel"));
    char *r = xasprintf("- %s [%s \xc2\xb7 %s/%lld]: %s%s", label, pl, json_get_str(a, "provider"),
                        (long long)json_get_int(json_get_obj(a, "target"), "credentialId", 0), sb_cstr(&d),
                        json_get_bool(a, "active", false) ? " (active)" : "");
    free(label);
    free(pl);
    sb_free(&d);
    return r;
}

static const char *normalize_provider(const char *v) {
    char *t = str_trim(v), *l = str_lower(t);
    free(t);
    const char *r = NULL;
    if (!strcmp(l, "anthropic") || !strcmp(l, "claude")) r = "anthropic";
    else if (!strcmp(l, "openai-codex") || !strcmp(l, "codex")) r = "openai-codex";
    free(l);
    return r;
}

const json_t *usage_resolve_reset_target(const json_t *accounts, const char *arg, const char *command, char **err) {
    char *text = str_trim(arg ? arg : "");
    char *slash = strchr(text, '/');
    if (!slash || slash == text) {
        *err = xasprintf("Choose an account with `%s <provider>/<credential id>`.", command);
        free(text);
        return NULL;
    }
    *slash = 0;
    const char *provider = normalize_provider(text);
    if (!provider) {
        char *o = usage_one_line(text);
        *err = xasprintf("Unknown reset provider \"%s\". Use anthropic or openai-codex.", o);
        free(o);
        free(text);
        return NULL;
    }
    char *acct_t = str_trim(slash + 1), *acct = str_lower(acct_t);
    free(acct_t);
    bool numeric = *acct != 0;
    for (const char *p = acct; *p; p++)
        if (*p < '0' || *p > '9') numeric = false;
    int64_t cid = numeric ? strtoll(acct, NULL, 10) : -1;
    const json_t *found = NULL, *a;
    json_arr_foreach(accounts, i, a) {
        if (!str_eq(json_get_str(a, "provider"), provider)) continue;
        if (!strcmp(acct, "active") ? json_get_bool(a, "active", false)
                                    : numeric && json_get_int(json_get_obj(a, "target"), "credentialId", -2) == cid) {
            found = a;
            break;
        }
    }
    free(acct);
    if (!found) {
        *slash = '/';
        char *t2 = str_trim(arg), *o = usage_one_line(t2);
        *err = xasprintf("No stored account matches \"%s\". List choices with `%s`.", o, command);
        free(o);
        free(t2);
        free(text);
        return NULL;
    }
    free(text);
    char *label = usage_one_line(json_get_str(found, "label")), *pl = usage_one_line(json_get_str(found, "providerLabel"));
    if (json_get_str(found, "error")) {
        char *e = usage_one_line(json_get_str(found, "error"));
        *err = xasprintf("%s [%s]: saved resets unavailable (%s), so nothing was spent.", label, pl, e);
        free(e);
        found = NULL;
    } else if (json_get_int(found, "redeemableCount", 0) <= 0) {
        const char *ur = json_get_str(found, "unavailableReason");
        char *e = ur ? usage_one_line(ur) : NULL;
        char *reason = e ? xasprintf(" (%s)", e) : xstrdup("");
        *err = xasprintf("%s [%s]: no saved resets usable right now%s.", label, pl, reason);
        free(reason);
        free(e);
        found = NULL;
    }
    free(label);
    free(pl);
    return found;
}

char *usage_describe_redeem(const json_t *o, const char *label) {
    const char *prov = json_get_str(o, "provider");
    char *p = prov && *prov ? xasprintf(" (%s)", usage_reset_provider_label(prov)) : xstrdup("");
    char *reason = json_get_str(o, "reason") ? xasprintf(" \xe2\x80\x94 %s", json_get_str(o, "reason")) : xstrdup("");
    const char *code = json_get_str(o, "code");
    if (!code) code = "";
    char *r;
    if (!strcmp(code, "reset")) {
        const json_t *cl = json_get_arr(o, "cleared");
        size_t n = json_len(cl);
        const char *scope;
        char *own = NULL;
        if (n == 1 && str_eq(json_sv(json_at(cl, 0)), "anthropic:5h")) scope = "Claude's 5h session limit has been refreshed; weekly limits are unchanged";
        else if (n > 0) scope = own = xasprintf("the covered rate-limit window%s been refreshed", n == 1 ? " has" : "s have");
        else scope = "your rate-limit window has been refreshed";
        r = xasprintf("Reset applied for %s%s \xe2\x80\x94 %s.", label, p, scope);
        free(own);
    } else if (!strcmp(code, "already_redeemed")) r = xasprintf("%s%s: that reset was already redeemed.", label, p);
    else if (!strcmp(code, "no_credit")) r = xasprintf("%s%s: no saved resets available to spend%s.", label, p, reason);
    else if (!strcmp(code, "credit_list_failed"))
        r = xasprintf("%s%s: couldn't load this account's saved resets (network/auth) \xe2\x80\x94 nothing was spent, try again.", label, p);
    else if (!strcmp(code, "nothing_to_reset"))
        r = xasprintf("%s%s: nothing to reset right now \xe2\x80\x94 your covered limits aren't constrained, so no credit was spent.", label, p);
    else if (!strcmp(code, "no_account")) r = xasprintf("Could not find the stored account %s%s.", label, p);
    else if (!strcmp(code, "account_unavailable")) r = xasprintf("%s%s: could not authenticate this account \xe2\x80\x94 try /login.", label, p);
    else if (!strcmp(code, "offer_changed"))
        r = xasprintf("%s%s: the reset offer changed \xe2\x80\x94 nothing was spent; reopen /usage reset.", label, p);
    else if (!strcmp(code, "reset_in_progress")) r = xasprintf("%s%s: a reset is already in progress.", label, p);
    else if (!strcmp(code, "reset_unconfirmed") || !strcmp(code, "network_error") || !strcmp(code, "malformed_response"))
        r = xasprintf("%s%s: couldn't confirm whether the reset applied \xe2\x80\x94 check /usage before trying again%s.", label, p, reason);
    else r = xasprintf("%s%s: reset was not confirmed (%s)%s.", label, p, code, reason);
    free(p);
    free(reason);
    return r;
}
