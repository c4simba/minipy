/* ========================= Typed compiler, code generator: calls =========================
   Arguments (*args, **kwargs, defaults), calls of functions, methods,
   function values and constructors; the call dispatcher (gen_call). */

#include "gen.h"

/* ---- calls ---- */
void put_arg(F *f, Ty *pt, int off){ if(is_flt(pt)) E(f,"fstp qword [esp+%d]",off); else { E(f,"mov [esp+%d],eax",off); if(is_lng(pt)) E(f,"mov [esp+%d],edx",off+4); } }
static int in_list(int *v, int n, int x){ for(int i=0;i<n;i++) if(v[i]==x) return 1; return 0; }
/* *args of a call: a new tuple (list) of the extra positionals and f(*xs) -> eax (held) */
static void gen_star_args(F *f, AFunc *fn, Expr *e, XInfo *xi){
    Ty *el=ty_find(fn->params[fn->star]->ty)->elem;
    new_list(f,el); E(f,"push eax");
    for(int j=0;j<xi->nxargs;j++){ gen_as(f,e->items[xi->xargs[j]],el,1); list_append_top(f,el); }
    if(xi->splat){
        Expr *sa=e->items[xi->splat-1]; Ty *st=TY(sa);
        if(st->k==TY_LIST && kd_of(el)==kd_of(st->elem)){ gen_borrow(f,sa); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_list_extend"); }
        else if(st->k==TY_LIST) cg_fail(e->line,"f(*list of int) into *args of float is not supported");
        else {
            int slot=frame_slot(f,4); gen_borrow(f,sa); panic_if_null(f); E(f,"mov [ebp%+d],eax",slot);
            for(int k=0;k<st->nelems;k++){ Ty *et=st->elems[k];
                E(f,"mov eax,[ebp%+d]",slot); load_mem(f,et,"eax",tuple_off(st,k)); if(is_ptr(et)) incref(f); conv(f,et,el); list_append_top(f,el); }
        }
    }
    E(f,"pop eax"); hold(f);
}
/* **kwargs of a call: a new dict of the extra keywords and f(**d) -> eax (held) */
static void gen_kwargs(F *f, AFunc *fn, Expr *e, XInfo *xi){
    Ty *dt=ty_find(fn->params[fn->dstar]->ty), *el=dt->elem;
    new_dict(f,dt); E(f,"push eax");
    for(int j=0;j<xi->nkwargs;j++){ Expr *a=e->items[xi->kwargs[j]];
        gen_as(f,a,el,1); push_value(f,el);
        E(f,"push S%d",str_lit(a->kw)); E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",4+esize(el)); CALLRT(f,"rt_dict_slot"); E(f,"add esp,4");
        E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0); }
    if(xi->dsplat){ gen_borrow(f,e->items[xi->dsplat-1]); E(f,"mov edx,eax"); E(f,"mov eax,[esp]"); CALLRT(f,"rt_dict_update"); }
    E(f,"pop eax"); hold(f);
}
/* The arguments of a call of fn (parameters from `first` on) into [esp+off[i]],
   in source order: positionals, keywords, f(*seq) spread, *args, **kwargs, defaults. */
