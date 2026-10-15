; ---------------------------------------------------------------- strings: case, tests, strip, replace, split, join
;;; code rt_str_case : rt_u8_next rt_cp_props rt_sb_cp rt_sb_cstr rt_sb_take rt_uc_special rt_final_sigma
rt_str_case:                    ; eax = s, edx = 0 upper / 1 lower / 2 swapcase / 3 title / 4 capitalize / 5 casefold -> eax = new str
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        push    eax             ; [esp] s, [esp+4] the mark
        mov     ebx,edx
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
        xor     ebp,ebp         ; bit 0: the previous one was cased (title), bit 1: past the first (capitalize)
.l:     cmp     esi,edi
        jae     .done
        push    esi
        call    rt_u8_next
        push    eax
        call    rt_cp_props     ; eax = classes, ecx = upper, edx = lower
        push    ecx edx         ; [esp] lower, [esp+4] upper, [esp+8] the code point, [esp+12] where it starts, [esp+16] s
        mov     dl,'U'          ; dl: which mapping - U upper, L lower, T title, F casefold
        test    ebx,ebx
        jz      .kind
        mov     dl,'L'
        cmp     ebx,1
        je      .kind
        mov     dl,'F'
        cmp     ebx,5
        je      .kind
        cmp     ebx,2
        jne     .tc
        mov     dl,'L'          ; swapcase
        test    eax,32
        jnz     .kind
        mov     dl,'U'
        test    eax,64
        jnz     .kind
        mov     eax,[esp+8]     ; neither: itself
        jmp     .put
.tc:    mov     dl,'T'
        cmp     ebx,3
        jne     .cap
        test    ebp,1           ; title: lower after a cased one
        jz      @f
        mov     dl,'L'
@@:     and     ebp,-2
        test    eax,128
        jz      .kind
        or      ebp,1
        jmp     .kind
.cap:   test    ebp,2           ; capitalize: title first, then lower
        jz      @f
        mov     dl,'L'
@@:     or      ebp,2
.kind:  mov     eax,[esp+8]
        cmp     dl,'L'
        jne     .spec
        cmp     eax,0x3A3
        jne     .spec
        mov     eax,[esp+16]    ; a capital sigma lowers to a final one at the end of a word
        mov     ecx,[esp+12]
        sub     ecx,eax
        sub     ecx,12
        mov     edx,esi
        sub     edx,eax
        sub     edx,12
        call    rt_final_sigma
        jmp     .put
.spec:  push    edx
        call    rt_uc_special
        pop     edx
        test    eax,eax
        jnz     .text
        cmp     dl,'T'          ; title is upper, casefold lower, unless special
        jne     @f
        mov     dl,'U'
        jmp     .again
@@:     cmp     dl,'F'
        jne     .simple
        mov     dl,'L'
.again: mov     eax,[esp+8]
        push    edx
        call    rt_uc_special
        pop     edx
        test    eax,eax
        jnz     .text
.simple:mov     eax,[esp]
        cmp     dl,'U'
        jne     .put
        mov     eax,[esp+4]
.put:   call    rt_sb_cp
        jmp     .next
.text:  call    rt_sb_cstr
.next:  add     esp,16
        jmp     .l
.done:  add     esp,4
        pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret

;;; code rt_str_is : rt_u8_next rt_cp_props rt_cp_printable
rt_str_is:                      ; eax = s, edx = 0 isalpha / 1 isdigit / 2 isalnum / 3 isspace / 4 islower / 5 isupper / 6 isdecimal / 7 isascii / 8 isnumeric / 9 istitle / 10 isprintable / 11 isidentifier -> eax = bool
        push    ebx esi edi ebp
        mov     ebx,edx
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        test    edi,edi
        jz      .empty
        add     edi,esi
        xor     ebp,ebp         ; bit 0: a cased one seen, bit 1: the previous one cased (istitle), bit 2: past the first (isidentifier)
.l:     cmp     esi,edi
        jae     .end
        call    rt_u8_next
        cmp     ebx,7
        jne     @f
        cmp     eax,0x80
        jae     .no
        jmp     .l
