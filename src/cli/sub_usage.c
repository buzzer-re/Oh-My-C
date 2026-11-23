/* sub_usage.c - `omc usage` (port of omp packages/coding-agent/src/commands/usage.ts + cli/usage-cli.ts):
 * the per-account usage breakdown, --json, --redact, invalidate, accounts, reset and --history. Account policies,
 * disabled-credential tombstones, re-login deadlines and the saved-reset expiry banner are not ported (omc keeps
 * one auth.json credential per provider and has no account policies or auth broker). */
#define _GNU_SOURCE
#include "cli/cli_int.h"

#include "config/auth.h"
#include "config/usage.h"
#include "session/trace.h"

#include <ctype.h>
#include <math.h>

#define BAR_WIDTH 28

/* ---------- redaction (buildRedactionMap) ---------- */

typedef struct {
    strvec_t keys, masks;
} redact_t;

static const char *rmask(const redact_t *r, const char *v) {
    if (!r || !v) return v;
    for (size_t i = 0; i < r->keys.n; i++)
        if (!strcmp(r->keys.v[i], v)) return r->masks.v[i];
    return v;
}

static char *infix(const char *value, const strvec_t *peers) {
    size_t len = strlen(value), start = MIN((size_t)2, len);
    double center = len / 2.0;
    for (size_t l = 1; l + start <= len; l++) {
        char *best = NULL;
        double bd = INFINITY;
        for (size_t pos = start; pos + l <= len; pos++) {
            char *cand = xstrndup(value + pos, l);
            bool hit = false;
            for (size_t k = 0; k < peers->n && !hit; k++) hit = strstr(peers->v[k], cand) != NULL;
            double d = fabs(pos + l / 2.0 - center);
            if (!hit && d < bd) {
                free(best);
                best = cand, bd = d;
            } else free(cand);
        }
        if (best) return best;
    }
    return NULL;
}

static void redact_build(redact_t *r, const strvec_t *values) {
    memset(r, 0, sizeof *r);
    strvec_t uniq = {0};
    for (size_t i = 0; i < values->n; i++)
        if (*values->v[i] && !strvec_contains(&uniq, values->v[i])) strvec_push(&uniq, values->v[i]);
    for (size_t i = 0; i < uniq.n; i++) {
        const char *v = uniq.v[i];
        char anchor[3] = {0};
        strncpy(anchor, v, 2);
        strvec_t peers = {0};
        for (size_t k = 0; k < uniq.n; k++)
            if (k != i && !strncmp(uniq.v[k], anchor, strlen(anchor)) && strlen(uniq.v[k]) >= strlen(anchor) &&
                !strncmp(uniq.v[k], v, MIN((size_t)2, strlen(v))))
                strvec_push(&peers, uniq.v[k]);
        char *mask;
        if (peers.n == 0) mask = xasprintf("%s*", anchor);
        else {
            char *x = infix(v, &peers);
            mask = x ? xasprintf("%s*%s*", anchor, x) : xasprintf("%s*", anchor);
            free(x);
        }
        strvec_push(&r->keys, v);
        strvec_push(&r->masks, mask);
        free(mask);
        strvec_free(&peers);
    }
    /* residual collisions fall back to prefix extension */
    for (size_t i = 0; i < r->keys.n; i++) {
        bool collided = false;
        for (size_t k = 0; k < r->keys.n && !collided; k++) collided = k != i && !strcmp(r->masks.v[i], r->masks.v[k]);
        if (!collided) continue;
        const char *v = r->keys.v[i];
        size_t l = MIN((size_t)2, strlen(v));
        for (;;) {
            bool clash = false;
            for (size_t k = 0; k < r->keys.n && !clash; k++)
                clash = k != i && !strcmp(r->masks.v[i], r->masks.v[k]) && !strncmp(r->keys.v[k], v, l);
            if (!clash || l >= strlen(v)) break;
            l++;
        }
        char *m = xasprintf("%.*s*", (int)l, v);
        free(r->masks.v[i]);
        r->masks.v[i] = m;
    }
    strvec_free(&uniq);
}

static void redact_free(redact_t *r) {
    strvec_free(&r->keys);
    strvec_free(&r->masks);
}

static void collect_identity(strvec_t *out, const json_t *reports, const json_t *accounts) {
    json_t *r, *l, *a;
    json_arr_foreach(reports, i, r) {
        const json_t *md = json_get_obj(r, "metadata");
        static const char *const mk[] = {"email", "accountId", "projectId", "orgId", "orgName"};
        for (size_t k = 0; k < ARRAY_LEN(mk); k++)
            if (json_get_str(md, mk[k]) && *json_get_str(md, mk[k])) strvec_push(out, json_get_str(md, mk[k]));
        json_arr_foreach(json_get_arr(r, "limits"), j, l) {
            const json_t *sc = json_get_obj(l, "scope");
            static const char *const sk[] = {"accountId", "projectId", "orgId"};
            for (size_t k = 0; k < ARRAY_LEN(sk); k++)
                if (json_get_str(sc, sk[k]) && *json_get_str(sc, sk[k])) strvec_push(out, json_get_str(sc, sk[k]));
        }
    }
    json_arr_foreach(accounts, i, a) {
        static const char *const ak[] = {"email", "accountId", "projectId", "orgId", "orgName", "enterpriseUrl"};
        for (size_t k = 0; k < ARRAY_LEN(ak); k++)
            if (json_get_str(a, ak[k]) && *json_get_str(a, ak[k])) strvec_push(out, json_get_str(a, ak[k]));
    }
}

/* ---------- limit rows ---------- */

static const char *resolve_status(const json_t *l) {
    const char *s = json_get_str(l, "status");
    if (s && strcmp(s, "unknown")) return s;
    double f;
    if (!usage_used_fraction(l, &f)) return "unknown";
    if (f >= 1) return "exhausted";
    if (f >= 0.8) return "warning";
    return "ok";
}

static char *status_color(const char *status, const char *text) {
    if (!strcmp(status, "exhausted")) return cli_red(text);
    if (!strcmp(status, "warning")) return cli_yellow(text);
    if (!strcmp(status, "ok")) return cli_green(text);
    return cli_dim(text);
}

static const char *aggregate_status(const json_t *limits) {
    bool ex = false, wa = false, ok = false;
    json_t *l;
    json_arr_foreach(limits, i, l) {
        const char *s = resolve_status(l);
        ex |= !strcmp(s, "exhausted"), wa |= !strcmp(s, "warning"), ok |= !strcmp(s, "ok");
    }
    return ex ? "exhausted" : wa ? "warning" : ok ? "ok" : "unknown";
}

static char *unit_value(double v, const char *unit) {
    if (str_eq(unit, "usd")) return xasprintf("$%.2f", v);
    return usage_fmt_number(v, 2);
}

static const char *unit_suffix(const char *unit) {
    if (str_eq(unit, "tokens")) return " tokens";
    if (str_eq(unit, "requests")) return " requests";
    if (str_eq(unit, "credits")) return " credits";
    if (str_eq(unit, "minutes")) return " min";
    if (str_eq(unit, "bytes")) return " bytes";
    return "";
}

static bool jnum(const json_t *o, const char *k, double *v) {
    json_t *x = json_get(o, k);
    if (!json_is_num(x)) return false;
    *v = x->u.num.d;
    return true;
}

