/* memstore.c - local memory job/output store (see memstore.h; port of omp memories/storage.ts). */
#include "memstore.h"

#include "core/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

char *memstore_normalize_cwd(const char *cwd) {
#ifdef _WIN32
    return str_lower(cwd ? cwd : "");
#else
    return xstrdup(cwd ? cwd : "");
#endif
}

static char *global_job_key(const char *cwd) {
    char *n = memstore_normalize_cwd(cwd);
    char *k = xasprintf("global:%s", n);
    free(n);
    return k;
}

/* ---------- transactions ---------- */

typedef struct {
    int fd;
    const char *path;
    json_t *doc;
    bool dirty;
} tx_t;

static json_t *new_doc(void) {
    json_t *d = json_obj();
    json_set_int(d, "version", 1);
    json_set(d, "threads", json_obj());
    json_set(d, "stage1_outputs", json_obj());
    json_set(d, "jobs", json_obj());
    return d;
}

static int tx_begin(tx_t *t, const char *path) {
    memset(t, 0, sizeof *t);
    t->path = path;
    char *dir = path_dirname(path);
    fs_mkdir_p(dir, 0700);
    free(dir);
    char *lock = xasprintf("%s.lock", path);
    t->fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(lock);
    if (t->fd < 0) return -1;
    while (flock(t->fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            close(t->fd);
            return -1;
        }
    }
    size_t len = 0;
    char *raw = fs_read(path, &len);
    if (raw && len) {
        t->doc = json_parse(raw, len, NULL);
        if (!json_is_obj(t->doc)) {
            /* corrupt state: keep a copy for inspection and start over (SQLite would refuse to open) */
            json_free(t->doc);
            t->doc = NULL;
            char *bak = xasprintf("%s.corrupt", path);
            fs_write_atomic(bak, raw, len, 0600);
            free(bak);
            LOGW("memory state %s was unreadable; starting fresh", path);
        }
    }
    free(raw);
    if (!t->doc) {
        t->doc = new_doc();
        t->dirty = true;
    }
    const char *tabs[] = {"threads", "stage1_outputs", "jobs"};
    for (size_t i = 0; i < ARRAY_LEN(tabs); i++)
        if (!json_get_obj(t->doc, tabs[i])) {
            json_set(t->doc, tabs[i], json_obj());
            t->dirty = true;
        }
    return 0;
}

static int tx_end(tx_t *t) {
    int rc = 0;
    if (t->dirty) {
        char *s = json_dump(t->doc, 0);
        rc = fs_write_atomic(t->path, s, strlen(s), 0600) == 0 ? 0 : -1;
        free(s);
    }
    json_free(t->doc);
    t->doc = NULL;
    flock(t->fd, LOCK_UN);
    close(t->fd);
    return rc;
}

/* ---------- nullable integer columns ---------- */

static bool nget(const json_t *o, const char *k, int64_t *v) {
    json_t *x = json_get(o, k);
    if (!json_is_num(x)) return false;
    *v = x->is_int ? x->u.num.i : json_d2i(x->u.num.d);
    return true;
}

static json_t *jobs_of(tx_t *t, const char *kind) {
    json_t *jobs = json_get_obj(t->doc, "jobs");
    json_t *k = json_get_obj(jobs, kind);
    if (!k) {
        k = json_obj();
        json_set(jobs, kind, k);
    }
    return k;
}

static json_t *job_get(tx_t *t, const char *kind, const char *key) { return json_get_obj(jobs_of(t, kind), key); }

