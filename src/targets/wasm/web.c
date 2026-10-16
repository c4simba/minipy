/* ========================= plainpy in a browser: the interpreter as WebAssembly =========================
   make wasm builds the interpreter with Emscripten (build/wasm: plainpy.js,
   plainpy.wasm, plainpy.data - the standard library, plainpy.lib, and lib/).
   The page (site/try.js) starts it once and gives it what the user types,
   piece by piece, as a REPL does:

     ppy_web_start()     the interpreter set up, an empty __main__
     ppy_web_run(src)    src run in __main__'s namespace: 0 done, 1 an
                         exception (its traceback printed), 2 not complete
                         yet (a block or a triple-quoted string goes on:
                         the page asks for more lines)

   An expression's value is printed (its repr, unless None) and kept in _,
   as CPython's REPL does. Everything else is the interpreter itself, the
   same as on Linux: the POSIX backend (targets/posix) on Emscripten's C
   library, files in its memory file system. No threads (no SharedArrayBuffer
   on a page that cannot set its own headers): threading's start() raises.

   The collector scans the C stack for the objects C code holds; in
   WebAssembly locals live outside memory, where it cannot see them, so here
   it collects only between two pieces (mp_gc_safe_point), when no C code
   holds one: a piece's garbage stays until it ends. (binaryen's
   --spill-pointers would make every point safe, but its bigger frames let
   the browser's stack hold five times fewer Python calls.) */

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include "targets/platform.h"
#include "interp/interp.h"
#include "fs.h"

static Thread web_thread;
static Value web_main;                                 /* the __main__ module (in sys.modules: the collector sees it) */
static int web_started;

static Value builtin(const char *name){
    Value f; if(!mp_dict_get(mp_builtins,mp_str(name),&f)) f=v_none();
    return f;
}

EMSCRIPTEN_KEEPALIVE int ppy_web_start(void){
    if(web_started) return 0;
    volatile int anchor;
    setenv("PLAINPYLIB","/plainpy/plainpy.lib",0);       /* (plainpy.data puts them there) */
    setenv("PLAINPYPATH","/plainpy/lib",0);
    ppy_platform_init();
    ppy_lock_init(&mp_gil); ppy_lock_acquire(&mp_gil);
    mp_ts=&web_thread; mp_thread_add(&web_thread);
    web_thread.cstack_base=(void*)&anchor;
    ppy_fs_set_program("/plainpy/plainpy");
    mp_init();
    static char name[]="<stdin>", *argv[]={name,NULL};
    mp_set_argv(1,argv);
    mp_main_dir="/";
    web_main=mp_new_module("__main__");
    mp_dict_set_s(mp_modules,"__main__",web_main);
    web_started=1;
    return 0;
}

/* a SyntaxError that only says the input is not finished yet */
static int incomplete(Value e){
    if(!mp_isinstance(e,E_SyntaxError)) return 0;
    Value msg=mp_getattr_s(e,"msg");
    const char *m=mp_cstr(mp_tostr(msg));
    return !strncmp(m,"expected an indented block",26) || !strncmp(m,"unterminated triple-quoted",26);
}

static __attribute__((noinline)) int web_run(const char *src){
    DictObj *g=((ModuleObj*)web_main.u.o)->dict;
    Value text=mp_str(src);
    Value fn_compile=builtin("compile");
    int rc=0;
    /* an expression: its value printed, as the REPL does */
    { Catch c; Value code=v_none();
      if(!CATCH_BEGIN(c)){ Value a[3]={text,mp_str("<stdin>"),mp_str("eval")}; code=mp_call(fn_compile,3,a,NULL); CATCH_END(c); }
      else { (void)mp_catch_exc(&c); code=v_none(); }
      if(!IS_NONE(code)){
          Catch r;
          if(!CATCH_BEGIN(r)){
              Value a[2]={code,v_obj((Obj*)g)};
              Value v=mp_call(builtin("eval"),2,a,NULL);
              if(!IS_NONE(v)){
                  mp_dict_set_s(mp_builtins,"_",v);
                  Value s=mp_repr(v);
                  mp_write_out(mp_cstr(s),AS_STR(s)->len); mp_write_out("\n",1);
              }
              CATCH_END(r);
          } else {
              Value e=mp_catch_exc(&r);
              if(mp_isinstance(e,E_SystemExit)){ Value s=mp_str("(sys.exit(): the page keeps the session going)\n"); mp_write_out(mp_cstr(s),AS_STR(s)->len); }
              else { mp_print_exception(e); rc=1; }
          }
          mp_flush_stdout();
          return rc;
      } }
    /* statements */
    Catch c;
    if(!CATCH_BEGIN(c)){
        Value a[3]={text,mp_str("<stdin>"),mp_str("exec")};
        Value code=mp_call(fn_compile,3,a,NULL);
        Value b[2]={code,v_obj((Obj*)g)};
        mp_call(builtin("exec"),2,b,NULL);
        CATCH_END(c);
    } else {
        Value e=mp_catch_exc(&c);
        if(incomplete(e)) rc=2;
        else if(mp_isinstance(e,E_SystemExit)){ Value s=mp_str("(sys.exit(): the page keeps the session going)\n"); mp_write_out(mp_cstr(s),AS_STR(s)->len); }
        else { mp_print_exception(e); rc=1; }
    }
    mp_flush_stdout();
    return rc;
}

EMSCRIPTEN_KEEPALIVE int ppy_web_run(const char *src){
    if(!web_started) ppy_web_start();
    volatile int anchor;
    web_thread.cstack_base=(void*)&anchor;
    mp_gc_safe_point();                                 /* (between two pieces: no C code holds an object) */
    return web_run(src);
}
