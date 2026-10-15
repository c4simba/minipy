; ---------------------------------------------------------------- dicts
; Insertion ordered. dict: +8 length, +12 capacity, +16 the keys (8 bytes
; each), +20 the values, +24 the key type (kd), +28 an index (a table of
; entry numbers, -1: empty, -2: a removed entry's; at most half full), +32
; its mask, +36 the value type (kd), +40 the keys' hashes (their low 32
; bits), +44 entries in use. del leaves a hole (key and value zeroed) as in
; CPython, so +44 - +8 entries are holes; whatever walks the entries in
; order squeezes them out first (rt_dict_flat), and so does a full table.

;;; code rt_dict_new : rt_alloc rt_dict_free
rt_dict_new:                    ; eax = key type, edx = value type -> eax = new empty dict
        push    edx
        push    eax
        mov     eax,48
        call    rt_alloc
        pop     ecx
        mov     [eax+24],ecx
        pop     ecx
        mov     [eax+36],ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_dict_free
if defined RT_GC
        mov     edx,[eax+24]
        mov     cl,[ecx+4]
        or      cl,[edx+4]
        test    cl,8
        jz      @f
        or      byte [eax-3],1
@@:
end if
        ret

;;; code rt_dict_free : rt_free rt_decref
rt_dict_free:                   ; destroy routine of dicts
        push    ebx esi
        mov     ebx,eax
        mov     esi,[ebx+44]    ; (holes: zeroes)
.l:     dec     esi
        js      .done
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[ebx+16]
        mov     eax,[eax+esi*8]
        call    rt_decref
@@:     mov     ecx,[ebx+36]
        test    byte [ecx+4],1
        jz      .l
        mov     eax,[ebx+20]
        mov     eax,[eax+esi*4]
        call    rt_decref
        jmp     .l
.done:  mov     eax,[ebx+16]
        call    rt_free
        mov     eax,[ebx+20]
        call    rt_free
        mov     eax,[ebx+28]
        call    rt_free
        mov     eax,[ebx+40]
        call    rt_free
        mov     eax,ebx
        pop     esi ebx
        jmp     rt_free

;;; code rt_dict_lookup
rt_dict_lookup:                 ; ebx = dict (with an index), esi = &key, edi = its hash -> eax = its entry or -1, edx = the index slot where the search ended
        push    ebp
        mov     ebp,edi
        and     ebp,[ebx+32]
.l:     mov     edx,[ebx+28]
        lea     edx,[edx+ebp*4]
        mov     eax,[edx]
        cmp     eax,-1
        je      .out
        test    eax,eax
        js      .n              ; -2: removed, the search goes on
        mov     ecx,[ebx+40]
        cmp     [ecx+eax*4],edi
        jne     .n
        push    eax edx
        shl     eax,3
        add     eax,[ebx+16]
        mov     edx,esi
        mov     ecx,[ebx+24]
        call    dword [ecx+8]
        test    eax,eax
        pop     edx eax
        jnz     .out
.n:     inc     ebp
        and     ebp,[ebx+32]
        jmp     .l
.out:   pop     ebp
        ret

;;; code rt_dict_find : rt_dict_lookup
rt_dict_find:                   ; eax = dict, edx = &key -> eax = its entry (edx = the address of its index slot), or -1
        test    eax,eax
        jz      .nf
        cmp     dword [eax+8],0
        je      .nf
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,edx
        mov     ecx,[ebx+24]
        call    dword [ecx+16]
        mov     edi,eax
        call    rt_dict_lookup
        pop     edi esi ebx
        ret
.nf:    or      eax,-1
        ret

;;; code rt_dict_reindex : rt_alloc rt_free
rt_dict_reindex:                ; ebx = dict without holes: its index made again (sized for its capacity)
        push    esi edi
        mov     ecx,8
        mov     eax,[ebx+12]
        add     eax,eax
@@:     cmp     ecx,eax
        jae     @f
        add     ecx,ecx
        jmp     @b
@@:     lea     eax,[ecx-1]
        cmp     dword [ebx+28],0
        je      .new
        cmp     eax,[ebx+32]
        je      .fill
        push    ecx
        mov     eax,[ebx+28]
        call    rt_free
        pop     ecx
