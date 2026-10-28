ENTRY(_start)
SECTIONS
{
    . = 1M;
    __kernel_start = .;
    .text : ALIGN(4K) {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }
    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
    }
    .data : ALIGN(4K) { *(.data .data.*) }
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }
    __kernel_end = .;
    /DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) }
}
