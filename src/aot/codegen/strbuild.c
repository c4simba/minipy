/* ========================= Typed compiler, code generator: string building =========================
   Concatenations and str() / repr() of values through the string
   builder; the runtime descriptors of classes (class_kd). */

#include "gen.h"

int is_str_lit(Expr *e){ return e->kind==EXPR_LITERAL && e->tok->kind==T_STRING && e->tok->i!=2; }   /* (not b"...") */
int is_str_call(Expr *e){ if(e->kind!=EXPR_CALL||e->count!=1) return 0; XInfo *xi=xinfo(e); return xi->kind==X_BUILTIN && !strcmp(xi->name,"str"); }
int is_fmt_expr(Expr *e){ return e->kind==EXPR_BINARY && e->op==T_PERCENT && is_str_lit(e->a) && TY(e)->k==TY_STR; }
int is_concat(Expr *e){ return e->kind==EXPR_BINARY && e->op==T_PLUS && TY(e)->k==TY_STR && !is_opt(TY(e->a)) && !is_opt(TY(e->b)); }
static int concat_parts(Expr *e, int *special){
    if(is_concat(e)) return concat_parts(e->a,special)+concat_parts(e->b,special);
    if(is_str_call(e)||is_fmt_expr(e)) *special=1;
    return 1;
}
int concat_worth(Expr *e){ int sp=0; int n=concat_parts(e,&sp); return n>=3||sp; }

/* append str(x) to the string builder */
void gen_into_sb(F *f, Expr *x){
    if(is_str_lit(x)){ sb_bytes_lit(f,x->tok->text,(int)x->tok->len); return; }
    if(is_concat(x)){ gen_into_sb(f,x->a); gen_into_sb(f,x->b); return; }
    if(is_fmt_expr(x)){ gen_format_body(f,x); return; }
    if(is_str_call(x)){ gen_into_sb(f,x->items[0]); return; }
    gen_borrow(f,x); gen_fmt(f,TY(x),0);
}
int gen_concat(F *f, Expr *e){
    rt("rt_sb_need"); E(f,"push dword [rt_sb_len]");
    gen_into_sb(f,e);
    E(f,"pop eax"); CALLRT(f,"rt_sb_take");
    return 1;
}
/* eax = str(x) (owned or borrowed: returns owned) */
int gen_str_of(F *f, Expr *x){
    if(TY(x)->k==TY_STR && !TY(x)->opt) return gen(f,x);
    return gen_concat(f,x);
}

/* formatters of containers and objects: FMT<i>, eax = value */
int fmt_index(Ty *t, int repr){
    t=ty_find(t);
    if(t->k!=TY_OBJ && repr<2) repr=1;
    for(int i=0;i<gg->nfmts;i++) if(gg->fmts[i].repr==repr && ty_same_exact(gg->fmts[i].ty,t)) return i;
    if(gg->nfmts==gg->cfmts){ gg->cfmts=gg->cfmts?gg->cfmts*2:16; gg->fmts=(Fmt*)xrealloc(gg->fmts,sizeof(Fmt)*(size_t)gg->cfmts); }
    gg->fmts[gg->nfmts].ty=t; gg->fmts[gg->nfmts].repr=repr;
    return gg->nfmts++;
}
/* KDC<id>: the descriptor of a class's objects in containers (see kd_of):
   ==, ordering and hash through __eq__, __lt__ and __hash__ when the class has
   them (as CPython: __eq__ without __hash__ makes it unhashable), else identity */
