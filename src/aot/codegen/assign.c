/* ========================= Typed compiler, code generator: assignments =========================
   Assignment targets, unpacking, augmented assignment. */

#include "gen.h"

/* Store the owned value in eax/st0 (of type vt) into an assignment target. */
/* a, *b, c = value (in eax, owned): b gets a new list of the middle items */
static void gen_star_unpack(F *f, Expr *t, Ty *lt, int star, int line){
    int n=t->count, after=n-1-star;
    hold(f); int slot=f->tmp[f->tmp_used-1];
    if(lt->k==TY_TUPLE){                                     /* a fixed tuple: its items by position */
        int rest=lt->nelems-(n-1);
        for(int i=0;i<star;i++){ Ty *et=lt->elems[i]; E(f,"mov eax,[ebp%+d]",slot); load_mem(f,et,"eax",tuple_off(lt,i)); if(is_ptr(et)) incref(f); store_target(f,t->items[i],et,line); }
        Ty *mt=ty_find(TY(t->items[star])), *el=mt->elem;
        new_list(f,el); E(f,"push eax");
        for(int i=star;i<star+rest;i++){ E(f,"mov eax,[ebp%+d]",slot); load_mem(f,lt->elems[i],"eax",tuple_off(lt,i)); conv_num(f,lt->elems[i],el); if(is_ptr(el)) incref(f); list_append_top(f,el); }
        E(f,"pop eax"); store_target(f,t->items[star],mt,line);
        for(int i=star+1;i<n;i++){ int k=lt->nelems-(n-i); Ty *et=lt->elems[k]; E(f,"mov eax,[ebp%+d]",slot); load_mem(f,et,"eax",tuple_off(lt,k)); if(is_ptr(et)) incref(f); store_target(f,t->items[i],et,line); }
        return;
    }
    Ty *el= lt->k==TY_STR ? TY_STR_T : lt->elem;
    if(lt->k==TY_STR){ E(f,"mov eax,[ebp%+d]",slot); CALLRT(f,"rt_str_chars"); hold(f); slot=f->tmp[f->tmp_used-1]; }
    E(f,"mov eax,[ebp%+d]",slot); E(f,"mov edx,%d",n-1); CALLRT(f,"rt_unpack_atleast");
    for(int i=0;i<star;i++){ E(f,"mov eax,[ebp%+d]",slot); E(f,"mov eax,[eax+16]"); load_mem(f,el,"eax",esize(el)*i); if(is_ptr(el)) incref(f); store_target(f,t->items[i],el,line); }
    E(f,"sub esp,24"); E(f,"mov eax,[ebp%+d]",slot); E(f,"mov [esp],eax");          /* the middle: value[star:len-after] */
    E(f,"mov dword [esp+4],%d",star); E(f,"mov dword [esp+8],%d",-after); E(f,"mov dword [esp+12],0");
    E(f,"mov dword [esp+16],%d",after?4:6); E(f,"mov dword [esp+20],%s",kd_of(el));
    CALLRT(f,"rt_list_slice"); E(f,"add esp,24");
    store_target(f,t->items[star],TY(t->items[star]),line);
    for(int i=star+1;i<n;i++){ E(f,"mov eax,[ebp%+d]",slot); E(f,"mov ecx,[eax+8]"); E(f,"sub ecx,%d",n-i); E(f,"mov eax,[eax+16]");
        if(esize(el)==8) E(f,"lea eax,[eax+ecx*8]"); else E(f,"lea eax,[eax+ecx*4]");
        load_mem(f,el,"eax",0); if(is_ptr(el)) incref(f); store_target(f,t->items[i],el,line); }
}
/* obj.name = value on a frozen dataclass: FrozenInstanceError("cannot assign to field 'name'") */
static void gen_frozen_raise(F *f, Expr *t, const char *what){
    gen_borrow(f,t->a); attr_check(f,t->name);
    AClass *cls=NULL; for(int i=0;i<gg->p->nclasses;i++) if(gg->p->classes[i]->builtin && !strcmp(gg->p->classes[i]->name,"FrozenInstanceError")) cls=gg->p->classes[i];
    char msg[300]; snprintf(msg,sizeof msg,"cannot %s field '%s'",what,t->name);
    use_class(cls);
    E(f,"mov eax,S%d",str_lit(msg)); incref(f); E(f,"push eax");
    E(f,"mov eax,%d",cls->size); E(f,"mov edx,%s",class_label(cls,0)); E(f,"mov ecx,%s",class_label(cls,1)); CALLRT(f,"rt_obj_new");
    E(f,"pop ecx"); E(f,"mov [eax+12],ecx");
    seed_fields(f,cls);
    CALLRT(f,"rt_throw");
}
void store_target(F *f, Expr *t, Ty *vt, int line){
    XInfo *xi=xinfo(t);
    switch(t->kind){
        case EXPR_NAME:{
            if(xi->name && !strcmp(xi->name,"_discard")){ drop_value(f,vt,1); return; }      /* _ = a value of another type */
            AVar *v=xi->kind==X_VAR&&xi->var?xi->var:find_var(f,t->name,line); conv(f,vt,v->ty); store_var(f,v); return; }
        case EXPR_ATTRIBUTE:
            if(xi->kind==X_VAR){ conv(f,vt,xi->var->ty); store_var(f,xi->var); return; }
            if(xi->name && !strcmp(xi->name,"frozen")){ drop_value(f,vt,1); gen_frozen_raise(f,t,"assign to"); return; }
            { AField *fd=xi->field;
              conv(f,vt,fd->ty); push_value(f,fd->ty);
              gen_borrow(f,t->a); attr_check_of(f,t->a,t->name); E(f,"mov ecx,eax");
              if(fd->setoff) E(f,"mov dword [ecx+%d],1",fd->setoff);
              pop_value(f,fd->ty); store_mem(f,fd->ty,"ecx",fd->offset); return; }
        case EXPR_SLICE:{                                     /* xs[a:b:c] = items: the items as a list */
            Ty *v=ty_find(vt); Expr *parts[3]={t->b,t->c,t->d}; int flags=0;
            if(v->k==TY_STR) CALLRT(f,"rt_str_chars");
            else if(v->k==TY_SET){ E(f,"push eax"); E(f,"mov edx,%s",kd_of(v->elem)); CALLRT(f,"rt_set_to_list"); E(f,"xchg eax,[esp]"); CALLRT(f,"rt_decref"); E(f,"pop eax"); }
            else if(v->k==TY_GEN){ E(f,"mov edx,%s",kd_of(v->elem)); CALLRT(f,"rt_gen_drain"); }
            hold(f); int vs=f->tmp[f->tmp_used-1];
            E(f,"sub esp,24"); gen_borrow(f,t->a); check_none(f,TY(t->a),"TypeError","'NoneType' object does not support item assignment"); E(f,"mov [esp],eax");
            for(int k=0;k<3;k++){
                if(parts[k] && parts[k]->kind!=EXPR_NONE){ gen(f,parts[k]); to_i32(f,TY(parts[k])); E(f,"mov [esp+%d],eax",4+4*k); }
                else { flags|=1<<k; E(f,"mov dword [esp+%d],0",4+4*k); } }
            E(f,"mov dword [esp+16],%d",flags); E(f,"mov eax,[ebp%+d]",vs); E(f,"mov [esp+20],eax");
            CALLRT(f,"rt_list_set_slice"); E(f,"add esp,24");
            return; }
        case EXPR_INDEX:{
            Ty *ct=TY(t->a), *el=ct->elem; int dict=ct->k==TY_DICT;
            if(ct->k==TY_OBJ){                                    /* obj[key] = value: obj.__setitem__(key, value) */
                AFunc *m=aot_find_method(ct->cls,"__setitem__"); Ty *kt=m->params[1]->ty, *pv=m->params[2]->ty;
                conv(f,vt,pv); int slot=park_value(f,pv,1);          /* the owned value, released with the statement's temporaries */
                int total=4+esize(kt)+esize(pv);
                E(f,"sub esp,%d",total);
                gen_borrow(f,t->a); E(f,"mov [esp],eax");
                gen_as(f,t->b,kt,0); put_arg(f,kt,4);
                load_mem(f,pv,"ebp",slot); put_arg(f,pv,4+esize(kt));
                call_on_top(f,m); E(f,"add esp,%d",total);
                drop_value(f,m->ret,1);
                return;
            }
            conv(f,vt,el); push_value(f,el);
            gen_borrow(f,t->a); check_none(f,ct,"TypeError","'NoneType' object does not support item assignment"); E(f,"push eax");
            if(dict){ Ty *kt=ty_dkey(ct); gen_as(f,t->b,kt,0); push_value(f,kt);
                E(f,"mov edx,esp"); E(f,"mov eax,[esp+%d]",esize(kt)); CALLRT(f,"rt_dict_slot"); E(f,"add esp,%d",esize(kt)+4); }
            else { gen(f,t->b); to_i32(f,TY(t->b)); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_set_at"); }
            E(f,"mov ecx,eax"); pop_value(f,el); store_mem(f,el,"ecx",0);
            return; }
        case EXPR_TUPLE: case EXPR_LIST:{                    /* a, b = some_list / some_tuple */
            Ty *lt=ty_find(vt), *el=lt->elem;
            int star=-1; for(int i=0;i<t->count;i++) if(t->items[i]->akind==1) star=i;
            if(star>=0){ gen_star_unpack(f,t,lt,star,line); return; }
            hold(f); int slot=f->tmp[f->tmp_used-1];
            if(lt->k==TY_TUPLE){ unpack_tuple_slot(f,slot,lt,t->items,t->count,line); return; }
            if(lt->k==TY_STR){ el=TY_STR_T; CALLRT(f,"rt_str_chars"); hold(f); slot=f->tmp[f->tmp_used-1]; }   /* a, b = "xy" */
            E(f,"mov eax,[ebp%+d]",slot); E(f,"mov edx,0x%x",t->count|(lt->k==TY_STR?0x80000000u:0)); CALLRT(f,"rt_unpack_check");
            for(int i=0;i<t->count;i++){
                E(f,"mov eax,[ebp%+d]",slot); E(f,"mov eax,[eax+16]");
                load_mem(f,el,"eax",esize(el)*i);
                if(is_ptr(el)) incref(f);
                store_target(f,t->items[i],el,line);
            }
            return; }
        default: cg_fail(line,"cannot assign to this expression");
    }
}
/* a borrowed copy of the value in eax/st0 kept until the end of the statement */
int park_value(F *f, Ty *t, int owned){
    if(is_ptr(t)){ if(!owned) incref(f); hold(f); return f->tmp[f->tmp_used-1]; }
    int slot=frame_slot(f,8);
    store_new(f,t,"ebp",slot);
    return slot;
}
static void load_parked(F *f, Ty *t, int slot){
    load_mem(f,t,"ebp",slot);
    if(is_ptr(t)) incref(f);
}

static void gen_augassign(F *f, AAssign *a, int line){
    Expr *t=a->target[0], *v=a->value;
    Ty *cur=TY(t), *tv=TY(v); int num=numeric_ty(cur);
    XInfo *xi=xinfo(t);
    if(a->opfn){                                             /* v += w with __add__ & co: v = v.__add__(w) */
        AFunc *m=a->opfn; Ty *pt=m->params[1]->ty; int total=op_frame(m);
        if(t->kind==EXPR_INDEX && TY(t->a)->k==TY_OBJ) cg_fail(line,"augmented assignment to obj[key] is not supported in compiled code");
        E(f,"sub esp,%d",total);
        gen_borrow(f,t); E(f,"mov [esp],eax");
        gen_as(f,v,pt,0); put_arg(f,pt,4);
        op_defaults(f,m,2);
        call_on_top(f,m); E(f,"add esp,%d",total);
        store_target(f,t,m->ret,line);
        return;
    }
    if(cur->k==TY_LIST && !cur->tup && a->aug==T_PLUS_ASSIGN){   /* list += x extends in place (a tuple: a new one) */
        gen_borrow(f,t); E(f,"push eax");
        if(TY(v)->k==TY_LIST) gen_borrow(f,v); else { gen_list_of(f,v,cur->elem); hold(f); }
        E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_list_extend");
        return;
    }
    if(cur->k==TY_SET && TY(v)->k==TY_SET && (a->aug==T_PIPE_ASSIGN||a->aug==T_AMP_ASSIGN||a->aug==T_MINUS_ASSIGN||a->aug==T_CARET_ASSIGN)){
        gen_borrow(f,t); E(f,"push eax"); gen_borrow(f,v);      /* in place, as CPython's set does */
        E(f,"mov edx,eax"); E(f,"pop eax");
        E(f,"mov ecx,%d",a->aug==T_PIPE_ASSIGN?0:a->aug==T_AMP_ASSIGN?1:a->aug==T_MINUS_ASSIGN?2:3); CALLRT(f,"rt_set_iop");
        return;
    }
    const char *mleft=binop_none_msg(a->aug,cur,tv,1), *mright=binop_none_msg(a->aug,cur,tv,0);
    if(t->kind==EXPR_NAME || (t->kind==EXPR_ATTRIBUTE && xi->kind==X_VAR)){
        AVar *var=xi->var?xi->var:find_var(f,t->name,line);
        load_var(f,var); check_none(f,cur,"TypeError",mleft); push_value(f,cur);
        gen_as(f,v,num?cur:tv,0); check_none(f,tv,"TypeError",mright);
        int o=apply_binop(f,a->aug,cur,cur,tv,line);
        if(is_ptr(cur) && !o) incref(f);
        store_var(f,var);
        return;
    }
    if(t->kind==EXPR_ATTRIBUTE){
        AField *fd=xi->field;
        if(xi->name && !strcmp(xi->name,"frozen")){ gen_borrow(f,t->a); attr_check(f,t->name); load_field(f,fd); gen_frozen_raise(f,t,"assign to"); return; }   /* (the value is never computed) */
        gen_borrow(f,t->a); attr_check_of(f,t->a,t->name); E(f,"push eax");
        load_field(f,fd); check_none(f,cur,"TypeError",mleft); push_value(f,cur);
        gen_as(f,v,num?cur:tv,0); check_none(f,tv,"TypeError",mright);
        int o=apply_binop(f,a->aug,cur,cur,tv,line);
        if(is_ptr(cur) && !o) incref(f);
        E(f,"pop ecx"); if(fd->setoff) E(f,"mov dword [ecx+%d],1",fd->setoff);
        store_mem(f,cur,"ecx",fd->offset);
        return;
    }
    if(t->kind==EXPR_INDEX){
        Ty *ct=TY(t->a); int es=esize(cur), dict=ct->k==TY_DICT;
        Ty *kt=dict?ty_dkey(ct):TY_INT_T; int ks=dict?esize(kt):4;
        const char *at=dict?"rt_dict_get":"rt_list_at";
        gen_borrow(f,t->a); check_none(f,ct,"TypeError","'NoneType' object is not subscriptable"); E(f,"push eax");
        if(dict){ gen_as(f,t->b,kt,0); push_value(f,kt); } else { gen(f,t->b); to_i32(f,TY(t->b)); E(f,"push eax"); }   /* [esp] key, then the container */
        if(dict) E(f,"mov edx,esp"); else E(f,"mov edx,[esp]");
        E(f,"mov eax,[esp+%d]",ks); CALLRT(f,at);
        load_mem(f,cur,"eax",0); check_none(f,cur,"TypeError",mleft); push_value(f,cur);
        gen_as(f,v,num?cur:tv,0); check_none(f,tv,"TypeError",mright);
        int o=apply_binop(f,a->aug,cur,cur,tv,line);
        if(is_ptr(cur) && !o) incref(f);
        push_value(f,cur);
        if(dict) E(f,"lea edx,[esp+%d]",es); else E(f,"mov edx,[esp+%d]",es);
        E(f,"mov eax,[esp+%d]",es+ks); CALLRT(f,at);
        E(f,"mov ecx,eax"); pop_value(f,cur); store_mem(f,cur,"ecx",0);
        E(f,"add esp,%d",ks+4);
        return;
    }
    cg_fail(line,"invalid target of augmented assignment");
}

void gen_assign(F *f, Stmt *s, AAssign *a){
    if(!a->value) return;
    if(a->aug){ gen_augassign(f,a,s->line); return; }
    Expr *v=a->value;
    int parallel=v->kind==EXPR_TUPLE;                         /* same rule as the checker */
    for(int k=0;k<a->ntarget && parallel;k++){ Expr *t=a->target[k]; if((t->kind!=EXPR_TUPLE&&t->kind!=EXPR_LIST)||t->count!=v->count) parallel=0; }
    if(parallel){                                             /* a, b = x, y: evaluate everything first */
        int n=v->count, slots[64];
        if(n>64) cg_fail(s->line,"too many values");
        for(int i=0;i<n;i++){ Ty *t=TY(v->items[i]); int o=gen(f,v->items[i]); slots[i]=park_value(f,t,o); }
        for(int k=0;k<a->ntarget;k++) for(int i=0;i<n;i++){
            Ty *t=TY(v->items[i]); load_parked(f,t,slots[i]); store_target(f,a->target[k]->items[i],t,s->line); }
        return;
    }
    Ty *t=TY(v);
    if(a->ntarget==1){
        Expr *tg=a->target[0];
        if(v->kind==EXPR_NONE){ Ty *want=TY(tg); if(tg->kind==EXPR_NAME||tg->kind==EXPR_ATTRIBUTE||tg->kind==EXPR_INDEX){ gen_none(f,want); store_target(f,tg,want,s->line); return; } }
        gen_owned(f,v); store_target(f,tg,t,s->line); return;
    }
    int o=gen(f,v), slot=park_value(f,t,o);
    for(int k=0;k<a->ntarget;k++){ load_parked(f,t,slot); store_target(f,a->target[k],t,s->line); }
}

