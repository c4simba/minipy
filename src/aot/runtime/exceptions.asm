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

