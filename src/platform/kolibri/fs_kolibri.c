/* KolibriOS filesystem backend using f70.
 *
 * Documentation: https://wiki.kolibrios.org/wiki/SysFn70/ru
 *
 * f70 uses paths in the struct - no open/close needed.
 *   Subfunc 0: read file at position (returns data)
 *   Subfunc 5: get file info (returns BDVK)
 *
 * f70 struct (80 bytes, ebx points to it):
 *   +0:  dword  subfunction
 *   +4:  dword  position_lo
 *   +8:  dword  position_hi / flags
 *   +12: dword  size (bytes to read)
 *   +16: dword  buffer pointer
 *   +20: path string (null-terminated)
 */

#include "fs.h"

/* f70 info block: 20-byte header + inline null-terminated path at +20.
   The block must be large enough to hold the whole path; 59 chars is far too
   small once imports build nested paths like /rd/1/app/pkg/sub/mod.mpy. */
#define KOS_F70_BUF   300
#define KOS_PATH_MAX  (KOS_F70_BUF - 20 - 1)

const char *mpy_fs_backend_name(void){
    return "kolibrios-native";
}

/* Subfunction 0: Read file at position.
 *   Returns: eax = 0 on success (or 6=EOF), ebx = bytes read */
static int kos_read_file(const char *path, void *buf, int size, int pos_lo, int pos_hi){
    unsigned char bi[KOS_F70_BUF] __attribute__((aligned(16))) = {0};
    *(int*)(bi + 0) = 0;          /* subfunction: read */
    *(int*)(bi + 4) = pos_lo;     /* position low */
    *(int*)(bi + 8) = pos_hi;     /* position high */
    *(int*)(bi + 12) = size;      /* bytes to read */
    *(int*)(bi + 16) = (int)buf;  /* buffer */
    /* Copy path to bi+20 */
    size_t plen = strlen(path);
    if(plen > KOS_PATH_MAX) plen = KOS_PATH_MAX;
    memcpy(bi + 20, path, plen);
    bi[20 + plen] = 0;

    int result;
    __asm__ __volatile__("int $0x40"
        : "=a"(result), "=b"(size)
        : "a"(70), "b"((int)bi)
        : "memory");

    if(result != 0 && result != 6) return result; /* 6 = EOF */
    return size; /* actual bytes read */
}

/* Subfunction 5: Get file info (BDVK, file_size at +32). */
static int kos_file_size(const char *path, int *lo32, int *hi32){
    unsigned char bi[KOS_F70_BUF] __attribute__((aligned(16))) = {0};
    unsigned char bdvk[40] __attribute__((aligned(16))) = {0};
    *(int*)(bi + 0) = 5;          /* subfunction: get info */
    *(int*)(bi + 16) = (int)bdvk; /* pointer to BDVK buffer */
    /* Copy path to bi+20 */
    size_t plen = strlen(path);
    if(plen > KOS_PATH_MAX) plen = KOS_PATH_MAX;
    memcpy(bi + 20, path, plen);
    bi[20 + plen] = 0;

    int result;
    __asm__ __volatile__("int $0x40"
        : "=a"(result)
        : "a"(70), "b"((int)bi)
        : "memory");

#ifdef MPY_FS_DEBUG
    fprintf(stderr, "[minipy] f70 sub5 path=%s result=%d\n", path, result);
    fprintf(stderr, "[minipy] BDVK hex:");
    for(int i = 0; i < 40; i++){
        if(i % 16 == 0) fprintf(stderr, "\n  %02x: ", i);
        fprintf(stderr, "%02x ", (unsigned char)bdvk[i]);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "[minipy] size qword@32: lo=%d hi=%d\n",
        *(int*)(bdvk+32), *(int*)(bdvk+36));
    fflush(stderr);
#endif

    *lo32 = *(int*)(bdvk + 32);
    *hi32 = *(int*)(bdvk + 36);
    return result;
}

/* Read entire file into a heap-allocated buffer. */
char *mpy_fs_backend_read_file(const char *normalized_path,char **error_message){
    /* First, try to get file size */
    int lo, hi;
    int st = kos_file_size(normalized_path, &lo, &hi);
    int fsize;
    if(st == 0 && lo > 0){
        fsize = lo;
    } else {
        /* Can't get size, use fallback chunked read */
        fsize = 0;
    }

    if(fsize > 0){
        /* Read entire file at once */
        char *buf = (char*)xmalloc((size_t)fsize + 1);
        int rd = kos_read_file(normalized_path, buf, fsize, 0, 0);
#ifdef MPY_FS_DEBUG
        fprintf(stderr, "[minipy] read_file rd=%d exp=%d\n", rd, fsize);
        fflush(stderr);
#endif
        if(rd > 0){
            buf[rd] = 0;
            return buf;
        }
        free(buf);
        if(error_message) *error_message = xstrdup2("cannot read file");
        return NULL;
    }

    /* Fallback: read in chunks */
    int chunk = 8192;
    int total = 0, cap = 65536;
    char *buf = (char*)xmalloc((size_t)cap + 1);

    for(;;){
        if(total + chunk > cap){
            cap *= 2;
            buf = (char*)xrealloc(buf, (size_t)cap + 1);
        }
        int n = kos_read_file(normalized_path, buf + total, chunk, total, 0);
        if(n < 0){
            free(buf);
            if(error_message) *error_message = xstrdup2("read error");
            return NULL;
        }
        if(n == 0) break;
        total += n;
    }

    if(total == 0){
        free(buf);
        if(error_message) *error_message = xstrdup2("empty file");
        return NULL;
    }

    buf[total] = 0;
    return buf;
}
