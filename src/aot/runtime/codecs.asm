; ---------------------------------------------------------------- codecs: encode / decode
;;; code rt_raise_unicode : rt_raise
rt_raise_unicode:               ; ecx = message (str, owned), edx = 0 decoding / 1 encoding: raise UnicodeDecodeError / UnicodeEncodeError
if defined rt_throw
        test    edx,edx
        jnz     @f
        mov     eax,VTX_UnicodeDecodeError
        mov     edx,DTX_UnicodeDecodeError
        xor     esi,esi
        jmp     rt_raise_builtin
@@:     mov     eax,VTX_UnicodeEncodeError
        mov     edx,DTX_UnicodeEncodeError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_unidec
        test    edx,edx
        jz      @f
        mov     esi,rt_s_unienc
@@:     jmp     rt_raise
end if
;;; data rt_raise_unicode
rt_s_unidec     db 'UnicodeDecodeError',0
rt_s_unienc     db 'UnicodeEncodeError',0

;;; code rt_raise_lookup : rt_raise
rt_raise_lookup:                ; ecx = message (str, owned): raise LookupError
if defined rt_throw
        mov     eax,VTX_LookupError
        mov     edx,DTX_LookupError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_lookup
        jmp     rt_raise
end if
;;; data rt_raise_lookup
rt_s_lookup     db 'LookupError',0

;;; code rt_codec : rt_sb_need rt_sb_cstr rt_sb_str rt_sb_take rt_raise_lookup
rt_codec:                       ; eax = an encoding's name (str) -> eax = 0 utf-8, 1 ascii, 2 latin-1 (LookupError otherwise)
        push    ebx esi edi
        sub     esp,32
        mov     ebx,eax
        lea     esi,[eax+12]
        mov     ecx,[eax+8]
        cmp     ecx,31
        ja      .unknown
        mov     edi,esp
        jecxz   .copied
.cp:    lodsb                   ; lower case, _ and space as -
        cmp     al,'A'
        jb      @f
        cmp     al,'Z'
        ja      @f
        or      al,32
@@:     cmp     al,'_'
        je      .dash
        cmp     al,' '
        jne     @f
.dash:  mov     al,'-'
@@:     stosb
        dec     ecx
        jnz     .cp
.copied:mov     byte [edi],0
        mov     esi,rt_codec_names
.name:  movzx   eax,byte [esi]  ; code, then the name
        cmp     al,0xFF
        je      .unknown
        inc     esi
        mov     edi,esp
@@:     mov     dl,[esi]
        cmp     dl,[edi]
        jne     .skip
        inc     esi
        inc     edi
        test    dl,dl
        jnz     @b
        add     esp,32
        pop     edi esi ebx
        ret
.skip:  cmp     byte [esi],0
        je      @f
        inc     esi
        jmp     .skip
@@:     inc     esi
        jmp     .name
.unknown:
        add     esp,32
        push    dword [rt_sb_len]
        mov     eax,rt_msg_unkenc
        call    rt_sb_cstr
        mov     eax,ebx
        call    rt_sb_str
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_lookup
;;; data rt_codec
rt_codec_names  db 0,'utf-8',0, 0,'utf8',0, 0,'u8',0, 1,'ascii',0, 1,'us-ascii',0
                db 2,'latin-1',0, 2,'latin1',0, 2,'iso-8859-1',0, 2,'iso8859-1',0, 2,'l1',0, 0xFF
rt_msg_unkenc   db 'unknown encoding: ',0

;;; code rt_errors : rt_str_eq rt_sb_need rt_sb_cstr rt_sb_str rt_sb_char rt_sb_take rt_raise_lookup rt_static
rt_errors:                      ; ecx = an error handler's name (str; 0: strict) -> eax = 0 strict / 1 ignore / 2 replace (LookupError otherwise)
        xor     eax,eax
        test    ecx,ecx
        jz      .out
        push    ecx
        mov     eax,ecx
        mov     edx,rt_s_strict
        call    rt_str_eq
        test    eax,eax
        mov     eax,0
        jnz     .found
        mov     eax,[esp]
        mov     edx,rt_s_ignore
        call    rt_str_eq
        test    eax,eax
        mov     eax,1
        jnz     .found
        mov     eax,[esp]
        mov     edx,rt_s_replace
        call    rt_str_eq
        test    eax,eax
        mov     eax,2
        jnz     .found
        push    dword [rt_sb_len]
        mov     eax,rt_msg_unkerr
        call    rt_sb_cstr
        mov     eax,[esp+4]
        call    rt_sb_str
        mov     al,39
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_lookup
.found: pop     ecx
.out:   ret
;;; data rt_errors
align 4
rt_s_strict     dd 0x40000000,rt_static,6
                db 'strict',0
                dd 6
align 4
rt_s_ignore     dd 0x40000000,rt_static,6
                db 'ignore',0
                dd 6
align 4
rt_s_replace    dd 0x40000000,rt_static,7
                db 'replace',0
                dd 7
rt_msg_unkerr   db "unknown error handler name '",0

