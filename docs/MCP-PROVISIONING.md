# Creating VMs and networks through MCP

AI agents can create VMs from the user's own installation media and appliances, and create networks for
them, without using the desktop: OmaWare asks the user to approve each request in its own window. This is
separate from `propose_lab`, which builds labs from cloud images set up by cloud-init (see the
[user guide](USER-GUIDE.md#labs)).

- [Getting media](#getting-media)
- [Creating networks and VMs](#creating-networks-and-vms)
- [Approval, status and retries](#approval-status-and-retries)
- [Dry runs](#dry-runs)
- [Failure reasons](#failure-reasons)
- [After creation: networks, adapters and addresses](#after-creation-networks-adapters-and-addresses)

## Getting media

`get_media {"action": "catalog"}` lists the OS Shop's sources: their `source` id, `kind` (`installer`,
`vm_image` or `website_only`), `versions` (newest first; empty while the publishers' release lists load and
`checking` is true), `languages` for Windows, and `verification`: `checked` (compared with the publisher's
SHA-256 or SHA-512) or `unverified` (the publisher gives no such checksum; `verification_note` says why).

`get_media {"action": "download", "source": "debian"}` (optionally with `version` and `language`) asks the
user every time; there is no session approval for downloads, and a refusal answers `declined`. It returns
`started`; poll `{"action": "status", "source": …}` until `files` lists the result. Only catalogue sources
are accepted, never URLs or paths. ISOs land in the ISO library; VM images (`vm_image`) are unpacked into
the appliance library for the user to import.

Codes: `not_found`, `website_only`, `not_ready` (the release list is still loading), `invalid_argument`,
`declined`, `download_failed`, `already_have` (a state) and `already_downloading` (a code on a status answer).

## Creating networks and VMs

1. **`list_installation_media {}`** lists up to 256 library entries, each with `kind` (`iso` or `disk`),
   `name` and `bytes`. Installation ISOs go in `$XDG_DATA_HOME/omaware/isos` and standalone `.qcow2`, `.raw`
   and `.ova` appliances in `$XDG_DATA_HOME/omaware/appliances` (normally under `~/.local/share`). An OVA must
   hold one VM with one standalone VMDK (plain or gzip-compressed); only its disk is imported. Listing doesn't
   download, create folders or verify publisher checksums. The library folders and their parents must not be
   symlinks or writable by group or others; files must be non-empty regular files owned by the user, with
   one hard link. Keep media in place until the installation is done and the ISO ejected.
2. **`create_network`** creates and starts a network OmaWare owns: `isolated` (no host address or DHCP),
   `hostonly` (reaches this computer) or `nat` (this computer and the internet). The last two need an IPv4
   `subnet` from /16 to /29; `dhcp` defaults to false and, when true, needs `dhcp_start` and `dhcp_end`.
   Conflicts with host and libvirt subnets are checked when the network is created. `autostart` (default
   true, as for networks made in OmaWare and by labs) starts the network with the computer, so VMs on it
   still work after a reboot. Start, stop, edit, delete or change autostart later with `manage_network`.
3. **`list_owned_networks {}`** returns each owned network's UUID, revision, mode, whether it runs and
   whether VMs may join it. **`authorize_network`** (with `network`, its expected `revision` and a new
   `request_id`) runs OmaWare's root helper after approval, so VMs may join the network's bridge; the user
   may also have to enter their computer's password. MCP can't supply passwords or install the helper.
   List the networks again afterwards. A failure says why in `result.code` and `result.reason`.
4. **`create_vm`** takes a plain `name`, `media_kind`, the exact library `media` file name and a new
   `request_id`. Optional: `cpus` (default 2), `memory_mib` (4096), `preset` (generic), `firmware` (bios),
   `tpm` (false) and `networks`, an ordered list of up to four distinct entries, each an owned, running
   network UUID VMs may join or `"user"` (the VM's own private internet connection, "Internet · private
   to this VM"). **The order is kept:** adapters are defined, given PCI addresses and numbered by the guest
   in this order, so the first entry is the guest's first NIC (pfSense's `vtnet0`, Linux's `eth0` or lowest
   `enp…`). The approval lists them in that order. ISO installs accept `disk_gib` (default 32, 1–2048);
   imported disks keep their own size and refuse that option. Host resource limits and libosinfo support
   are checked on creation. No storage path, host shell, XML, seed, cloud source, external bridge or
   network OmaWare didn't create can be given.
5. **Creating a VM never starts it.** An imported disk is copied into independent qcow2 storage (images with
   backing files, external data files or encryption are refused), and the VM gets fresh MAC addresses.
   The source image is opened without following symlinks and read through that open file, so it can't be
   swapped while it's copied. The guest's own identities, accounts and credentials are copied as they are;
   nothing inside the guest is changed. Start the VM with `vm_power`, then use the screen and command
   tools.

A REMnux appliance prepared in the appliance library is imported with:

```json
{"request_id":"10000000-0000-4000-8000-000000000001","name":"remnux","media_kind":"disk","media":"remnux-noble-amd64.ova","cpus":2,"memory_mib":4096,"preset":"generic"}
```

A Windows installer VM uses `media_kind: "iso"`, the listed Windows file name, `preset: "win11"`,
`firmware: "uefi"`, `tpm: true` and enough memory and disk; Windows itself is installed on the VM's screen
afterwards. A pfSense router with its WAN on the private internet connection and its LAN on an isolated
network is one request, WAN first:

```json
{"request_id":"10000000-0000-4000-8000-000000000002","name":"pfsense","media_kind":"iso","media":"pfSense-CE-2.7.2-RELEASE-amd64.iso","preset":"freebsd14.0","memory_mib":2048,"networks":["user","20000000-0000-4000-8000-000000000001"]}
```

Choose a FreeBSD preset the host's libosinfo knows. Installing the guest and its firewall rules happen
inside the guest. Use an isolated network for analysis unless the VM is meant to reach this computer or
the internet.

## Approval, status and retries

`create_network`, `authorize_network` and `create_vm` each need a `request_id` the agent generates: a
lower-case, non-zero UUID. They answer at once with an operation record; poll
`provision_status {"request_id": "…"}` until its `state` is `succeeded`, `failed` or `declined`. On the way
it is `preparing`, `awaiting_approval` or `running`. Only one request runs at a time; another one meanwhile
is refused with `busy`.

Sending **the same ID with the same arguments** again returns that operation's current state, without
another question or change; the same ID with different arguments is refused with `request_conflict`.
OmaWare remembers up to 128 requests until it closes (`session_limit` after that), and forgets them when
it restarts: after a restart, or for an unknown ID, look at the VMs and networks before trying again,
rather than retrying with a new ID. A `failed` operation may have done part of its work (for example a
network defined but not started); check the inventory and OmaWare's activity log first.

Results only show UUIDs, subnets, whether VMs may join a network, and revisions, plus a stable `code` and a
short `reason` on failure: never XML, program output, paths (other than the helper's documented location) or
credentials. OmaWare's window and activity log show the full cause.

An unanswered approval expires after five minutes. Turning agent access off declines unanswered approvals
and revokes approved requests: one approved earlier can't go ahead after access is turned on again, even
while it waits for OmaWare to be free or a disk is being copied. The media file's identity and the networks'
revisions and availability are checked after approval and again right before each change. A step that has
started can't be cancelled; revoking access stops the steps after it but doesn't undo what's done. libvirt
has no way to lock out other tools during a request. Library files must be trusted and not edited while a
request runs: the identity checks don't hash contents and can't stop another program running as the user.

## Dry runs

`dry_run: true` on any of the three checks the arguments, the media and its identity, the networks and the
request itself, and returns a preview without changing anything: it reserves no request ID, never asks the
user and starts no work. It is **not** a full rehearsal: host capacity, OS and TPM support, image format and
backing files, free space and subnet conflicts are only checked by the real request.

## Failure reasons

A `failed` or `declined` operation has `result.code`; failures the bridge helper explains also have
`result.reason`, a fixed sentence for that code.

| `code` | Meaning | What to do |
| --- | --- | --- |
| `helper_missing` | No helper at `/usr/local/libexec/omaware/authorize-bridge`. | Ask the user to install it. **Allow VMs** under Networks shows the command for their copy of OmaWare: `sudo install -D -o root -g root -m 0755 <OmaWare folder>/authorize-bridge /usr/local/libexec/omaware/authorize-bridge` (or `sudo cmake --install <build> --component helper` for a source build). |
| `helper_untrusted` | The helper, or a folder above it, isn't owned by root, is writable by others, or is a link, so OmaWare won't run it as root. | Ask the user to reinstall it with the command above. |
| `authorization_cancelled` | The user closed the administrator password prompt. | Ask before trying again. |
| `authorization_denied` | Wrong password, or polkit refused. | Ask the user. |
| `authorization_unavailable` | No pkexec, or no polkit authentication agent in the user's session. | The user has to allow it from their desktop session (**Allow VMs**). |
| `authorization_timeout` | Nobody answered the prompt within five minutes. | Ask the user, then retry with a new `request_id`. |
| `deny_rule` | A `deny` rule in the host's `bridge.conf` (or a file it includes) blocks the bridge. | Only the host's administrator can change it. |
| `bridge_inactive` | The network's bridge isn't up. | `manage_network start`, then authorize again. |
| `network_not_owned` | Not a network OmaWare created. | Use one OmaWare created. |
| `bridge_policy_unsafe` | The bridge policy files have unexpected owners, permissions or size. | The host's administrator has to review them. |
| `qemu_bridge_helper_missing` | QEMU's own `qemu-bridge-helper` is missing or not owned by root. | The user installs QEMU's bridge helper package. |
| `helper_failed` | Anything else the helper reported. | See OmaWare's activity log. |
| `precondition_failed`, `state_changed` | The media or networks no longer match the request (before or after approval). | List them again. |
| `declined` | The user said no, or agent access was turned off. | Don't retry without asking. |
| `operation_failed` | Any other failure. | Check the inventory and the activity log. |

The helper (`scripts/authorize-bridge.py`) exits with 3–7 for the refusals above; helpers installed by
older versions exit with 1 and are recognized by their message.

## After creation: networks, adapters and addresses

These tools answer when they're done (they take no `request_id`), and every change waits for the user's
approval in OmaWare.

- **`manage_network`** takes an owned `network` (UUID or name), an `action` and the `revision` from
  `list_owned_networks` or the last change:
  - `start` and `stop`; `stop` is refused (`in_use`) while any VM, running or not, has an adapter on it.
  - `set_autostart` with `autostart: true|false`: whether it starts with the computer. A network VMs use
    should normally start with the computer.
  - `edit` with `mode`, `subnet`, `dhcp`, `dhcp_start` and `dhcp_end` (values left out stay as they are),
    only for a stopped network no VM uses (`network_active`, `in_use`). It stays stopped. A new subnet with
    DHCP on needs a new range.
  - `delete` stops and removes an unused network. A lab's networks are refused (`lab_network`); use
    `delete_lab`.

  Revisions are checked before the approval prompt and again after it (`stale_revision`). The helper's
  `allow` line for a deleted network stays in `bridge.conf`; it only names that network's bridge.
- **`manage_network_adapter`** changes apply to a running VM when its guest supports PCI hot-plug. A paused
  VM, or a guest that doesn't release the adapter, keeps its old connection until a full shutdown and start;
  the answer then has `pending_change_count > 0` and `restart_needed: true`. With
  `apply: "restart_if_needed"` (and optionally `restart_timeout_seconds`, 10–300, default 120) the same
  approval covers a restart: OmaWare resumes a paused VM, asks the guest to shut down (guest agent or ACPI),
  waits, and starts it again. `restart.state` is `completed` or `not_needed`, or on failure `timed_out`
  (`code: restart_timed_out`) or `start_failed`. OmaWare never forces the VM off: after a timeout it keeps
  running, and the change stays saved and pending.
- **Session approvals:** adding, moving or removing an adapter on an owned isolated or host-only network can
  be approved for the rest of the session, with a checkbox in the question (off by default). Such changes
  then don't ask, and their answer has `approved_by: "session_grant"` (otherwise `"user"`). The approval
  ends when agent access is turned off, when the user revokes it, or after 8 hours, and is never saved.
  Revision, ownership, availability and containment are still checked. The private internet connection,
  NAT networks and every other tool always ask.
- **`serial_console`** sends text to a VM's serial console (`\n` is Enter) and returns what the guest prints,
  until `wait_for` appears, the guest is quiet for a second, or `timeout_seconds` (1–60) runs out. It opens
  the console through libvirt for itself alone (`console_unavailable` while another client has it), needs no
  network or guest agent, and logs only how much was typed. `type_login` with `via: "serial"` types a saved
  login there and masks the password if the guest echoes it.
- **Revisions:** adapter changes, resource and ISO changes, `set_cable`, `manage_network`, `create_network`
  and `authorize_network` return the new `revision`. Use it for the next change instead of listing again.
- **`omaware_overview`** shows `pending_change_count` and `restart_needed` for each VM, `autostart` for each
  network, and for each adapter its `ips` with an `ip_source`: `dhcp_lease` (a host network's DHCP), `arp`
  (this computer's neighbour table), `guest_agent` (asked inside the guest, for example for a VM behind a
  pfSense LAN whose DHCP server is the router) or `unknown`. OmaWare never gives this computer an address on
  an isolated network to find them.
