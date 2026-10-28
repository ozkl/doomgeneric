// doom.efi: UEFI loader for the bare-metal Doom kernel.
//
// Takes the framebuffer from the Graphics Output Protocol, copies the
// embedded position-independent kernel below 4 GiB, applies its
// relocations, exits boot services and jumps to it with the same
// Multiboot-style information GRUB would have provided. Doom's files:
// doom.img (a FAT32 disk image with the WAD on it), next to doom.efi, is
// read into memory and passed along as a module; it becomes the RAM disk.
//
// Diagnostics: it reports each step on screen while the firmware console is
// still there; "debug" in the load options (GRUB: chainloader ... debug)
// also waits for a key before handing over. From then on, progress is a
// row of coloured blocks along the bottom of the screen, block 1 drawn here
// and the rest by the kernel (see boot_mark in kernel.cpp), so a machine
// that hangs before the game starts still shows how far it got.
#include <efi.h>
#include <efilib.h>

extern const UINT8 kernel_image[], kernel_image_end[];
#include "kernel_layout.h"      // KERNEL_MEM_SIZE, KERNEL_ENTRY, KERNEL_RELA_START/END

struct __attribute__((packed)) MultibootInfo {
    UINT32 flags, mem_lower, mem_upper, boot_device, cmdline;
    UINT32 mods_count, mods_addr;
    UINT32 syms[4];
    UINT32 mmap_length, mmap_addr, drives_length, drives_addr;
    UINT32 config_table, boot_loader_name, apm_table;
    UINT32 vbe_control_info, vbe_mode_info;
    UINT16 vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    UINT64 fb_addr;
    UINT32 fb_pitch, fb_width, fb_height;
    UINT8 fb_bpp, fb_type;
    UINT8 fb_pad[2];            // GRUB aligns what follows to 4 bytes
    UINT8 fb_color[6];          // red, green, blue (position, size) pairs
};
struct __attribute__((packed)) MultibootModule { UINT32 mod_start, mod_end, string, reserved; };
struct __attribute__((packed)) MultibootMmap { UINT32 size; UINT64 addr, len; UINT32 type; };
typedef struct { UINT64 r_offset, r_info; INT64 r_addend; } Elf64_Rela;
#define MAX_MMAP 512

static EFI_SYSTEM_TABLE* ST_;

// Our own memset/memcpy rather than gnu-efi's SetMem/CopyMem: gnu-efi 4.x
// builds those with the Microsoft calling convention (GNU_EFI_USE_MS_ABI)
// and 3.x with the System V one, and nothing tells us which library we
// were linked with. Called the wrong way, SetMem zeroes the wrong memory
// and the loader crashes. Volatile so GCC doesn't turn them back into a
// call to memset/memcpy.
static void fill(void* p, UINTN n, UINT8 v) { volatile UINT8* d = p; while (n--) *d++ = v; }
static void copy(void* to, const void* from, UINTN n) {
    volatile UINT8* d = to;
    const UINT8* s = from;
    while (n--) *d++ = *s++;
}

static void fail(CHAR16* msg) {
    Print(L"\r\ndoom.efi: %s\r\nPress any key to return.\r\n", msg);
    UINTN idx;
    uefi_call_wrapper(ST_->BootServices->WaitForEvent, 3, 1, &ST_->ConIn->WaitForKey, &idx);
}

static void wait_key(void) {
    UINTN idx;
    uefi_call_wrapper(ST_->ConIn->Reset, 2, ST_->ConIn, FALSE);
    uefi_call_wrapper(ST_->BootServices->WaitForEvent, 3, 1, &ST_->ConIn->WaitForKey, &idx);
}

