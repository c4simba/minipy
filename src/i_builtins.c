/* ========================= Interpreter: built-in types and functions =========================
   The type objects (built at start: object, type, int, str ... and the
   exception hierarchy), their constructors, the builtins module (len, print,
   sorted, isinstance, open ...), stdout / stderr, files, and the iterators
   of enumerate / zip / map / filter / reversed / iter(f, sentinel). */

#include "interp.h"
#include "fs.h"
#include <math.h>
#if !defined(MPY_KOLIBRI)
#include <unistd.h>
#endif

Type *E_BaseException, *E_Exception, *E_TypeError, *E_ValueError, *E_KeyError, *E_IndexError, *E_AttributeError,
    *E_NameError, *E_UnboundLocalError, *E_ZeroDivisionError, *E_OverflowError, *E_StopIteration, *E_StopAsyncIteration,
    *E_RuntimeError, *E_RecursionError, *E_NotImplementedError, *E_AssertionError, *E_ImportError, *E_ModuleNotFoundError,
    *E_OSError, *E_FileNotFoundError, *E_LookupError, *E_ArithmeticError, *E_GeneratorExit, *E_SystemExit,
    *E_KeyboardInterrupt, *E_EOFError, *E_UnicodeError, *E_UnicodeDecodeError, *E_UnicodeEncodeError, *E_SyntaxError, *E_MemoryError,
    *E_SystemError, *E_BaseExceptionGroup, *E_ExceptionGroup;

void  mp_methods_init(void);
void  mp_type_finish(Type *t);
int   mp_parse_int(const char *s, int64_t n, int base, int64_t *out);
int   mp_parse_float(const char *s, int64_t n, double *out);
Value mp_iter_kind(int kind, Value src, Value aux, Value aux2);
Value mp_set_copy(SetObj *s, Type *t);
void  mp_dict_update_from(DictObj *d, Value src);
int   mp_native_send(Value it, Value v, Value *out);
Value mp_instance_call(Type *t, int argc, Value *argv, TupleObj *kw);
uint32_t mp_cp_upper(uint32_t c);

static int npos(int argc, TupleObj *kw){ return argc-(kw?(int)kw->len:0); }
static Value kwarg(int argc, Value *argv, TupleObj *kw, const char *name, Value dflt){
    if(!kw) return dflt;
    int np=npos(argc,kw);
    for(int64_t i=0;i<kw->len;i++) if(!strcmp(mp_cstr(kw->items[i]),name)) return argv[np+i];
    return dflt;
}
static void kw_only(const char *fn, TupleObj *kw, const char **allowed){
    if(!kw) return;
    for(int64_t i=0;i<kw->len;i++){ int ok=0; for(int k=0;allowed && allowed[k];k++) if(!strcmp(mp_cstr(kw->items[i]),allowed[k])) ok=1;
        if(!ok) mp_raise_t(E_TypeError,"%s() got an unexpected keyword argument '%s'",fn,mp_cstr(kw->items[i])); }
}
static void no_kw(const char *fn, TupleObj *kw){ if(kw && kw->len) mp_raise_t(E_TypeError,"%s() takes no keyword arguments",fn); }
static void nargs(const char *fn, int n, int lo, int hi){
    if(n<lo || n>hi){
        if(lo==hi) mp_raise_t(E_TypeError,"%s() takes exactly %s (%d given)",fn,lo==1?"one argument":lo==0?"no arguments":"2 arguments",n);
        if(n<lo) mp_raise_t(E_TypeError,"%s expected at least %d argument%s, got %d",fn,lo,lo==1?"":"s",n);
        mp_raise_t(E_TypeError,"%s expected at most %d argument%s, got %d",fn,hi,hi==1?"":"s",n);
    }
}

/* ---------------------------------------------------------------- output */
static SBuf outbuf;
static int out_tty=-1;
void mp_flush_stdout(void){
    if(!outbuf.n) return;
#if defined(MPY_KOLIBRI)
    printf("%.*s",(int)outbuf.n,outbuf.s);
#else
    fwrite(outbuf.s,1,(size_t)outbuf.n,stdout); fflush(stdout);
#endif
    outbuf.n=0;
}
void mp_write_out(const char *s, int64_t n){
    sb_put(&outbuf,s,n);
#if defined(MPY_KOLIBRI)
    mp_flush_stdout();
#else
    if(out_tty<0) out_tty=isatty(1);
    if(outbuf.n>8192 || (out_tty && memchr(s,'\n',(size_t)n))) mp_flush_stdout();
#endif
}
void mp_write_err(const char *s, int64_t n){
    mp_flush_stdout();
#if defined(MPY_KOLIBRI)
    printf("%.*s",(int)n,s);
#else
    fwrite(s,1,(size_t)n,stderr); fflush(stderr);
#endif
}

