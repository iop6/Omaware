// SPDX-License-Identifier: GPL-3.0-or-later
// Host networks (qemu:///system) for VmWorker::manage(): listing them with the VMs on them and their
// addresses, creating, editing, starting, stopping and removing OmaWare's own, and letting session VMs
// join their bridges through the root helper.
#include "management.h"
#include "bridgehelper.h"
#include "configuration.h"
#include "containment.h"
#include "domainconfig.h"
#include "networkcatalog.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QProcess>
#include <QSet>
#include <QUuid>

namespace {
using Management::managedNetwork;

// A host network a request changes, as it was before the change. `handle` is null for a new network.
struct ExistingNetwork {
    virNetworkPtr handle = nullptr;
    QString uuid, xml, bridge, name;
    // VMs (session and system) with an adapter on it.
    QVariantList users;
};

// Where every VM on a connection is attached, in its saved and running definition: each VM's name with its
// sources ("bridge:<name>" and "network:<name>"), plus problems reading the inventory. Read once per request,
// since listing asks the same question for every host network.
struct Attachments {
    QList<QPair<QString, QSet<QString>>> vms;
    QStringList problems;
};

Attachments attachmentsOn(virConnectPtr conn) {
    Attachments result;
    if (!conn) return result;
    virDomainPtr *domains = nullptr;
    const int count = virConnectListAllDomains(conn, &domains, 0);
    if (count < 0) {
        result.problems << "VM inventory unavailable";
        return result;
    }
    for (int i = 0; i < count; ++i) {
        QSet<QString> sources;
        for (bool live : {false, true}) {
            if (live && virDomainIsActive(domains[i]) != 1) continue;
            const unsigned flags = !live && virDomainIsPersistent(domains[i]) == 1 ? VIR_DOMAIN_XML_INACTIVE : 0;
            const auto xml = Virt::domainXml(domains[i], flags);
            QString failure;
            const auto info = xml.isEmpty() ? QVariantMap{} : DomainConfig::describe(xml, failure);
            if (xml.isEmpty() || !failure.isEmpty()) {
                result.problems << "VM network configuration unavailable";
                continue;
            }
            for (const auto &v : info["interfaces"].toList()) {
                const auto nic = v.toMap();
                if (nic["type"] == "bridge" || nic["type"] == "network")
                    sources.insert(nic["type"].toString() + ":" + nic["source"].toString());
            }
        }
        result.vms.append({QString::fromUtf8(virDomainGetName(domains[i])), sources});
        virDomainFree(domains[i]);
    }
    free(domains);
    return result;
}

// Names of the VMs with an adapter on this bridge or libvirt network, with any inventory problems first.
QVariantList usersFrom(const Attachments &attachments, const QString &bridge, const QString &name) {
    QVariantList users;
    for (const auto &problem : attachments.problems)
        users.append(problem);
    for (const auto &[vm, sources] : attachments.vms)
        if (sources.contains("bridge:" + bridge) || sources.contains("network:" + name)) users.append(vm);
    return users;
}

QVariantList usersOf(virConnectPtr conn, const QString &bridge, const QString &name) {
    return usersFrom(attachmentsOn(conn), bridge, name);
}

bool overlaps(const QString &subnet, const QString &other) {
    const auto a = QHostAddress::parseSubnet(subnet), b = QHostAddress::parseSubnet(other);
    return a.second >= 0 && b.second >= 0 && (a.first.isInSubnet(b) || b.first.isInSubnet(a));
}

// The network's address as CIDR ("192.168.100.1/24"), or empty without one.
QString cidrOf(const QDomElement &ip) {
    if (ip.isNull()) return {};
    const auto netmask = QHostAddress(ip.attribute("netmask")).toIPv4Address();
    const int prefix = netmask ? 32 - qPopulationCount(~netmask) : 24;
    return ip.attribute("address") + "/" + ip.attribute("prefix", QString::number(prefix));
}

// Why this subnet can't be used for the network `identity`, or empty when it's free. `ownBridge` is the
// network's current bridge, whose address is naturally in its own subnet.
QString subnetConflict(virConnectPtr system, const QString &cidr, const QString &ownBridge, const QString &identity) {
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (iface.name() == ownBridge) continue;
        for (const auto &entry : iface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol &&
                    overlaps(cidr, entry.ip().toString() + "/" + QString::number(entry.prefixLength())))
                return "The subnet overlaps host interface " + iface.name() + ". Choose another subnet.";
    }
    QString found;
    virNetworkPtr *all = nullptr;
    const int count = virConnectListAllNetworks(system, &all, 0);
    for (int i = 0; i < count; ++i) {
        QDomDocument doc;
        doc.setContent(Virt::networkXml(all[i]));
        const auto root = doc.documentElement();
        if (found.isEmpty() && root.firstChildElement("uuid").text() != identity) {
            for (auto ip = root.firstChildElement("ip"); !ip.isNull(); ip = ip.nextSiblingElement("ip")) {
                const auto prefix = ip.attribute("prefix", ip.attribute("netmask"));
                if (overlaps(cidr, ip.attribute("address") + "/" + prefix))
                    found = "The subnet overlaps network " + root.firstChildElement("name").text() +
                            ". Choose another subnet.";
            }
        }
        virNetworkFree(all[i]);
    }
    free(all);
    return found;
}

