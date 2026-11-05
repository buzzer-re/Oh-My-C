/* compat_detect.c - runtime model policy for custom and discovered models (see compat_detect.h). */
#define _GNU_SOURCE
#include "ai/compat_detect.h"

#include "core/core.h"
#include "core/embed.h"

#include <ctype.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

/* ---------- embedded rules (assets/compat_rules.json.z, build-tools/slim_compat_rules.py) ---------- */

static json_t *g_rules;
static pthread_once_t g_rules_once = PTHREAD_ONCE_INIT;

static void rules_load(void) {
    size_t len = 0;
    const char *z = embed_get("assets/compat_rules.json.z", &len);
    if (z) {
        z_stream zs = {0};
        if (inflateInit(&zs) == Z_OK) {
            sb_t b = SB_INIT;
            char out[65536];
            zs.next_in = (Bytef *)z;
            zs.avail_in = (uInt)len;
            int r;
            do {
                zs.next_out = (Bytef *)out;
                zs.avail_out = sizeof out;
                r = inflate(&zs, Z_NO_FLUSH);
                if (r != Z_OK && r != Z_STREAM_END) break;
                sb_appendn(&b, out, sizeof out - zs.avail_out);
            } while (r != Z_STREAM_END);
            inflateEnd(&zs);
            if (r == Z_STREAM_END) g_rules = json_parse(b.s ? b.s : "", b.len, NULL);
            sb_free(&b);
        }
    }
    if (!g_rules) g_rules = json_obj();
}

static const json_t *rules(void) {
    pthread_once(&g_rules_once, rules_load);
    return g_rules;
}

static bool arr_has(const json_t *arr, const char *s) {
    json_t *it;
    json_arr_foreach(arr, i, it) if (str_eq(json_sv(it), s)) return true;
    return false;
}

/* ---------- identity (compat/taxonomy.ts classifyModel, lenient) ---------- */

/* cascade.ts globMatch: anchored `*` wildcards */
static bool glob_match(const char *pat, const char *s) {
    const char *star = strchr(pat, '*');
    if (!star) return strcmp(pat, s) == 0;
    size_t hl = (size_t)(star - pat);
    if (strncmp(s, pat, hl)) return false;
    const char *rest = s + hl;
    const char *p = star + 1;
    for (;;) {
        const char *next = strchr(p, '*');
        if (!next) {
            size_t ll = strlen(p), rl = strlen(rest);
            return ll == 0 || (rl >= ll && strcmp(rest + rl - ll, p) == 0);
        }
        size_t sl = (size_t)(next - p);
        if (sl) {
            const char *f = memmem(rest, strlen(rest), p, sl);
            if (!f) return false;
            rest = f + sl;
        }
        p = next + 1;
    }
}

static bool bounded_match(const char *v, size_t vn, const char *tok) {
    size_t tn = strlen(tok);
    if (vn < tn || strncmp(v, tok, tn)) return false;
    if (vn == tn) return true;
    char c = v[tn];
    return c == '-' || c == '_' || c == '.' || c == ':' || (c >= '0' && c <= '9');
}

static bool namespace_match(const char *lower, const char *tok, bool bounded) {
    const char *p = lower;
    while (*p) {
        size_t n = bounded ? strcspn(p, "/.:") : strcspn(p, "/");
        if (n && (bounded ? bounded_match(p, n, tok) : (n == strlen(tok) && !strncmp(p, tok, n)))) return true;
        p += n;
        if (*p) p++;
    }
    return false;
}

static bool matcher_matches(const json_t *m, const char *lower, const char *bare) {
    const char *kind = json_get_str(m, "kind"), *tok = json_get_str(m, "token");
    if (!kind || !tok) return false;
    if (!strcmp(kind, "exact")) return strcmp(bare, tok) == 0;
    if (!strcmp(kind, "bounded")) return bounded_match(bare, strlen(bare), tok);
    if (!strcmp(kind, "namespace")) return namespace_match(lower, tok, json_get_bool(m, "bounded", false));
    if (!strcmp(kind, "prefix")) return str_starts(bare, tok);
    if (!strcmp(kind, "glob")) return glob_match(tok, bare);
    return false;
}

static int matcher_rank(const char *k) {
    return str_eq(k, "exact") ? 4 : str_eq(k, "bounded") ? 3 : str_eq(k, "namespace") ? 2 : str_eq(k, "prefix") ? 1 : 0;
}

static int non_wildcard(const char *g) {
    int n = 0;
    for (; *g; g++) n += *g != '*';
    return n;
}

static const char *rank_family(const json_t *cls, const char *subject, bool *tied) {
    const char *win = NULL;
    int wp = 0, wb = 0;
    *tied = false;
    json_t *f;
    json_arr_foreach(json_get_arr(cls, "families"), i, f) {
        const char *glob = json_get_str(f, "glob"), *id = json_get_str(f, "id");
        if (!glob || !id || !glob_match(glob, subject)) continue;
        int p = (int)json_get_int(f, "priority", 0), b = non_wildcard(glob);
        if (win && p == wp && b == wb && strcmp(win, id)) {
            *tied = true;
        } else if (!win || wp < p || (wp == p && wb < b)) {
            win = id, wp = p, wb = b;
            *tied = false;
        }
    }
    return win;
}

static const char *classify_family(const json_t *cls, const char *bare) {
    bool tied;
    const char *f = rank_family(cls, bare, &tied);
    if (!tied) return f;
    /* stripClassNamespace: a `vendor.model` / `vendor:model` id names the product after the class namespace */
    size_t sep = strcspn(bare, ".:");
    if (sep == 0 || !bare[sep] || !bare[sep + 1]) return NULL;
    json_t *m;
    json_arr_foreach(json_get_arr(cls, "matchers"), i, m) {
        const char *tok = json_get_str(m, "token");
        if (tok && strlen(tok) == sep && !strncmp(bare, tok, sep)) {
            f = rank_family(cls, bare + sep + 1, &tied);
            return tied ? NULL : f;
        }
    }
    return NULL;
}

/* revision.ts parseRevision: one to three components separated by `.` or `-` */
static bool parse_revision(const char *s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!s || !*s) return false;
    int count = 0;
    const char *p = s;
    for (;;) {
        if (count == 3 || !isdigit((unsigned char)*p)) return false;
        int v = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (*p++ - '0');
            if (v > 255) return false;
        }
        out[count++] = v;
        if (!*p) return true;
        if (*p != '.' && *p != '-') return false;
        p++;
    }
}

/* revision.ts parseRevisionPrefix: a component running into a letter is a size token (qwen3-32b), not a revision */
static bool parse_revision_prefix(const char *v, int out[3]) {
    int count = 0;
    size_t i = 0;
    out[0] = out[1] = out[2] = 0;
    while (count < 3) {
        size_t start = i;
        while (isdigit((unsigned char)v[i])) i++;
        bool size_tok = isalpha((unsigned char)v[i]);
        int comp = -1;
        if (!size_tok && i > start) {
            comp = 0;
            for (size_t k = start; k < i && comp >= 0; k++) {
                comp = comp * 10 + (v[k] - '0');
                if (comp > 255) comp = -1;
            }
        }
        if (comp < 0) return count > 0;
        out[count++] = comp;
        char sep = v[i];
        if (!sep) break;
        if ((sep != '.' && sep != '-') || !isdigit((unsigned char)v[i + 1])) break;
        i++;
    }
    return true;
}

static bool extract_revision(const json_t *cls, const char *bare, int rev[3]) {
    if (arr_has(json_get_arr(cls, "skipBare"), bare)) return false;
    json_t *r;
    json_arr_foreach(json_get_arr(cls, "revisionPrefixes"), i, r) {
        const char *prefix = json_get_str(r, "prefix");
        if (!prefix) continue;
        const char *tail = NULL;
        if (json_get_bool(r, "anywhere", false)) {
            const char *f = strstr(bare, prefix);
            if (f) tail = f + strlen(prefix);
        } else if (str_starts(bare, prefix)) {
            tail = bare + strlen(prefix);
        }
        if (!tail) continue;
        while (*tail && !isdigit((unsigned char)*tail)) tail++;
        if (!*tail) return false;
        return parse_revision_prefix(tail, rev);
    }
    return false;
}

static const char *bare_of(const char *id) {
    const char *s = strrchr(id, '/');
    return s ? s + 1 : id;
}

static void set_rev(model_identity_t *o, const int rev[3]) {
    memcpy(o->rev, rev, sizeof o->rev);
    o->has_rev = true;
    snprintf(o->rev_str, sizeof o->rev_str, "%d.%d.%d", rev[0], rev[1], rev[2]);
}

/* classifyRanks: the best class matcher (exact > bounded > namespace > prefix > glob, then token length) */
static const json_t *classify_class(const char *lower, const char *bare) {
    const json_t *win = NULL;
    int wr = 0, wl = 0;
    bool tied = false;
    json_t *c;
    json_arr_foreach(json_get_arr(rules(), "classes"), i, c) {
        json_t *m;
        json_arr_foreach(json_get_arr(c, "matchers"), k, m) {
            if (!matcher_matches(m, lower, bare)) continue;
            int r = matcher_rank(json_get_str(m, "kind")), l = (int)strlen(json_get_str(m, "token"));
            if (win && r == wr && l == wl && win != c) {
                tied = true;
            } else if (!win || wr < r || (wr == r && wl < l)) {
                win = c, wr = r, wl = l;
                tied = false;
            }
        }
    }
    return tied ? NULL : win;
}

static const json_t *class_by_id(const char *id) {
    json_t *c;
    json_arr_foreach(json_get_arr(rules(), "classes"), i, c) if (str_eq(json_get_str(c, "id"), id)) return c;
    return NULL;
}

static char *lower_trim(const char *s) {
    char *t = str_trim(s);
    char *l = str_lower(t);
    free(t);
    return l;
}

/* findIdentityOverride: exact bare id (provider-scoped first), then the most specific glob */
static const json_t *find_override(const char *provider_lower, const char *bare_lower) {
    const json_t *agnostic = NULL, *scoped = NULL;
    json_t *c, *o;
    json_arr_foreach(json_get_arr(rules(), "classes"), i, c) {
        json_arr_foreach(json_get_arr(c, "overrides"), k, o) {
            const char *model = json_get_str(o, "model");
            if (!model) continue;
            char *lm = str_lower(model);
            bool same = !strcmp(lm, bare_lower);
            free(lm);
            if (!same) continue;
            const char *p = json_get_str(o, "provider");
            if (p) {
                char *lp = str_lower(p);
                if (!scoped && !strcmp(lp, provider_lower)) scoped = o;
                free(lp);
            } else if (!agnostic) {
                agnostic = o;
            }
        }
    }
    if (scoped) return scoped;
    if (agnostic) return agnostic;
    for (int pass = 0; pass < 2; pass++) { /* provider-scoped patterns, then agnostic ones */
        const json_t *win = NULL;
        int spec = -1;
        bool tied = false;
        json_arr_foreach(json_get_arr(rules(), "classes"), i2, c) {
            json_arr_foreach(json_get_arr(c, "overrides"), k2, o) {
                const char *glob = json_get_str(o, "glob");
                if (!glob || json_get_str(o, "model")) continue;
                const char *p = json_get_str(o, "provider");
                if (pass == 0) {
                    if (!p) continue;
                    char *lp = str_lower(p);
                    bool same = !strcmp(lp, provider_lower);
                    free(lp);
                    if (!same) continue;
                } else if (p) {
                    continue;
                }
                if (!glob_match(glob, bare_lower)) continue;
                int sp = non_wildcard(glob);
                if (win && sp == spec) tied = true;
                else if (!win || spec < sp) {
                    win = o, spec = sp;
                    tied = false;
                }
            }
        }
        if (tied) return NULL;
        if (win) return win;
    }
    return NULL;
}

/* collapseVariantId: drop a declared effort / thinking suffix. *logical is owned. */
static char *collapse_variant(const char *provider_lower, const char *model, const char **effort, bool *thinking) {
    const json_t *col = json_get_obj(rules(), "collapse");
    *effort = NULL;
    *thinking = false;
    char *lower = str_lower(model);
    json_t *fam;
    json_arr_foreach(json_get_arr(col, "effortFamilies"), i, fam) {
        if (str_eq(json_get_str(fam, "provider"), provider_lower) && arr_has(json_get_arr(fam, "aliases"), lower)) {
            free(lower);
            return xstrdup(json_get_str(fam, "logical"));
        }
    }
    const char *bare = bare_of(lower);
    const json_t *win = NULL;
    size_t wl = 0;
    json_t *r;
    json_arr_foreach(json_get_arr(col, "suffixes"), k, r) {
        const char *suffix = json_get_str(r, "suffix"), *except = json_get_str(r, "exceptBarePrefix");
        if (!suffix || !str_ends(lower, suffix)) continue;
        if (except && str_starts(bare, except)) continue;
        if (!win || strlen(suffix) > wl) win = r, wl = strlen(suffix);
    }
    if (win) {
        *thinking = json_get_bool(win, "thinking", false);
        *effort = json_get_str(win, "effort");
        free(lower);
        return xstrndup(model, strlen(model) - wl);
    }
    json_t *lane;
    json_arr_foreach(json_get_arr(col, "lanes"), l, lane) {
        const char *suffix = json_get_str(lane, "suffix"), *bp = json_get_str(lane, "barePrefix");
        if (!suffix || !arr_has(json_get_arr(lane, "providers"), provider_lower) || !str_ends(lower, suffix)) continue;
        size_t tn = strlen(lower) - strlen(suffix);
        char *trimmed = xstrndup(lower, tn);
        const char *tb = bare_of(trimmed);
        const json_t *er = NULL;
        size_t el = 0;
        if (!bp || str_starts(tb, bp)) {
            json_arr_foreach(json_get_arr(col, "suffixes"), m, r) {
                const char *sf = json_get_str(r, "suffix"), *except = json_get_str(r, "exceptBarePrefix");
                if (!json_get_str(r, "effort") || !sf || !str_ends(trimmed, sf)) continue;
                if (except && str_starts(tb, except)) continue;
                if (!er || strlen(sf) > el) er = r, el = strlen(sf);
            }
        }
        free(trimmed);
        if (!er || tn == el || model[tn - el - 1] == '/') continue;
        *effort = json_get_str(er, "effort");
        free(lower);
        return xasprintf("%.*s%s", (int)(tn - el), model, model + tn);
    }
    free(lower);
    return xstrdup(model);
}