/* ---------------------------------------------------------------- type constructors */
static Value make_object(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)argv; (void)kw;
    if(t!=T_object) return mp_instance_call(t,argc,argv,kw);
    if(argc) mp_raise_t(E_TypeError,"object() takes no arguments");
    Obj *o=(Obj*)mp_alloc(T_object,sizeof(Obj)); return v_obj(o);
}
static Value make_type(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t; no_kw("type",kw);
    if(argc==1) return v_obj(TYPE(argv[0]));
    if(argc!=3) mp_raise_t(E_TypeError,"type() takes 1 or 3 arguments");
    if(!IS_STR(argv[0]) || !IS(argv[1],T_tuple) || !IS(argv[2],T_dict)) mp_raise_t(E_TypeError,"type() argument types: str, tuple, dict");
    Value ns=mp_dict(); mp_dict_update_from(AS_DICT(ns),argv[2]);
    if(!mp_dict_get_s(AS_DICT(ns),"__module__",NULL)) mp_dict_set_s(AS_DICT(ns),"__module__",mp_str("__main__"));
    return mp_make_class(argv[0],argv[1],AS_DICT(ns));
}
static Value make_int(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t;
    int np=npos(argc,kw);
    Value x= np>0 ? argv[0] : v_undef();
    Value basev= np>1 ? argv[1] : kwarg(argc,argv,kw,"base",v_undef());
    if(x.k==V_UNDEF){ if(basev.k!=V_UNDEF) mp_raise_t(E_TypeError,"int() missing string argument"); return v_int(0); }
    if(basev.k!=V_UNDEF || IS_STR(x) || IS(x,T_bytes)){
        int64_t base= basev.k==V_UNDEF ? 10 : mp_index(basev,"base");
        if(base!=0 && (base<2 || base>36)) mp_raise_t(E_ValueError,"int() base must be >= 2 and <= 36, or 0");
        const char *s; int64_t n;
        if(IS_STR(x)){ s=AS_STR(x)->s; n=AS_STR(x)->len; }
        else if(IS(x,T_bytes)){ s=(const char*)AS_BYTES(x)->s; n=AS_BYTES(x)->len; }
        else mp_raise_t(E_TypeError,"int() can't convert non-string with explicit base");
        int64_t v; int r=mp_parse_int(s,n,(int)base,&v);
        if(r<0) mp_raise_t(E_OverflowError,"int too large (ints are 64-bit)");
        if(!r){ Value rr=IS_STR(x)?mp_repr(x):mp_repr(x); mp_raise_t(E_ValueError,"invalid literal for int() with base %lld: %s",(long long)base,mp_cstr(rr)); }
        return v_int(v);
    }
    if(x.k==V_INT) return x;
    if(x.k==V_BOOL) return v_int(x.u.i);
    if(x.k==V_FLOAT){
        double f=x.u.f;
        if(isnan(f)) mp_raise_t(E_ValueError,"cannot convert float NaN to integer");
        if(isinf(f)) mp_raise_t(E_OverflowError,"cannot convert float infinity to integer");
        if(f>=9.2233720368547758e18 || f<-9.2233720368547758e18) mp_raise_t(E_OverflowError,"int too large (ints are 64-bit)");
        return v_int((int64_t)f);
    }
    if(TYPE(x)->flags&TF_DUNDERS){
        Value m=mp_type_lookup_s(TYPE(x),"__int__");
        if(m.k==V_UNDEF) m=mp_type_lookup_s(TYPE(x),"__index__");
        if(m.k!=V_UNDEF){ Value r=mp_call1(m,x); if(IS_INTLIKE(r)) return v_int(r.u.i); mp_raise_t(E_TypeError,"__int__ returned non-int (type %s)",mp_type_name(r)); }
        m=mp_type_lookup_s(TYPE(x),"__trunc__");
        if(m.k!=V_UNDEF) return mp_call1(m,x);
    }
    mp_raise_t(E_TypeError,"int() argument must be a string, a bytes-like object or a real number, not '%s'",mp_type_name(x));
}
static Value make_float(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t; no_kw("float",kw); nargs("float",argc,0,1);
    if(!argc) return v_float(0);
    Value x=argv[0];
    if(x.k==V_FLOAT) return x;
    if(IS_INTLIKE(x)) return v_float((double)x.u.i);
    if(IS_STR(x)){ double d; if(!mp_parse_float(AS_STR(x)->s,AS_STR(x)->len,&d)){ Value r=mp_repr(x); mp_raise_t(E_ValueError,"could not convert string to float: %s",mp_cstr(r)); } return v_float(d); }
    if(TYPE(x)->flags&TF_DUNDERS){ Value m=mp_type_lookup_s(TYPE(x),"__float__"); if(m.k!=V_UNDEF) return mp_call1(m,x);
        m=mp_type_lookup_s(TYPE(x),"__index__"); if(m.k!=V_UNDEF) return v_float((double)mp_index(x,"float")); }
    mp_raise_t(E_TypeError,"float() argument must be a string or a real number, not '%s'",mp_type_name(x));
}
/* A real number at p ([+-] digits [. digits] [e [+-] digits], inf, infinity, nan) -> its end; NULL if none */
static const char *cx_real(const char *p, const char *end, double *v){
    const char *q=p; if(q<end && (*q=='+'||*q=='-')) q++;
    if(end-q>=8 && !strncasecmp(q,"infinity",8)) q+=8;
    else if(end-q>=3 && (!strncasecmp(q,"inf",3)||!strncasecmp(q,"nan",3))) q+=3;
    else { int d=0;
        while(q<end && isdigit((unsigned char)*q)){ q++; d++; }
        if(q<end && *q=='.'){ q++; while(q<end && isdigit((unsigned char)*q)){ q++; d++; } }
        if(!d) return NULL;
        if(q<end && (*q=='e'||*q=='E')){ const char *r=q+1; if(r<end && (*r=='+'||*r=='-')) r++;
            if(r<end && isdigit((unsigned char)*r)){ while(r<end && isdigit((unsigned char)*r)) r++; q=r; } } }
    size_t n=(size_t)(q-p); char *b=(char*)malloc(n+1); memcpy(b,p,n); b[n]=0;   /* (strtod alone would take hex and spaces) */
    *v=strtod(b,NULL); free(b);
    return q;
}
/* complex("1+2j"), " (3-4J) ", "j", "-1.5e3j", "1+j", "inf+nanj" -> 1; 0 when malformed */
static int cx_parse(const char *p, size_t len, double *re, double *im){
    const char *end=p+len;
    while(p<end && isspace((unsigned char)*p)) p++;
    while(end>p && isspace((unsigned char)end[-1])) end--;
    if(p<end && *p=='(' && end[-1]==')'){ p++; end--;
        while(p<end && isspace((unsigned char)*p)) p++;
        while(end>p && isspace((unsigned char)end[-1])) end--; }
    #define ISJ(q) ((q)<end && (*(q)=='j'||*(q)=='J') && (q)+1==end)
    double a, b; const char *q=cx_real(p,end,&a);
    if(!q){ double sg=1; if(p<end && (*p=='+'||*p=='-')){ sg=*p=='-'?-1:1; p++; }
        if(ISJ(p)){ *im=sg; return 1; } return 0; }
    if(q==end){ *re=a; return 1; }
    if(ISJ(q)){ *im=a; return 1; }
    if(*q!='+' && *q!='-') return 0;
    const char *r=cx_real(q,end,&b);
    if(!r){ b=*q=='-'?-1:1; r=q+1; }
    if(ISJ(r)){ *re=a; *im=b; return 1; }
    #undef ISJ
    return 0;
}
static Value make_complex(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t; (void)kw;
    double re=0, im=0;
    if(argc>0){
        if(IS(argv[0],T_complex)){ re=((ComplexObj*)argv[0].u.o)->re; im=((ComplexObj*)argv[0].u.o)->im; }
        else if(IS_STR(argv[0])){
            if(argc>1) mp_raise_t(E_TypeError,"complex() argument 'real' must be a real number, not str");
            if(!cx_parse(AS_STR(argv[0])->s,(size_t)AS_STR(argv[0])->len,&re,&im)) mp_raise_t(E_ValueError,"complex() arg is a malformed string");
        }
        else re=mp_float_of(argv[0]);
    }
    if(argc>1){ if(IS(argv[1],T_complex)){ re-=((ComplexObj*)argv[1].u.o)->im; im+=((ComplexObj*)argv[1].u.o)->re; } else im+=mp_float_of(argv[1]); }
    return mp_complex(re,im);
}
static Value make_bool(Type *t, int argc, Value *argv, TupleObj *kw){ (void)t; no_kw("bool",kw); nargs("bool",argc,0,1); return v_bool(argc?mp_truth(argv[0]):0); }
static Value make_str(Type *t, int argc, Value *argv, TupleObj *kw){
    int np=npos(argc,kw);
    Value x= np>0 ? argv[0] : kwarg(argc,argv,kw,"object",v_undef());
    Value enc= np>1 ? argv[1] : kwarg(argc,argv,kw,"encoding",v_undef());
    Value r;
    if(x.k==V_UNDEF) r=mp_str("");
    else if(enc.k!=V_UNDEF || (IS(x,T_bytes) && kwarg(argc,argv,kw,"errors",v_undef()).k!=V_UNDEF)){
        Value a[3]={x, enc.k==V_UNDEF?mp_str("utf-8"):enc, np>2?argv[2]:kwarg(argc,argv,kw,"errors",mp_str("strict"))};
        r=mp_callmethod(x,"decode",2,a+1);
    } else r=mp_tostr(x);
    if(t!=T_str){ /* (a subclass of str: not supported as a layout) */ }
    return r;
}
static Value make_bytes(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t;
    int np=npos(argc,kw);
    if(!np) return mp_bytes("",0);
    Value x=argv[0];
    if(IS_STR(x)){
        Value enc= np>1 ? argv[1] : kwarg(argc,argv,kw,"encoding",v_undef());
        if(enc.k==V_UNDEF) mp_raise_t(E_TypeError,"string argument without an encoding");
        return mp_callmethod(x,"encode",1,&enc);
    }
    if(IS_INTLIKE(x)){ if(x.u.i<0) mp_raise_t(E_ValueError,"negative count"); char *z=(char*)xmalloc((size_t)x.u.i+1); memset(z,0,(size_t)x.u.i); Value r=mp_bytes(z,x.u.i); free(z); return r; }
    if(IS(x,T_bytes)) return x;
    if(IS(x,T_buffer)) return mp_bytes(((BufferObj*)x.u.o)->data,((BufferObj*)x.u.o)->len);
    Value l=mp_list_of(x); SBuf b={0};
    for(int64_t i=0;i<AS_LIST(l)->len;i++){ int64_t v=mp_index(AS_LIST(l)->items[i],"bytes"); if(v<0||v>255) mp_raise_t(E_ValueError,"bytes must be in range(0, 256)"); sb_putc(&b,(char)v); }
    Value r=mp_bytes(b.s?b.s:"",b.n); free(b.s); return r;
}
static Value make_list(Type *t, int argc, Value *argv, TupleObj *kw){ (void)t; no_kw("list",kw); nargs("list",argc,0,1); return argc ? mp_list_of(argv[0]) : mp_list(0,NULL); }
static Value make_tuple(Type *t, int argc, Value *argv, TupleObj *kw){ (void)t; no_kw("tuple",kw); nargs("tuple",argc,0,1);
    if(!argc) return mp_tuple(0,NULL);
    if(IS(argv[0],T_tuple)) return argv[0];
    Value l=mp_list_of(argv[0]); return mp_tuple(AS_LIST(l)->len,AS_LIST(l)->items); }
