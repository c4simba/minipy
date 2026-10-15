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

