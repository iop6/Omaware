#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts OmaWare from this folder after checking the bundle is complete and consistent.
set -euo pipefail
here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
(cd "$here" && sha256sum --status -c SHA256SUMS) || { echo 'This OmaWare package is incomplete or modified; download it again.' >&2; exit 1; }
# Groups added by the installer (libvirt, kvm) normally only apply after logging in again. Pick them up
# now instead: sg switches to a group you're a member of without a password, one group at a time.
# It also makes that group the primary one, so switch back to yours last (new files stay yours).
if [[ ${OMAWARE_GROUPS_TRIED:-0} -lt 3 ]] && command -v sg >/dev/null; then
    user=$(id -un) switch=
    for group in libvirt kvm; do
        if id -nG "$user" | tr ' ' '\n' | grep -qx "$group" && ! id -nG | tr ' ' '\n' | grep -qx "$group"; then switch=$group; break; fi
    done
    [[ -n $switch || -z ${OMAWARE_GROUPS_TRIED:-} || $(id -g) == $(id -g "$user") ]] || switch=$(id -gn "$user")
    if [[ -n $switch ]]; then
        export OMAWARE_GROUPS_TRIED=$(( ${OMAWARE_GROUPS_TRIED:-0} + 1 ))
        exec sg "$switch" -c "exec $(printf '%q ' "$here/omaware.sh" "$@")"
    fi
fi
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$here/vnc-abi-test" >/dev/null
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-"wayland;xcb"}
exec "$here/omaware" "$@"
