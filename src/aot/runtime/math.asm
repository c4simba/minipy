; ---------------------------------------------------------------- math, random

;;; code rt_fexp : rt_ext_call
rt_fexp:                        ; st0 = x -> st0 = e^x
        push    rt_fexp_x          ; in extended precision
        jmp     rt_ext_call
rt_fexp_x:
        fldl2e
        fmulp   st1,st0
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

; one-argument math functions, in extended precision (st0 = x -> st0 = f(x));
; macOS programs (C) use the C library's instead (aot/x2c.c natives)
;;; code rt_flog : rt_ext_call
rt_flog:        push    rt_flog_x
                jmp     rt_ext_call
rt_flog_x:      fldln2
                fxch
                fyl2x
                ret
;;; code rt_flog10 : rt_ext_call
rt_flog10:      push    rt_flog10_x
                jmp     rt_ext_call
rt_flog10_x:    fldlg2
                fxch
                fyl2x
                ret
;;; code rt_flog2 : rt_ext_call
rt_flog2:       push    rt_flog2_x
                jmp     rt_ext_call
rt_flog2_x:     fld1
                fxch
                fyl2x
                ret
;;; code rt_fatan : rt_ext_call
rt_fatan:       push    rt_fatan_x
                jmp     rt_ext_call
rt_fatan_x:     fld1
                fpatan
                ret
;;; code rt_fasin : rt_ext_call
rt_fasin:       push    rt_fasin_x
                jmp     rt_ext_call
rt_fasin_x:     fld     st0
                fmul    st0,st0
                fld1
                fsubrp  st1,st0
                fsqrt
                fpatan
                ret
;;; code rt_facos : rt_ext_call
rt_facos:       push    rt_facos_x
                jmp     rt_ext_call
rt_facos_x:     fld     st0
                fmul    st0,st0
                fld1
                fsubrp  st1,st0
                fsqrt
                fxch
                fpatan
                ret
;;; code rt_fsin : rt_ext_call
rt_fsin:        push    rt_fsin_x
                jmp     rt_ext_call
rt_fsin_x:      fsin
                ret
;;; code rt_fcos : rt_ext_call
rt_fcos:        push    rt_fcos_x
                jmp     rt_ext_call
rt_fcos_x:      fcos
                ret
;;; code rt_ftan : rt_ext_call
rt_ftan:        push    rt_ftan_x
                jmp     rt_ext_call
rt_ftan_x:      fptan
                fstp    st0
                ret
;;; code rt_fatan2 : rt_ext_call
rt_fatan2:      push    rt_fatan2_x     ; st0 = x, st1 = y -> st0 = atan2(y, x)
                jmp     rt_ext_call
rt_fatan2_x:    fpatan
                ret

;;; code rt_fmod_c : rt_panic_zero
rt_fmod_c:                      ; st0 = x, st1 = y (both popped) -> st0 = fmod(x, y), the sign of x
        fxch
        ftst
        fnstsw  ax
        sahf
        je      .zero
        fxch
.again: fprem
        fnstsw  ax
        sahf
        jp      .again
        fstp    st1
        ret
.zero:  fstp    st0
        fstp    st0
        jmp     rt_panic_zero

