; ========================= MiniPy compiled-program runtime (fasm, i386) =========================
; Building blocks the code generator (aot_codegen.c) pastes into a program -
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

; ---------------------------------------------------------------- exceptions
; A `try` keeps a handler record in its frame: +0 the enclosing record, +4 esp,
; +8 ebp, +12 handler address, +16 ebx, +20 esi, +24 edi; rt_exc_top is the
; innermost one. Raising pops it, restores those registers and jumps to the
; handler with the exception in rt_exc_cur.

;;; code rt_throw : rt_exc_uncaught
rt_throw:                       ; eax = exception (owned): unwind to the innermost handler
        mov     [rt_exc_cur],eax
        mov     ecx,[rt_exc_top]
        test    ecx,ecx
        jz      rt_exc_uncaught
        mov     edx,[ecx]
        mov     [rt_exc_top],edx
        fninit                  ; drop whatever the x87 stack held; back to double precision
        push    0x027F
        fldcw   [esp]
        mov     ebx,[ecx+16]
        mov     esi,[ecx+20]
        mov     edi,[ecx+24]
        mov     esp,[ecx+4]
        mov     ebp,[ecx+8]
        jmp     dword [ecx+12]
;;; bss rt_throw
rt_exc_top      rd 1
rt_exc_cur      rd 1

;;; code rt_exc_uncaught : rt_write_err rt_exit
rt_exc_uncaught:                ; eax = exception: "module.Name: message" (just Name without one), exit(1)
        mov     ebx,eax
        mov     ecx,[rt_uncaught_hook]      ; (SystemExit: the status it says)
        test    ecx,ecx
        jz      .show
        mov     dword [rt_uncaught_hook],0
        push    ebx
        call    ecx
        add     esp,4
        cmp     eax,0
        jl      .show
        mov     ebx,eax
        jmp     rt_exit
.show:  mov     eax,[ebx+8]
        mov     eax,[eax-4]     ; "module." (none for __main__'s classes and the built-in ones)
        lea     ecx,[eax+12]
        mov     edx,[eax+8]
        test    edx,edx
        jz      .name
        call    rt_write_err
.name:  mov     eax,[ebx+8]
        mov     eax,[eax]       ; the class name (str)
        lea     ecx,[eax+12]
        mov     edx,[eax+8]
        call    rt_write_err
        mov     eax,[ebx+12]    ; the message: BaseException's field
        test    eax,eax
        jz      .nl
        cmp     dword [eax+8],0
        je      .nl
        push    eax
        mov     ecx,rt_s_colsp
        mov     edx,2
        call    rt_write_err
        pop     eax
        lea     ecx,[eax+12]
        mov     edx,[eax+8]
        call    rt_write_err
.nl:    mov     ecx,rt_s_colsp+2
        mov     edx,1
        call    rt_write_err
        mov     ebx,1
        jmp     rt_exit
;;; data rt_exc_uncaught
rt_s_colsp      db ': ',10
align 4
rt_uncaught_hook dd 0

;;; code rt_raise_builtin : rt_throw rt_obj_new rt_str_new
rt_raise_builtin:               ; eax = vtable, edx = destroy routine, ecx = message str (owned) or 0 and esi = C text
        push    edx
        push    eax
        test    ecx,ecx
        jnz     .have
        xor     ecx,ecx
        test    esi,esi
        jz      .have
        mov     edi,esi
@@:     cmp     byte [edi],0
        je      @f
        inc     edi
        jmp     @b
@@:     sub     edi,esi
        mov     eax,edi
        call    rt_str_new
        push    eax
        lea     edi,[eax+12]
        mov     ecx,[eax+8]
        rep     movsb
        pop     ecx
.have:  push    ecx
        mov     eax,RT_EXC_SIZE ; BaseException's fields
        mov     edx,[esp+4]
        mov     ecx,[esp+8]
        call    rt_obj_new
        pop     ecx
        mov     [eax+12],ecx
        add     esp,8
        jmp     rt_throw

; ---------------------------------------------------------------- memory

;;; code rt_os_alloc linux : rt_panic
rt_os_alloc:                    ; eax = bytes (page multiple) -> eax = zeroed memory (mmap2)
        push    ebx esi edi ebp
        mov     ecx,eax
        xor     ebx,ebx
        mov     edx,3           ; PROT_READ|PROT_WRITE
        mov     esi,0x22        ; MAP_PRIVATE|MAP_ANONYMOUS
        or      edi,-1
        xor     ebp,ebp
        mov     eax,192
        int     0x80
        pop     ebp edi esi ebx
        cmp     eax,-4096
        ja      .oom
        ret
.oom:   mov     esi,rt_msg_oom
        jmp     rt_panic
;;; data rt_os_alloc linux
rt_msg_oom      db 'MemoryError: out of memory',0

;;; code rt_os_alloc kolibri : rt_panic
rt_os_alloc:                    ; eax = bytes -> eax = memory (fn 68.12)
        push    ebx
        mov     ecx,eax
        mov     eax,68
        mov     ebx,12
        int     0x40
        pop     ebx
        test    eax,eax
        jz      .oom
        ret
.oom:   mov     esi,rt_msg_oom
        jmp     rt_panic
;;; data rt_os_alloc kolibri
rt_msg_oom      db 'MemoryError: out of memory',0

;;; code rt_os_free linux
rt_os_free:                     ; eax = memory, edx = bytes (munmap)
        push    ebx
        mov     ebx,eax
        mov     ecx,edx
        mov     eax,91
        int     0x80
        pop     ebx
        ret

;;; code rt_os_free kolibri
rt_os_free:                     ; eax = memory (fn 68.13)
        push    ebx
        mov     ecx,eax
        mov     eax,68
        mov     ebx,13
        int     0x40
        pop     ebx
        ret

;;; code rt_alloc : rt_os_alloc
; Blocks carry a 4-byte header: the size class k (block = 16 << k bytes,
; k = 0..7) or, for big blocks, the page-rounded size | 0x80000000. Freed
; small blocks go to per-class free lists; big blocks go back to the system.
rt_alloc:                       ; eax = bytes -> eax = zeroed memory
        push    ebx esi edi
        lea     ecx,[eax+4]
        cmp     ecx,2048
        ja      .big
        mov     ebx,16
        xor     edx,edx
.cls:   cmp     ecx,ebx
        jbe     .have
        add     ebx,ebx
        inc     edx
        jmp     .cls
.have:  mov     eax,[rt_free_list+edx*4]
        test    eax,eax
        jz      .carve
        mov     ecx,[eax+4]
        mov     [rt_free_list+edx*4],ecx
        jmp     .ready
.carve: mov     eax,[rt_arena]
        mov     ecx,[rt_arena_end]
        sub     ecx,eax
        cmp     ecx,ebx
        jae     .take
        push    edx
        mov     eax,65536
        call    rt_os_alloc
        pop     edx
        lea     ecx,[eax+65536]
        mov     [rt_arena_end],ecx
.take:  lea     ecx,[eax+ebx]
        mov     [rt_arena],ecx
.ready: mov     [eax],edx
.zero:
if defined RT_COUNT_ALLOCS
        inc     dword [rt_live]
        inc     dword [rt_nalloc]
end if
        lea     edi,[eax+4]
        lea     ecx,[ebx-4]
        shr     ecx,2
        mov     edx,eax
        xor     eax,eax
        rep     stosd
        lea     eax,[edx+4]
        pop     edi esi ebx
        ret
.big:   add     ecx,4095
        and     ecx,-4096
        push    ecx
        mov     eax,ecx
        call    rt_os_alloc
        pop     ebx
        mov     ecx,ebx
        or      ecx,0x80000000
        mov     [eax],ecx
        jmp     .zero
;;; bss rt_alloc
rt_live         rd 1            ; --count-allocs: blocks in use, ...
rt_nalloc       rd 1            ; ... allocations, references taken and dropped (not counting 0)
rt_nincref      rd 1
rt_ndecref      rd 1
rt_free_list    rd 8
rt_arena        rd 1
rt_arena_end    rd 1

;;; code rt_free : rt_os_free
rt_free:                        ; eax = memory from rt_alloc (or 0); also the destroy routine of flat objects
        test    eax,eax
        jz      .done
if defined RT_COUNT_ALLOCS
        dec     dword [rt_live]
end if
        sub     eax,4
        mov     ecx,[eax]
        test    ecx,ecx
        js      .big
        mov     edx,[rt_free_list+ecx*4]
        mov     [eax+4],edx
        mov     [rt_free_list+ecx*4],eax
.done:  ret
.big:   and     ecx,0x7FFFFFFF
        mov     edx,ecx
        jmp     rt_os_free

;;; code rt_live_report : rt_write_err rt_alloc
rt_live_report:                 ; --count-allocs: "[live blocks: N, allocations: A, incref: I, decref: D]" to stderr
        push    ebx esi
        mov     ebx,rt_live_fields
        mov     esi,rt_live
.f:     mov     ecx,[ebx]
        movzx   edx,byte [ecx]
        inc     ecx
        call    rt_write_err
        lodsd
        mov     edi,rt_live_num+12
        mov     ecx,10
@@:     xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        test    eax,eax
        jnz     @b
        mov     ecx,edi
        mov     edx,rt_live_num+12
        sub     edx,edi
        call    rt_write_err
        add     ebx,4
        cmp     ebx,rt_live_fields+16
        jb      .f
        mov     ecx,rt_live_end
        mov     edx,2
        pop     esi ebx
        jmp     rt_write_err
;;; data rt_live_report
rt_live_fields  dd rt_live_m1,rt_live_m2,rt_live_m3,rt_live_m4
rt_live_m1      db 14,'[live blocks: '
rt_live_m2      db 15,', allocations: '
rt_live_m3      db 10,', incref: '
rt_live_m4      db 10,', decref: '
rt_live_end     db ']',10
rt_live_num     db 12 dup 0

;;; code rt_incref
rt_incref:                      ; eax = object or 0
        test    eax,eax
        jz      @f
if defined RT_COUNT_ALLOCS
        inc     dword [rt_nincref]
end if
        inc     dword [eax]
@@:     ret

;;; code rt_decref
rt_decref:                      ; eax = object or 0: drop a reference, destroy at zero
        test    eax,eax
        jz      @f
if defined RT_COUNT_ALLOCS
        inc     dword [rt_ndecref]
end if
        dec     dword [eax]
        jnz     @f
        jmp     dword [eax+4]
@@:     ret

;;; code rt_scratch
;;; bss rt_scratch
rt_scratch      rd 2            ; an element taken out of a container (pop, del)

; ---------------------------------------------------------------- strings
; A str: +0 refcount, +4 destroy, +8 length in bytes, +12 the UTF-8 bytes, a
; NUL, then a dword: the length in code points (-1 until someone asks for it).
; Lengths, indexes and slices count code points, as in the interpreter: a
; byte that does not start a valid sequence is one code point of its own.

;;; code rt_str_new : rt_alloc rt_str_free
rt_str_new:                     ; eax = length -> eax = new str (refcount 1, bytes zeroed)
        push    eax
        add     eax,17
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_str_free
        mov     [eax+8],ecx
        mov     dword [eax+13+ecx],-1
        ret

;;; code rt_str_free : rt_free
rt_str_free:                    ; eax = str: destroy it (and forget it in the cursor cache)
        cmp     eax,[rt_str_cur]
        jne     @f
        mov     dword [rt_str_cur],0
@@:     cmp     eax,[rt_str_cur+12]
        jne     @f
        mov     dword [rt_str_cur+12],0
@@:     jmp     rt_free
;;; bss rt_str_free
rt_str_cur      rd 6            ; two (str, code point index, byte offset): where the last indexing went

;;; code rt_str_concat : rt_str_new
rt_str_concat:                  ; eax = a, edx = b -> eax = a + b (new reference)
        push    ebx esi edi
        mov     esi,eax
        mov     edi,edx
        xor     ecx,ecx
        test    esi,esi
        jz      @f
        mov     ecx,[esi+8]
@@:     xor     edx,edx
        test    edi,edi
        jz      @f
        mov     edx,[edi+8]
@@:     test    edx,edx
        jnz     @f
        mov     eax,esi         ; b is empty: a itself
        jmp     .keep
@@:     test    ecx,ecx
        jnz     .both
        mov     eax,edi         ; a is empty: b itself
.keep:  test    eax,eax
        jz      .out
        inc     dword [eax]
.out:   pop     edi esi ebx
        ret
.both:  push    ecx edx
        lea     eax,[ecx+edx]
        call    rt_str_new
        pop     edx ecx
        mov     ebx,eax
        push    edi
        lea     edi,[ebx+12]
        add     esi,12
        rep     movsb
        pop     esi
        add     esi,12
        mov     ecx,edx
        rep     movsb
        mov     eax,ebx
        pop     edi esi ebx
        ret

;;; code rt_str_eq
rt_str_eq:                      ; eax = a, edx = b (0: None) -> eax = 1 if equal
        push    esi edi
        mov     esi,eax
        mov     edi,edx
        cmp     esi,edi
        je      .yes            ; the same str (or both None)
        test    esi,esi
        jz      .no
        test    edi,edi
        jz      .no
        mov     eax,[esi+8]
        mov     ecx,[edi+8]
        cmp     eax,ecx
        jne     .no
        test    ecx,ecx
        jz      .yes
        add     esi,12
        add     edi,12
        repe    cmpsb
        jne     .no
.yes:   mov     eax,1
        pop     edi esi
        ret
.no:    xor     eax,eax
        pop     edi esi
        ret

;;; code rt_str_cmp
rt_str_cmp:                     ; eax = a, edx = b -> eax = -1, 0, 1 (UTF-8 bytes sort like code points)
        push    ebx esi edi
        mov     esi,eax
        mov     edi,edx
        xor     eax,eax
        test    esi,esi
        jz      @f
        mov     eax,[esi+8]
        add     esi,12
@@:     xor     edx,edx
        test    edi,edi
        jz      @f
        mov     edx,[edi+8]
        add     edi,12
@@:     mov     ebx,eax
        mov     ecx,eax
        cmp     ecx,edx
        jbe     @f
        mov     ecx,edx
@@:     test    ecx,ecx
        jz      .len
        repe    cmpsb
        ja      .gt
        jb      .lt
.len:   cmp     ebx,edx
        ja      .gt
        jb      .lt
        xor     eax,eax
        jmp     .out
.gt:    mov     eax,1
        jmp     .out
.lt:    or      eax,-1
.out:   pop     edi esi ebx
        ret

;;; data rt_chars : rt_static
align 4
rt_chars:                       ; every ASCII one-character string, static (32 bytes apart)
repeat 128
        dd      0x40000000, rt_static, 1
        db      %-1, 0
        dd      1
        db      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
end repeat

;;; code rt_u8_next
rt_u8_next:                     ; esi = position, edi = end -> eax = the code point there, esi past it (rt_bmode: the byte there)
        movzx   eax,byte [esi]
        inc     esi
        cmp     al,0xC0
        jb      .out            ; ASCII, or a stray continuation byte
        cmp     byte [rt_bmode],0
        jne     .out
        cmp     al,0xF8
        jae     .out
        push    ebx ecx edx
        mov     ecx,1           ; continuation bytes
        mov     edx,0x1F
        cmp     al,0xE0
        jb      @f
        inc     ecx
        mov     edx,0x0F
        cmp     al,0xF0
        jb      @f
        inc     ecx
        mov     edx,0x07
@@:     mov     ebx,edi
        sub     ebx,esi
        cmp     ebx,ecx
        jb      .one            ; cut short by the end
        and     edx,eax         ; the bits of the first byte
        mov     ebx,esi
.cont:  movzx   eax,byte [ebx]
        xor     al,0x80
        test    al,0xC0
        jnz     .one            ; not a continuation byte
        shl     edx,6
        or      edx,eax
        inc     ebx
        dec     ecx
        jnz     .cont
        mov     esi,ebx
        mov     eax,edx
        pop     edx ecx ebx
.out:   ret
.one:   movzx   eax,byte [esi-1]    ; an invalid sequence: its first byte alone
        pop     edx ecx ebx
        ret
;;; bss rt_u8_next
rt_bmode        rd 1            ; 1 while a bytes method runs a str routine: bytes, ASCII whitespace

;;; code rt_u8_put
rt_u8_put:                      ; eax = code point, edi = where -> its UTF-8 there, edi past it
        cmp     eax,0x80
        jae     @f
        stosb
        ret
@@:     push    edx
        mov     edx,eax
        cmp     eax,0x800
        jae     @f
        shr     eax,6
        or      al,0xC0
        stosb
        jmp     .last
@@:     cmp     eax,0x10000
        jae     @f
        shr     eax,12
        or      al,0xE0
        stosb
        jmp     .mid
@@:     shr     eax,18
        or      al,0xF0
        stosb
        mov     eax,edx
        shr     eax,12
        and     al,0x3F
        or      al,0x80
        stosb
.mid:   mov     eax,edx
        shr     eax,6
        and     al,0x3F
        or      al,0x80
        stosb
.last:  mov     eax,edx
        and     al,0x3F
        or      al,0x80
        stosb
        pop     edx
        ret

;;; code rt_sb_cp : rt_sb_need rt_u8_put
rt_sb_cp:                       ; eax = code point: append its UTF-8
        push    edi
        push    eax
        mov     eax,4
        call    rt_sb_need
        mov     edi,eax
        pop     eax
        push    edi
        call    rt_u8_put
        pop     eax
        sub     edi,eax
        sub     edi,4
        add     [rt_sb_len],edi ; give back what it did not use
        pop     edi
        ret

;;; code rt_str_cplen : rt_u8_next
rt_str_cplen:                   ; eax = str (0: None) -> eax = its length in code points (nothing else changes)
        test    eax,eax
        jz      .out
        push    ecx
        mov     ecx,[eax+8]
        mov     ecx,[eax+13+ecx]
        cmp     ecx,-1
        je      .count
        mov     eax,ecx
        pop     ecx
.out:   ret
.count: push    ebx esi edi
        push    eax
        mov     ecx,[eax+8]
        lea     esi,[eax+12]
        lea     edi,[esi+ecx]
        xor     ebx,ebx
.l:     cmp     esi,edi
        jae     .done
        inc     ebx
        cmp     byte [esi],0xC0
        jae     @f
        inc     esi
        jmp     .l
@@:     call    rt_u8_next
        jmp     .l
.done:  pop     eax
        mov     ecx,[eax+8]
        mov     [eax+13+ecx],ebx
        mov     eax,ebx
        pop     edi esi ebx
        pop     ecx
        ret

;;; code rt_str_off : rt_str_cplen rt_u8_next rt_str_free
rt_str_off:                     ; eax = str, edx = code point index (0..length) -> eax = its byte offset (ecx changed)
        push    ebx esi edi
        mov     ebx,eax
        call    rt_str_cplen
        cmp     eax,[ebx+8]
        jne     .wide
        mov     eax,edx         ; one byte each
        pop     edi esi ebx
        ret
.wide:  xor     ecx,ecx         ; from code point ecx at byte esi: the start, or where a cursor is
        xor     esi,esi
        mov     edi,rt_str_cur
        cmp     ebx,[edi]
        je      @f
        add     edi,12
        cmp     ebx,[edi]
        jne     .walk
@@:     cmp     edx,[edi+4]
        jb      .walk
        mov     ecx,[edi+4]
        mov     esi,[edi+8]
.walk:  push    edx
        sub     edx,ecx         ; code points to go
        lea     esi,[ebx+12+esi]
        mov     edi,[ebx+8]
        lea     edi,[ebx+12+edi]
        test    edx,edx
        jz      .there
.step:  cmp     byte [esi],0xC0
        jae     .multi
        inc     esi
        dec     edx
        jnz     .step
        jmp     .there
.multi: call    rt_u8_next
        dec     edx
        jnz     .step
.there: pop     edx
        lea     eax,[ebx+12]
        sub     esi,eax
        cmp     ebx,[rt_str_cur]
        je      @f
        mov     eax,[rt_str_cur]        ; the other cursor goes second
        mov     [rt_str_cur+12],eax
        mov     eax,[rt_str_cur+4]
        mov     [rt_str_cur+16],eax
        mov     eax,[rt_str_cur+8]
        mov     [rt_str_cur+20],eax
@@:     mov     [rt_str_cur],ebx
        mov     [rt_str_cur+4],edx
        mov     [rt_str_cur+8],esi
        mov     eax,esi
        pop     edi esi ebx
        ret

;;; code rt_str_cpidx : rt_str_cplen rt_u8_next
rt_str_cpidx:                   ; eax = str, edx = byte offset -> eax = the index of the code point there
        push    ebx esi edi ebp
        mov     ebx,eax
        call    rt_str_cplen
        cmp     eax,[ebx+8]
        mov     eax,edx
        je      .out
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
        lea     ebp,[esi+edx]
        xor     edx,edx
.l:     cmp     esi,ebp
        jae     .done
        call    rt_u8_next
        inc     edx
        jmp     .l
.done:  mov     eax,edx
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_cp_char : rt_chars rt_str_new rt_u8_put rt_alloc rt_free rt_static
rt_cp_char:                     ; eax = code point -> eax = the one-character str (static: shared, never freed)
        cmp     eax,0x80
        jae     .wide
        shl     eax,5
        add     eax,rt_chars
        ret
.wide:  push    ebx esi edi
        mov     ebx,eax
.find:  mov     esi,[rt_cpc_tab]        ; open addressing: (str, code point) pairs
        test    esi,esi
        jz      .grow
        mov     eax,ebx
        imul    eax,eax,0x9E3779B1
        shr     eax,12
        mov     ecx,[rt_cpc_mask]
.probe: and     eax,ecx
        mov     edx,[esi+eax*8]
        test    edx,edx
        jz      .miss
        cmp     [esi+eax*8+4],ebx
        je      .hit
        inc     eax
        jmp     .probe
.hit:   mov     eax,edx
        pop     edi esi ebx
        ret
.miss:  mov     edx,[rt_cpc_n]
        inc     edx
        add     edx,edx
        cmp     edx,ecx
        ja      .grow                   ; half full
        push    eax
        mov     eax,1
        cmp     ebx,0x800
        jb      @f
        inc     eax
        cmp     ebx,0x10000
        jb      @f
        inc     eax
@@:     inc     eax
        call    rt_str_new
        mov     dword [eax],0x40000000
        mov     dword [eax+4],rt_static
        mov     ecx,[eax+8]
        mov     dword [eax+13+ecx],1
        lea     edi,[eax+12]
        push    eax
        mov     eax,ebx
        call    rt_u8_put
        pop     edx
        pop     eax
        mov     [esi+eax*8],edx
        mov     [esi+eax*8+4],ebx
        inc     dword [rt_cpc_n]
        mov     eax,edx
        pop     edi esi ebx
        ret
.grow:  mov     eax,[rt_cpc_mask]       ; twice the slots (64 at first), the pairs moved over
        lea     eax,[eax*2+2]
        cmp     eax,64
        jae     @f
        mov     eax,64
@@:     push    eax
        shl     eax,3
        call    rt_alloc
        pop     ecx
        dec     ecx
        mov     edi,eax
        mov     esi,[rt_cpc_tab]
        mov     [rt_cpc_tab],edi
        xchg    ecx,[rt_cpc_mask]
        test    esi,esi
        jz      .find
        inc     ecx                     ; the old slots
        push    esi
.move:  mov     edx,[esi]
        test    edx,edx
        jz      .moved
        mov     eax,[esi+4]
        imul    eax,eax,0x9E3779B1
        shr     eax,12
.mp:    and     eax,[rt_cpc_mask]
        cmp     dword [edi+eax*8],0
        je      @f
        inc     eax
        jmp     .mp
@@:     mov     [edi+eax*8],edx
        mov     edx,[esi+4]
        mov     [edi+eax*8+4],edx
.moved: add     esi,8
        dec     ecx
        jnz     .move
        pop     eax
        call    rt_free
        jmp     .find
;;; bss rt_cp_char
rt_cpc_tab      rd 1
rt_cpc_mask     rd 1
rt_cpc_n        rd 1

;;; data rt_unicode
; Code point classes and case mappings: tools/gen_unicode.py (src/i_unicode.h has
; the same). Sorted ranges lo, hi, then for the code points at an even and at an
; odd distance from lo: class bits, upper - cp, lower - cp (mod 0x10000). Bits: 1 alpha, 2 digit,
; 4 decimal, 8 numeric, 16 space, 32 upper, 64 lower, 128 cased, 512 case-ignorable,
; 1024 title. Code points in no range: none of them.
align 4
rt_unicode_n    dd 432
rt_unicode:
        dd 0x9,0xD
        dw 16,0,0,16,0,0
        dd 0x1C,0x20
        dw 16,0,0,16,0,0
        dd 0x27,0x27
        dw 512,0,0,512,0,0
        dd 0x2E,0x2E
        dw 512,0,0,512,0,0
        dd 0x30,0x39
        dw 14,0,0,14,0,0
        dd 0x3A,0x3A
        dw 512,0,0,512,0,0
        dd 0x41,0x5A
        dw 161,0,32,161,0,32
        dd 0x5E,0x5E
        dw 512,0,0,512,0,0
        dd 0x60,0x60
        dw 512,0,0,512,0,0
        dd 0x61,0x7A
        dw 193,65504,0,193,65504,0
        dd 0x85,0x85
        dw 16,0,0,16,0,0
        dd 0xA0,0xA0
        dw 16,0,0,16,0,0
        dd 0xA8,0xA8
        dw 512,0,0,512,0,0
        dd 0xAA,0xAA
        dw 193,0,0,193,0,0
        dd 0xAD,0xB0
        dw 512,0,0,0,0,0
        dd 0xB2,0xB3
        dw 10,0,0,10,0,0
        dd 0xB4,0xB4
        dw 512,0,0,512,0,0
        dd 0xB5,0xB5
        dw 193,743,0,193,743,0
        dd 0xB7,0xB8
        dw 512,0,0,512,0,0
        dd 0xB9,0xB9
        dw 10,0,0,10,0,0
        dd 0xBA,0xBA
        dw 193,0,0,193,0,0
        dd 0xBC,0xBE
        dw 8,0,0,8,0,0
        dd 0xC0,0xD6
        dw 161,0,32,161,0,32
        dd 0xD8,0xDE
        dw 161,0,32,161,0,32
        dd 0xDF,0xDF
        dw 193,0,0,193,0,0
        dd 0xE0,0xF6
        dw 193,65504,0,193,65504,0
        dd 0xF8,0xFE
        dw 193,65504,0,193,65504,0
        dd 0xFF,0xFF
        dw 193,121,0,193,121,0
        dd 0x100,0x12F
        dw 161,0,1,193,65535,0
        dd 0x130,0x130
        dw 161,0,0,161,0,0
        dd 0x131,0x131
        dw 193,65304,0,193,65304,0
        dd 0x132,0x137
        dw 161,0,1,193,65535,0
        dd 0x138,0x138
        dw 193,0,0,193,0,0
        dd 0x139,0x148
        dw 161,0,1,193,65535,0
        dd 0x149,0x149
        dw 193,0,0,193,0,0
        dd 0x14A,0x177
        dw 161,0,1,193,65535,0
        dd 0x178,0x178
        dw 161,0,65415,161,0,65415
        dd 0x179,0x17E
        dw 161,0,1,193,65535,0
        dd 0x17F,0x17F
        dw 193,65236,0,193,65236,0
        dd 0x180,0x180
        dw 193,195,0,193,195,0
        dd 0x181,0x181
        dw 161,0,210,161,0,210
        dd 0x182,0x185
        dw 161,0,1,193,65535,0
        dd 0x186,0x186
        dw 161,0,206,161,0,206
        dd 0x187,0x187
        dw 161,0,1,161,0,1
        dd 0x188,0x188
        dw 193,65535,0,193,65535,0
        dd 0x189,0x18A
        dw 161,0,205,161,0,205
        dd 0x18B,0x18B
        dw 161,0,1,161,0,1
        dd 0x18C,0x18C
        dw 193,65535,0,193,65535,0
        dd 0x18D,0x18D
        dw 193,0,0,193,0,0
        dd 0x18E,0x18E
        dw 161,0,79,161,0,79
        dd 0x18F,0x18F
        dw 161,0,202,161,0,202
        dd 0x190,0x190
        dw 161,0,203,161,0,203
        dd 0x191,0x191
        dw 161,0,1,161,0,1
        dd 0x192,0x192
        dw 193,65535,0,193,65535,0
        dd 0x193,0x193
        dw 161,0,205,161,0,205
        dd 0x194,0x194
        dw 161,0,207,161,0,207
        dd 0x195,0x195
        dw 193,97,0,193,97,0
        dd 0x196,0x196
        dw 161,0,211,161,0,211
        dd 0x197,0x197
        dw 161,0,209,161,0,209
        dd 0x198,0x198
        dw 161,0,1,161,0,1
        dd 0x199,0x199
        dw 193,65535,0,193,65535,0
        dd 0x19A,0x19A
        dw 193,163,0,193,163,0
        dd 0x19B,0x19B
        dw 193,42561,0,193,42561,0
        dd 0x19C,0x19C
        dw 161,0,211,161,0,211
        dd 0x19D,0x19D
        dw 161,0,213,161,0,213
        dd 0x19E,0x19E
        dw 193,130,0,193,130,0
        dd 0x19F,0x19F
        dw 161,0,214,161,0,214
        dd 0x1A0,0x1A5
        dw 161,0,1,193,65535,0
        dd 0x1A6,0x1A6
        dw 161,0,218,161,0,218
        dd 0x1A7,0x1A7
        dw 161,0,1,161,0,1
        dd 0x1A8,0x1A8
        dw 193,65535,0,193,65535,0
        dd 0x1A9,0x1A9
        dw 161,0,218,161,0,218
        dd 0x1AA,0x1AB
        dw 193,0,0,193,0,0
        dd 0x1AC,0x1AC
        dw 161,0,1,161,0,1
        dd 0x1AD,0x1AD
        dw 193,65535,0,193,65535,0
        dd 0x1AE,0x1AE
        dw 161,0,218,161,0,218
        dd 0x1AF,0x1AF
        dw 161,0,1,161,0,1
        dd 0x1B0,0x1B0
        dw 193,65535,0,193,65535,0
        dd 0x1B1,0x1B2
        dw 161,0,217,161,0,217
        dd 0x1B3,0x1B6
        dw 161,0,1,193,65535,0
        dd 0x1B7,0x1B7
        dw 161,0,219,161,0,219
        dd 0x1B8,0x1B8
        dw 161,0,1,161,0,1
        dd 0x1B9,0x1B9
        dw 193,65535,0,193,65535,0
        dd 0x1BA,0x1BA
        dw 193,0,0,193,0,0
        dd 0x1BB,0x1BB
        dw 1,0,0,1,0,0
        dd 0x1BC,0x1BC
        dw 161,0,1,161,0,1
        dd 0x1BD,0x1BD
        dw 193,65535,0,193,65535,0
        dd 0x1BE,0x1BE
        dw 193,0,0,193,0,0
        dd 0x1BF,0x1BF
        dw 193,56,0,193,56,0
        dd 0x1C0,0x1C3
        dw 1,0,0,1,0,0
        dd 0x1C4,0x1C4
        dw 161,0,2,161,0,2
        dd 0x1C5,0x1C5
        dw 1153,65535,1,1153,65535,1
        dd 0x1C6,0x1C6
        dw 193,65534,0,193,65534,0
        dd 0x1C7,0x1C7
        dw 161,0,2,161,0,2
        dd 0x1C8,0x1C8
        dw 1153,65535,1,1153,65535,1
        dd 0x1C9,0x1C9
        dw 193,65534,0,193,65534,0
        dd 0x1CA,0x1CA
        dw 161,0,2,161,0,2
        dd 0x1CB,0x1CB
        dw 1153,65535,1,1153,65535,1
        dd 0x1CC,0x1CC
        dw 193,65534,0,193,65534,0
        dd 0x1CD,0x1DC
        dw 161,0,1,193,65535,0
        dd 0x1DD,0x1DD
        dw 193,65457,0,193,65457,0
        dd 0x1DE,0x1EF
        dw 161,0,1,193,65535,0
        dd 0x1F0,0x1F0
        dw 193,0,0,193,0,0
        dd 0x1F1,0x1F1
        dw 161,0,2,161,0,2
        dd 0x1F2,0x1F2
        dw 1153,65535,1,1153,65535,1
        dd 0x1F3,0x1F3
        dw 193,65534,0,193,65534,0
        dd 0x1F4,0x1F4
        dw 161,0,1,161,0,1
        dd 0x1F5,0x1F5
        dw 193,65535,0,193,65535,0
        dd 0x1F6,0x1F6
        dw 161,0,65439,161,0,65439
        dd 0x1F7,0x1F7
        dw 161,0,65480,161,0,65480
        dd 0x1F8,0x21F
        dw 161,0,1,193,65535,0
        dd 0x220,0x220
        dw 161,0,65406,161,0,65406
        dd 0x221,0x221
        dw 193,0,0,193,0,0
        dd 0x222,0x233
        dw 161,0,1,193,65535,0
        dd 0x234,0x239
        dw 193,0,0,193,0,0
        dd 0x23A,0x23A
        dw 161,0,10795,161,0,10795
        dd 0x23B,0x23B
        dw 161,0,1,161,0,1
        dd 0x23C,0x23C
        dw 193,65535,0,193,65535,0
        dd 0x23D,0x23D
        dw 161,0,65373,161,0,65373
        dd 0x23E,0x23E
        dw 161,0,10792,161,0,10792
        dd 0x23F,0x240
        dw 193,10815,0,193,10815,0
        dd 0x241,0x241
        dw 161,0,1,161,0,1
        dd 0x242,0x242
        dw 193,65535,0,193,65535,0
        dd 0x243,0x243
        dw 161,0,65341,161,0,65341
        dd 0x244,0x244
        dw 161,0,69,161,0,69
        dd 0x245,0x245
        dw 161,0,71,161,0,71
        dd 0x246,0x24F
        dw 161,0,1,193,65535,0
        dd 0x250,0x250
        dw 193,10783,0,193,10783,0
        dd 0x251,0x251
        dw 193,10780,0,193,10780,0
        dd 0x252,0x252
        dw 193,10782,0,193,10782,0
        dd 0x253,0x253
        dw 193,65326,0,193,65326,0
        dd 0x254,0x254
        dw 193,65330,0,193,65330,0
        dd 0x255,0x255
        dw 193,0,0,193,0,0
        dd 0x256,0x257
        dw 193,65331,0,193,65331,0
        dd 0x258,0x258
        dw 193,0,0,193,0,0
        dd 0x259,0x259
        dw 193,65334,0,193,65334,0
        dd 0x25A,0x25A
        dw 193,0,0,193,0,0
        dd 0x25B,0x25B
        dw 193,65333,0,193,65333,0
        dd 0x25C,0x25C
        dw 193,42319,0,193,42319,0
        dd 0x25D,0x25F
        dw 193,0,0,193,0,0
        dd 0x260,0x260
        dw 193,65331,0,193,65331,0
        dd 0x261,0x261
        dw 193,42315,0,193,42315,0
        dd 0x262,0x262
        dw 193,0,0,193,0,0
        dd 0x263,0x263
        dw 193,65329,0,193,65329,0
        dd 0x264,0x264
        dw 193,42343,0,193,42343,0
        dd 0x265,0x265
        dw 193,42280,0,193,42280,0
        dd 0x266,0x266
        dw 193,42308,0,193,42308,0
        dd 0x267,0x267
        dw 193,0,0,193,0,0
        dd 0x268,0x268
        dw 193,65327,0,193,65327,0
        dd 0x269,0x269
        dw 193,65325,0,193,65325,0
        dd 0x26A,0x26A
        dw 193,42308,0,193,42308,0
        dd 0x26B,0x26B
        dw 193,10743,0,193,10743,0
        dd 0x26C,0x26C
        dw 193,42305,0,193,42305,0
        dd 0x26D,0x26E
        dw 193,0,0,193,0,0
        dd 0x26F,0x26F
        dw 193,65325,0,193,65325,0
        dd 0x270,0x270
        dw 193,0,0,193,0,0
        dd 0x271,0x271
        dw 193,10749,0,193,10749,0
        dd 0x272,0x272
        dw 193,65323,0,193,65323,0
        dd 0x273,0x274
        dw 193,0,0,193,0,0
        dd 0x275,0x275
        dw 193,65322,0,193,65322,0
        dd 0x276,0x27C
        dw 193,0,0,193,0,0
        dd 0x27D,0x27D
        dw 193,10727,0,193,10727,0
        dd 0x27E,0x27F
        dw 193,0,0,193,0,0
        dd 0x280,0x280
        dw 193,65318,0,193,65318,0
        dd 0x281,0x281
        dw 193,0,0,193,0,0
        dd 0x282,0x282
        dw 193,42307,0,193,42307,0
        dd 0x283,0x283
        dw 193,65318,0,193,65318,0
        dd 0x284,0x286
        dw 193,0,0,193,0,0
        dd 0x287,0x287
        dw 193,42282,0,193,42282,0
        dd 0x288,0x288
        dw 193,65318,0,193,65318,0
        dd 0x289,0x289
        dw 193,65467,0,193,65467,0
        dd 0x28A,0x28B
        dw 193,65319,0,193,65319,0
        dd 0x28C,0x28C
        dw 193,65465,0,193,65465,0
        dd 0x28D,0x291
        dw 193,0,0,193,0,0
        dd 0x292,0x292
        dw 193,65317,0,193,65317,0
        dd 0x293,0x293
        dw 193,0,0,193,0,0
        dd 0x294,0x294
        dw 1,0,0,1,0,0
        dd 0x295,0x29C
        dw 193,0,0,193,0,0
        dd 0x29D,0x29D
        dw 193,42261,0,193,42261,0
        dd 0x29E,0x29E
        dw 193,42258,0,193,42258,0
        dd 0x29F,0x2AF
        dw 193,0,0,193,0,0
        dd 0x2B0,0x2B8
        dw 705,0,0,705,0,0
        dd 0x2B9,0x2BF
        dw 513,0,0,513,0,0
        dd 0x2C0,0x2C1
        dw 705,0,0,705,0,0
        dd 0x2C2,0x2C5
        dw 512,0,0,512,0,0
        dd 0x2C6,0x2D1
        dw 513,0,0,513,0,0
        dd 0x2D2,0x2DF
        dw 512,0,0,512,0,0
        dd 0x2E0,0x2E4
        dw 705,0,0,705,0,0
        dd 0x2E5,0x2EB
        dw 512,0,0,512,0,0
        dd 0x2EC,0x2EF
        dw 513,0,0,512,0,0
        dd 0x2F0,0x344
        dw 512,0,0,512,0,0
        dd 0x345,0x345
        dw 704,84,0,704,84,0
        dd 0x346,0x36F
        dw 512,0,0,512,0,0
        dd 0x370,0x373
        dw 161,0,1,193,65535,0
        dd 0x374,0x374
        dw 513,0,0,513,0,0
        dd 0x375,0x375
        dw 512,0,0,512,0,0
        dd 0x376,0x376
        dw 161,0,1,161,0,1
        dd 0x377,0x377
        dw 193,65535,0,193,65535,0
        dd 0x37A,0x37A
        dw 705,0,0,705,0,0
        dd 0x37B,0x37D
        dw 193,130,0,193,130,0
        dd 0x37F,0x37F
        dw 161,0,116,161,0,116
        dd 0x384,0x385
        dw 512,0,0,512,0,0
        dd 0x386,0x386
        dw 161,0,38,161,0,38
        dd 0x387,0x387
        dw 512,0,0,512,0,0
        dd 0x388,0x38A
        dw 161,0,37,161,0,37
        dd 0x38C,0x38C
        dw 161,0,64,161,0,64
        dd 0x38E,0x38F
        dw 161,0,63,161,0,63
        dd 0x390,0x390
        dw 193,0,0,193,0,0
        dd 0x391,0x3A1
        dw 161,0,32,161,0,32
        dd 0x3A3,0x3AB
        dw 161,0,32,161,0,32
        dd 0x3AC,0x3AC
        dw 193,65498,0,193,65498,0
        dd 0x3AD,0x3AF
        dw 193,65499,0,193,65499,0
        dd 0x3B0,0x3B0
        dw 193,0,0,193,0,0
        dd 0x3B1,0x3C1
        dw 193,65504,0,193,65504,0
        dd 0x3C2,0x3C2
        dw 193,65505,0,193,65505,0
        dd 0x3C3,0x3CB
        dw 193,65504,0,193,65504,0
        dd 0x3CC,0x3CC
        dw 193,65472,0,193,65472,0
        dd 0x3CD,0x3CE
        dw 193,65473,0,193,65473,0
        dd 0x3CF,0x3CF
        dw 161,0,8,161,0,8
        dd 0x3D0,0x3D0
        dw 193,65474,0,193,65474,0
        dd 0x3D1,0x3D1
        dw 193,65479,0,193,65479,0
        dd 0x3D2,0x3D4
        dw 161,0,0,161,0,0
        dd 0x3D5,0x3D5
        dw 193,65489,0,193,65489,0
        dd 0x3D6,0x3D6
        dw 193,65482,0,193,65482,0
        dd 0x3D7,0x3D7
        dw 193,65528,0,193,65528,0
        dd 0x3D8,0x3EF
        dw 161,0,1,193,65535,0
        dd 0x3F0,0x3F0
        dw 193,65450,0,193,65450,0
        dd 0x3F1,0x3F1
        dw 193,65456,0,193,65456,0
        dd 0x3F2,0x3F2
        dw 193,7,0,193,7,0
        dd 0x3F3,0x3F3
        dw 193,65420,0,193,65420,0
        dd 0x3F4,0x3F4
        dw 161,0,65476,161,0,65476
        dd 0x3F5,0x3F5
        dw 193,65440,0,193,65440,0
        dd 0x3F7,0x3F7
        dw 161,0,1,161,0,1
        dd 0x3F8,0x3F8
        dw 193,65535,0,193,65535,0
        dd 0x3F9,0x3F9
        dw 161,0,65529,161,0,65529
        dd 0x3FA,0x3FA
        dw 161,0,1,161,0,1
        dd 0x3FB,0x3FB
        dw 193,65535,0,193,65535,0
        dd 0x3FC,0x3FC
        dw 193,0,0,193,0,0
        dd 0x3FD,0x3FF
        dw 161,0,65406,161,0,65406
        dd 0x400,0x40F
        dw 161,0,80,161,0,80
        dd 0x410,0x42F
        dw 161,0,32,161,0,32
        dd 0x430,0x44F
        dw 193,65504,0,193,65504,0
        dd 0x450,0x45F
        dw 193,65456,0,193,65456,0
        dd 0x460,0x481
        dw 161,0,1,193,65535,0
        dd 0x483,0x489
        dw 512,0,0,512,0,0
        dd 0x48A,0x4BF
        dw 161,0,1,193,65535,0
        dd 0x4C0,0x4C0
        dw 161,0,15,161,0,15
        dd 0x4C1,0x4CE
        dw 161,0,1,193,65535,0
        dd 0x4CF,0x4CF
        dw 193,65521,0,193,65521,0
        dd 0x4D0,0x52F
        dw 161,0,1,193,65535,0
        dd 0x531,0x556
        dw 161,0,48,161,0,48
        dd 0x559,0x559
        dw 513,0,0,513,0,0
        dd 0x55F,0x55F
        dw 512,0,0,512,0,0
        dd 0x560,0x560
        dw 193,0,0,193,0,0
        dd 0x561,0x586
        dw 193,65488,0,193,65488,0
        dd 0x587,0x588
        dw 193,0,0,193,0,0
        dd 0x591,0x5BD
        dw 512,0,0,512,0,0
        dd 0x5BF,0x5BF
        dw 512,0,0,512,0,0
        dd 0x5C1,0x5C2
        dw 512,0,0,512,0,0
        dd 0x5C4,0x5C5
        dw 512,0,0,512,0,0
        dd 0x5C7,0x5C7
        dw 512,0,0,512,0,0
        dd 0x5D0,0x5EA
        dw 1,0,0,1,0,0
        dd 0x5EF,0x5F2
        dw 1,0,0,1,0,0
        dd 0x5F4,0x5F4
        dw 512,0,0,512,0,0
        dd 0x600,0x605
        dw 512,0,0,512,0,0
        dd 0x610,0x61A
        dw 512,0,0,512,0,0
        dd 0x61C,0x61C
        dw 512,0,0,512,0,0
        dd 0x620,0x63F
        dw 1,0,0,1,0,0
        dd 0x640,0x640
        dw 513,0,0,513,0,0
        dd 0x641,0x64A
        dw 1,0,0,1,0,0
        dd 0x64B,0x65F
        dw 512,0,0,512,0,0
        dd 0x660,0x669
        dw 14,0,0,14,0,0
        dd 0x66E,0x66F
        dw 1,0,0,1,0,0
        dd 0x670,0x670
        dw 512,0,0,512,0,0
        dd 0x671,0x6D3
        dw 1,0,0,1,0,0
        dd 0x6D5,0x6D5
        dw 1,0,0,1,0,0
        dd 0x6D6,0x6DD
        dw 512,0,0,512,0,0
        dd 0x6DF,0x6E4
        dw 512,0,0,512,0,0
        dd 0x6E5,0x6E6
        dw 513,0,0,513,0,0
        dd 0x6E7,0x6E8
        dw 512,0,0,512,0,0
        dd 0x6EA,0x6ED
        dw 512,0,0,512,0,0
        dd 0x6EE,0x6EF
        dw 1,0,0,1,0,0
        dd 0x6F0,0x6F9
        dw 14,0,0,14,0,0
        dd 0x6FA,0x6FC
        dw 1,0,0,1,0,0
        dd 0x6FF,0x6FF
        dw 1,0,0,1,0,0
        dd 0x1680,0x1680
        dw 16,0,0,16,0,0
        dd 0x2000,0x200A
        dw 16,0,0,16,0,0
        dd 0x200B,0x200F
        dw 512,0,0,512,0,0
        dd 0x2018,0x2019
        dw 512,0,0,512,0,0
        dd 0x2024,0x2024
        dw 512,0,0,512,0,0
        dd 0x2027,0x2027
        dw 512,0,0,512,0,0
        dd 0x2028,0x2029
        dw 16,0,0,16,0,0
        dd 0x202A,0x202E
        dw 512,0,0,512,0,0
        dd 0x202F,0x202F
        dw 16,0,0,16,0,0
        dd 0x205F,0x205F
        dw 16,0,0,16,0,0
        dd 0x2060,0x2064
        dw 512,0,0,512,0,0
        dd 0x2066,0x206F
        dw 512,0,0,512,0,0
        dd 0x2150,0x215F
        dw 8,0,0,8,0,0
        dd 0x2160,0x216F
        dw 168,0,16,168,0,16
        dd 0x2170,0x217F
        dw 200,65520,0,200,65520,0
        dd 0x2180,0x2182
        dw 8,0,0,8,0,0
        dd 0x2183,0x2183
        dw 161,0,1,161,0,1
        dd 0x2184,0x2184
        dw 193,65535,0,193,65535,0
        dd 0x2185,0x2189
        dw 8,0,0,8,0,0
        dd 0x3000,0x3000
        dw 16,0,0,16,0,0
        dd 0x3005,0x3005
        dw 513,0,0,513,0,0
        dd 0x3006,0x3006
        dw 1,0,0,1,0,0
        dd 0x3007,0x3007
        dw 8,0,0,8,0,0
        dd 0x3021,0x3029
        dw 8,0,0,8,0,0
        dd 0x302A,0x302D
        dw 512,0,0,512,0,0
        dd 0x3031,0x3035
        dw 513,0,0,513,0,0
        dd 0x3038,0x303A
        dw 8,0,0,8,0,0
        dd 0x303B,0x303B
        dw 513,0,0,513,0,0
        dd 0x303C,0x303C
        dw 1,0,0,1,0,0
        dd 0x3041,0x3096
        dw 1,0,0,1,0,0
        dd 0x3099,0x309C
        dw 512,0,0,512,0,0
        dd 0x309D,0x309E
        dw 513,0,0,513,0,0
        dd 0x309F,0x309F
        dw 1,0,0,1,0,0
        dd 0x30A1,0x30FA
        dw 1,0,0,1,0,0
        dd 0x30FC,0x30FE
        dw 513,0,0,513,0,0
        dd 0x30FF,0x30FF
        dw 1,0,0,1,0,0
        dd 0x4E00,0x4E00
        dw 9,0,0,9,0,0
        dd 0x4E01,0x4E02
        dw 1,0,0,1,0,0
        dd 0x4E03,0x4E03
        dw 9,0,0,9,0,0
        dd 0x4E04,0x4E06
        dw 1,0,0,1,0,0
        dd 0x4E07,0x4E0A
        dw 9,0,0,1,0,0
        dd 0x4E0B,0x4E23
        dw 1,0,0,1,0,0
        dd 0x4E24,0x4E24
        dw 9,0,0,9,0,0
        dd 0x4E25,0x4E5C
        dw 1,0,0,1,0,0
        dd 0x4E5D,0x4E5D
        dw 9,0,0,9,0,0
        dd 0x4E5E,0x4E8B
        dw 1,0,0,1,0,0
        dd 0x4E8C,0x4E8C
        dw 9,0,0,9,0,0
        dd 0x4E8D,0x4E93
        dw 1,0,0,1,0,0
        dd 0x4E94,0x4E97
        dw 9,0,0,1,0,0
        dd 0x4E98,0x4EAB
        dw 1,0,0,1,0,0
        dd 0x4EAC,0x4EAC
        dw 9,0,0,9,0,0
        dd 0x4EAD,0x4EBE
        dw 1,0,0,1,0,0
        dd 0x4EBF,0x4EC0
        dw 9,0,0,9,0,0
        dd 0x4EC1,0x4EDE
        dw 1,0,0,1,0,0
        dd 0x4EDF,0x4EDF
        dw 9,0,0,9,0,0
        dd 0x4EE0,0x4EE7
        dw 1,0,0,1,0,0
        dd 0x4EE8,0x4EE8
        dw 9,0,0,9,0,0
        dd 0x4EE9,0x4F0C
        dw 1,0,0,1,0,0
        dd 0x4F0D,0x4F0D
        dw 9,0,0,9,0,0
        dd 0x4F0E,0x4F6F
        dw 1,0,0,1,0,0
        dd 0x4F70,0x4F70
        dw 9,0,0,9,0,0
        dd 0x4F71,0x4FE8
        dw 1,0,0,1,0,0
        dd 0x4FE9,0x4FE9
        dw 9,0,0,9,0,0
        dd 0x4FEA,0x5005
        dw 1,0,0,1,0,0
        dd 0x5006,0x5006
        dw 9,0,0,9,0,0
        dd 0x5007,0x5103
        dw 1,0,0,1,0,0
        dd 0x5104,0x5104
        dw 9,0,0,9,0,0
        dd 0x5105,0x5145
        dw 1,0,0,1,0,0
        dd 0x5146,0x5146
        dw 9,0,0,9,0,0
        dd 0x5147,0x5168
        dw 1,0,0,1,0,0
        dd 0x5169,0x516E
        dw 9,0,0,1,0,0
        dd 0x516F,0x5340
        dw 1,0,0,1,0,0
        dd 0x5341,0x5341
        dw 9,0,0,9,0,0
        dd 0x5342,0x5342
        dw 1,0,0,1,0,0
        dd 0x5343,0x5345
        dw 9,0,0,9,0,0
        dd 0x5346,0x534B
        dw 1,0,0,1,0,0
        dd 0x534C,0x534C
        dw 9,0,0,9,0,0
        dd 0x534D,0x53C0
        dw 1,0,0,1,0,0
        dd 0x53C1,0x53C4
        dw 9,0,0,9,0,0
        dd 0x53C5,0x56DA
        dw 1,0,0,1,0,0
        dd 0x56DB,0x56DB
        dw 9,0,0,9,0,0
        dd 0x56DC,0x58F0
        dw 1,0,0,1,0,0
        dd 0x58F1,0x58F1
        dw 9,0,0,9,0,0
        dd 0x58F2,0x58F8
        dw 1,0,0,1,0,0
        dd 0x58F9,0x58F9
        dw 9,0,0,9,0,0
        dd 0x58FA,0x5E79
        dw 1,0,0,1,0,0
        dd 0x5E7A,0x5E7A
        dw 9,0,0,9,0,0
        dd 0x5E7B,0x5EFD
        dw 1,0,0,1,0,0
        dd 0x5EFE,0x5EFF
        dw 9,0,0,9,0,0
        dd 0x5F00,0x5F0B
        dw 1,0,0,1,0,0
        dd 0x5F0C,0x5F0E
        dw 9,0,0,9,0,0
        dd 0x5F0F,0x5F0F
        dw 1,0,0,1,0,0
        dd 0x5F10,0x5F10
        dw 9,0,0,9,0,0
        dd 0x5F11,0x62CF
        dw 1,0,0,1,0,0
        dd 0x62D0,0x62D0
        dw 9,0,0,9,0,0
        dd 0x62D1,0x62FD
        dw 1,0,0,1,0,0
        dd 0x62FE,0x62FE
        dw 9,0,0,9,0,0
        dd 0x62FF,0x634B
        dw 1,0,0,1,0,0
        dd 0x634C,0x634C
        dw 9,0,0,9,0,0
        dd 0x634D,0x67D1
        dw 1,0,0,1,0,0
        dd 0x67D2,0x67D2
        dw 9,0,0,9,0,0
        dd 0x67D3,0x6D1D
        dw 1,0,0,1,0,0
        dd 0x6D1E,0x6D1E
        dw 9,0,0,9,0,0
        dd 0x6D1F,0x6F05
        dw 1,0,0,1,0,0
        dd 0x6F06,0x6F06
        dw 9,0,0,9,0,0
        dd 0x6F07,0x7395
        dw 1,0,0,1,0,0
        dd 0x7396,0x7396
        dw 9,0,0,9,0,0
        dd 0x7397,0x767D
        dw 1,0,0,1,0,0
        dd 0x767E,0x767E
        dw 9,0,0,9,0,0
        dd 0x767F,0x7694
        dw 1,0,0,1,0,0
        dd 0x7695,0x7695
        dw 9,0,0,9,0,0
        dd 0x7696,0x79EC
        dw 1,0,0,1,0,0
        dd 0x79ED,0x79ED
        dw 9,0,0,9,0,0
        dd 0x79EE,0x8085
        dw 1,0,0,1,0,0
        dd 0x8086,0x8086
        dw 9,0,0,9,0,0
        dd 0x8087,0x842B
        dw 1,0,0,1,0,0
        dd 0x842C,0x842C
        dw 9,0,0,9,0,0
        dd 0x842D,0x8CAD
        dw 1,0,0,1,0,0
        dd 0x8CAE,0x8CAE
        dw 9,0,0,9,0,0
        dd 0x8CAF,0x8CB2
        dw 1,0,0,1,0,0
        dd 0x8CB3,0x8CB3
        dw 9,0,0,9,0,0
        dd 0x8CB4,0x8D2F
        dw 1,0,0,1,0,0
        dd 0x8D30,0x8D30
        dw 9,0,0,9,0,0
        dd 0x8D31,0x920D
        dw 1,0,0,1,0,0
        dd 0x920E,0x920E
        dw 9,0,0,9,0,0
        dd 0x920F,0x94A8
        dw 1,0,0,1,0,0
        dd 0x94A9,0x94A9
        dw 9,0,0,9,0,0
        dd 0x94AA,0x9620
        dw 1,0,0,1,0,0
        dd 0x9621,0x9621
        dw 9,0,0,9,0,0
        dd 0x9622,0x9645
        dw 1,0,0,1,0,0
        dd 0x9646,0x9646
        dw 9,0,0,9,0,0
        dd 0x9647,0x964B
        dw 1,0,0,1,0,0
        dd 0x964C,0x964C
        dw 9,0,0,9,0,0
        dd 0x964D,0x9677
        dw 1,0,0,1,0,0
        dd 0x9678,0x9678
        dw 9,0,0,9,0,0
        dd 0x9679,0x96F5
        dw 1,0,0,1,0,0
        dd 0x96F6,0x96F6
        dw 9,0,0,9,0,0
        dd 0x96F7,0x9FFF
        dw 1,0,0,1,0,0
        dd 0xAC00,0xD7A3
        dw 1,0,0,1,0,0
        dd 0xFF07,0xFF07
        dw 512,0,0,512,0,0
        dd 0xFF0E,0xFF0E
        dw 512,0,0,512,0,0
        dd 0xFF10,0xFF19
        dw 14,0,0,14,0,0
        dd 0xFF1A,0xFF1A
        dw 512,0,0,512,0,0
        dd 0xFF21,0xFF3A
        dw 161,0,32,161,0,32
        dd 0xFF3E,0xFF3E
        dw 512,0,0,512,0,0
        dd 0xFF40,0xFF40
        dw 512,0,0,512,0,0
        dd 0xFF41,0xFF5A
        dw 193,65504,0,193,65504,0

;;; data rt_noprint
; the code points from 0x80 on that repr() escapes: sorted ranges lo, hi
align 4
rt_noprint_n    dd 736
rt_noprint:
        dd 0x80,0xA0
        dd 0xAD,0xAD
        dd 0x378,0x379
        dd 0x380,0x383
        dd 0x38B,0x38B
        dd 0x38D,0x38D
        dd 0x3A2,0x3A2
        dd 0x530,0x530
        dd 0x557,0x558
        dd 0x58B,0x58C
        dd 0x590,0x590
        dd 0x5C8,0x5CF
        dd 0x5EB,0x5EE
        dd 0x5F5,0x605
        dd 0x61C,0x61C
        dd 0x6DD,0x6DD
        dd 0x70E,0x70F
        dd 0x74B,0x74C
        dd 0x7B2,0x7BF
        dd 0x7FB,0x7FC
        dd 0x82E,0x82F
        dd 0x83F,0x83F
        dd 0x85C,0x85D
        dd 0x85F,0x85F
        dd 0x86B,0x86F
        dd 0x88F,0x896
        dd 0x8E2,0x8E2
        dd 0x984,0x984
        dd 0x98D,0x98E
        dd 0x991,0x992
        dd 0x9A9,0x9A9
        dd 0x9B1,0x9B1
        dd 0x9B3,0x9B5
        dd 0x9BA,0x9BB
        dd 0x9C5,0x9C6
        dd 0x9C9,0x9CA
        dd 0x9CF,0x9D6
        dd 0x9D8,0x9DB
        dd 0x9DE,0x9DE
        dd 0x9E4,0x9E5
        dd 0x9FF,0xA00
        dd 0xA04,0xA04
        dd 0xA0B,0xA0E
        dd 0xA11,0xA12
        dd 0xA29,0xA29
        dd 0xA31,0xA31
        dd 0xA34,0xA34
        dd 0xA37,0xA37
        dd 0xA3A,0xA3B
        dd 0xA3D,0xA3D
        dd 0xA43,0xA46
        dd 0xA49,0xA4A
        dd 0xA4E,0xA50
        dd 0xA52,0xA58
        dd 0xA5D,0xA5D
        dd 0xA5F,0xA65
        dd 0xA77,0xA80
        dd 0xA84,0xA84
        dd 0xA8E,0xA8E
        dd 0xA92,0xA92
        dd 0xAA9,0xAA9
        dd 0xAB1,0xAB1
        dd 0xAB4,0xAB4
        dd 0xABA,0xABB
        dd 0xAC6,0xAC6
        dd 0xACA,0xACA
        dd 0xACE,0xACF
        dd 0xAD1,0xADF
        dd 0xAE4,0xAE5
        dd 0xAF2,0xAF8
        dd 0xB00,0xB00
        dd 0xB04,0xB04
        dd 0xB0D,0xB0E
        dd 0xB11,0xB12
        dd 0xB29,0xB29
        dd 0xB31,0xB31
        dd 0xB34,0xB34
        dd 0xB3A,0xB3B
        dd 0xB45,0xB46
        dd 0xB49,0xB4A
        dd 0xB4E,0xB54
        dd 0xB58,0xB5B
        dd 0xB5E,0xB5E
        dd 0xB64,0xB65
        dd 0xB78,0xB81
        dd 0xB84,0xB84
        dd 0xB8B,0xB8D
        dd 0xB91,0xB91
        dd 0xB96,0xB98
        dd 0xB9B,0xB9B
        dd 0xB9D,0xB9D
        dd 0xBA0,0xBA2
        dd 0xBA5,0xBA7
        dd 0xBAB,0xBAD
        dd 0xBBA,0xBBD
        dd 0xBC3,0xBC5
        dd 0xBC9,0xBC9
        dd 0xBCE,0xBCF
        dd 0xBD1,0xBD6
        dd 0xBD8,0xBE5
        dd 0xBFB,0xBFF
        dd 0xC0D,0xC0D
        dd 0xC11,0xC11
        dd 0xC29,0xC29
        dd 0xC3A,0xC3B
        dd 0xC45,0xC45
        dd 0xC49,0xC49
        dd 0xC4E,0xC54
        dd 0xC57,0xC57
        dd 0xC5B,0xC5C
        dd 0xC5E,0xC5F
        dd 0xC64,0xC65
        dd 0xC70,0xC76
        dd 0xC8D,0xC8D
        dd 0xC91,0xC91
        dd 0xCA9,0xCA9
        dd 0xCB4,0xCB4
        dd 0xCBA,0xCBB
        dd 0xCC5,0xCC5
        dd 0xCC9,0xCC9
        dd 0xCCE,0xCD4
        dd 0xCD7,0xCDC
        dd 0xCDF,0xCDF
        dd 0xCE4,0xCE5
        dd 0xCF0,0xCF0
        dd 0xCF4,0xCFF
        dd 0xD0D,0xD0D
        dd 0xD11,0xD11
        dd 0xD45,0xD45
        dd 0xD49,0xD49
        dd 0xD50,0xD53
        dd 0xD64,0xD65
        dd 0xD80,0xD80
        dd 0xD84,0xD84
        dd 0xD97,0xD99
        dd 0xDB2,0xDB2
        dd 0xDBC,0xDBC
        dd 0xDBE,0xDBF
        dd 0xDC7,0xDC9
        dd 0xDCB,0xDCE
        dd 0xDD5,0xDD5
        dd 0xDD7,0xDD7
        dd 0xDE0,0xDE5
        dd 0xDF0,0xDF1
        dd 0xDF5,0xE00
        dd 0xE3B,0xE3E
        dd 0xE5C,0xE80
        dd 0xE83,0xE83
        dd 0xE85,0xE85
        dd 0xE8B,0xE8B
        dd 0xEA4,0xEA4
        dd 0xEA6,0xEA6
        dd 0xEBE,0xEBF
        dd 0xEC5,0xEC5
        dd 0xEC7,0xEC7
        dd 0xECF,0xECF
        dd 0xEDA,0xEDB
        dd 0xEE0,0xEFF
        dd 0xF48,0xF48
        dd 0xF6D,0xF70
        dd 0xF98,0xF98
        dd 0xFBD,0xFBD
        dd 0xFCD,0xFCD
        dd 0xFDB,0xFFF
        dd 0x10C6,0x10C6
        dd 0x10C8,0x10CC
        dd 0x10CE,0x10CF
        dd 0x1249,0x1249
        dd 0x124E,0x124F
        dd 0x1257,0x1257
        dd 0x1259,0x1259
        dd 0x125E,0x125F
        dd 0x1289,0x1289
        dd 0x128E,0x128F
        dd 0x12B1,0x12B1
        dd 0x12B6,0x12B7
        dd 0x12BF,0x12BF
        dd 0x12C1,0x12C1
        dd 0x12C6,0x12C7
        dd 0x12D7,0x12D7
        dd 0x1311,0x1311
        dd 0x1316,0x1317
        dd 0x135B,0x135C
        dd 0x137D,0x137F
        dd 0x139A,0x139F
        dd 0x13F6,0x13F7
        dd 0x13FE,0x13FF
        dd 0x1680,0x1680
        dd 0x169D,0x169F
        dd 0x16F9,0x16FF
        dd 0x1716,0x171E
        dd 0x1737,0x173F
        dd 0x1754,0x175F
        dd 0x176D,0x176D
        dd 0x1771,0x1771
        dd 0x1774,0x177F
        dd 0x17DE,0x17DF
        dd 0x17EA,0x17EF
        dd 0x17FA,0x17FF
        dd 0x180E,0x180E
        dd 0x181A,0x181F
        dd 0x1879,0x187F
        dd 0x18AB,0x18AF
        dd 0x18F6,0x18FF
        dd 0x191F,0x191F
        dd 0x192C,0x192F
        dd 0x193C,0x193F
        dd 0x1941,0x1943
        dd 0x196E,0x196F
        dd 0x1975,0x197F
        dd 0x19AC,0x19AF
        dd 0x19CA,0x19CF
        dd 0x19DB,0x19DD
        dd 0x1A1C,0x1A1D
        dd 0x1A5F,0x1A5F
        dd 0x1A7D,0x1A7E
        dd 0x1A8A,0x1A8F
        dd 0x1A9A,0x1A9F
        dd 0x1AAE,0x1AAF
        dd 0x1ACF,0x1AFF
        dd 0x1B4D,0x1B4D
        dd 0x1BF4,0x1BFB
        dd 0x1C38,0x1C3A
        dd 0x1C4A,0x1C4C
        dd 0x1C8B,0x1C8F
        dd 0x1CBB,0x1CBC
        dd 0x1CC8,0x1CCF
        dd 0x1CFB,0x1CFF
        dd 0x1F16,0x1F17
        dd 0x1F1E,0x1F1F
        dd 0x1F46,0x1F47
        dd 0x1F4E,0x1F4F
        dd 0x1F58,0x1F58
        dd 0x1F5A,0x1F5A
        dd 0x1F5C,0x1F5C
        dd 0x1F5E,0x1F5E
        dd 0x1F7E,0x1F7F
        dd 0x1FB5,0x1FB5
        dd 0x1FC5,0x1FC5
        dd 0x1FD4,0x1FD5
        dd 0x1FDC,0x1FDC
        dd 0x1FF0,0x1FF1
        dd 0x1FF5,0x1FF5
        dd 0x1FFF,0x200F
        dd 0x2028,0x202F
        dd 0x205F,0x206F
        dd 0x2072,0x2073
        dd 0x208F,0x208F
        dd 0x209D,0x209F
        dd 0x20C1,0x20CF
        dd 0x20F1,0x20FF
        dd 0x218C,0x218F
        dd 0x242A,0x243F
        dd 0x244B,0x245F
        dd 0x2B74,0x2B75
        dd 0x2B96,0x2B96
        dd 0x2CF4,0x2CF8
        dd 0x2D26,0x2D26
        dd 0x2D28,0x2D2C
        dd 0x2D2E,0x2D2F
        dd 0x2D68,0x2D6E
        dd 0x2D71,0x2D7E
        dd 0x2D97,0x2D9F
        dd 0x2DA7,0x2DA7
        dd 0x2DAF,0x2DAF
        dd 0x2DB7,0x2DB7
        dd 0x2DBF,0x2DBF
        dd 0x2DC7,0x2DC7
        dd 0x2DCF,0x2DCF
        dd 0x2DD7,0x2DD7
        dd 0x2DDF,0x2DDF
        dd 0x2E5E,0x2E7F
        dd 0x2E9A,0x2E9A
        dd 0x2EF4,0x2EFF
        dd 0x2FD6,0x2FEF
        dd 0x3000,0x3000
        dd 0x3040,0x3040
        dd 0x3097,0x3098
        dd 0x3100,0x3104
        dd 0x3130,0x3130
        dd 0x318F,0x318F
        dd 0x31E6,0x31EE
        dd 0x321F,0x321F
        dd 0xA48D,0xA48F
        dd 0xA4C7,0xA4CF
        dd 0xA62C,0xA63F
        dd 0xA6F8,0xA6FF
        dd 0xA7CE,0xA7CF
        dd 0xA7D2,0xA7D2
        dd 0xA7D4,0xA7D4
        dd 0xA7DD,0xA7F1
        dd 0xA82D,0xA82F
        dd 0xA83A,0xA83F
        dd 0xA878,0xA87F
        dd 0xA8C6,0xA8CD
        dd 0xA8DA,0xA8DF
        dd 0xA954,0xA95E
        dd 0xA97D,0xA97F
        dd 0xA9CE,0xA9CE
        dd 0xA9DA,0xA9DD
        dd 0xA9FF,0xA9FF
        dd 0xAA37,0xAA3F
        dd 0xAA4E,0xAA4F
        dd 0xAA5A,0xAA5B
        dd 0xAAC3,0xAADA
        dd 0xAAF7,0xAB00
        dd 0xAB07,0xAB08
        dd 0xAB0F,0xAB10
        dd 0xAB17,0xAB1F
        dd 0xAB27,0xAB27
        dd 0xAB2F,0xAB2F
        dd 0xAB6C,0xAB6F
        dd 0xABEE,0xABEF
        dd 0xABFA,0xABFF
        dd 0xD7A4,0xD7AF
        dd 0xD7C7,0xD7CA
        dd 0xD7FC,0xF8FF
        dd 0xFA6E,0xFA6F
        dd 0xFADA,0xFAFF
        dd 0xFB07,0xFB12
        dd 0xFB18,0xFB1C
        dd 0xFB37,0xFB37
        dd 0xFB3D,0xFB3D
        dd 0xFB3F,0xFB3F
        dd 0xFB42,0xFB42
        dd 0xFB45,0xFB45
        dd 0xFBC3,0xFBD2
        dd 0xFD90,0xFD91
        dd 0xFDC8,0xFDCE
        dd 0xFDD0,0xFDEF
        dd 0xFE1A,0xFE1F
        dd 0xFE53,0xFE53
        dd 0xFE67,0xFE67
        dd 0xFE6C,0xFE6F
        dd 0xFE75,0xFE75
        dd 0xFEFD,0xFF00
        dd 0xFFBF,0xFFC1
        dd 0xFFC8,0xFFC9
        dd 0xFFD0,0xFFD1
        dd 0xFFD8,0xFFD9
        dd 0xFFDD,0xFFDF
        dd 0xFFE7,0xFFE7
        dd 0xFFEF,0xFFFB
        dd 0xFFFE,0xFFFF
        dd 0x1000C,0x1000C
        dd 0x10027,0x10027
        dd 0x1003B,0x1003B
        dd 0x1003E,0x1003E
        dd 0x1004E,0x1004F
        dd 0x1005E,0x1007F
        dd 0x100FB,0x100FF
        dd 0x10103,0x10106
        dd 0x10134,0x10136
        dd 0x1018F,0x1018F
        dd 0x1019D,0x1019F
        dd 0x101A1,0x101CF
        dd 0x101FE,0x1027F
        dd 0x1029D,0x1029F
        dd 0x102D1,0x102DF
        dd 0x102FC,0x102FF
        dd 0x10324,0x1032C
        dd 0x1034B,0x1034F
        dd 0x1037B,0x1037F
        dd 0x1039E,0x1039E
        dd 0x103C4,0x103C7
        dd 0x103D6,0x103FF
        dd 0x1049E,0x1049F
        dd 0x104AA,0x104AF
        dd 0x104D4,0x104D7
        dd 0x104FC,0x104FF
        dd 0x10528,0x1052F
        dd 0x10564,0x1056E
        dd 0x1057B,0x1057B
        dd 0x1058B,0x1058B
        dd 0x10593,0x10593
        dd 0x10596,0x10596
        dd 0x105A2,0x105A2
        dd 0x105B2,0x105B2
        dd 0x105BA,0x105BA
        dd 0x105BD,0x105BF
        dd 0x105F4,0x105FF
        dd 0x10737,0x1073F
        dd 0x10756,0x1075F
        dd 0x10768,0x1077F
        dd 0x10786,0x10786
        dd 0x107B1,0x107B1
        dd 0x107BB,0x107FF
        dd 0x10806,0x10807
        dd 0x10809,0x10809
        dd 0x10836,0x10836
        dd 0x10839,0x1083B
        dd 0x1083D,0x1083E
        dd 0x10856,0x10856
        dd 0x1089F,0x108A6
        dd 0x108B0,0x108DF
        dd 0x108F3,0x108F3
        dd 0x108F6,0x108FA
        dd 0x1091C,0x1091E
        dd 0x1093A,0x1093E
        dd 0x10940,0x1097F
        dd 0x109B8,0x109BB
        dd 0x109D0,0x109D1
        dd 0x10A04,0x10A04
        dd 0x10A07,0x10A0B
        dd 0x10A14,0x10A14
        dd 0x10A18,0x10A18
        dd 0x10A36,0x10A37
        dd 0x10A3B,0x10A3E
        dd 0x10A49,0x10A4F
        dd 0x10A59,0x10A5F
        dd 0x10AA0,0x10ABF
        dd 0x10AE7,0x10AEA
        dd 0x10AF7,0x10AFF
        dd 0x10B36,0x10B38
        dd 0x10B56,0x10B57
        dd 0x10B73,0x10B77
        dd 0x10B92,0x10B98
        dd 0x10B9D,0x10BA8
        dd 0x10BB0,0x10BFF
        dd 0x10C49,0x10C7F
        dd 0x10CB3,0x10CBF
        dd 0x10CF3,0x10CF9
        dd 0x10D28,0x10D2F
        dd 0x10D3A,0x10D3F
        dd 0x10D66,0x10D68
        dd 0x10D86,0x10D8D
        dd 0x10D90,0x10E5F
        dd 0x10E7F,0x10E7F
        dd 0x10EAA,0x10EAA
        dd 0x10EAE,0x10EAF
        dd 0x10EB2,0x10EC1
        dd 0x10EC5,0x10EFB
        dd 0x10F28,0x10F2F
        dd 0x10F5A,0x10F6F
        dd 0x10F8A,0x10FAF
        dd 0x10FCC,0x10FDF
        dd 0x10FF7,0x10FFF
        dd 0x1104E,0x11051
        dd 0x11076,0x1107E
        dd 0x110BD,0x110BD
        dd 0x110C3,0x110CF
        dd 0x110E9,0x110EF
        dd 0x110FA,0x110FF
        dd 0x11135,0x11135
        dd 0x11148,0x1114F
        dd 0x11177,0x1117F
        dd 0x111E0,0x111E0
        dd 0x111F5,0x111FF
        dd 0x11212,0x11212
        dd 0x11242,0x1127F
        dd 0x11287,0x11287
        dd 0x11289,0x11289
        dd 0x1128E,0x1128E
        dd 0x1129E,0x1129E
        dd 0x112AA,0x112AF
        dd 0x112EB,0x112EF
        dd 0x112FA,0x112FF
        dd 0x11304,0x11304
        dd 0x1130D,0x1130E
        dd 0x11311,0x11312
        dd 0x11329,0x11329
        dd 0x11331,0x11331
        dd 0x11334,0x11334
        dd 0x1133A,0x1133A
        dd 0x11345,0x11346
        dd 0x11349,0x1134A
        dd 0x1134E,0x1134F
        dd 0x11351,0x11356
        dd 0x11358,0x1135C
        dd 0x11364,0x11365
        dd 0x1136D,0x1136F
        dd 0x11375,0x1137F
        dd 0x1138A,0x1138A
        dd 0x1138C,0x1138D
        dd 0x1138F,0x1138F
        dd 0x113B6,0x113B6
        dd 0x113C1,0x113C1
        dd 0x113C3,0x113C4
        dd 0x113C6,0x113C6
        dd 0x113CB,0x113CB
        dd 0x113D6,0x113D6
        dd 0x113D9,0x113E0
        dd 0x113E3,0x113FF
        dd 0x1145C,0x1145C
        dd 0x11462,0x1147F
        dd 0x114C8,0x114CF
        dd 0x114DA,0x1157F
        dd 0x115B6,0x115B7
        dd 0x115DE,0x115FF
        dd 0x11645,0x1164F
        dd 0x1165A,0x1165F
        dd 0x1166D,0x1167F
        dd 0x116BA,0x116BF
        dd 0x116CA,0x116CF
        dd 0x116E4,0x116FF
        dd 0x1171B,0x1171C
        dd 0x1172C,0x1172F
        dd 0x11747,0x117FF
        dd 0x1183C,0x1189F
        dd 0x118F3,0x118FE
        dd 0x11907,0x11908
        dd 0x1190A,0x1190B
        dd 0x11914,0x11914
        dd 0x11917,0x11917
        dd 0x11936,0x11936
        dd 0x11939,0x1193A
        dd 0x11947,0x1194F
        dd 0x1195A,0x1199F
        dd 0x119A8,0x119A9
        dd 0x119D8,0x119D9
        dd 0x119E5,0x119FF
        dd 0x11A48,0x11A4F
        dd 0x11AA3,0x11AAF
        dd 0x11AF9,0x11AFF
        dd 0x11B0A,0x11BBF
        dd 0x11BE2,0x11BEF
        dd 0x11BFA,0x11BFF
        dd 0x11C09,0x11C09
        dd 0x11C37,0x11C37
        dd 0x11C46,0x11C4F
        dd 0x11C6D,0x11C6F
        dd 0x11C90,0x11C91
        dd 0x11CA8,0x11CA8
        dd 0x11CB7,0x11CFF
        dd 0x11D07,0x11D07
        dd 0x11D0A,0x11D0A
        dd 0x11D37,0x11D39
        dd 0x11D3B,0x11D3B
        dd 0x11D3E,0x11D3E
        dd 0x11D48,0x11D4F
        dd 0x11D5A,0x11D5F
        dd 0x11D66,0x11D66
        dd 0x11D69,0x11D69
        dd 0x11D8F,0x11D8F
        dd 0x11D92,0x11D92
        dd 0x11D99,0x11D9F
        dd 0x11DAA,0x11EDF
        dd 0x11EF9,0x11EFF
        dd 0x11F11,0x11F11
        dd 0x11F3B,0x11F3D
        dd 0x11F5B,0x11FAF
        dd 0x11FB1,0x11FBF
        dd 0x11FF2,0x11FFE
        dd 0x1239A,0x123FF
        dd 0x1246F,0x1246F
        dd 0x12475,0x1247F
        dd 0x12544,0x12F8F
        dd 0x12FF3,0x12FFF
        dd 0x13430,0x1343F
        dd 0x13456,0x1345F
        dd 0x143FB,0x143FF
        dd 0x14647,0x160FF
        dd 0x1613A,0x167FF
        dd 0x16A39,0x16A3F
        dd 0x16A5F,0x16A5F
        dd 0x16A6A,0x16A6D
        dd 0x16ABF,0x16ABF
        dd 0x16ACA,0x16ACF
        dd 0x16AEE,0x16AEF
        dd 0x16AF6,0x16AFF
        dd 0x16B46,0x16B4F
        dd 0x16B5A,0x16B5A
        dd 0x16B62,0x16B62
        dd 0x16B78,0x16B7C
        dd 0x16B90,0x16D3F
        dd 0x16D7A,0x16E3F
        dd 0x16E9B,0x16EFF
        dd 0x16F4B,0x16F4E
        dd 0x16F88,0x16F8E
        dd 0x16FA0,0x16FDF
        dd 0x16FE5,0x16FEF
        dd 0x16FF2,0x16FFF
        dd 0x187F8,0x187FF
        dd 0x18CD6,0x18CFE
        dd 0x18D09,0x1AFEF
        dd 0x1AFF4,0x1AFF4
        dd 0x1AFFC,0x1AFFC
        dd 0x1AFFF,0x1AFFF
        dd 0x1B123,0x1B131
        dd 0x1B133,0x1B14F
        dd 0x1B153,0x1B154
        dd 0x1B156,0x1B163
        dd 0x1B168,0x1B16F
        dd 0x1B2FC,0x1BBFF
        dd 0x1BC6B,0x1BC6F
        dd 0x1BC7D,0x1BC7F
        dd 0x1BC89,0x1BC8F
        dd 0x1BC9A,0x1BC9B
        dd 0x1BCA0,0x1CBFF
        dd 0x1CCFA,0x1CCFF
        dd 0x1CEB4,0x1CEFF
        dd 0x1CF2E,0x1CF2F
        dd 0x1CF47,0x1CF4F
        dd 0x1CFC4,0x1CFFF
        dd 0x1D0F6,0x1D0FF
        dd 0x1D127,0x1D128
        dd 0x1D173,0x1D17A
        dd 0x1D1EB,0x1D1FF
        dd 0x1D246,0x1D2BF
        dd 0x1D2D4,0x1D2DF
        dd 0x1D2F4,0x1D2FF
        dd 0x1D357,0x1D35F
        dd 0x1D379,0x1D3FF
        dd 0x1D455,0x1D455
        dd 0x1D49D,0x1D49D
        dd 0x1D4A0,0x1D4A1
        dd 0x1D4A3,0x1D4A4
        dd 0x1D4A7,0x1D4A8
        dd 0x1D4AD,0x1D4AD
        dd 0x1D4BA,0x1D4BA
        dd 0x1D4BC,0x1D4BC
        dd 0x1D4C4,0x1D4C4
        dd 0x1D506,0x1D506
        dd 0x1D50B,0x1D50C
        dd 0x1D515,0x1D515
        dd 0x1D51D,0x1D51D
        dd 0x1D53A,0x1D53A
        dd 0x1D53F,0x1D53F
        dd 0x1D545,0x1D545
        dd 0x1D547,0x1D549
        dd 0x1D551,0x1D551
        dd 0x1D6A6,0x1D6A7
        dd 0x1D7CC,0x1D7CD
        dd 0x1DA8C,0x1DA9A
        dd 0x1DAA0,0x1DAA0
        dd 0x1DAB0,0x1DEFF
        dd 0x1DF1F,0x1DF24
        dd 0x1DF2B,0x1DFFF
        dd 0x1E007,0x1E007
        dd 0x1E019,0x1E01A
        dd 0x1E022,0x1E022
        dd 0x1E025,0x1E025
        dd 0x1E02B,0x1E02F
        dd 0x1E06E,0x1E08E
        dd 0x1E090,0x1E0FF
        dd 0x1E12D,0x1E12F
        dd 0x1E13E,0x1E13F
        dd 0x1E14A,0x1E14D
        dd 0x1E150,0x1E28F
        dd 0x1E2AF,0x1E2BF
        dd 0x1E2FA,0x1E2FE
        dd 0x1E300,0x1E4CF
        dd 0x1E4FA,0x1E5CF
        dd 0x1E5FB,0x1E5FE
        dd 0x1E600,0x1E7DF
        dd 0x1E7E7,0x1E7E7
        dd 0x1E7EC,0x1E7EC
        dd 0x1E7EF,0x1E7EF
        dd 0x1E7FF,0x1E7FF
        dd 0x1E8C5,0x1E8C6
        dd 0x1E8D7,0x1E8FF
        dd 0x1E94C,0x1E94F
        dd 0x1E95A,0x1E95D
        dd 0x1E960,0x1EC70
        dd 0x1ECB5,0x1ED00
        dd 0x1ED3E,0x1EDFF
        dd 0x1EE04,0x1EE04
        dd 0x1EE20,0x1EE20
        dd 0x1EE23,0x1EE23
        dd 0x1EE25,0x1EE26
        dd 0x1EE28,0x1EE28
        dd 0x1EE33,0x1EE33
        dd 0x1EE38,0x1EE38
        dd 0x1EE3A,0x1EE3A
        dd 0x1EE3C,0x1EE41
        dd 0x1EE43,0x1EE46
        dd 0x1EE48,0x1EE48
        dd 0x1EE4A,0x1EE4A
        dd 0x1EE4C,0x1EE4C
        dd 0x1EE50,0x1EE50
        dd 0x1EE53,0x1EE53
        dd 0x1EE55,0x1EE56
        dd 0x1EE58,0x1EE58
        dd 0x1EE5A,0x1EE5A
        dd 0x1EE5C,0x1EE5C
        dd 0x1EE5E,0x1EE5E
        dd 0x1EE60,0x1EE60
        dd 0x1EE63,0x1EE63
        dd 0x1EE65,0x1EE66
        dd 0x1EE6B,0x1EE6B
        dd 0x1EE73,0x1EE73
        dd 0x1EE78,0x1EE78
        dd 0x1EE7D,0x1EE7D
        dd 0x1EE7F,0x1EE7F
        dd 0x1EE8A,0x1EE8A
        dd 0x1EE9C,0x1EEA0
        dd 0x1EEA4,0x1EEA4
        dd 0x1EEAA,0x1EEAA
        dd 0x1EEBC,0x1EEEF
        dd 0x1EEF2,0x1EFFF
        dd 0x1F02C,0x1F02F
        dd 0x1F094,0x1F09F
        dd 0x1F0AF,0x1F0B0
        dd 0x1F0C0,0x1F0C0
        dd 0x1F0D0,0x1F0D0
        dd 0x1F0F6,0x1F0FF
        dd 0x1F1AE,0x1F1E5
        dd 0x1F203,0x1F20F
        dd 0x1F23C,0x1F23F
        dd 0x1F249,0x1F24F
        dd 0x1F252,0x1F25F
        dd 0x1F266,0x1F2FF
        dd 0x1F6D8,0x1F6DB
        dd 0x1F6ED,0x1F6EF
        dd 0x1F6FD,0x1F6FF
        dd 0x1F777,0x1F77A
        dd 0x1F7DA,0x1F7DF
        dd 0x1F7EC,0x1F7EF
        dd 0x1F7F1,0x1F7FF
        dd 0x1F80C,0x1F80F
        dd 0x1F848,0x1F84F
        dd 0x1F85A,0x1F85F
        dd 0x1F888,0x1F88F
        dd 0x1F8AE,0x1F8AF
        dd 0x1F8BC,0x1F8BF
        dd 0x1F8C2,0x1F8FF
        dd 0x1FA54,0x1FA5F
        dd 0x1FA6E,0x1FA6F
        dd 0x1FA7D,0x1FA7F
        dd 0x1FA8A,0x1FA8E
        dd 0x1FAC7,0x1FACD
        dd 0x1FADD,0x1FADE
        dd 0x1FAEA,0x1FAEF
        dd 0x1FAF9,0x1FAFF
        dd 0x1FB93,0x1FB93
        dd 0x1FBFA,0x1FFFF
        dd 0x2A6E0,0x2A6FF
        dd 0x2B73A,0x2B73F
        dd 0x2B81E,0x2B81F
        dd 0x2CEA2,0x2CEAF
        dd 0x2EBE1,0x2EBEF
        dd 0x2EE5E,0x2F7FF
        dd 0x2FA1E,0x2FFFF
        dd 0x3134B,0x3134F
        dd 0x323B0,0xE00FF
        dd 0xE01F0,0x10FFFF

;;; data rt_ucspecial
; mappings that are not one code point to one: code point, kind (U upper, L lower,
; T title: unlike upper, F casefold), then the text (UTF-8, NUL-terminated)
rt_ucspecial:
        dd 0xB5
        db 'F',206,188,0
        dd 0xDF
        db 'U',83,83,0
        dd 0xDF
        db 'T',83,115,0
        dd 0xDF
        db 'F',115,115,0
        dd 0x130
        db 'L',105,204,135,0
        dd 0x149
        db 'U',202,188,78,0
        dd 0x149
        db 'F',202,188,110,0
        dd 0x17F
        db 'F',115,0
        dd 0x1C4
        db 'T',199,133,0
        dd 0x1C5
        db 'T',199,133,0
        dd 0x1C6
        db 'T',199,133,0
        dd 0x1C7
        db 'T',199,136,0
        dd 0x1C8
        db 'T',199,136,0
        dd 0x1C9
        db 'T',199,136,0
        dd 0x1CA
        db 'T',199,139,0
        dd 0x1CB
        db 'T',199,139,0
        dd 0x1CC
        db 'T',199,139,0
        dd 0x1F0
        db 'U',74,204,140,0
        dd 0x1F0
        db 'F',106,204,140,0
        dd 0x1F1
        db 'T',199,178,0
        dd 0x1F2
        db 'T',199,178,0
        dd 0x1F3
        db 'T',199,178,0
        dd 0x345
        db 'F',206,185,0
        dd 0x390
        db 'U',206,153,204,136,204,129,0
        dd 0x390
        db 'F',206,185,204,136,204,129,0
        dd 0x3B0
        db 'U',206,165,204,136,204,129,0
        dd 0x3B0
        db 'F',207,133,204,136,204,129,0
        dd 0x3C2
        db 'F',207,131,0
        dd 0x3D0
        db 'F',206,178,0
        dd 0x3D1
        db 'F',206,184,0
        dd 0x3D5
        db 'F',207,134,0
        dd 0x3D6
        db 'F',207,128,0
        dd 0x3F0
        db 'F',206,186,0
        dd 0x3F1
        db 'F',207,129,0
        dd 0x3F5
        db 'F',206,181,0
        dd 0x587
        db 'U',212,181,213,146,0
        dd 0x587
        db 'T',212,181,214,130,0
        dd 0x587
        db 'F',213,165,214,130,0
        dd 0

;;; code rt_cp_props : rt_unicode
rt_cp_props:                    ; eax = code point -> eax = class bits (see rt_unicode), ecx = upper, edx = lower (simple mappings)
        push    ebx esi edi
        xor     esi,esi
        mov     edi,[rt_unicode_n]
        dec     edi
.l:     cmp     esi,edi
        jg      .none
        lea     ebx,[esi+edi]
        shr     ebx,1
        lea     ecx,[ebx*4+ebx]
        lea     ecx,[rt_unicode+ecx*4]
        cmp     eax,[ecx]
        jb      .below
        cmp     eax,[ecx+4]
        ja      .above
        mov     edx,eax
        sub     edx,[ecx]
        and     edx,1
        lea     edx,[edx*2+edx]
        lea     esi,[ecx+8+edx*2]
        movzx   ecx,word [esi+2]        ; the mappings stay in the BMP: mod 0x10000
        test    ecx,ecx
        jz      @f
        add     ecx,eax
        and     ecx,0xFFFF
        jmp     .lo
@@:     mov     ecx,eax
.lo:    movzx   edx,word [esi+4]
        test    edx,edx
        jz      @f
        add     edx,eax
        and     edx,0xFFFF
        jmp     .bits
@@:     mov     edx,eax
.bits:  movzx   eax,word [esi]
        pop     edi esi ebx
        ret
.below: lea     edi,[ebx-1]
        jmp     .l
.above: lea     esi,[ebx+1]
        jmp     .l
.none:  mov     ecx,eax
        mov     edx,eax
        xor     eax,eax
        pop     edi esi ebx
        ret

;;; code rt_cp_isspace : rt_u8_next
rt_cp_isspace:                  ; eax = code point -> ZF set when it is whitespace (nothing else changes; rt_bmode: ASCII whitespace)
        cmp     eax,' '
        je      .r
        cmp     eax,9
        jb      .no
        cmp     eax,13
        jbe     .yes
        cmp     byte [rt_bmode],0
        jne     .no
        cmp     eax,0x1C
        jb      .no
        cmp     eax,0x1F
        jbe     .yes
        cmp     eax,0x85
        je      .r
        cmp     eax,0xA0
        je      .r
        cmp     eax,0x1680
        je      .r
        cmp     eax,0x2000
        jb      .no
        cmp     eax,0x200A
        jbe     .yes
        cmp     eax,0x2028
        je      .r
        cmp     eax,0x2029
        je      .r
        cmp     eax,0x202F
        je      .r
        cmp     eax,0x205F
        je      .r
        cmp     eax,0x3000
.r:     ret
.yes:   cmp     eax,eax
        ret
.no:    test    esp,esp
        ret

;;; code rt_cp_printable : rt_noprint
rt_cp_printable:                ; eax = code point -> eax = 1 when repr() shows it as it is (nothing else changes)
        cmp     eax,0x80
        jae     .wide
        cmp     eax,0x20
        jb      .no
        cmp     eax,0x7F
        je      .no
.yes:   mov     eax,1
        ret
.no:    xor     eax,eax
        ret
.wide:  push    ebx esi edi
        xor     esi,esi
        mov     edi,[rt_noprint_n]
        dec     edi
.l:     cmp     esi,edi
        jg      .out
        lea     ebx,[esi+edi]
        shr     ebx,1
        cmp     eax,[rt_noprint+ebx*8]
        jb      .below
        cmp     eax,[rt_noprint+ebx*8+4]
        ja      .above
        pop     edi esi ebx
        jmp     .no
.below: lea     edi,[ebx-1]
        jmp     .l
.above: lea     esi,[ebx+1]
        jmp     .l
.out:   pop     edi esi ebx
        jmp     .yes

;;; code rt_uc_special : rt_ucspecial
rt_uc_special:                  ; eax = code point, dl = kind (U L T F) -> eax = its mapping (NUL-terminated) when not one code point to one, else 0
        cmp     eax,0xB5
        jb      .no
        push    esi
        mov     esi,rt_ucspecial
.l:     mov     ecx,[esi]
        test    ecx,ecx
        jz      .none
        cmp     ecx,eax
        jne     .skip
        cmp     [esi+4],dl
        jne     .skip
        lea     eax,[esi+5]
        pop     esi
        ret
.skip:  add     esi,5
@@:     cmp     byte [esi],0
        je      @f
        inc     esi
        jmp     @b
@@:     inc     esi
        jmp     .l
.none:  pop     esi
.no:    xor     eax,eax
        ret

;;; code rt_final_sigma : rt_u8_next rt_cp_props
rt_final_sigma:                 ; eax = str, ecx, edx = where a capital sigma starts and ends (byte offsets) -> eax = the small sigma it lowers to
        push    ebx esi edi ebp
        lea     ebx,[eax+12]
        mov     edi,[eax+8]
        add     edi,ebx
        lea     ebp,[ebx+edx]
        add     ecx,ebx
.back:  cmp     ecx,ebx         ; the one before (case-ignorable ones skipped) must be cased
        jbe     .no
        dec     ecx
@@:     cmp     ecx,ebx
        jbe     @f
        mov     al,[ecx]
        and     al,0xC0
        cmp     al,0x80
        jne     @f
        dec     ecx
        jmp     @b
@@:     mov     esi,ecx
        push    ecx
        call    rt_u8_next
        call    rt_cp_props
        pop     ecx
        test    eax,512
        jnz     .back
        test    eax,128
        jz      .no
        mov     esi,ebp         ; and the one after must not be
.fwd:   cmp     esi,edi
        jae     .yes
        call    rt_u8_next
        call    rt_cp_props
        test    eax,512
        jnz     .fwd
        test    eax,128
        jnz     .no
.yes:   mov     eax,0x3C2
        jmp     .out
.no:    mov     eax,0x3C3
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_str_char : rt_str_cplen rt_str_off rt_u8_next rt_cp_char rt_panic_index
rt_str_char:                    ; eax = s, edx = index -> eax = s[index] (static, borrowed)
        push    ebx
        mov     ebx,eax
        call    rt_str_cplen
        test    edx,edx
        jns     @f
        add     edx,eax
@@:     cmp     edx,eax
        jae     .bad
        cmp     eax,[ebx+8]
        jne     .wide
        movzx   eax,byte [ebx+12+edx]
        pop     ebx
        jmp     rt_cp_char
.wide:  mov     eax,ebx
        call    rt_str_off
        push    esi edi
        lea     esi,[ebx+12+eax]
        mov     edi,[ebx+8]
        lea     edi,[ebx+12+edi]
        call    rt_u8_next
        pop     edi esi
        pop     ebx
        jmp     rt_cp_char
.bad:   pop     ebx
        jmp     rt_panic_index_s

;;; code rt_str_chars : rt_list_new rt_list_push rt_kd_str rt_u8_next rt_cp_char
rt_str_chars:                   ; eax = s -> eax = list[str] of its characters
        push    ebx esi edi
        mov     esi,eax
        mov     eax,rt_kd_str
        call    rt_list_new
        mov     ebx,eax
        test    esi,esi
        jz      .out
        mov     edi,[esi+8]
        add     esi,12
        add     edi,esi
.next:  cmp     esi,edi
        jae     .out
        call    rt_u8_next
        call    rt_cp_char
        push    eax
        mov     eax,ebx
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        jmp     .next
.out:   mov     eax,ebx
        pop     edi esi ebx
        ret

;;; code rt_chr : rt_cp_char rt_panic_value
rt_chr:                         ; eax = code point -> eax = chr(code) (static)
        cmp     eax,0x110000
        jae     .bad
        jmp     rt_cp_char
.bad:   mov     esi,rt_msg_chr
        jmp     rt_panic_value
;;; data rt_chr
rt_msg_chr      db 'chr() arg not in range(0x110000)',0

;;; code rt_ord : rt_str_cplen rt_u8_next rt_sb_need rt_sb_cstr rt_sb_int rt_sb_take rt_raise_typeerr
rt_ord:                         ; eax = s -> eax = ord(s)
        push    esi edi
        mov     esi,eax
        call    rt_str_cplen
        cmp     eax,1
        jne     .bad
        mov     edi,[esi+8]
        add     esi,12
        add     edi,esi
        call    rt_u8_next
        pop     edi esi
        ret
.bad:   push    eax
        push    dword [rt_sb_len]
        mov     eax,rt_msg_ord
        call    rt_sb_cstr
        mov     eax,[esp+4]
        xor     edx,edx
        call    rt_sb_int
        mov     eax,rt_msg_ord2
        call    rt_sb_cstr
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_typeerr
;;; data rt_ord
rt_msg_ord      db 'ord() expected a character, but string of length ',0
rt_msg_ord2     db ' found',0

;;; code rt_fillchar : rt_str_cplen rt_u8_next rt_str_new rt_raise_typeerr rt_sb_need rt_sb_cstr rt_sb_take
rt_fillchar:                    ; eax = str -> eax = its one code point (a TypeError unless it has exactly one)
        push    esi edi
        mov     esi,eax
        call    rt_str_cplen
        cmp     eax,1
        jne     .bad
        mov     edi,[esi+8]
        add     esi,12
        add     edi,esi
        call    rt_u8_next
        pop     edi esi
        ret
.bad:   push    dword [rt_sb_len]
        mov     eax,rt_msg_fill
        call    rt_sb_cstr
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_typeerr
;;; data rt_fillchar
rt_msg_fill     db 'The fill character must be exactly one character long',0

;;; code rt_slice_range : rt_panic_value
rt_slice_range:                 ; eax = length, esi -> {lo, hi, step, flags}: eax = start, edx = step, ecx = count
        push    ebx edi ebp     ; flags: 1 = no lo, 2 = no hi, 4 = no step
        mov     ebp,eax
        mov     edx,1
        test    dword [esi+12],4
        jnz     @f
        mov     edx,[esi+8]
        test    edx,edx
        jz      .zero
@@:     test    dword [esi+12],1
        jz      .lo
        xor     ebx,ebx
        test    edx,edx
        jns     .hi0
        lea     ebx,[ebp-1]
        jmp     .hi0
.lo:    mov     ebx,[esi]
        call    .adjust
.hi0:   test    dword [esi+12],2
        jz      .hi
        mov     edi,ebp
        test    edx,edx
        jns     .count
        or      edi,-1
        jmp     .count
.hi:    push    ebx
        mov     ebx,[esi+4]
        call    .adjust
        mov     edi,ebx
        pop     ebx
.count: mov     eax,edi
        test    edx,edx
        js      .back
        sub     eax,ebx
        jle     .none
        dec     eax
        push    edx
        mov     ecx,edx
        xor     edx,edx
        div     ecx
        pop     edx
        lea     ecx,[eax+1]
        jmp     .done
.back:  mov     eax,ebx
        sub     eax,edi
        jle     .none
        dec     eax
        push    edx
        mov     ecx,edx
        neg     ecx
        xor     edx,edx
        div     ecx
        pop     edx
        lea     ecx,[eax+1]
        jmp     .done
.none:  xor     ecx,ecx
.done:  mov     eax,ebx
        pop     ebp edi ebx
        ret
.adjust:                        ; ebx = index -> clamped into the sequence (ebp = length, edx = step)
        test    ebx,ebx
        jns     .pos
        add     ebx,ebp
        jns     .ret
        xor     ebx,ebx
        test    edx,edx
        jns     .ret
        or      ebx,-1
        ret
.pos:   cmp     ebx,ebp
        jl      .ret
        mov     ebx,ebp
        test    edx,edx
        jns     .ret
        dec     ebx
.ret:   ret
.zero:  mov     esi,rt_msg_step
        jmp     rt_panic_value
;;; data rt_slice_range
rt_msg_step     db 'slice step cannot be zero',0

;;; code rt_str_slice : rt_slice_range rt_str_new rt_str_cplen rt_str_off rt_u8_next rt_alloc rt_free
rt_str_slice:                   ; [esp+4] s, lo, hi, step, flags -> eax = new str
        push    ebx esi edi ebp
        mov     ebx,[esp+20]
        mov     eax,ebx
        call    rt_str_cplen
        mov     ebp,eax
        lea     esi,[esp+24]
        call    rt_slice_range  ; eax = start, edx = step, ecx = count
        test    ecx,ecx
        jz      .empty
        test    ebx,ebx
        jz      .empty
        cmp     ebp,[ebx+8]
        jne     .wide
        push    eax edx ecx     ; one byte per code point
        mov     eax,ecx
        call    rt_str_new
        pop     ecx edx esi
        push    eax
        lea     edi,[eax+12]
        lea     esi,[ebx+12+esi]
.copy:  mov     al,[esi]
        mov     [edi],al
        inc     edi
        add     esi,edx
        dec     ecx
        jnz     .copy
        pop     eax
        pop     ebp edi esi ebx
        ret
.empty: xor     eax,eax
        call    rt_str_new
        pop     ebp edi esi ebx
        ret
.wide:  cmp     edx,1
        jne     .steps
        push    ecx             ; a run: the bytes from off(start) to off(start + count)
        push    eax
        mov     edx,eax
        mov     eax,ebx
        call    rt_str_off
        mov     esi,eax
        pop     edx
        pop     ecx
        add     edx,ecx
        mov     eax,ebx
        call    rt_str_off
        mov     ecx,eax
        sub     ecx,esi
        push    ecx
        mov     eax,ecx
        call    rt_str_new
        pop     ecx
        lea     esi,[ebx+12+esi]
        lea     edi,[eax+12]
        rep     movsb
        pop     ebp edi esi ebx
        ret
.steps: push    eax edx ecx     ; [esp] count, [esp+4] step, [esp+8] start
        lea     eax,[ebp*4+4]   ; the byte offset of every code point (and of the end)
        call    rt_alloc
        mov     ebp,eax
        push    ebp
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
.offs:  lea     eax,[ebx+12]
        mov     ecx,esi
        sub     ecx,eax
        mov     [ebp],ecx
        add     ebp,4
        cmp     esi,edi
        jae     .measure
        call    rt_u8_next
        jmp     .offs
.measure:
        pop     ebp
        mov     ecx,[esp]
        mov     edx,[esp+8]
        xor     eax,eax         ; the bytes of the result
.m:     mov     esi,[ebp+edx*4+4]
        sub     esi,[ebp+edx*4]
        add     eax,esi
        add     edx,[esp+4]
        dec     ecx
        jnz     .m
        call    rt_str_new
        lea     edi,[eax+12]
        push    eax
        mov     edx,[esp+12]
.c:     mov     esi,[ebp+edx*4]
        mov     ecx,[ebp+edx*4+4]
        sub     ecx,esi
        lea     esi,[ebx+12+esi]
        rep     movsb
        add     edx,[esp+8]
        dec     dword [esp+4]
        jnz     .c
        mov     eax,ebp
        call    rt_free
        pop     eax
        add     esp,12
        pop     ebp edi esi ebx
        ret

;;; code rt_str_mul : rt_str_new
rt_str_mul:                     ; eax = s, edx = n -> eax = s * n
        push    ebx esi edi
        mov     esi,eax
        xor     ecx,ecx
        test    esi,esi
        jz      @f
        mov     ecx,[esi+8]
@@:     test    edx,edx
        jg      @f
        xor     edx,edx
@@:     mov     eax,ecx
        imul    eax,edx
        push    ecx edx
        call    rt_str_new
        pop     ebx ecx
        lea     edi,[eax+12]
        test    ecx,ecx
        jz      .out
.rep:   test    ebx,ebx
        jz      .out
        push    esi ecx
        add     esi,12
        rep     movsb
        pop     ecx esi
        dec     ebx
        jmp     .rep
.out:   pop     edi esi ebx
        ret

;;; code rt_str_range : rt_str_cplen rt_str_off
rt_str_range:                   ; eax = str, esi -> {start, end, flags (1: no start, 2: no end)} in code points -> ecx, edx = the byte range (ecx = length+1 when start is past the end)
        push    ebx edi ebp
        mov     ebx,eax
        call    rt_str_cplen
        mov     ebp,eax
        xor     ecx,ecx
        test    dword [esi+8],1
        jnz     @f
        mov     ecx,[esi]
@@:     mov     edx,ebp
        test    dword [esi+8],2
        jnz     @f
        mov     edx,[esi+4]
@@:     test    ecx,ecx
        jns     .a
        add     ecx,ebp
        jns     .a
        xor     ecx,ecx
.a:     test    edx,edx
        jns     .e
        add     edx,ebp
        jns     .e
        xor     edx,edx
.e:     cmp     edx,ebp
        jle     @f
        mov     edx,ebp
@@:     mov     edi,edx
        cmp     ecx,ebp
        jle     @f
        mov     ecx,[ebx+8]
        inc     ecx
        jmp     .end
@@:     mov     eax,ebx
        mov     edx,ecx
        call    rt_str_off
        mov     ecx,eax
.end:   push    ecx
        mov     eax,ebx
        mov     edx,edi
        call    rt_str_off
        mov     edx,eax
        pop     ecx
        pop     ebp edi ebx
        ret

;;; code rt_find_bytes
rt_find_bytes:                  ; ebx = str, [ecx, edx) = byte range, edi = needle (str), eax = 1: the last one -> eax = byte offset or -1
        push    esi ebp
        cmp     ecx,edx
        jg      .nf
        mov     ebp,[edi+8]
        sub     edx,ebp         ; the last possible start
        cmp     ecx,edx
        jg      .nf
        test    ebp,ebp
        jz      .empty
        test    eax,eax
        jnz     .back
.fwd:   call    .at
        je      .found
        inc     ecx
        cmp     ecx,edx
        jle     .fwd
        jmp     .nf
.back:  xchg    ecx,edx
.bl:    call    .at
        je      .found
        dec     ecx
        cmp     ecx,edx
        jge     .bl
.nf:    or      eax,-1
        pop     ebp esi
        ret
.empty: test    eax,eax
        jz      .found
        mov     ecx,edx
.found: mov     eax,ecx
        pop     ebp esi
        ret
.at:    push    ecx esi edi     ; ZF set when the needle is at byte ecx
        lea     esi,[ebx+12+ecx]
        add     edi,12
        mov     ecx,ebp
        repe    cmpsb
        pop     edi esi ecx
        ret

;;; code rt_str_find : rt_str_range rt_find_bytes rt_str_cpidx rt_panic_value
rt_str_find:                    ; [esp+4] s, sub, start, end, flags (1: no start, 2: no end); eax = 0 find / 1 rfind / 2 index / 3 rindex (+4: of bytes) -> eax = code point index or -1
        push    ebx esi edi ebp
        mov     ebp,eax
        mov     ebx,[esp+20]
        mov     edi,[esp+24]
        lea     esi,[esp+28]
        mov     eax,ebx
        call    rt_str_range
        mov     eax,ebp
        and     eax,1
        call    rt_find_bytes
        cmp     eax,-1
        je      .nf
        mov     edx,eax
        mov     eax,ebx
        call    rt_str_cpidx
.out:   pop     ebp edi esi ebx
        ret
.nf:    test    ebp,2
        jz      .out
        mov     esi,rt_msg_substr
        test    ebp,4
        jz      @f
        mov     esi,rt_msg_subsec
@@:     jmp     rt_panic_value
;;; data rt_str_find
rt_msg_substr   db 'substring not found',0
rt_msg_subsec   db 'subsection not found',0

;;; code rt_str_unquote : rt_str_new
rt_str_unquote:                 ; eax = str -> eax = it without quotes around it ('k' -> k; only when nothing in it is escaped), new reference
        mov     ecx,[eax+8]
        cmp     ecx,2
        jb      .same
        mov     dl,[eax+12]
        cmp     dl,39
        je      @f
        cmp     dl,'"'
        jne     .same
@@:     cmp     dl,[eax+11+ecx]
        jne     .same
        push    esi edi
        lea     esi,[eax+13]
        sub     ecx,2
        jz      .copy
        push    ecx
        mov     edi,esi
        mov     al,'\'
        repne   scasb
        pop     ecx
        je      .back                   ; something escaped: as it is
.copy:  push    esi ecx
        mov     eax,ecx
        call    rt_str_new
        pop     ecx esi
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret
.back:  lea     eax,[esi-13]
        pop     edi esi
.same:  inc     dword [eax]
        ret

;;; code rt_str_contains : rt_find_bytes
rt_str_contains:                ; eax = s, edx = sub -> eax = 1 when sub is in s
        push    ebx edi
        mov     ebx,eax
        mov     edi,edx
        xor     ecx,ecx
        mov     edx,[ebx+8]
        xor     eax,eax
        call    rt_find_bytes
        inc     eax
        jz      @f
        mov     eax,1
@@:     pop     edi ebx
        ret

;;; code rt_str_count : rt_str_range rt_find_bytes rt_str_cpidx
rt_str_count:                   ; [esp+4] s, sub, start, end, flags (1: no start, 2: no end) -> eax = non-overlapping occurrences
        push    ebx esi edi ebp
        mov     ebx,[esp+20]
        mov     edi,[esp+24]
        lea     esi,[esp+28]
        mov     eax,ebx
        call    rt_str_range
        xor     ebp,ebp
        cmp     ecx,edx
        jg      .out            ; start past the end (or after end): none
        cmp     dword [edi+8],0
        je      .empty
.l:     push    edx
        xor     eax,eax
        call    rt_find_bytes
        pop     edx
        cmp     eax,-1
        je      .out
        inc     ebp
        mov     ecx,eax
        add     ecx,[edi+8]
        jmp     .l
.empty: push    ecx             ; "": one more than the code points in between
        mov     eax,ebx
        call    rt_str_cpidx
        mov     ebp,eax
        pop     edx
        mov     eax,ebx
        call    rt_str_cpidx
        sub     ebp,eax
        inc     ebp
.out:   mov     eax,ebp
        pop     ebp edi esi ebx
        ret

;;; code rt_str_affix : rt_str_range
rt_str_affix:                   ; [esp+4] s, affix, start, end, flags (1: no start, 2: no end, 4: affix is a tuple of str); eax = 0 startswith / 1 endswith -> eax = bool
        push    ebx esi edi ebp
        mov     ebp,eax
        mov     ebx,[esp+20]
        lea     esi,[esp+28]
        mov     eax,ebx
        call    rt_str_range
        mov     edi,[esp+24]
        test    dword [esp+36],4
        jnz     .tuple
        call    .one
        jmp     .out
.tuple: mov     esi,[edi+8]     ; its TD: count, size, (kd, offset)...
        push    ecx edx
        xor     eax,eax
.ti:    cmp     eax,[esi]
        jae     .tno
        push    eax
        mov     edi,[esi+12+eax*8]      ; the item's offset (from the tuple's start)
        mov     edx,[esp+36]
        mov     edi,[edx+edi]
        mov     ecx,[esp+8]
        mov     edx,[esp+4]
        call    .one
        pop     ecx
        test    eax,eax
        jnz     .tout
        lea     eax,[ecx+1]
        jmp     .ti
.tno:   xor     eax,eax
.tout:  add     esp,8
.out:   pop     ebp edi esi ebx
        ret
.one:   push    esi edi         ; edi = affix, [ecx, edx) -> eax = bool
        mov     eax,[edi+8]
        mov     esi,edx
        sub     esi,ecx
        cmp     ecx,[ebx+8]
        jg      .short
        cmp     esi,eax
        jl      .short
        lea     esi,[ebx+12+ecx]
        test    ebp,ebp
        jz      @f
        lea     esi,[ebx+12+edx]
        sub     esi,eax
@@:     mov     ecx,eax
        add     edi,12
        mov     eax,1
        jecxz   .ret
        repe    cmpsb
        je      .ret
        xor     eax,eax
.ret:   pop     edi esi
        ret
.short: xor     eax,eax         ; only "" fits, and only when start <= length, start <= end
        cmp     ecx,[ebx+8]
        jg      .ret
        cmp     dword [edi+8],0
        jne     .ret
        cmp     edx,ecx
        jl      .ret
        inc     eax
        jmp     .ret

;;; code rt_str_case : rt_u8_next rt_cp_props rt_sb_cp rt_sb_cstr rt_sb_take rt_uc_special rt_final_sigma
rt_str_case:                    ; eax = s, edx = 0 upper / 1 lower / 2 swapcase / 3 title / 4 capitalize / 5 casefold -> eax = new str
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        push    eax             ; [esp] s, [esp+4] the mark
        mov     ebx,edx
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
        xor     ebp,ebp         ; bit 0: the previous one was cased (title), bit 1: past the first (capitalize)
.l:     cmp     esi,edi
        jae     .done
        push    esi
        call    rt_u8_next
        push    eax
        call    rt_cp_props     ; eax = classes, ecx = upper, edx = lower
        push    ecx edx         ; [esp] lower, [esp+4] upper, [esp+8] the code point, [esp+12] where it starts, [esp+16] s
        mov     dl,'U'          ; dl: which mapping - U upper, L lower, T title, F casefold
        test    ebx,ebx
        jz      .kind
        mov     dl,'L'
        cmp     ebx,1
        je      .kind
        mov     dl,'F'
        cmp     ebx,5
        je      .kind
        cmp     ebx,2
        jne     .tc
        mov     dl,'L'          ; swapcase
        test    eax,32
        jnz     .kind
        mov     dl,'U'
        test    eax,64
        jnz     .kind
        mov     eax,[esp+8]     ; neither: itself
        jmp     .put
.tc:    mov     dl,'T'
        cmp     ebx,3
        jne     .cap
        test    ebp,1           ; title: lower after a cased one
        jz      @f
        mov     dl,'L'
@@:     and     ebp,-2
        test    eax,128
        jz      .kind
        or      ebp,1
        jmp     .kind
.cap:   test    ebp,2           ; capitalize: title first, then lower
        jz      @f
        mov     dl,'L'
@@:     or      ebp,2
.kind:  mov     eax,[esp+8]
        cmp     dl,'L'
        jne     .spec
        cmp     eax,0x3A3
        jne     .spec
        mov     eax,[esp+16]    ; a capital sigma lowers to a final one at the end of a word
        mov     ecx,[esp+12]
        sub     ecx,eax
        sub     ecx,12
        mov     edx,esi
        sub     edx,eax
        sub     edx,12
        call    rt_final_sigma
        jmp     .put
.spec:  push    edx
        call    rt_uc_special
        pop     edx
        test    eax,eax
        jnz     .text
        cmp     dl,'T'          ; title is upper, casefold lower, unless special
        jne     @f
        mov     dl,'U'
        jmp     .again
@@:     cmp     dl,'F'
        jne     .simple
        mov     dl,'L'
.again: mov     eax,[esp+8]
        push    edx
        call    rt_uc_special
        pop     edx
        test    eax,eax
        jnz     .text
.simple:mov     eax,[esp]
        cmp     dl,'U'
        jne     .put
        mov     eax,[esp+4]
.put:   call    rt_sb_cp
        jmp     .next
.text:  call    rt_sb_cstr
.next:  add     esp,16
        jmp     .l
.done:  add     esp,4
        pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret

;;; code rt_str_is : rt_u8_next rt_cp_props rt_cp_printable
rt_str_is:                      ; eax = s, edx = 0 isalpha / 1 isdigit / 2 isalnum / 3 isspace / 4 islower / 5 isupper / 6 isdecimal / 7 isascii / 8 isnumeric / 9 istitle / 10 isprintable / 11 isidentifier -> eax = bool
        push    ebx esi edi ebp
        mov     ebx,edx
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        test    edi,edi
        jz      .empty
        add     edi,esi
        xor     ebp,ebp         ; bit 0: a cased one seen, bit 1: the previous one cased (istitle), bit 2: past the first (isidentifier)
.l:     cmp     esi,edi
        jae     .end
        call    rt_u8_next
        cmp     ebx,7
        jne     @f
        cmp     eax,0x80
        jae     .no
        jmp     .l
@@:     cmp     ebx,10
        jne     @f
        call    rt_cp_printable
        test    eax,eax
        jz      .no
        jmp     .l
@@:     cmp     ebx,11
        jne     @f
        cmp     eax,'_'
        je      .id
@@:     call    rt_cp_props
        mov     ecx,1
        test    ebx,ebx
        jz      .need
        mov     ecx,2
        cmp     ebx,1
        je      .need
        mov     ecx,1+8
        cmp     ebx,2
        je      .need
        mov     ecx,16
        cmp     ebx,3
        je      .need
        mov     ecx,4
        cmp     ebx,6
        je      .need
        mov     ecx,8
        cmp     ebx,8
        je      .need
        cmp     ebx,4
        jne     @f
        test    eax,32+1024
        jnz     .no
        test    eax,64
        jz      .l
        or      ebp,1
        jmp     .l
@@:     cmp     ebx,5
        jne     @f
        test    eax,64+1024
        jnz     .no
        test    eax,32
        jz      .l
        or      ebp,1
        jmp     .l
@@:     cmp     ebx,9
        jne     .ident
        test    eax,32+1024
        jz      @f
        test    ebp,2           ; upper after a cased one
        jnz     .no
        or      ebp,3
        jmp     .l
@@:     test    eax,64
        jz      @f
        test    ebp,2           ; lower with no cased one before
        jz      .no
        or      ebp,3
        jmp     .l
@@:     and     ebp,-3
        jmp     .l
.ident: test    eax,1
        jnz     .id
        test    ebp,4
        jz      .no
        test    eax,4
        jz      .no
.id:    or      ebp,4
        jmp     .l
.need:  test    eax,ecx
        jz      .no
        jmp     .l
.end:   cmp     ebx,4
        je      .any
        cmp     ebx,5
        je      .any
        cmp     ebx,9
        jne     .yes
.any:   mov     eax,ebp
        and     eax,1
        jmp     .out
.empty: cmp     ebx,7
        je      .yes
        cmp     ebx,10
        je      .yes
.no:    xor     eax,eax
        jmp     .out
.yes:   mov     eax,1
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_str_strip : rt_str_new rt_u8_next rt_cp_isspace
rt_str_strip:                   ; eax = s, edx = 1 left / 2 right / 3 both, ecx = the characters (str), 0: whitespace -> eax = new str
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,ecx
        push    edx             ; [esp+4] which ends
        lea     esi,[ebx+12]    ; the start
        mov     edi,[ebx+8]
        add     edi,esi         ; the end of the str (for decoding)
        push    edi             ; [esp] the end
        test    dword [esp+4],1
        jz      .right
.left:  cmp     esi,[esp]
        jae     .right
        push    esi
        call    rt_u8_next
        call    .in
        pop     eax
        je      .left
        mov     esi,eax
.right: test    dword [esp+4],2
        jz      .make
.r:     mov     ecx,[esp]
        cmp     ecx,esi
        jbe     .make
        dec     ecx             ; back to where the last code point starts
.back:  cmp     ecx,esi
        jbe     @f
        cmp     byte [rt_bmode],0
        jne     @f
        mov     al,[ecx]
        and     al,0xC0
        cmp     al,0x80
        jne     @f
        dec     ecx
        jmp     .back
@@:     push    esi ecx
        mov     esi,ecx
        call    rt_u8_next
        call    .in
        pop     ecx esi
        jne     .make
        mov     [esp],ecx
        jmp     .r
.make:  pop     ecx
        add     esp,4
        sub     ecx,esi
        push    ecx
        mov     eax,ecx
        call    rt_str_new
        pop     ecx
        push    eax
        lea     edi,[eax+12]
        rep     movsb
        pop     eax
        pop     ebp edi esi ebx
        ret
.in:    test    ebp,ebp         ; eax = code point -> ZF set when it goes
        jz      rt_cp_isspace
        push    esi edi edx
        mov     edx,eax
        lea     esi,[ebp+12]
        mov     edi,[ebp+8]
        add     edi,esi
.inl:   cmp     esi,edi
        jae     .inno
        call    rt_u8_next
        cmp     eax,edx
        jne     .inl
        pop     edx edi esi
        ret
.inno:  pop     edx edi esi
        test    esp,esp
        ret

;;; code rt_str_replace : rt_sb_bytes rt_sb_take rt_find_bytes rt_u8_next
rt_str_replace:                 ; [esp+4] s, old, new, count (< 0: all) -> eax = new str
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        mov     ebx,[esp+24]
        mov     edi,[esp+28]
        mov     ebp,[esp+36]
        test    ebp,ebp
        jns     @f
        mov     ebp,0x7FFFFFFF
@@:     cmp     dword [edi+8],0
        je      .empty
        xor     esi,esi         ; position
.l:     test    ebp,ebp
        jz      .rest
        mov     ecx,esi
        mov     edx,[ebx+8]
        xor     eax,eax
        call    rt_find_bytes
        cmp     eax,-1
        je      .rest
        push    eax
        lea     eax,[ebx+12+esi]
        mov     edx,[esp]
        sub     edx,esi
        call    rt_sb_bytes
        pop     esi
        call    .new
        add     esi,[edi+8]
        dec     ebp
        jmp     .l
.rest:  lea     eax,[ebx+12+esi]
        mov     edx,[ebx+8]
        sub     edx,esi
        call    rt_sb_bytes
.out:   pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret
.empty: lea     esi,[ebx+12]    ; "": the new text before each code point and at the end
        mov     edi,[ebx+8]
        add     edi,esi
        test    ebp,ebp
        jz      .el
        call    .new
        dec     ebp
.el:    cmp     esi,edi
        jae     .out
        push    esi
        call    rt_u8_next
        pop     eax
        mov     edx,esi
        sub     edx,eax
        call    rt_sb_bytes
        test    ebp,ebp
        jz      .el
        call    .new
        dec     ebp
        jmp     .el
.new:   mov     eax,[esp+36]    ; append the new text (the stack: this call, the mark, 4 registers, the return)
        test    eax,eax
        jz      @f
        mov     edx,[eax+8]
        add     eax,12
        call    rt_sb_bytes
@@:     ret

;;; data rt_msg_sep
rt_msg_sep      db 'empty separator',0

;;; code rt_str_split : rt_list_new rt_list_push rt_kd_str rt_str_new rt_u8_next rt_cp_isspace rt_find_bytes rt_list_reverse rt_panic_value rt_msg_sep
rt_str_rsplit:                  ; eax = s, edx = separator (0: runs of whitespace), ecx = maxsplit (< 0: no limit) -> eax = list[str], split from the right
        push    ebx esi edi ebp
        push    1
        jmp     rt_str_split.go
rt_str_split:                   ; eax = s, edx = separator (0: runs of whitespace), ecx = maxsplit (< 0: no limit) -> eax = list[str]
        push    ebx esi edi ebp
        push    0
.go:    push    ecx             ; [esp] separator, [esp+4] splits left, [esp+8] 1: from the right
        push    edx
        test    edx,edx
        jz      @f
        cmp     dword [edx+8],0
        je      .esep
@@:     mov     ebx,eax
        mov     eax,rt_kd_str
        call    rt_list_new
        mov     ebp,eax
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
        cmp     dword [esp],0
        je      .ws
        cmp     dword [esp+8],0
        jne     .rsep
        xor     ecx,ecx         ; from the left, at a separator
.fs:    cmp     dword [esp+4],0
        je      .fslast
        push    ecx
        mov     edx,[ebx+8]
        mov     edi,[esp+4]
        xor     eax,eax
        call    rt_find_bytes
        pop     ecx
        cmp     eax,-1
        je      .fslast
        mov     edx,eax
        call    .piece
        mov     ecx,edx
        mov     eax,[esp]
        add     ecx,[eax+8]
        dec     dword [esp+4]
        jmp     .fs
.fslast:mov     edx,[ebx+8]
        call    .piece
        jmp     .done
.rsep:  mov     edx,[ebx+8]     ; from the right, at a separator
.rs:    cmp     dword [esp+4],0
        je      .rslast
        push    edx
        xor     ecx,ecx
        mov     edi,[esp+4]
        mov     eax,1
        call    rt_find_bytes
        pop     edx
        cmp     eax,-1
        je      .rslast
        push    eax
        mov     ecx,eax
        mov     edi,[esp+4]
        add     ecx,[edi+8]
        call    .piece          ; [where it is + its length, e)
        pop     edx
        dec     dword [esp+4]
        jmp     .rs
.rslast:xor     ecx,ecx
        call    .piece
        jmp     .rev
.ws:    cmp     dword [esp+8],0
        jne     .rws
.w:     cmp     esi,edi         ; from the left, at whitespace
        jae     .done
        push    esi
        call    rt_u8_next
        call    rt_cp_isspace
        pop     eax
        je      .w
        mov     esi,eax
        cmp     dword [esp+4],0
        je      .wlast
        mov     ecx,esi
.wl:    cmp     esi,edi
        jae     .wend
        push    esi ecx
        call    rt_u8_next
        call    rt_cp_isspace
        pop     ecx eax
        jne     .wl
        mov     esi,eax
.wend:  mov     edx,esi
        lea     eax,[ebx+12]
        sub     ecx,eax
        sub     edx,eax
        call    .piece
        dec     dword [esp+4]
        jmp     .w
.wlast: mov     edx,edi         ; the rest without the whitespace at its end
.wb:    cmp     edx,esi
        jbe     .wbend
        lea     ecx,[edx-1]
.wbk:   cmp     ecx,esi
        jbe     @f
        cmp     byte [rt_bmode],0
        jne     @f
        mov     al,[ecx]
        and     al,0xC0
        cmp     al,0x80
        jne     @f
        dec     ecx
        jmp     .wbk
@@:     push    esi ecx edx
        mov     esi,ecx
        call    rt_u8_next
        call    rt_cp_isspace
        pop     edx ecx esi
        jne     .wbend
        mov     edx,ecx
        jmp     .wb
.wbend: mov     ecx,esi
        lea     eax,[ebx+12]
        sub     ecx,eax
        sub     edx,eax
        call    .piece
        jmp     .done
.rws:   mov     edx,edi         ; from the right, at whitespace
.rw:    cmp     edx,esi
        jbe     .rev
        call    .prev
        jne     .rword
        mov     edx,ecx
        jmp     .rw
.rword: cmp     dword [esp+4],0
        je      .rwlast
        push    edx
.rwl:   cmp     edx,esi
        jbe     .rwe
        call    .prev
        je      .rwe
        mov     edx,ecx
        jmp     .rwl
.rwe:   pop     eax
        mov     ecx,edx
        mov     edx,eax
        lea     eax,[ebx+12]
        sub     ecx,eax
        sub     edx,eax
        call    .piece
        lea     edx,[ebx+12+ecx]
        dec     dword [esp+4]
        jmp     .rw
.rwlast:mov     ecx,esi         ; the rest without the whitespace at its start
.rwa:   cmp     ecx,edx
        jae     .rwae
        push    esi edx
        mov     esi,ecx
        call    rt_u8_next
        push    esi
        call    rt_cp_isspace
        pop     eax
        pop     edx esi
        jne     .rwae
        mov     ecx,eax
        jmp     .rwa
.rwae:  lea     eax,[ebx+12]
        sub     ecx,eax
        sub     edx,eax
        call    .piece
.rev:   mov     eax,ebp
        call    rt_list_reverse
.done:  add     esp,12
        mov     eax,ebp
        pop     ebp edi esi ebx
        ret
.esep:  mov     esi,rt_msg_sep
        jmp     rt_panic_value
.prev:  lea     ecx,[edx-1]     ; edx = a position -> ecx = where the code point before it starts; ZF set when it is whitespace
@@:     cmp     ecx,esi
        jbe     @f
        cmp     byte [rt_bmode],0
        jne     @f
        mov     al,[ecx]
        and     al,0xC0
        cmp     al,0x80
        jne     @f
        dec     ecx
        jmp     @b
@@:     push    esi ecx edx
        mov     esi,ecx
        call    rt_u8_next
        call    rt_cp_isspace
        pop     edx ecx esi
        ret
.piece: push    ecx edx esi edi ; append s[ecx:edx] (byte offsets) to the result
        sub     edx,ecx
        push    edx
        lea     esi,[ebx+12+ecx]
        mov     eax,edx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        push    eax
        mov     eax,ebp
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        pop     edi esi edx ecx
        ret

;;; code rt_str_splitlines : rt_list_new rt_list_push rt_kd_str rt_str_new rt_u8_next
rt_str_splitlines:              ; eax = s, edx = 1: keep the line breaks -> eax = list of its lines
        push    ebx esi edi ebp
        push    edx
        mov     ebx,eax
        mov     eax,rt_kd_str
        call    rt_list_new
        mov     ebp,eax
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
        mov     ecx,esi         ; where the line starts
.l:     cmp     esi,edi
        jae     .tail
        mov     edx,esi
        call    rt_u8_next
        call    .isbreak
        jne     .l
        cmp     eax,13          ; \r\n is one break
        jne     @f
        cmp     esi,edi
        jae     @f
        cmp     byte [esi],10
        jne     @f
        inc     esi
@@:     cmp     dword [esp],0
        je      @f
        mov     edx,esi
@@:     call    .piece
        mov     ecx,esi
        jmp     .l
.tail:  cmp     ecx,edi
        jae     .done
        mov     edx,edi
        call    .piece
.done:  pop     edx
        mov     eax,ebp
        pop     ebp edi esi ebx
        ret
.isbreak:                       ; eax = code point -> ZF set when it ends a line (rt_bmode: \n and \r only)
        cmp     eax,10
        je      .r
        cmp     eax,13
        je      .r
        jb      @f
        cmp     byte [rt_bmode],0
        jne     .r
        jmp     .w
@@:     cmp     byte [rt_bmode],0
        jne     .r
        cmp     eax,10
        jb      .r
        jmp     .y
.w:     cmp     eax,0x1C
        jb      .r
        cmp     eax,0x1E
        jbe     .y
        cmp     eax,0x85
        je      .r
        cmp     eax,0x2028
        je      .r
        cmp     eax,0x2029
.r:     ret
.y:     cmp     eax,eax
        ret
.piece: push    ecx edx esi edi ; append [ecx, edx) (addresses) to the result
        mov     esi,ecx
        sub     edx,ecx
        push    edx
        mov     eax,edx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        push    eax
        mov     eax,ebp
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        pop     edi esi edx ecx
        ret

;;; code rt_str_partition : rt_tuple_new rt_str_new rt_find_bytes rt_panic_value rt_msg_sep
rt_str_rpartition:              ; eax = s, edx = separator, ecx = TD of tuple[str, str, str] -> eax = new tuple, at the last separator
        push    ebx esi edi ebp
        mov     ebp,1
        jmp     rt_str_partition.go
rt_str_partition:               ; eax = s, edx = separator, ecx = TD of tuple[str, str, str] -> eax = new tuple (items 4 bytes apart)
        push    ebx esi edi ebp
        xor     ebp,ebp
.go:    mov     ebx,eax
        mov     edi,edx
        cmp     dword [edi+8],0
        je      .empty
        mov     eax,ecx
        call    rt_tuple_new
        mov     esi,eax
        xor     ecx,ecx
        mov     edx,[ebx+8]
        mov     eax,ebp
        call    rt_find_bytes
        cmp     eax,-1
        je      .nf
        push    eax
        xor     ecx,ecx
        mov     edx,eax
        call    .sub
        mov     [esi+12],eax
        inc     dword [edi]
        mov     [esi+16],edi
        pop     ecx
        add     ecx,[edi+8]
        mov     edx,[ebx+8]
        call    .sub
        mov     [esi+20],eax
        jmp     .out
.nf:    inc     dword [ebx]     ; (s, "", "") or ("", "", s)
        xor     eax,eax
        call    rt_str_new
        mov     [esi+16],eax
        xor     eax,eax
        call    rt_str_new
        test    ebp,ebp
        jnz     @f
        mov     [esi+12],ebx
        mov     [esi+20],eax
        jmp     .out
@@:     mov     [esi+12],eax
        mov     [esi+20],ebx
.out:   mov     eax,esi
        pop     ebp edi esi ebx
        ret
.sub:   push    esi edi         ; s[ecx:edx] (bytes) -> eax = new str
        sub     edx,ecx
        push    edx
        lea     esi,[ebx+12+ecx]
        mov     eax,edx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret
.empty: mov     esi,rt_msg_sep
        jmp     rt_panic_value

;;; code rt_str_remove : rt_str_new
rt_str_remove:                  ; eax = s, edx = affix, ecx = 0 removeprefix / 1 removesuffix -> eax = new reference
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,ecx
        mov     ecx,[edx+8]
        mov     eax,[ebx+8]
        jecxz   .same
        cmp     ecx,eax
        ja      .same
        lea     edi,[ebx+12]
        test    ebp,ebp
        jz      @f
        add     edi,eax
        sub     edi,ecx
@@:     lea     esi,[edx+12]
        push    ecx
        repe    cmpsb
        pop     ecx
        jne     .same
        mov     eax,[ebx+8]
        sub     eax,ecx
        push    eax ecx
        call    rt_str_new
        pop     edx ecx
        lea     esi,[ebx+12]
        test    ebp,ebp
        jnz     @f
        add     esi,edx
@@:     lea     edi,[eax+12]
        rep     movsb
        pop     ebp edi esi ebx
        ret
.same:  inc     dword [ebx]
        mov     eax,ebx
        pop     ebp edi esi ebx
        ret

;;; code rt_str_expandtabs : rt_u8_next rt_sb_bytes rt_sb_char rt_sb_take
rt_str_expandtabs:              ; eax = s, edx = tab size -> eax = new str
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        mov     ebp,edx
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
        xor     ebx,ebx         ; the column
.l:     cmp     esi,edi
        jae     .done
        mov     ecx,esi
        call    rt_u8_next
        cmp     eax,9
        je      .tab
        push    eax
        mov     eax,ecx
        mov     edx,esi
        sub     edx,ecx
        call    rt_sb_bytes
        pop     eax
        inc     ebx
        cmp     eax,10
        je      @f
        cmp     eax,13
        jne     .l
@@:     xor     ebx,ebx
        jmp     .l
.tab:   test    ebp,ebp
        jle     .l
        mov     eax,ebx
        xor     edx,edx
        div     ebp
        mov     ecx,ebp
        sub     ecx,edx         ; to the next stop
        add     ebx,ecx
@@:     push    ecx
        mov     al,' '
        call    rt_sb_char
        pop     ecx
        dec     ecx
        jnz     @b
        jmp     .l
.done:  pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret

;;; code rt_str_join : rt_sb_bytes rt_sb_take
rt_str_join:                    ; eax = separator, edx = list[str] -> eax = new str
        push    ebx esi edi
        push    dword [rt_sb_len]
        mov     esi,eax
        mov     edi,edx
        xor     ebx,ebx
.loop:  test    edi,edi
        jz      .done
        cmp     ebx,[edi+8]
        jae     .done
        test    ebx,ebx
        jz      @f
        test    esi,esi
        jz      @f
        lea     eax,[esi+12]
        mov     edx,[esi+8]
        call    rt_sb_bytes
@@:     mov     eax,[edi+16]
        mov     eax,[eax+ebx*4]
        test    eax,eax
        jz      @f
        mov     edx,[eax+8]
        add     eax,12
        call    rt_sb_bytes
@@:     inc     ebx
        jmp     .loop
.done:  pop     eax
        call    rt_sb_take
        pop     edi esi ebx
        ret

;;; code rt_is_space
rt_is_space:                    ; al = byte -> ZF set when it is ASCII whitespace (number parsing)
        cmp     al,' '
        je      @f
        cmp     al,9
        jb      @f
        cmp     al,13
        ja      @f
        cmp     al,al
@@:     ret

; ---------------------------------------------------------------- bytes
; A bytes object is laid out as a str whose length in code points is its
; length in bytes (rt_bytes_cp makes it so before a str routine measures it).
; The str routines that look at characters (split and strip at whitespace,
; splitlines) work byte by byte with ASCII whitespace while rt_bmode is set.

;;; code rt_bytes_cp
rt_bytes_cp:                    ; eax = bytes: one code point per byte (eax kept, nothing else changes)
        push    ecx
        mov     ecx,[eax+8]
        mov     [eax+13+ecx],ecx
        pop     ecx
        ret

;;; code rt_bytes_new : rt_str_new rt_panic_value
rt_bytes_new:                   ; eax = n -> eax = n zero bytes (ValueError when negative)
        test    eax,eax
        js      .neg
        push    eax
        call    rt_str_new
        pop     ecx
        mov     [eax+13+ecx],ecx
        ret
.neg:   mov     esi,rt_msg_negcount
        jmp     rt_panic_value
;;; data rt_bytes_new
rt_msg_negcount db 'negative count',0

;;; code rt_bytes_from_list : rt_bytes_new rt_panic_value
rt_bytes_from_list:             ; eax = list[int] -> eax = bytes of its values (ValueError outside 0..255)
        push    ebx esi
        mov     ebx,eax
        mov     esi,[ebx+16]
        mov     ecx,[ebx+8]
        xor     edx,edx
.chk:   cmp     edx,ecx
        jae     .ok
        cmp     dword [esi+edx*8+4],0
        jne     .bad
        cmp     dword [esi+edx*8],255
        ja      .bad
        inc     edx
        jmp     .chk
.ok:    mov     eax,ecx
        call    rt_bytes_new
        mov     ecx,[ebx+8]
        xor     edx,edx
.cp:    cmp     edx,ecx
        jae     .out
        push    eax
        mov     eax,[esi+edx*8]
        mov     ebx,[esp]
        mov     [ebx+12+edx],al
        pop     eax
        inc     edx
        jmp     .cp
.out:   pop     esi ebx
        ret
.bad:   mov     esi,rt_msg_brange
        jmp     rt_panic_value
;;; data rt_bytes_from_list
rt_msg_brange   db 'bytes must be in range(0, 256)',0

;;; code rt_bytes_at : rt_panic_index
rt_bytes_at:                    ; eax = bytes, edx = index -> eax = the byte there
        mov     ecx,[eax+8]
        test    edx,edx
        jns     @f
        add     edx,ecx
@@:     cmp     edx,ecx
        jae     rt_panic_index_b
        movzx   eax,byte [eax+12+edx]
        ret

;;; code rt_bytes_byte : rt_chars rt_bytes1 rt_panic_value
rt_bytes_byte:                  ; edx:eax = int -> eax = a static one-byte bytes of it (ValueError outside 0..255)
        test    edx,edx
        jnz     .bad
        cmp     eax,255
        ja      .bad
        cmp     eax,0x80
        jae     @f
        shl     eax,5
        add     eax,rt_chars
        ret
@@:     sub     eax,0x80
        shl     eax,5
        add     eax,rt_bytes1
        ret
.bad:   mov     esi,rt_msg_byte
        jmp     rt_panic_value
;;; data rt_bytes_byte
rt_msg_byte     db 'byte must be in range(0, 256)',0

;;; data rt_bytes1 : rt_static
align 4
rt_bytes1:                      ; the one-byte bytes 0x80..0xFF, static (32 bytes apart)
repeat 128
        dd      0x40000000, rt_static, 1
        db      %+127, 0
        dd      1
        db      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
end repeat

;;; code rt_bytes_has : rt_bytes_byte
rt_bytes_has:                   ; eax = bytes, edx:ecx = int -> eax = 1 when that byte is in it (ValueError outside 0..255)
        push    edi
        push    eax
        mov     eax,ecx
        call    rt_bytes_byte   ; the range check
        pop     edi
        mov     eax,ecx
        mov     ecx,[edi+8]
        add     edi,12
        jecxz   .no
        repne   scasb
        jne     .no
        mov     eax,1
        pop     edi
        ret
.no:    xor     eax,eax
        pop     edi
        ret

;;; code rt_sb_repr_bytes : rt_sb_char
rt_sb_repr_bytes:               ; eax = bytes: Python's repr - b'...' (b"..." if it has ' but no "), escapes
        push    ebx esi edi
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
        mov     ebx,39
        mov     ecx,esi
        xor     edx,edx
.scan:  cmp     ecx,edi
        jae     .chosen
        mov     al,[ecx]
        cmp     al,39
        jne     @f
        mov     dl,1
@@:     cmp     al,'"'
        jne     @f
        mov     dh,1
@@:     inc     ecx
        jmp     .scan
.chosen:
        test    dl,dl
        jz      @f
        test    dh,dh
        jnz     @f
        mov     ebx,'"'
@@:     mov     al,'b'
        call    rt_sb_char
        mov     al,bl
        call    rt_sb_char
.next:  cmp     esi,edi
        jae     .done
        movzx   eax,byte [esi]
        inc     esi
        mov     ah,al
        cmp     al,'\'
        je      .esc
        cmp     al,bl
        je      .esc
        mov     ah,'n'
        cmp     al,10
        je      .esc
        mov     ah,'r'
        cmp     al,13
        je      .esc
        mov     ah,'t'
        cmp     al,9
        je      .esc
        cmp     al,32
        jb      .hex
        cmp     al,127
        jae     .hex
        call    rt_sb_char
        jmp     .next
.esc:   push    eax
        mov     al,'\'
        call    rt_sb_char
        pop     eax
        mov     al,ah
        call    rt_sb_char
        jmp     .next
.hex:   push    eax
        mov     al,'\'
        call    rt_sb_char
        mov     al,'x'
        call    rt_sb_char
        mov     eax,[esp]
        shr     al,4
        call    .digit
        pop     eax
        and     al,15
        call    .digit
        jmp     .next
.digit: add     al,'0'
        cmp     al,'9'
        jbe     @f
        add     al,'a'-'0'-10
@@:     jmp     rt_sb_char
.done:  mov     al,bl
        call    rt_sb_char
        pop     edi esi ebx
        ret

;;; code rt_repr_b : rt_sb_repr_bytes rt_repr_none
rt_repr_b:                      ; eax = &value (bytes or None): its repr appended
        mov     eax,[eax]
        test    eax,eax
        jz      rt_repr_none
        jmp     rt_sb_repr_bytes

;;; code rt_kd_bytes : rt_eq_s rt_cmp_s rt_hash_s rt_repr_b
;;; data rt_kd_bytes
align 4
rt_kd_bytes     dd 4, 1, rt_eq_s, rt_cmp_s, rt_hash_s, rt_repr_b, rt_kd_bytes_n
rt_kd_bytes_n   db 'bytes',0

;;; code rt_bytes_hex : rt_sb_char rt_sb_take
rt_bytes_hex:                   ; eax = bytes -> eax = str of two hex digits per byte
        push    esi edi
        push    dword [rt_sb_len]
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
.l:     cmp     esi,edi
        jae     .done
        movzx   eax,byte [esi]
        inc     esi
        push    eax
        shr     al,4
        call    .digit
        pop     eax
        and     al,15
        call    .digit
        jmp     .l
.digit: add     al,'0'
        cmp     al,'9'
        jbe     @f
        add     al,'a'-'0'-10
@@:     jmp     rt_sb_char
.done:  pop     eax
        call    rt_sb_take
        pop     edi esi
        ret

;;; code rt_bytes_hex_sep : rt_bytes_hex rt_sb_char rt_sb_take rt_panic_value
rt_bytes_hex_sep:               ; eax = bytes, edx = sep (a str or bytes: one ASCII character), ecx = bytes_per_sep
        push    ebx esi edi ebp ;   -> eax = str: bytes.hex(sep, bytes_per_sep)
        push    eax
        mov     esi,edx
        mov     ebx,[esi+8]
        xor     edi,edi
.asc:   cmp     edi,ebx
        jae     .one
        cmp     byte [esi+edi+12],0x80
        jae     .ascii
        inc     edi
        jmp     .asc
.one:   cmp     ebx,1
        jne     .len
        movzx   ebx,byte [esi+12]
        mov     ebp,ecx         ; ebp = bytes in a group
        test    ebp,ebp
        jns     @f
        neg     ebp
@@:     pop     eax
        test    ebp,ebp
        jz      .plain
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
        test    ecx,ecx
        js      .left
        mov     eax,[eax+8]     ; groups counted from the right: the first has n % k bytes (k when 0)
        xor     edx,edx
        div     ebp
        test    edx,edx
        jnz     .start
.left:  mov     edx,ebp
.start: push    dword [rt_sb_len]
        push    edx             ; [esp] = bytes until the next separator
.l:     cmp     esi,edi
        jae     .done
        cmp     dword [esp],0
        jne     .byte
        mov     [esp],ebp
        mov     eax,ebx
        call    rt_sb_char
.byte:  dec     dword [esp]
        movzx   eax,byte [esi]
        inc     esi
        push    eax
        shr     al,4
        call    .digit
        pop     eax
        and     al,15
        call    .digit
        jmp     .l
.digit: add     al,'0'
        cmp     al,'9'
        jbe     @f
        add     al,'a'-'0'-10
@@:     jmp     rt_sb_char
.done:  add     esp,4
        pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret
.plain: pop     ebp edi esi ebx
        jmp     rt_bytes_hex
.ascii: mov     eax,rt_msg_hexasc
        jmp     .fail
.len:   mov     eax,rt_msg_hexlen
.fail:  add     esp,4
        pop     ebp edi esi ebx
        mov     esi,eax
        jmp     rt_panic_value
;;; data rt_bytes_hex_sep
rt_msg_hexasc   db 'sep must be ASCII.',0
rt_msg_hexlen   db 'sep must be length 1.',0

;;; code rt_bytes_fromhex : rt_sb_char rt_sb_take rt_sb_need rt_sb_cstr rt_sb_int rt_raise_valerr rt_str_cpidx rt_bytes_cp
rt_bytes_fromhex:               ; eax = str -> eax = bytes of its pairs of hex digits (whitespace between them)
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        mov     ebp,eax
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
.l:     cmp     esi,edi
        jae     .done
        movzx   eax,byte [esi]
        cmp     al,' '
        je      .skip
        cmp     al,9
        jb      @f
        cmp     al,13
        jbe     .skip
@@:     call    .hexd
        test    eax,eax
        js      .nonhex
        mov     ebx,eax
        inc     esi
        cmp     esi,edi
        jae     .odd
        movzx   eax,byte [esi]
        call    .hexd
        test    eax,eax
        jns     @f
        movzx   eax,byte [esi]
        cmp     al,' '          ; a lone digit before whitespace (or the end)
        je      .odd
        test    al,al
        jz      .odd
        cmp     al,9
        jb      .nonhex
        cmp     al,13
        jbe     .odd
        jmp     .nonhex
@@:     shl     ebx,4
        or      eax,ebx
        call    rt_sb_char
.skip:  inc     esi
        jmp     .l
.done:  pop     eax
        call    rt_sb_take
        call    rt_bytes_cp
        pop     ebp edi esi ebx
        ret
.hexd:  cmp     al,'0'          ; eax = a byte -> its value as a hex digit, or -1
        jb      .nd
        cmp     al,'9'
        jbe     .d09
        or      al,32
        cmp     al,'a'
        jb      .nd
        cmp     al,'f'
        ja      .nd
        sub     al,'a'-10
        ret
.d09:   sub     al,'0'
        ret
.nd:    or      eax,-1
        ret
.odd:   mov     eax,[esp]
        mov     [rt_sb_len],eax
        push    dword [rt_sb_len]
        mov     eax,rt_msg_fromhex_odd
        call    rt_sb_cstr
        jmp     .raise
.nonhex:mov     eax,[esp]
        mov     [rt_sb_len],eax
        mov     edx,esi
        lea     eax,[ebp+12]
        sub     edx,eax
        mov     eax,ebp
        call    rt_str_cpidx
        push    eax
        push    dword [rt_sb_len]
        mov     eax,rt_msg_fromhex_bad
        call    rt_sb_cstr
        mov     eax,[esp+4]
        xor     edx,edx
        call    rt_sb_int
        pop     eax
        pop     ecx
        push    eax
.raise: pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_bytes_fromhex
rt_msg_fromhex_odd db 'fromhex() arg must contain an even number of hexadecimal digits',0
rt_msg_fromhex_bad db 'non-hexadecimal number found in fromhex() arg at position ',0

;;; code rt_bytes_case : rt_str_new
rt_bytes_case:                  ; eax = bytes, edx = 0 upper / 1 lower / 2 swapcase / 3 title / 4 capitalize -> eax = new bytes (ASCII letters only)
        push    ebx esi edi ebp
        mov     esi,eax
        mov     ebx,edx
        mov     eax,[esi+8]
        call    rt_str_new
        push    eax
        mov     ecx,[esi+8]
        mov     [eax+13+ecx],ecx
        lea     edi,[eax+12]
        add     esi,12
        xor     ebp,ebp         ; the previous byte was a letter (title) / past the first (capitalize)
        jecxz   .done
.l:     mov     al,[esi]
        mov     dl,al
        or      dl,32
        sub     dl,'a'
        cmp     dl,25
        ja      .other
        mov     dl,al
        and     dl,32           ; 32: lower case
        cmp     ebx,0
        je      .up
        cmp     ebx,1
        je      .low
        cmp     ebx,2
        jne     @f
        xor     al,32
        jmp     .put
@@:     cmp     ebx,3
        jne     .cap
        test    ebp,ebp
        mov     ebp,1
        jnz     .low
        jmp     .up
.cap:   test    ebp,ebp
        mov     ebp,1
        jnz     .low
.up:    and     al,0xDF
        jmp     .put
.low:   or      al,32
        jmp     .put
.other: cmp     ebx,3
        jne     @f
        xor     ebp,ebp
        jmp     .put
@@:     cmp     ebx,4
        jne     .put
        mov     ebp,1
.put:   mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     .l
.done:  pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_bytes_is
rt_bytes_is:                    ; eax = bytes, edx = 0 isalpha / 1 isdigit / 2 isalnum / 3 isspace / 4 islower / 5 isupper / 6 istitle / 7 isascii -> eax = bool
        push    ebx esi edi ebp
        mov     ebx,edx
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        test    edi,edi
        jz      .empty
        add     edi,esi
        xor     ebp,ebp         ; bit 0: a cased one, bit 1: the previous one cased
.l:     cmp     esi,edi
        jae     .end
        movzx   eax,byte [esi]
        inc     esi
        xor     ecx,ecx         ; ecx: 1 upper, 2 lower, 4 digit, 8 space, 16 not ASCII
        cmp     al,'A'
        jb      @f
        cmp     al,'Z'
        ja      @f
        mov     ecx,1
@@:     cmp     al,'a'
        jb      @f
        cmp     al,'z'
        ja      @f
        mov     ecx,2
@@:     cmp     al,'0'
        jb      @f
        cmp     al,'9'
        ja      @f
        mov     ecx,4
@@:     cmp     al,' '
        je      .sp
        cmp     al,9
        jb      @f
        cmp     al,13
        ja      @f
.sp:    mov     ecx,8
@@:     cmp     al,0x80
        jb      @f
        mov     ecx,16
@@:     cmp     ebx,0
        jne     @f
        test    ecx,3
        jz      .no
        jmp     .l
@@:     cmp     ebx,1
        jne     @f
        test    ecx,4
        jz      .no
        jmp     .l
@@:     cmp     ebx,2
        jne     @f
        test    ecx,7
        jz      .no
        jmp     .l
@@:     cmp     ebx,3
        jne     @f
        test    ecx,8
        jz      .no
        jmp     .l
@@:     cmp     ebx,4
        jne     @f
        test    ecx,1
        jnz     .no
        test    ecx,2
        jz      .l
        or      ebp,1
        jmp     .l
@@:     cmp     ebx,5
        jne     @f
        test    ecx,2
        jnz     .no
        test    ecx,1
        jz      .l
        or      ebp,1
        jmp     .l
@@:     cmp     ebx,6
        jne     .asc
        test    ecx,1
        jz      @f
        test    ebp,2
        jnz     .no
        or      ebp,3
        jmp     .l
@@:     test    ecx,2
        jz      @f
        test    ebp,2
        jz      .no
        or      ebp,3
        jmp     .l
@@:     and     ebp,-3
        jmp     .l
.asc:   test    ecx,16
        jnz     .no
        jmp     .l
.end:   cmp     ebx,4
        jb      .yes
        cmp     ebx,6
        ja      .yes
        mov     eax,ebp
        and     eax,1
        jmp     .out
.empty: cmp     ebx,7
        je      .yes
.no:    xor     eax,eax
        jmp     .out
.yes:   mov     eax,1
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_bytes_split : rt_str_split rt_u8_next rt_panic_value rt_msg_sep
rt_bytes_split:                 ; eax = bytes, edx = separator (0: runs of ASCII whitespace), ecx = maxsplit -> eax = list[bytes]
        call    rt_bytes_sepchk
        mov     byte [rt_bmode],1
        call    rt_str_split
        mov     byte [rt_bmode],0
        ret
rt_bytes_rsplit:                ; the same from the right
        call    rt_bytes_sepchk
        mov     byte [rt_bmode],1
        call    rt_str_rsplit
        mov     byte [rt_bmode],0
        ret
rt_bytes_sepchk:                ; edx = a separator (or 0): ValueError when empty
        test    edx,edx
        jz      @f
        cmp     dword [edx+8],0
        je      .esep
@@:     ret
.esep:  mov     esi,rt_msg_sep
        jmp     rt_panic_value

;;; code rt_bytes_replace : rt_str_replace rt_u8_next
rt_bytes_replace:               ; [esp+4] bytes, old, new, count (< 0: all) -> eax = new bytes
        mov     byte [rt_bmode],1
        push    dword [esp+16]
        push    dword [esp+16]
        push    dword [esp+16]
        push    dword [esp+16]
        call    rt_str_replace
        add     esp,16
        mov     byte [rt_bmode],0
        ret

;;; code rt_bytes_strip : rt_str_strip rt_u8_next
rt_bytes_strip:                 ; eax = bytes, edx = 1 left / 2 right / 3 both, ecx = the bytes to strip (0: ASCII whitespace) -> eax = new bytes
        mov     byte [rt_bmode],1
        call    rt_str_strip
        mov     byte [rt_bmode],0
        ret

;;; code rt_bytes_splitlines : rt_str_splitlines rt_u8_next
rt_bytes_splitlines:            ; eax = bytes, edx = 1: keep the line breaks -> eax = list of its lines (\n, \r, \r\n)
        mov     byte [rt_bmode],1
        call    rt_str_splitlines
        mov     byte [rt_bmode],0
        ret

;;; code rt_raise_unicode : rt_raise
rt_raise_unicode:               ; ecx = message (str, owned), edx = 0 decoding / 1 encoding: raise UnicodeDecodeError / UnicodeEncodeError
if defined rt_throw
        test    edx,edx
        jnz     @f
        mov     eax,VTX_UnicodeDecodeError
        mov     edx,DTX_UnicodeDecodeError
        xor     esi,esi
        jmp     rt_raise_builtin
@@:     mov     eax,VTX_UnicodeEncodeError
        mov     edx,DTX_UnicodeEncodeError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_unidec
        test    edx,edx
        jz      @f
        mov     esi,rt_s_unienc
@@:     jmp     rt_raise
end if
;;; data rt_raise_unicode
rt_s_unidec     db 'UnicodeDecodeError',0
rt_s_unienc     db 'UnicodeEncodeError',0

;;; code rt_raise_lookup : rt_raise
rt_raise_lookup:                ; ecx = message (str, owned): raise LookupError
if defined rt_throw
        mov     eax,VTX_LookupError
        mov     edx,DTX_LookupError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_lookup
        jmp     rt_raise
end if
;;; data rt_raise_lookup
rt_s_lookup     db 'LookupError',0

;;; code rt_codec : rt_sb_need rt_sb_cstr rt_sb_str rt_sb_take rt_raise_lookup
rt_codec:                       ; eax = an encoding's name (str) -> eax = 0 utf-8, 1 ascii, 2 latin-1 (LookupError otherwise)
        push    ebx esi edi
        sub     esp,32
        mov     ebx,eax
        lea     esi,[eax+12]
        mov     ecx,[eax+8]
        cmp     ecx,31
        ja      .unknown
        mov     edi,esp
        jecxz   .copied
.cp:    lodsb                   ; lower case, _ and space as -
        cmp     al,'A'
        jb      @f
        cmp     al,'Z'
        ja      @f
        or      al,32
@@:     cmp     al,'_'
        je      .dash
        cmp     al,' '
        jne     @f
.dash:  mov     al,'-'
@@:     stosb
        dec     ecx
        jnz     .cp
.copied:mov     byte [edi],0
        mov     esi,rt_codec_names
.name:  movzx   eax,byte [esi]  ; code, then the name
        cmp     al,0xFF
        je      .unknown
        inc     esi
        mov     edi,esp
@@:     mov     dl,[esi]
        cmp     dl,[edi]
        jne     .skip
        inc     esi
        inc     edi
        test    dl,dl
        jnz     @b
        add     esp,32
        pop     edi esi ebx
        ret
.skip:  cmp     byte [esi],0
        je      @f
        inc     esi
        jmp     .skip
@@:     inc     esi
        jmp     .name
.unknown:
        add     esp,32
        push    dword [rt_sb_len]
        mov     eax,rt_msg_unkenc
        call    rt_sb_cstr
        mov     eax,ebx
        call    rt_sb_str
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_lookup
;;; data rt_codec
rt_codec_names  db 0,'utf-8',0, 0,'utf8',0, 0,'u8',0, 1,'ascii',0, 1,'us-ascii',0
                db 2,'latin-1',0, 2,'latin1',0, 2,'iso-8859-1',0, 2,'iso8859-1',0, 2,'l1',0, 0xFF
rt_msg_unkenc   db 'unknown encoding: ',0

;;; code rt_errors : rt_str_eq rt_sb_need rt_sb_cstr rt_sb_str rt_sb_char rt_sb_take rt_raise_lookup rt_static
rt_errors:                      ; ecx = an error handler's name (str; 0: strict) -> eax = 0 strict / 1 ignore / 2 replace (LookupError otherwise)
        xor     eax,eax
        test    ecx,ecx
        jz      .out
        push    ecx
        mov     eax,ecx
        mov     edx,rt_s_strict
        call    rt_str_eq
        test    eax,eax
        mov     eax,0
        jnz     .found
        mov     eax,[esp]
        mov     edx,rt_s_ignore
        call    rt_str_eq
        test    eax,eax
        mov     eax,1
        jnz     .found
        mov     eax,[esp]
        mov     edx,rt_s_replace
        call    rt_str_eq
        test    eax,eax
        mov     eax,2
        jnz     .found
        push    dword [rt_sb_len]
        mov     eax,rt_msg_unkerr
        call    rt_sb_cstr
        mov     eax,[esp+4]
        call    rt_sb_str
        mov     al,39
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_lookup
.found: pop     ecx
.out:   ret
;;; data rt_errors
align 4
rt_s_strict     dd 0x40000000,rt_static,6
                db 'strict',0
                dd 6
align 4
rt_s_ignore     dd 0x40000000,rt_static,6
                db 'ignore',0
                dd 6
align 4
rt_s_replace    dd 0x40000000,rt_static,7
                db 'replace',0
                dd 7
rt_msg_unkerr   db "unknown error handler name '",0

;;; code rt_bytes_decode : rt_sb_char rt_sb_bytes rt_sb_cp rt_sb_cstr rt_sb_int rt_sb_take rt_errors rt_raise_unicode
rt_bytes_decode:                ; eax = bytes, edx = codec (rt_codec), ecx = errors (str, 0: strict) -> eax = str
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        push    ecx
        push    edx             ; [esp] codec, [esp+4] errors, [esp+8] the mark
        mov     ebx,eax
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
.l:     cmp     esi,edi
        jae     .done
        movzx   eax,byte [esi]
        cmp     al,0x80
        jb      .one
        cmp     dword [esp],2
        je      .latin
        lea     edx,[esi+1]
        mov     ecx,4           ; ascii: ordinal not in range(128)
        cmp     dword [esp],1
        je      .bad
        call    .seq
        test    eax,eax
        jz      .bad
        push    eax
        mov     edx,eax
        mov     eax,esi
        call    rt_sb_bytes
        pop     eax
        add     esi,eax
        jmp     .l
.one:   call    rt_sb_char
        inc     esi
        jmp     .l
.latin: call    rt_sb_cp
        inc     esi
        jmp     .l
.bad:   push    edx ecx         ; [esi, edx) cannot be decoded, ecx = why
        mov     ecx,[esp+12]
        call    rt_errors
        test    eax,eax
        jz      .strict
        cmp     eax,2
        jne     @f
        mov     eax,0xFFFD
        call    rt_sb_cp
@@:     pop     ecx edx
        mov     esi,edx
        jmp     .l
.done:  add     esp,8
        pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret
.strict:mov     eax,[esp+16]    ; the message, instead of what was decoded
        mov     [rt_sb_len],eax
        push    eax
        mov     al,39
        call    rt_sb_char
        mov     eax,[esp+12]
        mov     eax,[rt_codec_label+eax*4]
        call    rt_sb_cstr
        mov     eax,rt_msg_cantdec
        call    rt_sb_cstr
        mov     edx,[esp+8]     ; the end
        sub     edx,esi
        cmp     edx,1
        jne     .range
        mov     eax,rt_msg_byte0x
        call    rt_sb_cstr
        movzx   eax,byte [esi]
        shr     al,4
        call    .digit
        movzx   eax,byte [esi]
        and     al,15
        call    .digit
        mov     eax,rt_msg_inpos
        call    rt_sb_cstr
        call    .pos
        jmp     .why
.range: mov     eax,rt_msg_bytesinpos
        call    rt_sb_cstr
        call    .pos
        mov     al,'-'
        call    rt_sb_char
        mov     eax,[esp+8]
        lea     edx,[ebx+13]
        sub     eax,edx
        xor     edx,edx
        call    rt_sb_int
.why:   mov     eax,rt_msg_colon
        call    rt_sb_cstr
        mov     eax,[esp+4]     ; why
        mov     eax,[rt_decode_why+eax*4-4]
        call    rt_sb_cstr
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        xor     edx,edx
        jmp     rt_raise_unicode
.pos:   lea     eax,[ebx+12]
        neg     eax
        add     eax,esi
        xor     edx,edx
        jmp     rt_sb_int
.digit: add     al,'0'
        cmp     al,'9'
        jbe     @f
        add     al,'a'-'0'-10
@@:     jmp     rt_sb_char
.seq:   movzx   eax,byte [esi]  ; a valid UTF-8 sequence at esi -> eax = its length; else 0, edx = where the bad part ends, ecx = 1 invalid start / 2 invalid continuation byte / 3 unexpected end
        cmp     al,0xC2
        jb      .start
        cmp     al,0xF5
        jae     .start
        push    ebx
        mov     ecx,1
        mov     bl,0x80
        mov     bh,0xBF
        cmp     al,0xE0
        jb      .go
        inc     ecx
        cmp     al,0xF0
        jae     .four
        cmp     al,0xE0
        jne     @f
        mov     bl,0xA0
@@:     cmp     al,0xED
        jne     .go
        mov     bh,0x9F
        jmp     .go
.four:  inc     ecx
        cmp     al,0xF0
        jne     @f
        mov     bl,0x90
@@:     cmp     al,0xF4
        jne     .go
        mov     bh,0x8F
.go:    mov     edx,1
.k:     lea     eax,[esi+edx]
        cmp     eax,edi
        jae     .eod
        mov     al,[esi+edx]
        cmp     al,bl
        jb      .cont
        cmp     al,bh
        ja      .cont
        mov     bl,0x80
        mov     bh,0xBF
        inc     edx
        cmp     edx,ecx
        jbe     .k
        lea     eax,[ecx+1]
        pop     ebx
        ret
.eod:   mov     edx,edi
        mov     ecx,3
        xor     eax,eax
        pop     ebx
        ret
.cont:  add     edx,esi
        mov     ecx,2
        xor     eax,eax
        pop     ebx
        ret
.start: lea     edx,[esi+1]
        mov     ecx,1
        xor     eax,eax
        ret
;;; data rt_bytes_decode
align 4
rt_codec_label  dd rt_s_utf8, rt_s_ascii, rt_s_latin1
rt_decode_why   dd rt_msg_why1, rt_msg_why2, rt_msg_why3, rt_msg_why4
rt_s_utf8       db 'utf-8',0
rt_s_ascii      db 'ascii',0
rt_s_latin1     db 'latin-1',0
rt_msg_cantdec  db "' codec can't decode ",0
rt_msg_byte0x   db 'byte 0x',0
rt_msg_inpos    db ' in position ',0
rt_msg_bytesinpos db 'bytes in position ',0
rt_msg_colon    db ': ',0
rt_msg_why1     db 'invalid start byte',0
rt_msg_why2     db 'invalid continuation byte',0
rt_msg_why3     db 'unexpected end of data',0
rt_msg_why4     db 'ordinal not in range(128)',0

;;; code rt_str_encode : rt_str_new rt_bytes_cp rt_u8_next rt_sb_char rt_sb_cstr rt_sb_int rt_sb_take rt_errors rt_raise_unicode rt_bytes_decode
rt_str_encode:                  ; eax = str, edx = codec (rt_codec), ecx = errors (str, 0: strict) -> eax = bytes
        push    ebx esi edi ebp
        mov     ebx,eax
        test    edx,edx
        jnz     .narrow
        mov     eax,[ebx+8]     ; UTF-8: the same bytes
        call    rt_str_new
        call    rt_bytes_cp
        mov     ecx,[ebx+8]
        lea     esi,[ebx+12]
        lea     edi,[eax+12]
        rep     movsb
        pop     ebp edi esi ebx
        ret
.narrow:push    dword [rt_sb_len]
        push    ecx
        mov     ebp,0x80        ; the limit: ascii 128, latin-1 256
        cmp     edx,1
        je      @f
        mov     ebp,0x100
@@:     push    edx             ; [esp] codec, [esp+4] errors, [esp+8] the mark
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
        xor     ecx,ecx         ; position (code points)
.l:     cmp     esi,edi
        jae     .done
        call    rt_u8_next
        cmp     eax,ebp
        jae     .bad
        push    ecx
        call    rt_sb_char
        pop     ecx
        inc     ecx
        jmp     .l
.bad:   push    eax             ; [esp] the first, [esp+4] its position after the pushes below
        push    ecx
        lea     edx,[ecx+1]     ; the run of code points it cannot encode
.run:   cmp     esi,edi
        jae     .ran
        push    esi
        call    rt_u8_next
        cmp     eax,ebp
        pop     eax
        jb      .back
        inc     edx
        jmp     .run
.back:  mov     esi,eax
.ran:   push    edx             ; [esp] end, [esp+4] start, [esp+8] the first code point
        mov     ecx,[esp+16]
        call    rt_errors
        test    eax,eax
        jz      .strict
        cmp     eax,2
        jne     .ignore
        mov     ecx,[esp]
        sub     ecx,[esp+4]
@@:     push    ecx
        mov     al,'?'
        call    rt_sb_char
        pop     ecx
        dec     ecx
        jnz     @b
.ignore:pop     ecx
        add     esp,8
        jmp     .l
.done:  add     esp,8
        pop     eax
        call    rt_sb_take
        call    rt_bytes_cp
        pop     ebp edi esi ebx
        ret
.strict:mov     eax,[esp+20]    ; the message, instead of what was encoded
        mov     [rt_sb_len],eax
        push    eax
        mov     al,39
        call    rt_sb_char
        mov     eax,[esp+16]
        mov     eax,[rt_codec_label+eax*4]
        call    rt_sb_cstr
        mov     eax,rt_msg_cantenc
        call    rt_sb_cstr
        mov     eax,[esp+4]
        sub     eax,[esp+8]
        cmp     eax,1
        jne     .range
        mov     eax,rt_msg_character
        call    rt_sb_cstr
        mov     eax,[esp+12]    ; '\xe9', '€', '\U0001f600'
        mov     ecx,2
        mov     dl,'x'
        cmp     eax,0x100
        jb      @f
        mov     ecx,4
        mov     dl,'u'
        cmp     eax,0x10000
        jb      @f
        mov     ecx,8
        mov     dl,'U'
@@:     push    eax ecx
        push    edx
        mov     al,'\'
        call    rt_sb_char
        pop     eax
        call    rt_sb_char
        pop     ecx eax
.hd:    dec     ecx
        push    eax ecx
        shl     ecx,2
        shr     eax,cl
        and     eax,15
        add     al,'0'
        cmp     al,'9'
        jbe     @f
        add     al,'a'-'0'-10
@@:     call    rt_sb_char
        pop     ecx eax
        test    ecx,ecx
        jnz     .hd
        mov     al,39
        call    rt_sb_char
        mov     eax,rt_msg_inpos
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
        jmp     .why
.range: mov     eax,rt_msg_charsinpos
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
        mov     al,'-'
        call    rt_sb_char
        mov     eax,[esp+4]
        dec     eax
        xor     edx,edx
        call    rt_sb_int
.why:   mov     eax,rt_msg_ordinal
        call    rt_sb_cstr
        mov     eax,ebp
        xor     edx,edx
        call    rt_sb_int
        mov     al,')'
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        mov     edx,1
        jmp     rt_raise_unicode
;;; data rt_str_encode
rt_msg_cantenc  db "' codec can't encode ",0
rt_msg_character db "character '",0
rt_msg_charsinpos db 'characters in position ',0
rt_msg_ordinal  db ': ordinal not in range(',0

;;; code rt_bytes_list : rt_list_new rt_list_push rt_kd_int
rt_bytes_list:                  ; eax = bytes -> eax = list[int] of its bytes
        push    ebx esi edi
        mov     esi,eax
        mov     eax,rt_kd_int
        call    rt_list_new
        mov     ebx,eax
        mov     edi,[esi+8]
        add     esi,12
        add     edi,esi
.l:     cmp     esi,edi
        jae     .out
        mov     eax,ebx
        call    rt_list_push
        movzx   ecx,byte [esi]
        mov     [eax],ecx
        mov     dword [eax+4],0
        inc     esi
        jmp     .l
.out:   mov     eax,ebx
        pop     edi esi ebx
        ret

;;; code rt_byteorder : rt_str_eq rt_panic_value rt_static
rt_byteorder:                   ; eax = str -> eax = 0 "big" / 1 "little" (ValueError otherwise)
        push    eax
        mov     edx,rt_s_big
        call    rt_str_eq
        test    eax,eax
        jnz     .big
        mov     eax,[esp]
        mov     edx,rt_s_little
        call    rt_str_eq
        test    eax,eax
        jnz     .little
        mov     esi,rt_msg_byteorder
        jmp     rt_panic_value
.big:   pop     eax
        xor     eax,eax
        ret
.little:pop     eax
        mov     eax,1
        ret
;;; data rt_byteorder
align 4
rt_s_big        dd 0x40000000,rt_static,3
                db 'big',0
                dd 3
align 4
rt_s_little     dd 0x40000000,rt_static,6
                db 'little',0
                dd 6
rt_msg_byteorder db "byteorder must be either 'little' or 'big'",0

;;; code rt_int_from_bytes : rt_int_overflow
rt_int_from_bytes:              ; eax = bytes, edx = 1: little-endian, ecx = 1: signed -> edx:eax = int.from_bytes(...)
        push    ebx esi edi ebp
        mov     esi,eax
        mov     ebp,ecx         ; signed
        mov     ecx,[esi+8]     ; n
        lea     esi,[esi+12]
        sub     esp,8           ; [esp]: the low 8 bytes, little-endian
        mov     dword [esp],0
        mov     dword [esp+4],0
        xor     ebx,ebx         ; byte k (from the least significant)
.l:     cmp     ebx,ecx
        jae     .got
        mov     eax,ebx         ; where byte k is
        test    edx,edx
        jnz     @f
        mov     eax,ecx
        sub     eax,ebx
        dec     eax
@@:     movzx   eax,byte [esi+eax]
        cmp     ebx,8
        jae     .extra
        mov     [esp+ebx],al
        inc     ebx
        jmp     .l
.extra: mov     edi,0           ; beyond 8 bytes: 0x00 (or 0xFF when signed and negative)
        test    ebp,ebp
        jz      @f
        test    byte [esp+7],0x80
        jz      @f
        mov     edi,0xFF
@@:     cmp     eax,edi
        jne     .ovf
        inc     ebx
        jmp     .l
.got:   test    ebp,ebp
        jnz     .signed
        test    byte [esp+7],0x80       ; unsigned: at most 2**63 - 1
        jnz     .ovf
        jmp     .out
.signed:cmp     ecx,8
        jae     .out
        jecxz   .out
        test    byte [esp+ecx-1],0x80   ; sign-extend n < 8 bytes
        jz      .out
        mov     ebx,ecx
@@:     mov     byte [esp+ebx],0xFF
        inc     ebx
        cmp     ebx,8
        jb      @b
.out:   pop     eax edx
        pop     ebp edi esi ebx
        ret
.ovf:   jmp     rt_int_overflow

;;; code rt_int_to_bytes : rt_bytes_new rt_ovferr_text rt_panic_value
rt_int_to_bytes:                ; [esp+4] 1: signed, 1: little-endian, length, the int (8 bytes) -> eax = n.to_bytes(...)
        push    ebx esi edi ebp ; (args from [esp+20]: signed, little, length, int)
        mov     eax,[esp+32]
        mov     edx,[esp+36]
        mov     ecx,[esp+28]
        test    ecx,ecx
        js      .neglen
        cmp     dword [esp+20],0
        jne     .sig
        test    edx,edx         ; unsigned: not negative, below 2**(8 * length)
        js      .negun
        cmp     ecx,8
        jae     .fits
        call    .shift
        or      eax,edx
        jnz     .big
        jmp     .fits
.sig:   cmp     ecx,8
        jae     .fits
        jecxz   .zerolen
        dec     ecx             ; signed: v >> (8 * length - 1) is 0 or -1
        shl     ecx,3
        add     ecx,7
        call    .sar
        mov     ecx,eax
        and     ecx,edx
        cmp     ecx,-1
        je      .fits
        or      eax,edx
        jnz     .big
        jmp     .fits
.zerolen:
        or      eax,edx         ; length 0: only 0 fits
        jnz     .big
.fits:  mov     eax,[esp+28]
        call    rt_bytes_new
        mov     ebx,eax
        mov     ecx,[esp+28]
        xor     esi,esi         ; byte k from the least significant: 0..7 from the int, then its sign
.l:     cmp     esi,ecx
        jae     .done
        xor     eax,eax
        cmp     dword [esp+36],0
        jns     @f
        mov     al,0xFF
@@:     cmp     esi,8
        jae     @f
        mov     al,[esp+32+esi]
@@:     mov     edx,esi
        cmp     dword [esp+24],0
        jne     @f
        mov     edx,ecx
        sub     edx,esi
        dec     edx
@@:     mov     [ebx+12+edx],al
        inc     esi
        jmp     .l
.done:  mov     eax,ebx
        pop     ebp edi esi ebx
        ret
.shift: mov     ebx,ecx         ; edx:eax >>= 8 * ecx (ecx < 8), logical
        shl     ebx,3
.sh:    test    ebx,ebx
        jz      .shr
        shrd    eax,edx,1
        shr     edx,1
        dec     ebx
        jmp     .sh
.shr:   ret
.sar:   test    ecx,ecx         ; edx:eax >>= ecx, arithmetic
        jz      .sr
        shrd    eax,edx,1
        sar     edx,1
        dec     ecx
        jmp     .sar
.sr:    ret
.neglen:mov     esi,rt_msg_neglen
        jmp     rt_panic_value
.negun: mov     esi,rt_msg_negun
        jmp     rt_ovferr_text
.big:   mov     esi,rt_msg_toobig
        jmp     rt_ovferr_text
;;; data rt_int_to_bytes
rt_msg_neglen   db 'length argument must be non-negative',0
rt_msg_negun    db "can't convert negative int to unsigned",0
rt_msg_toobig   db 'int too big to convert',0

;;; code rt_int_bitlen
rt_int_bitlen:                  ; edx:eax = int -> eax = (abs(int)).bit_length()
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     mov     ecx,64
        test    edx,edx
        jnz     .hi
        mov     ecx,32
        mov     edx,eax
.hi:    test    edx,edx
        jz      .zero
@@:     test    edx,0x80000000
        jnz     .out
        shl     edx,1
        dec     ecx
        jmp     @b
.zero:  xor     ecx,ecx
.out:   mov     eax,ecx
        ret

;;; code rt_int_bitcount
rt_int_bitcount:                ; edx:eax = int -> eax = the ones in abs(int)
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     xor     ecx,ecx
.l:     test    eax,1
        jz      @f
        inc     ecx
@@:     shrd    eax,edx,1
        shr     edx,1
        test    eax,eax
        jnz     .l
        test    edx,edx
        jnz     .l
        mov     eax,ecx
        ret

; ---------------------------------------------------------------- string builder
; One growing buffer for formatting (print, str(), %, f-strings). Users take
; the current length as a mark, append, and then take or flush [mark, length).

;;; code rt_sb_need : rt_alloc rt_free
rt_sb_need:                     ; eax = n -> eax = where to put n more bytes
        push    ebx esi edi
        mov     ebx,eax
        mov     ecx,[rt_sb_len]
        add     ecx,eax
        cmp     ecx,[rt_sb_cap]
        jbe     .ok
        mov     edx,[rt_sb_cap]
        add     edx,edx
        cmp     edx,ecx
        jae     @f
        mov     edx,ecx
@@:     cmp     edx,256
        jae     @f
        mov     edx,256
@@:     push    edx
        mov     eax,edx
        call    rt_alloc
        mov     edi,eax
        mov     esi,[rt_sb_buf]
        mov     ecx,[rt_sb_len]
        rep     movsb
        mov     edi,eax
        mov     eax,[rt_sb_buf]
        mov     [rt_sb_buf],edi
        call    rt_free
        pop     edx
        mov     [rt_sb_cap],edx
.ok:    mov     eax,[rt_sb_buf]
        add     eax,[rt_sb_len]
        add     [rt_sb_len],ebx
        pop     edi esi ebx
        ret
;;; bss rt_sb_need
rt_sb_buf       rd 1
rt_sb_len       rd 1
rt_sb_cap       rd 1

;;; code rt_sb_bytes : rt_sb_need
rt_sb_bytes:                    ; eax = bytes, edx = count
        push    esi edi
        mov     esi,eax
        push    edx
        mov     eax,edx
        call    rt_sb_need
        pop     ecx
        mov     edi,eax
        rep     movsb
        pop     edi esi
        ret

;;; code rt_sb_char : rt_sb_need
rt_sb_char:                     ; al = byte
        push    eax
        mov     eax,1
        call    rt_sb_need
        pop     ecx
        mov     [eax],cl
        ret

;;; code rt_sb_str : rt_sb_bytes
rt_sb_str:                      ; eax = str (or 0)
        test    eax,eax
        jz      @f
        mov     edx,[eax+8]
        add     eax,12
        jmp     rt_sb_bytes
@@:     ret

;;; code rt_sb_cstr : rt_sb_bytes
rt_sb_cstr:                     ; eax = NUL-terminated bytes
        mov     edx,eax
@@:     cmp     byte [edx],0
        je      @f
        inc     edx
        jmp     @b
@@:     sub     edx,eax
        jmp     rt_sb_bytes

;;; code rt_sb_json_str : rt_sb_char
rt_sb_json_str:                 ; eax = str (0: ""), edx = 1: non-ASCII as \uXXXX -> a JSON string literal
        push    ebx esi edi ebp
        mov     ebp,edx
        xor     ebx,ebx
        test    eax,eax
        jz      @f
        lea     esi,[eax+12]
        mov     ebx,[eax+8]
@@:     mov     al,'"'
        call    rt_sb_char
.next:  test    ebx,ebx
        jz      .done
        movzx   eax,byte [esi]
        inc     esi
        dec     ebx
        cmp     al,'"'
        je      .esc
        cmp     al,'\'
        je      .esc
        cmp     al,0x20
        jb      .ctrl
        cmp     al,0x7F
        jb      .plain
        ja      .high
        test    ebp,ebp                 ; DEL: \u007f when ensure_ascii (as Python's json)
        jz      .plain
        call    .u4
        jmp     .next
.high:  test    ebp,ebp
        jz      .plain
        cmp     al,0xE0                 ; UTF-8: the code point
        jb      .two
        cmp     al,0xF0
        jb      .three
        and     eax,0x07
        mov     edi,3
        jmp     .cont
.three: and     eax,0x0F
        mov     edi,2
        jmp     .cont
.two:   and     eax,0x1F
        mov     edi,1
.cont:  test    edi,edi
        jz      .cp
        test    ebx,ebx
        jz      .cp
        movzx   ecx,byte [esi]
        and     ecx,0x3F
        shl     eax,6
        or      eax,ecx
        inc     esi
        dec     ebx
        dec     edi
        jmp     .cont
.cp:    cmp     eax,0x10000
        jb      .bmp
        sub     eax,0x10000             ; a surrogate pair
        push    eax
        shr     eax,10
        add     eax,0xD800
        call    .u4
        pop     eax
        and     eax,0x3FF
        add     eax,0xDC00
.bmp:   call    .u4
        jmp     .next
.plain: call    rt_sb_char
        jmp     .next
.esc:   push    eax
        mov     al,'\'
        call    rt_sb_char
        pop     eax
        call    rt_sb_char
        jmp     .next
.ctrl:  mov     ecx,'n'
        cmp     al,10
        je      .short
        mov     ecx,'r'
        cmp     al,13
        je      .short
        mov     ecx,'t'
        cmp     al,9
        je      .short
        mov     ecx,'b'
        cmp     al,8
        je      .short
        mov     ecx,'f'
        cmp     al,12
        je      .short
        call    .u4
        jmp     .next
.short: push    ecx
        mov     al,'\'
        call    rt_sb_char
        pop     eax
        call    rt_sb_char
        jmp     .next
.done:  mov     al,'"'
        call    rt_sb_char
        pop     ebp edi esi ebx
        ret
.u4:    push    eax                     ; eax = a UTF-16 unit -> \uxxxx
        mov     al,'\'
        call    rt_sb_char
        mov     al,'u'
        call    rt_sb_char
        mov     ecx,12
.hex:   mov     eax,[esp]
        shr     eax,cl
        and     eax,15
        mov     al,[rt_json_hex+eax]
        push    ecx
        call    rt_sb_char
        pop     ecx
        sub     ecx,4
        jns     .hex
        pop     eax
        ret
;;; data rt_sb_json_str
rt_json_hex     db '0123456789abcdef'

;;; code rt_sb_jfloat : rt_sb_float rt_sb_cstr
rt_sb_jfloat:                   ; st0 (popped) -> a JSON number (as Python's json: NaN, Infinity, -Infinity)
        sub     esp,8
        fst     qword [esp]
        mov     eax,[esp+4]
        and     eax,0x7FF00000
        cmp     eax,0x7FF00000
        jne     .num
        fstp    st0
        mov     eax,rt_json_nan
        test    dword [esp+4],0x000FFFFF
        jnz     .out
        cmp     dword [esp],0
        jne     .out
        mov     eax,rt_json_inf
        test    dword [esp+4],0x80000000
        jz      .out
        mov     eax,rt_json_ninf
.out:   add     esp,8
        jmp     rt_sb_cstr
.num:   add     esp,8
        jmp     rt_sb_float
;;; data rt_sb_jfloat
rt_json_inf     db 'Infinity',0
rt_json_ninf    db '-Infinity',0
rt_json_nan     db 'NaN',0

;;; code rt_sb_repr_str : rt_sb_char rt_sb_bytes rt_u8_next rt_cp_printable
rt_sb_repr_str:                 ; eax = str: Python's repr - 'text' ("text" if it has ' but no "), escapes
        push    ebx esi edi ebp
        xor     esi,esi
        xor     edi,edi
        test    eax,eax
        jz      @f
        lea     esi,[eax+12]
        mov     edi,[eax+8]
@@:     add     edi,esi
        mov     ebx,39
        mov     ecx,esi
        xor     edx,edx         ; dl: has ', dh: has "
.scan:  cmp     ecx,edi
        jae     .chosen
        mov     al,[ecx]
        cmp     al,39
        jne     @f
        mov     dl,1
@@:     cmp     al,'"'
        jne     @f
        mov     dh,1
@@:     inc     ecx
        jmp     .scan
.chosen:
        test    dl,dl
        jz      @f
        test    dh,dh
        jnz     @f
        mov     ebx,'"'
@@:     mov     al,bl
        call    rt_sb_char
.next:  cmp     esi,edi
        jae     .done
        mov     ebp,esi         ; where its bytes start
        call    rt_u8_next
        cmp     eax,'\'
        je      .self
        cmp     eax,ebx
        je      .self
        mov     ecx,'n'
        cmp     eax,10
        je      .esc
        mov     ecx,'r'
        cmp     eax,13
        je      .esc
        mov     ecx,'t'
        cmp     eax,9
        je      .esc
        cmp     eax,0x80
        jae     .wide
        cmp     eax,32
        jb      .hex
        cmp     eax,127
        je      .hex
.copy:  mov     eax,ebp
        mov     edx,esi
        sub     edx,ebp
        call    rt_sb_bytes
        jmp     .next
.wide:  push    eax
        call    rt_cp_printable
        test    eax,eax
        pop     eax
        jnz     .copy
        mov     ecx,4
        mov     edx,'u'
        cmp     eax,0x100
        jae     @f
.hex:   mov     ecx,2
        mov     edx,'x'
@@:     cmp     eax,0x10000
        jb      .hexn
        mov     ecx,8
        mov     edx,'U'
.hexn:  push    eax ecx edx     ; \x.., \u...., \U........
        mov     al,'\'
        call    rt_sb_char
        pop     eax
        call    rt_sb_char
        pop     ecx eax
.hd:    dec     ecx
        push    eax ecx
        shl     ecx,2
        shr     eax,cl
        and     eax,15
        add     al,'0'
        cmp     al,'9'
        jbe     @f
        add     al,'a'-'0'-10
@@:     call    rt_sb_char
        pop     ecx eax
        test    ecx,ecx
        jnz     .hd
        jmp     .next
.self:  mov     ecx,eax
.esc:   push    ecx
        mov     al,'\'
        call    rt_sb_char
        pop     eax
        call    rt_sb_char
        jmp     .next
.done:  mov     al,bl
        call    rt_sb_char
        pop     ebp edi esi ebx
        ret

;;; code rt_sb_bool : rt_sb_cstr
rt_sb_bool:                     ; eax = bool
        test    eax,eax
        mov     eax,rt_s_true
        jnz     @f
        mov     eax,rt_s_false
@@:     jmp     rt_sb_cstr
;;; data rt_sb_bool
rt_s_true       db 'True',0
rt_s_false      db 'False',0

;;; code rt_sb_take : rt_str_new
rt_sb_take:                     ; eax = mark -> eax = new str of [mark, length); length = mark
        push    esi edi
        mov     ecx,[rt_sb_len]
        sub     ecx,eax
        mov     [rt_sb_len],eax
        mov     esi,[rt_sb_buf]
        add     esi,eax
        push    ecx
        mov     eax,ecx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret

;;; code rt_sb_flush : rt_write
rt_sb_flush:                    ; eax = mark: write [mark, length) to stdout; length = mark
        mov     edx,[rt_sb_len]
        sub     edx,eax
        mov     [rt_sb_len],eax
        mov     ecx,[rt_sb_buf]
        add     ecx,eax
        test    edx,edx
        jnz     rt_write
        ret

;;; code rt_sb_funcval : rt_sb_str rt_sb_cstr
rt_sb_funcval:                  ; eax = function value: its name if it is "<class '...'>" (a type made a function), else <function>
        test    eax,eax
        jz      .plain
        mov     ecx,[eax+12]
        test    ecx,ecx
        jz      .plain
        cmp     dword [ecx+8],8
        jb      .plain
        cmp     dword [ecx+12],0x616C633C       ; "<cla"
        jne     .plain
        mov     eax,ecx
        jmp     rt_sb_str
.plain: mov     eax,rt_msg_funcval
        jmp     rt_sb_cstr
;;; data rt_sb_funcval
rt_msg_funcval  db '<function>',0

;;; code rt_sb_flush_err : rt_write_err
rt_sb_flush_err:                ; eax = mark: write [mark, length) to stderr; length = mark
        mov     edx,[rt_sb_len]
        sub     edx,eax
        mov     [rt_sb_len],eax
        mov     ecx,[rt_sb_buf]
        add     ecx,eax
        test    edx,edx
        jnz     rt_write_err
        ret

;;; code rt_sb_pad : rt_sb_need rt_u8_next rt_u8_put
rt_sb_pad:                      ; eax = mark of a field, ecx = width (code points), edx = 0 right / 1 left / 2 centre / 3 centre as str.center | 4: a sign stays in front | fill code point << 8 (0: space): pad [mark, length)
        push    ebx esi edi ebp
        sub     esp,16          ; [esp] padding, [esp+4] the fill's bytes, [esp+8] their count, [esp+12] width
        mov     ebx,eax
        mov     ebp,edx
        mov     [esp+12],ecx
        mov     esi,[rt_sb_buf]
        mov     edi,[rt_sb_len]
        add     edi,esi
        add     esi,ebx
        xor     ecx,ecx
.cnt:   cmp     esi,edi         ; the field's code points
        jae     .counted
        inc     ecx
        cmp     byte [esi],0xC0
        jae     @f
        inc     esi
        jmp     .cnt
@@:     call    rt_u8_next
        jmp     .cnt
.counted:
        mov     eax,[esp+12]
        sub     eax,ecx
        jle     .out
        mov     [esp],eax
        mov     eax,ebp
        shr     eax,8
        jnz     @f
        mov     eax,' '
@@:     lea     edi,[esp+4]
        call    rt_u8_put
        lea     eax,[esp+4]
        sub     edi,eax
        mov     [esp+8],edi
        mov     eax,[esp]
        xor     edx,edx         ; how much goes in front
        mov     ecx,ebp
        and     ecx,3
        jnz     @f
        mov     edx,eax
        jmp     .split
@@:     cmp     ecx,1
        je      .split
        mov     edx,eax
        shr     edx,1
        cmp     ecx,3
        jne     .split
        mov     ecx,eax         ; str.center: one more in front when padding and width are odd
        and     ecx,[esp+12]
        and     ecx,1
        add     edx,ecx
.split: push    edx             ; [esp] front ([esp+4] padding, [esp+8] fill, [esp+12] its bytes)
        mov     eax,[esp+4]
        imul    eax,[esp+12]
        mov     esi,[rt_sb_len]
        sub     esi,ebx         ; the field's bytes
        call    rt_sb_need
        mov     edi,[rt_sb_buf]
        add     edi,ebx
        mov     eax,[esp]
        imul    eax,[esp+12]
        mov     ecx,esi
        jecxz   .moved
        push    esi edi         ; move the field behind the front padding
        lea     esi,[edi+ecx-1]
        lea     edi,[esi+eax]
        std
        rep     movsb
        cld
        pop     edi esi
.moved: mov     ecx,[esp]
        call    .fill
        add     edi,esi
        mov     ecx,[esp+4]
        sub     ecx,[esp]
        call    .fill
        test    ebp,4           ; a sign goes before the padding
        jz      .done
        mov     edi,[rt_sb_buf]
        add     edi,ebx
        mov     ecx,[esp]
        imul    ecx,[esp+12]
        mov     al,[edi+ecx]
        cmp     al,'-'
        je      @f
        cmp     al,'+'
        je      @f
        cmp     al,' '
        jne     .done
@@:     push    edi
        lea     esi,[edi+ecx-1]
        lea     edi,[edi+ecx]
        std
        rep     movsb
        cld
        pop     edi
        mov     [edi],al
.done:  add     esp,4
.out:   add     esp,16
        pop     ebp edi esi ebx
        ret
.fill:  jecxz   .fr             ; ecx copies of the fill at edi
        push    esi
.f1:    lea     esi,[esp+16]
        push    ecx
        mov     ecx,[esp+24]
        rep     movsb
        pop     ecx
        dec     ecx
        jnz     .f1
        pop     esi
.fr:    ret

;;; code rt_sb_trunc : rt_u8_next
rt_sb_trunc:                    ; eax = mark, edx = n: keep the first n code points of [mark, length)
        push    esi edi
        mov     esi,[rt_sb_buf]
        mov     edi,[rt_sb_len]
        add     edi,esi
        add     esi,eax
        mov     ecx,edx
.l:     test    ecx,ecx
        jz      .cut
        cmp     esi,edi
        jae     .out
        call    rt_u8_next
        dec     ecx
        jmp     .l
.cut:   sub     esi,[rt_sb_buf]
        mov     [rt_sb_len],esi
.out:   pop     edi esi
        ret

;;; code rt_sb_sign : rt_sb_need
rt_sb_sign:                     ; eax = mark of a number, dl = sign char: put it in front unless the number is negative
        push    esi edi ebx
        mov     ebx,eax
        mov     esi,[rt_sb_buf]
        cmp     ebx,[rt_sb_len]
        jae     @f
        cmp     byte [esi+ebx],'-'
        je      .out
@@:     push    edx
        mov     eax,1
        call    rt_sb_need
        mov     ecx,[rt_sb_len]
        dec     ecx             ; the old length
        mov     esi,[rt_sb_buf]
        lea     edi,[esi+ecx]
        lea     esi,[edi-1]
        sub     ecx,ebx
        std
        rep     movsb
        cld
        pop     edx
        mov     esi,[rt_sb_buf]
        mov     [esi+ebx],dl
.out:   pop     ebx edi esi
        ret

;;; code rt_print_str : rt_write
rt_print_str:                   ; eax = str: print it and a newline
        test    eax,eax
        jz      .nl
        mov     edx,[eax+8]
        test    edx,edx
        jz      .nl
        lea     ecx,[eax+12]
        call    rt_write
.nl:    mov     ecx,rt_nl
        mov     edx,1
        jmp     rt_write
;;; data rt_print_str
rt_nl           db 10

; ---------------------------------------------------------------- floats (x87)

;;; code rt_ext_call
; Compiled code computes in double precision, like Python (the x87 is set to
; round to 53 bits at start); formatting, parsing and pow need the full 64-bit
; mantissa internally. `push routine` + `jmp rt_ext_call` runs it that way.
rt_ext_call:
        sub     esp,4           ; [esp] saved control word, [esp+4] routine, [esp+8] return address
        fnstcw  [esp]
        push    dword [esp]
        or      word [esp],0x0300
        fldcw   [esp]
        add     esp,4
        call    dword [esp+4]
        fldcw   [esp]
        add     esp,8
        ret

;;; code rt_scale10
rt_scale10:                     ; st0 = v, eax = k -> st0 = v * 10^k
        push    eax
        fld1
        mov     ecx,eax
        test    ecx,ecx
        jns     @f
        neg     ecx
@@:     push    10
        fild    dword [esp]
        add     esp,4
        jecxz   .done
.mul:   fmul    st1,st0
        dec     ecx
        jnz     .mul
.done:  fstp    st0
        pop     eax
        test    eax,eax
        js      .div
        fmulp   st1,st0
        ret
.div:   fdivp   st1,st0
        ret

;;; code rt_sb_gen : rt_sb_bytes rt_sb_char rt_scale10 rt_ext_call
; st0 = value (popped). edx = significant digits (1..17); ecx bit 0: add ".0"
; when the text looks like an integer, bit 1: fixed notation up to 1e16 (both:
; Python's float printing), bit 2: scientific keeping trailing zeros (%.*e with
; P-1 decimals), bit 3: one unit more in the last digit. Otherwise printf %.*g.
rt_sb_gen:
        push    rt_sb_gen_x          ; in extended precision
        jmp     rt_ext_call
rt_sb_gen_x:
        push    ebx esi edi ebp
        sub     esp,64          ; +0 value, +8 exponent, +12 control words, +16 mantissa, +24 digits wanted, +28 flags, +32 text
        mov     [esp+24],edx
        mov     [esp+28],ecx
        fst     qword [esp]
        mov     eax,[esp+4]
        and     eax,0x7FF00000
        cmp     eax,0x7FF00000
        jne     .finite
        fstp    st0
        mov     eax,[esp+4]
        and     eax,0x000FFFFF
        or      eax,[esp]
        mov     eax,rt_s_nan
        jnz     .word
        test    byte [esp+7],0x80
        mov     eax,rt_s_inf
        jz      .word
        mov     eax,rt_s_minf
.word:  mov     edx,eax
@@:     cmp     byte [edx],0
        je      @f
        inc     edx
        jmp     @b
@@:     sub     edx,eax
        call    rt_sb_bytes
        jmp     .out
.finite:
        ftst
        fnstsw  ax
        sahf
        jne     .nonzero
        fstp    st0
        test    byte [esp+7],0x80
        jz      @f
        mov     al,'-'
        call    rt_sb_char
@@:     mov     al,'0'
        call    rt_sb_char
        jmp     .dotzero
.nonzero:
        jae     .pos
        fchs
        mov     al,'-'
        call    rt_sb_char
.pos:   fld     st0             ; exponent = floor(log10(v))
        fldlg2
        fxch
        fyl2x
        fnstcw  [esp+12]
        mov     ax,[esp+12]
        and     ax,0xF3FF
        or      ax,0x0400
        mov     [esp+14],ax
        fldcw   [esp+14]
        fistp   dword [esp+8]
        fldcw   [esp+12]
        mov     ebx,3           ; tries to land the mantissa in [10^(P-1), 10^P)
.scale: mov     eax,[esp+24]
        dec     eax
        sub     eax,[esp+8]
        fld     st0
        call    rt_scale10
        fistp   qword [esp+16]
        dec     ebx
        jz      .ok
        fild    qword [esp+16]
        fld1
        mov     eax,[esp+24]
        dec     eax
        call    rt_scale10
        fcomp   st1
        fnstsw  ax
        sahf
        jbe     @f
        fstp    st0
        dec     dword [esp+8]
        jmp     .scale
@@:     fld1
        mov     eax,[esp+24]
        call    rt_scale10
        fcomp   st1
        fnstsw  ax
        sahf
        fstp    st0
        ja      .ok
        inc     dword [esp+8]
        jmp     .scale
.ok:    fstp    st0
        test    dword [esp+28],8
        jz      .nobump
        add     dword [esp+16],1          ; one unit more (10^P: 10^(P-1), the exponent one more)
        adc     dword [esp+20],0
        mov     eax,[esp+24]
        mov     edx,dword [rt_pow10q+eax*8+4]
        cmp     edx,[esp+20]
        jne     .nobump
        mov     edx,dword [rt_pow10q+eax*8]
        cmp     edx,[esp+16]
        jne     .nobump
        mov     edx,dword [rt_pow10q+eax*8-8]
        mov     [esp+16],edx
        mov     edx,dword [rt_pow10q+eax*8-4]
        mov     [esp+20],edx
        inc     dword [esp+8]
.nobump:
        mov     ecx,[esp+24]    ; P digits of the mantissa into the text buffer
        lea     edi,[esp+32]
        add     edi,ecx
        mov     esi,[esp+16]
        mov     ebp,[esp+20]
        mov     ebx,10
.dig:   mov     eax,ebp
        xor     edx,edx
        div     ebx
        mov     ebp,eax
        mov     eax,esi
        div     ebx
        mov     esi,eax
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        dec     ecx
        jnz     .dig
        mov     ecx,[esp+24]    ; drop trailing zeros
        test    dword [esp+28],4
        jnz     .form
.tz:    cmp     ecx,1
        jbe     .form
        cmp     byte [esp+32+ecx-1],'0'
        jne     .form
        dec     ecx
        jmp     .tz
.form:  mov     ebx,ecx         ; ebx = significant digits
        mov     eax,[esp+8]
        test    dword [esp+28],4
        jnz     .sci
        cmp     eax,-4
        jl      .sci
        mov     edx,[esp+24]
        test    dword [esp+28],2
        jz      @f
        mov     edx,16
@@:     cmp     eax,edx
        jge     .sci
        test    eax,eax
        js      .small
        lea     edx,[eax+1]     ; integer digits
        cmp     edx,ebx
        jbe     @f
        lea     eax,[esp+32]
        mov     edx,ebx
        call    rt_sb_bytes
        mov     ecx,[esp+8]
        inc     ecx
        sub     ecx,ebx
.zeros: push    ecx
        mov     al,'0'
        call    rt_sb_char
        pop     ecx
        dec     ecx
        jnz     .zeros
        jmp     .dotzero
@@:     push    edx
        lea     eax,[esp+36]
        call    rt_sb_bytes
        pop     edx
        cmp     edx,ebx
        je      .dotzero
        push    edx
        mov     al,'.'
        call    rt_sb_char
        pop     edx
        lea     eax,[esp+32+edx]
        sub     ebx,edx
        mov     edx,ebx
        call    rt_sb_bytes
        jmp     .out
.small: mov     al,'0'          ; 0.000ddd
        call    rt_sb_char
        mov     al,'.'
        call    rt_sb_char
        mov     ecx,[esp+8]
        not     ecx             ; -e-1 zeros
        jecxz   @f
.z2:    push    ecx
        mov     al,'0'
        call    rt_sb_char
        pop     ecx
        dec     ecx
        jnz     .z2
@@:     lea     eax,[esp+32]
        mov     edx,ebx
        call    rt_sb_bytes
        jmp     .out
.sci:   lea     eax,[esp+32]    ; d[.ddd]e+XX
        mov     edx,1
        call    rt_sb_bytes
        cmp     ebx,1
        jbe     @f
        mov     al,'.'
        call    rt_sb_char
        lea     eax,[esp+33]
        lea     edx,[ebx-1]
        call    rt_sb_bytes
@@:     mov     al,'e'
        call    rt_sb_char
        mov     eax,[esp+8]
        mov     dl,'+'
        test    eax,eax
        jns     @f
        neg     eax
        mov     dl,'-'
@@:     push    eax
        mov     al,dl
        call    rt_sb_char
        pop     eax
        cmp     eax,10
        jae     @f
        push    eax
        mov     al,'0'
        call    rt_sb_char
        pop     eax
@@:     lea     edi,[esp+64]
        mov     ecx,10
.e:     xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        test    eax,eax
        jnz     .e
        mov     eax,edi
        lea     edx,[esp+64]
        sub     edx,edi
        call    rt_sb_bytes
        jmp     .out
.dotzero:
        test    dword [esp+28],1
        jz      .out
        mov     al,'.'
        call    rt_sb_char
        mov     al,'0'
        call    rt_sb_char
.out:   add     esp,64
        pop     ebp edi esi ebx
        ret
;;; data rt_sb_gen
align 8
rt_pow10q       dq 1,10,100,1000,10000,100000,1000000,10000000,100000000,1000000000,10000000000
                dq 100000000000,1000000000000,10000000000000,100000000000000,1000000000000000
                dq 10000000000000000,100000000000000000
rt_s_nan        db 'nan',0
rt_s_inf        db 'inf',0
rt_s_minf       db '-inf',0

;;; code rt_sb_float : rt_sb_gen rt_sb_take rt_sb_str rt_float_parse rt_free
rt_sb_float:                    ; st0 = value (popped): Python's repr - the shortest text that reads back the same
        push    ebx esi edi
        sub     esp,16          ; +0 value, +8 value read back
        fstp    qword [esp]
        mov     esi,15
        xor     edi,edi         ; edi 8: the candidate one unit up
        mov     eax,[esp+4]
        and     eax,0x7FF00000
        cmp     eax,0x7FF00000
        je      .last           ; inf, nan
        test    eax,eax
        jnz     .try
        mov     esi,1           ; a subnormal: fewer digits than 15 may read back the same
.try:   mov     ebx,[rt_sb_len]
        fld     qword [esp]
        mov     edx,esi
        mov     ecx,3
        or      ecx,edi
        call    rt_sb_gen
        cmp     esi,17
        jae     .done
        mov     eax,ebx         ; read the text back
        call    rt_sb_take
        push    eax
        call    rt_float_parse
        fstp    qword [esp+12]
        mov     eax,[esp+12]
        cmp     eax,[esp+4]
        jne     .no
        mov     eax,[esp+16]
        cmp     eax,[esp+8]
        jne     .no
        mov     eax,[esp]
        call    rt_sb_str
        pop     eax
        call    rt_free
        jmp     .done
.no:    pop     eax
        call    rt_free
        test    edi,edi
        jnz     .next
        fld     qword [esp+8]   ; read back below the value (an exact tie rounded down):
        fabs                    ; the candidate one unit up may read back
        fld     qword [esp]
        fabs
        fcompp
        fnstsw  ax
        sahf
        jbe     .next
        mov     edi,8
        jmp     .try
.next:  xor     edi,edi
        inc     esi
        jmp     .try
.last:  fld     qword [esp]
        mov     edx,esi
        mov     ecx,3
        call    rt_sb_gen
.done:  add     esp,16
        pop     edi esi ebx
        ret

;;; code rt_sb_fixed : rt_sb_char rt_sb_bytes rt_scale10 rt_sb_gen rt_ext_call
rt_sb_fixed:                    ; st0 = value (popped), edx = digits after the point (0..40): printf %.*f
        push    rt_sb_fixed_x          ; in extended precision
        jmp     rt_ext_call
rt_sb_fixed_x:
        push    ebx esi edi ebp
        sub     esp,96          ; +0 value, +8 mantissa, +16 digits after the point, +32..96 text
        mov     [esp+16],edx
        fst     qword [esp]
        mov     eax,[esp+4]
        and     eax,0x7FF00000
        cmp     eax,0x7FF00000
        je      .gen
        ftst
        fnstsw  ax
        sahf
        jae     @f
        fchs
        mov     al,'-'
        call    rt_sb_char
@@:     mov     eax,[esp+16]
        call    rt_scale10
        push    0x5E000000      ; 2^61: larger values fall back to %g
        fcom    dword [esp]
        add     esp,4
        fnstsw  ax
        sahf
        jae     .big
        fistp   qword [esp+8]
        lea     edi,[esp+96]    ; digits of the mantissa, backwards
        mov     esi,[esp+8]
        mov     ebp,[esp+12]
        mov     ebx,10
        xor     ecx,ecx
.dig:   mov     eax,ebp
        xor     edx,edx
        div     ebx
        mov     ebp,eax
        mov     eax,esi
        div     ebx
        mov     esi,eax
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        inc     ecx
        mov     eax,esi
        or      eax,ebp
        jnz     .dig
.pad:   cmp     ecx,[esp+16]    ; at least one digit before the point
        ja      .print
        dec     edi
        mov     byte [edi],'0'
        inc     ecx
        jmp     .pad
.print: mov     ebx,ecx
        sub     ebx,[esp+16]    ; digits before the point
        mov     eax,edi
        mov     edx,ebx
        call    rt_sb_bytes
        cmp     dword [esp+16],0
        je      .out
        mov     al,'.'
        call    rt_sb_char
        lea     eax,[edi+ebx]
        mov     edx,[esp+16]
        call    rt_sb_bytes
        jmp     .out
.big:   mov     eax,[esp+16]    ; undo the scaling
        neg     eax
        call    rt_scale10
.gen:   mov     edx,17
        xor     ecx,ecx
        call    rt_sb_gen
.out:   add     esp,96
        pop     ebp edi esi ebx
        ret

; ---------------------------------------------------------------- 64-bit integers
; Python's int in compiled code: 64 bits, in edx:eax (in memory: the low dword
; first). A result that does not fit raises OverflowError, as in the
; interpreter. Binary operators take the left operand on the stack ([esp+4])
; and the right one in edx:eax; the caller drops the left one.

;;; code rt_ovferr_text : rt_panic
rt_ovferr_text:                 ; esi = message (C text): raise OverflowError
if defined rt_throw
        mov     eax,VTX_OverflowError
        mov     edx,DTX_OverflowError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_ovferr
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_ovferr_text
rt_s_ovferr     db 'OverflowError: ',0

;;; code rt_int_overflow : rt_ovferr_text
rt_int_overflow:                ; an int result too big
        mov     esi,rt_msg_iovf
        jmp     rt_ovferr_text
;;; data rt_int_overflow
rt_msg_iovf     db 'integer overflow (ints are 64-bit)',0

;;; code rt_i32sat
rt_i32sat:                      ; edx:eax -> eax = the same int as 32 bits, or the nearest 32-bit one (indices, counts)
        push    ecx
        mov     ecx,eax
        sar     ecx,31
        cmp     ecx,edx
        je      .out
        mov     eax,0x7FFFFFFF
        test    edx,edx
        jns     .out
        mov     eax,0x80000000
.out:   pop     ecx
        ret

;;; code rt_ineg : rt_int_overflow
rt_ineg:                        ; edx:eax = -edx:eax
        cmp     edx,0x80000000
        jne     @f
        test    eax,eax
        jz      rt_int_overflow
@@:     neg     eax
        adc     edx,0
        neg     edx
        ret

;;; code rt_iabs : rt_ineg
rt_iabs:                        ; edx:eax = abs(edx:eax)
        test    edx,edx
        js      rt_ineg
        ret

;;; code rt_imul : rt_int_overflow
rt_imul:                        ; [esp+4] a, edx:eax b -> edx:eax = a * b
        push    ebx esi edi ebp
        mov     ebx,[esp+20]    ; |a| -> esi:ebx, ebp = the result's sign
        mov     esi,[esp+24]
        xor     ebp,ebp
        test    esi,esi
        jns     @f
        neg     ebx
        adc     esi,0
        neg     esi
        xor     ebp,1
@@:     test    edx,edx         ; |b| -> edx:eax
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
        xor     ebp,1
@@:     test    esi,esi
        jz      .alo
        test    edx,edx
        jnz     .ovf            ; both at least 2**32
        mov     ecx,eax         ; |a| has a high part, |b| does not
        mov     eax,esi
        mul     ecx
        test    edx,edx
        jnz     .ovf
        mov     edi,eax
        mov     eax,ebx
        mul     ecx
        add     edx,edi
        jc      .ovf
        jmp     .sign
.alo:   mov     edi,eax         ; |a| < 2**32
        mov     eax,edx
        mul     ebx
        test    edx,edx
        jnz     .ovf
        mov     ecx,eax
        mov     eax,edi
        mul     ebx
        add     edx,ecx
        jc      .ovf
.sign:  test    edx,edx         ; the product (unsigned) fits if < 2**63, or is 2**63 and negative
        js      .big
        test    ebp,ebp
        jz      .out
        neg     eax
        adc     edx,0
        neg     edx
.out:   pop     ebp edi esi ebx
        ret
.big:   test    ebp,ebp
        jz      .ovf
        cmp     edx,0x80000000
        jne     .ovf
        test    eax,eax
        jz      .out
.ovf:   pop     ebp edi esi ebx
        jmp     rt_int_overflow

;;; code rt_udivmod
rt_udivmod:                     ; edx:eax / ecx:ebx (unsigned, not 0) -> edx:eax = quotient, ecx:ebx = remainder
        test    ecx,ecx
        jnz     .big
        push    esi             ; a 32-bit divisor: two divisions
        mov     esi,eax
        mov     eax,edx
        xor     edx,edx
        div     ebx
        mov     ecx,eax
        mov     eax,esi
        div     ebx
        mov     ebx,edx
        mov     edx,ecx
        xor     ecx,ecx
        pop     esi
        ret
.big:   push    esi edi ebp     ; shift and subtract
        mov     ebp,64
        xor     esi,esi         ; the remainder: edi:esi
        xor     edi,edi
.l:     shld    edi,esi,1
        shld    esi,edx,1
        shld    edx,eax,1
        shl     eax,1
        cmp     edi,ecx
        jb      .n
        ja      .sub
        cmp     esi,ebx
        jb      .n
.sub:   sub     esi,ebx
        sbb     edi,ecx
        or      eax,1
.n:     dec     ebp
        jnz     .l
        mov     ebx,esi
        mov     ecx,edi
        pop     ebp edi esi
        ret

;;; code rt_idivmod : rt_udivmod rt_panic_zero rt_int_overflow rt_scratch
; Python's floor division: the quotient rounds toward minus infinity, the
; remainder has the divisor's sign
rt_idivmod:                     ; [esp+4] a, edx:eax b -> edx:eax = a // b, rt_scratch = a % b
        push    ebx esi edi ebp
        push    edx
        push    eax             ; [esp] b
        mov     ebx,eax
        mov     ecx,edx
        or      eax,edx
        jz      .zero
        mov     eax,[esp+28]    ; a
        mov     edx,[esp+32]
        cmp     ecx,-1          ; -2**63 // -1
        jne     @f
        cmp     ebx,-1
        jne     @f
        cmp     edx,0x80000000
        jne     @f
        test    eax,eax
        jz      .ovf
@@:     mov     esi,edx         ; the signs
        mov     edi,ecx
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     test    ecx,ecx
        jns     @f
        neg     ebx
        adc     ecx,0
        neg     ecx
@@:     call    rt_udivmod
        test    esi,esi         ; the remainder of |a| / |b| takes a's sign
        jns     @f
        neg     ebx
        adc     ecx,0
        neg     ecx
@@:     xor     esi,edi
        jns     .done
        neg     eax             ; signs differ: a negative quotient ...
        adc     edx,0
        neg     edx
        mov     ebp,ebx
        or      ebp,ecx
        jz      .done
        sub     eax,1           ; ... and, not exact, one less (the remainder + b)
        sbb     edx,0
        add     ebx,[esp]
        adc     ecx,[esp+4]
.done:  mov     [rt_scratch],ebx
        mov     [rt_scratch+4],ecx
        add     esp,8
        pop     ebp edi esi ebx
        ret
.zero:  add     esp,8
        pop     ebp edi esi ebx
        jmp     rt_panic_zero
.ovf:   add     esp,8
        pop     ebp edi esi ebx
        jmp     rt_int_overflow

;;; code rt_ipow : rt_imul rt_panic_value
rt_ipow:                        ; [esp+4] base, edx:eax exponent (>= 0) -> edx:eax = base ** exponent
        test    edx,edx
        js      .neg
        push    ebx esi edi ebp
        mov     esi,eax         ; the exponent: edi:esi
        mov     edi,edx
        push    dword [esp+24]  ; [esp+8] base (squared as needed)
        push    dword [esp+24]
        push    0               ; [esp] result = 1
        push    1
.l:     mov     eax,esi
        or      eax,edi
        jz      .done
        test    esi,1
        jz      .sq
        push    dword [esp+4]   ; result *= base
        push    dword [esp+4]
        mov     eax,[esp+16]
        mov     edx,[esp+20]
        call    rt_imul
        add     esp,8
        mov     [esp],eax
        mov     [esp+4],edx
.sq:    shrd    esi,edi,1
        shr     edi,1
        mov     eax,esi
        or      eax,edi
        jz      .done
        push    dword [esp+12]  ; base *= base
        push    dword [esp+12]
        mov     eax,[esp+16]
        mov     edx,[esp+20]
        call    rt_imul
        add     esp,8
        mov     [esp+8],eax
        mov     [esp+12],edx
        jmp     .l
.done:  pop     eax
        pop     edx
        add     esp,8
        pop     ebp edi esi ebx
        ret
.neg:   mov     esi,rt_msg_ipowneg
        jmp     rt_panic_value
;;; data rt_ipow
rt_msg_ipowneg  db 'negative exponent: int ** int with a negative exponent is a float (not in compiled code; use float ** int)',0

;;; code rt_sar64
rt_sar64:                       ; edx:eax >>= cl (0..63), arithmetic
        cmp     cl,32
        jae     @f
        shrd    eax,edx,cl
        sar     edx,cl
        ret
@@:     mov     eax,edx
        sar     edx,31
        sub     cl,32
        sar     eax,cl
        ret

;;; code rt_ishl : rt_shl64 rt_sar64 rt_int_overflow rt_panic_value
rt_ishl:                        ; [esp+4] a, edx:eax n -> edx:eax = a << n
        test    edx,edx
        js      .neg
        push    ebx esi
        mov     ebx,[esp+12]
        mov     esi,[esp+16]
        mov     ecx,eax
        mov     eax,ebx
        or      eax,esi
        jz      .zero
        test    edx,edx
        jnz     .ovf
        cmp     ecx,63
        ja      .ovf
        mov     eax,ebx
        mov     edx,esi
        push    ecx
        call    rt_shl64
        pop     ecx
        push    edx
        push    eax
        call    rt_sar64        ; shifted back: the same value unless bits were lost
        cmp     eax,ebx
        jne     .ovf2
        cmp     edx,esi
        jne     .ovf2
        pop     eax
        pop     edx
        pop     esi ebx
        ret
.zero:  xor     eax,eax
        xor     edx,edx
        pop     esi ebx
        ret
.ovf2:  add     esp,8
.ovf:   pop     esi ebx
        jmp     rt_int_overflow
.neg:   mov     esi,rt_msg_negshift
        jmp     rt_panic_value
;;; data rt_ishl
rt_msg_negshift db 'negative shift count',0

;;; code rt_isar : rt_sar64 rt_ishl
rt_isar:                        ; [esp+4] a, edx:eax n -> edx:eax = a >> n
        test    edx,edx
        js      .neg
        mov     ecx,eax
        jnz     .all
        cmp     eax,63
        jbe     @f
.all:   mov     eax,[esp+8]     ; every bit shifted out: the sign
        sar     eax,31
        mov     edx,eax
        ret
@@:     mov     eax,[esp+4]
        mov     edx,[esp+8]
        jmp     rt_sar64
.neg:   mov     esi,rt_msg_negshift
        jmp     rt_panic_value

;;; code rt_utoa
rt_utoa:                        ; edx:eax = value (unsigned), ecx = base, edi = end of a buffer -> edi = first digit (written backwards)
        push    ebx esi
        mov     esi,edx
.l:     mov     ebx,eax
        mov     eax,esi
        xor     edx,edx
        div     ecx
        mov     esi,eax
        mov     eax,ebx
        div     ecx
        add     dl,'0'
        cmp     dl,'9'
        jbe     .d
        add     dl,'a'-'9'-1
.d:     dec     edi
        mov     [edi],dl
        mov     edx,eax
        or      edx,esi
        jnz     .l
        pop     esi ebx
        ret

;;; code rt_sb_int : rt_utoa rt_sb_bytes
rt_sb_int:                      ; edx:eax = int: append it in decimal
        push    edi ebx
        sub     esp,24
        lea     edi,[esp+24]
        mov     ebx,edx
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     mov     ecx,10
        call    rt_utoa
        test    ebx,ebx
        jns     @f
        dec     edi
        mov     byte [edi],'-'
@@:     mov     eax,edi
        lea     edx,[esp+24]
        sub     edx,edi
        call    rt_sb_bytes
        add     esp,24
        pop     ebx edi
        ret

;;; code rt_sb_radix : rt_utoa rt_sb_bytes
rt_sb_radix:                    ; edx:eax = int, ecx = base | 0x100 for upper case: append it in that base
        push    ebx edi esi
        mov     esi,ecx
        sub     esp,68
        lea     edi,[esp+68]
        mov     ebx,edx
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     mov     ecx,esi
        and     ecx,0xFF
        call    rt_utoa
        test    ebx,ebx
        jns     .case
        dec     edi
        mov     byte [edi],'-'
.case:  test    esi,0x100
        jz      .out
        mov     ecx,edi
        lea     edx,[esp+68]
@@:     cmp     ecx,edx
        jae     .out
        cmp     byte [ecx],'a'
        jb      .n
        sub     byte [ecx],32
.n:     inc     ecx
        jmp     @b
.out:   mov     eax,edi
        lea     edx,[esp+68]
        sub     edx,edi
        call    rt_sb_bytes
        add     esp,68
        pop     esi edi ebx
        ret

;;; code rt_sb_group : rt_sb_need
rt_sb_group:                    ; eax = mark of a number in the builder, dl = separator, ecx = digits per group (3, 4): separators into its integer part
        push    ebx esi edi ebp
        push    edx                     ; [esp] the separator
        mov     ebp,ecx
        mov     esi,[rt_sb_buf]
        mov     ebx,eax                 ; where the digits start
        cmp     ebx,[rt_sb_len]
        jae     .out
        mov     al,[esi+ebx]
        cmp     al,'-'
        je      .sign
        cmp     al,'+'
        je      .sign
        cmp     al,' '
        jne     .scan
.sign:  inc     ebx
.scan:  mov     ecx,ebx                 ; where they end
.d:     cmp     ecx,[rt_sb_len]
        jae     .have
        mov     al,[esi+ecx]
        cmp     al,'0'
        jb      .have
        cmp     al,'9'
        jbe     .dig
        cmp     ebp,4
        jne     .have
        or      al,0x20
        cmp     al,'a'
        jb      .have
        cmp     al,'f'
        ja      .have
.dig:   inc     ecx
        jmp     .d
.have:  mov     eax,ecx
        sub     eax,ebx
        jz      .out
        dec     eax
        xor     edx,edx
        div     ebp                     ; separators
        test    eax,eax
        jz      .out
        push    ecx
        push    eax
        call    rt_sb_need              ; room for them
        pop     eax
        pop     ecx
        mov     edx,[rt_sb_len]
        sub     edx,eax                 ; the old length
        push    eax
        push    ecx
        mov     esi,[rt_sb_buf]         ; what follows the digits moves right
        mov     edi,esi
        add     esi,edx
        dec     esi
        add     edi,[rt_sb_len]
        dec     edi
        sub     edx,ecx
        mov     ecx,edx
        std
        rep     movsb
        cld
        pop     ecx
        pop     eax
        mov     edx,[rt_sb_buf]         ; the digits, from the last, a separator after each group
        lea     esi,[ecx-1]
        lea     edi,[ecx+eax-1]
        xor     eax,eax
.cp:    cmp     esi,ebx
        jl      .out
        mov     cl,[edx+esi]
        mov     [edx+edi],cl
        dec     esi
        dec     edi
        inc     eax
        cmp     eax,ebp
        jne     .cp
        cmp     esi,ebx
        jl      .out
        mov     cl,[esp]
        mov     [edx+edi],cl
        dec     edi
        xor     eax,eax
        jmp     .cp
.out:   pop     edx
        pop     ebp edi esi ebx
        ret

;;; code rt_print_int : rt_utoa rt_write
rt_print_int:                   ; edx:eax = int: print it and a newline
        push    edi ebx
        sub     esp,24
        lea     edi,[esp+23]
        mov     byte [edi],10
        mov     ebx,edx
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     mov     ecx,10
        call    rt_utoa
        test    ebx,ebx
        jns     @f
        dec     edi
        mov     byte [edi],'-'
@@:     mov     ecx,edi
        lea     edx,[esp+24]
        sub     edx,edi
        call    rt_write
        add     esp,24
        pop     ebx edi
        ret

;;; code rt_int_parse : rt_is_space rt_sb_need rt_sb_cstr rt_sb_repr_str rt_sb_take rt_raise_valerr rt_ovferr_text
rt_int_parse:                   ; eax = str -> edx:eax = int(str): spaces around, a sign, _ between digits (CPython's)
        push    ebx esi edi ebp
        push    eax             ; the str, for the message
        test    eax,eax
        jz      .bad
        mov     ecx,[eax+8]
        lea     esi,[eax+12]
        lea     edi,[esi+ecx]
.lead:  cmp     esi,edi
        jae     .bad
        mov     al,[esi]
        call    rt_is_space
        jne     @f
        inc     esi
        jmp     .lead
@@:     xor     ebp,ebp         ; negative?
        cmp     al,'-'
        jne     @f
        inc     ebp
        inc     esi
        jmp     .first
@@:     cmp     al,'+'
        jne     .first
        inc     esi
.first: xor     eax,eax         ; the value (unsigned, at most 2**63)
        xor     edx,edx
        cmp     esi,edi
        jae     .bad
        movzx   ecx,byte [esi]
        sub     ecx,'0'
        cmp     ecx,9
        ja      .bad
.digit: cmp     esi,edi
        jae     .done
        movzx   ecx,byte [esi]
        cmp     cl,'_'
        je      .us
        sub     ecx,'0'
        cmp     ecx,9
        ja      .tail
        push    ecx             ; value = value*10 + digit
        mov     ecx,10
        mov     ebx,eax
        mov     eax,edx
        mul     ecx
        test    edx,edx
        jnz     .ovf1
        push    eax
        mov     eax,ebx
        mul     ecx
        pop     ebx
        add     edx,ebx
        jc      .ovf1
        pop     ecx
        add     eax,ecx
        adc     edx,0
        jc      .ovf
        cmp     edx,0x80000000
        ja      .ovf
        jb      @f
        test    eax,eax
        jnz     .ovf
@@:     inc     esi
        jmp     .digit
.us:    inc     esi             ; _: a digit must follow
        cmp     esi,edi
        jae     .bad
        movzx   ecx,byte [esi]
        sub     ecx,'0'
        cmp     ecx,9
        ja      .bad
        jmp     .digit
.tail:  cmp     esi,edi
        jae     .done
        push    eax
        mov     al,[esi]
        call    rt_is_space
        pop     eax
        jne     .bad
        inc     esi
        jmp     .tail
.done:  test    ebp,ebp
        jz      .pos
        neg     eax
        adc     edx,0
        neg     edx
        jmp     .out
.pos:   test    edx,edx         ; +2**63 does not fit
        js      .ovf
.out:   add     esp,4
        pop     ebp edi esi ebx
        ret
.ovf1:  pop     ecx
.ovf:   mov     esi,rt_msg_ibig
        jmp     rt_ovferr_text
.bad:   push    dword [rt_sb_len]
        mov     eax,rt_msg_int
        call    rt_sb_cstr
        mov     eax,[esp+4]
        call    rt_sb_repr_str
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_int_parse
rt_msg_int      db 'invalid literal for int() with base 10: ',0
rt_msg_ibig     db 'int too large (ints are 64-bit)',0

;;; code rt_int_parse_base : rt_is_space rt_sb_cstr rt_sb_char rt_sb_int rt_sb_repr_str rt_sb_repr_bytes rt_sb_take rt_raise_valerr rt_ovferr_text rt_panic_value
rt_int_parse_base:              ; eax = str (bytes: edx 1), ecx = base (0 or 2..36) -> edx:eax = int(x, base):
        push    ebx esi edi ebp ;   spaces around, a sign, 0x/0o/0b, _ between digits (CPython's)
        push    edx             ; [esp+8] bytes?
        push    eax             ; [esp+4] the object, for the message
        push    ecx             ; [esp] the base as given
        cmp     ecx,1
        je      .badbase
        cmp     ecx,36
        ja      .badbase
        mov     ebx,ecx
        test    eax,eax
        jz      .bad
        mov     ecx,[eax+8]
        lea     esi,[eax+12]
        lea     edi,[esi+ecx]
.lead:  cmp     esi,edi
        jae     .bad
        mov     al,[esi]
        call    rt_is_space
        jne     @f
        inc     esi
        jmp     .lead
@@:     xor     ebp,ebp         ; ebp bit 0 negative, 1 after a prefix, 2 base 0 decimal, 3 a leading 0 of base 0
        cmp     al,'-'
        jne     @f
        inc     ebp
        inc     esi
        jmp     .pfx
@@:     cmp     al,'+'
        jne     .pfx
        inc     esi
.pfx:   mov     ecx,edi
        sub     ecx,esi
        cmp     ecx,2
        jb      .nopfx
        cmp     byte [esi],'0'
        jne     .nopfx
        mov     al,[esi+1]
        or      al,0x20
        mov     edx,16
        cmp     al,'x'
        je      .haspfx
        mov     edx,8
        cmp     al,'o'
        je      .haspfx
        mov     edx,2
        cmp     al,'b'
        je      .haspfx
        jmp     .nopfx
.haspfx:
        test    ebx,ebx
        jz      @f
        cmp     ebx,edx
        jne     .nopfx          ; (int('0b1', 16): hex digits)
@@:     mov     ebx,edx
        add     esi,2
        or      ebp,2
        cmp     esi,edi
        jae     .bad
        cmp     byte [esi],'_'  ; (0x_1f)
        jne     .first
        inc     esi
        jmp     .first
.nopfx: test    ebx,ebx
        jnz     .first
        mov     ebx,10
        or      ebp,4
.first: xor     eax,eax
        xor     edx,edx
        cmp     esi,edi
        jae     .bad
        movzx   ecx,byte [esi]
        call    .val
        cmp     ecx,ebx
        jae     .bad
        test    ebp,4
        jz      .digit
        test    ecx,ecx
        jnz     .digit
        or      ebp,8           ; base 0: a leading 0 makes it 0 (zeros only)
.digit: cmp     esi,edi
        jae     .done
        movzx   ecx,byte [esi]
        cmp     cl,'_'
        je      .us
        call    .val
        cmp     ecx,ebx
        jae     .tail
        test    ebp,8
        jz      @f
        test    ecx,ecx
        jnz     .bad
@@:     push    ecx             ; value = value*base + digit
        mov     ecx,eax
        mov     eax,edx
        mul     ebx
        test    edx,edx
        jnz     .ovf1
        push    eax
        mov     eax,ecx
        mul     ebx
        pop     ecx
        add     edx,ecx
        jc      .ovf1
        pop     ecx
        add     eax,ecx
        adc     edx,0
        jc      .ovf
        cmp     edx,0x80000000
        ja      .ovf
        jb      @f
        test    eax,eax
        jnz     .ovf
@@:     inc     esi
        jmp     .digit
.us:    inc     esi             ; _: a digit must follow
        cmp     esi,edi
        jae     .bad
        movzx   ecx,byte [esi]
        call    .val
        cmp     ecx,ebx
        jae     .bad
        jmp     .digit
.tail:  cmp     esi,edi
        jae     .done
        push    eax
        mov     al,[esi]
        call    rt_is_space
        pop     eax
        jne     .bad
        inc     esi
        jmp     .tail
.done:  test    ebp,1
        jz      .pos
        neg     eax
        adc     edx,0
        neg     edx
        jmp     .out
.pos:   test    edx,edx
        js      .ovf
.out:   add     esp,12
        pop     ebp edi esi ebx
        ret
.val:   cmp     ecx,'0'         ; ecx = a character -> its digit (99: none)
        jb      .none
        cmp     ecx,'9'
        jbe     .dec
        or      ecx,0x20
        cmp     ecx,'a'
        jb      .none
        cmp     ecx,'z'
        ja      .none
        sub     ecx,'a'-10
        ret
.dec:   sub     ecx,'0'
        ret
.none:  mov     ecx,99
        ret
.ovf1:  pop     ecx
.ovf:   mov     esi,rt_msg_ibig_b
        jmp     rt_ovferr_text
.badbase:
        mov     esi,rt_msg_intbase
        jmp     rt_panic_value
.bad:   push    dword [rt_sb_len]
        mov     eax,rt_msg_intb
        call    rt_sb_cstr
        mov     eax,[esp+4]
        cdq
        call    rt_sb_int
        mov     al,':'
        call    rt_sb_char
        mov     al,' '
        call    rt_sb_char
        mov     eax,[esp+8]
        cmp     dword [esp+12],0
        jne     .rb
        call    rt_sb_repr_str
        jmp     .rd
.rb:    call    rt_sb_repr_bytes
.rd:    pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_int_parse_base
rt_msg_intb     db 'invalid literal for int() with base ',0
rt_msg_intbase  db 'int() base must be >= 2 and <= 36, or 0',0
rt_msg_ibig_b   db 'int too large (ints are 64-bit)',0

;;; code rt_fround : rt_ftoi
;;; code rt_ftoi : rt_ovferr_text rt_panic_value
; float -> int, as CPython's int(x) (truncating) and round(x) (halves to even)
rt_ftoi:                        ; st0 (popped) -> edx:eax = int(st0)
        mov     ecx,0x0C00
        jmp     rt_f2int
rt_fround:                      ; st0 (popped) -> edx:eax = round(st0)
        xor     ecx,ecx
rt_f2int:                       ; ecx = the rounding control bits
        sub     esp,12
        fld     st0
        fcomp   st0
        fnstsw  ax
        sahf
        jp      .nan
        fld     st0
        fabs
        fcomp   qword [rt_f2p63]
        fnstsw  ax
        sahf
        jb      .ok
        fld     st0
        fabs
        fcomp   qword [rt_finf]
        fnstsw  ax
        sahf
        je      .inf
        fcom    qword [rt_fm2p63]
        fnstsw  ax
        sahf
        jne     .big
.ok:    fnstcw  [esp]
        mov     ax,[esp]
        and     ax,0xF3FF
        or      ax,cx
        mov     [esp+2],ax
        fldcw   [esp+2]
        fistp   qword [esp+4]
        fldcw   [esp]
        mov     eax,[esp+4]
        mov     edx,[esp+8]
        add     esp,12
        ret
.nan:   fstp    st0
        add     esp,12
        mov     esi,rt_msg_fnan
        jmp     rt_panic_value
.inf:   fstp    st0
        add     esp,12
        mov     esi,rt_msg_finf
        jmp     rt_ovferr_text
.big:   fstp    st0
        add     esp,12
        mov     esi,rt_msg_ibig2
        jmp     rt_ovferr_text
;;; data rt_ftoi
align 8
rt_f2p63        dd 0,0x43E00000         ; 2**63
rt_fm2p63       dd 0,0xC3E00000         ; -2**63
rt_finf         dd 0,0x7FF00000
rt_msg_fnan     db 'cannot convert float NaN to integer',0
rt_msg_finf     db 'cannot convert float infinity to integer',0
rt_msg_ibig2    db 'int too large (ints are 64-bit)',0

;;; code rt_gcd : rt_udivmod
rt_gcd:                         ; [esp+4] a, edx:eax b -> edx:eax = gcd(a, b) (not negative)
        push    ebx esi edi
        mov     ebx,eax         ; |b| -> ecx:ebx
        mov     ecx,edx
        test    ecx,ecx
        jns     @f
        neg     ebx
        adc     ecx,0
        neg     ecx
@@:     mov     eax,[esp+16]    ; |a| -> edx:eax
        mov     edx,[esp+20]
        test    edx,edx
        jns     .l
        neg     eax
        adc     edx,0
        neg     edx
.l:     mov     esi,ebx         ; while b: a, b = b, a % b
        or      esi,ecx
        jz      .out
        push    ecx
        push    ebx
        call    rt_udivmod      ; ecx:ebx = a % b
        pop     eax
        pop     edx             ; a = the old b
        jmp     .l
.out:   pop     edi esi ebx
        ret

;;; code rt_isqrt : rt_panic_value
rt_isqrt:                       ; edx:eax -> edx:eax = floor(sqrt(edx:eax))
        test    edx,edx
        js      .neg
        push    ebx esi
        mov     ebx,eax         ; n = esi:ebx
        mov     esi,edx
        push    edx
        push    eax
        fild    qword [esp]
        fsqrt
        fistp   qword [esp]     ; a close guess (< 2**32)
        pop     ecx
        add     esp,4
.down:  mov     eax,ecx         ; while x*x > n: x -= 1
        mul     ecx
        cmp     edx,esi
        ja      .dec
        jb      .up
        cmp     eax,ebx
        jbe     .up
.dec:   dec     ecx
        jmp     .down
.up:    lea     eax,[ecx+1]     ; while (x+1)*(x+1) <= n: x += 1
        test    eax,eax
        jz      .out
        mul     eax
        cmp     edx,esi
        ja      .out
        jb      .inc
        cmp     eax,ebx
        ja      .out
.inc:   inc     ecx
        jmp     .up
.out:   mov     eax,ecx
        xor     edx,edx
        pop     esi ebx
        ret
.neg:   mov     esi,rt_msg_isqrt
        jmp     rt_panic_value
;;; data rt_isqrt
rt_msg_isqrt    db 'isqrt() argument must be nonnegative',0

;;; code rt_mulmod
rt_mulmod:                      ; edx:eax * [ecx] modulo [ecx+8] (all < 2**63, unsigned) -> edx:eax
        push    ebx esi edi ebp
        push    dword [ecx+12]  ; [esp+8] m
        push    dword [ecx+8]
        push    dword [ecx+4]   ; [esp] b (doubled each step)
        push    dword [ecx]
        mov     esi,eax         ; a: edi:esi, its bits from the bottom
        mov     edi,edx
        xor     ebx,ebx         ; the result: ebp:ebx
        xor     ebp,ebp
.l:     mov     eax,esi
        or      eax,edi
        jz      .out
        test    esi,1
        jz      .dbl
        add     ebx,[esp]       ; result = (result + b) % m
        adc     ebp,[esp+4]
        cmp     ebp,[esp+12]
        jb      .dbl
        ja      @f
        cmp     ebx,[esp+8]
        jb      .dbl
@@:     sub     ebx,[esp+8]
        sbb     ebp,[esp+12]
.dbl:   mov     eax,[esp]       ; b = 2b % m
        mov     edx,[esp+4]
        shld    edx,eax,1
        shl     eax,1
        cmp     edx,[esp+12]
        jb      @f
        ja      .sub
        cmp     eax,[esp+8]
        jb      @f
.sub:   sub     eax,[esp+8]
        sbb     edx,[esp+12]
@@:     mov     [esp],eax
        mov     [esp+4],edx
        shrd    esi,edi,1
        shr     edi,1
        jmp     .l
.out:   mov     eax,ebx
        mov     edx,ebp
        add     esp,16
        pop     ebp edi esi ebx
        ret

;;; code rt_ipowmod : rt_mulmod rt_idivmod rt_imul rt_panic_value rt_scratch
; pow(base, exp, mod), CPython's: the result has mod's sign; exp < 0 uses the
; inverse of base modulo mod
rt_ipowmod:                     ; [esp+4] base, [esp+12] exp, edx:eax mod -> edx:eax
        push    ebx esi edi ebp
        mov     ebx,eax
        or      ebx,edx
        jz      .zero
        sub     esp,32          ; +0 |m|, +8 base mod |m|, +16 result, +24 m
        mov     [esp+24],eax
        mov     [esp+28],edx
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     mov     [esp],eax
        mov     [esp+4],edx
        push    dword [esp+56]  ; base % |m| (0 <= it < |m|): |m| is in edx:eax
        push    dword [esp+56]
        call    rt_idivmod
        add     esp,8
        mov     eax,[rt_scratch]
        mov     edx,[rt_scratch+4]
        mov     [esp+8],eax
        mov     [esp+12],edx
        mov     esi,[esp+60]    ; the exponent: edi:esi
        mov     edi,[esp+64]
        test    edi,edi
        jns     .pos
        call    .inverse        ; base = base**-1 mod |m|
        neg     esi
        adc     edi,0
        neg     edi
.pos:   mov     dword [esp+16],1   ; result = 1 % |m|
        mov     dword [esp+20],0
        cmp     dword [esp+4],0
        jne     .l
        cmp     dword [esp],1
        jne     .l
        mov     dword [esp+16],0
.l:     mov     eax,esi
        or      eax,edi
        jz      .sign
        test    esi,1
        jz      .sq
        mov     eax,[esp+16]
        mov     edx,[esp+20]
        push    dword [esp+4]   ; for rt_mulmod: [ecx] base, [ecx+8] |m|
        push    dword [esp+4]
        push    dword [esp+20]
        push    dword [esp+20]
        mov     ecx,esp
        call    rt_mulmod
        add     esp,16
        mov     [esp+16],eax
        mov     [esp+20],edx
.sq:    shrd    esi,edi,1
        shr     edi,1
        mov     eax,esi
        or      eax,edi
        jz      .sign
        mov     eax,[esp+8]
        mov     edx,[esp+12]
        push    dword [esp+4]
        push    dword [esp+4]
        push    dword [esp+20]
        push    dword [esp+20]
        mov     ecx,esp
        call    rt_mulmod
        add     esp,16
        mov     [esp+8],eax
        mov     [esp+12],edx
        jmp     .l
.sign:  mov     eax,[esp+16]    ; a negative modulus: result - |m| unless 0
        mov     edx,[esp+20]
        cmp     dword [esp+28],0
        jns     .out
        mov     ecx,eax
        or      ecx,edx
        jz      .out
        sub     eax,[esp]
        sbb     edx,[esp+4]
.out:   add     esp,32
        pop     ebp edi esi ebx
        ret
.zero:  mov     esi,rt_msg_powmod0
        jmp     rt_panic_value
; the inverse of [esp+12] (base) modulo [esp+4] (|m|, called with one more
; dword on the stack): extended Euclid on (base, m)
.inverse:
        push    esi edi
        ; r0 = m, r1 = base; t0 = 0, t1 = 1 (as signed 64-bit, |t| <= m)
        sub     esp,32          ; +0 r0, +8 r1, +16 t0, +24 t1
        mov     eax,[esp+44]
        mov     edx,[esp+48]
        mov     [esp],eax
        mov     [esp+4],edx
        mov     eax,[esp+52]
        mov     edx,[esp+56]
        mov     [esp+8],eax
        mov     [esp+12],edx
        mov     dword [esp+16],0
        mov     dword [esp+20],0
        mov     dword [esp+24],1
        mov     dword [esp+28],0
.eu:    mov     eax,[esp+8]     ; while r1 != 0
        or      eax,[esp+12]
        jz      .eud
        push    dword [esp+4]   ; q = r0 // r1, r = r0 % r1
        push    dword [esp+4]
        mov     eax,[esp+16]
        mov     edx,[esp+20]
        call    rt_idivmod
        add     esp,8
        push    edx             ; q
        push    eax
        mov     eax,[esp+16]    ; r0, r1 = r1, r
        mov     edx,[esp+20]
        mov     [esp+8],eax
        mov     [esp+12],edx
        mov     eax,[rt_scratch]
        mov     edx,[rt_scratch+4]
        mov     [esp+16],eax
        mov     [esp+20],edx
        mov     eax,[esp+32]    ; t0, t1 = t1, t0 - q*t1
        mov     edx,[esp+36]
        push    edx
        push    eax
        mov     eax,[esp+8]
        mov     edx,[esp+12]
        call    rt_imul         ; q * t1
        add     esp,8
        mov     ecx,[esp+24]
        mov     ebx,[esp+28]
        sub     ecx,eax
        sbb     ebx,edx         ; t0 - q*t1
        mov     eax,[esp+32]
        mov     edx,[esp+36]
        mov     [esp+24],eax
        mov     [esp+28],edx
        mov     [esp+32],ecx
        mov     [esp+36],ebx
        add     esp,8
        jmp     .eu
.eud:   cmp     dword [esp+4],0   ; r0 must be 1
        jne     .noinv
        cmp     dword [esp],1
        jne     .noinv
        mov     eax,[esp+16]    ; t0 mod m
        mov     edx,[esp+20]
        test    edx,edx
        jns     @f
        add     eax,[esp+44]
        adc     edx,[esp+48]
@@:     mov     [esp+52],eax
        mov     [esp+56],edx
        add     esp,32
        pop     edi esi
        ret
.noinv: mov     esi,rt_msg_noinv
        jmp     rt_panic_value
;;; data rt_ipowmod
rt_msg_powmod0  db 'pow() 3rd argument cannot be 0',0
rt_msg_noinv    db 'base is not invertible for the given modulus',0

;;; code rt_list_isum : rt_int_overflow
rt_list_isum:                   ; eax = list of int -> edx:eax = their sum
        push    ebx esi
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
        mov     esi,[eax+16]
@@:     xor     eax,eax
        xor     edx,edx
        jecxz   .out
@@:     add     eax,[esi]
        adc     edx,[esi+4]
        jo      .ovf
        add     esi,8
        dec     ecx
        jnz     @b
.out:   pop     esi ebx
        ret
.ovf:   pop     esi ebx
        jmp     rt_int_overflow

;;; code rt_range_list : rt_list_new rt_list_reserve rt_list_push rt_kd_int rt_panic_value
rt_range_list:                  ; [esp+4] start, [esp+12] stop, [esp+20] step (8 bytes each) -> eax = list(range(start, stop, step))
        push    ebx esi edi ebp
        mov     eax,[esp+36]
        or      eax,[esp+40]
        jz      .zero
        mov     eax,rt_kd_int
        call    rt_list_new
        mov     ebx,eax
        mov     esi,[esp+20]    ; i = start: edi:esi
        mov     edi,[esp+24]
.l:     cmp     dword [esp+40],0
        jl      .down
        mov     eax,esi         ; i < stop?
        cmp     eax,[esp+28]
        mov     eax,edi
        sbb     eax,[esp+32]
        jge     .out
        jmp     .put
.down:  mov     eax,[esp+28]    ; stop < i?
        cmp     eax,esi
        mov     eax,[esp+32]
        sbb     eax,edi
        jge     .out
.put:   mov     eax,ebx
        call    rt_list_push
        mov     [eax],esi
        mov     [eax+4],edi
        add     esi,[esp+36]
        adc     edi,[esp+40]
        jo      .out
        jmp     .l
.out:   mov     eax,ebx
        pop     ebp edi esi ebx
        ret
.zero:  mov     esi,rt_msg_rstep
        jmp     rt_panic_value
;;; data rt_range_list
rt_msg_rstep     db 'range() arg 3 must not be zero',0

;;; code rt_syscall_list : rt_list_new rt_list_push rt_kd_int
rt_syscall_list:                ; eax = address of 6 dwords (eax..edi after the call) -> eax = list[int] (each sign-extended)
        push    ebx esi
        mov     esi,eax
        mov     eax,rt_kd_int
        call    rt_list_new
        mov     ebx,eax
        mov     ecx,6
@@:     push    ecx
        mov     eax,ebx
        call    rt_list_push
        mov     ecx,eax
        mov     eax,[esi]
        cdq
        mov     [ecx],eax
        mov     [ecx+4],edx
        add     esi,4
        pop     ecx
        dec     ecx
        jnz     @b
        mov     eax,ebx
        pop     esi ebx
        ret

;;; code rt_eq_q
rt_eq_q:                        ; two qwords
        mov     ecx,[eax]
        cmp     ecx,[edx]
        jne     .no
        mov     ecx,[eax+4]
        cmp     ecx,[edx+4]
        jne     .no
        mov     eax,1
        ret
.no:    xor     eax,eax
        ret

;;; code rt_cmp_i64
rt_cmp_i64:
        mov     ecx,[eax+4]
        cmp     ecx,[edx+4]
        jl      .lt
        jg      .gt
        mov     ecx,[eax]
        cmp     ecx,[edx]
        jb      .lt
        ja      .gt
        xor     eax,eax
        ret
.lt:    or      eax,-1
        ret
.gt:    mov     eax,1
        ret

;;; code rt_hash_i64 : rt_hash_int
rt_hash_i64:
        mov     edx,[eax+4]
        mov     eax,[eax]
        jmp     rt_hash_int

;;; code rt_repr_i64 : rt_sb_int
rt_repr_i64:
        mov     edx,[eax+4]
        mov     eax,[eax]
        jmp     rt_sb_int

; ---------------------------------------------------------------- None
; None is a null reference; an int -2**63, a float the NaN rt_fnone, a bool 2.

;;; code rt_fnone
;;; data rt_fnone
align 8
rt_fnone        dd 0x6E650001,0x7FF86E6F

;;; code rt_finf : rt_ftoi
;;; data rt_finf
align 8
rt_fnan         dd 0,0x7FF80000   ; math.nan (not None's NaN); math.inf is rt_finf of rt_ftoi

;;; code rt_unbound_text : rt_panic
rt_unbound_text:                ; esi = message (C text): raise UnboundLocalError
if defined rt_throw
        mov     eax,VTX_UnboundLocalError
        mov     edx,DTX_UnboundLocalError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_unbound
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_unbound_text
rt_s_unbound    db 'UnboundLocalError: ',0

;;; code rt_nameerr_text : rt_panic
rt_nameerr_text:                ; esi = message (C text): raise NameError
if defined rt_throw
        mov     eax,VTX_NameError
        mov     edx,DTX_NameError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_nameerr
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_nameerr_text
rt_s_nameerr    db 'NameError: ',0

;;; code rt_typeerr_text : rt_panic
rt_typeerr_text:                ; esi = message (C text): raise TypeError
if defined rt_throw
        mov     eax,VTX_TypeError
        mov     edx,DTX_TypeError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_typeerr2
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_typeerr_text
rt_s_typeerr2   db 'TypeError: ',0

;;; code rt_attrerr_text : rt_panic
rt_attrerr_text:                ; esi = message (C text): raise AttributeError
if defined rt_throw
        mov     eax,VTX_AttributeError
        mov     edx,DTX_AttributeError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_attrerr
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_attrerr_text
rt_s_attrerr    db 'AttributeError: ',0

;;; code rt_hash_none
rt_hash_none:                   ; -> edx:eax = hash(None), the interpreter's
        mov     eax,0xFCA86420
        xor     edx,edx
        ret

;;; code rt_repr_none : rt_sb_cstr
rt_repr_none:                   ; append None
        mov     eax,rt_s_none
        jmp     rt_sb_cstr
;;; data rt_repr_none
rt_s_none       db 'None',0

;;; code rt_cmp_none : rt_sb_need rt_sb_cstr rt_sb_char rt_sb_take rt_raise_typeerr
rt_cmp_none:                    ; one of two values compared for order is None (eax = 1: the left one), ecx = the other's kd: TypeError
        push    ecx
        push    eax
        push    dword [rt_sb_len]
        mov     eax,rt_s_cmpn1
        call    rt_sb_cstr
        cmp     dword [esp+4],0
        je      @f
        mov     eax,rt_s_cmpnn
        call    rt_sb_cstr
        mov     eax,[esp+8]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        jmp     .end
@@:     mov     eax,[esp+8]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        mov     eax,rt_s_cmpn2
        call    rt_sb_cstr
.end:   mov     al,39
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_typeerr
;;; data rt_cmp_none
rt_s_cmpn1      db "'<' not supported between instances of '",0
rt_s_cmpnn      db "NoneType' and '",0
rt_s_cmpn2      db "' and 'NoneType",0

;;; code rt_kd_oint : rt_eq_q rt_cmp_oi64 rt_hash_oi64 rt_repr_oi64
;;; data rt_kd_oint
align 4
rt_kd_oint      dd 8,4,rt_eq_q,rt_cmp_oi64,rt_hash_oi64,rt_repr_oi64,rt_tn_oint
rt_tn_oint      db 'int',0

;;; code rt_cmp_oi64 : rt_cmp_i64 rt_cmp_none
rt_cmp_oi64:
        cmp     dword [eax+4],0x80000000
        jne     @f
        cmp     dword [eax],0
        je      .ln
@@:     cmp     dword [edx+4],0x80000000
        jne     rt_cmp_i64
        cmp     dword [edx],0
        jne     rt_cmp_i64
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_hash_oi64 : rt_hash_i64 rt_hash_none
rt_hash_oi64:
        cmp     dword [eax+4],0x80000000
        jne     rt_hash_i64
        cmp     dword [eax],0
        jne     rt_hash_i64
        jmp     rt_hash_none

;;; code rt_repr_oi64 : rt_repr_i64 rt_repr_none
rt_repr_oi64:
        cmp     dword [eax+4],0x80000000
        jne     rt_repr_i64
        cmp     dword [eax],0
        jne     rt_repr_i64
        jmp     rt_repr_none

;;; code rt_kd_obool : rt_eq_w rt_cmp_obool rt_hash_obool rt_repr_obool
;;; data rt_kd_obool
align 4
rt_kd_obool     dd 4,0,rt_eq_w,rt_cmp_obool,rt_hash_obool,rt_repr_obool,rt_tn_obool
rt_tn_obool     db 'bool',0

;;; code rt_cmp_obool : rt_cmp_i32 rt_cmp_none
rt_cmp_obool:
        cmp     dword [eax],2
        je      .ln
        cmp     dword [edx],2
        jne     rt_cmp_i32
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_hash_obool : rt_hash_i32 rt_hash_none
rt_hash_obool:
        cmp     dword [eax],2
        jne     rt_hash_i32
        jmp     rt_hash_none

;;; code rt_repr_obool : rt_repr_bool rt_repr_none
rt_repr_obool:
        cmp     dword [eax],2
        jne     rt_repr_bool
        jmp     rt_repr_none

;;; code rt_kd_ofloat : rt_eq_of rt_cmp_of rt_hash_of rt_repr_of_f
;;; data rt_kd_ofloat
align 4
rt_kd_ofloat    dd 8,2,rt_eq_of,rt_cmp_of,rt_hash_of,rt_repr_of_f,rt_tn_ofloat
rt_tn_ofloat    db 'float',0

;;; code rt_is_fnone : rt_fnone
rt_is_fnone:                    ; eax = &float -> ZF set if it is None (keeps eax, edx)
        push    ecx
        mov     ecx,[eax+4]
        cmp     ecx,[rt_fnone+4]
        jne     @f
        mov     ecx,[eax]
        cmp     ecx,[rt_fnone]
@@:     pop     ecx
        ret

;;; code rt_eq_of : rt_is_fnone rt_eq_f
rt_eq_of:                       ; None equals None only
        push    ebx
        xor     ebx,ebx
        call    rt_is_fnone
        jne     @f
        inc     ebx
@@:     xchg    eax,edx
        call    rt_is_fnone
        jne     @f
        add     ebx,2
@@:     xchg    eax,edx
        test    ebx,ebx
        jz      .num
        cmp     ebx,3
        sete    al
        movzx   eax,al
        pop     ebx
        ret
.num:   pop     ebx
        jmp     rt_eq_f

;;; code rt_cmp_of : rt_is_fnone rt_cmp_f rt_cmp_none
rt_cmp_of:
        call    rt_is_fnone
        je      .ln
        xchg    eax,edx
        call    rt_is_fnone
        xchg    eax,edx
        jne     rt_cmp_f
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_hash_of : rt_is_fnone rt_hash_f rt_hash_none
rt_hash_of:
        call    rt_is_fnone
        jne     rt_hash_f
        jmp     rt_hash_none

;;; code rt_repr_of_f : rt_is_fnone rt_repr_f rt_repr_none
rt_repr_of_f:
        call    rt_is_fnone
        jne     rt_repr_f
        jmp     rt_repr_none

; ---------------------------------------------------------------- element types
; Containers know the type of their elements by a descriptor (kd). The runtime
; has those of the built-in types; the code generator emits one per class
; whose objects go into containers. A descriptor:
;   +0  bytes of an element in a slot (4, or 8: float, int)
;   +4  flags: 1 a counted reference, 2 float (x87 qword), 4 a 64-bit int
;   +8  eq:   eax = &a, edx = &b, ecx = kd -> eax = 1 if a == b
;   +12 cmp:  eax = &a, edx = &b, ecx = kd -> eax = -1, 0, 1 (TypeError if unordered)
;   +16 hash: eax = &a, ecx = kd -> edx:eax = hash(a), the interpreter's (TypeError if unhashable)
;   +20 repr: eax = &a, ecx = kd: append repr(a) to the string builder
;   +24 the type's name (NUL-terminated)
; The functions keep ebx, esi, edi and ebp.

;;; code rt_kd_bool : rt_eq_w rt_cmp_i32 rt_hash_i32 rt_repr_bool
;;; data rt_kd_bool
align 4
rt_kd_bool      dd 4,0,rt_eq_w,rt_cmp_i32,rt_hash_i32,rt_repr_bool,rt_tn_bool
rt_tn_bool      db 'bool',0

;;; code rt_kd_int : rt_eq_q rt_cmp_i64 rt_hash_i64 rt_repr_i64
;;; data rt_kd_int
align 4
rt_kd_int       dd 8,4,rt_eq_q,rt_cmp_i64,rt_hash_i64,rt_repr_i64,rt_tn_int
rt_tn_int       db 'int',0

;;; code rt_kd_float : rt_eq_f rt_cmp_f rt_hash_f rt_repr_f
;;; data rt_kd_float
align 4
rt_kd_float     dd 8,2,rt_eq_f,rt_cmp_f,rt_hash_f,rt_repr_f,rt_tn_float
rt_tn_float     db 'float',0

;;; code rt_kd_str : rt_eq_s rt_cmp_s rt_hash_s rt_repr_s
;;; data rt_kd_str
align 4
rt_kd_str       dd 4,1,rt_eq_s,rt_cmp_s,rt_hash_s,rt_repr_s,rt_tn_str
rt_tn_str       db 'str',0

;;; code rt_sb_type : rt_sb_cstr rt_sb_str
rt_sb_type:                     ; eax = a class (its descriptor: name, base, ...): "<class 'module.Name'>"
        push    esi
        push    eax
        mov     eax,rt_s_classq
        call    rt_sb_cstr
        pop     eax
        mov     esi,TYQ         ; the qualified names of the program's classes (by descriptor)
@@:     mov     ecx,[esi]
        test    ecx,ecx
        jz      .short
        cmp     ecx,eax
        je      .qual
        add     esi,8
        jmp     @b
.qual:  mov     eax,[esi+4]
        jmp     .put
.short: mov     eax,[eax]
.put:   call    rt_sb_str
        mov     eax,rt_s_classe
        call    rt_sb_cstr
        pop     esi
        ret
;;; data rt_sb_type
rt_s_classq     db "<class '",0
rt_s_classe     db "'>",0

;;; code rt_type_qualname
rt_type_qualname:               ; eax = a class (its descriptor) -> eax = its __qualname__ (static)
        mov     ecx,TYQN        ; the classes whose qualified name is not their name (Outer.Inner)
@@:     mov     edx,[ecx]
        test    edx,edx
        jz      .name
        cmp     edx,eax
        je      .qual
        add     ecx,8
        jmp     @b
.qual:  mov     eax,[ecx+4]
        ret
.name:  mov     eax,[eax]
        ret

;;; code rt_repr_type : rt_sb_type
rt_repr_type:                   ; eax = &class
        mov     eax,[eax]
        jmp     rt_sb_type

;;; code rt_issubclass
rt_issubclass:                  ; eax = a class, edx = another -> eax = 1 when the first is the second or derives from it
@@:     cmp     eax,edx
        je      .yes
        mov     eax,[eax+4]
        test    eax,eax
        jnz     @b
        ret
.yes:   mov     eax,1
        ret

;;; code rt_kd_type : rt_eq_w rt_cmp_no rt_hash_p rt_repr_type
;;; data rt_kd_type
align 4
rt_kd_type      dd 4,0,rt_eq_w,rt_cmp_no,rt_hash_p,rt_repr_type,rt_tn_type
rt_tn_type      db 'type',0

;;; code rt_kd_obj : rt_eq_w rt_cmp_no rt_hash_p rt_repr_p
;;; data rt_kd_obj
align 4
rt_kd_obj       dd 4,1,rt_eq_w,rt_cmp_no,rt_hash_p,rt_repr_p,rt_tn_obj
rt_tn_obj       db 'object',0

;;; code rt_kd_tuple : rt_tuple_eq_k rt_tuple_cmp_k rt_tuple_hash_k rt_tuple_repr_k
;;; data rt_kd_tuple
align 4
rt_kd_tuple     dd 4,1,rt_tuple_eq_k,rt_tuple_cmp_k,rt_tuple_hash_k,rt_tuple_repr_k,rt_tn_tuple
rt_tn_tuple     db 'tuple',0

;;; code rt_kd_list : rt_list_eq_k rt_list_cmp_k rt_hash_no rt_list_repr_k
;;; data rt_kd_list
align 4
rt_kd_list      dd 4,1,rt_list_eq_k,rt_list_cmp_k,rt_hash_no,rt_list_repr_k,rt_tn_list
rt_tn_list      db 'list',0

;;; code rt_kd_dict : rt_dict_eq_k rt_cmp_no rt_hash_no rt_dict_repr_k
;;; data rt_kd_dict
align 4
rt_kd_dict      dd 4,1,rt_dict_eq_k,rt_cmp_no,rt_hash_no,rt_dict_repr_k,rt_tn_dict
rt_tn_dict      db 'dict',0

;;; code rt_kd_set : rt_set_eq_k rt_cmp_no rt_hash_no rt_set_repr_k
;;; data rt_kd_set
align 4
rt_kd_set       dd 4,1,rt_set_eq_k,rt_cmp_no,rt_hash_no,rt_set_repr_k,rt_tn_set
rt_tn_set       db 'set',0

;;; code rt_kd_word : rt_eq_w rt_cmp_i32 rt_hash_i32 rt_repr_i32
;;; data rt_kd_word
align 4
rt_kd_word      dd 4,0,rt_eq_w,rt_cmp_i32,rt_hash_i32,rt_repr_i32,rt_tn_word
rt_tn_word      db 'object',0

;;; code rt_eq_w
rt_eq_w:                        ; one dword: bool, int, identity
        mov     eax,[eax]
        cmp     eax,[edx]
        sete    al
        movzx   eax,al
        ret

;;; code rt_cmp_i32
rt_cmp_i32:
        mov     eax,[eax]
        cmp     eax,[edx]
        mov     eax,0
        jl      .lt
        setg    al
        ret
.lt:    dec     eax
        ret

;;; code rt_hash_i32 : rt_hash_int
rt_hash_i32:
        mov     eax,[eax]
        cdq
        jmp     rt_hash_int

;;; code rt_hash_int
rt_hash_int:                    ; edx:eax = int -> edx:eax = its hash (CPython's: modulo 2**61-1, -1 -> -2)
        push    ebx
        mov     ebx,edx         ; the sign
        test    edx,edx
        jns     @f
        neg     eax             ; |x|
        adc     edx,0
        neg     edx
@@:     mov     ecx,edx         ; (|x| & (2**61-1)) + (|x| >> 61)
        shr     ecx,29
        and     edx,0x1FFFFFFF
        add     eax,ecx
        adc     edx,0
        cmp     edx,0x1FFFFFFF  ; at most one modulus too big
        jb      .ok
        ja      .sub
        cmp     eax,-1
        jne     .ok
.sub:   sub     eax,-1
        sbb     edx,0x1FFFFFFF
.ok:    test    ebx,ebx
        jns     .out
        neg     eax
        adc     edx,0
        neg     edx
        cmp     edx,-1
        jne     .out
        cmp     eax,-1
        jne     .out
        dec     eax
.out:   pop     ebx
        ret

;;; code rt_repr_i32 : rt_sb_int
rt_repr_i32:
        mov     eax,[eax]
        jmp     rt_sb_int

;;; code rt_repr_bool : rt_sb_bool
rt_repr_bool:
        mov     eax,[eax]
        jmp     rt_sb_bool

;;; code rt_eq_f
rt_eq_f:
        fld     qword [eax]
        fcomp   qword [edx]
        fnstsw  ax
        sahf
        mov     eax,0
        jp      @f
        sete    al
@@:     ret

;;; code rt_cmp_f
rt_cmp_f:                       ; NaN: neither less nor greater
        fld     qword [eax]
        fcomp   qword [edx]
        fnstsw  ax
        sahf
        mov     eax,0
        jp      .out
        jb      .lt
        seta    al
.out:   ret
.lt:    dec     eax
        ret

;;; code rt_hash_f : rt_shl64 rt_shr64
rt_hash_f:                      ; a float's hash, CPython's: its value modulo 2**61-1
        push    ebx esi edi
        mov     esi,[eax]       ; the mantissa: edi:esi
        mov     edi,[eax+4]
        mov     ebx,edi         ; the sign: bit 31
        mov     ecx,edi
        shr     ecx,20
        and     ecx,0x7FF
        and     edi,0xFFFFF
        cmp     ecx,0x7FF
        je      .special
        test    ecx,ecx
        jz      .denorm
        or      edi,0x100000
        sub     ecx,1075        ; value = mantissa * 2**ecx
        jmp     .have
.denorm: mov    ecx,-1074
.have:  mov     eax,esi
        or      eax,edi
        jz      .zero
        mov     eax,ecx         ; s = the exponent modulo 61
        cdq
        mov     ecx,61
        idiv    ecx
        test    edx,edx
        jns     @f
        add     edx,61
@@:     push    edx
        mov     ecx,edx         ; mantissa * 2**s modulo 2**61-1: rotate left by s in 61 bits
        mov     eax,esi
        mov     edx,edi
        call    rt_shl64
        and     edx,0x1FFFFFFF
        pop     ecx
        push    edx
        push    eax
        neg     ecx
        add     ecx,61
        mov     eax,esi
        mov     edx,edi
        call    rt_shr64
        or      eax,[esp]
        or      edx,[esp+4]
        add     esp,8
        jmp     .sign
.special: or    esi,edi
        jnz     .zero           ; NaN
        mov     eax,314159      ; infinity
        xor     edx,edx
.sign:  test    ebx,ebx
        jns     .out
        neg     eax
        adc     edx,0
        neg     edx
        cmp     edx,-1
        jne     .out
        cmp     eax,-1
        jne     .out
        dec     eax
.out:   pop     edi esi ebx
        ret
.zero:  xor     eax,eax
        xor     edx,edx
        pop     edi esi ebx
        ret

;;; code rt_shl64
rt_shl64:                       ; edx:eax <<= cl (0..63)
        cmp     cl,32
        jae     @f
        shld    edx,eax,cl
        shl     eax,cl
        ret
@@:     mov     edx,eax
        xor     eax,eax
        sub     cl,32
        shl     edx,cl
        ret

;;; code rt_shr64
rt_shr64:                       ; edx:eax >>= cl (0..63), unsigned
        cmp     cl,32
        jae     @f
        shrd    eax,edx,cl
        shr     edx,cl
        ret
@@:     mov     eax,edx
        xor     edx,edx
        sub     cl,32
        shr     eax,cl
        ret

;;; code rt_mul64
rt_mul64:                       ; edx:eax *= the qword at [ecx] (modulo 2**64)
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        imul    esi,[ecx]       ; a.hi * b.lo
        mov     edx,[ecx+4]
        imul    edx,ebx         ; a.lo * b.hi
        add     esi,edx
        mov     eax,ebx
        mul     dword [ecx]     ; a.lo * b.lo
        add     edx,esi
        pop     esi ebx
        ret

;;; code rt_repr_f : rt_sb_float
rt_repr_f:
        fld     qword [eax]
        jmp     rt_sb_float

;;; code rt_eq_s : rt_str_eq
rt_eq_s:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_str_eq

;;; code rt_cmp_s : rt_str_cmp rt_cmp_none
rt_cmp_s:
        mov     eax,[eax]
        mov     edx,[edx]
        test    eax,eax
        jz      .ln
        test    edx,edx
        jnz     rt_str_cmp
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_hash_s : rt_str_hash rt_hash_none
rt_hash_s:
        mov     eax,[eax]
        test    eax,eax
        jz      rt_hash_none
        jmp     rt_str_hash

;;; code rt_str_hash
rt_str_hash:                    ; eax = str -> edx:eax = its hash (64-bit FNV-1a of the bytes, the interpreter's)
        push    ebx esi edi ebp
        mov     edi,0x84222325  ; ebp:edi = the offset basis
        mov     ebp,0xCBF29CE4
        test    eax,eax
        jz      .done
        mov     ecx,[eax+8]
        lea     esi,[eax+12]
        jecxz   .done
.l:     movzx   eax,byte [esi]
        xor     edi,eax
        mov     eax,0x1B3       ; * 0x100000001B3: lo*0x1B3 + (hi*0x1B3 + lo<<8)<<32
        mul     edi
        mov     ebx,edi
        shl     ebx,8
        imul    ebp,ebp,0x1B3
        add     ebp,edx
        add     ebp,ebx
        mov     edi,eax
        inc     esi
        dec     ecx
        jnz     .l
.done:  mov     eax,edi
        mov     edx,ebp
        cmp     edx,-1
        jne     @f
        cmp     eax,-1
        jne     @f
        dec     eax
@@:     pop     ebp edi esi ebx
        ret

;;; code rt_repr_s : rt_sb_repr_str rt_repr_none
rt_repr_s:
        mov     eax,[eax]
        test    eax,eax
        jz      rt_repr_none
        jmp     rt_sb_repr_str

;;; code rt_hash_p : rt_hash_none
rt_hash_p:                      ; identity: the address, rotated as CPython does
        mov     eax,[eax]
        test    eax,eax
        jz      rt_hash_none
        mov     edx,eax
        shl     edx,28
        shr     eax,4
        ret

;;; code rt_repr_p : rt_sb_char rt_sb_cstr rt_repr_none
rt_repr_p:                      ; <name object>
        cmp     dword [eax],0
        je      rt_repr_none
        push    ecx
        mov     al,'<'
        call    rt_sb_char
        pop     ecx
        mov     eax,[ecx+24]
        call    rt_sb_cstr
        mov     eax,rt_s_objgt
        jmp     rt_sb_cstr
;;; data rt_repr_p
rt_s_objgt      db ' object>',0

;;; code rt_cmp_no : rt_sb_need rt_sb_cstr rt_sb_char rt_sb_take rt_raise_typeerr
rt_cmp_no:                      ; a type without an order: TypeError
        push    ecx
        push    dword [rt_sb_len]
        mov     eax,rt_s_cmpno1
        call    rt_sb_cstr
        mov     eax,[esp+4]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        mov     eax,rt_s_cmpno2
        call    rt_sb_cstr
        mov     eax,[esp+4]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        mov     al,39
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_typeerr
;;; data rt_cmp_no
rt_s_cmpno1     db "'<' not supported between instances of '",0
rt_s_cmpno2     db "' and '",0

;;; code rt_hash_no : rt_sb_need rt_sb_cstr rt_sb_char rt_sb_take rt_raise_typeerr
rt_hash_no:                     ; a type without a hash: TypeError
        push    ecx
        push    dword [rt_sb_len]
        mov     eax,rt_s_unhash
        call    rt_sb_cstr
        mov     eax,[esp+4]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        mov     al,39
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_typeerr
;;; data rt_hash_no
rt_s_unhash     db "unhashable type: '",0

;;; code rt_elem_copy : rt_incref
rt_elem_copy:                   ; eax = to, edx = from, ecx = kd: copy an element, taking a reference to it
        push    ecx
        mov     ecx,[edx]
        mov     [eax],ecx
        mov     ecx,[esp]
        cmp     dword [ecx],8
        jne     @f
        mov     ecx,[edx+4]
        mov     [eax+4],ecx
        pop     ecx
        ret
@@:     pop     ecx
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[eax]
        jmp     rt_incref
@@:     ret

;;; code rt_repr_of : rt_sb_need rt_sb_take
rt_repr_of:                     ; eax = &value, ecx = kd -> eax = new str: repr(value)
        push    dword [rt_sb_len]
        call    dword [ecx+20]
        pop     eax
        jmp     rt_sb_take

;;; code rt_raise_key : rt_repr_of rt_raise_keyerr
rt_raise_key:                   ; eax = &key, ecx = kd: raise KeyError(key)
        call    rt_repr_of
        mov     ecx,eax
        jmp     rt_raise_keyerr

;;; code rt_raise_typeerr : rt_raise
rt_raise_typeerr:               ; ecx = message (str, owned): raise TypeError
if defined rt_throw
        mov     eax,VTX_TypeError
        mov     edx,DTX_TypeError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_typeerr
        jmp     rt_raise
end if
;;; data rt_raise_typeerr
rt_s_typeerr    db 'TypeError',0

;;; code rt_raise_keyerr : rt_raise
rt_raise_keyerr:                ; ecx = message (str, owned: the key's repr): raise KeyError
if defined rt_throw
        mov     eax,VTX_KeyError
        mov     edx,DTX_KeyError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_keyerr
        jmp     rt_raise
end if
;;; data rt_raise_keyerr
rt_s_keyerr     db 'KeyError',0

;;; code rt_raise_valerr : rt_raise
rt_raise_valerr:                ; ecx = message (str, owned): raise ValueError
if defined rt_throw
        mov     eax,VTX_ValueError
        mov     edx,DTX_ValueError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_valerr
        jmp     rt_raise
end if
;;; data rt_raise_valerr
rt_s_valerr     db 'ValueError',0

;;; code rt_keyerr_text : rt_panic
rt_keyerr_text:                 ; esi = the message (C text, quoted already): raise KeyError
if defined rt_throw
        mov     eax,VTX_KeyError
        mov     edx,DTX_KeyError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_keyerr2
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_keyerr_text
rt_s_keyerr2    db 'KeyError: ',0

; ---------------------------------------------------------------- lists
; list: +8 length, +12 capacity, +16 the element array, +20 the element type (kd)

;;; code rt_list_new : rt_alloc rt_list_free
rt_list_new:                    ; eax = element type -> eax = new empty list
        push    eax
        mov     eax,24
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_list_free
        mov     [eax+20],ecx
        ret

;;; code rt_list_free : rt_free rt_decref
rt_list_free:                   ; destroy routine of lists
        push    ebx esi
        mov     ebx,eax
        mov     ecx,[ebx+20]
        test    byte [ecx+4],1
        jz      .items
        mov     esi,[ebx+8]
.drop:  dec     esi
        js      .items
        mov     eax,[ebx+16]
        mov     eax,[eax+esi*4]
        call    rt_decref
        jmp     .drop
.items: mov     eax,[ebx+16]
        call    rt_free
        mov     eax,ebx
        pop     esi ebx
        jmp     rt_free

;;; code rt_list_reserve : rt_alloc rt_free
rt_list_reserve:                ; eax = list, ecx = wanted length: room for it
        cmp     ecx,[eax+12]
        jbe     .ok
        push    ebx esi edi
        mov     ebx,eax
        mov     eax,[ebx+12]
        add     eax,eax
        cmp     eax,ecx
        jae     @f
        mov     eax,ecx
@@:     cmp     eax,4
        jae     @f
        mov     eax,4
@@:     mov     [ebx+12],eax
        mov     edx,[ebx+20]
        mul     dword [edx]
        call    rt_alloc
        push    eax
        mov     edi,eax
        mov     esi,[ebx+16]
        mov     ecx,[ebx+20]
        mov     ecx,[ecx]
        imul    ecx,[ebx+8]
        rep     movsb
        mov     eax,[ebx+16]
        call    rt_free
        pop     dword [ebx+16]
        mov     eax,ebx
        pop     edi esi ebx
.ok:    ret

;;; code rt_list_push : rt_list_reserve rt_panic_none
rt_list_push:                   ; eax = list -> eax = address of a new last slot
        test    eax,eax
        jz      rt_panic_none
        push    ebx
        mov     ebx,eax
        mov     ecx,[eax+8]
        inc     ecx
        call    rt_list_reserve
        mov     eax,[ebx+8]
        inc     dword [ebx+8]
        mov     ecx,[ebx+20]
        imul    eax,[ecx]
        add     eax,[ebx+16]
        pop     ebx
        ret

;;; code rt_list_at : rt_panic_index
rt_list_at:                     ; eax = list, edx = index (negative: from the end) -> eax = address of the element
        test    eax,eax
        jz      rt_panic_index
        test    edx,edx
        jns     @f
        add     edx,[eax+8]
@@:     cmp     edx,[eax+8]
        jae     rt_panic_index
        mov     ecx,[eax+20]
        imul    edx,[ecx]
        mov     eax,[eax+16]
        add     eax,edx
        ret

;;; code rt_list_set_at : rt_panic_index
rt_list_set_at:                 ; rt_list_at for an assignment (its own message)
        test    eax,eax
        jz      .bad
        test    edx,edx
        jns     @f
        add     edx,[eax+8]
@@:     cmp     edx,[eax+8]
        jae     .bad
        mov     ecx,[eax+20]
        imul    edx,[ecx]
        mov     eax,[eax+16]
        add     eax,edx
        ret
.bad:   mov     esi,rt_msg_lassign
        jmp     rt_index_error
;;; data rt_list_set_at
rt_msg_lassign  db 'list assignment index out of range',0

;;; code rt_list_pop : rt_panic_index rt_scratch
rt_list_pop:                    ; eax = list, edx = index (negative: from the end) -> eax = rt_scratch holding the removed element
        push    ebx esi edi
        mov     ebx,eax
        test    ebx,ebx
        jz      .empty
        mov     ecx,[ebx+8]
        test    ecx,ecx
        jz      .empty
        test    edx,edx
        jns     @f
        add     edx,ecx
@@:     cmp     edx,ecx
        jae     .range
        mov     ecx,[ebx+20]
        mov     ecx,[ecx]       ; element size
        mov     esi,edx
        imul    esi,ecx
        add     esi,[ebx+16]    ; the element
        mov     eax,[esi]
        mov     [rt_scratch],eax
        mov     eax,[esi+ecx-4]
        mov     [rt_scratch+ecx-4],eax
        mov     eax,[ebx+8]
        dec     eax
        mov     [ebx+8],eax
        sub     eax,edx         ; elements after it
        imul    eax,ecx
        mov     edi,esi
        add     esi,ecx
        mov     ecx,eax
        rep     movsb
        mov     eax,rt_scratch
        pop     edi esi ebx
        ret
.empty: mov     esi,rt_msg_popempty
        jmp     rt_index_error
.range: mov     esi,rt_msg_poprange
        jmp     rt_index_error
;;; data rt_list_pop
rt_msg_popempty db 'pop from empty list',0
rt_msg_poprange db 'pop index out of range',0

;;; code rt_list_insert : rt_list_reserve rt_panic_none
rt_list_insert:                 ; eax = list, edx = index -> eax = address of a new slot there (zero)
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,[ebx+20]
        mov     ebp,[ebp]       ; element size
        mov     eax,[ebx+8]
        test    edx,edx
        jns     @f
        add     edx,eax
        jns     @f
        xor     edx,edx
@@:     cmp     edx,eax
        jbe     @f
        mov     edx,eax
@@:     push    edx
        lea     ecx,[eax+1]
        mov     eax,ebx
        call    rt_list_reserve
        pop     edx
        mov     ecx,[ebx+8]
        sub     ecx,edx
        imul    ecx,ebp         ; bytes to move up
        mov     esi,[ebx+8]
        imul    esi,ebp
        add     esi,[ebx+16]
        dec     esi
        lea     edi,[esi+ebp]
        std
        rep     movsb
        cld
        inc     dword [ebx+8]
        mov     eax,edx
        imul    eax,ebp
        add     eax,[ebx+16]
        mov     dword [eax],0
        mov     dword [eax+ebp-4],0
        pop     ebp edi esi ebx
        ret

;;; code rt_list_find
rt_list_find:                   ; eax = list, edx = &value -> eax = index of the first equal element, or -1
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        xor     esi,esi
        test    ebx,ebx
        jz      .nf
.l:     cmp     esi,[ebx+8]
        jae     .nf
        mov     ebp,[ebx+20]
        mov     eax,esi
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     edx,edi
        mov     ecx,ebp
        call    dword [ebp+8]
        test    eax,eax
        jnz     .found
        inc     esi
        jmp     .l
.found: mov     eax,esi
        pop     ebp edi esi ebx
        ret
.nf:    or      eax,-1
        pop     ebp edi esi ebx
        ret

;;; code rt_list_count
rt_list_count:                  ; eax = list, edx = &value -> eax = how many elements equal it
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        xor     esi,esi
        push    0
        test    ebx,ebx
        jz      .out
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     ebp,[ebx+20]
        mov     eax,esi
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     edx,edi
        mov     ecx,ebp
        call    dword [ebp+8]
        add     [esp],eax
        inc     esi
        jmp     .l
.out:   pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_list_index : rt_list_find rt_panic_value
rt_list_index:                  ; eax = list, edx = &value -> eax = index of the first equal element (ValueError if none)
        call    rt_list_find
        test    eax,eax
        js      .bad
        ret
.bad:   mov     esi,rt_msg_lindex
        jmp     rt_panic_value
;;; data rt_list_index
rt_msg_lindex   db 'list.index(x): x not in list',0

;;; code rt_list_remove : rt_list_find rt_list_pop rt_decref rt_panic_value
rt_list_remove:                 ; eax = list, edx = &value: remove the first equal element
        push    ebx
        mov     ebx,eax
        call    rt_list_find
        test    eax,eax
        js      .bad
        mov     edx,eax
        mov     eax,ebx
        call    rt_list_pop
        mov     ecx,[ebx+20]
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[eax]
        call    rt_decref
@@:     pop     ebx
        ret
.bad:   mov     esi,rt_msg_lremove
        jmp     rt_panic_value
;;; data rt_list_remove
rt_msg_lremove  db 'list.remove(x): x not in list',0

;;; code rt_list_reverse
rt_list_reverse:                ; eax = list: reverse it in place
        push    ebx esi edi
        test    eax,eax
        jz      .out
        mov     ebx,[eax+20]
        mov     ebx,[ebx]       ; element size
        mov     ecx,[eax+8]
        mov     esi,[eax+16]
        dec     ecx
        js      .out
        imul    ecx,ebx
        lea     edi,[esi+ecx]
.l:     cmp     esi,edi
        jae     .out
        mov     eax,[esi]
        mov     edx,[edi]
        mov     [esi],edx
        mov     [edi],eax
        cmp     ebx,8
        jne     @f
        mov     eax,[esi+4]
        mov     edx,[edi+4]
        mov     [esi+4],edx
        mov     [edi+4],eax
@@:     add     esi,ebx
        sub     edi,ebx
        jmp     .l
.out:   pop     edi esi ebx
        ret

;;; code rt_list_sort : rt_alloc rt_free
; A stable merge sort (equal elements keep their order, as in CPython) of
; pointers to the elements; then the elements are copied into that order.
rt_list_sort:                   ; eax = list: sort it in place, ascending
        push    ebx esi edi ebp
        mov     ebx,eax
        test    ebx,ebx
        jz      .out
        mov     ecx,[ebx+8]
        cmp     ecx,2
        jb      .out
        sub     esp,32          ; +0 pointers, +4 the other array, +8 n, +12 run width, +16 mid, +20 hi, +24 k, +28 new elements
        mov     [esp+8],ecx
        lea     eax,[ecx*4]
        call    rt_alloc
        mov     [esp],eax
        mov     eax,[esp+8]
        shl     eax,2
        call    rt_alloc
        mov     [esp+4],eax
        mov     edi,[esp]
        mov     esi,[ebx+16]
        mov     edx,[ebx+20]
        mov     edx,[edx]
        mov     ecx,[esp+8]
@@:     mov     [edi],esi
        add     edi,4
        add     esi,edx
        dec     ecx
        jnz     @b
        mov     dword [esp+12],1
.pass:  mov     eax,[esp+12]
        cmp     eax,[esp+8]
        jae     .sorted
        xor     esi,esi         ; lo
.run:   cmp     esi,[esp+8]
        jae     .swap
        mov     eax,esi
        add     eax,[esp+12]
        cmp     eax,[esp+8]
        jbe     @f
        mov     eax,[esp+8]
@@:     mov     [esp+16],eax    ; mid
        add     eax,[esp+12]
        cmp     eax,[esp+8]
        jbe     @f
        mov     eax,[esp+8]
@@:     mov     [esp+20],eax    ; hi
        mov     edi,esi         ; i
        mov     ebp,[esp+16]    ; j
        mov     [esp+24],esi    ; k
.m:     mov     ecx,[esp+24]
        cmp     ecx,[esp+20]
        jae     .mdone
        cmp     edi,[esp+16]
        jae     .takej
        cmp     ebp,[esp+20]
        jae     .takei
        mov     eax,[esp]
        mov     edx,[eax+edi*4]
        mov     eax,[eax+ebp*4]
        mov     ecx,[ebx+20]
        call    dword [ecx+12]  ; a[j] < a[i]: j first
        test    eax,eax
        js      .takej
.takei: mov     eax,[esp]
        mov     eax,[eax+edi*4]
        inc     edi
        jmp     .put
.takej: mov     eax,[esp]
        mov     eax,[eax+ebp*4]
        inc     ebp
.put:   mov     ecx,[esp+4]
        mov     edx,[esp+24]
        mov     [ecx+edx*4],eax
        inc     dword [esp+24]
        jmp     .m
.mdone: mov     esi,[esp+20]
        jmp     .run
.swap:  mov     eax,[esp]
        mov     ecx,[esp+4]
        mov     [esp],ecx
        mov     [esp+4],eax
        shl     dword [esp+12],1
        jmp     .pass
.sorted:
        mov     eax,[ebx+12]
        mov     ecx,[ebx+20]
        mul     dword [ecx]
        call    rt_alloc
        mov     [esp+28],eax
        mov     edi,eax
        mov     esi,[esp]
        mov     ecx,[esp+8]
        mov     edx,[ebx+20]
        mov     edx,[edx]
.cp:    mov     eax,[esi]
        push    ecx
        mov     ecx,[eax]
        mov     [edi],ecx
        mov     ecx,[eax+edx-4]
        mov     [edi+edx-4],ecx
        pop     ecx
        add     esi,4
        add     edi,edx
        dec     ecx
        jnz     .cp
        mov     eax,[ebx+16]
        call    rt_free
        mov     eax,[esp+28]
        mov     [ebx+16],eax
        mov     eax,[esp]
        call    rt_free
        mov     eax,[esp+4]
        call    rt_free
        add     esp,32
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_extend : rt_list_push rt_elem_copy rt_panic_none
rt_list_extend:                 ; eax = destination, edx = source: append the source's elements
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        test    esi,esi
        jz      .out
        mov     ebp,[esi+8]     ; a list extended by itself: its old length
        xor     edi,edi
.l:     cmp     edi,ebp
        jae     .out
        mov     eax,ebx
        call    rt_list_push
        mov     ecx,[esi+20]
        mov     edx,edi
        imul    edx,[ecx]
        add     edx,[esi+16]
        call    rt_elem_copy
        inc     edi
        jmp     .l
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_copy : rt_list_new rt_list_reserve rt_list_extend
rt_list_copy:                   ; eax = list, edx = its element type -> eax = new list of its elements
        push    ebx
        mov     ebx,eax
        mov     eax,edx
        call    rt_list_new
        test    ebx,ebx
        jz      .out
        push    eax
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        mov     eax,[esp]
        mov     edx,ebx
        call    rt_list_extend
        pop     eax
.out:   pop     ebx
        ret

;;; code rt_list_concat : rt_list_copy rt_list_extend
rt_list_concat:                 ; eax = a, edx = b, ecx = element type -> eax = new list a + b
        push    ebx
        mov     ebx,edx
        mov     edx,ecx
        call    rt_list_copy
        push    eax
        mov     edx,ebx
        call    rt_list_extend
        pop     eax
        pop     ebx
        ret

;;; code rt_list_mul : rt_list_new rt_list_extend
rt_list_mul:                    ; eax = list, edx = n, ecx = element type -> eax = new list: n times its elements
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,ecx
        call    rt_list_new
        push    eax
.l:     test    esi,esi
        jle     .out
        mov     eax,[esp]
        mov     edx,ebx
        call    rt_list_extend
        dec     esi
        jmp     .l
.out:   pop     eax
        pop     esi ebx
        ret

;;; code rt_list_del_slice : rt_slice_range rt_decref
rt_list_del_slice:              ; [esp+4] list, lo, hi, step, flags: del list[lo:hi:step]
        push    ebx esi edi ebp
        mov     ebx,[esp+20]
        mov     eax,[ebx+8]
        lea     esi,[esp+24]
        call    rt_slice_range          ; eax = start, edx = step, ecx = count
        jecxz   .out
        test    edx,edx                 ; going down: the same items going up
        jns     @f
        push    edx
        imul    edx,ecx
        sub     edx,[esp]               ; (count - 1) * step
        add     eax,edx
        pop     edx
        neg     edx
@@:     push    ecx edx eax             ; [esp] first, [esp+4] step, [esp+8] count
        mov     ebp,[ebx+20]            ; kd
        xor     esi,esi                 ; from
        xor     edi,edi                 ; to
.l:     cmp     esi,[ebx+8]
        jae     .done
        mov     eax,esi                 ; going: first <= i, (i - first) % step == 0, within count
        sub     eax,[esp]
        jb      .keep
        xor     edx,edx
        div     dword [esp+4]
        test    edx,edx
        jnz     .keep
        cmp     eax,[esp+8]
        jae     .keep
        test    byte [ebp+4],1          ; a reference: released
        jz      .next
        mov     eax,[ebp]
        imul    eax,esi
        add     eax,[ebx+16]
        mov     eax,[eax]
        call    rt_decref
        jmp     .next
.keep:  cmp     esi,edi
        je      .same
        push    esi edi
        mov     ecx,[ebp]
        mov     eax,ecx
        imul    eax,esi
        imul    edi,ecx
        add     edi,[ebx+16]
        mov     esi,eax
        add     esi,[ebx+16]
        rep     movsb
        pop     edi esi
.same:  inc     edi
.next:  inc     esi
        jmp     .l
.done:  mov     [ebx+8],edi
        add     esp,12
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_set_slice : rt_slice_range rt_list_reserve rt_list_copy rt_elem_copy rt_decref rt_sb_need rt_sb_cstr rt_sb_int rt_sb_take rt_raise_valerr
rt_list_set_slice:              ; [esp+4] list, lo, hi, step, flags, a list of the new items: list[lo:hi:step] = them
        push    ebx esi edi ebp
        mov     ebx,[esp+20]
        mov     eax,[esp+40]
        cmp     eax,ebx                 ; xs[1:2] = xs: a copy first
        jne     @f
        mov     edx,[ebx+20]
        call    rt_list_copy
        mov     [esp+40],eax
        push    eax
        jmp     .go
@@:     push    0                       ; [esp] a copy to release
.go:    mov     eax,[ebx+8]
        lea     esi,[esp+28]
        call    rt_slice_range          ; eax = start, edx = step, ecx = count
        mov     ebp,[ebx+20]            ; kd
        mov     edi,[esp+44]            ; the new items
        test    dword [esp+40],4        ; no step, or 1: any number of items
        jnz     .run
        cmp     edx,1
        je      .run                    ; else as many items as places
        cmp     ecx,[edi+8]
        jne     .size
        push    eax edx ecx             ; replace them one by one
.one:   cmp     dword [esp],0
        je      .ones
        mov     eax,[esp+8]             ; place
        mov     ecx,[ebp]
        imul    eax,ecx
        add     eax,[ebx+16]
        test    byte [ebp+4],1
        jz      @f
        push    eax
        mov     eax,[eax]
        call    rt_decref
        pop     eax
@@:     mov     edx,[edi+8]             ; the item: index count of them - left
        sub     edx,[esp]
        imul    edx,[ebp]
        add     edx,[edi+16]
        mov     ecx,ebp
        call    rt_elem_copy
        mov     eax,[esp+4]
        add     [esp+8],eax
        dec     dword [esp]
        jmp     .one
.ones:  add     esp,12
        jmp     .out
.run:   push    eax ecx                 ; [esp] count, [esp+4] start: release them, make room, copy
        mov     esi,eax
.rel:   test    ecx,ecx
        jz      .relz
        test    byte [ebp+4],1
        jz      .relz
        mov     eax,[ebp]
        imul    eax,esi
        add     eax,[ebx+16]
        push    ecx
        mov     eax,[eax]
        call    rt_decref
        pop     ecx
        inc     esi
        dec     ecx
        jmp     .rel
.relz:  mov     ecx,[ebx+8]             ; the new length
        sub     ecx,[esp]
        add     ecx,[edi+8]
        push    ecx
        mov     eax,ebx
        call    rt_list_reserve
        pop     ecx
        push    ecx
        mov     eax,[esp+8]             ; the tail: from start + count to start + len(new)
        add     eax,[esp+4]
        mov     ecx,[ebx+8]
        sub     ecx,eax                 ; its items
        imul    ecx,[ebp]
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     esi,eax
        mov     eax,[esp+8]
        add     eax,[edi+8]
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     edi,eax
        cmp     edi,esi
        jbe     .fwd
        lea     esi,[esi+ecx-1]
        lea     edi,[edi+ecx-1]
        std
        rep     movsb
        cld
        jmp     .moved
.fwd:   rep     movsb
.moved: pop     ecx
        mov     [ebx+8],ecx
        mov     edi,[esp+52]            ; the new items, copied in (the stack: count, start, a copy, 4 registers, the return, 6 arguments)
        xor     esi,esi
.cp:    cmp     esi,[edi+8]
        jae     .cpd
        mov     eax,[esp+4]
        add     eax,esi
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     edx,esi
        imul    edx,[ebp]
        add     edx,[edi+16]
        mov     ecx,ebp
        call    rt_elem_copy
        inc     esi
        jmp     .cp
.cpd:   add     esp,8
.out:   pop     eax                     ; the copy (or 0)
        call    rt_decref
        pop     ebp edi esi ebx
        ret
.size:  push    ecx                     ; ValueError: attempt to assign sequence of size n to extended slice of size m
        push    dword [edi+8]
        push    dword [rt_sb_len]
        mov     eax,rt_msg_extslice1
        call    rt_sb_cstr
        mov     eax,[esp+4]
        xor     edx,edx
        call    rt_sb_int
        mov     eax,rt_msg_extslice2
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_list_set_slice
rt_msg_extslice1 db 'attempt to assign sequence of size ',0
rt_msg_extslice2 db ' to extended slice of size ',0

;;; code rt_list_slice : rt_slice_range rt_list_new rt_list_push rt_elem_copy
rt_list_slice:                  ; [esp+4] list, lo, hi, step, flags, element type -> eax = new list
        push    ebx esi edi ebp
        mov     ebx,[esp+20]
        xor     eax,eax
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+8]
@@:     lea     esi,[esp+24]
        call    rt_slice_range
        mov     ebp,ecx         ; count
        mov     esi,eax         ; index
        mov     edi,edx         ; step
        mov     eax,[esp+40]
        call    rt_list_new
        push    eax
.l:     test    ebp,ebp
        jz      .out
        mov     eax,[esp]
        call    rt_list_push
        mov     ecx,[ebx+20]
        mov     edx,esi
        imul    edx,[ecx]
        add     edx,[ebx+16]
        call    rt_elem_copy
        add     esi,edi
        dec     ebp
        jmp     .l
.out:   pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_list_clear : rt_decref
rt_list_clear:                  ; eax = list: remove every element
        test    eax,eax
        jz      .out
        mov     ecx,[eax+20]
        test    byte [ecx+4],1
        jnz     .drop
        mov     dword [eax+8],0
.out:   ret
.drop:  push    ebx
        mov     ebx,eax
@@:     mov     eax,[ebx+8]
        test    eax,eax
        jz      @f
        dec     eax
        mov     [ebx+8],eax
        mov     ecx,[ebx+16]
        mov     eax,[ecx+eax*4]
        call    rt_decref
        jmp     @b
@@:     pop     ebx
        ret

;;; code rt_list_eq
rt_list_eq:                     ; eax = a, edx = b (lists of one type) -> eax = 1 if equal
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        cmp     ebx,edi
        je      .yes
        test    ebx,ebx         ; None and a list
        jz      .no
        test    edi,edi
        jz      .no
        xor     eax,eax
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+8]
@@:     xor     edx,edx
        test    edi,edi
        jz      @f
        mov     edx,[edi+8]
@@:     cmp     eax,edx
        jne     .no
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .yes
        cmp     esi,[edi+8]
        jae     .no
        mov     ebp,[ebx+20]
        mov     eax,esi
        imul    eax,[ebp]
        mov     edx,eax
        add     eax,[ebx+16]
        add     edx,[edi+16]
        mov     ecx,ebp
        call    dword [ebp+8]
        test    eax,eax
        jz      .no
        inc     esi
        jmp     .l
.yes:   mov     eax,1
        pop     ebp edi esi ebx
        ret
.no:    xor     eax,eax
        pop     ebp edi esi ebx
        ret

;;; code rt_list_cmp
rt_list_cmp:                    ; eax = a, edx = b -> eax = -1, 0, 1: the first unequal elements decide, else the lengths
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        xor     esi,esi
.l:     xor     eax,eax
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+8]
@@:     xor     edx,edx
        test    edi,edi
        jz      @f
        mov     edx,[edi+8]
@@:     cmp     esi,eax
        jae     .end
        cmp     esi,edx
        jae     .end
        mov     ebp,[ebx+20]
        mov     eax,esi
        imul    eax,[ebp]
        mov     edx,eax
        add     eax,[ebx+16]
        add     edx,[edi+16]
        push    eax edx
        mov     ecx,ebp
        call    dword [ebp+8]
        pop     edx ecx
        test    eax,eax
        jz      .diff
        inc     esi
        jmp     .l
.diff:  mov     eax,ecx
        mov     ecx,ebp
        call    dword [ebp+12]
        jmp     .out
.end:   cmp     eax,edx
        mov     eax,0
        je      .out
        jb      @f
        inc     eax
        jmp     .out
@@:     dec     eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_sum
; floats: CPython's compensated (Neumaier) sum
rt_list_sum:                    ; eax = list of int (-> eax) or float (-> st0)
        push    esi
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
        mov     esi,[eax+16]
        mov     edx,[eax+20]
        test    byte [edx+4],2
        jnz     .f
@@:     xor     eax,eax
        jecxz   .out
@@:     add     eax,[esi]
        add     esi,4
        dec     ecx
        jnz     @b
.out:   pop     esi
        ret
.f:     fldz                    ; st0 = c, the compensation
        fldz                    ; st0 = sum
        jecxz   .fdone
.fl:    fld     qword [esi]     ; x, sum, c
        fld     st1
        fadd    st0,st1         ; t = sum + x
        fld     st2
        fabs
        fld     st2
        fabs
        fcompp                  ; |x| vs |sum|
        fnstsw  ax
        sahf
        ja      .xbig           ; |x| > |sum|
        fld     st2             ; c += (sum - t) + x
        fsub    st0,st1
        fadd    st0,st2
        faddp   st4,st0
        jmp     .fn
.xbig:  fld     st1             ; c += (x - t) + sum
        fsub    st0,st1
        fadd    st0,st3
        faddp   st4,st0
.fn:    fstp    st2             ; sum = t; drop x
        fstp    st0
        add     esi,8
        dec     ecx
        jnz     .fl
.fdone: fxch                    ; c added when it is a finite non-zero
        fld     st0
        fsub    st0,st0
        fcomp   st0             ; finite: c - c == c - c (not NaN)
        fnstsw  ax
        sahf
        jp      .nofix
        ftst
        fnstsw  ax
        sahf
        je      .nofix
        faddp   st1,st0
        pop     esi
        ret
.nofix: fstp    st0
        pop     esi
        ret

;;; code rt_list_minmax : rt_panic_value
rt_list_minmax:                 ; eax = list, edx = 1 for max -> eax = address of the smallest / largest element (the first of equals)
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,edx
        test    ebx,ebx
        jz      .empty
        cmp     dword [ebx+8],0
        je      .empty
        mov     edi,[ebx+16]    ; the best so far
        mov     esi,1
.l:     cmp     esi,[ebx+8]
        jae     .done
        mov     ecx,[ebx+20]
        mov     eax,esi
        imul    eax,[ecx]
        add     eax,[ebx+16]
        push    eax
        mov     edx,edi
        call    dword [ecx+12]  ; cmp(x, best)
        pop     edx
        test    ebp,ebp
        jnz     .max
        test    eax,eax
        jns     .n
        mov     edi,edx
        jmp     .n
.max:   test    eax,eax
        jle     .n
        mov     edi,edx
.n:     inc     esi
        jmp     .l
.done:  mov     eax,edi
        pop     ebp edi esi ebx
        ret
.empty: mov     esi,rt_msg_minempty
        test    ebp,ebp
        jz      @f
        mov     esi,rt_msg_maxempty
@@:     jmp     rt_panic_value
;;; data rt_list_minmax
rt_msg_minempty db 'min() iterable argument is empty',0
rt_msg_maxempty db 'max() iterable argument is empty',0

;;; code rt_list_eq_k : rt_list_eq
rt_list_eq_k:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_list_eq

;;; code rt_list_cmp_k : rt_list_cmp rt_cmp_none
rt_list_cmp_k:
        mov     eax,[eax]
        mov     edx,[edx]
        test    eax,eax
        jz      .ln
        test    edx,edx
        jnz     rt_list_cmp
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_list_repr_k : rt_sb_char rt_sb_cstr rt_repr_none
rt_list_repr_k:                 ; repr of a list (for messages)
        cmp     dword [eax],0
        je      rt_repr_none
        push    ebx esi
        mov     ebx,[eax]
        mov     al,'['
        call    rt_sb_char
        xor     esi,esi
.l:     test    ebx,ebx
        jz      .end
        cmp     esi,[ebx+8]
        jae     .end
        test    esi,esi
        jz      @f
        mov     eax,rt_s_lsep
        call    rt_sb_cstr
@@:     mov     ecx,[ebx+20]
        mov     eax,esi
        imul    eax,[ecx]
        add     eax,[ebx+16]
        call    dword [ecx+20]
        inc     esi
        jmp     .l
.end:   mov     al,']'
        pop     esi ebx
        jmp     rt_sb_char
;;; data rt_list_repr_k
rt_s_lsep       db ', ',0

; ---------------------------------------------------------------- tuples
; tuple: +8 its type TD (+0 items, +4 their bytes, then per item: its kd, its
; offset in the tuple), +12 the items

;;; code rt_tuple_new : rt_alloc rt_tuple_free
rt_tuple_new:                   ; eax = TD -> eax = new tuple (items zero)
        push    eax
        mov     eax,[eax+4]
        add     eax,12
        call    rt_alloc
        pop     edx
        mov     dword [eax],1
        mov     dword [eax+4],rt_tuple_free
        mov     [eax+8],edx
        ret

;;; code rt_tuple_free : rt_decref rt_free
rt_tuple_free:                  ; destroy routine of tuples: drop the reference items
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,[ebx+8]
        mov     edi,[esi]
        add     esi,8
.l:     test    edi,edi
        jz      .done
        mov     eax,[esi]
        test    byte [eax+4],1
        jz      .n
        mov     eax,[esi+4]
        mov     eax,[ebx+eax]
        call    rt_decref
.n:     add     esi,8
        dec     edi
        jmp     .l
.done:  mov     eax,ebx
        pop     edi esi ebx
        jmp     rt_free

;;; code rt_tuple_eq
rt_tuple_eq:                    ; eax = a, edx = b (tuples of one type) -> eax = 1 if equal
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,edx
        cmp     ebx,ebp
        je      .yes
        test    ebx,ebx
        jz      .no
        test    ebp,ebp
        jz      .no
        mov     esi,[ebx+8]
        mov     edi,[esi]
        add     esi,8
.l:     test    edi,edi
        jz      .yes
        mov     eax,[esi+4]
        lea     edx,[ebp+eax]
        add     eax,ebx
        mov     ecx,[esi]
        call    dword [ecx+8]
        test    eax,eax
        jz      .no
        add     esi,8
        dec     edi
        jmp     .l
.yes:   mov     eax,1
        pop     ebp edi esi ebx
        ret
.no:    xor     eax,eax
        pop     ebp edi esi ebx
        ret

;;; code rt_tuple_cmp
rt_tuple_cmp:                   ; eax = a, edx = b -> eax = -1, 0, 1: the first unequal items decide, then the lengths
        push    ebx esi edi ebp ; (a shorter tuple's items are of the longer one's first items' types)
        mov     ebx,eax
        mov     ebp,edx
        cmp     ebx,ebp
        je      .eq
        test    ebx,ebx
        jz      .lt
        test    ebp,ebp
        jz      .gt
        mov     esi,[ebx+8]
        mov     edi,[esi]
        mov     eax,[ebp+8]
        mov     eax,[eax]
        push    edi             ; [esp+4] len(a)
        push    eax             ; [esp] len(b)
        cmp     edi,eax
        jbe     .min
        mov     edi,eax
.min:   add     esi,8
.l:     test    edi,edi
        jz      .len
        mov     eax,[esi+4]
        lea     edx,[ebp+eax]
        add     eax,ebx
        mov     ecx,[esi]
        push    eax edx
        call    dword [ecx+8]
        pop     edx ecx
        test    eax,eax
        jz      .diff
        add     esi,8
        dec     edi
        jmp     .l
.diff:  mov     eax,ecx
        mov     ecx,[esi]
        call    dword [ecx+12]
        add     esp,8
        jmp     .out
.len:   pop     edx
        pop     eax
        sub     eax,edx
        jz      .out
        jl      .lt
        jmp     .gt
.lt:    or      eax,-1
        jmp     .out
.gt:    mov     eax,1
        jmp     .out
.eq:    xor     eax,eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_tuple_hash : rt_mul64
rt_tuple_hash:                  ; eax = tuple -> edx:eax = its hash, CPython's (xxHash of the items' hashes)
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,[ebx+8]
        mov     ebp,[esi]       ; items
        add     esi,8
        push    0x27D4EB2F      ; acc = XXPRIME_5
        push    0x165667C5
        mov     edi,ebp
.l:     test    edi,edi
        jz      .end
        mov     eax,[esi+4]
        add     eax,ebx
        mov     ecx,[esi]
        call    dword [ecx+16]  ; the item's hash
        mov     ecx,rt_xxp2
        call    rt_mul64
        add     [esp],eax       ; acc += hash * XXPRIME_2
        adc     [esp+4],edx
        mov     eax,[esp]       ; acc = acc rotated left by 31
        mov     edx,[esp+4]
        mov     ecx,eax
        shl     ecx,31
        push    edx
        shr     edx,1
        or      ecx,edx         ; lo = lo << 31 | hi >> 1
        pop     edx
        shl     edx,31
        shr     eax,1
        or      edx,eax         ; hi = hi << 31 | lo >> 1
        mov     eax,ecx
        mov     ecx,rt_xxp1
        call    rt_mul64        ; acc *= XXPRIME_1
        mov     [esp],eax
        mov     [esp+4],edx
        add     esi,8
        dec     edi
        jmp     .l
.end:   pop     eax
        pop     edx
        xor     ebp,0x1663B4B6  ; acc += items ^ (XXPRIME_5 ^ 3527539)
        add     eax,ebp
        adc     edx,0x27D4EB2F
        cmp     edx,-1
        jne     .out
        cmp     eax,-1
        jne     .out
        mov     eax,1546275796
        xor     edx,edx
.out:   pop     ebp edi esi ebx
        ret
;;; data rt_tuple_hash
align 4
rt_xxp1         dd 0x85EBCA87,0x9E3779B1
rt_xxp2         dd 0x27D4EB4F,0xC2B2AE3D

;;; code rt_tuple_at : rt_panic_index
rt_tuple_at:                    ; eax = tuple (items of one type), edx = index (negative: from the end) -> eax = address of the item
        test    eax,eax
        jz      rt_panic_index_t
        mov     ecx,[eax+8]
        push    ebx
        mov     ebx,[ecx]
        test    edx,edx
        jns     @f
        add     edx,ebx
@@:     cmp     edx,ebx
        pop     ebx
        jae     rt_panic_index_t
        mov     ecx,[ecx+8]     ; the items' kd
        imul    edx,[ecx]
        lea     eax,[eax+12+edx]
        ret

;;; code rt_tuple_eq_k : rt_tuple_eq
rt_tuple_eq_k:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_tuple_eq

;;; code rt_tuple_cmp_k : rt_tuple_cmp rt_cmp_none
rt_tuple_cmp_k:
        mov     eax,[eax]
        mov     edx,[edx]
        test    eax,eax
        jz      .ln
        test    edx,edx
        jnz     rt_tuple_cmp
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_tuple_hash_k : rt_tuple_hash rt_hash_none
rt_tuple_hash_k:
        mov     eax,[eax]
        test    eax,eax
        jz      rt_hash_none
        jmp     rt_tuple_hash

;;; code rt_tuple_repr_k : rt_sb_char rt_sb_cstr
rt_tuple_repr_k:                ; repr of a tuple (for messages)
        push    ebx esi edi
        mov     ebx,[eax]
        test    ebx,ebx
        jz      .none
        mov     al,'('
        call    rt_sb_char
        mov     esi,[ebx+8]
        mov     edi,[esi]
        add     esi,8
        push    edi
.l:     test    edi,edi
        jz      .end
        cmp     edi,[esp]
        je      @f
        mov     eax,rt_s_tsep
        call    rt_sb_cstr
@@:     mov     eax,[esi+4]
        add     eax,ebx
        mov     ecx,[esi]
        call    dword [ecx+20]
        add     esi,8
        dec     edi
        jmp     .l
.end:   pop     edi
        cmp     edi,1
        jne     @f
        mov     al,','
        call    rt_sb_char
@@:     mov     al,')'
        pop     edi esi ebx
        jmp     rt_sb_char
.none:  mov     eax,rt_s_tnone
        pop     edi esi ebx
        jmp     rt_sb_cstr
;;; data rt_tuple_repr_k
rt_s_tsep       db ', ',0
rt_s_tnone      db 'None',0

; ---------------------------------------------------------------- sets
; CPython's hash table, so that sets iterate in CPython's order (the
; interpreter's). set: +8 used, +12 fill (used and dummies), +16 the table,
; +20 its mask (size - 1), +24 the element type (kd), +28 where pop() looks
; first. Entry (24 bytes): +0 the element (8 bytes), +8 its hash (8 bytes),
; +16 state: 0 empty, 1 in use, 2 dummy (removed).

;;; code rt_set_new : rt_alloc rt_set_free
rt_set_new:                     ; eax = element type -> eax = new empty set
        push    eax
        mov     eax,32
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_set_free
        mov     [eax+24],ecx
        mov     dword [eax+20],7
        push    eax
        mov     eax,8*24
        call    rt_alloc
        mov     ecx,eax
        pop     eax
        mov     [eax+16],ecx
        ret

;;; code rt_set_free : rt_free rt_decref
rt_set_free:                    ; destroy routine of sets
        push    ebx esi
        mov     ebx,eax
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .t
        mov     esi,[ebx+20]
.l:     test    esi,esi
        js      .t
        mov     eax,esi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     @f
        mov     eax,[eax]
        call    rt_decref
@@:     dec     esi
        jmp     .l
.t:     mov     eax,[ebx+16]
        call    rt_free
        mov     eax,ebx
        pop     esi ebx
        jmp     rt_free

;;; code rt_set_insert_clean
; Into a table without dummies nor this element: [esp+4] table, mask, &element,
; kd, hash (lo, hi). Copies the element (no reference taken).
rt_set_insert_clean:
        push    ebx esi edi ebp
        mov     esi,[esp+36]    ; perturb = hash
        mov     edi,[esp+40]
        mov     ebx,esi
        and     ebx,[esp+24]    ; i
.probe: mov     eax,ebx
        imul    eax,24
        add     eax,[esp+20]
        cmp     dword [eax+16],0
        je      .found
        mov     ecx,ebx
        add     ecx,9
        cmp     ecx,[esp+24]
        ja      .next
        mov     ecx,9
.lin:   add     eax,24
        cmp     dword [eax+16],0
        je      .found
        dec     ecx
        jnz     .lin
.next:  shrd    esi,edi,5
        shr     edi,5
        lea     ebx,[ebx+ebx*4+1]
        add     ebx,esi
        and     ebx,[esp+24]
        jmp     .probe
.found: mov     edx,[esp+28]
        mov     ecx,[edx]
        mov     [eax],ecx
        mov     ecx,[esp+32]
        cmp     dword [ecx],8
        jne     @f
        mov     ecx,[edx+4]
        mov     [eax+4],ecx
@@:     mov     ecx,[esp+36]
        mov     [eax+8],ecx
        mov     ecx,[esp+40]
        mov     [eax+12],ecx
        mov     dword [eax+16],1
        pop     ebp edi esi ebx
        ret

;;; code rt_set_resize : rt_alloc rt_free rt_set_insert_clean
rt_set_resize:                  ; eax = set, edx = minused: a table of the first size (8, 16 ...) above it
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ecx,8
@@:     cmp     ecx,edx
        ja      @f
        add     ecx,ecx
        jmp     @b
@@:     push    ecx
        mov     eax,ecx
        imul    eax,24
        call    rt_alloc
        mov     edi,eax         ; the new table
        mov     esi,[ebx+16]    ; the old one
        mov     ebp,[ebx+20]
        pop     ecx
        dec     ecx
        mov     [ebx+20],ecx
        mov     [ebx+16],edi
        xor     edx,edx
.l:     cmp     edx,ebp
        ja      .done
        mov     eax,edx
        imul    eax,24
        add     eax,esi
        cmp     dword [eax+16],1
        jne     .n
        push    edx
        push    dword [eax+12]
        push    dword [eax+8]
        push    dword [ebx+24]
        push    eax
        push    dword [ebx+20]
        push    edi
        call    rt_set_insert_clean
        add     esp,24
        pop     edx
.n:     inc     edx
        jmp     .l
.done:  mov     eax,[ebx+8]
        mov     [ebx+12],eax
        mov     eax,esi
        call    rt_free
        pop     ebp edi esi ebx
        ret

;;; code rt_set_add_h : rt_set_resize rt_incref
; [esp+4] set, &element, hash (lo, hi): add it unless there (CPython's
; set_add_entry) -> eax = 1 if added (a reference taken), 0 if it was there
rt_set_add_h:
        push    ebx esi edi ebp
        sub     esp,12          ; +0 the first dummy seen, +4 i, +8 probes left
        mov     ebx,[esp+32]
        mov     esi,[esp+40]    ; perturb = hash
        mov     edi,[esp+44]
        mov     dword [esp],0
        mov     eax,esi
        and     eax,[ebx+20]
        mov     [esp+4],eax
.probe: mov     ebp,[esp+4]
        imul    ebp,24
        add     ebp,[ebx+16]
        mov     dword [esp+8],0
        mov     eax,[esp+4]
        add     eax,9
        cmp     eax,[ebx+20]
        ja      .one
        mov     dword [esp+8],9
.one:   mov     eax,[ebp+16]
        test    eax,eax
        jz      .unused
        cmp     eax,1
        jne     .dummy
        mov     eax,[ebp+8]
        cmp     eax,[esp+40]
        jne     .next
        mov     eax,[ebp+12]
        cmp     eax,[esp+44]
        jne     .next
        mov     eax,ebp
        mov     edx,[esp+36]
        mov     ecx,[ebx+24]
        call    dword [ecx+8]
        test    eax,eax
        jnz     .present
        jmp     .next
.dummy: cmp     dword [esp],0
        jne     .next
        mov     [esp],ebp
.next:  add     ebp,24
        dec     dword [esp+8]
        jns     .one
        shrd    esi,edi,5
        shr     edi,5
        mov     eax,[esp+4]
        lea     eax,[eax+eax*4+1]
        add     eax,esi
        and     eax,[ebx+20]
        mov     [esp+4],eax
        jmp     .probe
.present:
        xor     eax,eax
        jmp     .out
.unused:
        mov     eax,[esp]
        test    eax,eax
        jz      .fresh
        mov     ebp,eax         ; the first dummy instead
        inc     dword [ebx+8]
        call    .store
        jmp     .added
.fresh: inc     dword [ebx+12]
        inc     dword [ebx+8]
        call    .store
        mov     eax,[ebx+12]    ; fill*5 >= mask*3: grow
        lea     eax,[eax+eax*4]
        mov     ecx,[ebx+20]
        lea     ecx,[ecx+ecx*2]
        cmp     eax,ecx
        jb      .added
        mov     edx,[ebx+8]
        cmp     edx,50000
        ja      @f
        shl     edx,2
        jmp     .rs
@@:     add     edx,edx
.rs:    mov     eax,ebx
        call    rt_set_resize
.added: mov     eax,1
.out:   add     esp,12
        pop     ebp edi esi ebx
        ret
.store: mov     edx,[esp+4+36]  ; the element into entry ebp
        mov     ecx,[ebx+24]
        mov     eax,[edx]
        mov     [ebp],eax
        cmp     dword [ecx],8
        jne     @f
        mov     eax,[edx+4]
        mov     [ebp+4],eax
@@:     mov     eax,[esp+4+40]
        mov     [ebp+8],eax
        mov     eax,[esp+4+44]
        mov     [ebp+12],eax
        mov     dword [ebp+16],1
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[ebp]
        call    rt_incref
@@:     ret

;;; code rt_set_add : rt_set_add_h rt_panic_none
rt_set_add:                     ; eax = set, edx = &element: add it (a reference taken) -> eax = 1 if new
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,edx
        mov     ecx,[ebx+24]
        call    dword [ecx+16]
        push    edx
        push    eax
        push    esi
        push    ebx
        call    rt_set_add_h
        add     esp,16
        pop     esi ebx
        ret

;;; code rt_set_find_h
; [esp+4] set, &element, hash (lo, hi) -> eax = its entry, or 0
rt_set_find_h:
        push    ebx esi edi ebp
        push    0               ; i
        mov     ebx,[esp+24]
        mov     esi,[esp+32]
        mov     edi,[esp+36]
        mov     eax,esi
        and     eax,[ebx+20]
        mov     [esp],eax
.probe: mov     ebp,[esp]
        imul    ebp,24
        add     ebp,[ebx+16]
        xor     ecx,ecx
        mov     eax,[esp]
        add     eax,9
        cmp     eax,[ebx+20]
        ja      @f
        mov     ecx,9
@@:     push    ecx             ; probes left
.one:   mov     eax,[ebp+16]
        test    eax,eax
        jz      .nf
        cmp     eax,1
        jne     .next
        mov     eax,[ebp+8]
        cmp     eax,[esp+4+32]
        jne     .next
        mov     eax,[ebp+12]
        cmp     eax,[esp+4+36]
        jne     .next
        mov     eax,ebp
        mov     edx,[esp+4+28]
        mov     ecx,[ebx+24]
        call    dword [ecx+8]
        test    eax,eax
        jnz     .found
.next:  add     ebp,24
        dec     dword [esp]
        jns     .one
        pop     ecx
        shrd    esi,edi,5
        shr     edi,5
        mov     eax,[esp]
        lea     eax,[eax+eax*4+1]
        add     eax,esi
        and     eax,[ebx+20]
        mov     [esp],eax
        jmp     .probe
.found: mov     eax,ebp
        jmp     .out
.nf:    xor     eax,eax
.out:   add     esp,8
        pop     ebp edi esi ebx
        ret

;;; code rt_set_find : rt_set_find_h
rt_set_find:                    ; eax = set, edx = &element -> eax = its entry, or 0
        test    eax,eax
        jz      .no
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,edx
        mov     ecx,[ebx+24]
        call    dword [ecx+16]
        push    edx
        push    eax
        push    esi
        push    ebx
        call    rt_set_find_h
        add     esp,16
        pop     esi ebx
        ret
.no:    xor     eax,eax
        ret

;;; code rt_set_contains : rt_set_find
rt_set_contains:                ; eax = set, edx = &element -> eax = bool
        call    rt_set_find
        test    eax,eax
        setnz   al
        movzx   eax,al
        ret

;;; code rt_set_discard : rt_set_find rt_decref
rt_set_discard:                 ; eax = set, edx = &element: remove it if there -> eax = 1 if it was
        push    ebx
        mov     ebx,eax
        call    rt_set_find
        test    eax,eax
        jz      .out
        mov     dword [eax+16],2
        dec     dword [ebx+8]
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .one
        mov     eax,[eax]
        call    rt_decref
.one:   mov     eax,1
.out:   pop     ebx
        ret

;;; code rt_set_remove : rt_set_discard rt_raise_key rt_panic_none
rt_set_remove:                  ; eax = set, edx = &element: remove it (KeyError if not there)
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        call    rt_set_discard
        test    eax,eax
        jz      .missing
        pop     esi ebx
        ret
.missing:
        mov     eax,esi
        mov     ecx,[ebx+24]
        jmp     rt_raise_key

;;; code rt_set_pop : rt_scratch rt_keyerr_text
rt_set_pop:                     ; eax = set -> eax = rt_scratch holding an element taken out (owned)
        push    ebx
        mov     ebx,eax
        test    ebx,ebx
        jz      .empty
        cmp     dword [ebx+8],0
        je      .empty
        mov     eax,[ebx+28]
        and     eax,[ebx+20]
.l:     mov     ecx,eax
        imul    ecx,24
        add     ecx,[ebx+16]
        cmp     dword [ecx+16],1
        je      .take
        inc     eax
        cmp     eax,[ebx+20]
        jbe     .l
        xor     eax,eax
        jmp     .l
.take:  inc     eax
        mov     [ebx+28],eax
        mov     eax,[ecx]
        mov     [rt_scratch],eax
        mov     eax,[ecx+4]
        mov     [rt_scratch+4],eax
        mov     dword [ecx+16],2
        dec     dword [ebx+8]
        mov     eax,rt_scratch
        pop     ebx
        ret
.empty: mov     esi,rt_msg_popset
        jmp     rt_keyerr_text
;;; data rt_set_pop
rt_msg_popset   db "'pop from an empty set'",0

;;; code rt_set_clear : rt_alloc rt_free rt_decref
rt_set_clear:                   ; eax = set: remove every element
        test    eax,eax
        jz      .out
        push    ebx esi
        mov     ebx,eax
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .t
        mov     esi,[ebx+20]
.l:     test    esi,esi
        js      .t
        mov     eax,esi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     @f
        mov     eax,[eax]
        call    rt_decref
@@:     dec     esi
        jmp     .l
.t:     mov     eax,[ebx+16]
        call    rt_free
        mov     eax,8*24
        call    rt_alloc
        mov     [ebx+16],eax
        mov     dword [ebx+20],7
        xor     eax,eax
        mov     [ebx+8],eax
        mov     [ebx+12],eax
        mov     [ebx+28],eax
        pop     esi ebx
.out:   ret

;;; code rt_set_merge : rt_set_resize rt_set_insert_clean rt_set_add_h rt_incref
rt_set_merge:                   ; eax = set, edx = another: add the other's elements (CPython's set_merge)
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        test    esi,esi
        jz      .out
        cmp     esi,ebx
        je      .out
        cmp     dword [esi+8],0
        je      .out
        mov     eax,[ebx+12]    ; (fill + other's used)*5 >= mask*3: one resize first
        add     eax,[esi+8]
        lea     eax,[eax+eax*4]
        mov     ecx,[ebx+20]
        lea     ecx,[ecx+ecx*2]
        cmp     eax,ecx
        jb      @f
        mov     edx,[ebx+8]
        add     edx,[esi+8]
        add     edx,edx
        mov     eax,ebx
        call    rt_set_resize
@@:     cmp     dword [ebx+12],0
        jne     .add
        mov     eax,[ebx+20]
        cmp     eax,[esi+20]
        jne     .clean
        mov     eax,[esi+12]
        cmp     eax,[esi+8]
        jne     .clean
        mov     ecx,[esi+20]    ; empty, the same size, no dummies: copy the table
        inc     ecx
        imul    ecx,24
        mov     edi,[ebx+16]
        push    esi
        mov     esi,[esi+16]
        rep     movsb
        pop     esi
        mov     eax,[esi+12]
        mov     [ebx+12],eax
        mov     eax,[esi+8]
        mov     [ebx+8],eax
        call    .refs
        jmp     .out
.clean: xor     edi,edi         ; empty: insert into the clean table
.cl:    cmp     edi,[esi+20]
        ja      .cdone
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .cn
        push    dword [eax+12]
        push    dword [eax+8]
        push    dword [ebx+24]
        push    eax
        push    dword [ebx+20]
        push    dword [ebx+16]
        call    rt_set_insert_clean
        add     esp,24
.cn:    inc     edi
        jmp     .cl
.cdone: mov     eax,[esi+8]
        mov     [ebx+8],eax
        mov     [ebx+12],eax
        call    .refs
        jmp     .out
.add:   xor     edi,edi         ; otherwise one by one
.al:    cmp     edi,[esi+20]
        ja      .out
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .an
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    ebx
        call    rt_set_add_h
        add     esp,16
.an:    inc     edi
        jmp     .al
.out:   pop     ebp edi esi ebx
        ret
.refs:  mov     ecx,[ebx+24]    ; a reference to every element now in the set
        test    byte [ecx+4],1
        jz      .rdone
        mov     edi,[ebx+20]
.rl:    test    edi,edi
        js      .rdone
        mov     eax,edi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     @f
        mov     eax,[eax]
        call    rt_incref
@@:     dec     edi
        jmp     .rl
.rdone: ret

;;; code rt_set_copy : rt_set_new rt_set_merge
rt_set_copy:                    ; eax = set, edx = element type -> eax = new set of its elements (CPython's set.copy)
        push    ebx
        mov     ebx,eax
        mov     eax,edx
        call    rt_set_new
        push    eax
        mov     edx,ebx
        call    rt_set_merge
        pop     eax
        pop     ebx
        ret

;;; code rt_set_add_list : rt_set_add
rt_set_add_list:                ; eax = set, edx = list: add its elements
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        xor     edi,edi
.l:     test    esi,esi
        jz      .out
        cmp     edi,[esi+8]
        jae     .out
        mov     ecx,[esi+20]
        mov     edx,edi
        imul    edx,[ecx]
        add     edx,[esi+16]
        mov     eax,ebx
        call    rt_set_add
        inc     edi
        jmp     .l
.out:   pop     edi esi ebx
        ret

;;; code rt_set_from_list : rt_set_new rt_set_add_list
rt_set_from_list:               ; eax = list, edx = element type -> eax = new set of its elements
        push    ebx
        mov     ebx,eax
        mov     eax,edx
        call    rt_set_new
        push    eax
        mov     edx,ebx
        call    rt_set_add_list
        pop     eax
        pop     ebx
        ret

;;; code rt_set_to_list : rt_list_new rt_list_reserve rt_list_push rt_elem_copy
rt_set_to_list:                 ; eax = set, edx = element type -> eax = new list of its elements, in its order
        push    ebx esi
        mov     ebx,eax
        mov     eax,edx
        call    rt_list_new
        push    eax
        test    ebx,ebx
        jz      .out
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        xor     esi,esi
.l:     cmp     esi,[ebx+20]
        ja      .out
        mov     edx,esi
        imul    edx,24
        add     edx,[ebx+16]
        cmp     dword [edx+16],1
        jne     .n
        mov     eax,[esp]
        push    edx
        call    rt_list_push
        pop     edx
        mov     ecx,[ebx+24]
        call    rt_elem_copy
.n:     inc     esi
        jmp     .l
.out:   pop     eax
        pop     esi ebx
        ret

;;; code rt_set_next
rt_set_next:                    ; eax = set, edx = position -> eax = the first entry in use from there (its index), or -1
        test    eax,eax
        jz      .end
.l:     cmp     edx,[eax+20]
        ja      .end
        mov     ecx,edx
        imul    ecx,24
        add     ecx,[eax+16]
        cmp     dword [ecx+16],1
        je      .found
        inc     edx
        jmp     .l
.found: mov     eax,edx
        ret
.end:   or      eax,-1
        ret

;;; code rt_set_compact : rt_set_resize
rt_set_compact:                 ; eax = set: after removals, more than a quarter dummies: a new table
        mov     ecx,[eax+12]
        sub     ecx,[eax+8]
        mov     edx,[eax+20]
        shr     edx,2
        cmp     ecx,edx
        jbe     .out
        mov     edx,[eax+8]
        cmp     edx,50000
        ja      @f
        shl     edx,2
        jmp     rt_set_resize
@@:     add     edx,edx
        jmp     rt_set_resize
.out:   ret

;;; code rt_set_diff_update : rt_set_clear rt_set_op rt_set_find_h rt_set_compact rt_decref
rt_set_diff_update:             ; eax = set, edx = another: its elements removed (CPython's set_difference_update)
        cmp     eax,edx
        je      rt_set_clear
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        test    esi,esi
        jz      .out
        xor     edi,edi         ; the other is much bigger: walk the intersection instead
        mov     eax,[esi+8]
        shr     eax,3
        cmp     eax,[ebx+8]
        jbe     .walk
        mov     eax,ebx
        mov     edx,esi
        mov     ecx,1
        call    rt_set_op
        mov     esi,eax
        mov     edi,eax         ; dropped at the end
.walk:  push    edi
        xor     edi,edi
.l:     cmp     edi,[esi+20]
        ja      .done
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .n
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    ebx
        call    rt_set_find_h
        add     esp,16
        test    eax,eax
        jz      .n
        mov     dword [eax+16],2
        dec     dword [ebx+8]
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .n
        mov     eax,[eax]
        call    rt_decref
.n:     inc     edi
        jmp     .l
.done:  pop     eax
        call    rt_decref
        mov     eax,ebx
        call    rt_set_compact
.out:   pop     edi esi ebx
        ret

;;; code rt_set_xor_update : rt_set_clear rt_set_find_h rt_set_add_h rt_decref
rt_set_xor_update:              ; eax = set, edx = another: its elements toggled (CPython's set_symmetric_difference_update)
        cmp     eax,edx
        je      rt_set_clear
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        xor     edi,edi
.l:     test    esi,esi
        jz      .out
        cmp     edi,[esi+20]
        ja      .out
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .n
        push    eax
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    ebx
        call    rt_set_find_h
        add     esp,16
        pop     edx
        test    eax,eax
        jz      .add
        mov     dword [eax+16],2
        dec     dword [ebx+8]
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .n
        mov     eax,[eax]
        call    rt_decref
        jmp     .n
.add:   push    dword [edx+12]
        push    dword [edx+8]
        push    edx
        push    ebx
        call    rt_set_add_h
        add     esp,16
.n:     inc     edi
        jmp     .l
.out:   pop     edi esi ebx
        ret

;;; code rt_set_op : rt_set_new rt_set_copy rt_set_merge rt_set_find_h rt_set_add_h rt_set_diff_update rt_set_xor_update
; a | b, a & b, a - b, a ^ b: the interpreter's (CPython's) algorithms, which
; decide the order of the result
rt_set_op:                      ; eax = a, edx = b, ecx = 0 |, 1 &, 2 -, 3 ^ -> eax = new set
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        cmp     ecx,1
        je      .and
        cmp     ecx,2
        je      .sub
        cmp     ecx,3
        je      .xor
        mov     eax,ebx         ; |: a copy of a, b merged in
        mov     edx,[ebx+24]
        call    rt_set_copy
        push    eax
        mov     edx,esi
        call    rt_set_merge
        pop     eax
        jmp     .out
.and:   mov     eax,[ebx+24]    ; &: the elements of the smaller found in the other
        call    rt_set_new
        mov     ebp,eax
        mov     eax,[esi+8]
        cmp     eax,[ebx+8]
        jbe     @f
        xchg    ebx,esi         ; ebx = the bigger, esi = the one walked
@@:     xor     edi,edi
.al:    cmp     edi,[esi+20]
        ja      .adone
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .an
        push    eax
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    ebx
        call    rt_set_find_h
        add     esp,16
        pop     edx
        test    eax,eax
        jz      .an
        push    dword [edx+12]
        push    dword [edx+8]
        push    edx
        push    ebp
        call    rt_set_add_h
        add     esp,16
.an:    inc     edi
        jmp     .al
.adone: mov     eax,ebp
        jmp     .out
.sub:   mov     eax,[ebx+8]     ; -: a big: a copy of a, b's elements removed
        shr     eax,2
        cmp     eax,[esi+8]
        jbe     .subw
        mov     eax,ebx
        mov     edx,[ebx+24]
        call    rt_set_copy
        mov     ebp,eax
        mov     edx,esi
        call    rt_set_diff_update
        mov     eax,ebp
        jmp     .out
.subw:  mov     eax,[ebx+24]    ; else a's elements not in b
        call    rt_set_new
        mov     ebp,eax
        xor     edi,edi
.wl:    cmp     edi,[ebx+20]
        ja      .wdone
        mov     eax,edi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     .wn
        push    eax
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    esi
        call    rt_set_find_h
        add     esp,16
        pop     edx
        test    eax,eax
        jnz     .wn
        push    dword [edx+12]
        push    dword [edx+8]
        push    edx
        push    ebp
        call    rt_set_add_h
        add     esp,16
.wn:    inc     edi
        jmp     .wl
.wdone: mov     eax,ebp
        jmp     .out
.xor:   mov     eax,esi         ; ^: a copy of b, a's elements toggled in
        mov     edx,[esi+24]
        call    rt_set_copy
        mov     ebp,eax
        mov     edx,ebx
        call    rt_set_xor_update
        mov     eax,ebp
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_set_iop : rt_set_op rt_set_merge rt_set_diff_update rt_set_xor_update rt_decref
rt_set_iop:                     ; eax = a, edx = b, ecx = 0 |=, 1 &=, 2 -=, 3 ^=: a changed in place (CPython's way)
        test    ecx,ecx
        jz      rt_set_merge
        cmp     ecx,2
        je      rt_set_diff_update
        cmp     ecx,3
        je      rt_set_xor_update
        push    ebx esi edi     ; &=: the intersection's table becomes a's
        mov     ebx,eax
        call    rt_set_op
        mov     edi,eax
        mov     eax,[ebx+16]
        mov     ecx,[edi+16]
        mov     [ebx+16],ecx
        mov     [edi+16],eax
        mov     eax,[ebx+20]
        mov     ecx,[edi+20]
        mov     [ebx+20],ecx
        mov     [edi+20],eax
        mov     eax,[ebx+8]
        mov     ecx,[edi+8]
        mov     [ebx+8],ecx
        mov     [edi+8],eax
        mov     eax,[ebx+12]
        mov     ecx,[edi+12]
        mov     [ebx+12],ecx
        mov     [edi+12],eax
        mov     dword [ebx+28],0
        mov     eax,edi         ; a's old elements go with it
        call    rt_decref
        pop     edi esi ebx
        ret

;;; code rt_set_le : rt_set_find_h
rt_set_le:                      ; eax = a, edx = b -> eax = 1 if every element of a is in b
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        xor     eax,eax
        test    ebx,ebx
        jz      .yes
        mov     ecx,[ebx+8]
        test    esi,esi
        jz      .empty
        cmp     ecx,[esi+8]
        ja      .no
        xor     edi,edi
.l:     cmp     edi,[ebx+20]
        ja      .yes
        mov     eax,edi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     .n
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    esi
        call    rt_set_find_h
        add     esp,16
        test    eax,eax
        jz      .no
.n:     inc     edi
        jmp     .l
.empty: test    ecx,ecx
        jnz     .no
.yes:   mov     eax,1
        pop     edi esi ebx
        ret
.no:    xor     eax,eax
        pop     edi esi ebx
        ret

;;; code rt_set_eq : rt_set_le
rt_set_eq:                      ; eax = a, edx = b -> eax = 1 if the same elements
        cmp     eax,edx
        je      .yes
        test    eax,eax
        jz      .no
        test    edx,edx
        jz      .no
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
@@:     push    ecx
        xor     ecx,ecx
        test    edx,edx
        jz      @f
        mov     ecx,[edx+8]
@@:     cmp     ecx,[esp]
        pop     ecx
        jne     .no
        jmp     rt_set_le
.yes:   mov     eax,1
        ret
.no:    xor     eax,eax
        ret

;;; code rt_set_disjoint : rt_set_find_h
rt_set_disjoint:                ; eax = a, edx = b -> eax = 1 if no element is in both
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        test    ebx,ebx
        jz      .yes
        test    esi,esi
        jz      .yes
        mov     eax,[esi+8]     ; walk the smaller
        cmp     eax,[ebx+8]
        jae     @f
        xchg    ebx,esi
@@:     xor     edi,edi
.l:     cmp     edi,[ebx+20]
        ja      .yes
        mov     eax,edi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     .n
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    esi
        call    rt_set_find_h
        add     esp,16
        test    eax,eax
        jnz     .no
.n:     inc     edi
        jmp     .l
.yes:   mov     eax,1
        pop     edi esi ebx
        ret
.no:    xor     eax,eax
        pop     edi esi ebx
        ret

;;; code rt_set_eq_k : rt_set_eq
rt_set_eq_k:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_set_eq

;;; code rt_set_repr_k : rt_sb_char rt_sb_cstr rt_repr_none
rt_set_repr_k:                  ; repr of a set (for messages)
        cmp     dword [eax],0
        je      rt_repr_none
        push    ebx esi edi
        mov     ebx,[eax]
        test    ebx,ebx
        jz      .empty
        cmp     dword [ebx+8],0
        je      .empty
        mov     al,'{'
        call    rt_sb_char
        xor     esi,esi
        xor     edi,edi
.l:     cmp     esi,[ebx+20]
        ja      .end
        mov     eax,esi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     .n
        test    edi,edi
        jz      @f
        push    eax
        mov     eax,rt_s_ssep
        call    rt_sb_cstr
        pop     eax
@@:     inc     edi
        mov     ecx,[ebx+24]
        call    dword [ecx+20]
.n:     inc     esi
        jmp     .l
.end:   mov     al,'}'
        pop     edi esi ebx
        jmp     rt_sb_char
.empty: mov     eax,rt_s_sempty
        pop     edi esi ebx
        jmp     rt_sb_cstr
;;; data rt_set_repr_k
rt_s_ssep       db ', ',0
rt_s_sempty     db 'set()',0

; ---------------------------------------------------------------- dicts
; Insertion ordered. dict: +8 length, +12 capacity, +16 the keys (8 bytes
; each), +20 the values, +24 the key type (kd), +28 an index (a table of
; entry numbers, -1: empty; at most half full), +32 its mask, +36 the value
; type (kd), +40 the keys' hashes (their low 32 bits).

;;; code rt_dict_new : rt_alloc rt_dict_free
rt_dict_new:                    ; eax = key type, edx = value type -> eax = new empty dict
        push    edx
        push    eax
        mov     eax,44
        call    rt_alloc
        pop     ecx
        mov     [eax+24],ecx
        pop     ecx
        mov     [eax+36],ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_dict_free
        ret

;;; code rt_dict_free : rt_free rt_decref
rt_dict_free:                   ; destroy routine of dicts
        push    ebx esi
        mov     ebx,eax
        mov     esi,[ebx+8]
.l:     dec     esi
        js      .done
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[ebx+16]
        mov     eax,[eax+esi*8]
        call    rt_decref
@@:     mov     ecx,[ebx+36]
        test    byte [ecx+4],1
        jz      .l
        mov     eax,[ebx+20]
        mov     eax,[eax+esi*4]
        call    rt_decref
        jmp     .l
.done:  mov     eax,[ebx+16]
        call    rt_free
        mov     eax,[ebx+20]
        call    rt_free
        mov     eax,[ebx+28]
        call    rt_free
        mov     eax,[ebx+40]
        call    rt_free
        mov     eax,ebx
        pop     esi ebx
        jmp     rt_free

;;; code rt_dict_lookup
rt_dict_lookup:                 ; ebx = dict (with an index), esi = &key, edi = its hash -> eax = its entry or -1, edx = the index slot where the search ended
        push    ebp
        mov     ebp,edi
        and     ebp,[ebx+32]
.l:     mov     edx,[ebx+28]
        lea     edx,[edx+ebp*4]
        mov     eax,[edx]
        cmp     eax,-1
        je      .out
        mov     ecx,[ebx+40]
        cmp     [ecx+eax*4],edi
        jne     .n
        push    eax edx
        shl     eax,3
        add     eax,[ebx+16]
        mov     edx,esi
        mov     ecx,[ebx+24]
        call    dword [ecx+8]
        test    eax,eax
        pop     edx eax
        jnz     .out
.n:     inc     ebp
        and     ebp,[ebx+32]
        jmp     .l
.out:   pop     ebp
        ret

;;; code rt_dict_find : rt_dict_lookup
rt_dict_find:                   ; eax = dict, edx = &key -> eax = its entry, or -1
        test    eax,eax
        jz      .nf
        cmp     dword [eax+8],0
        je      .nf
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,edx
        mov     ecx,[ebx+24]
        call    dword [ecx+16]
        mov     edi,eax
        call    rt_dict_lookup
        pop     edi esi ebx
        ret
.nf:    or      eax,-1
        ret

;;; code rt_dict_reindex : rt_alloc rt_free
rt_dict_reindex:                ; ebx = dict: its index made again (sized for its capacity)
        push    esi edi
        mov     ecx,8
        mov     eax,[ebx+12]
        add     eax,eax
@@:     cmp     ecx,eax
        jae     @f
        add     ecx,ecx
        jmp     @b
@@:     lea     eax,[ecx-1]
        cmp     dword [ebx+28],0
        je      .new
        cmp     eax,[ebx+32]
        je      .fill
        push    ecx
        mov     eax,[ebx+28]
        call    rt_free
        pop     ecx
.new:   lea     eax,[ecx-1]
        mov     [ebx+32],eax
        lea     eax,[ecx*4]
        call    rt_alloc
        mov     [ebx+28],eax
.fill:  mov     ecx,[ebx+32]
        inc     ecx
        mov     edi,[ebx+28]
        or      eax,-1
        rep     stosd
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     eax,[ebx+40]
        mov     eax,[eax+esi*4]
        mov     edx,[ebx+28]
.p:     and     eax,[ebx+32]
        cmp     dword [edx+eax*4],-1
        je      @f
        inc     eax
        jmp     .p
@@:     mov     [edx+eax*4],esi
        inc     esi
        jmp     .l
.out:   pop     edi esi
        ret

;;; code rt_dict_grow : rt_alloc rt_free rt_dict_reindex
rt_dict_grow:                   ; ebx = dict: room for more entries
        push    esi edi ebp
        mov     ebp,[ebx+12]
        add     ebp,ebp
        cmp     ebp,4
        jae     @f
        mov     ebp,4
@@:     lea     eax,[ebp*8]     ; keys
        mov     ecx,[ebx+8]
        shl     ecx,3
        lea     edx,[ebx+16]
        call    .move
        mov     eax,[ebx+36]    ; values
        mov     eax,[eax]
        mov     ecx,eax
        imul    eax,ebp
        imul    ecx,[ebx+8]
        lea     edx,[ebx+20]
        call    .move
        lea     eax,[ebp*4]     ; hashes
        mov     ecx,[ebx+8]
        shl     ecx,2
        lea     edx,[ebx+40]
        call    .move
        mov     [ebx+12],ebp
        call    rt_dict_reindex
        pop     ebp edi esi
        ret
.move:  push    ecx edx         ; eax = new bytes, ecx = bytes in use, edx = where the array's pointer is: a bigger copy
        call    rt_alloc
        pop     edx ecx
        mov     edi,eax
        mov     esi,[edx]
        push    eax
        rep     movsb
        mov     eax,[edx]
        push    edx
        call    rt_free
        pop     edx
        pop     eax
        mov     [edx],eax
        ret

;;; code rt_dict_slot : rt_dict_lookup rt_dict_grow rt_incref rt_panic_none
rt_dict_slot:                   ; eax = dict, edx = &key -> eax = address of its value (a new entry, value zero, if missing)
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,edx
        mov     ecx,[ebx+24]
        call    dword [ecx+16]
        mov     edi,eax
        cmp     dword [ebx+8],0
        je      .new
        call    rt_dict_lookup
        test    eax,eax
        jns     .have
.new:   mov     eax,[ebx+8]
        cmp     eax,[ebx+12]
        jb      @f
        call    rt_dict_grow
@@:     call    rt_dict_lookup  ; where it goes
        mov     eax,[ebx+8]
        mov     [edx],eax
        inc     dword [ebx+8]
        mov     ecx,[ebx+40]
        mov     [ecx+eax*4],edi
        push    eax
        mov     ecx,eax         ; the key, a reference taken
        shl     ecx,3
        add     ecx,[ebx+16]
        mov     eax,[esi]
        mov     [ecx],eax
        mov     edx,[ebx+24]
        cmp     dword [edx],8
        jne     @f
        mov     eax,[esi+4]
        mov     [ecx+4],eax
@@:     test    byte [edx+4],1
        jz      @f
        mov     eax,[ecx]
        call    rt_incref
@@:     pop     eax             ; the value: zero
        mov     ecx,[ebx+36]
        mov     ecx,[ecx]
        imul    eax,ecx
        add     eax,[ebx+20]
        mov     dword [eax],0
        mov     dword [eax+ecx-4],0
        pop     edi esi ebx
        ret
.have:  mov     ecx,[ebx+36]
        imul    eax,[ecx]
        add     eax,[ebx+20]
        pop     edi esi ebx
        ret

;;; code rt_dict_get : rt_dict_find rt_raise_key rt_panic_none
rt_dict_get:                    ; eax = dict, edx = &key -> eax = address of its value (KeyError if missing)
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        call    rt_dict_find
        test    eax,eax
        js      .miss
        mov     ecx,[ebx+36]
        imul    eax,[ecx]
        add     eax,[ebx+20]
        pop     esi ebx
        ret
.miss:  test    ebx,ebx
        jz      rt_panic_none
        mov     eax,esi
        mov     ecx,[ebx+24]
        jmp     rt_raise_key

;;; code rt_dict_del : rt_dict_find rt_dict_reindex rt_raise_key rt_decref rt_scratch rt_panic_none
rt_dict_del:                    ; eax = dict, edx = &key -> eax = rt_scratch holding its value (owned); KeyError if missing
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        call    rt_dict_find
        test    eax,eax
        js      .miss
        mov     edi,eax         ; the entry
        mov     ecx,[ebx+36]    ; its value -> rt_scratch
        mov     ebp,[ecx]
        mov     edx,eax
        imul    edx,ebp
        add     edx,[ebx+20]
        mov     eax,[edx]
        mov     [rt_scratch],eax
        mov     eax,[edx+ebp-4]
        mov     [rt_scratch+ebp-4],eax
        mov     ecx,[ebx+24]    ; its key dropped
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[ebx+16]
        mov     eax,[eax+edi*8]
        call    rt_decref
@@:     mov     eax,[ebx+8]     ; the entries after it move down
        dec     eax
        mov     [ebx+8],eax
        sub     eax,edi
        mov     ebp,eax
        push    edi
        mov     esi,[ebx+16]    ; keys
        lea     edi,[esi+edi*8]
        lea     esi,[edi+8]
        lea     ecx,[ebp*2]
        rep     movsd
        mov     edi,[esp]       ; hashes
        mov     esi,[ebx+40]
        lea     edi,[esi+edi*4]
        lea     esi,[edi+4]
        mov     ecx,ebp
        rep     movsd
        mov     eax,[ebx+36]    ; values
        mov     eax,[eax]
        mov     ecx,ebp
        imul    ecx,eax
        mov     edi,[esp]
        imul    edi,eax
        add     edi,[ebx+20]
        lea     esi,[edi+eax]
        rep     movsb
        add     esp,4
        call    rt_dict_reindex
        mov     eax,rt_scratch
        pop     ebp edi esi ebx
        ret
.miss:  test    ebx,ebx
        jz      rt_panic_none
        mov     eax,esi
        mov     ecx,[ebx+24]
        jmp     rt_raise_key

;;; code rt_dict_keys : rt_list_new rt_list_reserve rt_list_push rt_elem_copy rt_panic_none
rt_dict_keys:                   ; eax = dict -> eax = new list of its keys
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi
        mov     ebx,eax
        mov     eax,[ebx+24]
        call    rt_list_new
        push    eax
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     eax,[esp]
        call    rt_list_push
        mov     edx,[ebx+16]
        lea     edx,[edx+esi*8]
        mov     ecx,[ebx+24]
        call    rt_elem_copy
        inc     esi
        jmp     .l
.out:   pop     eax
        pop     esi ebx
        ret

;;; code rt_dict_values : rt_list_new rt_list_reserve rt_list_push rt_elem_copy rt_panic_none
rt_dict_values:                 ; eax = dict -> eax = new list of its values
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi
        mov     ebx,eax
        mov     eax,[ebx+36]
        call    rt_list_new
        push    eax
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     eax,[esp]
        call    rt_list_push
        mov     ecx,[ebx+36]
        mov     edx,esi
        imul    edx,[ecx]
        add     edx,[ebx+20]
        call    rt_elem_copy
        inc     esi
        jmp     .l
.out:   pop     eax
        pop     esi ebx
        ret

;;; code rt_dict_items : rt_list_new rt_list_reserve rt_list_push rt_tuple_new rt_elem_copy rt_kd_tuple rt_panic_none
rt_dict_items:                  ; eax = dict, edx = TD of tuple[K, V] -> eax = new list of (key, value)
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,edx
        mov     eax,rt_kd_tuple
        call    rt_list_new
        push    eax
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     eax,ebp
        call    rt_tuple_new
        mov     edi,eax
        mov     eax,[ebp+12]    ; the key
        add     eax,edi
        mov     edx,[ebx+16]
        lea     edx,[edx+esi*8]
        mov     ecx,[ebx+24]
        call    rt_elem_copy
        mov     eax,[ebp+20]    ; the value
        add     eax,edi
        mov     ecx,[ebx+36]
        mov     edx,esi
        imul    edx,[ecx]
        add     edx,[ebx+20]
        call    rt_elem_copy
        mov     eax,[esp]
        call    rt_list_push
        mov     [eax],edi
        inc     esi
        jmp     .l
.out:   pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_dict_clear : rt_decref
rt_dict_clear:                  ; eax = dict: remove every entry
        test    eax,eax
        jz      .out
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,[ebx+8]
.l:     dec     esi
        js      .done
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[ebx+16]
        mov     eax,[eax+esi*8]
        call    rt_decref
@@:     mov     ecx,[ebx+36]
        test    byte [ecx+4],1
        jz      .l
        mov     eax,[ebx+20]
        mov     eax,[eax+esi*4]
        call    rt_decref
        jmp     .l
.done:  mov     dword [ebx+8],0
        mov     edi,[ebx+28]
        test    edi,edi
        jz      @f
        mov     ecx,[ebx+32]
        inc     ecx
        or      eax,-1
        rep     stosd
@@:     pop     edi esi ebx
.out:   ret

;;; code rt_dict_update : rt_dict_slot rt_elem_copy rt_decref rt_scratch
rt_dict_update:                 ; eax = destination, edx = source: every entry of the source set in the destination
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        xor     edi,edi
.l:     test    esi,esi
        jz      .out
        cmp     edi,[esi+8]
        jae     .out
        mov     eax,ebx
        mov     edx,[esi+16]
        lea     edx,[edx+edi*8]
        call    rt_dict_slot
        mov     ebp,eax
        mov     ecx,[ebx+36]    ; the old value (a reference) is dropped after the copy
        push    dword [ebp]
        mov     ecx,[esi+36]
        mov     edx,edi
        imul    edx,[ecx]
        add     edx,[esi+20]
        mov     eax,ebp
        call    rt_elem_copy
        pop     eax
        mov     ecx,[ebx+36]
        test    byte [ecx+4],1
        jz      @f
        call    rt_decref
@@:     inc     edi
        jmp     .l
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_dict_copy : rt_dict_new rt_dict_update
rt_dict_copy:                   ; eax = dict -> eax = new dict with its entries
        push    ebx
        mov     ebx,eax
        mov     eax,[ebx+24]
        mov     edx,[ebx+36]
        call    rt_dict_new
        push    eax
        mov     edx,ebx
        call    rt_dict_update
        pop     eax
        pop     ebx
        ret

;;; code rt_dict_eq : rt_dict_find
rt_dict_eq:                     ; eax = a, edx = b (dicts of one type) -> eax = 1 if the same keys map to equal values
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        cmp     ebx,esi
        je      .yes
        test    ebx,ebx
        jz      .no
        test    esi,esi
        jz      .no
        xor     eax,eax
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+8]
@@:     xor     edx,edx
        test    esi,esi
        jz      @f
        mov     edx,[esi+8]
@@:     cmp     eax,edx
        jne     .no
        xor     edi,edi
.l:     test    ebx,ebx
        jz      .yes
        cmp     edi,[ebx+8]
        jae     .yes
        mov     eax,esi
        mov     edx,[ebx+16]
        lea     edx,[edx+edi*8]
        call    rt_dict_find
        test    eax,eax
        js      .no
        mov     ebp,[ebx+36]
        imul    eax,[ebp]
        add     eax,[esi+20]
        mov     edx,edi
        imul    edx,[ebp]
        add     edx,[ebx+20]
        mov     ecx,ebp
        call    dword [ebp+8]
        test    eax,eax
        jz      .no
        inc     edi
        jmp     .l
.yes:   mov     eax,1
        pop     ebp edi esi ebx
        ret
.no:    xor     eax,eax
        pop     ebp edi esi ebx
        ret

;;; code rt_dict_eq_k : rt_dict_eq
rt_dict_eq_k:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_dict_eq

;;; code rt_dict_repr_k : rt_sb_char rt_sb_cstr rt_repr_none
rt_dict_repr_k:                 ; repr of a dict (for messages)
        cmp     dword [eax],0
        je      rt_repr_none
        push    ebx esi
        mov     ebx,[eax]
        mov     al,'{'
        call    rt_sb_char
        xor     esi,esi
.l:     test    ebx,ebx
        jz      .end
        cmp     esi,[ebx+8]
        jae     .end
        test    esi,esi
        jz      @f
        mov     eax,rt_s_dsep
        call    rt_sb_cstr
@@:     mov     eax,[ebx+16]
        lea     eax,[eax+esi*8]
        mov     ecx,[ebx+24]
        call    dword [ecx+20]
        mov     eax,rt_s_dcol
        call    rt_sb_cstr
        mov     ecx,[ebx+36]
        mov     eax,esi
        imul    eax,[ecx]
        add     eax,[ebx+20]
        call    dword [ecx+20]
        inc     esi
        jmp     .l
.end:   mov     al,'}'
        pop     esi ebx
        jmp     rt_sb_char
;;; data rt_dict_repr_k
rt_s_dsep       db ', ',0
rt_s_dcol       db ': ',0
; ---------------------------------------------------------------- objects and buffers

;;; code rt_obj_new : rt_alloc
rt_obj_new:                     ; eax = size, edx = vtable, ecx = destroy routine -> eax = new zeroed object
        push    edx ecx
        call    rt_alloc
        pop     ecx edx
        mov     dword [eax],1
        mov     [eax+4],ecx
        mov     [eax+8],edx
        ret

;;; code rt_isinstance
rt_isinstance:                  ; eax = object, edx = vtable -> eax = bool
        test    eax,eax
        jz      .no
        mov     eax,[eax+8]
@@:     cmp     eax,edx
        je      .yes
        mov     eax,[eax+4]
        test    eax,eax
        jnz     @b
.no:    xor     eax,eax
        ret
.yes:   mov     eax,1
        ret

;;; code rt_buf_new : rt_alloc rt_free
rt_buf_new:                     ; eax = length -> eax = new zeroed buffer
        test    eax,eax
        jns     @f
        xor     eax,eax
@@:     push    eax
        add     eax,13
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_free
        mov     [eax+8],ecx
        ret

;;; code rt_buf_from_str : rt_buf_new
rt_buf_from_str:                ; eax = str -> eax = new buffer with its bytes
        push    esi edi
        mov     esi,eax
        xor     eax,eax
        test    esi,esi
        jz      @f
        mov     eax,[esi+8]
@@:     push    eax
        call    rt_buf_new
        pop     ecx
        jecxz   @f
        lea     edi,[eax+12]
        add     esi,12
        rep     movsb
@@:     pop     edi esi
        ret

;;; code rt_buf_at : rt_panic
rt_buf_at:                      ; eax = buffer, edx = offset, ecx = bytes -> eax = their address (bounds checked)
        test    eax,eax
        jz      .bad
        test    edx,edx
        js      .bad
        add     ecx,edx
        jc      .bad
        cmp     ecx,[eax+8]
        ja      .bad
        lea     eax,[eax+12+edx]
        ret
.bad:   mov     esi,rt_msg_buf
        jmp     rt_panic
;;; data rt_buf_at
rt_msg_buf      db 'IndexError: buffer offset out of range',0

;;; code rt_buf_peek_str : rt_buf_at rt_str_new
rt_buf_peek_str:                ; eax = buffer, edx = offset, ecx = length -> eax = new str
        push    esi edi
        push    ecx
        call    rt_buf_at
        mov     esi,eax
        mov     eax,[esp]
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret

;;; code rt_buf_peek_bytes : rt_buf_at rt_str_new
rt_buf_peek_bytes:              ; eax = buffer, edx = offset, ecx = length -> eax = new bytes
        push    esi edi
        push    ecx
        call    rt_buf_at
        mov     esi,eax
        mov     eax,[esp]
        call    rt_str_new
        pop     ecx
        mov     [eax+13+ecx],ecx        ; (bytes: the length where a str keeps its code point count)
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret

;;; code rt_mem_peek
rt_mem_peek:                    ; eax = address, edx = size (bytes) -> eax = little-endian value
        mov     ecx,edx
        xor     edx,edx
        jecxz   .out
        add     eax,ecx
@@:     dec     eax
        shl     edx,8
        mov     dl,[eax]
        dec     ecx
        jnz     @b
.out:   mov     eax,edx
        ret

;;; code rt_mem_poke
rt_mem_poke:                    ; eax = address, edx = value, ecx = size (bytes): store little-endian
        jecxz   .out
@@:     mov     [eax],dl
        inc     eax
        shr     edx,8
        dec     ecx
        jnz     @b
.out:   ret

;;; code rt_mem_peek_str : rt_str_new
rt_mem_peek_str:                ; eax = address, edx = length -> eax = new str of those bytes
        push    esi edi
        mov     esi,eax
        test    edx,edx
        jns     @f
        xor     edx,edx
@@:     push    edx
        mov     eax,edx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret

;;; code rt_sys_args linux : rt_list_new rt_list_push rt_mem_cstr rt_kd_str
rt_sys_args:                    ; eax = 0: argv, 1: the environment ("NAME=value") -> eax = new list[str]
        push    ebx esi
        mov     esi,[rt_sp0]    ; the stack at start: argc, argv..., 0, envp..., 0
        mov     ebx,[esi]
        add     esi,4
        test    eax,eax
        jz      @f
        lea     esi,[esi+ebx*4+4]
@@:     mov     eax,rt_kd_str
        call    rt_list_new
        mov     ebx,eax
.l:     mov     eax,[esi]
        test    eax,eax
        jz      .out
        mov     edx,65536
        call    rt_mem_cstr
        push    eax
        mov     eax,ebx
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        add     esi,4
        jmp     .l
.out:   mov     eax,ebx
        pop     esi ebx
        ret
;;; data rt_sys_args linux
rt_sp0          dd 0            ; esp at start (set there when rt_sys_args is used)

;;; code rt_sys_args kolibri : rt_list_new rt_list_push rt_mem_cstr rt_kd_str
rt_sys_args:                    ; eax = 0: [the program's path, its parameters], 1: [] (no environment)
        push    ebx
        push    eax
        mov     eax,rt_kd_str
        call    rt_list_new
        mov     ebx,eax
        pop     eax
        test    eax,eax
        jnz     .out
        mov     eax,rt_kpath    ; (the kernel filled both: the MENUET01 header points at them)
        call    .add
        mov     eax,rt_kparams
        call    .add
.out:   mov     eax,ebx
        pop     ebx
        ret
.add:   mov     edx,1024
        call    rt_mem_cstr
        push    eax
        mov     eax,ebx
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        ret
;;; bss rt_sys_args kolibri
rt_kparams      rb 1024
rt_kpath        rb 1024

;;; code rt_mem_cstr : rt_mem_peek_str
rt_mem_cstr:                    ; eax = address, edx = most bytes -> eax = new str of the bytes before the first 0
        push    edi
        mov     edi,eax
        mov     ecx,edx
        xor     edx,edx
        test    ecx,ecx
        jle     .take
        push    eax
        xor     eax,eax
        repne   scasb           ; edi: one past the 0, or past the last byte looked at
        jne     @f
        dec     edi
@@:     pop     eax
        mov     edx,edi
        sub     edx,eax
.take:  pop     edi
        jmp     rt_mem_peek_str

;;; code rt_mem_poke_str
rt_mem_poke_str:                ; eax = address, edx = str: its bytes there
        test    edx,edx
        jz      .out
        push    esi edi
        mov     edi,eax
        lea     esi,[edx+12]
        mov     ecx,[edx+8]
        rep     movsb
        pop     edi esi
.out:   ret

;;; code rt_unpack_check : rt_sb_need rt_sb_cstr rt_sb_int rt_sb_char rt_sb_take rt_raise_valerr
rt_unpack_check:                ; eax = list, edx = the targets (| 0x80000000: from a str - "too many" says no count) - ValueError unless as many
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
@@:     push    edx
        and     edx,0x7FFFFFFF
        cmp     ecx,edx
        jne     @f
        pop     edx
        ret
@@:     push    ecx
        push    edx
        push    dword [rt_sb_len]       ; [esp] mark, [esp+4] expected, [esp+8] got, [esp+12] flags
        mov     eax,rt_msg_unp_few
        cmp     ecx,edx
        jb      @f
        mov     eax,rt_msg_unp_many
@@:     call    rt_sb_cstr
        mov     eax,[esp+4]
        xor     edx,edx
        call    rt_sb_int
        mov     eax,[esp+8]
        cmp     eax,[esp+4]
        jb      .got
        test    dword [esp+12],0x80000000
        jnz     .close
.got:   mov     eax,rt_msg_unp_got
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
.close: mov     al,')'
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_unpack_check
rt_msg_unp_few  db 'not enough values to unpack (expected ',0
rt_msg_unp_many db 'too many values to unpack (expected ',0
rt_msg_unp_got  db ', got ',0

;;; code rt_unpack_atleast : rt_sb_need rt_sb_cstr rt_sb_int rt_sb_char rt_sb_take rt_raise_valerr rt_unpack_check
rt_unpack_atleast:              ; eax = list, edx = the targets besides a starred one: ValueError when fewer
        mov     ecx,[eax+8]
        cmp     ecx,edx
        jb      @f
        ret
@@:     push    ecx
        push    edx
        push    dword [rt_sb_len]
        mov     eax,rt_msg_unp_least
        call    rt_sb_cstr
        mov     eax,[esp+4]
        xor     edx,edx
        call    rt_sb_int
        mov     eax,rt_msg_unp_got
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
        mov     al,')'
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_unpack_atleast
rt_msg_unp_least db 'not enough values to unpack (expected at least ',0

;;; code rt_buf_poke : rt_buf_at
rt_buf_poke:                    ; [esp+4] buffer, offset, value, size (bytes): store little-endian
        mov     eax,[esp+4]
        mov     edx,[esp+8]
        mov     ecx,[esp+16]
        call    rt_buf_at
        mov     edx,[esp+12]
        mov     ecx,[esp+16]
        jecxz   .out
@@:     mov     [eax],dl
        inc     eax
        shr     edx,8
        dec     ecx
        jnz     @b
.out:   ret

;;; code rt_buf_peek : rt_buf_at
rt_buf_peek:                    ; [esp+4] buffer, offset, size (bytes) -> eax = little-endian value
        mov     eax,[esp+4]
        mov     edx,[esp+8]
        mov     ecx,[esp+12]
        call    rt_buf_at
        mov     ecx,[esp+12]
        xor     edx,edx
        jecxz   .out
        add     eax,ecx
@@:     dec     eax
        shl     edx,8
        mov     dl,[eax]
        dec     ecx
        jnz     @b
.out:   mov     eax,edx
        ret

;;; code rt_buf_poke_str : rt_buf_at
rt_buf_poke_str:                ; eax = buffer, edx = offset, ecx = str: copy its bytes there
        push    esi edi
        mov     esi,ecx
        xor     ecx,ecx
        test    esi,esi
        jz      @f
        mov     ecx,[esi+8]
@@:     push    ecx
        call    rt_buf_at
        pop     ecx
        mov     edi,eax
        lea     esi,[esi+12]
        rep     movsb
        pop     edi esi
        ret

;;; code rt_fround_n : rt_scale10
rt_fround_n:                    ; st0 = value, eax = digits -> st0 = round(value, digits), halves to even
        push    eax
        call    rt_scale10
        frndint
        pop     eax
        neg     eax
        jmp     rt_scale10

; ---------------------------------------------------------------- files
; File: +0 refcount, +4 destroy, +8 mode (0 read, 1 write, 2 append), +12 the
; contents (read: a str, loaded by open) or the bytes written so far (kolibri),
; +16 read position / bytes buffered, +20 buffer capacity, +24 fd (linux) or
; the path (kolibri, a str), +28 closed.

;;; code rt_file_mode
rt_file_mode:                   ; edx = mode str or 0 -> eax = 0 read, 1 write, 2 append
        xor     eax,eax
        test    edx,edx
        jz      .out
        cmp     dword [edx+8],0
        je      .out
        mov     cl,[edx+12]
        cmp     cl,'w'
        jne     @f
        mov     eax,1
@@:     cmp     cl,'a'
        jne     .out
        mov     eax,2
.out:   ret

;;; code rt_file_notfound : rt_sb_need rt_sb_bytes rt_sb_repr_str rt_sb_take rt_write_err rt_exit
rt_file_notfound:               ; eax = path: FileNotFoundError: [Errno 2] No such file or directory: 'path'
        push    eax
        push    dword [rt_sb_len]
        mov     eax,rt_msg_nofile
        mov     edx,rt_msg_nofile_len
        call    rt_sb_bytes
        mov     eax,[esp+4]
        call    rt_sb_repr_str
        pop     eax
        call    rt_sb_take
        add     esp,4
if defined rt_throw
        mov     ecx,eax
        mov     eax,VTX_FileNotFoundError
        mov     edx,DTX_FileNotFoundError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        push    eax
        mov     ecx,rt_msg_fnf
        mov     edx,rt_msg_fnf_len
        call    rt_write_err
        pop     eax
        lea     ecx,[eax+12]
        mov     edx,[eax+8]
        call    rt_write_err
        mov     ecx,rt_msg_nofile-1
        mov     edx,1
        call    rt_write_err
        mov     ebx,1
        jmp     rt_exit
end if
;;; data rt_file_notfound
rt_msg_fnf      db 'FileNotFoundError: '
rt_msg_fnf_len = $ - rt_msg_fnf
                db 10
rt_msg_nofile   db '[Errno 2] No such file or directory: '
rt_msg_nofile_len = $ - rt_msg_nofile

;;; code rt_file_open linux : rt_alloc rt_file_mode rt_file_notfound rt_file_free rt_sb_need rt_sb_take
rt_file_open:                   ; eax = path, edx = mode (str or 0) -> eax = new file
        push    ebx esi edi ebp
        mov     esi,eax
        call    rt_file_mode
        mov     ebp,eax
        test    esi,esi
        jz      .nf
        lea     ebx,[esi+12]
        xor     ecx,ecx         ; O_RDONLY
        cmp     ebp,1
        jne     @f
        mov     ecx,0x241       ; O_WRONLY | O_CREAT | O_TRUNC
@@:     cmp     ebp,2
        jne     @f
        mov     ecx,0x441       ; O_WRONLY | O_CREAT | O_APPEND
@@:     mov     edx,420         ; 0644
        mov     eax,5
        int     0x80
        test    eax,eax
        js      .nf
        mov     edi,eax
        mov     eax,32
        call    rt_alloc
        mov     dword [eax],1
        mov     dword [eax+4],rt_file_free
        mov     [eax+8],ebp
        mov     [eax+24],edi
        test    ebp,ebp
        jnz     .out
        push    eax             ; reading: all of it now
        push    dword [rt_sb_len]
.more:  mov     eax,4096
        call    rt_sb_need
        mov     ecx,eax
        mov     edx,4096
        mov     ebx,edi
        mov     eax,3
        int     0x80
        test    eax,eax
        jg      @f
        xor     eax,eax
@@:     sub     dword [rt_sb_len],4096
        add     [rt_sb_len],eax
        test    eax,eax
        jnz     .more
        pop     eax
        call    rt_sb_take
        pop     ecx
        mov     [ecx+12],eax
        push    ecx
        mov     ebx,edi
        mov     eax,6           ; close
        int     0x80
        pop     eax
        mov     dword [eax+24],-1
.out:   pop     ebp edi esi ebx
        ret
.nf:    mov     eax,esi
        jmp     rt_file_notfound

;;; code rt_file_write linux
rt_file_write:                  ; eax = file, edx = str -> eax = characters written
        push    ebx
        xor     ecx,ecx
        test    edx,edx
        jz      .none
        cmp     dword [eax+28],0
        jne     .none
        mov     ebx,[eax+24]
        test    ebx,ebx
        js      .none
        lea     ecx,[edx+12]
        mov     edx,[edx+8]
        push    edx
        mov     eax,4
        int     0x80
        pop     eax
        pop     ebx
        ret
.none:  xor     eax,eax
        pop     ebx
        ret

;;; code rt_file_close linux
rt_file_close:                  ; eax = file
        test    eax,eax
        jz      .out
        cmp     dword [eax+28],0
        jne     .out
        mov     dword [eax+28],1
        push    ebx
        mov     ebx,[eax+24]
        test    ebx,ebx
        js      @f
        mov     eax,6
        int     0x80
@@:     pop     ebx
.out:   ret

;;; code rt_file_open kolibri : rt_alloc rt_file_mode rt_file_notfound rt_file_free rt_str_new rt_incref rt_file_f70
rt_file_open:                   ; eax = path, edx = mode -> eax = new file
        push    ebx esi edi ebp
        mov     esi,eax
        call    rt_file_mode
        mov     ebp,eax
        test    esi,esi
        jz      .nf
        mov     eax,32
        call    rt_alloc
        mov     edi,eax
        mov     dword [edi],1
        mov     dword [edi+4],rt_file_free
        mov     [edi+8],ebp
        mov     [edi+24],esi
        mov     eax,esi
        call    rt_incref
        test    ebp,ebp
        jnz     .out
        sub     esp,40          ; reading: the size (70.5), then the whole file (70.0)
        mov     eax,esi
        mov     ecx,5
        xor     edx,edx
        mov     ebx,esp
        call    rt_file_f70
        test    eax,eax
        jnz     .nf2
        mov     eax,[esp+32]    ; size (low dword)
        add     esp,40
        push    eax
        call    rt_str_new
        mov     [edi+12],eax
        pop     edx             ; bytes
        lea     ebx,[eax+12]
        mov     eax,esi
        xor     ecx,ecx         ; 70.0 read
        call    rt_file_f70
.out:   mov     eax,edi
        pop     ebp edi esi ebx
        ret
.nf2:   add     esp,40
.nf:    mov     eax,esi
        jmp     rt_file_notfound

;;; code rt_file_f70 kolibri
rt_file_f70:                    ; eax = path str, ecx = subfunction, edx = size, ebx = buffer, [esp+4] = offset -> eax = status
        push    esi edi ebp
        mov     ebp,esp
        sub     esp,1048        ; information block + the path
        mov     edi,esp
        mov     [edi],ecx
        mov     ecx,[ebp+16]    ; offset (when the caller passed one; 0 otherwise)
        cmp     dword [edi],3
        je      @f
        xor     ecx,ecx
@@:     mov     [edi+4],ecx
        mov     dword [edi+8],0
        mov     [edi+12],edx
        mov     [edi+16],ebx
        lea     edi,[edi+20]
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        lea     esi,[eax+12]
        mov     ecx,[eax+8]
        cmp     ecx,1020
        jbe     @f
        mov     ecx,1020
@@:     rep     movsb
        mov     byte [edi],0
        mov     eax,70
        push    ebx
        mov     ebx,esp
        add     ebx,4
        int     0x40
        pop     ebx
        mov     esp,ebp
        pop     ebp edi esi
        ret

;;; code rt_file_write kolibri : rt_alloc rt_free
rt_file_write:                  ; eax = file, edx = str -> eax = characters written (buffered until close)
        push    ebx esi edi
        mov     ebx,eax
        xor     eax,eax
        test    edx,edx
        jz      .out
        cmp     dword [ebx+28],0
        jne     .out
        mov     esi,edx
        mov     ecx,[ebx+16]
        add     ecx,[esi+8]
        cmp     ecx,[ebx+20]
        jbe     .copy
        add     ecx,ecx         ; grow: twice what is needed
        add     ecx,256
        push    ecx             ; the new capacity
        mov     eax,ecx
        call    rt_alloc
        push    eax             ; the new buffer
        mov     edi,eax
        push    esi
        mov     esi,[ebx+12]
        mov     ecx,[ebx+16]
        rep     movsb
        pop     esi
        mov     eax,[ebx+12]
        call    rt_free
        pop     dword [ebx+12]
        pop     dword [ebx+20]
.copy:  mov     edi,[ebx+12]
        add     edi,[ebx+16]
        mov     ecx,[esi+8]
        mov     eax,ecx
        add     [ebx+16],ecx
        lea     esi,[esi+12]
        rep     movsb
.out:   pop     edi esi ebx
        ret

;;; code rt_file_close kolibri : rt_file_f70
rt_file_close:                  ; eax = file: writing files are written now (70.2, or 70.3 at the end for "a")
        test    eax,eax
        jz      .out
        cmp     dword [eax+28],0
        jne     .out
        mov     dword [eax+28],1
        cmp     dword [eax+8],0
        je      .out
        push    ebx esi
        mov     esi,eax
        cmp     dword [esi+8],2
        jne     .rewrite
        sub     esp,40          ; append: after what is there
        mov     eax,[esi+24]
        mov     ecx,5
        xor     edx,edx
        mov     ebx,esp
        call    rt_file_f70
        mov     ecx,[esp+32]
        add     esp,40
        test    eax,eax
        jnz     .rewrite
        push    ecx             ; offset
        mov     eax,[esi+24]
        mov     ecx,3
        mov     edx,[esi+16]
        mov     ebx,[esi+12]
        call    rt_file_f70
        add     esp,4
        jmp     .done
.rewrite:
        mov     eax,[esi+24]
        mov     ecx,2
        mov     edx,[esi+16]
        mov     ebx,[esi+12]
        call    rt_file_f70
.done:  pop     esi ebx
.out:   ret

;;; code rt_file_free : rt_file_close rt_decref rt_free
rt_file_free:                   ; destroy routine of files: close, drop what it holds
        push    ebx
        mov     ebx,eax
        call    rt_file_close
        cmp     dword [ebx+8],0
        jne     @f
        mov     eax,[ebx+12]    ; the contents
        call    rt_decref
        jmp     .path
@@:
if defined rt_file_f70
        mov     eax,[ebx+12]    ; the write buffer
        call    rt_free
end if
.path:
if defined rt_file_f70
        mov     eax,[ebx+24]
        call    rt_decref
end if
        mov     eax,ebx
        pop     ebx
        jmp     rt_free

;;; code rt_file_read : rt_str_slice
rt_file_read:                   ; eax = file -> eax = the rest of its contents (new str)
        push    ebx
        mov     ebx,eax
        mov     ecx,[ebx+12]
        xor     edx,edx
        test    ecx,ecx
        jz      @f
        mov     edx,[ecx+8]
@@:     push    0               ; flags: lo given, hi given, step 1
        push    1
        push    edx
        push    dword [ebx+16]
        push    ecx
        mov     [ebx+16],edx
        call    rt_str_slice
        add     esp,20
        pop     ebx
        ret

;;; code rt_file_readline : rt_str_slice
rt_file_readline:               ; eax = file -> eax = the next line, with its "\n" ("" at the end)
        push    ebx esi
        mov     ebx,eax
        mov     esi,[ebx+12]
        mov     ecx,[ebx+16]    ; from
        xor     edx,edx
        test    esi,esi
        jz      .end
        mov     edx,[esi+8]     ; length
.scan:  cmp     ecx,edx
        jae     .end
        cmp     byte [esi+12+ecx],10
        je      .nl
        inc     ecx
        jmp     .scan
.nl:    inc     ecx
.end:   push    0
        push    1
        push    ecx
        push    dword [ebx+16]
        push    esi
        mov     [ebx+16],ecx
        call    rt_str_slice
        add     esp,20
        pop     esi ebx
        ret

;;; code rt_file_readlines : rt_file_readline rt_list_new rt_list_push rt_kd_str rt_decref
rt_file_readlines:              ; eax = file -> eax = list of the remaining lines
        push    ebx esi
        mov     ebx,eax
        mov     eax,rt_kd_str
        call    rt_list_new
        mov     esi,eax
.next:  mov     eax,ebx
        call    rt_file_readline
        test    eax,eax
        jz      .out
        cmp     dword [eax+8],0
        jne     @f
        call    rt_decref
        jmp     .out
@@:     push    eax
        mov     eax,esi
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        jmp     .next
.out:   mov     eax,esi
        pop     esi ebx
        ret

; ---------------------------------------------------------------- asyncio
; Stackful tasks: every task runs on its own stack; `await coroutine()` is an
; ordinary call on the current task's stack, and a task gives the CPU back to
; the scheduler (rt_async_run, on the program's stack) when it sleeps or waits.
; Task object: +0 refcount, +4 destroy, +8 state (0 ready, 1 running,
; 2 sleeping, 3 waiting, 4 done), +12 saved esp, +16 stack memory, +20 next in
; the run queue / sleeper list, +24 result (8 bytes), +32 wake time (ms),
; +36 tasks waiting for this one, +40 next waiter, +44 result is a reference,
; +48 its handlers, +52 the generator it runs (an await inside a generator:
; generators are per task).

;;; code rt_task_new : rt_alloc rt_os_alloc rt_task_free rt_task_ready
rt_task_new:                    ; eax = argument bytes, edx = entry -> eax = new ready task; arguments at [[eax+12]+20]
        push    ebx esi
        mov     esi,eax
        push    edx
        mov     eax,56
        call    rt_alloc
        mov     ebx,eax
        mov     dword [ebx],2   ; the caller's reference and the scheduler's
        mov     dword [ebx+4],rt_task_free
        mov     eax,65536
        call    rt_os_alloc
        mov     [ebx+16],eax
        add     eax,65536-64
        sub     eax,esi
        and     eax,-16
        sub     eax,20          ; edi esi ebx ebp, then "return" into the entry
        xor     ecx,ecx
        mov     [eax],ecx
        mov     [eax+4],ecx
        mov     [eax+8],ecx
        mov     [eax+12],ecx
        pop     edx
        mov     [eax+16],edx
        mov     [ebx+12],eax
        mov     eax,ebx
        call    rt_task_ready
        mov     eax,ebx
        pop     esi ebx
        ret

;;; code rt_task_free : rt_os_free rt_free rt_decref
rt_task_free:                   ; destroy routine of tasks
        push    ebx
        mov     ebx,eax
        cmp     dword [ebx+44],0
        je      @f
        mov     eax,[ebx+24]
        call    rt_decref
@@:     mov     eax,[ebx+16]
        mov     edx,65536
        call    rt_os_free
        mov     eax,ebx
        pop     ebx
        jmp     rt_free

;;; code rt_task_ready
rt_task_ready:                  ; eax = task: append it to the run queue
        mov     dword [eax+8],0
        mov     dword [eax+20],0
        mov     ecx,[rt_ready_tail]
        test    ecx,ecx
        jz      @f
        mov     [ecx+20],eax
        mov     [rt_ready_tail],eax
        ret
@@:     mov     [rt_ready_head],eax
        mov     [rt_ready_tail],eax
        ret
;;; bss rt_task_ready
rt_ready_head   rd 1
rt_ready_tail   rd 1
rt_cur_task     rd 1            ; the running task, 0 in the scheduler
rt_sched_esp    rd 1
rt_sched_exc    rd 1            ; the scheduler's handler chain while a task runs
rt_sched_gen    rd 1            ; the scheduler's running generator while a task runs (it may wait in one)
rt_sleepers     rd 1

;;; code rt_thread_current : rt_task_ready
rt_thread_current:              ; -> edx:eax = the running task (0: the main program), thread.current()
        mov     eax,[rt_cur_task]
        xor     edx,edx
        ret

;;; code rt_task_yield : rt_task_ready
rt_task_yield:                  ; the running task hands the CPU back to the scheduler; returns when resumed
        push    ebp ebx esi edi
        mov     eax,[rt_cur_task]
        mov     [eax+12],esp
if defined rt_throw
        mov     ecx,[rt_exc_top] ; every task has its own handlers (+48)
        mov     [eax+48],ecx
        mov     ecx,[rt_sched_exc]
        mov     [rt_exc_top],ecx
end if
if defined rt_cur_gen
        mov     ecx,[rt_cur_gen]
        mov     [eax+52],ecx
        mov     ecx,[rt_sched_gen]
        mov     [rt_cur_gen],ecx
end if
        mov     esp,[rt_sched_esp]
        pop     edi esi ebx ebp
        ret

;;; code rt_task_resume : rt_task_ready
rt_task_resume:                 ; eax = task: run it until it yields or ends
        push    ebp ebx esi edi
        mov     [rt_sched_esp],esp
if defined rt_throw
        mov     ecx,[rt_exc_top]
        mov     [rt_sched_exc],ecx
        mov     ecx,[eax+48]
        mov     [rt_exc_top],ecx
end if
        mov     [rt_cur_task],eax
        mov     dword [eax+8],1
if defined rt_cur_gen
        mov     ecx,[rt_cur_gen]
        mov     [rt_sched_gen],ecx
        mov     ecx,[eax+52]
        mov     [rt_cur_gen],ecx
end if
        mov     esp,[eax+12]
        pop     edi esi ebx ebp
        ret

;;; code rt_task_exit : rt_task_ready
rt_task_exit:                   ; a task's function returned (the result is stored): wake its waiters, leave for good
        mov     ebx,[rt_cur_task]
        mov     dword [ebx+8],4
        mov     eax,[ebx+36]
        mov     dword [ebx+36],0
@@:     test    eax,eax
        jz      @f
        push    dword [eax+40]
        call    rt_task_ready
        pop     eax
        jmp     @b
@@:
if defined rt_throw
        mov     ecx,[rt_sched_exc]
        mov     [rt_exc_top],ecx
end if
if defined rt_cur_gen
        mov     ecx,[rt_sched_gen]
        mov     [rt_cur_gen],ecx
end if
        mov     esp,[rt_sched_esp]
        pop     edi esi ebx ebp
        ret

;;; code rt_task_wait : rt_task_yield
rt_task_wait:                   ; eax = task: wait until it is done (from a task) -> eax = task
        cmp     dword [eax+8],4
        je      .done
        push    eax
        mov     ecx,[rt_cur_task]
        mov     edx,[eax+36]
        mov     [ecx+40],edx
        mov     [eax+36],ecx
        mov     dword [ecx+8],3
        call    rt_task_yield
        pop     eax
.done:  ret

;;; code rt_task_result : rt_panic
rt_task_result:                 ; eax = task -> eax = task, which must be done
        cmp     dword [eax+8],4
        jne     @f
        ret
@@:     mov     esi,rt_msg_notdone
        jmp     rt_panic
;;; data rt_task_result
rt_msg_notdone  db 'InvalidStateError: result is not set',0

;;; code rt_async_sleep : rt_task_yield rt_task_ready rt_now_ms rt_idle_sleep
rt_async_sleep:                 ; st0 = seconds (popped): suspend the running task that long
        push    1000
        fimul   dword [esp]
        fistp   dword [esp]
        pop     eax
        cmp     dword [rt_cur_task],0
        jne     .task
        test    eax,eax         ; no event loop (a coroutine called directly): just sleep
        jle     .none
        jmp     rt_idle_sleep
.none:  ret
.task:  test    eax,eax
        jg      @f
        mov     eax,[rt_cur_task] ; sleep(0): to the back of the run queue
        call    rt_task_ready
        jmp     rt_task_yield
@@:     push    eax
        call    rt_now_ms
        pop     edx
        add     eax,edx
        mov     ecx,[rt_cur_task]
        mov     [ecx+32],eax
        mov     dword [ecx+8],2
        mov     eax,[rt_sleepers]
        mov     [ecx+20],eax
        mov     [rt_sleepers],ecx
        jmp     rt_task_yield

;;; code rt_sched_step : rt_task_resume rt_task_ready rt_now_ms rt_idle_sleep rt_decref
; thread (minipy's module) in compiled programs: threads are tasks; the program's own code
; runs the others when it waits (join, acquire, sleep) and at its end.
rt_sched_step:                  ; eax = ms it may idle at most (-1: any): run one ready task -> eax = 0 when no task is left
        push    ebx esi edi ebp
        mov     ebp,eax
        call    rt_now_ms
        mov     esi,eax
        mov     edi,rt_sleepers
.sl:    mov     ebx,[edi]
        test    ebx,ebx
        jz      .pick
        mov     eax,[ebx+32]
        sub     eax,esi
        jg      .keep
        mov     eax,[ebx+20]
        mov     [edi],eax
        mov     eax,ebx
        call    rt_task_ready
        jmp     .sl
.keep:  lea     edi,[ebx+20]
        jmp     .sl
.pick:  mov     eax,[rt_ready_head]
        test    eax,eax
        jnz     .run
        mov     ebx,[rt_sleepers]
        test    ebx,ebx
        jz      .none
        mov     ecx,0x7FFFFFFF
.min:   mov     eax,[ebx+32]
        sub     eax,esi
        cmp     eax,ecx
        jge     @f
        mov     ecx,eax
@@:     mov     ebx,[ebx+20]
        test    ebx,ebx
        jnz     .min
        test    ebp,ebp
        js      @f
        cmp     ecx,ebp
        jle     @f
        mov     ecx,ebp
@@:     mov     eax,ecx
        call    rt_idle_sleep
        mov     eax,1
        jmp     .out
.run:   mov     ecx,[eax+20]
        mov     [rt_ready_head],ecx
        test    ecx,ecx
        jnz     @f
        mov     [rt_ready_tail],ecx
@@:     push    eax
        call    rt_task_resume
        pop     eax
        mov     dword [rt_cur_task],0
        cmp     dword [eax+8],4
        jne     @f
        call    rt_decref       ; the scheduler's reference to a finished task
@@:     mov     eax,1
        jmp     .out
.none:  xor     eax,eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_thread_join : rt_task_wait rt_async_run
rt_thread_join:                 ; eax = a thread (task): until it is done
        cmp     dword [rt_cur_task],0
        jne     rt_task_wait
        jmp     rt_async_run

;;; code rt_thread_drain : rt_sched_step
rt_thread_drain:                ; the program's end: the threads still running finish
@@:     or      eax,-1
        call    rt_sched_step
        test    eax,eax
        jnz     @b
        ret

;;; code rt_thread_sleep : rt_async_sleep rt_sched_step rt_now_ms rt_idle_sleep
rt_thread_sleep:                ; st0 = seconds (popped): the other threads run meanwhile
        cmp     dword [rt_cur_task],0
        jne     rt_async_sleep
        push    1000
        fimul   dword [esp]
        fistp   dword [esp]
        call    rt_now_ms
        add     [esp],eax       ; the time to wake up
.l:     call    rt_now_ms
        mov     ecx,[esp]
        sub     ecx,eax
        jle     .done
        mov     eax,ecx
        call    rt_sched_step
        test    eax,eax
        jnz     .l
        call    rt_now_ms       ; nothing else to run: sleep the rest
        mov     ecx,[esp]
        sub     ecx,eax
        jle     .done
        mov     eax,ecx
        call    rt_idle_sleep
.done:  add     esp,4
        ret

;;; code rt_thread_lock : rt_panic
rt_thread_lock:                 ; -> eax = a new lock's number (edx 0)
        xor     eax,eax
@@:     cmp     dword [rt_locks+eax*4],0
        je      @f
        inc     eax
        cmp     eax,256
        jb      @b
        mov     esi,rt_msg_locks
        jmp     rt_panic
@@:     mov     dword [rt_locks+eax*4],1
        xor     edx,edx
        ret
;;; data rt_thread_lock
rt_msg_locks    db 'RuntimeError: too many locks',0
;;; bss rt_thread_lock
rt_locks        rd 256          ; 0 unused, 1 free, 2 held

;;; code rt_thread_acquire : rt_thread_lock rt_task_ready rt_task_yield rt_sched_step rt_panic
rt_thread_acquire:              ; eax = a lock's number: wait until it is free, take it
        and     eax,255
        lea     ecx,[rt_locks+eax*4]
.try:   cmp     dword [ecx],2
        jne     .take
        push    ecx
        cmp     dword [rt_cur_task],0
        je      .main
        mov     eax,[rt_cur_task] ; a thread: the others first
        call    rt_task_ready
        call    rt_task_yield
        pop     ecx
        jmp     .try
.main:  or      eax,-1          ; the program: run the others until it is free
        call    rt_sched_step
        pop     ecx
        test    eax,eax
        jnz     .try
        mov     esi,rt_msg_lockdead
        jmp     rt_panic
.take:  mov     dword [ecx],2
        ret
;;; data rt_thread_acquire
rt_msg_lockdead db 'RuntimeError: the lock is held by no running thread (deadlock)',0

;;; code rt_thread_release : rt_thread_lock
rt_thread_release:              ; eax = a lock's number
        and     eax,255
        mov     dword [rt_locks+eax*4],1
        ret

;;; code rt_async_run : rt_task_resume rt_task_ready rt_now_ms rt_idle_sleep rt_decref rt_panic
rt_async_run:                   ; eax = task: run the event loop until it is done -> eax = task
        push    ebx esi edi ebp
        mov     ebp,eax
.loop:  cmp     dword [ebp+8],4
        je      .out
        call    rt_now_ms       ; sleepers whose time has come -> run queue
        mov     esi,eax
        mov     edi,rt_sleepers
.sl:    mov     ebx,[edi]
        test    ebx,ebx
        jz      .pick
        mov     eax,[ebx+32]
        sub     eax,esi
        jg      .keep
        mov     eax,[ebx+20]
        mov     [edi],eax
        mov     eax,ebx
        call    rt_task_ready
        jmp     .sl
.keep:  lea     edi,[ebx+20]
        jmp     .sl
.pick:  mov     eax,[rt_ready_head]
        test    eax,eax
        jnz     .run
        mov     ebx,[rt_sleepers] ; nothing to run: sleep until the first sleeper is due
        test    ebx,ebx
        jz      .stuck
        mov     ecx,0x7FFFFFFF
.min:   mov     eax,[ebx+32]
        sub     eax,esi
        cmp     eax,ecx
        jge     @f
        mov     ecx,eax
@@:     mov     ebx,[ebx+20]
        test    ebx,ebx
        jnz     .min
        mov     eax,ecx
        call    rt_idle_sleep
        jmp     .loop
.run:   mov     ecx,[eax+20]
        mov     [rt_ready_head],ecx
        test    ecx,ecx
        jnz     @f
        mov     [rt_ready_tail],ecx
@@:     push    eax
        call    rt_task_resume
        pop     eax
        mov     dword [rt_cur_task],0
        cmp     dword [eax+8],4
        jne     .loop
        call    rt_decref       ; the scheduler's reference to a finished task
        jmp     .loop
.stuck: mov     esi,rt_msg_stuck
        jmp     rt_panic
.out:   mov     eax,ebp
        pop     ebp edi esi ebx
        ret
;;; data rt_async_run
rt_msg_stuck    db 'RuntimeError: every task is waiting (deadlock)',0

; ---------------------------------------------------------------- generators
; A generator runs its function on a stack of its own: next() switches to it
; until it yields (the value is left in the generator object) or returns.
; Generator: +0 refcount, +4 destroy, +8 state (0 created, 1 running,
; 2 suspended, 3 done), +12 its saved esp, +16 stack memory, +20 the
; consumer's esp, +24 the current value (8 bytes), +32 the value is a
; reference, +36 the arguments, +40 descriptor (function, flags: 1 reference
; values, 2 8-byte values; slot count, frame offsets of the references it
; owns), +44 closure, +48 its handlers, +52 the consumer's handlers, +56 the
; generator that ran before, +60 its outermost handler record (28 bytes),
; +88 a value sent (8 bytes), +96 a value was sent, +100 its return value
; (8 bytes), +108 it returned one, +112 an exception to raise where it waits
; (throw(), close()). Descriptor flags also: 4 sent values are references,
; 8 the return value is one. State 4: closed before it started.

;;; code rt_gen_new : rt_alloc rt_os_alloc rt_gen_free rt_gen_yield
rt_gen_new:                     ; eax = argument bytes, edx = descriptor -> eax = new generator; arguments at [eax+36]
        push    ebx esi
        mov     esi,eax
        push    edx
        mov     eax,116
        call    rt_alloc
        mov     ebx,eax
        mov     dword [ebx],1
        mov     dword [ebx+4],rt_gen_free
        pop     edx
        mov     [ebx+40],edx
        mov     eax,[edx+4]
        and     eax,1
        mov     [ebx+32],eax
        mov     eax,65536
        call    rt_os_alloc
        mov     [ebx+16],eax
        add     eax,65536-64
        sub     eax,esi
        and     eax,-16
        sub     eax,20          ; edi esi ebx ebp, then "return" into rt_gen_start
        xor     ecx,ecx
        mov     [eax],ecx
        mov     [eax+4],ecx
        mov     [eax+8],ecx
        mov     [eax+12],ecx
        mov     dword [eax+16],rt_gen_start
        mov     [ebx+12],eax
        add     eax,20
        mov     [ebx+36],eax
        mov     eax,ebx
        pop     esi ebx
        ret

;;; code rt_gen_next : rt_gen_yield rt_panic_value
rt_gen_next:                    ; eax = generator: run it to its next value -> eax = 1 (the value is at +24) or 0 (finished)
        mov     ecx,[eax+8]
        cmp     ecx,3
        jae     .done
        cmp     ecx,1
        je      .busy
        push    ebp ebx esi edi
        mov     [eax+20],esp
if defined rt_throw
        mov     ecx,[rt_exc_top]
        mov     [eax+52],ecx
        mov     ecx,[eax+48]
        mov     [rt_exc_top],ecx
end if
        mov     ecx,[rt_cur_gen]
        mov     [eax+56],ecx
        mov     [rt_cur_gen],eax
        mov     dword [eax+8],1
        mov     esp,[eax+12]
        pop     edi esi ebx ebp
        ret
.done:  xor     eax,eax
        ret
.busy:  mov     esi,rt_msg_genbusy
        jmp     rt_panic_value
;;; data rt_gen_next
rt_msg_genbusy  db 'generator already executing',0

;;; code rt_gen_yield
rt_gen_yield:                   ; from the generator's function, its value stored: back to the consumer until the next next()
        call    rt_gen_suspend  ; (resumed: an exception thrown in is raised here)
if defined rt_throw
        mov     ecx,[rt_cur_gen]
        mov     eax,[ecx+112]
        test    eax,eax
        jnz     .throw
end if
        ret
if defined rt_throw
.throw: mov     dword [ecx+112],0
        jmp     rt_throw
end if
rt_gen_suspend:
        push    ebp ebx esi edi
        mov     eax,[rt_cur_gen]
        mov     [eax+12],esp
        mov     dword [eax+8],2
        mov     ecx,1
rt_gen_switch:                  ; eax = generator, ecx = what next() returns
if defined rt_throw
        mov     edx,[rt_exc_top]
        mov     [eax+48],edx
        mov     edx,[eax+52]
        mov     [rt_exc_top],edx
end if
        mov     edx,[eax+56]
        mov     [rt_cur_gen],edx
        mov     esp,[eax+20]
        pop     edi esi ebx ebp
        mov     eax,ecx
        ret
rt_gen_start:                   ; the first next(): run the generator's function
        mov     ecx,[rt_cur_gen]
if defined rt_throw
        lea     eax,[ecx+60]    ; its outermost handler: an exception leaves the generator
        mov     dword [eax],0
        mov     [eax+4],esp
        mov     [eax+8],ebp
        mov     dword [eax+12],rt_gen_caught
        mov     [eax+16],ebx
        mov     [eax+20],esi
        mov     [eax+24],edi
        mov     [rt_exc_top],eax
end if
        mov     edx,[ecx+44]    ; its closure
        mov     eax,[ecx+40]
        call    dword [eax]
        mov     eax,[rt_cur_gen]
        mov     dword [eax+8],3
        xor     ecx,ecx
        jmp     rt_gen_switch
if defined rt_throw
rt_gen_caught:                  ; an exception escaped the generator: it is finished, the consumer gets the exception
        mov     eax,[rt_cur_gen]
        mov     dword [eax+8],3
        mov     ecx,[eax+52]
        mov     [rt_exc_top],ecx
        mov     ecx,[eax+56]
        mov     [rt_cur_gen],ecx
        mov     esp,[eax+20]
        pop     edi esi ebx ebp
        mov     eax,[rt_exc_cur]
        jmp     rt_throw
end if
;;; bss rt_gen_yield
rt_cur_gen      rd 1            ; the running generator

;;; code rt_gen_free : rt_decref rt_os_free rt_free
rt_gen_free:                    ; destroy routine: also releases what a suspended (or never started) generator's frame holds
        push    ebx esi edi ebp
        mov     ebx,eax
if defined rt_gen_close
        cmp     dword [ebx+8],2
        jne     @f
        mov     dword [ebx],1   ; (alive while it closes)
        xor     edx,edx         ; a suspended one is closed first: its finally blocks run
        call    rt_gen_close
        mov     dword [ebx],0
@@:
end if
        mov     eax,[ebx+8]
        cmp     eax,4
        jne     @f
        xor     eax,eax         ; closed before it started: the arguments are still there
@@:
        xor     edi,edi
        cmp     eax,2
        jne     .ns
        mov     ebp,[ebx+12]
        mov     ebp,[ebp+12]    ; the function's frame (ebp saved by rt_gen_yield)
        jmp     .slots
.ns:    test    eax,eax
        jnz     .rest
        mov     ebp,[ebx+36]    ; never started: just the arguments
        sub     ebp,8
        inc     edi
.slots: mov     esi,[ebx+40]
        mov     ecx,[esi+8]
        add     esi,12
.l:     test    ecx,ecx
        jz      .rest
        mov     eax,[esi]
        test    edi,edi
        jz      .take
        test    eax,eax
        js      .skip
.take:  push    ecx
        mov     eax,[ebp+eax]
        call    rt_decref
        pop     ecx
.skip:  add     esi,4
        dec     ecx
        jmp     .l
.rest:  cmp     dword [ebx+32],0
        je      .nv
        mov     eax,[ebx+24]
        call    rt_decref
.nv:    mov     esi,[ebx+40]
        test    dword [esi+4],4
        jz      .ns2
        cmp     dword [ebx+96],0
        je      .ns2
        mov     eax,[ebx+88]
        call    rt_decref
.ns2:   test    dword [esi+4],8
        jz      .nr
        cmp     dword [ebx+108],0
        je      .nr
        mov     eax,[ebx+100]
        call    rt_decref
.nr:    mov     eax,[ebx+112]
        call    rt_decref
        mov     eax,[ebx+44]
        call    rt_decref
        mov     eax,[ebx+16]
        mov     edx,65536
        call    rt_os_free
        mov     eax,ebx
        pop     ebp edi esi ebx
        jmp     rt_free

;;; code rt_gen_close : rt_gen_next rt_obj_new rt_decref rt_throw
rt_gen_close:                   ; eax = generator, edx = 1: other exceptions propagate (0: dropped) -> eax = 1 if it yielded again
        mov     ecx,[eax+8]
        test    ecx,ecx
        jnz     @f
        mov     dword [eax+8],4 ; never started: finished
        xor     eax,eax
        ret
@@:     cmp     ecx,2
        je      .susp
        xor     eax,eax
        ret
.susp:  push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        mov     eax,RT_EXC_SIZE ; GeneratorExit, raised where it waits
        mov     edx,VTX_GeneratorExit
        mov     ecx,DTX_GeneratorExit
        call    rt_obj_new
        mov     [ebx+112],eax
        sub     esp,28          ; a handler for what leaves the generator
        mov     eax,[rt_exc_top]
        mov     [esp],eax
        mov     [esp+4],esp
        mov     [esp+8],ebp
        mov     dword [esp+12],.caught
        mov     [esp+16],ebx
        mov     [esp+20],esi
        mov     [esp+24],edi
        mov     [rt_exc_top],esp
        mov     eax,ebx
        call    rt_gen_next
        mov     ecx,[esp]
        mov     [rt_exc_top],ecx
        add     esp,28
        pop     ebp edi esi ebx
        ret
.caught: add    esp,28
        mov     eax,[rt_exc_cur]
        mov     ecx,[eax+8]
        cmp     ecx,VTX_GeneratorExit
        je      .gone
        cmp     ecx,VTX_StopIteration
        je      .gone
        test    esi,esi
        jz      .gone
        pop     ebp edi esi ebx
        jmp     rt_throw
.gone:  call    rt_decref
        xor     eax,eax
        pop     ebp edi esi ebx
        ret

;;; code rt_gen_throw : rt_gen_next rt_throw
rt_gen_throw:                   ; eax = generator, edx = exception (owned): raised where it waits -> as rt_gen_next
        cmp     dword [eax+8],2
        jne     .now
        mov     [eax+112],edx
        jmp     rt_gen_next
.now:   cmp     dword [eax+8],0
        jne     @f
        mov     dword [eax+8],4 ; not started: finished, the exception is raised here
@@:     mov     eax,edx
        jmp     rt_throw

;;; code rt_gen_drain : rt_gen_next rt_list_new rt_list_push rt_elem_copy
rt_gen_drain:                   ; eax = generator, edx = the type of its values -> eax = new list of the values left
        push    ebx esi
        mov     ebx,eax
        mov     eax,edx
        call    rt_list_new
        mov     esi,eax
.l:     test    ebx,ebx
        jz      .out
        mov     eax,ebx
        call    rt_gen_next
        test    eax,eax
        jz      .out
        mov     eax,esi
        call    rt_list_push
        lea     edx,[ebx+24]
        mov     ecx,[esi+20]
        call    rt_elem_copy
        jmp     .l
.out:   mov     eax,esi
        pop     esi ebx
        ret

;;; code rt_panic_stop : rt_panic
rt_panic_stop:                  ; next() of a finished generator
if defined rt_throw
        mov     eax,VTX_StopIteration
        mov     edx,DTX_StopIteration
        xor     ecx,ecx
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     esi,rt_msg_stop
        jmp     rt_panic
end if
;;; data rt_panic_stop
rt_msg_stop     db 'StopIteration',0

;;; code rt_panic_type : rt_panic
rt_panic_type:                  ; esi = detail
if defined rt_throw
        mov     eax,VTX_TypeError
        mov     edx,DTX_TypeError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_msg_type
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_panic_type
rt_msg_type     db 'TypeError: ',0

; ---------------------------------------------------------------- closures
; A function value: +0 refcount, +4 destroy, +8 code, +12 captured values
; (copies, or cells shared with the defining function). A cell: +0 refcount,
; +4 destroy, +8 the value (8 bytes).

;;; code rt_cell_new : rt_alloc rt_cell_free
rt_cell_new:                    ; eax = 1 if the value is a reference -> eax = new cell (value 0)
        push    eax
        mov     eax,16
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_free
        test    ecx,ecx
        jz      @f
        mov     dword [eax+4],rt_cell_free
@@:     ret

;;; code rt_cell_free : rt_decref rt_free
rt_cell_free:                   ; destroy routine of a cell holding a reference
        push    eax
        mov     eax,[eax+8]
        call    rt_decref
        pop     eax
        jmp     rt_free

;;; code rt_now_ms linux
rt_now_ms:                      ; -> eax = milliseconds (wraps; compare differences)
        push    ebx
        sub     esp,8
        mov     eax,78          ; gettimeofday
        mov     ebx,esp
        xor     ecx,ecx
        int     0x80
        mov     eax,[esp+4]
        xor     edx,edx
        mov     ecx,1000
        div     ecx
        imul    ecx,[esp],1000
        add     eax,ecx
        add     esp,8
        pop     ebx
        ret

;;; code rt_now_ms kolibri
rt_now_ms:
        push    ebx
        mov     eax,26          ; fn 26.9: time since boot in 1/100 s
        mov     ebx,9
        int     0x40
        imul    eax,10
        pop     ebx
        ret

;;; code rt_idle_sleep linux
rt_idle_sleep:                  ; eax = milliseconds
        push    ebx
        xor     edx,edx
        mov     ecx,1000
        div     ecx
        imul    edx,1000000
        push    edx             ; nanoseconds
        push    eax             ; seconds
        mov     eax,162         ; nanosleep
        mov     ebx,esp
        xor     ecx,ecx
        int     0x80
        add     esp,8
        pop     ebx
        ret

;;; code rt_idle_sleep kolibri
rt_idle_sleep:
        push    ebx
        add     eax,9
        xor     edx,edx
        mov     ecx,10
        div     ecx
        mov     ebx,eax
        mov     eax,5           ; fn 5: sleep in 1/100 s
        int     0x40
        pop     ebx
        ret

;;; code rt_time_sleep : rt_idle_sleep
rt_time_sleep:                  ; st0 = seconds (popped): block (once threads were started: let them run meanwhile)
        mov     eax,[rt_sleep_hook]
        test    eax,eax
        jz      .block
        call    eax
        ret
.block: push    1000
        fimul   dword [esp]
        fistp   dword [esp]
        pop     eax
        test    eax,eax
        jle     @f
        jmp     rt_idle_sleep
@@:     ret
;;; data rt_time_sleep
rt_sleep_hook   dd 0            ; rt_thread_sleep once thread.start() ran

;;; code rt_time_now linux
rt_time_now:                    ; -> st0 = seconds since the epoch
        push    ebx
        sub     esp,8
        mov     eax,78          ; gettimeofday
        mov     ebx,esp
        xor     ecx,ecx
        int     0x80
        fild    dword [esp+4]
        mov     dword [esp+4],1000000
        fidiv   dword [esp+4]
        fiadd   dword [esp]
        add     esp,8
        pop     ebx
        ret

;;; code rt_time_now kolibri
rt_time_now:                    ; -> st0 = seconds since boot
        push    ebx
        mov     eax,26
        mov     ebx,9
        int     0x40
        push    eax
        fild    dword [esp]
        mov     dword [esp],100
        fidiv   dword [esp]
        pop     eax
        pop     ebx
        ret

; ---------------------------------------------------------------- math, random

;;; code rt_fexp : rt_ext_call
rt_fexp:                        ; st0 = x -> st0 = e^x
        push    rt_fexp_x          ; in extended precision
        jmp     rt_ext_call
rt_fexp_x:
        fldl2e
        fmulp   st1,st0
        fld     st0
        frndint
        fsub    st1,st0
        fxch
        f2xm1
        fld1
        faddp   st1,st0
        fscale
        fstp    st1
        ret

; one-argument math functions, in extended precision (st0 = x -> st0 = f(x));
; macOS programs (C) use the C library's instead (aot_x2c.c natives)
;;; code rt_flog : rt_ext_call
rt_flog:        push    rt_flog_x
                jmp     rt_ext_call
rt_flog_x:      fldln2
                fxch
                fyl2x
                ret
;;; code rt_flog10 : rt_ext_call
rt_flog10:      push    rt_flog10_x
                jmp     rt_ext_call
rt_flog10_x:    fldlg2
                fxch
                fyl2x
                ret
;;; code rt_flog2 : rt_ext_call
rt_flog2:       push    rt_flog2_x
                jmp     rt_ext_call
rt_flog2_x:     fld1
                fxch
                fyl2x
                ret
;;; code rt_fatan : rt_ext_call
rt_fatan:       push    rt_fatan_x
                jmp     rt_ext_call
rt_fatan_x:     fld1
                fpatan
                ret
;;; code rt_fasin : rt_ext_call
rt_fasin:       push    rt_fasin_x
                jmp     rt_ext_call
rt_fasin_x:     fld     st0
                fmul    st0,st0
                fld1
                fsubrp  st1,st0
                fsqrt
                fpatan
                ret
;;; code rt_facos : rt_ext_call
rt_facos:       push    rt_facos_x
                jmp     rt_ext_call
rt_facos_x:     fld     st0
                fmul    st0,st0
                fld1
                fsubrp  st1,st0
                fsqrt
                fxch
                fpatan
                ret
;;; code rt_fsin : rt_ext_call
rt_fsin:        push    rt_fsin_x
                jmp     rt_ext_call
rt_fsin_x:      fsin
                ret
;;; code rt_fcos : rt_ext_call
rt_fcos:        push    rt_fcos_x
                jmp     rt_ext_call
rt_fcos_x:      fcos
                ret
;;; code rt_ftan : rt_ext_call
rt_ftan:        push    rt_ftan_x
                jmp     rt_ext_call
rt_ftan_x:      fptan
                fstp    st0
                ret
;;; code rt_fatan2 : rt_ext_call
rt_fatan2:      push    rt_fatan2_x     ; st0 = x, st1 = y -> st0 = atan2(y, x)
                jmp     rt_ext_call
rt_fatan2_x:    fpatan
                ret

;;; code rt_fmod_c : rt_panic_zero
rt_fmod_c:                      ; st0 = x, st1 = y (both popped) -> st0 = fmod(x, y), the sign of x
        fxch
        ftst
        fnstsw  ax
        sahf
        je      .zero
        fxch
.again: fprem
        fnstsw  ax
        sahf
        jp      .again
        fstp    st1
        ret
.zero:  fstp    st0
        fstp    st0
        jmp     rt_panic_zero

;;; code rt_frexp
rt_frexp:                       ; st0 = x -> st0 = m (0.5 <= |m| < 1, x's sign), eax = e: x = m * 2**e
        sub     esp,8           ; (0, an infinity, a NaN: x itself and 0)
        fst     qword [esp]
        mov     eax,[esp+4]
        mov     ecx,eax
        shr     ecx,20
        and     ecx,0x7FF
        jz      .small
        cmp     ecx,0x7FF
        je      .same
.exp:   and     eax,0x800FFFFF  ; the exponent of 0.5
        or      eax,0x3FE00000
        mov     [esp+4],eax
        fstp    st0
        fld     qword [esp]
        lea     eax,[ecx-1022]
        add     esp,8
        ret
.small: mov     edx,eax
        and     edx,0x7FFFFFFF
        or      edx,[esp]
        jz      .same
        fmul    qword [rt_two54]  ; a subnormal: scaled up into the normal ones
        fst     qword [esp]
        mov     eax,[esp+4]
        mov     ecx,eax
        shr     ecx,20
        and     ecx,0x7FF
        sub     ecx,54
        jmp     .exp
.same:  xor     eax,eax
        add     esp,8
        ret
;;; data rt_frexp
align 8
rt_two54        dd 0,0x43500000   ; 2**54

;;; code rt_ldexp : rt_ovferr_text
rt_ldexp:                       ; st0 = x, edx:eax = i -> st0 = x * 2**i (OverflowError when too big)
        mov     ecx,eax
        sar     ecx,31
        cmp     ecx,edx
        jne     .far
        cmp     eax,65536
        jg      .up
        cmp     eax,-65536
        jl      .down
        jmp     .go
.far:   test    edx,edx
        js      .down
.up:    mov     eax,65536
        jmp     .go
.down:  mov     eax,-65536
.go:    sub     esp,8
        fst     qword [esp]     ; 0, an infinity, a NaN: x itself
        mov     ecx,[esp+4]
        and     ecx,0x7FF00000
        cmp     ecx,0x7FF00000
        je      .same
        mov     ecx,[esp+4]
        and     ecx,0x7FFFFFFF
        or      ecx,[esp]
        jz      .same
        mov     [esp],eax
        fild    dword [esp]
        fxch
        fscale
        fstp    st1
        fstp    qword [esp]     ; (rounded to a float once)
        fld     qword [esp]
        mov     ecx,[esp+4]
        and     ecx,0x7FF00000
        cmp     ecx,0x7FF00000
        je      .big
.same:  add     esp,8
        ret
.big:   fstp    st0
        add     esp,8
        mov     esi,rt_msg_mrange
        jmp     rt_ovferr_text
;;; data rt_ldexp
rt_msg_mrange   db 'math range error',0

;;; code rt_rand : rt_now_ms
rt_rand:                        ; -> eax = 32 random bits (xorshift32; seeded from the clock unless seeded)
        mov     eax,[rt_rand_state]
        test    eax,eax
        jnz     @f
        call    rt_now_ms
        xor     eax,0x9E3779B9
        or      eax,1
@@:     mov     edx,eax
        shl     edx,13
        xor     eax,edx
        mov     edx,eax
        shr     edx,17
        xor     eax,edx
        mov     edx,eax
        shl     edx,5
        xor     eax,edx
        mov     [rt_rand_state],eax
        ret
;;; bss rt_rand
rt_rand_state   rd 1

;;; code rt_rand_seed : rt_rand
rt_rand_seed:                   ; eax = seed
        imul    eax,eax,0x9E3779B1
        xor     eax,0x5DEECE66
        or      eax,1
        mov     [rt_rand_state],eax
        ret

;;; code rt_rand_float : rt_rand
rt_rand_float:                  ; -> st0 in [0, 1)
        call    rt_rand
        shr     eax,1
        push    eax
        fild    dword [esp]
        mov     dword [esp],0x4F000000   ; 2^31 as a float
        fdiv    dword [esp]
        add     esp,4
        ret

;;; code rt_rand_below : rt_rand
rt_rand_below:                  ; eax = n (> 0, 32 bits) -> eax = a random number in [0, n)
        push    eax
        call    rt_rand
        xor     edx,edx
        div     dword [esp]
        add     esp,4
        mov     eax,edx
        ret

;;; code rt_rand_range : rt_rand rt_rand_below rt_udivmod rt_sb_need rt_sb_cstr rt_sb_int rt_sb_char rt_sb_take rt_raise_valerr rt_panic_index
; randrange / randint / choice: the interpreter's numbers (xorshift32; a range of
; 2**32 or more takes two draws)
rt_rand_range:                  ; [esp+4] lo, edx:eax hi (exclusive; randint: inclusive), ecx = 0 randrange(n), 1 randrange(a, b), 2 randint, 3 choice -> edx:eax
        push    ebx esi edi ebp
        mov     ebp,ecx
        push    edx             ; [esp] hi as given
        push    eax
        cmp     ebp,2
        jne     @f
        add     eax,1           ; randint: up to hi itself
        adc     edx,0
@@:     sub     eax,[esp+28]    ; the span
        sbb     edx,[esp+32]
        jo      .huge
        test    edx,edx
        js      .empty
        jnz     .wide
        test    eax,eax
        jz      .empty
        call    rt_rand_below
        xor     edx,edx
        jmp     .add
.huge:  jmp     .empty
.wide:  mov     ebx,eax         ; span: ecx:ebx
        mov     ecx,edx
        push    ecx
        push    ebx
        call    rt_rand
        mov     esi,eax
        call    rt_rand
        mov     edx,esi
        pop     ebx
        pop     ecx
        call    rt_udivmod      ; the remainder (ecx:ebx)
        mov     eax,ebx
        mov     edx,ecx
.add:   add     eax,[esp+28]
        adc     edx,[esp+32]
        add     esp,8
        pop     ebp edi esi ebx
        ret
.empty: cmp     ebp,3
        je      .choice
        push    dword [rt_sb_len]
        mov     eax,rt_msg_rr0
        test    ebp,ebp
        jz      .msg
        mov     eax,rt_msg_rr1
        cmp     ebp,1
        je      @f
        mov     eax,rt_msg_rr2
@@:     call    rt_sb_cstr
        mov     eax,[esp+32]
        mov     edx,[esp+36]
        call    rt_sb_int
        mov     eax,rt_msg_rrsep
        call    rt_sb_cstr
        mov     eax,[esp+4]
        mov     edx,[esp+8]
        call    rt_sb_int
        mov     al,')'
        call    rt_sb_char
        jmp     .take
.msg:   call    rt_sb_cstr
.take:  pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
.choice:
        mov     esi,rt_msg_choice
        jmp     rt_index_error
;;; data rt_rand_range
rt_msg_rr0      db 'empty range for randrange()',0
rt_msg_rr1      db 'empty range in randrange(',0
rt_msg_rr2      db 'empty range in randint(',0
rt_msg_rrsep    db ', ',0
rt_msg_choice   db 'Cannot choose from an empty sequence',0

;;; code rt_shuffle : rt_rand_below
rt_shuffle:                     ; eax = list, edx = element size (4 or 8): Fisher-Yates
        push    ebx esi edi ebp
        test    eax,eax
        jz      .out
        mov     ebx,eax
        mov     ebp,edx
        mov     esi,[ebx+8]
.next:  cmp     esi,1
        jbe     .out
        mov     eax,esi
        call    rt_rand_below   ; j in [0, i)
        dec     esi             ; swap elements i-1 and j
        mov     edi,[ebx+16]
        imul    eax,ebp
        mov     edx,esi
        imul    edx,ebp
        add     eax,edi
        add     edx,edi
        mov     ecx,[eax]
        mov     edi,[edx]
        mov     [eax],edi
        mov     [edx],ecx
        cmp     ebp,8
        jne     .next
        mov     ecx,[eax+4]
        mov     edi,[edx+4]
        mov     [eax+4],edi
        mov     [edx+4],ecx
        jmp     .next
.out:   pop     ebp edi esi ebx
        ret

; ---------------------------------------------------------------- arithmetic

;;; code rt_ffloor
rt_ffloor:                      ; st0 -> st0 = floor(st0)
        sub     esp,4
        fnstcw  [esp]
        mov     ax,[esp]
        and     ax,0xF3FF
        or      ax,0x0400
        mov     [esp+2],ax
        fldcw   [esp+2]
        frndint
        fldcw   [esp]
        add     esp,4
        ret

;;; code rt_fmod : rt_panic_zero rt_ffloor
rt_fmod:                        ; st0 = a, st1 = b (both popped) -> st0 = a % b (sign of b)
        fxch
        ftst
        fnstsw  ax
        sahf
        jz      rt_panic_zero   ; b == 0
        fxch                    ; st0 = a, st1 = b
        fld     st0
        fdiv    st0,st2
        call    rt_ffloor       ; st0 = floor(a/b), st1 = a, st2 = b
        fmul    st0,st2
        fsubp   st1,st0         ; st0 = a - floor(a/b)*b, st1 = b
        fstp    st1
        ret

;;; code rt_ffloordiv : rt_panic_zero rt_ffloor
rt_ffloordiv:                   ; st0 = a, st1 = b (both popped) -> st0 = a // b
        fxch
        ftst
        fnstsw  ax
        sahf
        jz      rt_panic_zero
        fdivp   st1,st0         ; st0 = a / b
        jmp     rt_ffloor

;;; code rt_fdiv : rt_panic_zero
rt_fdiv:                        ; st0 = a, st1 = b (both popped) -> st0 = a / b
        fxch
        ftst
        fnstsw  ax
        sahf
        jz      rt_panic_zero
        fdivp   st1,st0
        ret

;;; code rt_fpow : rt_ffloor rt_ext_call
rt_fpow:                        ; st0 = a, st1 = b (both popped) -> st0 = a ** b
        push    rt_fpow_x          ; in extended precision
        jmp     rt_ext_call
rt_fpow_x:
        fxch                    ; st0 = b, st1 = a
        fld     st0
        call    rt_ffloor
        fcomp   st1             ; integral exponent?
        fnstsw  ax
        sahf
        jne     .general
        fld     st0             ; |b| <= 2^30: square and multiply
        fabs
        push    0x4E800000      ; 2^30
        fcomp   dword [esp]
        add     esp,4
        fnstsw  ax
        sahf
        jae     .general
        push    eax
        fist    dword [esp]
        pop     ecx             ; ecx = b
        fstp    st0             ; st0 = a
        fld1                    ; st0 = result, st1 = base
        mov     edx,ecx
        test    ecx,ecx
        jns     @f
        neg     ecx
@@:     test    ecx,ecx
        jz      .done
        test    ecx,1
        jz      .sq
        fmul    st0,st1
.sq:    fxch
        fmul    st0,st0
        fxch
        shr     ecx,1
        jmp     @b
.done:  fstp    st1
        test    edx,edx
        jns     .out
        fld1
        fdivrp  st1,st0
.out:   ret
.general:                       ; a ** b = 2 ** (b * log2 a), a > 0
        fxch                    ; st0 = a, st1 = b
        fyl2x                   ; st0 = b * log2(a)
        fld     st0
        frndint
        fsub    st1,st0
        fxch
        f2xm1
        fld1
        faddp   st1,st0
        fscale
        fstp    st1
        ret

;;; code rt_float_parse : rt_is_space rt_scale10 rt_panic_value rt_ext_call
rt_float_parse:                 ; eax = str -> st0 = float(str)
        push    rt_float_parse_x          ; in extended precision
        jmp     rt_ext_call
rt_float_parse_x:
        push    ebx esi edi ebp
        test    eax,eax
        jz      .bad
        mov     ecx,[eax+8]
        lea     esi,[eax+12]
        lea     edi,[esi+ecx]
.lead:  cmp     esi,edi
        jae     .bad
        mov     al,[esi]
        call    rt_is_space
        jne     @f
        inc     esi
        jmp     .lead
@@:     xor     ebx,ebx         ; negative
        cmp     al,'-'
        jne     @f
        inc     ebx
        inc     esi
        jmp     .num
@@:     cmp     al,'+'
        jne     .num
        inc     esi
.num:   fldz
        xor     ebp,ebp         ; power of ten to apply
        xor     edx,edx         ; digits seen
        push    10
.int:   cmp     esi,edi
        jae     .exp
        movzx   ecx,byte [esi]
        cmp     cl,'.'
        je      .frac0
        sub     ecx,'0'
        cmp     ecx,9
        ja      .exp
        fimul   dword [esp]
        push    ecx
        fiadd   dword [esp]
        pop     ecx
        inc     edx
        inc     esi
        jmp     .int
.frac0: inc     esi
.frac:  cmp     esi,edi
        jae     .exp
        movzx   ecx,byte [esi]
        sub     ecx,'0'
        cmp     ecx,9
        ja      .exp
        fimul   dword [esp]
        push    ecx
        fiadd   dword [esp]
        pop     ecx
        inc     edx
        dec     ebp
        inc     esi
        jmp     .frac
.exp:   pop     ecx
        test    edx,edx
        jz      .word
        cmp     esi,edi
        jae     .apply
        mov     al,[esi]
        or      al,32
        cmp     al,'e'
        jne     .apply
        inc     esi
        xor     ecx,ecx         ; exponent
        xor     edx,edx         ; negative exponent
        cmp     esi,edi
        jae     .bad1
        cmp     byte [esi],'-'
        jne     @f
        inc     edx
        inc     esi
        jmp     .ed
@@:     cmp     byte [esi],'+'
        jne     .ed
        inc     esi
.ed:    cmp     esi,edi
        jae     .ee
        movzx   eax,byte [esi]
        sub     eax,'0'
        cmp     eax,9
        ja      .ee
        imul    ecx,10
        add     ecx,eax
        inc     esi
        jmp     .ed
.ee:    test    edx,edx
        jz      @f
        neg     ecx
@@:     add     ebp,ecx
.apply: mov     eax,ebp
        call    rt_scale10
.tail:  cmp     esi,edi
        jae     .done
        mov     al,[esi]
        call    rt_is_space
        jne     .bad1
        inc     esi
        jmp     .tail
.done:  test    ebx,ebx
        jz      @f
        fchs
@@:     pop     ebp edi esi ebx
        ret
.word:  mov     ecx,edi         ; no digits: inf, infinity, nan (any case)?
        sub     ecx,esi
        cmp     ecx,3
        jb      .bad1
        mov     eax,[esi]
        or      eax,0x202020
        and     eax,0xFFFFFF
        cmp     eax,'inf'
        je      .inf
        cmp     eax,'nan'
        jne     .bad1
        fstp    st0
        fldz
        fdiv    st0,st0         ; 0/0: a NaN (exceptions are masked)
        add     esi,3
        jmp     .tail
.inf:   fstp    st0
        fld1
        fldz
        fdivp   st1,st0         ; 1/0
        add     esi,3
        mov     ecx,edi
        sub     ecx,esi
        cmp     ecx,5
        jb      .tail
        mov     eax,[esi]
        or      eax,0x20202020
        cmp     eax,'init'
        jne     .tail
        mov     al,[esi+4]
        or      al,32
        cmp     al,'y'
        jne     .tail
        add     esi,5
        jmp     .tail
.bad1:  fstp    st0
.bad:   mov     esi,rt_msg_float
        jmp     rt_panic_value
;;; data rt_float_parse
rt_msg_float    db 'could not convert string to float',0

; ---------------------------------------------------------------- input()

;;; code rt_input linux : rt_write rt_sb_char rt_sb_take rt_eof_error
rt_input:                       ; eax = prompt (str or 0) -> eax = a line from stdin, without the newline (EOFError at the end)
        push    ebx esi
        test    eax,eax
        jz      @f
        mov     edx,[eax+8]
        lea     ecx,[eax+12]
        call    rt_write
@@:     push    dword [rt_sb_len]
        xor     esi,esi         ; bytes read
.next:  push    0
        mov     eax,3           ; read(0, &byte, 1)
        xor     ebx,ebx
        mov     ecx,esp
        mov     edx,1
        int     0x80
        pop     ecx
        cmp     eax,1
        jne     .eof
        inc     esi
        cmp     cl,10
        je      .end
        mov     al,cl
        call    rt_sb_char
        jmp     .next
.eof:   test    esi,esi
        jz      rt_eof_error
.end:   pop     eax
        call    rt_sb_take
        pop     esi ebx
        ret

;;; code rt_eof_error : rt_panic
rt_eof_error:                   ; input() at the end of the input
        mov     esi,rt_msg_eof
if defined rt_throw
        mov     eax,VTX_EOFError
        mov     edx,DTX_EOFError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        jmp     rt_panic
end if
;;; data rt_eof_error
if defined rt_throw
rt_msg_eof      db 'EOF when reading a line',0
else
rt_msg_eof      db 'EOFError: EOF when reading a line',0
end if

;;; code rt_input kolibri : rt_write rt_con_open rt_con_send rt_str_new
rt_input:                       ; eax = prompt -> eax = a line from the shell console
        push    ebx esi edi
        test    eax,eax
        jz      @f
        mov     edx,[eax+8]
        lea     ecx,[eax+12]
        call    rt_write
@@:     call    rt_con_open
        test    eax,eax
        jz      .empty
        mov     ebx,eax
        mov     dword [ebx+8],0 ; no reply yet
        push    1024            ; payload: our capacity
        mov     esi,esp
        mov     ecx,4
        mov     al,5            ; SC_GETS
        call    rt_con_send
        pop     ecx
        mov     esi,6000        ; up to 5 minutes
.wait:  cmp     dword [ebx+8],0
        jne     .got
        mov     eax,5
        push    ebx
        mov     ebx,5
        int     0x40
        pop     ebx
        dec     esi
        jnz     .wait
.empty: xor     eax,eax
        jmp     .out
.got:   lea     esi,[ebx+16]
        mov     ecx,esi
@@:     cmp     byte [ecx],0
        je      @f
        inc     ecx
        jmp     @b
@@:     sub     ecx,esi
        push    ecx
        mov     eax,ecx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
.out:   pop     edi esi ebx
        ret

; ---------------------------------------------------------------- KolibriOS console
; The shell's shared buffer "<pid>-SHELL" (see platform/kolibri/console.c):
;   +0 write_ptr, +4 read_ptr, +8 reply ready, +12 reply length, +16 reply[1024],
;   +1040 ring[15344] of frames [cmd][length lo][length hi][payload].
; Commands: 1 exit, 3 print (payload NUL-terminated), 5 read a line.

;;; code rt_con_open kolibri
rt_con_open:                    ; -> eax = the shared buffer, or 0 without a shell
        mov     eax,[rt_con_buf]
        cmp     eax,1
        ja      .done
        je      .none
        push    ebx esi edi
        mov     eax,9           ; our process information: PID at +30
        mov     ebx,rt_con_info
        or      ecx,-1
        int     0x40
        mov     eax,dword [rt_con_info+30]
        lea     edi,[rt_con_info+16]
        mov     ecx,10
@@:     xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        test    eax,eax
        jnz     @b
        mov     esi,edi
        lea     ecx,[rt_con_info+16]
        sub     ecx,esi
        mov     edi,rt_con_name
        rep     movsb
        mov     dword [edi],'-SHE'
        mov     dword [edi+4],'LL'
        mov     eax,68          ; open the shared memory
        mov     ebx,22
        mov     ecx,rt_con_name
        mov     edx,16384
        mov     esi,5
        int     0x40
        cmp     eax,0x1000
        jbe     .fail
        mov     [rt_con_buf],eax
        xor     edx,edx
        mov     [eax],edx
        mov     [eax+4],edx
        mov     [eax+8],edx
        mov     [eax+12],edx
        pop     edi esi ebx
.done:  ret
.fail:  mov     dword [rt_con_buf],1
        pop     edi esi ebx
.none:  xor     eax,eax
        ret
;;; bss rt_con_open kolibri
rt_con_buf      rd 1            ; 0 not opened yet, 1 no shell, else the buffer
rt_con_name     rb 32
rt_con_info     rb 1024

;;; code rt_con_send kolibri
rt_con_send:                    ; ebx = buffer, al = command, esi = payload, ecx = bytes -> eax = 0 if dropped
        push    edi ebp
        mov     ebp,eax
        lea     edx,[ecx+3]
        push    ecx
        push    esi
        mov     esi,4000        ; wait for room: yield, then sleep, then give up (~10 s)
.room:  mov     eax,[ebx]
        sub     eax,[ebx+4]
        jns     @f
        add     eax,15344
@@:     neg     eax
        add     eax,15343
        cmp     eax,edx
        jae     .put
        push    ebx edx
        mov     eax,68
        mov     ebx,1
        cmp     esi,200
        ja      @f
        mov     eax,5           ; sleep 50 ms
        mov     ebx,5
@@:     int     0x40
        pop     edx ebx
        dec     esi
        jnz     .room
        pop     esi
        pop     ecx
        xor     eax,eax
        pop     ebp edi
        ret
.put:   pop     esi
        pop     ecx
        mov     edi,[ebx]
        mov     eax,ebp
        call    .byte
        mov     eax,ecx
        call    .byte
        mov     al,ch
        call    .byte
        jecxz   .pub
@@:     mov     al,[esi]
        inc     esi
        call    .byte
        dec     ecx
        jnz     @b
.pub:   mov     [ebx],edi       ; publish the whole frame
        mov     eax,1
        pop     ebp edi
        ret
.byte:  mov     [ebx+1040+edi],al
        inc     edi
        cmp     edi,15344
        jb      @f
        xor     edi,edi
@@:     ret

;;; code rt_con_write kolibri : rt_con_open rt_con_send
rt_con_write:                   ; ecx = bytes, edx = count: print on the console (debug board without a shell)
        push    ebx esi edi
        mov     esi,ecx
        mov     edi,edx
        call    rt_con_open
        test    eax,eax
        jz      .board
        mov     ebx,eax
.next:  test    edi,edi
        jz      .out
        mov     ecx,edi
        cmp     ecx,1000
        jbe     @f
        mov     ecx,1000
@@:     sub     edi,ecx
        push    esi ecx
        sub     esp,1004        ; payload: the bytes and a NUL
        push    edi
        lea     edi,[esp+4]
        rep     movsb
        mov     byte [edi],0
        pop     edi
        mov     ecx,[esp+1004]
        inc     ecx
        mov     esi,esp
        mov     al,3
        call    rt_con_send
        add     esp,1004
        pop     ecx esi
        add     esi,ecx
        jmp     .next
.board: test    edi,edi
        jz      .out
        mov     eax,63
        mov     ebx,1
        mov     cl,[esi]
        int     0x40
        inc     esi
        dec     edi
        jmp     .board
.out:   pop     edi esi ebx
        ret

;;; code rt_con_close kolibri : rt_con_send
rt_con_close:                   ; at exit: tell the shell we are done, wait for it, unmap
        mov     eax,[rt_con_buf]
        cmp     eax,1
        jbe     .done
        push    ebx esi
        mov     ebx,eax
        mov     dword [ebx+8],0
        xor     ecx,ecx
        mov     al,1
        call    rt_con_send
        test    eax,eax
        jz      .close
        mov     esi,200
.w:     cmp     dword [ebx+8],0
        jne     .close
        mov     eax,5
        push    ebx
        mov     ebx,5
        int     0x40
        pop     ebx
        dec     esi
        jnz     .w
.close: mov     eax,68
        mov     ebx,23
        mov     ecx,rt_con_name
        int     0x40
        mov     dword [rt_con_buf],1
        pop     esi ebx
.done:  ret
