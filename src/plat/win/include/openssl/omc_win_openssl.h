/* The part of OpenSSL's API omc calls, on Windows: digests, HMAC, RSA-SHA256 signing and random bytes over CNG
 * (plat/win/crypto.c), and a TLS client over Schannel (plat/win/tls.c) with OpenSSL's non-blocking semantics
 * (SSL_ERROR_WANT_READ/WRITE) and its X509_V_ERR_* verify results, so net/http.c runs unchanged. No OpenSSL code. */
#ifndef OMC_WIN_OPENSSL_H
#define OMC_WIN_OPENSSL_H
#include <stddef.h>
#include <stdint.h>

/* ---------- digests ---------- */
#define SHA_DIGEST_LENGTH 20
#define SHA256_DIGEST_LENGTH 32
#define SHA512_DIGEST_LENGTH 64
#define EVP_MAX_MD_SIZE 64
typedef struct omc_win_evp_md EVP_MD;
typedef struct omc_win_evp_md_ctx EVP_MD_CTX;
typedef struct omc_win_evp_pkey EVP_PKEY;
typedef struct omc_win_engine ENGINE;
typedef struct omc_win_evp_pkey_ctx EVP_PKEY_CTX;
const EVP_MD *EVP_sha1(void);
const EVP_MD *EVP_sha256(void);
const EVP_MD *EVP_sha512(void);
int EVP_MD_size(const EVP_MD *md);
EVP_MD_CTX *EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX *c);
int EVP_DigestInit_ex(EVP_MD_CTX *c, const EVP_MD *md, ENGINE *e);
int EVP_DigestUpdate(EVP_MD_CTX *c, const void *d, size_t n);
int EVP_DigestFinal_ex(EVP_MD_CTX *c, unsigned char *md, unsigned int *n);
int EVP_Digest(const void *d, size_t n, unsigned char *md, unsigned int *mdn, const EVP_MD *type, ENGINE *e);
unsigned char *SHA1(const unsigned char *d, size_t n, unsigned char *md);
unsigned char *SHA256(const unsigned char *d, size_t n, unsigned char *md);
unsigned char *SHA512(const unsigned char *d, size_t n, unsigned char *md);
unsigned char *HMAC(const EVP_MD *md, const void *key, int keylen, const unsigned char *d, size_t n, unsigned char *out,
                    unsigned int *outlen);

/* ---------- keys and signatures (RSA private keys from PEM, PKCS#8 or PKCS#1) ---------- */
#define EVP_PKEY_RSA 6
typedef struct omc_win_bio BIO;
typedef int pem_password_cb(char *buf, int size, int rwflag, void *u);
BIO *BIO_new_mem_buf(const void *buf, int len);
int BIO_free(BIO *b);
EVP_PKEY *PEM_read_bio_PrivateKey(BIO *bp, EVP_PKEY **x, pem_password_cb *cb, void *u);
int EVP_PKEY_base_id(const EVP_PKEY *k);
void EVP_PKEY_free(EVP_PKEY *k);
int EVP_DigestSignInit(EVP_MD_CTX *c, EVP_PKEY_CTX **pctx, const EVP_MD *md, ENGINE *e, EVP_PKEY *k);
int EVP_DigestSignUpdate(EVP_MD_CTX *c, const void *d, size_t n);
int EVP_DigestSignFinal(EVP_MD_CTX *c, unsigned char *sig, size_t *siglen);
int EVP_DigestVerifyInit(EVP_MD_CTX *c, EVP_PKEY_CTX **pctx, const EVP_MD *md, ENGINE *e, EVP_PKEY *k);
int EVP_DigestVerifyUpdate(EVP_MD_CTX *c, const void *d, size_t n);
int EVP_DigestVerifyFinal(EVP_MD_CTX *c, const unsigned char *sig, size_t siglen);
/* RSA key generation and PKCS#8 PEM export (the unit tests make their own keys) */
typedef struct omc_win_bio_method BIO_METHOD;
typedef struct omc_win_evp_cipher EVP_CIPHER;
const BIO_METHOD *BIO_s_mem(void);
BIO *BIO_new(const BIO_METHOD *m);
long BIO_get_mem_data(BIO *b, char **pp);
EVP_PKEY_CTX *EVP_PKEY_CTX_new_id(int id, ENGINE *e);
void EVP_PKEY_CTX_free(EVP_PKEY_CTX *c);
int EVP_PKEY_keygen_init(EVP_PKEY_CTX *c);
int EVP_PKEY_CTX_set_rsa_keygen_bits(EVP_PKEY_CTX *c, int bits);
int EVP_PKEY_keygen(EVP_PKEY_CTX *c, EVP_PKEY **out);
int PEM_write_bio_PrivateKey(BIO *b, EVP_PKEY *k, const EVP_CIPHER *enc, unsigned char *kstr, int klen, pem_password_cb *cb,
                             void *u);

