# Bare-metal Doom: boots straight into the game, no operating system.
#
#   make iso          bootable CD / USB stick image (doom.iso), BIOS and UEFI
#                     (UEFI when grub-efi-amd64-bin is installed); dd it to a
#                     USB stick
#   make run          boot the kernel directly in QEMU
#   make run-iso      boot doom.iso in QEMU
#   make efi          UEFI application (doom.efi) plus doom.img; copy both to
#                     the same folder on the EFI system partition
#   make run-efi      boot doom.efi under UEFI firmware (OVMF) in QEMU
#   make              the kernel alone (doom.elf)
#
# WAD= the IWAD (and any PWADs) to put on the disk, e.g.
#   make iso WAD=~/games/doom2.wad
#   make iso WAD="doom.wad sigil.wad" ARGS="-file sigil.wad"
# Default: the first of *.wad here, or doom1.wad / freedoom from
# /usr/share/games/doom (Debian/Ubuntu: doom-wad-shareware, freedoom).
#
# doom.img is a FAT32 disk image with the WAD(s) on it. The boot loader
# loads it into memory and the kernel mounts it there as a RAM disk, so the
# config file and saved games work too (until the machine restarts).
#
# ARCH=x86_64 (default) or ARCH=i386 (a 386 or later with a 387, or a
# 486DX; outputs named doom-i386.*; needs gcc-multilib).
#
# Doom itself (../doomgeneric) is compiled unchanged; doomgeneric_baremetal.c
# and i_sound_baremetal.c connect it to kernel.cpp. Nothing from libc or
# libstdc++ is linked: libc.cpp and runtime.cpp stand in for them.

ARCH     ?= x86_64
CC       ?= gcc
CXX      ?= g++
# The doomgeneric sources: this directory sits next to them in the
# baremetaldoom repository. DOOM=/path/to/doomgeneric if it doesn't.
DOOM     ?= ../doomgeneric
ifeq ($(wildcard $(DOOM)/doomgeneric.h),)
$(error Doom's source isn't at $(DOOM). Put this directory in the baremetaldoom repository (next to doomgeneric/), or give its path: make DOOM=/path/to/baremetaldoom/doomgeneric)
endif
BUILD    ?= build/$(ARCH)

GRUB_MKRESCUE ?= $(shell command -v grub2-mkrescue || command -v grub-mkrescue)
MKFS_FAT      ?= $(shell command -v mkfs.fat || command -v mkfs.vfat || echo /usr/sbin/mkfs.fat)

WADDIR   ?= /usr/share/games/doom
WAD      ?= $(firstword $(wildcard *.wad) $(wildcard $(addprefix $(WADDIR)/,doom.wad doom2.wad doom1.wad freedoom2.wad freedoom1.wad)))

ifeq ($(ARCH),x86_64)
MFLAG    := -m64
SUFFIX   :=
BOOT_S   := boot.S
USB_OBJ  := usb
LDEMU    :=
# The screen's own resolution first (GRUB's "auto"); Doom is scaled to the
# largest 4:3 rectangle that fits.
GFXMODE  := auto,1024x768x32,800x600x32,640x480x32,640x480x16,640x480x8
# The disk: room beyond the WADs for the config and saved games, and at
# least 40 MB so FAT32 has the 65525 clusters other systems insist on.
IMG_SPARE_MB ?= 16
IMG_MIN_MB   ?= 40
FBDEFS   := -DFB_W=640 -DFB_H=480 -DFB_BPP=32 -DMB_W=0 -DMB_H=0 -DMB_BPP=0
# PIE=1 builds the position-independent kernel that doom.efi embeds.
ifeq ($(PIE),1)
ARCHFLAGS := -fpie -mno-red-zone -msse2
BOOTDEFS := -DEFI
else
ARCHFLAGS := -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -msse2
BOOTDEFS :=
endif
else ifeq ($(ARCH),i386)
MFLAG    := -m32
SUFFIX   := -i386
BOOT_S   := boot32.S
USB_OBJ  := usb_none
LDEMU    := -m elf_i386
# 8 bits: Doom's palette goes straight into the VGA DAC, and it's quick to
# redraw on an old machine.
GFXMODE  := 640x480x8,640x400x8,800x600x8,640x480x16,640x480x32,auto
# The disk is held in RAM: just the WADs plus a little, for 16 MB machines
# (it mounts here, though other systems may grumble at so few clusters).
IMG_SPARE_MB ?= 2
IMG_MIN_MB   ?= 1
FBDEFS   := -DFB_W=640 -DFB_H=480 -DFB_BPP=8
ARCHFLAGS := -fno-pic -fno-pie -march=i386 -mtune=i486 -mfpmath=387 -mno-sse -fcf-protection=none
else
$(error ARCH must be x86_64 or i386)
endif

