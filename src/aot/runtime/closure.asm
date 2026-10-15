; ---------------------------------------------------------------- closures
; A function value: +0 refcount, +4 destroy, +8 code, +12 captured values
; (copies, or cells shared with the defining function). A cell: +0 refcount,
; +4 destroy, +8 the value (8 bytes).

;;; code rt_cell_new : rt_alloc rt_cell_free
rt_cell_new:                    ; eax = 1 if the value is a reference (3: one that can be in a cycle) -> eax = new cell (value 0)
        push    eax
        mov     eax,16
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_free
        test    ecx,ecx
        jz      @f
        mov     dword [eax+4],rt_cell_free
if defined RT_GC
        test    ecx,2
        jz      @f
        or      byte [eax-3],1
end if
@@:     ret

;;; code rt_cell_free : rt_decref rt_free
rt_cell_free:                   ; destroy routine of a cell holding a reference
        push    eax
        mov     eax,[eax+8]
        call    rt_decref
        pop     eax
        jmp     rt_free

