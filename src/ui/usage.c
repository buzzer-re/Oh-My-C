/* usage.c - `/usage` in the TUI: the fullscreen provider quota dashboard (port of omp packages/tui/src/overlays/
 * usage-dashboard.ts: subscriptions grid + daily activity heatmap), its classic per-account detail view
 * (coding-agent modes/controllers/command-controller.ts renderUsageReports), the `/usage reset` account picker
 * (overlays/reset-usage-selector.ts) and the selector-controller flows around them.
 *
 * Reports are fetched off the UI thread (config/usage.c) and handed back through tui_post. The activity heatmap
 * scans the session files for per-day assistant requests and cost (omp reads its stats DB, which it syncs from
 * the same session files). */
#define _GNU_SOURCE
#include "ui/ui.h"
#include "ui/usage.h"

#include "coding/slashcmd.h"
#include "config/usage.h"
#include "core/fs.h"
#include "session/trace.h"
#include "tui/color.h"

#include <dirent.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#define CARD_MIN_WIDTH 32
#define CARD_GUTTER 3
#define CARD_MAX_WINDOWS 4
#define CARD_MIN_BAR_WIDTH 12
#define CARD_MAX_LABEL_LINES 2
#define IDLE_FRACTION 0.005
#define BAR_WIDTH_MAX 24
#define COLUMN_WIDTH_MIN 4

static char *S(tui_tc_t slot, const char *t) { return tui_fgs(NULL, slot, t ? t : ""); }

static const char *status_slot_name(const char *st) {
    if (str_eq(st, "exhausted")) return "error";
    if (str_eq(st, "warning")) return "warning";
    if (str_eq(st, "ok")) return "success";
    return "dim";
}

static tui_tc_t status_slot(const char *st) {
    const char *n = status_slot_name(st);
    return !strcmp(n, "error") ? TUI_TC_ERROR : !strcmp(n, "warning") ? TUI_TC_WARNING : !strcmp(n, "success") ? TUI_TC_SUCCESS : TUI_TC_DIM;
}

static char *status_icon(const char *st) {
    if (str_eq(st, "exhausted")) return S(TUI_TC_ERROR, tui_sym("status.error"));
    if (str_eq(st, "warning")) return S(TUI_TC_WARNING, tui_sym("status.warning"));
    if (str_eq(st, "ok")) return S(TUI_TC_SUCCESS, tui_sym("status.success"));
    return S(TUI_TC_DIM, tui_sym("status.info"));
}

static const char *aggregate_status_n(const char *const *st, size_t n) {
    bool ok = false, wa = false, ex = false;
    for (size_t i = 0; i < n; i++) ok |= str_eq(st[i], "ok"), wa |= str_eq(st[i], "warning"), ex |= str_eq(st[i], "exhausted");
    if (ok) return wa || ex ? "warning" : "ok";
    if (wa) return "warning";
    if (ex) return "exhausted";
    return "unknown";
}

static const char *limit_status(const json_t *l) {
    const char *s = json_get_str(l, "status");
    return s ? s : "";
}

static char *one(const char *s) { return usage_one_line(s ? s : ""); }

/* ============================================================================================
 * Subscriptions grid model (buildProviderCards)
 * ============================================================================================ */

typedef struct {
    char *label, *tag;
    bool has_f;
    double f;
    const char *status;
    bool has_reset;
    double reset_ms;
    char *used_text;
} cwin_t;

typedef struct {
    char *provider, *name;
    int accounts;
    strvec_t unavailable;
    VEC(cwin_t) windows;
    bool unlimited, idle;
    strvec_t notes;
    bool has_resets;
    int64_t banked, redeemable;
    bool has_exp;
    double exp_ms;
    strvec_t reasons;
    strvec_t daybreak;
    double worst;
} card_t;

typedef VEC(card_t) cards_t;

static void cards_free(cards_t *c) {
    for (size_t i = 0; i < c->n; i++) {
        card_t *k = &c->v[i];
        free(k->provider), free(k->name);
        strvec_free(&k->unavailable), strvec_free(&k->notes), strvec_free(&k->reasons), strvec_free(&k->daybreak);
        for (size_t w = 0; w < k->windows.n; w++) free(k->windows.v[w].label), free(k->windows.v[w].tag), free(k->windows.v[w].used_text);
        vec_free(&k->windows);
    }
    vec_free(c);
}

/* compactWindowTag */
static char *window_tag(const json_t *w) {
    double d;
    json_t *dm = json_get(w, "durationMs");
    if (json_is_num(dm) && (d = dm->u.num.d) != 0) {
        double h = d / 3600000.0;
        if (h >= 28 * 24) return xstrdup("mo");
        if (h >= 24) return xasprintf("%.0fd", floor(h / 24 + 0.5));
        return xasprintf("%.0fh", floor(h + 0.5));
    }
    char *id = str_lower(json_get_str(w, "id") ? json_get_str(w, "id") : "");
    if (strlen(id) > 3) id[1] = 0;
    return id;
}

static int cmp_card(const void *pa, const void *pb) {
    const card_t *a = pa, *b = pb;
    if (a->worst != b->worst) return b->worst > a->worst ? 1 : -1;
    return strcmp(a->name, b->name);
}

static void build_cards(const json_t *reports, double now, const json_t *unavailable, cards_t *out) {
    json_t *disp = usage_collapse_shared(reports), *r, *ua;
    strvec_t provs = {0};
    json_arr_foreach(disp, i, r) if (!strvec_contains(&provs, json_get_str(r, "provider"))) strvec_push(&provs, json_get_str(r, "provider"));
    json_arr_foreach(unavailable, i, ua) if (!strvec_contains(&provs, json_get_str(ua, "provider"))) strvec_push(&provs, json_get_str(ua, "provider"));
    for (size_t pi = 0; pi < provs.n; pi++) {
        const char *prov = provs.v[pi];
        card_t c = {0};
        c.provider = xstrdup(prov);
        c.name = usage_provider_name(prov);
        json_arr_foreach(unavailable, i, ua) if (str_eq(json_get_str(ua, "provider"), prov)) strvec_push(&c.unavailable, json_get_str(ua, "label"));
        /* buckets: key -> limits */
        strvec_t keys = {0}, labels = {0};
        VEC(json_t *) buckets = {0};
        size_t nreports = 0;
        json_arr_foreach(disp, i, r) {
            if (!str_eq(json_get_str(r, "provider"), prov)) continue;
            nreports++;
            json_t *l;
            json_arr_foreach(json_get_arr(r, "limits"), k, l) {
                char *label = usage_limit_title(l);
                const char *g = json_get_str(json_get_obj(l, "scope"), "sharedGroup");
                const char *wid = json_get_str(json_get_obj(l, "window"), "id");
                if (!wid) wid = json_get_str(json_get_obj(l, "scope"), "windowId");
                char *key = g ? xstrdup(g) : xasprintf("%s|%s|%s", label, wid ? wid : "default", json_get_str(l, "id") ? json_get_str(l, "id") : "");
                size_t bi = keys.n;
                for (size_t q = 0; q < keys.n; q++)
                    if (!strcmp(keys.v[q], key)) bi = q;
                if (bi == keys.n) {
                    strvec_push(&keys, key);
                    strvec_push(&labels, label);
                    vec_push(&buckets, json_arr());
                }
                json_push(buckets.v[bi], json_clone(l));
                free(key);
                free(label);
            }
            const char *n;
            json_t *nn;
            json_arr_foreach(json_get_arr(r, "notes"), k, nn) if ((n = json_sv(nn)) && !strvec_contains(&c.notes, n)) strvec_push(&c.notes, n);
        }
        VEC(cwin_t) declared = {0};
        for (size_t b = 0; b < buckets.n; b++) {
            json_t *lims = buckets.v[b], *l, *worst = NULL;
            double sum = 0, wf = -2;
            size_t nf = 0;
            const char *sts[64];
            size_t ns = 0;
            json_arr_foreach(lims, k, l) {
                double f;
                bool has = usage_used_fraction(l, &f);
                if (has) sum += f, nf++;
                double v = has ? f : -1;
                if (!worst || v > wf) worst = l, wf = v;
                if (ns < 64) sts[ns++] = limit_status(l);
            }
            cwin_t w = {.label = xstrdup(labels.v[b]), .status = aggregate_status_n(sts, ns)};
            if (nf) w.has_f = true, w.f = sum / nf;
            const json_t *ww = json_get_obj(worst, "window");
            if (ww) w.tag = window_tag(ww);
            json_t *ra = json_get(ww, "resetsAt");
            if (json_is_num(ra) && ra->u.num.d > now) w.has_reset = true, w.reset_ms = ra->u.num.d - now;
            if (!w.has_f) {
                const json_t *arr[64];
                size_t na = 0;
                json_arr_foreach(lims, k, l) if (na < 64) arr[na++] = l;
                w.used_text = usage_absolute_only_amount(arr, na);
            }
            vec_push(&declared, w);
        }
        /* the fullest CARD_MAX_WINDOWS rows lead, still in declared order */
        size_t nd = declared.n;
        bool *vis = xcalloc(nd ? nd : 1, sizeof *vis);
        for (size_t take = 0; take < MIN(nd, (size_t)CARD_MAX_WINDOWS); take++) {
            size_t best = nd;
            for (size_t q = 0; q < nd; q++) {
                if (vis[q]) continue;
                double v = declared.v[q].has_f ? declared.v[q].f : -1, bv = best < nd ? (declared.v[best].has_f ? declared.v[best].f : -1) : -INFINITY;
                if (best == nd || v > bv) best = q;
            }
            if (best < nd) vis[best] = true;
        }
        for (size_t q = 0; q < nd; q++)
            if (vis[q]) vec_push(&c.windows, declared.v[q]);
        for (size_t q = 0; q < nd; q++)
            if (!vis[q]) vec_push(&c.windows, declared.v[q]);
        free(vis);
        vec_free(&declared);
        for (size_t q = 0; q < c.windows.n; q++) {
            bool dup = false;
            for (size_t o = 0; o < c.windows.n && !dup; o++) dup = o != q && !strcmp(c.windows.v[o].label, c.windows.v[q].label);
            if (!dup) free(c.windows.v[q].tag), c.windows.v[q].tag = NULL;
        }
        /* saved resets */
        double soonest = INFINITY, latest_exp = -INFINITY;
        bool any_exp = false;
        json_arr_foreach(disp, i, r) {
            if (!str_eq(json_get_str(r, "provider"), prov)) continue;
            usage_reset_summary_t s;
            if (usage_reset_summary(json_get_obj(r, "resetCredits"), (int64_t)now, &s) && s.banked > 0) {
                c.banked += s.banked;
                c.redeemable += s.redeemable;
                double e;
                if (s.soonest_expiry && trace_parse_iso_ms(s.soonest_expiry, &e)) {
                    any_exp = true;
                    if (e > now && e < soonest) soonest = e;
                    latest_exp = MAX(latest_exp, e);
                }
                if (s.unavailable_reason && !strvec_contains(&c.reasons, s.unavailable_reason)) strvec_push(&c.reasons, s.unavailable_reason);
            }
            usage_reset_summary_free(&s);
        }
        if (c.banked > 0) {
            c.has_resets = true;
            if (any_exp) c.has_exp = true, c.exp_ms = (isfinite(soonest) ? soonest : latest_exp) - now;
        }
        size_t idx = 0;
        json_arr_foreach(disp, i, r) {
            if (!str_eq(json_get_str(r, "provider"), prov)) continue;
            const json_t *md = json_get_obj(r, "metadata");
            if (json_get_bool(md, "daybreak", false)) {
                const char *e = json_get_str(md, "email"), *a = json_get_str(md, "accountId");
                char *lab = e && *e ? xstrdup(e) : a && *a ? xstrdup(a) : xasprintf("account %zu", idx + 1);
                strvec_push(&c.daybreak, lab);
                free(lab);
            }
            idx++;
        }
        c.accounts = (int)(nreports + c.unavailable.n);
        c.unlimited = c.windows.n == 0 && c.unavailable.n == 0;
        bool all_idle = true;
        for (size_t q = 0; q < c.windows.n; q++) all_idle &= c.windows.v[q].has_f && c.windows.v[q].f < IDLE_FRACTION;
        c.idle = c.unavailable.n == 0 && !c.has_resets && c.daybreak.n == 0 && all_idle;
        c.worst = -1;
        for (size_t q = 0; q < c.windows.n; q++) c.worst = MAX(c.worst, c.windows.v[q].has_f ? c.windows.v[q].f : -1);
        vec_push(out, c);
        for (size_t b = 0; b < buckets.n; b++) json_free(buckets.v[b]);
        vec_free(&buckets);
        strvec_free(&keys);
        strvec_free(&labels);
    }
    xqsort(out->v, out->n, sizeof *out->v, cmp_card);
    strvec_free(&provs);
    json_free(disp);
}

