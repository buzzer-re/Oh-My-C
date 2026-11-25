/* rpc mode: JSONL commands on stdin, responses + agent events on stdout (omp RPC protocol subset). */
#include "modes.h"
#include "coding/compaction.h"
#include "coding/agents.h"
#include "coding/modelctl.h"
#include "coding/btw.h"
#include "coding/goal.h"
#include "coding/slashcmd.h"
#include "coding/turn_recovery.h"
#include "session/export_html.h"

#include "ai/toolschema.h"
#include "config/auth.h"
#include "config/settings.h"
#include "core/proc.h"
#include "core/fs.h"
#include "tools/approval.h"
#include "tools/artifacts.h"
#include "tools/jobs.h"
#include "text/htmlform.h"
#include "tui/fuzzy.h"
#include "ui/modelbrowser.h"
#include "tui/ansi.h"
#include "tui/symbols.h"
#include "ui/ui.h"

#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

typedef struct {
    char *id;
    json_t *result;
    bool is_error;
    bool done;
    tool_ctx_t *ctx; /* the waiting tool call (host_tool_update streams into it) */
} hostcall_t;

struct rpc {
    app_t *a;
    rpc_sink_fn sink; /* NULL => stdout */
    void *sink_ud;
    /* event filter: allowed event types; !filter_on => all */
    pthread_mutex_t filter_mu;
    strvec_t filter;
    bool filter_on;
    /* host tools implemented by the client */
    pthread_mutex_t host_mu;
    pthread_cond_t host_cv;
    VEC(hostcall_t *) host_calls;
    strvec_t host_tools;
    uint64_t host_seq;
    pthread_t th;
    bool running;
    bool settling;   /* the run's agent loop ended (agent_end sent): its thread only finishes and reports the run */
    bool compacting; /* a manual compaction is replacing history */
    char *prompt_id;
    char *text;
    json_t *images;
    const char *hidden; /* custom type when the run is a hidden wake-up (async-result) */
    bool slash;         /* the prompt is a builtin slash command (executeAcpBuiltinSlashCommand) */
    bool slash_reply;   /* its `prompt` response is owed by the prompt thread, once the builtin ran */
    /* session.abort(): the `abort` reply waits for the run's agent_end; later serial commands and the aborted
     * prompt's prompt_result wait for that reply */
    pthread_cond_t abort_cv;
    int abort_waiters;
    unsigned agent_ends;
    json_t *skill_msg;  /* the prompt invokes a skill (dispatchRpcSkillPrompt): its built custom message */
    bool resume;        /* the run is a `resume` (AgentSession.resume): no prompt ticket */
    pthread_mutex_t mu;
    /* /btw (rpc-btw.ts) */
    pthread_mutex_t btw_mu;
    btw_store_t *btw_store;
    char *btw_store_key;          /* session id \x01 artifacts dir the store was opened for */
    struct rpc_btw_run *btw_running;
    bool btw_starting;            /* a `btw` between validation and its first checkpoint */
    char *btw_starting_topic;     /* its recordId (NULL: a new topic) */
    int btw_epoch;                /* bumped by every cancel and session change */
    json_t *btw_unsaved;          /* id -> record whose terminal checkpoint failed */
    pthread_t btw_th;
    bool btw_th_live;
    /* output framing (rpc-frame.ts RpcFrameEncoder) */
    pthread_mutex_t out_mu;
    int proto;                 /* 1, or 2 once negotiate_protocol succeeded */
    unsigned chunk_seq;
    strvec_t streamed;         /* JSON of each message_end message since agent_start (agent_end compaction) */
    /* session event forwarder (rpc-session-events.ts): message ids, delta projection */
    bool delta;
    uint64_t msg_seq;
    strvec_t open_ids;
    /* extension UI dialogs (extension_ui_request / extension_ui_response) */
    pthread_mutex_t ui_mu;
    pthread_cond_t ui_cv;
    VEC(struct rpc_dialog *) dialogs;
    bool ask_dialog;           /* set_ask_dialog: the ask tool sends one `ask` request */
    bool closed;               /* stdin closed: every pending and future dialog fails */
    /* RPC `bash` runs off the command reader so `abort_bash` can stop it */
    pthread_mutex_t bash_mu;
    volatile sig_atomic_t bash_cancel;
    int bash_running;
    /* commands answered off the reader thread (bash, login): joined on detach */
    pthread_mutex_t bg_mu;
    VEC(pthread_t) bg_threads;
    volatile int login_abort;
    json_t *waiting;           /* ids (or null) of prompts queued into a live run: prompt_result owed at its yield */
    char *open_dir;            /* open_session: the session dir a fresh session was opened in (opts.session_dir) */
    /* subagent frames (RpcSubagentRegistry): running snapshots, retained transcripts, stale ids, subscription */
    pthread_mutex_t sub_mu;
    json_t *subs;      /* id -> RpcSubagentSnapshot of a running subagent */
    json_t *sub_files; /* id -> transcript file (newest RPC_SUB_RETAINED) */
    json_t *sub_stale; /* ids from before the last session change */
    int sub_level;     /* 0 off, 1 progress, 2 events */
};

/* ---------- output framing (port of modes/rpc/rpc-frame.ts) ---------- */

#define RPC_MAX_FRAME_BYTES (1024 * 1024)
#define RPC_MAX_REASSEMBLED_BYTES (64 * 1024 * 1024)
#define RPC_CHUNK_PAYLOAD_BYTES (256 * 1024)

/* RpcOutputWriter's failure: stdout cannot take a frame any more (a closed or broken pipe). Later frames are dropped
 * and run_rpc_mode ends the session and exits 1, as omp's onFailure does (session.dispose, then process.exit(1)) */
static volatile sig_atomic_t g_out_failed;

