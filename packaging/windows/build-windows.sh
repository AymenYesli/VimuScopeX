#!/usr/bin/env bash
#
# Builds VimuScopeX for Windows and packages it as a self-contained folder
# (and zip) that runs on any 64-bit Windows 10/11 PC with no installs:
# the exe, Qt, every runtime DLL, the Vimu SDK DLLs, the protocol
# decoders and a trimmed Python for them.
#
# Run from the "MSYS2 UCRT64" shell (https://www.msys2.org):
#
#     ./packaging/windows/build-windows.sh  [/c/path/to/VmMsoSDK-master]   (fetched if omitted)
#
# A Windows path works too:  ... "C:\Users\me\VmMsoSDK-master"
#
# Result: dist/VimuScopeX/  (run VimuScopeX.exe),  dist/VimuScopeX-windows.zip
# and the installer  dist/VimuScopeX-<version>-setup.exe
#
set -euo pipefail

say()  { printf '\n\033[1;32m== %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!! %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31mxx %s\033[0m\n' "$*"; exit 1; }

[ "${MSYSTEM:-}" = "UCRT64" ] || die "Run this from the 'MSYS2 UCRT64' shell (not MSYS, MINGW64 or CLANG64)."

# The Vimu SDK folder is optional: without it CMake fetches Vimu's own
# (github.com/vimu-tech/VmMsoSDK) into the build tree.
SDK="${1:-}"
if [ -n "$SDK" ]; then
    case "$SDK" in *\\*|[A-Za-z]:*) SDK="$(cygpath -u "$SDK")" ;; esac
    [ -d "$SDK/SharedLibrary" ] || die "No SharedLibrary folder in '$SDK' - is that the VmMsoSDK-master folder?"
fi

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPS="$HOME/vsx-deps"            # libsigrokdecode is built and installed here, once
BUILD="$SRC/build-windows"
DIST="$SRC/dist/VimuScopeX"
P=mingw-w64-ucrt-x86_64

# ---------------------------------------------------------------------------
say "1/6  Installing build tools and libraries (pacman)"
pacman -S --needed --noconfirm \
    git make autoconf automake libtool zip \
    $P-toolchain $P-cmake $P-ninja $P-pkgconf \
    $P-qt6-base $P-qt6-tools $P-qt6-multimedia $P-qt6-svg $P-fftw $P-glib2 $P-python \
    $P-nsis

# ---------------------------------------------------------------------------
say "2/6  libsigrokdecode (protocol decoding)"
# MSYS2 has no package for it, so it is built from source - once; later runs
# reuse it. If it fails the app is still built, just without decoding.
if [ -f "$DEPS/lib/pkgconfig/libsigrokdecode.pc" ]; then
    echo "already built in $DEPS"
else
    rm -rf "$DEPS/src/libsigrokdecode"
    mkdir -p "$DEPS/src"
    if git clone --depth 1 https://github.com/sigrokproject/libsigrokdecode.git \
           "$DEPS/src/libsigrokdecode" \
       && ( cd "$DEPS/src/libsigrokdecode" \
            && ./autogen.sh \
            && ./configure --prefix="$DEPS" --disable-static \
            && make -j"$(nproc)" \
            && make install ); then
        echo "libsigrokdecode installed in $DEPS"
    else
        warn "libsigrokdecode failed to build - continuing WITHOUT protocol decoding."
        warn "Send the error above to get it fixed."
    fi
fi
export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# ---------------------------------------------------------------------------
say "3/6  Building VimuScopeX"
# Fresh every time: CMake caches "library not found" results, which would
# otherwise hide a libsigrokdecode that has since been built.
rm -rf "$BUILD"
# Source copied from another machine can carry timestamps ahead of this
# PC's clock (a zip keeps them). Then CMakeLists.txt always looks newer
# than the build files, and Ninja re-runs CMake until it gives up with
# "manifest 'build.ninja' still dirty after 100 tries". Stamp everything
# with this PC's "now" first.
find "$SRC" \( -path "$SRC/dist" -o -path "$SRC/.git" \) -prune -o -exec touch {} + 2>/dev/null || true
find "$SDK" -exec touch {} + 2>/dev/null || true
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release ${SDK:+-DVIMU_SDK_DIR="$SDK"}
cmake --build "$BUILD"
[ -n "$SDK" ] || SDK="$BUILD/_deps/vimusdk-src"      # the one CMake fetched

# ---------------------------------------------------------------------------
say "4/6  Assembling a self-contained folder"
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$BUILD/VimuScopeX.exe" "$DIST/"

# Qt: its DLLs, the Windows platform plugin, styles, image formats.
WDQ="$(command -v windeployqt6 || command -v windeployqt || true)"
[ -z "$WDQ" ] && [ -x /ucrt64/share/qt6/bin/windeployqt.exe ] && WDQ=/ucrt64/share/qt6/bin/windeployqt.exe
[ -n "$WDQ" ] || die "windeployqt not found (package $P-qt6-tools)."
"$WDQ" --release --no-translations --no-system-d3d-compiler --no-opengl-sw \
       --dir "$DIST" "$DIST/VimuScopeX.exe"

# Vimu SDK runtime: vmmultmso.dll and what sits beside it (vmsignal.dll ...).
SDKDLL="$(find "$SDK/SharedLibrary" -ipath '*windows*' -ipath '*x64*' -ipath '*release*' \
               -iname 'vmmultmso.dll' | head -n 1)"
