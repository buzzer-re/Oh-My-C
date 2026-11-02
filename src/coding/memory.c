/* memory.c - memory backends and the local rollout-summary pipeline (see memory.h).
 * Ports omp memory-backend/{resolve,local-backend,off-backend,redact,messages}.ts, memories/index.ts and
 * the file-backed half of internal-urls/memory-protocol.ts. Storage is memstore.c. */
#include "memory.h"

#include "coding/app.h"
#include "coding/memstore.h"
#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"

#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- backend selection ---------- */

const char *memory_backend(void) {
    static const char *const IDS[] = {"off", "local", "hindsight", "mnemopi", "sharpshooter"};
    json_t *v = settings_get("memory.backend");
    if (json_is_str(v)) {
        for (size_t i = 0; i < ARRAY_LEN(IDS); i++)
            if (!strcmp(json_sv(v), IDS[i])) return IDS[i];
        return "off";
    }
    /* legacy `memories.enabled` migrates to memory.backend when the latter is unset (config/settings.ts) */
    json_t *legacy = settings_get("memories.enabled");
    if (json_is_bool(legacy)) return legacy->u.b ? "local" : "off";
    return "off";
}

static bool backend_is(const char *id) { return !strcmp(memory_backend(), id); }

/* ---------- paths ---------- */

static char *encode_project_path(const char *cwd) {
    const char *s = cwd;
    if (*s == '/' || *s == '\\') s++;
    char *t = xstrdup(s);
    for (char *p = t; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') *p = '-';
    char *r = xasprintf("--%s--", t);
    free(t);
    return r;
}

char *memory_root(const char *agent_dir, const char *cwd) {
    char *scope = memstore_normalize_cwd(cwd);
    char *enc = encode_project_path(scope);
    char *base = path_join(agent_dir, "memories");
    char *r = path_join(base, enc);
    free(base);
    free(enc);
    free(scope);
    return r;
}

char *memory_state_path(const char *agent_dir) { return path_join(agent_dir, "memory-state.json"); }

/* ---------- UTF-16 length helpers (JS string semantics for budgets) ---------- */

static size_t u16_len(const char *s, size_t n) {
    size_t u = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        u += cp >= 0x10000 ? 2 : 1;
        i += (size_t)k;
    }
    return u;
}

/* byte length of the longest code-point-aligned prefix holding at most `units` UTF-16 units */
static size_t u16_prefix(const char *s, size_t n, size_t units) {
    size_t u = 0, i = 0;
    while (i < n) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        size_t w = cp >= 0x10000 ? 2 : 1;
        if (u + w > units) break;
        u += w;
        i += (size_t)k;
    }
    return i;
}

/* byte offset where the longest code-point-aligned suffix holding at most `units` units starts */
static size_t u16_suffix_start(const char *s, size_t n, size_t units) {
    size_t total = u16_len(s, n);
    if (total <= units) return 0;
    size_t skip = total - units, u = 0, i = 0;
    while (i < n && u < skip) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        u += cp >= 0x10000 ? 2 : 1;
        i += (size_t)k;
    }
    return i;
}

char *memory_truncate_tokens(const char *s, int64_t token_limit) {
    if (token_limit <= 0) return xstrdup("");
    size_t n = strlen(s);
    size_t max_chars = (size_t)token_limit * 4;
    if (u16_len(s, n) <= max_chars) return xstrdup(s);
    size_t head = (size_t)floor((double)max_chars * 0.6), tail = max_chars - head;
    size_t hb = u16_prefix(s, n, head), ts = u16_suffix_start(s, n, tail);
    return xasprintf("%.*s\n\n...[truncated]...\n\n%s", (int)hb, s, s + ts);
}

/* ---------- secret redaction (memory-backend/redact.ts) ---------- */

static bool is_delim(unsigned char c) { return c == '-' || c == '_'; }
static bool is_token_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_delim(c);
}

static bool credential_segment(size_t len, bool letter, bool digit) {
    if (!letter && !digit) return false;
    if (letter && digit) return len >= 12;
    return len >= 16;
}

static long keyword_start(const char *in, size_t delim) {
    static const char *const KW[] = {"password", "secret", "token", "key", "tok", "sk", "pk", "rk"};
    for (size_t i = 0; i < ARRAY_LEN(KW); i++) {
        size_t kl = strlen(KW[i]);
        if (delim >= kl && !strncmp(in + delim - kl, KW[i], kl)) return (long)(delim - kl);
    }
    return -1;
}

static char *redact_keywords(const char *in) {
    size_t n = strlen(in), idx = 0, copied = 0;
    sb_t out = SB_INIT;
    bool any = false;
    VEC(size_t) starts = {0};
    VEC(bool) cred = {0};
    while (idx < n) {
        if (!is_token_char((unsigned char)in[idx])) {
            idx++;
            continue;
        }
        size_t run_start = idx;
        starts.n = cred.n = 0;
        vec_push(&starts, run_start);
        size_t len = 0;
        bool letter = false, digit = false;
        while (idx < n && is_token_char((unsigned char)in[idx])) {
            unsigned char c = (unsigned char)in[idx];
            if (is_delim(c)) {
                vec_push(&cred, credential_segment(len, letter, digit));
                vec_push(&starts, idx + 1);
                len = 0;
                letter = digit = false;
            } else {
                len++;
                if (c >= '0' && c <= '9') digit = true;
                else letter = true;
            }
            idx++;
        }
        vec_push(&cred, credential_segment(len, letter, digit));
        size_t run_end = idx;
        bool *suffix = xcalloc(cred.n + 1, sizeof *suffix);
        for (size_t k = cred.n; k-- > 0;) suffix[k] = suffix[k + 1] || cred.v[k];
        for (size_t k = 1; k < starts.n; k++) {
            if (!suffix[k]) continue;
            long st = keyword_start(in, starts.v[k] - 1);
            if (st < 0 || (size_t)st < copied || (size_t)st < run_start) continue;
            sb_appendn(&out, in + copied, (size_t)st - copied);
            sb_append(&out, "[REDACTED]");
            copied = run_end;
            any = true;
            break;
        }
        free(suffix);
    }
    vec_free(&starts);
    vec_free(&cred);
    if (!any) {
        sb_free(&out);
        return xstrdup(in);
    }
    sb_append(&out, in + copied);
    return sb_detach(&out);
}

static char *redact_jwts(const char *in) {
    size_t n = strlen(in), copied = 0;
    sb_t out = SB_INIT;
    bool any = false;
    const char *d = strchr(in, '.');
    while (d && d > in) {
        size_t dot = (size_t)(d - in);
        size_t start = dot;
        while (start > copied && is_token_char((unsigned char)in[start - 1])) start--;
        size_t middle = dot + 1;
        while (middle < n && is_token_char((unsigned char)in[middle])) middle++;
        size_t end = middle + 1;
        while (end < n && is_token_char((unsigned char)in[end])) end++;
        bool jwt = dot - start >= 16 && middle < n && in[middle] == '.' && middle - dot - 1 >= 16 && end - middle - 1 >= 16;
        if (jwt) {
            sb_appendn(&out, in + copied, start - copied);
            sb_append(&out, "[REDACTED]");
            copied = end;
            any = true;
            d = end < n ? strchr(in + end, '.') : NULL;
            continue;
        }
        d = strchr(d + 1, '.');
    }
    if (!any) {
        sb_free(&out);
        return xstrdup(in);
    }
    if (copied < n) sb_append(&out, in + copied);
    return sb_detach(&out);
}

typedef struct {
    const char *const *prefixes;
    bool (*cls)(unsigned char);
    size_t min;
    bool exact;
} fixed_pat_t;

