; ---------------------------------------------------------------- time
;;; code rt_now_ms linux : rt_sys_clock_gettime
rt_now_ms:                      ; -> eax = milliseconds of the monotonic clock (wraps; compare differences)
        push    ebx
        sub     esp,8
        mov     eax,265         ; clock_gettime(CLOCK_MONOTONIC)
        mov     ebx,1
        mov     ecx,esp
        call    rt_sys_clock_gettime
        mov     eax,[esp+4]
        xor     edx,edx
        mov     ecx,1000000
        div     ecx
        imul    ecx,[esp],1000
        add     eax,ecx
        add     esp,8
        pop     ebx
        ret

;;; code rt_sys_clock_gettime linux
rt_sys_clock_gettime:           ; as int 0x80 with eax = 265 (clock_gettime: ebx clock, ecx timespec): through the
        mov     eax,[rt_vdso_cgt]   ; vDSO when rt_vdso_init found it there - no trap into the kernel, as libc
        test    eax,eax
        jz      .trap
        push    ecx edx         ; (cdecl: eax, ecx and edx are the callee's)
        push    ecx ebx
        call    eax
        add     esp,8
        pop     edx ecx
        ret
.trap:  mov     eax,265
        int     0x80
        ret
;;; data rt_sys_clock_gettime linux
rt_vdso_cgt     dd 0            ; __vdso_clock_gettime, or 0: the system call

;;; code rt_vdso_init linux : rt_sys_clock_gettime
rt_vdso_init:                   ; eax = esp at the start (argc, argv..., 0, envp..., 0, the auxiliary vector):
        push    ebx esi edi ebp ; __vdso_clock_gettime of the vDSO the kernel maps (AT_SYSINFO_EHDR) -> rt_vdso_cgt
        mov     ecx,[eax]
        lea     esi,[eax+ecx*4+8]
.env:   lodsd                   ; (past the environment)
        test    eax,eax
        jnz     .env
.aux:   lodsd
        mov     edx,eax
        lodsd
        test    edx,edx         ; AT_NULL: no vDSO
        jz      .out
        cmp     edx,33          ; AT_SYSINFO_EHDR
        jne     .aux
        mov     ebx,eax         ; its ELF header
        cmp     dword [ebx],0x464C457F
        jne     .out
        mov     esi,[ebx+28]    ; program headers: PT_LOAD -> load bias (base + p_offset - p_vaddr), PT_DYNAMIC
        add     esi,ebx
        movzx   ecx,word [ebx+44]
        xor     edi,edi         ; edi = the dynamic section
        xor     ebp,ebp         ; ebp = the bias (+1 once found: a bias of 0 is possible)
.ph:    test    ecx,ecx
        jz      .phend
        mov     eax,[esi]
        cmp     eax,1
        jne     @f
        test    ebp,ebp
        jnz     .phnext
        mov     ebp,[esi+4]
        sub     ebp,[esi+8]
        add     ebp,ebx
        inc     ebp
        jmp     .phnext
@@:     cmp     eax,2
        jne     .phnext
        mov     edi,[esi+4]
        add     edi,ebx
.phnext:movzx   eax,word [ebx+42]
        add     esi,eax
        dec     ecx
        jmp     .ph
.phend: test    edi,edi
        jz      .out
        test    ebp,ebp
        jz      .out
        dec     ebp
        xor     esi,esi         ; DT_SYMTAB -> esi, DT_STRTAB -> ebx, DT_HASH -> ecx = its symbols
        xor     ecx,ecx
        xor     ebx,ebx
.dyn:   mov     eax,[edi]
        mov     edx,[edi+4]
        add     edi,8
        test    eax,eax
        jz      .dynend
        add     edx,ebp
        cmp     eax,6
        jne     @f
        mov     esi,edx
@@:     cmp     eax,5
        jne     @f
        mov     ebx,edx
@@:     cmp     eax,4
        jne     .dyn
        mov     ecx,[edx+4]
        jmp     .dyn
.dynend:test    esi,esi
        jz      .out
        test    ebx,ebx
        jz      .out
        test    ecx,ecx         ; (no DT_HASH: the symbols up to the strings)
        jnz     @f
        mov     ecx,ebx
        sub     ecx,esi
        jbe     .out
        shr     ecx,4
@@:     cmp     ecx,4096
        ja      .out
.sym:   test    ecx,ecx
        jz      .out
        mov     al,[esi+12]     ; a defined function?
        and     al,15
        cmp     al,2
        jne     .next
        cmp     word [esi+14],0
        je      .next
        mov     edi,[esi]
        add     edi,ebx
        push    esi ecx
        mov     esi,rt_vdso_name
        mov     ecx,21          ; "__vdso_clock_gettime" and its 0
        repe    cmpsb
        pop     ecx esi
        jne     .next
        mov     eax,[esi+4]
        add     eax,ebp
        mov     [rt_vdso_cgt],eax
        jmp     .out
.next:  add     esi,16
        dec     ecx
        jmp     .sym
.out:   pop     ebp edi esi ebx
        ret
;;; data rt_vdso_init linux
rt_vdso_name    db '__vdso_clock_gettime',0

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

