# Networks in OmaWare

- [Connection types](#connection-types)
- [Connecting a VM](#connecting-a-vm)
- [Networks](#networks)
- [The network map](#the-network-map)
- [How “VMs only” isolation is checked](#how-vms-only-isolation-is-checked)
- [Containment](#containment)
- [Limits](#limits)

## Connection types

Each VM network adapter is connected to one of these. OmaWare uses the plain names on the left everywhere.

| Connection | What the VM can reach |
| --- | --- |
| **Internet · private to this VM** | The internet, through your computer. Each VM gets its own private connection, so other VMs can't see it and this computer can't reach it directly. No setup needed; new VMs start with this. |
| **Internet + VMs** network | The internet, your computer, and other VMs on the same network. |
| **This computer + VMs** network | Your computer and other VMs on the network. No internet. |
| **VMs only** network | Only other VMs on the same network. Not your computer, not the internet. OmaWare checks this live; see [below](#how-vms-only-isolation-is-checked). |
| **Local network** (bridge) | Your local network (LAN), as if the VM were plugged into it. Needs an existing Linux bridge attached to the LAN; OmaWare doesn't create one. |
| **Routed / open** networks | Existing libvirt networks; what they reach depends on your computer's routing and firewall. |

Choices that can't be used yet say why, for example a stopped network or one your VMs don't have permission to join yet.

## Connecting a VM

Use the [map](#the-network-map) or **Details → Networks** (**Edit** an adapter or **Add adapter**). A VM can have several connections.

**Changes apply straight away.** On a running VM, OmaWare plugs the new adapter in, swaps it, or unplugs it while the VM runs, the way you'd move a cable to another socket. The guest OS has to support this (most Linux and Windows guests do). If it doesn't respond, for example while it's still booting or the VM is paused, the change is saved and takes effect after a full shutdown and start, and OmaWare says so. An AI agent can ask for that restart together with the change; you approve both at once, and OmaWare asks the guest to shut down rather than forcing it off.

**Adapter order.** The guest numbers network cards by their PCI slot. A new VM's adapters get slots in the order they're created, so the first is `vtnet0` on pfSense or FreeBSD and the first card on Linux and Windows; put a router's WAN first (agents pass the order to `create_vm`). An adapter added later gets the next free slot, which can be one a removed adapter left behind, so check the guest's own numbering after removing and adding adapters. A VM imported from another hypervisor sees new network cards (new MAC addresses and PCI slots): its saved settings may still name the old ones (for example netplan matching VMware's `ens33` while the card is now `enp6s0`), and then it has no address until you fix that inside the guest.

## Networks

**Networks → New network** asks one question: what should VMs on it reach?

- **Internet:** each other, this computer and the internet.
- **This computer:** each other and this computer, no internet.
- **Only each other:** nothing gets in or out.

You can tick VMs to connect straight away; each gets an extra connection to the new network, live if it's running. The addresses are chosen for you: OmaWare picks a free `192.168.x.0/24` subnet and hands out addresses automatically (DHCP). **Address settings** lets you choose your own.

Your VMs run as your user, so they need permission to join a network's bridge. Creating a network asks for your password once to grant it. For a network that needs it later, use **Allow VMs** on its card or in the map's side panel. This runs a small helper as root, which only accepts networks OmaWare created. It has to be owned by root, so OmaWare never runs a copy you could modify; the [one-command install](USER-GUIDE.md#installing) puts it in place (and updates it), and doesn't need cmake.

If the helper is missing, **Allow VMs** says so and shows the command for your copy of OmaWare, with the real path of the `authorize-bridge` script that comes with it (in the app's folder, or the build folder for a source build):

```sh
sudo install -D -o root -g root -m 0755 <OmaWare folder>/authorize-bridge /usr/local/libexec/omaware/authorize-bridge
```

From a source checkout, `scripts/authorize-bridge.py` is the same script, and with cmake installed `sudo cmake --install build/app --component helper` does the same.

When allowing fails, OmaWare's message says why: the password prompt was closed or refused, a `deny` rule in `/etc/qemu/bridge.conf` blocks the bridge, the network isn't running, or the helper or QEMU's bridge helper is missing or not safely owned by root. AI agents get the same reason as a short code (see [Failure reasons](MCP-PROVISIONING.md#failure-reasons)).

For networks created outside OmaWare, add `allow <bridge>` to `/etc/qemu/bridge.conf` yourself.

**Networks → List** shows each network with its type, addresses, status, the VMs on it, any problem with its fix, and **Start**, **Stop**, **Edit**, **Show on map** and **Remove**. OmaWare only changes networks it created. A network can't be stopped, edited or removed while a VM (running or not) still uses it. Subnets already in use are refused.

**Starts with the computer** is on by default, for networks made here, by labs and by AI agents. Turn it off under **Edit** (an agent uses `manage_network`). When you start a VM whose network is stopped (after a reboot, say), OmaWare starts that network first, with the same administrator prompt as **Start**; a network made outside OmaWare has to be started by you.

## The network map

**Networks → Map** shows everything as a diagram: the internet at the top, your computer below it, then each network with its VMs underneath, in a shaded area. Every VM adapter is a cable.

- **Cable colors** show where a cable leads: amber to the internet (or your local network), blue to this computer only, green to other VMs only. A red dashed cable is pulled; a grey dashed one is waiting for the VM's next full start. **Key** in the corner explains this; it folds away on small windows.
- **Each VM shows its IP address** when OmaWare can find it (from the network's DHCP leases or your computer's neighbour table). VMs on a private internet connection have a private address that this computer can't reach. VMs on a "VMs only" network that a guest router serves (pfSense's LAN, for example) have no address this computer can see; AI agents can still get it from the VM's QEMU guest agent when one runs, and OmaWare never adds a host address to the network for that.
- **Move** anything by dragging it; positions are remembered. Scroll to zoom, drag the background to pan, and use **Tidy up** and **Fit to view** (`F`). Cables that would pass behind another VM go around it.
- **Double-click a VM** to open its console. Double-click a stopped, unused network to edit it.
- **Connect a VM** by dragging from the + on its right side onto a network, or onto *This computer* for a private internet connection. If the VM already has connections, you choose between adding one and moving an existing one. **Connect to** in the side panel or the VM's right-click menu does the same without dragging, and **New network with this VM…** creates a network with it.
- **Pull or plug a cable** by double-clicking its port on the VM (the jack with the link light), or with **Pull** / **Plug in** in the side panel. This works instantly, even on a running VM, and a pulled cable stays pulled after a restart.
- **Disconnect** pulls every cable on one VM; **Disconnect all** does the same for a network. **Cut off internet** pulls every cable, on running and stopped VMs, that leads to the internet or your local network.
- **The side panel** answers the usual questions for whatever is selected: whether a VM is online and through what path, its connections and IP addresses (with a copy button), and for a network, what it's for, which VMs are on it and what, if anything, needs fixing.

### Can this VM reach the internet?

Every VM shows a badge worked out from its cables and the networks they lead to:

| Badge | Meaning |
| --- | --- |
| **INTERNET** | At least one connected cable leads to the internet. |
| **LOCAL NETWORK** | A connected cable leads to your local network, which usually means the internet too. |
| **THIS COMPUTER** | Only your computer (and other VMs) can be reached. |
| **VMS ONLY** | Only other VMs on "VMs only" networks. |
| **OFFLINE** | No connected cables. |

The badge comes from the configuration and cable state, not from testing a connection inside the VM.

## How “VMs only” isolation is checked

A "VMs only" network is meant to let VMs talk only to each other. OmaWare creates it with no NAT or routing, no address for your computer, no DHCP or DNS, and IPv6 disabled, then checks it live. It shows **Verified isolated** only when all of these hold (the individual checks are under **Isolation checks**):

- no forwarding, NAT or routing in its definition
- no host address, DHCP or DNS, and IPv6 not enabled
- the network is running and its bridge exists
- your computer has no IPv4 or IPv6 address on the bridge, and IPv6 is disabled on it
- only VM ports are attached to the bridge, no physical or host network cards

These checks need no administrator rights. libvirt's firewall rules also block traffic from leaving the bridge; OmaWare can't read those without root and doesn't claim to.

Isolated networks created by early builds could leave an IPv6 link-local address on the bridge. They show the failed check; recreate them.

## Containment

**Details → Networks → Containment → Contain VM** marks a VM as running untrusted software, such as unknown downloads or security testing. The mark is stored in the VM's own libvirt definition. While a VM is contained, OmaWare blocks:

| Blocked | Why |
| --- | --- |
| Every connection except verified “VMs only” networks (or no network) | Reaching the internet, your LAN or your computer |
| Shared folders, and disks other than local image files (network storage, host block or NVMe devices, host folders as disks, SCSI passthrough) | Access to your files, disks or other computers |
| Clipboard sharing | Data reaching your desktop |
| USB and PCI passthrough, USB redirection, smartcards, keyboard or input passthrough, TPM passthrough, shared memory, vsock | Direct access to your hardware, input or memory |
| Host audio | Sound reaching your speakers, or your microphone reaching the VM |
| Serial ports and channels that reach a host device, pipe, service or the network, and consoles that listen on the network (including VNC's default local TCP port) | Paths to programs on your computer |
| 3D acceleration and GPU-rendered displays | Guest graphics code running in your computer's GPU stack |
| Raw QEMU settings (`qemu:commandline`) | Devices and connections OmaWare can't check |

The QEMU guest agent is allowed but listed as a warning: it's a channel between OmaWare and the guest, not a network, and it lets you run commands in the VM. **AI agents can't use contained VMs at all**, the guest agent included: software inside controls what comes back (command output, addresses, screens), and that could try to steer the agent. Only you decide what to run in a contained VM.

Containment is enforced by OmaWare itself, not only hidden in the interface:

- A VM can only be contained once it already meets the rules, and containing it turns off autostart.
- Network changes, hardware edits, plugging cables back in, and snapshot reverts are refused if they would break the rules.
- Every start and resume checks the VM's definition again, so a network adapter added with another tool blocks the start.
- Copies made with **Create VM from snapshot** are contained too.
- Removing containment asks for confirmation.

## Limits

- **Containment doesn't sandbox QEMU.** VMs in your libvirt session run as your user. If software escaped the VM through a QEMU bug, it would have your user's access. For high-risk software, use a dedicated computer or user account.
- OmaWare enforces its rules on its own actions and at start time. Another tool with libvirt access can still start a VM that breaks them, or change host networking.
- Isolation checks run when they're shown or when a VM starts, not continuously.
- “VMs only” networks block network traffic, not side channels such as timing.
- Return to a known-clean snapshot after running untrusted software; **Verify stored files** confirms snapshot files haven't changed.