// Runs OmaWare's root helper through pkexec so session VMs may join these networks' bridges. pkexec runs it
// as root, so only a root-owned copy in a root-only folder is trusted (see BridgeHelper::trusted): a helper
// the user, or any program running as the user, could change would hand out root. On failure, `failure` is
// for the user (with the helper's own explanation) and `code` a stable reason for agents.
bool authorizeBridges(const QStringList &uuids, QString &failure, QString &code) {
    QString why;
    const auto helper = BridgeHelper::trusted(&why);
    if (helper.isEmpty()) {
        code = why;
        const auto appDir = QCoreApplication::applicationDirPath();
        const auto build = QFileInfo::exists(appDir + "/CMakeCache.txt") ? appDir : QString{};
        const auto intro = why == "helper_untrusted"
                                   ? "OmaWare won't run the network helper at " + BridgeHelper::destination() +
                                             ": it, or a folder above it, isn't safely owned by root. Reinstall it "
                                             "with:\n"
                                   : QString("Letting your VMs join networks needs OmaWare's small administrator "
                                             "helper. Install it once with:\n");
        failure = intro + BridgeHelper::installCommands(BridgeHelper::script(), build).join("\nor\n");
        return false;
    }
    QProcess process;
    process.start("pkexec", QStringList{helper} + uuids);
    if (!process.waitForStarted(5000)) {
        code = "authorization_unavailable";
        failure = "Cannot start pkexec. Install polkit to let OmaWare ask for administrator authorization.";
        return false;
    }
    // The password prompt waits for the user; give them a few minutes.
    if (!process.waitForFinished(300000)) {
        process.kill();
        process.waitForFinished(3000);
        code = "authorization_timeout";
        failure = "Nobody answered the administrator password prompt in time.";
        return false;
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) return true;
    const auto errors = process.readAllStandardError();
    code = BridgeHelper::classify(process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1, errors);
    if (code == "authorization_cancelled")
        failure = "The administrator password prompt was closed, so nothing changed.";
    else
        failure = (code == "helper_failed" ? QString("The network helper failed: ") : QString()) +
                  QString::fromUtf8(errors).trimmed().left(2000);
    if (failure.isEmpty()) failure = BridgeHelper::agentMessage(code);
    return false;
}

// The result fields an agent gets with a failed network change.
QVariantMap helperFailure(const QString &code) {
    return {{"code", code}, {"reason", BridgeHelper::agentMessage(code)}};
}

// One host network as the Networks page shows it.
QVariantMap describeHostNetwork(
        virNetworkPtr network, const Attachments &system, const Attachments &session, const QVariantList &choices) {
    const auto xml = Virt::networkXml(network);
    auto data = NetworkCatalog::describe(xml);
    QDomDocument doc;
    doc.setContent(xml);
    const auto root = doc.documentElement();
    const auto ip = root.firstChildElement("ip");
    data["uuid"] = root.firstChildElement("uuid").text();
    data["title"] = root.firstChildElement("title").text();
    data["active"] = virNetworkIsActive(network) == 1;
    data["managed"] = managedNetwork(xml);
    data["revision"] = DomainConfig::revision(xml);
    data["users"] = usersFrom(session, data["bridge"].toString(), data["name"].toString());
    data["systemUsers"] = usersFrom(system, data["bridge"].toString(), data["name"].toString());
    data["mode"] = !root.firstChildElement("forward").isNull() ? "nat" : ip.isNull() ? "isolated" : "hostonly";
    if (data["mode"] == "isolated") data["isolation"] = Containment::verifyNetwork(xml, data["active"].toBool());
    data["cidr"] = cidrOf(ip);
    const auto range = ip.firstChildElement("dhcp").firstChildElement("range");
    data["dhcpStart"] = range.attribute("start");
    data["dhcpEnd"] = range.attribute("end");
    int autostart = 0;
    virNetworkGetAutostart(network, &autostart);
    data["autostart"] = bool(autostart);
    for (const auto &choice : choices)
        if (choice.toMap()["id"].toString() == "bridge:" + data["bridge"].toString()) {
            data["available"] = choice.toMap()["available"];
            data["reason"] = choice.toMap()["reason"];
        }
    return data;
}

