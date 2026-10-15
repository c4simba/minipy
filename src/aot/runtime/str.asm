; ---------------------------------------------------------------- strings
; A str: +0 refcount, +4 destroy, +8 length in bytes, +12 the UTF-8 bytes, a
; NUL, then a dword: the length in code points (-1 until someone asks for it).
; Lengths, indexes and slices count code points, as in the interpreter: a
; byte that does not start a valid sequence is one code point of its own.

;;; code rt_str_new : rt_alloc rt_str_free
rt_str_new:                     ; eax = length -> eax = new str (refcount 1, bytes zeroed)
        push    eax
        add     eax,17
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_str_free
        mov     [eax+8],ecx
        mov     dword [eax+13+ecx],-1
        ret

;;; code rt_str_free : rt_free
rt_str_free:                    ; eax = str: destroy it (and forget it in the cursor cache)
        cmp     eax,[rt_str_cur]
        jne     @f
        mov     dword [rt_str_cur],0
@@:     cmp     eax,[rt_str_cur+12]
        jne     @f
        mov     dword [rt_str_cur+12],0
@@:     jmp     rt_free
;;; bss rt_str_free
rt_str_cur      rd 6            ; two (str, code point index, byte offset): where the last indexing went

;;; code rt_str_concat : rt_str_new
rt_str_concat:                  ; eax = a, edx = b -> eax = a + b (new reference)
        push    ebx esi edi
        mov     esi,eax
        mov     edi,edx
        xor     ecx,ecx
        test    esi,esi
        jz      @f
        mov     ecx,[esi+8]
@@:     xor     edx,edx
        test    edi,edi
        jz      @f
        mov     edx,[edi+8]
@@:     test    edx,edx
        jnz     @f
        mov     eax,esi         ; b is empty: a itself
        jmp     .keep
@@:     test    ecx,ecx
        jnz     .both
        mov     eax,edi         ; a is empty: b itself
.keep:  test    eax,eax
        jz      .out
        inc     dword [eax]
.out:   pop     edi esi ebx
        ret
.both:  push    ecx edx
        lea     eax,[ecx+edx]
        call    rt_str_new
        pop     edx ecx
        mov     ebx,eax
        push    edi
        lea     edi,[ebx+12]
        add     esi,12
        rep     movsb
        pop     esi
        add     esi,12
        mov     ecx,edx
        rep     movsb
        mov     eax,ebx
        pop     edi esi ebx
        ret

;;; code rt_str_eq
rt_str_eq:                      ; eax = a, edx = b (0: None) -> eax = 1 if equal
        push    esi edi
        mov     esi,eax
        mov     edi,edx
        cmp     esi,edi
        je      .yes            ; the same str (or both None)
        test    esi,esi
        jz      .no
        test    edi,edi
        jz      .no
        mov     eax,[esi+8]
        mov     ecx,[edi+8]
        cmp     eax,ecx
        jne     .no
        test    ecx,ecx
        jz      .yes
        add     esi,12
        add     edi,12
        repe    cmpsb
        jne     .no
.yes:   mov     eax,1
        pop     edi esi
        ret
.no:    xor     eax,eax
        pop     edi esi
        ret

;;; code rt_str_cmp
rt_str_cmp:                     ; eax = a, edx = b -> eax = -1, 0, 1 (UTF-8 bytes sort like code points)
        push    ebx esi edi
        mov     esi,eax
        mov     edi,edx
        xor     eax,eax
        test    esi,esi
        jz      @f
        mov     eax,[esi+8]
        add     esi,12
@@:     xor     edx,edx
        test    edi,edi
        jz      @f
        mov     edx,[edi+8]
        add     edi,12
@@:     mov     ebx,eax
        mov     ecx,eax
        cmp     ecx,edx
        jbe     @f
        mov     ecx,edx
@@:     test    ecx,ecx
        jz      .len
        repe    cmpsb
        ja      .gt
        jb      .lt
.len:   cmp     ebx,edx
        ja      .gt
        jb      .lt
        xor     eax,eax
        jmp     .out
.gt:    mov     eax,1
        jmp     .out
.lt:    or      eax,-1
.out:   pop     edi esi ebx
        ret

;;; data rt_chars : rt_static
align 4
rt_chars:                       ; every ASCII one-character string, static (32 bytes apart)
repeat 128
        dd      0x40000000, rt_static, 1
        db      %-1, 0
        dd      1
        db      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
end repeat