static void write_out(const char *s, size_t n) {
    while (n && !g_out_failed) {
        ssize_t w = write(1, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { /* a non-blocking stdout: wait until it drains */
            struct pollfd pfd = {.fd = 1, .events = POLLOUT};
            poll(&pfd, 1, 1000);
            continue;
        }
        if (w <= 0) {
            int e = w < 0 ? errno : EPIPE;
            g_out_failed = 1;
            LOGE("RPC output delivery failed: Error: %s", strerror(e));
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static char *dump_js(const json_t *j, size_t *len) {
    sb_t b = SB_INIT;
    json_dump_js_sb(&b, j);
    if (len) *len = b.len;
    return sb_detach(&b);
}

/* JS string length (UTF-16 code units) and the byte length of its first `units` units */
static size_t utf16_len(const char *s, size_t n) {
    size_t u = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80) continue;
        u += c >= 0xF0 ? 2 : 1;
    }
    return u;
}

static size_t utf16_prefix(const char *s, size_t n, size_t units) {
    size_t u = 0, i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t w = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        size_t cu = c >= 0xF0 ? 2 : 1;
        if (u + cu > units) break;
        u += cu;
        i += w;
    }
    return i > n ? n : i;
}

typedef struct {
    size_t string_cap, array_limit, object_limit;
} shrink_pass_t;

static const shrink_pass_t SHRINK_PASSES[] = {
    {256 * 1024, 512, 512}, {64 * 1024, 256, 256}, {16 * 1024, 128, 128}, {4 * 1024, 64, 64},
    {1024, 32, 32},         {256, 8, 16},          {64, 1, 8},
};

/* shrinkString: head + "\n…[N chars elided for RPC frame]" */
static json_t *shrink_string(const char *s, size_t n, size_t cap) {
    size_t len = utf16_len(s, n);
    if (len <= cap) return json_strn(s, n);
    size_t head = cap > 80 ? cap - 80 : 0;
    size_t hb = utf16_prefix(s, n, head);
    sb_t b = SB_INIT;
    sb_appendn(&b, s, hb);
    sb_appendf(&b, "\n\xe2\x80\xa6[%zu chars elided for RPC frame]", len - head);
    json_t *r = json_strn(b.s, b.len);
    sb_free(&b);
    return r;
}

static json_t *shrink_value(const json_t *v, const shrink_pass_t *p, int depth) {
    if (json_is_str(v)) return shrink_string(v->u.str.s, v->u.str.len, p->string_cap);
    if (depth > 256) return json_null();
    if (json_is_arr(v)) {
        size_t n = json_len(v), keep = MIN(n, p->array_limit);
        json_t *o = json_arr();
        for (size_t i = 0; i < keep; i++) json_push(o, shrink_value(json_at(v, i), p, depth + 1));
        if (keep < n) {
            char *m = xasprintf("\xe2\x80\xa6[%zu items elided for RPC frame]", n - keep);
            json_push_str(o, m);
            free(m);
        }
        return o;
    }
    if (json_is_obj(v)) {
        size_t n = json_len(v), keep = MIN(n, p->object_limit);
        json_t *o = json_obj();
        for (size_t i = 0; i < keep; i++) json_set(o, v->u.obj.keys[i], shrink_value(v->u.obj.vals[i], p, depth + 1));
        if (keep < n) json_set_int(o, "rpcFrameElidedKeys", (int64_t)(n - keep));
        return o;
    }
    return json_clone(v);
}

static json_t *overflow_frame(const json_t *f) {
    json_t *o = json_obj();
    const char *type = json_get_str(f, "type");
    if (str_eq(type, "response")) {
        const char *id = json_get_str(f, "id"), *cmd = json_get_str(f, "command");
        if (id) json_set(o, "id", shrink_string(id, strlen(id), 1024));
        json_set_str(o, "type", "response");
        if (cmd) json_set(o, "command", shrink_string(cmd, strlen(cmd), 1024));
        else json_set_str(o, "command", "unknown");
        json_set_bool(o, "success", false);
        json_set_str(o, "error", "RPC response exceeded the transport limit");
    } else if (str_eq(type, "agent_end")) {
        json_set_str(o, "type", "agent_end");
        json_set(o, "messages", json_arr());
        json_set_int(o, "messageCount", json_get_int(f, "messageCount", 0));
    } else {
        json_set_str(o, "type", "rpc_frame_error");
        if (type) json_set(o, "originalType", shrink_string(type, strlen(type), 1024));
        json_set_str(o, "error", "RPC frame exceeded the transport limit");
    }
    return o;
}

/* compactTerminalFrame: an agent_end drops the messages the client already received as message_end frames */
static json_t *compact_terminal(rpc_t *r, const json_t *f) {
    json_t *msgs = json_get_arr(f, "messages");
    if (!str_eq(json_get_str(f, "type"), "agent_end") || !msgs) return NULL;
    size_t streamed = 0, limit = MIN(r->streamed.n, json_len(msgs));
    while (streamed < limit) {
        char *d = dump_js(json_at(msgs, streamed), NULL);
        bool same = !strcmp(d, r->streamed.v[streamed]);
        free(d);
        if (!same) break;
        streamed++;
    }
    json_t *o = json_obj();
    for (size_t i = 0; i < json_len(f); i++) {
        if (!strcmp(f->u.obj.keys[i], "messages")) {
            json_t *rest = json_arr();
            for (size_t k = streamed; k < json_len(msgs); k++) json_push(rest, json_clone(json_at(msgs, k)));
            json_set(o, "messages", rest);
        } else json_set(o, f->u.obj.keys[i], json_clone(f->u.obj.vals[i]));
    }
    json_set_int(o, "messageCount", (int64_t)json_len(msgs));
    return o;
}

static void write_line_locked(const char *s, size_t n) {
    char *line = xmalloc(n + 1);
    memcpy(line, s, n);
    line[n] = '\n';
    write_out(line, n + 1);
    free(line);
}

/* encodeChunkedRpcFrames: protocol v2 frames over the line limit go out as base64 rpc_chunk lines */
static void write_chunks_locked(rpc_t *r, const json_t *f, const char *json, size_t len) {
    if (len > RPC_MAX_REASSEMBLED_BYTES) {
        json_t *o = overflow_frame(f);
        size_t n;
        char *d = dump_js(o, &n);
        write_line_locked(d, n);
        free(d);
        json_free(o);
        return;
    }
    char *cid = xasprintf("rpc-%u", ++r->chunk_seq);
    size_t count = (len + RPC_CHUNK_PAYLOAD_BYTES - 1) / RPC_CHUNK_PAYLOAD_BYTES;
    for (size_t i = 0; i < count; i++) {
        size_t off = i * RPC_CHUNK_PAYLOAD_BYTES, n = MIN((size_t)RPC_CHUNK_PAYLOAD_BYTES, len - off);
        char *b64 = base64_encode(json + off, n);
        char *line = xasprintf("{\"type\":\"rpc_chunk\",\"chunkId\":\"%s\",\"index\":%zu,\"count\":%zu,\"byteLength\":%zu,"
                               "\"data\":\"%s\"}",
                               cid, i, count, len, b64);
        write_line_locked(line, strlen(line));
        free(line);
        free(b64);
    }
    free(cid);
}

/* RpcFrameEncoder.encodeFrames: one logical frame as JSONL under the transport limits; returns the JSON of the
 * frame as written (owned; a compacted or shrunk frame when it had to shrink) */
static char *encode_locked(rpc_t *r, const json_t *f, char *json, size_t len) {
    if (len + 1 <= RPC_MAX_FRAME_BYTES) {
        write_line_locked(json, len);
        return json;
    }
    bool response = str_eq(json_get_str(f, "type"), "response");
    if (r->proto == 2) {
        json_t *c = compact_terminal(r, f);
        if (c) {
            free(json);
            json = dump_js(c, &len);
        }
        if (len + 1 <= RPC_MAX_FRAME_BYTES) write_line_locked(json, len);
        else write_chunks_locked(r, c ? c : f, json, len);
        json_free(c);
        return json;
    }
    json_t *o = NULL;
    if (!response) {
        json_t *c = compact_terminal(r, f);
        const json_t *base = c ? c : f;
        free(json);
        json = dump_js(base, &len);
        for (size_t p = 0; len + 1 > RPC_MAX_FRAME_BYTES && p < ARRAY_LEN(SHRINK_PASSES); p++) {
            json_t *sh = shrink_value(base, &SHRINK_PASSES[p], 0);
            free(json);
            json = dump_js(sh, &len);
            json_free(sh);
        }
        if (len + 1 > RPC_MAX_FRAME_BYTES) o = overflow_frame(base);
        json_free(c);
    } else o = overflow_frame(f);
    if (o) {
        free(json);
        json = dump_js(o, &len);
        json_free(o);
    }
    write_line_locked(json, len);
    return json;
}

static void emit(rpc_t *r, const json_t *j) {
    if (r && r->sink) {
        r->sink(r->sink_ud, j);
        return;
    }
    size_t len;
    char *json = dump_js(j, &len);
    const char *type = json_get_str(j, "type");
    pthread_mutex_lock(&r->out_mu);
    if (str_eq(type, "agent_start")) strvec_free(&r->streamed);
    json = encode_locked(r, j, json, len);
    if (str_eq(type, "message_end") && json_get(j, "message")) {
        /* the message as the client received it */
        json_t *sent = json_parse_str(json);
        json_t *m = json_get(sent, "message");
        strvec_push_own(&r->streamed, m ? dump_js(m, NULL) : dump_js(json_get(j, "message"), NULL));
        json_free(sent);
    } else if (str_eq(type, "agent_end") && !json_get_bool(j, "willContinue", false)) strvec_free(&r->streamed);
    pthread_mutex_unlock(&r->out_mu);
    free(json);
}

static void respond_code(rpc_t *rp, const char *id, const char *cmd, bool ok, json_t *data, const char *err,
                         const char *code) {
    json_t *r = json_obj();
    if (id) json_set_str(r, "id", id);
    json_set_str(r, "type", "response");
    json_set_str(r, "command", cmd ? cmd : "unknown");
    json_set_bool(r, "success", ok);
    if (data) json_set(r, "data", data);
    if (err) json_set_str(r, "error", err);
    if (code) json_set_str(r, "code", code);
    emit(rp, r);
    json_free(r);
}

static void respond(rpc_t *rp, const char *id, const char *cmd, bool ok, json_t *data, const char *err) {
    respond_code(rp, id, cmd, ok, data, err, NULL);
}

/* ---------- session events (port of modes/rpc/rpc-session-events.ts) ---------- */

/* openedBlock: the shell of the block a *_start opens (its streamed content emptied) */
static json_t *opened_block(const json_t *b) {
    if (!b) return NULL;
    json_t *o = json_clone(b);
    const char *t = json_get_str(b, "type");
    if (str_eq(t, "text")) json_set_str(o, "text", "");
    else if (str_eq(t, "thinking")) json_set_str(o, "thinking", "");
    else if (str_eq(t, "toolCall")) json_set(o, "arguments", json_obj());
    return o;
}

/* toDelta: drops the accumulated snapshot a streaming event carries, keeping what a client needs to rebuild it */
static json_t *to_delta(const json_t *ev) {
    const char *t = json_get_str(ev, "type");
    const json_t *partial = json_get(ev, "partial");
    json_t *o = json_obj();
    for (size_t i = 0; i < json_len(ev); i++)
        if (strcmp(ev->u.obj.keys[i], "partial")) json_set(o, ev->u.obj.keys[i], json_clone(ev->u.obj.vals[i]));
    if (str_eq(t, "done") || str_eq(t, "error") || !partial) return o;
    if (str_eq(t, "start")) {
        json_set(o, "message", json_clone(partial));
        return o;
    }
    if (str_eq(t, "text_delta") || str_eq(t, "thinking_delta") || str_eq(t, "toolcall_delta") || str_eq(t, "image_end") ||
        str_eq(t, "toolcall_end"))
        return o;
    const json_t *blk = json_at(json_get(partial, "content"), (size_t)json_get_int(ev, "contentIndex", -1));
    if (str_eq(t, "text_end") || str_eq(t, "thinking_end")) {
        const char *want = str_eq(t, "text_end") ? "text" : "thinking";
        if (str_eq(json_get_str(blk, "type"), want)) {
            json_t *meta = json_clone(blk);
            json_del(meta, want);
            json_set(o, "block", meta);
        }
        return o;
    }
    json_t *b = opened_block(blk);
    if (b) json_set(o, "block", b);
    return o;
}

/* RpcSessionEventForwarder.forward: stamps message lifecycle frames with a messageId (assigned whether or not
 * the frame passes the filter), applies set_event_filter and the "delta" projection. Takes `frame`. */
static void forward(rpc_t *r, json_t *frame) {
    const char *type = json_get_str(frame, "type");
    pthread_mutex_lock(&r->filter_mu);
    char *mid = NULL;
    if (str_eq(type, "message_start")) {
        mid = xasprintf("msg-%llu", (unsigned long long)++r->msg_seq);
        strvec_push(&r->open_ids, mid);
    } else if (str_eq(type, "message_update")) {
        if (r->open_ids.n) mid = xstrdup(r->open_ids.v[r->open_ids.n - 1]);
        else {
            mid = xasprintf("msg-%llu", (unsigned long long)++r->msg_seq);
            strvec_push(&r->open_ids, mid);
        }
    } else if (str_eq(type, "message_end")) {
        if (r->open_ids.n) {
            mid = r->open_ids.v[--r->open_ids.n];
            r->open_ids.v[r->open_ids.n] = NULL;
        } else mid = xasprintf("msg-%llu", (unsigned long long)++r->msg_seq);
    } else if (str_eq(type, "agent_end")) {
        strvec_free(&r->open_ids); /* a run never leaves a message open */
    }
    bool allowed = !r->filter_on || strvec_contains(&r->filter, type ? type : "");
    bool delta = r->delta;
    pthread_mutex_unlock(&r->filter_mu);
    if (mid) {
        json_set_str(frame, "messageId", mid);
        free(mid);
    }
    if (!allowed) {
        json_free(frame);
        return;
    }
    if (delta && str_eq(type, "message_update")) {
        json_t *ae = json_get(frame, "assistantMessageEvent");
        if (ae) json_set(frame, "assistantMessageEvent", to_delta(ae));
        json_t *role = json_obj();
        json_set_str(role, "role", json_get_str(json_get(frame, "message"), "role"));
        json_set(frame, "message", role);
    }
    emit(r, frame);
    json_free(frame);
}

static void session_event_rpc(void *ud, const json_t *ev) {
    forward(ud, json_clone(ev));
}

/* ---------- extension UI over RPC (RpcExtensionUIContext, requestRpcDialog) ---------- */

typedef struct rpc_dialog {
    char *id;
    json_t *response; /* the extension_ui_response, once answered */
    bool done;
} rpc_dialog_t;

/* fire-and-forget request (notify, setStatus, set_editor_text, open_url, cancel): {type, id, ...fields} */
static void ui_fire(rpc_t *r, json_t *fields) {
    json_t *f = json_obj();
    json_set_str(f, "type", "extension_ui_request");
    char *id = snowflake_next();
    json_set_str(f, "id", id);
    free(id);
    for (size_t i = 0; i < json_len(fields); i++) json_set(f, fields->u.obj.keys[i], json_clone(fields->u.obj.vals[i]));
    json_free(fields);
    emit(r, f);
    json_free(f);
}

static void ui_cancel(rpc_t *r, const char *target) {
    json_t *c = json_obj();
    json_set_str(c, "method", "cancel");
    json_set_str(c, "targetId", target);
    ui_fire(r, c);
}

/* requestRpcDialog: send {type, id, ...fields} and wait for the client's answer. `timeout_ms` > 0 closes the dialog
 * at the deadline (*timed_out); `cancel` (the tool's abort) closes it too. Returns 0 with *resp (owned) when
 * answered, 1 when closed by timeout or abort (a `cancel` request tells the client), -1 when the client is gone. */
static int ui_request(rpc_t *r, json_t *fields, int64_t timeout_ms, volatile sig_atomic_t *cancel, json_t **resp,
                      bool *timed_out) {
    *resp = NULL;
    if (timed_out) *timed_out = false;
    if (cancel && *cancel) {
        json_free(fields);
        return 1;
    }
    rpc_dialog_t *d = xcalloc(1, sizeof *d);
    d->id = snowflake_next();
    json_t *f = json_obj();
    json_set_str(f, "type", "extension_ui_request");
    json_set_str(f, "id", d->id);
    for (size_t i = 0; i < json_len(fields); i++) json_set(f, fields->u.obj.keys[i], json_clone(fields->u.obj.vals[i]));
    json_free(fields);
    pthread_mutex_lock(&r->ui_mu);
    bool closed = r->closed;
    if (!closed) vec_push(&r->dialogs, d);
    pthread_mutex_unlock(&r->ui_mu);
    if (closed) {
        json_free(f);
        free(d->id);
        free(d);
        return -1;
    }
    emit(r, f);
    json_free(f);
    int64_t deadline = timeout_ms > 0 ? mono_ms() + timeout_ms : 0;
    int rc = 0;
    pthread_mutex_lock(&r->ui_mu);
    while (!d->done) {
        if (r->closed) {
            rc = -1;
            break;
        }
        if (cancel && *cancel) {
            rc = 1;
            break;
        }
        if (deadline && mono_ms() >= deadline) {
            if (timed_out) *timed_out = true;
            rc = 1;
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&r->ui_cv, &r->ui_mu, &ts);
    }
    for (size_t i = 0; i < r->dialogs.n; i++)
        if (r->dialogs.v[i] == d) {
            r->dialogs.v[i] = r->dialogs.v[--r->dialogs.n];
            break;
        }
    pthread_mutex_unlock(&r->ui_mu);
    if (rc == 1) ui_cancel(r, d->id);
    *resp = d->response;
    free(d->id);
    free(d);
    return rc;
}

static void ui_answer(rpc_t *r, const json_t *frame) {
    const char *id = json_get_str(frame, "id");
    pthread_mutex_lock(&r->ui_mu);
    for (size_t i = 0; i < r->dialogs.n; i++)
        if (str_eq(r->dialogs.v[i]->id, id) && !r->dialogs.v[i]->done) {
            r->dialogs.v[i]->response = json_clone(frame);
            r->dialogs.v[i]->done = true;
        }
    pthread_cond_broadcast(&r->ui_cv);
    pthread_mutex_unlock(&r->ui_mu);
}

/* select: the chosen label (owned) or NULL (cancelled, timed out, client gone) */
static char *ui_select(rpc_t *r, const char *title, const strvec_t *labels, const json_t *details,
                       const tool_select_opts_t *so, int64_t timeout_ms, volatile sig_atomic_t *cancel, bool *timed_out,
                       bool *gone) {
    json_t *f = json_obj(), *opts = json_arr();
    json_set_str(f, "method", "select");
    json_set_str(f, "title", title);
    for (size_t i = 0; i < labels->n; i++) json_push_str(opts, labels->v[i]);
    json_set(f, "options", opts);
    if (details) json_set(f, "optionDetails", json_clone(details));
    if (so) { /* requestRpcSelect: the caller's initialIndex / selectionMarker / checkedIndices / markableCount */
        if (so->initial_index >= 0) json_set_int(f, "initialIndex", so->initial_index);
        if (so->marker) json_set_str(f, "selectionMarker", so->marker);
        if (so->checked) {
            json_t *ci = json_arr();
            for (size_t i = 0; i < so->nchecked; i++) json_push(ci, json_int(so->checked[i]));
            json_set(f, "checkedIndices", ci);
        }
        if (so->markable >= 0) json_set_int(f, "markableCount", so->markable);
    }
    if (timeout_ms > 0) json_set_int(f, "timeout", timeout_ms);
    json_t *resp = NULL;
    int rc = ui_request(r, f, timeout_ms, cancel, &resp, timed_out);
    if (gone) *gone = rc < 0;
    char *v = NULL;
    if (rc == 0 && !json_get_bool(resp, "cancelled", false) && json_get_str(resp, "value")) v = xstrdup(json_get_str(resp, "value"));
    if (rc == 0 && json_get_bool(resp, "cancelled", false) && json_get_bool(resp, "timedOut", false) && timed_out)
        *timed_out = true;
    json_free(resp);
    return v;
}

/* editor (promptStyle): the text (owned) or NULL */
static char *ui_editor(rpc_t *r, const char *title, volatile sig_atomic_t *cancel, bool *gone) {
    json_t *f = json_obj();
    json_set_str(f, "method", "editor");
    json_set_str(f, "title", title);
    json_set_bool(f, "promptStyle", true);
    json_t *resp = NULL;
    int rc = ui_request(r, f, 0, cancel, &resp, NULL);
    if (gone) *gone = rc < 0;
    char *v = NULL;
    if (rc == 0 && !json_get_bool(resp, "cancelled", false) && json_get_str(resp, "value")) v = xstrdup(json_get_str(resp, "value"));
    json_free(resp);
    return v;
}

#define ASK_OTHER "Other (type your own)"
#define ASK_RECOMMENDED " (Recommended)"

static int64_t ask_timeout_ms(app_t *a) {
    if (a->plan_mode) return 0; /* plan mode never times out a question */
    double secs = settings_num("ask.timeout", 0);
    return secs > 0 ? (int64_t)(secs * 1000) : 0;
}

/* askSingleQuestion over select/editor dialogs (hosts that did not opt into `set_ask_dialog`): one result
 * {id, selectedOptions, customInput?, timedOut?} (owned), or NULL when cancelled. *err set when the client left. */
static json_t *ask_one(rpc_t *r, const json_t *q, const char *progress, volatile sig_atomic_t *cancel, bool *gone) {
    const json_t *opts = json_get_arr(q, "options"), *o;
    bool multi = json_get_bool(q, "multi", false);
    int64_t rec = json_get(q, "recommended") ? json_get_int(q, "recommended", -1) : -1;
    int64_t timeout = ask_timeout_ms(r->a);
    char *prompt = progress ? xasprintf("%s (%s)", json_get_str(q, "question") ? json_get_str(q, "question") : "", progress)
                            : xstrdup(json_get_str(q, "question") ? json_get_str(q, "question") : "");
    strvec_t selected = {0};
    char *custom = NULL;
    bool timed_out = false, cancelled = false;
    if (multi) {
        char *done_label = xasprintf("%s Done selecting", tui_sym("status.success"));
        for (;;) {
            strvec_t labels = {0};
            json_t *details = NULL;
            json_arr_foreach(opts, i, o) {
                strvec_push(&labels, json_get_str(o, "label") ? json_get_str(o, "label") : "");
                char *desc = str_trim(json_get_str(o, "description") ? json_get_str(o, "description") : "");
                if (*desc) {
                    if (!details) details = json_arr();
                    while (json_len(details) < i) json_push(details, json_obj());
                    json_t *dd = json_obj();
                    json_set_str(dd, "description", desc);
                    json_push(details, dd);
                }
                free(desc);
            }
            if (selected.n) strvec_push(&labels, done_label);
            strvec_push(&labels, ASK_OTHER);
            while (details && json_len(details) < labels.n) json_push(details, json_obj());
            char *title = selected.n ? xasprintf("(%zu selected) %s", selected.n, prompt) : xstrdup(prompt);
            bool to = false;
            char *choice = ui_select(r, title, &labels, details, NULL, timeout, cancel, &to, gone);
            json_free(details);
            strvec_free(&labels);
            if (!choice) {
                free(title);
                if (to) timed_out = true;
                else cancelled = true;
                break;
            }
            if (!strcmp(choice, done_label)) {
                free(choice);
                free(title);
                break;
            }
            if (!strcmp(choice, ASK_OTHER)) {
                free(choice);
                char *in = ui_editor(r, title, cancel, gone);
                free(title);
                if (!in) {
                    if (*gone || (cancel && *cancel)) {
                        cancelled = true;
                        break;
                    }
                    continue;
                }
                custom = in;
                break;
            }
            free(title);
            bool had = false;
            for (size_t k = 0; k < selected.n; k++)
                if (!strcmp(selected.v[k], choice)) {
                    free(selected.v[k]);
                    memmove(&selected.v[k], &selected.v[k + 1], (selected.n - k - 1) * sizeof(char *));
                    selected.n--;
                    had = true;
                    break;
                }
            if (!had) strvec_push(&selected, choice);
            free(choice);
        }
        free(done_label);
    } else {
        for (;;) {
            strvec_t labels = {0};
            json_t *details = NULL;
            json_arr_foreach(opts, i, o) {
                const char *l = json_get_str(o, "label") ? json_get_str(o, "label") : "";
                if ((int64_t)i == rec && !str_ends(l, ASK_RECOMMENDED)) strvec_push_own(&labels, xasprintf("%s%s", l, ASK_RECOMMENDED));
                else strvec_push(&labels, l);
                char *desc = str_trim(json_get_str(o, "description") ? json_get_str(o, "description") : "");
                if (*desc) {
                    if (!details) details = json_arr();
                    while (json_len(details) < i) json_push(details, json_obj());
                    json_t *dd = json_obj();
                    json_set_str(dd, "description", desc);
                    json_push(details, dd);
                }
                free(desc);
            }
            size_t nopts = labels.n;
            strvec_push(&labels, ASK_OTHER);
            while (details && json_len(details) < labels.n) json_push(details, json_obj());
            bool to = false;
            char *choice = ui_select(r, prompt, &labels, details, NULL, timeout, cancel, &to, gone);
            json_free(details);
            timed_out = to;
            if (!choice) {
                strvec_free(&labels);
                if (!timed_out) cancelled = true;
                break;
            }
            if (!strcmp(choice, ASK_OTHER)) {
                free(choice);
                strvec_free(&labels);
                char *in = ui_editor(r, prompt, cancel, gone);
                if (!in) {
                    if (*gone || (cancel && *cancel)) {
                        cancelled = true;
                        break;
                    }
                    continue;
                }
                custom = in;
                strvec_free(&selected);
                break;
            }
            size_t picked = nopts;
            for (size_t k = 0; k < nopts && picked == nopts; k++)
                if (!strcmp(labels.v[k], choice)) picked = k;
            strvec_free(&selected);
            if (picked < nopts) strvec_push(&selected, json_get_str(json_at(opts, picked), "label"));
            else if (str_ends(choice, ASK_RECOMMENDED))
                strvec_push_own(&selected, xstrndup(choice, strlen(choice) - strlen(ASK_RECOMMENDED)));
            else strvec_push(&selected, choice);
            free(choice);
            strvec_free(&labels);
            break;
        }
    }
    free(prompt);
    if (timed_out && !selected.n && !custom && json_len(opts)) {
        int64_t idx = rec >= 0 && rec < (int64_t)json_len(opts) ? rec : 0;
        strvec_push(&selected, json_get_str(json_at(opts, (size_t)idx), "label"));
    }
    if (cancelled && !timed_out) {
        strvec_free(&selected);
        free(custom);
        return NULL;
    }
    json_t *res = json_obj(), *sel = json_arr();
    json_set_str(res, "id", json_get_str(q, "id") ? json_get_str(q, "id") : "");
    for (size_t k = 0; k < selected.n; k++) json_push_str(sel, selected.v[k]);
    json_set(res, "selectedOptions", sel);
    if (custom) json_set_str(res, "customInput", custom);
    if (timed_out) json_set_bool(res, "timedOut", true);
    strvec_free(&selected);
    free(custom);
    return res;
}

/* parseAskDialogResponse: the host's answers validated against the questions; errors as {"__error"} */
static json_t *ask_parse_answers(const json_t *resp, const json_t *qs) {
    const json_t *answers = json_get(resp, "answers");
    json_t *out = json_obj(), *results = json_arr();
    char *err = NULL;
    if (!json_is_arr(answers) || json_len(answers) != json_len(qs)) {
        err = xasprintf("Ask dialog response must carry %zu answers in question order", json_len(qs));
        goto fail;
    }
    const json_t *q;
    json_arr_foreach(qs, i, q) {
        const json_t *an = json_at(answers, i);
        const char *qid = json_get_str(q, "id") ? json_get_str(q, "id") : "";
        char *qidq = json_dump_js(json_get(q, "id"));
        if (!json_is_obj(an) || !str_eq(json_get_str(an, "id"), qid)) {
            err = xasprintf("Ask dialog answer %zu must have id %s", i, qidq);
            free(qidq);
            goto fail;
        }
        const json_t *sel = json_get(an, "selectedOptions"), *x, *o;
        if (!json_is_arr(sel)) {
            err = xasprintf("Ask dialog answer %s must carry a selectedOptions array", qidq);
            free(qidq);
            goto fail;
        }
        json_t *picked = json_arr();
        json_arr_foreach(sel, k, x) {
            bool known = false;
            json_arr_foreach(json_get_arr(q, "options"), m, o) known |= json_is_str(x) && str_eq(json_get_str(o, "label"), json_sv(x));
            char *xq = json_dump_js(x);
            if (!known) err = xasprintf("Ask dialog answer %s selected unknown option %s", qidq, xq);
            else {
                json_arr_foreach(picked, m, o) if (str_eq(json_sv(o), json_sv(x))) {
                    err = xasprintf("Ask dialog answer %s selected %s twice", qidq, xq);
                    break;
                }
            }
            free(xq);
            if (err) break;
            json_push_str(picked, json_sv(x));
        }
        const json_t *ci = json_get(an, "customInput");
        if (!err && ci && !json_is_str(ci)) err = xasprintf("Ask dialog answer %s customInput must be a string", qidq);
        char *custom = ci && json_is_str(ci) ? str_trim(json_sv(ci)) : NULL;
        if (custom && !*custom) {
            free(custom);
            custom = NULL;
        }
        if (!err && !json_get_bool(q, "multi", false) && (json_len(picked) > 1 || (json_len(picked) && custom)))
            err = xasprintf("Ask dialog answer %s is single-select but carries more than one answer", qidq);
        free(qidq);
        if (err) {
            free(custom);
            json_free(picked);
            goto fail;
        }
        json_t *res = json_obj();
        json_set_str(res, "id", qid);
        json_set(res, "selectedOptions", picked);
        if (custom) json_set_str(res, "customInput", custom);
        free(custom);
        json_push(results, res);
    }
    json_set(out, "__results", results);
    return out;
fail:
    json_free(results);
    json_set_str(out, "__error", err);
    free(err);
    return out;
}

/* tool_ui_t.ask: one `ask` request when the host opted in (set_ask_dialog), else select/editor dialogs per question */
static json_t *rpc_ask(void *ud, const json_t *qs) {
    rpc_t *r = ud;
    volatile sig_atomic_t *cancel = &r->a->agent->cancel;
    if (r->ask_dialog) {
        json_t *f = json_obj(), *arr = json_arr(), *q, *o;
        json_set_str(f, "method", "ask");
        json_arr_foreach(qs, i, q) {
            json_t *x = json_obj(), *opts = json_arr();
            json_set_str(x, "id", json_get_str(q, "id") ? json_get_str(q, "id") : "");
            json_set_str(x, "question", json_get_str(q, "question") ? json_get_str(q, "question") : "");
            char *h = str_trim(json_get_str(q, "header") ? json_get_str(q, "header") : "");
            if (*h) json_set_str(x, "header", json_get_str(q, "header"));
            free(h);
            json_arr_foreach(json_get_arr(q, "options"), k, o) {
                json_t *y = json_obj();
                json_set_str(y, "label", json_get_str(o, "label") ? json_get_str(o, "label") : "");
                char *d = str_trim(json_get_str(o, "description") ? json_get_str(o, "description") : "");
                if (*d) json_set_str(y, "description", d);
                free(d);
                char *pv = str_trim(json_get_str(o, "preview") ? json_get_str(o, "preview") : "");
                if (*pv) json_set_str(y, "preview", json_get_str(o, "preview"));
                free(pv);
                json_push(opts, y);
            }
            json_set(x, "options", opts);
            if (json_get(q, "multi")) json_set_bool(x, "multi", json_get_bool(q, "multi", false));
            if (json_get(q, "recommended")) json_set(x, "recommended", json_clone(json_get(q, "recommended")));
            json_push(arr, x);
        }
        json_set(f, "questions", arr);
        int64_t timeout = ask_timeout_ms(r->a);
        if (timeout > 0) json_set_int(f, "timeout", timeout);
        json_t *resp = NULL;
        bool timed_out = false;
        int rc = ui_request(r, f, timeout, cancel, &resp, &timed_out);
        if (rc < 0) {
            json_t *e = json_obj();
            json_set_str(e, "__error", "RPC client disconnected before extension UI response completed");
            return e;
        }
        if (timed_out || (rc == 0 && json_get_bool(resp, "cancelled", false) && json_get_bool(resp, "timedOut", false))) {
            json_free(resp); /* timedOutAskDialogResult: every question answers its recommended option */
            json_t *out = json_obj(), *res = json_arr();
            json_arr_foreach(qs, i, q) {
                json_t *x = json_obj(), *sel = json_arr();
                json_set_str(x, "id", json_get_str(q, "id") ? json_get_str(q, "id") : "");
                size_t n = json_len(json_get_arr(q, "options"));
                int64_t idx = json_get_int(q, "recommended", 0);
                if (idx < 0) idx = 0;
                if (n && idx > (int64_t)n - 1) idx = (int64_t)n - 1;
                if (n) json_push_str(sel, json_get_str(json_at(json_get_arr(q, "options"), (size_t)idx), "label"));
                json_set(x, "selectedOptions", sel);
                json_set_bool(x, "timedOut", true);
                json_push(res, x);
            }
            json_set(out, "__results", res);
            return out;
        }
        if (rc != 0 || json_get_bool(resp, "cancelled", false)) {
            json_free(resp);
            return NULL;
        }
        json_t *out = ask_parse_answers(resp, qs);
        json_free(resp);
        return out;
    }
    json_t *results = json_arr(), *q;
    size_t n = json_len(qs);
    bool gone = false;
    json_arr_foreach(qs, i, q) {
        char *progress = n > 1 ? xasprintf("%zu/%zu", i + 1, n) : NULL;
        json_t *res = ask_one(r, q, progress, cancel, &gone);
        free(progress);
        if (!res) {
            json_free(results);
            if (gone) {
                json_t *e = json_obj();
                json_set_str(e, "__error", "RPC client disconnected before extension UI response completed");
                return e;
            }
            return NULL;
        }
        json_push(results, res);
    }
    json_t *out = json_obj();
    json_set(out, "__results", results);
    return out;
}

#define RPC_GONE "RPC client disconnected before extension UI response completed"

/* ---------- the ask tool's whole HTML form (requestRpcFormDialog, omp b1470da89c and its follow-ups) ---------- */

static char *js_quote(const char *s) { /* JSON.stringify of a string (owned) */
    json_t *j = json_str(s ? s : "");
    char *q = json_dump(j, 0);
    json_free(j);
    return q;
}

/* parseFormChoice: each string one of the field's enabled option values, except one free-text entry when the field
 * offers "Other"; checkbox groups and multi-selects take an array. *val (owned, NULL = nothing chosen) */
static bool form_choice(const hf_field_t *f, const json_t *value, json_t **val, char **err) {
    *val = NULL;
    bool *checked = xcalloc(f->opts.n ? f->opts.n : 1, sizeof *checked);
    if (!value) {
        *val = hf_choice_value(f, checked, NULL);
        free(checked);
        return true;
    }
    char *label = js_quote(f->name);
    bool array = hf_is_array_choice(f);
    const char *shape = array ? "an array of strings" : "one string, not an array";
    const char *other = NULL;
    bool ok = true;
    if (array != json_is_arr(value)) {
        *err = xasprintf("Form field %s must carry %s", label, shape);
        ok = false;
    }
    size_t n = !ok ? 0 : array ? json_len(value) : 1;
    for (size_t i = 0; i < n && ok; i++) {
        const json_t *e = array ? json_at(value, i) : value;
        if (!json_is_str(e)) {
            *err = xasprintf("Form field %s must carry %s", label, shape);
            ok = false;
            break;
        }
        const char *entry = json_sv(e);
        long enabled = -1, any = -1;
        for (size_t k = 0; k < f->opts.n; k++) {
            if (strcmp(f->opts.v[k].value ? f->opts.v[k].value : "", entry)) continue;
            if (any < 0) any = (long)k;
            if (enabled < 0 && !f->opts.v[k].disabled) enabled = (long)k;
        }
        long idx = enabled >= 0 ? enabled : any;
        if (idx >= 0 && f->opts.v[idx].disabled) {
            char *q = js_quote(entry);
            *err = xasprintf("Form field %s carries disabled option %s", label, q);
            free(q);
            ok = false;
        } else if (idx >= 0 && !checked[idx]) checked[idx] = true;
        else if (idx < 0 && f->other && !other) other = entry;
        else {
            char *q = js_quote(entry);
            *err = xasprintf("Form field %s carries unexpected value %s", label, q);
            free(q);
            ok = false;
        }
    }
    if (ok) *val = hf_choice_value(f, checked, other);
    free(label);
    free(checked);
    return ok;
}

/* parseChoiceOrHiddenFallback: an echoed same-name hidden value counts as no selection when it cannot be the answer */
static bool form_choice_hidden(const hf_field_t *f, const json_t *value, const char *hidden, json_t **val, char **err) {
    if (!hidden || !value) return form_choice(f, value, val, err);
    long echo = -1;
    if (json_is_arr(value)) {
        for (size_t i = 0; i < json_len(value) && echo < 0; i++)
            if (json_is_str(json_at(value, i)) && !strcmp(json_sv(json_at(value, i)), hidden)) echo = (long)i;
    } else if (json_is_str(value) && !strcmp(json_sv(value), hidden)) echo = 0;
    if (echo < 0) return form_choice(f, value, val, err);
    json_t *without = NULL;
    if (json_is_arr(value)) {
        without = json_arr();
        for (size_t i = 0; i < json_len(value); i++)
            if ((long)i != echo) json_push(without, json_clone(json_at(value, i)));
    }
    bool carried_by_option = false;
    for (size_t k = 0; k < f->opts.n; k++)
        if (str_eq(f->opts.v[k].value, hidden)) carried_by_option = true;
    bool ok;
    if (json_is_arr(value) && !carried_by_option) ok = form_choice(f, without, val, err);
    else {
        char *e1 = NULL;
        ok = form_choice(f, value, val, &e1);
        if (!ok) {
            free(e1);
            ok = form_choice(f, without, val, err);
        }
    }
    json_free(without);
    return ok;
}

/* parseFormText: a string, or a finite number for `number`/`range` inputs; absent counts as empty */
static bool form_text(const hf_field_t *f, const json_t *value, json_t **val, char **err) {
    *val = NULL;
    if (!value) {
        *val = hf_text_value(f, "");
        return true;
    }
    bool numeric = str_eq(f->type, "number") || str_eq(f->type, "range");
    if (json_is_str(value)) {
        *val = hf_text_value(f, json_sv(value));
        return true;
    }
    if (numeric && json_is_num(value) && isfinite(value->u.num.d)) {
        char *t = json_dump(value, 0); /* String(value) */
        *val = hf_text_value(f, t);
        free(t);
        return true;
    }
    char *label = js_quote(f->name);
    *err = xasprintf("Form field %s must carry a string%s", label, numeric ? " or a number" : "");
    free(label);
    return false;
}

static bool form_field_value(const hf_field_t *f, const json_t *entry, const char *hidden, json_t **val, char **err) {
    return f->kind == HF_CHOICE ? form_choice_hidden(f, entry, hidden, val, err) : form_text(f, entry, val, err);
}

/* parseFormDialogResponse: validates submitted `data` against the parsed form; any mismatch fails (owned data, or NULL
 * with *err) */
static json_t *form_parse_data(const hf_form_t *form, const json_t *submitted, char **err) {
    if (!json_is_obj(submitted)) {
        *err = xstrdup("Form dialog response must carry a data object");
        return NULL;
    }
    for (size_t i = 0; i < json_len(submitted); i++) {
        const char *key = submitted->u.obj.keys[i];
        bool known = strvec_contains(&form->hidden_names, key);
        for (size_t k = 0; k < form->fields.n && !known; k++)
            if (form->fields.v[k]->name && *form->fields.v[k]->name && !strcmp(form->fields.v[k]->name, key)) known = true;
        if (!known) {
            char *q = js_quote(key);
            *err = xasprintf("Form dialog data carries unknown field %s", q);
            free(q);
            return NULL;
        }
    }
    json_t *data = json_obj();
    for (size_t i = 0; i < form->hidden_names.n; i++) json_set_str(data, form->hidden_names.v[i], form->hidden_values.v[i]);
    for (size_t i = 0; i < form->fields.n; i++) {
        const hf_field_t *first = form->fields.v[i];
        if (!first->name || !*first->name) continue;
        bool seen = false;
        for (size_t k = 0; k < i && !seen; k++) seen = str_eq(form->fields.v[k]->name, first->name);
        if (seen) continue;
        const char *name = first->name;
        const json_t *entry = json_get(submitted, name);
        const char *hidden = NULL;
        for (size_t k = 0; k < form->hidden_names.n; k++) /* new Map(form.hidden): the last pair wins */
            if (!strcmp(form->hidden_names.v[k], name)) hidden = form->hidden_values.v[k];
        VEC(const hf_field_t *) fields = {0};
        for (size_t k = i; k < form->fields.n; k++)
            if (str_eq(form->fields.v[k]->name, name)) vec_push(&fields, form->fields.v[k]);
        json_t **vals = xcalloc(fields.n, sizeof *vals);
        bool *carried = xcalloc(fields.n, sizeof *carried), any = false, ok = true;
        char *mismatch = NULL;
        for (size_t k = 0; k < fields.n && ok; k++) {
            char *e = NULL;
            if (form_field_value(fields.v[k], entry, hidden, &vals[k], &e)) {
                carried[k] = any = true;
                continue;
            }
            /* only a field sharing its name may decline the entry; a lone field's mismatch fails the answer */
            if (fields.n == 1) {
                *err = e;
                ok = false;
                break;
            }
            if (!mismatch) mismatch = e;
            else free(e);
            char *e2 = NULL;
            form_field_value(fields.v[k], NULL, NULL, &vals[k], &e2);
            free(e2);
        }
        if (ok && !any) {
            *err = mismatch;
            mismatch = NULL;
            ok = false;
        }
        for (size_t k = 0; k < fields.n && ok; k++) {
            char *problem = hf_field_error(fields.v[k], vals[k]);
            if (problem) {
                char *q = js_quote(name);
                *err = xasprintf("Form field %s: %s", q, problem);
                free(q);
                free(problem);
                ok = false;
                break;
            }
            if (carried[k] && vals[k]) json_set(data, name, json_clone(vals[k]));
        }
        for (size_t k = 0; k < fields.n; k++) json_free(vals[k]);
        free(vals);
        free(carried);
        free(mismatch);
        vec_free(&fields);
        if (!ok) {
            json_free(data);
            return NULL;
        }
    }
    return data;
}

/* tool_ui_t.form: the ask tool's HTML form as one `form` request; a timeout submits the form's defaults. A frame
 * larger than the negotiated protocol delivers intact fails without being sent (omp 9a323f5dc7). */
static json_t *rpc_form(void *ud, const char *html, int64_t timeout_ms, bool *timed_out, json_t **notes, json_t **given,
                        volatile sig_atomic_t *cancel, char **err) {
    rpc_t *r = ud;
    *timed_out = false;
    *given = NULL;
    if (notes) *notes = NULL;
    json_t *f = json_obj();
    json_set_str(f, "method", "form");
    json_set_str(f, "html", html);
    if (timeout_ms > 0) json_set_int(f, "timeout", timeout_ms);
    { /* a throwaway 16-character id measures the frame */
        json_t *probe = json_obj();
        json_set_str(probe, "type", "extension_ui_request");
        json_set_str(probe, "id", "0000000000000000");
        for (size_t i = 0; i < json_len(f); i++) json_set(probe, f->u.obj.keys[i], json_clone(f->u.obj.vals[i]));
        char *enc = json_dump(probe, 0);
        size_t bytes = strlen(enc), limit = r->proto == 2 ? RPC_MAX_REASSEMBLED_BYTES : RPC_MAX_FRAME_BYTES - 1;
        free(enc);
        json_free(probe);
        if (bytes > limit) {
            *err = xasprintf("Form is too large for the RPC host (%zu bytes, limit %zu); use fewer or smaller images, or split the form",
                             bytes, limit);
            json_free(f);
            return NULL;
        }
    }
    hf_form_t *form = hf_parse(html);
    json_t *resp = NULL;
    bool to = false;
    int rc = ui_request(r, f, timeout_ms, cancel, &resp, &to);
    json_t *data = NULL;
    if (rc < 0) *err = xstrdup(RPC_GONE);
    else if (to || (rc == 0 && json_get_bool(resp, "cancelled", false) && json_get_bool(resp, "timedOut", false))) {
        data = hf_default_data(form);
        *timed_out = true;
    } else if (rc == 0 && !json_get_bool(resp, "cancelled", false))
        data = form_parse_data(form, json_get(resp, "data"), err);
    json_free(resp);
    hf_form_free(form);
    return data;
}

/* ---------- the ask tool's field-by-field fallback (tool_ui_t.select / input / editor) ---------- */

static char *rpc_fb_select(void *ud, const char *title, const strvec_t *options, const tool_select_opts_t *so,
                           int64_t timeout_ms, bool *timed_out, volatile sig_atomic_t *cancel, char **err) {
    bool gone = false, to = false;
    char *v = ui_select(ud, title, options, NULL, so, timeout_ms, cancel, &to, &gone);
    if (timed_out) *timed_out = to;
    if (gone && !*err) *err = xstrdup(RPC_GONE);
    return v;
}

static char *rpc_fb_input(void *ud, const char *title, const char *placeholder, int64_t timeout_ms, bool *timed_out,
                          volatile sig_atomic_t *cancel, char **err) {
    rpc_t *r = ud;
    json_t *f = json_obj();
    json_set_str(f, "method", "input");
    json_set_str(f, "title", title);
    if (placeholder) json_set_str(f, "placeholder", placeholder);
    if (timeout_ms > 0) json_set_int(f, "timeout", timeout_ms);
    json_t *resp = NULL;
    bool to = false;
    int rc = ui_request(r, f, timeout_ms, cancel, &resp, &to);
    if (rc == 0 && json_get_bool(resp, "cancelled", false) && json_get_bool(resp, "timedOut", false)) to = true;
    if (timed_out) *timed_out = to;
    if (rc < 0 && !*err) *err = xstrdup(RPC_GONE);
    char *v = rc == 0 && !json_get_bool(resp, "cancelled", false) && json_get_str(resp, "value") ? xstrdup(json_get_str(resp, "value")) : NULL;
    json_free(resp);
    return v;
}

static char *rpc_fb_editor(void *ud, const char *title, const char *prefill, bool prompt_style, volatile sig_atomic_t *cancel,
                           char **err) {
    rpc_t *r = ud;
    json_t *f = json_obj();
    json_set_str(f, "method", "editor");
    json_set_str(f, "title", title);
    if (prefill) json_set_str(f, "prefill", prefill);
    if (prompt_style) json_set_bool(f, "promptStyle", true);
    json_t *resp = NULL;
    int rc = ui_request(r, f, 0, cancel, &resp, NULL);
    if (rc < 0 && !*err) *err = xstrdup(RPC_GONE);
    char *v = rc == 0 && !json_get_bool(resp, "cancelled", false) && json_get_str(resp, "value") ? xstrdup(json_get_str(resp, "value")) : NULL;
    json_free(resp);
    return v;
}

/* tool_ui_t.approve: requestApproval for a host without an approval dialog — a select with the full prompt and
 * approvalChoiceLabels; anything but an approval is a denial */
static int rpc_approve(void *ud, const char *tool_call_id, const char *tool_name, const char *status,
                       const approval_prompt_t *p, int *grant, bool *always, volatile sig_atomic_t *cancel) {
    UNUSED(tool_call_id);
    UNUSED(tool_name);
    UNUSED(status);
    rpc_t *r = ud;
    strvec_t labels = {0};
    approval_choice_labels(p, &labels);
    char *choice = ui_select(r, p->full, &labels, NULL, NULL, 0, cancel, NULL, NULL);
    strvec_free(&labels);
    approval_answer_t ans = approval_answer_for_choice(p, choice);
    free(choice);
    *grant = ans.grant;
    *always = ans.always;
    return (int)ans.kind;
}

/* ---------- host tools (implemented by the RPC client) ---------- */

static int host_tool_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    rpc_t *r = self->ud;
    hostcall_t *hc = xcalloc(1, sizeof(*hc));
    pthread_mutex_lock(&r->host_mu);
    hc->id = xasprintf("host-%llu", (unsigned long long)++r->host_seq);
    hc->ctx = ctx;
    vec_push(&r->host_calls, hc);
    pthread_mutex_unlock(&r->host_mu);
    json_t *req = json_obj();
    json_set_str(req, "type", "host_tool_call");
    json_set_str(req, "id", hc->id);
    json_set_str(req, "toolCallId", ctx && ctx->tool_call_id ? ctx->tool_call_id : "");
    json_set_str(req, "toolName", self->name);
    json_set(req, "arguments", args ? json_clone(args) : json_obj());
    emit(r, req);
    json_free(req);
    bool cancelled = false;
    pthread_mutex_lock(&r->host_mu);
    while (!hc->done) {
        if (ctx && ctx->cancel && *ctx->cancel) {
            cancelled = true;
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&r->host_cv, &r->host_mu, &ts);
    }
    for (size_t i = 0; i < r->host_calls.n; i++)
        if (r->host_calls.v[i] == hc) {
            r->host_calls.v[i] = r->host_calls.v[--r->host_calls.n];
            break;
        }
    pthread_mutex_unlock(&r->host_mu);
    if (cancelled) {
        json_t *c = json_obj();
        json_set_str(c, "type", "host_tool_cancel");
        char *cid = xasprintf("%s-cancel", hc->id);
        json_set_str(c, "id", cid);
        free(cid);
        json_set_str(c, "targetId", hc->id);
        emit(r, c);
        json_free(c);
        tool_result_error(out, "Tool call cancelled");
    } else {
        json_t *blk;
        json_arr_foreach(json_get_arr(hc->result, "content"), i, blk) {
            if (str_eq(json_get_str(blk, "type"), "image")) tool_result_image(out, json_get_str(blk, "data"), json_get_str(blk, "mimeType"));
            else if (json_get_str(blk, "text")) tool_result_text(out, json_get_str(blk, "text"));
        }
        if (json_is_str(json_get(hc->result, "content"))) tool_result_text(out, json_sv(json_get(hc->result, "content")));
        if (json_get(hc->result, "details")) out->details = json_clone(json_get(hc->result, "details"));
        out->is_error = hc->is_error;
    }
    json_free(hc->result);
    free(hc->id);
    free(hc);
    return 0;
}

