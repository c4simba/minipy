; ---------------------------------------------------------------- ints <-> bytes
;;; code rt_bytes_list : rt_list_new rt_list_push rt_kd_int
rt_bytes_list:                  ; eax = bytes -> eax = list[int] of its bytes
        push    ebx esi edi
        mov     esi,eax
        mov     eax,rt_kd_int
        call    rt_list_new
        mov     ebx,eax
        mov     edi,[esi+8]
        add     esi,12
        add     edi,esi
.l:     cmp     esi,edi
        jae     .out
        mov     eax,ebx
        call    rt_list_push
        movzx   ecx,byte [esi]
        mov     [eax],ecx
        mov     dword [eax+4],0
        inc     esi
        jmp     .l
.out:   mov     eax,ebx
        pop     edi esi ebx
        ret

;;; code rt_byteorder : rt_str_eq rt_panic_value rt_static
rt_byteorder:                   ; eax = str -> eax = 0 "big" / 1 "little" (ValueError otherwise)
        push    eax
        mov     edx,rt_s_big
        call    rt_str_eq
        test    eax,eax
        jnz     .big
        mov     eax,[esp]
        mov     edx,rt_s_little
        call    rt_str_eq
        test    eax,eax
        jnz     .little
        mov     esi,rt_msg_byteorder
        jmp     rt_panic_value
.big:   pop     eax
        xor     eax,eax
        ret
.little:pop     eax
        mov     eax,1
        ret
;;; data rt_byteorder
align 4
rt_s_big        dd 0x40000000,rt_static,3
                db 'big',0
                dd 3
align 4
rt_s_little     dd 0x40000000,rt_static,6
                db 'little',0
                dd 6
rt_msg_byteorder db "byteorder must be either 'little' or 'big'",0

;;; code rt_int_from_bytes : rt_int_overflow
rt_int_from_bytes:              ; eax = bytes, edx = 1: little-endian, ecx = 1: signed -> edx:eax = int.from_bytes(...)
        push    ebx esi edi ebp
        mov     esi,eax
        mov     ebp,ecx         ; signed
        mov     ecx,[esi+8]     ; n
        lea     esi,[esi+12]
        sub     esp,8           ; [esp]: the low 8 bytes, little-endian
        mov     dword [esp],0
        mov     dword [esp+4],0
        xor     ebx,ebx         ; byte k (from the least significant)
.l:     cmp     ebx,ecx
        jae     .got
        mov     eax,ebx         ; where byte k is
        test    edx,edx
        jnz     @f
        mov     eax,ecx
        sub     eax,ebx
        dec     eax
@@:     movzx   eax,byte [esi+eax]
        cmp     ebx,8
        jae     .extra
        mov     [esp+ebx],al
        inc     ebx
        jmp     .l
.extra: mov     edi,0           ; beyond 8 bytes: 0x00 (or 0xFF when signed and negative)
        test    ebp,ebp
        jz      @f
        test    byte [esp+7],0x80
        jz      @f
        mov     edi,0xFF
@@:     cmp     eax,edi
        jne     .ovf
        inc     ebx
        jmp     .l
.got:   test    ebp,ebp
        jnz     .signed
        test    byte [esp+7],0x80       ; unsigned: at most 2**63 - 1
        jnz     .ovf
        jmp     .out
.signed:cmp     ecx,8
        jae     .out
        jecxz   .out
        test    byte [esp+ecx-1],0x80   ; sign-extend n < 8 bytes
        jz      .out
        mov     ebx,ecx
@@:     mov     byte [esp+ebx],0xFF
        inc     ebx
        cmp     ebx,8
        jb      @b
.out:   pop     eax edx
        pop     ebp edi esi ebx
        ret
.ovf:   jmp     rt_int_overflow

