/* ========================= Interpreter, modules: sys =========================
   sys: exit, exceptions, frames, argv, and plainpy's raw memory and
   system calls (sys.syscall, sys.buffer, peek / poke). */

#include "modules.h"

static Value sys_exit(int argc, Value *argv, TupleObj *kw){
    mod_no_kw("exit",kw); mod_nargs("exit",argc,0,1);
    mp_raise(mp_exc_args(E_SystemExit,argc?mp_tuple(1,argv):mp_tuple(0,NULL)));
}
static Value sys_exc_info(int argc, Value *argv, TupleObj *kw){
    (void)argc; (void)argv; (void)kw;
    Thread *ts=mp_ts;
    if(!ts->nhandled){ Value n[3]={v_none(),v_none(),v_none()}; return mp_tuple(3,n); }
    Value e=ts->handled[ts->nhandled-1]; ExcObj *x=AS_EXC(e);
    Value r[3]={v_obj(TYPE(e)),e,x->tb.k==V_UNDEF?v_none():x->tb}; return mp_tuple(3,r);
}
static Value sys_exception(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; Thread *ts=mp_ts; return ts->nhandled ? ts->handled[ts->nhandled-1] : v_none(); }
static Value sys_getrecursionlimit(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_int(1000); }
static Value sys_setrecursionlimit(int argc, Value *argv, TupleObj *kw){ (void)argc; (void)argv; (void)kw; return v_none(); }
static Value sys_intern(int argc, Value *argv, TupleObj *kw){ (void)kw; mod_nargs("intern",argc,1,1); return mp_intern(mp_cstr(argv[0])); }
/* sys.audit(event, *args): each hook sys.addaudithook() added is called with (event, args) */
static Value audit_hooks;
static Value sys_addaudithook(int argc, Value *argv, TupleObj *kw){ (void)kw; mod_nargs("addaudithook",argc,1,1);
    if(audit_hooks.k!=V_OBJ){ audit_hooks=mp_list(0,NULL); mp_gc_add_root(&audit_hooks); }
    mp_list_append(audit_hooks,argv[0]); return v_none(); }
static Value sys_audit(int argc, Value *argv, TupleObj *kw){ (void)kw;
    if(argc<1) mp_raise_t(E_TypeError,"audit() missing 1 required positional argument: 'event'");
    if(!IS_STR(argv[0])) mp_raise_t(E_TypeError,"expected str for argument 'event', not %s",mp_type_name(argv[0]));
    if(audit_hooks.k!=V_OBJ || !AS_LIST(audit_hooks)->len) return v_none();
    Value a[2]={argv[0],mp_tuple(argc-1,argv+1)};
    ListObj *h=AS_LIST(audit_hooks); for(int64_t i=0;i<h->len;i++) mp_call(h->items[i],2,a,NULL);
    return v_none(); }
/* sys._getframemodulename(depth=0): the __name__ of the module of the function depth calls up */
static Value sys_getframemodulename(int argc, Value *argv, TupleObj *kw){
    int64_t depth=0;
    for(int i=0;i<argc;i++) depth=mp_index(argv[i],"depth");
    (void)kw;
    Frame *f=mp_ts?mp_ts->frame:NULL;
    for(int64_t i=0;i<depth && f;i++) f=f->back;
    Value n;
    if(f && f->globals && mp_dict_get_s(f->globals,"__name__",&n)) return n;
    return v_none();
}
/* syscalls and raw memory: the platform's gateway. KolibriOS: int 0x40. Elsewhere
   the i386 Linux system calls (int 0x80) a compiled program for Linux makes, done
   with the host's calls as the macos target's programs do them: the same numbers,
   flags and results (-errno) on every host. */
