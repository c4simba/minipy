/* ========================= Filesystem abstraction (platform-independent) ========================= */

#include "fs.h"

#ifndef PPY_FS_PATH_SEP
#define PPY_FS_PATH_SEP '/'
#endif

#ifndef PPY_FS_DEFAULT_IMPORT_DIR
#define PPY_FS_DEFAULT_IMPORT_DIR "."
#endif

static int ppy_fs_is_sep(char c){
    return c=='/' || c=='\\';
}

static int ppy_fs_path_is_absolute(const char *path){
    if(!path || !path[0]) return 0;
    if(ppy_fs_is_sep(path[0])) return 1;
    if(isalpha((unsigned char)path[0]) && path[1]==':') return 1;
    return 0;
}

static char *ppy_fs_normalize_path(const char *path){
    if(!path || !*path) return xstrdup2(".");
    size_t n=strlen(path);
    char *r=(char*)xmalloc(n+1);
    int out=0;
    int prev_sep=0;
    for(size_t i=0;i<n;i++){
        char c=path[i];
        if(ppy_fs_is_sep(c)) c=PPY_FS_PATH_SEP;
        if(c==PPY_FS_PATH_SEP){
            if(prev_sep && out>1) continue;
            prev_sep=1;
        } else {
            prev_sep=0;
        }
        r[out++]=c;
    }
    while(out>1 && r[out-1]==PPY_FS_PATH_SEP) out--;
    r[out]=0;
    return r;
}

char *ppy_fs_dirname(const char *path){
    char *norm=ppy_fs_normalize_path(path);
    const char *last=NULL;
    for(const char *p=norm; *p; p++) if(*p==PPY_FS_PATH_SEP) last=p;
    if(!last){ free(norm); return xstrdup2("."); }
    if(last==norm){
        char *root=xstrndup2(norm,1);
        free(norm);
        return root;
    }
    char *dir=xstrndup2(norm,(int)(last-norm));
    free(norm);
    return dir;
}

char *ppy_fs_join_path(const char *dir,const char *rel){
    if(!dir || !*dir || strcmp(dir,".")==0) return ppy_fs_normalize_path(rel);
    if(rel && ppy_fs_is_sep(rel[0])) return ppy_fs_normalize_path(rel);
    size_t n=strlen(dir)+1+strlen(rel)+1;
    char *tmp=(char*)xmalloc(n);
    snprintf(tmp,n,"%s%c%s",dir,PPY_FS_PATH_SEP,rel);
    char *r=ppy_fs_normalize_path(tmp);
    free(tmp);
    return r;
}

static char *ppy_fs_module_relpath(const char *module_name){
    size_t n=strlen(module_name);
    char *r=(char*)xmalloc(n+3+1);
    for(size_t i=0;i<n;i++){
        char c=module_name[i];
        r[i]=(c=='.')?PPY_FS_PATH_SEP:c;
    }
    memcpy(r+n,".py",4);
    return r;
}

char *ppy_fs_module_path(const char *importer_dir,const char *module_name){
    char *rel=ppy_fs_module_relpath(module_name);
    const char *base=(importer_dir && importer_dir[0]) ? importer_dir : ".";
    if(strcmp(base,".")==0 && strcmp(PPY_FS_DEFAULT_IMPORT_DIR,".")!=0){
        base=PPY_FS_DEFAULT_IMPORT_DIR;
    }
    char *r=ppy_fs_join_path(base,rel);
    free(rel);
    return r;
}

/* ---- the module search path ---- */
static char *program_dir;
static char **ppy_fs_search; static int ppy_fs_nsearch=-1;

void ppy_fs_set_program(const char *argv0){
    const char *exe=ppy_platform_exe_path();                 /* (the file itself, however it was started) */
    if(exe && *exe) program_dir=ppy_fs_dirname(exe);
    else if(argv0 && (strchr(argv0,'/') || strchr(argv0,'\\'))) program_dir=ppy_fs_dirname(argv0);
}
const char *ppy_fs_program_dir(void){ return program_dir; }
/* plainpy's data - plainpy.lib and the modules of lib/ - is next to the
   programs (a build tree, a folder unpacked), or, installed (make install),
   in <prefix>/lib/plainpy while the programs are in <prefix>/bin. */
