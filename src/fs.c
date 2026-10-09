/* ========================= Filesystem abstraction (platform-independent) ========================= */

#include "fs.h"

#ifndef MPY_FS_PATH_SEP
#define MPY_FS_PATH_SEP '/'
#endif

#ifndef MPY_FS_DEFAULT_IMPORT_DIR
#define MPY_FS_DEFAULT_IMPORT_DIR "."
#endif

static int mpy_fs_is_sep(char c){
    return c=='/' || c=='\\';
}

static int mpy_fs_path_is_absolute(const char *path){
    if(!path || !path[0]) return 0;
    if(mpy_fs_is_sep(path[0])) return 1;
    if(isalpha((unsigned char)path[0]) && path[1]==':') return 1;
    return 0;
}

static char *mpy_fs_normalize_path(const char *path){
    if(!path || !*path) return xstrdup2(".");
    size_t n=strlen(path);
    char *r=(char*)xmalloc(n+1);
    int out=0;
    int prev_sep=0;
    for(size_t i=0;i<n;i++){
        char c=path[i];
        if(mpy_fs_is_sep(c)) c=MPY_FS_PATH_SEP;
        if(c==MPY_FS_PATH_SEP){
            if(prev_sep && out>1) continue;
            prev_sep=1;
        } else {
            prev_sep=0;
        }
        r[out++]=c;
    }
    while(out>1 && r[out-1]==MPY_FS_PATH_SEP) out--;
    r[out]=0;
    return r;
}

