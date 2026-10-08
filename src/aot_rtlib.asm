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
;   object   +8 vtable (+0 class name, +4 base vtable, +8 methods), +12 fields
;   buffer   +8 length, +12 bytes
; A null pointer is the empty value (None): "" for str, an empty container.
; Static objects carry a huge refcount and are never destroyed.

;;; code rt_exit linux
rt_exit:                        ; ebx = exit status
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

;;; code rt_panic_key : rt_panic
rt_panic_key:
if defined rt_throw
        mov     eax,VTX_KeyError
        mov     edx,DTX_KeyError
        xor     ecx,ecx
        mov     esi,rt_msg_key_t
        jmp     rt_raise_builtin
else
        mov     esi,rt_msg_key
        jmp     rt_panic
end if
;;; data rt_panic_key
rt_msg_key      db 'KeyError: '
rt_msg_key_t    db 'key not found',0

;;; code rt_panic_key_of : rt_panic rt_errz rt_write_err rt_incref
rt_panic_key_of:                ; eax = the missing key (str): KeyError: 'key'
if defined rt_throw
        push    eax
        call    rt_incref
        pop     ecx
        mov     eax,VTX_KeyError
        mov     edx,DTX_KeyError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        push    eax
        mov     esi,rt_msg_keyq
        call    rt_errz
        pop     eax
        test    eax,eax
        jz      @f
        lea     ecx,[eax+12]
        mov     edx,[eax+8]
        call    rt_write_err
@@:     mov     esi,rt_msg_keyq+12
        jmp     rt_panic
end if
;;; data rt_panic_key_of
rt_msg_keyq     db "KeyError: '",0,"'",0

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
rt_exc_uncaught:                ; eax = exception: "Name: message" (just Name without one), exit(1)
        mov     ebx,eax
        mov     eax,[ebx+8]
        mov     eax,[eax]       ; the class name (str)
        lea     ecx,[eax+12]
        mov     edx,[eax+8]
        call    rt_write_err
        xor     edi,edi         ; KeyError shows its key quoted
if defined VTX_KeyError
        mov     ecx,[ebx+8]
.kchk:  cmp     ecx,VTX_KeyError
        jne     .knext
        inc     edi
        jmp     .kdone
.knext: mov     ecx,[ecx+4]
        test    ecx,ecx
        jnz     .kchk
.kdone:
end if
        mov     eax,[ebx+12]    ; the message: BaseException's field
        test    eax,eax
        jz      .nl
        cmp     dword [eax+8],0
        je      .nl
        push    eax
        mov     ecx,rt_s_colsp
        mov     edx,2
        call    rt_write_err
        test    edi,edi
        jz      @f
        mov     ecx,rt_s_colsp+3
        mov     edx,1
        call    rt_write_err
@@:     pop     eax
        lea     ecx,[eax+12]
        mov     edx,[eax+8]
        call    rt_write_err
        test    edi,edi
        jz      .nl
        mov     ecx,rt_s_colsp+3
        mov     edx,1
        call    rt_write_err
.nl:    mov     ecx,rt_s_colsp+2
        mov     edx,1
        call    rt_write_err
        mov     ebx,1
        jmp     rt_exit
;;; data rt_exc_uncaught
rt_s_colsp      db ': ',10,39

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
        mov     eax,16          ; header + the message field
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

;;; code rt_str_new : rt_alloc rt_free
rt_str_new:                     ; eax = length -> eax = new str (refcount 1, bytes zeroed)
        push    eax
        add     eax,13
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_free
        mov     [eax+8],ecx
        ret

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
rt_str_eq:                      ; eax = a, edx = b -> eax = 1 if equal
        push    esi edi
        mov     esi,eax
        mov     edi,edx
        xor     eax,eax
        test    esi,esi
        jz      @f
        mov     eax,[esi+8]
@@:     xor     ecx,ecx
        test    edi,edi
        jz      @f
        mov     ecx,[edi+8]
@@:     cmp     eax,ecx
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
rt_str_cmp:                     ; eax = a, edx = b -> eax = -1, 0, 1
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
rt_chars:                       ; every one-byte string, static
repeat 256
        dd      0x40000000, rt_static, 1
        db      %-1, 0, 0, 0
end repeat

;;; code rt_str_char : rt_chars rt_panic_index
rt_str_char:                    ; eax = s, edx = index -> eax = s[index] (static, borrowed)
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
@@:     test    edx,edx
        jns     @f
        add     edx,ecx
@@:     cmp     edx,ecx
        jae     rt_panic_index_s
        movzx   eax,byte [eax+12+edx]
        shl     eax,4
        add     eax,rt_chars
        ret

;;; code rt_chr : rt_chars rt_panic_value
rt_chr:                         ; eax = code -> eax = chr(code) (static)
        cmp     eax,255
        ja      .bad
        shl     eax,4
        add     eax,rt_chars
        ret
.bad:   mov     esi,rt_msg_chr
        jmp     rt_panic_value
;;; data rt_chr
rt_msg_chr      db 'chr() arg not in range(256)',0

;;; code rt_ord : rt_panic
rt_ord:                         ; eax = s -> eax = ord(s)
        test    eax,eax
        jz      .bad
        cmp     dword [eax+8],1
        jne     .bad
        movzx   eax,byte [eax+12]
        ret
.bad:   mov     esi,rt_msg_ord
        jmp     rt_panic
;;; data rt_ord
rt_msg_ord      db 'TypeError: ord() expected a character',0

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

;;; code rt_str_slice : rt_slice_range rt_str_new
rt_str_slice:                   ; [esp+4] s, lo, hi, step, flags -> eax = new str
        push    ebx esi edi
        mov     ebx,[esp+16]
        xor     eax,eax
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+8]
@@:     lea     esi,[esp+20]
        call    rt_slice_range
        push    eax edx ecx
        mov     eax,ecx
        call    rt_str_new
        pop     ecx edx esi
        push    eax
        lea     edi,[eax+12]
        test    ecx,ecx
        jz      .out
        lea     esi,[ebx+12+esi]
.copy:  mov     al,[esi]
        mov     [edi],al
        inc     edi
        add     esi,edx
        dec     ecx
        jnz     .copy
.out:   pop     eax
        pop     edi esi ebx
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

;;; code rt_str_find
rt_str_find:                    ; eax = haystack, edx = needle -> eax = first index or -1
        push    ebx esi edi ebp
        xor     ebx,ebx
        test    eax,eax
        jz      @f
        mov     ebx,[eax+8]
@@:     xor     ebp,ebp
        test    edx,edx
        jz      @f
        mov     ebp,[edx+8]
@@:     lea     esi,[eax+12]
        lea     edi,[edx+12]
        xor     ecx,ecx
        test    ebp,ebp
        jz      .found
.loop:  mov     eax,ebx
        sub     eax,ebp
        cmp     ecx,eax
        jg      .nf
        push    ecx esi edi
        add     esi,ecx
        mov     ecx,ebp
        repe    cmpsb
        pop     edi esi ecx
        je      .found
        inc     ecx
        jmp     .loop
.found: mov     eax,ecx
        jmp     .out
.nf:    or      eax,-1
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_str_count
rt_str_count:                   ; eax = s, edx = sub -> eax = non-overlapping occurrences
        push    ebx esi edi ebp
        xor     ebx,ebx
        test    eax,eax
        jz      @f
        mov     ebx,[eax+8]
@@:     xor     ebp,ebp
        test    edx,edx
        jz      @f
        mov     ebp,[edx+8]
@@:     lea     esi,[eax+12]
        lea     edi,[edx+12]
        test    ebp,ebp
        jnz     @f
        lea     eax,[ebx+1]
        jmp     .out
@@:     xor     ecx,ecx         ; position
        xor     eax,eax         ; count
.loop:  mov     edx,ebx
        sub     edx,ebp
        cmp     ecx,edx
        jg      .out
        push    eax ecx esi edi
        add     esi,ecx
        mov     ecx,ebp
        repe    cmpsb
        pop     edi esi ecx eax
        jne     .next
        inc     eax
        add     ecx,ebp
        jmp     .loop
.next:  inc     ecx
        jmp     .loop
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_str_affix
rt_str_affix:                   ; eax = s, edx = affix, ecx = 0 startswith / 1 endswith -> eax = bool
        push    ebx esi edi
        xor     ebx,ebx
        test    eax,eax
        jz      @f
        mov     ebx,[eax+8]
@@:     xor     esi,esi
        test    edx,edx
        jz      @f
        mov     esi,[edx+8]
@@:     cmp     esi,ebx
        ja      .no
        lea     edi,[eax+12]
        jecxz   @f
        add     edi,ebx
        sub     edi,esi
@@:     mov     ecx,esi
        lea     esi,[edx+12]
        xchg    esi,edi
        jecxz   .yes
        repe    cmpsb
        jne     .no
.yes:   mov     eax,1
        pop     edi esi ebx
        ret
.no:    xor     eax,eax
        pop     edi esi ebx
        ret

;;; code rt_str_case : rt_str_new
rt_str_case:                    ; eax = s, edx = 0 upper / 1 lower / 2 capitalize / 3 title / 4 swapcase -> eax = new str
        push    ebx esi edi ebp
        mov     esi,eax
        mov     ebx,edx
        xor     eax,eax
        test    esi,esi
        jz      @f
        mov     eax,[esi+8]
@@:     push    eax
        call    rt_str_new
        pop     ecx
        jecxz   .out
        push    eax
        lea     edi,[eax+12]
        add     esi,12
        mov     ebp,1           ; capitalize: the first byte; title: after a non-letter
.loop:  mov     al,[esi]
        mov     dl,al
        or      dl,32
        sub     dl,'a'
        cmp     dl,25
        ja      .nonl
        cmp     ebx,4
        jne     @f
        xor     al,32           ; swapcase
        jmp     .letter
@@:     and     al,0xDF         ; upper ...
        test    ebx,ebx
        jz      .letter
        cmp     ebx,1
        je      .lower
        test    ebp,ebp
        jnz     .letter
.lower: or      al,32           ; ... or lower
.letter:
        xor     ebp,ebp
        jmp     .put
.nonl:  mov     ebp,1
        cmp     ebx,2
        jne     .put
        xor     ebp,ebp
.put:   mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     .loop
        pop     eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_str_rfind
rt_str_rfind:                   ; eax = haystack, edx = needle -> eax = last index or -1
        push    ebx esi edi ebp
        xor     ebx,ebx
        test    eax,eax
        jz      @f
        mov     ebx,[eax+8]
