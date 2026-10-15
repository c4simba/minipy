; ---------------------------------------------------------------- bytes
; A bytes object is laid out as a str whose length in code points is its
; length in bytes (rt_bytes_cp makes it so before a str routine measures it).
; The str routines that look at characters (split and strip at whitespace,
; splitlines) work byte by byte with ASCII whitespace while rt_bmode is set.

;;; code rt_bytes_cp
rt_bytes_cp:                    ; eax = bytes: one code point per byte (eax kept, nothing else changes)
        push    ecx
        mov     ecx,[eax+8]
        mov     [eax+13+ecx],ecx
        pop     ecx
        ret

;;; code rt_bytes_new : rt_str_new rt_panic_value
rt_bytes_new:                   ; eax = n -> eax = n zero bytes (ValueError when negative)
        test    eax,eax
        js      .neg
        push    eax
        call    rt_str_new
        pop     ecx
        mov     [eax+13+ecx],ecx
        ret
.neg:   mov     esi,rt_msg_negcount
        jmp     rt_panic_value
;;; data rt_bytes_new
rt_msg_negcount db 'negative count',0

;;; code rt_bytes_from_list : rt_bytes_new rt_panic_value
rt_bytes_from_list:             ; eax = list[int] -> eax = bytes of its values (ValueError outside 0..255)
        push    ebx esi
        mov     ebx,eax
        mov     esi,[ebx+16]
        mov     ecx,[ebx+8]
        xor     edx,edx
.chk:   cmp     edx,ecx
        jae     .ok
        cmp     dword [esi+edx*8+4],0
        jne     .bad
        cmp     dword [esi+edx*8],255
        ja      .bad
        inc     edx
        jmp     .chk
.ok:    mov     eax,ecx
        call    rt_bytes_new
        mov     ecx,[ebx+8]
        xor     edx,edx
.cp:    cmp     edx,ecx
        jae     .out
        push    eax
        mov     eax,[esi+edx*8]
        mov     ebx,[esp]
        mov     [ebx+12+edx],al
        pop     eax
        inc     edx
        jmp     .cp
.out:   pop     esi ebx
        ret
.bad:   mov     esi,rt_msg_brange
        jmp     rt_panic_value
;;; data rt_bytes_from_list
rt_msg_brange   db 'bytes must be in range(0, 256)',0

;;; code rt_bytes_at : rt_panic_index
rt_bytes_at:                    ; eax = bytes, edx = index -> eax = the byte there
        mov     ecx,[eax+8]
        test    edx,edx
        jns     @f
        add     edx,ecx
@@:     cmp     edx,ecx
        jae     rt_panic_index_b
        movzx   eax,byte [eax+12+edx]
        ret

;;; code rt_bytes_byte : rt_chars rt_bytes1 rt_panic_value
rt_bytes_byte:                  ; edx:eax = int -> eax = a static one-byte bytes of it (ValueError outside 0..255)
        test    edx,edx
        jnz     .bad
        cmp     eax,255
        ja      .bad
        cmp     eax,0x80
        jae     @f
        shl     eax,5
        add     eax,rt_chars
        ret
@@:     sub     eax,0x80
        shl     eax,5
        add     eax,rt_bytes1
        ret
.bad:   mov     esi,rt_msg_byte
        jmp     rt_panic_value
;;; data rt_bytes_byte
rt_msg_byte     db 'byte must be in range(0, 256)',0

;;; data rt_bytes1 : rt_static
align 4
rt_bytes1:                      ; the one-byte bytes 0x80..0xFF, static (32 bytes apart)
repeat 128
        dd      0x40000000, rt_static, 1
        db      %+127, 0
        dd      1
        db      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
end repeat

;;; code rt_bytes_has : rt_bytes_byte
rt_bytes_has:                   ; eax = bytes, edx:ecx = int -> eax = 1 when that byte is in it (ValueError outside 0..255)
        push    edi
        push    eax
        mov     eax,ecx
        call    rt_bytes_byte   ; the range check
        pop     edi
        mov     eax,ecx
        mov     ecx,[edi+8]
        add     edi,12
        jecxz   .no
        repne   scasb
        jne     .no
        mov     eax,1
        pop     edi
        ret
.no:    xor     eax,eax
        pop     edi
        ret

;;; code rt_sb_repr_bytes : rt_sb_char
rt_sb_repr_bytes:               ; eax = bytes: Python's repr - b'...' (b"..." if it has ' but no "), escapes
        push    ebx esi edi
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
        mov     ebx,39
        mov     ecx,esi
        xor     edx,edx
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
@@:     mov     al,'b'
        call    rt_sb_char
        mov     al,bl
        call    rt_sb_char
