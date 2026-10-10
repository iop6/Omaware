# Changelog

## 1.12.0 (unreleased)

- Windows VMs no longer get stuck at a 1280×800 screen. Windows has no driver for QEMU's display adapters and keeps the size the firmware booted with, so new Windows VMs now tell the firmware to start at 1920×1080, and **Edit hardware → Integration** sets the boot screen size of any VM (applied at its next start, listed in Pending changes). Installers whose windows were taller than the screen, such as FLARE-VM's package selection, now fit.

## 1.11.0 (2026-10-09)

- Starting a VM whose network is stopped (after a reboot, for example) now starts the network first, with the same administrator prompt as **Start** in Networks, instead of failing with libvirt's "Unable to restore from managed state … Maybe the file is corrupted?" or a bridge error. Networks made outside OmaWare, and bridges no network defines, are reported by name instead. This applies to VMs started from the sidebar, the map, **Resume all**, a restart, an unattended setup and AI agents.
- AI agents: `type_login` only types the login the VM's own lab was built with; the undocumented `login` argument that could name any saved login is gone, and every tool now refuses arguments its description doesn't list. The tool's description says that the password is typed wherever the cursor is.
- Appliances: OVAs that put the manifest after the disk (VirtualBox exports) are imported and checked, with SHA1, SHA256 or SHA512 manifests.
- ISOs you add from elsewhere on the same disk (which OmaWare links instead of copying) are now offered to AI agents' `create_vm` too.
- A cloud image, OS Shop download or update whose file couldn't be written completely (a full disk) is reported and thrown away instead of being kept with a matching checksum.
- Snapshot and agent operations recover after the local libvirt daemon restarts, instead of failing until OmaWare is reopened.
- The updater only fetches packages and checksums over HTTPS, whatever the release feed says.
- The installer no longer treats a missing terminal as a yes (`--yes` says so explicitly) and works without `$USER` set.
- Faster network listing with many VMs: each VM's definition is read once per listing instead of once per network, and containment checks read the host's bridges once.

- **Containment** closes more ways out of a contained VM: raw QEMU settings, disks other than local image files (network storage, host block or NVMe devices, host folders, SCSI passthrough), serial ports and channels that reach host devices or services, keyboard and input passthrough, smartcards, host audio, network entropy sources, 3D acceleration, and VNC's default local TCP port. A VM that finishes setting itself up is checked before it starts.
- Everything OmaWare shows from guests, files, AI agents and other programs is plain text, so markup in a file name or an agent's request can't change the window or disguise what you approve.
- AI agents ask before plugging a cable into anything but an isolated or host-only network OmaWare created. The agent connection is only ever created in your private runtime folder.
- Cleaner code throughout: the VM operations, snapshots, agent tools, OS Shop and updater are split into smaller named parts, the C++ and QML are consistently formatted, unused code is removed, and the documentation is updated.

## 1.9.0 (2026-10-04)

