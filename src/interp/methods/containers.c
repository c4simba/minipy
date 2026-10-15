/* ========================= Interpreter, methods: list, dict, set =========================
   The methods of list, dict and set. */

#include "methods.h"

static ListObj *L(Value v){ return AS_LIST(v); }
static Value m_append(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("list.append",argc,1,1); mp_list_append(SELF,argv[1]); return v_none(); }
static Value m_extend(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("list.extend",argc,1,1);
    Value src= argv[1].k==V_OBJ && argv[1].u.o==SELF.u.o ? mp_list(L(SELF)->len,L(SELF)->items) : argv[1];
    if(IS_LIST(src)||IS_TUPLE(src)){ int64_t n= IS_LIST(src)?L(src)->len:AS_TUPLE(src)->len; Value *it= IS_LIST(src)?L(src)->items:AS_TUPLE(src)->items; for(int64_t i=0;i<n;i++) mp_list_append(SELF,it[i]); }
    else { Value it=mp_iter(src), x; while(mp_next(it,&x)) mp_list_append(SELF,x); }
    return v_none(); }
static Value m_insert(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("insert",argc,2,2);
    ListObj *l=L(SELF); int64_t i=mp_index(argv[1],"index");
    if(i<0){ i+=l->len; if(i<0) i=0; } if(i>l->len) i=l->len;
    mp_list_append(SELF,v_none()); l=L(SELF);
    memmove(l->items+i+1,l->items+i,sizeof(Value)*(size_t)(l->len-1-i)); l->items[i]=argv[2];
    return v_none(); }
static Value m_lpop(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("pop",argc,0,1);
    ListObj *l=L(SELF); if(!l->len) mp_raise_t(E_IndexError,"pop from empty list");
    int64_t i= argc>1 ? mp_index(argv[1],"index") : l->len-1;
    if(i<0) i+=l->len;
    if(i<0||i>=l->len) mp_raise_t(E_IndexError,"pop index out of range");
    Value v=l->items[i]; memmove(l->items+i,l->items+i+1,sizeof(Value)*(size_t)(l->len-i-1)); l->len--; return v; }
static int item_eq(Value a, Value b){ return (a.k==V_OBJ && b.k==V_OBJ && a.u.o==b.u.o) || mp_eq(a,b); }
static Value m_remove(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("remove",argc,1,1);
    ListObj *l=L(SELF);
    for(int64_t i=0;i<l->len;i++) if(item_eq(l->items[i],argv[1])){ l=L(SELF); if(i<l->len){ memmove(l->items+i,l->items+i+1,sizeof(Value)*(size_t)(l->len-i-1)); l->len--; } return v_none(); }
    mp_raise_t(E_ValueError,"list.remove(x): x not in list"); }
static Value m_lindex(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("index",argc,1,3);
    int is_list=IS_LIST(SELF); int64_t n= is_list?L(SELF)->len:AS_TUPLE(SELF)->len;
    int64_t a= argc>2 ? mp_index(argv[2],"start") : 0, e= argc>3 ? mp_index(argv[3],"stop") : n;
    if(a<0){ a+=n; if(a<0) a=0; } if(e<0){ e+=n; if(e<0) e=0; } if(e>n) e=n;
    for(int64_t i=a;i<e;i++){ Value x= is_list?L(SELF)->items[i]:AS_TUPLE(SELF)->items[i]; if(item_eq(x,argv[1])) return v_int(i); if(is_list && L(SELF)->len<e) e=L(SELF)->len; }
    mp_raise_t(E_ValueError,is_list?"list.index(x): x not in list":"tuple.index(x): x not in tuple"); }
static Value m_lcount(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("count",argc,1,1);
    int is_list=IS_LIST(SELF); int64_t n= is_list?L(SELF)->len:AS_TUPLE(SELF)->len, c=0;
    for(int64_t i=0;i<n;i++){ Value x= is_list?L(SELF)->items[i]:AS_TUPLE(SELF)->items[i]; if(item_eq(x,argv[1])) c++; }
    return v_int(c); }
static Value m_reverse(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("reverse",argc,0,0); ListObj *l=L(SELF);
    for(int64_t i=0,j=l->len-1;i<j;i++,j--){ Value t=l->items[i]; l->items[i]=l->items[j]; l->items[j]=t; } return v_none(); }