/* host_tool_update: the host's partial result streams as tool_execution_update (RpcHostToolBridge.handleUpdate) */
static void host_tool_progress(rpc_t *r, const json_t *cmd) {
    const char *id = json_get_str(cmd, "id");
    const json_t *pr = json_get_obj(cmd, "partialResult"), *blk;
    if (!pr) return;
    pthread_mutex_lock(&r->host_mu);
    for (size_t i = 0; i < r->host_calls.n; i++) {
        hostcall_t *hc = r->host_calls.v[i];
        if (!str_eq(hc->id, id) || hc->done || !hc->ctx || !hc->ctx->update) continue;
        tool_result_t part = {0};
        json_arr_foreach(json_get_arr(pr, "content"), k, blk) {
            if (str_eq(json_get_str(blk, "type"), "image")) tool_result_image(&part, json_get_str(blk, "data"), json_get_str(blk, "mimeType"));
            else if (json_get_str(blk, "text")) tool_result_text(&part, json_get_str(blk, "text"));
        }
        if (json_get(pr, "details")) part.details = json_clone(json_get(pr, "details"));
        hc->ctx->update(hc->ctx, &part);
        tool_result_free(&part);
    }
    pthread_mutex_unlock(&r->host_mu);
}

static void host_tool_complete(rpc_t *r, const json_t *cmd) {
    const char *id = json_get_str(cmd, "id");
    pthread_mutex_lock(&r->host_mu);
    for (size_t i = 0; i < r->host_calls.n; i++)
        if (str_eq(r->host_calls.v[i]->id, id)) {
            r->host_calls.v[i]->result = json_clone(json_get(cmd, "result"));
            r->host_calls.v[i]->is_error = json_get_bool(cmd, "isError", false);
            r->host_calls.v[i]->done = true;
        }
    pthread_cond_broadcast(&r->host_cv);
    pthread_mutex_unlock(&r->host_mu);
}

/* ---------- session tree ---------- */

static json_t *tree_node(const session_t *s, const json_t *e, const json_t *labels, int depth) {
    json_t *n = json_obj();
    json_set(n, "entry", json_clone(e));
    const char *lab = json_get_str(labels, json_get_str(e, "id"));
    if (lab) json_set_str(n, "label", lab);
    json_t *kids = json_arr();
    if (depth < 10000) {
        json_t *ch = session_children(s, json_get_str(e, "id")), *c;
        json_arr_foreach(ch, i, c) json_push(kids, tree_node(s, c, labels, depth + 1));
        json_free(ch);
    }
    json_set(n, "children", kids);
    return n;
}

static json_t *session_tree_json(const session_t *s) {
    json_t *labels = json_obj(), *e;
    json_arr_foreach(s->entries, i, e) {
        if (!str_eq(json_get_str(e, "type"), "label")) continue;
        const char *t = json_get_str(e, "targetId");
        if (!t) continue;
        if (json_get_str(e, "label") && *json_get_str(e, "label")) json_set_str(labels, t, json_get_str(e, "label"));
        else json_del(labels, t);
    }
    json_t *roots = json_arr();
    json_arr_foreach(s->entries, i, e) {
        const char *pid = json_get_str(e, "parentId");
        if (!pid || !session_get(s, pid)) json_push(roots, tree_node(s, e, labels, 0));
    }
    json_free(labels);
    return roots;
}

/* a returned background call's job frame (task progress): omp's onUpdate keeps emitting tool_execution_update for
 * the call after its tool_execution_end */
static void rpc_tool_frame(void *ud, const char *tool_call_id, const char *tool_name, const json_t *args, const char *text,
                           json_t *details) {
    rpc_t *r = ud;
    tool_result_t pr = {0};
    tool_result_text(&pr, text ? text : "");
    pr.details = details;
    agent_event_t ev = {.type = AGENT_EV_TOOL_UPDATE, .tool_call_id = tool_call_id, .tool_name = tool_name,
                        .args = args, .result = &pr};
    forward(r, agent_event_to_json(&ev));
    tool_result_free(&pr);
}

/* omc carries a delivered background-job batch as a hidden user turn; on the wire it is omp's custom message
 * (async-job-delivery.ts buildAsyncResultBatchMessage): {role:"custom", customType, content, display, attribution,
 * details:{meta, jobs}, timestamp}. Rewrites `m` in place. */
static void rpc_project_message(json_t *m) {
    if (!json_is_obj(m) || !str_eq(json_get_str(m, "role"), "user") || !str_eq(json_get_str(m, "customType"), "async-result"))
        return;
    char *txt = ai_message_text(m);
    json_t *det = json_get(m, "details") ? json_clone(json_get(m, "details")) : json_obj();
    int64_t ts = json_get_int(m, "timestamp", now_ms());
    json_t *nd = json_obj(), *meta = json_obj(), *src = json_obj();
    json_set_str(src, "type", "report");
    json_set_str(src, "value", "background job delivery");
    json_set(meta, "source", src);
    json_set(nd, "meta", meta);
    json_set(nd, "jobs", json_get_arr(det, "jobs") ? json_clone(json_get_arr(det, "jobs")) : json_arr());
    json_free(det);
    while (m->u.obj.n) json_del(m, m->u.obj.keys[0]);
    json_set_str(m, "role", "custom");
    json_set_str(m, "customType", "async-result");
    json_set_str(m, "content", txt);
    json_set_bool(m, "display", true);
    json_set_str(m, "attribution", "agent");
    json_set(m, "details", nd);
    json_set_int(m, "timestamp", ts);
    free(txt);
}

static void rpc_project_messages(json_t *frame) {
    rpc_project_message(json_get(frame, "message"));
    json_t *ms = json_get_arr(frame, "messages"), *m;
    json_arr_foreach(ms, i, m) rpc_project_message(m);
}

/* hasPendingAsyncWork: a background job still running or a settled one not yet delivered can wake the session */
static bool rpc_pending_async_work(app_t *a) { return app_pending_async_work(a); }

static void set_settling(rpc_t *r, bool on) {
    pthread_mutex_lock(&r->mu);
    r->settling = on;
    pthread_mutex_unlock(&r->mu);
}

static void rpc_listener(void *ud, const agent_event_t *ev) {
    rpc_t *r = ud;
    json_t *j = agent_event_to_json(ev);
    if (ev->type == AGENT_EV_AGENT_END) {
        /* a run that hands control back while background jobs can still wake it is a scheduling pause, not the
         * terminal stop (agent-session.ts #hasPendingAsyncWake -> willContinue + awaitingAsyncWork) */
        const char *stop = json_get_str(last_assistant(r->a->agent->messages), "stopReason");
        bool awaiting = !str_eq(stop, "aborted") && rpc_pending_async_work(r->a);
        json_set_bool(j, "isTerminal", !awaiting);
        json_set_bool(j, "yielded", true);
        if (awaiting) json_set_bool(j, "awaitingAsyncWork", true);
    }
    rpc_project_messages(j);
    /* past agent_end the prompt thread only finishes the run, unless another agent run starts (set before the
     * client can see the event and send its next prompt) */
    if (ev->type == AGENT_EV_AGENT_START || ev->type == AGENT_EV_AGENT_END) set_settling(r, ev->type == AGENT_EV_AGENT_END);
    forward(r, j);
    if (ev->type == AGENT_EV_AGENT_END) {
        pthread_mutex_lock(&r->mu);
        r->agent_ends++;
        pthread_cond_broadcast(&r->abort_cv);
        pthread_mutex_unlock(&r->mu);
    }
}

/* the serial command queue and the aborted prompt's prompt_result hold behind a pending `abort` reply */
static void abort_barrier(rpc_t *r) {
    pthread_mutex_lock(&r->mu);
    while (r->abort_waiters > 0) pthread_cond_wait(&r->abort_cv, &r->mu);
    pthread_mutex_unlock(&r->mu);
}

/* rpc-prompt-results.ts #report: sessionSettled, then the error */
static void prompt_result_settled(json_t *res, bool settled) {
    json_t *e = json_get(res, "error") ? json_clone(json_get(res, "error")) : NULL;
    json_del(res, "error");
    json_set_bool(res, "sessionSettled", settled);
    if (e) json_set(res, "error", e);
}

static void notice_rpc(void *ud, const char *level, const char *text) {
    rpc_t *r = ud;
    if (!strcmp(level, "retry-recovered")) return;
    if (!strcmp(level, "todo-reminder")) { /* {type:"todo_reminder", todos, attempt, maxAttempts} */
        json_t *j = json_parse(text, strlen(text), NULL);
        if (!json_is_obj(j)) {
            json_free(j);
            return;
        }
        json_t *o = json_obj();
        json_set_str(o, "type", "todo_reminder");
        const char *k;
        json_t *v;
        json_obj_foreach(j, i, k, v) json_set(o, k, json_clone(v));
        json_free(j);
        emit(r, o);
        json_free(o);
        return;
    }
    if (!strcmp(level, "cache_warming_start") || !strcmp(level, "cache_warming_end")) { /* cachewarm.c events */
        json_t *j = json_parse(text, strlen(text), NULL);
        if (json_is_obj(j)) forward(r, j);
        else json_free(j);
        return;
    }
    if (!strcmp(level, "auto_compaction_start") || !strcmp(level, "auto_compaction_end")) {
        /* compaction.c carries omp's AgentSessionEvent as JSON */
        json_t *j = json_parse(text, strlen(text), NULL);
        if (json_is_obj(j)) forward(r, j);
        else json_free(j);
        return;
    }
    /* the TUI's compaction progress lines; RPC clients get manual/auto_compaction_* events instead */
    if (!strcmp(level, "info") && (!strcmp(text, "Compacting context\xe2\x80\xa6") || !strcmp(text, "Context compacted"))) return;
    if (!strcmp(level, "retry-failed")) {
        const char *f1 = strchr(text, '\x1f'), *f2 = f1 ? strchr(f1 + 1, '\x1f') : NULL;
        if (!f1 || !f2) return;
        json_t *j = json_obj();
        json_set_str(j, "type", "auto_retry_end");
        json_set_bool(j, "success", false);
        json_set_int(j, "attempt", atoi(text));
        json_set_str(j, "finalError", f2 + 1);
        emit(r, j);
        json_free(j);
        return;
    }
    if (!strcmp(level, "session-persistence")) { /* registerRpcPersistenceSurface */
        char *d0 = tui_truncate_to_width(text, 110, "\xe2\x80\xa6"), *d = tui_strip_ansi(d0);
        free(d0);
        char *m = xasprintf("Session persistence failed: %s. Writes are retried; unsaved entries stay in memory until the store "
                            "accepts them again.", d);
        free(d);
        json_t *j = json_obj();
        json_set_str(j, "type", "notice");
        json_set_str(j, "level", "error");
        json_set_str(j, "message", m);
        json_set_str(j, "source", "session-persistence");
        emit(r, j);
        json_free(j);
        fprintf(stderr, "%s\n", m);
        free(m);
        return;
    }
    if (!strcmp(level, "retry_fallback_applied")) {
        static const char *const keys[] = {"from", "to", "role", "reason"};
        json_t *j = json_obj();
        json_set_str(j, "type", "retry_fallback_applied");
        const char *s = text;
        for (size_t k = 0; k < 4 && s; k++) {
            const char *sep = strchr(s, '\x1f');
            char *v = sep ? xstrndup(s, (size_t)(sep - s)) : xstrdup(s);
            json_set_str(j, keys[k], v);
            free(v);
            s = sep ? sep + 1 : NULL;
        }
        emit(r, j);
        json_free(j);
        return;
    }
    if (!strcmp(level, "retry_fallback_succeeded")) {
        const char *sep = strchr(text, '\x1f');
        json_t *j = json_obj();
        json_set_str(j, "type", "retry_fallback_succeeded");
        char *model = sep ? xstrndup(text, (size_t)(sep - text)) : xstrdup(text);
        json_set_str(j, "model", model);
        json_set_str(j, "role", sep ? sep + 1 : "");
        free(model);
        emit(r, j);
        json_free(j);
        return;
    }
    json_t *j = json_obj();
    json_set_str(j, "type", "notice");
    bool tc = !strcmp(level, "toolCommands"); /* emitNotice("info", …, "toolCommands") */
    json_set_str(j, "level", tc ? "info" : level);
    json_set_str(j, "message", text);
    if (tc) json_set_str(j, "source", "toolCommands");
    else if (str_starts(text, "Prewalk: ")) json_set_str(j, "source", "prewalk");
    emit(r, j);
    json_free(j);
}

static void rpc_command_output(void *ud, const char *text) {
    json_t *o = json_obj();
    json_set_str(o, "type", "command_output");
    json_set_str(o, "text", text);
    emit(ud, o);
    json_free(o);
}

/* /retry: the builtin's result reaches the host before the retried turn streams */
static void rpc_slash_agent(void *ud) {
    rpc_t *r = ud;
    if (!r->slash_reply) return;
    r->slash_reply = false;
    json_t *d = json_obj();
    json_set_bool(d, "agentInvoked", true);
    respond(r, r->prompt_id, "prompt", true, d, NULL);
}