/* INSERT OR IGNORE INTO jobs (...) VALUES (kind, key, 'pending', 3, 0, 0) */
static json_t *ensure_job(tx_t *t, const char *kind, const char *key) {
    json_t *j = job_get(t, kind, key);
    if (j) return j;
    j = json_obj();
    json_set_str(j, "status", "pending");
    json_set(j, "worker_id", json_null());
    json_set(j, "ownership_token", json_null());
    json_set(j, "started_at", json_null());
    json_set(j, "finished_at", json_null());
    json_set(j, "lease_until", json_null());
    json_set(j, "retry_at", json_null());
    json_set_int(j, "retry_remaining", MEMSTORE_DEFAULT_RETRY_REMAINING);
    json_set(j, "last_error", json_null());
    json_set_int(j, "input_watermark", 0);
    json_set_int(j, "last_success_watermark", 0);
    json_set(jobs_of(t, kind), key, j);
    t->dirty = true;
    return j;
}

static bool lease_live(const json_t *j, int64_t now) {
    int64_t lu;
    return str_eq(json_get_str(j, "status"), "running") && nget(j, "lease_until", &lu) && lu > now;
}

static bool owned_running(const json_t *j, const char *token) {
    return j && str_eq(json_get_str(j, "status"), "running") && str_eq(json_get_str(j, "ownership_token"), token);
}

/* ---------- operations ---------- */

int memstore_check(const char *path, char **err) {
    tx_t t;
    if (tx_begin(&t, path) != 0) {
        if (err) *err = xasprintf("cannot open memory state %s: %s", path, strerror(errno));
        return -1;
    }
    if (tx_end(&t) != 0) {
        if (err) *err = xasprintf("cannot write memory state %s", path);
        return -1;
    }
    return 0;
}

int memstore_clear_cwd(const char *path, const char *cwd) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return -1;
    char *scope = memstore_normalize_cwd(cwd);
    json_t *threads = json_get_obj(t.doc, "threads");
    json_t *outs = json_get_obj(t.doc, "stage1_outputs");
    json_t *s1 = jobs_of(&t, MEMSTORE_STAGE1_KIND);
    strvec_t ids = {0};
    for (size_t i = 0; i < threads->u.obj.n; i++)
        if (str_eq(json_get_str(threads->u.obj.vals[i], "cwd"), scope)) strvec_push(&ids, threads->u.obj.keys[i]);
    for (size_t i = 0; i < ids.n; i++) {
        json_del(outs, ids.v[i]);
        json_del(s1, ids.v[i]);
        json_del(threads, ids.v[i]);
    }
    char *gk = global_job_key(cwd);
    json_del(jobs_of(&t, MEMSTORE_GLOBAL_KIND), gk);
    free(gk);
    strvec_free(&ids);
    free(scope);
    t.dirty = true;
    return tx_end(&t);
}

int memstore_upsert_threads(const char *path, const mem_thread_t *threads, size_t n) {
    if (n == 0) return 0;
    tx_t t;
    if (tx_begin(&t, path) != 0) return -1;
    json_t *tab = json_get_obj(t.doc, "threads");
    for (size_t i = 0; i < n; i++) {
        json_t *r = json_obj();
        json_set_int(r, "updated_at", threads[i].updated_at);
        json_set_str(r, "rollout_path", threads[i].rollout_path);
        char *c = memstore_normalize_cwd(threads[i].cwd);
        json_set_str(r, "cwd", c);
        free(c);
        json_set_str(r, "source_kind", threads[i].source_kind);
        json_set(tab, threads[i].id, r);
    }
    t.dirty = true;
    return tx_end(&t);
}

typedef struct {
    const char *id;
    const json_t *row;
} thread_ref_t;

static int cmp_thread_desc(const void *a, const void *b) {
    const thread_ref_t *x = a, *y = b;
    int64_t ux = json_get_int(x->row, "updated_at", 0), uy = json_get_int(y->row, "updated_at", 0);
    if (ux != uy) return ux > uy ? -1 : 1;
    return strcmp(x->id, y->id);
}

