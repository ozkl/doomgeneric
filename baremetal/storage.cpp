// The FAT32 RAM disk Doom's files live on. See storage.hpp.
#include "storage.hpp"
#include "fat32.hpp"

extern "C" {
int printf(const char*, ...);
void* memcpy(void*, const void*, size_t);
size_t strlen(const char*);
long time(long*);
}

static uint8_t* ram;                      // the image, used where the boot loader put it
static uint64_t ram_sectors;
static bool mounted;
static char cwd[256] = "/";

static bool ram_read(uint32_t lba, uint32_t n, void* buf) {
    if ((uint64_t)lba + n > ram_sectors) return false;
    memcpy(buf, ram + (uint64_t)lba * 512, (size_t)n * 512);
    return true;
}
static bool ram_write(uint32_t lba, uint32_t n, const void* buf) {
    if ((uint64_t)lba + n > ram_sectors) return false;
    memcpy(ram + (uint64_t)lba * 512, buf, (size_t)n * 512);
    return true;
}
static const FatDisk ram_disk = {ram_read, ram_write, nullptr};

void storage_init(const void* image, uint64_t size) {
    ram = (uint8_t*)image;
    ram_sectors = image ? size / 512 : 0;
    if (!ram_sectors) { printf("Storage: no disk image from the boot loader\n"); return; }
    mounted = fat_mount(ram_disk);
    if (mounted)
        printf("Storage: %lu MB FAT32 RAM disk, %lu MB free\n",
               (unsigned long)(size >> 20), (unsigned long)(fat_free_bytes() >> 20));
    else
        printf("Storage: the %lu MB disk image isn't FAT32\n", (unsigned long)(size >> 20));
}
bool storage_ready() { return mounted; }
const void* storage_map(const char* path, size_t* size) {
    uint32_t lba, n;
    if (!mounted || !fat_extent(path, &lba, &n) || !n) return nullptr;
    if ((uint64_t)lba * 512 + n > ram_sectors * 512) return nullptr;
    *size = n;
    return ram + (uint64_t)lba * 512;
}
uint64_t storage_size() { return ram_sectors * 512; }

// ---------------------------------------------------------------- paths
const char* storage_cwd() { return cwd; }
void storage_set_cwd(const char* p) {
    size_t n = strlen(p);
    if (n >= sizeof cwd) return;
    memcpy(cwd, p, n + 1);
}

bool storage_resolve(const char* path, char* out, size_t cap) {
    if (!path || cap < 2) return false;
    if (((path[0] | 32) >= 'a' && (path[0] | 32) <= 'z') && path[1] == ':') path += 2;   // C:\DOOM.WAD
    char buf[512];
    size_t n = 0;
    bool abs = path[0] == '/' || path[0] == '\\';
    if (!abs) {
        size_t c = strlen(cwd);
        if (c >= sizeof buf) return false;
        memcpy(buf, cwd, c);
        n = c;
        buf[n++] = '/';
    }
    for (const char* p = path; *p && n < sizeof buf - 1; p++) buf[n++] = *p == '\\' ? '/' : *p;
    buf[n] = 0;
    // Normalize into out: split on '/', drop "." and empty parts, ".." pops.
    size_t o = 0;
    out[o++] = '/';
    for (char* p = buf; *p;) {
        while (*p == '/') p++;
        char* e = p;
        while (*e && *e != '/') e++;
        size_t len = e - p;
        if (len == 0) break;
        if (len == 1 && p[0] == '.') {
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; }   // back to the previous '/'
            if (o > 1) o--;
        } else {
            if (o > 1) { if (o + 1 >= cap) return false; out[o++] = '/'; }
            if (o + len >= cap) return false;
            memcpy(out + o, p, len);
            o += len;
        }
        p = e;
    }
    out[o] = 0;
    return true;
}

// File dates from the clock (libc.cpp: CMOS time, kept local).
long fat_clock() { return time(nullptr); }
