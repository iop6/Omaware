// SPDX-License-Identifier: GPL-3.0-or-later
#include "instance.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

InstanceGuard::InstanceGuard(const QString &lockPath) : lock_(lockPath) {
    // A lock left by a crashed copy is recognized by its dead process and taken over.
    lock_.setStaleLockTime(0);
}
QString InstanceGuard::defaultPath() {
    auto dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) dir = QDir::tempPath();
    return dir + "/omaware.lock";
}
QList<qint64> InstanceGuard::otherProcesses(const QString &name) {
    QList<qint64> result;
    const qint64 self = QCoreApplication::applicationPid();
    // /proc/<pid>/exe is only readable for this user's own processes.
    for (const auto &entry : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const qint64 pid = entry.toLongLong(&numeric);
        if (!numeric || pid == self) continue;
        auto target = QFileInfo("/proc/" + entry + "/exe").symLinkTarget();
        if (target.endsWith(" (deleted)")) target.chop(10);
        if (target.isEmpty() || QFileInfo(target).fileName() != name) continue;
        // `omaware mcp` (started by an AI agent) is a helper without a window, not another OmaWare.
        QFile cmdline("/proc/" + entry + "/cmdline");
        if (cmdline.open(QIODevice::ReadOnly) && cmdline.read(4096).split('\0').value(1) == "mcp") continue;
        result << pid;
    }
    return result;
}
bool InstanceGuard::acquire() {
    if (!lock_.tryLock(0)) {
        qint64 pid = 0; QString host, app;
        lock_.getLockInfo(&pid, &host, &app);
        blocker_ = pid > 0 ? QString("OmaWare is already open (process %1).").arg(pid) : QString("OmaWare is already open.");
        return false;
    }
    const auto others = otherProcesses();
    if (!others.isEmpty()) {
        lock_.unlock();
        blocker_ = QString("An older version of OmaWare is already open (process %1).").arg(others.first());
        return false;
    }
    return true;
}