static Value m_lcopy(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("copy",argc,0,0); return mp_list(L(SELF)->len,L(SELF)->items); }
static Value m_lclear(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("clear",argc,0,0); L(SELF)->len=0; return v_none(); }
static Value m_sort(int argc, Value *argv, TupleObj *kw){
    if(npos_of(argc,kw)>1) mp_raise_t(E_TypeError,"sort() takes no positional arguments");
    Value key=kwarg(argc,argv,kw,"key",v_none()), rev=kwarg(argc,argv,kw,"reverse",v_bool(0));
    if(kw) for(int64_t i=0;i<kw->len;i++){ const char *n=mp_cstr(kw->items[i]); if(strcmp(n,"key")&&strcmp(n,"reverse")) mp_raise_t(E_TypeError,"sort() got an unexpected keyword argument '%s'",n); }
    mp_sort(SELF,key,mp_truth(rev));
    return v_none();
}

/* ---------------------------------------------------------------- dict */
static DictObj *D(Value v){ return AS_DICT(v); }
static Value view(Type *t, Value d){ IterObj *v=(IterObj*)mp_alloc(t,sizeof(IterObj)); v->src=d; v->aux=v_undef(); return v_obj(v); }
static Value m_keys(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("keys",argc,0,0); return view(T_dict_keys,SELF); }
static Value m_values(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("values",argc,0,0); return view(T_dict_values,SELF); }
static Value m_items(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("items",argc,0,0); return view(T_dict_items,SELF); }
static Value m_get(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("get",argc,1,2); Value v; if(mp_dict_get(D(SELF),argv[1],&v)) return v; return argc>2?argv[2]:v_none(); }
static Value m_dpop(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("pop",argc,1,2); Value v;
    if(mp_dict_get(D(SELF),argv[1],&v)){ mp_dict_del(D(SELF),argv[1]); return v; }
    if(argc>2) return argv[2];
    mp_raise(mp_exc_args(E_KeyError,mp_tuple(1,&argv[1]))); }
static Value m_popitem(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("popitem",argc,0,0); DictObj *d=D(SELF);
    for(int64_t i=d->nent-1;i>=0;i--) if(d->ent[i].key.k!=V_UNDEF){ Value kv[2]={d->ent[i].key,d->ent[i].val}; mp_dict_del(d,kv[0]); return mp_tuple(2,kv); }
    mp_raise_t(E_KeyError,"popitem(): dictionary is empty"); }
static Value m_setdefault(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("setdefault",argc,1,2); Value v;
    if(mp_dict_get(D(SELF),argv[1],&v)) return v;
    v= argc>2 ? argv[2] : v_none(); mp_dict_set(D(SELF),argv[1],v); return v; }
void mp_dict_update_from(DictObj *d, Value src){
    if(src.k==V_OBJ && src.u.o->type->layout==LY_DICT){ int64_t pos=0; Value k, v; Value copy=mp_list(0,NULL);
        while(mp_dict_next(AS_DICT(src),&pos,&k,&v)){ Value kv[2]={k,v}; mp_list_append(copy,mp_tuple(2,kv)); }
        for(int64_t i=0;i<AS_LIST(copy)->len;i++){ TupleObj *t=AS_TUPLE(AS_LIST(copy)->items[i]); mp_dict_set(d,t->items[0],t->items[1]); }
        return; }
    Value keys;
    if(mp_getattr_opt(src,mp_intern("keys"),&keys)){ Value it=mp_iter(mp_call0(keys)), k; while(mp_next(it,&k)) mp_dict_set(d,k,mp_getitem(src,k)); return; }
    Value it=mp_iter(src), x; int64_t i=0;
    while(mp_next(it,&x)){
        Value l=mp_list_of(x);
        if(AS_LIST(l)->len!=2) mp_raise_t(E_ValueError,"dictionary update sequence element #%lld has length %lld; 2 is required",(long long)i,(long long)AS_LIST(l)->len);
        mp_dict_set(d,AS_LIST(l)->items[0],AS_LIST(l)->items[1]); i++;
    }
}
static Value m_update(int argc, Value *argv, TupleObj *kw){
    int np=npos_of(argc,kw);
    if(np>2) mp_raise_t(E_TypeError,"update expected at most 1 argument, got %d",np-1);
    if(np==2) mp_dict_update_from(D(SELF),argv[1]);
    if(kw) for(int64_t i=0;i<kw->len;i++) mp_dict_set(D(SELF),kw->items[i],argv[np+i]);
    return v_none();
}
static Value m_dcopy(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("copy",argc,0,0); Value d=mp_dict(); int64_t pos=0; Value k, v; while(mp_dict_next(D(SELF),&pos,&k,&v)) mp_dict_set(AS_DICT(d),k,v); return d; }
static Value m_dclear(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("clear",argc,0,0); DictObj *d=D(SELF); free(d->ent); free(d->idx); d->ent=NULL; d->idx=NULL; d->nent=d->cap=d->used=d->isize=0; return v_none(); }
static Value m_fromkeys(int argc, Value *argv, TupleObj *kw){ (void)kw;
    int base= IS_TYPE(argv[0]) ? 1 : 0;
    Value d=mp_dict(), it=mp_iter(argv[base]), k, v= argc>base+1 ? argv[base+1] : v_none();
    while(mp_next(it,&k)) mp_dict_set(AS_DICT(d),k,v);
    return d; }

