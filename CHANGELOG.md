# Changelog

## 1.4.0 (2026-10-01)

- Eight additional MCP tools for VM details, readiness checks, diagnostics, file transfers, CPU/RAM changes, cloning, local ISO media and network adapters.
- Agent file transfers use a dedicated host transfer folder rather than arbitrary host paths; VM ownership and containment restrictions still apply.

## 1.3.0 (2026-10-01)

- AI agents (such as Claude Code) can use OmaWare when you turn it on in Settings: build labs of networks and VMs from a description, use VM screens, run commands in lab VMs, take and restore snapshots, and plug or pull network cables.
- Labs are built from official cloud images (Ubuntu Server LTS, Debian, Fedora Cloud), set up on first boot with a login you choose. You approve every lab and set its password in OmaWare; the agent never sees it.
- Logins are kept in your system's password store, and shown on a lab VM's Details page.
- Letting VMs join several new networks asks for your password once.
- Security review (see SECURITY.md): clipboard sharing no longer carries over to another VM's console; containment also blocks vsock and TPM passthrough; names are always shown as plain text.

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
