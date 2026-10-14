#!/usr/bin/env bash
#
# Builds VimuScope X as one self-contained AppImage that runs on most Linux
# distributions: Qt, the protocol decoders (sigrok's and ours), the Python
# they run on and the Vimu SDK library are all inside.
#
#     ./packaging/linux/build-appimage.sh  [/path/to/VmMsoSDK-master]   (fetched if omitted)
#
# Needs: cmake, ninja (or make), a C++17 compiler, pkg-config, Qt 6 (base,
# multimedia, svg, tools), fftw, libsigrokdecode, python3, curl.
#   Arch:          sudo pacman -S --needed cmake ninja qt6-base qt6-multimedia qt6-svg qt6-tools fftw libsigrokdecode python curl
#   Debian/Ubuntu: sudo apt install cmake ninja-build g++ pkg-config qt6-base-dev qt6-multimedia-dev libqt6svg6-dev qt6-tools-dev-tools libfftw3-dev libsigrokdecode-dev python3 curl
#
# An AppImage runs on distributions as new as, or newer than, the one it was
# built on (glibc). For the widest reach build on an older LTS (e.g. Ubuntu
# 22.04, in a container if need be); built on Arch it suits current distros.
#
# Result: dist/VimuScopeX-<version>-x86_64.AppImage
#
set -euo pipefail
say()  { printf '\n\033[1;32m== %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!! %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31mxx %s\033[0m\n' "$*"; exit 1; }

SDK="${1:-}"                         # optional: fetched by CMake when omitted
[ -z "$SDK" ] || [ -d "$SDK" ] || die "No such folder: $SDK"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$SRC/build-appimage"
APPDIR="$BUILD/AppDir"
TOOLS="${XDG_CACHE_HOME:-$HOME/.cache}/vsx-appimage-tools"
VERSION="$(sed -n 's/^project(VimuScopeX VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")"
OUT="$SRC/dist/VimuScopeX-$VERSION-x86_64.AppImage"

for tool in cmake pkg-config python3 curl; do
    command -v "$tool" >/dev/null || die "$tool not found (see the package lists at the top of this script)."
done
QMAKE="$(command -v qmake6 || command -v qmake-qt6 || command -v qmake || true)"
[ -n "$QMAKE" ] || die "qmake (Qt 6) not found - install the Qt 6 tools package."
GENERATOR=(); command -v ninja >/dev/null && GENERATOR=(-G Ninja)

# ---------------------------------------------------------------------------
say "1/5  Building VimuScope X $VERSION"
cmake -S "$SRC" -B "$BUILD/build" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr ${SDK:+-DVIMU_SDK_DIR="$SDK"}
cmake --build "$BUILD/build" --parallel
[ -n "$SDK" ] || SDK="$BUILD/build/_deps/vimusdk-src"   # the one CMake fetched

# ---------------------------------------------------------------------------
say "2/5  Assembling the AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"
install -m 755 "$BUILD/build/VimuScopeX" "$APPDIR/usr/bin/"

# The Vimu SDK library and its siblings (libvmsignal.so ...). The SDK ships
# Linux builds for PCs and for Raspberry Pis: the x86-64 one is picked by
# what the file *is* (a folder-name guess like *x*64* also matched arm64).
SDKLIB=""
while IFS= read -r candidate; do
    if file -L "$candidate" | grep -q 'x86-64'; then SDKLIB="$candidate"; break; fi
done < <(find "$SDK" -name 'libvmmultmso.so' 2>/dev/null)
[ -n "$SDKLIB" ] || die "No x86-64 libvmmultmso.so found under $SDK"
cp -a "$(dirname "$SDKLIB")"/lib*.so* "$APPDIR/usr/lib/"
VENDOR_LIBS=()
for lib in "$APPDIR"/usr/lib/lib*.so; do VENDOR_LIBS+=(--library "$lib"); done

# Protocol decoders next to the executable (the app loads that folder):
# sigrok's own, then ours (K-Line) on top.
DECODING=no
SRD_DIR=""
for candidate in /usr/share/libsigrokdecode/decoders /usr/local/share/libsigrokdecode/decoders; do
    [ -d "$candidate" ] && { SRD_DIR="$candidate"; break; }
done
mkdir -p "$APPDIR/usr/bin/decoders"
if [ -n "$SRD_DIR" ]; then
    cp -r "$SRD_DIR/." "$APPDIR/usr/bin/decoders/"
    DECODING=yes
