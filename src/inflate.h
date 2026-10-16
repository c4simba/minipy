#ifndef PPY_INFLATE_H
#define PPY_INFLATE_H

#include <stddef.h>

/* ========================= Inflate (RFC 1951) =========================
   The text plainpy carries (the standard library, the runtime routines of
   compiled programs) is deflated at build time (tools/pack.c) and inflated
   here when it is first needed. A small decoder: raw deflate streams, all
   three block kinds, no zlib/gzip framing. */

/* in[0..inlen) inflated into out[0..outlen): 0 when it fills out exactly,
   else -1 (a damaged stream, or one of another length). */
int ppy_inflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t outlen);

/* A packed text (tools/pack.c): its deflated bytes and its length. Inflated on
   first use and kept (with a terminating 0); NULL when the data is damaged. */
typedef struct { const unsigned char *data; unsigned clen, len; char *text; } PpyPacked;
const char *ppy_unpack(PpyPacked *p);

#endif /* PPY_INFLATE_H */
