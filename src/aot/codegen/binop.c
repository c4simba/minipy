/* ========================= Typed compiler, code generator: binary operators =========================
   Arithmetic and the other binary operators: built-in, or calls of
   __add__ ... methods. */

#include "gen.h"

/* ---- binary operators: left operand on the machine stack, right in eax/st0 ---- */
int numeric_ty(Ty *t){ t=ty_find(t); return t->k==TY_INT||t->k==TY_BOOL||t->k==TY_FLOAT; }
int apply_binop(F *f, TokKind op, Ty *t, Ty *ta, Ty *tb, int line){
    t=ty_find(t); ta=ty_find(ta); tb=ty_find(tb);
    switch(op){ case T_PLUS_ASSIGN: op=T_PLUS; break; case T_MINUS_ASSIGN: op=T_MINUS; break; case T_STAR_ASSIGN: op=T_STAR; break;
        case T_SLASH_ASSIGN: op=T_SLASH; break; case T_FLOOR_DIV_ASSIGN: op=T_FLOOR_DIV; break; case T_PERCENT_ASSIGN: op=T_PERCENT; break;
        case T_POWER_ASSIGN: op=T_POWER; break; case T_AMP_ASSIGN: op=T_AMP; break; case T_PIPE_ASSIGN: op=T_PIPE; break;
        case T_CARET_ASSIGN: op=T_CARET; break; case T_SHL_ASSIGN: op=T_SHL; break; case T_SHR_ASSIGN: op=T_SHR; break; default: break; }
    if(t->k==TY_FLOAT){
        E(f,"fld qword [esp]"); E(f,"add esp,8");               /* st0 = a, st1 = b */
        switch(op){
            case T_PLUS: E(f,"faddp st1,st0"); break;
            case T_MINUS: E(f,"fsubrp st1,st0"); break;
            case T_STAR: E(f,"fmulp st1,st0"); break;
            case T_SLASH: CALLRT(f,"rt_fdiv"); break;
            case T_FLOOR_DIV: CALLRT(f,"rt_ffloordiv"); break;
            case T_PERCENT: CALLRT(f,"rt_fmod"); break;
            case T_POWER: CALLRT(f,"rt_fpow"); break;
            default: cg_fail(line,"unsupported float operator");
        }
        return 0;
    }
    if(t->k==TY_INT){                                         /* 64 bits: the left operand on the stack, the right in edx:eax */
        rt("rt_int_overflow");
        switch(op){
            case T_PLUS: E(f,"add eax,[esp]"); E(f,"adc edx,[esp+4]"); E(f,"jo rt_int_overflow"); break;
            case T_MINUS: E(f,"mov ecx,eax"); E(f,"mov eax,[esp]"); E(f,"sub eax,ecx"); E(f,"mov ecx,edx"); E(f,"mov edx,[esp+4]"); E(f,"sbb edx,ecx"); E(f,"jo rt_int_overflow"); break;
            case T_STAR: CALLRT(f,"rt_imul"); break;
            case T_AMP: E(f,"and eax,[esp]"); E(f,"and edx,[esp+4]"); break;
            case T_PIPE: E(f,"or eax,[esp]"); E(f,"or edx,[esp+4]"); break;
            case T_CARET: E(f,"xor eax,[esp]"); E(f,"xor edx,[esp+4]"); break;
            case T_SHL: CALLRT(f,"rt_ishl"); break;
            case T_SHR: CALLRT(f,"rt_isar"); break;
            case T_FLOOR_DIV: CALLRT(f,"rt_idivmod"); break;
            case T_PERCENT: CALLRT(f,"rt_idivmod"); E(f,"mov eax,[rt_scratch]"); E(f,"mov edx,[rt_scratch+4]"); break;
            case T_POWER: CALLRT(f,"rt_ipow"); break;
            default: cg_fail(line,"unsupported int operator");
        }
        E(f,"add esp,8");
        return 0;
    }
    if(t->k==TY_BOOL){                                        /* bool & | ^ bool */
        E(f,"mov ecx,eax"); E(f,"pop eax");
        switch(op){
            case T_AMP: E(f,"and eax,ecx"); break;
            case T_PIPE: E(f,"or eax,ecx"); break;
            case T_CARET: E(f,"xor eax,ecx"); break;
            default: cg_fail(line,"unsupported bool operator");
        }
        return 0;
    }
    if(t->k==TY_STR||t->k==TY_BYTES||t->k==TY_LIST){
        if(op==T_STAR){                                       /* x * n, n * x: n as a 32-bit count */
            if(ta->k==t->k){ to_i32(f,tb); E(f,"mov edx,eax"); E(f,"pop eax"); }
            else { E(f,"mov ecx,eax"); E(f,"pop eax"); if(is_lng(ta)){ E(f,"pop edx"); to_i32(f,ta); } E(f,"mov edx,eax"); E(f,"mov eax,ecx"); }
            if(t->k==TY_STR||t->k==TY_BYTES){ CALLRT(f,"rt_str_mul"); return 1; }
            E(f,"mov ecx,%s",kd_of(t->elem)); CALLRT(f,"rt_list_mul"); return 1;
        }
        E(f,"mov edx,eax"); E(f,"pop eax");
        if(t->k==TY_STR||t->k==TY_BYTES){ if(op==T_PLUS){ CALLRT(f,"rt_str_concat"); return 1; } }
        else if(op==T_PLUS){ E(f,"mov ecx,%s",kd_of(t->elem)); CALLRT(f,"rt_list_concat"); return 1; }
    }
    if(t->k==TY_SET){
        int code=op==T_PIPE?0:op==T_AMP?1:op==T_MINUS?2:3;
        E(f,"mov edx,eax"); E(f,"pop eax"); E(f,"mov ecx,%d",code); CALLRT(f,"rt_set_op"); return 1;
    }
    (void)tb;
    cg_fail(line,"unsupported operator");
}

