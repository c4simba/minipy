/* ========================= Typed compiler, code generator: frame and variables =========================
   Frame slots, temporaries parked until the end of a statement;
   loading and storing variables, fields and elements. */

#include "gen.h"

/* ---- frame ---- */
int frame_slot(F *f, int size){ f->frame+=size; return -(12+f->frame); }
void add_ref_slot(F *f, int off){
    if(f->nrefs==f->crefs){ f->crefs=f->crefs?f->crefs*2:16; f->refs=(int*)xrealloc(f->refs,sizeof(int)*(size_t)f->crefs); }
    f->refs[f->nrefs++]=off;
}
/* Park the owned reference in eax until the end of the current statement. */
void hold(F *f){
    if(f->tmp_used==f->ntmp){
        if(f->ntmp==f->ctmp){ f->ctmp=f->ctmp?f->ctmp*2:8; f->tmp=(int*)xrealloc(f->tmp,sizeof(int)*(size_t)f->ctmp); }
        int off=frame_slot(f,4); f->tmp[f->ntmp++]=off; add_ref_slot(f,off);
    }
    E(f,"mov [ebp%+d],eax",f->tmp[f->tmp_used++]);
}
int scope_open(F *f){ return f->tmp_used; }
void scope_close(F *f, int mark){
    while(f->tmp_used>mark){
        int off=f->tmp[--f->tmp_used];
        E(f,"lea eax,[ebp%+d]",off); CALLRT(f,"rt_release");
    }
}
/* A reference slot of its own (loop iterables, comprehension results). */
int ref_slot(F *f){ int off=frame_slot(f,4); add_ref_slot(f,off); return off; }

/* ---- variables and memory ---- */
int is_fn_body(AFunc *f){ return f->def||f->lam||f->genexp; }   /* not a module body */
AVar *root_var(AVar *v){ while(v->src) v=v->src; return v; }
/* A function value: +0 refcount, +4 destroy, +8 code, +12 its name (str),
   captured values from +16: a copy, or the pointer of a shared cell. */
