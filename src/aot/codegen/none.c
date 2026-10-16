/* ========================= Typed compiler, code generator: None =========================
   None as a value of each type (a null reference; int -2**63, a float
   NaN of its own, bool 2), the checks for it and their messages; built-in
   type descriptors. */

#include "gen.h"

/* ---- None: a null reference; an int -2**63, a float a NaN of its own, a bool 2 ---- */
int is_opt(Ty *t){ return ty_opt(t); }

void gen_none(F *f, Ty *t){
    t=ty_find(t);
    if(is_flt(t)){ rt("rt_fnone"); E(f,"fld qword [rt_fnone]"); }
    else if(is_lng(t)){ E(f,"xor eax,eax"); E(f,"mov edx," NONE_HI_INT); }
    else if(t->k==TY_BOOL) E(f,"mov eax,2");
    else if(t->k!=TY_VOID) E(f,"xor eax,eax");
}
/* jump to `label` when the value in eax / edx:eax / st0 (of type t) is None; the value stays */
void jump_if_none(F *f, Ty *t, int label){
    t=ty_find(t);
    if(is_flt(t)){ int l=new_label(); rt("rt_fnone");
        E(f,"sub esp,8"); E(f,"fst qword [esp]"); E(f,"mov ecx,[esp+4]"); E(f,"cmp ecx,[rt_fnone+4]"); E(f,"jne L%d",l);
        E(f,"mov ecx,[esp]"); E(f,"cmp ecx,[rt_fnone]"); LBL(f,l); E(f,"lea esp,[esp+8]"); E(f,"je L%d",label); }
    else if(is_lng(t)){ int l=new_label(); E(f,"cmp edx," NONE_HI_INT); E(f,"jne L%d",l); E(f,"test eax,eax"); E(f,"jz L%d",label); LBL(f,l); }
    else if(t->k==TY_BOOL){ E(f,"cmp eax,2"); E(f,"je L%d",label); }
    else { E(f,"test eax,eax"); E(f,"jz L%d",label); }
}
/* exc(msg) when the value of type t is None (only for optional types) */
void check_none(F *f, Ty *t, const char *exc, const char *msg){
    if(!is_opt(t)) return;
    if(!msg) msg="unsupported operand type: 'NoneType'";
    int lnone=new_label(), lok=new_label();
    jump_if_none(f,t,lnone); E(f,"jmp L%d",lok);
    LBL(f,lnone);
    E(f,"mov esi,Z%d",zlit(msg));
    CALLRT(f,!strcmp(exc,"AttributeError")?"rt_attrerr_text":"rt_typeerr_text");
    LBL(f,lok);
}
/* the descriptor of a built-in type (name, base): TYD_<name>, emitted at the end */
const char *btype_label(const char *name){
    static char buf[32][48];
    int i=0;
    for(;i<gg->nbtypes;i++) if(!strcmp(gg->btypes[i],name)) break;
    if(i==gg->nbtypes){ if(i==32) cg_fail(0,"too many built-in types as values"); gg->btypes[gg->nbtypes++]=xstrdup2(name); }
    if(!strcmp(name,"bool")) btype_label("int");
    snprintf(buf[i],sizeof buf[i],"TYD_%s",name); return buf[i];
}
/* a class as a value (X_TYPEVAL): its descriptor */
const char *typeval_label(XInfo *xi){
    if(xi->cls){ use_class(xi->cls); return class_label(xi->cls,0); }
    return btype_label(xi->name);
}
/* whether values of static type t are instances of the built-in type name (None aside) */
int btype_match(Ty *t, const char *name){
    t=ty_find(t);
    if(!strcmp(name,"object")) return 1;
    switch(t->k){
        case TY_INT: return !strcmp(name,"int");
        case TY_BOOL: return !strcmp(name,"bool")||!strcmp(name,"int");
        case TY_FLOAT: return !strcmp(name,"float");
        case TY_STR: return !strcmp(name,"str");
        case TY_BYTES: return !strcmp(name,"bytes");
        case TY_LIST: return !strcmp(name,t->tup?"tuple":"list");
        case TY_TUPLE: return !strcmp(name,t->names?"dict":"tuple");
        case TY_DICT: return !strcmp(name,"dict");
        case TY_SET: return !strcmp(name,t->tup?"frozenset":"set");
        case TY_TYPE: return !strcmp(name,"type");
        default: return 0;
    }
}
/* the Python name of t's values, for messages */
const char *py_type_name(Ty *t){
    t=ty_find(t);
    switch(t->k){ case TY_INT: return "int"; case TY_BOOL: return "bool"; case TY_FLOAT: return "float"; case TY_STR: return "str"; case TY_BYTES: return "bytes"; case TY_TYPE: return "type";
        case TY_LIST: return t->tup?"tuple":"list"; case TY_DICT: return "dict"; case TY_SET: return t->tup?"frozenset":"set"; case TY_TUPLE: return t->names?"dict":"tuple";
        case TY_OBJ: return t->cls->name; case TY_FUNC: return "function"; case TY_GEN: return "generator"; case TY_FILE: return "TextIOWrapper";
        case TY_BUF: return "buffer"; case TY_TASK: return "Task"; default: return "NoneType"; }
}
static char none_msg_buf[8][256]; static int none_msg_k;
const char *none_msg(const char *fmt, ...){
    char *b=none_msg_buf[none_msg_k++&7]; va_list ap; va_start(ap,fmt); vsnprintf(b,256,fmt,ap); va_end(ap); return b;
}
/* what a check of a value going where None is not allowed says (set around the operations below) */
const char *g_none_exc="TypeError", *g_none_text=NULL;
void push_value(F *f, Ty *t){ if(is_flt(t)){ E(f,"sub esp,8"); E(f,"fstp qword [esp]"); } else { if(is_lng(t)) E(f,"push edx"); E(f,"push eax"); } }
void pop_value(F *f, Ty *t){ if(is_flt(t)){ E(f,"fld qword [esp]"); E(f,"add esp,8"); } else { E(f,"pop eax"); if(is_lng(t)) E(f,"pop edx"); } }
/* an int in edx:eax as 32 bits (an index, a count): the nearest such */
void to_i32(F *f, Ty *t){ if(is_lng(t)) CALLRT(f,"rt_i32sat"); }
/* a 32-bit result (eax) as an int: sign- or zero-extended */
void widen(F *f, int is_signed){ if(is_signed) E(f,"cdq"); else E(f,"xor edx,edx"); }
void drop_value(F *f, Ty *t, int owned){
    t=ty_find(t);
    if(t->k==TY_VOID||t->k==TY_VAR) return;
    if(is_flt(t)) E(f,"fstp st0");
    else if(is_ptr(t) && owned){ rt("rt_decref"); E(f,"call rt_decref"); }
}
/* AttributeError for x.name when x (in eax) is None */
void attr_check(F *f, const char *name){
    int z=zlit(name);                                 /* (rt_attrerr_none makes the message of the name) */
    rt("rt_attrerr_none");
    E(f,"test eax,eax"); E(f,"jz NA%d",z);           /* one NA<z> per name, shared (emitted with the program) */
    if(z>=gg->na_cap){ int c=gg->na_cap?gg->na_cap:64; while(c<=z) c*=2;
        gg->na_mark=(unsigned char*)xrealloc(gg->na_mark,(size_t)c); memset(gg->na_mark+gg->na_cap,0,(size_t)(c-gg->na_cap)); gg->na_cap=c; }
    if(!gg->na_mark[z]){ gg->na_mark[z]=1;
        if(gg->nnas==gg->cnas){ gg->cnas=gg->cnas?gg->cnas*2:64; gg->nas=(int*)xrealloc(gg->nas,sizeof(int)*(size_t)gg->cnas); }
        gg->nas[gg->nnas++]=z; }
}
void panic_if_null(F *f){ rt("rt_panic_none"); E(f,"test eax,eax"); E(f,"jz rt_panic_none"); }
/* A method whose self is never None: every way it is called checks the
   object (obj.m(), super().m() from a method without this; a constructor
   makes it) - so its body does not. Not one called as Class.m(x), nor the
   ones the runtime calls (operators, __str__ ...), properties, decorated,
   async or generator methods, nor one used as a value. */
int self_nonnull(AFunc *fn){
    return fn && fn->cls && fn->def && !fn->is_static && !fn->is_clsm && fn->nparams>0 && !fn->value_used && !fn->ndeco && !fn->unbound_called
        && !fn->is_property && !fn->is_async && !fn->is_gen && !fn->outer && !fn->lam && !fn->genexp
        && (strncmp(fn->name,"__",2) || !strcmp(fn->name,"__init__"))
        && !fn->params[0]->nbind && !fn->params[0]->cell && !getenv("PPY_SELF_CHECKS");
}
int known_object(F *f, Expr *x){              /* x is self of such a method */
    if(!x || x->kind!=EXPR_NAME || !self_nonnull(f->fn)) return 0;
    XInfo *xi=(XInfo*)x->ty; return xi && xi->kind==X_VAR && xi->var==f->fn->params[0];
}
/* attr_check of obj.name, unless obj cannot be None */
void attr_check_of(F *f, Expr *obj, const char *name){ if(!known_object(f,obj)) attr_check(f,name); }
void incref(F *f){ CALLRT(f,"rt_incref"); }

