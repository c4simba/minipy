/* bytearray: mutable bytes. Its non-mutating methods are bytes' own, run on a copy (bytes results
   come back as bytearrays, as in CPython). */
#include "interp.h"

Type *T_bytearray;

static ByteArrayObj *BA(Value v){ return (ByteArrayObj*)v.u.o; }
static void reserve(ByteArrayObj *b, int64_t n){
    if(n<=b->cap) return;
    int64_t c=b->cap?b->cap:8; while(c<n) c*=2;
    b->data=(unsigned char*)xrealloc(b->data,(size_t)c); b->cap=c;
}
static void put(ByteArrayObj *b, const void *s, int64_t n){ reserve(b,b->len+n); if(n) memcpy(b->data+b->len,s,(size_t)n); b->len+=n; }
static Value ba_of(Type *t, const void *s, int64_t n){
    ByteArrayObj *b=(ByteArrayObj*)mp_alloc(t,sizeof(ByteArrayObj)); b->data=NULL; b->len=b->cap=0; put(b,s,n); return v_obj(b);
}
Value mp_bytearray(const void *s, int64_t n){ return ba_of(T_bytearray,s,n); }

/* bytes, bytearray or a buffer: its bytes */
int mp_byteslike(Value v, const unsigned char **s, int64_t *n){
    if(v.k!=V_OBJ) return 0;
    switch(v.u.o->type->layout){
        case LY_BYTES: *s=AS_BYTES(v)->s; *n=AS_BYTES(v)->len; return 1;
        case LY_BYTEARRAY: *s=BA(v)->data; *n=BA(v)->len; return 1;
        case LY_BUFFER: *s=((BufferObj*)v.u.o)->data; *n=((BufferObj*)v.u.o)->len; return 1;
        default: return 0;
    }
}
static unsigned char byte_of(Value v){
    int64_t x=mp_index(v,"bytearray");
    if(x<0 || x>255) mp_raise_t(E_ValueError,"byte must be in range(0, 256)");
    return (unsigned char)x;
}
/* the bytes an assignment / extension gives: a bytes-like value or an iterable of ints (a malloc'd copy) */
static unsigned char *bytes_of(Value v, int64_t *n, const char *bad){
    const unsigned char *s;
    if(mp_byteslike(v,&s,n)){ unsigned char *r=(unsigned char*)xmalloc((size_t)*n+1); if(*n) memcpy(r,s,(size_t)*n); return r; }
    if(IS_STR(v) || IS_INTLIKE(v) || v.k==V_FLOAT || IS_NONE(v)){
        if(bad) mp_raise_t(E_TypeError,"%s",bad);
        if(IS_STR(v)) mp_raise_t(E_TypeError,"expected iterable of integers; got: 'str'");
        mp_raise_t(E_TypeError,"can't extend bytearray with %s",mp_type_name(v)); }
    Value l=mp_list_of(v); ListObj *x=AS_LIST(l);
    unsigned char *r=(unsigned char*)xmalloc((size_t)x->len+1);
    for(int64_t i=0;i<x->len;i++) r[i]=byte_of(x->items[i]);
    *n=x->len; return r;
}

