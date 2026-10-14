# Packaging VimuScope X 1.0.0

The scope's driver library comes from Vimu's SDK (MIT licence,
https://github.com/vimu-tech/VmMsoSDK). Nothing to download by hand: when no SDK
folder is given, CMake fetches it into the build tree. Every package bundles it
with its licence (`licenses/VmMsoSDK-LICENSE.txt`). Passing a local SDK folder
to the scripts below still works.

## Windows: installer + portable zip

From the **MSYS2 UCRT64** shell (https://www.msys2.org):

    ./packaging/windows/build-windows.sh

Produces, in `dist/`:

- `VimuScopeX-1.0.0-setup.exe`: the installer (Program Files, Start menu and
  desktop shortcuts, uninstaller, Apps & features entry). Settings, recordings
  and probes are kept when it's uninstalled or updated.
- `VimuScopeX-windows.zip` / `VimuScopeX/`: the same, portable (no install).

The installer isn't code-signed, so Windows SmartScreen may show "Windows
protected your PC" the first time: *More info* → *Run anyway*. A PC that has
never had the scope plugged in may also need the scope's USB driver from the
vendor's own software.

## Linux, any distribution: AppImage

    ./packaging/linux/build-appimage.sh

Produces `dist/VimuScopeX-1.0.0-x86_64.AppImage`: one file with everything
inside (Qt, decoders, their Python, the SDK library). Run it with:

    chmod +x VimuScopeX-1.0.0-x86_64.AppImage && ./VimuScopeX-1.0.0-x86_64.AppImage

It runs on distributions as new as, or newer than, the one it was built on.
For the widest reach build it on an older LTS (e.g. Ubuntu 22.04).

## Arch Linux: native package

    cd packaging/arch
    makepkg -si

Installs `/opt/vimuscopex`, a `vimuscopex` command, a menu entry, icons and the
udev rule; remove with `sudo pacman -R vimuscopex`.

## Linux: scope access without root (once per PC)

The Arch package installs this itself. For the AppImage or a source build:

    ./packaging/linux/install-udev-rule.sh

then unplug and replug the scope.

---

# Publishing: GitHub, AUR (yay) and winget

End result for users: `yay -S vimuscopex-bin` on Arch, `winget install VimuScopeX`
on Windows; the AppImage from the Releases page on any other distribution.
Replace `yourname` everywhere with your GitHub user name.

## 0. Licence (your choice, before publishing)
Vimu's SDK is MIT: redistributing it is allowed with its notice (done). Your
own code needs a licence too: the app links libsigrokdecode (GPL-3.0) and FFTW
(GPL-2.0+), so **GPL-3.0-or-later** is the natural choice — add it as `LICENSE`
at the repository root (GitHub offers the text when you add a file named LICENSE).
One nuance: Vimu publishes their library as binaries only, and GPL works are
expected to come with source for what they link; adding a sentence to your
LICENSE that explicitly permits linking the Vimu SDK library is the usual way
projects handle that for their own code.

## 1. GitHub repository
    git init && git add . && git commit -m "VimuScope X 1.0.0"
    git branch -M main
    git remote add origin git@github.com:yourname/VimuScopeX.git
    git push -u origin main

## 2. A release
    git tag v1.0.0 && git push origin v1.0.0
GitHub Actions (`.github/workflows/release.yml`) builds the Windows installer, the
portable zip and the AppImage, and publishes them with `SHA256SUMS.txt` on the
repository's Releases page (about 20-30 minutes).

## 3. AUR (yay)
Once: an account on https://aur.archlinux.org with your SSH public key. Then:
    git clone ssh://aur@aur.archlinux.org/vimuscopex-bin.git
    cp packaging/aur/PKGBUILD vimuscopex-bin/ && cd vimuscopex-bin
    sed -i 's/yourname/your-user/; s/your-email@example.com/you@example.com/' PKGBUILD
    updpkgsums && makepkg --printsrcinfo > .SRCINFO
    makepkg -si                       # test it locally
    git add PKGBUILD .SRCINFO && git commit -m "vimuscopex-bin 1.0.0" && git push
Users: `yay -S vimuscopex-bin`. New version: bump `pkgver`, repeat `updpkgsums`,
`.SRCINFO`, commit, push.

## 4. winget
On Windows, once: `winget install wingetcreate`, and a GitHub token
(`wingetcreate token -s`). Then:
    wingetcreate new https://github.com/yourname/VimuScopeX/releases/download/v1.0.0/VimuScopeX-1.0.0-setup.exe
It reads the installer, asks for the details (`packaging/winget/` holds the values to
use: identifier `yourname.VimuScopeX`, name, licence, description) and opens a
pull request on microsoft/winget-pkgs. Microsoft's checks and review take a few
days; then: `winget install VimuScopeX`. New version:
    wingetcreate update yourname.VimuScopeX -v 1.0.1 -u <new setup.exe URL> --submit

## 5. Zero friction on a clean Windows PC
Check whether the scope works on a Windows PC that never had Vimu's software.
The SDK talks to the scope through libusb, which on Windows needs the WinUSB
driver bound to the device: if Windows doesn't bind it by itself, our installer
can do it (an INF for USB d4a2:5663 installed with `pnputil /add-driver <inf> /install`).
Linux needs nothing more: the AUR package installs the udev rule; AppImage users
run `packaging/linux/install-udev-rule.sh` once.