.next:  cmp     esi,edi
        jae     .done
        movzx   eax,byte [esi]
        inc     esi
        mov     ah,al
        cmp     al,'\'
        je      .esc
        cmp     al,bl
        je      .esc
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
        jae     .hex
        call    rt_sb_char
        jmp     .next
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

;;; code rt_repr_b : rt_sb_repr_bytes rt_repr_none
rt_repr_b:                      ; eax = &value (bytes or None): its repr appended
        mov     eax,[eax]
        test    eax,eax
        jz      rt_repr_none
        jmp     rt_sb_repr_bytes

;;; code rt_kd_bytes : rt_eq_s rt_cmp_s rt_hash_s rt_repr_b
;;; data rt_kd_bytes
align 4
rt_kd_bytes     dd 4, 1, rt_eq_s, rt_cmp_s, rt_hash_s, rt_repr_b, rt_kd_bytes_n
rt_kd_bytes_n   db 'bytes',0

;;; code rt_bytes_hex : rt_sb_char rt_sb_take
rt_bytes_hex:                   ; eax = bytes -> eax = str of two hex digits per byte
        push    esi edi
        push    dword [rt_sb_len]
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
.l:     cmp     esi,edi
        jae     .done
        movzx   eax,byte [esi]
        inc     esi
        push    eax
        shr     al,4
        call    .digit
        pop     eax
        and     al,15
        call    .digit
        jmp     .l
.digit: add     al,'0'
        cmp     al,'9'
        jbe     @f
        add     al,'a'-'0'-10
@@:     jmp     rt_sb_char
.done:  pop     eax
        call    rt_sb_take
        pop     edi esi
        ret

;;; code rt_bytes_hex_sep : rt_bytes_hex rt_sb_char rt_sb_take rt_panic_value
rt_bytes_hex_sep:               ; eax = bytes, edx = sep (a str or bytes: one ASCII character), ecx = bytes_per_sep
        push    ebx esi edi ebp ;   -> eax = str: bytes.hex(sep, bytes_per_sep)
        push    eax
        mov     esi,edx
        mov     ebx,[esi+8]
        xor     edi,edi
.asc:   cmp     edi,ebx
        jae     .one
        cmp     byte [esi+edi+12],0x80
        jae     .ascii
        inc     edi
        jmp     .asc
.one:   cmp     ebx,1
        jne     .len
        movzx   ebx,byte [esi+12]
        mov     ebp,ecx         ; ebp = bytes in a group
        test    ebp,ebp
        jns     @f
        neg     ebp
@@:     pop     eax
        test    ebp,ebp
        jz      .plain
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
        test    ecx,ecx
        js      .left
        mov     eax,[eax+8]     ; groups counted from the right: the first has n % k bytes (k when 0)
        xor     edx,edx
        div     ebp
        test    edx,edx
        jnz     .start
.left:  mov     edx,ebp
.start: push    dword [rt_sb_len]
        push    edx             ; [esp] = bytes until the next separator
.l:     cmp     esi,edi
        jae     .done
        cmp     dword [esp],0
        jne     .byte
        mov     [esp],ebp
        mov     eax,ebx
        call    rt_sb_char
.byte:  dec     dword [esp]
        movzx   eax,byte [esi]
        inc     esi
        push    eax
        shr     al,4
        call    .digit
        pop     eax
        and     al,15
        call    .digit
        jmp     .l
.digit: add     al,'0'
        cmp     al,'9'
        jbe     @f
        add     al,'a'-'0'-10
@@:     jmp     rt_sb_char
.done:  add     esp,4
        pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret
.plain: pop     ebp edi esi ebx
        jmp     rt_bytes_hex
.ascii: mov     eax,rt_msg_hexasc
        jmp     .fail
.len:   mov     eax,rt_msg_hexlen
.fail:  add     esp,4
        pop     ebp edi esi ebx
        mov     esi,eax
        jmp     rt_panic_value
;;; data rt_bytes_hex_sep
rt_msg_hexasc   db 'sep must be ASCII.',0
rt_msg_hexlen   db 'sep must be length 1.',0

