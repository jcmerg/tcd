#!/bin/bash
# Build and install tcd from source
# Automatically installs all dependencies, builds and deploys.
# Run as root or with sudo.
#
# Usage:
#   sudo bash build.sh               # build without md380 (requires ≥2 AMBE devices)
#   sudo bash build.sh --with-md380  # build with md380 software vocoder
#
# Dependencies (auto-installed):
#   - libftd2xx (FTDI driver) — from ftdichip.com
#   - libimbe_vocoder — from jcmerg/imbe_vocoder
#   - libmd380_vocoder (only with --with-md380) — armhf native (jcmerg/md380_vocoder)
#                        or x86_64/aarch64 via dynarmic (jcmerg/md380_vocoder_dynarmic)
#   - urfd (for shared TCPacketDef.h/TCSocket.h)

set -e

# Parse arguments
WITH_MD380=false
for arg in "$@"; do
    case "$arg" in
        --with-md380) WITH_MD380=true ;;
        *) echo "Unknown argument: $arg"; echo "Usage: $0 [--with-md380]"; exit 1 ;;
    esac
done

BUILDDIR="/tmp/tcd-build"
TCD_REPO="https://github.com/jcmerg/tcd.git"
URFD_REPO="https://github.com/jcmerg/urfd.git"
IMBE_REPO="https://github.com/jcmerg/imbe_vocoder.git"
MD380_ARM_REPO="https://github.com/jcmerg/md380_vocoder.git"
MD380_X64_REPO="https://github.com/jcmerg/md380_vocoder_dynarmic.git"
FTDI_URL_X64="https://ftdichip.com/wp-content/uploads/2025/03/libftd2xx-linux-x86_64-1.4.33.tgz"
FTDI_URL_ARM32="https://ftdichip.com/wp-content/uploads/2025/03/libftd2xx-linux-arm-v7-hf-1.4.33.tgz"
FTDI_URL_ARM64="https://ftdichip.com/wp-content/uploads/2025/03/libftd2xx-linux-arm-v8-1.4.33.tgz"
INSTALL_DIR="/usr/local/bin"

# Detect actual toolchain target, not kernel arch (handles armhf-on-aarch64)
CC_ARCH=$(g++ -dumpmachine 2>/dev/null || true)
case "$CC_ARCH" in
    x86_64*)          ARCH="x86_64" ;;
    aarch64*|arm64*)  ARCH="aarch64" ;;
    arm*hf*|arm*eabi*) ARCH="armhf" ;;
    *)                ARCH=$(uname -m) ;;   # fallback
esac

echo "=== Building tcd ==="
echo "Architecture: $ARCH (compiler: $CC_ARCH)"
echo "MD380 software vocoder: $WITH_MD380"

# Check build tools
for cmd in g++ make git; do
    if ! command -v $cmd &>/dev/null; then
        echo "Installing build tools..."
        apt-get update && apt-get install -y build-essential git libncurses-dev
        break
    fi
done

# Ensure ncurses is available (for tcdmon)
if ! dpkg -l libncurses-dev 2>/dev/null | grep -q '^ii'; then
    echo "Installing libncurses-dev..."
    apt-get update && apt-get install -y libncurses-dev
fi

mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# ---------------------------------------------------------------
# 1. libftd2xx (FTDI USB driver)
# ---------------------------------------------------------------
if [ ! -f /usr/local/include/ftd2xx.h ]; then
    echo "Installing libftd2xx..."
    if [ "$ARCH" = "x86_64" ]; then
        FTDI_URL="$FTDI_URL_X64"
    elif [ "$ARCH" = "aarch64" ]; then
        FTDI_URL="$FTDI_URL_ARM64"
    elif [ "$ARCH" = "armhf" ]; then
        FTDI_URL="$FTDI_URL_ARM32"
    else
        echo "ERROR: Unsupported architecture $ARCH for libftd2xx"
        exit 1
    fi
    curl -sL "$FTDI_URL" -o ftdi.tgz
    tar xzf ftdi.tgz
    # Find the extracted directory (varies by version: release/, linux-x86_64/, etc.)
    FTDI_DIR=$(tar tzf ftdi.tgz | head -1 | cut -d/ -f1)
    cp "$FTDI_DIR"/libftd2xx.so /usr/local/lib/
    # Symlink versioned name instead of duplicating the binary
    FTDI_VER=$(ls "$FTDI_DIR"/libftd2xx.so.* 2>/dev/null | head -1 | xargs basename)
    if [ -n "$FTDI_VER" ]; then
        ln -sf libftd2xx.so /usr/local/lib/"$FTDI_VER"
    fi
    cp "$FTDI_DIR"/ftd2xx.h "$FTDI_DIR"/WinTypes.h /usr/local/include/
    ldconfig
    rm -rf "$FTDI_DIR" ftdi.tgz
    echo "libftd2xx installed"
else
    echo "libftd2xx: already installed"
fi