static Value make_dict(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t; int np=npos(argc,kw);
    if(np>1) mp_raise_t(E_TypeError,"dict expected at most 1 argument, got %d",np);
    Value d=mp_dict();
    if(np==1) mp_dict_update_from(AS_DICT(d),argv[0]);
    if(kw) for(int64_t i=0;i<kw->len;i++) mp_dict_set(AS_DICT(d),kw->items[i],argv[np+i]);
    return d;
}
static Value make_set(Type *t, int argc, Value *argv, TupleObj *kw){
    no_kw(t->name->s,kw); nargs(t->name->s,argc,0,1);
    if(argc && t==T_frozenset && IS(argv[0],T_frozenset)) return argv[0];
    if(argc && argv[0].k==V_OBJ && argv[0].u.o->type->layout==LY_SET) return mp_set_copy((SetObj*)argv[0].u.o,t);
    Value s=mp_set(t);
    if(argc){ Value it=mp_iter(argv[0]), x; while(mp_next(it,&x)) mp_set_add((SetObj*)s.u.o,x); }
    return s;
}
static Value make_range(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t; no_kw("range",kw);
    if(argc<1 || argc>3) mp_raise_t(E_TypeError,argc<1?"range expected at least 1 argument, got %d":"range expected at most 3 arguments, got %d",argc);
    for(int i=0;i<argc;i++) if(!IS_INTLIKE(argv[i]) && !(TYPE(argv[i])->flags&TF_DUNDERS)) mp_raise_t(E_TypeError,"'%s' object cannot be interpreted as an integer",mp_type_name(argv[i]));
    int64_t a=0, b, s=1;
    if(argc==1) b=mp_index(argv[0],"range");
    else { a=mp_index(argv[0],"range"); b=mp_index(argv[1],"range"); if(argc==3) s=mp_index(argv[2],"range"); }
    if(s==0) mp_raise_t(E_ValueError,"range() arg 3 must not be zero");
    return mp_range(a,b,s);
}
static Value make_slice(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t; no_kw("slice",kw); nargs("slice",argc,1,3);
    if(argc==1) return mp_slice(v_none(),argv[0],v_none());
    return mp_slice(argv[0],argv[1],argc>2?argv[2]:v_none());
}
static Value make_property(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t;
    PropertyObj *p=(PropertyObj*)mp_alloc(T_property,sizeof(PropertyObj));
    int np=npos(argc,kw);
    p->get= np>0 ? argv[0] : kwarg(argc,argv,kw,"fget",v_none());
    p->set= np>1 ? argv[1] : kwarg(argc,argv,kw,"fset",v_none());
    p->del= np>2 ? argv[2] : kwarg(argc,argv,kw,"fdel",v_none());
    p->doc= np>3 ? argv[3] : kwarg(argc,argv,kw,"doc",v_none());
    return v_obj(p);
}
static Value make_box(Type *t, int argc, Value *argv, TupleObj *kw){
    no_kw(t->name->s,kw); nargs(t->name->s,argc,1,1);
    BoxObj *b=(BoxObj*)mp_alloc(t,sizeof(BoxObj)); b->v=argv[0]; return v_obj(b);
}
static Value make_super(Type *t, int argc, Value *argv, TupleObj *kw){
    (void)t; no_kw("super",kw);
    Type *start; Value obj;
    if(argc==0){
        Frame *f=mp_ts->frame;
        if(!f || !f->code || f->code->argc<1) mp_raise_t(E_RuntimeError,"super(): no arguments");
        Value cls=v_undef();
        for(int i=0;i<f->code->nfrees;i++) if(!strcmp(f->code->freenames[i]->s,"__class__")) cls=((CellObj*)f->cells[f->code->ncells+i].u.o)->v;
        if(cls.k==V_UNDEF) mp_raise_t(E_RuntimeError,"super(): __class__ cell not found");
        obj=f->fast[0];
        if(obj.k==V_UNDEF && f->code->ncells){ for(int i=0;i<f->code->ncells;i++) if(f->code->cellarg[i]==0) obj=((CellObj*)f->cells[i].u.o)->v; }
        if(obj.k==V_UNDEF) mp_raise_t(E_RuntimeError,"super(): arg[0] deleted");
        start=AS_TYPE(cls);
    } else {
        if(!IS(argv[0],T_type)) mp_raise_t(E_TypeError,"super() argument 1 must be a type, not %s",mp_type_name(argv[0]));
        start=AS_TYPE(argv[0]); obj= argc>1 ? argv[1] : v_none();
    }
    SuperObj *s=(SuperObj*)mp_alloc(T_super,sizeof(SuperObj));
    s->start=start; s->obj=obj;
    s->objtype= IS(obj,T_type) && mp_is_subtype(AS_TYPE(obj),start) ? AS_TYPE(obj) : TYPE(obj);
    if(!mp_is_subtype(s->objtype,start)) mp_raise_t(E_TypeError,"super(type, obj): obj must be an instance or subtype of type");
    return v_obj(s);
}

/* ---------------------------------------------------------------- iterators made here */
int mp_native_iter_next(IterObj *it, Value *out){
    switch(it->kind){
        case IT_ENUM:{ Value x; if(!mp_next(it->src,&x)) return 0; Value p[2]={v_int(it->i++),x}; *out=mp_tuple(2,p); return 1; }
        case IT_ZIP:{
            TupleObj *its=AS_TUPLE(it->src); int64_t n=its->len;
            if(!n) return 0;
            Value r=mp_tuple(n,NULL);
            for(int64_t i=0;i<n;i++){
                Value x;
                if(!mp_next(its->items[i],&x)){
                    if(mp_truth(it->aux)){
                        if(i>0) mp_raise_t(E_ValueError,"zip() argument %lld is shorter than argument%s%lld",(long long)i+1,i==1?" ":"s 1-",(long long)i);
                        for(int64_t j=1;j<n;j++){ Value y; if(mp_next(its->items[j],&y)) mp_raise_t(E_ValueError,"zip() argument %lld is longer than argument%s%lld",(long long)j+1,j==1?" ":"s 1-",(long long)j); }
                    }
                    return 0;
                }
                AS_TUPLE(r)->items[i]=x;
            }
            *out=r; return 1; }
        case IT_MAP:{
            TupleObj *its=AS_TUPLE(it->aux); int64_t n=its->len;
            Value args=mp_tuple(n,NULL);
            for(int64_t i=0;i<n;i++){ Value x; if(!mp_next(its->items[i],&x)) return 0; AS_TUPLE(args)->items[i]=x; }
            *out=mp_call(it->src,(int)n,AS_TUPLE(args)->items,NULL); return 1; }
        case IT_FILTER:{
            Value x;
            while(mp_next(it->aux,&x)){
                int ok= IS_NONE(it->src) ? mp_truth(x) : mp_truth(mp_call1(it->src,x));
                if(ok){ *out=x; return 1; }
            }
            return 0; }
        case IT_CALL:{ if(it->i) return 0; Value x=mp_call0(it->src); if(mp_eq(x,it->aux)){ it->i=1; return 0; } *out=x; return 1; }
        case IT_NATIVE: return mp_native_send(v_obj(it),v_none(),out);
        default: mp_raise_t(E_SystemError,"bad iterator");
    }
}

/* ---------------------------------------------------------------- builtin functions */
#define BI(name) static Value bi_##name(int argc, Value *argv, TupleObj *kw)
BI(len){ no_kw("len",kw); nargs("len",argc,1,1); return v_int(mp_len(argv[0])); }
BI(repr){ no_kw("repr",kw); nargs("repr",argc,1,1); return mp_repr(argv[0]); }
BI(ascii){ no_kw("ascii",kw); nargs("ascii",argc,1,1);
    Value r=mp_repr(argv[0]); StrObj *s=AS_STR(r); SBuf b={0}; int64_t p=0;
    while(p<s->len){ int64_t st=p; uint32_t c=(uint32_t)mp_utf8_decode(s->s,s->len,&p); if(c<0x80) sb_put(&b,s->s+st,p-st); else if(c<0x100) sb_printf(&b,"\\x%02x",c); else if(c<0x10000) sb_printf(&b,"\\u%04x",c); else sb_printf(&b,"\\U%08x",c); }
    return sb_value(&b); }
BI(abs){ no_kw("abs",kw); nargs("abs",argc,1,1); Value x=argv[0];
    if(x.k==V_INT||x.k==V_BOOL){ if(x.u.i==INT64_MIN) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)"); return v_int(x.u.i<0?-x.u.i:x.u.i); }
    if(x.k==V_FLOAT) return v_float(fabs(x.u.f));
    if(IS(x,T_complex)) return v_float(hypot(((ComplexObj*)x.u.o)->re,((ComplexObj*)x.u.o)->im));
    Value m=mp_type_lookup_s(TYPE(x),"__abs__"); if(m.k!=V_UNDEF) return mp_call1(m,x);
    mp_raise_t(E_TypeError,"bad operand type for abs(): '%s'",mp_type_name(x)); }