/* bytearray(), bytearray(n), bytearray(iterable / bytes-like), bytearray(str, encoding[, errors]) */
static Value make_bytearray(Type *t, int argc, Value *argv, TupleObj *kw){
    int np=argc-(kw?(int)kw->len:0);
    Value src=v_undef(), enc=v_undef(), errs=v_undef();
    if(np>3) mp_raise_t(E_TypeError,"bytearray() takes at most 3 arguments (%d given)",np);
    if(np>0) src=argv[0];
    if(np>1) enc=argv[1];
    if(np>2) errs=argv[2];
    for(int64_t i=0;kw && i<kw->len;i++){ const char *k=mp_cstr(kw->items[i]); Value v=argv[np+i];
        if(!strcmp(k,"source")) src=v; else if(!strcmp(k,"encoding")) enc=v; else if(!strcmp(k,"errors")) errs=v;
        else mp_raise_t(E_TypeError,"bytearray() got an unexpected keyword argument '%s'",k); }
    if(src.k==V_UNDEF){
        if(enc.k!=V_UNDEF) mp_raise_t(E_TypeError,"encoding without a string argument");
        if(errs.k!=V_UNDEF) mp_raise_t(E_TypeError,"errors without a string argument");
        return ba_of(t,"",0); }
    if(IS_STR(src)){
        if(enc.k==V_UNDEF) mp_raise_t(E_TypeError,"string argument without an encoding");
        Value a[2]={enc,errs}; Value b=mp_callmethod(src,"encode",errs.k==V_UNDEF?1:2,a);
        return ba_of(t,AS_BYTES(b)->s,AS_BYTES(b)->len); }
    if(enc.k!=V_UNDEF) mp_raise_t(E_TypeError,"encoding without a string argument");
    if(errs.k!=V_UNDEF) mp_raise_t(E_TypeError,"errors without a string argument");
    if(IS_INTLIKE(src)){
        if(src.u.i<0) mp_raise_t(E_ValueError,"negative count");
        Value r=ba_of(t,"",0); reserve(BA(r),src.u.i); memset(BA(r)->data,0,(size_t)src.u.i); BA(r)->len=src.u.i; return r; }
    const unsigned char *s; int64_t n;
    if(mp_byteslike(src,&s,&n)) return ba_of(t,s,n);
    if(src.k!=V_OBJ || IS_NONE(src)) mp_raise_t(E_TypeError,"cannot convert '%s' object to bytearray",mp_type_name(src));
    Type *st=src.u.o->type;
    if(st->layout!=LY_LIST && st->layout!=LY_TUPLE && st->layout!=LY_RANGE && st->layout!=LY_ITER && st->layout!=LY_GEN && st->layout!=LY_SET && st->layout!=LY_DICT
       && mp_type_lookup_s(st,"__iter__").k==V_UNDEF && mp_type_lookup_s(st,"__getitem__").k==V_UNDEF)
        mp_raise_t(E_TypeError,"cannot convert '%s' object to bytearray",mp_type_name(src));
    unsigned char *d=bytes_of(src,&n,NULL); Value r=ba_of(t,d,n); free(d); return r;
}

