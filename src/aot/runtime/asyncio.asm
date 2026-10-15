; ---------------------------------------------------------------- asyncio
; Stackful tasks: every task runs on its own stack; `await coroutine()` is an
; ordinary call on the current task's stack, and a task gives the CPU back to
; the scheduler (rt_async_run, on the program's stack) when it sleeps or waits.
; Task object: +0 refcount, +4 destroy, +8 state (0 ready, 1 running,
; 2 sleeping, 3 waiting, 4 done), +12 saved esp, +16 stack memory, +20 next in
; the run queue / sleeper list, +24 result (8 bytes), +32 wake time (ms),
; +36 tasks waiting for this one, +40 next waiter, +44 result is a reference,
; +48 its handlers, +52 the generator it runs (an await inside a generator:
; generators are per task).

;;; code rt_task_new : rt_alloc rt_os_alloc rt_task_free rt_task_ready
rt_task_new:                    ; eax = argument bytes, edx = entry -> eax = new ready task; arguments at [[eax+12]+20]
        push    ebx esi
        mov     esi,eax
        push    edx
        mov     eax,56
        call    rt_alloc
        mov     ebx,eax
        mov     dword [ebx],2   ; the caller's reference and the scheduler's
        mov     dword [ebx+4],rt_task_free
        mov     eax,65536
        call    rt_os_alloc
        mov     [ebx+16],eax
        add     eax,65536-64
        sub     eax,esi
        and     eax,-16
        sub     eax,20          ; edi esi ebx ebp, then "return" into the entry
        xor     ecx,ecx
        mov     [eax],ecx
        mov     [eax+4],ecx
        mov     [eax+8],ecx
        mov     [eax+12],ecx
        pop     edx
        mov     [eax+16],edx
        mov     [ebx+12],eax
        mov     eax,ebx
        call    rt_task_ready
        mov     eax,ebx
        pop     esi ebx
        ret

;;; code rt_task_free : rt_os_free rt_free rt_decref
rt_task_free:                   ; destroy routine of tasks
        push    ebx
        mov     ebx,eax
        cmp     dword [ebx+44],0
        je      @f
        mov     eax,[ebx+24]
        call    rt_decref
@@:     mov     eax,[ebx+16]
        mov     edx,65536
        call    rt_os_free
        mov     eax,ebx
        pop     ebx
        jmp     rt_free

;;; code rt_task_ready
rt_task_ready:                  ; eax = task: append it to the run queue
        mov     dword [eax+8],0
        mov     dword [eax+20],0
        mov     ecx,[rt_ready_tail]
        test    ecx,ecx
        jz      @f
        mov     [ecx+20],eax
        mov     [rt_ready_tail],eax
        ret
@@:     mov     [rt_ready_head],eax
        mov     [rt_ready_tail],eax
        ret
;;; bss rt_task_ready
rt_ready_head   rd 1
rt_ready_tail   rd 1
rt_cur_task     rd 1            ; the running task, 0 in the scheduler
rt_sched_esp    rd 1
rt_sched_exc    rd 1            ; the scheduler's handler chain while a task runs
rt_sched_gen    rd 1            ; the scheduler's running generator while a task runs (it may wait in one)
rt_sleepers     rd 1

;;; code rt_thread_current : rt_task_ready
rt_thread_current:              ; -> edx:eax = the running task (0: the main program), thread.current()
        mov     eax,[rt_cur_task]
        xor     edx,edx
        ret

;;; code rt_task_yield : rt_task_ready
rt_task_yield:                  ; the running task hands the CPU back to the scheduler; returns when resumed
        push    ebp ebx esi edi
        mov     eax,[rt_cur_task]
        mov     [eax+12],esp
if defined rt_throw
        mov     ecx,[rt_exc_top] ; every task has its own handlers (+48)
        mov     [eax+48],ecx
        mov     ecx,[rt_sched_exc]
        mov     [rt_exc_top],ecx
end if
if defined rt_cur_gen
        mov     ecx,[rt_cur_gen]
        mov     [eax+52],ecx
        mov     ecx,[rt_sched_gen]
        mov     [rt_cur_gen],ecx
end if
        mov     esp,[rt_sched_esp]
        pop     edi esi ebx ebp
        ret

;;; code rt_task_resume : rt_task_ready
rt_task_resume:                 ; eax = task: run it until it yields or ends
        push    ebp ebx esi edi
        mov     [rt_sched_esp],esp
if defined rt_throw
        mov     ecx,[rt_exc_top]
        mov     [rt_sched_exc],ecx
        mov     ecx,[eax+48]
        mov     [rt_exc_top],ecx
end if
        mov     [rt_cur_task],eax
        mov     dword [eax+8],1
if defined rt_cur_gen
        mov     ecx,[rt_cur_gen]
        mov     [rt_sched_gen],ecx
        mov     ecx,[eax+52]
        mov     [rt_cur_gen],ecx
end if
        mov     esp,[eax+12]
        pop     edi esi ebx ebp
        ret

;;; code rt_task_exit : rt_task_ready
rt_task_exit:                   ; a task's function returned (the result is stored): wake its waiters, leave for good
        mov     ebx,[rt_cur_task]
        mov     dword [ebx+8],4
        mov     eax,[ebx+36]
        mov     dword [ebx+36],0
@@:     test    eax,eax
        jz      @f
        push    dword [eax+40]
        call    rt_task_ready
        pop     eax
        jmp     @b
@@:
if defined rt_throw
        mov     ecx,[rt_sched_exc]
        mov     [rt_exc_top],ecx
end if
if defined rt_cur_gen
        mov     ecx,[rt_sched_gen]
        mov     [rt_cur_gen],ecx
end if
        mov     esp,[rt_sched_esp]
        pop     edi esi ebx ebp
        ret

;;; code rt_task_wait : rt_task_yield
rt_task_wait:                   ; eax = task: wait until it is done (from a task) -> eax = task
        cmp     dword [eax+8],4
        je      .done
        push    eax
        mov     ecx,[rt_cur_task]
        mov     edx,[eax+36]
        mov     [ecx+40],edx
        mov     [eax+36],ecx
        mov     dword [ecx+8],3
        call    rt_task_yield
        pop     eax
.done:  ret

;;; code rt_task_result : rt_panic
rt_task_result:                 ; eax = task -> eax = task, which must be done
        cmp     dword [eax+8],4
        jne     @f
        ret
@@:     mov     esi,rt_msg_notdone
        jmp     rt_panic
;;; data rt_task_result
rt_msg_notdone  db 'InvalidStateError: result is not set',0

;;; code rt_async_sleep : rt_task_yield rt_task_ready rt_now_ms rt_idle_sleep
rt_async_sleep:                 ; st0 = seconds (popped): suspend the running task that long
        push    1000
        fimul   dword [esp]
        fistp   dword [esp]
        pop     eax
        cmp     dword [rt_cur_task],0
        jne     .task
        test    eax,eax         ; no event loop (a coroutine called directly): just sleep
        jle     .none
        jmp     rt_idle_sleep
.none:  ret
.task:  test    eax,eax
        jg      @f
        mov     eax,[rt_cur_task] ; sleep(0): to the back of the run queue
        call    rt_task_ready
        jmp     rt_task_yield
@@:     push    eax
        call    rt_now_ms
        pop     edx
        add     eax,edx
        mov     ecx,[rt_cur_task]
        mov     [ecx+32],eax
        mov     dword [ecx+8],2
        mov     eax,[rt_sleepers]
        mov     [ecx+20],eax
        mov     [rt_sleepers],ecx
        jmp     rt_task_yield

;;; code rt_sched_step : rt_task_resume rt_task_ready rt_now_ms rt_idle_sleep rt_decref
; thread (minipy's module) in compiled programs: threads are tasks; the program's own code
; runs the others when it waits (join, acquire, sleep) and at its end.
rt_sched_step:                  ; eax = ms it may idle at most (-1: any): run one ready task -> eax = 0 when no task is left
        push    ebx esi edi ebp
        mov     ebp,eax
        call    rt_now_ms
        mov     esi,eax
        mov     edi,rt_sleepers
.sl:    mov     ebx,[edi]
        test    ebx,ebx
        jz      .pick
        mov     eax,[ebx+32]
        sub     eax,esi
        jg      .keep
        mov     eax,[ebx+20]
        mov     [edi],eax
        mov     eax,ebx
        call    rt_task_ready
        jmp     .sl
.keep:  lea     edi,[ebx+20]
        jmp     .sl
.pick:  mov     eax,[rt_ready_head]
        test    eax,eax
        jnz     .run
        mov     ebx,[rt_sleepers]
        test    ebx,ebx
        jz      .none
        mov     ecx,0x7FFFFFFF
.min:   mov     eax,[ebx+32]
        sub     eax,esi
        cmp     eax,ecx
        jge     @f
        mov     ecx,eax
@@:     mov     ebx,[ebx+20]
        test    ebx,ebx
        jnz     .min
        test    ebp,ebp
        js      @f
        cmp     ecx,ebp
        jle     @f
        mov     ecx,ebp
@@:     mov     eax,ecx
        call    rt_idle_sleep
        mov     eax,1
        jmp     .out
.run:   mov     ecx,[eax+20]
        mov     [rt_ready_head],ecx
        test    ecx,ecx
        jnz     @f
        mov     [rt_ready_tail],ecx
@@:     push    eax
        call    rt_task_resume
        pop     eax
        mov     dword [rt_cur_task],0
        cmp     dword [eax+8],4
        jne     @f
        call    rt_decref       ; the scheduler's reference to a finished task
@@:     mov     eax,1
        jmp     .out
.none:  xor     eax,eax
.out:   pop     ebp edi esi ebx
        ret

;;; code rt_thread_join : rt_task_wait rt_async_run
rt_thread_join:                 ; eax = a thread (task): until it is done
        cmp     dword [rt_cur_task],0
        jne     rt_task_wait
        jmp     rt_async_run

;;; code rt_thread_drain : rt_sched_step
rt_thread_drain:                ; the program's end: the threads still running finish
@@:     or      eax,-1
        call    rt_sched_step
        test    eax,eax
        jnz     @b
        ret

;;; code rt_thread_sleep : rt_async_sleep rt_sched_step rt_now_ms rt_idle_sleep
rt_thread_sleep:                ; st0 = seconds (popped): the other threads run meanwhile
        cmp     dword [rt_cur_task],0
        jne     rt_async_sleep
        push    1000
        fimul   dword [esp]
        fistp   dword [esp]
        call    rt_now_ms
        add     [esp],eax       ; the time to wake up
.l:     call    rt_now_ms
        mov     ecx,[esp]
        sub     ecx,eax
        jle     .done
        mov     eax,ecx
        call    rt_sched_step
        test    eax,eax
        jnz     .l
        call    rt_now_ms       ; nothing else to run: sleep the rest
        mov     ecx,[esp]
        sub     ecx,eax
        jle     .done
        mov     eax,ecx
        call    rt_idle_sleep
.done:  add     esp,4
        ret

;;; code rt_thread_lock : rt_panic
rt_thread_lock:                 ; -> eax = a new lock's number (edx 0)
        xor     eax,eax
@@:     cmp     dword [rt_locks+eax*4],0
        je      @f
        inc     eax
        cmp     eax,256
        jb      @b
        mov     esi,rt_msg_locks
        jmp     rt_panic
@@:     mov     dword [rt_locks+eax*4],1
        xor     edx,edx
        ret
;;; data rt_thread_lock
rt_msg_locks    db 'RuntimeError: too many locks',0
;;; bss rt_thread_lock
rt_locks        rd 256          ; 0 unused, 1 free, 2 held

;;; code rt_thread_acquire : rt_thread_lock rt_task_ready rt_task_yield rt_sched_step rt_panic
rt_thread_acquire:              ; eax = a lock's number: wait until it is free, take it
        and     eax,255
        lea     ecx,[rt_locks+eax*4]
.try:   cmp     dword [ecx],2
        jne     .take
        push    ecx
        cmp     dword [rt_cur_task],0
        je      .main
        mov     eax,[rt_cur_task] ; a thread: the others first
        call    rt_task_ready
        call    rt_task_yield
        pop     ecx
        jmp     .try
.main:  or      eax,-1          ; the program: run the others until it is free
        call    rt_sched_step
        pop     ecx
        test    eax,eax
        jnz     .try
        mov     esi,rt_msg_lockdead
        jmp     rt_panic
.take:  mov     dword [ecx],2
        ret
;;; data rt_thread_acquire
rt_msg_lockdead db 'RuntimeError: the lock is held by no running thread (deadlock)',0

;;; code rt_thread_release : rt_thread_lock
rt_thread_release:              ; eax = a lock's number
        and     eax,255
        mov     dword [rt_locks+eax*4],1
        ret

;;; code rt_async_run : rt_task_resume rt_task_ready rt_now_ms rt_idle_sleep rt_decref rt_panic
rt_async_run:                   ; eax = task: run the event loop until it is done -> eax = task
        push    ebx esi edi ebp
        mov     ebp,eax
.loop:  cmp     dword [ebp+8],4
        je      .out
        call    rt_now_ms       ; sleepers whose time has come -> run queue
        mov     esi,eax
        mov     edi,rt_sleepers
.sl:    mov     ebx,[edi]
        test    ebx,ebx
        jz      .pick
        mov     eax,[ebx+32]
        sub     eax,esi
        jg      .keep
        mov     eax,[ebx+20]
        mov     [edi],eax
        mov     eax,ebx
        call    rt_task_ready
        jmp     .sl
.keep:  lea     edi,[ebx+20]
        jmp     .sl
.pick:  mov     eax,[rt_ready_head]
        test    eax,eax
        jnz     .run
        mov     ebx,[rt_sleepers] ; nothing to run: sleep until the first sleeper is due
        test    ebx,ebx
        jz      .stuck
        mov     ecx,0x7FFFFFFF
.min:   mov     eax,[ebx+32]
        sub     eax,esi
        cmp     eax,ecx
        jge     @f
        mov     ecx,eax
@@:     mov     ebx,[ebx+20]
        test    ebx,ebx
        jnz     .min
        mov     eax,ecx
        call    rt_idle_sleep
        jmp     .loop
.run:   mov     ecx,[eax+20]
        mov     [rt_ready_head],ecx
        test    ecx,ecx
        jnz     @f
        mov     [rt_ready_tail],ecx
@@:     push    eax
        call    rt_task_resume
        pop     eax
        mov     dword [rt_cur_task],0
        cmp     dword [eax+8],4
        jne     .loop
        call    rt_decref       ; the scheduler's reference to a finished task
        jmp     .loop
.stuck: mov     esi,rt_msg_stuck
        jmp     rt_panic
.out:   mov     eax,ebp
        pop     ebp edi esi ebx
        ret
;;; data rt_async_run
rt_msg_stuck    db 'RuntimeError: every task is waiting (deadlock)',0