@@:     cmp     ebx,10
        jne     @f
        call    rt_cp_printable
        test    eax,eax
        jz      .no
        jmp     .l
@@:     cmp     ebx,11
        jne     @f
        cmp     eax,'_'
        je      .id
@@:     call    rt_cp_props
        mov     ecx,1
        test    ebx,ebx
        jz      .need
        mov     ecx,2
        cmp     ebx,1
        je      .need
        mov     ecx,1+8
        cmp     ebx,2
        je      .need
        mov     ecx,16
        cmp     ebx,3
        je      .need
        mov     ecx,4
        cmp     ebx,6
        je      .need
        mov     ecx,8
        cmp     ebx,8
        je      .need
        cmp     ebx,4
        jne     @f
        test    eax,32+1024
        jnz     .no
        test    eax,64
        jz      .l
        or      ebp,1
        jmp     .l
@@:     cmp     ebx,5
        jne     @f
        test    eax,64+1024
        jnz     .no
        test    eax,32
        jz      .l
        or      ebp,1
        jmp     .l
@@:     cmp     ebx,9
        jne     .ident
        test    eax,32+1024
        jz      @f
        test    ebp,2           ; upper after a cased one
        jnz     .no
        or      ebp,3
        jmp     .l
@@:     test    eax,64
        jz      @f
        test    ebp,2           ; lower with no cased one before
        jz      .no
        or      ebp,3
        jmp     .l
@@:     and     ebp,-3
        jmp     .l
.ident: test    eax,1
        jnz     .id
        test    ebp,4
        jz      .no
        test    eax,4
        jz      .no
.id:    or      ebp,4
        jmp     .l
.need:  test    eax,ecx
        jz      .no
        jmp     .l
.end:   cmp     ebx,4
        je      .any
        cmp     ebx,5
        je      .any
        cmp     ebx,9
        jne     .yes
.any:   mov     eax,ebp
        and     eax,1
        jmp     .out
.empty: cmp     ebx,7
        je      .yes
        cmp     ebx,10
        je      .yes
.no:    xor     eax,eax
        jmp     .out
.yes:   mov     eax,1
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_str_strip : rt_str_new rt_u8_next rt_cp_isspace
rt_str_strip:                   ; eax = s, edx = 1 left / 2 right / 3 both, ecx = the characters (str), 0: whitespace -> eax = new str
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,ecx
        push    edx             ; [esp+4] which ends
        lea     esi,[ebx+12]    ; the start
        mov     edi,[ebx+8]
        add     edi,esi         ; the end of the str (for decoding)
        push    edi             ; [esp] the end
        test    dword [esp+4],1
        jz      .right
.left:  cmp     esi,[esp]
        jae     .right
        push    esi
        call    rt_u8_next
        call    .in
        pop     eax
        je      .left
        mov     esi,eax
.right: test    dword [esp+4],2
        jz      .make
.r:     mov     ecx,[esp]
        cmp     ecx,esi
        jbe     .make
        dec     ecx             ; back to where the last code point starts
.back:  cmp     ecx,esi
        jbe     @f
        cmp     byte [rt_bmode],0
        jne     @f
        mov     al,[ecx]
        and     al,0xC0
        cmp     al,0x80
        jne     @f
        dec     ecx
        jmp     .back
@@:     push    esi ecx
        mov     esi,ecx
        call    rt_u8_next
        call    .in
        pop     ecx esi
        jne     .make
        mov     [esp],ecx
        jmp     .r
.make:  pop     ecx
        add     esp,4
        sub     ecx,esi
        push    ecx
        mov     eax,ecx
        call    rt_str_new
        pop     ecx
        push    eax
        lea     edi,[eax+12]
        rep     movsb
        pop     eax
        pop     ebp edi esi ebx
        ret
.in:    test    ebp,ebp         ; eax = code point -> ZF set when it goes
        jz      rt_cp_isspace
        push    esi edi edx
        mov     edx,eax
        lea     esi,[ebp+12]
        mov     edi,[ebp+8]
        add     edi,esi
.inl:   cmp     esi,edi
        jae     .inno
        call    rt_u8_next
        cmp     eax,edx
        jne     .inl
        pop     edx edi esi
        ret