static bool cls_upper_digit(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
static bool cls_alnum(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
static bool cls_alnum_us(unsigned char c) { return cls_alnum(c) || c == '_'; }
static bool cls_alnum_dash(unsigned char c) { return cls_alnum(c) || c == '-'; }
static bool cls_alnum_us_dash(unsigned char c) { return cls_alnum(c) || c == '_' || c == '-'; }

static char *apply_fixed(const char *in, const fixed_pat_t *p) {
    sb_t out = SB_INIT;
    size_t n = strlen(in), i = 0, copied = 0;
    bool any = false;
    while (i < n) {
        size_t end = 0;
        for (const char *const *pre = p->prefixes; *pre && !end; pre++) {
            size_t pl = strlen(*pre);
            if (strncmp(in + i, *pre, pl) != 0) continue;
            size_t j = i + pl, cnt = 0;
            while (j < n && p->cls((unsigned char)in[j]) && (!p->exact || cnt < p->min)) j++, cnt++;
            if (cnt >= p->min) end = j;
        }
        if (end) {
            sb_appendn(&out, in + copied, i - copied);
            sb_append(&out, "[REDACTED]");
            copied = i = end;
            any = true;
        } else {
            i++;
        }
    }
    if (!any) {
        sb_free(&out);
        return xstrdup(in);
    }
    sb_append(&out, in + copied);
    return sb_detach(&out);
}

char *memory_redact_secrets(const char *s) {
    if (!s) return xstrdup("");
    static const char *const AWS[] = {"AKIA", "ASIA", NULL};
    static const char *const GH[] = {"ghp_", "gho_", "ghu_", "ghs_", "ghr_", NULL};
    static const char *const GHPAT[] = {"github_pat_", NULL};
    static const char *const NPM[] = {"npm_", NULL};
    static const char *const SLACK[] = {"xoxb-", "xoxa-", "xoxp-", "xoxr-", "xoxs-", NULL};
    static const char *const GOOG[] = {"AIza", NULL};
    const fixed_pat_t pats[] = {
        {AWS, cls_upper_digit, 16, true}, {GH, cls_alnum, 20, false},      {GHPAT, cls_alnum_us, 20, false},
        {NPM, cls_alnum, 30, false},      {SLACK, cls_alnum_dash, 10, false}, {GOOG, cls_alnum_us_dash, 30, false},
    };
    char *k = redact_keywords(s);
    char *out = redact_jwts(k);
    free(k);
    for (size_t i = 0; i < ARRAY_LEN(pats); i++) {
        char *x = apply_fixed(out, &pats[i]);
        free(out);
        out = x;
    }
    return out;
}

/* ---------- lesson text normalization ---------- */

static bool is_cf(uint32_t c) {
    return c == 0xAD || (c >= 0x600 && c <= 0x605) || c == 0x61C || c == 0x6DD || c == 0x70F || c == 0x890 ||
           c == 0x891 || c == 0x8E2 || c == 0x180E || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2060 && c <= 0x2064) || (c >= 0x2066 && c <= 0x206F) || c == 0xFEFF ||
           (c >= 0xFFF9 && c <= 0xFFFB) || c == 0x110BD || c == 0x110CD || (c >= 0x13430 && c <= 0x1343F) ||
           (c >= 0x1BCA0 && c <= 0x1BCA3) || (c >= 0x1D173 && c <= 0x1D17A) || c == 0xE0001 ||
           (c >= 0xE0020 && c <= 0xE007F);
}

static bool is_js_space(uint32_t c) {
    return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

/* neutralizeInjection / sanitizeManagedDescription: control+format chars -> space, drop <>`, ~~+ -> ~,
 * whitespace runs -> one space, trim */
char *memory_neutralize(const char *s) {
    size_t n = s ? strlen(s) : 0;
    sb_t b = SB_INIT;
    bool pending_space = false, prev_tilde = false;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        i += (size_t)k;
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || is_cf(cp)) cp = ' ';
        if (cp == '<' || cp == '>' || cp == '`') continue;
        if (is_js_space(cp)) {
            pending_space = b.len > 0;
            continue;
        }
        if (cp == '~' && prev_tilde && !pending_space) continue;
        if (pending_space) {
            sb_putc(&b, ' ');
            pending_space = false;
        }
        prev_tilde = cp == '~';
        sb_append_utf8(&b, cp);
    }
    return sb_detach(&b);
}

/* ---------- learned.md ---------- */

#define LEARNED_FILE "learned.md"
#define MAX_LEARNED_LESSONS 100
#define MAX_LEARNED_CONTENT 2000
#define MAX_LEARNED_CONTEXT 400

static char *normalize_learned(const char *text, size_t max_units) {
    char *ne = memory_neutralize(text);
    char *red = memory_redact_secrets(ne);
    char *tr = str_trim(red);
    free(ne);
    free(red);
    size_t cut = u16_prefix(tr, strlen(tr), max_units);
    tr[cut] = 0;
    return tr;
}

static bool is_lesson_line(const char *l) {
    while (*l) {
        uint32_t cp;
        int k = utf8_decode(l, strlen(l), &cp);
        if (!is_js_space(cp)) break;
        l += k;
    }
    return str_starts(l, "- ");
}

static pthread_mutex_t g_learned_mu = PTHREAD_MUTEX_INITIALIZER;

int memory_save_lesson(const char *agent_dir, const char *cwd, const char *content, const char *context, char **msg) {
    *msg = NULL;
    char *c = normalize_learned(content ? content : "", MAX_LEARNED_CONTENT);
    if (!*c) {
        free(c);
        *msg = xstrdup("Empty lesson; nothing stored.");
        return 0;
    }
    char *x = context ? normalize_learned(context, MAX_LEARNED_CONTEXT) : xstrdup("");
    char *line = *x ? xasprintf("- %s _(context: %s)_", c, x) : xasprintf("- %s", c);
    free(c);
    free(x);
    char *root = memory_root(agent_dir, cwd);
    char *file = path_join(root, LEARNED_FILE);
    free(root);
    pthread_mutex_lock(&g_learned_mu);
    size_t len = 0;
    char *existing = fs_read(file, &len);
    strvec_t lines = existing ? str_split(existing, "\n") : (strvec_t){0};
    free(existing);
    if (lines.n && !*lines.v[lines.n - 1]) free(vec_pop(&lines));
    strvec_t out = {0};
    for (size_t i = 0; i < lines.n; i++) {
        char *t = str_trim(lines.v[i]);
        bool dup = is_lesson_line(lines.v[i]) && !strcmp(t, line);
        free(t);
        if (!dup) strvec_push(&out, lines.v[i]);
    }
    size_t first = out.n;
    for (size_t i = 0; i < out.n; i++)
        if (is_lesson_line(out.v[i])) {
            first = i;
            break;
        }
    vec_push(&out, NULL);
    memmove(out.v + first + 1, out.v + first, (out.n - 1 - first) * sizeof(char *));
    out.v[first] = xstrdup(line);
    size_t count = 0;
    for (size_t i = 0; i < out.n; i++) count += is_lesson_line(out.v[i]);
    for (size_t i = out.n; i-- > 0 && count > MAX_LEARNED_LESSONS;) {
        if (!is_lesson_line(out.v[i])) continue;
        free(out.v[i]);
        memmove(out.v + i, out.v + i + 1, (out.n - i - 1) * sizeof(char *));
        out.n--;
        count--;
    }
    char *body = strvec_join(&out, "\n");
    char *text = xasprintf("%s\n", body);
    int rc = fs_write_atomic(file, text, strlen(text), 0644);
    pthread_mutex_unlock(&g_learned_mu);
    free(body);
    free(text);
    strvec_free(&lines);
    strvec_free(&out);
    free(file);
    free(line);
    if (rc != 0) {
        *msg = xasprintf("could not write %s", LEARNED_FILE);
        return -1;
    }
    *msg = xstrdup("Lesson saved to " LEARNED_FILE ".");
    return 1;
}

static char *read_learned(const char *root) {
    char *p = path_join(root, LEARNED_FILE);
    char *raw = fs_read(p, NULL);
    free(p);
    if (!raw) return xstrdup("");
    char *t = str_trim(raw);
    free(raw);
    if (!*t) return t;
    strvec_t lines = str_split(t, "\n");
    free(t);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < lines.n; i++) {
        char *ne = memory_neutralize(lines.v[i]);
        char *red = memory_redact_secrets(ne);
        if (i) sb_putc(&b, '\n');
        sb_append(&b, red);
        free(ne);
        free(red);
    }
    strvec_free(&lines);
    return sb_detach(&b);
}

/* ---------- system-prompt memory block ---------- */

typedef struct {
    int64_t max_rollouts_per_startup, thread_scan_limit, max_raw_for_global, stage1_concurrency, stage1_lease,
        stage1_retry_delay, phase2_lease, phase2_retry_delay, phase2_heartbeat, phase1_input_limit, fallback_limit,
        injection_limit;
    double max_age_days, min_idle_hours, payload_percent;
} mem_cfg_t;

static void load_cfg(mem_cfg_t *c) {
    c->max_rollouts_per_startup = settings_int("memories.maxRolloutsPerStartup", 64);
    c->max_age_days = settings_num("memories.maxRolloutAgeDays", 30);
    c->min_idle_hours = settings_num("memories.minRolloutIdleHours", 12);
    c->thread_scan_limit = settings_int("memories.threadScanLimit", 300);
    c->max_raw_for_global = settings_int("memories.maxRawMemoriesForGlobal", 200);
    c->stage1_concurrency = settings_int("memories.stage1Concurrency", 8);
    c->stage1_lease = settings_int("memories.stage1LeaseSeconds", 120);
    c->stage1_retry_delay = settings_int("memories.stage1RetryDelaySeconds", 120);
    c->phase2_lease = settings_int("memories.phase2LeaseSeconds", 180);
    c->phase2_retry_delay = settings_int("memories.phase2RetryDelaySeconds", 180);
    c->phase2_heartbeat = settings_int("memories.phase2HeartbeatSeconds", 30);
    c->payload_percent = settings_num("memories.rolloutPayloadPercent", 0.7);
    c->phase1_input_limit = settings_int("memories.phase1InputTokenLimit", 4000);
    c->fallback_limit = settings_int("memories.fallbackTokenLimit", 16000);
    c->injection_limit = settings_int("memories.summaryInjectionTokenLimit", 5000);
}

