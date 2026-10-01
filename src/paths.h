// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>

// Where OmaWare keeps VMs and installation media: one ordinary per-user data folder,
// ~/.local/share/omaware (or $XDG_DATA_HOME/omaware), with vms/ and isos/ inside.
// Snapshots, pending changes and the activity log stay in Qt's application data folder.
namespace Paths {
QString root();
QString vms();
QString isos();
// VM folders made by earlier versions, in Qt's application data folder. They keep working.
QString legacyVms();
// The folder for one VM's extra disks and restores: its existing folder if it has one, else a new one under vms().
QString vmDir(const QString &uuid);
}