/* ---------------------------------------------------------------- set */
static SetObj *ST(Value v){ return (SetObj*)v.u.o; }
static Value as_set(Value v){ if(v.k==V_OBJ && v.u.o->type->layout==LY_SET) return v; Value s=mp_set(T_set); Value it=mp_iter(v), x; while(mp_next(it,&x)) mp_set_add(ST(s),x); return s; }
static Value m_add(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("set.add",argc,1,1); mp_set_add(ST(SELF),argv[1]); return v_none(); }
static Value m_discard(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("discard",argc,1,1); mp_set_del(ST(SELF),argv[1]); return v_none(); }
static Value m_sremove(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("remove",argc,1,1); if(!mp_set_del(ST(SELF),argv[1])) mp_raise(mp_exc_args(E_KeyError,mp_tuple(1,&argv[1]))); return v_none(); }
static Value m_spop(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("pop",argc,0,0); return mp_set_pop(ST(SELF)); }

static Value m_sclear(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("clear",argc,0,0); mp_set_clear(ST(SELF)); return v_none(); }
static Value m_scopy(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("copy",argc,0,0); return mp_set_copy(ST(SELF),TYPE(SELF)); }

static int is_aset(Value v){ return v.k==V_OBJ && v.u.o->type->layout==LY_SET; }
/* CPython's set_intersection: a set as the binary operator, anything else walked */
static Value set_inter(Value s, Value o){
    if(is_aset(o)) return mp_binop(OP_BitAnd,s,o);
    Value r=mp_set(TYPE(s)), it=mp_iter(o), x;
    while(mp_next(it,&x)){ uint64_t h=mp_hash(x); if(mp_set_has_hashed(ST(s),x,h)) mp_set_add_hashed(ST(r),x,h); }
    return r;
}
static Value m_union(int argc, Value *argv, TupleObj *kw){ (void)kw;
    Value r=mp_set_copy(ST(SELF),TYPE(SELF));
    for(int i=1;i<argc;i++) mp_set_update(ST(r),argv[i]);
    return r; }
static Value m_intersection(int argc, Value *argv, TupleObj *kw){ (void)kw;
    if(argc==1) return mp_set_copy(ST(SELF),TYPE(SELF));
    Value r=SELF; for(int i=1;i<argc;i++) r=set_inter(r,argv[i]);
    return r; }
static Value m_difference(int argc, Value *argv, TupleObj *kw){ (void)kw;
    if(argc==1) return mp_set_copy(ST(SELF),TYPE(SELF));
    Value r;
    if(is_aset(argv[1])) r=mp_binop(OP_Sub,SELF,argv[1]);
    else { r=mp_set_copy(ST(SELF),TYPE(SELF)); mp_set_diff_update(ST(r),argv[1]); }
    for(int i=2;i<argc;i++) mp_set_diff_update(ST(r),argv[i]);
    return r; }
static Value m_symdiff(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("symmetric_difference",argc,1,1);
    Value r=mp_set(TYPE(SELF)); mp_set_update(ST(r),argv[1]); mp_set_xor_update(ST(r),SELF); return r; }