#if !defined(PPY_KOLIBRI)
#define LX_WORD intptr_t
#define LX_PTR(a) ((void*)(a))
#define LX_BLOCKING_BEGIN Thread *lx_self_=mp_ts; mp_gil_release()
#define LX_BLOCKING_END mp_gil_acquire(lx_self_)
#define LX_FLUSH() mp_flush_stdout()
#include "targets/posix/lx_emul.c"
#endif
static Value sys_syscall(int argc, Value *argv, TupleObj *kw){
    mod_no_kw("syscall",kw);
    if(argc<1||argc>7) mp_raise_t(E_TypeError,"syscall() takes 1 to 7 register arguments (eax..edi, ebp)");
    intptr_t in[7]={0};
    for(int i=0;i<argc;i++){
        Value a=argv[i];
        if(IS_INTLIKE(a)) in[i]=(intptr_t)a.u.i;
        else if(IS_STR(a)) in[i]=(intptr_t)AS_STR(a)->s;
        else if(IS_BYTES(a)) in[i]=(intptr_t)AS_BYTES(a)->s;
        else if(IS(a,T_buffer)) in[i]=(intptr_t)((BufferObj*)a.u.o)->data;
        else if(IS_BA(a)) in[i]=(intptr_t)((ByteArrayObj*)a.u.o)->data;
        else if(IS_NONE(a)) in[i]=0;
        else mp_raise_t(E_TypeError,"syscall() arguments must be int, str, buffer or None");
    }
    Value l=mp_list(6,NULL);
#if defined(PPY_KOLIBRI)
    uint32_t in32[7], out[6]={0};
    for(int i=0;i<7;i++) in32[i]=(uint32_t)in[i];
    ppy_platform_syscall(in32,out);
    for(int i=0;i<6;i++) AS_LIST(l)->items[i]=v_int((int64_t)(int32_t)out[i]);   /* the registers, as compiled code reads them */
#else
    AS_LIST(l)->items[0]=v_int(lx_syscall(in));
    for(int i=1;i<6;i++) AS_LIST(l)->items[i]=v_int((int64_t)(int32_t)in[i]);
#endif
    return l;
}
static BufferObj *buf_arg(Value v, const char *who){ if(!IS(v,T_buffer)) mp_raise_t(E_TypeError,"%s expects a buffer",who); return (BufferObj*)v.u.o; }
static Value sys_buffer(int argc, Value *argv, TupleObj *kw){
    mod_no_kw("buffer",kw); mod_nargs("buffer",argc,1,1);
    Value a=argv[0]; BufferObj *b=(BufferObj*)mp_alloc(T_buffer,sizeof(BufferObj));
    if(IS_INTLIKE(a)){ if(a.u.i<0||a.u.i>(1<<26)) mp_raise_t(E_ValueError,"buffer() size out of range"); b->len=a.u.i; b->data=(unsigned char*)xmalloc((size_t)b->len+1); memset(b->data,0,(size_t)b->len+1); return v_obj(b); }
    if(IS_STR(a)){ b->len=AS_STR(a)->len; b->data=(unsigned char*)xmalloc((size_t)b->len+1); memcpy(b->data,AS_STR(a)->s,(size_t)b->len+1); return v_obj(b); }
    if(IS_BYTES(a)){ b->len=AS_BYTES(a)->len; b->data=(unsigned char*)xmalloc((size_t)b->len+1); memcpy(b->data,AS_BYTES(a)->s,(size_t)b->len+1); return v_obj(b); }
    mp_raise_t(E_TypeError,"buffer() expects an int size or a str");
}
static int nbytes(Value v, const char *who){ int64_t n=mp_index(v,who); if(n<1||n>4) mp_raise_t(E_ValueError,"%s nbytes must be 1, 2, 3 or 4",who); return (int)n; }   /* (compiled code: a byte loop as well) */
static Value sys_poke(int argc, Value *argv, TupleObj *kw){ mod_no_kw("poke",kw); mod_nargs("poke",argc,4,4);
    BufferObj *b=buf_arg(argv[0],"poke()"); int64_t off=mp_index(argv[1],"poke"); uint32_t val=(uint32_t)mp_index(argv[2],"poke"); int nb=nbytes(argv[3],"poke()");
    if(off<0||off+nb>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    for(int i=0;i<nb;i++) b->data[off+i]=(unsigned char)((val>>(8*i))&0xFF);
    return v_none(); }
static Value sys_peek(int argc, Value *argv, TupleObj *kw){ mod_no_kw("peek",kw); mod_nargs("peek",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"peek()"); int64_t off=mp_index(argv[1],"peek"); int nb=nbytes(argv[2],"peek()");
    if(off<0||off+nb>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    uint32_t v=0; for(int i=0;i<nb;i++) v|=((uint32_t)b->data[off+i])<<(8*i);
    return v_int((int64_t)v); }
static Value sys_poke_str(int argc, Value *argv, TupleObj *kw){ mod_no_kw("poke_str",kw); mod_nargs("poke_str",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"poke_str()"); int64_t off=mp_index(argv[1],"poke_str");
    if(!IS_STR(argv[2])) mp_raise_t(E_TypeError,"poke_str() third argument must be a str");
    StrObj *s=AS_STR(argv[2]);
    if(off<0||off+s->len>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    memcpy(b->data+off,s->s,(size_t)s->len); return v_none(); }
static Value sys_peek_str(int argc, Value *argv, TupleObj *kw){ mod_no_kw("peek_str",kw); mod_nargs("peek_str",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"peek_str()"); int64_t off=mp_index(argv[1],"peek_str"), n=mp_index(argv[2],"peek_str");
    if(off<0||n<0||off+n>b->len) mp_raise_t(E_IndexError,"peek_str() range out of bounds");
    return mp_strn((const char*)b->data+off,n); }
static Value sys_peek_bytes(int argc, Value *argv, TupleObj *kw){ mod_no_kw("peek_bytes",kw); mod_nargs("peek_bytes",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"peek_bytes()"); int64_t off=mp_index(argv[1],"peek_bytes"), n=mp_index(argv[2],"peek_bytes");
    if(off<0||n<0||off+n>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    return mp_bytes((const char*)b->data+off,n); }
static Value sys_poke_bytes(int argc, Value *argv, TupleObj *kw){ mod_no_kw("poke_bytes",kw); mod_nargs("poke_bytes",argc,3,3);
    BufferObj *b=buf_arg(argv[0],"poke_bytes()"); int64_t off=mp_index(argv[1],"poke_bytes");
    if(!IS_BYTES(argv[2])) mp_raise_t(E_TypeError,"poke_bytes() third argument must be bytes");
    int64_t n=AS_BYTES(argv[2])->len;
    if(off<0||off+n>b->len) mp_raise_t(E_IndexError,"buffer offset out of range");
    memcpy(b->data+off,AS_BYTES(argv[2])->s,(size_t)n); return v_none(); }
#if !defined(PPY_KOLIBRI)
extern char **environ;
#endif
/* sys._rawargs(0): the arguments; (1): the environment as "NAME=value" strings (os.environ) */
static Value sys_rawargs(int argc, Value *argv, TupleObj *kw){ mod_no_kw("_rawargs",kw); mod_nargs("_rawargs",argc,1,1);
    Value l=mp_list(0,NULL);
    if(mp_index(argv[0],"_rawargs")==0){ if(argv_list.k==V_OBJ) for(int64_t i=0;i<AS_LIST(argv_list)->len;i++) mp_list_append(l,AS_LIST(argv_list)->items[i]); return l; }
#if !defined(PPY_KOLIBRI)
    for(char **e=environ;e && *e;e++) mp_list_append(l,mp_str(*e));
#endif
    return l; }
static Value sys_addr(int argc, Value *argv, TupleObj *kw){ mod_no_kw("addr",kw); mod_nargs("addr",argc,1,1); return v_int((int64_t)(uintptr_t)buf_arg(argv[0],"addr()")->data); }
static unsigned char *addr_of(Value v){ return (unsigned char*)(uintptr_t)(uint64_t)mp_index(v,"address"); }
static Value sys_peek_at(int argc, Value *argv, TupleObj *kw){ mod_no_kw("peek_at",kw); mod_nargs("peek_at",argc,2,2);
    unsigned char *p=addr_of(argv[0]); int nb=nbytes(argv[1],"peek_at()"); uint32_t v=0; for(int i=0;i<nb;i++) v|=((uint32_t)p[i])<<(8*i); return v_int((int64_t)v); }
static Value sys_poke_at(int argc, Value *argv, TupleObj *kw){ mod_no_kw("poke_at",kw); mod_nargs("poke_at",argc,3,3);
    unsigned char *p=addr_of(argv[0]); uint32_t val=(uint32_t)mp_index(argv[1],"poke_at"); int nb=nbytes(argv[2],"poke_at()");
    for(int i=0;i<nb;i++) p[i]=(unsigned char)((val>>(8*i))&0xFF); return v_none(); }
static Value sys_peek_str_at(int argc, Value *argv, TupleObj *kw){ mod_no_kw("peek_str_at",kw); mod_nargs("peek_str_at",argc,2,2);
    int64_t n=mp_index(argv[1],"peek_str_at"); if(n<0) mp_raise_t(E_ValueError,"peek_str_at() length must not be negative"); return mp_strn((const char*)addr_of(argv[0]),n); }
static Value sys_poke_str_at(int argc, Value *argv, TupleObj *kw){ mod_no_kw("poke_str_at",kw); mod_nargs("poke_str_at",argc,2,2);
    if(!IS_STR(argv[1])) mp_raise_t(E_TypeError,"poke_str_at() second argument must be a str");
    memcpy(addr_of(argv[0]),AS_STR(argv[1])->s,(size_t)AS_STR(argv[1])->len); return v_none(); }
static Value sys_cstr_at(int argc, Value *argv, TupleObj *kw){ mod_no_kw("cstr_at",kw); mod_nargs("cstr_at",argc,2,2);
    const char *p=(const char*)addr_of(argv[0]); int64_t max=mp_index(argv[1],"cstr_at"), n=0; while(n<max && p[n]) n++; return mp_strn(p,n); }
void mp_set_argv(int argc, char **argv){
    argv_list=mp_list(0,NULL); mp_gc_add_root(&argv_list);
    for(int i=0;i<argc;i++) mp_list_append(argv_list,mp_str(argv[i]));
}
static Value sys_format_exception(int argc, Value *argv, TupleObj *kw){ mod_no_kw("_format_exception",kw); mod_nargs("_format_exception",argc,1,1); return mp_format_exception(argv[0]); }
static Value sys_excepthook(int argc, Value *argv, TupleObj *kw){ mod_no_kw("excepthook",kw); mod_nargs("excepthook",argc,3,3); mp_print_exception(argv[1]); return v_none(); }
static Value sys_stack(int argc, Value *argv, TupleObj *kw){ (void)argv; mod_no_kw("_stack",kw); mod_nargs("_stack",argc,0,0); return mp_stack_list(); }
static Value sys_getline(int argc, Value *argv, TupleObj *kw){ mod_no_kw("_getline",kw); mod_nargs("_getline",argc,2,2);
    if(!IS_STR(argv[0])) return mp_str(""); return mp_source_getline(mp_cstr(argv[0]),(int)mp_index(argv[1],"_getline")); }
/* sys._getframe(depth): a snapshot of the running frames (f_code, f_lineno, f_back, f_globals, f_locals ...) */
static Type *T_frame;
static Value sys_getframe(int argc, Value *argv, TupleObj *kw){
    mod_no_kw("_getframe",kw); mod_nargs("_getframe",argc,0,1);
    int64_t depth= argc ? mp_index(argv[0],"_getframe") : 0;
    Frame *f=mp_ts->frame;
    for(int64_t i=0;i<depth && f;i++) f=f->back;
    if(!f || depth<0) mp_raise_t(E_ValueError,"call stack is not deep enough");
    if(!T_frame) T_frame=AS_TYPE(mp_new_type("frame",T_object,LY_INSTANCE,0));
    int n=0; for(Frame *g=f;g;g=g->back) n++;
    Frame **fs=(Frame**)xmalloc(sizeof(Frame*)*(size_t)n); n=0; for(Frame *g=f;g;g=g->back) fs[n++]=g;
    Value back=v_none();
    for(int i=n-1;i>=0;i--){ Frame *g=fs[i];
        InstObj *o=(InstObj*)mp_alloc(T_frame,sizeof(InstObj)); o->dict=AS_DICT(mp_dict()); Value fo=v_obj(o);
        int ip= g->ip>0 ? g->ip-1 : 0;
        int line= g->code->ncode ? g->code->lines[ip<g->code->ncode?ip:g->code->ncode-1] : g->code->firstline;
        mp_dict_set_s(o->dict,"f_code",v_obj(g->code)); mp_dict_set_s(o->dict,"f_lineno",v_int(line)); mp_dict_set_s(o->dict,"f_lasti",v_int(ip*2));
        mp_dict_set_s(o->dict,"f_globals",v_obj(g->globals)); mp_dict_set_s(o->dict,"f_builtins",v_obj(g->builtins?g->builtins:mp_builtins));
        mp_dict_set_s(o->dict,"f_locals",mp_frame_locals(g)); mp_dict_set_s(o->dict,"f_back",back); mp_dict_set_s(o->dict,"f_trace",v_none());
        mp_dict_set_s(o->dict,"f_generator",v_none());
        back=fo; }
    free(fs);
    return back;
}
static Value sys_suggestion(int argc, Value *argv, TupleObj *kw){ mod_no_kw("_suggestion",kw); mod_nargs("_suggestion",argc,2,2);
    if(!mp_isinstance(argv[0],E_BaseException)) return mp_str(""); return mp_exc_suggestion(argv[0],mp_truth(argv[1])); }
/* sys._builtin(f): f stands for a C function of CPython's (not bound as a method; its repr) */
static Value sys_builtin(int argc, Value *argv, TupleObj *kw){ mod_no_kw("_builtin",kw); mod_nargs("_builtin",argc,1,1);
    if(IS(argv[0],T_function)) ((FuncObj*)argv[0].u.o)->builtin=1;
    return argv[0]; }
static Value sys_frame_source(int argc, Value *argv, TupleObj *kw){ mod_no_kw("_frame_source",kw); mod_nargs("_frame_source",argc,4,4);
    if(!IS_STR(argv[0])) return mp_str(""); return mp_frame_source(mp_cstr(argv[0]),(int)mp_index(argv[1],"line"),argv[2],(int)mp_index(argv[3],"ip")); }
static Value sys_eval_annotation(int argc, Value *argv, TupleObj *kw){ mod_no_kw("_eval_annotation",kw); mod_nargs("_eval_annotation",argc,2,3);
    if(!IS_STR(argv[0]) || !IS_DICT(argv[1])) mp_raise_t(E_TypeError,"_eval_annotation(text, globals[, locals])");
    return mp_eval_annotation(mp_cstr(argv[0]),AS_DICT(argv[1]),argc>2&&IS_DICT(argv[2])?AS_DICT(argv[2]):NULL); }
static Value sys_getdefaultencoding(int argc, Value *argv, TupleObj *kw){ (void)argv; (void)argc; (void)kw; return mp_str("utf-8"); }
Value make_sys(void){
    Value m=register_module("sys");
#if defined(__APPLE__)
    const char *os="darwin";
#elif defined(__linux__)
    const char *os="linux";
#elif defined(_WIN32)
    const char *os="win32";
#else
    const char *os="host";
#endif
    mp_dict_set_s(mdict(m),"platform",mp_str(ppy_platform_has_syscall()?"kolibrios":os));
    mp_dict_set_s(mdict(m),"argv",argv_list.k==V_OBJ?argv_list:mp_list(0,NULL));
    mp_dict_set_s(mdict(m),"modules",v_obj(mp_modules));
    mp_dict_set_s(mdict(m),"path",mp_list(0,NULL));
    mp_dict_set_s(mdict(m),"warnoptions",mp_list(0,NULL));
    mp_dict_set_s(mdict(m),"maxsize",v_int(INT64_MAX));
    mp_dict_set_s(mdict(m),"byteorder",mp_str("little"));
    mp_dict_set_s(mdict(m),"_compiled",v_bool(0));          /* (compiled programs: True, folded by the compiler) */
    mp_dict_set_s(mdict(m),"version",mp_str("3.14.0 (plainpy)"));
    { Value vi[5]={v_int(3),v_int(14),v_int(0),mp_str("final"),v_int(0)}; mp_dict_set_s(mdict(m),"version_info",mp_tuple(5,vi)); }
    mp_dict_set_s(mdict(m),"stdout",mp_std_stream(0)); mp_dict_set_s(mdict(m),"stderr",mp_std_stream(1));
    mp_dict_set_s(mdict(m),"__stdout__",mp_std_stream(0)); mp_dict_set_s(mdict(m),"__stderr__",mp_std_stream(1));
    set_fn(m,"exit",sys_exit); set_fn(m,"exc_info",sys_exc_info); set_fn(m,"exception",sys_exception);
    set_fn(m,"getrecursionlimit",sys_getrecursionlimit); set_fn(m,"setrecursionlimit",sys_setrecursionlimit); set_fn(m,"intern",sys_intern); set_fn(m,"_getframemodulename",sys_getframemodulename);
    set_fn(m,"audit",sys_audit); set_fn(m,"addaudithook",sys_addaudithook);
    set_fn(m,"syscall",sys_syscall); set_fn(m,"buffer",sys_buffer); set_fn(m,"poke",sys_poke); set_fn(m,"peek",sys_peek);
    set_fn(m,"poke_str",sys_poke_str); set_fn(m,"peek_str",sys_peek_str); set_fn(m,"peek_bytes",sys_peek_bytes); set_fn(m,"poke_bytes",sys_poke_bytes); set_fn(m,"_rawargs",sys_rawargs); set_fn(m,"_sleep",ti_sleep); set_fn(m,"addr",sys_addr); set_fn(m,"peek_at",sys_peek_at);
    set_fn(m,"poke_at",sys_poke_at); set_fn(m,"peek_str_at",sys_peek_str_at); set_fn(m,"poke_str_at",sys_poke_str_at); set_fn(m,"cstr_at",sys_cstr_at);
    set_fn(m,"_format_exception",sys_format_exception); set_fn(m,"excepthook",sys_excepthook); set_fn(m,"_stack",sys_stack); set_fn(m,"_getline",sys_getline); set_fn(m,"_suggestion",sys_suggestion); set_fn(m,"_frame_source",sys_frame_source); set_fn(m,"_eval_annotation",sys_eval_annotation); set_fn(m,"_getframe",sys_getframe); set_fn(m,"_builtin",sys_builtin);
    mp_dict_set_s(mdict(m),"__excepthook__",mp_dict_get_s(mdict(m),"excepthook",NULL)?mp_native("excepthook",sys_excepthook):v_none());
    set_fn(m,"getdefaultencoding",sys_getdefaultencoding); set_fn(m,"getfilesystemencoding",sys_getdefaultencoding);
    mp_dict_set_s(mdict(m),"maxunicode",v_int(0x10FFFF)); mp_dict_set_s(mdict(m),"hexversion",v_int(0x030E00F0));
    mp_dict_set_s(mdict(m),"executable",mp_str(ppy_platform_exe_path()?ppy_platform_exe_path():"plainpy"));
    return m;
}