/* ============================================================================================
 * Activity heatmap (buildHeatmapLayout) + the session scan behind it
 * ============================================================================================ */

static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const DAY_LABELS[] = {"M", "T", "W", "T", "F", "S", "S"};

typedef struct {
    char day[11];
    double cost, requests;
} apoint_t;
typedef VEC(apoint_t) apoints_t;

static void day_of(time_t t, char *buf) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, 11, "%Y-%m-%d", &tm);
}

static time_t add_days(time_t midnight, int d) {
    struct tm tm;
    localtime_r(&midnight, &tm);
    tm.tm_mday += d;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static time_t local_midnight(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static const apoint_t *point_for(const apoints_t *p, const char *day) {
    for (size_t i = 0; i < p->n; i++)
        if (!strcmp(p->v[i].day, day)) return &p->v[i];
    return NULL;
}

typedef struct {
    int weeks;
    char *month_labels[64];
    int cells[7][64]; /* -1 future, 0..4 */
    double total_cost, total_requests;
} heat_t;

static void heat_build(const apoints_t *pts, int weeks, time_t today, heat_t *h) {
    memset(h, 0, sizeof *h);
    h->weeks = weeks = MIN(weeks, 64);
    bool any_cost = false;
    for (size_t i = 0; i < pts->n; i++) any_cost |= pts->v[i].cost > 0;
    time_t today0 = local_midnight(today);
    struct tm tm;
    localtime_r(&today0, &tm);
    int monday_off = (tm.tm_wday + 6) % 7;
    time_t cur_monday = add_days(today0, -monday_off), start = add_days(cur_monday, -(weeks - 1) * 7);
    char start_iso[11], today_iso[11];
    day_of(start, start_iso);
    day_of(today0, today_iso);
    double max = 0;
    for (size_t i = 0; i < pts->n; i++) {
        if (strcmp(pts->v[i].day, start_iso) < 0 || strcmp(pts->v[i].day, today_iso) > 0) continue;
        max = MAX(max, any_cost ? pts->v[i].cost : pts->v[i].requests);
        h->total_cost += pts->v[i].cost;
        h->total_requests += pts->v[i].requests;
    }
    int prev_month = -1;
    for (int w = 0; w < weeks; w++) {
        time_t ws = add_days(start, w * 7);
        localtime_r(&ws, &tm);
        h->month_labels[w] = tm.tm_mon != prev_month ? (char *)MONTHS[tm.tm_mon] : NULL;
        prev_month = tm.tm_mon;
        for (int d = 0; d < 7; d++) {
            time_t dt = add_days(ws, d);
            if (dt > today0) {
                h->cells[d][w] = -1;
                continue;
            }
            char iso[11];
            day_of(dt, iso);
            const apoint_t *p = point_for(pts, iso);
            double v = p ? (any_cost ? p->cost : p->requests) : 0;
            int lvl = 0;
            if (v > 0 && max > 0) lvl = MIN(4, MAX(1, (int)ceil(sqrt(v / max) * 4)));
            h->cells[d][w] = lvl;
        }
    }
}

/* one assistant request per assistant message entry (model_usage entries too), deduped by entry id */
static void scan_session_file(const char *path, hmap_t *seen, apoints_t *out, double cutoff) {
    size_t n = 0;
    char *text = fs_read(path, &n);
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strstr(line, "\"assistant\"") && !strstr(line, "\"model_usage\"")) continue;
        json_t *e = json_parse_str(line);
        const char *type = json_get_str(e, "type"), *id = json_get_str(e, "id");
        const json_t *m = json_get_obj(e, "message"), *usage = NULL;
        double ts = 0;
        if (str_eq(type, "message") && str_eq(json_get_str(m, "role"), "assistant")) {
            usage = json_get_obj(m, "usage");
            ts = json_get_num(m, "timestamp", 0);
        } else if (str_eq(type, "model_usage")) usage = json_get_obj(e, "usage");
        if ((!usage && !str_eq(type, "message")) || (id && hmap_has(seen, id))) {
            json_free(e);
            continue;
        }
        if (!(ts > 0) && json_get_str(e, "timestamp")) trace_parse_iso_ms(json_get_str(e, "timestamp"), &ts);
        if (ts >= cutoff && (str_eq(type, "model_usage") || m)) {
            if (id) hmap_put(seen, id, NULL);
            char day[11];
            day_of((time_t)(ts / 1000), day);
            apoint_t *p = (apoint_t *)point_for(out, day);
            if (!p) {
                apoint_t np = {.cost = 0};
                memcpy(np.day, day, sizeof np.day);
                vec_push(out, np);
                p = &out->v[out->n - 1];
            }
            p->requests += 1;
            p->cost += json_get_num(json_get_obj(usage, "cost"), "total", 0);
        }
        json_free(e);
    }
    free(text);
}

static void scan_dir(const char *dir, int depth, hmap_t *seen, apoints_t *out, double cutoff, volatile int *abort_flag) {
    if (depth > 4 || (abort_flag && *abort_flag)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *p = path_join(dir, de->d_name);
        struct stat st;
        if (stat(p, &st) == 0) {
            if (S_ISDIR(st.st_mode)) scan_dir(p, depth + 1, seen, out, cutoff, abort_flag);
            else if (str_ends(de->d_name, ".jsonl") && st.st_mtime * 1000.0 >= cutoff) scan_session_file(p, seen, out, cutoff);
        }
        free(p);
    }
    closedir(d);
}

static int cmp_point(const void *a, const void *b) { return strcmp(((const apoint_t *)a)->day, ((const apoint_t *)b)->day); }

void ui_usage_load_activity(const char *sessions_dir, json_t **points_out) {
    apoints_t pts = {0};
    hmap_t seen;
    hmap_init(&seen);
    double cutoff = (double)now_ms() - 371.0 * 86400000.0;
    scan_dir(sessions_dir, 0, &seen, &pts, cutoff, NULL);
    hmap_free(&seen, NULL);
    xqsort(pts.v, pts.n, sizeof *pts.v, cmp_point);
    json_t *arr = json_arr();
    for (size_t i = 0; i < pts.n; i++) {
        json_t *o = json_obj();
        json_set_str(o, "day", pts.v[i].day);
        json_set_num(o, "cost", pts.v[i].cost);
        json_set_num(o, "requests", pts.v[i].requests);
        json_push(arr, o);
    }
    vec_free(&pts);
    *points_out = arr;
}

/* ============================================================================================
 * Classic detail report (command-controller.ts renderUsageReports)
 * ============================================================================================ */

static char *truncate_label(const char *s, int maxw) {
    if (tui_visible_width(s) <= maxw) return xstrdup(s);
    if (maxw <= 1) return xstrdup("\xe2\x80\xa6");
    return tui_truncate_to_width(s, maxw, "\xe2\x80\xa6");
}

static char *pad_col(const char *s, int width) {
    int w = tui_visible_width(s);
    return w >= width ? xstrdup(s) : xasprintf("%s%*s", s, width - w, "");
}

static const char *meta(const json_t *r, const char *k) {
    const char *v = json_get_str(json_get_obj(r, "metadata"), k);
    return v && *v ? v : NULL;
}

static char *codex_plan(const json_t *r) {
    if (!str_eq(json_get_str(r, "provider"), "openai-codex") || !meta(r, "planType")) return NULL;
    char *t = str_trim(meta(r, "planType"));
    if (!*t) {
        free(t);
        return NULL;
    }
    char *o = one(t);
    free(t);
    return o;
}

static char *codex_tui_label(const json_t *r, const json_t *peers, const char *base) {
    char *id = usage_codex_label(r, peers, base, false), *plan = codex_plan(r);
    char *o = plan ? xasprintf("%s (%s)", id, plan) : xstrdup(id);
    free(id), free(plan);
    return o;
}

static char *org_suffix(const json_t *r) {
    const char *org = meta(r, "orgName") ? meta(r, "orgName") : json_get_str(json_get_obj(r, "metadata"), "orgId");
    return org ? xasprintf(" (%s)", org) : xstrdup("");
}

static char *account_label(const json_t *limit, const json_t *r, const json_t *peers, size_t index) {
    bool codex = str_eq(json_get_str(r, "provider"), "openai-codex");
    const json_t *sc = json_get_obj(limit, "scope");
    const char *base = meta(r, "email");
    if (!base) base = meta(r, "accountId");
    if (!base && json_get_str(sc, "accountId") && *json_get_str(sc, "accountId")) base = json_get_str(sc, "accountId");
    if (base) {
        if (codex) return codex_tui_label(r, peers, base);
        char *o = org_suffix(r), *x = xasprintf("%s%s", base, o);
        free(o);
        return x;
    }
    const char *proj = meta(r, "projectId");
    if (!proj && json_get_str(sc, "projectId") && *json_get_str(sc, "projectId")) proj = json_get_str(sc, "projectId");
    char *b = proj ? xstrdup(proj) : xasprintf("account %zu", index + 1);
    if (codex) {
        char *x = codex_tui_label(r, peers, b);
        free(b);
        return x;
    }
    return b;
}

static char *usage_bar(const json_t *l, int width) {
    const json_t *a = json_get_obj(l, "amount");
    if (json_is_num(json_get(a, "used")) && usage_used_only_amount(l)) {
        double used = json_get_num(a, "used", 0);
        const char *unit = json_get_str(a, "unit");
        char *u;
        if (str_eq(unit, "usd")) u = xasprintf("$%.2f used", used);
        else {
            char *nn = usage_fmt_grouped(used, 2);
            u = xasprintf("%s %s used", nn, unit);
            free(nn);
        }
        char *t = truncate_label(u, width), *r = S(TUI_TC_DIM, t);
        free(u), free(t);
        return r;
    }
    double f;
    if (!usage_used_fraction(l, &f)) {
        sb_t b = SB_INIT;
        for (int i = 0; i < width; i++) sb_append(&b, "\xc2\xb7");
        char *r = S(TUI_TC_DIM, sb_cstr(&b));
        sb_free(&b);
        return r;
    }
    double c = MIN(MAX(f, 0.0), 1.0), exact = c * width, rem;
    int full = (int)floor(exact);
    rem = exact - full;
    const char *partial = rem >= 2.0 / 3 ? "\xe2\x96\x93" : rem >= 1.0 / 3 ? "\xe2\x96\x92" : "";
    sb_t lead = SB_INIT, empty = SB_INIT;
    for (int i = 0; i < full; i++) sb_append(&lead, "\xe2\x96\x88");
    sb_append(&lead, partial);
    for (int i = 0; i < MAX(0, width - full - (*partial ? 1 : 0)); i++) sb_append(&empty, "\xe2\x96\x91");
    char *a1 = S(status_slot(limit_status(l)), sb_cstr(&lead)), *a2 = S(TUI_TC_DIM, sb_cstr(&empty));
    char *r = xasprintf("%s%s", a1, a2);
    free(a1), free(a2);
    sb_free(&lead), sb_free(&empty);
    return r;
}