/* ---------------------------------------------------------------- items (mp_getitem ... come here) */
static int64_t ba_index(ByteArrayObj *b, Value key, const char *msg){
    if(!IS_INTLIKE(key) && !(key.k==V_OBJ && (key.u.o->type->flags&TF_DUNDERS) && mp_type_lookup_s(key.u.o->type,"__index__").k!=V_UNDEF))
        mp_raise_t(E_TypeError,"bytearray indices must be integers or slices, not %s",mp_type_name(key));
    int64_t i=mp_index(key,"bytearray"); if(i<0) i+=b->len;
    if(i<0 || i>=b->len) mp_raise_t(E_IndexError,"%s",msg);
    return i;
}
Value mp_ba_getitem(Value o, Value key){
    ByteArrayObj *b=BA(o);
    if(IS(key,T_slice)){
        int64_t a,e,st, n=mp_slice_indices(key,b->len,&a,&e,&st);
        Value r=mp_bytearray("",0); reserve(BA(r),n);
        for(int64_t i=0,j=a;i<n;i++,j+=st) BA(r)->data[i]=b->data[j];
        BA(r)->len=n; return r; }
    return v_int(b->data[ba_index(b,key,"bytearray index out of range")]);
}
static void del_slice(ByteArrayObj *b, Value key){
    int64_t a,e,s, n=mp_slice_indices(key,b->len,&a,&e,&s);
    if(!n) return;
    if(s<0){ a=a+(n-1)*s; s=-s; }
    if(s==1){ memmove(b->data+a,b->data+a+n,(size_t)(b->len-a-n)); b->len-=n; return; }
    int64_t w=a;
    for(int64_t i=a;i<b->len;i++){ if(i>=a && (i-a)%s==0 && (i-a)/s<n) continue; b->data[w++]=b->data[i]; }
    b->len=w;
}
void mp_ba_setitem(Value o, Value key, Value val){
    ByteArrayObj *b=BA(o);
    if(IS(key,T_slice)){
        int64_t m; unsigned char *d=bytes_of(val,&m,"can assign only bytes, buffers, or iterables of ints in range(0, 256)");
        int64_t a,e,s, n=mp_slice_indices(key,b->len,&a,&e,&s);
        SliceObj *sl=(SliceObj*)key.u.o;
        if(s==1 && (IS_NONE(sl->step) || (IS_INTLIKE(sl->step) && sl->step.u.i==1))){
            if(e<a) e=a;
            int64_t old=e-a;
            reserve(b,b->len-old+m);
            memmove(b->data+a+m,b->data+e,(size_t)(b->len-e));
            if(m) memcpy(b->data+a,d,(size_t)m);
            b->len+=m-old; free(d); return; }
        if(m!=n){ free(d); mp_raise_t(E_ValueError,"attempt to assign bytes of size %lld to extended slice of size %lld",(long long)m,(long long)n); }
        for(int64_t i=0,j=a;i<n;i++,j+=s) b->data[j]=d[i];
        free(d); return; }
    int64_t i=ba_index(b,key,"bytearray index out of range");
    b->data[i]=byte_of(val);
}
void mp_ba_delitem(Value o, Value key){
    ByteArrayObj *b=BA(o);
    if(IS(key,T_slice)){ del_slice(b,key); return; }
    int64_t i=ba_index(b,key,"bytearray index out of range");
    memmove(b->data+i,b->data+i+1,(size_t)(b->len-i-1)); b->len--;
}
int mp_ba_contains(Value c, Value x){
    ByteArrayObj *h=BA(c);
    if(IS_INTLIKE(x)){ unsigned char v=byte_of(x); for(int64_t i=0;i<h->len;i++) if(h->data[i]==v) return 1; return 0; }
    const unsigned char *s; int64_t n;
    if(!mp_byteslike(x,&s,&n)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(x));
    if(!n) return 1;
    for(int64_t i=0;i+n<=h->len;i++) if(!memcmp(h->data+i,s,(size_t)n)) return 1;
    return 0;
}
/* bytearray + x (a new one), x * n, += and *= (in place) */
Value mp_ba_concat(Value a, Value b){
    const unsigned char *s; int64_t n;
    if(!mp_byteslike(b,&s,&n)) mp_raise_t(E_TypeError,"can't concat %s to bytearray",mp_type_name(b));
    Value r=mp_bytearray(BA(a)->data,BA(a)->len); put(BA(r),s,n); return r;
}
Value mp_ba_repeat(Value a, int64_t n){
    Value r=mp_bytearray("",0);
    for(int64_t i=0;i<n;i++) put(BA(r),BA(a)->data,BA(a)->len);
    return r;
}
void mp_ba_extend_bytes(Value a, Value b){
    const unsigned char *s; int64_t n;
    if(!mp_byteslike(b,&s,&n)) mp_raise_t(E_TypeError,"can't concat %s to bytearray",mp_type_name(b));
    if(b.u.o==a.u.o){                                             /* (CPython: its own buffer is exported) */
        Value be; if(mp_dict_get(mp_builtins,mp_intern("BufferError"),&be)) mp_raise(mp_call1(be,mp_str("Existing exports of data: object cannot be re-sized")));
        mp_raise_t(E_TypeError,"Existing exports of data: object cannot be re-sized"); }
    put(BA(a),s,n);
}
void mp_ba_repeat_inplace(Value a, int64_t n){
    ByteArrayObj *b=BA(a); int64_t n0=b->len;
    if(n<=0){ b->len=0; return; }
    reserve(b,n0*n);
    for(int64_t i=1;i<n;i++) memcpy(b->data+i*n0,b->data,(size_t)n0);
    b->len=n0*n;
}

