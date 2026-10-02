#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build OmaWare on Ubuntu/Debian, Fedora, Arch (incl. Omarchy) or openSUSE.
#
#   scripts/build.sh [--install-deps] [--install] [--prefix DIR] [--build-dir DIR] [--no-tests]
#
# The console needs LibVNCClient 0.9.15 plus two reviewed fixes that distributions do not ship
# (see docs/VNC-COMPATIBILITY.md). This script builds that pinned, patched library privately inside
# the build directory; nothing is installed system-wide except with --install-deps (packages) or the
# separate, explicit helper step printed at the end.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$project_dir/build"
prefix="$HOME/.local"
install_deps=0 install_app=0 run_tests=1
while [[ $# -gt 0 ]]; do
    case $1 in
        --install-deps) install_deps=1 ;;
        --install) install_app=1 ;;
        --prefix) prefix=${2:?--prefix needs a directory}; shift ;;
        --build-dir) build_dir=${2:?--build-dir needs a directory}; shift ;;
        --no-tests) run_tests=0 ;;
        -h|--help) sed -n '3,11p' "$0"; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
    esac
    shift
done

. /etc/os-release
family=${ID:-unknown}
for like in ${ID_LIKE:-}; do case $like in debian|ubuntu|fedora|rhel|arch|suse|opensuse) family=$like ;; esac; done
case $family in
    ubuntu|debian|linuxmint|pop)
        installer=(sudo apt-get install -y --no-install-recommends)
        packages=(build-essential cmake ninja-build pkg-config git ca-certificates python3
            qt6-base-dev qt6-declarative-dev qt6-wayland qml6-module-qtqml qml6-module-qtquick
            qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-window
            qml6-module-qtquick-dialogs qml6-module-qtqml-workerscript qml6-module-qtquick-templates
            libvirt-dev libvirt-daemon-system libvirt-clients qemu-system-x86 qemu-utils ovmf swtpm virtinst openssh-client libcrypt-dev
            libtomlplusplus-dev libarchive-dev zlib1g-dev libjpeg-dev libpng-dev policykit-1) ;;
    fedora|rhel|centos|rocky|almalinux)
        installer=(sudo dnf install -y)
        packages=(gcc-c++ cmake ninja-build pkgconf-pkg-config git python3 qt6-qtbase-devel
            qt6-qtdeclarative-devel qt6-qtwayland libvirt-devel libvirt-daemon-kvm qemu-kvm qemu-img
            edk2-ovmf swtpm virt-install openssh-clients libxcrypt-devel tomlplusplus-devel libarchive-devel zlib-devel libjpeg-turbo-devel libpng-devel polkit) ;;
    arch|endeavouros|manjaro|omarchy)
        installer=(sudo pacman -S --needed --noconfirm)
        packages=(base-devel cmake ninja pkgconf git python qt6-base qt6-declarative qt6-wayland libvirt
            qemu-desktop edk2-ovmf swtpm virt-install openssh libxcrypt tomlplusplus libarchive zlib libjpeg-turbo libpng polkit) ;;
    opensuse*|suse|sles)
        installer=(sudo zypper install -y)
        packages=(gcc-c++ cmake ninja pkgconf git python3 qt6-base-devel qt6-declarative-devel
            qt6-wayland libvirt-devel libvirt-daemon-qemu qemu-x86 qemu-tools qemu-ovmf-x86_64 swtpm virt-install openssh-clients libxcrypt-devel
            tomlplusplus-devel libarchive-devel zlib-devel libjpeg8-devel libpng16-devel polkit) ;;
    *) installer=(); packages=() ;;
