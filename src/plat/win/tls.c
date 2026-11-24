/* plat/win/tls.c - a TLS client over Schannel behind the OpenSSL calls net/http.c makes.
 *
 * Non-blocking like OpenSSL on a non-blocking socket: SSL_connect/SSL_read/SSL_write return -1 with
 * SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE when the socket would block, and are called again. Encrypted bytes not
 * yet sent are flushed before anything else. The server's chain is verified here (Schannel's own validation is off):
 * the Windows trust store plus the CA files omc is pointed at (OMC_EXTRA_CA_CERTS / NODE_EXTRA_CA_CERTS /
 * SSL_CERT_FILE), the name or IP checked, and a failure reported as OpenSSL's X509_V_ERR_* code so the caller prints
 * the same "TLS certificate verification failed for <host>: <CODE>" it does on Linux. */
#define SECURITY_WIN32
#include "plat/win/win.h"

#include <schannel.h>
#include <security.h>
#include <sspi.h>
#include <wincrypt.h>

#include <openssl/omc_win_openssl.h>
#include <sys/socket.h>

void omc_win_ssl_set_error(unsigned long code, const char *fmt, ...);

/* ---------- context ---------- */

struct omc_win_ssl_method {
    int dummy;
};
struct omc_win_x509_verify_param {
    int dummy;
};

struct omc_win_ssl_ctx {
    int verify;
    HCERTSTORE extra; /* CA certificates from files, NULL if none */
};

static struct omc_win_ssl_method g_method;

int OPENSSL_init_ssl(uint64_t opts, const OPENSSL_INIT_SETTINGS *s) {
    (void)opts;
    (void)s;
    omc_win_init();
    return 1;
}

const SSL_METHOD *TLS_client_method(void) { return &g_method; }

SSL_CTX *SSL_CTX_new(const SSL_METHOD *m) {
    (void)m;
    SSL_CTX *c = calloc(1, sizeof *c);
    if (c) c->verify = SSL_VERIFY_PEER;
    return c;
}

void SSL_CTX_free(SSL_CTX *c) {
    if (!c) return;
    if (c->extra) CertCloseStore(c->extra, 0);
    free(c);
}

int SSL_CTX_set_min_proto_version(SSL_CTX *c, int v) {
    (void)c;
    (void)v;
    return 1;
}

void SSL_CTX_set_verify(SSL_CTX *c, int mode, SSL_verify_cb cb) {
    (void)cb;
    c->verify = mode;
}

int SSL_CTX_set_default_verify_paths(SSL_CTX *c) {
    (void)c;
    return 1;
}

int SSL_CTX_load_verify_locations(SSL_CTX *c, const char *file, const char *dir) {
    (void)dir;
    if (!file) return 0;
    FILE *f = fopen(file, "rb");
    if (!f) return 0;
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    size_t r;
    while ((r = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) buf = realloc(buf, cap *= 2);
    }
    fclose(f);
    buf[n] = 0;
    if (!c->extra) c->extra = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    int added = 0;
    const char *p = buf;
    static const char B[] = "-----BEGIN CERTIFICATE-----", E[] = "-----END CERTIFICATE-----";
    while ((p = strstr(p, B))) {
        const char *e = strstr(p, E);
        if (!e) break;
        e += sizeof E - 1;
        DWORD dl = 0;
        if (CryptStringToBinaryA(p, (DWORD)(e - p), CRYPT_STRING_BASE64HEADER, NULL, &dl, NULL, NULL)) {
            BYTE *der = malloc(dl);
            if (CryptStringToBinaryA(p, (DWORD)(e - p), CRYPT_STRING_BASE64HEADER, der, &dl, NULL, NULL) &&
                CertAddEncodedCertificateToStore(c->extra, X509_ASN_ENCODING, der, dl, CERT_STORE_ADD_ALWAYS, NULL))
                added++;
            free(der);
        }
        p = e;
    }
    free(buf);
    return added > 0;
}

long SSL_CTX_set_mode(SSL_CTX *c, long mode) {
    (void)c;
    return mode;
}

long SSL_CTX_set_session_cache_mode(SSL_CTX *c, long mode) {
    (void)c;
    return mode;
}

int SSL_CTX_set_alpn_protos(SSL_CTX *c, const unsigned char *protos, unsigned int len) {
    (void)c;
    (void)protos;
    (void)len;
    return 0; /* no ALPN offered: servers default to HTTP/1.1, the only protocol omc speaks */
}

