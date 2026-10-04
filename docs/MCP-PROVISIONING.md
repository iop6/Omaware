# Background provisioning through MCP

The development build supports local installation media and appliances without an
agent focusing or clicking the desktop. The user still approves mutations in
OmaWare's existing confirmation dialog. This is separate from `propose_lab`,
which remains the cloud-image, cloud-init lab builder.

## Tools and sequence

0. Missing media? `get_media {"action": "catalog"}` lists the OS Shop's sources:
   `source` id, `kind` (`installer`, `vm_image` or `website_only`), `versions`
   (newest first; empty until the publishers' release lists have loaded, while
   `checking` is true), `languages` for Windows, and `verification` (`checked`:
   compared with the publisher's SHA-256/SHA-512; `unverified`: the publisher gives
   no such checksum, with `verification_note`). `get_media {"action": "download",
   "source": "debian"}` (optionally `version`, `language`) asks the user in OmaWare
   every time, with no session approval; a refusal answers `declined`. It then
   returns `started`; poll `{"action": "status", "source": …}` until `files` lists
   the result. Only catalogue sources are accepted, never URLs or paths. ISOs land
   in the ISO library; VM images (`vm_image`) are unpacked into the appliance
   library for the user to import. Codes: `not_found`, `website_only`, `not_ready`
   (release list still loading), `invalid_argument`, `declined`, `download_failed`,
   `already_have` (state) and `already_downloading` (code on a status answer).
1. `list_installation_media {}` lists at most 256 safe local library entries,
   with `kind` (`iso` or `disk`), `name` and `bytes`. Stage verified installation
   ISOs in `$XDG_DATA_HOME/omaware/isos` and standalone `.qcow2`/`.raw`/`.ova` appliances
   in `$XDG_DATA_HOME/omaware/appliances` (normally under `~/.local/share`). An OVA
   must hold one VM and one standalone VMDK (plain or gzip); only its disk is imported.
   Listing does not download, create directories or verify publisher checksums.
   The libraries and their parents must not be symlinks or group/world writable;
   files must be user-owned, nonempty regular files with one hard link. Keep
   media in place until installation and explicit ISO ejection are complete.
2. `create_network` defines and starts an owned network using `networks.save`.
   Select `isolated` (no host address/DHCP), `hostonly` (host access), or `nat`
   (host/internet access). For the latter two give an explicit IPv4 /16–/29
   `subnet`; `dhcp` defaults to false, and when true needs `dhcp_start` and
   `dhcp_end`. Host/libvirt subnet conflicts are checked by the worker.
   `autostart` (default true, as for networks made in OmaWare and labs) starts the
   network with the computer, so VMs on it still work after a reboot; the
   approval says which. Start, stop, edit, delete or change autostart later with
   `manage_network` (see below).
