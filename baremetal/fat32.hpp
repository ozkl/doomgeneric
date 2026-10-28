#pragma once
// FAT32 filesystem: read, write, create, delete, rename, directories and
// long (VFAT) file names. Freestanding: needs only memcpy/memset/memcmp,
// malloc/free and the two sector callbacks given to fat_mount(), so it also
// builds on a normal host for testing (tools/fat32_test.sh).
//
// Paths use '/' and are relative to the root ("/saves/doomsav0.dsg" and
// "saves/doomsav0.dsg" are the same). Names match case-insensitively.
#include <stdint.h>
#include <stddef.h>

struct FatDisk {
    // Read or write `count` 512-byte sectors starting at `lba`. Return false on error.
    bool (*read)(uint32_t lba, uint32_t count, void* buf);
    bool (*write)(uint32_t lba, uint32_t count, const void* buf);
    void (*idle)();          // optional: called when an operation finishes (motor off)
};

bool fat_mount(const FatDisk& disk);
bool fat_mounted();
void fat_unmount();

// Reads the whole file into buf. Fails if it's bigger than cap; *len gets
// the file's size either way.
bool fat_read(const char* path, void* buf, size_t cap, size_t* len);
bool fat_write(const char* path, const void* data, size_t len);    // create or replace
bool fat_append(const char* path, const void* data, size_t len);   // create or extend
bool fat_remove(const char* path);                                 // file or empty directory
bool fat_mkdir(const char* path);                                  // parents created too
bool fat_rename(const char* from, const char* to);                 // also moves between directories
bool fat_exists(const char* path, bool* is_dir = nullptr, uint32_t* size = nullptr);
// Calls fn for each entry in a directory (not "." or ".."); stops if fn returns false.
bool fat_list(const char* path, bool (*fn)(const char* name, uint32_t size, bool is_dir, void* ctx), void* ctx);
uint64_t fat_free_bytes();
// Where a file sits, if its clusters are all in one run: its first sector
// and size (a RAM disk can then hand out the data in place).
bool fat_extent(const char* path, uint32_t* first_lba, uint32_t* size);

// Local time for directory entries, as seconds since 1970. Weak: the default
// returns 0 (1 Jan 1980 on disk); define it to give files real dates.
long fat_clock();
