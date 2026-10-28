# Bare-metal Doom

Boots straight into Doom: no operating system. The Doom source in `../doomgeneric` is compiled unchanged; this directory is the machine underneath it (based on the Felix BASIC bare-metal build).

```
sudo apt install build-essential gcc-multilib qemu-system-x86 grub-pc-bin grub-efi-amd64-bin grub-common mtools dosfstools xorriso gnu-efi
make iso WAD=path/to/doom.wad     # doom.iso: CD or USB stick (dd it), BIOS and UEFI
make run WAD=path/to/doom.wad     # QEMU
make efi WAD=path/to/doom.wad     # doom.efi + doom.img for an EFI system partition
make ARCH=i386 iso                # 386/486 build (needs a 387 or 486DX, 16 MB)
```

Without `WAD=`, it uses a `*.wad` here or one from `/usr/share/games/doom` (`doom-wad-shareware`, `freedoom`). Several WADs: `WAD="doom2.wad mymod.wad" ARGS="-file mymod.wad"`.

The WAD goes on `doom.img`, a FAT32 disk image that GRUB (or `doom.efi`) loads into memory and the kernel mounts as a RAM disk. The config and saved games are written there too, so they last until the machine restarts.

Booting from an existing GRUB on Linux (UEFI): copy `doom.efi` and `doom.img` to `/boot/efi/EFI/doom/` and add to `/etc/grub.d/40_custom`:

```
menuentry "Doom" {
    insmod part_gpt
    insmod fat
    insmod chain
    search --no-floppy --file --set=root /EFI/doom/doom.efi
    chainloader /EFI/doom/doom.efi
}
```

**Hardware:** keyboard and mouse (PS/2 or USB/xHCI), any VESA/GOP framebuffer, sound effects and music on HD Audio, AC'97 or Sound Blaster. Music is General MIDI on two emulated OPL3s, from [SBPRO](https://github.com/jasonbrianhall/sbpro2hda). Secure Boot needs `make sign`.

**Sound card:** F4 (Options → Sound Volume), then the CARD line: Left/Right/Enter step through the cards found at boot, OFF included. The choice lasts until restart; `audio=` picks the one to start with.

**Controls:** WASD move and strafe (the arrows move and turn too), Ctrl or the left button fires, Space opens doors, Alt strafes, Shift runs. Keys are set in `default.cfg` (`key_up`, `key_strafeleft`, ... as DOS scan codes). Ctrl+Alt+Del restarts. The mouse looks around Quake style (free look, with a crosshair and no autoaim); `mouse_freelook 0` in the config file brings back mouse-forward and autoaim, `mouse_invert 1` inverts it.

**Video:** 320x200 by default, scaled to fit. `-widescreen` shows more at the sides (16:9, fills a widescreen monitor) and `-hires 2` or `-hires 3` renders at 2x or 3x; `screen_widescreen` and `screen_scale` in `default.cfg` do the same.

**Boot options** (GRUB's `multiboot` line or `ARGS=`): `audio=hda|hdmi|analog|ac97|sb|off`, `sb=220,1`, `latency=MS`, `usb=off`. Anything else is passed to Doom (`-warp 1 1`, `-skill 4`, `-file x.wad`, `-mb 32`...). Boot messages go to COM1.

`tools/fat32_test.sh [file.wad]` tests the FAT32 driver on the host.
