// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Internal to VmWorker::manage() and its handlers (management.cpp, hostnetworks.cpp, vmcreation.cpp,
// vmoperations.cpp and snapshotoperations.cpp).
#include "backend.h"
#include "virtutil.h"
#include <QDomDocument>
#include <QStringList>

// One manage() call, as its handlers see it. Every handler ends by calling done() or fail() exactly once.
struct VmWorker::Request {
    VmWorker &worker;
    // The operation as requested ("vm.create", "snapshots.restore", …).
    QString op;
    // The caller's input. Handlers read it with operator[], which adds missing keys, so checks that need the
    // input exactly as sent use `envelope`.
    QVariantMap &in;
    // Lists and statistics: they don't refresh the inventory or report to the activity log.
    bool query = false;
    // An agent's approved provisioning request (see agentprovision.h): the input exactly as approved, and the
    // media file it was approved with, opened and checked before any change.
    bool provisioning = false;
    QVariantMap envelope;
    int mediaFd = -1;

    bool fromAgent() const { return in.value("agentRequest").toBool(); }
    // Reports the result: to the activity log and inventory (unless a query), and to the caller.
    void done(bool ok, const QString &message, QVariantMap result = {}) const;
    void fail(const QString &message, const QVariantMap &result = {}) const { done(false, message, result); }
    // For a provisioning request: whether the approval still holds against fresh libvirt state, checked again
    // right before each change. Reports the failure itself. Always true for other requests.
    bool stillAuthorized() const;
};

// The VM a per-VM request is about, looked up afresh for each request.
struct VmWorker::Target {
    QString uuid;
    virDomainPtr domain;
    // Running or paused.
    bool active;
    // Its saved definition.
    QString xml;
};

namespace Management {
// The namespace of the ownership marker in each OmaWare VM's metadata. "prototype" is historical: changing
// it would make OmaWare stop recognizing the VMs it already created.
inline constexpr const char *ownershipNs = "https://omaware.org/xmlns/prototype/1";
// Runs a program and collects its standard output; `error` explains a failure.
bool run(const QString &program, const QStringList &args, QByteArray &output, QString &error, int timeout = 120000);
// Appends a new element (with optional text) to `parent`.
QDomElement child(QDomDocument &doc, QDomElement parent, const QString &name, const QString &content = {});
// Whether OmaWare created this host network.
bool managedNetwork(const QString &xml);
// Every host network's DHCP leases: IPv4 addresses by lower-case MAC.
QHash<QString, QStringList> dhcpLeases(virConnectPtr system);
}