- The ISO Shop is now the **OS Shop**, with a new look: a spotlight for updates and favourites, a shelf per category, cards in each system's colors that show how the download is checked, a details panel, a downloads tray, and a storage bar over **Your media**. Press `/` to search.
- New in the shop: Parrot Security, Security Onion, CAINE, Tsurugi Linux, a ready-made **Kali Linux VM** (downloaded, checked and unpacked into `appliances/`, then **Import VM**), and Windows 11 Enterprise and Windows Server 2025 evaluation copies.
- Earlier versions where the publisher still offers them (Ubuntu LTS releases, the previous Debian and Fedora, Rocky Linux and AlmaLinux 8–10, recent Mint, FreeBSD and OPNsense releases). A version you pick on purpose is kept and never offered for deletion as an older version; pin or unpin any file.
- Downloads whose publisher gives no SHA-256 or SHA-512 checksum are labelled **Unverified**, with the reason (Microsoft's evaluation copies have none; Parrot publishes only MD5, still used to catch damaged downloads). Your media shows how each file was checked.
- **Set it up for me** when creating a VM: Windows 11 (and the Enterprise and Server evaluation copies) and Ubuntu Server install without questions, with the user name and password you give. Ubuntu gets the QEMU guest agent and sudo; OmaWare takes out the installation media afterwards and starts the new system.
- AI agents can list the OS Shop and download from it (`get_media`). Every download asks you first, and agents can only pick the shop's own systems.
- AI agents: `create_vm` takes the VM's network adapters as an ordered list that can include the private internet connection (`"user"`), so a router's WAN can be its first network card in one request.
- AI agents can start, stop, edit and delete networks OmaWare created, and choose whether they start with the computer (`manage_network`). Networks agents create now start with the computer by default, like the ones made in OmaWare.
- An adapter change that can't apply to a running or paused VM can ask for a clean restart in the same approval. OmaWare never forces the VM off and reports a timeout when the guest doesn't shut down.
- Allowing VMs on a network says why it failed (password prompt closed or refused, a deny rule, the network not running, the helper missing or not owned by root), and agents get the same reason as a code. A missing helper comes with the `sudo install` command for your copy of OmaWare; cmake is no longer needed.
- `run_command` works on VMs on isolated networks through the QEMU guest agent, and says what to do when no agent is running. The overview shows addresses the guest agent reports (for VMs behind a guest router), where each address came from, and which VMs need a restart for saved changes.
- Every change an agent makes returns the new revision; `diagnose_vm` gives hints such as a guest with no IPv4 address on any network card.
- AI agents can use a VM's serial console as text (`serial_console`, and `type_login` over serial): routers like pfSense can be configured without a guest agent, a network or typing on the screen.
- Optional session approval: tick **Don't ask again** on an agent's adapter change for isolated and host-only networks, and such changes stop asking until agent access is turned off (at most 8 hours). The banner shows it, with **Revoke**.

## 1.8.1 (2026-10-03)

- Fixed OmaWare not starting again after an update installed from the app when no VMs were running.

## 1.8.0 (2026-10-03)

- Network map cables run like circuit-board traces: straight runs with rounded corners, a port of their own at both ends, nested so they don't cross, and never through another device. Ports have link lights that flicker with traffic.
- Fixed the diagonal line from This computer to its network, and the whole map shifting when you clicked or dragged a device.
- Network map look: outlined, glowing cables, shaded zones with title tabs, hover highlights and breathing power lights. Drag the **+** on a VM's side onto a network to connect it.
- 15 built-in themes, including Tokyo Night, Catppuccin, Nord, Gruvbox, Rosé Pine, Dracula, Everforest, Kanagawa, Solarized Light, Synthwave and Graphite, chosen from previews in Settings. Follow Omarchy now uses your Omarchy theme's own colors for errors, warnings and running VMs.
- Built-in Help: press F1, click Help in the sidebar or a **?** button, or type `help` in the command prompt. The screens themselves have much less text.
- Deleting a VM whose snapshots build on each other now deletes its snapshots too; before, their folder was kept.
- Fixed a crash on Qt 6.4 (Ubuntu 24.04) when the theme changed while the window was resizing.

## 1.7.1 (2026-10-02)

- Network map cables look cleaner: each is a straight line into its own port on its network (no more cables merging into one line), pulled cables hang loose with an unplugged plug instead of a dashed line across the map, cable lights are small LEDs, and traffic packets glow with short trails.
- Devices on the network map snap to a 40 px grid when dropped, and Tidy up uses the same grid.

## 1.7.0 (2026-10-02)

- Network map: cables run in straight lines with square corners, routed around devices.
- Live traffic per cable: each cable shows its own adapter's packets and glows when busy; hover its light for upload/download rates, packets, errors and a short history graph.
- Select a VM to see everything it can reach light up while the rest fades.
- **Trace a ping to** walks a ping hop by hop with an explanation at each step, stopping where it would be blocked (simulated from your settings; no packet is sent).
- Export the map as PNG, SVG or Mermaid; zone titles; a minimap when the map doesn't fit; an operations-center map style.
- Lab files: export a lab as TOML and import one for review and building, even with AI agent access off.
- The sidebar's Command button is gone; the command prompt still opens with `:` or Ctrl+Shift+P.

## 1.6.0 (2026-10-02)

- **Delete VM…** in a VM's right-click menu (and its Details menu) deletes the VM with its disks, all its snapshots, restored disks, pending changes and its UEFI/TPM state, after you type its name. Running VMs are powered off first. Installation media, disks OmaWare didn't make for the VM and disks another VM uses are kept. Agents can't delete VMs.

## 1.5.2 (2026-10-01)

- Fixed Windows 11 (UEFI) VMs failing to create with "Unable to find 'efi' firmware that is compatible with the current configuration" on systems whose UEFI firmware has no preloaded Microsoft keys, such as Arch. OmaWare now picks Secure Boot firmware from what the system actually has: with Microsoft's keys if available, otherwise without them, otherwise plain UEFI.

## 1.5.1 (2026-10-01)

- Fixed AI-agent `create_vm` failing right after approval with "Approved provisioning media, arguments or network identity changed." for every request.
- Agent provisioning errors now give the actual reason when the approved media or storage can't be prepared.

## 1.5.0 (2026-10-01)

- Background MCP provisioning from confined local ISO and qcow2/raw appliance libraries, with stopped independent VM imports and up to four owned network destinations.
- Approval-gated network creation and bridge authorization, immediate operation status and bounded session-local request deduplication; fresh worker access, media and ownership checks.
- OVA and QCOW2 appliances: listed as *Appliance disk* in the ISO Shop's media list, added by drag and drop or **Add files…** into `appliances/`, and always routed to disk import. OVAs with one VM and one VMDK (plain or gzip-compressed) are unpacked once into private staging beside the VM storage, checked against their manifest, and converted into an independent stopped qcow2 VM; only the disk is imported and the OVF's hardware suggestions are reported, not applied.
- REMnux in the ISO Shop, linking to its official virtual appliance page (no automatic download or invented checksum).
- Host-safe schema, media-confinement, request-envelope, replay and protocol regression coverage; no live VM provisioning implied by these tests.

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