static void *prompt_thread(void *p) {
    rpc_t *r = p;
again:;
    set_settling(r, false);
    size_t before = json_len(r->a->agent->messages);
    bool refused = false;
    if (r->slash) {
        /* builtin: output as command_output frames; only an agent-invoking one (/retry) reports a
         * prompt_result (the prompt response already said agentInvoked) */
        r->slash = false;
        slash_result_t sr;
        slash_execute_text_ex(r->a, r->text, rpc_command_output, rpc_slash_agent, r, &sr);
        bool invoked = sr.agent_invoked;
        if (r->slash_reply && sr.kind != SLASH_PROMPT) { /* a builtin that ran locally: its output came first */
            r->slash_reply = false;
            json_t *d = json_obj();
            json_set_bool(d, "agentInvoked", invoked);
            respond(r, r->prompt_id, "prompt", true, d, NULL);
        }
        if (sr.kind == SLASH_PROMPT && sr.prompt) { /* builtinResult.prompt: sent as the prompt (/force <tool> <prompt>) */
            pthread_mutex_lock(&r->mu);
            free(r->text);
            r->text = sr.prompt;
            sr.prompt = NULL;
            pthread_mutex_unlock(&r->mu);
            slash_result_free(&sr);
            if (r->slash_reply) { /* builtinResult.prompt: acknowledged as the prompt it became */
                r->slash_reply = false;
                respond(r, r->prompt_id, "prompt", true, NULL, NULL);
            }
            goto again;
        }
        slash_result_free(&sr);
        json_free(r->images);
        r->images = NULL;
        if (!invoked) {
            pthread_mutex_lock(&r->mu);
            free(r->text);
            r->text = NULL;
            free(r->prompt_id);
            r->prompt_id = NULL;
            r->running = false;
            pthread_mutex_unlock(&r->mu);
            return NULL;
        }
    } else if (r->resume) {
        char *err = NULL;
        if (app_resume(r->a, &err) != 0 && err) { /* a failure after the acknowledgement is a late error */
            json_t *f = json_obj();
            json_set_str(f, "type", "response");
            json_set_str(f, "command", "resume");
            json_set_bool(f, "success", false);
            json_set_str(f, "error", err);
            emit(r, f);
            json_free(f);
        }
        free(err);
    } else if (r->hidden) app_prompt_hidden(r->a, r->text, r->hidden);
    else if (r->skill_msg) {
        json_t *m = r->skill_msg;
        r->skill_msg = NULL;
        refused = app_prompt_skill(r->a, m) == APP_PROMPT_REFUSED;
    } else {
        /* AgentSession.prompt: custom (MCP prompt) / file commands / prompt templates expand first */
        char *x = app_expand_input(r->a, r->text, true);
        if (!x) { /* consumed locally: prompt_result agentInvoked false (completeLocal) */
            json_free(r->images);
            r->images = NULL;
            json_t *res = json_obj();
            json_set_str(res, "type", "prompt_result");
            if (r->prompt_id) json_set_str(res, "id", r->prompt_id);
            json_set_bool(res, "agentInvoked", false);
            json_set_str(res, "status", "completed");
            emit(r, res);
            json_free(res);
            pthread_mutex_lock(&r->mu);
            free(r->text);
            r->text = NULL;
            free(r->prompt_id);
            r->prompt_id = NULL;
            r->running = false;
            pthread_mutex_unlock(&r->mu);
            return NULL;
        }
        refused = app_prompt(r->a, x, r->images) == APP_PROMPT_REFUSED;
        free(x);
    }
    r->images = NULL;
    set_settling(r, true); /* the prompt returned: what follows is this run's tail (also without an agent run) */
    abort_barrier(r); /* an `abort` of this run is answered before its prompt_result */
    const json_t *last = last_assistant(r->a->agent->messages);
    const char *stop = json_get_str(last, "stopReason");
    json_t *res = json_obj();
    json_set_str(res, "type", "prompt_result");
    if (r->prompt_id) json_set_str(res, "id", r->prompt_id);
    json_set_bool(res, "agentInvoked", !refused);
    /* a run stopped from inside (a cancelled ask, Esc) ends aborted even when its last turn completed */
    const char *status = refused ? "error"
                         : str_eq(stop, "aborted") || r->a->agent->cancel ? "aborted"
                         : str_eq(stop, "error")                          ? "error"
                                                                          : "completed";
    json_set_str(res, "status", status);
    if (refused) {
        json_t *e = json_obj();
        json_set_str(e, "message", r->a->prompt_refusal ? r->a->prompt_refusal : "error");
        json_set_str(e, "provider", r->a->model ? r->a->model->provider : "");
        json_set_str(e, "model", r->a->model ? r->a->model->id : "");
        json_set_bool(e, "retryable", false);
        json_set(res, "error", e);
    } else if (!strcmp(status, "error")) {
        json_t *e = json_obj();
        char *pm = ai_strip_request_dump_lines(json_get_str(last, "errorMessage"));
        json_set_str(e, "message", pm ? pm : "error");
        free(pm);
        json_set_str(e, "provider", json_get_str(last, "provider"));
        json_set_str(e, "model", json_get_str(last, "model"));
        json_set_bool(e, "retryable", (aie_classify_message(last) & AIE_TRANSIENT) != 0); /* AIError.Flag.Transient */
        if (json_get(last, "errorStatus")) json_set_int(e, "httpStatus", json_get_int(last, "errorStatus", 0));
        json_set(res, "error", e);
    }
    /* goal mode: keep going with hidden continuation turns while the goal is active and making progress */
    bool was_cont = false;
    const char *prev_type = r->hidden;
    /* a background-job wake run has no prompt ticket: omp reports no prompt_result for it (rpc-prompt-results.ts) */
    bool wake_run = str_eq(r->hidden, "async-result") || r->resume;
    r->resume = false;
    for (;;) {
        json_t *run = json_arr();
        pthread_mutex_lock(&r->a->agent->mu);
        for (size_t i = before; i < json_len(r->a->agent->messages); i++) json_push(run, json_clone(json_at(r->a->agent->messages, i)));
        pthread_mutex_unlock(&r->a->agent->mu);
        const char *ctype = NULL;
        char *cont = app_after_run(r->a, run, prev_type, "rpc", &ctype);
        json_free(run);
        if (!cont) break;
        if (!was_cont && !wake_run) {
            prompt_result_settled(res, false);
            emit(r, res);
        }
        was_cont = true;
        before = json_len(r->a->agent->messages);
        prev_type = ctype;
        set_settling(r, false); /* a continuation turn: prompts queue into it again */
        app_prompt_hidden(r->a, cont, ctype);
        set_settling(r, true);
        free(cont);
    }
    /* isSessionSettled: background work that can still wake the session keeps it unsettled; its wake run's
     * terminal agent_end closes the stretch with session_settled */
    bool settled = !rpc_pending_async_work(r->a);
    prompt_result_settled(res, settled);
    if (!was_cont && !wake_run) emit(r, res);
    /* prompts queued into this run (steer / follow-up) report at the same yield */
    pthread_mutex_lock(&r->mu);
    json_t *waiting = r->waiting;
    r->waiting = NULL;
    pthread_mutex_unlock(&r->mu);
    json_t *w;
    json_arr_foreach(waiting, wi, w) {
        json_t *x = json_obj();
        json_set_str(x, "type", "prompt_result");
        if (json_is_str(w)) json_set_str(x, "id", json_sv(w));
        json_set_bool(x, "agentInvoked", true);
        json_set_str(x, "status", json_get_str(res, "status"));
        json_set_bool(x, "sessionSettled", settled);
        if (json_get(res, "error")) json_set(x, "error", json_clone(json_get(res, "error")));
        emit(r, x);
        json_free(x);
    }
    json_free(waiting);
    json_free(res);
    if (settled) {
        json_t *sf = json_obj();
        json_set_str(sf, "type", "session_settled");
        emit(r, sf);
        json_free(sf);
    }
    pthread_mutex_lock(&r->mu);
    free(r->text);
    r->text = NULL;
    free(r->prompt_id);
    r->prompt_id = NULL;
    /* a job may have settled between the run's last check and now: deliver it in this same thread */
    char *late = jobs_undelivered(r->a->env->jobs) || jobs_has_notices(r->a->env->jobs) ? app_job_results_text(r->a) : NULL;
    if (late) {
        r->text = late;
        r->hidden = "async-result";
        r->images = NULL;
        pthread_mutex_unlock(&r->mu);
        goto again;
    }
    r->running = false;
    r->hidden = NULL;
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

/* background job settled while idle: run its result as a hidden turn */
static void rpc_wake(void *ud) {
    rpc_t *r = ud;
    pthread_mutex_lock(&r->mu);
    if (r->running || r->compacting) { /* a manual compaction replaces history first; it wakes again when it ends */
        pthread_mutex_unlock(&r->mu);
        return;
    }
    char *t = app_job_results_text(r->a);
    if (!t) {
        pthread_mutex_unlock(&r->mu);
        return;
    }
    if (r->th) pthread_join(r->th, NULL);
    r->th = 0;
    r->running = true;
    r->settling = false;
    r->hidden = "async-result";
    r->text = t;
    r->prompt_id = NULL;
    r->images = NULL;
    if (pthread_create(&r->th, NULL, prompt_thread, r) != 0) {
        r->running = false;
        r->hidden = NULL;
    }
    pthread_mutex_unlock(&r->mu);
}

/* pi-ai Model as JSON: the catalog object with omp's leading key order and compat merged over the api defaults */
static json_t *model_json(const ai_model_t *m) {
    json_t *o = json_obj();
    const json_t *raw = m->raw;
    json_set_str(o, "id", m->id);
    json_set_str(o, "name", m->name ? m->name : m->id);
    json_set_str(o, "api", m->api);
    json_set_str(o, "provider", m->provider);
    if (m->base_url) json_set_str(o, "baseUrl", m->base_url);
    json_set_bool(o, "reasoning", m->reasoning);
    if (m->thinking) json_set(o, "thinking", json_clone(m->thinking));
    if (json_get(raw, "input")) json_set(o, "input", json_clone(json_get(raw, "input")));
    else {
        json_t *in = json_arr();
        json_push_str(in, "text");
        if (m->input_image) json_push_str(in, "image");
        json_set(o, "input", in);
    }
    if (json_get(raw, "cost")) json_set(o, "cost", json_clone(json_get(raw, "cost")));
    else {
        json_t *c = json_obj();
        json_set_num(c, "input", m->cost_input);
        json_set_num(c, "output", m->cost_output);
        json_set_num(c, "cacheRead", m->cost_cache_read);
        json_set_num(c, "cacheWrite", m->cost_cache_write);
        json_set(o, "cost", c);
    }
    json_set_int(o, "contextWindow", m->context_window);
    json_set_int(o, "maxTokens", m->max_tokens);
    json_t *merged = json_obj();
    if (m->compat_defaults) json_merge(merged, m->compat_defaults);
    if (m->compat) json_merge(merged, m->compat);
    if (json_len(merged)) {
        /* pi-ai's resolved compat key order, then anything else the catalog carries */
        static const char *const order[] = {
            "supportsStore", "supportsDeveloperRole", "supportsMultipleSystemMessages", "supportsReasoningEffort",
            "supportsReasoningParams", "supportsSamplingParams", "supportsPenaltyAndStopParams", "reasoningEffortMap",
            "supportsUsageInStreaming", "alwaysSendMaxTokens", "disableReasoningOnForcedToolChoice",
            "disableReasoningOnToolChoice", "disableReasoningWithTools", "supportsToolChoice", "supportsForcedToolChoice",
            "supportsNamedToolChoice", "maxTokensField", "requiresToolResultName", "requiresAssistantAfterToolResult",
            "foldUserAfterToolResult", "requiresThinkingAsText", "requiresMistralToolIds", "thinkingFormat", "reasoningDisableMode",
            "omitReasoningEffort", "includeEncryptedReasoning", "filterReasoningHistory", "reasoningContentField",
            "requiresReasoningContentForToolCalls", "requiresReasoningContentForAllAssistantTurns",
            "allowsSyntheticReasoningContentForToolCalls", "replayReasoningContent", "qwenPreserveThinking",
            "qwenTemplateReasoningEffort", "requiresAssistantContentForToolCalls", "supportsPromptCacheBreakpoints",
            "isOpenRouterHost", "wireModelIdMode", "isVercelGatewayHost", "supportsStrictMode", "toolStrictMode",
            "toolSchemaFlavor", "streamFirstEventTimeoutMs", "streamIdleTimeoutMs", "stripDeepseekSpecialTokens",
            "streamMarkupHealingPattern", "reasoningDeltasMayBeCumulative", "emptyLengthFinishIsContextError",
            "usesOpenAIToolCallIdLimit", "dropThinkingWhenReasoningEffort", "nativeKimiK3Reasoning",
            "zaiReasoningEffortDialect", "clampOutputToModelMax", "stripImageInput", "rejectRootObjectUnion",
            "retryWithoutStrictOnGrammarError", "supportsPromptCacheKey"};
        json_t *compat = json_obj();
        for (size_t i = 0; i < ARRAY_LEN(order); i++)
            if (json_get(merged, order[i])) json_set(compat, order[i], json_clone(json_get(merged, order[i])));
        for (size_t i = 0; i < json_len(merged); i++)
            if (!json_get(compat, merged->u.obj.keys[i])) json_set(compat, merged->u.obj.keys[i], json_clone(merged->u.obj.vals[i]));
        json_set(o, "compat", compat);
    }
    json_free(merged);
    for (size_t i = 0; json_is_obj(raw) && i < json_len(raw); i++) {
        const char *k = raw->u.obj.keys[i];
        if (json_get(o, k) || !strcmp(k, "compat") || !strcmp(k, "headers")) continue;
        json_set(o, k, json_clone(raw->u.obj.vals[i]));
    }
    /* catalog defaults every resolved model carries */
    if (!json_get(o, "identity")) {
        json_t *idn = json_obj();
        json_set_str(idn, "class", "unknown");
        json_set(o, "identity", idn);
    }
    if (!json_get(o, "requiresGlyphTokenization")) json_set_bool(o, "requiresGlyphTokenization", false);
    if (!json_get(o, "supportsComputerUse")) json_set_bool(o, "supportsComputerUse", false);
    return o;
}

/* ContextUsage {tokens, contextWindow, percent} or NULL without a model */
static json_t *context_usage_json(app_t *a) {
    if (!a->model) return NULL;
    int64_t used = ui_context_used(a), window = a->model->context_window;
    json_t *c = json_obj();
    json_set_int(c, "tokens", used);
    json_set_int(c, "contextWindow", window);
    json_set_num(c, "percent", window > 0 ? (double)used / (double)window * 100 : 0);
    return c;
}

/* SessionStats (session-stats.ts getSessionStats) */
static json_t *session_stats_json(app_t *a) {
    int64_t user = 0, asst = 0, tres = 0, tcalls = 0, in = 0, out = 0, rs = 0, cr = 0, cw = 0, tot = 0, prem = 0;
    double cost = 0;
    json_t *m, *b;
    pthread_mutex_lock(&a->agent->mu);
    size_t total = json_len(a->agent->messages);
    json_arr_foreach(a->agent->messages, i, m) {
        const char *role = json_get_str(m, "role");
        const json_t *u = NULL;
        if (session_message_is_user(m)) user++;
        else if (str_eq(role, "toolResult")) {
            tres++;
            if (str_eq(json_get_str(m, "toolName"), "task")) u = json_get_obj(json_get(m, "details"), "usage");
        } else if (str_eq(role, "assistant")) {
            asst++;
            json_arr_foreach(json_get_arr(m, "content"), k, b) if (str_eq(json_get_str(b, "type"), "toolCall")) tcalls++;
            u = json_get_obj(m, "usage");
        }
        if (!u) continue;
        in += json_get_int(u, "input", 0);
        out += json_get_int(u, "output", 0);
        rs += json_get_int(u, "reasoningTokens", 0);
        cr += json_get_int(u, "cacheRead", 0);
        cw += json_get_int(u, "cacheWrite", 0);
        tot += json_get_int(u, "totalTokens", 0);
        prem += json_get_int(u, "premiumRequests", 0);
        cost += json_get_num(json_get(u, "cost"), "total", 0);
    }
    pthread_mutex_unlock(&a->agent->mu);
    json_t *o = json_obj();
    if (a->session->file) json_set_str(o, "sessionFile", a->session->file);
    json_set_str(o, "sessionId", session_id(a->session));
    json_set_int(o, "userMessages", user);
    json_set_int(o, "assistantMessages", asst);
    json_set_int(o, "toolCalls", tcalls);
    json_set_int(o, "toolResults", tres);
    json_set_int(o, "totalMessages", (int64_t)total);
    json_t *t = json_obj();
    json_set_int(t, "input", in);
    json_set_int(t, "output", out);
    json_set_int(t, "reasoning", rs);
    json_set_int(t, "cacheRead", cr);
    json_set_int(t, "cacheWrite", cw);
    json_set_int(t, "total", tot);
    json_set(o, "tokens", t);
    json_set_num(o, "cost", cost);
    json_set_int(o, "premiumRequests", prem);
    json_t *cu = context_usage_json(a);
    if (cu) json_set(o, "contextUsage", cu);
    return o;
}

/* calculateTokensPerSecond: the last assistant message's output rate, or null */
static json_t *tokens_per_second(app_t *a, bool streaming) {
    json_t *res = json_null();
    pthread_mutex_lock(&a->agent->mu);
    for (size_t i = json_len(a->agent->messages); i-- > 0;) {
        const json_t *m = json_at(a->agent->messages, i);
        if (!str_eq(json_get_str(m, "role"), "assistant") || !json_get(m, "timestamp") || !json_get_obj(m, "usage")) continue;
        double outp = json_get_num(json_get(m, "usage"), "output", 0), dur = json_get_num(m, "duration", 0);
        if (outp > 0) {
            double d = dur > 0 ? dur : streaming ? (double)(now_ms() - json_get_int(m, "timestamp", 0)) : -1;
            if (d >= 100) {
                json_free(res);
                res = json_num(outp * 1000 / d);
            }
        }
        break;
    }
    pthread_mutex_unlock(&a->agent->mu);
    return res;
}

static bool session_settled(rpc_t *r) {
    pthread_mutex_lock(&r->mu);
    bool running = r->running;
    pthread_mutex_unlock(&r->mu);
    app_t *a = r->a;
    bool queued = json_len(a->agent->steering) || json_len(a->agent->follow_up);
    return !running && !agent_is_running(a->agent) && !queued && !jobs_running(a->env->jobs) && !jobs_undelivered(a->env->jobs);
}

/* getAvailableEffortSelectors: off, auto, then the model's efforts; [] without reasoning */
static json_t *effort_selectors(app_t *a) {
    json_t *lv = json_arr();
    if (!a->model || !a->model->reasoning) return lv;
    json_push_str(lv, "off");
    json_push_str(lv, "auto");
    for (int i = EFFORT_OFF + 1; i <= EFFORT_MAX; i++)
        if (model_supports_effort(a->model, (ai_effort_t)i)) json_push_str(lv, effort_name((ai_effort_t)i));
    return lv;
}

/* get_model_picker (rpc-model-picker.ts): omp's session model picker served over RPC: the Recent block first, the
 * search ranking, role chips, the thinking slider, `@` quick roles */
static json_t *model_picker_json(app_t *a, const char *query, int64_t limit) {
    json_t *rows = json_arr(), *out = json_obj();
    int64_t total = 0, recent = 0;
    if (query && query[0] == '@') {
        app_role_entry_t *cyc = NULL;
        int cur = 0;
        size_t n = app_role_cycle(a, &cyc, &cur);
        char **texts = xcalloc(n ? n : 1, sizeof *texts);
        for (size_t i = 0; i < n; i++) texts[i] = xasprintf("/@%s", cyc[i].role); /* modelSearchText of `@role` */
        size_t *idx = xmalloc((n ? n : 1) * sizeof *idx), m = n;
        char *q = str_trim(query);
        if (*q) m = tui_fuzzy_rank(query, (const char *const *)texts, n, idx);
        else
            for (size_t i = 0; i < n; i++) idx[i] = i;
        free(q);
        total = (int64_t)m;
        for (size_t k = 0; k < m && (int64_t)k < limit; k++) {
            const app_role_entry_t *e = &cyc[idx[k]];
            json_t *row = json_obj();
            json_set_str(row, "provider", e->model->provider);
            json_set_str(row, "id", e->model->id);
            json_set_str(row, "name", e->model->name ? e->model->name : e->model->id);
            json_set_str(row, "role", e->role);
            if (e->explicit_level && e->level) json_set_str(row, "thinkingLevel", e->level);
            if ((int)idx[k] == cur) json_set_bool(row, "current", true);
            json_push(rows, row);
        }
        for (size_t i = 0; i < n; i++) free(texts[i]);
        free(texts);
        free(idx);
        app_role_cycle_free(cyc, n);
    } else {
        ui_model_browser_t *b = ui_session_model_browser();
        ui_model_browser_done_adding(b);
        char *q = str_trim(query ? query : "");
        ui_model_browser_set_query(b, *q ? q : NULL);
        free(q);
        /* the untouched slider shows the level the session runs at: auto's resolved effort once a turn settled it */
        const char *level = a->auto_thinking ? (a->auto_resolved >= 0 ? effort_name((ai_effort_t)a->auto_resolved) : "auto")
                                             : app_wire_thinking(a);
        int64_t tokens = a->model ? ui_context_used(a) : 0;
        bool sep_seen = false;
        for (size_t v = 0; v < b->visible.n; v++) {
            if (b->visible.v[v] == UI_MB_SEPARATOR) {
                sep_seen = true;
                continue;
            }
            if (!sep_seen) recent++;
            total++;
            if (total > limit) continue;
            const ai_model_t *m = b->items.v[b->visible.v[v]].model;
            json_t *row = json_obj();
            json_set_str(row, "provider", m->provider);
            json_set_str(row, "id", m->id);
            json_set_str(row, "name", m->name ? m->name : m->id);
            json_t *roles = json_arr();
            for (size_t i = 0; i < b->nroles; i++) {
                if (b->roles[i].model != m || b->roles[i].hidden) continue;
                json_t *ro = json_obj();
                json_set_str(ro, "role", b->roles[i].role);
                json_set_str(ro, "label", b->roles[i].label ? b->roles[i].label : b->roles[i].role);
                if (b->roles[i].auto_selected) json_set_bool(ro, "auto", true);
                json_push(roles, ro);
            }
            if (json_len(roles)) json_set(row, "roles", roles);
            else json_free(roles);
            const char *steps[12], *value = NULL;
            bool disabled = true;
            size_t ns = ui_thinking_slider(m, level, steps, ARRAY_LEN(steps), &value, &disabled);
            if (!disabled) {
                json_t *th = json_arr();
                for (size_t i = 0; i < ns; i++) json_push_str(th, steps[i]);
                json_set(row, "thinking", th);
                json_set_str(row, "thinkingLevel", value);
            }
            if (tokens > 0 && m->context_window > 0 && tokens > m->context_window) json_set_bool(row, "overContext", true);
            if (a->model == m) json_set_bool(row, "current", true);
            json_push(rows, row);
        }
        if (!sep_seen) recent = 0;
        recent = MIN(recent, (int64_t)json_len(rows));
        ui_model_browser_free(b);
    }
    json_set(out, "rows", rows);
    json_set_int(out, "recent", recent);
    json_set_int(out, "total", total);
    return out;
}

/* RpcSessionState (get_state) */
static json_t *state_json(rpc_t *r) {
    app_t *a = r->a;
    json_t *s = json_obj();
    if (a->model) json_set(s, "model", model_json(a->model));
    if (app_wire_thinking(a)) json_set_str(s, "thinkingLevel", app_wire_thinking(a));
    /* the user's selector (`auto` while auto mode picks the effort) and the selectors the model accepts */
    json_set_str(s, "configuredThinkingLevel", a->auto_thinking ? "auto" : effort_name(a->thinking));
    json_set(s, "thinkingLevels", effort_selectors(a));
    bool streaming = agent_is_running(a->agent);
    json_set_bool(s, "isStreaming", streaming);
    json_set_bool(s, "isCompacting", a->compacting);
    json_set_str(s, "steeringMode", settings_str("steeringMode", "one-at-a-time"));
    json_set_str(s, "followUpMode", settings_str("followUpMode", "one-at-a-time"));
    json_set_str(s, "interruptMode", settings_str("interruptMode", "immediate"));
    if (a->session->file) json_set_str(s, "sessionFile", a->session->file);
    json_set_str(s, "sessionId", session_id(a->session));
    char *title = session_title_dup(a->session); /* the prompt thread may be renaming the session */
    if (title) json_set_str(s, "sessionName", title);
    free(title);
    json_set_bool(s, "autoCompactionEnabled", settings_bool("compaction.enabled", true));
    pthread_mutex_lock(&a->agent->mu);
    int64_t queued = (int64_t)(json_len(a->agent->steering) + json_len(a->agent->follow_up));
    pthread_mutex_unlock(&a->agent->mu);
    json_set_int(s, "queuedMessageCount", queued);
    json_set_bool(s, "hasPendingAsyncWork", jobs_running(a->env->jobs) || jobs_undelivered(a->env->jobs));
    json_set_bool(s, "isSettled", session_settled(r));
    json_set(s, "queuedMessages", app_queued_messages(a));
    json_set(s, "todoPhases", a->env->todo.phases ? json_clone(a->env->todo.phases) : json_arr());
    json_set_bool(s, "fastModeEnabled", app_fast_mode_enabled(a));
    json_set(s, "tokensPerSecond", tokens_per_second(a, streaming));
    json_set_bool(s, "fastModeActive", app_fast_mode_active(a));
    json_set_bool(s, "slowModeSupported", app_slow_mode_supported(a));
    json_set_bool(s, "slowModeEnabled", app_slow_mode_enabled(a));
    if (app_slow_mode_scope(a)) json_set_str(s, "slowModeScope", app_slow_mode_scope(a));
    json_t *ul = app_usage_limit_state(a);
    if (ul) json_set(s, "usageLimit", ul);
    json_set_int(s, "messageCount", (int64_t)json_len(a->agent->messages));
    pthread_mutex_lock(&a->agent->mu);
    json_set(s, "systemPrompt", json_clone(a->agent->system_prompt));
    json_t *tools = json_arr();
    for (size_t i = 0; i < a->agent->tools.n; i++) {
        const agent_tool_t *t = a->agent->tools.v[i];
        json_t *x = json_obj();
        json_set_str(x, "name", t->name);
        json_set_str(x, "description", t->description ? t->description : "");
        json_set(x, "parameters", t->parameters ? json_clone(t->parameters) : json_obj());
        if (t->examples) json_set(x, "examples", json_clone(t->examples)); /* rpc-mode.ts dumpTools: tool.examples */
        json_push(tools, x);
    }
    pthread_mutex_unlock(&a->agent->mu);
    json_set(s, "dumpTools", tools);
    json_t *cu = context_usage_json(a);
    if (cu) json_set(s, "contextUsage", cu);
    json_set(s, "goal", goal_state_json(app_goal(a)));
    return s;
}

/* ---------- get_messages_page (port of rpc-messages.ts) ---------- */

static double jnum(const json_t *j) { return json_is_num(j) ? j->u.num.d : 0; }

#define RPC_PAGE_DEFAULT_LIMIT 100
#define RPC_PAGE_MAX_LIMIT 256
#define RPC_PAGE_MAX_BYTES (768 * 1024)

static char *page_cursor(const char *sid, const char *leaf, int64_t count, int64_t offset) {
    json_t *c = json_obj();
    json_set_int(c, "version", 1);
    json_set_str(c, "sessionId", sid);
    json_set(c, "leafId", leaf ? json_str(leaf) : json_null());
    json_set_int(c, "messageCount", count);
    json_set_int(c, "offset", offset);
    size_t n;
    char *j = dump_js(c, &n);
    json_free(c);
    char *b = base64url_encode(j, n);
    free(j);
    return b;
}

/* decodeCursor: -1 invalid */
static int64_t page_decode(const char *cur, const char *sid, const char *leaf, int64_t count, bool *stale) {
    size_t n = strlen(cur);
    *stale = false;
    if (!n || n > 2048) return -1;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)cur[i]) && cur[i] != '_' && cur[i] != '-') return -1;
    char *std = xstrdup(cur);
    for (char *p = std; *p; p++) *p = *p == '-' ? '+' : *p == '_' ? '/' : *p;
    size_t blen = 0;
    unsigned char *bytes = base64_decode(std, n, &blen);
    free(std);
    if (!bytes) return -1;
    char *again = base64url_encode(bytes, blen);
    bool canon = again && !strcmp(again, cur);
    free(again);
    json_t *v = canon && utf8_valid((const char *)bytes, blen) ? json_parse((const char *)bytes, blen, NULL) : NULL;
    free(bytes);
    const json_t *l = json_get(v, "leafId");
    const char *csid = json_get_str(v, "sessionId");
    json_t *mc = json_get(v, "messageCount"), *off = json_get(v, "offset");
    bool ok = json_is_obj(v) && json_is_num(json_get(v, "version")) && jnum(json_get(v, "version")) == 1 && csid &&
              *csid && strlen(csid) <= 256 &&
              ((json_is_str(l) && *json_sv(l) && strlen(json_sv(l)) <= 256) || (json_is_null(l) && l)) && json_is_num(mc) &&
              mc->is_int && jnum(mc) >= 0 && json_is_num(off) && off->is_int && jnum(off) >= 0 &&
              jnum(off) <= jnum(mc);
    int64_t offset = ok ? (int64_t)jnum(off) : -1;
    if (ok && (strcmp(csid, sid) || (json_is_str(l) ? !str_eq(json_sv(l), leaf) : leaf != NULL) || (int64_t)jnum(mc) != count))
        *stale = true;
    json_free(v);
    return offset;
}