3. `list_owned_networks {}` returns UUIDs, revisions, modes, active state and
   availability. `authorize_network` takes `network`, its expected `revision`
   and a new `request_id`. It calls the existing trusted bridge helper after
   approval; system/polkit authorization may still be required. MCP cannot
   supply passwords or install the helper. Refresh the list afterward. A failure
   says why in `result.code` and `result.reason` (see [Failure reasons](#failure-reasons)).
4. `create_vm` takes a plain `name`, `media_kind`, exact library `media` filename,
   and a new `request_id`. Optional `cpus` (default 2), `memory_mib` (4096),
   `preset` (generic), `firmware` (bios), `tpm` (false), and `networks` select the
   configuration. `networks` is an ordered list of up to four distinct entries,
   each an owned, active, available network UUID or `"user"` (the VM's own private
   internet connection, "Internet · private to this VM"). **The order is kept:**
   adapters are defined, given PCI addresses and numbered by the guest in this
   order, so the first entry is the guest's first NIC (pfSense `vtnet0`, Linux
   `eth0`/the lowest `enp…`). The approval lists them in that order.
   ISO installs accept `disk_gib` (default 32, 1–2048); disk imports retain source
   capacity and reject that option. Host resource limits and libosinfo support
   are enforced by `vm.create`. No storage path, host shell, XML, seed, cloud
   source, external bridge or non-owned network can be supplied.
5. Creation **never boots the VM**. The UI's existing backend copies imports to
   independent qcow2 storage, rejects backing/external-data/encrypted images,
   assigns fresh MACs and defines a stopped managed domain. The source image is
   opened without following any symlink components and pinned by descriptor for
   qemu-img. Imported guest identities, accounts and credentials are copied;
   this is not guest customization. Use `vm_power` explicitly to boot and then
   the existing guest screen/command tools as appropriate.

For example, a prepared REMnux appliance can be imported with:

```json
{"request_id":"10000000-0000-4000-8000-000000000001","name":"remnux","media_kind":"disk","media":"remnux-noble-amd64.ova","cpus":2,"memory_mib":4096,"preset":"generic"}
```

A Windows installer VM can use `media_kind: "iso"`, the listed Windows filename,
`preset: "win11"`, `firmware: "uefi"`, `tpm: true`, and suitable RAM/disk capacity.
This does not install Windows or FLARE automatically. A pfSense router with its
WAN on the private internet connection and its LAN on an isolated network is one
request, with the WAN first:

```json
{"request_id":"10000000-0000-4000-8000-000000000002","name":"pfsense","media_kind":"iso","media":"pfSense-CE-2.7.2-RELEASE-amd64.iso","preset":"freebsd14.0","memory_mib":2048,"networks":["user","20000000-0000-4000-8000-000000000001"]}
```

Choose a FreeBSD preset supported by the host's libosinfo. Guest installation and
firewall policy are still guest tasks, not host-side scripts. Use an isolated
analysis network unless host/internet access is intentional.

## Approval, asynchronous status and retry safety

All three mutations require a client-generated lowercase nonzero UUID
`request_id`. They immediately return a structured operation record; call
`provision_status {"request_id": "..."}` until its `state` is `succeeded`,
`failed` or `declined`. Intermediate states are `preparing`,
`awaiting_approval` and `running`. Polling is immediate, with no long-held socket
or duplicate worker dispatch. Only one provisioning request is outstanding at a
time; a different concurrent request receives `busy`.

Resubmitting **the same ID and arguments** returns the same operation's current
state, without another prompt or mutation. Changed arguments with the same ID
receive `request_conflict`. Records are never evicted while this process lives;
128 records bound session memory (`session_limit` afterward). Deduplication and
status are not persisted across app restarts. After a restart or an unknown ID,
inspect inventories, not blindly retry with a new ID. A `failed` worker operation
may have partial effects (for example a defined but unstarted network): inspect
inventory and OmaWare's activity before a new request. Public results expose
only UUID, subnet, authorization and revision fields and, on failure, a stable
`code` with a short `reason`: never XML, subprocess output, paths (other than the
helper's documented location) or credentials. OmaWare's own window and activity
log show the full cause.

Approval expires after five minutes using the existing confirmation mechanism.
Disabling agent access declines unanswered approvals and changes an atomic
access epoch: an old approved request cannot revive after access is enabled
again, including during busy retries or long disk copying. Media stat identities
and owned network revisions/availability are checked after approval and again
in the worker before writes and domain definition. Already-started worker calls
are not cancellable by this API; revocation prevents later steps when observed,
but does not undo effects already completed. Libvirt has no transactional
ownership lock against other host tools. Library files must be trusted and not
edited in place while provisioning; identity checks are not content hashing or
protection against another malicious process running as the same user.

`dry_run: true` on a mutation validates its schema, media confinement/identity,
network eligibility and request preparation, then returns a read-only preview.
It reserves no request ID, never prompts and never dispatches a worker mutation.
It is **not** a full provisioning simulation: host capacity, supported OS/TPM,
image format/backing files, storage capacity and subnet conflicts are checked
only during real execution. The worker rejects any dry-run mutation envelope.

## Failure reasons

A `failed` or `declined` operation has `result.code`; failures the bridge helper
explains also have `result.reason`, a fixed sentence for that code.

| `code` | Meaning | What to do |
| --- | --- | --- |
| `helper_missing` | No helper at `/usr/local/libexec/omaware/authorize-bridge`. | Ask the user to install it. **Allow VMs** under Networks shows the command for their copy of OmaWare: `sudo install -D -o root -g root -m 0755 <OmaWare folder>/authorize-bridge /usr/local/libexec/omaware/authorize-bridge` (or `sudo cmake --install <build> --component helper` for a source build). |
| `helper_untrusted` | The helper, or a folder above it, isn't root-owned or is writable by others or a link, so OmaWare won't run it as root. | Ask the user to reinstall it with the command above. |
| `authorization_cancelled` | The user closed the administrator password prompt. | Ask before trying again. |
| `authorization_denied` | Wrong password, or polkit refused. | Ask the user. |
| `authorization_unavailable` | No pkexec, or no polkit authentication agent in the user's session. | The user has to authorize from their desktop session (**Allow VMs**). |
| `authorization_timeout` | Nobody answered the prompt within five minutes. | Ask the user, then retry with a new `request_id`. |
| `deny_rule` | A `deny` rule in the host's `bridge.conf` (or a file it includes) blocks the bridge. | Only the host administrator can change it. |
| `bridge_inactive` | The network's bridge isn't up. | `manage_network start`, then authorize again. |
| `network_not_owned` | Not a network OmaWare created. | Use an owned network. |
| `bridge_policy_unsafe` | The bridge policy files have unexpected owners, permissions or size. | The host administrator has to review them. |
| `qemu_bridge_helper_missing` | QEMU's own `qemu-bridge-helper` is missing or not root-owned. | The user installs QEMU's bridge helper package. |
| `helper_failed` | Anything else the helper reported. | See OmaWare's activity log. |
| `precondition_failed`, `state_changed` | The request no longer matched owned, active networks or media (before or after approval). | List inventory again. |
| `declined` | The user said no, or agent access was turned off. | Don't retry without asking. |
| `operation_failed` | Any other worker failure. | Inspect inventory and the activity log. |

The helper (`scripts/authorize-bridge.py`) exits with 3–7 for the refusals above;
helpers installed by older versions exit with 1 and are recognised by their message.

## After creation: networks, adapters and addresses

These are management tools: they answer when they finish (no `request_id`), and
every change waits for the user's approval in OmaWare.

- **`manage_network`** takes an owned `network` (UUID or name), an `action` and
  the `revision` from `list_owned_networks` or the last change:
  - `start`, `stop`: `stop` is refused (`in_use`) while any VM, running or not,
    has an adapter on it.
  - `set_autostart` with `autostart: true|false`: whether it starts with the
    computer. An owned network a VM uses should normally autostart.
  - `edit` with `mode`, `subnet`, `dhcp`, `dhcp_start`, `dhcp_end` (unchanged
    values are kept): only for a stopped network no VM uses (`network_active`,
    `in_use`). It stays stopped. A new subnet with DHCP on needs a new range.
  - `delete`: stops and removes an unused network. Networks that belong to a lab
    are refused (`lab_network`); use `delete_lab`.

  Revisions are checked before the approval prompt and again after it
  (`stale_revision`). The helper's `allow` line for a deleted network stays in
  `bridge.conf`; it only names that network's bridge.
- **`manage_network_adapter`** changes apply to a running VM when its guest
  supports PCI hot-plug. A paused VM, or a guest that doesn't release the
  adapter, keeps its old connection until a full shutdown and start; the response
  then has `pending_change_count > 0` and `restart_needed: true`. With
  `apply: "restart_if_needed"` (and optionally `restart_timeout_seconds`, 10–300,
  default 120) the same approval also covers a restart: OmaWare resumes a paused
  VM, asks the guest to shut down (guest agent or ACPI), waits, and starts it
  again. `restart.state` is `completed`, `not_needed`, or on failure
  `timed_out` (`code: restart_timed_out`) or `start_failed`. OmaWare never forces
  power off; after a timeout the VM keeps running, and the change stays saved and
  pending.
- **Session approvals:** an adapter change onto an owned isolated or host-only
  network, or a removal, can be approved for the rest of the session with a
  checkbox in OmaWare (off by default). Later such changes don't ask; their
  response has `approved_by: "session_grant"` (otherwise `"user"`). The grant
  ends when agent access is turned off, when the user revokes it, or after 8
  hours, and is never saved. Revision, ownership, availability and containment
  checks are unchanged. The private internet connection, NAT and every other
  tool always ask.
- **`serial_console`** sends text to a VM's serial console (`\n` is Enter) and
  returns the guest's output as text, until `wait_for` appears, the guest is
  quiet for a second, or `timeout_seconds` (1–60). It opens the console
  exclusively through libvirt (`console_unavailable` while another client has
  it), needs no network or guest agent, and logs only how much was typed.
  `type_login` with `via: "serial"` types a saved login there and masks the
  password if the guest echoes it.
- **Revisions in responses:** adapter changes, resource/ISO changes, `set_cable`,
  `manage_network`, `create_network` and `authorize_network` return the new
  `revision`. Use it for the next change instead of listing again.
- **`omaware_overview`** shows `pending_change_count` and `restart_needed` per
  VM, `autostart` per network, and per adapter `ips` with `ip_source`:
  `dhcp_lease` (a host network's DHCP), `arp` (this computer's neighbour table),
  `guest_agent` (asked inside the guest, for example a VM behind a pfSense LAN
  whose DHCP server is the router) or `unknown`. OmaWare never gives the host an
  address on isolated networks to find them.

## Safe verification

The registered `agent-tools` test exercises schemas, exact argument types and
bounds, library confinement (symlinks, hardlinks, FIFO, empty files), storage
confinement, changed media/revisions, worker envelope preparation, replay
admission and protocol results without a Backend or libvirt connection. When
qemu-img is installed, it additionally copies a tiny temporary raw fixture through
a pinned descriptor and verifies independent copied bytes despite replacement of
the source filename. `mcp-stdio` checks the real MCP executable with an isolated
runtime socket directory. Neither test reads real installation media or changes
VMs/networks. Live provisioning and administrator approval must be verified
separately on an explicitly authorized disposable environment. `agent-tools`
also checks the ordered `networks` list, the failure codes and their messages,
the trusted-helper check and the install commands.

The opt-in VM tests in `omaware-management-tests` cover the rest on a disposable
machine:
- `agentRestartTimesOut`: a guest that ignores shutdown is resumed, never forced
  off, and reported as `restart_timed_out` with the change pending.
- `agentNetworkWorkflow`, with `OMAWARE_ALLOWED_NETWORK_UUID` set to a network UUID
  whose bridge that machine's `bridge.conf` already allows (so no administrator
  step is needed). It checks:
  - `set_autostart` and refused stale revisions
  - `create_vm` with `["user", <network>]`: definition order, PCI order and the
    guest's `eth0`/`eth1`
  - `run_command` through the guest agent on the isolated network
  - the overview's `guest_agent` address
  - `in_use` refusals
  - `restart_if_needed` on a paused VM
  - `delete`
