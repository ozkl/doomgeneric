/* Position-independent kernel for the UEFI loader. Linked at 0; doom.efi
   copies it wherever the firmware gives it room and applies the relocations
   between __rela_start and __rela_end (all R_X86_64_RELATIVE). */
ENTRY(efi_entry64)
SECTIONS
{
    . = 0;
    __kernel_start = .;
    .text : ALIGN(4K) { *(.text .text.*) }
    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
    }
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
    .dynamic : { *(.dynamic) }
    .got : { *(.got .got.plt) }
    .data : ALIGN(4K) { *(.data .data.*) }
    .rela.dyn : ALIGN(8) { __rela_start = .; *(.rela.dyn .rela.*) __rela_end = .; }
    __image_end = .;
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }
    __kernel_end = .;
    /DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) *(.interp) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
