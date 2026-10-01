// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include "containment.h"
#include "domainconfig.h"
#include "networkcatalog.h"
#include "configuration.h"
#include "diagnostics.h"
#include <QDateTime>
#include <QDomDocument>
#include <QRegularExpression>
#include <QJsonDocument>
#include <algorithm>
#include <QFileInfo>
#include <QMetaObject>
#include <QUuid>
#include <QXmlStreamReader>
#include <libvirt/virterror.h>
#include <cstdlib>

// Ownership marker in each OmaWare VM's metadata. The "prototype" in the URL is historical; changing it
// would make OmaWare stop recognizing VMs it already created.
static constexpr const char *marker = "https://omaware.org/xmlns/prototype/1";
QString VmWorker::error(const QString &context) {
    auto e = virGetLastError();
    return context + ": " + (e && e->message ? QString::fromUtf8(e->message) : "Unknown libvirt error");
}
bool VmWorker::owned(virDomainPtr d) {
    char *metadata = virDomainGetMetadata(d, VIR_DOMAIN_METADATA_ELEMENT, marker, 0);
    bool result = metadata != nullptr && QString::fromUtf8(virDomainGetName(d)).startsWith("omaware-");
    free(metadata);
    return result;
}
int VmWorker::event(virConnectPtr, virDomainPtr, int, int, void *opaque) {
    auto self = static_cast<VmWorker *>(opaque);
    auto generation = self->generation_.load();
    QMetaObject::invokeMethod(self, [self, generation] {
        if (generation != self->generation_) return;
        emit self->lifecycle(); self->refresh();
    }, Qt::QueuedConnection);
    return 0;
}
void VmWorker::closed(virConnectPtr, int, void *opaque) {
    auto self = static_cast<VmWorker *>(opaque);
    auto generation = self->generation_.load();
    QMetaObject::invokeMethod(self, [self, generation] {
        if (generation != self->generation_) return;
        self->stop();
        emit self->inventory({});
        emit self->connection(false, "Backend disconnected. Use Reconnect to recover authoritative state.");
    }, Qt::QueuedConnection);
}
void VmWorker::open() {
    stop();
    // Only the local user session is supported: no remote connections or implicit default URI.
    if (uri_ != "qemu:///session") {
        emit connection(false, "OmaWare only supports the local user session (qemu:///session); system VMs stay separate.");
        return;
    }
    static const int registered = virEventRegisterDefaultImpl();
    if (registered < 0) { emit connection(false, error("Event registration")); return; }
    conn_ = virConnectOpen(uri_.toUtf8().constData());
    if (!conn_) { emit connection(false, error("Connect to " + uri_)); return; }
    callback_ = virConnectDomainEventRegisterAny(conn_, nullptr, VIR_DOMAIN_EVENT_ID_LIFECYCLE,
        VIR_DOMAIN_EVENT_CALLBACK(event), this, nullptr);
    if (callback_ < 0) { auto message = error("Subscribe to lifecycle events"); stop(); emit connection(false, message); return; }
    virConnectRegisterCloseCallback(conn_, closed, this, nullptr);
    // Wake the default event loop on shutdown; domain state itself is event-driven.
    timer_ = virEventAddTimeout(-1, [](int, void *) {}, nullptr, nullptr);
    if (timer_ < 0) { auto message = error("Event wakeup registration"); stop(); emit connection(false, message); return; }
    running_ = true;
    events_ = std::thread([this] {
        while (running_) {
            if (virEventRunDefaultImpl() < 0) { closed(nullptr, 0, this); break; }
        }
    });
    emit connection(true, "Connected to user-owned VMs · " + uri_);
    refresh();
}
void VmWorker::stop() {
    if (restartTimer_) restartTimer_->stop();
    restartUuid_.clear();
    running_ = false;
    if (timer_ >= 0) virEventUpdateTimeout(timer_, 0);
    if (events_.joinable()) events_.join();
    ++generation_;
    if (timer_ >= 0) { virEventRemoveTimeout(timer_); timer_ = -1; }
    if (conn_) {
        if (callback_ >= 0) virConnectDomainEventDeregisterAny(conn_, callback_);
        virConnectUnregisterCloseCallback(conn_, closed);
        virConnectClose(conn_);
        conn_ = nullptr;
    }
    callback_ = -1;
}
void VmWorker::refresh() {
    if (!conn_) return;
    virDomainPtr *domains = nullptr;
    int count = virConnectListAllDomains(conn_, &domains, 0);
    if (count < 0) { emit finished(error("List VMs"), false); return; }
    QVariantList rows;
    for (int i = 0; i < count; ++i) {
        char uuid[VIR_UUID_STRING_BUFLEN];
        virDomainGetUUIDString(domains[i], uuid);
        virDomainInfo info{};
        if (virDomainGetInfo(domains[i], &info) == 0) {
            bool diskless = true;
            char *xml = virDomainGetXMLDesc(domains[i], 0);
            const bool contained = xml && Containment::enabled(QString::fromUtf8(xml));
            QXmlStreamReader reader(xml ? QString::fromUtf8(xml) : QString{});
            while (!reader.atEnd()) {
                reader.readNext();
                if (reader.isStartElement() && reader.name() == u"disk" && reader.attributes().value("device") == u"disk") diskless = false;
            }
            free(xml);
            const QStringList states = {"Unknown", "Running", "Blocked", "Paused", "Shutting down", "Stopped", "Crashed", "Suspended"};
            rows.append(QVariantMap{{"uuid", QString::fromUtf8(uuid)}, {"name", QString::fromUtf8(virDomainGetName(domains[i]))},
                {"state", states.value(info.state, "Unknown")}, {"stateCode", info.state},
                {"memoryMiB", double(info.maxMem) / 1024}, {"cpus", info.nrVirtCpu}, {"owned", owned(domains[i])}, {"diskless", diskless}, {"contained", contained}});
        }
        virDomainFree(domains[i]);
    }
    free(domains);
    emit inventory(rows);
}
void VmWorker::createTest() {
    if (!conn_) { emit finished("Connect first", false); return; }
    const QString name = "omaware-test-" + QUuid::createUuid().toString(QUuid::Id128).left(8);
    const QString type = QFileInfo("/dev/kvm").isWritable() ? "kvm" : "qemu";
    // Deliberately diskless and without a NIC: never references preserved VM storage.
    const QString xml = QString(R"(<domain type='%1'><name>%2</name>
      <metadata><test xmlns='https://omaware.org/xmlns/prototype/1'/></metadata>
      <memory unit='MiB'>256</memory><vcpu>1</vcpu><os><type arch='x86_64' machine='pc'>hvm</type><boot dev='hd'/></os>
      <features><acpi/></features><devices><graphics type='vnc'><listen type='none'/></graphics>
      <video><model type='vga'/></video><input type='tablet' bus='usb'/></devices></domain>)").arg(type, name);
    auto d = virDomainDefineXML(conn_, xml.toUtf8().constData());
    if (!d) { emit finished(error("Define test VM"), false); return; }
    char uuid[VIR_UUID_STRING_BUFLEN];
    virDomainGetUUIDString(d, uuid);
    emit created(QString::fromUtf8(uuid));
    virDomainFree(d);
    refresh();
    emit finished("Created " + name + " · diskless, no network", true);
}
// Runs one power operation on a VM; returns an empty string on success.
QString VmWorker::power(virDomainPtr d, const QString &operation) {
    if (!owned(d)) return "OmaWare only changes VMs it created; this one is read-only.";
    if (operation == "start" || operation == "resume") {
        const auto blocker = Containment::blocker(d, operation == "resume");
        if (!blocker.isEmpty()) return blocker;
    }
    int result = -1;
    if (operation == "start") result = virDomainCreate(d);
    else if (operation == "pause") result = virDomainSuspend(d);
    else if (operation == "resume") result = virDomainResume(d);
    else if (operation == "shutdown") result = virDomainShutdown(d);
    else if (operation == "force-off") result = virDomainDestroy(d);
    else if (operation == "remove") {
        if (virDomainIsActive(d) != 0) return "Stop the VM before removing its definition.";
        result = virDomainUndefine(d);
    } else return "Unsupported operation";
    return result < 0 ? error(operation) : QString{};
}
void VmWorker::action(QString uuid, QString operation) {
    if (!conn_) { emit finished("Connect first", false); return; }
    auto d = virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData());
    if (!d) { emit finished(error("Find VM"), false); return; }
    const auto message = power(d, operation);
    virDomainFree(d);
    refresh();
    emit finished(message.isEmpty() ? operation + " requested" : message, message.isEmpty());
}
void VmWorker::bulk(QVariantList uuids, QString operation) {
    if (!conn_) { emit finished("Connect first", false); return; }
    QMap<QString, int> counts; QStringList failures;
    for (const auto &entry : uuids) {
        auto d = virDomainLookupByUUIDString(conn_, entry.toString().toUtf8().constData());
        if (!d) { failures << "A VM is no longer available."; continue; }
        int state = VIR_DOMAIN_NOSTATE, reason = 0;
        virDomainGetState(d, &state, &reason, 0);
        // Pick the step for this VM's current state; VMs already there are skipped.
        QString step;
        if (operation == "power-on") step = state == VIR_DOMAIN_SHUTOFF ? "start" : state == VIR_DOMAIN_PAUSED ? "resume" : "";
        else if (operation == "pause" || operation == "shutdown") step = state == VIR_DOMAIN_RUNNING ? operation : "";
        else if (operation == "force-off") step = virDomainIsActive(d) == 1 ? operation : "";
        else failures << "Unsupported operation";
        if (!step.isEmpty()) {
            const auto message = power(d, step);
            if (message.isEmpty()) ++counts[step];
            else failures << QString::fromUtf8(virDomainGetName(d)).remove(QRegularExpression("^omaware-")) + ": " + message;
        }
        virDomainFree(d);
    }
    refresh();
    const QMap<QString, QString> verbs{{"start", "started"}, {"resume", "resumed"}, {"pause", "paused"}, {"shutdown", "asked to shut down"}, {"force-off", "powered off"}};
    QStringList parts;
    for (const QString &step : {QString("resume"), QString("start"), QString("pause"), QString("shutdown"), QString("force-off")})
        if (counts.value(step)) parts << QString::number(counts[step]) + (counts[step] == 1 ? " VM " : " VMs ") + verbs[step];
    QString message = parts.isEmpty() ? QString("Nothing to change: the selected VMs are already in that state.") : parts.join(", ") + ".";
    if (!failures.isEmpty()) message = (parts.isEmpty() ? QString{} : message + " ") + failures.join(" ");
    emit finished(message, failures.isEmpty());
}
void VmWorker::pauseForExit(QString skip) {
    QStringList paused, failures;
    virDomainPtr *domains = nullptr;
    const int count = conn_ ? virConnectListAllDomains(conn_, &domains, VIR_CONNECT_LIST_DOMAINS_RUNNING) : 0;
    for (int i = 0; i < count; ++i) {
        char uuid[VIR_UUID_STRING_BUFLEN];
        virDomainGetUUIDString(domains[i], uuid);
        const QString name = QString::fromUtf8(virDomainGetName(domains[i])).remove(QRegularExpression("^omaware-"));
        if (owned(domains[i])) {
            if (QString::fromUtf8(uuid) == skip) failures << name + ": left running because a snapshot was in progress.";
            else if (virDomainSuspend(domains[i]) == 0) paused << QString::fromUtf8(uuid);
            else failures << name + ": " + error("pause");
        }
        virDomainFree(domains[i]);
    }
    free(domains);
    refresh();
    emit exitPaused(paused, failures);
}
void VmWorker::openConsole(QString uuid) {
    if (!conn_) { emit finished("Connect first", false); return; }
    auto d = virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData());
    if (!d) { emit finished(error("Find VM"), false); return; }
    if (!owned(d)) { virDomainFree(d); emit finished("The console only works with VMs OmaWare created.", false); return; }
    char *xml = virDomainGetXMLDesc(d, 0);
    QXmlStreamReader reader(xml ? QString::fromUtf8(xml) : QString{});
    bool vnc = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == u"graphics") {
            vnc = reader.attributes().value("type") == u"vnc";
            break;
        }
    }
    free(xml);
    int fd = vnc ? virDomainOpenGraphicsFD(d, 0, VIR_DOMAIN_OPEN_GRAPHICS_SKIPAUTH) : -1;
    QString message = !vnc ? "The console needs the VM's first display to be VNC." : fd < 0 ? error("Open console") : "Console connected through libvirt";
    virDomainFree(d);
    if (fd >= 0) emit graphics(std::make_shared<GraphicsSocket>(fd, uuid));
    emit finished(message, fd >= 0);
}

