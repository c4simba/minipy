; ---------------------------------------------------------------- element types
; Containers know the type of their elements by a descriptor (kd). The runtime
; has those of the built-in types; the code generator emits one per class
; whose objects go into containers. A descriptor:
;   +0  bytes of an element in a slot (4, or 8: float, int)
;   +4  flags: 1 a counted reference, 2 float (x87 qword), 4 a 64-bit int
;   +8  eq:   eax = &a, edx = &b, ecx = kd -> eax = 1 if a == b
;   +12 cmp:  eax = &a, edx = &b, ecx = kd -> eax = -1, 0, 1 (TypeError if unordered)
;   +16 hash: eax = &a, ecx = kd -> edx:eax = hash(a), the interpreter's (TypeError if unhashable)
;   +20 repr: eax = &a, ecx = kd: append repr(a) to the string builder
;   +24 the type's name (NUL-terminated)
; The functions keep ebx, esi, edi and ebp.

;;; code rt_kd_bool : rt_eq_w rt_cmp_i32 rt_hash_i32 rt_repr_bool
;;; data rt_kd_bool
align 4
rt_kd_bool      dd 4,0,rt_eq_w,rt_cmp_i32,rt_hash_i32,rt_repr_bool,rt_tn_bool
rt_tn_bool      db 'bool',0

;;; code rt_kd_int : rt_eq_q rt_cmp_i64 rt_hash_i64 rt_repr_i64
;;; data rt_kd_int
align 4
rt_kd_int       dd 8,4,rt_eq_q,rt_cmp_i64,rt_hash_i64,rt_repr_i64,rt_tn_int
rt_tn_int       db 'int',0

;;; code rt_kd_float : rt_eq_f rt_cmp_f rt_hash_f rt_repr_f
;;; data rt_kd_float
align 4
rt_kd_float     dd 8,2,rt_eq_f,rt_cmp_f,rt_hash_f,rt_repr_f,rt_tn_float
rt_tn_float     db 'float',0

;;; code rt_kd_str : rt_eq_s rt_cmp_s rt_hash_s rt_repr_s
;;; data rt_kd_str
align 4
rt_kd_str       dd 4,1,rt_eq_s,rt_cmp_s,rt_hash_s,rt_repr_s,rt_tn_str
rt_tn_str       db 'str',0

;;; code rt_sb_type : rt_sb_cstr rt_sb_str
rt_sb_type:                     ; eax = a class (its descriptor: name, base, ...): "<class 'module.Name'>"
        push    esi
        push    eax
        mov     eax,rt_s_classq
        call    rt_sb_cstr
        pop     eax
        mov     esi,TYQ         ; the qualified names of the program's classes (by descriptor)
@@:     mov     ecx,[esi]
        test    ecx,ecx
        jz      .short
        cmp     ecx,eax
        je      .qual
        add     esi,8
        jmp     @b
.qual:  mov     eax,[esi+4]
        jmp     .put
.short: mov     eax,[eax]
.put:   call    rt_sb_str
        mov     eax,rt_s_classe
        call    rt_sb_cstr
        pop     esi
        ret
;;; data rt_sb_type
rt_s_classq     db "<class '",0
rt_s_classe     db "'>",0

;;; code rt_type_qualname
rt_type_qualname:               ; eax = a class (its descriptor) -> eax = its __qualname__ (static)
        mov     ecx,TYQN        ; the classes whose qualified name is not their name (Outer.Inner)
@@:     mov     edx,[ecx]
        test    edx,edx
        jz      .name
        cmp     edx,eax
        je      .qual
        add     ecx,8
        jmp     @b
.qual:  mov     eax,[ecx+4]
        ret
.name:  mov     eax,[eax]
        ret

;;; code rt_repr_type : rt_sb_type
rt_repr_type:                   ; eax = &class
        mov     eax,[eax]
        jmp     rt_sb_type

;;; code rt_issubclass
rt_issubclass:                  ; eax = a class, edx = another -> eax = 1 when the first is the second or derives from it
@@:     cmp     eax,edx
        je      .yes
        mov     eax,[eax+4]
        test    eax,eax
        jnz     @b
        ret
.yes:   mov     eax,1
        ret

