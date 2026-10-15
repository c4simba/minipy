; ---------------------------------------------------------------- lists
; list: +8 length, +12 capacity, +16 the element array, +20 the element type (kd)

;;; code rt_list_new : rt_alloc rt_list_free
rt_list_new:                    ; eax = element type -> eax = new empty list
        push    eax
        mov     eax,24
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_list_free
        mov     [eax+20],ecx
if defined RT_GC
        test    byte [ecx+4],8
        jz      @f
        or      byte [eax-3],1  ; its elements can be in a cycle: so can it
@@:
end if
        ret

;;; code rt_list_free : rt_free rt_decref
rt_list_free:                   ; destroy routine of lists
        push    ebx esi
        mov     ebx,eax
        mov     ecx,[ebx+20]
        test    byte [ecx+4],1
        jz      .items
        mov     esi,[ebx+8]
.drop:  dec     esi
        js      .items
        mov     eax,[ebx+16]
        mov     eax,[eax+esi*4]
        call    rt_decref
        jmp     .drop
.items: mov     eax,[ebx+16]
        call    rt_free
        mov     eax,ebx
        pop     esi ebx
        jmp     rt_free

;;; code rt_list_reserve : rt_alloc rt_free
rt_list_reserve:                ; eax = list, ecx = wanted length: room for it
        cmp     ecx,[eax+12]
        jbe     .ok
        push    ebx esi edi
        mov     ebx,eax
        mov     eax,[ebx+12]
        add     eax,eax
        cmp     eax,ecx
        jae     @f
        mov     eax,ecx
@@:     cmp     eax,4
        jae     @f
        mov     eax,4
@@:     mov     [ebx+12],eax
        mov     edx,[ebx+20]
        mul     dword [edx]
        call    rt_alloc
        push    eax
        mov     edi,eax
        mov     esi,[ebx+16]
        mov     ecx,[ebx+20]
        mov     ecx,[ecx]
        imul    ecx,[ebx+8]
        rep     movsb
        mov     eax,[ebx+16]
        call    rt_free
        pop     dword [ebx+16]
        mov     eax,ebx
        pop     edi esi ebx
.ok:    ret