esac
if [[ $install_deps == 1 ]]; then
    [[ ${#installer[@]} -gt 0 ]] || { printf 'Unrecognized distribution "%s"; install the dependencies manually (see README).\n' "$ID" >&2; exit 1; }
    [[ $family == ubuntu || $family == debian ]] && sudo apt-get update
    "${installer[@]}" "${packages[@]}"
fi
missing=()
for tool in cmake ninja pkg-config git cc c++ python3; do command -v "$tool" >/dev/null || missing+=("$tool"); done
for module in Qt6Quick libvirt tomlplusplus libarchive; do pkg-config --exists "$module" 2>/dev/null || missing+=("$module (development files)"); done
if [[ ${#missing[@]} -gt 0 ]]; then
    printf 'Missing: %s\n' "${missing[*]}" >&2
    [[ ${#installer[@]} -gt 0 ]] && printf 'Install them with:  %s --install-deps\n' "$0" >&2
    exit 1
fi

# ---- Pinned, patched LibVNCClient (built privately) ----
vnc_tag=LibVNCServer-0.9.15
vnc_pin=9b54b1ec32731bd23158ca014dc18014db4194c3
declare -A patch_sha=(
    [libvncclient-zero-screen-id.patch]=76034e83a397bca4f9cca2bfdf8bcfcc0c94f891361e9bae30b90f0a6e1e585c
    [libvncclient-qemu-key-ack.patch]=1c5eed6c9af481cc6de9082b9ffee4236c3dc1957fdb640951308bec7256da82
)
deps="$build_dir/deps"; vnc_src="$deps/libvncserver"; vnc_build="$deps/libvncserver-build"; vnc_prefix="$deps/prefix"
mkdir -p "$deps"
if [[ ! -f $vnc_prefix/lib/pkgconfig/libvncclient.pc && ! -f $vnc_prefix/lib64/pkgconfig/libvncclient.pc ]]; then
    for name in "${!patch_sha[@]}"; do
        actual=$(sha256sum "$project_dir/patches/$name" | cut -d' ' -f1)
        [[ $actual == "${patch_sha[$name]}" ]] || { printf 'Patch %s differs from the reviewed version; refusing.\n' "$name" >&2; exit 1; }
    done
    rm -rf "$vnc_src" "$vnc_build"
    # Retry transient network failures; later attempts fall back to HTTP/1.1.
    for attempt in 1 2 3; do
        version=HTTP/2; [[ $attempt -gt 1 ]] && version=HTTP/1.1
        git -c http.version=$version clone --quiet --branch "$vnc_tag" --depth 1 https://github.com/LibVNC/libvncserver.git "$vnc_src" && break
        rm -rf "$vnc_src"; [[ $attempt == 3 ]] && { echo 'Could not download LibVNCServer; check the network and retry.' >&2; exit 1; }
        sleep 3
    done
    [[ $(git -C "$vnc_src" rev-parse HEAD) == "$vnc_pin" ]] || { echo 'Upstream tag does not match the pinned commit; refusing.' >&2; exit 1; }
    git -C "$vnc_src" apply "$project_dir/patches/libvncclient-zero-screen-id.patch"
    git -C "$vnc_src" apply "$project_dir/patches/libvncclient-qemu-key-ack.patch"
    # SASL is disabled so the private library needs no distribution-specific SASL SONAME.
    cmake -S "$vnc_src" -B "$vnc_build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_EXAMPLES=OFF -DWITH_TESTS=OFF \
        -DWITH_SASL=OFF -DCMAKE_INSTALL_PREFIX="$vnc_prefix" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build "$vnc_build" >/dev/null
    cmake --install "$vnc_build" >/dev/null
fi
export PKG_CONFIG_PATH="$vnc_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$vnc_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ---- OmaWare ----
cmake -S "$project_dir" -B "$build_dir/app" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DOMAWARE_BUNDLE_VNC=ON
cmake --build "$build_dir/app"
if [[ $run_tests == 1 ]]; then
    QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir/app" --output-on-failure
fi
if [[ $install_app == 1 ]]; then
    cmake --install "$build_dir/app"
    printf '\nInstalled %s/bin/omaware and its desktop entry.\n' "$prefix"
    printf 'Optional, for bridged/isolated switches (runs as root via pkexec, so it must be root-owned):\n'
    printf '  sudo cmake --install %s --component helper\n' "$build_dir/app"
    groups | grep -qw libvirt || printf 'Note: add yourself to the "libvirt" and "kvm" groups for host networks and KVM.\n'
else
    printf '\nBuilt %s/app/omaware. Install with: %s --install\n' "$build_dir" "$0"
fi