static char *describe_amount(const json_t *l) {
    const json_t *a = json_get_obj(l, "amount");
    const char *unit = json_get_str(a, "unit");
    bool abs_unit = unit && strcmp(unit, "percent") && strcmp(unit, "unknown");
    double used, lim, rem, f, rf;
    bool hu = jnum(a, "used", &used), hl = jnum(a, "limit", &lim), hr = jnum(a, "remaining", &rem);
    bool hf = usage_used_fraction(l, &f);
    strvec_t parts = {0};
    if (abs_unit && hu && hl) {
        char *u = unit_value(used, unit), *li = unit_value(lim, unit);
        char *p = xasprintf("%s / %s%s", u, li, unit_suffix(unit));
        strvec_push(&parts, p);
        free(p), free(u), free(li);
    } else if (abs_unit && hr) {
        char *r = unit_value(rem, unit), *p = xasprintf("%s%s left", r, unit_suffix(unit));
        strvec_push(&parts, p);
        free(p), free(r);
    } else if (abs_unit && hu && isfinite(used) && !hl && !hr && !hf) {
        char *u = unit_value(used, unit), *p = xasprintf("%s%s used", u, unit_suffix(unit));
        strvec_push(&parts, p);
        free(p), free(u);
    }
    if (hf) {
        char *p = xasprintf("%.1f%% used", f * 100);
        strvec_push(&parts, p);
        free(p);
    } else if (jnum(a, "remainingFraction", &rf)) {
        char *p = xasprintf("%.1f%% left", rf * 100);
        strvec_push(&parts, p);
        free(p);
    }
    if (!parts.n) strvec_push(&parts, "no data");
    char *r = strvec_join(&parts, " \xc2\xb7 ");
    strvec_free(&parts);
    return r;
}

static char *render_bar(const json_t *l) {
    double f;
    sb_t b = SB_INIT;
    if (!usage_used_fraction(l, &f)) {
        for (int i = 0; i < BAR_WIDTH; i++) sb_append(&b, "\xc2\xb7");
        char *d = cli_dim(sb_cstr(&b));
        sb_free(&b);
        return d;
    }
    double c = MIN(MAX(f, 0.0), 1.0);
    int filled = (int)floor(c * BAR_WIDTH + 0.5);
    sb_t e = SB_INIT;
    for (int i = 0; i < filled; i++) sb_append(&b, "\xe2\x96\x88");
    for (int i = filled; i < BAR_WIDTH; i++) sb_append(&e, "\xe2\x96\x91");
    char *fc = status_color(resolve_status(l), sb_cstr(&b)), *ec = cli_dim(sb_cstr(&e));
    char *r = xasprintf("%s%s", fc, ec);
    free(fc), free(ec);
    sb_free(&b), sb_free(&e);
    return r;
}

/* limitTitle: label (+ tier) (+ window) */
static char *limit_title(const json_t *l) {
    char *label = usage_limit_title(l);
    const json_t *w = json_get_obj(l, "window");
    const char *win = json_get_str(w, "label") ? json_get_str(w, "label") : json_get_str(json_get_obj(l, "scope"), "windowId");
    if (!win || !strcasecmp(win, "quota window") || str_istr(label, win)) return label;
    char *r = xasprintf("%s (%s)", label, win);
    free(label);
    return r;
}

static char *pad_end(const char *s, int width) {
    int w = (int)utf8_strlen(s);
    return w >= width ? xstrdup(s) : xasprintf("%s%*s", s, width - w, "");
}

static void limit_lines(strvec_t *out, const json_t *l, int label_width, int64_t now) {
    char *title = limit_title(l), *padded = pad_end(title, label_width);
    char *amount = describe_amount(l);
    sb_t det = SB_INIT;
    sb_append(&det, amount);
    const json_t *w = json_get_obj(l, "window");
    double ra;
    if (jnum(w, "resetsAt", &ra) && ra > now) {
        char *d = usage_fmt_duration(ra - (double)now);
        sb_appendf(&det, " \xc2\xb7 %s in %s", json_get_str(w, "resetLabel") ? json_get_str(w, "resetLabel") : "resets", d);
        free(d);
    }
    char *dot = status_color(resolve_status(l), "\xe2\x97\x8f"), *bar = render_bar(l), *dd = cli_dim(sb_cstr(&det));
    char *line = xasprintf("      %s %s  %s  %s", dot, padded, bar, dd);
    strvec_push(out, line);
    free(line), free(dot), free(bar), free(dd);
    json_t *notes = json_get_arr(l, "notes");
    if (json_len(notes)) {
        sb_t n = SB_INIT;
        json_t *x;
        json_arr_foreach(notes, i, x) sb_appendf(&n, "%s%s", i ? " \xc2\xb7 " : "", json_sv(x) ? json_sv(x) : "");
        char *nd = cli_dim(sb_cstr(&n)), *nl = xasprintf("        %s", nd);
        strvec_push(out, nl);
        free(nl), free(nd);
        sb_free(&n);
    }
    sb_free(&det);
    free(title), free(padded), free(amount);
}

static char *meter_key(const json_t *l) {
    const char *wid = json_get_str(json_get_obj(l, "window"), "id");
    if (!wid) wid = json_get_str(json_get_obj(l, "scope"), "windowId");
    return xasprintf("%s|%s", json_get_str(l, "label") ? json_get_str(l, "label") : "", wid ? wid : "");
}

/* createLimitRowKey */
static char *row_key(const json_t *l, const strvec_t *tiered) {
    char *k = meter_key(l);
    if (!strvec_contains(tiered, k)) return k;
    const char *tier = json_get_str(json_get_obj(l, "scope"), "tier");
    char *r = xasprintf("%s|%s", k, tier ? tier : "");
    free(k);
    return r;
}

static const char *meta(const json_t *r, const char *k) {
    const char *v = json_get_str(json_get_obj(r, "metadata"), k);
    return v && *v ? v : NULL;
}

static char *report_account_label(const json_t *r, size_t index) {
    static const char *const keys[] = {"email", "accountId", "projectId"};
    for (size_t k = 0; k < ARRAY_LEN(keys); k++)
        if (meta(r, keys[k])) return xstrdup(meta(r, keys[k]));
    json_t *l;
    json_arr_foreach(json_get_arr(r, "limits"), i, l) {
        const json_t *sc = json_get_obj(l, "scope");
        const char *v = json_get_str(sc, "accountId") ? json_get_str(sc, "accountId") : json_get_str(sc, "projectId");
        if (v) return xstrdup(v);
    }
    return xasprintf("account %zu", index + 1);
}