static json_t *content_with_images(const char *text, json_t *images) {
    json_t *c = json_arr();
    json_push(c, text_block(text ? text : ""));
    json_t *img;
    json_arr_foreach(images, i, img) {
        if (str_eq(json_get_str(img, "type"), "image")) json_push(c, json_clone(img));
    }
    return c;
}

/* ---------- /btw side questions (port of modes/rpc/rpc-btw.ts) ----------
 * One side question runs at a time against the main session; answers stream as `btw_delta` frames, every
 * lifecycle change is a full `btw_record` frame, and each turn is checkpointed into the session's BTW history
 * sidecar so the TUI and RPC hosts read and continue the same topics. */

#define BTW_CANCELLED_WHILE_STARTING "The /btw question was cancelled before it started"

typedef struct rpc_btw_run {
    rpc_t *r;
    json_t *record;
    btw_store_t *store;
    bool owns_store; /* the store was replaced while this turn ran: the turn frees it */
    char *question;
    json_t *history;
    char *conversation_key;
    volatile sig_atomic_t cancel;
} rpc_btw_run_t;

static void btw_emit_record(rpc_t *r, const json_t *record) {
    json_t *f = json_obj();
    json_set_str(f, "type", "btw_record");
    json_set(f, "record", btw_wire_record(record));
    emit(r, f);
    json_free(f);
}

static void btw_notice(rpc_t *r, const char *message) {
    json_t *j = json_obj();
    json_set_str(j, "type", "notice");
    json_set_str(j, "level", "error");
    json_set_str(j, "message", message);
    json_set_str(j, "source", "btw-history");
    emit(r, j);
    json_free(j);
}

/* #finish: the turn's terminal state, once; caller holds btw_mu */
static void btw_finish_locked(rpc_t *r, rpc_btw_run_t *run, const char *status, const char *answer, const char *error) {
    if (r->btw_running != run) return;
    r->btw_running = NULL;
    btw_set_status(run->record, status, answer, error);
    btw_emit_record(r, run->record);
    char *err = NULL;
    if (btw_store_upsert(run->store, run->record, &err) != 0) {
        if (run->store == r->btw_store) {
            if (!r->btw_unsaved) r->btw_unsaved = json_obj();
            json_set(r->btw_unsaved, json_get_str(run->record, "id"), json_clone(run->record));
        }
        char *m = xasprintf("Could not save /btw history: %s", err ? err : "write failed");
        log_msg(LOG_ERROR, "%s", m);
        btw_notice(r, m);
        free(m);
    }
    free(err);
}

static void btw_delta(void *ud, const char *delta) {
    rpc_btw_run_t *run = ud;
    rpc_t *r = run->r;
    pthread_mutex_lock(&r->btw_mu);
    if (r->btw_running == run) {
        json_t *t = btw_latest_turn(run->record);
        char *ans = xasprintf("%s%s", json_get_str(t, "answer") ? json_get_str(t, "answer") : "", delta);
        json_set_str(t, "answer", ans);
        json_set_int(t, "updatedAt", now_ms());
        free(ans);
        json_t *f = json_obj();
        json_set_str(f, "type", "btw_delta");
        json_set_str(f, "recordId", json_get_str(run->record, "id"));
        json_set_str(f, "delta", delta);
        emit(r, f);
        json_free(f);
    }
    pthread_mutex_unlock(&r->btw_mu);
}

static void *btw_thread(void *p) {
    rpc_btw_run_t *run = p;
    rpc_t *r = run->r;
    char *reply = NULL, *err = NULL;
    json_t *assistant = NULL;
    int rc = btw_run_turn(r->a, run->question, NULL, run->history, false, run->conversation_key, NULL, btw_delta, run,
                          &run->cancel, &reply, &assistant, &err);
    pthread_mutex_lock(&r->btw_mu);
    if (rc == 0) btw_finish_locked(r, run, "complete", reply, NULL);
    else btw_finish_locked(r, run, "error", NULL, err ? err : "side question failed");
    if (run->owns_store) btw_store_free(run->store);
    pthread_mutex_unlock(&r->btw_mu);
    json_free(assistant);
    free(reply);
    free(err);
    json_free(run->record);
    json_free(run->history);
    free(run->question);
    free(run->conversation_key);
    free(run);
    return NULL;
}

/* cancel(recordId?): the running or starting question (only topic `record_id` when given); caller holds btw_mu */
static bool btw_cancel_locked(rpc_t *r, const char *record_id) {
    rpc_btw_run_t *run = r->btw_running;
    if (run) {
        if (record_id && !str_eq(json_get_str(run->record, "id"), record_id)) return false;
        r->btw_epoch++;
        btw_finish_locked(r, run, "cancelled", NULL, NULL);
        run->cancel = 1;
        return true;
    }
    if (!r->btw_starting || (record_id && !str_eq(r->btw_starting_topic, record_id))) return false;
    r->btw_epoch++;
    return true;
}

/* #settleWrites: retry failed terminal checkpoints once; a conflict drops the record with a notice */
static int btw_settle_locked(rpc_t *r, char **err) {
    if (!r->btw_unsaved || !r->btw_store) return 0;
    while (json_len(r->btw_unsaved)) {
        const char *id = r->btw_unsaved->u.obj.keys[0];
        char *e = NULL;
        if (btw_store_retry(r->btw_store, r->btw_unsaved->u.obj.vals[0], &e) == 0 || (e && strstr(e, "conflict"))) {
            if (e) {
                char *m = xasprintf("/btw answer %s was not saved: %s; it changed or was removed on disk", id, e);
                log_msg(LOG_ERROR, "%s", m);
                btw_notice(r, m);
                free(m);
            }
            free(e);
            json_del(r->btw_unsaved, id);
            continue;
        }
        *err = xasprintf("/btw history could not be saved: %s. Fix the storage and retry; unsaved answers remain in this "
                         "session.",
                         e ? e : "write failed");
        free(e);
        return -1;
    }
    return 0;
}

static char *btw_artifacts_dir(app_t *a) {
    return a->session && a->session->file && a->session->persist ? artifacts_dir_for_session(a->session->file) : NULL;
}

/* #openStore: the current session's store; `fresh` re-reads it from disk (only while nothing runs here) */
static btw_store_t *btw_open_store_locked(rpc_t *r, bool fresh, char **err) {
    app_t *a = r->a;
    char *dir = btw_artifacts_dir(a);
    char *key = xasprintf("%s%c%s", a->session ? session_id(a->session) : "", 1, dir ? dir : "");
    bool same = r->btw_store && str_eq(r->btw_store_key, key);
    if (same && (!fresh || !dir || (r->btw_unsaved && json_len(r->btw_unsaved)))) {
        free(key);
        free(dir);
        return r->btw_store;
    }
    if (!same && r->btw_running) btw_cancel_locked(r, NULL); /* the old session's question must not stream on */
    btw_store_t *s = btw_store_open(dir, NULL, err);
    free(dir);
    if (!s) {
        free(key);
        return NULL;
    }
    if (r->btw_store) {
        if (r->btw_running && r->btw_running->store == r->btw_store) r->btw_running->owns_store = true;
        else btw_store_free(r->btw_store);
    }
    r->btw_store = s;
    free(r->btw_store_key);
    r->btw_store_key = key;
    return s;
}

/* ask(question, recordId?): checkpointed as running, then the turn starts after the response */
static json_t *btw_ask(rpc_t *r, const char *question, const char *record_id, char **err) {
    app_t *a = r->a;
    char *trimmed = str_trim(question ? question : "");
    json_t *out = NULL;
    pthread_mutex_lock(&r->btw_mu);
    if (!*trimmed) {
        *err = xstrdup("btw requires a non-empty question");
        goto done;
    }
    if (r->btw_starting || r->btw_running) {
        *err = xstrdup("A /btw question is still running; cancel it first");
        goto done;
    }
    r->btw_starting = true;
    free(r->btw_starting_topic);
    r->btw_starting_topic = record_id ? xstrdup(record_id) : NULL;
    int epoch = r->btw_epoch;
    if (btw_settle_locked(r, err) != 0) goto starting_done;
    btw_store_t *store = btw_open_store_locked(r, true, err);
    if (!store) goto starting_done;
    if (epoch != r->btw_epoch) {
        *err = xstrdup(BTW_CANCELLED_WHILE_STARTING);
        goto starting_done;
    }
    json_t *previous = record_id ? btw_store_find(store, record_id) : NULL;
    if (record_id && !previous) {
        *err = xasprintf("Unknown /btw topic: %s", record_id);
        goto starting_done;
    }
    if (!a->model) {
        json_free(previous);
        *err = xstrdup("No active model available for /btw.");
        goto starting_done;
    }
    if (a->session) session_ensure_on_disk(a->session);
    btw_begin_t b;
    char *leaf = a->session ? session_leaf_dup(a->session) : NULL;
    btw_begin_turn(trimmed, leaf, previous, NULL, NULL, &b);
    free(leaf);
    json_free(previous);
    char *werr = NULL;
    if (btw_store_upsert(store, b.record, &werr) != 0) {
        *err = xasprintf("Could not save /btw history: %s", werr ? werr : "write failed");
        free(werr);
        btw_begin_free(&b);
        goto starting_done;
    }
    rpc_btw_run_t *run = xcalloc(1, sizeof *run);
    run->r = r;
    run->record = b.record;
    run->history = b.history;
    run->conversation_key = b.conversation_key;
    run->store = store;
    run->question = xstrdup(trimmed);
    r->btw_running = run;
    btw_emit_record(r, run->record);
    out = btw_wire_record(run->record);
    if (r->btw_th_live) pthread_join(r->btw_th, NULL); /* the previous turn already finished */
    r->btw_th_live = false;
    if (pthread_create(&r->btw_th, NULL, btw_thread, run) == 0) r->btw_th_live = true;
    else btw_finish_locked(r, run, "error", NULL, "failed to start the /btw thread");
starting_done:
    r->btw_starting = false;
    free(r->btw_starting_topic);
    r->btw_starting_topic = NULL;
done:
    pthread_mutex_unlock(&r->btw_mu);
    free(trimmed);
    return out;
}

/* history(): newest first, re-read from disk when idle; a running topic carries its live partial answer */
static json_t *btw_history(rpc_t *r, char **err) {
    pthread_mutex_lock(&r->btw_mu);
    btw_store_t *s = btw_open_store_locked(r, !r->btw_running && !r->btw_starting, err);
    json_t *out = NULL;
    if (s) {
        json_t *recs = btw_store_records(s), *rec;
        json_t *keyed = json_obj();
        json_arr_foreach(recs, i, rec) json_set(keyed, json_get_str(rec, "id"), json_clone(rec));
        if (r->btw_unsaved && s == r->btw_store) {
            for (size_t i = 0; i < json_len(r->btw_unsaved); i++)
                json_set(keyed, r->btw_unsaved->u.obj.keys[i], json_clone(r->btw_unsaved->u.obj.vals[i]));
        }
        if (r->btw_running && r->btw_running->store == s)
            json_set(keyed, json_get_str(r->btw_running->record, "id"), json_clone(r->btw_running->record));
        json_t *all = json_arr();
        for (size_t i = 0; i < json_len(keyed); i++) json_push(all, json_clone(keyed->u.obj.vals[i]));
        /* sort newest first by createdAt, then id */
        size_t n = json_len(all);
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0; j--) {
                json_t *x = all->u.arr.items[j - 1], *y = all->u.arr.items[j];
                int64_t cx = json_get_int(x, "createdAt", 0), cy = json_get_int(y, "createdAt", 0);
                if (cx > cy || (cx == cy && strcmp(json_get_str(x, "id"), json_get_str(y, "id")) <= 0)) break;
                all->u.arr.items[j - 1] = y;
                all->u.arr.items[j] = x;
            }
        out = json_arr();
        json_arr_foreach(all, i, rec) json_push(out, btw_wire_record(rec));
        json_free(all);
        json_free(keyed);
        json_free(recs);
    }
    pthread_mutex_unlock(&r->btw_mu);
    return out;
}

/* close(): before the session is replaced or the process exits */
static int btw_close(rpc_t *r, char **err) {
    pthread_mutex_lock(&r->btw_mu);
    r->btw_epoch++;
    btw_cancel_locked(r, NULL);
    int rc = btw_settle_locked(r, err);
    pthread_mutex_unlock(&r->btw_mu);
    return rc;
}

/* ---------- commands answered off the reader thread ---------- */

typedef struct {
    rpc_t *r;
    json_t *cmd;
    void (*fn)(rpc_t *r, const json_t *cmd);
} bg_cmd_t;

static void *bg_main(void *p) {
    bg_cmd_t *b = p;
    b->fn(b->r, b->cmd);
    json_free(b->cmd);
    free(b);
    return NULL;
}

/* the background compaction's idle hook: no prompt can start while r->mu is held */
static bool rpc_idle_commit(app_t *a, void *ud) {
    rpc_t *r = ud;
    pthread_mutex_lock(&r->mu);
    bool applied = !r->running && !r->compacting && !agent_is_running(a->agent) && cmp_bg_apply(a);
    pthread_mutex_unlock(&r->mu);
    return applied;
}

/* `handoff`: one manual_compaction_start {mode: "handoff"} / _end pair; the response once the summary landed */
static void handoff_main(rpc_t *r, const json_t *cmd) {
    app_t *a = r->a;
    const char *id = json_get_str(cmd, "id");
    json_t *st = json_obj();
    json_set_str(st, "type", "manual_compaction_start");
    json_set_str(st, "mode", "handoff");
    app_emit(a, st);
    char *err = NULL;
    int rc = app_handoff_background(a, json_get_str(cmd, "customInstructions"), rpc_idle_commit, r, &err);
    json_t *en = json_obj();
    json_set_str(en, "type", "manual_compaction_end");
    bool skipped = rc != 0 && err && (!strcmp(err, "Already compacted") || !strncmp(err, "Nothing to compact", 18));
    if (rc == 0) json_set(en, "result", app_compaction_result(a));
    json_set_bool(en, "aborted", rc != 0 && !skipped && err && !strcmp(err, "Compaction cancelled"));
    if (skipped) json_set_bool(en, "skipped", true);
    if (rc != 0) json_set_str(en, "errorMessage", err ? err : "Compaction failed");
    app_emit(a, en);
    if (rc == 0) respond(r, id, "handoff", true, json_obj(), NULL);
    else respond(r, id, "handoff", false, NULL, err ? err : "Compaction failed");
    free(err);
    rpc_wake(r); /* results that settled meanwhile go into the compacted context */
}

static void run_background(rpc_t *r, const json_t *cmd, void (*fn)(rpc_t *, const json_t *)) {
    bg_cmd_t *b = xcalloc(1, sizeof *b);
    b->r = r;
    b->cmd = json_clone(cmd);
    b->fn = fn;
    pthread_t th;
    if (pthread_create(&th, NULL, bg_main, b) != 0) {
        bg_main(b);
        return;
    }
    pthread_mutex_lock(&r->bg_mu);
    vec_push(&r->bg_threads, th);
    pthread_mutex_unlock(&r->bg_mu);
}

/* executeBash: BashResult {exitCode?, cancelled, workingDir, output, truncated, totalLines, totalBytes, outputLines,
 * outputBytes, elidedBytes?, elidedLines?, columnDroppedBytes?, columnTruncatedLines?, columnTruncatedRange?, columnMax?};
 * `abort_bash` stops it */
static void bash_main(rpc_t *r, const json_t *cmd) {
    app_t *a = r->a;
    const char *id = json_get_str(cmd, "id");
    pthread_mutex_lock(&r->bash_mu);
    if (!r->bash_running++) r->bash_cancel = 0;
    pthread_mutex_unlock(&r->bash_mu);
    json_t *m = app_user_bash_run(a, json_get_str(cmd, "command"), json_get_bool(cmd, "excludeFromContext", false), NULL,
                                  NULL, &r->bash_cancel);
    pthread_mutex_lock(&r->bash_mu);
    r->bash_running--;
    pthread_mutex_unlock(&r->bash_mu);
    if (!m) {
        respond(r, id, "bash", false, NULL, "command required");
        return;
    }
    app_record_bash(a, m);
    json_t *d = json_obj();
    if (json_get(m, "exitCode")) json_set(d, "exitCode", json_clone(json_get(m, "exitCode")));
    json_set_bool(d, "cancelled", json_get_bool(m, "cancelled", false));
    json_set_str(d, "workingDir", a->cwd);
    static const char *const keys[] = {"output", "truncated", "totalLines", "totalBytes", "outputLines", "outputBytes", "elidedBytes",
                                       "elidedLines", "columnDroppedBytes", "columnTruncatedLines", "columnTruncatedRange", "columnMax"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++)
        if (json_get(m, keys[i])) json_set(d, keys[i], json_clone(json_get(m, keys[i])));
    json_free(m);
    respond(r, id, "bash", true, d, NULL);
}

static void bash_start(rpc_t *r, const json_t *cmd, const char *id) {
    if (!json_get_str(cmd, "command")) {
        respond(r, id, "bash", false, NULL, "command required");
        return;
    }
    run_background(r, cmd, bash_main);
}

/* ---------- login / logout (rpc-mode.ts; omc keeps one credential per provider: credentialId 1) ---------- */

static const json_t *login_provider(const char *pid) {
    const json_t *p;
    json_arr_foreach(auth_oauth_providers(), i, p) if (json_get_obj(p, "login") && str_eq(json_get_str(p, "id"), pid)) return p;
    return NULL;
}

typedef struct {
    rpc_t *r;
    const char *provider;
    bool auth_emitted;
    char *error;
} login_ctx_t;

static void login_on_auth(void *ud, const char *url, const char *launch_url, const char *instructions) {
    login_ctx_t *c = ud;
    c->auth_emitted = true;
    json_t *f = json_obj();
    json_set_str(f, "method", "open_url");
    json_set_str(f, "url", url ? url : "");
    if (launch_url) json_set_str(f, "launchUrl", launch_url);
    if (instructions) json_set_str(f, "instructions", instructions);
    ui_fire(c->r, f);
}

static void login_on_progress(void *ud, const char *msg) {
    login_ctx_t *c = ud;
    json_t *f = json_obj();
    json_set_str(f, "method", "notify");
    json_set_str(f, "message", msg ? msg : "");
    json_set_str(f, "notifyType", "info");
    ui_fire(c->r, f);
}

static char *login_prompt(void *ud, const auth_prompt_t *p) {
    login_ctx_t *c = ud;
    if (p->secret) {
        if (!c->error)
            c->error = xasprintf("Provider '%s' requires secret input, which is not supported in RPC mode. Use the terminal UI "
                                 "to log in.",
                                 c->provider);
        c->r->login_abort = 1;
        return NULL;
    }
    if (!c->auth_emitted && !p->risk_acknowledgement) { /* the login-warning acceptance is a plain yes/no the host can show */
        if (!c->error)
            c->error = xasprintf("Provider '%s' requires interactive prompts which are not supported in RPC mode. Use the "
                                 "terminal UI to log in.",
                                 c->provider);
        c->r->login_abort = 1;
        return NULL;
    }
    json_t *f = json_obj();
    json_set_str(f, "method", "input");
    json_set_str(f, "title", p->message ? p->message : "");
    if (p->placeholder) json_set_str(f, "placeholder", p->placeholder);
    json_set_int(f, "timeout", 600000);
    json_t *resp = NULL;
    int rc = ui_request(c->r, f, 600000, NULL, &resp, NULL);
    char *v = xstrdup(rc == 0 && !json_get_bool(resp, "cancelled", false) && json_get_str(resp, "value") ? json_get_str(resp, "value") : "");
    if (rc < 0 && !c->error) c->error = xstrdup("RPC client disconnected before extension UI response completed");
    json_free(resp);
    return v;
}

static void login_main(rpc_t *r, const json_t *cmd) {
    const char *id = json_get_str(cmd, "id"), *pid = json_get_str(cmd, "providerId");
    login_ctx_t c = {r, pid, false, NULL};
    r->login_abort = 0;
    auth_login_hooks_t h = {0};
    h.on_auth = login_on_auth;
    h.on_progress = login_on_progress;
    h.prompt = login_prompt;
    h.abort = &r->login_abort;
    h.ud = &c;
    char *err = NULL;
    int rc = auth_login_hooked(pid, &h, &err);
    if (rc == 0) catalog_discover_provider(pid); /* refreshProvider after a login: the models appear in this session */
    if (rc == 0 && !c.error) {
        auth_reload();
        json_t *d = json_obj();
        json_set_str(d, "providerId", pid);
        respond(r, id, "login", true, d, NULL);
    } else respond(r, id, "login", false, NULL, c.error ? c.error : err ? err : "Login failed");
    free(c.error);
    free(err);
}