@@:     xor     ebp,ebp
        test    edx,edx
        jz      @f
        mov     ebp,[edx+8]
@@:     lea     esi,[eax+12]
        lea     edi,[edx+12]
        mov     ecx,ebx
        sub     ecx,ebp         ; the last possible start
        js      .nf
        test    ebp,ebp
        jz      .found
.loop:  push    ecx esi edi
        add     esi,ecx
        mov     ecx,ebp
        repe    cmpsb
        pop     edi esi ecx
        je      .found
        dec     ecx
        jns     .loop
.nf:    or      eax,-1
        jmp     .out
.found: mov     eax,ecx
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_str_splitlines : rt_str_split rt_list_pop rt_decref rt_static
rt_str_splitlines:              ; eax = s -> eax = list of its lines, without the line breaks
        mov     edx,rt_s_lf
        call    rt_str_split
        mov     ecx,[eax+8]
        jecxz   .out
        mov     edx,[eax+16]
        mov     edx,[edx+ecx*4-4]
        test    edx,edx
        jz      .drop
        cmp     dword [edx+8],0
        jne     .out
.drop:  push    eax             ; "a\n" has one line, not a trailing empty one
        or      edx,-1
        mov     ecx,4
        call    rt_list_pop
        mov     eax,[eax]
        call    rt_decref
        pop     eax
.out:   ret
;;; data rt_str_splitlines
align 4
rt_s_lf         dd 0x40000000,rt_static,1
                db 10,0

;;; code rt_ipowmod : rt_panic_zero
rt_ipowmod:                     ; eax = base, edx = exponent (>= 0), ecx = modulus (> 0) -> eax = base ** exponent % modulus
        test    ecx,ecx
        jz      rt_panic_zero
        push    ebx esi edi ebp
        mov     ebp,ecx
        mov     esi,edx
        cdq
        idiv    ebp
        test    edx,edx
        jns     @f
        add     edx,ebp
@@:     mov     ebx,edx         ; base % m
        mov     eax,1
        xor     edx,edx
        div     ebp
        mov     edi,edx         ; result = 1 % m
.loop:  test    esi,esi
        jz      .done
        test    esi,1
        jz      .sq
        mov     eax,edi
        mul     ebx
        div     ebp
        mov     edi,edx
.sq:    mov     eax,ebx
        mul     ebx
        div     ebp
        mov     ebx,edx
        shr     esi,1
        jmp     .loop
.done:  mov     eax,edi
        pop     ebp edi esi ebx
        ret

;;; code rt_is_space
rt_is_space:                    ; al = byte -> ZF set when it is whitespace
        cmp     al,' '
        je      @f
        cmp     al,9
        jb      @f
        cmp     al,13
        ja      @f
        cmp     al,al
@@:     ret

;;; code rt_str_strip : rt_str_new rt_is_space
rt_str_strip:                   ; eax = s, edx = 1 left / 2 right / 3 both, ecx = the characters (str), 0: whitespace -> eax = new str
        push    ebx esi edi ebp
        mov     ebp,ecx
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
@@:     lea     esi,[eax+12]
        lea     edi,[esi+ecx]   ; end
        test    edx,1
        jz      .right
.l:     cmp     esi,edi
        jae     .right
        mov     al,[esi]
        call    .strip
        jne     .right
        inc     esi
        jmp     .l
.right: test    edx,2
        jz      .make
.r:     cmp     edi,esi
        jbe     .make
        mov     al,[edi-1]
        call    .strip
        jne     .make
        dec     edi
        jmp     .r
.make:  mov     ebx,edi
        sub     ebx,esi
        mov     eax,ebx
        call    rt_str_new
        lea     edi,[eax+12]
        mov     ecx,ebx
        rep     movsb
        pop     ebp edi esi ebx
        ret
.strip: test    ebp,ebp         ; ZF set when al goes
        jz      rt_is_space
        push    ecx edi
        mov     ecx,[ebp+8]
        lea     edi,[ebp+12]
        repne   scasb
        pop     edi ecx
        ret

;;; code rt_str_replace : rt_sb_bytes rt_sb_take
rt_str_replace:                 ; eax = s, edx = old, ecx = new -> eax = new str
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        mov     esi,eax         ; s
        mov     edi,edx         ; old
        mov     ebp,ecx         ; new
        xor     ebx,ebx         ; position
.loop:  xor     eax,eax
        test    esi,esi
        jz      @f
        mov     eax,[esi+8]
@@:     cmp     ebx,eax
        jae     .done
        xor     ecx,ecx
        test    edi,edi
        jz      .char
        mov     ecx,[edi+8]
        jecxz   .char
        mov     edx,eax
        sub     edx,ebx
        cmp     ecx,edx
        ja      .char
        push    esi edi
        lea     esi,[esi+12+ebx]
        add     edi,12
        repe    cmpsb
        pop     edi esi
        jne     .char
        test    ebp,ebp
        jz      @f
        lea     eax,[ebp+12]
        mov     edx,[ebp+8]
        call    rt_sb_bytes
@@:     add     ebx,[edi+8]
        jmp     .loop
.char:  lea     eax,[esi+12+ebx]
        mov     edx,1
        call    rt_sb_bytes
        inc     ebx
        jmp     .loop
.done:  pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret

;;; code rt_str_split : rt_list_new rt_list_push rt_list_destroy_ptr rt_str_new rt_is_space rt_panic_value
rt_str_split:                   ; eax = s, edx = separator (0: runs of whitespace) -> eax = list[str]
        push    ebx esi edi ebp
        mov     esi,eax
        mov     edi,edx
        mov     eax,rt_list_destroy_ptr
        call    rt_list_new
        mov     ebp,eax         ; result
        xor     ebx,ebx         ; position
        xor     ecx,ecx
        test    esi,esi
        jz      @f
        mov     ecx,[esi+8]
@@:     push    ecx             ; [esp] = length
        test    edi,edi
        jz      .ws
        cmp     dword [edi+8],0
        je      .empty
.sep:   mov     edx,ebx         ; piece start
.scan:  mov     eax,[esp]
        sub     eax,[edi+8]
        cmp     ebx,eax
        jg      .last
        push    esi edi
        lea     esi,[esi+12+ebx]
        mov     ecx,[edi+8]
        add     edi,12
        repe    cmpsb
        pop     edi esi
        je      .cut
        inc     ebx
        jmp     .scan
.cut:   mov     ecx,ebx
        call    .piece          ; [edx, ecx)
        add     ebx,[edi+8]
        jmp     .sep
.last:  mov     ecx,[esp]
        call    .piece
        jmp     .done
.ws:    cmp     ebx,[esp]
        jae     .done
        mov     al,[esi+12+ebx]
        call    rt_is_space
        jne     @f
        inc     ebx
        jmp     .ws
@@:     mov     edx,ebx
.word:  inc     ebx
        cmp     ebx,[esp]
        jae     @f
        mov     al,[esi+12+ebx]
        call    rt_is_space
        jne     .word
@@:     mov     ecx,ebx
        call    .piece
        jmp     .ws
.done:  pop     ecx
        mov     eax,ebp
        pop     ebp edi esi ebx
        ret
.empty: mov     esi,rt_msg_sep
        jmp     rt_panic_value
.piece:                         ; append s[edx:ecx] to the result
        push    esi edi edx ecx
        sub     ecx,edx
        push    ecx
        mov     eax,ecx
        call    rt_str_new
        pop     ecx
        mov     edi,[esp+4]     ; start
        lea     esi,[esi+12+edi]
        lea     edi,[eax+12]
        rep     movsb
        push    eax
        mov     eax,ebp
        mov     edx,4
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        pop     ecx edx edi esi
        ret
;;; data rt_str_split
rt_msg_sep      db 'empty separator',0

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

;;; code rt_str_is : rt_is_space
rt_str_is:                      ; eax = s, edx = 0 digit / 1 alpha / 2 space / 3 upper / 4 lower / 5 alnum -> eax = bool
        push    ebx esi
        xor     ecx,ecx
        test    eax,eax
        jz      .no
        mov     ecx,[eax+8]
        jecxz   .no
        lea     esi,[eax+12]
        xor     ebx,ebx         ; cased letters seen (upper/lower)
.loop:  mov     al,[esi]
        cmp     edx,2
        jne     @f
        call    rt_is_space
        jne     .no
        jmp     .next
@@:     cmp     edx,0
        jne     @f
        cmp     al,'0'
        jb      .no
        cmp     al,'9'
        ja      .no
        jmp     .next
@@:     mov     ah,al
        or      ah,32
        cmp     ah,'a'
        jb      .notalpha
        cmp     ah,'z'
        ja      .notalpha
        inc     ebx             ; a letter
        cmp     edx,3
        jne     @f
        cmp     al,'a'
        jae     .no
@@:     cmp     edx,4
        jne     .next
        cmp     al,'Z'
        jbe     .no
        jmp     .next
.notalpha:
        cmp     edx,5
        jne     @f
        cmp     al,'0'
        jb      .no
        cmp     al,'9'
        ja      .no
        jmp     .next
@@:     cmp     edx,1
        je      .no             ; alpha: every byte a letter
.next:  inc     esi
        dec     ecx
        jnz     .loop
        cmp     edx,3
        jb      .yes
        cmp     edx,5
        je      .yes
        test    ebx,ebx         ; upper/lower need a letter
        jz      .no
.yes:   mov     eax,1
        pop     esi ebx
        ret
.no:    xor     eax,eax
        pop     esi ebx
        ret

;;; code rt_int_parse : rt_is_space rt_panic_value
rt_int_parse:                   ; eax = str -> eax = int(str)
        push    ebx esi edi
        xor     ecx,ecx
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
@@:     xor     ebx,ebx         ; negative?
        cmp     al,'-'
        jne     @f
        inc     ebx
        inc     esi
        jmp     .first
@@:     cmp     al,'+'
        jne     .first
        inc     esi
.first: xor     eax,eax
        cmp     esi,edi
        jae     .bad
        movzx   ecx,byte [esi]
        sub     ecx,'0'
        cmp     ecx,9
        ja      .bad
.digit: movzx   ecx,byte [esi]
        sub     ecx,'0'
        cmp     ecx,9
        ja      .tail
        imul    eax,10
        add     eax,ecx
        inc     esi
        cmp     esi,edi
        jb      .digit
