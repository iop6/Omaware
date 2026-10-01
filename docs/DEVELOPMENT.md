# Developing OmaWare

- [Project layout](#project-layout)
- [Building](#building)
- [Tests](#tests)
- [How it fits together](#how-it-fits-together)
- [Rules the code relies on](#rules-the-code-relies-on)
- [Design decisions](#design-decisions)
- [Releasing](#releasing)

## Project layout

| Path | Contents |
| --- | --- |
| `src/main.cpp` | Starts the app: sets up Qt, the backend and the theme, loads the QML. |
| `src/backend.*` | The facade QML talks to (`backend`), and `VmWorker`, which does all libvirt work on background threads: inventory, power actions, console connection, details, network edits, cables. |
| `src/management.cpp` | `VmWorker::manage`: VM creation, hardware edits, disks, statistics and host network management. |
| `src/checkpoints.*`, `src/snapshothistory.*` | Snapshots: capture, restore, revert, delete, verify, clone, storage cleanup and recovery after interruptions. |
| `src/configuration.*` | Editing saved VM definitions safely, and the record of pending changes for running VMs. |
| `src/domainconfig.*` | Reading a libvirt domain XML into plain values for the UI, and writing network adapter XML. |
| `src/networkcatalog.*` | Finding the networks and bridges a VM can use, and whether each is usable. |
| `src/containment.*` | The containment policy and live isolation checks. |
| `src/console.*` | The VNC console: a Qt Quick item plus a worker thread running LibVNCClient. |
| `src/theme.*` | Reads and watches the Omarchy palette; provides the colors QML uses. |
| `src/workspace.*` | Per-user settings (`preferences` in QML). |
| `src/diagnostics.*` | Activity history and friendlier explanations of libvirt errors. |
| `src/paths.*` | Where VMs and ISOs live (`~/.local/share/omaware/{vms,isos}`), with fallback to folders from earlier versions. |
| `src/isolibrary.*` | The ISO library behind the ISO Shop (`qml/IsoShopPage.qml`): each source's lookup of the publisher's latest release, verified downloads and unpacking (`IsoLibrary` in QML). |
| `src/instance.*` | Keeps OmaWare to one running copy per user. |
| `src/updater.*` | Built-in updates from GitHub Releases (`Updater` in QML): checks, verified download, in-place swap and restart. |
| `src/agentbridge.*` | AI agent access (`agent` in QML): the local socket `omaware mcp` talks to, the tools, the user's approvals, and building and deleting labs. |
| `src/agentmanagement.cpp`, `src/agenttransfer.*` | Agent VM-management tools, bounded readiness checks, and confined file transfers. |
| `src/mcpserver.*` | `omaware mcp`: the Model Context Protocol server agents start; describes the tools and forwards calls to the app. |
| `src/labplan.*`, `src/labs.*` | Checking an agent's lab plan; the record of built labs (their networks, VMs, login name and SSH keys). |
| `src/cloudimages.*`, `src/cloudseed.*` | Cloud images for labs (download and checksum), and the cloud-init setup disc (an ISO 9660 image OmaWare writes itself). |
| `src/logins.*` | Saved VM logins: passwords in the Secret Service over D-Bus, or a private file without one. |
| `src/guestinput.*` | Text and key names to keyboard codes for typing into a VM. |
| `packaging/` | Desktop entry, and the release package's `omaware.sh` launcher and `install.sh`. |
| `qml/` | The interface. `Main.qml` is the window; `App*.qml` are shared controls. |
| `scripts/build.sh` | Builds everything, including the patched LibVNCClient. |
| `scripts/authorize-bridge.py` | The root helper that lets session VMs use a switch's bridge. |
| `patches/` | The two LibVNCClient fixes, their upstream license and a regression test ([why](VNC-COMPATIBILITY.md)). |
| `tests/` | Unit tests (`test_core.cpp`) and the VM test suites (`integration.cpp`, `test_management.cpp`). |

## Building

`scripts/build.sh` works on Ubuntu/Debian, Fedora, Arch and openSUSE:

```sh
scripts/build.sh --install-deps   # installs the distribution packages (uses sudo)
scripts/build.sh                  # builds into build/ and runs the unit tests
scripts/build.sh --install        # also installs to ~/.local
```

Requirements: CMake 3.22+, a C++20 compiler, Qt 6.4+ (Base including Network, D-Bus, Declarative, Wayland), libvirt, libcrypt (libxcrypt), toml++, zlib, libjpeg and libpng. At runtime OmaWare also uses QEMU/KVM, `qemu-img`, `virt-install` with libosinfo, UEFI firmware (OVMF) for UEFI VMs, `swtpm` for TPMs, and the OpenSSH client (`ssh`, `ssh-keygen`) for labs.

The script downloads LibVNCServer 0.9.15 at a pinned commit, checks both patch files against their SHA-256 hashes, and builds the library privately in `build/deps`. It is installed next to OmaWare and found through RPATH; nothing is installed system-wide. Always build OmaWare against the headers of the exact LibVNCClient it loads: libraries built with and without SASL have different struct layouts, and mixing them corrupts memory.

## Tests

- **Unit tests** (`ctest`, run by `build.sh`): palette handling, domain XML, network rules, snapshot bookkeeping and the patched VNC decoder. Safe anywhere.
- **Agent regression tests** (also in `ctest`): `agent-tools` checks MCP schemas and transfer validation; `agent-transfer-io` exercises binary transfers, overwrite protection and unsafe-file rejection in temporary directories, including the generated guest-side Python commands. `mcp-stdio` starts the actual MCP executable with a fresh runtime directory, checking protocol responses without connecting to a running OmaWare app. These tests do not contact or change any VM and need Python 3.
- **VM test suites** (`omaware-integration`, `omaware-management-tests`): create, start, pause, snapshot and power off real VMs in your libvirt session, and render the interface offscreen. **Run them only on a disposable machine or VM.** The management tests' pause-on-close test pauses every running OmaWare VM in the session. They refuse to start without an explicit opt-in:

  ```sh
  export OMAWARE_VM_TEST=1 OMAWARE_VM_TEST_HOST=$(hostname)
  export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
  build/app/omaware-integration
  build/app/omaware-management-tests
  ```

  Set `OMAWARE_SCREENSHOT_DIR` to save screenshots of each UI state. Test VMs are named `omaware-test-*`, and cleanup removes only the exact VMs and files a test created. If a run is killed, check leftover VMs by UUID before removing anything.
- **Guest agent tests** need a tiny test kernel and initrd: copy the host kernel to `artifacts/agent-fixture/vmlinuz`, then run `tests/fixtures/make-agent-initrd.py`. The memory-snapshot and filesystem-flush tests fail without them.
- **Online ISO checks** (not run by default, as they contact the publishers): `OMAWARE_ONLINE_TEST=1 build/app/omaware-tests isoLatestOnline` looks up every source and checks each ISO link exists; add `OMAWARE_ONLINE_DOWNLOAD=1` to also download and verify the Debian ISO (about 750 MB), or a list of source ids such as `alpine,opnsense`.
- **Agent and lab tests** (`agentLabOnScreen`, `agentLabNetwork` in the management tests) build real labs from a cloud image. Give them an Ubuntu cloud image so they don't download one: `OMAWARE_CLOUD_IMAGE=/path/to/noble-server-cloudimg-amd64.img`. The first logs in on the VM's screen with the saved login and shuts it down with sudo; the second creates a private network, so it needs `OMAWARE_AGENT_NETWORK_TEST=1` and a machine where the installed bridge helper may run without a password prompt (a polkit rule for `org.freedesktop.policykit.exec` on that program).
- **Host network test:** creates, edits, starts, stops and removes system networks, authorizes a bridge and boots a VM on it. It changes host networking and the bridge helper's permissions, so it needs a further opt-in, `OMAWARE_NETWORK_ADMIN_TEST=1`, and the installed helper.

## How it fits together

```text
QML interface ── Backend (facade) ── queued calls ──▶ VmWorker thread ──▶ libvirt ──▶ QEMU
      ▲                                                     │
      └──────────── inventory, results, events ◀────────────┘

Console item ◀── latest frame ◀── VNC worker thread (LibVNCClient) ◀── socket from libvirt
     └── keys and mouse ──────────────▶

Omarchy colors.toml ── file watcher ── toml++ ── Theme ── QML colors
```

- **libvirt is the only authority over VMs.** OmaWare never talks to QEMU directly; even an agent's mouse input (`input-send-event`) and guest commands go through libvirt's monitor and guest-agent pass-through. It uses the session connection `qemu:///session` for VMs, and reads `qemu:///system` for networks (writing only for explicit network actions).
- **All blocking libvirt calls run on worker threads.** The GUI thread only sends queued requests and receives results. Snapshot jobs use a second worker so the interface, details and console stay responsive during long copies.
- **State changes come from libvirt events**, not polling: a separate thread runs libvirt's event loop, and each lifecycle event triggers a fresh inventory. Statistics are polled only while something is shown that needs them.
- **The console never listens on the network.** QEMU's VNC server has no listener; libvirt hands OmaWare a connected socket. Frames are decoded on the VNC thread and passed to the GUI as the latest frame only, so a slow GUI never builds up a queue.
- **Theme:** OmaWare reads Omarchy's palette file (never runs its scripts), checks the colors are readable, and swaps them atomically. A broken file keeps the last good colors.

### AI agents

```text
agent ── stdio (MCP JSON-RPC) ── omaware mcp ── local socket, one JSON line each way ──▶ AgentBridge (in the app)
                                                                                           ├─ Backend: changes and lists
                                                                                           └─ its own VmWorker: screen, input, guest commands
```

- The socket lives in the user's runtime folder (`$XDG_RUNTIME_DIR/omaware-agent.sock`, user-only), and exists only while agent access is on.
- Agents only reach OmaWare-owned VMs (`findVm`), never get passwords (lab logins are typed by `type_login`), and can't restore or delete without a yes in `agent.confirmation`.
- A lab build is a list of steps run one after another (images, networks, one helper call for all bridges, keys, VMs, snapshots, start); each finished step is saved in the lab's record so a failed build can still be deleted.
- Lab VMs get a cloud-init seed with the user, a SHA-512 password hash, OmaWare's lab SSH key and a host key OmaWare made, which it pins in the lab's `known_hosts` under the VM's UUID.

## Rules the code relies on

- **Ownership:** OmaWare only changes VMs named `omaware-*` that carry its metadata marker, and networks it created. Everything else is read-only. Tests rely on this to never touch the user's other VMs.
- **Stale-edit protection:** edit dialogs record a revision (a hash of the saved definition) when they open; the worker refuses to save if it changed. This catches edits from other tools, though libvirt offers no true transaction.
- **Running VMs:** configuration changes go to the saved definition only (`VIR_DOMAIN_AFFECT_CONFIG`). Before the first such change, `PendingChanges` stores a baseline, so each change can be listed and undone, even after restarting OmaWare. Cable link state is the exception: it changes live and saved together.
- **Destructive storage:** snapshot and restore steps write a journal first, so an interrupted operation can be recovered or rolled back. Files are deleted only after checking every VM definition, backing chain, pending change and journal that could still need them.
- **Qt and libvirt event loops:** `main.cpp` sets `QT_NO_GLIB=1` before creating the application. Otherwise Qt's GLib dispatcher and libvirt's event loop share GLib's default context and can deadlock at shutdown. Keep this in any new entry point.
- **Containment** is checked in the worker on every start, resume, revert and configuration change, not only in the interface.

## Design decisions

- **C++20 and Qt Quick** call libvirt's and LibVNCClient's C APIs directly. Rust with CXX-Qt was considered and would have added a second toolchain for no gain.
- **VNC first.** SPICE would add audio and better clipboard support, but its display widget is GTK-only; it remains an option for later.
- **Reuse over reinvention.** VM creation uses `virt-install --print-xml` with libosinfo for sensible defaults, disk work uses `qemu-img`, and statistics, snapshots and networks use libvirt's public APIs. See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for what is used and how.
- **Settings identity:** the Qt organization and application name stay `Omaware` so existing settings, snapshots and disk paths keep working; the visible name is OmaWare.

## Releasing

The main branch is always the next version in development, and its builds end in `-dev` (for example `1.1.0-dev`). Each finished version is a git tag (`v1.0.0`) and a [GitHub Release](https://github.com/iop6/Omaware/releases) with release notes, a ready-to-run app, a source archive and checksums. The version is set once, in `CMakeLists.txt`; `omaware --version` and the Settings panel show it. [CHANGELOG.md](../CHANGELOG.md) lists what changed in each version.

**To make a release:**

1. Add the new version to the top of [CHANGELOG.md](../CHANGELOG.md): a heading with the version and date, and one short line per change.
2. In `CMakeLists.txt`, set the version and remove the `-dev` suffix.
3. Build and run the tests.
4. Commit, then tag: `git tag -a vX.Y.Z -m "OmaWare X.Y.Z"`.
5. Build the app package (the binary, its private LibVNCClient in `lib/`, `vnc-abi-test`, `packaging/omaware.sh`, `packaging/install.sh` and `scripts/authorize-bridge.py` as `authorize-bridge`, the license files and a `SHA256SUMS` covering them) and the source archive, and publish them with a `SHA256SUMS` for both as the GitHub Release for the tag. Also attach `packaging/get-omaware.sh` as `install.sh`: the one-command install in the README downloads it from the latest release. The built-in updater looks for `omaware-X.Y.Z-linux-x86_64.tar.gz` and `SHA256SUMS` there.
6. Set `CMakeLists.txt` to the next version, put the `-dev` suffix back, and commit.

