# The flat kernel image, embedded in doom.efi's data section.
.section .data
.balign 4096
.global kernel_image, kernel_image_end
kernel_image:
.incbin KERNEL_BIN
kernel_image_end:

.section .note.GNU-stack,"",@progbits
