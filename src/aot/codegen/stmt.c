/* ========================= Typed compiler, code generator: statements =========================
   print, imports, module initialization, for loops; the statement
   dispatcher (gen_stmt). */

#include "gen.h"

static int has_call(Expr *e){
    if(!e) return 0;
    if(e->kind==EXPR_CALL||e->kind==EXPR_COMPREHENSION) return 1;
    if(has_call(e->a)||has_call(e->b)||has_call(e->c)||has_call(e->d)) return 1;
    for(int i=0;i<e->count;i++) if(has_call(e->items[i])) return 1;
    for(int i=0;i<e->vcount;i++) if(has_call(e->vals[i])) return 1;
    return 0;
}
/* print(..., *xs, ...): separators only between the items actually printed */
static void gen_print_star(F *f, APrint *pr){
    int first=frame_slot(f,4);
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    E(f,"mov dword [ebp%+d],1",first);
    for(int i=0;i<pr->n;i++){
        Iter I; int star=pr->star[i];
        if(star){ iter_begin(f,pr->args[i],&I); }
        int l=new_label();
        E(f,"cmp dword [ebp%+d],0",first); E(f,"jne L%d",l);
        if(pr->sep) gen_into_sb(f,pr->sep); else { E(f,"mov al,' '"); CALLRT(f,"rt_sb_char"); }
        LBL(f,l); E(f,"mov dword [ebp%+d],0",first);
        if(star){ iter_value(f,&I,0); gen_fmt(f,I.elem,0); iter_end(f,&I); LBL(f,I.exit); iter_release(f,&I); }
        else gen_into_sb(f,pr->args[i]);
    }
    if(pr->end) gen_into_sb(f,pr->end); else { E(f,"mov al,10"); CALLRT(f,"rt_sb_char"); }
    E(f,"pop eax"); CALLRT(f,pr->fd==2?"rt_sb_flush_err":"rt_sb_flush");
}
static void gen_print(F *f, APrint *pr){
    int n=pr->n;
    if(pr->flush){ int o=gen(f,pr->flush); drop_value(f,TY(pr->flush),o); }   /* (writes are not buffered: nothing to flush) */
    for(int i=0;i<n;i++) if(pr->star[i]){ gen_print_star(f,pr); return; }
    if(!pr->sep && !pr->end && pr->fd!=2){
        if(n==0){ E(f,"mov ecx,P%d",print_lit("",0)); E(f,"mov edx,1"); CALLRT(f,"rt_write"); return; }
        Expr *a=pr->args[0]; Ty *t=TY(a);
        if(n==1 && is_str_lit(a) && a->tok->len){
            E(f,"mov ecx,P%d",print_lit(a->tok->text,(int)a->tok->len)); E(f,"mov edx,%d",(int)a->tok->len+1); CALLRT(f,"rt_write"); return; }
        if(n==1 && t->k==TY_STR && !t->opt && !is_concat(a) && !is_fmt_expr(a) && !is_str_call(a)){ gen_borrow(f,a); CALLRT(f,"rt_print_str"); return; }
        if(n==1 && t->k==TY_INT && !t->opt){ gen(f,a); CALLRT(f,"rt_print_int"); return; }      /* edx:eax */
    }
    /* print(xs, xs.pop()): every argument is evaluated before any is formatted */
    int early=0, slots[32];
    for(int i=1;i<n && !early;i++) if(has_call(pr->args[i])) for(int k=0;k<i;k++){ Ty *t=TY(pr->args[k]); if(t->k==TY_LIST||t->k==TY_DICT||t->k==TY_SET||t->k==TY_OBJ) early=1; }
    if(early) for(int i=0;i<n;i++){ Expr *a=pr->args[i]; int o=gen(f,a); Ty *t=TY(a);
        slots[i]=(t->k==TY_VAR||t->k==TY_VOID) ? 0 : park_value(f,t,o); }
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    for(int i=0;i<n;i++){
        if(i){ if(pr->sep) gen_into_sb(f,pr->sep); else { E(f,"mov al,' '"); CALLRT(f,"rt_sb_char"); } }
        if(early){ Ty *t=TY(pr->args[i]);
            if(slots[i]) load_mem(f,t,"ebp",slots[i]);
            gen_fmt(f,t,0); }
        else gen_into_sb(f,pr->args[i]);
    }
    if(pr->end) gen_into_sb(f,pr->end); else { E(f,"mov al,10"); CALLRT(f,"rt_sb_char"); }
    E(f,"pop eax"); CALLRT(f,pr->fd==2?"rt_sb_flush_err":"rt_sb_flush");
}

