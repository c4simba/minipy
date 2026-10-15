; ---------------------------------------------------------------- strings: characters, slices, repetition
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

