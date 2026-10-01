# Security

## Reporting a problem

Please report security problems privately through GitHub's **Report a vulnerability** button on the [Security tab](https://github.com/iop6/Omaware/security), not in a public issue.

## How OmaWare protects you

- **No root.** OmaWare runs as you and manages VMs in your own libvirt session (`qemu:///session`). The only thing that runs as root is a small helper (`/usr/local/libexec/omaware/authorize-bridge`) that lets your VMs join networks OmaWare created. It runs through `pkexec`, asking for your password each time. It only accepts networks OmaWare created, and OmaWare refuses to run a copy that isn't owned by root in root-only folders.
- **Only its own VMs.** OmaWare changes only VMs and networks it created (named `omaware-*`, with its marker). Everything else is read-only.
- **The console never listens on the network.** libvirt hands OmaWare a connected socket to each VM's display.
- **Containment.** A contained VM may only use verified isolated switches: no internet, no route to this computer, no shared folders, clipboard, USB, device or TPM passthrough, vsock or network-exposed consoles. This is checked every time it starts, resumes, is restored or changed. AI agents can't use contained VMs.
- **Downloads are verified.** ISOs, cloud images and updates are kept only if they match their publisher's published checksum. Updates also check every file in the package against the package's own checksum list, and refuse links that point outside it.
- **Clipboard sharing** is off by default, chosen per VM, and switches off when the console moves to another VM.

## AI agents

Agent access is off until you turn it on in Settings. When it's on:

- Agents connect through a socket in your private runtime folder (`$XDG_RUNTIME_DIR`), which only your user can open. Any program running as you could use it, just as it could use libvirt directly; it gains no rights you don't have.
- Agents see and use only OmaWare's own VMs, never contained ones.
- Labs are built only after you approve them in OmaWare. The plan shows every command that will run inside the VMs.
- Agents never receive passwords. You set lab passwords in OmaWare; they're stored in your system's password store (or a private file only you can read), and VMs get only a SHA-512 hash. `type_login` types a password into a VM for the agent. An agent could still read it if it had it typed somewhere it's shown, so use a separate password for labs (**Generate** makes one).
- Restoring a snapshot and deleting a lab always ask you first.
- Lab VMs are reached over SSH with a key OmaWare makes for each lab, checked against a host key OmaWare made for each VM (no "trust on first use").
- What a VM shows on screen or prints can try to steer an agent (prompt injection). Keep VMs running untrusted software contained, and review what an agent proposes.
- Every agent action is in the activity log; typed text never is.

## Review, October 2026 (version 1.3.0)

The whole code base was reviewed for security problems and dead code before 1.3.0. Fixed:

- **Clipboard sharing carried over between VMs.** Choosing "To guest" or "Both directions" for one VM stayed on when the console switched to another, so text you copied could reach a VM you never shared it with. It now switches off when the console moves to a different VM.
- **Containment missed two host channels:** vsock devices and TPM passthrough. Both now block a contained VM.
- **Release-note links** in the update window could open any kind of link; only `https://` links are opened now.
- **Rich text:** labels showing VM, network or snapshot names and error messages now display them as plain text, so names containing markup can't change the interface or load images. Text from guests (guest-agent addresses and interface names) was already shown as plain text.
- **Lab SSH:** each VM's private host key is deleted from the lab folder once it's on the VM's setup disc.
- **`omaware mcp`** is no longer mistaken for a second OmaWare (which would have stopped OmaWare from starting while an agent was connected).
- Dead code removed: an unused interface function and property, an unused cloud-image cancel path (now used when OmaWare closes), and a console method exposed to the interface but only used internally.

Reviewed without changes: running external programs (always with argument lists, never a shell), domain and network XML (built with an XML API), file deletion (limited to OmaWare's own folders and checked names), the root helper, the updater, ISO import and deletion, snapshot storage cleanup, and the activity log (private to you).

Known limits, by design:

- Updates are verified against checksums published in the same GitHub release, not a separate signature, so they're as trustworthy as the GitHub account that publishes them.
- Allowing VMs to join networks makes QEMU's bridge helper setuid root (as most distributions already ship it) and adds the network's bridge to `/etc/qemu/bridge.conf`.
- Saved passwords in the desktop's password store can be read by other programs running as you while it's unlocked, as with any Secret Service.