else
    warn "sigrok's decoders not found: the AppImage will decode SENT only (install libsigrokdecode)."
fi
[ -d "$BUILD/build/decoders" ] && cp -r "$BUILD/build/decoders/." "$APPDIR/usr/bin/decoders/"

# The Python the decoders run on: the one libsigrokdecode is linked against
# (not whichever `python3` comes first on PATH — CI's Qt setup installs its
# own, newer one), its standard library from the system (AppRun points
# PYTHONHOME at it); libpython itself comes with libsigrokdecode below.
PYVER="$(ldd "$BUILD/build/VimuScopeX" | grep -o 'libpython3\.[0-9]*' | head -n 1 | sed 's/^lib//')"
if [ -n "$PYVER" ] && [ -d "/usr/lib/$PYVER" ]; then
    PYLIB="/usr/lib/$PYVER"
else
    PYVER="$(python3 -c 'import sys; print("python%d.%d" % sys.version_info[:2])')"
    PYLIB="$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["stdlib"])')"
fi
echo "Bundling $PYVER from $PYLIB"
mkdir -p "$APPDIR/usr/lib/$PYVER"
# Not: tests, IDLE, Tk (and _tkinter, which pulls in Tcl/Tk), the build
# configuration (config-*: a static python.o that tools choke on), packages.
( cd "$PYLIB" && tar --exclude='./test' --exclude='./idlelib' --exclude='./tkinter' --exclude='./turtledemo' \
      --exclude='./config-*' --exclude='./lib-dynload/_tkinter*' --exclude='./lib-dynload/_test*' \
      --exclude='./lib-dynload/xxlimited*' --exclude='./site-packages' --exclude='./dist-packages' \
      --exclude='__pycache__' -cf - . ) \
    | tar -xf - -C "$APPDIR/usr/lib/$PYVER"

# Desktop entry, icons, udev rule (for reference: an AppImage can't install it).
install -Dm644 "$SRC/packaging/linux/vimuscopex.desktop" "$APPDIR/usr/share/applications/vimuscopex.desktop"
for size in 16 32 48 64 128 256 512; do
    icon="$SRC/resources/app/app-icon-$size.png"
    [ -f "$icon" ] && install -Dm644 "$icon" "$APPDIR/usr/share/icons/hicolor/${size}x${size}/apps/vimuscopex.png"
done
install -Dm644 "$SRC/packaging/linux/60-vimuscope.rules" "$APPDIR/usr/lib/udev/rules.d/60-vimuscope.rules"
install -Dm644 "$SRC/licenses/VmMsoSDK-LICENSE.txt" "$APPDIR/usr/share/licenses/vimuscopex/VmMsoSDK-LICENSE.txt"

# ---------------------------------------------------------------------------
say "3/5  Fetching linuxdeploy (once)"
mkdir -p "$TOOLS"
fetch() {
    [ -x "$TOOLS/$1" ] && return 0
    curl -fL --retry 3 -o "$TOOLS/$1" "$2" || die "Could not download $1"
    chmod +x "$TOOLS/$1"
}
fetch linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch linuxdeploy-plugin-qt-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage

# ---------------------------------------------------------------------------
say "4/5  Bundling Qt and every library"
export PATH="$TOOLS:$PATH"
export APPIMAGE_EXTRACT_AND_RUN=1          # no FUSE needed to run the tools
export QMAKE
export EXTRA_QT_PLUGINS="svg;iconengines;imageformats;multimedia;tls"
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"
export LDAI_OUTPUT="$OUT" OUTPUT="$OUT"
mkdir -p "$SRC/dist"
rm -f "$OUT"
"$TOOLS/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/VimuScopeX" \
    --desktop-file "$APPDIR/usr/share/applications/vimuscopex.desktop" \
    --icon-file "$SRC/resources/app/app-icon-256.png" \
    --icon-filename vimuscopex \
    --custom-apprun "$SRC/packaging/linux/AppRun" \
    "${VENDOR_LIBS[@]}" \
    --plugin qt \
    --output appimage

# ---------------------------------------------------------------------------
say "5/5  Done"
[ -f "$OUT" ] || die "No AppImage was produced - see the linuxdeploy output above."
echo "AppImage : $OUT"
echo "Protocol decoding (sigrok's decoders) included: $DECODING"
echo
echo "Run it:        chmod +x \"$OUT\" && \"$OUT\""
echo "Scope access:  ./packaging/linux/install-udev-rule.sh   (once per PC, then replug the scope)"
