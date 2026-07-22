#ifndef MPY_QSTR_H
#define MPY_QSTR_H

#include "util.h"

/* ========================= Interned strings (qstr) =========================
   Every distinct string passed through qstr_intern is stored once; equal
   strings return the SAME canonical pointer. This lets dictionaries compare
   keys by pointer instead of strcmp and de-duplicates identifier storage
   (the core idea behind MicroPython's QSTRs).

   Interned strings are immutable and live for the whole program. */

const char *qstr_intern(const char *s);
const char *qstr_intern_n(const char *s, int n);

#endif /* MPY_QSTR_H */
