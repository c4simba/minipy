; ---------------------------------------------------------------- 64-bit integers: conversions, number theory, hashing
;;; code rt_fround : rt_ftoi
;;; code rt_ftoi : rt_ovferr_text rt_panic_value
; float -> int, as CPython's int(x) (truncating) and round(x) (halves to even)
rt_ftoi:                        ; st0 (popped) -> edx:eax = int(st0)
        mov     ecx,0x0C00
        jmp     rt_f2int
rt_fround:                      ; st0 (popped) -> edx:eax = round(st0)
        xor     ecx,ecx
rt_f2int:                       ; ecx = the rounding control bits
        sub     esp,12
        fld     st0
        fcomp   st0
        fnstsw  ax
        sahf
        jp      .nan
        fld     st0
        fabs
        fcomp   qword [rt_f2p63]
        fnstsw  ax
        sahf
        jb      .ok
        fld     st0
        fabs
        fcomp   qword [rt_finf]
        fnstsw  ax
        sahf
        je      .inf
        fcom    qword [rt_fm2p63]
        fnstsw  ax
        sahf
        jne     .big
.ok:    fnstcw  [esp]
        mov     ax,[esp]
        and     ax,0xF3FF
        or      ax,cx
        mov     [esp+2],ax
        fldcw   [esp+2]
        fistp   qword [esp+4]
        fldcw   [esp]
        mov     eax,[esp+4]
        mov     edx,[esp+8]
        add     esp,12
        ret
.nan:   fstp    st0
        add     esp,12
        mov     esi,rt_msg_fnan
        jmp     rt_panic_value
.inf:   fstp    st0
        add     esp,12
        mov     esi,rt_msg_finf
        jmp     rt_ovferr_text
.big:   fstp    st0
        add     esp,12
        mov     esi,rt_msg_ibig2
        jmp     rt_ovferr_text
;;; data rt_ftoi
align 8
rt_f2p63        dd 0,0x43E00000         ; 2**63
rt_fm2p63       dd 0,0xC3E00000         ; -2**63
rt_finf         dd 0,0x7FF00000
rt_msg_fnan     db 'cannot convert float NaN to integer',0
rt_msg_finf     db 'cannot convert float infinity to integer',0
rt_msg_ibig2    db 'int too large (ints are 64-bit)',0

;;; code rt_gcd : rt_udivmod
rt_gcd:                         ; [esp+4] a, edx:eax b -> edx:eax = gcd(a, b) (not negative)
        push    ebx esi edi
        mov     ebx,eax         ; |b| -> ecx:ebx
        mov     ecx,edx
        test    ecx,ecx
        jns     @f
        neg     ebx
        adc     ecx,0
        neg     ecx
@@:     mov     eax,[esp+16]    ; |a| -> edx:eax
        mov     edx,[esp+20]
        test    edx,edx
        jns     .l
        neg     eax
        adc     edx,0
        neg     edx
.l:     mov     esi,ebx         ; while b: a, b = b, a % b
        or      esi,ecx
        jz      .out
        push    ecx
        push    ebx
        call    rt_udivmod      ; ecx:ebx = a % b
        pop     eax
        pop     edx             ; a = the old b
        jmp     .l
.out:   pop     edi esi ebx
        ret

;;; code rt_isqrt : rt_panic_value
rt_isqrt:                       ; edx:eax -> edx:eax = floor(sqrt(edx:eax))
        test    edx,edx
        js      .neg
        push    ebx esi
        mov     ebx,eax         ; n = esi:ebx
        mov     esi,edx
        push    edx
        push    eax
        fild    qword [esp]
        fsqrt
        fistp   qword [esp]     ; a close guess (< 2**32)
        pop     ecx
        add     esp,4
.down:  mov     eax,ecx         ; while x*x > n: x -= 1
        mul     ecx
        cmp     edx,esi
        ja      .dec
        jb      .up
        cmp     eax,ebx
        jbe     .up
.dec:   dec     ecx
        jmp     .down