;;; code rt_bytes_fromhex : rt_sb_char rt_sb_take rt_sb_need rt_sb_cstr rt_sb_int rt_raise_valerr rt_str_cpidx rt_bytes_cp
rt_bytes_fromhex:               ; eax = str -> eax = bytes of its pairs of hex digits (whitespace between them)
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        mov     ebp,eax
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
.l:     cmp     esi,edi
        jae     .done
        movzx   eax,byte [esi]
        cmp     al,' '
        je      .skip
        cmp     al,9
        jb      @f
        cmp     al,13
        jbe     .skip
@@:     call    .hexd
        test    eax,eax
        js      .nonhex
        mov     ebx,eax
        inc     esi
        cmp     esi,edi
        jae     .odd
        movzx   eax,byte [esi]
        call    .hexd
        test    eax,eax
        jns     @f
        movzx   eax,byte [esi]
        cmp     al,' '          ; a lone digit before whitespace (or the end)
        je      .odd
        test    al,al
        jz      .odd
        cmp     al,9
        jb      .nonhex
        cmp     al,13
        jbe     .odd
        jmp     .nonhex
@@:     shl     ebx,4
        or      eax,ebx
        call    rt_sb_char
.skip:  inc     esi
        jmp     .l
.done:  pop     eax
        call    rt_sb_take
        call    rt_bytes_cp
        pop     ebp edi esi ebx
        ret
.hexd:  cmp     al,'0'          ; eax = a byte -> its value as a hex digit, or -1
        jb      .nd
        cmp     al,'9'
        jbe     .d09
        or      al,32
        cmp     al,'a'
        jb      .nd
        cmp     al,'f'
        ja      .nd
        sub     al,'a'-10
        ret
.d09:   sub     al,'0'
        ret
.nd:    or      eax,-1
        ret
.odd:   mov     eax,[esp]
        mov     [rt_sb_len],eax
        push    dword [rt_sb_len]
        mov     eax,rt_msg_fromhex_odd
        call    rt_sb_cstr
        jmp     .raise
.nonhex:mov     eax,[esp]
        mov     [rt_sb_len],eax
        mov     edx,esi
        lea     eax,[ebp+12]
        sub     edx,eax
        mov     eax,ebp
        call    rt_str_cpidx
        push    eax
        push    dword [rt_sb_len]
        mov     eax,rt_msg_fromhex_bad
        call    rt_sb_cstr
        mov     eax,[esp+4]
        xor     edx,edx
        call    rt_sb_int
        pop     eax
        pop     ecx
        push    eax
.raise: pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_bytes_fromhex
rt_msg_fromhex_odd db 'fromhex() arg must contain an even number of hexadecimal digits',0
rt_msg_fromhex_bad db 'non-hexadecimal number found in fromhex() arg at position ',0

;;; code rt_bytes_case : rt_str_new
rt_bytes_case:                  ; eax = bytes, edx = 0 upper / 1 lower / 2 swapcase / 3 title / 4 capitalize -> eax = new bytes (ASCII letters only)
        push    ebx esi edi ebp
        mov     esi,eax
        mov     ebx,edx
        mov     eax,[esi+8]
        call    rt_str_new
        push    eax
        mov     ecx,[esi+8]
        mov     [eax+13+ecx],ecx
        lea     edi,[eax+12]
        add     esi,12
        xor     ebp,ebp         ; the previous byte was a letter (title) / past the first (capitalize)
        jecxz   .done
.l:     mov     al,[esi]
        mov     dl,al
        or      dl,32
        sub     dl,'a'
        cmp     dl,25
        ja      .other
        mov     dl,al
        and     dl,32           ; 32: lower case
        cmp     ebx,0
        je      .up
        cmp     ebx,1
        je      .low
        cmp     ebx,2
        jne     @f
        xor     al,32
        jmp     .put
@@:     cmp     ebx,3
        jne     .cap
        test    ebp,ebp
        mov     ebp,1
        jnz     .low
        jmp     .up
.cap:   test    ebp,ebp
        mov     ebp,1
        jnz     .low
.up:    and     al,0xDF
        jmp     .put
.low:   or      al,32
        jmp     .put
.other: cmp     ebx,3
        jne     @f
        xor     ebp,ebp
        jmp     .put
@@:     cmp     ebx,4
        jne     .put
        mov     ebp,1
.put:   mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     .l
.done:  pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_bytes_is
rt_bytes_is:                    ; eax = bytes, edx = 0 isalpha / 1 isdigit / 2 isalnum / 3 isspace / 4 islower / 5 isupper / 6 istitle / 7 isascii -> eax = bool
        push    ebx esi edi ebp
        mov     ebx,edx
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        test    edi,edi
        jz      .empty
        add     edi,esi
        xor     ebp,ebp         ; bit 0: a cased one, bit 1: the previous one cased
