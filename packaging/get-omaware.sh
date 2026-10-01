#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs OmaWare in one step:
#
#   curl -fsSL https://github.com/iop6/Omaware/releases/latest/download/install.sh | bash
#
# 1. Installs what OmaWare needs from your distribution (QEMU/KVM, libvirt, virt-install, UEFI
#    firmware, swtpm for Windows 11, Qt 6), after asking once. Ubuntu/Debian, Fedora, Arch/Omarchy
#    and openSUSE are supported.
# 2. Starts libvirt and adds you to the libvirt and kvm groups.
# 3. Downloads the newest OmaWare release, checks it against its published checksums, and installs it
#    in your home folder (~/.local/share/omaware/app, the "omaware" command and an app-menu entry).
#
# Run it again to update. Options: --yes (don't ask), --no-packages (skip step 1 and 2).
set -euo pipefail

repo=iop6/Omaware
base=${OMAWARE_RELEASE_BASE:-https://github.com/$repo/releases/latest/download}
assume_yes=0 packages_step=1 dry_run=0
for arg in "$@"; do
    case $arg in
        --yes|-y) assume_yes=1 ;;
        --no-packages) packages_step=0 ;;
        --dry-run) dry_run=1 ;;   # show what would be installed, change nothing
        -h|--help) sed -n '3,15p' "$0" 2>/dev/null || true; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

say() { printf '\033[1m%s\033[0m\n' "$*"; }
fail() { printf 'OmaWare install: %s\n' "$*" >&2; exit 1; }
# Questions go to the terminal even when this script arrives through a pipe.
ask() {
    [[ $assume_yes == 1 ]] && return 0
    local answer
    if [[ -r /dev/tty ]]; then read -r -p "$1 [Y/n] " answer < /dev/tty || answer=; else return 0; fi
    [[ -z $answer || $answer == [Yy]* ]]
}
run() { if [[ $dry_run == 1 ]]; then echo "  would run: $*"; else "$@"; fi; }

[[ $(uname -m) == x86_64 ]] || fail "OmaWare runs on 64-bit x86 Linux; this computer is $(uname -m)."
[[ $EUID -ne 0 ]] || fail "run this as yourself, not as root; it asks for your password when it needs it."
command -v curl >/dev/null || fail "curl is needed to download OmaWare."
[[ -e /dev/kvm ]] || echo "Note: /dev/kvm is missing. Turn on virtualization (VT-x/AMD-V) in your computer's firmware settings so VMs run at full speed."

# ---- 1. Packages ----
. /etc/os-release
family=${ID:-unknown}
for like in ${ID_LIKE:-}; do case $like in debian|ubuntu|fedora|rhel|arch|suse|opensuse) family=$like ;; esac; done
update=() installer=() packages=()
case $family in
    ubuntu|debian|linuxmint|pop)
        update=(sudo apt-get update); installer=(sudo apt-get install -y --no-install-recommends)
        packages=(qemu-system-x86 qemu-utils libvirt-daemon-system libvirt-clients virtinst ovmf swtpm swtpm-tools dnsmasq-base
            qt6-base-dev qt6-declarative-dev qt6-wayland qml6-module-qtqml qml6-module-qtquick qml6-module-qtquick-controls
            qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-dialogs qml6-module-qtqml-workerscript
            qml6-module-qtquick-templates libtomlplusplus-dev bzip2 ca-certificates) ;;
    fedora|rhel|centos|rocky|almalinux)
        installer=(sudo dnf install -y)
        packages=(qemu-kvm qemu-img libvirt-daemon-kvm virt-install edk2-ovmf swtpm swtpm-tools qt6-qtbase qt6-qtdeclarative qt6-qtwayland tomlplusplus bzip2) ;;
    arch|endeavouros|manjaro|omarchy)
        installer=(sudo pacman -S --needed --noconfirm)
        packages=(qemu-desktop libvirt virt-install edk2-ovmf swtpm dnsmasq qt6-base qt6-declarative qt6-wayland tomlplusplus bzip2) ;;
    opensuse*|suse|sles)
        installer=(sudo zypper install -y)
        packages=(qemu-x86 qemu-tools libvirt-daemon-qemu virt-install qemu-ovmf-x86_64 swtpm qt6-base-devel qt6-declarative-devel qt6-wayland tomlplusplus-devel bzip2) ;;
