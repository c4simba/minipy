; ---------------------------------------------------------------- strings: searching
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