static void gen_call_args(F *f, AFunc *fn, Expr *e, XInfo *xi, int *off, int first){
    int done_star=fn->star<0, done_dstar=fn->dstar<0;
    for(int ai=0;ai<e->count;ai++){
        Expr *a=e->items[ai]; int mapped=0;
        if(xi->emptysplat==ai+1){ gen_borrow(f,a); E(f,"xor edx,edx"); CALLRT(f,"rt_unpack_check"); continue; }   /* f(*xs) with nothing left to fill */
        for(int i=first;i<fn->nparams;i++) if(xi->argmap[i]==ai) mapped++;
        if(mapped && a->akind==1){                      /* f(*seq) over parameters */
            Ty *st=TY(a); int slot=frame_slot(f,4);
            gen_borrow(f,a);
            if(st->k==TY_LIST){ E(f,"mov edx,%d",mapped); CALLRT(f,"rt_unpack_check"); } else panic_if_null(f);
            E(f,"mov [ebp%+d],eax",slot);
            for(int i=first;i<fn->nparams;i++) if(xi->argmap[i]==ai){
                Ty *pt=fn->params[i]->ty; int k=xi->argelem[i]-1;
                E(f,"mov eax,[ebp%+d]",slot);
                if(st->k==TY_TUPLE){ load_mem(f,st->elems[k],"eax",tuple_off(st,k)); conv_num(f,st->elems[k],pt); }
                else { E(f,"mov eax,[eax+16]"); load_mem(f,st->elem,"eax",k*esize(st->elem)); conv_num(f,st->elem,pt); }
                if(fn->params[i]->consumed) incref(f);
                put_arg(f,pt,off[i]);
            }
            continue;
        }
        if(mapped){ for(int i=first;i<fn->nparams;i++) if(xi->argmap[i]==ai){ Ty *pt=fn->params[i]->ty; gen_as(f,a,pt,fn->params[i]->consumed); put_arg(f,pt,off[i]); } continue; }
        if(!done_star && (in_list(xi->xargs,xi->nxargs,ai) || xi->splat==ai+1)){ done_star=1; gen_star_args(f,fn,e,xi); put_arg(f,fn->params[fn->star]->ty,off[fn->star]); }
        if(!done_dstar && (in_list(xi->kwargs,xi->nkwargs,ai) || xi->dsplat==ai+1)){ done_dstar=1; gen_kwargs(f,fn,e,xi); put_arg(f,fn->params[fn->dstar]->ty,off[fn->dstar]); }
        else if(fn->dstar<0 && xi->dsplat==ai+1){          /* f(**d) without **kwargs: d must be empty */
            int l=new_label(); gen_borrow(f,a); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"cmp dword [eax+8],0"); E(f,"je L%d",l);
            E(f,"mov esi,Z%d",zlit("got unexpected keyword arguments")); CALLRT(f,"rt_panic_type"); LBL(f,l); }
    }
    if(!done_star){ gen_star_args(f,fn,e,xi); put_arg(f,fn->params[fn->star]->ty,off[fn->star]); }
    if(!done_dstar){ gen_kwargs(f,fn,e,xi); put_arg(f,fn->params[fn->dstar]->ty,off[fn->dstar]); }
    for(int i=first;i<fn->nparams;i++) if(xi->argmap[i]<0 && fn->defaults[i] && i!=fn->star && i!=fn->dstar){
        Ty *pt=fn->params[i]->ty;
        if(const_default(fn->defaults[i])){ gen_as(f,fn->defaults[i],pt,fn->params[i]->consumed); put_arg(f,pt,off[i]); continue; }
        layout_defaults(fn);                                 /* evaluated where it was defined: in its function object */
        if(xi->kind==X_CALLNEST && fn->outer){ if(xi->var) load_var(f,xi->var); else E(f,"mov eax,[ebp%+d]",f->env); }
        else { use_fn(fn); gg->fv_used[fn->id]=1; rt("rt_static"); E(f,"mov eax,%s",fnl(fn,LB_VALUE)); }
        load_mem(f,pt,"eax",16+fn->defoff[i]);
        if(fn->params[i]->consumed && is_ptr(pt)) incref(f);
        put_arg(f,pt,off[i]);
    }
}
/* self_kind: 0 none, 1 the object of obj.method(), 2 the current method's self (super());
   env: the closure of a nested function (NULL: none, or the current one if it calls itself) */
