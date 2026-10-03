// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QSet>
#include <QStringList>

// What deleting a VM removes: only files OmaWare made for that VM. Installation media from the
// libraries, disks somewhere else and anything another VM still uses are kept.
namespace VmFiles {
struct Plan {
    QStringList files;   // disks and first-boot seeds to delete
    QStringList trees;   // OmaWare's own per-VM folders (snapshots, restored disks), deleted whole
    QStringList folders; // folders made when the VM was created, deleted only once empty
    QStringList kept;    // files the VM used that are not deleted
};
// xmls are every definition the VM has had (current, snapshots, pending changes), since a restore
// keeps the disks and firmware variables it replaced. ownedTrees are OmaWare's per-VM folders; a
// file inside one belongs to the VM. So does a file in the "<name>-<first 8 characters of the
// UUID>" folder that creating the VM made for it, and libvirt's "<name>_VARS" firmware variables
// in nvramFolder.
Plan plan(const QStringList &xmls, const QString &uuid, const QStringList &ownedTrees, const QString &nvramFolder = {});
// Keeps anything another VM still uses, including a whole folder that holds such a file.
void exclude(Plan &plan, const QSet<QString> &referenced);
// Deletes what the plan lists and returns the disk space freed.
qint64 remove(const Plan &plan, QStringList &failures);
}
