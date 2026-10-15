; ---------------------------------------------------------------- float arithmetic, float()

;;; code rt_ffloor
rt_ffloor:                      ; st0 -> st0 = floor(st0)
        sub     esp,4
        fnstcw  [esp]
        mov     ax,[esp]
        and     ax,0xF3FF
        or      ax,0x0400
        mov     [esp+2],ax
        fldcw   [esp+2]
        frndint
        fldcw   [esp]
        add     esp,4
        ret

;;; code rt_fmod : rt_panic_zero rt_ffloor
rt_fmod:                        ; st0 = a, st1 = b (both popped) -> st0 = a % b (sign of b)
        fxch
        ftst
        fnstsw  ax
        sahf
        jz      rt_panic_zero   ; b == 0
        fxch                    ; st0 = a, st1 = b
        fld     st0
        fdiv    st0,st2
        call    rt_ffloor       ; st0 = floor(a/b), st1 = a, st2 = b
        fmul    st0,st2
        fsubp   st1,st0         ; st0 = a - floor(a/b)*b, st1 = b
        fstp    st1
        ret

;;; code rt_ffloordiv : rt_panic_zero rt_ffloor
rt_ffloordiv:                   ; st0 = a, st1 = b (both popped) -> st0 = a // b
        fxch
        ftst
        fnstsw  ax
        sahf
        jz      rt_panic_zero
        fdivp   st1,st0         ; st0 = a / b
        jmp     rt_ffloor

;;; code rt_fdiv : rt_panic_zero
rt_fdiv:                        ; st0 = a, st1 = b (both popped) -> st0 = a / b
        fxch
        ftst
        fnstsw  ax
        sahf
        jz      rt_panic_zero
        fdivp   st1,st0
        ret

;;; code rt_fpow : rt_ffloor rt_ext_call
rt_fpow:                        ; st0 = a, st1 = b (both popped) -> st0 = a ** b
        push    rt_fpow_x          ; in extended precision
        jmp     rt_ext_call
rt_fpow_x:
        fxch                    ; st0 = b, st1 = a
        fld     st0
        call    rt_ffloor
        fcomp   st1             ; integral exponent?
        fnstsw  ax
        sahf
        jne     .general
        fld     st0             ; |b| <= 2^30: square and multiply
        fabs
        push    0x4E800000      ; 2^30
        fcomp   dword [esp]
        add     esp,4
        fnstsw  ax
        sahf
        jae     .general
        push    eax
        fist    dword [esp]
        pop     ecx             ; ecx = b
        fstp    st0             ; st0 = a
        fld1                    ; st0 = result, st1 = base
        mov     edx,ecx
        test    ecx,ecx
        jns     @f
        neg     ecx
@@:     test    ecx,ecx
        jz      .done
        test    ecx,1
        jz      .sq
        fmul    st0,st1
.sq:    fxch
        fmul    st0,st0
        fxch
        shr     ecx,1
        jmp     @b
.done:  fstp    st1
        test    edx,edx
        jns     .out
        fld1
        fdivrp  st1,st0
.out:   ret
.general:                       ; a ** b = 2 ** (b * log2 a), a > 0
        fxch                    ; st0 = a, st1 = b
        fyl2x                   ; st0 = b * log2(a)
        fld     st0
        frndint
        fsub    st1,st0
        fxch
        f2xm1
        fld1
        faddp   st1,st0
        fscale
        fstp    st1
        ret

;;; code rt_float_parse : rt_is_space rt_scale10 rt_panic_value rt_ext_call
rt_float_parse:                 ; eax = str -> st0 = float(str)
        push    rt_float_parse_x          ; in extended precision
        jmp     rt_ext_call
rt_float_parse_x:
        push    ebx esi edi ebp
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
@@:     xor     ebx,ebx         ; negative
        cmp     al,'-'
        jne     @f
        inc     ebx
        inc     esi
        jmp     .num
@@:     cmp     al,'+'
        jne     .num
        inc     esi
.num:   fldz
        xor     ebp,ebp         ; power of ten to apply
        xor     edx,edx         ; digits seen
        push    10
.int:   cmp     esi,edi
        jae     .exp
        movzx   ecx,byte [esi]
        cmp     cl,'.'
        je      .frac0
        sub     ecx,'0'
        cmp     ecx,9
        ja      .exp
        fimul   dword [esp]
        push    ecx
        fiadd   dword [esp]
        pop     ecx
        inc     edx
        inc     esi
        jmp     .int
.frac0: inc     esi
.frac:  cmp     esi,edi
        jae     .exp
        movzx   ecx,byte [esi]
        sub     ecx,'0'
        cmp     ecx,9
        ja      .exp
        fimul   dword [esp]
        push    ecx
        fiadd   dword [esp]
        pop     ecx
        inc     edx
        dec     ebp
        inc     esi
        jmp     .frac
.exp:   pop     ecx
        test    edx,edx
        jz      .word
        cmp     esi,edi
        jae     .apply
        mov     al,[esi]
        or      al,32
        cmp     al,'e'
        jne     .apply
        inc     esi
        xor     ecx,ecx         ; exponent
        xor     edx,edx         ; negative exponent
        cmp     esi,edi
        jae     .bad1
        cmp     byte [esi],'-'
        jne     @f
        inc     edx
        inc     esi
        jmp     .ed
@@:     cmp     byte [esi],'+'
        jne     .ed
        inc     esi
.ed:    cmp     esi,edi
        jae     .ee
        movzx   eax,byte [esi]
        sub     eax,'0'
        cmp     eax,9
        ja      .ee
        imul    ecx,10
        add     ecx,eax
        inc     esi
        jmp     .ed
.ee:    test    edx,edx
        jz      @f
        neg     ecx
@@:     add     ebp,ecx
.apply: mov     eax,ebp
        call    rt_scale10
.tail:  cmp     esi,edi
        jae     .done
        mov     al,[esi]
        call    rt_is_space
        jne     .bad1
        inc     esi
        jmp     .tail
.done:  test    ebx,ebx
        jz      @f
        fchs
@@:     pop     ebp edi esi ebx
        ret
.word:  mov     ecx,edi         ; no digits: inf, infinity, nan (any case)?
        sub     ecx,esi
        cmp     ecx,3
        jb      .bad1
        mov     eax,[esi]
        or      eax,0x202020
        and     eax,0xFFFFFF
        cmp     eax,'inf'
        je      .inf
        cmp     eax,'nan'
        jne     .bad1
        fstp    st0
        fldz
        fdiv    st0,st0         ; 0/0: a NaN (exceptions are masked)
        add     esi,3
        jmp     .tail
.inf:   fstp    st0
        fld1
        fldz
        fdivp   st1,st0         ; 1/0
        add     esi,3
        mov     ecx,edi
        sub     ecx,esi
        cmp     ecx,5
        jb      .tail
        mov     eax,[esi]
        or      eax,0x20202020
        cmp     eax,'init'
        jne     .tail
        mov     al,[esi+4]
        or      al,32
        cmp     al,'y'
        jne     .tail
        add     esi,5
        jmp     .tail
.bad1:  fstp    st0
.bad:   mov     esi,rt_msg_float
        jmp     rt_panic_value
;;; data rt_float_parse
rt_msg_float    db 'could not convert string to float',0

