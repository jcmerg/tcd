#!/bin/bash
# Build and install tcd from source
# Usage: bash build.sh [--swambe2]
#
# Clones/updates tcd and urfd repos, builds tcd, installs binary.
# On x86_64 with --swambe2, uses md380_vocoder_dynarmic (ARM JIT via dynarmic).
# On ARM with --swambe2, uses native md380_vocoder.
# Run as root or with sudo.

set -e

BUILDDIR="/tmp/tcd-build"
TCD_REPO="https://github.com/jcmerg/tcd.git"
URFD_REPO="https://github.com/jcmerg/urfd.git"
MD380_DYN_REPO="https://github.com/jcmerg/md380_vocoder_dynarmic.git"
INSTALL_DIR="/usr/local/bin"
SWAMBE2=false
ARCH=$(uname -m)

# Parse args
for arg in "$@"; do
    case $arg in
        --swambe2) SWAMBE2=true ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

echo "=== Building tcd ==="
echo "Architecture: $ARCH"
echo "Software AMBE2 (md380): $SWAMBE2"

# Clone or update repos
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

if [ -d tcd ]; then
    echo "Updating tcd..."
    cd tcd && git checkout -- . && git pull && cd ..
else
    echo "Cloning tcd..."
    git clone --depth 1 "$TCD_REPO"
fi

if [ -d urfd ]; then
    echo "Updating urfd..."
    cd urfd && git checkout -- . && git pull && cd ..
else
    echo "Cloning urfd..."
    git clone --depth 1 "$URFD_REPO"
fi

# Build md380_vocoder_dynarmic for x86_64 if needed
if [ "$SWAMBE2" = true ] && [ "$ARCH" = "x86_64" ]; then
    echo "Building md380_vocoder_dynarmic for x86_64..."

    # Check build dependencies
    for cmd in cmake unzip python3 xxd; do
        if ! command -v $cmd &>/dev/null; then
            echo "ERROR: $cmd is required but not installed"
            echo "  apt install build-essential cmake unzip python3 xxd libboost-dev"
            exit 1
        fi
    done

    if [ -d md380_vocoder_dynarmic ]; then
        cd md380_vocoder_dynarmic && git pull && cd ..
    else
        git clone --depth 1 "$MD380_DYN_REPO"
    fi

    cd md380_vocoder_dynarmic
    mkdir -p build && cd build
    cmake .. && make -j$(nproc)
    sh ../makelib.sh

    # Install library and header
    cp libmd380_vocoder.a /usr/local/lib/
    cp ../md380_vocoder.h /usr/local/include/
    ldconfig
    echo "md380_vocoder_dynarmic installed"
    cd "$BUILDDIR"
fi

# Prepare build
cd "$BUILDDIR/tcd"
cp config/* .

if [ "$SWAMBE2" = true ]; then
    sed -i 's/^swambe2 = false/swambe2 = true/' tcd.mk
    sed -i 's/-W -Werror/-W -fpermissive/' Makefile
fi

# Build
echo "Compiling tcd..."
make clean
make -j$(nproc)

# Install
echo "Installing..."
if systemctl is-active --quiet tcd 2>/dev/null; then
    systemctl stop tcd
    cp tcd "$INSTALL_DIR/tcd"
    systemctl start tcd
    echo "=== tcd restarted ==="
else
    cp tcd "$INSTALL_DIR/tcd"
    echo "=== tcd installed (not running as service) ==="
fi

echo "=== Build complete ==="
