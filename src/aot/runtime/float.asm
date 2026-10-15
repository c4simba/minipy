; ---------------------------------------------------------------- floats (x87)

;;; code rt_ext_call
; Compiled code computes in double precision, like Python (the x87 is set to
; round to 53 bits at start); formatting, parsing and pow need the full 64-bit
; mantissa internally. `push routine` + `jmp rt_ext_call` runs it that way.
rt_ext_call:
        sub     esp,4           ; [esp] saved control word, [esp+4] routine, [esp+8] return address
        fnstcw  [esp]
        push    dword [esp]
        or      word [esp],0x0300
        fldcw   [esp]
        add     esp,4
        call    dword [esp+4]
        fldcw   [esp]
        add     esp,8
        ret

;;; code rt_scale10
rt_scale10:                     ; st0 = v, eax = k -> st0 = v * 10^k
        push    eax
        fld1
        mov     ecx,eax
        test    ecx,ecx
        jns     @f
        neg     ecx
@@:     push    10
        fild    dword [esp]
        add     esp,4
        jecxz   .done
.mul:   fmul    st1,st0
        dec     ecx
        jnz     .mul
.done:  fstp    st0
        pop     eax
        test    eax,eax
        js      .div
        fmulp   st1,st0
        ret
.div:   fdivp   st1,st0
        ret

;;; code rt_sb_gen : rt_sb_bytes rt_sb_char rt_scale10 rt_ext_call
; st0 = value (popped). edx = significant digits (1..17); ecx bit 0: add ".0"
; when the text looks like an integer, bit 1: fixed notation up to 1e16 (both:
; Python's float printing), bit 2: scientific keeping trailing zeros (%.*e with
; P-1 decimals), bit 3: one unit more in the last digit. Otherwise printf %.*g.
rt_sb_gen:
        push    rt_sb_gen_x          ; in extended precision
        jmp     rt_ext_call
rt_sb_gen_x:
        push    ebx esi edi ebp
        sub     esp,64          ; +0 value, +8 exponent, +12 control words, +16 mantissa, +24 digits wanted, +28 flags, +32 text
        mov     [esp+24],edx
        mov     [esp+28],ecx
        fst     qword [esp]
        mov     eax,[esp+4]
        and     eax,0x7FF00000
        cmp     eax,0x7FF00000
        jne     .finite
        fstp    st0
        mov     eax,[esp+4]
        and     eax,0x000FFFFF
        or      eax,[esp]
        mov     eax,rt_s_nan
        jnz     .word
        test    byte [esp+7],0x80
        mov     eax,rt_s_inf
        jz      .word
        mov     eax,rt_s_minf
.word:  mov     edx,eax
@@:     cmp     byte [edx],0
        je      @f
        inc     edx
        jmp     @b
@@:     sub     edx,eax
        call    rt_sb_bytes
        jmp     .out
.finite:
        ftst
        fnstsw  ax
        sahf
        jne     .nonzero
        fstp    st0
        test    byte [esp+7],0x80
        jz      @f
        mov     al,'-'
        call    rt_sb_char
@@:     mov     al,'0'
        call    rt_sb_char
        jmp     .dotzero
.nonzero:
        jae     .pos
        fchs
        mov     al,'-'
        call    rt_sb_char
.pos:   fld     st0             ; exponent = floor(log10(v))
        fldlg2
        fxch
        fyl2x
        fnstcw  [esp+12]
        mov     ax,[esp+12]
        and     ax,0xF3FF
        or      ax,0x0400
        mov     [esp+14],ax
        fldcw   [esp+14]
        fistp   dword [esp+8]
        fldcw   [esp+12]
        mov     ebx,3           ; tries to land the mantissa in [10^(P-1), 10^P)
.scale: mov     eax,[esp+24]
        dec     eax
        sub     eax,[esp+8]
        fld     st0
        call    rt_scale10
        fistp   qword [esp+16]
        dec     ebx
        jz      .ok
        fild    qword [esp+16]
        fld1
        mov     eax,[esp+24]
        dec     eax
        call    rt_scale10
        fcomp   st1
        fnstsw  ax
        sahf
        jbe     @f
        fstp    st0
        dec     dword [esp+8]
        jmp     .scale
@@:     fld1
        mov     eax,[esp+24]
        call    rt_scale10
        fcomp   st1
        fnstsw  ax
        sahf
        fstp    st0
        ja      .ok
        inc     dword [esp+8]
        jmp     .scale