LIBGCC   := $(shell $(CC) $(MFLAG) -print-libgcc-file-name)
BASE     := $(MFLAG) $(ARCHFLAGS) $(FBDEFS) -O2 -fno-stack-protector -fno-asynchronous-unwind-tables \
            -fno-exceptions -fno-math-errno -fno-builtin-printf -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DNDEBUG
CXXFLAGS := $(BASE) -std=gnu++17 -fno-rtti -fno-threadsafe-statics -I. -I$(DOOM) -Wno-write-strings
# Doom: 320x200, 8-bit (we scale and convert it), sound effects on. ctype
# as functions (libc.cpp), not glibc's tables.
DOOMDEFS := -DBAREMETAL -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND -DNORMALUNIX -DLINUX -D__NO_CTYPE
CFLAGS   := $(BASE) -std=gnu99 $(DOOMDEFS) -Ioverrides -I$(DOOM) -I. -w
# Interrupt-time code (and what it calls) never touches SSE or x87
# registers: the entry stubs save only the general-purpose ones.
IRQFLAGS := $(filter-out -msse2 -mfpmath=387,$(CXXFLAGS)) -mgeneral-regs-only
LDFLAGS  := $(LDEMU) -nostdlib -static -no-pie -T linker.ld -z max-page-size=0x1000 --no-warn-rwx-segments

KERNEL := doom$(SUFFIX).elf
ISO    := doom$(SUFFIX).iso
DISK   := doom$(SUFFIX).img

DOOM_SRC := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net \
            f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound \
            i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc \
            m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats \
            p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw \
            r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables \
            v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric \
            mus2mid
PORT_C   := doomgeneric_baremetal i_sound_baremetal
PLATFORM := kernel libc runtime pci $(USB_OBJ) fat32 storage music
# The General MIDI synth on two OPL3s, from SBPRO (github.com/jasonbrianhall/sbpro2hda)
OPL_OBJS := opl/opl opl/dbopl opl/gmsynth opl/gmtables
IRQ_OBJS := irq mixer audio

OBJS := $(BUILD)/boot.o $(BUILD)/setjmp.o \
        $(addprefix $(BUILD)/,$(addsuffix .o,$(PLATFORM) $(IRQ_OBJS) $(PORT_C) $(OPL_OBJS))) \
        $(addprefix $(BUILD)/doom/,$(addsuffix .o,$(DOOM_SRC)))