// One VM as the network map shows it: its adapters, their addresses and pending changes.
QVariantMap topologyEntry(virDomainPtr domain, const QHash<QString, QStringList> &leases) {
    QString failure;
    const auto saved = Virt::definition(domain);
    auto info = DomainConfig::describe(saved, failure);
    info["active"] = virDomainIsActive(domain) == 1;
    QVariantMap addresses, addressSources;
    int pendingChanges = 0;
    if (info["active"].toBool()) {
        const auto live = Virt::definition(domain, true);
        info["liveInterfaces"] = DomainConfig::describe(live, failure)["interfaces"];
        // Guest IPv4 addresses by MAC: DHCP leases of host networks, then the host's neighbour table. The first
        // source that knew a MAC's addresses is reported for it.
        QHash<QString, QStringList> found = leases;
        QHash<QString, QString> sources;
        for (auto it = leases.cbegin(); it != leases.cend(); ++it)
            sources[it.key()] = "dhcp_lease";
        virDomainInterfacePtr *ifaces = nullptr;
        const int count = virDomainInterfaceAddresses(domain, &ifaces, VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_ARP, 0);
        for (int j = 0; j < count; ++j) {
            const auto mac = QString::fromUtf8(ifaces[j]->hwaddr ? ifaces[j]->hwaddr : "").toLower();
            for (unsigned k = 0; k < ifaces[j]->naddrs; ++k) {
                const auto address = QString::fromUtf8(ifaces[j]->addrs[k].addr);
                if (ifaces[j]->addrs[k].type == VIR_IP_ADDR_TYPE_IPV4 && !found[mac].contains(address)) {
                    found[mac] << address;
                    if (!sources.contains(mac)) sources[mac] = "arp";
                }
            }
            virDomainInterfaceFree(ifaces[j]);
        }
        free(ifaces);
        for (const auto &nic : info["liveInterfaces"].toList()) {
            const auto mac = nic.toMap()["mac"].toString().toLower();
            if (found.contains(mac) && !found[mac].isEmpty()) {
                addresses[mac] = found[mac];
                addressSources[mac] = sources.value(mac);
            }
        }
        // Saved changes the running VM doesn't use yet (they apply on its next full start).
        PendingChanges pending(Virt::uuidOf(domain));
        if (!pending.baseline().isEmpty() && !Configuration::changes(live, saved).isEmpty())
            pendingChanges = pending.items(saved).size();
    }
    // The revision lets the network map rewire adapters with the same stale-edit protection as Details.
    return {{"uuid", info["uuid"]}, {"name", info["name"]}, {"active", info["active"]},
            {"interfaces", info["interfaces"]}, {"liveInterfaces", info["liveInterfaces"]},
            {"revision", DomainConfig::revision(saved)}, {"addresses", addresses}, {"addressSources", addressSources},
            {"pendingChanges", pendingChanges}};
}

// "networks.list": every host network, and every session VM with the networks it's connected to.
void listHostNetworks(const VmWorker::Request &request, virConnectPtr system, virConnectPtr session) {
    QString status;
    const auto choices = NetworkCatalog::discover(session, status);
    const auto sessionVms = attachmentsOn(session), systemVms = attachmentsOn(system);
    QVariantList networks;
    virNetworkPtr *all = nullptr;
    const int count = virConnectListAllNetworks(system, &all, 0);
    for (int i = 0; i < count; ++i) {
        networks.append(describeHostNetwork(all[i], systemVms, sessionVms, choices));
        virNetworkFree(all[i]);
    }
    free(all);
    const auto leases = Management::dhcpLeases(system);
    QVariantList topology;
    virDomainPtr *domains = nullptr;
    const int vms = virConnectListAllDomains(session, &domains, 0);
    for (int i = 0; i < vms; ++i) {
        topology.append(topologyEntry(domains[i], leases));
        virDomainFree(domains[i]);
    }
    free(domains);
    request.done(true, status, {{"items", networks}, {"topology", topology}, {"choices", choices}});
}

