; ---------------------------------------------------------------- files
; File: +0 refcount, +4 destroy, +8 mode (0 read, 1 write, 2 append), +12 the
; contents (read: a str, loaded by open) or the bytes written so far (kolibri),
; +16 read position / bytes buffered, +20 buffer capacity, +24 fd (linux) or
; the path (kolibri, a str), +28 closed.

;;; code rt_file_mode
rt_file_mode:                   ; edx = mode str or 0 -> eax = 0 read, 1 write, 2 append
        xor     eax,eax
        test    edx,edx
        jz      .out
        cmp     dword [edx+8],0
        je      .out
        mov     cl,[edx+12]
        cmp     cl,'w'
        jne     @f
        mov     eax,1
@@:     cmp     cl,'a'
        jne     .out
        mov     eax,2
.out:   ret

;;; code rt_file_notfound : rt_sb_need rt_sb_bytes rt_sb_repr_str rt_sb_take rt_write_err rt_exit
rt_file_notfound:               ; eax = path: FileNotFoundError: [Errno 2] No such file or directory: 'path'
        push    eax
        push    dword [rt_sb_len]
        mov     eax,rt_msg_nofile
        mov     edx,rt_msg_nofile_len
        call    rt_sb_bytes
        mov     eax,[esp+4]
        call    rt_sb_repr_str
        pop     eax
        call    rt_sb_take
        add     esp,4
if defined rt_throw
        mov     ecx,eax
        mov     eax,VTX_FileNotFoundError
        mov     edx,DTX_FileNotFoundError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        push    eax
        mov     ecx,rt_msg_fnf
        mov     edx,rt_msg_fnf_len
        call    rt_write_err
        pop     eax
        lea     ecx,[eax+12]
        mov     edx,[eax+8]
        call    rt_write_err
        mov     ecx,rt_msg_nofile-1
        mov     edx,1
        call    rt_write_err
        mov     ebx,1
        jmp     rt_exit
end if
;;; data rt_file_notfound
rt_msg_fnf      db 'FileNotFoundError: '
rt_msg_fnf_len = $ - rt_msg_fnf
                db 10
rt_msg_nofile   db '[Errno 2] No such file or directory: '
rt_msg_nofile_len = $ - rt_msg_nofile

;;; code rt_file_open linux : rt_alloc rt_file_mode rt_file_notfound rt_file_free rt_sb_need rt_sb_take
rt_file_open:                   ; eax = path, edx = mode (str or 0) -> eax = new file
        push    ebx esi edi ebp
        mov     esi,eax
        call    rt_file_mode
        mov     ebp,eax
        test    esi,esi
        jz      .nf
        lea     ebx,[esi+12]
        xor     ecx,ecx         ; O_RDONLY
        cmp     ebp,1
        jne     @f
        mov     ecx,0x241       ; O_WRONLY | O_CREAT | O_TRUNC
@@:     cmp     ebp,2
        jne     @f
        mov     ecx,0x441       ; O_WRONLY | O_CREAT | O_APPEND
@@:     mov     edx,420         ; 0644
        mov     eax,5
        int     0x80
        test    eax,eax
        js      .nf
        mov     edi,eax
        mov     eax,32
        call    rt_alloc
        mov     dword [eax],1
        mov     dword [eax+4],rt_file_free
        mov     [eax+8],ebp
        mov     [eax+24],edi
        test    ebp,ebp
        jnz     .out
        push    eax             ; reading: all of it now
        push    dword [rt_sb_len]
.more:  mov     eax,4096
        call    rt_sb_need
        mov     ecx,eax
        mov     edx,4096
        mov     ebx,edi
        mov     eax,3
        int     0x80
        test    eax,eax
        jg      @f
        xor     eax,eax
@@:     sub     dword [rt_sb_len],4096
        add     [rt_sb_len],eax
        test    eax,eax
        jnz     .more
        pop     eax
        call    rt_sb_take
        pop     ecx
        mov     [ecx+12],eax
        push    ecx
        mov     ebx,edi
        mov     eax,6           ; close
        int     0x80
        pop     eax
        mov     dword [eax+24],-1
.out:   pop     ebp edi esi ebx
        ret
.nf:    mov     eax,esi
        jmp     rt_file_notfound