.ok:    fstp    st0
        test    dword [esp+28],8
        jz      .nobump
        add     dword [esp+16],1          ; one unit more (10^P: 10^(P-1), the exponent one more)
        adc     dword [esp+20],0
        mov     eax,[esp+24]
        mov     edx,dword [rt_pow10q+eax*8+4]
        cmp     edx,[esp+20]
        jne     .nobump
        mov     edx,dword [rt_pow10q+eax*8]
        cmp     edx,[esp+16]
        jne     .nobump
        mov     edx,dword [rt_pow10q+eax*8-8]
        mov     [esp+16],edx
        mov     edx,dword [rt_pow10q+eax*8-4]
        mov     [esp+20],edx
        inc     dword [esp+8]
.nobump:
        mov     ecx,[esp+24]    ; P digits of the mantissa into the text buffer
        lea     edi,[esp+32]
        add     edi,ecx
        mov     esi,[esp+16]
        mov     ebp,[esp+20]
        mov     ebx,10
.dig:   mov     eax,ebp
        xor     edx,edx
        div     ebx
        mov     ebp,eax
        mov     eax,esi
        div     ebx
        mov     esi,eax
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        dec     ecx
        jnz     .dig
        mov     ecx,[esp+24]    ; drop trailing zeros
        test    dword [esp+28],4
        jnz     .form
.tz:    cmp     ecx,1
        jbe     .form
        cmp     byte [esp+32+ecx-1],'0'
        jne     .form
        dec     ecx
        jmp     .tz
.form:  mov     ebx,ecx         ; ebx = significant digits
        mov     eax,[esp+8]
        test    dword [esp+28],4
        jnz     .sci
        cmp     eax,-4
        jl      .sci
        mov     edx,[esp+24]
        test    dword [esp+28],2
        jz      @f
        mov     edx,16
@@:     cmp     eax,edx
        jge     .sci
        test    eax,eax
        js      .small
        lea     edx,[eax+1]     ; integer digits
        cmp     edx,ebx
        jbe     @f
        lea     eax,[esp+32]
        mov     edx,ebx
        call    rt_sb_bytes
        mov     ecx,[esp+8]
        inc     ecx
        sub     ecx,ebx
.zeros: push    ecx
        mov     al,'0'
        call    rt_sb_char
        pop     ecx
        dec     ecx
        jnz     .zeros
        jmp     .dotzero
@@:     push    edx
        lea     eax,[esp+36]
        call    rt_sb_bytes
        pop     edx
        cmp     edx,ebx
        je      .dotzero
        push    edx
        mov     al,'.'
        call    rt_sb_char
        pop     edx
        lea     eax,[esp+32+edx]
        sub     ebx,edx
        mov     edx,ebx
        call    rt_sb_bytes
        jmp     .out
.small: mov     al,'0'          ; 0.000ddd
        call    rt_sb_char
        mov     al,'.'
        call    rt_sb_char
        mov     ecx,[esp+8]
        not     ecx             ; -e-1 zeros
        jecxz   @f
.z2:    push    ecx
        mov     al,'0'
        call    rt_sb_char
        pop     ecx
        dec     ecx
        jnz     .z2
@@:     lea     eax,[esp+32]
        mov     edx,ebx
        call    rt_sb_bytes
        jmp     .out
.sci:   lea     eax,[esp+32]    ; d[.ddd]e+XX
        mov     edx,1
        call    rt_sb_bytes
        cmp     ebx,1
        jbe     @f
        mov     al,'.'
        call    rt_sb_char
        lea     eax,[esp+33]
        lea     edx,[ebx-1]
        call    rt_sb_bytes
@@:     mov     al,'e'
        call    rt_sb_char
        mov     eax,[esp+8]
        mov     dl,'+'
        test    eax,eax
        jns     @f
        neg     eax
        mov     dl,'-'
@@:     push    eax
        mov     al,dl
        call    rt_sb_char
        pop     eax
        cmp     eax,10
        jae     @f
        push    eax
        mov     al,'0'
        call    rt_sb_char
        pop     eax
@@:     lea     edi,[esp+64]
        mov     ecx,10
.e:     xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        test    eax,eax
        jnz     .e
        mov     eax,edi
        lea     edx,[esp+64]
        sub     edx,edi
        call    rt_sb_bytes
        jmp     .out
.dotzero:
        test    dword [esp+28],1
        jz      .out
        mov     al,'.'
        call    rt_sb_char
        mov     al,'0'
        call    rt_sb_char
.out:   add     esp,64
        pop     ebp edi esi ebx
        ret
;;; data rt_sb_gen
align 8
rt_pow10q       dq 1,10,100,1000,10000,100000,1000000,10000000,100000000,1000000000,10000000000
                dq 100000000000,1000000000000,10000000000000,100000000000000,1000000000000000
                dq 10000000000000000,100000000000000000