// "networks.authorizeMany": one administrator prompt for several networks (a lab's).
void authorizeHostNetworks(const VmWorker::Request &request, virConnectPtr system) {
    QStringList ids;
    for (const auto &v : request.in["uuids"].toList()) {
        Network network(virNetworkLookupByUUIDString(system, v.toString().toUtf8().constData()), virNetworkFree);
        if (!network || !managedNetwork(Virt::networkXml(network.get())))
            return request.fail("Only OmaWare-created host networks can be authorized.");
        if (virNetworkIsActive(network.get()) != 1)
            return request.fail("Start the networks before allowing VMs to join them.");
        ids << v.toString();
    }
    if (ids.isEmpty() || ids.size() > 16) return request.fail("Choose one to sixteen networks.");
    QString failure, code;
    if (!authorizeBridges(ids, failure, code)) {
        // Helpers from before 1.3 take one network at a time.
        if (!failure.contains("one network UUID")) return request.fail(failure, helperFailure(code));
        for (const auto &id : ids)
            if (!authorizeBridges({id}, failure, code)) return request.fail(failure, helperFailure(code));
    }
    request.done(
            true, ids.size() == 1 ? "Your VMs can now join this network." : "Your VMs can now join these networks.");
}

// "networks.save": creates a network, or changes a stopped one's addresses.
void saveHostNetwork(const VmWorker::Request &request, virConnectPtr system, const ExistingNetwork &existing) {
    const bool editing = existing.handle != nullptr;
    if (editing && (virNetworkIsActive(existing.handle) == 1 || !existing.users.isEmpty()))
        return request.fail("Stop this network and disconnect its VMs before editing its addresses.");
    const auto identity = editing ? existing.uuid : QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto bridge = editing ? existing.bridge : "oma" + QUuid(identity).toString(QUuid::Id128).left(8);
    auto settings = request.in;
    if (settings["mode"] != "isolated" && settings["subnet"].toString().trimmed().isEmpty()) {
        // No subnet given: pick the first free 192.168.X.0/24, with a DHCP range in it.
        for (int x = 100; x < 250 && settings["subnet"].toString().isEmpty(); ++x) {
            const auto cidr = QString("192.168.%1.0/24").arg(x);
            if (!subnetConflict(system, cidr, existing.bridge, identity).isEmpty()) continue;
            settings["subnet"] = cidr;
            if (settings["dhcpStart"].toString().isEmpty()) settings["dhcpStart"] = QString("192.168.%1.100").arg(x);
            if (settings["dhcpEnd"].toString().isEmpty()) settings["dhcpEnd"] = QString("192.168.%1.200").arg(x);
        }
        if (settings["subnet"].toString().isEmpty())
            return request.fail("No free 192.168.x.0/24 subnet was found. Enter one under Address settings.");
    }
    QString failure;
    const auto xml = Configuration::networkXml(settings, identity, bridge, failure);
    if (!failure.isEmpty()) return request.fail(failure);
    QDomDocument edited;
    edited.setContent(xml);
    if (editing && edited.documentElement().firstChildElement("name").text() != existing.name)
        return request.fail("Keep the existing network name when editing its settings.");
    if (settings["mode"] != "isolated") {
        failure = subnetConflict(system, settings["subnet"].toString(), existing.bridge, identity);
        if (!failure.isEmpty()) return request.fail(failure);
    }
    if (!request.stillAuthorized()) return;

    Network defined(virNetworkDefineXML(system, xml.toUtf8().constData()), virNetworkFree);
    if (!defined) return request.fail(Virt::lastError("Save host network"));
    if (virNetworkSetAutostart(defined.get(), settings.value("autostart", true).toBool()) < 0)
        return request.fail(Virt::lastError("Network saved, but setting autostart failed"));
    const bool started = !editing && settings.value("start", true).toBool();
    if (started && virNetworkCreate(defined.get()) < 0)
        return request.fail(Virt::lastError("Network saved, but starting it failed"));
    QVariantMap result{{"uuid", identity}, {"subnet", settings["subnet"]}, {"authorized", false},
            {"revision", DomainConfig::revision(Virt::networkXml(defined.get()))}};
    QString message = editing ? "Network saved." : "Network created.";
    if (started && settings.value("authorize", false).toBool()) {
        QString why, code;
        if (authorizeBridges({identity}, why, code)) {
            result["authorized"] = true;
            message += " Your VMs can join it.";
        } else {
            message += " Your VMs can't join it yet: " + why;
            result.insert(helperFailure(code));
        }
    }
    request.done(true, message, result);
}