;;; code rt_frexp
rt_frexp:                       ; st0 = x -> st0 = m (0.5 <= |m| < 1, x's sign), eax = e: x = m * 2**e
        sub     esp,8           ; (0, an infinity, a NaN: x itself and 0)
        fst     qword [esp]
        mov     eax,[esp+4]
        mov     ecx,eax
        shr     ecx,20
        and     ecx,0x7FF
        jz      .small
        cmp     ecx,0x7FF
        je      .same
.exp:   and     eax,0x800FFFFF  ; the exponent of 0.5
        or      eax,0x3FE00000
        mov     [esp+4],eax
        fstp    st0
        fld     qword [esp]
        lea     eax,[ecx-1022]
        add     esp,8
        ret
.small: mov     edx,eax
        and     edx,0x7FFFFFFF
        or      edx,[esp]
        jz      .same
        fmul    qword [rt_two54]  ; a subnormal: scaled up into the normal ones
        fst     qword [esp]
        mov     eax,[esp+4]
        mov     ecx,eax
        shr     ecx,20
        and     ecx,0x7FF
        sub     ecx,54
        jmp     .exp
.same:  xor     eax,eax
        add     esp,8
        ret
;;; data rt_frexp
align 8
rt_two54        dd 0,0x43500000   ; 2**54

;;; code rt_ldexp : rt_ovferr_text
rt_ldexp:                       ; st0 = x, edx:eax = i -> st0 = x * 2**i (OverflowError when too big)
        mov     ecx,eax
        sar     ecx,31
        cmp     ecx,edx
        jne     .far
        cmp     eax,65536
        jg      .up
        cmp     eax,-65536
        jl      .down
        jmp     .go
.far:   test    edx,edx
        js      .down
.up:    mov     eax,65536
        jmp     .go
.down:  mov     eax,-65536
.go:    sub     esp,8
        fst     qword [esp]     ; 0, an infinity, a NaN: x itself
        mov     ecx,[esp+4]
        and     ecx,0x7FF00000
        cmp     ecx,0x7FF00000
        je      .same
        mov     ecx,[esp+4]
        and     ecx,0x7FFFFFFF
        or      ecx,[esp]
        jz      .same
        mov     [esp],eax
        fild    dword [esp]
        fxch
        fscale
        fstp    st1
        fstp    qword [esp]     ; (rounded to a float once)
        fld     qword [esp]
        mov     ecx,[esp+4]
        and     ecx,0x7FF00000
        cmp     ecx,0x7FF00000
        je      .big
.same:  add     esp,8
        ret
.big:   fstp    st0
        add     esp,8
        mov     esi,rt_msg_mrange
        jmp     rt_ovferr_text
;;; data rt_ldexp
rt_msg_mrange   db 'math range error',0

;;; code rt_rand : rt_now_ms
rt_rand:                        ; -> eax = 32 random bits (xorshift32; seeded from the clock unless seeded)
        mov     eax,[rt_rand_state]
        test    eax,eax
        jnz     @f
        call    rt_now_ms
        xor     eax,0x9E3779B9
        or      eax,1
@@:     mov     edx,eax
        shl     edx,13
        xor     eax,edx
        mov     edx,eax
        shr     edx,17
        xor     eax,edx
        mov     edx,eax
        shl     edx,5
        xor     eax,edx
        mov     [rt_rand_state],eax
        ret
;;; bss rt_rand
rt_rand_state   rd 1

;;; code rt_rand_seed : rt_rand
rt_rand_seed:                   ; eax = seed
        imul    eax,eax,0x9E3779B1
        xor     eax,0x5DEECE66
        or      eax,1
        mov     [rt_rand_state],eax
        ret

;;; code rt_rand_float : rt_rand
rt_rand_float:                  ; -> st0 in [0, 1)
        call    rt_rand
        shr     eax,1
        push    eax
        fild    dword [esp]
        mov     dword [esp],0x4F000000   ; 2^31 as a float
        fdiv    dword [esp]
        add     esp,4
        ret

;;; code rt_rand_below : rt_rand
rt_rand_below:                  ; eax = n (> 0, 32 bits) -> eax = a random number in [0, n)
        push    eax
        call    rt_rand
        xor     edx,edx
        div     dword [esp]
        add     esp,4
        mov     eax,edx
        ret

;;; code rt_rand_range : rt_rand rt_rand_below rt_udivmod rt_sb_need rt_sb_cstr rt_sb_int rt_sb_char rt_sb_take rt_raise_valerr rt_panic_index
; randrange / randint / choice: the interpreter's numbers (xorshift32; a range of
; 2**32 or more takes two draws)
rt_rand_range:                  ; [esp+4] lo, edx:eax hi (exclusive; randint: inclusive), ecx = 0 randrange(n), 1 randrange(a, b), 2 randint, 3 choice -> edx:eax
        push    ebx esi edi ebp
        mov     ebp,ecx
        push    edx             ; [esp] hi as given
        push    eax
        cmp     ebp,2
        jne     @f
        add     eax,1           ; randint: up to hi itself
        adc     edx,0
@@:     sub     eax,[esp+28]    ; the span
        sbb     edx,[esp+32]
        jo      .huge
        test    edx,edx
        js      .empty
        jnz     .wide
        test    eax,eax
        jz      .empty
        call    rt_rand_below
        xor     edx,edx
        jmp     .add
.huge:  jmp     .empty
.wide:  mov     ebx,eax         ; span: ecx:ebx
        mov     ecx,edx
        push    ecx
        push    ebx
        call    rt_rand
        mov     esi,eax
        call    rt_rand
        mov     edx,esi
        pop     ebx
        pop     ecx
        call    rt_udivmod      ; the remainder (ecx:ebx)
        mov     eax,ebx
        mov     edx,ecx
.add:   add     eax,[esp+28]
        adc     edx,[esp+32]
        add     esp,8
        pop     ebp edi esi ebx
        ret
.empty: cmp     ebp,3
        je      .choice
        push    dword [rt_sb_len]
        mov     eax,rt_msg_rr0
        test    ebp,ebp
        jz      .msg
        mov     eax,rt_msg_rr1
        cmp     ebp,1
        je      @f
        mov     eax,rt_msg_rr2
@@:     call    rt_sb_cstr
        mov     eax,[esp+32]
        mov     edx,[esp+36]
        call    rt_sb_int
        mov     eax,rt_msg_rrsep
        call    rt_sb_cstr
        mov     eax,[esp+4]
        mov     edx,[esp+8]
        call    rt_sb_int
        mov     al,')'
        call    rt_sb_char
        jmp     .take
.msg:   call    rt_sb_cstr
.take:  pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
.choice:
        mov     esi,rt_msg_choice
        jmp     rt_index_error
;;; data rt_rand_range
rt_msg_rr0      db 'empty range for randrange()',0
rt_msg_rr1      db 'empty range in randrange(',0
rt_msg_rr2      db 'empty range in randint(',0
rt_msg_rrsep    db ', ',0
rt_msg_choice   db 'Cannot choose from an empty sequence',0

;;; code rt_shuffle : rt_rand_below
rt_shuffle:                     ; eax = list, edx = element size (4 or 8): Fisher-Yates
        push    ebx esi edi ebp
        test    eax,eax
        jz      .out
        mov     ebx,eax
        mov     ebp,edx
        mov     esi,[ebx+8]
.next:  cmp     esi,1
        jbe     .out
        mov     eax,esi
        call    rt_rand_below   ; j in [0, i)
        dec     esi             ; swap elements i-1 and j
        mov     edi,[ebx+16]
        imul    eax,ebp
        mov     edx,esi
        imul    edx,ebp
        add     eax,edi
        add     edx,edi
        mov     ecx,[eax]
        mov     edi,[edx]
        mov     [eax],edi
        mov     [edx],ecx
        cmp     ebp,8
        jne     .next
        mov     ecx,[eax+4]
        mov     edi,[edx+4]
        mov     [eax+4],edi
        mov     [edx+4],ecx
        jmp     .next
.out:   pop     ebp edi esi ebx
        ret