/* LogoutAccount rows for one provider */
static json_t *logout_accounts(const char *pid) {
    json_t *out = json_arr(), *all = auth_list(), *e;
    const char *store = auth_store_id(pid);
    json_arr_foreach(all, i, e) {
        if (!str_eq(json_get_str(e, "provider"), store)) continue;
        bool oauth = str_eq(json_get_str(e, "type"), "oauth");
        json_t *x = json_obj();
        json_set_int(x, "credentialId", 1);
        json_set_str(x, "provider", store);
        json_set_str(x, "label", oauth ? "OAuth credential #1" : "API key #1");
        json_set_str(x, "detail", oauth ? "oauth #1" : "stored API key #1");
        json_set_str(x, "type", oauth ? "oauth" : "api_key");
        json_set_bool(x, "active", true);
        json_push(out, x);
    }
    json_free(all);
    return out;
}

static void login_command(rpc_t *r, const json_t *cmd, const char *id, const char *type) {
    const json_t *pidj = json_get(cmd, "providerId");
    const char *pid = json_sv(pidj);
    if (!strcmp(type, "login")) {
        if (!pid || !login_provider(pid)) {
            char *v = pid ? xstrdup(pid) : pidj ? json_dump_js(pidj) : xstrdup("undefined");
            char *m = xasprintf("Unknown OAuth provider: %s", v);
            respond(r, id, type, false, NULL, m);
            free(m);
            free(v);
            return;
        }
        run_background(r, cmd, login_main);
    } else if (!strcmp(type, "get_logout_accounts")) {
        if (!pid) {
            respond(r, id, type, false, NULL, "providerId must be a string");
            return;
        }
        auth_reload();
        json_t *d = json_obj();
        json_set(d, "accounts", logout_accounts(pid));
        respond(r, id, type, true, d, NULL);
    } else {
        const json_t *cid = json_get(cmd, "credentialId");
        if (!pid || !json_is_num(cid) || !cid->is_int) {
            respond(r, id, type, false, NULL, "providerId must be a string and credentialId an integer");
            return;
        }
        auth_reload();
        json_t *rows = logout_accounts(pid);
        bool stored = json_len(rows) > 0 && cid->u.num.i == 1;
        json_free(rows);
        if (!stored || auth_logout(auth_store_id(pid)) != 0) {
            char *m = xasprintf("Credential %lld is not stored for %s", (long long)cid->u.num.i, pid);
            respond(r, id, type, false, NULL, m);
            free(m);
            return;
        }
        respond(r, id, type, true, json_obj(), NULL);
    }
}

/* ---------- session changes (handleRpcSessionChange, openRpcSession) ---------- */

static void emit_available_commands(rpc_t *r) {
    json_t *f = json_obj();
    json_set_str(f, "type", "available_commands_update");
    json_set(f, "commands", slash_available_commands(r->a));
    emit(r, f);
    json_free(f);
}

/* resolveRequestedRpcModel: the optional provider/modelId pair; -1 + *err when invalid */
static int requested_model(const json_t *cmd, const ai_model_t **out, char **err) {
    const char *p = json_get_str(cmd, "provider"), *mid = json_get_str(cmd, "modelId");
    *out = NULL;
    if (!json_get(cmd, "provider") && !json_get(cmd, "modelId")) return 0;
    if (!json_get(cmd, "provider") || !json_get(cmd, "modelId")) {
        *err = xstrdup("provider and modelId must be given together");
        return -1;
    }
    const ai_model_t *m = p && mid ? catalog_find(p, mid) : NULL;
    if (!m || !catalog_model_available(m) || !str_eq(m->provider, p) || !str_eq(m->id, mid)) {
        *err = xasprintf("Model not found: %s/%s", p ? p : "undefined", mid ? mid : "undefined");
        return -1;
    }
    *out = m;
    return 0;
}


/* ---------- subagent frames and commands (port of modes/rpc/rpc-subagents.ts) ---------- */

#define RPC_SUB_RETAINED 256

/* insert-or-move `key` to the newest end of an ordered set object, keeping at most `max` entries */
static void sub_add_pruned(json_t *set, const char *key, json_t *val, size_t max) {
    json_del(set, key);
    json_set(set, key, val);
    while (json_len(set) > max) json_del(set, set->u.obj.keys[0]);
}

static bool sub_same_owner(const json_t *payload, const json_t *snap) {
    const char *pa = json_get_str(payload, "parentToolCallId"), *sa = json_get_str(snap, "parentToolCallId");
    if (pa && sa) return str_eq(pa, sa);
    const char *pf = json_get_str(payload, "sessionFile"), *sf = json_get_str(snap, "sessionFile");
    if (pf && sf) return str_eq(pf, sf);
    return true;
}

static void sub_frame(rpc_t *r, const char *type, const json_t *payload) {
    json_t *f = json_obj();
    json_set_str(f, "type", type);
    json_set(f, "payload", json_clone(payload));
    emit(r, f);
    json_free(f);
}

/* handleLifecycle / handleProgress / handleEvent, on the reporting agent's thread */
static void rpc_sub_observe(void *ud, const char *channel, const json_t *payload) {
    rpc_t *r = ud;
    pthread_mutex_lock(&r->sub_mu);
    int level = r->sub_level;
    bool out = false;
    if (str_eq(channel, "lifecycle")) {
        const char *id = json_get_str(payload, "id"), *st = json_get_str(payload, "status");
        json_t *existing = json_get(r->subs, id);
        if ((existing && !sub_same_owner(payload, existing)) || (!existing && !str_eq(st, "started"))) {
            pthread_mutex_unlock(&r->sub_mu);
            return;
        }
        if (str_eq(st, "started")) json_del(r->sub_stale, id);
        const char *sf = json_get_str(payload, "sessionFile");
        if (!sf) sf = json_get_str(existing, "sessionFile");
        json_t *snap = json_obj();
        json_set_str(snap, "id", id);
        json_set_int(snap, "index", json_get_int(payload, "index", 0));
        json_set_str(snap, "agent", json_get_str(payload, "agent"));
        json_set_str(snap, "agentSource", json_get_str(payload, "agentSource"));
        const char *desc = json_get_str(payload, "description");
        if (!desc) desc = json_get_str(existing, "description");
        if (desc) json_set_str(snap, "description", desc);
        json_set_str(snap, "status", str_eq(st, "started") ? "running" : st);
        if (json_get_str(existing, "task")) json_set_str(snap, "task", json_get_str(existing, "task"));
        if (json_get_str(existing, "assignment")) json_set_str(snap, "assignment", json_get_str(existing, "assignment"));
        if (sf) json_set_str(snap, "sessionFile", sf);
        const char *ptc = json_get_str(payload, "parentToolCallId");
        if (!ptc) ptc = json_get_str(existing, "parentToolCallId");
        if (ptc) json_set_str(snap, "parentToolCallId", ptc);
        json_set_int(snap, "lastUpdate", now_ms());
        if (json_get(existing, "progress")) json_set(snap, "progress", json_clone(json_get(existing, "progress")));
        if (sf) sub_add_pruned(r->sub_files, id, json_str(sf), RPC_SUB_RETAINED);
        if (!str_eq(st, "started")) {
            json_del(r->subs, id);
            json_free(snap);
        } else json_set(r->subs, id, snap);
        out = level > 0;
    } else if (str_eq(channel, "progress")) {
        const json_t *pr = json_get(payload, "progress");
        const char *id = json_get_str(pr, "id");
        json_t *existing = id ? json_get(r->subs, id) : NULL;
        if (!id || json_get(r->sub_stale, id) || !existing || !sub_same_owner(payload, existing)) {
            pthread_mutex_unlock(&r->sub_mu);
            return;
        }
        const char *sf = json_get_str(payload, "sessionFile");
        if (!sf) sf = json_get_str(existing, "sessionFile");
        if (sf) sub_add_pruned(r->sub_files, id, json_str(sf), RPC_SUB_RETAINED);
        json_t *snap = json_obj();
        json_set_str(snap, "id", id);
        json_set_int(snap, "index", json_get_int(payload, "index", 0));
        json_set_str(snap, "agent", json_get_str(payload, "agent"));
        json_set_str(snap, "agentSource", json_get_str(payload, "agentSource"));
        const char *desc = json_get_str(pr, "description");
        if (!desc) desc = json_get_str(existing, "description");
        if (desc) json_set_str(snap, "description", desc);
        json_set_str(snap, "status", json_get_str(pr, "status"));
        if (json_get_str(payload, "task")) json_set_str(snap, "task", json_get_str(payload, "task"));
        if (json_get_str(payload, "assignment")) json_set_str(snap, "assignment", json_get_str(payload, "assignment"));
        if (sf) json_set_str(snap, "sessionFile", sf);
        json_set_int(snap, "lastUpdate", now_ms());
        const char *ptc = json_get_str(payload, "parentToolCallId");
        if (!ptc) ptc = json_get_str(existing, "parentToolCallId");
        if (ptc) json_set_str(snap, "parentToolCallId", ptc);
        json_set(snap, "progress", json_clone(pr));
        json_set(r->subs, id, snap);
        out = level > 0;
    } else if (str_eq(channel, "event")) {
        out = level == 2 && !json_get(r->sub_stale, json_get_str(payload, "id"));
    }
    pthread_mutex_unlock(&r->sub_mu);
    if (out)
        sub_frame(r, str_eq(channel, "lifecycle") ? "subagent_lifecycle" : str_eq(channel, "progress") ? "subagent_progress" : "subagent_event",
                  payload);
}

/* session changed: every known subagent is stale (its late frames are dropped) */
static void rpc_sub_clear(rpc_t *r) {
    pthread_mutex_lock(&r->sub_mu);
    const char *k;
    json_t *v;
    json_obj_foreach(r->subs, i, k, v) sub_add_pruned(r->sub_stale, k, json_bool(true), RPC_SUB_RETAINED);
    json_obj_foreach(r->sub_files, i, k, v) sub_add_pruned(r->sub_stale, k, json_bool(true), RPC_SUB_RETAINED);
    UNUSED(v);
    json_free(r->subs);
    json_free(r->sub_files);
    r->subs = json_obj();
    r->sub_files = json_obj();
    pthread_mutex_unlock(&r->sub_mu);
}

static int sub_cmp(const void *a, const void *b) {
    const json_t *x = *(const json_t *const *)a, *y = *(const json_t *const *)b;
    int64_t d = json_get_int(x, "index", 0) - json_get_int(y, "index", 0);
    if (d) return d < 0 ? -1 : 1;
    return strcmp(json_get_str(x, "id"), json_get_str(y, "id"));
}

static json_t *rpc_sub_list(rpc_t *r) {
    pthread_mutex_lock(&r->sub_mu);
    size_t n = json_len(r->subs);
    json_t **v = xcalloc(n + 1, sizeof *v);
    const char *k;
    json_t *s;
    json_obj_foreach(r->subs, i, k, s) v[i] = s;
    UNUSED(k);
    xqsort(v, n, sizeof *v, sub_cmp);
    json_t *arr = json_arr();
    for (size_t i = 0; i < n; i++) json_push(arr, json_clone(v[i]));
    free(v);
    pthread_mutex_unlock(&r->sub_mu);
    return arr;
}

/* resolveOwnedLiveSubagent: a running subagent this session lists, still running in the registry */
static bool rpc_sub_owned_live(rpc_t *r, const char *id) {
    pthread_mutex_lock(&r->sub_mu);
    json_t *snap = json_get(r->subs, id);
    const char *st = json_get_str(snap, "status");
    bool listed = snap && (str_eq(st, "running") || str_eq(st, "pending"));
    pthread_mutex_unlock(&r->sub_mu);
    if (!listed) return false;
    json_t *ref = agents_get(id);
    bool ok = ref && str_eq(json_get_str(ref, "status"), "running") && json_get_bool(ref, "live", false);
    json_free(ref);
    return ok;
}

/* readRpcSubagentTranscript */
static json_t *rpc_sub_transcript(const char *file, int64_t from) {
    int64_t start = from > 0 ? from : 0;
    size_t size = 0;
    char *data = fs_read(file, &size);
    json_t *d = json_obj();
    json_set_str(d, "sessionFile", file);
    bool reset = false;
    if (!data) {
        json_set_int(d, "fromByte", start);
        json_set_int(d, "nextByte", start);
        json_set_bool(d, "reset", false);
        json_set(d, "entries", json_arr());
        json_set(d, "messages", json_arr());
        return d;
    }
    if (start > (int64_t)size) start = 0, reset = true;
    const char *text = data + start;
    size_t len = size - (size_t)start;
    size_t complete = 0;
    for (size_t i = len; i > 0; i--)
        if (text[i - 1] == '\n') {
            complete = i;
            break;
        }
    json_t *entries = json_arr(), *messages = json_arr();
    size_t pos = 0;
    while (pos < complete) {
        const char *nl = memchr(text + pos, '\n', complete - pos);
        size_t ll = (size_t)(nl - (text + pos));
        json_t *e = ll ? json_parse(text + pos, ll, NULL) : NULL;
        if (json_is_obj(e)) {
            if (str_eq(json_get_str(e, "type"), "message") && json_get(e, "message")) json_push(messages, json_clone(json_get(e, "message")));
            json_push(entries, e);
        } else json_free(e);
        pos += ll + 1;
    }
    json_set_int(d, "fromByte", start);
    json_set_int(d, "nextByte", start + (int64_t)complete);
    json_set_bool(d, "reset", reset);
    json_set(d, "entries", entries);
    json_set(d, "messages", messages);
    free(data);
    return d;
}

/* set_subagent_subscription, get_subagents, get_subagent_messages, cancel_subagent, steer_subagent; false when
 * `type` is none of them */
static bool rpc_sub_command(rpc_t *r, const json_t *cmd, const char *id, const char *type) {
    if (str_eq(type, "set_subagent_subscription")) {
        const char *lv = json_get_str(cmd, "level");
        int level = str_eq(lv, "off") ? 0 : str_eq(lv, "progress") ? 1 : str_eq(lv, "events") ? 2 : -1;
        if (level < 0) {
            json_t *l = json_get(cmd, "level");
            char *s = !l ? xstrdup("undefined") : json_is_str(l) ? xstrdup(json_sv(l)) : json_dump_js(l);
            char *m = xasprintf("Invalid subagent subscription level: %s", s);
            respond(r, id, type, false, NULL, m);
            free(m), free(s);
            return true;
        }
        pthread_mutex_lock(&r->sub_mu);
        r->sub_level = level;
        pthread_mutex_unlock(&r->sub_mu);
        agents_observe(rpc_sub_observe, r, level == 2);
        json_t *d = json_obj();
        json_set_str(d, "level", lv);
        respond(r, id, type, true, d, NULL);
        return true;
    }
    if (str_eq(type, "get_subagents")) {
        json_t *d = json_obj();
        json_set(d, "subagents", rpc_sub_list(r));
        respond(r, id, type, true, d, NULL);
        return true;
    }
    if (str_eq(type, "get_subagent_messages")) {
        const json_t *fb = json_get(cmd, "fromByte");
        if (fb && !json_is_num(fb)) {
            respond(r, id, type, false, NULL, "fromByte must be a finite number");
            return true;
        }
        const char *sid = json_get_str(cmd, "subagentId"), *sfile = json_get_str(cmd, "sessionFile");
        char *file = NULL, *err = NULL;
        pthread_mutex_lock(&r->sub_mu);
        if (sid && *sid) {
            const char *f = json_get_str(json_get(r->subs, sid), "sessionFile");
            if (!f) f = json_sv(json_get(r->sub_files, sid));
            if (f) file = xstrdup(f);
            else err = xasprintf("Unknown subagent or session file unavailable: %s", sid);
        } else if (sfile && *sfile) {
            bool known = false;
            const char *k;
            json_t *v;
            json_obj_foreach(r->subs, i, k, v) known = known || str_eq(json_get_str(v, "sessionFile"), sfile);
            json_obj_foreach(r->sub_files, i, k, v) known = known || str_eq(json_sv(v), sfile);
            UNUSED(k);
            if (known) file = xstrdup(sfile);
            else err = xstrdup("Unknown subagent session file");
        } else err = xstrdup("get_subagent_messages requires subagentId or sessionFile");
        pthread_mutex_unlock(&r->sub_mu);
        if (err) respond(r, id, type, false, NULL, err);
        else respond(r, id, type, true, rpc_sub_transcript(file, fb ? json_d2i(json_get_num(cmd, "fromByte", 0)) : 0), NULL);
        free(file), free(err);
        return true;
    }
    if (str_eq(type, "cancel_subagent") || str_eq(type, "steer_subagent")) {
        const char *sid = json_get_str(cmd, "subagentId");
        if (!sid || !*sid) {
            respond(r, id, type, false, NULL, "`subagentId` must be a non-empty string.");
            return true;
        }
        if (str_eq(type, "cancel_subagent")) {
            bool cancelled = false;
            if (rpc_sub_owned_live(r, sid)) {
                char *err = NULL;
                cancelled = agents_kill(sid, &err);
                free(err);
            }
            json_t *d = json_obj();
            json_set_bool(d, "cancelled", cancelled);
            respond(r, id, type, true, d, NULL);
            return true;
        }
        const char *msg = json_get_str(cmd, "message");
        char *mt = msg ? str_trim(msg) : NULL;
        bool empty = !mt || !*mt;
        free(mt);
        if (empty) {
            respond(r, id, type, false, NULL, "`message` is required for steer_subagent.");
            return true;
        }
        if (!rpc_sub_owned_live(r, sid)) {
            char *m = xasprintf("Subagent not running: %s", sid);
            respond(r, id, type, false, NULL, m);
            free(m);
            return true;
        }
        char *err = NULL;
        if (agents_prompt(sid, msg, &err)) respond(r, id, type, true, NULL, NULL);
        else {
            char *m = xasprintf("Subagent refused the message: %s", err ? err : "unknown error");
            respond(r, id, type, false, NULL, m);
            free(m);
        }
        free(err);
        return true;
    }
    return false;
}

static void session_change(rpc_t *r, const json_t *cmd, const char *id, const char *type) {
    app_t *a = r->a;
    bool busy = r->running || agent_is_running(a->agent) || a->compacting;
    if (!strcmp(type, "fork") && busy) {
        respond_code(r, id, type, false, NULL,
                     "Cannot fork the session while session maintenance or user work is still running", "session_busy");
        return;
    }
    const ai_model_t *model = NULL;
    char *err = NULL;
    if ((!strcmp(type, "switch_session") || !strcmp(type, "open_session")) && requested_model(cmd, &model, &err) != 0) {
        respond(r, id, type, false, NULL, err);
        free(err);
        return;
    }
    const char *eid = json_get_str(cmd, "entryId");
    const json_t *entry = eid ? session_get(a->session, eid) : NULL;
    if (!strcmp(type, "branch") && (!entry || !str_eq(json_get_str(entry, "type"), "message") ||
                                    !str_eq(json_get_str(json_get(entry, "message"), "role"), "user"))) {
        respond(r, id, type, false, NULL, "Invalid entry ID for branching");
        return;
    }
    if (!strcmp(type, "fork") && eid && !str_eq(json_get_str(entry, "type"), "message")) {
        char *m = xasprintf("Invalid entry ID for forking: %s", eid);
        respond(r, id, type, false, NULL, m);
        free(m);
        return;
    }
    if (!strcmp(type, "open_session") && (!a->session->file || !a->session->persist || a->opts.no_session)) {
        respond(r, id, type, false, NULL, "open_session requires session persistence (omit --no-session)");
        return;
    }
    /* the side question of the old session must not outlive it */
    if (btw_close(r, &err) != 0) {
        respond(r, id, type, false, NULL, err);
        free(err);
        return;
    }
    /* new/switch detach a live run: stop it first */
    if (busy && strcmp(type, "branch") && strcmp(type, "fork")) {
        app_abort(a);
        for (int k = 0; k < 600; k++) {
            pthread_mutex_lock(&r->mu);
            bool running = r->running;
            pthread_mutex_unlock(&r->mu);
            if (!running && !agent_is_running(a->agent)) break;
            usleep(100 * 1000);
        }
    }
    int rc = 0;
    json_t *d = json_obj();
    if (!strcmp(type, "new_session")) {
        rc = app_new_session(a);
        json_set_bool(d, "cancelled", rc != 0);
    } else if (!strcmp(type, "switch_session")) {
        const char *path = json_get_str(cmd, "sessionPath");
        rc = path ? app_switch_session(a, path, &err) : (err = xstrdup("sessionPath must be a string"), -1);
        if (rc == 0 && model && model != a->model) app_use_model(a, model);
        json_set_bool(d, "cancelled", false);
    } else if (!strcmp(type, "branch")) {
        char *text = ai_message_text(json_get(entry, "message"));
        const char *parent = json_get_str(entry, "parentId");
        rc = parent ? app_fork_from(a, parent, &err) : app_new_session(a);
        json_set_str(d, "text", text ? text : "");
        json_set_bool(d, "cancelled", false);
        free(text);
    } else if (!strcmp(type, "fork")) {
        if (eid) rc = app_fork_from(a, eid, &err);
        else rc = app_fork_from(a, a->session->leaf_id, &err);
        json_set_bool(d, "cancelled", false);
    } else { /* open_session: continue the newest non-empty session in sessionDir, or start one there */
        const char *dir = json_get_str(cmd, "sessionDir");
        char *abs = path_abs(dir ? dir : ".", a->cwd);
        session_list_t list = {0};
        session_list(abs, &list);
        const char *latest = NULL;
        for (size_t i = 0; i < list.n && !latest; i++)
            if (list.v[i].message_count > 0) latest = list.v[i].path;
        char *cur_dir = path_dirname(a->session->file);
        bool already = latest ? str_eq(latest, a->session->file) : str_eq(cur_dir, abs) && json_len(a->agent->messages) == 0;
        free(cur_dir);
        bool resumed = false;
        if (!already) {
            if (latest) {
                rc = app_switch_session(a, latest, &err);
                resumed = rc == 0;
            } else {
                free(r->open_dir);
                r->open_dir = xstrdup(abs);
                const char *saved = a->opts.session_dir;
                a->opts.session_dir = r->open_dir;
                rc = app_new_session(a);
                a->opts.session_dir = saved;
            }
        } else resumed = latest != NULL;
        if (rc == 0 && model && model != a->model) app_use_model(a, model);
        session_list_free(&list);
        free(abs);
        json_set_bool(d, "cancelled", false);
        json_set_bool(d, "resumed", rc == 0 && resumed);
        json_set_str(d, "sessionId", session_id(a->session));
        if (a->session->file) json_set_str(d, "sessionFile", a->session->file);
    }
    if (rc != 0) {
        json_free(d);
        respond(r, id, type, false, NULL, err ? err : "session change failed");
        free(err);
        return;
    }
    free(err);
    if (!json_get_bool(d, "cancelled", false)) rpc_sub_clear(r);
    app_queue_changed(a);
    emit_available_commands(r);
    respond(r, id, type, true, d, NULL);
}

static void rpc_abort_user(app_t *a) {
    cmp_bg_abort(a); /* session.abort cancels a background compaction too */
    agent_abort_user(a->agent);
}

/* `abort`: session.abort waits for the agent to go idle (its agent_end is out) before the reply */
typedef struct {
    rpc_t *r;
    char *id;
    const char *type;
    json_t *data; /* the reply's data (abort_and_restore_queue: the restored input) */
    bool live;
    unsigned ends;
} abort_job_t;

static void *abort_main(void *p) {
    abort_job_t *j = p;
    rpc_t *r = j->r;
    if (j->live) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += 60;
        pthread_mutex_lock(&r->mu);
        while (r->agent_ends == j->ends && pthread_cond_timedwait(&r->abort_cv, &r->mu, &dl) == 0) {}
        pthread_mutex_unlock(&r->mu);
    }
    respond(r, j->id, j->type, true, j->data, NULL);
    pthread_mutex_lock(&r->mu);
    r->abort_waiters--;
    pthread_cond_broadcast(&r->abort_cv);
    pthread_mutex_unlock(&r->mu);
    free(j->id);
    free(j);
    return NULL;
}

