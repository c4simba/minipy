/* ========================= Typed compiler, code generator: ctypes =========================
   Calls of C functions through ctypes (cdecl). */

#include "gen.h"

/* A C function through ctypes (cdecl, i386 System V): the arguments are
   built in a block the call finds 16-byte aligned, the old esp saved above
   it; the function is reached through its slot CI<id>, which the dynamic
   linker fills in. Results: eax (edx:eax for 64 bits: the low half), st0 for
   floating point, a char * copied into a new str. */
static int csize(int k){ return k==CT_DOUBLE||k==CT_LONGLONG||k==CT_ULONGLONG ? 8 : 4; }
/* macos: the C translation (aot/x2c.c) calls the function natively, so each
   call carries its C signature in a comment: ";@ccall R:ARGS[|VARARGS]", one
   letter per type (i int, u unsigned, h/H short, b/B char, o bool, l/L long,
   q/Q long long, d double, f float, s char *, p void *, v void). */
static char ct_letter(int k){
    switch(k){
        case CT_UINT: return 'u'; case CT_SHORT: return 'h'; case CT_USHORT: return 'H'; case CT_BYTE: return 'b';
        case CT_UBYTE: return 'B'; case CT_BOOL: return 'o'; case CT_LONG: return 'l'; case CT_ULONG: return 'L';
        case CT_LONGLONG: return 'q'; case CT_ULONGLONG: return 'Q'; case CT_DOUBLE: return 'd'; case CT_FLOAT: return 'f';
        case CT_CHARP: return 's'; case CT_VOIDP: return 'p'; case CT_VOID: return 'v'; default: return 'i';
    }
}
/* The fixed parameters of the C library's variadic functions: without
   argtypes the rest of their arguments are the variadic ones. */
static int c_variadic_fixed(const char *sym){
    static const struct { const char *name; int fixed; } v[]={{"printf",1},{"fprintf",2},{"dprintf",2},{"sprintf",2},{"snprintf",3},
        {"scanf",1},{"fscanf",2},{"sscanf",2},{"open",2},{"openat",3},{"fcntl",2},{"ioctl",2},{"syslog",2},{"execl",2},{"execlp",2},{NULL,0}};
    for(int i=0;v[i].name;i++) if(!strcmp(v[i].name,sym)) return v[i].fixed;
    return -1;
}
static void ccall_marker(F *f, ACFunc *cf, const int *kinds, int n){
    if(gg->target!=AOT_TARGET_MACOS) return;
    char sig[64]; int k=0, fixed=cf->nargtypes>=0 ? cf->nargtypes : c_variadic_fixed(cf->sym);
    sig[k++]=ct_letter(cf->restype); sig[k++]=':';
    for(int i=0;i<n;i++){ if(i==fixed) sig[k++]='|'; sig[k++]=ct_letter(kinds[i]); }
    sig[k]=0;
    buf_printf(&f->code,"        ;@ccall %s\n",sig);
}
int gen_ccall(F *f, Expr *e, XInfo *xi){
    ACFunc *cf=xi->cfn; int off[16], size=0;
    for(int i=0;i<e->count;i++){ off[i]=size; size+=csize(xi->argmap[i]); }
    E(f,"mov eax,esp"); E(f,"sub esp,%d",size+4); E(f,"and esp,-16"); E(f,"mov [esp+%d],eax",size);
    for(int i=0;i<e->count;i++){ Expr *a=e->items[i]; int k=xi->argmap[i]; Ty *t=TY(a);
        if(k==CT_DOUBLE||k==CT_FLOAT){ gen_as(f,a,TY_FLOAT_T,0); E(f,k==CT_DOUBLE?"fstp qword [esp+%d]":"fstp dword [esp+%d]",off[i]); continue; }
        gen_borrow(f,a);
        if(t->k==TY_STR||t->k==TY_BYTES){ int l=new_label(), l2=new_label();                 /* the bytes, NUL-terminated ("" too) */
            E(f,"test eax,eax"); E(f,"jnz L%d",l); E(f,"mov eax,Z%d",zlit("")); E(f,"jmp L%d",l2); LBL(f,l); E(f,"add eax,12"); LBL(f,l2); }
        else if(t->k==TY_BUF){ int l=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"add eax,12"); LBL(f,l); }
        E(f,"mov [esp+%d],eax",off[i]);
        if(k==CT_LONGLONG||k==CT_ULONGLONG){ if(!is_lng(t)) widen(f,k==CT_LONGLONG); E(f,"mov [esp+%d],edx",off[i]+4); }
    }
    int kinds[16]; for(int i=0;i<e->count;i++) kinds[i]=xi->argmap[i];
    ccall_marker(f,cf,kinds,e->count);
    E(f,"call [CI%d]   ; %s",cf->id,cf->sym);
    E(f,"mov esp,[esp+%d]",size);
    switch(cf->restype){
        case CT_SHORT: E(f,"movsx eax,ax"); break;
        case CT_USHORT: E(f,"movzx eax,ax"); break;
        case CT_BYTE: E(f,"movsx eax,al"); break;
        case CT_UBYTE: E(f,"movzx eax,al"); break;
        case CT_BOOL: E(f,"test al,al"); E(f,"setnz al"); E(f,"movzx eax,al"); break;
        case CT_CHARP:{ int l=new_label(); E(f,"test eax,eax"); E(f,"jz L%d",l); E(f,"mov edx,0x7FFFFFFF"); CALLRT(f,"rt_mem_cstr"); LBL(f,l); return 1; }
        default: break;
    }
    if(is_lng(TY(e)) && cf->restype!=CT_LONGLONG && cf->restype!=CT_ULONGLONG)      /* as an int (64 bits) */
        widen(f,cf->restype==CT_DEFAULT||cf->restype==CT_INT||cf->restype==CT_SHORT||cf->restype==CT_BYTE||cf->restype==CT_LONG);
    return 0;
}
/* ctypes.create_string_buffer / string_at / addressof / get_errno */
int gen_ctypes_fn(F *f, Expr *e, XInfo *xi){
    const char *m=xi->name+7;
    if(!strcmp(m,"create_string_buffer")){ Ty *t0=TY(e->items[0]); gen_borrow(f,e->items[0]);
        if(t0->k==TY_STR){ CALLRT(f,"rt_buf_from_str"); E(f,"inc dword [eax+8]"); }   /* and its 0 byte (a buffer has one more) */
        else CALLRT(f,"rt_buf_new");
        return 1; }
    if(!strcmp(m,"addressof")){ gen_borrow(f,e->items[0]); E(f,"add eax,12"); widen(f,0); return 0; }
    if(!strcmp(m,"string_at")){ gen(f,e->items[0]); E(f,"push eax");
        if(e->count==2){ gen(f,e->items[1]); to_i32(f,TY(e->items[1])); E(f,"mov edx,eax"); E(f,"pop eax"); CALLRT(f,"rt_mem_peek_str"); return 1; }
        E(f,"pop eax"); E(f,"mov edx,0x7FFFFFFF"); CALLRT(f,"rt_mem_cstr"); return 1; }
    if(!strcmp(m,"get_errno")){ E(f,"mov eax,esp"); E(f,"and esp,-16"); E(f,"sub esp,12"); E(f,"push eax");
        ccall_marker(f,xi->cfn,NULL,0);
        E(f,"call [CI%d]   ; __errno_location",xi->cfn->id); E(f,"pop esp"); E(f,"mov eax,[eax]"); widen(f,1); return 0; }
    cg_fail(e->line,"unsupported ctypes function");
    return 0;
}

