/* ========================= The targets of plainpyc: the table, helpers =========================
   See targets.h. A new target: a folder with its target.c (an AotTargetDef
   and its build step), an entry in AotTarget (aot/aot.h) and in the table
   below, its files in AOT_SRC (Makefile). */

#include "targets/targets.h"
#include "targets/platform.h"
#include "pystdlib.h"
#include "util.h"
#include "fs.h"

static const AotTargetDef *const targets[]={&aot_target_linux,&aot_target_kolibri,&aot_target_macos,&aot_target_c,&aot_target_esp32,NULL};

const AotTargetDef *aot_target(AotTarget id){
    for(int i=0;targets[i];i++) if(targets[i]->id==id) return targets[i];
    return targets[0];
}

static int word_in(const char *list, const char *w){
    size_t n=strlen(w);
    for(const char *p=list;p && *p;){
        while(*p==' ') p++;
        const char *e=p; while(*e && *e!=' ') e++;
        if((size_t)(e-p)==n && !strncmp(p,w,n)) return 1;
        p=e;
    }
    return 0;
}
const AotTargetDef *aot_target_find(const char *name){
    for(int i=0;targets[i];i++) if(!strcmp(targets[i]->name,name) || word_in(targets[i]->aliases,name)) return targets[i];
    return NULL;
}

AotTarget aot_target_host(void){
#if defined(PPY_KOLIBRI)
    return AOT_TARGET_KOLIBRI;
#elif defined(__APPLE__)
    return AOT_TARGET_MACOS;
#else
    return AOT_TARGET_LINUX;
#endif
}

void aot_targets_help(FILE *f){
    for(int i=0;targets[i];i++){
        const AotTargetDef *t=targets[i];
        fprintf(f,"    %-9s %s%s\n",t->name,t->summary,t->id==aot_target_host()?" (default here)":"");
        if(t->aliases && *t->aliases) fprintf(f,"              (also: %s)\n",t->aliases);
    }
}

/* ---------------------------------------------------------------- helpers of build steps */
char *aot_concat(const char *a, const char *b){ size_t la=strlen(a), lb=strlen(b); char *r=(char*)xmalloc(la+lb+1); memcpy(r,a,la); memcpy(r+la,b,lb+1); return r; }

/* Quote one argument for the platform's command line. */
static char *quote_arg(const char *s){
#if defined(PPY_KOLIBRI)
    return xstrdup2(s);                                   /* KolibriOS passes the parameter string verbatim */
#else
    size_t n=strlen(s), k=2; for(size_t i=0;i<n;i++) k+= s[i]=='\'' ? 4 : 1;
    char *r=(char*)xmalloc(k+1), *o=r; *o++='\'';
    for(size_t i=0;i<n;i++){ if(s[i]=='\''){ memcpy(o,"'\\''",4); o+=4; } else *o++=s[i]; }
    *o++='\''; *o=0; return r;
#endif
}
char *aot_tool_args(const char *tmpl, const char *in, const char *out){
    char *dir=ppy_fs_dirname(in), *qi=quote_arg(in), *qo=quote_arg(out), *qd=quote_arg(dir);
    size_t cap=strlen(tmpl)+strlen(qi)+strlen(qo)+strlen(qd)*4+16; char *r=(char*)xmalloc(cap); size_t k=0;
    for(const char *p=tmpl;*p;){
        const char *rep=NULL; size_t skip=0;
        if(!strncmp(p,"{in}",4)){ rep=qi; skip=4; } else if(!strncmp(p,"{out}",5)){ rep=qo; skip=5; } else if(!strncmp(p,"{dir}",5)){ rep=qd; skip=5; }
        if(rep){ size_t l=strlen(rep); if(k+l+1>cap){ cap=(k+l+1)*2; r=(char*)xrealloc(r,cap); } memcpy(r+k,rep,l); k+=l; p+=skip; }
        else { if(k+2>cap){ cap*=2; r=(char*)xrealloc(r,cap); } r[k++]=*p++; }
    }
    r[k]=0; free(dir); free(qi); free(qo); free(qd);
    return r;
}
int aot_run_tool(const char *program, const char *args, const char *option, int verbose){
    if(verbose) printf("%s %s\n",program,args);
    int rc=ppy_platform_run(program,args);
    if(rc==127) fprintf(stderr,"plainpy: cannot run `%s`; is it installed? (choose another with %s)\n",program,option);
    else if(rc!=0) fprintf(stderr,"plainpy: `%s` failed (exit status %d)\n",program,rc);
    return rc;
}
int aot_write_file(const char *path, const char *data, size_t n, int verbose){
    char *err=NULL;
    if(ppy_fs_write_file(path,data,n,&err)){ fprintf(stderr,"plainpy: %s\n",err?err:"cannot write a file"); free(err); return 1; }
    if(verbose) printf("wrote %s\n",path);
    return 0;
}
const char *aot_lib_text(const char *name){
    const char *t=ppy_lib_text(name);
    if(!t){ fprintf(stderr,"plainpyc: plainpy.lib has no '%s' (a plainpy.lib of another build?)\n",name); exit(1); }
    return t;
}
