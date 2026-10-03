# Changelog

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
