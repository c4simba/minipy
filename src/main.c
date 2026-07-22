/* ========================= Entry point ========================= */

#include "platform/platform.h"
#include "vm.h"
#include "compiler.h"
#include "frontparser.h"
#include "bytecode.h"
#include "containers.h"
#include "fs.h"
#include "gc.h"

int main(int argc,char **argv){
    const char *script_path = NULL;
    gc_set_stack_base(&argc);
    mpy_platform_init();
    const char *program = (argc>0 && argv && argv[0]) ? argv[0] : "minipy";

    if(argc<2){
        script_path = mpy_platform_default_script();
        if(!script_path){
            fprintf(stderr,"usage: %s [--dump-ast|--dump-symbols|--dump-bytecode|--fs-info] file.mpy\n",program);
            return 2;
        }
    }
    if(!script_path && strcmp(argv[1],"--fs-info")==0){
        printf("%s\n",mpy_fs_backend_name());
        return 0;
    }
    if(!script_path && (strcmp(argv[1],"--dump-ast")==0 || strcmp(argv[1],"--dump-symbols")==0 || strcmp(argv[1],"--dump-bytecode")==0)){
        if(argc<3){ fprintf(stderr,"usage: %s %s file.mpy\n",program,argv[1]); return 2; }
        char *src=mpy_fs_read_file(argv[2]);
        if(strcmp(argv[1],"--dump-ast")==0) dump_ast_for_source(src);
        else if(strcmp(argv[1],"--dump-symbols")==0) dump_symbols_for_source(src);
        else {
            char *dir=mpy_fs_dirname(argv[2]); Dict *globals=dict_new(); Function *mainfn=compile_source(src,"__main__",dir,globals); dump_function_bytecode(mainfn); free(dir);
        }
        free(src);
        return 0;
    }
    if(!script_path) script_path = argv[1];

    mpy_repr_hook = mpy_instance_repr;
    mpy_platform_banner(script_path);
    memset(&vm,0,sizeof(vm));
    if(setjmp(vm.panic)){ print_traceback(is_obj(vm.pending_exception,O_EXCEPTION)?vm.pending_exception:exceptionv("RuntimeError",vm.error_msg?vm.error_msg:"error",nonev())); return 1; }
    vm.builtins=dict_new(); vm.modules=dict_new();
    dict_set(vm.builtins,"len",nativev(&N_LEN)); dict_set(vm.builtins,"range",nativev(&N_RANGE)); dict_set(vm.builtins,"next",nativev(&N_NEXT)); dict_set(vm.builtins,"iter",nativev(&N_ITER)); dict_set(vm.builtins,"input",nativev(&N_INPUT));
    dict_set(vm.builtins,"str",nativev(&N_STR)); dict_set(vm.builtins,"repr",nativev(&N_REPR)); dict_set(vm.builtins,"int",nativev(&N_INT)); dict_set(vm.builtins,"float",nativev(&N_FLOAT)); dict_set(vm.builtins,"bool",nativev(&N_BOOL));
    dict_set(vm.builtins,"list",nativev(&N_LIST)); dict_set(vm.builtins,"tuple",nativev(&N_TUPLE)); dict_set(vm.builtins,"set",nativev(&N_SET)); dict_set(vm.builtins,"dict",nativev(&N_DICT));
    dict_set(vm.builtins,"abs",nativev(&N_ABS)); dict_set(vm.builtins,"min",nativev(&N_MIN)); dict_set(vm.builtins,"max",nativev(&N_MAX)); dict_set(vm.builtins,"sum",nativev(&N_SUM)); dict_set(vm.builtins,"sorted",nativev(&N_SORTED)); dict_set(vm.builtins,"reversed",nativev(&N_REVERSED));
    dict_set(vm.builtins,"enumerate",nativev(&N_ENUMERATE)); dict_set(vm.builtins,"zip",nativev(&N_ZIP)); dict_set(vm.builtins,"map",nativev(&N_MAP)); dict_set(vm.builtins,"filter",nativev(&N_FILTER));
    dict_set(vm.builtins,"type",nativev(&N_TYPE)); dict_set(vm.builtins,"isinstance",nativev(&N_ISINSTANCE)); dict_set(vm.builtins,"ord",nativev(&N_ORD)); dict_set(vm.builtins,"chr",nativev(&N_CHR)); dict_set(vm.builtins,"round",nativev(&N_ROUND)); dict_set(vm.builtins,"any",nativev(&N_ANY)); dict_set(vm.builtins,"all",nativev(&N_ALL));
    dict_set(vm.builtins,"super",nativev(&N_SUPER)); dict_set(vm.builtins,"staticmethod",nativev(&N_STATICMETHOD)); dict_set(vm.builtins,"classmethod",nativev(&N_CLASSMETHOD)); dict_set(vm.builtins,"property",nativev(&N_PROPERTY));
    {
        const char *excs[]={"BaseException","Exception","RuntimeError","StopIteration","ValueError","TypeError","KeyError","IndexError","ZeroDivisionError","NameError","AttributeError","AssertionError","ImportError","ModuleNotFoundError",NULL};
        for(int i=0;excs[i];i++){ Obj *ec=new_obj(O_CLASS); ec->as.klass.name=xstrdup2(excs[i]); ec->as.klass.methods=dict_new(); dict_set(vm.builtins,excs[i],objv(ec)); }
    }
    /* Built-in `sys` module: the raw syscall gateway + platform tag. Preloaded
       into the module cache so `import sys` resolves without touching the FS. */
    {
        Dict *sysd=dict_new();
        dict_set(sysd,"__name__",stringv("sys"));
        dict_set(sysd,"syscall",nativev(&N_SYSCALL));
        dict_set(sysd,"platform",stringv(mpy_platform_has_syscall()?"kolibrios":"host"));
        dict_set(sysd,"buffer",nativev(&N_BUFFER));       /* raw struct build/parse */
        dict_set(sysd,"poke",nativev(&N_POKE));
        dict_set(sysd,"peek",nativev(&N_PEEK));
        dict_set(sysd,"poke_str",nativev(&N_POKE_STR));
        dict_set(sysd,"peek_str",nativev(&N_PEEK_STR));
        dict_set(sysd,"addr",nativev(&N_ADDR));
        dict_set(vm.modules,"sys",objv(new_module("sys",sysd)));
    }
    char *src=mpy_fs_read_file(script_path);
    if(!src) return 1;
    char *dir=mpy_fs_dirname(script_path);
    Dict *globals=dict_new();
    Function *mainfn=compile_source(src,"__main__",dir,globals);
    run_function(mainfn,0,NULL);
    free(src); free(dir);
    mpy_platform_shutdown();
    return 0;
}