void model_identity_classify(const char *provider, const char *id, model_identity_t *out) {
    memset(out, 0, sizeof(*out));
    out->cls = "unknown";
    if (!id) return;
    char *trimmed = str_trim(id);
    char *provider_lower = str_lower(provider ? provider : "");
    char *bare_lower = str_lower(bare_of(trimmed));
    const json_t *ov = find_override(provider_lower, bare_lower);
    char *logical;
    const char *forced_family = NULL, *forced_rev = NULL;
    const json_t *cls = NULL;
    if (ov) {
        const char *l = json_get_str(ov, "logical");
        logical = xstrdup(l ? l : trimmed);
        forced_family = json_get_str(ov, "family");
        forced_rev = json_get_str(ov, "revision");
        out->effort = json_get_str(ov, "effort");
        out->thinking_variant = json_get_bool(ov, "thinkingVariant", false);
        const char *c = json_get_str(ov, "class");
        if (c) {
            cls = class_by_id(c);
            out->cls = cls ? json_get_str(cls, "id") : c;
        }
    } else {
        bool same_len = strlen(trimmed) == strlen(id);
        bool th = false;
        const char *eff = NULL;
        logical = same_len ? collapse_variant(provider_lower, trimmed, &eff, &th) : xstrdup(trimmed);
        out->effort = eff;
        out->thinking_variant = th;
    }
    char *lower = lower_trim(logical);
    const char *bare = bare_of(lower);
    if (!cls && !(ov && json_get_str(ov, "class"))) {
        cls = classify_class(lower, bare);
        if (cls) out->cls = json_get_str(cls, "id");
    }
    if (cls) {
        out->family = classify_family(cls, bare);
        int rev[3];
        if (extract_revision(cls, bare, rev)) set_rev(out, rev);
    }
    if (forced_family) out->family = forced_family;
    if (forced_rev) {
        snprintf(out->rev_str, sizeof out->rev_str, "%s", forced_rev);
        out->has_rev = parse_revision(forced_rev, out->rev);
        if (!out->has_rev) memset(out->rev, 0, sizeof out->rev);
    }
    if (strcmp(logical, trimmed)) snprintf(out->logical, sizeof out->logical, "%s", logical);
    free(lower);
    free(logical);
    free(bare_lower);
    free(provider_lower);
    free(trimmed);
}

static bool is_cls(const model_identity_t *idn, const char *c) { return str_eq(idn->cls, c); }
static bool is_fam(const model_identity_t *idn, const char *f) { return str_eq(idn->family, f); }

static bool rev_gte(const model_identity_t *idn, int a, int b) {
    if (!idn->has_rev) return false;
    int want[3] = {a, b, 0};
    for (int i = 0; i < 3; i++)
        if (idn->rev[i] != want[i]) return idn->rev[i] > want[i];
    return true;
}

static bool anthropic_adaptive_gen(const model_identity_t *idn, int a, int b) {
    if (!is_cls(idn, "anthropic")) return false;
    if (is_fam(idn, "opus")) return rev_gte(idn, a, b);
    if (is_fam(idn, "sonnet") || is_fam(idn, "fable") || is_fam(idn, "mythos")) return rev_gte(idn, 5, 0);
    if (is_fam(idn, "haiku")) return rev_gte(idn, 5, 5);
    return false;
}

/* stripThinkingVariantSuffix != undefined: a bounded pair token anywhere names a reasoning-locked sibling */
static bool has_thinking_pair_token(const char *id) {
    char *lower = str_lower(id);
    bool found = false;
    json_t *t;
    json_arr_foreach(json_get_arr(json_get_obj(rules(), "collapse"), "pairTokens"), i, t) {
        const char *tok = json_sv(t);
        if (!tok || found) continue;
        char *needle = xasprintf("-%s", tok);
        size_t nl = strlen(needle);
        for (const char *p = lower; (p = strstr(p, needle)) && !found; p++) {
            char next = p[nl];
            const char *w = p;
            while (w > lower && (isdigit((unsigned char)w[-1]) || (w[-1] >= 'a' && w[-1] <= 'z'))) w--;
            size_t pl = (size_t)(p - w);
            bool negated = (pl == 3 && !strncmp(w, "non", 3)) || (pl == 2 && !strncmp(w, "no", 2));
            if (!(isdigit((unsigned char)next) || (next >= 'a' && next <= 'z')) && !negated) found = strlen(id) > nl;
        }
        free(needle);
    }
    free(lower);
    return found;
}

/* ---------- rule cascade (compat/cascade.ts) ---------- */

typedef struct {
    json_t *wire, *thinking, *catalog;
    bool reasoning;
} axes_t;

typedef struct {
    const json_t *rule;
    int rank[3];
} ranked_t;

typedef struct {
    const char *axis;
    const json_t *val;
    int rank[3];
} winner_t;
typedef VEC(winner_t) winner_vec_t;

static int rank_cmp(const int *a, const int *b) {
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}

static bool revision_satisfies(const int rev[3], const json_t *terms) {
    json_t *t;
    json_arr_foreach(terms, i, t) {
        int b[3];
        if (!parse_revision(json_get_str(t, "revision"), b)) return false;
        int cmp = rank_cmp(rev, b);
        const char *op = json_get_str(t, "op");
        if (str_eq(op, ">=") ? cmp < 0 : str_eq(op, ">") ? cmp <= 0 : str_eq(op, "<=") ? cmp > 0 : str_eq(op, "<") ? cmp >= 0 : cmp != 0)
            return false;
    }
    return true;
}

static bool model_token(const char *lower, const char *tok) {
    size_t tl = strlen(tok);
    for (const char *p = lower; *p;) {
        while (*p && !(isdigit((unsigned char)*p) || (*p >= 'a' && *p <= 'z'))) p++;
        const char *e = p;
        while (*e && (isdigit((unsigned char)*e) || (*e >= 'a' && *e <= 'z'))) e++;
        if ((size_t)(e - p) == tl && !strncmp(p, tok, tl)) return true;
        p = e;
    }
    return false;
}

/* rankRule: (model-selector exactness, constrained dimensions, priority) when the rule matches */
static bool rank_rule(const json_t *r, const char *provider, const char *api, const model_identity_t *idn, const char *model,
                      const char *lower, int rank[3]) {
    const char *cls = json_get_str(r, "class");
    if (cls && !str_eq(cls, idn->cls)) return false;
    const json_t *providers = json_get_arr(r, "providers"), *apis = json_get_arr(r, "apis");
    if (providers && !arr_has(providers, provider)) return false;
    if (apis && !arr_has(apis, api)) return false;
    if (json_get_arr(r, "upstreams")) return false; /* no request route when a model is built */
    const char *fam = json_get_str(r, "family");
    if (fam && !str_eq(fam, idn->family)) return false;
    const json_t *rev = json_get_arr(r, "revision");
    if (rev && (!idn->has_rev || !revision_satisfies(idn->rev, rev))) return false;
    const json_t *models = json_get_arr(r, "models");
    int exactness = 0;
    if (models) {
        int best = -1;
        json_t *s;
        json_arr_foreach(models, i, s) {
            const char *kind = json_get_str(s, "kind"), *v = json_get_str(s, "value");
            if (!kind || !v) continue;
            bool hit = !strcmp(kind, "exact") ? !strcmp(v, model) : !strcmp(kind, "glob") ? glob_match(v, lower) : model_token(lower, v);
            if (hit && (!strcmp(kind, "exact") ? 2 : 1) > best) best = !strcmp(kind, "exact") ? 2 : 1;
        }
        if (best < 0) return false;
        exactness = best;
    }
    rank[0] = exactness;
    rank[1] = (cls != NULL) + (providers != NULL) + (apis != NULL) + (fam != NULL) + (rev != NULL) + (models != NULL);
    rank[2] = (int)json_get_int(r, "priority", 0);
    return true;
}

static void contest(winner_vec_t *w, const json_t *axes, const int rank[3]) {
    const char *k;
    json_t *v;
    json_obj_foreach(axes, i, k, v) {
        winner_t *held = NULL;
        for (size_t n = 0; n < w->n; n++)
            if (!strcmp(w->v[n].axis, k)) held = &w->v[n];
        if (held) {
            if (rank_cmp(held->rank, rank) >= 0) continue; /* omp throws on a tie: a rule-authoring defect */
            held->val = v;
            memcpy(held->rank, rank, sizeof held->rank);
        } else {
            winner_t nw = {.axis = k, .val = v};
            memcpy(nw.rank, rank, sizeof nw.rank);
            vec_push(w, nw);
        }
    }
}

static json_t *collect(const winner_vec_t *w) {
    json_t *o = json_obj();
    for (size_t n = 0; n < w->n; n++) json_set(o, w->v[n].axis, json_clone(w->v[n].val));
    return o;
}

/* resolveOverIndex for one target */
static void resolve_cascade(axes_t *ax, const char *provider, const char *api, const model_identity_t *idn, const char *model,
                            bool reasoning_in) {
    char *lower = str_lower(model);
    VEC(ranked_t) matched = {0};
    json_t *r;
    json_arr_foreach(json_get_arr(rules(), "cascade"), i, r) {
        ranked_t x = {.rule = r};
        if (rank_rule(r, provider, api, idn, model, lower, x.rank)) vec_push(&matched, x);
    }
    bool reasoning = reasoning_in;
    if (!reasoning) {
        const json_t *upgrade = NULL;
        int urank[3] = {0, 0, 0};
        bool has_efforts = false;
        for (size_t n = 0; n < matched.n; n++) {
            const json_t *th = json_get_obj(matched.v[n].rule, "thinking");
            if (!th) continue;
            if (json_get(th, "upgradeNeutral") && (!upgrade || rank_cmp(urank, matched.v[n].rank) < 0)) {
                upgrade = matched.v[n].rule;
                memcpy(urank, matched.v[n].rank, sizeof urank);
            }
            if (json_get(th, "efforts")) {
                has_efforts = true;
                if (matched.v[n].rank[0] == 2) reasoning = true;
            }
        }
        if (upgrade && has_efforts && json_get_bool(json_get_obj(upgrade, "thinking"), "upgradeNeutral", false)) {
            const char *uc = json_get_str(upgrade, "class");
            if ((uc && strcmp(uc, "unknown")) || json_get_str(upgrade, "family") || json_get_arr(upgrade, "revision") ||
                json_get_arr(upgrade, "models"))
                reasoning = true;
        }
    }
    /* unclassified targets get their provider's ladder too, offered as unverified thinking */
    bool offers = reasoning || str_eq(idn->cls, "unknown");
    winner_vec_t wire = {0}, thinking = {0}, catalog = {0};
    for (size_t n = 0; n < matched.n; n++) {
        const json_t *rule = matched.v[n].rule;
        contest(&wire, json_get_obj(rule, "wire"), matched.v[n].rank);
        contest(&catalog, json_get_obj(rule, "catalog"), matched.v[n].rank);
        if (offers) contest(&thinking, json_get_obj(rule, "thinking"), matched.v[n].rank);
    }
    ax->wire = collect(&wire);
    ax->thinking = collect(&thinking);
    ax->catalog = collect(&catalog);
    ax->reasoning = reasoning;
    free(wire.v);
    free(thinking.v);
    free(catalog.v);
    free(matched.v);
    free(lower);
}

static void axes_free(axes_t *ax) {
    json_free(ax->wire);
    json_free(ax->thinking);
    json_free(ax->catalog);
}

static bool compat_reasoning(bool spec_reasoning, const axes_t *ax) {
    return spec_reasoning || (ax->reasoning && json_get_bool(ax->thinking, "upgradeNeutral", false));
}

/* axes.ts API_COMPAT_RECORDS */
static bool api_record(const char *api, const char *record) {
    if (str_eq(api, "ollama-chat")) return str_eq(record, "ollama");
    if (str_eq(api, "openai-completions")) return str_eq(record, "openai");
    if (str_eq(api, "openrouter")) return str_eq(record, "openai") || str_eq(record, "openai-responses");
    if (str_eq(api, "openai-responses") || str_eq(api, "azure-openai-responses") || str_eq(api, "openai-codex-responses"))
        return str_eq(record, "openai-responses");
    if (str_eq(api, "anthropic-messages")) return str_eq(record, "anthropic");
    if (str_eq(api, "bedrock-converse-stream")) return str_eq(record, "bedrock");
    if (str_eq(api, "devin-agent")) return str_eq(record, "devin");
    if (str_eq(api, "google-generative-ai") || str_eq(api, "google-vertex") || str_eq(api, "google-gemini-cli"))
        return str_eq(record, "google");
    return false;
}

bool compat_api_has_record(const char *api) {
    return str_eq(api, "ollama-chat") || str_eq(api, "openai-completions") || str_eq(api, "openrouter") ||
           str_eq(api, "openai-responses") || str_eq(api, "azure-openai-responses") || str_eq(api, "openai-codex-responses") ||
           str_eq(api, "anthropic-messages") || str_eq(api, "bedrock-converse-stream") || str_eq(api, "devin-agent") ||
           str_eq(api, "google-generative-ai") || str_eq(api, "google-vertex") || str_eq(api, "google-gemini-cli");
}

/* applyWireAxes: an axis lands only when the API maps to one of its declared records */
static void apply_wire(json_t *c, const json_t *wire, const char *api) {
    const json_t *records = json_get_obj(rules(), "wireRecords");
    const char *k;
    json_t *v;
    json_obj_foreach(wire, i, k, v) {
        json_t *rec, *recs = json_get_arr(records, k);
        bool hit = false;
        json_arr_foreach(recs, j, rec) if (api_record(api, json_sv(rec))) hit = true;
        if (hit) json_set(c, k, json_clone(v));
    }
}

