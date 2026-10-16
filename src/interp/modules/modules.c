/* ========================= Interpreter, modules: native modules =========================
   Argument helpers of the native modules; the BaseExceptionGroup
   module, gc, and the table of modules written in C. */

#include "modules.h"

const char *mp_main_dir=".";

Value argv_list;

int mod_npos(int argc, TupleObj *kw){ return argc-(kw?(int)kw->len:0); }
Value mod_kwarg(int argc, Value *argv, TupleObj *kw, const char *name, Value dflt){
    if(!kw) return dflt;
    int np=mod_npos(argc,kw);
    for(int64_t i=0;i<kw->len;i++) if(!strcmp(mp_cstr(kw->items[i]),name)) return argv[np+i];
    return dflt;
}
void mod_no_kw(const char *fn, TupleObj *kw){ if(kw && kw->len) mp_raise_t(E_TypeError,"%s() takes no keyword arguments",fn); }
void mod_nargs(const char *fn, int n, int lo, int hi){
    if(n<lo || n>hi){ if(lo==hi) mp_raise_t(E_TypeError,"%s() takes exactly %d argument%s (%d given)",fn,lo,lo==1?"":"s",n);
        mp_raise_t(E_TypeError,"%s() takes from %d to %d arguments (%d given)",fn,lo,hi,n); }
}

/* BaseExceptionGroup's behavior: from the _excgroup module, the first time a group is made */
static Value excgroup_mod;
Value mp_excgroup(const char *fn){
    if(excgroup_mod.k!=V_OBJ){
        Value nm=mp_str("_excgroup"); excgroup_mod=mp_builtin_import(1,&nm,NULL); mp_gc_add_root(&excgroup_mod);
        DictObj *m=mdict(excgroup_mod), *d=E_BaseExceptionGroup->dict; Value v;
        E_BaseExceptionGroup->flags|=TF_DUNDERS; E_ExceptionGroup->flags|=TF_DUNDERS;      /* (__str__ written in Python) */
        mp_dict_get_s(m,"_new",&v); BoxObj *sm=(BoxObj*)mp_alloc(T_staticmethod,sizeof(BoxObj)); sm->v=v; mp_dict_set_s(d,"__new__",v_obj(sm));
        mp_dict_get_s(m,"_str",&v); mp_dict_set_s(d,"__str__",v);
        mp_dict_get_s(m,"_derive",&v); mp_dict_set_s(d,"derive",v);
        mp_dict_get_s(m,"_split",&v); mp_dict_set_s(d,"split",v);
        mp_dict_get_s(m,"_subgroup",&v); mp_dict_set_s(d,"subgroup",v);
        const char *props[2][2]={{"message","_message"},{"exceptions","_exceptions"}};
        for(int i=0;i<2;i++){ mp_dict_get_s(m,props[i][1],&v); PropertyObj *p=(PropertyObj*)mp_alloc(T_property,sizeof(PropertyObj)); p->get=v; p->set=p->del=p->doc=v_none(); mp_dict_set_s(d,props[i][0],v_obj(p)); }
    }
    Value f; if(!fn || !mp_dict_get_s(mdict(excgroup_mod),fn,&f)) return v_none();
    return f;
}
/* gc: the interpreter's collector (cycles; reference counting is not used) */
extern int mp_gc_enabled;

static Value gc_enable(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; mod_no_kw("enable",kw); mp_gc_enabled=1; return v_none(); }
static Value gc_disable(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; mod_no_kw("disable",kw); mp_gc_enabled=0; return v_none(); }
static Value gc_isenabled(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; mod_no_kw("isenabled",kw); return v_bool(mp_gc_enabled); }
static Value gc_collect(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)kw; mod_nargs("collect",argc,0,1);
    int64_t before=mp_gc_objects(); mp_gc_collect(); int64_t after=mp_gc_objects(); return v_int(before>after?before-after:0); }
static Value gc_get_count(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; Value t[3]={v_int(mp_gc_objects()%700),v_int(0),v_int(0)}; return mp_tuple(3,t); }
static Value gc_get_threshold(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; Value t[3]={v_int(2000),v_int(10),v_int(0)}; return mp_tuple(3,t); }
static Value gc_none(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_none(); }
static Value gc_zero(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_int(0); }
static Value gc_true(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_bool(1); }
static Value gc_false(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_bool(0); }
static Value gc_list(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return mp_list(0,NULL); }
static Value make_gc(void){
    Value m=register_module("gc");
    set_fn(m,"enable",gc_enable); set_fn(m,"disable",gc_disable); set_fn(m,"isenabled",gc_isenabled); set_fn(m,"collect",gc_collect);
    set_fn(m,"get_count",gc_get_count); set_fn(m,"get_threshold",gc_get_threshold); set_fn(m,"set_threshold",gc_none);
    set_fn(m,"get_debug",gc_zero); set_fn(m,"set_debug",gc_none); set_fn(m,"freeze",gc_none); set_fn(m,"unfreeze",gc_none);
    set_fn(m,"get_freeze_count",gc_zero); set_fn(m,"is_tracked",gc_true); set_fn(m,"is_finalized",gc_false);
    set_fn(m,"get_objects",gc_list); set_fn(m,"get_referrers",gc_list); set_fn(m,"get_referents",gc_list);
    mp_dict_set_s(mdict(m),"garbage",mp_list(0,NULL)); mp_dict_set_s(mdict(m),"callbacks",mp_list(0,NULL));
    static const char *const dn[]={"DEBUG_STATS","DEBUG_COLLECTABLE","DEBUG_UNCOLLECTABLE","DEBUG_SAVEALL","DEBUG_LEAK"}; static const int dv[]={1,2,4,32,38};
    for(int i=0;i<5;i++) mp_dict_set_s(mdict(m),dn[i],v_int(dv[i]));
    return m;
}
Value native_module(const char *name){
    if(!strcmp(name,"sys")) return make_sys();
    if(!strcmp(name,"math")) return make_math();
    if(!strcmp(name,"_ppy_json")) return make_json();
    if(!strcmp(name,"thread")) return make_thread();
    if(!strcmp(name,"_thread")) return make__thread();
    if(!strcmp(name,"gc")) return make_gc();
    if(!strcmp(name,"_ctypes")) return make_ctypes_native();
    if(!strcmp(name,"builtins")){ Value m=register_module("builtins"); ((ModuleObj*)m.u.o)->dict=mp_builtins; return m; }
    return v_undef();
}
void mp_modules_init(void){
    native_module("builtins");
}
