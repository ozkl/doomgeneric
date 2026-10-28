#pragma once
// Doom's files (the WAD, doomgeneric.cfg, saved games) live on a FAT32 RAM
// disk: the disk image the boot loader hands over (doom.img; GRUB's
// "module", or next to doom.efi for the UEFI loader) is mounted where it
// sits in memory. Changes last until the machine restarts.
#include <stdint.h>
#include <stddef.h>

void storage_init(const void* image, uint64_t size);   // image may be null: no disk
bool storage_ready();                                  // a FAT32 volume is mounted
// A file's contents where they sit on the RAM disk (read-only use), if
// they're in one piece; null otherwise. Path as from storage_resolve.
const void* storage_map(const char* abs_path, size_t* size);
uint64_t storage_size();

// Paths: '/' or '\' separators, optional "C:" in front, relative to the
// current directory, "." and ".." resolved. Out: an absolute path.
bool storage_resolve(const char* path, char* out, size_t cap);
const char* storage_cwd();
void storage_set_cwd(const char* abs_path);