static Value minmax(int argc, Value *argv, TupleObj *kw, int is_max){
    const char *name= is_max?"max":"min";
    static const char *ok[]={"key","default",NULL}; kw_only(name,kw,ok);
    int np=npos(argc,kw);
    Value key=kwarg(argc,argv,kw,"key",v_none()), dflt=kwarg(argc,argv,kw,"default",v_undef());
    if(np==0) mp_raise_t(E_TypeError,"%s expected at least 1 argument, got 0",name);
    Value it= np==1 ? mp_iter(argv[0]) : mp_iter(mp_tuple(np,argv));
    if(np>1 && dflt.k!=V_UNDEF) mp_raise_t(E_TypeError,"Cannot specify a default for %s() with multiple positional arguments",name);
    Value best=v_undef(), bestk=v_undef(), x;
    while(mp_next(it,&x)){
        Value k= IS_NONE(key) ? x : mp_call1(key,x);
        if(best.k==V_UNDEF || mp_truth(mp_compare(is_max?OP_Gt:OP_Lt,k,bestk))){ best=x; bestk=k; }
    }
    if(best.k==V_UNDEF){ if(dflt.k!=V_UNDEF) return dflt; mp_raise_t(E_ValueError,"%s() iterable argument is empty",name); }
    return best;
}
BI(min){ return minmax(argc,argv,kw,0); }
BI(max){ return minmax(argc,argv,kw,1); }
BI(sum){
    int np=npos(argc,kw); if(np<1||np>2) nargs("sum",np,1,2);
    Value acc= np>1 ? argv[1] : kwarg(argc,argv,kw,"start",v_int(0));
    if(IS_STR(acc)) mp_raise_t(E_TypeError,"sum() can't sum strings [use ''.join(seq) instead]");
    Value it=mp_iter(argv[0]), x;
    int64_t ia= acc.k==V_INT ? acc.u.i : 0; int ints= acc.k==V_INT;
    double fa=0, fc=0; int floats=0;          /* floats: CPython's compensated (Neumaier) sum */
    while(mp_next(it,&x)){
        if(ints && x.k==V_INT){ int64_t r; if(!__builtin_add_overflow(ia,x.u.i,&r)){ ia=r; continue; } }
        if(ints){ acc=v_int(ia); ints=0; }
        if(floats && (x.k==V_FLOAT || x.k==V_INT || x.k==V_BOOL)){
            double v= x.k==V_FLOAT ? x.u.f : (double)x.u.i, t=fa+v;
            if(fabs(fa)>=fabs(v)) fc+=(fa-t)+v; else fc+=(v-t)+fa;
            fa=t; continue; }
        if(floats){ if(fc!=0 && isfinite(fc)) fa+=fc; acc=v_float(fa); floats=0; }
        acc=mp_binop(OP_Add,acc,x);
        if(acc.k==V_FLOAT){ floats=1; fa=acc.u.f; fc=0; }
    }
    if(ints) return v_int(ia);
    if(floats){ if(fc!=0 && isfinite(fc)) fa+=fc; return v_float(fa); }
    return acc;
}
BI(sorted){
    static const char *ok[]={"key","reverse",NULL}; kw_only("sorted",kw,ok);
    int np=npos(argc,kw); if(np!=1) mp_raise_t(E_TypeError,"sorted expected 1 argument, got %d",np);
    Value l=mp_list_of(argv[0]);
    mp_sort(l,kwarg(argc,argv,kw,"key",v_none()),mp_truth(kwarg(argc,argv,kw,"reverse",v_bool(0))));
    return l;
}
BI(any){ no_kw("any",kw); nargs("any",argc,1,1); Value it=mp_iter(argv[0]), x; while(mp_next(it,&x)) if(mp_truth(x)) return v_bool(1); return v_bool(0); }
BI(all){ no_kw("all",kw); nargs("all",argc,1,1); Value it=mp_iter(argv[0]), x; while(mp_next(it,&x)) if(!mp_truth(x)) return v_bool(0); return v_bool(1); }
BI(enumerate){
    int np=npos(argc,kw); Value st= np>1 ? argv[1] : kwarg(argc,argv,kw,"start",v_int(0));
    if(np<1) mp_raise_t(E_TypeError,"enumerate() missing required argument 'iterable'");
    Value it=mp_iter_kind(IT_ENUM,mp_iter(argv[0]),v_undef(),v_undef()); ((IterObj*)it.u.o)->i=mp_index(st,"start"); return it; }
BI(zip){
    int np=npos(argc,kw); Value its=mp_tuple(np,NULL);
    for(int i=0;i<np;i++) AS_TUPLE(its)->items[i]=mp_iter(argv[i]);
    return mp_iter_kind(IT_ZIP,its,v_bool(mp_truth(kwarg(argc,argv,kw,"strict",v_bool(0)))),v_undef()); }
BI(map){ no_kw("map",kw); if(argc<2) mp_raise_t(E_TypeError,"map() must have at least two arguments.");
    Value its=mp_tuple(argc-1,NULL); for(int i=1;i<argc;i++) AS_TUPLE(its)->items[i-1]=mp_iter(argv[i]);
    return mp_iter_kind(IT_MAP,argv[0],its,v_undef()); }
BI(filter){ no_kw("filter",kw); nargs("filter",argc,2,2); return mp_iter_kind(IT_FILTER,argv[0],mp_iter(argv[1]),v_undef()); }
BI(reversed){ no_kw("reversed",kw); nargs("reversed",argc,1,1); Value x=argv[0];
    Value m=mp_type_lookup_s(TYPE(x),"__reversed__");
    if(m.k!=V_UNDEF && !IS(m,T_native)) return mp_call1(m,x);
    if(IS(x,T_range)){ RangeObj *r=(RangeObj*)x.u.o; int64_t n=mp_len(x); return mp_iter(mp_range(r->start+(n-1)*r->step,r->start-r->step,-r->step)); }
    if(IS(x,T_dict)){ Value l=mp_list_of(x); Value it=mp_iter_kind(IT_REVLIST,l,v_undef(),v_undef()); ((IterObj*)it.u.o)->i=AS_LIST(l)->len-1; return it; }
    if(!(IS(x,T_list)||IS(x,T_tuple)||IS_STR(x)||IS(x,T_bytes)||((TYPE(x)->flags&TF_DUNDERS) && mp_type_lookup_s(TYPE(x),"__getitem__").k!=V_UNDEF)))
        mp_raise_t(E_TypeError,"'%s' object is not reversible",mp_type_name(x));
    if(IS(x,T_bytes)) x=mp_list_of(x);
    Value it=mp_iter_kind(IT_REVLIST,x,v_undef(),v_undef()); ((IterObj*)it.u.o)->i=mp_len(x)-1; return it; }
BI(iter){ no_kw("iter",kw); nargs("iter",argc,1,2);
    if(argc==2) return mp_iter_kind(IT_CALL,argv[0],argv[1],v_undef());
    return mp_iter(argv[0]); }
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);
BI(mpy_tstr){                                       /* t"...": a string.templatelib.Template */
    (void)kw; static Value fn; if(fn.k!=V_OBJ){ Value nm=mp_str("string.templatelib"), fl=mp_list(0,NULL); mp_list_append(fl,mp_str("_from_parts"));
        Value a[4]={nm,v_none(),v_none(),fl}; Value mod=mp_builtin_import(4,a,NULL); fn=mp_getattr_s(mod,"_from_parts"); mp_gc_add_root(&fn); }
    return mp_call1(fn,mp_tuple(argc,argv)); }
BI(anext){ no_kw("anext",kw); nargs("anext",argc,1,2); return mp_anext(argv[0],argc>1,argc>1?argv[1]:v_none()); }
BI(aiter){ no_kw("aiter",kw); nargs("aiter",argc,1,1); Value m=mp_type_lookup_s(TYPE(argv[0]),"__aiter__");
    if(m.k==V_UNDEF) mp_raise_t(E_TypeError,"'%s' object is not an async iterable",mp_type_name(argv[0]));
    return mp_call1(m,argv[0]); }
BI(next){ no_kw("next",kw); nargs("next",argc,1,2);
    Value it=argv[0], v;
    if(!(IS(it,T_iter) || IS(it,T_generator) || IS(it,T_file) || ((TYPE(it)->flags&TF_DUNDERS) && mp_type_lookup_s(TYPE(it),"__next__").k!=V_UNDEF)))
        mp_raise_t(E_TypeError,"'%s' object is not an iterator",mp_type_name(it));
    if(IS(it,T_generator) && argc==1){
        int done; Value r=mp_gen_send(it,v_none(),&done);
        if(done) mp_raise(mp_exc_args(E_StopIteration,IS_NONE(r)?mp_tuple(0,NULL):mp_tuple(1,&r)));
        return r;
    }
    if(mp_next(it,&v)) return v;
    if(argc>1) return argv[1];
    mp_raise(mp_exc_args(E_StopIteration,mp_tuple(0,NULL))); }
BI(isinstance){ no_kw("isinstance",kw); nargs("isinstance",argc,2,2);
    Value c=argv[1];
    if(IS(c,T_tuple)){ for(int64_t i=0;i<AS_TUPLE(c)->len;i++){ Value a[2]={argv[0],AS_TUPLE(c)->items[i]}; if(mp_truth(bi_isinstance(2,a,NULL))) return v_bool(1); } return v_bool(0); }
    if(!IS(c,T_type)){ Value m=mp_type_lookup_s(TYPE(c),"__instancecheck__");       /* int | str (typing.Union) */
        if(m.k!=V_UNDEF && !IS(m,T_native)) return v_bool(mp_truth(mp_call2(m,c,argv[0])));
        mp_raise_t(E_TypeError,"isinstance() arg 2 must be a type, a tuple of types, or a union"); }
    return v_bool(mp_isinstance(argv[0],AS_TYPE(c))); }