const char *ppy_fs_data_dir(void){
    static char *data_dir; static int known;
    if(known) return data_dir;
    known=1; data_dir=program_dir;
    if(!program_dir) return NULL;
    char *here=ppy_fs_join_path(program_dir,"plainpy.lib");
    if(!ppy_fs_exists(here)){
        char *up=ppy_fs_join_path(program_dir,"../lib/plainpy"), *lib=ppy_fs_join_path(up,"plainpy.lib");
        if(ppy_fs_exists(lib)) data_dir=up; else free(up);
        free(lib);
    }
    free(here);
    return data_dir;
}
static void ppy_fs_add_search(const char *dir,int len){
    if(len<=0) return;
    ppy_fs_search=(char**)xrealloc(ppy_fs_search,sizeof(char*)*(size_t)(ppy_fs_nsearch+1));
    ppy_fs_search[ppy_fs_nsearch++]=xstrndup2(dir,len);
}
static char *ppy_fs_site;               /* <project>/.plainpy/site (see ppy_fs_use_project) */
static void ppy_fs_init_search(void){
    if(ppy_fs_nsearch>=0) return;
    ppy_fs_nsearch=0;
    if(ppy_fs_site) ppy_fs_add_search(ppy_fs_site,(int)strlen(ppy_fs_site));
    const char *env=getenv("PLAINPYPATH");
    for(const char *p=env; p && *p; ){
        const char *e=p; while(*e && *e!=':' && *e!=';') e++;
        if(e-p==1 && isalpha((unsigned char)*p) && *e==':' && (e[1]=='\\' || e[1]=='/')){ e++; while(*e && *e!=':' && *e!=';') e++; }   /* C:\dir */
        ppy_fs_add_search(p,(int)(e-p));
        p=*e?e+1:e;
    }
    const char *data=ppy_fs_data_dir();
    if(data){ char *lib=ppy_fs_join_path(data,"lib"); ppy_fs_add_search(lib,(int)strlen(lib)); free(lib); }
}
static char *ppy_fs_try_module(const char *dir,const char *module_name){
    static const char *forms[]={"%s.py","%s/__init__.py",NULL};
    char *rel=ppy_fs_module_relpath(module_name);
    rel[strlen(rel)-3]=0;                                   /* without ".py" */
    for(int i=0;forms[i];i++){
        size_t n=strlen(rel)+16; char *name=(char*)xmalloc(n); snprintf(name,n,forms[i],rel);
        char *path=ppy_fs_join_path(dir,name);
        free(name);
        if(ppy_fs_exists(path)){ free(rel); return path; }
        free(path);
    }
    free(rel);
    return NULL;
}
char *ppy_fs_find_module(const char *importer_dir,const char *module_name){
    const char *base=(importer_dir && importer_dir[0]) ? importer_dir : ".";
    if(strcmp(base,".")==0 && strcmp(PPY_FS_DEFAULT_IMPORT_DIR,".")!=0) base=PPY_FS_DEFAULT_IMPORT_DIR;
    char *found=ppy_fs_try_module(base,module_name);
    ppy_fs_init_search();
    for(int i=0;!found && i<ppy_fs_nsearch;i++) found=ppy_fs_try_module(ppy_fs_search[i],module_name);
    return found;
}

/* ---- a project's packages ----
   `plainpy sync` keeps the packages of a project in <project>/.plainpy/site
   (links into ~/.plainpy/store, or copies) and what `plainpy check` found in
   <project>/.plainpy/index, a line per module: "module package version status",
   status ok, fail or unchecked. Programs of the project import them; compiled
   ones only the modules that passed. */
typedef struct { char *mod, *dist, *ver, *status; } PpyPkgModule;
static char *ppy_fs_proj;               /* <project>/.plainpy */
static PpyPkgModule *ppy_fs_pkgs; static int ppy_fs_npkgs;

static char *ppy_fs_next_word(char **p){
    while(**p==' '||**p=='\t') (*p)++;
    char *w=*p;
    while(**p && **p!=' ' && **p!='\t' && **p!='\n' && **p!='\r') (*p)++;
    if(*p==w) return NULL;
    char *r=xstrndup2(w,(int)(*p-w));
    return r;
}
static void ppy_fs_load_index(const char *path){
    char *err=NULL, *text=ppy_fs_try_read_file(path,&err);
    free(err);
    if(!text) return;
    for(char *line=text; *line; ){
        char *eol=strchr(line,'\n'); if(!eol) eol=line+strlen(line);
        char *p=line;
        if(*p!='#'){
            PpyPkgModule m; m.mod=ppy_fs_next_word(&p); m.dist=ppy_fs_next_word(&p); m.ver=ppy_fs_next_word(&p); m.status=ppy_fs_next_word(&p);
            if(m.mod && m.dist && m.ver && m.status){
                ppy_fs_pkgs=(PpyPkgModule*)xrealloc(ppy_fs_pkgs,sizeof(PpyPkgModule)*(size_t)(ppy_fs_npkgs+1));
                ppy_fs_pkgs[ppy_fs_npkgs++]=m;
            } else { free(m.mod); free(m.dist); free(m.ver); free(m.status); }
        }
        line=*eol?eol+1:eol;
    }
    free(text);
}
void ppy_fs_use_project(const char *script_path){
    if(ppy_fs_proj || !script_path) return;
    char *dir=ppy_fs_dirname(script_path);
    for(int depth=0;depth<24 && dir;depth++){
        char *index=ppy_fs_join_path(dir,".plainpy/index");
        int found=ppy_fs_exists(index);
        free(index);
        if(found){
            ppy_fs_proj=ppy_fs_join_path(dir,".plainpy");
            ppy_fs_site=ppy_fs_join_path(ppy_fs_proj,"site");
            char *ip=ppy_fs_join_path(ppy_fs_proj,"index"); ppy_fs_load_index(ip); free(ip);
            if(ppy_fs_nsearch>=0){                      /* (searched already: in front now) */
                ppy_fs_add_search(ppy_fs_site,(int)strlen(ppy_fs_site));
                for(int i=ppy_fs_nsearch-1;i>0;i--) ppy_fs_search[i]=ppy_fs_search[i-1];
                ppy_fs_search[0]=xstrdup2(ppy_fs_site); }
            break;
        }
        char *up;                                       /* the folder above */
        size_t n=strlen(dir);
        if(dir[0]=='/'){
            while(n>1 && dir[n-1]=='/') n--;
            while(n>0 && dir[n-1]!='/') n--;
            while(n>1 && dir[n-1]=='/') n--;
            if(n==0 || (n==1 && strlen(dir)==1)){ free(dir); dir=NULL; break; }
            up=xstrndup2(dir,(int)n);
        } else up=ppy_fs_join_path(dir,"..");
        free(dir); dir=up;
    }
    free(dir);
}
char *ppy_fs_package_problem(const char *path, const char *module_name){
    if(!ppy_fs_site || !path) return NULL;
    size_t n=strlen(ppy_fs_site);
    if(strncmp(path,ppy_fs_site,n) || (path[n]!='/' && path[n]!='\\')) return NULL;
    char buf[600];
    for(int i=0;i<ppy_fs_npkgs;i++){ PpyPkgModule *m=&ppy_fs_pkgs[i];
        if(strcmp(m->mod,module_name)) continue;
        if(!strcmp(m->status,"ok")) return NULL;
        if(!strcmp(m->status,"fail"))
            snprintf(buf,sizeof buf,"module '%s' (package %s %s) did not pass the check for compiled programs: "
                     "see .plainpy/check/%s-%s.txt (`plainpy check %s`)",module_name,m->dist,m->ver,m->dist,m->ver,m->dist);
        else snprintf(buf,sizeof buf,"module '%s' (package %s %s) has not been checked for compiled programs: run `plainpy check %s`",
                      module_name,m->dist,m->ver,m->dist);
        return xstrdup2(buf);
    }
    snprintf(buf,sizeof buf,"module '%s' of the project's packages has not been checked for compiled programs: run `plainpy sync`",module_name);
    return xstrdup2(buf);
}

