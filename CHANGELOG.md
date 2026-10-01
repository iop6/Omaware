# Changelog

All notable changes to OmaWare are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[semantic versioning](https://semver.org).

## [Unreleased]

### Added

- Drag ISO files onto the window to add them to your ISOs. A file on the same disk is added
  instantly without using extra space; others are copied in the background with a progress bar
  and a Cancel button. Dropping one ISO opens Create VM with it chosen.

### Fixed

- The ISO list in Create VM now shows the chosen ISO however it was picked (Browse, the ISO Shop or a drop).

## [1.0.0] - 2026-09-30

First release.

### Virtual machines
- Create VMs from an ISO or an existing raw/qcow2 disk, with BIOS or UEFI firmware.
- Edit CPU, memory, boot order, disks and network adapters, with pending changes shown until a full restart.
- Start, pause, resume, shut down and force off, one VM at a time or several at once with Ctrl/Shift-click.
- Running VMs are paused when OmaWare closes and can be resumed together or individually next time.
- Only VMs OmaWare created (`omaware-*` with OmaWare metadata) can be changed; others are read-only.

### Console
- Built-in VNC console with input capture (Ctrl+Alt releases), clipboard sharing, display resizing, fullscreen and a detachable window.

### Snapshots
- Branching snapshot tree with memory, so a running VM resumes exactly where it was.
- Restore, undo restore, rename, pin, verify, delete a snapshot or a whole branch, and create a new VM from a snapshot.
- Background copying with progress and cancel.

### Networks
- A live network map: drag a VM onto a network to connect it, pull or plug virtual cables, see each VM's IP address and whether it can reach the internet, and cut every VM off the internet with one click.
- Connection changes apply to running VMs straight away when the guest OS supports it.
- Plain-language networks ("Internet + VMs", "This computer + VMs", "VMs only"), created with automatic addresses and permission for VMs to join in one step.
- "VMs only" networks are verified isolated, live.
- Containment for VMs running untrusted software: verified isolated networks only, no shared folders, clipboard or device passthrough.

### ISO Shop
- One-click downloads of the newest official ISOs for Ubuntu, Kubuntu, Xubuntu, Linux Mint, Fedora Workstation and KDE, Debian, Arch, openSUSE Tumbleweed, Pop!_OS, NixOS, Ubuntu Server, Rocky Linux, AlmaLinux, Alpine, Proxmox VE, FreeBSD, Kali and OPNsense; pfSense and Windows 11 link to their publishers' pages.
- Every download is verified against the publisher's SHA-256 checksum; update alerts; quick deleting of old ISOs.
- "New VM" from the shop fills in the ISO, the matching operating system and a name.

### Workspace
- VMs and ISOs live in one ordinary folder, `~/.local/share/omaware/`.
- Monitor page with live CPU, memory, disk and network use for the host and every VM.
- Favorites, folders, tags, search, command palette, keyboard navigation and an activity log.
- Omarchy, dark, light and hacker themes, larger text and reduced motion.
- Only one OmaWare can be open at a time.

### Installing and updating
- The Linux package installs with `./install.sh` (your home folder only: the app, an `omaware` command and a menu entry).
- Built-in updates from GitHub Releases: OmaWare checks once a day (can be turned off), downloads and verifies the new version in the background, and restarts into it in a moment without stopping your VMs. The previous version is kept.

[Unreleased]: https://github.com/iop6/Omaware/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/iop6/Omaware/releases/tag/v1.0.0