BI(issubclass){ no_kw("issubclass",kw); nargs("issubclass",argc,2,2);
    if(!IS(argv[0],T_type)) mp_raise_t(E_TypeError,"issubclass() arg 1 must be a class");
    Value c=argv[1];
    if(IS(c,T_tuple)){ for(int64_t i=0;i<AS_TUPLE(c)->len;i++){ Value a[2]={argv[0],AS_TUPLE(c)->items[i]}; if(mp_truth(bi_issubclass(2,a,NULL))) return v_bool(1); } return v_bool(0); }
    if(!IS(c,T_type)){ Value m=mp_type_lookup_s(TYPE(c),"__subclasscheck__");
        if(m.k!=V_UNDEF && !IS(m,T_native)) return v_bool(mp_truth(mp_call2(m,c,argv[0])));
        mp_raise_t(E_TypeError,"issubclass() arg 2 must be a class, a tuple of classes, or a union"); }
    return v_bool(mp_is_subtype(AS_TYPE(argv[0]),AS_TYPE(c))); }
BI(hasattr){ no_kw("hasattr",kw); nargs("hasattr",argc,2,2); if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"attribute name must be string, not '%s'",mp_type_name(argv[1]));
    Value v; Catch c; int r;
    if(!CATCH_BEGIN(c)){ r=mp_getattr_opt(argv[0],mp_intern(mp_cstr(argv[1])),&v); CATCH_END(c); return v_bool(r); }
    Value e=mp_catch_exc(&c); if(mp_isinstance(e,E_AttributeError)) return v_bool(0); mp_raise(e); }
BI(getattr){ no_kw("getattr",kw); nargs("getattr",argc,2,3); if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"attribute name must be string, not '%s'",mp_type_name(argv[1]));
    Value name=mp_intern(mp_cstr(argv[1]));
    if(argc==2) return mp_getattr(argv[0],name);
    Value v; Catch c; int r;
    if(!CATCH_BEGIN(c)){ r=mp_getattr_opt(argv[0],name,&v); CATCH_END(c); return r?v:argv[2]; }
    Value e=mp_catch_exc(&c); if(mp_isinstance(e,E_AttributeError)) return argv[2]; mp_raise(e); }
BI(setattr){ no_kw("setattr",kw); nargs("setattr",argc,3,3); if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"attribute name must be string, not '%s'",mp_type_name(argv[1])); mp_setattr(argv[0],mp_intern(mp_cstr(argv[1])),argv[2]); return v_none(); }
BI(delattr){ no_kw("delattr",kw); nargs("delattr",argc,2,2); mp_delattr(argv[0],mp_intern(mp_cstr(argv[1]))); return v_none(); }
BI(callable){ no_kw("callable",kw); nargs("callable",argc,1,1); Value x=argv[0]; Type *t=TYPE(x);
    return v_bool(t->layout==LY_FUNC||t->layout==LY_NATIVE||t->layout==LY_METHOD||t->layout==LY_TYPE||t->layout==LY_STATICMETHOD||((t->flags&TF_DUNDERS) && mp_type_lookup_s(t,"__call__").k!=V_UNDEF)); }
BI(hash){ no_kw("hash",kw); nargs("hash",argc,1,1); return v_int((int64_t)mp_hash(argv[0])); }
BI(id){ no_kw("id",kw); nargs("id",argc,1,1); Value x=argv[0]; if(x.k==V_OBJ) return v_int((int64_t)(intptr_t)x.u.o); return v_int((int64_t)mp_hash(x)*16+x.k); }
BI(chr){ no_kw("chr",kw); nargs("chr",argc,1,1); int64_t c=mp_index(argv[0],"chr"); if(c<0||c>0x10FFFF) mp_raise_t(E_ValueError,"chr() arg not in range(0x110000)"); char u[4]; int m=mp_utf8_encode(u,(uint32_t)c); return mp_strn(u,m); }
BI(ord){ no_kw("ord",kw); nargs("ord",argc,1,1); Value s=argv[0];
    if(IS_STR(s)){ if(AS_STR(s)->cplen!=1) mp_raise_t(E_TypeError,"ord() expected a character, but string of length %lld found",(long long)AS_STR(s)->cplen); int64_t p=0; return v_int(mp_utf8_decode(AS_STR(s)->s,AS_STR(s)->len,&p)); }
    if(IS(s,T_bytes)){ if(AS_BYTES(s)->len!=1) mp_raise_t(E_TypeError,"ord() expected a character, but string of length %lld found",(long long)AS_BYTES(s)->len); return v_int(AS_BYTES(s)->s[0]); }
    mp_raise_t(E_TypeError,"ord() expected string of length 1, but %s found",mp_type_name(s)); }
static Value radix(Value x, int base, const char *prefix){
    int64_t v=mp_index(x,"radix"); uint64_t a= v<0 ? (uint64_t)(-(v+1))+1 : (uint64_t)v;
    char d[80]; int n=0; do{ d[n++]="0123456789abcdef"[a%(uint64_t)base]; a/=(uint64_t)base; }while(a);
    SBuf b={0}; if(v<0) sb_putc(&b,'-'); sb_puts(&b,prefix); for(int i=n-1;i>=0;i--) sb_putc(&b,d[i]); return sb_value(&b);
}
BI(hex){ no_kw("hex",kw); nargs("hex",argc,1,1); return radix(argv[0],16,"0x"); }
BI(oct){ no_kw("oct",kw); nargs("oct",argc,1,1); return radix(argv[0],8,"0o"); }
BI(bin){ no_kw("bin",kw); nargs("bin",argc,1,1); return radix(argv[0],2,"0b"); }
BI(divmod){ no_kw("divmod",kw); nargs("divmod",argc,2,2);
    Value a=argv[0], b=argv[1];
    if(TYPE(a)->flags&TF_DUNDERS){ Value m=mp_type_lookup_s(TYPE(a),"__divmod__"); if(m.k!=V_UNDEF) return mp_call2(m,a,b); }
    if(!((IS_INTLIKE(a)||a.k==V_FLOAT) && (IS_INTLIKE(b)||b.k==V_FLOAT))) mp_raise_t(E_TypeError,"unsupported operand type(s) for divmod(): '%s' and '%s'",mp_type_name(a),mp_type_name(b));
    Value r[2]={mp_binop(OP_FloorDiv,a,b),mp_binop(OP_Mod,a,b)}; return mp_tuple(2,r); }
/* a*b mod m without 128-bit numbers (a, b < m < 2**63): as compiled programs do it */
static uint64_t mulmod64(uint64_t a, uint64_t b, uint64_t m){
    uint64_t r=0;
    while(a){ if(a&1){ r+=b; if(r>=m) r-=m; } b+=b; if(b>=m) b-=m; a>>=1; }
    return r;
}
BI(pow){
    int np=npos(argc,kw);
    Value base= np>0 ? argv[0] : kwarg(argc,argv,kw,"base",v_undef());
    Value exp= np>1 ? argv[1] : kwarg(argc,argv,kw,"exp",v_undef());
    Value mod= np>2 ? argv[2] : kwarg(argc,argv,kw,"mod",v_none());
    if(base.k==V_UNDEF || exp.k==V_UNDEF) mp_raise_t(E_TypeError,"pow() missing required argument");
    if(IS_NONE(mod)) return mp_binop(OP_Pow,base,exp);
    if(!IS_INTLIKE(base)||!IS_INTLIKE(exp)||!IS_INTLIKE(mod)) mp_raise_t(E_TypeError,"pow() 3rd argument not allowed unless all arguments are integers");
    int64_t m=mod.u.i, e=exp.u.i; if(m==0) mp_raise_t(E_ValueError,"pow() 3rd argument cannot be 0");
    uint64_t mm= m<0 ? (uint64_t)0-(uint64_t)m : (uint64_t)m;
    int64_t bm=base.u.i%(int64_t)mm; if(bm<0) bm+=(int64_t)mm;
    uint64_t b=(uint64_t)bm, r=mm==1?0:1;
    if(e<0){                                    /* the inverse of base, then the positive power (CPython 3.8+) */
        int64_t r0=(int64_t)mm, r1=(int64_t)b, t0=0, t1=1;
        while(r1){ int64_t q=r0/r1, t; t=r0-q*r1; r0=r1; r1=t; t=t0-q*t1; t0=t1; t1=t; }
        if(r0!=1) mp_raise_t(E_ValueError,"base is not invertible for the given modulus");
        if(t0<0) t0+=(int64_t)mm;
        b=(uint64_t)t0; e=e==INT64_MIN?INT64_MAX:-e;
    }
    while(e){ if(e&1) r=mulmod64(r,b,mm); b=mulmod64(b,b,mm); e>>=1; }
    int64_t res=(int64_t)r; if(m<0 && res) res+=m;
    return v_int(res); }