static char *aggregate_amount(const json_t *const *lims, size_t n) {
    double sum = 0;
    size_t nf = 0;
    for (size_t i = 0; i < n; i++) {
        double f;
        if (usage_used_fraction(lims[i], &f)) sum += f, nf++;
    }
    if (nf == n && n > 0) {
        double avg = MAX(0.0, ((double)n - sum) / n * 100);
        char *x = usage_fmt_grouped(avg, 1), *r = xasprintf("%s%% free", x);
        free(x);
        return r;
    }
    size_t na = 0;
    double tu = 0, tl = 0;
    for (size_t i = 0; i < n; i++) {
        const json_t *a = json_get_obj(lims[i], "amount");
        if (json_is_num(json_get(a, "used")) && json_is_num(json_get(a, "limit")) && json_get_num(a, "limit", 0) > 0)
            na++, tu += json_get_num(a, "used", 0), tl += json_get_num(a, "limit", 0);
    }
    if (na == n && n > 0) {
        double rem = tl > 0 ? MAX(0.0, 100 - tu / tl * 100) : 0;
        char *x = usage_fmt_grouped(rem, 1), *r = xasprintf("%s%% free", x);
        free(x);
        return r;
    }
    bool all_used = n > 0;
    for (size_t i = 0; i < n; i++) all_used &= usage_used_only_amount(lims[i]);
    if (all_used) return xstrdup("");
    char *rem = usage_remaining_only_total(lims, n);
    if (rem) return rem;
    strvec_t ids = {0};
    for (size_t i = 0; i < n; i++) {
        const char *a = json_get_str(json_get_obj(lims[i], "scope"), "accountId");
        if (a && *a && !strvec_contains(&ids, a)) strvec_push(&ids, a);
    }
    char *r = ids.n ? xasprintf("%zu %s", ids.n, ids.n == 1 ? "acct" : "accts") : xasprintf("%zu accts", n);
    strvec_free(&ids);
    return r;
}

static char *reset_range(const json_t *const *lims, size_t n, double now) {
    double mn = INFINITY, mx = -INFINITY;
    strvec_t verbs = {0};
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        const json_t *w = json_get_obj(lims[i], "window");
        json_t *ra = json_get(w, "resetsAt");
        if (!json_is_num(ra) || !isfinite(ra->u.num.d) || ra->u.num.d <= now) continue;
        cnt++;
        double off = ra->u.num.d - now;
        mn = MIN(mn, off), mx = MAX(mx, off);
        const char *v = json_get_str(w, "resetLabel") ? json_get_str(w, "resetLabel") : "resets";
        if (!strvec_contains(&verbs, v)) strvec_push(&verbs, v);
    }
    if (!cnt) {
        strvec_free(&verbs);
        return NULL;
    }
    const char *verb = verbs.n == 1 ? verbs.v[0] : "resets";
    char *a = usage_fmt_duration(mn), *r;
    if (mx - mn > 60000) {
        char *b = usage_fmt_duration(mx);
        r = xasprintf("%s in %s\xe2\x80\x93%s", verb, a, b);
        free(b);
    } else r = xasprintf("%s in %s", verb, a);
    free(a);
    strvec_free(&verbs);
    return r;
}

typedef struct {
    char *key, *label, *window_label;
    VEC(const json_t *) limits;
    VEC(const json_t *) reports;
} lgroup_t;

static int resolve_column_width(int count, int available, int trailing) {
    if (count <= 0) return BAR_WIDTH_MAX;
    int space = available - 2 - (count - 1) - (trailing > 0 ? trailing + 1 : 0);
    int ideal = (int)floor((double)space / count);
    return ideal < COLUMN_WIDTH_MIN ? COLUMN_WIDTH_MIN : ideal;
}

typedef struct {
    const json_t *r;
    double total;
    const char *provider;
} pentry_t;

static int cmp_pentry(const void *pa, const void *pb) {
    const pentry_t *a = pa, *b = pb;
    if (a->total != b->total) return a->total < b->total ? -1 : 1;
    return strcmp(a->provider, b->provider);
}

static char *active_label(const json_t *id) {
    if (!id) return NULL;
    const char *base = json_get_str(id, "email");
    if (!base || !*base) base = json_get_str(id, "accountId");
    if (!base || !*base) base = json_get_str(id, "projectId");
    if (!base || !*base) return NULL;
    const char *org = json_get_str(id, "orgName");
    if (!org || !*org) org = json_get_str(id, "orgId");
    return org && *org && strcmp(org, base) ? xasprintf("%s (%s)", base, org) : xstrdup(base);
}