;;; code rt_u8_next
rt_u8_next:                     ; esi = position, edi = end -> eax = the code point there, esi past it (rt_bmode: the byte there)
        movzx   eax,byte [esi]
        inc     esi
        cmp     al,0xC0
        jb      .out            ; ASCII, or a stray continuation byte
        cmp     byte [rt_bmode],0
        jne     .out
        cmp     al,0xF8
        jae     .out
        push    ebx ecx edx
        mov     ecx,1           ; continuation bytes
        mov     edx,0x1F
        cmp     al,0xE0
        jb      @f
        inc     ecx
        mov     edx,0x0F
        cmp     al,0xF0
        jb      @f
        inc     ecx
        mov     edx,0x07
@@:     mov     ebx,edi
        sub     ebx,esi
        cmp     ebx,ecx
        jb      .one            ; cut short by the end
        and     edx,eax         ; the bits of the first byte
        mov     ebx,esi
.cont:  movzx   eax,byte [ebx]
        xor     al,0x80
        test    al,0xC0
        jnz     .one            ; not a continuation byte
        shl     edx,6
        or      edx,eax
        inc     ebx
        dec     ecx
        jnz     .cont
        mov     esi,ebx
        mov     eax,edx
        pop     edx ecx ebx
.out:   ret
.one:   movzx   eax,byte [esi-1]    ; an invalid sequence: its first byte alone
        pop     edx ecx ebx
        ret
;;; bss rt_u8_next
rt_bmode        rd 1            ; 1 while a bytes method runs a str routine: bytes, ASCII whitespace

;;; code rt_u8_put
rt_u8_put:                      ; eax = code point, edi = where -> its UTF-8 there, edi past it
        cmp     eax,0x80
        jae     @f
        stosb
        ret
@@:     push    edx
        mov     edx,eax
        cmp     eax,0x800
        jae     @f
        shr     eax,6
        or      al,0xC0
        stosb
        jmp     .last
@@:     cmp     eax,0x10000
        jae     @f
        shr     eax,12
        or      al,0xE0
        stosb
        jmp     .mid
@@:     shr     eax,18
        or      al,0xF0
        stosb
        mov     eax,edx
        shr     eax,12
        and     al,0x3F
        or      al,0x80
        stosb
.mid:   mov     eax,edx
        shr     eax,6
        and     al,0x3F
        or      al,0x80
        stosb
.last:  mov     eax,edx
        and     al,0x3F
        or      al,0x80
        stosb
        pop     edx
        ret

;;; code rt_sb_cp : rt_sb_need rt_u8_put
rt_sb_cp:                       ; eax = code point: append its UTF-8
        push    edi
        push    eax
        mov     eax,4
        call    rt_sb_need
        mov     edi,eax
        pop     eax
        push    edi
        call    rt_u8_put
        pop     eax
        sub     edi,eax
        sub     edi,4
        add     [rt_sb_len],edi ; give back what it did not use
        pop     edi
        ret

;;; code rt_str_cplen : rt_u8_next
rt_str_cplen:                   ; eax = str (0: None) -> eax = its length in code points (nothing else changes)
        test    eax,eax
        jz      .out
        push    ecx
        mov     ecx,[eax+8]
        mov     ecx,[eax+13+ecx]
        cmp     ecx,-1
        je      .count
        mov     eax,ecx
        pop     ecx
.out:   ret
.count: push    ebx esi edi
        push    eax
        mov     ecx,[eax+8]
        lea     esi,[eax+12]
        lea     edi,[esi+ecx]
        xor     ebx,ebx
.l:     cmp     esi,edi
        jae     .done
        inc     ebx
        cmp     byte [esi],0xC0
        jae     @f
        inc     esi
        jmp     .l
@@:     call    rt_u8_next
        jmp     .l
.done:  pop     eax
        mov     ecx,[eax+8]
        mov     [eax+13+ecx],ebx
        mov     eax,ebx
        pop     edi esi ebx
        pop     ecx
        ret

;;; code rt_str_off : rt_str_cplen rt_u8_next rt_str_free
rt_str_off:                     ; eax = str, edx = code point index (0..length) -> eax = its byte offset (ecx changed)
        push    ebx esi edi
        mov     ebx,eax
        call    rt_str_cplen
        cmp     eax,[ebx+8]
        jne     .wide
        mov     eax,edx         ; one byte each
        pop     edi esi ebx
        ret
.wide:  xor     ecx,ecx         ; from code point ecx at byte esi: the start, or where a cursor is
        xor     esi,esi
        mov     edi,rt_str_cur
        cmp     ebx,[edi]
        je      @f
        add     edi,12
        cmp     ebx,[edi]
        jne     .walk
@@:     cmp     edx,[edi+4]
        jb      .walk
        mov     ecx,[edi+4]
        mov     esi,[edi+8]
.walk:  push    edx
        sub     edx,ecx         ; code points to go
        lea     esi,[ebx+12+esi]
        mov     edi,[ebx+8]
        lea     edi,[ebx+12+edi]
        test    edx,edx
        jz      .there
