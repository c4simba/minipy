/* ========================= Dict / List helpers ========================= */

#include "containers.h"
#include "qstr.h"

/* Dict keys are interned (qstr) canonical pointers, so lookups compare
   pointers instead of running strcmp, and identical keys share storage. */
Dict *dict_new(void){ Dict *d=MPY_NEW0(Dict); return d; }
int dict_find(Dict *d,const char *key){ const char *k=qstr_intern(key); for(int i=0;i<d->count;i++) if(d->keys[i]==k) return i; return -1; }
void dict_set(Dict *d,const char *key,Value v){
    const char *k=qstr_intern(key);
    for(int i=0;i<d->count;i++) if(d->keys[i]==k){ d->vals[i]=v; return; }
    if(d->count==d->cap){ d->cap=d->cap?d->cap*2:16; d->keys=(char**)xrealloc(d->keys,sizeof(char*)*(size_t)d->cap); d->vals=(Value*)xrealloc(d->vals,sizeof(Value)*(size_t)d->cap); }
    d->keys[d->count]=(char*)k; d->vals[d->count]=v; d->count++;
}
int dict_get(Dict *d,const char *key,Value *out){ if(!d) return 0; int i=dict_find(d,key); if(i<0) return 0; *out=d->vals[i]; return 1; }
/* NOTE: keys are interned and never freed here. */
int dict_del(Dict *d,const char *key){ int i=dict_find(d,key); if(i<0) return 0; for(int j=i;j<d->count-1;j++){ d->keys[j]=d->keys[j+1]; d->vals[j]=d->vals[j+1]; } d->count--; return 1; }
Dict *dict_clone(Dict *src){ Dict *d=dict_new(); if(src) for(int i=0;i<src->count;i++) dict_set(d,src->keys[i],src->vals[i]); return d; }
void list_push(List *l,Value v){ if(l->count==l->cap){ l->cap=l->cap?l->cap*2:8; l->items=(Value*)xrealloc(l->items,sizeof(Value)*(size_t)l->cap); } l->items[l->count++]=v; }
void set_add(List *l,Value v){ for(int i=0;i<l->count;i++) if(val_equal(l->items[i],v)) return; list_push(l,v); }
