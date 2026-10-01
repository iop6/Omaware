# OmaWare user guide

- [The window at a glance](#the-window-at-a-glance)
- [Creating a VM](#creating-a-vm)
- [The VM list](#the-vm-list)
- [Using the console](#using-the-console)
- [Details: overview, hardware and networks](#details-overview-hardware-and-networks)
- [Changing hardware](#changing-hardware)
- [Snapshots](#snapshots)
- [The ISO Shop](#the-iso-shop)
- [Monitor, log and command prompt](#monitor-log-and-command-prompt)
- [Updating OmaWare](#updating-omaware)
- [Settings and themes](#settings-and-themes)
- [Closing OmaWare](#closing-omaware)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Where OmaWare keeps its files](#where-omaware-keeps-its-files)

Networking has its own guide: [NETWORKS.md](NETWORKS.md).

## The window at a glance

- **Sidebar (left):** navigation (Virtual machines, Monitor, Command, Networks), the VM list, **Create VM**, theme buttons and **Settings**.
- **Workspace (right):** the selected VM with three tabs: **Console**, **Details** and **Snapshots**.
- **Status line (bottom):** current mode, connection state, host CPU and memory, number of running VMs, snapshot jobs, the log and the time.

OmaWare works with VMs in your own libvirt session (`qemu:///session`). It only changes VMs it created itself. Other VMs in the session are shown read-only.

## Creating a VM

Click **Create VM**.

1. Enter a name and choose where the system comes from:
   - **An installation ISO.** Choose one from your ISO library, get one from the **ISO Shop**, browse to a file, or drag an ISO onto the window. OmaWare creates a new empty disk and attaches the ISO. Picking a known ISO also picks the matching operating system and a name.
   - **An existing disk image** (raw or qcow2). OmaWare copies it into a new, independent qcow2 disk. The original file is never changed.
2. Choose the operating system type, so the VM gets sensible default devices.
3. Optional: open **Advanced setup** for processors, memory, disk size, network, storage location and BIOS or UEFI firmware. UEFI needs firmware installed on the host.
4. Review and create. The new VM starts out stopped; start it to run the installer.

After installing from an ISO, shut the VM down and use **Details → Hardware → Edit hardware** to eject the ISO and boot from the disk first.

The small arrow next to Create VM also offers a **diskless test VM**: a tiny VM with no disk or network, handy for trying things out. It shows "No bootable device", which is expected.

**Windows:** pick a Windows ISO and OmaWare sets the VM up the way Windows expects: UEFI firmware with Secure Boot (when your computer has that firmware), a SATA disk and network card that Windows recognizes without extra drivers, and **Add a TPM 2.0 chip**, which Windows 11 requires. The TPM needs the `swtpm` package on your computer; snapshots keep the TPM's contents along with the disks. You need your own Windows license to activate it.

OmaWare doesn't install operating systems unattended.

## The VM list

Each row shows the VM's state (● running, ‖ paused, ○ stopped, ✕ crashed), live CPU use, a small CPU graph and tags: `contained`, `read-only`, `test`, `auto-paused` and up to two of your own tags.

- **Search** (Ctrl+K) matches names, display names, tags and folders. **All / Active / Stopped** filter the list.
- **Right-click** a VM (or use its `⋯` button, the Menu key or Shift+F10) for everything you can do with it: open the console, Details or Snapshots; start, pause, resume, shut down or force off; take or revert a snapshot; **Show on network map**; favorite; **Rename & organize**; containment; copy its name or UUID; remove it.
- Power and organize actions from this menu don't switch the console away from the VM you're looking at.
- **Rename & organize** sets a display name, folder, tags, notes and favorite. This is only stored in OmaWare; the VM itself keeps its real name.
- Favorites come first, then folders (alphabetically), then everything else. Click a group heading to fold it.
- **Remove definition** (stopped VMs only) removes the VM from libvirt. Its disk files are kept.
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
- **Command** (`:` or Ctrl+Shift+P) searches every action and VM. Type part of a name, pick with the arrow keys and press Enter.

## The ISO Shop

**ISO Shop** in the sidebar (Ctrl+4) offers free, official installation images, always the newest release:

- **Desktop:** Ubuntu, Kubuntu, Xubuntu, Linux Mint, Fedora Workstation, Fedora KDE Plasma, Debian, Arch Linux, openSUSE Tumbleweed, Pop!_OS and NixOS.
- **Server:** Ubuntu Server, Rocky Linux, AlmaLinux, Alpine Linux, Proxmox VE and FreeBSD.
- **Security & networking:** Kali Linux and OPNsense.
- **Windows:** Windows 11 straight from Microsoft, in the language you choose on its card (OmaWare picks your computer's language at first), checked against the SHA-256 checksum Microsoft publishes for that language. Microsoft sometimes refuses automated downloads from some networks or after many attempts; the card then says so and offers **Website**, and a Windows ISO you download there is recognized too.
- **From the publisher's website:** pfSense (through Netgate's free store). Save the ISO into your ISO folder and the shop recognizes it.

Filter by category or search by name. **Download** saves the ISO into `~/.local/share/omaware/isos/`. Every download is checked against the publisher's own SHA-256 checksum and thrown away if it doesn't match; compressed images (OPNsense) are unpacked after they're verified. Downloads keep going in the background (the sidebar shows their progress) and can be cancelled.

**Your own ISOs:** drag ISO files from your file manager onto any part of the OmaWare window. They're added to your ISO folder; a file on the same disk is added instantly and takes no extra space, anything else is copied (with progress and **Cancel**). Your original file stays where it was. Drop a single ISO and the create dialog opens with it chosen; drop it while that dialog is open and it switches to that ISO.

When a newer release is out, its card says **Update**, and **Your ISOs** marks the older file.

Deleting ISOs is quick, and always asks once in place before anything is removed:

- **The trash button on a card** deletes your copies of that system.
- **Delete older versions** in Your ISOs removes every ISO a newer download has replaced, and shows how much space that frees.
- **Tick several files** (or **Select all**) and use **Delete selected**. Each file also has its own trash button.

**New VM** on a card or file opens the create dialog with that ISO chosen, the matching operating system selected (when your computer knows it) and a name suggested. OmaWare only contacts the publishers when you open the shop or press **Check for updates**.

## Updating OmaWare

The **download button** at the bottom of the sidebar checks GitHub for a new version at any time, and OmaWare also checks once a day (turn this off in **Settings → Updates**). When one is out, a window shows what's new and warns before anything happens: updating restarts OmaWare, so running VMs are paused first. **Update now** downloads the new version, checks it against the release's checksums, pauses your VMs, and restarts into it on the page you were on; then **Resume all** continues the VMs exactly where they left off. **Not now** changes nothing. The previous version is kept next to the app as `app.previous`.

- A copy that wasn't installed from a release package (for example, built from source) can't update itself; the window links to the download instead.
- If a VM can't be paused, the window says which one and offers **Update anyway** (it keeps running) or **Keep OmaWare open**.

## Settings and themes

The theme buttons at the bottom of the sidebar switch between **following Omarchy** (when installed), **dark**, **light** and **hacker** (green on black). **Settings** has:

- Text size: 100%, 115% or 130%.
- **Storage:** where your VMs and ISOs are, with a button to open the folder.
- **Updates:** your version, **Check for updates**, and whether OmaWare checks GitHub automatically (once a day). See [Updating OmaWare](#updating-omaware).
- **Reduce motion**, which turns off animations.
- **When OmaWare closes**: a reminder that running VMs are always paused.
- The OmaWare version.

When following Omarchy, OmaWare reads the current Omarchy theme's colors and updates as soon as you change themes. It only reads the theme; it never changes anything in Omarchy.

## Closing OmaWare

Closing OmaWare always **pauses every running VM** it manages, then closes; restarting for an update does the same. A paused VM keeps its memory, so it continues exactly where it was.

- Next time you open OmaWare, those VMs are tagged `auto-paused` and a banner offers **Resume all**, or **Choose…** to pick which ones to resume. You can also resume any VM on its own.
- If a VM can't be paused (for example, while a snapshot is being taken), OmaWare tells you and offers **Close anyway** or **Keep OmaWare open**.
- If the computer restarts, paused VMs come back stopped and what was in their memory is lost, as with running VMs.
- Only a normal close pauses VMs. If OmaWare crashes or is killed, they keep running.

Only one OmaWare can be open at a time. Opening it again while it's already open shows a short message instead of a second window, because two copies would both manage the same VMs.

## Keyboard shortcuts

Press `?` or F1 in OmaWare for the full list. Shortcuts are off while the console has your input; Ctrl+Alt releases it.

| Keys | Action |
| --- | --- |
| `:` or Ctrl+Shift+P | Command prompt |
| Ctrl+K or Ctrl+F | Search VMs |
| Ctrl+1 / 2 / 3 / 4 | VMs, Monitor, Networks, ISO Shop |
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

- **VMs, ISOs and the app:** `~/.local/share/omaware/` (or `$XDG_DATA_HOME/omaware`), with each VM's disks in `vms/`, installation ISOs in `isos/`, and OmaWare itself in `app/` when installed from the Linux package. A new VM can use another storage location, chosen when creating it.
- **Snapshots, pending changes and activity history:** `~/.local/share/Omaware/Omaware/`. VMs made by earlier versions keep their disks there too; nothing is moved.
- **Settings** (theme, window size, folders, tags, notes): `~/.config/Omaware/Omaware/workspace.ini`.
- **VM definitions** are kept by libvirt, not OmaWare.

Removing a VM or detaching a disk never deletes disk files. When moving to another computer, copy the data folder as well as the disks: a libvirt definition alone doesn't include the disks or snapshots.
