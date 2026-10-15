; ---------------------------------------------------------------- sets
; CPython's hash table, so that sets iterate in CPython's order (the
; interpreter's). set: +8 used, +12 fill (used and dummies), +16 the table,
; +20 its mask (size - 1), +24 the element type (kd), +28 where pop() looks
; first. Entry (24 bytes): +0 the element (8 bytes), +8 its hash (8 bytes),
; +16 state: 0 empty, 1 in use, 2 dummy (removed).

;;; code rt_set_new : rt_alloc rt_set_free
rt_set_new:                     ; eax = element type -> eax = new empty set
        push    eax
        mov     eax,32
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_set_free
        mov     [eax+24],ecx
        mov     dword [eax+20],7
if defined RT_GC
        test    byte [ecx+4],8
        jz      @f
        or      byte [eax-3],1
@@:
end if
        push    eax
        mov     eax,8*24
        call    rt_alloc
        mov     ecx,eax
        pop     eax
        mov     [eax+16],ecx
        ret

;;; code rt_set_free : rt_free rt_decref
rt_set_free:                    ; destroy routine of sets
        push    ebx esi
        mov     ebx,eax
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .t
        mov     esi,[ebx+20]
.l:     test    esi,esi
        js      .t
        mov     eax,esi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     @f
        mov     eax,[eax]
        call    rt_decref
@@:     dec     esi
        jmp     .l
.t:     mov     eax,[ebx+16]
        call    rt_free
        mov     eax,ebx
        pop     esi ebx
        jmp     rt_free

;;; code rt_set_insert_clean
; Into a table without dummies nor this element: [esp+4] table, mask, &element,
; kd, hash (lo, hi). Copies the element (no reference taken).
rt_set_insert_clean:
        push    ebx esi edi ebp
        mov     esi,[esp+36]    ; perturb = hash
        mov     edi,[esp+40]
        mov     ebx,esi
        and     ebx,[esp+24]    ; i
.probe: mov     eax,ebx
        imul    eax,24
        add     eax,[esp+20]
        cmp     dword [eax+16],0
        je      .found
        mov     ecx,ebx
        add     ecx,9
        cmp     ecx,[esp+24]
        ja      .next
        mov     ecx,9
.lin:   add     eax,24
        cmp     dword [eax+16],0
        je      .found
        dec     ecx
        jnz     .lin
.next:  shrd    esi,edi,5
        shr     edi,5
        lea     ebx,[ebx+ebx*4+1]
        add     ebx,esi
        and     ebx,[esp+24]
        jmp     .probe
.found: mov     edx,[esp+28]
        mov     ecx,[edx]
        mov     [eax],ecx
        mov     ecx,[esp+32]
        cmp     dword [ecx],8
        jne     @f
        mov     ecx,[edx+4]
        mov     [eax+4],ecx
@@:     mov     ecx,[esp+36]
        mov     [eax+8],ecx
        mov     ecx,[esp+40]
        mov     [eax+12],ecx
        mov     dword [eax+16],1
        pop     ebp edi esi ebx
        ret

;;; code rt_set_resize : rt_alloc rt_free rt_set_insert_clean
rt_set_resize:                  ; eax = set, edx = minused: a table of the first size (8, 16 ...) above it
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ecx,8
@@:     cmp     ecx,edx
        ja      @f
        add     ecx,ecx
        jmp     @b
@@:     push    ecx
        mov     eax,ecx
        imul    eax,24
        call    rt_alloc
        mov     edi,eax         ; the new table
        mov     esi,[ebx+16]    ; the old one
        mov     ebp,[ebx+20]
        pop     ecx
        dec     ecx
        mov     [ebx+20],ecx
        mov     [ebx+16],edi
        xor     edx,edx
.l:     cmp     edx,ebp
        ja      .done
        mov     eax,edx
        imul    eax,24
        add     eax,esi
        cmp     dword [eax+16],1
        jne     .n
        push    edx
        push    dword [eax+12]
        push    dword [eax+8]
        push    dword [ebx+24]
        push    eax
        push    dword [ebx+20]
        push    edi
        call    rt_set_insert_clean
        add     esp,24
        pop     edx
.n:     inc     edx
        jmp     .l
.done:  mov     eax,[ebx+8]
        mov     [ebx+12],eax
        mov     eax,esi
        call    rt_free
        pop     ebp edi esi ebx
        ret

;;; code rt_set_add_h : rt_set_resize rt_incref
; [esp+4] set, &element, hash (lo, hi): add it unless there (CPython's
; set_add_entry) -> eax = 1 if added (a reference taken), 0 if it was there
rt_set_add_h:
        push    ebx esi edi ebp
        sub     esp,12          ; +0 the first dummy seen, +4 i, +8 probes left
        mov     ebx,[esp+32]
        mov     esi,[esp+40]    ; perturb = hash
        mov     edi,[esp+44]
        mov     dword [esp],0
        mov     eax,esi
        and     eax,[ebx+20]
        mov     [esp+4],eax
.probe: mov     ebp,[esp+4]
        imul    ebp,24
        add     ebp,[ebx+16]
        mov     dword [esp+8],0
        mov     eax,[esp+4]
        add     eax,9
        cmp     eax,[ebx+20]
        ja      .one
        mov     dword [esp+8],9
.one:   mov     eax,[ebp+16]
        test    eax,eax
        jz      .unused
        cmp     eax,1
        jne     .dummy
        mov     eax,[ebp+8]
        cmp     eax,[esp+40]
        jne     .next
        mov     eax,[ebp+12]
        cmp     eax,[esp+44]
        jne     .next
        mov     eax,ebp
        mov     edx,[esp+36]
        mov     ecx,[ebx+24]
        call    dword [ecx+8]
        test    eax,eax
        jnz     .present
        jmp     .next
.dummy: cmp     dword [esp],0
        jne     .next
        mov     [esp],ebp
.next:  add     ebp,24
        dec     dword [esp+8]
        jns     .one
        shrd    esi,edi,5
        shr     edi,5
        mov     eax,[esp+4]
        lea     eax,[eax+eax*4+1]
        add     eax,esi
        and     eax,[ebx+20]
        mov     [esp+4],eax
        jmp     .probe
.present:
        xor     eax,eax
        jmp     .out
.unused:
        mov     eax,[esp]
        test    eax,eax
        jz      .fresh
        mov     ebp,eax         ; the first dummy instead
        inc     dword [ebx+8]
        call    .store
        jmp     .added
.fresh: inc     dword [ebx+12]
        inc     dword [ebx+8]
        call    .store
        mov     eax,[ebx+12]    ; fill*5 >= mask*3: grow
        lea     eax,[eax+eax*4]
        mov     ecx,[ebx+20]
        lea     ecx,[ecx+ecx*2]
        cmp     eax,ecx
        jb      .added
        mov     edx,[ebx+8]
        cmp     edx,50000
        ja      @f
        shl     edx,2
        jmp     .rs
@@:     add     edx,edx
.rs:    mov     eax,ebx
        call    rt_set_resize
.added: mov     eax,1
.out:   add     esp,12
        pop     ebp edi esi ebx
        ret
.store: mov     edx,[esp+4+36]  ; the element into entry ebp
        mov     ecx,[ebx+24]
        mov     eax,[edx]
        mov     [ebp],eax
        cmp     dword [ecx],8
        jne     @f
        mov     eax,[edx+4]
        mov     [ebp+4],eax
@@:     mov     eax,[esp+4+40]
        mov     [ebp+8],eax
        mov     eax,[esp+4+44]
        mov     [ebp+12],eax
        mov     dword [ebp+16],1
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[ebp]
        call    rt_incref
@@:     ret

;;; code rt_set_add : rt_set_add_h rt_panic_none
rt_set_add:                     ; eax = set, edx = &element: add it (a reference taken) -> eax = 1 if new
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,edx
        mov     ecx,[ebx+24]
        call    dword [ecx+16]
        push    edx
        push    eax
        push    esi
        push    ebx
        call    rt_set_add_h
        add     esp,16
        pop     esi ebx
        ret

;;; code rt_set_find_h
; [esp+4] set, &element, hash (lo, hi) -> eax = its entry, or 0
rt_set_find_h:
        push    ebx esi edi ebp
        push    0               ; i
        mov     ebx,[esp+24]
        mov     esi,[esp+32]
        mov     edi,[esp+36]
        mov     eax,esi
        and     eax,[ebx+20]
        mov     [esp],eax
.probe: mov     ebp,[esp]
        imul    ebp,24
        add     ebp,[ebx+16]
        xor     ecx,ecx
        mov     eax,[esp]
        add     eax,9
        cmp     eax,[ebx+20]
        ja      @f
        mov     ecx,9
@@:     push    ecx             ; probes left
.one:   mov     eax,[ebp+16]
        test    eax,eax
        jz      .nf
        cmp     eax,1
        jne     .next
        mov     eax,[ebp+8]
        cmp     eax,[esp+4+32]
        jne     .next
        mov     eax,[ebp+12]
        cmp     eax,[esp+4+36]
        jne     .next
        mov     eax,ebp
        mov     edx,[esp+4+28]
        mov     ecx,[ebx+24]
        call    dword [ecx+8]
        test    eax,eax
        jnz     .found
.next:  add     ebp,24
        dec     dword [esp]
        jns     .one
        pop     ecx
        shrd    esi,edi,5
        shr     edi,5
        mov     eax,[esp]
        lea     eax,[eax+eax*4+1]
        add     eax,esi
        and     eax,[ebx+20]
        mov     [esp],eax
        jmp     .probe
.found: mov     eax,ebp
        jmp     .out
.nf:    xor     eax,eax
.out:   add     esp,8
        pop     ebp edi esi ebx
        ret

;;; code rt_set_find : rt_set_find_h
rt_set_find:                    ; eax = set, edx = &element -> eax = its entry, or 0
        test    eax,eax
        jz      .no
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,edx
        mov     ecx,[ebx+24]
        call    dword [ecx+16]
        push    edx
        push    eax
        push    esi
        push    ebx
        call    rt_set_find_h
        add     esp,16
        pop     esi ebx
        ret
.no:    xor     eax,eax
        ret

;;; code rt_set_contains : rt_set_find
rt_set_contains:                ; eax = set, edx = &element -> eax = bool
        call    rt_set_find
        test    eax,eax
        setnz   al
        movzx   eax,al
        ret

;;; code rt_set_discard : rt_set_find rt_decref
rt_set_discard:                 ; eax = set, edx = &element: remove it if there -> eax = 1 if it was
        push    ebx
        mov     ebx,eax
        call    rt_set_find
        test    eax,eax
        jz      .out
        mov     dword [eax+16],2
        dec     dword [ebx+8]
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .one
        mov     eax,[eax]
        call    rt_decref
.one:   mov     eax,1
.out:   pop     ebx
        ret

;;; code rt_set_remove : rt_set_discard rt_raise_key rt_panic_none
rt_set_remove:                  ; eax = set, edx = &element: remove it (KeyError if not there)
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        call    rt_set_discard
        test    eax,eax
        jz      .missing
        pop     esi ebx
        ret
.missing:
        mov     eax,esi
        mov     ecx,[ebx+24]
        jmp     rt_raise_key

;;; code rt_set_pop : rt_scratch rt_keyerr_text
rt_set_pop:                     ; eax = set -> eax = rt_scratch holding an element taken out (owned)
        push    ebx
        mov     ebx,eax
        test    ebx,ebx
        jz      .empty
        cmp     dword [ebx+8],0
        je      .empty
        mov     eax,[ebx+28]
        and     eax,[ebx+20]
.l:     mov     ecx,eax
        imul    ecx,24
        add     ecx,[ebx+16]
        cmp     dword [ecx+16],1
        je      .take
        inc     eax
        cmp     eax,[ebx+20]
        jbe     .l
        xor     eax,eax
        jmp     .l
.take:  inc     eax
        mov     [ebx+28],eax
        mov     eax,[ecx]
        mov     [rt_scratch],eax
        mov     eax,[ecx+4]
        mov     [rt_scratch+4],eax
        mov     dword [ecx+16],2
        dec     dword [ebx+8]
        mov     eax,rt_scratch
        pop     ebx
        ret
.empty: mov     esi,rt_msg_popset
        jmp     rt_keyerr_text
;;; data rt_set_pop
rt_msg_popset   db "'pop from an empty set'",0

;;; code rt_set_clear : rt_alloc rt_free rt_decref
rt_set_clear:                   ; eax = set: remove every element
        test    eax,eax
        jz      .out
        push    ebx esi
        mov     ebx,eax
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .t
        mov     esi,[ebx+20]
.l:     test    esi,esi
        js      .t
        mov     eax,esi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     @f
        mov     eax,[eax]
        call    rt_decref
@@:     dec     esi
        jmp     .l
.t:     mov     eax,[ebx+16]
        call    rt_free
        mov     eax,8*24
        call    rt_alloc
        mov     [ebx+16],eax
        mov     dword [ebx+20],7
        xor     eax,eax
        mov     [ebx+8],eax
        mov     [ebx+12],eax
        mov     [ebx+28],eax
        pop     esi ebx
.out:   ret

;;; code rt_set_merge : rt_set_resize rt_set_insert_clean rt_set_add_h rt_incref
rt_set_merge:                   ; eax = set, edx = another: add the other's elements (CPython's set_merge)
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        test    esi,esi
        jz      .out
        cmp     esi,ebx
        je      .out
        cmp     dword [esi+8],0
        je      .out
        mov     eax,[ebx+12]    ; (fill + other's used)*5 >= mask*3: one resize first
        add     eax,[esi+8]
        lea     eax,[eax+eax*4]
        mov     ecx,[ebx+20]
        lea     ecx,[ecx+ecx*2]
        cmp     eax,ecx
        jb      @f
        mov     edx,[ebx+8]
        add     edx,[esi+8]
        add     edx,edx
        mov     eax,ebx
        call    rt_set_resize
@@:     cmp     dword [ebx+12],0
        jne     .add
        mov     eax,[ebx+20]
        cmp     eax,[esi+20]
        jne     .clean
        mov     eax,[esi+12]
        cmp     eax,[esi+8]
        jne     .clean
        mov     ecx,[esi+20]    ; empty, the same size, no dummies: copy the table
        inc     ecx
        imul    ecx,24
        mov     edi,[ebx+16]
        push    esi
        mov     esi,[esi+16]
        rep     movsb
        pop     esi
        mov     eax,[esi+12]
        mov     [ebx+12],eax
        mov     eax,[esi+8]
        mov     [ebx+8],eax
        call    .refs
        jmp     .out
.clean: xor     edi,edi         ; empty: insert into the clean table
.cl:    cmp     edi,[esi+20]
        ja      .cdone
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .cn
        push    dword [eax+12]
        push    dword [eax+8]
        push    dword [ebx+24]
        push    eax
        push    dword [ebx+20]
        push    dword [ebx+16]
        call    rt_set_insert_clean
        add     esp,24
.cn:    inc     edi
        jmp     .cl
.cdone: mov     eax,[esi+8]
        mov     [ebx+8],eax
        mov     [ebx+12],eax
        call    .refs
        jmp     .out
.add:   xor     edi,edi         ; otherwise one by one
.al:    cmp     edi,[esi+20]
        ja      .out
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .an
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    ebx
        call    rt_set_add_h
        add     esp,16
.an:    inc     edi
        jmp     .al
.out:   pop     ebp edi esi ebx
        ret
.refs:  mov     ecx,[ebx+24]    ; a reference to every element now in the set
        test    byte [ecx+4],1
        jz      .rdone
        mov     edi,[ebx+20]
.rl:    test    edi,edi
        js      .rdone
        mov     eax,edi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     @f
        mov     eax,[eax]
        call    rt_incref
@@:     dec     edi
        jmp     .rl
.rdone: ret

;;; code rt_set_copy : rt_set_new rt_set_merge
rt_set_copy:                    ; eax = set, edx = element type -> eax = new set of its elements (CPython's set.copy)
        push    ebx
        mov     ebx,eax
        mov     eax,edx
        call    rt_set_new
        push    eax
        mov     edx,ebx
        call    rt_set_merge
        pop     eax
        pop     ebx
        ret

;;; code rt_set_add_list : rt_set_add
rt_set_add_list:                ; eax = set, edx = list: add its elements
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        xor     edi,edi
.l:     test    esi,esi
        jz      .out
        cmp     edi,[esi+8]
        jae     .out
        mov     ecx,[esi+20]
        mov     edx,edi
        imul    edx,[ecx]
        add     edx,[esi+16]
        mov     eax,ebx
        call    rt_set_add
        inc     edi
        jmp     .l
.out:   pop     edi esi ebx
        ret

;;; code rt_set_from_list : rt_set_new rt_set_add_list
rt_set_from_list:               ; eax = list, edx = element type -> eax = new set of its elements
        push    ebx
        mov     ebx,eax
        mov     eax,edx
        call    rt_set_new
        push    eax
        mov     edx,ebx
        call    rt_set_add_list
        pop     eax
        pop     ebx
        ret

;;; code rt_set_to_list : rt_list_new rt_list_reserve rt_list_push rt_elem_copy
rt_set_to_list:                 ; eax = set, edx = element type -> eax = new list of its elements, in its order
        push    ebx esi
        mov     ebx,eax
        mov     eax,edx
        call    rt_list_new
        push    eax
        test    ebx,ebx
        jz      .out
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        xor     esi,esi
.l:     cmp     esi,[ebx+20]
        ja      .out
        mov     edx,esi
        imul    edx,24
        add     edx,[ebx+16]
        cmp     dword [edx+16],1
        jne     .n
        mov     eax,[esp]
        push    edx
        call    rt_list_push
        pop     edx
        mov     ecx,[ebx+24]
        call    rt_elem_copy
.n:     inc     esi
        jmp     .l
.out:   pop     eax
        pop     esi ebx
        ret

;;; code rt_set_next
rt_set_next:                    ; eax = set, edx = position -> eax = the first entry in use from there (its index), or -1
        test    eax,eax
        jz      .end
.l:     cmp     edx,[eax+20]
        ja      .end
        mov     ecx,edx
        imul    ecx,24
        add     ecx,[eax+16]
        cmp     dword [ecx+16],1
        je      .found
        inc     edx
        jmp     .l
.found: mov     eax,edx
        ret
.end:   or      eax,-1
        ret

;;; code rt_set_compact : rt_set_resize
rt_set_compact:                 ; eax = set: after removals, more than a quarter dummies: a new table
        mov     ecx,[eax+12]
        sub     ecx,[eax+8]
        mov     edx,[eax+20]
        shr     edx,2
        cmp     ecx,edx
        jbe     .out
        mov     edx,[eax+8]
        cmp     edx,50000
        ja      @f
        shl     edx,2
        jmp     rt_set_resize
@@:     add     edx,edx
        jmp     rt_set_resize
.out:   ret

;;; code rt_set_diff_update : rt_set_clear rt_set_op rt_set_find_h rt_set_compact rt_decref
rt_set_diff_update:             ; eax = set, edx = another: its elements removed (CPython's set_difference_update)
        cmp     eax,edx
        je      rt_set_clear
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        test    esi,esi
        jz      .out
        xor     edi,edi         ; the other is much bigger: walk the intersection instead
        mov     eax,[esi+8]
        shr     eax,3
        cmp     eax,[ebx+8]
        jbe     .walk
        mov     eax,ebx
        mov     edx,esi
        mov     ecx,1
        call    rt_set_op
        mov     esi,eax
        mov     edi,eax         ; dropped at the end
.walk:  push    edi
        xor     edi,edi
.l:     cmp     edi,[esi+20]
        ja      .done
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .n
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    ebx
        call    rt_set_find_h
        add     esp,16
        test    eax,eax
        jz      .n
        mov     dword [eax+16],2
        dec     dword [ebx+8]
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .n
        mov     eax,[eax]
        call    rt_decref
.n:     inc     edi
        jmp     .l
.done:  pop     eax
        call    rt_decref
        mov     eax,ebx
        call    rt_set_compact
.out:   pop     edi esi ebx
        ret

;;; code rt_set_xor_update : rt_set_clear rt_set_find_h rt_set_add_h rt_decref
rt_set_xor_update:              ; eax = set, edx = another: its elements toggled (CPython's set_symmetric_difference_update)
        cmp     eax,edx
        je      rt_set_clear
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        xor     edi,edi
.l:     test    esi,esi
        jz      .out
        cmp     edi,[esi+20]
        ja      .out
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .n
        push    eax
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    ebx
        call    rt_set_find_h
        add     esp,16
        pop     edx
        test    eax,eax
        jz      .add
        mov     dword [eax+16],2
        dec     dword [ebx+8]
        mov     ecx,[ebx+24]
        test    byte [ecx+4],1
        jz      .n
        mov     eax,[eax]
        call    rt_decref
        jmp     .n
.add:   push    dword [edx+12]
        push    dword [edx+8]
        push    edx
        push    ebx
        call    rt_set_add_h
        add     esp,16
.n:     inc     edi
        jmp     .l
.out:   pop     edi esi ebx
        ret

;;; code rt_set_op : rt_set_new rt_set_copy rt_set_merge rt_set_find_h rt_set_add_h rt_set_diff_update rt_set_xor_update
; a | b, a & b, a - b, a ^ b: the interpreter's (CPython's) algorithms, which
; decide the order of the result
rt_set_op:                      ; eax = a, edx = b, ecx = 0 |, 1 &, 2 -, 3 ^ -> eax = new set
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        cmp     ecx,1
        je      .and
        cmp     ecx,2
        je      .sub
        cmp     ecx,3
        je      .xor
        mov     eax,ebx         ; |: a copy of a, b merged in
        mov     edx,[ebx+24]
        call    rt_set_copy
        push    eax
        mov     edx,esi
        call    rt_set_merge
        pop     eax
        jmp     .out
.and:   mov     eax,[ebx+24]    ; &: the elements of the smaller found in the other
        call    rt_set_new
        mov     ebp,eax
        mov     eax,[esi+8]
        cmp     eax,[ebx+8]
        jbe     @f
        xchg    ebx,esi         ; ebx = the bigger, esi = the one walked
@@:     xor     edi,edi
.al:    cmp     edi,[esi+20]
        ja      .adone
        mov     eax,edi
        imul    eax,24
        add     eax,[esi+16]
        cmp     dword [eax+16],1
        jne     .an
        push    eax
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    ebx
        call    rt_set_find_h
        add     esp,16
        pop     edx
        test    eax,eax
        jz      .an
        push    dword [edx+12]
        push    dword [edx+8]
        push    edx
        push    ebp
        call    rt_set_add_h
        add     esp,16
.an:    inc     edi
        jmp     .al
.adone: mov     eax,ebp
        jmp     .out
.sub:   mov     eax,[ebx+8]     ; -: a big: a copy of a, b's elements removed
        shr     eax,2
        cmp     eax,[esi+8]
        jbe     .subw
        mov     eax,ebx
        mov     edx,[ebx+24]
        call    rt_set_copy
        mov     ebp,eax
        mov     edx,esi
        call    rt_set_diff_update
        mov     eax,ebp
        jmp     .out
.subw:  mov     eax,[ebx+24]    ; else a's elements not in b
        call    rt_set_new
        mov     ebp,eax
        xor     edi,edi
.wl:    cmp     edi,[ebx+20]
        ja      .wdone
        mov     eax,edi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     .wn
        push    eax
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    esi
        call    rt_set_find_h
        add     esp,16
        pop     edx
        test    eax,eax
        jnz     .wn
        push    dword [edx+12]
        push    dword [edx+8]
        push    edx
        push    ebp
        call    rt_set_add_h
        add     esp,16
.wn:    inc     edi
        jmp     .wl
.wdone: mov     eax,ebp
        jmp     .out
.xor:   mov     eax,esi         ; ^: a copy of b, a's elements toggled in
        mov     edx,[esi+24]
        call    rt_set_copy
        mov     ebp,eax
        mov     edx,ebx
        call    rt_set_xor_update
        mov     eax,ebp
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_set_iop : rt_set_op rt_set_merge rt_set_diff_update rt_set_xor_update rt_decref
rt_set_iop:                     ; eax = a, edx = b, ecx = 0 |=, 1 &=, 2 -=, 3 ^=: a changed in place (CPython's way)
        test    ecx,ecx
        jz      rt_set_merge
        cmp     ecx,2
        je      rt_set_diff_update
        cmp     ecx,3
        je      rt_set_xor_update
        push    ebx esi edi     ; &=: the intersection's table becomes a's
        mov     ebx,eax
        call    rt_set_op
        mov     edi,eax
        mov     eax,[ebx+16]
        mov     ecx,[edi+16]
        mov     [ebx+16],ecx
        mov     [edi+16],eax
        mov     eax,[ebx+20]
        mov     ecx,[edi+20]
        mov     [ebx+20],ecx
        mov     [edi+20],eax
        mov     eax,[ebx+8]
        mov     ecx,[edi+8]
        mov     [ebx+8],ecx
        mov     [edi+8],eax
        mov     eax,[ebx+12]
        mov     ecx,[edi+12]
        mov     [ebx+12],ecx
        mov     [edi+12],eax
        mov     dword [ebx+28],0
        mov     eax,edi         ; a's old elements go with it
        call    rt_decref
        pop     edi esi ebx
        ret

;;; code rt_set_le : rt_set_find_h
rt_set_le:                      ; eax = a, edx = b -> eax = 1 if every element of a is in b
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        xor     eax,eax
        test    ebx,ebx
        jz      .yes
        mov     ecx,[ebx+8]
        test    esi,esi
        jz      .empty
        cmp     ecx,[esi+8]
        ja      .no
        xor     edi,edi
.l:     cmp     edi,[ebx+20]
        ja      .yes
        mov     eax,edi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     .n
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    esi
        call    rt_set_find_h
        add     esp,16
        test    eax,eax
        jz      .no
.n:     inc     edi
        jmp     .l
.empty: test    ecx,ecx
        jnz     .no
.yes:   mov     eax,1
        pop     edi esi ebx
        ret
.no:    xor     eax,eax
        pop     edi esi ebx
        ret

;;; code rt_set_eq : rt_set_le
rt_set_eq:                      ; eax = a, edx = b -> eax = 1 if the same elements
        cmp     eax,edx
        je      .yes
        test    eax,eax
        jz      .no
        test    edx,edx
        jz      .no
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
@@:     push    ecx
        xor     ecx,ecx
        test    edx,edx
        jz      @f
        mov     ecx,[edx+8]
@@:     cmp     ecx,[esp]
        pop     ecx
        jne     .no
        jmp     rt_set_le
.yes:   mov     eax,1
        ret
.no:    xor     eax,eax
        ret

;;; code rt_set_disjoint : rt_set_find_h
rt_set_disjoint:                ; eax = a, edx = b -> eax = 1 if no element is in both
        push    ebx esi edi
        mov     ebx,eax
        mov     esi,edx
        test    ebx,ebx
        jz      .yes
        test    esi,esi
        jz      .yes
        mov     eax,[esi+8]     ; walk the smaller
        cmp     eax,[ebx+8]
        jae     @f
        xchg    ebx,esi
@@:     xor     edi,edi
.l:     cmp     edi,[ebx+20]
        ja      .yes
        mov     eax,edi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     .n
        push    dword [eax+12]
        push    dword [eax+8]
        push    eax
        push    esi
        call    rt_set_find_h
        add     esp,16
        test    eax,eax
        jnz     .no
.n:     inc     edi
        jmp     .l
.yes:   mov     eax,1
        pop     edi esi ebx
        ret
.no:    xor     eax,eax
        pop     edi esi ebx
        ret

;;; code rt_set_eq_k : rt_set_eq
rt_set_eq_k:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_set_eq

;;; code rt_set_repr_k : rt_sb_char rt_sb_cstr rt_repr_none
rt_set_repr_k:                  ; repr of a set (for messages)
        cmp     dword [eax],0
        je      rt_repr_none
        push    ebx esi edi
        mov     ebx,[eax]
        test    ebx,ebx
        jz      .empty
        cmp     dword [ebx+8],0
        je      .empty
        mov     al,'{'
        call    rt_sb_char
        xor     esi,esi
        xor     edi,edi
.l:     cmp     esi,[ebx+20]
        ja      .end
        mov     eax,esi
        imul    eax,24
        add     eax,[ebx+16]
        cmp     dword [eax+16],1
        jne     .n
        test    edi,edi
        jz      @f
        push    eax
        mov     eax,rt_s_ssep
        call    rt_sb_cstr
        pop     eax
@@:     inc     edi
        mov     ecx,[ebx+24]
        call    dword [ecx+20]
.n:     inc     esi
        jmp     .l
.end:   mov     al,'}'
        pop     edi esi ebx
        jmp     rt_sb_char
.empty: mov     eax,rt_s_sempty
        pop     edi esi ebx
        jmp     rt_sb_cstr
;;; data rt_set_repr_k
rt_s_ssep       db ', ',0
rt_s_sempty     db 'set()',0

