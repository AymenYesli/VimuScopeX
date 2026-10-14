#!/usr/bin/env bash
# Lets your user account use the scope without root (once per PC):
#     ./packaging/linux/install-udev-rule.sh
# Then unplug and replug the scope.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sudo install -m 644 "$HERE/60-vimuscope.rules" /etc/udev/rules.d/60-vimuscope.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
echo "Installed. Unplug and replug the scope."