static void call_user(F *f, AFunc *fn, Expr *e, XInfo *xi, int self_kind){
    use_fn(fn);
    int off[16], total=0;
    for(int i=0;i<fn->nparams;i++){ off[i]=total; total+=esize(fn->params[i]->ty); }
    if(total) E(f,"sub esp,%d",total);
    if(self_kind==1){ gen_borrow(f,e->a->a); E(f,"mov [esp],eax"); }
    if(self_kind==2){ E(f,"mov eax,[ebp+8]"); E(f,"mov [esp],eax"); }
    gen_call_args(f,fn,e,xi,off,self_kind?1:0);
    if(xi->kind==X_CALLNEST && (fn->ncaps || dyn_defaults(fn))){   /* its closure in edx */
        if(xi->var){ load_var(f,xi->var); panic_if_null(f); E(f,"mov edx,eax"); }
        else E(f,"mov edx,[ebp%+d]",f->env);
    }
    if(self_kind==2 && self_nonnull(fn) && !self_nonnull(f->fn)){
        E(f,"mov eax,[esp]"); attr_check(f,fn->name); }      /* super().m(): the method takes self as never None */
    if(self_kind==1 && fn->cls && !fn->is_static){
        if(fn->overridden || !known_object(f,e->a->a)){ E(f,"mov eax,[esp]"); attr_check_of(f,e->a->a,fn->name); }
        if(fn->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*fn->vslot); }
        else E(f,"call %s",fnl(fn,LB_CODE));
    } else E(f,"call %s",fnl(fn,LB_CODE));
    if(total) E(f,"add esp,%d",total);
}
/* fv(args): a function value -> its result (owned if a reference). Its
   parameters: the regular ones, then *args (a tuple) and **kwargs (a dict).
   With dv: a decorated method obj.m(args) -> dv(obj, args). */
