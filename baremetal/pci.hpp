#pragma once
#include <stdint.h>

struct PciDevice { uint8_t bus, dev, fn; };

uint32_t pci_read(const PciDevice& d, int off);
void pci_write(const PciDevice& d, int off, uint32_t v);
// Find the nth (0-based via 'skip') device with this class/subclass and, if
// prog_if >= 0, programming interface.
bool pci_find_class(uint8_t cls, uint8_t sub, int prog_if, PciDevice* out, int skip = 0);
bool pci_find_id(uint16_t vendor, uint16_t device, PciDevice* out);
