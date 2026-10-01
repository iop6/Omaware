// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QList>
#include <QLockFile>
#include <QString>

// Keeps OmaWare to one running copy per user: two copies would both manage the same VMs,
// snapshots and settings. A lock file covers current versions; a process scan catches
// older versions that predate the lock.
class InstanceGuard {
public:
    explicit InstanceGuard(const QString &lockPath);
    // True when no other OmaWare runs for this user. The lock is then held until destruction.
    bool acquire();
    // Why acquire() failed, for the user.
    QString blocker() const { return blocker_; }
    // Default lock location, in the user's runtime directory.
    static QString defaultPath();
    // Other processes of this user running an executable named `omaware`.
    static QList<qint64> otherProcesses(const QString &name = "omaware");
private:
    QLockFile lock_;
    QString blocker_;
};