static double round_half_even(double x){ double r=floor(x+0.5); if(r-x==0.5 && fmod(r,2)!=0) r-=1; return r; }
BI(round){
    int np=npos(argc,kw);
    Value x= np>0 ? argv[0] : kwarg(argc,argv,kw,"number",v_undef());
    Value nd= np>1 ? argv[1] : kwarg(argc,argv,kw,"ndigits",v_none());
    if(x.k==V_UNDEF) mp_raise_t(E_TypeError,"round() missing required argument 'number' (pos 1)");
    if(TYPE(x)->flags&TF_DUNDERS){ Value m=mp_type_lookup_s(TYPE(x),"__round__"); if(m.k!=V_UNDEF){ if(IS_NONE(nd)) return mp_call1(m,x); return mp_call2(m,x,nd); } }
    if(IS_INTLIKE(x)){
        if(IS_NONE(nd)) return v_int(x.u.i);
        int64_t n=mp_index(nd,"round"); if(n>=0) return v_int(x.u.i);
        int64_t p=1; for(int64_t i=0;i<-n && p<=INT64_MAX/10;i++) p*=10;
        if(-n>18) return v_int(0);
        int64_t q=x.u.i/p, r=x.u.i%p; if(r<0){ r+=p; q--; }
        if(r*2>p || (r*2==p && (q&1))) q++;
        return v_int(q*p);
    }
    if(x.k!=V_FLOAT) mp_raise_t(E_TypeError,"type %s doesn't define __round__ method",mp_type_name(x));
    double f=x.u.f;
    if(IS_NONE(nd)){
        if(isnan(f)) mp_raise_t(E_ValueError,"cannot convert float NaN to integer");
        if(isinf(f)) mp_raise_t(E_OverflowError,"cannot convert float infinity to integer");
        double r=round_half_even(f);
        if(r>=9.2233720368547758e18 || r<-9.2233720368547758e18) mp_raise_t(E_OverflowError,"int too large (ints are 64-bit)");
        return v_int((int64_t)r);
    }
    int64_t n=mp_index(nd,"round");
    if(!isfinite(f) || f==0) return x;
    if(n>300) return x;
    if(n<-320) return v_float(0.0*f);
    char buf[512];
    if(n>=0){ snprintf(buf,sizeof buf,"%.*f",(int)n,f); return v_float(strtod(buf,NULL)); }
    double p=pow(10.0,(double)-n), y=f/p, r=round_half_even(y);
    return v_float(r*p);
}
BI(format){ no_kw("format",kw); nargs("format",argc,1,2); Value spec= argc>1 ? argv[1] : mp_str(""); if(!IS_STR(spec)) mp_raise_t(E_TypeError,"format() argument 2 must be str, not %s",mp_type_name(spec)); return mp_format(argv[0],spec); }
int mp_print_to(SBuf *b, int argc, Value *argv, Value sep, Value end){
    for(int i=0;i<argc;i++){
        if(i){ if(IS_NONE(sep)) sb_putc(b,' '); else sb_put(b,AS_STR(sep)->s,AS_STR(sep)->len); }
        Value s=mp_tostr(argv[i]); sb_put(b,AS_STR(s)->s,AS_STR(s)->len);
    }
    if(IS_NONE(end)) sb_putc(b,'\n'); else sb_put(b,AS_STR(end)->s,AS_STR(end)->len);
    return 0;
}
static Value sys_stdout, sys_stderr;
BI(print){
    static const char *ok[]={"sep","end","file","flush",NULL}; kw_only("print",kw,ok);
    int np=npos(argc,kw);
    Value sep=kwarg(argc,argv,kw,"sep",v_none()), end=kwarg(argc,argv,kw,"end",v_none()), file=kwarg(argc,argv,kw,"file",v_none());
    if(!IS_NONE(sep) && !IS_STR(sep)) mp_raise_t(E_TypeError,"sep must be None or a string, not %s",mp_type_name(sep));
    if(!IS_NONE(end) && !IS_STR(end)) mp_raise_t(E_TypeError,"end must be None or a string, not %s",mp_type_name(end));
    SBuf b={0};
    Catch c;
    if(!CATCH_BEGIN(c)){ mp_print_to(&b,np,argv,sep,end); CATCH_END(c); }
    else { free(b.s); mp_raise(mp_catch_exc(&c)); }
    if(IS_NONE(file) || (file.k==V_OBJ && sys_stdout.k==V_OBJ && file.u.o==sys_stdout.u.o)){ mp_write_out(b.s?b.s:"",b.n); free(b.s); }
    else if(file.k==V_OBJ && sys_stderr.k==V_OBJ && file.u.o==sys_stderr.u.o){ mp_write_err(b.s?b.s:"",b.n); free(b.s); }
    else { Value s=sb_value(&b); mp_callmethod(file,"write",1,&s); }
    if(mp_truth(kwarg(argc,argv,kw,"flush",v_bool(0)))) mp_flush_stdout();
    return v_none();
}
BI(input){
    no_kw("input",kw); nargs("input",argc,0,1);
    if(argc){ Value s=mp_tostr(argv[0]); mp_write_out(AS_STR(s)->s,AS_STR(s)->len); }
    mp_flush_stdout();
#if defined(MPY_KOLIBRI)
    char buf[1024]; kol_console_gets(buf,(int)sizeof buf);
    size_t n=strlen(buf); while(n && (buf[n-1]=='\n'||buf[n-1]=='\r')) buf[--n]=0;
    return mp_strn(buf,(int64_t)n);
#else
    SBuf b={0}; int ch; int any=0;
    Thread *self=mp_ts; mp_gil_release();
    while((ch=fgetc(stdin))!=EOF){ any=1; if(ch=='\n') break; sb_putc(&b,(char)ch); }
    mp_gil_acquire(self);
    if(!any){ free(b.s); mp_raise_t(E_EOFError,"EOF when reading a line"); }
    if(b.n && b.s[b.n-1]=='\r') b.n--;
    return sb_value(&b);
#endif
}
BI(vars){ no_kw("vars",kw); nargs("vars",argc,0,1);
    if(!argc){ Frame *f=mp_ts->frame; if(f && f->locals) return v_obj(f->locals); return v_obj(f?f->globals:mp_builtins); }
    Value d; if(mp_getattr_opt(argv[0],mp_intern("__dict__"),&d)) return d;
    mp_raise_t(E_TypeError,"vars() argument must have __dict__ attribute"); }
BI(globals){ (void)argc; (void)argv; no_kw("globals",kw); Frame *f=mp_ts->frame; return v_obj(f?f->globals:mp_builtins); }
BI(locals){ (void)argc; (void)argv; no_kw("locals",kw); Frame *f=mp_ts->frame;
    if(!f) return v_obj(mp_builtins);
    if(f->locals) return v_obj(f->locals);
    if(f->code->flags&CO_MODULE) return v_obj(f->globals);
    Value d=mp_dict();
    for(int i=0;i<f->code->nlocals;i++) if(f->fast[i].k!=V_UNDEF && f->code->varnames[i]->s[0]!='.') mp_dict_set(AS_DICT(d),v_obj(f->code->varnames[i]),f->fast[i]);
    for(int i=0;i<f->code->ncells+f->code->nfrees;i++){ Value v=((CellObj*)f->cells[i].u.o)->v; if(v.k!=V_UNDEF) mp_dict_set(AS_DICT(d),v_obj(i<f->code->ncells?f->code->cellnames[i]:f->code->freenames[i-f->code->ncells]),v); }
    return d; }
static void dir_add(Value set, DictObj *d){ if(!d) return; int64_t pos=0; Value k; while(mp_dict_next(d,&pos,&k,NULL)) if(IS_STR(k)) mp_set_add((SetObj*)set.u.o,k); }
BI(dir){ no_kw("dir",kw); nargs("dir",argc,0,1);
    Value s=mp_set(T_set);
    if(!argc){ Value l=bi_locals(0,NULL,NULL); dir_add(s,AS_DICT(l)); }
    else {
        Value x=argv[0]; Type *t=TYPE(x);
        if(t->layout==LY_TYPE){ Type *c=AS_TYPE(x); for(int i=0;i<c->nmro;i++) dir_add(s,c->mro[i]->dict); }
        else { Value d; if(mp_getattr_opt(x,mp_intern("__dict__"),&d) && IS(d,T_dict)) dir_add(s,AS_DICT(d)); for(int i=0;i<t->nmro;i++) dir_add(s,t->mro[i]->dict); }
    }
    Value l=mp_list_of(s); mp_sort(l,v_none(),0); return l; }
BI(open){
    int np=npos(argc,kw);
    Value path= np>0 ? argv[0] : kwarg(argc,argv,kw,"file",v_undef());
    Value mode= np>1 ? argv[1] : kwarg(argc,argv,kw,"mode",mp_str("r"));
    if(path.k==V_UNDEF) mp_raise_t(E_TypeError,"open() missing required argument 'file' (pos 1)");
    return mp_open(path,mode);
}
BI(exec_eval_unsupported){ (void)argc; (void)argv; (void)kw; mp_raise_t(E_NotImplementedError,"not available"); }
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);
BI(typealias){ (void)kw;                 /* type X = ...: typing.TypeAliasType(name, value function, type parameter names) */
    Value name=mp_str("typing"); Value mod=mp_builtin_import(1,&name,NULL);
    return mp_call(mp_getattr_s(mod,"TypeAliasType"),argc,argv,NULL); }