char *memory_render_instructions(const char *summary, const char *learned, int64_t limit) {
    if ((!summary || !*summary) && (!learned || !*learned)) return NULL;
    char *sum_out = xstrdup("");
    if (summary && *summary) {
        char *t = memory_truncate_tokens(summary, limit);
        free(sum_out);
        sum_out = str_trim(t);
        free(t);
    }
    int64_t budget = limit - (int64_t)ceil((double)u16_len(sum_out, strlen(sum_out)) / 4.0);
    if (budget < 0) budget = 0;
    char *learned_out = xstrdup("");
    if (learned && *learned && budget > 0) {
        char *t = memory_truncate_tokens(learned, budget);
        free(learned_out);
        learned_out = str_trim(t);
        free(t);
    }
    char *r = NULL;
    if (*sum_out || *learned_out) {
        json_t *ctx = json_obj();
        json_set_str(ctx, "memory_summary", sum_out);
        json_set_str(ctx, "learned", learned_out);
        r = prompt_render("coding-agent/prompts/memories/read-path.md", ctx);
        json_free(ctx);
    }
    free(sum_out);
    free(learned_out);
    return r;
}

struct memory_rt {
    pthread_mutex_t mu;
    pthread_t th;
    bool th_running;
    volatile sig_atomic_t cancel;
    /* per-session instruction snapshot (memoryToolDeveloperInstructionsBySession) */
    bool cached;
    char *cache_session; /* session file the snapshot belongs to (NULL = none) */
    bool has_snapshot;
    char *snap_summary, *snap_learned;
    bool refresh_prompt;
};

memory_rt_t *memory_rt_new(void) {
    memory_rt_t *m = xcalloc(1, sizeof *m);
    pthread_mutex_init(&m->mu, NULL);
    return m;
}

static void cache_clear_locked(memory_rt_t *m) {
    m->cached = false;
    m->has_snapshot = false;
    free(m->cache_session);
    free(m->snap_summary);
    free(m->snap_learned);
    m->cache_session = m->snap_summary = m->snap_learned = NULL;
}

void memory_rt_free(memory_rt_t *m) {
    if (!m) return;
    m->cancel = 1;
    if (m->th_running) pthread_join(m->th, NULL);
    cache_clear_locked(m);
    pthread_mutex_destroy(&m->mu);
    free(m);
}

static const char *app_session_path(struct app *a) { return a->session && a->session->persist ? a->session->file : NULL; }

static char *read_summary(const char *root) {
    char *p = path_join(root, "memory_summary.md");
    char *raw = fs_read(p, NULL);
    free(p);
    char *t = str_trim(raw ? raw : "");
    free(raw);
    return t;
}

/* instructions for whichever backend is active; NULL when it contributes nothing */
char *memory_instructions(struct app *a) {
    if (!backend_is("local")) return NULL; /* off; hindsight/mnemopi/sharpshooter are not implemented */
    mem_cfg_t cfg;
    load_cfg(&cfg);
    memory_rt_t *m = a->memory;
    const char *sf = app_session_path(a);
    char *root = memory_root(omc_agent_dir(), a->cwd);
    if (!m) {
        char *s = read_summary(root), *l = read_learned(root);
        char *r = memory_render_instructions(s, l, cfg.injection_limit);
        free(s);
        free(l);
        free(root);
        return r;
    }
    pthread_mutex_lock(&m->mu);
    if (!m->cached || !str_eq(m->cache_session, sf)) {
        cache_clear_locked(m);
        m->cached = true;
        m->cache_session = sf ? xstrdup(sf) : NULL;
        m->has_snapshot = true;
        m->snap_summary = read_summary(root);
        m->snap_learned = read_learned(root);
    }
    char *r = m->has_snapshot ? memory_render_instructions(m->snap_summary, m->snap_learned, cfg.injection_limit) : NULL;
    pthread_mutex_unlock(&m->mu);
    free(root);
    return r;
}

static bool conversation_started(struct app *a) {
    pthread_mutex_lock(&a->agent->mu);
    bool s = json_len(a->agent->messages) > 0;
    pthread_mutex_unlock(&a->agent->mu);
    return s;
}

bool memory_take_prompt_refresh(struct app *a) {
    memory_rt_t *m = a->memory;
    if (!m) return false;
    pthread_mutex_lock(&m->mu);
    bool r = m->refresh_prompt;
    m->refresh_prompt = false;
    pthread_mutex_unlock(&m->mu);
    if (r && conversation_started(a)) r = false;
    if (r) app_rebuild_system_prompt(a);
    return r;
}

/* ---------- rollout collection (collectThreads) ---------- */

typedef VEC(mem_thread_t) mem_threads_t;