static AModule *module_named(const char *name){
    for(int i=0;i<gg->p->nmods;i++) if(!strcmp(gg->p->mods[i]->name,name)) return gg->p->mods[i];
    return NULL;
}
/* run an imported module's body the first time it is imported */
static void init_module(F *f, AModule *m){
    if(!m || !m->unit->ast || m->index==0 || !m->body) return;
    use_fn(m->body); m->used=1;
    int l=new_label();
    E(f,"cmp byte [%s],0",module_label(m)); E(f,"jne L%d",l);
    E(f,"mov byte [%s],1",module_label(m)); E(f,"call %s",fnl(m->body,LB_CODE));
    LBL(f,l);
}
static void init_prefixes(F *f, const char *dotted){
    char buf[300];
    for(const char *q=dotted;;q++){
        if(*q=='.'||!*q){ snprintf(buf,sizeof buf,"%.*s",(int)(q-dotted),dotted); init_module(f,module_named(buf)); }
        if(!*q) break;
    }
}
static void gen_import(F *f, Stmt *s){
    AotUnit *u=f->fn->mod->unit;
    if(s->kind==STMT_IMPORT){ init_prefixes(f,s->name2?s->name2:s->name); return; }
    char *dotted=aot_from_import_module(u,s);
    init_prefixes(f,dotted);
    AModule *t=module_named(dotted);
    for(int i=0;i<s->nnames;i++){
        const char *nm=s->names[i];
        if(!strcmp(nm,"*") || (t && symtab_find(&t->syms,nm))) continue;
        char sub[300]; snprintf(sub,sizeof sub,"%s.%s",dotted,nm);
        init_module(f,module_named(sub));
    }
    free(dotted);
}

static void gen_for(F *f, Stmt *s){
    AotUnit *u=f->fn->mod->unit;
    Expr *it=aot_expr(u,&s->expr);
    Iter I; iter_begin(f,it,&I);
    int lbreak=new_label();
    AVar *vars[16];
    for(int k=0;k<s->param_count && k<16;k++) vars[k]=find_var(f,s->params[k],s->line);
    iter_store_vars(f,&I,vars,s->param_count);
    if(f->nloops==64) cg_fail(s->line,"loops nested too deeply");
    f->loops[f->nloops].lcont=I.cont; f->loops[f->nloops].lbreak=lbreak; f->nloops++;
    gen_stmts(f,s->body,s->body_count);
    f->nloops--;
    iter_end(f,&I);
    LBL(f,I.exit);
    gen_stmts(f,s->orelse,s->orelse_count);
    LBL(f,lbreak);
    iter_release(f,&I);
}