namespace {
QString domainXml(virDomainPtr domain, unsigned flags) {
    char *raw = virDomainGetXMLDesc(domain, flags);
    QString xml = raw ? QString::fromUtf8(raw) : QString{};
    free(raw);
    return xml;
}
QByteArray networkSignature(QVariantList interfaces) {
    QVariantList normalized;
    for (const auto &entry : interfaces) {
        const auto nic = entry.toMap();
        QVariantMap row;
        for (const QString &key : {QString("mac"), QString("type"), QString("source"), QString("model"), QString("linkUp")}) row[key] = nic.value(key);
        normalized.append(row);
    }
    std::sort(normalized.begin(), normalized.end(), [](const QVariant &a, const QVariant &b) { return a.toMap()["mac"].toString() < b.toMap()["mac"].toString(); });
    return QJsonDocument::fromVariant(normalized).toJson(QJsonDocument::Compact);
}
}
void VmWorker::inspect(QString uuid, quint64 request, bool guestInfo) {
    QVariantMap details{{"uuid", uuid}};
    auto fail = [&](const QString &message) { details["error"] = message; emit inspected(uuid, request, details); };
    if (!conn_) { fail("Connect to your local session to load VM details."); return; }
    auto domain = virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData());
    if (!domain) { fail(error("Load VM details")); return; }
    const bool persistent = virDomainIsPersistent(domain) == 1;
    const bool active = virDomainIsActive(domain) == 1;
    const auto xml = domainXml(domain, (persistent ? VIR_DOMAIN_XML_INACTIVE : 0) | VIR_DOMAIN_XML_SECURE);
    QString failure;
    details = DomainConfig::describe(xml, failure);
    details["uuid"] = uuid;
    if (!failure.isEmpty()) { virDomainFree(domain); fail(failure); return; }
    auto live = active ? DomainConfig::describe(domainXml(domain, 0), failure) : details;
    PendingChanges pending(uuid);
    if (active && !pending.baseline().isEmpty() && Configuration::changes(domainXml(domain, VIR_DOMAIN_XML_SECURE), xml).isEmpty()) pending.clear();
    details["changes"] = pending.items(xml);
    details["pendingConflict"] = !pending.matches(xml);
    details["canDiscardChanges"] = !pending.baseline().isEmpty() && pending.matches(xml);
    details["persistent"] = persistent;
    details["active"] = active;
    details["owned"] = owned(domain);
    details["uri"] = uri_;
    details["id"] = active ? QString::number(virDomainGetID(domain)) : "Not running";
    int autostart = 0;
    details["autostart"] = virDomainGetAutostart(domain, &autostart) == 0 ? (autostart ? "Enabled" : "Disabled") : "Not reported";
    details["liveInterfaces"] = live["interfaces"];
    details["pendingNetworkChanges"] = active && networkSignature(details["interfaces"].toList()) != networkSignature(live["interfaces"].toList());
    details["agentConnected"] = active && live["agentConnected"].toBool();
    {
        // Containment policy and its current verdict, including live devices of a running VM.
        auto verdict = Containment::check(xml);
        if (active) {
            const auto liveVerdict = Containment::check(domainXml(domain, 0));
            auto merge = [&](const char *key) { auto list = verdict[key].toStringList() + liveVerdict[key].toStringList(); list.removeDuplicates(); verdict[key] = list; };
            merge("violations"); merge("warnings");
        }
        verdict["enabled"] = Containment::enabled(xml);
        details["containment"] = verdict;
    }
    details["liveClipboardConfigured"] = active && live["clipboardConfigured"].toBool();
    auto disks = details["disks"].toList();
    for (auto &entry : disks) {
        auto disk = entry.toMap();
        bool sameLiveDisk = !active;
        for (auto liveEntry : live["disks"].toList()) {
            const auto current = liveEntry.toMap();
            if (current["target"] == disk["target"] && current["source"] == disk["source"]) sameLiveDisk = true;
        }
        if (sameLiveDisk && disk["device"] == "disk" && (disk["type"] == "file" || disk["type"] == "block")) {
            virDomainBlockInfo info{};
            if (virDomainGetBlockInfo(domain, disk["target"].toString().toUtf8().constData(), &info, 0) == 0)
            { disk["capacityBytes"] = static_cast<qulonglong>(info.capacity); disk["allocationBytes"] = static_cast<qulonglong>(info.allocation); }
        }
        entry = disk;
    }
    details["disks"] = disks;
    QVariantList addresses;
    QString addressStatus = !active ? "Start the VM to query guest IP addresses."
        : !details["agentConfigured"].toBool() ? "Guest IP addresses require a configured QEMU guest agent."
        : !details["agentConnected"].toBool() ? "The QEMU guest agent is not connected."
        : "Choose Refresh guest IPs to query the guest agent.";
    if (guestInfo && details["agentConnected"].toBool()) {
        virDomainInterfacePtr *interfaces = nullptr;
        int count = virDomainInterfaceAddresses(domain, &interfaces, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT, 0);
        if (count < 0) addressStatus = "Guest IP query failed. The guest agent may still be starting.";
        else {
            for (int i = 0; i < count; ++i) {
                auto iface = interfaces[i];
                if (QString::fromUtf8(iface->name) != "lo") {
                    QStringList ips;
                    for (unsigned j = 0; j < iface->naddrs; ++j)
                        ips << QString::fromUtf8(iface->addrs[j].addr) + "/" + QString::number(iface->addrs[j].prefix);
                    addresses.append(QVariantMap{{"name", QString::fromUtf8(iface->name)},
                        {"mac", QString::fromUtf8(iface->hwaddr ? iface->hwaddr : "")}, {"addresses", ips.join(", ")}});
                }
                virDomainInterfaceFree(iface);
            }
            free(interfaces);
            addressStatus = addresses.isEmpty() ? "The guest agent reported no network addresses." : "Reported by the guest agent at " + QDateTime::currentDateTime().toString("HH:mm:ss");
        }
    }
    details["guestAddresses"] = addresses;
    details["addressStatus"] = addressStatus;
    QString networkStatus;
    details["networkOptions"] = NetworkCatalog::discover(conn_, networkStatus);
    details["networkStatus"] = networkStatus;
    details["loadedAt"] = QDateTime::currentDateTime().toString("HH:mm:ss");
    virDomainFree(domain);
    emit inspected(uuid, request, details);
}
namespace {
// The <interface> element with this MAC as standalone device XML, or empty when absent.
QString interfaceXml(const QString &xml, const QString &mac) {
    QDomDocument doc;
    if (mac.isEmpty() || !doc.setContent(xml)) return {};
    auto devices = doc.documentElement().firstChildElement("devices");
    for (auto nic = devices.firstChildElement("interface"); !nic.isNull(); nic = nic.nextSiblingElement("interface")) {
        if (nic.firstChildElement("mac").attribute("address").compare(mac, Qt::CaseInsensitive) != 0) continue;
        QDomDocument single; single.appendChild(single.importNode(nic, true));
        return single.toString(-1);
    }
    return {};
}
QStringList macsOf(const QString &xml) {
    QString failure; QStringList result;
    for (const auto &nic : DomainConfig::describe(xml, failure)["interfaces"].toList()) result << nic.toMap()["mac"].toString().toLower();
    return result;
}
// Replaces (or removes, when device is empty, or adds) the adapter with this MAC in a full definition.
QString withInterface(const QString &xml, const QString &mac, const QString &device) {
    QDomDocument doc, part;
    if (!doc.setContent(xml)) return xml;
    auto devices = doc.documentElement().firstChildElement("devices");
    QDomElement found;
    for (auto nic = devices.firstChildElement("interface"); !nic.isNull(); nic = nic.nextSiblingElement("interface"))
        if (nic.firstChildElement("mac").attribute("address").compare(mac, Qt::CaseInsensitive) == 0) { found = nic; break; }
    QDomNode replacement;
    if (!device.isEmpty() && part.setContent(device)) replacement = doc.importNode(part.documentElement(), true);
    if (!found.isNull() && replacement.isNull()) devices.removeChild(found);
    else if (!found.isNull()) devices.replaceChild(replacement, found);
    else if (!replacement.isNull()) devices.appendChild(replacement);
    return doc.toString(-1);
}
QString withoutAddress(const QString &device) {
    QDomDocument doc;
    if (!doc.setContent(device)) return device;
    auto address = doc.documentElement().firstChildElement("address");
    if (!address.isNull()) doc.documentElement().removeChild(address);
    return doc.toString(-1);
}
}
// Removes an adapter from the running VM and waits for the guest to release it.
bool VmWorker::detachLive(virDomainPtr domain, const QString &device, const QString &mac, QString &why) {
    if (virDomainDetachDeviceFlags(domain, device.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0) { why = error("Unplug adapter"); return false; }
    // PCI unplug needs the guest OS to acknowledge it. libvirt already waits a few seconds for that
    // and may return before it happens, so allow a little longer before giving up.
    for (int i = 0; i < 15; ++i) {
        if (interfaceXml(domainXml(domain, 0), mac).isEmpty()) return true;
        QThread::msleep(200);
    }
    why = "the guest OS did not release the adapter (it may not support hot-plugging)";
    return false;
}
bool VmWorker::attachLive(virDomainPtr domain, const QString &device, QString &why) {
    if (virDomainAttachDeviceFlags(domain, device.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) == 0) return true;
    why = error("Plug in adapter");
    // The saved PCI slot can differ from what is free in the running VM; let libvirt choose one.
    return virDomainAttachDeviceFlags(domain, withoutAddress(device).toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) == 0;
}
// Saves an adapter change and, on a running VM, applies it live when the guest allows.
bool VmWorker::changeNetwork(const QString &uuid, const QString &mac, const QString &networkId, const QString &model,
    bool linkUp, bool remove, const QString &revision, QString &message) {
    auto domain = virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData());
    if (!domain) { message = error("Find VM"); return false; }
    QString failure;
    const bool active = virDomainIsActive(domain) == 1;
    const auto xml = domainXml(domain, VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_SECURE);
    auto existing = DomainConfig::describe(xml, failure);
    if (!owned(domain)) failure = "Network changes are limited to VMs OmaWare created.";
    else if (virDomainIsPersistent(domain) != 1) failure = "This VM has no saved configuration to edit.";
    else if (revision.isEmpty() || DomainConfig::revision(xml) != revision) failure = "The VM configuration changed. Refresh and review your changes again.";
    QString kind, source;
    if (failure.isEmpty() && !remove) {
        // Resolve the ID against fresh host state; never accept arbitrary device XML.
        QString status;
        const auto choices = NetworkCatalog::discover(conn_, status);
        QVariantMap current;
        for (auto row : existing["interfaces"].toList())
            if (row.toMap()["mac"].toString().compare(mac, Qt::CaseInsensitive) == 0) current = row.toMap();
        if (!current.isEmpty() && current["networkId"].toString() == networkId) {
            kind = current["type"].toString(); source = current["source"].toString();
        } else {
            for (auto row : choices) {
                const auto choice = row.toMap();
                if (choice["id"].toString() == networkId) {
                    if (!choice["available"].toBool()) failure = choice["reason"].toString();
                    else { kind = choice["kind"].toString(); source = choice["source"].toString(); }
                    break;
                }
            }
            if (failure.isEmpty() && kind.isEmpty()) failure = "The selected network is no longer available. Refresh and try again.";
        }
    }
    // Contained VMs may only attach to switches that pass live isolation checks.
    if (failure.isEmpty() && !remove && Containment::enabled(xml) && (kind != "bridge" || !Containment::hostBridges().value(source)["isolated"].toBool()))
        failure = "This VM is contained. Choose a verified isolated network, or remove the adapter.";
    QString device;
    if (failure.isEmpty()) DomainConfig::networkDevice(xml, mac, kind, source, model, linkUp, remove, device, failure);
    PendingChanges pending(uuid);
    if (failure.isEmpty() && (active || !pending.baseline().isEmpty())) pending.prepare(xml, failure);
    int result = -1;
    if (failure.isEmpty()) {
        const auto bytes = device.toUtf8();
        if (remove) result = virDomainDetachDeviceFlags(domain, bytes.constData(), VIR_DOMAIN_AFFECT_CONFIG);
        else if (mac.isEmpty()) result = virDomainAttachDeviceFlags(domain, bytes.constData(), VIR_DOMAIN_AFFECT_CONFIG);
        else result = virDomainUpdateDeviceFlags(domain, bytes.constData(), VIR_DOMAIN_AFFECT_CONFIG);
        if (result < 0) failure = error("Save network adapter");
        else if (!pending.baseline().isEmpty() && !pending.saved(domainXml(domain, VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_SECURE), failure)) {
            failure = "Network settings saved, but " + failure;
            result = -1;
        }
    }
    if (result < 0) { virDomainFree(domain); message = failure; return false; }
    const QString verb = remove ? "Adapter removed" : mac.isEmpty() ? "Adapter added" : "Adapter updated";
    if (!active) { virDomainFree(domain); message = verb + "."; return true; }

    // Apply the same change to the running VM, like moving a cable between sockets.
    const auto saved = domainXml(domain, VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_SECURE);
    QString target = mac.toLower();
    if (target.isEmpty()) { const auto before = macsOf(xml); for (const auto &m : macsOf(saved)) if (!before.contains(m)) target = m; }
    const auto next = remove ? QString{} : interfaceXml(saved, target);
    const auto current = interfaceXml(domainXml(domain, 0), target);
    bool applied = false; QString why;
    if (remove) applied = current.isEmpty() || detachLive(domain, current, target, why);
    else if (next.isEmpty()) why = "the new adapter could not be found in the saved configuration";
    else if (current.isEmpty()) applied = attachLive(domain, next, why);
    else if (virDomainUpdateDeviceFlags(domain, next.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) == 0) applied = true;
    else if (detachLive(domain, current, target, why)) {
        applied = attachLive(domain, next, why);
        if (!applied) { QString ignored; attachLive(domain, current, ignored); }   // put the old adapter back
    }
    if (applied && !pending.baseline().isEmpty()) {
        // The change is already live, so it must not show up as a pending restart item.
        QString ignored;
        pending.rebase(withInterface(pending.baseline(), target, next), saved, ignored);
    }
    virDomainFree(domain);
    message = applied ? verb + " on the running VM." : verb + ". The running VM keeps its current connection until a full shutdown and start, because " + why + ".";
    return true;
}
void VmWorker::configureNetwork(QString uuid, QString mac, QString networkId, QString model,
    bool linkUp, bool remove, QString revision) {
    if (!conn_) { const QString m = "Connect to your local session first."; emit finished(m, false); emit networkConfigured(uuid, false, m); return; }
    QString message;
    const bool ok = changeNetwork(uuid, mac, networkId, model, linkUp, remove, revision, message);
    refresh();
    emit finished(message, ok); emit networkConfigured(uuid, ok, message);
}
void VmWorker::connectVms(QVariantList uuids, QString networkId) {
    if (!conn_) { const QString m = "Connect to your local session first."; emit finished(m, false); emit networkConfigured({}, false, m); return; }
    QStringList failures; int connected = 0;
    for (const auto &entry : uuids) {
        const auto uuid = entry.toString();
        auto domain = virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData());
        if (!domain) { failures << "A VM is no longer available."; continue; }
        const QString name = QString::fromUtf8(virDomainGetName(domain)).remove(QRegularExpression("^omaware-"));
        const auto revision = DomainConfig::revision(domainXml(domain, VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_SECURE));
        virDomainFree(domain);
        QString message;
        if (changeNetwork(uuid, "", networkId, "virtio", true, false, revision, message)) ++connected;
        else failures << name + ": " + message;
    }
    refresh();
    QString message = connected ? QString::number(connected) + (connected == 1 ? " VM connected." : " VMs connected.") : QString{};
    if (!failures.isEmpty()) message = (message.isEmpty() ? QString{} : message + " ") + failures.join(" ");
    emit finished(message, failures.isEmpty()); emit networkConfigured({}, failures.isEmpty(), message);
}