.new:   lea     eax,[ecx-1]
        mov     [ebx+32],eax
        lea     eax,[ecx*4]
        call    rt_alloc
        mov     [ebx+28],eax
.fill:  mov     ecx,[ebx+32]
        inc     ecx
        mov     edi,[ebx+28]
        or      eax,-1
        rep     stosd
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     eax,[ebx+40]
        mov     eax,[eax+esi*4]
        mov     edx,[ebx+28]
.p:     and     eax,[ebx+32]
        cmp     dword [edx+eax*4],-1
        je      @f
        inc     eax
        jmp     .p
@@:     mov     [edx+eax*4],esi
        inc     esi
        jmp     .l
.out:   pop     edi esi
        ret

;;; code rt_dict_compact : rt_alloc rt_free rt_dict_reindex
rt_dict_compact:                ; ebx = dict with holes: they are squeezed out (the entries keep their order), the index made again
        push    esi edi ebp
        mov     eax,[ebx+44]
        call    rt_alloc        ; a byte an entry: 1 if the index still has it
        push    eax
        mov     edx,eax
        mov     esi,[ebx+28]
        mov     ecx,[ebx+32]
.mark:  mov     eax,[esi+ecx*4]
        test    eax,eax
        js      @f
        mov     byte [edx+eax],1
@@:     dec     ecx
        jns     .mark
        xor     esi,esi         ; the entry read
        xor     edi,edi         ; where it goes
        mov     ebp,[ebx+36]
        mov     ebp,[ebp]       ; the value's bytes
.l:     cmp     esi,[ebx+44]
        jae     .done
        mov     edx,[esp]
        cmp     byte [edx+esi],0
        je      .n
        cmp     esi,edi
        je      .kept
        mov     edx,[ebx+16]    ; the key
        mov     eax,[edx+esi*8]
        mov     [edx+edi*8],eax
        mov     eax,[edx+esi*8+4]
        mov     [edx+edi*8+4],eax
        mov     edx,[ebx+40]    ; its hash
        mov     eax,[edx+esi*4]
        mov     [edx+edi*4],eax
        mov     edx,[ebx+20]    ; the value
        mov     eax,esi
        imul    eax,ebp
        mov     ecx,edi
        imul    ecx,ebp
        push    dword [edx+eax]
        pop     dword [edx+ecx]
        cmp     ebp,8
        jne     .kept
        push    dword [edx+eax+4]
        pop     dword [edx+ecx+4]
.kept:  inc     edi
.n:     inc     esi
        jmp     .l
.done:  mov     [ebx+44],edi
        pop     eax
        call    rt_free
        call    rt_dict_reindex
        pop     ebp edi esi
        ret

;;; code rt_dict_flat : rt_dict_compact
rt_dict_flat:                   ; eax = dict or 0: no holes left, so its entries can be walked in order (every register kept)
        test    eax,eax
        jz      .out
        push    ecx
        mov     ecx,[eax+44]
        cmp     ecx,[eax+8]
        pop     ecx
        jne     .sq
.out:   ret
.sq:    push    ebx ecx edx eax
        mov     ebx,eax
        call    rt_dict_compact
        pop     eax edx ecx ebx
        ret

;;; code rt_dict_grow : rt_alloc rt_free rt_dict_reindex rt_dict_compact
rt_dict_grow:                   ; ebx = dict, every entry in use: the holes squeezed out; room for more unless that left a quarter free
        mov     eax,[ebx+44]
        cmp     eax,[ebx+8]
        je      .grow
        call    rt_dict_compact
        mov     eax,[ebx+12]
        shr     eax,2
        add     eax,[ebx+8]
        cmp     eax,[ebx+12]
        ja      .grow
        ret
.grow:  push    esi edi ebp
        mov     ebp,[ebx+12]
        add     ebp,ebp
        cmp     ebp,4
        jae     @f
        mov     ebp,4
@@:     lea     eax,[ebp*8]     ; keys
        mov     ecx,[ebx+8]
        shl     ecx,3
        lea     edx,[ebx+16]
        call    .move
        mov     eax,[ebx+36]    ; values
        mov     eax,[eax]
        mov     ecx,eax
        imul    eax,ebp
        imul    ecx,[ebx+8]
        lea     edx,[ebx+20]
        call    .move
        lea     eax,[ebp*4]     ; hashes
        mov     ecx,[ebx+8]
        shl     ecx,2
        lea     edx,[ebx+40]
        call    .move
        mov     [ebx+12],ebp
        call    rt_dict_reindex
        pop     ebp edi esi
        ret
