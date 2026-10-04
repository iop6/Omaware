// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"

QString Diagnostics::nextStep(const QString &message) {
    const auto text = message.toLower();
    if (text.contains("bridge") && (text.contains("helper") || text.contains("denied") || text.contains("authoriz")))
        return "Open Networks, select the network, and authorize its bridge for session VMs.";
    if (text.contains("permission") || text.contains("authentication") || text.contains("access denied"))
        return "Check access to the named file or connection. Host network changes may require administrator "
               "authentication.";
    if (text.contains("configuration changed") || text.contains("pending-change"))
        return "Refresh Details and review the current configuration before retrying.";
    if (text.contains("no space") || text.contains("free space"))
        return "Choose a storage location with sufficient free space.";
    if (text.contains("lock") && text.contains("disk"))
        return "Shut down the VM using this disk before copying or changing its storage.";
    if (text.contains("agent")) return "Install and start qemu-guest-agent in the guest, then refresh its details.";
    if (text.contains("network is not active") || text.contains("network is stopped"))
        return "Start the network from Networks, then start the VM.";
    if (text.contains("connect") || text.contains("socket"))
        return "Check that the local libvirt service is available, then use Reconnect.";
    if (text.contains("virt-install"))
        return "Install virt-install and the libosinfo database on the machine running OmaWare.";
    if (text.contains("file") || text.contains("path") || text.contains("directory"))
        return "Check that the selected local path exists and your account can access it.";
    return "Review the operation details, correct the indicated setting, and try again.";
}

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

QVariantMap Diagnostics::advice(const QString &message) {
    const auto text = message.toLower();
    QString summary = message.section('\n', 0, 0).left(180), action = "review", label = "Review settings";
    if (text.contains("bridge") || text.contains("network is not active") || text.contains("network is stopped")) {
        summary = "The VM's network needs attention.";
        action = "networks";
        label = "Open Networks";
    } else if (text.contains("configuration changed") || text.contains("pending-change")) {
        summary = "The saved settings changed. Refresh before trying again.";
        action = "refresh";
        label = "Refresh details";
    } else if (text.contains("no space") || text.contains("free space")) {
        summary = "There isn't enough space in the selected location.";
        action = "storage";
        label = "Review storage";
    } else if (text.contains("file") || text.contains("path") || text.contains("directory")) {
        action = "source";
        label = "Review file location";
    } else if (text.contains("reconnect") || text.contains("connect") || text.contains("socket")) {
        summary = "The connection needs attention.";
        action = "connection";
        label = "Reconnect";
    }
    return {{"summary", summary}, {"action", action}, {"label", label}, {"nextStep", nextStep(message)}};
}

ActivityLog::ActivityLog(QString path)
    : path_(path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/activity.json"
                           : std::move(path)) {}

QVariantList ActivityLog::load() const {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) return {};
    const auto data = QJsonDocument::fromJson(file.readAll()).toVariant().toList();
    QVariantList result;
    for (const auto &value : data) {
        const auto entry = value.toMap();
        if (entry["message"].toString().isEmpty() ||
                !QDateTime::fromString(entry["time"].toString(), Qt::ISODateWithMs).isValid())
            continue;
        result.append(QVariantMap{{"time", entry["time"]}, {"ok", entry["ok"].toBool()},
                {"message", entry["message"].toString().left(4096)},
                {"nextStep", entry["nextStep"].toString().left(1024)}, {"id", entry["id"].toString()},
                {"connection", entry["connection"].toString().left(256)}});
        if (result.size() == 200) break;
    }
    return result;
}

bool ActivityLog::save(const QVariantList &entries, QString &error) const {
    QSaveFile file(path_);
    const auto bytes = QJsonDocument::fromVariant(entries).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
            file.write(bytes) != bytes.size() || !file.commit()) {
        error = "Activity history could not be saved: " + file.errorString();
        return false;
    }
    return true;
}

bool ActivityLog::append(QVariantMap entry, QVariantList &entries, QString &error) const {
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        error = "Activity history folder is unavailable.";
        return false;
    }
    QLockFile lock(path_ + ".lock");
    if (!lock.tryLock(0)) {
        error = "Activity history is being updated by another instance.";
        return false;
    }
    entries = load();
    entry["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry["time"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    entry["message"] = entry["message"].toString().left(4096);
    entry["nextStep"] = entry["nextStep"].toString().left(1024);
    entries.prepend(entry);
    while (entries.size() > 200)
        entries.removeLast();
    return save(entries, error);
}

bool ActivityLog::clear(QString &error) const {
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        error = "Activity history folder is unavailable.";
        return false;
    }
    QLockFile lock(path_ + ".lock");
    if (!lock.tryLock(0)) {
        error = "Activity history is being updated by another instance.";
        return false;
    }
    return save({}, error);
}
