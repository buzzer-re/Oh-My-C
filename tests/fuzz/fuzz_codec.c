/* fuzz_codec - small decoders fed untrusted text: base64, URL parse/resolve/percent-decoding, path normalization,
 * UTF-8 helpers and string splitting (src/core, src/net/http.c). Input: <selector byte><text>[\n\x01\n<second>]. */
#include "core/core.h"
#include "core/fs.h"
#include "net/http.h"
#include "fuzz.h"

FUZZ_SETUP {}

FUZZ_ENTRY {
    if (size > 1 << 16) return 0;
    unsigned sel = fuzz_byte(&data, &size);
    char *a, *b;
    size_t al, bl;
    fuzz_split(data, size, &a, &al, &b, &bl);
    switch (sel % 6) {
    case 0: {
        size_t n = 0;
        unsigned char *d = base64_decode(a, al, &n);
        if (d) {
            char *e = base64_encode(d, n);
            size_t n2 = 0;
            unsigned char *d2 = base64_decode(e, strlen(e), &n2);
            FUZZ_ASSERT(d2 && n2 == n && memcmp(d, d2, n) == 0);
            free(d2);
            free(e);
            free(d);
        }
        char *e = base64url_encode(a, al);
        free(e);
        break;
    }
    case 1: {
        url_t u;
        if (url_parse(a, &u) == 0) {
            FUZZ_ASSERT(u.scheme && u.host && u.path);
            url_free(&u);
        }
        char *r = url_resolve(a, b);
        free(r);
        char *d = url_decode(a);
        free(d);
        char *e = url_encode(a);
        char *d2 = url_decode(e);
        FUZZ_ASSERT(strcmp(d2, a) == 0);
        free(d2);
        free(e);
        break;
    }
    case 2: {
        char *n = path_normalize(a);
        if (n) {
            char *n2 = path_normalize(n);
            FUZZ_ASSERT(n2 && strcmp(n, n2) == 0);
            free(n2);
            free(n);
        }
        char *j = path_join(a, b);
        free(j);
        break;
    }
    case 3: {
        char *s = utf8_sanitize(a, al);
        FUZZ_ASSERT(utf8_valid(s, strlen(s)));
        (void)utf8_strlen(s);
        (void)str_display_width(s);
        free(s);
        for (size_t i = 0; i < al;) {
            uint32_t cp;
            int k = utf8_decode(a + i, al - i, &cp);
            FUZZ_ASSERT(k >= 1 && (size_t)k <= al - i);
            i += (size_t)k;
        }
        break;
    }
    case 4: {
        strvec_t v = str_split_lines(a, al);
        strvec_free(&v);
        v = str_split(a, bl ? b : ",");
        strvec_free(&v);
        char *r = str_replace(a, bl ? b : "x", "yy");
        free(r);
        (void)str_istr(a, b);
        (void)str_count(a, bl ? b : "a");
        (void)str_cmp_natural(a, b);
        char *t = str_trim(a);
        free(t);
        break;
    }
    case 5: {
        http_headers_t h = {0};
        http_headers_add(&h, a, b);
        (void)http_headers_get(&h, a);
        http_headers_del(&h, a);
        http_headers_free(&h);
        break;
    }
    }
    free(a);
    free(b);
    return 0;
}
