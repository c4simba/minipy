/* ========================= Interpreter: methods of the built-in types =========================
   str (code points over UTF-8; upper / lower / isalpha ... know Latin-1, Latin
   Extended-A, Greek and Cyrillic), bytes, list, tuple, dict, set, int, float,
   files, generators and coroutines, object, BaseException, property.

   This header is internal to src/interp/methods: what its files share,
   grouped by the file defining it. */
#ifndef MPY_INTERP_METHODS_H
#define MPY_INTERP_METHODS_H

#include "interp/interp.h"
#include <math.h>

Value mp_set_copy(SetObj *s, Type *t);
void  mp_set_merge(SetObj *s, SetObj *o);
int   mp_set_next(SetObj *s, int64_t *pos, Value *key);
Value mp_set_pop(SetObj *s);
void  mp_str_repr_into(SBuf *b, StrObj *s);
int   mp_parse_int(const char *s, int64_t n, int base, int64_t *out);
Value mp_iter_kind(int kind, Value src, Value aux, Value aux2);

#define SELF argv[0]

Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);

void mp_set_clear(SetObj *s);

void mp_set_update(SetObj *s, Value it);
void mp_set_diff_update(SetObj *s, Value other);
void mp_set_xor_update(SetObj *s, Value other);

int  mp_set_has_hashed(SetObj *s, Value v, uint64_t h);
void mp_set_add_hashed(SetObj *s, Value v, uint64_t h);

int mp_file_flush(FileObj *f);

/* ---- register.c: registration */
int npos_of(int argc, TupleObj *kw);
Value kwarg(int argc, Value *argv, TupleObj *kw, const char *name, Value dflt);
void nargs(const char *name, int argc, int lo, int hi);
StrObj *S(Value v);
StrObj *str_arg(Value v, const char *what);

/* ---- str.c: str */
void put_utf8(SBuf *b, uint32_t c);
int64_t cp_index(StrObj *s, int64_t off);
int64_t find_bytes(const char *h, int64_t hs, int64_t he, const char *n, int64_t nl, int rev);
Value py_codec(const char *fn, Value obj, Value enc, Value errs);
void mp_str_methods_init(void);

/* ---- codecs.c: codecs */
int codec_known(Value enc);

/* ---- bytes.c: bytes */
void bytes_arg(Value v, int byte_ok, const char **s, int64_t *n, char *one);
void mp_bytes_methods_init(void);

/* ---- containers.c: list, dict, set */
void mp_container_methods_init(void);

/* ---- numbers.c: int, float */
void mp_number_methods_init(void);

/* ---- object.c: objects */
void mp_object_methods_init(void);

#endif /* MPY_INTERP_METHODS_H */
