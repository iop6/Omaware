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
- **Pause on close:** running VMs are always paused when OmaWare closes (updates included) and can be resumed next time.
- **Themes:** follow Omarchy, dark, light or hacker (green on black), with larger text and reduced-motion options.

## Install

Open a terminal and run:

```sh
curl -fsSL https://github.com/iop6/Omaware/releases/latest/download/install.sh | bash
```

That's it. It installs what OmaWare needs from your distribution (asking first; Ubuntu/Debian, Fedora, Arch/Omarchy and openSUSE), turns on libvirt, downloads the newest release, checks it against its published checksums and installs it in your home folder: the app in `~/.local/share/omaware/app` (next to your `vms/` and `isos/`), an `omaware` command and an app-menu entry. If it added you to the `libvirt` and `kvm` groups, log out and back in once. Run the same command again to reinstall; updates normally come through OmaWare itself.

**By hand:** download `omaware-<version>-linux-x86_64.tar.gz` from the [Releases page](https://github.com/iop6/Omaware/releases/latest), unpack it and run its `install.sh`. You need QEMU/KVM, libvirt, `virt-install`, the OVMF UEFI firmware, Qt 6.4+ and toml++ from your distribution, plus `swtpm` for Windows 11.

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

The **download button** at the bottom of the sidebar checks GitHub for a new version at any time, and OmaWare also checks once a day (turn this off in **Settings → Updates**). When one is out, a window shows what's new and warns before anything happens: updating restarts OmaWare, so running VMs are paused first. **Update now** downloads the new version, checks it against the release's checksums, pauses your VMs, and restarts into it on the page you were on; then **Resume all** continues the VMs exactly where they left off. **Not now** changes nothing. The previous version is kept next to the app as `app.previous`. Copies built from source update by building the new version.

## Good to know

- OmaWare manages VMs in your own libvirt session (`qemu:///session`), which needs no root. It only changes VMs it created itself; other VMs are shown read-only.
- Removing a VM never deletes its disk files.
- Snapshots are stored on the same disk as the VM, so they don't replace backups.
- Not supported yet: unattended OS installation, creating LAN bridges, port forwarding, device hotplug, backups, system-wide VMs, shared folders, audio and 3D graphics.

## Documentation

- [User guide](docs/USER-GUIDE.md): everything in the interface, step by step.
- [Networks](docs/NETWORKS.md): connection types, switches, the network map and containment.
- [Development](docs/DEVELOPMENT.md): project layout, building, tests and how the code fits together.
- [VNC compatibility](docs/VNC-COMPATIBILITY.md): why OmaWare ships a patched LibVNCClient.
- [Changelog](CHANGELOG.md) and [third-party software](THIRD_PARTY_NOTICES.md).

## Versions

OmaWare uses [semantic versioning](https://semver.org): bug-fix releases change the last number (1.0.1), new features the middle one (1.1.0), and anything that breaks existing VMs, snapshots or settings the first (2.0.0). Downloads and release notes are on the [Releases page](https://github.com/iop6/Omaware/releases), and [CHANGELOG.md](CHANGELOG.md) lists what changed in each version.

## License

OmaWare is licensed under GPL-3.0-or-later ([LICENSE](LICENSE)). It includes a patched copy of LibVNCClient (GPL-2.0-or-later); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
