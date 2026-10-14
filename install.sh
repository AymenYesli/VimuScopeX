#!/bin/sh
# VimuScope X for any Linux distribution, in one line:
#
#     curl -fsSL https://raw.githubusercontent.com/AymenYesli/VimuScopeX/main/install.sh | sh
#
# Installs (or updates to) the latest release for your user: the app, a menu
# entry, a "vimuscopex" command, and the USB rule that lets you use the scope
# without root (that part asks for your password once).
# Remove it with:   curl -fsSL .../install.sh | sh -s -- --uninstall
set -eu

REPO="AymenYesli/VimuScopeX"
DEST="$HOME/.local/opt/vimuscopex"
BIN="$HOME/.local/bin"
APPS="$HOME/.local/share/applications"
ICONS="$HOME/.local/share/icons/hicolor"
RULE="/etc/udev/rules.d/60-vimuscope.rules"

say() { printf '\033[1;32m==\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

if [ "${1:-}" = "--uninstall" ]; then
    rm -rf "$DEST" "$BIN/vimuscopex" "$APPS/vimuscopex.desktop"
    find "$ICONS" -name 'vimuscopex.png' -delete 2>/dev/null || true
    if [ -f "$RULE" ]; then
        say "Removing the USB rule (asks for your password)"
        sudo rm -f "$RULE" && sudo udevadm control --reload-rules || true
    fi
    say "VimuScope X removed. (Your settings and recordings are kept.)"
    exit 0
fi

[ "$(uname -m)" = "x86_64" ] || die "Only 64-bit PCs (x86_64) are supported for now."
command -v curl >/dev/null || die "curl is needed."

say "Finding the latest release"
URL="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
      | grep -o '"browser_download_url": *"[^"]*x86_64\.AppImage"' | head -n 1 | cut -d'"' -f4)"
[ -n "$URL" ] || die "No AppImage found in the latest release of $REPO."
VERSION="$(echo "$URL" | sed -n 's/.*VimuScopeX-\([0-9.]*\)-x86_64.*/\1/p')"

say "Downloading VimuScope X $VERSION"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fL --progress-bar -o "$TMP/VimuScopeX.AppImage" "$URL"
chmod +x "$TMP/VimuScopeX.AppImage"

# Unpacked rather than run as an AppImage: no FUSE needed, starts faster.
say "Installing to $DEST"
( cd "$TMP" && ./VimuScopeX.AppImage --appimage-extract >/dev/null )
rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")" "$BIN" "$APPS"
mv "$TMP/squashfs-root" "$DEST"

ln -sf "$DEST/AppRun" "$BIN/vimuscopex"
sed "s|^Exec=.*|Exec=$DEST/AppRun %f|" "$DEST/usr/share/applications/vimuscopex.desktop" > "$APPS/vimuscopex.desktop"
for icon in "$DEST"/usr/share/icons/hicolor/*/apps/vimuscopex.png; do
    [ -f "$icon" ] || continue
    size="$(basename "$(dirname "$(dirname "$icon")")")"
    mkdir -p "$ICONS/$size/apps"
    cp "$icon" "$ICONS/$size/apps/vimuscopex.png"
done
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true

if [ ! -f "$RULE" ]; then
    say "Allowing your account to use the scope (asks for your password once)"
    if sudo install -m 644 "$DEST/usr/lib/udev/rules.d/60-vimuscope.rules" "$RULE"; then
        sudo udevadm control --reload-rules
        sudo udevadm trigger --subsystem-match=usb
        say "Unplug and replug the scope if it's connected."
    else
        printf 'Skipped: without it, run the app with sudo to reach the scope.\n'
    fi
fi

say "Done: VimuScope X $VERSION is in your applications menu (command: vimuscopex)."
case ":$PATH:" in *":$BIN:"*) ;; *) printf 'Note: add %s to your PATH to use the "vimuscopex" command.\n' "$BIN" ;; esac