static Value m_supdate(int argc, Value *argv, TupleObj *kw){ (void)kw; for(int i=1;i<argc;i++) mp_set_update(ST(SELF),argv[i]); return v_none(); }
static Value m_iupdate(int argc, Value *argv, TupleObj *kw){ (void)kw;
    Value r=m_intersection(argc,argv,kw); SetObj *x=ST(SELF), *rs=ST(r);
    if(rs==x) return v_none();
    SEnt *t=x->table; int64_t m=x->mask, fl=x->fill, u=x->used;
    x->table=rs->table; x->mask=rs->mask; x->fill=rs->fill; x->used=rs->used; x->hashed=0;
    rs->table=t; rs->mask=m; rs->fill=fl; rs->used=u;
    return v_none(); }
static Value m_dupdate(int argc, Value *argv, TupleObj *kw){ (void)kw; for(int i=1;i<argc;i++) mp_set_diff_update(ST(SELF),argv[i]); return v_none(); }
static Value m_sdupdate(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("symmetric_difference_update",argc,1,1); mp_set_xor_update(ST(SELF),argv[1]); return v_none(); }
static Value m_issubset(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("issubset",argc,1,1); return mp_compare(OP_LtE,SELF,as_set(argv[1])); }
static Value m_issuperset(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("issuperset",argc,1,1); return mp_compare(OP_GtE,SELF,as_set(argv[1])); }
static Value m_isdisjoint(int argc, Value *argv, TupleObj *kw){ (void)kw; nargs("isdisjoint",argc,1,1); Value it=mp_iter(argv[1]), x; while(mp_next(it,&x)) if(mp_set_has(ST(SELF),x)) return v_bool(0); return v_bool(1); }
/* the methods of list, tuple, dict, set and frozenset (mp_methods_init) */
void mp_container_methods_init(void){
    Type *t=T_list;
    mp_type_add(t,"append",m_append); mp_type_add(t,"extend",m_extend); mp_type_add(t,"insert",m_insert); mp_type_add(t,"pop",m_lpop); mp_type_add(t,"remove",m_remove);
    mp_type_add(t,"index",m_lindex); mp_type_add(t,"count",m_lcount); mp_type_add(t,"reverse",m_reverse); mp_type_add(t,"copy",m_lcopy); mp_type_add(t,"clear",m_lclear);
    mp_type_add(t,"sort",m_sort);
    t=T_tuple; mp_type_add(t,"index",m_lindex); mp_type_add(t,"count",m_lcount);
    t=T_dict;
    mp_type_add(t,"keys",m_keys); mp_type_add(t,"values",m_values); mp_type_add(t,"items",m_items); mp_type_add(t,"get",m_get); mp_type_add(t,"pop",m_dpop);
    mp_type_add(t,"popitem",m_popitem); mp_type_add(t,"setdefault",m_setdefault); mp_type_add(t,"update",m_update); mp_type_add(t,"copy",m_dcopy); mp_type_add(t,"clear",m_dclear);
    { Value fk=mp_native("fromkeys",m_fromkeys); BoxObj *cm=(BoxObj*)mp_alloc(T_classmethod,sizeof(BoxObj)); cm->v=fk; mp_dict_set(t->dict,mp_intern("fromkeys"),v_obj(cm)); }
    t=T_set;
    mp_type_add(t,"add",m_add); mp_type_add(t,"discard",m_discard); mp_type_add(t,"remove",m_sremove); mp_type_add(t,"pop",m_spop); mp_type_add(t,"clear",m_sclear);
    mp_type_add(t,"update",m_supdate); mp_type_add(t,"intersection_update",m_iupdate); mp_type_add(t,"difference_update",m_dupdate); mp_type_add(t,"symmetric_difference_update",m_sdupdate);
    for(int k=0;k<2;k++){ t= k==0 ? T_set : T_frozenset;
        mp_type_add(t,"copy",m_scopy); mp_type_add(t,"union",m_union); mp_type_add(t,"intersection",m_intersection); mp_type_add(t,"difference",m_difference);
        mp_type_add(t,"symmetric_difference",m_symdiff); mp_type_add(t,"issubset",m_issubset); mp_type_add(t,"issuperset",m_issuperset); mp_type_add(t,"isdisjoint",m_isdisjoint); }
}