;;; code rt_file_write linux
rt_file_write:                  ; eax = file, edx = str -> eax = characters written
        push    ebx
        xor     ecx,ecx
        test    edx,edx
        jz      .none
        cmp     dword [eax+28],0
        jne     .none
        mov     ebx,[eax+24]
        test    ebx,ebx
        js      .none
        lea     ecx,[edx+12]
        mov     edx,[edx+8]
        push    edx
        mov     eax,4
        int     0x80
        pop     eax
        pop     ebx
        ret
.none:  xor     eax,eax
        pop     ebx
        ret

;;; code rt_file_close linux
rt_file_close:                  ; eax = file
        test    eax,eax
        jz      .out
        cmp     dword [eax+28],0
        jne     .out
        mov     dword [eax+28],1
        push    ebx
        mov     ebx,[eax+24]
        test    ebx,ebx
        js      @f
        mov     eax,6
        int     0x80
@@:     pop     ebx
.out:   ret

;;; code rt_file_open kolibri : rt_alloc rt_file_mode rt_file_notfound rt_file_free rt_str_new rt_incref rt_file_f70
rt_file_open:                   ; eax = path, edx = mode -> eax = new file
        push    ebx esi edi ebp
        mov     esi,eax
        call    rt_file_mode
        mov     ebp,eax
        test    esi,esi
        jz      .nf
        mov     eax,32
        call    rt_alloc
        mov     edi,eax
        mov     dword [edi],1
        mov     dword [edi+4],rt_file_free
        mov     [edi+8],ebp
        mov     [edi+24],esi
        mov     eax,esi
        call    rt_incref
        test    ebp,ebp
        jnz     .out
        sub     esp,40          ; reading: the size (70.5), then the whole file (70.0)
        mov     eax,esi
        mov     ecx,5
        xor     edx,edx
        mov     ebx,esp
        call    rt_file_f70
        test    eax,eax
        jnz     .nf2
        mov     eax,[esp+32]    ; size (low dword)
        add     esp,40
        push    eax
        call    rt_str_new
        mov     [edi+12],eax
        pop     edx             ; bytes
        lea     ebx,[eax+12]
        mov     eax,esi
        xor     ecx,ecx         ; 70.0 read
        call    rt_file_f70
.out:   mov     eax,edi
        pop     ebp edi esi ebx
        ret
.nf2:   add     esp,40
.nf:    mov     eax,esi
        jmp     rt_file_notfound

;;; code rt_file_f70 kolibri
rt_file_f70:                    ; eax = path str, ecx = subfunction, edx = size, ebx = buffer, [esp+4] = offset -> eax = status
        push    esi edi ebp
        mov     ebp,esp
        sub     esp,1048        ; information block + the path
        mov     edi,esp
        mov     [edi],ecx
        mov     ecx,[ebp+16]    ; offset (when the caller passed one; 0 otherwise)
        cmp     dword [edi],3
        je      @f
        xor     ecx,ecx
@@:     mov     [edi+4],ecx
        mov     dword [edi+8],0
        mov     [edi+12],edx
        mov     [edi+16],ebx
        lea     edi,[edi+20]
        xor     ecx,ecx
        test    eax,eax
        jz      @f
        lea     esi,[eax+12]
        mov     ecx,[eax+8]
        cmp     ecx,1020
        jbe     @f
        mov     ecx,1020
@@:     rep     movsb
        mov     byte [edi],0
        mov     eax,70
        push    ebx
        mov     ebx,esp
        add     ebx,4
        int     0x40
        pop     ebx
        mov     esp,ebp
        pop     ebp edi esi
        ret

;;; code rt_file_write kolibri : rt_alloc rt_free
rt_file_write:                  ; eax = file, edx = str -> eax = characters written (buffered until close)
        push    ebx esi edi
        mov     ebx,eax
        xor     eax,eax
        test    edx,edx
        jz      .out
        cmp     dword [ebx+28],0
        jne     .out
        mov     esi,edx
        mov     ecx,[ebx+16]
        add     ecx,[esi+8]
        cmp     ecx,[ebx+20]
        jbe     .copy
        add     ecx,ecx         ; grow: twice what is needed
        add     ecx,256
        push    ecx             ; the new capacity
        mov     eax,ecx
        call    rt_alloc
        push    eax             ; the new buffer
        mov     edi,eax
        push    esi
        mov     esi,[ebx+12]
        mov     ecx,[ebx+16]
        rep     movsb
        pop     esi
        mov     eax,[ebx+12]
        call    rt_free
        pop     dword [ebx+12]
        pop     dword [ebx+20]
