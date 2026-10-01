// SPDX-License-Identifier: GPL-3.0-or-later
#include "paths.h"
#include <QDir>
#include <QStandardPaths>

QString Paths::root() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/omaware"; }
QString Paths::vms() { return root() + "/vms"; }
QString Paths::isos() { return root() + "/isos"; }
QString Paths::legacyVms() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/vms"; }
QString Paths::vmDir(const QString &uuid) {
    const auto legacy = legacyVms() + "/" + uuid;
    return QDir(legacy).exists() ? legacy : vms() + "/" + uuid;
}