static int call_value_of(F *f, Expr *e, AVar *dv){
    Ty *ft=dv?ty_find(dv->ty):TY(e->a); int cs=frame_slot(f,4), off[17], total=0;
    int star=ft->tup&1, dstar=(ft->tup>>1)&1, nreg=ft->nelems-star-dstar, skip=dv?1:0;
    AFunc pseudo; memset(&pseudo,0,sizeof pseudo); AVar pv[16], *pp[16]; Expr *nodef[16]={NULL};
    for(int i=0;i<ft->nelems;i++){
        Ty *pt;
        if(i<nreg) pt=ft->elems[i];
        else if(star && i==nreg){ pt=ty_new(TY_LIST,ft->elems[i],NULL); pt->tup=1; }
        else pt=ty_dict(TY_STR_T,ft->elems[i]);
        off[i]=total; total+=esize(pt);
        if(i>=skip){ memset(&pv[i-skip],0,sizeof pv[0]); pv[i-skip].ty=pt; pp[i-skip]=&pv[i-skip]; }
    }
    pseudo.name="function"; pseudo.nparams=ft->nelems-skip; pseudo.params=pp; pseudo.defaults=nodef;
    pseudo.star=star?nreg-skip:-1; pseudo.dstar=dstar?nreg+star-skip:-1; pseudo.kwonly=nreg-skip;
    if(dv) load_var(f,dv); else gen_borrow(f,e->a);
    E(f,"mov [ebp%+d],eax",cs);
    if(total) E(f,"sub esp,%d",total);
    if(dv){ gen_borrow(f,e->a->a); E(f,"mov [esp],eax"); }
    gen_call_args(f,&pseudo,e,xinfo(e),off+skip,0);
    if(ft->ndef){ int doff=16;                              /* omitted ones: the defaults kept in the function object */
        for(int i=nreg-ft->ndef;i<nreg;i++){ Ty *pt=ft->elems[i];
            if(i>=skip && xinfo(e)->argmap[i-skip]<0){ E(f,"mov eax,[ebp%+d]",cs); load_mem(f,pt,"eax",doff); put_arg(f,pt,off[i]); }
            doff+=esize(pt); } }
    E(f,"mov eax,[ebp%+d]",cs);
    { int l=new_label(); E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov esi,Z%d",zlit("'NoneType' object is not callable")); CALLRT(f,"rt_typeerr_text"); LBL(f,l); }
    E(f,"mov edx,eax"); E(f,"call dword [edx+8]");
    if(total) E(f,"add esp,%d",total);
    return is_ptr(ft->elem);
}
static int call_value(F *f, Expr *e){ return call_value_of(f,e,NULL); }
/* the message of an exception: str(x), owned, in eax; a KeyError keeps repr(x), which str(e) gives */
int is_keyerror(AClass *c){ for(;c;c=c->base) if(c->builtin && !strcmp(c->name,"KeyError")) return 1; return 0; }
/* an exception's message shown as is by repr() (KeyError: the key's repr; a non-str argument: str(1) == repr(1)) */
static int raw_message(Expr *x, AClass *cls){ if(!x) return 0; if(cls && is_keyerror(cls)) return 1; return ty_find(TY(x))->k!=TY_STR; }
int exc_rawoff(AClass *cls){ AField *fd=aot_find_field(cls,"_rawrepr"); return fd?fd->offset:0; }
static void gen_exc_message_of(F *f, Expr *x, AClass *cls){
    if(cls && is_keyerror(cls)){ rt("rt_sb_need"); E(f,"push dword [rt_sb_len]"); gen_borrow(f,x); gen_fmt(f,TY(x),1); E(f,"pop eax"); CALLRT(f,"rt_sb_take"); return; }
    int o=gen_str_of(f,x); if(!o) incref(f);
}
/* A new object (eax, kept): the slots of class attributes an object may set start as the class's
   value (the most derived class's, for one a subclass redefines). */
/* eax/st0 = the attribute of the object in eax (borrowed) */
void load_field(F *f, AField *fd){
    if(aot_field_shared(fd)){ load_var(f,fd->cvar); return; }        /* a class attribute no object sets */
    if(fd->setoff){ int own=new_label(), done=new_label();          /* the object's own value once set */
        E(f,"cmp dword [eax+%d],0",fd->setoff); E(f,"jne L%d",own); load_var(f,fd->cvar); E(f,"jmp L%d",done);
        LBL(f,own); load_mem(f,fd->ty,"eax",fd->offset); LBL(f,done); return; }
    load_mem(f,fd->ty,"eax",fd->offset);
}
void seed_fields(F *f, AClass *cls){
    AClass *chain[32]; int n=0; for(AClass *c=cls;c&&n<32;c=c->base) chain[n++]=c;
    for(int k=n-1;k>=0;k--) for(int i=0;i<chain[k]->nfields;i++){ AField *fd=chain[k]->fields[i], *src=NULL;
        if(fd->over || aot_field_shared(fd) || fd->setoff) continue;
        for(int j=0;j<=k && !src;j++) for(int q=0;q<chain[j]->nfields;q++){ AField *g=chain[j]->fields[q]; if(aot_field_root(g)==fd && g->cvar){ src=g; break; } }
        if(!src && !fd->init) continue;
        E(f,"push eax");
        if(src){ load_var(f,src->cvar); if(is_ptr(fd->ty)) incref(f); }
        else gen_as(f,fd->init,fd->ty,1);
        E(f,"mov ecx,[esp]");
        store_new(f,fd->ty,"ecx",fd->offset);
        E(f,"pop eax");
    }
}
int gen_ctor(F *f, Expr *e, XInfo *xi){
    AClass *cls=xi->cls; AFunc *init=xi->fn;
    if(xi->abstract_msg){                                  /* an abstract class: TypeError (after the arguments) */
        for(int i=0;i<e->count;i++){ int o=gen(f,e->items[i]); drop_value(f,TY(e->items[i]),o); }
        E(f,"mov esi,Z%d",zlit(xi->abstract_msg)); CALLRT(f,"rt_typeerr_text"); E(f,"xor eax,eax"); return 1; }
    use_class(cls);
    if(!init && aot_is_exception(cls)){                  /* ValueError("message") */
        if(e->kind==EXPR_CALL && e->count){ gen_exc_message_of(f,e->items[0],cls); E(f,"push eax"); } else E(f,"push 0");
        E(f,"mov eax,%d",cls->size); E(f,"mov edx,%s",class_label(cls,0)); E(f,"mov ecx,%s",class_label(cls,1)); CALLRT(f,"rt_obj_new");
        E(f,"pop ecx"); E(f,"mov [eax+12],ecx");
        seed_fields(f,cls);
        if(raw_message(e->count?e->items[0]:NULL,cls)) E(f,"mov dword [eax+%d],1",exc_rawoff(cls));
        return 1;
    }
    int off[16], total=0;
    if(init){ use_fn(init); for(int i=0;i<init->nparams;i++){ off[i]=total; total+=esize(init->params[i]->ty); } }
    if(total) E(f,"sub esp,%d",total);
    if(init) gen_call_args(f,init,e,xi,off,1);
    E(f,"mov eax,%d",cls->size); E(f,"mov edx,%s",class_label(cls,0)); E(f,"mov ecx,%s",class_label(cls,1)); CALLRT(f,"rt_obj_new");
    seed_fields(f,cls);
    if(init){ E(f,"mov [esp],eax"); E(f,"call %s",fnl(init,LB_CODE)); E(f,"mov eax,[esp]"); }
    if(total) E(f,"add esp,%d",total);
    return 1;
}
void call_method_on_top(F *f, AFunc *m){      /* object at [esp], one 4-byte argument area */
    use_fn(m);
    E(f,"mov eax,[esp]"); attr_check(f,m->name);
    if(m->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*m->vslot); }
    else E(f,"call %s",fnl(m,LB_CODE));
}