static char *report_identity(const json_t *r, const json_t *peers, size_t index, const redact_t *red) {
    char *label = report_account_label(r, index);
    sb_t b = SB_INIT;
    if (str_eq(json_get_str(r, "provider"), "openai-codex")) {
        /* formatQualifiedIdentity: bold identity, dim " · org · plan" */
        char *ident = usage_one_line(rmask(red, label));
        const char *email = meta(r, "email");
        bool collision = false;
        size_t idx = 0;
        json_t *p;
        json_arr_foreach(peers, i, p) {
            if (p == r) idx = i;
            else if (email && str_eq(meta(p, "email"), email)) collision = true;
        }
        sb_t rest = SB_INIT;
        if (collision) {
            char *org = meta(r, "orgId") ? xstrdup(meta(r, "orgId")) : meta(r, "accountId") ? xstrdup(meta(r, "accountId")) : xasprintf("account %zu", idx + 1);
            if (strcmp(org, label)) {
                char *o = usage_one_line(rmask(red, org));
                sb_appendf(&rest, " \xc2\xb7 %s", o);
                free(o);
            }
            free(org);
        }
        const char *plan = meta(r, "subscriptionPlan") ? meta(r, "subscriptionPlan") : meta(r, "planType"); /* usageSubscriptionPlan */
        char *pt = plan ? str_trim(plan) : NULL;
        if (pt && *pt) {
            char *o = usage_one_line(pt);
            sb_appendf(&rest, " \xc2\xb7 plan: %s", o);
            free(o);
        }
        free(pt);
        char *bi = cli_bold(ident), *dr = cli_dim(sb_cstr(&rest));
        sb_appendf(&b, "%s%s", bi, dr);
        free(bi), free(dr), free(ident);
        sb_free(&rest);
    } else {
        char *bi = cli_bold(rmask(red, label));
        sb_append(&b, bi);
        free(bi);
        const char *org = meta(r, "orgName") ? meta(r, "orgName") : json_get_str(json_get_obj(r, "metadata"), "orgId");
        if (org && *org && strcmp(org, label)) {
            char *t = xasprintf(" \xc2\xb7 %s", rmask(red, org)), *d = cli_dim(t);
            sb_append(&b, d);
            free(t), free(d);
        }
        const char *plan = meta(r, "subscriptionPlan") ? meta(r, "subscriptionPlan") : meta(r, "planType"); /* usageSubscriptionPlan */
        char *pt = plan ? str_trim(plan) : NULL;
        if (pt && *pt) {
            char *t = xasprintf(" \xc2\xb7 plan: %s", pt), *d = cli_dim(t);
            sb_append(&b, d);
            free(t), free(d);
        }
        free(pt);
    }
    free(label);
    return sb_detach(&b);
}

static char *account_header(const json_t *r, const json_t *peers, size_t index, int64_t now, const redact_t *red) {
    sb_t b = SB_INIT;
    char *dot = status_color(aggregate_status(json_get_arr(r, "limits")), "\xe2\x97\x8f"), *id = report_identity(r, peers, index, red);
    sb_appendf(&b, "%s %s", dot, id);
    free(dot), free(id);
    if (json_get_bool(json_get_obj(r, "metadata"), "daybreak", false)) {
        char *c = cli_cyan(" \xc2\xb7 daybreak");
        sb_append(&b, c);
        free(c);
    }
    usage_reset_summary_t rs;
    if (usage_reset_summary(json_get_obj(r, "resetCredits"), now, &rs) && rs.banked > 0) {
        char *t = xasprintf(" \xc2\xb7 \xe2\x9c\xa6 %lld saved reset%s", (long long)rs.banked, rs.banked == 1 ? "" : "s"), *c = cli_cyan(t);
        sb_append(&b, c);
        free(t), free(c);
        if (rs.redeemable != rs.banked) {
            t = xasprintf(" \xc2\xb7 %lld usable now", (long long)rs.redeemable), c = cli_dim(t);
            sb_append(&b, c);
            free(t), free(c);
        }
        if (rs.soonest_expiry) {
            double ems;
            if (trace_parse_iso_ms(rs.soonest_expiry, &ems) && ems > now) {
                char *d = usage_fmt_duration(ems - (double)now);
                t = xasprintf(" \xc2\xb7 soonest expires in %s (%.10s)", d, rs.soonest_expiry);
                free(d);
            } else t = xasprintf(" \xc2\xb7 expired (%.10s)", rs.soonest_expiry);
            c = cli_dim(t);
            sb_append(&b, c);
            free(t), free(c);
        }
        if (rs.redeemable == 0 && rs.unavailable_reason) {
            char *o = usage_one_line(rs.unavailable_reason);
            t = xasprintf(" \xc2\xb7 unavailable: %s", o), c = cli_dim(t);
            sb_append(&b, c);
            free(o), free(t), free(c);
        }
    }
    usage_reset_summary_free(&rs);
    double fa = json_get_num(r, "fetchedAt", 0);
    if (fa > 0 && now - fa > 90000) {
        char *d = usage_fmt_duration(now - fa), *t = xasprintf(" \xc2\xb7 fetched %s ago", d), *c = cli_dim(t);
        sb_append(&b, c);
        free(d), free(t), free(c);
    }
    return sb_detach(&b);
}

/* ---------- capacity (computeProviderWindowStats) ---------- */

typedef struct {
    char *key, *window, *meter;
    bool has_dur;
    double dur;
    VEC(double) fr;
} wstat_t;

static char *meter_for(const json_t *r, const json_t *l) {
    const char *prov = json_get_str(r, "provider");
    if (!str_eq(prov, "anthropic") && !str_eq(prov, "openai-codex")) return NULL;
    const char *tier = json_get_str(json_get_obj(l, "scope"), "tier");
    if (tier) {
        char *t = str_trim(tier), *lo = str_lower(t);
        free(t);
        if (*lo) return lo;
        free(lo);
    }
    if (!str_eq(prov, "openai-codex")) return NULL;
    const char *id = json_get_str(l, "id");
    const char *c = id ? strchr(id, ':') : NULL;
    if (c) {
        char *slug = xstrndup(c + 1, strcspn(c + 1, ":")), *lo = str_lower(slug);
        free(slug);
        if (*lo && strcmp(lo, "primary") && strcmp(lo, "secondary")) return lo;
        free(lo);
    }
    return xstrdup("chat");
}

static int wstat_cmp(const void *pa, const void *pb) {
    const wstat_t *a = pa, *b = pb;
    double da = a->has_dur ? a->dur : INFINITY, db = b->has_dur ? b->dur : INFINITY;
    if (da != db) return da < db ? -1 : 1;
    return strcmp(a->meter ? a->meter : "", b->meter ? b->meter : "");
}

static char *capacity_line(const json_t *reports, json_t *json_out) {
    VEC(wstat_t) st = {0};
    json_t *r, *l;
    json_arr_foreach(reports, i, r) {
        strvec_t keys = {0};
        VEC(double) maxes = {0};
        json_arr_foreach(json_get_arr(r, "limits"), j, l) {
            double f;
            if (!usage_used_fraction(l, &f)) continue;
            const json_t *w = json_get_obj(l, "window");
            double dur = 0;
            bool hd = jnum(w, "durationMs", &dur);
            char *wkey = hd ? xasprintf("d:%.17g", dur)
                            : xstrdup(json_get_str(json_get_obj(l, "scope"), "windowId") ? json_get_str(json_get_obj(l, "scope"), "windowId")
                                      : json_get_str(w, "label") ? json_get_str(w, "label") : json_get_str(l, "label"));
            char *m = meter_for(r, l);
            char *key = m ? xasprintf("m:%s|%s", m, wkey) : xstrdup(wkey);
            size_t ki = keys.n;
            for (size_t k = 0; k < keys.n; k++)
                if (!strcmp(keys.v[k], key)) ki = k;
            if (ki == keys.n) {
                strvec_push(&keys, key);
                vec_push(&maxes, f);
            } else if (f > maxes.v[ki]) maxes.v[ki] = f;
            bool found = false;
            for (size_t k = 0; k < st.n && !found; k++) found = !strcmp(st.v[k].key, key);
            if (!found) {
                wstat_t s = {.key = xstrdup(key), .meter = m ? xstrdup(m) : NULL, .has_dur = hd, .dur = dur};
                s.window = hd ? usage_fmt_duration(dur)
                              : xstrdup(json_get_str(w, "label") ? json_get_str(w, "label")
                                        : json_get_str(json_get_obj(l, "scope"), "windowId") ? json_get_str(json_get_obj(l, "scope"), "windowId")
                                                                                           : json_get_str(l, "label"));
                vec_push(&st, s);
            }
            free(wkey), free(m), free(key);
        }
        for (size_t k = 0; k < keys.n; k++)
            for (size_t s = 0; s < st.n; s++)
                if (!strcmp(st.v[s].key, keys.v[k])) vec_push(&st.v[s].fr, maxes.v[k]);
        strvec_free(&keys);
        vec_free(&maxes);
    }
    xqsort(st.v, st.n, sizeof *st.v, wstat_cmp);
    char *line = NULL;
    if (st.n) {
        sb_t b = SB_INIT;
        sb_append(&b, "capacity: ");
        for (size_t s = 0; s < st.n; s++) {
            double used = 0;
            for (size_t k = 0; k < st.v[s].fr.n; k++) used += st.v[s].fr.v[k];
            double acc = (double)st.v[s].fr.n, rem = MAX(0.0, acc - used);
            if (s) sb_append(&b, " \xc2\xb7 ");
            sb_append(&b, st.v[s].window);
            if (st.v[s].meter) sb_appendf(&b, " (%c%s)", toupper((unsigned char)st.v[s].meter[0]), st.v[s].meter + 1);
            sb_appendf(&b, " \xe2\x86\x92 %.2f/%zu %s used (%.2f\xc3\x97 quota left)", used, st.v[s].fr.n, st.v[s].fr.n == 1 ? "account" : "accounts", rem);
            if (json_out) {
                json_t *o = json_obj();
                json_set_str(o, "window", st.v[s].window);
                if (st.v[s].has_dur) json_set_num(o, "durationMs", st.v[s].dur);
                if (st.v[s].meter) json_set_str(o, "meter", st.v[s].meter);
                json_set_int(o, "accounts", (int64_t)st.v[s].fr.n);
                json_set_num(o, "usedAccounts", used);
                json_set_num(o, "remainingAccounts", rem);
                json_push(json_out, o);
            }
        }
        line = sb_detach(&b);
    }
    for (size_t s = 0; s < st.n; s++) {
        free(st.v[s].key), free(st.v[s].window), free(st.v[s].meter);
        vec_free(&st.v[s].fr);
    }
    vec_free(&st);
    return line;
}