.inno:  pop     edx edi esi
        test    esp,esp
        ret

;;; code rt_str_replace : rt_sb_bytes rt_sb_take rt_find_bytes rt_u8_next
rt_str_replace:                 ; [esp+4] s, old, new, count (< 0: all) -> eax = new str
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        mov     ebx,[esp+24]
        mov     edi,[esp+28]
        mov     ebp,[esp+36]
        test    ebp,ebp
        jns     @f
        mov     ebp,0x7FFFFFFF
@@:     cmp     dword [edi+8],0
        je      .empty
        xor     esi,esi         ; position
.l:     test    ebp,ebp
        jz      .rest
        mov     ecx,esi
        mov     edx,[ebx+8]
        xor     eax,eax
        call    rt_find_bytes
        cmp     eax,-1
        je      .rest
        push    eax
        lea     eax,[ebx+12+esi]
        mov     edx,[esp]
        sub     edx,esi
        call    rt_sb_bytes
        pop     esi
        call    .new
        add     esi,[edi+8]
        dec     ebp
        jmp     .l
.rest:  lea     eax,[ebx+12+esi]
        mov     edx,[ebx+8]
        sub     edx,esi
        call    rt_sb_bytes
.out:   pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret
.empty: lea     esi,[ebx+12]    ; "": the new text before each code point and at the end
        mov     edi,[ebx+8]
        add     edi,esi
        test    ebp,ebp
        jz      .el
        call    .new
        dec     ebp
.el:    cmp     esi,edi
        jae     .out
        push    esi
        call    rt_u8_next
        pop     eax
        mov     edx,esi
        sub     edx,eax
        call    rt_sb_bytes
        test    ebp,ebp
        jz      .el
        call    .new
        dec     ebp
        jmp     .el
.new:   mov     eax,[esp+36]    ; append the new text (the stack: this call, the mark, 4 registers, the return)
        test    eax,eax
        jz      @f
        mov     edx,[eax+8]
        add     eax,12
        call    rt_sb_bytes
@@:     ret

;;; data rt_msg_sep
rt_msg_sep      db 'empty separator',0

;;; code rt_str_split : rt_list_new rt_list_push rt_kd_str rt_str_new rt_u8_next rt_cp_isspace rt_find_bytes rt_list_reverse rt_panic_value rt_msg_sep
rt_str_rsplit:                  ; eax = s, edx = separator (0: runs of whitespace), ecx = maxsplit (< 0: no limit) -> eax = list[str], split from the right
        push    ebx esi edi ebp
        push    1
        jmp     rt_str_split.go
rt_str_split:                   ; eax = s, edx = separator (0: runs of whitespace), ecx = maxsplit (< 0: no limit) -> eax = list[str]
        push    ebx esi edi ebp
        push    0
.go:    push    ecx             ; [esp] separator, [esp+4] splits left, [esp+8] 1: from the right
        push    edx
        test    edx,edx
        jz      @f
        cmp     dword [edx+8],0
        je      .esep
@@:     mov     ebx,eax
        mov     eax,rt_kd_str
        call    rt_list_new
        mov     ebp,eax
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
        cmp     dword [esp],0
        je      .ws
        cmp     dword [esp+8],0
        jne     .rsep
        xor     ecx,ecx         ; from the left, at a separator
.fs:    cmp     dword [esp+4],0
        je      .fslast
        push    ecx
        mov     edx,[ebx+8]
        mov     edi,[esp+4]
        xor     eax,eax
        call    rt_find_bytes
        pop     ecx
        cmp     eax,-1
        je      .fslast
        mov     edx,eax
        call    .piece
        mov     ecx,edx
        mov     eax,[esp]
        add     ecx,[eax+8]
        dec     dword [esp+4]
        jmp     .fs
.fslast:mov     edx,[ebx+8]
        call    .piece
        jmp     .done
