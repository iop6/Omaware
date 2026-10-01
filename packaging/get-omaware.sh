#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs OmaWare in one step:
#
#   curl -fsSL https://github.com/iop6/Omaware/releases/latest/download/install.sh | bash
#
# It installs the system packages OmaWare needs (asking once, only if some are missing), turns on
# libvirt, then downloads the newest release, checks its checksum and installs it in your home folder.
# Run it again to reinstall. Options: --yes (don't ask), --no-packages (skip the system setup),
# --dry-run (show what would be done, change nothing).
set -euo pipefail

repo=iop6/Omaware
base=${OMAWARE_RELEASE_BASE:-https://github.com/$repo/releases/latest/download}
helper=/usr/local/libexec/omaware/authorize-bridge
assume_yes=0 system_step=1 dry_run=0
for arg in "$@"; do
    case $arg in
        --yes|-y) assume_yes=1 ;;
        --no-packages) system_step=0 ;;
        --dry-run) dry_run=1 ;;
        -h|--help) sed -n '3,10p' "$0" 2>/dev/null || true; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

say() { printf '\033[1m%s\033[0m\n' "$*"; }
fail() { printf 'OmaWare install: %s\n' "$*" >&2; exit 1; }
# Questions go to the terminal even when this script arrives through a pipe.
ask() {
    [[ $assume_yes == 1 ]] && return 0
    local answer
    { exec 3< /dev/tty; } 2>/dev/null || return 0
    read -r -p "$1 [Y/n] " answer <&3 || answer=
    exec 3<&-
    [[ -z $answer || $answer == [Yy]* ]]
}
run() { if [[ $dry_run == 1 ]]; then echo "  would run: $*"; else "$@"; fi; }

[[ $(uname -m) == x86_64 ]] || fail "OmaWare runs on 64-bit x86 Linux; this computer is $(uname -m)."
[[ $EUID -ne 0 ]] || fail "run this as yourself, not as root; it asks for your password when it needs it."
[[ -e /dev/kvm ]] || echo "Note: turn on virtualization (VT-x/AMD-V) in your computer's firmware settings so VMs run at full speed."

# ---- System packages, libvirt and access ----
. /etc/os-release
family=${ID:-unknown}
for like in ${ID_LIKE:-}; do case $like in debian|ubuntu|fedora|rhel|arch|suse|opensuse) family=$like ;; esac; done
update=() installer=() packages=() missing=()
case $family in
    ubuntu|debian|linuxmint|pop)
        update=(sudo apt-get update -q); installer=(sudo apt-get install -y -q --no-install-recommends)
        packages=(qemu-system-x86 qemu-utils libvirt-daemon-system libvirt-clients virtinst ovmf swtpm swtpm-tools dnsmasq-base
            qt6-base-dev qt6-declarative-dev qt6-wayland qml6-module-qtqml qml6-module-qtquick qml6-module-qtquick-controls
            qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-dialogs qml6-module-qtqml-workerscript
            qml6-module-qtquick-templates libtomlplusplus-dev bzip2 ca-certificates)
        has() { [[ $(dpkg-query -W -f='${Status}' "$1" 2>/dev/null) == "install ok installed" ]]; } ;;
    fedora|rhel|centos|rocky|almalinux)
        installer=(sudo dnf install -y -q)
        packages=(qemu-kvm qemu-img libvirt-daemon-kvm virt-install edk2-ovmf swtpm swtpm-tools qt6-qtbase qt6-qtdeclarative qt6-qtwayland tomlplusplus bzip2)
        has() { rpm -q --whatprovides "$1" >/dev/null 2>&1; } ;;
    arch|endeavouros|manjaro|omarchy)
        installer=(sudo pacman -S --needed --noconfirm)
        packages=(qemu-desktop libvirt virt-install edk2-ovmf swtpm dnsmasq qt6-base qt6-declarative qt6-wayland tomlplusplus bzip2)
        has() { pacman -T "$1" >/dev/null 2>&1; } ;;
    opensuse*|suse|sles)
        installer=(sudo zypper -q install -y)
        packages=(qemu-x86 qemu-tools libvirt-daemon-qemu virt-install qemu-ovmf-x86_64 swtpm qt6-base-devel qt6-declarative-devel qt6-wayland tomlplusplus-devel bzip2)
        has() { rpm -q --whatprovides "$1" >/dev/null 2>&1; } ;;
esac