/* ---------- breakdown (formatUsageBreakdown) ---------- */

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void strvec_insert(strvec_t *v, size_t at, const char *s) {
    strvec_push(v, s);
    char *x = v->v[v->n - 1];
    memmove(v->v + at + 1, v->v + at, (v->n - 1 - at) * sizeof *v->v);
    v->v[at] = x;
}

static char *breakdown(const json_t *reports_in, const json_t *accounts, int64_t now, const redact_t *red) {
    json_t *disp = usage_collapse_shared(reports_in), *r, *a;
    json_t *unrep = usage_unreported_accounts(disp, accounts);
    strvec_t provs = {0};
    json_arr_foreach(disp, i, r) if (!strvec_contains(&provs, json_get_str(r, "provider"))) strvec_push(&provs, json_get_str(r, "provider"));
    json_arr_foreach(unrep, i, a) if (!strvec_contains(&provs, json_get_str(a, "provider"))) strvec_push(&provs, json_get_str(a, "provider"));
    xqsort(provs.v, provs.n, sizeof *provs.v, cmp_str);
    strvec_t out = {0};
    double latest = 0;
    json_arr_foreach(disp, i, r) latest = MAX(latest, json_get_num(r, "fetchedAt", 0));
    char *head = cli_bold("Usage");
    if (latest > 0) {
        char *d = usage_fmt_duration(now - latest), *t = xasprintf(" \xc2\xb7 fetched %s ago", d), *c = cli_dim(t);
        char *h = xasprintf("%s%s", head, c);
        free(head);
        head = h;
        free(d), free(t), free(c);
    }
    strvec_push(&out, head);
    free(head);
    for (size_t pi = 0; pi < provs.n; pi++) {
        const char *prov = provs.v[pi];
        json_t *pr = json_arr();
        json_arr_foreach(disp, i, r) if (str_eq(json_get_str(r, "provider"), prov)) json_push(pr, json_clone(r));
        size_t nunrep = 0;
        json_arr_foreach(unrep, i, a) if (str_eq(json_get_str(a, "provider"), prov)) nunrep++;
        size_t count = json_len(pr) + nunrep;
        strvec_push(&out, "");
        char *name = usage_provider_name(prov), *bn = cli_bold(name), *cn = cli_cyan(bn);
        char *cnt = xasprintf("\xe2\x80\x94 %zu %s", count, count == 1 ? "account" : "accounts"), *dc = cli_dim(cnt);
        char *line = xasprintf("%s %s", cn, dc);
        strvec_push(&out, line);
        free(name), free(bn), free(cn), free(cnt), free(dc), free(line);
        strvec_t notes = {0};
        json_arr_foreach(pr, i, r) {
            json_t *n;
            json_arr_foreach(json_get_arr(r, "notes"), k, n) if (json_sv(n) && !strvec_contains(&notes, json_sv(n))) strvec_push(&notes, json_sv(n));
        }
        for (size_t k = 0; k < notes.n; k++) {
            char *o = usage_one_line(notes.v[k]), *d = cli_dim(o), *l2 = xasprintf("  %s", d);
            strvec_push(&out, l2);
            free(o), free(d), free(l2);
        }
        strvec_free(&notes);
        /* row keys and templates */
        strvec_t tiered = {0};
        json_arr_foreach(pr, i, r) {
            strvec_t seen = {0};
            json_t *l;
            json_arr_foreach(json_get_arr(r, "limits"), k, l) {
                char *mk = meter_key(l);
                if (strvec_contains(&seen, mk) && !strvec_contains(&tiered, mk)) strvec_push(&tiered, mk);
                strvec_push(&seen, mk);
                free(mk);
            }
            strvec_free(&seen);
        }
        strvec_t tkeys = {0}, ttitles = {0};
        json_arr_foreach(pr, i, r) {
            size_t insert = 0;
            json_t *l;
            json_arr_foreach(json_get_arr(r, "limits"), k, l) {
                char *key = row_key(l, &tiered);
                size_t ex = tkeys.n;
                for (size_t t = 0; t < tkeys.n; t++)
                    if (!strcmp(tkeys.v[t], key)) ex = t;
                if (ex < tkeys.n) insert = ex + 1;
                else {
                    char *title = limit_title(l);
                    strvec_insert(&tkeys, insert, key);
                    strvec_insert(&ttitles, insert, title);
                    free(title);
                    insert++;
                }
                free(key);
            }
        }
        int lw = 0;
        for (size_t t = 0; t < ttitles.n; t++) lw = MAX(lw, (int)utf8_strlen(ttitles.v[t]));
        json_arr_foreach(pr, i, r) {
            json_t *l;
            json_arr_foreach(json_get_arr(r, "limits"), k, l) {
                char *t = limit_title(l);
                lw = MAX(lw, (int)utf8_strlen(t));
                free(t);
            }
        }
        json_arr_foreach(pr, i, r) {
            char *h = account_header(r, pr, i, now, red), *hl = xasprintf("  %s", h);
            strvec_push(&out, hl);
            free(h), free(hl);
            json_t *limits = json_get_arr(r, "limits");
            if (json_len(limits) == 0) {
                char *d = cli_dim("no limits reported"), *l2 = xasprintf("      %s", d);
                strvec_push(&out, l2);
                free(d), free(l2);
                continue;
            }
            for (size_t t = 0; t < tkeys.n; t++) {
                bool any = false;
                json_t *l;
                json_arr_foreach(limits, k, l) {
                    char *key = row_key(l, &tiered);
                    if (!strcmp(key, tkeys.v[t])) {
                        limit_lines(&out, l, lw, now);
                        any = true;
                    }
                    free(key);
                }
                if (!any) {
                    char *padded = pad_end(ttitles.v[t], lw);
                    sb_t dots = SB_INIT;
                    for (int x = 0; x < BAR_WIDTH; x++) sb_append(&dots, "\xc2\xb7");
                    char *c1 = cli_dim("\xe2\x97\x8b"), *c2 = cli_dim(sb_cstr(&dots)), *c3 = cli_dim("not reported");
                    char *l2 = xasprintf("      %s %s  %s  %s", c1, padded, c2, c3);
                    strvec_push(&out, l2);
                    free(padded), free(c1), free(c2), free(c3), free(l2);
                    sb_free(&dots);
                }
            }
        }
        json_arr_foreach(unrep, i, a) {
            if (!str_eq(json_get_str(a, "provider"), prov)) continue;
            char *label = usage_account_label(a);
            /* accountIdentityLabel with masks */
            if (red && !str_eq(json_get_str(a, "type"), "api_key")) {
                const char *base = json_get_str(a, "email") ? json_get_str(a, "email") : json_get_str(a, "accountId") ? json_get_str(a, "accountId")
                                   : json_get_str(a, "projectId") ? json_get_str(a, "projectId") : json_get_str(a, "enterpriseUrl") ? json_get_str(a, "enterpriseUrl") : "OAuth account";
                const char *org = json_get_str(a, "orgName") ? json_get_str(a, "orgName") : json_get_str(a, "orgId");
                free(label);
                label = !org || !strcmp(org, base) ? xstrdup(rmask(red, base)) : xasprintf("%s \xc2\xb7 %s", rmask(red, base), rmask(red, org));
            }
            char *t = xasprintf("%s \xe2\x80\x94 no usage data", label), *c1 = cli_dim("\xe2\x97\x8b"), *c2 = cli_dim(t), *l2 = xasprintf("  %s %s", c1, c2);
            strvec_push(&out, l2);
            free(label), free(t), free(c1), free(c2), free(l2);
        }
        char *cap = capacity_line(pr, NULL);
        if (cap) {
            char *d = cli_dim(cap), *l2 = xasprintf("  %s", d);
            strvec_push(&out, l2);
            free(d), free(l2), free(cap);
        }
        strvec_free(&tiered), strvec_free(&tkeys), strvec_free(&ttitles);
        json_free(pr);
    }
    char *s = strvec_join(&out, "\n");
    strvec_free(&out), strvec_free(&provs);
    json_free(unrep), json_free(disp);
    return s;
}

