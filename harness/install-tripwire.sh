#!/usr/bin/env bash
# Installs MAIC's tripwire. Run once, as yourself, with sudo:
#   sudo ./harness/install-tripwire.sh
# Installs:
#   /usr/local/sbin/maic-lock   root:root 0755, a copy (never a symlink to a user-writable file)
#   /etc/sudoers.d/maic         lets you run exactly `maic-lock trip` without a password
#   /var/lib/maic/              root-owned directory for the lock file
set -euo pipefail

if [ "$(id -u)" -ne 0 ] || [ -z "${SUDO_USER:-}" ]; then
    echo "run as: sudo $0" >&2
    exit 1
fi

here="$(cd "$(dirname "$0")/.." && pwd)"
bin="$here/build/harness/maic-lock"
if [ ! -x "$bin" ]; then
    echo "build first: cmake --build --preset default" >&2
    exit 1
fi

install -o root -g root -m 0755 "$bin" /usr/local/sbin/maic-lock
install -d -o root -g root -m 0755 /var/lib/maic

rule="$SUDO_USER ALL=(root) NOPASSWD: /usr/local/sbin/maic-lock trip"
tmp="$(mktemp)"
printf '%s\n' "# MAIC tripwire: tripping is passwordless, resetting is not." "$rule" > "$tmp"
visudo -cf "$tmp"
install -o root -g root -m 0440 "$tmp" /etc/sudoers.d/maic
rm -f "$tmp"

echo "installed. check with: maic-lock status"