/* ---------- random, memory ---------- */
int RAND_bytes(unsigned char *buf, int n);
void OPENSSL_cleanse(void *p, size_t n);

/* ---------- errors ---------- */
unsigned long ERR_get_error(void);
unsigned long ERR_peek_error(void);
void ERR_clear_error(void);
void ERR_error_string_n(unsigned long e, char *buf, size_t len);
char *ERR_error_string(unsigned long e, char *buf);

/* ---------- TLS ---------- */
typedef struct omc_win_ssl_ctx SSL_CTX;
typedef struct omc_win_ssl SSL;
typedef struct omc_win_ssl_method SSL_METHOD;
typedef struct omc_win_x509_verify_param X509_VERIFY_PARAM;
typedef struct omc_win_x509_store_ctx X509_STORE_CTX;
typedef struct omc_win_init_settings OPENSSL_INIT_SETTINGS;
#define OPENSSL_INIT_LOAD_SSL_STRINGS 0x00200000L
#define TLS1_VERSION 0x0301
#define TLS1_1_VERSION 0x0302
#define TLS1_2_VERSION 0x0303
#define TLS1_3_VERSION 0x0304
#define SSL_VERIFY_NONE 0
#define SSL_VERIFY_PEER 1
#define SSL_MODE_AUTO_RETRY 0x4
#define SSL_SESS_CACHE_CLIENT 0x1
#define SSL_ERROR_NONE 0
#define SSL_ERROR_SSL 1
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_SYSCALL 5
#define SSL_ERROR_ZERO_RETURN 6
typedef int (*SSL_verify_cb)(int ok, X509_STORE_CTX *ctx);
int OPENSSL_init_ssl(uint64_t opts, const OPENSSL_INIT_SETTINGS *s);
const SSL_METHOD *TLS_client_method(void);
SSL_CTX *SSL_CTX_new(const SSL_METHOD *m);
void SSL_CTX_free(SSL_CTX *c);
int SSL_CTX_set_min_proto_version(SSL_CTX *c, int v);
void SSL_CTX_set_verify(SSL_CTX *c, int mode, SSL_verify_cb cb);
int SSL_CTX_set_default_verify_paths(SSL_CTX *c);
int SSL_CTX_load_verify_locations(SSL_CTX *c, const char *file, const char *dir);
long SSL_CTX_set_mode(SSL_CTX *c, long mode);
long SSL_CTX_set_session_cache_mode(SSL_CTX *c, long mode);
int SSL_CTX_set_alpn_protos(SSL_CTX *c, const unsigned char *protos, unsigned int len);
SSL *SSL_new(SSL_CTX *c);
void SSL_free(SSL *s);
int SSL_set_fd(SSL *s, int fd);
long SSL_set_tlsext_host_name(SSL *s, const char *name);
void SSL_set_verify(SSL *s, int mode, SSL_verify_cb cb);
int SSL_set1_host(SSL *s, const char *host);
X509_VERIFY_PARAM *SSL_get0_param(SSL *s);
int X509_VERIFY_PARAM_set1_ip_asc(X509_VERIFY_PARAM *p, const char *ip);
int SSL_connect(SSL *s);
int SSL_read(SSL *s, void *buf, int n);
int SSL_write(SSL *s, const void *buf, int n);
int SSL_shutdown(SSL *s);
int SSL_get_error(const SSL *s, int ret);
long SSL_get_verify_result(const SSL *s);
int SSL_pending(const SSL *s);

/* ---------- certificate verification results (OpenSSL's numbers) ---------- */
#define X509_V_OK 0
#define X509_V_ERR_UNSPECIFIED 1
#define X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT 2
#define X509_V_ERR_CERT_SIGNATURE_FAILURE 7
#define X509_V_ERR_CERT_NOT_YET_VALID 9
#define X509_V_ERR_CERT_HAS_EXPIRED 10
#define X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT 18
#define X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN 19
#define X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY 20
#define X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE 21
#define X509_V_ERR_CERT_CHAIN_TOO_LONG 22
#define X509_V_ERR_CERT_REVOKED 23
#define X509_V_ERR_INVALID_CA 24
#define X509_V_ERR_INVALID_PURPOSE 26
#define X509_V_ERR_CERT_UNTRUSTED 27
#define X509_V_ERR_CERT_REJECTED 28
#define X509_V_ERR_HOSTNAME_MISMATCH 62
#define X509_V_ERR_IP_ADDRESS_MISMATCH 64
const char *X509_verify_cert_error_string(long n);

#endif