.step:  cmp     byte [esi],0xC0
        jae     .multi
        inc     esi
        dec     edx
        jnz     .step
        jmp     .there
.multi: call    rt_u8_next
        dec     edx
        jnz     .step
.there: pop     edx
        lea     eax,[ebx+12]
        sub     esi,eax
        cmp     ebx,[rt_str_cur]
        je      @f
        mov     eax,[rt_str_cur]        ; the other cursor goes second
        mov     [rt_str_cur+12],eax
        mov     eax,[rt_str_cur+4]
        mov     [rt_str_cur+16],eax
        mov     eax,[rt_str_cur+8]
        mov     [rt_str_cur+20],eax
@@:     mov     [rt_str_cur],ebx
        mov     [rt_str_cur+4],edx
        mov     [rt_str_cur+8],esi
        mov     eax,esi
        pop     edi esi ebx
        ret

;;; code rt_str_cpidx : rt_str_cplen rt_u8_next
rt_str_cpidx:                   ; eax = str, edx = byte offset -> eax = the index of the code point there
        push    ebx esi edi ebp
        mov     ebx,eax
        call    rt_str_cplen
        cmp     eax,[ebx+8]
        mov     eax,edx
        je      .out
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
        lea     ebp,[esi+edx]
        xor     edx,edx
.l:     cmp     esi,ebp
        jae     .done
        call    rt_u8_next
        inc     edx
        jmp     .l
.done:  mov     eax,edx
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_cp_char : rt_chars rt_str_new rt_u8_put rt_alloc rt_free rt_static
rt_cp_char:                     ; eax = code point -> eax = the one-character str (static: shared, never freed)
        cmp     eax,0x80
        jae     .wide
        shl     eax,5
        add     eax,rt_chars
        ret
.wide:  push    ebx esi edi
        mov     ebx,eax
.find:  mov     esi,[rt_cpc_tab]        ; open addressing: (str, code point) pairs
        test    esi,esi
        jz      .grow
        mov     eax,ebx
        imul    eax,eax,0x9E3779B1
        shr     eax,12
        mov     ecx,[rt_cpc_mask]
.probe: and     eax,ecx
        mov     edx,[esi+eax*8]
        test    edx,edx
        jz      .miss
        cmp     [esi+eax*8+4],ebx
        je      .hit
        inc     eax
        jmp     .probe
.hit:   mov     eax,edx
        pop     edi esi ebx
        ret
.miss:  mov     edx,[rt_cpc_n]
        inc     edx
        add     edx,edx
        cmp     edx,ecx
        ja      .grow                   ; half full
        push    eax
        mov     eax,1
        cmp     ebx,0x800
        jb      @f
        inc     eax
        cmp     ebx,0x10000
        jb      @f
        inc     eax
@@:     inc     eax
        call    rt_str_new
        mov     dword [eax],0x40000000
        mov     dword [eax+4],rt_static
        mov     ecx,[eax+8]
        mov     dword [eax+13+ecx],1
        lea     edi,[eax+12]
        push    eax
        mov     eax,ebx
        call    rt_u8_put
        pop     edx
        pop     eax
        mov     [esi+eax*8],edx
        mov     [esi+eax*8+4],ebx
        inc     dword [rt_cpc_n]
        mov     eax,edx
        pop     edi esi ebx
        ret
.grow:  mov     eax,[rt_cpc_mask]       ; twice the slots (64 at first), the pairs moved over
        lea     eax,[eax*2+2]
        cmp     eax,64
        jae     @f
        mov     eax,64
@@:     push    eax
        shl     eax,3
        call    rt_alloc
        pop     ecx
        dec     ecx
        mov     edi,eax
        mov     esi,[rt_cpc_tab]
        mov     [rt_cpc_tab],edi
        xchg    ecx,[rt_cpc_mask]
        test    esi,esi
        jz      .find
        inc     ecx                     ; the old slots
        push    esi
.move:  mov     edx,[esi]
        test    edx,edx
        jz      .moved
        mov     eax,[esi+4]
        imul    eax,eax,0x9E3779B1
        shr     eax,12
.mp:    and     eax,[rt_cpc_mask]
        cmp     dword [edi+eax*8],0
        je      @f
        inc     eax
        jmp     .mp
@@:     mov     [edi+eax*8],edx
        mov     edx,[esi+4]
        mov     [edi+eax*8+4],edx
.moved: add     esi,8
        dec     ecx
        jnz     .move
        pop     eax
        call    rt_free
        jmp     .find
;;; bss rt_cp_char
rt_cpc_tab      rd 1
rt_cpc_mask     rd 1
rt_cpc_n        rd 1

