/* ========================= Interpreter: operations =========================
   What the bytecode does to values, as CPython does it: repr / str, ==, order,
   truth, arithmetic (int64: OverflowError instead of growing), item access
   with slices, `in`, len, iteration, sorting. Instances get their dunder
   methods (__add__ / __radd__, __eq__, __getitem__, __iter__, ...). */

#include "interp.h"
#include <math.h>

Value mp_set_copy(SetObj *s, Type *t);
void  mp_set_merge(SetObj *s, SetObj *o);
int   mp_set_next(SetObj *s, int64_t *pos, Value *key);
void  mp_set_add_hashed(SetObj *s, Value v, uint64_t h);
void  mp_set_diff_update(SetObj *s, Value other);
void  mp_set_xor_update(SetObj *s, Value other);
int   mp_set_has_hashed(SetObj *s, Value v, uint64_t h);
Value mp_list_slice(Value l, int64_t start, int64_t stop, int64_t step);

static int is_set(Value v){ return v.k==V_OBJ && v.u.o->type->layout==LY_SET; }
static int user(Value v){ return v.k==V_OBJ && (v.u.o->type->flags&TF_DUNDERS); }
const char *mp_type_name(Value v){ return TYPE(v)->name->s; }

/* ---------------------------------------------------------------- repr / str */
static Obj *repr_stack[256]; static int repr_depth;
static int repr_enter(Obj *o){ for(int i=0;i<repr_depth;i++) if(repr_stack[i]==o) return 0; if(repr_depth<256) repr_stack[repr_depth++]=o; return 1; }
static void repr_leave(void){ if(repr_depth) repr_depth--; }

int mp_cp_printable(uint32_t c);
void mp_str_repr_into(SBuf *b, StrObj *s){
    int sq=0, dq=0;
    for(int64_t i=0;i<s->len;i++){ if(s->s[i]=='\'') sq=1; else if(s->s[i]=='"') dq=1; }
    char q= sq && !dq ? '"' : '\'';
    sb_putc(b,q);
    int64_t p=0;
    while(p<s->len){
        int64_t st=p; uint32_t c=(uint32_t)mp_utf8_decode(s->s,s->len,&p);
        if(c=='\\') sb_puts(b,"\\\\");
        else if(c==(uint32_t)q){ sb_putc(b,'\\'); sb_putc(b,q); }
        else if(c=='\n') sb_puts(b,"\\n");
        else if(c=='\r') sb_puts(b,"\\r");
        else if(c=='\t') sb_puts(b,"\\t");
        else if(!mp_cp_printable(c)){
            if(c<0x100) sb_printf(b,"\\x%02x",c);
            else if(c<0x10000) sb_printf(b,"\\u%04x",c);
            else sb_printf(b,"\\U%08x",c);
        } else sb_put(b,s->s+st,p-st);
    }
    sb_putc(b,q);
}
static void bytes_repr_into(SBuf *b, const unsigned char *s, int64_t n){
    int sq=0, dq=0;
    for(int64_t i=0;i<n;i++){ if(s[i]=='\'') sq=1; else if(s[i]=='"') dq=1; }
    char q= sq && !dq ? '"' : '\'';
    sb_putc(b,'b'); sb_putc(b,q);
    for(int64_t i=0;i<n;i++){ unsigned c=s[i];
        if(c=='\\') sb_puts(b,"\\\\");
        else if(c==(unsigned)q){ sb_putc(b,'\\'); sb_putc(b,q); }
        else if(c=='\n') sb_puts(b,"\\n");
        else if(c=='\r') sb_puts(b,"\\r");
        else if(c=='\t') sb_puts(b,"\\t");
        else if(c<0x20 || c>=0x7f) sb_printf(b,"\\x%02x",c);
        else sb_putc(b,(char)c);
    }
    sb_putc(b,q);
}
static void float_into(SBuf *b, double f){ char t[64]; mp_float_repr(t,sizeof t,f); sb_puts(b,t); }
static void complex_part(SBuf *b, double f){
    char t[64]; mp_float_repr(t,sizeof t,f);
    size_t n=strlen(t); if(n>2 && !strcmp(t+n-2,".0")) t[n-2]=0;
    sb_puts(b,t);
}
static void repr_into(SBuf *b, Value v);
static const char *module_of(Type *t){
    Value m; if(t->dict && mp_dict_get_s(t->dict,"__module__",&m) && IS_STR(m)) return mp_cstr(m);
    return "builtins";
}
static void type_repr_into(SBuf *b, Type *t){
    const char *m=module_of(t);
    if(!strcmp(m,"builtins")) sb_printf(b,"<class '%s'>",t->qualname?t->qualname->s:t->name->s);
    else sb_printf(b,"<class '%s.%s'>",m,t->qualname?t->qualname->s:t->name->s);
}
static void default_repr_into(SBuf *b, Value v){
    Type *t=TYPE(v); const char *m=module_of(t);
    if(!strcmp(m,"builtins")) sb_printf(b,"<%s object at %p>",t->qualname?t->qualname->s:t->name->s,(void*)v.u.o);
    else sb_printf(b,"<%s.%s object at %p>",m,t->qualname?t->qualname->s:t->name->s,(void*)v.u.o);
}
static void seq_into(SBuf *b, const Value *items, int64_t n){
    for(int64_t i=0;i<n;i++){ if(i) sb_puts(b,", "); repr_into(b,items[i]); }
}
static void set_into(SBuf *b, SetObj *s){
    int64_t pos=0; Value k; int first=1;
    while(mp_set_next(s,&pos,&k)){ if(!first) sb_puts(b,", "); first=0; repr_into(b,k); }
}
static void repr_into(SBuf *b, Value v){
    switch(v.k){
        case V_NONE: sb_puts(b,"None"); return;
        case V_BOOL: sb_puts(b,v.u.i?"True":"False"); return;
        case V_INT: sb_printf(b,"%lld",(long long)v.u.i); return;
        case V_FLOAT: float_into(b,v.u.f); return;
        case V_UNDEF: sb_puts(b,"<undefined>"); return;
        default: break;
    }
    Obj *o=v.u.o; Type *t=o->type;
    if((t->flags&TF_DUNDERS)){
        Value r=mp_type_lookup_s(t,"__repr__");
        if(r.k!=V_UNDEF && !IS(r,T_native)){
            Value s=mp_call1(r,v);
            if(!IS_STR(s)) mp_raise_t(E_TypeError,"__repr__ returned non-string (type %s)",mp_type_name(s));
            sb_put(b,AS_STR(s)->s,AS_STR(s)->len); return;
        }
    }
    switch(t->layout){
        case LY_STR: mp_str_repr_into(b,(StrObj*)o); return;
        case LY_BYTES: bytes_repr_into(b,((BytesObj*)o)->s,((BytesObj*)o)->len); return;
        case LY_TUPLE:{ TupleObj *tp=(TupleObj*)o;
            if(!repr_enter(o)){ sb_puts(b,"(...)"); return; }
            sb_putc(b,'('); seq_into(b,tp->items,tp->len); if(tp->len==1) sb_putc(b,','); sb_putc(b,')'); repr_leave(); return; }
        case LY_LIST:{ ListObj *l=(ListObj*)o;
            if(!repr_enter(o)){ sb_puts(b,"[...]"); return; }
            sb_putc(b,'[');
            for(int64_t i=0;i<l->len;i++){ if(i) sb_puts(b,", "); repr_into(b,l->items[i]); }
            sb_putc(b,']'); repr_leave(); return; }
        case LY_DICT:{ DictObj *d=(DictObj*)o;
            if(!repr_enter(o)){ sb_puts(b,"{...}"); return; }
            int64_t pos=0; Value k, val; int first=1;
            sb_putc(b,'{');
            while(mp_dict_next(d,&pos,&k,&val)){ if(!first) sb_puts(b,", "); first=0; repr_into(b,k); sb_puts(b,": "); repr_into(b,val); }
            sb_putc(b,'}'); repr_leave(); return; }
        case LY_SET:{ SetObj *s=(SetObj*)o; int fz=t==T_frozenset;
            if(!s->used){ sb_puts(b,fz?"frozenset()":"set()"); return; }
            if(!repr_enter(o)){ sb_puts(b,"{...}"); return; }
            if(fz) sb_puts(b,"frozenset(");
            sb_putc(b,'{'); set_into(b,s); sb_putc(b,'}');
            if(fz) sb_putc(b,')');
            repr_leave(); return; }
        case LY_RANGE:{ RangeObj *r=(RangeObj*)o;
            if(r->step==1) sb_printf(b,"range(%lld, %lld)",(long long)r->start,(long long)r->stop);
            else sb_printf(b,"range(%lld, %lld, %lld)",(long long)r->start,(long long)r->stop,(long long)r->step);
            return; }
        case LY_SLICE:{ SliceObj *s=(SliceObj*)o; sb_puts(b,"slice("); repr_into(b,s->start); sb_puts(b,", "); repr_into(b,s->stop); sb_puts(b,", "); repr_into(b,s->step); sb_putc(b,')'); return; }
        case LY_COMPLEX:{ ComplexObj *c=(ComplexObj*)o;
            if(c->re==0 && !signbit(c->re)){ complex_part(b,c->im); sb_putc(b,'j'); return; }
            sb_putc(b,'('); complex_part(b,c->re);
            if(c->im>=0 || isnan(c->im)) sb_putc(b,'+');
            complex_part(b,c->im); sb_puts(b,"j)"); return; }
        case LY_TYPE: type_repr_into(b,(Type*)o); return;
        case LY_FUNC: sb_printf(b,"<function %s at %p>",((FuncObj*)o)->qualname->s,(void*)o); return;
        case LY_NATIVE:{ NativeObj *n=(NativeObj*)o;
            if(n->self.k!=V_UNDEF) sb_printf(b,"<built-in method %s of %s object at %p>",n->name,mp_type_name(n->self),(void*)n->self.u.o);
            else sb_printf(b,"<built-in function %s>",n->name);
            return; }
        case LY_METHOD:{ MethodObj *m=(MethodObj*)o;
            sb_puts(b,"<bound method ");
            if(IS(m->func,T_function)) sb_puts(b,AS_FUNC(m->func)->qualname->s); else sb_puts(b,"?");
            sb_puts(b," of "); repr_into(b,m->self); sb_putc(b,'>'); return; }
        case LY_MODULE: sb_printf(b,"<module '%s'>",((ModuleObj*)o)->name->s); return;
        case LY_CELL: sb_printf(b,"<cell at %p>",(void*)o); return;
        case LY_GEN:{ GenObj *g=(GenObj*)o; sb_printf(b,"<%s object %s at %p>",g->kind==G_CORO?"coroutine":g->kind==G_ASYNCGEN?"async_generator":"generator",g->qualname?g->qualname->s:"?",(void*)o); return; }
        case LY_EXC:{ ExcObj *e=(ExcObj*)o;
            sb_puts(b,t->name->s); sb_putc(b,'(');
            if(IS(e->args,T_tuple)) seq_into(b,AS_TUPLE(e->args)->items,AS_TUPLE(e->args)->len);
            sb_putc(b,')'); return; }
        case LY_CODE: sb_printf(b,"<code object %s at %p>",((CodeObj*)o)->name->s,(void*)o); return;
        case LY_FILE:{ FileObj *f=(FileObj*)o; sb_puts(b,"<_io.TextIOWrapper name="); repr_into(b,f->name); sb_printf(b," mode='%c'>",f->mode); return; }
        default: break;
    }
    if(t==T_none){ sb_puts(b,"None"); return; }
    if(v.u.o==mp_NotImplemented.u.o){ sb_puts(b,"NotImplemented"); return; }
    if(v.u.o==mp_Ellipsis.u.o){ sb_puts(b,"Ellipsis"); return; }
    if(t==T_dict_keys || t==T_dict_values || t==T_dict_items){
        IterObj *it=(IterObj*)o; DictObj *d=AS_DICT(it->src);
        sb_puts(b,t==T_dict_keys?"dict_keys([":t==T_dict_values?"dict_values([":"dict_items([");
        int64_t pos=0; Value k, val; int first=1;
        while(mp_dict_next(d,&pos,&k,&val)){
            if(!first) sb_puts(b,", "); first=0;
            if(t==T_dict_keys) repr_into(b,k);
            else if(t==T_dict_values) repr_into(b,val);
            else { sb_putc(b,'('); repr_into(b,k); sb_puts(b,", "); repr_into(b,val); sb_putc(b,')'); }
        }
        sb_puts(b,"])"); return;
    }
    default_repr_into(b,v);
}
Value mp_repr(Value v){ SBuf b={0}; repr_into(&b,v); return sb_value(&b); }
void mp_repr_into(SBuf *b, Value v){ repr_into(b,v); }