rt_s_nan        db 'nan',0
rt_s_inf        db 'inf',0
rt_s_minf       db '-inf',0

;;; code rt_sb_float : rt_sb_gen rt_sb_take rt_sb_str rt_float_parse rt_free
rt_sb_float:                    ; st0 = value (popped): Python's repr - the shortest text that reads back the same
        push    ebx esi edi
        sub     esp,16          ; +0 value, +8 value read back
        fstp    qword [esp]
        mov     esi,15
        xor     edi,edi         ; edi 8: the candidate one unit up
        mov     eax,[esp+4]
        and     eax,0x7FF00000
        cmp     eax,0x7FF00000
        je      .last           ; inf, nan
        test    eax,eax
        jnz     .try
        mov     esi,1           ; a subnormal: fewer digits than 15 may read back the same
.try:   mov     ebx,[rt_sb_len]
        fld     qword [esp]
        mov     edx,esi
        mov     ecx,3
        or      ecx,edi
        call    rt_sb_gen
        cmp     esi,17
        jae     .done
        mov     eax,ebx         ; read the text back
        call    rt_sb_take
        push    eax
        call    rt_float_parse
        fstp    qword [esp+12]
        mov     eax,[esp+12]
        cmp     eax,[esp+4]
        jne     .no
        mov     eax,[esp+16]
        cmp     eax,[esp+8]
        jne     .no
        mov     eax,[esp]
        call    rt_sb_str
        pop     eax
        call    rt_free
        jmp     .done
.no:    pop     eax
        call    rt_free
        test    edi,edi
        jnz     .next
        fld     qword [esp+8]   ; read back below the value (an exact tie rounded down):
        fabs                    ; the candidate one unit up may read back
        fld     qword [esp]
        fabs
        fcompp
        fnstsw  ax
        sahf
        jbe     .next
        mov     edi,8
        jmp     .try
.next:  xor     edi,edi
        inc     esi
        jmp     .try
.last:  fld     qword [esp]
        mov     edx,esi
        mov     ecx,3
        call    rt_sb_gen
.done:  add     esp,16
        pop     edi esi ebx
        ret

;;; code rt_sb_fixed : rt_sb_char rt_sb_bytes rt_scale10 rt_sb_gen rt_ext_call
rt_sb_fixed:                    ; st0 = value (popped), edx = digits after the point (0..40): printf %.*f
        push    rt_sb_fixed_x          ; in extended precision
        jmp     rt_ext_call
rt_sb_fixed_x:
        push    ebx esi edi ebp
        sub     esp,96          ; +0 value, +8 mantissa, +16 digits after the point, +32..96 text
        mov     [esp+16],edx
        fst     qword [esp]
        mov     eax,[esp+4]
        and     eax,0x7FF00000
        cmp     eax,0x7FF00000
        je      .gen
        ftst
        fnstsw  ax
        sahf
        jae     @f
        fchs
        mov     al,'-'
        call    rt_sb_char
@@:     mov     eax,[esp+16]
        call    rt_scale10
        push    0x5E000000      ; 2^61: larger values fall back to %g
        fcom    dword [esp]
        add     esp,4
        fnstsw  ax
        sahf
        jae     .big
        fistp   qword [esp+8]
        lea     edi,[esp+96]    ; digits of the mantissa, backwards
        mov     esi,[esp+8]
        mov     ebp,[esp+12]
        mov     ebx,10
        xor     ecx,ecx
.dig:   mov     eax,ebp
        xor     edx,edx
        div     ebx
        mov     ebp,eax
        mov     eax,esi
        div     ebx
        mov     esi,eax
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        inc     ecx
        mov     eax,esi
        or      eax,ebp
        jnz     .dig
.pad:   cmp     ecx,[esp+16]    ; at least one digit before the point
        ja      .print
        dec     edi
        mov     byte [edi],'0'
        inc     ecx
        jmp     .pad
.print: mov     ebx,ecx
        sub     ebx,[esp+16]    ; digits before the point
        mov     eax,edi
        mov     edx,ebx
        call    rt_sb_bytes
        cmp     dword [esp+16],0
        je      .out
        mov     al,'.'
        call    rt_sb_char
        lea     eax,[edi+ebx]
        mov     edx,[esp+16]
        call    rt_sb_bytes
        jmp     .out
.big:   mov     eax,[esp+16]    ; undo the scaling
        neg     eax
        call    rt_scale10
.gen:   mov     edx,17
        xor     ecx,ecx
        call    rt_sb_gen
.out:   add     esp,96
        pop     ebp edi esi ebx
        ret