static void kd_call_method(Buf *o, AFunc *m){         /* the arguments are on the stack, self first */
    if(m->overridden) buf_printf(o,"        mov eax,[esp]\n        mov eax,[eax+8]\n        call dword [eax+%d]\n",8+4*m->vslot);
    else buf_printf(o,"        call %s\n",fnl(m,LB_CODE));
}
static void kd_truth(Buf *o, Ty *r){                  /* eax = truth of a method's result */
    if(ty_find(r)->k!=TY_BOOL) buf_printf(o,"        test eax,eax\n        setne al\n        movzx eax,al\n");
}
static int kd_method_ok(AFunc *m, int nparams){
    if(!m || m->is_static || m->nparams<nparams) return 0;
    for(int i=1;i<nparams;i++) if(!is_ptr(m->params[i]->ty) || is_flt(m->params[i]->ty)) return 0;
    for(int i=nparams;i<m->nparams;i++)                  /* (more parameters: pointers left None - __lt__(self, other, context=None)) */
        if(!m->defaults[i] || m->defaults[i]->kind!=EXPR_NONE || !is_ptr(m->params[i]->ty) || i==m->star || i==m->dstar) return 0;
    return 1;
}
/* the None arguments of a kind method's parameters after the first n (pushed before the others) */
static const char *kd_nones(AFunc *m, int n){
    static char b[4][160]; static int k; char *o=b[k++&3]; o[0]=0;
    for(int i=n;i<m->nparams && i<16;i++) strcat(o,"        push 0\n");
    return o;
}
const char *class_kd(AClass *c){
    if(!gg->kdlabels){ gg->kdlabels=(char**)xmalloc(sizeof(char*)*(size_t)(gg->p->nclasses+1)); memset(gg->kdlabels,0,sizeof(char*)*(size_t)(gg->p->nclasses+1)); }
    if(gg->kdlabels[c->id]) return gg->kdlabels[c->id];
    char lab[64]; snprintf(lab,sizeof lab,"KDC%d",c->id);
    gg->kdlabels[c->id]=xstrdup2(lab);
    use_class(c);
    Buf *o=&gg->text;
    AFunc *eq=c->eq_inst?c->eq_inst:aot_find_method(c,"__eq__"), *lt=c->lt_inst?c->lt_inst:aot_find_method(c,"__lt__"), *hs=aot_find_method(c,"__hash__");
    if(!kd_method_ok(eq,2)) eq=NULL;
    if(!kd_method_ok(lt,2)) lt=NULL;
    if(!kd_method_ok(hs,1) || !(ty_find(hs->ret)->k==TY_INT||ty_find(hs->ret)->k==TY_BOOL)) hs=NULL;
    char eqf[64]="rt_eq_w", cmpf[64]="rt_cmp_no", hashf[64]="rt_hash_p";
    if(eq){ use_fn(eq); snprintf(eqf,sizeof eqf,"KDC%d_eq",c->id);
        int lid=++gg->labels;
        buf_printf(o,"\n%s:                       ; %s.__eq__ for containers (None: identity)\n        mov ecx,[eax]\n        mov eax,[edx]\n        test ecx,ecx\n        jz L%d\n        test eax,eax\n        jz L%d\n%s        push eax\n        push ecx\n",eqf,c->name,lid,lid,kd_nones(eq,2));
        kd_call_method(o,eq); buf_printf(o,"        add esp,%d\n",4*eq->nparams); kd_truth(o,eq->ret);
        buf_printf(o,"        ret\nL%d:\n        cmp ecx,eax\n        sete al\n        movzx eax,al\n        ret\n",lid); }
    if(lt){ use_fn(lt); snprintf(cmpf,sizeof cmpf,"KDC%d_cmp",c->id);
        int l1=++gg->labels, l2=++gg->labels;
        buf_printf(o,"\n%s:                       ; ordering by %s.__lt__\n        push ebx\n        push esi\n        mov ebx,[eax]\n        mov esi,[edx]\n%s        push esi\n        push ebx\n",cmpf,c->name,kd_nones(lt,2));
        kd_call_method(o,lt); buf_printf(o,"        add esp,%d\n",4*lt->nparams); kd_truth(o,lt->ret);
        buf_printf(o,"        test eax,eax\n        jz L%d\n        or eax,-1\n        jmp L%d\nL%d:\n%s        push ebx\n        push esi\n",l1,l2,l1,kd_nones(lt,2));
        kd_call_method(o,lt); buf_printf(o,"        add esp,%d\n",4*lt->nparams); kd_truth(o,lt->ret);
        buf_printf(o,"L%d:\n        pop esi\n        pop ebx\n        ret\n",l2); }
    if(hs){ use_fn(hs); snprintf(hashf,sizeof hashf,"KDC%d_hash",c->id);
        buf_printf(o,"\n%s:                       ; %s.__hash__\n%s        push dword [eax]\n",hashf,c->name,kd_nones(hs,1));
        kd_call_method(o,hs); buf_printf(o,"        add esp,%d\n%s        jmp rt_hash_int\n",4*hs->nparams,ty_find(hs->ret)->k==TY_BOOL?"        cdq\n":""); rt("rt_hash_int"); }   /* (an int: all 64 bits in edx:eax) */
    else if(eq) snprintf(hashf,sizeof hashf,"rt_hash_no");
    if(!eq) rt("rt_eq_w");
    if(!lt) rt("rt_cmp_no");
    if(!hs) rt(hashf);
    int fi=fmt_index(ty_new(TY_OBJ,NULL,c),1); rt("rt_sb_need");
    buf_printf(o,"\nKDC%d_repr:                     ; repr of a %s\n        mov eax,[eax]\n        jmp FMT%d\n",c->id,c->name,fi);
    Ty ct; memset(&ct,0,sizeof ct); ct.k=TY_OBJ; ct.cls=c;
    buf_printf(&gg->data,"align 4\nKDC%d dd 4,%d,%s,%s,%s,KDC%d_repr,Z%d   ; objects of %s\n",c->id,gc_type(&ct)?9:1,eqf,cmpf,hashf,c->id,zlit(c->name),c->name);
    return gg->kdlabels[c->id];
}
/* append str()/repr() of the value in eax/st0 (borrowed) to the string builder */
void gen_fmt(F *f, Ty *t, int repr){
    t=ty_find(t);
    if(t->opt && (!is_ptr(t) || t->k==TY_STR || t->k==TY_BYTES)){             /* None prints as None (other references: their formatters) */
        int lnone=new_label(), lend=new_label();
        jump_if_none(f,t,lnone);
        Ty plain=*t; plain.opt=0; gen_fmt(f,&plain,repr); E(f,"jmp L%d",lend);
        LBL(f,lnone); drop_value(f,t,0); E(f,"mov eax,Z%d",zlit("None")); CALLRT(f,"rt_sb_cstr");
        LBL(f,lend); return; }
    switch(t->k){
        case TY_INT: CALLRT(f,"rt_sb_int"); return;
        case TY_BOOL: CALLRT(f,"rt_sb_bool"); return;
        case TY_FLOAT: CALLRT(f,"rt_sb_float"); return;
        case TY_STR: CALLRT(f,repr?"rt_sb_repr_str":"rt_sb_str"); return;
        case TY_BYTES: CALLRT(f,"rt_sb_repr_bytes"); return;
        case TY_TYPE: CALLRT(f,"rt_sb_type"); return;
        case TY_BUF: E(f,"mov eax,Z%d",zlit("<buffer>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_TASK: E(f,"mov eax,Z%d",zlit("<Task>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_FILE: E(f,"mov eax,Z%d",zlit("<file>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_FUNC: CALLRT(f,"rt_sb_funcval"); return;
        case TY_GEN: E(f,"mov eax,Z%d",zlit("<generator object>")); CALLRT(f,"rt_sb_cstr"); return;
        case TY_LIST: case TY_SET: case TY_DICT: case TY_OBJ: case TY_TUPLE:
            if(t->k==TY_LIST && t->view){                                  /* a dict's view: dict_items([...]) */
                static const char *const pre[]={"","dict_keys(","dict_values(","dict_items("};
                Ty *plain=(Ty*)xmalloc(sizeof(Ty)); *plain=*t; plain->view=0;
                E(f,"push eax"); E(f,"mov eax,Z%d",zlit(pre[t->view])); CALLRT(f,"rt_sb_cstr"); E(f,"pop eax");
                rt("rt_sb_need"); E(f,"call FMT%d",fmt_index(plain,repr)); E(f,"mov eax,Z%d",zlit(")")); CALLRT(f,"rt_sb_cstr"); return; }
            if(t->k==TY_SET && t->tup){                                    /* frozenset({...}), frozenset() */
                Ty *plain=(Ty*)xmalloc(sizeof(Ty)); *plain=*t; plain->tup=0; int le=new_label(), ld=new_label();
                E(f,"cmp dword [eax+8],0"); E(f,"je L%d",le);
                E(f,"push eax"); E(f,"mov eax,Z%d",zlit("frozenset(")); CALLRT(f,"rt_sb_cstr"); E(f,"pop eax");
                rt("rt_sb_need"); E(f,"call FMT%d",fmt_index(plain,repr)); E(f,"mov eax,Z%d",zlit(")")); CALLRT(f,"rt_sb_cstr"); E(f,"jmp L%d",ld);
                LBL(f,le); E(f,"mov eax,Z%d",zlit("frozenset()")); CALLRT(f,"rt_sb_cstr"); LBL(f,ld); return; }
            rt("rt_sb_need"); E(f,"call FMT%d",fmt_index(t,repr)); return;
        default: E(f,"mov eax,Z%d",zlit("None")); CALLRT(f,"rt_sb_cstr"); return;     /* a bare None */
    }
}
