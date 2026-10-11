#!/bin/bash
# R6-Bionic QEMU Build Script
# Builds qemu-eos with Canon EOS R6 support
#
# Usage: ./scripts/build.sh [--clean] [--run]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PATCHES_DIR="$PROJECT_DIR/patches"
BUILD_DIR="$PROJECT_DIR/build"
QEMU_DIR="$PROJECT_DIR/qemu"

# Check for required tools
for cmd in git ninja meson python3; do
    if ! command -v $cmd &> /dev/null; then
        echo "ERROR: $cmd not found. Install build dependencies first."
        echo "  sudo apt install git ninja-build meson python3 python3-pyelftools libglib2.0-dev libpixman-1-dev"
        exit 1
    fi
done

# Step 1: Clone qemu-eos if not present
if [ ! -d "$QEMU_DIR" ]; then
    echo "=== Cloning qemu-eos ==="
    git clone --depth=50 https://github.com/reticulatedpines/qemu-eos.git "$QEMU_DIR"
fi

# Step 2: Fix CRLF if on Windows/WSL
if [ "$(uname)" = "Linux" ]; then
    if grep -q $'\r' "$QEMU_DIR/configure" 2>/dev/null; then
        echo "=== Fixing CRLF line endings ==="
        find "$QEMU_DIR" -name '*.sh' -exec sed -i 's/\r$//' {} \;
        sed -i 's/\r$//' "$QEMU_DIR/configure"
        find "$QEMU_DIR" -name 'meson.build' -exec sed -i 's/\r$//' {} \;
    fi
fi

# Step 3: Apply R6 patch
echo "=== Applying R6 patches ==="
cd "$QEMU_DIR"

# Apply Kconfig + meson.build patch
git apply --whitespace=nowarn "$PATCHES_DIR/qemu-r6.patch" || {
    # Try patch command as fallback
    patch -p1 < "$PATCHES_DIR/qemu-r6.patch"
}

# Copy r6.c
cp "$PATCHES_DIR/r6.c" hw/arm/r6.c
chmod +x hw/arm/r6.c

# Step 4: Build
if [ "$1" = "--clean" ]; then
    echo "=== Cleaning build directory ==="
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Configuring QEMU ==="
"$QEMU_DIR/configure" \
    --target-list=arm-softmmu \
    --disable-werror \
    --prefix="$PROJECT_DIR/install"

echo "=== Building QEMU ($(nproc) cores) ==="
ninja -j$(nproc)

echo ""
echo "=== BUILD COMPLETE ==="
echo ""
echo "QEMU binary: $BUILD_DIR/qemu-system-arm"
echo ""
echo "To run the R6 emulator:"
echo "  $BUILD_DIR/qemu-system-arm \\"
echo "    -machine canon-r6 \\"
echo "    -cpu cortex-a9 \\"
echo "    -m 1G \\"
echo "    -nographic \\"
echo "    -s -S"
echo ""
echo "Place your ROM dumps in the current directory:"
echo "  gang100.bin (64 MB) - primary ROM"
echo "  boot00.bin (36.75 MB) - bootable firmware"
echo ""
echo "Connect GDB:"
echo "  arm-none-eabi-gdb"
echo "  (gdb) target remote localhost:1234"
echo "  (gdb) break *0xE01000AE"
echo "  (gdb) continue"
