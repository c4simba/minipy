; ---------------------------------------------------------------- generators
; A generator runs its function on a stack of its own: next() switches to it
; until it yields (the value is left in the generator object) or returns.
; Generator: +0 refcount, +4 destroy, +8 state (0 created, 1 running,
; 2 suspended, 3 done), +12 its saved esp, +16 stack memory, +20 the
; consumer's esp, +24 the current value (8 bytes), +32 the value is a
; reference, +36 the arguments, +40 descriptor (function, flags: 1 reference
; values, 2 8-byte values; slot count, frame offsets of the references it
; owns), +44 closure, +48 its handlers, +52 the consumer's handlers, +56 the
; generator that ran before, +60 its outermost handler record (28 bytes),
; +88 a value sent (8 bytes), +96 a value was sent, +100 its return value
; (8 bytes), +108 it returned one, +112 an exception to raise where it waits
; (throw(), close()). Descriptor flags also: 4 sent values are references,
; 8 the return value is one. State 4: closed before it started.

;;; code rt_gen_new : rt_alloc rt_os_alloc rt_gen_free rt_gen_yield
rt_gen_new:                     ; eax = argument bytes, edx = descriptor -> eax = new generator; arguments at [eax+36]
        push    ebx esi
        mov     esi,eax
        push    edx
        mov     eax,116
        call    rt_alloc
        mov     ebx,eax
        mov     dword [ebx],1
        mov     dword [ebx+4],rt_gen_free
        pop     edx
        mov     [ebx+40],edx
        mov     eax,[edx+4]
        and     eax,1
        mov     [ebx+32],eax
        mov     eax,RT_COSTACK  ; its stack (a constant of the program: its target's)
        call    rt_os_alloc
        mov     [ebx+16],eax
        add     eax,RT_COSTACK-64
        sub     eax,esi
        and     eax,-16
        sub     eax,20          ; edi esi ebx ebp, then "return" into rt_gen_start
        xor     ecx,ecx
        mov     [eax],ecx
        mov     [eax+4],ecx
        mov     [eax+8],ecx
        mov     [eax+12],ecx
        mov     dword [eax+16],rt_gen_start
        mov     [ebx+12],eax
        add     eax,20
        mov     [ebx+36],eax
        mov     eax,ebx
        pop     esi ebx
        ret

;;; code rt_gen_next : rt_gen_yield rt_panic_value
rt_gen_next:                    ; eax = generator: run it to its next value -> eax = 1 (the value is at +24) or 0 (finished)
        mov     ecx,[eax+8]
        cmp     ecx,3
        jae     .done
        cmp     ecx,1
        je      .busy
        push    ebp ebx esi edi
        mov     [eax+20],esp
if defined rt_throw
        mov     ecx,[rt_exc_top]
        mov     [eax+52],ecx
        mov     ecx,[eax+48]
        mov     [rt_exc_top],ecx
end if
        mov     ecx,[rt_cur_gen]
        mov     [eax+56],ecx
        mov     [rt_cur_gen],eax
        mov     dword [eax+8],1
        mov     esp,[eax+12]
        pop     edi esi ebx ebp
        ret
.done:  xor     eax,eax
        ret
.busy:  mov     esi,rt_msg_genbusy
        jmp     rt_panic_value
;;; data rt_gen_next
rt_msg_genbusy  db 'generator already executing',0

;;; code rt_gen_yield
rt_gen_yield:                   ; from the generator's function, its value stored: back to the consumer until the next next()
        call    rt_gen_suspend  ; (resumed: an exception thrown in is raised here)
if defined rt_throw
        mov     ecx,[rt_cur_gen]
        mov     eax,[ecx+112]
        test    eax,eax
        jnz     .throw
end if
        ret
if defined rt_throw
.throw: mov     dword [ecx+112],0
        jmp     rt_throw
end if
rt_gen_suspend:
        push    ebp ebx esi edi
        mov     eax,[rt_cur_gen]
        mov     [eax+12],esp
        mov     dword [eax+8],2
        mov     ecx,1
rt_gen_switch:                  ; eax = generator, ecx = what next() returns
if defined rt_throw
        mov     edx,[rt_exc_top]
        mov     [eax+48],edx
        mov     edx,[eax+52]
        mov     [rt_exc_top],edx
end if
        mov     edx,[eax+56]
        mov     [rt_cur_gen],edx
        mov     esp,[eax+20]
        pop     edi esi ebx ebp
        mov     eax,ecx
        ret
rt_gen_start:                   ; the first next(): run the generator's function
        mov     ecx,[rt_cur_gen]
if defined rt_throw
        lea     eax,[ecx+60]    ; its outermost handler: an exception leaves the generator
        mov     dword [eax],0
        mov     [eax+4],esp
        mov     [eax+8],ebp
        mov     dword [eax+12],rt_gen_caught
        mov     [eax+16],ebx
        mov     [eax+20],esi
        mov     [eax+24],edi
        mov     [rt_exc_top],eax
end if
        mov     edx,[ecx+44]    ; its closure
        mov     eax,[ecx+40]
        call    dword [eax]
        mov     eax,[rt_cur_gen]
        mov     dword [eax+8],3
        xor     ecx,ecx
        jmp     rt_gen_switch
if defined rt_throw
rt_gen_caught:                  ; an exception escaped the generator: it is finished, the consumer gets the exception
        mov     eax,[rt_cur_gen]
        mov     dword [eax+8],3
        mov     ecx,[eax+52]
        mov     [rt_exc_top],ecx
        mov     ecx,[eax+56]
        mov     [rt_cur_gen],ecx
        mov     esp,[eax+20]
        pop     edi esi ebx ebp
        mov     eax,[rt_exc_cur]
        jmp     rt_throw
end if
;;; bss rt_gen_yield
rt_cur_gen      rd 1            ; the running generator

;;; code rt_gen_free : rt_decref rt_os_free rt_free
rt_gen_free:                    ; destroy routine: also releases what a suspended (or never started) generator's frame holds
        push    ebx esi edi ebp
        mov     ebx,eax
if defined rt_gen_close
        cmp     dword [ebx+8],2
        jne     @f
        mov     dword [ebx],1   ; (alive while it closes)
        xor     edx,edx         ; a suspended one is closed first: its finally blocks run
        call    rt_gen_close
        mov     dword [ebx],0
@@:
end if
        mov     eax,[ebx+8]
        cmp     eax,4
        jne     @f
        xor     eax,eax         ; closed before it started: the arguments are still there
@@:
        xor     edi,edi
        cmp     eax,2
        jne     .ns
        mov     ebp,[ebx+12]
        mov     ebp,[ebp+12]    ; the function's frame (ebp saved by rt_gen_yield)
        jmp     .slots
.ns:    test    eax,eax
        jnz     .rest
        mov     ebp,[ebx+36]    ; never started: just the arguments
        sub     ebp,8
        inc     edi
.slots: mov     esi,[ebx+40]
        mov     ecx,[esi+8]
        add     esi,12
.l:     test    ecx,ecx
        jz      .rest
        mov     eax,[esi]
        test    edi,edi
        jz      .take
        test    eax,eax
        js      .skip
.take:  push    ecx
        mov     eax,[ebp+eax]
        call    rt_decref
        pop     ecx
.skip:  add     esi,4
        dec     ecx
        jmp     .l
.rest:  cmp     dword [ebx+32],0
        je      .nv
        mov     eax,[ebx+24]
        call    rt_decref
.nv:    mov     esi,[ebx+40]
        test    dword [esi+4],4
        jz      .ns2
        cmp     dword [ebx+96],0
        je      .ns2
        mov     eax,[ebx+88]
        call    rt_decref
.ns2:   test    dword [esi+4],8
        jz      .nr
        cmp     dword [ebx+108],0
        je      .nr
        mov     eax,[ebx+100]
        call    rt_decref
.nr:    mov     eax,[ebx+112]
        call    rt_decref
        mov     eax,[ebx+44]
        call    rt_decref
        mov     eax,[ebx+16]
        mov     edx,RT_COSTACK
        call    rt_os_free
        mov     eax,ebx
        pop     ebp edi esi ebx
        jmp     rt_free

;;; code rt_gen_close : rt_gen_next rt_obj_new rt_decref rt_throw
rt_gen_close:                   ; eax = generator, edx = 1: other exceptions propagate (0: dropped) -> eax = 1 if it yielded again
        mov     ecx,[eax+8]
        test    ecx,ecx
        jnz     @f
        mov     dword [eax+8],4 ; never started: finished
        xor     eax,eax
        ret
@@:     cmp     ecx,2
        je      .susp
        xor     eax,eax
        ret
.susp:  push    ebx esi edi ebp
        mov     ebx,eax
        mov     esi,edx
        mov     eax,RT_EXC_SIZE ; GeneratorExit, raised where it waits
        mov     edx,VTX_GeneratorExit
        mov     ecx,DTX_GeneratorExit
        call    rt_obj_new
        mov     [ebx+112],eax
        sub     esp,28          ; a handler for what leaves the generator
        mov     eax,[rt_exc_top]
        mov     [esp],eax
        mov     [esp+4],esp
        mov     [esp+8],ebp
        mov     dword [esp+12],.caught
        mov     [esp+16],ebx
        mov     [esp+20],esi
        mov     [esp+24],edi
        mov     [rt_exc_top],esp
        mov     eax,ebx
        call    rt_gen_next
        mov     ecx,[esp]
        mov     [rt_exc_top],ecx
        add     esp,28
        pop     ebp edi esi ebx
        ret
.caught: add    esp,28
        mov     eax,[rt_exc_cur]
        mov     ecx,[eax+8]
        cmp     ecx,VTX_GeneratorExit
        je      .gone
        cmp     ecx,VTX_StopIteration
        je      .gone
        test    esi,esi
        jz      .gone
        pop     ebp edi esi ebx
        jmp     rt_throw
.gone:  call    rt_decref
        xor     eax,eax
        pop     ebp edi esi ebx
        ret

;;; code rt_gen_throw : rt_gen_next rt_throw
rt_gen_throw:                   ; eax = generator, edx = exception (owned): raised where it waits -> as rt_gen_next
        cmp     dword [eax+8],2
        jne     .now
        mov     [eax+112],edx
        jmp     rt_gen_next
.now:   cmp     dword [eax+8],0
        jne     @f
        mov     dword [eax+8],4 ; not started: finished, the exception is raised here
@@:     mov     eax,edx
        jmp     rt_throw

;;; code rt_gen_drain : rt_gen_next rt_list_new rt_list_push rt_elem_copy
rt_gen_drain:                   ; eax = generator, edx = the type of its values -> eax = new list of the values left
        push    ebx esi
        mov     ebx,eax
        mov     eax,edx
        call    rt_list_new
        mov     esi,eax
.l:     test    ebx,ebx
        jz      .out
        mov     eax,ebx
        call    rt_gen_next
        test    eax,eax
        jz      .out
        mov     eax,esi
        call    rt_list_push
        lea     edx,[ebx+24]
        mov     ecx,[esi+20]
        call    rt_elem_copy
        jmp     .l
.out:   mov     eax,esi
        pop     esi ebx
        ret

;;; code rt_panic_stop : rt_panic
rt_panic_stop:                  ; next() of a finished generator
if defined rt_throw
        mov     eax,VTX_StopIteration
        mov     edx,DTX_StopIteration
        xor     ecx,ecx
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     esi,rt_msg_stop
        jmp     rt_panic
end if
;;; data rt_panic_stop
rt_msg_stop     db 'StopIteration',0

;;; code rt_panic_type : rt_panic
rt_panic_type:                  ; esi = detail
if defined rt_throw
        mov     eax,VTX_TypeError
        mov     edx,DTX_TypeError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_msg_type
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_panic_type
rt_msg_type     db 'TypeError: ',0