.tail:  cmp     esi,edi
        jae     .done
        push    eax
        mov     al,[esi]
        call    rt_is_space
        pop     eax
        jne     .bad
        inc     esi
        jmp     .tail
.done:  test    ebx,ebx
        jz      @f
        neg     eax
@@:     pop     edi esi ebx
        ret
.bad:   mov     esi,rt_msg_int
        jmp     rt_panic_value
;;; data rt_int_parse
rt_msg_int      db 'invalid literal for int()',0

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

;;; code rt_sb_repr_str : rt_sb_char
rt_sb_repr_str:                 ; eax = str: Python's repr - 'text' ("text" if it has ' but no "), escapes
        push    ebx esi edi
        xor     esi,esi
        xor     edi,edi
        test    eax,eax
        jz      @f
        lea     esi,[eax+12]
        mov     edi,[eax+8]
@@:     mov     bl,39
        xor     ecx,ecx
        xor     edx,edx         ; dl: has ', dh: has "
.scan:  cmp     ecx,edi
        jae     .chosen
        mov     al,[esi+ecx]
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
        mov     bl,'"'
@@:     mov     al,bl
        call    rt_sb_char
        add     edi,esi
.next:  cmp     esi,edi
        jae     .done
        mov     al,[esi]
        inc     esi
        cmp     al,'\'
        je      .self
        cmp     al,bl
        je      .self
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
        je      .hex
        call    rt_sb_char
        jmp     .next
.self:  mov     ah,al
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

;;; code rt_utoa
rt_utoa:                        ; eax = value, ecx = base, edi = end of a buffer -> edi = first digit (writes backwards)
        push    ebx
@@:     xor     edx,edx
        div     ecx
        mov     bl,dl
        add     bl,'0'
        cmp     bl,'9'
        jbe     .d
        add     bl,'a'-'9'-1
.d:     dec     edi
        mov     [edi],bl
        test    eax,eax
        jnz     @b
        pop     ebx
        ret

;;; code rt_sb_int : rt_utoa rt_sb_bytes
rt_sb_int:                      ; eax = int (decimal)
        push    edi
        sub     esp,16
        lea     edi,[esp+16]
        test    eax,eax
        jns     @f
        neg     eax
        mov     ecx,10
        call    rt_utoa
        dec     edi
        mov     byte [edi],'-'
        jmp     .out
@@:     mov     ecx,10
        call    rt_utoa
.out:   mov     eax,edi
        lea     edx,[esp+16]
        sub     edx,edi
        call    rt_sb_bytes
        add     esp,16
        pop     edi
        ret

;;; code rt_sb_radix : rt_utoa rt_sb_bytes
rt_sb_radix:                    ; eax = int, edx = base (8/16), ecx = 1 for upper case
        push    ebx edi
        mov     ebx,ecx
        sub     esp,40
        lea     edi,[esp+40]
        mov     ecx,edx
        test    eax,eax
        jns     @f
        neg     eax
        call    rt_utoa
        dec     edi
        mov     byte [edi],'-'
        jmp     .case
@@:     call    rt_utoa
.case:  test    ebx,ebx
        jz      .out
        mov     ecx,edi
        lea     edx,[esp+40]
@@:     cmp     ecx,edx
        jae     .out
        cmp     byte [ecx],'a'
        jb      .n
        sub     byte [ecx],32
.n:     inc     ecx
        jmp     @b
.out:   mov     eax,edi
        lea     edx,[esp+40]
        sub     edx,edi
        call    rt_sb_bytes
        add     esp,40
        pop     edi ebx
        ret

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

;;; code rt_sb_pad : rt_sb_need rt_sb_char
rt_sb_pad:                      ; eax = mark of a field, edx = width | 0x10000 left | 0x20000 zeros | 0x40000 centre | fill char << 24: pad [mark, length)
        push    ebx esi edi ebp
        mov     ebx,[rt_sb_len]
        sub     ebx,eax         ; field length
        movzx   ecx,dx
        cmp     ebx,ecx
        jae     .out
        sub     ecx,ebx         ; padding
        mov     ebp,edx
        shr     ebp,24
        jnz     @f
        mov     ebp,' '
@@:     test    edx,0x40000
        jz      .lr
        mov     esi,ecx         ; centre: half in front, the rest behind
        shr     ecx,1
        sub     esi,ecx
        add     ebx,esi
        push    ecx
@@:     push    eax
        mov     eax,ebp
        call    rt_sb_char
        pop     eax
        dec     esi
        jnz     @b
        pop     ecx
        jecxz   .out
        jmp     .front
.lr:    test    edx,0x10000
        jz      .front
.sp:    push    eax ecx
        mov     eax,ebp
        call    rt_sb_char
        pop     ecx eax
        dec     ecx
        jnz     .sp
        jmp     .out
.front: push    eax edx ecx
        mov     eax,ecx
        call    rt_sb_need      ; length += padding
        pop     ecx edx eax
        mov     esi,[rt_sb_buf]
        add     esi,eax         ; field start
        push    ecx
        lea     edi,[esi+ebx-1]
        add     edi,ecx
        push    esi
        lea     esi,[esi+ebx-1]
        mov     ecx,ebx
        std
        rep     movsb           ; shift the field right
        cld
        pop     esi
        pop     ecx
        mov     eax,ebp
        test    edx,0x20000
        jz      .fill
        mov     al,'0'
        mov     ah,[esi+ecx]    ; a sign stays in front of the zeros
        cmp     ah,'-'
        je      .sign
        cmp     ah,'+'
        jne     .fill
.sign:  mov     [esi],ah
        mov     [esi+ecx],al
        inc     esi
.fill:  mov     edi,esi
        rep     stosb
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_sb_intc : rt_utoa rt_sb_bytes rt_sb_char
rt_sb_intc:                     ; eax = int: decimal with thousands separators (1,234,567)
        push    ebx esi edi
        sub     esp,44
        lea     edi,[esp+44]
        mov     ebx,eax
        test    eax,eax
        jns     @f
        neg     eax
@@:     mov     ecx,10
        call    rt_utoa
        lea     esi,[esp+44]
        sub     esi,edi         ; digits
        test    ebx,ebx
        jns     @f
        mov     al,'-'
        call    rt_sb_char
@@:     mov     eax,esi
        xor     edx,edx
        mov     ecx,3
        div     ecx
        test    edx,edx
        jnz     @f
        mov     edx,3
@@:     mov     ebx,edx         ; digits of the first group
.grp:   mov     eax,edi
        mov     edx,ebx
        add     edi,ebx
        sub     esi,ebx
        call    rt_sb_bytes
        test    esi,esi
        jz      .out
        mov     al,','
        call    rt_sb_char
        mov     ebx,3
        jmp     .grp
.out:   add     esp,44
        pop     edi esi ebx
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

;;; code rt_print_int : rt_utoa rt_write
rt_print_int:                   ; eax = int: print it and a newline
        push    edi
        sub     esp,16
        lea     edi,[esp+15]
        mov     byte [edi],10
        mov     ecx,10
        test    eax,eax
        jns     @f
        neg     eax
        call    rt_utoa
        dec     edi
        mov     byte [edi],'-'
        jmp     .out
@@:     call    rt_utoa
.out:   mov     ecx,edi
        lea     edx,[esp+16]
        sub     edx,edi
        call    rt_write
        add     esp,16
        pop     edi
        ret

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
; P-1 decimals). Otherwise printf %.*g.
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
rt_s_nan        db 'nan',0
rt_s_inf        db 'inf',0
rt_s_minf       db '-inf',0

;;; code rt_sb_float : rt_sb_gen rt_sb_take rt_sb_str rt_float_parse rt_free
rt_sb_float:                    ; st0 = value (popped): Python's repr - the shortest text that reads back the same
        push    ebx esi
        sub     esp,16          ; +0 value, +8 value read back
        fstp    qword [esp]
        mov     esi,15
        mov     eax,[esp+4]
        and     eax,0x7FF00000
        cmp     eax,0x7FF00000
        je      .last           ; inf, nan
.try:   mov     ebx,[rt_sb_len]
        fld     qword [esp]
        mov     edx,esi
        mov     ecx,3
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
        inc     esi
        jmp     .try
.last:  fld     qword [esp]
        mov     edx,esi
        mov     ecx,3
        call    rt_sb_gen
.done:  add     esp,16
        pop     esi ebx
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

; ---------------------------------------------------------------- lists and sets
; Element kinds (`kind` arguments): 0 int/bool, 1 str, 2 float (8 bytes; passed
; by address), 3 other object (compared by identity), 4 tuple (compared by
; value). Kinds 1, 3 and 4 hold references.

;;; code rt_list_new : rt_alloc
rt_list_new:                    ; eax = destroy routine -> eax = new empty list
        push    eax
        mov     eax,20
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     [eax+4],ecx
        ret

;;; code rt_list_destroy : rt_free
rt_list_destroy:                ; destroy routine of lists/sets of plain values
        push    eax
        mov     eax,[eax+16]
        call    rt_free
        pop     eax
        jmp     rt_free

;;; code rt_list_destroy_ptr : rt_list_destroy rt_decref
rt_list_destroy_ptr:            ; ... of references: drop the elements first
        push    ebx esi
        mov     ebx,eax
        mov     esi,[ebx+8]
.drop:  dec     esi
        js      .done
        mov     eax,[ebx+16]
        mov     eax,[eax+esi*4]
        call    rt_decref
        jmp     .drop
.done:  mov     eax,ebx
        pop     esi ebx
        jmp     rt_list_destroy

;;; code rt_list_reserve : rt_alloc rt_free
rt_list_reserve:                ; eax = list, edx = element size, ecx = wanted length
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
        push    edx
        mul     edx
        call    rt_alloc
        pop     edx
        mov     edi,eax
        mov     esi,[ebx+16]
        mov     ecx,[ebx+8]
        imul    ecx,edx
        rep     movsb
        mov     edi,eax
        mov     eax,[ebx+16]
        mov     [ebx+16],edi
        call    rt_free
        mov     eax,ebx
        pop     edi esi ebx
.ok:    ret

;;; code rt_list_push : rt_list_reserve rt_panic_none
rt_list_push:                   ; eax = list, edx = element size -> eax = address of a new last slot
        test    eax,eax
        jz      rt_panic_none
        push    ebx
        mov     ebx,eax
        mov     ecx,[eax+8]
        inc     ecx
        push    edx
        call    rt_list_reserve
        pop     edx
        mov     eax,[ebx+8]
        inc     dword [ebx+8]
        imul    eax,edx
        add     eax,[ebx+16]
        pop     ebx
        ret