/* applyCompatOverrides: `key in compat`, where a record declares some keys without a value */
static void apply_overrides(json_t *c, const json_t *user, const char *const *declared, size_t ndeclared) {
    const char *k;
    json_t *v;
    json_obj_foreach(user, i, k, v) {
        bool known = json_get(c, k) != NULL;
        for (size_t d = 0; !known && d < ndeclared; d++) known = !strcmp(declared[d], k);
        if (known && !json_is_null(v)) json_set(c, k, json_clone(v));
    }
}

/* ---------- hosts (hosts.ts KNOWN_HOSTS) ---------- */

typedef struct {
    const char *name;
    const char *providers[4];
    const char *prefix;
    const char *markers[4];
} host_t;

static const host_t HOSTS[] = {
    {"openai", {"openai"}, NULL, {"api.openai.com"}},
    {"openaiCodex", {0}, NULL, {"chatgpt.com/backend-api", "chat.openai.com/backend-api"}},
    {"azureOpenAI", {"azure"}, NULL, {".openai.azure.com", "azure.com/openai", "models.inference.ai.azure.com"}},
    {"openrouter", {"openrouter"}, NULL, {"openrouter.ai"}},
    {"vercelAIGateway", {"vercel-ai-gateway"}, NULL, {"ai-gateway.vercel.sh"}},
    {"githubCopilot", {"github-copilot"}, NULL, {"githubcopilot.com", "copilot-api."}},
    {"anthropic", {"anthropic"}, NULL, {"api.anthropic.com"}},
    {"deepseekDirect", {"deepseek"}, NULL, {"api.deepseek.com"}},
    {"deepseekFamily", {"deepseek"}, NULL, {"deepseek.com"}},
    {"cerebras", {"cerebras"}, NULL, {"cerebras.ai"}},
    {"zai", {"zai"}, NULL, {"api.z.ai"}},
    {"zhipu", {"zhipu-coding-plan"}, NULL, {"open.bigmodel.cn"}},
    {"kilo", {"kilo"}, NULL, {"api.kilo.ai"}},
    {"alibabaDashscope", {"alibaba-coding-plan", "alibaba-token-plan"}, NULL, {"dashscope", "token-plan."}},
    {"xiaomi", {"xiaomi"}, "xiaomi-token-plan-", {"xiaomimimo.com"}},
    {"xai", {"xai", "xai-oauth"}, NULL, {"api.x.ai"}},
    {"mistral", {"mistral"}, NULL, {"mistral.ai"}},
    {"together", {"together"}, NULL, {"api.together.xyz"}},
    {"fireworks", {0}, NULL, {"fireworks.ai"}},
    {"groq", {"groq"}, NULL, {"api.groq.com"}},
    {"minimax", {"minimax", "minimax-code", "minimax-code-cn"}, NULL, {"api.minimax.io", "api.minimaxi.com"}},
    {"qwenPortal", {"qwen-portal"}, NULL, {"portal.qwen.ai"}},
    {"nvidia", {"nvidia"}, NULL, {"integrate.api.nvidia.com"}},
    {"venice", {"venice"}, NULL, {"api.venice.ai"}},
    {"moonshotNative", {"moonshot", "kimi-code"}, NULL, {"api.moonshot.ai", "api.kimi.com"}},
    {"googleAistudio", {0}, NULL, {"generativelanguage.googleapis.com"}},
    {"opencode", {"opencode-go", "opencode-zen"}, NULL, {"opencode.ai"}},
    {"zenmux", {"zenmux"}, NULL, {"zenmux.ai"}},
    {"chutes", {0}, NULL, {"chutes.ai"}},
};

static const host_t *host_get(const char *name) {
    for (size_t i = 0; i < ARRAY_LEN(HOSTS); i++)
        if (!strcmp(HOSTS[i].name, name)) return &HOSTS[i];
    abort(); /* programming error: unknown host class */
}

static bool url_has(const char *url, const char *h) {
    if (!url || !*url) return false;
    const host_t *s = host_get(h);
    for (size_t i = 0; i < ARRAY_LEN(s->markers) && s->markers[i]; i++)
        if (strcasestr(url, s->markers[i])) return true;
    return false;
}

static bool model_on(const char *provider, const char *url, const char *h) {
    const host_t *s = host_get(h);
    for (size_t i = 0; i < ARRAY_LEN(s->providers) && s->providers[i]; i++)
        if (str_eq(provider, s->providers[i])) return true;
    if (s->prefix && provider && str_starts(provider, s->prefix)) return true;
    return url_has(url, h);
}

/* the hostname of a URL, lower-cased, without a trailing dot; false when the URL has no scheme */
static bool url_host(const char *url, char *host, size_t cap) {
    const char *p = url ? strstr(url, "://") : NULL;
    if (!p) return false;
    p += 3;
    const char *at = NULL;
    for (const char *q = p; *q && *q != '/' && *q != '?' && *q != '#'; q++)
        if (*q == '@') at = q;
    if (at) p = at + 1;
    size_t n;
    if (*p == '[') {
        const char *e = strchr(p, ']');
        if (!e) return false;
        n = (size_t)(e - p + 1);
    } else {
        n = strcspn(p, ":/?#");
    }
    if (n == 0 || n >= cap) return false;
    for (size_t i = 0; i < n; i++) host[i] = (char)tolower((unsigned char)p[i]);
    host[n] = 0;
    if (n > 1 && host[n - 1] == '.') host[n - 1] = 0;
    return true;
}

bool compat_local_base_url(const char *base_url) {
    char host[256];
    if (!url_host(base_url, host, sizeof host)) return false;
    if (!strcmp(host, "localhost") || !strcmp(host, "127.0.0.1") || !strcmp(host, "0.0.0.0") || !strcmp(host, "[::1]"))
        return true;
    if (str_starts(host, "10.") || str_starts(host, "192.168.")) return true;
    if (str_starts(host, "172.")) {
        int b = atoi(host + 4);
        const char *d = host + 4;
        while (isdigit((unsigned char)*d)) d++;
        if (d > host + 4 && *d == '.' && b >= 16 && b <= 31) return true;
    }
    return str_ends(host, ".local");
}

static const char *disable_mode_for(const char *fmt) {
    if (str_eq(fmt, "openrouter")) return "openrouter-enabled-false";
    if (str_eq(fmt, "zai") || str_eq(fmt, "kimi")) return "zai-thinking-disabled";
    if (str_eq(fmt, "qwen")) return "qwen-enable-thinking-false";
    if (str_eq(fmt, "qwen-chat-template")) return "qwen-template-false";
    if (str_eq(fmt, "chat-template")) return "chat-template-thinking-false";
    return "lowest-effort";
}

static bool official_openai(const char *provider, const char *url) {
    if (!str_eq(provider, "openai")) return false;
    if (!url || !*url) return true;
    char host[256];
    return url_host(url, host, sizeof host) && !strcmp(host, "api.openai.com");
}

static const char *healing_pattern(const char *provider, const model_identity_t *idn, const char *url) {
    if (str_eq(provider, "kimi-code") || str_eq(provider, "moonshot") ||
        (is_cls(idn, "kimi") && idn->family && str_starts(idn->family, "k2")))
        return "kimi";
    if (is_cls(idn, "deepseek")) return "dsml";
    return official_openai(provider, url) ? NULL : "thinking";
}

static bool local_backend_provider(const char *p) {
    return str_eq(p, "llama.cpp") || str_eq(p, "lm-studio") || str_eq(p, "vllm") || str_eq(p, "ollama");
}

/* overlayEffortMapAxis: a rule's map is the base, sparse spec overrides overlay it key by key */
static void overlay_effort_map(json_t *c, const axes_t *ax, const json_t *user) {
    const json_t *axis = json_get_obj(ax->wire, "reasoningEffortMap"), *spec = json_get_obj(user, "reasoningEffortMap");
    if (!axis || !spec) return;
    json_t *m = json_clone(axis);
    const char *k;
    json_t *v;
    json_obj_foreach(spec, i, k, v) json_set(m, k, json_clone(v));
    json_set(c, "reasoningEffortMap", m);
}

/* ---------- openai-completions / openrouter chat record (resolve.ts) ---------- */

/* the record's keys that exist without a value in the detected baseline */
static const char *const CHAT_DECLARED[] = {
    "trustExplicitThinkingOnly", "kimiApiFormat", "thinkingKeep", "mistralReasoningContentParts",
    "syntheticReasoningContentFallback", "cacheControlFormat", "promptCacheBreakpointTtl", "openRouterRouting",
    "vercelGatewayRouting", "extraBody", "toolSchemaFlavor", "streamFirstEventTimeoutMs", "streamIdleTimeoutMs",
    "stripThinkingHistory", "officialEndpoint", "streamMarkupHealingPattern", "promptCacheSessionHeader", "thinkingLoopGuard"};

