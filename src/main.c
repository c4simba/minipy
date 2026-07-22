/* ========================= Entry point ========================= */

#include "platform/platform.h"
#include "vm.h"
#include "compiler.h"
#include "frontparser.h"
#include "bytecode.h"
#include "containers.h"
#include "fs.h"

int main(int argc,char **argv){
    const char *script_path = NULL;
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

    mpy_platform_banner(script_path);
    memset(&vm,0,sizeof(vm));
    if(setjmp(vm.panic)){ print_traceback(is_obj(vm.pending_exception,O_EXCEPTION)?vm.pending_exception:exceptionv("RuntimeError",vm.error_msg?vm.error_msg:"error",nonev())); return 1; }
    vm.builtins=dict_new(); vm.modules=dict_new();
    dict_set(vm.builtins,"len",nativev(&N_LEN)); dict_set(vm.builtins,"range",nativev(&N_RANGE)); dict_set(vm.builtins,"next",nativev(&N_NEXT)); dict_set(vm.builtins,"iter",nativev(&N_ITER)); dict_set(vm.builtins,"input",nativev(&N_INPUT));
    Obj *be=new_obj(O_CLASS); be->as.klass.name=xstrdup2("BaseException"); be->as.klass.methods=dict_new();
    Obj *re=new_obj(O_CLASS); re->as.klass.name=xstrdup2("RuntimeError"); re->as.klass.methods=dict_new();
    Obj *se=new_obj(O_CLASS); se->as.klass.name=xstrdup2("StopIteration"); se->as.klass.methods=dict_new();
    dict_set(vm.builtins,"BaseException",objv(be)); dict_set(vm.builtins,"RuntimeError",objv(re)); dict_set(vm.builtins,"StopIteration",objv(se));
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
