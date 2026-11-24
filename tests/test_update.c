/* test_update.c - `omc update` mechanics: version comparison (omp pi-utils version.test.ts cases), the reported-version
 * parse, SHA256SUMS lookup, the strict tar.gz member reader (src/core/tarx.c), source-checkout detection and the
 * auto-update status file. */
#include "coding/selfupdate.h"
#include "core/fs.h"
#include "core/tarx.h"

#include <zlib.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                      \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void versions(void) {
    static const struct {
        const char *a, *b;
        int want;
    } T[] = {
        {" 1.2.3 ", "1.2.3", 0}, {"v1.2.3", "1.2.3", 0}, {"V1.2.3", "v1.2.3", 0}, {" v1.2.3 ", "1.2.3", 0},
        {"1.2", "1.2.0", 0}, {"1", "1.0.0.0", 0}, {"1.2", "1.2.1", -1}, {"1.2.3", "1.2", 1},
        {"1.2.3.4.5", "1.2.3.4.5", 0}, {"1.2.3.4", "1.2.3.5", -1}, {"1.0.0.1", "1.0.0", 1},
        {"1.0.0-beta", "1.0.0", -1}, {"1.0.0", "1.0.0-rc.1", 1}, {"v1.0.0-beta", "1.0.0", -1},
        {"1.1.0-alpha", "1.0.0-beta", 1}, {"1.0.0-alpha", "1.0.0-beta", -1}, {"1.0.0-rc.1", "1.0.0-rc.2", -1},
        {"1.0.0-rc.9", "1.0.0-rc.10", -1}, {"1.0.0-alpha", "1.0.0-alpha.1", -1}, {"1.0.0-1", "1.0.0-alpha", -1},
        {"1.0.0-beta.2", "1.0.0-beta.1", 1}, {"1.0.1+linux", "1.0.0", 1}, {"1.0.0-rc.1+abc", "1.0.0-rc.1+xyz", 0},
        {"1.0.0+build1", "1.0.0+build2", 0}, {"1.0.0+linux", "1.0.1+linux", -1}, {"v1.2.3+meta", "1.2.3", 0},
        {"1.2.x", "1.2.0", 0}, {"1.x", "1.0", 0}, {"1.2.x", "1.2.1", -1}, {"not_a_version", "0", 0}, {"", "", 0},
        {"v", "", 0}, {"not-a-version", "1.0.0", -1}, {"3.0.0", "1.0.0", 1}, {"1.0.0", "3.0.0", -1},
        {"1.2.99999999999999999999", "1.2.100000000000000000000", -1},
        /* part by part, never as strings */
        {"18.22.0", "18.9.9", 1}, {"18.18.7", "18.22.0", -1}, {"99.0.0", "18.22.0", 1}, {"18.022.0", "18.22.0", 0},
    };
    for (size_t i = 0; i < ARRAY_LEN(T); i++) {
        int got = su_compare_versions(T[i].a, T[i].b);
        if (got != T[i].want) fprintf(stderr, "compare(%s, %s) = %d, want %d\n", T[i].a, T[i].b, got, T[i].want);
        CHECK(got == T[i].want);
        CHECK(su_compare_versions(T[i].b, T[i].a) == -T[i].want);
    }
    char *v = su_parse_reported_version("omc/18.22.0\n");
    CHECK(v && !strcmp(v, "18.22.0"));
    free(v);
    v = su_parse_reported_version("omc/1.2.3-canary.1");
    CHECK(v && !strcmp(v, "1.2.3-canary.1"));
    free(v);
    CHECK(!su_parse_reported_version("omp/18.22.0"));
    CHECK(!su_parse_reported_version("omc/18.22"));
    CHECK(!su_parse_reported_version("omc/18.22.0 extra"));
    CHECK(!su_parse_reported_version("fake omc 99"));
}

