// The i386 build has no USB: xHCI needs PCI Express-era hardware, and the
// driver's 2 MB DMA pool is a quarter of a typical 386's memory.
#include "usb.hpp"
bool usb_init(const char*) { return false; }
void usb_poll() {}
