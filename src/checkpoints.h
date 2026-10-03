// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QVariantList>
#include <QSet>
#include <QStringList>
#include <libvirt/libvirt.h>
#include <atomic>
#include <functional>
namespace Checkpoints {
using Progress = std::function<void(QString phase, qulonglong done, qulonglong total)>;
QVariantList list(QString uuid);
QVariantMap history(QString uuid);
bool create(virDomainPtr domain, QString xml, QString name, QString notes, QString &error, QVariantMap options = {}, const std::atomic_bool *cancel = nullptr, Progress progress = {});
bool createLive(virDomainPtr domain, QString xml, QString name, QString notes, const std::atomic_bool &cancel, Progress progress, QString &error, QVariantMap options = {}, bool holdForRestore = false);
bool restore(virConnectPtr connection, virDomainPtr domain, QString xml, QString id, QString &error, bool allowRestart = false, Progress progress = {}, const std::atomic_bool *cancel = nullptr, bool safety = false);
bool remove(virConnectPtr connection, QString uuid, QString id, QString &error, bool descendants = false, bool allowPinned = false, QStringList expectedIds = {});
bool edit(QString uuid, QString id, QVariantMap values, QString &error);
bool verify(QString uuid, QString id, const std::atomic_bool &cancel, Progress progress, QString &error);
QVariantMap storage(virConnectPtr connection, QString uuid);
bool cleanup(virConnectPtr connection, QString uuid, QString key, QString &error);
bool recover(virConnectPtr connection, virDomainPtr domain, QString &error);
QString clone(virConnectPtr connection, QString uuid, QString id, QString name, const std::atomic_bool &cancel, Progress progress, QString &error);
QString undoId(QString uuid);
// Every file a VM, pending change or snapshot record still needs. `ignoreUuid` leaves out that VM's
// own snapshot records (for deleting it).
bool references(virConnectPtr connection, QSet<QString> &paths, QString &error, const QString &ignoreUuid = {});
}