static void collect_threads(const char *session_dir, const char *current_id, mem_threads_t *out) {
    dirlist_t dl;
    if (!session_dir || fs_list_dir(session_dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.n; i++) {
        const char *name = dl.v[i].name;
        if (dl.v[i].is_dir || !str_ends(name, ".jsonl")) continue;
        char *full = path_join(session_dir, name);
        struct stat st;
        if (stat(full, &st) != 0) {
            free(full);
            continue;
        }
        char *cwd = xstrdup("");
        char *id = xstrndup(name, strlen(name) - 6);
        const size_t HEAD = 64 * 1024;
        size_t len = 0;
        bool large = (size_t)st.st_size > HEAD, trunc = false;
        char *text = large ? fs_read_limit(full, HEAD, &len, &trunc) : fs_read(full, &len);
        if (text) {
            strvec_t lines = str_split(text, "\n");
            bool saw_title = false;
            for (size_t k = 0; k < lines.n; k++) {
                if (large && k == lines.n - 1) { /* line cut by the head window: read it all */
                    free(text);
                    strvec_free(&lines);
                    text = fs_read(full, &len);
                    lines = str_split(text ? text : "", "\n");
                    large = false;
                }
                char *l = str_trim(lines.v[k]);
                if (!*l) {
                    free(l);
                    continue;
                }
                json_t *h = json_parse_str(l);
                free(l);
                if (!saw_title && str_eq(json_get_str(h, "type"), "title")) {
                    saw_title = true;
                    json_free(h);
                    continue;
                }
                if (str_eq(json_get_str(h, "type"), "session")) {
                    if (json_get_str(h, "cwd")) {
                        free(cwd);
                        cwd = xstrdup(json_get_str(h, "cwd"));
                    }
                    if (json_get_str(h, "id")) {
                        free(id);
                        id = xstrdup(json_get_str(h, "id"));
                    }
                }
                json_free(h);
                break;
            }
            strvec_free(&lines);
            free(text);
        }
        if (current_id && !strcmp(id, current_id)) {
            free(id);
            free(cwd);
            free(full);
            continue;
        }
        mem_thread_t t = {id, (int64_t)st.st_mtime, full, cwd, xstrdup("cli")};
        vec_push(out, t);
    }
    dirlist_free(&dl);
}

/* ---------- stage-1 input (buildStage1RolloutItems) ---------- */

static char *message_text(const json_t *m) {
    json_t *c = json_get(m, "content");
    if (json_is_str(c)) return xstrdup(json_sv(c));
    if (!json_is_arr(c)) return xstrdup("");
    sb_t b = SB_INIT;
    json_t *it;
    json_arr_foreach(c, i, it) {
        if (i) sb_putc(&b, '\n');
        const char *ty = json_get_str(it, "type");
        if (str_eq(ty, "text")) sb_append(&b, json_get_str(it, "text") ? json_get_str(it, "text") : "");
        else if (str_eq(ty, "toolCall")) {
            const char *tn = json_get_str(it, "toolName");
            char *args = json_get(it, "arguments") ? json_dump(json_get(it, "arguments"), 0) : NULL;
            sb_appendf(&b, "%s %s", tn ? tn : "undefined", args ? args : "undefined");
            free(args);
        }
    }
    return sb_detach(&b);
}

/* persistable message from one JSONL line, serialized like JSON.stringify (owned) or NULL */
static char *persistable_line(const char *line, size_t len) {
    json_t *e = json_parse(line, len, NULL);
    if (!json_is_obj(e) || !str_eq(json_get_str(e, "type"), "message")) {
        json_free(e);
        return NULL;
    }
    json_t *m = json_get_obj(e, "message");
    if (!m) {
        json_free(e);
        return NULL;
    }
    const char *role = json_get_str(m, "role");
    bool keep = str_eq(role, "system") || str_eq(role, "developer") || str_eq(role, "user") || str_eq(role, "assistant");
    if (!keep && str_eq(role, "toolResult")) {
        const char *tn = json_get_str(m, "toolName");
        if (str_eq(tn, "bash") || str_eq(tn, "eval") || str_eq(tn, "read") || str_eq(tn, "grep")) {
            char *t = message_text(m);
            size_t u = u16_len(t, strlen(t));
            keep = u > 0 && u <= 32000;
            free(t);
        }
    }
    char *r = NULL;
    if (keep) {
        if (str_eq(role, "assistant")) {
            json_t *c = json_get_arr(m, "content");
            bool has_thinking = false;
            json_t *it;
            json_arr_foreach(c, i, it) {
                const char *ty = json_get_str(it, "type");
                if (str_eq(ty, "thinking") || str_eq(ty, "redactedThinking")) has_thinking = true;
            }
            if (has_thinking) {
                json_t *nc = json_arr();
                json_arr_foreach(c, i, it) {
                    const char *ty = json_get_str(it, "type");
                    if (!str_eq(ty, "thinking") && !str_eq(ty, "redactedThinking")) json_push(nc, json_clone(it));
                }
                json_set(m, "content", nc);
            }
            if (has_thinking || json_get(m, "providerPayload")) json_del(m, "providerPayload");
        }
        r = json_dump(m, 0);
    }
    json_free(e);
    return r;
}

typedef VEC(char *) charvec_t;

static void lines_persistable(const char *buf, size_t n, charvec_t *out) {
    size_t s = 0;
    while (s < n) {
        const char *nl = memchr(buf + s, '\n', n - s);
        size_t e = nl ? (size_t)(nl - buf) : n;
        size_t ls = s, le = e;
        while (ls < le && (buf[ls] == ' ' || buf[ls] == '\t' || buf[ls] == '\r')) ls++;
        while (le > ls && (buf[le - 1] == ' ' || buf[le - 1] == '\t' || buf[le - 1] == '\r')) le--;
        if (le > ls) {
            char *m = persistable_line(buf + ls, le - ls);
            if (m) vec_push(out, m);
        }
        s = e + 1;
    }
}

#define STAGE1_WINDOW (1024 * 1024)

char *memory_build_rollout_items(const char *rollout_path, int64_t token_limit) {
    FILE *f = fopen(rollout_path, "rb");
    if (!f) return NULL;
    if (token_limit <= 0) {
        fclose(f);
        return xstrdup("");
    }
    size_t max_chars = (size_t)token_limit * 4;
    size_t head_chars = (size_t)floor((double)max_chars * 0.6), tail_chars = max_chars - head_chars;
    sb_t head = SB_INIT;
    sb_append(&head, "[");
    size_t head_u = 1;
    bool truncated = false;
    char *line = NULL;
    size_t cap = 0;
    ssize_t nr;
    while ((nr = getline(&line, &cap, f)) > 0) {
        charvec_t ms = {0};
        lines_persistable(line, (size_t)nr, &ms);
        for (size_t i = 0; i < ms.n; i++) {
            if (!truncated) {
                if (head_u > 1) {
                    sb_putc(&head, ',');
                    head_u++;
                }
                sb_append(&head, ms.v[i]);
                head_u += u16_len(ms.v[i], strlen(ms.v[i]));
                if (head_u > max_chars) truncated = true;
            }
            free(ms.v[i]);
        }
        vec_free(&ms);
        if (truncated) break;
    }
    free(line);
    if (!truncated && head_u + 1 <= max_chars) {
        fclose(f);
        sb_putc(&head, ']');
        return sb_detach(&head);
    }
    /* tail: walk the file backwards window by window */
    fseeko(f, 0, SEEK_END);
    off_t end = ftello(f);
    charvec_t pieces = {0}; /* ",<msg>" pieces, newest first */
    size_t tail_u = 1;
    bool exhausted = true;
    char *carry = NULL;
    size_t carry_n = 0;
    while (end > 0 && exhausted) {
        off_t start = end > STAGE1_WINDOW ? end - STAGE1_WINDOW : 0;
        size_t wn = (size_t)(end - start);
        char *buf = xmalloc(wn + carry_n + 1);
        fseeko(f, start, SEEK_SET);
        size_t got = fread(buf, 1, wn, f);
        if (got != wn) {
            free(buf);
            break;
        }
        if (carry_n) memcpy(buf + wn, carry, carry_n);
        size_t bn = wn + carry_n;
        free(carry);
        carry = NULL;
        carry_n = 0;
        end = start;
        size_t cut = 0;
        if (start > 0) {
            char *nl = memchr(buf, '\n', bn);
            if (!nl) {
                carry = buf;
                carry_n = bn;
                continue;
            }
            cut = (size_t)(nl - buf) + 1;
            carry = xmalloc(cut);
            memcpy(carry, buf, cut);
            carry_n = cut;
        }
        charvec_t ms = {0};
        lines_persistable(buf + cut, bn - cut, &ms);
        free(buf);
        for (size_t i = ms.n; i-- > 0;) {
            if (exhausted) {
                tail_u += 1 + u16_len(ms.v[i], strlen(ms.v[i]));
                vec_push(&pieces, ms.v[i]);
                ms.v[i] = NULL;
                if (tail_u >= tail_chars) exhausted = false;
            }
            free(ms.v[i]);
        }
        vec_free(&ms);
    }
    free(carry);
    fclose(f);
    sb_t tail = SB_INIT;
    for (size_t i = pieces.n; i-- > 0;) {
        sb_putc(&tail, (exhausted && i == pieces.n - 1) ? '[' : ',');
        sb_append(&tail, pieces.v[i]);
        free(pieces.v[i]);
    }
    if (exhausted && !pieces.n) sb_putc(&tail, '[');
    sb_putc(&tail, ']');
    vec_free(&pieces);
    const char *h = sb_cstr(&head), *t = sb_cstr(&tail);
    size_t hb = u16_prefix(h, head.len, head_chars), ts = u16_suffix_start(t, tail.len, tail_chars);
    char *r = xasprintf("%.*s\n\n...[truncated]...\n\n%s", (int)hb, h, t + ts);
    sb_free(&head);
    sb_free(&tail);
    return r;
}

/* ---------- model output parsing ---------- */

json_t *memory_parse_json_object(const char *text) {
    if (!text || !*text) return NULL;
    char *err = NULL;
    json_t *j = json_parse(text, strlen(text), &err);
    if (!err && j) {
        if (json_is_obj(j)) return j;
        json_free(j);
        return NULL;
    }
    free(err);
    json_free(j);
    const char *a = strchr(text, '{'), *b = strrchr(text, '}');
    if (!a || !b || b < a) return NULL;
    err = NULL;
    j = json_parse(a, (size_t)(b - a + 1), &err);
    if (err || !json_is_obj(j)) {
        free(err);
        json_free(j);
        return NULL;
    }
    return j;
}

static bool has_exact_keys(const json_t *o, const char *const *keys, size_t nk, bool allow_missing) {
    if (!allow_missing && o->u.obj.n != nk) return false;
    for (size_t i = 0; i < o->u.obj.n; i++) {
        bool found = false;
        for (size_t k = 0; k < nk; k++)
            if (!strcmp(o->u.obj.keys[i], keys[k])) found = true;
        if (!found) return false;
    }
    if (allow_missing) return true;
    for (size_t k = 0; k < nk; k++)
        if (!json_get(o, keys[k])) return false;
    return true;
}

char *memory_rollout_filename(const char *thread_id, const char *slug) {
    if (!slug || !*slug) return xstrdup(thread_id);
    sb_t b = SB_INIT;
    bool in_run = false;
    for (const unsigned char *p = (const unsigned char *)slug; *p; p++) {
        unsigned char c = (unsigned char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            sb_putc(&b, (char)c);
            in_run = false;
        } else if (!in_run) {
            sb_putc(&b, '_');
            in_run = true;
        } else if (c >= 0x80 && (c & 0xC0) == 0x80) {
            /* continuation byte of the same non-ASCII char: still one run */
        }
    }
    while (b.len && b.s[b.len - 1] == '_') b.len--;
    if (b.len > 20) b.len = 20;
    if (!b.len) {
        sb_free(&b);
        return xstrdup(thread_id);
    }
    char *n = xasprintf("%s-%.*s", thread_id, (int)b.len, b.s);
    sb_free(&b);
    return n;
}

static char *sanitize_consolidation_skill_name(const char *raw) {
    char *t = str_trim(raw);
    sb_t b = SB_INIT;
    bool dash = false;
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) {
        unsigned char c = (unsigned char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            sb_putc(&b, (char)c);
            dash = false;
        } else if (!dash) {
            sb_putc(&b, '-');
            dash = true;
        }
    }
    free(t);
    char *s = sb_detach(&b);
    char *p = s;
    while (*p == '-') p++;
    size_t n = strlen(p);
    while (n && p[n - 1] == '-') n--;
    if (n > 64) n = 64;
    char *r = xstrndup(p, n);
    free(s);
    return r;
}