static void mask_field(json_t *o, const char *k, const redact_t *red) {
    const char *v = json_get_str(o, k);
    if (v) json_set_str(o, k, rmask(red, v));
}

static char *no_provider_credentials(const char *provider, const json_t *stored) {
    strvec_t ps = {0};
    json_t *a;
    json_arr_foreach(stored, i, a) if (!strvec_contains(&ps, json_get_str(a, "provider"))) strvec_push(&ps, json_get_str(a, "provider"));
    xqsort(ps.v, ps.n, sizeof *ps.v, cmp_str);
    char *hint;
    if (ps.n) {
        char *j = strvec_join(&ps, ", ");
        hint = xasprintf("Providers with stored credentials: %s.", j);
        free(j);
    } else hint = xstrdup("Run `omc` and use /login to add accounts.");
    char *r = xasprintf("No credentials stored for provider \"%s\". %s\n", provider, hint);
    free(hint);
    strvec_free(&ps);
    return r;
}

static void warn_out(const char *msg) {
    char *y = cli_yellow(msg);
    fputs(y, stderr);
    free(y);
}

/* ---------- history (formatUsageHistory) ---------- */

#define HISTORY_SPARK_WIDTH 48

static const char *const SPARK[] = {"\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
                                    "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"};

static const char *history_status(bool has, double f, const char *status) {
    if (status && strcmp(status, "unknown")) return status;
    if (!has) return "unknown";
    return f >= 1 ? "exhausted" : f >= 0.8 ? "warning" : "ok";
}

static char *history_title(const json_t *e) {
    const char *label = json_get_str(e, "label") ? json_get_str(e, "label") : "", *w = json_get_str(e, "windowLabel");
    if (!w || !strcasecmp(w, "quota window") || str_istr(label, w)) return xstrdup(label);
    return xasprintf("%s (%s)", label, w);
}

static char *history_spark(const json_t *entries, int64_t since, int64_t now) {
    double b[HISTORY_SPARK_WIDTH];
    bool has[HISTORY_SPARK_WIDTH] = {0};
    double span = MAX(1.0, (double)(now - since));
    json_t *e;
    json_arr_foreach(entries, i, e) {
        if (!json_is_num(json_get(e, "usedFraction"))) continue;
        double v = json_get_num(e, "usedFraction", 0);
        int idx = (int)floor((json_get_num(e, "recordedAt", 0) - since) / span * HISTORY_SPARK_WIDTH);
        idx = MIN(HISTORY_SPARK_WIDTH - 1, MAX(0, idx));
        b[idx] = has[idx] ? MAX(b[idx], v) : v;
        has[idx] = true;
    }
    sb_t out = SB_INIT;
    for (int i = 0; i < HISTORY_SPARK_WIDTH; i++) {
        if (!has[i] || !isfinite(b[i])) {
            char *d = cli_dim("\xc2\xb7");
            sb_append(&out, d);
            free(d);
            continue;
        }
        double f = MIN(MAX(b[i], 0.0), 1.0);
        int lvl = MIN(7, (int)floor(f * 8));
        char *g = status_color(history_status(true, f, NULL), SPARK[lvl]);
        sb_append(&out, g);
        free(g);
    }
    return sb_detach(&out);
}

static int cmp_label(const void *a, const void *b) {
    return strcmp(json_get_str(*(json_t *const *)a, "_label"), json_get_str(*(json_t *const *)b, "_label"));
}

static int cmp_title(const void *a, const void *b) {
    return strcmp(json_get_str(*(json_t *const *)a, "title"), json_get_str(*(json_t *const *)b, "title"));
}

