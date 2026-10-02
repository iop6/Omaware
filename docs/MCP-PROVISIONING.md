# Background provisioning through MCP

The development build supports local installation media and appliances without an
agent focusing or clicking the desktop. The user still approves mutations in
OmaWare's existing confirmation dialog. This is separate from `propose_lab`,
which remains the cloud-image, cloud-init lab builder.

## Tools and sequence

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
   `dhcp_end`. Host/libvirt subnet conflicts are checked by the worker. New
   networks have autostart off. There are no MCP network edit/delete/start/stop
   capabilities in this provisioning API.
3. `list_owned_networks {}` returns UUIDs, revisions, modes, active state and
   availability. `authorize_network` takes `network`, its expected `revision`
   and a new `request_id`. It calls the existing trusted bridge helper after
   approval; system/polkit authorization may still be required. MCP cannot
   supply passwords or install the helper. Refresh the list afterward.
4. `create_vm` takes a plain `name`, `media_kind`, exact library `media` filename,
   and a new `request_id`. Optional `cpus` (default 2), `memory_mib` (4096),
   `preset` (generic), `firmware` (bios), `tpm` (false), and `networks` (zero to
   four distinct owned, active, available network UUIDs) select the configuration.
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
This does not install Windows or FLARE automatically. pfSense can use its
verified local ISO or standalone disk and two owned network UUIDs; choose a
FreeBSD preset supported by the host's libosinfo. Guest installation, NIC
assignment and firewall policy are still guest tasks, not host-side scripts.
Use an isolated analysis network unless host/internet access is intentional.

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
only UUID/subnet/authorization fields, not XML, subprocess output, paths or
credentials.

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
separately on an explicitly authorized disposable environment.