;;; code rt_list_add32 : rt_list_push
rt_list_add32:                  ; eax = a 4-byte value, the list at [esp+4] (the caller's top): appended
        push    eax
        mov     eax,[esp+8]
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        ret

;;; code rt_list_add64 : rt_list_push
rt_list_add64:                  ; edx:eax = an 8-byte value, the list at [esp+4] (the caller's top): appended
        push    edx
        push    eax
        mov     eax,[esp+12]
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        pop     ecx
        mov     [eax+4],ecx
        ret

;;; code rt_list_addf : rt_list_push
rt_list_addf:                   ; st0 = a float (popped), the list at [esp+4] (the caller's top): appended
        mov     eax,[esp+4]
        call    rt_list_push
        fstp    qword [eax]
        ret

;;; code rt_list_push : rt_list_reserve rt_panic_none
rt_list_push:                   ; eax = list -> eax = address of a new last slot
        test    eax,eax
        jz      rt_panic_none
        push    ebx
        mov     ebx,eax
        mov     ecx,[eax+8]
        inc     ecx
        call    rt_list_reserve
        mov     eax,[ebx+8]
        inc     dword [ebx+8]
        mov     ecx,[ebx+20]
        imul    eax,[ecx]
        add     eax,[ebx+16]
        pop     ebx
        ret

;;; code rt_list_at : rt_panic_index
rt_list_at:                     ; eax = list, edx = index (negative: from the end) -> eax = address of the element
        test    eax,eax
        jz      rt_panic_index
        test    edx,edx
        jns     @f
        add     edx,[eax+8]
@@:     cmp     edx,[eax+8]
        jae     rt_panic_index
        mov     ecx,[eax+20]
        imul    edx,[ecx]
        mov     eax,[eax+16]
        add     eax,edx
        ret

;;; code rt_list_set_at : rt_panic_index
rt_list_set_at:                 ; rt_list_at for an assignment (its own message)
        test    eax,eax
        jz      .bad
        test    edx,edx
        jns     @f
        add     edx,[eax+8]
@@:     cmp     edx,[eax+8]
        jae     .bad
        mov     ecx,[eax+20]
        imul    edx,[ecx]
        mov     eax,[eax+16]
        add     eax,edx
        ret
.bad:   mov     esi,rt_msg_lassign
        jmp     rt_index_error
;;; data rt_list_set_at
rt_msg_lassign  db 'list assignment index out of range',0

;;; code rt_list_pop : rt_panic_index rt_scratch
rt_list_pop:                    ; eax = list, edx = index (negative: from the end) -> eax = rt_scratch holding the removed element
        push    ebx esi edi
        mov     ebx,eax
        test    ebx,ebx
        jz      .empty
        mov     ecx,[ebx+8]
        test    ecx,ecx
        jz      .empty
        test    edx,edx
        jns     @f
        add     edx,ecx
@@:     cmp     edx,ecx
        jae     .range
        mov     ecx,[ebx+20]
        mov     ecx,[ecx]       ; element size
        mov     esi,edx
        imul    esi,ecx
        add     esi,[ebx+16]    ; the element
        mov     eax,[esi]
        mov     [rt_scratch],eax
        mov     eax,[esi+ecx-4]
        mov     [rt_scratch+ecx-4],eax
        mov     eax,[ebx+8]
        dec     eax
        mov     [ebx+8],eax
        sub     eax,edx         ; elements after it
        imul    eax,ecx
        mov     edi,esi
        add     esi,ecx
        mov     ecx,eax
        rep     movsb
        mov     eax,rt_scratch
        pop     edi esi ebx
        ret
.empty: mov     esi,rt_msg_popempty
        jmp     rt_index_error
.range: mov     esi,rt_msg_poprange
        jmp     rt_index_error
;;; data rt_list_pop
rt_msg_popempty db 'pop from empty list',0
rt_msg_poprange db 'pop index out of range',0

;;; code rt_list_insert : rt_list_reserve rt_panic_none
rt_list_insert:                 ; eax = list, edx = index -> eax = address of a new slot there (zero)
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,[ebx+20]
        mov     ebp,[ebp]       ; element size
        mov     eax,[ebx+8]
        test    edx,edx
        jns     @f
        add     edx,eax
        jns     @f
        xor     edx,edx
@@:     cmp     edx,eax
        jbe     @f
        mov     edx,eax
@@:     push    edx
        lea     ecx,[eax+1]
        mov     eax,ebx
        call    rt_list_reserve
        pop     edx
        mov     ecx,[ebx+8]
        sub     ecx,edx
        imul    ecx,ebp         ; bytes to move up
        mov     esi,[ebx+8]
        imul    esi,ebp
        add     esi,[ebx+16]
        dec     esi
        lea     edi,[esi+ebp]
        std
        rep     movsb
        cld
        inc     dword [ebx+8]
        mov     eax,edx
        imul    eax,ebp
        add     eax,[ebx+16]
        mov     dword [eax],0
        mov     dword [eax+ebp-4],0
        pop     ebp edi esi ebx
        ret

;;; code rt_list_find
rt_list_find:                   ; eax = list, edx = &value -> eax = index of the first equal element, or -1
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        xor     esi,esi
        test    ebx,ebx
        jz      .nf
.l:     cmp     esi,[ebx+8]
        jae     .nf
        mov     ebp,[ebx+20]
        mov     eax,esi
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     edx,edi
        mov     ecx,ebp
        call    dword [ebp+8]
        test    eax,eax
        jnz     .found
        inc     esi
        jmp     .l
.found: mov     eax,esi
        pop     ebp edi esi ebx
        ret
.nf:    or      eax,-1
        pop     ebp edi esi ebx
        ret

;;; code rt_list_count
rt_list_count:                  ; eax = list, edx = &value -> eax = how many elements equal it
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        xor     esi,esi
        push    0
        test    ebx,ebx
        jz      .out
.l:     cmp     esi,[ebx+8]
        jae     .out
        mov     ebp,[ebx+20]
        mov     eax,esi
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     edx,edi
        mov     ecx,ebp
        call    dword [ebp+8]
        add     [esp],eax
        inc     esi
        jmp     .l
.out:   pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_list_index : rt_list_find rt_panic_value
rt_list_index:                  ; eax = list, edx = &value -> eax = index of the first equal element (ValueError if none)
        call    rt_list_find
        test    eax,eax
        js      .bad
        ret
.bad:   mov     esi,rt_msg_lindex
        jmp     rt_panic_value
;;; data rt_list_index
rt_msg_lindex   db 'list.index(x): x not in list',0

;;; code rt_list_remove : rt_list_find rt_list_pop rt_decref rt_panic_value
rt_list_remove:                 ; eax = list, edx = &value: remove the first equal element
        push    ebx
        mov     ebx,eax
        call    rt_list_find
        test    eax,eax
        js      .bad
        mov     edx,eax
        mov     eax,ebx
        call    rt_list_pop
        mov     ecx,[ebx+20]
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[eax]
        call    rt_decref
@@:     pop     ebx
        ret
.bad:   mov     esi,rt_msg_lremove
        jmp     rt_panic_value
;;; data rt_list_remove
rt_msg_lremove  db 'list.remove(x): x not in list',0

;;; code rt_list_reverse
rt_list_reverse:                ; eax = list: reverse it in place
        push    ebx esi edi
        test    eax,eax
        jz      .out
        mov     ebx,[eax+20]
        mov     ebx,[ebx]       ; element size
        mov     ecx,[eax+8]
        mov     esi,[eax+16]
        dec     ecx
        js      .out
        imul    ecx,ebx
        lea     edi,[esi+ecx]
.l:     cmp     esi,edi
        jae     .out
        mov     eax,[esi]
        mov     edx,[edi]
        mov     [esi],edx
        mov     [edi],eax
        cmp     ebx,8
        jne     @f
        mov     eax,[esi+4]
        mov     edx,[edi+4]
        mov     [esi+4],edx
        mov     [edi+4],eax
@@:     add     esi,ebx
        sub     edi,ebx
        jmp     .l
.out:   pop     edi esi ebx
        ret

;;; code rt_list_sort : rt_alloc rt_free
; A stable merge sort (equal elements keep their order, as in CPython) of
; pointers to the elements; then the elements are copied into that order.
rt_list_sort:                   ; eax = list: sort it in place, ascending
        push    ebx esi edi ebp
        mov     ebx,eax
        test    ebx,ebx
        jz      .out
        mov     ecx,[ebx+8]
        cmp     ecx,2
        jb      .out
        sub     esp,32          ; +0 pointers, +4 the other array, +8 n, +12 run width, +16 mid, +20 hi, +24 k, +28 new elements
        mov     [esp+8],ecx
        lea     eax,[ecx*4]
        call    rt_alloc
        mov     [esp],eax
        mov     eax,[esp+8]
        shl     eax,2
        call    rt_alloc
        mov     [esp+4],eax
        mov     edi,[esp]
        mov     esi,[ebx+16]
        mov     edx,[ebx+20]
        mov     edx,[edx]
        mov     ecx,[esp+8]
@@:     mov     [edi],esi
        add     edi,4
        add     esi,edx
        dec     ecx
        jnz     @b
        mov     dword [esp+12],1
.pass:  mov     eax,[esp+12]
        cmp     eax,[esp+8]
        jae     .sorted
        xor     esi,esi         ; lo
.run:   cmp     esi,[esp+8]
        jae     .swap
        mov     eax,esi
        add     eax,[esp+12]
        cmp     eax,[esp+8]
        jbe     @f
        mov     eax,[esp+8]
@@:     mov     [esp+16],eax    ; mid
        add     eax,[esp+12]
        cmp     eax,[esp+8]
        jbe     @f
        mov     eax,[esp+8]
@@:     mov     [esp+20],eax    ; hi
        mov     edi,esi         ; i
        mov     ebp,[esp+16]    ; j
        mov     [esp+24],esi    ; k
.m:     mov     ecx,[esp+24]
        cmp     ecx,[esp+20]
        jae     .mdone
        cmp     edi,[esp+16]
        jae     .takej
        cmp     ebp,[esp+20]
        jae     .takei
        mov     eax,[esp]
        mov     edx,[eax+edi*4]
        mov     eax,[eax+ebp*4]
        mov     ecx,[ebx+20]
        call    dword [ecx+12]  ; a[j] < a[i]: j first
        test    eax,eax
        js      .takej
.takei: mov     eax,[esp]
        mov     eax,[eax+edi*4]
        inc     edi
        jmp     .put
.takej: mov     eax,[esp]
        mov     eax,[eax+ebp*4]
        inc     ebp
.put:   mov     ecx,[esp+4]
        mov     edx,[esp+24]
        mov     [ecx+edx*4],eax
        inc     dword [esp+24]
        jmp     .m
.mdone: mov     esi,[esp+20]
        jmp     .run
.swap:  mov     eax,[esp]
        mov     ecx,[esp+4]
        mov     [esp],ecx
        mov     [esp+4],eax
        shl     dword [esp+12],1
        jmp     .pass
.sorted:
        mov     eax,[ebx+12]
        mov     ecx,[ebx+20]
        mul     dword [ecx]
        call    rt_alloc
        mov     [esp+28],eax
        mov     edi,eax
        mov     esi,[esp]
        mov     ecx,[esp+8]
        mov     edx,[ebx+20]
        mov     edx,[edx]
.cp:    mov     eax,[esi]
        push    ecx
        mov     ecx,[eax]
        mov     [edi],ecx
        mov     ecx,[eax+edx-4]
        mov     [edi+edx-4],ecx
        pop     ecx
        add     esi,4
        add     edi,edx
        dec     ecx
        jnz     .cp
        mov     eax,[ebx+16]
        call    rt_free
        mov     eax,[esp+28]
        mov     [ebx+16],eax
        mov     eax,[esp]
        call    rt_free
        mov     eax,[esp+4]
        call    rt_free
        add     esp,32
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_extend : rt_list_push rt_elem_copy rt_panic_none
rt_list_extend:                 ; eax = destination, edx = source: append the source's elements
        test    eax,eax
        jz      rt_panic_none
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        test    esi,esi
        jz      .out
        mov     ebp,[esi+8]     ; a list extended by itself: its old length
        xor     edi,edi
.l:     cmp     edi,ebp
        jae     .out
        mov     eax,ebx
        call    rt_list_push
        mov     ecx,[esi+20]
        mov     edx,edi
        imul    edx,[ecx]
        add     edx,[esi+16]
        call    rt_elem_copy
        inc     edi
        jmp     .l
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_copy : rt_list_new rt_list_reserve rt_list_extend
rt_list_copy:                   ; eax = list, edx = its element type -> eax = new list of its elements
        push    ebx
        mov     ebx,eax
        mov     eax,edx
        call    rt_list_new
        test    ebx,ebx
        jz      .out
        push    eax
        mov     ecx,[ebx+8]
        call    rt_list_reserve
        mov     eax,[esp]
        mov     edx,ebx
        call    rt_list_extend
        pop     eax
.out:   pop     ebx
        ret

;;; code rt_list_concat : rt_list_copy rt_list_extend
rt_list_concat:                 ; eax = a, edx = b, ecx = element type -> eax = new list a + b
        push    ebx
        mov     ebx,edx
        mov     edx,ecx
        call    rt_list_copy
        push    eax
        mov     edx,ebx
        call    rt_list_extend
        pop     eax
        pop     ebx
        ret

;;; code rt_list_mul : rt_list_new rt_list_extend
rt_list_mul:                    ; eax = list, edx = n, ecx = element type -> eax = new list: n times its elements
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        mov     eax,ecx
        call    rt_list_new
        push    eax
.l:     test    esi,esi
        jle     .out
        mov     eax,[esp]
        mov     edx,ebx
        call    rt_list_extend
        dec     esi
        jmp     .l
.out:   pop     eax
        pop     esi ebx
        ret

;;; code rt_list_del_slice : rt_slice_range rt_decref
rt_list_del_slice:              ; [esp+4] list, lo, hi, step, flags: del list[lo:hi:step]
        push    ebx esi edi ebp
        mov     ebx,[esp+20]
        mov     eax,[ebx+8]
        lea     esi,[esp+24]
        call    rt_slice_range          ; eax = start, edx = step, ecx = count
        jecxz   .out
        test    edx,edx                 ; going down: the same items going up
        jns     @f
        push    edx
        imul    edx,ecx
        sub     edx,[esp]               ; (count - 1) * step
        add     eax,edx
        pop     edx
        neg     edx
@@:     push    ecx edx eax             ; [esp] first, [esp+4] step, [esp+8] count
        mov     ebp,[ebx+20]            ; kd
        xor     esi,esi                 ; from
        xor     edi,edi                 ; to
.l:     cmp     esi,[ebx+8]
        jae     .done
        mov     eax,esi                 ; going: first <= i, (i - first) % step == 0, within count
        sub     eax,[esp]
        jb      .keep
        xor     edx,edx
        div     dword [esp+4]
        test    edx,edx
        jnz     .keep
        cmp     eax,[esp+8]
        jae     .keep
        test    byte [ebp+4],1          ; a reference: released
        jz      .next
        mov     eax,[ebp]
        imul    eax,esi
        add     eax,[ebx+16]
        mov     eax,[eax]
        call    rt_decref
        jmp     .next
.keep:  cmp     esi,edi
        je      .same
        push    esi edi
        mov     ecx,[ebp]
        mov     eax,ecx
        imul    eax,esi
        imul    edi,ecx
        add     edi,[ebx+16]
        mov     esi,eax
        add     esi,[ebx+16]
        rep     movsb
        pop     edi esi
.same:  inc     edi
.next:  inc     esi
        jmp     .l
.done:  mov     [ebx+8],edi
        add     esp,12
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_set_slice : rt_slice_range rt_list_reserve rt_list_copy rt_elem_copy rt_decref rt_sb_need rt_sb_cstr rt_sb_int rt_sb_take rt_raise_valerr
rt_list_set_slice:              ; [esp+4] list, lo, hi, step, flags, a list of the new items: list[lo:hi:step] = them
        push    ebx esi edi ebp
        mov     ebx,[esp+20]
        mov     eax,[esp+40]
        cmp     eax,ebx                 ; xs[1:2] = xs: a copy first
        jne     @f
        mov     edx,[ebx+20]
        call    rt_list_copy
        mov     [esp+40],eax
        push    eax
        jmp     .go
@@:     push    0                       ; [esp] a copy to release
.go:    mov     eax,[ebx+8]
        lea     esi,[esp+28]
        call    rt_slice_range          ; eax = start, edx = step, ecx = count
        mov     ebp,[ebx+20]            ; kd
        mov     edi,[esp+44]            ; the new items
        test    dword [esp+40],4        ; no step, or 1: any number of items
        jnz     .run
        cmp     edx,1
        je      .run                    ; else as many items as places
        cmp     ecx,[edi+8]
        jne     .size
        push    eax edx ecx             ; replace them one by one
.one:   cmp     dword [esp],0
        je      .ones
        mov     eax,[esp+8]             ; place
        mov     ecx,[ebp]
        imul    eax,ecx
        add     eax,[ebx+16]
        test    byte [ebp+4],1
        jz      @f
        push    eax
        mov     eax,[eax]
        call    rt_decref
        pop     eax
@@:     mov     edx,[edi+8]             ; the item: index count of them - left
        sub     edx,[esp]
        imul    edx,[ebp]
        add     edx,[edi+16]
        mov     ecx,ebp
        call    rt_elem_copy
        mov     eax,[esp+4]
        add     [esp+8],eax
        dec     dword [esp]
        jmp     .one
.ones:  add     esp,12
        jmp     .out
.run:   push    eax ecx                 ; [esp] count, [esp+4] start: release them, make room, copy
        mov     esi,eax
.rel:   test    ecx,ecx
        jz      .relz
        test    byte [ebp+4],1
        jz      .relz
        mov     eax,[ebp]
        imul    eax,esi
        add     eax,[ebx+16]
        push    ecx
        mov     eax,[eax]
        call    rt_decref
        pop     ecx
        inc     esi
        dec     ecx
        jmp     .rel
.relz:  mov     ecx,[ebx+8]             ; the new length
        sub     ecx,[esp]
        add     ecx,[edi+8]
        push    ecx
        mov     eax,ebx
        call    rt_list_reserve
        pop     ecx
        push    ecx
        mov     eax,[esp+8]             ; the tail: from start + count to start + len(new)
        add     eax,[esp+4]
        mov     ecx,[ebx+8]
        sub     ecx,eax                 ; its items
        imul    ecx,[ebp]
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     esi,eax
        mov     eax,[esp+8]
        add     eax,[edi+8]
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     edi,eax
        cmp     edi,esi
        jbe     .fwd
        lea     esi,[esi+ecx-1]
        lea     edi,[edi+ecx-1]
        std
        rep     movsb
        cld
        jmp     .moved
.fwd:   rep     movsb
.moved: pop     ecx
        mov     [ebx+8],ecx
        mov     edi,[esp+52]            ; the new items, copied in (the stack: count, start, a copy, 4 registers, the return, 6 arguments)
        xor     esi,esi
.cp:    cmp     esi,[edi+8]
        jae     .cpd
        mov     eax,[esp+4]
        add     eax,esi
        imul    eax,[ebp]
        add     eax,[ebx+16]
        mov     edx,esi
        imul    edx,[ebp]
        add     edx,[edi+16]
        mov     ecx,ebp
        call    rt_elem_copy
        inc     esi
        jmp     .cp
.cpd:   add     esp,8
.out:   pop     eax                     ; the copy (or 0)
        call    rt_decref
        pop     ebp edi esi ebx
        ret
.size:  push    ecx                     ; ValueError: attempt to assign sequence of size n to extended slice of size m
        push    dword [edi+8]
        push    dword [rt_sb_len]
        mov     eax,rt_msg_extslice1
        call    rt_sb_cstr
        mov     eax,[esp+4]
        xor     edx,edx
        call    rt_sb_int
        mov     eax,rt_msg_extslice2
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_list_set_slice
rt_msg_extslice1 db 'attempt to assign sequence of size ',0
rt_msg_extslice2 db ' to extended slice of size ',0

;;; code rt_list_slice : rt_slice_range rt_list_new rt_list_push rt_elem_copy
rt_list_slice:                  ; [esp+4] list, lo, hi, step, flags, element type -> eax = new list
        push    ebx esi edi ebp
        mov     ebx,[esp+20]
        xor     eax,eax
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+8]
@@:     lea     esi,[esp+24]
        call    rt_slice_range
        mov     ebp,ecx         ; count
        mov     esi,eax         ; index
        mov     edi,edx         ; step
        mov     eax,[esp+40]
        call    rt_list_new
        push    eax
.l:     test    ebp,ebp
        jz      .out
        mov     eax,[esp]
        call    rt_list_push
        mov     ecx,[ebx+20]
        mov     edx,esi
        imul    edx,[ecx]
        add     edx,[ebx+16]
        call    rt_elem_copy
        add     esi,edi
        dec     ebp
        jmp     .l
.out:   pop     eax
        pop     ebp edi esi ebx
        ret

;;; code rt_list_clear : rt_decref
rt_list_clear:                  ; eax = list: remove every element
        test    eax,eax
        jz      .out
        mov     ecx,[eax+20]
        test    byte [ecx+4],1
        jnz     .drop
        mov     dword [eax+8],0
.out:   ret
.drop:  push    ebx
        mov     ebx,eax
@@:     mov     eax,[ebx+8]
        test    eax,eax
        jz      @f
        dec     eax
        mov     [ebx+8],eax
        mov     ecx,[ebx+16]
        mov     eax,[ecx+eax*4]
        call    rt_decref
        jmp     @b
@@:     pop     ebx
        ret

;;; code rt_list_eq
rt_list_eq:                     ; eax = a, edx = b (lists of one type) -> eax = 1 if equal
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        cmp     ebx,edi
        je      .yes
        test    ebx,ebx         ; None and a list
        jz      .no
        test    edi,edi
        jz      .no
        xor     eax,eax
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+8]
@@:     xor     edx,edx
        test    edi,edi
        jz      @f
        mov     edx,[edi+8]
@@:     cmp     eax,edx
        jne     .no
        xor     esi,esi
.l:     cmp     esi,[ebx+8]
        jae     .yes
        cmp     esi,[edi+8]
        jae     .no
        mov     ebp,[ebx+20]
        mov     eax,esi
        imul    eax,[ebp]
        mov     edx,eax
        add     eax,[ebx+16]
        add     edx,[edi+16]
        mov     ecx,ebp
        call    dword [ebp+8]
        test    eax,eax
        jz      .no
        inc     esi
        jmp     .l
.yes:   mov     eax,1
        pop     ebp edi esi ebx
        ret
.no:    xor     eax,eax
        pop     ebp edi esi ebx
        ret

;;; code rt_list_cmp
rt_list_cmp:                    ; eax = a, edx = b -> eax = -1, 0, 1: the first unequal elements decide, else the lengths
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     edi,edx
        xor     esi,esi
.l:     xor     eax,eax
        test    ebx,ebx
        jz      @f
        mov     eax,[ebx+8]
@@:     xor     edx,edx
        test    edi,edi
        jz      @f
        mov     edx,[edi+8]
@@:     cmp     esi,eax
        jae     .end
        cmp     esi,edx
        jae     .end
        mov     ebp,[ebx+20]
        mov     eax,esi
        imul    eax,[ebp]
        mov     edx,eax
        add     eax,[ebx+16]
        add     edx,[edi+16]
        push    eax edx
        mov     ecx,ebp
        call    dword [ebp+8]
        pop     edx ecx
        test    eax,eax
        jz      .diff
        inc     esi
        jmp     .l
.diff:  mov     eax,ecx
        mov     ecx,ebp
        call    dword [ebp+12]
        jmp     .out
.end:   cmp     eax,edx
        mov     eax,0
        je      .out
        jb      @f
        inc     eax
        jmp     .out
@@:     dec     eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_list_sum
; floats: CPython's compensated (Neumaier) sum
rt_list_sum:                    ; eax = list of int (-> eax) or float (-> st0)
        push    esi
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
        mov     esi,[eax+16]
        mov     edx,[eax+20]
        test    byte [edx+4],2
        jnz     .f
@@:     xor     eax,eax
        jecxz   .out
@@:     add     eax,[esi]
        add     esi,4
        dec     ecx
        jnz     @b
.out:   pop     esi
        ret
.f:     fldz                    ; st0 = c, the compensation
        fldz                    ; st0 = sum
        jecxz   .fdone
.fl:    fld     qword [esi]     ; x, sum, c
        fld     st1
        fadd    st0,st1         ; t = sum + x
        fld     st2
        fabs
        fld     st2
        fabs
        fcompp                  ; |x| vs |sum|
        fnstsw  ax
        sahf
        ja      .xbig           ; |x| > |sum|
        fld     st2             ; c += (sum - t) + x
        fsub    st0,st1
        fadd    st0,st2
        faddp   st4,st0
        jmp     .fn
.xbig:  fld     st1             ; c += (x - t) + sum
        fsub    st0,st1
        fadd    st0,st3
        faddp   st4,st0
.fn:    fstp    st2             ; sum = t; drop x
        fstp    st0
        add     esi,8
        dec     ecx
        jnz     .fl
.fdone: fxch                    ; c added when it is a finite non-zero
        fld     st0
        fsub    st0,st0
        fcomp   st0             ; finite: c - c == c - c (not NaN)
        fnstsw  ax
        sahf
        jp      .nofix
        ftst
        fnstsw  ax
        sahf
        je      .nofix
        faddp   st1,st0
        pop     esi
        ret
.nofix: fstp    st0
        pop     esi
        ret

;;; code rt_list_minmax : rt_panic_value
rt_list_minmax:                 ; eax = list, edx = 1 for max -> eax = address of the smallest / largest element (the first of equals)
        push    ebx esi edi ebp
        mov     ebx,eax
        mov     ebp,edx
        test    ebx,ebx
        jz      .empty
        cmp     dword [ebx+8],0
        je      .empty
        mov     edi,[ebx+16]    ; the best so far
        mov     esi,1
.l:     cmp     esi,[ebx+8]
        jae     .done
        mov     ecx,[ebx+20]
        mov     eax,esi
        imul    eax,[ecx]
        add     eax,[ebx+16]
        push    eax
        mov     edx,edi
        call    dword [ecx+12]  ; cmp(x, best)
        pop     edx
        test    ebp,ebp
        jnz     .max
        test    eax,eax
        jns     .n
        mov     edi,edx
        jmp     .n
.max:   test    eax,eax
        jle     .n
        mov     edi,edx
.n:     inc     esi
        jmp     .l
.done:  mov     eax,edi
        pop     ebp edi esi ebx
        ret
.empty: mov     esi,rt_msg_minempty
        test    ebp,ebp
        jz      @f
        mov     esi,rt_msg_maxempty
@@:     jmp     rt_panic_value
;;; data rt_list_minmax
rt_msg_minempty db 'min() iterable argument is empty',0
rt_msg_maxempty db 'max() iterable argument is empty',0

;;; code rt_list_eq_k : rt_list_eq
rt_list_eq_k:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_list_eq

;;; code rt_list_cmp_k : rt_list_cmp rt_cmp_none
rt_list_cmp_k:
        mov     eax,[eax]
        mov     edx,[edx]
        test    eax,eax
        jz      .ln
        test    edx,edx
        jnz     rt_list_cmp
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_list_repr_k : rt_sb_char rt_sb_cstr rt_repr_none
rt_list_repr_k:                 ; repr of a list (for messages)
        cmp     dword [eax],0
        je      rt_repr_none
        push    ebx esi
        mov     ebx,[eax]
        mov     al,'['
        call    rt_sb_char
        xor     esi,esi
.l:     test    ebx,ebx
        jz      .end
        cmp     esi,[ebx+8]
        jae     .end
        test    esi,esi
        jz      @f
        mov     eax,rt_s_lsep
        call    rt_sb_cstr
@@:     mov     ecx,[ebx+20]
        mov     eax,esi
        imul    eax,[ecx]
        add     eax,[ebx+16]
        call    dword [ecx+20]
        inc     esi
        jmp     .l
.end:   mov     al,']'
        pop     esi ebx
        jmp     rt_sb_char
;;; data rt_list_repr_k
rt_s_lsep       db ', ',0

