; ---------------------------------------------------------------- character properties
;;; code rt_cp_props : rt_unicode
rt_cp_props:                    ; eax = code point -> eax = class bits (see rt_unicode), ecx = upper, edx = lower (simple mappings)
        push    ebx esi edi
        xor     esi,esi
        mov     edi,[rt_unicode_n]
        dec     edi
.l:     cmp     esi,edi
        jg      .none
        lea     ebx,[esi+edi]
        shr     ebx,1
        lea     ecx,[ebx*4+ebx]
        lea     ecx,[rt_unicode+ecx*4]
        cmp     eax,[ecx]
        jb      .below
        cmp     eax,[ecx+4]
        ja      .above
        mov     edx,eax
        sub     edx,[ecx]
        and     edx,1
        lea     edx,[edx*2+edx]
        lea     esi,[ecx+8+edx*2]
        movzx   ecx,word [esi+2]        ; the mappings stay in the BMP: mod 0x10000
        test    ecx,ecx
        jz      @f
        add     ecx,eax
        and     ecx,0xFFFF
        jmp     .lo
@@:     mov     ecx,eax
.lo:    movzx   edx,word [esi+4]
        test    edx,edx
        jz      @f
        add     edx,eax
        and     edx,0xFFFF
        jmp     .bits
@@:     mov     edx,eax
.bits:  movzx   eax,word [esi]
        pop     edi esi ebx
        ret
.below: lea     edi,[ebx-1]
        jmp     .l
.above: lea     esi,[ebx+1]
        jmp     .l
.none:  mov     ecx,eax
        mov     edx,eax
        xor     eax,eax
        pop     edi esi ebx
        ret

;;; code rt_cp_isspace : rt_u8_next
rt_cp_isspace:                  ; eax = code point -> ZF set when it is whitespace (nothing else changes; rt_bmode: ASCII whitespace)
        cmp     eax,' '
        je      .r
        cmp     eax,9
        jb      .no
        cmp     eax,13
        jbe     .yes
        cmp     byte [rt_bmode],0
        jne     .no
        cmp     eax,0x1C
        jb      .no
        cmp     eax,0x1F
        jbe     .yes
        cmp     eax,0x85
        je      .r
        cmp     eax,0xA0
        je      .r
        cmp     eax,0x1680
        je      .r
        cmp     eax,0x2000
        jb      .no
        cmp     eax,0x200A
        jbe     .yes
        cmp     eax,0x2028
        je      .r
        cmp     eax,0x2029
        je      .r
        cmp     eax,0x202F
        je      .r
        cmp     eax,0x205F
        je      .r
        cmp     eax,0x3000
.r:     ret
.yes:   cmp     eax,eax
        ret
.no:    test    esp,esp
        ret

;;; code rt_cp_printable : rt_noprint
rt_cp_printable:                ; eax = code point -> eax = 1 when repr() shows it as it is (nothing else changes)
        cmp     eax,0x80
        jae     .wide
        cmp     eax,0x20
        jb      .no
        cmp     eax,0x7F
        je      .no
.yes:   mov     eax,1
        ret
.no:    xor     eax,eax
        ret
.wide:  push    ebx esi edi
        xor     esi,esi
        mov     edi,[rt_noprint_n]
        dec     edi
.l:     cmp     esi,edi
        jg      .out
        lea     ebx,[esi+edi]
        shr     ebx,1
        cmp     eax,[rt_noprint+ebx*8]
        jb      .below
        cmp     eax,[rt_noprint+ebx*8+4]
        ja      .above
        pop     edi esi ebx
        jmp     .no
.below: lea     edi,[ebx-1]
        jmp     .l
.above: lea     esi,[ebx+1]
        jmp     .l
.out:   pop     edi esi ebx
        jmp     .yes

;;; code rt_uc_special : rt_ucspecial
rt_uc_special:                  ; eax = code point, dl = kind (U L T F) -> eax = its mapping (NUL-terminated) when not one code point to one, else 0
        cmp     eax,0xB5
        jb      .no
        push    esi
        mov     esi,rt_ucspecial
.l:     mov     ecx,[esi]
        test    ecx,ecx
        jz      .none
        cmp     ecx,eax
        jne     .skip
        cmp     [esi+4],dl
        jne     .skip
        lea     eax,[esi+5]
        pop     esi
        ret
.skip:  add     esi,5
@@:     cmp     byte [esi],0
        je      @f
        inc     esi
        jmp     @b
@@:     inc     esi
        jmp     .l
.none:  pop     esi
.no:    xor     eax,eax
        ret

;;; code rt_final_sigma : rt_u8_next rt_cp_props
rt_final_sigma:                 ; eax = str, ecx, edx = where a capital sigma starts and ends (byte offsets) -> eax = the small sigma it lowers to
        push    ebx esi edi ebp
        lea     ebx,[eax+12]
        mov     edi,[eax+8]
        add     edi,ebx
        lea     ebp,[ebx+edx]
        add     ecx,ebx
.back:  cmp     ecx,ebx         ; the one before (case-ignorable ones skipped) must be cased
        jbe     .no
        dec     ecx
@@:     cmp     ecx,ebx
        jbe     @f
        mov     al,[ecx]
        and     al,0xC0
        cmp     al,0x80
        jne     @f
        dec     ecx
        jmp     @b
@@:     mov     esi,ecx
        push    ecx
        call    rt_u8_next
        call    rt_cp_props
        pop     ecx
        test    eax,512
        jnz     .back
        test    eax,128
        jz      .no
        mov     esi,ebp         ; and the one after must not be
.fwd:   cmp     esi,edi
        jae     .yes
        call    rt_u8_next
        call    rt_cp_props
        test    eax,512
        jnz     .fwd
        test    eax,128
        jnz     .no
.yes:   mov     eax,0x3C2
        jmp     .out
.no:    mov     eax,0x3C3
.out:   pop     ebp edi esi ebx
        ret