BI(build_class){ (void)argc; (void)argv; (void)kw; mp_raise_t(E_NotImplementedError,"__build_class__"); }

/* ---------------------------------------------------------------- files */
Value mp_open(Value path, Value modev){
    if(!IS_STR(path)) mp_raise_t(E_TypeError,"expected str, bytes or os.PathLike object, not %s",mp_type_name(path));
    const char *m=mp_cstr(modev); int mode=0, binary=0, plus=0;
    for(const char *q=m;*q;q++){
        if(*q=='r'||*q=='w'||*q=='a'||*q=='x'){ if(mode) mp_raise_t(E_ValueError,"must have exactly one of create/read/write/append mode"); mode= *q=='x'?'w':*q; }
        else if(*q=='b') binary=1;
        else if(*q=='t'){}
        else if(*q=='+') plus=1;
        else mp_raise_t(E_ValueError,"invalid mode: '%s'",m);
    }
    if(!mode) mode='r';
    (void)plus;
    FileObj *f=(FileObj*)mp_alloc(T_file,sizeof(FileObj));
    f->name=path; f->mode=mode; f->binary=binary;
    const char *p=mp_cstr(path);
    if(mode=='r' || mode=='a'){
        char *err=NULL;
        mpy_fs_last_len=0;
        char *data=mpy_fs_try_read_file(p,&err);
        if(!data){
            free(err);
            if(mode=='r'){ Value args[3]={v_int(2),mp_str("No such file or directory"),path}; mp_raise(mp_exc_args(E_FileNotFoundError,mp_tuple(3,args))); }
        } else {
            size_t n=mpy_fs_last_len ? mpy_fs_last_len : strlen(data);
            f->buf=data; f->len=(int64_t)n; f->cap=(int64_t)n+1;
            if(!binary && f->len>=3 && (unsigned char)data[0]==0xEF && (unsigned char)data[1]==0xBB && (unsigned char)data[2]==0xBF && 0){}
        }
        if(mode=='a') f->pos=f->len;
    }
    if(mode=='w'){
        char *err=NULL;
        if(mpy_fs_write_file(p,"",0,&err)){ free(err); Value args[3]={v_int(2),mp_str("No such file or directory"),path}; mp_raise(mp_exc_args(E_FileNotFoundError,mp_tuple(3,args))); }
    }
    if(!f->buf){ f->buf=(char*)xmalloc(64); f->cap=64; }
    return v_obj(f);
}
int mp_file_flush(FileObj *f){
    if(f->mode=='r') return 0;
    char *err=NULL;
    if(mpy_fs_write_file(mp_cstr(f->name),f->buf?f->buf:"",(size_t)f->len,&err)){ free(err); mp_raise_t(E_OSError,"cannot write %s",mp_cstr(f->name)); }
    return 0;
}

/* ---------------------------------------------------------------- sys.stdout / stderr objects */
static Value so_write(int argc, Value *argv, TupleObj *kw){ (void)kw; if(argc<2||!IS_STR(argv[1])) mp_raise_t(E_TypeError,"write() argument must be str"); int err=argv[0].u.o==sys_stderr.u.o;
    if(err) mp_write_err(AS_STR(argv[1])->s,AS_STR(argv[1])->len); else mp_write_out(AS_STR(argv[1])->s,AS_STR(argv[1])->len); return v_int(AS_STR(argv[1])->cplen); }
static Value so_flush(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; mp_flush_stdout(); return v_none(); }
static Value so_isatty(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw;
#if defined(MPY_KOLIBRI)
    return v_bool(1);
#else
    return v_bool(isatty(argv[0].u.o==sys_stderr.u.o?2:1));
#endif
}
Value mp_std_stream(int err){ return err?sys_stderr:sys_stdout; }