static char *sanitize_rel_path(const char *raw) {
    char *x = str_replace(raw, "\\", "/");
    char *n = str_trim(x);
    free(x);
    if (!*n || *n == '/' || strchr(n, ':')) {
        free(n);
        return NULL;
    }
    strvec_t parts = str_split(n, "/");
    free(n);
    strvec_t keep = {0};
    bool ok = true;
    for (size_t i = 0; i < parts.n && ok; i++) {
        const char *p = parts.v[i];
        if (!*p) continue;
        if (!strcmp(p, ".") || !strcmp(p, "..")) ok = false;
        for (const char *c = p; *c && ok; c++)
            if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '.' ||
                  *c == '_' || *c == '-'))
                ok = false;
        if (ok) strvec_push(&keep, p);
    }
    char *r = ok && keep.n ? strvec_join(&keep, "/") : NULL;
    strvec_free(&parts);
    strvec_free(&keep);
    return r;
}

static bool valid_file_array(const json_t *v) {
    if (!v) return true;
    if (!json_is_arr(v)) return false;
    static const char *const K[] = {"path", "content"};
    json_t *it;
    json_arr_foreach(v, i, it) {
        if (!json_is_obj(it) || !has_exact_keys(it, K, 2, false)) return false;
        if (!json_is_str(json_get(it, "path")) || !json_is_str(json_get(it, "content"))) return false;
    }
    return true;
}

static bool valid_consolidation(const json_t *v) {
    static const char *const TOP[] = {"memory_md", "memory_summary", "skills"};
    static const char *const SK[] = {"name", "content", "scripts", "templates", "examples"};
    if (!json_is_obj(v) || !has_exact_keys(v, TOP, 3, false)) return false;
    if (!json_is_str(json_get(v, "memory_md")) || !json_is_str(json_get(v, "memory_summary"))) return false;
    json_t *sk = json_get(v, "skills");
    if (!json_is_arr(sk)) return false;
    json_t *it;
    json_arr_foreach(sk, i, it) {
        if (!json_is_obj(it) || !has_exact_keys(it, SK, 5, true)) return false;
        if (!json_is_str(json_get(it, "name"))) return false;
        json_t *c = json_get(it, "content");
        if (c && !json_is_str(c)) return false;
        if (!valid_file_array(json_get(it, "scripts")) || !valid_file_array(json_get(it, "templates")) ||
            !valid_file_array(json_get(it, "examples")))
            return false;
    }
    return true;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void list_rel_files(const char *dir, const char *prefix, strvec_t *out) {
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.n; i++) {
        char *rel = *prefix ? xasprintf("%s/%s", prefix, dl.v[i].name) : xstrdup(dl.v[i].name);
        char *full = path_join(dir, dl.v[i].name);
        if (dl.v[i].is_dir && !dl.v[i].is_link) list_rel_files(full, rel, out);
        else if (fs_is_file(full) && !dl.v[i].is_link) strvec_push(out, rel);
        free(full);
        free(rel);
    }
    dirlist_free(&dl);
}

static void prune_empty_dirs(const char *dir) {
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return;
    for (size_t i = 0; i < dl.n; i++) {
        if (!dl.v[i].is_dir || dl.v[i].is_link) continue;
        char *child = path_join(dir, dl.v[i].name);
        prune_empty_dirs(child);
        dirlist_t cl;
        if (fs_list_dir(child, &cl) == 0) {
            if (!cl.n) fs_rm_rf(child);
            dirlist_free(&cl);
        }
        free(child);
    }
    dirlist_free(&dl);
}

static void write_text(const char *path, const char *content) {
    char *t = str_trim(content);
    char *x = xasprintf("%s\n", t);
    fs_write_atomic(path, x, strlen(x), 0644);
    free(t);
    free(x);
}

int memory_apply_consolidation(const char *root, const json_t *parsed, char **err) {
    *err = NULL;
    if (!valid_consolidation(parsed)) {
        *err = xstrdup("phase2 JSON schema validation failure");
        return -1;
    }
    char *red_md = memory_redact_secrets(json_get_str(parsed, "memory_md"));
    char *red_sum = memory_redact_secrets(json_get_str(parsed, "memory_summary"));
    char *md = str_trim(red_md), *sum = str_trim(red_sum);
    free(red_md);
    free(red_sum);
    if (!*md || !*sum) {
        free(md);
        free(sum);
        *err = xstrdup("phase2 returned empty consolidated memory");
        return -1;
    }
    char *p = path_join(root, "MEMORY.md");
    write_text(p, md);
    free(p);
    p = path_join(root, "memory_summary.md");
    write_text(p, sum);
    free(p);
    free(md);
    free(sum);
    char *skills_dir = path_join(root, "skills");
    fs_mkdir_p(skills_dir, 0755);
    strvec_t keep_names = {0};
    json_t *sk;
    json_arr_foreach(json_get(parsed, "skills"), i, sk) {
        char *name = sanitize_consolidation_skill_name(json_get_str(sk, "name"));
        char *red = memory_redact_secrets(json_get_str(sk, "content") ? json_get_str(sk, "content") : "");
        char *content = str_trim(red);
        free(red);
        if (!*name || !*content) {
            free(name);
            free(content);
            continue;
        }
        char *dir = path_join(skills_dir, name);
        strvec_push(&keep_names, name);
        fs_mkdir_p(dir, 0755);
        /* relative path -> content; later entries win (Map semantics) */
        json_t *files = json_obj();
        char *sc = xasprintf("%s\n", content);
        json_set_str(files, "SKILL.md", sc);
        free(sc);
        static const char *const BUCKETS[] = {"scripts", "templates", "examples"};
        for (size_t b = 0; b < ARRAY_LEN(BUCKETS); b++) {
            json_t *f;
            json_arr_foreach(json_get(sk, BUCKETS[b]), k, f) {
                char *rel = sanitize_rel_path(json_get_str(f, "path"));
                if (!rel) continue;
                char *r2 = memory_redact_secrets(json_get_str(f, "content"));
                char *c2 = str_trim(r2);
                free(r2);
                if (*c2) {
                    char *key = xasprintf("%s/%s", BUCKETS[b], rel);
                    char *val = xasprintf("%s\n", c2);
                    json_set_str(files, key, val);
                    free(key);
                    free(val);
                }
                free(c2);
                free(rel);
            }
        }
        strvec_t keys = {0};
        for (size_t k = 0; k < files->u.obj.n; k++) strvec_push(&keys, files->u.obj.keys[k]);
        if (keys.n > 1) qsort(keys.v, keys.n, sizeof(char *), cmp_str);
        for (size_t k = 0; k < keys.n; k++) {
            char *fp = path_join(dir, keys.v[k]);
            const char *v = json_get_str(files, keys.v[k]);
            fs_write_atomic(fp, v, strlen(v), 0644);
            free(fp);
        }
        strvec_t existing = {0};
        list_rel_files(dir, "", &existing);
        for (size_t k = 0; k < existing.n; k++) {
            if (strvec_contains(&keys, existing.v[k])) continue;
            char *fp = path_join(dir, existing.v[k]);
            unlink(fp);
            free(fp);
        }
        prune_empty_dirs(dir);
        strvec_free(&existing);
        strvec_free(&keys);
        json_free(files);
        free(dir);
        free(name);
        free(content);
    }
    dirlist_t dl;
    if (fs_list_dir(skills_dir, &dl) == 0) {
        for (size_t i = 0; i < dl.n; i++) {
            if (!dl.v[i].is_dir || strvec_contains(&keep_names, dl.v[i].name)) continue;
            char *d = path_join(skills_dir, dl.v[i].name);
            fs_rm_rf(d);
            free(d);
        }
        dirlist_free(&dl);
    }
    strvec_free(&keep_names);
    free(skills_dir);
    return 0;
}

/* ---------- phase-2 artifacts ---------- */

static void sync_phase2_artifacts(const char *root, const mem_outputs_t *outs) {
    char *sdir = path_join(root, "rollout_summaries");
    fs_mkdir_p(sdir, 0755);
    strvec_t keep = {0};
    for (size_t i = 0; i < outs->n; i++) {
        const mem_output_t *r = &outs->v[i];
        char *stem = memory_rollout_filename(r->thread_id, r->rollout_slug);
        char *fn = xasprintf("%s.md", stem);
        strvec_push(&keep, fn);
        char *body = xasprintf("thread_id: %s\nupdated_at: %lld\n\n%s", r->thread_id, (long long)r->source_updated_at,
                               r->rollout_summary);
        char *fp = path_join(sdir, fn);
        write_text(fp, body);
        free(fp);
        free(body);
        free(fn);
        free(stem);
    }
    dirlist_t dl;
    if (fs_list_dir(sdir, &dl) == 0) {
        for (size_t i = 0; i < dl.n; i++) {
            if (!str_ends(dl.v[i].name, ".md") || strvec_contains(&keep, dl.v[i].name)) continue;
            char *fp = path_join(sdir, dl.v[i].name);
            fs_rm_rf(fp);
            free(fp);
        }
        dirlist_free(&dl);
    }
    strvec_free(&keep);
    free(sdir);
    sb_t b = SB_INIT;
    if (!outs->n) sb_append(&b, "# Raw Memories\n\nNo raw memories yet.\n");
    else {
        sb_append(&b, "# Raw Memories\n\n");
        for (size_t i = 0; i < outs->n; i++) {
            if (i) sb_putc(&b, '\n');
            char *t = str_trim(outs->v[i].raw_memory);
            sb_appendf(&b, "## %s\nupdated_at: %lld\n%s\n", outs->v[i].thread_id, (long long)outs->v[i].source_updated_at, t);
            free(t);
        }
    }
    char *rp = path_join(root, "raw_memories.md");
    fs_write_atomic(rp, b.s ? b.s : "", b.len, 0644);
    free(rp);
    sb_free(&b);
}

