#ifndef MPY_CONTAINERS_H
#define MPY_CONTAINERS_H

#include "value.h"

/* ========================= Dict / List helpers ========================= */

Dict *dict_new(void);
int   dict_find(Dict *d, const char *key);
void  dict_set(Dict *d, const char *key, Value v);
int   dict_get(Dict *d, const char *key, Value *out);
Dict *dict_clone(Dict *src);

void  list_push(List *l, Value v);
void  set_add(List *l, Value v); /* uses val_equal (declared in value.h) */

#endif /* MPY_CONTAINERS_H */
