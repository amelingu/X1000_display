#!/usr/bin/env bash
# compile_win.sh — Cross-compile X1000_display.xpl for Windows (win_x64)
# from Linux using MinGW-w64.
#
# Prerequisites:
#   sudo apt install mingw-w64

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
BUILD_DIR="$SCRIPT_DIR/build/win_x64"
SDK="$SCRIPT_DIR/SDK"

CROSS="x86_64-w64-mingw32"
CXX="${CROSS}-g++"
DLLTOOL="${CROSS}-dlltool"
NM="${CROSS}-nm"

mkdir -p "$BUILD_DIR"

# ---------------------------------------------------------------------------
# Generate MinGW import libraries from SDK .lib files
# Strategy: extract symbol names from MSVC .lib via nm, create .def, run dlltool
# ---------------------------------------------------------------------------

# Check if X-Plane DLLs exist (better source than .lib files)
find_xplane_dll() {
    local name="$1"
    # Common Windows X-Plane install paths via Wine or shared folder
    for dir in         "$HOME/.wine/drive_c/Program Files/X-Plane 12"         "/mnt/c/Program Files/X-Plane 12"         "$HOME/X-Plane 12 Windows"
    do
        local dll="$dir/${name}.dll"
        if [ -f "$dll" ]; then echo "$dll"; return; fi
    done
}

make_import_lib() {
    local name="$1"          # e.g. XPLM_64
    local msvc_lib="$SDK/Libraries/Win/${name}.lib"
    local deffile="$BUILD_DIR/${name}.def"
    local mingw_lib="$BUILD_DIR/lib${name}.a"

    if [ -f "$mingw_lib" ] && [ "$msvc_lib" -nt "$mingw_lib" ] || [ ! -f "$mingw_lib" ]; then
        echo "  Generating ${name} import library..."

        # Try to use actual X-Plane DLL for perfect symbol extraction
        local xp_dll
        xp_dll=$(find_xplane_dll "$name")
        local symbols=""

        if [ -n "$xp_dll" ] && command -v gendef >/dev/null 2>&1; then
            echo "    (using gendef on $xp_dll)"
            symbols=$(gendef - "$xp_dll" 2>/dev/null | grep -oE "^[A-Za-z][A-Za-z0-9_]+" | sort -u)
        fi

        # Use strings on the MSVC .lib (reliable for import libs)
        if [ -z "$symbols" ]; then
            symbols=$(
                strings "$msvc_lib" 2>/dev/null \
                | grep -E "^XPLM[A-Za-z0-9_]+$|^XPWidget[A-Za-z0-9_]+$|^XPUIElement[A-Za-z0-9_]+$" \
                | sort -u
            )
        fi

        # Final fallback: parse SDK headers
        if [ -z "$symbols" ]; then
            echo "    (parsing SDK headers...)"
            symbols=$(
                grep -rh "XPLM_API\|WIDGET_API" \
                    "$SDK/CHeaders/XPLM/" \
                    "$SDK/CHeaders/Widgets/" \
                    2>/dev/null \
                | grep -oE "(XPLM|XPWidget|XPUIElement)[A-Za-z0-9_]+" \
                | grep -v "_API$\|_f$\|_t$" \
                | sort -u
            )
        fi

        if [ -z "$symbols" ]; then
            echo "  ERROR: Could not extract symbols for ${name}"
            echo "  Check that $msvc_lib exists and SDK headers are present"
            exit 1
        fi

        local count=""
        count=$(echo "$symbols" | wc -l)
        echo "    Found $count symbols"

        # Write .def file
        {
            echo "LIBRARY ${name}.dll"
            echo "EXPORTS"
            echo "$symbols"
        } > "$deffile"

        # Generate MinGW import library
        # Note: do NOT use --kill-at — MinGW needs __imp_ prefixed symbols
        "$DLLTOOL" \
            --def "$deffile" \
            --dllname "${name}.dll" \
            --output-lib "$mingw_lib"

        echo "  → $mingw_lib ($(du -h "$mingw_lib" | cut -f1))"
        # Verify __imp_ symbols were generated
        local imp_count
        imp_count=$("$NM" "$mingw_lib" 2>/dev/null | grep -c "__imp_" || echo 0)
        echo "    __imp_ symbols: $imp_count"
        if [ "$imp_count" -eq 0 ]; then
            echo "  WARNING: No __imp_ symbols — trying without --leading-underscore..."
            "$DLLTOOL" --def "$deffile" --dllname "${name}.dll" \
                --output-lib "$mingw_lib" --add-underscore
            imp_count=$("$NM" "$mingw_lib" 2>/dev/null | grep -c "__imp_" || echo 0)
            echo "    __imp_ symbols after retry: $imp_count"
        fi
    else
        echo "  ${name}: import library up to date"
    fi
}

echo "Preparing import libraries..."
make_import_lib XPLM_64
make_import_lib XPWidgets_64

# Verify they have content
for lib in XPLM_64 XPWidgets_64; do
    libfile="$BUILD_DIR/lib${lib}.a"
    if [ ! -s "$libfile" ]; then
        echo "ERROR: $libfile is empty — symbol extraction failed"
        echo "Inspect $BUILD_DIR/${lib}.def for the generated symbol list"
        exit 1
    fi