int gen_call(F *f, Expr *e){
    XInfo *xi=xinfo(e); Ty *t=TY(e);
    switch(xi->kind){
        case X_FUNC: case X_STATIC: case X_CALLNEST: call_user(f,xi->fn,e,xi,0); return is_ptr(t);
        case X_CALLVAL: return call_value(f,e);
        case X_CALLDECO: return call_value_of(f,e,xi->var);
        case X_METHOD: call_user(f,xi->fn,e,xi,1); return is_ptr(t);
        case X_SUPER:
            if(!xi->fn){                                         /* super().__init__(message) of an exception */
                if(e->count) gen_exc_message_of(f,e->items[0],f->fn->cls); else E(f,"xor eax,eax");
                E(f,"mov ecx,[ebp+8]"); store_mem(f,TY_STR_T,"ecx",12);
                if(raw_message(e->count?e->items[0]:NULL,f->fn->cls)){ E(f,"mov ecx,[ebp+8]"); E(f,"mov dword [ecx+%d],1",exc_rawoff(f->fn->cls)); }
                return 0;
            }
            call_user(f,xi->fn,e,xi,xi->fn->is_static?0:2); return is_ptr(t);
        case X_CTOR: return gen_ctor(f,e,xi);
        case X_TYPECALL:{                                 /* cls_value(args): the constructor of the class it is */
            int slot=frame_slot(f,4), lend=new_label();
            gen(f,e->a); E(f,"mov [ebp%+d],eax",slot);
            for(int i=0;i<xi->ncands;i++){ int lnext=new_label(); AClass *k=xi->cands[i]; use_class(k);
                E(f,"cmp dword [ebp%+d],%s",slot,class_label(k,0)); E(f,"jne L%d",lnext);
                gen_ctor(f,e,xi->cxi[i]); E(f,"jmp L%d",lend);
                LBL(f,lnext); }
            E(f,"mov esi,Z%d",zlit("'NoneType' object is not callable")); CALLRT(f,"rt_typeerr_text");
            LBL(f,lend); return 1; }
        case X_BUILTIN: return gen_builtin(f,e,xi);
        case X_TMETHOD: return gen_tmethod(f,e,xi);
        case X_SYSCALL: gen_syscall_regs(f,e,63); E(f,"mov eax,esp"); CALLRT(f,"rt_syscall_list"); E(f,"add esp,28"); return 1;
        case X_SYS: return gen_sys(f,e,xi);
        case X_ASYNC: return gen_asyncio(f,e,xi);
        case X_BMOD: if(!strncmp(xi->name,"ctypes.",7)) return gen_ctypes_fn(f,e,xi);
            if(!strcmp(xi->name,"json.dumps")) return gen_json_dumps(f,e,xi);
            if(!strcmp(xi->name,"minipy.endpoint")){ gen_as(f,e->items[0],TY(e),1); return 1; }
            return gen_bmod(f,e,xi);
        case X_CCALL: return gen_ccall(f,e,xi);
        default: cg_fail(e->line,"unsupported call");
    }
}

