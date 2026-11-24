/* plat/win/crypto.c - the OpenSSL calls omc makes for hashing, HMAC, random bytes and RSA-SHA256 signatures, over
 * Windows CNG (bcrypt) and the certificate API's DER decoders. Errors are kept per thread for ERR_get_error(). */
#include "plat/win/win.h"

#include <bcrypt.h>
#include <wincrypt.h>

#include <openssl/omc_win_openssl.h>

/* ---------- errors ---------- */

static __thread char g_err_msg[256];
static __thread unsigned long g_err_code;

void omc_win_ssl_set_error(unsigned long code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err_msg, sizeof g_err_msg, fmt, ap);
    va_end(ap);
    g_err_code = code ? code : 1;
}

unsigned long ERR_get_error(void) {
    unsigned long c = g_err_code;
    g_err_code = 0;
    return c;
}

unsigned long ERR_peek_error(void) { return g_err_code; }

void ERR_clear_error(void) {
    g_err_code = 0;
    g_err_msg[0] = 0;
}

void ERR_error_string_n(unsigned long e, char *buf, size_t len) {
    if (!len) return;
    if (g_err_msg[0]) snprintf(buf, len, "%s", g_err_msg);
    else snprintf(buf, len, "error:%08lX:lib(0)::reason(%lu)", e, e);
}

char *ERR_error_string(unsigned long e, char *buf) {
    static __thread char tmp[256];
    if (!buf) buf = tmp;
    ERR_error_string_n(e, buf, 256);
    return buf;
}

/* ---------- digests ---------- */

struct omc_win_evp_md {
    const wchar_t *alg;
    int size;
    BCRYPT_ALG_HANDLE h, hmac;
    INIT_ONCE once;
};

static struct omc_win_evp_md g_sha1 = {BCRYPT_SHA1_ALGORITHM, 20, NULL, NULL, INIT_ONCE_STATIC_INIT};
static struct omc_win_evp_md g_sha256 = {BCRYPT_SHA256_ALGORITHM, 32, NULL, NULL, INIT_ONCE_STATIC_INIT};
static struct omc_win_evp_md g_sha512 = {BCRYPT_SHA512_ALGORITHM, 64, NULL, NULL, INIT_ONCE_STATIC_INIT};