static void cleanup_consolidated(const char *root) {
    const char *names[] = {"MEMORY.md", "memory_summary.md", "skills"};
    for (size_t i = 0; i < ARRAY_LEN(names); i++) {
        char *p = path_join(root, names[i]);
        fs_rm_rf(p);
        free(p);
    }
}

static char *read_rollout_summaries(const char *root) {
    char *sdir = path_join(root, "rollout_summaries");
    dirlist_t dl;
    sb_t b = SB_INIT;
    if (fs_list_dir(sdir, &dl) == 0) {
        for (size_t i = 0; i < dl.n; i++) { /* fs_list_dir sorts by name */
            if (!str_ends(dl.v[i].name, ".md")) continue;
            char *fp = path_join(sdir, dl.v[i].name);
            char *raw = fs_read(fp, NULL);
            char *t = str_trim(raw ? raw : "");
            if (*t) {
                if (b.len) sb_append(&b, "\n\n");
                sb_appendf(&b, "--- %s ---\n%s", dl.v[i].name, t);
            }
            free(t);
            free(raw);
            free(fp);
        }
        dirlist_free(&dl);
    }
    free(sdir);
    if (!b.len) {
        sb_free(&b);
        return xstrdup("No rollout summaries yet.");
    }
    return sb_detach(&b);
}

/* ---------- model calls ---------- */

static char *response_text(const json_t *msg) {
    sb_t b = SB_INIT;
    json_t *c;
    bool first = true;
    json_arr_foreach(json_get_arr(msg, "content"), i, c) {
        if (!str_eq(json_get_str(c, "type"), "text")) continue;
        if (!first) sb_putc(&b, '\n');
        sb_append(&b, json_get_str(c, "text") ? json_get_str(c, "text") : "");
        first = false;
    }
    char *s = sb_detach(&b);
    char *t = str_trim(s);
    free(s);
    return t;
}

/* one completion; owned text, or NULL with *err (owned) */
static char *mem_complete(const ai_model_t *m, const char *system, const char *user, int64_t max_tokens,
                          ai_effort_t effort, const char *session_id, volatile sig_atomic_t *cancel, char **err) {
    *err = NULL;
    json_t *sys = json_arr();
    json_push_str(sys, system);
    json_t *msgs = json_arr();
    json_push(msgs, msg_user_text(user));
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    char *key = app_api_key_for(m);
    o.api_key = key;
    o.max_tokens = max_tokens;
    o.reasoning = model_clamp_effort(m, effort);
    o.session_id = session_id;
    o.cancel = cancel;
    /* chat-completions owns its transient retries in the HTTP transport (memories/index.ts maxHttpAttempts) */
    if (str_eq(m->api, "openai-completions")) o.max_http_attempts = 3;
    json_t *r = ai_complete(m, &ctx, &o);
    char *text = NULL;
    const char *stop = json_get_str(r, "stopReason");
    if (str_eq(stop, "error") || str_eq(stop, "aborted")) {
        const char *em = json_get_str(r, "errorMessage");
        *err = xstrdup(em && *em ? em : "model error");
    } else {
        text = response_text(r);
    }
    json_free(r);
    json_free(sys);
    json_free(msgs);
    free(key);
    return text;
}

static const ai_model_t *resolve_memory_model(const char *role, const ai_model_t *session_model) {
    char *rk = xasprintf("modelRoles.%s", role);
    const char *req = settings_str(rk, NULL);
    free(rk);
    if (!req) req = settings_str("modelRoles.default", NULL);
    if (req) {
        const ai_model_t *m = app_resolve_model_pattern(req, NULL, NULL);
        if (m) return m;
    }
    return session_model ? session_model : (catalog_count() ? catalog_at(0) : NULL);
}

/* ---------- startup pipeline ---------- */

typedef struct {
    memory_rt_t *rt;
    struct app *app;
    mem_cfg_t cfg;
    char *agent_dir, *cwd, *session_dir, *session_id, *state, *root, *worker_id;
    const ai_model_t *session_model;
    /* phase 1 work queue */
    pthread_mutex_t qmu;
    mem_claims_t claims;
    size_t next;
    const ai_model_t *p1_model;
    int64_t p1_budget;
} startup_t;

static bool active(startup_t *s) { return !s->rt->cancel && backend_is("local"); }
static int64_t unix_now(void) { return now_ms() / 1000; }

static int64_t model_token_budget(const ai_model_t *m, const mem_cfg_t *c) {
    int64_t mt = m && m->context_window > 0 ? m->context_window : c->fallback_limit;
    return mt > 2048 ? mt : 2048;
}

static void run_stage1(startup_t *s, const mem_stage1_claim_t *c) {
    int64_t budget = (int64_t)floor((double)s->p1_budget * s->cfg.payload_percent);
    if (s->cfg.phase1_input_limit < budget) budget = s->cfg.phase1_input_limit;
    char *reason = NULL;
    char *items = memory_build_rollout_items(c->rollout_path, budget);
    if (!items) reason = xasprintf("ENOENT: no such file or directory, open '%s'", c->rollout_path);
    char *text = NULL;
    if (!reason) {
        json_t *ctx = json_obj();
        json_set_str(ctx, "thread_id", c->thread_id);
        json_set_str(ctx, "response_items_json", items);
        char *input = prompt_render("coding-agent/prompts/memories/stage_one_input.md", ctx);
        json_free(ctx);
        char *sys = prompt_load("coding-agent/prompts/memories/stage_one_system.md");
        int64_t mt = (int64_t)floor((double)s->p1_budget * 0.2);
        if (mt > 4096) mt = 4096;
        if (mt < 1024) mt = 1024;
        char *err = NULL;
        text = mem_complete(s->p1_model, sys ? sys : "", input ? input : "", mt, EFFORT_LOW, s->session_id,
                            &s->rt->cancel, &err);
        if (!text) reason = err ? err : xstrdup("stage1 model error");
        free(sys);
        free(input);
    }
    free(items);
    if (!active(s)) {
        free(text);
        free(reason);
        return;
    }
    json_t *parsed = NULL;
    if (!reason) {
        parsed = memory_parse_json_object(text);
        static const char *const K[] = {"rollout_summary", "rollout_slug", "raw_memory"};
        if (!parsed) reason = xstrdup("stage1 JSON parse failure");
        else if (!has_exact_keys(parsed, K, 3, false) || !json_is_str(json_get(parsed, "rollout_summary")) ||
                 !(json_is_str(json_get(parsed, "rollout_slug")) || json_get(parsed, "rollout_slug")->type == JSON_NULL) ||
                 !json_is_str(json_get(parsed, "raw_memory")))
            reason = xstrdup("stage1 JSON schema validation failure");
    }
    free(text);
    if (reason) {
        LOGE("Memory phase1 stage1 job failed: thread %s (%s): %s", c->thread_id, c->rollout_path, reason);
        memstore_stage1_failed(s->state, c->thread_id, c->ownership_token, s->cfg.stage1_retry_delay, reason, unix_now());
        free(reason);
        json_free(parsed);
        return;
    }
    char *r1 = memory_redact_secrets(json_get_str(parsed, "raw_memory"));
    char *r2 = memory_redact_secrets(json_get_str(parsed, "rollout_summary"));
    char *raw = str_trim(r1), *sum = str_trim(r2);
    char *slug = NULL;
    if (json_get_str(parsed, "rollout_slug")) {
        char *r3 = memory_redact_secrets(json_get_str(parsed, "rollout_slug"));
        slug = str_trim(r3);
        free(r3);
        if (!*slug) {
            free(slug);
            slug = NULL;
        }
    }
    free(r1);
    free(r2);
    if (!*raw || !*sum)
        memstore_stage1_no_output(s->state, c->thread_id, c->ownership_token, c->source_updated_at, unix_now(), c->cwd);
    else
        memstore_stage1_succeeded(s->state, c->thread_id, c->ownership_token, c->source_updated_at, raw, sum, slug,
                                  unix_now(), c->cwd);
    free(raw);
    free(sum);
    free(slug);
    json_free(parsed);
}

static void *stage1_worker(void *p) {
    startup_t *s = p;
    for (;;) {
        pthread_mutex_lock(&s->qmu);
        size_t i = s->next < s->claims.n ? s->next++ : SIZE_MAX;
        pthread_mutex_unlock(&s->qmu);
        if (i == SIZE_MAX || !active(s)) break;
        run_stage1(s, &s->claims.v[i]);
    }
    return NULL;
}