static json_t *chat_compat(const char *provider, const char *backend, const char *url, const model_identity_t *idn,
                           bool spec_reasoning, const json_t *user, const axes_t *ax) {
    const char *u = url ? url : "";
    bool reasoning = compat_reasoning(spec_reasoning, ax);
#define ON(h) model_on(provider, u, h)
#define URL(h) url_has(u, h)
    bool is_zai = ON("zai"), is_zhipu = ON("zhipu"), moonshot = ON("moonshotNative"), xiaomi_host = ON("xiaomi");
    bool ds_direct = ON("deepseekDirect");
    bool ds_family = ON("deepseekFamily") || is_cls(idn, "deepseek");
    bool ds_reasoning = ds_family && reasoning;
    bool local_compat = !str_eq(backend, "litellm") && (local_backend_provider(backend) || compat_local_base_url(u));
    bool local_serving = local_compat || compat_local_base_url(u);
    bool opencode_host = ON("opencode");
    bool opencode_provider = str_eq(provider, "opencode-go") || str_eq(provider, "opencode-zen");
    bool ds_direct_reasoning = ds_direct && ds_reasoning;
    bool venice = ON("venice");
    bool kimi = is_cls(idn, "kimi");
    bool requires_enabled_thinking = moonshot && kimi && is_fam(idn, "k2.7-code");
    bool xiaomi_mimo = xiaomi_host && is_cls(idn, "mimo");
    bool openrouter = ON("openrouter");

    bool cerebras = ON("cerebras"), cerebras_host = URL("cerebras"), kilo = ON("kilo"), alibaba = ON("alibabaDashscope");
    bool grok = ON("xai"), mistral = ON("mistral"), aistudio = URL("googleAistudio"), openai_host = ON("openai");
    bool azure = ON("azureOpenAI"), vercel = ON("vercelAIGateway"), together = ON("together"), fireworks = URL("fireworks");
    bool groq = ON("groq"), minimax = ON("minimax"), qwen_portal = ON("qwenPortal");
    bool moonshot_kimi = kimi && moonshot, moonshot_k3 = moonshot_kimi && is_fam(idn, "k3");
    bool preserved_kimi = moonshot_kimi && is_fam(idn, "k2.6");
    bool anthropic_model = ON("anthropic") || is_cls(idn, "anthropic");
    bool qwen = is_cls(idn, "qwen");
    bool zai_effort = (is_zai || is_zhipu) && is_cls(idn, "glm") && rev_gte(idn, 5, 2);
    bool nonstandard = cerebras || grok || mistral || aistudio || URL("chutes") || URL("deepseekFamily") || fireworks ||
                       alibaba || is_zai || is_zhipu || kilo || qwen || xiaomi_host || moonshot || opencode_host;
    bool use_max_tokens = mistral || moonshot || is_zai || is_zhipu || URL("chutes") || fireworks || ds_direct;
    bool cache_bp = official_openai(provider, u) && is_cls(idn, "openai") && rev_gte(idn, 5, 6);
    bool multi_sys = !minimax && !alibaba && !qwen_portal && !qwen &&
                     (openai_host || azure || openrouter || cerebras || together || fireworks || groq || ds_family ||
                      mistral || grok || is_zai || is_zhipu || str_eq(provider, "github-copilot") || str_eq(provider, "zenmux"));
    int64_t idle = -1;
    if (is_cls(idn, "glm") && rev_gte(idn, 5, 0) && !is_fam(idn, "vision") && (URL("zai") || URL("zhipu") || URL("opencode")))
        idle = 600000;
    else if (is_cls(idn, "mimo") && URL("xiaomi")) idle = 300000;
    else if (reasoning && kimi && (is_fam(idn, "k3") || is_fam(idn, "k2.7-code")) && URL("moonshotNative")) idle = 300000;
    else if (reasoning && is_cls(idn, "deepseek") && URL("deepseekDirect")) idle = 300000;
    else if (local_serving) idle = 300000;
    const char *fmt = (kimi && !is_fam(idn, "k3") && URL("moonshotNative")) || URL("zai") || URL("zhipu") ||
                              (is_cls(idn, "mimo") && URL("xiaomi"))
                          ? "zai"
                      : URL("openrouter")                    ? "openrouter"
                      : qwen && URL("nvidia")                ? "qwen-chat-template"
                      : qwen && (fireworks || URL("venice")) ? "openai"
                      : URL("alibabaDashscope") || qwen      ? "qwen"
                                                             : "openai";
    const char *heal = healing_pattern(provider, idn, u);

    json_t *c = json_obj();
    json_set_bool(c, "supportsStore", !nonstandard);
    json_set_bool(c, "supportsDeveloperRole", openai_host || azure);
    json_set_bool(c, "supportsMultipleSystemMessages", multi_sys);
    json_set_bool(c, "supportsReasoningEffort", !grok && !xiaomi_mimo && (!(is_zai || is_zhipu) || zai_effort));
    json_set_bool(c, "supportsReasoningParams", true);
    json_set_bool(c, "supportsSamplingParams", true);
    json_set_bool(c, "supportsPenaltyAndStopParams", !(grok && reasoning));
    json_set(c, "reasoningEffortMap", json_obj());
    json_set_bool(c, "supportsUsageInStreaming", !cerebras_host);
    json_set_bool(c, "alwaysSendMaxTokens", kimi);
    json_set_bool(c, "disableReasoningOnForcedToolChoice", (kimi && !moonshot_k3) || anthropic_model);
    json_set_bool(c, "disableReasoningOnToolChoice", ds_family && reasoning && !openrouter);
    json_set_bool(c, "disableReasoningWithTools", false);
    json_set_bool(c, "supportsToolChoice", !ds_direct_reasoning);
    json_set_bool(c, "supportsForcedToolChoice", !requires_enabled_thinking && !(opencode_host && ds_reasoning));
    json_set_bool(c, "supportsNamedToolChoice", true);
    json_set_str(c, "maxTokensField", use_max_tokens ? "max_tokens" : "max_completion_tokens");
    json_set_bool(c, "requiresToolResultName", mistral);
    json_set_bool(c, "requiresAssistantAfterToolResult", mistral);
    json_set_bool(c, "requiresThinkingAsText", mistral);
    json_set_bool(c, "requiresMistralToolIds", mistral);
    json_set_str(c, "thinkingFormat", fmt);
    json_set_str(c, "reasoningDisableMode", venice ? "venice-disable-thinking" : disable_mode_for(fmt));
    json_set_bool(c, "omitReasoningEffort", false);
    json_set_bool(c, "includeEncryptedReasoning", true);
    json_set_bool(c, "filterReasoningHistory", openrouter && anthropic_model);
    if (preserved_kimi) json_set_str(c, "thinkingKeep", "all");
    json_set_str(c, "reasoningContentField", "reasoning_content");
    json_set_bool(c, "requiresReasoningContentForToolCalls",
                  (kimi && !opencode_provider) || ds_reasoning || xiaomi_mimo || (openrouter && reasoning));
    json_set_bool(c, "requiresReasoningContentForAllAssistantTurns", (ds_reasoning || xiaomi_mimo) && !openrouter);
    json_set_bool(c, "allowsSyntheticReasoningContentForToolCalls", (!ds_family || !reasoning) && !xiaomi_mimo);
    json_set_bool(c, "replayReasoningContent", local_compat);
    json_set_bool(c, "qwenPreserveThinking", (str_eq(fmt, "qwen") || str_eq(fmt, "qwen-chat-template")) && local_compat);
    json_set_bool(c, "qwenTemplateReasoningEffort", false);
    json_set_bool(c, "requiresAssistantContentForToolCalls", kimi || ds_direct_reasoning);
    if (openrouter && anthropic_model) json_set_str(c, "cacheControlFormat", "anthropic");
    json_set_bool(c, "supportsPromptCacheBreakpoints", cache_bp);
    if (cache_bp) json_set_str(c, "promptCacheBreakpointTtl", "30m");
    json_set_bool(c, "isOpenRouterHost", openrouter);
    json_set_str(c, "wireModelIdMode", URL("openrouter") ? "openrouter" : "raw");
    json_set_bool(c, "isVercelGatewayHost", vercel);
    json_set_bool(c, "supportsStrictMode", URL("openai") || URL("azureOpenAI") || URL("cerebras") || URL("together") ||
                                               URL("openrouter") || URL("deepseekFamily"));
    json_set_str(c, "toolStrictMode", cerebras_host ? "all_strict" : "mixed");
    if (moonshot || kimi) json_set_str(c, "toolSchemaFlavor", "moonshot-mfjs");
    else if (local_compat) json_set_str(c, "toolSchemaFlavor", "grammar");
    if (local_serving) json_set_int(c, "streamFirstEventTimeoutMs", 0);
    if (idle > 0) json_set_int(c, "streamIdleTimeoutMs", idle);
    json_set_bool(c, "stripDeepseekSpecialTokens", false);
    if (official_openai(provider, u)) json_set_bool(c, "officialEndpoint", true);
    if (heal) json_set_str(c, "streamMarkupHealingPattern", heal);
    json_set_bool(c, "reasoningDeltasMayBeCumulative", false);
    json_set_bool(c, "emptyLengthFinishIsContextError", false);
    json_set_bool(c, "usesOpenAIToolCallIdLimit", false);
    if (URL("xai")) json_set_str(c, "promptCacheSessionHeader", "x-grok-conv-id");
    json_set_bool(c, "dropThinkingWhenReasoningEffort", false);
    json_set_bool(c, "nativeKimiK3Reasoning", false);
    json_set_bool(c, "zaiReasoningEffortDialect", false);
    json_set_bool(c, "clampOutputToModelMax", local_compat);
    json_set_bool(c, "stripImageInput", false);
    json_set_bool(c, "rejectRootObjectUnion", false);
    json_set_bool(c, "retryWithoutStrictOnGrammarError", false);
    json_set_bool(c, "supportsPromptCacheKey", false);
#undef ON
#undef URL

    apply_wire(c, ax->wire, "openai-completions");
    apply_overrides(c, user, CHAT_DECLARED, ARRAY_LEN(CHAT_DECLARED));
    overlay_effort_map(c, ax, user);

    /* fixupOpenAICompat */
    json_t *eb = json_get_obj(c, "extraBody");
    json_t *ebt = eb ? json_get_obj(eb, "thinking") : NULL;
    if (ds_direct_reasoning && ebt && str_eq(json_get_str(ebt, "type"), "enabled")) {
        json_del(eb, "thinking");
        if (!json_len(eb)) json_del(c, "extraBody");
    }
    if (!json_get(user, "reasoningDisableMode") && !json_get(ax->wire, "reasoningDisableMode")) {
        json_set_str(c, "reasoningDisableMode", requires_enabled_thinking ? "omit"
                                                : ds_direct_reasoning     ? "zai-thinking-disabled"
                                                : venice                  ? "venice-disable-thinking"
                                                                          : disable_mode_for(json_get_str(c, "thinkingFormat")));
    }
    if (!json_get(user, "omitReasoningEffort") && !json_get(ax->wire, "omitReasoningEffort") &&
        !json_get_bool(c, "supportsReasoningEffort", true))
        json_set_bool(c, "omitReasoningEffort", true);
    const json_t *axis_wt = reasoning ? json_get_obj(ax->wire, "whenThinking") : NULL;
    const json_t *wt = json_get_obj(user, "whenThinking");
    if (!wt) wt = axis_wt;
    json_t *ds_policy = NULL;
    if (!wt && ds_direct_reasoning) {
        ds_policy = json_obj();
        json_t *body = json_get_obj(c, "extraBody") ? json_clone(json_get(c, "extraBody")) : json_obj();
        json_t *th = json_obj();
        json_set_str(th, "type", "enabled");
        json_set(body, "thinking", th);
        json_set(ds_policy, "extraBody", body);
        wt = ds_policy;
    }
    if (wt) { /* the whenThinking variant: the full record with the policy's overrides applied */
        json_t *var = json_clone(c);
        json_del(var, "whenThinking");
        apply_overrides(var, wt, CHAT_DECLARED, ARRAY_LEN(CHAT_DECLARED));
        if (!json_get(wt, "reasoningDisableMode"))
            json_set_str(var, "reasoningDisableMode",
                         venice ? "venice-disable-thinking" : disable_mode_for(json_get_str(var, "thinkingFormat")));
        if (!json_get(wt, "omitReasoningEffort") && !json_get_bool(var, "supportsReasoningEffort", true))
            json_set_bool(var, "omitReasoningEffort", true);
        json_set(c, "whenThinking", var);
    } else {
        json_del(c, "whenThinking");
    }
    json_free(ds_policy);
    return c;
}

/* ---------- Responses record ---------- */

static const char *const RESPONSES_DECLARED[] = {
    "trustExplicitThinkingOnly", "promptCacheBreakpointTtl", "statefulResponses", "thinkingLoopGuard",
    "syntheticReasoningContentFallback", "openRouterRouting", "vercelGatewayRouting", "toolSchemaFlavor", "officialEndpoint",
    "cacheControlFormat", "streamMarkupHealingPattern", "promptCacheSessionHeader", "streamFirstEventTimeoutMs",
    "streamIdleTimeoutMs"};

static bool official_codex(const char *url) {
    if (!url || !*url) return true;
    static const char base[] = "https://chatgpt.com/backend-api";
    size_t n = strlen(base);
    if (strncasecmp(url, base, strlen("https://chatgpt.com")) || strncmp(url + strlen("https://chatgpt.com"), "/backend-api", n - strlen("https://chatgpt.com")))
        return false;
    char c = url[n];
    return !c || c == '/' || c == '?' || c == '#';
}

static json_t *responses_compat(const char *provider, const char *backend, const char *url, const model_identity_t *idn,
                                bool spec_reasoning, const json_t *user, const axes_t *ax, const char *api) {
    const char *u = url ? url : "";
#define ON(h) model_on(provider, u, h)
#define URL(h) url_has(u, h)
    bool azure = ON("azureOpenAI"), openrouter = ON("openrouter"), openai_url = URL("openai");
    bool vercel = ON("vercelAIGateway"), xai = ON("xai");
    bool cache_bp = official_openai(provider, u) && is_cls(idn, "openai") && rev_gte(idn, 5, 6);
    const char *fmt = openrouter ? "openrouter" : "openai";
    bool reasoning = compat_reasoning(spec_reasoning, ax);
    bool local_serving = (!str_eq(backend, "litellm") && local_backend_provider(backend)) || compat_local_base_url(u);
    bool anthropic_model = is_cls(idn, "anthropic"), ds_family = is_cls(idn, "deepseek"), kimi = is_cls(idn, "kimi");
    bool local_compat = !str_eq(backend, "litellm") && (local_backend_provider(backend) || compat_local_base_url(u));

    json_t *c = json_obj();
    json_set_bool(c, "supportsDeveloperRole", azure || openai_url || URL("githubCopilot"));
    json_set_bool(c, "supportsStrictMode", azure || URL("openai") || URL("azureOpenAI") || URL("cerebras") || URL("together") ||
                                               URL("openrouter") || URL("deepseekFamily"));
    json_set_bool(c, "supportsReasoningEffort", !xai);
    json_set_bool(c, "supportsLongPromptCacheRetention", openai_url);
    json_set_bool(c, "supportsPromptCacheBreakpoints", cache_bp);
    if (cache_bp) json_set_str(c, "promptCacheBreakpointTtl", "30m");
    json_set_bool(c, "strictResponsesPairing", URL("azureOpenAI"));
    json_set_bool(c, "supportsImageDetailOriginal", openai_url || URL("azureOpenAI") || (azure && !*u) || URL("openaiCodex"));
    json_set_bool(c, "supportsReasoningSummary", !xai);
    json_set_bool(c, "supportsAllTurnsReasoningContext", false);
    json_set_bool(c, "supportsConfigurationUpdate", false);
    json_set_bool(c, "supportsSteering", false);
    json_set_bool(c, "requiresReasoningOffJuiceInstruction", false);
    json_set_bool(c, "stripImageInput", false);
    json_set(c, "reasoningEffortMap", json_obj());
    json_set_bool(c, "supportsReasoningParams", true);
    json_set_bool(c, "supportsSamplingParams", true);
    json_set_bool(c, "supportsPenaltyAndStopParams", !xai);
    json_set_str(c, "thinkingFormat", fmt);
    json_set_str(c, "reasoningDisableMode", disable_mode_for(fmt));
    json_set_bool(c, "omitReasoningEffort", false);
    json_set_bool(c, "includeEncryptedReasoning", true);
    json_set_bool(c, "filterReasoningHistory", openrouter && anthropic_model);
    json_set_bool(c, "disableReasoningOnForcedToolChoice", kimi);
    json_set_bool(c, "disableReasoningOnToolChoice", ds_family && reasoning && !openrouter);
    json_set_bool(c, "disableReasoningWithTools", false);
    json_set_bool(c, "supportsToolChoice", true);
    json_set_bool(c, "supportsForcedToolChoice", true);
    json_set_bool(c, "supportsNamedToolChoice", true);
    json_set_str(c, "reasoningContentField", "reasoning_content");
    json_set_bool(c, "requiresReasoningContentForToolCalls", (kimi || (ds_family && reasoning) || (openrouter && reasoning)) && reasoning);
    json_set_bool(c, "requiresReasoningContentForAllAssistantTurns", ds_family && reasoning && !openrouter);
    json_set_bool(c, "allowsSyntheticReasoningContentForToolCalls", !ds_family || !reasoning);
    json_set_bool(c, "replayReasoningContent", false);
    json_set_bool(c, "qwenPreserveThinking", false);
    json_set_bool(c, "qwenTemplateReasoningEffort", false);
    json_set_bool(c, "requiresThinkingAsText", false);
    json_set_bool(c, "requiresMistralToolIds", false);
    json_set_bool(c, "requiresToolResultName", false);
    json_set_bool(c, "requiresAssistantAfterToolResult", false);
    json_set_bool(c, "requiresAssistantContentForToolCalls", kimi);
    json_set_bool(c, "isOpenRouterHost", openrouter);
    json_set_bool(c, "isVercelGatewayHost", vercel);
    json_set_str(c, "wireModelIdMode", openrouter ? "openrouter" : "raw");
    if (kimi) json_set_str(c, "toolSchemaFlavor", "moonshot-mfjs");
    json_set_bool(c, "alwaysSendMaxTokens", kimi);
    json_set_bool(c, "clampOutputToModelMax", local_compat);
    json_set_bool(c, "supportsObfuscationOptOut", openai_url || str_eq(provider, "openai"));
    json_set_bool(c, "storeResponses", false);
    json_set_bool(c, "connectionBoundNativeHistory", false);
    json_set_bool(c, "officialEndpoint", official_openai(provider, u) || (str_eq(api, "openai-codex-responses") && official_codex(u)));
    json_set_bool(c, "harmonyLeakMitigation", false);
    json_set_bool(c, "rejectRootObjectUnion", false);
    json_set_bool(c, "retryWithoutStrictOnGrammarError", false);
    if (openrouter && anthropic_model) json_set_str(c, "cacheControlFormat", "anthropic");
    json_set_bool(c, "stripDeepseekSpecialTokens", false);
    const char *heal = healing_pattern(provider, idn, u);
    if (heal) json_set_str(c, "streamMarkupHealingPattern", heal);
    json_set_bool(c, "reasoningDeltasMayBeCumulative", false);
    json_set_bool(c, "emptyLengthFinishIsContextError", false);
    json_set_bool(c, "usesOpenAIToolCallIdLimit", false);
    if (URL("xai")) json_set_str(c, "promptCacheSessionHeader", "x-grok-conv-id");
    if (local_serving) {
        json_set_int(c, "streamFirstEventTimeoutMs", 0);
        json_set_int(c, "streamIdleTimeoutMs", 300000);
    } else {
        json_t *fe = json_get(user, "streamFirstEventTimeoutMs"), *it = json_get(user, "streamIdleTimeoutMs");
        if (fe) json_set(c, "streamFirstEventTimeoutMs", json_clone(fe));
        if (it) json_set(c, "streamIdleTimeoutMs", json_clone(it));
    }
#undef ON
#undef URL
    apply_wire(c, ax->wire, api);
    apply_overrides(c, user, RESPONSES_DECLARED, ARRAY_LEN(RESPONSES_DECLARED));
    overlay_effort_map(c, ax, user);
    const json_t *wire_map = json_get_obj(ax->wire, "reasoningEffortMap");
    if (xai && wire_map) { /* the rules' canonical first-party xAI map beats stale overrides */
        json_t *m = json_get_obj(c, "reasoningEffortMap");
        const char *k;
        json_t *v;
        json_obj_foreach(wire_map, i, k, v) json_set(m, k, json_clone(v));
        if (!json_get(wire_map, "xhigh")) json_del(m, "xhigh");
        if (!json_get(wire_map, "max")) json_del(m, "max");
    }
    if (!json_get(user, "reasoningDisableMode") && !json_get(ax->wire, "reasoningDisableMode"))
        json_set_str(c, "reasoningDisableMode", disable_mode_for(json_get_str(c, "thinkingFormat")));
    if (!json_get(user, "omitReasoningEffort") && !json_get(ax->wire, "omitReasoningEffort") &&
        !json_get_bool(c, "supportsReasoningEffort", true))
        json_set_bool(c, "omitReasoningEffort", true);
    if (str_eq(provider, "xai-oauth") && json_get_bool(ax->wire, "supportsReasoningEffort", false) &&
        json_get_bool(user, "supportsReasoningEffort", true)) {
        json_set_bool(c, "supportsReasoningEffort", true);
        json_set_bool(c, "omitReasoningEffort", false);
    }
    return c;
}