HDRS := $(wildcard *.hpp) bm.h bm_audio.h music.h font.h $(wildcard opl/*.h)

all: $(KERNEL)

$(BUILD)/boot.o: $(BOOT_S)
	@mkdir -p $(BUILD)
	$(CC) $(MFLAG) $(FBDEFS) $(BOOTDEFS) $(if $(filter i386,$(ARCH)),-march=i386 -DCPU_386) -c $< -o $@

$(BUILD)/setjmp.o: setjmp.S
	@mkdir -p $(BUILD)
	$(CC) $(MFLAG) -c $< -o $@

$(addprefix $(BUILD)/,$(addsuffix .o,$(IRQ_OBJS))): $(BUILD)/%.o: %.cpp $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(IRQFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/doom/%.o: $(DOOM)/%.c $(wildcard $(DOOM)/*.h)
	@mkdir -p $(BUILD)/doom
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(ARCH),x86_64)
# Link as ELF64, then wrap as ELF32 so QEMU's -kernel and GRUB's multiboot
# loader both accept it. The 32-bit entry stub switches to long mode.
$(KERNEL): $(OBJS) linker.ld
	ld $(LDFLAGS) $(OBJS) $(LIBGCC) -o $(BUILD)/doom64.elf
	objcopy -O elf32-i386 $(BUILD)/doom64.elf $@
else
$(KERNEL): $(OBJS) linker.ld
	ld $(LDFLAGS) $(OBJS) $(LIBGCC) -o $@
endif

# ---------------------------------------------------------------- the disk
# 512-byte clusters: little waste on small saved games.
$(DISK): $(WAD) Makefile
	@test -n "$(WAD)" || { echo "No WAD found: make WAD=path/to/doom.wad (or put one here)"; exit 1; }
	@for w in $(WAD); do test -f "$$w" || { echo "$$w: not found"; exit 1; }; done
	rm -f $@
	mb=$$(( ( $$(cat $(WAD) | wc -c) >> 20 ) + 1 + $(IMG_SPARE_MB) )); [ $$mb -ge $(IMG_MIN_MB) ] || mb=$(IMG_MIN_MB); \
	$(MKFS_FAT) -C -F 32 -S 512 -s 1 -n DOOM $@ $$(( mb * 1024 )) 2>&1 | grep -v "less then suggested\|^mkfs.fat" || true
	for w in $(WAD); do mcopy -i $@ "$$w" "::/$$(basename "$$w" | tr A-Z a-z)"; done
	@mdir -i $@ ::/ | grep -i wad

# ---------------------------------------------------------------- CD / USB
$(BUILD)/grub.cfg: grub.cfg
	@mkdir -p $(BUILD)
	sed 's/@GFXMODE@/$(GFXMODE)/; s|multiboot /boot/doom.elf|multiboot /boot/doom.elf $(ARGS)|' $< > $@

iso: $(ISO)
$(ISO): $(KERNEL) $(BUILD)/grub.cfg $(DISK)
	@rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/doom.elf
	gzip -9 -n -c $(DISK) > $(BUILD)/iso/boot/doom.img.gz
	cp $(BUILD)/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso 2>/dev/null

# ---------------------------------------------------------------- UEFI
# doom.efi = a small UEFI loader (gnu-efi, efi/loader.c) with the kernel
# embedded. x86_64 only.
EFI_INC  ?= $(firstword $(wildcard /usr/include/efi))
EFI_CRT0 ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'crt0-efi-x86_64.o' -o -path '*gnuefi*x64*crt0.o' \) 2>/dev/null))
EFI_LDS  ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'elf_x86_64_efi.lds' -o -name 'elf_x64_efi.lds' \) 2>/dev/null))
EFI_LIBS ?= $(shell find /usr/lib /usr/lib64 \( -name libgnuefi.a -o -name libefi.a \) 2>/dev/null | sort -u)
EFI_BUILD := build/efi
EFI_CFLAGS := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(EFI_BUILD) -fpic -ffreestanding -fno-stack-protector \
              -fno-stack-check -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -O2 -Wall

efi: doom.efi doom.img
FORCE:

ifeq ($(ARCH),x86_64)
# The kernel again, position-independent, built in its own directory.
$(EFI_BUILD)/kernel.elf: FORCE
	$(MAKE) --no-print-directory ARCH=x86_64 PIE=1 BUILD=$(EFI_BUILD)/k $(EFI_BUILD)/k/kernel-pie.elf
	cp $(EFI_BUILD)/k/kernel-pie.elf $@

$(BUILD)/kernel-pie.elf: $(OBJS) efi/kernel.ld
	ld -nostdlib -pie --no-dynamic-linker -z notext -z max-page-size=0x1000 --no-warn-rwx-segments \
	   -T efi/kernel.ld $(OBJS) $(LIBGCC) -o $@

$(EFI_BUILD)/kernel.bin $(EFI_BUILD)/kernel_layout.h: $(EFI_BUILD)/kernel.elf
	objcopy -O binary $< $(EFI_BUILD)/kernel.bin
	@nm $< | awk '/ __kernel_end$$/ {print "#define KERNEL_MEM_SIZE 0x" $$1} \
	              / efi_entry64$$/  {print "#define KERNEL_ENTRY 0x" $$1} \
	              / __rela_start$$/ {print "#define KERNEL_RELA_START 0x" $$1} \
	              / __rela_end$$/   {print "#define KERNEL_RELA_END 0x" $$1}' > $(EFI_BUILD)/kernel_layout.h
	@test $$(grep -c define $(EFI_BUILD)/kernel_layout.h) = 4 || { echo "kernel layout symbols missing"; exit 1; }

doom.efi: efi/loader.c efi/image.S $(EFI_BUILD)/kernel.bin $(EFI_BUILD)/kernel_layout.h
	@test -n "$(EFI_CRT0)" -a -n "$(EFI_LDS)" || { echo "gnu-efi not found (Debian: gnu-efi, Fedora: gnu-efi-devel)"; exit 1; }
	gcc $(EFI_CFLAGS) -c efi/loader.c -o $(EFI_BUILD)/loader.o
	gcc -c -DKERNEL_BIN='"$(EFI_BUILD)/kernel.bin"' efi/image.S -o $(EFI_BUILD)/image.o
	ld -nostdlib -znocombreloc -shared -Bsymbolic -T $(EFI_LDS) $(EFI_CRT0) \
	   $(EFI_BUILD)/loader.o $(EFI_BUILD)/image.o $(EFI_LIBS) $(EFI_LIBS) -o $(EFI_BUILD)/doom.so
	objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel -j .rela \
	        -j '.rel.*' -j '.rela.*' -j .reloc -O efi-app-x86_64 --subsystem=10 $(EFI_BUILD)/doom.so $@

# Secure Boot: sign with your own key (enrolled in db, or as a MOK for shim).
sign: doom.efi
	@test -n "$(KEY)" -a -n "$(CERT)" || { echo "usage: make sign KEY=db.key CERT=db.crt"; exit 1; }
	sbsign --key $(KEY) --cert $(CERT) --output doom-signed.efi doom.efi
else
doom.efi run-efi sign:
	@echo "UEFI is x86_64 only; build it without ARCH=i386"; exit 1
.PHONY: doom.efi
endif

# ---------------------------------------------------------------- QEMU
# SOUND=hda (default for x86_64), ac97, sb (default for i386), or none.
AUDIODEV ?= $(if $(filter Darwin,$(shell uname)),coreaudio,pa)
comma := ,
QAUDIO_OPTS ?= timer-period=2000,out.buffer-length=10000$(if $(filter pa,$(AUDIODEV)),$(comma)out.latency=5000)
QSOUND_ac97 := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -device AC97,audiodev=snd0
QSOUND_hda  := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -device intel-hda -device hda-output,audiodev=snd0
QSOUND_sb   := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -device sb16,audiodev=snd0
QSOUND_none :=
SOUND    ?= $(if $(filter i386,$(ARCH)),sb,hda)
KVM      ?= $(if $(wildcard /dev/kvm),-enable-kvm,)
# QEMU's GTK window stops redrawing while its Sound Blaster 16 plays (the
# ISA DMA keeps its main loop busy); the SDL window doesn't. QDISPLAY= for
# QEMU's default.
QDISPLAY ?= $(if $(filter sb,$(SOUND)),-display sdl,)
ifeq ($(ARCH),x86_64)
QEMU     := qemu-system-x86_64 $(KVM) -m 512 -serial stdio $(QDISPLAY) $(QSOUND_$(SOUND))
else
# The oldest CPU QEMU models is a 486.
QEMU     := qemu-system-i386 -cpu 486 -m 24 -serial stdio $(QDISPLAY) $(QSOUND_$(SOUND))
endif

run: $(KERNEL) $(DISK)
	$(QEMU) -kernel $(KERNEL) -initrd $(DISK) -append "audio=$(SOUND) $(ARGS)"

run-iso: $(ISO)
	$(QEMU) -cdrom $(ISO)

ifeq ($(ARCH),x86_64)
# Boot doom.efi under UEFI firmware (OVMF) from a folder QEMU presents as a
# FAT disk. OVMF needs its code image plus a writable copy of the variables.
OVMF ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
          /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/qemu/ovmf-x86_64.bin))
OVMF_VARS ?= $(firstword $(wildcard $(subst CODE,VARS,$(OVMF))))
run-efi: doom.efi doom.img
	@test -n "$(OVMF)" -a -n "$(OVMF_VARS)" || { echo "OVMF firmware not found (Debian: ovmf, Fedora: edk2-ovmf); set OVMF= and OVMF_VARS="; exit 1; }
	@rm -rf $(BUILD)/esp && mkdir -p $(BUILD)/esp/EFI/BOOT
	cp doom.efi $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
	cp doom.img $(BUILD)/esp/EFI/BOOT/
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	        -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd -drive format=raw,file=fat:rw:$(BUILD)/esp
endif

clean:
	rm -rf build doom.elf doom.iso doom-i386.elf doom-i386.iso doom.efi doom-signed.efi doom.img doom-i386.img

.PHONY: all iso efi sign run run-iso run-efi clean FORCE
