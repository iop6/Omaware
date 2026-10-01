// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// Labs OmaWare has built: for each, its plan, the networks and VMs it made, the login it was set up with
// (by name; the password is in the password store) and the SSH key OmaWare uses to run commands in its
// VMs. Kept in OmaWare's application data folder, one private folder per lab.
namespace Labs {
// XML namespace of the <omalab:lab> tag in a lab VM's metadata.
inline constexpr const char *ns = "https://omaware.org/xmlns/lab/1";
QString folder();
QString folderOf(const QString &slug);
QVariantList list();
QVariantMap load(const QString &slug);
bool save(const QVariantMap &lab);
bool remove(const QString &slug);
// The lab's private SSH key (created when the lab is built).
QString keyPath(const QString &slug);
}
