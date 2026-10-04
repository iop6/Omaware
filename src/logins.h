// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QVariantList>

// Logins for VMs that OmaWare sets up (user name and password), saved under a name such as a lab's.
// Passwords live in the desktop's password store (GNOME Keyring, KWallet or any other Secret Service).
// Without one, they go in a private file only you can read. User names and where each password is kept
// are listed in a plain index; passwords are never written there, into the project, or to agents.
namespace Logins {
// [{name, user, store}] where store is "keyring" or "file".
QVariantList list();
QString user(const QString &name);
bool exists(const QString &name);
// Saves (or replaces) a login. `store` says where the password went.
bool save(const QString &name, const QString &user, const QString &password, QString &store, QString &error);
bool password(const QString &name, QString &password, QString &error);
// A random password that is easy to read and type: four groups of letters and digits.
QString generate();
// Tests use a separate index and file, and never touch the real password store.
void setTestMode(const QString &folder);
}