/* ---------- other records ---------- */

/* anchored host patterns of compat/anthropic.ts, on the parsed hostname */
static bool host_is_label_dot(const char *host, const char *suffix) { /* <label>.<suffix>, label = [a-z0-9-]+ at any depth */
    size_t hl = strlen(host), sl = strlen(suffix);
    if (hl <= sl + 1 || strcmp(host + hl - sl, suffix) || host[hl - sl - 1] != '.') return false;
    return true;
}

static bool azure_anthropic(const char *url) {
    char host[256];
    if (!url_host(url, host, sizeof host)) return false;
    return host_is_label_dot(host, "inference.ai.azure.com") || host_is_label_dot(host, "services.ai.azure.com");
}

/* hosts.ts isBedrockRouteUrl(url, "anthropic") */
static bool bedrock_anthropic_route(const char *url) {
    if (!url || strncasecmp(url, "https://", 8)) return false;
    char host[256];
    if (!url_host(url, host, sizeof host)) return false;
    bool runtime = (str_starts(host, "bedrock-runtime.") || str_starts(host, "bedrock-runtime-fips.")) && str_ends(host, ".amazonaws.com");
    bool mantle = str_starts(host, "bedrock-mantle.") && str_ends(host, ".api.aws");
    bool vpce = str_starts(host, "vpce-") && str_ends(host, ".vpce.amazonaws.com") && (strstr(host, ".bedrock-runtime") || strstr(host, ".bedrock-mantle."));
    if (!runtime && !mantle && !vpce) return false;
    const char *path = strstr(url, "://") + 3;
    path += strcspn(path, "/?#");
    if (*path != '/') return false;
    return !strncmp(path, "/anthropic", 10) && (!path[10] || path[10] == '/' || path[10] == '?' || path[10] == '#');
}

static bool anthropic_signing_proxy(const char *url) {
    if (!url) return false;
    if (url_has(url, "githubCopilot") || url_has(url, "zenmux")) return true;
    const char *g = strcasestr(url, "gateway.ai.cloudflare.com/");
    if (g) {
        const char *a = strcasestr(g + strlen("gateway.ai.cloudflare.com/") + 1, "/anthropic");
        if (a && (!a[10] || a[10] == '/')) return true;
    }
    const char *v = strcasestr(url, "aiplatform.googleapis.com/");
    if (v && strcasestr(v + strlen("aiplatform.googleapis.com/") + 1, "/publishers/anthropic/")) return true;
    char host[256];
    if (url_host(url, host, sizeof host) && (str_starts(host, "bedrock-runtime.") || strstr(host, ".bedrock-runtime.")) && str_ends(host, ".amazonaws.com"))
        return true;
    return bedrock_anthropic_route(url) || azure_anthropic(url);
}

static const char *const ANTHROPIC_DECLARED[] = {"bedrockMessagesApi", "maxImageDimension", "maxImagePayloadBytes", "thinkingLoopGuard",
                                                 "stripThinkingHistory", "fastMode", "streamIdleTimeoutMs"};

static json_t *anthropic_compat(const char *provider, const char *url, const model_identity_t *idn, bool spec_reasoning,
                                const json_t *user, const axes_t *ax) {
    const char *u = url ? url : "";
    bool official = !*u || !strcasecmp(u, "https://api.anthropic.com") || !strncasecmp(u, "https://api.anthropic.com/", 26);
    bool copilot = model_on(provider, u, "githubCopilot"), zenmux = model_on(provider, u, "zenmux");
    bool kimi_mandatory = is_cls(idn, "kimi") && (is_fam(idn, "k2.7-code") || is_fam(idn, "k3"));
    bool requires_thinking = model_on(provider, u, "moonshotNative") && kimi_mandatory;
    bool bedrock = bedrock_anthropic_route(u);
    bool signing = official || copilot || zenmux || anthropic_signing_proxy(u);
    json_t *c = json_obj();
    json_set_bool(c, "officialEndpoint", official);
    json_set_bool(c, "signingEndpoint", signing);
    json_set_bool(c, "supportsContextManagement", true);
    json_set_bool(c, "supportsServerCompaction", false);
    json_set_bool(c, "firstPartyProvider", false);
    json_set_bool(c, "honorsClaudeCodeBetaKillSwitch", false);
    json_set_bool(c, "supportsOutputEffort", true);
    json_set_bool(c, "disableStrictTools", azure_anthropic(u) || bedrock);
    if (bedrock) json_set_bool(c, "bedrockMessagesApi", true);
    json_set_bool(c, "disableAdaptiveThinking", false);
    json_set_bool(c, "allowAnthropicHeaderOverrides", false);
    json_set_bool(c, "supportsEagerToolInputStreaming", official);
    json_set_bool(c, "supportsLongCacheRetention", official);
    json_set_bool(c, "supportsMidConversationSystem", official && !is_fam(idn, "sonnet") && anthropic_adaptive_gen(idn, 4, 8));
    json_set_bool(c, "supportsTurnScopedSystem", false);
    json_set_bool(c, "supportsMidConversationToolChanges", false);
    json_set_bool(c, "supportsPerMessageEffort", false);
    json_set_bool(c, "supportsThinkingBindingControls", false);
    json_set_bool(c, "supportsBetweenToolsThinking", false);
    json_set_bool(c, "supportsForcedToolChoice", !requires_thinking && !is_fam(idn, "fable") && !is_fam(idn, "mythos"));
    json_set_bool(c, "supportsSamplingParams", true);
    json_set_bool(c, "requiresToolResultId", false);
    json_set_bool(c, "requiresThinkingEnabled", requires_thinking);
    json_set_bool(c, "requiresThinkingPresence", false);
    json_set_bool(c, "replayUnsignedThinking", !signing && (compat_reasoning(spec_reasoning, ax) || model_on(provider, u, "deepseekFamily")));
    json_set_bool(c, "escapeBuiltinToolNames", false);
    json_set_bool(c, "injectClaudeCodeInstruction", true);
    json_set_bool(c, "stripImageInput", false);
    json_t *idle = json_get(user, "streamIdleTimeoutMs");
    if (idle) json_set(c, "streamIdleTimeoutMs", json_clone(idle));
    apply_wire(c, ax->wire, "anthropic-messages");
    apply_overrides(c, user, ANTHROPIC_DECLARED, ARRAY_LEN(ANTHROPIC_DECLARED));
    return c;
}

static json_t *bedrock_compat(bool spec_reasoning, const json_t *user, const axes_t *ax) {
    static const char *const declared[] = {"streamIdleTimeoutMs"};
    json_t *c = json_obj();
    json_set_str(c, "promptCacheMode", "none");
    json_set_bool(c, "supportsServerCompaction", false);
    json_set_bool(c, "supportsLongPromptCacheRetention", false);
    json_set_int(c, "promptCacheMinimumTokens", 0);
    json_set_int(c, "promptCacheMaximumCheckpoints", 0);
    json_set_bool(c, "supportsForcedToolChoice", true);
    json_set_bool(c, "supportsThinkingBindingControls", true);
    if (compat_reasoning(spec_reasoning, ax)) json_set_int(c, "streamIdleTimeoutMs", 600000);
    apply_wire(c, ax->wire, "bedrock-converse-stream");
    apply_overrides(c, user, declared, ARRAY_LEN(declared));
    return c;
}

static json_t *ollama_compat(const json_t *user, const axes_t *ax) {
    json_t *c = json_obj();
    json_set_bool(c, "stripThinkingHistory", false);
    apply_wire(c, ax->wire, "ollama-chat");
    apply_overrides(c, user, NULL, 0);
    return c;
}

static json_t *devin_compat(const json_t *user, const axes_t *ax) {
    json_t *c = json_obj();
    json_set_bool(c, "trustExplicitThinkingOnly", false);
    json_set_bool(c, "modelRouter", false);
    json_set_bool(c, "supportsParallelToolCalls", false);
    apply_wire(c, ax->wire, "devin-agent");
    apply_overrides(c, user, NULL, 0);
    return c;
}

static json_t *google_compat(const char *api, const json_t *user, const axes_t *ax) {
    static const char *const declared[] = {"maxImageDimension", "maxImagePayloadBytes"};
    static const char *const flags[] = {"supportsFunctionPartId", "requiresSkipThoughtSignature",
                                        "requiresSkipThoughtSignatureOnFirstFunctionCall", "dropUnsignedThinking",
                                        "ccaLegacyParametersSchema", "multimodalFunctionResponse", "flashStreamLeakWorkaround",
                                        "claudeThinkingBetaHeader", "antigravityClaudeToolMode", "stripImageInput"};
    json_t *c = json_obj();
    for (size_t i = 0; i < ARRAY_LEN(flags); i++) json_set_bool(c, flags[i], false);
    apply_wire(c, ax->wire, api);
    apply_overrides(c, user, declared, ARRAY_LEN(declared));
    return c;
}

/* ---------- thinking (resolveThinkingPolicy) ---------- */

static const char *const EFFORT_ORDER[] = {"minimal", "low", "medium", "high", "xhigh", "max"};

static int effort_idx(const char *e) {
    for (int i = 0; i < 6; i++)
        if (str_eq(e, EFFORT_ORDER[i])) return i;
    return -1;
}

/* models-config-schema-bundle.ts ModelThinkingSchema pipe: `efforts` beats legacy `levels`, which beats the
 * `minLevel`..`maxLevel` range; the legacy keys never survive */
static void normalize_thinking(json_t *t) {
    if (!json_is_obj(t)) return;
    json_t *levels = json_take(t, "levels");
    json_t *minl = json_take(t, "minLevel"), *maxl = json_take(t, "maxLevel");
    if (!json_get_arr(t, "efforts")) {
        json_t *eff = NULL;
        if (json_is_arr(levels)) {
            eff = levels;
            levels = NULL;
        } else if (effort_idx(json_sv(minl)) >= 0 && effort_idx(json_sv(maxl)) >= 0) {
            int lo = effort_idx(json_sv(minl)), hi = MAX(lo, effort_idx(json_sv(maxl)));
            eff = json_arr();
            for (int i = lo; i <= hi; i++) json_push_str(eff, EFFORT_ORDER[i]);
        }
        if (eff) { /* keep omp's key order: mode, efforts, then the rest */
            json_t *rest = json_clone(t);
            json_t *mode = json_take(rest, "mode");
            while (json_len(t)) json_free(json_take(t, t->u.obj.keys[0]));
            if (mode) json_set(t, "mode", mode);
            json_set(t, "efforts", eff);
            const char *k;
            json_t *v;
            json_obj_foreach(rest, i, k, v) json_set(t, k, json_clone(v));
            json_free(rest);
        }
    }
    json_free(levels);
    json_free(minl);
    json_free(maxl);
}

static json_t *fallback_efforts(const char *api, const json_t *compat) {
    int n = 4;
    if (str_eq(api, "anthropic-messages")) n = 5;
    else if (str_eq(api, "bedrock-converse-stream")) n = 4;
    else if ((str_eq(api, "openai-completions") || str_eq(api, "openrouter")) && json_get(compat, "thinkingFormat"))
        n = str_eq(json_get_str(compat, "thinkingFormat"), "openai") && json_get_bool(compat, "supportsReasoningEffort", false) ? 5 : 4;
    else if (str_eq(api, "openai-responses") || str_eq(api, "openai-codex-responses") || str_eq(api, "azure-openai-responses"))
        n = 5;
    json_t *a = json_arr();
    for (int i = 0; i < n; i++) json_push_str(a, EFFORT_ORDER[i]);
    return a;
}

