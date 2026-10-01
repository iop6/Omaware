# Third-party software

OmaWare's own code is licensed GPL-3.0-or-later ([LICENSE](LICENSE)). It builds on the projects below. No source code from other projects is copied into OmaWare, apart from the two LibVNCClient patches described here.

## Shipped with OmaWare

| Project | License | How it's used |
| --- | --- | --- |
| [LibVNCClient](https://github.com/LibVNC/libvncserver) 0.9.15, commit `9b54b1ec` | GPL-2.0-or-later | The console's VNC client. Built from the pinned upstream source plus two patches, and installed privately next to OmaWare. Its license text is in [patches/COPYING.LibVNCServer](patches/COPYING.LibVNCServer). |

The two patches, in [`patches/`](patches):

- `libvncclient-zero-screen-id.patch` is upstream commit [041ea576](https://github.com/LibVNC/libvncserver/commit/041ea576c3dddd6c7169935aaf8889673024fbfc) by vin, committed by Christian Beier, kept unchanged with its original header.
- `libvncclient-qemu-key-ack.patch` is OmaWare's own fix to the same file, under the same GPL-2.0-or-later license.

The modified file, `src/libvncclient/rfbclient.c`, credits Constantin Kaplinsky (2000–2002), Tridia Corporation (2000) and AT&T Laboratories Cambridge (1999); those headers are left untouched. The corresponding source for the shipped library is the upstream commit above plus these two patches. [VNC-COMPATIBILITY.md](docs/VNC-COMPATIBILITY.md) explains why they're needed.

## Used from your system

| Project | License | How it's used |
| --- | --- | --- |
| [Qt 6](https://www.qt.io) (Core, Gui, Quick, QML, Quick Controls, XML, Network; Test for tests) | LGPL-3.0 or GPL-3.0 | The interface and application framework. Linked dynamically, unmodified. |
| [libvirt](https://libvirt.org) | LGPL-2.1-or-later | Manages VMs, snapshots, statistics and networks. Linked dynamically, unmodified, through its public API. |
| [toml++](https://github.com/marzer/tomlplusplus) 3 | MIT | Reads the Omarchy theme file. Unmodified. License text below. |
| [QEMU](https://www.qemu.org) and `qemu-img` | GPL-2.0 (some files under other compatible licenses) | Runs the VMs (through libvirt) and converts and creates disk images. Run as separate programs, unmodified. |
| [virt-install](https://github.com/virt-manager/virt-manager) with [libosinfo](https://libosinfo.org) | GPL-2.0-or-later / LGPL-2.1-or-later | Produces a VM definition with sensible defaults for the chosen OS (`virt-install --print-xml`). Run as a separate program, unmodified. |
| UEFI firmware (OVMF / EDK II) | BSD-2-Clause-Patent | Firmware for UEFI VMs, loaded by QEMU. |
| [swtpm](https://github.com/stefanberger/swtpm) (optional) | BSD-3-Clause | Emulates the TPM 2.0 that Windows 11 VMs need, started by libvirt. |

When distributing OmaWare together with any of these, include their license notices as shipped by your distribution.

## Studied, not used

These projects were read for ideas or compared as alternatives; none of their code is in OmaWare: virt-manager's interface, Quickemu, Cockpit Machines, spice-gtk, virt-viewer, CXX-Qt, KDE's KCommandBar and Omarchy's theme scripts.

The ISO Shop's Windows download follows the sequence of requests that [Fido](https://github.com/pbatard/Fido) (GPL-3.0) and [Mido](https://github.com/ElliotKillick/Mido) / Quickemu's quickget (MIT) send to Microsoft's download service; OmaWare implements it independently in C++.

## toml++ license

MIT License

Copyright (c) Mark Gillard <mark.gillard@outlook.com.au>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
