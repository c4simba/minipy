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