.move:  push    ecx edx         ; eax = new bytes, ecx = bytes in use, edx = where the array's pointer is: a bigger copy
        call    rt_alloc
        pop     edx ecx
        mov     edi,eax
        mov     esi,[edx]
        push    eax
        rep     movsb
        mov     eax,[edx]
        push    edx
        call    rt_free
        pop     edx
        pop     eax
        mov     [edx],eax
        ret

;;; code rt_dict_slot : rt_dict_lookup rt_dict_grow rt_incref rt_panic_none
rt_dict_slot:                   ; eax = dict, edx = &key -> eax = address of its value (a new entry, value zero, if missing)
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,edx
        mov     ecx,[ebx+24]
        call    dword [ecx+16]
        mov     edi,eax
        cmp     dword [ebx+8],0
        je      .new
        call    rt_dict_lookup
        test    eax,eax
        jns     .have
.new:   mov     eax,[ebx+44]
        cmp     eax,[ebx+12]
        jb      @f
        call    rt_dict_grow
@@:     call    rt_dict_lookup  ; where it goes
        mov     eax,[ebx+44]
        mov     [edx],eax
        inc     dword [ebx+44]
        inc     dword [ebx+8]
        mov     ecx,[ebx+40]
        mov     [ecx+eax*4],edi
        push    eax
        mov     ecx,eax         ; the key, a reference taken
        shl     ecx,3
        add     ecx,[ebx+16]
        mov     eax,[esi]
        mov     [ecx],eax
        mov     edx,[ebx+24]
        cmp     dword [edx],8
        jne     @f
        mov     eax,[esi+4]
        mov     [ecx+4],eax
@@:     test    byte [edx+4],1
        jz      @f
        mov     eax,[ecx]
        call    rt_incref
@@:     pop     eax             ; the value: zero
        mov     ecx,[ebx+36]
        mov     ecx,[ecx]
        imul    eax,ecx
        add     eax,[ebx+20]
        mov     dword [eax],0
        mov     dword [eax+ecx-4],0
        pop     edi esi ebx
        ret
.have:  mov     ecx,[ebx+36]
        imul    eax,[ecx]
        add     eax,[ebx+20]
        pop     edi esi ebx
        ret

;;; code rt_dict_get : rt_dict_find rt_raise_key rt_panic_none
rt_dict_get:                    ; eax = dict, edx = &key -> eax = address of its value (KeyError if missing)
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        call    rt_dict_find
        test    eax,eax
        js      .miss
        mov     ecx,[ebx+36]
        imul    eax,[ecx]
        add     eax,[ebx+20]
        pop     esi ebx
        ret
.miss:  test    ebx,ebx
        jz      rt_panic_none
        mov     eax,esi
        mov     ecx,[ebx+24]
        jmp     rt_raise_key

;;; code rt_dict_del : rt_dict_find rt_raise_key rt_decref rt_scratch rt_panic_none
rt_dict_del:                    ; eax = dict, edx = &key -> eax = rt_scratch holding its value (owned); KeyError if missing
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        call    rt_dict_find
        test    eax,eax
        js      .miss
        mov     edi,eax         ; the entry: a hole from now on
        mov     dword [edx],-2  ; its index slot: removed (searches go on past it) - or empty when the next one is
        mov     ecx,[ebx+28]
        sub     edx,ecx
        shr     edx,2
.free:  lea     eax,[edx+1]
        and     eax,[ebx+32]
        cmp     dword [ecx+eax*4],-1
        jne     @f
        mov     dword [ecx+edx*4],-1    ; (no search passes it: so neither the removed ones before it)
        dec     edx
        and     edx,[ebx+32]
        cmp     dword [ecx+edx*4],-2
        je      .free
