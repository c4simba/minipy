/* What the static newlib of KolibriOS's ports toolchain
   (tools/kolibrios-toolchain.sh; make kolibrios KOS_PORTS=1) leaves to the
   program: the global errno its system layer sets, and the DLL import
   relocation startup.c runs for the SDK's libc.dll (nothing to relocate in
   a static program). */
#ifdef MPY_KOS_PORTS_LIBC
#undef errno
int errno;
void _pei386_runtime_relocator(void){}
#else
typedef int mpy_kos_ports_libc_unused;     /* (an empty file is not C) */
#endif
