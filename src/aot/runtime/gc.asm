; ---------------------------------------------------------------- cycles
; Reference counting leaves objects that refer to each other alive once
; nothing else refers to them. The code generator knows from the types which
; objects can be part of such a cycle at all (src/aot/codegen/gc.c): those
; get the CYC mark in their block header when made - the second byte of the
; header ([object-3]): 1 CYC, 2 noted, 4/8/12 gray/white/purple (0 black).
; When a CYC object loses a reference but lives on (rt_decref), it is noted
; as a possible root of a garbage cycle; once enough are noted, rt_gc_collect
; looks at them all (Bacon and Rajan's synchronous cycle collection): the
; references among the CYC objects they lead to are taken off the counts;
; what still has a count is referred to from outside and stays, with all it
; leads to; the rest refers only to itself and is freed. Static objects
; (counts from 0x40000000) are never looked at. A CYC object's references:
; a list's, dict's, set's or tuple's elements of CYC types (their kd: flag
; 8), a cell's value, a bound method's object, and the fields of a class's
; objects and the captures of a closure that rt_gc_table lists.
rt_gc_root:                     ; eax = a CYC object that lost a reference and lives on: noted (rt_decref; every register kept)
        cmp     dword [eax],0x20000000
        jae     .out            ; (a static object: what precedes it only looked like a mark)
        push    ecx
        mov     cl,[eax-3]
        or      cl,0x0C         ; purple
        test    cl,2
        jnz     .noted
        or      cl,2
        mov     [eax-3],cl
        push    eax edx ebx
        mov     ebx,rt_gc_rootv
        call    rt_gc_push
        mov     eax,[ebx+4]
        cmp     eax,[rt_gc_limit]
        jb      @f
        cmp     dword [rt_gc_busy],0
        jne     @f
        call    rt_gc_collect
@@:     pop     ebx edx eax ecx
        ret
.noted: mov     [eax-3],cl
        pop     ecx
.out:   ret
;;; data rt_gc_root
align 4
rt_gc_limit     dd 1000         ; noted objects that start a collection
;;; bss rt_gc_root
rt_gc_rootv     rd 3            ; the noted objects (a vector: items, count, room)
rt_gc_busy      rd 1

;;; code rt_gc_push : rt_os_alloc rt_os_free
rt_gc_push:                     ; ebx = a vector (+0 items, +4 count, +8 room), eax = a dword appended (eax kept)
        mov     ecx,[ebx+4]
        cmp     ecx,[ebx+8]
        jae     .grow
.put:   mov     edx,[ebx]
        mov     [edx+ecx*4],eax
        inc     ecx
        mov     [ebx+4],ecx
        ret
.grow:  push    eax esi edi     ; (memory from the system: not counted as the program's blocks)
        mov     eax,[ebx+8]
        add     eax,eax
        jnz     @f
        mov     eax,1024
@@:     push    eax
        shl     eax,2
        call    rt_os_alloc
        mov     edi,eax
        mov     esi,[ebx]
        mov     ecx,[ebx+4]
        rep     movsd
        mov     edx,[ebx+8]
        xchg    eax,[ebx]
        pop     ecx
        mov     [ebx+8],ecx
        test    eax,eax
        jz      @f
        shl     edx,2
        call    rt_os_free
@@:     pop     edi esi eax
        mov     ecx,[ebx+4]
        jmp     .put

;;; code rt_gc_kids : rt_gc_room rt_list_free rt_dict_free rt_set_free rt_tuple_free rt_cell_free rt_adfree
rt_gc_kids:                     ; eax = a CYC object -> rt_gc_kidv: the addresses of its slots that may hold CYC objects, ecx = their number
        push    ebx esi edi ebp
        mov     esi,eax
        mov     edx,[esi+4]     ; what it is: by its destroy routine
        cmp     edx,rt_list_free
        je      .list
        cmp     edx,rt_dict_free
        je      .dict
        cmp     edx,rt_set_free
        je      .set
        cmp     edx,rt_tuple_free
        je      .tuple
        cmp     edx,rt_cell_free
        je      .cell
        cmp     edx,rt_adfree
        je      .ad
        mov     ecx,rt_gc_table ; an object of a class or a closure: dd n, (destroy routine, map)*
        mov     edi,[ecx]
.find:  test    edi,edi
        jz      .none
        add     ecx,8
        dec     edi
        cmp     [ecx-4],edx
        jne     .find
        mov     edi,[ecx]       ; its map: dd n, offsets
        mov     ebp,[edi]
        mov     eax,ebp
        call    rt_gc_room
        xor     ecx,ecx
.ml:    cmp     ecx,ebp
        jae     .out
        mov     eax,[edi+4+ecx*4]
        add     eax,esi
        mov     [ebx+ecx*4],eax
        inc     ecx
        jmp     .ml
.cell:  lea     edx,[esi+8]
        jmp     .one
.ad:    lea     edx,[esi+16]
.one:   mov     eax,1
        call    rt_gc_room
        mov     [ebx],edx
        mov     ecx,1
        jmp     .out
.list:  mov     eax,[esi+20]
        test    byte [eax+4],8
        jz      .none
        mov     ebp,[esi+8]
        mov     eax,ebp
        call    rt_gc_room
        mov     edx,[esi+16]
        xor     ecx,ecx
.ll:    cmp     ecx,ebp
        jae     .out
        lea     eax,[edx+ecx*4]
        mov     [ebx+ecx*4],eax
        inc     ecx
        jmp     .ll
.dict:  mov     ebp,[esi+44]    ; keys (8 bytes each; holes: 0), then values
        lea     eax,[ebp*2]
        call    rt_gc_room
        xor     ecx,ecx
        mov     eax,[esi+24]
        test    byte [eax+4],8
        jz      .dv
        mov     edx,[esi+16]
        xor     edi,edi
.dk:    cmp     edi,ebp
        jae     .dv
        lea     eax,[edx+edi*8]
        mov     [ebx+ecx*4],eax
        inc     ecx
        inc     edi
        jmp     .dk
.dv:    mov     eax,[esi+36]
        test    byte [eax+4],8
        jz      .out
        mov     edx,[esi+20]
        xor     edi,edi
.dvl:   cmp     edi,ebp
        jae     .out
        lea     eax,[edx+edi*4]
        mov     [ebx+ecx*4],eax
        inc     ecx
        inc     edi
        jmp     .dvl
.set:   mov     eax,[esi+24]
        test    byte [eax+4],8
        jz      .none
        mov     ebp,[esi+20]    ; entries (24 bytes: the element, its hash, state 1 in use)
        inc     ebp
        mov     eax,ebp
        call    rt_gc_room
        mov     edx,[esi+16]
        xor     ecx,ecx
.sl:    cmp     dword [edx+16],1
        jne     @f
        mov     [ebx+ecx*4],edx
        inc     ecx
@@:     add     edx,24
        dec     ebp
        jnz     .sl
        jmp     .out
.tuple: mov     edi,[esi+8]     ; TD: n, size, (kd, offset)*
        mov     ebp,[edi]
        mov     eax,ebp
        call    rt_gc_room
        xor     ecx,ecx
.tl:    test    ebp,ebp
        jz      .out
        add     edi,8
        dec     ebp
        mov     eax,[edi]
        test    byte [eax+4],8
        jz      .tl
        mov     eax,[edi+4]
        add     eax,esi
        mov     [ebx+ecx*4],eax
        inc     ecx
        jmp     .tl
.none:  xor     ecx,ecx
.out:   mov     [rt_gc_kidv+4],ecx
        pop     ebp edi esi ebx
        ret

;;; code rt_gc_room : rt_os_alloc rt_os_free
rt_gc_room:                     ; eax = slots wanted in rt_gc_kidv (its old ones dropped) -> ebx = its items (eax, ecx, edx, esi, edi, ebp kept)
        cmp     eax,[rt_gc_kidv+8]
        ja      .grow
        mov     ebx,[rt_gc_kidv]
        ret
.grow:  push    eax ecx edx
        mov     ebx,[rt_gc_kidv+8]
        add     ebx,ebx
        cmp     ebx,1024
        jae     @f
        mov     ebx,1024
@@:     cmp     ebx,eax
        jae     @f
        add     eax,1023
        and     eax,-1024
        mov     ebx,eax
@@:     mov     eax,[rt_gc_kidv]
        test    eax,eax
        jz      @f
        mov     edx,[rt_gc_kidv+8]
        shl     edx,2
        call    rt_os_free
@@:     mov     [rt_gc_kidv+8],ebx
        lea     eax,[ebx*4]
        call    rt_os_alloc
        mov     [rt_gc_kidv],eax
        mov     ebx,eax
        pop     edx ecx eax
        ret
;;; bss rt_gc_room
rt_gc_kidv      rd 3            ; (a vector: items, count, room)

;;; code rt_gc_collect : rt_gc_kids rt_gc_push rt_free
rt_gc_collect:                  ; the noted objects looked at: the cycles only they refer to freed -> eax = objects freed
        push    ebx esi edi ebp
        inc     dword [rt_gc_busy]
        xor     esi,esi         ; 1. the noted ones still purple and alive: gray, with what they lead to,
        xor     edi,edi         ;    the references among them taken off the counts; the others forgotten
.r1:    cmp     esi,[rt_gc_rootv+4]
        jae     .r1e
        mov     eax,[rt_gc_rootv]
        mov     eax,[eax+esi*4]
        inc     esi
        mov     cl,[eax-3]
        and     cl,0x0C
        cmp     cl,0x0C
        jne     .drop           ; (gray: one noted before leads to it)
        cmp     dword [eax],0
        je      .freed
        mov     edx,[rt_gc_rootv]
        mov     [edx+edi*4],eax
        inc     edi
        call    rt_gc_gray
        jmp     .r1
.drop:  and     byte [eax-3],0xFD
        jmp     .r1
.freed: and     byte [eax-3],0xFD
        call    rt_free         ; (destroyed while noted: its memory waited for this)
        jmp     .r1
.r1e:   mov     [rt_gc_rootv+4],edi
        xor     esi,esi         ; 2. gray ones with counts left: black again (and what they lead to); the rest white
.r2:    cmp     esi,[rt_gc_rootv+4]
        jae     .r2e
        mov     eax,[rt_gc_rootv]
        mov     eax,[eax+esi*4]
        inc     esi
        call    rt_gc_scan
        jmp     .r2
.r2e:   mov     dword [rt_gc_whitev+4],0
        xor     esi,esi         ; 3. the white ones: garbage
.r3:    cmp     esi,[rt_gc_rootv+4]
        jae     .r3e
        mov     eax,[rt_gc_rootv]
        mov     eax,[eax+esi*4]
        inc     esi
        and     byte [eax-3],0xFD
        call    rt_gc_white
        jmp     .r3
.r3e:   mov     dword [rt_gc_rootv+4],0
        mov     ebp,[rt_gc_whitev+4]    ; 4. destroyed (their references to CYC objects already cut)
        xor     edi,edi
.dl:    cmp     edi,ebp
        jae     .done
        mov     eax,[rt_gc_whitev]      ; (destroying may note more objects: the vectors can move)
        mov     eax,[eax+edi*4]
        inc     edi
        call    dword [eax+4]
        jmp     .dl
.done:
if defined RT_GC_STRESS
        jmp     .out                    ; (MPY_GC_STRESS: a collection at every noted object)
end if
        mov     eax,ebp
        shl     eax,3                   ; little garbage among many noted ones: wait longer next time
        mov     ecx,[rt_gc_limit]
        cmp     eax,ecx
        jae     .less
        cmp     ecx,0x100000
        jae     .out
        add     ecx,ecx
        jmp     .lim
.less:  shr     ecx,1
        cmp     ecx,1000
        jae     .lim
        mov     ecx,1000
.lim:   mov     [rt_gc_limit],ecx
.out:   dec     dword [rt_gc_busy]
        mov     eax,ebp
        pop     ebp edi esi ebx
        ret

rt_gc_gray:                     ; eax = an object: gray, and the references among the CYC objects it leads to taken off their counts
        push    ebx esi edi
        mov     cl,[eax-3]
        and     cl,0xF3
        or      cl,4
        mov     [eax-3],cl
        mov     ebx,rt_gc_stk
        mov     dword [ebx+4],0
        call    rt_gc_push
.l:     mov     ecx,[ebx+4]
        test    ecx,ecx
        jz      .out
        dec     ecx
        mov     [ebx+4],ecx
        mov     eax,[ebx]
        mov     eax,[eax+ecx*4]
        call    rt_gc_kids
        mov     esi,[rt_gc_kidv]
        mov     edi,ecx
.k:     test    edi,edi
        jz      .l
        dec     edi
        mov     eax,[esi+edi*4]
        mov     eax,[eax]
        test    eax,eax
        jz      .k
        cmp     dword [eax],0x20000000
        jae     .k
        mov     cl,[eax-3]
        test    cl,1
        jz      .k
        dec     dword [eax]
        mov     dl,cl
        and     dl,0x0C
        cmp     dl,4
        je      .k
        and     cl,0xF3
        or      cl,4
        mov     [eax-3],cl
        call    rt_gc_push
        jmp     .k
.out:   pop     edi esi ebx
        ret

rt_gc_scan:                     ; eax = a gray object: black with what it leads to if it has a count left, else white (and so on)
        push    ebx esi edi
        mov     ebx,rt_gc_stk
        mov     dword [ebx+4],0
        call    rt_gc_push
.l:     mov     ecx,[ebx+4]
        test    ecx,ecx
        jz      .out
        dec     ecx
        mov     [ebx+4],ecx
        mov     eax,[ebx]
        mov     eax,[eax+ecx*4]
        mov     cl,[eax-3]
        and     cl,0x0C
        cmp     cl,4
        jne     .l
        cmp     dword [eax],0
        je      .white
        call    rt_gc_black
        jmp     .l
.white: mov     cl,[eax-3]
        and     cl,0xF3
        or      cl,8
        mov     [eax-3],cl
        call    rt_gc_kids
        mov     esi,[rt_gc_kidv]
        mov     edi,ecx
.k:     test    edi,edi
        jz      .l
        dec     edi
        mov     eax,[esi+edi*4]
        mov     eax,[eax]
        test    eax,eax
        jz      .k
        cmp     dword [eax],0x20000000
        jae     .k
        test    byte [eax-3],1
        jz      .k
        call    rt_gc_push
        jmp     .k
.out:   pop     edi esi ebx
        ret

rt_gc_black:                    ; eax = an object with a count left: black, and what it leads to (their counts given back)
        push    ebx esi edi
        and     byte [eax-3],0xF3
        mov     ebx,rt_gc_stk2
        mov     dword [ebx+4],0
        call    rt_gc_push
.l:     mov     ecx,[ebx+4]
        test    ecx,ecx
        jz      .out
        dec     ecx
        mov     [ebx+4],ecx
        mov     eax,[ebx]
        mov     eax,[eax+ecx*4]
        call    rt_gc_kids
        mov     esi,[rt_gc_kidv]
        mov     edi,ecx
.k:     test    edi,edi
        jz      .l
        dec     edi
        mov     eax,[esi+edi*4]
        mov     eax,[eax]
        test    eax,eax
        jz      .k
        cmp     dword [eax],0x20000000
        jae     .k
        mov     cl,[eax-3]
        test    cl,1
        jz      .k
        inc     dword [eax]
        test    cl,0x0C
        jz      .k
        and     cl,0xF3
        mov     [eax-3],cl
        call    rt_gc_push
        jmp     .k
.out:   pop     edi esi ebx
        ret

rt_gc_white:                    ; eax = a root no longer noted: if white, it and the white ones it leads to (not noted) are garbage:
        push    ebx esi edi     ; listed, kept alive until all are destroyed, their references to CYC objects cut (the counts lack them)
        mov     cl,[eax-3]
        and     cl,0x0C
        cmp     cl,8
        jne     .out
        and     byte [eax-3],0xF3
        mov     ebx,rt_gc_stk
        mov     dword [ebx+4],0
        call    rt_gc_push
.l:     mov     ecx,[ebx+4]
        test    ecx,ecx
        jz      .out
        dec     ecx
        mov     [ebx+4],ecx
        mov     eax,[ebx]
        mov     eax,[eax+ecx*4]
        push    ebx
        mov     ebx,rt_gc_whitev
        call    rt_gc_push
        pop     ebx
        mov     dword [eax],0x10000000
        call    rt_gc_kids
        mov     esi,[rt_gc_kidv]
        mov     edi,ecx
.k:     test    edi,edi
        jz      .l
        dec     edi
        mov     edx,[esi+edi*4]
        mov     eax,[edx]
        test    eax,eax
        jz      .k
        cmp     dword [eax],0x20000000
        jae     .k
        mov     cl,[eax-3]
        test    cl,1
        jz      .k
        mov     dword [edx],0   ; (cut)
        and     cl,0x0E         ; white and not noted: garbage too
        cmp     cl,8
        jne     .k
        and     byte [eax-3],0xF3
        call    rt_gc_push
        jmp     .k
.out:   pop     edi esi ebx
        ret
;;; bss rt_gc_collect
rt_gc_stk       rd 3
rt_gc_stk2      rd 3
rt_gc_whitev    rd 3

;;; code rt_adfree : rt_decref rt_free
rt_adfree:                      ; destroy routine of bound methods and adapters (+16: the object or function they hold)
        push    eax
        mov     eax,[eax+16]
        call    rt_decref
        pop     eax
        jmp     rt_free

;;; code rt_scratch
;;; bss rt_scratch
rt_scratch      rd 2            ; an element taken out of a container (pop, del)