/* abort the run with `kick` and answer `type` (with `data`, owned) once it unwound */
static void abort_start(rpc_t *r, const char *id, const char *type, json_t *data, void (*kick)(app_t *a)) {
    abort_job_t *j = xcalloc(1, sizeof *j);
    j->r = r;
    j->id = id ? xstrdup(id) : NULL;
    j->type = type;
    j->data = data;
    pthread_mutex_lock(&r->mu);
    r->abort_waiters++;
    j->ends = r->agent_ends;
    pthread_mutex_unlock(&r->mu);
    j->live = agent_is_running(r->a->agent);
    kick(r->a);
    pthread_t th;
    if (pthread_create(&th, NULL, abort_main, j) != 0) {
        abort_main(j);
        return;
    }
    pthread_mutex_lock(&r->bg_mu);
    vec_push(&r->bg_threads, th);
    pthread_mutex_unlock(&r->bg_mu);
}

static void handle(rpc_t *r, json_t *cmd) {
    app_t *a = r->a;
    const char *id = json_get_str(cmd, "id");
    const char *type = json_get_str(cmd, "type");
    if (!type) {
        respond(r, id, "undefined", false, NULL, "Unknown command: undefined");
        return;
    }
    /* RpcInputDispatcher: control frames and BACKGROUND_COMMANDS overtake the serial queue; everything else waits
     * behind a pending `abort` */
    if (!str_eq(type, "extension_ui_response") && !str_eq(type, "host_tool_result") && !str_eq(type, "host_tool_update") &&
        !str_eq(type, "host_uri_result") && !str_eq(type, "bash") && !str_eq(type, "btw_cancel") &&
        !str_eq(type, "abort_branch_summary") && !str_eq(type, "predict_word") && !str_eq(type, "live_start") &&
        !str_eq(type, "live_connect"))
        abort_barrier(r);
    { /* set_fast_mode / set_slow_mode / set_cache_warming (modelctl.c) */
        char *err = NULL;
        bool handled = false;
        json_t *d = modelctl_rpc(a, type, cmd, &handled, &err);
        if (handled) {
            respond(r, id, type, !err, d, err);
            free(err);
            return;
        }
    }
    if (!strcmp(type, "negotiate_protocol")) {
        const json_t *pv = json_get(cmd, "protocolVersion");
        if (!(json_is_num(pv) && jnum(pv) == 2) && !str_eq(json_sv(pv), "2")) {
            char *v = !pv ? xstrdup("undefined") : json_is_str(pv) ? xstrdup(json_sv(pv)) : json_dump_js(pv);
            char *m = xasprintf("Unsupported RPC protocol version: %s", v);
            respond(r, id, type, false, NULL, m);
            free(m);
            free(v);
        } else {
            json_t *d = json_obj();
            json_set_int(d, "protocolVersion", 2);
            respond(r, id, type, true, d, NULL);
            pthread_mutex_lock(&r->out_mu);
            r->proto = 2;
            pthread_mutex_unlock(&r->out_mu);
        }
    } else if (!strcmp(type, "prompt") || !strcmp(type, "abort_and_prompt")) {
        const char *msg = json_get_str(cmd, "message");
        if (!strcmp(type, "abort_and_prompt")) {
            app_abort(a);
            for (int k = 0; k < 600; k++) { /* wait for the run to unwind (max ~60s) */
                pthread_mutex_lock(&r->mu);
                bool busy = r->running;
                pthread_mutex_unlock(&r->mu);
                if (!busy) break;
                usleep(100 * 1000);
            }
        }
        /* dispatchRpcSkillPrompt: a runnable /skill: invocation is read before the command is acknowledged */
        json_t *skill_msg = NULL;
        {
            skill_invocation_t inv;
            if (app_skill_command(a, msg, &inv)) {
                skill_invocation_free(&inv);
                char *serr = NULL;
                json_t *imgs = json_get_arr(cmd, "images");
                skill_msg = app_skill_message(a, msg, imgs ? json_clone(imgs) : NULL, &serr);
                if (!skill_msg) {
                    respond(r, id, type, false, NULL, serr ? serr : "Failed to load skill");
                    free(serr);
                    return;
                }
            }
        }
        pthread_mutex_lock(&r->mu);
        /* a run past its agent_end is only reporting (prompt_result, after-run checks) and cleaning up: omp's agent
         * is idle by then, so wait for it and start a run, instead of steering into one that never takes the prompt */
        for (int k = 0; r->running && r->settling && k < 300; k++) {
            pthread_mutex_unlock(&r->mu);
            usleep(10 * 1000);
            pthread_mutex_lock(&r->mu);
        }
        if (r->running) {
            pthread_mutex_unlock(&r->mu);
            const char *behav = json_get_str(cmd, "streamingBehavior");
            json_t *m = skill_msg;
            if (!m) {
                char *x = app_expand_input(a, msg ? msg : "", true);
                if (!x) {
                    respond(r, id, type, true, NULL, NULL);
                    return;
                }
                m = msg_user_typed(content_with_images(x, json_get_arr(cmd, "images")));
                free(x);
            }
            if (str_eq(behav, "followUp")) agent_follow_up(a->agent, m);
            else agent_steer(a->agent, m);
            pthread_mutex_lock(&r->mu);
            if (!r->waiting) r->waiting = json_arr();
            json_push(r->waiting, id ? json_str(id) : json_null());
            pthread_mutex_unlock(&r->mu);
            respond(r, id, type, true, NULL, NULL);
            return;
        }
        if (r->th) pthread_join(r->th, NULL);
        r->th = 0;
        r->running = true;
        r->settling = false;
        r->hidden = NULL;
        r->text = xstrdup(msg ? msg : "");
        r->prompt_id = id ? xstrdup(id) : NULL;
        json_t *imgs = json_get_arr(cmd, "images");
        r->images = imgs && !skill_msg ? json_clone(imgs) : NULL;
        r->skill_msg = skill_msg;
        slash_parsed_t sp = {0};
        const slash_cmd_t *sc = !skill_msg && slash_parse(r->text, &sp) ? slash_lookup(sp.name) : NULL;
        if (sp.name) slash_parsed_free(&sp);
        r->slash = sc && sc->handle;
        r->slash_reply = r->slash && !strcmp(type, "prompt");
        pthread_mutex_unlock(&r->mu);
        if (r->slash_reply) {
            /* answered by the prompt thread once the builtin ran (its command_output first) */
        } else if (r->slash) {
            json_t *d = json_obj();
            json_set_bool(d, "agentInvoked", slash_invokes_agent(a, sc->name));
            respond(r, id, type, true, d, NULL);
        } else respond(r, id, type, true, NULL, NULL);
        if (pthread_create(&r->th, NULL, prompt_thread, r) != 0) {
            r->running = false;
            respond(r, id, type, false, NULL, "failed to start agent thread");
        }
    } else if (!strcmp(type, "abort_and_deliver_queue")) {
        /* the TUI's empty Enter (omp 82e8e939a7): a streaming run with queued user input is interrupted and continues
         * with that input; ordered behind earlier input, so a prompt sent just before is the input it delivers */
        pthread_mutex_lock(&r->mu);
        bool running = r->running && !r->settling;
        pthread_mutex_unlock(&r->mu);
        bool aborted = running && agent_has_interruptible_input(a->agent);
        if (aborted) {
            agent_abort_deliver_queued(a->agent);
            app_abort_user(a);
        }
        json_t *d = json_obj();
        json_set_bool(d, "aborted", aborted);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "resume")) {
        /* the TUI's `.`/`c` continue (AgentSession.resume): a running turn is interrupted first; queued input continues
         * the run in place of the directive; answered once the continuation is under way */
        pthread_mutex_lock(&r->mu);
        bool running = r->running;
        pthread_mutex_unlock(&r->mu);
        if (running && agent_has_interruptible_input(a->agent)) {
            agent_abort_deliver_queued(a->agent);
            app_abort_user(a);
            respond(r, id, type, true, NULL, NULL);
            return;
        }
        if (running) {
            app_abort_user(a);
            for (int k = 0; k < 600; k++) { /* wait for the run to unwind (max ~60s) */
                pthread_mutex_lock(&r->mu);
                bool busy = r->running;
                pthread_mutex_unlock(&r->mu);
                if (!busy) break;
                usleep(100 * 1000);
            }
        }
        bool has_user = false;
        pthread_mutex_lock(&a->agent->mu);
        const json_t *m;
        json_arr_foreach(a->agent->messages, mi, m) if (str_eq(json_get_str(m, "role"), "user")) has_user = true;
        pthread_mutex_unlock(&a->agent->mu);
        pthread_mutex_lock(&r->mu);
        if (!has_user || r->running) {
            pthread_mutex_unlock(&r->mu);
            respond(r, id, type, false, NULL, "Nothing to resume");
            return;
        }
        if (r->th) pthread_join(r->th, NULL);
        r->th = 0;
        r->running = true;
        r->settling = false;
        r->hidden = NULL;
        r->resume = true;
        r->text = xstrdup("");
        r->prompt_id = NULL;
        r->images = NULL;
        r->skill_msg = NULL;
        r->slash = r->slash_reply = false;
        pthread_mutex_unlock(&r->mu);
        respond(r, id, type, true, NULL, NULL);
        if (pthread_create(&r->th, NULL, prompt_thread, r) != 0) {
            r->running = false;
            r->resume = false;
        }
    } else if (!strcmp(type, "steer") || !strcmp(type, "follow_up")) {
        /* steer() / followUp(): prompt templates expand (no slash or skill commands) */
        const char *raw = json_get_str(cmd, "message");
        char *x = app_expand_templates(a, raw ? raw : "");
        json_t *m = msg_user_typed(content_with_images(x, json_get_arr(cmd, "images")));
        free(x);
        if (!strcmp(type, "steer")) agent_steer(a->agent, m);
        else agent_follow_up(a->agent, m);
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "abort")) {
        abort_start(r, id, "abort", NULL, rpc_abort_user); /* session.abort({reason: USER_INTERRUPT_LABEL}) */
    } else if (!strcmp(type, "goal")) {
        const char *op = json_get_str(cmd, "op");
        char *err = NULL;
        int rc = 0;
        int64_t budget = json_get_int(cmd, "token_budget", 0);
        if (str_eq(op, "create")) rc = app_goal_create(a, json_get_str(cmd, "objective"), budget, &err);
        else if (str_eq(op, "resume")) rc = app_goal_resume(a, &err);
        else if (str_eq(op, "pause")) rc = app_goal(a) ? app_goal_pause(a) : (err = xstrdup("No goal."), -1);
        else if (str_eq(op, "drop")) app_goal_drop(a);
        else if (!str_eq(op, "get")) rc = (err = xasprintf("unknown goal op '%s'", op ? op : ""), -1);
        if (rc) {
            respond(r, id, type, false, NULL, err ? err : "goal operation failed");
        } else {
            json_t *d = json_obj();
            json_set(d, "goal", goal_json(app_goal(a)));
            json_set(d, "state", goal_state_json(app_goal(a)));
            respond(r, id, type, true, d, NULL);
        }
        free(err);
    } else if (!strcmp(type, "extension_ui_response")) {
        if (id) ui_answer(r, cmd);
    } else if (!strcmp(type, "set_ask_dialog")) {
        r->ask_dialog = json_get_bool(cmd, "enabled", false) && json_is_bool(json_get(cmd, "enabled"));
        json_t *d = json_obj();
        json_set_bool(d, "enabled", r->ask_dialog);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "host_tool_result")) {
        host_tool_complete(r, cmd);
    } else if (!strcmp(type, "host_tool_update")) {
        host_tool_progress(r, cmd);
        /* partial results are informational for the host UI; nothing to do server-side */
    } else if (!strcmp(type, "set_host_tools")) {
        json_t *ht;
        char *verr = NULL;
        json_arr_foreach(json_get_arr(cmd, "tools"), i, ht) { /* normalizeHostToolDefinitions */
            char *nm = str_trim(json_get_str(ht, "name") ? json_get_str(ht, "name") : "");
            char *ds = str_trim(json_get_str(ht, "description") ? json_get_str(ht, "description") : "");
            if (!*nm) verr = xasprintf("Host tool at index %zu must provide a non-empty name", i);
            else if (!*ds) verr = xasprintf("Host tool \"%s\" must provide a non-empty description", nm);
            else if (json_get(ht, "exclusive") && !json_is_bool(json_get(ht, "exclusive"))) verr = xasprintf("Host tool \"%s\" exclusive must be a boolean", nm);
            else if (!json_is_obj(json_get(ht, "parameters"))) verr = xasprintf("Host tool \"%s\" must provide a JSON Schema object", nm);
            free(nm);
            free(ds);
            if (verr) break;
        }
        if (verr) {
            respond(r, id, type, false, NULL, verr);
            free(verr);
            return;
        }
        for (size_t i = 0; i < r->host_tools.n; i++) agent_remove_tool(a->agent, r->host_tools.v[i]);
        strvec_free(&r->host_tools);
        json_t *names = json_arr(), *t;
        json_arr_foreach(json_get_arr(cmd, "tools"), i, t) {
            const char *nm = json_get_str(t, "name");
            if (!nm || !*nm) continue;
            if (agent_find_tool(a->agent, nm)) agent_remove_tool(a->agent, nm);
            /* a raw JSON Schema: providers see it after omp's wire pass (toolWireSchema) */
            json_t *params = json_get(t, "parameters");
            agent_tool_t *tool = agent_tool_new(nm, json_get_str(t, "label") ? json_get_str(t, "label") : nm,
                                                json_get_str(t, "description") ? json_get_str(t, "description") : "",
                                                params ? ts_wire_schema(params) : json_parse_str("{\"type\":\"object\",\"properties\":{}}"),
                                                host_tool_execute);
            tool->strict = TOOL_STRICT; /* RpcHostToolAdapter */
            tool->hidden = json_get_bool(t, "hidden", false);
            tool->exclusive = json_get_bool(t, "exclusive", false); /* omc-only: runs alone, in message order */
            tool->ud = r;
            agent_add_tool(a->agent, tool);
            strvec_push(&r->host_tools, nm);
            json_push_str(names, nm);
        }
        app_rebuild_system_prompt(a);
        json_t *d = json_obj();
        json_set(d, "toolNames", names);
        respond(r, id, type, true, d, NULL);
    } else if (rpc_sub_command(r, cmd, id, type)) {
        /* subagent commands (rpc-subagents.ts) */
    } else if (!strcmp(type, "set_event_filter")) {
        json_t *ev = json_get(cmd, "events"), *x;
        bool ok = json_is_null(ev) && ev;
        if (json_is_arr(ev)) {
            ok = true;
            json_arr_foreach(ev, i, x) ok = ok && json_is_str(x) && *json_sv(x);
        }
        const json_t *mu = json_get(cmd, "messageUpdates");
        const char *upd = mu ? json_sv(mu) : "full";
        if (!ok) respond(r, id, type, false, NULL, "events must be null or an array of non-empty event type strings");
        else if (!str_eq(upd, "full") && !str_eq(upd, "delta")) respond(r, id, type, false, NULL, "messageUpdates must be \"full\" or \"delta\"");
        else {
            pthread_mutex_lock(&r->filter_mu);
            strvec_free(&r->filter);
            r->filter_on = json_is_arr(ev);
            json_t *sel = json_arr();
            json_arr_foreach(ev, i, x) if (!strvec_contains(&r->filter, json_sv(x))) {
                strvec_push(&r->filter, json_sv(x));
                json_push_str(sel, json_sv(x));
            }
            r->delta = str_eq(upd, "delta");
            pthread_mutex_unlock(&r->filter_mu);
            json_t *d = json_obj();
            if (r->filter_on) json_set(d, "events", sel);
            else {
                json_free(sel);
                json_set(d, "events", json_null());
            }
            json_set_str(d, "messageUpdates", upd);
            respond(r, id, type, true, d, NULL);
        }
    } else if (!strcmp(type, "set_todos")) {
        json_t *ph = json_get_arr(cmd, "phases");
        json_free(a->env->todo.phases);
        a->env->todo.phases = ph ? json_clone(ph) : json_arr();
        json_t *ce = json_obj(); /* USER_TODO_EDIT_CUSTOM_TYPE entry */
        json_set(ce, "phases", json_clone(a->env->todo.phases));
        session_append_custom(a->session, "user_todo_edit", ce); /* takes ce */
        json_t *d = json_obj();
        json_set(d, "todoPhases", json_clone(a->env->todo.phases));
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_tree")) {
        json_t *d = json_obj();
        session_lock(a->session); /* commands run beside the prompt thread, which may be appending */
        json_set(d, "tree", session_tree_json(a->session));
        json_set(d, "leafId", a->session->leaf_id ? json_str(a->session->leaf_id) : json_null());
        session_unlock(a->session);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "navigate_tree")) {
        const char *eid = json_get_str(cmd, "entryId");
        json_t *e = eid ? session_get(a->session, eid) : NULL;
        if (!eid) respond(r, id, type, false, NULL, "entryId must be a string");
        else if (!e) {
            char *m = xasprintf("Entry %s not found", eid);
            respond_code(r, id, type, false, NULL, m, "entry_not_found");
            free(m);
        } else if (r->running || agent_is_running(a->agent) || a->compacting)
            respond_code(r, id, type, false, NULL,
                         "Cannot navigate the session tree while session maintenance or user work is still running",
                         "session_busy");
        else {
            app_nav_result_t nav;
            char *err = NULL;
            int rc = app_navigate_tree(a, eid, json_get_bool(cmd, "summarize", false), json_get_str(cmd, "customInstructions"),
                                       &nav, &err);
            if (rc < 0) respond(r, id, type, false, NULL, err);
            else {
                json_t *d = json_obj();
                json_set_bool(d, "cancelled", rc == 1);
                if (rc == 1) json_set_bool(d, "aborted", true);
                if (nav.editor_text) json_set_str(d, "editorText", nav.editor_text);
                respond(r, id, type, true, d, NULL);
            }
            free(nav.editor_text);
            free(err);
        }
    } else if (!strcmp(type, "set_label")) {
        const char *eid = json_get_str(cmd, "entryId");
        const json_t *lab = json_get(cmd, "label");
        if (!eid) respond(r, id, type, false, NULL, "entryId must be a string");
        else if (lab && !json_is_str(lab)) respond(r, id, type, false, NULL, "label must be a string");
        else if (!session_get(a->session, eid)) {
            char *m = xasprintf("Entry %s not found", eid);
            respond(r, id, type, false, NULL, m);
            free(m);
        } else {
            session_append_label(a->session, eid, lab ? json_sv(lab) : "");
            respond(r, id, type, true, NULL, NULL);
        }
    } else if (!strcmp(type, "get_branch_messages")) {
        json_t *path = session_current_path(a->session), *e, *arr = json_arr();
        json_arr_foreach(path, i, e) {
            json_t *m = json_get(e, "message");
            if (!str_eq(json_get_str(e, "type"), "message") || !str_eq(json_get_str(m, "role"), "user") || json_get_bool(m, "synthetic", false))
                continue;
            json_t *x = json_obj();
            json_set_str(x, "entryId", json_get_str(e, "id"));
            char *t = ai_message_text(m);
            json_set_str(x, "text", t);
            free(t);
            json_push(arr, x);
        }
        json_free(path);
        json_t *d = json_obj();
        json_set(d, "messages", arr);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_last_assistant_text")) {
        const json_t *last = last_assistant(a->agent->messages);
        json_t *d = json_obj();
        char *t = last ? ai_message_text(last) : NULL;
        json_set(d, "text", t ? json_str(t) : json_null());
        free(t);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_messages_page")) {
        const json_t *lim = json_get(cmd, "limit"), *curj = json_get(cmd, "cursor");
        if (r->running || agent_is_running(a->agent) || a->compacting) {
            respond_code(r, id, type, false, NULL, "Cannot page messages while the session is changing", "session_busy");
            return;
        }
        int64_t limit = RPC_PAGE_DEFAULT_LIMIT;
        if (lim) {
            if (!json_is_num(lim) || !lim->is_int || jnum(lim) < 1 || jnum(lim) > RPC_PAGE_MAX_LIMIT) {
                respond(r, id, type, false, NULL, "RPC message page limit must be between 1 and 256");
                return;
            }
            limit = (int64_t)jnum(lim);
        }
        pthread_mutex_lock(&a->agent->mu);
        int64_t total = (int64_t)json_len(a->agent->messages);
        const char *sid = session_id(a->session), *leaf = a->session->leaf_id;
        int64_t offset = 0;
        if (curj) {
            bool stale = false;
            offset = json_is_str(curj) ? page_decode(json_sv(curj), sid, leaf, total, &stale) : -1;
            if (offset < 0 || stale) {
                pthread_mutex_unlock(&a->agent->mu);
                if (offset < 0) respond(r, id, type, false, NULL, "Invalid RPC message cursor");
                else respond_code(r, id, type, false, NULL, "RPC message cursor is stale", "stale_cursor");
                return;
            }
        }
        json_t *page = json_arr();
        size_t bytes = 2;
        while (offset + (int64_t)json_len(page) < total && (int64_t)json_len(page) < limit) {
            const json_t *m = json_at(a->agent->messages, (size_t)(offset + (int64_t)json_len(page)));
            size_t mb = 0;
            free(dump_js(m, &mb));
            mb += json_len(page) ? 1 : 0;
            if (json_len(page) && bytes + mb > RPC_PAGE_MAX_BYTES) break;
            json_push(page, json_clone(m));
            bytes += mb;
        }
        int64_t next = offset + (int64_t)json_len(page);
        char *cursor = next < total ? page_cursor(sid, leaf, total, next) : NULL;
        pthread_mutex_unlock(&a->agent->mu);
        json_t *d = json_obj();
        json_set(d, "messages", page);
        if (cursor) json_set_str(d, "nextCursor", cursor);
        json_set_int(d, "totalMessages", total);
        free(cursor);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_available_thinking_levels")) {
        json_t *lv = json_arr();
        json_push_str(lv, "off");
        for (int i = EFFORT_OFF + 1; a->model && a->model->reasoning && i <= EFFORT_MAX; i++)
            if (model_supports_effort(a->model, (ai_effort_t)i)) json_push_str(lv, effort_name((ai_effort_t)i));
        json_t *d = json_obj();
        json_set(d, "levels", lv);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "cycle_thinking_level")) {
        /* cycleThinkingLevel: off -> auto -> supported efforts -> off */
        if (!a->model || !a->model->reasoning) {
            respond(r, id, type, true, json_null(), NULL);
        } else {
            if (a->auto_thinking) {
                int e = EFFORT_MINIMAL;
                while (e <= EFFORT_MAX && !model_supports_effort(a->model, (ai_effort_t)e)) e++;
                app_set_thinking(a, e > EFFORT_MAX ? EFFORT_OFF : (ai_effort_t)e);
            } else if (a->thinking == EFFORT_OFF) {
                app_set_thinking_auto(a);
            } else {
                int cur = (int)a->thinking, next = cur;
                for (int k = 1; k <= EFFORT_MAX + 1; k++) {
                    int c = (cur + k) % (EFFORT_MAX + 1);
                    if (model_supports_effort(a->model, (ai_effort_t)c)) {
                        next = c;
                        break;
                    }
                }
                app_set_thinking(a, (ai_effort_t)next);
            }
            json_t *d = json_obj();
            json_set_str(d, "level", a->auto_thinking ? "auto" : effort_name(a->thinking));
            respond(r, id, type, true, d, NULL);
        }
    } else if (!strcmp(type, "cycle_model")) {
        /* cycle through settings.enabledModels (scoped) or models with credentials */
        json_t *scope = settings_get("enabledModels");
        strvec_t cands = {0};
        json_t *x;
        if (json_len(scope)) {
            json_arr_foreach(scope, i, x) if (json_is_str(x)) strvec_push(&cands, json_sv(x));
        } else {
            for (size_t i = 0; i < catalog_count() && cands.n < 200; i++) {
                const ai_model_t *m = catalog_at(i);
                if (!catalog_model_available(m) || !str_eq(catalog_model_kind(m), "chat")) continue;
                char *l = xasprintf("%s/%s", m->provider, m->id);
                strvec_push_own(&cands, l);
            }
        }
        char *curl = app_model_label(a);
        size_t at = 0;
        for (size_t i = 0; i < cands.n; i++)
            if (!strcmp(cands.v[i], curl)) at = i + 1;
        char *err = NULL;
        if (cands.n < 2 || app_set_model(a, cands.v[at % cands.n], &err) != 0) {
            respond(r, id, type, true, json_null(), NULL);
        } else {
            json_t *d = json_obj();
            json_set(d, "model", model_json(a->model));
            if (app_wire_thinking(a)) json_set_str(d, "thinkingLevel", app_wire_thinking(a));
            json_set_bool(d, "isScoped", json_len(scope) > 0);
            respond(r, id, type, true, d, NULL);
        }
        free(err);
        free(curl);
        strvec_free(&cands);
    } else if (!strcmp(type, "export_html")) {
        /* session.exportToHtml(outputPath): the path as written */
        char *written = NULL, *err = NULL;
        if (app_export_html(a, json_get_str(cmd, "outputPath"), false, &written, &err) != 0)
            respond(r, id, type, false, NULL, err ? err : "export failed");
        else {
            json_t *d = json_obj();
            json_set_str(d, "path", written);
            respond(r, id, type, true, d, NULL);
        }
        free(written);
        free(err);
    } else if (!strcmp(type, "handoff")) {
        /* session.compact(focus, {mode: "handoff"}) in the background: the queue stays free for steering and abort */
        if (cmp_bg_active(a) || a->compacting) respond(r, id, type, false, NULL, "Compaction already in progress");
        else run_background(r, cmd, handoff_main);
    } else if (!strcmp(type, "btw")) {
        /* Side questions (/btw): answers stream as btw_delta / btw_record frames */
        char *err = NULL;
        json_t *rec = btw_ask(r, json_get_str(cmd, "question"), json_get_str(cmd, "recordId"), &err);
        if (!rec) respond(r, id, type, false, NULL, err ? err : "btw failed");
        else {
            json_t *d = json_obj();
            json_set(d, "record", rec);
            respond(r, id, type, true, d, NULL);
        }
        free(err);
    } else if (!strcmp(type, "btw_cancel")) {
        pthread_mutex_lock(&r->btw_mu);
        bool c = btw_cancel_locked(r, json_get_str(cmd, "recordId"));
        pthread_mutex_unlock(&r->btw_mu);
        json_t *d = json_obj();
        json_set_bool(d, "cancelled", c);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_btw_history")) {
        char *err = NULL;
        json_t *recs = btw_history(r, &err);
        if (!recs) respond(r, id, type, false, NULL, err ? err : "btw history unavailable");
        else {
            json_t *d = json_obj();
            json_set(d, "records", recs);
            respond(r, id, type, true, d, NULL);
        }
        free(err);
    } else if (!strcmp(type, "abort_and_restore_queue")) {
        json_t *d = json_obj();
        pthread_mutex_lock(&a->agent->mu);
        json_t *st = a->agent->steering ? a->agent->steering : json_arr();
        json_t *fu = a->agent->follow_up ? a->agent->follow_up : json_arr();
        json_t *sr = json_arr(), *fr = json_arr(), *m;
        for (int qi = 0; qi < 2; qi++) {
            json_arr_foreach(qi ? fu : st, i, m) {
                if (!agent_msg_user_authored(m)) continue; /* restores only user-authored input */
                json_t *x = json_obj(), *b, *imgs = NULL;
                char *t = ai_message_text(m);
                json_set_str(x, "text", t);
                free(t);
                json_arr_foreach(json_get_arr(m, "content"), k, b) if (str_eq(json_get_str(b, "type"), "image")) {
                    if (!imgs) imgs = json_arr();
                    json_push(imgs, json_clone(b));
                }
                if (imgs) json_set(x, "images", imgs);
                json_push(qi ? fr : sr, x);
            }
        }
        pthread_mutex_unlock(&a->agent->mu);
        agent_clear_queues(a->agent);
        json_set(d, "steering", sr);
        json_set(d, "followUp", fr);
        abort_start(r, id, "abort_and_restore_queue", d, app_abort);
    } else if (!strcmp(type, "remove_queued_message") || !strcmp(type, "promote_queued_message")) {
        const char *msg = json_get_str(cmd, "message");
        bool promote = !strcmp(type, "promote_queued_message");
        const char *qn = json_get_str(cmd, "queue");
        if (!msg) {
            respond(r, id, type, false, NULL, "message must be a string");
            return;
        }
        if (!promote && !str_eq(qn, "steering") && !str_eq(qn, "followUp")) {
            respond(r, id, type, false, NULL, "queue must be \"steering\" or \"followUp\"");
            return;
        }
        bool from_follow = promote || str_eq(qn, "followUp");
        bool done = false;
        json_t *removed_images = NULL;
        pthread_mutex_lock(&a->agent->mu);
        json_t *q = from_follow ? a->agent->follow_up : a->agent->steering;
        for (size_t i = 0; q && i < json_len(q) && !done; i++) {
            char *t = ai_message_text(json_at(q, i));
            if (str_eq(t, msg)) {
                json_t *m = json_arr_remove(q, i);
                if (promote) {
                    if (!a->agent->steering) a->agent->steering = json_arr();
                    json_push(a->agent->steering, m);
                } else {
                    json_t *b;
                    json_arr_foreach(json_get_arr(m, "content"), k, b) if (str_eq(json_get_str(b, "type"), "image")) {
                        if (!removed_images) removed_images = json_arr();
                        json_push(removed_images, json_clone(b));
                    }
                    json_free(m);
                }
                done = true;
            }
            free(t);
        }
        pthread_mutex_unlock(&a->agent->mu);
        if (done) agent_queue_changed(a->agent);
        json_t *d = json_obj();
        json_set_bool(d, promote ? "promoted" : "removed", done);
        if (removed_images) {
            size_t ib = 0;
            free(dump_js(removed_images, &ib));
            if (ib + 128 <= (r->proto == 2 ? RPC_MAX_REASSEMBLED_BYTES : RPC_MAX_FRAME_BYTES - 1)) json_set(d, "images", removed_images);
            else {
                json_free(removed_images);
                json_set_bool(d, "imagesDropped", true);
            }
        }
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "set_auto_compaction") || !strcmp(type, "set_auto_retry")) {
        settings_set_runtime(!strcmp(type, "set_auto_compaction") ? "compaction.enabled" : "retry.enabled",
                             json_bool(json_get_bool(cmd, "enabled", true)));
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "abort_retry") || !strcmp(type, "abort_bash") || !strcmp(type, "abort_branch_summary")) {
        if (!strcmp(type, "abort_retry")) app_abort(a);
        if (!strcmp(type, "abort_bash")) r->bash_cancel = 1;
        if (!strcmp(type, "abort_branch_summary")) app_abort_branch_summary(a);
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "get_available_commands")) {
        /* buildAvailableSlashCommands: builtins with a text-mode handler, skill, MCP prompt and file commands */
        json_t *arr = slash_available_commands(a);
        json_t *d = json_obj();
        json_set(d, "commands", arr);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_login_providers")) {
        json_t *arr = json_arr();
        json_t *provs = auth_oauth_providers();
        json_t *p;
        json_arr_foreach(provs, i, p) {
            if (!auth_login_listed(p)) continue; /* env-only providers have no login flow; paused ones offer none */
            json_t *x = json_obj();
            json_set_str(x, "id", json_get_str(p, "id"));
            json_set(x, "name", json_str_own(auth_login_name(p)));
            json_set_bool(x, "available", true);
            json_set_bool(x, "authenticated", auth_has_credentials(auth_store_id(json_get_str(p, "id"))));
            json_push(arr, x);
        }
        json_t *d = json_obj();
        json_set(d, "providers", arr);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "set_advisor")) {
        app_set_advisor(a, json_get_bool(cmd, "enabled", true));
        respond(r, id, type, true, NULL, NULL);
    } else if (!strcmp(type, "get_state")) {
        respond(r, id, type, true, state_json(r), NULL);
    } else if (!strcmp(type, "get_messages")) {
        json_t *d = json_obj();
        json_set(d, "messages", json_clone(a->agent->messages));
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "get_entries")) {
        /* selectRpcEntries: append order, entries strictly after `since` */
        const json_t *since = json_get(cmd, "since");
        size_t from = 0;
        bool found = !since;
        session_lock(a->session); /* commands run beside the prompt thread, which may be appending */
        if (since) {
            json_t *e;
            json_arr_foreach(a->session->entries, i, e) if (!found && json_is_str(since) && str_eq(json_get_str(e, "id"), json_sv(since))) {
                from = i + 1;
                found = true;
            }
        }
        if (!found) {
            session_unlock(a->session);
            char *v = json_is_str(since) ? xstrdup(json_sv(since)) : json_dump_js(since);
            char *m = xasprintf("Unknown entries cursor: %s", v);
            respond_code(r, id, type, false, NULL, m, "unknown_since");
            free(m);
            free(v);
        } else {
            json_t *d = json_obj(), *arr = json_arr();
            for (size_t i = from; i < json_len(a->session->entries); i++) json_push(arr, json_clone(json_at(a->session->entries, i)));
            json_set(d, "entries", arr);
            json_set(d, "leafId", a->session->leaf_id ? json_str(a->session->leaf_id) : json_null());
            session_unlock(a->session);
            respond(r, id, type, true, d, NULL);
        }
    } else if (!strcmp(type, "set_model")) {
        const char *p = json_get_str(cmd, "provider"), *mid = json_get_str(cmd, "modelId");
        const ai_model_t *m = p && mid ? catalog_find(p, mid) : NULL;
        if (!m || !catalog_model_available(m) || !str_eq(m->provider, p) || !str_eq(m->id, mid)) {
            char *e = xasprintf("Model not found: %s/%s", p ? p : "undefined", mid ? mid : "undefined");
            respond(r, id, type, false, NULL, e);
            free(e);
        } else {
            /* `role` records the role the switch belongs to (a quick-role pick); `thinkingLevel` runs it at that
             * level, clamped; nothing is persisted (omp 4189e22bea) */
            const char *role = json_get_str(cmd, "role");
            if (role && *role) app_use_model_role(a, m, role);
            else app_use_model(a, m);
            const char *lv = json_get_str(cmd, "thinkingLevel");
            if (lv) app_set_thinking_level(a, lv);
            respond(r, id, type, true, model_json(m), NULL);
        }
    } else if (!strcmp(type, "get_model_picker")) {
        const json_t *lj = json_get(cmd, "limit");
        int64_t limit = lj ? json_get_int(cmd, "limit", 0) : 100;
        if (lj && (!json_is_num(lj) || lj->u.num.d != floor(lj->u.num.d) || lj->u.num.d < 1)) {
            respond(r, id, type, false, NULL, "`limit` must be a positive integer.");
        } else {
            respond(r, id, type, true, model_picker_json(a, json_get_str(cmd, "query"), limit), NULL);
        }
    } else if (!strcmp(type, "set_thinking_level")) {
        if (app_set_thinking_level(a, json_get_str(cmd, "level"))) {
            respond(r, id, type, true, NULL, NULL);
        } else {
            respond(r, id, type, false, NULL, "invalid level");
        }
    } else if (!strcmp(type, "get_available_models")) {
        json_t *arr = json_arr();
        for (size_t i = 0; i < catalog_count(); i++) {
            const ai_model_t *m = catalog_at(i);
            if (!catalog_model_available(m) || !str_eq(catalog_model_kind(m), "chat")) continue;
            json_push(arr, model_json(m));
        }
        json_t *d = json_obj();
        json_set(d, "models", arr);
        respond(r, id, type, true, d, NULL);
    } else if (!strcmp(type, "compact")) {
        /* session.compact: one manual_compaction_start / _end pair; the response carries the CompactionResult */
        char *err = NULL;
        if (r->running || agent_is_running(a->agent)) {
            app_abort(a);
            for (int k = 0; k < 600 && (r->running || agent_is_running(a->agent)); k++) usleep(100 * 1000);
        }
        json_t *st = json_obj();
        json_set_str(st, "type", "manual_compaction_start");
        app_emit(a, st);
        pthread_mutex_lock(&r->mu);
        r->compacting = true;
        pthread_mutex_unlock(&r->mu);
        int rc = app_compact(a, json_get_str(cmd, "customInstructions"), &err);
        pthread_mutex_lock(&r->mu);
        r->compacting = false;
        pthread_mutex_unlock(&r->mu);
        json_t *en = json_obj();
        json_set_str(en, "type", "manual_compaction_end");
        bool skipped = rc != 0 && err && (!strcmp(err, "Already compacted") || !strncmp(err, "Nothing to compact", 18));
        if (rc == 0) json_set(en, "result", app_compaction_result(a));
        json_set_bool(en, "aborted", rc != 0 && !skipped && a->agent->cancel);
        if (skipped) json_set_bool(en, "skipped", true);
        if (rc != 0) json_set_str(en, "errorMessage", err ? err : "Compaction failed");
        app_emit(a, en);
        if (rc == 0) respond(r, id, type, true, app_compaction_result(a), NULL);
        else respond(r, id, type, false, NULL, err ? err : "Compaction failed");
        free(err);
        rpc_wake(r); /* results that settled during the compaction go into the compacted context */
    } else if (!strcmp(type, "new_session") || !strcmp(type, "switch_session") || !strcmp(type, "fork") ||
               !strcmp(type, "branch") || !strcmp(type, "open_session")) {
        session_change(r, cmd, id, type);
    } else if (!strcmp(type, "set_session_name") || !strcmp(type, "set_title")) {
        const char *nm = json_get_str(cmd, "name") ? json_get_str(cmd, "name") : json_get_str(cmd, "title");
        char *t = str_trim(nm ? nm : "");
        if (!*t) respond(r, id, type, false, NULL, "Session name cannot be empty");
        else {
            session_set_title(a->session, t, "user");
            respond(r, id, type, true, NULL, NULL);
        }
        free(t);
    } else if (!strcmp(type, "bash")) {
        /* reject rather than coerce: a falsy non-boolean such as `null` would silently share the output */
        const json_t *ex = json_get(cmd, "excludeFromContext");
        if (ex && !json_is_bool(ex)) respond(r, id, type, false, NULL, "excludeFromContext must be a boolean");
        else bash_start(r, cmd, id);
    } else if (!strcmp(type, "login") || !strcmp(type, "get_logout_accounts") || !strcmp(type, "logout")) {
        login_command(r, cmd, id, type);
    } else if (!strcmp(type, "get_session_stats")) {
        respond(r, id, type, true, session_stats_json(a), NULL);
    } else if (!strcmp(type, "set_steering_mode") || !strcmp(type, "set_follow_up_mode") || !strcmp(type, "set_interrupt_mode")) {
        const char *key = !strcmp(type, "set_steering_mode") ? "steeringMode" : !strcmp(type, "set_follow_up_mode") ? "followUpMode" : "interruptMode";
        settings_set_runtime(key, json_str(json_get_str(cmd, "mode")));
        app_sync_queue_modes(a);
        respond(r, id, type, true, NULL, NULL);
    } else {
        char *m = xasprintf("Unknown command: %s", type);
        respond(r, id, type, false, NULL, m);
        free(m);
    }
}

