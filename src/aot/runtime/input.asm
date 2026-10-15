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