.l:     cmp     esi,edi
        jae     .end
        movzx   eax,byte [esi]
        inc     esi
        xor     ecx,ecx         ; ecx: 1 upper, 2 lower, 4 digit, 8 space, 16 not ASCII
        cmp     al,'A'
        jb      @f
        cmp     al,'Z'
        ja      @f
        mov     ecx,1
@@:     cmp     al,'a'
        jb      @f
        cmp     al,'z'
        ja      @f
        mov     ecx,2
@@:     cmp     al,'0'
        jb      @f
        cmp     al,'9'
        ja      @f
        mov     ecx,4
@@:     cmp     al,' '
        je      .sp
        cmp     al,9
        jb      @f
        cmp     al,13
        ja      @f
.sp:    mov     ecx,8
@@:     cmp     al,0x80
        jb      @f
        mov     ecx,16
@@:     cmp     ebx,0
        jne     @f
        test    ecx,3
        jz      .no
        jmp     .l
@@:     cmp     ebx,1
        jne     @f
        test    ecx,4
        jz      .no
        jmp     .l
@@:     cmp     ebx,2
        jne     @f
        test    ecx,7
        jz      .no
        jmp     .l
@@:     cmp     ebx,3
        jne     @f
        test    ecx,8
        jz      .no
        jmp     .l
@@:     cmp     ebx,4
        jne     @f
        test    ecx,1
        jnz     .no
        test    ecx,2
        jz      .l
        or      ebp,1
        jmp     .l
@@:     cmp     ebx,5
        jne     @f
        test    ecx,2
        jnz     .no
        test    ecx,1
        jz      .l
        or      ebp,1
        jmp     .l
@@:     cmp     ebx,6
        jne     .asc
        test    ecx,1
        jz      @f
        test    ebp,2
        jnz     .no
        or      ebp,3
        jmp     .l
@@:     test    ecx,2
        jz      @f
        test    ebp,2
        jz      .no
        or      ebp,3
        jmp     .l
@@:     and     ebp,-3
        jmp     .l
.asc:   test    ecx,16
        jnz     .no
        jmp     .l
.end:   cmp     ebx,4
        jb      .yes
        cmp     ebx,6
        ja      .yes
        mov     eax,ebp
        and     eax,1
        jmp     .out
.empty: cmp     ebx,7
        je      .yes
.no:    xor     eax,eax
        jmp     .out
.yes:   mov     eax,1
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_bytes_split : rt_str_split rt_u8_next rt_panic_value rt_msg_sep
rt_bytes_split:                 ; eax = bytes, edx = separator (0: runs of ASCII whitespace), ecx = maxsplit -> eax = list[bytes]
        call    rt_bytes_sepchk
        mov     byte [rt_bmode],1
        call    rt_str_split
        mov     byte [rt_bmode],0
        ret
rt_bytes_rsplit:                ; the same from the right
        call    rt_bytes_sepchk
        mov     byte [rt_bmode],1
        call    rt_str_rsplit
        mov     byte [rt_bmode],0
        ret
rt_bytes_sepchk:                ; edx = a separator (or 0): ValueError when empty
        test    edx,edx
        jz      @f
        cmp     dword [edx+8],0
        je      .esep
@@:     ret
.esep:  mov     esi,rt_msg_sep
        jmp     rt_panic_value

;;; code rt_bytes_replace : rt_str_replace rt_u8_next
rt_bytes_replace:               ; [esp+4] bytes, old, new, count (< 0: all) -> eax = new bytes
        mov     byte [rt_bmode],1
        push    dword [esp+16]
        push    dword [esp+16]
        push    dword [esp+16]
        push    dword [esp+16]
        call    rt_str_replace
        add     esp,16
        mov     byte [rt_bmode],0
        ret

;;; code rt_bytes_strip : rt_str_strip rt_u8_next
rt_bytes_strip:                 ; eax = bytes, edx = 1 left / 2 right / 3 both, ecx = the bytes to strip (0: ASCII whitespace) -> eax = new bytes
        mov     byte [rt_bmode],1
        call    rt_str_strip
        mov     byte [rt_bmode],0
        ret

;;; code rt_bytes_splitlines : rt_str_splitlines rt_u8_next
rt_bytes_splitlines:            ; eax = bytes, edx = 1: keep the line breaks -> eax = list of its lines (\n, \r, \r\n)
        mov     byte [rt_bmode],1
        call    rt_str_splitlines
        mov     byte [rt_bmode],0
        ret

