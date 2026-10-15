/* ========================= Typed compiler, code generator: expressions =========================
   Closures and functions as values, constants, and the expression
   dispatcher (gen). */

#include "gen.h"

static int gen_closure(F *f, AFunc *g){
    layout_caps(g); use_fn(g); gg->cd_used[g->id]=1;
    E(f,"mov eax,%d",16+g->defsize+g->capsize); CALLRT(f,"rt_alloc");
    E(f,"mov dword [eax],1"); E(f,"mov dword [eax+4],%s",fnl(g,LB_FREE)); E(f,"mov dword [eax+8],%s",fnl(g,LB_CODE)); E(f,"mov dword [eax+12],S%d",str_lit(fn_display_name(g)));
    if(gg->gc_clo[g->id]) E(f,"or byte [eax-3],1");          /* it can be in a cycle (rt_gc_root) */
    E(f,"push eax");
    fill_defaults(f,g,NULL);                                /* its defaults, evaluated now */
    for(int k=0;k<g->ncaps;k++){ AVar *cv=g->caps[k], *src=cv->src;
        if(root_var(cv)->cell){                             /* share the cell */
            if(src->src){ layout_caps(src->owner); E(f,"mov ecx,[ebp%+d]",f->env); E(f,"mov eax,[ecx+%d]",src->capoff); }
            else E(f,"mov eax,[ebp%+d]",src->offset);
            incref(f); E(f,"mov ecx,[esp]"); E(f,"mov [ecx+%d],eax",cv->capoff);
        } else {
            load_var(f,src); if(is_ptr(cv->ty)) incref(f);
            E(f,"mov ecx,[esp]"); store_new(f,cv->ty,"ecx",cv->capoff);
        }
    }
    E(f,"pop eax");
    return 1;
}
/* g's default slots in its function object ([esp], or the static object lbl): its defaults, evaluated here */
void fill_defaults(F *f, AFunc *g, const char *lbl){
    layout_defaults(g);
    for(int i=0;i<g->nparams;i++){ Expr *d=g->defaults[i]; if(!d || i==g->star || i==g->dstar) continue;
        if(i<32 && (g->defaults_mismatch>>i&1)) continue;        /* (a generic copy's constant default of another type: calls put it in) */
        if(!d->ty && g->origin && i<g->origin->nparams && g->origin->defaults[i] && g->origin->defaults[i]->ty) d=g->origin->defaults[i];   /* (a copy's unchecked one: its origin's) */
        if(!d->ty) continue;
        Ty *pt=g->params[i]->ty;
        gen_as(f,d,pt,1);
        if(lbl) E(f,"mov ecx,%s",lbl); else E(f,"mov ecx,[esp]");
        store_mem(f,pt,"ecx",16+g->defoff[i]); }
}
/* function g as a value -> eax */
int gen_funcref(F *f, AFunc *g){
    use_fn(g);
    if(!g->ncaps && !dyn_defaults(g)){ gg->fv_used[g->id]=1; rt("rt_static");    /* nothing captured: a static object */
        if(has_defaults(g)) fill_defaults(f,g,fnl(g,LB_VALUE));
        E(f,"mov eax,%s",fnl(g,LB_VALUE)); return 0; }
    if(g==f->fn){ E(f,"mov eax,[ebp%+d]",f->env); return 0; }                                    /* a nested def naming itself */
    return gen_closure(f,g);
}
/* the constant n as a value of type t (int: edx:eax) */
void gen_int_const(F *f, Ty *t, int64_t n){
    uint32_t lo=(uint32_t)n, hi=(uint32_t)((uint64_t)n>>32);
    if(lo==0) E(f,"xor eax,eax"); else E(f,"mov eax,%u",lo);
    if(is_lng(t)){ if(hi==0) E(f,"xor edx,edx"); else if(hi==0xFFFFFFFFu) E(f,"or edx,-1"); else E(f,"mov edx,%u",hi); }
}
int gen(F *f, Expr *e){
    XInfo *xi=xinfo(e); Ty *t=TY(e);
    switch(e->kind){
        case EXPR_LITERAL:{
            Tok *tk=e->tok;
            if(tk->kind==T_STRING && tk->i==2){ E(f,"mov eax,B%d",bytes_lit(tk->text,(int)tk->len)); return 0; }
            if(tk->kind==T_STRING){ E(f,"mov eax,S%d",str_litn(tk->text,(int)tk->len)); return 0; }      /* "" too: a str (0 is None) */
            if(tk->is_float){ double d=tk->f; if(d==0.0) E(f,"fldz"); else if(d==1.0) E(f,"fld1"); else E(f,"fld qword [FC%d]",float_const(tk->text)); return 0; }
            if(t->k==TY_FLOAT){ char num[32]; snprintf(num,sizeof num,"%lld",(long long)tk->i);
                if(tk->i==0) E(f,"fldz"); else if(tk->i==1) E(f,"fld1"); else E(f,"fld qword [FC%d]",float_const(num)); return 0; }
            gen_int_const(f,t,tk->i);
            return 0; }
        case EXPR_TRUE: E(f,"mov eax,1"); return 0;
        case EXPR_FALSE: E(f,"xor eax,eax"); return 0;
        case EXPR_NONE: gen_none(f,t); return 0;
        case EXPR_NAME:
            if(xi->kind==X_CONST_STR){ E(f,"mov eax,S%d",str_lit(xi->name)); return 0; }
            if(xi->kind==X_FUNCREF) return gen_funcref(f,xi->fn);
            if(xi->kind==X_TYPEVAL){ E(f,"mov eax,%s",typeval_label(xi)); return 0; }
            load_var(f,xi->var); return 0;
        case EXPR_LAMBDA: return gen_funcref(f,xi->fn);
        case EXPR_YIELD:{                                 /* (yield v): what send() gives; (yield from g): what g returns */
            Ty *yt=f->fn->yield_ty;
            if(e->akind==7){
                if(ty_find(TY(e->a))->k==TY_GEN) return gen_yield_from(f,e->a,ty_find(t)->k!=TY_VOID);
                Iter I; iter_begin(f,e->a,&I);
                iter_value(f,&I,0); if(is_ptr(I.elem)) incref(f); conv(f,I.elem,yt);
                gen_yield_value(f,yt); drop_sent(f);
                iter_end(f,&I); LBL(f,I.exit); iter_release(f,&I);
                return 0; }
            if(e->a) gen_as(f,e->a,yt,1); else { int o=gen_zero(f,yt); if(is_ptr(yt)&&!o) incref(f); }
            gen_yield_value(f,yt);
            return take_sent(f,gpart(f->fn->ret,0)); }
        case EXPR_UNARY:
            if(e->op==T_NOT){ gen_bool(f,e->a); E(f,"xor eax,1"); return 0; }
            if(xi->kind==X_OPMETHOD) return gen_op_call(f,xi->fn,e->a,NULL);
            if(e->op==T_MINUS && is_lng(t) && e->a->kind==EXPR_LITERAL && !e->a->tok->is_float && e->a->tok->kind==T_NUMBER){   /* -5 */
                gen_int_const(f,t,-e->a->tok->i); return 0; }
            g_none_text=none_msg("bad operand type for unary %s: 'NoneType'",e->op==T_MINUS?"-":e->op==T_PLUS?"+":"~");
            gen_as(f,e->a,t,0); g_none_text=NULL;
            if(e->op==T_PLUS) return 0;
            if(e->op==T_MINUS){ if(is_flt(t)) E(f,"fchs"); else if(is_lng(t)) CALLRT(f,"rt_ineg"); else E(f,"neg eax"); }
            else { E(f,"not eax"); if(is_lng(t)) E(f,"not edx"); }
            return 0;
        case EXPR_BINARY: return gen_binary(f,e);
        case EXPR_BOOL: return gen_boolop(f,e);
        case EXPR_COMPARE: gen_compare(f,e); return 0;
        case EXPR_TERNARY:{
            int lelse=new_label(), lend=new_label();
            gen_jump(f,e->a,lelse,0);
            gen_as(f,e->b,t,1); E(f,"jmp L%d",lend);
            LBL(f,lelse); gen_as(f,e->c,t,1);
            LBL(f,lend);
            return 1; }
        case EXPR_CALL: return gen_call(f,e);
        case EXPR_ATTRIBUTE:
            if(xi->kind==X_CONST_STR){ E(f,"mov eax,S%d",str_lit(xi->name)); return 0; }
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type")){ gen_type_of(f,e->a); return 0; }        /* x.__class__ */
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"class_mro")){            /* C.__mro__ */
                AClass *k=xi->cls, *ks[64]; int n=0;
                if(k->mro) for(int i=0;i<k->nmro && n<64;i++) ks[n++]=k->mro[i]; else for(AClass *x=k;x && n<64;x=x->base) ks[n++]=x;
                Ty *el=ty_find(t)->elem; new_list(f,el); E(f,"push eax");
                for(int i=0;i<=n;i++){ if(i<n) use_class(ks[i]); E(f,"mov eax,%s",i<n?class_label(ks[i],0):btype_label("object")); list_append_top(f,el); }
                E(f,"pop eax"); return 1; }
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type_qualname") && xinfo(e->a)->kind==X_TYPEVAL && xinfo(e->a)->cls){   /* C.__qualname__: Outer.Inner */
                E(f,"mov eax,S%d",str_lit(xinfo(e->a)->cls->qualname)); return 0; }
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type_qualname")){ gen(f,e->a); CALLRT(f,"rt_type_qualname"); return 0; }   /* type(x).__qualname__ */
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type_name")){ gen(f,e->a); E(f,"mov eax,[eax]"); return 0; }   /* C.__name__ (static) */
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"type_module")){ gen(f,e->a); E(f,"mov eax,[eax-8]"); return 0; }   /* C.__module__ (static) */
            if(xi->kind==X_BUILTIN && !strcmp(xi->name,"exc_args")){            /* e.args: (message,) or () */
                AField *fm=aot_find_field(ty_find(TY(e->a))->cls,"_msg"); int l=new_label();
                gen_borrow(f,e->a); E(f,"push dword [eax+%d]",fm->offset);
                new_list(f,TY_STR_T); E(f,"pop ecx"); E(f,"test ecx,ecx"); E(f,"jz L%d",l);
                E(f,"push eax");
                if(is_keyerror(ty_find(TY(e->a))->cls)){ E(f,"mov eax,ecx"); CALLRT(f,"rt_str_unquote"); E(f,"mov ecx,eax"); }   /* (a KeyError keeps repr(key)) */
                else E(f,"inc dword [ecx]");
                E(f,"push ecx"); E(f,"mov eax,[esp+4]"); E(f,"call rt_list_push"); E(f,"pop ecx"); E(f,"mov [eax],ecx"); E(f,"pop eax"); rt("rt_list_push");
                LBL(f,l); return 1; }
            if(xi->kind==X_BMOD && (!strcmp(xi->name,"inf")||!strcmp(xi->name,"nan"))){ rt("rt_finf"); E(f,"fld qword [%s]",xi->name[0]=='i'?"rt_finf":"rt_fnan"); return 0; }
            if(xi->kind==X_BMOD){                         /* math.pi / math.e / math.tau */
                E(f,"fld qword [FC%d]",float_const(!strcmp(xi->name,"pi")?"3.141592653589793":!strcmp(xi->name,"tau")?"6.283185307179586":"2.718281828459045"));
                return 0; }
            if(xi->kind==X_VAR){ load_var(f,xi->var); return 0; }
            if(xi->kind==X_FUNCREF) return gen_funcref(f,xi->fn);
            if(xi->kind==X_TYPEVAL){ E(f,"mov eax,%s",typeval_label(xi)); return 0; }      /* Outer.Inner */
            if(xi->kind==X_BOUND){                        /* obj.method: a closure of obj (+16) */
                AFunc *m=xi->fn; use_fn(m); gg->bm_used[m->id]=1;
                gen_owned(f,e->a); attr_check_of(f,e->a,e->name); E(f,"push eax");
                E(f,"mov eax,20"); CALLRT(f,"rt_alloc");
                E(f,"mov dword [eax],1"); E(f,"mov dword [eax+4],rt_adfree"); rt("rt_adfree"); E(f,"mov dword [eax+8],%s",fnl(m,LB_BOUND)); E(f,"mov dword [eax+12],S%d",str_lit(m->name));
                if(gg->gc_func && gc_type(TY(e->a))) E(f,"or byte [eax-3],1");   /* it and its object can be in a cycle */
                E(f,"pop ecx"); E(f,"mov [eax+16],ecx");
                return 1; }
            if(xi->kind==X_PROP){ E(f,"sub esp,4"); gen_borrow(f,e->a); E(f,"mov [esp],eax"); call_on_top(f,xi->fn); E(f,"add esp,4"); return is_ptr(xi->fn->ret); }
            if(xi->kind==X_CLASSCONST){ gen_as(f,xi->field->init,xi->field->ty,0); return 0; }
            gen_borrow(f,e->a); attr_check_of(f,e->a,e->name);
            load_field(f,xi->field);
            return 0;
        case EXPR_INDEX: return gen_index(f,e);
        case EXPR_SLICE: return gen_slice(f,e);
        case EXPR_DICT: if(t->k==TY_TUPLE) return gen_tuple(f,e); return gen_literal_container(f,e);
        case EXPR_LIST: case EXPR_SET: return gen_literal_container(f,e);
        case EXPR_TUPLE:{ int k=const_tuple(e,t); if(k>=0){ E(f,"mov eax,CT%d",k); return 0; }   /* (static: borrowed) */
            return t->k==TY_TUPLE ? gen_tuple(f,e) : gen_literal_container(f,e); }
        case EXPR_COMPREHENSION:
            if(e->comp_kind=='G'){                       /* a generator expression: call its generator function with the first iterable */
                AFunc *g=xi->fn; use_fn(g);
                if(g->nparams){ Ty *pt=g->params[0]->ty; E(f,"sub esp,%d",esize(pt)); gen_as(f,e->clauses[0].iter,pt,0); put_arg(f,pt,0); }
                if(g->ncaps){ gen_closure(f,g); hold(f); E(f,"mov edx,eax"); }
                E(f,"call %s",fnl(g,LB_CODE));
                if(g->nparams) E(f,"add esp,%d",esize(g->params[0]->ty));
                return 1;
            }
            return gen_comprehension(f,e);
        case EXPR_AWAIT: return gen_await(f,e);
        case EXPR_WALRUS:{                                       /* name := value: stored, and a reference kept as the result */
            AVar *v=xi->var;
            gen_as(f,e->a,v->ty,1);
            if(is_flt(v->ty)){ E(f,"fld st0"); store_var(f,v); return 0; }
            if(is_ptr(v->ty)){ incref(f); E(f,"push eax"); store_var(f,v); E(f,"pop eax"); return 1; }
            store_var(f,v); return 0; }
        default: cg_fail(e->line,"unsupported expression");
    }
}

/* None as a value of type t (returns 0: nothing owned) */
int gen_zero(F *f, Ty *t){ gen_none(f,t); return 0; }