// "networks.autostart", "networks.start", "networks.stop", "networks.remove" and "networks.authorize" on an
// existing network.
void changeHostNetwork(const VmWorker::Request &request, const ExistingNetwork &existing) {
    const auto &op = request.op;
    const auto network = existing.handle;
    if (!network) return request.fail("Select an existing network.");
    if (op == "networks.autostart") {
        const bool on = request.in["autostart"].toBool();
        if (virNetworkSetAutostart(network, on ? 1 : 0) < 0)
            return request.fail(Virt::lastError("Set network autostart"));
        return request.done(true,
                on ? "The network now starts with the computer." : "The network no longer starts with the computer.",
                {{"revision", DomainConfig::revision(Virt::networkXml(network))}});
    }
    if (op == "networks.authorize") {
        if (virNetworkIsActive(network) != 1)
            return request.fail("Start the network before allowing VMs to join it.", helperFailure("bridge_inactive"));
        if (!request.stillAuthorized()) return;
        QString failure, code;
        if (!authorizeBridges({existing.uuid}, failure, code)) return request.fail(failure, helperFailure(code));
        return request.done(
                true, "Your VMs can now join this network.", {{"revision", DomainConfig::revision(existing.xml)}});
    }
    int result = -1;
    if (op == "networks.start") {
        result = virNetworkCreate(network);
    } else if (op == "networks.stop" || op == "networks.remove") {
        if (!existing.users.isEmpty())
            return request.fail(
                    "This network is still attached to VMs: " + QVariant(existing.users).toStringList().join(", "));
        if (op == "networks.stop")
            result = virNetworkDestroy(network);
        else if (virNetworkIsActive(network) == 1)
            return request.fail("Stop this network before removing it.");
        else
            result = virNetworkUndefine(network);
    } else {
        return request.fail("Unknown host network operation.");
    }
    const bool ok = result == 0;
    request.done(ok, ok ? "Host network operation completed." : Virt::lastError("Update host network"),
            {{"revision",
                    ok && op != "networks.remove" ? DomainConfig::revision(Virt::networkXml(network)) : QString{}}});
}
}

void VmWorker::manageHostNetworks(const Request &request) {
    const auto &op = request.op;
    Connection system(
            op == "networks.list" ? virConnectOpenReadOnly("qemu:///system") : virConnectOpen("qemu:///system"),
            virConnectClose);
    if (!system)
        return request.fail(Virt::lastError("Open host networks; administrator authorization may be required"));
    if (op == "networks.list") return listHostNetworks(request, system.get(), conn_);
    if (op == "networks.authorizeMany") return authorizeHostNetworks(request, system.get());

    ExistingNetwork existing;
    existing.uuid = request.in["uuid"].toString();
    Network network(existing.uuid.isEmpty()
                            ? nullptr
                            : virNetworkLookupByUUIDString(system.get(), existing.uuid.toUtf8().constData()),
            virNetworkFree);
    existing.handle = network.get();
    existing.xml = network ? Virt::networkXml(network.get()) : QString{};
    if (!existing.uuid.isEmpty() && (!network || !managedNetwork(existing.xml)))
        return request.fail(
                "Only OmaWare-created host networks can be changed here.", helperFailure("network_not_owned"));
    QDomDocument doc;
    doc.setContent(existing.xml);
    existing.bridge = doc.documentElement().firstChildElement("bridge").attribute("name");
    existing.name = doc.documentElement().firstChildElement("name").text();
    if (network)
        existing.users =
                usersOf(conn_, existing.bridge, existing.name) + usersOf(system.get(), existing.bridge, existing.name);
    if (network && request.in["revision"].toString() != DomainConfig::revision(existing.xml))
        return request.fail("The network configuration changed. Refresh Networks before retrying.");
    if (op == "networks.save") return saveHostNetwork(request, system.get(), existing);
    changeHostNetwork(request, existing);
}

