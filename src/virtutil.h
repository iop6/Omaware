// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <libvirt/libvirt.h>
#include <memory>

// Small helpers shared by the code that calls libvirt directly.

// Owning handles that free the libvirt object when they go out of scope.
using Domain = std::unique_ptr<virDomain, decltype(&virDomainFree)>;
using Connection = std::unique_ptr<virConnect, decltype(&virConnectClose)>;
using Network = std::unique_ptr<virNetwork, decltype(&virNetworkFree)>;
using Snapshot = std::unique_ptr<virDomainSnapshot, decltype(&virDomainSnapshotFree)>;
using Stream = std::unique_ptr<virStream, decltype(&virStreamFree)>;

namespace Virt {
// "context: libvirt's message for the last failed call".
QString lastError(const QString &context);
// The domain's XML (virDomainGetXMLDesc flags), or empty when libvirt can't provide it.
QString domainXml(virDomainPtr domain, unsigned flags);
// The saved definition (with secrets), or the running one when `live` is set.
QString definition(virDomainPtr domain, bool live = false);
// A network's saved definition, or its running one for a transient network.
QString networkXml(virNetworkPtr network);
QString uuidOf(virDomainPtr domain);
// The VM's name as the user knows it: without OmaWare's "omaware-" prefix.
QString displayName(virDomainPtr domain);
}
