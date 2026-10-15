ENTRY(___kosapp_entry)
__image_base__ = 0;
__size_of_stack_reserve__ = 0x100000;

SECTIONS
{
    .text 0x0:
    {
        LONG(0x554e454D);
        LONG(0x32305445);
        LONG(1);
        LONG(___kosapp_entry);
        LONG(___kosapp_image_end);
        LONG(___kosapp_memsize);
        LONG(___kosapp_stack_high);
        LONG(___kosapp_cmdline);
        LONG(___kosapp_name);
        LONG(__subsystem__);

        *(.init)
        *(.text)
        *(SORT(.text$*))
        *(.text.*)
        *(.glue_7t)
        *(.glue_7)

        ___CTOR_LIST__ = .; __CTOR_LIST__ = . ;
                        LONG (-1);*(.ctors); *(.ctor); *(SORT(.ctors.*));  LONG (0);
        ___DTOR_LIST__ = .; __DTOR_LIST__ = . ;
                        LONG (-1); *(.dtors); *(.dtor); *(SORT(.dtors.*));  LONG (0);
        *(.fini)
        *(.gcc_exc)
        PROVIDE (etext = .);
        *(.gcc_except_table)
    }

    .rdata ALIGN(16) :
    {
        *(.rdata)
        *(SORT(.rdata$*))
        ___RUNTIME_PSEUDO_RELOC_LIST__ = .;
        __RUNTIME_PSEUDO_RELOC_LIST__ = .;
        *(.rdata_runtime_pseudo_reloc)
        ___RUNTIME_PSEUDO_RELOC_LIST_END__ = .;
        __RUNTIME_PSEUDO_RELOC_LIST_END__ = .;
    }

    .CRT ALIGN(16) :
    {
         ___crt_xc_start__ = . ;
        *(SORT(.CRT$XC*))
        ___crt_xc_end__ = . ;
        ___crt_xi_start__ = . ;
        *(SORT(.CRT$XI*))
        ___crt_xi_end__ = . ;
        ___crt_xl_start__ = . ;
        *(SORT(.CRT$XL*))
        ___crt_xp_start__ = . ;
        *(SORT(.CRT$XP*))
        ___crt_xp_end__ = . ;
        ___crt_xt_start__ = . ;
        *(SORT(.CRT$XT*))
        ___crt_xt_end__ = . ;
    }

    .data ALIGN(16) :
    {
        PROVIDE ( __data_start__ = .) ;
        *(.data)
        *(.data2)
        *(SORT(.data$*))
        *(.jcr)
        __CRT_MT = .;
        LONG(1);
        PROVIDE ( __data_end__ = .) ;
        *(.data_cygwin_nocopy)
        ___kosapp_image_end = . ;
    }

    .eh_frame BLOCK(16) :
    {
        PROVIDE (___EH_FRAME_BEGIN__ = .) ;
        *(.eh_frame*)
        ___FRAME_END__ = . ;
        LONG(0);
    }

    .bss ALIGN(16):
    {
        *(.bss)
        *(COMMON)
        . = ALIGN(16);
        ___kosapp_cmdline = .;
        . = . + 256;
        ___kosapp_name = .;
        . = . + 1024 + 16;
        ___kosapp_stack_low = .;
        . = . + __size_of_stack_reserve__;
        ___kosapp_stack_high = .;
        ___kosapp_memsize = . ;
    }

    /DISCARD/ :
    {
        *(.idata)
        *(.idata$*)
        *(.eh_fram)
        *(.eh_fram$*)
        *(.debug$S)
        *(.debug$T)
        *(.debug$F)
        *(.drectve)
        *(.note.GNU-stack)
        *(.comment)
        *(.debug_abbrev)
        *(.debug_info)
        *(.debug_line)
        *(.debug_frame)
        *(.debug_loc)
        *(.debug_pubnames)
        *(.debug_aranges)
        *(.debug_ranges)
        *(.reloc)
    }
}