;;; code rt_kd_type : rt_eq_w rt_cmp_no rt_hash_p rt_repr_type
;;; data rt_kd_type
align 4
rt_kd_type      dd 4,0,rt_eq_w,rt_cmp_no,rt_hash_p,rt_repr_type,rt_tn_type
rt_tn_type      db 'type',0

;;; code rt_kd_obj : rt_eq_w rt_cmp_no rt_hash_p rt_repr_p
;;; data rt_kd_obj
align 4
rt_kd_obj       dd 4,1,rt_eq_w,rt_cmp_no,rt_hash_p,rt_repr_p,rt_tn_obj
rt_tn_obj       db 'object',0

;;; code rt_kd_tuple : rt_tuple_eq_k rt_tuple_cmp_k rt_tuple_hash_k rt_tuple_repr_k
;;; data rt_kd_tuple
align 4
rt_kd_tuple     dd 4,1,rt_tuple_eq_k,rt_tuple_cmp_k,rt_tuple_hash_k,rt_tuple_repr_k,rt_tn_tuple
rt_tn_tuple     db 'tuple',0

;;; code rt_kd_list : rt_list_eq_k rt_list_cmp_k rt_hash_no rt_list_repr_k
;;; data rt_kd_list
align 4
rt_kd_list      dd 4,1,rt_list_eq_k,rt_list_cmp_k,rt_hash_no,rt_list_repr_k,rt_tn_list
rt_tn_list      db 'list',0

;;; code rt_kd_dict : rt_dict_eq_k rt_cmp_no rt_hash_no rt_dict_repr_k
;;; data rt_kd_dict
align 4
rt_kd_dict      dd 4,1,rt_dict_eq_k,rt_cmp_no,rt_hash_no,rt_dict_repr_k,rt_tn_dict
rt_tn_dict      db 'dict',0

;;; code rt_kd_set : rt_set_eq_k rt_cmp_no rt_hash_no rt_set_repr_k
;;; data rt_kd_set
align 4
rt_kd_set       dd 4,1,rt_set_eq_k,rt_cmp_no,rt_hash_no,rt_set_repr_k,rt_tn_set
rt_tn_set       db 'set',0

;;; code rt_kd_tuple_c : rt_kd_tuple
;;; data rt_kd_tuple_c
align 4                         ; (flag 8: values that can be in a cycle - see rt_gc_root)
rt_kd_tuple_c   dd 4,9,rt_tuple_eq_k,rt_tuple_cmp_k,rt_tuple_hash_k,rt_tuple_repr_k,rt_tn_tuple

;;; code rt_kd_list_c : rt_kd_list
;;; data rt_kd_list_c
align 4
rt_kd_list_c    dd 4,9,rt_list_eq_k,rt_list_cmp_k,rt_hash_no,rt_list_repr_k,rt_tn_list

;;; code rt_kd_dict_c : rt_kd_dict
;;; data rt_kd_dict_c
align 4
rt_kd_dict_c    dd 4,9,rt_dict_eq_k,rt_cmp_no,rt_hash_no,rt_dict_repr_k,rt_tn_dict

;;; code rt_kd_set_c : rt_kd_set
;;; data rt_kd_set_c
align 4
rt_kd_set_c     dd 4,9,rt_set_eq_k,rt_cmp_no,rt_hash_no,rt_set_repr_k,rt_tn_set

;;; code rt_kd_obj_c : rt_kd_obj
;;; data rt_kd_obj_c
align 4
rt_kd_obj_c     dd 4,9,rt_eq_w,rt_cmp_no,rt_hash_p,rt_repr_p,rt_tn_obj

;;; code rt_kd_word : rt_eq_w rt_cmp_i32 rt_hash_i32 rt_repr_i32
;;; data rt_kd_word
align 4
rt_kd_word      dd 4,0,rt_eq_w,rt_cmp_i32,rt_hash_i32,rt_repr_i32,rt_tn_word
rt_tn_word      db 'object',0

;;; code rt_eq_w
rt_eq_w:                        ; one dword: bool, int, identity
        mov     eax,[eax]
        cmp     eax,[edx]
        sete    al
        movzx   eax,al
        ret

;;; code rt_cmp_i32
rt_cmp_i32:
        mov     eax,[eax]
        cmp     eax,[edx]
        mov     eax,0
        jl      .lt
        setg    al
        ret
