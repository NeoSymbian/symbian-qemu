# SPDX-License-Identifier: Apache-2.0
# symbian-qemu: the vendored QEMU (qemu/, with the Syborg board) and run-syborg, for Linux hosts.
# Build from the repository root: docker build -f container/Dockerfile -t symbian-qemu .
# qemu-system-arm boots the Symbian ROM on -M syborg; qemu-system-aarch64 serves the kernel's virt runs.

FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git ninja-build pkg-config python3 python3-venv \
        libglib2.0-dev libpixman-1-dev libfdt-dev zlib1g-dev nettle-dev \
    && rm -rf /var/lib/apt/lists/*
COPY qemu /src/qemu
RUN mkdir /src/build && cd /src/build \
    && ../qemu/configure --prefix=/opt/symbian-qemu --target-list=arm-softmmu,aarch64-softmmu \
        --disable-docs --disable-tools --disable-gtk --disable-sdl --disable-opengl --enable-vnc \
        --enable-nettle \
        --disable-guest-agent \
    && ninja \
    && ninja tests/qtest/syborg-test \
    && QTEST_QEMU_BINARY=./qemu-system-arm tests/qtest/syborg-test \
    && ninja install \
    && rm -rf /opt/symbian-qemu/share/qemu/*.fd /opt/symbian-qemu/share/qemu/edk2-*

FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libglib2.0-0t64 libpixman-1-0 libfdt1 zlib1g libnettle8t64 libhogweed6t64 python3 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/symbian-qemu /opt/symbian-qemu
COPY run-syborg /usr/local/bin/run-syborg
ENV PATH=/opt/symbian-qemu/bin:$PATH
# nettle gives VNC its DES (password auth, run-syborg VNC_PASSWORD)
RUN qemu-system-arm -M help | grep -q '^syborg ' && qemu-system-aarch64 --version && run-syborg --self-test \
    && ldd /opt/symbian-qemu/bin/qemu-system-arm | grep -q libnettle
WORKDIR /work