static bool md_ready(struct omc_win_evp_md *m) {
    BOOL pending;
    InitOnceBeginInitialize(&m->once, 0, &pending, NULL);
    if (pending) {
        BCryptOpenAlgorithmProvider(&m->h, m->alg, NULL, 0);
        BCryptOpenAlgorithmProvider(&m->hmac, m->alg, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        InitOnceComplete(&m->once, 0, NULL);
    }
    return m->h != NULL;
}

const EVP_MD *EVP_sha1(void) { return &g_sha1; }
const EVP_MD *EVP_sha256(void) { return &g_sha256; }
const EVP_MD *EVP_sha512(void) { return &g_sha512; }
int EVP_MD_size(const EVP_MD *md) { return md ? md->size : 0; }

struct omc_win_evp_md_ctx {
    struct omc_win_evp_md *md;
    BCRYPT_HASH_HANDLE h;
    EVP_PKEY *sign_key;
};

EVP_MD_CTX *EVP_MD_CTX_new(void) { return calloc(1, sizeof(EVP_MD_CTX)); }

void EVP_MD_CTX_free(EVP_MD_CTX *c) {
    if (!c) return;
    if (c->h) BCryptDestroyHash(c->h);
    free(c);
}

int EVP_DigestInit_ex(EVP_MD_CTX *c, const EVP_MD *md, ENGINE *e) {
    (void)e;
    if (!c || !md || !md_ready((struct omc_win_evp_md *)md)) return 0;
    if (c->h) BCryptDestroyHash(c->h);
    c->h = NULL;
    c->md = (struct omc_win_evp_md *)md;
    return BCryptCreateHash(c->md->h, &c->h, NULL, 0, NULL, 0, 0) == 0;
}

int EVP_DigestUpdate(EVP_MD_CTX *c, const void *d, size_t n) {
    if (!c || !c->h) return 0;
    const unsigned char *p = d;
    while (n) {
        ULONG k = n > 0x40000000 ? 0x40000000 : (ULONG)n;
        if (BCryptHashData(c->h, (PUCHAR)p, k, 0) != 0) return 0;
        p += k;
        n -= k;
    }
    return 1;
}

int EVP_DigestFinal_ex(EVP_MD_CTX *c, unsigned char *md, unsigned int *n) {
    if (!c || !c->h) return 0;
    int ok = BCryptFinishHash(c->h, md, (ULONG)c->md->size, 0) == 0;
    BCryptDestroyHash(c->h);
    c->h = NULL;
    if (n) *n = ok ? (unsigned)c->md->size : 0;
    return ok;
}

int EVP_Digest(const void *d, size_t n, unsigned char *md, unsigned int *mdn, const EVP_MD *type, ENGINE *e) {
    EVP_MD_CTX c = {0};
    int ok = EVP_DigestInit_ex(&c, type, e) && EVP_DigestUpdate(&c, d, n) && EVP_DigestFinal_ex(&c, md, mdn);
    if (c.h) BCryptDestroyHash(c.h);
    return ok;
}

static unsigned char *one_shot(const EVP_MD *m, const unsigned char *d, size_t n, unsigned char *md) {
    static __thread unsigned char tmp[64];
    if (!md) md = tmp;
    return EVP_Digest(d, n, md, NULL, m, NULL) ? md : NULL;
}

unsigned char *SHA1(const unsigned char *d, size_t n, unsigned char *md) { return one_shot(&g_sha1, d, n, md); }
unsigned char *SHA256(const unsigned char *d, size_t n, unsigned char *md) { return one_shot(&g_sha256, d, n, md); }
unsigned char *SHA512(const unsigned char *d, size_t n, unsigned char *md) { return one_shot(&g_sha512, d, n, md); }

unsigned char *HMAC(const EVP_MD *md, const void *key, int keylen, const unsigned char *d, size_t n, unsigned char *out,
                    unsigned int *outlen) {
    struct omc_win_evp_md *m = (struct omc_win_evp_md *)md;
    if (!m || !md_ready(m) || !m->hmac) return NULL;
    static __thread unsigned char tmp[64];
    if (!out) out = tmp;
    BCRYPT_HASH_HANDLE h;
    if (BCryptCreateHash(m->hmac, &h, NULL, 0, (PUCHAR)key, (ULONG)keylen, 0) != 0) return NULL;
    bool ok = true;
    while (n && ok) {
        ULONG k = n > 0x40000000 ? 0x40000000 : (ULONG)n;
        ok = BCryptHashData(h, (PUCHAR)d, k, 0) == 0;
        d += k;
        n -= k;
    }
    ok = ok && BCryptFinishHash(h, out, (ULONG)m->size, 0) == 0;
    BCryptDestroyHash(h);
    if (outlen) *outlen = ok ? (unsigned)m->size : 0;
    return ok ? out : NULL;
}

/* ---------- random, memory ---------- */

int RAND_bytes(unsigned char *buf, int n) {
    if (n < 0) return 0;
    return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

void OPENSSL_cleanse(void *p, size_t n) { SecureZeroMemory(p, n); }

/* ---------- RSA keys from PEM ---------- */

struct omc_win_bio {
    char *data;
    size_t len;
};

BIO *BIO_new_mem_buf(const void *buf, int len) {
    BIO *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    size_t n = len < 0 ? strlen(buf) : (size_t)len;
    b->data = malloc(n + 1);
    memcpy(b->data, buf, n);
    b->data[n] = 0;
    b->len = n;
    return b;
}

int BIO_free(BIO *b) {
    if (!b) return 0;
    free(b->data);
    free(b);
    return 1;
}

struct omc_win_evp_pkey {
    BCRYPT_KEY_HANDLE key;
    ULONG bits;
};

static BCRYPT_ALG_HANDLE rsa_alg(void) {
    static BCRYPT_ALG_HANDLE h;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (pending) {
        BCryptOpenAlgorithmProvider(&h, BCRYPT_RSA_ALGORITHM, NULL, 0);
        InitOnceComplete(&once, 0, NULL);
    }
    return h;
}

/* the DER of the first PEM block named `label` (owned) */
static unsigned char *pem_block(const char *pem, const char *label, DWORD *len) {
    char begin[64], end[64];
    snprintf(begin, sizeof begin, "-----BEGIN %s-----", label);
    snprintf(end, sizeof end, "-----END %s-----", label);
    const char *b = strstr(pem, begin);
    if (!b) return NULL;
    b += strlen(begin);
    const char *e = strstr(b, end);
    if (!e) return NULL;
    DWORD n = 0;
    if (!CryptStringToBinaryA(b, (DWORD)(e - b), CRYPT_STRING_BASE64, NULL, &n, NULL, NULL)) return NULL;
    unsigned char *der = malloc(n);
    if (!CryptStringToBinaryA(b, (DWORD)(e - b), CRYPT_STRING_BASE64, der, &n, NULL, NULL)) {
        free(der);
        return NULL;
    }
    *len = n;
    return der;
}

/* PKCS#1 RSAPrivateKey DER -> CNG key */
static BCRYPT_KEY_HANDLE import_pkcs1(const unsigned char *der, DWORD len) {
    BYTE *blob = NULL;
    DWORD bl = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, der, len, CRYPT_DECODE_ALLOC_FLAG,
                             NULL, &blob, &bl))
        return NULL;
    BCRYPT_KEY_HANDLE k = NULL;
    if (BCryptImportKeyPair(rsa_alg(), NULL, LEGACY_RSAPRIVATE_BLOB, &k, blob, bl, 0) != 0) k = NULL;
    LocalFree(blob);
    return k;
}

