; ---------------------------------------------------------------- memory

;;; code rt_os_alloc linux : rt_panic
rt_os_alloc:                    ; eax = bytes (page multiple) -> eax = zeroed memory (mmap2)
        push    ebx esi edi ebp
        mov     ecx,eax
        xor     ebx,ebx
        mov     edx,3           ; PROT_READ|PROT_WRITE
        mov     esi,0x22        ; MAP_PRIVATE|MAP_ANONYMOUS
        or      edi,-1
        xor     ebp,ebp
        mov     eax,192
        int     0x80
        pop     ebp edi esi ebx
        cmp     eax,-4096
        ja      .oom
        ret
.oom:   mov     esi,rt_msg_oom
        jmp     rt_panic
;;; data rt_os_alloc linux
rt_msg_oom      db 'MemoryError: out of memory',0

;;; code rt_os_alloc kolibri : rt_panic
rt_os_alloc:                    ; eax = bytes -> eax = memory (fn 68.12)
        push    ebx
        mov     ecx,eax
        mov     eax,68
        mov     ebx,12
        int     0x40
        pop     ebx
        test    eax,eax
        jz      .oom
        ret
.oom:   mov     esi,rt_msg_oom
        jmp     rt_panic
;;; data rt_os_alloc kolibri
rt_msg_oom      db 'MemoryError: out of memory',0

;;; code rt_os_free linux
rt_os_free:                     ; eax = memory, edx = bytes (munmap)
        push    ebx
        mov     ebx,eax
        mov     ecx,edx
        mov     eax,91
        int     0x80
        pop     ebx
        ret

;;; code rt_os_free kolibri
rt_os_free:                     ; eax = memory (fn 68.13)
        push    ebx
        mov     ecx,eax
        mov     eax,68
        mov     ebx,13
        int     0x40
        pop     ebx
        ret

;;; code rt_alloc : rt_os_alloc
; Blocks carry a 4-byte header: the size class k (block = 16 << k bytes,
; k = 0..7) or, for big blocks, the page-rounded size | 0x80000000. Freed
; small blocks go to per-class free lists; big blocks go back to the system.
rt_alloc:                       ; eax = bytes -> eax = zeroed memory
        push    ebx esi edi
        lea     ecx,[eax+4]
        cmp     ecx,2048
        ja      .big
        mov     ebx,16
        xor     edx,edx
.cls:   cmp     ecx,ebx
        jbe     .have
        add     ebx,ebx
        inc     edx
        jmp     .cls
.have:  mov     eax,[rt_free_list+edx*4]
        test    eax,eax
        jz      .carve
        mov     ecx,[eax+4]
        mov     [rt_free_list+edx*4],ecx
        jmp     .ready
.carve: mov     eax,[rt_arena]
        mov     ecx,[rt_arena_end]
        sub     ecx,eax
        cmp     ecx,ebx
        jae     .take
        push    edx
        mov     eax,65536
        call    rt_os_alloc
        pop     edx
        lea     ecx,[eax+65536]
        mov     [rt_arena_end],ecx
.take:  lea     ecx,[eax+ebx]
        mov     [rt_arena],ecx
.ready: mov     [eax],edx
.zero:
if defined RT_COUNT_ALLOCS
        inc     dword [rt_live]
        inc     dword [rt_nalloc]
end if
        lea     edi,[eax+4]
        lea     ecx,[ebx-4]
        shr     ecx,2
        mov     edx,eax
        xor     eax,eax
        rep     stosd
        lea     eax,[edx+4]
        pop     edi esi ebx
        ret
.big:   add     ecx,4095
        and     ecx,-4096
        push    ecx
        mov     eax,ecx
        call    rt_os_alloc
        pop     ebx
        mov     ecx,ebx
        or      ecx,0x80000000
        mov     [eax],ecx
        jmp     .zero
;;; bss rt_alloc
rt_live         rd 1            ; --count-allocs: blocks in use, ...
rt_nalloc       rd 1            ; ... allocations, references taken and dropped (not counting 0)
rt_nincref      rd 1
rt_ndecref      rd 1
rt_free_list    rd 8
rt_arena        rd 1
rt_arena_end    rd 1

;;; code rt_free : rt_os_free rt_alloc
rt_free:                        ; eax = memory from rt_alloc (or 0); also the destroy routine of flat objects
        test    eax,eax
        jz      .done
if defined RT_GC
        test    byte [eax-3],2
        jnz     .done           ; noted by the cycle collector: rt_gc_collect frees it
end if
if defined RT_COUNT_ALLOCS
        dec     dword [rt_live]
end if
        sub     eax,4
        mov     ecx,[eax]
if defined RT_GC
        and     ecx,0xFFFFF0FF  ; (the cycle collector's marks)
end if
        test    ecx,ecx
        js      .big
        mov     edx,[rt_free_list+ecx*4]
        mov     [eax+4],edx
        mov     [rt_free_list+ecx*4],eax
.done:  ret
.big:   and     ecx,0x7FFFFFFF
        mov     edx,ecx
        jmp     rt_os_free

;;; code rt_live_report : rt_write_err rt_alloc
rt_live_report:                 ; --count-allocs: "[live blocks: N, allocations: A, incref: I, decref: D]" to stderr
        push    ebx esi
        mov     ebx,rt_live_fields
        mov     esi,rt_live
.f:     mov     ecx,[ebx]
        movzx   edx,byte [ecx]
        inc     ecx
        call    rt_write_err
        lodsd
        mov     edi,rt_live_num+12
        mov     ecx,10
@@:     xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        test    eax,eax
        jnz     @b
        mov     ecx,edi
        mov     edx,rt_live_num+12
        sub     edx,edi
        call    rt_write_err
        add     ebx,4
        cmp     ebx,rt_live_fields+16
        jb      .f
        mov     ecx,rt_live_end
        mov     edx,2
        pop     esi ebx
        jmp     rt_write_err
;;; data rt_live_report
rt_live_fields  dd rt_live_m1,rt_live_m2,rt_live_m3,rt_live_m4
rt_live_m1      db 14,'[live blocks: '
rt_live_m2      db 15,', allocations: '
rt_live_m3      db 10,', incref: '
rt_live_m4      db 10,', decref: '
rt_live_end     db ']',10
rt_live_num     db 0,0,0,0,0,0,0,0,0,0,0,0

;;; code rt_incref
rt_incref:                      ; eax = object or 0
        test    eax,eax
        jz      @f
if defined RT_COUNT_ALLOCS
        inc     dword [rt_nincref]
end if
        inc     dword [eax]
@@:     ret

;;; code rt_decref
rt_decref:                      ; eax = object or 0: drop a reference, destroy at zero
        test    eax,eax
        jz      @f
if defined RT_COUNT_ALLOCS
        inc     dword [rt_ndecref]
end if
        dec     dword [eax]
        jz      .zero
if defined RT_GC
        test    byte [eax-3],1
        jnz     rt_gc_root      ; it may be in a cycle that only refers to itself now
end if
@@:     ret
.zero:  jmp     dword [eax+4]

;;; code rt_release : rt_decref
rt_release:                     ; eax = address of a reference slot: emptied, its reference dropped
        push    dword [eax]
        mov     dword [eax],0
        pop     eax
        jmp     rt_decref

;;; code rt_gc_root : rt_gc_collect rt_gc_push
