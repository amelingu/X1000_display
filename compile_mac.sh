#!/usr/bin/env bash
# compile_mac.sh — Build X1000_display.xpl for macOS (universal binary)
#
# *** Must be run ON a Mac — clang++ is not available on Linux ***
#
# Steps to build on Mac:
#   1. Copy the X1000_display/ project folder to the Mac
#      (USB stick, scp, or shared folder)
#   2. Install Xcode command line tools if not already present:
#        xcode-select --install
#   3. Make the script executable and run:
#        chmod +x compile_mac.sh
#        ./compile_mac.sh
#   4. To build AND install directly into X-Plane 12:
#        ./compile_mac.sh install
#      (Looks for X-Plane 12 in ~/X-Plane 12 by default.
#       Override with: XP12="/path/to/X-Plane 12" ./compile_mac.sh install)
#
# Output:
#   build/mac_fat/X1000_display.xpl  — universal binary (x86_64 + arm64)
#   Installed to: <XP12>/Resources/plugins/X1000_display/mac_x64/
#
# Note: cross-compiling from Linux requires osxcross + Apple macOS SDK
#       (needs Apple account). Much simpler to just run this on a Mac.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
SDK="$SCRIPT_DIR/SDK"

BUILD_X64="$SCRIPT_DIR/build/mac_x64"
BUILD_ARM="$SCRIPT_DIR/build/mac_arm64"
BUILD_FAT="$SCRIPT_DIR/build/mac_fat"

mkdir -p "$BUILD_X64" "$BUILD_ARM" "$BUILD_FAT"

SOURCES=(
    "$SRC_DIR/Platform.cpp"
    "$SRC_DIR/UDPSocket.cpp"
    "$SRC_DIR/UKPHandler.cpp"
    "$SRC_DIR/BacklightManager.cpp"
    "$SRC_DIR/ConnectionManager.cpp"
    "$SRC_DIR/DisplayStreamer.cpp"
    "$SRC_DIR/SettingsManager.cpp"
    "$SRC_DIR/RelayManager.cpp"
    "$SRC_DIR/UIManager.cpp"
    "$SRC_DIR/Plugin.cpp"
)

CXXFLAGS_COMMON=(
    -std=c++17
    -O2
    -fPIC
    -fvisibility=hidden

    -DXPLM410=1 -DXPLM400=1 -DXPLM303=1 -DXPLM301=1
    -DXPLM300=1 -DXPLM210=1 -DXPLM200=1
    -DAPL=1

    -I"$SDK/CHeaders/XPLM"
    -I"$SDK/CHeaders/Widgets"
    -I"$SDK/CHeaders/Wrappers"
    -I"$SRC_DIR"

    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-reorder

    # macOS minimum version
    -mmacosx-version-min=10.15
)

LDFLAGS_COMMON=(
    -dynamiclib
    -Wl,-exported_symbols_list,"$SCRIPT_DIR/exports_mac.sym"
    -framework OpenGL
    -lpthread
)

build_arch() {
    local arch="$1"
    local outdir="$2"
    echo "  Building $arch..."
    clang++ "${CXXFLAGS_COMMON[@]}" -arch "$arch" \
            "${LDFLAGS_COMMON[@]}" \
            "${SOURCES[@]}" \
            -o "$outdir/X1000_display.xpl"
}

echo "Building X1000_display.xpl (macOS universal)"
echo "  SDK:    $SDK"

build_arch x86_64 "$BUILD_X64"
build_arch arm64  "$BUILD_ARM"

# Combine into universal binary
lipo -create \
    "$BUILD_X64/X1000_display.xpl" \
    "$BUILD_ARM/X1000_display.xpl" \
    -output "$BUILD_FAT/X1000_display.xpl"

echo ""
echo "✓ Universal binary: $BUILD_FAT/X1000_display.xpl"

# macOS needs .xpl as a bundle — rename to .xpl (X-Plane accepts flat dylib)
if [[ "${1:-}" == "install" ]]; then
    XP12_DIR="${XP12:-$HOME/X-Plane 12}"
    PLUGIN_DIR="$XP12_DIR/Resources/plugins/X1000_display"
    mkdir -p "$PLUGIN_DIR/mac_x64" "$PLUGIN_DIR/tools"
    cp "$BUILD_FAT/X1000_display.xpl" "$PLUGIN_DIR/mac_x64/X1000_display.xpl"
    cp "$SCRIPT_DIR/tools/x1000_relay.py" "$PLUGIN_DIR/tools/x1000_relay.py"
    echo "✓ Installed to: $PLUGIN_DIR/mac_x64/"
fi
