; ---------------------------------------------------------------- 64-bit integers
; Python's int in compiled code: 64 bits, in edx:eax (in memory: the low dword
; first). A result that does not fit raises OverflowError, as in the
; interpreter. Binary operators take the left operand on the stack ([esp+4])
; and the right one in edx:eax; the caller drops the left one.

;;; code rt_ovferr_text : rt_panic
rt_ovferr_text:                 ; esi = message (C text): raise OverflowError
if defined rt_throw
        mov     eax,VTX_OverflowError
        mov     edx,DTX_OverflowError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_ovferr
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_ovferr_text
rt_s_ovferr     db 'OverflowError: ',0

;;; code rt_int_overflow : rt_ovferr_text
rt_int_overflow:                ; an int result too big
        mov     esi,rt_msg_iovf
        jmp     rt_ovferr_text
;;; data rt_int_overflow
rt_msg_iovf     db 'integer overflow (ints are 64-bit)',0

;;; code rt_i32sat
rt_i32sat:                      ; edx:eax -> eax = the same int as 32 bits, or the nearest 32-bit one (indices, counts)
        push    ecx
        mov     ecx,eax
        sar     ecx,31
        cmp     ecx,edx
        je      .out
        mov     eax,0x7FFFFFFF
        test    edx,edx
        jns     .out
        mov     eax,0x80000000
.out:   pop     ecx
        ret

;;; code rt_ineg : rt_int_overflow
rt_ineg:                        ; edx:eax = -edx:eax
        cmp     edx,0x80000000
        jne     @f
        test    eax,eax
        jz      rt_int_overflow
@@:     neg     eax
        adc     edx,0
        neg     edx
        ret

;;; code rt_iabs : rt_ineg
rt_iabs:                        ; edx:eax = abs(edx:eax)
        test    edx,edx
        js      rt_ineg
        ret

;;; code rt_imul : rt_int_overflow
rt_imul:                        ; [esp+4] a, edx:eax b -> edx:eax = a * b
        push    ebx esi edi ebp
        mov     ebx,[esp+20]    ; |a| -> esi:ebx, ebp = the result's sign
        mov     esi,[esp+24]
        xor     ebp,ebp
        test    esi,esi
        jns     @f
        neg     ebx
        adc     esi,0
        neg     esi
        xor     ebp,1
@@:     test    edx,edx         ; |b| -> edx:eax
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
        xor     ebp,1
@@:     test    esi,esi
        jz      .alo
        test    edx,edx
        jnz     .ovf            ; both at least 2**32
        mov     ecx,eax         ; |a| has a high part, |b| does not
        mov     eax,esi
        mul     ecx
        test    edx,edx
        jnz     .ovf
        mov     edi,eax
        mov     eax,ebx
        mul     ecx
        add     edx,edi
        jc      .ovf
        jmp     .sign
.alo:   mov     edi,eax         ; |a| < 2**32
        mov     eax,edx
        mul     ebx
        test    edx,edx
        jnz     .ovf
        mov     ecx,eax
        mov     eax,edi
        mul     ebx
        add     edx,ecx
        jc      .ovf
.sign:  test    edx,edx         ; the product (unsigned) fits if < 2**63, or is 2**63 and negative
        js      .big
        test    ebp,ebp
        jz      .out
        neg     eax
        adc     edx,0
        neg     edx
.out:   pop     ebp edi esi ebx
        ret
.big:   test    ebp,ebp
        jz      .ovf
        cmp     edx,0x80000000
        jne     .ovf
        test    eax,eax
        jz      .out
.ovf:   pop     ebp edi esi ebx
        jmp     rt_int_overflow

;;; code rt_udivmod
rt_udivmod:                     ; edx:eax / ecx:ebx (unsigned, not 0) -> edx:eax = quotient, ecx:ebx = remainder
        test    ecx,ecx
        jnz     .big
        push    esi             ; a 32-bit divisor: two divisions
        mov     esi,eax
        mov     eax,edx
        xor     edx,edx
        div     ebx
        mov     ecx,eax
        mov     eax,esi
        div     ebx
        mov     ebx,edx
        mov     edx,ecx
        xor     ecx,ecx
        pop     esi
        ret