char *ui_usage_detail(const json_t *reports, double now, int width, const char *active_provider, const json_t *identity,
                      const json_t *selectors, const json_t *unavailable) {
    json_t *disp = usage_collapse_shared(reports), *r, *ua;
    strvec_t lines = {0};
    double latest = 0;
    json_arr_foreach(reports, i, r) latest = MAX(latest, json_get_num(r, "fetchedAt", 0));
    char *head;
    if (latest > 0) {
        char *d = usage_fmt_duration(now - latest);
        head = xasprintf("Usage (%s ago)", d);
        free(d);
    } else head = xstrdup("Usage");
    char *ha = S(TUI_TC_ACCENT, head), *hb = tui_bold(ha);
    strvec_push(&lines, hb);
    free(head), free(ha), free(hb);
    strvec_t provs = {0};
    json_arr_foreach(disp, i, r) if (!strvec_contains(&provs, json_get_str(r, "provider"))) strvec_push(&provs, json_get_str(r, "provider"));
    json_arr_foreach(unavailable, i, ua) if (!strvec_contains(&provs, json_get_str(ua, "provider"))) strvec_push(&provs, json_get_str(ua, "provider"));
    pentry_t *pe = xcalloc(provs.n ? provs.n : 1, sizeof *pe);
    for (size_t i = 0; i < provs.n; i++) {
        pe[i].provider = provs.v[i];
        json_arr_foreach(disp, k, r) {
            if (!str_eq(json_get_str(r, "provider"), provs.v[i])) continue;
            json_t *l;
            json_arr_foreach(json_get_arr(r, "limits"), q, l) {
                double f;
                if (usage_used_fraction(l, &f)) pe[i].total += f;
            }
        }
    }
    xqsort(pe, provs.n, sizeof *pe, cmp_pentry);
    for (size_t pi = 0; pi < provs.n; pi++) {
        const char *prov = pe[pi].provider;
        json_t *peers = json_arr();
        VEC(const json_t *) pr = {0};
        json_arr_foreach(disp, i, r) if (str_eq(json_get_str(r, "provider"), prov)) vec_push(&pr, r);
        json_arr_foreach(disp, i, r) if (str_eq(json_get_str(r, "provider"), prov)) json_push(peers, json_clone(r));
        /* peers as clones lose identity; map indices */
        strvec_push(&lines, "");
        const json_t *active = str_eq(prov, active_provider) ? identity : NULL;
        /* groups */
        VEC(lgroup_t) groups = {0};
        for (size_t ri = 0; ri < pr.n; ri++) {
            json_t *l;
            json_arr_foreach(json_get_arr(pr.v[ri], "limits"), k, l) {
                const char *wid = json_get_str(json_get_obj(l, "window"), "id");
                if (!wid) wid = json_get_str(json_get_obj(l, "scope"), "windowId");
                if (!wid) wid = "default";
                char *title = usage_limit_title(l);
                const char *g = json_get_str(json_get_obj(l, "scope"), "sharedGroup");
                char *key = g ? xstrdup(g) : xasprintf("%s|%s|%s", title, wid, json_get_str(l, "id") ? json_get_str(l, "id") : "");
                size_t gi = groups.n;
                for (size_t q = 0; q < groups.n; q++)
                    if (!strcmp(groups.v[q].key, key)) gi = q;
                if (gi == groups.n) {
                    const char *wl = json_get_str(json_get_obj(l, "window"), "label");
                    lgroup_t ng = {.key = xstrdup(key), .label = xstrdup(title), .window_label = xstrdup(wl ? wl : wid)};
                    vec_push(&groups, ng);
                }
                vec_push(&groups.v[gi].limits, l);
                vec_push(&groups.v[gi].reports, pr.v[ri]);
                free(key), free(title);
            }
        }
        char *pn = usage_provider_name(prov), *pa = S(TUI_TC_ACCENT, pn), *pb = tui_bold(pa);
        strvec_push(&lines, pb);
        free(pn), free(pa), free(pb);
        /* in use by this session */
        char *al = NULL;
        size_t active_report = (size_t)-1;
        if (str_eq(prov, "openai-codex") && active) {
            const char *aacc = json_get_str(active, "accountId"), *aemail = json_get_str(active, "email"), *aorg = json_get_str(active, "orgId");
            for (size_t ri = 0; ri < pr.n && active_report == (size_t)-1; ri++)
                if (aacc && str_eq(meta(pr.v[ri], "orgId"), aorg) && str_eq(meta(pr.v[ri], "accountId"), aacc)) active_report = ri;
            for (size_t ri = 0; ri < pr.n && active_report == (size_t)-1; ri++)
                if (str_eq(meta(pr.v[ri], "orgId"), aorg) && aemail && *aemail && str_eq(meta(pr.v[ri], "email"), aemail) &&
                    (!aacc || !meta(pr.v[ri], "accountId")))
                    active_report = ri;
            const char *base = aemail && *aemail ? aemail : aacc && *aacc ? aacc : "account";
            if (active_report != (size_t)-1) al = codex_tui_label(json_at(peers, active_report), peers, base);
            else {
                const char *b2 = aemail && *aemail ? aemail : aacc && *aacc ? aacc : json_get_str(active, "projectId");
                al = b2 && *b2 ? xstrdup(b2) : NULL;
            }
        } else al = active_label(active);
        if (al) {
            char *k = S(TUI_TC_ACCENT, "in use by this session:"), *l2 = xasprintf("  %s %s", k, al);
            strvec_push(&lines, l2);
            free(k), free(l2);
        }
        free(al);
        /* models with usage data */
        bool any_model = false;
        json_t *sel;
        char *pfx = xasprintf("%s/", prov);
        json_arr_foreach(selectors, i, sel) {
            if (!str_starts(json_sv(sel), pfx)) continue;
            if (!any_model) {
                char *k = S(TUI_TC_ACCENT, "Models with usage data"), *l2 = xasprintf("  %s", k);
                strvec_push(&lines, l2);
                free(k), free(l2);
                any_model = true;
            }
            char *o = one(json_sv(sel)), *t = tui_truncate_to_width(o, width - 4, NULL), *l2 = xasprintf("    %s", t);
            strvec_push(&lines, l2);
            free(o), free(t), free(l2);
        }
        free(pfx);
        json_arr_foreach(unavailable, i, ua) {
            if (!str_eq(json_get_str(ua, "provider"), prov)) continue;
            char *lab = one(json_get_str(ua, "label"));
            const char *st = " \xe2\x80\x94 usage unavailable";
            char *bl = tui_truncate_to_width(lab, MAX(0, width - 2 - tui_visible_width(st)), NULL);
            char *full = xasprintf("%s%s", bl, st), *tt = tui_truncate_to_width(full, width - 2, NULL), *d = S(TUI_TC_DIM, tt), *l2 = xasprintf("  %s", d);
            strvec_push(&lines, l2);
            free(lab), free(bl), free(full), free(tt), free(d), free(l2);
        }
        /* provider notes */
        strvec_t notes = {0};
        for (size_t ri = 0; ri < pr.n; ri++) {
            json_t *n;
            json_arr_foreach(json_get_arr(pr.v[ri], "notes"), k, n) if (json_sv(n) && !strvec_contains(&notes, json_sv(n))) strvec_push(&notes, json_sv(n));
        }
        if (notes.n) {
            char *j = strvec_join(&notes, " \xe2\x80\xa2 "), *o = one(j), *t = tui_truncate_to_width(o, 110, NULL), *d = S(TUI_TC_DIM, t), *l2 = xasprintf("  %s", d);
            strvec_push(&lines, l2);
            free(j), free(o), free(t), free(d), free(l2);
        }
        strvec_free(&notes);
        /* saved resets */
        strvec_t rlines = {0};
        for (size_t ri = 0; ri < pr.n; ri++) {
            usage_reset_summary_t rs;
            if (!usage_reset_summary(json_get_obj(pr.v[ri], "resetCredits"), (int64_t)now, &rs) || rs.banked <= 0) {
                usage_reset_summary_free(&rs);
                continue;
            }
            const char *idl = meta(pr.v[ri], "email") ? meta(pr.v[ri], "email") : meta(pr.v[ri], "accountId") ? meta(pr.v[ri], "accountId") : "account";
            const char *org = meta(pr.v[ri], "orgName") ? meta(pr.v[ri], "orgName") : json_get_str(json_get_obj(pr.v[ri], "metadata"), "orgId");
            char *raw = str_eq(prov, "openai-codex") ? codex_tui_label(json_at(peers, ri), peers, idl)
                        : org && strcmp(org, idl) ? xasprintf("%s (%s)", idl, org) : xstrdup(idl);
            char *label = one(raw);
            free(raw);
            bool is_active;
            if (str_eq(prov, "openai-codex")) is_active = active_report == ri;
            else {
                const char *ao = json_get_str(active, "orgId"), *ro = json_get_str(json_get_obj(pr.v[ri], "metadata"), "orgId");
                bool orgm = (!ao && !ro) || str_eq(ao, ro);
                is_active = orgm && active &&
                            ((json_get_str(active, "accountId") && str_eq(json_get_str(active, "accountId"), meta(pr.v[ri], "accountId"))) ||
                             (json_get_str(active, "email") && str_eq(json_get_str(active, "email"), meta(pr.v[ri], "email"))));
            }
            char *avail = rs.redeemable == rs.banked ? xstrdup("") : xasprintf(" \xc2\xb7 %lld usable now", (long long)rs.redeemable);
            char *l2 = xasprintf("    \xe2\x80\xa2 %s: %lld saved reset%s%s%s", label, (long long)rs.banked, rs.banked == 1 ? "" : "s", avail,
                                 is_active ? " (active)" : "");
            strvec_push(&rlines, l2);
            free(l2), free(avail), free(label);
            if (rs.soonest_expiry) {
                double e;
                if (trace_parse_iso_ms(rs.soonest_expiry, &e) && e - now > 0) {
                    char *d = usage_fmt_duration(e - now);
                    l2 = xasprintf("        soonest expires in %s (%.10s)", d, rs.soonest_expiry);
                    free(d);
                } else l2 = xasprintf("        expired (%.10s)", rs.soonest_expiry);
                strvec_push(&rlines, l2);
                free(l2);
            }
            if (rs.redeemable == 0 && rs.unavailable_reason) {
                char *o = one(rs.unavailable_reason);
                l2 = xasprintf("        unavailable: %s", o);
                strvec_push(&rlines, l2);
                free(o), free(l2);
            }
            usage_reset_summary_free(&rs);
        }
        if (rlines.n) {
            char *k = S(TUI_TC_ACCENT, "Saved rate-limit resets"), *h = S(TUI_TC_DIM, "(/usage reset to spend)"), *l2 = xasprintf("  %s %s", k, h);
            strvec_push(&lines, l2);
            free(k), free(h), free(l2);
            for (size_t q = 0; q < rlines.n; q++) strvec_push_own(&lines, S(TUI_TC_DIM, rlines.v[q]));
        }
        strvec_free(&rlines);
        /* account order: worst-first, discovery order as tiebreak */
        double *rank = xcalloc(pr.n ? pr.n : 1, sizeof *rank);
        for (size_t ri = 0; ri < pr.n; ri++) {
            double worst = -1;
            json_t *l;
            json_arr_foreach(json_get_arr(pr.v[ri], "limits"), k, l) {
                double f;
                double v = usage_used_fraction(l, &f) ? f : -1;
                if (v > worst) worst = v;
            }
            rank[ri] = -worst * 1000 + (double)ri;
        }
        int sec_count = 0, sec_trailing = 0;
        char **amounts = xcalloc(groups.n ? groups.n : 1, sizeof *amounts);
        for (size_t gi = 0; gi < groups.n; gi++) {
            lgroup_t *g = &groups.v[gi];
            /* sort entries by account rank (stable) */
            size_t n = g->limits.n;
            for (size_t a = 1; a < n; a++)
                for (size_t b = a; b > 0; b--) {
                    size_t ia = 0, ib = 0;
                    for (size_t q = 0; q < pr.n; q++) {
                        if (pr.v[q] == g->reports.v[b - 1]) ia = q;
                        if (pr.v[q] == g->reports.v[b]) ib = q;
                    }
                    if (rank[ib] < rank[ia]) {
                        const json_t *t = g->limits.v[b];
                        g->limits.v[b] = g->limits.v[b - 1], g->limits.v[b - 1] = t;
                        t = g->reports.v[b];
                        g->reports.v[b] = g->reports.v[b - 1], g->reports.v[b - 1] = t;
                    } else break;
                }
            amounts[gi] = aggregate_amount((const json_t *const *)g->limits.v, n);
            sec_count = MAX(sec_count, (int)n);
            sec_trailing = MAX(sec_trailing, tui_visible_width(amounts[gi]));
        }
        int colw = resolve_column_width(sec_count, width, sec_trailing), barw = MIN(colw, BAR_WIDTH_MAX);
        for (size_t gi = 0; gi < groups.n; gi++) {
            lgroup_t *g = &groups.v[gi];
            size_t n = g->limits.n;
            /* resolveAggregateStatus */
            bool ok = false, wa = false, ex = false, all_used = n > 0;
            for (size_t q = 0; q < n; q++) {
                const char *s = limit_status(g->limits.v[q]);
                ok |= !strcmp(s, "ok"), wa |= !strcmp(s, "warning"), ex |= !strcmp(s, "exhausted");
                all_used &= usage_used_only_amount(g->limits.v[q]);
            }
            char *icon;
            if (!ok && !wa && !ex) icon = all_used ? S(TUI_TC_DIM, tui_sym("status.info")) : S(TUI_TC_DIM, tui_sym("status.pending"));
            else icon = status_icon(ok ? (wa || ex ? "warning" : "ok") : wa ? "warning" : "exhausted");
            char *ws;
            if (!strcasecmp(g->window_label, "quota window") || str_istr(g->label, g->window_label)) ws = xstrdup("");
            else {
                char *t = xasprintf("(%s)", g->window_label);
                ws = S(TUI_TC_DIM, t);
                free(t);
            }
            char *bl = tui_bold(g->label), *hl = xasprintf("%s %s %s", icon, bl, ws), *hlt = str_trim(hl);
            strvec_push(&lines, hlt);
            free(icon), free(ws), free(bl), free(hl), free(hlt);
            /* account header row */
            int max_suffix = 0;
            char **plabels = xcalloc(n, sizeof *plabels), **suffix = xcalloc(n, sizeof *suffix);
            bool *pact = xcalloc(n, sizeof *pact), *pday = xcalloc(n, sizeof *pday);
            for (size_t q = 0; q < n; q++) {
                const json_t *l = g->limits.v[q], *rr = g->reports.v[q];
                size_t ri = 0;
                for (size_t z = 0; z < pr.n; z++)
                    if (pr.v[z] == rr) ri = z;
                double ra = json_get_num(json_get_obj(l, "window"), "resetsAt", 0);
                char *rs = json_is_num(json_get(json_get_obj(l, "window"), "resetsAt")) && ra > now ? usage_fmt_duration(ra - now) : NULL;
                pact[q] = usage_limit_matches_active(rr, l, active);
                char *lab = account_label(l, json_at(peers, ri), peers, q);
                plabels[q] = pact[q] ? xasprintf("\xe2\x97\x8f %s", lab) : xstrdup(lab);
                free(lab);
                suffix[q] = rs ? xasprintf("(%s)", rs) : xstrdup("");
                free(rs);
                pday[q] = json_get_bool(json_get_obj(rr, "metadata"), "daybreak", false);
                max_suffix = MAX(max_suffix, tui_visible_width(suffix[q]));
            }
            int gap = max_suffix > 0 ? 1 : 0, budget = colw - max_suffix - gap;
            sb_t row = SB_INIT;
            for (size_t q = 0; q < n; q++) {
                char *cell;
                if (budget < 2) {
                    char *full = xasprintf("%s%s%s%s", plabels[q], pday[q] ? " daybreak" : "", *suffix[q] ? " " : "", suffix[q]);
                    char *t = truncate_label(full, colw), *p = pad_col(t, colw);
                    cell = pact[q] ? S(TUI_TC_ACCENT, p) : xstrdup(p);
                    free(full), free(t), free(p);
                } else {
                    const char *badge = pday[q] && budget >= 10 ? " daybreak" : "";
                    char *lab = truncate_label(plabels[q], budget - tui_visible_width(badge));
                    char *la = pact[q] ? S(TUI_TC_ACCENT, lab) : xstrdup(lab), *bd = *badge ? S(TUI_TC_SUCCESS, badge) : xstrdup("");
                    char *prefix = xasprintf("%s%s", la, bd);
                    int pw = tui_visible_width(prefix);
                    char *pcell = xasprintf("%s%*s", prefix, MAX(0, budget - pw), "");
                    if (!*suffix[q]) cell = xasprintf("%s%*s", pcell, max_suffix + gap, "");
                    else {
                        char *ds = S(TUI_TC_DIM, suffix[q]);
                        cell = xasprintf("%s %*s%s", pcell, max_suffix - tui_visible_width(suffix[q]), "", ds);
                        free(ds);
                    }
                    free(lab), free(la), free(bd), free(prefix), free(pcell);
                }
                sb_appendf(&row, "%s%s", q ? " " : "", cell);
                free(cell);
            }
            char *rl = xasprintf("  %s", sb_cstr(&row));
            size_t L = strlen(rl);
            while (L && rl[L - 1] == ' ') rl[--L] = 0;
            strvec_push(&lines, rl);
            free(rl);
            sb_free(&row);
            /* bars */
            sb_t bars = SB_INIT;
            for (size_t q = 0; q < n; q++) {
                char *b = usage_bar(g->limits.v[q], barw), *p = pad_col(b, colw);
                sb_appendf(&bars, "%s%s", q ? " " : "", p);
                free(b), free(p);
            }
            char *bline = xasprintf("  %s %s", sb_cstr(&bars), amounts[gi]);
            L = strlen(bline);
            while (L && bline[L - 1] == ' ') bline[--L] = 0;
            strvec_push(&lines, bline);
            free(bline);
            sb_free(&bars);
            if (n <= 1) {
                char *rr = reset_range((const json_t *const *)g->limits.v, n, now);
                if (rr) {
                    char *d = S(TUI_TC_DIM, rr), *l2 = xasprintf("  %s", d);
                    strvec_push(&lines, l2);
                    free(d), free(l2), free(rr);
                }
            }
            strvec_t ln = {0};
            for (size_t q = 0; q < n; q++) {
                json_t *x;
                json_arr_foreach(json_get_arr(g->limits.v[q], "notes"), z, x) if (json_sv(x) && !strvec_contains(&ln, json_sv(x))) strvec_push(&ln, json_sv(x));
            }
            if (ln.n) {
                char *j = strvec_join(&ln, " \xe2\x80\xa2 "), *o = one(j), *t = tui_truncate_to_width(o, 110, NULL), *d = S(TUI_TC_DIM, t), *l2 = xasprintf("  %s", d);
                strvec_push(&lines, l2);
                free(j), free(o), free(t), free(d), free(l2);
            }
            strvec_free(&ln);
            for (size_t q = 0; q < n; q++) free(plabels[q]), free(suffix[q]);
            free(plabels), free(suffix), free(pact), free(pday);
        }
        /* accounts without limits */
        for (size_t ri = 0; ri < pr.n; ri++) {
            if (json_len(json_get_arr(pr.v[ri], "limits")) != 0) continue;
            char *label;
            const char *email = meta(pr.v[ri], "email"), *acc = meta(pr.v[ri], "accountId"), *proj = meta(pr.v[ri], "projectId");
            bool codex = str_eq(prov, "openai-codex");
            if (email || acc) {
                if (codex) label = codex_tui_label(json_at(peers, ri), peers, email ? email : acc);
                else {
                    char *o = org_suffix(pr.v[ri]);
                    label = xasprintf("%s%s", email ? email : acc, o);
                    free(o);
                }
            } else {
                char *b = proj ? xstrdup(proj) : xstrdup("account 1");
                label = codex ? codex_tui_label(json_at(peers, ri), peers, b) : xstrdup(b);
                free(b);
            }
            const char *tier = codex ? NULL : meta(pr.v[ri], "planType");
            char *ts = NULL, *db = json_get_bool(json_get_obj(pr.v[ri], "metadata"), "daybreak", false) ? S(TUI_TC_SUCCESS, " daybreak") : xstrdup("");
            if (tier) {
                char *t = xasprintf("(%s)", tier), *d = S(TUI_TC_DIM, t);
                ts = xasprintf(" %s", d);
                free(t), free(d);
            } else ts = xstrdup("");
            char *ok = S(TUI_TC_SUCCESS, tui_sym("status.success")), *nl = S(TUI_TC_DIM, "-- no limits");
            char *l2 = xasprintf("%s %s%s%s %s", ok, label, db, ts, nl);
            strvec_push(&lines, l2);
            free(label), free(ts), free(db), free(ok), free(nl), free(l2);
        }
        for (size_t gi = 0; gi < groups.n; gi++) {
            free(groups.v[gi].key), free(groups.v[gi].label), free(groups.v[gi].window_label), free(amounts[gi]);
            vec_free(&groups.v[gi].limits);
            vec_free(&groups.v[gi].reports);
        }
        free(amounts);
        vec_free(&groups);
        free(rank);
        vec_free(&pr);
        json_free(peers);
    }
    free(pe);
    strvec_free(&provs);
    json_free(disp);
    char *s = strvec_join(&lines, "\n");
    strvec_free(&lines);
    return s;
}