static void run_phase1(startup_t *s) {
    mem_threads_t threads = {0};
    collect_threads(s->session_dir, s->session_id, &threads);
    if (active(s)) memstore_upsert_threads(s->state, threads.v, threads.n);
    for (size_t i = 0; i < threads.n; i++) {
        free(threads.v[i].id);
        free(threads.v[i].rollout_path);
        free(threads.v[i].cwd);
        free(threads.v[i].source_kind);
    }
    vec_free(&threads);
    if (!active(s)) return;
    s->p1_model = resolve_memory_model("default", s->session_model);
    if (!s->p1_model) return;
    char *key = app_api_key_for(s->p1_model);
    if (!key) return;
    free(key);
    if (!active(s)) return;
    strvec_t ex = {0};
    if (s->session_id) strvec_push(&ex, s->session_id);
    mem_claim_params_t cp = {.now_sec = unix_now(),
                             .thread_scan_limit = s->cfg.thread_scan_limit,
                             .max_rollouts_per_startup = s->cfg.max_rollouts_per_startup,
                             .max_rollout_age_days = s->cfg.max_age_days,
                             .min_rollout_idle_hours = s->cfg.min_idle_hours,
                             .lease_seconds = s->cfg.stage1_lease,
                             .running_concurrency_cap = s->cfg.stage1_concurrency,
                             .worker_id = s->worker_id,
                             .exclude_thread_ids = &ex};
    memstore_claim_stage1(s->state, &cp, &s->claims);
    strvec_free(&ex);
    if (!s->claims.n) return;
    s->p1_budget = model_token_budget(s->p1_model, &s->cfg);
    size_t nw = (size_t)MAX(1, s->cfg.stage1_concurrency);
    if (nw > s->claims.n) nw = s->claims.n;
    pthread_t *th = xcalloc(nw, sizeof *th);
    bool *ok = xcalloc(nw, sizeof *ok);
    for (size_t i = 0; i < nw; i++) ok[i] = pthread_create(&th[i], NULL, stage1_worker, s) == 0;
    for (size_t i = 0; i < nw; i++)
        if (ok[i]) pthread_join(th[i], NULL);
    if (!ok[0]) stage1_worker(s);
    free(th);
    free(ok);
    LOGD("Memory phase1 completed: %zu claimed (%s)", s->claims.n, s->root);
}

typedef struct {
    startup_t *s;
    const char *token;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool done, lost;
} heartbeat_t;

static void *heartbeat_thread(void *p) {
    heartbeat_t *h = p;
    pthread_mutex_lock(&h->mu);
    while (!h->done) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += MAX(1, h->s->cfg.phase2_heartbeat);
        pthread_cond_timedwait(&h->cv, &h->mu, &ts);
        if (h->done || !active(h->s)) break;
        pthread_mutex_unlock(&h->mu);
        bool ok = memstore_heartbeat_global(h->s->state, h->token, h->s->cfg.phase2_lease, unix_now(), h->s->cwd);
        pthread_mutex_lock(&h->mu);
        if (!ok) {
            h->lost = true;
            break;
        }
    }
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

static void phase2_fail(startup_t *s, const char *token, const char *reason) {
    int64_t now = unix_now();
    if (memstore_global_failed(s->state, token, s->cfg.phase2_retry_delay, reason, now, s->cwd)) return;
    if (!memstore_global_failed_unowned(s->state, s->cfg.phase2_retry_delay, reason, now, s->cwd))
        LOGW("Phase2 could not mark failure (ownership lost and unowned fallback skipped): %s", reason);
}

static void run_phase2(startup_t *s) {
    char *token = NULL;
    int64_t iw = 0;
    int rc = memstore_claim_global(s->state, s->worker_id, s->cfg.phase2_lease, unix_now(), s->cwd, &token, &iw);
    if (rc != MEMSTORE_CLAIMED) return;
    mem_outputs_t outs;
    memstore_list_outputs(s->state, s->cfg.max_raw_for_global, s->cwd, &outs);
    int64_t wm = iw;
    for (size_t i = 0; i < outs.n; i++)
        if (outs.v[i].source_updated_at > wm) wm = outs.v[i].source_updated_at;
    fs_mkdir_p(s->root, 0755);
    sync_phase2_artifacts(s->root, &outs);
    if (!active(s)) goto out;
    if (!outs.n) {
        cleanup_consolidated(s->root);
        if (!active(s)) goto out;
        if (!memstore_global_succeeded(s->state, token, wm, unix_now(), s->cwd))
            LOGW("Phase2 empty-input completion lost ownership (%s)", s->root);
        goto out;
    }
    const ai_model_t *m = resolve_memory_model("smol", s->session_model);
    if (!m) {
        phase2_fail(s, token, "No model available for phase2");
        goto out;
    }
    char *key = app_api_key_for(m);
    if (!key) {
        phase2_fail(s, token, "No API key available for phase2");
        goto out;
    }
    free(key);
    if (!active(s)) goto out;
    heartbeat_t hb = {.s = s, .token = token};
    pthread_mutex_init(&hb.mu, NULL);
    pthread_cond_init(&hb.cv, NULL);
    pthread_t ht;
    bool hb_ok = pthread_create(&ht, NULL, heartbeat_thread, &hb) == 0;
    char *rawp = path_join(s->root, "raw_memories.md");
    char *raw = fs_read(rawp, NULL);
    free(rawp);
    char *sums = read_rollout_summaries(s->root);
    char *t1 = memory_truncate_tokens(raw ? raw : "", 20000), *t2 = memory_truncate_tokens(sums, 12000);
    json_t *ctx = json_obj();
    json_set_str(ctx, "raw_memories", t1);
    json_set_str(ctx, "rollout_summaries", t2);
    char *input = prompt_render("coding-agent/prompts/memories/consolidation.md", ctx);
    json_free(ctx);
    free(t1);
    free(t2);
    free(raw);
    free(sums);
    char *sys = prompt_load("coding-agent/prompts/memories/consolidation_system.md");
    char *err = NULL;
    char *text = mem_complete(m, sys ? sys : "", input ? input : "", 8192, EFFORT_MEDIUM, s->session_id, &s->rt->cancel, &err);
    free(sys);
    free(input);
    char *reason = NULL;
    if (active(s)) {
        if (!text) reason = xasprintf("Error: %s", err ? err : "phase2 model error");
        else {
            json_t *parsed = memory_parse_json_object(text);
            if (!parsed) reason = xstrdup("Error: phase2 JSON parse failure");
            else {
                char *aerr = NULL;
                if (memory_apply_consolidation(s->root, parsed, &aerr) != 0) {
                    reason = xasprintf("Error: %s", aerr);
                    free(aerr);
                }
                json_free(parsed);
            }
        }
        if (!reason && active(s)) {
            pthread_mutex_lock(&hb.mu);
            bool lost = hb.lost;
            pthread_mutex_unlock(&hb.mu);
            if (lost) reason = xstrdup("Error: Phase2 lease ownership lost before completion");
            else if (!memstore_global_succeeded(s->state, token, wm, unix_now(), s->cwd))
                reason = xstrdup("Error: Phase2 could not mark success: ownership lost");
        }
        if (reason && active(s)) phase2_fail(s, token, reason);
    }
    free(reason);
    free(err);
    free(text);
    pthread_mutex_lock(&hb.mu);
    hb.done = true;
    pthread_cond_broadcast(&hb.cv);
    pthread_mutex_unlock(&hb.mu);
    if (hb_ok) pthread_join(ht, NULL);
    pthread_cond_destroy(&hb.cv);
    pthread_mutex_destroy(&hb.mu);
out:
    mem_outputs_free(&outs);
    free(token);
}

static void *startup_thread(void *p) {
    startup_t *s = p;
    if (active(s)) run_phase1(s);
    if (active(s)) run_phase2(s);
    if (active(s) && !conversation_started(s->app)) {
        /* refreshMemoryToolDeveloperInstructionsCacheAfterStartup: adopt the new summary, keep the lessons
         * the session already captured, and rebuild the prompt before the first request */
        memory_rt_t *m = s->rt;
        char *sum = read_summary(s->root);
        pthread_mutex_lock(&m->mu);
        const char *sf = app_session_path(s->app);
        if (!m->cached || !str_eq(m->cache_session, sf)) {
            free(m->snap_learned);
            m->snap_learned = xstrdup("");
            free(m->cache_session);
            m->cache_session = sf ? xstrdup(sf) : NULL;
            m->cached = true;
        }
        free(m->snap_summary);
        m->snap_summary = sum;
        m->has_snapshot = true;
        m->refresh_prompt = true;
        pthread_mutex_unlock(&m->mu);
    }
    mem_claims_free(&s->claims);
    pthread_mutex_destroy(&s->qmu);
    free(s->agent_dir);
    free(s->cwd);
    free(s->session_dir);
    free(s->session_id);
    free(s->state);
    free(s->root);
    free(s->worker_id);
    free(s);
    return NULL;
}

