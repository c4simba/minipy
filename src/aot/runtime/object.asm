; ---------------------------------------------------------------- objects and buffers

;;; code rt_obj_new : rt_alloc
rt_obj_new:                     ; eax = size, edx = vtable, ecx = destroy routine -> eax = new zeroed object
        push    edx ecx
        call    rt_alloc
        pop     ecx edx
        mov     dword [eax],1
        mov     [eax+4],ecx
        mov     [eax+8],edx
if defined RT_GC
        cmp     dword [edx-12],0
        je      @f
        or      byte [eax-3],1  ; a class whose objects can be in a cycle (vtable-12: the map of its references)
@@:
end if
        ret

;;; code rt_isinstance
rt_isinstance:                  ; eax = object, edx = vtable -> eax = bool
        test    eax,eax
        jz      .no
        mov     eax,[eax+8]
@@:     cmp     eax,edx
        je      .yes
        mov     eax,[eax+4]
        test    eax,eax
        jnz     @b
.no:    xor     eax,eax
        ret
.yes:   mov     eax,1
        ret

;;; code rt_buf_new : rt_alloc rt_free
rt_buf_new:                     ; eax = length -> eax = new zeroed buffer
        test    eax,eax
        jns     @f
        xor     eax,eax
@@:     push    eax
        add     eax,13
        call    rt_alloc
        pop     ecx
        mov     dword [eax],1
        mov     dword [eax+4],rt_free
        mov     [eax+8],ecx
        ret

;;; code rt_buf_from_str : rt_buf_new
rt_buf_from_str:                ; eax = str -> eax = new buffer with its bytes
        push    esi edi
        mov     esi,eax
        xor     eax,eax
        test    esi,esi
        jz      @f
        mov     eax,[esi+8]
@@:     push    eax
        call    rt_buf_new
        pop     ecx
        jecxz   @f
        lea     edi,[eax+12]
        add     esi,12
        rep     movsb
@@:     pop     edi esi
        ret

;;; code rt_buf_at : rt_panic
rt_buf_at:                      ; eax = buffer, edx = offset, ecx = bytes -> eax = their address (bounds checked)
        test    eax,eax
        jz      .bad
        test    edx,edx
        js      .bad
        add     ecx,edx
        jc      .bad
        cmp     ecx,[eax+8]
        ja      .bad
        lea     eax,[eax+12+edx]
        ret
.bad:   mov     esi,rt_msg_buf
        jmp     rt_panic
;;; data rt_buf_at
rt_msg_buf      db 'IndexError: buffer offset out of range',0

;;; code rt_buf_peek_str : rt_buf_at rt_str_new
rt_buf_peek_str:                ; eax = buffer, edx = offset, ecx = length -> eax = new str
        push    esi edi
        push    ecx
        call    rt_buf_at
        mov     esi,eax
        mov     eax,[esp]
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret

;;; code rt_buf_peek_bytes : rt_buf_at rt_str_new
rt_buf_peek_bytes:              ; eax = buffer, edx = offset, ecx = length -> eax = new bytes
        push    esi edi
        push    ecx
        call    rt_buf_at
        mov     esi,eax
        mov     eax,[esp]
        call    rt_str_new
        pop     ecx
        mov     [eax+13+ecx],ecx        ; (bytes: the length where a str keeps its code point count)
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret

;;; code rt_mem_peek
rt_mem_peek:                    ; eax = address, edx = size (bytes) -> eax = little-endian value
        mov     ecx,edx
        xor     edx,edx
        jecxz   .out
        add     eax,ecx
@@:     dec     eax
        shl     edx,8
        mov     dl,[eax]
        dec     ecx
        jnz     @b
.out:   mov     eax,edx
        ret

;;; code rt_mem_poke
rt_mem_poke:                    ; eax = address, edx = value, ecx = size (bytes): store little-endian
        jecxz   .out
@@:     mov     [eax],dl
        inc     eax
        shr     edx,8
        dec     ecx
        jnz     @b
.out:   ret

;;; code rt_mem_peek_str : rt_str_new
rt_mem_peek_str:                ; eax = address, edx = length -> eax = new str of those bytes
        push    esi edi
        mov     esi,eax
        test    edx,edx
        jns     @f
        xor     edx,edx
@@:     push    edx
        mov     eax,edx
        call    rt_str_new
        pop     ecx
        lea     edi,[eax+12]
        rep     movsb
        pop     edi esi
        ret

;;; code rt_sys_args linux : rt_list_new rt_list_push rt_mem_cstr rt_kd_str
rt_sys_args:                    ; eax = 0: argv, 1: the environment ("NAME=value") -> eax = new list[str]
        push    ebx esi
        mov     esi,[rt_sp0]    ; the stack at start: argc, argv..., 0, envp..., 0
        mov     ebx,[esi]
        add     esi,4
        test    eax,eax
        jz      @f
        lea     esi,[esi+ebx*4+4]
@@:     mov     eax,rt_kd_str
        call    rt_list_new
        mov     ebx,eax
.l:     mov     eax,[esi]
        test    eax,eax
        jz      .out
        mov     edx,65536
        call    rt_mem_cstr
        push    eax
        mov     eax,ebx
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        add     esi,4
        jmp     .l
.out:   mov     eax,ebx
        pop     esi ebx
        ret
;;; data rt_sys_args linux
rt_sp0          dd 0            ; esp at start (set there when rt_sys_args is used)