/* ============================================================================================
 * Dashboard component
 * ============================================================================================ */

typedef struct dash dash_t;

typedef struct {
    int refs;   /* the dashboard's and the worker's (handed to the posted callback) */
    tui_t *tui;
    dash_t *d; /* NULL once the dashboard closed */
    json_t *result;
    bool failed;
    char *error;
} djob_t;

struct dash {
    tui_component_t base;
    ui_t *u;
    json_t *reports;
    json_t *unavailable;
    cards_t cards;
    double now;
    bool refreshing;
    char *refresh_error;
    bool detail;
    int scroll;
    json_t *activity; /* NULL while loading */
    char *activity_error;
    bool syncing;
    int detail_w;
    strvec_t detail_cache;
    int viewport;
    djob_t *activity_job, *refresh_job;
};

static dash_t *g_dash;
static pthread_mutex_t g_job_mu = PTHREAD_MUTEX_INITIALIZER;

static void job_unref(djob_t *j) {
    pthread_mutex_lock(&g_job_mu);
    bool last = --j->refs == 0;
    pthread_mutex_unlock(&g_job_mu);
    if (!last) return;
    json_free(j->result);
    free(j->error);
    free(j);
}

static char *editor_keys2(const char *a, const char *b) {
    char *x = ui_editor_key(a), *y = ui_editor_key(b), *r = xasprintf("%s/%s", x, y);
    free(x), free(y);
    return r;
}

static void mini_bar(sb_t *b, bool has, double f, const char *status, int width) {
    if (!has) {
        sb_t d = SB_INIT;
        for (int i = 0; i < width; i++) sb_append(&d, "\xc2\xb7");
        tui_fg(b, NULL, TUI_TC_DIM, sb_cstr(&d));
        sb_free(&d);
        return;
    }
    double c = MIN(MAX(f, 0.0), 1.0);
    int filled = (int)floor(c * width + 0.5);
    sb_t fb = SB_INIT, eb = SB_INIT;
    for (int i = 0; i < filled; i++) sb_append(&fb, "\xe2\x96\x88");
    for (int i = filled; i < width; i++) sb_append(&eb, "\xe2\x96\x91");
    tui_fg(b, NULL, status_slot(status), sb_cstr(&fb));
    tui_fg(b, NULL, TUI_TC_DIM, sb_cstr(&eb));
    sb_free(&fb), sb_free(&eb);
}

static void wrap_push(strvec_t *out, const char *text, int width, const char *indent) {
    strvec_t w = {0};
    tui_wrap_text_with_ansi(text, MAX(1, width), &w);
    for (size_t i = 0; i < w.n; i++) strvec_push_own(out, xasprintf("%s%s", indent, w.v[i]));
    strvec_free(&w);
}

typedef struct {
    int label_w, reset_w, bar_w;
    bool stacked;
    int heights[CARD_MAX_WINDOWS];
} cardlay_t;

static void card_lines(const card_t *c, int width, strvec_t *labels, const cardlay_t *lay, strvec_t *out) {
    const char *st;
    if (c->unavailable.n) {
        const char *sts[64];
        size_t n = 0;
        for (size_t i = 0; i < c->windows.n && n < 64; i++) sts[n++] = c->windows.v[i].status;
        st = !c->windows.n ? "unknown" : !strcmp(aggregate_status_n(sts, n), "exhausted") ? "exhausted" : "warning";
    } else if (c->unlimited) st = "ok";
    else {
        const char *sts[64];
        size_t n = 0;
        for (size_t i = 0; i < c->windows.n && n < 64; i++) sts[n++] = c->windows.v[i].status;
        st = aggregate_status_n(sts, n);
    }
    char *acc = c->accounts > 1 ? xasprintf("%d accts", c->accounts) : xstrdup("");
    char *accd = *acc ? S(TUI_TC_DIM, acc) : xstrdup("");
    int budget = width - 2 - tui_visible_width(accd) - (*acc ? 1 : 0);
    char *tn = tui_truncate_to_width(c->name, MAX(4, budget), NULL), *title = tui_bold(tn), *icon = status_icon(st);
    int pad = MAX(0, width - 2 - tui_visible_width(title) - tui_visible_width(accd));
    strvec_push_own(out, xasprintf("%s %s%*s%s", icon, title, pad, "", accd));
    free(acc), free(accd), free(tn), free(title), free(icon);
    for (size_t i = 0; i < c->daybreak.n; i++) {
        char *lab = one(c->daybreak.v[i]), *t = xasprintf("daybreak \xc2\xb7 %s", lab), *tt = tui_truncate_to_width(t, width - 2, NULL), *s = S(TUI_TC_SUCCESS, tt);
        strvec_push_own(out, xasprintf("  %s", s));
        free(lab), free(t), free(tt), free(s);
    }
    if (c->has_resets) {
        sb_t t = SB_INIT;
        sb_appendf(&t, "\xe2\x9c\xa6 %lld reset%s", (long long)c->banked, c->banked == 1 ? "" : "s");
        if (c->redeemable != c->banked) sb_appendf(&t, " \xc2\xb7 %lld usable", (long long)c->redeemable);
        if (c->has_exp) {
            if (c->exp_ms > 0) {
                char *d = usage_fmt_duration(c->exp_ms);
                sb_appendf(&t, " \xc2\xb7 expires %s", d);
                free(d);
            } else sb_append(&t, " \xc2\xb7 expired");
        }
        char *tt = tui_truncate_to_width(sb_cstr(&t), width - 2, NULL), *s = S(c->redeemable > 0 ? TUI_TC_SUCCESS : TUI_TC_WARNING, tt);
        strvec_push_own(out, xasprintf("  %s", s));
        free(tt), free(s);
        sb_free(&t);
        if (c->redeemable == 0 && c->reasons.n) {
            char *j = strvec_join(&c->reasons, " \xe2\x80\xa2 "), *o = one(j), *x = xasprintf("unavailable: %s", o), *tt2 = tui_truncate_to_width(x, width - 2, NULL),
                 *d = S(TUI_TC_DIM, tt2);
            strvec_push_own(out, xasprintf("  %s", d));
            free(j), free(o), free(x), free(tt2), free(d);
        }
    }
    for (size_t i = 0; i < c->unavailable.n; i++) {
        char *t = xasprintf("%s \xe2\x80\x94 usage unavailable", c->unavailable.v[i]), *o = one(t);
        strvec_t w = {0};
        tui_wrap_text_with_ansi(o, MAX(1, width - 2), &w);
        for (size_t k = 0; k < w.n; k++) {
            char *d = S(TUI_TC_DIM, w.v[k]);
            strvec_push_own(out, xasprintf("  %s", d));
            free(d);
        }
        strvec_free(&w);
        free(t), free(o);
    }
    if (c->unlimited) {
        char *d = S(TUI_TC_DIM, "no limits");
        strvec_push_own(out, xasprintf("  %s", d));
        free(d);
        return;
    }
    int content = MAX(1, width - 2);
    size_t shown = MIN(c->windows.n, (size_t)CARD_MAX_WINDOWS);
    for (size_t i = 0; i < shown; i++) {
        const cwin_t *w = &c->windows.v[i];
        const char *label0 = labels[i].n ? labels[i].v[0] : "";
        char *prefix = lay->stacked ? xstrdup("") : xasprintf("%s%*s ", label0, MAX(0, lay->label_w - tui_visible_width(label0)), "");
        if (lay->stacked)
            for (int l = 0; l < lay->heights[i]; l++) strvec_push_own(out, xasprintf("  %s", l < (int)labels[i].n ? labels[i].v[l] : ""));
        if (!w->has_f) {
            /* barlessAmountText / pressureTone: a spent quota without an amount reads `exhausted`, and the row takes
             * its reported status's tone (omp ac6918f301, 98129705b7) */
            bool ex = str_eq(w->status, "exhausted"), warn = str_eq(w->status, "warning");
            const char *amount = w->used_text ? w->used_text : ex ? "exhausted" : "no data";
            char *d = S(ex ? TUI_TC_ERROR : warn ? TUI_TC_WARNING : TUI_TC_DIM, amount), *x = xasprintf("%s%s", prefix, d);
            wrap_push(out, x, content, "  ");
            free(d), free(x), free(prefix);
            continue;
        }
        sb_t b = SB_INIT;
        sb_append(&b, prefix);
        mini_bar(&b, true, w->f, w->status, lay->bar_w);
        char pct[16];
        snprintf(pct, sizeof pct, "%.0f%%", MAX(0.0, floor((1 - w->f) * 100 + 0.5)));
        char *pp = xasprintf("%5s", pct);
        tui_fg(&b, NULL, status_slot(w->status), pp);
        free(pp);
        if (lay->reset_w > 0) {
            char *rp = w->has_reset ? usage_fmt_duration(w->reset_ms) : xstrdup("");
            char *rpp = xasprintf("%*s", lay->reset_w, rp);
            sb_putc(&b, ' ');
            tui_fg(&b, NULL, TUI_TC_DIM, rpp);
            free(rp), free(rpp);
        }
        wrap_push(out, sb_cstr(&b), content, "  ");
        sb_free(&b);
        free(prefix);
    }
    if (c->windows.n > CARD_MAX_WINDOWS) {
        char *t = xasprintf("+%zu more", c->windows.n - CARD_MAX_WINDOWS), *d = S(TUI_TC_DIM, t);
        strvec_push_own(out, xasprintf("  %s", d));
        free(t), free(d);
    }
}

