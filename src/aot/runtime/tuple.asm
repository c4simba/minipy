; ---------------------------------------------------------------- tuples
; tuple: +8 its type TD (+0 items, +4 their bytes, then per item: its kd, its
; offset in the tuple), +12 the items

;;; code rt_tuple_new : rt_alloc rt_tuple_free
rt_tuple_new:                   ; eax = TD -> eax = new tuple (items zero)
        push    eax
        mov     eax,[eax+4]
        add     eax,12
        call    rt_alloc
        pop     edx
        mov     dword [eax],1
        mov     dword [eax+4],rt_tuple_free
        mov     [eax+8],edx
if defined RT_GC
        push    esi
        mov     ecx,[edx]       ; an item of a CYC type: CYC
.cyc:   dec     ecx
        js      .ok
        mov     esi,[edx+8+ecx*8]
        test    byte [esi+4],8
        jz      .cyc
        or      byte [eax-3],1
.ok:    pop     esi
end if
        ret

;;; code rt_tuple_free : rt_decref rt_free
rt_tuple_free:                  ; destroy routine of tuples: drop the reference items
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,[ebx+8]
        mov     edi,[esi]
        add     esi,8
.l:     test    edi,edi
        jz      .done
        mov     eax,[esi]
        test    byte [eax+4],1
        jz      .n
        mov     eax,[esi+4]
        mov     eax,[ebx+eax]
        call    rt_decref
.n:     add     esi,8
        dec     edi
        jmp     .l
.done:  mov     eax,ebx
        pop     edi esi ebx
        jmp     rt_free

;;; code rt_tuple_eq
rt_tuple_eq:                    ; eax = a, edx = b (tuples of one type) -> eax = 1 if equal
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,edx
        cmp     ebx,ebp
        je      .yes
        test    ebx,ebx
        jz      .no
        test    ebp,ebp
        jz      .no
        mov     esi,[ebx+8]
        mov     edi,[esi]
        add     esi,8
.l:     test    edi,edi
        jz      .yes
        mov     eax,[esi+4]
        lea     edx,[ebp+eax]
        add     eax,ebx
        mov     ecx,[esi]
        call    dword [ecx+8]
        test    eax,eax
        jz      .no
        add     esi,8
        dec     edi
        jmp     .l
.yes:   mov     eax,1
        pop     ebp edi esi ebx
        ret
.no:    xor     eax,eax
        pop     ebp edi esi ebx
        ret

;;; code rt_tuple_cmp
rt_tuple_cmp:                   ; eax = a, edx = b -> eax = -1, 0, 1: the first unequal items decide, then the lengths
        push    ebx esi edi ebp ; (a shorter tuple's items are of the longer one's first items' types)
        mov     ebx,eax
        mov     ebp,edx
        cmp     ebx,ebp
        je      .eq
        test    ebx,ebx
        jz      .lt
        test    ebp,ebp
        jz      .gt
        mov     esi,[ebx+8]
        mov     edi,[esi]
        mov     eax,[ebp+8]
        mov     eax,[eax]
        push    edi             ; [esp+4] len(a)
        push    eax             ; [esp] len(b)
        cmp     edi,eax
        jbe     .min
        mov     edi,eax
.min:   add     esi,8
.l:     test    edi,edi
        jz      .len
        mov     eax,[esi+4]
        lea     edx,[ebp+eax]
        add     eax,ebx
        mov     ecx,[esi]
        push    eax edx
        call    dword [ecx+8]
        pop     edx ecx
        test    eax,eax
        jz      .diff
        add     esi,8
        dec     edi
        jmp     .l
.diff:  mov     eax,ecx
        mov     ecx,[esi]
        call    dword [ecx+12]
        add     esp,8
        jmp     .out
.len:   pop     edx
        pop     eax
        sub     eax,edx
        jz      .out
        jl      .lt
        jmp     .gt
.lt:    or      eax,-1
        jmp     .out
.gt:    mov     eax,1
        jmp     .out
.eq:    xor     eax,eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_tuple_hash : rt_mul64
rt_tuple_hash:                  ; eax = tuple -> edx:eax = its hash, CPython's (xxHash of the items' hashes)
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,[ebx+8]
        mov     ebp,[esi]       ; items
        add     esi,8
        push    0x27D4EB2F      ; acc = XXPRIME_5
        push    0x165667C5
        mov     edi,ebp
