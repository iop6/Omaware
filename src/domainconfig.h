// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QVariantMap>
#include <QString>

namespace DomainConfig {
QVariantMap describe(const QString &xml, QString &error);
QString revision(const QString &xml);
// Produces only one device XML. The backend applies it through libvirt's device API.
bool networkDevice(const QString &domainXml, const QString &mac, const QString &kind,
    const QString &source, const QString &model, bool linkUp, bool remove,
    QString &deviceXml, QString &error);
bool bridgeAllowed(const QString &bridge, const QString &aclPath = "/etc/qemu/bridge.conf");
}
