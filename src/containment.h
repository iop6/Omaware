// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <libvirt/libvirt.h>

// Containment for VMs running untrusted software: a per-VM policy stored in the libvirt definition
// and enforced by every OmaWare path that can start, resume or reconfigure the VM.
namespace Containment {
inline constexpr const char *ns = "https://omaware.org/xmlns/containment/1";
bool enabled(const QString &domainXml);
// Adds or removes the policy marker in a full domain definition.
QString withMarker(const QString &domainXml, bool enabled);
// Live, unprivileged verification of one host network definition and its bridge.
// Returns {isolated, bridge, name, checks: [{label, ok, detail}]}.
QVariantMap verifyNetwork(const QString &networkXml, bool active, const QString &sysRoot = "/");
// Bridge name -> verification for every system libvirt network (read-only connection).
QHash<QString, QVariantMap> hostBridges();
// Policy evaluation. Violations block start; warnings are shown but allowed.
// Returns {violations: [text], warnings: [text]}.
QVariantMap check(const QString &domainXml, const QHash<QString, QVariantMap> &bridges);
QVariantMap check(const QString &domainXml);
QString summary(const QVariantMap &result);
// Empty when the domain may start/resume; otherwise the reason, for contained VMs only.
QString blocker(virDomainPtr domain, bool live);
}