.up:    lea     eax,[ecx+1]     ; while (x+1)*(x+1) <= n: x += 1
        test    eax,eax
        jz      .out
        mul     eax
        cmp     edx,esi
        ja      .out
        jb      .inc
        cmp     eax,ebx
        ja      .out
.inc:   inc     ecx
        jmp     .up
.out:   mov     eax,ecx
        xor     edx,edx
        pop     esi ebx
        ret
.neg:   mov     esi,rt_msg_isqrt
        jmp     rt_panic_value
;;; data rt_isqrt
rt_msg_isqrt    db 'isqrt() argument must be nonnegative',0

;;; code rt_mulmod
rt_mulmod:                      ; edx:eax * [ecx] modulo [ecx+8] (all < 2**63, unsigned) -> edx:eax
        push    ebx esi edi ebp
        push    dword [ecx+12]  ; [esp+8] m
        push    dword [ecx+8]
        push    dword [ecx+4]   ; [esp] b (doubled each step)
        push    dword [ecx]
        mov     esi,eax         ; a: edi:esi, its bits from the bottom
        mov     edi,edx
        xor     ebx,ebx         ; the result: ebp:ebx
        xor     ebp,ebp
.l:     mov     eax,esi
        or      eax,edi
        jz      .out
        test    esi,1
        jz      .dbl
        add     ebx,[esp]       ; result = (result + b) % m
        adc     ebp,[esp+4]
        cmp     ebp,[esp+12]
        jb      .dbl
        ja      @f
        cmp     ebx,[esp+8]
        jb      .dbl
@@:     sub     ebx,[esp+8]
        sbb     ebp,[esp+12]
.dbl:   mov     eax,[esp]       ; b = 2b % m
        mov     edx,[esp+4]
        shld    edx,eax,1
        shl     eax,1
        cmp     edx,[esp+12]
        jb      @f
        ja      .sub
        cmp     eax,[esp+8]
        jb      @f
.sub:   sub     eax,[esp+8]
        sbb     edx,[esp+12]
@@:     mov     [esp],eax
        mov     [esp+4],edx
        shrd    esi,edi,1
        shr     edi,1
        jmp     .l
.out:   mov     eax,ebx
        mov     edx,ebp
        add     esp,16
        pop     ebp edi esi ebx
        ret

;;; code rt_ipowmod : rt_mulmod rt_idivmod rt_imul rt_panic_value rt_scratch
; pow(base, exp, mod), CPython's: the result has mod's sign; exp < 0 uses the
; inverse of base modulo mod
rt_ipowmod:                     ; [esp+4] base, [esp+12] exp, edx:eax mod -> edx:eax
        push    ebx esi edi ebp
        mov     ebx,eax
        or      ebx,edx
        jz      .zero
        sub     esp,32          ; +0 |m|, +8 base mod |m|, +16 result, +24 m
        mov     [esp+24],eax
        mov     [esp+28],edx
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     mov     [esp],eax
        mov     [esp+4],edx
        push    dword [esp+56]  ; base % |m| (0 <= it < |m|): |m| is in edx:eax
        push    dword [esp+56]
        call    rt_idivmod
        add     esp,8
        mov     eax,[rt_scratch]
        mov     edx,[rt_scratch+4]
        mov     [esp+8],eax
        mov     [esp+12],edx
        mov     esi,[esp+60]    ; the exponent: edi:esi
        mov     edi,[esp+64]
        test    edi,edi
        jns     .pos
        call    .inverse        ; base = base**-1 mod |m|
        neg     esi
        adc     edi,0
        neg     edi
.pos:   mov     dword [esp+16],1   ; result = 1 % |m|
        mov     dword [esp+20],0
        cmp     dword [esp+4],0
        jne     .l
        cmp     dword [esp],1
        jne     .l
        mov     dword [esp+16],0
