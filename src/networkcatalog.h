// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QVariantList>
#include <QString>
#include <libvirt/libvirt.h>

namespace NetworkCatalog {
QVariantMap describe(const QString &xml);
QVariantList discover(virConnectPtr session, QString &status);
}
