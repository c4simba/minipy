/* ========================= Interpreter, modules: _ctypes =========================
   C functions of shared libraries (dlopen / dlsym, cdecl calls). */

#include "modules.h"

#if !defined(PPY_KOLIBRI)
static int ct_errno;
static Value ct_dlopen(int argc, Value *argv, TupleObj *kw){
    (void)kw;
    const char *name= argc>0 && IS_STR(argv[0]) ? mp_cstr(argv[0]) : NULL;
    void *h=dlopen(name,RTLD_NOW|RTLD_GLOBAL);
#if defined(__APPLE__)
    if(!h && name && (!strncmp(name,"libc.so",7) || !strncmp(name,"libm.so",7))) h=dlopen("libSystem.B.dylib",RTLD_NOW|RTLD_GLOBAL);
#endif
    if(!h){ const char *e=dlerror(); mp_raise_t(E_OSError,"%s",e?e:"cannot load the library"); }
    return v_int((int64_t)(intptr_t)h);
}
static Value ct_dlsym(int argc, Value *argv, TupleObj *kw){
    (void)kw; mod_nargs("dlsym",argc,2,2);
    void *p=dlsym((void*)(intptr_t)mp_index(argv[0],"handle"),mp_cstr(argv[1]));
    if(!p) mp_raise_t(E_AttributeError,"function '%s' not found",mp_cstr(argv[1]));
    return v_int((int64_t)(intptr_t)p);
}
typedef intptr_t (*cfn_i)(intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t,intptr_t);
typedef intptr_t (*cfn_v1)(intptr_t,...); typedef intptr_t (*cfn_v2)(intptr_t,intptr_t,...);
typedef intptr_t (*cfn_v3)(intptr_t,intptr_t,intptr_t,...); typedef intptr_t (*cfn_v4)(intptr_t,intptr_t,intptr_t,intptr_t,...);
#define CT_NOINLINE __attribute__((noinline))
static CT_NOINLINE intptr_t ct_call_n(void *f, intptr_t *a){ return ((cfn_i)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v1(void *f, intptr_t *a){ return ((cfn_v1)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v2(void *f, intptr_t *a){ return ((cfn_v2)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v3(void *f, intptr_t *a){ return ((cfn_v3)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
static CT_NOINLINE intptr_t ct_call_v4(void *f, intptr_t *a){ return ((cfn_v4)f)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]); }
typedef double (*cfn_d0)(void); typedef double (*cfn_d1)(double); typedef double (*cfn_d2)(double,double); typedef double (*cfn_d3)(double,double,double);
static Value ct_call(int argc, Value *argv, TupleObj *kw){
    (void)kw;
    if(argc<3 || !IS_STR(argv[1])) mp_raise_t(E_TypeError,"call(address, restype, fixed, *args)");
    void *fp=(void*)(intptr_t)mp_index(argv[0],"address"); const char *rk=mp_cstr(argv[1]);
    int fixed=(int)mp_index(argv[2],"fixed");
    int n=argc-3, floats=0; Value *a0=argv+3;
    for(int i=0;i<n;i++) if(a0[i].k==V_FLOAT) floats++;
    errno=0;
    if(!strcmp(rk,"f64")||!strcmp(rk,"f32")){
        if(floats!=n || n>3) mp_raise_t(E_TypeError,"the interpreter calls a C function returning a double with 0 to 3 double arguments");
        double d[3]={0,0,0}; for(int i=0;i<n;i++) d[i]=a0[i].u.f;
        mp_flush_stdout();
        double r= n==0?((cfn_d0)fp)():n==1?((cfn_d1)fp)(d[0]):n==2?((cfn_d2)fp)(d[0],d[1]):((cfn_d3)fp)(d[0],d[1],d[2]);
        ct_errno=errno; return v_float(r);
    }
    if(floats || n>8) mp_raise_t(E_TypeError,"the interpreter calls C functions with up to 8 integer, pointer or str arguments");
    intptr_t a[8]={0};
    for(int i=0;i<n;i++){ Value v=a0[i];
        if(IS_INTLIKE(v)) a[i]=(intptr_t)v.u.i;
        else if(IS_NONE(v)) a[i]=0;
        else if(IS_STR(v)) a[i]=(intptr_t)AS_STR(v)->s;
        else if(IS_BYTES(v)) a[i]=(intptr_t)AS_BYTES(v)->s;
        else if(IS(v,T_buffer)) a[i]=(intptr_t)((BufferObj*)v.u.o)->data;
        else if(IS_BA(v)) a[i]=(intptr_t)((ByteArrayObj*)v.u.o)->data;
        else mp_raise_t(E_TypeError,"a C function takes int, str, bytes, buffers or None");
    }
    if(fixed>=0 && fixed<n && (fixed<1 || fixed>4)) mp_raise_t(E_TypeError,"a variadic C function takes 1 to 4 fixed arguments in the interpreter");
    mp_flush_stdout();
    Thread *self=mp_ts; mp_gil_release();
    intptr_t r= fixed<0 || fixed>=n ? ct_call_n(fp,a) : fixed==1 ? ct_call_v1(fp,a) : fixed==2 ? ct_call_v2(fp,a) : fixed==3 ? ct_call_v3(fp,a) : ct_call_v4(fp,a);
    ct_errno=errno;
    mp_gil_acquire(self);
    if(!strcmp(rk,"void")) return v_none();
    if(!strcmp(rk,"i32")) return v_int((int32_t)r);
    if(!strcmp(rk,"u32")) return v_int((uint32_t)r);
    if(!strcmp(rk,"i16")) return v_int((int16_t)r);
    if(!strcmp(rk,"u16")) return v_int((uint16_t)r);
    if(!strcmp(rk,"i8")) return v_int((int8_t)r);
    if(!strcmp(rk,"u8")) return v_int((uint8_t)r);
    if(!strcmp(rk,"bool")) return v_bool((r&0xFF)!=0);
    if(!strcmp(rk,"str")) return r ? mp_str((const char*)r) : v_none();
    if(!strcmp(rk,"up")) return v_int((int64_t)(uintptr_t)r);
    return v_int((int64_t)r);
}
static Value ct_errno_fn(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_int(ct_errno); }
#else
static Value ct_unsupported(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; mp_raise_t(E_OSError,"ctypes is not available on this platform"); }
#define ct_dlopen ct_unsupported
#define ct_dlsym ct_unsupported
#define ct_call ct_unsupported
#define ct_errno_fn ct_unsupported
#endif
Value make_ctypes_native(void){
    Value m=register_module("_ctypes");
    set_fn(m,"dlopen",ct_dlopen); set_fn(m,"dlsym",ct_dlsym); set_fn(m,"call",ct_call); set_fn(m,"errno",ct_errno_fn);
    return m;
}