int memstore_claim_stage1(const char *path, const mem_claim_params_t *p, mem_claims_t *out) {
    memset(out, 0, sizeof *out);
    tx_t t;
    if (tx_begin(&t, path) != 0) return -1;
    int64_t now = p->now_sec;
    double max_age = p->max_rollout_age_days * 24 * 60 * 60;
    double min_idle = p->min_rollout_idle_hours * 60 * 60;
    json_t *s1 = jobs_of(&t, MEMSTORE_STAGE1_KIND);
    int64_t running = 0;
    for (size_t i = 0; i < s1->u.obj.n; i++)
        if (lease_live(s1->u.obj.vals[i], now)) running++;
    if (running >= p->running_concurrency_cap) return tx_end(&t);
    json_t *threads = json_get_obj(t.doc, "threads");
    json_t *outs = json_get_obj(t.doc, "stage1_outputs");
    size_t nt = threads->u.obj.n;
    thread_ref_t *refs = xcalloc(nt + 1, sizeof *refs);
    for (size_t i = 0; i < nt; i++) refs[i] = (thread_ref_t){threads->u.obj.keys[i], threads->u.obj.vals[i]};
    if (nt > 1) qsort(refs, nt, sizeof *refs, cmp_thread_desc);
    size_t limit = p->thread_scan_limit < 0 ? 0 : (size_t)p->thread_scan_limit;
    for (size_t i = 0; i < nt && i < limit; i++) {
        if ((int64_t)out->n >= p->max_rollouts_per_startup) break;
        const char *id = refs[i].id;
        const json_t *row = refs[i].row;
        if (p->exclude_thread_ids && strvec_contains(p->exclude_thread_ids, id)) continue;
        const char *kind = json_get_str(row, "source_kind");
        if (!str_eq(kind, "cli") && !str_eq(kind, "app")) continue;
        int64_t upd = json_get_int(row, "updated_at", 0);
        if ((double)(now - upd) > max_age) continue;
        if ((double)(now - upd) < min_idle) continue;
        if (running >= p->running_concurrency_cap) break;
        int64_t prev = 0;
        nget(json_get(outs, id), "source_updated_at", &prev);
        if (prev >= upd) continue;
        json_t *j = ensure_job(&t, MEMSTORE_STAGE1_KIND, id);
        /* UPDATE ... WHERE: not yet succeeded for this watermark, no live lease, new input or retry due */
        int64_t lsw = 0, iw = 0, ra = 0, rr = json_get_int(j, "retry_remaining", 0);
        bool has_lsw = nget(j, "last_success_watermark", &lsw), has_iw = nget(j, "input_watermark", &iw),
             has_ra = nget(j, "retry_at", &ra);
        if (has_lsw && !(lsw < upd)) continue;
        if (lease_live(j, now)) continue;
        if (!(!has_iw || iw < upd || (rr > 0 && (!has_ra || ra <= now)))) continue;
        char *token = uuid_v4();
        json_set_str(j, "status", "running");
        json_set_str(j, "worker_id", p->worker_id);
        json_set_str(j, "ownership_token", token);
        json_set_int(j, "started_at", now);
        json_set(j, "finished_at", json_null());
        json_set_int(j, "lease_until", now + p->lease_seconds);
        json_set(j, "retry_at", json_null());
        json_set(j, "last_error", json_null());
        json_set_int(j, "input_watermark", upd);
        if (!has_iw || iw < upd) json_set_int(j, "retry_remaining", MEMSTORE_DEFAULT_RETRY_REMAINING);
        t.dirty = true;
        mem_stage1_claim_t c = {xstrdup(id), token, upd, upd, xstrdup(json_get_str(row, "rollout_path")),
                                xstrdup(json_get_str(row, "cwd"))};
        vec_push(out, c);
        running++;
    }
    free(refs);
    return tx_end(&t);
}