static char *format_history(const json_t *entries, int64_t since, int64_t now, const redact_t *red) {
    strvec_t out = {0};
    char *h = cli_bold("Usage history"), *d = usage_fmt_duration((double)(now - since));
    char *t = xasprintf(" \xc2\xb7 last %s \xc2\xb7 peak per bucket", d), *dt = cli_dim(t), *l = xasprintf("%s%s", h, dt);
    strvec_push(&out, l);
    free(h), free(d), free(t), free(dt), free(l);
    strvec_t provs = {0};
    json_t *e;
    json_arr_foreach(entries, i, e) if (!strvec_contains(&provs, json_get_str(e, "provider"))) strvec_push(&provs, json_get_str(e, "provider"));
    xqsort(provs.v, provs.n, sizeof *provs.v, cmp_str);
    for (size_t pi = 0; pi < provs.n; pi++) {
        const char *prov = provs.v[pi];
        json_t *accts = json_arr(), *a;
        json_arr_foreach(entries, i, e) {
            if (!str_eq(json_get_str(e, "provider"), prov)) continue;
            json_t *acc = NULL;
            json_arr_foreach(accts, k, a) if (str_eq(json_get_str(a, "_key"), json_get_str(e, "accountKey"))) acc = a;
            if (!acc) {
                acc = json_obj();
                json_set_str(acc, "_key", json_get_str(e, "accountKey"));
                const char *lab = json_get_str(e, "email") ? json_get_str(e, "email") : json_get_str(e, "accountId") ? json_get_str(e, "accountId") : json_get_str(e, "accountKey");
                json_set_str(acc, "_label", lab ? lab : "");
                if (json_get_str(e, "email")) json_set_str(acc, "email", json_get_str(e, "email"));
                if (json_get_str(e, "accountId")) json_set_str(acc, "accountId", json_get_str(e, "accountId"));
                json_set(acc, "series", json_arr());
                json_push(accts, acc);
            }
            json_t *ser = NULL, *x;
            json_arr_foreach(json_get_arr(acc, "series"), k, x) if (str_eq(json_get_str(x, "id"), json_get_str(e, "limitId"))) ser = x;
            if (!ser) {
                ser = json_obj();
                json_set_str(ser, "id", json_get_str(e, "limitId"));
                json_set(ser, "entries", json_arr());
                json_push(json_get_arr(acc, "series"), ser);
            }
            char *tt = history_title(e);
            json_set_str(ser, "title", tt);
            free(tt);
            json_push(json_get_arr(ser, "entries"), json_clone(e));
        }
        size_t na = json_len(accts);
        strvec_push(&out, "");
        char *nm = usage_provider_name(prov), *bn = cli_bold(nm), *cn = cli_cyan(bn);
        char *cnt = xasprintf("\xe2\x80\x94 %zu %s", na, na == 1 ? "account" : "accounts"), *dc = cli_dim(cnt), *pl = xasprintf("%s %s", cn, dc);
        strvec_push(&out, pl);
        free(nm), free(bn), free(cn), free(cnt), free(dc), free(pl);
        json_t **av = xcalloc(na ? na : 1, sizeof *av);
        for (size_t i = 0; i < na; i++) av[i] = json_at(accts, i);
        xqsort(av, na, sizeof *av, cmp_label);
        for (size_t ai = 0; ai < na; ai++) {
            json_t *acc = av[ai];
            const char *label = json_get_str(acc, "_label");
            char *ident;
            if (str_eq(prov, "openai-codex")) {
                const char *email = json_get_str(acc, "email");
                bool collision = false;
                for (size_t k = 0; k < na; k++)
                    if (k != ai && email && str_eq(json_get_str(av[k], "email"), email)) collision = true;
                char *base = usage_one_line(rmask(red, label)), *bb = cli_bold(base);
                sb_t rest = SB_INIT;
                if (collision) {
                    char *org = json_get_str(acc, "accountId") ? xstrdup(json_get_str(acc, "accountId")) : xasprintf("account %zu", ai + 1);
                    if (strcmp(org, label)) {
                        char *o = usage_one_line(rmask(red, org));
                        sb_appendf(&rest, " \xc2\xb7 %s", o);
                        free(o);
                    }
                    free(org);
                }
                char *dr = cli_dim(sb_cstr(&rest));
                ident = xasprintf("%s%s", bb, dr);
                free(base), free(bb), free(dr);
                sb_free(&rest);
            } else ident = cli_bold(rmask(red, label));
            char *il = xasprintf("  %s", ident);
            strvec_push(&out, il);
            free(il), free(ident);
            json_t *series = json_get_arr(acc, "series"), *x;
            int lw = 0;
            json_arr_foreach(series, k, x) lw = MAX(lw, (int)utf8_strlen(json_get_str(x, "title")));
            size_t ns = json_len(series);
            json_t **sv = xcalloc(ns ? ns : 1, sizeof *sv);
            for (size_t k = 0; k < ns; k++) sv[k] = json_at(series, k);
            xqsort(sv, ns, sizeof *sv, cmp_title);
            for (size_t k = 0; k < ns; k++) {
                json_t *se = json_get_arr(sv[k], "entries"), *y;
                bool hl = false;
                double latest = 0, peak = -INFINITY;
                json_arr_foreach(se, q, y) if (json_is_num(json_get(y, "usedFraction"))) {
                    latest = json_get_num(y, "usedFraction", 0);
                    peak = MAX(peak, latest);
                    hl = true;
                }
                const json_t *last = json_at(se, json_len(se) - 1);
                const char *st = history_status(hl, latest, json_get_str(last, "status"));
                sb_t det = SB_INIT;
                if (hl) sb_appendf(&det, "latest %.1f%% \xc2\xb7 peak %.1f%% \xc2\xb7 ", latest * 100, peak * 100);
                sb_appendf(&det, "%zu snapshot%s", json_len(se), json_len(se) == 1 ? "" : "s");
                char *dot = status_color(st, "\xe2\x97\x8f"), *pad = pad_end(json_get_str(sv[k], "title"), lw), *sp = history_spark(se, since, now),
                     *dd = cli_dim(sb_cstr(&det)), *ln = xasprintf("      %s %s  %s  %s", dot, pad, sp, dd);
                strvec_push(&out, ln);
                free(dot), free(pad), free(sp), free(dd), free(ln);
                sb_free(&det);
            }
            free(sv);
        }
        free(av);
        json_free(accts);
    }
    char *r = strvec_join(&out, "\n");
    strvec_free(&out), strvec_free(&provs);
    return r;
}

static int history_cmd(const char *provider, long long days, bool json_out, bool redact_on) {
    int64_t now = now_ms(), since = now - days * 86400000LL;
    json_t *entries = usage_history(provider, since), *e;
    redact_t red = {0};
    if (redact_on) {
        strvec_t ids = {0};
        json_arr_foreach(entries, i, e) {
            if (json_get_str(e, "email")) strvec_push(&ids, json_get_str(e, "email"));
            if (json_get_str(e, "accountId")) strvec_push(&ids, json_get_str(e, "accountId"));
            if (json_get_str(e, "accountKey")) strvec_push(&ids, json_get_str(e, "accountKey"));
        }
        redact_build(&red, &ids);
        strvec_free(&ids);
    }
    int rc = 0;
    if (json_out) {
        if (redact_on) {
            json_arr_foreach(entries, i, e) {
                mask_field(e, "accountKey", &red);
                mask_field(e, "email", &red);
                mask_field(e, "accountId", &red);
            }
        }
        json_t *doc = json_obj();
        json_set_int(doc, "generatedAt", now);
        json_set_int(doc, "sinceMs", since);
        json_set(doc, "entries", entries);
        entries = NULL;
        char *s = cli_json_pretty(doc);
        printf("%s\n", s);
        free(s);
        json_free(doc);
    } else if (json_len(entries) == 0) {
        char *scope = provider ? xasprintf(" for provider \"%s\"", provider) : xstrdup("");
        char *m = xasprintf("No usage history recorded%s yet. Snapshots accumulate whenever usage is fetched (TUI footer, /usage, omc usage).\n", scope);
        warn_out(m);
        free(m), free(scope);
        rc = 1;
    } else {
        char *s = format_history(entries, since, now, redact_on ? &red : NULL);
        printf("%s\n", s);
        free(s);
    }
    json_free(entries);
    redact_free(&red);
    return rc;
}

/* ---------- accounts / reset ---------- */

static char *identity_key(const char *provider, const json_t *c) {
    char *email = NULL;
    if (json_get_str(c, "email")) {
        char *t = str_trim(json_get_str(c, "email"));
        email = str_lower(t);
        free(t);
        if (!*email) free(email), email = NULL;
    }
    const char *acc = json_get_str(c, "accountId"), *proj = json_get_str(c, "projectId"), *org = json_get_str(c, "orgId");
    acc = acc && *acc ? acc : NULL, proj = proj && *proj ? proj : NULL, org = org && *org ? org : NULL;
    char *r = NULL;
    if (str_eq(provider, "anthropic") || str_eq(provider, "openai-codex") || str_eq(provider, "factory-droid")) {
        char *base = email ? xasprintf("email:%s", email) : acc ? xasprintf("account:%s", acc) : proj ? xasprintf("project:%s", proj) : NULL;
        if (base) r = org ? xasprintf("%s|org:%s", base, org) : xstrdup(base);
        else if (org) r = xasprintf("org:%s", org);
        free(base);
    } else if (acc) r = xasprintf("account:%s", acc);
    else if (email) r = xasprintf("email:%s", email);
    else if (proj) r = xasprintf("project:%s", proj);
    free(email);
    return r;
}