static void sums(void) {
    const char *s = "ab01  other.tar.gz\n"
                    "3032B9AEC0CC276BF667C7FEE1EA011DEB8962E00B6B9C56BB06029B47350F38 *omc-1.0.0-linux-arm64.tar.gz\r\n"
                    "1111111111111111111111111111111111111111111111111111111111111111  omc-1.0.0.tar.gz\n";
    char *h = su_sums_lookup(s, "omc-1.0.0-linux-arm64.tar.gz");
    CHECK(h && !strcmp(h, "3032b9aec0cc276bf667c7fee1ea011deb8962e00b6b9c56bb06029b47350f38"));
    free(h);
    h = su_sums_lookup(s, "omc-1.0.0.tar.gz");
    CHECK(h && h[0] == '1');
    free(h);
    CHECK(!su_sums_lookup(s, "other.tar.gz"));            /* not a 64-digit digest */
    CHECK(!su_sums_lookup(s, "omc-1.0.0-linux-arm64.tar")); /* exact names only */
    CHECK(!su_sums_lookup("", "x"));
    char *d = su_sha256_hex("abc", 3);
    CHECK(!strcmp(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    free(d);
}

/* ---------- tar images built by hand ---------- */

/* n - 1 zero-padded octal digits and a NUL */
static void octal(unsigned char *f, size_t n, uint64_t v) {
    f[n - 1] = 0;
    for (size_t i = n - 1; i-- > 0; v >>= 3) f[i] = (unsigned char)('0' + (v & 7));
}

/* one header (+ data, padded); `prefix` goes in the ustar prefix field */
static void entry(sb_t *t, const char *name, const char *prefix, char type, const char *data, size_t len) {
    unsigned char h[512] = {0};
    memcpy(h, name, MIN(strlen(name), (size_t)100));
    octal(h + 100, 8, 0755);
    octal(h + 108, 8, 0);
    octal(h + 116, 8, 0);
    octal(h + 124, 12, len);
    octal(h + 136, 12, 1700000000);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar\0" "00", 8);
    if (prefix) memcpy(h + 345, prefix, strlen(prefix));
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    sb_appendn(t, (const char *)h, 512);
    sb_appendn(t, data, len);
    while (t->len % 512) sb_putc(t, 0);
}

static void end(sb_t *t) {
    for (int i = 0; i < 1024; i++) sb_putc(t, 0);
}

static int find(const sb_t *t, const char *name, size_t max, const char **data, size_t *len) {
    char *err = NULL;
    int rc = tarx_find(t->s, t->len, name, max, data, len, &err);
    CHECK((rc < 0) == (err != NULL));
    free(err);
    return rc;
}

static char *gz(const char *data, size_t n, size_t *out_len) {
    z_stream z;
    memset(&z, 0, sizeof z);
    deflateInit2(&z, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    size_t cap = deflateBound(&z, n) + 64;
    char *out = xmalloc(cap);
    z.next_in = (Bytef *)data;
    z.avail_in = (uInt)n;
    z.next_out = (Bytef *)out;
    z.avail_out = (uInt)cap;
    deflate(&z, Z_FINISH);
    *out_len = cap - z.avail_out;
    deflateEnd(&z);
    return out;
}

static void tar_reader(void) {
    const char *bin = "#!/bin/sh\necho omc/9.9.9\n", *d;
    size_t bl = strlen(bin), len;
    const char *want = "omc-9.9.9-linux-arm64/omc";
    sb_t t = SB_INIT;
    entry(&t, "omc-9.9.9-linux-arm64/", NULL, '5', "", 0);
    entry(&t, "omc-9.9.9-linux-arm64/README.md", NULL, '0', "hi\n", 3);
    entry(&t, want, NULL, '0', bin, bl);
    end(&t);
    CHECK(find(&t, want, 1 << 20, &d, &len) == 0 && len == bl && !memcmp(d, bin, bl));
    CHECK(find(&t, "./omc-9.9.9-linux-arm64/omc", 1 << 20, &d, &len) == 0);
    CHECK(find(&t, "omc", 1 << 20, &d, &len) == 1);
    CHECK(find(&t, "omc-9.9.9-linux-arm64", 1 << 20, &d, &len) == 1); /* a directory */
    CHECK(find(&t, want, 4, &d, &len) == -1);                          /* over the size cap */
    /* the archive as gzip: inflates back; over a cap, truncated, or not gzip is refused */
    size_t zl, ol;
    char *z = gz(t.s, t.len, &zl), *err = NULL;
    char *o = tarx_gunzip(z, zl, 1 << 20, &ol, &err);
    CHECK(o && ol == t.len && !memcmp(o, t.s, ol) && !err);
    free(o);
    CHECK(!tarx_gunzip(z, zl, 1000, &ol, &err) && err && strstr(err, "limit"));
    free(err);
    CHECK(!tarx_gunzip(z, zl / 2, 1 << 20, &ol, &err) && err && strstr(err, "truncated"));
    free(err);
    CHECK(!tarx_gunzip(t.s, t.len, 1 << 20, &ol, &err) && err && !strcmp(err, "not a gzip file"));
    free(err);
    /* concatenated gzip members are one stream */
    sb_t two = SB_INIT;
    sb_appendn(&two, z, zl);
    sb_appendn(&two, z, zl);
    o = tarx_gunzip(two.s, two.len, 1 << 20, &ol, &err);
    CHECK(o && ol == 2 * t.len);
    free(o);
    sb_free(&two);
    z[zl / 2] ^= 0x55; /* corrupt deflate data (or a bad CRC) */
    o = tarx_gunzip(z, zl, 1 << 20, &ol, &err);
    CHECK(!o && err);
    free(err);
    free(z);
    /* a corrupt header checksum anywhere refuses the whole archive */
    sb_t bad = SB_INIT;
    sb_appendn(&bad, t.s, t.len);
    bad.s[512 * 2 + 3] ^= 1; /* README's data: fine */
    CHECK(find(&bad, want, 1 << 20, &d, &len) == 0);
    bad.s[0] ^= 1; /* the first header's name */
    CHECK(find(&bad, want, 1 << 20, &d, &len) == -1);
    sb_free(&bad);
    /* truncated member data, and a header cut short */
    sb_t cut = SB_INIT;
    sb_appendn(&cut, t.s, 512 * 4 + 5);
    CHECK(find(&cut, want, 1 << 20, &d, &len) == -1);
    sb_clear(&cut);
    sb_appendn(&cut, t.s, 512 * 3 + 100);
    CHECK(find(&cut, want, 1 << 20, &d, &len) == -1);
    sb_free(&cut);
    sb_free(&t);
    /* a link named like the binary is not the binary; two binaries are refused */
    entry(&t, want, NULL, '2', "", 0);
    end(&t);
    CHECK(find(&t, want, 1 << 20, &d, &len) == 1);
    sb_clear(&t);
    entry(&t, want, NULL, '0', bin, bl);
    entry(&t, want, NULL, '0', "evil", 4);
    end(&t);
    CHECK(find(&t, want, 1 << 20, &d, &len) == -1);
    sb_clear(&t);
    /* ustar prefix, GNU long name, pax path and size */
    entry(&t, "omc", "omc-9.9.9-linux-arm64", '0', bin, bl);
    end(&t);
    CHECK(find(&t, want, 1 << 20, &d, &len) == 0 && len == bl);
    sb_clear(&t);
    entry(&t, "././@LongLink", NULL, 'L', want, strlen(want) + 1);
    entry(&t, "truncated-name", NULL, '0', bin, bl);
    end(&t);
    CHECK(find(&t, want, 1 << 20, &d, &len) == 0 && len == bl);
    CHECK(find(&t, "truncated-name", 1 << 20, &d, &len) == 1);
    sb_clear(&t);
    char *rec = xasprintf("%zu path=%s\n", strlen(want) + 9, want); /* "NN path=<name>\n": 2 digits + 7 */
    entry(&t, "PaxHeader", NULL, 'x', rec, strlen(rec));
    entry(&t, "short", NULL, '0', bin, bl);
    end(&t);
    CHECK(find(&t, want, 1 << 20, &d, &len) == 0 && len == bl);
    free(rec);
    sb_clear(&t);
    entry(&t, "PaxHeader", NULL, 'x', "12 path=x\n", 10); /* a record length past the header's data */
    entry(&t, want, NULL, '0', bin, bl);
    end(&t);
    CHECK(find(&t, want, 1 << 20, &d, &len) == -1);
    sb_clear(&t);
    entry(&t, "PaxHeader", NULL, 'x', "18 size=999999999\n", 18); /* a size past the archive */
    entry(&t, want, NULL, '0', bin, bl);
    end(&t);
    CHECK(find(&t, want, 1 << 20, &d, &len) == -1);
    sb_clear(&t);
    /* base-256 size field */
    entry(&t, want, NULL, '0', bin, bl);
    end(&t);
    memset(t.s + 124, 0, 12);
    t.s[124] = (char)0x80;
    t.s[135] = (char)bl;
    memset(t.s + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (unsigned char)t.s[i];
    snprintf(t.s + 148, 8, "%06o", sum);
    CHECK(find(&t, want, 1 << 20, &d, &len) == 0 && len == bl);
    sb_clear(&t);
    /* no end-of-archive blocks: fine when the last member ends on a block boundary */
    entry(&t, want, NULL, '0', bin, bl);
    CHECK(find(&t, want, 1 << 20, &d, &len) == 0);
    CHECK(tarx_find("", 0, want, 1, &d, &len, &(char *){NULL}) == 1);
    sb_free(&t);
}

/* ---------- zip (the Windows release archive) ---------- */

static void put16(sb_t *b, unsigned v) {
    sb_putc(b, (char)(v & 0xff));
    sb_putc(b, (char)(v >> 8 & 0xff));
}
static void put32(sb_t *b, uint32_t v) {
    put16(b, v & 0xffff);
    put16(b, v >> 16);
}

typedef struct {
    const char *name, *data;
    int method; /* 0 stored, 8 deflate */
} zent_t;

/* a zip of `ents` the way build-tools/zipdir.py writes one (sizes in the local header, no data descriptor) */
static void mkzip(sb_t *z, const zent_t *ents, size_t n) {
    sb_t cd = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        size_t ul = strlen(ents[i].data), cl = ul;
        char *comp = NULL;
        if (ents[i].method == 8) {
            z_stream s;
            memset(&s, 0, sizeof s);
            deflateInit2(&s, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
            size_t cap = deflateBound(&s, ul) + 16;
            comp = xmalloc(cap);
            s.next_in = (Bytef *)ents[i].data;
            s.avail_in = (uInt)ul;
            s.next_out = (Bytef *)comp;
            s.avail_out = (uInt)cap;
            deflate(&s, Z_FINISH);
            cl = cap - s.avail_out;
            deflateEnd(&s);
        }
        uint32_t crc = (uint32_t)crc32(0L, (const Bytef *)ents[i].data, (uInt)ul);
        size_t off = z->len, nl = strlen(ents[i].name);
        put32(z, 0x04034b50), put16(z, 20), put16(z, 0), put16(z, ents[i].method), put32(z, 0), put32(z, crc);
        put32(z, (uint32_t)cl), put32(z, (uint32_t)ul), put16(z, nl), put16(z, 0);
        sb_appendn(z, ents[i].name, nl);
        sb_appendn(z, comp ? comp : ents[i].data, cl);
        put32(&cd, 0x02014b50), put16(&cd, 20), put16(&cd, 20), put16(&cd, 0), put16(&cd, ents[i].method);
        put32(&cd, 0), put32(&cd, crc), put32(&cd, (uint32_t)cl), put32(&cd, (uint32_t)ul), put16(&cd, nl);
        put16(&cd, 0), put16(&cd, 0), put16(&cd, 0), put16(&cd, 0), put32(&cd, 0), put32(&cd, (uint32_t)off);
        sb_appendn(&cd, ents[i].name, nl);
        free(comp);
    }
    size_t cd_off = z->len;
    sb_appendn(z, cd.s, cd.len);
    put32(z, 0x06054b50), put16(z, 0), put16(z, 0), put16(z, n), put16(z, n), put32(z, (uint32_t)cd.len);
    put32(z, (uint32_t)cd_off), put16(z, 0);
    sb_free(&cd);
}

static int unzip(const sb_t *z, const char *name, size_t max, char **out, size_t *len) {
    char *err = NULL;
    int rc = zipx_extract(z->s, z->len, name, max, out, len, &err);
    CHECK((rc < 0) == (err != NULL));
    CHECK((rc == 0) == (*out != NULL));
    free(err);
    return rc;
}

static void zip_reader(void) {
    const char *want = "omc-9.9.9-windows-x86_64/omc.exe";
    char big[3000];
    for (size_t i = 0; i < sizeof big - 1; i++) big[i] = "MZomc"[i % 5];
    big[sizeof big - 1] = 0;
    zent_t ents[] = {{"omc-9.9.9-windows-x86_64/README.md", "hi\n", 0}, {want, big, 8},
                     {"omc-9.9.9-windows-x86_64/lib/omc.pc", "prefix=x\n", 8}};
    sb_t z = SB_INIT;
    mkzip(&z, ents, 3);
    char *o;
    size_t len;
    CHECK(unzip(&z, want, 1 << 20, &o, &len) == 0 && len == strlen(big) && !strcmp(o, big));
    free(o);
    CHECK(unzip(&z, "omc-9.9.9-windows-x86_64/README.md", 1 << 20, &o, &len) == 0 && !strcmp(o, "hi\n"));
    free(o);
    CHECK(unzip(&z, "omc.exe", 1 << 20, &o, &len) == 1);
    CHECK(unzip(&z, want, 100, &o, &len) == -1); /* over the size cap */
    /* a trailing archive comment is fine */
    sb_t c = SB_INIT;
    sb_appendn(&c, z.s, z.len);
    c.s[c.len - 2] = 4;
    sb_appendn(&c, "note", 4);
    CHECK(unzip(&c, want, 1 << 20, &o, &len) == 0);
    free(o);
    sb_free(&c);
    /* not a zip, truncated, a corrupt member or CRC, a duplicate name: refused */
    sb_t t = SB_INIT;
    sb_append(&t, "just text, no zip here at all.....");
    CHECK(unzip(&t, want, 1 << 20, &o, &len) == -1);
    sb_free(&t);
    sb_t h = SB_INIT;
    sb_appendn(&h, z.s, z.len / 2);
    CHECK(unzip(&h, want, 1 << 20, &o, &len) == -1);
    sb_free(&h);
    sb_t bad = SB_INIT;
    sb_appendn(&bad, z.s, z.len);
    size_t data_at = 30 + strlen(ents[0].name) + 3 + 30 + strlen(want); /* the second member's data */
    bad.s[data_at + 5] ^= 0x5a;
    CHECK(unzip(&bad, want, 1 << 20, &o, &len) == -1);
    bad.s[data_at + 5] ^= 0x5a;
    bad.s[30 + strlen(ents[0].name)] ^= 1; /* README's stored data: its CRC no longer matches */
    CHECK(unzip(&bad, "omc-9.9.9-windows-x86_64/README.md", 1 << 20, &o, &len) == -1);
    CHECK(unzip(&bad, want, 1 << 20, &o, &len) == 0); /* the binary is still intact */
    free(o);
    sb_free(&bad);
    zent_t dup[] = {{want, "a", 0}, {want, "b", 0}};
    sb_t d = SB_INIT;
    mkzip(&d, dup, 2);
    CHECK(unzip(&d, want, 1 << 20, &o, &len) == -1);
    sb_free(&d);
    sb_free(&z);
}

static void checkout(void) {
    char *root = fs_mktempdir("omc-upd-test");
    char *bin = path_join(root, "build/obj/Linux-x/omc");
    char *mk = path_join(root, "Makefile"), *core = path_join(root, "src/core/core.h"), *git = path_join(root, ".git");
    char *bdir = path_dirname(bin), *cdir = path_dirname(core);
    fs_mkdir_p(bdir, 0700);
    fs_mkdir_p(cdir, 0700);
    free(bdir);
    free(cdir);
    fs_write(bin, "x", 1);
    fs_write(mk, "", 0);
    fs_write(core, "", 0);
    CHECK(!su_is_source_checkout(bin)); /* no .git: not a work tree */
    fs_mkdir_p(git, 0700);
    CHECK(su_is_source_checkout(bin));
    char *build = path_join(root, "build/omc");
    CHECK(su_is_source_checkout(build));
    char *other = path_join(root, "bin/omc");
    CHECK(!su_is_source_checkout(other)); /* not under build/ */
    CHECK(!su_is_source_checkout("/usr/local/bin/omc"));
    fs_rm_rf(root);
    free(other);
    free(build);
    free(git);
    free(core);
    free(mk);
    free(bin);
    free(root);
}

static void state(void) {
    char *root = fs_mktempdir("omc-upd-state"), *p = path_join(root, "auto-update.json");
    su_state_t s = {.phase = SU_PH_DOWNLOADING, .target = "2.0.0", .version = "2.0.0", .received = 5, .total = 10,
                    .at = 1700000000000LL};
    CHECK(su_state_write(p, &s) == 0);
    su_state_t r;
    CHECK(su_state_read(p, &r) && r.phase == SU_PH_DOWNLOADING && r.received == 5 && r.total == 10 &&
          !strcmp(r.target, "2.0.0") && r.at == 1700000000000LL && !r.error);
    CHECK(!su_should_start_worker(&r, "2.0.0", r.at + 1000));
    CHECK(su_should_start_worker(&r, "2.0.0", r.at + SU_RETRY_AFTER_MS));
    CHECK(su_should_start_worker(&r, "3.0.0", r.at + 1000));
    CHECK(su_should_start_worker(NULL, "3.0.0", 0));
    su_state_free(&r);
    fs_write(p, "{\"phase\":\"done\"", 15); /* mid-write */
    CHECK(!su_state_read(p, &r));
    fs_write(p, "{\"phase\":\"bogus\",\"at\":1}", 24);
    CHECK(!su_state_read(p, &r));
    /* the worker's lock: held while a fd keeps it, probed without taking it */
    CHECK(!su_lock_held(p));
    int fd = su_lock_try(p);
    CHECK(fd >= 0);
    su_lock_release(fd);
    CHECK(!su_lock_held(p));
    fs_rm_rf(root);
    free(p);
    free(root);
}

int main(void) {
    versions();
    sums();
    tar_reader();
    zip_reader();
    checkout();
    state();
    bool prebuilt;
    const char *plat = su_platform(&prebuilt);
    CHECK(plat && strchr(plat, '-'));
    char *a = su_asset_name("1.2.3");
#ifdef _WIN32
    CHECK(str_starts(a, "omc-1.2.3-") && str_ends(a, ".zip") && prebuilt == !strcmp(plat, "windows-x86_64"));
#else
    CHECK(str_starts(a, "omc-1.2.3-") && str_ends(a, ".tar.gz"));
#endif
    free(a);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("test_update ok\n");
    return failures ? 1 : 0;
}
