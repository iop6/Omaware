// SPDX-License-Identifier: GPL-3.0-or-later
#include "virtutil.h"
#include <QRegularExpression>
#include <libvirt/virterror.h>
#include <cstdlib>

namespace Virt {
QString lastError(const QString &context) {
    const auto e = virGetLastError();
    return context + ": " + (e && e->message ? QString::fromUtf8(e->message) : QString("Unknown libvirt error"));
}

QString domainXml(virDomainPtr domain, unsigned flags) {
    char *raw = virDomainGetXMLDesc(domain, flags);
    QString xml = raw ? QString::fromUtf8(raw) : QString{};
    free(raw);
    return xml;
}

QString definition(virDomainPtr domain, bool live) {
    return domainXml(domain, VIR_DOMAIN_XML_SECURE | (live ? 0 : VIR_DOMAIN_XML_INACTIVE));
}

QString networkXml(virNetworkPtr network) {
    char *raw = virNetworkGetXMLDesc(network, virNetworkIsPersistent(network) == 1 ? VIR_NETWORK_XML_INACTIVE : 0);
    QString xml = raw ? QString::fromUtf8(raw) : QString{};
    free(raw);
    return xml;
}

QString uuidOf(virDomainPtr domain) {
    char uuid[VIR_UUID_STRING_BUFLEN];
    return virDomainGetUUIDString(domain, uuid) == 0 ? QString::fromLatin1(uuid) : QString{};
}

QString displayName(virDomainPtr domain) {
    return QString::fromUtf8(virDomainGetName(domain)).remove(QRegularExpression("^omaware-"));
}
}