;;; code rt_list_at : rt_panic_index
rt_list_at:                     ; eax = list, edx = index, ecx = element size -> eax = address of the element
        test    eax,eax
        jz      rt_panic_index
        test    edx,edx
        jns     @f
        add     edx,[eax+8]
@@:     cmp     edx,[eax+8]
        jae     rt_panic_index
        imul    edx,ecx
        mov     eax,[eax+16]
        add     eax,edx
        ret

;;; code rt_list_pop : rt_panic_index rt_scratch
rt_list_pop:                    ; eax = list, edx = index, ecx = element size -> eax = rt_scratch holding the removed element
        push    ebx esi edi
        test    eax,eax
        jz      rt_panic_index
        mov     ebx,eax
        test    edx,edx
        jns     @f
        add     edx,[eax+8]
@@:     cmp     edx,[eax+8]
        jae     rt_panic_index
        mov     esi,edx
        imul    esi,ecx
        add     esi,[ebx+16]    ; the element
        mov     eax,[esi]
        mov     [rt_scratch],eax
        cmp     ecx,8
        jne     @f
        mov     eax,[esi+4]
        mov     [rt_scratch+4],eax
@@:     mov     eax,[ebx+8]
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

;;; code rt_list_insert : rt_list_reserve rt_panic_none
rt_list_insert:                 ; eax = list, edx = index, ecx = element size -> eax = address of a new slot at index
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,ecx
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
        mov     edx,ebp
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

;;; code rt_list_find : rt_str_eq
rt_list_find:                   ; eax = list, edx = value (float: its address), ecx = kind -> eax = index or -1
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        mov     ebp,ecx
        xor     esi,esi
        test    ebx,ebx
        jz      .nf
.loop:  cmp     esi,[ebx+8]
        jae     .nf
        mov     eax,[ebx+16]
        cmp     ebp,2
        je      .flt
        mov     eax,[eax+esi*4]
        cmp     ebp,1
        je      .str
if defined rt_tuple_eq
        cmp     ebp,4
        je      .tup
end if
        cmp     eax,edi
        je      .found
        jmp     .next
if defined rt_tuple_eq
.tup:   mov     edx,edi
        call    rt_tuple_eq
        test    eax,eax
        jnz     .found
        jmp     .next
end if
.str:   mov     edx,edi
        call    rt_str_eq
        test    eax,eax
        jnz     .found
        jmp     .next
.flt:   fld     qword [eax+esi*8]
        fcomp   qword [edi]
        fnstsw  ax
        sahf
        jp      .next
        je      .found
.next:  inc     esi
        jmp     .loop
.found: mov     eax,esi
        jmp     .out
.nf:    or      eax,-1
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_count : rt_str_eq
rt_list_count:                  ; eax = list, edx = value, ecx = kind -> eax = occurrences
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        mov     ebp,ecx
        xor     esi,esi
        push    0
        test    ebx,ebx
        jz      .out
.loop:  cmp     esi,[ebx+8]
        jae     .out
        mov     eax,[ebx+16]
        cmp     ebp,2
        je      .flt
        mov     eax,[eax+esi*4]
        cmp     ebp,1
        je      .str
if defined rt_tuple_eq
        cmp     ebp,4
        je      .tup
end if
        cmp     eax,edi
        jne     .next
        jmp     .hit
if defined rt_tuple_eq
.tup:   mov     edx,edi
        call    rt_tuple_eq
        test    eax,eax
        jz      .next
        jmp     .hit
end if
.str:   mov     edx,edi
        call    rt_str_eq
        test    eax,eax
        jz      .next
        jmp     .hit
.flt:   fld     qword [eax+esi*8]
        fcomp   qword [edi]
        fnstsw  ax
        sahf
        jp      .next
        jne     .next
.hit:   inc     dword [esp]
.next:  inc     esi
        jmp     .loop
.out:   pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_list_index : rt_list_find rt_panic_value
rt_list_index:                  ; like rt_list_find, but a missing value is an error
        call    rt_list_find
        test    eax,eax
        js      .bad
        ret
.bad:   mov     esi,rt_msg_notin
        jmp     rt_panic_value
;;; data rt_list_index
rt_msg_notin    db 'value is not in the list',0

;;; code rt_list_remove : rt_list_index rt_list_pop rt_decref
rt_list_remove:                 ; eax = list, edx = value, ecx = kind: remove the first equal element
        push    ebx esi
        mov     ebx,eax
        mov     esi,ecx
        call    rt_list_index
        mov     edx,eax
        mov     eax,ebx
        mov     ecx,4
        cmp     esi,2
        jne     @f
        mov     ecx,8
@@:     call    rt_list_pop
        cmp     esi,1
        je      .drop
        cmp     esi,3
        jb      .out
.drop:  mov     eax,[eax]
        call    rt_decref
.out:   pop     esi ebx
        ret

;;; code rt_list_reverse
rt_list_reverse:                ; eax = list, edx = element size
        push    ebx esi edi
        test    eax,eax
        jz      .out
        mov     ecx,[eax+8]
        mov     esi,[eax+16]
        dec     ecx
        imul    ecx,edx
        lea     edi,[esi+ecx]   ; last element
.loop:  cmp     esi,edi
        jae     .out
        mov     eax,[esi]
        mov     ebx,[edi]
        mov     [esi],ebx
        mov     [edi],eax
        cmp     edx,8
        jne     @f
        mov     eax,[esi+4]
        mov     ebx,[edi+4]
        mov     [esi+4],ebx
        mov     [edi+4],eax
@@:     add     esi,edx
        sub     edi,edx
        jmp     .loop
.out:   pop     edi esi ebx
        ret

;;; code rt_list_sort : rt_str_cmp
rt_list_sort:                   ; eax = list, edx = kind (0 int, 1 str, 2 float, 4 tuple): stable insertion sort, ascending
        push    ebx esi edi ebp
        test    eax,eax
        jz      .out
        mov     ebx,[eax+16]
        mov     ebp,[eax+8]
        mov     esi,1           ; i
.outer: cmp     esi,ebp
        jae     .out
        mov     edi,esi         ; j
        cmp     edx,2
        je      .fl
.in:    test    edi,edi         ; 4-byte keys: while j > 0 and a[j-1] > a[j]: swap
        jz      .nexti
        mov     eax,[ebx+edi*4-4]
        mov     ecx,[ebx+edi*4]
        cmp     edx,1
        je      .s
if defined rt_tuple_cmp
        cmp     edx,4
        je      .t
end if
        cmp     eax,ecx
        jle     .nexti
        jmp     .swap
if defined rt_tuple_cmp
.t:     push    eax ecx edx     ; tuples
        mov     edx,ecx
        call    rt_tuple_cmp
        test    eax,eax
        pop     edx ecx eax
        jle     .nexti
        jmp     .swap
end if
.s:     push    eax ecx edx     ; strings: a[j-1] vs a[j]
        mov     edx,ecx
        call    rt_str_cmp
        test    eax,eax
        pop     edx ecx eax
        jle     .nexti
.swap:  mov     [ebx+edi*4-4],ecx
        mov     [ebx+edi*4],eax
        dec     edi
        jmp     .in
.fl:    test    edi,edi         ; 8-byte keys
        jz      .nexti
        fld     qword [ebx+edi*8]
        fcomp   qword [ebx+edi*8-8]
        fnstsw  ax
        sahf
        jae     .nexti          ; a[j] >= a[j-1]
        mov     eax,[ebx+edi*8-8]
        mov     ecx,[ebx+edi*8]
        mov     [ebx+edi*8-8],ecx
        mov     [ebx+edi*8],eax
        mov     eax,[ebx+edi*8-4]
        mov     ecx,[ebx+edi*8+4]
        mov     [ebx+edi*8-4],ecx
        mov     [ebx+edi*8+4],eax
        dec     edi
        jmp     .fl
.nexti: inc     esi
        jmp     .outer
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_extend : rt_list_push rt_incref rt_panic_none
rt_list_extend:                 ; eax = destination, edx = source, ecx = kind: append the source's elements
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        mov     ebp,ecx
        xor     edi,edi
        test    esi,esi
        jz      .out
        mov     eax,[esi+8]     ; a list extended by itself: stop at its old length
        push    eax
.loop:  cmp     edi,[esp]
        jae     .done
        mov     eax,ebx
        mov     edx,4
        cmp     ebp,2
        jne     @f
        mov     edx,8
@@:     call    rt_list_push
        mov     ecx,[esi+16]
        cmp     ebp,2
        je      .f
        mov     ecx,[ecx+edi*4]
        mov     [eax],ecx
        cmp     ebp,1
        jb      .next
        mov     eax,ecx
        call    rt_incref
        jmp     .next
.f:     mov     edx,[ecx+edi*8]
        mov     [eax],edx
        mov     edx,[ecx+edi*8+4]
        mov     [eax+4],edx
.next:  inc     edi
        jmp     .loop
.done:  pop     eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_copy : rt_list_new rt_list_extend
rt_list_copy:                   ; eax = list, edx = kind -> eax = new list (same kind of destroy routine)
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,[ebx+4]
        call    rt_list_new
        push    eax
        mov     edx,ebx
        mov     ecx,esi
        call    rt_list_extend
        pop     eax
        pop     esi ebx
        ret

;;; code rt_list_concat : rt_list_copy rt_list_extend
rt_list_concat:                 ; eax = a, edx = b, ecx = kind -> eax = new list a + b
        push    ebx esi
        mov     ebx,edx
        mov     esi,ecx
        mov     edx,ecx
        call    rt_list_copy
        push    eax
        mov     edx,ebx
        mov     ecx,esi
        call    rt_list_extend
        pop     eax
        pop     esi ebx
        ret

;;; code rt_list_mul : rt_list_new rt_list_extend
rt_list_mul:                    ; eax = list, edx = n, ecx = kind -> eax = new list
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        mov     edi,ecx
        mov     eax,[ebx+4]
        call    rt_list_new
        push    eax
.rep:   test    esi,esi
        jle     .out
        mov     eax,[esp]
        mov     edx,ebx
        mov     ecx,edi
        call    rt_list_extend
        dec     esi
        jmp     .rep
.out:   pop     eax
        pop     edi esi ebx
        ret