.l:     test    edi,edi
        jz      .end
        mov     eax,[esi+4]
        add     eax,ebx
        mov     ecx,[esi]
        call    dword [ecx+16]  ; the item's hash
        mov     ecx,rt_xxp2
        call    rt_mul64
        add     [esp],eax       ; acc += hash * XXPRIME_2
        adc     [esp+4],edx
        mov     eax,[esp]       ; acc = acc rotated left by 31
        mov     edx,[esp+4]
        mov     ecx,eax
        shl     ecx,31
        push    edx
        shr     edx,1
        or      ecx,edx         ; lo = lo << 31 | hi >> 1
        pop     edx
        shl     edx,31
        shr     eax,1
        or      edx,eax         ; hi = hi << 31 | lo >> 1
        mov     eax,ecx
        mov     ecx,rt_xxp1
        call    rt_mul64        ; acc *= XXPRIME_1
        mov     [esp],eax
        mov     [esp+4],edx
        add     esi,8
        dec     edi
        jmp     .l
.end:   pop     eax
        pop     edx
        xor     ebp,0x1663B4B6  ; acc += items ^ (XXPRIME_5 ^ 3527539)
        add     eax,ebp
        adc     edx,0x27D4EB2F
        cmp     edx,-1
        jne     .out
        cmp     eax,-1
        jne     .out
        mov     eax,1546275796
        xor     edx,edx
.out:   pop     ebp edi esi ebx
        ret
;;; data rt_tuple_hash
align 4
rt_xxp1         dd 0x85EBCA87,0x9E3779B1
rt_xxp2         dd 0x27D4EB4F,0xC2B2AE3D

;;; code rt_tuple_at : rt_panic_index
rt_tuple_at:                    ; eax = tuple (items of one type), edx = index (negative: from the end) -> eax = address of the item
        test    eax,eax
        jz      rt_panic_index_t
        mov     ecx,[eax+8]
        push    ebx
        mov     ebx,[ecx]
        test    edx,edx
        jns     @f
        add     edx,ebx
@@:     cmp     edx,ebx
        pop     ebx
        jae     rt_panic_index_t
        mov     ecx,[ecx+8]     ; the items' kd
        imul    edx,[ecx]
        lea     eax,[eax+12+edx]
        ret

;;; code rt_tuple_eq_k : rt_tuple_eq
rt_tuple_eq_k:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_tuple_eq

;;; code rt_tuple_cmp_k : rt_tuple_cmp rt_cmp_none
rt_tuple_cmp_k:
        mov     eax,[eax]
        mov     edx,[edx]
        test    eax,eax
        jz      .ln
        test    edx,edx
        jnz     rt_tuple_cmp
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_tuple_hash_k : rt_tuple_hash rt_hash_none
rt_tuple_hash_k:
        mov     eax,[eax]
        test    eax,eax
        jz      rt_hash_none
        jmp     rt_tuple_hash

;;; code rt_tuple_repr_k : rt_sb_char rt_sb_cstr
rt_tuple_repr_k:                ; repr of a tuple (for messages)
        push    ebx esi edi
        mov     ebx,[eax]
        test    ebx,ebx
        jz      .none
        mov     al,'('
        call    rt_sb_char
        mov     esi,[ebx+8]
        mov     edi,[esi]
        add     esi,8
        push    edi
.l:     test    edi,edi
        jz      .end
        cmp     edi,[esp]
        je      @f
        mov     eax,rt_s_tsep
        call    rt_sb_cstr
@@:     mov     eax,[esi+4]
        add     eax,ebx
        mov     ecx,[esi]
        call    dword [ecx+20]
        add     esi,8
        dec     edi
        jmp     .l
.end:   pop     edi
        cmp     edi,1
        jne     @f
        mov     al,','
        call    rt_sb_char
@@:     mov     al,')'
        pop     edi esi ebx
        jmp     rt_sb_char
.none:  mov     eax,rt_s_tnone
        pop     edi esi ebx
        jmp     rt_sb_cstr
;;; data rt_tuple_repr_k
rt_s_tsep       db ', ',0
rt_s_tnone      db 'None',0