;;; code rt_int_to_bytes : rt_bytes_new rt_ovferr_text rt_panic_value
rt_int_to_bytes:                ; [esp+4] 1: signed, 1: little-endian, length, the int (8 bytes) -> eax = n.to_bytes(...)
        push    ebx esi edi ebp ; (args from [esp+20]: signed, little, length, int)
        mov     eax,[esp+32]
        mov     edx,[esp+36]
        mov     ecx,[esp+28]
        test    ecx,ecx
        js      .neglen
        cmp     dword [esp+20],0
        jne     .sig
        test    edx,edx         ; unsigned: not negative, below 2**(8 * length)
        js      .negun
        cmp     ecx,8
        jae     .fits
        call    .shift
        or      eax,edx
        jnz     .big
        jmp     .fits
.sig:   cmp     ecx,8
        jae     .fits
        jecxz   .zerolen
        dec     ecx             ; signed: v >> (8 * length - 1) is 0 or -1
        shl     ecx,3
        add     ecx,7
        call    .sar
        mov     ecx,eax
        and     ecx,edx
        cmp     ecx,-1
        je      .fits
        or      eax,edx
        jnz     .big
        jmp     .fits
.zerolen:
        or      eax,edx         ; length 0: only 0 fits
        jnz     .big
.fits:  mov     eax,[esp+28]
        call    rt_bytes_new
        mov     ebx,eax
        mov     ecx,[esp+28]
        xor     esi,esi         ; byte k from the least significant: 0..7 from the int, then its sign
.l:     cmp     esi,ecx
        jae     .done
        xor     eax,eax
        cmp     dword [esp+36],0
        jns     @f
        mov     al,0xFF
@@:     cmp     esi,8
        jae     @f
        mov     al,[esp+32+esi]
@@:     mov     edx,esi
        cmp     dword [esp+24],0
        jne     @f
        mov     edx,ecx
        sub     edx,esi
        dec     edx
@@:     mov     [ebx+12+edx],al
        inc     esi
        jmp     .l
.done:  mov     eax,ebx
        pop     ebp edi esi ebx
        ret
.shift: mov     ebx,ecx         ; edx:eax >>= 8 * ecx (ecx < 8), logical
        shl     ebx,3
.sh:    test    ebx,ebx
        jz      .shr
        shrd    eax,edx,1
        shr     edx,1
        dec     ebx
        jmp     .sh
.shr:   ret
.sar:   test    ecx,ecx         ; edx:eax >>= ecx, arithmetic
        jz      .sr
        shrd    eax,edx,1
        sar     edx,1
        dec     ecx
        jmp     .sar
.sr:    ret
.neglen:mov     esi,rt_msg_neglen
        jmp     rt_panic_value
.negun: mov     esi,rt_msg_negun
        jmp     rt_ovferr_text
.big:   mov     esi,rt_msg_toobig
        jmp     rt_ovferr_text
;;; data rt_int_to_bytes
rt_msg_neglen   db 'length argument must be non-negative',0
rt_msg_negun    db "can't convert negative int to unsigned",0
rt_msg_toobig   db 'int too big to convert',0

;;; code rt_int_bitlen
rt_int_bitlen:                  ; edx:eax = int -> eax = (abs(int)).bit_length()
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     mov     ecx,64
        test    edx,edx
        jnz     .hi
        mov     ecx,32
        mov     edx,eax
.hi:    test    edx,edx
        jz      .zero
@@:     test    edx,0x80000000
        jnz     .out
        shl     edx,1
        dec     ecx
        jmp     @b
.zero:  xor     ecx,ecx
.out:   mov     eax,ecx
        ret

;;; code rt_int_bitcount
rt_int_bitcount:                ; edx:eax = int -> eax = the ones in abs(int)
        test    edx,edx
        jns     @f
        neg     eax
        adc     edx,0
        neg     edx
@@:     xor     ecx,ecx
.l:     test    eax,1
        jz      @f
        inc     ecx
@@:     shrd    eax,edx,1
        shr     edx,1
        test    eax,eax
        jnz     .l
        test    edx,edx
        jnz     .l
        mov     eax,ecx
        ret

