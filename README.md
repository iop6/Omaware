# OmaWare

OmaWare is a virtual machine manager for Linux desktops. It runs QEMU/KVM virtual machines through libvirt, with a built-in console, a snapshot tree and an interactive network map. It follows your [Omarchy](https://omarchy.org) theme when available and works on any desktop.

## Features

- **Create VMs** from an ISO or an existing disk image, and change processors, memory, disks, boot order and network adapters.
- **ISO Shop:** the newest official installers for 19 systems (Ubuntu, Mint, Fedora, Debian, Arch, openSUSE, Rocky, Alma, Kali, OPNsense and more), one click each, verified against the publisher's checksum, with update alerts.
- **Built-in console** with fullscreen, a detachable window, clipboard sharing and display resizing.
- **Snapshots** in a branching tree. Snapshots of a running VM include its memory, so going back resumes it exactly where it was, without rebooting.
- **Networks made simple:** a live map of how your VMs connect, with their IP addresses. Drag a VM onto a network to connect it, pull a virtual cable to disconnect it, see at a glance which VMs can reach the internet, and cut them all off with one click. Changes apply to running VMs straight away.
- **Containment** for VMs running untrusted software: isolated networks only, no shared folders, clipboard or USB passthrough.
- **Monitor:** live CPU, memory, disk and network use for your computer and every VM.
- **Organized workspace:** favorites, folders, tags, search, a command prompt, keyboard shortcuts, and actions on several VMs at once.
- **Pause on close:** running VMs are paused when OmaWare closes and can be resumed next time.
- **Themes:** follow Omarchy, dark, light or hacker (green on black), with larger text and reduced-motion options.

## Install

You need a 64-bit Linux with KVM, plus QEMU, libvirt, `qemu-img` and `virt-install`. Most distributions have these as packages (Arch/Omarchy: `qemu-desktop libvirt virt-install`).

**The easy way:** download `omaware-<version>-linux-x86_64.tar.gz` from the [Releases page](https://github.com/iop6/Omaware/releases/latest), then:

```sh
tar -xzf omaware-*-linux-x86_64.tar.gz
omaware-*-linux-x86_64/install.sh
omaware                     # or open "OmaWare" from your app menu
```

This installs into your home folder only: the app goes in `~/.local/share/omaware/app` (next to your `vms/` and `isos/`), plus an `omaware` command and a menu entry. The package uses the Qt 6.4+, libvirt and toml++ libraries already on your system.

**From source** (Ubuntu/Debian, Fedora, Arch/Omarchy and openSUSE):

```sh
scripts/build.sh --install-deps   # optional: installs the needed packages (uses sudo)
scripts/build.sh --install        # builds, tests and installs to ~/.local
```

Then add yourself to the `libvirt` and `kvm` groups (log out and back in afterwards):

```sh
sudo usermod -aG libvirt,kvm "$USER"
```

To connect session VMs to networks you create, also install the small root helper (from a source build):

```sh
sudo cmake --install build/app --component helper
```

If the window stays blank on a machine without working GPU drivers, start OmaWare with `QT_QUICK_BACKEND=software`.

## Updating

OmaWare checks GitHub for a new release once a day (turn this off in **Settings → Updates**). When one is out, the sidebar says so: **Update** downloads it in the background and checks it against the release's checksums, and **Restart now** switches to it in a moment, right back on the page you were on. Your VMs keep running throughout, and the previous version is kept next to the app as `app.previous`. Copies built from source update by building the new version.

## Good to know

- OmaWare manages VMs in your own libvirt session (`qemu:///session`), which needs no root. It only changes VMs it created itself; other VMs are shown read-only.
- Removing a VM never deletes its disk files.
- Snapshots are stored on the same disk as the VM, so they don't replace backups.
- Not supported yet: unattended OS installation, Windows 11 (TPM), creating LAN bridges, port forwarding, device hotplug, backups, system-wide VMs, shared folders, audio and 3D graphics.

## Documentation

- [User guide](docs/USER-GUIDE.md): everything in the interface, step by step.
- [Networks](docs/NETWORKS.md): connection types, switches, the network map and containment.
- [Development](docs/DEVELOPMENT.md): project layout, building, tests and how the code fits together.
- [VNC compatibility](docs/VNC-COMPATIBILITY.md): why OmaWare ships a patched LibVNCClient.
- [Changelog](CHANGELOG.md) and [third-party software](THIRD_PARTY_NOTICES.md).

## Versions

OmaWare uses [semantic versioning](https://semver.org): `MAJOR.MINOR.PATCH`.

- **PATCH** (1.0.1): bug fixes only.
- **MINOR** (1.1.0): new features that keep existing VMs, snapshots and settings working.
- **MAJOR** (2.0.0): changes that break compatibility with existing snapshots, settings or saved data.

The main branch is always the next version in development, and its builds end in `-dev` (for example `1.1.0-dev`). Each finished version is a git tag (`v1.0.0`) and a [GitHub Release](https://github.com/iop6/Omaware/releases) with release notes, a ready-to-run app, a source archive and checksums. The version is set once, in `CMakeLists.txt`; `omaware --version` and the Settings panel show it. [CHANGELOG.md](CHANGELOG.md) lists what changed in each version.

**To make a release:**

1. In [CHANGELOG.md](CHANGELOG.md), move the "Unreleased" notes under the new version number and date.
2. In `CMakeLists.txt`, set the version and remove the `-dev` suffix.
3. Build and run the tests.
4. Commit, then tag: `git tag -a vX.Y.Z -m "OmaWare X.Y.Z"`.
5. Build the app package (the binary, its private LibVNCClient in `lib/`, `vnc-abi-test`, `packaging/omaware.sh` and `packaging/install.sh`, the license files and a `SHA256SUMS` covering them) and the source archive, and publish them with a `SHA256SUMS` for both as the GitHub Release for the tag. The built-in updater looks for `omaware-X.Y.Z-linux-x86_64.tar.gz` and `SHA256SUMS` there.
6. Set `CMakeLists.txt` to the next version, put the `-dev` suffix back, and commit.

## License

OmaWare is licensed under GPL-3.0-or-later ([LICENSE](LICENSE)). It includes a patched copy of LibVNCClient (GPL-2.0-or-later); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