EVP_PKEY *PEM_read_bio_PrivateKey(BIO *bp, EVP_PKEY **x, pem_password_cb *cb, void *u) {
    (void)cb;
    (void)u;
    if (!bp || !rsa_alg()) return NULL;
    DWORD len = 0;
    BCRYPT_KEY_HANDLE k = NULL;
    unsigned char *der = pem_block(bp->data, "PRIVATE KEY", &len);
    if (der) {
        CRYPT_PRIVATE_KEY_INFO *pki = NULL;
        DWORD pl = 0;
        if (CryptDecodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, PKCS_PRIVATE_KEY_INFO, der, len, CRYPT_DECODE_ALLOC_FLAG,
                                NULL, &pki, &pl)) {
            if (pki->Algorithm.pszObjId && !strcmp(pki->Algorithm.pszObjId, szOID_RSA_RSA))
                k = import_pkcs1(pki->PrivateKey.pbData, pki->PrivateKey.cbData);
            LocalFree(pki);
        }
        free(der);
    } else if ((der = pem_block(bp->data, "RSA PRIVATE KEY", &len))) {
        k = import_pkcs1(der, len);
        free(der);
    }
    if (!k) {
        omc_win_ssl_set_error(0x1E08010C, "error:1E08010C:DECODER routines::unsupported");
        return NULL;
    }
    EVP_PKEY *p = calloc(1, sizeof *p);
    p->key = k;
    ULONG got = 0;
    BCryptGetProperty(k, BCRYPT_KEY_LENGTH, (PUCHAR)&p->bits, sizeof p->bits, &got, 0);
    if (x) *x = p;
    return p;
}

int EVP_PKEY_base_id(const EVP_PKEY *k) { return k ? EVP_PKEY_RSA : 0; }

void EVP_PKEY_free(EVP_PKEY *k) {
    if (!k) return;
    BCryptDestroyKey(k->key);
    free(k);
}

int EVP_DigestSignInit(EVP_MD_CTX *c, EVP_PKEY_CTX **pctx, const EVP_MD *md, ENGINE *e, EVP_PKEY *k) {
    (void)pctx;
    if (!c || !k || !EVP_DigestInit_ex(c, md, e)) return 0;
    c->sign_key = k;
    return 1;
}

int EVP_DigestSignUpdate(EVP_MD_CTX *c, const void *d, size_t n) { return EVP_DigestUpdate(c, d, n); }

int EVP_DigestVerifyInit(EVP_MD_CTX *c, EVP_PKEY_CTX **pctx, const EVP_MD *md, ENGINE *e, EVP_PKEY *k) {
    return EVP_DigestSignInit(c, pctx, md, e, k);
}

int EVP_DigestVerifyUpdate(EVP_MD_CTX *c, const void *d, size_t n) { return EVP_DigestUpdate(c, d, n); }

int EVP_DigestVerifyFinal(EVP_MD_CTX *c, const unsigned char *sig, size_t siglen) {
    if (!c || !c->sign_key) return 0;
    unsigned char dg[64];
    unsigned dn = 0;
    if (!EVP_DigestFinal_ex(c, dg, &dn)) return 0;
    BCRYPT_PKCS1_PADDING_INFO pad = {c->md->alg};
    return BCryptVerifySignature(c->sign_key->key, &pad, dg, dn, (PUCHAR)sig, (ULONG)siglen, BCRYPT_PAD_PKCS1) == 0;
}

/* ---------- key generation, PEM export ---------- */

struct omc_win_bio_method {
    int dummy;
};
static struct omc_win_bio_method g_mem_method;

const BIO_METHOD *BIO_s_mem(void) { return &g_mem_method; }

BIO *BIO_new(const BIO_METHOD *m) {
    (void)m;
    return calloc(1, sizeof(BIO));
}

long BIO_get_mem_data(BIO *b, char **pp) {
    if (pp) *pp = b->data;
    return (long)b->len;
}

struct omc_win_evp_pkey_ctx {
    ULONG bits;
};

EVP_PKEY_CTX *EVP_PKEY_CTX_new_id(int id, ENGINE *e) {
    (void)e;
    if (id != EVP_PKEY_RSA) return NULL;
    EVP_PKEY_CTX *c = calloc(1, sizeof *c);
    if (c) c->bits = 2048;
    return c;
}

