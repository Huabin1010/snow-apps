# Ubuntu arm64 build environment for the Snow Shot Linux arm64 Debian package.
# Build it on an x64 host with qemu-user-binfmt; scripts/build-linux-arm64-docker.sh
# runs the repository build inside this image.
FROM --platform=linux/arm64 docker.1ms.run/library/ubuntu:26.04

# The base image has no CA bundle yet; apt verifies the archive with its GPG
# keyring, so a plain HTTP mirror is safe for the first package install.
ARG UBUNTU_MIRROR=http://mirrors.ustc.edu.cn/ubuntu-ports
ARG RUST_STABLE=1.98.1
ARG RUST_NIGHTLY=nightly-2026-09-08
ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i "s|http://ports.ubuntu.com/ubuntu-ports/|${UBUNTU_MIRROR}/|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git curl ca-certificates python3 \
        patchelf dpkg-dev file clang-format libssl-dev \
        qt6-base-dev qt6-base-private-dev qt6-svg-dev qt6-tools-dev qt6-wayland-dev \
        qt6-svg-plugins libqt6waylandclient6 \
        qt6-l10n-tools qt6-translations-l10n libgl-dev \
        libavcodec-dev libavformat-dev libavfilter-dev libavutil-dev libswscale-dev \
        libswresample-dev libpipewire-0.3-dev \
        libx11-dev libxtst-dev libx11-xcb-dev libxcb-xfixes0-dev \
        libdbus-1-dev libatspi2.0-dev \
        libopencv-dev libminizip-ng-dev \
        libheif-dev libjxl-dev libgif-dev libtinyxml2-dev libcups2-dev \
        clang libclang-dev \
    && rm -rf /var/lib/apt/lists/*

# Snow Shot requires TurboJPEG 3.1 under the same prefix as the x64 build.
COPY libjpeg-turbo-3.1.1.tar.gz /tmp/
RUN cd /tmp && tar xf libjpeg-turbo-3.1.1.tar.gz \
    && cmake -S libjpeg-turbo-3.1.1 -B jpeg-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/libjpeg-turbo-3.1.1 \
        -DCMAKE_INSTALL_LIBDIR=/opt/libjpeg-turbo-3.1.1/lib \
    && cmake --build jpeg-build \
    && cmake --install jpeg-build \
    && rm -rf /tmp/libjpeg-turbo-3.1.1 /tmp/jpeg-build /tmp/libjpeg-turbo-3.1.1.tar.gz

ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal \
        --default-toolchain "${RUST_STABLE}" --no-modify-path \
    && rustup toolchain install "${RUST_NIGHTLY}" --profile minimal \
    && chmod -R a+rwX /opt/rustup /opt/cargo
