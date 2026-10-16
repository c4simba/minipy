/* ========================= Typed compiler, code generator: conversions =========================
   Owned and borrowed results, conversions between numeric types,
   function values adapted to another signature. */

#include "gen.h"

Ty *gpart(Ty *g, int i){ Ty *t=aot_gen_part(g,i); return t->k==TY_VAR ? TY_VOID_T : t; }   /* a generator's send / return type (void: None) */

const char *cg_path="?";        /* source of the function being generated (messages) */
PPY_NORETURN void cg_fail(int line, const char *msg){
    fprintf(stderr,"%s:%d: error: %s\n",cg_path,line,msg); exit(1);
}
/* The variable a name stores to inside the current function. */
AVar *find_var(F *f, const char *name, int line){
    AFunc *fn=f->fn; ASym *s=NULL;
    if(is_fn_body(fn)){
        int global=0;
        for(int i=0;i<fn->nglobals_decl;i++) if(!strcmp(fn->globals_decl[i],name)) global=1;
        if(!global) s=symtab_find(&fn->locals,name);
        if(!global && !s) for(int i=0;i<fn->ncaps;i++) if(!strcmp(fn->caps[i]->name,name)) return fn->caps[i];   /* nonlocal */
    }
    if(!s) s=symtab_find(&fn->mod->syms,name);
    if(!s || s->kind!=AS_VAR) cg_fail(line,"internal error: unknown variable");
    return (AVar*)s->p;
}

/* reference results: make owned / borrowed (owned temporaries are held) */
void gen_owned(F *f, Expr *e){ if(try_move(f,e)) return; int o=gen(f,e); if(is_ptr(TY(e)) && !o) incref(f); }
void gen_borrow(F *f, Expr *e){ int o=gen(f,e); if(is_ptr(TY(e)) && o) hold(f); }
/* convert eax (int/bool) to st0 when a float is wanted */
void conv_num(F *f, Ty *from, Ty *to){
    from=ty_find(from); to=ty_find(to);
    if(to->k==TY_FLOAT && from->k==TY_INT){ E(f,"push edx"); E(f,"push eax"); E(f,"fild qword [esp]"); E(f,"add esp,8"); }
    else if(to->k==TY_FLOAT && from->k==TY_BOOL){ E(f,"push eax"); E(f,"fild dword [esp]"); E(f,"add esp,4"); }
    else if(to->k==TY_INT && from->k==TY_BOOL) E(f,"xor edx,edx");
}
/* A function taking *args where a fixed signature is wanted: an adapter
   closure AD<k> (+16 the function) packs the extra arguments into the tuple. */
static int adapt_needed(Ty *from, Ty *to){ from=ty_find(from); to=ty_find(to); return from->k==TY_FUNC && to->k==TY_FUNC && !to->tup && (from->tup&1); }
static void conv_func(F *f, Ty *from, Ty *to, int owned){
    if(!adapt_needed(from,to)) return;
    int k=adapter_index(ty_find(to),ty_find(from)), l=new_label();
    E(f,"test eax,eax"); E(f,"jz L%d",l);
    E(f,"push eax"); E(f,"mov eax,20"); CALLRT(f,"rt_alloc");
    E(f,"mov dword [eax],1"); E(f,"mov dword [eax+4],rt_adfree"); rt("rt_adfree"); E(f,"mov dword [eax+8],AD%d",k);
    if(gg->gc_func) E(f,"or byte [eax-3],1");
    E(f,"pop ecx"); E(f,"mov [eax+16],ecx"); E(f,"mov edx,[ecx+12]"); E(f,"mov [eax+12],edx");
    if(!owned){ E(f,"push eax"); E(f,"mov eax,ecx"); CALLRT(f,"rt_incref"); E(f,"pop eax"); }   /* an owned one moves into the adapter */
    LBL(f,l);
    if(!owned) hold(f);
}
/* the value in eax/st0 (owned) as a `to` */
/* an int / float (eax:edx / st0) where a complex is wanted: complex(x, 0.0), owned */
static void conv_complex(F *f, Ty *from, Ty *to){
    from=ty_find(from); to=ty_find(to);
    if(!aot_is_complex(to) || (from->k!=TY_INT && from->k!=TY_BOOL && from->k!=TY_FLOAT)) return;
    conv_num(f,from,TY_FLOAT_T);
    AClass *k=ty_find(to)->cls; use_class(k);
    AField *re=aot_find_field(k,"real"), *im=aot_find_field(k,"imag");
    E(f,"sub esp,8"); E(f,"fstp qword [esp]");
    E(f,"mov eax,%d",k->size); E(f,"mov edx,%s",class_label(k,0)); E(f,"mov ecx,%s",class_label(k,1)); CALLRT(f,"rt_obj_new");
    E(f,"fld qword [esp]"); E(f,"fstp qword [eax+%d]",re->offset); E(f,"add esp,8");
    E(f,"mov dword [eax+%d],0",im->offset); E(f,"mov dword [eax+%d],0",im->offset+4);
}
void conv(F *f, Ty *from, Ty *to){ if(aot_is_complex(to) && !aot_is_complex(from)){ conv_complex(f,from,to); return; } conv_num(f,from,to); conv_func(f,from,to,1); }
static int is_endpoint_ty(Ty *t){ t=ty_find(t); return t->k==TY_FUNC && (t->tup&4); }
void gen_as(F *f, Expr *e, Ty *want, int owned){
    if(e->kind==EXPR_NONE){ gen_none(f,want); return; }
    if(is_endpoint_ty(want) && !is_endpoint_ty(TY(e))){                /* a function as a plainpy.Endpoint: its adapter */
        XInfo *x=xinfo(e);
        if(x->kind!=X_FUNCREF || !x->fn || !x->fn->ep_adapter) cg_fail(e->line,"a plainpy.Endpoint is made from a function given by its name");
        AFunc *ad=x->fn->ep_adapter; use_fn(ad); gg->fv_used[ad->id]=1;
        E(f,"mov eax,%s",fnl(ad,LB_VALUE));
        return;
    }
    const char *given=g_none_text;                                           /* (e's own code may set it) */
    char *keep= given && is_opt(TY(e)) ? xstrdup2(given) : NULL;
    if(owned) gen_owned(f,e); else gen_borrow(f,e);
    Ty *w=ty_find(want);
    if(is_opt(TY(e)) && !w->opt && w->k!=TY_VAR && w->k!=TY_VOID){          /* None where a value is needed */
        const char *msg=keep;
        if(!msg) msg = w->k==TY_INT||w->k==TY_BOOL ? "'NoneType' object cannot be interpreted as an integer"
                     : w->k==TY_FLOAT ? "must be real number, not NoneType"
                     : none_msg("expected %s, got NoneType",py_type_name(w));
        check_none(f,TY(e),g_none_exc,msg);
    }
    if(aot_is_complex(want) && !aot_is_complex(TY(e)) && ty_find(TY(e))->k!=TY_OBJ){   /* 1 as a complex: a new object */
        conv_complex(f,TY(e),want); if(!owned) hold(f); return; }
    conv_num(f,TY(e),want); conv_func(f,TY(e),want,owned);
}