void EVP_PKEY_CTX_free(EVP_PKEY_CTX *c) { free(c); }
int EVP_PKEY_keygen_init(EVP_PKEY_CTX *c) { return c != NULL; }

int EVP_PKEY_CTX_set_rsa_keygen_bits(EVP_PKEY_CTX *c, int bits) {
    if (!c || bits < 512) return 0;
    c->bits = (ULONG)bits;
    return 1;
}

int EVP_PKEY_keygen(EVP_PKEY_CTX *c, EVP_PKEY **out) {
    if (!c || !out || !rsa_alg()) return 0;
    BCRYPT_KEY_HANDLE k;
    if (BCryptGenerateKeyPair(rsa_alg(), &k, c->bits, 0) != 0) return 0;
    if (BCryptFinalizeKeyPair(k, 0) != 0) {
        BCryptDestroyKey(k);
        return 0;
    }
    EVP_PKEY *p = calloc(1, sizeof *p);
    p->key = k;
    p->bits = c->bits;
    *out = p;
    return 1;
}

int PEM_write_bio_PrivateKey(BIO *b, EVP_PKEY *k, const EVP_CIPHER *enc, unsigned char *kstr, int klen, pem_password_cb *cb,
                             void *u) {
    (void)enc, (void)kstr, (void)klen, (void)cb, (void)u;
    if (!b || !k) return 0;
    /* CNG -> legacy private key blob -> PKCS#1 DER -> PKCS#8 DER -> PEM */
    ULONG bl = 0;
    if (BCryptExportKey(k->key, NULL, LEGACY_RSAPRIVATE_BLOB, NULL, 0, &bl, 0) != 0) return 0;
    BYTE *blob = malloc(bl);
    if (BCryptExportKey(k->key, NULL, LEGACY_RSAPRIVATE_BLOB, blob, bl, &bl, 0) != 0) {
        free(blob);
        return 0;
    }
    BYTE *p1 = NULL;
    DWORD p1l = 0;
    BOOL ok = CryptEncodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, blob, CRYPT_ENCODE_ALLOC_FLAG, NULL, &p1, &p1l);
    SecureZeroMemory(blob, bl);
    free(blob);
    if (!ok) return 0;
    CRYPT_PRIVATE_KEY_INFO pki = {0};
    pki.Algorithm.pszObjId = szOID_RSA_RSA;
    static BYTE null_params[] = {0x05, 0x00};
    pki.Algorithm.Parameters.cbData = sizeof null_params;
    pki.Algorithm.Parameters.pbData = null_params;
    pki.PrivateKey.cbData = p1l;
    pki.PrivateKey.pbData = p1;
    BYTE *p8 = NULL;
    DWORD p8l = 0;
    ok = CryptEncodeObjectEx(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, PKCS_PRIVATE_KEY_INFO, &pki, CRYPT_ENCODE_ALLOC_FLAG, NULL, &p8, &p8l);
    LocalFree(p1);
    if (!ok) return 0;
    DWORD sl = 0;
    CryptBinaryToStringA(p8, p8l, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCR, NULL, &sl);
    char *b64 = malloc(sl + 1);
    CryptBinaryToStringA(p8, p8l, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCR, b64, &sl);
    LocalFree(p8);
    static const char H[] = "-----BEGIN PRIVATE KEY-----\n", F[] = "-----END PRIVATE KEY-----\n";
    size_t n = strlen(H) + strlen(b64) + strlen(F);
    char *pem = malloc(b->len + n + 1);
    if (b->data) memcpy(pem, b->data, b->len);
    char *o = pem + b->len;
    o += sprintf(o, "%s%s%s", H, b64, F);
    free(b->data);
    b->data = pem;
    b->len = (size_t)(o - pem);
    free(b64);
    return 1;
}

int EVP_DigestSignFinal(EVP_MD_CTX *c, unsigned char *sig, size_t *siglen) {
    if (!c || !c->sign_key || !siglen) return 0;
    size_t need = (c->sign_key->bits + 7) / 8;
    if (!sig) {
        *siglen = need;
        return 1;
    }
    unsigned char dg[64];
    unsigned dn = 0;
    if (!EVP_DigestFinal_ex(c, dg, &dn)) return 0;
    BCRYPT_PKCS1_PADDING_INFO pad = {c->md->alg};
    ULONG out = 0;
    if (BCryptSignHash(c->sign_key->key, &pad, dg, dn, sig, (ULONG)*siglen, &out, BCRYPT_PAD_PKCS1) != 0) return 0;
    *siglen = out;
    return 1;
}
