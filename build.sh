#!/bin/bash
# Build and install tcd from source
# Automatically installs all dependencies, builds and deploys.
# Run as root or with sudo.
#
# Dependencies (auto-installed):
#   - libftd2xx (FTDI driver) — from ftdichip.com
#   - libimbe_vocoder — from jcmerg/imbe_vocoder
#   - libmd380_vocoder — ARM native (jcmerg/md380_vocoder)
#                        or x86_64 via dynarmic (jcmerg/md380_vocoder_dynarmic)
#   - urfd (for shared TCPacketDef.h/TCSocket.h)

set -e

BUILDDIR="/tmp/tcd-build"
TCD_REPO="https://github.com/jcmerg/tcd.git"
URFD_REPO="https://github.com/jcmerg/urfd.git"
IMBE_REPO="https://github.com/jcmerg/imbe_vocoder.git"
MD380_ARM_REPO="https://github.com/jcmerg/md380_vocoder.git"
MD380_X64_REPO="https://github.com/jcmerg/md380_vocoder_dynarmic.git"
FTDI_URL_X64="https://ftdichip.com/wp-content/uploads/2024/02/libftd2xx-x86_64-1.4.34.tgz"
FTDI_URL_ARM="https://ftdichip.com/wp-content/uploads/2024/02/libftd2xx-arm-v7-hf-1.4.34.tgz"
INSTALL_DIR="/usr/local/bin"
ARCH=$(uname -m)

echo "=== Building tcd ==="
echo "Architecture: $ARCH"

# Check build tools
for cmd in g++ make git; do
    if ! command -v $cmd &>/dev/null; then
        echo "Installing build tools..."
        apt-get update && apt-get install -y build-essential git
        break
    fi
done

mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# ---------------------------------------------------------------
# 1. libftd2xx (FTDI USB driver)
# ---------------------------------------------------------------
if [ ! -f /usr/local/include/ftd2xx.h ]; then
    echo "Installing libftd2xx..."
    if [ "$ARCH" = "x86_64" ]; then
        FTDI_URL="$FTDI_URL_X64"
    elif [ "$ARCH" = "armv7l" ] || [ "$ARCH" = "armv6l" ] || [ "$ARCH" = "aarch64" ]; then
        FTDI_URL="$FTDI_URL_ARM"
    else
        echo "ERROR: Unsupported architecture $ARCH for libftd2xx"
        exit 1
    fi
    curl -sL "$FTDI_URL" -o ftdi.tgz
    tar xzf ftdi.tgz
    cd release/build
    cp libftd2xx.* /usr/local/lib/
    cd ..
    cp ftd2xx.h WinTypes.h /usr/local/include/
    ldconfig
    cd "$BUILDDIR"
    rm -rf release ftdi.tgz
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
# 3. libmd380_vocoder (DMR/YSF software codec)
# ---------------------------------------------------------------
if [ ! -f /usr/local/lib/libmd380_vocoder.a ]; then
    echo "Building md380_vocoder..."
    if [ "$ARCH" = "x86_64" ]; then
        # x86_64: use dynarmic (ARM JIT emulation)
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
make -j$(nproc)

# ---------------------------------------------------------------
# 6. Install
# ---------------------------------------------------------------
echo "Installing..."
if systemctl is-active --quiet tcd 2>/dev/null; then
    systemctl stop tcd
    cp tcd "$INSTALL_DIR/tcd"
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
    cp tcd "$INSTALL_DIR/tcd"
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
