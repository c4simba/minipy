/* Default host filesystem backend.
   Uses ANSI stdio and preserves normal Unix/Windows relative-path behaviour. */

static const char *mpy_fs_backend_name(void){
    return "host-stdio";
}

static char *mpy_fs_backend_read_file(const char *normalized_path,char **error_message){
    return mpy_fs_read_file_stdio_path(normalized_path,error_message);
}