.lt:    dec     eax
        ret

;;; code rt_hash_i32 : rt_hash_int
rt_hash_i32:
        mov     eax,[eax]
        cdq
        jmp     rt_hash_int

;;; code rt_hash_int
rt_hash_int:                    ; edx:eax = int -> edx:eax = its hash (CPython's: modulo 2**61-1, -1 -> -2)
        push    ebx
        mov     ebx,edx         ; the sign
        test    edx,edx
        jns     @f
        neg     eax             ; |x|
        adc     edx,0
        neg     edx
@@:     mov     ecx,edx         ; (|x| & (2**61-1)) + (|x| >> 61)
        shr     ecx,29
        and     edx,0x1FFFFFFF
        add     eax,ecx
        adc     edx,0
        cmp     edx,0x1FFFFFFF  ; at most one modulus too big
        jb      .ok
        ja      .sub
        cmp     eax,-1
        jne     .ok
.sub:   sub     eax,-1
        sbb     edx,0x1FFFFFFF
.ok:    test    ebx,ebx
        jns     .out
        neg     eax
        adc     edx,0
        neg     edx
        cmp     edx,-1
        jne     .out
        cmp     eax,-1
        jne     .out
        dec     eax
.out:   pop     ebx
        ret

;;; code rt_repr_i32 : rt_sb_int
rt_repr_i32:
        mov     eax,[eax]
        jmp     rt_sb_int

;;; code rt_repr_bool : rt_sb_bool
rt_repr_bool:
        mov     eax,[eax]
        jmp     rt_sb_bool

;;; code rt_eq_f
rt_eq_f:
        fld     qword [eax]
        fcomp   qword [edx]
        fnstsw  ax
        sahf
        mov     eax,0
        jp      @f
        sete    al
@@:     ret

;;; code rt_cmp_f
rt_cmp_f:                       ; NaN: neither less nor greater
        fld     qword [eax]
        fcomp   qword [edx]
        fnstsw  ax
        sahf
        mov     eax,0
        jp      .out
        jb      .lt
        seta    al
.out:   ret
.lt:    dec     eax
        ret

;;; code rt_hash_f : rt_shl64 rt_shr64
rt_hash_f:                      ; a float's hash, CPython's: its value modulo 2**61-1
        push    ebx esi edi
        mov     esi,[eax]       ; the mantissa: edi:esi
        mov     edi,[eax+4]
        mov     ebx,edi         ; the sign: bit 31
        mov     ecx,edi
        shr     ecx,20
        and     ecx,0x7FF
        and     edi,0xFFFFF
        cmp     ecx,0x7FF
        je      .special
        test    ecx,ecx
        jz      .denorm
        or      edi,0x100000
        sub     ecx,1075        ; value = mantissa * 2**ecx
        jmp     .have
.denorm: mov    ecx,-1074
.have:  mov     eax,esi
        or      eax,edi
        jz      .zero
        mov     eax,ecx         ; s = the exponent modulo 61
        cdq
        mov     ecx,61
        idiv    ecx
        test    edx,edx
        jns     @f
        add     edx,61
@@:     push    edx
        mov     ecx,edx         ; mantissa * 2**s modulo 2**61-1: rotate left by s in 61 bits
        mov     eax,esi
        mov     edx,edi
        call    rt_shl64
        and     edx,0x1FFFFFFF
        pop     ecx
        push    edx
        push    eax
        neg     ecx
        add     ecx,61
        mov     eax,esi
        mov     edx,edi
        call    rt_shr64
        or      eax,[esp]
        or      edx,[esp+4]
        add     esp,8
        jmp     .sign
.special: or    esi,edi
        jnz     .zero           ; NaN
        mov     eax,314159      ; infinity
        xor     edx,edx
.sign:  test    ebx,ebx
        jns     .out
        neg     eax
        adc     edx,0
        neg     edx
        cmp     edx,-1
        jne     .out
        cmp     eax,-1
        jne     .out
        dec     eax
.out:   pop     edi esi ebx
        ret
.zero:  xor     eax,eax
        xor     edx,edx
        pop     edi esi ebx
        ret

;;; code rt_shl64
rt_shl64:                       ; edx:eax <<= cl (0..63)
        cmp     cl,32
        jae     @f
        shld    edx,eax,cl
        shl     eax,cl
        ret
@@:     mov     edx,eax
        xor     eax,eax
        sub     cl,32
        shl     edx,cl
        ret

;;; code rt_shr64
rt_shr64:                       ; edx:eax >>= cl (0..63), unsigned
        cmp     cl,32
        jae     @f
        shrd    eax,edx,cl
        shr     edx,cl
        ret
@@:     mov     eax,edx
        xor     edx,edx
        sub     cl,32
        shr     eax,cl
        ret

;;; code rt_mul64
rt_mul64:                       ; edx:eax *= the qword at [ecx] (modulo 2**64)
        push    ebx esi
        mov     ebx,eax
        mov     esi,edx
        imul    esi,[ecx]       ; a.hi * b.lo
        mov     edx,[ecx+4]
        imul    edx,ebx         ; a.lo * b.hi
        add     esi,edx
        mov     eax,ebx
        mul     dword [ecx]     ; a.lo * b.lo
        add     edx,esi
        pop     esi ebx
        ret

;;; code rt_repr_f : rt_sb_float
rt_repr_f:
        fld     qword [eax]
        jmp     rt_sb_float

;;; code rt_eq_s : rt_str_eq
rt_eq_s:
        mov     eax,[eax]
        mov     edx,[edx]
        jmp     rt_str_eq

;;; code rt_cmp_s : rt_str_cmp rt_cmp_none
rt_cmp_s:
        mov     eax,[eax]
        mov     edx,[edx]
        test    eax,eax
        jz      .ln
        test    edx,edx
        jnz     rt_str_cmp
        xor     eax,eax
        jmp     rt_cmp_none
.ln:    mov     eax,1
        jmp     rt_cmp_none

;;; code rt_hash_s : rt_str_hash rt_hash_none
rt_hash_s:
        mov     eax,[eax]
        test    eax,eax
        jz      rt_hash_none
        jmp     rt_str_hash

;;; code rt_str_hash
rt_str_hash:                    ; eax = str -> edx:eax = its hash (64-bit FNV-1a of the bytes, the interpreter's)
        push    ebx esi edi ebp
        mov     edi,0x84222325  ; ebp:edi = the offset basis
        mov     ebp,0xCBF29CE4
        test    eax,eax
        jz      .done
        mov     ecx,[eax+8]
        lea     esi,[eax+12]
        jecxz   .done
.l:     movzx   eax,byte [esi]
        xor     edi,eax
        mov     eax,0x1B3       ; * 0x100000001B3: lo*0x1B3 + (hi*0x1B3 + lo<<8)<<32
        mul     edi
        mov     ebx,edi
        shl     ebx,8
        imul    ebp,ebp,0x1B3
        add     ebp,edx
        add     ebp,ebx
        mov     edi,eax
        inc     esi
        dec     ecx
        jnz     .l
.done:  mov     eax,edi
        mov     edx,ebp
        cmp     edx,-1
        jne     @f
        cmp     eax,-1
        jne     @f
        dec     eax
@@:     pop     ebp edi esi ebx
        ret

;;; code rt_repr_s : rt_sb_repr_str rt_repr_none
rt_repr_s:
        mov     eax,[eax]
        test    eax,eax
        jz      rt_repr_none
        jmp     rt_sb_repr_str

;;; code rt_hash_p : rt_hash_none
rt_hash_p:                      ; identity: the address, rotated as CPython does
        mov     eax,[eax]
        test    eax,eax
        jz      rt_hash_none
        mov     edx,eax
        shl     edx,28
        shr     eax,4
        ret

;;; code rt_repr_p : rt_sb_char rt_sb_cstr rt_repr_none
rt_repr_p:                      ; <name object>
        cmp     dword [eax],0
        je      rt_repr_none
        push    ecx
        mov     al,'<'
        call    rt_sb_char
        pop     ecx
        mov     eax,[ecx+24]
        call    rt_sb_cstr
        mov     eax,rt_s_objgt
        jmp     rt_sb_cstr
;;; data rt_repr_p
rt_s_objgt      db ' object>',0

;;; code rt_cmp_no : rt_sb_need rt_sb_cstr rt_sb_char rt_sb_take rt_raise_typeerr
rt_cmp_no:                      ; a type without an order: TypeError
        push    ecx
        push    dword [rt_sb_len]
        mov     eax,rt_s_cmpno1
        call    rt_sb_cstr
        mov     eax,[esp+4]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        mov     eax,rt_s_cmpno2
        call    rt_sb_cstr
        mov     eax,[esp+4]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        mov     al,39
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_typeerr
;;; data rt_cmp_no
rt_s_cmpno1     db "'<' not supported between instances of '",0
rt_s_cmpno2     db "' and '",0

;;; code rt_hash_no : rt_sb_need rt_sb_cstr rt_sb_char rt_sb_take rt_raise_typeerr
rt_hash_no:                     ; a type without a hash: TypeError
        push    ecx
        push    dword [rt_sb_len]
        mov     eax,rt_s_unhash
        call    rt_sb_cstr
        mov     eax,[esp+4]
        mov     eax,[eax+24]
        call    rt_sb_cstr
        mov     al,39
        call    rt_sb_char
        pop     eax
        call    rt_sb_take
        mov     ecx,eax
        jmp     rt_raise_typeerr
;;; data rt_hash_no
rt_s_unhash     db "unhashable type: '",0

;;; code rt_elem_copy : rt_incref
rt_elem_copy:                   ; eax = to, edx = from, ecx = kd: copy an element, taking a reference to it
        push    ecx
        mov     ecx,[edx]
        mov     [eax],ecx
        mov     ecx,[esp]
        cmp     dword [ecx],8
        jne     @f
        mov     ecx,[edx+4]
        mov     [eax+4],ecx
        pop     ecx
        ret
@@:     pop     ecx
        test    byte [ecx+4],1
        jz      @f
        mov     eax,[eax]
        jmp     rt_incref
@@:     ret

;;; code rt_repr_of : rt_sb_need rt_sb_take
rt_repr_of:                     ; eax = &value, ecx = kd -> eax = new str: repr(value)
        push    dword [rt_sb_len]
        call    dword [ecx+20]
        pop     eax
        jmp     rt_sb_take

;;; code rt_raise_key : rt_repr_of rt_raise_keyerr
rt_raise_key:                   ; eax = &key, ecx = kd: raise KeyError(key)
        call    rt_repr_of
        mov     ecx,eax
        jmp     rt_raise_keyerr

;;; code rt_raise_typeerr : rt_raise
rt_raise_typeerr:               ; ecx = message (str, owned): raise TypeError
if defined rt_throw
        mov     eax,VTX_TypeError
        mov     edx,DTX_TypeError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_typeerr
        jmp     rt_raise
end if
;;; data rt_raise_typeerr
rt_s_typeerr    db 'TypeError',0

;;; code rt_raise_keyerr : rt_raise
rt_raise_keyerr:                ; ecx = message (str, owned: the key's repr): raise KeyError
if defined rt_throw
        mov     eax,VTX_KeyError
        mov     edx,DTX_KeyError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_keyerr
        jmp     rt_raise
end if
;;; data rt_raise_keyerr
rt_s_keyerr     db 'KeyError',0

;;; code rt_raise_valerr : rt_raise
rt_raise_valerr:                ; ecx = message (str, owned): raise ValueError
if defined rt_throw
        mov     eax,VTX_ValueError
        mov     edx,DTX_ValueError
        xor     esi,esi
        jmp     rt_raise_builtin
else
        mov     eax,ecx
        mov     esi,rt_s_valerr
        jmp     rt_raise
end if
;;; data rt_raise_valerr
rt_s_valerr     db 'ValueError',0

;;; code rt_keyerr_text : rt_panic
rt_keyerr_text:                 ; esi = the message (C text, quoted already): raise KeyError
if defined rt_throw
        mov     eax,VTX_KeyError
        mov     edx,DTX_KeyError
        xor     ecx,ecx
        jmp     rt_raise_builtin
else
        push    esi
        mov     esi,rt_s_keyerr2
        call    rt_errz
        pop     esi
        jmp     rt_panic
end if
;;; data rt_keyerr_text
rt_s_keyerr2    db 'KeyError: ',0

