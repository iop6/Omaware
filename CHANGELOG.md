# Changelog

## 1.2.1 (2026-10-01)

- Simpler install: the one command asks at most once, skips anything already set up, and no longer needs you to log out and back in.
- The install command also sets up the helper that lets VMs join networks you create.

## 1.2.0 (2026-10-01)

- Install with one command (see the README).
- Windows 11 in the ISO Shop, straight from Microsoft in your language and checked against Microsoft's checksum.
- Windows VMs get UEFI with Secure Boot, a TPM 2.0 chip and hardware Windows recognizes; snapshots keep the TPM.
- Downloads pick up where they stopped when the connection drops or stalls.
- Fixed: the snapshot tree could show an old state after a restore.

## 1.1.0 (2026-09-30)

- Drag ISO files onto the window to add them to your ISOs.
- A button in the sidebar checks GitHub for a new version.
- Updating asks first and warns that running VMs will be paused.
- Closing OmaWare always pauses running VMs.
- Fixed: Create VM's ISO list now shows the ISO you picked.

## 1.0.0 (2026-09-30)

- First release: create and run VMs, built-in console, snapshots, network map, ISO Shop and built-in updates.