.l:     mov     eax,esi
        or      eax,edi
        jz      .sign
        test    esi,1
        jz      .sq
        mov     eax,[esp+16]
        mov     edx,[esp+20]
        push    dword [esp+4]   ; for rt_mulmod: [ecx] base, [ecx+8] |m|
        push    dword [esp+4]
        push    dword [esp+20]
        push    dword [esp+20]
        mov     ecx,esp
        call    rt_mulmod
        add     esp,16
        mov     [esp+16],eax
        mov     [esp+20],edx
.sq:    shrd    esi,edi,1
        shr     edi,1
        mov     eax,esi
        or      eax,edi
        jz      .sign
        mov     eax,[esp+8]
        mov     edx,[esp+12]
        push    dword [esp+4]
        push    dword [esp+4]
        push    dword [esp+20]
        push    dword [esp+20]
        mov     ecx,esp
        call    rt_mulmod
        add     esp,16
        mov     [esp+8],eax
        mov     [esp+12],edx
        jmp     .l
.sign:  mov     eax,[esp+16]    ; a negative modulus: result - |m| unless 0
        mov     edx,[esp+20]
        cmp     dword [esp+28],0
        jns     .out
        mov     ecx,eax
        or      ecx,edx
        jz      .out
        sub     eax,[esp]
        sbb     edx,[esp+4]
.out:   add     esp,32
        pop     ebp edi esi ebx
        ret
.zero:  mov     esi,rt_msg_powmod0
        jmp     rt_panic_value
; the inverse of [esp+12] (base) modulo [esp+4] (|m|, called with one more
; dword on the stack): extended Euclid on (base, m)
.inverse:
        push    esi edi
        ; r0 = m, r1 = base; t0 = 0, t1 = 1 (as signed 64-bit, |t| <= m)
        sub     esp,32          ; +0 r0, +8 r1, +16 t0, +24 t1
        mov     eax,[esp+44]
        mov     edx,[esp+48]
        mov     [esp],eax
        mov     [esp+4],edx
        mov     eax,[esp+52]
        mov     edx,[esp+56]
        mov     [esp+8],eax
        mov     [esp+12],edx
        mov     dword [esp+16],0
        mov     dword [esp+20],0
        mov     dword [esp+24],1
        mov     dword [esp+28],0
.eu:    mov     eax,[esp+8]     ; while r1 != 0
        or      eax,[esp+12]
        jz      .eud
        push    dword [esp+4]   ; q = r0 // r1, r = r0 % r1
        push    dword [esp+4]
        mov     eax,[esp+16]
        mov     edx,[esp+20]
        call    rt_idivmod
        add     esp,8
        push    edx             ; q
        push    eax
        mov     eax,[esp+16]    ; r0, r1 = r1, r
        mov     edx,[esp+20]
        mov     [esp+8],eax
        mov     [esp+12],edx
        mov     eax,[rt_scratch]
        mov     edx,[rt_scratch+4]
        mov     [esp+16],eax
        mov     [esp+20],edx
        mov     eax,[esp+32]    ; t0, t1 = t1, t0 - q*t1
        mov     edx,[esp+36]
        push    edx
        push    eax
        mov     eax,[esp+8]
        mov     edx,[esp+12]
        call    rt_imul         ; q * t1
        add     esp,8
        mov     ecx,[esp+24]
        mov     ebx,[esp+28]
        sub     ecx,eax
        sbb     ebx,edx         ; t0 - q*t1
        mov     eax,[esp+32]
        mov     edx,[esp+36]
        mov     [esp+24],eax
        mov     [esp+28],edx
        mov     [esp+32],ecx
        mov     [esp+36],ebx
        add     esp,8
        jmp     .eu
.eud:   cmp     dword [esp+4],0   ; r0 must be 1
        jne     .noinv
        cmp     dword [esp],1
        jne     .noinv
        mov     eax,[esp+16]    ; t0 mod m
        mov     edx,[esp+20]
        test    edx,edx
        jns     @f
        add     eax,[esp+44]
        adc     edx,[esp+48]
@@:     mov     [esp+52],eax
        mov     [esp+56],edx
        add     esp,32
        pop     edi esi
        ret
.noinv: mov     esi,rt_msg_noinv
        jmp     rt_panic_value
