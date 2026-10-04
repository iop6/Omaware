# OmaWare user guide

- [Installing](#installing)
- [The window at a glance](#the-window-at-a-glance)
- [Creating a VM](#creating-a-vm)
- [The VM list](#the-vm-list)
- [Using the console](#using-the-console)
- [Details: overview, hardware and networks](#details-overview-hardware-and-networks)
- [Changing hardware](#changing-hardware)
- [The network map](#the-network-map)
- [Snapshots](#snapshots)
- [The OS Shop](#the-os-shop)
- [Monitor, log and command prompt](#monitor-log-and-command-prompt)
- [AI agents and labs](#ai-agents-and-labs)
- [Updating OmaWare](#updating-omaware)
- [Settings and themes](#settings-and-themes)
- [Closing OmaWare](#closing-omaware)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Where OmaWare keeps its files](#where-omaware-keeps-its-files)

Networking has its own guide: [NETWORKS.md](NETWORKS.md).

## Installing

```sh
curl -fsSL https://github.com/iop6/Omaware/releases/latest/download/install.sh | bash
```

This one command does everything, asking for your password once and only if something is missing:

- installs QEMU/KVM, libvirt, `virt-install`, UEFI firmware, `swtpm` (for Windows 11) and Qt 6 from your distribution, on Ubuntu/Debian, Fedora, Arch/Omarchy and openSUSE;
- turns on libvirt and gives you access to it (OmaWare picks up the access when it starts, so there's no need to log out);
- downloads the newest release, checks it against its published checksums, and installs it in `~/.local/share/omaware/app` with an `omaware` command and an app-menu entry;
- installs the small root-owned helper that lets VMs join networks you create (`/usr/local/libexec/omaware/authorize-bridge`).

Running it again reinstalls; `--dry-run` shows what it would do. Updates come through OmaWare itself (see [Updating OmaWare](#updating-omaware)).

**By hand:** download `omaware-<version>-linux-x86_64.tar.gz` from the [Releases page](https://github.com/iop6/Omaware/releases/latest), unpack it and run its `install.sh`. Install the packages above yourself and add yourself to the `libvirt` group (`sudo usermod -aG libvirt "$USER"`).

**From source:** see [Building](DEVELOPMENT.md#building).

**If the window stays blank** on a machine without working GPU drivers, start OmaWare with `QT_QUICK_BACKEND=software omaware`.

## The window at a glance

- **Sidebar (left):** navigation (Virtual machines, Monitor, Command, Networks), the VM list, **Create VM**, **Settings**, **Help** and the theme button.
- **Workspace (right):** the selected VM with three tabs: **Console**, **Details** and **Snapshots**.
- **Status line (bottom):** current mode, connection state, host CPU and memory, number of running VMs, snapshot jobs, the log and the time.

**Help** (F1) shows this guide and the networks guide inside OmaWare, as searchable topics. The **?** buttons around the app open the topic about what's next to them, and typing `help` in the command prompt finds any topic.

OmaWare works with VMs in your own libvirt session (`qemu:///session`). It only changes VMs it created itself. Other VMs in the session are shown read-only.

## Creating a VM

Click **Create VM**.

1. Enter a name and choose where the system comes from:
   - **An installation ISO.** Choose one from your ISO library, get one from the **OS Shop**, browse to a file, or drag an ISO onto the window. OmaWare creates a new empty disk and attaches the ISO. Picking a known ISO also picks the matching operating system and a name.
   - **An existing disk image or appliance** (OVA, qcow2 or raw). OmaWare copies it into a new, independent qcow2 disk and leaves the VM stopped. The original file is never changed. Picking an OVA or qcow2 anywhere (your media list, **Browse file…**, or dropping it on the window) switches to this mode automatically; appliances are never attached as installer ISOs. See [Appliances (OVA and QCOW2)](#appliances-ova-and-qcow2).
2. Choose the operating system type, so the VM gets sensible default devices.
3. Optional: open **Advanced setup** for processors, memory, disk size, network, storage location and BIOS or UEFI firmware. UEFI needs firmware installed on the host.
4. Review and create. The new VM starts out stopped; start it to run the installer.

After installing from an ISO by hand, shut the VM down and use **Details → Hardware → Edit hardware** to eject the ISO and boot from the disk first. With **Set it up for me** (below), OmaWare does that for you.

The small arrow next to Create VM also offers a **diskless test VM**: a tiny VM with no disk or network, handy for trying things out. It shows "No bootable device", which is expected.

**Windows:** pick a Windows ISO and OmaWare sets the VM up the way Windows expects: UEFI firmware with Secure Boot (when your computer has that firmware), a SATA disk and network card that Windows recognizes without extra drivers, and **Add a TPM 2.0 chip**, which Windows 11 requires. The TPM needs the `swtpm` package on your computer; snapshots keep the TPM's contents along with the disks. You need your own Windows license to activate it.

### Set it up for me

For some installers, the create dialog offers **Set it up for me** (on by default): enter a user name and a password, and the installer runs without asking anything. It works with:

- **Windows 11** from Microsoft (installed as Windows 11 Pro without a product key; activate it with your own license), **Windows 11 Enterprise** and **Windows Server 2025** evaluation copies. Windows creates your account as a local administrator (no Microsoft account or internet needed); Windows Server's Administrator gets the same password, which must be complex. It takes 20–40 minutes and restarts a few times. When you first shut the VM down afterwards, OmaWare takes out the installation media. Windows Server ignores **Shut down** until someone has signed in: sign in and shut it down from its Start menu.
- **Ubuntu Server.**

Ubuntu gets your account as an administrator (sudo), the QEMU guest agent (so `run_command` and other agent tools work) and OpenSSH. It uses your computer's time zone and language, and its keyboard layout where OmaWare can tell. When its installer is done it switches the VM off; OmaWare then takes out the installation media and starts the new system, usually 10–20 minutes after you first started it.

How it works: OmaWare writes the answers for the installer onto a small extra disc in the VM's private folder (`setup.iso`). Windows reads `autounattend.xml` from it, and Ubuntu reads its autoinstall settings from a `cidata` disc; for Ubuntu, the first start boots the installer straight away with `autoinstall`, so it doesn't stop to ask "Continue with autoinstall?". Ubuntu gets only a hash of the password; Windows needs the password itself, so it's on that disc until the setup is done and OmaWare deletes the disc. If you force a VM off while it's installing, the answers stay so the next start can try again. Turn **Set it up for me** off to answer the installer's questions yourself.

## The VM list

Each row shows the VM's state (● running, ‖ paused, ○ stopped, ✕ crashed), live CPU use, a small CPU graph and tags: `contained`, `read-only`, `test`, `auto-paused` and up to two of your own tags.

- **Search** (Ctrl+K) matches names, display names, tags and folders. **All / Active / Stopped** filter the list.
- **Right-click** a VM (or use its `⋯` button, the Menu key or Shift+F10) for everything you can do with it: open the console, Details or Snapshots; start, pause, resume, shut down or force off; take or revert a snapshot; **Show on network map**; favorite; **Rename & organize**; containment; copy its name or UUID; remove it.
- Power and organize actions from this menu don't switch the console away from the VM you're looking at.
- **Rename & organize** sets a display name, folder, tags, notes and favorite. This is only stored in OmaWare; the VM itself keeps its real name.
- Favorites come first, then folders (alphabetically), then everything else. Click a group heading to fold it.
- **Remove definition** (stopped VMs only) removes the VM from libvirt. Its disk files are kept.
- **Delete VM…** deletes the VM for good: it's powered off if needed, then its disks, all its snapshots, restored disks, pending changes, and its UEFI firmware variables and TPM state are deleted. Type the VM's name to confirm. Installation ISOs, appliance files, disks OmaWare didn't make for this VM, and any disk another VM still uses are kept, and the result says which. AI agents can't delete VMs.
- **Ctrl+B** shrinks the sidebar to a narrow strip of icons.

### Selecting several VMs

Ctrl+click adds or removes a VM from the selection, Shift+click selects a range, Ctrl+A selects everything listed and Esc clears it. A bar appears above Create VM:

- **Turn on:** starts stopped VMs and resumes paused ones.
- **Pause**, **Shut down** (asks each guest OS to shut down) and **Force off** (asks for confirmation first).
- **New network…:** creates a network and connects the selected VMs to it.

Each button shows how many of the selected VMs it would change; VMs already in that state are skipped. The command prompt also has **Resume all paused VMs** and **Pause all running VMs**.

## Using the console

Selecting a running VM opens its screen in the **Console** tab.

- **Click inside** the screen to send your keyboard and mouse to the VM. **Ctrl+Alt** gives them back. An accent border shows when the VM has your input.
- The console toolbar has **Snapshot**, **Revert**, input release, clipboard, fullscreen and a tools menu.
- **Fullscreen** gives the whole window to the VM. Move the mouse to the top edge to show the toolbar. Esc leaves fullscreen once input is released.
- The **tools menu** has focus view, detaching the console into its own window (close that window to dock it again), **Send keys** (for example Ctrl+Alt+Delete), **Display size** requests and connection controls.
- **Clipboard:** off by default. Choose to share text **To guest** or **Both directions**. This needs the clipboard channel enabled in the VM's hardware and `spice-vdagent` running inside the guest. Text is limited to 1 MiB.
- Resizing the VM's display to fit the window depends on the guest's display driver.

## Details: overview, hardware and networks

- **Overview:** processors, memory, uptime, disks and network at a glance, with live CPU, memory, disk and network charts while the VM runs. **Technical details** has firmware, identity and management information.
- **Hardware:** disks and installation media (with sizes and file locations), display, input and integration devices.
- **Networks:** network adapters and what they're connected to. Changes apply to a running VM straight away when its OS supports it. **Refresh guest IPs** asks the QEMU guest agent inside the VM for its addresses; without a running agent, OmaWare says why none are shown rather than guessing. The network map also shows IP addresses it can find without the agent.
- **Containment:** see [NETWORKS.md](NETWORKS.md#containment).

Values can be selected and copied.

## Changing hardware

**Details → Hardware → Edit hardware** changes processors, memory, CPU mode, boot order, the installation ISO and the clipboard channel. **Add disk** creates an extra disk; detaching a disk keeps its file. Network adapters are edited under **Details → Networks**.

Changes to a **running** VM are saved for its next start; it keeps its current hardware until then. A banner lists each pending change with its old and new value:

- **Discard** one change, or **Discard all changes**.
- **Shut down & start** asks the guest to shut down, waits up to two minutes, then starts it with the new settings. It never forces the VM off. **Cancel automatic start** stops the restart if you change your mind.
- Restarting from inside the guest is not enough: the VM must fully stop and start.

Pending changes are remembered if you close OmaWare. If something else changes the VM's configuration in the meantime, OmaWare notices and asks you to review.

## The network map

**Networks** (Ctrl+3) shows every VM, network, this computer and the internet as a map. Cables run like traces on a circuit board, from a port on the VM to a port of their own on the network, without crossing through other devices, and are colored by where they lead (amber: the internet, blue: this computer only, green: other VMs only). Each port has a link light. A pulled cable hangs loose from its VM with a red unplugged plug, and a cable waiting for the VM's next start is gray and dashed. Drag devices anywhere: they snap to the map's grid when you let go, so rows and columns line up.

- **Connect and disconnect:** drag the + on the side of a VM onto a network (or onto This computer for a private internet connection). Double-click a cable's port on the VM to pull or plug it; right-click it to move or remove it. **Cut off internet** pulls every cable with a way out.
- **Live traffic:** while a cable carries data, packets travel along it (up toward the network for uploads, down toward the VM for downloads), more and faster the busier it is, and the cable glows. Each cable shows its own adapter's traffic.
- **Hover a cable's port on the VM** for its upload and download rates, packets per second, errors or dropped packets, and a graph of the last couple of minutes.
- **Select a VM** to see what it can reach: its networks, the other running VMs on them, this computer and the internet light up, and everything it can't reach fades. The side panel says the same in words.
- **Trace a ping:** right-click a VM, **Trace a ping to**, and pick the internet, this computer or another VM. An envelope walks the path one hop at a time (**Next**, **Back** or **Play**) and each step says what happens there: the virtual switch, this computer's NAT router, and so on. Where the ping would be stopped — a pulled cable, a VMs-only network, a stopped network, a VM that's off, or two networks that don't connect — it stops there and says why. It's worked out from your settings; no packet is sent.
- **Zones:** each network and its VMs sit in a shaded, titled area such as *Internet-connected* or *Isolated · VMs only*.
- **Getting around:** drag the background to pan, scroll or use − / + to zoom, **F** to fit, and the grid button to tidy everything up. When the map doesn't fit, a minimap in the corner shows where you are; click or drag in it to jump.
- **Operations-center style:** the monitor button (or right-click the background) switches to a darker map where live cables glow. OmaWare remembers your choice.
- **Export:** the download button saves the map as a **PNG** image, an **SVG** drawing or a **Mermaid** diagram (for docs and wikis), or copies it as Mermaid.

### Lab files

A lab can be saved as a file (TOML) and built again later or on another computer: right-click the map's background and choose **Export a lab**, or use **Export lab file…** under *Lab login* in a lab VM's Details. The file lists the lab's networks (type `internet`, `private` or `isolated`, and subnet) and VMs (operating system, CPUs, memory, disk, networks with optional fixed addresses, packages and first-boot commands). It never contains passwords or keys, and you can edit it in any text editor.

**Import lab file…** (right-click the map's background) checks the file and shows the plan for review, exactly like a lab an agent proposes: you set the VMs' password and click **Build**. This works with AI agent access turned off.

## Snapshots

A snapshot saves a VM's disks and settings, and for a running VM also its memory, so you can return to that exact moment later.

### Taking a snapshot

Click **Take snapshot** in the Snapshots tab, or **Snapshot** in the console toolbar. It gets a timestamped name you can change.

- **Running or paused VMs** are saved with their memory by default. The VM freezes for a moment while its memory is written, then carries on while the disks are copied in the background.
- **Stopped VMs** save disks and settings only.
- **More options** has notes, tags, **Pin against deletion**, a **Known good** label, whether to include a console preview, and for disk-only snapshots of a running VM, incremental storage and a guest filesystem flush.

Snapshots with memory need free space for the VM's disks plus its memory. Disk-only snapshots of a running VM can be **incremental**: after the first full copy, only changed blocks are saved.

### The snapshot tree

Snapshots are shown as a tree, each connected to the one it was taken from. **You are here** is the VM as it is now, connected by a dotted line to the snapshot it's based on (marked **Current snapshot**).

For example: take **A**, then **B**. Revert to **A**, make changes and take **C**. The tree now has two branches, A → B and A → C, and you can go back to either.

Drag or scroll to move around, **Fit** shows the whole tree, and the arrow keys step between snapshots. Clicking a snapshot only selects it.

### Going back to a snapshot

- **Revert here…** on a snapshot returns the VM to exactly that moment. For a snapshot with memory, programs carry on where they were without rebooting, even if the VM is currently stopped. The console reconnects for a moment. It's almost instant, however big the disks are.
- **Revert…** in the toolbar goes back to the VM's current snapshot.
- **Disk-only** snapshots have no memory to return to, so the VM restarts from the saved disks.
- Reverting discards changes made since that snapshot.
- **Undo last restore…** (in the **…** menu) is only available when a restore saved a recovery point first. Reverting from the interface doesn't, so take a snapshot before reverting if you might want to come back.

### Other snapshot actions

Right-click a snapshot (or use its **…** button):

- **Rename** (also F2 or double-click) and **Edit notes & options…**
- **Details & preview…:** its preview image, parent, size, verification, notes and tags.
- **Create VM from snapshot…:** a new, separate VM with copied disks, a new identity and network cables unplugged. The guest's accounts and hostname are copied as they are.
- **Verify stored files…:** checks the snapshot's files for damage and records checksums, so later changes are detected. It can't repair anything.
- **Delete this snapshot…** or **Delete this branch…**. Deleting a snapshot keeps its children, reconnected to the snapshot above it. Pinned snapshots need an extra confirmation. Your current VM is never changed by deleting snapshots. If the current disks depend on the deleted snapshot, OmaWare first merges the needed data into them, which can take a while for big disks.

The **…** menu at the top has **Undo last restore…**, **Snapshot storage…** (space used, and **Remove unused files…** for leftovers that are safe to delete) and help.

### Snapshot jobs

Copying and verifying run in the background. **Jobs** in the status line shows progress, speed and time left, and most phases can be cancelled. You can keep using OmaWare meanwhile. If OmaWare is closed during a job, the job is cancelled and any half-finished files are cleaned up or offered for recovery next time.

Snapshots are not backups: they live on the same disk as the VM.

## Monitor, log and command prompt

- **Monitor** (Ctrl+2) is an htop-style view: host CPU and memory, then one row per VM with CPU, memory, disk and network use and uptime. Click a column heading to sort; Enter opens a VM's console.
- **Log** (Ctrl+`) slides up a list of every operation. Type to filter, show only errors, copy lines, or open the full **activity history** (the last 200 operations, kept across restarts).
- **Command prompt** (`:` or Ctrl+Shift+P; there's no sidebar button) searches every action and VM. Type part of a name, pick with the arrow keys and press Enter.

## The OS Shop

**OS Shop** in the sidebar (Ctrl+4) offers free, official operating systems: installer ISOs, and a few ready-made VMs that need no installing. The top of the page spotlights updates for your media and a few favourites; under it is the catalogue, one shelf per category:

- **Desktop:** Ubuntu, Kubuntu, Xubuntu, Linux Mint, Fedora Workstation, Fedora KDE Plasma, Debian, Arch Linux, openSUSE Tumbleweed, Pop!_OS and NixOS.
- **Server:** Ubuntu Server, Rocky Linux, AlmaLinux, Alpine Linux, Proxmox VE and FreeBSD.
- **Security & networking:** Kali Linux, the ready-made **Kali Linux VM**, Parrot Security, Security Onion, CAINE, Tsurugi Linux, OPNsense, and REMnux and pfSense CE from their publishers' websites (see below).
- **Windows:** Windows 11 straight from Microsoft, in the language you choose (OmaWare picks your computer's language at first), checked against the SHA-256 checksum Microsoft publishes for that language. Also **Windows 11 Enterprise** and **Windows Server 2025** evaluation copies (free for 90 and 180 days), which are *unverified* (see below). Microsoft sometimes refuses automated downloads from some networks or after many attempts; the card then says so and offers **Website**, and a Windows ISO you download there is recognized too.

Filter by category, or press `/` and type to search. Click a card (or its ⓘ button) for its details: the versions on offer, the language, the file name, its checksum and your copies of it.

**Download** saves the file into `~/.local/share/omaware/isos/`. Downloads keep going in the background, in the tray at the bottom of the page and in the sidebar, and can be cancelled. Each card says how its download is checked:

- **Checked:** the download is compared with the publisher's own SHA-256 (or SHA-512) checksum and thrown away if it doesn't match, so you get exactly the file the publisher released. Compressed images (OPNsense) are unpacked after they're checked.
- **Unverified:** the publisher gives no such checksum. Microsoft publishes none for its evaluation copies, so only the secure (HTTPS) connection to Microsoft vouches for them. Parrot publishes only MD5 checksums, which OmaWare still uses to catch a damaged download but which can't prove the file is Parrot's. The details panel says why for each.

**Earlier versions:** where the publisher still offers them, a card's details let you pick an earlier version, such as an older Ubuntu LTS, the previous Debian or Fedora release, or Rocky Linux and AlmaLinux 8, 9 or 10. OmaWare remembers your pick. A version you download on purpose is **kept**: it's never offered for deletion as an older version. Keep or un-keep any file with its pin button.

**Ready-made VMs** (the Kali Linux VM) are a finished VM disk instead of an installer. OmaWare checks the download, unpacks the disk into `~/.local/share/omaware/appliances/`, and its button says **Import VM**: that makes a new, stopped VM from a copy of the disk, as for any appliance (see below).

**Your own files:** drag ISO, OVA or QCOW2 files from your file manager onto any part of the OmaWare window, or use **Add files…**. ISOs go into your ISO folder (a file on the same disk is added instantly and takes no extra space; anything else is copied with progress and **Cancel**), appliances into `appliances/`. Your original file stays where it was. Drop a single file and the create dialog opens with it chosen; drop it while that dialog is open and it switches to that file.

When a newer release is out, its card says **Update**, the spotlight shows it, and **Your media** marks the older file. **Your media** lists every file with how it was checked (or *added by you*), and a bar shows how much space your media takes next to what's left on the disk.

Deleting files is quick, and always asks once in place before anything is removed:

- **The trash button on a card** deletes your copies of that system.
- **Delete older versions** in Your media removes every file a newer download has replaced (kept files stay), and shows how much space that frees.
- **Tick several files** (or **Select all**) and use **Delete selected**. Each file also has its own trash button.

**New VM** (or **Import VM**) on a card or file opens the create dialog with that file chosen, the matching operating system selected (when your computer knows it) and a name suggested. OmaWare only contacts the publishers when you open the shop or press **Check for updates**. AI agents can get media from the shop too, but only after you approve each download (see [AI agents and labs](#ai-agents-and-labs)).

### Appliances (OVA and QCOW2)

Some systems ship as a ready-made virtual appliance instead of an installer. **Your media** lists these as *Appliance disk* (installers are *Installer ISO*), and their button says **Import VM** instead of **New VM**. Add one with **Add files…** or by dropping it on the window: it is copied into `~/.local/share/omaware/appliances/` (never hard-linked, so the library copy is yours alone).

The **REMnux** card links to the [official REMnux virtual appliance page](https://docs.remnux.org/install-distro/get-virtual-appliance). OmaWare doesn't download REMnux itself or claim a checksum for it: download the OVA there, verify it as the page describes, then add it. A file named like `remnux-noble-amd64.ova` is recognized as REMnux.

Importing an OVA:

- Reads it once into a private folder next to the new VM's storage (an appliance can unpack to tens of GiB, so make sure that drive has room for the unpacked disk plus its copy), then converts it into an independent qcow2 disk. The VM is created **stopped**.
- Supports OVAs with one virtual machine and one VMDK disk (monolithicSparse or streamOptimized, plain or gzip-compressed, as exported by VMware and VirtualBox). OVAs with several disks or VMs, split (chunked) disks, external or parent disks, links, folders or unexpected files are refused.
- Checks the disk against the OVA's manifest (`.mf`) checksums when there is one. That catches a damaged download; it does not prove who made the file. A signing certificate, if present, is not checked.
- **Imports only the disk.** CPU and memory come from the create dialog (the OVF's suggestion is shown when the VM is created), and the OVF's network adapters, disk controllers, sound and other devices are not copied. Choose the matching firmware (most VMware/VirtualBox appliances, including REMnux, use BIOS) and an isolated network for malware analysis.

## AI agents and labs

OmaWare can be used by an AI agent, such as [Claude Code](https://claude.com/claude-code). You describe what you want ("two networks with a firewall VM between them, three Ubuntu clients on one side and a web server on the other, no internet") and the agent builds it and works with it: it can see your OmaWare VMs and networks, build **labs**, look at and use a VM's screen, run commands inside lab VMs, take and restore snapshots, and plug or pull virtual network cables.

### Turning it on

Agent access is off until you turn it on: **Settings → AI agents → Let AI agents use OmaWare**. Settings then shows the command that connects Claude Code, with a **Copy** button:

```sh
claude mcp add omaware -- ~/.local/bin/omaware mcp
```

`omaware mcp` is a small helper the agent starts: it speaks the Model Context Protocol (MCP) that agents use, over its standard input and output, and passes each request to the open OmaWare window. Other MCP-capable agents work the same way. OmaWare has to be open for the agent to use it. Turning the setting off disconnects agents at once.

If you start OmaWare through a launcher script of your own, make sure `mcp` (like `--version`) runs in the foreground with its input and output attached, not detached into a log file; otherwise the agent waits for an answer that never comes. To check, `printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}\n' | ~/.local/bin/omaware mcp` must print one line of JSON.

### Agent tools

The MCP interface exposes focused VM-management tools rather than unrestricted commands on your host computer. Ask your agent to start with `omaware_overview` and use `vm_details` to check a VM's configuration and supported operations.

| Tool | What it does |
| --- | --- |
| `omaware_overview` | Lists OmaWare VMs (with IP addresses and where each came from, and changes waiting for a restart), networks, labs and cloud images. |
| `vm_details` | Inspects a VM's configuration and agent capabilities. |
| `diagnose_vm` | Collects structured diagnostics for a VM. |
| `wait_for_vm` | Waits for a requested readiness condition with a timeout. |
| `vm_power` | Starts, shuts down, pauses, resumes or restarts a VM, or forces it off. |
| `screenshot`, `vm_input` | Captures and operates a VM's screen. |
| `type_login` | Enters a saved login, on the screen or the serial console, without returning its password to the agent. |
| `serial_console` | Types on a VM's serial console and returns what the guest printed, as text. For routers and appliances without a guest agent. |
| `run_command` | Runs a command inside the guest, not on the host: through the QEMU guest agent on any network, isolated ones included, or over SSH for lab VMs. |
| `transfer_file` | Transfers files between the guest and a dedicated host transfer folder. |
| `update_vm_resources` | Previews or changes a VM's CPU and RAM allocation. |
| `clone_vm` | Creates a separate working copy of a VM. |
| `manage_iso` | Lists local installation media and attaches or ejects it. |
| `get_media` | Lists the OS Shop's systems (versions, languages, how each download is checked) and downloads one from its publisher. Every download asks you first, showing the name, version, size and whether it's checked or unverified; there's no "Don't ask again" for downloads. Agents can only pick the shop's own systems, never a web address. |
| `manage_network_adapter` | Inspects and changes a VM's network adapters; can also restart the VM cleanly when a change can't apply while it runs. |
| `manage_network` | Starts, stops, edits or deletes a network OmaWare created, or sets whether it starts with the computer. |
| `set_cable` | Plugs or pulls an existing adapter's virtual cable. |
| `list_snapshots`, `snapshot_vm`, `restore_snapshot` | Lists, saves and restores snapshots. |
| `propose_lab`, `lab_status`, `delete_lab` | Proposes, tracks and removes labs. |
| `list_installation_media`, `list_owned_networks` | Lists safe local ISO/appliance filenames and owned network UUIDs/revisions without selecting an existing VM. |
| `create_vm`, `create_network`, `authorize_network` | After approval, creates a stopped VM from local media (with its adapters in the order given, including the private internet connection), creates/starts an owned network, or authorizes its bridge through the existing administrator helper. |
| `provision_status` | Observes a background provisioning request; repeated IDs do not create duplicate work during the same app session. |

The agent should use the server's `tools/list` response for exact arguments and limits. A successful MCP connection is not proof that OmaWare is reachable: the app must be open and agent access enabled before VM tools can work. A powered-on VM may still be booting; use readiness checks before sending commands. For a VM's screen, `screenshot` and `vm_input` are reliable while OmaWare shows the console; `virsh send-key` and `virsh screenshot` can hang then.

Every change an agent asks for answers with the VM's or network's new `revision`, so it can make the next change without listing everything again. When something fails, the answer has a stable `code` the agent can act on (for example `helper_missing` or `authorization_cancelled` when allowing VMs on a network), and OmaWare's activity log has the full story.

Local-media provisioning does not need the agent to click or focus the desktop. User approval still happens in OmaWare; administrator authorization may also be necessary. Imports copy an independent disk and leave VMs stopped, without changing existing VMs. Windows/FLARE installation and pfSense configuration remain guest tasks. Read [Background provisioning through MCP](MCP-PROVISIONING.md) for media staging, safe network selection, exact retry rules, dry-run limits and examples. The running installed release must support these development tools before they can be used.

#### Transfer and cloning limits

File transfers use `${XDG_DATA_HOME:-~/.local/share}/omaware/transfers/` on the host. The folder must be owned by you and accessible only to you (mode `0700`); OmaWare creates it with those permissions on the first transfer attempt. Put upload files there yourself, and look there for downloads. The agent supplies a plain file name, never an arbitrary host path. Files are limited to 32 KiB each; existing destinations are protected unless replacement is explicitly requested and approved. Symlinks, hardlinks, special files and obvious credential-file names are refused. These checks do not identify every secret: only place files you intend to share in the transfer folder.

The initial transfer implementation requires a Linux guest with `python3`, reachable through the QEMU guest agent or lab SSH. It is intended for small scripts and reports, not disk images or large archives.

`clone_vm` creates an independent full copy from an existing checkpoint ID (see `list_snapshots`). The new VM is stopped, with new identifiers and MAC addresses and disconnected network cables. Guest files, identities and credentials are copied: change those inside the clone before connecting it to a network. Linked clones are not supported.

`wait_for_vm` can wait for running state, the guest-agent connection, lab SSH, or completed cloud-init, for up to 120 seconds per call. A timeout reports `ready: false`; it does not power off the VM or undo boot work.

### Labs

A lab is a set of networks and VMs built together. The agent writes a plan and OmaWare shows it to you in **Build “…”?** before anything is created:

- **Networks** and what they reach: **Internet** (each other, this computer and the internet), **Private** (each other and this computer) or **Isolated** (only each other).
- **VMs**: the operating system (Ubuntu Server LTS, Debian or Fedora Cloud), size, networks and addresses, packages to install, and every command that will run inside the VM on its first boot, in full.
- **Login for these VMs**: a user name (the agent suggests one; ask it to use the one you want) and a password you type or **Generate** here. The password goes into your system's password store (GNOME Keyring, KWallet or another Secret Service; without one, a private file only you can read). The agent never sees it. You can also **Use a saved login** from an earlier lab.

**Build** then downloads the cloud images it needs (once, checked against the publisher's checksums, into `~/.local/share/omaware/images/`), creates the networks (asking for your computer's password once, to let VMs join them), creates the VMs, saves a **Lab built** snapshot of each, and starts them. The dialog and a banner show the progress. VMs are set up on their first boot by cloud-init, which takes one to three minutes: the user with administrator rights (sudo), the password, the network addresses and any packages.

A lab VM's **Details → Overview** shows its **Lab login**, with the password hidden until you choose **Show**. **Decline** throws a plan away.

### What agents can and can't do

- They only see and use VMs and networks OmaWare created. Your other VMs aren't shown to them, and **contained** VMs (for untrusted software) are off limits, since what's on their screens could try to steer the agent.
- They use a VM's screen by taking screenshots and sending mouse clicks and keys, like a person at the console. A banner says which VM's screen an agent is using, with **Watch** (opens its console) and **Stop agent** (turns agent access off). Text is typed as on a US keyboard layout.
- When a login prompt asks for the password, the agent asks OmaWare to type it (**type login**); the agent doesn't receive it. It could still read it if it had it typed somewhere it's shown, so keep lab passwords separate from your others (**Generate** makes a fresh one).
- They run commands through the QEMU guest agent where one runs in the VM (as root). The agent talks to OmaWare over a virtual serial channel, not the network, so this works on **Isolated** networks too; every VM OmaWare creates has the channel, and the guest needs the `qemu-guest-agent` package running. Without it, only lab VMs on Internet or Private networks can run commands, over SSH as the lab user, with a key OmaWare made for that lab, checked against each VM's own host key. Other VMs can only be used through their screens.
- **Contained** VMs stay off limits to agents entirely, including through the guest agent: what runs inside them controls what comes back (output, addresses), and that could try to steer the agent. You can still use the guest agent yourself on a contained VM (see [Containment](NETWORKS.md#containment)).
- **Routers and appliances without a guest agent** (pfSense, OPNsense, VyOS) can be configured through their serial console with `serial_console`: plain text in and out, no network needed, so an isolated lab stays isolated. pfSense has to use its serial console: install from the serial image, or set **System → Advanced → Admin Access → Serial Terminal** once (or choose it in the console menu). The tool refuses while someone else has the console open (for example `virsh console`), and what's typed is never logged.
- **Restoring a snapshot**, **deleting a lab**, creating VMs and networks, allowing VMs on a network, changing adapters, CPU, memory or ISOs, managing networks, cloning and file transfers ask you first, in OmaWare. If you say no, the agent is told so. Power actions, taking snapshots and plugging or pulling cables don't ask. A restart an agent wants along with an adapter change is in the same question; it asks the guest to shut down and never forces it off.
- **Fewer questions, if you want:** when an agent adds, moves or removes an adapter on an isolated or host-only network OmaWare created, the question has an unticked option, **Don't ask again for adapter changes on isolated and host-only networks**. If you tick it, such changes go ahead without asking until you turn agent access off (or the banner's **Revoke**), or for at most 8 hours; it's never saved. A banner shows it while it's on, and the activity log marks each such change as auto-approved. Connecting a VM to the internet (a NAT network or its private internet connection) or your local network, and everything else (deleting, restoring, creating, allowing VMs on networks, contained VMs) still asks every time.
- Everything an agent does is listed in the activity log (Ctrl+`), marked "Agent:". What it types is never logged.

Deleting a lab removes its VMs with their disks and snapshots, and its networks. Its saved login is kept.

## Updating OmaWare

The **download button** at the bottom of the sidebar checks GitHub for a new version at any time, and OmaWare also checks once a day (turn this off in **Settings → Updates**). When one is out, a window shows what's new and warns before anything happens: updating restarts OmaWare, so running VMs are paused first. **Update now** downloads the new version, checks it against the release's checksums, pauses your VMs, and restarts into it on the page you were on; then **Resume all** continues the VMs exactly where they left off. **Not now** changes nothing. The previous version is kept next to the app as `app.previous`.

- A copy that wasn't installed from a release package (for example, built from source) can't update itself; the window links to the download instead.
- If a VM can't be paused, the window says which one and offers **Update anyway** (it keeps running) or **Keep OmaWare open**.

## Settings and themes

**Settings** (or the palette button at the bottom of the sidebar) has:

- Text size: 100%, 115% or 130%.
- **Reduce motion**, which turns off animations.
- **Theme:** every theme as a small preview in its own colors; click one to switch. **Follow Omarchy** (when Omarchy is installed) uses your desktop's theme. The built-in themes are Dark, Light, Hacker (green on black, with scanlines), Tokyo Night, Catppuccin Mocha, Catppuccin Latte, Nord, Gruvbox, Rosé Pine, Dracula, Everforest, Kanagawa, Solarized Light, Synthwave (with a neon grid) and Graphite. Every theme keeps text, warnings and errors readable. Themes are also in the command prompt: type `theme`.
- **Storage:** where your VMs and ISOs are, with a button to open the folder.
- **Updates:** your version, **Check for updates**, and whether OmaWare checks GitHub automatically (once a day). See [Updating OmaWare](#updating-omaware).
- **When OmaWare closes**: a reminder that running VMs are always paused.
- The OmaWare version.

When following Omarchy, OmaWare reads the current Omarchy theme's colors, including its own reds, yellows and greens for errors, warnings and "running", and updates as soon as you change themes. It only reads the theme; it never changes anything in Omarchy.

## Closing OmaWare

Closing OmaWare always **pauses every running VM** it manages, then closes; restarting for an update does the same. A paused VM keeps its memory, so it continues exactly where it was.

- Next time you open OmaWare, those VMs are tagged `auto-paused` and a banner offers **Resume all**, or **Choose…** to pick which ones to resume. You can also resume any VM on its own.
- If a VM can't be paused (for example, while a snapshot is being taken), OmaWare tells you and offers **Close anyway** or **Keep OmaWare open**.
- If the computer restarts, paused VMs come back stopped and what was in their memory is lost, as with running VMs.
- Only a normal close pauses VMs. If OmaWare crashes or is killed, they keep running.

Only one OmaWare can be open at a time. Opening it again while it's already open shows a short message instead of a second window, because two copies would both manage the same VMs.

## Keyboard shortcuts

Press `?` in OmaWare for the full list, or F1 for Help. Shortcuts are off while the console has your input; Ctrl+Alt releases it.

| Keys | Action |
| --- | --- |
| F1 | Help |
| `?` | All shortcuts |
| `:` or Ctrl+Shift+P | Command prompt |
| Ctrl+K or Ctrl+F | Search VMs |
| Ctrl+1 / 2 / 3 / 4 | VMs, Monitor, Networks, OS Shop |
| Ctrl+B | Collapse or expand the sidebar |
| Ctrl+` | Log |
| j / k or ↑ / ↓ | Move through the VM list |
| Enter | Open the console |
| d / s | Details / Snapshots |
| f / F2 / Delete | Favorite / rename / remove a stopped VM |
| Ctrl+click, Shift+click, Ctrl+A, Esc | Select several VMs, select all, clear |
| Menu or Shift+F10 | Actions for the selected VM |
| Ctrl+Alt | Release the console's input |
| Esc | Leave fullscreen or focus view |

## Where OmaWare keeps its files

- **VMs, ISOs and the app:** `~/.local/share/omaware/` (or `$XDG_DATA_HOME/omaware`), with each VM's disks in `vms/`, installation ISOs in `isos/` (with a small `.omaware-media.json` noting kept files and how downloads were checked), OVA and qcow2 appliances and ready-made VMs in `appliances/`, and OmaWare itself in `app/` when installed from the Linux package. A new VM can use another storage location, chosen when creating it.
- **Snapshots, pending changes and activity history:** `~/.local/share/Omaware/Omaware/`. VMs made by earlier versions keep their disks there too; nothing is moved.
- **Settings** (theme, window size, folders, tags, notes): `~/.config/Omaware/Omaware/workspace.ini`.
- **VM definitions** are kept by libvirt, not OmaWare.

Removing a VM's definition or detaching a disk never deletes disk files; **Delete VM…** does. When moving to another computer, copy the data folder as well as the disks: a libvirt definition alone doesn't include the disks or snapshots.
