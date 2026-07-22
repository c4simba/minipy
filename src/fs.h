#ifndef MPY_FS_H
#define MPY_FS_H

#include "util.h"

/* ========================= Filesystem abstraction =========================
   MiniPy talks to the outside filesystem only through this layer.

   Front API (platform-independent, implemented in fs.c):
       mpy_fs_read_file / mpy_fs_try_read_file / mpy_fs_dirname
       mpy_fs_module_path / mpy_fs_backend_name

   Backend contract (implemented per platform under platform/host and
   platform/kolibri):
       mpy_fs_backend_name and mpy_fs_backend_read_file

   Contract:
   - returned strings are heap-allocated (xmalloc/xstrdup2), caller frees;
   - read buffers are NUL-terminated;
   - mpy_fs_try_read_file returns NULL and fills *error_message on I/O errors;
   - mpy_fs_read_file calls die() on I/O errors.
*/

char       *mpy_fs_read_file(const char *path);
char       *mpy_fs_try_read_file(const char *path, char **error_message);
char       *mpy_fs_dirname(const char *path);
char       *mpy_fs_module_path(const char *importer_dir, const char *module_name);
const char *mpy_fs_backend_name(void);

/* Backend contract (exactly one backend .o is linked per build). */
char       *mpy_fs_backend_read_file(const char *normalized_path, char **error_message);

/* Shared ANSI-stdio reader; used by the host backend. */
char       *mpy_fs_read_file_stdio_path(const char *path, char **error_message);

#endif /* MPY_FS_H */