;;; code rt_list_slice : rt_slice_range rt_list_new rt_list_push rt_incref
rt_list_slice:                  ; [esp+4] list, lo, hi, step, flags, kind -> eax = new list
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
        mov     eax,rt_list_destroy
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+4]
@@:     call    rt_list_new
        push    eax
.loop:  test    ebp,ebp
        jz      .out
        mov     eax,[esp]
        mov     edx,4
        cmp     dword [esp+44],2
        jne     @f
        mov     edx,8
@@:     call    rt_list_push
        mov     ecx,[ebx+16]
        cmp     dword [esp+44],2
        je      .f
        mov     ecx,[ecx+esi*4]
        mov     [eax],ecx
        cmp     dword [esp+44],1
        jb      .next
        mov     eax,ecx
        call    rt_incref
        jmp     .next
.f:     mov     edx,[ecx+esi*8]
        mov     [eax],edx
        mov     edx,[ecx+esi*8+4]
        mov     [eax+4],edx
.next:  add     esi,edi
        dec     ebp
        jmp     .loop
.out:   pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_list_clear : rt_decref
rt_list_clear:                  ; eax = list, edx = kind
        test    eax,eax
        jz      .out
        cmp     edx,1
        je      .drop
        cmp     edx,3
        jae     .drop
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

;;; code rt_list_eq : rt_list_find rt_str_eq
rt_list_eq:                     ; eax = a, edx = b, ecx = kind -> eax = bool (same elements in order)
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        mov     ebp,ecx
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
.loop:  cmp     esi,edx
        jae     .yes
        mov     eax,[ebx+16]
        mov     ecx,[edi+16]
        cmp     ebp,2
        je      .f
        mov     eax,[eax+esi*4]
        mov     ecx,[ecx+esi*4]
        cmp     ebp,1
        je      .s
if defined rt_tuple_eq
        cmp     ebp,4
        je      .t
end if
        cmp     eax,ecx
        jne     .no
        jmp     .next
if defined rt_tuple_eq
.t:     push    edx
        mov     edx,ecx
        call    rt_tuple_eq
        pop     edx
        test    eax,eax
        jz      .no
        jmp     .next
end if
.s:     push    edx
        mov     edx,ecx
        call    rt_str_eq
        pop     edx
        test    eax,eax
        jz      .no
        jmp     .next
.f:     fld     qword [eax+esi*8]
        fcomp   qword [ecx+esi*8]
        push    eax
        fnstsw  ax
        sahf
        pop     eax
        jne     .no
.next:  inc     esi
        jmp     .loop
.yes:   mov     eax,1
        jmp     .out
.no:    xor     eax,eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_sum
rt_list_sum:                    ; eax = list, edx = kind (0 int -> eax, 2 float -> st0)
        push    esi
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
        mov     esi,[eax+16]
@@:     cmp     edx,2
        je      .f
        xor     eax,eax
        jecxz   .out
@@:     add     eax,[esi]
        add     esi,4
        dec     ecx
        jnz     @b
.out:   pop     esi
        ret
.f:     fldz
        jecxz   .out
@@:     fadd    qword [esi]
        add     esi,8
        dec     ecx
        jnz     @b
        pop     esi
        ret

;;; code rt_list_minmax : rt_str_cmp rt_panic_value
rt_list_minmax:                 ; eax = list, edx = kind | 0x100 for max -> eax (int/str) or st0 (float)
        push    ebx esi edi ebp
        test    eax,eax
        jz      .empty
        mov     ecx,[eax+8]
        test    ecx,ecx
        jz      .empty
        mov     esi,[eax+16]
        mov     ebp,edx
        cmp     dl,2
        je      .f
        mov     ebx,[esi]       ; best
.l:     add     esi,4
        dec     ecx
        jz      .done
        mov     edi,[esi]
        cmp     dl,1
        je      .s
        cmp     dl,4            ; tuples: compared item by item
        je      .s
        test    ebp,0x100
        jnz     .imax
        cmp     edi,ebx
        jge     .l
        mov     ebx,edi
        jmp     .l
.imax:  cmp     edi,ebx
        jle     .l
        mov     ebx,edi
        jmp     .l
.s:     push    ecx edx
        mov     eax,edi
        mov     edx,ebx
if defined rt_tuple_cmp
        cmp     byte [esp],4
        jne     .sc
        call    rt_tuple_cmp
        jmp     .sd
end if
.sc:    call    rt_str_cmp
.sd:    pop     edx ecx
        test    ebp,0x100
        jnz     .smax
        test    eax,eax
        jns     .l
        mov     ebx,edi
        jmp     .l
.smax:  test    eax,eax
        jle     .l
        mov     ebx,edi
        jmp     .l
.done:  mov     eax,ebx
        pop     ebp edi esi ebx
        ret
.f:     fld     qword [esi]
.fl:    add     esi,8
        dec     ecx
        jz      .fdone
        fld     qword [esi]
        test    ebp,0x100
        jnz     .fmax
        fcom    st1
        fnstsw  ax
        sahf
        jb      .take
        fstp    st0
        jmp     .fl
.fmax:  fcom    st1
        fnstsw  ax
        sahf
        ja      .take
        fstp    st0
        jmp     .fl
.take:  fstp    st1
        jmp     .fl
.fdone: pop     ebp edi esi ebx
        ret
.empty: mov     esi,rt_msg_empty
        jmp     rt_panic_value
;;; data rt_list_minmax
rt_msg_empty    db 'min()/max() of an empty sequence',0

;;; code rt_set_add : rt_list_find rt_list_push rt_incref
rt_set_add:                     ; eax = set, edx = value (float: its address), ecx = kind
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        mov     edi,ecx
        call    rt_list_find
        test    eax,eax
        jns     .out
        mov     eax,ebx
        mov     edx,4
        cmp     edi,2
        jne     @f
        mov     edx,8
@@:     call    rt_list_push
        cmp     edi,2
        je      .f
        mov     [eax],esi
        cmp     edi,1
        jb      .out
        mov     eax,esi
        call    rt_incref
        jmp     .out
.f:     mov     ecx,[esi]
        mov     [eax],ecx
        mov     ecx,[esi+4]
        mov     [eax+4],ecx
.out:   pop     edi esi ebx
        ret

;;; code rt_set_remove : rt_list_find rt_list_pop rt_decref rt_panic_key
rt_set_remove:                  ; eax = set, edx = value, ecx = kind | 0x100 to ignore a missing value (discard)
        push    ebx esi edi
        mov     ebx,eax
        mov     edi,ecx
        movzx   esi,cl          ; kind
        mov     ecx,esi
        call    rt_list_find
        test    eax,eax
        jns     .found
        test    edi,0x100
        jz      rt_panic_key
        jmp     .out
.found: mov     edx,eax
        mov     eax,ebx
        mov     ecx,4
        cmp     esi,2
        jne     @f
        mov     ecx,8
@@:     call    rt_list_pop
        cmp     esi,1
        je      .drop
        cmp     esi,3
        jb      .out
.drop:  mov     eax,[eax]
        call    rt_decref
.out:   pop     edi esi ebx
        ret

;;; code rt_set_op : rt_list_new rt_set_add rt_list_find
rt_set_op:                      ; eax = a, edx = b, ecx = kind | op<<8 (0 union, 1 intersection, 2 difference) -> eax = new set
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        mov     ebp,ecx
        mov     eax,rt_list_destroy
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+4]
        jmp     .new
@@:     test    esi,esi
        jz      .new
        mov     eax,[esi+4]
.new:   call    rt_list_new
        push    eax
        xor     edi,edi         ; elements of a
.la:    test    ebx,ebx
        jz      .b
        cmp     edi,[ebx+8]
        jae     .b
        call    .elem_a         ; edx = element of a (float: address)
        mov     eax,ebp
        shr     eax,8
        jz      .add            ; union: all of a
        push    edx
        mov     eax,esi
        mov     ecx,ebp
        and     ecx,0xFF
        call    rt_list_find
        pop     edx
        mov     ecx,ebp
        shr     ecx,8
        cmp     ecx,1
        jne     .diff
        test    eax,eax         ; intersection: only those in b
        js      .nexta
        jmp     .add
.diff:  test    eax,eax         ; difference: only those not in b
        jns     .nexta
.add:   mov     eax,[esp]
        mov     ecx,ebp
        and     ecx,0xFF
        call    rt_set_add
.nexta: inc     edi
        jmp     .la
.b:     mov     eax,ebp         ; union: then all of b
        shr     eax,8
        jnz     .out
        xor     edi,edi
.lb:    test    esi,esi
        jz      .out
        cmp     edi,[esi+8]
        jae     .out
        mov     eax,[esi+16]
        mov     ecx,ebp
        and     ecx,0xFF
        cmp     ecx,2
        je      @f
        mov     edx,[eax+edi*4]
        jmp     .addb
@@:     lea     edx,[eax+edi*8]
.addb:  mov     eax,[esp]
        call    rt_set_add
        inc     edi
        jmp     .lb
.out:   pop     eax
        pop     ebp edi esi ebx
        ret
.elem_a:
        mov     eax,[ebx+16]
        mov     ecx,ebp
        and     ecx,0xFF
        cmp     ecx,2
        je      @f
        mov     edx,[eax+edi*4]
        ret
@@:     lea     edx,[eax+edi*8]
        ret

;;; code rt_set_eq : rt_list_find
rt_set_eq:                      ; eax = a, edx = b, ecx = kind -> eax = bool (same elements)
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        mov     ebp,ecx
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
.loop:  cmp     edi,edx
        jae     .yes
        push    edx
        mov     eax,[ebx+16]
        cmp     ebp,2
        je      @f
        mov     edx,[eax+edi*4]
        jmp     .find
@@:     lea     edx,[eax+edi*8]
.find:  mov     eax,esi
        mov     ecx,ebp
        call    rt_list_find
        pop     edx
        test    eax,eax
        js      .no
        inc     edi
        jmp     .loop
.yes:   mov     eax,1
        jmp     .out
.no:    xor     eax,eax
.out:   pop     ebp edi esi ebx
        ret

; ---------------------------------------------------------------- dicts (str keys, insertion ordered)

;;; code rt_dict_new : rt_alloc
; dict: +8 length, +12 capacity, +16 keys, +20 values, +24 key kind (0 int,
; 1 str, 3 object, 4 tuple; kinds 1, 3, 4 are references)
rt_dict_new:                    ; eax = destroy routine, edx = key kind -> eax = new empty dict
        push    edx
        push    eax
        mov     eax,28
        call    rt_alloc
        pop     ecx
        pop     edx
        mov     dword [eax],1
        mov     [eax+4],ecx
        mov     [eax+24],edx
        ret

