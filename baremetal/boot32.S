# Multiboot entry for the 32-bit builds (i386 and up): stay in 32-bit protected mode
# with a flat GDT and paging off, so every physical address (RAM, the
# framebuffer, PCI MMIO) below 4 GiB is directly addressable. Then call kmain().

.set MB_MAGIC,  0x1BADB002
.set MB_FLAGS,  (1<<0) | (1<<1) | (1<<2)   # page-align modules, memory info, video mode
.set MB_CHECK,  -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
    .long MB_MAGIC, MB_FLAGS, MB_CHECK
    .long 0, 0, 0, 0, 0          # a.out kludge fields (unused for ELF)
    .long 0                      # mode_type: 0 = linear graphics
#ifndef FB_W
#define FB_W 640
#define FB_H 480
#define FB_BPP 32
#endif
#ifndef MB_W                     /* what to ask the boot loader for; 0: no preference */
#define MB_W FB_W
#define MB_H FB_H
#define MB_BPP FB_BPP
#endif
    .long MB_W, MB_H, MB_BPP     # preferred width, height, depth

.section .bss
.align 16
stack_bottom:
        .skip 512 << 10              # 512 KiB
.global stack_top
stack_top:

.section .data
.global mb_magic, mb_info
mb_magic: .long 0
mb_info:  .long 0
.global fpu_present
fpu_present: .long 0
fpu_sw:   .word 0x5A5A

.align 8
gdt32:
    .quad 0
    .quad 0x00CF9A000000FFFF     # 0x08: 32-bit code, base 0, limit 4 GiB
    .quad 0x00CF92000000FFFF     # 0x10: data, base 0, limit 4 GiB
gdt32_ptr:
    .word gdt32_ptr - gdt32 - 1
    .long gdt32

.section .text
.code32
.global _start
_start:
    cli
    mov  $stack_top, %esp
    mov  %eax, mb_magic
    mov  %ebx, mb_info

    # Zero .bss ourselves rather than trusting the loader.
    mov  $__bss_start, %edi
    mov  $__bss_end, %ecx
    sub  %edi, %ecx
    xor  %eax, %eax
    cld
    rep stosb
    mov  $stack_top, %esp        # the stack lives in .bss too

    # Our own GDT: the loader's may be gone or temporary.
    lgdt gdt32_ptr
    ljmp $0x08, $1f
1:  mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  %ax, %fs
    mov  %ax, %gs

    # CR0: paging off. Look for an x87: FNINIT then FNSTSW leaves the status
    # word untouched when there's none (a 386 or 486SX on its own).
    # No SSE before the Pentium III, so CR4 (which a 386 lacks) is left alone.
    mov  %cr0, %eax
    and  $~((1<<31) | (1<<2) | (1<<1)), %eax
    mov  %eax, %cr0
    fninit
    fnstsw fpu_sw
    cmpw $0, fpu_sw
    jne  2f
    movl $1, fpu_present
    or   $(1<<1), %eax           # MP: an FPU is there
#ifndef CPU_386
    or   $(1<<5), %eax           # NE: native FPU errors (486 and later)
#endif
    jmp  4f
2:  or   $(1<<2), %eax           # EM: no FPU, so x87 instructions trap (#NM) instead of hanging
4:  mov  %eax, %cr0

    call kmain
3:  cli
    hlt
    jmp  3b

# ---- Interrupt stubs (save caller-saved registers, call C, iret) ----
.macro IRQ_STUB name, handler
.global \name
\name:
    push %eax
    push %ecx
    push %edx
    cld
    call \handler
    pop  %edx
    pop  %ecx
    pop  %eax
    iret
.endm

# The timer's handler mixes sound and runs the music synth (ordinary
# compiled code that may use the x87), so it also saves the FPU state.
.global isr_timer
isr_timer:
    push %eax
    push %ecx
    push %edx
    cld
    fnsave timer_fpu
    call irq_timer
    frstor timer_fpu
    pop  %edx
    pop  %ecx
    pop  %eax
    iret

.section .bss
.align 16
timer_fpu: .skip 108
.section .text

IRQ_STUB isr_keyboard, irq_keyboard
IRQ_STUB isr_mouse,    irq_mouse
IRQ_STUB isr_spurious, irq_spurious

.global isr_fault
isr_fault:
    cli
    call fault_handler
4:  hlt
    jmp 4b

.section .note.GNU-stack,"",@progbits