/* ---------- connections ---------- */

enum { S_INIT, S_HANDSHAKE, S_FLUSH_THEN_UP, S_UP, S_CLOSED, S_ERROR };

struct omc_win_ssl {
    SSL_CTX *ctx;
    int fd;
    char *host;
    int verify;
    int state;
    CredHandle cred;
    CtxtHandle ctxt;
    bool have_cred, have_ctxt;
    SecPkgContext_StreamSizes sizes;
    unsigned char *in;
    size_t in_len, in_cap;
    unsigned char *out;
    size_t out_len, out_pos;
    unsigned char *plain;
    size_t plain_len, plain_pos;
    int last_err;
    long verify_result;
    struct omc_win_x509_verify_param param;
};

SSL *SSL_new(SSL_CTX *c) {
    SSL *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ctx = c;
    s->fd = -1;
    s->verify = c ? c->verify : SSL_VERIFY_PEER;
    s->verify_result = X509_V_OK;
    return s;
}

void SSL_free(SSL *s) {
    if (!s) return;
    if (s->have_ctxt) DeleteSecurityContext(&s->ctxt);
    if (s->have_cred) FreeCredentialsHandle(&s->cred);
    free(s->host);
    free(s->in);
    free(s->out);
    free(s->plain);
    free(s);
}

int SSL_set_fd(SSL *s, int fd) {
    s->fd = fd;
    return 1;
}

long SSL_set_tlsext_host_name(SSL *s, const char *name) {
    free(s->host);
    s->host = strdup(name);
    return 1;
}

void SSL_set_verify(SSL *s, int mode, SSL_verify_cb cb) {
    (void)cb;
    s->verify = mode;
}

int SSL_set1_host(SSL *s, const char *host) {
    free(s->host);
    s->host = strdup(host);
    return 1;
}

X509_VERIFY_PARAM *SSL_get0_param(SSL *s) { return &s->param; }

int X509_VERIFY_PARAM_set1_ip_asc(X509_VERIFY_PARAM *p, const char *ip) {
    SSL *s = (SSL *)((char *)p - offsetof(SSL, param));
    free(s->host);
    s->host = strdup(ip);
    return 1;
}

static void out_append(SSL *s, const void *p, size_t n) {
    if (s->out_pos == s->out_len) s->out_pos = s->out_len = 0;
    s->out = realloc(s->out, s->out_len + n);
    memcpy(s->out + s->out_len, p, n);
    s->out_len += n;
}