;;; code rt_dict_destroy : rt_free rt_decref
rt_dict_destroy:                ; destroy routine of dicts of plain values
        xor     edx,edx
        jmp     rt_dict_drop
rt_dict_drop:                   ; eax = dict, edx = 1 to drop the values too
        push    ebx esi
        mov     ebx,eax
        mov     esi,[ebx+8]
.l:     dec     esi
        js      .done
        cmp     dword [ebx+24],1
        jb      @f
        mov     eax,[ebx+16]
        mov     eax,[eax+esi*4]
        push    edx
        call    rt_decref
        pop     edx
@@:     test    edx,edx
        jz      .l
        mov     eax,[ebx+20]
        mov     eax,[eax+esi*4]
        push    edx
        call    rt_decref
        pop     edx
        jmp     .l
.done:  mov     eax,[ebx+16]
        call    rt_free
        mov     eax,[ebx+20]
        call    rt_free
        mov     eax,ebx
        pop     esi ebx
        jmp     rt_free

;;; code rt_dict_destroy_ptr : rt_dict_destroy
rt_dict_destroy_ptr:            ; ... of references
        mov     edx,1
        jmp     rt_dict_drop

;;; code rt_dict_eq : rt_dict_find rt_str_eq
rt_dict_eq:                     ; eax = a, edx = b, ecx = value kind -> eax = 1 if the same keys map to equal values
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        mov     ebp,ecx
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
        mov     eax,[ebx+16]
        mov     edx,[eax+edi*4]
        mov     eax,esi
        call    rt_dict_find
        cmp     eax,-1
        je      .no
        mov     ecx,[esi+20]
        mov     edx,[ebx+20]
        cmp     ebp,2
        je      .f
        mov     ecx,[ecx+eax*4]
        mov     edx,[edx+edi*4]
        cmp     ebp,1
        je      .s
if defined rt_tuple_eq
        cmp     ebp,4
        je      .t
end if
        cmp     ecx,edx
        jne     .no
        jmp     .next
.s:     mov     eax,ecx
        call    rt_str_eq
        test    eax,eax
        jz      .no
        jmp     .next
if defined rt_tuple_eq
.t:     mov     eax,ecx
        call    rt_tuple_eq
        test    eax,eax
        jz      .no
        jmp     .next
end if
.f:     fld     qword [ecx+eax*8]
        fcomp   qword [edx+edi*8]
        fnstsw  ax
        sahf
        jne     .no
.next:  inc     edi
        jmp     .l
.yes:   mov     eax,1
        pop     ebp edi esi ebx
        ret
.no:    xor     eax,eax
        pop     ebp edi esi ebx
        ret

;;; code rt_dict_find : rt_str_eq
rt_dict_find:                   ; eax = dict, edx = key -> eax = index or -1
        push    ebx esi edi
        mov     ebx,eax
        mov     edi,edx
        xor     esi,esi
        test    ebx,ebx
        jz      .nf
.loop:  cmp     esi,[ebx+8]
        jae     .nf
        mov     eax,[ebx+16]
        mov     eax,[eax+esi*4]
        mov     edx,edi
        cmp     dword [ebx+24],1
        je      .str
if defined rt_tuple_eq
        cmp     dword [ebx+24],4
        jne     .ident
        call    rt_tuple_eq
        jmp     .test
end if
.ident: cmp     eax,edi         ; ints, objects
        je      .found
        inc     esi
        jmp     .loop
.str:   call    rt_str_eq
.test:  test    eax,eax
        jnz     .found
        inc     esi
        jmp     .loop
.found: mov     eax,esi
        pop     edi esi ebx
        ret
.nf:    or      eax,-1
        pop     edi esi ebx
        ret

;;; code rt_dict_reserve : rt_alloc rt_free
rt_dict_reserve:                ; eax = dict, edx = value size: room for one more entry
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,edx
        mov     eax,[ebx+8]
        cmp     eax,[ebx+12]
        jb      .ok
        add     eax,eax
        cmp     eax,4
        jae     @f
        mov     eax,4
@@:     mov     [ebx+12],eax    ; new capacity
        shl     eax,2
        call    rt_alloc        ; keys
        push    eax
        mov     edi,eax
        mov     esi,[ebx+16]
        mov     ecx,[ebx+8]
        rep     movsd
        mov     eax,[ebx+16]
        call    rt_free
        pop     dword [ebx+16]
        mov     eax,[ebx+12]
        imul    eax,ebp
        call    rt_alloc        ; values
        push    eax
        mov     edi,eax
        mov     esi,[ebx+20]
        mov     ecx,[ebx+8]
        imul    ecx,ebp
        rep     movsb
        mov     eax,[ebx+20]
        call    rt_free
        pop     dword [ebx+20]
.ok:    pop     ebp edi esi ebx
        ret

;;; code rt_dict_slot : rt_dict_find rt_dict_reserve rt_incref rt_panic_none
rt_dict_slot:                   ; eax = dict, edx = key, ecx = value size -> eax = address of the value (added if missing)
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        mov     edi,ecx
        call    rt_dict_find
        test    eax,eax
        jns     .have
        mov     eax,ebx
        mov     edx,edi
        call    rt_dict_reserve
        mov     eax,[ebx+8]
        inc     dword [ebx+8]
        mov     ecx,[ebx+16]
        mov     [ecx+eax*4],esi
        cmp     dword [ebx+24],1
        jb      @f
        push    eax
        mov     eax,esi
        call    rt_incref
        pop     eax
@@:
        mov     ecx,eax
        imul    ecx,edi
        add     ecx,[ebx+20]
        mov     dword [ecx],0
        mov     dword [ecx+edi-4],0
.have:  imul    eax,edi
        add     eax,[ebx+20]
        pop     edi esi ebx
        ret

;;; code rt_dict_get : rt_dict_find rt_panic_key_of rt_panic_key
rt_dict_get:                    ; eax = dict, edx = key, ecx = value size -> eax = address of the value
        push    ebx
        mov     ebx,eax
        push    ecx
        push    edx
        call    rt_dict_find
        pop     edx
        pop     ecx
        test    eax,eax
        jns     @f
        mov     eax,edx
        cmp     dword [ebx+24],1
        je      rt_panic_key_of
        jmp     rt_panic_key
@@:
        imul    eax,ecx
        add     eax,[ebx+20]
        pop     ebx
        ret

;;; code rt_dict_del : rt_dict_find rt_panic_key_of rt_panic_key rt_decref rt_scratch
rt_dict_del:                    ; eax = dict, edx = key, ecx = value size -> eax = rt_scratch holding the removed value
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,ecx
        push    edx
        call    rt_dict_find
        pop     edx
        test    eax,eax
        jns     @f
        mov     eax,edx
        cmp     dword [ebx+24],1
        je      rt_panic_key_of
        jmp     rt_panic_key
@@:
        mov     edx,eax
        mov     esi,[ebx+20]    ; save the value
        imul    eax,ebp
        add     esi,eax
        mov     eax,[esi]
        mov     [rt_scratch],eax
        mov     eax,[esi+ebp-4]
        mov     [rt_scratch+ebp-4],eax
        cmp     dword [ebx+24],1
        jb      @f
        mov     eax,[ebx+16]    ; drop the key
        mov     eax,[eax+edx*4]
        push    edx
        call    rt_decref
        pop     edx
@@:
        mov     ecx,[ebx+8]     ; close the gaps
        dec     ecx
        mov     [ebx+8],ecx
        sub     ecx,edx
        push    ecx
        mov     edi,[ebx+16]
        lea     edi,[edi+edx*4]
        lea     esi,[edi+4]
        rep     movsd
        pop     ecx
        imul    ecx,ebp
        mov     edi,[ebx+20]
        imul    edx,ebp
        add     edi,edx
        lea     esi,[edi+ebp]
        rep     movsb
        mov     eax,rt_scratch
        pop     ebp edi esi ebx
        ret

;;; code rt_dict_keys : rt_list_new rt_list_push rt_list_destroy rt_list_destroy_ptr rt_incref
rt_dict_keys:                   ; eax = dict -> eax = list of its keys
        push    ebx esi
        mov     ebx,eax
        mov     eax,rt_list_destroy_ptr
        test    ebx,ebx
        jz      @f
        cmp     dword [ebx+24],1
        jae     @f
        mov     eax,rt_list_destroy
@@:     call    rt_list_new
        push    eax
        xor     esi,esi
.l:     test    ebx,ebx
        jz      .out
        cmp     esi,[ebx+8]
        jae     .out
        mov     eax,[esp]
        mov     edx,4
        call    rt_list_push
        mov     ecx,[ebx+16]
        mov     ecx,[ecx+esi*4]
        mov     [eax],ecx
        cmp     dword [ebx+24],1
        jb      @f
        mov     eax,ecx
        call    rt_incref
@@:     inc     esi
        jmp     .l
.out:   pop     eax
        pop     esi ebx
        ret

;;; code rt_dict_values : rt_list_new rt_list_push rt_list_destroy rt_list_destroy_ptr rt_incref
rt_dict_values:                 ; eax = dict, edx = kind -> eax = list of its values
        push    ebx esi edi
        mov     ebx,eax
        mov     edi,edx
        mov     eax,rt_list_destroy
        cmp     edi,1
        je      @f
        cmp     edi,3
        jb      .new
@@:     mov     eax,rt_list_destroy_ptr
.new:   call    rt_list_new
        push    eax
        xor     esi,esi
.l:     test    ebx,ebx
        jz      .out
        cmp     esi,[ebx+8]
        jae     .out
        mov     eax,[esp]
        mov     edx,4
        cmp     edi,2
        jne     @f
        mov     edx,8
@@:     call    rt_list_push
        mov     ecx,[ebx+20]
        cmp     edi,2
        je      .f
        mov     ecx,[ecx+esi*4]
        mov     [eax],ecx
        cmp     edi,1
        jb      .next
        mov     eax,ecx
        call    rt_incref
        jmp     .next
.f:     mov     edx,[ecx+esi*8]
        mov     [eax],edx
        mov     edx,[ecx+esi*8+4]
        mov     [eax+4],edx
.next:  inc     esi
        jmp     .l