static void cards_grid(const cards_t *cards, int inner, strvec_t *out) {
    if (cards->n == 0) {
        strvec_push_own(out, S(TUI_TC_DIM, "No usage data available."));
        return;
    }
    VEC(const card_t *) active = {0}, idle = {0};
    for (size_t i = 0; i < cards->n; i++) {
        if (cards->v[i].idle) vec_push(&idle, &cards->v[i]);
        else vec_push(&active, &cards->v[i]);
    }
    int cols = MAX(1, (inner + CARD_GUTTER) / (CARD_MIN_WIDTH + CARD_GUTTER));
    int cw = (inner - (cols - 1) * CARD_GUTTER) / cols;
    for (size_t start = 0; start < active.n; start += (size_t)cols) {
        size_t nc = MIN((size_t)cols, active.n - start);
        strvec_t labels[64][CARD_MAX_WINDOWS];
        memset(labels, 0, sizeof labels);
        int label_w = 0, reset_w = 0;
        char *raw[64][CARD_MAX_WINDOWS] = {{0}};
        for (size_t c = 0; c < nc && c < 64; c++) {
            const card_t *cd = active.v[start + c];
            for (size_t i = 0; i < MIN(cd->windows.n, (size_t)CARD_MAX_WINDOWS); i++) {
                const cwin_t *w = &cd->windows.v[i];
                char *lo = one(w->label), *lm = S(TUI_TC_MUTED, lo);
                if (w->tag) {
                    char *to = one(w->tag), *td = S(TUI_TC_DIM, to);
                    raw[c][i] = xasprintf("%s %s", lm, td);
                    free(to), free(td);
                } else raw[c][i] = xstrdup(lm);
                free(lo), free(lm);
                label_w = MAX(label_w, tui_visible_width(raw[c][i]));
                if (w->has_reset) {
                    char *d = usage_fmt_duration(w->reset_ms);
                    reset_w = MAX(reset_w, (int)strlen(d));
                    free(d);
                }
            }
        }
        int content = MAX(1, cw - 2), suffix = 5 + (reset_w > 0 ? reset_w + 1 : 0), inline_bar = content - label_w - 1 - suffix;
        cardlay_t lay = {.label_w = label_w, .reset_w = reset_w, .stacked = inline_bar < CARD_MIN_BAR_WIDTH};
        lay.bar_w = MAX(1, lay.stacked ? content - suffix : inline_bar);
        for (size_t c = 0; c < nc && c < 64; c++)
            for (size_t i = 0; i < CARD_MAX_WINDOWS; i++) {
                if (!raw[c][i]) continue;
                if (!lay.stacked) strvec_push(&labels[c][i], raw[c][i]);
                else {
                    char *bounded = tui_visible_width(raw[c][i]) > content * CARD_MAX_LABEL_LINES ? tui_truncate_to_width(raw[c][i], content * CARD_MAX_LABEL_LINES, NULL)
                                                                                                    : xstrdup(raw[c][i]);
                    strvec_t w = {0};
                    tui_wrap_text_with_ansi(bounded, content, &w);
                    if (w.n <= CARD_MAX_LABEL_LINES) {
                        for (size_t k = 0; k < w.n; k++) strvec_push(&labels[c][i], w.v[k]);
                    } else {
                        char *first = tui_slice_cols(bounded, 0, content), *rest = tui_slice_cols(bounded, content, tui_visible_width(bounded) - content);
                        strvec_push(&labels[c][i], first);
                        strvec_push_own(&labels[c][i], tui_truncate_to_width(rest, content, NULL));
                        free(first), free(rest);
                    }
                    strvec_free(&w);
                    free(bounded);
                }
                lay.heights[i] = MAX(lay.heights[i], (int)labels[c][i].n);
                free(raw[c][i]);
            }
        strvec_t rows[64];
        size_t height = 0;
        for (size_t c = 0; c < nc && c < 64; c++) {
            memset(&rows[c], 0, sizeof rows[c]);
            card_lines(active.v[start + c], cw, labels[c], &lay, &rows[c]);
            height = MAX(height, rows[c].n);
        }
        for (size_t li = 0; li < height; li++) {
            sb_t b = SB_INIT;
            for (size_t c = 0; c < nc && c < 64; c++) {
                const char *line = li < rows[c].n ? rows[c].v[li] : "";
                if (c) sb_appendf(&b, "%*s", CARD_GUTTER, "");
                sb_append(&b, line);
                tui_sb_spaces(&b, MAX(0, cw - tui_visible_width(line)));
            }
            while (b.len && b.s[b.len - 1] == ' ') b.s[--b.len] = 0;
            strvec_push(out, sb_cstr(&b));
            sb_free(&b);
        }
        for (size_t c = 0; c < nc && c < 64; c++) {
            strvec_free(&rows[c]);
            for (size_t i = 0; i < CARD_MAX_WINDOWS; i++) strvec_free(&labels[c][i]);
        }
        if (start + (size_t)cols < active.n) strvec_push(out, "");
    }
    if (idle.n) {
        if (active.n) strvec_push(out, "");
        sb_t names = SB_INIT;
        for (size_t i = 0; i < idle.n; i++) sb_appendf(&names, "%s%s", i ? " \xc2\xb7 " : "", idle.v[i]->name);
        char *t = xasprintf("untouched: %s", sb_cstr(&names)), *d = S(TUI_TC_DIM, t), *ok = S(TUI_TC_SUCCESS, tui_sym("status.success"));
        char *l = xasprintf("%s %s", ok, d);
        strvec_push_own(out, tui_truncate_to_width(l, inner, NULL));
        free(t), free(d), free(ok), free(l);
        sb_free(&names);
    }
    vec_free(&active);
    vec_free(&idle);
}

static void heat_ramp(char ramp[4][32]) {
    const tui_theme_t *th = tui_theme();
    bool dark_bg = tui_color_luma(tui_theme_rgb(th, TUI_TC_TEXT)) > 0.5;
    int fr = dark_bg ? 20 : 244, fgc = dark_bg ? 20 : 244, fb = dark_bg ? 24 : 246;
    uint32_t to = tui_theme_rgb(th, TUI_TC_ACCENT);
    static const double T[4] = {0.3, 0.5, 0.72, 1};
    for (int i = 0; i < 4; i++) {
        int r = (int)floor(fr + (((to >> 16) & 255) - fr) * T[i] + 0.5), g = (int)floor(fgc + (((to >> 8) & 255) - fgc) * T[i] + 0.5),
            b = (int)floor(fb + ((to & 255) - fb) * T[i] + 0.5);
        tui_rgb_fg(th, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b, ramp[i], 32);
    }
}

static void heatmap(const dash_t *d, int inner, strvec_t *out) {
    if (d->activity_error) {
        char *x = *d->activity_error ? xasprintf("Usage history unavailable (%s).", d->activity_error) : xstrdup("Usage history unavailable.");
        strvec_push_own(out, S(TUI_TC_DIM, x));
        free(x);
        return;
    }
    if (!d->activity) {
        strvec_push_own(out, S(TUI_TC_DIM, "Loading usage history\xe2\x80\xa6"));
        return;
    }
    apoints_t pts = {0};
    json_t *p;
    json_arr_foreach(d->activity, i, p) {
        apoint_t a = {.cost = json_get_num(p, "cost", 0), .requests = json_get_num(p, "requests", 0)};
        snprintf(a.day, sizeof a.day, "%s", json_get_str(p, "day") ? json_get_str(p, "day") : "");
        vec_push(&pts, a);
    }
    int weeks = MAX(4, MIN(53, (inner - 2) / 2));
    heat_t h;
    heat_build(&pts, weeks, time(NULL), &h);
    vec_free(&pts);
    char ramp[4][32];
    heat_ramp(ramp);
    char *cost = h.total_cost >= 1 ? (char *)NULL : xasprintf("$%.2f", h.total_cost);
    if (!cost) {
        char *g = usage_fmt_grouped(h.total_cost, 0);
        cost = xasprintf("$%s", g);
        free(g);
    }
    char *reqs = usage_fmt_compact(h.total_requests);
    char *meta_t = xasprintf("%s \xc2\xb7 %s requests \xc2\xb7 last %d weeks", cost, reqs, weeks);
    char *acc = S(TUI_TC_ACCENT, "Activity"), *bacc = tui_bold(acc), *dm = S(TUI_TC_DIM, meta_t), *sy = d->syncing ? S(TUI_TC_DIM, " \xc2\xb7 syncing\xe2\x80\xa6") : xstrdup("");
    strvec_push_own(out, xasprintf("%s %s%s", bacc, dm, sy));
    free(cost), free(reqs), free(meta_t), free(acc), free(bacc), free(dm), free(sy);
    strvec_push(out, "");
    sb_t ml = SB_INIT;
    sb_append(&ml, "  ");
    int mlw = 2;
    for (int w = 0; w < weeks; w++) {
        int col = 2 + w * 2;
        if (h.month_labels[w] && col >= mlw) {
            tui_sb_spaces(&ml, col - mlw);
            sb_append(&ml, h.month_labels[w]);
            mlw = col + (int)strlen(h.month_labels[w]);
        }
    }
    char *mt = tui_truncate_to_width(sb_cstr(&ml), inner, NULL);
    strvec_push_own(out, S(TUI_TC_DIM, mt));
    free(mt);
    sb_free(&ml);
    for (int day = 0; day < 7; day++) {
        sb_t b = SB_INIT;
        tui_fg(&b, NULL, TUI_TC_DIM, DAY_LABELS[day]);
        sb_putc(&b, ' ');
        for (int w = 0; w < weeks; w++) {
            int c = h.cells[day][w];
            if (c < 0) sb_append(&b, "  ");
            else if (c == 0) {
                tui_fg(&b, NULL, TUI_TC_DIM, "\xc2\xb7");
                sb_putc(&b, ' ');
            } else sb_appendf(&b, "%s\xe2\x96\xa0\x1b[39m ", ramp[c - 1]);
        }
        while (b.len && b.s[b.len - 1] == ' ') b.s[--b.len] = 0;
        strvec_push(out, sb_cstr(&b));
        sb_free(&b);
    }
}

static void dash_overview(dash_t *d, int inner, strvec_t *out) {
    cards_grid(&d->cards, inner, out);
    strvec_push(out, "");
    heatmap(d, inner, out);
}

static const strvec_t *dash_detail_lines(dash_t *d, int inner) {
    if (d->detail_w != inner) {
        strvec_free(&d->detail_cache);
        const char *prov = d->u->app->model ? d->u->app->model->provider : NULL;
        json_t *id = usage_active_identity(prov), *sel = usage_model_selectors(d->reports);
        char *s = ui_usage_detail(d->reports, (double)now_ms(), inner, prov, id, sel, d->unavailable);
        d->detail_cache = str_split(s, "\n");
        free(s);
        json_free(id);
        json_free(sel);
        d->detail_w = inner;
    }
    return &d->detail_cache;
}

static void dash_render(tui_component_t *c, int width, strvec_t *out) {
    dash_t *d = (dash_t *)c;
    int rows = tui_term(d->u->tui)->rows;
    int height = MAX(14, rows > 0 ? rows : 40), inner = MAX(20, width - 4);
    strvec_t ov = {0};
    const strvec_t *content;
    if (d->detail) content = dash_detail_lines(d, inner);
    else {
        dash_overview(d, inner, &ov);
        content = &ov;
    }
    int crow = MAX(5, height - 5);
    d->viewport = crow;
    int max_scroll = MAX(0, (int)content->n - crow);
    if (d->scroll > max_scroll) d->scroll = max_scroll;
    double latest = 0;
    json_t *r;
    json_arr_foreach(d->reports, i, r) latest = MAX(latest, json_get_num(r, "fetchedAt", 0));
    char *checked;
    if (d->refreshing) checked = xstrdup("refreshing\xe2\x80\xa6");
    else if (latest > 0) {
        char *x = usage_fmt_duration(d->now - latest);
        checked = xasprintf("checked %s ago", x);
        free(x);
    } else checked = xstrdup("");
    char *scroll = max_scroll > 0 ? editor_keys2("tui.select.up", "tui.select.down") : NULL;
    char *sh = scroll ? xasprintf("%s scroll \xc2\xb7 ", scroll) : xstrdup("");
    char *cancel = ui_editor_key("tui.select.cancel"), *enter = ui_key_hint("enter"), *hint;
    if (d->detail) {
        char *tabk = ui_key_hint("tab");
        hint = xasprintf("%sr refresh \xc2\xb7 %s overview \xc2\xb7 %s back", sh, tabk, cancel);
        free(tabk);
    }
    else hint = xasprintf("%sr refresh \xc2\xb7 %s details \xc2\xb7 %s close", sh, enter, cancel);
    strvec_push_own(out, ui_box_top(width, d->detail ? "Usage \xc2\xb7 Details" : "Usage"));
    char *cd = *checked ? S(TUI_TC_DIM, checked) : xstrdup("");
    strvec_push_own(out, ui_box_row(cd, width));
    for (int i = 0; i < crow; i++) {
        int idx = d->scroll + i;
        strvec_push_own(out, ui_box_row(idx < (int)content->n ? content->v[idx] : "", width));
    }
    strvec_push_own(out, ui_box_divider(width));
    char *hd = S(TUI_TC_DIM, hint);
    strvec_push_own(out, ui_box_row(hd, width));
    strvec_push_own(out, ui_box_bottom(width));
    free(cd), free(hd), free(checked), free(scroll), free(sh), free(cancel), free(enter), free(hint);
    strvec_free(&ov);
}