rpc_t *rpc_attach(app_t *a, rpc_sink_fn sink, void *ud) {
    rpc_t *r = xcalloc(1, sizeof(*r));
    r->a = a;
    r->sink = sink;
    r->sink_ud = ud;
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->abort_cv, NULL);
    pthread_mutex_init(&r->filter_mu, NULL);
    pthread_mutex_init(&r->host_mu, NULL);
    pthread_cond_init(&r->host_cv, NULL);
    pthread_mutex_init(&r->btw_mu, NULL);
    pthread_mutex_init(&r->out_mu, NULL);
    pthread_mutex_init(&r->ui_mu, NULL);
    pthread_cond_init(&r->ui_cv, NULL);
    pthread_mutex_init(&r->bash_mu, NULL);
    pthread_mutex_init(&r->bg_mu, NULL);
    pthread_mutex_init(&r->sub_mu, NULL);
    r->subs = json_obj(), r->sub_files = json_obj(), r->sub_stale = json_obj();
    agents_observe(rpc_sub_observe, r, false); /* snapshots for get_subagents; frames only once subscribed */
    r->proto = 1;
    json_t *ready = json_obj(), *sv = json_arr();
    json_set_str(ready, "type", "ready");
    json_set_int(ready, "protocolVersion", 1);
    json_push(sv, json_int(1));
    json_push(sv, json_int(2));
    json_set(ready, "supportedProtocolVersions", sv);
    json_set_int(ready, "maxFrameBytes", RPC_MAX_FRAME_BYTES);
    json_set_int(ready, "maxReassembledFrameBytes", RPC_MAX_REASSEMBLED_BYTES);
    json_set_bool(ready, "supportsBashExcludeFromContext", true); /* `bash` accepts excludeFromContext */
    emit(r, ready);
    json_free(ready);
    agent_subscribe(a->agent, rpc_listener, r);
    a->notice = notice_rpc;
    a->notice_ud = r;
    a->wake = rpc_wake;
    a->wake_ud = r;
    a->session_event = session_event_rpc;
    a->session_event_ud = r;
    json_t *qsnap = app_queued_messages(a); /* queue_update reports changes from here on */
    pthread_mutex_lock(&a->mu);
    json_free(a->queue_snapshot);
    a->queue_snapshot = qsnap;
    pthread_mutex_unlock(&a->mu);
    a->env->ui.ask = rpc_ask;
    a->env->ui.form = rpc_form;
    a->env->ui.select = rpc_fb_select;
    a->env->ui.input = rpc_fb_input;
    a->env->ui.editor = rpc_fb_editor;
    a->env->ui.approve = rpc_approve;
    a->env->ui.tool_frame = rpc_tool_frame;
    a->env->ui.ud = r;
    json_t *adv = json_obj(); /* the session's advisor runtimes reset at startup */
    json_set_str(adv, "type", "advisor_cost_changed");
    forward(r, adv);
    emit_available_commands(r);
    return r;
}

void rpc_handle_json(rpc_t *r, json_t *cmd) {
    if (!cmd) respond(r, NULL, "parse", false, NULL, "Failed to parse command: JSON Parse error");
    else if (!json_is_obj(cmd)) respond(r, NULL, "undefined", false, NULL, "Unknown command: undefined");
    else handle(r, cmd);
}

void rpc_handle_line(rpc_t *r, const char *line) {
    char *t = str_trim(line);
    if (*t) {
        char *perr = NULL;
        json_t *cmd = json_parse(t, strlen(t), &perr);
        if (!cmd) {
            char *m = xasprintf("Failed to parse command: JSON Parse error: %s", perr ? perr : "Unexpected input");
            respond(r, NULL, "parse", false, NULL, m);
            free(m);
        } else rpc_handle_json(r, cmd);
        free(perr);
        json_free(cmd);
    }
    free(t);
}

/* stdin closed: dialogs fail (the client can no longer answer them) and a login stops */
static void rpc_close_input(rpc_t *r) {
    pthread_mutex_lock(&r->ui_mu);
    r->closed = true;
    pthread_cond_broadcast(&r->ui_cv);
    pthread_mutex_unlock(&r->ui_mu);
    r->login_abort = 1;
}

bool rpc_busy(rpc_t *r) {
    pthread_mutex_lock(&r->mu);
    bool b = r->running;
    pthread_mutex_unlock(&r->mu);
    return b;
}

void rpc_detach(rpc_t *r) {
    if (!r) return;
    app_t *a = r->a;
    a->wake = NULL;
    pthread_mutex_lock(&r->mu);
    bool running = r->running;
    pthread_mutex_unlock(&r->mu);
    if (running) agent_abort(a->agent);
    pthread_mutex_lock(&r->mu);
    pthread_t th = r->th;
    pthread_mutex_unlock(&r->mu);
    if (th) pthread_join(th, NULL);
    rpc_close_input(r);
    r->bash_cancel = 1;
    cmp_bg_abort(a); /* abortCompaction("RPC client disconnected") */
    for (;;) {
        pthread_mutex_lock(&r->bg_mu);
        bool any = r->bg_threads.n > 0;
        pthread_t bt = any ? r->bg_threads.v[--r->bg_threads.n] : 0;
        pthread_mutex_unlock(&r->bg_mu);
        if (!any) break;
        pthread_join(bt, NULL);
    }
    vec_free(&r->bg_threads);
    if (a->session_event == session_event_rpc) {
        a->session_event = NULL;
        a->session_event_ud = NULL;
    }
    if (a->env->ui.ud == r) {
        a->env->ui.ask = NULL;
        a->env->ui.form = NULL;
        a->env->ui.select = NULL;
        a->env->ui.input = NULL;
        a->env->ui.editor = NULL;
        a->env->ui.approve = NULL;
        a->env->ui.tool_frame = NULL;
        a->env->ui.ud = NULL;
    }
    char *berr = NULL;
    btw_close(r, &berr);
    free(berr);
    if (r->btw_th_live) pthread_join(r->btw_th, NULL);
    btw_store_free(r->btw_store);
    free(r->btw_store_key);
    free(r->btw_starting_topic);
    json_free(r->btw_unsaved);
    pthread_mutex_destroy(&r->btw_mu);
    agents_unobserve(rpc_sub_observe, r);
    json_free(r->subs), json_free(r->sub_files), json_free(r->sub_stale);
    pthread_mutex_destroy(&r->sub_mu);
    agent_unsubscribe(a->agent, rpc_listener, r);
    if (a->notice == notice_rpc) {
        a->notice = NULL;
        a->notice_ud = NULL;
    }
    for (size_t i = 0; i < r->host_tools.n; i++) agent_remove_tool(a->agent, r->host_tools.v[i]);
    strvec_free(&r->host_tools);
    vec_free(&r->host_calls);
    strvec_free(&r->filter);
    strvec_free(&r->streamed);
    strvec_free(&r->open_ids);
    vec_free(&r->dialogs);
    json_free(r->waiting);
    free(r->open_dir);
    pthread_mutex_destroy(&r->out_mu);
    pthread_mutex_destroy(&r->ui_mu);
    pthread_cond_destroy(&r->ui_cv);
    pthread_mutex_destroy(&r->bash_mu);
    pthread_mutex_destroy(&r->bg_mu);
    pthread_mutex_destroy(&r->mu);
    pthread_mutex_destroy(&r->filter_mu);
    pthread_mutex_destroy(&r->host_mu);
    pthread_cond_destroy(&r->host_cv);
    free(r);
}

int run_rpc_mode(app_t *a) {
    g_out_failed = 0;
    rpc_t *r = rpc_attach(a, NULL, NULL);
    sb_t line = SB_INIT;
    char buf[65536];
    for (;;) {
        /* wake now and then: an output failure on another thread ends the session */
        struct pollfd pfd = {.fd = 0, .events = POLLIN};
        int pr = poll(&pfd, 1, 250);
        if (g_out_failed) break;
        if (pr == 0 || (pr < 0 && errno == EINTR)) continue;
        ssize_t n = read(0, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                sb_putc(&line, buf[i]);
                continue;
            }
            rpc_handle_line(r, sb_cstr(&line));
            sb_clear(&line);
        }
    }
    if (g_out_failed) {
        /* session.dispose(): nothing more is delivered, so the run in flight stops rather than finishing */
        rpc_close_input(r);
        if (rpc_busy(r)) agent_abort(a->agent);
        sb_free(&line);
        pthread_mutex_lock(&r->mu);
        pthread_t th = r->th;
        pthread_mutex_unlock(&r->mu);
        if (th) pthread_join(th, NULL);
        pthread_mutex_lock(&r->mu);
        r->th = 0;
        r->running = false;
        pthread_mutex_unlock(&r->mu);
        rpc_detach(r);
        return 1; /* the caller frees the app (its MCP servers) and exits 1 */
    }
    if (line.len) rpc_handle_line(r, sb_cstr(&line)); /* a last line without a newline */
    /* stdin closed: pending dialogs fail, then an in-flight run finishes (scripted clients pipe commands then EOF) */
    rpc_close_input(r);
    pthread_mutex_lock(&r->mu);
    pthread_t th = r->th;
    pthread_mutex_unlock(&r->mu);
    if (th) pthread_join(th, NULL);
    pthread_mutex_lock(&r->mu);
    r->th = 0;
    r->running = false;
    pthread_mutex_unlock(&r->mu);
    rpc_detach(r);
    sb_free(&line);
    return 0;
}