# ---------------------------------------------------------------
# 2. libimbe_vocoder (P25 IMBE codec)
# ---------------------------------------------------------------
if [ ! -f /usr/local/lib/libimbe_vocoder.a ]; then
    echo "Building imbe_vocoder..."
    if [ -d imbe_vocoder ]; then
        cd imbe_vocoder && git pull && cd ..
    else
        git clone --depth 1 "$IMBE_REPO" imbe_vocoder
    fi
    cd imbe_vocoder
    make clean && make -j$(nproc)
    cp libimbe_vocoder.a /usr/local/lib/
    cp imbe_vocoder_api.h /usr/local/include/
    cd "$BUILDDIR"
    echo "imbe_vocoder installed"
else
    echo "imbe_vocoder: already installed"
fi

# ---------------------------------------------------------------
# 3. libmd380_vocoder (DMR/YSF software codec) — only with --with-md380
# ---------------------------------------------------------------
if [ "$WITH_MD380" = "true" ]; then
    if [ ! -f /usr/local/lib/libmd380_vocoder.a ]; then
        echo "Building md380_vocoder..."
        if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "aarch64" ]; then
            # x86_64/aarch64: use dynarmic (ARM Cortex-M JIT emulation)
            for cmd in cmake unzip python3 xxd; do
                if ! command -v $cmd &>/dev/null; then
                    echo "Installing cmake and tools..."
                    apt-get update && apt-get install -y cmake unzip python3 xxd libboost-dev
                    break
                fi
            done

            if [ -d md380_vocoder_dynarmic ]; then
                cd md380_vocoder_dynarmic && git pull && cd ..
            else
                git clone --depth 1 "$MD380_X64_REPO" md380_vocoder_dynarmic
            fi
            cd md380_vocoder_dynarmic
            mkdir -p build && cd build
            cmake .. && make -j$(nproc)
            sh ../makelib.sh
            cp libmd380_vocoder.a /usr/local/lib/
            cp ../md380_vocoder.h /usr/local/include/
            cd "$BUILDDIR"
        else
            # ARM: native md380 firmware
            if [ -d md380_vocoder ]; then
                cd md380_vocoder && git pull && cd ..
            else
                git clone --depth 1 "$MD380_ARM_REPO" md380_vocoder
            fi
            cd md380_vocoder
            make clean && make -j$(nproc)
            cp libmd380_vocoder.a /usr/local/lib/
            cp md380_vocoder.h /usr/local/include/
            cd "$BUILDDIR"
        fi
        ldconfig
        echo "md380_vocoder installed"
    else
        echo "md380_vocoder: already installed"
    fi
else
    echo "md380_vocoder: skipped (use --with-md380 to enable)"
fi

# ---------------------------------------------------------------
# 4. Clone/update source repos
# ---------------------------------------------------------------
if [ -d tcd/.git ]; then
    echo "Updating tcd..."
    cd tcd && git checkout -- . && git pull && cd ..
else
    rm -rf tcd
    echo "Cloning tcd..."
    git clone --depth 1 "$TCD_REPO"
fi

if [ -d urfd/.git ]; then
    echo "Updating urfd..."
    cd urfd && git checkout -- . && git pull && cd ..
else
    rm -rf urfd
    echo "Cloning urfd..."
    git clone --depth 1 "$URFD_REPO"
fi

# ---------------------------------------------------------------
# 5. Build tcd
# ---------------------------------------------------------------
cd "$BUILDDIR/tcd"
cp config/* .

echo "Compiling tcd..."
make clean
if [ "$WITH_MD380" = "true" ]; then
    make -j$(nproc) md380=true
else
    make -j$(nproc)
fi

# ---------------------------------------------------------------
# 6. Install
# ---------------------------------------------------------------
echo "Installing..."
if systemctl is-active --quiet tcd 2>/dev/null; then
    systemctl stop tcd
    cp tcd tcdmon "$INSTALL_DIR/"
    cp tools/agc-analyze.py "$INSTALL_DIR/agc-analyze"
    chmod +x "$INSTALL_DIR/agc-analyze"
    if [ ! -f /usr/local/etc/tcd.ini ]; then
        cp config/tcd.ini /usr/local/etc/tcd.ini
        echo "Default tcd.ini installed — edit /usr/local/etc/tcd.ini before starting!"
    fi
    if [ ! -f /etc/systemd/system/tcd.service ]; then
        cp config/tcd.service /etc/systemd/system/
        systemctl daemon-reload
        systemctl enable tcd
    fi
    systemctl start tcd
    echo "=== tcd restarted ==="
else
    cp tcd tcdmon "$INSTALL_DIR/"
    cp tools/agc-analyze.py "$INSTALL_DIR/agc-analyze"
    chmod +x "$INSTALL_DIR/agc-analyze"
    if [ ! -f /usr/local/etc/tcd.ini ]; then
        mkdir -p /usr/local/etc
        cp config/tcd.ini /usr/local/etc/tcd.ini
        echo "Default tcd.ini installed — edit /usr/local/etc/tcd.ini before starting!"
    fi
    if [ ! -f /etc/systemd/system/tcd.service ]; then
        cp config/tcd.service /etc/systemd/system/
        systemctl daemon-reload
        systemctl enable tcd
    fi
    echo "=== tcd installed (start with: systemctl start tcd) ==="
fi

echo "=== Build complete ==="