if [[ $system_step == 1 ]]; then
    [[ ${#installer[@]} -gt 0 ]] || fail "this script doesn't know ${PRETTY_NAME:-this distribution}. Install QEMU/KVM, libvirt, virt-install, OVMF, swtpm and Qt 6.4+ yourself, then run it again with --no-packages."
    for package in "${packages[@]}"; do has "$package" || missing+=("$package"); done

    # Everything that needs your password, collected so you're asked once.
    services=()
    if systemctl list-unit-files virtqemud.socket 2>/dev/null | grep -q virtqemud; then
        for unit in virtqemud.socket virtnetworkd.socket virtstoraged.socket; do
            systemctl is-enabled --quiet "$unit" 2>/dev/null || services+=("$unit")
        done
    elif [[ ${#missing[@]} -gt 0 ]] || ! systemctl is-enabled --quiet libvirtd.socket 2>/dev/null; then
        services=(libvirtd.socket)
    fi
    groups=()
    getent group libvirt >/dev/null && ! id -nG "$USER" | tr ' ' '\n' | grep -qx libvirt && groups+=(libvirt)
    # Most systems already let every user use /dev/kvm; only ask for the group where they don't.
    [[ -e /dev/kvm && ! -w /dev/kvm ]] && getent group kvm >/dev/null && ! id -nG "$USER" | tr ' ' '\n' | grep -qx kvm && groups+=(kvm)

    if [[ ${#missing[@]} -gt 0 || ${#services[@]} -gt 0 || ${#groups[@]} -gt 0 ]]; then
        [[ ${#missing[@]} -gt 0 ]] && what="install QEMU, libvirt and Qt from ${NAME:-your distribution}" || what="turn on libvirt"
        [[ $dry_run == 1 && ${#missing[@]} -gt 0 ]] && echo "Missing packages: ${missing[*]}"
        ask "OmaWare needs to $what. Continue? You'll be asked for your password." \
            || fail "nothing was changed. Run this again when you're ready."
        if [[ ${#missing[@]} -gt 0 ]]; then
            say "Installing system packages…"
            [[ ${#update[@]} -gt 0 ]] && run "${update[@]}"
            run "${installer[@]}" "${missing[@]}"
            # The packages may have added libvirt sockets or the libvirt group.
            if [[ ${#services[@]} -eq 1 && ${services[0]} == libvirtd.socket ]] && systemctl list-unit-files virtqemud.socket 2>/dev/null | grep -q virtqemud; then
                services=(virtqemud.socket virtnetworkd.socket virtstoraged.socket)
            fi
            if [[ ! " ${groups[*]} " == *" libvirt "* ]] && getent group libvirt >/dev/null && ! id -nG "$USER" | tr ' ' '\n' | grep -qx libvirt; then
                groups+=(libvirt)
            fi
        fi
        say "Setting up libvirt…"
        [[ ${#services[@]} -gt 0 ]] && { run sudo systemctl enable --now "${services[@]}" 2>/dev/null || run sudo systemctl enable --now libvirtd 2>/dev/null || true; }
        # OmaWare picks up new groups when it starts, so there's no need to log out.
        [[ ${#groups[@]} -gt 0 ]] && run sudo usermod -aG "$(IFS=,; echo "${groups[*]}")" "$USER"
    fi
fi

# ---- The newest release ----
say "Downloading OmaWare…"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
curl -fsSL --retry 3 -o "$work/SHA256SUMS" "$base/SHA256SUMS" || fail "couldn't reach GitHub. Check your internet connection and try again."
package=$(grep -oE 'omaware-[0-9][0-9.]*-linux-x86_64\.tar\.gz' "$work/SHA256SUMS" | head -1)
[[ -n $package ]] || fail "the release doesn't list a Linux package."
curl -fL --retry 3 --progress-bar -o "$work/$package" "$base/$package" || fail "the download failed. Try again."
(cd "$work" && grep " $package\$" SHA256SUMS | sha256sum --quiet -c -) || fail "the download doesn't match its published checksum, so it wasn't installed. Try again."
tar -xzf "$work/$package" -C "$work"
folder=$work/${package%.tar.gz}
[[ -x $folder/install.sh ]] || fail "the package is incomplete."
if [[ $dry_run == 1 ]]; then echo "  would run: $folder/install.sh"; else OMAWARE_QUIET=1 "$folder/install.sh"; fi

# The small root-owned helper that lets VMs join networks OmaWare creates (asked for in Networks).
# It has to be owned by root, so it's installed with your password like the packages above.
if [[ $system_step == 1 && -f $folder/authorize-bridge ]] && ! cmp -s "$folder/authorize-bridge" "$helper"; then
    run sudo install -D -o root -g root -m 755 "$folder/authorize-bridge" "$helper" || echo "Note: the network helper wasn't installed; creating networks will still work, but allowing VMs to join them won't."
fi

# Libraries the app can't find mean a package is missing (for example on an unusual distribution).
libs=$(cd "$folder" && LD_LIBRARY_PATH=lib ldd ./omaware 2>/dev/null | awk '/not found/ {print $1}' | tr '\n' ' ')
[[ -z $libs ]] || echo "Warning: these libraries are missing, so OmaWare may not start: $libs"
echo
say "OmaWare $(cat "$folder/VERSION" 2>/dev/null) is installed. Open it from your app menu, or type: omaware"