;;; code rt_bytes_decode : rt_sb_char rt_sb_bytes rt_sb_cp rt_sb_cstr rt_sb_int rt_sb_take rt_errors rt_raise_unicode
rt_bytes_decode:                ; eax = bytes, edx = codec (rt_codec), ecx = errors (str, 0: strict) -> eax = str
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        push    ecx
        push    edx             ; [esp] codec, [esp+4] errors, [esp+8] the mark
        mov     ebx,eax
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
.l:     cmp     esi,edi
        jae     .done
        movzx   eax,byte [esi]
        cmp     al,0x80
        jb      .one
        cmp     dword [esp],2
        je      .latin
        lea     edx,[esi+1]
        mov     ecx,4           ; ascii: ordinal not in range(128)
        cmp     dword [esp],1
        je      .bad
        call    .seq
        test    eax,eax
        jz      .bad
        push    eax
        mov     edx,eax
        mov     eax,esi
        call    rt_sb_bytes
        pop     eax
        add     esi,eax
        jmp     .l
.one:   call    rt_sb_char
        inc     esi
        jmp     .l
.latin: call    rt_sb_cp
        inc     esi
        jmp     .l
.bad:   push    edx ecx         ; [esi, edx) cannot be decoded, ecx = why
        mov     ecx,[esp+12]
        call    rt_errors
        test    eax,eax
        jz      .strict
        cmp     eax,2
        jne     @f
        mov     eax,0xFFFD
        call    rt_sb_cp
@@:     pop     ecx edx
        mov     esi,edx
        jmp     .l
.done:  add     esp,8
        pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret
.strict:mov     eax,[esp+16]    ; the message, instead of what was decoded
        mov     [rt_sb_len],eax
        push    eax
        mov     al,39
        call    rt_sb_char
        mov     eax,[esp+12]
        mov     eax,[rt_codec_label+eax*4]
        call    rt_sb_cstr
        mov     eax,rt_msg_cantdec
        call    rt_sb_cstr
        mov     edx,[esp+8]     ; the end
        sub     edx,esi
        cmp     edx,1
        jne     .range
        mov     eax,rt_msg_byte0x
        call    rt_sb_cstr
        movzx   eax,byte [esi]
        shr     al,4
        call    .digit
        movzx   eax,byte [esi]
        and     al,15
        call    .digit
        mov     eax,rt_msg_inpos
        call    rt_sb_cstr
        call    .pos
        jmp     .why
.range: mov     eax,rt_msg_bytesinpos
        call    rt_sb_cstr
        call    .pos
        mov     al,'-'
        call    rt_sb_char
        mov     eax,[esp+8]
        lea     edx,[ebx+13]
        sub     eax,edx
        xor     edx,edx
        call    rt_sb_int
.why:   mov     eax,rt_msg_colon
        call    rt_sb_cstr
        mov     eax,[esp+4]     ; why
        mov     eax,[rt_decode_why+eax*4-4]
        call    rt_sb_cstr
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        xor     edx,edx
        jmp     rt_raise_unicode
.pos:   lea     eax,[ebx+12]
        neg     eax
        add     eax,esi
        xor     edx,edx
        jmp     rt_sb_int
.digit: add     al,'0'
        cmp     al,'9'
        jbe     @f
        add     al,'a'-'0'-10
@@:     jmp     rt_sb_char
.seq:   movzx   eax,byte [esi]  ; a valid UTF-8 sequence at esi -> eax = its length; else 0, edx = where the bad part ends, ecx = 1 invalid start / 2 invalid continuation byte / 3 unexpected end
        cmp     al,0xC2
        jb      .start
        cmp     al,0xF5
        jae     .start
        push    ebx
        mov     ecx,1
        mov     bl,0x80
        mov     bh,0xBF
        cmp     al,0xE0
        jb      .go
        inc     ecx
        cmp     al,0xF0
        jae     .four
        cmp     al,0xE0
        jne     @f
        mov     bl,0xA0
@@:     cmp     al,0xED
        jne     .go
        mov     bh,0x9F
        jmp     .go
.four:  inc     ecx
        cmp     al,0xF0
        jne     @f
        mov     bl,0x90
@@:     cmp     al,0xF4
        jne     .go
        mov     bh,0x8F
.go:    mov     edx,1
.k:     lea     eax,[esi+edx]
        cmp     eax,edi
        jae     .eod
        mov     al,[esi+edx]
        cmp     al,bl
        jb      .cont
        cmp     al,bh
        ja      .cont
        mov     bl,0x80
        mov     bh,0xBF
        inc     edx
        cmp     edx,ecx
        jbe     .k
        lea     eax,[ecx+1]
        pop     ebx
        ret
.eod:   mov     edx,edi
        mov     ecx,3
        xor     eax,eax
        pop     ebx
        ret
.cont:  add     edx,esi
        mov     ecx,2
        xor     eax,eax
        pop     ebx
        ret
.start: lea     edx,[esi+1]
        mov     ecx,1
        xor     eax,eax
        ret