namespace {
// Sets <link state> on the adapter with this MAC. Returns the rewritten definition and the device XML
// libvirt needs; an empty device means the adapter is not in this definition.
QString withLink(const QString &xml, const QString &mac, bool up, QString &device, QString &type, QString &source) {
    QDomDocument doc; device.clear();
    if (!doc.setContent(xml)) return xml;
    auto devices = doc.documentElement().firstChildElement("devices");
    for (auto nic = devices.firstChildElement("interface"); !nic.isNull(); nic = nic.nextSiblingElement("interface")) {
        if (nic.firstChildElement("mac").attribute("address").compare(mac, Qt::CaseInsensitive) != 0) continue;
        auto link = nic.firstChildElement("link");
        if (link.isNull()) link = nic.appendChild(doc.createElement("link")).toElement();
        link.setAttribute("state", up ? "up" : "down");
        type = nic.attribute("type");
        source = nic.firstChildElement("source").attribute(type == "network" ? "network" : "bridge");
        QDomDocument single; single.appendChild(single.importNode(nic, true));
        device = single.toString(-1);
        break;
    }
    return doc.toString(-1);
}
}
void VmWorker::setLinks(QVariantList targets, bool up) {
    if (!conn_) { emit linksSet(false, "Connect to your local session first."); return; }
    QStringList failures; int changed = 0;
    QHash<QString, QVariantMap> bridges; bool bridgesLoaded = false;
    for (const auto &row : targets) {
        const auto uuid = row.toMap()["uuid"].toString(), mac = row.toMap()["mac"].toString();
        auto domain = virDomainLookupByUUIDString(conn_, uuid.toUtf8().constData());
        if (!domain) { failures << "A VM is no longer available."; continue; }
        const QString name = QString::fromUtf8(virDomainGetName(domain)).remove(QRegularExpression("^omaware-"));
        auto fail = [&](const QString &why) { failures << name + ": " + why; };
        const bool active = virDomainIsActive(domain) == 1, persistent = virDomainIsPersistent(domain) == 1;
        if (!owned(domain)) { fail("cables can only be changed on OmaWare-managed VMs."); virDomainFree(domain); continue; }
        const auto live = active ? domainXml(domain, VIR_DOMAIN_XML_SECURE) : QString{};
        const auto saved = persistent ? domainXml(domain, VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_SECURE) : QString{};
        QString liveDevice, savedDevice, type, source, failure;
        const auto liveNext = active ? withLink(live, mac, up, liveDevice, type, source) : QString{};
        const auto savedNext = persistent ? withLink(saved, mac, up, savedDevice, type, source) : QString{};
        if (liveDevice.isEmpty() && savedDevice.isEmpty()) { fail("this adapter no longer exists."); virDomainFree(domain); continue; }
        // A contained VM may only be plugged into a verified isolated switch.
        if (up && Containment::enabled(active ? live : saved)) {
            if (!bridgesLoaded) { bridges = Containment::hostBridges(); bridgesLoaded = true; }
            if (type != "bridge" || !bridges.value(source)["isolated"].toBool()) { fail("contained VMs can only connect to a verified isolated switch."); virDomainFree(domain); continue; }
        }
        PendingChanges pending(uuid);
        if (!savedDevice.isEmpty() && !pending.matches(saved)) failure = "its pending configuration changed elsewhere. Refresh Details first.";
        if (failure.isEmpty() && !liveDevice.isEmpty() && virDomainUpdateDeviceFlags(domain, liveDevice.toUtf8().constData(), VIR_DOMAIN_AFFECT_LIVE) < 0)
            failure = error("Change live cable");
        if (failure.isEmpty() && !savedDevice.isEmpty()) {
            if (virDomainUpdateDeviceFlags(domain, savedDevice.toUtf8().constData(), VIR_DOMAIN_AFFECT_CONFIG) < 0) failure = error("Save cable state");
            else if (!pending.baseline().isEmpty()) {
                // The change is already live, so it must not show up as a pending restart item.
                QString ignored, t, src;
                const auto baseline = withLink(pending.baseline(), mac, up, ignored, t, src);
                pending.rebase(baseline, domainXml(domain, VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_SECURE), failure);
            }
        }
        if (failure.isEmpty()) ++changed; else fail(failure);
        virDomainFree(domain);
    }
    refresh();
    const QString verb = up ? "plugged in" : "pulled";
    if (failures.isEmpty()) emit linksSet(true, changed == 1 ? "Cable " + verb + "." : QString::number(changed) + " cables " + verb + ".");
    else emit linksSet(false, (changed ? QString::number(changed) + " cables " + verb + ". " : QString{}) + failures.join(" "));
}