// Progress block n (1-based) along the bottom of the screen, straight into
// the framebuffer: usable after ExitBootServices. Same layout and colours
// as boot_mark in kernel.cpp.
static void fb_block(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, int n, UINT32 rgb) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = gop->Mode->Info;
    UINT32 w = info->HorizontalResolution, h = info->VerticalResolution, pitch = info->PixelsPerScanLine;
    UINT32 x0 = 8 + (UINT32)(n - 1) * 24, y0 = h - 24;
    if (h < 40 || x0 + 16 > w) return;
    UINT32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    UINT32 px = info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor
              ? (r << 16) | (g << 8) | b : (b << 16) | (g << 8) | r;
    UINT32* fb = (UINT32*)(UINTN)gop->Mode->FrameBufferBase;
    for (UINT32 y = y0; y < y0 + 16; y++)
        for (UINT32 x = x0; x < x0 + 16; x++) fb[(UINTN)y * pitch + x] = px;
}

// Allocate pages below 4 GiB (the kernel's DMA structures need 32-bit addresses).
static void* alloc_low(UINTN bytes) {
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFF;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->AllocatePages, 4, AllocateMaxAddress,
                                    EfiLoaderData, EFI_SIZE_TO_PAGES(bytes), &addr)))
        return NULL;
    return (void*)(UINTN)addr;
}

// Pick a 32-bit mode (BGRX or RGBX): keep the current one if it qualifies,
// else the largest that does.
static int gop_format_ok(EFI_GRAPHICS_PIXEL_FORMAT f) {
    return f == PixelBlueGreenRedReserved8BitPerColor || f == PixelRedGreenBlueReserved8BitPerColor;
}
static EFI_GRAPHICS_OUTPUT_PROTOCOL* setup_gop(void) {
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->LocateProtocol, 3, &guid, NULL, (void**)&gop)) || !gop)
        return NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* cur = gop->Mode->Info;
    if (gop_format_ok(cur->PixelFormat) && cur->HorizontalResolution >= 256)
        return gop;
    UINT32 best = (UINT32)-1, best_px = 0;
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
        UINTN size;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &size, &info))) continue;
        UINT32 px = info->HorizontalResolution * info->VerticalResolution;
        if (gop_format_ok(info->PixelFormat) && px > best_px) { best = m; best_px = px; }
    }
    if (best == (UINT32)-1) return NULL;
    uefi_call_wrapper(gop->SetMode, 2, gop, best);
    return gop;
}