.rsep:  mov     edx,[ebx+8]     ; from the right, at a separator
.rs:    cmp     dword [esp+4],0
        je      .rslast
        push    edx
        xor     ecx,ecx
        mov     edi,[esp+4]
        mov     eax,1
        call    rt_find_bytes
        pop     edx
        cmp     eax,-1
        je      .rslast
        push    eax
        mov     ecx,eax
        mov     edi,[esp+4]
        add     ecx,[edi+8]
        call    .piece          ; [where it is + its length, e)
        pop     edx
        dec     dword [esp+4]
        jmp     .rs
.rslast:xor     ecx,ecx
        call    .piece
        jmp     .rev
.ws:    cmp     dword [esp+8],0
        jne     .rws
.w:     cmp     esi,edi         ; from the left, at whitespace
        jae     .done
        push    esi
        call    rt_u8_next
        call    rt_cp_isspace
        pop     eax
        je      .w
        mov     esi,eax
        cmp     dword [esp+4],0
        je      .wlast
        mov     ecx,esi
.wl:    cmp     esi,edi
        jae     .wend
        push    esi ecx
        call    rt_u8_next
        call    rt_cp_isspace
        pop     ecx eax
        jne     .wl
        mov     esi,eax
.wend:  mov     edx,esi
        lea     eax,[ebx+12]
        sub     ecx,eax
        sub     edx,eax
        call    .piece
        dec     dword [esp+4]
        jmp     .w
.wlast: mov     edx,edi         ; the rest without the whitespace at its end
.wb:    cmp     edx,esi
        jbe     .wbend
        lea     ecx,[edx-1]
.wbk:   cmp     ecx,esi
        jbe     @f
        cmp     byte [rt_bmode],0
        jne     @f
        mov     al,[ecx]
        and     al,0xC0
        cmp     al,0x80
        jne     @f
        dec     ecx
        jmp     .wbk
@@:     push    esi ecx edx
        mov     esi,ecx
        call    rt_u8_next
        call    rt_cp_isspace
        pop     edx ecx esi
        jne     .wbend
        mov     edx,ecx
        jmp     .wb
.wbend: mov     ecx,esi
        lea     eax,[ebx+12]
        sub     ecx,eax
        sub     edx,eax
        call    .piece
        jmp     .done
.rws:   mov     edx,edi         ; from the right, at whitespace
.rw:    cmp     edx,esi
        jbe     .rev
        call    .prev
        jne     .rword
        mov     edx,ecx
        jmp     .rw
.rword: cmp     dword [esp+4],0
        je      .rwlast
        push    edx
.rwl:   cmp     edx,esi
        jbe     .rwe
        call    .prev
        je      .rwe
        mov     edx,ecx
        jmp     .rwl
.rwe:   pop     eax
        mov     ecx,edx
        mov     edx,eax
        lea     eax,[ebx+12]
        sub     ecx,eax
        sub     edx,eax
        call    .piece
        lea     edx,[ebx+12+ecx]
        dec     dword [esp+4]
        jmp     .rw
.rwlast:mov     ecx,esi         ; the rest without the whitespace at its start
.rwa:   cmp     ecx,edx
        jae     .rwae
        push    esi edx
        mov     esi,ecx
        call    rt_u8_next
        push    esi
        call    rt_cp_isspace
        pop     eax
        pop     edx esi
        jne     .rwae
        mov     ecx,eax
        jmp     .rwa
.rwae:  lea     eax,[ebx+12]
        sub     ecx,eax
        sub     edx,eax
        call    .piece
.rev:   mov     eax,ebp
        call    rt_list_reverse
.done:  add     esp,12
        mov     eax,ebp
        pop     ebp edi esi ebx
        ret
.esep:  mov     esi,rt_msg_sep
        jmp     rt_panic_value
.prev:  lea     ecx,[edx-1]     ; edx = a position -> ecx = where the code point before it starts; ZF set when it is whitespace
@@:     cmp     ecx,esi
        jbe     @f
        cmp     byte [rt_bmode],0
        jne     @f
        mov     al,[ecx]
        and     al,0xC0
        cmp     al,0x80
        jne     @f
        dec     ecx
        jmp     @b
@@:     push    esi ecx edx
        mov     esi,ecx
        call    rt_u8_next
        call    rt_cp_isspace
        pop     edx ecx esi
        ret
