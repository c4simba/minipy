; ========================= PlainPy compiled-program runtime (fasm, i386) =========================
; Building blocks the code generator (aot/codegen) pastes into a program -
; only the ones the program needs, plus their dependencies. The file is split
; into blocks:
;
;     ;;; code|data|bss <name> [linux|kolibri] [: dependency ...]
;
; A block runs until the next ";;;" line; a block without a target is used for
; both. Using <name> pulls all of its blocks for the target.
;
; Calling convention of these routines: arguments in eax, edx, ecx (more on
; the stack, cdecl), result in eax (float: st0). They clobber eax, ecx, edx
; and keep ebx, esi, edi, ebp. The x87 stack is left alone unless stated.
;
; Heap objects (reference counted): +0 refcount, +4 destroy routine (eax=obj)
;   str      +8 length, +12 bytes, NUL
;   list/set +8 length, +12 capacity, +16 element array (4 or 8 bytes each)
;   dict     +8 length, +12 capacity, +16 key array (str), +20 value array
;   object   +8 vtable (+0 class name, +4 base vtable, +8 methods; -8 its module's name, -4 "module." or ""), +12 fields
;   buffer   +8 length, +12 bytes
; A null pointer is the empty value (None): "" for str, an empty container.
; Static objects carry a huge refcount and are never destroyed.

;;; code rt_exit linux
rt_exit:                        ; ebx = exit status
if defined CI_fflush
        push    0               ; the C library's stdio buffers (ctypes)
        ;@ccall i:p
        call    dword [CI_fflush]
end if
        mov     eax,1
        int     0x80

;;; code rt_exit kolibri
rt_exit:                        ; ebx = exit status (KolibriOS has none)
if defined rt_con_close
        call    rt_con_close
end if
        or      eax,-1
        int     0x40

;;; code rt_write linux
rt_write:                       ; write ecx[0..edx) to stdout
        push    ebx
        mov     eax,4
        mov     ebx,1
        int     0x80
        pop     ebx
        ret

;;; code rt_write kolibri : rt_con_write
rt_write:                       ; to the shell console
        jmp     rt_con_write

;;; code rt_write_err linux
rt_write_err:                   ; write ecx[0..edx) to stderr
        push    ebx
        mov     eax,4
        mov     ebx,2
        int     0x80
        pop     ebx
        ret

;;; code rt_write_err kolibri : rt_con_write
rt_write_err:
        jmp     rt_con_write

;;; code rt_static
rt_static:                      ; destroy routine of static objects (never reached)
        ret

;;; code rt_errz : rt_write_err
rt_errz:                        ; write the NUL-terminated string esi to stderr
        mov     ecx,esi
        mov     edx,esi
@@:     cmp     byte [edx],0
        je      @f
        inc     edx
        jmp     @b
@@:     sub     edx,ecx
        jmp     rt_write_err

;;; code rt_panic : rt_errz rt_exit
rt_panic:                       ; esi = message: print it to stderr and exit(1)
        call    rt_errz
        mov     esi,rt_lf_z
        call    rt_errz
        mov     ebx,1
        jmp     rt_exit
;;; data rt_panic
rt_lf_z         db 10,0

;;; code rt_raise : rt_panic rt_errz rt_write_err
rt_raise:                       ; esi = exception name, eax = message (str or 0): "Name: message", exit(1)
        push    eax
        call    rt_errz
        pop     eax
        test    eax,eax
        jz      .bare
        cmp     dword [eax+8],0
        je      .bare
        push    eax
        mov     esi,rt_colon_z
        call    rt_errz
        pop     eax
        lea     ecx,[eax+12]
        mov     edx,[eax+8]
        call    rt_write_err
.bare:  mov     esi,rt_empty_z
        jmp     rt_panic
;;; data rt_raise
rt_colon_z      db ': ',0
rt_empty_z      db 0

;;; code rt_panic_index : rt_panic
; The run-time errors below end the program with "Name: message" - or, in a
; program that handles exceptions (rt_throw is present), raise the built-in
; exception (VTX_<Name>/DTX_<Name>, emitted by the code generator).
rt_panic_index:                 ; IndexError: list index out of range
        mov     esi,rt_msg_index_l
        jmp     rt_index_error
rt_panic_index_s:
        mov     esi,rt_msg_index_s
        jmp     rt_index_error
rt_panic_index_t:
        mov     esi,rt_msg_index_t
        jmp     rt_index_error
rt_panic_index_b:
        mov     esi,rt_msg_index_b
rt_index_error:                 ; esi = what is out of range
if defined rt_throw
        mov     eax,VTX_IndexError
        mov     edx,DTX_IndexError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_msg_index
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_panic_index
rt_msg_index    db 'IndexError: ',0
rt_msg_index_l  db 'list index out of range',0
rt_msg_index_s  db 'string index out of range',0
rt_msg_index_t  db 'tuple index out of range',0
rt_msg_index_b  db 'index out of range',0

;;; code rt_panic_none : rt_panic
rt_panic_none:
if defined rt_throw
        mov     eax,VTX_AttributeError
        mov     edx,DTX_AttributeError
        xor     ecx,ecx
        mov     esi,rt_msg_none_t
        jmp     rt_raise_builtin
else
        mov     esi,rt_msg_none
        jmp     rt_panic
end if
;;; data rt_panic_none
rt_msg_none     db 'AttributeError: '
rt_msg_none_t   db "'NoneType' object has no attribute",0

;;; code rt_panic_zero : rt_panic
rt_panic_zero:
if defined rt_throw
        mov     eax,VTX_ZeroDivisionError
        mov     edx,DTX_ZeroDivisionError
        xor     ecx,ecx
        mov     esi,rt_msg_zero_t
        jmp     rt_raise_builtin
else
        mov     esi,rt_msg_zero
        jmp     rt_panic
end if
;;; data rt_panic_zero
rt_msg_zero     db 'ZeroDivisionError: '
rt_msg_zero_t   db 'division by zero',0

;;; code rt_panic_value : rt_panic
rt_panic_value:                 ; esi = detail
if defined rt_throw
        mov     eax,VTX_ValueError
        mov     edx,DTX_ValueError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_msg_value
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_panic_value
rt_msg_value    db 'ValueError: ',0

;;; code rt_panic_assert : rt_raise
rt_panic_assert:                ; eax = message (str or 0)
if defined rt_throw
        mov     ecx,eax
        mov     eax,VTX_AssertionError
        mov     edx,DTX_AssertionError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     esi,rt_msg_assert
        jmp     rt_raise
end if
;;; data rt_panic_assert
rt_msg_assert   db 'AssertionError',0