// Read a whole file from the volume this image was loaded from.
static void* read_file(EFI_HANDLE image, CHAR16* name, UINTN* size) {
    EFI_GUID lip = LOADED_IMAGE_PROTOCOL, sfs = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_LOADED_IMAGE* li;
    EFI_FILE_IO_INTERFACE* fs;
    EFI_FILE_HANDLE root, f;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, image, &lip, (void**)&li))) return NULL;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, li->DeviceHandle, &sfs, (void**)&fs))) return NULL;
    if (EFI_ERROR(uefi_call_wrapper(fs->OpenVolume, 2, fs, &root))) return NULL;
    if (EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &f, name, EFI_FILE_MODE_READ, 0))) return NULL;
    EFI_FILE_INFO* info = LibFileInfo(f);
    if (!info) return NULL;
    *size = info->FileSize;
    FreePool(info);
    void* buf = alloc_low(*size + 1);
    if (!buf) return NULL;
    UINTN n = *size;
    if (EFI_ERROR(uefi_call_wrapper(f->Read, 3, f, &n, buf)) || n != *size) return NULL;
    uefi_call_wrapper(f->Close, 1, f);
    return buf;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st) {
    InitializeLib(image, st);
    ST_ = st;
    uefi_call_wrapper(st->BootServices->SetWatchdogTimer, 4, 0, 0, 0, NULL);

    // Where are we? The disk image sits next to doom.efi.
    EFI_GUID lip = LOADED_IMAGE_PROTOCOL;
    EFI_LOADED_IMAGE* li;
    uefi_call_wrapper(st->BootServices->HandleProtocol, 3, image, &lip, (void**)&li);
    CHAR16* self = DevicePathToStr(li->FilePath);
    static CHAR16 img_path[512];
    UINTN len = self ? StrLen(self) : 0, cut = 0;
    for (UINTN i = 0; i < len && i < 400; i++) if (self[i] == L'\\' || self[i] == L'/') cut = i + 1;
    for (UINTN i = 0; i < cut; i++) img_path[i] = self[i] == L'/' ? L'\\' : self[i];
    StrCpy(img_path + cut, L"doom.img");

    // "debug" anywhere in the load options: pause before handing over.
    int debug = 0;
    {
        CHAR16* o = li->LoadOptions;
        UINTN on = li->LoadOptionsSize / 2;
        for (UINTN i = 0; o && i + 5 <= on; i++)
            if (o[i] == L'd' && o[i+1] == L'e' && o[i+2] == L'b' && o[i+3] == L'u' && o[i+4] == L'g') debug = 1;
    }

    UINTN disk_size = 0;
    Print(L"Reading %s...\r\n", img_path);
    UINT8* disk = read_file(image, img_path, &disk_size);
    if (!disk) { fail(L"can't read doom.img (it goes next to doom.efi)"); return EFI_NOT_FOUND; }

    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = setup_gop();
    if (!gop) { fail(L"no 32-bit graphics mode available"); return EFI_UNSUPPORTED; }

    // (after setup_gop: a mode change clears the screen)
    Print(L"Doom UEFI loader\r\n");
    Print(L"  loaded from:  %s\r\n", self ? self : L"(unknown)");
    Print(L"  disk image:   %s, %d bytes at 0x%lx\r\n", img_path, disk_size, (UINT64)(UINTN)disk);
    Print(L"  graphics:     %dx%d, %s, framebuffer 0x%lx, %d px/line\r\n",
          gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution,
          gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ? L"BGRX" : L"RGBX",
          (UINT64)gop->Mode->FrameBufferBase, gop->Mode->Info->PixelsPerScanLine);

    // Kernel: copy, zero .bss, relocate.
    UINT8* kbase = alloc_low(KERNEL_MEM_SIZE);
    struct MultibootInfo* mbi = alloc_low(4096);
    struct MultibootMmap* mmap = alloc_low(MAX_MMAP * sizeof(struct MultibootMmap));
    if (!kbase || !mbi || !mmap) { fail(L"out of memory below 4 GiB"); return EFI_OUT_OF_RESOURCES; }
    UINTN image_size = kernel_image_end - kernel_image;
    Print(L"  kernel:       %d bytes (%d in memory) at 0x%lx, entry +0x%lx\r\n",
          image_size, (UINTN)KERNEL_MEM_SIZE, (UINT64)(UINTN)kbase, (UINT64)KERNEL_ENTRY);
    copy(kbase, (void*)kernel_image, image_size);
    fill(kbase + image_size, KERNEL_MEM_SIZE - image_size, 0);
    for (Elf64_Rela* r = (Elf64_Rela*)(kbase + KERNEL_RELA_START); r < (Elf64_Rela*)(kbase + KERNEL_RELA_END); r++) {
        UINT32 type = (UINT32)(r->r_info & 0xFFFFFFFF);
        if (type == 0) continue;                                  // R_X86_64_NONE
        if (type != 8) {                                          // R_X86_64_RELATIVE is all we can do
            Print(L"\r\nkernel relocation %d at +0x%lx (symbol %d) isn't R_X86_64_RELATIVE (8).\r\n",
                  type, r->r_offset, (UINT32)(r->r_info >> 32));
            fail(L"unexpected relocation type in kernel; rebuild with the current Makefile");
            return EFI_LOAD_ERROR;
        }
        *(UINT64*)(kbase + r->r_offset) = (UINT64)(UINTN)kbase + r->r_addend;
    }

    // Multiboot-style boot information, in the same page.
    fill(mbi, 4096, 0);
    struct MultibootModule* mod = (struct MultibootModule*)((UINT8*)mbi + 512);
    char* cmdline = (char*)mbi + 1024;
    char* modname = (char*)mbi + 3072;
    copy(modname, "doom.img", 9);
    mod->mod_start = (UINT32)(UINTN)disk;
    mod->mod_end = (UINT32)(UINTN)disk + (UINT32)disk_size;
    mod->string = (UINT32)(UINTN)modname;
    UINTN n = 0;                                       // load options -> ASCII command line
    CHAR16* opts = li->LoadOptions;
    for (UINTN i = 0; opts && i < li->LoadOptionsSize / 2 && n < 2000; i++) {
        CHAR16 c = opts[i];
        if (!c) break;
        cmdline[n++] = (c >= 32 && c < 127) ? (char)c : ' ';
    }
    cmdline[n] = 0;
    mbi->flags = (1 << 2) | (1 << 3) | (1 << 12);
    mbi->cmdline = (UINT32)(UINTN)cmdline;
    mbi->mods_count = 1;
    mbi->mods_addr = (UINT32)(UINTN)mod;
    mbi->fb_addr = gop->Mode->FrameBufferBase;
    mbi->fb_width = gop->Mode->Info->HorizontalResolution;
    mbi->fb_height = gop->Mode->Info->VerticalResolution;
    mbi->fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    mbi->fb_bpp = 32;
    mbi->fb_type = 1;
    int bgr = gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor;
    mbi->fb_color[0] = bgr ? 16 : 0;  mbi->fb_color[1] = 8;   // red
    mbi->fb_color[2] = 8;             mbi->fb_color[3] = 8;   // green
    mbi->fb_color[4] = bgr ? 0 : 16;  mbi->fb_color[5] = 8;   // blue

    Print(L"  command line: %a\r\n", cmdline);
    Print(L"Progress blocks along the bottom from here: 1 loader done, 2 kernel started,\r\n"
          L"3 memory, 4 video, 5 start-up, 6 sound, 7 USB, 8 mouse, 9 interrupts, 10 disk.\r\n");
    if (debug) { Print(L"debug: press any key to start the kernel.\r\n"); wait_key(); }
    Print(L"Leaving boot services...\r\n");

    // Leave boot services. The map buffer is allocated once, with room to
    // spare, before the first attempt: after a failed ExitBootServices only
    // GetMemoryMap and ExitBootServices may be called (some firmware hangs
    // on an AllocatePool there), and nothing may be printed.
    UINTN map_size = 0, map_cap, key, desc_size;
    UINT32 desc_ver;
    uefi_call_wrapper(st->BootServices->GetMemoryMap, 5, &map_size, NULL, &key, &desc_size, &desc_ver);
    map_cap = map_size + 64 * (desc_size ? desc_size : 48);
    EFI_MEMORY_DESCRIPTOR* map = AllocatePool(map_cap);
    if (!map) { fail(L"out of memory for the memory map"); return EFI_OUT_OF_RESOURCES; }
    for (int attempt = 0; attempt < 8; attempt++) {
        map_size = map_cap;
        if (EFI_ERROR(uefi_call_wrapper(st->BootServices->GetMemoryMap, 5, &map_size, map, &key, &desc_size, &desc_ver)))
            continue;
        if (!EFI_ERROR(uefi_call_wrapper(st->BootServices->ExitBootServices, 2, image, key)))
            goto exited;
    }
    fb_block(gop, 1, 0x404040);     // a grey first block: stuck in the firmware
    for (;;) __asm__ volatile("hlt");

exited:
    __asm__ volatile("cli");
    fb_block(gop, 1, 0xFF0000);     // 1: out of the firmware
    // Hand the kernel the free RAM as a Multiboot memory map. Only
    // EfiConventionalMemory: boot-services memory still holds the page
    // tables the kernel keeps using, and doom.img sits in loader memory.
    {
        UINTN count = 0;
        for (UINTN off = 0; off < map_size && count < MAX_MMAP; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)map + off);
            if (d->Type != EfiConventionalMemory) continue;
            mmap[count].size = sizeof(struct MultibootMmap) - 4;
            mmap[count].addr = d->PhysicalStart;
            mmap[count].len = d->NumberOfPages * 4096;
            mmap[count].type = 1;
            count++;
        }
        mbi->mmap_addr = (UINT32)(UINTN)mmap;
        mbi->mmap_length = (UINT32)(count * sizeof(struct MultibootMmap));
        mbi->flags |= 1 << 6;
    }
    ((void (*)(void*))(kbase + KERNEL_ENTRY))(mbi);   // never returns
    for (;;) __asm__ volatile("hlt");
}