.out:   pop     eax
        pop     edi esi ebx
        ret

;;; code rt_dict_clear : rt_decref
rt_dict_clear:                  ; eax = dict, edx = kind
        push    ebx
        mov     ebx,eax
        test    ebx,ebx
        jz      .out
.l:     mov     eax,[ebx+8]
        test    eax,eax
        jz      .out
        dec     eax
        mov     [ebx+8],eax
        push    eax edx
        cmp     dword [ebx+24],1
        jb      @f
        mov     ecx,[ebx+16]
        mov     eax,[ecx+eax*4]
        call    rt_decref
@@:     pop     edx eax
        cmp     edx,1
        je      @f
        cmp     edx,3
        jb      .l
@@:     mov     ecx,[ebx+20]
        mov     eax,[ecx+eax*4]
        push    edx
        call    rt_decref
        pop     edx
        jmp     .l
.out:   pop     ebx
        ret

;;; code rt_dict_update : rt_dict_slot rt_incref rt_decref
rt_dict_update:                 ; eax = destination, edx = source, ecx = kind: copy every entry
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        mov     ebp,ecx
        xor     edi,edi
.l:     test    esi,esi
        jz      .out
        cmp     edi,[esi+8]
        jae     .out
        mov     eax,ebx
        mov     edx,[esi+16]
        mov     edx,[edx+edi*4]
        mov     ecx,4
        cmp     ebp,2
        jne     @f
        mov     ecx,8
@@:     call    rt_dict_slot
        mov     ecx,[esi+20]
        cmp     ebp,2
        je      .f
        mov     ecx,[ecx+edi*4]
        cmp     ebp,1
        jb      .plain
        push    eax ecx
        mov     eax,ecx
        call    rt_incref
        pop     ecx eax
        push    dword [eax]
        mov     [eax],ecx
        pop     eax
        call    rt_decref
        jmp     .next
.plain: mov     [eax],ecx
        jmp     .next
.f:     mov     edx,[ecx+edi*8]
        mov     [eax],edx
        mov     edx,[ecx+edi*8+4]
        mov     [eax+4],edx
.next:  inc     edi
        jmp     .l
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_dict_copy : rt_dict_new rt_dict_update
rt_dict_copy:                   ; eax = dict, edx = kind -> eax = new dict
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,rt_dict_destroy
        mov     edx,1
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+4]
        mov     edx,[ebx+24]
@@:     call    rt_dict_new
        push    eax
        mov     edx,ebx
        mov     ecx,esi
        call    rt_dict_update
        pop     eax
        pop     esi ebx
        ret

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

;;; code rt_syscall_list : rt_list_new rt_list_push rt_list_destroy
rt_syscall_list:                ; eax = address of 6 dwords (eax..edi after the call) -> eax = list[int]
        push    ebx esi
        mov     esi,eax
        mov     eax,rt_list_destroy
        call    rt_list_new
        mov     ebx,eax
        mov     ecx,6
@@:     push    ecx
        mov     eax,ebx
        mov     edx,4
        call    rt_list_push
        mov     ecx,[esi]
        mov     [eax],ecx
        add     esi,4
        pop     ecx
        dec     ecx
        jnz     @b
        mov     eax,ebx
        pop     esi ebx
        ret

;;; code rt_unpack_check : rt_panic_value
rt_unpack_check:                ; eax = list, edx = expected length
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
@@:     cmp     ecx,edx
        jne     @f
        ret
@@:     mov     esi,rt_msg_unpack
        jmp     rt_panic_value
;;; data rt_unpack_check
rt_msg_unpack   db 'wrong number of values to unpack',0

;;; code rt_str_index : rt_str_find rt_panic_value
rt_str_index:                   ; eax = s, edx = sub -> eax = first index (a missing substring is an error)
        call    rt_str_find
        cmp     eax,-1
        je      @f
        ret
@@:     mov     esi,rt_msg_substr
        jmp     rt_panic_value
;;; data rt_str_index
rt_msg_substr   db 'substring not found',0

;;; code rt_str_chars : rt_list_new rt_list_push rt_list_destroy_ptr rt_chars
rt_str_chars:                   ; eax = s -> eax = list[str] of its characters
        push    ebx esi edi
        mov     esi,eax
        mov     eax,rt_list_destroy_ptr
        call    rt_list_new
        mov     ebx,eax
        xor     edi,edi
        test    esi,esi
        jz      .out
.next:  cmp     edi,[esi+8]
        jae     .out
        mov     eax,ebx
        mov     edx,4
        call    rt_list_push
        movzx   ecx,byte [esi+12+edi]
        shl     ecx,4
        add     ecx,rt_chars
        mov     [eax],ecx
        inc     edi
        jmp     .next
.out:   mov     eax,ebx
        pop     edi esi ebx
        ret

;;; code rt_range_list : rt_list_new rt_list_push rt_list_destroy rt_panic_value
rt_range_list:                  ; eax = start, edx = stop, ecx = step -> eax = list(range(start, stop, step))
        push    ebx esi edi
        mov     esi,eax
        mov     edi,edx
        test    ecx,ecx
        jnz     @f
        mov     esi,rt_msg_rstep
        jmp     rt_panic_value
@@:     push    ecx
        mov     eax,rt_list_destroy
        call    rt_list_new
        mov     ebx,eax
.next:  cmp     dword [esp],0
        jl      .down
        cmp     esi,edi
        jge     .out
        jmp     .put
.down:  cmp     esi,edi
        jle     .out
.put:   mov     eax,ebx
        mov     edx,4
        call    rt_list_push
        mov     [eax],esi
        add     esi,[esp]
        jmp     .next
.out:   pop     ecx
        mov     eax,ebx
        pop     edi esi ebx
        ret
;;; data rt_range_list
rt_msg_rstep     db 'range() arg 3 must not be zero',0

;;; code rt_set_from : rt_list_new rt_list_destroy rt_list_destroy_ptr rt_set_add
rt_set_from:                    ; eax = list, edx = kind -> eax = new set of its elements
        push    ebx esi edi
        mov     esi,eax
        mov     edi,edx
        mov     eax,rt_list_destroy
        cmp     edi,1
        jb      @f
        cmp     edi,2
        je      @f
        mov     eax,rt_list_destroy_ptr
@@:     call    rt_list_new
        mov     ebx,eax
        test    esi,esi
        jz      .out
        push    0
.next:  mov     ecx,[esp]
        cmp     ecx,[esi+8]
        jae     .done
        mov     edx,[esi+16]
        cmp     edi,2
        je      .flt
        mov     edx,[edx+ecx*4]
        jmp     .add
.flt:   lea     edx,[edx+ecx*8]
.add:   mov     eax,ebx
        mov     ecx,edi
        call    rt_set_add
        inc     dword [esp]
        jmp     .next
.done:  pop     ecx
.out:   mov     eax,ebx
        pop     edi esi ebx
        ret

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

;;; code rt_file_readlines : rt_file_readline rt_list_new rt_list_push rt_list_destroy_ptr rt_decref
rt_file_readlines:              ; eax = file -> eax = list of the remaining lines
        push    ebx esi
        mov     ebx,eax
        mov     eax,rt_list_destroy_ptr
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
        mov     edx,4
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        jmp     .next
.out:   mov     eax,esi
        pop     esi ebx
        ret

; ---------------------------------------------------------------- tuples
; Tuple: +0 refcount, +4 destroy, +8 descriptor, +12 items (4 bytes each,
; floats 8). Descriptor (static data): dd count, then one element kind per
; item (0 int/bool, 1 str, 2 float, 3 object, 4 tuple).

;;; code rt_tuple_new : rt_alloc rt_tuple_free
rt_tuple_new:                   ; eax = descriptor, edx = bytes of the items -> eax = new tuple (items zero)
        push    eax
        lea     eax,[edx+12]
        call    rt_alloc
        pop     edx
        mov     dword [eax],1
        mov     dword [eax+4],rt_tuple_free
        mov     [eax+8],edx
        ret

;;; code rt_tuple_free : rt_decref rt_free
rt_tuple_free:                  ; destroy routine of tuples: drop the reference items
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,[ebx+8]
        mov     edi,[esi]
        add     esi,4
        lea     ebp,[ebx+12]
.l:     test    edi,edi
        jz      .done
        mov     eax,[esi]
        cmp     eax,2
        je      .f
        cmp     eax,1
        je      .ref
        cmp     eax,3
        jb      .n
.ref:   mov     eax,[ebp]
        call    rt_decref
.n:     add     ebp,4
        jmp     .next
.f:     add     ebp,8
.next:  add     esi,4
        dec     edi
        jmp     .l
.done:  mov     eax,ebx
        pop     ebp edi esi ebx
        jmp     rt_free

;;; code rt_tuple_cmp : rt_str_cmp
rt_tuple_cmp:                   ; eax = a, edx = b (same tuple type) -> eax = -1, 0, 1 (item by item)
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,edx
        cmp     ebx,ebp
        je      .eq
        test    ebx,ebx
        jz      .lt
        test    ebp,ebp
        jz      .gt
        mov     esi,[ebx+8]
        mov     ecx,[esi]
        add     esi,4
        mov     edi,12
.l:     test    ecx,ecx
        jz      .eq
        push    ecx
        mov     eax,[esi]
        cmp     eax,2
        je      .f
        mov     ecx,eax
        mov     eax,[ebx+edi]
        mov     edx,[ebp+edi]
        add     edi,4
        cmp     ecx,1
        je      .s
        cmp     ecx,4
        je      .t
        cmp     eax,edx         ; ints (objects: by address)
        jl      .lt1
        jg      .gt1
        jmp     .n
.s:     call    rt_str_cmp
        jmp     .r
.t:     call    rt_tuple_cmp
.r:     test    eax,eax
        js      .lt1
        jg      .gt1
        jmp     .n
.f:     fld     qword [ebx+edi]
        fcomp   qword [ebp+edi]
        add     edi,8
        fnstsw  ax
        sahf
        jb      .lt1
        ja      .gt1
.n:     pop     ecx
        add     esi,4
        dec     ecx
        jmp     .l
.lt1:   pop     ecx
.lt:    or      eax,-1
        jmp     .out
.gt1:   pop     ecx
.gt:    mov     eax,1
        jmp     .out
