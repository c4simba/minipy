/* ========================= AOT driver: minipy --compile --check =========================
   The modules named on the command line (and what they import) checked for
   compiled programs - what `minipy check` runs for the packages of a project
   (src/stdlib/_mpy_pkg). Unlike a compilation it goes on after an error: the
   function, class or module with it is reported and left out, the rest is
   checked (aot_check_mode). A function nothing calls whose parameters have no
   types cannot be checked: it is reported as unclear.

   stdout, a line per fact (tab-separated):
       module  <name> <path>          every module of the program
       imports <name> <a,b,...>       the modules its import statements name
       failed  <name>                 left out: its source, imports or declarations have errors
   stderr: the diagnostics, "path:line: error: ..." and "path:line: unclear: ...".
   Exit status: 0 without errors, 1 with. */

#include "driver.h"
#include "model.h"

static void put_import(SBuf *b, int *n, const char *name){
    sbf(b,"%s%s",*n?",":"",name); (*n)++;
}
/* the modules the statements import (nested ones too) */
static void imports_in(Units *us, AotUnit *u, Stmt **body, int count, SBuf *b, int *n){
    for(int i=0;i<count;i++){ Stmt *s=body[i];
        if(s->kind==STMT_IMPORT) put_import(b,n,s->name2?s->name2:s->name);
        else if(s->kind==STMT_FROM_IMPORT){
            char *mod=aot_from_import_module(u,s);
            if(!mod) continue;
            put_import(b,n,mod);
            for(int k=0;k<s->nnames;k++){                      /* from pkg import sub: the submodule */
                char sub[512]; snprintf(sub,sizeof sub,"%s.%s",mod,s->names[k]);
                if(find_unit(us,sub)) put_import(b,n,sub); }
            free(mod);
        }
        imports_in(us,u,s->body,s->body_count,b,n);
        imports_in(us,u,s->orelse,s->orelse_count,b,n);
    }
}

int aot_check_modules(char **modules, int nmodules, AotTarget target){
    aot_check_mode=1;
    SBuf src={0};
    for(int i=0;i<nmodules;i++) sbf(&src,"import %s\n",modules[i]);
    if(!src.s) sbf(&src,"\n");
    Units us; memset(&us,0,sizeof us);
    if(load_program(&us,"<check>",src.s)) return 1;
    for(int i=1;i<us.n;i++){ AotUnit *u=us.v[i];
        printf("module\t%s\t%s\n",u->name,u->path?u->path:"-");
        if(u->failed){ printf("failed\t%s\n",u->name); continue; }
        if(!u->ast) continue;
        SBuf b={0}; int n=0;
        imports_in(&us,u,u->ast->body,u->ast->body_count,&b,&n);
        printf("imports\t%s\t%s\n",u->name,b.s?b.s:"");
        free(b.s);
    }
    fflush(stdout);
    AProg *p=aot_check(us.v,us.n,target);
    if(!p){ if(!aot_check_errors) aot_check_errors=1; }
    else for(int i=1;i<p->nmods;i++) if(p->mods[i]->check_failed) printf("failed\t%s\n",p->mods[i]->name);
    fflush(stdout);
    return aot_check_errors ? 1 : 0;
}