/* send what is pending: 1 done, 0 would block, -1 error */
static int flush_out(SSL *s) {
    while (s->out_pos < s->out_len) {
        ssize_t w = send(s->fd, s->out + s->out_pos, s->out_len - s->out_pos, 0);
        if (w > 0) {
            s->out_pos += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        return -1;
    }
    s->out_pos = s->out_len = 0;
    return 1;
}

/* receive more ciphertext: 1 got some, 0 would block, -1 error, -2 closed */
static int fill_in(SSL *s) {
    size_t want = 16384 + 512;
    if (s->in_cap - s->in_len < want) {
        s->in_cap = s->in_len + want;
        s->in = realloc(s->in, s->in_cap);
    }
    ssize_t r = recv(s->fd, s->in + s->in_len, s->in_cap - s->in_len, 0);
    if (r > 0) {
        s->in_len += (size_t)r;
        return 1;
    }
    if (r == 0) return -2;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

static int fail(SSL *s, int err) {
    s->last_err = err;
    if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) s->state = S_ERROR;
    return -1;
}

/* ---------- certificate verification ---------- */

static bool store_has(HCERTSTORE st, PCCERT_CONTEXT c) {
    if (!st || !c) return false;
    PCCERT_CONTEXT f = CertFindCertificateInStore(st, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, c, NULL);
    if (f) CertFreeCertificateContext(f);
    return f != NULL;
}

static long map_policy_error(DWORD err, PCCERT_CHAIN_CONTEXT chain, bool is_ip) {
    switch (err) {
    case 0: return X509_V_OK;
    case CERT_E_UNTRUSTEDROOT:
    case CERT_E_UNTRUSTEDTESTROOT:
        if (chain && chain->cChain && chain->rgpChain[0]->cElement == 1) return X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT;
        return X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN;
    case CERT_E_CHAINING: return X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY;
    case CERT_E_EXPIRED: {
        if (chain && chain->cChain) {
            PCCERT_CONTEXT leaf = chain->rgpChain[0]->rgpElement[0]->pCertContext;
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            if (CompareFileTime(&now, &leaf->pCertInfo->NotBefore) < 0) return X509_V_ERR_CERT_NOT_YET_VALID;
        }
        return X509_V_ERR_CERT_HAS_EXPIRED;
    }
    case CERT_E_CN_NO_MATCH: return is_ip ? X509_V_ERR_IP_ADDRESS_MISMATCH : X509_V_ERR_HOSTNAME_MISMATCH;
    case CRYPT_E_REVOKED: return X509_V_ERR_CERT_REVOKED;
    case TRUST_E_CERT_SIGNATURE: return X509_V_ERR_CERT_SIGNATURE_FAILURE;
    case CERT_E_WRONG_USAGE: return X509_V_ERR_INVALID_PURPOSE;
    case CERT_E_ROLE: return X509_V_ERR_INVALID_CA;
    case CERT_E_PATHLENCONST: return X509_V_ERR_CERT_CHAIN_TOO_LONG;
    default: return X509_V_ERR_UNSPECIFIED;
    }
}

static long verify_peer(SSL *s) {
    PCCERT_CONTEXT cert = NULL;
    if (QueryContextAttributesW(&s->ctxt, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert)
        return X509_V_ERR_UNSPECIFIED;
    bool is_ip = s->host && (strchr(s->host, ':') || strspn(s->host, "0123456789.") == strlen(s->host));
    /* the server's intermediates and omc's extra CAs both help build the chain */
    HCERTSTORE coll = CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, NULL);
    if (coll) {
        if (cert->hCertStore) CertAddStoreToCollection(coll, cert->hCertStore, 0, 0);
        if (s->ctx && s->ctx->extra) CertAddStoreToCollection(coll, s->ctx->extra, 0, 0);
    }
    static LPSTR usages[] = {szOID_PKIX_KP_SERVER_AUTH, szOID_SERVER_GATED_CRYPTO, szOID_SGC_NETSCAPE};
    CERT_CHAIN_PARA para = {.cbSize = sizeof para};
    para.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
    para.RequestedUsage.Usage.cUsageIdentifier = ARRAYSIZE(usages);
    para.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    long result = X509_V_ERR_UNSPECIFIED;
    if (CertGetCertificateChain(NULL, cert, NULL, coll ? coll : cert->hCertStore, &para, 0, NULL, &chain)) {
        wchar_t *wname = win_wide(s->host ? s->host : "");
        HTTPSPolicyCallbackData https = {.cbStruct = sizeof https};
        https.dwAuthType = AUTHTYPE_SERVER;
        https.pwszServerName = wname;
        CERT_CHAIN_POLICY_PARA pp = {.cbSize = sizeof pp};
        pp.pvExtraPolicyPara = &https;
        CERT_CHAIN_POLICY_STATUS ps = {.cbSize = sizeof ps};
        if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps)) {
            DWORD err = ps.dwError;
            /* a chain that ends at one of omc's extra CAs is trusted; the other checks still apply */
            if ((err == CERT_E_UNTRUSTEDROOT || err == CERT_E_CHAINING) && chain->cChain && s->ctx && s->ctx->extra) {
                PCERT_SIMPLE_CHAIN sc = chain->rgpChain[0];
                bool anchored = false;
                for (DWORD i = 0; i < sc->cElement && !anchored; i++) anchored = store_has(s->ctx->extra, sc->rgpElement[i]->pCertContext);
                if (anchored) {
                    https.fdwChecks = 0x00000100 /* SECURITY_FLAG_IGNORE_UNKNOWN_CA */;
                    CERT_CHAIN_POLICY_STATUS ps2 = {.cbSize = sizeof ps2};
                    if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps2)) err = ps2.dwError;
                    if (err == CERT_E_CHAINING) err = 0;
                }
            }
            result = map_policy_error(err, chain, is_ip);
        }
        free(wname);
        CertFreeCertificateChain(chain);
    }
    if (coll) CertCloseStore(coll, 0);
    CertFreeCertificateContext(cert);
    return result;
}

/* ---------- handshake ---------- */

static const DWORD ISC_FLAGS = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                               ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS;

