/* ========================= Target esp32: an ESP-IDF project =========================
   The program as C for a port (the c target: aot/x2c.c, x2c_port.c) with the
   ESP32 family's port (port_esp32.c here), in a project ESP-IDF builds:

     plainpy --compile --target esp32 blink.py      blink/ (and runs `idf.py build` in it
                                                    when ESP-IDF is set up: idf.py on the PATH)
       blink/CMakeLists.txt, sdkconfig.defaults     (project/ here, the name and chip put in)
       blink/main/blink.c, ppy_port.h, ppy_port_esp32.c, CMakeLists.txt
     cd blink && idf.py flash monitor

   --chip (or --target esp32s3 ...) chooses the chip; --build / --no-build
   whether idf.py builds it now. See README.md. */

#include "targets/targets.h"
#include "targets/platform.h"
#include "util.h"
#include "fs.h"

int aot_write_c(AotBuild *b, const X2cOptions *xo, const char *c_path, size_t *clen);   /* targets/macos/target.c */
int aot_write_port_files(AotBuild *b, const char *dir, const char *port, char **port_path);   /* targets/c/target.c */

/* text with every @NAME@ and @CHIP@ replaced */
static char *fill(const char *t, const char *name, const char *chip){
    size_t cap=strlen(t)+16*(strlen(name)+strlen(chip))+16, k=0; char *r=(char*)xmalloc(cap);
    while(*t){
        const char *rep=NULL;
        if(!strncmp(t,"@NAME@",6)) rep=name; else if(!strncmp(t,"@CHIP@",6)) rep=chip;
        if(rep){ size_t l=strlen(rep); if(k+l+1>cap){ cap=(k+l+1)*2; r=(char*)xrealloc(r,cap); } memcpy(r+k,rep,l); k+=l; t+=6; }
        else { if(k+2>cap){ cap*=2; r=(char*)xrealloc(r,cap); } r[k++]=*t++; }
    }
    r[k]=0; return r;
}
static int write_filled(AotBuild *b, const char *path, const char *text_name, const char *name, const char *chip){
    char *t=fill(aot_lib_text(text_name),name,chip);
    int rc=aot_write_file(path,t,strlen(t),b->verbose); free(t);
    return rc;
}
/* program on the PATH: 1 */
static int on_path(const char *program){
    const char *p=getenv("PATH");
    while(p && *p){
        const char *e=strchr(p,':'); size_t n=e?(size_t)(e-p):strlen(p);
        char dir[1024]; if(n && n<sizeof dir-64){ memcpy(dir,p,n); dir[n]=0; char *f=ppy_fs_join_path(dir,program); int ok=ppy_fs_exists(f); free(f); if(ok) return 1; }
        p=e?e+1:NULL;
    }
    return 0;
}

static int build(AotBuild *b){
    const char *chip=b->chip ? b->chip : strcmp(b->target_name,"esp32") && !strncmp(b->target_name,"esp32",5) ? b->target_name : "esp32";
    /* the project's name: the output's last part, as an identifier */
    const char *slash=strrchr(b->base,'/'); const char *last=slash?slash+1:b->base;
    char name[64]; size_t k=0;
    for(const char *p=last;*p && k<sizeof name-1;p++) name[k++]=(char)((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_' ? *p : '_');
    name[k]=0; if(!k || (name[0]>='0'&&name[0]<='9')){ memmove(name+1,name,k+1); name[0]='p'; }
    char *dir=aot_concat(b->base,"/"), *mdir=aot_concat(dir,"main/");
    { char *q=aot_tool_args("-p {in}",mdir,mdir); int rc=aot_run_tool("mkdir",q,"-o",b->verbose); free(q); if(rc) return 1; }
    char path[2048];
    snprintf(path,sizeof path,"%sCMakeLists.txt",dir);       if(write_filled(b,path,"esp32_cmake",name,chip)) return 1;
    snprintf(path,sizeof path,"%ssdkconfig.defaults",dir);   if(write_filled(b,path,"esp32_sdkconfig",name,chip)) return 1;
    snprintf(path,sizeof path,"%sCMakeLists.txt",mdir);      if(write_filled(b,path,"esp32_main_cmake",name,chip)) return 1;
    snprintf(path,sizeof path,"%sppy_port_esp32.c",mdir);    if(write_filled(b,path,"esp32_port",name,chip)) return 1;
    if(aot_write_port_files(b,mdir,NULL,NULL)) return 1;    /* ppy_port.h */
    X2cOptions xo; memset(&xo,0,sizeof xo); xo.mode=X2C_PORT; xo.stack=b->stack; xo.arena=b->arena;
    snprintf(path,sizeof path,"%s%s.c",mdir,name);
    if(aot_write_c(b,&xo,path,NULL)) return 1;
    int idf=on_path("idf.py");
    if(b->run_build==1 && !idf){ fprintf(stderr,"plainpy: idf.py is not on the PATH: set up ESP-IDF first (. $IDF_PATH/export.sh)\n"); return 1; }
    if(b->run_build!=0 && idf){
        char args[2300]; char *q=aot_tool_args("{in}",b->base,b->base);
        snprintf(args,sizeof args,"-C %s -DIDF_TARGET=%s build",q,chip); free(q);
        if(aot_run_tool("idf.py",args,"--no-build",1)) return 1;
        printf("built %s for %s: cd %s && idf.py flash monitor\n",b->base,chip,b->base);
    } else
        printf("wrote the ESP-IDF project %s (%s): cd %s && idf.py build flash monitor\n",b->base,chip,b->base);
    free(dir); free(mdir);
    return 0;
}

const AotTargetDef aot_target_esp32={
    AOT_TARGET_ESP32, "esp32", "esp32s2 esp32s3 esp32c2 esp32c3 esp32c5 esp32c6 esp32c61 esp32h2 esp32p4", "esp32",
    AOT_ABI_LINUX, AOT_OUT_C, AOT_CFN_STATIC,
    16384, 8192, 16384,
    "an ESP-IDF project for an ESP32 chip (the c target with port_esp32.c)",
    build
};