;;; data rt_bytes_decode
align 4
rt_codec_label  dd rt_s_utf8, rt_s_ascii, rt_s_latin1
rt_decode_why   dd rt_msg_why1, rt_msg_why2, rt_msg_why3, rt_msg_why4
rt_s_utf8       db 'utf-8',0
rt_s_ascii      db 'ascii',0
rt_s_latin1     db 'latin-1',0
rt_msg_cantdec  db "' codec can't decode ",0
rt_msg_byte0x   db 'byte 0x',0
rt_msg_inpos    db ' in position ',0
rt_msg_bytesinpos db 'bytes in position ',0
rt_msg_colon    db ': ',0
rt_msg_why1     db 'invalid start byte',0
rt_msg_why2     db 'invalid continuation byte',0
rt_msg_why3     db 'unexpected end of data',0
rt_msg_why4     db 'ordinal not in range(128)',0

;;; code rt_str_encode : rt_str_new rt_bytes_cp rt_u8_next rt_sb_char rt_sb_cstr rt_sb_int rt_sb_take rt_errors rt_raise_unicode rt_bytes_decode
rt_str_encode:                  ; eax = str, edx = codec (rt_codec), ecx = errors (str, 0: strict) -> eax = bytes
        push    ebx esi edi ebp
        mov     ebx,eax
        test    edx,edx
        jnz     .narrow
        mov     eax,[ebx+8]     ; UTF-8: the same bytes
        call    rt_str_new
        call    rt_bytes_cp
        mov     ecx,[ebx+8]
        lea     esi,[ebx+12]
        lea     edi,[eax+12]
        rep     movsb
        pop     ebp edi esi ebx
        ret
.narrow:push    dword [rt_sb_len]
        push    ecx
        mov     ebp,0x80        ; the limit: ascii 128, latin-1 256
        cmp     edx,1
        je      @f
        mov     ebp,0x100
@@:     push    edx             ; [esp] codec, [esp+4] errors, [esp+8] the mark
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
        xor     ecx,ecx         ; position (code points)
.l:     cmp     esi,edi
        jae     .done
        call    rt_u8_next
        cmp     eax,ebp
        jae     .bad
        push    ecx
        call    rt_sb_char
        pop     ecx
        inc     ecx
        jmp     .l
.bad:   push    eax             ; [esp] the first, [esp+4] its position after the pushes below
        push    ecx
        lea     edx,[ecx+1]     ; the run of code points it cannot encode
.run:   cmp     esi,edi
        jae     .ran
        push    esi
        call    rt_u8_next
        cmp     eax,ebp
        pop     eax
        jb      .back
        inc     edx
        jmp     .run
.back:  mov     esi,eax
.ran:   push    edx             ; [esp] end, [esp+4] start, [esp+8] the first code point
        mov     ecx,[esp+16]
        call    rt_errors
        test    eax,eax
        jz      .strict
        cmp     eax,2
        jne     .ignore
        mov     ecx,[esp]
        sub     ecx,[esp+4]
@@:     push    ecx
        mov     al,'?'
        call    rt_sb_char
        pop     ecx
        dec     ecx
        jnz     @b
.ignore:pop     ecx
        add     esp,8
        jmp     .l
.done:  add     esp,8
        pop     eax
        call    rt_sb_take
        call    rt_bytes_cp
        pop     ebp edi esi ebx
        ret
.strict:mov     eax,[esp+20]    ; the message, instead of what was encoded
        mov     [rt_sb_len],eax
        push    eax
        mov     al,39
        call    rt_sb_char
        mov     eax,[esp+16]
        mov     eax,[rt_codec_label+eax*4]
        call    rt_sb_cstr
        mov     eax,rt_msg_cantenc
        call    rt_sb_cstr
        mov     eax,[esp+4]
        sub     eax,[esp+8]
        cmp     eax,1
        jne     .range
        mov     eax,rt_msg_character
        call    rt_sb_cstr
        mov     eax,[esp+12]    ; '\xe9', '€', '\U0001f600'
        mov     ecx,2
        mov     dl,'x'
        cmp     eax,0x100
        jb      @f
        mov     ecx,4
        mov     dl,'u'
        cmp     eax,0x10000
        jb      @f
        mov     ecx,8
        mov     dl,'U'
@@:     push    eax ecx
        push    edx
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
        mov     al,39
        call    rt_sb_char
        mov     eax,rt_msg_inpos
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
        jmp     .why
.range: mov     eax,rt_msg_charsinpos
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
        mov     al,'-'
        call    rt_sb_char
        mov     eax,[esp+4]
        dec     eax
        xor     edx,edx
        call    rt_sb_int
.why:   mov     eax,rt_msg_ordinal
        call    rt_sb_cstr
        mov     eax,ebp
        xor     edx,edx
        call    rt_sb_int
        mov     al,')'
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        mov     edx,1
        jmp     rt_raise_unicode
;;; data rt_str_encode
rt_msg_cantenc  db "' codec can't encode ",0
rt_msg_character db "character '",0
rt_msg_charsinpos db 'characters in position ',0
rt_msg_ordinal  db ': ordinal not in range(',0