static bool acquire_cred(SSL *s) {
    SCHANNEL_CRED sc = {0};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    TimeStamp ts;
    SECURITY_STATUS st = AcquireCredentialsHandleW(NULL, (wchar_t *)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &s->cred, &ts);
    if (st != SEC_E_OK) {
        /* systems where TLS 1.2 must come from the defaults */
        sc.grbitEnabledProtocols = 0;
        st = AcquireCredentialsHandleW(NULL, (wchar_t *)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &s->cred, &ts);
    }
    if (st != SEC_E_OK) {
        omc_win_ssl_set_error((unsigned long)st, "AcquireCredentialsHandle failed (0x%08lx)", (unsigned long)st);
        return false;
    }
    s->have_cred = true;
    return true;
}

/* one InitializeSecurityContext step on the buffered input; returns the status */
static SECURITY_STATUS isc_step(SSL *s, bool first) {
    SecBuffer inb[2], outb[1];
    SecBufferDesc ind = {SECBUFFER_VERSION, 2, inb}, outd = {SECBUFFER_VERSION, 1, outb};
    inb[0] = (SecBuffer){(ULONG)s->in_len, SECBUFFER_TOKEN, s->in};
    inb[1] = (SecBuffer){0, SECBUFFER_EMPTY, NULL};
    outb[0] = (SecBuffer){0, SECBUFFER_TOKEN, NULL};
    ULONG attrs = 0;
    TimeStamp ts;
    wchar_t *target = s->host ? win_wide(s->host) : NULL;
    SECURITY_STATUS st = InitializeSecurityContextW(&s->cred, first ? NULL : &s->ctxt, target, ISC_FLAGS, 0, 0, first ? NULL : &ind,
                                                    0, first ? &s->ctxt : NULL, &outd, &attrs, &ts);
    free(target);
    if (first && (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED)) s->have_ctxt = true;
    if (outb[0].pvBuffer) {
        if (outb[0].cbBuffer && (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || FAILED(st))) out_append(s, outb[0].pvBuffer, outb[0].cbBuffer);
        FreeContextBuffer(outb[0].pvBuffer);
    }
    if (!first && st != SEC_E_INCOMPLETE_MESSAGE) {
        if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer) {
            memmove(s->in, s->in + s->in_len - inb[1].cbBuffer, inb[1].cbBuffer);
            s->in_len = inb[1].cbBuffer;
        } else {
            s->in_len = 0;
        }
    }
    return st;
}

int SSL_connect(SSL *s) {
    for (;;) {
        switch (s->state) {
        case S_UP: return 1;
        case S_ERROR:
        case S_CLOSED: return fail(s, SSL_ERROR_SSL);
        case S_INIT:
            if (!s->have_cred && !acquire_cred(s)) return fail(s, SSL_ERROR_SSL);
            {
                SECURITY_STATUS st = isc_step(s, true);
                if (st != SEC_I_CONTINUE_NEEDED) {
                    omc_win_ssl_set_error((unsigned long)st, "InitializeSecurityContext failed (0x%08lx)", (unsigned long)st);
                    return fail(s, SSL_ERROR_SSL);
                }
            }
            s->state = S_HANDSHAKE;
            break;
        case S_FLUSH_THEN_UP: {
            int f = flush_out(s);
            if (f == 0) return fail(s, SSL_ERROR_WANT_WRITE);
            if (f < 0) return fail(s, SSL_ERROR_SYSCALL);
            if (s->verify != SSL_VERIFY_NONE) {
                s->verify_result = verify_peer(s);
                if (s->verify_result != X509_V_OK) {
                    omc_win_ssl_set_error(0x0A000086, "error:0A000086:SSL routines::certificate verify failed");
                    return fail(s, SSL_ERROR_SSL);
                }
            }
            QueryContextAttributesW(&s->ctxt, SECPKG_ATTR_STREAM_SIZES, &s->sizes);
            s->state = S_UP;
            return 1;
        }
        case S_HANDSHAKE: {
            int f = flush_out(s);
            if (f == 0) return fail(s, SSL_ERROR_WANT_WRITE);
            if (f < 0) return fail(s, SSL_ERROR_SYSCALL);
            if (!s->in_len) {
                int r = fill_in(s);
                if (r == 0) return fail(s, SSL_ERROR_WANT_READ);
                if (r < 0) {
                    omc_win_ssl_set_error(0x0A000126, "error:0A000126:SSL routines::unexpected eof while reading");
                    return fail(s, SSL_ERROR_SYSCALL);
                }
            }
            SECURITY_STATUS st = isc_step(s, false);
            if (st == SEC_E_INCOMPLETE_MESSAGE) {
                int r = fill_in(s);
                if (r == 0) return fail(s, SSL_ERROR_WANT_READ);
                if (r < 0) {
                    omc_win_ssl_set_error(0x0A000126, "error:0A000126:SSL routines::unexpected eof while reading");
                    return fail(s, SSL_ERROR_SYSCALL);
                }
                break;
            }
            if (st == SEC_E_OK) {
                s->state = S_FLUSH_THEN_UP;
                break;
            }
            if (st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS) break;
            flush_out(s); /* the alert, if any */
            omc_win_ssl_set_error((unsigned long)st, "error:0A000410:SSL routines::sslv3 alert handshake failure (0x%08lx)", (unsigned long)st);
            return fail(s, SSL_ERROR_SSL);
        }
        default: return fail(s, SSL_ERROR_SSL);
        }
    }
}