Value mp_exc_str(Value e){
    ExcObj *x=AS_EXC(e);
    if(!IS(x->args,T_tuple)) return mp_str("");
    TupleObj *a=AS_TUPLE(x->args);
    if(a->len==0) return mp_str("");
    if(a->len>=2 && a->len<=3 && mp_is_subtype(TYPE(e),E_OSError) && IS_INTLIKE(a->items[0])){   /* [Errno 2] text: 'file' */
        SBuf b={0}; Value t=mp_tostr(a->items[1]);
        sb_printf(&b,"[Errno %lld] ",(long long)a->items[0].u.i); sb_put(&b,AS_STR(t)->s,AS_STR(t)->len);
        if(a->len==3){ Value r=mp_repr(a->items[2]); sb_puts(&b,": "); sb_put(&b,AS_STR(r)->s,AS_STR(r)->len); }
        return sb_value(&b);
    }
    if(a->len==1){
        if(mp_is_subtype(TYPE(e),E_KeyError)) return mp_repr(a->items[0]);
        return mp_tostr(a->items[0]);
    }
    return mp_repr(x->args);
}
Value mp_tostr(Value v){
    if(v.k==V_OBJ){
        Type *t=v.u.o->type;
        if(t==T_str) return v;
        if(t->flags&TF_DUNDERS){
            Value s=mp_type_lookup_s(t,"__str__");
            if(s.k!=V_UNDEF && !IS(s,T_native)){
                Value r=mp_call1(s,v);
                if(!IS_STR(r)) mp_raise_t(E_TypeError,"__str__ returned non-string (type %s)",mp_type_name(r));
                return r;
            }
        }
        if(t->layout==LY_EXC) return mp_exc_str(v);
        if(t->layout==LY_STR) return mp_strn(AS_STR(v)->s,AS_STR(v)->len);
    }
    return mp_repr(v);
}

/* ---------------------------------------------------------------- truth */
int mp_truth(Value v){
    switch(v.k){
        case V_NONE: case V_UNDEF: return 0;
        case V_BOOL: case V_INT: return v.u.i!=0;
        case V_FLOAT: return v.u.f!=0;
        default: break;
    }
    Obj *o=v.u.o; Type *t=o->type;
    if(t->flags&TF_DUNDERS){
        Value m=mp_type_lookup_s(t,"__bool__");
        if(m.k!=V_UNDEF){ Value r=mp_call1(m,v); if(r.k!=V_BOOL) mp_raise_t(E_TypeError,"__bool__ should return bool, returned %s",mp_type_name(r)); return (int)r.u.i; }
        m=mp_type_lookup_s(t,"__len__");
        if(m.k!=V_UNDEF){ Value r=mp_call1(m,v); return mp_index(r,"__len__")!=0; }
        return 1;
    }
    switch(t->layout){
        case LY_STR: return AS_STR(v)->len!=0;
        case LY_BYTES: return AS_BYTES(v)->len!=0;
        case LY_TUPLE: return AS_TUPLE(v)->len!=0;
        case LY_LIST: return AS_LIST(v)->len!=0;
        case LY_DICT: return AS_DICT(v)->used!=0;
        case LY_SET: return ((SetObj*)o)->used!=0;
        case LY_RANGE: return mp_len(v)!=0;
        case LY_COMPLEX: return ((ComplexObj*)o)->re!=0 || ((ComplexObj*)o)->im!=0;
        default: break;
    }
    if(t==T_none) return 0;
    if(t==T_dict_keys||t==T_dict_values||t==T_dict_items) return AS_DICT(((IterObj*)o)->src)->used!=0;
    return 1;
}