char *mpy_fs_dirname(const char *path){
    char *norm=mpy_fs_normalize_path(path);
    const char *last=NULL;
    for(const char *p=norm; *p; p++) if(*p==MPY_FS_PATH_SEP) last=p;
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

static char *mpy_fs_join_path(const char *dir,const char *rel){
    if(!dir || !*dir || strcmp(dir,".")==0) return mpy_fs_normalize_path(rel);
    if(rel && mpy_fs_is_sep(rel[0])) return mpy_fs_normalize_path(rel);
    size_t n=strlen(dir)+1+strlen(rel)+1;
    char *tmp=(char*)xmalloc(n);
    snprintf(tmp,n,"%s%c%s",dir,MPY_FS_PATH_SEP,rel);
    char *r=mpy_fs_normalize_path(tmp);
    free(tmp);
    return r;
}

static char *mpy_fs_module_relpath(const char *module_name){
    size_t n=strlen(module_name);
    char *r=(char*)xmalloc(n+4+1);
    for(size_t i=0;i<n;i++){
        char c=module_name[i];
        r[i]=(c=='.')?MPY_FS_PATH_SEP:c;
    }
    memcpy(r+n,".mpy",5);
    return r;
}

char *mpy_fs_module_path(const char *importer_dir,const char *module_name){
    char *rel=mpy_fs_module_relpath(module_name);
    const char *base=(importer_dir && importer_dir[0]) ? importer_dir : ".";
    if(strcmp(base,".")==0 && strcmp(MPY_FS_DEFAULT_IMPORT_DIR,".")!=0){
        base=MPY_FS_DEFAULT_IMPORT_DIR;
    }
    char *r=mpy_fs_join_path(base,rel);
    free(rel);
    return r;
}

/* ---- the module search path ---- */
static char *mpy_fs_program_dir;
static char **mpy_fs_search; static int mpy_fs_nsearch=-1;

void mpy_fs_set_program(const char *argv0){
    if(argv0 && (strchr(argv0,'/') || strchr(argv0,'\\'))) mpy_fs_program_dir=mpy_fs_dirname(argv0);
}
static void mpy_fs_add_search(const char *dir,int len){
    if(len<=0) return;
    mpy_fs_search=(char**)xrealloc(mpy_fs_search,sizeof(char*)*(size_t)(mpy_fs_nsearch+1));
    mpy_fs_search[mpy_fs_nsearch++]=xstrndup2(dir,len);
}
static void mpy_fs_init_search(void){
    if(mpy_fs_nsearch>=0) return;
    mpy_fs_nsearch=0;
    const char *env=getenv("MINIPYPATH");
    for(const char *p=env; p && *p; ){
        const char *e=p; while(*e && *e!=':' && *e!=';') e++;
        if(e-p==1 && isalpha((unsigned char)*p) && *e==':' && (e[1]=='\\' || e[1]=='/')){ e++; while(*e && *e!=':' && *e!=';') e++; }   /* C:\dir */
        mpy_fs_add_search(p,(int)(e-p));
        p=*e?e+1:e;
    }
    if(mpy_fs_program_dir){ char *lib=mpy_fs_join_path(mpy_fs_program_dir,"lib"); mpy_fs_add_search(lib,(int)strlen(lib)); free(lib); }
}
static char *mpy_fs_try_module(const char *dir,const char *module_name){
    static const char *forms[]={"%s.mpy","%s/__init__.mpy","%s.py","%s/__init__.py",NULL};
    char *rel=mpy_fs_module_relpath(module_name);
    rel[strlen(rel)-4]=0;                                   /* without ".mpy" */
    for(int i=0;forms[i];i++){
        size_t n=strlen(rel)+16; char *name=(char*)xmalloc(n); snprintf(name,n,forms[i],rel);
        char *path=mpy_fs_join_path(dir,name);
        free(name);
        if(mpy_fs_exists(path)){ free(rel); return path; }
        free(path);
    }
    free(rel);
    return NULL;
}
char *mpy_fs_find_module(const char *importer_dir,const char *module_name){
    const char *base=(importer_dir && importer_dir[0]) ? importer_dir : ".";
    if(strcmp(base,".")==0 && strcmp(MPY_FS_DEFAULT_IMPORT_DIR,".")!=0) base=MPY_FS_DEFAULT_IMPORT_DIR;
    char *found=mpy_fs_try_module(base,module_name);
    mpy_fs_init_search();
    for(int i=0;!found && i<mpy_fs_nsearch;i++) found=mpy_fs_try_module(mpy_fs_search[i],module_name);
    return found;
}

static char *mpy_fs_format_error(const char *prefix,const char *path){
    const char *reason=(errno!=0)?strerror(errno):"I/O error";
    size_t n=strlen(prefix)+strlen(path)+strlen(reason)+8;
    char *msg=(char*)xmalloc(n);
    snprintf(msg,n,"%s %s: %s",prefix,path,reason);
    return msg;
}

size_t mpy_fs_last_len;
char *mpy_fs_read_file_stdio_path(const char *path,char **error_message){
    errno=0;
    FILE *f=fopen(path,"rb");
    if(!f){
        if(error_message) *error_message=mpy_fs_format_error("cannot open",path);
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
        if(error_message) *error_message=mpy_fs_format_error("cannot read",path);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[len]=0;
    mpy_fs_last_len=len;
    return buf;
}

char *mpy_fs_try_read_file(const char *path,char **error_message){
    if(error_message) *error_message=NULL;
    char *norm=mpy_fs_normalize_path(path);
    char *data=mpy_fs_backend_read_file(norm,error_message);

    if(!data && !mpy_fs_path_is_absolute(norm) && strcmp(MPY_FS_DEFAULT_IMPORT_DIR,".")!=0){
        char *fallback_err=NULL;
        char *candidate=mpy_fs_join_path(MPY_FS_DEFAULT_IMPORT_DIR,norm);
        data=mpy_fs_backend_read_file(candidate,&fallback_err);
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

char *mpy_fs_read_file(const char *path){
    char *err=NULL;
    char *data=mpy_fs_try_read_file(path,&err);
    if(!data){
        die(err?err:"cannot read file");
    }
    free(err);
    return data;
}

int mpy_fs_write_file(const char *path,const char *data,size_t len,char **error_message){
    if(error_message) *error_message=NULL;
    char *norm=mpy_fs_normalize_path(path);
    int rc=mpy_fs_backend_write_file(norm,data,len,error_message);
    free(norm);
    return rc;
}
int mpy_fs_remove(const char *path){ char *norm=mpy_fs_normalize_path(path); int rc=mpy_fs_backend_remove(norm); free(norm); return rc; }
int mpy_fs_exists(const char *path){ char *norm=mpy_fs_normalize_path(path); int rc=mpy_fs_backend_exists(norm); free(norm); return rc; }