[ -n "$SDKDLL" ] || die "vmmultmso.dll (Windows/X64/Release) not found under $SDK/SharedLibrary"
cp "$(dirname "$SDKDLL")"/*.dll "$DIST/"
mkdir -p "$DIST/licenses" && cp "$SRC"/licenses/*.txt "$DIST/licenses/"

# Protocol decoders + the Python they run on.
if ls "$DEPS"/bin/libsigrokdecode*.dll >/dev/null 2>&1 && [ -d "$BUILD/decoders" ]; then
    cp "$DEPS"/bin/libsigrokdecode*.dll "$DIST/"
    mkdir -p "$DIST/decoders"
    cp -r "$DEPS/share/libsigrokdecode/decoders/." "$DIST/decoders/"
    cp -r "$BUILD/decoders/." "$DIST/decoders/"          # our own (K-Line)

    PYVER="$(python3 -c 'import sys; print("python%d.%d" % sys.version_info[:2])')"
    mkdir -p "$DIST/lib"
    cp -r "/ucrt64/lib/$PYVER" "$DIST/lib/"
    # Trim what an embedded decoder interpreter never uses.
    for d in test tests idlelib tkinter turtledemo ensurepip lib2to3 site-packages; do
        rm -rf "$DIST/lib/$PYVER/$d"
    done
    find "$DIST/lib" -name '__pycache__' -type d -prune -exec rm -rf {} +
    DECODING=yes
else
    DECODING=no
fi

# Every DLL anything in the folder needs (Qt's own dependencies, FFTW,
# glib, Python, the C/C++ runtime ...), repeated until nothing new appears.
# Microsoft's C++ runtime, if the SDK DLLs use it, is copied app-locally so
# the target PC needs no Visual C++ Redistributable either.
#
# ldd has to be able to resolve everything, or it falls back to listing
# imports it can only find on PATH: so libsigrokdecode's folder goes on
# PATH, and ldd runs from inside the folder, where the app's own DLLs are.
export PATH="$DEPS/bin:$PATH"
MISSING=""
for pass in 1 2 3 4 5 6 7 8; do
    added=0
    while IFS= read -r line; do
        dep="$(awk '{print $3}' <<<"$line")"
        name="$(awk '{print $1}' <<<"$line")"
        if [ "$dep" = "not" ]; then            # "foo.dll => not found"
            # api-ms-win-* / ext-ms-win-* are Windows API-set names, not
            # files: Windows 10/11 resolves them itself.
            grep -qiE '^(api|ext)-ms-win-' <<<"$name" || MISSING="$MISSING $name"
            continue
        fi
        keep=no
        case "$dep" in
            /ucrt64/*|"$DEPS"/*) keep=yes ;;
        esac
        if grep -qiE '/system32/(vcruntime|msvcp|concrt|vccorlib)[0-9a-z_]*\.dll$' <<<"$dep"; then
            keep=yes
        fi
        [ "$keep" = yes ] || continue
        [ -e "$DIST/$(basename "$dep")" ] && continue
        cp "$dep" "$DIST/"
        added=1
    done < <(cd "$DIST" && find . -type f \( -iname '*.exe' -o -iname '*.dll' -o -iname '*.pyd' \) \
                  -exec ldd {} + 2>/dev/null | grep '=>' | sort -u)
    [ "$added" = 0 ] && break
done

# ---------------------------------------------------------------------------
say "5/6  Zipping"
( cd "$SRC/dist" && rm -f VimuScopeX-windows.zip && zip -qr VimuScopeX-windows.zip VimuScopeX )

# ---------------------------------------------------------------------------
say "6/6  Installer"
VERSION="$(sed -n 's/^project(VimuScopeX VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")"
SETUP="$SRC/dist/VimuScopeX-$VERSION-setup.exe"
MAKENSIS="$(command -v makensis || true)"
if [ -n "$MAKENSIS" ]; then
    rm -f "$SETUP"
    "$MAKENSIS" -V2 -DVERSION="$VERSION" \
        -DSRCDIR="$(cygpath -w "$DIST")" \
        -DOUTFILE="$(cygpath -w "$SETUP")" \
        -DICON="$(cygpath -w "$SRC/resources/windows/VimuScopeX.ico")" \
        "$(cygpath -w "$SRC/packaging/windows/installer.nsi")" \
        || { warn "The installer failed to build (the folder and zip are fine)."; SETUP=""; }
else
    warn "makensis not found (package $P-nsis): no installer, the folder and zip are fine."
    SETUP=""
fi

say "Done"
echo "Folder    : $DIST"
echo "Zip       : $SRC/dist/VimuScopeX-windows.zip"
[ -n "$SETUP" ] && echo "Installer : $SETUP"
echo "Protocol decoding included: $DECODING"
MISSING="$(tr ' ' '\n' <<<"$MISSING" | sort -u | tr '\n' ' ')"
if [ -n "${MISSING// /}" ]; then
    warn "These DLLs could not be found on this PC and are NOT in the folder:$MISSING"
fi
echo
echo "Test it from Windows Explorer (double-click VimuScopeX.exe), not from this"
echo "shell - this shell's PATH would hide any DLL the folder is missing."