static const char *default_thinking_mode(const char *api, const model_identity_t *idn) {
    if (str_eq(api, "google-generative-ai") || str_eq(api, "google-gemini-cli") || str_eq(api, "google-vertex")) return "budget";
    if (str_eq(api, "anthropic-messages")) {
        if (is_cls(idn, "anthropic")) {
            if (is_fam(idn, "haiku") ? rev_gte(idn, 5, 5) : rev_gte(idn, 4, 6)) return "anthropic-adaptive";
            if (is_fam(idn, "opus") && rev_gte(idn, 4, 5)) return "anthropic-budget-effort";
        }
        return "budget";
    }
    if (str_eq(api, "bedrock-converse-stream")) {
        if (is_cls(idn, "anthropic")) {
            if (anthropic_adaptive_gen(idn, 4, 6)) return "anthropic-adaptive";
            if (is_fam(idn, "opus") && rev_gte(idn, 4, 5)) return "anthropic-budget-effort";
        }
        return is_cls(idn, "openai") ? "effort" : "budget";
    }
    return "effort";
}

static bool default_supports_display(const char *api, const model_identity_t *idn) {
    return (str_eq(api, "anthropic-messages") || str_eq(api, "bedrock-converse-stream")) && anthropic_adaptive_gen(idn, 4, 7);
}

/* mergeEffortMap: detected Fireworks remap, rule map, configured map; only the ladder's efforts. Owned or NULL. */
static json_t *merge_effort_map(const char *api, const char *provider, const char *url, const json_t *rule_map, const json_t *compat,
                                const json_t *efforts) {
    bool fw = (str_eq(api, "openai-completions") || str_eq(api, "openrouter")) && model_on(provider, url ? url : "", "fireworks");
    const json_t *cfg = json_get_obj(compat, "reasoningEffortMap");
    if (cfg && !json_len(cfg)) cfg = NULL;
    if (!fw && !rule_map && !cfg) return NULL;
    json_t *merged = json_obj();
    if (fw) json_set_str(merged, "minimal", "none");
    const char *k;
    json_t *v;
    json_obj_foreach(rule_map, i, k, v) json_set(merged, k, json_clone(v));
    json_obj_foreach(cfg, j, k, v) json_set(merged, k, json_clone(v));
    json_t *out = NULL, *e;
    json_arr_foreach(efforts, m, e) {
        json_t *mapped = json_get(merged, json_sv(e));
        if (!mapped) continue;
        if (!out) out = json_obj();
        json_set(out, json_sv(e), json_clone(mapped));
    }
    json_free(merged);
    return out;
}

/* Owned thinking config, or NULL for none */
static json_t *resolve_thinking(const char *api, const char *provider, const char *url, const char *id, const model_identity_t *idn,
                                const axes_t *ax, const json_t *compat, const json_t *spec_thinking, bool spec_reasoning) {
    const json_t *rule = ax->thinking;
    const json_t *explicit_t = json_len(json_get_arr(spec_thinking, "efforts")) > 0 ? spec_thinking : NULL;
    bool rule_unverified = json_get_bool(rule, "unverified", false);
    bool unverified = !compat_reasoning(spec_reasoning, ax);
    if (unverified && (explicit_t || !is_cls(idn, "unknown") || !rule_unverified)) return NULL;
    bool responses = str_eq(api, "openai-responses") || str_eq(api, "openai-codex-responses") || str_eq(api, "azure-openai-responses");
    if (responses && json_is_bool(json_get(compat, "supportsReasoningEffort")) && !json_get_bool(compat, "supportsReasoningEffort", true))
        return NULL;
    const json_t *rule_map = json_get_obj(rule, "effortMap");
    const json_t *rule_req = json_get(rule, "requiresEffort");
    bool mandatory = idn->thinking_variant || has_thinking_pair_token(id) || json_get_bool(compat, "qwenTemplateReasoningEffort", false);
    bool requires = json_is_bool(rule_req) ? rule_req->u.b : mandatory;
    const json_t *rule_display = json_get(rule, "supportsDisplay");
    bool display = json_is_bool(rule_display) ? rule_display->u.b : default_supports_display(api, idn);
    if (explicit_t) { /* fillExplicitThinking */
        json_t *t = json_clone(explicit_t);
        if (!json_get(t, "effortMap")) {
            json_t *m = merge_effort_map(api, provider, url, rule_map, compat, json_get_arr(t, "efforts"));
            if (m) json_set(t, "effortMap", m);
        }
        if (!json_get(t, "supportsDisplay") && display) json_set_bool(t, "supportsDisplay", true);
        if (!json_get(t, "defaultLevel") && json_get_str(rule, "defaultLevel")) json_set_str(t, "defaultLevel", json_get_str(rule, "defaultLevel"));
        if (!json_get(t, "requiresEffort") && requires) json_set_bool(t, "requiresEffort", true);
        if (!json_get(t, "prefixBinding") && json_get_bool(rule, "prefixBinding", false)) json_set_bool(t, "prefixBinding", true);
        if (!json_get(t, "effortBudgets") && json_get_obj(rule, "effortBudgets")) json_set(t, "effortBudgets", json_clone(json_get(rule, "effortBudgets")));
        return t;
    }
    if (json_get_bool(compat, "trustExplicitThinkingOnly", false)) return NULL;
    const json_t *rule_efforts = json_get_arr(rule, "efforts");
    if (rule_efforts && !json_len(rule_efforts)) return NULL;
    json_t *cfg = json_obj();
    const char *mode = json_get_str(rule, "mode");
    json_set_str(cfg, "mode", mode ? mode : default_thinking_mode(api, idn));
    json_t *efforts = rule_efforts ? json_clone(rule_efforts) : fallback_efforts(api, compat);
    json_set(cfg, "efforts", efforts);
    json_t *m = merge_effort_map(api, provider, url, rule_map, compat, efforts);
    if (m) json_set(cfg, "effortMap", m);
    if (unverified) {
        json_set_bool(cfg, "unverified", true);
        return cfg;
    }
    if (json_get_str(rule, "defaultLevel")) json_set_str(cfg, "defaultLevel", json_get_str(rule, "defaultLevel"));
    if (json_get_obj(rule, "effortBudgets")) json_set(cfg, "effortBudgets", json_clone(json_get(rule, "effortBudgets")));
    if (display) json_set_bool(cfg, "supportsDisplay", true);
    if (json_get_bool(rule, "prefixBinding", false)) json_set_bool(cfg, "prefixBinding", true);
    if (requires) json_set_bool(cfg, "requiresEffort", true);
    if (json_get_bool(rule, "suppressWhenOff", false)) json_set_bool(cfg, "suppressWhenOff", true);
    return cfg;
}

/* ---------- catalog axes (build.ts) ---------- */

static bool utc_instant(const char *s, time_t *out) {
    struct tm tm = {0};
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 3) return false;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    *out = timegm(&tm);
    return true;
}

static const char *const COST_LEGS[] = {"input", "output", "cacheRead", "cacheWrite"};

/* applyDueDatedCostCard: the latest `effectiveRates` card already in force */
static void apply_due_card(json_t *cost, const json_t *axis) {
    const json_t *rates = json_get_obj(axis, "effectiveRates");
    const json_t *due = NULL;
    time_t due_at = 0, now = time(NULL);
    const char *k;
    json_t *v;
    json_obj_foreach(rates, i, k, v) {
        time_t at;
        if (utc_instant(json_get_str(v, "effectiveFrom"), &at) && at <= now && (!due || at > due_at)) due = v, due_at = at;
    }
    if (!due) return;
    for (size_t l = 0; l < ARRAY_LEN(COST_LEGS); l++)
        if (json_is_num(json_get(due, COST_LEGS[l]))) json_set_num(cost, COST_LEGS[l], json_get_num(due, COST_LEGS[l], 0));
}

static void apply_legs(json_t *cost, const json_t *axis) {
    for (size_t l = 0; l < ARRAY_LEN(COST_LEGS); l++)
        if (json_is_num(json_get(axis, COST_LEGS[l]))) json_set_num(cost, COST_LEGS[l], json_get_num(axis, COST_LEGS[l], 0));
}

static bool has_token_price(const json_t *cost) {
    for (size_t l = 0; l < ARRAY_LEN(COST_LEGS); l++)
        if (json_get_num(cost, COST_LEGS[l], 0) != 0) return true;
    return false;
}

/* correctedCost at now (a recurring `time-based-cost` schedule is not carried) */
static void corrected_cost(json_t *cost, const json_t *cat) {
    const json_t *patch = json_get_obj(cat, "costPatch"), *fallback = json_get_obj(cat, "costFallback");
    if (patch) {
        apply_legs(cost, patch);
        apply_due_card(cost, patch);
    }
    if (fallback && !has_token_price(cost)) {
        apply_legs(cost, fallback);
        apply_due_card(cost, fallback);
    }
    const json_t *lc = json_get_obj(cat, "longContext");
    if (lc && json_is_num(json_get(lc, "inputThreshold"))) {
        json_t *tier = NULL;
        if (json_is_num(json_get(lc, "multiplier")) && has_token_price(cost)) {
            double mul = json_get_num(lc, "multiplier", 1), omul = json_get_num(lc, "outputMultiplier", mul);
            tier = json_obj();
            json_set(tier, "inputThreshold", json_clone(json_get(lc, "inputThreshold")));
            if (json_get_bool(lc, "inputThresholdInclusive", false)) json_set_bool(tier, "inputThresholdInclusive", true);
            json_set_num(tier, "input", json_get_num(cost, "input", 0) * mul);
            json_set_num(tier, "output", json_get_num(cost, "output", 0) * omul);
            json_set_num(tier, "cacheRead", json_get_num(cost, "cacheRead", 0) * mul);
            json_set_num(tier, "cacheWrite", json_get_num(cost, "cacheWrite", 0) * mul);
        } else if (json_is_num(json_get(lc, "input")) && json_is_num(json_get(lc, "output")) && json_is_num(json_get(lc, "cacheRead")) &&
                   json_is_num(json_get(lc, "cacheWrite"))) {
            tier = json_obj();
            json_set(tier, "inputThreshold", json_clone(json_get(lc, "inputThreshold")));
            apply_legs(tier, lc);
        }
        if (tier) json_set(cost, "longContext", tier);
    }
    json_t *tier = json_get_obj(cost, "longContext");
    if (json_get_bool(cat, "cacheReadAtInputRate", false)) {
        json_set_num(cost, "cacheRead", json_get_num(cost, "input", 0));
        if (tier) json_set_num(tier, "cacheRead", json_get_num(tier, "input", 0));
    }
    if (json_is_num(json_get(cat, "cacheWriteRate"))) {
        double rate = json_get_num(cat, "cacheWriteRate", 0);
        json_set_num(cost, "cacheWrite", json_get_num(cost, "input", 0) * rate);
        if (tier) json_set_num(tier, "cacheWrite", json_get_num(tier, "input", 0) * rate);
    }
}

static void apply_catalog(json_t *m, const json_t *cat) {
    const char *s;
    if ((s = json_get_str(cat, "kind"))) json_set_str(m, "kind", s);
    static const char *const flags[] = {"contextWindowAuthoritative", "longContextWindowUncapped", "requiresCursorToolSchemaProjection",
                                        "requiresToolResultImageHoisting", "supportsAssistantPrefill"};
    for (size_t i = 0; i < ARRAY_LEN(flags); i++) {
        if (json_get_bool(cat, flags[i], false)) json_set_bool(m, flags[i], true);
        else json_del(m, flags[i]);
    }
    if ((s = json_get_str(cat, "webSearch"))) json_set_str(m, "webSearch", s);
    if ((s = json_get_str(cat, "webSearchModel"))) json_set_str(m, "webSearchModel", s);
    json_t *v = json_get(cat, "hostedImage");
    if (json_is_bool(v)) {
        if (v->u.b) json_set_bool(m, "hostedImage", true);
        else json_del(m, "hostedImage");
    }
    v = json_get(cat, "imageModel");
    if (json_is_str(v)) json_set_str(m, "imageModel", json_sv(v));
    else if (json_is_bool(v) && !v->u.b) json_del(m, "imageModel");
    static const char *const copies[] = {"serviceTierCost", "priority", "promptCache", "applyPatchToolType", "editPromptVariant", "pricingStatus"};
    for (size_t i = 0; i < ARRAY_LEN(copies); i++)
        if ((v = json_get(cat, copies[i]))) json_set(m, copies[i], json_clone(v));
    v = json_get(cat, "toolSchemasInContext");
    if (json_is_bool(v) && !v->u.b) json_set_bool(m, "toolSchemasInContext", false);
    else json_del(m, "toolSchemasInContext");
    if (json_is_bool(json_get(cat, "omitMaxOutputTokens")) && !json_get(m, "omitMaxOutputTokens"))
        json_set_bool(m, "omitMaxOutputTokens", json_get_bool(cat, "omitMaxOutputTokens", false));
    if ((s = json_get_str(cat, "contextPromotionTarget")) && !json_get(m, "contextPromotionTarget"))
        json_set_str(m, "contextPromotionTarget", s);

    /* applyCatalogCorrections */
    json_t *cost = json_get_obj(m, "cost");
    if (cost) corrected_cost(cost, cat);
    const json_t *limits = json_get_obj(cat, "limitsPatch");
    if (limits) {
        if (json_is_num(json_get(limits, "contextWindow"))) json_set(m, "contextWindow", json_clone(json_get(limits, "contextWindow")));
        if (json_is_num(json_get(limits, "maxTokens"))) json_set(m, "maxTokens", json_clone(json_get(limits, "maxTokens")));
    }
    if (json_is_num(json_get(cat, "maxTokensFallback")) && !json_get(m, "maxTokens"))
        json_set(m, "maxTokens", json_clone(json_get(cat, "maxTokensFallback")));
    if (json_is_num(json_get(cat, "maxTokensCeiling")) && json_is_num(json_get(m, "maxTokens")) &&
        json_get_int(m, "maxTokens", 0) > json_get_int(cat, "maxTokensCeiling", 0))
        json_set(m, "maxTokens", json_clone(json_get(cat, "maxTokensCeiling")));
    if (json_is_num(json_get(cat, "contextWindowFloor"))) {
        int64_t floor_ = json_get_int(cat, "contextWindowFloor", 0);
        json_set_int(m, "contextWindow", MAX(json_get_int(m, "contextWindow", 0), floor_));
    }
    v = json_get(cat, "inputModalities");
    if (json_is_arr(v)) json_set(m, "input", json_clone(v));
}

