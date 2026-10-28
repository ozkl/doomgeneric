# Multiboot entry: switch from 32-bit protected mode to 64-bit long mode,
# identity-map the first 4 GiB with 2 MiB pages, then call kmain().

#ifndef EFI
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

#endif

.section .bss
.align 4096
#ifndef EFI
pml4:   .skip 4096
pdpt:   .skip 4096
pdir:   .skip 4096 * 4
#endif
stack_bottom:
        .skip 512 << 10              # 512 KiB
.global stack_top
stack_top:

.section .data
.global mb_magic, mb_info
mb_magic: .long 0
mb_info:  .long 0

.align 16
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF     # 0x08: 64-bit code
    .quad 0x00CF92000000FFFF     # 0x10: data
gdt64_ptr:
    .word gdt64_ptr - gdt64 - 1
    .quad gdt64

.section .text
#ifndef EFI
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

    # PML4[0] -> PDPT
    mov  $pdpt, %eax
    or   $3, %eax
    mov  %eax, pml4

    # PDPT[0..3] -> four page directories
    mov  $pdir, %eax
    or   $3, %eax
    xor  %ecx, %ecx
1:  mov  %eax, pdpt(,%ecx,8)
    add  $4096, %eax
    inc  %ecx
    cmp  $4, %ecx
    jne  1b

    # 2048 x 2 MiB pages = 4 GiB identity map
    xor  %ecx, %ecx
2:  mov  %ecx, %eax
    shl  $21, %eax
    or   $0x83, %eax             # present | writable | huge
    mov  %eax, pdir(,%ecx,8)
    movl $0, pdir+4(,%ecx,8)
    inc  %ecx
    cmp  $2048, %ecx
    jne  2b

    # CR4: PAE | OSFXSR | OSXMMEXCPT (enables SSE)
    mov  %cr4, %eax
    or   $((1<<5) | (1<<9) | (1<<10)), %eax
    mov  %eax, %cr4

    mov  $pml4, %eax
    mov  %eax, %cr3

    # EFER.LME
    mov  $0xC0000080, %ecx
    rdmsr
    or   $(1<<8), %eax
    wrmsr

    # CR0: paging on, FPU monitoring on, emulation off
    mov  %cr0, %eax
    and  $~(1<<2), %eax
    or   $((1<<31) | (1<<1)), %eax
    mov  %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_mode

.code64
long_mode:
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  %ax, %fs
    mov  %ax, %gs
    mov  $stack_top, %rsp
    fninit
    call kmain
3:  cli
    hlt
    jmp  3b
#else
# UEFI build: doom.efi has already left boot services, copied this kernel
# somewhere below 4 GiB and applied its relocations. We arrive in long mode on
# the firmware's identity-mapped page tables (which cover every RAM and MMIO
# region, including a framebuffer above 4 GiB), so we keep them and only
# switch to our own GDT and stack. %rdi = Multiboot-style info block.
.code64
.global efi_entry64
efi_entry64:
    cli
    lea  stack_top(%rip), %rsp
    movl $0x2BADB002, mb_magic(%rip)
    movl %edi, mb_info(%rip)
    movq $-1, phys_limit(%rip)
    lgdt gdt64_ptr(%rip)
    pushq $0x08
    lea  1f(%rip), %rax
    pushq %rax
    lretq
1:  mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  %ax, %fs
    mov  %ax, %gs
    fninit
    call kmain
3:  cli
    hlt
    jmp  3b
#endif

# ---- Interrupt stubs (save caller-saved registers, call C, iretq) ----
.macro IRQ_STUB name, handler
.global \name
\name:
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    cld
    call \handler
    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rdi
    pop  %rsi
    pop  %rdx
    pop  %rcx
    pop  %rax
    iretq
.endm

# The timer's handler mixes sound and runs the music synth (ordinary
# compiled code that may use SSE/x87), so it also saves the FPU/SSE state.
# Interrupt gates keep IF clear, so one save area is enough.
.global isr_timer
isr_timer:
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    cld
    fxsave timer_fx(%rip)
    call irq_timer
    fxrstor timer_fx(%rip)
    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rdi
    pop  %rsi
    pop  %rdx
    pop  %rcx
    pop  %rax
    iretq

.section .bss
.align 16
timer_fx: .skip 512
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
