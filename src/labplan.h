// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QStringList>
#include <QVariantMap>

// A lab: a few networks and VMs described in one plan, usually written by an AI agent from what the user
// asked for. check() turns the plan into a normalized form (names, sizes, addresses filled in) and lists
// anything that must change before it can be built. It never touches the computer.
//
// Plan:
//   {"name": "Web lab", "user": "alex",
//    "networks": [{"name": "dmz", "type": "internet" | "private" | "isolated", "subnet": "10.20.0.0/24"}],
//    "vms": [{"name": "web1", "os": "ubuntu", "cpus": 2, "memory_mib": 2048, "disk_gib": 20,
//             "networks": ["dmz", {"network": "lan", "ip": "10.30.0.5"}],
//             "packages": ["nginx"], "setup": ["shell command run once as root on first boot"]}]}
namespace LabPlan {
struct Host {
    int cpus = 1;
    qint64 memoryMiB = 0;
    QStringList images;      // operating systems available as cloud images, e.g. "ubuntu"
    QStringList existingVms; // names already used by VMs (without the omaware- prefix)
    QStringList existingNetworks;
};

// {ok, problems: [..], warnings: [..], plan: normalized}. The normalized plan has
// networks [{name, fullName, type, mode, subnet, prefix}] and
// vms [{name, os, cpus, memoryMiB, diskGiB, nics: [{network, ip}], packages, setup, internet, reachable}].
QVariantMap check(const QVariantMap &plan, const Host &host);
// A short name for files and network names: "Web lab" -> "web-lab".
QString slug(const QString &name);
}