done

# ---------------------------------------------------------------------------

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
    -fvisibility=hidden

    -DXPLM410=1 -DXPLM400=1 -DXPLM303=1 -DXPLM301=1
    -DXPLM300=1 -DXPLM210=1 -DXPLM200=1
    -DWIN32=1 -D_WIN32=1 -DIBM=1
    -DWIN32_LEAN_AND_MEAN

    -I"$SDK/CHeaders/XPLM"
    -I"$SDK/CHeaders/Widgets"
    -I"$SDK/CHeaders/Wrappers"
    -I"$SRC_DIR"

    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-reorder
    -Wno-unknown-pragmas
    -Wno-type-limits
)

# Find MinGW system library directory
MINGW_LIBDIR=$(dirname "$("$CXX" -print-file-name=libws2_32.a 2>/dev/null)" 2>/dev/null)
if [ -z "$MINGW_LIBDIR" ] || [ "$MINGW_LIBDIR" = "." ]; then
    # Fallback: search common locations
    for d in         /usr/x86_64-w64-mingw32/lib         /usr/lib/gcc/x86_64-w64-mingw32/*/lib         /usr/x86_64-w64-mingw32/lib64
    do
        if [ -f "$d/libws2_32.a" ]; then MINGW_LIBDIR="$d"; break; fi
    done
fi
if [ -n "$MINGW_LIBDIR" ]; then
    echo "  MinGW libs: $MINGW_LIBDIR"
else
    echo "  WARNING: Could not find MinGW system libs directory"
fi

LDFLAGS=(
    -shared
    -static-libgcc
    -static-libstdc++

    # Static pthread
    -Wl,-Bstatic -lpthread -Wl,-Bdynamic
)

# Windows system libraries — specified as full paths to avoid search issues
SYSLIBS=()
for lib in ws2_32 opengl32 iphlpapi gdi32; do
    if [ -n "$MINGW_LIBDIR" ] && [ -f "$MINGW_LIBDIR/lib${lib}.a" ]; then
        SYSLIBS+=("$MINGW_LIBDIR/lib${lib}.a")
    else
        SYSLIBS+=("-l${lib}")  # fallback to -l
    fi
done

# XPLM import libs passed as explicit file paths (not -l flags)
# This avoids MinGW link-order issues with import libraries
XPLM_LIBS=(
    "$BUILD_DIR/libXPLM_64.a"
    "$BUILD_DIR/libXPWidgets_64.a"
)

WARN_LOG="$BUILD_DIR/warnings.log"

echo ""
echo "Building X1000_display.xpl (Windows)"
echo "  SDK:    $SDK"
echo "  Output: $BUILD_DIR/X1000_display.xpl"
echo ""

"$CXX" "${CXXFLAGS[@]}" "${LDFLAGS[@]}" \
    "${SOURCES[@]}" \
    "${XPLM_LIBS[@]}" \
    "${SYSLIBS[@]}" \
    -o "$BUILD_DIR/X1000_display.xpl" \
    2>&1 | tee "$WARN_LOG"
BUILD_STATUS=${PIPESTATUS[0]}

if [ "${BUILD_STATUS:-0}" -ne 0 ]; then
    echo ""
    echo "✗ Build failed."
    echo ""
    echo "Diagnostic: checking .def files..."
    for lib in XPLM_64 XPWidgets_64; do
        echo "--- $lib.def (first 10 lines) ---"
        head -10 "$BUILD_DIR/${lib}.def" 2>/dev/null || echo "(not found)"
    done
    exit 1
fi

if grep -q "warning:" "$WARN_LOG" 2>/dev/null; then echo "  Warnings logged — see $WARN_LOG"; fi
echo ""
echo "✓ Build succeeded: $BUILD_DIR/X1000_display.xpl"

# ---- Install ----------------------------------------------------------------
if [[ "${1:-}" == "install" ]]; then
    for candidate in \
        "$HOME/Bureau/X-Plane 12" \
        "$HOME/X-Plane 12" \
        "$HOME/Desktop/X-Plane 12" \
        "/opt/X-Plane 12"
    do
        if [[ -d "$candidate" ]]; then XP12_DIR="$candidate"; break; fi
    done
    XP12_DIR="${XP12:-${XP12_DIR:-}}"
    if [[ -z "${XP12_DIR:-}" ]]; then
        echo "✗ X-Plane 12 not found. Set XP12= env var."
        exit 1
    fi
    PLUGIN_DIR="$XP12_DIR/Resources/plugins/X1000_display"
    mkdir -p "$PLUGIN_DIR/win_x64" "$PLUGIN_DIR/tools"
    cp "$BUILD_DIR/X1000_display.xpl" "$PLUGIN_DIR/win_x64/X1000_display.xpl"
    cp "$SCRIPT_DIR/tools/x1000_relay.py" "$PLUGIN_DIR/tools/x1000_relay.py"
    cp "$SCRIPT_DIR/tools/x1000_bezel.py" "$PLUGIN_DIR/tools/x1000_bezel.py"
    echo "✓ Installed to: $PLUGIN_DIR/win_x64/"
fi
