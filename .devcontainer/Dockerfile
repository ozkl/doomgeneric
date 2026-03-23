FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    xz-utils \
    ca-certificates \
    libncurses5 \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

RUN wget https://archive.openwrt.org/releases/21.02.0/targets/ramips/mt76x8/openwrt-sdk-21.02.0-ramips-mt76x8_gcc-8.4.0_musl.Linux-x86_64.tar.xz \
    && tar -xf openwrt-sdk-*.tar.xz \
    && rm openwrt-sdk-*.tar.xz \
    && mv openwrt-sdk-* sdk

ENV SDK_PATH=/opt/sdk
ENV STAGING_DIR=$SDK_PATH/staging_dir
ENV TOOLCHAIN_BIN=$SDK_PATH/staging_dir/toolchain-mipsel_24kc_gcc-8.4.0_musl/bin
ENV PATH="${TOOLCHAIN_BIN}:${PATH}"

WORKDIR /workspace