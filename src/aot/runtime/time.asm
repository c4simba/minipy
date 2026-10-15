; ---------------------------------------------------------------- time
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

