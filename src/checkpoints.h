// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <libvirt/libvirt.h>
#include <atomic>
#include <functional>

// Snapshots of OmaWare VMs (called checkpoints in the code): independent copies of a VM's disks, firmware
// variables, TPM state and, for live ones, memory, stored per VM in OmaWare's data folder. Every function
// returns false (or empty) on failure with a message for the user in `error`.
namespace Checkpoints {
// Reports a long operation's current phase and, when known, how far along it is.
using Progress = std::function<void(QString phase, qulonglong done, qulonglong total)>;

// The VM's snapshots as a tree in display order: each with its parent, depth and health.
QVariantList list(QString uuid);
// list() with where the VM currently is in the tree (see SnapshotHistory::view).
QVariantMap history(QString uuid);
// Copies a stopped VM's disks and firmware into a new snapshot. `options`: tags, pinned, knownGood, safety,
// preview (a PNG data URL).
bool create(virDomainPtr domain, QString xml, QString name, QString notes, QString &error, QVariantMap options = {},
        const std::atomic_bool *cancel = nullptr, Progress progress = {});
// Snapshots a running or paused VM through a libvirt backup job while it keeps running. `options` adds memory
// (save RAM and device state, to resume exactly there), clean (freeze guest filesystems through the guest
// agent) and incremental (copy only what changed since the previous snapshot of the same run).
// `holdForRestore` leaves a memory snapshot's VM paused for the restore that asked for it.
bool createLive(virDomainPtr domain, QString xml, QString name, QString notes, const std::atomic_bool &cancel,
        Progress progress, QString &error, QVariantMap options = {}, bool holdForRestore = false);
// Switches the VM to snapshot `id`. A running VM is replaced (`allowRestart` confirms that): resumed from the
// snapshot's memory if it has some, else restarted. `safety` first takes a recovery snapshot that undoId()
// then offers. A journal (restore.json) makes an interruption recoverable with recover().
bool restore(virConnectPtr connection, virDomainPtr domain, QString xml, QString id, QString &error,
        bool allowRestart = false, Progress progress = {}, const std::atomic_bool *cancel = nullptr,
        bool safety = false);
// Deletes a snapshot (with `descendants`, its whole branch, which must be exactly `expectedIds`). Pinned ones
// need `allowPinned`. Files are reclaimed only once nothing else needs them.
bool remove(virConnectPtr connection, QString uuid, QString id, QString &error, bool descendants = false,
        bool allowPinned = false, QStringList expectedIds = {});
// Changes a snapshot's name, notes, tags, pinned or known-good flag.
bool edit(QString uuid, QString id, QVariantMap values, QString &error);
// Checks the snapshot and the snapshots it builds on: disk structure, and contents against their last checksums.
bool verify(QString uuid, QString id, const std::atomic_bool &cancel, Progress progress, QString &error);
// Storage use of the VM's snapshots, and the retained folders that could be reclaimed (with cleanup()).
QVariantMap storage(virConnectPtr connection, QString uuid);
bool cleanup(virConnectPtr connection, QString uuid, QString key, QString &error);
// Finishes or rolls back an interrupted restore, and thaws or resumes a guest an interrupted snapshot froze or
// paused.
bool recover(virConnectPtr connection, virDomainPtr domain, QString &error);
// A new, stopped VM from a snapshot, with its own copies of the files, new MAC addresses and unplugged cables.
// Returns the new VM's UUID.
QString clone(virConnectPtr connection, QString uuid, QString id, QString name, const std::atomic_bool &cancel,
        Progress progress, QString &error);
// The recovery snapshot that undoes the last restore, or empty.
QString undoId(QString uuid);
// Every file a VM, pending change or snapshot record still needs. `ignoreUuid` leaves out that VM's
// own snapshot records (for deleting it).
bool references(virConnectPtr connection, QSet<QString> &paths, QString &error, const QString &ignoreUuid = {});
}