static void enqueue_global_locked(tx_t *t, int64_t src, const char *cwd, bool force) {
    char *key = global_job_key(cwd);
    json_t *j = ensure_job(t, MEMSTORE_GLOBAL_KIND, key);
    free(key);
    int64_t iw = 0, lsw = 0;
    bool has_iw = nget(j, "input_watermark", &iw), has_lsw = nget(j, "last_success_watermark", &lsw);
    bool forced = force && (!has_lsw || (has_iw && iw <= lsw));
    if (!has_iw || iw < src) {
        json_set_int(j, "input_watermark", src);
        json_set_int(j, "retry_remaining", MEMSTORE_DEFAULT_RETRY_REMAINING);
        json_set(j, "retry_at", json_null());
    } else if (forced) {
        json_set_int(j, "input_watermark", has_lsw ? lsw + 1 : iw + 1);
        json_set_int(j, "retry_remaining", MEMSTORE_DEFAULT_RETRY_REMAINING);
        json_set(j, "retry_at", json_null());
    }
    t->dirty = true;
}

int memstore_enqueue_global(const char *path, int64_t source_updated_at, const char *cwd, bool force_dirty) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return -1;
    enqueue_global_locked(&t, source_updated_at, cwd, force_dirty);
    return tx_end(&t);
}

static void mark_done(json_t *j, int64_t now) {
    json_set_str(j, "status", "done");
    json_set_int(j, "finished_at", now);
    json_set(j, "lease_until", json_null());
    json_set(j, "retry_at", json_null());
    json_set(j, "last_error", json_null());
    json_t *iw = json_get(j, "input_watermark");
    json_set(j, "last_success_watermark", iw ? json_clone(iw) : json_null());
}

bool memstore_stage1_succeeded(const char *path, const char *thread_id, const char *token, int64_t source_updated_at,
                               const char *raw_memory, const char *rollout_summary, const char *rollout_slug,
                               int64_t now_sec, const char *cwd) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return false;
    json_t *j = job_get(&t, MEMSTORE_STAGE1_KIND, thread_id);
    bool ok = owned_running(j, token);
    if (ok) {
        mark_done(j, now_sec);
        json_t *outs = json_get_obj(t.doc, "stage1_outputs");
        json_t *prev = json_get_obj(outs, thread_id);
        int64_t pv;
        if (!prev || !nget(prev, "source_updated_at", &pv) || source_updated_at >= pv) {
            json_t *r = json_obj();
            json_set_int(r, "source_updated_at", source_updated_at);
            json_set_str(r, "raw_memory", raw_memory);
            json_set_str(r, "rollout_summary", rollout_summary);
            if (rollout_slug) json_set_str(r, "rollout_slug", rollout_slug);
            else json_set(r, "rollout_slug", json_null());
            json_set_int(r, "generated_at", now_sec);
            json_set(outs, thread_id, r);
        }
        enqueue_global_locked(&t, source_updated_at, cwd, true);
        t.dirty = true;
    }
    tx_end(&t);
    return ok;
}

bool memstore_stage1_no_output(const char *path, const char *thread_id, const char *token, int64_t source_updated_at,
                               int64_t now_sec, const char *cwd) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return false;
    json_t *j = job_get(&t, MEMSTORE_STAGE1_KIND, thread_id);
    bool ok = owned_running(j, token);
    if (ok) {
        mark_done(j, now_sec);
        json_del(json_get_obj(t.doc, "stage1_outputs"), thread_id);
        enqueue_global_locked(&t, source_updated_at, cwd, true);
        t.dirty = true;
    }
    tx_end(&t);
    return ok;
}

static void mark_error(json_t *j, int64_t now, int64_t delay, const char *reason) {
    json_set_str(j, "status", "error");
    json_set_int(j, "finished_at", now);
    json_set(j, "lease_until", json_null());
    json_set_int(j, "retry_at", now + delay);
    int64_t rr = json_get_int(j, "retry_remaining", 0);
    json_set_int(j, "retry_remaining", rr > 0 ? rr - 1 : 0);
    json_set_str(j, "last_error", reason ? reason : "");
}

bool memstore_stage1_failed(const char *path, const char *thread_id, const char *token, int64_t retry_delay,
                            const char *reason, int64_t now_sec) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return false;
    json_t *j = job_get(&t, MEMSTORE_STAGE1_KIND, thread_id);
    bool ok = owned_running(j, token);
    if (ok) {
        mark_error(j, now_sec, retry_delay, reason);
        t.dirty = true;
    }
    tx_end(&t);
    return ok;
}