static int accounts_cmd(const char *provider, bool json_out) {
    json_t *all = auth_list(), *e, *rows = json_arr();
    json_arr_foreach(all, i, e) {
        const char *p = json_get_str(e, "provider");
        if (!str_eq(json_get_str(e, "type"), "oauth") || (provider && strcasecmp(provider, p))) continue;
        json_t *c = auth_get_credential_raw(p);
        json_t *row = json_obj();
        json_set_str(row, "provider", p);
        char *k = identity_key(p, c);
        if (k) json_set_str(row, "identityKey", k);
        else json_set(row, "identityKey", json_null());
        free(k);
        if (json_get_str(c, "orgName") && *json_get_str(c, "orgName")) json_set_str(row, "orgName", json_get_str(c, "orgName"));
        json_push(rows, row);
        json_free(c);
    }
    json_free(all);
    int rc = 0;
    if (json_out) {
        json_t *doc = json_obj();
        json_set(doc, "accounts", rows);
        rows = NULL;
        char *s = cli_json_pretty(doc);
        printf("%s\n", s);
        free(s);
        json_free(doc);
    } else if (json_len(rows) == 0) {
        char *m = provider ? xasprintf("No OAuth accounts found for provider \"%s\". Run `omc` and use /login to add accounts.\n", provider)
                           : xstrdup("No OAuth accounts found. Run `omc` and use /login to add accounts.\n");
        warn_out(m);
        free(m);
        rc = 1;
    } else {
        size_t width = 0;
        json_t *row;
        json_arr_foreach(rows, i, row) width = MAX(width, json_get_str(row, "identityKey") ? strlen(json_get_str(row, "identityKey")) : 0);
        const char *cur = NULL;
        sb_t b = SB_INIT;
        json_arr_foreach(rows, i, row) {
            const char *p = json_get_str(row, "provider");
            if (!str_eq(cur, p)) {
                cur = p;
                char *bp = cli_bold(p);
                sb_appendf(&b, "%s%s", b.len ? "\n" : "", bp);
                free(bp);
            }
            const char *k = json_get_str(row, "identityKey");
            if (!k) {
                char *d = cli_dim("  (no identity key: no pool can name this account)");
                sb_appendf(&b, "\n%s", d);
                free(d);
                continue;
            }
            if (json_get_str(row, "orgName")) {
                char *o = usage_one_line(json_get_str(row, "orgName")), *d = cli_dim(o);
                sb_appendf(&b, "\n  %-*s  %s", (int)width, k, d);
                free(o), free(d);
            } else sb_appendf(&b, "\n  %s", k);
        }
        printf("%s\n", sb_cstr(&b));
        sb_free(&b);
    }
    json_free(rows);
    return rc;
}

static int fail_reset(const char *msg) {
    char *m = xasprintf("%s\n", msg), *r = cli_red(m);
    fputs(r, stderr);
    free(m), free(r);
    return 1;
}

#define RESET_COMMAND "omc usage reset"

static int reset_cmd(const char *target, const char *provider, bool json_out) {
    bool want_codex = true, want_claude = true;
    if (target) {
        char *t = str_trim(target);
        char *slash = strchr(t, '/');
        if (!slash || slash == t) {
            free(t);
            return fail_reset("Choose an account with `" RESET_COMMAND " <provider>/<credential id>`.");
        }
        *slash = 0;
        char *lo = str_lower(t);
        bool codex = !strcmp(lo, "codex") || !strcmp(lo, "openai-codex"), claude = !strcmp(lo, "claude") || !strcmp(lo, "anthropic");
        free(lo);
        if (!codex && !claude) {
            char *o = usage_one_line(t), *m = xasprintf("Unknown reset provider \"%s\". Use anthropic or openai-codex.", o);
            int rc = fail_reset(m);
            free(o), free(m), free(t);
            return rc;
        }
        char *acct = str_trim(slash + 1), *la = str_lower(acct);
        bool active = !strcmp(la, "active");
        free(acct), free(la), free(t);
        if (active)
            return fail_reset("`" RESET_COMMAND "` runs outside a session, so no account is active. Name one by the credential id `" RESET_COMMAND "` lists.");
        want_codex = codex, want_claude = claude;
    } else if (provider) {
        char *lo = str_lower(provider);
        bool codex = !strcmp(lo, "codex") || !strcmp(lo, "openai-codex"), claude = !strcmp(lo, "claude") || !strcmp(lo, "anthropic");
        free(lo);
        if (!codex && !claude) return fail_reset("Saved resets exist only for Codex and Claude accounts. Use --provider openai-codex or anthropic.");
        want_codex = codex, want_claude = claude;
    }
    json_t *statuses = want_codex && want_claude ? usage_list_reset_credits(NULL) : usage_list_reset_credits_for(want_codex ? "openai-codex" : "anthropic", NULL);
    json_t *accounts = usage_reset_accounts(statuses);
    int rc = 0;
    if (!target) {
        if (json_out) {
            json_t *doc = json_obj(), *rows = json_arr(), *st;
            json_set_int(doc, "generatedAt", now_ms());
            json_arr_foreach(statuses, i, st) {
                const json_t *acc = NULL, *a;
                json_arr_foreach(accounts, k, a) if (json_get_int(json_get_obj(a, "target"), "credentialId", -1) == json_get_int(st, "credentialId", -2) &&
                                                     str_eq(json_get_str(a, "provider"), json_get_str(st, "provider"))) acc = a;
                json_t *row = json_obj();
                json_set_str(row, "provider", json_get_str(st, "provider"));
                json_set_int(row, "credentialId", json_get_int(st, "credentialId", 0));
                static const char *const ik[] = {"email", "accountId", "orgId", "orgName"};
                for (size_t k = 0; k < ARRAY_LEN(ik); k++)
                    if (json_get_str(st, ik[k])) json_set_str(row, ik[k], json_get_str(st, ik[k]));
                json_set_int(row, "availableCount", json_get_int(acc, "availableCount", 0));
                json_set_int(row, "redeemableCount", json_get_int(acc, "redeemableCount", 0));
                if (json_get_str(st, "nextCreditId")) json_set_str(row, "nextCreditId", json_get_str(st, "nextCreditId"));
                if (json_get_str(acc, "expiresAt")) json_set_str(row, "soonestExpiry", json_get_str(acc, "expiresAt"));
                if (json_get_str(acc, "unavailableReason")) json_set_str(row, "unavailableReason", json_get_str(acc, "unavailableReason"));
                if (json_get_str(acc, "error")) json_set_str(row, "error", json_get_str(acc, "error"));
                json_t *credits = json_arr(), *c;
                json_arr_foreach(json_get_arr(st, "credits"), k, c) {
                    json_t *x = json_obj();
                    static const char *const ck[] = {"id", "title", "program", "status"};
                    for (size_t q = 0; q < ARRAY_LEN(ck); q++)
                        if (json_get_str(c, ck[q])) json_set_str(x, ck[q], json_get_str(c, ck[q]));
                    if (json_get_str(c, "grantedAt")) json_set_str(x, "grantedAt", json_get_str(c, "grantedAt"));
                    if (json_get_str(c, "expiresAt")) json_set_str(x, "expiresAt", json_get_str(c, "expiresAt"));
                    json_push(credits, x);
                }
                json_set(row, "credits", credits);
                json_push(rows, row);
            }
            json_set(doc, "accounts", rows);
            char *s = cli_json_pretty(doc);
            printf("%s\n", s);
            free(s);
            json_free(doc);
        } else if (json_len(accounts) == 0) {
            const char *names = want_codex && want_claude ? "Codex or Claude" : want_codex ? "Codex" : "Claude";
            char *m = xasprintf("No %s accounts found. Run `omc` and use /login to add one.\n", names);
            warn_out(m);
            free(m);
            rc = 1;
        } else {
            printf("Saved rate-limit resets:\n");
            json_t *a;
            json_arr_foreach(accounts, i, a) {
                char *l = usage_reset_account_line(a);
                printf("%s\n", l);
                free(l);
            }
            printf("\nSpend one with `" RESET_COMMAND " <provider>/<credential id>`.\n");
        }
    } else {
        char *err = NULL;
        const json_t *acc = usage_resolve_reset_target(accounts, target, RESET_COMMAND, &err);
        if (!acc) rc = fail_reset(err);
        else {
            json_t *outcome = usage_redeem_reset_credit(json_get_obj(acc, "target"));
            char *msg = usage_describe_redeem(outcome, json_get_str(acc, "label")), *one = usage_one_line(msg);
            if (json_get_bool(outcome, "ok", false)) printf("%s\n", one);
            else rc = fail_reset(one);
            free(msg), free(one);
            json_free(outcome);
        }
        free(err);
    }
    json_free(accounts);
    json_free(statuses);
    return rc;
}