.copy:  mov     edi,[ebx+12]
        add     edi,[ebx+16]
        mov     ecx,[esi+8]
        mov     eax,ecx
        add     [ebx+16],ecx
        lea     esi,[esi+12]
        rep     movsb
.out:   pop     edi esi ebx
        ret

;;; code rt_file_close kolibri : rt_file_f70
rt_file_close:                  ; eax = file: writing files are written now (70.2, or 70.3 at the end for "a")
        test    eax,eax
        jz      .out
        cmp     dword [eax+28],0
        jne     .out
        mov     dword [eax+28],1
        cmp     dword [eax+8],0
        je      .out
        push    ebx esi
        mov     esi,eax
        cmp     dword [esi+8],2
        jne     .rewrite
        sub     esp,40          ; append: after what is there
        mov     eax,[esi+24]
        mov     ecx,5
        xor     edx,edx
        mov     ebx,esp
        call    rt_file_f70
        mov     ecx,[esp+32]
        add     esp,40
        test    eax,eax
        jnz     .rewrite
        push    ecx             ; offset
        mov     eax,[esi+24]
        mov     ecx,3
        mov     edx,[esi+16]
        mov     ebx,[esi+12]
        call    rt_file_f70
        add     esp,4
        jmp     .done
.rewrite:
        mov     eax,[esi+24]
        mov     ecx,2
        mov     edx,[esi+16]
        mov     ebx,[esi+12]
        call    rt_file_f70
.done:  pop     esi ebx
.out:   ret

;;; code rt_file_free : rt_file_close rt_decref rt_free
rt_file_free:                   ; destroy routine of files: close, drop what it holds
        push    ebx
        mov     ebx,eax
        call    rt_file_close
        cmp     dword [ebx+8],0
        jne     @f
        mov     eax,[ebx+12]    ; the contents
        call    rt_decref
        jmp     .path
@@:
if defined rt_file_f70
        mov     eax,[ebx+12]    ; the write buffer
        call    rt_free
end if
.path:
if defined rt_file_f70
        mov     eax,[ebx+24]
        call    rt_decref
end if
        mov     eax,ebx
        pop     ebx
        jmp     rt_free

;;; code rt_file_read : rt_str_slice
rt_file_read:                   ; eax = file -> eax = the rest of its contents (new str)
        push    ebx
        mov     ebx,eax
        mov     ecx,[ebx+12]
        xor     edx,edx
        test    ecx,ecx
        jz      @f
        mov     edx,[ecx+8]
@@:     push    0               ; flags: lo given, hi given, step 1
        push    1
        push    edx
        push    dword [ebx+16]
        push    ecx
        mov     [ebx+16],edx
        call    rt_str_slice
        add     esp,20
        pop     ebx
        ret

;;; code rt_file_readline : rt_str_slice
rt_file_readline:               ; eax = file -> eax = the next line, with its "\n" ("" at the end)
        push    ebx esi
        mov     ebx,eax
        mov     esi,[ebx+12]
        mov     ecx,[ebx+16]    ; from
        xor     edx,edx
        test    esi,esi
        jz      .end
        mov     edx,[esi+8]     ; length
.scan:  cmp     ecx,edx
        jae     .end
        cmp     byte [esi+12+ecx],10
        je      .nl
        inc     ecx
        jmp     .scan
.nl:    inc     ecx
.end:   push    0
        push    1
        push    ecx
        push    dword [ebx+16]
        push    esi
        mov     [ebx+16],ecx
        call    rt_str_slice
        add     esp,20
        pop     esi ebx
        ret

;;; code rt_file_readlines : rt_file_readline rt_list_new rt_list_push rt_kd_str rt_decref
rt_file_readlines:              ; eax = file -> eax = list of the remaining lines
        push    ebx esi
        mov     ebx,eax
        mov     eax,rt_kd_str
        call    rt_list_new
        mov     esi,eax
.next:  mov     eax,ebx
        call    rt_file_readline
        test    eax,eax
        jz      .out
        cmp     dword [eax+8],0
        jne     @f
        call    rt_decref
        jmp     .out
@@:     push    eax
        mov     eax,esi
        call    rt_list_push
        pop     ecx
        mov     [eax],ecx
        jmp     .next
.out:   mov     eax,esi
        pop     esi ebx
        ret

