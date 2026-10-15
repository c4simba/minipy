/* Default host filesystem backend.
   Uses ANSI stdio and preserves normal Unix/Windows relative-path behaviour. */

#include "fs.h"

const char *mpy_fs_backend_name(void){
    return "host-stdio";
}

char *mpy_fs_backend_read_file(const char *normalized_path,char **error_message){
    return mpy_fs_read_file_stdio_path(normalized_path,error_message);
}

int mpy_fs_backend_write_file(const char *normalized_path,const char *data,size_t len,char **error_message){
    FILE *f=fopen(normalized_path,"wb");
    if(!f || fwrite(data,1,len,f)!=len){
        if(f) fclose(f);
        if(error_message){ size_t n=strlen(normalized_path)+32; *error_message=(char*)xmalloc(n); snprintf(*error_message,n,"cannot write %s",normalized_path); }
        return 1;
    }
    return fclose(f)!=0;
}
int mpy_fs_backend_remove(const char *normalized_path){ return remove(normalized_path); }
int mpy_fs_backend_exists(const char *normalized_path){ FILE *f=fopen(normalized_path,"rb"); if(!f) return 0; fclose(f); return 1; }