/* ---------- data ---------- */

int SSL_read(SSL *s, void *buf, int n) {
    if (s->state != S_UP) return s->state == S_CLOSED ? (s->last_err = SSL_ERROR_ZERO_RETURN, 0) : fail(s, SSL_ERROR_SSL);
    for (;;) {
        if (s->plain_pos < s->plain_len) {
            size_t k = s->plain_len - s->plain_pos;
            if (k > (size_t)n) k = (size_t)n;
            memcpy(buf, s->plain + s->plain_pos, k);
            s->plain_pos += k;
            return (int)k;
        }
        int f = flush_out(s);
        if (f == 0) return fail(s, SSL_ERROR_WANT_WRITE);
        if (f < 0) return fail(s, SSL_ERROR_SYSCALL);
        if (s->in_len) {
            SecBuffer b[4] = {{(ULONG)s->in_len, SECBUFFER_DATA, s->in}, {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}};
            SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
            SECURITY_STATUS st = DecryptMessage(&s->ctxt, &d, 0, NULL);
            if (st == SEC_E_OK || st == SEC_I_RENEGOTIATE || st == SEC_I_CONTEXT_EXPIRED) {
                SecBuffer *data = NULL, *extra = NULL;
                for (int i = 1; i < 4; i++) {
                    if (b[i].BufferType == SECBUFFER_DATA) data = &b[i];
                    else if (b[i].BufferType == SECBUFFER_EXTRA) extra = &b[i];
                }
                if (data && data->cbBuffer) {
                    s->plain = realloc(s->plain, data->cbBuffer);
                    memcpy(s->plain, data->pvBuffer, data->cbBuffer);
                    s->plain_len = data->cbBuffer;
                    s->plain_pos = 0;
                }
                if (extra && extra->cbBuffer) {
                    memmove(s->in, s->in + s->in_len - extra->cbBuffer, extra->cbBuffer);
                    s->in_len = extra->cbBuffer;
                } else {
                    s->in_len = 0;
                }
                if (st == SEC_I_CONTEXT_EXPIRED) {
                    if (s->plain_pos < s->plain_len) continue;
                    s->state = S_CLOSED;
                    s->last_err = SSL_ERROR_ZERO_RETURN;
                    return 0;
                }
                if (st == SEC_I_RENEGOTIATE) {
                    /* post-handshake messages (TLS 1.3 tickets, key updates): the extra bytes go back to the handshake */
                    SECURITY_STATUS hs = isc_step(s, false);
                    if (hs != SEC_E_OK && hs != SEC_I_CONTINUE_NEEDED && hs != SEC_E_INCOMPLETE_MESSAGE) {
                        omc_win_ssl_set_error((unsigned long)hs, "renegotiation failed (0x%08lx)", (unsigned long)hs);
                        return fail(s, SSL_ERROR_SSL);
                    }
                }
                continue;
            }
            if (st != SEC_E_INCOMPLETE_MESSAGE) {
                omc_win_ssl_set_error((unsigned long)st, "DecryptMessage failed (0x%08lx)", (unsigned long)st);
                return fail(s, SSL_ERROR_SSL);
            }
        }
        int r = fill_in(s);
        if (r == 0) return fail(s, SSL_ERROR_WANT_READ);
        if (r == -2) {
            /* closed without close_notify: OpenSSL reports SSL_ERROR_SYSCALL with 0 */
            s->last_err = SSL_ERROR_SYSCALL;
            s->state = S_CLOSED;
            return 0;
        }
        if (r < 0) return fail(s, SSL_ERROR_SYSCALL);
    }
}