int memstore_claim_global(const char *path, const char *worker_id, int64_t lease_seconds, int64_t now_sec,
                          const char *cwd, char **token, int64_t *input_watermark) {
    *token = NULL;
    *input_watermark = 0;
    tx_t t;
    if (tx_begin(&t, path) != 0) return -1;
    char *key = global_job_key(cwd);
    json_t *j = ensure_job(&t, MEMSTORE_GLOBAL_KIND, key);
    free(key);
    int64_t iw = 0, lsw = 0, ra = 0, rr = json_get_int(j, "retry_remaining", 0);
    bool has_iw = nget(j, "input_watermark", &iw), has_lsw = nget(j, "last_success_watermark", &lsw),
         has_ra = nget(j, "retry_at", &ra);
    int rc;
    if (!lease_live(j, now_sec) && has_iw && (!has_lsw || iw > lsw) && rr > 0 && (!has_ra || ra <= now_sec)) {
        char *tok = uuid_v4();
        json_set_str(j, "status", "running");
        json_set_str(j, "worker_id", worker_id);
        json_set_str(j, "ownership_token", tok);
        json_set_int(j, "started_at", now_sec);
        json_set(j, "finished_at", json_null());
        json_set_int(j, "lease_until", now_sec + lease_seconds);
        json_set(j, "retry_at", json_null());
        json_set(j, "last_error", json_null());
        t.dirty = true;
        *token = tok;
        *input_watermark = iw;
        rc = MEMSTORE_CLAIMED;
    } else {
        rc = lease_live(j, now_sec) ? MEMSTORE_SKIPPED_RUNNING : MEMSTORE_SKIPPED_NOT_DIRTY;
    }
    if (tx_end(&t) != 0 && rc == MEMSTORE_CLAIMED) {
        free(*token);
        *token = NULL;
        return -1;
    }
    return rc;
}

bool memstore_heartbeat_global(const char *path, const char *token, int64_t lease_seconds, int64_t now_sec,
                               const char *cwd) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return false;
    char *key = global_job_key(cwd);
    json_t *j = job_get(&t, MEMSTORE_GLOBAL_KIND, key);
    free(key);
    bool ok = owned_running(j, token);
    if (ok) {
        json_set_int(j, "lease_until", now_sec + lease_seconds);
        t.dirty = true;
    }
    tx_end(&t);
    return ok;
}

static bool blank(const char *s) {
    if (!s) return true;
    char *x = str_trim(s);
    bool b = !*x;
    free(x);
    return b;
}

static int cmp_output_desc(const void *a, const void *b) {
    const mem_output_t *x = a, *y = b;
    if (x->source_updated_at != y->source_updated_at) return x->source_updated_at > y->source_updated_at ? -1 : 1;
    return strcmp(x->thread_id, y->thread_id);
}

int memstore_list_outputs(const char *path, int64_t limit, const char *cwd, mem_outputs_t *out) {
    memset(out, 0, sizeof *out);
    tx_t t;
    if (tx_begin(&t, path) != 0) return -1;
    char *scope = memstore_normalize_cwd(cwd);
    json_t *outs = json_get_obj(t.doc, "stage1_outputs");
    json_t *threads = json_get_obj(t.doc, "threads");
    for (size_t i = 0; i < outs->u.obj.n; i++) {
        const char *id = outs->u.obj.keys[i];
        const json_t *o = outs->u.obj.vals[i];
        const json_t *th = json_get_obj(threads, id);
        if (!th || !str_eq(json_get_str(th, "cwd"), scope)) continue;
        if (blank(json_get_str(o, "raw_memory")) && blank(json_get_str(o, "rollout_summary"))) continue;
        const char *slug = json_get_str(o, "rollout_slug");
        mem_output_t r = {xstrdup(id),
                          json_get_int(o, "source_updated_at", 0),
                          xstrdup(json_get_str(o, "raw_memory") ? json_get_str(o, "raw_memory") : ""),
                          xstrdup(json_get_str(o, "rollout_summary") ? json_get_str(o, "rollout_summary") : ""),
                          slug ? xstrdup(slug) : NULL,
                          json_get_int(o, "generated_at", 0),
                          xstrdup(json_get_str(th, "cwd"))};
        vec_push(out, r);
    }
    free(scope);
    if (out->n > 1) qsort(out->v, out->n, sizeof *out->v, cmp_output_desc);
    if (limit >= 0 && (int64_t)out->n > limit) {
        mem_outputs_t keep = *out;
        for (size_t i = (size_t)limit; i < keep.n; i++) {
            mem_output_t *r = &keep.v[i];
            free(r->thread_id);
            free(r->raw_memory);
            free(r->rollout_summary);
            free(r->rollout_slug);
            free(r->cwd);
        }
        out->n = (size_t)limit;
    }
    return tx_end(&t);
}