static void dash_free_state(dash_t *d) {
    json_free(d->reports);
    json_free(d->unavailable);
    json_free(d->activity);
    cards_free(&d->cards);
    free(d->refresh_error);
    free(d->activity_error);
    strvec_free(&d->detail_cache);
}

static void dash_close(dash_t *d) {
    ui_t *u = d->u;
    pthread_mutex_lock(&g_job_mu);
    if (d->activity_job) d->activity_job->d = NULL;
    if (d->refresh_job) d->refresh_job->d = NULL;
    pthread_mutex_unlock(&g_job_mu);
    if (d->activity_job) job_unref(d->activity_job);
    if (d->refresh_job) job_unref(d->refresh_job);
    d->activity_job = d->refresh_job = NULL;
    g_dash = NULL;
    tui_overlay_hide(u->tui, &d->base);
    tui_alt_screen(u->tui, false);
    tui_set_focus(u->tui, &u->editor->base);
    tui_component_destroy(&d->base);
    tui_request_render(u->tui);
}

static void refresh_done(void *ud) {
    djob_t *j = ud;
    dash_t *d = j->d;
    if (d && g_dash == d) {
        d->refreshing = false;
        if (j->result) {
            json_free(d->reports);
            d->reports = j->result;
            j->result = NULL;
            d->now = (double)now_ms();
            cards_free(&d->cards);
            build_cards(d->reports, d->now, d->unavailable, &d->cards);
            d->detail_w = -1;
        }
        if (d->refresh_job == j) d->refresh_job = NULL, job_unref(j);
        tui_component_request_render(&d->base);
    }
    job_unref(j);
}

static void *refresh_thread(void *arg) {
    djob_t *j = arg;
    pthread_mutex_lock(&g_job_mu);
    bool alive = j->d != NULL;
    pthread_mutex_unlock(&g_job_mu);
    if (alive) j->result = usage_fetch_reports(NULL);
    pthread_mutex_lock(&g_job_mu);
    alive = j->d != NULL;
    pthread_mutex_unlock(&g_job_mu);
    if (alive) tui_post(j->tui, refresh_done, j);
    else job_unref(j);
    return NULL;
}

static void dash_refresh(dash_t *d) {
    if (d->refreshing) return;
    d->refreshing = true;
    free(d->refresh_error);
    d->refresh_error = NULL;
    djob_t *j = xcalloc(1, sizeof *j);
    j->refs = 2;
    j->tui = d->u->tui;
    j->d = d;
    d->refresh_job = j;
    pthread_t t;
    if (pthread_create(&t, NULL, refresh_thread, j) == 0) pthread_detach(t);
    else {
        d->refreshing = false;
        j->refs = 1;
        d->refresh_job = NULL;
        job_unref(j);
    }
    tui_component_request_render(&d->base);
}

static void activity_done(void *ud) {
    djob_t *j = ud;
    dash_t *d = j->d;
    if (d && g_dash == d) {
        d->syncing = false;
        if (j->result) {
            json_free(d->activity);
            d->activity = j->result;
            j->result = NULL;
        } else {
            free(d->activity_error);
            d->activity_error = xstrdup(j->error ? j->error : "");
        }
        if (d->activity_job == j) d->activity_job = NULL, job_unref(j);
        tui_component_request_render(&d->base);
    }
    job_unref(j);
}

static void *activity_thread(void *arg) {
    djob_t *j = arg;
    char *dir = omc_agent_path("sessions");
    ui_usage_load_activity(dir, &j->result);
    free(dir);
    pthread_mutex_lock(&g_job_mu);
    bool alive = j->d != NULL;
    pthread_mutex_unlock(&g_job_mu);
    if (alive) tui_post(j->tui, activity_done, j);
    else job_unref(j);
    return NULL;
}

static bool dash_key(tui_component_t *c, const tui_key_t *k) {
    dash_t *d = (dash_t *)c;
    if (k->kind == TUI_KEY_MOUSE) {
        const char *t = k->text;
        if (t && !strncmp(t, "\x1b[<", 3)) {
            int b = atoi(t + 3);
            if ((b & 64) && !(b & 2)) d->scroll = MAX(0, d->scroll + ((b & 1) ? 2 : -2));
        }
        tui_component_request_render(c);
        return true;
    }
    if (tui_kb(k, "tui.select.cancel") || tui_key_matches(k, "q")) {
        if (d->detail) {
            d->detail = false;
            d->scroll = 0;
        } else {
            dash_close(d);
            return true;
        }
    } else if (tui_key_matches(k, "r")) dash_refresh(d);
    else if (tui_key_matches(k, "tab")) {
        d->detail = !d->detail;
        d->scroll = 0;
    } else if (!d->detail && (tui_key_matches(k, "enter") || tui_key_matches(k, "d"))) {
        d->detail = true;
        d->scroll = 0;
    } else if (tui_kb(k, "tui.select.up")) d->scroll = MAX(0, d->scroll - 1);
    else if (tui_kb(k, "tui.select.down")) d->scroll++;
    else if (tui_kb(k, "tui.select.pageUp")) d->scroll = MAX(0, d->scroll - d->viewport);
    else if (tui_kb(k, "tui.select.pageDown")) d->scroll += d->viewport;
    else if (tui_key_matches(k, "home")) d->scroll = 0;
    else if (tui_key_matches(k, "end")) d->scroll = INT32_MAX / 2;
    tui_component_request_render(c);
    return true;
}

static void dash_invalidate(tui_component_t *c) { ((dash_t *)c)->detail_w = -1; }

static void dash_destroy(tui_component_t *c) {
    dash_free_state((dash_t *)c);
    free(c);
}

static const tui_component_vt DASH_VT = {"usage-dashboard", dash_render, dash_key, dash_invalidate, dash_destroy};

/* selector-controller.ts showUsageDashboard */
static void show_dashboard(ui_t *u, json_t *reports) {
    if (g_dash) {
        json_free(reports);
        tui_set_focus(u->tui, &g_dash->base);
        return;
    }
    json_t *accounts = usage_stored_accounts(NULL);
    if (json_len(reports) == 0 && json_len(accounts) == 0) {
        add_notice(u, "warning", "No usage data available.");
        json_free(accounts);
        json_free(reports);
        return;
    }
    json_t *unrep = usage_unreported_accounts(reports, accounts), *a, *unavailable = json_arr();
    json_arr_foreach(unrep, i, a) {
        json_t *o = json_obj();
        json_set_str(o, "provider", json_get_str(a, "provider"));
        char *l = usage_account_label(a);
        json_set_str(o, "label", l);
        free(l);
        json_push(unavailable, o);
    }
    json_free(unrep);
    json_free(accounts);
    dash_t *d = xcalloc(1, sizeof *d);
    tui_component_init(&d->base, &DASH_VT);
    d->base.wants_mouse = true;
    d->u = u;
    d->reports = reports;
    d->unavailable = unavailable;
    d->now = (double)now_ms();
    d->syncing = true;
    d->detail_w = -1;
    build_cards(reports, d->now, unavailable, &d->cards);
    g_dash = d;
    djob_t *j = xcalloc(1, sizeof *j);
    j->refs = 2;
    j->tui = u->tui;
    j->d = d;
    d->activity_job = j;
    pthread_t t;
    if (pthread_create(&t, NULL, activity_thread, j) == 0) pthread_detach(t);
    else {
        j->refs = 1;
        d->activity_job = NULL;
        job_unref(j);
        d->syncing = false;
        d->activity_error = xstrdup("");
    }
    tui_alt_screen(u->tui, true);
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &d->base, &o);
    tui_set_focus(u->tui, &d->base);
}

/* ---------- /usage fetch ---------- */

typedef struct {
    ui_t *u;
    json_t *reports;
} fetch_t;

static void fetch_done(void *ud) {
    fetch_t *f = ud;
    ui_t *u = f->u;
    show_dashboard(u, f->reports);
    free(f);
    tui_request_render(u->tui);
}

static void *fetch_thread(void *arg) {
    fetch_t *f = arg;
    f->reports = usage_fetch_reports(NULL);
    tui_post(f->u->tui, fetch_done, f);
    return NULL;
}

/* ============================================================================================
 * /usage reset picker (reset-usage-selector.ts)
 * ============================================================================================ */

#define RESET_MAX_VISIBLE 10

typedef struct {
    tui_component_t base;
    ui_t *u;
    json_t *accounts;
    int selected;
    int pending; /* -1 none */
    char *status;
} rsel_t;

static char *count_label(const json_t *a) {
    if (json_get_str(a, "error")) return one(json_get_str(a, "error"));
    int64_t av = json_get_int(a, "availableCount", 0), rd = json_get_int(a, "redeemableCount", 0);
    sb_t b = SB_INIT;
    sb_appendf(&b, "%lld saved reset%s", (long long)av, av == 1 ? "" : "s");
    if (rd != av) sb_appendf(&b, " \xc2\xb7 %lld usable now", (long long)rd);
    double e;
    if (json_get_str(a, "expiresAt") && trace_parse_iso_ms(json_get_str(a, "expiresAt"), &e)) {
        double now = (double)now_ms();
        if (e > now) {
            char *d = usage_fmt_duration(e - now);
            sb_appendf(&b, " \xc2\xb7 expires in %s", d);
            free(d);
        } else sb_append(&b, " \xc2\xb7 expired");
    }
    if (rd <= 0 && json_get_str(a, "unavailableReason")) {
        char *o = one(json_get_str(a, "unavailableReason"));
        sb_appendf(&b, " \xc2\xb7 %s", o);
        free(o);
    }
    return sb_detach(&b);
}

static char *confirm_message(const json_t *a) {
    const json_t *credit = json_get_obj(a, "credit");
    char *subject = json_get_str(credit, "title") ? xasprintf("\xe2\x80\x9c%s\xe2\x80\x9d", json_get_str(credit, "title")) : xstrdup("1 saved reset");
    sb_t b = SB_INIT;
    char *enter = ui_key_hint("enter"), *esc = ui_editor_key("tui.select.cancel");
    sb_appendf(&b, "Press %s again to spend %s for %s (%s).", enter, subject, json_get_str(a, "label"), json_get_str(a, "providerLabel"));
    if (str_eq(json_get_str(credit, "program"), "juniper_tide")) sb_append(&b, " This resets Claude's 5h session limit only; weekly limits stay unchanged.");
    else if (json_len(json_get_arr(credit, "clears"))) {
        sb_append(&b, " Covers ");
        json_t *c;
        json_arr_foreach(json_get_arr(credit, "clears"), i, c) sb_appendf(&b, "%s%s", i ? ", " : "", usage_reset_window_name(json_sv(c)));
        sb_append(&b, ".");
    }
    if (json_is_bool(json_get(credit, "requiresLimit")) && !json_get_bool(credit, "requiresLimit", true))
        sb_append(&b, " Optional early use: this can be spent before the covered limit is reached.");
    sb_appendf(&b, " %s cancels.", esc);
    free(subject), free(enter), free(esc);
    return sb_detach(&b);
}