/* ---------------------------------------------------------------- methods */
#define SELF argv[0]
static void nargs(const char *name, int argc, int lo, int hi){
    int n=argc-1;
    if(n<lo || n>hi){
        if(lo==hi && lo==0) mp_raise_t(E_TypeError,"%s() takes no arguments (%d given)",name,n);
        if(lo==hi) mp_raise_t(E_TypeError,"%s() takes exactly one argument (%d given)",name,n);
        if(n<lo) mp_raise_t(E_TypeError,"%s expected at least %d argument%s, got %d",name,lo,lo==1?"":"s",n);
        mp_raise_t(E_TypeError,"%s expected at most %d argument%s, got %d",name,hi,hi==1?"":"s",n);
    }
}
static void no_kw(const char *fn, TupleObj *kw){ if(kw && kw->len) mp_raise_t(E_TypeError,"%s() takes no keyword arguments",fn); }
static Value m_append(int argc, Value *argv, TupleObj *kw){ no_kw("append",kw); nargs("append",argc,1,1); unsigned char c=byte_of(argv[1]); put(BA(SELF),&c,1); return v_none(); }
static Value m_extend(int argc, Value *argv, TupleObj *kw){ no_kw("extend",kw); nargs("extend",argc,1,1);
    int64_t n; unsigned char *d=bytes_of(argv[1],&n,NULL); put(BA(SELF),d,n); free(d); return v_none(); }
static Value m_insert(int argc, Value *argv, TupleObj *kw){ no_kw("insert",kw); nargs("insert",argc,2,2);
    ByteArrayObj *b=BA(SELF); int64_t i=mp_index(argv[1],"insert"); unsigned char c=byte_of(argv[2]);
    if(i<0){ i+=b->len; if(i<0) i=0; } if(i>b->len) i=b->len;
    reserve(b,b->len+1); memmove(b->data+i+1,b->data+i,(size_t)(b->len-i)); b->data[i]=c; b->len++; return v_none(); }
static Value m_pop(int argc, Value *argv, TupleObj *kw){ no_kw("pop",kw); nargs("pop",argc,0,1);
    ByteArrayObj *b=BA(SELF);
    if(!b->len) mp_raise_t(E_IndexError,"pop from empty bytearray");
    int64_t i= argc>1 ? mp_index(argv[1],"pop") : -1; if(i<0) i+=b->len;
    if(i<0 || i>=b->len) mp_raise_t(E_IndexError,"pop index out of range");
    unsigned char c=b->data[i]; memmove(b->data+i,b->data+i+1,(size_t)(b->len-i-1)); b->len--; return v_int(c); }
static Value m_remove(int argc, Value *argv, TupleObj *kw){ no_kw("remove",kw); nargs("remove",argc,1,1);
    ByteArrayObj *b=BA(SELF); unsigned char c=byte_of(argv[1]);
    for(int64_t i=0;i<b->len;i++) if(b->data[i]==c){ memmove(b->data+i,b->data+i+1,(size_t)(b->len-i-1)); b->len--; return v_none(); }
    mp_raise_t(E_ValueError,"value not found in bytearray"); }
static Value m_clear(int argc, Value *argv, TupleObj *kw){ no_kw("clear",kw); nargs("clear",argc,0,0); BA(SELF)->len=0; return v_none(); }
static Value m_reverse(int argc, Value *argv, TupleObj *kw){ no_kw("reverse",kw); nargs("reverse",argc,0,0);
    ByteArrayObj *b=BA(SELF); for(int64_t i=0,j=b->len-1;i<j;i++,j--){ unsigned char t=b->data[i]; b->data[i]=b->data[j]; b->data[j]=t; } return v_none(); }