.piece: push    ecx edx esi edi ; append s[ecx:edx] (byte offsets) to the result
        sub     edx,ecx
        push    edx
        lea     esi,[ebx+12+ecx]
        mov     eax,edx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        push    eax
        mov     eax,ebp
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        pop     edi esi edx ecx
        ret

;;; code rt_str_splitlines : rt_list_new rt_list_push rt_kd_str rt_str_new rt_u8_next
rt_str_splitlines:              ; eax = s, edx = 1: keep the line breaks -> eax = list of its lines
        push    ebx esi edi ebp
        push    edx
        mov     ebx,eax
        mov     eax,rt_kd_str
        call    rt_list_new
        mov     ebp,eax
        lea     esi,[ebx+12]
        mov     edi,[ebx+8]
        add     edi,esi
        mov     ecx,esi         ; where the line starts
.l:     cmp     esi,edi
        jae     .tail
        mov     edx,esi
        call    rt_u8_next
        call    .isbreak
        jne     .l
        cmp     eax,13          ; \r\n is one break
        jne     @f
        cmp     esi,edi
        jae     @f
        cmp     byte [esi],10
        jne     @f
        inc     esi
@@:     cmp     dword [esp],0
        je      @f
        mov     edx,esi
@@:     call    .piece
        mov     ecx,esi
        jmp     .l
.tail:  cmp     ecx,edi
        jae     .done
        mov     edx,edi
        call    .piece
.done:  pop     edx
        mov     eax,ebp
        pop     ebp edi esi ebx
        ret
.isbreak:                       ; eax = code point -> ZF set when it ends a line (rt_bmode: \n and \r only)
        cmp     eax,10
        je      .r
        cmp     eax,13
        je      .r
        jb      @f
        cmp     byte [rt_bmode],0
        jne     .r
        jmp     .w
@@:     cmp     byte [rt_bmode],0
        jne     .r
        cmp     eax,10
        jb      .r
        jmp     .y
.w:     cmp     eax,0x1C
        jb      .r
        cmp     eax,0x1E
        jbe     .y
        cmp     eax,0x85
        je      .r
        cmp     eax,0x2028
        je      .r
        cmp     eax,0x2029
.r:     ret
.y:     cmp     eax,eax
        ret
.piece: push    ecx edx esi edi ; append [ecx, edx) (addresses) to the result
        mov     esi,ecx
        sub     edx,ecx
        push    edx
        mov     eax,edx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        push    eax
        mov     eax,ebp
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        pop     edi esi edx ecx
        ret

;;; code rt_str_partition : rt_tuple_new rt_str_new rt_find_bytes rt_panic_value rt_msg_sep
rt_str_rpartition:              ; eax = s, edx = separator, ecx = TD of tuple[str, str, str] -> eax = new tuple, at the last separator
        push    ebx esi edi ebp
        mov     ebp,1
        jmp     rt_str_partition.go
rt_str_partition:               ; eax = s, edx = separator, ecx = TD of tuple[str, str, str] -> eax = new tuple (items 4 bytes apart)
        push    ebx esi edi ebp
        xor     ebp,ebp
.go:    mov     ebx,eax
        mov     edi,edx
        cmp     dword [edi+8],0
        je      .empty
        mov     eax,ecx
        call    rt_tuple_new
        mov     esi,eax
        xor     ecx,ecx
        mov     edx,[ebx+8]
        mov     eax,ebp
        call    rt_find_bytes
        cmp     eax,-1
        je      .nf
        push    eax
        xor     ecx,ecx
        mov     edx,eax
        call    .sub
        mov     [esi+12],eax
        inc     dword [edi]
        mov     [esi+16],edi
        pop     ecx
        add     ecx,[edi+8]
        mov     edx,[ebx+8]
        call    .sub
        mov     [esi+20],eax
        jmp     .out
.nf:    inc     dword [ebx]     ; (s, "", "") or ("", "", s)
        xor     eax,eax
        call    rt_str_new
        mov     [esi+16],eax
        xor     eax,eax
        call    rt_str_new
        test    ebp,ebp
        jnz     @f
        mov     [esi+12],ebx
        mov     [esi+20],eax
        jmp     .out
