// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QVariantMap>
#include <QVariantList>

// Immutable parentId links plus deletion redirects describe the visible tree.
// The marker identifies the working parent. Disk dependencies belong to Checkpoints.
namespace SnapshotHistory {
QVariantMap view(const QVariantList &snapshots, const QVariantMap &marker);
QVariantMap captured(QVariantMap marker, const QVariantMap &snapshot);
QVariantMap restored(const QVariantList &snapshots, QVariantMap marker, QString id, QString &error);
void removed(QVariantMap &marker, const QVariantMap &snapshot);
QString survivingParent(const QVariantMap &marker, QString id);
}