Backend::Backend(QString uri, QObject *parent) : QObject(parent), worker_(new VmWorker(uri)), storageWorker_(new VmWorker(uri, true)), uri_(std::move(uri)) {
    activity_ = activityLog_.load();
    qRegisterMetaType<GraphicsHandle>();
    worker_->moveToThread(&thread_);
    storageWorker_->moveToThread(&storageThread_);
    connect(&storageThread_, &QThread::finished, storageWorker_, &QObject::deleteLater);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &VmWorker::inventory, this, [this](QVariantList rows) { rows_ = rows; emit changed(); if (!inspectedUuid_.isEmpty()) inspect(inspectedUuid_); });
    connect(worker_, &VmWorker::connection, this, [this](bool ok, QString msg) { connected_ = ok; busy_ = checkpointJob_.value("active").toBool(); message_ = msg; if (!ok) inspect({}); emit changed(); if (!ok) emit operationFinished(msg, false); });
    connect(worker_, &VmWorker::finished, this, [this](QString msg, bool ok) { busy_ = checkpointJob_.value("active").toBool(); message_ = msg; emit changed(); emit operationFinished(msg, ok); });
    connect(this, &Backend::operationFinished, this, &Backend::note);
    connect(worker_, &VmWorker::progress, this, [this](QString message) { message_ = message; emit changed(); });
    connect(storageWorker_, &VmWorker::checkpointProgress, this, [this](QVariantMap job) {
        const auto done = job["completed"].toULongLong(), total = job["total"].toULongLong();
        if (done < previousBytes_ || checkpointJob_["phase"] != job["phase"]) { rateClock_.restart(); previousBytes_ = 0; }
        const auto seconds = rateClock_.elapsed() / 1000.0;
        const double rate = seconds >= 0.25 && done > 0 ? done / seconds : 0;
        job["rate"] = rate; job["eta"] = rate > 0 && total > done ? (total - done) / rate : -1;
        job["elapsed"] = jobClock_.elapsed() / 1000; job["name"] = checkpointJob_["name"];
        job["cancelRequested"] = checkpointJob_.value("cancelRequested", false);
        previousBytes_ = done; checkpointJob_ = job; emit checkpointJobChanged();
    });
    auto managed = [this](QString operation, bool ok, QVariantMap result) {
        if (ok) { management_[operation] = result; emit managementChanged(); }
        else if (operation.endsWith(".list") || operation == "capabilities") {
            message_ = result["message"].toString(); emit changed(); emit operationFinished(message_, false);
        }
        emit commandFinished(operation, ok, result);
    };
    connect(worker_, &VmWorker::managed, this, managed);
    connect(storageWorker_, &VmWorker::managed, this, managed);
    connect(storageWorker_, &VmWorker::finished, this, [this](QString message, bool ok) {
        checkpointJob_["active"] = false; checkpointJob_["ok"] = ok; checkpointJob_["phase"] = message; checkpointJob_["elapsed"] = jobClock_.elapsed() / 1000;
        emit checkpointJobChanged(); busy_ = false; message_ = message; emit changed(); emit operationFinished(message, ok); refresh();
    });
    connect(storageWorker_, &VmWorker::created, this, [this](QString uuid) {
        QMetaObject::invokeMethod(worker_, [this, uuid] { worker_->refresh(); emit created(uuid); });
    });
    connect(worker_, &VmWorker::linksSet, this, [this](bool ok, QString msg) { emit linksSet(ok, msg); emit operationFinished(msg, ok); });
    connect(worker_, &VmWorker::exitPaused, this, [this](QStringList uuids, QStringList failures) {
        if (!uuids.isEmpty()) emit operationFinished(QString::number(uuids.size()) + (uuids.size() == 1 ? " VM" : " VMs") + " paused because OmaWare closed.", true);
        if (!failures.isEmpty()) emit operationFinished(failures.join(" "), false);
        emit pausedForExit(uuids, failures);
    });
    connect(worker_, &VmWorker::graphics, this, &Backend::graphics);
    connect(worker_, &VmWorker::created, this, &Backend::created);
    connect(worker_, &VmWorker::lifecycle, this, &Backend::lifecycle);
    connect(worker_, &VmWorker::inspected, this, [this](QString uuid, quint64 request, QVariantMap details) {
        if (request != detailRequest_ || uuid != inspectedUuid_) return;
        details_ = details; detailsBusy_ = false; emit detailsChanged();
    });
    connect(worker_, &VmWorker::networkConfigured, this, &Backend::networkConfigured);
    thread_.start(); storageThread_.start();
    reconnect();
}
Backend::~Backend() {
    storageWorker_->requestCheckpointCancel();
    QMetaObject::invokeMethod(storageWorker_, &VmWorker::stop, Qt::BlockingQueuedConnection);
    storageThread_.quit(); storageThread_.wait();
    worker_->requestCheckpointCancel();
    QMetaObject::invokeMethod(worker_, &VmWorker::stop, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
}
bool Backend::begin() { if (busy_ || checkpointJob_.value("active").toBool()) return false; busy_ = true; emit changed(); return true; }
void Backend::reconnect() { if (begin()) QMetaObject::invokeMethod(worker_, &VmWorker::open); }
void Backend::refresh() { QMetaObject::invokeMethod(worker_, &VmWorker::refresh); }
void Backend::createTest() { if (begin()) QMetaObject::invokeMethod(worker_, &VmWorker::createTest); }
void Backend::action(QString uuid, QString operation) { if (begin()) QMetaObject::invokeMethod(worker_, [=, this] { worker_->action(uuid, operation); }); }
void Backend::bulkAction(QVariantList uuids, QString operation) { if (begin()) QMetaObject::invokeMethod(worker_, [=, this] { worker_->bulk(uuids, operation); }); }
void Backend::pauseForExit() {
    const QString skip = checkpointJob_.value("active").toBool() ? checkpointJob_.value("uuid").toString() : QString{};
    QMetaObject::invokeMethod(worker_, [=, this] { worker_->pauseForExit(skip); });
}
void Backend::openConsole(QString uuid) { if (checkpointJob_.value("active").toBool() || begin()) QMetaObject::invokeMethod(worker_, [=, this] { worker_->openConsole(uuid); }); }

void Backend::inspect(QString uuid, bool guestInfo) {
    const auto request = ++detailRequest_;
    if (uuid != inspectedUuid_) details_.clear();
    inspectedUuid_ = uuid;
    detailsBusy_ = !uuid.isEmpty();
    if (uuid.isEmpty()) details_.clear();
    emit detailsChanged();
    if (!uuid.isEmpty()) QMetaObject::invokeMethod(worker_, [=, this] { worker_->inspect(uuid, request, guestInfo); });
}
bool Backend::configureNetwork(QString uuid, QString mac, QString networkId, QString model, bool linkUp, bool remove, QString revision) {
    if (!begin()) return false;
    QMetaObject::invokeMethod(worker_, [=, this] { worker_->configureNetwork(uuid, mac, networkId, model, linkUp, remove, revision); });
    return true;
}
bool Backend::connectVms(QVariantList uuids, QString networkId) {
    if (!begin()) return false;
    QMetaObject::invokeMethod(worker_, [=, this] { worker_->connectVms(uuids, networkId); });
    return true;
}
void Backend::setLinks(QVariantList targets, bool up) {
    QMetaObject::invokeMethod(worker_, [=, this] { worker_->setLinks(targets, up); });
}
bool Backend::request(QString operation, QVariantMap input) {
    const bool query = operation.endsWith(".list") || operation.startsWith("stats") || operation == "capabilities";
    if (!query && !begin()) return false;
    if (query && busy_ && !checkpointJob_.value("active").toBool()) return false;
    if (!query && operation.startsWith("snapshots.")) {
        storageWorker_->resetCheckpointCancel(); jobClock_.start(); rateClock_.start(); previousBytes_ = 0;
        checkpointJob_ = {{"active", true}, {"uuid", input["uuid"]}, {"operation", operation}, {"name", input["name"]}, {"phase", "Preparing checkpoint operation"}, {"cancellable", true}, {"completed", 0}, {"total", 0}};
        emit checkpointJobChanged();
        QMetaObject::invokeMethod(storageWorker_, [=, this] { storageWorker_->manage(operation, input); });
    } else QMetaObject::invokeMethod(worker_, [=, this] { worker_->manage(operation, input); });
    return true;
}
void Backend::note(const QString &msg, bool ok) {
    const QVariantMap entry{{"ok", ok}, {"message", msg}, {"nextStep", ok ? QString{} : Diagnostics::nextStep(msg)}, {"connection", uri_}};
    activityWarning_.clear();
    if (!activityLog_.append(entry, activity_, activityWarning_)) {
        auto temporary = entry; temporary["time"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        if (activity_.isEmpty() || activity_.first().toMap()["message"] != msg) activity_.prepend(temporary);
        while (activity_.size() > 200) activity_.removeLast();
    }
    emit activityChanged();
}
void Backend::cancelRestart() { QMetaObject::invokeMethod(worker_, &VmWorker::cancelRestart); }
void Backend::clearActivity() {
    activityWarning_.clear();
    if (activityLog_.clear(activityWarning_)) activity_.clear();
    emit activityChanged();
}
void Backend::cancelCheckpoint() {
    if (!checkpointJob_.value("active").toBool() || !checkpointJob_.value("cancellable").toBool()) return;
    storageWorker_->requestCheckpointCancel(); checkpointJob_["cancelRequested"] = true; emit checkpointJobChanged();
}