/* model-tokenizer.ts resolveModelTokenizer */
static const char *resolve_tokenizer(const char *provider, const char *id) {
    model_identity_t i;
    model_identity_classify(provider, bare_of(id), &i);
    if (is_cls(&i, "anthropic")) {
        if (is_fam(&i, "opus")) return rev_gte(&i, 5, 0) ? "claude-v5" : rev_gte(&i, 4, 7) ? "claude-v47" : "claude-v3";
        if (is_fam(&i, "sonnet") || is_fam(&i, "fable") || is_fam(&i, "mythos")) return rev_gte(&i, 5, 0) ? "claude-v5-sonnet" : "claude-v3";
        if (is_fam(&i, "haiku") && rev_gte(&i, 5, 5)) return "claude-v5-sonnet";
        return "claude-v3";
    }
    if (is_cls(&i, "qwen") && rev_gte(&i, 3, 5)) return "qwen3";
    if (is_cls(&i, "deepseek")) return "deepseek-v3";
    if (is_cls(&i, "kimi")) return "kimi-k2";
    if (is_cls(&i, "glm") && rev_gte(&i, 5, 0)) return "glm5";
    /* GPT-5 and later bill text in o200k_base, GPT-3.5 and GPT-4 (Turbo) in cl100k_base. GPT-4.1, GPT-4.5, gpt-4o and the
     * o-series are o200k too, which is the default for a model without a tokenizer */
    if (is_cls(&i, "openai")) {
        if (rev_gte(&i, 5, 0)) return "o200k";
        if (is_fam(&i, "gpt") && rev_gte(&i, 3, 5) && !rev_gte(&i, 4, 1)) return "cl100k";
    }
    return NULL;
}

/* utils.ts cleanModelName: gateway author prefix and model-extrinsic decorations. Owned. */
static char *clean_model_name(const char *name) {
    const char *b = name;
    if (isalpha((unsigned char)name[0])) { /* /^[A-Za-z][A-Za-z0-9 .+&'-]{0,23}: / */
        size_t i = 1;
        while (i <= 23 && name[i] && (isalnum((unsigned char)name[i]) || strchr(" .+&'-", name[i]))) i++;
        if (name[i] == ':' && name[i + 1] == ' ') b = name + i + 2;
    }
    sb_t o = SB_INIT;
    for (const char *p = b; *p;) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        bool tag = false;
        if (*q == '(') {
            const char *e = strchr(q, ')');
            if (e) {
                size_t n = (size_t)(e - q - 1);
                const char *t = q + 1;
                if ((n == 6 && !strncmp(t, "latest", 6)) || (n == 11 && !strncmp(t, "Antigravity", 11)) || !strncmp(t, "retires ", 8)) tag = true;
                else if (n && strspn(t, "$") == n) tag = true;
                else {
                    const char *d = t;
                    if (*d == '>') d++;
                    const char *ds = d;
                    while (isdigit((unsigned char)*d)) d++;
                    if (d > ds && !strncmp(d, "% off)", 6)) tag = true;
                }
                if (tag) {
                    p = e + 1;
                    continue;
                }
            }
        }
        sb_putc(&o, *p++);
    }
    /* collapse runs of spaces, trim */
    sb_t c = SB_INIT;
    const char *src = sb_cstr(&o);
    for (const char *p = src; *p; p++)
        if (!(*p == ' ' && p[1] == ' ')) sb_putc(&c, *p);
    char *t = str_trim(sb_cstr(&c));
    sb_free(&o);
    sb_free(&c);
    if (!*t) {
        free(t);
        return xstrdup(name);
    }
    return t;
}

/* ---------- buildModel ---------- */

static void build_model(json_t *model) {
    const char *id = json_get_str(model, "id"), *provider = json_get_str(model, "provider"), *api = json_get_str(model, "api");
    if (!id || !provider) return;
    json_t *t = json_get_obj(model, "thinking");
    if (t) normalize_thinking(t);
    if (json_get_bool(t, "unverified", false)) { /* withoutUnverifiedThinking: derived output, not authored input */
        json_del(model, "thinking");
        t = NULL;
        json_set_bool(model, "reasoning", false);
    }
    const char *url = json_get_str(model, "baseUrl");
    const char *backend = json_get_str(model, "providerType");
    if (!backend) backend = provider;
    bool reasoning = json_get_bool(model, "reasoning", false);
    model_identity_t idn;
    model_identity_classify(provider, id, &idn);
    axes_t ax = {0};
    resolve_cascade(&ax, backend, api ? api : "", &idn, id, reasoning);

    json_t *user = json_get_obj(model, "compat") ? json_clone(json_get(model, "compat")) : NULL;
    json_t *compat = NULL;
    if (str_eq(api, "openrouter")) {
        compat = chat_compat(provider, backend, url, &idn, reasoning, user, &ax);
        json_t *resp = responses_compat(provider, backend, url, &idn, reasoning, user, &ax, api);
        static const char *const only[] = {"supportsLongPromptCacheRetention", "strictResponsesPairing", "supportsImageDetailOriginal",
                                           "supportsObfuscationOptOut", "storeResponses", "connectionBoundNativeHistory",
                                           "supportsAllTurnsReasoningContext", "supportsConfigurationUpdate", "supportsSteering",
                                           "officialEndpoint", "harmonyLeakMitigation", "cacheControlFormat",
                                           "requiresReasoningOffJuiceInstruction", "supportsReasoningSummary", "statefulResponses",
                                           "isVercelGatewayHost"};
        for (size_t i = 0; i < ARRAY_LEN(only); i++) {
            json_t *v = json_get(resp, only[i]);
            if (v) json_set(compat, only[i], json_clone(v));
            else json_del(compat, only[i]);
        }
        json_free(resp);
    } else if (str_eq(api, "openai-completions")) {
        compat = chat_compat(provider, backend, url, &idn, reasoning, user, &ax);
    } else if (str_eq(api, "openai-responses") || str_eq(api, "azure-openai-responses") || str_eq(api, "openai-codex-responses")) {
        compat = responses_compat(provider, backend, url, &idn, reasoning, user, &ax, api);
    } else if (str_eq(api, "anthropic-messages")) {
        compat = anthropic_compat(provider, url, &idn, reasoning, user, &ax);
    } else if (str_eq(api, "bedrock-converse-stream")) {
        compat = bedrock_compat(reasoning, user, &ax);
    } else if (str_eq(api, "ollama-chat")) {
        compat = ollama_compat(user, &ax);
    } else if (str_eq(api, "devin-agent")) {
        compat = devin_compat(user, &ax);
    } else if (str_eq(api, "google-generative-ai") || str_eq(api, "google-vertex") || str_eq(api, "google-gemini-cli")) {
        compat = google_compat(api, user, &ax);
    }

    json_t *thinking = resolve_thinking(api ? api : "", provider, url, id, &idn, &ax, compat, json_get_obj(model, "thinking"), reasoning);
    if (thinking) json_set(model, "thinking", thinking);
    else json_del(model, "thinking");
    json_set_bool(model, "reasoning", reasoning || thinking != NULL);

    const char *name = json_get_str(model, "name");
    if (name) json_set(model, "name", json_str_own(clean_model_name(name)));

    if (!json_get_obj(model, "identity")) {
        json_t *o = json_obj();
        json_set_str(o, "class", idn.cls);
        if (idn.family) json_set_str(o, "family", idn.family);
        if (idn.rev_str[0]) json_set_str(o, "revision", idn.rev_str);
        if (idn.effort) json_set_str(o, "effort", idn.effort);
        if (idn.thinking_variant) json_set_bool(o, "thinkingVariant", true);
        if (idn.logical[0]) json_set_str(o, "logicalId", idn.logical);
        json_set(model, "identity", o);
    }
    json_set_bool(model, "requiresGlyphTokenization", is_cls(&idn, "anthropic"));
    if (!json_get_str(model, "tokenizer")) {
        const char *tk = resolve_tokenizer(provider, id);
        if (tk) json_set_str(model, "tokenizer", tk);
    }
    if (compat) {
        /* omp's models do not report this axis and omc's loop guard (coding/thinkloop.c) keys on the model itself */
        json_del(compat, "thinkingLoopGuard");
        json_set(model, "compat", compat);
    }
    if (user) json_set(model, "compatConfig", user);
    apply_catalog(model, ax.catalog);
    axes_free(&ax);
}

void compat_resolve_model(json_t *model) {
    if (json_is_obj(model)) build_model(model);
}

/* mergeCompat: objects merge recursively, otherwise the override wins. Owned. */
json_t *compat_merge(const json_t *base, const json_t *over) {
    if (!base) return over ? json_clone(over) : NULL;
    if (!over) return json_clone(base);
    json_t *m = json_clone(base);
    const char *k;
    json_t *v;
    json_obj_foreach(over, i, k, v) {
        json_t *b = json_get(base, k);
        json_set(m, k, json_is_obj(b) && json_is_obj(v) ? compat_merge(b, v) : json_clone(v));
    }
    return m;
}

/* types.ts runnerApiKind */
static const char *runner_api_kind(const char *api) {
    static const char *const map[][2] = {{"web-search", "search"}, {"typesafe", "judge"}, {"openrouter-decisions", "judge"},
                                         {"openai-decisions", "judge"}, {"workers-ai-systemone", "judge"}, {"openai-images", "image"},
                                         {"openrouter-images", "image"}, {"xai-tts", "tts"}, {"openai-speech", "tts"},
                                         {"openai-embeddings", "embedding"}, {"openrouter-rerank", "rerank"},
                                         {"openrouter-video", "video"}, {"openai-transcriptions", "stt"}};
    for (size_t i = 0; i < ARRAY_LEN(map); i++)
        if (str_eq(api, map[i][0])) return map[i][1];
    return NULL;
}

void compat_finalize_custom(json_t *o, const json_t *reference) {
    if (!json_is_obj(o)) return;
    if (!json_get_str(o, "kind") && runner_api_kind(json_get_str(o, "api"))) json_set_str(o, "kind", runner_api_kind(json_get_str(o, "api")));
    const char *id = json_get_str(o, "id"), *provider = json_get_str(o, "provider");
    json_t *ex_cost = json_get_obj(o, "cost") ? json_clone(json_get(o, "cost")) : NULL;
    json_t *ex_ctx = json_is_num(json_get(o, "contextWindow")) ? json_clone(json_get(o, "contextWindow")) : NULL;
    json_t *ex_max = json_is_num(json_get(o, "maxTokens")) ? json_clone(json_get(o, "maxTokens")) : NULL;
    json_t *ex_kind = json_get_str(o, "kind") ? json_clone(json_get(o, "kind")) : NULL;
    json_t *ex_cache = json_get_obj(o, "promptCache") ? json_clone(json_get(o, "promptCache")) : NULL;
    if (!json_get_str(o, "name") && id) json_set_str(o, "name", id);
    if (!json_get(o, "cost")) {
        const json_t *rc = json_get_obj(reference, "cost");
        if (rc) {
            json_t *c = json_clone(rc);
            json_del(c, "timeBased");
            json_set(o, "cost", c);
        } else {
            json_t *c = json_obj();
            for (size_t l = 0; l < ARRAY_LEN(COST_LEGS); l++) json_set_int(c, COST_LEGS[l], 0);
            json_set(o, "cost", c);
        }
    }
    if (!json_get(o, "input")) {
        const json_t *ri = json_get_arr(reference, "input");
        if (ri) json_set(o, "input", json_clone(ri));
        else {
            json_t *in = json_arr();
            json_push_str(in, "text");
            json_set(o, "input", in);
        }
    }
    if (!json_get(o, "supportsTools") && json_is_bool(json_get(reference, "supportsTools")))
        json_set(o, "supportsTools", json_clone(json_get(reference, "supportsTools")));
    if (!json_is_bool(json_get(o, "reasoning")))
        json_set_bool(o, "reasoning", json_get_bool(reference, "reasoning", false));
    /* inheritReferenceThinking: wire routing is provider-specific, so only a same-provider reference */
    if (!json_get_obj(o, "thinking") && json_get_obj(reference, "thinking") && str_eq(json_get_str(reference, "provider"), provider))
        json_set(o, "thinking", json_clone(json_get(reference, "thinking")));
    if (!ex_ctx) json_set_int(o, "contextWindow", json_get_int(reference, "contextWindow", 128000));
    if (!ex_max) json_set_int(o, "maxTokens", json_get_int(reference, "maxTokens", 16384));
    if (!json_get(o, "omitMaxOutputTokens") && json_is_bool(json_get(reference, "omitMaxOutputTokens")))
        json_set(o, "omitMaxOutputTokens", json_clone(json_get(reference, "omitMaxOutputTokens")));
    const json_t *rc = json_get_obj(reference, "compatConfig");
    if (rc) {
        json_t *m = compat_merge(rc, json_get_obj(o, "compat"));
        json_set(o, "compat", m);
    }
    build_model(o);
    /* explicit custom prices and limits outrank catalog corrections; configured kinds and lifetimes replace them */
    if (ex_cost) json_set(o, "cost", ex_cost);
    if (ex_ctx) json_set(o, "contextWindow", ex_ctx);
    if (ex_max) json_set(o, "maxTokens", ex_max);
    if (ex_kind) json_set(o, "kind", ex_kind);
    if (ex_cache) json_set(o, "promptCache", ex_cache);
}

