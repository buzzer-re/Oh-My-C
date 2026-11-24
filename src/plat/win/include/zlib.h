/* zlib.h for Windows: the part of zlib's API omc uses, implemented in src/plat/win/zlib.c (written for omc, no zlib
 * code). Same names, constants and stream semantics: windowBits 8..15 zlib, -8..-15 raw deflate, +16 gzip,
 * +32 zlib-or-gzip auto-detect (inflate only). */
#ifndef OMC_WIN_ZLIB_H
#define OMC_WIN_ZLIB_H
#include <stddef.h>

#define ZLIB_VERSION "1.3.1-omc"
#define MAX_WBITS 15
#define MAX_MEM_LEVEL 9

typedef unsigned char Byte;
typedef Byte Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;
typedef uLong uLongf;
typedef void *voidpf;
typedef void *voidp;
typedef const void *voidpc;
typedef char charf;
typedef int intf;
typedef voidpf (*alloc_func)(voidpf opaque, uInt items, uInt size);
typedef void (*free_func)(voidpf opaque, voidpf address);
struct internal_state;

typedef struct z_stream_s {
    const Bytef *next_in;
    uInt avail_in;
    uLong total_in;
    Bytef *next_out;
    uInt avail_out;
    uLong total_out;
    const char *msg;
    struct internal_state *state;
    alloc_func zalloc;
    free_func zfree;
    voidpf opaque;
    int data_type;
    uLong adler;
    uLong reserved;
} z_stream;
typedef z_stream *z_streamp;

#define Z_NO_FLUSH 0
#define Z_PARTIAL_FLUSH 1
#define Z_SYNC_FLUSH 2
#define Z_FULL_FLUSH 3
#define Z_FINISH 4
#define Z_BLOCK 5
#define Z_TREES 6

#define Z_OK 0
#define Z_STREAM_END 1
#define Z_NEED_DICT 2
#define Z_ERRNO (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR (-3)
#define Z_MEM_ERROR (-4)
#define Z_BUF_ERROR (-5)
#define Z_VERSION_ERROR (-6)

#define Z_NO_COMPRESSION 0
#define Z_BEST_SPEED 1
#define Z_BEST_COMPRESSION 9
#define Z_DEFAULT_COMPRESSION (-1)
#define Z_FILTERED 1
#define Z_HUFFMAN_ONLY 2
#define Z_RLE 3
#define Z_FIXED 4
#define Z_DEFAULT_STRATEGY 0
#define Z_BINARY 0
#define Z_TEXT 1
#define Z_UNKNOWN 2
#define Z_DEFLATED 8
#define Z_NULL 0

const char *zlibVersion(void);

int inflateInit_(z_streamp strm, const char *version, int stream_size);
int inflateInit2_(z_streamp strm, int windowBits, const char *version, int stream_size);
#define inflateInit(strm) inflateInit_((strm), ZLIB_VERSION, (int)sizeof(z_stream))
#define inflateInit2(strm, wb) inflateInit2_((strm), (wb), ZLIB_VERSION, (int)sizeof(z_stream))
int inflate(z_streamp strm, int flush);
int inflateEnd(z_streamp strm);
int inflateReset(z_streamp strm);
int inflateReset2(z_streamp strm, int windowBits);

int deflateInit_(z_streamp strm, int level, const char *version, int stream_size);
int deflateInit2_(z_streamp strm, int level, int method, int windowBits, int memLevel, int strategy, const char *version,
                  int stream_size);
#define deflateInit(strm, level) deflateInit_((strm), (level), ZLIB_VERSION, (int)sizeof(z_stream))
#define deflateInit2(strm, level, method, wb, ml, st)                                                                    \
    deflateInit2_((strm), (level), (method), (wb), (ml), (st), ZLIB_VERSION, (int)sizeof(z_stream))
int deflate(z_streamp strm, int flush);
int deflateEnd(z_streamp strm);
int deflateReset(z_streamp strm);
uLong deflateBound(z_streamp strm, uLong sourceLen);

int compress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen);
int compress2(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen, int level);
uLong compressBound(uLong sourceLen);
int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source, uLong sourceLen);

uLong crc32(uLong crc, const Bytef *buf, uInt len);
uLong crc32_z(uLong crc, const Bytef *buf, size_t len);
uLong adler32(uLong adler, const Bytef *buf, uInt len);

typedef struct gzFile_s *gzFile;
gzFile gzopen(const char *path, const char *mode);
int gzwrite(gzFile file, voidpc buf, unsigned len);
int gzread(gzFile file, voidp buf, unsigned len);
int gzclose(gzFile file);

#endif
