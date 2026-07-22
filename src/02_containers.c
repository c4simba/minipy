/* ========================= Dict helpers ========================= */

static Dict *dict_new(void){ Dict *d=(Dict*)xmalloc(sizeof(Dict)); memset(d,0,sizeof(Dict)); return d; }
static int dict_find(Dict *d,const char *key){ for(int i=0;i<d->count;i++) if(strcmp(d->keys[i],key)==0) return i; return -1; }
static void dict_set(Dict *d,const char *key,Value v){
    int i=dict_find(d,key); if(i>=0){ d->vals[i]=v; return; }
    if(d->count==d->cap){ d->cap=d->cap?d->cap*2:16; d->keys=(char**)xrealloc(d->keys,sizeof(char*)*(size_t)d->cap); d->vals=(Value*)xrealloc(d->vals,sizeof(Value)*(size_t)d->cap); }
    d->keys[d->count]=xstrdup2(key); d->vals[d->count]=v; d->count++;
}
static int dict_get(Dict *d,const char *key,Value *out){ if(!d) return 0; int i=dict_find(d,key); if(i<0) return 0; *out=d->vals[i]; return 1; }
static Dict *dict_clone(Dict *src){ Dict *d=dict_new(); if(src) for(int i=0;i<src->count;i++) dict_set(d,src->keys[i],src->vals[i]); return d; }
static void list_push(List *l,Value v){ if(l->count==l->cap){ l->cap=l->cap?l->cap*2:8; l->items=(Value*)xrealloc(l->items,sizeof(Value)*(size_t)l->cap); } l->items[l->count++]=v; }
static int val_equal(Value a,Value b);
static void set_add(List *l,Value v){ for(int i=0;i<l->count;i++) if(val_equal(l->items[i],v)) return; list_push(l,v); }

