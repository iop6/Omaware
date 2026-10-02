// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QVariantMap>
#include <QVariantList>
#include <QHash>
namespace AgentProvision {
QVariantList tools();
bool handles(const QString &tool);
bool validate(const QString &tool, const QVariantMap &args, QString &error);
// Read-only preparation. Network inventory must come from the current backend.
bool prepare(const QString &tool, const QVariantMap &args, const QVariantList &networks, QVariantMap &input, QString &error);
bool verifyEnvelope(const QString &op, const QVariantMap &input, const QVariantList &networks, QString &error);
QString admission(const QString &id, const QVariantMap &request, const QHash<QString,QVariantMap> &requests, const QHash<QString,QVariantMap> &states);
QVariantList media();
// Confined default storage only; read-only by default, worker may create the final vms directory.
bool storageRoot(QString &error, bool create = false);
QString mediaPath(const QString &kind, const QString &name);
// Opens every directory component without following links; caller owns the returned descriptor.
int openMedia(const QString &kind, const QString &name, QVariantMap &identity, QString &error);
}
