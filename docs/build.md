# Build
This guide explains how to build the DOOM executable for the MT76x8 platform.

## Automatic build without dependencies
This way is recommended because it does not require installing any local dependencies other than docker. However, if your router has a different version of OpenWrt and you encounter build errors, you can perform the [build manually](#manual-build).
- Install Docker Desktop
- Clone this repository
- Open folder in VS Code IDE(recommended). Agree to open project in a container in a pop-up window. If you didn't see this pop-up, press `Ctrl+Shift+P` and select `Reopen in Container`. Wait a moment while the container is being built
- Press `Ctrl+Shift+B` and select `Rebuild`
- The executable file is ready: `doomgeneric/build/doomgeneric`

## Manual build
- Clone this repository

- Download the SDK for your OpenWrt version:
  - For OpenWrt 21.02: [SDK for MT76x8](https://archive.openwrt.org/releases/21.02.0/targets/ramips/mt76x8/openwrt-sdk-21.02.0-ramips-mt76x8_gcc-8.4.0_musl.Linux-x86_64.tar.xz)
  - For other versions, check [archive.openwrt.org](https://archive.openwrt.org/releases/)
- Unzip SDK:
  ```bash
  tar -xf openwrt-sdk-*.tar.xz
  ```
- Add compiler to the PATH:
  ```bash
  export PATH=$PATH:$(pwd)/openwrt-sdk-*/staging_dir/toolchain-mipsel_24kc_gcc-8.4.0_musl/bin/
  ```
- Check toolchain binaries, both commans should print version info:
  ```bash
  mipsel-openwrt-linux-musl-gcc --version
  mipsel-openwrt-linux-musl-g++ --version
  ```
- Go to the folder `<<this_repo>>/doomgeneric/`, execute commands:
  ```bash
  make clean
  make -C doomgeneric -f Makefile.mt7628 all
  ```
- The executable file is ready: `doomgeneric/build/doomgeneric`