@@:     mov     ecx,[ebx+36]    ; its value -> rt_scratch, zeroed
        mov     ebp,[ecx]
        mov     edx,edi
        imul    edx,ebp
        add     edx,[ebx+20]
        xor     ecx,ecx
        mov     eax,[edx]
        mov     [rt_scratch],eax
        mov     eax,[edx+ebp-4]
        mov     [rt_scratch+ebp-4],eax
        mov     [edx],ecx
        mov     [edx+ebp-4],ecx
        mov     edx,[ebx+16]    ; its key dropped, zeroed
        lea     edx,[edx+edi*8]
        mov     eax,[edx]
        mov     [edx],ecx
        mov     [edx+4],ecx
        dec     dword [ebx+8]
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      @f
        call    rt_decref
@@:     mov     eax,rt_scratch
        pop     ebp edi esi ebx
        ret
.miss:  test    ebx,ebx
        jz      rt_panic_none
        mov     eax,esi
        mov     ecx,[ebx+24]
        jmp     rt_raise_key

;;; code rt_dict_keys : rt_list_new rt_list_reserve rt_list_push rt_elem_copy rt_panic_none rt_dict_flat
rt_dict_keys:                   ; eax = dict -> eax = new list of its keys
        test    eax,eax
        jz      rt_panic_none
        call    rt_dict_flat
        push    ebx esi
        mov     ebx,eax
        mov     eax,[ebx+24]
        call    rt_list_new
        push    eax
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     eax,[esp]
        call    rt_list_push
        mov     edx,[ebx+16]
        lea     edx,[edx+esi*8]
        mov     ecx,[ebx+24]
        call    rt_elem_copy
        inc     esi
        jmp     .l
.out:   pop     eax
        pop     esi ebx
        ret

;;; code rt_dict_values : rt_list_new rt_list_reserve rt_list_push rt_elem_copy rt_panic_none rt_dict_flat
rt_dict_values:                 ; eax = dict -> eax = new list of its values
        test    eax,eax
        jz      rt_panic_none
        call    rt_dict_flat
        push    ebx esi
        mov     ebx,eax
        mov     eax,[ebx+36]
        call    rt_list_new
        push    eax
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     eax,[esp]
        call    rt_list_push
        mov     ecx,[ebx+36]
        mov     edx,esi
        imul    edx,[ecx]
        add     edx,[ebx+20]
        call    rt_elem_copy
        inc     esi
        jmp     .l
.out:   pop     eax
        pop     esi ebx
        ret

;;; code rt_dict_items : rt_list_new rt_list_reserve rt_list_push rt_tuple_new rt_elem_copy rt_kd_tuple rt_panic_none rt_dict_flat
rt_dict_items:                  ; eax = dict, edx = TD of tuple[K, V] -> eax = new list of (key, value)
        test    eax,eax
        jz      rt_panic_none
        call    rt_dict_flat
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,edx
        mov     eax,rt_kd_tuple
        call    rt_list_new
        push    eax
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     eax,ebp
        call    rt_tuple_new
        mov     edi,eax
        mov     eax,[ebp+12]    ; the key
        add     eax,edi
        mov     edx,[ebx+16]
        lea     edx,[edx+esi*8]
        mov     ecx,[ebx+24]
        call    rt_elem_copy
        mov     eax,[ebp+20]    ; the value
        add     eax,edi
        mov     ecx,[ebx+36]
        mov     edx,esi
        imul    edx,[ecx]
        add     edx,[ebx+20]
        call    rt_elem_copy
        mov     eax,[esp]
        call    rt_list_push
        mov     [eax],edi
        inc     esi
        jmp     .l
.out:   pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_dict_clear : rt_decref
rt_dict_clear:                  ; eax = dict: remove every entry
        test    eax,eax
        jz      .out
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,[ebx+44]    ; (holes: zeroes)
.l:     dec     esi
        js      .done
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[ebx+16]
        mov     eax,[eax+esi*8]
        call    rt_decref
@@:     mov     ecx,[ebx+36]
        test    byte [ecx+4],1
        jz      .l
        mov     eax,[ebx+20]
        mov     eax,[eax+esi*4]
        call    rt_decref
        jmp     .l
.done:  mov     dword [ebx+8],0
        mov     dword [ebx+44],0
        mov     edi,[ebx+28]
        test    edi,edi
        jz      @f
        mov     ecx,[ebx+32]
        inc     ecx
        or      eax,-1
        rep     stosd
@@:     pop     edi esi ebx
.out:   ret