.big:   push    esi edi ebp     ; shift and subtract
        mov     ebp,64
        xor     esi,esi         ; the remainder: edi:esi
        xor     edi,edi
.l:     shld    edi,esi,1
        shld    esi,edx,1
        shld    edx,eax,1
        shl     eax,1
        cmp     edi,ecx
        jb      .n
        ja      .sub
        cmp     esi,ebx
        jb      .n
.sub:   sub     esi,ebx
        sbb     edi,ecx
        or      eax,1
.n:     dec     ebp
        jnz     .l
        mov     ebx,esi
        mov     ecx,edi
        pop     ebp edi esi
        ret

;;; code rt_idivc
; a // c and a % c for a constant 0 < c < 2**31 (the peephole optimizer's for
; rt_idivmod with such a divisor): one or two divisions, no sign of c to mind
rt_idivc:                       ; edx:eax a, ecx c -> edx:eax = a // c, ecx = a % c
        test    edx,edx
        js      .neg
        cmp     edx,ecx
        jae     .two
        div     ecx             ; a < c * 2**32: the quotient fits in eax
        mov     ecx,edx
        xor     edx,edx
        ret
.two:   push    ebx
        mov     ebx,eax
        mov     eax,edx
        xor     edx,edx
        div     ecx             ; the high word: eax its quotient, edx what is left
        xchg    eax,ebx
        div     ecx             ; (what is left, the low word) / c: below 2**32
        mov     ecx,edx
        mov     edx,ebx
        pop     ebx
        ret
.neg:   not     eax             ; a < 0: with a' = -a - 1 = ~a (not negative),
        not     edx             ; a // c = ~(a' // c), a % c = c - 1 - a' % c
        push    ecx
        call    rt_idivc
        not     eax
        not     edx
        neg     ecx
        add     ecx,[esp]
        dec     ecx
        add     esp,4
        ret

;;; code rt_idivmod : rt_udivmod rt_panic_zero rt_int_overflow rt_scratch rt_idivc
; Python's floor division: the quotient rounds toward minus infinity, the
; remainder has the divisor's sign
rt_idivmod:                     ; [esp+4] a, edx:eax b -> edx:eax = a // b, rt_scratch = a % b
        push    ebx esi edi ebp
        push    edx
        push    eax             ; [esp] b
        mov     ebx,eax
        mov     ecx,edx
        or      eax,edx
        jz      .zero
        mov     eax,[esp+28]    ; a
        mov     edx,[esp+32]
        cmp     ecx,-1          ; -2**63 // -1
        jne     @f
        cmp     ebx,-1
        jne     @f
        cmp     edx,0x80000000
        jne     @f
        test    eax,eax
        jz      .ovf
@@:     mov     esi,edx         ; the signs
        mov     edi,ecx
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     test    ecx,ecx
        jns     @f
        neg     ebx
        adc     ecx,0
        neg     ecx
@@:     call    rt_udivmod
        test    esi,esi         ; the remainder of |a| / |b| takes a's sign
        jns     @f
        neg     ebx
        adc     ecx,0
        neg     ecx
@@:     xor     esi,edi
        jns     .done
        neg     eax             ; signs differ: a negative quotient ...
        adc     edx,0
        neg     edx
        mov     ebp,ebx
        or      ebp,ecx
        jz      .done
        sub     eax,1           ; ... and, not exact, one less (the remainder + b)
        sbb     edx,0
        add     ebx,[esp]
        adc     ecx,[esp+4]
.done:  mov     [rt_scratch],ebx
        mov     [rt_scratch+4],ecx
        add     esp,8
        pop     ebp edi esi ebx
        ret
.zero:  add     esp,8
        pop     ebp edi esi ebx
        jmp     rt_panic_zero
