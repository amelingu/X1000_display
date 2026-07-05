#!/usr/bin/env bash
# compile.sh — Build X1000_display.xpl for X-Plane 12 on Ubuntu 24.04
#
# Prerequisites:
#   sudo apt install build-essential libgl-dev ffmpeg
#
# SDK: The project expects SDK/ in the project root (already present).
#   If you want to upgrade to the latest SDK 4.3.0:
#     1. Download from https://developer.x-plane.com/sdk/plugin-sdk-downloads/
#     2. Unzip and replace the SDK/ folder in this project.
#
# Usage:
#   chmod +x compile.sh && ./compile.sh
#
# Output:
#   build/lin_x64/X1000_display.xpl
#
# Install:
#   cp build/lin_x64/X1000_display.xpl \
#      "<X-Plane 12>/Resources/plugins/X1000_display/lin_x64/X1000_display.xpl"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
BUILD_DIR="$SCRIPT_DIR/build/lin_x64"

# SDK is inside the project — no external path needed
SDK="$SCRIPT_DIR/SDK"
SDK_HEADERS="$SDK/CHeaders"
# On Linux, the SDK stub .so files are not linked against directly;
# X-Plane resolves XPLM symbols at runtime. We do NOT pass -L or -lXPLM.

mkdir -p "$BUILD_DIR"

SOURCES=(
    "$SRC_DIR/Platform.cpp"
    "$SRC_DIR/UDPSocket.cpp"
    "$SRC_DIR/AudioPanelManager.cpp"
    "$SRC_DIR/UKPHandler.cpp"
    "$SRC_DIR/BacklightManager.cpp"
    "$SRC_DIR/ConnectionManager.cpp"
    "$SRC_DIR/DisplayStreamer.cpp"
    "$SRC_DIR/SettingsManager.cpp"
    "$SRC_DIR/RelayManager.cpp"
    "$SRC_DIR/UIManager.cpp"
    "$SRC_DIR/Plugin.cpp"
)

CXXFLAGS=(
    -std=c++17
    -O2
    -fPIC
    -fvisibility=hidden

    # Target XPLM 4.1+ API (required for XPLMGetAvionicsHandle/Geometry/etc.)
    # Also define the earlier version macros as required by the SDK
    -DXPLM410=1
    -DXPLM400=1
    -DXPLM303=1
    -DXPLM301=1
    -DXPLM300=1
    -DXPLM210=1
    -DXPLM200=1
    -DLIN=1

    # SDK headers (all in CHeaders/XPLM/ and CHeaders/Widgets/)
    -I"$SDK_HEADERS/XPLM"
    -I"$SDK_HEADERS/Widgets"
    -I"$SDK_HEADERS/Wrappers"

    -I"$SRC_DIR"

    -Wall -Wextra -Wshadow
    -Wno-unused-parameter
    -Wno-missing-field-initializers  # suppresses stb_image_write.h warnings
    -Wno-reorder                       # suppresses member init order warnings
)

LDFLAGS=(
    -shared
    -Wl,--version-script="$SCRIPT_DIR/exports.sym"
    -lGL
    -ldl
    -lpthread
    # No -lXPLM: X-Plane resolves XPLM symbols at runtime on Linux
)

echo "Building X1000_display.xpl"
echo "  SDK:    $SDK"
echo "  Output: $BUILD_DIR/X1000_display.xpl"
echo ""

WARN_LOG="$BUILD_DIR/warnings.log"

g++ "${CXXFLAGS[@]}" "${LDFLAGS[@]}" \
    "${SOURCES[@]}" \
    -o "$BUILD_DIR/X1000_display.xpl" \
    2> >(grep -v "^$" | tee "$WARN_LOG" | grep -E "error:|warning:.*DisplayStreamer|warning:.*Plugin|warning:.*Connection|warning:.*UKP|warning:.*Backlight" >&2 || true)

if grep -q "warning:" "$WARN_LOG" 2>/dev/null; then echo "  Warnings logged — see $WARN_LOG"; fi

echo ""
echo "✓ Build succeeded: $BUILD_DIR/X1000_display.xpl"

# ---- Optional install -------------------------------------------------------
# Usage: ./compile.sh install
# Searches common X-Plane 12 locations; set XP12 env var to override.
# -----------------------------------------------------------------------------

if [[ "${1:-}" == "install" ]]; then
    # Find X-Plane 12 installation
    if [[ -n "${XP12:-}" ]]; then
        XP12_DIR="$XP12"
    else
        # Common locations
        for candidate in             "$HOME/Bureau/X-Plane 12"             "$HOME/X-Plane 12"             "$HOME/Desktop/X-Plane 12"             "/opt/X-Plane 12"             "/usr/local/X-Plane 12"
        do
            if [[ -d "$candidate" ]]; then
                XP12_DIR="$candidate"
                break
            fi
        done
    fi

    if [[ -z "${XP12_DIR:-}" ]]; then
        echo ""
        echo "✗ Could not find X-Plane 12. Set XP12 environment variable:"
        echo "  XP12='/path/to/X-Plane 12' ./compile.sh install"
        exit 1
    fi

    PLUGIN_DIR="$XP12_DIR/Resources/plugins/X1000_display"
    PLATFORM_DIR="$PLUGIN_DIR/lin_x64"
    TOOLS_DIR="$PLUGIN_DIR/tools"

    mkdir -p "$PLATFORM_DIR"
    mkdir -p "$TOOLS_DIR"

    # Install plugin binary
    cp "$BUILD_DIR/X1000_display.xpl" "$PLATFORM_DIR/X1000_display.xpl"
    echo "✓ Plugin:  $PLATFORM_DIR/X1000_display.xpl"

    # Install relay scripts
    cp "$SCRIPT_DIR/tools/x1000_relay.py" "$TOOLS_DIR/x1000_relay.py"
    echo "✓ Relay:   $TOOLS_DIR/x1000_relay.py"
    cp "$SCRIPT_DIR/tools/x1000_bezel.py" "$TOOLS_DIR/x1000_bezel.py"
    echo "✓ Bezel:   $TOOLS_DIR/x1000_bezel.py"
    echo ""
    echo "Reload the plugin in X-Plane: Plugins → Plugin Admin → X1000 display → Reload"
else
    echo ""
    echo "Install:"
    echo "  ./compile.sh install"
    echo "  (or manually: cp $BUILD_DIR/X1000_display.xpl '<XP12>/Resources/plugins/X1000_display/lin_x64/')"
fi
