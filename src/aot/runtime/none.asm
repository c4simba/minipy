; ---------------------------------------------------------------- None
; None is a null reference; an int -2**63, a float the NaN rt_fnone, a bool 2.

;;; code rt_fnone
;;; data rt_fnone
align 8
rt_fnone        dd 0x6E650001,0x7FF86E6F

;;; code rt_finf : rt_ftoi
;;; data rt_finf
align 8
rt_fnan         dd 0,0x7FF80000   ; math.nan (not None's NaN); math.inf is rt_finf of rt_ftoi

;;; code rt_unbound_text : rt_panic
rt_unbound_text:                ; esi = message (C text): raise UnboundLocalError
if defined rt_throw
        mov     eax,VTX_UnboundLocalError
        mov     edx,DTX_UnboundLocalError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_unbound
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_unbound_text
rt_s_unbound    db 'UnboundLocalError: ',0

;;; code rt_nameerr_text : rt_panic
rt_nameerr_text:                ; esi = message (C text): raise NameError
if defined rt_throw
        mov     eax,VTX_NameError
        mov     edx,DTX_NameError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_nameerr
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_nameerr_text
rt_s_nameerr    db 'NameError: ',0

;;; code rt_typeerr_text : rt_panic
rt_typeerr_text:                ; esi = message (C text): raise TypeError
if defined rt_throw
        mov     eax,VTX_TypeError
        mov     edx,DTX_TypeError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_typeerr2
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_typeerr_text
rt_s_typeerr2   db 'TypeError: ',0

;;; code rt_attrerr_text : rt_panic
rt_attrerr_text:                ; esi = message (C text): raise AttributeError
if defined rt_throw
        mov     eax,VTX_AttributeError
        mov     edx,DTX_AttributeError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_attrerr
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_attrerr_text
rt_s_attrerr    db 'AttributeError: ',0

;;; code rt_attrerr_none : rt_attrerr_text
rt_attrerr_none:                ; esi = an attribute's name (C text): AttributeError "'NoneType' object has no attribute 'name'"
        mov     edi,rt_attr_msg
        mov     ecx,rt_s_none_attr
.pre:   mov     al,[ecx]
        test    al,al
        jz      .name
        mov     [edi],al
        inc     ecx
        inc     edi
        jmp     .pre
.name:  mov     ecx,250         ; (the buffer's room)
.n:     mov     al,[esi]
        test    al,al
        jz      .end
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     .n
.end:   mov     byte [edi],39
        mov     byte [edi+1],0
        mov     esi,rt_attr_msg
        jmp     rt_attrerr_text
;;; data rt_attrerr_none
rt_s_none_attr  db "'NoneType' object has no attribute '",0
;;; bss rt_attrerr_none
rt_attr_msg     rb 300

;;; code rt_hash_none
rt_hash_none:                   ; -> edx:eax = hash(None), the interpreter's
        mov     eax,0xFCA86420
        xor     edx,edx
        ret

;;; code rt_repr_none : rt_sb_cstr
rt_repr_none:                   ; append None
        mov     eax,rt_s_none
        jmp     rt_sb_cstr
;;; data rt_repr_none
rt_s_none       db 'None',0

;;; code rt_cmp_none : rt_sb_need rt_sb_cstr rt_sb_char rt_sb_take rt_raise_typeerr
rt_cmp_none:                    ; one of two values compared for order is None (eax = 1: the left one), ecx = the other's kd: TypeError
        push    ecx
        push    eax
        push    dword [rt_sb_len]
        mov     eax,rt_s_cmpn1
        call    rt_sb_cstr
        cmp     dword [esp+4],0
        je      @f
        mov     eax,rt_s_cmpnn
        call    rt_sb_cstr
        mov     eax,[esp+8]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        jmp     .end
@@:     mov     eax,[esp+8]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        mov     eax,rt_s_cmpn2
        call    rt_sb_cstr
.end:   mov     al,39
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_typeerr
;;; data rt_cmp_none
rt_s_cmpn1      db "'<' not supported between instances of '",0
rt_s_cmpnn      db "NoneType' and '",0
rt_s_cmpn2      db "' and 'NoneType",0

;;; code rt_kd_oint : rt_eq_q rt_cmp_oi64 rt_hash_oi64 rt_repr_oi64
;;; data rt_kd_oint
align 4
rt_kd_oint      dd 8,4,rt_eq_q,rt_cmp_oi64,rt_hash_oi64,rt_repr_oi64,rt_tn_oint
rt_tn_oint      db 'int',0

;;; code rt_cmp_oi64 : rt_cmp_i64 rt_cmp_none
rt_cmp_oi64:
        cmp     dword [eax+4],0x80000000
        jne     @f
        cmp     dword [eax],0
        je      .ln
@@:     cmp     dword [edx+4],0x80000000
        jne     rt_cmp_i64
        cmp     dword [edx],0
        jne     rt_cmp_i64
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_hash_oi64 : rt_hash_i64 rt_hash_none
rt_hash_oi64:
        cmp     dword [eax+4],0x80000000
        jne     rt_hash_i64
        cmp     dword [eax],0
        jne     rt_hash_i64
        jmp     rt_hash_none

;;; code rt_repr_oi64 : rt_repr_i64 rt_repr_none
rt_repr_oi64:
        cmp     dword [eax+4],0x80000000
        jne     rt_repr_i64
        cmp     dword [eax],0
        jne     rt_repr_i64
        jmp     rt_repr_none

;;; code rt_kd_obool : rt_eq_w rt_cmp_obool rt_hash_obool rt_repr_obool
;;; data rt_kd_obool
align 4
rt_kd_obool     dd 4,0,rt_eq_w,rt_cmp_obool,rt_hash_obool,rt_repr_obool,rt_tn_obool
rt_tn_obool     db 'bool',0

;;; code rt_cmp_obool : rt_cmp_i32 rt_cmp_none
rt_cmp_obool:
        cmp     dword [eax],2
        je      .ln
        cmp     dword [edx],2
        jne     rt_cmp_i32
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_hash_obool : rt_hash_i32 rt_hash_none
rt_hash_obool:
        cmp     dword [eax],2
        jne     rt_hash_i32
        jmp     rt_hash_none

;;; code rt_repr_obool : rt_repr_bool rt_repr_none
rt_repr_obool:
        cmp     dword [eax],2
        jne     rt_repr_bool
        jmp     rt_repr_none

;;; code rt_kd_ofloat : rt_eq_of rt_cmp_of rt_hash_of rt_repr_of_f
;;; data rt_kd_ofloat
align 4
rt_kd_ofloat    dd 8,2,rt_eq_of,rt_cmp_of,rt_hash_of,rt_repr_of_f,rt_tn_ofloat
rt_tn_ofloat    db 'float',0

;;; code rt_is_fnone : rt_fnone
rt_is_fnone:                    ; eax = &float -> ZF set if it is None (keeps eax, edx)
        push    ecx
        mov     ecx,[eax+4]
        cmp     ecx,[rt_fnone+4]
        jne     @f
        mov     ecx,[eax]
        cmp     ecx,[rt_fnone]
@@:     pop     ecx
        ret

;;; code rt_eq_of : rt_is_fnone rt_eq_f
rt_eq_of:                       ; None equals None only
        push    ebx
        xor     ebx,ebx
        call    rt_is_fnone
        jne     @f
        inc     ebx
@@:     xchg    eax,edx
        call    rt_is_fnone
        jne     @f
        add     ebx,2
@@:     xchg    eax,edx
        test    ebx,ebx
        jz      .num
        cmp     ebx,3
        sete    al
        movzx   eax,al
        pop     ebx
        ret
.num:   pop     ebx
        jmp     rt_eq_f

;;; code rt_cmp_of : rt_is_fnone rt_cmp_f rt_cmp_none
rt_cmp_of:
        call    rt_is_fnone
        je      .ln
        xchg    eax,edx
        call    rt_is_fnone
        xchg    eax,edx
        jne     rt_cmp_f
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_hash_of : rt_is_fnone rt_hash_f rt_hash_none
rt_hash_of:
        call    rt_is_fnone
        jne     rt_hash_f
        jmp     rt_hash_none

;;; code rt_repr_of_f : rt_is_fnone rt_repr_f rt_repr_none
rt_repr_of_f:
        call    rt_is_fnone
        jne     rt_repr_f
        jmp     rt_repr_none