/* ---------- entry ---------- */

int cmd_usage(cparse_t *p) {
    cli_init_runtime(NULL);
    const char *action = cp_arg(p, "action"), *target = cp_arg(p, "target"), *provider_in = cp_str(p, "provider");
    bool json_out = cp_bool(p, "json"), redact_on = cp_bool(p, "redact");
    long long days = 7;
    if (!cp_int(p, "days", &days) || days <= 0) days = 7;
    char *provider = provider_in ? str_lower(provider_in) : NULL;
    int rc = 0;
    if (str_eq(action, "invalidate")) {
        if (provider) {
            json_t *stored = usage_stored_accounts(provider);
            if (!usage_provider_for(provider) && json_len(stored) == 0) {
                json_t *all = usage_stored_accounts(NULL);
                char *m = no_provider_credentials(provider, all);
                warn_out(m);
                free(m);
                json_free(all);
                json_free(stored);
                free(provider);
                return 1;
            }
            json_free(stored);
        }
        usage_invalidate(provider);
        if (provider) printf("Invalidated cached usage reports for provider \"%s\".\n", provider);
        else printf("Invalidated cached usage reports for all providers.\n");
    } else if (str_eq(action, "accounts")) {
        if (redact_on) {
            char *m = cli_red("`omc usage accounts` prints identity keys verbatim for configuration; --redact does not apply.\n");
            fputs(m, stderr);
            free(m);
            rc = 1;
        } else rc = accounts_cmd(provider, json_out);
    } else if (str_eq(action, "reset")) {
        rc = reset_cmd(target, provider_in, json_out);
    } else if (str_eq(action, "clients")) {
        int64_t now = now_ms(), since = now - days * 86400000LL;
        if (json_out) {
            json_t *doc = json_obj();
            json_set_int(doc, "generatedAt", now);
            json_set_int(doc, "sinceMs", since);
            json_set(doc, "clients", json_arr());
            char *s = cli_json_pretty(doc);
            printf("%s\n", s);
            free(s);
            json_free(doc);
        } else {
            warn_out("No per-client usage recorded yet.\n");
            rc = 1;
        }
    } else if (cp_bool(p, "history")) {
        rc = history_cmd(provider, days, json_out, redact_on);
    } else {
        json_t *reports = usage_fetch_reports(NULL);
        json_t *stored = usage_stored_accounts(NULL), *all_stored = auth_list();
        json_t *accounts = usage_stored_accounts(provider);
        json_t *filtered = json_arr(), *r;
        json_arr_foreach(reports, i, r) if (!provider || !strcasecmp(json_get_str(r, "provider"), provider)) json_push(filtered, json_clone(r));
        redact_t red = {0};
        if (redact_on) {
            strvec_t ids = {0};
            collect_identity(&ids, filtered, accounts);
            redact_build(&red, &ids);
            strvec_free(&ids);
        }
        if (json_out) {
            json_t *doc = json_obj(), *trimmed = json_arr();
            json_set_int(doc, "generatedAt", now_ms());
            json_arr_foreach(filtered, i, r) {
                json_t *c = json_clone(r);
                json_del(c, "raw");
                if (redact_on) {
                    json_t *md = json_get_obj(c, "metadata"), *l;
                    static const char *const mk[] = {"email", "accountId", "projectId", "orgId", "orgName"};
                    for (size_t k = 0; md && k < ARRAY_LEN(mk); k++) mask_field(md, mk[k], &red);
                    json_arr_foreach(json_get_arr(c, "limits"), k, l) {
                        json_t *sc = json_get_obj(l, "scope");
                        mask_field(sc, "accountId", &red);
                        mask_field(sc, "projectId", &red);
                        mask_field(sc, "orgId", &red);
                    }
                }
                json_push(trimmed, c);
            }
            json_set(doc, "reports", trimmed);
            json_t *unrep = usage_unreported_accounts(filtered, accounts), *a;
            json_arr_foreach(unrep, i, a) {
                json_del(a, "credentialId");
                if (redact_on) {
                    static const char *const ak[] = {"email", "accountId", "projectId", "enterpriseUrl", "orgId", "orgName"};
                    for (size_t k = 0; k < ARRAY_LEN(ak); k++) mask_field(a, ak[k], &red);
                }
            }
            json_set(doc, "accountsWithoutUsage", unrep);
            json_set(doc, "disabledCredentials", json_arr());
            json_t *cap = json_obj();
            strvec_t seen = {0};
            json_arr_foreach(filtered, i, r) {
                const char *pv = json_get_str(r, "provider");
                if (strvec_contains(&seen, pv)) continue;
                strvec_push(&seen, pv);
                json_t *peers = json_arr(), *q, *stats = json_arr();
                json_arr_foreach(filtered, k, q) if (str_eq(json_get_str(q, "provider"), pv)) json_push(peers, json_clone(q));
                char *line = capacity_line(peers, stats);
                free(line);
                if (json_len(stats)) json_set(cap, pv, stats);
                else json_free(stats);
                json_free(peers);
            }
            strvec_free(&seen);
            json_set(doc, "capacity", cap);
            char *s = cli_json_pretty(doc);
            printf("%s\n", s);
            free(s);
            json_free(doc);
        } else if (json_len(filtered) == 0 && json_len(accounts) == 0) {
            char *m;
            if (provider) m = no_provider_credentials(provider_in, stored);
            else if (json_len(all_stored) > 0) m = xstrdup("No usage data. Stored credentials are for providers without a usage endpoint.\n");
            else m = xstrdup("No credentials found. Run `omc` and use /login to add accounts.\n");
            warn_out(m);
            free(m);
            rc = 1;
        } else {
            char *s = breakdown(filtered, accounts, now_ms(), redact_on ? &red : NULL);
            printf("%s\n", s);
            free(s);
        }
        redact_free(&red);
        json_free(filtered), json_free(accounts), json_free(stored), json_free(all_stored), json_free(reports);
    }
    free(provider);
    return rc;
}
