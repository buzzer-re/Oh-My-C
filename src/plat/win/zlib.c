/* plat/win/zlib.c - DEFLATE (RFC 1951) with zlib (RFC 1950) and gzip (RFC 1952) framing, for the Windows build,
 * written for omc (no zlib code). zlib's API and stream semantics: inflate() takes any split of input and output and
 * resumes where it stopped (the HTTP layer feeds it gzip'd event streams as they arrive); deflate() compresses what
 * it was given once flushed (LZ77 over a 32 KiB window with hash chains and lazy matching, then the cheapest of
 * stored, fixed and dynamic Huffman blocks). Input is untrusted: every table and length is bounds-checked. */
#include <zlib.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *zlibVersion(void) { return ZLIB_VERSION; }

/* ---------- checksums ---------- */

static uint32_t g_crc_table[8][256];

static void crc_init(void) {
    static volatile int done;
    if (done) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[0][n] = c;
    }
    for (uint32_t n = 0; n < 256; n++)
        for (int t = 1; t < 8; t++) g_crc_table[t][n] = g_crc_table[0][g_crc_table[t - 1][n] & 0xff] ^ (g_crc_table[t - 1][n] >> 8);
    done = 1;
}

uLong crc32_z(uLong crc, const Bytef *buf, size_t len) {
    if (!buf) return 0;
    crc_init();
    uint32_t c = ~(uint32_t)crc;
    while (len >= 8) {
        uint32_t a = c ^ ((uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24);
        c = g_crc_table[7][a & 0xff] ^ g_crc_table[6][(a >> 8) & 0xff] ^ g_crc_table[5][(a >> 16) & 0xff] ^
            g_crc_table[4][a >> 24] ^ g_crc_table[3][buf[4]] ^ g_crc_table[2][buf[5]] ^ g_crc_table[1][buf[6]] ^
            g_crc_table[0][buf[7]];
        buf += 8;
        len -= 8;
    }
    while (len--) c = g_crc_table[0][(c ^ *buf++) & 0xff] ^ (c >> 8);
    return ~c;
}

uLong crc32(uLong crc, const Bytef *buf, uInt len) { return crc32_z(crc, buf, len); }

uLong adler32(uLong adler, const Bytef *buf, uInt len) {
    if (!buf) return 1;
    uint32_t a = adler & 0xffff, b = (adler >> 16) & 0xffff;
    while (len) {
        uInt n = len < 5552 ? len : 5552;
        len -= n;
        while (n--) {
            a += *buf++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (uLong)(b << 16 | a);
}

/* ---------- shared tables ---------- */

static const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t CLEN_ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* ---------- inflate ---------- */

#define FAST_BITS 9
#define WSIZE 32768u

typedef struct {
    uint16_t fast[1 << FAST_BITS]; /* (len << 12) | sym for codes of <= FAST_BITS bits; 0 = slow path */
    uint16_t count[16];
    uint16_t symbol[320];
    int nsym;
} huff_t;

enum {
    I_HEAD, I_GZ_FIXED, I_GZ_XLEN, I_GZ_EXTRA, I_GZ_NAME, I_GZ_COMMENT, I_GZ_HCRC, I_BLOCK, I_STORED_LEN, I_STORED,
    I_DYN_HDR, I_DYN_CLEN, I_DYN_LENS, I_CODES, I_LENEXT, I_DIST, I_DISTEXT, I_COPY, I_CHECK, I_DONE, I_BAD
};

struct internal_state {
    int is_deflate;
    /* inflate */
    int wrap; /* 0 raw, 1 zlib, 2 gzip, 3 auto */
    int mode, last, btype;
    uint64_t hold;
    unsigned bits;
    unsigned char *window;
    unsigned wpos, wfill;
    uint32_t check; /* running adler32 or crc32 of the output */
    uint64_t out_total;
    huff_t lit, dist;
    int use_fixed;
    /* sub-state */
    unsigned need, got, flags, xlen, stored_left;
    unsigned char hdr[10];
    unsigned hlit, hdist, hclen, nlens;
    uint8_t lens[320];
    unsigned sym, length, distance;
    /* deflate */
    int level, dwrap, finished;
    unsigned char *in;
    size_t in_len, in_cap;
    unsigned char *out;
    size_t out_len, out_pos;
    int header_done;
    uint32_t dcheck;
    uint64_t dtotal;
};

static unsigned rev_bits(unsigned code, unsigned len) {
    unsigned r = 0;
    while (len--) r = (r << 1) | (code & 1), code >>= 1;
    return r;
}

/* canonical code from lengths; false on an over-subscribed set (incomplete sets are allowed, as zlib does for one
 * distance code) */
static bool huff_build(huff_t *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
    h->nsym = n;
    /* fast table */
    unsigned code = 0;
    int idx = 0;
    for (unsigned l = 1; l <= FAST_BITS; l++) {
        for (int k = 0; k < h->count[l]; k++) {
            unsigned r = rev_bits(code + (unsigned)k, l);
            for (unsigned e = r; e < (1u << FAST_BITS); e += 1u << l) h->fast[e] = (uint16_t)(l << 12 | h->symbol[idx + k]);
        }
        idx += h->count[l];
        code = (code + h->count[l]) << 1;
    }
    return true;
}

/* decode one symbol from the bit buffer without consuming on failure: >= 0 symbol (bits consumed), -1 need more
 * bits, -2 invalid code */
static int huff_decode(struct internal_state *s, const huff_t *h) {
    if (s->bits >= FAST_BITS || s->bits >= 1) {
        uint16_t f = h->fast[s->hold & ((1u << FAST_BITS) - 1)];
        if (f && (unsigned)(f >> 12) <= s->bits) {
            s->hold >>= f >> 12;
            s->bits -= f >> 12;
            return f & 0xfff;
        }
    }
    int code = 0, first = 0, index = 0;
    for (unsigned len = 1; len <= 15; len++) {
        if (len > s->bits) return -1;
        code |= (int)((s->hold >> (len - 1)) & 1);
        int count = h->count[len];
        if (code - count < first) {
            s->hold >>= len;
            s->bits -= len;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

static void fixed_tables(struct internal_state *s) {
    uint8_t l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    huff_build(&s->lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    huff_build(&s->dist, l, 30);
}

static int inflate_setup(z_streamp strm, int wb) {
    if (!strm) return Z_STREAM_ERROR;
    struct internal_state *s = calloc(1, sizeof *s);
    if (!s) return Z_MEM_ERROR;
    s->window = malloc(WSIZE);
    if (!s->window) {
        free(s);
        return Z_MEM_ERROR;
    }
    strm->state = s;
    strm->msg = NULL;
    strm->total_in = strm->total_out = 0;
    if (inflateReset2(strm, wb) != Z_OK) {
        inflateEnd(strm);
        return Z_STREAM_ERROR;
    }
    return Z_OK;
}

int inflateInit2_(z_streamp strm, int windowBits, const char *version, int stream_size) {
    (void)version;
    (void)stream_size;
    return inflate_setup(strm, windowBits);
}

int inflateInit_(z_streamp strm, const char *version, int stream_size) { return inflateInit2_(strm, MAX_WBITS, version, stream_size); }

int inflateReset2(z_streamp strm, int windowBits) {
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    struct internal_state *s = strm->state;
    int wrap;
    if (windowBits < 0) {
        if (windowBits < -15 || windowBits > -8) return Z_STREAM_ERROR;
        wrap = 0;
    } else if (windowBits >= 48 || (windowBits > 32 && windowBits < 40) || (windowBits > 16 && windowBits < 24)) {
        return Z_STREAM_ERROR;
    } else if (windowBits >= 32) { /* 32 + 8..15 (or 32 alone): zlib or gzip, by the first byte */
        wrap = 3;
    } else if (windowBits >= 16) { /* 16 + 8..15: gzip */
        wrap = 2;
    } else if (windowBits >= 8 || windowBits == 0) {
        wrap = 1;
    } else {
        return Z_STREAM_ERROR;
    }
    unsigned char *w = s->window;
    memset(s, 0, sizeof *s);
    s->window = w;
    s->wrap = wrap;
    s->mode = wrap ? I_HEAD : I_BLOCK;
    s->check = wrap == 2 ? 0 : 1;
    strm->adler = s->check;
    strm->total_in = strm->total_out = 0;
    strm->msg = NULL;
    return Z_OK;
}

int inflateReset(z_streamp strm) {
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    int wb = strm->state->wrap == 0 ? -15 : strm->state->wrap == 2 ? 31 : strm->state->wrap == 3 ? 47 : 15;
    return inflateReset2(strm, wb);
}

int inflateEnd(z_streamp strm) {
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    free(strm->state->window);
    free(strm->state);
    strm->state = NULL;
    return Z_OK;
}

/* pull whole bytes into the bit buffer until it holds n bits; false when input ran out first */
static bool need_bits(struct internal_state *s, z_streamp strm, unsigned n) {
    while (s->bits < n) {
        if (!strm->avail_in) return false;
        s->hold |= (uint64_t)*strm->next_in++ << s->bits;
        strm->avail_in--;
        strm->total_in++;
        s->bits += 8;
    }
    return true;
}

static unsigned take_bits(struct internal_state *s, unsigned n) {
    unsigned v = (unsigned)(s->hold & ((1ull << n) - 1));
    s->hold >>= n;
    s->bits -= n;
    return v;
}

/* the next byte: from the bit buffer if a whole one is there, else from the input; -1 when none */
static int next_byte(struct internal_state *s, z_streamp strm) {
    if (!need_bits(s, strm, 8)) return -1;
    return (int)take_bits(s, 8);
}

static int put_byte(struct internal_state *s, z_streamp strm, unsigned char c) {
    *strm->next_out++ = c;
    strm->avail_out--;
    strm->total_out++;
    s->window[s->wpos] = c;
    s->wpos = (s->wpos + 1) & (WSIZE - 1);
    if (s->wfill < WSIZE) s->wfill++;
    s->out_total++;
    return 0;
}

static int bad(z_streamp strm, const char *msg) {
    strm->msg = msg;
    strm->state->mode = I_BAD;
    return Z_DATA_ERROR;
}

int inflate(z_streamp strm, int flush) {
    if (!strm || !strm->state || (!strm->next_out && strm->avail_out) || (!strm->next_in && strm->avail_in)) return Z_STREAM_ERROR;
    struct internal_state *s = strm->state;
    if (s->mode == I_BAD) return Z_DATA_ERROR;
    uLong in0 = strm->avail_in, out0 = strm->avail_out;
    Bytef *out_start = strm->next_out;
    int ret = Z_OK;
    for (;;) {
        switch (s->mode) {
        case I_HEAD: {
            if (s->wrap == 3) {
                if (!need_bits(s, strm, 8)) goto done;
                s->wrap = (s->hold & 0xff) == 0x1f ? 2 : 1;
                s->check = s->wrap == 2 ? 0 : 1;
            }
            if (s->wrap == 2) {
                s->mode = I_GZ_FIXED;
                s->got = 0;
                break;
            }
            if (!need_bits(s, strm, 16)) goto done;
            unsigned cmf = take_bits(s, 8), flg = take_bits(s, 8);
            if ((cmf * 256 + flg) % 31) return bad(strm, "incorrect header check");
            if ((cmf & 15) != 8) return bad(strm, "unknown compression method");
            if ((cmf >> 4) > 7) return bad(strm, "invalid window size");
            if (flg & 0x20) return bad(strm, "need dictionary");
            s->mode = I_BLOCK;
            break;
        }
        case I_GZ_FIXED:
            while (s->got < 10) {
                int c = next_byte(s, strm);
                if (c < 0) goto done;
                s->hdr[s->got++] = (unsigned char)c;
            }
            if (s->hdr[0] != 0x1f || s->hdr[1] != 0x8b) return bad(strm, "incorrect header check");
            if (s->hdr[2] != 8) return bad(strm, "unknown compression method");
            s->flags = s->hdr[3];
            s->got = 0;
            s->mode = I_GZ_XLEN;
            break;
        case I_GZ_XLEN:
            if (s->flags & 4) {
                while (s->got < 2) {
                    int c = next_byte(s, strm);
                    if (c < 0) goto done;
                    s->xlen |= (unsigned)c << (8 * s->got++);
                }
            }
            s->mode = I_GZ_EXTRA;
            break;
        case I_GZ_EXTRA:
            if (s->flags & 4)
                while (s->xlen) {
                    if (next_byte(s, strm) < 0) goto done;
                    s->xlen--;
                }
            s->mode = I_GZ_NAME;
            break;
        case I_GZ_NAME:
            if (s->flags & 8)
                for (;;) {
                    int c = next_byte(s, strm);
                    if (c < 0) goto done;
                    if (!c) break;
                }
            s->mode = I_GZ_COMMENT;
            break;
        case I_GZ_COMMENT:
            if (s->flags & 16)
                for (;;) {
                    int c = next_byte(s, strm);
                    if (c < 0) goto done;
                    if (!c) break;
                }
            s->got = 0;
            s->mode = I_GZ_HCRC;
            break;
        case I_GZ_HCRC:
            if (s->flags & 2)
                while (s->got < 2) {
                    if (next_byte(s, strm) < 0) goto done;
                    s->got++;
                }
            s->mode = I_BLOCK;
            break;
        case I_BLOCK:
            if (s->last) {
                s->hold >>= s->bits & 7;
                s->bits -= s->bits & 7;
                s->got = 0;
                s->need = 0;
                s->mode = s->wrap ? I_CHECK : I_DONE;
                break;
            }
            if (!need_bits(s, strm, 3)) goto done;
            s->last = (int)take_bits(s, 1);
            s->btype = (int)take_bits(s, 2);
            if (s->btype == 0) {
                s->hold >>= s->bits & 7;
                s->bits -= s->bits & 7;
                s->mode = I_STORED_LEN;
            } else if (s->btype == 1) {
                fixed_tables(s);
                s->mode = I_CODES;
            } else if (s->btype == 2) {
                s->mode = I_DYN_HDR;
            } else {
                return bad(strm, "invalid block type");
            }
            break;
        case I_STORED_LEN: {
            if (!need_bits(s, strm, 32)) goto done;
            unsigned len = take_bits(s, 16), nlen = take_bits(s, 16);
            if ((len ^ 0xffff) != nlen) return bad(strm, "invalid stored block lengths");
            s->stored_left = len;
            s->mode = I_STORED;
            break;
        }
        case I_STORED:
            while (s->stored_left) {
                if (!strm->avail_out) goto done;
                int c = next_byte(s, strm);
                if (c < 0) goto done;
                put_byte(s, strm, (unsigned char)c);
                s->stored_left--;
            }
            s->mode = I_BLOCK;
            break;
        case I_DYN_HDR:
            if (!need_bits(s, strm, 14)) goto done;
            s->hlit = take_bits(s, 5) + 257;
            s->hdist = take_bits(s, 5) + 1;
            s->hclen = take_bits(s, 4) + 4;
            if (s->hlit > 286 || s->hdist > 30) return bad(strm, "too many length or distance symbols");
            memset(s->lens, 0, sizeof s->lens);
            s->got = 0;
            s->mode = I_DYN_CLEN;
            break;
        case I_DYN_CLEN:
            while (s->got < s->hclen) {
                if (!need_bits(s, strm, 3)) goto done;
                s->lens[CLEN_ORDER[s->got++]] = (uint8_t)take_bits(s, 3);
            }
            if (!huff_build(&s->lit, s->lens, 19)) return bad(strm, "invalid code lengths set");
            memset(s->lens, 0, sizeof s->lens);
            s->nlens = 0;
            s->sym = 0xffff;
            s->mode = I_DYN_LENS;
            break;
        case I_DYN_LENS:
            while (s->nlens < s->hlit + s->hdist) {
                if (s->sym == 0xffff) {
                    need_bits(s, strm, 15);
                    int sym = huff_decode(s, &s->lit);
                    if (sym == -1) goto done;
                    if (sym < 0) return bad(strm, "invalid code lengths set");
                    s->sym = (unsigned)sym;
                }
                if (s->sym < 16) {
                    s->lens[s->nlens++] = (uint8_t)s->sym;
                    s->sym = 0xffff;
                    continue;
                }
                unsigned extra = s->sym == 16 ? 2 : s->sym == 17 ? 3 : 7;
                if (!need_bits(s, strm, extra)) goto done;
                unsigned rep = take_bits(s, extra) + (s->sym == 16 ? 3 : s->sym == 17 ? 3 : 11);
                uint8_t v = 0;
                if (s->sym == 16) {
                    if (!s->nlens) return bad(strm, "invalid bit length repeat");
                    v = s->lens[s->nlens - 1];
                }
                if (s->nlens + rep > s->hlit + s->hdist) return bad(strm, "invalid bit length repeat");
                while (rep--) s->lens[s->nlens++] = v;
                s->sym = 0xffff;
            }
            if (!s->lens[256]) return bad(strm, "invalid code -- missing end-of-block");
            if (!huff_build(&s->lit, s->lens, (int)s->hlit)) return bad(strm, "invalid literal/lengths set");
            if (!huff_build(&s->dist, s->lens + s->hlit, (int)s->hdist)) return bad(strm, "invalid distances set");
            s->mode = I_CODES;
            break;
        case I_CODES: {
            if (!strm->avail_out) goto done;
            need_bits(s, strm, 15);
            int sym = huff_decode(s, &s->lit);
            if (sym == -1) goto done;
            if (sym < 0) return bad(strm, "invalid literal/length code");
            if (sym < 256) {
                put_byte(s, strm, (unsigned char)sym);
                break;
            }
            if (sym == 256) {
                s->mode = I_BLOCK;
                break;
            }
            sym -= 257;
            if (sym >= 29) return bad(strm, "invalid literal/length code");
            s->sym = (unsigned)sym;
            s->mode = I_LENEXT;
            break;
        }
        case I_LENEXT:
            if (!need_bits(s, strm, LEN_EXTRA[s->sym])) goto done;
            s->length = LEN_BASE[s->sym] + take_bits(s, LEN_EXTRA[s->sym]);
            s->mode = I_DIST;
            break;
        case I_DIST: {
            need_bits(s, strm, 15);
            int sym = huff_decode(s, &s->dist);
            if (sym == -1) goto done;
            if (sym < 0 || sym >= 30) return bad(strm, "invalid distance code");
            s->sym = (unsigned)sym;
            s->mode = I_DISTEXT;
            break;
        }
        case I_DISTEXT:
            if (!need_bits(s, strm, DIST_EXTRA[s->sym])) goto done;
            s->distance = DIST_BASE[s->sym] + take_bits(s, DIST_EXTRA[s->sym]);
            if (s->distance > s->wfill) return bad(strm, "invalid distance too far back");
            s->mode = I_COPY;
            break;
        case I_COPY:
            while (s->length) {
                if (!strm->avail_out) goto done;
                unsigned char c = s->window[(s->wpos - s->distance) & (WSIZE - 1)];
                put_byte(s, strm, c);
                s->length--;
            }
            s->mode = I_CODES;
            break;
        case I_CHECK: {
            unsigned want = s->wrap == 2 ? 8 : 4;
            /* the running check covers everything produced so far */
            while (s->got < want) {
                int c = next_byte(s, strm);
                if (c < 0) goto done;
                s->hdr[s->got++] = (unsigned char)c;
            }
            /* account the output of this call before comparing */
            size_t produced = (size_t)(strm->next_out - out_start);
            if (produced) {
                s->check = s->wrap == 2 ? (uint32_t)crc32_z(s->check, out_start, produced) : (uint32_t)adler32(s->check, out_start, (uInt)produced);
                out_start = strm->next_out;
            }
            if (s->wrap == 2) {
                uint32_t crc = (uint32_t)s->hdr[0] | (uint32_t)s->hdr[1] << 8 | (uint32_t)s->hdr[2] << 16 | (uint32_t)s->hdr[3] << 24;
                uint32_t isize = (uint32_t)s->hdr[4] | (uint32_t)s->hdr[5] << 8 | (uint32_t)s->hdr[6] << 16 | (uint32_t)s->hdr[7] << 24;
                if (crc != s->check) return bad(strm, "incorrect data check");
                if (isize != (uint32_t)s->out_total) return bad(strm, "incorrect length check");
            } else {
                uint32_t ad = (uint32_t)s->hdr[0] << 24 | (uint32_t)s->hdr[1] << 16 | (uint32_t)s->hdr[2] << 8 | s->hdr[3];
                if (ad != s->check) return bad(strm, "incorrect data check");
            }
            s->mode = I_DONE;
            break;
        }
        case I_DONE:
            /* whole bytes left in the bit buffer belong to whatever follows the stream: give back the ones this call
             * took (earlier calls' bytes are no longer in the caller's buffer) */
            while (s->bits >= 8 && strm->avail_in < in0) {
                strm->next_in--;
                strm->avail_in++;
                strm->total_in--;
                s->bits -= 8;
            }
            s->hold = 0;
            s->bits = 0;
            ret = Z_STREAM_END;
            goto done;
        default: return Z_STREAM_ERROR;
        }
    }
done:;
    size_t produced = (size_t)(strm->next_out - out_start);
    if (produced && s->wrap) s->check = s->wrap == 2 ? (uint32_t)crc32_z(s->check, out_start, produced) : (uint32_t)adler32(s->check, out_start, (uInt)produced);
    strm->adler = s->check;
    if (ret == Z_STREAM_END) return Z_STREAM_END;
    if (in0 == strm->avail_in && out0 == strm->avail_out) return Z_BUF_ERROR;
    if (flush == Z_FINISH && s->mode != I_DONE) return Z_BUF_ERROR;
    return Z_OK;
}

/* ---------- deflate ---------- */

typedef struct {
    unsigned char *p;
    size_t len, cap;
    uint64_t acc;
    unsigned nacc;
} bw_t;

static void bw_grow(bw_t *w, size_t extra) {
    if (w->len + extra <= w->cap) return;
    size_t nc = w->cap ? w->cap * 2 : 4096;
    while (nc < w->len + extra) nc *= 2;
    w->p = realloc(w->p, nc);
    w->cap = nc;
}

static void bw_put(bw_t *w, unsigned v, unsigned n) {
    w->acc |= (uint64_t)v << w->nacc;
    w->nacc += n;
    if (w->nacc >= 32) {
        bw_grow(w, 4);
        for (int i = 0; i < 4; i++) w->p[w->len++] = (unsigned char)(w->acc >> (8 * i));
        w->acc >>= 32;
        w->nacc -= 32;
    }
}

static void bw_align(bw_t *w) {
    while (w->nacc > 0) {
        bw_grow(w, 1);
        w->p[w->len++] = (unsigned char)w->acc;
        w->acc >>= 8;
        w->nacc = w->nacc >= 8 ? w->nacc - 8 : 0;
    }
    w->acc = 0;
}

static void bw_byte(bw_t *w, unsigned char c) {
    bw_grow(w, 1);
    w->p[w->len++] = c;
}

/* code lengths for freqs, limited to maxlen (frequencies halved until the tree fits) */
static void build_lengths(const uint32_t *freq_in, int n, int maxlen, uint8_t *lens) {
    uint32_t freq[320];
    memcpy(freq, freq_in, (size_t)n * sizeof *freq);
    for (;;) {
        int nodes_sym[640];
        uint64_t w[640];
        int parent[640];
        int nn = 0;
        int heap[640], hn = 0;
        memset(lens, 0, (size_t)n);
        for (int i = 0; i < n; i++)
            if (freq[i]) {
                w[nn] = freq[i];
                nodes_sym[nn] = i;
                parent[nn] = -1;
                heap[hn++] = nn++;
            }
        if (nn == 0) return;
        if (nn == 1) {
            lens[nodes_sym[0]] = 1;
            return;
        }
        /* simple binary heap by weight */
#define HLESS(a, b) (w[a] < w[b] || (w[a] == w[b] && (a) < (b)))
        for (int i = hn / 2 - 1; i >= 0; i--) {
            int k = i;
            for (;;) {
                int l = 2 * k + 1, r = l + 1, m = k;
                if (l < hn && HLESS(heap[l], heap[m])) m = l;
                if (r < hn && HLESS(heap[r], heap[m])) m = r;
                if (m == k) break;
                int t = heap[k];
                heap[k] = heap[m];
                heap[m] = t;
                k = m;
            }
        }
        int leaves = nn;
        while (hn > 1) {
            int pick[2];
            for (int j = 0; j < 2; j++) {
                pick[j] = heap[0];
                heap[0] = heap[--hn];
                int k = 0;
                for (;;) {
                    int l = 2 * k + 1, r = l + 1, m = k;
                    if (l < hn && HLESS(heap[l], heap[m])) m = l;
                    if (r < hn && HLESS(heap[r], heap[m])) m = r;
                    if (m == k) break;
                    int t = heap[k];
                    heap[k] = heap[m];
                    heap[m] = t;
                    k = m;
                }
            }
            w[nn] = w[pick[0]] + w[pick[1]];
            parent[nn] = -1;
            parent[pick[0]] = parent[pick[1]] = nn;
            int k = hn++;
            heap[k] = nn++;
            while (k > 0) {
                int p = (k - 1) / 2;
                if (!HLESS(heap[k], heap[p])) break;
                int t = heap[k];
                heap[k] = heap[p];
                heap[p] = t;
                k = p;
            }
        }
#undef HLESS
        int maxseen = 0;
        for (int i = 0; i < leaves; i++) {
            int d = 0;
            for (int k = i; parent[k] >= 0; k = parent[k]) d++;
            lens[nodes_sym[i]] = (uint8_t)d;
            if (d > maxseen) maxseen = d;
        }
        if (maxseen <= maxlen) return;
        for (int i = 0; i < n; i++)
            if (freq[i]) freq[i] = (freq[i] + 1) / 2;
    }
}

static void codes_from_lengths(const uint8_t *lens, int n, uint16_t *codes) {
    uint16_t count[16] = {0}, next[16];
    for (int i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    unsigned code = 0;
    for (int l = 1; l < 16; l++) {
        code = (code + count[l - 1]) << 1;
        next[l] = (uint16_t)code;
    }
    for (int i = 0; i < n; i++)
        if (lens[i]) codes[i] = (uint16_t)rev_bits(next[lens[i]]++, lens[i]);
}

typedef struct {
    uint16_t litlen; /* < 256 literal, else 256 + length */
    uint16_t dist;   /* 0 for literals */
} lz_t;

static int len_code(unsigned len) {
    for (int i = 28; i >= 0; i--)
        if (len >= LEN_BASE[i]) return i;
    return 0;
}

static int dist_code(unsigned d) {
    for (int i = 29; i >= 0; i--)
        if (d >= DIST_BASE[i]) return i;
    return 0;
}

/* emit one block of symbols with the cheapest encoding */
static void emit_stored(bw_t *w, const unsigned char *raw, size_t rawlen, bool last) {
    size_t off = 0;
    do {
        size_t chunk = rawlen - off > 65535 ? 65535 : rawlen - off;
        bool fin = last && off + chunk == rawlen;
        bw_put(w, fin ? 1 : 0, 1);
        bw_put(w, 0, 2);
        bw_align(w);
        bw_byte(w, (unsigned char)chunk);
        bw_byte(w, (unsigned char)(chunk >> 8));
        bw_byte(w, (unsigned char)~chunk);
        bw_byte(w, (unsigned char)(~chunk >> 8));
        bw_grow(w, chunk);
        memcpy(w->p + w->len, raw + off, chunk);
        w->len += chunk;
        off += chunk;
    } while (off < rawlen);
}

static void emit_block(bw_t *w, const lz_t *sy, size_t ns, const unsigned char *raw, size_t rawlen, bool last) {
    if (!sy) { /* level 0: the bytes as they are */
        emit_stored(w, raw, rawlen, last);
        return;
    }
    uint32_t lf[286] = {0}, df[30] = {0};
    for (size_t i = 0; i < ns; i++) {
        if (sy[i].dist == 0) lf[sy[i].litlen]++;
        else {
            lf[257 + len_code(sy[i].litlen - 256)]++;
            df[dist_code(sy[i].dist)]++;
        }
    }
    lf[256] = 1;
    uint8_t ll[288] = {0}, dl[30]; /* 288: the fixed code counts 286 and 287 too */
    build_lengths(lf, 286, 15, ll);
    build_lengths(df, 30, 15, dl);
    int nl = 286, nd = 30;
    while (nl > 257 && !ll[nl - 1]) nl--;
    while (nd > 1 && !dl[nd - 1]) nd--;
    if (!dl[0] && nd == 1) dl[0] = 1; /* at least one distance code */
    /* code length RLE */
    uint8_t all[316];
    memcpy(all, ll, (size_t)nl);
    memcpy(all + nl, dl, (size_t)nd);
    int total = nl + nd;
    uint8_t rle[316 * 2];
    int nr = 0;
    uint32_t cf[19] = {0};
    for (int i = 0; i < total;) {
        int j = i + 1;
        while (j < total && all[j] == all[i]) j++;
        int run = j - i;
        if (all[i] == 0 && run >= 3) {
            while (run >= 3) {
                int r = run > 138 ? 138 : run;
                if (r >= 11) rle[nr++] = 18, rle[nr++] = (uint8_t)(r - 11), cf[18]++;
                else rle[nr++] = 17, rle[nr++] = (uint8_t)(r - 3), cf[17]++;
                run -= r;
            }
            while (run--) rle[nr++] = 0, rle[nr++] = 0, cf[0]++;
        } else if (run >= 4) {
            rle[nr++] = all[i], rle[nr++] = 0, cf[all[i]]++;
            run--;
            while (run >= 3) {
                int r = run > 6 ? 6 : run;
                rle[nr++] = 16, rle[nr++] = (uint8_t)(r - 3), cf[16]++;
                run -= r;
            }
            while (run--) rle[nr++] = all[i], rle[nr++] = 0, cf[all[i]]++;
        } else {
            while (run--) rle[nr++] = all[i], rle[nr++] = 0, cf[all[i]]++;
        }
        i = j;
    }
    uint8_t cl[19];
    build_lengths(cf, 19, 7, cl);
    int nc = 19;
    while (nc > 4 && !cl[CLEN_ORDER[nc - 1]]) nc--;
    /* costs in bits */
    uint64_t dyn = 3 + 14 + 3 * (uint64_t)nc, fix = 3;
    for (int i = 0; i < nr; i += 2) {
        int c = rle[i];
        dyn += cl[c] + (c == 16 ? 2 : c == 17 ? 3 : c == 18 ? 7 : 0);
    }
    for (int i = 0; i < 286; i++) {
        unsigned fl = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        unsigned extra = i >= 257 ? LEN_EXTRA[i - 257] : 0;
        dyn += (uint64_t)lf[i] * (ll[i] + extra);
        fix += (uint64_t)lf[i] * (fl + extra);
    }
    for (int i = 0; i < 30; i++) {
        dyn += (uint64_t)df[i] * (dl[i] + DIST_EXTRA[i]);
        fix += (uint64_t)df[i] * (5 + DIST_EXTRA[i]);
    }
    uint64_t stored = ((uint64_t)rawlen + 5 * ((rawlen + 65534) / 65535 + (rawlen == 0))) * 8 + 8;
    if (stored <= dyn && stored <= fix) {
        emit_stored(w, raw, rawlen, last);
        return;
    }
    uint16_t lc[288], dc[30];
    if (fix <= dyn) {
        for (int i = 0; i < 288; i++) ll[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        for (int i = 0; i < 30; i++) dl[i] = 5;
        bw_put(w, last ? 1 : 0, 1);
        bw_put(w, 1, 2);
    } else {
        bw_put(w, last ? 1 : 0, 1);
        bw_put(w, 2, 2);
        bw_put(w, (unsigned)(nl - 257), 5);
        bw_put(w, (unsigned)(nd - 1), 5);
        bw_put(w, (unsigned)(nc - 4), 4);
        for (int i = 0; i < nc; i++) bw_put(w, cl[CLEN_ORDER[i]], 3);
        uint16_t cc[19];
        codes_from_lengths(cl, 19, cc);
        for (int i = 0; i < nr; i += 2) {
            int c = rle[i];
            bw_put(w, cc[c], cl[c]);
            if (c == 16) bw_put(w, rle[i + 1], 2);
            else if (c == 17) bw_put(w, rle[i + 1], 3);
            else if (c == 18) bw_put(w, rle[i + 1], 7);
        }
    }
    codes_from_lengths(ll, 288, lc); /* fixed: all 288 lengths shape the canonical codes; dynamic: 286, 287 are 0 */
    codes_from_lengths(dl, 30, dc);
    for (size_t i = 0; i < ns; i++) {
        if (sy[i].dist == 0) {
            bw_put(w, lc[sy[i].litlen], ll[sy[i].litlen]);
        } else {
            unsigned len = sy[i].litlen - 256u;
            int lcd = len_code(len);
            bw_put(w, lc[257 + lcd], ll[257 + lcd]);
            if (LEN_EXTRA[lcd]) bw_put(w, len - LEN_BASE[lcd], LEN_EXTRA[lcd]);
            int dcd = dist_code(sy[i].dist);
            bw_put(w, dc[dcd], dl[dcd]);
            if (DIST_EXTRA[dcd]) bw_put(w, sy[i].dist - DIST_BASE[dcd], DIST_EXTRA[dcd]);
        }
    }
    bw_put(w, lc[256], ll[256]);
}

#define HASH_BITS 15
#define HASH(p) ((((unsigned)(p)[0] << 10) ^ ((unsigned)(p)[1] << 5) ^ (p)[2]) & ((1u << HASH_BITS) - 1))

/* LZ77 + blocks of up to 32K symbols; level 0 stores */
static void deflate_all(bw_t *w, const unsigned char *in, size_t n, int level, bool last) {
    if (level == 0 || n == 0) {
        emit_block(w, NULL, 0, in, n, last);
        return;
    }
    int max_chain = level >= 9 ? 4096 : level >= 6 ? 128 : level >= 4 ? 32 : 8;
    unsigned nice = level >= 9 ? 258 : level >= 6 ? 128 : 32;
    bool lazy = level >= 4;
    int32_t *head = malloc((1u << HASH_BITS) * sizeof *head);
    int32_t *prev = malloc(n * sizeof *prev);
    for (unsigned i = 0; i < (1u << HASH_BITS); i++) head[i] = -1;
    size_t cap = 32768;
    lz_t *sy = malloc(cap * sizeof *sy);
    size_t ns = 0, block_start = 0;
    size_t i = 0;
    while (i < n) {
        unsigned best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            unsigned h = HASH(in + i);
            int chain = max_chain;
            for (int32_t c = head[h]; c >= 0 && chain-- > 0; c = prev[c]) {
                size_t d = i - (size_t)c;
                if (d > 32768) break;
                const unsigned char *a = in + c, *b = in + i;
                size_t lim = n - i < 258 ? n - i : 258;
                if (a[best_len] != b[best_len]) continue;
                size_t l = 0;
                while (l < lim && a[l] == b[l]) l++;
                if (l > best_len) {
                    best_len = (unsigned)l;
                    best_dist = (unsigned)d;
                    if (l >= nice) break;
                }
            }
            prev[i] = head[h];
            head[h] = (int32_t)i;
        }
        if (best_len >= 3 && lazy && i + 1 + 3 <= n && best_len < nice) {
            /* one step of lazy evaluation: a longer match at i+1 wins */
            unsigned h2 = HASH(in + i + 1);
            unsigned l2best = 0;
            int chain = max_chain / 2;
            for (int32_t c = head[h2]; c >= 0 && chain-- > 0; c = prev[c]) {
                size_t d = i + 1 - (size_t)c;
                if (d > 32768 || d == 0) break;
                const unsigned char *a = in + c, *b = in + i + 1;
                size_t lim = n - i - 1 < 258 ? n - i - 1 : 258, l = 0;
                while (l < lim && a[l] == b[l]) l++;
                if (l > l2best) l2best = (unsigned)l;
            }
            if (l2best > best_len + 1) best_len = 0;
        }
        if (ns + 1 >= cap) {
            emit_block(w, sy, ns, in + block_start, i - block_start, false);
            ns = 0;
            block_start = i;
        }
        if (best_len >= 3) {
            sy[ns++] = (lz_t){(uint16_t)(256 + best_len), (uint16_t)best_dist};
            for (size_t k = 1; k < best_len; k++) {
                size_t p = i + k;
                if (p + 3 <= n) {
                    unsigned h = HASH(in + p);
                    prev[p] = head[h];
                    head[h] = (int32_t)p;
                }
            }
            i += best_len;
        } else {
            sy[ns++] = (lz_t){in[i], 0};
            i++;
        }
    }
    emit_block(w, sy, ns, in + block_start, n - block_start, last);
    free(sy);
    free(prev);
    free(head);
}

int deflateInit2_(z_streamp strm, int level, int method, int windowBits, int memLevel, int strategy, const char *version,
                  int stream_size) {
    (void)memLevel;
    (void)strategy;
    (void)version;
    (void)stream_size;
    if (!strm || method != Z_DEFLATED) return Z_STREAM_ERROR;
    if (level == Z_DEFAULT_COMPRESSION) level = 6;
    if (level < 0 || level > 9) return Z_STREAM_ERROR;
    int wrap;
    if (windowBits < 0) wrap = 0;
    else if (windowBits > 15) wrap = 2;
    else wrap = 1;
    struct internal_state *s = calloc(1, sizeof *s);
    if (!s) return Z_MEM_ERROR;
    s->is_deflate = 1;
    s->level = level;
    s->dwrap = wrap;
    strm->state = s;
    strm->total_in = strm->total_out = 0;
    strm->msg = NULL;
    strm->adler = wrap == 2 ? 0 : 1;
    s->dcheck = (uint32_t)strm->adler;
    return Z_OK;
}

int deflateInit_(z_streamp strm, int level, const char *version, int stream_size) {
    return deflateInit2_(strm, level, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY, version, stream_size);
}

uLong deflateBound(z_streamp strm, uLong n) {
    (void)strm;
    return n + (n >> 3) + (n >> 8) + 5 * (n / 65535 + 1) + 64;
}

uLong compressBound(uLong n) { return deflateBound(NULL, n); }

int deflateReset(z_streamp strm) {
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    struct internal_state *s = strm->state;
    int level = s->level, wrap = s->dwrap;
    free(s->in);
    free(s->out);
    memset(s, 0, sizeof *s);
    s->is_deflate = 1;
    s->level = level;
    s->dwrap = wrap;
    strm->total_in = strm->total_out = 0;
    strm->adler = wrap == 2 ? 0 : 1;
    s->dcheck = (uint32_t)strm->adler;
    return Z_OK;
}

int deflateEnd(z_streamp strm) {
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    free(strm->state->in);
    free(strm->state->out);
    free(strm->state);
    strm->state = NULL;
    return Z_OK;
}

/* compress the buffered input into s->out (header first, trailer when finishing) */
static void deflate_compress(struct internal_state *s, bool finish) {
    bw_t w = {0};
    if (s->out_pos < s->out_len) {
        /* still draining: append behind what is left */
        w.p = s->out;
        w.len = s->out_len;
        w.cap = s->out_len;
        memmove(w.p, w.p + s->out_pos, s->out_len - s->out_pos);
        w.len -= s->out_pos;
    } else {
        free(s->out);
    }
    s->out = NULL;
    s->out_pos = 0;
    if (!s->header_done) {
        if (s->dwrap == 1) {
            bw_byte(&w, 0x78);
            bw_byte(&w, s->level >= 7 ? 0xDA : s->level >= 6 ? 0x9C : s->level >= 2 ? 0x5E : 0x01);
        } else if (s->dwrap == 2) {
            static const unsigned char gz[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
            for (int i = 0; i < 10; i++) bw_byte(&w, gz[i]);
            w.p[w.len - 2] = s->level >= 9 ? 2 : s->level == 1 ? 4 : 0;
        }
        s->header_done = 1;
    }
    if (s->in_len || finish) deflate_all(&w, s->in, s->in_len, s->level, finish);
    if (!finish) {
        /* sync flush: an empty stored block brings the output to a byte boundary */
        bw_put(&w, 0, 1);
        bw_put(&w, 0, 2);
        bw_align(&w);
        bw_byte(&w, 0);
        bw_byte(&w, 0);
        bw_byte(&w, 0xff);
        bw_byte(&w, 0xff);
    } else {
        bw_align(&w);
        if (s->dwrap == 1) {
            for (int i = 3; i >= 0; i--) bw_byte(&w, (unsigned char)(s->dcheck >> (8 * i)));
        } else if (s->dwrap == 2) {
            for (int i = 0; i < 4; i++) bw_byte(&w, (unsigned char)(s->dcheck >> (8 * i)));
            for (int i = 0; i < 4; i++) bw_byte(&w, (unsigned char)(s->dtotal >> (8 * i)));
        }
    }
    s->in_len = 0;
    s->out = w.p;
    s->out_len = w.len;
}

int deflate(z_streamp strm, int flush) {
    if (!strm || !strm->state || !strm->state->is_deflate) return Z_STREAM_ERROR;
    struct internal_state *s = strm->state;
    if (s->finished && s->out_pos >= s->out_len) return flush == Z_FINISH ? Z_STREAM_END : Z_BUF_ERROR;
    uInt in0 = strm->avail_in, out0 = strm->avail_out;
    if (strm->avail_in && !s->finished) {
        if (s->in_len + strm->avail_in > s->in_cap) {
            size_t nc = s->in_cap ? s->in_cap : 65536;
            while (nc < s->in_len + strm->avail_in) nc *= 2;
            unsigned char *ni = realloc(s->in, nc);
            if (!ni) return Z_MEM_ERROR;
            s->in = ni;
            s->in_cap = nc;
        }
        memcpy(s->in + s->in_len, strm->next_in, strm->avail_in);
        s->dcheck = s->dwrap == 2 ? (uint32_t)crc32_z(s->dcheck, strm->next_in, strm->avail_in)
                                   : (uint32_t)adler32(s->dcheck, strm->next_in, strm->avail_in);
        s->dtotal += strm->avail_in;
        s->in_len += strm->avail_in;
        strm->total_in += strm->avail_in;
        strm->next_in += strm->avail_in;
        strm->avail_in = 0;
        strm->adler = s->dcheck;
    }
    if (!s->finished && flush == Z_FINISH) {
        deflate_compress(s, true);
        s->finished = 1;
    } else if (!s->finished && (flush == Z_SYNC_FLUSH || flush == Z_FULL_FLUSH || flush == Z_PARTIAL_FLUSH)) {
        deflate_compress(s, false);
    }
    size_t avail = s->out_len - s->out_pos;
    size_t k = avail < strm->avail_out ? avail : strm->avail_out;
    if (k) {
        memcpy(strm->next_out, s->out + s->out_pos, k);
        s->out_pos += k;
        strm->next_out += k;
        strm->avail_out -= (uInt)k;
        strm->total_out += k;
    }
    if (s->finished && s->out_pos >= s->out_len) return Z_STREAM_END;
    if (in0 == strm->avail_in && out0 == strm->avail_out && flush != Z_FINISH) return Z_BUF_ERROR;
    return Z_OK;
}

int compress2(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen, int level) {
    z_stream z = {0};
    int r = deflateInit2_(&z, level, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, (int)sizeof z);
    if (r != Z_OK) return r;
    z.next_in = source;
    z.avail_in = (uInt)sourceLen;
    z.next_out = dest;
    z.avail_out = (uInt)*destLen;
    r = deflate(&z, Z_FINISH);
    *destLen = z.total_out;
    deflateEnd(&z);
    return r == Z_STREAM_END ? Z_OK : Z_BUF_ERROR;
}

int compress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen) {
    return compress2(dest, destLen, source, sourceLen, Z_DEFAULT_COMPRESSION);
}

int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen) {
    z_stream z = {0};
    int r = inflateInit_(&z, ZLIB_VERSION, (int)sizeof z);
    if (r != Z_OK) return r;
    z.next_in = source;
    z.avail_in = (uInt)sourceLen;
    z.next_out = dest;
    z.avail_out = (uInt)*destLen;
    r = inflate(&z, Z_FINISH);
    *destLen = z.total_out;
    inflateEnd(&z);
    if (r == Z_STREAM_END) return Z_OK;
    return r == Z_OK || r == Z_BUF_ERROR ? (z.avail_out ? Z_DATA_ERROR : Z_BUF_ERROR) : r;
}

/* ---------- gzip files ---------- */

struct gzFile_s {
    FILE *f;
    z_stream z;
    int reading, eof;
    unsigned char in[16384];
};

gzFile gzopen(const char *path, const char *mode) {
    if (!mode) return NULL;
    if (strchr(mode, 'r')) {
        struct gzFile_s *g = calloc(1, sizeof *g);
        if (!g) return NULL;
        g->reading = 1;
        g->f = fopen(path, "rb");
        if (!g->f || inflateInit2_(&g->z, 47, ZLIB_VERSION, (int)sizeof g->z) != Z_OK) {
            if (g->f) fclose(g->f);
            free(g);
            return NULL;
        }
        return g;
    }
    if (!strchr(mode, 'w')) return NULL;
    struct gzFile_s *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->f = fopen(path, "wb");
    int level = Z_DEFAULT_COMPRESSION;
    for (const char *m = mode; *m; m++)
        if (*m >= '0' && *m <= '9') level = *m - '0';
    if (!g->f || deflateInit2_(&g->z, level, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, (int)sizeof g->z) != Z_OK) {
        if (g->f) fclose(g->f);
        free(g);
        return NULL;
    }
    return g;
}

int gzwrite(gzFile g, voidpc buf, unsigned len) {
    if (!g) return 0;
    g->z.next_in = buf;
    g->z.avail_in = len;
    unsigned char out[1];
    g->z.next_out = out;
    g->z.avail_out = 0;
    deflate(&g->z, Z_NO_FLUSH);
    return (int)len;
}

int gzread(gzFile g, voidp buf, unsigned len) {
    if (!g || !g->reading) return -1;
    g->z.next_out = buf;
    g->z.avail_out = len;
    while (g->z.avail_out && !g->eof) {
        if (!g->z.avail_in) {
            size_t n = fread(g->in, 1, sizeof g->in, g->f);
            if (!n) break;
            g->z.next_in = g->in;
            g->z.avail_in = (uInt)n;
        }
        int r = inflate(&g->z, Z_NO_FLUSH);
        if (r == Z_STREAM_END) {
            /* a following gzip member continues the file */
            if (g->z.avail_in || !feof(g->f)) inflateReset(&g->z);
            else g->eof = 1;
        } else if (r != Z_OK && r != Z_BUF_ERROR) {
            return -1;
        }
    }
    return (int)(len - g->z.avail_out);
}

int gzclose(gzFile g) {
    if (!g) return Z_STREAM_ERROR;
    if (g->reading) {
        inflateEnd(&g->z);
        int c = fclose(g->f);
        free(g);
        return c == 0 ? Z_OK : Z_ERRNO;
    }
    unsigned char out[16384];
    int r;
    do {
        g->z.next_out = out;
        g->z.avail_out = sizeof out;
        r = deflate(&g->z, Z_FINISH);
        size_t n = sizeof out - g->z.avail_out;
        if (n && fwrite(out, 1, n, g->f) != n) r = Z_ERRNO;
    } while (r == Z_OK);
    deflateEnd(&g->z);
    int c = fclose(g->f);
    free(g);
    return r == Z_STREAM_END && c == 0 ? Z_OK : Z_ERRNO;
}