/* ---------------------------------------------------------------- numbers */
static int is_num(Value v){ return v.k==V_INT || v.k==V_BOOL || v.k==V_FLOAT || IS(v,T_complex); }
double mp_float_of(Value v){
    if(v.k==V_FLOAT) return v.u.f;
    if(v.k==V_INT || v.k==V_BOOL) return (double)v.u.i;
    if(user(v)){ Value m=mp_type_lookup_s(TYPE(v),"__float__"); if(m.k!=V_UNDEF){ Value r=mp_call1(m,v); if(r.k==V_FLOAT) return r.u.f; } }
    mp_raise_t(E_TypeError,"must be real number, not %s",mp_type_name(v));
}
int64_t mp_index(Value v, const char *what){
    if(v.k==V_INT || v.k==V_BOOL) return v.u.i;
    if(user(v)){ Value m=mp_type_lookup_s(TYPE(v),"__index__"); if(m.k!=V_UNDEF){ Value r=mp_call1(m,v); if(IS_INTLIKE(r)) return r.u.i; } }
    (void)what;
    mp_raise_t(E_TypeError,"'%s' object cannot be interpreted as an integer",mp_type_name(v));
}
static int64_t floordiv_i(int64_t a, int64_t b){
    if(b==0) mp_raise_t(E_ZeroDivisionError,"division by zero");
    if(a==INT64_MIN && b==-1) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)");
    int64_t q=a/b; if((a%b!=0) && ((a<0)!=(b<0))) q--; return q;
}
static int64_t mod_i(int64_t a, int64_t b){
    if(b==0) mp_raise_t(E_ZeroDivisionError,"division by zero");
    if(b==-1) return 0;
    int64_t r=a%b; if(r!=0 && ((r<0)!=(b<0))) r+=b; return r;
}
static double floordiv_f(double a, double b){
    if(b==0) mp_raise_t(E_ZeroDivisionError,"division by zero");
    double m=fmod(a,b), d=(a-m)/b;
    if(m && ((b<0)!=(m<0))) d-=1.0;
    double fl; if(d){ fl=floor(d); if(d-fl>0.5) fl+=1.0; } else fl=copysign(0.0,a/b);
    return fl;
}
static double mod_f(double a, double b){
    if(b==0) mp_raise_t(E_ZeroDivisionError,"division by zero");
    double m=fmod(a,b);
    if(m){ if((b<0)!=(m<0)) m+=b; } else m=copysign(0.0,b);
    return m;
}
static Value pow_i(int64_t a, int64_t b){
    if(b<0){ if(a==0) mp_raise_t(E_ZeroDivisionError,"zero to a negative power"); return v_float(pow((double)a,(double)b)); }
    int64_t r=1, base=a;
    while(b){ if(b&1) r=mp_int_checked('*',r,base); b>>=1; if(b) base=mp_int_checked('*',base,base); }
    return v_int(r);
}
static Value pow_f(double a, double b){
    if(a==0 && b<0) mp_raise_t(E_ZeroDivisionError,"zero to a negative power");
    if(a<0 && b!=floor(b) && isfinite(b)){                  /* a negative number to a fractional power: complex */
        double r=pow(-a,b), th=b*M_PI;
        return mp_complex(r*cos(th),r*sin(th));
    }
    double r=pow(a,b);
    if(isinf(r) && isfinite(a) && isfinite(b)) mp_raise_t(E_OverflowError,"(34, 'Numerical result out of range')");
    return v_float(r);
}
static int c_of(Value v, double *re, double *im){
    if(IS(v,T_complex)){ *re=((ComplexObj*)v.u.o)->re; *im=((ComplexObj*)v.u.o)->im; return 1; }
    if(v.k==V_INT||v.k==V_BOOL){ *re=(double)v.u.i; *im=0; return 1; }
    if(v.k==V_FLOAT){ *re=v.u.f; *im=0; return 1; }
    return 0;
}
static Value complex_op(int op, Value a, Value b){
    double ar, ai, br, bi;
    if(!c_of(a,&ar,&ai) || !c_of(b,&br,&bi)) return v_undef();
    switch(op){
        case OP_Add: return mp_complex(ar+br,ai+bi);
        case OP_Sub: return mp_complex(ar-br,ai-bi);
        case OP_Mult: return mp_complex(ar*br-ai*bi,ar*bi+ai*br);
        case OP_Div:{
            if(br==0 && bi==0) mp_raise_t(E_ZeroDivisionError,"division by zero");
            double abs_br=fabs(br), abs_bi=fabs(bi);
            if(abs_br>=abs_bi){ double r=bi/br, d=br+bi*r; return mp_complex((ar+ai*r)/d,(ai-ar*r)/d); }
            double r=br/bi, d=br*r+bi; return mp_complex((ar*r+ai)/d,(ai*r-ar)/d); }
        case OP_Pow:{
            if(br==0 && bi==0) return mp_complex(1,0);
            if(ar==0 && ai==0){ if(bi!=0 || br<0) mp_raise_t(E_ZeroDivisionError,"zero to a negative or complex power"); return mp_complex(0,0); }
            if(bi==0 && br==floor(br) && fabs(br)<=100.0){              /* CPython's c_powi: by multiplying */
                long n=(long)br, u=n<0?-n:n; double rr=1, ri=0, pr=ar, pi=ai;
                for(long mask=1; mask>0 && u>=mask; mask<<=1){
                    if(u & mask){ double t=rr*pr-ri*pi; ri=rr*pi+ri*pr; rr=t; }
                    double t=pr*pr-pi*pi; pi=pr*pi+pi*pr; pr=t; }
                if(n>=0) return mp_complex(rr,ri);
                double abs_br=fabs(rr), abs_bi=fabs(ri);                   /* 1 / r */
                if(abs_br>=abs_bi){ double r=ri/rr, d=rr+ri*r; return mp_complex(1/d,(-r)/d); }
                double r=rr/ri, d=rr*r+ri; return mp_complex(r/d,-1/d); }
            double vabs=hypot(ar,ai), len=pow(vabs,br), at=atan2(ai,ar), phase=at*br;
            if(bi!=0){ len/=exp(at*bi); phase+=bi*log(vabs); }
            return mp_complex(len*cos(phase),len*sin(phase)); }
        default: return v_undef();
    }
}
static Value num_op(int op, Value a, Value b){
    if(IS(a,T_complex) || IS(b,T_complex)){ if(!is_num(a)||!is_num(b)) return v_undef(); return complex_op(op,a,b); }
    if(IS_INTLIKE(a) && IS_INTLIKE(b)){
        int64_t x=a.u.i, y=b.u.i;
        switch(op){
            case OP_Add: return v_int(mp_int_checked('+',x,y));
            case OP_Sub: return v_int(mp_int_checked('-',x,y));
            case OP_Mult: return v_int(mp_int_checked('*',x,y));
            case OP_Div: if(y==0) mp_raise_t(E_ZeroDivisionError,"division by zero"); return v_float((double)x/(double)y);
            case OP_FloorDiv: return v_int(floordiv_i(x,y));
            case OP_Mod: return v_int(mod_i(x,y));
            case OP_Pow: return pow_i(x,y);
            case OP_LShift:
                if(y<0) mp_raise_t(E_ValueError,"negative shift count");
                if(x==0) return v_int(0);
                if(y>=63 || (x>0 ? x>(INT64_MAX>>y) : x<(INT64_MIN>>y))) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)");
                return v_int((int64_t)((uint64_t)x<<y));
            case OP_RShift: if(y<0) mp_raise_t(E_ValueError,"negative shift count"); return v_int(y>=64 ? (x<0?-1:0) : x>>y);
            case OP_BitAnd: if(a.k==V_BOOL && b.k==V_BOOL) return v_bool(x&y); return v_int(x&y);
            case OP_BitOr: if(a.k==V_BOOL && b.k==V_BOOL) return v_bool(x|y); return v_int(x|y);
            case OP_BitXor: if(a.k==V_BOOL && b.k==V_BOOL) return v_bool(x^y); return v_int(x^y);
            default: return v_undef();
        }
    }
    if((a.k==V_FLOAT||IS_INTLIKE(a)) && (b.k==V_FLOAT||IS_INTLIKE(b))){
        double x=a.k==V_FLOAT?a.u.f:(double)a.u.i, y=b.k==V_FLOAT?b.u.f:(double)b.u.i;
        switch(op){
            case OP_Add: return v_float(x+y);
            case OP_Sub: return v_float(x-y);
            case OP_Mult: return v_float(x*y);
            case OP_Div: if(y==0) mp_raise_t(E_ZeroDivisionError,"division by zero"); return v_float(x/y);
            case OP_FloorDiv: return v_float(floordiv_f(x,y));
            case OP_Mod: return v_float(mod_f(x,y));
            case OP_Pow: return pow_f(x,y);
            default: return v_undef();
        }
    }
    return v_undef();
}