/* self.m(arg): an operator implemented by a method -> its result (owned if a reference) */
void call_on_top(F *f, AFunc *m){
    use_fn(m);
    E(f,"mov eax,[esp]"); attr_check(f,m->name);
    if(m->overridden){ E(f,"mov eax,[eax+8]"); E(f,"call dword [eax+%d]",8+4*m->vslot); }
    else E(f,"call %s",fnl(m,LB_CODE));
}
/* the size of the arguments of an operator method's call: self, then the rest (the operand, defaults) */
int op_frame(AFunc *m){ int t=4; for(int i=1;i<m->nparams;i++) t+=esize(m->params[i]->ty); return t; }
/* the defaults of its parameters after the used ones (constants: see op_method_of) */
void op_defaults(F *f, AFunc *m, int used){
    int off=4; for(int i=1;i<used && i<m->nparams;i++) off+=esize(m->params[i]->ty);
    for(int i=used;i<m->nparams;i++){ Ty *pt=m->params[i]->ty; gen_as(f,m->defaults[i],pt,m->params[i]->consumed); put_arg(f,pt,off); off+=esize(pt); }
}
/* self.__r<op>__(arg) for `arg <op> self`: arg (the left operand) evaluated first */
int gen_op_call_r(F *f, AFunc *m, Expr *self, Expr *arg){
    Ty *pt=m->params[1]->ty; int total=op_frame(m);
    E(f,"sub esp,%d",total);
    gen_as(f,arg,pt,0); put_arg(f,pt,4);
    op_defaults(f,m,2);
    gen_borrow(f,self); E(f,"mov [esp],eax");
    call_on_top(f,m);
    E(f,"add esp,%d",total);
    return is_ptr(m->ret);
}
int gen_op_call(F *f, AFunc *m, Expr *self, Expr *arg){
    int total=op_frame(m);
    E(f,"sub esp,%d",total);
    gen_borrow(f,self); E(f,"mov [esp],eax");
    if(arg){ Ty *pt=m->params[1]->ty; gen_as(f,arg,pt,0); put_arg(f,pt,4); }
    op_defaults(f,m,arg?2:1);
    call_on_top(f,m);
    E(f,"add esp,%d",total);
    return is_ptr(m->ret);
}
/* Python's spelling of a binary operator (for messages) */
static const char *op_symbol(TokKind op){
    switch(op){
        case T_PLUS: case T_PLUS_ASSIGN: return "+"; case T_MINUS: case T_MINUS_ASSIGN: return "-";
        case T_STAR: case T_STAR_ASSIGN: return "*"; case T_SLASH: case T_SLASH_ASSIGN: return "/";
        case T_FLOOR_DIV: case T_FLOOR_DIV_ASSIGN: return "//"; case T_PERCENT: case T_PERCENT_ASSIGN: return "%";
        case T_POWER: case T_POWER_ASSIGN: return "** or pow()"; case T_AMP: case T_AMP_ASSIGN: return "&";
        case T_PIPE: case T_PIPE_ASSIGN: return "|"; case T_CARET: case T_CARET_ASSIGN: return "^";
        case T_SHL: case T_SHL_ASSIGN: return "<<"; case T_SHR: case T_SHR_ASSIGN: return ">>"; default: return "?";
    }
}
static int is_aug_op(TokKind op){ return op==T_PLUS_ASSIGN||op==T_MINUS_ASSIGN||op==T_STAR_ASSIGN||op==T_SLASH_ASSIGN||op==T_FLOOR_DIV_ASSIGN||op==T_PERCENT_ASSIGN||op==T_POWER_ASSIGN||op==T_AMP_ASSIGN||op==T_PIPE_ASSIGN||op==T_CARET_ASSIGN||op==T_SHL_ASSIGN||op==T_SHR_ASSIGN; }
/* the TypeError of `None op b` (left) / `a op None` (right) */
const char *binop_none_msg(TokKind op, Ty *ta, Ty *tb, int left){
    const char *sym=op_symbol(op); char aug[24]; snprintf(aug,sizeof aug,"%s%s",sym,is_aug_op(op)&&op!=T_POWER_ASSIGN?"=":"");
    if(op==T_POWER_ASSIGN) snprintf(aug,sizeof aug,"**=");
    Ty *x=ty_find(left?tb:ta);
    if(!left && (op==T_PLUS||op==T_PLUS_ASSIGN) && (x->k==TY_STR||(x->k==TY_LIST&&!x->tup))) return none_msg("can only concatenate %s (not \"NoneType\") to %s",py_type_name(x),py_type_name(x));
    if((op==T_STAR||op==T_STAR_ASSIGN) && !left && (x->k==TY_STR||x->k==TY_LIST)) return none_msg("can't multiply sequence by non-int of type 'NoneType'");
    return left ? none_msg("unsupported operand type(s) for %s: 'NoneType' and '%s'",aug,py_type_name(x))
                : none_msg("unsupported operand type(s) for %s: '%s' and 'NoneType'",aug,py_type_name(x));
}
int gen_binary(F *f, Expr *e){
    Ty *t=TY(e), *ta=TY(e->a), *tb=TY(e->b);
    if(xinfo(e)->kind==X_OPMETHOD) return xinfo(e)->reflected ? gen_op_call_r(f,xinfo(e)->fn,e->b,e->a) : gen_op_call(f,xinfo(e)->fn,e->a,e->b);
    if(e->op==T_POWER && t->k==TY_FLOAT && e->b->kind==EXPR_LITERAL && e->b->tok->is_float && e->b->tok->f==0.5){
        g_none_text=binop_none_msg(e->op,ta,tb,1); gen_as(f,e->a,TY_FLOAT_T,0); g_none_text=NULL; E(f,"fsqrt"); return 0; }   /* x ** 0.5: exact */
    if(e->op==T_PERCENT && ta->k==TY_STR) return gen_format(f,e);
    if(e->op==T_PLUS && t->k==TY_STR && concat_worth(e) && !is_opt(ta) && !is_opt(tb)) return gen_concat(f,e);
    int num=numeric_ty(t);
    char *ma=xstrdup2(binop_none_msg(e->op,ta,tb,1)), *mb=xstrdup2(binop_none_msg(e->op,ta,tb,0));   /* (an operand's own code may set g_none_text) */
    g_none_text=ma;
    gen_as(f,e->a,num?t:ta,0); if(!num) check_none(f,ta,"TypeError",ma);
    push_value(f,num?t:ta);
    g_none_text=mb;
    gen_as(f,e->b,num?t:tb,0); if(!num) check_none(f,tb,"TypeError",mb);
    g_none_text=NULL;
    if(e->op==T_SLASH && t->k==TY_FLOAT && e->b->kind==EXPR_LITERAL && e->b->tok->kind==T_NUMBER
       && (e->b->tok->is_float ? e->b->tok->f!=0 : e->b->tok->i!=0)){          /* x / 1e9: no zero to check for */
        E(f,"fld qword [esp]"); E(f,"add esp,8"); E(f,"fxch"); E(f,"fdivp st1,st0"); return 0; }
    return apply_binop(f,e->op,t,ta,tb,e->line);
}