static Value m_copy(int argc, Value *argv, TupleObj *kw){ no_kw("copy",kw); nargs("copy",argc,0,0); return mp_bytearray(BA(SELF)->data,BA(SELF)->len); }
static Value m_iadd(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__iadd__",argc,1,1); mp_ba_extend_bytes(SELF,argv[1]); return SELF; }
static Value m_imul(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__imul__",argc,1,1); mp_ba_repeat_inplace(SELF,mp_index(argv[1],"__imul__")); return SELF; }
static Value m_add(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__add__",argc,1,1); return mp_ba_concat(SELF,argv[1]); }
static Value m_mul(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__mul__",argc,1,1); return mp_ba_repeat(SELF,mp_index(argv[1],"__mul__")); }
static Value m_resize(int argc, Value *argv, TupleObj *kw){ no_kw("resize",kw); nargs("resize",argc,1,1);
    ByteArrayObj *b=BA(SELF); int64_t n=mp_index(argv[1],"resize");
    if(n<0) mp_raise_t(E_ValueError,"Can only resize to positive sizes, got %lld",(long long)n);
    reserve(b,n); if(n>b->len) memset(b->data+b->len,0,(size_t)(n-b->len)); b->len=n; return v_none(); }
static Value m_alloc(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("__alloc__",argc,0,0); return v_int(BA(SELF)->cap+1); }

/* the bytes methods: on a bytes copy, bytes results made bytearrays */
static Value as_ba(Value r){
    if(IS(r,T_bytes)) return mp_bytearray(AS_BYTES(r)->s,AS_BYTES(r)->len);
    if(IS(r,T_list)){ ListObj *l=AS_LIST(r); for(int64_t i=0;i<l->len;i++) l->items[i]=as_ba(l->items[i]); return r; }
    if(IS(r,T_tuple)){ TupleObj *t=AS_TUPLE(r); for(int64_t i=0;i<t->len;i++) t->items[i]=as_ba(t->items[i]); return r; }
    return r;
}
static Value delegate(const char *name, int argc, Value *argv, TupleObj *kw, int convert){
    Value m;
    if(!mp_dict_get(T_bytes->dict,mp_intern(name),&m) || !IS(m,T_native)) mp_raise_t(E_AttributeError,"'bytearray' object has no attribute '%s'",name);
    Value *a=(Value*)xmalloc(sizeof(Value)*(size_t)(argc>0?argc:1));
    for(int i=0;i<argc;i++) a[i]=argv[i];
    a[0]=mp_bytes(BA(SELF)->data,BA(SELF)->len);
    Catch c; Value r=v_none();
    if(!CATCH_BEGIN(c)){ r=((NativeObj*)m.u.o)->fn(argc,a,kw); CATCH_END(c); }
    else { free(a); mp_raise(mp_catch_exc(&c)); }
    free(a);
    return convert ? as_ba(r) : r;
}
#define DEL(fn,name,conv) static Value fn(int argc, Value *argv, TupleObj *kw){ return delegate(name,argc,argv,kw,conv); }
DEL(d_decode,"decode",0) DEL(d_hex,"hex",0) DEL(d_find,"find",0) DEL(d_rfind,"rfind",0) DEL(d_index,"index",0) DEL(d_rindex,"rindex",0)
DEL(d_count,"count",0) DEL(d_startswith,"startswith",0) DEL(d_endswith,"endswith",0)
DEL(d_split,"split",1) DEL(d_rsplit,"rsplit",1) DEL(d_strip,"strip",1) DEL(d_lstrip,"lstrip",1) DEL(d_rstrip,"rstrip",1)
DEL(d_join,"join",1) DEL(d_replace,"replace",1) DEL(d_upper,"upper",1) DEL(d_lower,"lower",1) DEL(d_swapcase,"swapcase",1)
DEL(d_title,"title",1) DEL(d_capitalize,"capitalize",1) DEL(d_isalpha,"isalpha",0) DEL(d_isdigit,"isdigit",0) DEL(d_isalnum,"isalnum",0)
DEL(d_isspace,"isspace",0) DEL(d_islower,"islower",0) DEL(d_isupper,"isupper",0) DEL(d_istitle,"istitle",0) DEL(d_isascii,"isascii",0)
DEL(d_partition,"partition",1) DEL(d_rpartition,"rpartition",1) DEL(d_removeprefix,"removeprefix",1) DEL(d_removesuffix,"removesuffix",1)
DEL(d_splitlines,"splitlines",1) DEL(d_center,"center",1) DEL(d_ljust,"ljust",1) DEL(d_rjust,"rjust",1) DEL(d_zfill,"zfill",1)
DEL(d_expandtabs,"expandtabs",1) DEL(d_translate,"translate",1)
static Value m_fromhex(int argc, Value *argv, TupleObj *kw){
    Value m; if(!mp_dict_get(T_bytes->dict,mp_intern("fromhex"),&m)) mp_raise_t(E_AttributeError,"fromhex");
    Value f=((BoxObj*)m.u.o)->v;                                  /* (bytes.fromhex: a classmethod) */
    Value r=((NativeObj*)f.u.o)->fn(argc,argv,kw);
    Type *t= IS_TYPE(argv[0]) ? (Type*)argv[0].u.o : T_bytearray;
    if(t==T_bytearray) return mp_bytearray(AS_BYTES(r)->s,AS_BYTES(r)->len);
    return mp_call1(argv[0],mp_bytearray(AS_BYTES(r)->s,AS_BYTES(r)->len));
}

void mp_bytearray_init(void){
    Type *t=AS_TYPE(mp_new_type("bytearray",T_object,LY_BYTEARRAY,TF_BASETYPE)); t->make=make_bytearray; T_bytearray=t;
    mp_dict_set_s(mp_builtins,"bytearray",v_obj(t));
    mp_type_add(t,"append",m_append); mp_type_add(t,"extend",m_extend); mp_type_add(t,"insert",m_insert); mp_type_add(t,"pop",m_pop);
    mp_type_add(t,"remove",m_remove); mp_type_add(t,"clear",m_clear); mp_type_add(t,"reverse",m_reverse); mp_type_add(t,"copy",m_copy);
    mp_type_add(t,"__iadd__",m_iadd); mp_type_add(t,"__imul__",m_imul); mp_type_add(t,"__add__",m_add); mp_type_add(t,"__mul__",m_mul);
    mp_type_add(t,"__rmul__",m_mul); mp_type_add(t,"__alloc__",m_alloc);
    mp_type_add(t,"decode",d_decode); mp_type_add(t,"hex",d_hex); mp_type_add(t,"find",d_find); mp_type_add(t,"rfind",d_rfind);
    mp_type_add(t,"index",d_index); mp_type_add(t,"rindex",d_rindex); mp_type_add(t,"count",d_count); mp_type_add(t,"startswith",d_startswith);
    mp_type_add(t,"endswith",d_endswith); mp_type_add(t,"split",d_split); mp_type_add(t,"rsplit",d_rsplit); mp_type_add(t,"strip",d_strip);
    mp_type_add(t,"lstrip",d_lstrip); mp_type_add(t,"rstrip",d_rstrip); mp_type_add(t,"join",d_join); mp_type_add(t,"replace",d_replace);
    mp_type_add(t,"upper",d_upper); mp_type_add(t,"lower",d_lower); mp_type_add(t,"swapcase",d_swapcase); mp_type_add(t,"title",d_title);
    mp_type_add(t,"capitalize",d_capitalize); mp_type_add(t,"isalpha",d_isalpha); mp_type_add(t,"isdigit",d_isdigit); mp_type_add(t,"isalnum",d_isalnum);
    mp_type_add(t,"isspace",d_isspace); mp_type_add(t,"islower",d_islower); mp_type_add(t,"isupper",d_isupper); mp_type_add(t,"istitle",d_istitle);
    mp_type_add(t,"isascii",d_isascii); mp_type_add(t,"partition",d_partition); mp_type_add(t,"rpartition",d_rpartition);
    mp_type_add(t,"removeprefix",d_removeprefix); mp_type_add(t,"removesuffix",d_removesuffix); mp_type_add(t,"splitlines",d_splitlines);
    mp_type_add(t,"center",d_center); mp_type_add(t,"ljust",d_ljust); mp_type_add(t,"rjust",d_rjust); mp_type_add(t,"zfill",d_zfill);
    mp_type_add(t,"expandtabs",d_expandtabs); mp_type_add(t,"translate",d_translate); mp_type_add(t,"resize",m_resize);
    { Value fh=mp_native("fromhex",m_fromhex); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=fh; mp_dict_set(t->dict,mp_intern("fromhex"),v_obj(cm)); }
    mp_dict_set_s(t->dict,"__hash__",v_none());
}