static const char *op_sym(int op){
    switch(op){
        case OP_Add: return "+"; case OP_Sub: return "-"; case OP_Mult: return "*"; case OP_MatMult: return "@"; case OP_Div: return "/";
        case OP_Mod: return "%"; case OP_Pow: return "** or pow()"; case OP_LShift: return "<<"; case OP_RShift: return ">>";
        case OP_BitOr: return "|"; case OP_BitXor: return "^"; case OP_BitAnd: return "&"; case OP_FloorDiv: return "//";
        default: return "?";
    }
}
static const char *op_dunder(int op, int kind){   /* kind 0 normal, 1 reflected, 2 in place */
    static const char *n[][3]={
        {"__add__","__radd__","__iadd__"},{"__sub__","__rsub__","__isub__"},{"__mul__","__rmul__","__imul__"},{"__matmul__","__rmatmul__","__imatmul__"},
        {"__truediv__","__rtruediv__","__itruediv__"},{"__mod__","__rmod__","__imod__"},{"__pow__","__rpow__","__ipow__"},{"__lshift__","__rlshift__","__ilshift__"},
        {"__rshift__","__rrshift__","__irshift__"},{"__or__","__ror__","__ior__"},{"__xor__","__rxor__","__ixor__"},{"__and__","__rand__","__iand__"},
        {"__floordiv__","__rfloordiv__","__ifloordiv__"}};
    return op>=OP_Add && op<=OP_FloorDiv ? n[op-OP_Add][kind] : NULL;
}
static Value seq_repeat(Value s, int64_t n){
    if(n<0) n=0;
    if(IS_STR(s)){ StrObj *x=AS_STR(s); if(x->len && n>INT64_MAX/x->len) mp_raise_t(E_OverflowError,"repeated string is too long");
        SBuf b={0}; for(int64_t i=0;i<n;i++) sb_put(&b,x->s,x->len); return sb_value(&b); }
    if(IS(s,T_bytes)){ BytesObj *x=AS_BYTES(s); SBuf b={0}; for(int64_t i=0;i<n;i++) sb_put(&b,(const char*)x->s,x->len); Value r=mp_bytes(b.s?b.s:"",b.n); free(b.s); return r; }
    if(IS(s,T_list)){ ListObj *x=AS_LIST(s); Value r=mp_list(0,NULL); for(int64_t i=0;i<n;i++) for(int64_t j=0;j<x->len;j++) mp_list_append(r,x->items[j]); return r; }
    TupleObj *x=AS_TUPLE(s); int64_t len=x->len*n; Value r=mp_tuple(len,NULL);
    for(int64_t i=0;i<n;i++) memcpy(AS_TUPLE(r)->items+i*x->len,x->items,sizeof(Value)*(size_t)x->len);
    return r;
}
static int is_seq(Value v){ return IS_STR(v) || IS(v,T_bytes) || IS(v,T_list) || IS(v,T_tuple); }
static Value builtin_binop(int op, Value a, Value b){
    Value r=num_op(op,a,b);
    if(r.k!=V_UNDEF) return r;
    Type *ta=TYPE(a), *tb=TYPE(b);
    switch(op){
        case OP_Add:
            if(IS_STR(a)){
                if(!IS_STR(b)) mp_raise_t(E_TypeError,"can only concatenate str (not \"%s\") to str",tb->name->s);
                StrObj *x=AS_STR(a), *y=AS_STR(b); SBuf s={0}; sb_put(&s,x->s,x->len); sb_put(&s,y->s,y->len); return sb_value(&s); }
            if(IS(a,T_list)){
                if(!IS(b,T_list)) mp_raise_t(E_TypeError,"can only concatenate list (not \"%s\") to list",tb->name->s);
                ListObj *x=AS_LIST(a), *y=AS_LIST(b); Value l=mp_list(x->len+y->len,NULL);
                memcpy(AS_LIST(l)->items,x->items,sizeof(Value)*(size_t)x->len); memcpy(AS_LIST(l)->items+x->len,y->items,sizeof(Value)*(size_t)y->len); return l; }
            if(IS(a,T_tuple)){
                if(!IS(b,T_tuple)) mp_raise_t(E_TypeError,"can only concatenate tuple (not \"%s\") to tuple",tb->name->s);
                TupleObj *x=AS_TUPLE(a), *y=AS_TUPLE(b); Value t=mp_tuple(x->len+y->len,NULL);
                memcpy(AS_TUPLE(t)->items,x->items,sizeof(Value)*(size_t)x->len); memcpy(AS_TUPLE(t)->items+x->len,y->items,sizeof(Value)*(size_t)y->len); return t; }
            if(IS(a,T_bytes) && IS(b,T_bytes)){ BytesObj *x=AS_BYTES(a), *y=AS_BYTES(b); SBuf s={0}; sb_put(&s,(const char*)x->s,x->len); sb_put(&s,(const char*)y->s,y->len); Value v=mp_bytes(s.s,s.n); free(s.s); return v; }
            break;
        case OP_Mult:
            if(is_seq(a) && IS_INTLIKE(b)) return seq_repeat(a,b.u.i);
            if(is_seq(b) && IS_INTLIKE(a)) return seq_repeat(b,a.u.i);
            if(is_seq(a) && !user(b)) mp_raise_t(E_TypeError,"can't multiply sequence by non-int of type '%s'",tb->name->s);
            if(is_seq(b) && !user(a)) mp_raise_t(E_TypeError,"can't multiply sequence by non-int of type '%s'",ta->name->s);
            break;
        case OP_Mod:
            if(IS_STR(a)) return mp_percent_format(a,b);
            break;
        case OP_BitOr: case OP_BitAnd: case OP_Sub: case OP_BitXor:
            if(is_set(a) && is_set(b)){
                SetObj *x=(SetObj*)a.u.o, *y=(SetObj*)b.u.o; Type *rt=ta;
                if(op==OP_BitOr){ Value r2=mp_set_copy(x,rt); mp_set_merge((SetObj*)r2.u.o,y); return r2; }
                if(op==OP_BitAnd){
                    Value r2=mp_set(rt);
                    if(y->used>x->used){ SetObj *t=x; x=y; y=t; }
                    int64_t pos=0; Value k;
                    while(mp_set_next(y,&pos,&k)){ uint64_t h=y->table[pos-1].hash; if(mp_set_has_hashed(x,k,h)) mp_set_add_hashed((SetObj*)r2.u.o,k,h); }
                    return r2; }
                if(op==OP_Sub){
                    if((x->used>>2)>y->used){ Value r2=mp_set_copy(x,rt); mp_set_diff_update((SetObj*)r2.u.o,b); return r2; }
                    Value r2=mp_set(rt); int64_t pos=0; Value k;
                    while(mp_set_next(x,&pos,&k)){ uint64_t h=x->table[pos-1].hash; if(!mp_set_has_hashed(y,k,h)) mp_set_add_hashed((SetObj*)r2.u.o,k,h); }
                    return r2; }
                /* ^: a copy of b, then a toggled in */
                Value r2=mp_set_copy(y,rt); mp_set_xor_update((SetObj*)r2.u.o,a);
                return r2;
            }
            if(op==OP_BitOr && IS(a,T_dict) && IS(b,T_dict)){
                Value d=mp_dict(); int64_t pos=0; Value k, val;
                while(mp_dict_next(AS_DICT(a),&pos,&k,&val)) mp_dict_set(AS_DICT(d),k,val);
                pos=0; while(mp_dict_next(AS_DICT(b),&pos,&k,&val)) mp_dict_set(AS_DICT(d),k,val);
                return d; }
            break;
        default: break;
    }
    return v_undef();
}
static Value try_dunder(Value self, const char *name, Value other){
    Value m=mp_type_lookup_s(TYPE(self),name);
    if(m.k==V_UNDEF || IS(m,T_native)) return mp_NotImplemented;
    return mp_call2(m,self,other);
}
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);
static Value binop_ex(int op, Value a, Value b, int inplace);
Value mp_binop(int op, Value a, Value b){ return binop_ex(op,a,b,0); }
static Value binop_ex(int op, Value a, Value b, int inplace){
    if(user(a) || user(b)){
        const char *n=op_dunder(op,0), *rn=op_dunder(op,1);
        Type *ta=TYPE(a), *tb=TYPE(b);
        int sub= ta!=tb && user(b) && mp_is_subtype(tb,ta) && mp_type_lookup_s(tb,rn).k!=V_UNDEF;
        if(sub){ Value r=try_dunder(b,rn,a); if(r.u.o!=mp_NotImplemented.u.o || r.k!=V_OBJ) return r; }
        if(user(a)){ Value r=try_dunder(a,n,b); if(!(r.k==V_OBJ && r.u.o==mp_NotImplemented.u.o)) return r; }
        if(!sub && user(b) && ta!=tb){ Value r=try_dunder(b,rn,a); if(!(r.k==V_OBJ && r.u.o==mp_NotImplemented.u.o)) return r; }
        if(!user(a)){ Value r=builtin_binop(op,a,b); if(r.k!=V_UNDEF) return r; }
    } else {
        Value r=builtin_binop(op,a,b);
        if(r.k!=V_UNDEF) return r;
    }
    if(op==OP_BitOr && (IS(a,T_type)||a.k==V_NONE) && (IS(b,T_type)||b.k==V_NONE) && !(a.k==V_NONE && b.k==V_NONE)){   /* int | None: a union type */
        Value nm=mp_str("typing"); Value mod=mp_builtin_import(1,&nm,NULL);
        return mp_call2(mp_getattr_s(mod,"_union2"),a,b); }
    if(inplace){ const char *sy=op==OP_Pow?"**":op_sym(op);
        mp_raise_t(E_TypeError,"unsupported operand type(s) for %s=: '%s' and '%s'",sy,mp_type_name(a),mp_type_name(b)); }
    mp_raise_t(E_TypeError,"unsupported operand type(s) for %s: '%s' and '%s'",op_sym(op),mp_type_name(a),mp_type_name(b));
}
Value mp_inplace(int op, Value a, Value b){
    if(user(a)){
        Value m=mp_type_lookup_s(TYPE(a),op_dunder(op,2));
        if(m.k!=V_UNDEF){ Value r=mp_call2(m,a,b); if(!(r.k==V_OBJ && r.u.o==mp_NotImplemented.u.o)) return r; }
        return binop_ex(op,a,b,1);
    }
    if(IS(a,T_list)){
        if(op==OP_Add){ Value l=mp_list_of(b); ListObj *y=AS_LIST(l); for(int64_t i=0;i<y->len;i++) mp_list_append(a,y->items[i]); return a; }
        if(op==OP_Mult && IS_INTLIKE(b)){ ListObj *x=AS_LIST(a); int64_t n0=x->len, n=b.u.i; if(n<=0){ x->len=0; return a; }
            for(int64_t r=1;r<n;r++) for(int64_t j=0;j<n0;j++) mp_list_append(a,AS_LIST(a)->items[j]); return a; }
    }
    if(IS(a,T_set) && is_set(b)){
        SetObj *x=(SetObj*)a.u.o, *y=(SetObj*)b.u.o;
        if(op==OP_BitOr){ mp_set_merge(x,y); return a; }
        if(op==OP_Sub){ mp_set_diff_update(x,b); return a; }
        if(op==OP_BitXor){ mp_set_xor_update(x,b); return a; }
        if(op==OP_BitAnd){
            Value r=mp_binop(op,a,b); SetObj *rs=(SetObj*)r.u.o;
            free(x->table); x->table=rs->table; x->mask=rs->mask; x->fill=rs->fill; x->used=rs->used; x->hashed=0;
            rs->table=(SEnt*)xmalloc(sizeof(SEnt)*8); memset(rs->table,0,sizeof(SEnt)*8); rs->mask=7; rs->fill=rs->used=0;
            return a; }
    }
    if(IS(a,T_dict) && op==OP_BitOr && IS(b,T_dict)){ int64_t pos=0; Value k, v; while(mp_dict_next(AS_DICT(b),&pos,&k,&v)) mp_dict_set(AS_DICT(a),k,v); return a; }
    return binop_ex(op,a,b,1);
}
Value mp_unary(int op, Value a){
    if(op==OP_Not) return v_bool(!mp_truth(a));
    if(user(a)){
        const char *n= op==OP_USub?"__neg__":op==OP_UAdd?"__pos__":"__invert__";
        Value m=mp_type_lookup_s(TYPE(a),n);
        if(m.k!=V_UNDEF) return mp_call1(m,a);
    }
    switch(a.k){
        case V_INT: case V_BOOL:
            if(op==OP_USub){ if(a.u.i==INT64_MIN) mp_raise_t(E_OverflowError,"integer overflow (ints are 64-bit)"); return v_int(-a.u.i); }
            if(op==OP_UAdd) return v_int(a.u.i);
            return v_int(~a.u.i);
        case V_FLOAT:
            if(op==OP_USub) return v_float(-a.u.f);
            if(op==OP_UAdd) return a;
            break;
        default:
            if(IS(a,T_complex)){ ComplexObj *c=(ComplexObj*)a.u.o; if(op==OP_USub) return mp_complex(-c->re,-c->im); if(op==OP_UAdd) return a; }
            break;
    }
    mp_raise_t(E_TypeError,"bad operand type for unary %s: '%s'",op==OP_USub?"-":op==OP_UAdd?"+":"~",mp_type_name(a));
}