;;; code rt_dict_update : rt_dict_slot rt_elem_copy rt_decref rt_scratch rt_dict_flat
rt_dict_update:                 ; eax = destination, edx = source: every entry of the source set in the destination
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        mov     eax,edx
        call    rt_dict_flat
        xor     edi,edi
.l:     test    esi,esi
        jz      .out
        cmp     edi,[esi+8]
        jae     .out
        mov     eax,ebx
        mov     edx,[esi+16]
        lea     edx,[edx+edi*8]
        call    rt_dict_slot
        mov     ebp,eax
        mov     ecx,[ebx+36]    ; the old value (a reference) is dropped after the copy
        push    dword [ebp]
        mov     ecx,[esi+36]
        mov     edx,edi
        imul    edx,[ecx]
        add     edx,[esi+20]
        mov     eax,ebp
        call    rt_elem_copy
        pop     eax
        mov     ecx,[ebx+36]
        test    byte [ecx+4],1
        jz      @f
        call    rt_decref
@@:     inc     edi
        jmp     .l
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_dict_copy : rt_dict_new rt_dict_update
rt_dict_copy:                   ; eax = dict -> eax = new dict with its entries
        push    ebx
        mov     ebx,eax
        mov     eax,[ebx+24]
        mov     edx,[ebx+36]
        call    rt_dict_new
        push    eax
        mov     edx,ebx
        call    rt_dict_update
        pop     eax
        pop     ebx
        ret

;;; code rt_dict_eq : rt_dict_find rt_dict_flat
rt_dict_eq:                     ; eax = a, edx = b (dicts of one type) -> eax = 1 if the same keys map to equal values
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        call    rt_dict_flat
        cmp     ebx,esi
        je      .yes
        test    ebx,ebx
        jz      .no
        test    esi,esi
        jz      .no
        xor     eax,eax
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+8]
@@:     xor     edx,edx
        test    esi,esi
        jz      @f
        mov     edx,[esi+8]
@@:     cmp     eax,edx
        jne     .no
        xor     edi,edi
.l:     test    ebx,ebx
        jz      .yes
        cmp     edi,[ebx+8]
        jae     .yes
        mov     eax,esi
        mov     edx,[ebx+16]
        lea     edx,[edx+edi*8]
        call    rt_dict_find
        test    eax,eax
        js      .no
        mov     ebp,[ebx+36]
        imul    eax,[ebp]
        add     eax,[esi+20]
        mov     edx,edi
        imul    edx,[ebp]
        add     edx,[ebx+20]
        mov     ecx,ebp
        call    dword [ebp+8]
        test    eax,eax
        jz      .no
        inc     edi
        jmp     .l
.yes:   mov     eax,1
        pop     ebp edi esi ebx
        ret
.no:    xor     eax,eax
        pop     ebp edi esi ebx
        ret

;;; code rt_dict_eq_k : rt_dict_eq
rt_dict_eq_k:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_dict_eq

;;; code rt_dict_repr_k : rt_sb_char rt_sb_cstr rt_repr_none rt_dict_flat
rt_dict_repr_k:                 ; repr of a dict (for messages)
        cmp     dword [eax],0
        je      rt_repr_none
        push    ebx esi
        mov     ebx,[eax]
        mov     eax,ebx
        call    rt_dict_flat
        mov     al,'{'
        call    rt_sb_char
        xor     esi,esi
.l:     test    ebx,ebx
        jz      .end
        cmp     esi,[ebx+8]
        jae     .end
        test    esi,esi
        jz      @f
        mov     eax,rt_s_dsep
        call    rt_sb_cstr
@@:     mov     eax,[ebx+16]
        lea     eax,[eax+esi*8]
        mov     ecx,[ebx+24]
        call    dword [ecx+20]
        mov     eax,rt_s_dcol
        call    rt_sb_cstr
        mov     ecx,[ebx+36]
        mov     eax,esi
        imul    eax,[ecx]
        add     eax,[ebx+20]
        call    dword [ecx+20]
        inc     esi
        jmp     .l
.end:   mov     al,'}'
        pop     esi ebx
        jmp     rt_sb_char
;;; data rt_dict_repr_k
rt_s_dsep       db ', ',0
rt_s_dcol       db ': ',0