bool memstore_global_succeeded(const char *path, const char *token, int64_t new_watermark, int64_t now_sec,
                               const char *cwd) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return false;
    char *key = global_job_key(cwd);
    json_t *j = job_get(&t, MEMSTORE_GLOBAL_KIND, key);
    free(key);
    bool ok = owned_running(j, token);
    if (ok) {
        json_set_str(j, "status", "done");
        json_set_int(j, "finished_at", now_sec);
        json_set(j, "lease_until", json_null());
        json_set(j, "retry_at", json_null());
        json_set(j, "last_error", json_null());
        int64_t lsw;
        if (!nget(j, "last_success_watermark", &lsw) || lsw < new_watermark)
            json_set_int(j, "last_success_watermark", new_watermark);
        t.dirty = true;
    }
    tx_end(&t);
    return ok;
}

bool memstore_global_failed(const char *path, const char *token, int64_t retry_delay, const char *reason,
                            int64_t now_sec, const char *cwd) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return false;
    char *key = global_job_key(cwd);
    json_t *j = job_get(&t, MEMSTORE_GLOBAL_KIND, key);
    free(key);
    bool ok = owned_running(j, token);
    if (ok) {
        mark_error(j, now_sec, retry_delay, reason);
        t.dirty = true;
    }
    tx_end(&t);
    return ok;
}

bool memstore_global_failed_unowned(const char *path, int64_t retry_delay, const char *reason, int64_t now_sec,
                                    const char *cwd) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return false;
    char *key = global_job_key(cwd);
    json_t *j = job_get(&t, MEMSTORE_GLOBAL_KIND, key);
    free(key);
    int64_t lu;
    bool ok = j && str_eq(json_get_str(j, "status"), "running") &&
              (!json_get_str(j, "ownership_token") || !nget(j, "lease_until", &lu) || lu <= now_sec);
    if (ok) {
        mark_error(j, now_sec, retry_delay, reason);
        t.dirty = true;
    }
    tx_end(&t);
    return ok;
}

json_t *memstore_job(const char *path, const char *kind, const char *key) {
    tx_t t;
    if (tx_begin(&t, path) != 0) return NULL;
    json_t *j = job_get(&t, kind, key);
    json_t *r = j ? json_clone(j) : NULL;
    tx_end(&t);
    return r;
}

void mem_claims_free(mem_claims_t *c) {
    for (size_t i = 0; i < c->n; i++) {
        free(c->v[i].thread_id);
        free(c->v[i].ownership_token);
        free(c->v[i].rollout_path);
        free(c->v[i].cwd);
    }
    vec_free(c);
}

void mem_outputs_free(mem_outputs_t *o) {
    for (size_t i = 0; i < o->n; i++) {
        free(o->v[i].thread_id);
        free(o->v[i].raw_memory);
        free(o->v[i].rollout_summary);
        free(o->v[i].rollout_slug);
        free(o->v[i].cwd);
    }
    vec_free(o);
}