/* ---------------------------------------------------------------- comparison */
static int item_eq(Value a, Value b){ return (a.k==V_OBJ && b.k==V_OBJ && a.u.o==b.u.o) || mp_eq(a,b); }
static int seq_eq(const Value *x, int64_t nx, const Value *y, int64_t ny){
    if(nx!=ny) return 0;
    for(int64_t i=0;i<nx;i++) if(!item_eq(x[i],y[i])) return 0;
    return 1;
}
static int set_subset(SetObj *a, SetObj *b){
    if(a->used>b->used) return 0;
    int64_t pos=0; Value k; while(mp_set_next(a,&pos,&k)) if(!mp_set_has_hashed(b,k,a->table[pos-1].hash)) return 0;
    return 1;
}
/* == of built-in values; -1: not comparable that way */
static int builtin_eq(Value a, Value b){
    if(is_num(a) && is_num(b)){
        if(IS(a,T_complex)||IS(b,T_complex)){ double ar, ai, br, bi; c_of(a,&ar,&ai); c_of(b,&br,&bi); return ar==br && ai==bi; }
        if(IS_INTLIKE(a) && IS_INTLIKE(b)) return a.u.i==b.u.i;
        if(a.k==V_FLOAT && b.k==V_FLOAT) return a.u.f==b.u.f;
        double f= a.k==V_FLOAT ? a.u.f : b.u.f; int64_t i= a.k==V_FLOAT ? b.u.i : a.u.i;
        if(f!=f) return 0;
        if(f>=9.2233720368547758e18 || f<-9.2233720368547758e18) return 0;
        return (double)(int64_t)f==f && (int64_t)f==i;
    }
    if(a.k!=V_OBJ || b.k!=V_OBJ){ if(a.k==V_NONE && b.k==V_NONE) return 1; return a.k==V_OBJ||b.k==V_OBJ ? 0 : (a.k==b.k); }
    if(a.u.o==b.u.o && a.u.o->type->layout!=LY_TUPLE && a.u.o->type->layout!=LY_LIST) return 1;
    Layout la=a.u.o->type->layout, lb=b.u.o->type->layout;
    if(la==LY_STR && lb==LY_STR){ StrObj *x=AS_STR(a), *y=AS_STR(b); return x->len==y->len && !memcmp(x->s,y->s,(size_t)x->len); }
    if(la==LY_BYTES && lb==LY_BYTES){ BytesObj *x=AS_BYTES(a), *y=AS_BYTES(b); return x->len==y->len && !memcmp(x->s,y->s,(size_t)x->len); }
    if(la==LY_TUPLE && lb==LY_TUPLE) return seq_eq(AS_TUPLE(a)->items,AS_TUPLE(a)->len,AS_TUPLE(b)->items,AS_TUPLE(b)->len);
    if(la==LY_LIST && lb==LY_LIST) return seq_eq(AS_LIST(a)->items,AS_LIST(a)->len,AS_LIST(b)->items,AS_LIST(b)->len);
    if(la==LY_DICT && lb==LY_DICT){
        DictObj *x=AS_DICT(a), *y=AS_DICT(b);
        if(x->used!=y->used) return 0;
        int64_t pos=0; Value k, v, w;
        while(mp_dict_next(x,&pos,&k,&v)){ if(!mp_dict_get(y,k,&w) || !item_eq(v,w)) return 0; }
        return 1;
    }
    if(la==LY_SET && lb==LY_SET){ SetObj *x=(SetObj*)a.u.o, *y=(SetObj*)b.u.o; return x->used==y->used && set_subset(x,y); }
    if(la==LY_RANGE && lb==LY_RANGE){
        RangeObj *x=(RangeObj*)a.u.o, *y=(RangeObj*)b.u.o; int64_t nx=mp_len(a), ny=mp_len(b);
        if(nx!=ny) return 0; if(nx==0) return 1; if(x->start!=y->start) return 0; if(nx==1) return 1; return x->step==y->step;
    }
    if(la==LY_SLICE && lb==LY_SLICE){ SliceObj *x=(SliceObj*)a.u.o, *y=(SliceObj*)b.u.o; return item_eq(x->start,y->start) && item_eq(x->stop,y->stop) && item_eq(x->step,y->step); }
    if(a.u.o->type==T_dict_keys && b.u.o->type==T_dict_keys){
        DictObj *x=AS_DICT(((IterObj*)a.u.o)->src), *y=AS_DICT(((IterObj*)b.u.o)->src);
        if(x->used!=y->used) return 0; int64_t pos=0; Value k; while(mp_dict_next(x,&pos,&k,NULL)) if(!mp_dict_get(y,k,NULL)) return 0; return 1;
    }
    return a.u.o==b.u.o;
}
static const char *cmp_dunder(int op){
    switch(op){ case OP_Eq: return "__eq__"; case OP_NotEq: return "__ne__"; case OP_Lt: return "__lt__"; case OP_LtE: return "__le__"; case OP_Gt: return "__gt__"; default: return "__ge__"; }
}
static int cmp_swap(int op){
    switch(op){ case OP_Lt: return OP_Gt; case OP_LtE: return OP_GtE; case OP_Gt: return OP_Lt; case OP_GtE: return OP_LtE; default: return op; }
}
static int is_notimpl(Value v){ return v.k==V_OBJ && v.u.o==mp_NotImplemented.u.o; }
/* rich comparison through the dunders; V_UNDEF when neither side has one */
static Value rich_dunder(int op, Value a, Value b){
    Type *ta=TYPE(a), *tb=TYPE(b);
    int swapped_first= user(b) && ta!=tb && mp_is_subtype(tb,ta);
    if(swapped_first){
        Value m=mp_type_lookup_s(tb,cmp_dunder(cmp_swap(op)));
        if(m.k!=V_UNDEF && !IS(m,T_native)){ Value r=mp_call2(m,b,a); if(!is_notimpl(r)) return r; }
    }
    if(user(a)){
        Value m=mp_type_lookup_s(ta,cmp_dunder(op));
        if(m.k!=V_UNDEF && !IS(m,T_native)){ Value r=mp_call2(m,a,b); if(!is_notimpl(r)) return r; }
    }
    if(!swapped_first && user(b)){
        Value m=mp_type_lookup_s(tb,cmp_dunder(cmp_swap(op)));
        if(m.k!=V_UNDEF && !IS(m,T_native)){ Value r=mp_call2(m,b,a); if(!is_notimpl(r)) return r; }
    }
    if(op==OP_NotEq && user(a)){                   /* != from __eq__ */
        Value m=mp_type_lookup_s(ta,"__eq__");
        if(m.k!=V_UNDEF && !IS(m,T_native)){ Value r=mp_call2(m,a,b); if(!is_notimpl(r)) return v_bool(!mp_truth(r)); }
    }
    return v_undef();
}
int mp_eq(Value a, Value b){
    if(user(a)||user(b)){
        Value r=rich_dunder(OP_Eq,a,b);
        if(r.k!=V_UNDEF) return mp_truth(r);
        return a.k==V_OBJ && b.k==V_OBJ && a.u.o==b.u.o;
    }
    return builtin_eq(a,b);
}
/* <: -1 / 0 / 1 for built-in ordered values; 2: not ordered that way */
static int seq_cmp(const Value *x, int64_t nx, const Value *y, int64_t ny, int op, Value *res){
    int64_t n=nx<ny?nx:ny;
    for(int64_t i=0;i<n;i++) if(!item_eq(x[i],y[i])){ *res=mp_compare(op,x[i],y[i]); return 1; }
    switch(op){ case OP_Lt: *res=v_bool(nx<ny); break; case OP_LtE: *res=v_bool(nx<=ny); break; case OP_Gt: *res=v_bool(nx>ny); break; default: *res=v_bool(nx>=ny); }
    return 1;
}
static int order_res(int op, int c){
    switch(op){ case OP_Lt: return c<0; case OP_LtE: return c<=0; case OP_Gt: return c>0; default: return c>=0; }
}
static int builtin_order(int op, Value a, Value b, Value *res){
    if((IS_INTLIKE(a)||a.k==V_FLOAT) && (IS_INTLIKE(b)||b.k==V_FLOAT)){
        if(IS_INTLIKE(a) && IS_INTLIKE(b)){ *res=v_bool(order_res(op,a.u.i<b.u.i?-1:a.u.i>b.u.i?1:0)); return 1; }
        double x=a.k==V_FLOAT?a.u.f:(double)a.u.i, y=b.k==V_FLOAT?b.u.f:(double)b.u.i;
        if(x!=x||y!=y){ *res=v_bool(0); return 1; }
        *res=v_bool(order_res(op,x<y?-1:x>y?1:0)); return 1;
    }
    if(a.k!=V_OBJ || b.k!=V_OBJ) return 0;
    Layout la=a.u.o->type->layout, lb=b.u.o->type->layout;
    if(la==LY_STR && lb==LY_STR){ StrObj *x=AS_STR(a), *y=AS_STR(b); int64_t n=x->len<y->len?x->len:y->len; int c=memcmp(x->s,y->s,(size_t)n); if(!c) c=x->len<y->len?-1:x->len>y->len?1:0; *res=v_bool(order_res(op,c)); return 1; }
    if(la==LY_BYTES && lb==LY_BYTES){ BytesObj *x=AS_BYTES(a), *y=AS_BYTES(b); int64_t n=x->len<y->len?x->len:y->len; int c=memcmp(x->s,y->s,(size_t)n); if(!c) c=x->len<y->len?-1:x->len>y->len?1:0; *res=v_bool(order_res(op,c)); return 1; }
    if(la==LY_TUPLE && lb==LY_TUPLE) return seq_cmp(AS_TUPLE(a)->items,AS_TUPLE(a)->len,AS_TUPLE(b)->items,AS_TUPLE(b)->len,op,res);
    if(la==LY_LIST && lb==LY_LIST) return seq_cmp(AS_LIST(a)->items,AS_LIST(a)->len,AS_LIST(b)->items,AS_LIST(b)->len,op,res);
    if(la==LY_SET && lb==LY_SET){
        SetObj *x=(SetObj*)a.u.o, *y=(SetObj*)b.u.o;
        switch(op){
            case OP_LtE: *res=v_bool(set_subset(x,y)); return 1;
            case OP_GtE: *res=v_bool(set_subset(y,x)); return 1;
            case OP_Lt: *res=v_bool(x->used<y->used && set_subset(x,y)); return 1;
            default: *res=v_bool(x->used>y->used && set_subset(y,x)); return 1;
        }
    }
    return 0;
}
Value mp_compare(int op, Value a, Value b){
    switch(op){
        case OP_Is: return v_bool(a.k==b.k && (a.k==V_OBJ ? a.u.o==b.u.o : a.k==V_FLOAT ? a.u.f==b.u.f : a.u.i==b.u.i));
        case OP_IsNot: return v_bool(!(a.k==b.k && (a.k==V_OBJ ? a.u.o==b.u.o : a.k==V_FLOAT ? a.u.f==b.u.f : a.u.i==b.u.i)));
        case OP_In: return v_bool(mp_contains(b,a));
        case OP_NotIn: return v_bool(!mp_contains(b,a));
        default: break;
    }
    if(user(a)||user(b)){
        Value r=rich_dunder(op,a,b);
        if(r.k!=V_UNDEF) return r;
        if(op==OP_Eq) return v_bool(a.k==V_OBJ && b.k==V_OBJ && a.u.o==b.u.o);
        if(op==OP_NotEq) return v_bool(!(a.k==V_OBJ && b.k==V_OBJ && a.u.o==b.u.o));
    } else {
        if(op==OP_Eq) return v_bool(builtin_eq(a,b));
        if(op==OP_NotEq) return v_bool(!builtin_eq(a,b));
        Value res;
        if(builtin_order(op,a,b,&res)) return res;
    }
    const char *s= op==OP_Lt?"<":op==OP_LtE?"<=":op==OP_Gt?">":">=";
    mp_raise_t(E_TypeError,"'%s' not supported between instances of '%s' and '%s'",s,mp_type_name(a),mp_type_name(b));
}

