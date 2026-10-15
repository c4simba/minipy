; ---------------------------------------------------------------- 64-bit integers: text (formatting, parsing)
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