@@:     mov     [esi+12],eax
        mov     [esi+20],ebx
.out:   mov     eax,esi
        pop     ebp edi esi ebx
        ret
.sub:   push    esi edi         ; s[ecx:edx] (bytes) -> eax = new str
        sub     edx,ecx
        push    edx
        lea     esi,[ebx+12+ecx]
        mov     eax,edx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret
.empty: mov     esi,rt_msg_sep
        jmp     rt_panic_value

;;; code rt_str_remove : rt_str_new
rt_str_remove:                  ; eax = s, edx = affix, ecx = 0 removeprefix / 1 removesuffix -> eax = new reference
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,ecx
        mov     ecx,[edx+8]
        mov     eax,[ebx+8]
        jecxz   .same
        cmp     ecx,eax
        ja      .same
        lea     edi,[ebx+12]
        test    ebp,ebp
        jz      @f
        add     edi,eax
        sub     edi,ecx
@@:     lea     esi,[edx+12]
        push    ecx
        repe    cmpsb
        pop     ecx
        jne     .same
        mov     eax,[ebx+8]
        sub     eax,ecx
        push    eax ecx
        call    rt_str_new
        pop     edx ecx
        lea     esi,[ebx+12]
        test    ebp,ebp
        jnz     @f
        add     esi,edx
@@:     lea     edi,[eax+12]
        rep     movsb
        pop     ebp edi esi ebx
        ret
.same:  inc     dword [ebx]
        mov     eax,ebx
        pop     ebp edi esi ebx
        ret

;;; code rt_str_expandtabs : rt_u8_next rt_sb_bytes rt_sb_char rt_sb_take
rt_str_expandtabs:              ; eax = s, edx = tab size -> eax = new str
        push    ebx esi edi ebp
        push    dword [rt_sb_len]
        mov     ebp,edx
        lea     esi,[eax+12]
        mov     edi,[eax+8]
        add     edi,esi
        xor     ebx,ebx         ; the column
.l:     cmp     esi,edi
        jae     .done
        mov     ecx,esi
        call    rt_u8_next
        cmp     eax,9
        je      .tab
        push    eax
        mov     eax,ecx
        mov     edx,esi
        sub     edx,ecx
        call    rt_sb_bytes
        pop     eax
        inc     ebx
        cmp     eax,10
        je      @f
        cmp     eax,13
        jne     .l
@@:     xor     ebx,ebx
        jmp     .l
.tab:   test    ebp,ebp
        jle     .l
        mov     eax,ebx
        xor     edx,edx
        div     ebp
        mov     ecx,ebp
        sub     ecx,edx         ; to the next stop
        add     ebx,ecx
@@:     push    ecx
        mov     al,' '
        call    rt_sb_char
        pop     ecx
        dec     ecx
        jnz     @b
        jmp     .l
.done:  pop     eax
        call    rt_sb_take
        pop     ebp edi esi ebx
        ret

;;; code rt_str_join : rt_sb_bytes rt_sb_take
rt_str_join:                    ; eax = separator, edx = list[str] -> eax = new str
        push    ebx esi edi
        push    dword [rt_sb_len]
        mov     esi,eax
        mov     edi,edx
        xor     ebx,ebx
.loop:  test    edi,edi
        jz      .done
        cmp     ebx,[edi+8]
        jae     .done
        test    ebx,ebx
        jz      @f
        test    esi,esi
        jz      @f
        lea     eax,[esi+12]
        mov     edx,[esi+8]
        call    rt_sb_bytes
@@:     mov     eax,[edi+16]
        mov     eax,[eax+ebx*4]
        test    eax,eax
        jz      @f
        mov     edx,[eax+8]
        add     eax,12
        call    rt_sb_bytes
@@:     inc     ebx
        jmp     .loop
.done:  pop     eax
        call    rt_sb_take
        pop     edi esi ebx
        ret

;;; code rt_is_space
rt_is_space:                    ; al = byte -> ZF set when it is ASCII whitespace (number parsing)
        cmp     al,' '
        je      @f
        cmp     al,9
        jb      @f
        cmp     al,13
        ja      @f
        cmp     al,al
@@:     ret