/* ---------------------------------------------------------------- start */
static Type *exc_type(const char *name, Type *base){
    Value t=mp_new_type(name,base,LY_EXC,TF_BASETYPE);
    mp_dict_set_s(mp_builtins,name,t);
    return AS_TYPE(t);
}
static Type *btype(const char *name, Type *base, Layout l, int flags, Value (*make)(Type*,int,Value*,TupleObj*)){
    Value t=mp_new_type(name,base,l,flags);
    AS_TYPE(t)->make=make;
    return AS_TYPE(t);
}
static void reg(const char *name, NFn fn){ mp_dict_set_s(mp_builtins,name,mp_native(name,fn)); }
static Value builtins_v, modules_v, stdout_keep, stderr_keep;
void mp_init(void){
    /* the first types by hand: type, object, str, dict (interning needs them) */
    T_type=(Type*)mp_alloc(NULL,sizeof(Type)); T_type->h.type=T_type; T_type->layout=LY_TYPE; T_type->flags=TF_BUILTIN|TF_BASETYPE;
    T_object=(Type*)mp_alloc(T_type,sizeof(Type)); T_object->layout=LY_OBJECT; T_object->flags=TF_BUILTIN|TF_BASETYPE;
    T_str=(Type*)mp_alloc(T_type,sizeof(Type)); T_str->layout=LY_STR; T_str->flags=TF_BUILTIN;
    T_dict=(Type*)mp_alloc(T_type,sizeof(Type)); T_dict->layout=LY_DICT; T_dict->flags=TF_BUILTIN;
    T_tuple=(Type*)mp_alloc(T_type,sizeof(Type)); T_tuple->layout=LY_TUPLE; T_tuple->flags=TF_BUILTIN;
    Type *boot[5]={T_type,T_object,T_str,T_dict,T_tuple};
    const char *names[5]={"type","object","str","dict","tuple"};
    for(int i=0;i<5;i++){ boot[i]->mro=(Type**)xmalloc(sizeof(Type*)*2); }
    for(int i=0;i<5;i++){ Type *t=boot[i]; t->dict=AS_DICT(mp_dict()); }
    for(int i=0;i<5;i++){ Type *t=boot[i]; t->name=AS_STR(mp_intern(names[i])); t->qualname=t->name; t->base= t==T_object?NULL:T_object;
        if(t==T_object){ t->bases=AS_TUPLE(mp_tuple(0,NULL)); t->mro[0]=t; t->nmro=1; }
        else { Value ob=v_obj(T_object); t->bases=AS_TUPLE(mp_tuple(1,&ob)); t->mro[0]=t; t->mro[1]=T_object; t->nmro=2; } }
    T_type->make=make_type; T_object->make=make_object; T_str->make=make_str; T_dict->make=make_dict; T_tuple->make=make_tuple;
    builtins_v=mp_dict(); mp_builtins=AS_DICT(builtins_v); mp_gc_add_root(&builtins_v);
    modules_v=mp_dict(); mp_modules=AS_DICT(modules_v); mp_gc_add_root(&modules_v);
    T_none=btype("NoneType",T_object,LY_OBJECT,0,NULL);
    T_int=btype("int",T_object,LY_OBJECT,TF_BASETYPE,make_int);
    T_bool=btype("bool",T_int,LY_OBJECT,0,make_bool);
    T_float=btype("float",T_object,LY_OBJECT,TF_BASETYPE,make_float);
    T_complex=btype("complex",T_object,LY_COMPLEX,0,make_complex);
    T_bytes=btype("bytes",T_object,LY_BYTES,0,make_bytes);
    T_list=btype("list",T_object,LY_LIST,0,make_list);
    T_set=btype("set",T_object,LY_SET,0,make_set);
    T_frozenset=btype("frozenset",T_object,LY_SET,0,make_set);
    T_range=btype("range",T_object,LY_RANGE,0,make_range);
    T_slice=btype("slice",T_object,LY_SLICE,0,make_slice);
    T_function=btype("function",T_object,LY_FUNC,0,NULL);
    T_native=btype("builtin_function_or_method",T_object,LY_NATIVE,0,NULL);
    T_method=btype("method",T_object,LY_METHOD,0,NULL);
    T_module=btype("module",T_object,LY_MODULE,0,NULL);
    T_code=btype("code",T_object,LY_CODE,0,NULL);
    T_cell=btype("cell",T_object,LY_CELL,0,NULL);
    T_generator=btype("generator",T_object,LY_GEN,0,NULL);
    T_coroutine=btype("coroutine",T_object,LY_GEN,0,NULL);
    T_asyncgen=btype("async_generator",T_object,LY_GEN,0,NULL);
    T_property=btype("property",T_object,LY_PROPERTY,0,make_property);
    T_staticmethod=btype("staticmethod",T_object,LY_STATICMETHOD,0,make_box);
    T_classmethod=btype("classmethod",T_object,LY_CLASSMETHOD,0,make_box);
    T_super=btype("super",T_object,LY_SUPER,0,make_super);
    T_iter=btype("iterator",T_object,LY_ITER,0,NULL);
    T_file=btype("TextIOWrapper",T_object,LY_FILE,0,NULL);
    T_buffer=btype("buffer",T_object,LY_BUFFER,0,NULL);
    T_notimpl=btype("NotImplementedType",T_object,LY_OBJECT,0,NULL);
    T_ellipsis=btype("ellipsis",T_object,LY_OBJECT,0,NULL);
    T_dict_keys=btype("dict_keys",T_object,LY_OBJECT,0,NULL);
    T_dict_values=btype("dict_values",T_object,LY_OBJECT,0,NULL);
    T_dict_items=btype("dict_items",T_object,LY_OBJECT,0,NULL);
    T_box=btype("box",T_object,LY_BOX,0,NULL);
    mp_NotImplemented=v_obj(mp_alloc(T_notimpl,sizeof(Obj)));
    mp_Ellipsis=v_obj(mp_alloc(T_ellipsis,sizeof(Obj)));
    /* exceptions */
    E_BaseException=exc_type("BaseException",T_object);
    E_BaseExceptionGroup=exc_type("BaseExceptionGroup",E_BaseException);
    E_GeneratorExit=exc_type("GeneratorExit",E_BaseException);
    E_KeyboardInterrupt=exc_type("KeyboardInterrupt",E_BaseException);
    E_SystemExit=exc_type("SystemExit",E_BaseException);
    E_Exception=exc_type("Exception",E_BaseException);
    E_ArithmeticError=exc_type("ArithmeticError",E_Exception);
    exc_type("FloatingPointError",E_ArithmeticError);
    E_OverflowError=exc_type("OverflowError",E_ArithmeticError);
    E_ZeroDivisionError=exc_type("ZeroDivisionError",E_ArithmeticError);
    E_AssertionError=exc_type("AssertionError",E_Exception);
    E_AttributeError=exc_type("AttributeError",E_Exception);
    exc_type("BufferError",E_Exception);
    E_EOFError=exc_type("EOFError",E_Exception);
    E_ExceptionGroup=exc_type("ExceptionGroup",E_BaseExceptionGroup);
    { Value b2[2]={v_obj(E_BaseExceptionGroup),v_obj(E_Exception)}; E_ExceptionGroup->bases=AS_TUPLE(mp_tuple(2,b2)); mp_type_finish(E_ExceptionGroup); }
    E_ImportError=exc_type("ImportError",E_Exception);
    E_ModuleNotFoundError=exc_type("ModuleNotFoundError",E_ImportError);
    E_LookupError=exc_type("LookupError",E_Exception);
    E_IndexError=exc_type("IndexError",E_LookupError);
    E_KeyError=exc_type("KeyError",E_LookupError);
    E_MemoryError=exc_type("MemoryError",E_Exception);
    E_NameError=exc_type("NameError",E_Exception);
    E_UnboundLocalError=exc_type("UnboundLocalError",E_NameError);
    E_OSError=exc_type("OSError",E_Exception);
    mp_dict_set_s(mp_builtins,"IOError",v_obj(E_OSError)); mp_dict_set_s(mp_builtins,"EnvironmentError",v_obj(E_OSError));
    Type *conn=exc_type("ConnectionError",E_OSError);
    exc_type("BrokenPipeError",conn); exc_type("ConnectionAbortedError",conn); exc_type("ConnectionRefusedError",conn); exc_type("ConnectionResetError",conn);
    exc_type("BlockingIOError",E_OSError); exc_type("ChildProcessError",E_OSError); exc_type("FileExistsError",E_OSError);
    E_FileNotFoundError=exc_type("FileNotFoundError",E_OSError);
    exc_type("InterruptedError",E_OSError); exc_type("IsADirectoryError",E_OSError); exc_type("NotADirectoryError",E_OSError);
    exc_type("PermissionError",E_OSError); exc_type("ProcessLookupError",E_OSError); exc_type("TimeoutError",E_OSError);
    exc_type("ReferenceError",E_Exception);
    E_RuntimeError=exc_type("RuntimeError",E_Exception);
    E_NotImplementedError=exc_type("NotImplementedError",E_RuntimeError);
    E_RecursionError=exc_type("RecursionError",E_RuntimeError);
    exc_type("PythonFinalizationError",E_RuntimeError);
    E_StopAsyncIteration=exc_type("StopAsyncIteration",E_Exception);
    E_StopIteration=exc_type("StopIteration",E_Exception);
    E_SyntaxError=exc_type("SyntaxError",E_Exception);
    Type *ind=exc_type("IndentationError",E_SyntaxError); exc_type("TabError",ind);
    E_SystemError=exc_type("SystemError",E_Exception);
    E_TypeError=exc_type("TypeError",E_Exception);
    E_ValueError=exc_type("ValueError",E_Exception);
    E_UnicodeError=exc_type("UnicodeError",E_ValueError);
    E_UnicodeDecodeError=exc_type("UnicodeDecodeError",E_UnicodeError);
    E_UnicodeEncodeError=exc_type("UnicodeEncodeError",E_UnicodeError); exc_type("UnicodeTranslateError",E_UnicodeError);
    Type *w=exc_type("Warning",E_Exception);
    const char *ws[]={"BytesWarning","DeprecationWarning","EncodingWarning","FutureWarning","ImportWarning","PendingDeprecationWarning","ResourceWarning","RuntimeWarning","SyntaxWarning","UnicodeWarning","UserWarning",NULL};
    for(int i=0;ws[i];i++) exc_type(ws[i],w);
    /* the builtins module */
    Type *pub[]={T_object,T_type,T_int,T_bool,T_float,T_complex,T_str,T_bytes,T_list,T_tuple,T_dict,T_set,T_frozenset,T_range,T_slice,T_property,T_staticmethod,T_classmethod,T_super,NULL};
    for(int i=0;pub[i];i++) mp_dict_set(mp_builtins,v_obj(pub[i]->name),v_obj(pub[i]));
    mp_dict_set_s(mp_builtins,"None",v_none()); mp_dict_set_s(mp_builtins,"True",v_bool(1)); mp_dict_set_s(mp_builtins,"False",v_bool(0));
    mp_dict_set_s(mp_builtins,"NotImplemented",mp_NotImplemented); mp_dict_set_s(mp_builtins,"Ellipsis",mp_Ellipsis);
    mp_dict_set_s(mp_builtins,"__debug__",v_bool(1)); mp_dict_set_s(mp_builtins,"__name__",mp_str("builtins"));
    reg("len",bi_len); reg("repr",bi_repr); reg("ascii",bi_ascii); reg("abs",bi_abs); reg("min",bi_min); reg("max",bi_max); reg("sum",bi_sum);
    reg("sorted",bi_sorted); reg("any",bi_any); reg("all",bi_all); reg("enumerate",bi_enumerate); reg("zip",bi_zip); reg("map",bi_map);
    reg("filter",bi_filter); reg("reversed",bi_reversed); reg("iter",bi_iter); reg("next",bi_next); reg("anext",bi_anext); reg("__mpy_tstr__",bi_mpy_tstr); reg("aiter",bi_aiter); reg("isinstance",bi_isinstance);
    reg("issubclass",bi_issubclass); reg("hasattr",bi_hasattr); reg("getattr",bi_getattr); reg("setattr",bi_setattr); reg("delattr",bi_delattr);
    reg("callable",bi_callable); reg("hash",bi_hash); reg("id",bi_id); reg("chr",bi_chr); reg("ord",bi_ord); reg("hex",bi_hex); reg("oct",bi_oct);
    reg("bin",bi_bin); reg("divmod",bi_divmod); reg("pow",bi_pow); reg("round",bi_round); reg("format",bi_format); reg("print",bi_print);
    reg("input",bi_input); reg("vars",bi_vars); reg("globals",bi_globals); reg("locals",bi_locals); reg("dir",bi_dir); reg("open",bi_open);
    reg("__import__",mp_builtin_import); reg("__typealias__",bi_typealias);
    (void)bi_exec_eval_unsupported; (void)bi_build_class;
    mp_methods_init();
    /* sys.stdout / sys.stderr */
    Value so=mp_new_type("TextIO",T_object,LY_INSTANCE,0);
    mp_type_add(AS_TYPE(so),"write",so_write); mp_type_add(AS_TYPE(so),"flush",so_flush); mp_type_add(AS_TYPE(so),"isatty",so_isatty);
    sys_stdout=mp_instance(AS_TYPE(so)); sys_stderr=mp_instance(AS_TYPE(so));
    stdout_keep=sys_stdout; stderr_keep=sys_stderr; mp_gc_add_root(&stdout_keep); mp_gc_add_root(&stderr_keep);
    mp_modules_init();
}
