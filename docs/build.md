# Build
This guide explains how to build the DOOM executable for the MIPS based router.
First try to run the [pre-built DOOM executables](https://github.com/antonKirpich/doom-on-router/releases/latest); if they do not work, follow the instructions below to build your own version.

## Automatic build for MIPS arch without local dependencies
This way does not require installing any local dependencies other than docker, but it is tied to a specific version of the cross-compiler. Therefore, if your router has a different version of OpenWrt or other architecture you'll most likely need to [build manually](#manual-build).
- Install Docker Desktop
- Clone this repository
- Open folder in VS Code IDE(recommended). Agree to open project in a container in a pop-up window. If you didn't see this pop-up, press `Ctrl+Shift+P` and select `Reopen in Container`. Wait a moment while the container is being built
- Press `Ctrl+Shift+B` and select `Rebuild`
- The executable file is ready: `doomgeneric/build/doomgeneric`

## Manual build
- Clone this repository

- Download the SDK for your OpenWrt version:
  - Choose your OpenWrt version [here](https://archive.openwrt.org/releases/)
  - Then open `targets -> ramips`
  - Found your SoC (as example: MT76x8 for TP-Link TL-WR840N v6)
  - At the bottom of the page download `openwrt-sdk-*` archive
- Unzip SDK:
  ```bash
  tar -xf openwrt-sdk-*.tar.xz
  ```
- cd to the directory `/openwrt-sdk-*/staging_dir`, inside you'll see a folder `toolchain-*` - this is a cross-compiler
- Add cross-compiler to the PATH(replace * in the folder names with your actual names):
  ```bash
  export PATH=$PATH:$(pwd)/openwrt-sdk-*/staging_dir/toolchain-*/bin/
  ```
- Go to the folder `<<this_repo>>/doomgeneric/`, execute commands:
  ```bash
  make clean
  make -C doomgeneric -f Makefile.mips all
  ```
- The executable file is ready: `doomgeneric/build/doomgeneric`
