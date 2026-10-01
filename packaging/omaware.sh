#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts OmaWare from this folder after checking the bundle is complete and consistent.
set -euo pipefail
here=$(cd -- "$(dirname -- "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
(cd "$here" && sha256sum --status -c SHA256SUMS) || { echo 'This OmaWare package is incomplete or modified; download it again.' >&2; exit 1; }
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$here/vnc-abi-test" >/dev/null
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-"wayland;xcb"}
exec "$here/omaware" "$@"
