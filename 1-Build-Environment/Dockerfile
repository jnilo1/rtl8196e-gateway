# Dockerfile — Unified Build Environment for the RTL8196E Gateway project
#
# This image provides a complete build environment for both:
#   - Main SoC (RTL8196E/Lexra MIPS) — Linux kernel, rootfs, userdata
#   - Zigbee Radio (Silabs EFR32) — NCP/RCP firmware
#
# Usage:
#   # Build the image (first time ~45 min)
#   docker build -t rtl8196e-gateway-builder .
#
#   # Run interactive shell
#   docker run -it --rm -v $(pwd)/..:/workspace rtl8196e-gateway-builder
#
#   # Build Main SoC images
#   docker run --rm -v $(pwd)/..:/workspace rtl8196e-gateway-builder \
#       /workspace/3-Main-SoC-Realtek-RTL8196E/build_rtl8196e.sh
#
#   # Build Zigbee firmware
#   docker run --rm -v $(pwd)/..:/workspace rtl8196e-gateway-builder \
#       /workspace/2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/build_ncp.sh
#
# J. Nilo - December 2025

FROM ubuntu:22.04

LABEL maintainer="J. Nilo"
LABEL description="Unified build environment for the RTL8196E Gateway project"

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# ============================================================================
# Step 1: Install all system dependencies
# ============================================================================
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y \
        # Build essentials
        build-essential \
        bison \
        flex \
        gawk \
        bc \
        # Crosstool-ng requirements
        help2man \
        texinfo \
        libtool-bin \
        libncurses5-dev \
        # Compression & archives
        unzip \
        wget \
        curl \
        bzip2 \
        lbzip2 \
        xz-utils \
        zlib1g-dev \
        # 32-bit support (legacy tools)
        libc6-i386 \
        zlib1g:i386 \
        # Version control
        git \
        git-lfs \
        # Kernel build
        libssl-dev \
        libelf-dev \
        device-tree-compiler \
        pkg-config \
        # Filesystem tools
        cpio \
        squashfs-tools \
        mtd-utils \
        fakeroot \
        # Python (for various tools)
        python3 \
        python3-pip \
        python3-venv \
        # Networking tools
        tftp-hpa \
        sshpass \
        iproute2 \
        # Other
        sudo \
        rsync \
        nano \
        # Silabs tools dependencies (slc-cli 5.11+ requires Java 21)
        openjdk-21-jre-headless \
        # Commander dependencies
        libgl1 \
        # cpcd build
        cmake \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# ============================================================================
# Step 2: Create non-root user (required for crosstool-ng)
# ============================================================================
RUN useradd -m -s /bin/bash builder && \
    echo 'builder ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

USER builder
WORKDIR /home/builder

# ============================================================================
# Step 3: Install crosstool-ng
# ============================================================================
RUN cd /tmp && \
    wget -q http://crosstool-ng.org/download/crosstool-ng/crosstool-ng-1.28.0.tar.bz2 && \
    tar xjf crosstool-ng-1.28.0.tar.bz2 && \
    cd crosstool-ng-1.28.0 && \
    ./configure --prefix=$HOME/crosstool-ng >/dev/null && \
    make -j$(nproc) >/dev/null && \
    make install >/dev/null && \
    rm -rf /tmp/crosstool-ng*

ENV PATH="/home/builder/crosstool-ng/bin:${PATH}"

# ============================================================================
# Step 4: Build Lexra MIPS toolchain (~30 min)
# ============================================================================
COPY --chown=builder:builder 10-lexra-toolchain/crosstool-ng.config /home/builder/toolchain.config
COPY --chown=builder:builder 10-lexra-toolchain/patches /home/builder/patches