/* ---------------------------------------------------------------- containers */
int64_t mp_len(Value v){
    if(v.k==V_OBJ){
        Obj *o=v.u.o; Type *t=o->type;
        if(t->flags&TF_DUNDERS){
            Value m=mp_type_lookup_s(t,"__len__");
            if(m.k!=V_UNDEF && !IS(m,T_native)){
                Value r=mp_call1(m,v); int64_t n=mp_index(r,"len");
                if(n<0) mp_raise_t(E_ValueError,"__len__() should return >= 0");
                return n;
            }
        }
        switch(t->layout){
            case LY_STR: return ((StrObj*)o)->cplen;
            case LY_BYTES: return ((BytesObj*)o)->len;
            case LY_TUPLE: return ((TupleObj*)o)->len;
            case LY_LIST: return ((ListObj*)o)->len;
            case LY_DICT: return ((DictObj*)o)->used;
            case LY_SET: return ((SetObj*)o)->used;
            case LY_BUFFER: return ((BufferObj*)o)->len;
            case LY_RANGE:{ RangeObj *r=(RangeObj*)o;
                if(r->step>0 && r->start<r->stop) return (int64_t)(((uint64_t)r->stop-(uint64_t)r->start-1)/(uint64_t)r->step+1);
                if(r->step<0 && r->start>r->stop) return (int64_t)(((uint64_t)r->start-(uint64_t)r->stop-1)/(uint64_t)(-r->step)+1);
                return 0; }
            default: break;
        }
        if(t==T_dict_keys||t==T_dict_values||t==T_dict_items) return AS_DICT(((IterObj*)o)->src)->used;
    }
    mp_raise_t(E_TypeError,"object of type '%s' has no len()",mp_type_name(v));
}
/* start / stop / step of a slice over n items; -> the count */
int64_t mp_slice_indices(Value sl, int64_t n, int64_t *start, int64_t *stop, int64_t *step){
    SliceObj *s=(SliceObj*)sl.u.o;
    *step= IS_NONE(s->step) ? 1 : mp_index(s->step,"slice");
    if(*step==0) mp_raise_t(E_ValueError,"slice step cannot be zero");
    int64_t lo= *step<0 ? -1 : 0, hi= *step<0 ? n-1 : n;
    if(IS_NONE(s->start)) *start= *step<0 ? hi : lo;
    else { *start=mp_index(s->start,"slice"); if(*start<0){ *start+=n; if(*start<lo) *start=lo; } else if(*start>hi) *start=hi; }
    if(IS_NONE(s->stop)) *stop= *step<0 ? lo : hi;
    else { *stop=mp_index(s->stop,"slice"); if(*stop<0){ *stop+=n; if(*stop<lo) *stop=lo; } else if(*stop>hi) *stop=hi; }
    if(*step>0) return *start<*stop ? (*stop-*start-1)/ *step+1 : 0;
    return *start>*stop ? (*start-*stop-1)/(-*step)+1 : 0;
}
static int64_t norm_index(int64_t i, int64_t n, const char *what){
    if(i<0) i+=n;
    if(i<0 || i>=n){ if(!what) mp_raise_t(E_IndexError,"index out of range"); mp_raise_t(E_IndexError,"%s index out of range",what); }
    return i;
}
static Value str_at(StrObj *s, int64_t i){
    if(s->ascii) return mp_strn(s->s+i,1);
    int64_t p=mp_str_byteoff(s,i), q=p; mp_utf8_decode(s->s,s->len,&q);
    return mp_strn(s->s+p,q-p);
}
Value mp_builtin_import(int argc, Value *argv, TupleObj *kw);
Value mp_getitem(Value o, Value key){
    if(o.k==V_OBJ){
        Type *t=o.u.o->type;
        if(t->flags&TF_DUNDERS){
            Value m=mp_type_lookup_s(t,"__getitem__");
            if(m.k!=V_UNDEF && !IS(m,T_native)) return mp_call2(m,o,key);
        }
        int isslice=IS(key,T_slice);
        switch(t->layout){
            case LY_LIST:{ ListObj *l=AS_LIST(o);
                if(isslice){ int64_t a,b,s; mp_slice_indices(key,l->len,&a,&b,&s); return mp_list_slice(o,a,b,s); }
                if(!IS_INTLIKE(key) && !user(key)) mp_raise_t(E_TypeError,"list indices must be integers or slices, not %s",mp_type_name(key));
                return l->items[norm_index(mp_index(key,"list"),l->len,"list")]; }
            case LY_TUPLE:{ TupleObj *tp=AS_TUPLE(o);
                if(isslice){ int64_t a,b,s, n=mp_slice_indices(key,tp->len,&a,&b,&s); Value r=mp_tuple(n,NULL); for(int64_t i=0,j=a;i<n;i++,j+=s) AS_TUPLE(r)->items[i]=tp->items[j]; return r; }
                if(!IS_INTLIKE(key) && !user(key)) mp_raise_t(E_TypeError,"tuple indices must be integers or slices, not %s",mp_type_name(key));
                return tp->items[norm_index(mp_index(key,"tuple"),tp->len,"tuple")]; }
            case LY_STR:{ StrObj *s=AS_STR(o);
                if(isslice){
                    int64_t a,b,st, n=mp_slice_indices(key,s->cplen,&a,&b,&st);
                    if(st==1){ int64_t pa=mp_str_byteoff(s,a), pb=mp_str_byteoff(s,a+n); return mp_strn(s->s+pa,pb-pa); }
                    SBuf sb={0};
                    for(int64_t i=0,j=a;i<n;i++,j+=st){ int64_t p=mp_str_byteoff(s,j), q=p; mp_utf8_decode(s->s,s->len,&q); sb_put(&sb,s->s+p,q-p); }
                    return sb_value(&sb);
                }
                if(!IS_INTLIKE(key) && !user(key)) mp_raise_t(E_TypeError,"string indices must be integers, not '%s'",mp_type_name(key));
                return str_at(s,norm_index(mp_index(key,"str"),s->cplen,"string")); }
            case LY_BYTES:{ BytesObj *b=AS_BYTES(o);
                if(isslice){ int64_t a,bb,st, n=mp_slice_indices(key,b->len,&a,&bb,&st); SBuf sb={0}; for(int64_t i=0,j=a;i<n;i++,j+=st) sb_putc(&sb,(char)b->s[j]); Value r=mp_bytes(sb.s?sb.s:"",sb.n); free(sb.s); return r; }
                return v_int(b->s[norm_index(mp_index(key,"bytes"),b->len,NULL)]); }
            case LY_DICT:{ Value v;
                if(mp_dict_get(AS_DICT(o),key,&v)) return v;
                if(t!=T_dict){ Value m=mp_type_lookup_s(t,"__missing__"); if(m.k!=V_UNDEF) return mp_call2(m,o,key); }
                mp_raise(mp_exc_args(E_KeyError,mp_tuple(1,&key))); }
            case LY_RANGE:{ RangeObj *r=(RangeObj*)o.u.o; int64_t n=mp_len(o);
                if(isslice){ int64_t a,b,s; mp_slice_indices(key,n,&a,&b,&s); return mp_range(r->start+a*r->step,r->start+b*r->step,r->step*s); }
                return v_int(r->start+norm_index(mp_index(key,"range"),n,"range object")*r->step); }
            case LY_BUFFER:{ BufferObj *b=(BufferObj*)o.u.o; return v_int(b->data[norm_index(mp_index(key,"buffer"),b->len,"buffer")]); }
            case LY_TYPE:{                                /* list[int] ...: C.__class_getitem__, else a GenericAlias */
                Value m=mp_type_lookup_s((Type*)o.u.o,"__class_getitem__");          /* (an implicit classmethod) */
                if(m.k!=V_UNDEF){ if(IS(m,T_classmethod)||IS(m,T_staticmethod)) return mp_call1(mp_getattr_s(o,"__class_getitem__"),key); return mp_call2(m,o,key); }
                Value nm=mp_str("typing"); Value mod=mp_builtin_import(1,&nm,NULL);
                return mp_call2(mp_getattr_s(mod,"GenericAlias"),o,key); }
            default: break;
        }
    }
    mp_raise_t(E_TypeError,"'%s' object is not subscriptable",mp_type_name(o));
}
void mp_list_setslice(Value l, Value sl, Value v);
void mp_setitem(Value o, Value key, Value val){
    if(o.k==V_OBJ){
        Type *t=o.u.o->type;
        if(t->flags&TF_DUNDERS){
            Value m=mp_type_lookup_s(t,"__setitem__");
            if(m.k!=V_UNDEF && !IS(m,T_native)){ Value a[3]={o,key,val}; mp_call(m,3,a,NULL); return; }
        }
        switch(t->layout){
            case LY_LIST:{ ListObj *l=AS_LIST(o);
                if(IS(key,T_slice)){ mp_list_setslice(o,key,val); return; }
                if(!IS_INTLIKE(key) && !user(key)) mp_raise_t(E_TypeError,"list indices must be integers or slices, not %s",mp_type_name(key));
                int64_t i=mp_index(key,"list"); if(i<0) i+=l->len;
                if(i<0 || i>=l->len) mp_raise_t(E_IndexError,"list assignment index out of range");
                l->items[i]=val; return; }
            case LY_DICT: mp_dict_set(AS_DICT(o),key,val); return;
            case LY_BUFFER:{ BufferObj *b=(BufferObj*)o.u.o; b->data[norm_index(mp_index(key,"buffer"),b->len,"buffer")]=(unsigned char)mp_index(val,"buffer"); return; }
            default: break;
        }
    }
    mp_raise_t(E_TypeError,"'%s' object does not support item assignment",mp_type_name(o));
}
void mp_delitem(Value o, Value key){
    if(o.k==V_OBJ){
        Type *t=o.u.o->type;
        if(t->flags&TF_DUNDERS){
            Value m=mp_type_lookup_s(t,"__delitem__");
            if(m.k!=V_UNDEF && !IS(m,T_native)){ mp_call2(m,o,key); return; }
        }
        switch(t->layout){
            case LY_LIST:{ ListObj *l=AS_LIST(o);
                if(IS(key,T_slice)){
                    int64_t a,b,s, n=mp_slice_indices(key,l->len,&a,&b,&s);
                    if(!n) return;
                    if(s<0){ a=a+(n-1)*s; s=-s; }
                    char *del=(char*)xmalloc((size_t)l->len); memset(del,0,(size_t)l->len);
                    for(int64_t i=0,j=a;i<n;i++,j+=s) del[j]=1;
                    int64_t w=0; for(int64_t i=0;i<l->len;i++) if(!del[i]) l->items[w++]=l->items[i];
                    l->len=w; free(del); return;
                }
                int64_t i=mp_index(key,"list"); if(i<0) i+=l->len;
                if(i<0 || i>=l->len) mp_raise_t(E_IndexError,"list assignment index out of range");
                memmove(l->items+i,l->items+i+1,sizeof(Value)*(size_t)(l->len-i-1)); l->len--; return; }
            case LY_DICT: if(!mp_dict_del(AS_DICT(o),key)) mp_raise(mp_exc_args(E_KeyError,mp_tuple(1,&key))); return;
            default: break;
        }
    }
    mp_raise_t(E_TypeError,"'%s' object doesn't support item deletion",mp_type_name(o));
}
int mp_contains(Value c, Value x){
    if(c.k==V_OBJ){
        Type *t=c.u.o->type;
        if(t->flags&TF_DUNDERS){
            Value m=mp_type_lookup_s(t,"__contains__");
            if(m.k!=V_UNDEF && !IS(m,T_native)) return mp_truth(mp_call2(m,c,x));
        }
        switch(t->layout){
            case LY_STR:{
                if(!IS_STR(x)) mp_raise_t(E_TypeError,"'in <string>' requires string as left operand, not %s",mp_type_name(x));
                StrObj *h=AS_STR(c), *n=AS_STR(x);
                if(n->len==0) return 1;
                for(int64_t i=0;i+n->len<=h->len;i++) if(!memcmp(h->s+i,n->s,(size_t)n->len)) return 1;
                return 0; }
            case LY_BYTES:{
                BytesObj *h=AS_BYTES(c);
                if(IS_INTLIKE(x)){ if(x.u.i<0 || x.u.i>255) mp_raise_t(E_ValueError,"byte must be in range(0, 256)");
                    for(int64_t i=0;i<h->len;i++) if(h->s[i]==x.u.i) return 1; return 0; }
                if(!IS(x,T_bytes)) mp_raise_t(E_TypeError,"a bytes-like object is required, not '%s'",mp_type_name(x));
                BytesObj *n=AS_BYTES(x); if(!n->len) return 1;
                for(int64_t i=0;i+n->len<=h->len;i++) if(!memcmp(h->s+i,n->s,(size_t)n->len)) return 1;
                return 0; }
            case LY_LIST:{ ListObj *l=AS_LIST(c); for(int64_t i=0;i<l->len;i++) if(item_eq(l->items[i],x)) return 1; return 0; }
            case LY_TUPLE:{ TupleObj *tp=AS_TUPLE(c); for(int64_t i=0;i<tp->len;i++) if(item_eq(tp->items[i],x)) return 1; return 0; }
            case LY_DICT: return mp_dict_get(AS_DICT(c),x,NULL);
            case LY_SET: return mp_set_has((SetObj*)c.u.o,x);
            case LY_RANGE:{ RangeObj *r=(RangeObj*)c.u.o;
                if(!IS_INTLIKE(x)){ if(x.k==V_FLOAT && x.u.f==floor(x.u.f)) x=v_int((int64_t)x.u.f); else break; }
                int64_t v=x.u.i;
                if(r->step>0 ? (v<r->start || v>=r->stop) : (v>r->start || v<=r->stop)) return 0;
                return (v-r->start)%r->step==0; }
            default: break;
        }
        if(t==T_dict_keys) return mp_dict_get(AS_DICT(((IterObj*)c.u.o)->src),x,NULL);
        if((t->flags&TF_DUNDERS) && mp_type_lookup_s(t,"__iter__").k==V_UNDEF && mp_type_lookup_s(t,"__getitem__").k==V_UNDEF)
            mp_raise_t(E_TypeError,"argument of type '%s' is not a container or iterable",mp_type_name(c));
    } else mp_raise_t(E_TypeError,"argument of type '%s' is not a container or iterable",mp_type_name(c));
    Value it=mp_iter(c), v;
    while(mp_next(it,&v)) if(item_eq(v,x)) return 1;
    return 0;
}

