#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs this OmaWare package for the current user:
#   ~/.local/share/omaware/app    the app (next to your vms/ and isos/); updates replace it in place
#   ~/.local/bin/omaware          the command
#   an entry in your app menu
# Run it again from a newer package to update by hand. Nothing outside your home folder is changed.
set -euo pipefail
here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
data="${XDG_DATA_HOME:-$HOME/.local/share}"
target="$data/omaware/app"
bin="$HOME/.local/bin"
if [[ $here == "$target" ]]; then echo "OmaWare is already installed here."; exit 0; fi
(cd "$here" && sha256sum --status -c SHA256SUMS) || { echo "This package is incomplete or changed; download it again." >&2; exit 1; }
if pgrep -f "^$target/omaware( |\$)" >/dev/null; then echo "OmaWare is open. Close it first, then run this again." >&2; exit 1; fi
mkdir -p "$data/omaware" "$bin" "$data/applications"
# Keep the version being replaced, so it's easy to go back.
if [[ -d $target ]]; then rm -rf "$target.previous"; mv "$target" "$target.previous"; fi
cp -a "$here" "$target"
cat > "$bin/omaware" <<LAUNCHER
#!/bin/sh
exec "$target/omaware.sh" "\$@"
LAUNCHER
chmod +x "$bin/omaware"
cat > "$data/applications/omaware.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=OmaWare
GenericName=Virtual Machine Manager
Comment=Run and manage QEMU/KVM virtual machines
Exec=$bin/omaware
Icon=computer
Terminal=false
Categories=System;Emulator;
Keywords=vm;virtual;qemu;kvm;libvirt;snapshot;
DESKTOP
echo "Installed OmaWare $(cat "$target/VERSION" 2>/dev/null || echo) in $target."
echo "Start it from your app menu or with: omaware"
case ":$PATH:" in *":$bin:"*) ;; *) echo "($bin isn't on your PATH yet; add it to run 'omaware' by name.)" ;; esac