/* the defaults of fn's function objects: at 16 on, in parameter order (positional ones first: a value call fills them from there) */
void layout_defaults(AFunc *fn){
    if(fn->deflaid) return;
    fn->deflaid=1; int off=0;
    for(int i=0;i<fn->nparams && i<16;i++){ if(!fn->defaults[i] || i==fn->star || i==fn->dstar) continue; fn->defoff[i]=off; off+=ty_size(fn->params[i]->ty); }
    fn->defsize=off;
}
/* a default evaluated where its function is defined (not a constant put in at the call) */
int dyn_defaults(AFunc *fn){
    for(int i=0;i<fn->nparams;i++) if(fn->defaults[i] && i!=fn->star && i!=fn->dstar && !const_default(fn->defaults[i])) return 1;
    return 0;
}
int has_defaults(AFunc *fn){ for(int i=0;i<fn->nparams;i++) if(fn->defaults[i] && i!=fn->star && i!=fn->dstar) return 1; return 0; }
void layout_caps(AFunc *fn){
    layout_defaults(fn);
    if(fn->capsize || !fn->ncaps) return;
    int off=16+fn->defsize;
    for(int i=0;i<fn->ncaps;i++){ AVar *v=fn->caps[i]; v->capoff=off; off+=root_var(v)->cell?4:ty_size(v->ty); }
    fn->capsize=off-16;
}
const char *fn_display_name(AFunc *g){ return g->shown?g->shown:g->lam?"<lambda>":g->genexp?"<genexpr>":g->name; }
static void var_addr_at(char *buf, size_t n, AVar *v, int extra){
    if(v->global){ if(extra) snprintf(buf,n,"[%s+%d]",global_label(v),extra); else snprintf(buf,n,"[%s]",global_label(v)); }
    else snprintf(buf,n,"[ebp%+d]",v->offset+extra);
}
void var_addr(char *buf, size_t n, AVar *v){ var_addr_at(buf,n,v,0); }
/* v of `del v` somewhere: UnboundLocalError / NameError unless it holds a value */
void check_bound(F *f, AVar *v){
    char a[320]; var_addr(a,sizeof a,v->bound); int l=new_label(); char msg[400];
    E(f,"cmp dword %s,0",a); E(f,"jne L%d",l);
    if(v->global){ snprintf(msg,sizeof msg,"name '%s' is not defined",v->name); E(f,"mov esi,Z%d",zlit(msg)); CALLRT(f,"rt_nameerr_text"); }
    else { snprintf(msg,sizeof msg,"cannot access local variable '%s' where it is not associated with a value",v->name); E(f,"mov esi,Z%d",zlit(msg)); CALLRT(f,"rt_unbound_text"); }
    LBL(f,l);
}
void load_var(F *f, AVar *v){
    if(v->bound) check_bound(f,v);
    if(v->src){                                         /* captured: in the closure (or its cell) */
        layout_caps(v->owner);
        E(f,"mov ecx,[ebp%+d]",f->env);
        if(root_var(v)->cell){ E(f,"mov ecx,[ecx+%d]",v->capoff); load_mem(f,v->ty,"ecx",8); }
        else load_mem(f,v->ty,"ecx",v->capoff);
        return;
    }
    if(v->cell){ E(f,"mov ecx,[ebp%+d]",v->offset); load_mem(f,v->ty,"ecx",8); return; }
    char a[320]; var_addr(a,sizeof a,v);
    if(is_flt(v->ty)) E(f,"fld qword %s",a);
    else if(is_lng(v->ty)){ char h[320]; var_addr_at(h,sizeof h,v,4); E(f,"mov eax,%s",a); E(f,"mov edx,%s",h); }
    else E(f,"mov eax,%s",a);
}
/* Store eax/st0 (an owned reference for reference types) into the variable. */
void store_var(F *f, AVar *v){
    if(v->src){ layout_caps(v->owner); E(f,"mov ecx,[ebp%+d]",f->env); E(f,"mov ecx,[ecx+%d]",v->capoff); store_mem(f,v->ty,"ecx",8); return; }   /* nonlocal: a cell */
    if(v->cell){ E(f,"mov ecx,[ebp%+d]",v->offset); store_mem(f,v->ty,"ecx",8); return; }
    char a[320]; var_addr(a,sizeof a,v);
    if(!v->global && v->id>=0 && v->id<f->fn->nparams && f->fn->params[v->id]==v) f->param_stored[v->id]=1;
    if(is_flt(v->ty)) E(f,"fstp qword %s",a);
    else if(is_ptr(v->ty)){ rt("rt_decref"); E(f,"xchg eax,%s",a); E(f,"call rt_decref"); }
    else if(is_lng(v->ty)){ char h[320]; var_addr_at(h,sizeof h,v,4); E(f,"mov %s,eax",a); E(f,"mov %s,edx",h); }
    else E(f,"mov %s,eax",a);
    if(v->bound){ char b[320]; var_addr(b,sizeof b,v->bound); E(f,"mov dword %s,1",b); }   /* it holds a value again */
}
/* Store eax/st0 (owned) at [reg+off], releasing the previous reference. */
void store_mem(F *f, Ty *t, const char *reg, int off){
    if(is_flt(t)) E(f,"fstp qword [%s%+d]",reg,off);
    else if(is_ptr(t)){ rt("rt_decref"); E(f,"xchg eax,[%s%+d]",reg,off); E(f,"call rt_decref"); }
    else if(is_lng(t)){ E(f,"mov [%s%+d],eax",reg,off); E(f,"mov [%s%+d],edx",reg,off+4); }
    else E(f,"mov [%s%+d],eax",reg,off);
}
/* Store into a fresh (zeroed or garbage) slot: nothing to release. */
void store_new(F *f, Ty *t, const char *reg, int off){
    if(is_flt(t)) E(f,"fstp qword [%s%+d]",reg,off);
    else if(is_lng(t)){ E(f,"mov [%s%+d],eax",reg,off); E(f,"mov [%s%+d],edx",reg,off+4); }
    else E(f,"mov [%s%+d],eax",reg,off);
}
void load_mem(F *f, Ty *t, const char *reg, int off){
    if(is_flt(t)) E(f,"fld qword [%s%+d]",reg,off);
    else if(is_lng(t)){
        if(!strcmp(reg,"edx")){ E(f,"mov eax,[edx%+d]",off); E(f,"mov edx,[edx%+d]",off+4); }
        else { E(f,"mov edx,[%s%+d]",reg,off+4); E(f,"mov eax,[%s%+d]",reg,off); } }
    else E(f,"mov eax,[%s%+d]",reg,off);
}
/* eax/st0 = the element at [base+idx*scale] (scale: the slot size) */
void load_at(F *f, Ty *t, const char *base, const char *idx, int scale){
    if(is_flt(t)) E(f,"fld qword [%s+%s*%d]",base,idx,scale);
    else if(is_lng(t)){ E(f,"mov edx,[%s+%s*%d+4]",base,idx,scale); E(f,"mov eax,[%s+%s*%d]",base,idx,scale); }
    else E(f,"mov eax,[%s+%s*%d]",base,idx,scale);
}
