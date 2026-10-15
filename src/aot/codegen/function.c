/* ========================= Typed compiler, code generator: functions =========================
   Generators (yield, yield from), function bodies, adapters, bound
   methods, closures' destroy routines. */

#include "gen.h"

/* yield: the value (owned) into the generator, then back to its consumer */
void gen_yield_value(F *f, Ty *yt){
    E(f,"mov ecx,[rt_cur_gen]");
    store_mem(f,yt,"ecx",24);
    CALLRT(f,"rt_gen_yield");
}
/* after a yield statement: a value sent meanwhile is dropped */
void drop_sent(F *f){
    Ty *st=gpart(f->fn->ret,0); int l=new_label();
    E(f,"mov ecx,[rt_cur_gen]"); E(f,"cmp dword [ecx+96],0"); E(f,"je L%d",l); E(f,"mov dword [ecx+96],0");
    if(is_ptr(st)){ rt("rt_decref"); E(f,"xor eax,eax"); E(f,"xchg eax,[ecx+88]"); E(f,"call rt_decref"); }
    LBL(f,l);
}
/* eax/st0 = what the yield expression gives: the value sent (owned), None when next() resumed it */
int take_sent(F *f, Ty *st){
    int l=new_label(), end=new_label();
    E(f,"mov ecx,[rt_cur_gen]"); E(f,"cmp dword [ecx+96],0"); E(f,"je L%d",l);
    E(f,"mov dword [ecx+96],0"); load_mem(f,st,"ecx",88);
    if(is_ptr(st)) E(f,"mov dword [ecx+88],0");
    E(f,"jmp L%d",end);
    LBL(f,l); gen_none(f,st);
    LBL(f,end);
    return is_ptr(st);
}
/* yield from a generator: its values; values sent go to it -> (want) what it returns */
int gen_yield_from(F *f, Expr *src, int want){
    Ty *gt=ty_find(TY(src)), *iyt=ty_find(gt->elem), *yt=ty_find(f->fn->yield_ty), *gr=gpart(gt,1);
    int slot=frame_slot(f,4); add_ref_slot(f,slot);
    gen_owned(f,src); panic_if_null(f); E(f,"mov [ebp%+d],eax",slot);
    int lnext=new_label(), ldone=new_label();
    LBL(f,lnext); E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_gen_next"); E(f,"test eax,eax"); E(f,"jz L%d",ldone);
    E(f,"mov ecx,[ebp%+d]",slot); load_mem(f,iyt,"ecx",24); if(is_ptr(iyt)) incref(f); conv(f,iyt,yt);
    gen_yield_value(f,yt);
    E(f,"mov ecx,[rt_cur_gen]"); E(f,"cmp dword [ecx+96],0"); E(f,"je L%d",lnext);     /* a value sent: the inner generator's */
    E(f,"mov dword [ecx+96],0"); E(f,"mov edx,[ebp%+d]",slot);
    E(f,"mov eax,[ecx+88]"); E(f,"mov [edx+88],eax"); E(f,"mov eax,[ecx+92]"); E(f,"mov [edx+92],eax");
    E(f,"mov dword [ecx+88],0"); E(f,"mov dword [ecx+92],0"); E(f,"mov dword [edx+96],1");
    E(f,"jmp L%d",lnext);
    LBL(f,ldone);
    int o=0;
    if(want && gr->k!=TY_VOID){ int ln=new_label(), le=new_label();
        E(f,"mov ecx,[ebp%+d]",slot); E(f,"cmp dword [ecx+108],0"); E(f,"je L%d",ln);
        load_mem(f,gr,"ecx",100); if(is_ptr(gr)) incref(f); E(f,"jmp L%d",le);
        LBL(f,ln); gen_none(f,gr); LBL(f,le); o=is_ptr(gr);
        push_value(f,gr); }
    rt("rt_decref"); E(f,"xor eax,eax"); E(f,"xchg eax,[ebp%+d]",slot); E(f,"call rt_decref");
    if(want && gr->k!=TY_VOID) pop_value(f,gr);
    return o;
}
/* the function of a generator expression: its loops, yielding each element */
static void gen_genexp_body(F *f, AFunc *fn){
    Expr *e=fn->genexp; XInfo *xi=xinfo(e);
    Iter its[8]; int n=e->nclause;
    if(n>8) cg_fail(e->line,"too many for clauses");
    for(int i=0;i<n;i++){
        CompClause *cl=&e->clauses[i];
        iter_begin(f,(i==0&&xi->key)?xi->key:cl->iter,&its[i]);
        iter_store_vars(f,&its[i],&xi->cvars[2*i],cl->nvars); comp_unpack(f,e,i);
        for(int k=0;k<cl->ncond;k++) gen_jump(f,cl->conds[k],its[i].cont,0);
    }
    int mark=scope_open(f);
    gen_as(f,e->a,fn->yield_ty,1); gen_yield_value(f,fn->yield_ty);
    scope_close(f,mark);
    for(int i=n-1;i>=0;i--){ iter_end(f,&its[i]); LBL(f,its[i].exit); iter_release(f,&its[i]); }
}
void emit_function(AFunc *fn){
    F *f=MPY_NEW0(F); f->fn=fn; f->lret=new_label();
    f->ret=fn->is_gen ? TY_VOID_T : fn->ret;
    cg_path=fn->mod->unit->path?fn->mod->unit->path:fn->mod->name;
    int off=8, argpos[16];
    for(int i=0;i<fn->nparams;i++){ fn->params[i]->offset=argpos[i]=off; off+=esize(fn->params[i]->ty); }
    int argbytes=off-8;
    if(fn->ncaps || (fn->outer && dyn_defaults(fn))){ layout_caps(fn); f->env=frame_slot(f,4); }
    if(is_fn_body(fn)){
        if(!getenv("MPY_NO_BORROW")) plan_borrowed(f);
        if(!getenv("MPY_NO_MOVE")) plan_moves(f);
        for(int i=0;i<fn->nvars;i++){ AVar *v=fn->vars[i];
            if(i<fn->nparams && !v->cell) continue;
            if(v->cell){ v->offset=frame_slot(f,4); add_ref_slot(f,v->offset); }
            else { v->offset=frame_slot(f,esize(v->ty)); if(is_ptr(v->ty) && !v->borrowed) add_ref_slot(f,v->offset); }
        }
    }
    if(fn->lam){                                          /* lambda: return its expression */
        Expr *b=fn->lam->a; Ty *r=ty_find(fn->ret); int mark=scope_open(f);
        if(r->k==TY_VOID){ if(b->kind!=EXPR_NONE){ int o=gen(f,b); drop_value(f,TY(b),o); } } else gen_as(f,b,r,1);
        if(f->tmp_used>mark){ if(!is_flt(r) && r->k!=TY_VOID) push_value(f,r); scope_close(f,mark); if(!is_flt(r) && r->k!=TY_VOID) pop_value(f,r); }
    } else if(fn->genexp) gen_genexp_body(f,fn);
    else {
        gen_stmts(f,fn->body,fn->nbody);
        Ty *ret=ty_find(f->ret);
        if(ret->k!=TY_VOID){ int o=gen_zero(f,ret); if(is_ptr(ret)&&!o) incref(f); }   /* falling off the end returns None */
    }
    Ty *ret=ty_find(f->ret);
    for(int i=0;i<fn->nparams;i++){ Ty *pt=fn->params[i]->ty;
        if(!is_ptr(pt)) continue;
        if(fn->is_gen || fn->params[i]->consumed==1) add_ref_slot(f,argpos[i]);   /* a generator owns its arguments; so does a function keeping one */
        else if(f->param_stored[i] && !fn->params[i]->cell) add_ref_slot(f,argpos[i]); }

    Buf fb={0}, *o=&fb;                                   /* the whole function, then the peephole pass (aot/peep.c) */
    const char *kind=fn->lam?"<lambda>":fn->genexp?"<genexpr>":fn->def?fn->name:"<module>";
    buf_printf(o,"\n; %s%s%s()%s\n%s:\n",fn->cls?fn->cls->name:"",fn->cls?".":"",kind,fn->is_gen?": the generator's function":"",fnl(fn,fn->is_gen?LB_BODY:LB_CODE));
    buf_printf(o,"        push ebp\n        mov ebp,esp\n        push ebx\n        push esi\n        push edi\n");
    int words=(f->frame+3)/4;
    if(words<=8) for(int i=0;i<words;i++) buf_printf(o,"        push 0\n");
    else buf_printf(o,"        sub esp,%d\n        mov edi,esp\n        mov ecx,%d\n        xor eax,eax\n        rep stosd\n",words*4,words);
    if(f->env) buf_printf(o,"        mov [ebp%+d],edx\n",f->env);
    for(int i=0;i<fn->nparams;i++) if(fn->params[i]->bound){ char b[320]; var_addr(b,sizeof b,fn->params[i]->bound); buf_printf(o,"        mov dword %s,1\n",b); }
    if(!fn->is_gen) for(int i=0;i<fn->nparams;i++) if(f->param_stored[i] && is_ptr(fn->params[i]->ty) && !fn->params[i]->cell && !fn->params[i]->consumed){ rt("rt_incref"); buf_printf(o,"        mov eax,[ebp%+d]\n        call rt_incref\n",argpos[i]); }
    for(int i=0;i<fn->nvars;i++){ AVar *v=fn->vars[i]; if(!v->cell) continue;   /* cells of the variables closures share */
        rt("rt_cell_new"); rt("rt_incref");
        buf_printf(o,"        mov eax,%d\n        call rt_cell_new\n",is_ptr(v->ty)?(gc_type(v->ty)?3:1):0);
        if(i<fn->nparams){
            if(is_flt(v->ty)) buf_printf(o,"        fld qword [ebp%+d]\n        fstp qword [eax+8]\n",argpos[i]);
            else if(is_lng(v->ty)) buf_printf(o,"        mov ecx,[ebp%+d]\n        mov [eax+8],ecx\n        mov ecx,[ebp%+d]\n        mov [eax+12],ecx\n",argpos[i],argpos[i]+4);
            else { buf_printf(o,"        mov ecx,[ebp%+d]\n        mov [eax+8],ecx\n",argpos[i]);
                if(is_ptr(v->ty)) buf_printf(o,"        push eax\n        mov eax,ecx\n        call rt_incref\n        pop eax\n"); }
        }
        buf_printf(o,"        mov [ebp%+d],eax\n",v->offset);
    }
    buf_cat(o,&f->code);
    buf_printf(o,"L%d:\n",f->lret);
    if(f->nrefs){
        rt("rt_decref");
        int keep=!is_flt(ret) && ret->k!=TY_VOID;
        if(keep){ if(is_lng(ret)) buf_printf(o,"        push edx\n"); buf_printf(o,"        push eax\n"); }
        for(int i=0;i<f->nrefs;i++) buf_printf(o,"        mov eax,[ebp%+d]\n        call rt_decref\n",f->refs[i]);
        if(keep){ buf_printf(o,"        pop eax\n"); if(is_lng(ret)) buf_printf(o,"        pop edx\n"); }
    }
    buf_printf(o,"        lea esp,[ebp-12]\n        pop edi\n        pop esi\n        pop ebx\n        pop ebp\n        ret\n");
    if(fn->is_gen){                                       /* F<id> makes the generator: arguments and closure kept in it */
        Ty *yt=ty_find(fn->yield_ty);
        rt("rt_gen_new"); rt("rt_gen_next"); rt("rt_incref");
        buf_printf(&gg->data,"align 4\n%s dd %s,%d,%d",fnl(fn,LB_GEN),fnl(fn,LB_BODY),(is_ptr(yt)?1:0)|(esize(yt)==8?2:0)|(is_ptr(gpart(fn->ret,0))?4:0)|(is_ptr(gpart(fn->ret,1))?8:0),f->nrefs);
        for(int i=0;i<f->nrefs;i++) buf_printf(&gg->data,",%d",f->refs[i]);
        buf_printf(&gg->data,"   ; generator %s\n",kind);
        buf_printf(o,"%s:                            ; %s(): a new generator\n",fnl(fn,LB_CODE),kind);
        buf_printf(o,"        push ebx\n        mov ebx,edx\n        mov eax,%d\n        mov edx,%s\n        call rt_gen_new\n        push eax\n        mov ecx,[eax+36]\n",argbytes,fnl(fn,LB_GEN));
        for(int w=0;w<argbytes;w+=4) buf_printf(o,"        mov edx,[esp+%d]\n        mov [ecx+%d],edx\n",12+w,w);
        for(int i=0;i<fn->nparams;i++) if(is_ptr(fn->params[i]->ty)) buf_printf(o,"        mov eax,[esp+%d]\n        call rt_incref\n",12+argpos[i]-8);
        if(fn->ncaps) buf_printf(o,"        mov eax,ebx\n        call rt_incref\n        mov ecx,[esp]\n        mov [ecx+44],ebx\n");
        buf_printf(o,"        pop eax\n        pop ebx\n        ret\n");
    }
    size_t plen; char *ptext=aot_peephole(fb.s?fb.s:"",fb.len,&plen);
    buf_put(&gg->text,ptext,plen); free(ptext); free(fb.s);
    free(f->code.s); free(f->tmp); free(f->refs); free(f);
}
int adapter_index(Ty *to, Ty *from){
    for(int i=0;i<gg->nadapters;i++) if(ty_same_exact(gg->adapters[i].to,to) && ty_same_exact(gg->adapters[i].from,from)) return i;
    if(gg->nadapters==gg->cadapters){ gg->cadapters=gg->cadapters?gg->cadapters*2:4; gg->adapters=xrealloc(gg->adapters,sizeof(*gg->adapters)*(size_t)gg->cadapters); }
    gg->adapters[gg->nadapters].to=to; gg->adapters[gg->nadapters].from=from;
    rt("rt_alloc"); rt("rt_incref"); rt("rt_decref"); rt("rt_free");
    return gg->nadapters++;
}
/* AD<k>: called as `to`, calls the function at [env+16] (a `from`, which takes *args) */
void emit_adapter(int k){
    Ty *to=gg->adapters[k].to, *from=gg->adapters[k].from;
    F ff; memset(&ff,0,sizeof ff); F *f=&ff;
    int star=1, dstar=(from->tup>>1)&1, nreg=from->nelems-star-dstar, toff[16], foff[17], tot=0, a=8;
    for(int i=0;i<to->nelems;i++){ toff[i]=a; a+=esize(to->elems[i]); }
    for(int i=0;i<from->nelems;i++){ foff[i]=tot; tot+=i<nreg?esize(from->elems[i]):4; }
    Ty *sel=from->elems[nreg];
    buf_printf(&f->code,"\nAD%d:                          ; %s as %s\n",k,ty_name(from),ty_name(to));
    E(f,"push ebp"); E(f,"mov ebp,esp"); E(f,"push ebx"); E(f,"push esi"); E(f,"push edi"); E(f,"mov ebx,edx");
    new_list(f,sel); E(f,"mov esi,eax");
    for(int i=nreg;i<to->nelems;i++){ Ty *pt=to->elems[i];
        E(f,"push esi"); load_mem(f,pt,"ebp",toff[i]); if(is_ptr(pt)) incref(f); conv_num(f,pt,sel); list_append_top(f,sel); E(f,"add esp,4"); }
    E(f,"xor edi,edi");
    if(dstar){ Ty *dt=ty_dict(TY_STR_T,from->elems[nreg+1]); new_dict(f,dt); E(f,"mov edi,eax"); }
    if(tot) E(f,"sub esp,%d",tot);
    for(int i=0;i<nreg;i++){ Ty *pt=from->elems[i]; load_mem(f,to->elems[i],"ebp",toff[i]); conv_num(f,to->elems[i],pt); put_arg(f,pt,foff[i]); }
    E(f,"mov [esp+%d],esi",foff[nreg]);
    if(dstar) E(f,"mov [esp+%d],edi",foff[nreg+1]);
    E(f,"mov edx,[ebx+16]"); E(f,"call dword [edx+8]");
    if(tot) E(f,"add esp,%d",tot);
    E(f,"push edx"); E(f,"push eax"); E(f,"mov eax,esi"); CALLRT(f,"rt_decref"); E(f,"mov eax,edi"); CALLRT(f,"rt_decref"); E(f,"pop eax"); E(f,"pop edx");
    E(f,"pop edi"); E(f,"pop esi"); E(f,"pop ebx"); E(f,"pop ebp"); E(f,"ret");
    buf_cat(&gg->text,&f->code); free(f->code.s);
}
/* BM<id>: the code of method m's bound values: the object (+16) first, then the arguments */
void emit_bound_method(AFunc *m){
    Buf *o=&gg->text; int argbytes=0;
    for(int i=1;i<m->nparams;i++) argbytes+=esize(m->params[i]->ty);
    buf_printf(o,"\n%s:                          ; %s.%s bound to an object\n        push ebp\n        mov ebp,esp\n        sub esp,%d\n        mov eax,[edx+16]\n        mov [esp],eax\n",fnl(m,LB_BOUND),m->cls?m->cls->name:"",m->name,argbytes+4);
    for(int w=0;w<argbytes;w+=4) buf_printf(o,"        mov ecx,[ebp+%d]\n        mov [esp+%d],ecx\n",8+w,4+w);
    if(m->overridden) buf_printf(o,"        mov eax,[eax+8]\n        call dword [eax+%d]\n",8+4*m->vslot);
    else buf_printf(o,"        call %s\n",fnl(m,LB_CODE));
    buf_printf(o,"        leave\n        ret\n");
}
/* <fn>.free: destroy routine of fn's closures */
void emit_closure_destroy(AFunc *fn){
    Buf *o=&gg->text; int any=0;
    rt("rt_free"); rt("rt_decref");
    buf_printf(o,"\n%s:                          ; destroy a closure of %s\n",fnl(fn,LB_FREE),fn->lam?"<lambda>":fn->genexp?"<genexpr>":fn->name);
    for(int i=0;i<fn->ncaps;i++){ AVar *v=fn->caps[i]; if(!root_var(v)->cell && !is_ptr(v->ty)) continue;
        if(!any){ buf_printf(o,"        push ebx\n        mov ebx,eax\n"); any=1; }
        buf_printf(o,"        mov eax,[ebx+%d]\n        call rt_decref\n",v->capoff); }
    layout_defaults(fn);
    for(int i=0;i<fn->nparams;i++){ if(!fn->defaults[i] || i==fn->star || i==fn->dstar || !is_ptr(fn->params[i]->ty)) continue;
        if(!any){ buf_printf(o,"        push ebx\n        mov ebx,eax\n"); any=1; }
        buf_printf(o,"        mov eax,[ebx+%d]\n        call rt_decref\n",16+fn->defoff[i]); }
    if(any) buf_printf(o,"        mov eax,ebx\n        pop ebx\n");
    buf_printf(o,"        jmp rt_free\n");
}