.ovf:   add     esp,8
        pop     ebp edi esi ebx
        jmp     rt_int_overflow

;;; code rt_ipow : rt_imul rt_panic_value
rt_ipow:                        ; [esp+4] base, edx:eax exponent (>= 0) -> edx:eax = base ** exponent
        test    edx,edx
        js      .neg
        push    ebx esi edi ebp
        mov     esi,eax         ; the exponent: edi:esi
        mov     edi,edx
        push    dword [esp+24]  ; [esp+8] base (squared as needed)
        push    dword [esp+24]
        push    0               ; [esp] result = 1
        push    1
.l:     mov     eax,esi
        or      eax,edi
        jz      .done
        test    esi,1
        jz      .sq
        push    dword [esp+4]   ; result *= base
        push    dword [esp+4]
        mov     eax,[esp+16]
        mov     edx,[esp+20]
        call    rt_imul
        add     esp,8
        mov     [esp],eax
        mov     [esp+4],edx
.sq:    shrd    esi,edi,1
        shr     edi,1
        mov     eax,esi
        or      eax,edi
        jz      .done
        push    dword [esp+12]  ; base *= base
        push    dword [esp+12]
        mov     eax,[esp+16]
        mov     edx,[esp+20]
        call    rt_imul
        add     esp,8
        mov     [esp+8],eax
        mov     [esp+12],edx
        jmp     .l
.done:  pop     eax
        pop     edx
        add     esp,8
        pop     ebp edi esi ebx
        ret
.neg:   mov     esi,rt_msg_ipowneg
        jmp     rt_panic_value
;;; data rt_ipow
rt_msg_ipowneg  db 'negative exponent: int ** int with a negative exponent is a float (not in compiled code; use float ** int)',0

;;; code rt_sar64
rt_sar64:                       ; edx:eax >>= cl (0..63), arithmetic
        cmp     cl,32
        jae     @f
        shrd    eax,edx,cl
        sar     edx,cl
        ret
@@:     mov     eax,edx
        sar     edx,31
        sub     cl,32
        sar     eax,cl
        ret

;;; code rt_ishl : rt_shl64 rt_sar64 rt_int_overflow rt_panic_value
rt_ishl:                        ; [esp+4] a, edx:eax n -> edx:eax = a << n
        test    edx,edx
        js      .neg
        push    ebx esi
        mov     ebx,[esp+12]
        mov     esi,[esp+16]
        mov     ecx,eax
        mov     eax,ebx
        or      eax,esi
        jz      .zero
        test    edx,edx
        jnz     .ovf
        cmp     ecx,63
        ja      .ovf
        mov     eax,ebx
        mov     edx,esi
        push    ecx
        call    rt_shl64
        pop     ecx
        push    edx
        push    eax
        call    rt_sar64        ; shifted back: the same value unless bits were lost
        cmp     eax,ebx
        jne     .ovf2
        cmp     edx,esi
        jne     .ovf2
        pop     eax
        pop     edx
        pop     esi ebx
        ret
.zero:  xor     eax,eax
        xor     edx,edx
        pop     esi ebx
        ret
.ovf2:  add     esp,8
.ovf:   pop     esi ebx
        jmp     rt_int_overflow
.neg:   mov     esi,rt_msg_negshift
        jmp     rt_panic_value
;;; data rt_ishl
rt_msg_negshift db 'negative shift count',0

;;; code rt_isar : rt_sar64 rt_ishl
rt_isar:                        ; [esp+4] a, edx:eax n -> edx:eax = a >> n
        test    edx,edx
        js      .neg
        mov     ecx,eax
        jnz     .all
        cmp     eax,63
        jbe     @f
.all:   mov     eax,[esp+8]     ; every bit shifted out: the sign
        sar     eax,31
        mov     edx,eax
        ret
@@:     mov     eax,[esp+4]
        mov     edx,[esp+8]
        jmp     rt_sar64
.neg:   mov     esi,rt_msg_negshift
        jmp     rt_panic_value