/* ---------------------------------------------------------------- iteration */
static Value new_iter(int kind, Value src){ IterObj *it=(IterObj*)mp_alloc(T_iter,sizeof(IterObj)); it->kind=kind; it->src=src; return v_obj(it); }
Value mp_iter_kind(int kind, Value src, Value aux, Value aux2){ Value v=new_iter(kind,src); ((IterObj*)v.u.o)->aux=aux; ((IterObj*)v.u.o)->aux2=aux2; return v; }
Value mp_iter(Value v){
    if(v.k==V_OBJ){
        Obj *o=v.u.o; Type *t=o->type;
        if(t->flags&TF_DUNDERS){
            Value m=mp_type_lookup_s(t,"__iter__");
            if(m.k!=V_UNDEF && !IS(m,T_native)){
                Value r=mp_call1(m,v);
                if(!(TYPE(r)->flags&TF_DUNDERS) && !IS(r,T_iter) && TYPE(r)->layout!=LY_GEN && !IS(r,T_file))
                    mp_raise_t(E_TypeError,"iter() returned non-iterator of type '%s'",mp_type_name(r));
                return r;
            }
            if(mp_type_lookup_s(t,"__getitem__").k!=V_UNDEF) return new_iter(IT_GETITEM,v);
        }
        switch(t->layout){
            case LY_LIST: return new_iter(IT_SEQ,v);
            case LY_TUPLE: return new_iter(IT_SEQ,v);
            case LY_STR: return new_iter(IT_STR,v);
            case LY_BYTES: return new_iter(IT_BYTES,v);
            case LY_DICT: return new_iter(IT_DICTK,v);
            case LY_SET: return new_iter(IT_SET,v);
            case LY_RANGE:{ RangeObj *r=(RangeObj*)o; Value it=new_iter(IT_RANGE,v); IterObj *io=(IterObj*)it.u.o; io->i=r->start; io->n=mp_len(v); return it; }
            case LY_ITER: if(t==T_iter) return v; break;
            case LY_GEN: if(t!=T_coroutine) return v; break;
            case LY_FILE: return v;
            default: break;
        }
        if(t==T_dict_keys) return new_iter(IT_DICTK,((IterObj*)o)->src);
        if(t==T_dict_values) return new_iter(IT_DICTV,((IterObj*)o)->src);
        if(t==T_dict_items) return new_iter(IT_DICTI,((IterObj*)o)->src);
    }
    mp_raise_t(E_TypeError,"'%s' object is not iterable",mp_type_name(v));
}
int mp_file_readline(Value f, Value *out);
int mp_native_iter_next(IterObj *it, Value *out);
int mp_next(Value itv, Value *out){
    if(itv.k!=V_OBJ) mp_raise_t(E_TypeError,"'%s' object is not an iterator",mp_type_name(itv));
    Obj *o=itv.u.o; Type *t=o->type;
    if(t==T_iter){
        IterObj *it=(IterObj*)o;
        switch(it->kind){
            case IT_SEQ:
                if(IS(it->src,T_list)){ ListObj *l=AS_LIST(it->src); if(it->i>=l->len) return 0; *out=l->items[it->i++]; return 1; }
                { TupleObj *tp=AS_TUPLE(it->src); if(it->i>=tp->len) return 0; *out=tp->items[it->i++]; return 1; }
            case IT_STR:{ StrObj *s=AS_STR(it->src); if(it->i>=s->len) return 0;
                int64_t p=it->i; mp_utf8_decode(s->s,s->len,&p); *out=mp_strn(s->s+it->i,p-it->i); it->i=p; return 1; }
            case IT_BYTES:{ BytesObj *b=AS_BYTES(it->src); if(it->i>=b->len) return 0; *out=v_int(b->s[it->i++]); return 1; }
            case IT_RANGE:{ RangeObj *r=(RangeObj*)it->src.u.o; if(it->n<=0) return 0; *out=v_int(it->i); it->i+=r->step; it->n--; return 1; }
            case IT_DICTK: case IT_DICTV: case IT_DICTI:{
                DictObj *d=AS_DICT(it->src);
                if(it->aux.k==V_UNDEF){ it->aux=v_int(d->used); }
                else if(it->aux.u.i!=d->used){ it->i=d->nent; mp_raise_t(E_RuntimeError,"dictionary changed size during iteration"); }
                Value k, v;
                if(!mp_dict_next(d,&it->i,&k,&v)) return 0;
                if(it->kind==IT_DICTK) *out=k; else if(it->kind==IT_DICTV) *out=v; else { Value kv[2]={k,v}; *out=mp_tuple(2,kv); }
                return 1; }
            case IT_SET:{ SetObj *s=(SetObj*)it->src.u.o;
                if(it->aux.k==V_UNDEF) it->aux=v_int(s->used);
                else if(it->aux.u.i!=s->used){ it->i=s->mask+1; mp_raise_t(E_RuntimeError,"Set changed size during iteration"); }
                while(it->i<=s->mask){ SEnt *e=&s->table[it->i++]; if(e->key.k!=V_UNDEF){ *out=e->key; return 1; } }
                return 0; }
            case IT_REVLIST:{
                if(it->i<0) return 0;
                if(IS(it->src,T_list)){ ListObj *l=AS_LIST(it->src); if(it->i>=l->len){ it->i=-1; return 0; } *out=l->items[it->i--]; return 1; }
                if(IS(it->src,T_tuple)){ TupleObj *tp=AS_TUPLE(it->src); *out=tp->items[it->i--]; return 1; }
                if(IS_STR(it->src)){ *out=str_at(AS_STR(it->src),it->i--); return 1; }
                *out=mp_getitem(it->src,v_int(it->i--)); return 1; }
            case IT_GETITEM:{
                Catch c; Value v;
                if(!CATCH_BEGIN(c)){ v=mp_getitem(it->src,v_int(it->i)); CATCH_END(c); }
                else { Value e=mp_catch_exc(&c); if(mp_isinstance(e,E_IndexError)||mp_isinstance(e,E_StopIteration)) return 0; mp_raise(e); }
                it->i++; *out=v; return 1; }
            default: return mp_native_iter_next(it,out);
        }
    }
    if(t->layout==LY_GEN){
        if(t!=T_generator) mp_raise_t(E_TypeError,"'%s' object is not an iterator",t->name->s);
        int done; Value r=mp_gen_send(itv,v_none(),&done);
        if(done) return 0;
        *out=r; return 1;
    }
    if(t==T_file) return mp_file_readline(itv,out);
    if(t->flags&TF_DUNDERS){
        Value m=mp_type_lookup_s(t,"__next__");
        if(m.k==V_UNDEF) mp_raise_t(E_TypeError,"'%s' object is not an iterator",t->name->s);
        Catch c; Value v;
        if(!CATCH_BEGIN(c)){ v=mp_call1(m,itv); CATCH_END(c); }
        else { Value e=mp_catch_exc(&c); if(mp_isinstance(e,E_StopIteration)) return 0; mp_raise(e); }
        *out=v; return 1;
    }
    mp_raise_t(E_TypeError,"'%s' object is not an iterator",t->name->s);
}
Value mp_list_of(Value v){
    if(IS(v,T_list)) return mp_list(AS_LIST(v)->len,AS_LIST(v)->items);
    if(IS(v,T_tuple)) return mp_list(AS_TUPLE(v)->len,AS_TUPLE(v)->items);
    Value l=mp_list(0,NULL), it=mp_iter(v), x;
    while(mp_next(it,&x)) mp_list_append(l,x);
    return l;
}
Value mp_list_slice(Value lv, int64_t a, int64_t b, int64_t s){
    ListObj *l=AS_LIST(lv);
    int64_t n= s>0 ? (a<b?(b-a-1)/s+1:0) : (a>b?(a-b-1)/(-s)+1:0);
    Value r=mp_list(n,NULL);
    for(int64_t i=0,j=a;i<n;i++,j+=s) AS_LIST(r)->items[i]=l->items[j];
    return r;
}
void mp_list_setslice(Value lv, Value sl, Value v){
    ListObj *l=AS_LIST(lv);
    Value src= (v.k==V_OBJ && v.u.o==lv.u.o) ? mp_list(l->len,l->items) : mp_list_of(v);
    ListObj *x=AS_LIST(src);
    int64_t a,b,s, n=mp_slice_indices(sl,l->len,&a,&b,&s);
    if(s==1){
        if(b<a) b=a;
        int64_t nl=l->len-(b-a)+x->len;
        if(nl>l->cap){ l->cap=nl+4; l->items=(Value*)xrealloc(l->items,sizeof(Value)*(size_t)l->cap); }
        memmove(l->items+a+x->len,l->items+b,sizeof(Value)*(size_t)(l->len-b));
        memcpy(l->items+a,x->items,sizeof(Value)*(size_t)x->len);
        l->len=nl; return;
    }
    if(x->len!=n) mp_raise_t(E_ValueError,"attempt to assign sequence of size %lld to extended slice of size %lld",(long long)x->len,(long long)n);
    for(int64_t i=0,j=a;i<n;i++,j+=s) l->items[j]=x->items[i];
}