.eq:    xor     eax,eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_tuple_at : rt_panic_index
rt_tuple_at:                    ; eax = tuple (one item type), edx = index, ecx = item size -> eax = address of the item
        test    eax,eax
        jz      rt_panic_index_t
        push    ebx
        mov     ebx,[eax+8]
        mov     ebx,[ebx]       ; count
        test    edx,edx
        jns     @f
        add     edx,ebx
@@:     cmp     edx,ebx
        pop     ebx
        jae     rt_panic_index_t
        imul    edx,ecx
        lea     eax,[eax+12+edx]
        ret

;;; code rt_tuple_eq : rt_tuple_cmp
rt_tuple_eq:                    ; eax = a, edx = b -> eax = bool
        call    rt_tuple_cmp
        test    eax,eax
        sete    al
        movzx   eax,al
        ret

;;; code rt_dict_items : rt_list_new rt_list_push rt_list_destroy_ptr rt_tuple_new rt_incref
rt_dict_items:                  ; eax = dict, edx = descriptor of tuple[K, V] -> eax = list of (key, value)
        push    ebx esi edi ebp
        mov     esi,eax
        mov     ebp,edx
        mov     eax,rt_list_destroy_ptr
        call    rt_list_new
        mov     ebx,eax
        xor     edi,edi
        test    esi,esi
        jz      .out
.l:     cmp     edi,[esi+8]
        jae     .out
        mov     eax,ebp
        mov     edx,12
        call    rt_tuple_new
        push    eax
        mov     ecx,[esi+16]
        mov     ecx,[ecx+edi*4]
        mov     [eax+12],ecx
        cmp     dword [ebp+4],1
        jb      @f
        mov     eax,ecx
        call    rt_incref
@@:     mov     eax,[esp]
        mov     ecx,[ebp+8]     ; the value's kind
        mov     edx,[esi+20]
        cmp     ecx,2
        je      .f
        mov     edx,[edx+edi*4]
        mov     [eax+16],edx
        cmp     ecx,1
        je      .ref
        cmp     ecx,3
        jb      .push
.ref:   mov     eax,edx
        call    rt_incref
        jmp     .push
.f:     mov     ecx,[edx+edi*8]
        mov     [eax+16],ecx
        mov     ecx,[edx+edi*8+4]
        mov     [eax+20],ecx
.push:  mov     eax,ebx
        mov     edx,4
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        inc     edi
        jmp     .l
.out:   mov     eax,ebx
        pop     ebp edi esi ebx
        ret

; ---------------------------------------------------------------- asyncio
; Stackful tasks: every task runs on its own stack; `await coroutine()` is an
; ordinary call on the current task's stack, and a task gives the CPU back to
; the scheduler (rt_async_run, on the program's stack) when it sleeps or waits.
; Task object: +0 refcount, +4 destroy, +8 state (0 ready, 1 running,
; 2 sleeping, 3 waiting, 4 done), +12 saved esp, +16 stack memory, +20 next in
; the run queue / sleeper list, +24 result (8 bytes), +32 wake time (ms),
; +36 tasks waiting for this one, +40 next waiter, +44 result is a reference.

;;; code rt_task_new : rt_alloc rt_os_alloc rt_task_free rt_task_ready
rt_task_new:                    ; eax = argument bytes, edx = entry -> eax = new ready task; arguments at [[eax+12]+20]
        push    ebx esi
        mov     esi,eax
        push    edx
        mov     eax,52
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
rt_sleepers     rd 1

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

;;; code rt_async_sleep : rt_task_yield rt_task_ready rt_now_ms
rt_async_sleep:                 ; st0 = seconds (popped): suspend the running task that long
        push    1000
        fimul   dword [esp]
        fistp   dword [esp]
        pop     eax
        test    eax,eax
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
; generator that ran before, +60 its outermost handler record (28 bytes).

;;; code rt_gen_new : rt_alloc rt_os_alloc rt_gen_free rt_gen_yield
rt_gen_new:                     ; eax = argument bytes, edx = descriptor -> eax = new generator; arguments at [eax+36]
        push    ebx esi
        mov     esi,eax
        push    edx
        mov     eax,88
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
        je      .done
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
        mov     eax,[ebx+8]
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
.nv:    mov     eax,[ebx+44]
        call    rt_decref
        mov     eax,[ebx+16]
        mov     edx,65536
        call    rt_os_free
        mov     eax,ebx
        pop     ebp edi esi ebx
        jmp     rt_free

;;; code rt_gen_drain : rt_gen_next rt_list_new rt_list_push rt_incref
rt_gen_drain:                   ; eax = generator, edx = the list's destroy routine -> eax = new list of the values left
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
        mov     eax,[ebx+40]
        test    dword [eax+4],2
        jnz     .f
        mov     eax,esi
        mov     edx,4
        call    rt_list_push
        mov     ecx,[ebx+24]
        mov     [eax],ecx
        cmp     dword [ebx+32],0
        je      .l
        mov     eax,ecx
        call    rt_incref
        jmp     .l
.f:     mov     eax,esi
        mov     edx,8
        call    rt_list_push
        mov     ecx,[ebx+24]
        mov     [eax],ecx
        mov     ecx,[ebx+28]
        mov     [eax+4],ecx
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
rt_time_sleep:                  ; st0 = seconds (popped): block
        push    1000
        fimul   dword [esp]
        fistp   dword [esp]
        pop     eax
        test    eax,eax
        jle     @f
        jmp     rt_idle_sleep
@@:     ret

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

;;; code rt_gcd
rt_gcd:                         ; eax, edx -> eax = gcd (non-negative)
        mov     ecx,edx
        cdq
        xor     eax,edx
        sub     eax,edx
        mov     edx,ecx
        sar     ecx,31
        xor     edx,ecx
        sub     edx,ecx
        mov     ecx,edx
@@:     jecxz   @f
        xor     edx,edx
        div     ecx
        mov     eax,ecx
        mov     ecx,edx
        jmp     @b
@@:     ret

;;; code rt_isqrt : rt_panic_value
rt_isqrt:                       ; eax -> eax = floor(sqrt(eax))
        test    eax,eax
        js      .neg
        push    eax
        fild    dword [esp]
        fsqrt
        fistp   dword [esp]     ; rounded; fix up below
        pop     ecx
        mov     edx,ecx
        imul    edx,ecx
        cmp     edx,eax
        jbe     @f
        dec     ecx
@@:     mov     eax,ecx
        ret
.neg:   mov     esi,rt_msg_isqrt
        jmp     rt_panic_value
;;; data rt_isqrt
rt_msg_isqrt    db 'isqrt() argument must be nonnegative',0

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

;;; code rt_rand_range : rt_rand rt_panic_value
rt_rand_range:                  ; eax = lo, edx = hi (exclusive) -> eax in [lo, hi)
        mov     ecx,edx
        sub     ecx,eax
        jle     .bad
        push    eax
        push    ecx
        call    rt_rand
        xor     edx,edx
        div     dword [esp]
        add     esp,4
        pop     eax
        add     eax,edx
        ret
.bad:   mov     esi,rt_msg_range
        jmp     rt_panic_value
;;; data rt_rand_range
rt_msg_range    db 'empty range for randrange()',0

;;; code rt_shuffle : rt_rand_range
rt_shuffle:                     ; eax = list, edx = element size (4 or 8): Fisher-Yates
        push    ebx esi edi ebp
        test    eax,eax
        jz      .out
        mov     ebx,eax
        mov     ebp,edx
        mov     esi,[ebx+8]
.next:  cmp     esi,1
        jbe     .out
        xor     eax,eax
        mov     edx,esi
        call    rt_rand_range   ; j in [0, i)
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

;;; code rt_floordiv : rt_panic_zero
rt_floordiv:                    ; eax = a, edx = b -> eax = a // b (rounds toward -infinity)
        test    edx,edx
        jz      rt_panic_zero
        mov     ecx,edx
        cdq
        idiv    ecx
        test    edx,edx
        jz      @f
        xor     edx,ecx
        jns     @f
        dec     eax
@@:     ret

;;; code rt_mod : rt_panic_zero
rt_mod:                         ; eax = a, edx = b -> eax = a % b (sign of b)
        test    edx,edx
        jz      rt_panic_zero
        mov     ecx,edx
        cdq
        idiv    ecx
        mov     eax,edx
        test    eax,eax
        jz      @f
        xor     edx,ecx
        jns     @f
        add     eax,ecx
@@:     ret

;;; code rt_ipow
rt_ipow:                        ; eax = base, edx = exponent (< 0 gives 0) -> eax = base ** exponent
        mov     ecx,edx
        mov     edx,eax
        mov     eax,1
        test    ecx,ecx
        js      .neg
@@:     test    ecx,ecx
        jz      .out
        test    ecx,1
        jz      .sq
        imul    eax,edx
.sq:    imul    edx,edx
        shr     ecx,1
        jmp     @b
.neg:   xor     eax,eax
.out:   ret

;;; code rt_ftoi
rt_ftoi:                        ; st0 (popped) -> eax = int(st0), truncated toward zero
        sub     esp,8
        fnstcw  [esp]
        mov     ax,[esp]
        or      ax,0x0C00
        mov     [esp+2],ax
        fldcw   [esp+2]
        fistp   dword [esp+4]
        fldcw   [esp]
        mov     eax,[esp+4]
        add     esp,8
        ret

;;; code rt_fround
rt_fround:                      ; st0 (popped) -> eax = round(st0), halves to even
        push    eax
        fistp   dword [esp]
        pop     eax
        ret

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
        jz      .bad1
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
.bad1:  fstp    st0
.bad:   mov     esi,rt_msg_float
        jmp     rt_panic_value
;;; data rt_float_parse
rt_msg_float    db 'could not convert string to float',0

; ---------------------------------------------------------------- input()

;;; code rt_input linux : rt_write rt_sb_char rt_sb_take
rt_input:                       ; eax = prompt (str or 0) -> eax = a line from stdin, without the newline
        push    ebx
        test    eax,eax
        jz      @f
        mov     edx,[eax+8]
        lea     ecx,[eax+12]
        call    rt_write
@@:     push    dword [rt_sb_len]
.next:  push    0
        mov     eax,3           ; read(0, &byte, 1)
        xor     ebx,ebx
        mov     ecx,esp
        mov     edx,1
        int     0x80
        pop     ecx
        cmp     eax,1
        jne     .end
        cmp     cl,10
        je      .end
        mov     al,cl
        call    rt_sb_char
        jmp     .next
.end:   pop     eax
        call    rt_sb_take
        pop     ebx
        ret

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