namespace Management {
QHash<QString, QVariantMap> bridgeNetworks(virConnectPtr system) {
    QHash<QString, QVariantMap> result;
    if (!system) return result;
    virNetworkPtr *networks = nullptr;
    const int count = virConnectListAllNetworks(system, &networks, 0);
    for (int i = 0; i < count; ++i) {
        const auto xml = Virt::networkXml(networks[i]);
        QDomDocument doc;
        doc.setContent(xml);
        const auto root = doc.documentElement();
        const auto bridge = root.firstChildElement("bridge").attribute("name");
        if (!bridge.isEmpty())
            result[bridge] = {{"uuid", root.firstChildElement("uuid").text()},
                    {"name", root.firstChildElement("name").text()}, {"active", virNetworkIsActive(networks[i]) == 1},
                    {"managed", managedNetwork(xml)}};
        virNetworkFree(networks[i]);
    }
    free(networks);
    return result;
}

QVariantMap networksToStart(const QString &domainXml, const QHash<QString, QVariantMap> &networks) {
    QVariantList start;
    QStringList foreign, missing;
    QDomDocument doc;
    doc.setContent(domainXml);
    const auto devices = doc.documentElement().firstChildElement("devices");
    QSet<QString> seen;
    for (auto e = devices.firstChildElement("interface"); !e.isNull(); e = e.nextSiblingElement("interface")) {
        if (e.attribute("type") != "bridge") continue;
        const auto bridge = e.firstChildElement("source").attribute("bridge");
        if (bridge.isEmpty() || seen.contains(bridge)) continue;
        seen.insert(bridge);
        const auto network = networks.value(bridge);
        if (network.isEmpty())
            missing << bridge;
        else if (network["active"].toBool())
            continue;
        else if (!network["managed"].toBool())
            foreign << network["name"].toString();
        else
            start.append(QVariantMap{{"uuid", network["uuid"]}, {"name", network["name"]}, {"bridge", bridge}});
    }
    return {{"start", start}, {"foreign", foreign}, {"missing", missing}};
}

QString startNetworksFor(virDomainPtr domain) {
    const auto xml = Virt::domainXml(domain, VIR_DOMAIN_XML_INACTIVE);
    if (xml.isEmpty()) return "The VM definition could not be read.";
    if (!xml.contains("type='bridge'") && !xml.contains("type=\"bridge\"")) return {};
    Connection readOnly(virConnectOpenReadOnly("qemu:///system"), virConnectClose);
    // Without a look at the host networks, libvirt's own start report is all there is.
    if (!readOnly) return {};
    const auto plan = networksToStart(xml, bridgeNetworks(readOnly.get()));
    readOnly.reset();
    QStringList missing;
    for (const auto &bridge : plan["missing"].toStringList())
        if (!QNetworkInterface::interfaceFromName(bridge).isValid()) missing << bridge;
    if (!missing.isEmpty())
        return "A network adapter is plugged into " + missing.join(", ") +
               ", which doesn't exist on this computer. Connect it to a network in Hardware, or start the "
               "network that owns that bridge.";
    if (const auto foreign = plan["foreign"].toStringList(); !foreign.isEmpty())
        return "Network " + foreign.join(", ") +
               " is stopped. Start it first; OmaWare only starts networks it created.";
    const auto start = plan["start"].toList();
    if (start.isEmpty()) return {};
    QStringList names;
    for (const auto &v : start)
        names << v.toMap()["name"].toString();
    Connection system(virConnectOpen("qemu:///system"), virConnectClose);
    if (!system)
        return Virt::lastError("Start network " + names.join(", ") + "; administrator authorization may be required");
    for (const auto &v : start) {
        const auto entry = v.toMap();
        Network network(
                virNetworkLookupByUUIDString(system.get(), entry["uuid"].toString().toUtf8().constData()),
                virNetworkFree);
        if (!network) return Virt::lastError("Start network " + entry["name"].toString());
        if (virNetworkIsActive(network.get()) != 1 && virNetworkCreate(network.get()) < 0)
            return Virt::lastError("Start network " + entry["name"].toString());
    }
    return {};
}
}