RUN mkdir -p ~/.crosstool-ng/patches && \
    cp -r /home/builder/patches/* ~/.crosstool-ng/patches/

RUN mkdir -p /tmp/toolchain-build && \
    cd /tmp/toolchain-build && \
    cp /home/builder/toolchain.config .config && \
    (ct-ng build || ct-ng build || ct-ng build) && \
    rm -rf /tmp/toolchain-build && \
    rm -rf ~/.crosstool-ng/tarballs && \
    rm -rf ~/.crosstool-ng/src

ENV PATH="/home/builder/x-tools/mips-lexra-linux-musl/bin:${PATH}"

# Verify Lexra toolchain
RUN mips-lexra-linux-musl-gcc --version

# ============================================================================
# Step 5: Install ARM GCC toolchain (for Silabs EFR32)
# ============================================================================
RUN cd /home/builder && \
    wget -q --show-progress -O arm-gcc.tar.xz \
        "https://developer.arm.com/-/media/Files/downloads/gnu/12.2.rel1/binrel/arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi.tar.xz" && \
    tar xf arm-gcc.tar.xz && \
    mv arm-gnu-toolchain-* arm-gnu-toolchain && \
    rm arm-gcc.tar.xz

ENV PATH="/home/builder/arm-gnu-toolchain/bin:${PATH}"

# Verify ARM toolchain
RUN arm-none-eabi-gcc --version

# ============================================================================
# Step 6: Install slc-cli (Silicon Labs CLI)
# ============================================================================
# silabs.com answers 403 to command-line clients (since 2026-10-22). An archive
# saved by hand in 12-silabs-toolchain/downloads/ is used instead of the
# download (see the README there). It is bind-mounted for this step only, so it
# never lands in an image layer.
RUN --mount=type=bind,source=12-silabs-toolchain/downloads,target=/tmp/silabs-downloads \
    cd /home/builder && \
    if [ -f /tmp/silabs-downloads/slc_cli_linux.zip ]; then \
        echo "Using 12-silabs-toolchain/downloads/slc_cli_linux.zip" && \
        cp /tmp/silabs-downloads/slc_cli_linux.zip slc_cli.zip; \
    else \
        wget -q -O slc_cli.zip "https://www.silabs.com/documents/public/software/slc_cli_linux.zip" || { \
            echo "ERROR: could not download slc_cli_linux.zip; silabs.com may refuse command-line downloads."; \
            echo "Open https://www.silabs.com/documents/public/software/slc_cli_linux.zip in a browser,"; \
            echo "save it as 1-Build-Environment/12-silabs-toolchain/downloads/slc_cli_linux.zip, and rebuild."; \
            exit 1; }; \
    fi && \
    unzip -q slc_cli.zip && \
    rm slc_cli.zip && \
    chmod +x slc_cli/slc

ENV PATH="/home/builder/slc_cli:${PATH}"

# Verify slc-cli
RUN slc --version

# ============================================================================
# Step 7: Install Simplicity Commander (for .gbl generation)
# ============================================================================
# Same hand-download fallback as slc-cli above.
RUN --mount=type=bind,source=12-silabs-toolchain/downloads,target=/tmp/silabs-downloads \
    cd /home/builder && \
    if [ -f /tmp/silabs-downloads/SimplicityCommander-Linux.zip ]; then \
        echo "Using 12-silabs-toolchain/downloads/SimplicityCommander-Linux.zip" && \
        cp /tmp/silabs-downloads/SimplicityCommander-Linux.zip commander.zip; \
    else \
        wget -q -O commander.zip "https://www.silabs.com/documents/public/software/SimplicityCommander-Linux.zip" || { \
            echo "ERROR: could not download SimplicityCommander-Linux.zip; silabs.com may refuse command-line downloads."; \
            echo "Open https://www.silabs.com/documents/public/software/SimplicityCommander-Linux.zip in a browser,"; \
            echo "save it as 1-Build-Environment/12-silabs-toolchain/downloads/SimplicityCommander-Linux.zip, and rebuild."; \
            exit 1; }; \
    fi && \
    unzip -q commander.zip && \
    tar xf SimplicityCommander-Linux/Commander_linux_x86_64_*.tar.bz -C . && \
    rm -rf commander.zip SimplicityCommander-Linux

ENV PATH="/home/builder/commander:${PATH}"

# Verify commander
RUN commander --version

# ============================================================================
# Step 8: Install Gecko SDK (from GitHub)
# ============================================================================
# Version fixed for reproducibility (must match install_silabs.sh)
ENV GECKO_SDK_VERSION="4.5.1"

RUN git lfs install && \
    cd /home/builder && \
    echo "Downloading Gecko SDK v${GECKO_SDK_VERSION}..." && \
    git clone --depth 1 --branch "v${GECKO_SDK_VERSION}" --progress \
        https://github.com/SiliconLabs/gecko_sdk.git gecko_sdk && \
    rm -rf gecko_sdk/.git

ENV GECKO_SDK="/home/builder/gecko_sdk"
ENV ARM_GCC_DIR="/home/builder/arm-gnu-toolchain"

# Trust the SDK signature (required by slc-cli 5.11+)
RUN slc signature trust --sdk /home/builder/gecko_sdk || true

# ============================================================================
# Step 9: Build Realtek tools (cvimg, lzma)
# ============================================================================
COPY --chown=builder:builder 11-realtek-tools /home/builder/realtek-tools

# `rm -rf bin` first: .dockerignore should keep the host's generated bin/ out of
# the build context, but a stale entrypoint symlink that slips in would be copied
# as a self-referential loop and break `mkdir -p bin` (#103). Clearing it is safe —
# the directory is always rebuilt from source by the make step just below.
RUN cd /home/builder/realtek-tools && \
    rm -rf bin && \
    mkdir -p bin && \
    cd cvimg && make && cp cvimg ../bin/ && cd .. && \
    cd lzma-4.65/CPP/7zip/Compress/LZMA_Alone && make -f makefile.gcc && cp lzma /home/builder/realtek-tools/bin/
# Note: lzma-loader is built during kernel compilation (requires kernel headers)

ENV PATH="/home/builder/realtek-tools/bin:${PATH}"

# ============================================================================
# Step 10: Setup environment and entrypoint
# ============================================================================
RUN echo '' >> ~/.bashrc && \
    echo '# Lidl Gateway Build Environment' >> ~/.bashrc && \
    echo 'export PATH="/home/builder/x-tools/mips-lexra-linux-musl/bin:$PATH"' >> ~/.bashrc && \
    echo 'export PATH="/home/builder/arm-gnu-toolchain/bin:$PATH"' >> ~/.bashrc && \
    echo 'export PATH="/home/builder/slc_cli:$PATH"' >> ~/.bashrc && \
    echo 'export PATH="/home/builder/commander:$PATH"' >> ~/.bashrc && \
    echo 'export PATH="/home/builder/realtek-tools/bin:$PATH"' >> ~/.bashrc && \
    echo 'export GECKO_SDK="/home/builder/gecko_sdk"' >> ~/.bashrc && \
    echo 'export ARM_GCC_DIR="/home/builder/arm-gnu-toolchain"' >> ~/.bashrc

WORKDIR /workspace

# Entrypoint creates symlinks for consistent paths with native builds:
#   /workspace/x-tools -> /home/builder/x-tools
#   /workspace/1-Build-Environment/11-realtek-tools/bin -> /home/builder/realtek-tools/bin
# These links land in the bind-mounted /workspace and persist on the host. On
# re-entry we first drop any pre-existing *symlink* (which may be stale, dangling,
# or a self-loop left by a previous run) and recreate it fresh — but we never touch
# a real directory, so a native build's own bin/ is preserved (#103).
ENTRYPOINT ["/bin/bash", "-c", "[ -L /workspace/x-tools ] && rm -f /workspace/x-tools; [ ! -e /workspace/x-tools ] && ln -s /home/builder/x-tools /workspace/x-tools 2>/dev/null; RT=/workspace/1-Build-Environment/11-realtek-tools; if [ -d \"$RT\" ]; then [ -L \"$RT/bin\" ] && rm -f \"$RT/bin\"; [ ! -e \"$RT/bin\" ] && ln -s /home/builder/realtek-tools/bin \"$RT/bin\" 2>/dev/null; fi; exec \"$@\"", "--"]
CMD ["/bin/bash"]