;;; data rt_ipowmod
rt_msg_powmod0  db 'pow() 3rd argument cannot be 0',0
rt_msg_noinv    db 'base is not invertible for the given modulus',0

;;; code rt_list_isum : rt_int_overflow
rt_list_isum:                   ; eax = list of int -> edx:eax = their sum
        push    ebx esi
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
        mov     esi,[eax+16]
@@:     xor     eax,eax
        xor     edx,edx
        jecxz   .out
@@:     add     eax,[esi]
        adc     edx,[esi+4]
        jo      .ovf
        add     esi,8
        dec     ecx
        jnz     @b
.out:   pop     esi ebx
        ret
.ovf:   pop     esi ebx
        jmp     rt_int_overflow

;;; code rt_range_list : rt_list_new rt_list_reserve rt_list_push rt_kd_int rt_panic_value
rt_range_list:                  ; [esp+4] start, [esp+12] stop, [esp+20] step (8 bytes each) -> eax = list(range(start, stop, step))
        push    ebx esi edi ebp
        mov     eax,[esp+36]
        or      eax,[esp+40]
        jz      .zero
        mov     eax,rt_kd_int
        call    rt_list_new
        mov     ebx,eax
        mov     esi,[esp+20]    ; i = start: edi:esi
        mov     edi,[esp+24]
.l:     cmp     dword [esp+40],0
        jl      .down
        mov     eax,esi         ; i < stop?
        cmp     eax,[esp+28]
        mov     eax,edi
        sbb     eax,[esp+32]
        jge     .out
        jmp     .put
.down:  mov     eax,[esp+28]    ; stop < i?
        cmp     eax,esi
        mov     eax,[esp+32]
        sbb     eax,edi
        jge     .out
.put:   mov     eax,ebx
        call    rt_list_push
        mov     [eax],esi
        mov     [eax+4],edi
        add     esi,[esp+36]
        adc     edi,[esp+40]
        jo      .out
        jmp     .l
.out:   mov     eax,ebx
        pop     ebp edi esi ebx
        ret
.zero:  mov     esi,rt_msg_rstep
        jmp     rt_panic_value
;;; data rt_range_list
rt_msg_rstep     db 'range() arg 3 must not be zero',0

;;; code rt_syscall_list : rt_list_new rt_list_push rt_kd_int
rt_syscall_list:                ; eax = address of 6 dwords (eax..edi after the call) -> eax = list[int] (each sign-extended)
        push    ebx esi
        mov     esi,eax
        mov     eax,rt_kd_int
        call    rt_list_new
        mov     ebx,eax
        mov     ecx,6
@@:     push    ecx
        mov     eax,ebx
        call    rt_list_push
        mov     ecx,eax
        mov     eax,[esi]
        cdq
        mov     [ecx],eax
        mov     [ecx+4],edx
        add     esi,4
        pop     ecx
        dec     ecx
        jnz     @b
        mov     eax,ebx
        pop     esi ebx
        ret

;;; code rt_eq_q
rt_eq_q:                        ; two qwords
        mov     ecx,[eax]
        cmp     ecx,[edx]
        jne     .no
        mov     ecx,[eax+4]
        cmp     ecx,[edx+4]
        jne     .no
        mov     eax,1
        ret
.no:    xor     eax,eax
        ret

;;; code rt_cmp_i64
rt_cmp_i64:
        mov     ecx,[eax+4]
        cmp     ecx,[edx+4]
        jl      .lt
        jg      .gt
        mov     ecx,[eax]
        cmp     ecx,[edx]
        jb      .lt
        ja      .gt
        xor     eax,eax
        ret
.lt:    or      eax,-1
        ret
.gt:    mov     eax,1
        ret

;;; code rt_hash_i64 : rt_hash_int
rt_hash_i64:
        mov     edx,[eax+4]
        mov     eax,[eax]
        jmp     rt_hash_int

;;; code rt_repr_i64 : rt_sb_int
rt_repr_i64:
        mov     edx,[eax+4]
        mov     eax,[eax]
        jmp     rt_sb_int