/* ---------- proxy reference lookup (identity/reference.ts, identity/id.ts) ---------- */

typedef struct {
    const json_t *m;
    char *alias_key;
} ref_slot_t;

struct compat_ref_index {
    hmap_t exact, alias;
    VEC(ref_slot_t *) order;
};

/* MODEL_FAMILY_PREFIX_PATTERN plus "has a digit" on a lower-cased segment */
static bool model_like(const char *s) {
    static const char *const fam[] = {"claude", "gemini", "gpt", "grok", "glm", "qwen", "deepseek", "kimi", "mimo", "doubao", "ernie",
                                      "gpt-oss", "gemma", "minimax", "step", "command", "jamba", "llama", "o1", "o3", "o4", "o5"};
    if (!strpbrk(s, "0123456789")) return false;
    for (size_t i = 0; i < ARRAY_LEN(fam); i++)
        if (str_starts(s, fam[i])) return true;
    return false;
}

static int seg_cmp(const void *a, const void *b) {
    const char *l = *(const char *const *)a, *r = *(const char *const *)b;
    size_t ll = strlen(l), rl = strlen(r);
    if (ll != rl) return ll > rl ? -1 : 1;
    return strcmp(l, r);
}

/* normalizeModelIdWhitespace + the /[a-z0-9.:-]+/g runs of the lower-cased id that look like a model id, best first */
static void model_like_segments(const char *id, strvec_t *out) {
    char *t = lower_trim(id);
    for (const char *p = t; *p;) {
        size_t n = strspn(p, "abcdefghijklmnopqrstuvwxyz0123456789.:-");
        if (n) {
            char *seg = xstrndup(p, n);
            if (model_like(seg) && !strvec_contains(out, seg)) strvec_push_own(out, seg);
            else free(seg);
            p += n;
        } else {
            p++;
        }
    }
    free(t);
    if (out->n > 1) qsort(out->v, out->n, sizeof(char *), seg_cmp);
}

static bool ref_replaces(const json_t *existing, const json_t *c) {
    if (!existing) return true;
    int64_t cw = json_get_int(c, "contextWindow", 0), ew = json_get_int(existing, "contextWindow", 0);
    if (cw != ew) return cw > ew;
    int64_t cm = json_get_int(c, "maxTokens", 0), em = json_get_int(existing, "maxTokens", 0);
    if (cm != em) return cm > em;
    const json_t *cc = json_get_obj(c, "cost"), *ec = json_get_obj(existing, "cost");
    bool ep = json_get_num(ec, "cacheRead", 0) > 0 || json_get_num(ec, "cacheWrite", 0) > 0;
    bool cp = json_get_num(cc, "cacheRead", 0) > 0 || json_get_num(cc, "cacheWrite", 0) > 0;
    if (cp != ep) return cp;
    return !str_eq(json_get_str(existing, "provider"), "openai") && str_eq(json_get_str(c, "provider"), "openai");
}

compat_ref_index_t *compat_ref_index_new(void) {
    compat_ref_index_t *x = xcalloc(1, sizeof(*x));
    hmap_init(&x->exact);
    hmap_init(&x->alias);
    return x;
}

void compat_ref_index_add(compat_ref_index_t *x, const json_t *m) {
    const char *provider = json_get_str(m, "provider"), *id = json_get_str(m, "id");
    if (!provider || !id || arr_has(json_get_arr(rules(), "referenceIsolatedProviders"), provider)) return;
    if (str_eq(provider, "xai-oauth") && !has_token_price(json_get_obj(m, "cost"))) return;
    char *key = lower_trim(id);
    ref_slot_t *slot = hmap_get(&x->exact, key);
    if (!slot) {
        slot = xcalloc(1, sizeof(*slot));
        slot->m = m;
        hmap_put(&x->exact, key, slot);
        vec_push(&x->order, slot);
    } else if (ref_replaces(slot->m, m)) {
        slot->m = m;
    }
    free(key);
}

void compat_ref_index_finish(compat_ref_index_t *x) {
    for (size_t i = 0; i < x->order.n; i++) { /* buildSuffixAliasMap, in insertion order */
        const json_t *m = x->order.v[i]->m;
        const char *id = json_get_str(m, "id");
        const char *slash = strrchr(id, '/');
        if (!slash) continue;
        strvec_t segs = {0};
        model_like_segments(slash + 1, &segs);
        if (segs.n) {
            const json_t *held = hmap_get(&x->alias, segs.v[0]);
            if (ref_replaces(held, m)) hmap_put(&x->alias, segs.v[0], (void *)m);
        }
        strvec_free(&segs);
    }
}

/* one leading or trailing run of `[tag]` / `【tag】` affixes removed from a whitespace-normalized id. Owned. */
static const char *skip_affix_open(const char *p) {
    if (*p == '[') return p + 1;
    if (!strncmp(p, "\xe3\x80\x90", 3)) return p + 3;
    return NULL;
}

static const char *skip_leading_affixes(const char *s) {
    const char *p = s;
    for (;;) {
        const char *q = p;
        while (isspace((unsigned char)*q)) q++;
        const char *in = skip_affix_open(q);
        if (!in) return p;
        const char *e = in;
        size_t body = 0;
        while (*e && *e != ']' && strncmp(e, "\xe3\x80\x91", 3)) e++, body++;
        if (!*e || !body) return p;
        e += *e == ']' ? 1 : 3;
        while (isspace((unsigned char)*e)) e++;
        p = e;
    }
}

static size_t trailing_affix_start(const char *s) {
    /* the shortest leading part such that the rest is one or more complete `[tag]` affixes */
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        const char *rest = s + i;
        const char *q = rest;
        while (isspace((unsigned char)*q)) q++;
        if (!skip_affix_open(q)) continue;
        if (!*skip_leading_affixes(rest)) return i;
    }
    return n;
}

static char *norm_ws(const char *s) {
    char *t = str_trim(s);
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = t; *p; p++) {
        if (isspace((unsigned char)*p)) {
            sp = true;
        } else {
            if (sp) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *p);
        }
    }
    free(t);
    return sb_detach(&b);
}

static void bracket_candidates(const char *id, strvec_t *out) {
    if (!strpbrk(id, "[]") && !strstr(id, "\xe3\x80\x90") && !strstr(id, "\xe3\x80\x91")) return;
    char *normalized = norm_ws(id);
    if (!*normalized) {
        free(normalized);
        return;
    }
    const char *lead = skip_leading_affixes(normalized);
    char *without_leading = norm_ws(lead);
    char *t1 = xstrndup(normalized, trailing_affix_start(normalized));
    char *without_trailing = norm_ws(t1);
    char *t2 = xstrndup(lead, trailing_affix_start(lead));
    char *without_both = norm_ws(t2);
    const char *cands[] = {without_both, without_leading, without_trailing};
    for (size_t i = 0; i < 3; i++)
        if (*cands[i] && strcmp(cands[i], normalized) && !strvec_contains(out, cands[i])) strvec_push(out, cands[i]);
    free(without_leading);
    free(t1);
    free(without_trailing);
    free(t2);
    free(without_both);
    free(normalized);
}

/* stripReferenceTrailingMarker. Owned or NULL. */
static char *strip_trailing_marker(const char *candidate) {
    char *lower = str_lower(candidate);
    size_t best = 0;
    const json_t *d = json_get_obj(rules(), "discovery"), *c = json_get_obj(rules(), "collapse");
    json_t *it;
    json_arr_foreach(json_get_arr(d, "billingVariantSuffixes"), i, it) {
        const char *s = json_sv(it);
        if (s && str_ends(lower, s) && strlen(s) > best) best = strlen(s);
    }
    static const char *const lists[] = {"trailingMarkers", "referenceOnlyTrailingMarkers"};
    for (size_t l = 0; l < 2; l++) {
        json_arr_foreach(json_get_arr(d, lists[l]), j, it) {
            const char *s = json_sv(it);
            if (!s) continue;
            char *a = xasprintf("-%s", s), *b = xasprintf(":%s", s);
            if ((str_ends(lower, a) || str_ends(lower, b)) && strlen(a) > best) best = strlen(a);
            free(a);
            free(b);
        }
    }
    json_arr_foreach(json_get_arr(c, "suffixes"), k, it) {
        const char *s = json_get_str(it, "suffix");
        if (s && str_ends(lower, s) && strlen(s) > best) best = strlen(s);
    }
    json_arr_foreach(json_get_arr(c, "routingVariants"), m, it) {
        const char *s = json_get_str(it, "suffix");
        if (s && str_ends(lower, s) && strlen(s) > best) best = strlen(s);
    }
    size_t n = strlen(candidate);
    free(lower);
    return best > 0 && best < n ? xstrndup(candidate, n - best) : NULL;
}

/* getReferenceCandidateIds: distinct ids a proxied / affixed id may be catalogued under, least-stripped first */
static void reference_candidates(const char *model_id, strvec_t *out) {
    strvec_t queue = {0};
    strvec_push(&queue, model_id);
    for (size_t i = 0; i < queue.n && out->n < 256; i++) {
        char *cand = str_trim(queue.v[i]);
        if (!*cand || strvec_contains(out, cand)) {
            free(cand);
            continue;
        }
        strvec_push(out, cand);
        strvec_t br = {0};
        bracket_candidates(cand, &br);
        for (size_t k = 0; k < br.n; k++) strvec_push(&queue, br.v[k]);
        strvec_free(&br);
        strvec_t segs = {0};
        model_like_segments(cand, &segs);
        for (size_t k = 0; k < segs.n; k++) strvec_push(&queue, segs.v[k]);
        strvec_free(&segs);
        char *lower = str_lower(cand);
        if (str_ends(lower, ":cloud") || str_ends(lower, "-cloud")) strvec_push_own(&queue, xstrndup(cand, strlen(cand) - 6));
        const char *slash = strrchr(cand, '/');
        if (slash) strvec_push(&queue, slash + 1);
        if (strchr(cand, ':')) {
            char *dash = xstrdup(cand);
            for (char *p = dash; *p; p++)
                if (*p == ':') *p = '-';
            strvec_push_own(&queue, dash);
        }
        if (strcmp(lower, cand)) strvec_push(&queue, lower);
        free(lower);
        char *stripped = strip_trailing_marker(cand);
        if (stripped) strvec_push_own(&queue, stripped);
        free(cand);
    }
    strvec_free(&queue);
}

const json_t *compat_ref_lookup(const compat_ref_index_t *x, const char *model_id) {
    if (!x || !model_id || model_id[0] == '@') return NULL;
    strvec_t cands = {0};
    reference_candidates(model_id, &cands);
    const json_t *found = NULL;
    for (size_t i = 0; i < cands.n && !found; i++) {
        char *key = lower_trim(cands.v[i]);
        ref_slot_t *slot = hmap_get(&x->exact, key);
        found = slot ? slot->m : hmap_get(&x->alias, key);
        free(key);
    }
    strvec_free(&cands);
    return found;
}

/* ---------- grammar schema flavor (utils/schema/normalize.ts sanitizeSchemaForGrammar) ---------- */

static bool in_list(const char *k, const char *const *l) {
    for (; *l; l++)
        if (!strcmp(k, *l)) return true;
    return false;
}

static const char *const SUB_ARRAY[] = {"anyOf", "oneOf", "allOf", "prefixItems", NULL};
static const char *const SUB_MAP[] = {"properties", "patternProperties", "dependencies", "dependentSchemas", "$defs",
                                      "definitions", NULL};
static const char *const GRAMMAR_VALUE[] = {"items", "additionalItems", "contains", "contentSchema", "propertyNames", "if",
                                            "then", "else", "not", "unevaluatedItems", NULL};

static json_t *open_widening(void) {
    json_t *any = json_arr();
    static const char *const types[] = {"string", "number", "boolean", "object", "array", "null"};
    for (size_t i = 0; i < ARRAY_LEN(types); i++) {
        json_t *o = json_obj();
        json_set_str(o, "type", types[i]);
        json_push(any, o);
    }
    json_t *w = json_obj();
    json_set(w, "anyOf", any);
    return w;
}

static json_t *grammar_node(const json_t *v, bool sub, int depth) {
    if (depth > 64) return json_clone(v);
    if (json_is_bool(v) && sub) {
        if (v->u.b) return open_widening();
        json_t *n = json_obj();
        json_set(n, "not", open_widening());
        return n;
    }
    if (json_is_arr(v)) {
        json_t *a = json_arr(), *it;
        json_arr_foreach(v, i, it) json_push(a, grammar_node(it, sub, depth + 1));
        return a;
    }
    if (!json_is_obj(v)) return json_clone(v);
    json_t *o = json_obj();
    const char *k;
    json_t *c;
    json_obj_foreach(v, i, k, c) {
        json_t *n;
        if (in_list(k, SUB_MAP) && json_is_obj(c)) {
            n = json_obj();
            const char *ck;
            json_t *cv;
            json_obj_foreach(c, j, ck, cv) json_set(n, ck, grammar_node(cv, true, depth + 1));
        } else if (in_list(k, SUB_ARRAY) && json_is_arr(c)) {
            n = json_arr();
            json_t *it;
            json_arr_foreach(c, j, it) json_push(n, grammar_node(it, true, depth + 1));
        } else if (in_list(k, GRAMMAR_VALUE)) {
            n = grammar_node(c, true, depth + 1);
        } else if ((!strcmp(k, "additionalProperties") || !strcmp(k, "unevaluatedProperties")) && !json_is_bool(c)) {
            n = grammar_node(c, true, depth + 1);
        } else {
            n = json_clone(c);
        }
        json_set(o, k, n);
    }
    return o;
}

json_t *compat_schema_for_grammar(const json_t *schema) { return grammar_node(schema, true, 0); }