/* ---------------------------------------------------------------- sorting: stable merge sort on < */
static int lt(Value a, Value b){
    if(IS_INTLIKE(a) && IS_INTLIKE(b)) return a.u.i<b.u.i;
    if(a.k==V_FLOAT && b.k==V_FLOAT) return a.u.f<b.u.f;
    if(IS(a,T_str) && IS(b,T_str)){ StrObj *x=AS_STR(a), *y=AS_STR(b); int64_t n=x->len<y->len?x->len:y->len; int c=memcmp(x->s,y->s,(size_t)n); return c<0 || (c==0 && x->len<y->len); }
    return mp_truth(mp_compare(OP_Lt,a,b));
}
static void merge_sort(Value *v, Value *k, Value *tv, Value *tk, int64_t n, int rev){
    if(n<2) return;
    if(n<=8){                                     /* insertion sort */
        for(int64_t i=1;i<n;i++){
            Value x=v[i], xk=k[i]; int64_t j=i;
            while(j>0 && (rev ? lt(k[j-1],xk) : lt(xk,k[j-1]))){ v[j]=v[j-1]; k[j]=k[j-1]; j--; }
            v[j]=x; k[j]=xk;
        }
        return;
    }
    int64_t m=n/2;
    merge_sort(v,k,tv,tk,m,rev); merge_sort(v+m,k+m,tv,tk,n-m,rev);
    if(!(rev ? lt(k[m-1],k[m]) : lt(k[m],k[m-1]))) return;   /* already in order */
    memcpy(tv,v,sizeof(Value)*(size_t)m); memcpy(tk,k,sizeof(Value)*(size_t)m);
    int64_t i=0, j=m, o=0;
    while(i<m && j<n){
        if(rev ? lt(tk[i],k[j]) : lt(k[j],tk[i])){ v[o]=v[j]; k[o]=k[j]; j++; }
        else { v[o]=tv[i]; k[o]=tk[i]; i++; }
        o++;
    }
    while(i<m){ v[o]=tv[i]; k[o]=tk[i]; i++; o++; }
}
int mp_sort(Value lv, Value key, int reverse){
    ListObj *l=AS_LIST(lv);
    int64_t n=l->len;
    if(n<2) return 0;
    /* sort a copy (a key function may change the list), every array a collected list */
    Value vals=mp_list(n,l->items), keys=mp_list(n,NULL), tv=mp_list(n,NULL), tk=mp_list(n,NULL);
    for(int64_t i=0;i<n;i++) AS_LIST(keys)->items[i]= key.k==V_UNDEF||IS_NONE(key) ? l->items[i] : mp_call1(key,l->items[i]);
    merge_sort(AS_LIST(vals)->items,AS_LIST(keys)->items,AS_LIST(tv)->items,AS_LIST(tk)->items,n,reverse);
    if(l->len!=n) mp_raise_t(E_ValueError,"list modified during sort");
    memcpy(l->items,AS_LIST(vals)->items,sizeof(Value)*(size_t)n);
    return 0;
}