static void rsel_render(tui_component_t *c, int width, strvec_t *out) {
    rsel_t *s = (rsel_t *)c;
    int inner = MAX(1, width - 4);
    strvec_t body = {0};
    int total = (int)json_len(s->accounts);
    int start = 0, end = total;
    if (total > RESET_MAX_VISIBLE) {
        start = MAX(0, MIN(s->selected - RESET_MAX_VISIBLE / 2, total - RESET_MAX_VISIBLE));
        end = start + RESET_MAX_VISIBLE;
    }
    strvec_t rows = {0};
    for (int i = start; i < end; i++) {
        const json_t *a = json_at(s->accounts, (size_t)i);
        bool sel = i == s->selected, red = json_get_int(a, "redeemableCount", 0) > 0;
        char *cl = count_label(a);
        char *ct = json_get_str(a, "error") ? S(TUI_TC_ERROR, cl) : red ? S(TUI_TC_SUCCESS, cl) : S(TUI_TC_DIM, cl);
        char *at = json_get_bool(a, "active", false) ? S(TUI_TC_MUTED, " (active)") : xstrdup("");
        char *lab = one(json_get_str(a, "label")), *pl = one(json_get_str(a, "providerLabel"));
        char *pt_raw = xasprintf("[%s \xc2\xb7 #%lld] ", pl, (long long)json_get_int(json_get_obj(a, "target"), "credentialId", 0)), *pt = S(TUI_TC_MUTED, pt_raw);
        char *line;
        if (sel) {
            char *cur = xasprintf("%s ", tui_sym("nav.cursor")), *cc = S(TUI_TC_ACCENT, cur), *name = red ? S(TUI_TC_ACCENT, lab) : S(TUI_TC_DIM, lab);
            line = xasprintf("%s%s%s%s  %s", cc, pt, name, at, ct);
            free(cur), free(cc), free(name);
        } else {
            char *name = red ? xstrdup(lab) : S(TUI_TC_DIM, lab);
            line = xasprintf("  %s%s%s  %s", pt, name, at, ct);
            free(name);
        }
        strvec_push_own(&rows, line);
        free(cl), free(ct), free(at), free(lab), free(pl), free(pt_raw), free(pt);
    }
    if (rows.n) {
        int off = start;
        ui_scroll_view(&rows, (int)rows.n, &off, inner, &body);
    }
    strvec_free(&rows);
    if (total == 0) strvec_push_own(&body, S(TUI_TC_MUTED, "No provider accounts with saved resets"));
    char *hint;
    if (s->pending >= 0) {
        char *m = confirm_message(json_at(s->accounts, (size_t)s->pending)), *o = one(m);
        hint = S(TUI_TC_WARNING, o);
        free(m), free(o);
    } else {
        char *ud = editor_keys2("tui.select.up", "tui.select.down"), *en = ui_key_hint("enter"), *ca = ui_editor_key("tui.select.cancel");
        char *t = xasprintf("%s select \xc2\xb7 %s spend a reset \xc2\xb7 %s cancel", ud, en, ca);
        hint = S(TUI_TC_MUTED, t);
        free(ud), free(en), free(ca), free(t);
    }
    ui_push_text(&body, hint, inner);
    free(hint);
    if (s->status) {
        strvec_push(&body, "");
        char *o = one(s->status), *w = S(TUI_TC_WARNING, o);
        ui_push_text(&body, w, inner);
        free(o), free(w);
    }
    strvec_push_own(out, ui_box_top(width, "Spend a saved rate-limit reset"));
    for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
    strvec_push_own(out, ui_box_bottom(width));
    strvec_free(&body);
}

typedef struct {
    ui_t *u;
    json_t *account;
    json_t *outcome;
} redeem_t;

static void redeem_done(void *ud) {
    redeem_t *r = ud;
    char *m = usage_describe_redeem(r->outcome, json_get_str(r->account, "label")), *o = one(m);
    add_notice(r->u, json_get_bool(r->outcome, "ok", false) ? "info" : "warning", o);
    free(m), free(o);
    json_free(r->account);
    json_free(r->outcome);
    tui_request_render(r->u->tui);
    free(r);
}

static void *redeem_thread(void *arg) {
    redeem_t *r = arg;
    r->outcome = usage_redeem_reset_credit(json_get_obj(r->account, "target"));
    tui_post(r->u->tui, redeem_done, r);
    return NULL;
}

static void start_redeem(ui_t *u, const json_t *account) {
    char *lab = one(json_get_str(account, "label"));
    char *m = xasprintf("Spending 1 saved reset for %s (%s)\xe2\x80\xa6", lab, json_get_str(account, "providerLabel"));
    add_notice(u, "info", m);
    free(lab), free(m);
    redeem_t *r = xcalloc(1, sizeof *r);
    r->u = u;
    r->account = json_clone(account);
    pthread_t t;
    if (pthread_create(&t, NULL, redeem_thread, r) == 0) pthread_detach(t);
    else {
        json_free(r->account);
        free(r);
    }
}

static bool rsel_key(tui_component_t *c, const tui_key_t *k) {
    rsel_t *s = (rsel_t *)c;
    int total = (int)json_len(s->accounts);
    if (tui_kb(k, "tui.select.cancel")) {
        if (s->pending >= 0) {
            s->pending = -1;
            free(s->status);
            s->status = NULL;
        } else {
            ui_close_selector(s->u);
            tui_request_render(s->u->tui);
            return true;
        }
    } else if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down")) {
        s->pending = -1;
        if (total) s->selected = (s->selected + (tui_kb(k, "tui.select.up") ? total - 1 : 1)) % total;
        free(s->status);
        s->status = NULL;
    } else if (tui_key_matches(k, "pageUp") || tui_key_matches(k, "pageDown")) {
        s->pending = -1;
        if (total) s->selected = MAX(0, MIN(total - 1, s->selected + (tui_key_matches(k, "pageUp") ? -RESET_MAX_VISIBLE : RESET_MAX_VISIBLE)));
        free(s->status);
        s->status = NULL;
    } else if (tui_key_matches(k, "enter") || tui_key_matches(k, "return") || tui_key_matches(k, "ctrl+j")) {
        const json_t *a = json_at(s->accounts, (size_t)s->selected);
        if (!a) return true;
        free(s->status);
        s->status = NULL;
        if (json_get_int(a, "redeemableCount", 0) <= 0) {
            s->status = json_get_str(a, "unavailableReason")
                            ? xasprintf("That account's saved resets are unavailable: %s", json_get_str(a, "unavailableReason"))
                            : xstrdup("That account has no saved resets usable right now.");
        } else if (s->pending != s->selected) s->pending = s->selected;
        else {
            json_t *acc = json_clone(a);
            ui_t *u = s->u;
            ui_close_selector(u);
            start_redeem(u, acc);
            json_free(acc);
            tui_request_render(u->tui);
            return true;
        }
    }
    tui_component_request_render(c);
    return true;
}

static void rsel_destroy(tui_component_t *c) {
    rsel_t *s = (rsel_t *)c;
    json_free(s->accounts);
    free(s->status);
    free(s);
}

static const tui_component_vt RSEL_VT = {"reset-usage-selector", rsel_render, rsel_key, NULL, rsel_destroy};

typedef struct {
    ui_t *u;
    char *arg; /* NULL = picker */
    json_t *statuses;
    char *text;
} rlist_t;

static void rlist_done(void *ud) {
    rlist_t *r = ud;
    ui_t *u = r->u;
    if (r->text) {
        add_notice(u, "info", r->text);
    } else {
        json_t *accounts = usage_reset_accounts(r->statuses);
        bool any_avail = false, any_err = false;
        json_t *a;
        json_arr_foreach(accounts, i, a) {
            any_avail |= json_get_int(a, "availableCount", 0) > 0;
            any_err |= json_get_str(a, "error") != NULL;
        }
        if (json_len(accounts) == 0) add_notice(u, "info", "No provider accounts found. Use /login to add one.");
        else if (!any_avail)
            add_notice(u, "info", any_err ? "No saved resets available \xe2\x80\x94 some accounts couldn't be reached (try /login)."
                                          : "No saved rate-limit resets available to spend right now.");
        else {
            /* attach each account's selected credit for the confirmation text */
            json_arr_foreach(accounts, i, a) {
                json_t *st;
                json_arr_foreach(r->statuses, k, st) {
                    if (!str_eq(json_get_str(st, "provider"), json_get_str(a, "provider")) ||
                        json_get_int(st, "credentialId", -1) != json_get_int(json_get_obj(a, "target"), "credentialId", -2))
                        continue;
                    const char *next = json_get_str(st, "nextCreditId");
                    json_t *c, *pick = NULL;
                    json_arr_foreach(json_get_arr(st, "credits"), q, c) if (next ? str_eq(json_get_str(c, "id"), next) : !pick && json_get_bool(c, "usable", true)) pick = c;
                    if (!pick && !next) pick = json_at(json_get_arr(st, "credits"), 0);
                    if (pick) json_set(a, "credit", json_clone(pick));
                }
            }
            rsel_t *s = xcalloc(1, sizeof *s);
            tui_component_init(&s->base, &RSEL_VT);
            s->u = u;
            s->accounts = accounts;
            accounts = NULL;
            s->pending = -1;
            json_t *x;
            json_arr_foreach(s->accounts, i, x) if (json_get_int(x, "redeemableCount", 0) > 0) {
                s->selected = (int)i;
                break;
            }
            ui_show_selector(u, &s->base, &s->base);
        }
        json_free(accounts);
    }
    json_free(r->statuses);
    free(r->text);
    free(r->arg);
    tui_request_render(u->tui);
    free(r);
}

static void *rlist_thread(void *arg) {
    rlist_t *r = arg;
    const char *prov = r->u->app->model ? r->u->app->model->provider : NULL;
    if (r->arg) r->text = slash_usage_reset_text(r->u->app, r->arg);
    else r->statuses = usage_list_reset_credits(prov);
    tui_post(r->u->tui, rlist_done, r);
    return NULL;
}

/* ============================================================================================
 * /usage
 * ============================================================================================ */

void ui_usage_command(ui_t *u, const char *args) {
    char *verb, *rest;
    slash_subcommand(args, &verb, &rest);
    if (!*verb || (!strcmp(verb, "show") && !*rest)) {
        fetch_t *f = xcalloc(1, sizeof *f);
        f->u = u;
        pthread_t t;
        if (pthread_create(&t, NULL, fetch_thread, f) == 0) pthread_detach(t);
        else free(f);
    } else if (!strcmp(verb, "reset")) {
        rlist_t *r = xcalloc(1, sizeof *r);
        r->u = u;
        if (*rest) r->arg = xstrdup(rest);
        else add_notice(u, "info", "Checking saved rate-limit resets\xe2\x80\xa6");
        pthread_t t;
        if (pthread_create(&t, NULL, rlist_thread, r) == 0) pthread_detach(t);
        else free(r->arg), free(r);
    } else add_notice(u, "info", "Usage: /usage [show|reset [provider/credential-id|provider/active]]");
    free(verb);
    free(rest);
}

bool ui_usage_dashboard_open(void) { return g_dash != NULL; }

/* ============================================================================================
 * Per-turn usage row (usage-row.ts usageRowSpecs / formatMetricRow, separator "  ")
 * ============================================================================================ */

char *ui_usage_row(const json_t *usage, double duration_ms, double ttft_ms, double timestamp, double turn_ms) {
    strvec_t parts = {0};
    if (timestamp > 0 && isfinite(timestamp)) {
        time_t t = (time_t)(timestamp / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        char buf[32];
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
        strvec_push(&parts, buf);
    }
    if (turn_ms > 0) {
        char *d = usage_fmt_duration(floor(turn_ms + 0.5));
        strvec_push_own(&parts, xasprintf("\xce\x94 %s", d));
        free(d);
    }
    double in = json_get_num(usage, "input", 0) + json_get_num(usage, "cacheWrite", 0), out = json_get_num(usage, "output", 0),
           cr = json_get_num(usage, "cacheRead", 0);
    char *a = usage_fmt_number(in, -1), *b = usage_fmt_number(out, -1);
    strvec_push_own(&parts, xasprintf("%s %s", tui_sym("icon.input"), a));
    strvec_push_own(&parts, xasprintf("%s %s", tui_sym("icon.output"), b));
    free(a), free(b);
    if (cr > 0) {
        char *c = usage_fmt_number(cr, -1);
        strvec_push_own(&parts, xasprintf("%s %s", tui_sym("icon.cache"), c));
        free(c);
    }
    if (ttft_ms > 0) strvec_push_own(&parts, xasprintf("%s %.1fs", tui_sym("icon.time"), ttft_ms / 1000));
    if (duration_ms > 100 && out > 0) strvec_push_own(&parts, xasprintf("%s %.1f/s", tui_sym("icon.throughput"), out / duration_ms * 1000));
    char *r = strvec_join(&parts, "  ");
    strvec_free(&parts);
    return r;
}