void memory_start(struct app *a) {
    if (!a->memory || a->memory->th_running) return;
    if (!backend_is("local")) return;
    if (a->opts.subagent) return;
    if (!app_session_path(a)) return;
    char *state = memory_state_path(omc_agent_dir());
    char *err = NULL;
    if (memstore_check(state, &err) != 0) {
        LOGD("Memory startup skipped: state DB unavailable: %s", err ? err : "");
        free(err);
        free(state);
        return;
    }
    startup_t *s = xcalloc(1, sizeof *s);
    s->rt = a->memory;
    s->app = a;
    load_cfg(&s->cfg);
    s->agent_dir = xstrdup(omc_agent_dir());
    s->cwd = xstrdup(a->cwd);
    s->session_dir = a->session->dir ? xstrdup(a->session->dir) : path_dirname(a->session->file);
    s->session_id = xstrdup(session_id(a->session));
    s->state = state;
    s->root = memory_root(s->agent_dir, s->cwd);
    s->worker_id = xasprintf("memory-%d", (int)getpid());
    s->session_model = a->model;
    pthread_mutex_init(&s->qmu, NULL);
    a->memory->cancel = 0;
    if (pthread_create(&a->memory->th, NULL, startup_thread, s) == 0) a->memory->th_running = true;
    else startup_thread(s);
}

/* ---------- /memory ---------- */

static void res(memory_cmd_result_t *out, const char *level, const char *title, char *text) {
    out->level = level;
    out->title = title ? xstrdup(title) : NULL;
    out->text = text;
}

static int local_clear(struct app *a, char **err) {
    if (a->memory) {
        pthread_mutex_lock(&a->memory->mu);
        cache_clear_locked(a->memory);
        pthread_mutex_unlock(&a->memory->mu);
    }
    char *state = memory_state_path(omc_agent_dir());
    int rc = memstore_clear_cwd(state, a->cwd);
    free(state);
    if (rc != 0) {
        *err = xstrdup("could not update the memory state");
        return -1;
    }
    char *root = memory_root(omc_agent_dir(), a->cwd);
    fs_rm_rf(root);
    free(root);
    return 0;
}

static int local_enqueue(struct app *a, char **err) {
    char *state = memory_state_path(omc_agent_dir());
    int rc = memstore_enqueue_global(state, unix_now(), a->cwd, true);
    free(state);
    if (rc != 0) *err = xstrdup("could not update the memory state");
    return rc;
}

static char *stats_unavailable(const char *backend, const char *action) {
    if (!strcmp(backend, "off")) return xstrdup("Memory backend is off — there is nothing to show.");
    return xasprintf("Memory %s is not available for the %s backend.", action, backend);
}

void memory_command(struct app *a, const char *args, memory_cmd_result_t *out) {
    memset(out, 0, sizeof *out);
    while (*args == ' ' || *args == '\t') args++;
    char *action = xstrndup(args, strcspn(args, " \t\n"));
    for (char *p = action; *p; p++) *p = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    if (!*action) {
        free(action);
        action = xstrdup("view");
    }
    const char *backend = memory_backend();
    bool local = !strcmp(backend, "local");
    char *err = NULL;
    if (!strcmp(action, "view")) {
        char *payload = memory_instructions(a);
        if (!payload) res(out, "warning", NULL, xstrdup("Memory payload is empty (memory backend off, disabled, or no memory available)."));
        else res(out, "panel", "Memory Injection Payload", payload);
    } else if (!strcmp(action, "reset") || !strcmp(action, "clear")) {
        if (local && local_clear(a, &err) != 0) res(out, "error", NULL, xasprintf("Memory clear failed: %s", err));
        else {
            app_rebuild_system_prompt(a);
            res(out, "info", NULL, xstrdup("Memory data cleared and system prompt refreshed."));
        }
    } else if (!strcmp(action, "enqueue") || !strcmp(action, "rebuild")) {
        if (local && local_enqueue(a, &err) != 0) res(out, "error", NULL, xasprintf("Memory enqueue failed: %s", err));
        else res(out, "info", NULL, xstrdup("Memory consolidation enqueued."));
    } else if (!strcmp(action, "queue")) {
        res(out, "warning", NULL, xasprintf("Memory queue is not available for the %s backend.", backend));
    } else if (!strcmp(action, "sync")) {
        if (local && local_enqueue(a, &err) != 0) res(out, "error", NULL, xasprintf("Memory sync failed: %s", err));
        else res(out, "info", NULL, xstrdup("Memory consolidation ran."));
    } else if (!strcmp(action, "stats") || !strcmp(action, "diagnose")) {
        res(out, "warning", NULL, stats_unavailable(backend, action));
    } else if (!strcmp(action, "mm")) {
        res(out, "error", NULL, xstrdup("Hindsight backend is not active for this session."));
    } else {
        res(out, "error", NULL, xstrdup("Usage: /memory <view|stats|diagnose|clear|reset|enqueue|rebuild|queue|sync|mm ...>"));
    }
    free(err);
    free(action);
}

void memory_cmd_result_free(memory_cmd_result_t *r) {
    free(r->title);
    free(r->text);
    r->title = r->text = NULL;
}

/* ---------- memory:// (internal-urls/memory-protocol.ts, file-backed namespace) ---------- */

char *memory_url_resolve(const char *cwd, const char *url, char **err) {
    *err = NULL;
    const char *backend = memory_backend();
    if (!strcmp(backend, "off")) {
        *err = xstrdup("Unknown protocol: memory://");
        return NULL;
    }
    const char *rest = url + strlen("memory://");
    size_t hl = strcspn(rest, "/?#");
    char *ns = xstrndup(rest, hl);
    const char *path = rest + hl;
    if (!*ns) {
        free(ns);
        *err = xstrdup("memory:// URL requires a namespace: memory://root or memory://<memory-id>");
        return NULL;
    }
    if (strcmp(ns, "root") != 0) {
        if (!strcmp(backend, "hindsight"))
            *err = xstrdup("Hindsight memories are not addressable via memory://. Recall results are final — use `recall` "
                           "to search or `reflect` to synthesize. `read memory://<id>` is only available with "
                           "memory.backend=mnemopi.");
        else
            *err = xasprintf("Unknown memory namespace: %s. Supported: root (file-backed memory summary), or a mnemopi "
                             "memory id when memory.backend=mnemopi is active.",
                             ns);
        free(ns);
        return NULL;
    }
    free(ns);
    if (strcmp(backend, "local") != 0) {
        const char *hint = !strcmp(backend, "mnemopi")
                               ? " Use `recall`/`reflect` to search Mnemopi memories, or `read memory://<memory-id>` for a full row."
                           : !strcmp(backend, "hindsight") ? " Use `recall`/`reflect` to search Hindsight memories."
                                                           : "";
        *err = xasprintf("File-backed memory artifacts only exist with memory.backend=local (active backend: %s).%s",
                         backend, hint);
        return NULL;
    }
    char *root = memory_root(omc_agent_dir(), cwd);
    if (!fs_is_dir(root)) {
        free(root);
        *err = xstrdup("Memory artifacts are not available for this project yet. Run a session with memories enabled first.");
        return NULL;
    }
    char *rel;
    size_t pl = strcspn(path, "?#");
    if (pl == 0 || (pl == 1 && *path == '/')) rel = xstrdup("memory_summary.md");
    else rel = xstrndup(path + 1, pl - 1);
    /* percent-decoding + validateRelativePath: no absolute paths or traversal */
    sb_t d = SB_INIT;
    for (const char *p = rel; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hx[3] = {p[1], p[2], 0};
            char *end;
            long v = strtol(hx, &end, 16);
            if (*end == 0) {
                sb_putc(&d, (char)v);
                p += 2;
                continue;
            }
        }
        sb_putc(&d, *p);
    }
    free(rel);
    rel = sb_detach(&d);
    strvec_t parts = str_split(rel, "/");
    bool bad = *rel == '/' || strchr(rel, '\\') != NULL;
    for (size_t i = 0; i < parts.n && !bad; i++)
        if (!strcmp(parts.v[i], "..")) bad = true;
    strvec_free(&parts);
    if (bad) {
        *err = xasprintf("Path traversal (..) is not allowed in memory:// URLs: %s", url);
        free(rel);
        free(root);
        return NULL;
    }
    char *full = path_join(root, rel);
    free(rel);
    char *real_root = path_real(root);
    char *real = path_real(full);
    free(root);
    if (!real) {
        *err = xasprintf("Memory file not found: %s", url);
        free(full);
        free(real_root);
        return NULL;
    }
    if (!real_root || !(str_eq(real, real_root) || (str_starts(real, real_root) && real[strlen(real_root)] == '/'))) {
        *err = xasprintf("memory:// URL escapes the memory root: %s", url);
        free(full);
        free(real);
        free(real_root);
        return NULL;
    }
    free(real_root);
    free(full);
    return real;
}