/* the function of a module-level def statement (decorated: under its hidden name " name") */
static AFunc *module_def_fn(F *f, Stmt *s){
    ASym *x=symtab_find(&f->fn->mod->syms,s->name);
    if(!x || x->kind!=AS_FUNC){ char h[300]; snprintf(h,sizeof h," %s",s->name); x=symtab_find(&f->fn->mod->syms,h); }
    return x && x->kind==AS_FUNC ? (AFunc*)x->p : NULL;
}
static void gen_stmt(F *f, Stmt *s){
    AotUnit *u=f->fn->mod->unit;
    int mark=scope_open(f);
    moves_begin(f,s);
    switch(s->kind){
        case STMT_EXPR:{
            APrint *pr=aot_print(u,s);
            if(pr){ gen_print(f,pr); break; }
            if(aot_is_annotation_only(u,s)){ gen_assign(f,s,aot_assign(u,s)); break; }
            Expr *e=aot_expr(u,&s->expr);
            if(is_str_lit(e)) break;                          /* docstring */
            if(e->kind==EXPR_CALL && xinfo(e)->kind==X_SYSCALL){ gen_syscall_regs(f,e,0); E(f,"add esp,28"); break; }   /* result unused: no list */
            int o=gen(f,e); drop_value(f,TY(e),o);
            break; }
        case STMT_ASSIGN: gen_assign(f,s,aot_assign(u,s)); break;
        case STMT_IF:{
            int lelse=new_label();
            gen_jump(f,aot_expr(u,&s->expr),lelse,0);
            gen_stmts(f,s->body,s->body_count);
            if(s->orelse_count){ int lend=new_label(); E(f,"jmp L%d",lend); LBL(f,lelse); gen_stmts(f,s->orelse,s->orelse_count); LBL(f,lend); }
            else LBL(f,lelse);
            break; }
        case STMT_WHILE:{
            int ltop=new_label(), lelse=new_label(), lbreak=new_label();
            LBL(f,ltop);
            gen_jump(f,aot_expr(u,&s->expr),lelse,0);
            if(f->nloops==64) cg_fail(s->line,"loops nested too deeply");
            f->loops[f->nloops].lcont=ltop; f->loops[f->nloops].lbreak=lbreak; f->nloops++;
            gen_stmts(f,s->body,s->body_count);
            f->nloops--;
            E(f,"jmp L%d",ltop);
            LBL(f,lelse);
            gen_stmts(f,s->orelse,s->orelse_count);
            LBL(f,lbreak);
            break; }
        case STMT_FOR: gen_for(f,s); break;
        case STMT_BREAK: if(!f->nloops) cg_fail(s->line,"'break' outside a loop"); leave_tries(f,loop_tries(f)); E(f,"jmp L%d",f->loops[f->nloops-1].lbreak); break;
        case STMT_CONTINUE: if(!f->nloops) cg_fail(s->line,"'continue' outside a loop"); leave_tries(f,loop_tries(f)); E(f,"jmp L%d",f->loops[f->nloops-1].lcont); break;
        case STMT_TRY: gen_try(f,s); break;
        case STMT_RETURN:{
            Ty *rt_=ty_find(f->ret);
            if(s->expr && f->fn->is_gen){                         /* a generator's return value: kept in it */
                Ty *gr=gpart(f->fn->ret,1); Expr *e=aot_expr(u,&s->expr);
                gen_as(f,e,gr,1); E(f,"mov ecx,[rt_cur_gen]"); store_mem(f,gr,"ecx",100); E(f,"mov ecx,[rt_cur_gen]"); E(f,"mov dword [ecx+108],1");
            } else if(s->expr){
                Expr *e=aot_expr(u,&s->expr);
                if(rt_->k==TY_VOID){ int o=gen(f,e); drop_value(f,TY(e),o); }
                else gen_as(f,e,rt_,1);
            } else if(rt_->k!=TY_VOID){ int o=gen_zero(f,rt_); if(is_ptr(rt_) && !o) incref(f); }
            if(f->tmp_used>mark){ if(!is_flt(rt_)) push_value(f,rt_); scope_close(f,mark); if(!is_flt(rt_)) pop_value(f,rt_); }
            if(f->ntries){                                    /* finally blocks run first; keep the value meanwhile */
                int keep=frame_slot(f,8);
                if(rt_->k!=TY_VOID) store_new(f,rt_,"ebp",keep);
                leave_tries(f,0);
                if(rt_->k!=TY_VOID) load_mem(f,rt_,"ebp",keep);
            }
            E(f,"jmp L%d",f->lret);
            break; }
        case STMT_DEL:{
            ADel *d=aot_del(u,s);
            for(int i=0;i<d->n;i++){ Expr *t=d->t[i];
                if(t->kind==EXPR_NAME){ AVar *v=xinfo(t)->var; char a[320], b[320];       /* del x */
                    check_bound(f,v); var_addr(a,sizeof a,v); var_addr(b,sizeof b,v->bound);
                    E(f,"mov dword %s,0",b);
                    if(is_ptr(v->ty)){ E(f,"xor eax,eax"); E(f,"xchg eax,%s",a); CALLRT(f,"rt_decref"); }
                    continue; }
                if(t->kind==EXPR_SLICE){ Expr *parts[3]={t->b,t->c,t->d}; int flags=0;   /* del xs[a:b:c] */
                    E(f,"sub esp,20"); gen_borrow(f,t->a); check_none(f,TY(t->a),"TypeError","'NoneType' object does not support item deletion"); E(f,"mov [esp],eax");
                    for(int k=0;k<3;k++){
                        if(parts[k] && parts[k]->kind!=EXPR_NONE){ gen(f,parts[k]); to_i32(f,TY(parts[k])); E(f,"mov [esp+%d],eax",4+4*k); }
                        else { flags|=1<<k; E(f,"mov dword [esp+%d],0",4+4*k); } }
                    E(f,"mov dword [esp+16],%d",flags); CALLRT(f,"rt_list_del_slice"); E(f,"add esp,20");
                    continue; }
                Ty *ct=TY(t->a), *el=ct->elem; int dict=ct->k==TY_DICT;
                gen_borrow(f,t->a); check_none(f,ct,"TypeError","'NoneType' object does not support item deletion"); E(f,"push eax");
                if(dict){ Ty *kt=ty_dkey(ct); gen_as(f,t->b,kt,0); push_value(f,kt);
                    E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(kt)); CALLRT(f,"rt_dict_del"); E(f,"add esp,%d",esize(kt)+4); }
                else { gen(f,t->b); to_i32(f,TY(t->b)); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_pop"); }
                if(is_ptr(el)){ E(f,"mov eax,[eax]"); CALLRT(f,"rt_decref"); }
            }
            break; }
        case STMT_ASSERT:{
            int lok=new_label();
            gen_jump(f,aot_expr(u,&s->expr),lok,1);
            if(s->expr2) gen_str_of(f,aot_expr(u,&s->expr2)); else E(f,"xor eax,eax");
            CALLRT(f,"rt_panic_assert");
            LBL(f,lok);
            break; }
        case STMT_RAISE:{
            if(!s->expr){                                        /* re-raise the exception being handled */
                int slot=f->excs[f->nexcs-1];
                E(f,"mov eax,[ebp%+d]",slot); incref(f); E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_throw"); break; }
            Expr *e=aot_expr(u,&s->expr);
            if(e->kind==EXPR_NAME && xinfo(e)->kind==X_CTOR) gen_ctor(f,e,xinfo(e));   /* raise ValueError */
            else gen_owned(f,e);
            AClass *xc=ty_find(TY(e))->cls; AField *fcause=aot_find_field(xc,"__cause__"), *fctx=aot_find_field(xc,"__context__"), *fsup=aot_find_field(xc,"__suppress_context__");
            if(s->expr2 && fcause){                          /* raise X from Y: X.__cause__ = Y */
                Expr *y=aot_expr(u,&s->expr2);
                E(f,"push eax");
                if(y->kind==EXPR_NONE) E(f,"xor eax,eax");
                else if(y->kind==EXPR_NAME && xinfo(y)->kind==X_CTOR) gen_ctor(f,y,xinfo(y));
                else gen_owned(f,y);
                E(f,"mov ecx,[esp]"); store_mem(f,fcause->ty,"ecx",fcause->offset);
                E(f,"mov ecx,[esp]"); E(f,"mov dword [ecx+%d],1",fsup->offset);
                E(f,"pop eax"); }
            if(f->nexcs>0 && fctx){                          /* raised while handling another: its __context__ */
                int l=new_label(), lw=new_label(), lc=new_label(), ld=new_label(); E(f,"mov ecx,[ebp%+d]",f->excs[f->nexcs-1]);
                E(f,"test ecx,ecx"); E(f,"jz L%d",l); E(f,"cmp ecx,eax"); E(f,"je L%d",l); E(f,"cmp dword [eax+%d],0",fctx->offset); E(f,"jne L%d",l);
                E(f,"push ecx"); LBL(f,lw);                       /* (as CPython: the new one out of the other's chain - no cycle) */
                E(f,"mov edx,[ecx+%d]",fctx->offset); E(f,"test edx,edx"); E(f,"jz L%d",ld); E(f,"cmp edx,eax"); E(f,"je L%d",lc); E(f,"mov ecx,edx"); E(f,"jmp L%d",lw);
                LBL(f,lc); E(f,"mov dword [ecx+%d],0",fctx->offset); E(f,"dec dword [eax]");   /* (eax keeps the reference being raised) */
                LBL(f,ld); E(f,"pop ecx");
                E(f,"inc dword [ecx]"); E(f,"mov [eax+%d],ecx",fctx->offset); LBL(f,l); }
            CALLRT(f,"rt_throw");
            break; }
        case STMT_WITH:{                                      /* with open(...) as f: the file is closed at the end */
            AWith *w=aot_with(u,s); int slots[8];
            for(int i=0;i<w->n;i++){
                Ty *t=TY(w->e[i]); slots[i]=0;
                if(t->k==TY_FILE){ slots[i]=ref_slot(f); gen_owned(f,w->e[i]); E(f,"mov [ebp%+d],eax",slots[i]);
                    if(w->as[i]){ incref(f); store_target(f,&(Expr){.kind=EXPR_NAME,.name=w->as[i]},t,s->line); } }
                else if(w->as[i]){ gen_owned(f,w->e[i]); store_target(f,&(Expr){.kind=EXPR_NAME,.name=w->as[i]},t,s->line); }
                else { int o=gen(f,w->e[i]); drop_value(f,t,o); }
            }
            gen_stmts(f,s->body,s->body_count);
            for(int i=w->n-1;i>=0;i--) if(slots[i]){
                E(f,"mov eax,[ebp%+d]",slots[i]); CALLRT(f,"rt_file_close");
                E(f,"mov eax,[ebp%+d]",slots[i]); E(f,"mov dword [ebp%+d],0",slots[i]); CALLRT(f,"rt_decref"); }
            break; }
        case STMT_IMPORT: case STMT_FROM_IMPORT: gen_import(f,s); break;
        case STMT_FUNCTION_DEF:
            if(!is_fn_body(f->fn)){ AFunc *mf=module_def_fn(f,s);       /* module level: its defaults, evaluated now */
                if(mf && has_defaults(mf)){
                    for(int q=-1;q<mf->ninsts;q++){ AFunc *g=q<0?mf:mf->insts[q];  /* (a generic function: the instances calls use) */
                        if(q<0 && mf->pristine && !mf->direct) continue;
                        gg->fv_used[g->id]=1; rt("rt_static"); fill_defaults(f,g,fnl(g,LB_VALUE)); } } }   /* (its code only if something calls it) */
            if(s->expr2){                                /* decorated: name = d1(d2(function)) */
                if(is_fn_body(f->fn) && ((AFunc*)s->aux)->unused) break;
                gen_owned(f,s->expr2); store_var(f,find_var(f,s->name,s->line));
            } else if(is_fn_body(f->fn)){                /* a nested def: its closure into its variable */
                AFunc *inner=(AFunc*)s->aux;
                if(inner->unused) break;
                AVar *v=find_var(f,s->name,s->line);
                int o=gen_funcref(f,inner); if(!o) incref(f);
                store_var(f,v);
            }
            break;
        case STMT_CLASS_DEF:                             /* attributes; methods' defaults; decorated methods: Class.name = decorators(method) */
            for(int q=-1;q<((AClass*)s->aux)->ninsts;q++){ AClass *cls=q<0?(AClass*)s->aux:((AClass*)s->aux)->insts[q];   /* (and a generic class's instances) */
                for(int b=0;b<cls->def->body_count;b++){ Stmt *bs=cls->def->body[b];      /* the body, in order */
                    if(bs->kind==STMT_ASSIGN){ AAssign *a=aot_assign(u,bs);             /* name = value: the class attribute */
                        for(int k=0;k<cls->nfields;k++){ AField *fd=cls->fields[k];
                            if(fd->cvar && fd->init==a->value){ gen_as(f,fd->init,fd->ty,1); store_var(f,fd->cvar); } } }
                    for(int k=0;k<cls->nmethods;k++){ AFunc *m=cls->methods[k];
                        if(m->def!=bs) continue;
                        if(has_defaults(m)) for(int q=-1;q<m->ninsts;q++){ AFunc *g=q<0?m:m->insts[q];      /* its defaults, evaluated now (its code: if called); */
                            gg->fv_used[g->id]=1; rt("rt_static"); fill_defaults(f,g,fnl(g,LB_VALUE)); }      /* (a generic method's copies too) */
                        if(m->decovar){ gen_owned(f,m->def->expr2); store_var(f,m->decovar); } }
                    if(bs->kind==STMT_CLASS_DEF) gen_stmt(f,bs); }                 /* a nested class */
            }
            break;
        case STMT_YIELD:{
            Ty *yt=f->fn->yield_ty;
            if(!s->expr){ int o=gen_zero(f,yt); if(is_ptr(yt)&&!o) incref(f); gen_yield_value(f,yt); drop_sent(f); break; }
            Expr *e=aot_expr(u,&s->expr);
            if(s->block_tag==7){                          /* yield from: each item */
                if(ty_find(TY(e))->k==TY_GEN){ gen_yield_from(f,e,0); break; }
                Iter I; iter_begin(f,e,&I);
                iter_value(f,&I,0); if(is_ptr(I.elem)) incref(f); conv(f,I.elem,yt);
                gen_yield_value(f,yt); drop_sent(f);
                iter_end(f,&I); LBL(f,I.exit); iter_release(f,&I);
                break;
            }
            gen_as(f,e,yt,1); gen_yield_value(f,yt); drop_sent(f);
            break; }
        case STMT_NONLOCAL: case STMT_PASS: case STMT_GLOBAL: break;
        default: cg_fail(s->line,"unsupported statement");
    }
    f->live_after=NULL;
    scope_close(f,mark);
}
void gen_stmts(F *f, Stmt **b, int n){ for(int i=0;i<n;i++) gen_stmt(f,b[i]); }

