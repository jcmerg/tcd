#!/bin/bash
# Build and install tcd from source
# Usage: bash build.sh [--swambe2]
#
# Clones/updates tcd and urfd repos, builds tcd, installs binary.
# Run as root or with sudo.

set -e

BUILDDIR="/tmp/tcd-build"
TCD_REPO="https://github.com/jcmerg/tcd.git"
URFD_REPO="https://github.com/jcmerg/urfd.git"
INSTALL_DIR="/usr/local/bin"
SWAMBE2=false

# Parse args
for arg in "$@"; do
    case $arg in
        --swambe2) SWAMBE2=true ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

echo "=== Building tcd ==="
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

# Prepare build
cd "$BUILDDIR/tcd"
cp config/* .

if [ "$SWAMBE2" = true ]; then
    sed -i 's/^swambe2 = false/swambe2 = true/' tcd.mk
    sed -i 's/-W -Werror/-W -fpermissive/' Makefile
fi

# Build
echo "Compiling..."
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
