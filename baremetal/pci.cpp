#include "pci.hpp"
#include "hw.hpp"

static uint32_t cfg_addr(int bus, int dev, int fn, int off) {
    return 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC);
}
uint32_t pci_read(const PciDevice& d, int off) {
    outl(0xCF8, cfg_addr(d.bus, d.dev, d.fn, off));
    return inl(0xCFC);
}
void pci_write(const PciDevice& d, int off, uint32_t v) {
    outl(0xCF8, cfg_addr(d.bus, d.dev, d.fn, off));
    outl(0xCFC, v);
}

template <typename F> static bool scan(F match, PciDevice* out) {
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                PciDevice d = { (uint8_t)bus, (uint8_t)dev, (uint8_t)fn };
                uint32_t id = pci_read(d, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                if (match(d, id)) { *out = d; return true; }
                if (fn == 0 && !(pci_read(d, 0x0C) & 0x800000)) break;   // single-function
            }
    return false;
}

bool pci_find_class(uint8_t cls, uint8_t sub, int prog_if, PciDevice* out, int skip) {
    return scan([&](const PciDevice& d, uint32_t) {
        uint32_t c = pci_read(d, 0x08);
        if ((c >> 24) != cls || ((c >> 16) & 0xFF) != sub) return false;
        if (prog_if >= 0 && (int)((c >> 8) & 0xFF) != prog_if) return false;
        return skip-- == 0;
    }, out);
}

bool pci_find_id(uint16_t vendor, uint16_t device, PciDevice* out) {
    return scan([&](const PciDevice&, uint32_t id) {
        return id == ((uint32_t)device << 16 | vendor);
    }, out);
}