int SSL_write(SSL *s, const void *buf, int n) {
    if (s->state != S_UP) return fail(s, SSL_ERROR_SSL);
    int f = flush_out(s);
    if (f == 0) return fail(s, SSL_ERROR_WANT_WRITE);
    if (f < 0) return fail(s, SSL_ERROR_SYSCALL);
    size_t take = (size_t)n;
    if (take > s->sizes.cbMaximumMessage) take = s->sizes.cbMaximumMessage;
    size_t total = s->sizes.cbHeader + take + s->sizes.cbTrailer;
    unsigned char *msg = malloc(total);
    memcpy(msg + s->sizes.cbHeader, buf, take);
    SecBuffer b[4] = {{s->sizes.cbHeader, SECBUFFER_STREAM_HEADER, msg},
                      {(ULONG)take, SECBUFFER_DATA, msg + s->sizes.cbHeader},
                      {s->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, msg + s->sizes.cbHeader + take},
                      {0, SECBUFFER_EMPTY, NULL}};
    SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
    SECURITY_STATUS st = EncryptMessage(&s->ctxt, 0, &d, 0);
    if (st != SEC_E_OK) {
        free(msg);
        omc_win_ssl_set_error((unsigned long)st, "EncryptMessage failed (0x%08lx)", (unsigned long)st);
        return fail(s, SSL_ERROR_SSL);
    }
    out_append(s, msg, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
    free(msg);
    /* the record is committed: report it written; the rest goes out on the next call (or here, if it can) */
    flush_out(s);
    return (int)take;
}

int SSL_shutdown(SSL *s) {
    if (s->state == S_UP && s->have_ctxt) {
        DWORD type = SCHANNEL_SHUTDOWN;
        SecBuffer b = {sizeof type, SECBUFFER_TOKEN, &type};
        SecBufferDesc d = {SECBUFFER_VERSION, 1, &b};
        if (ApplyControlToken(&s->ctxt, &d) == SEC_E_OK) {
            SecBuffer ob = {0, SECBUFFER_TOKEN, NULL};
            SecBufferDesc od = {SECBUFFER_VERSION, 1, &ob};
            ULONG attrs;
            TimeStamp ts;
            if (InitializeSecurityContextW(&s->cred, &s->ctxt, NULL, ISC_FLAGS, 0, 0, NULL, 0, NULL, &od, &attrs, &ts) == SEC_E_OK &&
                ob.pvBuffer) {
                out_append(s, ob.pvBuffer, ob.cbBuffer);
                FreeContextBuffer(ob.pvBuffer);
                flush_out(s);
            } else if (ob.pvBuffer) {
                FreeContextBuffer(ob.pvBuffer);
            }
        }
    }
    s->state = S_CLOSED;
    return 1;
}

int SSL_get_error(const SSL *s, int ret) {
    if (ret > 0) return SSL_ERROR_NONE;
    return s->last_err ? s->last_err : SSL_ERROR_SYSCALL;
}

long SSL_get_verify_result(const SSL *s) { return s->verify_result; }

int SSL_pending(const SSL *s) { return (int)(s->plain_len - s->plain_pos); }

const char *X509_verify_cert_error_string(long n) {
    switch (n) {
    case X509_V_OK: return "ok";
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT: return "unable to get issuer certificate";
    case X509_V_ERR_CERT_SIGNATURE_FAILURE: return "certificate signature failure";
    case X509_V_ERR_CERT_NOT_YET_VALID: return "certificate is not yet valid";
    case X509_V_ERR_CERT_HAS_EXPIRED: return "certificate has expired";
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT: return "self-signed certificate";
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN: return "self-signed certificate in certificate chain";
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY: return "unable to get local issuer certificate";
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE: return "unable to verify the first certificate";
    case X509_V_ERR_CERT_CHAIN_TOO_LONG: return "certificate chain too long";
    case X509_V_ERR_CERT_REVOKED: return "certificate revoked";
    case X509_V_ERR_INVALID_CA: return "invalid CA certificate";
    case X509_V_ERR_INVALID_PURPOSE: return "unsupported certificate purpose";
    case X509_V_ERR_CERT_UNTRUSTED: return "certificate not trusted";
    case X509_V_ERR_CERT_REJECTED: return "certificate rejected";
    case X509_V_ERR_HOSTNAME_MISMATCH: return "hostname mismatch";
    case X509_V_ERR_IP_ADDRESS_MISMATCH: return "IP address mismatch";
    default: return "unspecified certificate verification error";
    }
}
