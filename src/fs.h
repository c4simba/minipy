#ifndef PPY_FS_H
#define PPY_FS_H

#include "util.h"

/* ========================= Filesystem abstraction =========================
   PlainPy talks to the outside filesystem only through this layer.

   Front API (platform-independent, implemented in fs.c):
       ppy_fs_read_file / ppy_fs_try_read_file / ppy_fs_dirname
       ppy_fs_module_path / ppy_fs_backend_name

   Backend contract (implemented per platform under platform/host and
   platform/kolibri):
       ppy_fs_backend_name and ppy_fs_backend_read_file

   Contract:
   - returned strings are heap-allocated (xmalloc/xstrdup2), caller frees;
   - read buffers are NUL-terminated;
   - ppy_fs_try_read_file returns NULL and fills *error_message on I/O errors;
   - ppy_fs_read_file calls die() on I/O errors.
*/

char       *ppy_fs_read_file(const char *path);
extern size_t ppy_fs_last_len;   /* the byte length of the last file read (it may hold NUL bytes) */
char       *ppy_fs_try_read_file(const char *path, char **error_message);
char       *ppy_fs_dirname(const char *path);
char       *ppy_fs_module_path(const char *importer_dir, const char *module_name);
/* The file of module `name` (dotted): <dir>/a/b.py or <dir>/a/b/__init__.py
   for dir = the importing module's folder, then each folder of
   PLAINPYPATH (separated by ':' or ';'), then the `lib` folder next to the
   plainpy executable. NULL if there is none. */
char       *ppy_fs_find_module(const char *importer_dir, const char *module_name);
void        ppy_fs_set_program(const char *argv0);
/* The folder of the running program (plainpy.lib and lib/ are next to it), or NULL. */
const char *ppy_fs_program_dir(void);
const char *ppy_fs_data_dir(void);      /* plainpy.lib and lib/: next to the programs, or <prefix>/lib/plainpy */
char       *ppy_fs_join_path(const char *dir, const char *rel);
/* A project's packages (`plainpy add` / `plainpy sync`): the project of the
   program at script_path is the first folder up from it with .plainpy/index;
   its .plainpy/site is searched for modules first. ppy_fs_package_problem:
   NULL, or why the compiler may not use the module found at path (a module
   of the project's packages that did not pass `plainpy check`; a new string). */
void        ppy_fs_use_project(const char *script_path);
char       *ppy_fs_package_problem(const char *path, const char *module_name);
const char *ppy_fs_backend_name(void);
/* Writing (used by `--compile`): 0 on success. */
int         ppy_fs_write_file(const char *path, const char *data, size_t len, char **error_message);
int         ppy_fs_remove(const char *path);
int         ppy_fs_exists(const char *path);

/* Backend contract (exactly one backend .o is linked per build). */
char       *ppy_fs_backend_read_file(const char *normalized_path, char **error_message);
int         ppy_fs_backend_write_file(const char *normalized_path, const char *data, size_t len, char **error_message);
int         ppy_fs_backend_remove(const char *normalized_path);
int         ppy_fs_backend_exists(const char *normalized_path);

/* Shared ANSI-stdio reader; used by the host backend. */
char       *ppy_fs_read_file_stdio_path(const char *path, char **error_message);

#endif /* PPY_FS_H */