static char *ppy_fs_format_error(const char *prefix,const char *path){
    const char *reason=(errno!=0)?strerror(errno):"I/O error";
    size_t n=strlen(prefix)+strlen(path)+strlen(reason)+8;
    char *msg=(char*)xmalloc(n);
    snprintf(msg,n,"%s %s: %s",prefix,path,reason);
    return msg;
}

size_t ppy_fs_last_len;
char *ppy_fs_read_file_stdio_path(const char *path,char **error_message){
    errno=0;
    FILE *f=fopen(path,"rb");
    if(!f){
        if(error_message) *error_message=ppy_fs_format_error("cannot open",path);
        return NULL;
    }

    size_t cap=4096;
    size_t len=0;
    char *buf=(char*)xmalloc(cap+1);
    for(;;){
        if(len==cap){
            cap*=2;
            buf=(char*)xrealloc(buf,cap+1);
        }
        size_t n=fread(buf+len,1,cap-len,f);
        len+=n;
        if(n==0) break;
    }

    if(ferror(f)){
        if(error_message) *error_message=ppy_fs_format_error("cannot read",path);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[len]=0;
    ppy_fs_last_len=len;
    return buf;
}

char *ppy_fs_try_read_file(const char *path,char **error_message){
    if(error_message) *error_message=NULL;
    char *norm=ppy_fs_normalize_path(path);
    char *data=ppy_fs_backend_read_file(norm,error_message);

    if(!data && !ppy_fs_path_is_absolute(norm) && strcmp(PPY_FS_DEFAULT_IMPORT_DIR,".")!=0){
        char *fallback_err=NULL;
        char *candidate=ppy_fs_join_path(PPY_FS_DEFAULT_IMPORT_DIR,norm);
        data=ppy_fs_backend_read_file(candidate,&fallback_err);
        if(data){
            if(error_message && *error_message){ free(*error_message); *error_message=NULL; }
        } else {
            if(error_message){
                if(*error_message) free(*error_message);
                *error_message=fallback_err;
                fallback_err=NULL;
            }
        }
        free(fallback_err);
        free(candidate);
    }

    free(norm);
    return data;
}

char *ppy_fs_read_file(const char *path){
    char *err=NULL;
    char *data=ppy_fs_try_read_file(path,&err);
    if(!data){
        die(err?err:"cannot read file");
    }
    free(err);
    return data;
}

int ppy_fs_write_file(const char *path,const char *data,size_t len,char **error_message){
    if(error_message) *error_message=NULL;
    char *norm=ppy_fs_normalize_path(path);
    int rc=ppy_fs_backend_write_file(norm,data,len,error_message);
    free(norm);
    return rc;
}
int ppy_fs_remove(const char *path){ char *norm=ppy_fs_normalize_path(path); int rc=ppy_fs_backend_remove(norm); free(norm); return rc; }
int ppy_fs_exists(const char *path){ char *norm=ppy_fs_normalize_path(path); int rc=ppy_fs_backend_exists(norm); free(norm); return rc; }