esac
if [[ $packages_step == 1 ]]; then
    if [[ ${#installer[@]} -eq 0 ]]; then
        echo "This distribution (${PRETTY_NAME:-$ID}) isn't one this script knows. Install QEMU/KVM, libvirt, virt-install, OVMF, swtpm and Qt 6 (6.4 or newer) yourself, then run this again with --no-packages."
        exit 1
    fi
    say "OmaWare needs these packages from ${PRETTY_NAME:-your distribution} (ones you already have are skipped):"
    printf '  %s\n' "${packages[*]}" | fold -s -w 100
    ask "Install them now? You'll be asked for your password." || fail "nothing was installed. Run this again when you're ready, or use --no-packages if you installed them yourself."
    [[ ${#update[@]} -gt 0 ]] && run "${update[@]}"
    run "${installer[@]}" "${packages[@]}"

    say "Starting libvirt and giving you access to VMs…"
    # Newer libvirt runs as several small services; older versions as one daemon.
    if systemctl list-unit-files virtqemud.socket >/dev/null 2>&1 && systemctl list-unit-files virtqemud.socket | grep -q virtqemud; then
        run sudo systemctl enable --now virtqemud.socket virtnetworkd.socket virtstoraged.socket 2>/dev/null || true
    else
        run sudo systemctl enable --now libvirtd.socket 2>/dev/null || run sudo systemctl enable --now libvirtd || true
    fi
    regroup=()
    for group in libvirt kvm; do
        getent group "$group" >/dev/null && ! id -nG "$USER" | tr ' ' '\n' | grep -qx "$group" && regroup+=("$group")
    done
    if [[ ${#regroup[@]} -gt 0 ]]; then
        run sudo usermod -aG "$(IFS=,; echo "${regroup[*]}")" "$USER"
        relogin=1
    fi
fi

# ---- 2. The newest release ----
say "Downloading the newest OmaWare…"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
curl -fsSL --retry 3 -o "$work/SHA256SUMS" "$base/SHA256SUMS" || fail "couldn't reach GitHub. Check your internet connection and try again."
package=$(grep -oE 'omaware-[0-9][0-9.]*-linux-x86_64\.tar\.gz' "$work/SHA256SUMS" | head -1)
[[ -n $package ]] || fail "the release doesn't list a Linux package."
curl -fL --retry 3 --progress-bar -o "$work/$package" "$base/$package" || fail "the download failed. Try again."
(cd "$work" && grep " $package\$" SHA256SUMS | sha256sum --quiet -c -) || fail "the download doesn't match its published checksum, so it wasn't installed. Try again."
tar -xzf "$work/$package" -C "$work"
folder=$work/${package%.tar.gz}
[[ -x $folder/install.sh ]] || fail "the package is incomplete."
if [[ $dry_run == 1 ]]; then echo "  would run: $folder/install.sh"; else "$folder/install.sh"; fi

# Libraries the app can't find mean a package is missing (for example on an unusual distribution).
missing=$(cd "$folder" && LD_LIBRARY_PATH=lib ldd ./omaware 2>/dev/null | awk '/not found/ {print $1}' | tr '\n' ' ')
[[ -z $missing ]] || echo "Warning: these libraries are missing, so OmaWare may not start: $missing"
echo
if [[ ${relogin:-0} == 1 ]]; then
    say "Done. Log out and back in once (so your new groups apply), then open OmaWare from your app menu or type: omaware"
else
    say "Done. Open OmaWare from your app menu or type: omaware"
fi
