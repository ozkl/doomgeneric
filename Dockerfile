# Dockerized PlayStation 2 build environment for the doomgeneric PS2 port.
#
# The upstream ps2dev image ships the full PS2 cross-toolchain
# (mips64r5900el-ps2-elf-gcc, ps2sdk, gsKit, SDL2, audsrv) and all the env
# vars (PS2DEV, PS2SDK, GSKIT, PATH) preconfigured -- but it's a minimal Alpine
# image without `make`. We add just enough to drive the toolchain.
#
# Build the port with ./build.sh (see that script / README), which mounts the
# repo and runs `make` in ps2/.
FROM ghcr.io/ps2dev/ps2dev:latest

RUN apk add --no-cache make bash

WORKDIR /work/ps2
