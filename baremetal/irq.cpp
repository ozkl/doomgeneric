// Interrupt handlers. Built with -mgeneral-regs-only. The keyboard and
// mouse stubs save only general-purpose registers; the timer's also saves
// the FPU/SSE state, because the music synth it runs is ordinary code.
#include <stdint.h>
#include "hw.hpp"
#include "mixer.hpp"

// The PIT runs at 240 Hz: `fine_ticks` counts every interrupt (sound is
// topped up that often), `ticks` every 4th (60 Hz).
volatile uint32_t ticks, fine_ticks;
volatile uint8_t kbd_buf[256];
volatile uint8_t kbd_head, kbd_tail;

extern "C" void irq_timer() {
    fine_ticks = fine_ticks + 1;
    if ((fine_ticks & 3) == 0) ticks = ticks + 1;
    outb(0x20, 0x20);
    mixer_tick();                             // Doom's sound effects, on time however slow a frame is
}

extern "C" void irq_keyboard() {
    uint8_t b = inb(0x60);
    kbd_buf[kbd_head] = b;
    kbd_head = kbd_head + 1;
    outb(0x20, 0x20);
}

// PS/2 mouse (IRQ 12, on the slave PIC): bytes are queued here and put
// together into packets by kernel.cpp.
volatile uint8_t mouse_buf[256];
volatile uint8_t mouse_head, mouse_tail;

extern "C" void irq_mouse() {
    uint8_t b = inb(0x60);
    mouse_buf[mouse_head] = b;
    mouse_head = mouse_head + 1;
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

extern "C" void irq_spurious() {}