;;; code rt_sys_args kolibri : rt_list_new rt_list_push rt_mem_cstr rt_kd_str
rt_sys_args:                    ; eax = 0: [the program's path, its parameters], 1: [] (no environment)
        push    ebx
        push    eax
        mov     eax,rt_kd_str
        call    rt_list_new
        mov     ebx,eax
        pop     eax
        test    eax,eax
        jnz     .out
        mov     eax,rt_kpath    ; (the kernel filled both: the MENUET01 header points at them)
        call    .add
        mov     eax,rt_kparams
        call    .add
.out:   mov     eax,ebx
        pop     ebx
        ret
.add:   mov     edx,1024
        call    rt_mem_cstr
        push    eax
        mov     eax,ebx
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        ret
;;; bss rt_sys_args kolibri
rt_kparams      rb 1024
rt_kpath        rb 1024

;;; code rt_mem_cstr : rt_mem_peek_str
rt_mem_cstr:                    ; eax = address, edx = most bytes -> eax = new str of the bytes before the first 0
        push    edi
        mov     edi,eax
        mov     ecx,edx
        xor     edx,edx
        test    ecx,ecx
        jle     .take
        push    eax
        xor     eax,eax
        repne   scasb           ; edi: one past the 0, or past the last byte looked at
        jne     @f
        dec     edi
@@:     pop     eax
        mov     edx,edi
        sub     edx,eax
.take:  pop     edi
        jmp     rt_mem_peek_str

;;; code rt_mem_poke_str
rt_mem_poke_str:                ; eax = address, edx = str: its bytes there
        test    edx,edx
        jz      .out
        push    esi edi
        mov     edi,eax
        lea     esi,[edx+12]
        mov     ecx,[edx+8]
        rep     movsb
        pop     edi esi
.out:   ret

;;; code rt_unpack_check : rt_sb_need rt_sb_cstr rt_sb_int rt_sb_char rt_sb_take rt_raise_valerr
rt_unpack_check:                ; eax = list, edx = the targets (| 0x80000000: from a str - "too many" says no count) - ValueError unless as many
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        mov     ecx,[eax+8]
@@:     push    edx
        and     edx,0x7FFFFFFF
        cmp     ecx,edx
        jne     @f
        pop     edx
        ret
@@:     push    ecx
        push    edx
        push    dword [rt_sb_len]       ; [esp] mark, [esp+4] expected, [esp+8] got, [esp+12] flags
        mov     eax,rt_msg_unp_few
        cmp     ecx,edx
        jb      @f
        mov     eax,rt_msg_unp_many
@@:     call    rt_sb_cstr
        mov     eax,[esp+4]
        xor     edx,edx
        call    rt_sb_int
        mov     eax,[esp+8]
        cmp     eax,[esp+4]
        jb      .got
        test    dword [esp+12],0x80000000
        jnz     .close
.got:   mov     eax,rt_msg_unp_got
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
.close: mov     al,')'
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_unpack_check
rt_msg_unp_few  db 'not enough values to unpack (expected ',0
rt_msg_unp_many db 'too many values to unpack (expected ',0
rt_msg_unp_got  db ', got ',0

;;; code rt_unpack_atleast : rt_sb_need rt_sb_cstr rt_sb_int rt_sb_char rt_sb_take rt_raise_valerr rt_unpack_check
rt_unpack_atleast:              ; eax = list, edx = the targets besides a starred one: ValueError when fewer
        mov     ecx,[eax+8]
        cmp     ecx,edx
        jb      @f
        ret
@@:     push    ecx
        push    edx
        push    dword [rt_sb_len]
        mov     eax,rt_msg_unp_least
        call    rt_sb_cstr
        mov     eax,[esp+4]
        xor     edx,edx
        call    rt_sb_int
        mov     eax,rt_msg_unp_got
        call    rt_sb_cstr
        mov     eax,[esp+8]
        xor     edx,edx
        call    rt_sb_int
        mov     al,')'
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_valerr
;;; data rt_unpack_atleast
rt_msg_unp_least db 'not enough values to unpack (expected at least ',0

;;; code rt_buf_poke : rt_buf_at
rt_buf_poke:                    ; [esp+4] buffer, offset, value, size (bytes): store little-endian
        mov     eax,[esp+4]
        mov     edx,[esp+8]
        mov     ecx,[esp+16]
        call    rt_buf_at
        mov     edx,[esp+12]
        mov     ecx,[esp+16]
        jecxz   .out
@@:     mov     [eax],dl
        inc     eax
        shr     edx,8
        dec     ecx
        jnz     @b
.out:   ret

;;; code rt_buf_peek : rt_buf_at
rt_buf_peek:                    ; [esp+4] buffer, offset, size (bytes) -> eax = little-endian value
        mov     eax,[esp+4]
        mov     edx,[esp+8]
        mov     ecx,[esp+12]
        call    rt_buf_at
        mov     ecx,[esp+12]
        cmp     ecx,4
        je      .dword
        xor     edx,edx
        jecxz   .out
        add     eax,ecx
@@:     dec     eax
        shl     edx,8
        mov     dl,[eax]
        dec     ecx
        jnz     @b
.out:   mov     eax,edx
        ret
.dword: mov     eax,[eax]       ; (4 bytes: little-endian already)
        ret

;;; code rt_buf_poke_str : rt_buf_at
rt_buf_poke_str:                ; eax = buffer, edx = offset, ecx = str: copy its bytes there
        push    esi edi
        mov     esi,ecx
        xor     ecx,ecx
        test    esi,esi
        jz      @f
        mov     ecx,[esi+8]
@@:     push    ecx
        call    rt_buf_at
        pop     ecx
        mov     edi,eax
        lea     esi,[esi+12]
        rep     movsb
        pop     edi esi
        ret

;;; code rt_fround_n : rt_scale10
rt_fround_n:                    ; st0 = value, eax = digits -> st0 = round(value, digits), halves to even
        push    eax
        call    rt_scale10
        frndint
        pop     eax
        neg     eax
        jmp     rt_scale10

