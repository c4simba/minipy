/* ========================= Typed compiler, code generator: exceptions =========================
   try / except / finally: handler records, leaving try blocks. */

#include "gen.h"

/* ---- exceptions ---- */
static void push_handler(F *f, int rec, int label){
    rt("rt_throw");
    E(f,"lea eax,[ebp%+d]",rec); E(f,"mov ecx,[rt_exc_top]"); E(f,"mov [eax],ecx");
    E(f,"mov [eax+4],esp"); E(f,"mov [eax+8],ebp"); E(f,"mov dword [eax+12],L%d",label);
    E(f,"mov [eax+16],ebx"); E(f,"mov [eax+20],esi"); E(f,"mov [eax+24],edi");
    E(f,"mov [rt_exc_top],eax");
}
static void pop_handler(F *f, int rec){ E(f,"mov eax,[ebp%+d]",rec); E(f,"mov [rt_exc_top],eax"); }
/* Leaving try statements tries[to..] (return/break/continue): drop their
   handlers and run their finally blocks, innermost first. */
void leave_tries(F *f, int to){
    for(int i=f->ntries-1;i>=to;i--){
        TryCtx ctx=f->tries[i];
        if(ctx.active) pop_handler(f,ctx.rec);
        if(ctx.nfin){ int save=f->ntries; f->ntries=i; gen_stmts(f,ctx.fin,ctx.nfin); f->ntries=save; }
    }
}
/* temporaries of statements interrupted by an exception */
static void release_temps_from(F *f, int mark){
    for(int k=mark;k<f->ntmp;k++){ rt("rt_decref"); E(f,"mov eax,[ebp%+d]",f->tmp[k]); E(f,"mov dword [ebp%+d],0",f->tmp[k]); E(f,"call rt_decref"); }
}
void gen_try(F *f, Stmt *s){
    Stmt **fin=NULL; int nfin=0; Stmt *els=NULL;
    for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->block_tag==3){ fin=b->body; nfin=b->body_count; } else if(b->block_tag==2) els=b; }
    if(f->ntries==32) cg_fail(s->line,"try statements nested too deeply");
    int rec=frame_slot(f,28), exc=ref_slot(f), mark=f->tmp_used;
    int lhandler=new_label(), lfinexc=new_label(), ldone=new_label();
    TryCtx *ctx=&f->tries[f->ntries++];
    ctx->rec=rec; ctx->active=1; ctx->fin=fin; ctx->nfin=nfin; ctx->nloops=f->nloops;
    push_handler(f,rec,lhandler);
    gen_stmts(f,s->body,s->body_count);
    pop_handler(f,rec); f->tries[f->ntries-1].active=0;
    if(els){
        if(nfin){ push_handler(f,rec,lfinexc); f->tries[f->ntries-1].active=1; }
        gen_stmts(f,els->body,els->body_count);
        if(nfin){ pop_handler(f,rec); f->tries[f->ntries-1].active=0; }
    }
    E(f,"jmp L%d",ldone);
    /* an exception in the body: release what the interrupted statement held, keep the exception */
    LBL(f,lhandler);
    f->tries[f->ntries-1].active=0;
    release_temps_from(f,mark);
    E(f,"mov eax,[rt_exc_cur]"); E(f,"mov dword [rt_exc_cur],0"); E(f,"xchg eax,[ebp%+d]",exc); CALLRT(f,"rt_decref");
    if(nfin){ push_handler(f,rec,lfinexc); f->tries[f->ntries-1].active=1; }
    int nclause=0, clause_lbl[32]; Stmt *clauses[32];
    for(int i=0;i<s->orelse_count;i++){ Stmt *b=s->orelse[i]; if(b->block_tag!=1) continue;
        if(nclause==32) cg_fail(s->line,"too many except clauses");
        int l=new_label(); clause_lbl[nclause]=l; clauses[nclause++]=b;
        if(!b->expr){ E(f,"jmp L%d",l); continue; }
        Expr *te=b->expr; int n=te->kind==EXPR_TUPLE?te->count:1;
        for(int k=0;k<n;k++){ AClass *cls=xinfo(te->kind==EXPR_TUPLE?te->items[k]:te)->cls; use_class(cls);
            E(f,"mov eax,[ebp%+d]",exc); E(f,"mov edx,%s",class_label(cls,0)); CALLRT(f,"rt_isinstance"); E(f,"test eax,eax"); E(f,"jnz L%d",l); }
    }
    /* no clause matches: finally, then on to the enclosing handler */
    if(nfin){ pop_handler(f,rec); f->tries[f->ntries-1].active=0; }
    f->ntries--;
    if(nfin) gen_stmts(f,fin,nfin);
    E(f,"mov eax,[ebp%+d]",exc); E(f,"mov dword [ebp%+d],0",exc); CALLRT(f,"rt_throw");
    f->ntries++;
    for(int i=0;i<nclause;i++){ Stmt *b=clauses[i];
        LBL(f,clause_lbl[i]);
        f->tries[f->ntries-1].active=nfin?1:0;
        if(b->name){ AVar *v=(AVar*)b->aux; E(f,"mov eax,[ebp%+d]",exc); incref(f); store_var(f,v); }
        if(f->nexcs==32) cg_fail(b->line,"except clauses nested too deeply");
        f->excs[f->nexcs++]=exc;
        gen_stmts(f,b->body,b->body_count);
        f->nexcs--;
        if(nfin){ pop_handler(f,rec); f->tries[f->ntries-1].active=0; }
        E(f,"mov eax,[ebp%+d]",exc); E(f,"mov dword [ebp%+d],0",exc); CALLRT(f,"rt_decref");
        E(f,"jmp L%d",ldone);
    }
    f->ntries--;
    if(nfin){                                            /* an exception in an except/else block: finally, re-raise */
        LBL(f,lfinexc);
        release_temps_from(f,mark);
        gen_stmts(f,fin,nfin);
        E(f,"mov eax,[rt_exc_cur]"); E(f,"mov dword [rt_exc_cur],0"); CALLRT(f,"rt_throw");
    }
    LBL(f,ldone);
    if(nfin) gen_stmts(f,fin,nfin);
}
int loop_tries(F *f){ int i=f->ntries; while(i>0 && f->tries[i-1].nloops>=f->nloops) i--; return i; }

